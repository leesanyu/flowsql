// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <framework/interfaces/ibaseline_evaluation.h>
#include <operators/baseliner/model_output.h>
#include <plugins/baseline/baseline_plugin.h>
#include <services/database/database_plugin.h>
#include <time.h>
#include <unistd.h>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <numeric>
#include <vector>
using namespace flowsql;
using namespace flowsql::baseliner;
namespace {
using Clock = std::chrono::steady_clock;
constexpr int64_t kSubmits = 10000;
constexpr int kJoins = 200;
constexpr auto task_config =
    R"({"schema_version":1,"task_id":"cost","task_name":"contract cost","task_kind":"value","feature_id":"v","feature_type":"value_basic","profile":"default","clock_spec":{"bucket_seconds":60,"timezone":"UTC"},"calendar_ref":{"calendar_id":"cn-holiday","calendar_version":"2026.1"}})";
constexpr auto config =
    R"({"schema_version":1,"task_key":"contract-cost","source":"sqlite.cost","mode":"snapshot","clock":{"bucket_seconds":60,"timezone":"UTC"},"calendar":{"calendar_id":"cn-holiday","calendar_version":"2026.1"},"datasets":[{"id":"d","table":"facts","fields":{"id":"uint64","t":"int64","v":"float64"},"scope":{"begin_bucket":0,"end_bucket":20000,"consistency":"immutable_range"},"series_keys":["id"],"deduplicate":{"keys":["id","t"],"on_duplicate":"require_equal"},"bucket":{"column":"t","unit":"bucket_id"},"metrics":[{"id":"v","kind":"value","column":"v","feature_type":"value_basic","profile":"default"}]}]})";
int64_t CpuNs() {
    timespec t{};
    assert(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t) == 0);
    return int64_t(t.tv_sec) * 1000000000 + t.tv_nsec;
}
double Ns(Clock::duration d) { return std::chrono::duration<double, std::nano>(d).count(); }
struct Environment {
    baseline::BaselinePlugin plugin;
    Environment() { assert(plugin.Option(nullptr) == 0 && plugin.Load(nullptr) == 0 && plugin.Start() == 0); }
    ~Environment() {
        plugin.Stop();
        plugin.Unload();
    }
};
struct Measurement {
    double wall_ns = 0, cpu_ns = 0, checksum = 0;
    uint64_t cold = 0, ready = 0;
    BaselineSerializationResult snapshot;
};
Measurement Submit(bool typed) {
    Environment env;
    auto task = env.plugin.CreateValueTask(task_config, BaselineSerializationFormat::kJson).second;
    assert(task);
    auto* capability = dynamic_cast<IBaselineValueEvaluationV1*>(task.get());
    assert(capability);
    Measurement m;
    const auto cpu = CpuNs();
    const auto start = Clock::now();
    for (int64_t t = 0; t < kSubmits; ++t) {
        ValueRollingObservation observation{"s", t, 100. + t % 10, 1};
        RollingBaselineResult result;
        if (typed) {
            auto output = capability->SubmitObservationWithReadiness(observation, {});
            m.cold += output.readiness == BaselineEvaluationReadinessV1::kColdStart;
            m.ready += output.readiness == BaselineEvaluationReadinessV1::kReady;
            result = std::move(output.result);
        } else
            result = task->SubmitObservation(observation, {});
        assert(result.status == BaselineStatus::kOk);
        m.checksum += result.baseline_mu + result.baseline_lower + result.baseline_upper + result.update_weight;
    }
    m.wall_ns = Ns(Clock::now() - start);
    m.cpu_ns = CpuNs() - cpu;
    m.snapshot = task->QuerySeriesSnapshot("s", BaselineSerializationFormat::kJson);
    assert(m.snapshot.first == BaselineStatus::kOk);
    return m;
}
void Handoff() {
    ConfigSnapshot c;
    assert(ParseConfig(config, &c).ok());
    const auto path =
        std::filesystem::temp_directory_path() / ("baseliner-join-cost-" + std::to_string(getpid()) + ".db");
    const auto options = "type=sqlite;name=cost;path=" + path.string();
    database::DatabasePlugin db;
    assert(db.Option(options.c_str()) == 0 && db.Load(nullptr) == 0 && db.Start() == 0);
    std::vector<double> wall, cpu;
    double setup_ns = 0, close_ns = 0;
    {
        auto channel = db.AcquireChannel("sqlite", "cost");
        assert(channel);
        for (int i = 0; i < kJoins; ++i) {
            ModelOutputStore results, models;
            auto start = Clock::now();
            assert(results.Open(c, channel, "results", SchemaKind::kResults) == 0);
            assert(models.Open(c, channel, "models", SchemaKind::kModelParameters) == 0);
            setup_ns += Ns(Clock::now() - start);
            const auto cpu_start = CpuNs();
            start = Clock::now();
            assert(results.Join(models) == 0);
            wall.push_back(Ns(Clock::now() - start));
            cpu.push_back(CpuNs() - cpu_start);
            assert(results.SharesSession(models));
            start = Clock::now();
            results.Close();
            models.Close();
            close_ns += Ns(Clock::now() - start);
        }
    }
    db.Stop();
    db.Unload();
    std::sort(wall.begin(), wall.end());
    std::cout << "CONTRACT_JOIN {\"pairs\":" << kJoins << ",\"wall_median_ns\":" << wall[kJoins / 2]
              << ",\"wall_p95_ns\":" << wall[kJoins * 95 / 100]
              << ",\"wall_mean_ns\":" << std::accumulate(wall.begin(), wall.end(), 0.) / kJoins
              << ",\"cpu_mean_ns\":" << std::accumulate(cpu.begin(), cpu.end(), 0.) / kJoins
              << ",\"open_schema_mean_ns\":" << setup_ns / kJoins << ",\"final_close_mean_ns\":" << close_ns / kJoins
              << "}\n";
}
}  // namespace
int main(int argc, char** argv) {
    const bool reverse = argc > 1 && std::stoi(argv[1]) % 2;
    const auto first = Submit(reverse), second = Submit(!reverse);
    const auto& old = reverse ? second : first;
    const auto& typed = reverse ? first : second;
    assert(old.checksum == typed.checksum && old.snapshot == typed.snapshot);
    assert(typed.cold == 1 && typed.ready == kSubmits - 1);
    std::cout << "CONTRACT_SUBMIT {\"observations\":" << kSubmits << ",\"identities\":1,\"typed_first\":" << reverse
              << ",\"legacy_wall_ns_per_observation\":" << old.wall_ns / kSubmits
              << ",\"typed_wall_ns_per_observation\":" << typed.wall_ns / kSubmits
              << ",\"legacy_cpu_ns_per_observation\":" << old.cpu_ns / kSubmits
              << ",\"typed_cpu_ns_per_observation\":" << typed.cpu_ns / kSubmits << ",\"checksum\":" << typed.checksum
              << "}\n";
    Handoff();
}
