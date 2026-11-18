// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <cassert>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include <common/loader.hpp>
#include <framework/interfaces/ibaseline_service.h>

using namespace flowsql;

namespace {

constexpr int64_t kBucketSeconds = 60;
constexpr int64_t kDayMinutes = 24 * 60;
constexpr int64_t kWeekMinutes = 7 * kDayMinutes;
constexpr int64_t kShanghaiUtcOffsetSeconds = 8 * 60 * 60;
constexpr double kConfidenceLevel = 0.95;
constexpr double kBootstrapBandZ = 1.96;
constexpr double kRollingBandZ = 1.96;
constexpr int32_t kDefaultDailyHarmonicOrder = 6;
constexpr int32_t kDefaultWeeklyHarmonicOrder = 3;
constexpr const char* kSeriesKey = "link-traffic-bps";

struct LoadedBaselineService {
    PluginLoader* loader = nullptr;
    IBaselineService* service = nullptr;

    ~LoadedBaselineService() {
        if (!loader) return;
        loader->StopAll();
        loader->Unload();
    }
};

struct LinkPoint {
    std::string timestamp;
    int64_t bucket_id = 0;
    double mbps = 0.0;
};

struct EvalSummary {
    uint64_t count = 0;
    uint64_t inside_band_count = 0;
    uint64_t abs_z_gt_3_count = 0;
    uint64_t abs_z_gt_5_count = 0;
    double coverage_ratio = 0.0;
    double mean_abs_z = 0.0;
    double max_abs_z = 0.0;
    double rmse = 0.0;
    double mape = 0.0;
    double mean_band_width = 0.0;
};

struct SmoothnessSummary {
    uint64_t adjacent_count = 0;
    double mean_abs_delta_mu = 0.0;
    double mean_abs_delta_upper = 0.0;
    double mean_abs_delta_width = 0.0;
};

struct TransitionCheckpoint {
    int64_t minute_offset = 0;
    std::string timestamp;
    double actual = 0.0;
    double baseline_mu = 0.0;
    double abs_z = 0.0;
    double drift_evidence = 0.0;
    double adapt_boost = 0.0;
    double update_weight = 0.0;
    std::string maturity_status;
    std::string score_trust_status;
    std::string calibration_status;
    bool can_alert = false;
};

struct B3StatusSummary {
    uint64_t count = 0;
    uint64_t can_score_count = 0;
    uint64_t can_alert_count = 0;
    uint64_t score_untrusted_count = 0;
    uint64_t score_warming_count = 0;
    uint64_t score_ready_count = 0;
    uint64_t drift_learning_count = 0;
    uint64_t recalibrating_count = 0;
    uint64_t daily_ready_or_above_count = 0;
    uint64_t weekly_ready_or_above_count = 0;
    uint64_t calibration_warming_count = 0;
    uint64_t calibration_calibrated_count = 0;
    uint64_t calibration_expanding_count = 0;
    uint64_t calibration_recalibrating_count = 0;

    void Add(const RollingBaselineResult& result) {
        ++count;
        if (result.can_score) ++can_score_count;
        if (result.can_alert) ++can_alert_count;
        if (result.score_trust_status == "score_untrusted") ++score_untrusted_count;
        if (result.score_trust_status == "score_warming") ++score_warming_count;
        if (result.score_trust_status == "score_ready") ++score_ready_count;
        if (result.score_trust_status == "drift_learning") ++drift_learning_count;
        if (result.score_trust_status == "recalibrating") ++recalibrating_count;
        if (result.maturity_status == "daily_ready" ||
            result.maturity_status == "weekly_warming" ||
            result.maturity_status == "weekly_ready" ||
            result.maturity_status == "monthly_warming" ||
            result.maturity_status == "monthly_ready") {
            ++daily_ready_or_above_count;
        }
        if (result.maturity_status == "weekly_ready" ||
            result.maturity_status == "monthly_warming" ||
            result.maturity_status == "monthly_ready") {
            ++weekly_ready_or_above_count;
        }
        if (result.calibration_status == "warming") ++calibration_warming_count;
        if (result.calibration_status == "calibrated") ++calibration_calibrated_count;
        if (result.calibration_status == "expanding") ++calibration_expanding_count;
        if (result.calibration_status == "recalibrating") ++calibration_recalibrating_count;
    }
};

bool TryGetDiagnosticValue(const std::string& diagnostics,
                           const std::string& key,
                           double* out) {
    if (!out) return false;
    const std::string needle = key + "=";
    std::size_t pos = diagnostics.find(needle);
    while (pos != std::string::npos && pos > 0 && diagnostics[pos - 1] != ';') {
        pos = diagnostics.find(needle, pos + 1);
    }
    if (pos == std::string::npos) return false;

    const std::size_t begin = pos + needle.size();
    const std::size_t end = diagnostics.find(';', begin);
    const std::string text =
        diagnostics.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
    char* parse_end = nullptr;
    const double value = std::strtod(text.c_str(), &parse_end);
    if (parse_end == text.c_str() || !std::isfinite(value)) return false;
    *out = value;
    return true;
}

struct RollingObservabilitySummary {
    uint64_t count = 0;
    uint64_t diagnostics_count = 0;
    uint64_t cap_applied_count = 0;
    double sum_std_log = 0.0;
    double sum_raw_std_log = 0.0;
    double sum_pred_var = 0.0;
    double sum_calibrated_sigma_var = 0.0;
    double sum_extra_obs_var = 0.0;
    double sum_maturity_var = 0.0;
    double sum_missing_component_var = 0.0;
    double sum_multiplier = 0.0;
    double sum_abs_level_shift_evidence = 0.0;
    double max_abs_level_shift_evidence = 0.0;
    double max_combined_drift_evidence = 0.0;

    void Add(const RollingBaselineResult& result) {
        ++count;
        double std_log = 0.0;
        double raw_std_log = 0.0;
        double pred_var = 0.0;
        double calibrated_sigma_var = 0.0;
        double extra_obs_var = 0.0;
        double maturity_var = 0.0;
        double missing_component_var = 0.0;
        double multiplier = 0.0;
        double cap_applied = 0.0;
        double level_shift_evidence = 0.0;
        double combined_drift_evidence = 0.0;
        if (!TryGetDiagnosticValue(result.diagnostics, "std_log", &std_log) ||
            !TryGetDiagnosticValue(result.diagnostics, "raw_std_log", &raw_std_log) ||
            !TryGetDiagnosticValue(result.diagnostics, "pred_var", &pred_var) ||
            !TryGetDiagnosticValue(result.diagnostics,
                                   "calibrated_sigma_var",
                                   &calibrated_sigma_var) ||
            !TryGetDiagnosticValue(result.diagnostics, "extra_obs_var", &extra_obs_var) ||
            !TryGetDiagnosticValue(result.diagnostics, "maturity_var", &maturity_var) ||
            !TryGetDiagnosticValue(result.diagnostics,
                                   "missing_component_var",
                                   &missing_component_var) ||
            !TryGetDiagnosticValue(result.diagnostics, "multiplier", &multiplier) ||
            !TryGetDiagnosticValue(result.diagnostics, "cap_applied", &cap_applied)) {
            return;
        }

        ++diagnostics_count;
        sum_std_log += std_log;
        sum_raw_std_log += raw_std_log;
        sum_pred_var += pred_var;
        sum_calibrated_sigma_var += calibrated_sigma_var;
        sum_extra_obs_var += extra_obs_var;
        sum_maturity_var += maturity_var;
        sum_missing_component_var += missing_component_var;
        sum_multiplier += multiplier;
        if (cap_applied >= 0.5) ++cap_applied_count;

        if (TryGetDiagnosticValue(result.diagnostics,
                                  "level_shift_evidence",
                                  &level_shift_evidence)) {
            const double abs_value = std::fabs(level_shift_evidence);
            sum_abs_level_shift_evidence += abs_value;
            max_abs_level_shift_evidence =
                std::max(max_abs_level_shift_evidence, abs_value);
        }
        if (TryGetDiagnosticValue(result.diagnostics,
                                  "combined_drift_evidence",
                                  &combined_drift_evidence)) {
            max_combined_drift_evidence =
                std::max(max_combined_drift_evidence, std::fabs(combined_drift_evidence));
        }
    }
};

struct EvalAccumulator {
    uint64_t count = 0;
    uint64_t inside_band_count = 0;
    uint64_t abs_z_gt_3_count = 0;
    uint64_t abs_z_gt_5_count = 0;
    double sum_abs_z = 0.0;
    double max_abs_z = 0.0;
    double sum_square_error = 0.0;
    double sum_abs_percentage_error = 0.0;
    double sum_band_width = 0.0;

    void Add(double actual,
             double baseline_mu,
             double baseline_lower,
             double baseline_upper,
             double band_z) {
        const double z = DirectionalZ(actual,
                                      baseline_mu,
                                      baseline_lower,
                                      baseline_upper,
                                      band_z);
        const double abs_z = std::fabs(z);
        const double error = actual - baseline_mu;
        if (actual >= baseline_lower && actual <= baseline_upper) ++inside_band_count;
        if (abs_z > 3.0) ++abs_z_gt_3_count;
        if (abs_z > 5.0) ++abs_z_gt_5_count;
        sum_abs_z += abs_z;
        max_abs_z = std::max(max_abs_z, abs_z);
        sum_square_error += error * error;
        sum_band_width += std::max(0.0, baseline_upper - baseline_lower);
        if (std::fabs(actual) > 1.0e-9) {
            sum_abs_percentage_error += std::fabs(error / actual);
        }
        ++count;
    }

    EvalSummary Finish() const {
        EvalSummary summary;
        summary.count = count;
        summary.inside_band_count = inside_band_count;
        summary.abs_z_gt_3_count = abs_z_gt_3_count;
        summary.abs_z_gt_5_count = abs_z_gt_5_count;
        summary.max_abs_z = max_abs_z;
        if (count > 0) {
            const double n = static_cast<double>(count);
            summary.coverage_ratio = static_cast<double>(inside_band_count) / n;
            summary.mean_abs_z = sum_abs_z / n;
            summary.rmse = std::sqrt(sum_square_error / n);
            summary.mape = sum_abs_percentage_error / n;
            summary.mean_band_width = sum_band_width / n;
        }
        return summary;
    }

    static double DirectionalZ(double actual,
                               double baseline_mu,
                               double baseline_lower,
                               double baseline_upper,
                               double band_z) {
        const double safe_z = band_z > 1.0e-9 ? band_z : 1.0;
        const double upper_sigma = (baseline_upper - baseline_mu) / safe_z;
        const double lower_sigma = (baseline_mu - baseline_lower) / safe_z;
        const double fallback_sigma =
            std::max((baseline_upper - baseline_lower) / (2.0 * safe_z), 1.0e-9);
        double sigma = fallback_sigma;
        if (actual >= baseline_mu && upper_sigma > 1.0e-9) {
            sigma = upper_sigma;
        } else if (actual < baseline_mu && lower_sigma > 1.0e-9) {
            sigma = lower_sigma;
        }
        return (actual - baseline_mu) / sigma;
    }
};

struct SmoothnessAccumulator {
    uint64_t adjacent_count = 0;
    bool has_previous = false;
    double previous_mu = 0.0;
    double previous_upper = 0.0;
    double previous_width = 0.0;
    double sum_abs_delta_mu = 0.0;
    double sum_abs_delta_upper = 0.0;
    double sum_abs_delta_width = 0.0;

    void Add(double baseline_mu, double baseline_lower, double baseline_upper) {
        const double width = std::max(0.0, baseline_upper - baseline_lower);
        if (has_previous) {
            sum_abs_delta_mu += std::fabs(baseline_mu - previous_mu);
            sum_abs_delta_upper += std::fabs(baseline_upper - previous_upper);
            sum_abs_delta_width += std::fabs(width - previous_width);
            ++adjacent_count;
        }
        previous_mu = baseline_mu;
        previous_upper = baseline_upper;
        previous_width = width;
        has_previous = true;
    }

    SmoothnessSummary Finish() const {
        SmoothnessSummary summary;
        summary.adjacent_count = adjacent_count;
        if (adjacent_count > 0) {
            const double n = static_cast<double>(adjacent_count);
            summary.mean_abs_delta_mu = sum_abs_delta_mu / n;
            summary.mean_abs_delta_upper = sum_abs_delta_upper / n;
            summary.mean_abs_delta_width = sum_abs_delta_width / n;
        }
        return summary;
    }
};

bool ParseTimestampMinute(const std::string& text, int64_t* out_bucket_id) {
    if (!out_bucket_id) return false;
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    if (std::sscanf(text.c_str(), "%d/%d/%d %d:%d", &year, &month, &day, &hour, &minute) != 5) {
        return false;
    }

    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = 0;
    tm.tm_isdst = 0;
    const std::time_t utc_if_local_was_utc = timegm(&tm);
    if (utc_if_local_was_utc == static_cast<std::time_t>(-1)) return false;
    *out_bucket_id =
        (static_cast<int64_t>(utc_if_local_was_utc) - kShanghaiUtcOffsetSeconds) /
        kBucketSeconds;
    return true;
}

bool LoadCsv(const std::string& path, std::vector<LinkPoint>* out_points) {
    if (!out_points) return false;
    out_points->clear();

    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "failed to open csv: " << path << "\n";
        return false;
    }

    std::string line;
    if (!std::getline(file, line)) return false;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        const std::size_t comma = line.find(',');
        if (comma == std::string::npos) {
            std::cerr << "invalid csv line: " << line << "\n";
            return false;
        }
        LinkPoint point;
        point.timestamp = line.substr(0, comma);
        if (!ParseTimestampMinute(point.timestamp, &point.bucket_id)) {
            std::cerr << "invalid timestamp: " << point.timestamp << "\n";
            return false;
        }
        point.mbps = std::strtod(line.c_str() + comma + 1, nullptr);
        if (!std::isfinite(point.mbps)) {
            std::cerr << "invalid Mbps value: " << line << "\n";
            return false;
        }
        out_points->push_back(std::move(point));
    }
    return !out_points->empty();
}

bool WriteTextFile(const std::filesystem::path& path, const std::string& content) {
    std::ofstream file(path);
    if (!file.is_open()) {
        std::cerr << "failed to open output: " << path << "\n";
        return false;
    }
    file << content;
    return file.good();
}

std::string BuildRuntimeConfigYaml(int32_t daily_harmonic_order,
                                   int32_t weekly_harmonic_order) {
    std::ostringstream out;
    out << "baseline:\n";
    out << "  parser:\n";
    out << "    tz_default: \"Asia/Shanghai\"\n";
    out << "  shared_profile_config:\n";
    out << "    daily_harmonic_order: " << daily_harmonic_order << "\n";
    out << "    weekly_harmonic_order: " << weekly_harmonic_order << "\n";
    out << "    dme_max: 7\n";
    out << "    m_month_enable: 4\n";
    out << "    month_cov_min: 0.8\n";
    out << "    lambda_season: 1.0\n";
    out << "    lambda_dom: 4.0\n";
    out << "    lambda_dme: 2.0\n";
    out << "    lambda_lwd: 1.0\n";
    out << "    lambda_event: 2.0\n";
    out << "  rolling_config:\n";
    out << "    band_z: " << kRollingBandZ << "\n";
    out << "  value_sampled_profiles:\n";
    out << "    cont_core:\n";
    out << "      n_train_min: 50\n";
    out << "      transform_name_override: \"log1p\"\n";
    out << "  ratio_profiles:\n";
    out << "    global:\n";
    out << "      eps_logit: 1.0e-4\n";
    out << "      m_floor: 1.0e-4\n";
    out << "      v_floor: 0.25\n";
    out << "    rate_core:\n";
    out << "      d_min_train: 50\n";
    out << "      s_prior: 2.0\n";
    out << "      phi_over: 1.5\n";
    out << "  solver_constants:\n";
    out << "    solver_name: \"weighted_huber_ridge_irls\"\n";
    out << "    c_huber: 1.5\n";
    out << "    s_min_fit: 1.0e-3\n";
    out << "    max_iter_fit: 15\n";
    out << "    tol_obj_rel: 1.0e-4\n";
    out << "    tol_beta_inf: 1.0e-5\n";
    out << "    cond_max: 1.0e8\n";
    return out.str();
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

std::string ValueTaskConfig() {
    return R"({
        "schema_version": 1,
        "task_id": "link_bps_rolling_eval",
        "task_name": "link bps rolling eval",
        "task_kind": "value",
        "feature_id": "link_bps_mbps",
        "feature_type": "value_basic",
        "profile": "default",
        "clock_spec": {
            "bucket_seconds": 60,
            "timezone": "Asia/Shanghai"
        },
        "calendar_ref": {
            "calendar_id": "cn-holiday",
            "calendar_version": "2026.1"
        }
    })";
}

std::size_t FindIndexGE(const std::vector<LinkPoint>& points, int64_t bucket_id) {
    for (std::size_t i = 0; i < points.size(); ++i) {
        if (points[i].bucket_id >= bucket_id) return i;
    }
    return points.size();
}

ValueRollingObservation ToObservation(const LinkPoint& point) {
    ValueRollingObservation obs;
    obs.series_key = kSeriesKey;
    obs.bucket_id = point.bucket_id;
    obs.value = point.mbps;
    return obs;
}

void TrainRollingRange(IBaselineValueTask* task,
                       const std::vector<LinkPoint>& points,
                       std::size_t begin,
                       std::size_t end) {
    for (std::size_t i = begin; i < end; ++i) {
        const RollingBaselineResult result =
            task->SubmitObservation(ToObservation(points[i]), RollingSubmitOptions{}).result;
        if (result.status != BaselineStatus::kOk) {
            std::cerr << "rolling train failed at " << points[i].timestamp
                      << ", status=" << static_cast<int>(result.status) << "\n";
            std::abort();
        }
    }
}

void AppendMetricsJson(std::ostringstream* out,
                       const EvalSummary& summary,
                       const std::string& indent) {
    *out << indent << "\"count\": " << summary.count << ",\n";
    *out << indent << "\"inside_band_count\": " << summary.inside_band_count << ",\n";
    *out << indent << "\"abs_z_gt_3_count\": " << summary.abs_z_gt_3_count << ",\n";
    *out << indent << "\"abs_z_gt_5_count\": " << summary.abs_z_gt_5_count << ",\n";
    *out << indent << "\"coverage_ratio\": " << std::fixed << std::setprecision(6)
         << summary.coverage_ratio << ",\n";
    *out << indent << "\"mean_abs_z\": " << summary.mean_abs_z << ",\n";
    *out << indent << "\"max_abs_z\": " << summary.max_abs_z << ",\n";
    *out << indent << "\"rmse_mbps\": " << summary.rmse << ",\n";
    *out << indent << "\"mape\": " << summary.mape << ",\n";
    *out << indent << "\"mean_band_width_mbps\": " << summary.mean_band_width << "\n";
}

void AppendSmoothnessJson(std::ostringstream* out,
                          const SmoothnessSummary& summary,
                          const std::string& indent) {
    *out << indent << "\"adjacent_count\": " << summary.adjacent_count << ",\n";
    *out << indent << "\"mean_abs_delta_mu_mbps\": " << std::fixed << std::setprecision(6)
         << summary.mean_abs_delta_mu << ",\n";
    *out << indent << "\"mean_abs_delta_upper_mbps\": " << summary.mean_abs_delta_upper << ",\n";
    *out << indent << "\"mean_abs_delta_width_mbps\": " << summary.mean_abs_delta_width << "\n";
}

void AppendB3StatusJson(std::ostringstream* out,
                        const B3StatusSummary& summary,
                        const std::string& indent) {
    *out << indent << "\"count\": " << summary.count << ",\n";
    *out << indent << "\"can_score_count\": " << summary.can_score_count << ",\n";
    *out << indent << "\"can_alert_count\": " << summary.can_alert_count << ",\n";
    *out << indent << "\"score_untrusted_count\": " << summary.score_untrusted_count << ",\n";
    *out << indent << "\"score_warming_count\": " << summary.score_warming_count << ",\n";
    *out << indent << "\"score_ready_count\": " << summary.score_ready_count << ",\n";
    *out << indent << "\"drift_learning_count\": " << summary.drift_learning_count << ",\n";
    *out << indent << "\"recalibrating_count\": " << summary.recalibrating_count << ",\n";
    *out << indent << "\"daily_ready_or_above_count\": "
         << summary.daily_ready_or_above_count << ",\n";
    *out << indent << "\"weekly_ready_or_above_count\": "
         << summary.weekly_ready_or_above_count << ",\n";
    *out << indent << "\"calibration_warming_count\": "
         << summary.calibration_warming_count << ",\n";
    *out << indent << "\"calibration_calibrated_count\": "
         << summary.calibration_calibrated_count << ",\n";
    *out << indent << "\"calibration_expanding_count\": "
         << summary.calibration_expanding_count << ",\n";
    *out << indent << "\"calibration_recalibrating_count\": "
         << summary.calibration_recalibrating_count << "\n";
}

void AppendObservabilityJson(std::ostringstream* out,
                             const RollingObservabilitySummary& summary,
                             const std::string& indent) {
    const double n = summary.diagnostics_count > 0
                         ? static_cast<double>(summary.diagnostics_count)
                         : 1.0;
    *out << indent << "\"count\": " << summary.count << ",\n";
    *out << indent << "\"diagnostics_count\": " << summary.diagnostics_count << ",\n";
    *out << indent << "\"mean_std_log\": " << std::fixed << std::setprecision(6)
         << summary.sum_std_log / n << ",\n";
    *out << indent << "\"mean_raw_std_log\": " << summary.sum_raw_std_log / n << ",\n";
    *out << indent << "\"mean_pred_var\": " << summary.sum_pred_var / n << ",\n";
    *out << indent << "\"mean_calibrated_sigma_var\": "
         << summary.sum_calibrated_sigma_var / n << ",\n";
    *out << indent << "\"mean_extra_obs_var\": " << summary.sum_extra_obs_var / n << ",\n";
    *out << indent << "\"mean_maturity_var\": " << summary.sum_maturity_var / n << ",\n";
    *out << indent << "\"mean_missing_component_var\": "
         << summary.sum_missing_component_var / n << ",\n";
    *out << indent << "\"mean_band_multiplier\": " << summary.sum_multiplier / n << ",\n";
    *out << indent << "\"cap_applied_count\": " << summary.cap_applied_count << ",\n";
    *out << indent << "\"cap_applied_ratio\": "
         << (summary.diagnostics_count > 0
                 ? static_cast<double>(summary.cap_applied_count) /
                       static_cast<double>(summary.diagnostics_count)
                 : 0.0)
         << ",\n";
    *out << indent << "\"mean_abs_level_shift_evidence\": "
         << summary.sum_abs_level_shift_evidence / n << ",\n";
    *out << indent << "\"max_abs_level_shift_evidence\": "
         << summary.max_abs_level_shift_evidence << ",\n";
    *out << indent << "\"max_combined_drift_evidence\": "
         << summary.max_combined_drift_evidence << "\n";
}

bool WriteWeek4ComparisonAndTrainRolling(const std::filesystem::path& path,
                                         IBaselineValueTask* task,
                                         const std::vector<LinkPoint>& points,
                                         std::size_t begin,
                                         std::size_t end,
                                         EvalSummary* bootstrap_summary,
                                         EvalSummary* rolling_summary,
                                         B3StatusSummary* rolling_b3_summary,
                                         RollingObservabilitySummary* rolling_observability) {
    std::ofstream file(path);
    if (!file.is_open()) return false;
    file << "timestamp,bucket_id,actual_mbps,bootstrap_mu,bootstrap_lower,"
            "bootstrap_upper,bootstrap_abs_z,rolling_mu,rolling_lower,rolling_upper,"
            "rolling_abs_z,maturity_status,score_trust_status,calibration_status,"
            "learning_confidence,score_confidence,effective_confidence,can_alert,"
            "rolling_diagnostics\n";

    BootstrapPredictionOptions bootstrap_options;
    bootstrap_options.confidence_level = kConfidenceLevel;
    EvalAccumulator bootstrap_acc;
    EvalAccumulator rolling_acc;

    const int64_t start_bucket_id = points[begin].bucket_id;
    const int64_t end_bucket_id = points[end - 1].bucket_id;
    if (end_bucket_id < start_bucket_id) {
        std::cerr << "invalid week4 train comparison bucket range\n";
        return false;
    }
    const uint64_t sequence_bucket_count =
        static_cast<uint64_t>(end_bucket_id - start_bucket_id + 1);
    if (sequence_bucket_count > std::numeric_limits<uint32_t>::max()) {
        std::cerr << "week4 train comparison bucket range is too large for sequence API\n";
        return false;
    }
    const BootstrapPredictionSequence bootstrap_sequence =
        task->PredictBootstrap(kSeriesKey,
                               start_bucket_id,
                               static_cast<uint32_t>(sequence_bucket_count),
                               bootstrap_options);
    if (bootstrap_sequence.status != BaselineStatus::kOk ||
        bootstrap_sequence.predictions.size() != sequence_bucket_count) {
        std::cerr << "week4 train comparison bootstrap batch predict failed, status="
                  << static_cast<int>(bootstrap_sequence.status) << "\n";
        return false;
    }

    for (std::size_t i = begin; i < end; ++i) {
        const LinkPoint& point = points[i];
        if (point.bucket_id < start_bucket_id) {
            std::cerr << "week4 train comparison point is before batch start: "
                      << point.bucket_id << "\n";
            return false;
        }
        const uint64_t prediction_offset =
            static_cast<uint64_t>(point.bucket_id - start_bucket_id);
        if (prediction_offset >= bootstrap_sequence.predictions.size()) {
            std::cerr << "week4 train comparison point is outside batch range: "
                      << point.bucket_id << "\n";
            return false;
        }
        const BootstrapPrediction& bp =
            bootstrap_sequence.predictions[static_cast<std::size_t>(prediction_offset)];
        if (bp.bucket_id != point.bucket_id) {
            std::cerr << "week4 train comparison bootstrap batch bucket mismatch at "
                      << point.timestamp << "\n";
            return false;
        }
        const RollingBaselineResult rr = task->SubmitObservation(ToObservation(point), RollingSubmitOptions{}).result;
        if (bp.status != BaselineStatus::kOk || rr.status != BaselineStatus::kOk) {
            std::cerr << "week4 prediction failed at " << point.timestamp << "\n";
            return false;
        }
        bootstrap_acc.Add(point.mbps,
                          bp.baseline_mu,
                          bp.baseline_lower,
                          bp.baseline_upper,
                          kBootstrapBandZ);
        rolling_acc.Add(point.mbps,
                        rr.baseline_mu,
                        rr.baseline_lower,
                        rr.baseline_upper,
                        kRollingBandZ);
        if (rolling_b3_summary) rolling_b3_summary->Add(rr);
        if (rolling_observability) rolling_observability->Add(rr);
        const double bz = EvalAccumulator::DirectionalZ(point.mbps,
                                                        bp.baseline_mu,
                                                        bp.baseline_lower,
                                                        bp.baseline_upper,
                                                        kBootstrapBandZ);
        const double rz = EvalAccumulator::DirectionalZ(point.mbps,
                                                        rr.baseline_mu,
                                                        rr.baseline_lower,
                                                        rr.baseline_upper,
                                                        kRollingBandZ);
        file << point.timestamp << ',' << point.bucket_id << ',' << std::fixed
             << std::setprecision(6) << point.mbps << ',' << bp.baseline_mu << ','
             << bp.baseline_lower << ',' << bp.baseline_upper << ',' << std::fabs(bz) << ','
             << rr.baseline_mu << ',' << rr.baseline_lower << ',' << rr.baseline_upper << ','
             << std::fabs(rz) << ',' << rr.maturity_status << ',' << rr.score_trust_status
             << ',' << rr.calibration_status << ',' << rr.learning_confidence << ','
             << rr.score_confidence << ',' << rr.effective_confidence << ','
             << (rr.can_alert ? 1 : 0) << ',' << rr.diagnostics << '\n';
    }
    *bootstrap_summary = bootstrap_acc.Finish();
    *rolling_summary = rolling_acc.Finish();
    return file.good();
}

bool WriteWeek4FrozenPredictComparison(const std::filesystem::path& path,
                                       IBaselineValueTask* task,
                                       const std::vector<LinkPoint>& points,
                                       std::size_t begin,
                                       std::size_t end,
                                       EvalSummary* bootstrap_summary,
                                       EvalSummary* rolling_forecast_summary,
                                       SmoothnessSummary* bootstrap_smoothness,
                                       SmoothnessSummary* rolling_forecast_smoothness) {
    std::ofstream file(path);
    if (!file.is_open()) return false;
    file << "timestamp,bucket_id,actual_mbps,bootstrap_mu,bootstrap_lower,"
            "bootstrap_upper,bootstrap_abs_z,rolling_frozen_mu,"
            "rolling_frozen_lower,rolling_frozen_upper,rolling_frozen_abs_z\n";

    BootstrapPredictionOptions bootstrap_options;
    bootstrap_options.confidence_level = kConfidenceLevel;
    EvalAccumulator bootstrap_acc;
    EvalAccumulator rolling_acc;
    SmoothnessAccumulator bootstrap_smooth_acc;
    SmoothnessAccumulator rolling_smooth_acc;

    const int64_t start_bucket_id = points[begin].bucket_id;
    const int64_t end_bucket_id = points[end - 1].bucket_id;
    if (end_bucket_id < start_bucket_id) {
        std::cerr << "invalid frozen prediction bucket range\n";
        return false;
    }
    const uint64_t sequence_bucket_count =
        static_cast<uint64_t>(end_bucket_id - start_bucket_id + 1);
    if (sequence_bucket_count > std::numeric_limits<uint32_t>::max()) {
        std::cerr << "frozen prediction bucket range is too large for sequence API\n";
        return false;
    }
    const BootstrapPredictionSequence bootstrap_sequence =
        task->PredictBootstrap(kSeriesKey,
                               start_bucket_id,
                               static_cast<uint32_t>(sequence_bucket_count),
                               bootstrap_options);
    const RollingPredictionSequence rolling_sequence =
        task->PredictRolling(kSeriesKey,
                             start_bucket_id,
                             static_cast<uint32_t>(sequence_bucket_count));
    if (bootstrap_sequence.status != BaselineStatus::kOk ||
        bootstrap_sequence.predictions.size() != sequence_bucket_count ||
        rolling_sequence.status != BaselineStatus::kOk ||
        rolling_sequence.predictions.size() != sequence_bucket_count) {
        std::cerr << "week4 frozen batch predict failed"
                  << ", bootstrap_status=" << static_cast<int>(bootstrap_sequence.status)
                  << ", rolling_status=" << static_cast<int>(rolling_sequence.status) << "\n";
        return false;
    }

    for (std::size_t i = begin; i < end; ++i) {
        const LinkPoint& point = points[i];
        if (point.bucket_id < start_bucket_id) {
            std::cerr << "frozen prediction point is before batch start: " << point.bucket_id << "\n";
            return false;
        }
        const uint64_t prediction_offset =
            static_cast<uint64_t>(point.bucket_id - start_bucket_id);
        if (prediction_offset >= bootstrap_sequence.predictions.size() ||
            prediction_offset >= rolling_sequence.predictions.size()) {
            std::cerr << "frozen prediction point is outside batch range: " << point.bucket_id << "\n";
            return false;
        }
        const BootstrapPrediction& bp =
            bootstrap_sequence.predictions[static_cast<std::size_t>(prediction_offset)];
        const RollingPrediction& rp =
            rolling_sequence.predictions[static_cast<std::size_t>(prediction_offset)];
        if (bp.status != BaselineStatus::kOk || rp.status != BaselineStatus::kOk) {
            std::cerr << "week4 frozen predict failed at " << point.timestamp
                      << ", bootstrap_status=" << static_cast<int>(bp.status)
                      << ", rolling_status=" << static_cast<int>(rp.status) << "\n";
            return false;
        }
        if (bp.bucket_id != point.bucket_id || rp.bucket_id != point.bucket_id) {
            std::cerr << "week4 frozen batch prediction bucket mismatch at " << point.timestamp
                      << "\n";
            return false;
        }
        bootstrap_acc.Add(point.mbps,
                          bp.baseline_mu,
                          bp.baseline_lower,
                          bp.baseline_upper,
                          kBootstrapBandZ);
        rolling_acc.Add(point.mbps,
                        rp.baseline_mu,
                        rp.baseline_lower,
                        rp.baseline_upper,
                        rp.band_z);
        bootstrap_smooth_acc.Add(bp.baseline_mu, bp.baseline_lower, bp.baseline_upper);
        rolling_smooth_acc.Add(rp.baseline_mu, rp.baseline_lower, rp.baseline_upper);
        const double bz = EvalAccumulator::DirectionalZ(point.mbps,
                                                        bp.baseline_mu,
                                                        bp.baseline_lower,
                                                        bp.baseline_upper,
                                                        kBootstrapBandZ);
        const double rz = EvalAccumulator::DirectionalZ(point.mbps,
                                                        rp.baseline_mu,
                                                        rp.baseline_lower,
                                                        rp.baseline_upper,
                                                        rp.band_z);
        file << point.timestamp << ',' << point.bucket_id << ',' << std::fixed
             << std::setprecision(6) << point.mbps << ',' << bp.baseline_mu << ','
             << bp.baseline_lower << ',' << bp.baseline_upper << ',' << std::fabs(bz)
             << ',' << rp.baseline_mu << ',' << rp.baseline_lower << ','
             << rp.baseline_upper << ',' << std::fabs(rz) << '\n';
    }
    *bootstrap_summary = bootstrap_acc.Finish();
    *rolling_forecast_summary = rolling_acc.Finish();
    *bootstrap_smoothness = bootstrap_smooth_acc.Finish();
    *rolling_forecast_smoothness = rolling_smooth_acc.Finish();
    return file.good();
}

bool WriteWeek4RollingPredictAndTrainComparison(
    const std::filesystem::path& detection_path,
    const std::filesystem::path& forecast_path,
    IBaselineValueTask* task,
    const std::vector<LinkPoint>& points,
    std::size_t begin,
    std::size_t end,
    EvalSummary* bootstrap_summary,
    EvalSummary* rolling_summary,
    EvalSummary* forecast_bootstrap_summary,
    EvalSummary* rolling_forecast_summary,
    SmoothnessSummary* bootstrap_smoothness,
    SmoothnessSummary* rolling_forecast_smoothness,
    B3StatusSummary* rolling_b3_summary,
    RollingObservabilitySummary* rolling_observability) {
    std::ofstream detection_file(detection_path);
    std::ofstream forecast_file(forecast_path);
    if (!detection_file.is_open() || !forecast_file.is_open()) return false;

    detection_file << "timestamp,bucket_id,actual_mbps,bootstrap_mu,bootstrap_lower,"
                      "bootstrap_upper,bootstrap_abs_z,rolling_mu,rolling_lower,rolling_upper,"
                      "rolling_abs_z,maturity_status,score_trust_status,calibration_status,"
                      "learning_confidence,score_confidence,effective_confidence,can_alert,"
                      "rolling_diagnostics\n";
    forecast_file << "timestamp,bucket_id,actual_mbps,bootstrap_mu,bootstrap_lower,"
                     "bootstrap_upper,bootstrap_abs_z,rolling_predict_mu,"
                     "rolling_predict_lower,rolling_predict_upper,rolling_predict_abs_z\n";

    BootstrapPredictionOptions bootstrap_options;
    bootstrap_options.confidence_level = kConfidenceLevel;
    EvalAccumulator bootstrap_acc;
    EvalAccumulator rolling_acc;
    EvalAccumulator forecast_bootstrap_acc;
    EvalAccumulator rolling_forecast_acc;
    SmoothnessAccumulator bootstrap_smooth_acc;
    SmoothnessAccumulator rolling_smooth_acc;

    const int64_t start_bucket_id = points[begin].bucket_id;
    const int64_t end_bucket_id = points[end - 1].bucket_id;
    if (end_bucket_id < start_bucket_id) {
        std::cerr << "invalid week4 prediction bucket range\n";
        return false;
    }
    const uint64_t sequence_bucket_count =
        static_cast<uint64_t>(end_bucket_id - start_bucket_id + 1);
    if (sequence_bucket_count > std::numeric_limits<uint32_t>::max()) {
        std::cerr << "week4 prediction bucket range is too large for sequence API\n";
        return false;
    }
    const BootstrapPredictionSequence bootstrap_sequence =
        task->PredictBootstrap(kSeriesKey,
                               start_bucket_id,
                               static_cast<uint32_t>(sequence_bucket_count),
                               bootstrap_options);
    if (bootstrap_sequence.status != BaselineStatus::kOk ||
        bootstrap_sequence.predictions.size() != sequence_bucket_count) {
        std::cerr << "week4 bootstrap batch predict failed, status="
                  << static_cast<int>(bootstrap_sequence.status) << "\n";
        return false;
    }

    for (std::size_t i = begin; i < end; ++i) {
        const LinkPoint& point = points[i];
        if (point.bucket_id < start_bucket_id) {
            std::cerr << "week4 prediction point is before batch start: " << point.bucket_id << "\n";
            return false;
        }
        const uint64_t prediction_offset =
            static_cast<uint64_t>(point.bucket_id - start_bucket_id);
        if (prediction_offset >= bootstrap_sequence.predictions.size()) {
            std::cerr << "week4 prediction point is outside batch range: " << point.bucket_id << "\n";
            return false;
        }
        const BootstrapPrediction& bp =
            bootstrap_sequence.predictions[static_cast<std::size_t>(prediction_offset)];
        if (bp.bucket_id != point.bucket_id) {
            std::cerr << "week4 bootstrap batch prediction bucket mismatch at "
                      << point.timestamp << "\n";
            return false;
        }
        // 这里是 walk-forward 预测：每个点预测后会立即 SubmitObservation 更新状态。
        // 整段 rolling 批预测会冻结状态，改变该评估窗口的语义。
        const RollingPrediction rp = task->PredictRolling(kSeriesKey, point.bucket_id);
        if (bp.status != BaselineStatus::kOk || rp.status != BaselineStatus::kOk) {
            std::cerr << "week4 rolling predict failed at " << point.timestamp
                      << ", bootstrap_status=" << static_cast<int>(bp.status)
                      << ", rolling_status=" << static_cast<int>(rp.status) << "\n";
            return false;
        }

        forecast_bootstrap_acc.Add(point.mbps,
                                   bp.baseline_mu,
                                   bp.baseline_lower,
                                   bp.baseline_upper,
                                   kBootstrapBandZ);
        rolling_forecast_acc.Add(point.mbps,
                                 rp.baseline_mu,
                                 rp.baseline_lower,
                                 rp.baseline_upper,
                                 rp.band_z);
        bootstrap_smooth_acc.Add(bp.baseline_mu, bp.baseline_lower, bp.baseline_upper);
        rolling_smooth_acc.Add(rp.baseline_mu, rp.baseline_lower, rp.baseline_upper);
        const double forecast_bz = EvalAccumulator::DirectionalZ(point.mbps,
                                                                 bp.baseline_mu,
                                                                 bp.baseline_lower,
                                                                 bp.baseline_upper,
                                                                 kBootstrapBandZ);
        const double forecast_rz = EvalAccumulator::DirectionalZ(point.mbps,
                                                                 rp.baseline_mu,
                                                                 rp.baseline_lower,
                                                                 rp.baseline_upper,
                                                                 rp.band_z);
        forecast_file << point.timestamp << ',' << point.bucket_id << ',' << std::fixed
                      << std::setprecision(6) << point.mbps << ',' << bp.baseline_mu << ','
                      << bp.baseline_lower << ',' << bp.baseline_upper << ','
                      << std::fabs(forecast_bz) << ',' << rp.baseline_mu << ','
                      << rp.baseline_lower << ',' << rp.baseline_upper << ','
                      << std::fabs(forecast_rz) << '\n';

        const RollingBaselineResult rr = task->SubmitObservation(ToObservation(point), RollingSubmitOptions{}).result;
        if (rr.status != BaselineStatus::kOk) {
            std::cerr << "week4 rolling update failed at " << point.timestamp
                      << ", status=" << static_cast<int>(rr.status) << "\n";
            return false;
        }
        bootstrap_acc.Add(point.mbps,
                          bp.baseline_mu,
                          bp.baseline_lower,
                          bp.baseline_upper,
                          kBootstrapBandZ);
        rolling_acc.Add(point.mbps,
                        rr.baseline_mu,
                        rr.baseline_lower,
                        rr.baseline_upper,
                        kRollingBandZ);
        if (rolling_b3_summary) rolling_b3_summary->Add(rr);
        if (rolling_observability) rolling_observability->Add(rr);
        const double detection_bz = EvalAccumulator::DirectionalZ(point.mbps,
                                                                  bp.baseline_mu,
                                                                  bp.baseline_lower,
                                                                  bp.baseline_upper,
                                                                  kBootstrapBandZ);
        const double detection_rz = EvalAccumulator::DirectionalZ(point.mbps,
                                                                  rr.baseline_mu,
                                                                  rr.baseline_lower,
                                                                  rr.baseline_upper,
                                                                  kRollingBandZ);
        detection_file << point.timestamp << ',' << point.bucket_id << ',' << std::fixed
                       << std::setprecision(6) << point.mbps << ',' << bp.baseline_mu << ','
                       << bp.baseline_lower << ',' << bp.baseline_upper << ','
                       << std::fabs(detection_bz) << ',' << rr.baseline_mu << ','
                       << rr.baseline_lower << ',' << rr.baseline_upper << ','
                       << std::fabs(detection_rz) << ',' << rr.maturity_status << ','
                       << rr.score_trust_status << ',' << rr.calibration_status << ','
                       << rr.learning_confidence << ',' << rr.score_confidence << ','
                       << rr.effective_confidence << ',' << (rr.can_alert ? 1 : 0)
                       << ',' << rr.diagnostics << '\n';
    }
    *bootstrap_summary = bootstrap_acc.Finish();
    *rolling_summary = rolling_acc.Finish();
    *forecast_bootstrap_summary = forecast_bootstrap_acc.Finish();
    *rolling_forecast_summary = rolling_forecast_acc.Finish();
    *bootstrap_smoothness = bootstrap_smooth_acc.Finish();
    *rolling_forecast_smoothness = rolling_smooth_acc.Finish();
    return detection_file.good() && forecast_file.good();
}

bool WriteTransitionLearning(const std::filesystem::path& path,
                             IBaselineValueTask* task,
                             const std::vector<LinkPoint>& points,
                             std::size_t begin,
                             std::size_t end,
                             EvalSummary* summary,
                             std::vector<TransitionCheckpoint>* checkpoints,
                             B3StatusSummary* b3_summary,
                             RollingObservabilitySummary* observability) {
    std::ofstream file(path);
    if (!file.is_open()) return false;
    file << "timestamp,bucket_id,minute_offset,actual_mbps,baseline_mu,baseline_lower,"
            "baseline_upper,z_score,abs_z,drift_evidence,adapt_boost,update_weight,"
            "maturity_status,score_trust_status,calibration_status,learning_confidence,"
            "score_confidence,effective_confidence,can_alert\n";

    EvalAccumulator acc;
    for (std::size_t i = begin; i < end; ++i) {
        const LinkPoint& point = points[i];
        const RollingBaselineResult result =
            task->SubmitObservation(ToObservation(point), RollingSubmitOptions{}).result;
        if (result.status != BaselineStatus::kOk) {
            std::cerr << "transition update failed at " << point.timestamp
                      << ", status=" << static_cast<int>(result.status) << "\n";
            return false;
        }
        acc.Add(point.mbps,
                result.baseline_mu,
                result.baseline_lower,
                result.baseline_upper,
                3.0);
        if (b3_summary) b3_summary->Add(result);
        if (observability) observability->Add(result);
        const int64_t minute_offset = point.bucket_id - points[begin].bucket_id;
        if (checkpoints &&
            (minute_offset == 0 || minute_offset == 60 || minute_offset == 240 ||
             minute_offset == 720 || minute_offset == 1439)) {
            TransitionCheckpoint checkpoint;
            checkpoint.minute_offset = minute_offset;
            checkpoint.timestamp = point.timestamp;
            checkpoint.actual = point.mbps;
            checkpoint.baseline_mu = result.baseline_mu;
            checkpoint.abs_z = std::fabs(result.z_score);
            checkpoint.drift_evidence = result.drift_evidence;
            checkpoint.adapt_boost = result.adapt_boost;
            checkpoint.update_weight = result.update_weight;
            checkpoint.maturity_status = result.maturity_status;
            checkpoint.score_trust_status = result.score_trust_status;
            checkpoint.calibration_status = result.calibration_status;
            checkpoint.can_alert = result.can_alert;
            checkpoints->push_back(std::move(checkpoint));
        }
        file << point.timestamp << ',' << point.bucket_id << ',' << minute_offset << ','
             << std::fixed << std::setprecision(6) << point.mbps << ','
             << result.baseline_mu << ',' << result.baseline_lower << ','
             << result.baseline_upper << ',' << result.z_score << ','
             << std::fabs(result.z_score) << ',' << result.drift_evidence << ','
             << result.adapt_boost << ',' << result.update_weight << ','
             << result.maturity_status << ',' << result.score_trust_status << ','
             << result.calibration_status << ',' << result.learning_confidence << ','
             << result.score_confidence << ',' << result.effective_confidence << ','
             << (result.can_alert ? 1 : 0) << '\n';
    }
    *summary = acc.Finish();
    return file.good();
}

bool WriteRollingWalkForwardDetection(const std::filesystem::path& path,
                                      IBaselineValueTask* task,
                                      const std::vector<LinkPoint>& points,
                                      std::size_t begin,
                                      std::size_t end,
                                      EvalSummary* summary,
                                      B3StatusSummary* b3_summary,
                                      RollingObservabilitySummary* observability) {
    std::ofstream file(path);
    if (!file.is_open()) return false;
    file << "timestamp,bucket_id,actual_mbps,baseline_mu,baseline_lower,"
            "baseline_upper,band_width,z_score,abs_z,in_band,maturity_status,"
            "score_trust_status,calibration_status,learning_confidence,score_confidence,"
            "effective_confidence,can_alert,rolling_diagnostics\n";

    EvalAccumulator acc;
    for (std::size_t i = begin; i < end; ++i) {
        const LinkPoint& point = points[i];
        const RollingBaselineResult result =
            task->SubmitObservation(ToObservation(point), RollingSubmitOptions{}).result;
        if (result.status != BaselineStatus::kOk) {
            std::cerr << "rolling walk-forward failed at " << point.timestamp
                      << ", status=" << static_cast<int>(result.status) << "\n";
            return false;
        }
        acc.Add(point.mbps,
                result.baseline_mu,
                result.baseline_lower,
                result.baseline_upper,
                kRollingBandZ);
        if (b3_summary) b3_summary->Add(result);
        if (observability) observability->Add(result);
        const bool in_band = point.mbps >= result.baseline_lower && point.mbps <= result.baseline_upper;
        file << point.timestamp << ',' << point.bucket_id << ',' << std::fixed
             << std::setprecision(6) << point.mbps << ',' << result.baseline_mu << ','
             << result.baseline_lower << ',' << result.baseline_upper << ','
             << (result.baseline_upper - result.baseline_lower) << ',' << result.z_score << ','
             << std::fabs(result.z_score) << ',' << (in_band ? 1 : 0) << ','
             << result.maturity_status << ',' << result.score_trust_status << ','
             << result.calibration_status << ',' << result.learning_confidence << ','
             << result.score_confidence << ',' << result.effective_confidence << ','
             << (result.can_alert ? 1 : 0) << ',' << result.diagnostics << '\n';
    }
    *summary = acc.Finish();
    return file.good();
}

bool WritePostShiftTraining(const std::filesystem::path& path,
                            IBaselineValueTask* task,
                            const std::vector<LinkPoint>& points,
                            std::size_t begin,
                            std::size_t end,
                            EvalSummary* summary,
                            B3StatusSummary* b3_summary,
                            RollingObservabilitySummary* observability) {
    std::ofstream file(path);
    if (!file.is_open()) return false;
    file << "timestamp,bucket_id,actual_mbps,baseline_mu,baseline_lower,"
            "baseline_upper,z_score,abs_z,drift_evidence,adapt_boost,update_weight,"
            "maturity_status,score_trust_status,calibration_status,learning_confidence,"
            "score_confidence,effective_confidence,can_alert\n";

    EvalAccumulator acc;
    for (std::size_t i = begin; i < end; ++i) {
        const LinkPoint& point = points[i];
        const RollingBaselineResult result =
            task->SubmitObservation(ToObservation(point), RollingSubmitOptions{}).result;
        if (result.status != BaselineStatus::kOk) {
            std::cerr << "post-shift rolling train failed at " << point.timestamp
                      << ", status=" << static_cast<int>(result.status) << "\n";
            return false;
        }
        acc.Add(point.mbps,
                result.baseline_mu,
                result.baseline_lower,
                result.baseline_upper,
                kRollingBandZ);
        if (b3_summary) b3_summary->Add(result);
        if (observability) observability->Add(result);
        file << point.timestamp << ',' << point.bucket_id << ',' << std::fixed
             << std::setprecision(6) << point.mbps << ',' << result.baseline_mu << ','
             << result.baseline_lower << ',' << result.baseline_upper << ','
             << result.z_score << ',' << std::fabs(result.z_score) << ','
             << result.drift_evidence << ',' << result.adapt_boost << ','
             << result.update_weight << ',' << result.maturity_status << ','
             << result.score_trust_status << ',' << result.calibration_status << ','
             << result.learning_confidence << ',' << result.score_confidence << ','
             << result.effective_confidence << ',' << (result.can_alert ? 1 : 0) << '\n';
    }
    *summary = acc.Finish();
    return file.good();
}

std::string SummaryJson(const EvalSummary& week4_bootstrap,
                        const EvalSummary& week4_rolling,
                        const EvalSummary& week4_frozen_bootstrap,
                        const EvalSummary& week4_frozen_forecast,
                        const EvalSummary& week4_forecast_bootstrap,
                        const EvalSummary& week4_rolling_forecast,
                        const EvalSummary& transition,
                        const EvalSummary& post_shift_training,
                        const EvalSummary& final7,
                        const SmoothnessSummary& week4_frozen_bootstrap_smoothness,
                        const SmoothnessSummary& week4_frozen_forecast_smoothness,
                        const SmoothnessSummary& week4_forecast_bootstrap_smoothness,
                        const SmoothnessSummary& week4_rolling_forecast_smoothness,
                        const B3StatusSummary& week4_b3,
                        const B3StatusSummary& transition_b3,
                        const B3StatusSummary& post_shift_b3,
                        const RollingObservabilitySummary& week4_observability,
                        const RollingObservabilitySummary& transition_observability,
                        const RollingObservabilitySummary& post_shift_observability,
                        const RollingObservabilitySummary& final7_observability,
                        const std::vector<LinkPoint>& points,
                        std::size_t bootstrap_begin,
                        std::size_t bootstrap_end,
                        std::size_t week3_begin,
                        std::size_t week3_end,
                        std::size_t week4_begin,
                        std::size_t week4_end,
                        std::size_t transition_begin,
                        std::size_t transition_end,
                        std::size_t post_train_begin,
                        std::size_t post_train_end,
                        std::size_t final_begin,
                        std::size_t final_end,
                        const std::vector<TransitionCheckpoint>& checkpoints,
                        const std::filesystem::path& week4_csv,
                        const std::filesystem::path& week4_frozen_csv,
                        const std::filesystem::path& week4_forecast_csv,
                        const std::filesystem::path& transition_csv,
                        const std::filesystem::path& post_shift_csv,
                        const std::filesystem::path& final_csv,
                        const std::filesystem::path& series_snapshot_json) {
    auto range = [&](std::size_t begin, std::size_t end) {
        std::ostringstream out;
        out << "\"start_time\": \"" << points[begin].timestamp << "\", ";
        out << "\"end_time\": \"" << points[end - 1].timestamp << "\", ";
        out << "\"count\": " << (end - begin);
        return out.str();
    };

    std::ostringstream out;
    out << "{\n";
    out << "  \"schema_version\": 1,\n";
    out << "  \"document_kind\": \"baseline_rolling_eval_summary\",\n";
    out << "  \"csv\": \"src/tests/data/csv/link_data_2_month.csv\",\n";
    out << "  \"feature\": \"link_bps_mbps\",\n";
    out << "  \"unit\": \"Mbps\",\n";
    out << "  \"bucket_seconds\": 60,\n";
    out << "  \"timezone\": \"Asia/Shanghai\",\n";
    out << "  \"windows\": {\n";
    out << "    \"bootstrap_train\": {" << range(bootstrap_begin, bootstrap_end) << "},\n";
    out << "    \"rolling_train_week3\": {" << range(week3_begin, week3_end) << "},\n";
    out << "    \"week4_compare_walk_forward\": {"
        << range(week4_begin, week4_end) << "},\n";
    out << "    \"week4_compare_frozen_predict\": {"
        << range(week4_begin, week4_end) << "},\n";
    out << "    \"week4_compare_rolling_predict\": {"
        << range(week4_begin, week4_end) << "},\n";
    out << "    \"transition_learning\": {"
        << range(transition_begin, transition_end) << "},\n";
    out << "    \"post_shift_train_4d\": {"
        << range(post_train_begin, post_train_end) << "},\n";
    out << "    \"final7_walk_forward_detection\": {" << range(final_begin, final_end) << "}\n";
    out << "  },\n";
    out << "  \"metrics\": {\n";
    out << "    \"week4_bootstrap\": {\n";
    AppendMetricsJson(&out, week4_bootstrap, "      ");
    out << "    },\n";
    out << "    \"week4_rolling\": {\n";
    AppendMetricsJson(&out, week4_rolling, "      ");
    out << "    },\n";
    out << "    \"week4_frozen_forecast_bootstrap\": {\n";
    AppendMetricsJson(&out, week4_frozen_bootstrap, "      ");
    out << "    },\n";
    out << "    \"week4_rolling_frozen_forecast\": {\n";
    AppendMetricsJson(&out, week4_frozen_forecast, "      ");
    out << "    },\n";
    out << "    \"week4_forecast_bootstrap\": {\n";
    AppendMetricsJson(&out, week4_forecast_bootstrap, "      ");
    out << "    },\n";
    out << "    \"week4_rolling_predict_forecast\": {\n";
    AppendMetricsJson(&out, week4_rolling_forecast, "      ");
    out << "    },\n";
    out << "    \"transition_learning\": {\n";
    AppendMetricsJson(&out, transition, "      ");
    out << "    },\n";
    out << "    \"post_shift_training\": {\n";
    AppendMetricsJson(&out, post_shift_training, "      ");
    out << "    },\n";
    out << "    \"final7_rolling\": {\n";
    AppendMetricsJson(&out, final7, "      ");
    out << "    }\n";
    out << "  },\n";
    out << "  \"smoothness\": {\n";
    out << "    \"week4_frozen_forecast_bootstrap\": {\n";
    AppendSmoothnessJson(&out, week4_frozen_bootstrap_smoothness, "      ");
    out << "    },\n";
    out << "    \"week4_rolling_frozen_forecast\": {\n";
    AppendSmoothnessJson(&out, week4_frozen_forecast_smoothness, "      ");
    out << "    },\n";
    out << "    \"week4_forecast_bootstrap\": {\n";
    AppendSmoothnessJson(&out, week4_forecast_bootstrap_smoothness, "      ");
    out << "    },\n";
    out << "    \"week4_rolling_predict_forecast\": {\n";
    AppendSmoothnessJson(&out, week4_rolling_forecast_smoothness, "      ");
    out << "    }\n";
    out << "  },\n";
    out << "  \"b3_status\": {\n";
    out << "    \"week4_walk_forward\": {\n";
    AppendB3StatusJson(&out, week4_b3, "      ");
    out << "    },\n";
    out << "    \"transition_learning\": {\n";
    AppendB3StatusJson(&out, transition_b3, "      ");
    out << "    },\n";
    out << "    \"post_shift_training\": {\n";
    AppendB3StatusJson(&out, post_shift_b3, "      ");
    out << "    }\n";
    out << "  },\n";
    out << "  \"observability\": {\n";
    out << "    \"week4_walk_forward\": {\n";
    AppendObservabilityJson(&out, week4_observability, "      ");
    out << "    },\n";
    out << "    \"transition_learning\": {\n";
    AppendObservabilityJson(&out, transition_observability, "      ");
    out << "    },\n";
    out << "    \"post_shift_training\": {\n";
    AppendObservabilityJson(&out, post_shift_observability, "      ");
    out << "    },\n";
    out << "    \"final7_walk_forward_detection\": {\n";
    AppendObservabilityJson(&out, final7_observability, "      ");
    out << "    }\n";
    out << "  },\n";
    out << "  \"transition_checkpoints\": [\n";
    for (std::size_t i = 0; i < checkpoints.size(); ++i) {
        const TransitionCheckpoint& item = checkpoints[i];
        out << "    {\n";
        out << "      \"minute_offset\": " << item.minute_offset << ",\n";
        out << "      \"timestamp\": \"" << item.timestamp << "\",\n";
        out << "      \"actual_mbps\": " << item.actual << ",\n";
        out << "      \"baseline_mu_mbps\": " << item.baseline_mu << ",\n";
        out << "      \"abs_z\": " << item.abs_z << ",\n";
        out << "      \"drift_evidence\": " << item.drift_evidence << ",\n";
        out << "      \"adapt_boost\": " << item.adapt_boost << ",\n";
        out << "      \"update_weight\": " << item.update_weight << ",\n";
        out << "      \"maturity_status\": \"" << item.maturity_status << "\",\n";
        out << "      \"score_trust_status\": \"" << item.score_trust_status << "\",\n";
        out << "      \"calibration_status\": \"" << item.calibration_status << "\",\n";
        out << "      \"can_alert\": " << (item.can_alert ? "true" : "false") << "\n";
        out << "    }" << (i + 1 < checkpoints.size() ? "," : "") << "\n";
    }
    out << "  ],\n";
    out << "  \"outputs\": {\n";
    out << "    \"week4_comparison_csv\": \"" << week4_csv.string() << "\",\n";
    out << "    \"week4_frozen_predict_csv\": \"" << week4_frozen_csv.string() << "\",\n";
    out << "    \"week4_rolling_predict_csv\": \"" << week4_forecast_csv.string() << "\",\n";
    out << "    \"transition_learning_csv\": \"" << transition_csv.string() << "\",\n";
    out << "    \"post_shift_training_csv\": \"" << post_shift_csv.string() << "\",\n";
    out << "    \"final7_rolling_csv\": \"" << final_csv.string() << "\",\n";
    out << "    \"series_snapshot_json\": \"" << series_snapshot_json.string() << "\"\n";
    out << "  }\n";
    out << "}\n";
    return out.str();
}

}  // namespace

int main(int argc, char** argv) {
    const std::string csv_path =
        argc >= 2 ? argv[1] : "src/tests/data/csv/link_data_2_month.csv";
    const std::filesystem::path output_dir =
        argc >= 3 ? std::filesystem::path(argv[2])
                  : std::filesystem::path(get_absolute_process_path()) /
                        "baseline_link_rolling_eval";
    const int32_t daily_harmonic_order =
        argc >= 4 ? std::atoi(argv[3]) : kDefaultDailyHarmonicOrder;
    const int32_t weekly_harmonic_order =
        argc >= 5 ? std::atoi(argv[4]) : kDefaultWeeklyHarmonicOrder;

    std::filesystem::create_directories(output_dir);
    const std::filesystem::path config_path = output_dir / "runtime_config.yaml";
    if (!WriteTextFile(config_path,
                       BuildRuntimeConfigYaml(daily_harmonic_order,
                                              weekly_harmonic_order))) {
        return 1;
    }

    std::vector<LinkPoint> points;
    if (!LoadCsv(csv_path, &points)) return 1;

    const int64_t start_bucket = points.front().bucket_id;
    const int64_t bootstrap_end_bucket = start_bucket + 2 * kWeekMinutes;
    const int64_t week3_end_bucket = start_bucket + 3 * kWeekMinutes;
    const int64_t week4_end_bucket = start_bucket + 4 * kWeekMinutes;
    const int64_t transition_end_bucket = week4_end_bucket + kDayMinutes;
    const int64_t post_train_end_bucket = transition_end_bucket + 4 * kDayMinutes;
    const int64_t final_end_bucket = post_train_end_bucket + kWeekMinutes;

    const std::size_t bootstrap_begin = 0;
    const std::size_t bootstrap_end = FindIndexGE(points, bootstrap_end_bucket);
    const std::size_t week3_begin = bootstrap_end;
    const std::size_t week3_end = FindIndexGE(points, week3_end_bucket);
    const std::size_t week4_begin = week3_end;
    const std::size_t week4_end = FindIndexGE(points, week4_end_bucket);
    const std::size_t transition_begin = week4_end;
    const std::size_t transition_end = FindIndexGE(points, transition_end_bucket);
    const std::size_t post_train_begin = transition_end;
    const std::size_t post_train_end = FindIndexGE(points, post_train_end_bucket);
    const std::size_t final_begin = post_train_end;
    const std::size_t final_end = FindIndexGE(points, final_end_bucket);

    if (bootstrap_end == 0 || week3_end <= week3_begin || week4_end <= week4_begin ||
        transition_end <= transition_begin || post_train_end <= post_train_begin ||
        final_end <= final_begin || final_end > points.size()) {
        std::cerr << "not enough data for rolling eval windows\n";
        return 1;
    }

    LoadedBaselineService env = LoadBaselineService(config_path);
    auto [task_status, task] =
        env.service->CreateValueTask(ValueTaskConfig(), BaselineSerializationFormat::kJson);
    if (task_status != BaselineStatus::kOk || !task) {
        std::cerr << "create value task failed\n";
        return 1;
    }

    ValueBootstrapInput bootstrap_input;
    bootstrap_input.series_key = kSeriesKey;
    bootstrap_input.observations.reserve(bootstrap_end - bootstrap_begin);
    for (std::size_t i = bootstrap_begin; i < bootstrap_end; ++i) {
        bootstrap_input.observations.push_back(
            ValueBootstrapPoint{points[i].bucket_id, points[i].mbps, 1});
    }
    const BootstrapTrainResult train = task->Bootstrap(bootstrap_input);
    if (train.status != BaselineStatus::kOk) {
        std::cerr << "bootstrap train failed: " << train.diagnostics << "\n";
        return 1;
    }

    TrainRollingRange(task.get(), points, week3_begin, week3_end);

    EvalSummary week4_bootstrap;
    EvalSummary week4_rolling;
    EvalSummary week4_frozen_bootstrap;
    EvalSummary week4_frozen_forecast;
    EvalSummary week4_forecast_bootstrap;
    EvalSummary week4_rolling_forecast;
    EvalSummary transition;
    EvalSummary post_shift_training;
    EvalSummary final7;
    SmoothnessSummary week4_frozen_bootstrap_smoothness;
    SmoothnessSummary week4_frozen_forecast_smoothness;
    SmoothnessSummary week4_forecast_bootstrap_smoothness;
    SmoothnessSummary week4_rolling_forecast_smoothness;
    B3StatusSummary week4_b3;
    B3StatusSummary transition_b3;
    B3StatusSummary post_shift_b3;
    B3StatusSummary final7_b3;
    RollingObservabilitySummary week4_observability;
    RollingObservabilitySummary transition_observability;
    RollingObservabilitySummary post_shift_observability;
    RollingObservabilitySummary final7_observability;
    std::vector<TransitionCheckpoint> transition_checkpoints;
    const std::filesystem::path week4_csv = output_dir / "week4_comparison.csv";
    const std::filesystem::path week4_frozen_csv =
        output_dir / "week4_frozen_predict_comparison.csv";
    const std::filesystem::path week4_forecast_csv =
        output_dir / "week4_rolling_predict_comparison.csv";
    const std::filesystem::path transition_csv = output_dir / "transition_learning.csv";
    const std::filesystem::path post_shift_csv = output_dir / "post_shift_training.csv";
    const std::filesystem::path final_csv =
        output_dir / "final7_walk_forward_detection.csv";
    const std::filesystem::path series_snapshot_path = output_dir / "rolling_series_snapshot.json";
    const std::filesystem::path summary_path = output_dir / "rolling_eval_summary.json";

    if (!WriteWeek4FrozenPredictComparison(week4_frozen_csv,
                                           task.get(),
                                           points,
                                           week4_begin,
                                           week4_end,
                                           &week4_frozen_bootstrap,
                                           &week4_frozen_forecast,
                                           &week4_frozen_bootstrap_smoothness,
                                           &week4_frozen_forecast_smoothness)) {
        return 1;
    }

    if (!WriteWeek4RollingPredictAndTrainComparison(week4_csv,
                                                    week4_forecast_csv,
                                                    task.get(),
                                                    points,
                                                    week4_begin,
                                                    week4_end,
                                                    &week4_bootstrap,
                                                    &week4_rolling,
                                                    &week4_forecast_bootstrap,
                                                    &week4_rolling_forecast,
                                                    &week4_forecast_bootstrap_smoothness,
                                                    &week4_rolling_forecast_smoothness,
                                                    &week4_b3,
                                                    &week4_observability)) {
        return 1;
    }

    if (!WriteTransitionLearning(transition_csv,
                                 task.get(),
                                 points,
                                 transition_begin,
                                 transition_end,
                                 &transition,
                                 &transition_checkpoints,
                                 &transition_b3,
                                 &transition_observability)) {
        return 1;
    }
    if (!WritePostShiftTraining(post_shift_csv,
                                task.get(),
                                points,
                                post_train_begin,
                                post_train_end,
                                &post_shift_training,
                                &post_shift_b3,
                                &post_shift_observability)) {
        return 1;
    }
    if (!WriteRollingWalkForwardDetection(final_csv,
                                          task.get(),
                                          points,
                                          final_begin,
                                          final_end,
                                          &final7,
                                          &final7_b3,
                                          &final7_observability)) {
        return 1;
    }
    auto [snapshot_status, snapshot_json] =
        task->QuerySeriesSnapshot(kSeriesKey, BaselineSerializationFormat::kJson);
    if (snapshot_status != BaselineStatus::kOk ||
        !WriteTextFile(series_snapshot_path, snapshot_json)) {
        return 1;
    }

    assert(week4_bootstrap.count == week4_rolling.count);
    assert(week4_bootstrap.count == static_cast<uint64_t>(kWeekMinutes));
    assert(week4_frozen_bootstrap.count == week4_frozen_forecast.count);
    assert(week4_frozen_forecast.count == static_cast<uint64_t>(kWeekMinutes));
    assert(week4_frozen_forecast_smoothness.adjacent_count ==
           static_cast<uint64_t>(kWeekMinutes - 1));
    assert(week4_forecast_bootstrap.count == week4_rolling_forecast.count);
    assert(week4_rolling_forecast.count == static_cast<uint64_t>(kWeekMinutes));
    assert(week4_rolling_forecast_smoothness.adjacent_count ==
           static_cast<uint64_t>(kWeekMinutes - 1));
    assert(week4_rolling_forecast.rmse < week4_forecast_bootstrap.rmse);
    assert(week4_rolling_forecast.coverage_ratio >= 0.80);
    assert(transition.count == static_cast<uint64_t>(kDayMinutes));
    assert(transition_checkpoints.size() == 5);
    assert(post_shift_training.count == static_cast<uint64_t>(4 * kDayMinutes));
    assert(final7.count == static_cast<uint64_t>(kWeekMinutes));
    assert(week4_b3.count == week4_rolling.count);
    assert(transition_b3.count == transition.count);
    assert(post_shift_b3.count == post_shift_training.count);
    assert(final7_b3.count == final7.count);
    assert(week4_observability.diagnostics_count == week4_rolling.count);
    assert(transition_observability.diagnostics_count == transition.count);
    assert(post_shift_observability.diagnostics_count == post_shift_training.count);
    assert(final7_observability.diagnostics_count == final7.count);
    assert(final7.mean_band_width < 1.0e9);

    if (!WriteTextFile(summary_path,
                       SummaryJson(week4_bootstrap,
                                   week4_rolling,
                                   week4_frozen_bootstrap,
                                   week4_frozen_forecast,
                                   week4_forecast_bootstrap,
                                   week4_rolling_forecast,
                                   transition,
                                   post_shift_training,
                                   final7,
                                   week4_frozen_bootstrap_smoothness,
                                   week4_frozen_forecast_smoothness,
                                   week4_forecast_bootstrap_smoothness,
                                   week4_rolling_forecast_smoothness,
                                   week4_b3,
                                   transition_b3,
                                   post_shift_b3,
                                   week4_observability,
                                   transition_observability,
                                   post_shift_observability,
                                   final7_observability,
                                   points,
                                   bootstrap_begin,
                                   bootstrap_end,
                                   week3_begin,
                                   week3_end,
                                   week4_begin,
                                   week4_end,
                                   transition_begin,
                                   transition_end,
                                   post_train_begin,
                                   post_train_end,
                                   final_begin,
                                   final_end,
                                   transition_checkpoints,
                                   week4_csv,
                                   week4_frozen_csv,
                                   week4_forecast_csv,
                                   transition_csv,
                                   post_shift_csv,
                                   final_csv,
                                   series_snapshot_path))) {
        return 1;
    }

    std::cout << "summary_json=" << summary_path << "\n";
    std::cout << "week4_bootstrap_count=" << week4_bootstrap.count
              << ", rmse_mbps=" << std::fixed << std::setprecision(6)
              << week4_bootstrap.rmse
              << ", abs_z_gt_3=" << week4_bootstrap.abs_z_gt_3_count
              << ", abs_z_gt_5=" << week4_bootstrap.abs_z_gt_5_count << "\n";
    std::cout << "week4_rolling_count=" << week4_rolling.count
              << ", rmse_mbps=" << week4_rolling.rmse
              << ", abs_z_gt_3=" << week4_rolling.abs_z_gt_3_count
              << ", abs_z_gt_5=" << week4_rolling.abs_z_gt_5_count << "\n";
    std::cout << "week4_rolling_frozen_forecast_count=" << week4_frozen_forecast.count
              << ", rmse_mbps=" << week4_frozen_forecast.rmse
              << ", mean_band_width_mbps=" << week4_frozen_forecast.mean_band_width
              << ", smooth_upper_delta_mbps="
              << week4_frozen_forecast_smoothness.mean_abs_delta_upper << "\n";
    std::cout << "week4_rolling_predict_forecast_count=" << week4_rolling_forecast.count
              << ", rmse_mbps=" << week4_rolling_forecast.rmse
              << ", mean_band_width_mbps=" << week4_rolling_forecast.mean_band_width
              << ", smooth_upper_delta_mbps="
              << week4_rolling_forecast_smoothness.mean_abs_delta_upper << "\n";
    std::cout << "transition_count=" << transition.count
              << ", rmse_mbps=" << transition.rmse
              << ", abs_z_gt_3=" << transition.abs_z_gt_3_count
              << ", abs_z_gt_5=" << transition.abs_z_gt_5_count << "\n";
    std::cout << "post_shift_training_count=" << post_shift_training.count
              << ", rmse_mbps=" << post_shift_training.rmse
              << ", abs_z_gt_3=" << post_shift_training.abs_z_gt_3_count
              << ", abs_z_gt_5=" << post_shift_training.abs_z_gt_5_count << "\n";
    std::cout << "final7_rolling_count=" << final7.count
              << ", rmse_mbps=" << final7.rmse
              << ", abs_z_gt_3=" << final7.abs_z_gt_3_count
              << ", abs_z_gt_5=" << final7.abs_z_gt_5_count
              << ", mean_band_width_mbps=" << final7.mean_band_width << "\n";
    std::cout << "week4_b3_can_alert=" << week4_b3.can_alert_count
              << ", score_ready=" << week4_b3.score_ready_count
              << ", score_warming=" << week4_b3.score_warming_count << "\n";
    std::cout << "transition_b3_drift_learning=" << transition_b3.drift_learning_count
              << ", recalibrating=" << transition_b3.recalibrating_count
              << ", can_alert=" << transition_b3.can_alert_count << "\n";
    std::cout << "post_shift_b3_score_ready=" << post_shift_b3.score_ready_count
              << ", can_alert=" << post_shift_b3.can_alert_count << "\n";
    std::cout << "final7_b3_score_ready=" << final7_b3.score_ready_count
              << ", can_alert=" << final7_b3.can_alert_count << "\n";
    std::cout << "transition_observability_diagnostics="
              << transition_observability.diagnostics_count
              << ", max_level_shift_evidence="
              << transition_observability.max_abs_level_shift_evidence
              << ", max_combined_drift_evidence="
              << transition_observability.max_combined_drift_evidence << "\n";
    return 0;
}
