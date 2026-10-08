// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_PLUGINS_BASELINE_ROLLING_ROLLING_STATE_H_
#define _FLOWSQL_PLUGINS_BASELINE_ROLLING_ROLLING_STATE_H_

#include <framework/interfaces/ibaseline_types.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "plugins/baseline/bootstrap/bootstrap_types.h"
#include "plugins/baseline/model/task_spec.h"
#include "plugins/baseline/rolling/observation_adapter.h"
#include "plugins/baseline/rolling/rolling_config.h"

namespace flowsql {
namespace baseline {

enum class RollingStateStatus : int32_t {
    kColdLearning = 0,
    kWarming = 1,
    kReadyHint = 2,
};

const char* RollingStateStatusName(RollingStateStatus status);

enum class RollingMaturityStatus : int32_t {
    kColdLearning = 0,
    kLevelReady = 1,
    kDailyWarming = 2,
    kDailyReady = 3,
    kWeeklyWarming = 4,
    kWeeklyReady = 5,
    kMonthlyWarming = 6,
    kMonthlyReady = 7,
};

enum class ScoreTrustStatus : int32_t {
    kScoreUntrusted = 0,
    kScoreWarming = 1,
    kScoreReady = 2,
    kDriftLearning = 3,
    kRecalibrating = 4,
};

enum class RollingCalibrationStatus : int32_t {
    kUncalibrated = 0,
    kWarming = 1,
    kCalibrated = 2,
    kExpanding = 3,
    kRecalibrating = 4,
};

enum class RollingMonthposStatus : int32_t {
    kDisabled = 0,
    kMonthlyWarming = 1,
    kMonthlyReady = 2,
};

enum class RollingSeasonalPriorQuality : int32_t {
    kEmpty = 0,
    kWeak = 1,
    kPartial = 2,
    kFull = 3,
};

const char* RollingMaturityStatusName(RollingMaturityStatus status);
const char* ScoreTrustStatusName(ScoreTrustStatus status);
const char* RollingCalibrationStatusName(RollingCalibrationStatus status);
const char* RollingMonthposStatusName(RollingMonthposStatus status);

bool MaturityAtLeast(RollingMaturityStatus actual, RollingMaturityStatus expected);

struct RollingHarmonicState {
    std::vector<double> sin_coeff;
    std::vector<double> cos_coeff;
    std::vector<double> sin_p;
    std::vector<double> cos_p;
};

struct RollingThetaState {
    double level = 0.0;
    double trend = 0.0;
    RollingHarmonicState daily;
    RollingHarmonicState weekly;
};

// Minimal last-successful-Submit diagnostics; the bucket uses last_processed_bucket.
struct RollingLastSubmitView {
    double baseline_mu = 0.0;
    double baseline_lower = 0.0;
    double baseline_upper = 0.0;
    double update_weight = 0.0;
    bool valid = false;
    bool can_score = false;
    bool can_update = false;
    bool can_alert = false;
};

struct RollingState {
    std::string series_key;
    RollingThetaState theta;

    double sigma_init = 0.0;
    double sigma = 0.0;
    BootstrapSeedStatus bootstrap_seed_status = BootstrapSeedStatus::kNone;
    RollingSeasonalPriorQuality daily_prior_quality = RollingSeasonalPriorQuality::kEmpty;
    RollingSeasonalPriorQuality weekly_prior_quality = RollingSeasonalPriorQuality::kEmpty;

    double p_level = 0.0;
    double p_level_trend = 0.0;
    double p_trend = 0.0;

    double short_ewma = 0.0;
    double long_ewma = 0.0;
    double drift_evidence = 0.0;
    double level_shift_cusum_pos = 0.0;
    double level_shift_cusum_neg = 0.0;
    double level_shift_evidence = 0.0;

    RollingStateStatus state_status = RollingStateStatus::kColdLearning;
    RollingMaturityStatus maturity_status = RollingMaturityStatus::kColdLearning;
    ScoreTrustStatus score_trust_status = ScoreTrustStatus::kScoreUntrusted;
    RollingCalibrationStatus calibration_status = RollingCalibrationStatus::kUncalibrated;
    RollingMonthposStatus monthpos_status = RollingMonthposStatus::kDisabled;
    bool has_seen_observation = false;
    int64_t last_seen_bucket = 0;
    // Observation order is independent of the last model update's time anchor.
    int64_t last_processed_bucket = 0;
    RollingLastSubmitView last_submit;
    uint64_t accepted_update_count = 0;
    uint64_t maturity_prior_update_count = 0;
    double learning_confidence = 0.0;
    double score_confidence = 0.0;
    double effective_confidence = 0.0;

    double detection_band_multiplier = 1.0;
    double residual_scale_ewma = 1.0;
    double coverage_ewma = 1.0;
    double tail3_ewma = 0.0;
    double tail5_ewma = 0.0;
    double abs_z_ewma = 0.0;
    uint64_t calibration_update_count = 0;
    uint64_t stable_score_count = 0;
    uint64_t score_ready_count = 0;
    int64_t last_degradation_bucket = 0;
    std::string degradation_reason;

    std::vector<uint32_t> daily_bin_count;
    std::vector<uint32_t> weekly_bin_count;
    std::array<uint32_t, 31> monthpos_count{};
    std::vector<uint32_t> monthpos_dme_count;
    std::array<uint32_t, 7> monthpos_lwd_count{};
    uint64_t monthpos_lwd_update_count = 0;
    uint64_t month_transition_count = 0;
    int32_t last_seen_month_id = 0;

    std::vector<double> monthpos_dom_coeff;
    std::vector<double> monthpos_dme_coeff;
    std::vector<double> monthpos_lwd_coeff;
    std::vector<double> monthpos_dom_center;
    std::vector<double> monthpos_dme_center;
    std::vector<double> monthpos_lwd_center;
    uint64_t monthpos_update_count = 0;
    uint64_t monthpos_ready_count = 0;
    std::string diagnostics;
};

RollingStateStatus StatusFromAcceptedUpdateCount(uint64_t accepted_update_count,
                                                 const BaselineRollingConfig& config);

BaselineStatus BuildEmptyRollingState(std::string_view series_key,
                                      const BaselineRollingConfig& config,
                                      RollingState* out);

BaselineStatus InitializeEmptyRollingStateFromObservation(
    const ObservedModelPoint& point,
    const BaselineRollingConfig& config,
    RollingState* state);

BaselineStatus InitializeRollingStateFromBootstrapSeed(const BaselineTaskSpec& spec,
                                                       std::string_view series_key,
                                                       const BootstrapSeed& seed,
                                                       const BaselineRollingConfig& config,
                                                       RollingState* out,
                                                       std::string* diagnostics = nullptr);

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_ROLLING_ROLLING_STATE_H_
