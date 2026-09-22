// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <framework/interfaces/iflow_labeling.h>
#include <plugins/flow_labeling/flow_labeling_plugin.h>
#include <common/loader.hpp>

#include <arpa/inet.h>
#include <sched.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr const char* kSchemaId = "flowsql.io/flow-labeling/v1alpha1";

std::string ReadTemplate() {
    const std::filesystem::path repository =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path();
    std::ifstream input(repository / "config/flow-labeling-template.yaml", std::ios::binary);
    assert(input.is_open());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

flowsql::ConfigChannelSnapshot MakeSnapshot(const std::string& schema_id, const std::string& content) {
    flowsql::ConfigChannelSnapshot snapshot;
    snapshot.channel_name = "flow-labeling-rules";
    snapshot.revision = 7;
    snapshot.format = "yaml";
    snapshot.schema_id = schema_id;
    snapshot.sha256_hex = "test";
    snapshot.content = std::make_shared<const std::string>(content);
    snapshot.content_bytes = snapshot.content->size();
    return snapshot;
}

flowsql::FlowLabelingCompileRequestV1 MakeRequest(const flowsql::ConfigChannelSnapshot* snapshot) {
    flowsql::FlowLabelingCompileRequestV1 request;
    request.snapshot = snapshot;
    request.reserved_module_state_bytes = 64ULL * 1024ULL * 1024ULL;
    request.max_labels = 10000;
    request.max_logical_rules = 50000;
    request.max_compiled_rules = 100000;
    return request;
}

uint32_t FirstAvailableCpu() {
    cpu_set_t affinity;
    CPU_ZERO(&affinity);
    assert(sched_getaffinity(0, sizeof(affinity), &affinity) == 0);
    for (uint32_t cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET(cpu, &affinity)) return cpu;
    }
    assert(false && "the process affinity set must contain at least one CPU");
    return 0;
}

uint32_t FirstUnavailableCpu() {
    cpu_set_t affinity;
    CPU_ZERO(&affinity);
    assert(sched_getaffinity(0, sizeof(affinity), &affinity) == 0);
    for (uint32_t cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (!CPU_ISSET(cpu, &affinity)) return cpu;
    }
    return CPU_SETSIZE;
}

void TestStartupOptions() {
    const uint32_t available_cpu = FirstAvailableCpu();
    flowsql::FlowLabelingStartupOptions options;
    options.eal_memory_mib = 2048;
    options.eal_lcore_cpu = available_cpu;

    assert(flowsql::ParseFlowLabelingStartupOptions(nullptr, &options) == 0);
    assert(options.eal_memory_mib == 512 && !options.eal_lcore_cpu.has_value());
    options.eal_memory_mib = 2048;
    options.eal_lcore_cpu = available_cpu;
    assert(flowsql::ParseFlowLabelingStartupOptions("", &options) == 0);
    assert(options.eal_memory_mib == 512 && !options.eal_lcore_cpu.has_value());

    assert(flowsql::ParseFlowLabelingStartupOptions("eal_memory_mib=4096", &options) == 0);
    assert(options.eal_memory_mib == 4096 && !options.eal_lcore_cpu.has_value());
    const std::string cpu_only = "eal_lcore_cpu=" + std::to_string(available_cpu);
    assert(flowsql::ParseFlowLabelingStartupOptions(cpu_only.c_str(), &options) == 0);
    assert(options.eal_memory_mib == 512 && options.eal_lcore_cpu == available_cpu);
    const std::string both = "eal_lcore_cpu=" + std::to_string(available_cpu) + ";eal_memory_mib=512";
    assert(flowsql::ParseFlowLabelingStartupOptions(both.c_str(), &options) == 0);
    assert(options.eal_memory_mib == 512 && options.eal_lcore_cpu == available_cpu);

    const flowsql::FlowLabelingStartupOptions previous = options;
    const std::vector<std::string> invalid = {
        ";",
        "eal_memory_mib=512;",
        ";eal_memory_mib=512",
        "eal_memory_mib=512;;eal_lcore_cpu=" + std::to_string(available_cpu),
        "eal_memory_mib",
        "eal_memory_mib=",
        "=512",
        "eal_memory_mib=512=0",
        "unknown=512",
        "eal_memory_mib=512;eal_memory_mib=1024",
        "eal_lcore_cpu=" + std::to_string(available_cpu) + ";eal_lcore_cpu=" + std::to_string(available_cpu),
        "eal_memory_mib=511",
        "eal_memory_mib=4097",
        "eal_memory_mib=+512",
        "eal_memory_mib=-512",
        "eal_memory_mib=512x",
        "eal_memory_mib=4294967296",
        "eal_lcore_cpu=+" + std::to_string(available_cpu),
        "eal_lcore_cpu=-1",
        "eal_lcore_cpu=1x",
        "eal_lcore_cpu=4294967296",
        "eal_lcore_cpu=" + std::to_string(FirstUnavailableCpu()),
    };
    for (const auto& option : invalid) {
        assert(flowsql::ParseFlowLabelingStartupOptions(option.c_str(), &options) != 0);
        assert(options.eal_memory_mib == previous.eal_memory_mib);
        assert(options.eal_lcore_cpu == previous.eal_lcore_cpu);
    }
    assert(flowsql::ParseFlowLabelingStartupOptions(nullptr, nullptr) != 0);
}

void TestEalArguments() {
    const uint32_t available_cpu = FirstAvailableCpu();
    flowsql::FlowLabelingStartupOptions options;
    const std::vector<std::string> expected_default = {"flowsql-flow-labeling",
                                                       "--lcores=0@" + std::to_string(available_cpu),
                                                       "--main-lcore=0",
                                                       "-m",
                                                       "512",
                                                       "--no-huge",
                                                       "--no-pci",
                                                       "--no-telemetry",
                                                       "--no-shconf"};
    assert(flowsql::BuildFlowLabelingEalArguments(options, available_cpu) == expected_default);

    options.eal_memory_mib = 768;
    options.eal_lcore_cpu = available_cpu;
    auto expected_override = expected_default;
    expected_override[4] = "768";
    assert(flowsql::BuildFlowLabelingEalArguments(options, available_cpu + 1) == expected_override);
}

void TestIsolatedEalStartup(const char* library) {
    // DPDK EAL is process-global; separate children prove the default and an override can each start cleanly.
    const std::vector<std::string> startup_options = {
        "", "eal_memory_mib=768;eal_lcore_cpu=" + std::to_string(FirstAvailableCpu())};
    for (const auto& option : startup_options) {
        const pid_t child = fork();
        assert(child >= 0);
        if (child == 0) {
            auto* loader = flowsql::PluginLoader::Single();
            const char* plugins[] = {library};
            const char* options[] = {option.empty() ? nullptr : option.c_str()};
            if (loader->Load(flowsql::get_absolute_process_path(), plugins, options, 1) != 0) _exit(1);
            auto* provider =
                static_cast<flowsql::IFlowLabelingProviderV1*>(loader->First(flowsql::IID_FLOW_LABELING_PROVIDER_V1));
            if (provider == nullptr || loader->StartAll() != 0) _exit(2);
            flowsql::FlowLabelingDiagnosticV1 diagnostic;
            if (provider->RuntimeStatus(&diagnostic) != flowsql::FlowLabelingErrorV1::kNone) _exit(3);
            loader->StopAll();
            if (loader->Unload() != 0) _exit(4);
            _exit(0);
        }
        int status = 0;
        assert(waitpid(child, &status, 0) == child);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }

    // Two independent plugin instances share one process EAL. The second initialization must fail and roll back both.
    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        const std::filesystem::path copy = std::string(library) + ".second." + std::to_string(getpid()) + ".so";
        std::error_code error;
        if (!std::filesystem::copy_file(library, copy, std::filesystem::copy_options::overwrite_existing, error)) {
            _exit(5);
        }
        auto* loader = flowsql::PluginLoader::Single();
        const std::string copy_path = copy.string();
        const char* plugins[] = {library, copy_path.c_str()};
        const char* options[] = {"eal_memory_mib=512", "eal_memory_mib=512"};
        if (loader->Load(flowsql::get_absolute_process_path(), plugins, options, 2) != 0) _exit(6);
        if (loader->StartAll() == 0) _exit(7);
        auto* provider =
            static_cast<flowsql::IFlowLabelingProviderV1*>(loader->First(flowsql::IID_FLOW_LABELING_PROVIDER_V1));
        flowsql::FlowLabelingDiagnosticV1 diagnostic;
        if (provider == nullptr || provider->RuntimeStatus(&diagnostic) != flowsql::FlowLabelingErrorV1::kUnavailable) {
            _exit(8);
        }
        loader->StopAll();
        if (loader->Unload() != 0) _exit(9);
        std::filesystem::remove(copy, error);
        if (error) _exit(10);
        _exit(0);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

std::string EmptyConfig(const std::string& algorithm = "scalar", uint64_t runtime_bytes = 4 * 1024 * 1024) {
    return "api_version: flowsql.io/flow-labeling/v1alpha1\n"
           "kind: FlowLabelingSet\n"
           "spec:\n"
           "  engine:\n"
           "    type: dpdk-acl\n"
           "    categories: 1\n"
           "    algorithm: " +
           algorithm +
           "\n"
           "    numa_socket_id: any\n"
           "    max_runtime_bytes: " +
           std::to_string(runtime_bytes) +
           "\n"
           "    limits:\n"
           "      max_labels: 10000\n"
           "      max_logical_rules: 50000\n"
           "      max_compiled_rules: 100000\n"
           "      max_expanded_fields: 64\n"
           "      reject_duplicate_label_priorities: true\n"
           "  labels: []\n"
           "  rules: []\n";
}

std::string FullConfig() {
    return R"(api_version: flowsql.io/flow-labeling/v1alpha1
kind: FlowLabelingSet
metadata:
  name: corp-primary-labels
  display_name: Corporate primary labels
  description: Strict DPDK ACL integration test.
spec:
  engine:
    type: dpdk-acl
    categories: 1
    algorithm: scalar
    numa_socket_id: any
    max_runtime_bytes: 8388608
    limits:
      max_labels: 10000
      max_logical_rules: 50000
      max_compiled_rules: 100000
      max_expanded_fields: 64
      reject_duplicate_label_priorities: true
  labels:
    - id: 1001
      priority: 3000
      name: corp-web
      display_name: Corporate Web
      description: Corporate HTTPS traffic.
    - id: 1002
      priority: 2000
      name: broad-web
    - id: 1003
      priority: 1500
      name: gateway-management
    - id: 1004
      priority: 1000
      name: ipv6-dns
    - id: 1005
      priority: 2500
      name: native-type-matrix
  rules:
    - id: corp-web-client-to-server
      label_id: 1001
      direction: bidirectional
      matches:
        - field: observation_domain
          min: 7
          max: 7
        - field: outer_vlan_tpid
          value: 33024
          mask: 65535
        - field: outer_vlan_vid
          min: 20
          max: 22
        - field: destination_ipv4_prefix
          value: 10.30.0.0
          prefix_bits: 16
        - field: transport_protocol
          value: 6
          mask: 255
        - field: destination_port
          min: 443
          max: 443
    - id: broad-web-range
      label_id: 1002
      direction: bidirectional
      matches:
        - field: destination_ipv4_range
          min: 10.30.0.0
          max: 10.30.255.255
        - field: transport_protocol
          value: 6
          mask: 255
        - field: destination_port
          min: 443
          max: 443
    - id: gateway-source-mac
      label_id: 1003
      direction: bidirectional
      matches:
        - field: source_mac_prefix
          value: "00:11:22:33:44:55"
          prefix_bits: 48
    - id: ipv6-inner-vlan-dns
      label_id: 1004
      direction: bidirectional
      matches:
        - field: destination_ipv6_prefix
          value: "2001:db8:1::"
          prefix_bits: 48
        - field: inner_vlan_tpid
          value: 34984
          mask: 65535
        - field: inner_vlan_vid
          min: 100
          max: 100
        - field: transport_protocol
          value: 17
          mask: 255
        - field: destination_port
          min: 53
          max: 53
    - id: native-operation-size-matrix
      label_id: 1005
      direction: bidirectional
      matches:
        - field: observation_domain
          min: 99
          max: 99
        - field: observation_domain_prefix
          value: 99
          prefix_bits: 64
        - field: observation_domain_bitmask
          value: 99
          mask: 18446744073709551615
        - field: source_ipv4_prefix
          value: 192.0.2.1
          prefix_bits: 32
        - field: source_ipv4_range
          min: 192.0.2.1
          max: 192.0.2.1
        - field: source_ipv4_bitmask
          value: 192.0.2.1
          mask: 255.255.255.255
        - field: outer_vlan_tpid
          value: 33024
          mask: 65535
        - field: outer_vlan_tpid_prefix
          value: 33024
          prefix_bits: 16
        - field: outer_vlan_vid
          min: 321
          max: 321
        - field: transport_protocol
          value: 17
          mask: 255
        - field: transport_protocol_prefix
          value: 17
          prefix_bits: 8
        - field: transport_protocol_range
          min: 17
          max: 17
        - field: source_port
          min: 5353
          max: 5353
)";
}

std::string SingleRuleConfig(bool match_all) {
    std::string yaml = EmptyConfig();
    const std::string compiled_limit = "      max_compiled_rules: 100000";
    const size_t limit = yaml.find(compiled_limit);
    assert(limit != std::string::npos);
    yaml.replace(limit, compiled_limit.size(), "      max_compiled_rules: 1");
    const size_t labels = yaml.find("  labels: []");
    assert(labels != std::string::npos);
    yaml.replace(labels, std::strlen("  labels: []"), R"(  labels:
    - id: 1
      priority: 1
      name: one)");
    const size_t rules = yaml.find("  rules: []");
    assert(rules != std::string::npos);
    const std::string body = match_all ? "      match_all: true\n"
                                       : "      matches:\n"
                                         "        - field: destination_port\n"
                                         "          min: 443\n"
                                         "          max: 443\n";
    yaml.replace(rules, std::strlen("  rules: []"),
                 "  rules:\n"
                 "    - id: one-rule\n"
                 "      label_id: 1\n"
                 "      direction: bidirectional\n" +
                     body);
    return yaml;
}

std::string CompressionEquivalenceConfig(bool conflicting_masks = false) {
    return R"(api_version: flowsql.io/flow-labeling/v1alpha1
kind: FlowLabelingSet
spec:
  engine:
    type: dpdk-acl
    categories: 1
    algorithm: scalar
    numa_socket_id: any
    max_runtime_bytes: 4194304
    limits:
      max_labels: 10000
      max_logical_rules: 50000
      max_compiled_rules: 100000
      max_expanded_fields: 32
      reject_duplicate_label_priorities: true
  labels:
    - {id: 2001, priority: 1, name: compressed-intersection}
  rules:
    - id: range-prefix-bitmask-intersection
      label_id: 2001
      direction: bidirectional
      matches:
        - {field: observation_domain, min: 293, max: 309}
        - {field: observation_domain_prefix, value: 288, prefix_bits: 60}
        - {field: observation_domain_bitmask, value: )" +
           std::string(conflicting_masks ? "304, mask: 18446744073709551600" : "1, mask: 3") + R"(}
        - {field: source_ipv4_prefix, value: 192.0.2.0, prefix_bits: 24}
        - {field: source_port, min: 1000, max: 2000}
)";
}

std::string PresenceBitmapConfig() {
    return R"(api_version: flowsql.io/flow-labeling/v1alpha1
kind: FlowLabelingSet
spec:
  engine:
    type: dpdk-acl
    categories: 1
    algorithm: scalar
    numa_socket_id: any
    max_runtime_bytes: 4194304
    limits:
      max_labels: 10000
      max_logical_rules: 50000
      max_compiled_rules: 100000
      max_expanded_fields: 32
      reject_duplicate_label_priorities: true
  labels:
    - {id: 3001, priority: 1, name: all-presence-bits}
  rules:
    - id: all-presence-bits
      label_id: 3001
      direction: bidirectional
      matches:
        - {field: source_mac_prefix, value: "00:00:00:00:00:00", prefix_bits: 0}
        - {field: destination_mac_prefix, value: "00:00:00:00:00:00", prefix_bits: 0}
        - {field: source_ipv4_prefix, value: 0.0.0.0, prefix_bits: 0}
        - {field: destination_ipv4_prefix, value: 0.0.0.0, prefix_bits: 0}
        - {field: transport_protocol, value: 0, mask: 0}
        - {field: source_port, min: 0, max: 65535}
        - {field: destination_port, min: 0, max: 65535}
        - {field: outer_vlan_tpid, value: 0, mask: 0}
        - {field: outer_vlan_vid, min: 0, max: 4095}
        - {field: inner_vlan_tpid, value: 0, mask: 0}
        - {field: inner_vlan_vid, min: 0, max: 4095}
)";
}

std::string SamePredicateDifferentLabelsConfig() {
    std::string yaml = EmptyConfig();
    const std::string compiled_limit = "      max_compiled_rules: 100000";
    const size_t limit = yaml.find(compiled_limit);
    assert(limit != std::string::npos);
    yaml.replace(limit, compiled_limit.size(), "      max_compiled_rules: 1");
    const size_t labels = yaml.find("  labels: []");
    assert(labels != std::string::npos);
    yaml.replace(labels, std::strlen("  labels: []"), R"(  labels:
    - {id: 1, priority: 2, name: first}
    - {id: 2, priority: 1, name: second})");
    const size_t rules = yaml.find("  rules: []");
    assert(rules != std::string::npos);
    yaml.replace(rules, std::strlen("  rules: []"), R"(  rules:
    - {id: first-rule, label_id: 1, direction: bidirectional, match_all: true}
    - {id: second-rule, label_id: 2, direction: bidirectional, match_all: true})");
    return yaml;
}

std::string BudgetConfig(uint32_t rule_count) {
    std::string yaml = R"(api_version: flowsql.io/flow-labeling/v1alpha1
kind: FlowLabelingSet
spec:
  engine:
    type: dpdk-acl
    categories: 1
    algorithm: scalar
    numa_socket_id: any
    max_runtime_bytes: 1048576
    limits:
      max_labels: 10000
      max_logical_rules: 50000
      max_compiled_rules: 100000
      max_expanded_fields: 32
      reject_duplicate_label_priorities: true
  labels:
    - {id: 1, priority: 1, name: budget}
  rules:
)";
    yaml.reserve(yaml.size() + static_cast<size_t>(rule_count) * 120);
    for (uint32_t index = 0; index < rule_count; ++index) {
        yaml += "    - {id: r" + std::to_string(index) +
                ", label_id: 1, direction: bidirectional, matches: [{field: observation_domain, min: " +
                std::to_string(index) + ", max: " + std::to_string(index) + "}]}\n";
    }
    return yaml;
}

void SetIpv4(const char* value, flowsql::FlowLabelEndpointFactsV1* endpoint) {
    assert(inet_pton(AF_INET, value, endpoint->ip) == 1);
    endpoint->ip_valid = 1;
}

void SetIpv6(const char* value, flowsql::FlowLabelEndpointFactsV1* endpoint) {
    assert(inet_pton(AF_INET6, value, endpoint->ip) == 1);
    endpoint->ip_valid = 1;
}

void SetMac(const uint8_t (&value)[6], flowsql::FlowLabelEndpointFactsV1* endpoint) {
    std::memcpy(endpoint->mac, value, sizeof(value));
    endpoint->mac_valid = 1;
}

flowsql::FlowLabelFactsV1 WebFacts() {
    flowsql::FlowLabelFactsV1 facts;
    facts.observation_domain_id = 7;
    facts.ip_family = 4;
    facts.transport_protocol = 6;
    facts.transport_valid = 1;
    SetIpv4("192.168.1.10", &facts.source);
    SetIpv4("10.30.4.5", &facts.destination);
    facts.source.port = 49152;
    facts.source.port_valid = 1;
    facts.destination.port = 443;
    facts.destination.port_valid = 1;
    facts.vlan[0].tpid = 0x8100;
    facts.vlan[0].vid = 21;
    facts.vlan[0].valid = 1;
    return facts;
}

flowsql::FlowLabelFactsV1 Reverse(flowsql::FlowLabelFactsV1 facts) {
    std::swap(facts.source, facts.destination);
    return facts;
}

flowsql::FlowLabelFactsV1 MacFacts() {
    flowsql::FlowLabelFactsV1 facts;
    const uint8_t gateway[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    SetMac(gateway, &facts.source);
    return facts;
}

flowsql::FlowLabelFactsV1 Ipv6Facts() {
    flowsql::FlowLabelFactsV1 facts;
    facts.ip_family = 6;
    facts.transport_protocol = 17;
    facts.transport_valid = 1;
    SetIpv6("2001:db8:ffff::1", &facts.source);
    SetIpv6("2001:db8:1::53", &facts.destination);
    facts.source.port = 40000;
    facts.source.port_valid = 1;
    facts.destination.port = 53;
    facts.destination.port_valid = 1;
    facts.vlan[1].tpid = 0x88a8;
    facts.vlan[1].vid = 100;
    facts.vlan[1].valid = 1;
    return facts;
}

flowsql::FlowLabelFactsV1 NativeMatrixFacts() {
    flowsql::FlowLabelFactsV1 facts;
    facts.observation_domain_id = 99;
    facts.ip_family = 4;
    facts.transport_protocol = 17;
    facts.transport_valid = 1;
    SetIpv4("192.0.2.1", &facts.source);
    SetIpv4("198.51.100.1", &facts.destination);
    facts.source.port = 5353;
    facts.source.port_valid = 1;
    facts.destination.port = 9999;
    facts.destination.port_valid = 1;
    facts.vlan[0].tpid = 0x8100;
    facts.vlan[0].vid = 321;
    facts.vlan[0].valid = 1;
    return facts;
}

void ExpectFailure(flowsql::IFlowLabelingProviderV1* provider, const std::string& yaml,
                   flowsql::FlowLabelingErrorV1 expected, const char* expected_path = nullptr,
                   flowsql::FlowLabelingCompileRequestV1 request = {}) {
    const auto snapshot = MakeSnapshot(kSchemaId, yaml);
    if (request.snapshot == nullptr && request.reserved_module_state_bytes == 0 && request.max_labels == 0 &&
        request.max_logical_rules == 0 && request.max_compiled_rules == 0) {
        request = MakeRequest(&snapshot);
    }
    request.snapshot = &snapshot;
    auto* sentinel = reinterpret_cast<flowsql::IFlowLabelMatcherV1*>(static_cast<uintptr_t>(0x1));
    flowsql::FlowLabelingDiagnosticV1 diagnostic;
    const auto actual = provider->CreateMatcher(request, &sentinel, &diagnostic);
    if (actual != expected) {
        std::cerr << "expected error " << static_cast<int>(expected) << ", got " << static_cast<int>(actual) << " at "
                  << (diagnostic.path ? diagnostic.path : "<none>") << ": "
                  << (diagnostic.detail ? diagnostic.detail : "<none>") << '\n';
    }
    assert(actual == expected);
    assert(diagnostic.error == expected);
    assert(sentinel == reinterpret_cast<flowsql::IFlowLabelMatcherV1*>(static_cast<uintptr_t>(0x1)));
    if (expected_path != nullptr && (diagnostic.path == nullptr || std::string(diagnostic.path) != expected_path)) {
        std::cerr << "expected path " << expected_path << ", got " << (diagnostic.path ? diagnostic.path : "<none>")
                  << '\n';
        assert(false);
    }
}

void TestStrictFailures(flowsql::IFlowLabelingProviderV1* provider) {
    ExpectFailure(provider, R"(api_version: flowsql.io/flow-labeling/v1alpha1
kind: FlowLabelingSet
unknown: true
spec: {}
)",
                  flowsql::FlowLabelingErrorV1::kInvalidConfig, "/unknown");

    ExpectFailure(provider, R"(api_version: flowsql.io/flow-labeling/v1alpha1
api_version: flowsql.io/flow-labeling/v1alpha1
kind: FlowLabelingSet
spec: {}
)",
                  flowsql::FlowLabelingErrorV1::kInvalidConfig, "/api_version");

    std::string duplicate_priority = EmptyConfig();
    const size_t empty_labels = duplicate_priority.find("  labels: []");
    assert(empty_labels != std::string::npos);
    duplicate_priority.replace(empty_labels, std::strlen("  labels: []"), R"(  labels:
    - id: 1
      priority: 100
      name: first
    - id: 2
      priority: 100
      name: second)");
    ExpectFailure(provider, duplicate_priority, flowsql::FlowLabelingErrorV1::kInvalidConfig);

    ExpectFailure(provider, EmptyConfig("rvv"), flowsql::FlowLabelingErrorV1::kUnsupportedAlgorithm,
                  "/spec/engine/algorithm");

    std::string over_budget = EmptyConfig("scalar", 64ULL * 1024ULL * 1024ULL);
    const size_t labels = over_budget.find("  labels: []");
    assert(labels != std::string::npos);
    over_budget.replace(labels, std::strlen("  labels: []"), R"(  labels:
    - id: 1
      priority: 1
      name: one)");
    ExpectFailure(provider, over_budget, flowsql::FlowLabelingErrorV1::kBudgetExceeded,
                  "/spec/engine/max_runtime_bytes");

    auto limited = MakeRequest(nullptr);
    limited.max_labels = 1;
    ExpectFailure(provider, FullConfig(), flowsql::FlowLabelingErrorV1::kLimitExceeded,
                  "/spec/engine/limits/max_labels", limited);

    limited = MakeRequest(nullptr);
    limited.max_compiled_rules = 1;
    ExpectFailure(provider, FullConfig(), flowsql::FlowLabelingErrorV1::kLimitExceeded,
                  "/spec/engine/limits/max_compiled_rules", limited);

    ExpectFailure(provider, SingleRuleConfig(false), flowsql::FlowLabelingErrorV1::kLimitExceeded, "/spec/rules");
    ExpectFailure(provider, SamePredicateDifferentLabelsConfig(), flowsql::FlowLabelingErrorV1::kLimitExceeded,
                  "/spec/rules");
    const auto deduplicated_snapshot = MakeSnapshot(kSchemaId, SingleRuleConfig(true));
    auto deduplicated_request = MakeRequest(&deduplicated_snapshot);
    flowsql::IFlowLabelMatcherV1* deduplicated = nullptr;
    flowsql::FlowLabelingDiagnosticV1 diagnostic;
    assert(provider->CreateMatcher(deduplicated_request, &deduplicated, &diagnostic) ==
           flowsql::FlowLabelingErrorV1::kNone);
    assert(deduplicated != nullptr);
    deduplicated->Release();

    std::string unknown_match = EmptyConfig();
    const size_t labels_position = unknown_match.find("  labels: []");
    const size_t rules_position = unknown_match.find("  rules: []");
    assert(labels_position != std::string::npos && rules_position != std::string::npos);
    unknown_match.replace(labels_position, std::strlen("  labels: []"), R"(  labels:
    - id: 1
      priority: 1
      name: one)");
    const size_t adjusted_rules = unknown_match.find("  rules: []");
    unknown_match.replace(adjusted_rules, std::strlen("  rules: []"), R"(  rules:
    - id: bad-field
      label_id: 1
      direction: bidirectional
      matches:
        - field: arbitrary_offset
          value: 1
          mask: 1)");
    ExpectFailure(provider, unknown_match, flowsql::FlowLabelingErrorV1::kInvalidConfig);

    auto budget_request = MakeRequest(nullptr);
    budget_request.reserved_module_state_bytes = 1200000;
    ExpectFailure(provider, BudgetConfig(256), flowsql::FlowLabelingErrorV1::kBudgetExceeded,
                  "/spec/engine/max_runtime_bytes", budget_request);

    const auto budget_snapshot = MakeSnapshot(kSchemaId, BudgetConfig(256));
    budget_request = MakeRequest(&budget_snapshot);
    budget_request.reserved_module_state_bytes = 1300000;
    flowsql::IFlowLabelMatcherV1* budget_matcher = nullptr;
    assert(provider->CreateMatcher(budget_request, &budget_matcher, &diagnostic) ==
           flowsql::FlowLabelingErrorV1::kNone);
    assert(budget_matcher != nullptr);
    budget_matcher->Release();
}

void TestCompressionEquivalence(flowsql::IFlowLabelingProviderV1* provider) {
    flowsql::FlowLabelingDiagnosticV1 diagnostic;
    const auto snapshot = MakeSnapshot(kSchemaId, CompressionEquivalenceConfig());
    auto request = MakeRequest(&snapshot);
    flowsql::IFlowLabelMatcherV1* matcher = nullptr;
    assert(provider->CreateMatcher(request, &matcher, &diagnostic) == flowsql::FlowLabelingErrorV1::kNone);

    flowsql::FlowLabelFactsV1 matching;
    matching.observation_domain_id = 297;
    matching.ip_family = 4;
    SetIpv4("192.0.2.1", &matching.source);
    matching.source.port = 1500;
    matching.source.port_valid = 1;
    std::vector<flowsql::FlowLabelFactsV1> facts = {matching, Reverse(matching)};
    auto range_miss = matching;
    range_miss.observation_domain_id = 289;
    facts.push_back(range_miss);
    auto prefix_miss = matching;
    prefix_miss.observation_domain_id = 305;
    facts.push_back(prefix_miss);
    auto bitmask_miss = matching;
    bitmask_miss.observation_domain_id = 296;
    facts.push_back(bitmask_miss);
    auto ip_presence_miss = matching;
    ip_presence_miss.source.ip_valid = 0;
    facts.push_back(ip_presence_miss);
    auto port_presence_miss = matching;
    port_presence_miss.source.port_valid = 0;
    facts.push_back(port_presence_miss);

    std::vector<uint32_t> labels(facts.size(), 99);
    assert(matcher->ClassifyBatch(facts.data(), facts.size(), labels.data()) == 0);
    assert(labels == std::vector<uint32_t>({2001, 2001, 0, 0, 0, 0, 0}));
    matcher->Release();

    const auto conflict_snapshot = MakeSnapshot(kSchemaId, CompressionEquivalenceConfig(true));
    request = MakeRequest(&conflict_snapshot);
    flowsql::IFlowLabelMatcherV1* conflict_matcher = nullptr;
    assert(provider->CreateMatcher(request, &conflict_matcher, &diagnostic) == flowsql::FlowLabelingErrorV1::kNone);
    uint32_t conflict_label = 99;
    assert(conflict_matcher->ClassifyBatch(&matching, 1, &conflict_label) == 0 && conflict_label == 0);
    conflict_matcher->Release();
}

void TestPresenceBitmap(flowsql::IFlowLabelingProviderV1* provider) {
    const auto snapshot = MakeSnapshot(kSchemaId, PresenceBitmapConfig());
    auto request = MakeRequest(&snapshot);
    flowsql::FlowLabelingDiagnosticV1 diagnostic;
    flowsql::IFlowLabelMatcherV1* matcher = nullptr;
    assert(provider->CreateMatcher(request, &matcher, &diagnostic) == flowsql::FlowLabelingErrorV1::kNone);

    flowsql::FlowLabelFactsV1 all_present;
    all_present.ip_family = 4;
    all_present.transport_valid = 1;
    all_present.source.mac_valid = 1;
    all_present.destination.mac_valid = 1;
    SetIpv4("192.0.2.1", &all_present.source);
    SetIpv4("198.51.100.1", &all_present.destination);
    all_present.source.port_valid = 1;
    all_present.destination.port_valid = 1;
    all_present.vlan[0].valid = 1;
    all_present.vlan[1].valid = 1;

    std::vector<flowsql::FlowLabelFactsV1> facts = {all_present};
    for (size_t missing = 0; missing < 9; ++missing) {
        auto candidate = all_present;
        switch (missing) {
            case 0:
                candidate.source.mac_valid = 0;
                break;
            case 1:
                candidate.destination.mac_valid = 0;
                break;
            case 2:
                candidate.source.ip_valid = 0;
                break;
            case 3:
                candidate.destination.ip_valid = 0;
                break;
            case 4:
                candidate.transport_valid = 0;
                break;
            case 5:
                candidate.source.port_valid = 0;
                break;
            case 6:
                candidate.destination.port_valid = 0;
                break;
            case 7:
                candidate.vlan[0].valid = 0;
                break;
            case 8:
                candidate.vlan[1].valid = 0;
                break;
        }
        facts.push_back(candidate);
    }
    std::vector<uint32_t> labels(facts.size(), 99);
    assert(matcher->ClassifyBatch(facts.data(), facts.size(), labels.data()) == 0);
    assert(labels.front() == 3001);
    for (size_t index = 1; index < labels.size(); ++index) {
        if (labels[index] != 0) std::cerr << "presence bit missing index " << index - 1 << " still matched\n";
    }
    assert(std::all_of(labels.begin() + 1, labels.end(), [](uint32_t label) { return label == 0; }));
    matcher->Release();
}

void ClassifyConcurrently(flowsql::IFlowLabelMatcherV1* matcher) {
    constexpr uint32_t kBatchSize = 16;
    constexpr int kIterations = 200;
    std::atomic<bool> succeeded{true};
    const auto classify = [&]() {
        std::vector<flowsql::FlowLabelFactsV1> facts(kBatchSize, WebFacts());
        std::vector<uint32_t> labels(kBatchSize, 99);
        for (int iteration = 0; iteration < kIterations; ++iteration) {
            std::fill(labels.begin(), labels.end(), 99);
            if (matcher->ClassifyBatch(facts.data(), kBatchSize, labels.data()) != 0) succeeded = false;
            for (uint32_t label : labels) {
                if (label != 1001) succeeded = false;
            }
            flowsql::FlowPrimaryLabelViewV1 view;
            if (!matcher->FindLabel(1001, &view) || view.label_id != 1001 || view.priority != 3000) {
                succeeded = false;
            }
        }
    };
    std::thread first(classify);
    std::thread second(classify);
    std::thread third(classify);
    std::thread fourth(classify);
    first.join();
    second.join();
    third.join();
    fourth.join();
    assert(succeeded.load());
}

void TestClassification(flowsql::IFlowLabelMatcherV1* matcher) {
    std::vector<flowsql::FlowLabelFactsV1> facts;
    facts.push_back(WebFacts());
    facts.push_back(Reverse(WebFacts()));
    auto missing_ip = WebFacts();
    missing_ip.destination.ip_valid = 0;
    facts.push_back(missing_ip);
    auto missing_vlan = WebFacts();
    missing_vlan.vlan[0].valid = 0;
    facts.push_back(missing_vlan);
    auto missing_protocol = WebFacts();
    missing_protocol.transport_valid = 0;
    facts.push_back(missing_protocol);
    auto missing_port = WebFacts();
    missing_port.destination.port_valid = 0;
    facts.push_back(missing_port);
    facts.push_back(MacFacts());
    facts.emplace_back();
    facts.push_back(Reverse(MacFacts()));
    facts.push_back(Ipv6Facts());
    auto missing_inner_vlan = Ipv6Facts();
    missing_inner_vlan.vlan[1].valid = 0;
    facts.push_back(missing_inner_vlan);
    facts.push_back(Reverse(Ipv6Facts()));
    facts.push_back(NativeMatrixFacts());
    facts.push_back(Reverse(NativeMatrixFacts()));
    facts.emplace_back();

    std::vector<uint32_t> labels(facts.size(), 99);
    assert(matcher->ClassifyBatch(facts.data(), facts.size(), labels.data()) == 0);
    const std::vector<uint32_t> expected = {
        1001, 1001, 0, 1002, 0, 0, 1003, 0, 1003, 1004, 0, 1004, 1005, 1005, 0,
    };
    assert(labels == expected);

    flowsql::FlowPrimaryLabelViewV1 view;
    assert(matcher->FindLabel(1001, &view));
    assert(view.label_id == 1001 && view.priority == 3000);
    assert(std::string(view.name) == "corp-web");
    assert(std::string(view.display_name) == "Corporate Web");
    assert(std::string(view.description) == "Corporate HTTPS traffic.");
    assert(!matcher->FindLabel(0, &view));
    assert(!matcher->FindLabel(9999, &view));

    flowsql::FlowLabelFactsV1 invalid;
    invalid.struct_size = 0;
    uint32_t invalid_output = 77;
    assert(matcher->ClassifyBatch(&invalid, 1, &invalid_output) != 0);
    assert(matcher->ClassifyBatch(nullptr, 1, &invalid_output) != 0);
    assert(matcher->ClassifyBatch(&invalid, 0, &invalid_output) != 0);
}

}  // namespace

int main(int argc, char** argv) {
    assert(argc == 2);
    TestStartupOptions();
    TestEalArguments();
    TestIsolatedEalStartup(argv[1]);
    flowsql::PluginLoader* loader = flowsql::PluginLoader::Single();
    const char* plugins[] = {argv[1]};
    const std::string missing = std::string(argv[1]) + ".not-installed";
    const char* missing_plugins[] = {missing.c_str()};
    assert(loader->Load(missing_plugins, 1) != 0);
    assert(loader->First(flowsql::IID_PLUGIN) == nullptr);
    const char* invalid_options[] = {"eal_memory_mib=511"};
    assert(loader->Load(flowsql::get_absolute_process_path(), plugins, invalid_options, 1) != 0);
    assert(loader->First(flowsql::IID_PLUGIN) == nullptr);
    assert(loader->First(flowsql::IID_FLOW_LABELING_PROVIDER_V1) == nullptr);

    const std::string option = "eal_memory_mib=512;eal_lcore_cpu=" + std::to_string(FirstAvailableCpu());
    const char* options[] = {option.c_str()};
    assert(loader->Load(flowsql::get_absolute_process_path(), plugins, options, 1) == 0);

    auto* plugin = static_cast<flowsql::IPlugin*>(loader->First(flowsql::IID_PLUGIN));
    auto* provider =
        static_cast<flowsql::IFlowLabelingProviderV1*>(loader->First(flowsql::IID_FLOW_LABELING_PROVIDER_V1));
    assert(plugin != nullptr && provider != nullptr);
    assert(plugin->Option(nullptr) != 0);

    flowsql::FlowLabelingDiagnosticV1 diagnostic;
    assert(provider->RuntimeStatus(&diagnostic) == flowsql::FlowLabelingErrorV1::kUnavailable);

    const auto empty_snapshot = MakeSnapshot(kSchemaId, EmptyConfig());
    auto request = MakeRequest(&empty_snapshot);
    auto* sentinel = reinterpret_cast<flowsql::IFlowLabelMatcherV1*>(static_cast<uintptr_t>(0x1));
    assert(provider->CreateMatcher(request, &sentinel, &diagnostic) == flowsql::FlowLabelingErrorV1::kUnavailable);
    assert(sentinel == reinterpret_cast<flowsql::IFlowLabelMatcherV1*>(static_cast<uintptr_t>(0x1)));

    assert(loader->StartAll() == 0);
    assert(loader->StartAll() == 0);
    assert(provider->RuntimeStatus(&diagnostic) == flowsql::FlowLabelingErrorV1::kNone);

    auto invalid_request = request;
    invalid_request.struct_size = 0;
    ExpectFailure(provider, EmptyConfig(), flowsql::FlowLabelingErrorV1::kInvalidSnapshot, "/snapshot",
                  invalid_request);
    const auto wrong_schema = MakeSnapshot("flowsql.io/other/v1", EmptyConfig());
    invalid_request = MakeRequest(&wrong_schema);
    assert(provider->CreateMatcher(invalid_request, &sentinel, &diagnostic) ==
           flowsql::FlowLabelingErrorV1::kInvalidSnapshot);
    assert(sentinel == reinterpret_cast<flowsql::IFlowLabelMatcherV1*>(static_cast<uintptr_t>(0x1)));

    TestStrictFailures(provider);
    TestCompressionEquivalence(provider);
    TestPresenceBitmap(provider);

    const auto template_snapshot = MakeSnapshot(kSchemaId, ReadTemplate());
    request = MakeRequest(&template_snapshot);
    flowsql::IFlowLabelMatcherV1* template_matcher = nullptr;
    assert(provider->CreateMatcher(request, &template_matcher, &diagnostic) == flowsql::FlowLabelingErrorV1::kNone);
    assert(template_matcher != nullptr);

    const auto full_snapshot = MakeSnapshot(kSchemaId, FullConfig());
    request = MakeRequest(&full_snapshot);
    flowsql::IFlowLabelMatcherV1* matcher = nullptr;
    assert(provider->CreateMatcher(request, &matcher, &diagnostic) == flowsql::FlowLabelingErrorV1::kNone);
    assert(matcher != nullptr);
    TestClassification(matcher);
    ClassifyConcurrently(matcher);

    std::string default_yaml = FullConfig();
    const size_t scalar = default_yaml.find("    algorithm: scalar");
    assert(scalar != std::string::npos);
    default_yaml.replace(scalar, std::strlen("    algorithm: scalar"), "    algorithm: default");
    const auto default_snapshot = MakeSnapshot(kSchemaId, default_yaml);
    request = MakeRequest(&default_snapshot);
    flowsql::IFlowLabelMatcherV1* default_matcher = nullptr;
    assert(provider->CreateMatcher(request, &default_matcher, &diagnostic) == flowsql::FlowLabelingErrorV1::kNone);
    auto default_facts = WebFacts();
    uint32_t default_label = 0;
    assert(default_matcher->ClassifyBatch(&default_facts, 1, &default_label) == 0 && default_label == 1001);

    flowsql::IFlowLabelMatcherV1* empty_matcher = nullptr;
    request = MakeRequest(&empty_snapshot);
    assert(provider->CreateMatcher(request, &empty_matcher, &diagnostic) == flowsql::FlowLabelingErrorV1::kNone);
    flowsql::FlowLabelFactsV1 unmatched;
    uint32_t unmatched_label = 99;
    assert(empty_matcher->ClassifyBatch(&unmatched, 1, &unmatched_label) == 0 && unmatched_label == 0);

    assert(plugin->Stop() != 0);
    assert(plugin->Unload() != 0);
    matcher->Release();
    default_matcher->Release();
    template_matcher->Release();
    empty_matcher->Release();

    loader->StopAll();
    assert(provider->RuntimeStatus(&diagnostic) == flowsql::FlowLabelingErrorV1::kUnavailable);
    assert(loader->Unload() == 0);

    std::cout << "Flow Labeling strict schema and DPDK ACL tests passed\n";
    return 0;
}
