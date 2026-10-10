// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include "evaluation.h"
#include <arrow/io/memory.h>
#include <arrow/ipc/reader.h>
#include <arrow/ipc/writer.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <utility>
#include "durable_store.h"
#include "result_codec.h"

namespace flowsql::baseliner {
namespace {
std::string Hex(std::string_view value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(value.size() * 2);
    for (unsigned char c : value) {
        out += digits[c >> 4];
        out += digits[c & 15];
    }
    return out;
}
bool Finite(const std::optional<double>& value) { return value && std::isfinite(*value); }
bool Cold(const RollingBaselineResult& result) {
    return std::find(result.uncertainty_source.begin(), result.uncertainty_source.end(), "first_observation") !=
           result.uncertainty_source.end();
}
Observation Identity(const Observation& input) {
    Observation identity;
    identity.dataset_id = input.dataset_id;
    identity.metric_id = input.metric_id;
    identity.identity = input.identity;
    identity.source_epoch = input.source_epoch;
    identity.kind = input.kind;
    identity.bucket = input.bucket;
    return identity;
}
EvaluationRow Row(const Observation& input, const RollingBaselineResult& result, const std::string& basis) {
    EvaluationRow row;
    row.input = Identity(input);
    row.rolling = result;
    row.model_basis_id = basis;
    row.unit = input.kind == BaselineTaskKind::kRatio ? "ratio" : "value";
    row.values.target_bucket = row.values.issued_after_bucket = input.bucket;
    row.values.observed = result.observed;
    row.values.status = result.status;
    row.values.can_alert = false;
    if (result.status == BaselineStatus::kOk && !Cold(result)) {
        row.values.expected = result.baseline_mu;
        row.values.lower = result.baseline_lower;
        row.values.upper = result.baseline_upper;
        row.values.can_alert = result.can_alert;
    } else if (Cold(result))
        row.values.status = BaselineStatus::kNotTrained;
    return row;
}
EvaluationRow RelationRow(const Observation& input, const RelationRoutedSummaryResult& routed,
                          const std::string& basis) {
    auto row = Row(input, routed.rolling, basis);
    row.summary_id = routed.summary;
    row.input.metric_id = routed.metric;
    row.basis_id = routed.basis_scoped ? std::to_string(routed.basis_version) : "";
    row.unit = routed.feature_type;
    return row;
}
EvaluationRow ForecastRow(const Observation& input, const RollingPrediction& prediction, std::string basis,
                          std::string summary = {}, std::string basis_id = {}) {
    EvaluationRow row;
    row.input = Identity(input);
    row.values.forecast = true;
    row.values.target_bucket = prediction.bucket_id;
    row.values.issued_after_bucket = input.bucket;
    row.values.status = prediction.status;
    row.model_basis_id = std::move(basis);
    row.summary_id = std::move(summary);
    row.basis_id = std::move(basis_id);
    row.band_kind = "forecast";
    row.unit = input.kind == BaselineTaskKind::kRelation ? "relation"
                                                         : (input.kind == BaselineTaskKind::kRatio ? "ratio" : "value");
    if (prediction.status == BaselineStatus::kOk) {
        row.values.expected = prediction.baseline_mu;
        row.values.lower = prediction.baseline_lower;
        row.values.upper = prediction.baseline_upper;
    }
    return row;
}
}  // namespace
struct EvaluationEngine::Impl {
    struct Series {
        std::string epoch;
        std::optional<int64_t> last;
        bool initialized = false;
        bool bootstrapped = false;
        uint64_t revision = 0;
        uint64_t bytes = 0;
        std::vector<Observation> history;
        int64_t active_ns = 0;
    };
    struct Module {
        Metric metric;
        std::shared_ptr<IBaselineTask> task;
        std::shared_ptr<IBaselineTaskStateControlV1> control;
        std::shared_ptr<IBaselineCheckpointV1> checkpoint;
        std::map<std::string, Series> series;
        std::set<std::string> parents;
        std::string released_epoch;
        ~Module() {
            if (task) task->Close();
        }
    };
    ConfigSnapshot config;
    IBaselineService* service = nullptr;
    IBaselineStateControlServiceV1* state_service = nullptr;
    IBaselineCheckpointServiceV1* checkpoint_service = nullptr;
    std::map<std::tuple<std::string, std::string, std::string>, std::pair<std::string, int64_t>> replay;
    bool restored = false;
    int64_t monotonic_ns = 0, wall_ns = 0;
    using ActivityKey = std::tuple<int64_t, std::string, std::string, std::string>;
    std::set<ActivityKey> activity;
    BlockInputProgressV1 progress;
    std::vector<MaintenanceRow> maintenance;
    int64_t Timeout() const { return int64_t(config.config.state.idle_timeout_ms) * 1000000; }
    void Touch(const std::pair<std::string, std::string>& key, const std::string& identity, Series& state) {
        if (!Timeout()) return;
        activity.erase({state.active_ns, key.first, key.second, identity});
        state.active_ns = monotonic_ns;
        activity.emplace(state.active_ns, key.first, key.second, identity);
    }
    uint64_t ReleaseWork(const Module& module, const std::string& identity, const Series& state) const {
        // Conservative work charge: each retained routed child and bounded basis version is included.
        uint64_t children = 1;
        if (module.metric.kind == BaselineTaskKind::kRelation)
            children += module.metric.relation_metrics.size() *
                        (4 + uint64_t(config.config.state.limits.max_basis_versions_per_metric) *
                                 (3 + module.metric.k_stable + module.metric.k_support));
        return state.bytes + sizeof(Series) + identity.size() * 4 + state.epoch.size() + children * 4096;
    }
    int CheckAdmission(const Observation& input) {
        auto& module = *modules.at({input.dataset_id, input.metric_id});
        if (module.series.count(input.identity)) return 0;
        if (module.released_epoch.empty()) return 0;
        auto position = std::find_if(progress.datasets.begin(), progress.datasets.end(),
                                     [&](const auto& p) { return p.dataset_id == input.dataset_id; });
        if (module.released_epoch == input.source_epoch && position == progress.datasets.end())
            return Fail("post-release input requires upstream progress or a new epoch");
        if (position != progress.datasets.end() &&
            (position->epoch != input.source_epoch || input.bucket < position->closed_before_bucket))
            return Fail("new lifecycle input precedes confirmed source progress");
        return 0;
    }
    std::map<std::pair<std::string, std::string>, std::unique_ptr<Module>> modules;
    uint64_t history_bytes = 0;
    uint64_t identities = 0;
    uint64_t models = 0;
    bool opened = false;
    bool finished = false;
    std::string error;
    int Fail(std::string message) {
        error = std::move(message);
        return -1;
    }
    std::string Basis(const Observation& input, const Series& state) const {
        return config.sha256_hex + ":" + Hex(input.identity) + ":" + Hex(input.source_epoch) + ":" +
               (state.bootstrapped ? "bootstrap:" : "cold:") + std::to_string(state.revision);
    }
    bool FitsRows(const Observation& input, const std::string& basis, uint64_t count) const {
        // Reject large forecast fanout before allocating owned copies of identity/model references.
        const uint64_t per_row = sizeof(EvaluationRow) + input.dataset_id.size() + input.metric_id.size() +
                                 input.identity.size() * 3 + input.source_epoch.size() + basis.size();
        return count <= config.config.read.max_pending_bytes / per_row;
    }
    std::string Invalid(const Observation& input) const {
        auto found = modules.find({input.dataset_id, input.metric_id});
        if (found == modules.end() || found->second->metric.kind != input.kind || input.identity.empty() ||
            input.identity.size() > kMaxConfigBytes || input.source_epoch.empty() ||
            input.source_epoch.size() > kMaxConfigBytes)
            return "invalid observation identity/kind";
        auto& module = *found->second;
        if (config.config.horizon_buckets &&
            input.bucket > std::numeric_limits<int64_t>::max() - config.config.horizon_buckets)
            return "forecast bucket range overflow";
        if (input.kind == BaselineTaskKind::kRelation) {
            if (input.value || input.sample_count || input.numerator || input.denominator || input.groups.empty() ||
                input.metrics.size() != module.metric.relation_metrics.size())
                return "invalid relation observation";
            std::set<uint32_t> groups(input.groups.begin(), input.groups.end());
            if (groups.size() != input.groups.size()) return "duplicate relation group index";
            for (size_t i = 0; i < input.metrics.size(); ++i) {
                const auto& metric = input.metrics[i];
                if (metric.metric != module.metric.relation_metrics[i].id || !std::isfinite(metric.total) ||
                    metric.total <= 0 || metric.active_count > input.groups.size() ||
                    metric.values_by_group.size() != input.groups.size())
                    return "invalid relation metric order/header";
                for (double value : metric.values_by_group)
                    if (!std::isfinite(value) || value < 0) return "invalid relation group mass";
            }
        } else if (input.kind == BaselineTaskKind::kValue
                       ? (!Finite(input.value) || *input.value < 0 ||
                          (module.metric.feature_type == "value_sampled" && !input.sample_count) || input.numerator ||
                          input.denominator || !input.metrics.empty() || !input.groups.empty())
                       : (!Finite(input.numerator) || !Finite(input.denominator) || *input.denominator <= 0 ||
                          *input.numerator < 0 || *input.numerator > *input.denominator || input.value ||
                          !input.metrics.empty() || !input.groups.empty()))
            return "invalid scalar observation";
        auto old = module.series.find(input.identity);
        if (old != module.series.end() &&
            (old->second.epoch != input.source_epoch || (old->second.last && input.bucket <= *old->second.last)))
            return "duplicate/late observation or source epoch change";
        return {};
    }
    int Warmup(Module& module, const std::string& identity, Series& state) {
        if (state.initialized) return 0;
        const auto& policy = config.config.bootstrap;
        if (module.parents.count(identity)) {
            state.bootstrapped = true;
            state.initialized = true;
            history_bytes -= state.bytes;
            state.bytes = 0;
            std::vector<Observation>{}.swap(state.history);
            return 0;
        }
        if (policy.history) {
            BootstrapTrainResult trained;
            trained.status = BaselineStatus::kInsufficientData;
            if (state.history.size() >= policy.min_observations) {
                if (models >= config.config.state.limits.max_model_identities)
                    return Fail("bootstrap model capacity exceeded; RuntimeOnly retains parent model quota");
                BootstrapTrainOptions options;
                options.min_observation_count = policy.min_observations;
                options.force_replace_existing_artifact = false;
                if (module.metric.kind == BaselineTaskKind::kValue) {
                    ValueBootstrapInput input;
                    input.series_key = Hex(identity);
                    input.options = options;
                    for (const auto& o : state.history)
                        input.observations.push_back({o.bucket, *o.value, o.sample_count.value_or(1)});
                    trained = std::static_pointer_cast<IBaselineValueTask>(module.task)->Bootstrap(input);
                } else if (module.metric.kind == BaselineTaskKind::kRatio) {
                    RatioBootstrapInput input;
                    input.series_key = Hex(identity);
                    input.options = options;
                    for (const auto& o : state.history)
                        input.observations.push_back({o.bucket, *o.numerator, *o.denominator});
                    trained = std::static_pointer_cast<IBaselineRatioTask>(module.task)->Bootstrap(input);
                } else {
                    RelationBootstrapInput input;
                    input.series_key = Hex(identity);
                    input.options = options;
                    for (const auto& o : state.history) {
                        RelationBootstrapBlock block;
                        block.bucket_id = o.bucket;
                        block.group_idx = o.groups;
                        for (const auto& metric : o.metrics)
                            block.metrics.push_back(
                                {metric.metric, metric.total, metric.active_count, metric.values_by_group});
                        input.blocks.push_back(std::move(block));
                    }
                    trained = std::static_pointer_cast<IBaselineRelationTask>(module.task)->Bootstrap(input);
                }
            }
            if (!trained.ok() && !(trained.status == BaselineStatus::kInsufficientData && policy.insufficient_cold))
                return Fail("history bootstrap failed: " + std::to_string(static_cast<int>(trained.status)));
            state.bootstrapped = trained.ok();
            if (state.bootstrapped) {
                ++models;
                module.parents.insert(identity);
                ++state.revision;
            }
        }
        history_bytes -= state.bytes;
        state.bytes = 0;
        std::vector<Observation>{}.swap(state.history);
        state.initialized = true;
        return 0;
    }
};
EvaluationEngine::EvaluationEngine() : impl_(std::make_unique<Impl>()) {}
EvaluationEngine::~EvaluationEngine() = default;
const std::string& EvaluationEngine::LastError() const { return impl_->error; }
void EvaluationEngine::Close() {
    impl_->modules.clear();
    impl_->activity.clear();
    impl_->replay.clear();
    impl_->maintenance.clear();
    impl_->progress = {};
    impl_->history_bytes = impl_->identities = impl_->models = 0;
    impl_->opened = false;
    impl_->finished = true;
}
int EvaluationEngine::Open(ConfigSnapshot config, IBaselineService* service, IBaselineStateControlServiceV1* control,
                           IBaselineCheckpointServiceV1* checkpoints) {
    if (impl_->opened || impl_->finished || !impl_->modules.empty() || !service || !control)
        return impl_->Fail("invalid evaluation open");
    impl_->config = std::move(config);
    impl_->service = service;
    impl_->state_service = control;
    impl_->checkpoint_service = checkpoints;
    for (const auto& dataset : impl_->config.config.datasets) {
        for (const auto& metric : dataset.metrics) {
            auto module = std::make_unique<Impl::Module>();
            module->metric = metric;
            BaselineStatus status = BaselineStatus::kInvalidArgument;
            if (metric.kind == BaselineTaskKind::kValue) {
                auto created =
                    service->CreateValueTask(metric.algorithm_config_json, BaselineSerializationFormat::kJson);
                status = created.first;
                module->task = std::move(created.second);
            } else if (metric.kind == BaselineTaskKind::kRatio) {
                auto created =
                    service->CreateRatioTask(metric.algorithm_config_json, BaselineSerializationFormat::kJson);
                status = created.first;
                module->task = std::move(created.second);
            } else {
                auto created =
                    service->CreateRelationTask(metric.algorithm_config_json, BaselineSerializationFormat::kJson);
                status = created.first;
                module->task = std::move(created.second);
            }
            if (status != BaselineStatus::kOk || !module->task) {
                Close();
                return impl_->Fail("algorithm task creation failed");
            }
            auto bound = control->Bind(module->task, impl_->config.config.state.limits);
            if (bound.first != BaselineStatus::kOk || !bound.second) {
                Close();
                return impl_->Fail("algorithm capacity binding failed");
            }
            module->control = std::move(bound.second);
            if (checkpoints) {
                BaselineCheckpointBindingV1 binding;
                binding.max_payload_bytes = impl_->config.config.persistence.max_checkpoint_bytes;
                auto checkpoint = checkpoints->Bind(module->task, module->control, binding);
                if (checkpoint.first != BaselineStatus::kOk || !checkpoint.second) {
                    Close();
                    return impl_->Fail("checkpoint binding failed");
                }
                module->checkpoint = std::move(checkpoint.second);
            }
            impl_->modules.emplace(std::make_pair(dataset.id, metric.id), std::move(module));
        }
    }
    impl_->opened = true;
    impl_->finished = false;
    return 0;
}
int EvaluationEngine::Submit(const Observation& input, EvaluationOutput* output) {
    if (!output) return impl_->Fail("null evaluation output");
    *output = {};
    if (!impl_->opened || impl_->finished) return impl_->Fail("evaluation engine closed");
    auto invalid = impl_->Invalid(input);
    if (!invalid.empty()) return impl_->Fail(std::move(invalid));
    auto& module = *impl_->modules.at({input.dataset_id, input.metric_id});
    auto old = module.series.find(input.identity);
    const auto& policy = impl_->config.config.bootstrap;
    if (policy.history && input.bucket < policy.begin_bucket) return 0;
    if (old == module.series.end()) {
        if (impl_->CheckAdmission(input) != 0) return -1;
        if (impl_->identities >= impl_->config.config.state.limits.max_runtime_identities) {
            if (!impl_->config.config.state.evict_idle) return impl_->Fail("evaluation identity capacity exceeded");
            if (Maintain(&impl_->maintenance) != 0) return -1;
            if (impl_->identities >= impl_->config.config.state.limits.max_runtime_identities)
                return impl_->Fail("capacity has no expired candidate within maintenance budget");
        }
    }
    if (old == module.series.end()) {
        old = module.series.emplace(input.identity, Impl::Series{}).first;
        old->second.epoch = input.source_epoch;
        if (!module.released_epoch.empty() && (!policy.history || input.bucket >= policy.end_bucket)) {
            old->second.initialized = true;
            old->second.bootstrapped = module.parents.count(input.identity);
        }
        ++impl_->identities;
    }
    auto& state = old->second;
    if (policy.history && input.bucket < policy.end_bucket) {
        uint64_t bytes = sizeof(Observation) + input.dataset_id.size() + input.metric_id.size() +
                         input.identity.size() + input.source_epoch.size() + input.groups.size() * sizeof(uint32_t);
        for (const auto& metric : input.metrics)
            bytes += sizeof(RelationValues) + metric.metric.size() + metric.values_by_group.size() * sizeof(double);
        if (bytes > impl_->config.config.read.max_pending_bytes - impl_->history_bytes)
            return impl_->Fail("history buffer budget exceeded");
        state.history.push_back(input);
        state.bytes += bytes;
        impl_->history_bytes += bytes;
        state.last = input.bucket;
        impl_->Touch({input.dataset_id, input.metric_id}, input.identity, state);
        return 0;
    }
    if (impl_->Warmup(module, input.identity, state) != 0) return -1;
    const auto basis = impl_->Basis(input, state);
    EvaluationOutput next;
    if (input.kind == BaselineTaskKind::kRelation) {
        RelationRollingObservation observation;
        observation.series_key = Hex(input.identity);
        observation.bucket_id = input.bucket;
        observation.group_idx = input.groups;
        for (const auto& metric : input.metrics)
            observation.metrics.push_back({metric.metric, metric.total, metric.active_count, metric.values_by_group});
        auto relation =
            std::static_pointer_cast<IBaselineRelationTask>(module.task)->SubmitObservation(observation, {});
        if (relation.status != BaselineStatus::kOk) return impl_->Fail("algorithm relation Submit failed");
        if (!impl_->FitsRows(input, basis,
                             relation.routed_results.size() * (uint64_t(impl_->config.config.horizon_buckets) + 1)))
            return impl_->Fail("relation evaluation/forecast budget exceeded");
        next.relation = relation;
        for (const auto& routed : relation.routed_results) {
            auto row = RelationRow(input, routed, basis);
            if (!ValidateResult(row.values).ok()) return impl_->Fail("invalid relation evaluation result");
            next.results.push_back(std::move(row));
        }
        state.last = input.bucket;
        impl_->Touch({input.dataset_id, input.metric_id}, input.identity, state);
        ++state.revision;
        if (impl_->config.config.horizon_buckets > 0) {
            auto task = std::static_pointer_cast<IBaselineRelationTask>(module.task);
            for (const auto& routed : relation.routed_results) {
                RelationRoutedSummaryQuery query{Hex(input.identity), routed.metric, routed.summary,
                                                 routed.feature_type, routed.basis_version};
                for (uint32_t i = 0; i < impl_->config.config.horizon_buckets; ++i) {
                    auto point = task->PredictRoutedSummary(query, input.bucket + 1 + static_cast<int64_t>(i));
                    auto row = ForecastRow(input, point, impl_->Basis(input, state), routed.summary,
                                           routed.basis_scoped ? std::to_string(routed.basis_version) : "");
                    row.input.metric_id = routed.metric;
                    row.unit = routed.feature_type;
                    if (!ValidateResult(row.values).ok()) return impl_->Fail("invalid relation forecast result");
                    next.forecasts.push_back(std::move(row));
                }
            }
        }
        *output = std::move(next);
        return 0;
    }
    RollingBaselineResult result;
    if (!impl_->FitsRows(input, basis, uint64_t(impl_->config.config.horizon_buckets) + 1))
        return impl_->Fail("scalar evaluation/forecast budget exceeded");
    if (input.kind == BaselineTaskKind::kValue)
        result = std::static_pointer_cast<IBaselineValueTask>(module.task)
                     ->SubmitObservation(
                         {Hex(input.identity), input.bucket, *input.value, input.sample_count.value_or(1)}, {});
    else
        result = std::static_pointer_cast<IBaselineRatioTask>(module.task)
                     ->SubmitObservation({Hex(input.identity), input.bucket, *input.numerator, *input.denominator}, {});
    if (result.status != BaselineStatus::kOk && result.status != BaselineStatus::kInsufficientData &&
        result.status != BaselineStatus::kNotTrained)
        return impl_->Fail("algorithm Submit failed: " + std::to_string(static_cast<int>(result.status)));
    auto row = Row(input, result, basis);
    if (!ValidateResult(row.values).ok()) return impl_->Fail("invalid algorithm evaluation result");
    state.last = input.bucket;
    impl_->Touch({input.dataset_id, input.metric_id}, input.identity, state);
    ++state.revision;
    next.results.push_back(std::move(row));
    if (impl_->config.config.horizon_buckets > 0) {
        RollingPredictionSequence predictions;
        if (input.kind == BaselineTaskKind::kValue)
            predictions =
                std::static_pointer_cast<IBaselineValueTask>(module.task)
                    ->PredictRolling(Hex(input.identity), input.bucket + 1, impl_->config.config.horizon_buckets);
        else
            predictions =
                std::static_pointer_cast<IBaselineRatioTask>(module.task)
                    ->PredictRolling(Hex(input.identity), input.bucket + 1, impl_->config.config.horizon_buckets);
        for (uint32_t i = 0; i < impl_->config.config.horizon_buckets; ++i) {
            RollingPrediction point;
            point.bucket_id = input.bucket + 1 + static_cast<int64_t>(i);
            if (predictions.status == BaselineStatus::kOk && i < predictions.predictions.size())
                point = predictions.predictions[i];
            else
                point.status =
                    predictions.status == BaselineStatus::kOk ? BaselineStatus::kNotTrained : predictions.status;
            auto forecast = ForecastRow(input, point, impl_->Basis(input, state));
            if (!ValidateResult(forecast.values).ok()) return impl_->Fail("invalid scalar forecast result");
            next.forecasts.push_back(std::move(forecast));
        }
    }
    *output = std::move(next);
    return 0;
}
int EvaluationEngine::ValidateBatch(const std::vector<Observation>& rows) {
    if (!impl_->opened || impl_->finished) return impl_->Fail("evaluation engine closed");
    std::map<std::tuple<std::string, std::string, std::string>, std::pair<std::string, int64_t>> seen;
    uint64_t new_identities = 0;
    uint64_t incoming_bytes = 0;
    for (const auto& input : rows) {
        auto invalid = impl_->Invalid(input);
        if (!invalid.empty()) return impl_->Fail(std::move(invalid));
        const auto& policy = impl_->config.config.bootstrap;
        if (policy.history && input.bucket < policy.begin_bucket) continue;
        auto key = std::make_tuple(input.dataset_id, input.metric_id, input.identity);
        auto [it, added] = seen.emplace(key, std::make_pair(input.source_epoch, input.bucket));
        if (!added && (it->second.first != input.source_epoch || input.bucket <= it->second.second))
            return impl_->Fail("duplicate/late observation within batch");
        it->second.second = input.bucket;
        if (added && !impl_->modules.at({input.dataset_id, input.metric_id})->series.count(input.identity)) {
            if (impl_->CheckAdmission(input) != 0) return -1;
            ++new_identities;
        }
        incoming_bytes += sizeof(Observation) + input.identity.size() + input.source_epoch.size();
    }
    if (new_identities > impl_->config.config.state.limits.max_runtime_identities - impl_->identities) {
        if (!impl_->config.config.state.evict_idle) return impl_->Fail("evaluation identity capacity exceeded");
        uint64_t candidates = 0, bytes = incoming_bytes;
        uint32_t visited = 0;
        for (const auto& key : impl_->activity) {
            if (impl_->monotonic_ns - std::get<0>(key) < impl_->Timeout()) break;
            if (++visited > impl_->config.config.state.maintenance_max_releases) break;
            bytes +=
                sizeof(Impl::ActivityKey) + std::get<1>(key).size() + std::get<2>(key).size() + std::get<3>(key).size();
            if (bytes > impl_->config.config.state.maintenance_max_bytes) break;
            auto identity = std::make_tuple(std::get<1>(key), std::get<2>(key), std::get<3>(key));
            if (seen.count(identity)) continue;  // Incoming live identities are not eviction candidates.
            const auto& module = *impl_->modules.at({std::get<1>(key), std::get<2>(key)});
            bytes += impl_->ReleaseWork(module, std::get<3>(key), module.series.at(std::get<3>(key)));
            if (bytes > impl_->config.config.state.maintenance_max_bytes) break;
            ++candidates;
        }
        if (new_identities > impl_->config.config.state.limits.max_runtime_identities - impl_->identities + candidates)
            return impl_->Fail("capacity has no expired candidate within maintenance budget");
    }
    return 0;
}
int EvaluationEngine::Finish() {
    if (!impl_->opened || impl_->finished) return impl_->Fail("evaluation engine already finished/closed");
    for (auto& module : impl_->modules)
        for (auto& series : module.second->series)
            if (impl_->Warmup(*module.second, series.first, series.second) != 0) return -1;
    impl_->finished = true;
    return 0;
}
int EvaluationEngine::ExportModelParameters(std::vector<ModelParametersRow>* output) const {
    auto& p = *impl_;
    if (!output || !p.opened || !p.finished) return p.Fail("final models require normal EOF");
    std::vector<ModelParametersRow> rows;
    uint64_t bytes = 0, serialized = 0;
    for (const auto& module : p.modules) {
        auto* exporter = dynamic_cast<IBaselineModelParametersV1*>(module.second->task.get());
        if (!exporter) return p.Fail("model parameter capability unavailable");
        for (const auto& item : module.second->series) {
            ModelParametersRow row;
            row.identity.dataset_id = module.first.first;
            row.identity.metric_id = module.first.second;
            row.identity.identity = item.first;
            row.identity.source_epoch = item.second.epoch;
            row.identity.kind = module.second->metric.kind;
            row.as_of_bucket = item.second.last;
            row.model_basis_id = p.Basis(row.identity, item.second);
            const uint64_t overhead = sizeof(row) + row.identity.dataset_id.size() + row.identity.metric_id.size() +
                                      row.identity.identity.size() + row.identity.source_epoch.size() +
                                      row.model_basis_id.size();
            if (overhead > p.config.config.read.max_pending_bytes - bytes)
                return p.Fail("model output buffer budget exceeded");
            if (serialized >= p.config.config.persistence.max_checkpoint_bytes)
                return p.Fail("model serialization budget exceeded");
            const auto status =
                exporter->ExportModelParameters(Hex(item.first),
                                                std::min(p.config.config.persistence.max_checkpoint_bytes - serialized,
                                                         p.config.config.read.max_pending_bytes - bytes - overhead),
                                                &row.parameters);
            if (status != BaselineStatus::kOk) return p.Fail("model parameter serialization failed");
            serialized += row.parameters.parameters_json.size();
            bytes += overhead + row.parameters.parameters_json.size() + row.parameters.maturity.size();
            if (bytes > p.config.config.read.max_pending_bytes) return p.Fail("model output buffer budget exceeded");
            rows.push_back(std::move(row));
        }
    }
    *output = std::move(rows);
    return 0;
}
BaselineSerializationResult EvaluationEngine::QuerySeriesSnapshot(std::string_view dataset, std::string_view metric,
                                                                  std::string_view identity) const {
    auto found = impl_->modules.find({std::string(dataset), std::string(metric)});
    if (!impl_->opened || found == impl_->modules.end()) return {BaselineStatus::kInvalidArgument, {}};
    return found->second->task->QuerySeriesSnapshot(Hex(identity), BaselineSerializationFormat::kJson);
}
int EvaluationEngine::ExportCheckpoint(std::string* output) const {
    if (!output) return impl_->Fail("checkpoint output required");
    output->clear();
    if (!impl_->opened) return impl_->Fail("engine checkpoint unavailable");
    const uint64_t limit = impl_->Timeout() ? std::min(impl_->config.config.persistence.max_checkpoint_bytes,
                                                       impl_->config.config.state.maintenance_max_bytes)
                                            : impl_->config.config.persistence.max_checkpoint_bytes;
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> w(buffer);
    auto text = [&](const std::string& value) { w.String(value.data(), value.size()); };
    w.StartObject();
    w.Key("version");
    w.Uint(2);
    w.Key("activity_wall_ns");
    w.Int64(impl_->Timeout() ? impl_->wall_ns : 0);
    w.Key("config_hash");
    text(impl_->config.sha256_hex);
    w.Key("compatibility_hash");
    text(RestoreCompatibilityHash(impl_->config));
    w.Key("identities");
    w.Uint64(impl_->identities);
    w.Key("models");
    w.Uint64(impl_->models);
    w.Key("history_bytes");
    w.Uint64(impl_->history_bytes);
    w.Key("modules");
    w.StartArray();
    uint64_t size = 0;
    for (const auto& entry : impl_->modules) {
        const auto& m = *entry.second;
        if (!m.checkpoint) return impl_->Fail("checkpoint service not bound");
        auto payload = m.checkpoint->ExportCheckpoint(BaselineSerializationFormat::kJson);
        if (payload.first != BaselineStatus::kOk) return impl_->Fail("algorithm checkpoint export failed");
        size += payload.second.size();
        if (size > limit) return impl_->Fail("combined checkpoint byte limit");
        w.StartObject();
        w.Key("dataset");
        text(entry.first.first);
        w.Key("metric");
        text(entry.first.second);
        w.Key("model");
        text(payload.second);
        w.Key("parents");
        w.StartArray();
        for (const auto& parent : m.parents) text(Hex(parent));
        w.EndArray();
        w.Key("released_epoch");
        text(m.released_epoch);
        w.Key("series");
        w.StartArray();
        for (const auto& entry : m.series) {
            const auto& state = entry.second;
            w.StartObject();
            w.Key("identity");
            text(Hex(entry.first));
            w.Key("epoch");
            text(state.epoch);
            w.Key("last");
            if (state.last)
                w.Int64(*state.last);
            else
                w.Null();
            w.Key("initialized");
            w.Bool(state.initialized);
            w.Key("bootstrapped");
            w.Bool(state.bootstrapped);
            w.Key("revision");
            w.Uint64(state.revision);
            w.Key("bytes");
            w.Uint64(state.bytes);
            w.Key("activity_age_ns");
            w.Int64(impl_->Timeout() ? std::max<int64_t>(0, impl_->monotonic_ns - state.active_ns) : 0);
            std::string history;
            if (!state.history.empty()) {
                std::shared_ptr<arrow::RecordBatch> batch;
                std::string error;
                if (MakeObservationBatch(state.history, impl_->config.config.read.max_pending_bytes, &batch, &error) !=
                    0)
                    return impl_->Fail(error);
                auto encoded = arrow::ipc::SerializeRecordBatch(*batch, arrow::ipc::IpcWriteOptions::Defaults());
                if (!encoded.ok()) return impl_->Fail(encoded.status().ToString());
                history = Hex(std::string_view(reinterpret_cast<const char*>((*encoded)->data()), (*encoded)->size()));
            }
            w.Key("history");
            text(history);
            w.EndObject();
            if (buffer.GetSize() > limit) return impl_->Fail("engine metadata checkpoint byte limit");
        }
        w.EndArray();
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    if (buffer.GetSize() > limit) return impl_->Fail("combined checkpoint byte limit");
    *output = {buffer.GetString(), buffer.GetSize()};
    return 0;
}
int EvaluationEngine::ClassifyReplay(const Observation& input) const {
    auto found = impl_->replay.find({input.dataset_id, input.metric_id, input.identity});
    if (found == impl_->replay.end()) {
        auto module = impl_->modules.find({input.dataset_id, input.metric_id});
        if (module == impl_->modules.end() || module->second->released_epoch.empty()) return 0;
        auto position = std::find_if(impl_->progress.datasets.begin(), impl_->progress.datasets.end(),
                                     [&](const auto& p) { return p.dataset_id == input.dataset_id; });
        if (position == impl_->progress.datasets.end()) return 0;
        if (position->epoch != input.source_epoch) return impl_->Fail("source progress epoch mismatch");
        return input.bucket < position->closed_before_bucket ? 1 : 0;
    }
    if (found->second.first != input.source_epoch) return impl_->Fail("restored source epoch mismatch");
    return input.bucket <= found->second.second ? 1 : 0;
}
int EvaluationEngine::RestoreCheckpoint(std::string_view checkpoint) {
    if (!impl_->opened || impl_->restored || impl_->identities || !impl_->checkpoint_service || checkpoint.empty() ||
        checkpoint.size() > impl_->config.config.persistence.max_checkpoint_bytes)
        return impl_->Fail("engine restore requires new empty checkpoint-bound task");
    bool rebuilding = false;
    try {
        auto require = [](bool valid) {
            if (!valid) throw std::runtime_error("invalid engine checkpoint");
        };
        auto unhex = [&](const std::string& value) {
            require(value.size() % 2 == 0);
            std::string out;
            out.reserve(value.size() / 2);
            for (size_t i = 0; i < value.size(); i += 2) {
                auto digit = [](char c) {
                    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
                };
                int a = digit(value[i]), b = digit(value[i + 1]);
                require(a >= 0 && b >= 0);
                out += static_cast<char>((a << 4) | b);
            }
            return out;
        };
        auto text = [&](const rapidjson::Value& object, const char* key) {
            require(object.IsObject() && object.HasMember(key) && object[key].IsString());
            return std::string(object[key].GetString(), object[key].GetStringLength());
        };
        auto exact = [&](const rapidjson::Value& object, const std::set<std::string>& keys) {
            require(object.IsObject() && object.MemberCount() == keys.size());
            std::set<std::string> seen;
            for (auto i = object.MemberBegin(); i != object.MemberEnd(); ++i) {
                std::string key(i->name.GetString(), i->name.GetStringLength());
                require(keys.count(key) && seen.insert(key).second);
            }
        };
        rapidjson::Document doc;
        doc.Parse<rapidjson::kParseFullPrecisionFlag>(checkpoint.data(), checkpoint.size());
        require(!doc.HasParseError());
        require(doc.IsObject() && doc.HasMember("version") && doc["version"].IsUint());
        const bool legacy = doc["version"].GetUint() == 1;
        auto root_keys = std::set<std::string>{
            "version", "config_hash", "compatibility_hash", "identities", "models", "history_bytes", "modules"};
        if (!legacy) root_keys.insert("activity_wall_ns");
        exact(doc, root_keys);
        require((legacy || doc["version"].GetUint() == 2) &&
                text(doc, "compatibility_hash") == RestoreCompatibilityHash(impl_->config) && doc["modules"].IsArray());
        require(doc["identities"].IsUint64() && doc["models"].IsUint64() && doc["history_bytes"].IsUint64());
        EvaluationEngine prepared;
        prepared.impl_->config = impl_->config;
        prepared.impl_->monotonic_ns = impl_->monotonic_ns;
        prepared.impl_->wall_ns = impl_->wall_ns;
        prepared.impl_->progress = impl_->progress;
        const bool age_valid = !legacy && doc["activity_wall_ns"].IsInt64() && doc["activity_wall_ns"].GetInt64() > 0 &&
                               impl_->wall_ns >= doc["activity_wall_ns"].GetInt64();
        const int64_t downtime = age_valid ? impl_->wall_ns - doc["activity_wall_ns"].GetInt64() : 0;
        for (const auto& entry : impl_->modules) {
            auto module = std::make_unique<Impl::Module>();
            module->metric = entry.second->metric;
            prepared.impl_->modules.emplace(entry.first, std::move(module));
        }
        std::map<std::pair<std::string, std::string>, std::string> payloads;
        require(doc["modules"].Size() == prepared.impl_->modules.size());
        std::set<std::pair<std::string, std::string>> loaded;
        for (const auto& entry : doc["modules"].GetArray()) {
            auto module_keys = std::set<std::string>{"dataset", "metric", "model", "series"};
            if (!legacy) {
                module_keys.insert("parents");
                module_keys.insert("released_epoch");
            }
            exact(entry, module_keys);
            auto key = std::make_pair(text(entry, "dataset"), text(entry, "metric"));
            auto found = prepared.impl_->modules.find(key);
            require(found != prepared.impl_->modules.end() && loaded.insert(key).second && entry["series"].IsArray());
            auto& module = *found->second;
            if (!legacy) {
                require(entry["parents"].IsArray());
                for (const auto& parent : entry["parents"].GetArray()) {
                    require(parent.IsString());
                    auto identity = unhex({parent.GetString(), parent.GetStringLength()});
                    require(!identity.empty() && identity.size() <= kMaxConfigBytes &&
                            module.parents.insert(identity).second);
                }
                module.released_epoch = text(entry, "released_epoch");
                require(module.released_epoch.size() <= kMaxConfigBytes);
                prepared.impl_->models += module.parents.size();
                require(prepared.impl_->models <= prepared.impl_->config.config.state.limits.max_model_identities);
            }
            for (const auto& item : entry["series"].GetArray()) {
                auto series_keys = std::set<std::string>{"identity",     "epoch",    "last",  "initialized",
                                                         "bootstrapped", "revision", "bytes", "history"};
                if (!legacy) series_keys.insert("activity_age_ns");
                exact(item, series_keys);
                auto identity = unhex(text(item, "identity"));
                auto epoch = text(item, "epoch");
                require(!identity.empty() && !epoch.empty() && identity.size() <= kMaxConfigBytes &&
                        epoch.size() <= kMaxConfigBytes && item["last"].IsInt64() && item["initialized"].IsBool() &&
                        item["bootstrapped"].IsBool() && item["revision"].IsUint64() && item["bytes"].IsUint64());
                Impl::Series state;
                state.epoch = epoch;
                state.last = item["last"].GetInt64();
                state.initialized = item["initialized"].GetBool();
                state.bootstrapped = item["bootstrapped"].GetBool();
                state.revision = item["revision"].GetUint64();
                state.bytes = item["bytes"].GetUint64();
                if (impl_->Timeout()) {
                    int64_t age = 0;
                    if (!legacy) require(item["activity_age_ns"].IsInt64() && item["activity_age_ns"].GetInt64() >= 0);
                    if (age_valid) {
                        age = std::min(item["activity_age_ns"].GetInt64(), impl_->Timeout());
                        age += std::min(downtime, impl_->Timeout() - age);
                    } else {
                        Observation o;
                        o.dataset_id = key.first;
                        o.metric_id = key.second;
                        o.identity = identity;
                        o.source_epoch = epoch;
                        o.bucket = *state.last;
                        prepared.impl_->maintenance.push_back(
                            {o, "deadline_reset", "wall_clock_rollback_or_unverifiable_age", {}});
                    }
                    state.active_ns = impl_->monotonic_ns - age;
                    prepared.impl_->activity.emplace(state.active_ns, key.first, key.second, identity);
                }
                auto history = unhex(text(item, "history"));
                if (!history.empty()) {
                    arrow::io::BufferReader reader(
                        arrow::Buffer::Wrap(reinterpret_cast<const uint8_t*>(history.data()), history.size()));
                    arrow::ipc::DictionaryMemo memo;
                    auto batch = arrow::ipc::ReadRecordBatch(MakeSchema(SchemaKind::kObservation), &memo,
                                                             arrow::ipc::IpcReadOptions::Defaults(), &reader);
                    require(batch.ok());
                    std::string error;
                    require(DecodeObservations(*batch, prepared.impl_->config.config.read.max_pending_bytes,
                                               &state.history, &error) == 0);
                }
                require(!state.bootstrapped || state.initialized);
                require(!state.initialized || (state.history.empty() && state.bytes == 0));
                if (!state.initialized)
                    require(prepared.impl_->config.config.bootstrap.history && !state.history.empty());
                uint64_t bytes = 0;
                std::optional<int64_t> last;
                for (const auto& observation : state.history) {
                    require(observation.dataset_id == key.first && observation.metric_id == key.second &&
                            observation.identity == identity && observation.source_epoch == epoch &&
                            (!last || observation.bucket > *last));
                    require(observation.bucket >= prepared.impl_->config.config.bootstrap.begin_bucket &&
                            observation.bucket < prepared.impl_->config.config.bootstrap.end_bucket &&
                            prepared.impl_->Invalid(observation).empty());
                    bytes += sizeof(Observation) + observation.dataset_id.size() + observation.metric_id.size() +
                             observation.identity.size() + observation.source_epoch.size() +
                             observation.groups.size() * sizeof(uint32_t);
                    for (const auto& metric : observation.metrics)
                        bytes += sizeof(RelationValues) + metric.metric.size() +
                                 metric.values_by_group.size() * sizeof(double);
                    last = observation.bucket;
                }
                require(bytes == state.bytes && (!last || last == state.last));
                prepared.impl_->history_bytes += bytes;
                prepared.impl_->identities++;
                if (state.bootstrapped) {
                    if (legacy) {
                        prepared.impl_->models++;
                        module.parents.insert(identity);
                    } else
                        require(module.parents.count(identity));
                }
                require(prepared.impl_->history_bytes <= prepared.impl_->config.config.read.max_pending_bytes &&
                        prepared.impl_->identities <=
                            prepared.impl_->config.config.state.limits.max_runtime_identities &&
                        prepared.impl_->models <= prepared.impl_->config.config.state.limits.max_model_identities);
                require(module.series.emplace(identity, std::move(state)).second);
                prepared.impl_->replay.emplace(std::make_tuple(key.first, key.second, identity),
                                               std::make_pair(epoch, item["last"].GetInt64()));
            }
            auto payload = text(entry, "model");
            payloads.emplace(key, std::move(payload));
        }
        require(prepared.impl_->identities == doc["identities"].GetUint64() &&
                prepared.impl_->models == doc["models"].GetUint64() &&
                prepared.impl_->history_bytes == doc["history_bytes"].GetUint64());
        // Service registration forbids duplicate task IDs. Only empty internal tasks are retired;
        // failure recreates empty handles and restores eligibility before returning.
        impl_->modules.clear();
        rebuilding = true;
        EvaluationEngine runnable;
        if (runnable.Open(impl_->config, impl_->service, impl_->state_service, impl_->checkpoint_service) != 0)
            throw std::runtime_error(runnable.LastError());
        for (auto& entry : runnable.impl_->modules) {
            BaselineCheckpointRestoreV1 restore;
            restore.content = payloads.at(entry.first);
            require(entry.second->checkpoint->RestoreCheckpoint(restore) == BaselineStatus::kOk);
            entry.second->series = std::move(prepared.impl_->modules.at(entry.first)->series);
            entry.second->parents = std::move(prepared.impl_->modules.at(entry.first)->parents);
            entry.second->released_epoch = std::move(prepared.impl_->modules.at(entry.first)->released_epoch);
            auto usage = entry.second->control->QueryUsage();
            require(usage.first == BaselineStatus::kOk &&
                    usage.second.model_identities == entry.second->parents.size());
        }
        runnable.impl_->identities = prepared.impl_->identities;
        runnable.impl_->models = prepared.impl_->models;
        runnable.impl_->history_bytes = prepared.impl_->history_bytes;
        runnable.impl_->replay = std::move(prepared.impl_->replay);
        runnable.impl_->activity = std::move(prepared.impl_->activity);
        runnable.impl_->maintenance = std::move(prepared.impl_->maintenance);
        runnable.impl_->monotonic_ns = impl_->monotonic_ns;
        runnable.impl_->wall_ns = impl_->wall_ns;
        runnable.impl_->progress = impl_->progress;
        runnable.impl_->restored = true;
        impl_.swap(runnable.impl_);
        rebuilding = false;
        return 0;
    } catch (const std::exception& error) {
        const std::string message = error.what();
        if (rebuilding) {
            EvaluationEngine empty;
            if (empty.Open(impl_->config, impl_->service, impl_->state_service, impl_->checkpoint_service) == 0)
                impl_.swap(empty.impl_);
            else
                return impl_->Fail("failed to recreate empty restore target: " + empty.LastError());
        }
        return impl_->Fail(message);
    }
}
void EvaluationEngine::SetTime(int64_t monotonic_ns, int64_t wall_ns) {
    impl_->monotonic_ns = std::max(impl_->monotonic_ns, monotonic_ns);
    impl_->wall_ns = wall_ns;
}
int EvaluationEngine::SetSourceProgress(const BlockInputProgressV1& progress) {
    std::vector<std::string> ids;
    for (const auto& d : impl_->config.config.datasets) ids.push_back(d.id);
    if (!ValidateProgress(progress, ids).ok()) return impl_->Fail("invalid maintenance source progress");
    impl_->progress = progress;
    return 0;
}
std::optional<int64_t> EvaluationEngine::NextMaintenanceDeadline() const {
    if (!impl_->maintenance.empty()) return impl_->monotonic_ns;
    if (impl_->activity.empty()) return {};
    return std::get<0>(*impl_->activity.begin()) + impl_->Timeout();
}
std::pair<BaselineStatus, BaselineStateUsageV1> EvaluationEngine::QueryUsage() const {
    if (!impl_->opened) return {BaselineStatus::kInvalidArgument, {}};
    BaselineStateUsageV1 usage;
    usage.runtime_identities = impl_->identities;  // Includes bootstrap buffers, before native runtime exists.
    for (const auto& entry : impl_->modules) {
        auto native = entry.second->control->QueryUsage();
        if (native.first != BaselineStatus::kOk) return native;
        usage.model_identities += native.second.model_identities;
        usage.routed_states += native.second.routed_states;
        usage.retained_basis_versions += native.second.retained_basis_versions;
    }
    return {BaselineStatus::kOk, usage};
}
int EvaluationEngine::Maintain(std::vector<MaintenanceRow>* output) {
    if (!output || !impl_->opened || impl_->finished) return impl_->Fail("invalid maintenance lifecycle");
    const auto& policy = impl_->config.config.state;
    uint64_t work = 0, checkpoint_work = 0;
    uint32_t releases = 0;
    if (!impl_->activity.empty() && impl_->monotonic_ns - std::get<0>(*impl_->activity.begin()) >= impl_->Timeout() &&
        impl_->checkpoint_service) {
        std::string before;
        if (ExportCheckpoint(&before) != 0) return -1;
        // Reserve the preflight export, post-release export and escaped task envelope publication.
        checkpoint_work = before.size() * 4;
    }
    // Keep queued records owned and bounded until the operator encodes them into the same generation.
    if (output != &impl_->maintenance) {
        output->clear();
        uint64_t record_bytes = 0;
        auto end = impl_->maintenance.begin();
        while (end != impl_->maintenance.end() && output->size() < policy.maintenance_max_releases) {
            const auto bytes = sizeof(MaintenanceRow) + end->identity.identity.size() +
                               end->identity.source_epoch.size() + end->reason.size();
            if (bytes > policy.maintenance_max_bytes - record_bytes)
                return impl_->Fail("maintenance record byte budget exceeded");
            record_bytes += bytes;
            if (end->event_kind == "deadline_reset") {
                auto usage = QueryUsage();
                if (usage.first != BaselineStatus::kOk) return impl_->Fail("deadline reset usage query failed");
                end->usage = usage.second;
            }
            output->push_back(std::move(*end++));
        }
        impl_->maintenance.erase(impl_->maintenance.begin(), end);
        work += record_bytes;
    }
    while (!impl_->activity.empty() && releases < policy.maintenance_max_releases &&
           output->size() < policy.maintenance_max_releases) {
        const auto key = *impl_->activity.begin();
        if (impl_->monotonic_ns - std::get<0>(key) < impl_->Timeout()) break;
        auto& module = *impl_->modules.at({std::get<1>(key), std::get<2>(key)});
        auto found = module.series.find(std::get<3>(key));
        const auto cost = checkpoint_work + impl_->ReleaseWork(module, found->first, found->second);
        if (cost > policy.maintenance_max_bytes - work) {
            if (!releases) return impl_->Fail("expired identity exceeds maintenance work byte budget");
            break;
        }
        Observation o;
        o.dataset_id = std::get<1>(key);
        o.metric_id = std::get<2>(key);
        o.identity = found->first;
        o.source_epoch = found->second.epoch;
        o.bucket = found->second.last.value_or(0);
        if (module.control->ReleaseIdentity(Hex(found->first), policy.release_scope) != BaselineStatus::kOk)
            return impl_->Fail("algorithm identity release failed");
        impl_->history_bytes -= found->second.bytes;
        if (policy.release_scope == BaselineStateReleaseScopeV1::kAllState && module.parents.erase(found->first))
            --impl_->models;
        module.released_epoch = found->second.epoch;
        impl_->replay.erase({o.dataset_id, o.metric_id, o.identity});
        module.series.erase(found);
        impl_->activity.erase(impl_->activity.begin());
        --impl_->identities;
        work += cost;
        ++releases;
        auto usage = QueryUsage();
        if (usage.first != BaselineStatus::kOk) return impl_->Fail("maintenance usage query failed");
        output->push_back({std::move(o), "idle_release",
                           policy.release_scope == BaselineStateReleaseScopeV1::kAllState ? "AllState" : "RuntimeOnly",
                           usage.second});
    }
    return 0;
}
}  // namespace flowsql::baseliner
