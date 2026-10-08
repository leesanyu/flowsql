// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "plugins/baseline/rolling/rolling_state.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <string>

#include "plugins/baseline/rolling/math_utils.h"
#include "plugins/baseline/rolling/monthpos_state.h"

namespace flowsql {
namespace baseline {
namespace {

uint64_t ReadyHintThreshold(const BaselineRollingConfig& config) {
    return config.min_ready_hint_updates == 0 ? config.day_buckets
                                              : config.min_ready_hint_updates;
}

double SafeSigma(double sigma, const BaselineRollingConfig& config) {
    if (!std::isfinite(sigma) || sigma <= 0.0) return config.sigma_floor;
    return std::max(config.sigma_floor, sigma);
}

void ResizeHarmonicState(int32_t order, double p_value, RollingHarmonicState* state) {
    const std::size_t size = static_cast<std::size_t>(std::max(order, 0));
    state->sin_coeff.assign(size, 0.0);
    state->cos_coeff.assign(size, 0.0);
    state->sin_p.assign(size, p_value);
    state->cos_p.assign(size, p_value);
}

BaselineStatus InvalidArgument(std::string* diagnostics, const char* message) {
    if (diagnostics) *diagnostics = message ? message : "invalid rolling state input";
    return BaselineStatus::kInvalidArgument;
}

BaselineStatus Incompatible(std::string* diagnostics, const char* message) {
    if (diagnostics) *diagnostics = message ? message : "incompatible bootstrap seed";
    return BaselineStatus::kIncompatibleArtifact;
}

BaselineStatus Insufficient(std::string* diagnostics, const char* message) {
    if (diagnostics) *diagnostics = message ? message : "insufficient bootstrap seed";
    return BaselineStatus::kInsufficientData;
}

std::string ExpectedModelSpace(const BaselineTaskSpec& spec) {
    if (spec.feature_type == "ratio") return "logit";
    if (spec.feature_type == "value_basic" || spec.feature_type == "value_sampled") {
        return spec.value_identity_transform ? "identity" : "log1p";
    }
    return "";
}

int64_t ExpectedBucketSeconds(const BaselineTaskSpec& spec) {
    return spec.clock_spec.bucket_seconds > 0 ? spec.clock_spec.bucket_seconds : spec.delta;
}

std::string ExpectedTimezone(const BaselineTaskSpec& spec) {
    if (!spec.clock_spec.timezone.empty()) return spec.clock_spec.timezone;
    return spec.tz;
}

bool SameIdentity(const BaselineTaskSpec& spec, const BootstrapTaskIdentity& identity) {
    return identity.task_id == spec.task_id && identity.task_kind == spec.task_kind &&
           identity.feature_type == spec.feature_type && identity.feature_id == spec.feature_id &&
           identity.profile == spec.profile;
}

bool SameClock(const BaselineTaskSpec& spec, const BootstrapClockSpec& clock) {
    return clock.bucket_seconds == ExpectedBucketSeconds(spec) &&
           clock.timezone == ExpectedTimezone(spec);
}

bool SameCalendar(const BaselineTaskSpec& spec, const BootstrapCalendarRef& calendar) {
    return calendar.calendar_id == spec.calendar_ref.calendar_id &&
           calendar.calendar_version == spec.calendar_ref.calendar_version;
}

double SeedStatusScale(BootstrapSeedStatus status) {
    switch (status) {
        case BootstrapSeedStatus::kFull:
            return 0.25;
        case BootstrapSeedStatus::kPartial:
            return 0.5;
        case BootstrapSeedStatus::kWeak:
            return 2.0;
        case BootstrapSeedStatus::kNone:
            break;
    }
    return 0.0;
}

double CoverageScale(double coverage_ratio) {
    const double denominator = std::max(coverage_ratio, 0.25);
    const double scale = denominator > 0.0 ? 1.0 / denominator : 4.0;
    return std::max(1.0, std::min(4.0, scale));
}

bool HasEnabledComponent(const BootstrapSeed& seed, const char* name) {
    return std::find(seed.enabled_components.begin(), seed.enabled_components.end(), name) !=
           seed.enabled_components.end();
}

RollingSeasonalPriorQuality SeasonalPriorQuality(const BootstrapSeed& seed,
                                                 const char* component) {
    if (!HasEnabledComponent(seed, component)) return RollingSeasonalPriorQuality::kEmpty;
    switch (seed.seed_status) {
        case BootstrapSeedStatus::kFull:
            return RollingSeasonalPriorQuality::kFull;
        case BootstrapSeedStatus::kPartial:
            return RollingSeasonalPriorQuality::kPartial;
        case BootstrapSeedStatus::kWeak:
            return RollingSeasonalPriorQuality::kWeak;
        case BootstrapSeedStatus::kNone:
            break;
    }
    return RollingSeasonalPriorQuality::kEmpty;
}

RollingMaturityStatus SeedMaturityStatus(const BootstrapSeed& seed,
                                         const BaselineRollingConfig& config) {
    const uint64_t accepted = seed.maturity_init.accepted_count;
    const double coverage = std::max(seed.maturity_init.coverage_ratio,
                                     seed.coverage_report.coverage_ratio);
    if (accepted >= config.weekly_ready_min_weeks * config.week_buckets &&
        coverage >= config.weekly_ready_coverage_ratio &&
        (HasEnabledComponent(seed, "weekly") || HasEnabledComponent(seed, "daily"))) {
        return RollingMaturityStatus::kWeeklyReady;
    }
    if (accepted >= config.daily_ready_min_days * config.day_buckets &&
        coverage >= config.daily_ready_coverage_ratio &&
        (HasEnabledComponent(seed, "daily") || HasEnabledComponent(seed, "weekly"))) {
        return RollingMaturityStatus::kDailyReady;
    }
    if (accepted >= config.level_ready_min_updates) {
        return RollingMaturityStatus::kLevelReady;
    }
    return RollingMaturityStatus::kColdLearning;
}

void InitializeB3Defaults(const BaselineRollingConfig& config, RollingState* state) {
    if (!state) return;
    state->maturity_status = RollingMaturityStatus::kColdLearning;
    state->score_trust_status = ScoreTrustStatus::kScoreUntrusted;
    state->calibration_status = RollingCalibrationStatus::kUncalibrated;
    state->monthpos_status = RollingMonthposStatus::kDisabled;
    state->learning_confidence = config.confidence_cold;
    state->score_confidence = 0.0;
    state->effective_confidence = 0.0;
    state->detection_band_multiplier = 1.0;
    state->residual_scale_ewma = 1.0;
    state->coverage_ewma = 1.0;
    state->tail3_ewma = 0.0;
    state->tail5_ewma = 0.0;
    state->abs_z_ewma = 0.0;
    state->calibration_update_count = 0;
    state->stable_score_count = 0;
    state->score_ready_count = 0;
    state->last_degradation_bucket = 0;
    state->degradation_reason.clear();
    state->daily_bin_count.assign(std::max<uint32_t>(1, config.daily_coverage_bins), 0);
    state->weekly_bin_count.assign(std::max<uint32_t>(1, config.weekly_coverage_bins), 0);
    state->monthpos_count.fill(0);
    state->monthpos_dme_count.assign(8, 0);
    state->monthpos_lwd_count.fill(0);
    state->monthpos_lwd_update_count = 0;
    state->month_transition_count = 0;
    state->last_seen_month_id = 0;
    state->monthpos_dom_coeff.assign(31, 0.0);
    state->monthpos_dom_center.assign(31, 0.0);
    state->monthpos_dme_coeff.assign(8, 0.0);
    state->monthpos_dme_center.assign(8, 0.0);
    state->monthpos_lwd_coeff.assign(7, 0.0);
    state->monthpos_lwd_center.assign(7, 0.0);
    state->monthpos_update_count = 0;
    state->monthpos_ready_count = 0;
}

double SeedCovariance(double init_scale,
                      double component_scale,
                      double sigma,
                      double seed_status_scale,
                      double coverage_scale,
                      const BaselineRollingConfig& config) {
    return ClampCovariance(init_scale * sigma * sigma * component_scale *
                               seed_status_scale * coverage_scale,
                           config);
}

void CopyHarmonicSeed(const std::vector<BootstrapHarmonicInit>& source,
                      int32_t max_order,
                      RollingHarmonicState* target,
                      std::ostringstream* diagnostics,
                      const char* diagnostic_key) {
    for (const auto& item : source) {
        if (item.order <= 0) continue;
        if (item.order > max_order) {
            if (diagnostics && diagnostics->tellp() >= 0) {
                if (diagnostics->tellp() > 0) *diagnostics << ";";
                *diagnostics << diagnostic_key << "=" << item.order;
            }
            continue;
        }
        const std::size_t idx = static_cast<std::size_t>(item.order - 1);
        target->sin_coeff[idx] = item.sin;
        target->cos_coeff[idx] = item.cos;
    }
}

BaselineStatus ValidateSeedForState(const BaselineTaskSpec& spec,
                                    std::string_view series_key,
                                    const BootstrapSeed& seed,
                                    std::string* diagnostics) {
    if (series_key.empty()) return InvalidArgument(diagnostics, "series_key must not be empty");
    if (seed.series_key != std::string(series_key)) {
        return Incompatible(diagnostics, "bootstrap seed series_key mismatch");
    }
    if (seed.seed_status == BootstrapSeedStatus::kNone) {
        return Insufficient(diagnostics, "bootstrap seed_status is none");
    }
    if (spec.task_kind == "value" && seed.artifact_kind != BootstrapArtifactKind::kValue) {
        return Incompatible(diagnostics, "bootstrap seed artifact kind mismatch");
    }
    if (spec.task_kind == "ratio" && seed.artifact_kind != BootstrapArtifactKind::kRatio) {
        return Incompatible(diagnostics, "bootstrap seed artifact kind mismatch");
    }
    if (!SameIdentity(spec, seed.task_identity)) {
        return Incompatible(diagnostics, "bootstrap seed task identity mismatch");
    }
    if (!SameClock(spec, seed.clock_spec)) {
        return Incompatible(diagnostics, "bootstrap seed clock mismatch");
    }
    if (!SameCalendar(spec, seed.calendar_ref)) {
        return Incompatible(diagnostics, "bootstrap seed calendar mismatch");
    }
    if (!seed.theta_init.available || !seed.sigma_init.available ||
        !seed.uncertainty_init.available || !seed.maturity_init.available) {
        return Insufficient(diagnostics, "bootstrap seed missing rolling-critical init fields");
    }
    const std::string expected_model_space = ExpectedModelSpace(spec);
    if (expected_model_space.empty()) {
        return Incompatible(diagnostics, "unsupported task feature_type for rolling seed");
    }
    if (seed.theta_init.model_space != expected_model_space ||
        seed.sigma_init.model_space != expected_model_space) {
        return Incompatible(diagnostics, "bootstrap seed model_space mismatch");
    }
    if (!std::isfinite(seed.sigma_init.value) || seed.sigma_init.value <= 0.0) {
        return Insufficient(diagnostics, "bootstrap seed sigma_init is invalid");
    }
    if (SeedStatusScale(seed.seed_status) <= 0.0) {
        return Insufficient(diagnostics, "bootstrap seed status cannot initialize rolling state");
    }
    return BaselineStatus::kOk;
}

}  // namespace

const char* RollingStateStatusName(RollingStateStatus status) {
    switch (status) {
        case RollingStateStatus::kColdLearning:
            return "cold_learning";
        case RollingStateStatus::kWarming:
            return "warming";
        case RollingStateStatus::kReadyHint:
            return "ready_hint";
    }
    return "cold_learning";
}

const char* RollingMaturityStatusName(RollingMaturityStatus status) {
    switch (status) {
        case RollingMaturityStatus::kColdLearning:
            return "cold_learning";
        case RollingMaturityStatus::kLevelReady:
            return "level_ready";
        case RollingMaturityStatus::kDailyWarming:
            return "daily_warming";
        case RollingMaturityStatus::kDailyReady:
            return "daily_ready";
        case RollingMaturityStatus::kWeeklyWarming:
            return "weekly_warming";
        case RollingMaturityStatus::kWeeklyReady:
            return "weekly_ready";
        case RollingMaturityStatus::kMonthlyWarming:
            return "monthly_warming";
        case RollingMaturityStatus::kMonthlyReady:
            return "monthly_ready";
    }
    return "cold_learning";
}

const char* ScoreTrustStatusName(ScoreTrustStatus status) {
    switch (status) {
        case ScoreTrustStatus::kScoreUntrusted:
            return "score_untrusted";
        case ScoreTrustStatus::kScoreWarming:
            return "score_warming";
        case ScoreTrustStatus::kScoreReady:
            return "score_ready";
        case ScoreTrustStatus::kDriftLearning:
            return "drift_learning";
        case ScoreTrustStatus::kRecalibrating:
            return "recalibrating";
    }
    return "score_untrusted";
}

const char* RollingCalibrationStatusName(RollingCalibrationStatus status) {
    switch (status) {
        case RollingCalibrationStatus::kUncalibrated:
            return "uncalibrated";
        case RollingCalibrationStatus::kWarming:
            return "warming";
        case RollingCalibrationStatus::kCalibrated:
            return "calibrated";
        case RollingCalibrationStatus::kExpanding:
            return "expanding";
        case RollingCalibrationStatus::kRecalibrating:
            return "recalibrating";
    }
    return "uncalibrated";
}

const char* RollingMonthposStatusName(RollingMonthposStatus status) {
    switch (status) {
        case RollingMonthposStatus::kDisabled:
            return "disabled";
        case RollingMonthposStatus::kMonthlyWarming:
            return "monthly_warming";
        case RollingMonthposStatus::kMonthlyReady:
            return "monthly_ready";
    }
    return "disabled";
}

bool MaturityAtLeast(RollingMaturityStatus actual, RollingMaturityStatus expected) {
    return static_cast<int32_t>(actual) >= static_cast<int32_t>(expected);
}

RollingStateStatus StatusFromAcceptedUpdateCount(uint64_t accepted_update_count,
                                                 const BaselineRollingConfig& config) {
    if (accepted_update_count < config.min_warming_updates) {
        return RollingStateStatus::kColdLearning;
    }
    if (accepted_update_count < ReadyHintThreshold(config)) {
        return RollingStateStatus::kWarming;
    }
    return RollingStateStatus::kReadyHint;
}

BaselineStatus BuildEmptyRollingState(std::string_view series_key,
                                      const BaselineRollingConfig& config,
                                      RollingState* out) {
    if (!out || series_key.empty()) return BaselineStatus::kInvalidArgument;

    RollingState state;
    state.series_key = std::string(series_key);
    state.sigma_init = config.sigma_floor;
    state.sigma = config.sigma_floor;
    state.bootstrap_seed_status = BootstrapSeedStatus::kNone;
    state.daily_prior_quality = RollingSeasonalPriorQuality::kEmpty;
    state.weekly_prior_quality = RollingSeasonalPriorQuality::kEmpty;
    state.p_level =
        ClampCovariance(config.p_level_init_scale * config.sigma_floor * config.sigma_floor,
                        config);
    state.p_level_trend = 0.0;
    state.p_trend =
        ClampCovariance(config.p_trend_init_scale * config.sigma_floor * config.sigma_floor,
                        config);
    const double p_day =
        ClampCovariance(config.p_day_init_scale * config.sigma_floor * config.sigma_floor,
                        config);
    const double p_week =
        ClampCovariance(config.p_week_init_scale * config.sigma_floor * config.sigma_floor,
                        config);
    ResizeHarmonicState(config.daily_harmonic_order, p_day, &state.theta.daily);
    ResizeHarmonicState(config.weekly_harmonic_order, p_week, &state.theta.weekly);
    InitializeB3Defaults(config, &state);
    state.state_status = RollingStateStatus::kColdLearning;
    *out = std::move(state);
    return BaselineStatus::kOk;
}

BaselineStatus InitializeEmptyRollingStateFromObservation(const ObservedModelPoint& point,
                                                         const BaselineRollingConfig& config,
                                                         RollingState* state) {
    if (!state || point.series_key.empty() || state->series_key != point.series_key) {
        return BaselineStatus::kInvalidArgument;
    }
    if (point.status != BaselineStatus::kOk) return point.status;
    if (!point.can_update) return BaselineStatus::kInsufficientData;
    if (!std::isfinite(point.y_model)) return BaselineStatus::kInvalidArgument;

    RollingState next = *state;
    next.theta.level = point.y_model;
    next.theta.trend = 0.0;
    next.has_seen_observation = true;
    next.last_seen_bucket = point.bucket_id;
    next.last_processed_bucket = point.bucket_id;
    next.accepted_update_count = 1;
    next.maturity_prior_update_count = 0;
    next.state_status = StatusFromAcceptedUpdateCount(next.accepted_update_count, config);
    next.maturity_status = RollingMaturityStatus::kColdLearning;
    next.learning_confidence = config.confidence_cold;
    next.effective_confidence = 0.0;
    next.diagnostics = "cold_start;first_observation";
    *state = std::move(next);
    return BaselineStatus::kOk;
}

BaselineStatus InitializeRollingStateFromBootstrapSeed(const BaselineTaskSpec& spec,
                                                       std::string_view series_key,
                                                       const BootstrapSeed& seed,
                                                       const BaselineRollingConfig& config,
                                                       RollingState* out,
                                                       std::string* diagnostics) {
    if (!out) return InvalidArgument(diagnostics, "rolling state output must not be null");
    const BaselineStatus validation = ValidateSeedForState(spec, series_key, seed, diagnostics);
    if (validation != BaselineStatus::kOk) return validation;

    const double sigma = SafeSigma(seed.sigma_init.value, config);
    const double seed_status_scale = SeedStatusScale(seed.seed_status);
    const double coverage_scale = CoverageScale(seed.coverage_report.coverage_ratio);
    const auto& component = seed.uncertainty_init.component_uncertainty;

    RollingState state;
    state.series_key = std::string(series_key);
    InitializeB3Defaults(config, &state);
    const int64_t anchor_bucket =
        std::max(seed.theta_init.reference_bucket_id, seed.coverage_report.train_end_bucket);
    state.theta.level =
        seed.theta_init.level +
        seed.theta_init.trend *
            static_cast<double>(anchor_bucket - seed.theta_init.reference_bucket_id);
    state.theta.trend = seed.theta_init.trend;
    state.sigma_init = sigma;
    state.sigma = sigma;
    state.bootstrap_seed_status = seed.seed_status;
    state.daily_prior_quality = SeasonalPriorQuality(seed, "daily");
    state.weekly_prior_quality = SeasonalPriorQuality(seed, "weekly");
    state.p_level = SeedCovariance(config.p_level_init_scale,
                                   component.level_scale,
                                   sigma,
                                   seed_status_scale,
                                   coverage_scale,
                                   config);
    state.p_level_trend = 0.0;
    state.p_trend = SeedCovariance(config.p_trend_init_scale,
                                   component.trend_scale,
                                   sigma,
                                   seed_status_scale,
                                   coverage_scale,
                                   config);

    const double p_day = SeedCovariance(config.p_day_init_scale,
                                        component.daily_scale,
                                        sigma,
                                        seed_status_scale,
                                        coverage_scale,
                                        config);
    const double p_week = SeedCovariance(config.p_week_init_scale,
                                         component.weekly_scale,
                                         sigma,
                                         seed_status_scale,
                                         coverage_scale,
                                         config);
    ResizeHarmonicState(config.daily_harmonic_order, p_day, &state.theta.daily);
    ResizeHarmonicState(config.weekly_harmonic_order, p_week, &state.theta.weekly);

    std::ostringstream diag;
    CopyHarmonicSeed(seed.theta_init.daily_harmonic,
                     config.daily_harmonic_order,
                     &state.theta.daily,
                     &diag,
                     "ignored_daily_harmonic_order");
    CopyHarmonicSeed(seed.theta_init.weekly_harmonic,
                     config.weekly_harmonic_order,
                     &state.theta.weekly,
                     &diag,
                     "ignored_weekly_harmonic_order");

    state.has_seen_observation = true;
    state.last_seen_bucket = anchor_bucket;
    state.last_processed_bucket = anchor_bucket;
    state.accepted_update_count =
        std::min(seed.maturity_init.accepted_count, ReadyHintThreshold(config));
    state.maturity_prior_update_count =
        seed.maturity_init.accepted_count > state.accepted_update_count
            ? seed.maturity_init.accepted_count - state.accepted_update_count
            : 0;
    state.state_status = StatusFromAcceptedUpdateCount(state.accepted_update_count, config);
    state.maturity_status = SeedMaturityStatus(seed, config);
    state.learning_confidence =
        std::min(seed.maturity_init.confidence, config.confidence_ready_hint_cap);
    state.score_trust_status =
        MaturityAtLeast(state.maturity_status, RollingMaturityStatus::kLevelReady)
            ? ScoreTrustStatus::kScoreWarming
            : ScoreTrustStatus::kScoreUntrusted;
    state.score_confidence =
        state.score_trust_status == ScoreTrustStatus::kScoreWarming ? config.confidence_warming
                                                                    : 0.0;
    state.effective_confidence = std::min(state.learning_confidence, state.score_confidence);
    state.calibration_status = RollingCalibrationStatus::kWarming;
    state.detection_band_multiplier =
        ClampMultiplier(seed.uncertainty_init.band_z / std::max(config.band_z, 1.0e-12),
                        config,
                        1.0);
    state.residual_scale_ewma =
        state.detection_band_multiplier * state.detection_band_multiplier;
    state.coverage_ewma = 1.0;
    state.calibration_update_count = 0;
    if (seed.coverage_report.coverage_ratio > 0.0) {
        const auto daily_fill = static_cast<std::size_t>(
            std::min<double>(state.daily_bin_count.size(),
                             std::ceil(seed.coverage_report.coverage_ratio *
                                       state.daily_bin_count.size())));
        for (std::size_t i = 0; i < daily_fill; ++i) state.daily_bin_count[i] = 1;
        const auto weekly_fill = static_cast<std::size_t>(
            std::min<double>(state.weekly_bin_count.size(),
                             std::ceil(seed.coverage_report.coverage_ratio *
                                       state.weekly_bin_count.size())));
        for (std::size_t i = 0; i < weekly_fill; ++i) state.weekly_bin_count[i] = 1;
    }
    if (seed.monthpos_hint.available) {
        const BaselineStatus monthpos_status =
            InitializeRollingMonthposFromSeed(seed, config, &state);
        if (monthpos_status != BaselineStatus::kOk && diag.tellp() >= 0) {
            if (diag.tellp() > 0) diag << ";";
            diag << "monthpos_seed_ignored";
        }
    }
    state.short_ewma = 0.0;
    state.long_ewma = 0.0;
    state.drift_evidence = 0.0;
    state.diagnostics = diag.str();
    if (diagnostics) *diagnostics = state.diagnostics;
    *out = std::move(state);
    return BaselineStatus::kOk;
}

}  // namespace baseline
}  // namespace flowsql
