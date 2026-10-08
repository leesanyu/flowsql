// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_PLUGINS_BASELINE_ROLLING_ROLLING_CONFIG_H_
#define _FLOWSQL_PLUGINS_BASELINE_ROLLING_ROLLING_CONFIG_H_

#include <framework/interfaces/ibaseline_types.h>

#include <cstdint>
#include <string>

#include "plugins/baseline/model/profile_config.h"
#include "plugins/baseline/model/task_spec.h"

namespace flowsql {
namespace baseline {

struct BaselineRelationFusionConfig {
    bool enable_relation_fusion = true;
    double fusion_z_score_cap = 5.0;
    double fusion_min_evidence_score = 0.20;
    uint32_t fusion_persistence_window = 2;
    double fusion_warming_weight = 0.25;
    double fusion_degraded_weight = 0.25;
    double fusion_support_weight = 0.50;
    double fusion_oppose_weight = 0.50;
    double basic_pattern_weight = 0.70;
    double stable_head_pattern_weight = 0.85;
    uint32_t dominant_single_cap = 3;
    uint32_t dominant_pattern_cap = 2;
    uint64_t fusion_state_ttl_buckets = 20160;
    uint64_t fusion_state_max_sources = 4096;
    uint64_t fusion_state_cleanup_interval_updates = 512;
    uint64_t fusion_state_cleanup_scan_limit = 256;
    uint64_t fusion_persistence_max_keys_per_source = 512;
};

struct BaselineRelationRollingConfig {
    bool enable_routed_rolling = true;
    bool enable_stream_basis = true;
    bool include_universal_summaries_without_basis = true;
    uint64_t basis_stats_max_groups = 256;
    uint64_t basis_collect_min_buckets = 0;
    uint64_t basis_ready_min_buckets = 0;
    uint64_t basis_refresh_interval_buckets = 0;
    double basis_candidate_min_coverage_ratio = 0.60;
    double basis_replacement_cap_ratio = 0.20;
    uint32_t basis_replacement_cap_max = 2;
    uint64_t basis_handover_warmup_buckets = 0;
    double basis_threshold_margin = 1.20;
    uint64_t basis_min_stable_refresh_count = 2;
    uint32_t routed_state_shard_count = 16;
    BaselineRelationFusionConfig relation_fusion;
};

struct BaselineRollingConfig {
    // n_min_score 是最低可评分样本量；n_ref 是达到满权重的样本量，必须 >= n_min_score。
    uint32_t n_min_score = 3;
    uint32_t n_min_update = 10;
    uint32_t n_ref = 10;
    double sample_count_noise = 1.0;

    // d_min_score 是 ratio 最低可评分分母；d_ref 是达到满权重的分母，必须 >= d_min_score。
    uint32_t d_min_score = 10;
    uint32_t d_min_update = 100;
    uint32_t d_ref = 100;
    double ratio_denominator_noise = 1.0;

    double z_downweight = 3.0;
    double z_skip = 5.0;
    double small_update_weight = 0.2;

    // Resolved from shared_profile_config by ResolveBaselineRollingConfig().
    int32_t daily_harmonic_order = 0;
    int32_t weekly_harmonic_order = 0;

    double level_learning_scale = 1.0;
    double day_learning_scale = 0.2;
    double week_learning_scale = 0.05;
    double cold_day_learning_scale = 0.05;
    double cold_week_learning_scale = 0.01;
    double seasonal_drift_min_scale = 0.1;
    double full_seed_seasonal_scale = 0.1;
    double partial_seed_seasonal_scale = 0.5;
    bool freeze_seeded_seasonal_on_drift = true;

    double day_delta_coeff_max_scale = 0.05;
    double week_delta_coeff_max_scale = 0.02;

    double q_day_scale = 1.0e-4;
    double q_week_scale = 2.0e-5;
    double q_level_scale = 1.0e-3;
    double q_trend_scale = 1.0e-5;

    double trend_update_scale = 0.05;
    double cold_trend_update_scale = 0.0;
    double trend_delta_max_scale = 0.01;
    double trend_abs_max_scale = 0.1;

    double p_level_init_scale = 9.0;
    double p_trend_init_scale = 16.0;
    double p_day_init_scale = 4.0;
    double p_week_init_scale = 9.0;
    // P floor/cap 的缩放基准是 sigma_floor^2，不随当前 sigma 动态变化。
    double p_floor_scale = 1.0e-6;
    double p_cap_scale = 1.0e6;

    double alpha_short = 0.05;
    double alpha_long = 0.005;
    double z_cap = 5.0;
    double drift_start = 1.0;
    double drift_full = 2.0;
    double level_shift_reference_z = 0.5;
    double level_shift_cusum_decay = 0.98;
    double level_shift_cusum_threshold = 16.0;
    double max_level_boost = 4.0;
    double max_q_boost = 9.0;
    double skip_relax = 2.0;

    // 0 表示自动派生；Resolve 后为 7 * day_buckets。
    uint64_t process_noise_gap_cap_buckets = 0;

    double alpha_sigma = 0.02;
    double c_sigma = 3.0;
    double sigma_floor = 0.05;
    double cold_start_band_scale = 3.0;
    double band_z = 1.96;
    double forecast_band_z = 3.0;
    double confidence_cold = 0.2;
    double confidence_warming = 0.5;
    double confidence_ready_hint_cap = 0.8;
    uint64_t min_warming_updates = 10;
    // 0 表示自动派生；Resolve 后为 day_buckets。
    uint64_t min_ready_hint_updates = 0;

    uint64_t level_ready_min_updates = 30;
    uint64_t score_warming_min_updates = 60;
    uint64_t score_ready_min_updates = 240;
    uint64_t score_recovery_min_updates = 120;
    double score_drift_degrade_start = 1.5;

    double calibration_alpha = 0.02;
    uint64_t calibration_warmup_min_updates = 60;
    double calibration_coverage_floor = 0.98;
    double calibration_tail3_limit = 0.02;
    double calibration_tail5_limit = 0.002;
    double calibration_multiplier_min = 1.0;
    double calibration_multiplier_max = 6.0;

    // 覆盖率 bin 是周期覆盖统计的聚合粒度，默认等价于按小时统计日/周覆盖。
    uint32_t daily_coverage_bins = 24;
    uint32_t weekly_coverage_bins = 168;
    uint64_t daily_ready_min_days = 2;
    double daily_ready_coverage_ratio = 0.75;
    uint64_t weekly_ready_min_weeks = 2;
    double weekly_ready_coverage_ratio = 0.70;

    // uncertainty scale 会以 sigma^2 为基准形成诊断方差，成熟度和缺失组件两类可叠加；
    // 当前只进入 diagnostics/result 字段，不直接进入 detection band 方差。
    double maturity_uncertainty_cold_scale = 9.0;
    double maturity_uncertainty_warming_scale = 4.0;
    double maturity_uncertainty_drift_scale = 4.0;
    double maturity_uncertainty_recalibrating_scale = 2.0;
    double missing_daily_uncertainty_scale = 9.0;
    double missing_weekly_uncertainty_scale = 4.0;
    double level_only_extreme_z = 8.0;
    double detection_band_std_cap = 0.5;
    // 0 表示自动派生；Resolve 后为 day_buckets。
    uint64_t forecast_trend_cap_buckets = 0;

    double monthpos_alpha = 0.005;
    double monthpos_delta_max_scale = 0.5;
    uint64_t monthpos_min_month_transitions = 2;
    double monthpos_ready_coverage_ratio = 0.60;

    int64_t bucket_seconds = 60;
    std::string timezone = "UTC";
    uint64_t day_buckets = 1440;
    uint64_t week_buckets = 10080;
    BaselineRelationRollingConfig relation_rolling;
    // Derived from ratio_profiles.global in the same runtime configuration snapshot.
    double ratio_eps_logit = kRatioEpsLogit;
};

BaselineRollingConfig DefaultBaselineRollingConfig();

BaselineStatus ValidateBaselineRollingConfig(const BaselineRollingConfig& config,
                                             std::string* err = nullptr);

BaselineStatus __attribute__((visibility("default"))) ResolveBaselineRollingConfig(
    const BaselineTaskSpec& spec,
    BaselineRollingConfig* out,
    std::string* err = nullptr);

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_ROLLING_ROLLING_CONFIG_H_
