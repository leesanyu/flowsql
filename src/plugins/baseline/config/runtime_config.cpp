// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "plugins/baseline/config/runtime_config.h"

#include <common/error_code.h>

#include <algorithm>
#include <atomic>
#include <exception>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <yaml-cpp/yaml.h>

#include "plugins/baseline/model/event_calendar_matcher.h"
#include "plugins/baseline/model/event_calendar_spec.h"
#include "plugins/baseline/model/profile_config.h"
#include "plugins/baseline/model/task_spec.h"
#include "plugins/baseline/bootstrap/bootstrap_types.h"
#include "plugins/baseline/rolling/rolling_config.h"
#include "plugins/baseline/solver/solver_backend.h"

namespace flowsql {
namespace baseline {

namespace {

struct RatioGlobalNumericalConfig {
    double eps_logit = kRatioEpsLogit;
    double m_floor = kRatioMFloor;
    double v_floor = kRatioVFloor;
};

struct RuntimeConfigState {
    std::string tz_default = "Asia/Shanghai";

    SharedProfileConfig shared_profile;
    std::unordered_map<std::string, ValueSampledProfileConfig> value_sampled_profiles;
    RatioGlobalNumericalConfig ratio_global;
    std::unordered_map<std::string, RatioProfileConfig> ratio_profiles;
    BootstrapSeedQualityConfig seed_quality;
    BaselineRollingConfig rolling_config;
    BlockSolverConfig block_solver;
    std::unordered_map<std::string, std::shared_ptr<const CompiledEventCalendar>>
        calendars;
};

SharedProfileConfig BuildDefaultSharedProfileConfigRaw() {
    SharedProfileConfig config;
    return config;
}

std::unordered_map<std::string, ValueSampledProfileConfig> BuildDefaultValueSampledProfiles() {
    std::unordered_map<std::string, ValueSampledProfileConfig> profiles;
    ValueSampledProfileConfig cont_core;
    cont_core.name = "cont_core";
    cont_core.n_train_min = 50;
    cont_core.transform_name_override = "log1p";
    profiles.emplace(cont_core.name, cont_core);

    ValueSampledProfileConfig cont_tail;
    cont_tail.name = "cont_tail";
    cont_tail.n_train_min = 100;
    cont_tail.transform_name_override = "log1p";
    profiles.emplace(cont_tail.name, cont_tail);
    return profiles;
}

std::unordered_map<std::string, RatioProfileConfig> BuildDefaultRatioProfiles(
    const RatioGlobalNumericalConfig& global) {
    std::unordered_map<std::string, RatioProfileConfig> profiles;

    RatioProfileConfig rate_core;
    rate_core.name = "rate_core";
    rate_core.d_min_train = 50;
    rate_core.s_prior = 2.0;
    rate_core.phi_over = 1.5;
    rate_core.eps_logit = global.eps_logit;
    rate_core.m_floor = global.m_floor;
    rate_core.v_floor = global.v_floor;
    profiles.emplace(rate_core.name, rate_core);

    RatioProfileConfig ratio_bursty;
    ratio_bursty.name = "ratio_bursty";
    ratio_bursty.d_min_train = 100;
    ratio_bursty.s_prior = 4.0;
    ratio_bursty.phi_over = 2.0;
    ratio_bursty.eps_logit = global.eps_logit;
    ratio_bursty.m_floor = global.m_floor;
    ratio_bursty.v_floor = global.v_floor;
    profiles.emplace(ratio_bursty.name, ratio_bursty);

    return profiles;
}

RuntimeConfigState BuildDefaultRuntimeConfigState() {
    RuntimeConfigState state;
    state.shared_profile = BuildDefaultSharedProfileConfigRaw();
    state.value_sampled_profiles = BuildDefaultValueSampledProfiles();
    state.ratio_profiles = BuildDefaultRatioProfiles(state.ratio_global);
    state.rolling_config = DefaultBaselineRollingConfig();
    return state;
}

std::string CalendarKey(const std::string& calendar_id,
                        const std::string& calendar_version) {
    return calendar_id + "\n" + calendar_version;
}

std::shared_ptr<const RuntimeConfigState>& SnapshotRef() {
    static std::shared_ptr<const RuntimeConfigState> snapshot =
        std::make_shared<const RuntimeConfigState>(BuildDefaultRuntimeConfigState());
    return snapshot;
}

std::shared_ptr<const RuntimeConfigState> Snapshot() {
    return std::atomic_load_explicit(&SnapshotRef(), std::memory_order_acquire);
}

void StoreSnapshot(const RuntimeConfigState& state) {
    auto next = std::make_shared<const RuntimeConfigState>(state);
    std::atomic_store_explicit(&SnapshotRef(), std::move(next), std::memory_order_release);
}

template <typename T>
void ParseOptionalScalar(const YAML::Node& node, const char* key, T* out) {
    if (!out || !node || !node[key]) return;
    *out = node[key].as<T>();
}

void ParseSharedProfileConfig(const YAML::Node& node, SharedProfileConfig* out) {
    if (!node || !out) return;
    ParseOptionalScalar(node, "daily_harmonic_order", &out->k_day);
    ParseOptionalScalar(node, "weekly_harmonic_order", &out->k_week);
    ParseOptionalScalar(node, "dme_max", &out->dme_max);
    ParseOptionalScalar(node, "m_month_enable", &out->m_month_enable);
    ParseOptionalScalar(node, "month_cov_min", &out->month_cov_min);
    ParseOptionalScalar(node, "lambda_season", &out->lambda_season);
    ParseOptionalScalar(node, "lambda_dom", &out->lambda_dom);
    ParseOptionalScalar(node, "lambda_dme", &out->lambda_dme);
    ParseOptionalScalar(node, "lambda_lwd", &out->lambda_lwd);
    ParseOptionalScalar(node, "lambda_event", &out->lambda_event);
}

void ParseValueSampledProfiles(const YAML::Node& node, RuntimeConfigState* out) {
    if (!node || !out) return;
    out->value_sampled_profiles.clear();
    for (auto it = node.begin(); it != node.end(); ++it) {
        const std::string profile_name = it->first.as<std::string>();
        const YAML::Node profile_node = it->second;
        if (!profile_node || !profile_node.IsMap()) continue;

        ValueSampledProfileConfig profile;
        profile.name = profile_name;
        ParseOptionalScalar(profile_node, "n_train_min", &profile.n_train_min);
        ParseOptionalScalar(
            profile_node, "transform_name_override", &profile.transform_name_override);
        out->value_sampled_profiles[profile_name] = std::move(profile);
    }
}

void ParseRatioProfiles(const YAML::Node& node, RuntimeConfigState* out) {
    if (!node || !out) return;
    if (node["global"]) {
        const YAML::Node global = node["global"];
        ParseOptionalScalar(global, "eps_logit", &out->ratio_global.eps_logit);
        ParseOptionalScalar(global, "m_floor", &out->ratio_global.m_floor);
        ParseOptionalScalar(global, "v_floor", &out->ratio_global.v_floor);
    }

    out->ratio_profiles.clear();
    for (auto it = node.begin(); it != node.end(); ++it) {
        const std::string profile_name = it->first.as<std::string>();
        if (profile_name == "global") continue;

        const YAML::Node profile_node = it->second;
        if (!profile_node || !profile_node.IsMap()) continue;

        RatioProfileConfig profile;
        profile.name = profile_name;
        profile.eps_logit = out->ratio_global.eps_logit;
        profile.m_floor = out->ratio_global.m_floor;
        profile.v_floor = out->ratio_global.v_floor;
        ParseOptionalScalar(profile_node, "d_min_train", &profile.d_min_train);
        ParseOptionalScalar(profile_node, "s_prior", &profile.s_prior);
        ParseOptionalScalar(profile_node, "phi_over", &profile.phi_over);
        out->ratio_profiles[profile_name] = std::move(profile);
    }
}

void ParseBlockSolverConfig(const YAML::Node& node, BlockSolverConfig* out) {
    if (!node || !out) return;
    ParseOptionalScalar(node, "solver_name", &out->solver_name);
    ParseOptionalScalar(node, "c_huber", &out->c_huber);
    ParseOptionalScalar(node, "s_min_fit", &out->s_min_fit);
    ParseOptionalScalar(node, "max_iter_fit", &out->max_iter_fit);
    ParseOptionalScalar(node, "tol_obj_rel", &out->tol_obj_rel);
    ParseOptionalScalar(node, "tol_beta_inf", &out->tol_beta_inf);
    ParseOptionalScalar(node, "cond_max", &out->cond_max);
}

void ParseBootstrapConfig(const YAML::Node& node, BootstrapSeedQualityConfig* out) {
    if (!node || !out) return;
    const YAML::Node seed_quality = node["seed_quality"];
    if (!seed_quality) return;
    ParseOptionalScalar(seed_quality, "full_min_coverage_ratio",
                        &out->full_min_coverage_ratio);
    ParseOptionalScalar(seed_quality, "partial_min_coverage_ratio",
                        &out->partial_min_coverage_ratio);
    ParseOptionalScalar(seed_quality, "daily_min_span_days",
                        &out->daily_min_span_days);
    ParseOptionalScalar(seed_quality, "weekly_min_span_days",
                        &out->weekly_min_span_days);
    ParseOptionalScalar(seed_quality, "daily_phase_coverage_ratio",
                        &out->daily_phase_coverage_ratio);
    ParseOptionalScalar(seed_quality, "weekly_phase_coverage_ratio",
                        &out->weekly_phase_coverage_ratio);
}

void ParseRollingConfig(const YAML::Node& node, BaselineRollingConfig* out) {
    if (!node || !out) return;
    ParseOptionalScalar(node, "n_min_score", &out->n_min_score);
    ParseOptionalScalar(node, "n_min_update", &out->n_min_update);
    ParseOptionalScalar(node, "n_ref", &out->n_ref);
    ParseOptionalScalar(node, "sample_count_noise", &out->sample_count_noise);
    ParseOptionalScalar(node, "d_min_score", &out->d_min_score);
    ParseOptionalScalar(node, "d_min_update", &out->d_min_update);
    ParseOptionalScalar(node, "d_ref", &out->d_ref);
    ParseOptionalScalar(node, "ratio_denominator_noise", &out->ratio_denominator_noise);
    ParseOptionalScalar(node, "z_downweight", &out->z_downweight);
    ParseOptionalScalar(node, "z_skip", &out->z_skip);
    ParseOptionalScalar(node, "small_update_weight", &out->small_update_weight);
    ParseOptionalScalar(node, "level_learning_scale", &out->level_learning_scale);
    ParseOptionalScalar(node, "day_learning_scale", &out->day_learning_scale);
    ParseOptionalScalar(node, "week_learning_scale", &out->week_learning_scale);
    ParseOptionalScalar(node, "cold_day_learning_scale", &out->cold_day_learning_scale);
    ParseOptionalScalar(node, "cold_week_learning_scale", &out->cold_week_learning_scale);
    ParseOptionalScalar(node, "seasonal_drift_min_scale", &out->seasonal_drift_min_scale);
    ParseOptionalScalar(node, "full_seed_seasonal_scale",
                        &out->full_seed_seasonal_scale);
    ParseOptionalScalar(node, "partial_seed_seasonal_scale",
                        &out->partial_seed_seasonal_scale);
    ParseOptionalScalar(node, "freeze_seeded_seasonal_on_drift",
                        &out->freeze_seeded_seasonal_on_drift);
    ParseOptionalScalar(node, "day_delta_coeff_max", &out->day_delta_coeff_max_scale);
    ParseOptionalScalar(node, "day_delta_coeff_max_scale", &out->day_delta_coeff_max_scale);
    ParseOptionalScalar(node, "week_delta_coeff_max", &out->week_delta_coeff_max_scale);
    ParseOptionalScalar(node, "week_delta_coeff_max_scale", &out->week_delta_coeff_max_scale);
    ParseOptionalScalar(node, "Q_day", &out->q_day_scale);
    ParseOptionalScalar(node, "q_day_scale", &out->q_day_scale);
    ParseOptionalScalar(node, "Q_week", &out->q_week_scale);
    ParseOptionalScalar(node, "q_week_scale", &out->q_week_scale);
    ParseOptionalScalar(node, "Q_level", &out->q_level_scale);
    ParseOptionalScalar(node, "q_level_scale", &out->q_level_scale);
    ParseOptionalScalar(node, "Q_trend", &out->q_trend_scale);
    ParseOptionalScalar(node, "q_trend_scale", &out->q_trend_scale);
    ParseOptionalScalar(node, "trend_update_scale", &out->trend_update_scale);
    ParseOptionalScalar(node, "cold_trend_update_scale", &out->cold_trend_update_scale);
    ParseOptionalScalar(node, "trend_delta_max", &out->trend_delta_max_scale);
    ParseOptionalScalar(node, "trend_delta_max_scale", &out->trend_delta_max_scale);
    ParseOptionalScalar(node, "trend_abs_max", &out->trend_abs_max_scale);
    ParseOptionalScalar(node, "trend_abs_max_scale", &out->trend_abs_max_scale);
    ParseOptionalScalar(node, "P_level_init", &out->p_level_init_scale);
    ParseOptionalScalar(node, "p_level_init_scale", &out->p_level_init_scale);
    ParseOptionalScalar(node, "P_trend_init", &out->p_trend_init_scale);
    ParseOptionalScalar(node, "p_trend_init_scale", &out->p_trend_init_scale);
    ParseOptionalScalar(node, "P_day_init", &out->p_day_init_scale);
    ParseOptionalScalar(node, "p_day_init_scale", &out->p_day_init_scale);
    ParseOptionalScalar(node, "P_week_init", &out->p_week_init_scale);
    ParseOptionalScalar(node, "p_week_init_scale", &out->p_week_init_scale);
    ParseOptionalScalar(node, "P_floor", &out->p_floor_scale);
    ParseOptionalScalar(node, "p_floor_scale", &out->p_floor_scale);
    ParseOptionalScalar(node, "P_cap", &out->p_cap_scale);
    ParseOptionalScalar(node, "p_cap_scale", &out->p_cap_scale);
    ParseOptionalScalar(node, "alpha_short", &out->alpha_short);
    ParseOptionalScalar(node, "alpha_long", &out->alpha_long);
    ParseOptionalScalar(node, "z_cap", &out->z_cap);
    ParseOptionalScalar(node, "drift_start", &out->drift_start);
    ParseOptionalScalar(node, "drift_full", &out->drift_full);
    ParseOptionalScalar(node, "level_shift_reference_z", &out->level_shift_reference_z);
    ParseOptionalScalar(node, "level_shift_cusum_decay", &out->level_shift_cusum_decay);
    ParseOptionalScalar(node, "level_shift_cusum_threshold",
                        &out->level_shift_cusum_threshold);
    ParseOptionalScalar(node, "max_level_boost", &out->max_level_boost);
    ParseOptionalScalar(node, "max_q_boost", &out->max_q_boost);
    ParseOptionalScalar(node, "skip_relax", &out->skip_relax);
    ParseOptionalScalar(node, "process_noise_gap_cap_buckets",
                        &out->process_noise_gap_cap_buckets);
    ParseOptionalScalar(node, "alpha_sigma", &out->alpha_sigma);
    ParseOptionalScalar(node, "c_sigma", &out->c_sigma);
    ParseOptionalScalar(node, "sigma_floor", &out->sigma_floor);
    ParseOptionalScalar(node, "cold_start_band_scale", &out->cold_start_band_scale);
    ParseOptionalScalar(node, "band_z", &out->band_z);
    ParseOptionalScalar(node, "forecast_band_z", &out->forecast_band_z);
    ParseOptionalScalar(node, "confidence_cold", &out->confidence_cold);
    ParseOptionalScalar(node, "confidence_warming", &out->confidence_warming);
    ParseOptionalScalar(node, "confidence_ready_hint_cap",
                        &out->confidence_ready_hint_cap);
    ParseOptionalScalar(node, "min_warming_updates", &out->min_warming_updates);
    ParseOptionalScalar(node, "min_ready_hint_updates", &out->min_ready_hint_updates);
    ParseOptionalScalar(node, "level_ready_min_updates", &out->level_ready_min_updates);
    ParseOptionalScalar(node, "score_warming_min_updates", &out->score_warming_min_updates);
    ParseOptionalScalar(node, "score_ready_min_updates", &out->score_ready_min_updates);
    ParseOptionalScalar(node, "score_recovery_min_updates", &out->score_recovery_min_updates);
    ParseOptionalScalar(node, "score_drift_degrade_start", &out->score_drift_degrade_start);
    ParseOptionalScalar(node, "calibration_alpha", &out->calibration_alpha);
    ParseOptionalScalar(node, "calibration_warmup_min_updates",
                        &out->calibration_warmup_min_updates);
    ParseOptionalScalar(node, "calibration_coverage_floor",
                        &out->calibration_coverage_floor);
    ParseOptionalScalar(node, "calibration_tail3_limit", &out->calibration_tail3_limit);
    ParseOptionalScalar(node, "calibration_tail5_limit", &out->calibration_tail5_limit);
    ParseOptionalScalar(node, "calibration_multiplier_min",
                        &out->calibration_multiplier_min);
    ParseOptionalScalar(node, "calibration_multiplier_max",
                        &out->calibration_multiplier_max);
    ParseOptionalScalar(node, "daily_coverage_bins", &out->daily_coverage_bins);
    ParseOptionalScalar(node, "weekly_coverage_bins", &out->weekly_coverage_bins);
    ParseOptionalScalar(node, "daily_ready_min_days", &out->daily_ready_min_days);
    ParseOptionalScalar(node, "daily_ready_coverage_ratio",
                        &out->daily_ready_coverage_ratio);
    ParseOptionalScalar(node, "weekly_ready_min_weeks", &out->weekly_ready_min_weeks);
    ParseOptionalScalar(node, "weekly_ready_coverage_ratio",
                        &out->weekly_ready_coverage_ratio);
    ParseOptionalScalar(node, "maturity_uncertainty_cold_scale",
                        &out->maturity_uncertainty_cold_scale);
    ParseOptionalScalar(node, "maturity_uncertainty_warming_scale",
                        &out->maturity_uncertainty_warming_scale);
    ParseOptionalScalar(node, "maturity_uncertainty_drift_scale",
                        &out->maturity_uncertainty_drift_scale);
    ParseOptionalScalar(node, "maturity_uncertainty_recalibrating_scale",
                        &out->maturity_uncertainty_recalibrating_scale);
    ParseOptionalScalar(node, "missing_daily_uncertainty_scale",
                        &out->missing_daily_uncertainty_scale);
    ParseOptionalScalar(node, "missing_weekly_uncertainty_scale",
                        &out->missing_weekly_uncertainty_scale);
    ParseOptionalScalar(node, "level_only_extreme_z", &out->level_only_extreme_z);
    ParseOptionalScalar(node, "detection_band_std_cap", &out->detection_band_std_cap);
    ParseOptionalScalar(node, "forecast_trend_cap_buckets",
                        &out->forecast_trend_cap_buckets);
    ParseOptionalScalar(node, "monthpos_alpha", &out->monthpos_alpha);
    ParseOptionalScalar(node, "monthpos_delta_max_scale", &out->monthpos_delta_max_scale);
    ParseOptionalScalar(node, "monthpos_min_month_transitions",
                        &out->monthpos_min_month_transitions);
    ParseOptionalScalar(node, "monthpos_ready_coverage_ratio",
                        &out->monthpos_ready_coverage_ratio);
    const YAML::Node relation = node["relation_rolling"];
    if (relation) {
        ParseOptionalScalar(relation,
                            "enable_routed_rolling",
                            &out->relation_rolling.enable_routed_rolling);
        ParseOptionalScalar(relation,
                            "enable_stream_basis",
                            &out->relation_rolling.enable_stream_basis);
        ParseOptionalScalar(
            relation,
            "include_universal_summaries_without_basis",
            &out->relation_rolling.include_universal_summaries_without_basis);
        ParseOptionalScalar(relation,
                            "basis_stats_max_groups",
                            &out->relation_rolling.basis_stats_max_groups);
        ParseOptionalScalar(relation,
                            "basis_collect_min_buckets",
                            &out->relation_rolling.basis_collect_min_buckets);
        ParseOptionalScalar(relation,
                            "basis_ready_min_buckets",
                            &out->relation_rolling.basis_ready_min_buckets);
        ParseOptionalScalar(relation,
                            "basis_refresh_interval_buckets",
                            &out->relation_rolling.basis_refresh_interval_buckets);
        ParseOptionalScalar(
            relation,
            "basis_candidate_min_coverage_ratio",
            &out->relation_rolling.basis_candidate_min_coverage_ratio);
        ParseOptionalScalar(relation,
                            "basis_replacement_cap_ratio",
                            &out->relation_rolling.basis_replacement_cap_ratio);
        ParseOptionalScalar(relation,
                            "basis_replacement_cap_max",
                            &out->relation_rolling.basis_replacement_cap_max);
        ParseOptionalScalar(relation,
                            "basis_handover_warmup_buckets",
                            &out->relation_rolling.basis_handover_warmup_buckets);
        ParseOptionalScalar(relation,
                            "basis_threshold_margin",
                            &out->relation_rolling.basis_threshold_margin);
        ParseOptionalScalar(relation,
                            "basis_min_stable_refresh_count",
                            &out->relation_rolling.basis_min_stable_refresh_count);
        ParseOptionalScalar(relation,
                            "routed_state_shard_count",
                            &out->relation_rolling.routed_state_shard_count);
        const YAML::Node fusion = relation["relation_fusion"];
        if (fusion) {
            auto& relation_fusion = out->relation_rolling.relation_fusion;
            ParseOptionalScalar(fusion,
                                "enable_relation_fusion",
                                &relation_fusion.enable_relation_fusion);
            ParseOptionalScalar(fusion,
                                "fusion_z_score_cap",
                                &relation_fusion.fusion_z_score_cap);
            ParseOptionalScalar(fusion,
                                "fusion_min_evidence_score",
                                &relation_fusion.fusion_min_evidence_score);
            ParseOptionalScalar(fusion,
                                "fusion_persistence_window",
                                &relation_fusion.fusion_persistence_window);
            ParseOptionalScalar(fusion,
                                "fusion_warming_weight",
                                &relation_fusion.fusion_warming_weight);
            ParseOptionalScalar(fusion,
                                "fusion_degraded_weight",
                                &relation_fusion.fusion_degraded_weight);
            ParseOptionalScalar(fusion,
                                "fusion_support_weight",
                                &relation_fusion.fusion_support_weight);
            ParseOptionalScalar(fusion,
                                "fusion_oppose_weight",
                                &relation_fusion.fusion_oppose_weight);
            ParseOptionalScalar(fusion,
                                "basic_pattern_weight",
                                &relation_fusion.basic_pattern_weight);
            ParseOptionalScalar(fusion,
                                "stable_head_pattern_weight",
                                &relation_fusion.stable_head_pattern_weight);
            ParseOptionalScalar(fusion,
                                "dominant_single_cap",
                                &relation_fusion.dominant_single_cap);
            ParseOptionalScalar(fusion,
                                "dominant_pattern_cap",
                                &relation_fusion.dominant_pattern_cap);
            ParseOptionalScalar(fusion,
                                "fusion_state_ttl_buckets",
                                &relation_fusion.fusion_state_ttl_buckets);
            ParseOptionalScalar(fusion,
                                "fusion_state_max_sources",
                                &relation_fusion.fusion_state_max_sources);
            ParseOptionalScalar(fusion,
                                "fusion_state_cleanup_interval_updates",
                                &relation_fusion.fusion_state_cleanup_interval_updates);
            ParseOptionalScalar(fusion,
                                "fusion_state_cleanup_scan_limit",
                                &relation_fusion.fusion_state_cleanup_scan_limit);
            ParseOptionalScalar(fusion,
                                "fusion_persistence_max_keys_per_source",
                                &relation_fusion.fusion_persistence_max_keys_per_source);
        }
    }
}

void ParseCalendars(const YAML::Node& node, RuntimeConfigState* out) {
    if (!node || !out || !node.IsSequence()) return;
    out->calendars.clear();
    for (const auto& calendar_node : node) {
        if (!calendar_node || !calendar_node.IsMap()) continue;
        EventCalendarSpec spec;
        ParseOptionalScalar(calendar_node, "calendar_id", &spec.calendar_id);
        ParseOptionalScalar(calendar_node, "calendar_version", &spec.calendar_version);
        const YAML::Node entries_node = calendar_node["entries"];
        if (entries_node && entries_node.IsSequence()) {
            for (const auto& entry_node : entries_node) {
                if (!entry_node || !entry_node.IsMap()) continue;
                EventCalendarEntry entry;
                ParseOptionalScalar(entry_node, "event_code", &entry.event_code);
                ParseOptionalScalar(entry_node, "alignment_mode", &entry.alignment_mode);
                ParseOptionalScalar(entry_node, "start_ts", &entry.start_ts);
                ParseOptionalScalar(entry_node, "end_ts", &entry.end_ts);
                ParseOptionalScalar(entry_node, "enabled", &entry.enabled);
                ParseOptionalScalar(entry_node, "tz", &entry.tz);
                spec.entries.push_back(std::move(entry));
            }
        }

        CompiledEventCalendar compiled;
        std::string compile_err;
        BaselineTaskSpec empty_task;
        if (CompileEventCalendar(spec, empty_task, &compiled, &compile_err) == error::OK) {
            auto calendar = std::make_shared<CompiledEventCalendar>(std::move(compiled));
            out->calendars[CalendarKey(calendar->calendar_id, calendar->calendar_version)] =
                std::move(calendar);
        }
    }
}

bool ValidateAllowedKeys(const YAML::Node& node,
                         std::initializer_list<const char*> allowed,
                         const char* path,
                         std::string* err) {
    if (!node) return true;
    if (!node.IsMap()) {
        if (err) *err = std::string(path) + " must be an object";
        return false;
    }
    const std::unordered_set<std::string> allowed_keys(allowed.begin(), allowed.end());
    for (auto it = node.begin(); it != node.end(); ++it) {
        if (!it->first.IsScalar()) {
            if (err) *err = std::string(path) + " contains non-scalar key";
            return false;
        }
        const std::string key = it->first.as<std::string>();
        if (allowed_keys.find(key) != allowed_keys.end()) continue;
        if (err) *err = std::string(path) + "." + key + " is not allowed in strict mode";
        return false;
    }
    return true;
}

bool ValidateNamedProfileMapKeys(const YAML::Node& node,
                                 std::initializer_list<const char*> profile_allowed_keys,
                                 const char* path,
                                 std::string* err) {
    if (!node) return true;
    if (!node.IsMap()) {
        if (err) *err = std::string(path) + " must be an object";
        return false;
    }
    const std::unordered_set<std::string> allowed_keys(
        profile_allowed_keys.begin(), profile_allowed_keys.end());
    for (auto it = node.begin(); it != node.end(); ++it) {
        if (!it->first.IsScalar()) {
            if (err) *err = std::string(path) + " contains non-scalar profile name";
            return false;
        }
        const std::string profile_name = it->first.as<std::string>();
        const YAML::Node profile = it->second;
        if (!profile || !profile.IsMap()) {
            if (err) *err = std::string(path) + "." + profile_name + " must be an object";
            return false;
        }
        for (auto pit = profile.begin(); pit != profile.end(); ++pit) {
            if (!pit->first.IsScalar()) {
                if (err) {
                    *err = std::string(path) + "." + profile_name +
                           " contains non-scalar key";
                }
                return false;
            }
            const std::string key = pit->first.as<std::string>();
            if (allowed_keys.find(key) != allowed_keys.end()) continue;
            if (err) {
                *err = std::string(path) + "." + profile_name + "." + key +
                       " is not allowed in strict mode";
            }
            return false;
        }
    }
    return true;
}

bool ValidateStrictDefaultsSchema(const YAML::Node& defaults, std::string* err) {
    if (!ValidateAllowedKeys(defaults,
                             {"parser",
                              "shared_profile_config",
                              "bootstrap",
                              "rolling_config",
                              "value_sampled_profiles",
                              "ratio_profiles",
                              "solver_constants"},
                             "baseline",
                             err)) {
        return false;
    }
    if (!ValidateAllowedKeys(defaults["parser"], {"tz_default"}, "baseline.parser", err)) {
        return false;
    }
    if (!ValidateAllowedKeys(defaults["shared_profile_config"],
                             {"daily_harmonic_order",
                              "weekly_harmonic_order",
                              "dme_max",
                              "m_month_enable",
                              "month_cov_min",
                              "lambda_season",
                              "lambda_dom",
                              "lambda_dme",
                              "lambda_lwd",
                              "lambda_event"},
                             "shared_profile_config",
                             err)) {
        return false;
    }
    const YAML::Node bootstrap = defaults["bootstrap"];
    if (bootstrap) {
        if (!ValidateAllowedKeys(bootstrap,
                                 {"seed_quality",
                                  "prediction_band",
                                  "export_seed",
                                  "min_train_points"},
                                 "bootstrap",
                                 err)) {
            return false;
        }
        if (!ValidateAllowedKeys(bootstrap["seed_quality"],
                                 {"full_min_coverage_ratio",
                                  "partial_min_coverage_ratio",
                                  "daily_min_span_days",
                                  "weekly_min_span_days",
                                  "daily_phase_coverage_ratio",
                                  "weekly_phase_coverage_ratio"},
                                 "bootstrap.seed_quality",
                                 err)) {
            return false;
        }
        if (!ValidateAllowedKeys(bootstrap["prediction_band"],
                                 {"z_value", "sigma_floor", "low_maturity_band_scale"},
                                 "bootstrap.prediction_band",
                                 err)) {
            return false;
        }
        if (!ValidateAllowedKeys(bootstrap["export_seed"],
                                 {"include_monthpos_when_ready",
                                  "include_event_when_ready",
                                  "include_relation_basis_when_ready"},
                                 "bootstrap.export_seed",
                                 err)) {
            return false;
        }
    }
    if (defaults["rolling_config"] && defaults["rolling_config"]["relation_rolling"]) {
        if (!ValidateAllowedKeys(defaults["rolling_config"]["relation_rolling"],
                                 {"enable_routed_rolling",
                                  "enable_stream_basis",
                                  "include_universal_summaries_without_basis",
                                  "basis_stats_max_groups",
                                  "basis_collect_min_buckets",
                                  "basis_ready_min_buckets",
                                  "basis_refresh_interval_buckets",
                                  "basis_candidate_min_coverage_ratio",
                                  "basis_replacement_cap_ratio",
                                  "basis_replacement_cap_max",
                                  "basis_handover_warmup_buckets",
                                  "basis_threshold_margin",
                                  "basis_min_stable_refresh_count",
                                  "routed_state_shard_count",
                                  "relation_fusion"},
                                 "rolling_config.relation_rolling",
                                 err)) {
            return false;
        }
        if (!ValidateAllowedKeys(
                defaults["rolling_config"]["relation_rolling"]["relation_fusion"],
                {"enable_relation_fusion",
                 "fusion_z_score_cap",
                 "fusion_min_evidence_score",
                 "fusion_persistence_window",
                 "fusion_warming_weight",
                 "fusion_degraded_weight",
                 "fusion_support_weight",
                 "fusion_oppose_weight",
                 "basic_pattern_weight",
                 "stable_head_pattern_weight",
                 "dominant_single_cap",
                 "dominant_pattern_cap",
                 "fusion_state_ttl_buckets",
                 "fusion_state_max_sources",
                 "fusion_state_cleanup_interval_updates",
                 "fusion_state_cleanup_scan_limit",
                 "fusion_persistence_max_keys_per_source"},
                "rolling_config.relation_rolling.relation_fusion",
                err)) {
            return false;
        }
    }
    if (!ValidateAllowedKeys(defaults["rolling_config"],
                             {"n_min_score",
                              "n_min_update",
                              "n_ref",
                              "sample_count_noise",
                              "d_min_score",
                              "d_min_update",
                              "d_ref",
                              "ratio_denominator_noise",
                              "z_downweight",
                              "z_skip",
                              "small_update_weight",
                              "level_learning_scale",
                              "day_learning_scale",
                              "week_learning_scale",
                              "cold_day_learning_scale",
                              "cold_week_learning_scale",
                              "seasonal_drift_min_scale",
                              "full_seed_seasonal_scale",
                              "partial_seed_seasonal_scale",
                              "freeze_seeded_seasonal_on_drift",
                              "day_delta_coeff_max",
                              "day_delta_coeff_max_scale",
                              "week_delta_coeff_max",
                              "week_delta_coeff_max_scale",
                              "Q_day",
                              "q_day_scale",
                              "Q_week",
                              "q_week_scale",
                              "Q_level",
                              "q_level_scale",
                              "Q_trend",
                              "q_trend_scale",
                              "trend_update_scale",
                              "cold_trend_update_scale",
                              "trend_delta_max",
                              "trend_delta_max_scale",
                              "trend_abs_max",
                              "trend_abs_max_scale",
                              "P_level_init",
                              "p_level_init_scale",
                              "P_trend_init",
                              "p_trend_init_scale",
                              "P_day_init",
                              "p_day_init_scale",
                              "P_week_init",
                              "p_week_init_scale",
                              "P_floor",
                              "p_floor_scale",
                              "P_cap",
                              "p_cap_scale",
                              "alpha_short",
                              "alpha_long",
                              "z_cap",
                              "drift_start",
                              "drift_full",
                              "level_shift_reference_z",
                              "level_shift_cusum_decay",
                              "level_shift_cusum_threshold",
                              "max_level_boost",
                              "max_q_boost",
                              "skip_relax",
                              "process_noise_gap_cap_buckets",
                              "alpha_sigma",
                              "c_sigma",
                              "sigma_floor",
                              "cold_start_band_scale",
                              "band_z",
                              "forecast_band_z",
                              "confidence_cold",
                              "confidence_warming",
                              "confidence_ready_hint_cap",
                              "min_warming_updates",
                              "min_ready_hint_updates",
                              "level_ready_min_updates",
                              "score_warming_min_updates",
                              "score_ready_min_updates",
                              "score_recovery_min_updates",
                              "score_drift_degrade_start",
                              "calibration_alpha",
                              "calibration_warmup_min_updates",
                              "calibration_coverage_floor",
                              "calibration_tail3_limit",
                              "calibration_tail5_limit",
                              "calibration_multiplier_min",
                              "calibration_multiplier_max",
                              "daily_coverage_bins",
                              "weekly_coverage_bins",
                              "daily_ready_min_days",
                              "daily_ready_coverage_ratio",
                              "weekly_ready_min_weeks",
                              "weekly_ready_coverage_ratio",
                              "maturity_uncertainty_cold_scale",
                              "maturity_uncertainty_warming_scale",
                              "maturity_uncertainty_drift_scale",
                              "maturity_uncertainty_recalibrating_scale",
                              "missing_daily_uncertainty_scale",
                              "missing_weekly_uncertainty_scale",
                              "level_only_extreme_z",
                              "detection_band_std_cap",
                              "forecast_trend_cap_buckets",
                              "monthpos_alpha",
                              "monthpos_delta_max_scale",
                              "monthpos_min_month_transitions",
                              "monthpos_ready_coverage_ratio",
                              "relation_rolling"},
                             "rolling_config",
                             err)) {
        return false;
    }
    if (!ValidateNamedProfileMapKeys(
            defaults["value_sampled_profiles"],
            {"n_train_min", "transform_name_override"},
            "value_sampled_profiles",
            err)) {
        return false;
    }
    if (defaults["ratio_profiles"]) {
        if (!defaults["ratio_profiles"].IsMap()) {
            if (err) *err = "ratio_profiles must be an object";
            return false;
        }
        for (auto it = defaults["ratio_profiles"].begin();
             it != defaults["ratio_profiles"].end();
             ++it) {
            if (!it->first.IsScalar()) {
                if (err) *err = "ratio_profiles contains non-scalar profile name";
                return false;
            }
            const std::string profile_name = it->first.as<std::string>();
            const std::string profile_path = std::string("ratio_profiles.") + profile_name;
            if (profile_name == "global") {
                if (!ValidateAllowedKeys(it->second,
                                         {"eps_logit", "m_floor", "v_floor"},
                                         profile_path.c_str(),
                                         err)) {
                    return false;
                }
                continue;
            }
            if (!ValidateAllowedKeys(it->second,
                                     {"d_min_train", "s_prior", "phi_over"},
                                     profile_path.c_str(),
                                     err)) {
                return false;
            }
        }
    }
    return ValidateAllowedKeys(defaults["solver_constants"],
                               {"solver_name",
                                "c_huber",
                                "s_min_fit",
                                "max_iter_fit",
                                "tol_obj_rel",
                                "tol_beta_inf",
                                "cond_max"},
                               "solver_constants",
                               err);
}

bool ValidateCalendarsSchema(const YAML::Node& calendars, std::string* err) {
    if (!calendars) return true;
    if (!calendars.IsSequence()) {
        if (err) *err = "calendars must be an array";
        return false;
    }
    for (std::size_t i = 0; i < calendars.size(); ++i) {
        const YAML::Node calendar = calendars[i];
        const std::string calendar_path = "calendars[" + std::to_string(i) + "]";
        if (!ValidateAllowedKeys(calendar,
                                 {"calendar_id", "calendar_version", "entries"},
                                 calendar_path.c_str(),
                                 err)) {
            return false;
        }
        if (!calendar["calendar_id"] || !calendar["calendar_version"]) {
            if (err) *err = calendar_path + " requires calendar_id and calendar_version";
            return false;
        }
        const YAML::Node entries = calendar["entries"];
        if (!entries) continue;
        if (!entries.IsSequence()) {
            if (err) *err = calendar_path + ".entries must be an array";
            return false;
        }
        for (std::size_t j = 0; j < entries.size(); ++j) {
            const std::string entry_path =
                calendar_path + ".entries[" + std::to_string(j) + "]";
            if (!ValidateAllowedKeys(entries[j],
                                     {"event_code",
                                      "alignment_mode",
                                      "start_ts",
                                      "end_ts",
                                      "enabled",
                                      "tz"},
                                     entry_path.c_str(),
                                     err)) {
                return false;
            }
        }
    }
    return true;
}

bool RejectLegacyHarmonicConfig(const YAML::Node& defaults, std::string* err) {
    const YAML::Node shared = defaults["shared_profile_config"];
    if (shared && shared.IsMap()) {
        if (shared["k_day"]) {
            if (err) {
                *err = "shared_profile_config.k_day is no longer supported; "
                       "use shared_profile_config.daily_harmonic_order";
            }
            return false;
        }
        if (shared["k_week"]) {
            if (err) {
                *err = "shared_profile_config.k_week is no longer supported; "
                       "use shared_profile_config.weekly_harmonic_order";
            }
            return false;
        }
    }

    const YAML::Node rolling = defaults["rolling_config"];
    if (rolling && rolling.IsMap()) {
        if (rolling["daily_harmonic_order"]) {
            if (err) {
                *err = "rolling_config.daily_harmonic_order is no longer supported; "
                       "use shared_profile_config.daily_harmonic_order";
            }
            return false;
        }
        if (rolling["weekly_harmonic_order"]) {
            if (err) {
                *err = "rolling_config.weekly_harmonic_order is no longer supported; "
                       "use shared_profile_config.weekly_harmonic_order";
            }
            return false;
        }
    }
    return true;
}

bool ValidateRuntimeConfig(const RuntimeConfigState& cfg, std::string* err) {
    if (cfg.tz_default.empty()) {
        if (err) *err = "baseline.parser.tz_default must not be empty";
        return false;
    }
    if (cfg.shared_profile.k_day < 0 ||
        cfg.shared_profile.k_week < 0 ||
        cfg.shared_profile.dme_max < 0) {
        if (err) *err = "shared_profile_config integer fields are invalid";
        return false;
    }
    if (!(cfg.shared_profile.month_cov_min > 0.0 &&
          cfg.shared_profile.month_cov_min <= 1.0)) {
        if (err) *err = "shared_profile_config.month_cov_min must be in (0,1]";
        return false;
    }
    if (cfg.value_sampled_profiles.empty()) {
        if (err) *err = "value_sampled_profiles must not be empty";
        return false;
    }
    for (const auto& entry : cfg.value_sampled_profiles) {
        if (entry.first.empty() || entry.second.n_train_min == 0) {
            if (err) *err = "value_sampled_profiles contains invalid profile";
            return false;
        }
    }
    if (cfg.ratio_profiles.empty()) {
        if (err) *err = "ratio_profiles must not be empty";
        return false;
    }
    if (!(cfg.ratio_global.eps_logit > 0.0 && cfg.ratio_global.eps_logit < 0.5) ||
        !(cfg.ratio_global.m_floor > 0.0 && cfg.ratio_global.m_floor < 0.5) ||
        cfg.ratio_global.v_floor <= 0.0) {
        if (err) *err = "ratio_profiles.global numerical fields are invalid";
        return false;
    }
    for (const auto& entry : cfg.ratio_profiles) {
        if (entry.first.empty() ||
            entry.second.d_min_train == 0 ||
            entry.second.s_prior < 0.0 ||
            entry.second.phi_over < 1.0) {
            if (err) *err = "ratio_profiles contains invalid profile";
            return false;
        }
    }
    if (cfg.block_solver.solver_name.empty() ||
        cfg.block_solver.c_huber <= 0.0 ||
        cfg.block_solver.s_min_fit <= 0.0 ||
        cfg.block_solver.max_iter_fit == 0 ||
        cfg.block_solver.tol_obj_rel <= 0.0 ||
        cfg.block_solver.tol_beta_inf <= 0.0 ||
        cfg.block_solver.cond_max <= 0.0) {
        if (err) *err = "solver_constants are invalid";
        return false;
    }
    if (ValidateBaselineRollingConfig(cfg.rolling_config, err) != BaselineStatus::kOk) {
        return false;
    }
    if (!(cfg.seed_quality.full_min_coverage_ratio > 0.0 &&
          cfg.seed_quality.full_min_coverage_ratio <= 1.0) ||
        !(cfg.seed_quality.partial_min_coverage_ratio > 0.0 &&
          cfg.seed_quality.partial_min_coverage_ratio <= 1.0) ||
        cfg.seed_quality.partial_min_coverage_ratio >
            cfg.seed_quality.full_min_coverage_ratio ||
        cfg.seed_quality.daily_min_span_days == 0 ||
        cfg.seed_quality.weekly_min_span_days == 0 ||
        cfg.seed_quality.weekly_min_span_days < cfg.seed_quality.daily_min_span_days ||
        !(cfg.seed_quality.daily_phase_coverage_ratio > 0.0 &&
          cfg.seed_quality.daily_phase_coverage_ratio <= 1.0) ||
        !(cfg.seed_quality.weekly_phase_coverage_ratio > 0.0 &&
          cfg.seed_quality.weekly_phase_coverage_ratio <= 1.0)) {
        if (err) *err = "bootstrap.seed_quality fields are invalid";
        return false;
    }
    for (const auto& entry : cfg.calendars) {
        if (!entry.second ||
            entry.second->calendar_id.empty() ||
            entry.second->calendar_version.empty()) {
            if (err) *err = "calendars contains invalid calendar";
            return false;
        }
    }
    return true;
}

bool ApplyYamlConfig(const YAML::Node& root,
                     bool strict,
                     RuntimeConfigState* out,
                     std::string* err) {
    if (!out) {
        if (err) *err = "runtime config output must not be null";
        return false;
    }
    if (strict &&
        !ValidateAllowedKeys(root, {"baseline", "calendars", "examples", "notes"}, "root", err)) {
        return false;
    }
    if (strict && !ValidateCalendarsSchema(root["calendars"], err)) return false;

    YAML::Node defaults = root["baseline"];
    if (!defaults) {
        if (strict) {
            if (err) *err = "missing object field: baseline";
            return false;
        }
        defaults = root;
    }
    if (!defaults || !defaults.IsMap()) {
        if (err) *err = "baseline must be an object";
        return false;
    }
    if (!RejectLegacyHarmonicConfig(defaults, err)) return false;
    if (strict && !ValidateStrictDefaultsSchema(defaults, err)) return false;

    if (defaults["parser"]) {
        ParseOptionalScalar(defaults["parser"], "tz_default", &out->tz_default);
    }
    ParseSharedProfileConfig(defaults["shared_profile_config"], &out->shared_profile);
    ParseValueSampledProfiles(defaults["value_sampled_profiles"], out);
    ParseRatioProfiles(defaults["ratio_profiles"], out);
    ParseBootstrapConfig(defaults["bootstrap"], &out->seed_quality);
    ParseRollingConfig(defaults["rolling_config"], &out->rolling_config);
    ParseBlockSolverConfig(defaults["solver_constants"], &out->block_solver);
    ParseCalendars(root["calendars"], out);
    return true;
}

}  // namespace

int LoadBaselineRuntimeConfigFromYaml(const std::string& file_path,
                                      bool strict,
                                      std::string* err) {
    if (file_path.empty()) {
        ResetBaselineRuntimeConfig();
        return error::OK;
    }

    RuntimeConfigState next = BuildDefaultRuntimeConfigState();
    try {
        const YAML::Node root = YAML::LoadFile(file_path);
        if (!ApplyYamlConfig(root, strict, &next, err)) return error::BAD_REQUEST;
    } catch (const YAML::Exception& ex) {
        if (err) *err = ex.what();
        return error::BAD_REQUEST;
    } catch (const std::exception& ex) {
        if (err) *err = ex.what();
        return error::BAD_REQUEST;
    } catch (...) {
        if (err) *err = "unknown exception while loading baseline runtime config";
        return error::BAD_REQUEST;
    }

    if (!ValidateRuntimeConfig(next, err)) return error::BAD_REQUEST;
    StoreSnapshot(next);
    return error::OK;
}

void ResetBaselineRuntimeConfig() {
    StoreSnapshot(BuildDefaultRuntimeConfigState());
}

bool TryGetSharedProfileConfigOverride(SharedProfileConfig* out) {
    if (!out) return false;
    const auto snapshot = Snapshot();
    *out = snapshot->shared_profile;
    return true;
}

bool TryGetValueSampledProfileConfigOverride(const std::string& profile_name,
                                             ValueSampledProfileConfig* out) {
    if (!out || profile_name.empty()) return false;
    const auto snapshot = Snapshot();
    const auto& profiles = snapshot->value_sampled_profiles;
    const auto it = profiles.find(profile_name);
    if (it == profiles.end()) return false;
    *out = it->second;
    return true;
}

bool TryGetRatioProfileConfigOverride(const std::string& profile_name,
                                      RatioProfileConfig* out) {
    if (!out || profile_name.empty()) return false;
    const auto snapshot = Snapshot();
    const auto& profiles = snapshot->ratio_profiles;
    const auto it = profiles.find(profile_name);
    if (it == profiles.end()) return false;
    *out = it->second;
    return true;
}

bool TryGetRatioGlobalNumericalOverride(double* eps_logit,
                                        double* m_floor,
                                        double* v_floor) {
    const auto snapshot = Snapshot();
    const RatioGlobalNumericalConfig& global = snapshot->ratio_global;
    if (eps_logit) *eps_logit = global.eps_logit;
    if (m_floor) *m_floor = global.m_floor;
    if (v_floor) *v_floor = global.v_floor;
    return true;
}

bool TryGetBlockSolverConfigOverride(BlockSolverConfig* out) {
    if (!out) return false;
    const auto snapshot = Snapshot();
    *out = snapshot->block_solver;
    return true;
}

bool TryGetBaselineRollingConfigOverride(BaselineRollingConfig* out) {
    if (!out) return false;
    const auto snapshot = Snapshot();
    *out = snapshot->rolling_config;
    out->ratio_eps_logit = snapshot->ratio_global.eps_logit;
    return true;
}

bool TryGetBootstrapSeedQualityConfigOverride(BootstrapSeedQualityConfig* out) {
    if (!out) return false;
    const auto snapshot = Snapshot();
    *out = snapshot->seed_quality;
    return true;
}

std::string BaselineDefaultTimezone() {
    const auto snapshot = Snapshot();
    return snapshot->tz_default;
}

std::shared_ptr<const CompiledEventCalendar> FindBaselineEventCalendar(
    const BaselineCalendarRef& calendar_ref) {
    if (calendar_ref.calendar_id.empty() || calendar_ref.calendar_version.empty()) {
        return nullptr;
    }
    const auto snapshot = Snapshot();
    const auto it = snapshot->calendars.find(
        CalendarKey(calendar_ref.calendar_id, calendar_ref.calendar_version));
    return it == snapshot->calendars.end() ? nullptr : it->second;
}

}  // namespace baseline
}  // namespace flowsql
