// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <plugins/baseline/bootstrap/bootstrap_types.h>
#include <plugins/baseline/model/event_calendar_matcher.h>
#include <plugins/baseline/model/formal_model.h>
#include <plugins/baseline/model/profile_config.h>
#include <plugins/baseline/model/task_spec.h>
#include <plugins/baseline/relation/relation_basis.h>
#include <plugins/baseline/relation/relation_basis_state.h>
#include <plugins/baseline/relation/relation_fusion.h>
#include <plugins/baseline/rolling/rolling_config.h>
#include <plugins/baseline/rolling/rolling_state.h>
#include <plugins/baseline/solver/solver_backend.h>

namespace flowsql::baseline::checkpoint {
// Explicit native field schema. Update its version when changing serialized fields.
template <class T>
struct Fields;
template <>
struct Fields<BootstrapCoverageReport> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("accepted_count", v.accepted_count);
        a("rejected_count", v.rejected_count);
        a("train_start_bucket", v.train_start_bucket);
        a("train_end_bucket", v.train_end_bucket);
        a("coverage_ratio", v.coverage_ratio);
    }
};
template <>
struct Fields<BootstrapTaskIdentity> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("task_id", v.task_id);
        a("task_kind", v.task_kind);
        a("feature_type", v.feature_type);
        a("feature_id", v.feature_id);
        a("profile", v.profile);
    }
};
template <>
struct Fields<BootstrapClockSpec> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("bucket_seconds", v.bucket_seconds);
        a("timezone", v.timezone);
    }
};
template <>
struct Fields<BootstrapCalendarRef> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("calendar_id", v.calendar_id);
        a("calendar_version", v.calendar_version);
    }
};
template <>
struct Fields<BootstrapHarmonicInit> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("order", v.order);
        a("sin", v.sin);
        a("cos", v.cos);
    }
};
template <>
struct Fields<BootstrapThetaInit> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("available", v.available);
        a("model_space", v.model_space);
        a("reference_bucket_id", v.reference_bucket_id);
        a("level", v.level);
        a("trend", v.trend);
        a("daily_harmonic", v.daily_harmonic);
        a("weekly_harmonic", v.weekly_harmonic);
    }
};
template <>
struct Fields<BootstrapSigmaInit> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("available", v.available);
        a("value", v.value);
        a("model_space", v.model_space);
        a("source", v.source);
    }
};
template <>
struct Fields<BootstrapRatioPriorInit> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("available", v.available);
        a("m0", v.m0);
        a("alpha0", v.alpha0);
        a("beta0", v.beta0);
        a("model_space", v.model_space);
        a("source", v.source);
    }
};
template <>
struct Fields<BootstrapMonthPosHint> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("available", v.available);
        a("dom_coeff", v.dom_coeff);
        a("dme_coeff", v.dme_coeff);
        a("lwd_coeff", v.lwd_coeff);
        a("dom_center", v.dom_center);
        a("dme_center", v.dme_center);
        a("lwd_center", v.lwd_center);
    }
};
template <>
struct Fields<BootstrapEventHint> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("available", v.available);
        a("calendar_id", v.calendar_id);
        a("calendar_version", v.calendar_version);
        a("active_event_codes", v.active_event_codes);
        a("coeff", v.coeff);
    }
};
template <>
struct Fields<BootstrapSeedQualityConfig> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("full_min_coverage_ratio", v.full_min_coverage_ratio);
        a("partial_min_coverage_ratio", v.partial_min_coverage_ratio);
        a("daily_min_span_days", v.daily_min_span_days);
        a("weekly_min_span_days", v.weekly_min_span_days);
        a("daily_phase_coverage_ratio", v.daily_phase_coverage_ratio);
        a("weekly_phase_coverage_ratio", v.weekly_phase_coverage_ratio);
    }
};
template <>
struct Fields<BootstrapComponentUncertaintyInit> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("level_scale", v.level_scale);
        a("trend_scale", v.trend_scale);
        a("daily_scale", v.daily_scale);
        a("weekly_scale", v.weekly_scale);
    }
};
template <>
struct Fields<BootstrapUncertaintyInit> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("available", v.available);
        a("confidence_base", v.confidence_base);
        a("confidence_level", v.confidence_level);
        a("coverage_ratio", v.coverage_ratio);
        a("band_z", v.band_z);
        a("band_source", v.band_source);
        a("uncertainty_source", v.uncertainty_source);
        a("component_uncertainty", v.component_uncertainty);
    }
};
template <>
struct Fields<BootstrapMaturityInit> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("available", v.available);
        a("seed_status", v.seed_status);
        a("confidence", v.confidence);
        a("accepted_count", v.accepted_count);
        a("rejected_count", v.rejected_count);
        a("coverage_ratio", v.coverage_ratio);
    }
};
template <>
struct Fields<BootstrapRelationBasisSeed> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("basis_version", v.basis_version);
        a("feature_base", v.feature_base);
        a("metric_name", v.metric_name);
        a("group_space_id", v.group_space_id);
        a("group_space_version", v.group_space_version);
        a("k_head", v.k_head);
        a("other_group_idxs", v.other_group_idxs);
        a("support_explicit", v.support_explicit);
        a("stable_head", v.stable_head);
        a("head_proto_q", v.head_proto_q);
    }
};
template <>
struct Fields<RelationRoutedBootstrapSeed> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("metric_name", v.metric_name);
        a("summary_name", v.summary_name);
        a("task_kind", v.task_kind);
        a("basis_version", v.basis_version);
        a("basis_scoped", v.basis_scoped);
        a("task_identity", v.task_identity);
        a("clock_spec", v.clock_spec);
        a("calendar_ref", v.calendar_ref);
        a("coverage_report", v.coverage_report);
        a("seed_status", v.seed_status);
        a("seeded_components", v.seeded_components);
        a("enabled_components", v.enabled_components);
        a("theta_init", v.theta_init);
        a("monthpos_hint", v.monthpos_hint);
        a("event_hint", v.event_hint);
        a("sigma_init", v.sigma_init);
        a("ratio_prior_init", v.ratio_prior_init);
        a("uncertainty_init", v.uncertainty_init);
        a("maturity_init", v.maturity_init);
    }
};
template <>
struct Fields<RelationFusionSummaryMetadata> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("metric_name", v.metric_name);
        a("summary_name", v.summary_name);
        a("task_kind", v.task_kind);
        a("basis_scoped", v.basis_scoped);
        a("basis_version", v.basis_version);
    }
};
template <>
struct Fields<RelationFusionPatternMetadata> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("pattern", v.pattern);
        a("scope", v.scope);
        a("pattern_weight", v.pattern_weight);
        a("required_summaries", v.required_summaries);
        a("optional_summaries", v.optional_summaries);
        a("oppose_summaries", v.oppose_summaries);
        a("metrics", v.metrics);
    }
};
template <>
struct Fields<RelationFusionBootstrapMetadata> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("metadata_version", v.metadata_version);
        a("feature_base", v.feature_base);
        a("summary_metadata", v.summary_metadata);
        a("pattern_metadata", v.pattern_metadata);
    }
};
template <>
struct Fields<BootstrapSeed> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("artifact_kind", v.artifact_kind);
        a("seed_status", v.seed_status);
        a("source_artifact_version", v.source_artifact_version);
        a("series_key", v.series_key);
        a("task_identity", v.task_identity);
        a("clock_spec", v.clock_spec);
        a("calendar_ref", v.calendar_ref);
        a("coverage_report", v.coverage_report);
        a("seeded_components", v.seeded_components);
        a("enabled_components", v.enabled_components);
        a("theta_init", v.theta_init);
        a("monthpos_hint", v.monthpos_hint);
        a("event_hint", v.event_hint);
        a("sigma_init", v.sigma_init);
        a("ratio_prior_init", v.ratio_prior_init);
        a("uncertainty_init", v.uncertainty_init);
        a("maturity_init", v.maturity_init);
        a("relation_basis_by_metric", v.relation_basis_by_metric);
        a("relation_routed_summary_seeds", v.relation_routed_summary_seeds);
        a("relation_fusion_metadata", v.relation_fusion_metadata);
        a("diagnostics", v.diagnostics);
    }
};
template <>
struct Fields<RelationRoutedBootstrapArtifact> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("metric_name", v.metric_name);
        a("summary_name", v.summary_name);
        a("task_kind", v.task_kind);
        a("basis_version", v.basis_version);
        a("basis_scoped", v.basis_scoped);
        a("task_identity", v.task_identity);
        a("clock_spec", v.clock_spec);
        a("calendar_ref", v.calendar_ref);
        a("coverage_report", v.coverage_report);
        a("seeded_components", v.seeded_components);
        a("enabled_components", v.enabled_components);
        a("value_model", v.value_model);
        a("ratio_model", v.ratio_model);
        a("diagnostics", v.diagnostics);
    }
};
template <>
struct Fields<BootstrapArtifact> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("artifact_kind", v.artifact_kind);
        a("train_status", v.train_status);
        a("model_version", v.model_version);
        a("series_key", v.series_key);
        a("task_identity", v.task_identity);
        a("clock_spec", v.clock_spec);
        a("calendar_ref", v.calendar_ref);
        a("coverage_report", v.coverage_report);
        a("seeded_components", v.seeded_components);
        a("enabled_components", v.enabled_components);
        a("value_model", v.value_model);
        a("ratio_model", v.ratio_model);
        a("relation_basis_by_metric", v.relation_basis_by_metric);
        a("relation_routed_summary_artifacts", v.relation_routed_summary_artifacts);
        a("relation_fusion_metadata", v.relation_fusion_metadata);
        a("diagnostics", v.diagnostics);
    }
};
template <>
struct Fields<FormalModelMetadata> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("kind", v.kind);
        a("model_version", v.model_version);
        a("train_bucket_start", v.train_bucket_start);
        a("train_bucket_end", v.train_bucket_end);
        a("holdout_count", v.holdout_count);
        a("observation_count", v.observation_count);
        a("calendar_id", v.calendar_id);
        a("calendar_version", v.calendar_version);
    }
};
template <>
struct Fields<CoreBlock> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("beta0", v.beta0);
        a("trend_k", v.trend_k);
        a("day_sin", v.day_sin);
        a("day_cos", v.day_cos);
        a("week_sin", v.week_sin);
        a("week_cos", v.week_cos);
    }
};
template <>
struct Fields<MonthPosBlock> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("enabled", v.enabled);
        a("dom_coeff", v.dom_coeff);
        a("dme_coeff", v.dme_coeff);
        a("lwd_coeff", v.lwd_coeff);
        a("dom_center", v.dom_center);
        a("dme_center", v.dme_center);
        a("lwd_center", v.lwd_center);
    }
};
template <>
struct Fields<EventBlock> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("enabled", v.enabled);
        a("calendar_id", v.calendar_id);
        a("calendar_version", v.calendar_version);
        a("active_event_codes", v.active_event_codes);
        a("coeff", v.coeff);
    }
};
template <>
struct Fields<FitBlockDigest> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("block_name", v.block_name);
        a("status", v.status);
        a("sample_count", v.sample_count);
        a("objective", v.objective);
        a("condition_est", v.condition_est);
    }
};
template <>
struct Fields<ValueFormalModel> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("metadata", v.metadata);
        a("readiness", v.readiness);
        a("transform_name", v.transform_name);
        a("solver_name", v.solver_name);
        a("fit_strategy", v.fit_strategy);
        a("delta", v.delta);
        a("tz", v.tz);
        a("profile", v.profile);
        a("core_block", v.core_block);
        a("monthpos_block", v.monthpos_block);
        a("event_block", v.event_block);
        a("fit_summary", v.fit_summary);
        a("sigma_ref", v.sigma_ref);
        a("train_start", v.train_start);
        a("train_end", v.train_end);
        a("confidence_base_at_train", v.confidence_base_at_train);
    }
};
template <>
struct Fields<RatioFormalModel> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("metadata", v.metadata);
        a("readiness", v.readiness);
        a("transform_name", v.transform_name);
        a("solver_name", v.solver_name);
        a("fit_strategy", v.fit_strategy);
        a("m0", v.m0);
        a("alpha0", v.alpha0);
        a("beta0", v.beta0);
        a("delta", v.delta);
        a("tz", v.tz);
        a("profile", v.profile);
        a("core_block", v.core_block);
        a("monthpos_block", v.monthpos_block);
        a("event_block", v.event_block);
        a("fit_summary", v.fit_summary);
        a("sigma_ref", v.sigma_ref);
        a("train_start", v.train_start);
        a("train_end", v.train_end);
        a("confidence_base_at_train", v.confidence_base_at_train);
    }
};
template <>
struct Fields<CompiledEventCalendarEntry> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("event_code", v.event_code);
        a("alignment_mode", v.alignment_mode);
        a("start_ts", v.start_ts);
        a("end_ts", v.end_ts);
        a("tz", v.tz);
        a("event_code_index", v.event_code_index);
    }
};
template <>
struct Fields<CompiledEventBucketRange> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("begin", v.begin);
        a("end", v.end);
        a("subtree_end", v.subtree_end);
        a("event_code_index", v.event_code_index);
    }
};
template <>
struct Fields<CompiledEventCalendar> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("calendar_id", v.calendar_id);
        a("calendar_version", v.calendar_version);
        a("enabled_event_codes", v.enabled_event_codes);
        a("entries", v.entries);
        a("bound_bucket_seconds", v.bound_bucket_seconds);
        a("bucket_ranges", v.bucket_ranges);
    }
};
template <>
struct Fields<BaselineClockSpec> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("bucket_seconds", v.bucket_seconds);
        a("timezone", v.timezone);
    }
};
template <>
struct Fields<BaselineCalendarRef> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("calendar_id", v.calendar_id);
        a("calendar_version", v.calendar_version);
    }
};
template <>
struct Fields<BaselineTaskSpec> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("task_id", v.task_id);
        a("name", v.name);
        a("task_kind", v.task_kind);
        a("feature_id", v.feature_id);
        a("profile", v.profile);
        a("clock_spec", v.clock_spec);
        a("calendar_ref", v.calendar_ref);
        a("key", v.key);
        a("feature", v.feature);
        a("feature_type", v.feature_type);
        a("delta", v.delta);
        a("tz", v.tz);
        a("config_json", v.config_json);
        a("value_identity_transform", v.value_identity_transform);
    }
};
template <>
struct Fields<RelationSupportPolicySpec> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("k_support", v.k_support);
        a("min_hist_share", v.min_hist_share);
        a("min_active_ratio", v.min_active_ratio);
    }
};
template <>
struct Fields<RelationSummaryPolicySpec> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("k_head", v.k_head);
        a("k_stable", v.k_stable);
    }
};
template <>
struct Fields<RelationTaskSpec> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("task_id", v.task_id);
        a("name", v.name);
        a("task_kind", v.task_kind);
        a("feature_id", v.feature_id);
        a("profile", v.profile);
        a("calendar_ref", v.calendar_ref);
        a("feature_base", v.feature_base);
        a("group_space_id", v.group_space_id);
        a("group_space_version", v.group_space_version);
        a("metric_set_id", v.metric_set_id);
        a("metrics", v.metrics);
        a("encode_type", v.encode_type);
        a("other_group_idxs", v.other_group_idxs);
        a("support_policy", v.support_policy);
        a("summary_policy", v.summary_policy);
        a("config_json", v.config_json);
    }
};
template <>
struct Fields<RelationTaskClockSpec> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("delta", v.delta);
        a("tz", v.tz);
    }
};
template <>
struct Fields<RelationTaskCreateSpec> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("task_spec", v.task_spec);
        a("clock_spec", v.clock_spec);
    }
};
template <>
struct Fields<SharedProfileConfig> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("k_day", v.k_day);
        a("k_week", v.k_week);
        a("dme_max", v.dme_max);
        a("m_month_enable", v.m_month_enable);
        a("month_cov_min", v.month_cov_min);
        a("lambda_season", v.lambda_season);
        a("lambda_dom", v.lambda_dom);
        a("lambda_dme", v.lambda_dme);
        a("lambda_lwd", v.lambda_lwd);
        a("lambda_event", v.lambda_event);
    }
};
template <>
struct Fields<ValueSampledProfileConfig> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("name", v.name);
        a("n_train_min", v.n_train_min);
        a("transform_name_override", v.transform_name_override);
    }
};
template <>
struct Fields<RatioProfileConfig> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("name", v.name);
        a("s_prior", v.s_prior);
        a("d_min_train", v.d_min_train);
        a("phi_over", v.phi_over);
        a("m_floor", v.m_floor);
        a("eps_logit", v.eps_logit);
        a("v_floor", v.v_floor);
    }
};
template <>
struct Fields<RatioPriorConfig> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("m0", v.m0);
        a("alpha0", v.alpha0);
        a("beta0", v.beta0);
    }
};
template <>
struct Fields<RollingHarmonicState> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("sin_coeff", v.sin_coeff);
        a("cos_coeff", v.cos_coeff);
        a("sin_p", v.sin_p);
        a("cos_p", v.cos_p);
    }
};
template <>
struct Fields<RollingThetaState> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("level", v.level);
        a("trend", v.trend);
        a("daily", v.daily);
        a("weekly", v.weekly);
    }
};
template <>
struct Fields<RollingLastSubmitView> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("baseline_mu", v.baseline_mu);
        a("baseline_lower", v.baseline_lower);
        a("baseline_upper", v.baseline_upper);
        a("update_weight", v.update_weight);
        a("valid", v.valid);
        a("can_score", v.can_score);
        a("can_update", v.can_update);
        a("can_alert", v.can_alert);
    }
};
template <>
struct Fields<RollingState> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("series_key", v.series_key);
        a("theta", v.theta);
        a("sigma_init", v.sigma_init);
        a("sigma", v.sigma);
        a("bootstrap_seed_status", v.bootstrap_seed_status);
        a("daily_prior_quality", v.daily_prior_quality);
        a("weekly_prior_quality", v.weekly_prior_quality);
        a("p_level", v.p_level);
        a("p_level_trend", v.p_level_trend);
        a("p_trend", v.p_trend);
        a("short_ewma", v.short_ewma);
        a("long_ewma", v.long_ewma);
        a("drift_evidence", v.drift_evidence);
        a("level_shift_cusum_pos", v.level_shift_cusum_pos);
        a("level_shift_cusum_neg", v.level_shift_cusum_neg);
        a("level_shift_evidence", v.level_shift_evidence);
        a("state_status", v.state_status);
        a("maturity_status", v.maturity_status);
        a("score_trust_status", v.score_trust_status);
        a("calibration_status", v.calibration_status);
        a("monthpos_status", v.monthpos_status);
        a("has_seen_observation", v.has_seen_observation);
        a("last_seen_bucket", v.last_seen_bucket);
        a("last_processed_bucket", v.last_processed_bucket);
        a("last_submit", v.last_submit);
        a("accepted_update_count", v.accepted_update_count);
        a("maturity_prior_update_count", v.maturity_prior_update_count);
        a("learning_confidence", v.learning_confidence);
        a("score_confidence", v.score_confidence);
        a("effective_confidence", v.effective_confidence);
        a("detection_band_multiplier", v.detection_band_multiplier);
        a("residual_scale_ewma", v.residual_scale_ewma);
        a("coverage_ewma", v.coverage_ewma);
        a("tail3_ewma", v.tail3_ewma);
        a("tail5_ewma", v.tail5_ewma);
        a("abs_z_ewma", v.abs_z_ewma);
        a("calibration_update_count", v.calibration_update_count);
        a("stable_score_count", v.stable_score_count);
        a("score_ready_count", v.score_ready_count);
        a("last_degradation_bucket", v.last_degradation_bucket);
        a("degradation_reason", v.degradation_reason);
        a("daily_bin_count", v.daily_bin_count);
        a("weekly_bin_count", v.weekly_bin_count);
        a("monthpos_count", v.monthpos_count);
        a("monthpos_dme_count", v.monthpos_dme_count);
        a("monthpos_lwd_count", v.monthpos_lwd_count);
        a("monthpos_lwd_update_count", v.monthpos_lwd_update_count);
        a("month_transition_count", v.month_transition_count);
        a("last_seen_month_id", v.last_seen_month_id);
        a("monthpos_dom_coeff", v.monthpos_dom_coeff);
        a("monthpos_dme_coeff", v.monthpos_dme_coeff);
        a("monthpos_lwd_coeff", v.monthpos_lwd_coeff);
        a("monthpos_dom_center", v.monthpos_dom_center);
        a("monthpos_dme_center", v.monthpos_dme_center);
        a("monthpos_lwd_center", v.monthpos_lwd_center);
        a("monthpos_update_count", v.monthpos_update_count);
        a("monthpos_ready_count", v.monthpos_ready_count);
        a("diagnostics", v.diagnostics);
    }
};
template <>
struct Fields<BaselineRelationFusionConfig> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("enable_relation_fusion", v.enable_relation_fusion);
        a("fusion_z_score_cap", v.fusion_z_score_cap);
        a("fusion_min_evidence_score", v.fusion_min_evidence_score);
        a("fusion_persistence_window", v.fusion_persistence_window);
        a("fusion_warming_weight", v.fusion_warming_weight);
        a("fusion_degraded_weight", v.fusion_degraded_weight);
        a("fusion_support_weight", v.fusion_support_weight);
        a("fusion_oppose_weight", v.fusion_oppose_weight);
        a("basic_pattern_weight", v.basic_pattern_weight);
        a("stable_head_pattern_weight", v.stable_head_pattern_weight);
        a("dominant_single_cap", v.dominant_single_cap);
        a("dominant_pattern_cap", v.dominant_pattern_cap);
        a("fusion_state_ttl_buckets", v.fusion_state_ttl_buckets);
        a("fusion_state_max_sources", v.fusion_state_max_sources);
        a("fusion_state_cleanup_interval_updates", v.fusion_state_cleanup_interval_updates);
        a("fusion_state_cleanup_scan_limit", v.fusion_state_cleanup_scan_limit);
        a("fusion_persistence_max_keys_per_source", v.fusion_persistence_max_keys_per_source);
    }
};
template <>
struct Fields<BaselineRelationRollingConfig> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("enable_routed_rolling", v.enable_routed_rolling);
        a("enable_stream_basis", v.enable_stream_basis);
        a("include_universal_summaries_without_basis", v.include_universal_summaries_without_basis);
        a("basis_stats_max_groups", v.basis_stats_max_groups);
        a("basis_collect_min_buckets", v.basis_collect_min_buckets);
        a("basis_ready_min_buckets", v.basis_ready_min_buckets);
        a("basis_refresh_interval_buckets", v.basis_refresh_interval_buckets);
        a("basis_candidate_min_coverage_ratio", v.basis_candidate_min_coverage_ratio);
        a("basis_replacement_cap_ratio", v.basis_replacement_cap_ratio);
        a("basis_replacement_cap_max", v.basis_replacement_cap_max);
        a("basis_handover_warmup_buckets", v.basis_handover_warmup_buckets);
        a("basis_threshold_margin", v.basis_threshold_margin);
        a("basis_min_stable_refresh_count", v.basis_min_stable_refresh_count);
        a("routed_state_shard_count", v.routed_state_shard_count);
        a("relation_fusion", v.relation_fusion);
    }
};
template <>
struct Fields<BaselineRollingConfig> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("n_min_score", v.n_min_score);
        a("n_min_update", v.n_min_update);
        a("n_ref", v.n_ref);
        a("sample_count_noise", v.sample_count_noise);
        a("d_min_score", v.d_min_score);
        a("d_min_update", v.d_min_update);
        a("d_ref", v.d_ref);
        a("ratio_denominator_noise", v.ratio_denominator_noise);
        a("z_downweight", v.z_downweight);
        a("z_skip", v.z_skip);
        a("small_update_weight", v.small_update_weight);
        a("daily_harmonic_order", v.daily_harmonic_order);
        a("weekly_harmonic_order", v.weekly_harmonic_order);
        a("level_learning_scale", v.level_learning_scale);
        a("day_learning_scale", v.day_learning_scale);
        a("week_learning_scale", v.week_learning_scale);
        a("cold_day_learning_scale", v.cold_day_learning_scale);
        a("cold_week_learning_scale", v.cold_week_learning_scale);
        a("seasonal_drift_min_scale", v.seasonal_drift_min_scale);
        a("full_seed_seasonal_scale", v.full_seed_seasonal_scale);
        a("partial_seed_seasonal_scale", v.partial_seed_seasonal_scale);
        a("freeze_seeded_seasonal_on_drift", v.freeze_seeded_seasonal_on_drift);
        a("day_delta_coeff_max_scale", v.day_delta_coeff_max_scale);
        a("week_delta_coeff_max_scale", v.week_delta_coeff_max_scale);
        a("q_day_scale", v.q_day_scale);
        a("q_week_scale", v.q_week_scale);
        a("q_level_scale", v.q_level_scale);
        a("q_trend_scale", v.q_trend_scale);
        a("trend_update_scale", v.trend_update_scale);
        a("cold_trend_update_scale", v.cold_trend_update_scale);
        a("trend_delta_max_scale", v.trend_delta_max_scale);
        a("trend_abs_max_scale", v.trend_abs_max_scale);
        a("p_level_init_scale", v.p_level_init_scale);
        a("p_trend_init_scale", v.p_trend_init_scale);
        a("p_day_init_scale", v.p_day_init_scale);
        a("p_week_init_scale", v.p_week_init_scale);
        a("p_floor_scale", v.p_floor_scale);
        a("p_cap_scale", v.p_cap_scale);
        a("alpha_short", v.alpha_short);
        a("alpha_long", v.alpha_long);
        a("z_cap", v.z_cap);
        a("drift_start", v.drift_start);
        a("drift_full", v.drift_full);
        a("level_shift_reference_z", v.level_shift_reference_z);
        a("level_shift_cusum_decay", v.level_shift_cusum_decay);
        a("level_shift_cusum_threshold", v.level_shift_cusum_threshold);
        a("max_level_boost", v.max_level_boost);
        a("max_q_boost", v.max_q_boost);
        a("skip_relax", v.skip_relax);
        a("process_noise_gap_cap_buckets", v.process_noise_gap_cap_buckets);
        a("alpha_sigma", v.alpha_sigma);
        a("c_sigma", v.c_sigma);
        a("sigma_floor", v.sigma_floor);
        a("cold_start_band_scale", v.cold_start_band_scale);
        a("band_z", v.band_z);
        a("forecast_band_z", v.forecast_band_z);
        a("confidence_cold", v.confidence_cold);
        a("confidence_warming", v.confidence_warming);
        a("confidence_ready_hint_cap", v.confidence_ready_hint_cap);
        a("min_warming_updates", v.min_warming_updates);
        a("min_ready_hint_updates", v.min_ready_hint_updates);
        a("level_ready_min_updates", v.level_ready_min_updates);
        a("score_warming_min_updates", v.score_warming_min_updates);
        a("score_ready_min_updates", v.score_ready_min_updates);
        a("score_recovery_min_updates", v.score_recovery_min_updates);
        a("score_drift_degrade_start", v.score_drift_degrade_start);
        a("calibration_alpha", v.calibration_alpha);
        a("calibration_warmup_min_updates", v.calibration_warmup_min_updates);
        a("calibration_coverage_floor", v.calibration_coverage_floor);
        a("calibration_tail3_limit", v.calibration_tail3_limit);
        a("calibration_tail5_limit", v.calibration_tail5_limit);
        a("calibration_multiplier_min", v.calibration_multiplier_min);
        a("calibration_multiplier_max", v.calibration_multiplier_max);
        a("daily_coverage_bins", v.daily_coverage_bins);
        a("weekly_coverage_bins", v.weekly_coverage_bins);
        a("daily_ready_min_days", v.daily_ready_min_days);
        a("daily_ready_coverage_ratio", v.daily_ready_coverage_ratio);
        a("weekly_ready_min_weeks", v.weekly_ready_min_weeks);
        a("weekly_ready_coverage_ratio", v.weekly_ready_coverage_ratio);
        a("maturity_uncertainty_cold_scale", v.maturity_uncertainty_cold_scale);
        a("maturity_uncertainty_warming_scale", v.maturity_uncertainty_warming_scale);
        a("maturity_uncertainty_drift_scale", v.maturity_uncertainty_drift_scale);
        a("maturity_uncertainty_recalibrating_scale", v.maturity_uncertainty_recalibrating_scale);
        a("missing_daily_uncertainty_scale", v.missing_daily_uncertainty_scale);
        a("missing_weekly_uncertainty_scale", v.missing_weekly_uncertainty_scale);
        a("level_only_extreme_z", v.level_only_extreme_z);
        a("detection_band_std_cap", v.detection_band_std_cap);
        a("forecast_trend_cap_buckets", v.forecast_trend_cap_buckets);
        a("monthpos_alpha", v.monthpos_alpha);
        a("monthpos_delta_max_scale", v.monthpos_delta_max_scale);
        a("monthpos_min_month_transitions", v.monthpos_min_month_transitions);
        a("monthpos_ready_coverage_ratio", v.monthpos_ready_coverage_ratio);
        a("bucket_seconds", v.bucket_seconds);
        a("timezone", v.timezone);
        a("day_buckets", v.day_buckets);
        a("week_buckets", v.week_buckets);
        a("relation_rolling", v.relation_rolling);
        a("ratio_eps_logit", v.ratio_eps_logit);
    }
};
template <>
struct Fields<RelationGroupHistoryStat> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("group_idx", v.group_idx);
        a("hist_mass", v.hist_mass);
        a("active_bucket_count", v.active_bucket_count);
    }
};
template <>
struct Fields<RelationBasisBuildInput> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("basis_version", v.basis_version);
        a("feature_base", v.feature_base);
        a("metric_name", v.metric_name);
        a("group_space_id", v.group_space_id);
        a("group_space_version", v.group_space_version);
        a("other_group_idxs", v.other_group_idxs);
        a("support_policy", v.support_policy);
        a("summary_policy", v.summary_policy);
        a("valid_bucket_count", v.valid_bucket_count);
        a("total_hist_mass_denominator", v.total_hist_mass_denominator);
        a("group_stats", v.group_stats);
    }
};
template <>
struct Fields<RelationServiceBasis> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("basis_version", v.basis_version);
        a("feature_base", v.feature_base);
        a("metric_name", v.metric_name);
        a("group_space_id", v.group_space_id);
        a("group_space_version", v.group_space_version);
        a("k_head", v.k_head);
        a("other_group_idxs", v.other_group_idxs);
        a("support_explicit", v.support_explicit);
        a("stable_head", v.stable_head);
        a("head_proto_q", v.head_proto_q);
    }
};
template <>
struct Fields<BlockSolverConfig> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("solver_name", v.solver_name);
        a("c_huber", v.c_huber);
        a("s_min_fit", v.s_min_fit);
        a("max_iter_fit", v.max_iter_fit);
        a("tol_obj_rel", v.tol_obj_rel);
        a("tol_beta_inf", v.tol_beta_inf);
        a("cond_max", v.cond_max);
    }
};
template <>
struct Fields<RollingBaselineResult> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("status", v.status);
        a("series_key", v.series_key);
        a("bucket_id", v.bucket_id);
        a("observed", v.observed);
        a("observed_model", v.observed_model);
        a("baseline_mu", v.baseline_mu);
        a("baseline_lower", v.baseline_lower);
        a("baseline_upper", v.baseline_upper);
        a("band_width", v.band_width);
        a("model_mu", v.model_mu);
        a("model_lower", v.model_lower);
        a("model_upper", v.model_upper);
        a("residual", v.residual);
        a("band_std", v.band_std);
        a("z_score", v.z_score);
        a("is_outside_band", v.is_outside_band);
        a("can_score", v.can_score);
        a("can_update", v.can_update);
        a("score_weight", v.score_weight);
        a("update_weight", v.update_weight);
        a("confidence", v.confidence);
        a("maturity_status", v.maturity_status);
        a("score_trust_status", v.score_trust_status);
        a("calibration_status", v.calibration_status);
        a("learning_confidence", v.learning_confidence);
        a("score_confidence", v.score_confidence);
        a("effective_confidence", v.effective_confidence);
        a("can_alert", v.can_alert);
        a("enabled_components", v.enabled_components);
        a("component_readiness", v.component_readiness);
        a("state_status", v.state_status);
        a("uncertainty_source", v.uncertainty_source);
        a("drift_evidence", v.drift_evidence);
        a("adapt_boost", v.adapt_boost);
        a("sample_count", v.sample_count);
        a("skipped_low_sample_count", v.skipped_low_sample_count);
        a("numerator", v.numerator);
        a("denominator", v.denominator);
        a("skipped_low_denominator", v.skipped_low_denominator);
        a("diagnostics", v.diagnostics);
    }
};
template <>
struct Fields<RelationRoutedSummaryResult> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("source_series_key", v.source_series_key);
        a("routed_series_key", v.routed_series_key);
        a("metric", v.metric);
        a("summary", v.summary);
        a("feature_type", v.feature_type);
        a("basis_version", v.basis_version);
        a("basis_scoped", v.basis_scoped);
        a("rolling", v.rolling);
    }
};
template <>
struct Fields<RelationFusionSingleEvidence> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("source_series_key", v.source_series_key);
        a("routed_series_key", v.routed_series_key);
        a("feature_base", v.feature_base);
        a("metric", v.metric);
        a("summary", v.summary);
        a("feature_type", v.feature_type);
        a("basis_version", v.basis_version);
        a("basis_scoped", v.basis_scoped);
        a("direction", v.direction);
        a("normalized_score", v.normalized_score);
        a("confidence", v.confidence);
        a("persistence", v.persistence);
        a("evidence_strength", v.evidence_strength);
        a("available", v.available);
        a("can_alert", v.can_alert);
        a("score_trust_status", v.score_trust_status);
        a("metric_basis_status", v.metric_basis_status);
        a("unavailable_reason", v.unavailable_reason);
    }
};
template <>
struct Fields<RelationFusionPatternScore> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("source_series_key", v.source_series_key);
        a("feature_base", v.feature_base);
        a("pattern", v.pattern);
        a("score", v.score);
        a("weighted_score", v.weighted_score);
        a("pattern_weight", v.pattern_weight);
        a("metrics_hit", v.metrics_hit);
        a("supporting_features", v.supporting_features);
        a("diagnostics", v.diagnostics);
    }
};
template <>
struct Fields<RelationFusionResult> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("status", v.status);
        a("source_series_key", v.source_series_key);
        a("feature_base", v.feature_base);
        a("bucket_id", v.bucket_id);
        a("relation_risk", v.relation_risk);
        a("single_risk", v.single_risk);
        a("pattern_risk", v.pattern_risk);
        a("dominant_single", v.dominant_single);
        a("dominant_pattern", v.dominant_pattern);
        a("pattern_scores", v.pattern_scores);
        a("diagnostics", v.diagnostics);
    }
};
template <>
struct Fields<RelationRollingResult> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("status", v.status);
        a("series_key", v.series_key);
        a("bucket_id", v.bucket_id);
        a("basis_version", v.basis_version);
        a("basis_status", v.basis_status);
        a("basis_updated", v.basis_updated);
        a("handover_active", v.handover_active);
        a("routed_results", v.routed_results);
        a("has_fusion_result", v.has_fusion_result);
        a("fusion_result", v.fusion_result);
        a("diagnostics", v.diagnostics);
    }
};
template <>
struct Fields<RelationFusionRuntimeState> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("last_bucket_id", v.last_bucket_id);
        a("has_last_bucket", v.has_last_bucket);
        a("last_touched_bucket_id", v.last_touched_bucket_id);
        a("last_touched_update_seq", v.last_touched_update_seq);
        a("persistence_by_evidence_dir", v.persistence_by_evidence_dir);
        a("last_result", v.last_result);
    }
};
template <>
struct Fields<RelationStreamBasisConfig> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("max_groups", v.max_groups);
        a("threshold_margin", v.threshold_margin);
    }
};
template <>
struct Fields<RelationBasisRuntimeConfig> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("stream", v.stream);
        a("collect_min_buckets", v.collect_min_buckets);
        a("ready_min_buckets", v.ready_min_buckets);
        a("refresh_interval_buckets", v.refresh_interval_buckets);
        a("candidate_min_coverage_ratio", v.candidate_min_coverage_ratio);
        a("replacement_cap_ratio", v.replacement_cap_ratio);
        a("replacement_cap_max", v.replacement_cap_max);
        a("handover_warmup_buckets", v.handover_warmup_buckets);
        a("min_stable_refresh_count", v.min_stable_refresh_count);
    }
};
template <>
struct Fields<RelationStreamGroupEstimate> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("group_idx", v.group_idx);
        a("estimated_mass", v.estimated_mass);
        a("mass_error_upper_bound", v.mass_error_upper_bound);
        a("active_bucket_count", v.active_bucket_count);
        a("last_seen_bucket", v.last_seen_bucket);
    }
};
}  // namespace flowsql::baseline::checkpoint
