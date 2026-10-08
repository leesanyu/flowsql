// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <cassert>
#include <cstdio>
#include <limits>
#include <utility>
#include <vector>

#include <common/error_code.h>
#include <framework/interfaces/ibaseline_types.h>
#include <plugins/baseline/relation/relation_basis.h>
#include <plugins/baseline/relation/relation_basis_state.h>

using namespace flowsql;
using namespace flowsql::baseline;

namespace {

RelationBasisBuildInput MakeBuildInput() {
    RelationBasisBuildInput input;
    input.basis_version = 1;
    input.feature_base = "client_mix";
    input.metric_name = "bps";
    input.group_space_id = "client";
    input.support_policy.k_support = 10;
    input.support_policy.min_hist_share = 0.5;
    input.support_policy.min_active_ratio = 0.0;
    input.summary_policy.k_head = 2;
    input.summary_policy.k_stable = 2;
    input.valid_bucket_count = 1;
    return input;
}

RelationRollingObservation MakeObservation() {
    RelationRollingObservation obs;
    obs.series_key = "linkA.client_mix";
    obs.bucket_id = 100;
    obs.group_idx = {1, 2, 3};

    RelationBootstrapMetric metric;
    metric.metric = "bps";
    metric.total = 100.0;
    metric.active_count = 3;
    metric.values_by_group = {60.0, 30.0, 10.0};
    obs.metrics.push_back(metric);
    return obs;
}

RelationRollingObservation MakeObservationWithGroups(int64_t bucket_id,
                                                     std::vector<uint32_t> groups,
                                                     std::vector<double> values) {
    RelationRollingObservation obs;
    obs.series_key = "linkA.client_mix";
    obs.bucket_id = bucket_id;
    obs.group_idx = std::move(groups);

    RelationBootstrapMetric metric;
    metric.metric = "bps";
    metric.total = 0.0;
    metric.active_count = static_cast<uint32_t>(values.size());
    for (double value : values) metric.total += value;
    metric.values_by_group = std::move(values);
    obs.metrics.push_back(metric);
    return obs;
}

RelationServiceBasis MakeActiveBasis() {
    RelationServiceBasis basis;
    basis.basis_version = 1;
    basis.feature_base = "client_mix";
    basis.metric_name = "bps";
    basis.group_space_id = "client";
    basis.k_head = 3;
    basis.support_explicit = {1, 2, 3};
    basis.stable_head = {1, 2};
    basis.head_proto_q = {0.6, 0.4};
    return basis;
}

void TestBuilderUsesExternalTotalMassDenominator() {
    std::printf("[TEST] Relation basis builder uses external total mass denominator...\n");

    RelationBasisBuildInput input = MakeBuildInput();
    input.total_hist_mass_denominator = 100.0;
    input.group_stats.push_back(RelationGroupHistoryStat{1, 40.0, 1});

    RelationServiceBasis basis;
    assert(RelationBasisBuilder::BuildServiceBasis(input, &basis) == error::OK);
    assert(basis.support_explicit.empty());

    input.total_hist_mass_denominator = 0.0;
    assert(RelationBasisBuilder::BuildServiceBasis(input, &basis) == error::OK);
    assert(basis.support_explicit.size() == 1);
    assert(basis.support_explicit[0] == 1);

    std::printf("[PASS] Relation basis builder uses external total mass denominator\n");
}

void TestStreamAccumulatorBuildsConservativeInput() {
    std::printf("[TEST] Relation stream accumulator builds conservative input...\n");

    RelationStreamBasisConfig config;
    config.max_groups = 2;
    config.threshold_margin = 1.0;

    RelationStreamBasisAccumulator accumulator(config);
    assert(accumulator.Observe(MakeObservation(), 0) == BaselineStatus::kOk);
    assert(accumulator.group_count() <= 2);
    assert(accumulator.valid_bucket_count() == 1);
    assert(accumulator.total_mass() == 100.0);
    assert(accumulator.Observe(MakeObservation(), 0) == BaselineStatus::kInvalidArgument);
    auto older = MakeObservation();
    older.bucket_id = 99;
    assert(accumulator.Observe(older, 0) == BaselineStatus::kInvalidArgument);
    RelationBootstrapBlock block;
    block.bucket_id = 100;
    block.group_idx = older.group_idx;
    block.metrics = older.metrics;
    assert(accumulator.Observe(block, 0) == BaselineStatus::kInvalidArgument);
    assert(accumulator.total_mass() == 100.0 && accumulator.valid_bucket_count() == 1);
    for (double mass : {-1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        auto invalid = MakeObservation();
        invalid.bucket_id = 101;
        invalid.metrics[0].values_by_group[1] = mass;
        assert(accumulator.Observe(invalid, 0) == BaselineStatus::kInvalidArgument);
        block.bucket_id = invalid.bucket_id;
        block.metrics = invalid.metrics;
        assert(accumulator.Observe(block, 0) == BaselineStatus::kInvalidArgument);
        assert(accumulator.total_mass() == 100.0 && accumulator.valid_bucket_count() == 1);
    }
    auto misaligned = MakeObservation();
    misaligned.bucket_id = 101;
    misaligned.metrics[0].values_by_group.push_back(0.0);
    assert(accumulator.Observe(misaligned, 0) == BaselineStatus::kInvalidArgument);
    misaligned = MakeObservation();
    misaligned.bucket_id = 101;
    misaligned.metrics[0].total = 200.0;
    assert(accumulator.Observe(misaligned, 0) == BaselineStatus::kOk);

    RelationBasisBuildInput input = MakeBuildInput();
    input.support_policy.min_hist_share = 0.2;
    assert(accumulator.BuildConservativeInput(input, &input) == BaselineStatus::kOk);
    assert(input.total_hist_mass_denominator == 300.0);
    assert(input.group_stats.size() <= 2);

    RelationServiceBasis basis;
    assert(RelationBasisBuilder::BuildServiceBasis(input, &basis) == error::OK);
    assert(!basis.support_explicit.empty());
    assert(basis.support_explicit[0] == 1);

    std::printf("[PASS] Relation stream accumulator builds conservative input\n");
}

void TestRuntimeCreatesInitialBasisAndPromotesReady() {
    std::printf("[TEST] Relation basis runtime creates initial basis and promotes ready...\n");

    RelationBasisRuntimeConfig config;
    config.stream.max_groups = 8;
    config.stream.threshold_margin = 1.0;
    config.collect_min_buckets = 1;
    config.ready_min_buckets = 2;
    config.refresh_interval_buckets = 1;
    config.candidate_min_coverage_ratio = 0.0;
    config.min_stable_refresh_count = 1;

    RelationBasisRuntimeState runtime(config);
    RelationBasisBuildInput input = MakeBuildInput();
    input.support_policy.min_hist_share = 0.1;
    input.support_policy.k_support = 3;
    input.summary_policy.k_stable = 2;

    assert(runtime.Observe(MakeObservationWithGroups(100, {1, 2, 3}, {60, 30, 10}), 0) ==
           BaselineStatus::kOk);
    RelationBasisRefreshDecision decision = runtime.MaybeRefresh(input, 100);
    assert(decision.status == BaselineStatus::kOk);
    assert(decision.basis_updated);
    assert(runtime.basis_status() == RelationBasisStatus::kBasisWarming);
    assert(runtime.active_basis() != nullptr);
    assert(runtime.active_basis()->basis_version == 1);

    assert(runtime.Observe(MakeObservationWithGroups(101, {1, 2, 3}, {60, 30, 10}), 0) ==
           BaselineStatus::kOk);
    decision = runtime.MaybeRefresh(input, 101);
    assert(decision.status == BaselineStatus::kOk);
    assert(runtime.basis_status() == RelationBasisStatus::kBasisReady);

    std::printf("[PASS] Relation basis runtime creates initial basis and promotes ready\n");
}

void TestRuntimeRequiresConsecutiveStableRefreshes() {
    std::printf("[TEST] Relation basis runtime requires consecutive stable refreshes...\n");

    RelationBasisRuntimeConfig config;
    config.stream.max_groups = 8;
    config.stream.threshold_margin = 1.0;
    config.collect_min_buckets = 1;
    config.ready_min_buckets = 1;
    config.refresh_interval_buckets = 1;
    config.candidate_min_coverage_ratio = 0.0;
    config.min_stable_refresh_count = 2;

    RelationBasisRuntimeState runtime(config);
    RelationBasisBuildInput input = MakeBuildInput();
    input.support_policy.min_hist_share = 0.1;
    input.support_policy.k_support = 3;
    input.summary_policy.k_stable = 2;

    assert(runtime.Observe(MakeObservationWithGroups(100, {1, 2, 3}, {60, 30, 10}), 0) ==
           BaselineStatus::kOk);
    RelationBasisRefreshDecision decision = runtime.MaybeRefresh(input, 100);
    assert(decision.status == BaselineStatus::kOk);
    assert(decision.evaluated);
    assert(!decision.basis_updated);
    assert(runtime.active_basis() == nullptr);
    assert(runtime.basis_status() == RelationBasisStatus::kCollecting);

    assert(runtime.Observe(MakeObservationWithGroups(101, {1, 2, 3}, {60, 30, 10}), 0) ==
           BaselineStatus::kOk);
    decision = runtime.MaybeRefresh(input, 101);
    assert(decision.status == BaselineStatus::kOk);
    assert(decision.basis_updated);
    assert(runtime.active_basis() != nullptr);
    assert(runtime.active_basis()->basis_version == 1);
    assert(runtime.active_basis()->support_explicit.size() == 3);

    std::printf("[PASS] Relation basis runtime requires consecutive stable refreshes\n");
}

void TestRuntimeAppliesReplacementCapAndFiniteHandover() {
    std::printf("[TEST] Relation basis runtime applies replacement cap and finite handover...\n");

    RelationBasisRuntimeConfig config;
    config.stream.max_groups = 8;
    config.stream.threshold_margin = 1.0;
    config.collect_min_buckets = 1;
    config.ready_min_buckets = 1;
    config.refresh_interval_buckets = 1;
    config.candidate_min_coverage_ratio = 0.0;
    config.replacement_cap_ratio = 0.5;
    config.replacement_cap_max = 1;
    config.handover_warmup_buckets = 2;
    config.min_stable_refresh_count = 1;

    RelationBasisRuntimeState runtime(config);
    assert(runtime.LoadSeedBasis(MakeActiveBasis(), RelationBasisStatus::kBasisReady) ==
           BaselineStatus::kOk);

    RelationBasisBuildInput input = MakeBuildInput();
    input.support_policy.min_hist_share = 0.1;
    input.support_policy.k_support = 3;
    input.summary_policy.k_stable = 2;

    assert(runtime.Observe(MakeObservationWithGroups(200, {1, 2, 4}, {55, 30, 15}), 0) ==
           BaselineStatus::kOk);
    RelationBasisRefreshDecision decision = runtime.MaybeRefresh(input, 200);
    assert(decision.status == BaselineStatus::kOk);
    assert(decision.basis_updated);
    assert(decision.handover_started);
    assert(runtime.basis_status() == RelationBasisStatus::kHandoverWarming);
    assert(runtime.active_basis() != nullptr);
    assert(runtime.active_basis()->basis_version == 2);

    decision = runtime.MaybeRefresh(input, 202);
    assert(decision.status == BaselineStatus::kOk);
    assert(runtime.basis_status() == RelationBasisStatus::kBasisReady);

    RelationBasisRuntimeState reject_runtime(config);
    assert(reject_runtime.LoadSeedBasis(MakeActiveBasis(), RelationBasisStatus::kBasisReady) ==
           BaselineStatus::kOk);
    assert(reject_runtime.Observe(MakeObservationWithGroups(300, {4, 5, 6}, {40, 35, 25}), 0) ==
           BaselineStatus::kOk);
    decision = reject_runtime.MaybeRefresh(input, 300);
    assert(decision.status == BaselineStatus::kOk);
    assert(!decision.basis_updated);
    assert(decision.rejected_by_replacement_cap);
    assert(reject_runtime.basis_status() == RelationBasisStatus::kBasisReady);
    assert(reject_runtime.active_basis()->basis_version == 1);

    std::printf("[PASS] Relation basis runtime applies replacement cap and finite handover\n");
}

}  // namespace

int main() {
    TestBuilderUsesExternalTotalMassDenominator();
    TestStreamAccumulatorBuildsConservativeInput();
    TestRuntimeCreatesInitialBasisAndPromotesReady();
    TestRuntimeRequiresConsecutiveStableRefreshes();
    TestRuntimeAppliesReplacementCapAndFiniteHandover();
    return 0;
}
