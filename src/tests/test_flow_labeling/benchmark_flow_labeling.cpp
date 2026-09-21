// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <framework/interfaces/iflow_labeling.h>
#include <common/loader.hpp>

#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr const char* kSchemaId = "flowsql.io/flow-labeling/v1alpha1";
constexpr uint64_t kSnapshotLimitBytes = 8ULL * 1024ULL * 1024ULL;
constexpr uint64_t kMebibyte = 1024ULL * 1024ULL;
constexpr uint32_t kBatchSize = 256;

struct BenchmarkCase {
    uint32_t logical_rules;
    uint32_t reserved_mib;
    uint32_t runtime_mib;
    bool expect_success;
};

void Require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

std::string BuildConfig(const BenchmarkCase& benchmark) {
    std::string yaml = R"(api_version: flowsql.io/flow-labeling/v1alpha1
kind: FlowLabelingSet
spec:
  engine:
    type: dpdk-acl
    categories: 1
    algorithm: scalar
    numa_socket_id: any
    max_runtime_bytes: )";
    yaml += std::to_string(static_cast<uint64_t>(benchmark.runtime_mib) * kMebibyte);
    yaml += R"(
    limits:
      max_labels: 10000
      max_logical_rules: 50000
      max_compiled_rules: 100000
      max_expanded_fields: 64
      reject_duplicate_label_priorities: true
  labels:
    - {id: 1, priority: 1, name: benchmark}
  rules:
)";
    yaml.reserve(yaml.size() + static_cast<size_t>(benchmark.logical_rules) * 120U);
    for (uint32_t index = 0; index < benchmark.logical_rules; ++index) {
        yaml += "    - {id: r";
        yaml += std::to_string(index + 1U);
        yaml += ", label_id: 1, direction: bidirectional, matches: [{field: observation_domain, min: ";
        yaml += std::to_string(index + 1U);
        yaml += ", max: ";
        yaml += std::to_string(index + 1U);
        yaml += "}, {field: source_port, min: 443, max: 443}]}\n";
    }
    return yaml;
}

flowsql::ConfigChannelSnapshot MakeSnapshot(std::string content) {
    flowsql::ConfigChannelSnapshot snapshot;
    snapshot.channel_name = "flow-labeling-benchmark";
    snapshot.revision = 1;
    snapshot.format = "yaml";
    snapshot.schema_id = kSchemaId;
    snapshot.sha256_hex = "benchmark";
    snapshot.content = std::make_shared<const std::string>(std::move(content));
    snapshot.content_bytes = snapshot.content->size();
    return snapshot;
}

flowsql::FlowLabelingCompileRequestV1 MakeRequest(const flowsql::ConfigChannelSnapshot* snapshot,
                                                  uint64_t reserved_bytes) {
    flowsql::FlowLabelingCompileRequestV1 request;
    request.snapshot = snapshot;
    request.reserved_module_state_bytes = reserved_bytes;
    request.max_labels = 10000;
    request.max_logical_rules = 50000;
    request.max_compiled_rules = 100000;
    return request;
}

uint64_t ReadStatusKiB(std::string_view key) {
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.compare(0, key.size(), key) != 0) continue;
        std::istringstream fields(line.substr(key.size()));
        uint64_t kib = 0;
        fields >> kib;
        return fields ? kib : 0;
    }
    return 0;
}

uint64_t PeakRssKiB() {
    rusage usage{};
    return getrusage(RUSAGE_SELF, &usage) == 0 ? static_cast<uint64_t>(usage.ru_maxrss) : 0;
}

std::vector<flowsql::FlowLabelFactsV1> MakeFacts(uint32_t logical_rules) {
    std::vector<flowsql::FlowLabelFactsV1> facts(kBatchSize);
    for (uint32_t index = 0; index < kBatchSize; ++index) {
        facts[index].observation_domain_id = (index % logical_rules) + 1U;
        facts[index].source.port = 443;
        facts[index].source.port_valid = 1;
    }
    return facts;
}

int RunCase(const char* plugin_path, const BenchmarkCase& benchmark) {
    flowsql::PluginLoader* loader = flowsql::PluginLoader::Single();
    flowsql::IFlowLabelMatcherV1* matcher = nullptr;
    bool loaded = false;
    try {
        const uint64_t reserved_bytes = static_cast<uint64_t>(benchmark.reserved_mib) * kMebibyte;
        const uint64_t runtime_bytes = static_cast<uint64_t>(benchmark.runtime_mib) * kMebibyte;
        const uint32_t physical_rules = benchmark.logical_rules * 2U;
        const uint64_t rss_before_config_kib = ReadStatusKiB("VmRSS:");
        auto snapshot = MakeSnapshot(BuildConfig(benchmark));
        const uint64_t config_bytes = snapshot.content_bytes;
        Require(config_bytes <= kSnapshotLimitBytes, "generated configuration exceeds the 8 MiB snapshot limit");
        const uint64_t rss_before_build_kib = ReadStatusKiB("VmRSS:");

        const char* plugins[] = {plugin_path};
        Require(loader->Load(plugins, 1) == 0, "Flow Labeling plugin loading failed");
        loaded = true;
        Require(loader->StartAll() == 0, "Flow Labeling plugin start failed");
        auto* provider =
            static_cast<flowsql::IFlowLabelingProviderV1*>(loader->First(flowsql::IID_FLOW_LABELING_PROVIDER_V1));
        Require(provider != nullptr, "Flow Labeling provider interface is unavailable");

        flowsql::FlowLabelingDiagnosticV1 diagnostic;
        const auto request = MakeRequest(&snapshot, reserved_bytes);
        const auto build_start = std::chrono::steady_clock::now();
        const auto build_status = provider->CreateMatcher(request, &matcher, &diagnostic);
        const double build_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - build_start).count();
        const uint64_t rss_after_build_kib = ReadStatusKiB("VmRSS:");
        if (!benchmark.expect_success) {
            Require(build_status == flowsql::FlowLabelingErrorV1::kBudgetExceeded,
                    "expected matcher budget rejection did not occur");
            Require(matcher == nullptr, "failed matcher build published a lease");
            Require(diagnostic.path != nullptr && std::string_view(diagnostic.path) == "/spec/engine/max_runtime_bytes",
                    "budget rejection returned an unexpected diagnostic path");
        } else if (build_status != flowsql::FlowLabelingErrorV1::kNone) {
            throw std::runtime_error("matcher build failed with error " +
                                     std::to_string(static_cast<int>(build_status)) + " at " +
                                     (diagnostic.path ? diagnostic.path : "<none>") + ": " +
                                     (diagnostic.detail ? diagnostic.detail : "<none>"));
        }
        const std::string diagnostic_path = diagnostic.path == nullptr ? "" : diagnostic.path;
        uint64_t classifications = 0;
        double classify_ms = 0.0;
        double mpps = 0.0;
        if (benchmark.expect_success) {
            Require(matcher != nullptr, "matcher build returned a null lease");
            const auto facts = MakeFacts(benchmark.logical_rules);
            std::vector<uint32_t> labels(kBatchSize, 0);
            Require(matcher->ClassifyBatch(facts.data(), facts.size(), labels.data()) == 0,
                    "classification warm-up failed");
            for (uint32_t label : labels) Require(label == 1, "forward match result is incorrect");

            flowsql::FlowLabelFactsV1 reverse;
            reverse.observation_domain_id = 1;
            reverse.destination.port = 443;
            reverse.destination.port_valid = 1;
            uint32_t reverse_label = 0;
            Require(matcher->ClassifyBatch(&reverse, 1, &reverse_label) == 0 && reverse_label == 1,
                    "reverse match result is incorrect");

            flowsql::FlowLabelFactsV1 unmatched;
            unmatched.observation_domain_id = static_cast<uint64_t>(benchmark.logical_rules) + 1U;
            unmatched.source.port = 443;
            unmatched.source.port_valid = 1;
            uint32_t unmatched_label = 99;
            Require(matcher->ClassifyBatch(&unmatched, 1, &unmatched_label) == 0 && unmatched_label == 0,
                    "unmatched classification result is incorrect");

            constexpr uint32_t kWarmupIterations = 16;
            for (uint32_t iteration = 0; iteration < kWarmupIterations; ++iteration) {
                Require(matcher->ClassifyBatch(facts.data(), facts.size(), labels.data()) == 0,
                        "classification warm-up failed");
            }
            uint64_t iterations = 0;
            const auto classify_start = std::chrono::steady_clock::now();
            do {
                for (uint32_t group = 0; group < 64; ++group) {
                    Require(matcher->ClassifyBatch(facts.data(), facts.size(), labels.data()) == 0,
                            "timed classification failed");
                }
                iterations += 64;
                classify_ms =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - classify_start)
                        .count();
            } while (classify_ms < 250.0);
            for (uint32_t label : labels) Require(label == 1, "timed classification result is incorrect");
            classifications = iterations * kBatchSize;
            mpps = static_cast<double>(classifications) / (classify_ms * 1000.0);
        }

        if (matcher != nullptr) {
            matcher->Release();
            matcher = nullptr;
        }
        snapshot.content.reset();
        const uint64_t rss_after_release_kib = ReadStatusKiB("VmRSS:");
        const uint64_t peak_rss_kib = PeakRssKiB();
        loader->StopAll();
        loader->Unload();
        loaded = false;

        const int64_t rss_build_delta_kib =
            static_cast<int64_t>(rss_after_build_kib) - static_cast<int64_t>(rss_before_build_kib);
        std::cout << "exact_observation_domain_asymmetric_port," << benchmark.logical_rules << ',' << physical_rules
                  << ',' << config_bytes << ',' << reserved_bytes << ',' << runtime_bytes << ','
                  << (build_status == flowsql::FlowLabelingErrorV1::kNone ? "success" : "budget_exceeded") << ','
                  << static_cast<int>(build_status) << ',' << diagnostic_path << ',' << std::fixed
                  << std::setprecision(3) << build_ms << ',' << rss_before_config_kib << ',' << rss_before_build_kib
                  << ',' << rss_after_build_kib << ',' << rss_build_delta_kib << ',' << rss_after_release_kib << ','
                  << peak_rss_kib << ',' << classifications << ',' << classify_ms << ',' << mpps << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "benchmark_flow_labeling " << benchmark.logical_rules << " rules at " << benchmark.reserved_mib
                  << " MiB failed: " << error.what() << '\n';
        if (matcher != nullptr) matcher->Release();
        if (loaded) {
            loader->StopAll();
            loader->Unload();
        }
        return 1;
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2 && argc != 6) {
        std::cerr << "usage: benchmark_flow_labeling <flow-labeling-plugin-path> "
                     "[logical-rules reserved-mib runtime-mib success|budget_exceeded]\n";
        return 2;
    }

    const std::string plugin_path = std::filesystem::absolute(argv[1]).string();
    if (!std::filesystem::is_regular_file(plugin_path)) {
        std::cerr << "Flow Labeling plugin does not exist: " << plugin_path << '\n';
        return 2;
    }

    std::cout << "profile,logical_rules,physical_rules,config_bytes,reserved_bytes,max_runtime_bytes,result,error,path,"
                 "build_ms,"
                 "rss_before_config_kib,rss_before_build_kib,rss_after_build_kib,rss_build_delta_kib,"
                 "rss_after_release_kib,peak_rss_kib,classifications,classify_ms,mpps\n"
              << std::flush;
    if (argc == 6) {
        try {
            const unsigned long logical_rules = std::stoul(argv[2]);
            const unsigned long reserved_mib = std::stoul(argv[3]);
            const unsigned long runtime_mib = std::stoul(argv[4]);
            Require(logical_rules <= UINT32_MAX && reserved_mib <= UINT32_MAX && runtime_mib <= UINT32_MAX,
                    "single-case argument is out of range");
            const std::string_view expectation(argv[5]);
            Require(expectation == "success" || expectation == "budget_exceeded", "single-case expectation is invalid");
            return RunCase(plugin_path.c_str(),
                           {static_cast<uint32_t>(logical_rules), static_cast<uint32_t>(reserved_mib),
                            static_cast<uint32_t>(runtime_mib), expectation == "success"});
        } catch (const std::exception& error) {
            std::cerr << "invalid single-case benchmark arguments: " << error.what() << '\n';
            return 2;
        }
    }
    bool succeeded = true;
    constexpr std::array<BenchmarkCase, 4> kCases = {
        BenchmarkCase{1000, 64, 40, true},
        BenchmarkCase{10000, 64, 40, true},
        BenchmarkCase{50000, 64, 40, false},
        BenchmarkCase{50000, 128, 40, true},
    };
    for (const BenchmarkCase& benchmark : kCases) {
        const pid_t child = fork();
        if (child < 0) {
            std::cerr << "fork failed: " << std::strerror(errno) << '\n';
            return 1;
        }
        if (child == 0) {
            const int status = RunCase(plugin_path.c_str(), benchmark);
            std::cout.flush();
            std::cerr.flush();
            _exit(status);
        }
        int status = 0;
        if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            std::cerr << "benchmark child failed for " << benchmark.logical_rules << " logical rules at "
                      << benchmark.reserved_mib << " MiB\n";
            succeeded = false;
        }
    }
    return succeeded ? 0 : 1;
}
