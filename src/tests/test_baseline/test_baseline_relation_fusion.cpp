// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <cassert>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <string>
#include <vector>

#include <plugins/baseline/relation/relation_fusion.h>

using namespace flowsql;
using namespace flowsql::baseline;

namespace {

void AssertNear(double actual, double expected, double eps = 1.0e-9) {
    assert(std::fabs(actual - expected) < eps);
}

RelationFusionMetricContext MetricContext(const std::string& metric) {
    RelationFusionMetricContext ctx;
    ctx.metric = metric;
    ctx.present = true;
    ctx.valid = true;
    return ctx;
}

RelationFusionMetricContext BasisMetricContext(const std::string& metric) {
    RelationFusionMetricContext ctx = MetricContext(metric);
    ctx.has_active_basis = true;
    ctx.basis_version = 1;
    ctx.stable_head_size = 2;
    ctx.stable_head_mix_drift_expected = true;
    return ctx;
}

RelationFusionRoutedInput RoutedInput(const std::string& source,
                                      const std::string& feature_base,
                                      const std::string& metric,
                                      const std::string& summary,
                                      double residual,
                                      double z_score,
                                      const std::string& score_trust_status = "score_ready",
                                      bool can_alert = true,
                                      double effective_confidence = 0.8,
                                      double learning_confidence = 0.8,
                                      bool active_count_from_upstream = true,
                                      bool basis_scoped = false,
                                      uint64_t basis_version = 0,
                                      const std::string& metric_basis_status = "basis_ready") {
    RelationFusionRoutedInput input;
    input.feature_base = feature_base;
    input.metric_basis_status = metric_basis_status;
    input.active_count_from_upstream = active_count_from_upstream;
    input.routed.source_series_key = source;
    input.routed.routed_series_key =
        source + "::" + metric + "::" + summary + "::ratio";
    if (basis_scoped) {
        input.routed.routed_series_key += "::basis:" + std::to_string(basis_version);
    }
    input.routed.metric = metric;
    input.routed.summary = summary;
    input.routed.feature_type = "ratio";
    input.routed.basis_version = basis_version;
    input.routed.basis_scoped = basis_scoped;
    input.routed.rolling.status = BaselineStatus::kOk;
    input.routed.rolling.bucket_id = 100;
    input.routed.rolling.can_score = true;
    input.routed.rolling.can_alert = can_alert;
    input.routed.rolling.residual = residual;
    input.routed.rolling.z_score = z_score;
    input.routed.rolling.score_trust_status = score_trust_status;
    input.routed.rolling.effective_confidence = effective_confidence;
    input.routed.rolling.learning_confidence = learning_confidence;
    return input;
}

RelationFusionUpdateInput BaseUpdate(int64_t bucket_id) {
    RelationFusionUpdateInput input;
    input.source_series_key = "linkA.client_mix";
    input.feature_base = "client_mix";
    input.bucket_id = bucket_id;
    input.metrics.push_back(MetricContext("bps"));
    return input;
}

const RelationFusionPatternScore* FindPattern(const RelationFusionResult& result,
                                              const std::string& pattern) {
    for (const auto& score : result.pattern_scores) {
        if (score.pattern == pattern) return &score;
    }
    return nullptr;
}

const RelationFusionSingleEvidence* FindSingle(const RelationFusionResult& result,
                                               const std::string& summary) {
    for (const auto& evidence : result.dominant_single) {
        if (evidence.summary == summary) return &evidence;
    }
    return nullptr;
}

const RelationFusionSingleEvidence* FindSingle(const RelationFusionResult& result,
                                               const std::string& summary,
                                               const std::string& direction,
                                               const std::string& metric = "") {
    for (const auto& evidence : result.dominant_single) {
        if (evidence.summary == summary && evidence.direction == direction &&
            (metric.empty() || evidence.metric == metric)) {
            return &evidence;
        }
    }
    return nullptr;
}

bool ContainsString(const std::vector<std::string>& values, const std::string& expected) {
    return std::find(values.begin(), values.end(), expected) != values.end();
}

void TestSupportEscapeUsesPersistenceAndPatternSupport() {
    std::printf("[TEST] Relation fusion support_escape uses persistence and support...\n");

    RelationFusionRuntimeConfig config;
    RelationFusionRuntimeState state;

    RelationFusionUpdateInput first = BaseUpdate(100);
    first.config = config;
    first.routed_inputs.push_back(RoutedInput(first.source_series_key,
                                             first.feature_base,
                                             "bps",
                                             "out_of_support_share",
                                             10.0,
                                             5.0));
    first.routed_inputs.push_back(RoutedInput(first.source_series_key,
                                             first.feature_base,
                                             "bps",
                                             "entropy_shannon",
                                             5.0,
                                             2.5));
    RelationFusionResult result;
    assert(UpdateRelationFusion(first, &state, &result) == BaselineStatus::kOk);
    const RelationFusionSingleEvidence* first_core =
        FindSingle(result, "out_of_support_share");
    assert(first_core != nullptr);
    assert(first_core->direction == "up");
    assert(first_core->persistence == 1);
    AssertNear(first_core->normalized_score, 1.0);
    AssertNear(first_core->confidence, 0.8);
    AssertNear(first_core->evidence_strength, 0.4);

    RelationFusionUpdateInput second = first;
    second.bucket_id = 101;
    assert(UpdateRelationFusion(second, &state, &result) == BaselineStatus::kOk);
    const RelationFusionSingleEvidence* second_core =
        FindSingle(result, "out_of_support_share");
    assert(second_core != nullptr);
    assert(second_core->persistence == 2);
    AssertNear(second_core->evidence_strength, 0.8);

    const RelationFusionPatternScore* support_escape =
        FindPattern(result, "support_escape");
    assert(support_escape != nullptr);
    AssertNear(support_escape->score, 1.0);
    AssertNear(support_escape->weighted_score, 0.70);
    assert(result.single_risk > 0.85);
    assert(result.pattern_risk > 0.69);
    assert(result.relation_risk > 0.95);

    std::printf("[PASS] Relation fusion support_escape uses persistence and support\n");
}

void TestTrustGateUsesLearningConfidenceForDegradedEvidence() {
    std::printf("[TEST] Relation fusion degraded trust uses learning confidence...\n");

    RelationFusionRuntimeState state;
    RelationFusionUpdateInput first = BaseUpdate(100);
    first.routed_inputs.push_back(RoutedInput(first.source_series_key,
                                             first.feature_base,
                                             "bps",
                                             "top1_share",
                                             10.0,
                                             5.0,
                                             "drift_learning",
                                             false,
                                             0.0,
                                             0.6));
    RelationFusionResult result;
    assert(UpdateRelationFusion(first, &state, &result) == BaselineStatus::kOk);

    RelationFusionUpdateInput second = first;
    second.bucket_id = 101;
    assert(UpdateRelationFusion(second, &state, &result) == BaselineStatus::kOk);

    const RelationFusionSingleEvidence* top1 = FindSingle(result, "top1_share");
    assert(top1 != nullptr);
    assert(top1->available);
    assert(top1->score_trust_status == "drift_learning");
    AssertNear(top1->confidence, 0.6);
    AssertNear(top1->evidence_strength, 0.15);

    std::printf("[PASS] Relation fusion degraded trust uses learning confidence\n");
}

void TestDistinctGroupFallbackIsUnavailableAndMissingMetricResetsPersistence() {
    std::printf("[TEST] Relation fusion distinct fallback and missing metric reset...\n");

    RelationFusionRuntimeState state;
    RelationFusionUpdateInput first = BaseUpdate(100);
    first.routed_inputs.push_back(RoutedInput(first.source_series_key,
                                             first.feature_base,
                                             "bps",
                                             "distinct_group_count",
                                             10.0,
                                             5.0,
                                             "score_ready",
                                             true,
                                             0.8,
                                             0.8,
                                             false));
    RelationFusionResult result;
    assert(UpdateRelationFusion(first, &state, &result) == BaselineStatus::kOk);
    const RelationFusionSingleEvidence* distinct =
        FindSingle(result, "distinct_group_count");
    assert(distinct != nullptr);
    assert(!distinct->available);
    assert(distinct->unavailable_reason == "distinct_group_count_untrusted");
    AssertNear(distinct->evidence_strength, 0.0);

    RelationFusionUpdateInput second = BaseUpdate(101);
    second.routed_inputs.push_back(RoutedInput(second.source_series_key,
                                              second.feature_base,
                                              "bps",
                                              "top1_share",
                                              10.0,
                                              5.0));
    assert(UpdateRelationFusion(second, &state, &result) == BaselineStatus::kOk);
    const RelationFusionSingleEvidence* top1 = FindSingle(result, "top1_share");
    assert(top1 != nullptr);
    assert(top1->persistence == 1);

    RelationFusionUpdateInput missing = BaseUpdate(102);
    missing.metrics[0].present = false;
    missing.metrics[0].valid = false;
    missing.metrics[0].unavailable_reason = "metric_missing";
    assert(UpdateRelationFusion(missing, &state, &result) == BaselineStatus::kOk);
    top1 = FindSingle(result, "top1_share");
    assert(top1 != nullptr);
    assert(!top1->available);
    assert(top1->persistence == 0);
    assert(top1->unavailable_reason == "metric_missing");

    RelationFusionUpdateInput recovered = second;
    recovered.bucket_id = 103;
    assert(UpdateRelationFusion(recovered, &state, &result) == BaselineStatus::kOk);
    top1 = FindSingle(result, "top1_share");
    assert(top1 != nullptr);
    assert(top1->persistence == 1);

    std::printf("[PASS] Relation fusion distinct fallback and missing metric reset\n");
}

void TestCrossMetricPatternUsesSaturatingUnion() {
    std::printf("[TEST] Relation fusion cross metric pattern uses saturating union...\n");

    RelationFusionRuntimeState state;
    RelationFusionUpdateInput first = BaseUpdate(100);
    first.metrics.push_back(MetricContext("pps"));
    first.routed_inputs.push_back(RoutedInput(first.source_series_key,
                                             first.feature_base,
                                             "bps",
                                             "top1_share",
                                             10.0,
                                             5.0));
    first.routed_inputs.push_back(RoutedInput(first.source_series_key,
                                             first.feature_base,
                                             "pps",
                                             "top1_share",
                                             10.0,
                                             5.0));
    RelationFusionResult result;
    assert(UpdateRelationFusion(first, &state, &result) == BaselineStatus::kOk);

    RelationFusionUpdateInput second = first;
    second.bucket_id = 101;
    assert(UpdateRelationFusion(second, &state, &result) == BaselineStatus::kOk);

    const RelationFusionPatternScore* head =
        FindPattern(result, "head_concentration");
    assert(head != nullptr);
    AssertNear(head->score, 0.96);
    AssertNear(head->weighted_score, 0.672);
    assert(head->metrics_hit.size() == 2);

    std::printf("[PASS] Relation fusion cross metric pattern uses saturating union\n");
}

void TestNegativeEvidenceDrivesLegacyAndHeadPatterns() {
    std::printf("[TEST] Relation fusion negative evidence drives legacy and head patterns...\n");

    RelationFusionRuntimeConfig config;
    config.dominant_single_cap = 20;
    RelationFusionRuntimeState state;
    RelationFusionUpdateInput first = BaseUpdate(100);
    first.config = config;
    first.metrics[0] = BasisMetricContext("bps");
    first.routed_inputs.push_back(RoutedInput(first.source_series_key,
                                             first.feature_base,
                                             "bps",
                                             "stable_headk_coverage",
                                             -5.0,
                                             -5.0,
                                             "score_ready",
                                             true,
                                             0.8,
                                             0.8,
                                             true,
                                             true,
                                             1));
    first.routed_inputs.push_back(RoutedInput(first.source_series_key,
                                             first.feature_base,
                                             "bps",
                                             "entropy_shannon",
                                             -5.0,
                                             -5.0));
    first.routed_inputs.push_back(RoutedInput(first.source_series_key,
                                             first.feature_base,
                                             "bps",
                                             "top1_share",
                                             5.0,
                                             5.0));
    first.routed_inputs.push_back(RoutedInput(first.source_series_key,
                                             first.feature_base,
                                             "bps",
                                             "stable_headk_mix_drift",
                                             5.0,
                                             5.0,
                                             "score_ready",
                                             true,
                                             0.8,
                                             0.8,
                                             true,
                                             true,
                                             1));
    RelationFusionResult result;
    assert(UpdateRelationFusion(first, &state, &result) == BaselineStatus::kOk);

    RelationFusionUpdateInput second = first;
    second.bucket_id = 101;
    assert(UpdateRelationFusion(second, &state, &result) == BaselineStatus::kOk);

    const RelationFusionSingleEvidence* stable_down =
        FindSingle(result, "stable_headk_coverage", "down", "bps");
    assert(stable_down != nullptr);
    assert(stable_down->available);
    AssertNear(stable_down->normalized_score, 1.0);
    AssertNear(stable_down->evidence_strength, 0.8);

    const RelationFusionSingleEvidence* entropy_down =
        FindSingle(result, "entropy_shannon", "down", "bps");
    assert(entropy_down != nullptr);
    assert(entropy_down->available);
    AssertNear(entropy_down->evidence_strength, 0.8);

    const RelationFusionPatternScore* legacy =
        FindPattern(result, "legacy_head_dilution");
    assert(legacy != nullptr);
    AssertNear(legacy->score, 0.8);
    assert(ContainsString(legacy->supporting_features, "stable_headk_coverage:down"));

    const RelationFusionPatternScore* head =
        FindPattern(result, "head_concentration");
    assert(head != nullptr);
    AssertNear(head->score, 1.0);
    assert(ContainsString(head->supporting_features, "entropy_shannon:down"));

    const RelationFusionPatternScore* stable_mix =
        FindPattern(result, "stable_head_mix_shift");
    assert(stable_mix != nullptr);
    AssertNear(stable_mix->score, 0.4);
    assert(ContainsString(stable_mix->supporting_features, "stable_headk_mix_drift:up"));

    std::printf("[PASS] Relation fusion negative evidence drives legacy and head patterns\n");
}

void TestRoutedInputsOutsideMetricUniverseAreIgnored() {
    std::printf("[TEST] Relation fusion ignores routed inputs outside metric universe...\n");

    RelationFusionRuntimeConfig config;
    config.dominant_single_cap = 20;
    RelationFusionRuntimeState state;
    RelationFusionUpdateInput first = BaseUpdate(100);
    first.config = config;
    first.routed_inputs.push_back(RoutedInput(first.source_series_key,
                                             first.feature_base,
                                             "pps",
                                             "top1_share",
                                             5.0,
                                             5.0));
    RelationFusionResult result;
    assert(UpdateRelationFusion(first, &state, &result) == BaselineStatus::kOk);

    RelationFusionUpdateInput second = first;
    second.bucket_id = 101;
    assert(UpdateRelationFusion(second, &state, &result) == BaselineStatus::kOk);

    assert(FindSingle(result, "top1_share", "up", "pps") == nullptr);
    const RelationFusionPatternScore* head =
        FindPattern(result, "head_concentration");
    assert(head == nullptr);
    AssertNear(result.relation_risk, 0.0);

    std::printf("[PASS] Relation fusion ignores routed inputs outside metric universe\n");
}

void TestPersistencePrunesStaleEvidenceUniverse() {
    std::printf("[TEST] Relation fusion prunes stale evidence universe...\n");

    RelationFusionRuntimeConfig config;
    config.dominant_single_cap = 20;
    RelationFusionRuntimeState state;

    RelationFusionUpdateInput first = BaseUpdate(100);
    first.config = config;
    first.metrics[0] = BasisMetricContext("bps");
    first.routed_inputs.push_back(RoutedInput(first.source_series_key,
                                             first.feature_base,
                                             "bps",
                                             "stable_headk_coverage",
                                             5.0,
                                             5.0,
                                             "score_ready",
                                             true,
                                             0.8,
                                             0.8,
                                             true,
                                             true,
                                             1));
    RelationFusionResult result;
    uint64_t evicted = 0;
    assert(UpdateRelationFusion(first, &state, &result, &evicted) ==
           BaselineStatus::kOk);
    assert(evicted == 0);
    bool has_basis_one = false;
    for (const auto& entry : state.persistence_by_evidence_dir) {
        if (entry.first.find("::basis:1") != std::string::npos) {
            has_basis_one = true;
        }
    }
    assert(has_basis_one);

    RelationFusionUpdateInput second = first;
    second.bucket_id = 101;
    second.metrics[0].basis_version = 2;
    second.routed_inputs.clear();
    second.routed_inputs.push_back(RoutedInput(second.source_series_key,
                                              second.feature_base,
                                              "bps",
                                              "stable_headk_coverage",
                                              5.0,
                                              5.0,
                                              "score_ready",
                                              true,
                                              0.8,
                                              0.8,
                                              true,
                                              true,
                                              2));
    assert(UpdateRelationFusion(second, &state, &result, &evicted) ==
           BaselineStatus::kOk);
    assert(evicted > 0);
    bool still_has_basis_one = false;
    bool has_basis_two = false;
    for (const auto& entry : state.persistence_by_evidence_dir) {
        if (entry.first.find("::basis:1") != std::string::npos) {
            still_has_basis_one = true;
        }
        if (entry.first.find("::basis:2") != std::string::npos) {
            has_basis_two = true;
        }
    }
    assert(!still_has_basis_one);
    assert(has_basis_two);

    std::printf("[PASS] Relation fusion prunes stale evidence universe\n");
}

void TestPersistenceCapacityIsEnforced() {
    RelationFusionRuntimeState state;
    auto input = BaseUpdate(100);
    input.config.fusion_persistence_max_keys_per_source = 3;
    input.config.dominant_single_cap = 20;
    input.routed_inputs.push_back(RoutedInput(input.source_series_key, input.feature_base, "bps", "top1_share", 5, 5));
    RelationFusionResult result;
    uint64_t evicted = 0;
    assert(UpdateRelationFusion(input, &state, &result, &evicted) == BaselineStatus::kOk);
    assert(state.persistence_by_evidence_dir.size() == 3);
    assert(result.diagnostics.find("persistence_key_cap_reached") != std::string::npos);
    const auto retained = state.persistence_by_evidence_dir;
    const auto last = state.last_bucket_id;
    assert(UpdateRelationFusion(input, &state, &result) == BaselineStatus::kOk);
    assert(state.persistence_by_evidence_dir == retained && state.last_bucket_id == last);
    input.bucket_id = 101;
    assert(UpdateRelationFusion(input, &state, &result) == BaselineStatus::kOk);
    assert(state.persistence_by_evidence_dir.size() == 3);
    for (const auto& entry : retained) assert(state.persistence_by_evidence_dir.count(entry.first) == 1);
    input.bucket_id = 104;
    input.routed_inputs.clear();
    assert(UpdateRelationFusion(input, &state, &result) == BaselineStatus::kOk);
    assert(state.persistence_by_evidence_dir.size() == 3);
    for (const auto& entry : state.persistence_by_evidence_dir) assert(entry.second == 0);
    // Zero preserves the pre-existing unlimited internal configuration semantics.
    input.config.fusion_persistence_max_keys_per_source = 0;
    input.bucket_id = 105;
    assert(UpdateRelationFusion(input, &state, &result) == BaselineStatus::kOk);
    assert(state.persistence_by_evidence_dir.size() > 3);
    input.config.fusion_persistence_max_keys_per_source = 2;
    input.bucket_id = 106;
    assert(UpdateRelationFusion(input, &state, &result, &evicted) == BaselineStatus::kOk);
    assert(state.persistence_by_evidence_dir.size() == 2 && evicted > 0);
    std::printf("[PASS] Relation fusion enforces persistence capacity\n");
}

}  // namespace

int main() {
    TestSupportEscapeUsesPersistenceAndPatternSupport();
    TestTrustGateUsesLearningConfidenceForDegradedEvidence();
    TestDistinctGroupFallbackIsUnavailableAndMissingMetricResetsPersistence();
    TestCrossMetricPatternUsesSaturatingUnion();
    TestNegativeEvidenceDrivesLegacyAndHeadPatterns();
    TestRoutedInputsOutsideMetricUniverseAreIgnored();
    TestPersistencePrunesStaleEvidenceUniverse();
    TestPersistenceCapacityIsEnforced();
    return 0;
}
