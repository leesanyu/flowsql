// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include "checkpoint.h"
#include "plugins/baseline/task/ratio_task.h"
#include "plugins/baseline/task/value_task.h"
#include "state_codec.h"
namespace flowsql::baseline {
namespace model {
using checkpoint::Require;
using checkpoint::Writer;
template <class T>
void Write(Writer& w, const T& v);
struct FieldsWriter {
    Writer& writer;
    template <class T>
    void operator()(const char* name, const T& value) {
        writer.Key(name);
        model::Write(writer, value);
    }
};
template <class T>
void Write(Writer& w, const T& v) {
    if constexpr (std::is_same_v<T, std::string>)
        w.String(v.data(), v.size());
    else if constexpr (std::is_same_v<T, bool>)
        w.Bool(v);
    else if constexpr (std::is_enum_v<T>)
        w.Int64(static_cast<int64_t>(v));
    else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>)
        w.Int64(v);
    else if constexpr (std::is_integral_v<T>)
        w.Uint64(v);
    else if constexpr (std::is_floating_point_v<T>) {
        Require(std::isfinite(v), "nonfinite model parameter");
        w.Double(v);
    } else if constexpr (checkpoint::Vector<T>::value || checkpoint::Array<T>::value) {
        w.StartArray();
        for (const auto& item : v) model::Write(w, item);
        w.EndArray();
    } else if constexpr (checkpoint::Map<T>::value) {
        std::map<typename T::key_type, const typename T::mapped_type*> ordered;
        for (const auto& item : v) ordered.emplace(item.first, &item.second);
        w.StartArray();
        for (const auto& item : ordered) {
            w.StartArray();
            model::Write(w, item.first);
            model::Write(w, *item.second);
            w.EndArray();
        }
        w.EndArray();
    } else if constexpr (checkpoint::Optional<T>::value) {
        if (v)
            model::Write(w, *v);
        else
            w.Null();
    } else {
        w.StartObject();
        FieldsWriter a{w};
        checkpoint::Fields<T>::Visit(a, v);
        w.EndObject();
    }
}
void Rolling(Writer& writer, const BaselineTaskSpec& spec, const RollingState& state, const BootstrapSeed* seed) {
    writer.StartObject();
    FieldsWriter f{writer};
    f("model_space",
      std::string(spec.feature_type == "ratio" ? "logit" : (spec.value_identity_transform ? "identity" : "log1p")));
    f("reference_bucket", state.last_seen_bucket);
    f("theta", state.theta);
    f("sigma", state.sigma);
    f("sigma_init", state.sigma_init);
    f("p_level", state.p_level);
    f("p_level_trend", state.p_level_trend);
    f("p_trend", state.p_trend);
    f("maturity", std::string(RollingMaturityStatusName(state.maturity_status)));
    f("monthpos_status", state.monthpos_status);
    f("monthpos_dom_coeff", state.monthpos_dom_coeff);
    f("monthpos_dom_center", state.monthpos_dom_center);
    f("monthpos_dme_coeff", state.monthpos_dme_coeff);
    f("monthpos_dme_center", state.monthpos_dme_center);
    f("monthpos_lwd_coeff", state.monthpos_lwd_coeff);
    f("monthpos_lwd_center", state.monthpos_lwd_center);
    f("detection_band_multiplier", state.detection_band_multiplier);
    f("effective_confidence", state.effective_confidence);
    f("learning_confidence", state.learning_confidence);
    f("has_seen_observation", state.has_seen_observation);
    BaselineRollingConfig effective;
    Require(ResolveBaselineRollingConfig(spec, &effective) == BaselineStatus::kOk, "rolling config unavailable");
    f("effective_rolling", effective);
    // Forecast fusion and event effects consume these learned seed components in addition to theta.
    if (seed) {
        writer.Key("bootstrap_components");
        writer.StartObject();
        f("theta", seed->theta_init);
        f("sigma", seed->sigma_init);
        f("ratio_prior", seed->ratio_prior_init);
        f("uncertainty", seed->uncertainty_init);
        f("monthpos", seed->monthpos_hint);
        f("event", seed->event_hint);
        writer.EndObject();
    }
    writer.EndObject();
}
bool Trained(const RollingState& state, const BootstrapSeed* seed) {
    return state.accepted_update_count > 1 || (seed && seed->theta_init.available);
}
}  // namespace model

template <class T>
BaselineModelParametersV1 CheckpointAccess::ScalarParameters(const T& task, const std::string& key, uint64_t limit) {
    BaselineModelParametersV1 result;
    const auto found = task.rolling_states_.find(key);
    if (found == task.rolling_states_.end()) return result;
    const auto& state = found->second;
    result.maturity = RollingMaturityStatusName(state.maturity_status);
    const auto seed_it = task.seeds_by_series_.find(key);
    const auto* seed = seed_it == task.seeds_by_series_.end() ? nullptr : &seed_it->second;
    if (!model::Trained(state, seed)) return result;
    checkpoint::BoundedStream stream(limit);
    checkpoint::Writer writer(stream);
    writer.StartObject();
    model::FieldsWriter f{writer};
    f("parameters_version", uint32_t{1});
    f("algorithm_version", std::string("baseline.native.v1"));
    f("feature_type", task.spec_.feature_type);
    writer.Key("rolling");
    model::Rolling(writer, task.spec_, state, seed);
    if (task.compiled_event_calendar_) f("event_calendar", *task.compiled_event_calendar_);
    writer.EndObject();
    result.status = BaselineStatus::kOk;
    result.parameters_json = std::move(stream.content);
    return result;
}
BaselineModelParametersV1 CheckpointAccess::RelationParameters(const BaselineRelationTask& task, const std::string& key,
                                                               uint64_t limit) {
    BaselineModelParametersV1 result;
    checkpoint::BoundedStream stream(limit);
    checkpoint::Writer w(stream);
    model::FieldsWriter f{w};
    w.StartObject();
    f("parameters_version", uint32_t{1});
    f("algorithm_version", std::string("baseline.native.v1"));
    f("source_series_key", key);
    bool trained = false;
    w.Key("basis");
    w.StartArray();
    // Explicit metric identity, basis version and active support/prototypes; no accumulator/checkpoint.
    for (const auto& metric : task.spec_.task_spec.metrics) {
        const auto it = task.basis_states_.find(key + "::" + metric);
        w.StartObject();
        f("metric", metric);
        if (it != task.basis_states_.end()) {
            f("status", it->second.basis_status());
            if (const auto* basis = it->second.active_basis()) {
                f("active", *basis);
                trained = true;
            }
        }
        w.EndObject();
    }
    w.EndArray();
    w.Key("routed");
    w.StartArray();
    // Sort references only; do not copy all runtime state or synthesize observations.
    std::map<std::string, const BaselineRelationTask::RelationRoutedRuntimeShard*> ordered;
    for (const auto& shard : task.routed_shards_)
        for (const auto& entry : shard->routed_specs_by_series)
            if (entry.first.compare(0, key.size() + 2, key + "::") == 0) ordered.emplace(entry.first, shard.get());
    for (const auto& entry : ordered) {
        const auto& shard = *entry.second;
        const auto state = shard.routed_rolling_states.find(entry.first);
        const auto seed_it = shard.routed_seeds_by_series.find(entry.first);
        const auto* seed = seed_it == shard.routed_seeds_by_series.end() ? nullptr : &seed_it->second;
        w.StartObject();
        f("routed_series_key", entry.first);
        const auto& spec = shard.routed_specs_by_series.at(entry.first);
        f("feature_type", spec.feature_type);
        f("feature_id", spec.feature_id);
        const auto metric_begin = key.size() + 2;
        const auto metric_end = entry.first.find("::", metric_begin);
        const auto summary_end = entry.first.find("::", metric_end + 2);
        f("metric", entry.first.substr(metric_begin, metric_end - metric_begin));
        f("summary", entry.first.substr(metric_end + 2, summary_end - metric_end - 2));
        const auto version_at = entry.first.rfind("::basis:");
        f("basis_scoped", version_at != std::string::npos);
        f("basis_version",
          version_at == std::string::npos ? uint64_t{0} : std::stoull(entry.first.substr(version_at + 8)));
        f("parameters_version", uint32_t{1});
        if (state != shard.routed_rolling_states.end()) {
            f("maturity", std::string(RollingMaturityStatusName(state->second.maturity_status)));
            w.Key("parameters");
            if (model::Trained(state->second, seed)) {
                model::Rolling(w, spec, state->second, seed);
                trained = true;
            } else
                w.Null();
        }
        w.EndObject();
    }
    w.EndArray();
    if (task.compiled_event_calendar_) f("event_calendar", *task.compiled_event_calendar_);
    w.EndObject();
    result.maturity = trained ? "relation_active" : "cold_learning";
    if (trained) {
        result.status = BaselineStatus::kOk;
        result.parameters_json = std::move(stream.content);
    }
    return result;
}
BaselineStatus CheckpointAccess::ModelParameters(const BaselineTaskBase& task, std::string_view key, uint64_t limit,
                                                 BaselineModelParametersV1* output) {
    if (!output || key.empty() || !limit || limit > kBaselineCheckpointMaxBytesV1 || task.closed_)
        return BaselineStatus::kInvalidArgument;
    try {
        BaselineModelParametersV1 result;
        if (auto* t = dynamic_cast<const BaselineValueTask*>(&task))
            result = ScalarParameters(*t, std::string(key), limit);
        else if (auto* t = dynamic_cast<const BaselineRatioTask*>(&task))
            result = ScalarParameters(*t, std::string(key), limit);
        else if (auto* t = dynamic_cast<const BaselineRelationTask*>(&task))
            result = RelationParameters(*t, std::string(key), limit);
        else
            return BaselineStatus::kInvalidArgument;
        *output = std::move(result);
        return BaselineStatus::kOk;
    } catch (const std::exception&) {
        return BaselineStatus::kSerializationFailed;
    }
}
BaselineStatus BaselineTaskBase::ExportModelParameters(std::string_view key, uint64_t limit,
                                                       BaselineModelParametersV1* output) const {
    return CheckpointAccess::ModelParameters(*this, key, limit, output);
}
}  // namespace flowsql::baseline
