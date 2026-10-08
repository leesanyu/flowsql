// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <framework/interfaces/ibaseline_service.h>
#include <plugins/baseline/bootstrap/bootstrap_engine.h>
#include <plugins/baseline/task/baseline_task_base.h>
#include <plugins/baseline/task/ratio_task.h>
#include <plugins/baseline/task/relation_task.h>
#include <plugins/baseline/task/task_registry.h>
#include <plugins/baseline/task/value_task.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

using namespace flowsql;
using namespace flowsql::baseline;

namespace {

void AssertNear(double actual, double expected) {
    const double diff = actual > expected ? actual - expected : expected - actual;
    assert(diff < 1.0e-9);
}

bool ContainsString(const std::vector<std::string>& values, const std::string& expected) {
    return std::find(values.begin(), values.end(), expected) != values.end();
}

const RelationFusionPatternMetadata* FindPatternMetadata(
    const RelationFusionBootstrapMetadata& metadata,
    const std::string& pattern) {
    for (const auto& item : metadata.pattern_metadata) {
        if (item.pattern == pattern) return &item;
    }
    return nullptr;
}

void AssertRelationFusionMetadata(const RelationFusionBootstrapMetadata& metadata) {
    assert(metadata.metadata_version == 1);
    assert(metadata.feature_base == "client_mix");
    assert(!metadata.summary_metadata.empty());
    assert(metadata.pattern_metadata.size() == 4);

    bool has_support_summary = false;
    bool has_top1_summary = false;
    for (const auto& summary : metadata.summary_metadata) {
        if (summary.metric_name == "bps" &&
            summary.summary_name == "out_of_support_share") {
            has_support_summary = true;
            assert(summary.task_kind == BaselineTaskKind::kRatio);
            assert(summary.basis_scoped);
            assert(summary.basis_version == 1);
        }
        if (summary.metric_name == "bps" && summary.summary_name == "top1_share") {
            has_top1_summary = true;
            assert(summary.task_kind == BaselineTaskKind::kRatio);
            assert(!summary.basis_scoped);
            assert(summary.basis_version == 0);
        }
    }
    assert(has_support_summary);
    assert(has_top1_summary);

    const RelationFusionPatternMetadata* support_escape =
        FindPatternMetadata(metadata, "support_escape");
    assert(support_escape != nullptr);
    assert(support_escape->scope == "relation_local");
    AssertNear(support_escape->pattern_weight, 0.70);
    assert(ContainsString(support_escape->required_summaries,
                          "out_of_support_share:up"));
    assert(ContainsString(support_escape->optional_summaries,
                          "stable_headk_coverage:down"));
    assert(ContainsString(support_escape->oppose_summaries, "top1_share:up"));
    assert(ContainsString(support_escape->metrics, "bps"));

    const RelationFusionPatternMetadata* stable_head_mix_shift =
        FindPatternMetadata(metadata, "stable_head_mix_shift");
    assert(stable_head_mix_shift != nullptr);
    assert(stable_head_mix_shift->scope == "relation_local");
    AssertNear(stable_head_mix_shift->pattern_weight, 0.85);
    assert(ContainsString(stable_head_mix_shift->required_summaries,
                          "stable_headk_mix_drift:up"));
    assert(ContainsString(stable_head_mix_shift->oppose_summaries,
                          "out_of_support_share:up"));
    assert(ContainsString(stable_head_mix_shift->metrics, "bps"));
}

template <typename T, typename = void>
struct HasRequestRebuild : std::false_type {};

template <typename T>
struct HasRequestRebuild<T, std::void_t<decltype(&T::RequestRebuild)>> : std::true_type {};

template <typename T, typename = void>
struct HasValueSubmitObservation : std::false_type {};

template <typename T>
struct HasValueSubmitObservation<
    T,
    std::void_t<decltype(static_cast<RollingBaselineResult (T::*)(
        const ValueRollingObservation&, const RollingSubmitOptions&)>(&T::SubmitObservation))>>
    : std::true_type {};

template <typename T, typename = void>
struct HasValuePredictRolling : std::false_type {};

template <typename T>
struct HasValuePredictRolling<
    T,
    std::void_t<decltype(static_cast<RollingPrediction (T::*)(
        std::string_view, int64_t) const>(&T::PredictRolling))>> : std::true_type {};

template <typename T, typename = void>
struct HasValuePredictRollingSequence : std::false_type {};

template <typename T>
struct HasValuePredictRollingSequence<
    T,
    std::void_t<decltype(static_cast<RollingPredictionSequence (T::*)(
        std::string_view, int64_t, uint32_t) const>(&T::PredictRolling))>>
    : std::true_type {};

template <typename T, typename = void>
struct HasValueInitRollingFromEmpty : std::false_type {};

template <typename T>
struct HasValueInitRollingFromEmpty<T,
                                    std::void_t<decltype(&T::InitRollingFromEmpty)>>
    : std::true_type {};

template <typename T, typename = void>
struct HasValueInitRollingFromBootstrap : std::false_type {};

template <typename T>
struct HasValueInitRollingFromBootstrap<
    T,
    std::void_t<decltype(&T::InitRollingFromBootstrap)>> : std::true_type {};

template <typename T, typename = void>
struct HasRatioSubmitObservation : std::false_type {};

template <typename T>
struct HasRatioSubmitObservation<
    T,
    std::void_t<decltype(static_cast<RollingBaselineResult (T::*)(
        const RatioRollingObservation&, const RollingSubmitOptions&)>(&T::SubmitObservation))>>
    : std::true_type {};

template <typename T, typename = void>
struct HasRatioPredictRolling : std::false_type {};

template <typename T>
struct HasRatioPredictRolling<
    T,
    std::void_t<decltype(static_cast<RollingPrediction (T::*)(
        std::string_view, int64_t) const>(&T::PredictRolling))>> : std::true_type {};

template <typename T, typename = void>
struct HasRatioPredictRollingSequence : std::false_type {};

template <typename T>
struct HasRatioPredictRollingSequence<
    T,
    std::void_t<decltype(static_cast<RollingPredictionSequence (T::*)(
        std::string_view, int64_t, uint32_t) const>(&T::PredictRolling))>>
    : std::true_type {};

template <typename T, typename = void>
struct HasRatioInitRollingFromEmpty : std::false_type {};

template <typename T>
struct HasRatioInitRollingFromEmpty<T,
                                    std::void_t<decltype(&T::InitRollingFromEmpty)>>
    : std::true_type {};

template <typename T, typename = void>
struct HasRatioInitRollingFromBootstrap : std::false_type {};

template <typename T>
struct HasRatioInitRollingFromBootstrap<
    T,
    std::void_t<decltype(&T::InitRollingFromBootstrap)>> : std::true_type {};

template <typename T, typename = void>
struct HasRelationSubmitBlock : std::false_type {};

template <typename T>
struct HasRelationSubmitBlock<T, std::void_t<decltype(&T::SubmitBlock)>>
    : std::true_type {};

template <typename T, typename = void>
struct HasRelationSubmitObservation : std::false_type {};

template <typename T>
struct HasRelationSubmitObservation<
    T,
    std::void_t<decltype(static_cast<RelationRollingResult (T::*)(
        const RelationRollingObservation&, const RelationRollingSubmitOptions&)>(
        &T::SubmitObservation))>>
    : std::true_type {};

template <typename T, typename = void>
struct HasRelationPredictRoutedSummary : std::false_type {};

template <typename T>
struct HasRelationPredictRoutedSummary<
    T,
    std::void_t<decltype(&T::PredictRoutedSummary)>> : std::true_type {};

template <typename T, typename = void>
struct HasRelationQueryRoutedSummarySnapshot : std::false_type {};

template <typename T>
struct HasRelationQueryRoutedSummarySnapshot<
    T,
    std::void_t<decltype(&T::QueryRoutedSummarySnapshot)>> : std::true_type {};

template <typename T, typename = void>
struct HasSetHistoryReader : std::false_type {};

template <typename T>
struct HasSetHistoryReader<T, std::void_t<decltype(&T::SetHistoryReader)>> : std::true_type {};

template <typename T, typename = void>
struct HasQueryBootstrapSeed : std::false_type {};

template <typename T>
struct HasQueryBootstrapSeed<T, std::void_t<decltype(&T::QueryBootstrapSeed)>>
    : std::true_type {};

template <typename T>
struct CanDerivedAccessTaskMutex : public T {
 private:
    template <typename U>
    static auto Test(int) -> decltype(std::declval<U&>().mutex_, std::true_type{});
    template <typename U>
    static std::false_type Test(...);

 public:
    static constexpr bool value = decltype(Test<CanDerivedAccessTaskMutex>(0))::value;
};

void TestTaskHeaderContracts() {
    std::printf("[TEST] Task header contracts...\n");

    static_assert(std::is_base_of_v<BaselineTaskBase, BaselineValueTask>);
    static_assert(std::is_base_of_v<BaselineTaskBase, BaselineRatioTask>);
    static_assert(std::is_base_of_v<BaselineTaskBase, BaselineRelationTask>);
    static_assert(!CanDerivedAccessTaskMutex<BaselineTaskBase>::value);

    using ExpectedCreateValueTask =
        std::pair<BaselineStatus, std::shared_ptr<IBaselineValueTask>> (
            IBaselineService::*)(std::string_view, BaselineSerializationFormat);
    using ExpectedCreateRatioTask =
        std::pair<BaselineStatus, std::shared_ptr<IBaselineRatioTask>> (
            IBaselineService::*)(std::string_view, BaselineSerializationFormat);
    using ExpectedCreateRelationTask =
        std::pair<BaselineStatus, std::shared_ptr<IBaselineRelationTask>> (
            IBaselineService::*)(std::string_view, BaselineSerializationFormat);
    using ExpectedValuePredictRolling =
        RollingPrediction (IBaselineValueTask::*)(std::string_view, int64_t) const;
    using ExpectedValuePredictRollingSequence =
        RollingPredictionSequence (IBaselineValueTask::*)(
            std::string_view, int64_t, uint32_t) const;
    using ExpectedValuePredictBootstrap =
        BootstrapPrediction (IBaselineValueTask::*)(
            std::string_view, int64_t, const BootstrapPredictionOptions&) const;
    using ExpectedValuePredictBootstrapSequence =
        BootstrapPredictionSequence (IBaselineValueTask::*)(
            std::string_view,
            int64_t,
            uint32_t,
            const BootstrapPredictionOptions&) const;
    using ExpectedRatioPredictRolling =
        RollingPrediction (IBaselineRatioTask::*)(std::string_view, int64_t) const;
    using ExpectedRatioPredictRollingSequence =
        RollingPredictionSequence (IBaselineRatioTask::*)(
            std::string_view, int64_t, uint32_t) const;
    using ExpectedRatioPredictBootstrap =
        BootstrapPrediction (IBaselineRatioTask::*)(
            std::string_view, int64_t, const BootstrapPredictionOptions&) const;
    using ExpectedRatioPredictBootstrapSequence =
        BootstrapPredictionSequence (IBaselineRatioTask::*)(
            std::string_view,
            int64_t,
            uint32_t,
            const BootstrapPredictionOptions&) const;
    using ExpectedRelationSubmitObservation =
        RelationRollingResult (IBaselineRelationTask::*)(
            const RelationRollingObservation&, const RelationRollingSubmitOptions&);
    using ExpectedRelationPredictRoutedSummary =
        RollingPrediction (IBaselineRelationTask::*)(
            const RelationRoutedSummaryQuery&, int64_t) const;
    using ExpectedRelationQueryRoutedSummarySnapshot =
        BaselineSerializationResult (IBaselineRelationTask::*)(
            const RelationRoutedSummaryQuery&, BaselineSerializationFormat) const;

    static_assert(std::is_same_v<decltype(&IBaselineService::CreateValueTask),
                                 ExpectedCreateValueTask>);
    static_assert(std::is_same_v<decltype(&IBaselineService::CreateRatioTask),
                                 ExpectedCreateRatioTask>);
    static_assert(std::is_same_v<decltype(&IBaselineService::CreateRelationTask),
                                 ExpectedCreateRelationTask>);
    static_assert(std::is_same_v<
                  decltype(static_cast<ExpectedValuePredictRolling>(
                      &IBaselineValueTask::PredictRolling)),
                  ExpectedValuePredictRolling>);
    static_assert(std::is_same_v<
                  decltype(static_cast<ExpectedValuePredictRollingSequence>(
                      &IBaselineValueTask::PredictRolling)),
                  ExpectedValuePredictRollingSequence>);
    static_assert(std::is_same_v<
                  decltype(static_cast<ExpectedValuePredictBootstrap>(
                      &IBaselineValueTask::PredictBootstrap)),
                  ExpectedValuePredictBootstrap>);
    static_assert(std::is_same_v<
                  decltype(static_cast<ExpectedValuePredictBootstrapSequence>(
                      &IBaselineValueTask::PredictBootstrap)),
                  ExpectedValuePredictBootstrapSequence>);
    static_assert(std::is_same_v<
                  decltype(static_cast<ExpectedRatioPredictRolling>(
                      &IBaselineRatioTask::PredictRolling)),
                  ExpectedRatioPredictRolling>);
    static_assert(std::is_same_v<
                  decltype(static_cast<ExpectedRatioPredictRollingSequence>(
                      &IBaselineRatioTask::PredictRolling)),
                  ExpectedRatioPredictRollingSequence>);
    static_assert(std::is_same_v<
                  decltype(static_cast<ExpectedRatioPredictBootstrap>(
                      &IBaselineRatioTask::PredictBootstrap)),
                  ExpectedRatioPredictBootstrap>);
    static_assert(std::is_same_v<
                  decltype(static_cast<ExpectedRatioPredictBootstrapSequence>(
                      &IBaselineRatioTask::PredictBootstrap)),
                  ExpectedRatioPredictBootstrapSequence>);
    static_assert(std::is_same_v<decltype(&IBaselineRelationTask::SubmitObservation),
                                 ExpectedRelationSubmitObservation>);
    static_assert(std::is_same_v<decltype(&IBaselineRelationTask::PredictRoutedSummary),
                                 ExpectedRelationPredictRoutedSummary>);
    static_assert(std::is_same_v<decltype(&IBaselineRelationTask::QueryRoutedSummarySnapshot),
                                 ExpectedRelationQueryRoutedSummarySnapshot>);

    static_assert(!HasRequestRebuild<IBaselineTask>::value);
    static_assert(!HasSetHistoryReader<IBaselineValueTask>::value);
    static_assert(!HasSetHistoryReader<IBaselineRatioTask>::value);
    static_assert(!HasSetHistoryReader<IBaselineRelationTask>::value);
    static_assert(!HasValueInitRollingFromEmpty<IBaselineValueTask>::value);
    static_assert(!HasValueInitRollingFromBootstrap<IBaselineValueTask>::value);
    static_assert(HasValueSubmitObservation<IBaselineValueTask>::value);
    static_assert(HasValuePredictRolling<IBaselineValueTask>::value);
    static_assert(HasValuePredictRollingSequence<IBaselineValueTask>::value);
    static_assert(!HasRatioInitRollingFromEmpty<IBaselineRatioTask>::value);
    static_assert(!HasRatioInitRollingFromBootstrap<IBaselineRatioTask>::value);
    static_assert(HasRatioSubmitObservation<IBaselineRatioTask>::value);
    static_assert(HasRatioPredictRolling<IBaselineRatioTask>::value);
    static_assert(HasRatioPredictRollingSequence<IBaselineRatioTask>::value);
    static_assert(!HasValueInitRollingFromEmpty<IBaselineRelationTask>::value);
    static_assert(!HasValueInitRollingFromBootstrap<IBaselineRelationTask>::value);
    static_assert(!HasValueSubmitObservation<IBaselineRelationTask>::value);
    static_assert(!HasValuePredictRolling<IBaselineRelationTask>::value);
    static_assert(!HasValuePredictRollingSequence<IBaselineRelationTask>::value);
    static_assert(!HasRatioInitRollingFromEmpty<IBaselineRelationTask>::value);
    static_assert(!HasRatioInitRollingFromBootstrap<IBaselineRelationTask>::value);
    static_assert(!HasRatioSubmitObservation<IBaselineRelationTask>::value);
    static_assert(!HasRatioPredictRolling<IBaselineRelationTask>::value);
    static_assert(!HasRatioPredictRollingSequence<IBaselineRelationTask>::value);
    static_assert(!HasRelationSubmitBlock<IBaselineRelationTask>::value);
    static_assert(HasRelationSubmitObservation<IBaselineRelationTask>::value);
    static_assert(HasRelationPredictRoutedSummary<IBaselineRelationTask>::value);
    static_assert(HasRelationQueryRoutedSummarySnapshot<IBaselineRelationTask>::value);
    static_assert(!HasQueryBootstrapSeed<IBaselineValueTask>::value);
    static_assert(!HasQueryBootstrapSeed<IBaselineRatioTask>::value);
    static_assert(!HasQueryBootstrapSeed<IBaselineRelationTask>::value);
    static_assert(!HasQueryBootstrapSeed<BaselineValueTask>::value);
    static_assert(!HasQueryBootstrapSeed<BaselineRatioTask>::value);
    static_assert(!HasQueryBootstrapSeed<BaselineRelationTask>::value);

    RollingSubmitOptions rolling_submit_options;
    ValueRollingObservation value_obs;
    RatioRollingObservation ratio_obs;
    RelationRollingObservation relation_obs;
    RelationRollingSubmitOptions relation_options;
    RelationRoutedSummaryResult relation_routed_result;
    RelationRollingResult relation_result;
    RelationRoutedSummaryQuery relation_query;
    RollingBaselineResult rolling_result;
    RollingPrediction rolling_prediction;
    RollingPredictionSequence rolling_prediction_sequence;
    BootstrapPredictionSequence bootstrap_prediction_sequence;
    assert(rolling_submit_options.allow_auto_init_from_bootstrap);
    assert(rolling_submit_options.allow_auto_init_from_empty);
    assert(value_obs.sample_count == 0);
    assert(ratio_obs.denominator == 0.0);
    assert(relation_obs.bucket_id == 0);
    assert(relation_options.routed_options.allow_auto_init_from_bootstrap);
    assert(relation_options.routed_options.allow_auto_init_from_empty);
    assert(relation_options.allow_basis_update);
    assert(relation_options.include_routed_results);
    assert(!relation_options.include_diagnostics);
    assert(relation_routed_result.basis_version == 0);
    assert(!relation_routed_result.basis_scoped);
    assert(relation_result.status == BaselineStatus::kOk);
    assert(relation_result.basis_version == 0);
    assert(!relation_result.basis_updated);
    assert(rolling_prediction_sequence.point_count == 0);
    assert(bootstrap_prediction_sequence.point_count == 0);
    assert(!relation_result.handover_active);
    assert(relation_result.routed_results.empty());
    assert(relation_query.basis_version == 0);
    assert(rolling_result.status == BaselineStatus::kOk);
    assert(rolling_result.maturity_status.empty());
    assert(rolling_result.score_trust_status.empty());
    assert(rolling_result.calibration_status.empty());
    AssertNear(rolling_result.learning_confidence, 0.0);
    AssertNear(rolling_result.score_confidence, 0.0);
    AssertNear(rolling_result.effective_confidence, 0.0);
    assert(!rolling_result.can_alert);
    assert(rolling_result.enabled_components.empty());
    assert(rolling_result.component_readiness.empty());
    assert(rolling_prediction.status == BaselineStatus::kOk);
    AssertNear(rolling_prediction.baseline_mu, 0.0);
    AssertNear(rolling_prediction.baseline_lower, 0.0);
    AssertNear(rolling_prediction.baseline_upper, 0.0);
    AssertNear(rolling_prediction.band_z, 0.0);

    std::printf("[PASS] Task header contracts\n");
}

#ifdef FLOWSQL_BASELINE_CLOSE_RESOURCE_TEST
// Resource ownership regressions link the plugin's exact objects, including its hidden task implementations.
void TestCloseReleasesResourcesWhileTaskHandlesRemainAlive() {
    std::printf("[TEST] Close releases resources while task handles remain alive...\n");
    for (const bool through_registry : {false, true}) {
        TaskRegistry registry;
        std::vector<std::weak_ptr<const CompiledEventCalendar>> calendars;
        auto make_calendar = [&] {
            auto calendar = std::make_shared<CompiledEventCalendar>();
            calendar->bucket_ranges.reserve(128);
            calendars.emplace_back(calendar);
            return calendar;
        };

        BaselineTaskSpec value_spec;
        value_spec.task_id = "close-value";
        value_spec.task_kind = "value";
        value_spec.feature_id = "bps";
        value_spec.feature_type = "value_basic";
        value_spec.profile = "default";
        value_spec.delta = value_spec.clock_spec.bucket_seconds = 60;
        value_spec.tz = value_spec.clock_spec.timezone = "UTC";
        auto value = std::make_shared<BaselineValueTask>(&registry, "close-value", "value", "value-config", value_spec,
                                                         make_calendar());
        BaselineTaskSpec ratio_spec = value_spec;
        ratio_spec.task_id = "close-ratio";
        ratio_spec.task_kind = "ratio";
        ratio_spec.feature_type = "ratio";
        ratio_spec.profile = "rate_core";
        auto ratio = std::make_shared<BaselineRatioTask>(&registry, "close-ratio", "ratio", "ratio-config", ratio_spec,
                                                         make_calendar());
        RelationTaskCreateSpec relation_spec;
        relation_spec.task_spec.task_id = "close-relation";
        relation_spec.task_spec.task_kind = "relation";
        relation_spec.task_spec.feature_id = relation_spec.task_spec.feature_base = "client_mix";
        relation_spec.task_spec.group_space_id = "client_group";
        relation_spec.task_spec.group_space_version = "v1";
        relation_spec.task_spec.metrics = {"bps"};
        relation_spec.task_spec.support_policy = {2, 0.01, 0.1};
        relation_spec.task_spec.summary_policy = {2, 1};
        relation_spec.clock_spec.delta = 60;
        relation_spec.clock_spec.tz = "UTC";
        auto relation = std::make_shared<BaselineRelationTask>(&registry, "close-relation", "relation",
                                                               "relation-config", relation_spec, make_calendar());
        assert(registry.Register(value) == 0);
        assert(registry.Register(ratio) == 0);
        assert(registry.Register(relation) == 0);

        ValueBootstrapInput value_input;
        RatioBootstrapInput ratio_input;
        RelationBootstrapInput relation_input;
        value_input.series_key = ratio_input.series_key = relation_input.series_key = "trained";
        for (int64_t bucket = 0; bucket < 10; ++bucket) {
            value_input.observations.push_back({bucket, 100.0 + bucket, 1});
            ratio_input.observations.push_back({bucket, 50.0 + bucket, 100.0});
            RelationBootstrapBlock block;
            block.bucket_id = bucket;
            block.group_idx = {1, 2};
            RelationBootstrapMetric metric;
            metric.metric = "bps";
            metric.total = 100.0;
            metric.active_count = 2;
            metric.values_by_group = {80.0, 20.0};
            block.metrics.push_back(std::move(metric));
            relation_input.blocks.push_back(std::move(block));
        }
        assert(value->Bootstrap(value_input).status == BaselineStatus::kOk);
        assert(ratio->Bootstrap(ratio_input).status == BaselineStatus::kOk);
        assert(relation->Bootstrap(relation_input).status == BaselineStatus::kOk);
        for (int i = 0; i < 32; ++i) {
            const std::string key = "online-" + std::to_string(i);
            assert(value->SubmitObservation({key, 100, 100.0, 1}, {}).status == BaselineStatus::kOk);
            assert(ratio->SubmitObservation({key, 100, 50.0, 100.0}, {}).status == BaselineStatus::kOk);
            RelationRollingObservation observation;
            observation.series_key = key;
            observation.bucket_id = 100;
            observation.group_idx = {1, 2};
            observation.metrics = relation_input.blocks.back().metrics;
            assert(relation->SubmitObservation(observation, {}).status == BaselineStatus::kOk);
        }

        std::vector<std::shared_ptr<IBaselineTask>> tasks{value, ratio, relation};
        for (const auto& weak : calendars) assert(!weak.expired());
        if (through_registry) {
            // Stop uses these base handles, so cleanup must run through OnClosing.
            for (const auto& task : registry.Snapshot()) assert(task->Close() == BaselineStatus::kOk);
        }
        for (std::size_t i = 0; i < tasks.size(); ++i) {
            const std::string id = tasks[i]->Id();
            const std::string name = tasks[i]->Name();
            const auto kind = tasks[i]->Kind();
            const auto config = tasks[i]->ExportConfig(BaselineSerializationFormat::kJson);
            assert(tasks[i]->Close() == BaselineStatus::kOk);
            assert(calendars[i].expired());
            assert(tasks[i]->Close() == BaselineStatus::kOk);
            assert(tasks[i]->Id() == id);
            assert(tasks[i]->Name() == name);
            assert(tasks[i]->Kind() == kind);
            assert(tasks[i]->ExportConfig(BaselineSerializationFormat::kJson) == config);
            assert(tasks[i]->QueryTaskSnapshot(BaselineSerializationFormat::kJson).first != BaselineStatus::kOk);
            if (!through_registry) {
                assert(registry.Size() == tasks.size() - i - 1);
                for (std::size_t j = i + 1; j < tasks.size(); ++j) {
                    assert(!calendars[j].expired());
                    assert(tasks[j]->QuerySeriesSnapshot("trained", BaselineSerializationFormat::kJson).first ==
                           BaselineStatus::kOk);
                }
            }
        }
        assert(registry.Size() == 0);
        assert(value->SubmitObservation({"trained", 101, 100.0, 1}, {}).status != BaselineStatus::kOk);
        assert(ratio->SubmitObservation({"trained", 101, 50.0, 100.0}, {}).status != BaselineStatus::kOk);
        RelationRollingObservation observation;
        observation.series_key = "trained";
        observation.bucket_id = 101;
        observation.group_idx = {1, 2};
        observation.metrics = relation_input.blocks.back().metrics;
        assert(relation->SubmitObservation(observation, {}).status != BaselineStatus::kOk);
    }
    std::printf("[PASS] Close releases resources through public and registry base handles\n");
}
#endif

void TestBootstrapEngineTrainsValueAndRatio() {
    std::printf("[TEST] Bootstrap engine trains value and ratio...\n");

    BootstrapEngine engine;
    BaselineTaskSpec value_spec;
    value_spec.task_id = "bootstrap-value-task";
    value_spec.feature_id = "bps";
    value_spec.task_kind = "value";
    value_spec.feature_type = "value_basic";
    value_spec.profile = "default";
    value_spec.clock_spec.bucket_seconds = 60;
    value_spec.clock_spec.timezone = "UTC";
    value_spec.calendar_ref.calendar_id = "test-calendar";
    value_spec.calendar_ref.calendar_version = "v1";

    ValueBootstrapInput input;
    input.series_key = "svc-a";
    for (int64_t bucket = 0; bucket < 200; ++bucket) {
        input.observations.push_back(
            ValueBootstrapPoint{bucket, 100.0 + static_cast<double>(bucket % 10), 1});
    }

    BootstrapArtifact value_artifact;
    const BootstrapTrainResult value_result =
        engine.TrainValue(value_spec, input, &value_artifact);
    assert(value_result.status == BaselineStatus::kOk);
    assert(value_result.accepted_count == input.observations.size());
    assert(value_artifact.artifact_kind == BootstrapArtifactKind::kValue);
    assert(value_artifact.train_status == BaselineStatus::kOk);
    assert(value_artifact.value_model != nullptr);

    BootstrapPredictionOptions prediction_options;
    prediction_options.include_model_space_debug = true;
    const BootstrapPrediction value_prediction =
        engine.PredictValue(value_artifact, 220, prediction_options);
    assert(value_prediction.status == BaselineStatus::kOk);
    assert(value_prediction.baseline_mu > 0.0);
    assert(value_prediction.baseline_lower <= value_prediction.baseline_mu);
    assert(value_prediction.baseline_upper >= value_prediction.baseline_mu);
    assert(value_prediction.band_width > 0.0);
    assert(value_prediction.has_model_space);
    const BootstrapPredictionSequence value_sequence =
        engine.PredictValueSequence(value_artifact, 220, 3, prediction_options);
    assert(value_sequence.status == BaselineStatus::kOk);
    assert(value_sequence.series_key == "svc-a");
    assert(value_sequence.start_bucket_id == 220);
    assert(value_sequence.point_count == 3);
    assert(value_sequence.predictions.size() == 3);
    assert(value_sequence.predictions[0].bucket_id == 220);
    assert(value_sequence.predictions[1].bucket_id == 221);
    assert(value_sequence.predictions[2].bucket_id == 222);
    assert(value_sequence.predictions[0].status == BaselineStatus::kOk);
    AssertNear(value_sequence.predictions[0].baseline_mu, value_prediction.baseline_mu);

    auto [artifact_status, artifact_json] =
        engine.ExportArtifact(value_artifact, BaselineSerializationFormat::kJson);
    assert(artifact_status == BaselineStatus::kOk);
    assert(artifact_json.find("\"document_kind\":\"bootstrap_artifact\"") != std::string::npos);

    BootstrapArtifact loaded_value_artifact;
    assert(engine.LoadArtifact(
               artifact_json, BaselineSerializationFormat::kJson, &loaded_value_artifact) ==
           BaselineStatus::kOk);
    const BootstrapPrediction loaded_value_prediction =
        engine.PredictValue(loaded_value_artifact, 220, prediction_options);
    assert(loaded_value_prediction.status == BaselineStatus::kOk);
    assert(loaded_value_prediction.baseline_mu > 0.0);

    BootstrapSeed value_seed;
    assert(engine.ExportSeed(value_artifact, &value_seed) == BaselineStatus::kOk);
    auto [seed_status, seed_json] =
        engine.ExportSeed(value_seed, BaselineSerializationFormat::kJson);
    assert(seed_status == BaselineStatus::kOk);
    assert(seed_json.find("\"document_kind\":\"bootstrap_seed\"") != std::string::npos);
    assert(seed_json.find("\"algorithm_version\":\"b1-bootstrap-v1\"") != std::string::npos);
    assert(seed_json.find("\"feature_type\":\"value_basic\"") != std::string::npos);
    assert(seed_json.find("\"calendar_ref\"") != std::string::npos);
    assert(seed_json.find("\"clock_spec\"") != std::string::npos);
    assert(seed_json.find("\"bucket_seconds\":60") != std::string::npos);
    assert(seed_json.find("\"timezone\":\"UTC\"") != std::string::npos);
    assert(seed_json.find("\"seeded_components\"") != std::string::npos);
    assert(seed_json.find("\"enabled_components\"") != std::string::npos);
    assert(seed_json.find("\"level\"") != std::string::npos);
    assert(seed_json.find("\"trend\"") != std::string::npos);
    assert(seed_json.find("\"daily\"") != std::string::npos);
    assert(seed_json.find("\"weekly\"") != std::string::npos);
    assert(seed_json.find("\"core\"") == std::string::npos);
    assert(seed_json.find("\"theta_init\"") != std::string::npos);
    assert(seed_json.find("\"level\"") != std::string::npos);
    assert(seed_json.find("\"trend\"") != std::string::npos);
    assert(seed_json.find("\"daily_harmonic\"") != std::string::npos);
    assert(seed_json.find("\"weekly_harmonic\"") != std::string::npos);
    assert(seed_json.find("\"sigma_init\"") != std::string::npos);
    assert(seed_json.find("\"uncertainty_init\"") != std::string::npos);
    assert(seed_json.find("\"component_uncertainty\"") != std::string::npos);
    assert(seed_json.find("\"level_scale\"") != std::string::npos);
    assert(seed_json.find("\"trend_scale\"") != std::string::npos);
    assert(seed_json.find("\"daily_scale\"") != std::string::npos);
    assert(seed_json.find("\"weekly_scale\"") != std::string::npos);
    assert(seed_json.find("\"maturity_init\"") != std::string::npos);
    assert(seed_json.find("\"model\"") == std::string::npos);

    BaselineTaskSpec sampled_value_spec = value_spec;
    sampled_value_spec.task_id = "bootstrap-sampled-value-task";
    sampled_value_spec.feature_type = "value_sampled";
    sampled_value_spec.profile = "cont_core";
    ValueBootstrapInput sampled_input = input;
    for (auto& point : sampled_input.observations) {
        point.sample_count = 50;
    }
    BootstrapArtifact sampled_value_artifact;
    const BootstrapTrainResult sampled_value_result =
        engine.TrainValue(sampled_value_spec, sampled_input, &sampled_value_artifact);
    assert(sampled_value_result.status == BaselineStatus::kOk);
    assert(sampled_value_artifact.value_model != nullptr);

    BaselineTaskSpec invalid_basic_spec = value_spec;
    invalid_basic_spec.task_id = "bootstrap-invalid-basic-task";
    invalid_basic_spec.feature_type = "value_basic";
    invalid_basic_spec.profile = "cont_core";
    BootstrapArtifact invalid_basic_artifact;
    const BootstrapTrainResult invalid_basic_result =
        engine.TrainValue(invalid_basic_spec, sampled_input, &invalid_basic_artifact);
    assert(invalid_basic_result.status == BaselineStatus::kInvalidArgument);

    BaselineTaskSpec invalid_sampled_spec = value_spec;
    invalid_sampled_spec.task_id = "bootstrap-invalid-sampled-task";
    invalid_sampled_spec.feature_type = "value_sampled";
    invalid_sampled_spec.profile = "default";
    BootstrapArtifact invalid_sampled_artifact;
    const BootstrapTrainResult invalid_sampled_result =
        engine.TrainValue(invalid_sampled_spec, sampled_input, &invalid_sampled_artifact);
    assert(invalid_sampled_result.status == BaselineStatus::kInvalidArgument);

    BaselineTaskSpec ratio_spec;
    ratio_spec.task_id = "bootstrap-ratio-task";
    ratio_spec.feature_id = "success_rate";
    ratio_spec.task_kind = "ratio";
    ratio_spec.feature_type = "ratio";
    ratio_spec.profile = "rate_core";
    ratio_spec.clock_spec.bucket_seconds = 60;
    ratio_spec.clock_spec.timezone = "UTC";
    ratio_spec.calendar_ref.calendar_id = "test-calendar";
    ratio_spec.calendar_ref.calendar_version = "v1";

    RatioBootstrapInput ratio_input;
    ratio_input.series_key = "svc-a";
    for (int64_t bucket = 0; bucket < 200; ++bucket) {
        ratio_input.observations.push_back(
            RatioBootstrapPoint{bucket, 95.0 + static_cast<double>(bucket % 3), 100.0});
    }

    BootstrapArtifact ratio_artifact;
    const BootstrapTrainResult ratio_result =
        engine.TrainRatio(ratio_spec, ratio_input, &ratio_artifact);
    assert(ratio_result.status == BaselineStatus::kOk);
    assert(ratio_result.accepted_count == ratio_input.observations.size());
    assert(ratio_artifact.artifact_kind == BootstrapArtifactKind::kRatio);
    assert(ratio_artifact.train_status == BaselineStatus::kOk);
    assert(ratio_artifact.ratio_model != nullptr);

    const BootstrapPrediction ratio_prediction =
        engine.PredictRatio(ratio_artifact, 220, prediction_options);
    assert(ratio_prediction.status == BaselineStatus::kOk);
    assert(ratio_prediction.baseline_mu >= 0.0);
    assert(ratio_prediction.baseline_mu <= 1.0);
    assert(ratio_prediction.baseline_lower >= 0.0);
    assert(ratio_prediction.baseline_upper <= 1.0);
    assert(ratio_prediction.baseline_lower <= ratio_prediction.baseline_mu);
    assert(ratio_prediction.baseline_upper >= ratio_prediction.baseline_mu);
    assert(ratio_prediction.band_width > 0.0);
    const BootstrapPredictionSequence ratio_sequence =
        engine.PredictRatioSequence(ratio_artifact, 220, 2, prediction_options);
    assert(ratio_sequence.status == BaselineStatus::kOk);
    assert(ratio_sequence.series_key == "svc-a");
    assert(ratio_sequence.start_bucket_id == 220);
    assert(ratio_sequence.point_count == 2);
    assert(ratio_sequence.predictions.size() == 2);
    assert(ratio_sequence.predictions[0].bucket_id == 220);
    assert(ratio_sequence.predictions[1].bucket_id == 221);
    assert(ratio_sequence.predictions[0].status == BaselineStatus::kOk);
    AssertNear(ratio_sequence.predictions[0].baseline_mu, ratio_prediction.baseline_mu);

    BootstrapSeed ratio_seed;
    assert(engine.ExportSeed(ratio_artifact, &ratio_seed) == BaselineStatus::kOk);
    auto [ratio_seed_status, ratio_seed_json] =
        engine.ExportSeed(ratio_seed, BaselineSerializationFormat::kJson);
    assert(ratio_seed_status == BaselineStatus::kOk);
    assert(ratio_seed_json.find("\"feature_type\":\"ratio\"") != std::string::npos);
    assert(ratio_seed_json.find("\"calendar_ref\"") != std::string::npos);
    assert(ratio_seed_json.find("\"clock_spec\"") != std::string::npos);
    assert(ratio_seed_json.find("\"seeded_components\"") != std::string::npos);
    assert(ratio_seed_json.find("\"enabled_components\"") != std::string::npos);
    assert(ratio_seed_json.find("\"theta_init\"") != std::string::npos);
    assert(ratio_seed_json.find("\"sigma_init\"") != std::string::npos);
    assert(ratio_seed_json.find("\"uncertainty_init\"") != std::string::npos);
    assert(ratio_seed_json.find("\"component_uncertainty\"") != std::string::npos);
    assert(ratio_seed_json.find("\"maturity_init\"") != std::string::npos);
    assert(ratio_seed_json.find("\"ratio_prior_init\"") != std::string::npos);
    assert(ratio_seed_json.find("\"m0\"") != std::string::npos);
    assert(ratio_seed_json.find("\"alpha0\"") != std::string::npos);
    assert(ratio_seed_json.find("\"beta0\"") != std::string::npos);
    assert(ratio_seed_json.find("\"model\"") == std::string::npos);

    rapidjson::Document ratio_seed_doc;
    ratio_seed_doc.Parse(ratio_seed_json.c_str());
    assert(!ratio_seed_doc.HasParseError());
    assert(ratio_seed_doc["theta_init"]["model_space"].IsString());
    assert(std::string(ratio_seed_doc["theta_init"]["model_space"].GetString()) == "logit");
    assert(ratio_seed_doc["sigma_init"]["model_space"].IsString());
    assert(std::string(ratio_seed_doc["sigma_init"]["model_space"].GetString()) == "logit");

    std::printf("[PASS] Bootstrap engine trains value and ratio\n");
}

void TestBootstrapEngineNormalizesHistoryBeforeTraining() {
    std::printf("[TEST] Bootstrap engine normalizes duplicate buckets before training...\n");

    BootstrapEngine engine;
    BaselineTaskSpec value_spec;
    value_spec.task_id = "bootstrap-sampled-normalize-task";
    value_spec.feature_id = "sampled_bps";
    value_spec.task_kind = "value";
    value_spec.feature_type = "value_sampled";
    value_spec.profile = "cont_core";
    value_spec.clock_spec.bucket_seconds = 60;
    value_spec.clock_spec.timezone = "UTC";
    value_spec.calendar_ref.calendar_id = "test-calendar";
    value_spec.calendar_ref.calendar_version = "v1";

    ValueBootstrapInput value_input;
    value_input.series_key = "svc-a";
    value_input.observations.push_back(ValueBootstrapPoint{0, 10.0, 25});
    value_input.observations.push_back(ValueBootstrapPoint{0, 30.0, 25});
    value_input.observations.push_back(ValueBootstrapPoint{1, 200.0, 20});
    value_input.observations.push_back(ValueBootstrapPoint{2, 40.0, 50});

    BootstrapArtifact value_artifact;
    const BootstrapTrainResult value_result =
        engine.TrainValue(value_spec, value_input, &value_artifact);
    assert(value_result.status == BaselineStatus::kOk);
    assert(value_result.accepted_count == 2);
    assert(value_result.rejected_count == 1);
    AssertNear(value_result.coverage_ratio, 2.0 / 3.0);
    assert(value_artifact.coverage_report.accepted_count == 2);
    assert(value_artifact.coverage_report.rejected_count == 1);
    AssertNear(value_artifact.coverage_report.coverage_ratio, 2.0 / 3.0);

    BaselineTaskSpec ratio_spec;
    ratio_spec.task_id = "bootstrap-ratio-normalize-task";
    ratio_spec.feature_id = "success_rate";
    ratio_spec.task_kind = "ratio";
    ratio_spec.feature_type = "ratio";
    ratio_spec.profile = "rate_core";
    ratio_spec.clock_spec.bucket_seconds = 60;
    ratio_spec.clock_spec.timezone = "UTC";
    ratio_spec.calendar_ref.calendar_id = "test-calendar";
    ratio_spec.calendar_ref.calendar_version = "v1";

    RatioBootstrapInput ratio_input;
    ratio_input.series_key = "svc-a";
    ratio_input.observations.push_back(RatioBootstrapPoint{0, 10.0, 25.0});
    ratio_input.observations.push_back(RatioBootstrapPoint{0, 15.0, 25.0});
    ratio_input.observations.push_back(RatioBootstrapPoint{1, 1.0, 20.0});
    ratio_input.observations.push_back(RatioBootstrapPoint{2, 40.0, 50.0});

    BootstrapArtifact ratio_artifact;
    const BootstrapTrainResult ratio_result =
        engine.TrainRatio(ratio_spec, ratio_input, &ratio_artifact);
    assert(ratio_result.status == BaselineStatus::kOk);
    assert(ratio_result.accepted_count == 2);
    assert(ratio_result.rejected_count == 1);
    AssertNear(ratio_result.coverage_ratio, 2.0 / 3.0);
    assert(ratio_artifact.coverage_report.accepted_count == 2);
    assert(ratio_artifact.coverage_report.rejected_count == 1);
    AssertNear(ratio_artifact.coverage_report.coverage_ratio, 2.0 / 3.0);

    ValueBootstrapInput min_value_input = value_input;
    min_value_input.options.min_observation_count = 3;
    BootstrapArtifact min_value_artifact;
    const BootstrapTrainResult min_value_result =
        engine.TrainValue(value_spec, min_value_input, &min_value_artifact);
    assert(min_value_result.status == BaselineStatus::kInsufficientData);
    assert(min_value_result.accepted_count == 2);

    RelationTaskCreateSpec relation_spec;
    relation_spec.task_spec.task_id = "relation-bootstrap-normalize-task";
    relation_spec.task_spec.task_kind = "relation";
    relation_spec.task_spec.feature_id = "client_mix";
    relation_spec.task_spec.feature_base = "client_mix";
    relation_spec.task_spec.group_space_id = "client_group";
    relation_spec.task_spec.group_space_version = "v1";
    relation_spec.task_spec.metrics = {"bps"};
    relation_spec.task_spec.support_policy.k_support = 2;
    relation_spec.task_spec.support_policy.min_hist_share = 0.01;
    relation_spec.task_spec.support_policy.min_active_ratio = 0.1;
    relation_spec.task_spec.summary_policy.k_head = 2;
    relation_spec.task_spec.summary_policy.k_stable = 1;
    relation_spec.clock_spec.delta = 60;
    relation_spec.clock_spec.tz = "UTC";

    auto relation_block = [](int64_t bucket,
                             double total,
                             double g1,
                             double g2) {
        RelationBootstrapBlock block;
        block.bucket_id = bucket;
        block.group_idx = {1, 2};
        RelationBootstrapMetric metric;
        metric.metric = "bps";
        metric.total = total;
        metric.values_by_group = {g1, g2};
        block.metrics.push_back(metric);
        return block;
    };

    RelationBootstrapInput relation_input;
    relation_input.series_key = "svc-a";
    relation_input.blocks.push_back(relation_block(0, 100.0, 60.0, 40.0));
    relation_input.blocks.push_back(relation_block(0, 50.0, 20.0, 30.0));
    relation_input.blocks.push_back(relation_block(1, 100.0, 70.0, 30.0));

    BootstrapArtifact relation_artifact;
    const BootstrapTrainResult relation_result =
        engine.TrainRelation(relation_spec, relation_input, &relation_artifact);
    assert(relation_result.status == BaselineStatus::kOk);
    assert(relation_result.accepted_count == 2);
    assert(relation_result.rejected_count == 0);
    AssertNear(relation_result.coverage_ratio, 1.0);
    assert(relation_artifact.coverage_report.accepted_count == 2);
    AssertNear(relation_artifact.coverage_report.coverage_ratio, 1.0);

    RelationBootstrapInput min_relation_input = relation_input;
    min_relation_input.options.min_observation_count = 3;
    BootstrapArtifact min_relation_artifact;
    const BootstrapTrainResult min_relation_result =
        engine.TrainRelation(relation_spec, min_relation_input, &min_relation_artifact);
    assert(min_relation_result.status == BaselineStatus::kInsufficientData);
    assert(min_relation_result.accepted_count == 2);

    std::printf("[PASS] Bootstrap engine normalizes duplicate buckets before training\n");
}

void TestBootstrapSeedQualityStatusIsEvaluatedFromCoverage() {
    std::printf("[TEST] Bootstrap seed quality status is evaluated from coverage...\n");

    auto build_spec = [](const char* task_id) {
        BaselineTaskSpec spec;
        spec.task_id = task_id;
        spec.feature_id = "bps";
        spec.task_kind = "value";
        spec.feature_type = "value_basic";
        spec.profile = "default";
        spec.clock_spec.bucket_seconds = 3600;
        spec.clock_spec.timezone = "UTC";
        spec.calendar_ref.calendar_id = "test-calendar";
        spec.calendar_ref.calendar_version = "v1";
        return spec;
    };

    auto build_input = [](const char* series_key,
                          int64_t count,
                          int64_t bucket_step) {
        ValueBootstrapInput input;
        input.series_key = series_key;
        for (int64_t i = 0; i < count; ++i) {
            const double daily_wave = static_cast<double>(i % 24);
            input.observations.push_back(
                ValueBootstrapPoint{i * bucket_step, 100.0 + daily_wave, 1});
        }
        return input;
    };

    BootstrapEngine engine;

    BootstrapArtifact full_artifact;
    const BootstrapTrainResult full_result =
        engine.TrainValue(build_spec("bootstrap-quality-full"),
                          build_input("svc-full", 14 * 24, 1),
                          &full_artifact);
    assert(full_result.status == BaselineStatus::kOk);
    assert(full_result.seed_status == BootstrapSeedStatus::kFull);
    BootstrapSeed full_seed;
    assert(engine.ExportSeed(full_artifact, &full_seed) == BaselineStatus::kOk);
    assert(full_seed.seed_status == BootstrapSeedStatus::kFull);
    assert(full_seed.maturity_init.seed_status == BootstrapSeedStatus::kFull);

    BootstrapArtifact partial_artifact;
    const BootstrapTrainResult partial_result =
        engine.TrainValue(build_spec("bootstrap-quality-partial"),
                          build_input("svc-partial", 2 * 24, 1),
                          &partial_artifact);
    assert(partial_result.status == BaselineStatus::kOk);
    assert(partial_result.seed_status == BootstrapSeedStatus::kPartial);
    BootstrapSeed partial_seed;
    assert(engine.ExportSeed(partial_artifact, &partial_seed) == BaselineStatus::kOk);
    assert(partial_seed.seed_status == BootstrapSeedStatus::kPartial);

    BootstrapArtifact weak_artifact;
    const BootstrapTrainResult weak_result =
        engine.TrainValue(build_spec("bootstrap-quality-weak"),
                          build_input("svc-weak", 100, 10),
                          &weak_artifact);
    assert(weak_result.status == BaselineStatus::kOk);
    assert(weak_result.seed_status == BootstrapSeedStatus::kWeak);
    BootstrapSeed weak_seed;
    assert(engine.ExportSeed(weak_artifact, &weak_seed) == BaselineStatus::kOk);
    assert(weak_seed.seed_status == BootstrapSeedStatus::kWeak);

    BootstrapArtifact failed_artifact;
    failed_artifact.train_status = BaselineStatus::kTrainFailed;
    BootstrapSeed none_seed;
    assert(engine.ExportSeed(failed_artifact, &none_seed) == BaselineStatus::kNotTrained);
    assert(none_seed.seed_status == BootstrapSeedStatus::kNone);

    std::printf("[PASS] Bootstrap seed quality status is evaluated from coverage\n");
}

void TestBootstrapEnginePreservesMonthposArtifactAndSeed() {
    std::printf("[TEST] Bootstrap engine preserves monthpos artifact and seed...\n");

    BootstrapEngine engine;
    BaselineTaskSpec value_spec;
    value_spec.task_id = "bootstrap-monthpos-task";
    value_spec.feature_id = "daily_bps";
    value_spec.task_kind = "value";
    value_spec.feature_type = "value_basic";
    value_spec.profile = "default";
    value_spec.clock_spec.bucket_seconds = 86400;
    value_spec.clock_spec.timezone = "UTC";
    value_spec.calendar_ref.calendar_id = "test-calendar";
    value_spec.calendar_ref.calendar_version = "v1";

    ValueBootstrapInput input;
    input.series_key = "svc-monthpos";
    const int64_t start_bucket = 18628;  // 2021-01-01 UTC when bucket_seconds=86400.
    for (int64_t offset = 0; offset < 160; ++offset) {
        const int64_t bucket = start_bucket + offset;
        const int64_t dom = (offset % 31) + 1;
        const double value = 100.0 + (dom == 1 ? 120.0 : 0.0);
        input.observations.push_back(ValueBootstrapPoint{bucket, value, 1});
    }

    BootstrapArtifact artifact;
    const BootstrapTrainResult result = engine.TrainValue(value_spec, input, &artifact);
    assert(result.status == BaselineStatus::kOk);
    assert(artifact.value_model != nullptr);
    assert(artifact.value_model->monthpos_block.enabled);

    BootstrapPredictionOptions options;
    options.include_model_space_debug = true;
    const int64_t predict_bucket = start_bucket + 151;
    const BootstrapPrediction before =
        engine.PredictValue(artifact, predict_bucket, options, &value_spec, nullptr);
    assert(before.status == BaselineStatus::kOk);

    auto [artifact_status, artifact_json] =
        engine.ExportArtifact(artifact, BaselineSerializationFormat::kJson);
    assert(artifact_status == BaselineStatus::kOk);
    assert(artifact_json.find("\"monthpos_block\"") != std::string::npos);

    BootstrapArtifact loaded;
    assert(engine.LoadArtifact(artifact_json, BaselineSerializationFormat::kJson, &loaded) ==
           BaselineStatus::kOk);
    assert(loaded.value_model != nullptr);
    assert(loaded.value_model->monthpos_block.enabled);
    const BootstrapPrediction after =
        engine.PredictValue(loaded, predict_bucket, options, &value_spec, nullptr);
    assert(after.status == BaselineStatus::kOk);
    AssertNear(after.model_space_mu, before.model_space_mu);

    BootstrapSeed seed;
    assert(engine.ExportSeed(artifact, &seed) == BaselineStatus::kOk);
    assert(seed.monthpos_hint.available);
    auto [seed_status, seed_json] =
        engine.ExportSeed(seed, BaselineSerializationFormat::kJson);
    assert(seed_status == BaselineStatus::kOk);
    assert(seed_json.find("\"monthpos_hint\"") != std::string::npos);
    assert(seed_json.find("\"monthpos\"") != std::string::npos);

    std::printf("[PASS] Bootstrap engine preserves monthpos artifact and seed\n");
}

void TestBootstrapEngineTrainsRelationBasis() {
    std::printf("[TEST] Bootstrap engine trains relation basis...\n");

    RelationTaskCreateSpec spec;
    spec.task_spec.task_id = "relation-bootstrap-task";
    spec.task_spec.task_kind = "relation";
    spec.task_spec.feature_id = "client_mix";
    spec.task_spec.feature_base = "client_mix";
    spec.task_spec.group_space_id = "client_group";
    spec.task_spec.group_space_version = "v1";
    spec.task_spec.metrics = {"bps"};
    spec.task_spec.support_policy.k_support = 2;
    spec.task_spec.support_policy.min_hist_share = 0.01;
    spec.task_spec.support_policy.min_active_ratio = 0.1;
    spec.task_spec.summary_policy.k_head = 2;
    spec.task_spec.summary_policy.k_stable = 1;
    spec.task_spec.calendar_ref.calendar_id = "test-calendar";
    spec.task_spec.calendar_ref.calendar_version = "v1";
    spec.clock_spec.delta = 60;
    spec.clock_spec.tz = "UTC";

    RelationBootstrapInput input;
    input.series_key = "svc-a";
    for (int64_t bucket = 0; bucket < 10; ++bucket) {
        RelationBootstrapBlock block;
        block.bucket_id = bucket;
        block.group_idx = {1, 2, 3};
        RelationBootstrapMetric metric;
        metric.metric = "bps";
        metric.total = 100.0;
        metric.values_by_group = {60.0, 30.0, 10.0};
        block.metrics.push_back(metric);
        input.blocks.push_back(block);
    }

    BootstrapEngine engine;
    BootstrapArtifact artifact;
    const BootstrapTrainResult result = engine.TrainRelation(spec, input, &artifact);
    assert(result.status == BaselineStatus::kOk);
    assert(artifact.artifact_kind == BootstrapArtifactKind::kRelation);
    assert(artifact.relation_basis_by_metric.size() == 1);
    assert(artifact.relation_basis_by_metric[0].support_explicit.size() == 2);
    assert(artifact.relation_basis_by_metric[0].stable_head.size() == 1);
    assert(!artifact.relation_routed_summary_artifacts.empty());
    AssertRelationFusionMetadata(artifact.relation_fusion_metadata);

    bool has_value_summary = false;
    bool has_ratio_summary = false;
    for (const auto& routed_artifact : artifact.relation_routed_summary_artifacts) {
        if (routed_artifact.summary_name == "entropy_shannon" &&
            routed_artifact.task_kind == BaselineTaskKind::kValue) {
            has_value_summary = true;
        }
        if (routed_artifact.summary_name == "top1_share" &&
            routed_artifact.task_kind == BaselineTaskKind::kRatio) {
            has_ratio_summary = true;
        }
    }
    assert(has_value_summary);
    assert(has_ratio_summary);

    BootstrapSeed relation_seed;
    assert(engine.ExportSeed(artifact, &relation_seed) == BaselineStatus::kOk);
    assert(!relation_seed.relation_routed_summary_seeds.empty());
    AssertRelationFusionMetadata(relation_seed.relation_fusion_metadata);
    bool has_universal_seed_scope = false;
    bool has_basis_scoped_seed_scope = false;
    const uint64_t relation_basis_version =
        relation_seed.relation_basis_by_metric[0].basis_version;
    for (const auto& routed_seed : relation_seed.relation_routed_summary_seeds) {
        if (routed_seed.summary_name == "top1_share") {
            assert(!routed_seed.basis_scoped);
            assert(routed_seed.basis_version == 0);
            has_universal_seed_scope = true;
        }
        if (routed_seed.summary_name == "out_of_support_share") {
            assert(routed_seed.basis_scoped);
            assert(routed_seed.basis_version == relation_basis_version);
            has_basis_scoped_seed_scope = true;
        }
    }
    assert(has_universal_seed_scope);
    assert(has_basis_scoped_seed_scope);
    auto [seed_status, seed_json] =
        engine.ExportSeed(relation_seed, BaselineSerializationFormat::kJson);
    assert(seed_status == BaselineStatus::kOk);
    assert(seed_json.find("\"basis_scoped\"") != std::string::npos);
    assert(seed_json.find("\"basis_version\"") != std::string::npos);
    assert(seed_json.find("\"feature_type\":\"relation\"") != std::string::npos);
    assert(seed_json.find("\"feature_type\":\"value_basic\"") != std::string::npos);
    assert(seed_json.find("\"feature_type\":\"ratio\"") != std::string::npos);
    assert(seed_json.find("\"clock_spec\"") != std::string::npos);
    assert(seed_json.find("\"seeded_components\"") != std::string::npos);
    assert(seed_json.find("\"enabled_components\"") != std::string::npos);
    assert(seed_json.find("\"relation_basis\"") != std::string::npos);
    assert(seed_json.find("\"relation_routed_summary_seeds\"") != std::string::npos);
    assert(seed_json.find("\"relation_basis_by_metric\"") != std::string::npos);
    assert(seed_json.find("\"relation_fusion_metadata\"") != std::string::npos);
    assert(seed_json.find("\"pattern\":\"support_escape\"") != std::string::npos);
    assert(seed_json.find("\"required_summaries\"") != std::string::npos);
    assert(seed_json.find("\"summary\":\"entropy_shannon\"") != std::string::npos);
    assert(seed_json.find("\"summary\":\"top1_share\"") != std::string::npos);
    assert(seed_json.find("\"theta_init\"") != std::string::npos);
    assert(seed_json.find("\"sigma_init\"") != std::string::npos);
    assert(seed_json.find("\"uncertainty_init\"") != std::string::npos);
    assert(seed_json.find("\"component_uncertainty\"") != std::string::npos);
    assert(seed_json.find("\"maturity_init\"") != std::string::npos);
    assert(seed_json.find("\"ratio_prior_init\"") != std::string::npos);
    assert(seed_json.find("\"model\"") == std::string::npos);

    auto [artifact_status, artifact_json] =
        engine.ExportArtifact(artifact, BaselineSerializationFormat::kJson);
    assert(artifact_status == BaselineStatus::kOk);
    assert(artifact_json.find("\"relation_fusion_metadata\"") != std::string::npos);
    BootstrapArtifact loaded_artifact;
    assert(engine.LoadArtifact(
               artifact_json, BaselineSerializationFormat::kJson, &loaded_artifact) ==
           BaselineStatus::kOk);
    assert(loaded_artifact.artifact_kind == BootstrapArtifactKind::kRelation);
    assert(loaded_artifact.relation_basis_by_metric.size() == 1);
    assert(!loaded_artifact.relation_routed_summary_artifacts.empty());
    AssertRelationFusionMetadata(loaded_artifact.relation_fusion_metadata);
    BootstrapSeed loaded_seed;
    assert(engine.ExportSeed(loaded_artifact, &loaded_seed) == BaselineStatus::kOk);
    assert(!loaded_seed.relation_routed_summary_seeds.empty());
    AssertRelationFusionMetadata(loaded_seed.relation_fusion_metadata);

    rapidjson::Document legacy_doc;
    legacy_doc.Parse(artifact_json.c_str());
    assert(!legacy_doc.HasParseError());
    assert(legacy_doc.RemoveMember("relation_fusion_metadata"));
    rapidjson::StringBuffer legacy_buffer;
    rapidjson::Writer<rapidjson::StringBuffer> legacy_writer(legacy_buffer);
    legacy_doc.Accept(legacy_writer);
    BootstrapArtifact legacy_loaded_artifact;
    assert(engine.LoadArtifact(legacy_buffer.GetString(),
                               BaselineSerializationFormat::kJson,
                               &legacy_loaded_artifact) == BaselineStatus::kOk);
    AssertRelationFusionMetadata(legacy_loaded_artifact.relation_fusion_metadata);
    assert(legacy_loaded_artifact.diagnostics.find("relation_fusion_metadata_defaulted") !=
           std::string::npos);

    std::printf("[PASS] Bootstrap engine trains relation basis\n");
}

void TestRatioBootstrapBandIncludesHistoricalResiduals() {
    BootstrapEngine engine;
    BaselineTaskSpec spec;
    spec.task_id = "ratio-residual-band";
    spec.task_kind = "ratio";
    spec.feature_id = "rate";
    spec.feature_type = "ratio";
    spec.profile = "rate_core";
    spec.clock_spec.bucket_seconds = 60;
    spec.clock_spec.timezone = "UTC";

    BootstrapArtifact low_noise;
    BootstrapArtifact high_noise;
    for (int mode = 0; mode < 2; ++mode) {
        RatioBootstrapInput input;
        input.series_key = "svc-band";
        const double amplitude = mode == 0 ? 0.01 : 0.4;
        for (int64_t bucket = 0; bucket < 200; ++bucket) {
            const double p = 0.5 + (bucket % 2 == 0 ? amplitude : -amplitude);
            input.observations.push_back({bucket, p * 100.0, 100.0});
        }
        auto& artifact = mode == 0 ? low_noise : high_noise;
        const auto result = engine.TrainRatio(spec, input, &artifact);
        assert(result.status == BaselineStatus::kOk && result.accepted_count == 200);
        assert(artifact.ratio_model != nullptr);
    }
    assert(high_noise.ratio_model->sigma_ref > low_noise.ratio_model->sigma_ref);

    BootstrapPredictionOptions options;
    options.confidence_level = 0.95;
    const auto quiet = engine.PredictRatio(low_noise, 220, options);
    const auto noisy = engine.PredictRatio(high_noise, 220, options);
    assert(quiet.status == BaselineStatus::kOk && noisy.status == BaselineStatus::kOk);
    assert(noisy.band_width > quiet.band_width);

    const double low_sigma = low_noise.ratio_model->sigma_ref;
    auto sigma_only = low_noise;
    sigma_only.ratio_model = std::make_shared<RatioFormalModel>(*low_noise.ratio_model);
    sigma_only.ratio_model->sigma_ref = high_noise.ratio_model->sigma_ref;
    const auto widened = engine.PredictRatio(sigma_only, 220, options);
    AssertNear(widened.baseline_mu, quiet.baseline_mu);
    AssertNear(widened.confidence, quiet.confidence);
    assert(widened.baseline_lower < quiet.baseline_lower);
    assert(widened.baseline_upper > quiet.baseline_upper);
    AssertNear(low_noise.ratio_model->sigma_ref, low_sigma);

    auto more_coverage = sigma_only;
    more_coverage.coverage_report.accepted_count *= 10;
    const auto covered = engine.PredictRatio(more_coverage, 220, options);
    AssertNear(covered.baseline_mu, widened.baseline_mu);
    AssertNear(covered.baseline_lower, widened.baseline_lower);
    AssertNear(covered.baseline_upper, widened.baseline_upper);

    // Check the envelope contract and agreement at every sequence point.
    const auto check_band = [&](const BootstrapArtifact& artifact, double confidence_level, double z) {
        BootstrapPredictionOptions local_options;
        local_options.confidence_level = confidence_level;
        const auto sequence = engine.PredictRatioSequence(artifact, 220, 8, local_options);
        assert(sequence.status == BaselineStatus::kOk && sequence.predictions.size() == 8);
        for (std::size_t i = 0; i < sequence.predictions.size(); ++i) {
            const auto point = engine.PredictRatio(artifact, 220 + static_cast<int64_t>(i), local_options);
            assert(point.status == BaselineStatus::kOk);
            assert(point.series_key == artifact.series_key);
            assert(std::isfinite(point.baseline_lower) && std::isfinite(point.baseline_upper));
            assert(point.baseline_lower >= 0.0 && point.baseline_upper <= 1.0);
            assert(point.baseline_lower <= point.baseline_mu && point.baseline_upper >= point.baseline_mu);
            const double p = point.baseline_mu;
            const double n = std::max(1.0, static_cast<double>(artifact.coverage_report.accepted_count) +
                                               artifact.ratio_model->alpha0 + artifact.ratio_model->beta0);
            const double probability_half_width = z * std::max(std::sqrt(p * (1.0 - p) / n), 1e-4);
            const double old_lower = std::max(0.0, p - probability_half_width);
            const double old_upper = std::min(1.0, p + probability_half_width);
            const double eta = std::log(p / (1.0 - p));
            const double residual_half_width = z * std::max(artifact.ratio_model->sigma_ref, 1e-3);
            const double residual_lower = 1.0 / (1.0 + std::exp(-eta + residual_half_width));
            const double residual_upper = 1.0 / (1.0 + std::exp(-eta - residual_half_width));
            AssertNear(point.baseline_lower, std::min(old_lower, residual_lower));
            AssertNear(point.baseline_upper, std::max(old_upper, residual_upper));
            AssertNear(point.band_width, point.baseline_upper - point.baseline_lower);
            assert(point.baseline_lower <= old_lower && point.baseline_upper >= old_upper);
            assert(ContainsString(point.uncertainty_source, "ratio_probability_variance"));
            assert(ContainsString(point.uncertainty_source, "ratio_logit_sigma_ref"));
            const auto& batched = sequence.predictions[i];
            assert(batched.status == point.status && batched.bucket_id == point.bucket_id);
            AssertNear(batched.baseline_mu, point.baseline_mu);
            AssertNear(batched.baseline_lower, point.baseline_lower);
            AssertNear(batched.baseline_upper, point.baseline_upper);
            AssertNear(batched.band_width, point.band_width);
            AssertNear(batched.confidence, point.confidence);
            assert(batched.uncertainty_source == point.uncertainty_source);
            if (&artifact == &low_noise) {
                AssertNear(point.baseline_lower, old_lower);
                AssertNear(point.baseline_upper, old_upper);
            }
        }
    };
    for (const auto* artifact : {&low_noise, &high_noise, &sigma_only, &more_coverage}) {
        check_band(*artifact, 0.95, 1.96);
    }
    check_band(sigma_only, 0.80, 1.0);
    check_band(sigma_only, 0.90, 1.645);
    check_band(sigma_only, 0.99, 2.576);
    options.confidence_level = 0.90;
    const auto narrower = engine.PredictRatio(sigma_only, 220, options);
    options.confidence_level = 0.99;
    const auto wider = engine.PredictRatio(sigma_only, 220, options);
    assert(narrower.baseline_lower > wider.baseline_lower);
    assert(narrower.baseline_upper < wider.baseline_upper);

    auto boundary = low_noise;
    boundary.ratio_model = std::make_shared<RatioFormalModel>(*low_noise.ratio_model);
    boundary.ratio_model->core_block = CoreBlock{};
    boundary.ratio_model->monthpos_block.enabled = false;
    boundary.ratio_model->event_block.enabled = false;
    boundary.coverage_report.accepted_count = 1000000000000ULL;
    for (double sigma : {0.0, 1000.0}) {
        boundary.ratio_model->sigma_ref = sigma;
        for (double eta : {-40.0, 0.0, 40.0}) {
            boundary.ratio_model->core_block.beta0 = eta;
            check_band(boundary, 0.95, 1.96);
        }
    }

    auto [export_status, json] = engine.ExportArtifact(high_noise, BaselineSerializationFormat::kJson);
    assert(export_status == BaselineStatus::kOk);
    BootstrapArtifact loaded;
    assert(engine.LoadArtifact(json, BaselineSerializationFormat::kJson, &loaded) == BaselineStatus::kOk);
    AssertNear(loaded.ratio_model->sigma_ref, high_noise.ratio_model->sigma_ref);
    options.confidence_level = 0.95;
    const auto restored = engine.PredictRatio(loaded, 220, options);
    assert(restored.status == BaselineStatus::kOk);
    AssertNear(restored.baseline_mu, noisy.baseline_mu);
    AssertNear(restored.baseline_lower, noisy.baseline_lower);
    AssertNear(restored.baseline_upper, noisy.baseline_upper);
    AssertNear(restored.band_width, noisy.band_width);
    AssertNear(restored.confidence, noisy.confidence);
    check_band(loaded, 0.95, 1.96);
    std::printf("[PASS] Ratio Bootstrap residual envelope: quiet=[%.9f,%.9f], noisy=[%.9f,%.9f]\n",
                quiet.baseline_lower, quiet.baseline_upper, noisy.baseline_lower, noisy.baseline_upper);
}

}  // namespace

int main() {
    TestTaskHeaderContracts();
#ifdef FLOWSQL_BASELINE_CLOSE_RESOURCE_TEST
    TestCloseReleasesResourcesWhileTaskHandlesRemainAlive();
#endif
    TestBootstrapEngineTrainsValueAndRatio();
    TestRatioBootstrapBandIncludesHistoricalResiduals();
    TestBootstrapEngineNormalizesHistoryBeforeTraining();
    TestBootstrapSeedQualityStatusIsEvaluatedFromCoverage();
    TestBootstrapEnginePreservesMonthposArtifactAndSeed();
    TestBootstrapEngineTrainsRelationBasis();
    return 0;
}
