// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "relation_fusion.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace flowsql {
namespace baseline {

namespace {

constexpr double kDirectionEpsilon = 1.0e-9;

double Clamp01(double value) {
    if (value < 0.0) return 0.0;
    if (value > 1.0) return 1.0;
    return value;
}

std::string DirectionForRolling(const RollingBaselineResult& rolling) {
    const double signed_offset =
        std::fabs(rolling.residual) > kDirectionEpsilon ? rolling.residual : rolling.z_score;
    if (signed_offset > kDirectionEpsilon) return "up";
    if (signed_offset < -kDirectionEpsilon) return "down";
    return "none";
}

std::string EvidenceKey(const std::string& source,
                        const std::string& feature_base,
                        const std::string& metric,
                        const std::string& summary,
                        const std::string& direction,
                        bool basis_scoped,
                        uint64_t basis_version) {
    std::string key = source + "::" + feature_base + "::" + metric + "::" +
                      summary + "::" + direction;
    if (basis_scoped) key += "::basis:" + std::to_string(basis_version);
    return key;
}

std::string FeatureToken(const std::string& summary, const std::string& direction) {
    return summary + ":" + direction;
}

bool IsMappedDirection(const std::string& summary, const std::string& direction) {
    if (direction == "none") return false;
    if (summary == "entropy_shannon") return direction == "up" || direction == "down";
    if (summary == "stable_headk_coverage") return direction == "up" || direction == "down";
    if (summary == "out_of_support_share" ||
        summary == "distinct_group_count" ||
        summary == "top1_share" ||
        summary == "headk_share" ||
        summary == "stable_headk_mix_drift") {
        return direction == "up";
    }
    return summary.compare(0, std::string("stable_g_share_").size(), "stable_g_share_") == 0;
}

std::vector<std::string> ExpectedDirectionsForSummary(const std::string& summary) {
    if (summary == "entropy_shannon" || summary == "stable_headk_coverage") {
        return {"up", "down"};
    }
    if (summary == "out_of_support_share" ||
        summary == "distinct_group_count" ||
        summary == "top1_share" ||
        summary == "headk_share" ||
        summary == "stable_headk_mix_drift") {
        return {"up"};
    }
    if (summary.compare(0, std::string("stable_g_share_").size(), "stable_g_share_") == 0) {
        return {"up", "down"};
    }
    return {};
}

struct ExpectedEvidence {
    std::string source_series_key;
    std::string feature_base;
    std::string metric;
    std::string summary;
    std::string feature_type;
    std::string direction;
    bool basis_scoped = false;
    uint64_t basis_version = 0;
    std::string unavailable_reason;
};

RelationFusionSingleEvidence EmptyEvidence(const ExpectedEvidence& expected) {
    RelationFusionSingleEvidence evidence;
    evidence.source_series_key = expected.source_series_key;
    evidence.feature_base = expected.feature_base;
    evidence.metric = expected.metric;
    evidence.summary = expected.summary;
    evidence.feature_type = expected.feature_type;
    evidence.direction = expected.direction;
    evidence.basis_scoped = expected.basis_scoped;
    evidence.basis_version = expected.basis_version;
    evidence.available = false;
    evidence.unavailable_reason = expected.unavailable_reason;
    return evidence;
}

void AddExpected(const ExpectedEvidence& expected,
                 std::map<std::string, ExpectedEvidence>* expected_by_key) {
    if (!expected_by_key) return;
    const std::string key = EvidenceKey(expected.source_series_key,
                                        expected.feature_base,
                                        expected.metric,
                                        expected.summary,
                                        expected.direction,
                                        expected.basis_scoped,
                                        expected.basis_version);
    expected_by_key->insert_or_assign(key, expected);
}

void AddExpectedSummary(const std::string& source,
                        const std::string& feature_base,
                        const RelationFusionMetricContext& metric,
                        const std::string& summary,
                        const std::string& feature_type,
                        bool basis_scoped,
                        uint64_t basis_version,
                        const std::string& unavailable_reason,
                        std::map<std::string, ExpectedEvidence>* expected_by_key) {
    for (const std::string& direction : ExpectedDirectionsForSummary(summary)) {
        ExpectedEvidence expected;
        expected.source_series_key = source;
        expected.feature_base = feature_base;
        expected.metric = metric.metric;
        expected.summary = summary;
        expected.feature_type = feature_type;
        expected.direction = direction;
        expected.basis_scoped = basis_scoped;
        expected.basis_version = basis_scoped ? basis_version : 0;
        expected.unavailable_reason = unavailable_reason;
        AddExpected(expected, expected_by_key);
    }
}

std::map<std::string, ExpectedEvidence> BuildExpectedUniverse(
    const RelationFusionUpdateInput& input) {
    std::map<std::string, ExpectedEvidence> expected_by_key;
    for (const auto& metric : input.metrics) {
        if (metric.metric.empty()) continue;
        const bool metric_available = metric.present && metric.valid;
        const std::string metric_reason =
            metric_available
                ? ""
                : (!metric.unavailable_reason.empty()
                       ? metric.unavailable_reason
                       : (!metric.present ? "metric_missing" : "metric_invalid"));

        AddExpectedSummary(input.source_series_key,
                           input.feature_base,
                           metric,
                           "entropy_shannon",
                           "value_basic",
                           false,
                           0,
                           metric_reason.empty() ? "summary_missing" : metric_reason,
                           &expected_by_key);
        AddExpectedSummary(input.source_series_key,
                           input.feature_base,
                           metric,
                           "top1_share",
                           "ratio",
                           false,
                           0,
                           metric_reason.empty() ? "summary_missing" : metric_reason,
                           &expected_by_key);
        AddExpectedSummary(input.source_series_key,
                           input.feature_base,
                           metric,
                           "headk_share",
                           "ratio",
                           false,
                           0,
                           metric_reason.empty() ? "summary_missing" : metric_reason,
                           &expected_by_key);
        AddExpectedSummary(input.source_series_key,
                           input.feature_base,
                           metric,
                           "distinct_group_count",
                           "value_basic",
                           false,
                           0,
                           metric.active_count_from_upstream && metric_reason.empty()
                               ? "summary_missing"
                               : (!metric_reason.empty()
                                      ? metric_reason
                                      : "distinct_group_count_untrusted"),
                           &expected_by_key);

        if (metric.has_active_basis && metric.basis_version > 0) {
            AddExpectedSummary(input.source_series_key,
                               input.feature_base,
                               metric,
                               "out_of_support_share",
                               "ratio",
                               true,
                               metric.basis_version,
                               metric_reason.empty() ? "summary_missing" : metric_reason,
                               &expected_by_key);
            AddExpectedSummary(input.source_series_key,
                               input.feature_base,
                               metric,
                               "stable_headk_coverage",
                               "ratio",
                               true,
                               metric.basis_version,
                               metric_reason.empty() ? "summary_missing" : metric_reason,
                               &expected_by_key);
            if (metric.stable_head_mix_drift_expected) {
                AddExpectedSummary(input.source_series_key,
                                   input.feature_base,
                                   metric,
                                   "stable_headk_mix_drift",
                                   "value_basic",
                                   true,
                                   metric.basis_version,
                                   metric_reason.empty() ? "summary_missing" : metric_reason,
                                   &expected_by_key);
            }
            for (uint32_t i = 0; i < metric.stable_head_size; ++i) {
                AddExpectedSummary(input.source_series_key,
                                   input.feature_base,
                                   metric,
                                   "stable_g_share_" + std::to_string(i),
                                   "ratio",
                                   true,
                                   metric.basis_version,
                                   metric_reason.empty() ? "summary_missing" : metric_reason,
                                   &expected_by_key);
            }
        }
    }
    return expected_by_key;
}

double TrustFactor(const RelationFusionRoutedInput& input,
                   const std::string& direction,
                   const RelationFusionRuntimeConfig& config,
                   std::string* unavailable_reason) {
    const RollingBaselineResult& rolling = input.routed.rolling;
    const double normalized_score =
        config.fusion_z_score_cap > 0.0
            ? Clamp01(std::fabs(rolling.z_score) / config.fusion_z_score_cap)
            : 0.0;
    const bool base_available = rolling.status == BaselineStatus::kOk &&
                                rolling.can_score &&
                                direction != "none" &&
                                normalized_score > 0.0 &&
                                !(input.routed.summary == "distinct_group_count" &&
                                  !input.active_count_from_upstream);
    if (!base_available) {
        if (unavailable_reason) {
            if (input.routed.summary == "distinct_group_count" &&
                !input.active_count_from_upstream) {
                *unavailable_reason = "distinct_group_count_untrusted";
            } else {
                *unavailable_reason = "score_unavailable";
            }
        }
        return 0.0;
    }
    if (rolling.score_trust_status == "score_untrusted") {
        if (unavailable_reason) *unavailable_reason = "score_untrusted";
        return 0.0;
    }
    if (input.routed.basis_scoped && input.metric_basis_status != "basis_ready") {
        if (unavailable_reason) *unavailable_reason = "relation_basis_not_ready";
        return 0.0;
    }
    if (rolling.score_trust_status == "score_ready") {
        if (!rolling.can_alert) {
            if (unavailable_reason) *unavailable_reason = "can_alert_false";
            return 0.0;
        }
        return 1.0;
    }
    if (rolling.score_trust_status == "score_warming") return config.fusion_warming_weight;
    if (rolling.score_trust_status == "drift_learning" ||
        rolling.score_trust_status == "recalibrating") {
        return config.fusion_degraded_weight;
    }
    if (unavailable_reason) *unavailable_reason = "score_trust_not_ready";
    return 0.0;
}

double FusionConfidence(const RollingBaselineResult& rolling) {
    if (rolling.score_trust_status == "score_ready" ||
        rolling.score_trust_status == "score_warming") {
        return Clamp01(rolling.effective_confidence);
    }
    if (rolling.score_trust_status == "drift_learning" ||
        rolling.score_trust_status == "recalibrating") {
        return Clamp01(rolling.learning_confidence);
    }
    return 0.0;
}

RelationFusionSingleEvidence BuildEvidence(
    const RelationFusionRoutedInput& input,
    const std::string& direction,
    const RelationFusionRuntimeConfig& config,
    uint32_t persistence,
    const std::string& unavailable_reason) {
    RelationFusionSingleEvidence evidence;
    evidence.source_series_key = input.routed.source_series_key;
    evidence.routed_series_key = input.routed.routed_series_key;
    evidence.feature_base = input.feature_base;
    evidence.metric = input.routed.metric;
    evidence.summary = input.routed.summary;
    evidence.feature_type = input.routed.feature_type;
    evidence.basis_version = input.routed.basis_version;
    evidence.basis_scoped = input.routed.basis_scoped;
    evidence.direction = direction;
    evidence.normalized_score =
        config.fusion_z_score_cap > 0.0
            ? Clamp01(std::fabs(input.routed.rolling.z_score) / config.fusion_z_score_cap)
            : 0.0;
    evidence.confidence = FusionConfidence(input.routed.rolling);
    evidence.persistence = persistence;
    evidence.can_alert = input.routed.rolling.can_alert;
    evidence.score_trust_status = input.routed.rolling.score_trust_status;
    evidence.metric_basis_status = input.metric_basis_status;
    evidence.unavailable_reason = unavailable_reason;
    evidence.available = unavailable_reason.empty();
    const double persistence_factor =
        config.fusion_persistence_window == 0
            ? 0.0
            : std::min(1.0,
                       static_cast<double>(persistence) /
                           static_cast<double>(config.fusion_persistence_window));
    const double trust = unavailable_reason.empty()
                             ? TrustFactor(input, direction, config, nullptr)
                             : 0.0;
    evidence.evidence_strength =
        evidence.normalized_score * evidence.confidence * persistence_factor * trust;
    return evidence;
}

double Top2Mean(std::initializer_list<double> values) {
    std::vector<double> positive;
    for (double value : values) {
        if (value > 0.0) positive.push_back(value);
    }
    if (positive.empty()) return 0.0;
    std::sort(positive.begin(), positive.end(), std::greater<double>());
    if (positive.size() == 1) return positive[0];
    return 0.5 * (positive[0] + positive[1]);
}

double SaturatingUnion(const std::vector<double>& values) {
    double remaining = 1.0;
    for (double value : values) remaining *= (1.0 - Clamp01(value));
    return 1.0 - remaining;
}

struct LocalPatternScore {
    std::string metric;
    double score = 0.0;
    std::vector<std::string> supporting_features;
};

double EvidenceValue(
    const std::map<std::string, double>& evidence_by_token,
    const std::string& token) {
    const auto it = evidence_by_token.find(token);
    return it == evidence_by_token.end() ? 0.0 : it->second;
}

void AddFeatureIfPositive(const std::map<std::string, double>& evidence_by_token,
                          const std::string& token,
                          std::vector<std::string>* features) {
    if (!features) return;
    if (EvidenceValue(evidence_by_token, token) > 0.0) features->push_back(token);
}

LocalPatternScore BuildLocalPattern(
    const std::string& pattern,
    const std::string& metric,
    const std::map<std::string, double>& evidence_by_token,
    const RelationFusionRuntimeConfig& config) {
    LocalPatternScore local;
    local.metric = metric;

    double core = 0.0;
    double support = 0.0;
    double oppose = 0.0;
    if (pattern == "support_escape") {
        core = EvidenceValue(evidence_by_token, "out_of_support_share:up");
        support = Top2Mean({EvidenceValue(evidence_by_token, "entropy_shannon:up"),
                            EvidenceValue(evidence_by_token, "distinct_group_count:up"),
                            EvidenceValue(evidence_by_token, "stable_headk_coverage:down")});
        oppose = Top2Mean({EvidenceValue(evidence_by_token, "top1_share:up"),
                           EvidenceValue(evidence_by_token, "headk_share:up"),
                           EvidenceValue(evidence_by_token, "entropy_shannon:down")});
        AddFeatureIfPositive(evidence_by_token, "out_of_support_share:up",
                             &local.supporting_features);
        AddFeatureIfPositive(evidence_by_token, "entropy_shannon:up",
                             &local.supporting_features);
        AddFeatureIfPositive(evidence_by_token, "distinct_group_count:up",
                             &local.supporting_features);
        AddFeatureIfPositive(evidence_by_token, "stable_headk_coverage:down",
                             &local.supporting_features);
    } else if (pattern == "head_concentration") {
        core = EvidenceValue(evidence_by_token, "top1_share:up");
        support = Top2Mean({EvidenceValue(evidence_by_token, "headk_share:up"),
                            EvidenceValue(evidence_by_token, "entropy_shannon:down")});
        oppose = Top2Mean({EvidenceValue(evidence_by_token, "out_of_support_share:up"),
                           EvidenceValue(evidence_by_token, "entropy_shannon:up")});
        AddFeatureIfPositive(evidence_by_token, "top1_share:up",
                             &local.supporting_features);
        AddFeatureIfPositive(evidence_by_token, "headk_share:up",
                             &local.supporting_features);
        AddFeatureIfPositive(evidence_by_token, "entropy_shannon:down",
                             &local.supporting_features);
    } else if (pattern == "legacy_head_dilution") {
        core = EvidenceValue(evidence_by_token, "stable_headk_coverage:down");
        support = Top2Mean({EvidenceValue(evidence_by_token, "out_of_support_share:up"),
                            EvidenceValue(evidence_by_token, "entropy_shannon:up")});
        oppose = EvidenceValue(evidence_by_token, "stable_headk_coverage:up");
        AddFeatureIfPositive(evidence_by_token, "stable_headk_coverage:down",
                             &local.supporting_features);
        AddFeatureIfPositive(evidence_by_token, "out_of_support_share:up",
                             &local.supporting_features);
        AddFeatureIfPositive(evidence_by_token, "entropy_shannon:up",
                             &local.supporting_features);
    } else if (pattern == "stable_head_mix_shift") {
        core = EvidenceValue(evidence_by_token, "stable_headk_mix_drift:up");
        support = 0.0;
        oppose = Top2Mean({EvidenceValue(evidence_by_token, "stable_headk_coverage:down"),
                           EvidenceValue(evidence_by_token, "out_of_support_share:up")});
        AddFeatureIfPositive(evidence_by_token, "stable_headk_mix_drift:up",
                             &local.supporting_features);
    }

    local.score =
        Clamp01(core + config.fusion_support_weight * support -
                config.fusion_oppose_weight * oppose);
    return local;
}

double PatternWeight(const std::string& pattern, const RelationFusionRuntimeConfig& config) {
    if (pattern == "legacy_head_dilution" || pattern == "stable_head_mix_shift") {
        return config.stable_head_pattern_weight;
    }
    return config.basic_pattern_weight;
}

std::vector<RelationFusionPatternScore> BuildPatternScores(
    const RelationFusionUpdateInput& input,
    const std::vector<RelationFusionSingleEvidence>& evidence) {
    std::map<std::string, std::map<std::string, double>> evidence_by_metric;
    for (const auto& item : evidence) {
        if (!item.available || item.evidence_strength <= 0.0) continue;
        evidence_by_metric[item.metric][FeatureToken(item.summary, item.direction)] =
            std::max(evidence_by_metric[item.metric][FeatureToken(item.summary, item.direction)],
                     item.evidence_strength);
    }

    const std::vector<std::string> patterns = {
        "support_escape",
        "head_concentration",
        "legacy_head_dilution",
        "stable_head_mix_shift",
    };
    std::vector<RelationFusionPatternScore> scores;
    for (const std::string& pattern : patterns) {
        std::vector<double> local_scores;
        std::set<std::string> supporting_features;
        RelationFusionPatternScore score;
        score.source_series_key = input.source_series_key;
        score.feature_base = input.feature_base;
        score.pattern = pattern;
        score.pattern_weight = PatternWeight(pattern, input.config);
        for (const auto& entry : evidence_by_metric) {
            const LocalPatternScore local =
                BuildLocalPattern(pattern, entry.first, entry.second, input.config);
            if (local.score <= 0.0) continue;
            local_scores.push_back(local.score);
            score.metrics_hit.push_back(entry.first);
            for (const std::string& feature : local.supporting_features) {
                supporting_features.insert(feature);
            }
        }
        if (local_scores.empty()) continue;
        score.score = SaturatingUnion(local_scores);
        score.weighted_score = Clamp01(score.pattern_weight * score.score);
        score.supporting_features.assign(supporting_features.begin(),
                                         supporting_features.end());
        scores.push_back(std::move(score));
    }
    std::sort(scores.begin(),
              scores.end(),
              [](const RelationFusionPatternScore& lhs,
                 const RelationFusionPatternScore& rhs) {
                  if (lhs.weighted_score != rhs.weighted_score) {
                      return lhs.weighted_score > rhs.weighted_score;
                  }
                  return lhs.pattern < rhs.pattern;
              });
    return scores;
}

void LimitVector(std::vector<RelationFusionPatternScore>* values, uint32_t cap) {
    if (values && cap > 0 && values->size() > cap) values->resize(cap);
}

void SelectDominantSingle(const std::vector<RelationFusionSingleEvidence>& evidence,
                          const RelationFusionRuntimeConfig& config,
                          const std::unordered_map<std::string, uint32_t>& previous,
                          std::vector<RelationFusionSingleEvidence>* out) {
    if (!out) return;
    out->clear();
    std::vector<RelationFusionSingleEvidence> available;
    std::vector<RelationFusionSingleEvidence> unavailable;
    for (const auto& item : evidence) {
        if (item.available && item.evidence_strength > 0.0) {
            available.push_back(item);
        } else {
            unavailable.push_back(item);
        }
    }
    std::sort(available.begin(),
              available.end(),
              [](const RelationFusionSingleEvidence& lhs,
                 const RelationFusionSingleEvidence& rhs) {
                  if (lhs.evidence_strength != rhs.evidence_strength) {
                      return lhs.evidence_strength > rhs.evidence_strength;
                  }
                  return std::tie(lhs.metric, lhs.summary, lhs.direction) <
                         std::tie(rhs.metric, rhs.summary, rhs.direction);
              });
    for (const auto& item : available) {
        if (out->size() >= config.dominant_single_cap) return;
        out->push_back(item);
    }
    if (!out->empty()) return;

    std::sort(unavailable.begin(),
              unavailable.end(),
              [&previous](const RelationFusionSingleEvidence& lhs,
                          const RelationFusionSingleEvidence& rhs) {
                  const auto lhs_it = previous.find(EvidenceKey(lhs.source_series_key,
                                                                lhs.feature_base,
                                                                lhs.metric,
                                                                lhs.summary,
                                                                lhs.direction,
                                                                lhs.basis_scoped,
                                                                lhs.basis_version));
                  const auto rhs_it = previous.find(EvidenceKey(rhs.source_series_key,
                                                                rhs.feature_base,
                                                                rhs.metric,
                                                                rhs.summary,
                                                                rhs.direction,
                                                                rhs.basis_scoped,
                                                                rhs.basis_version));
                  const uint32_t lhs_prev = lhs_it == previous.end() ? 0 : lhs_it->second;
                  const uint32_t rhs_prev = rhs_it == previous.end() ? 0 : rhs_it->second;
                  if (lhs_prev != rhs_prev) return lhs_prev > rhs_prev;
                  return std::tie(lhs.metric, lhs.summary, lhs.direction) <
                         std::tie(rhs.metric, rhs.summary, rhs.direction);
              });
    for (const auto& item : unavailable) {
        if (out->size() >= config.dominant_single_cap) return;
        out->push_back(item);
    }
}

double SingleRisk(const std::vector<RelationFusionSingleEvidence>& dominant_single) {
    std::vector<double> strengths;
    for (const auto& item : dominant_single) {
        if (item.available && item.evidence_strength > 0.0) {
            strengths.push_back(item.evidence_strength);
        }
    }
    return SaturatingUnion(strengths);
}

double PatternRisk(const std::vector<RelationFusionPatternScore>& patterns) {
    std::vector<double> strengths;
    for (const auto& pattern : patterns) {
        if (pattern.weighted_score > 0.0) strengths.push_back(pattern.weighted_score);
    }
    return SaturatingUnion(strengths);
}

void AppendDiagnostic(const std::string& item, std::string* diagnostics) {
    if (!diagnostics || item.empty()) return;
    if (!diagnostics->empty()) *diagnostics += ";";
    *diagnostics += item;
}

}  // namespace

BaselineStatus UpdateRelationFusion(const RelationFusionUpdateInput& input,
                                    RelationFusionRuntimeState* state,
                                    RelationFusionResult* out,
                                    uint64_t* evicted_persistence_keys) {
    if (!state || !out || input.source_series_key.empty() || input.feature_base.empty()) {
        return BaselineStatus::kInvalidArgument;
    }
    if (evicted_persistence_keys) *evicted_persistence_keys = 0;

    RelationFusionResult result;
    result.status = BaselineStatus::kOk;
    result.source_series_key = input.source_series_key;
    result.feature_base = input.feature_base;
    result.bucket_id = input.bucket_id;
    if (!input.config.enable_relation_fusion) {
        *out = result;
        return BaselineStatus::kOk;
    }
    if (state->has_last_bucket && input.bucket_id <= state->last_bucket_id) {
        result = state->last_result;
        result.status = BaselineStatus::kOk;
        AppendDiagnostic("relation_fusion_stale_bucket", &result.diagnostics);
        *out = result;
        return BaselineStatus::kOk;
    }

    uint64_t evicted_keys = 0;
    if (state->has_last_bucket && input.bucket_id > state->last_bucket_id + 1) {
        evicted_keys +=
            static_cast<uint64_t>(state->persistence_by_evidence_dir.size());
        state->persistence_by_evidence_dir.clear();
    }

    std::map<std::string, ExpectedEvidence> expected_by_key = BuildExpectedUniverse(input);
    std::set<std::string> metric_universe;
    for (const auto& metric : input.metrics) {
        if (!metric.metric.empty()) metric_universe.insert(metric.metric);
    }
    std::map<std::string, RelationFusionRoutedInput> routed_by_key;
    for (const auto& routed : input.routed_inputs) {
        if (metric_universe.find(routed.routed.metric) == metric_universe.end()) continue;
        const std::string direction = DirectionForRolling(routed.routed.rolling);
        if (!IsMappedDirection(routed.routed.summary, direction)) continue;
        const std::string key = EvidenceKey(routed.routed.source_series_key,
                                            routed.feature_base,
                                            routed.routed.metric,
                                            routed.routed.summary,
                                            direction,
                                            routed.routed.basis_scoped,
                                            routed.routed.basis_version);
        routed_by_key.insert_or_assign(key, routed);
        ExpectedEvidence expected;
        expected.source_series_key = routed.routed.source_series_key;
        expected.feature_base = routed.feature_base;
        expected.metric = routed.routed.metric;
        expected.summary = routed.routed.summary;
        expected.feature_type = routed.routed.feature_type;
        expected.direction = direction;
        expected.basis_scoped = routed.routed.basis_scoped;
        expected.basis_version = routed.routed.basis_version;
        expected.unavailable_reason = "summary_missing";
        expected_by_key.insert_or_assign(key, expected);
    }

    for (auto it = state->persistence_by_evidence_dir.begin();
         it != state->persistence_by_evidence_dir.end();) {
        if (expected_by_key.find(it->first) == expected_by_key.end()) {
            it = state->persistence_by_evidence_dir.erase(it);
            ++evicted_keys;
        } else {
            ++it;
        }
    }
    const std::unordered_map<std::string, uint32_t> previous =
        state->persistence_by_evidence_dir;

    std::vector<RelationFusionSingleEvidence> evidence;
    evidence.reserve(expected_by_key.size());
    std::size_t retained_keys = 0;
    const auto cap = input.config.fusion_persistence_max_keys_per_source;
    // expected_by_key already has a stable lexicographic order; retain its first cap keys without sorting.
    auto remember = [&](const std::string& key, uint32_t persistence) {
        if (cap == 0 || retained_keys < cap) {
            state->persistence_by_evidence_dir.insert_or_assign(key, persistence);
            ++retained_keys;
        } else {
            evicted_keys += state->persistence_by_evidence_dir.erase(key);
        }
    };
    for (const auto& entry : expected_by_key) {
        const std::string& key = entry.first;
        const ExpectedEvidence& expected = entry.second;
        const auto routed_it = routed_by_key.find(key);
        if (routed_it == routed_by_key.end()) {
            remember(key, 0);
            RelationFusionSingleEvidence missing = EmptyEvidence(expected);
            missing.persistence = 0;
            evidence.push_back(std::move(missing));
            continue;
        }

        const RelationFusionRoutedInput& routed = routed_it->second;
        const double normalized =
            input.config.fusion_z_score_cap > 0.0
                ? Clamp01(std::fabs(routed.routed.rolling.z_score) /
                          input.config.fusion_z_score_cap)
                : 0.0;
        std::string unavailable_reason;
        (void)TrustFactor(routed, expected.direction, input.config, &unavailable_reason);
        const bool available = unavailable_reason.empty();
        const auto previous_it = previous.find(key);
        const uint32_t previous_persistence = previous_it == previous.end() ? 0 : previous_it->second;
        const uint32_t next_persistence =
            available && normalized >= input.config.fusion_min_evidence_score ? previous_persistence + 1 : 0;
        remember(key, next_persistence);
        evidence.push_back(BuildEvidence(routed,
                                         expected.direction,
                                         input.config,
                                         next_persistence,
                                         unavailable_reason));
    }

    SelectDominantSingle(evidence, input.config, previous, &result.dominant_single);
    result.pattern_scores = BuildPatternScores(input, evidence);
    result.dominant_pattern = result.pattern_scores;
    LimitVector(&result.dominant_pattern, input.config.dominant_pattern_cap);
    result.single_risk = SingleRisk(result.dominant_single);
    result.pattern_risk = PatternRisk(result.pattern_scores);
    result.relation_risk =
        1.0 - (1.0 - result.single_risk) * (1.0 - result.pattern_risk);
    if (result.single_risk == 0.0 && result.pattern_risk == 0.0) {
        AppendDiagnostic("relation_fusion_no_available_evidence", &result.diagnostics);
    }
    if (cap > 0 && expected_by_key.size() > cap) {
        AppendDiagnostic("relation_fusion_persistence_key_cap_reached",
                         &result.diagnostics);
    }

    state->last_bucket_id = input.bucket_id;
    state->has_last_bucket = true;
    state->last_result = result;
    if (evicted_persistence_keys) *evicted_persistence_keys = evicted_keys;
    *out = std::move(result);
    return BaselineStatus::kOk;
}

}  // namespace baseline
}  // namespace flowsql
