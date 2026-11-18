// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

#include <common/loader.hpp>
#include <framework/interfaces/ibaseline_service.h>
#include <framework/interfaces/ibaseline_types.h>

using namespace flowsql;

namespace {

constexpr double kConfidenceLevel = 0.95;
constexpr int32_t kDefaultDailyHarmonicOrder = 6;
constexpr int32_t kDefaultWeeklyHarmonicOrder = 3;

/* ------------------------------------------------------------------ */
/*  helpers                                                           */
/* ------------------------------------------------------------------ */

struct LoadedBaselineService {
    PluginLoader* loader = nullptr;
    IBaselineService* service = nullptr;

    ~LoadedBaselineService() {
        if (!loader) return;
        loader->StopAll();
        loader->Unload();
    }
};

int64_t UtcEpoch(int year, int month, int day, int hour, int minute, int second) {
    std::tm utc{};
    utc.tm_year = year - 1900;
    utc.tm_mon = month - 1;
    utc.tm_mday = day;
    utc.tm_hour = hour;
    utc.tm_min = minute;
    utc.tm_sec = second;
    const std::time_t epoch = timegm(&utc);
    return static_cast<int64_t>(epoch);
}

std::string BuildRollingConfigYaml(int32_t daily_order, int32_t weekly_order) {
    std::ostringstream out;
    out << "baseline:\n"
        << "  parser:\n"
        << "    tz_default: \"Asia/Shanghai\"\n"
        << "  shared_profile_config:\n"
        << "    daily_harmonic_order: " << daily_order << "\n"
        << "    weekly_harmonic_order: " << weekly_order << "\n"
        << "    dme_max: 7\n"
        << "    m_month_enable: 4\n"
        << "    month_cov_min: 0.8\n"
        << "    lambda_season: 1.0\n"
        << "    lambda_dom: 4.0\n"
        << "    lambda_dme: 2.0\n"
        << "    lambda_lwd: 1.0\n"
        << "    lambda_event: 2.0\n"
        << "  rolling_config:\n"
        << "    band_z: 2.0\n"
        << "  value_sampled_profiles:\n"
        << "    cont_core:\n"
        << "      n_train_min: 50\n"
        << "      transform_name_override: \"log1p\"\n"
        << "  ratio_profiles:\n"
        << "    global:\n"
        << "      eps_logit: 1.0e-4\n"
        << "      m_floor: 1.0e-4\n"
        << "      v_floor: 0.25\n"
        << "    rate_core:\n"
        << "      d_min_train: 50\n"
        << "      s_prior: 2.0\n"
        << "      phi_over: 1.5\n"
        << "  solver_constants:\n"
        << "    solver_name: \"weighted_huber_ridge_irls\"\n"
        << "    c_huber: 1.5\n"
        << "    s_min_fit: 1.0e-3\n"
        << "    max_iter_fit: 15\n"
        << "    tol_obj_rel: 1.0e-4\n"
        << "    tol_beta_inf: 1.0e-5\n"
        << "    cond_max: 1.0e8\n";
    return out.str();
}

std::string ValueTaskJson(const std::string& task_id, const std::string& tz, int64_t bucket_seconds) {
    std::ostringstream out;
    out << R"({
        "schema_version": 1,
        "task_id": ")" << task_id << R"(",
        "task_name": "perf test value task",
        "task_kind": "value",
        "feature_id": "perf_test_feature",
        "feature_type": "value_basic",
        "profile": "default",
        "clock_spec": {
            "bucket_seconds": )" << bucket_seconds << R"(,
            "timezone": ")" << tz << R"("
        },
        "calendar_ref": {
            "calendar_id": "cn-holiday",
            "calendar_version": "2026.1"
        }
    })";
    return out.str();
}

bool WriteTextFile(const std::filesystem::path& path, std::string_view content) {
    std::ofstream f(path);
    if (!f.is_open()) return false;
    f << content;
    return f.good();
}

LoadedBaselineService LoadBaselineService(const std::filesystem::path& config_path) {
    LoadedBaselineService env;
    env.loader = PluginLoader::Single();

    std::string plugin_dir = get_absolute_process_path();
    std::string plugin_name = "libflowsql_baseline.so";
    const char* relapath[] = {plugin_name.c_str()};
    const std::string option = "config_file=" + config_path.string() + ";strict=true";
    const char* options[] = {option.c_str()};

    const int ret = env.loader->Load(plugin_dir.c_str(), relapath, options, 1);
    assert(ret == 0);
    assert(env.loader->StartAll() == 0);

    env.service = static_cast<IBaselineService*>(env.loader->First(IID_BASELINE_SERVICE));
    assert(env.service != nullptr);
    return env;
}

using Clock = std::chrono::steady_clock;

double ElapsedMicroseconds(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
}

/* ------------------------------------------------------------------ */
/*  Bootstrap a task with synthetic sinusoidal data.                   */
/* ------------------------------------------------------------------ */

void BootstrapSyntheticData(IBaselineValueTask* task,
                            const std::string& series_key,
                            int64_t bucket_seconds,
                            int64_t train_start_epoch,
                            int num_points) {
    ValueBootstrapInput bootstrap_input;
    bootstrap_input.series_key = series_key;
    bootstrap_input.observations.reserve(num_points);
    for (int i = 0; i < num_points; ++i) {
        ValueBootstrapPoint pt;
        pt.bucket_id = train_start_epoch / bucket_seconds + i;
        pt.value = 100.0 + 20.0 * std::sin(2.0 * 3.1415926535 * i / (24.0 * 60.0));
        pt.sample_count = 1;
        bootstrap_input.observations.push_back(pt);
    }
    const BootstrapTrainResult train_result = task->Bootstrap(bootstrap_input);
    if (train_result.status != BaselineStatus::kOk) {
        std::cerr << "bootstrap failed: " << train_result.diagnostics << "\n";
    }
}

void Warmup(IBaselineValueTask* task, const std::string& series_key, int64_t start_bucket) {
    for (int i = 0; i < 3; ++i) {
        ValueRollingObservation obs;
        obs.series_key = series_key;
        obs.bucket_id = start_bucket + i;
        obs.value = 100.0;
        obs.sample_count = 1;
        task->SubmitObservation(obs, RollingSubmitOptions{}).result;
        task->PredictRolling(series_key, obs.bucket_id);
    }
}

/* ------------------------------------------------------------------ */
/*  Rolling sequence: compare single-point loop vs batch sequence.     */
/* ------------------------------------------------------------------ */

struct RollingPerfResult {
    std::string timezone;
    uint32_t point_count = 0;
    int64_t bucket_seconds = 0;
    int32_t daily_order = 0;
    int32_t weekly_order = 0;

    double single_loop_us = 0.0;
    double batch_sequence_us = 0.0;
    double speedup = 0.0;
    double max_relative_mu_diff = 0.0;
};

RollingPerfResult MeasureRollingSequencePerf(IBaselineValueTask* task,
                                              const std::string& series_key,
                                              int32_t daily_order,
                                              int32_t weekly_order,
                                              const std::string& timezone,
                                              int64_t bucket_seconds,
                                              uint32_t point_count) {
    RollingPerfResult result;
    result.daily_order = daily_order;
    result.weekly_order = weekly_order;
    result.timezone = timezone;
    result.point_count = point_count;
    result.bucket_seconds = bucket_seconds;

    const int64_t start_bucket = UtcEpoch(2026, 5, 1, 0, 0, 0) / bucket_seconds;

    Warmup(task, series_key, start_bucket);

    /* Single-point loop */
    auto single_start = Clock::now();
    std::vector<double> single_mus;
    single_mus.reserve(point_count);
    for (uint32_t i = 0; i < point_count; ++i) {
        const int64_t bucket_id = start_bucket + static_cast<int64_t>(i);
        const RollingPrediction pred = task->PredictRolling(series_key, bucket_id);
        if (pred.status == BaselineStatus::kOk) {
            single_mus.push_back(pred.baseline_mu);
        }
    }
    auto single_end = Clock::now();
    result.single_loop_us = ElapsedMicroseconds(single_start, single_end);

    /* Batch sequence */
    auto batch_start = Clock::now();
    const RollingPredictionSequence sequence =
        task->PredictRolling(series_key, start_bucket, point_count);
    auto batch_end = Clock::now();
    result.batch_sequence_us = ElapsedMicroseconds(batch_start, batch_end);

    if (sequence.status == BaselineStatus::kOk &&
        sequence.predictions.size() == point_count &&
        single_mus.size() == point_count) {
        double max_rel = 0.0;
        for (uint32_t i = 0; i < point_count; ++i) {
            const double s = single_mus[i];
            const double b = sequence.predictions[i].baseline_mu;
            if (std::fabs(s) > 1e-12) {
                max_rel = std::max(max_rel, std::fabs(b - s) / std::fabs(s));
            }
        }
        result.max_relative_mu_diff = max_rel;
    }

    result.speedup =
        result.batch_sequence_us > 0 ? result.single_loop_us / result.batch_sequence_us : 0.0;

    return result;
}

/* ------------------------------------------------------------------ */
/*  Bootstrap sequence: single-point loop vs batch sequence.           */
/* ------------------------------------------------------------------ */

struct BootstrapPerfResult {
    std::string timezone;
    uint32_t point_count = 0;
    int64_t bucket_seconds = 0;
    int32_t daily_order = 0;
    int32_t weekly_order = 0;

    double single_loop_us = 0.0;
    double batch_sequence_us = 0.0;
    double speedup = 0.0;
    double max_relative_mu_diff = 0.0;
};

BootstrapPerfResult MeasureBootstrapSequencePerf(IBaselineValueTask* task,
                                                  const std::string& series_key,
                                                  int64_t bucket_seconds,
                                                  uint32_t point_count,
                                                  const BootstrapPredictionOptions& options) {
    BootstrapPerfResult result;
    result.timezone = "";
    result.point_count = point_count;
    result.bucket_seconds = bucket_seconds;

    const int64_t start_bucket = UtcEpoch(2026, 5, 1, 0, 0, 0) / bucket_seconds;

    /* Single-point loop */
    auto single_start = Clock::now();
    std::vector<double> single_mus;
    single_mus.reserve(point_count);
    for (uint32_t i = 0; i < point_count; ++i) {
        const int64_t bucket_id = start_bucket + static_cast<int64_t>(i);
        const BootstrapPrediction pred =
            task->PredictBootstrap(series_key, bucket_id, options);
        if (pred.status == BaselineStatus::kOk) {
            single_mus.push_back(pred.baseline_mu);
        }
    }
    auto single_end = Clock::now();
    result.single_loop_us = ElapsedMicroseconds(single_start, single_end);

    /* Batch sequence */
    auto batch_start = Clock::now();
    const BootstrapPredictionSequence sequence =
        task->PredictBootstrap(series_key, start_bucket, point_count, options);
    auto batch_end = Clock::now();
    result.batch_sequence_us = ElapsedMicroseconds(batch_start, batch_end);

    if (sequence.status == BaselineStatus::kOk &&
        sequence.predictions.size() == point_count &&
        single_mus.size() == point_count) {
        double max_rel = 0.0;
        for (uint32_t i = 0; i < point_count; ++i) {
            const double s = single_mus[i];
            const double b = sequence.predictions[i].baseline_mu;
            if (std::fabs(s) > 1e-12) {
                max_rel = std::max(max_rel, std::fabs(b - s) / std::fabs(s));
            }
        }
        result.max_relative_mu_diff = max_rel;
    }

    result.speedup =
        result.batch_sequence_us > 0 ? result.single_loop_us / result.batch_sequence_us : 0.0;

    return result;
}

/* ------------------------------------------------------------------ */
/*  JSON report                                                       */
/* ------------------------------------------------------------------ */

std::string PerfReportJson(const std::vector<RollingPerfResult>& rolling_results,
                            const std::vector<BootstrapPerfResult>& bootstrap_results) {
    std::ostringstream out;
    out << "{\n";
    out << "  \"schema_version\": 1,\n";
    out << "  \"document_kind\": \"baseline_batch_prediction_perf_report\",\n";
    out << "  \"rolling_sequence_perf\": [\n";
    for (std::size_t i = 0; i < rolling_results.size(); ++i) {
        const auto& r = rolling_results[i];
        out << "    {\n"
            << "      \"daily_order\": " << r.daily_order << ",\n"
            << "      \"weekly_order\": " << r.weekly_order << ",\n"
            << "      \"timezone\": \"" << r.timezone << "\",\n"
            << "      \"point_count\": " << r.point_count << ",\n"
            << "      \"bucket_seconds\": " << r.bucket_seconds << ",\n"
            << "      \"single_loop_us\": " << std::fixed << std::setprecision(1)
            << r.single_loop_us << ",\n"
            << "      \"batch_sequence_us\": " << r.batch_sequence_us << ",\n"
            << "      \"speedup\": " << std::setprecision(2) << r.speedup << ",\n"
            << "      \"max_relative_mu_diff\": " << std::scientific
            << std::setprecision(6) << r.max_relative_mu_diff << "\n"
            << "    }" << (i + 1 < rolling_results.size() ? "," : "") << "\n";
    }
    out << "  ],\n";
    out << "  \"bootstrap_sequence_perf\": [\n";
    for (std::size_t i = 0; i < bootstrap_results.size(); ++i) {
        const auto& r = bootstrap_results[i];
        out << "    {\n"
            << "      \"timezone\": \"" << r.timezone << "\",\n"
            << "      \"point_count\": " << r.point_count << ",\n"
            << "      \"bucket_seconds\": " << r.bucket_seconds << ",\n"
            << "      \"daily_order\": " << r.daily_order << ",\n"
            << "      \"weekly_order\": " << r.weekly_order << ",\n"
            << "      \"single_loop_us\": " << std::fixed << std::setprecision(1)
            << r.single_loop_us << ",\n"
            << "      \"batch_sequence_us\": " << r.batch_sequence_us << ",\n"
            << "      \"speedup\": " << std::setprecision(2) << r.speedup << ",\n"
            << "      \"max_relative_mu_diff\": " << std::scientific
            << std::setprecision(6) << r.max_relative_mu_diff << "\n"
            << "    }" << (i + 1 < bootstrap_results.size() ? "," : "") << "\n";
    }
    out << "  ]\n";
    out << "}\n";
    return out.str();
}

/* ------------------------------------------------------------------ */
/*  Table printer for stdout                                          */
/* ------------------------------------------------------------------ */

void PrintRollingTable(const std::vector<RollingPerfResult>& results) {
    std::cout << "\n=== Rolling Sequence Prediction Performance ===\n";
    std::cout << std::left << std::setw(6) << "Points"
              << std::setw(6) << "Daily"
              << std::setw(6) << "Week"
              << std::setw(12) << "Timezone"
              << std::setw(14) << "Single(us)"
              << std::setw(14) << "Batch(us)"
              << std::setw(10) << "Speedup"
              << std::setw(16) << "MaxRelDiff"
              << "\n";
    std::cout << std::string(90, '-') << "\n";
    for (const auto& r : results) {
        std::cout << std::left << std::setw(6) << r.point_count
                  << std::setw(6) << r.daily_order
                  << std::setw(6) << r.weekly_order
                  << std::setw(12) << r.timezone
                  << std::fixed << std::setprecision(0) << std::setw(14) << r.single_loop_us
                  << std::setw(14) << r.batch_sequence_us
                  << std::setprecision(2) << std::setw(10) << r.speedup
                  << std::scientific << std::setprecision(2) << std::setw(16) << r.max_relative_mu_diff
                  << "\n";
    }
}

void PrintBootstrapTable(const std::vector<BootstrapPerfResult>& results) {
    std::cout << "\n=== Bootstrap Sequence Prediction Performance ===\n";
    std::cout << std::left << std::setw(6) << "Points"
              << std::setw(6) << "Daily"
              << std::setw(6) << "Week"
              << std::setw(12) << "Timezone"
              << std::setw(14) << "Single(us)"
              << std::setw(14) << "Batch(us)"
              << std::setw(10) << "Speedup"
              << std::setw(16) << "MaxRelDiff"
              << "\n";
    std::cout << std::string(90, '-') << "\n";
    for (const auto& r : results) {
        std::cout << std::left << std::setw(6) << r.point_count
                  << std::setw(6) << r.daily_order
                  << std::setw(6) << r.weekly_order
                  << std::setw(12) << r.timezone
                  << std::fixed << std::setprecision(0) << std::setw(14) << r.single_loop_us
                  << std::setw(14) << r.batch_sequence_us
                  << std::setprecision(2) << std::setw(10) << r.speedup
                  << std::scientific << std::setprecision(2) << std::setw(16) << r.max_relative_mu_diff
                  << "\n";
    }
}

}  // namespace

int main(int argc, char** argv) {
    const std::filesystem::path output_dir =
        argc >= 2 ? std::filesystem::path(argv[1])
                  : std::filesystem::path(get_absolute_process_path()) /
                        "baseline_batch_prediction_perf";
    const int32_t daily_order =
        argc >= 3 ? std::atoi(argv[2]) : kDefaultDailyHarmonicOrder;
    const int32_t weekly_order =
        argc >= 4 ? std::atoi(argv[3]) : kDefaultWeeklyHarmonicOrder;

    std::filesystem::create_directories(output_dir);
    const std::filesystem::path config_path = output_dir / "runtime_config.yaml";
    if (!WriteTextFile(config_path, BuildRollingConfigYaml(daily_order, weekly_order))) {
        std::cerr << "failed to write config\n";
        return 1;
    }

    LoadedBaselineService env = LoadBaselineService(config_path);

    /* --------------------------------------------------------------- */
    /*  Create one task per timezone (clock_spec is per-task).         */
    /* --------------------------------------------------------------- */

    const int64_t bucket_seconds = 60;

    struct TestCase {
        std::string timezone;
        uint32_t point_count;
    };

    const std::vector<TestCase> rolling_cases = {
        {"UTC", 50},
        {"UTC", 200},
        {"UTC", 1440},
        {"UTC", 10080},
        {"Asia/Shanghai", 1440},
        {"Asia/Shanghai", 288},
        {"America/New_York", 1440},
    };

    const std::vector<TestCase> bootstrap_cases = {
        {"Asia/Shanghai", 50},
        {"Asia/Shanghai", 200},
        {"Asia/Shanghai", 1440},
        {"Asia/Shanghai", 10080},
    };

    std::vector<RollingPerfResult> rolling_results;
    std::vector<BootstrapPerfResult> bootstrap_results;

    /* --------------------------------------------------------------- */
    /*  Rolling: create task, bootstrap, measure per-timezone.         */
    /* --------------------------------------------------------------- */

    BootstrapPredictionOptions predict_options;
    predict_options.confidence_level = kConfidenceLevel;
    predict_options.include_model_space_debug = false;

    for (const auto& tc : rolling_cases) {
        std::string task_id = "perf-rolling-" + tc.timezone + "-" + std::to_string(tc.point_count);
        auto [task_status, task] =
            env.service->CreateValueTask(ValueTaskJson(task_id, tc.timezone, bucket_seconds),
                                         BaselineSerializationFormat::kJson);
        if (task_status != BaselineStatus::kOk || !task) {
            std::cerr << "create task failed for timezone " << tc.timezone << "\n";
            continue;
        }

        const std::string series_key = "perf-rolling-" + tc.timezone;
        const int64_t train_start_epoch = UtcEpoch(2026, 4, 1, 0, 0, 0);
        const int num_train_points = 7 * 24 * 60;  // 1 week of 1-min data
        BootstrapSyntheticData(task.get(), series_key, bucket_seconds,
                               train_start_epoch, num_train_points);

        auto r = MeasureRollingSequencePerf(task.get(),
                                             series_key,
                                             daily_order,
                                             weekly_order,
                                             tc.timezone,
                                             bucket_seconds,
                                             tc.point_count);
        rolling_results.push_back(std::move(r));
    }

    /* --------------------------------------------------------------- */
    /*  Bootstrap: create task, bootstrap, measure per-timezone.       */
    /* --------------------------------------------------------------- */

    for (const auto& tc : bootstrap_cases) {
        std::string task_id = "perf-bootstrap-" + tc.timezone + "-" + std::to_string(tc.point_count);
        auto [task_status, task] =
            env.service->CreateValueTask(ValueTaskJson(task_id, tc.timezone, bucket_seconds),
                                         BaselineSerializationFormat::kJson);
        if (task_status != BaselineStatus::kOk || !task) {
            std::cerr << "create task failed for timezone " << tc.timezone << "\n";
            continue;
        }

        const std::string series_key = "perf-bootstrap-" + tc.timezone;
        const int64_t train_start_epoch = UtcEpoch(2026, 4, 1, 0, 0, 0);
        const int num_train_points = 7 * 24 * 60;
        BootstrapSyntheticData(task.get(), series_key, bucket_seconds,
                               train_start_epoch, num_train_points);

        auto r = MeasureBootstrapSequencePerf(task.get(),
                                               series_key,
                                               bucket_seconds,
                                               tc.point_count,
                                               predict_options);
        bootstrap_results.push_back(std::move(r));
    }

    /* --------------------------------------------------------------- */
    /*  Report                                                         */
    /* --------------------------------------------------------------- */

    PrintRollingTable(rolling_results);
    PrintBootstrapTable(bootstrap_results);

    const std::filesystem::path report_path = output_dir / "batch_prediction_perf_report.json";
    if (!WriteTextFile(report_path, PerfReportJson(rolling_results, bootstrap_results))) {
        std::cerr << "failed to write perf report\n";
        return 1;
    }

    /* --------------------------------------------------------------- */
    /*  Assertions                                                     */
    /* --------------------------------------------------------------- */

    bool all_ok = true;
    for (const auto& r : rolling_results) {
        if (r.max_relative_mu_diff > 1e-9) {
            std::cerr << "FAIL: rolling " << r.timezone << " " << r.point_count
                      << " points: max relative mu diff = " << r.max_relative_mu_diff
                      << " exceeds 1e-9\n";
            all_ok = false;
        }
    }
    for (const auto& r : bootstrap_results) {
        if (r.max_relative_mu_diff > 1e-9) {
            std::cerr << "FAIL: bootstrap " << r.point_count
                      << " points: max relative mu diff = " << r.max_relative_mu_diff
                      << " exceeds 1e-9\n";
            all_ok = false;
        }
    }

    if (all_ok) {
        std::cout << "\nAll equivalence checks passed.\n";
        std::cout << "report=" << report_path << "\n";
    }

    return all_ok ? 0 : 1;
}
