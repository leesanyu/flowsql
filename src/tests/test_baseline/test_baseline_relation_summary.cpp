// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include <plugins/baseline/relation/relation_summary.h>
#include <plugins/baseline/relation/routed_summary.h>

using namespace flowsql;
using namespace flowsql::baseline;

namespace {

void AssertNear(double actual, double expected) {
    const double diff = std::fabs(actual - expected);
    assert(diff < 1.0e-9);
}

RelationBootstrapBlock MakeBlock() {
    RelationBootstrapBlock block;
    block.bucket_id = 42;
    block.group_idx = {1, 2, 999};

    RelationBootstrapMetric metric;
    metric.metric = "bps";
    metric.total = 100.0;
    metric.active_count = 3;
    metric.values_by_group = {50.0, 30.0, 20.0};
    block.metrics.push_back(metric);
    return block;
}

const RelationProjectedSummary* FindSummary(
    const std::vector<RelationProjectedSummary>& summaries,
    const std::string& name) {
    for (const auto& summary : summaries) {
        if (summary.summary_name == name) return &summary;
    }
    return nullptr;
}

void TestCommonSummariesDoNotRequireBasis() {
    std::printf("[TEST] Relation common summaries do not require basis...\n");

    RelationSummaryProjectionOptions options;
    options.summary_policy.k_head = 2;
    options.other_group_idxs = {999};

    std::vector<RelationProjectedSummary> summaries;
    const bool ok =
        ProjectRelationMetricSummaries(MakeBlock(), 0, "bps", options, &summaries);
    assert(ok);
    assert(summaries.size() == 4);

    const RelationProjectedSummary* entropy = FindSummary(summaries, "entropy_shannon");
    assert(entropy != nullptr);
    assert(entropy->task_kind == BaselineTaskKind::kValue);
    assert(entropy->feature_type == "value_basic");
    assert(!entropy->basis_scoped);
    const double expected_entropy =
        -(0.5 * std::log(0.5) + 0.3 * std::log(0.3) + 0.2 * std::log(0.2));
    AssertNear(entropy->value, expected_entropy);

    const RelationProjectedSummary* distinct = FindSummary(summaries, "distinct_group_count");
    assert(distinct != nullptr);
    assert(distinct->task_kind == BaselineTaskKind::kValue);
    AssertNear(distinct->value, 3.0);

    const RelationProjectedSummary* top1 = FindSummary(summaries, "top1_share");
    assert(top1 != nullptr);
    assert(top1->task_kind == BaselineTaskKind::kRatio);
    assert(top1->feature_type == "ratio");
    AssertNear(top1->numerator, 50.0);
    AssertNear(top1->denominator, 100.0);

    const RelationProjectedSummary* headk = FindSummary(summaries, "headk_share");
    assert(headk != nullptr);
    assert(headk->task_kind == BaselineTaskKind::kRatio);
    AssertNear(headk->numerator, 80.0);
    AssertNear(headk->denominator, 100.0);

    assert(FindSummary(summaries, "out_of_support_share") == nullptr);
    std::printf("[PASS] Relation common summaries do not require basis\n");
}

void TestBasisScopedSummariesRequireBasis() {
    std::printf("[TEST] Relation basis-scoped summaries require basis...\n");

    RelationServiceBasis basis;
    basis.metric_name = "bps";
    basis.basis_version = 7;
    basis.k_head = 2;
    basis.other_group_idxs = {999};
    basis.support_explicit = {1, 2};
    basis.stable_head = {1, 2};
    basis.head_proto_q = {0.6, 0.4};

    RelationSummaryProjectionOptions options;
    options.summary_policy.k_head = 2;
    options.other_group_idxs = {999};
    options.basis = &basis;

    std::vector<RelationProjectedSummary> summaries;
    const bool ok =
        ProjectRelationMetricSummaries(MakeBlock(), 0, "bps", options, &summaries);
    assert(ok);

    const RelationProjectedSummary* out_support =
        FindSummary(summaries, "out_of_support_share");
    assert(out_support != nullptr);
    assert(out_support->basis_scoped);
    assert(out_support->basis_version == 7);
    assert(out_support->task_kind == BaselineTaskKind::kRatio);
    AssertNear(out_support->numerator, 20.0);
    AssertNear(out_support->denominator, 100.0);

    const RelationProjectedSummary* stable_coverage =
        FindSummary(summaries, "stable_headk_coverage");
    assert(stable_coverage != nullptr);
    assert(stable_coverage->basis_scoped);
    AssertNear(stable_coverage->numerator, 80.0);
    AssertNear(stable_coverage->denominator, 100.0);

    const RelationProjectedSummary* stable_g0 =
        FindSummary(summaries, "stable_g_share_0");
    assert(stable_g0 != nullptr);
    assert(stable_g0->basis_scoped);
    AssertNear(stable_g0->numerator, 50.0);
    AssertNear(stable_g0->denominator, 100.0);

    const RelationProjectedSummary* mix_drift =
        FindSummary(summaries, "stable_headk_mix_drift");
    assert(mix_drift != nullptr);
    assert(mix_drift->basis_scoped);
    assert(mix_drift->task_kind == BaselineTaskKind::kValue);
    AssertNear(mix_drift->value, 0.025);

    std::printf("[PASS] Relation basis-scoped summaries require basis\n");
}

void TestRoutedSummaryIdentityUsesFixedScope() {
    std::printf("[TEST] Relation routed summary identity uses fixed scope...\n");

    RelationRoutedSummaryIdentity universal = MakeRelationRoutedSummaryIdentity(
        "linkA.client_mix",
        "bps",
        "top1_share",
        BaselineTaskKind::kRatio,
        7);
    assert(universal.feature_type == "ratio");
    assert(!universal.basis_scoped);
    assert(universal.basis_version == 0);
    assert(universal.routed_series_key == "linkA.client_mix::bps::top1_share::ratio");

    RelationRoutedSummaryIdentity scoped = MakeRelationRoutedSummaryIdentity(
        "linkA.client_mix",
        "bps",
        "stable_g_share_0",
        BaselineTaskKind::kRatio,
        7);
    assert(scoped.feature_type == "ratio");
    assert(scoped.basis_scoped);
    assert(scoped.basis_version == 7);
    assert(scoped.routed_series_key ==
           "linkA.client_mix::bps::stable_g_share_0::ratio::basis:7");

    assert(IsBasisScopedRelationSummary("out_of_support_share"));
    assert(IsBasisScopedRelationSummary("stable_headk_coverage"));
    assert(IsBasisScopedRelationSummary("stable_g_share_3"));
    assert(IsBasisScopedRelationSummary("stable_headk_mix_drift"));
    assert(!IsBasisScopedRelationSummary("entropy_shannon"));
    assert(!IsBasisScopedRelationSummary("distinct_group_count"));
    assert(!IsBasisScopedRelationSummary("top1_share"));
    assert(!IsBasisScopedRelationSummary("headk_share"));

    std::printf("[PASS] Relation routed summary identity uses fixed scope\n");
}

void TestRoutedSummaryTaskSpecMatchesBootstrapRule() {
    std::printf("[TEST] Relation routed summary task spec matches bootstrap rule...\n");

    RelationTaskCreateSpec spec;
    spec.task_spec.task_id = "relation_task";
    spec.task_spec.feature_id = "relation_feature";
    spec.task_spec.feature_base = "relation_base";
    spec.task_spec.calendar_ref.calendar_id = "calendar_a";
    spec.task_spec.calendar_ref.calendar_version = "v1";
    spec.clock_spec.delta = 60;
    spec.clock_spec.tz = "Asia/Shanghai";

    BaselineTaskSpec routed =
        MakeRoutedSummaryTaskSpec(spec, "bps", "top1_share", BaselineTaskKind::kRatio);
    assert(routed.task_id == "relation_task::bps::top1_share");
    assert(routed.name == routed.task_id);
    assert(routed.task_kind == "ratio");
    assert(routed.feature_id == "relation_base.bps.top1_share");
    assert(routed.feature == routed.feature_id);
    assert(routed.feature_type == "ratio");
    assert(routed.profile == "rate_core");
    assert(routed.clock_spec.bucket_seconds == 60);
    assert(routed.clock_spec.timezone == "Asia/Shanghai");
    assert(routed.calendar_ref.calendar_id == "calendar_a");
    assert(routed.calendar_ref.calendar_version == "v1");

    routed = MakeRoutedSummaryTaskSpec(
        spec, "bps", "entropy_shannon", BaselineTaskKind::kValue);
    assert(routed.task_kind == "value");
    assert(routed.feature_type == "value_basic");
    assert(routed.profile == "default");

    std::printf("[PASS] Relation routed summary task spec matches bootstrap rule\n");
}

void TestRoutedSeedMaterializationUsesFallbackBasisVersion() {
    std::printf("[TEST] Relation routed seed materialization uses fallback basis version...\n");

    RelationTaskCreateSpec spec;
    spec.task_spec.task_id = "relation_task";
    spec.task_spec.feature_id = "relation_feature";
    spec.task_spec.feature_base = "relation_base";
    spec.clock_spec.delta = 60;
    spec.clock_spec.tz = "UTC";

    RelationRoutedBootstrapSeed old_seed;
    old_seed.metric_name = "bps";
    old_seed.summary_name = "stable_g_share_0";
    old_seed.task_kind = BaselineTaskKind::kRatio;
    old_seed.seed_status = BootstrapSeedStatus::kPartial;
    old_seed.task_identity.task_id = "relation_task::bps::stable_g_share_0";
    old_seed.task_identity.task_kind = "ratio";
    old_seed.task_identity.feature_type = "ratio";
    old_seed.task_identity.feature_id = "relation_base.bps.stable_g_share_0";
    old_seed.task_identity.profile = "rate_core";
    old_seed.clock_spec.bucket_seconds = 60;
    old_seed.clock_spec.timezone = "UTC";

    RelationRoutedBootstrapSeedMaterialization materialized;
    assert(MaterializeRelationRoutedBootstrapSeed(
               spec, "linkA.client_mix", old_seed, 7, &materialized) ==
           BaselineStatus::kOk);
    assert(materialized.routed_series_key ==
           "linkA.client_mix::bps::stable_g_share_0::ratio::basis:7");
    assert(materialized.task_spec.task_id ==
           "relation_task::bps::stable_g_share_0");
    assert(materialized.seed.artifact_kind == BootstrapArtifactKind::kRatio);
    assert(materialized.seed.series_key == materialized.routed_series_key);
    assert(materialized.seed.task_identity.feature_id ==
           "relation_base.bps.stable_g_share_0");

    old_seed.summary_name = "top1_share";
    assert(MaterializeRelationRoutedBootstrapSeed(
               spec, "linkA.client_mix", old_seed, 0, &materialized) ==
           BaselineStatus::kOk);
    assert(materialized.routed_series_key ==
           "linkA.client_mix::bps::top1_share::ratio");

    std::printf("[PASS] Relation routed seed materialization uses fallback basis version\n");
}

}  // namespace

int main() {
    for (double mass : {-1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        auto block = MakeBlock();
        block.metrics[0].values_by_group[1] = mass;
        std::vector<RelationProjectedSummary> summaries(1);
        assert(!ProjectRelationMetricSummaries(block, 0, "bps", {}, &summaries));
        assert(summaries.empty());
    }
    auto block = MakeBlock();
    std::vector<RelationProjectedSummary> summaries;
    assert(!ProjectRelationMetricSummaries(block, 0, "wrong", {}, &summaries));
    block.metrics[0].values_by_group.push_back(0.0);
    assert(!ProjectRelationMetricSummaries(block, 0, "bps", {}, &summaries));
    block = MakeBlock();
    block.metrics[0].total = 200.0;
    assert(ProjectRelationMetricSummaries(block, 0, "bps", {}, &summaries));
    TestCommonSummariesDoNotRequireBasis();
    TestBasisScopedSummariesRequireBasis();
    TestRoutedSummaryIdentityUsesFixedScope();
    TestRoutedSummaryTaskSpecMatchesBootstrapRule();
    TestRoutedSeedMaterializationUsesFallbackBasisVersion();
    return 0;
}
