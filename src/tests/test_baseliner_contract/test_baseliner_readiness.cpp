// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <framework/interfaces/ibaseline_evaluation.h>
#include <operators/baseliner/evaluation.h>
#include <plugins/baseline/baseline_plugin.h>
#include <plugins/baseline/checkpoint/state_codec.h>
#include <rapidjson/document.h>
#include <tests/common/test_environment.h>
#include <cassert>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <type_traits>
using namespace flowsql;
using namespace flowsql::baseliner;
namespace {
using Readiness = BaselineEvaluationReadinessV1;
constexpr auto value_config =
    R"({"schema_version":1,"task_id":"readiness","task_name":"readiness","task_kind":"value","feature_id":"v","feature_type":"value_basic","profile":"default","clock_spec":{"bucket_seconds":60,"timezone":"UTC"},"calendar_ref":{"calendar_id":"cn-holiday","calendar_version":"2026.1"}})";
constexpr auto ratio_config =
    R"({"schema_version":1,"task_id":"readiness","task_name":"readiness","task_kind":"ratio","feature_id":"r","feature_type":"ratio","profile":"rate_core","clock_spec":{"bucket_seconds":60,"timezone":"UTC"},"calendar_ref":{"calendar_id":"cn-holiday","calendar_version":"2026.1"}})";
constexpr auto operator_json =
    R"({"schema_version":1,"task_key":"evaluation","source":"sqlite.source","mode":"snapshot",
"clock":{"bucket_seconds":60,"timezone":"UTC"},"calendar":{"calendar_id":"cn-holiday","calendar_version":"2026.1"},
"datasets":[{"id":"d","table":"facts","fields":{"id":"uint64","t":"int64","v":"float64","n":"float64","den":"float64"},
"scope":{"begin_bucket":0,"end_bucket":10000,"consistency":"immutable_range"},"series_keys":["id"],
"deduplicate":{"keys":["id","t"],"on_duplicate":"require_equal"},"bucket":{"column":"t","unit":"bucket_id"},
"metrics":[{"id":"v","kind":"value","column":"v","feature_type":"value_basic","profile":"default"},
{"id":"r","kind":"ratio","feature_type":"ratio","profile":"rate_core","numerator":{"column":"n"},"denominator":{"column":"den"}}]}]})";
constexpr auto relation_json =
    R"({"schema_version":1,"task_key":"relation-evaluation","source":"sqlite.source","mode":"snapshot",
"clock":{"bucket_seconds":60,"timezone":"UTC"},"calendar":{"calendar_id":"cn-holiday","calendar_version":"2026.1"},
"datasets":[{"id":"d","table":"facts","fields":{"g":"uint64","t":"int64","x":"float64"},"scope":{"begin_bucket":0,"end_bucket":10000,"consistency":"immutable_range"},
"series_keys":["g"],"deduplicate":{"keys":["g","t"],"on_duplicate":"require_equal"},"bucket":{"column":"t","unit":"bucket_id"},
"metrics":[{"id":"dist","kind":"relation","feature_type":"relation","profile":"default","group_space":{"id":"groups","version":"v1","column":"g","unknown":"other","other_group_idx":99},
"metrics":[{"id":"m","column":"x","aggregate":"sum"}],"support_policy":{"k_support":3,"min_hist_share":0.01,"min_active_ratio":0.1},"summary_policy":{"k_head":2,"k_stable":1}}]}]})";
struct Environment {
    baseline::BaselinePlugin plugin;
    explicit Environment(const char* options = nullptr) {
        assert(plugin.Option(options) == 0 && plugin.Load(nullptr) == 0 && plugin.Start() == 0);
    }
    ~Environment() {
        plugin.Stop();
        plugin.Unload();
    }
};
struct Value {
    using Task = IBaselineValueTask;
    using Evaluation = IBaselineValueEvaluationV1;
    using Observation = ValueRollingObservation;
    using Bootstrap = ValueBootstrapInput;
    static auto Create(Environment& e) {
        return e.plugin.CreateValueTask(value_config, BaselineSerializationFormat::kJson).second;
    }
    static Observation Input(int64_t t) { return {"s", t, t == 30 ? 1e12 : 100. + t % 3, 1}; }
    static Observation Invalid(int64_t t) { return {"s", t, std::numeric_limits<double>::quiet_NaN(), 1}; }
};
struct Ratio {
    using Task = IBaselineRatioTask;
    using Evaluation = IBaselineRatioEvaluationV1;
    using Observation = RatioRollingObservation;
    using Bootstrap = RatioBootstrapInput;
    static auto Create(Environment& e) {
        return e.plugin.CreateRatioTask(ratio_config, BaselineSerializationFormat::kJson).second;
    }
    static Observation Input(int64_t t) { return {"s", t, t == 30 ? 1. : 800., 1000.}; }
    static Observation Invalid(int64_t t) { return {"s", t, 1., 0.}; }
};
void Equal(const RollingBaselineResult& a, const RollingBaselineResult& b) {
    assert(baseline::checkpoint::Encode(a, 1 << 24) == baseline::checkpoint::Encode(b, 1 << 24));
}
auto Snapshot(const std::shared_ptr<IBaselineTask>& t) {
    return t->QuerySeriesSnapshot("s", BaselineSerializationFormat::kJson);
}
auto Bind(Environment& e, const std::shared_ptr<IBaselineTask>& t) {
    auto control = e.plugin.Bind(t, {8, 8, 8});
    assert(control.first == BaselineStatus::kOk && control.second);
    auto checkpoint = e.plugin.Bind(t, control.second, BaselineCheckpointBindingV1{});
    assert(checkpoint.first == BaselineStatus::kOk && checkpoint.second);
    return checkpoint.second;
}
template <class T>
void ScalarParity() {
    Environment old_env, new_env;
    auto old_task = T::Create(old_env), new_task = T::Create(new_env);
    auto* evaluation = dynamic_cast<typename T::Evaluation*>(new_task.get());
    assert(old_task && new_task && evaluation);
    auto old_checkpoint = Bind(old_env, old_task), new_checkpoint = Bind(new_env, new_task);
    bool skipped = false;
    for (int64_t t = 0; t < 80; ++t) {
        auto old = old_task->SubmitObservation(T::Input(t), {});
        auto next = evaluation->SubmitObservationWithReadiness(T::Input(t), {});
        Equal(old, next.result);
        assert(next.readiness == (t == 0 ? Readiness::kColdStart : Readiness::kReady));
        assert(Snapshot(old_task) == Snapshot(new_task));
        assert(old_checkpoint->ExportCheckpoint(BaselineSerializationFormat::kJson) ==
               new_checkpoint->ExportCheckpoint(BaselineSerializationFormat::kJson));
        skipped |= t > 0 && !next.result.can_update;
        auto before = Snapshot(new_task);
        auto duplicate = evaluation->SubmitObservationWithReadiness(T::Input(t), {});
        assert(duplicate.result.status == BaselineStatus::kInvalidArgument &&
               duplicate.readiness == Readiness::kUnavailable);
        assert(Snapshot(new_task) == before);
        auto invalid = evaluation->SubmitObservationWithReadiness(T::Invalid(t + 1), {});
        assert(invalid.result.status != BaselineStatus::kOk && invalid.readiness == Readiness::kUnavailable);
        assert(Snapshot(new_task) == before);
    }
    assert(skipped);
    auto saved = new_checkpoint->ExportCheckpoint(BaselineSerializationFormat::kJson);
    Environment restored_env;
    auto restored_task = T::Create(restored_env);
    auto restored_checkpoint = Bind(restored_env, restored_task);
    BaselineCheckpointRestoreV1 request;
    request.content = saved.second;
    assert(restored_checkpoint->RestoreCheckpoint(request) == BaselineStatus::kOk);
    auto first =
        dynamic_cast<typename T::Evaluation*>(restored_task.get())->SubmitObservationWithReadiness(T::Input(80), {});
    assert(first.readiness == Readiness::kReady);
    Equal(old_task->SubmitObservation(T::Input(80), {}), first.result);
    new_task->Close();
    assert(evaluation->SubmitObservationWithReadiness(T::Input(81), {}).readiness == Readiness::kUnavailable);
}
template <class T>
void BootstrapReadiness() {
    Environment e;
    auto task = T::Create(e);
    typename T::Bootstrap input;
    input.series_key = "s";
    input.options.min_observation_count = 100;
    for (int64_t t = 0; t < 200; ++t) {
        if constexpr (std::is_same_v<T, Value>)
            input.observations.push_back({t, 100. + t % 5, 1});
        else
            input.observations.push_back({t, 800. + t % 5, 1000.});
    }
    assert(task->Bootstrap(input).ok());
    auto result = dynamic_cast<typename T::Evaluation*>(task.get())->SubmitObservationWithReadiness(T::Input(200), {});
    assert(result.result.status == BaselineStatus::kOk && result.readiness == Readiness::kReady);
}
ConfigSnapshot Config(BaselineTaskKind kind) {
    ConfigSnapshot c;
    auto status = ParseConfig(kind == BaselineTaskKind::kRelation ? relation_json : operator_json, &c);
    assert(status.ok());
    if (kind != BaselineTaskKind::kRelation) {
        auto metric = c.config.datasets[0].metrics[kind == BaselineTaskKind::kValue ? 0 : 1];
        c.config.datasets[0].metrics = {metric};
    }
    return c;
}
Observation Input(BaselineTaskKind kind, int64_t bucket) {
    Observation o;
    o.dataset_id = "d";
    o.metric_id = kind == BaselineTaskKind::kRelation ? "dist" : kind == BaselineTaskKind::kValue ? "v" : "r";
    o.kind = kind;
    o.identity = "s";
    o.source_epoch = "epoch";
    o.bucket = bucket;
    if (kind == BaselineTaskKind::kValue) {
        o.value = 100.;
        o.sample_count = 1000;
    } else if (kind == BaselineTaskKind::kRatio) {
        o.numerator = 800.;
        o.denominator = 1000.;
    } else {
        o.groups = {1, 2, 3};
        o.metrics = {{"m", 1000., 3, {500., 300., 200.}}};
    }
    return o;
}
void LowSupportAndInitialization() {
    Environment e;
    std::string config = value_config;
    config.replace(config.find("value_basic"), 11, "value_sampled");
    config.replace(config.find("default"), 7, "cont_core");
    config.replace(config.find("readiness"), 9, "sampled-readiness");
    auto v = e.plugin.CreateValueTask(config, BaselineSerializationFormat::kJson).second;
    auto r = Ratio::Create(e);
    assert(v && r);
    auto* ve = dynamic_cast<IBaselineValueEvaluationV1*>(v.get());
    auto* re = dynamic_cast<IBaselineRatioEvaluationV1*>(r.get());
    assert(ve && re);
    auto unavailable_v = ve->SubmitObservationWithReadiness({"s", 0, 100., 1}, {});
    auto unavailable_r = re->SubmitObservationWithReadiness({"s", 0, 1., 1.}, {});
    assert(unavailable_v.readiness == Readiness::kUnavailable &&
           unavailable_v.result.status == BaselineStatus::kInsufficientData);
    assert(unavailable_r.readiness == Readiness::kUnavailable &&
           unavailable_r.result.status == BaselineStatus::kInsufficientData);
    assert(unavailable_v.result.observed == 100. && unavailable_r.result.observed == 1.);
    RollingSubmitOptions disabled;
    disabled.allow_auto_init_from_empty = false;
    disabled.allow_auto_init_from_bootstrap = false;
    assert(ve->SubmitObservationWithReadiness({"s", 0, 100., 1000}, disabled).readiness == Readiness::kUnavailable);
    assert(re->SubmitObservationWithReadiness({"s", 0, 800., 1000.}, disabled).readiness == Readiness::kUnavailable);
    assert(ve->SubmitObservationWithReadiness({"s", 0, 100., 1000}, {}).readiness == Readiness::kColdStart);
    assert(re->SubmitObservationWithReadiness({"s", 0, 800., 1000.}, {}).readiness == Readiness::kColdStart);
    auto low_v = ve->SubmitObservationWithReadiness({"s", 1, 100., 1}, {});
    auto low_r = re->SubmitObservationWithReadiness({"s", 1, 1., 1.}, {});
    for (const auto& result : {low_v, low_r}) {
        assert(result.readiness == Readiness::kReady && result.result.status == BaselineStatus::kOk);
        assert(!result.result.can_score && !result.result.can_update && !result.result.can_alert);
    }
    assert(v->PredictRolling("s", 2).status == BaselineStatus::kOk);
    assert(r->PredictRolling("s", 2).status == BaselineStatus::kOk);
}
std::string RelationOptions() {
    const std::string path = flowsql::test::WorkspacePath("baseliner-contract-safety-c1/relation.yml");
    std::ofstream(path) << R"(baseline:
  rolling_config:
    relation_rolling:
      basis_collect_min_buckets: 1
      basis_ready_min_buckets: 1
      basis_refresh_interval_buckets: 1
      basis_candidate_min_coverage_ratio: 0.01
      basis_replacement_cap_ratio: 1.0
      basis_replacement_cap_max: 8
      basis_handover_warmup_buckets: 1
      basis_threshold_margin: 1.0
      basis_min_stable_refresh_count: 1
)";
    return "config_file=" + path + ";strict=false";
}
void RelationReadiness() {
    auto options = RelationOptions();
    Environment old_env(options.c_str()), new_env(options.c_str());
    auto c = Config(BaselineTaskKind::kRelation);
    auto old_task = old_env.plugin
                        .CreateRelationTask(c.config.datasets[0].metrics[0].algorithm_config_json,
                                            BaselineSerializationFormat::kJson)
                        .second;
    auto task = new_env.plugin
                    .CreateRelationTask(c.config.datasets[0].metrics[0].algorithm_config_json,
                                        BaselineSerializationFormat::kJson)
                    .second;
    auto* evaluation = dynamic_cast<IBaselineRelationEvaluationV1*>(task.get());
    assert(old_task && task && evaluation);
    auto oc = Bind(old_env, old_task), nc = Bind(new_env, task);
    std::map<std::string, bool> initialized;
    bool mixed = false, switched = false;
    uint64_t first_basis = 0;
    for (int64_t t = 0; t < 40; ++t) {
        RelationRollingObservation obs;
        obs.series_key = "s";
        obs.bucket_id = t;
        obs.group_idx = t < 20 ? std::vector<uint32_t>{1, 2, 3} : std::vector<uint32_t>{4, 5, 6};
        obs.metrics = {{"m", 1000., 3, {500., 300., 200.}}};
        RelationRollingSubmitOptions submit;
        submit.include_diagnostics = t % 2 == 0;
        auto old = old_task->SubmitObservation(obs, submit);
        auto next = evaluation->SubmitObservationWithReadiness(obs, submit);
        assert(baseline::checkpoint::Encode(old, 1 << 24) == baseline::checkpoint::Encode(next.result, 1 << 24));
        assert(oc->ExportCheckpoint(BaselineSerializationFormat::kJson) ==
               nc->ExportCheckpoint(BaselineSerializationFormat::kJson));
        assert(Snapshot(old_task) == Snapshot(task));
        assert(next.routed_readiness.size() == next.result.routed_results.size());
        bool ready = false, cold = false;
        for (size_t i = 0; i < next.routed_readiness.size(); ++i) {
            const auto& routed = next.result.routed_results[i];
            const auto readiness = next.routed_readiness[i];
            if (routed.rolling.status != BaselineStatus::kOk)
                assert(readiness == Readiness::kUnavailable);
            else {
                assert(readiness ==
                       (initialized[routed.routed_series_key] ? Readiness::kReady : Readiness::kColdStart));
                initialized[routed.routed_series_key] = true;
            }
            ready |= readiness == Readiness::kReady;
            cold |= readiness == Readiness::kColdStart;
            if (routed.basis_scoped) {
                if (!first_basis) first_basis = routed.basis_version;
                switched |= routed.basis_version > first_basis && readiness == Readiness::kColdStart;
            }
        }
        mixed |= ready && cold;
        auto before = Snapshot(task);
        auto duplicate = evaluation->SubmitObservationWithReadiness(obs, submit);
        assert(duplicate.result.status == BaselineStatus::kInvalidArgument && duplicate.routed_readiness.empty() &&
               duplicate.result.routed_results.empty());
        assert(before == Snapshot(task));
    }
    assert(mixed && switched);
    RelationRollingSubmitOptions hidden;
    hidden.include_routed_results = false;
    RelationRollingObservation obs;
    obs.series_key = "s";
    obs.bucket_id = 40;
    obs.group_idx = {4, 5, 6};
    obs.metrics = {{"m", 1000., 3, {500., 300., 200.}}};
    auto next = evaluation->SubmitObservationWithReadiness(obs, hidden);
    auto old = old_task->SubmitObservation(obs, hidden);
    assert(next.result.status == BaselineStatus::kOk && next.result.routed_results.empty() &&
           next.routed_readiness.empty());
    assert(baseline::checkpoint::Encode(old, 1 << 24) == baseline::checkpoint::Encode(next.result, 1 << 24));
    assert(oc->ExportCheckpoint(BaselineSerializationFormat::kJson) ==
           nc->ExportCheckpoint(BaselineSerializationFormat::kJson));
}

// Decorate real task results to verify that diagnostic text cannot control output validity.
struct Calls {
    int legacy = 0, typed = 0, closed = 0, bound = 0;
    int diagnostics = 0;
    bool mismatch = false, unavailable = false;
};
struct ProxyRoot {
    virtual ~ProxyRoot() = default;
    virtual std::shared_ptr<IBaselineTask> Inner() const = 0;
};
void Diagnostics(RollingBaselineResult& r, int mode) {
    if (!mode) return;
    r.uncertainty_source.clear();
    r.diagnostics.clear();
    if (mode == 2) {
        r.uncertainty_source = {"first_observation", "arbitrary future wording"};
        r.diagnostics = "diagnostics changed";
    }
}
struct ValueProxy : IBaselineValueTask, ProxyRoot {
    std::shared_ptr<IBaselineValueTask> inner;
    std::shared_ptr<Calls> calls;
    ValueProxy(std::shared_ptr<IBaselineValueTask> t, std::shared_ptr<Calls> c)
        : inner(std::move(t)), calls(std::move(c)) {}
    std::shared_ptr<IBaselineTask> Inner() const override { return inner; }
    const char* Id() const override { return inner->Id(); }
    const char* Name() const override { return inner->Name(); }
    BaselineTaskKind Kind() const override { return inner->Kind(); }
    BaselineSerializationResult ExportConfig(BaselineSerializationFormat format) const override {
        return inner->ExportConfig(format);
    }
    BaselineSerializationResult QueryTaskSnapshot(BaselineSerializationFormat format) const override {
        return inner->QueryTaskSnapshot(format);
    }
    BaselineSerializationResult QuerySeriesSnapshot(std::string_view series_key,
                                                    BaselineSerializationFormat format) const override {
        return inner->QuerySeriesSnapshot(series_key, format);
    }
    BaselineStatus Close() override {
        ++calls->closed;
        return inner->Close();
    }
    RollingBaselineResult SubmitObservation(const ValueRollingObservation& obs,
                                            const RollingSubmitOptions& options) override {
        ++calls->legacy;
        return inner->SubmitObservation(obs, options);
    }
    RollingPrediction PredictRolling(std::string_view series_key, int64_t bucket_id) const override {
        return inner->PredictRolling(series_key, bucket_id);
    }
    RollingPredictionSequence PredictRolling(std::string_view series_key, int64_t start_bucket_id,
                                             uint32_t point_count) const override {
        return inner->PredictRolling(series_key, start_bucket_id, point_count);
    }
    BootstrapTrainResult Bootstrap(const ValueBootstrapInput& input) override { return inner->Bootstrap(input); }
    BootstrapPrediction PredictBootstrap(std::string_view series_key, int64_t bucket_id,
                                         const BootstrapPredictionOptions& options) const override {
        return inner->PredictBootstrap(series_key, bucket_id, options);
    }
    BootstrapPredictionSequence PredictBootstrap(std::string_view series_key, int64_t start_bucket_id,
                                                 uint32_t point_count,
                                                 const BootstrapPredictionOptions& options) const override {
        return inner->PredictBootstrap(series_key, start_bucket_id, point_count, options);
    }
    BaselineSerializationResult ExportBootstrapArtifact(BaselineSerializationFormat format) const override {
        return inner->ExportBootstrapArtifact(format);
    }
    BaselineStatus LoadBootstrapArtifact(std::string_view content, BaselineSerializationFormat format) override {
        return inner->LoadBootstrapArtifact(content, format);
    }
    BaselineSerializationResult ExportBootstrapSeed(BaselineSerializationFormat format) const override {
        return inner->ExportBootstrapSeed(format);
    }
};
struct TypedValueProxy final : ValueProxy, IBaselineValueEvaluationV1 {
    using ValueProxy::ValueProxy;
    BaselineEvaluationResultV1 SubmitObservationWithReadiness(const ValueRollingObservation& obs,
                                                              const RollingSubmitOptions& options) override {
        ++calls->typed;
        auto* evaluation = dynamic_cast<IBaselineValueEvaluationV1*>(inner.get());
        assert(evaluation);
        auto output = evaluation->SubmitObservationWithReadiness(obs, options);
        Diagnostics(output.result, calls->diagnostics);
        if (calls->unavailable) {
            output.readiness = Readiness::kUnavailable;
            output.result.status = BaselineStatus::kNotTrained;
        }
        return output;
    }
};
struct RatioProxy : IBaselineRatioTask, ProxyRoot {
    std::shared_ptr<IBaselineRatioTask> inner;
    std::shared_ptr<Calls> calls;
    RatioProxy(std::shared_ptr<IBaselineRatioTask> t, std::shared_ptr<Calls> c)
        : inner(std::move(t)), calls(std::move(c)) {}
    std::shared_ptr<IBaselineTask> Inner() const override { return inner; }
    const char* Id() const override { return inner->Id(); }
    const char* Name() const override { return inner->Name(); }
    BaselineTaskKind Kind() const override { return inner->Kind(); }
    BaselineSerializationResult ExportConfig(BaselineSerializationFormat format) const override {
        return inner->ExportConfig(format);
    }
    BaselineSerializationResult QueryTaskSnapshot(BaselineSerializationFormat format) const override {
        return inner->QueryTaskSnapshot(format);
    }
    BaselineSerializationResult QuerySeriesSnapshot(std::string_view series_key,
                                                    BaselineSerializationFormat format) const override {
        return inner->QuerySeriesSnapshot(series_key, format);
    }
    BaselineStatus Close() override {
        ++calls->closed;
        return inner->Close();
    }
    RollingBaselineResult SubmitObservation(const RatioRollingObservation& obs,
                                            const RollingSubmitOptions& options) override {
        ++calls->legacy;
        return inner->SubmitObservation(obs, options);
    }
    RollingPrediction PredictRolling(std::string_view series_key, int64_t bucket_id) const override {
        return inner->PredictRolling(series_key, bucket_id);
    }
    RollingPredictionSequence PredictRolling(std::string_view series_key, int64_t start_bucket_id,
                                             uint32_t point_count) const override {
        return inner->PredictRolling(series_key, start_bucket_id, point_count);
    }
    BootstrapTrainResult Bootstrap(const RatioBootstrapInput& input) override { return inner->Bootstrap(input); }
    BootstrapPrediction PredictBootstrap(std::string_view series_key, int64_t bucket_id,
                                         const BootstrapPredictionOptions& options) const override {
        return inner->PredictBootstrap(series_key, bucket_id, options);
    }
    BootstrapPredictionSequence PredictBootstrap(std::string_view series_key, int64_t start_bucket_id,
                                                 uint32_t point_count,
                                                 const BootstrapPredictionOptions& options) const override {
        return inner->PredictBootstrap(series_key, start_bucket_id, point_count, options);
    }
    BaselineSerializationResult ExportBootstrapArtifact(BaselineSerializationFormat format) const override {
        return inner->ExportBootstrapArtifact(format);
    }
    BaselineStatus LoadBootstrapArtifact(std::string_view content, BaselineSerializationFormat format) override {
        return inner->LoadBootstrapArtifact(content, format);
    }
    BaselineSerializationResult ExportBootstrapSeed(BaselineSerializationFormat format) const override {
        return inner->ExportBootstrapSeed(format);
    }
};
struct TypedRatioProxy final : RatioProxy, IBaselineRatioEvaluationV1 {
    using RatioProxy::RatioProxy;
    BaselineEvaluationResultV1 SubmitObservationWithReadiness(const RatioRollingObservation& obs,
                                                              const RollingSubmitOptions& options) override {
        ++calls->typed;
        auto* evaluation = dynamic_cast<IBaselineRatioEvaluationV1*>(inner.get());
        assert(evaluation);
        auto output = evaluation->SubmitObservationWithReadiness(obs, options);
        Diagnostics(output.result, calls->diagnostics);
        if (calls->unavailable) {
            output.readiness = Readiness::kUnavailable;
            output.result.status = BaselineStatus::kNotTrained;
        }
        return output;
    }
};
struct RelationProxy : IBaselineRelationTask, ProxyRoot {
    std::shared_ptr<IBaselineRelationTask> inner;
    std::shared_ptr<Calls> calls;
    RelationProxy(std::shared_ptr<IBaselineRelationTask> t, std::shared_ptr<Calls> c)
        : inner(std::move(t)), calls(std::move(c)) {}
    std::shared_ptr<IBaselineTask> Inner() const override { return inner; }
    const char* Id() const override { return inner->Id(); }
    const char* Name() const override { return inner->Name(); }
    BaselineTaskKind Kind() const override { return inner->Kind(); }
    BaselineSerializationResult ExportConfig(BaselineSerializationFormat format) const override {
        return inner->ExportConfig(format);
    }
    BaselineSerializationResult QueryTaskSnapshot(BaselineSerializationFormat format) const override {
        return inner->QueryTaskSnapshot(format);
    }
    BaselineSerializationResult QuerySeriesSnapshot(std::string_view series_key,
                                                    BaselineSerializationFormat format) const override {
        return inner->QuerySeriesSnapshot(series_key, format);
    }
    BaselineStatus Close() override {
        ++calls->closed;
        return inner->Close();
    }
    RelationRollingResult SubmitObservation(const RelationRollingObservation& obs,
                                            const RelationRollingSubmitOptions& options) override {
        ++calls->legacy;
        return inner->SubmitObservation(obs, options);
    }
    RollingPrediction PredictRoutedSummary(const RelationRoutedSummaryQuery& query, int64_t bucket_id) const override {
        return inner->PredictRoutedSummary(query, bucket_id);
    }
    BaselineSerializationResult QueryRoutedSummarySnapshot(const RelationRoutedSummaryQuery& query,
                                                           BaselineSerializationFormat format) const override {
        return inner->QueryRoutedSummarySnapshot(query, format);
    }
    BootstrapTrainResult Bootstrap(const RelationBootstrapInput& input) override { return inner->Bootstrap(input); }
    BaselineSerializationResult ExportBootstrapArtifact(BaselineSerializationFormat format) const override {
        return inner->ExportBootstrapArtifact(format);
    }
    BaselineStatus LoadBootstrapArtifact(std::string_view content, BaselineSerializationFormat format) override {
        return inner->LoadBootstrapArtifact(content, format);
    }
    BaselineSerializationResult ExportBootstrapSeed(BaselineSerializationFormat format) const override {
        return inner->ExportBootstrapSeed(format);
    }
    BaselineSerializationResult QueryBootstrapBasis(BaselineSerializationFormat format) const override {
        return inner->QueryBootstrapBasis(format);
    }
};
struct TypedRelationProxy final : RelationProxy, IBaselineRelationEvaluationV1 {
    using RelationProxy::RelationProxy;
    BaselineRelationEvaluationResultV1 SubmitObservationWithReadiness(
        const RelationRollingObservation& obs, const RelationRollingSubmitOptions& options) override {
        ++calls->typed;
        auto* evaluation = dynamic_cast<IBaselineRelationEvaluationV1*>(inner.get());
        assert(evaluation);
        auto output = evaluation->SubmitObservationWithReadiness(obs, options);
        for (auto& r : output.result.routed_results) Diagnostics(r.rolling, calls->diagnostics);
        if (calls->mismatch && !output.routed_readiness.empty()) output.routed_readiness.pop_back();
        return output;
    }
};

struct ProxyService final : IBaselineService, IBaselineStateControlServiceV1 {
    baseline::BaselinePlugin& plugin;
    bool typed;
    std::shared_ptr<Calls> calls = std::make_shared<Calls>();
    ProxyService(Environment& e, bool ready) : plugin(e.plugin), typed(ready) {}
    std::pair<BaselineStatus, std::shared_ptr<IBaselineValueTask>> CreateValueTask(
        std::string_view config, BaselineSerializationFormat format) override {
        auto created = plugin.CreateValueTask(config, format);
        assert(created.first == BaselineStatus::kOk && created.second);
        std::shared_ptr<IBaselineValueTask> proxy;
        if (typed)
            proxy = std::make_shared<TypedValueProxy>(created.second, calls);
        else
            proxy = std::make_shared<ValueProxy>(created.second, calls);
        return {created.first, std::move(proxy)};
    }
    std::pair<BaselineStatus, std::shared_ptr<IBaselineRatioTask>> CreateRatioTask(
        std::string_view config, BaselineSerializationFormat format) override {
        auto created = plugin.CreateRatioTask(config, format);
        assert(created.first == BaselineStatus::kOk && created.second);
        std::shared_ptr<IBaselineRatioTask> proxy;
        if (typed)
            proxy = std::make_shared<TypedRatioProxy>(created.second, calls);
        else
            proxy = std::make_shared<RatioProxy>(created.second, calls);
        return {created.first, std::move(proxy)};
    }
    std::pair<BaselineStatus, std::shared_ptr<IBaselineRelationTask>> CreateRelationTask(
        std::string_view config, BaselineSerializationFormat format) override {
        auto created = plugin.CreateRelationTask(config, format);
        assert(created.first == BaselineStatus::kOk && created.second);
        std::shared_ptr<IBaselineRelationTask> proxy;
        if (typed)
            proxy = std::make_shared<TypedRelationProxy>(created.second, calls);
        else
            proxy = std::make_shared<RelationProxy>(created.second, calls);
        return {created.first, std::move(proxy)};
    }
    BaselineSerializationResult QueryServiceSnapshot(BaselineSerializationFormat format) const override {
        return plugin.QueryServiceSnapshot(format);
    }
    std::pair<BaselineStatus, std::shared_ptr<IBaselineTaskStateControlV1>> Bind(
        std::shared_ptr<IBaselineTask> task, const BaselineStateLimitsV1& limits) override {
        ++calls->bound;
        auto* proxy = dynamic_cast<ProxyRoot*>(task.get());
        assert(proxy);
        return plugin.Bind(proxy->Inner(), limits);
    }
};
void MissingCapability() {
    for (auto kind : {BaselineTaskKind::kValue, BaselineTaskKind::kRatio, BaselineTaskKind::kRelation}) {
        Environment env;
        ProxyService service(env, false);
        EvaluationEngine e;
        assert(e.Open(Config(kind), &service, &service) != 0);
        assert(e.LastError().find("V1 capability") != std::string::npos);
        assert(service.calls->legacy == 0 && service.calls->typed == 0 && service.calls->bound == 0 &&
               service.calls->closed == 1);
        EvaluationOutput out;
        assert(e.Submit(Input(kind, 0), &out) != 0 && out.results.empty());
    }
}
void OutputEqual(const EvaluationOutput& a, const EvaluationOutput& b) {
    assert(a.results.size() == b.results.size() && a.forecasts.size() == b.forecasts.size());
    auto equal = [](const EvaluationRow& x, const EvaluationRow& y) {
        assert(x.values.observed == y.values.observed && x.values.expected == y.values.expected &&
               x.values.lower == y.values.lower && x.values.upper == y.values.upper);
        assert(x.values.status == y.values.status && x.values.can_alert == y.values.can_alert);
    };
    for (size_t i = 0; i < a.results.size(); ++i) equal(a.results[i], b.results[i]);
    for (size_t i = 0; i < a.forecasts.size(); ++i) equal(a.forecasts[i], b.forecasts[i]);
}
void DiagnosticIndependence() {
    for (auto kind : {BaselineTaskKind::kValue, BaselineTaskKind::kRatio, BaselineTaskKind::kRelation}) {
        for (int mode : {1, 2}) {
            auto options = RelationOptions();
            Environment original(options.c_str()), decorated(options.c_str());
            ProxyService left(original, true), right(decorated, true);
            right.calls->diagnostics = mode;
            EvaluationEngine a, b;
            auto c = Config(kind);
            c.config.horizon_buckets = 2;
            assert(a.Open(c, &left, &left) == 0 && b.Open(c, &right, &right) == 0);
            for (int64_t t = 0; t < 6; ++t) {
                EvaluationOutput x, y;
                assert(a.Submit(Input(kind, t), &x) == 0 && b.Submit(Input(kind, t), &y) == 0);
                OutputEqual(x, y);
                if (kind != BaselineTaskKind::kRelation) {
                    assert(y.results.size() == 1 && y.results[0].values.observed);
                    assert(bool(y.results[0].values.expected) == (t > 0));
                    assert(y.results[0].values.status == (t ? BaselineStatus::kOk : BaselineStatus::kNotTrained));
                    // Cold-start learning can forecast the future while the current bucket stays NULL.
                    assert(y.forecasts.size() == 2 && y.forecasts[0].values.expected);
                }
            }
            assert(left.calls->typed == 6 && right.calls->typed == 6 && left.calls->legacy == 0 &&
                   right.calls->legacy == 0);
        }
    }
    for (auto kind : {BaselineTaskKind::kValue, BaselineTaskKind::kRatio}) {
        Environment env;
        ProxyService service(env, true);
        service.calls->unavailable = true;
        service.calls->diagnostics = 2;
        EvaluationEngine e;
        assert(e.Open(Config(kind), &service, &service) == 0);
        EvaluationOutput out;
        assert(e.Submit(Input(kind, 0), &out) == 0 && out.results.size() == 1);
        assert(out.results[0].values.observed && !out.results[0].values.expected &&
               out.results[0].values.status == BaselineStatus::kNotTrained);
    }
    Environment env;
    ProxyService broken(env, true);
    broken.calls->mismatch = true;
    EvaluationEngine e;
    assert(e.Open(Config(BaselineTaskKind::kRelation), &broken, &broken) == 0);
    EvaluationOutput out;
    assert(e.Submit(Input(BaselineTaskKind::kRelation, 0), &out) != 0 && out.results.empty() && !out.relation);
    assert(e.LastError().find("readiness length mismatch") != std::string::npos);
}

}  // namespace
int main() {
    ScalarParity<Value>();
    ScalarParity<Ratio>();
    BootstrapReadiness<Value>();
    BootstrapReadiness<Ratio>();
    LowSupportAndInitialization();
    RelationReadiness();
    MissingCapability();
    DiagnosticIndependence();
    std::cout << "readiness contracts passed\n";
}
