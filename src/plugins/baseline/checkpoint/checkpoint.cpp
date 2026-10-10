// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include "checkpoint.h"
#include <openssl/sha.h>
#include <unordered_set>
#include "plugins/baseline/config/runtime_config.h"
#include "plugins/baseline/relation/routed_summary.h"
#include "plugins/baseline/task/ratio_task.h"
#include "plugins/baseline/task/value_task.h"
#include "state_codec.h"
namespace flowsql::baseline {
namespace checkpoint {
struct Compatibility {
    BaselineTaskSpec spec;
    BaselineRollingConfig rolling;
    SharedProfileConfig shared;
    ValueSampledProfileConfig sampled;
    RatioProfileConfig ratio;
    BlockSolverConfig solver;
    BootstrapSeedQualityConfig quality;
    std::optional<CompiledEventCalendar> calendar;
};
template <>
struct Fields<Compatibility> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("spec", v.spec);
        a("rolling", v.rolling);
        a("shared", v.shared);
        a("sampled", v.sampled);
        a("ratio", v.ratio);
        a("solver", v.solver);
        a("quality", v.quality);
        a("calendar", v.calendar);
    }
};
struct ScalarState {
    BaselineTaskKind kind;
    Compatibility compatibility;
    BootstrapArtifactStore artifacts;
    BootstrapSeedStore seeds;
    std::unordered_map<std::string, RollingState> rolling;
};
template <>
struct Fields<ScalarState> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("kind", v.kind);
        a("compatibility", v.compatibility);
        a("artifacts", v.artifacts);
        a("seeds", v.seeds);
        a("rolling", v.rolling);
    }
};
Compatibility Capture(const BaselineTaskSpec& spec, const std::shared_ptr<const CompiledEventCalendar>& calendar) {
    Compatibility result;
    result.spec = spec;
    Require(ResolveBaselineRollingConfig(spec, &result.rolling) == BaselineStatus::kOk,
            "effective rolling config unavailable");
    result.shared = DefaultSharedProfileConfig();
    result.solver = DefaultBlockSolverConfig();
    (void)TryGetBootstrapSeedQualityConfigOverride(&result.quality);
    if (spec.feature_type == "value_sampled")
        Require(GetValueSampledProfileConfig(spec.profile, &result.sampled), "sampled profile unavailable");
    if (spec.feature_type == "ratio")
        Require(GetRatioProfileConfig(spec.profile, &result.ratio), "ratio profile unavailable");
    if (calendar) result.calendar = *calendar;
    return result;
}
std::string Digest(std::string_view text) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(text.data()), text.size(), digest);
    return Hex({reinterpret_cast<const char*>(digest), sizeof(digest)});
}
std::string Envelope(std::string_view payload, uint64_t max_bytes) {
    BoundedStream stream(max_bytes);
    Writer writer(stream);
    writer.StartObject();
    writer.Key("payload_version");
    writer.Uint(kBaselineCheckpointPayloadVersionV1);
    writer.Key("schema_version");
    writer.Uint(1);
    writer.Key("algorithm_version");
    writer.String("baseline.native.v1");
    writer.Key("payload_bytes");
    writer.Uint64(payload.size());
    writer.Key("sha256");
    const auto digest = Digest(payload);
    writer.String(digest.data(), digest.size());
    writer.Key("payload");
    writer.RawValue(payload.data(), payload.size(), rapidjson::kObjectType);
    writer.EndObject();
    return std::move(stream.content);
}
rapidjson::Document DecodeEnvelope(std::string_view content, uint64_t max_bytes) {
    auto doc = Parse(content);
    Require(doc.IsObject() && doc.MemberCount() == 6, "checkpoint envelope fields");
    for (auto it = doc.MemberBegin(); it != doc.MemberEnd(); ++it)
        for (auto other = doc.MemberBegin(); other != it; ++other)
            Require(it->name != other->name, "checkpoint envelope duplicate");
    for (const char* name :
         {"payload_version", "schema_version", "algorithm_version", "payload_bytes", "sha256", "payload"})
        Require(doc.HasMember(name), "checkpoint envelope field missing");
    Require(doc["payload_version"].IsUint() && doc["payload_version"].GetUint() == 1 &&
                doc["schema_version"].IsUint() && doc["schema_version"].GetUint() == 1 &&
                doc["algorithm_version"].IsString() &&
                std::string_view(doc["algorithm_version"].GetString(), doc["algorithm_version"].GetStringLength()) ==
                    "baseline.native.v1",
            "checkpoint version mismatch");
    BoundedStream stream(max_bytes);
    Writer writer(stream);
    Require(doc["payload"].IsObject() && doc["payload"].Accept(writer), "checkpoint payload required");
    Require(doc["payload_bytes"].IsUint64() && doc["payload_bytes"].GetUint64() == stream.content.size(),
            "checkpoint payload length mismatch");
    Require(doc["sha256"].IsString() &&
                std::string_view(doc["sha256"].GetString(), doc["sha256"].GetStringLength()) == Digest(stream.content),
            "checkpoint integrity mismatch");
    return doc;
}
void ValidateRolling(const std::string& key, const RollingState& state, const BaselineRollingConfig& config) {
    Require(!key.empty() && key == state.series_key, "rolling identity mismatch");
    auto harmonic = [](const RollingHarmonicState& h, size_t size) {
        Require(h.sin_coeff.size() == size && h.cos_coeff.size() == size && h.sin_p.size() == size &&
                    h.cos_p.size() == size,
                "rolling harmonic dimensions");
        for (double v : h.sin_p) Require(v >= 0, "rolling harmonic covariance");
        for (double v : h.cos_p) Require(v >= 0, "rolling harmonic covariance");
    };
    harmonic(state.theta.daily, config.daily_harmonic_order);
    harmonic(state.theta.weekly, config.weekly_harmonic_order);
    Require(state.daily_bin_count.size() == config.daily_coverage_bins &&
                state.weekly_bin_count.size() == config.weekly_coverage_bins && state.monthpos_dme_count.size() == 8 &&
                state.monthpos_dom_coeff.size() == 31 && state.monthpos_dom_center.size() == 31 &&
                state.monthpos_dme_coeff.size() == 8 && state.monthpos_dme_center.size() == 8 &&
                state.monthpos_lwd_coeff.size() == 7 && state.monthpos_lwd_center.size() == 7,
            "rolling coverage/monthpos dimensions");
    Require(state.sigma > 0 && state.sigma_init > 0 && state.p_level >= 0 && state.p_trend >= 0,
            "rolling scale/covariance");
    Require(state.last_seen_bucket <= state.last_processed_bucket, "rolling consumed cursor");
    auto range = [](auto v, int max) { return static_cast<int>(v) >= 0 && static_cast<int>(v) <= max; };
    Require(range(state.state_status, 2) && range(state.maturity_status, 7) && range(state.score_trust_status, 4) &&
                range(state.calibration_status, 4) && range(state.monthpos_status, 2) &&
                range(state.daily_prior_quality, 3) && range(state.weekly_prior_quality, 3) &&
                range(state.bootstrap_seed_status, 3),
            "rolling enum range");
}
template <class M>
void ValidateFormalModel(const M& model, FormalModelKind kind) {
    const auto& core = model.core_block;
    const auto& month = model.monthpos_block;
    const auto& event = model.event_block;
    const auto shared = DefaultSharedProfileConfig();
    Require(model.metadata.kind == kind && static_cast<int>(model.readiness) >= 0 &&
                static_cast<int>(model.readiness) <= 2 && model.delta > 0 && model.sigma_ref > 0 &&
                model.train_start <= model.train_end,
            "checkpoint formal model metadata");
    Require(core.day_sin.size() == core.day_cos.size() && core.week_sin.size() == core.week_cos.size() &&
                month.dme_coeff.size() == month.dme_center.size() &&
                core.day_sin.size() <= static_cast<size_t>(shared.k_day) &&
                core.week_sin.size() <= static_cast<size_t>(shared.k_week) &&
                (!month.enabled || month.dme_coeff.size() == static_cast<size_t>(shared.dme_max) + 1) &&
                event.active_event_codes.size() == event.coeff.size(),
            "checkpoint formal model dimensions");
    if constexpr (std::is_same_v<M, RatioFormalModel>)
        Require(
            model.transform_name == "logit" && model.m0 > 0 && model.m0 < 1 && model.alpha0 >= 0 && model.beta0 >= 0,
            "checkpoint ratio prior");
}
void ValidateScalarModels(const BootstrapArtifact& artifact) {
    if (artifact.artifact_kind == BootstrapArtifactKind::kValue) {
        Require(artifact.value_model && !artifact.ratio_model, "checkpoint value model required");
        ValidateFormalModel(*artifact.value_model, FormalModelKind::kValueBaseline);
    } else {
        Require(artifact.ratio_model && !artifact.value_model, "checkpoint ratio model required");
        ValidateFormalModel(*artifact.ratio_model, FormalModelKind::kRatioBaseline);
    }
}
void PreflightScalar(const rapidjson::Value& payload, const Compatibility& expected,
                     const BaselineStateLimitsV1& limits, BaselineTaskKind kind, uint64_t max_bytes) {
    Require(payload.IsObject() && payload.HasMember("kind") && payload.HasMember("compatibility"),
            "checkpoint scalar header");
    BaselineTaskKind decoded_kind{};
    Compatibility compatibility{};
    Read(payload["kind"], decoded_kind);
    Read(payload["compatibility"], compatibility);
    Require(decoded_kind == kind && Encode(compatibility, max_bytes) == Encode(expected, max_bytes),
            "checkpoint configuration/calendar mismatch");
    for (const auto& item : {std::make_pair("rolling", limits.max_runtime_identities),
                             std::make_pair("artifacts", limits.max_model_identities),
                             std::make_pair("seeds", limits.max_model_identities)})
        Require(
            payload.HasMember(item.first) && payload[item.first].IsArray() && payload[item.first].Size() <= item.second,
            "checkpoint identity capacity");
}
void ValidateScalar(const ScalarState& state, const Compatibility& expected, const BaselineStateLimitsV1& limits,
                    BaselineTaskKind kind, uint64_t max_bytes) {
    Require(state.kind == kind && Encode(state.compatibility, max_bytes) == Encode(expected, max_bytes),
            "checkpoint configuration/calendar mismatch");
    Require(state.rolling.size() <= limits.max_runtime_identities &&
                state.artifacts.size() <= limits.max_model_identities && state.seeds.size() == state.artifacts.size(),
            "checkpoint identity capacity");
    for (const auto& item : state.rolling) ValidateRolling(item.first, item.second, expected.rolling);
    BootstrapEngine engine;
    const auto artifact_kind =
        kind == BaselineTaskKind::kValue ? BootstrapArtifactKind::kValue : BootstrapArtifactKind::kRatio;
    for (const auto& [key, artifact] : state.artifacts) {
        Require(!key.empty() && artifact.series_key == key && artifact.artifact_kind == artifact_kind &&
                    engine.ValidateArtifactCompatibility(artifact, expected.spec, artifact_kind) == BaselineStatus::kOk,
                "checkpoint artifact compatibility");
        ValidateScalarModels(artifact);
        auto seed = state.seeds.find(key);
        Require(
            seed != state.seeds.end() && seed->second.series_key == key && seed->second.artifact_kind == artifact_kind,
            "checkpoint seed identity");
        BootstrapSeed derived;
        Require(engine.ExportSeed(artifact, &derived) == BaselineStatus::kOk &&
                    Encode(derived, max_bytes) == Encode(seed->second, max_bytes),
                "checkpoint seed/artifact mismatch");
    }
}
}  // namespace checkpoint
using namespace checkpoint;
BaselineStatus CheckpointAccess::Bind(BaselineTaskBase& task) {
    if (task.closed_ || task.state_operations_started_ || !task.state_limits_ || task.checkpoint_bound_)
        return BaselineStatus::kInvalidArgument;
    task.checkpoint_bound_ = true;
    return BaselineStatus::kOk;
}
template <class T>
BaselineSerializationResult CheckpointAccess::ExportScalar(const T& task, uint64_t max_bytes) {
    try {
        const auto compatibility = Capture(task.spec_, task.compiled_event_calendar_);
        BoundedStream stream(max_bytes);
        Writer writer(stream);
        writer.StartObject();
        WriteFields fields{writer};
        fields("kind", task.kind_);
        fields("compatibility", compatibility);
        fields("artifacts", task.artifacts_by_series_);
        fields("seeds", task.seeds_by_series_);
        fields("rolling", task.rolling_states_);
        writer.EndObject();
        return {BaselineStatus::kOk, Envelope(stream.content, max_bytes)};
    } catch (const std::exception&) {
        return {BaselineStatus::kSerializationFailed, {}};
    }
}
BaselineSerializationResult CheckpointAccess::Export(const BaselineTaskBase& task, uint64_t max_bytes) {
    if (task.closed_ || !task.checkpoint_bound_) return {BaselineStatus::kInvalidArgument, {}};
    if (auto value = dynamic_cast<const BaselineValueTask*>(&task)) return ExportScalar(*value, max_bytes);
    if (auto ratio = dynamic_cast<const BaselineRatioTask*>(&task)) return ExportScalar(*ratio, max_bytes);
    if (auto relation = dynamic_cast<const BaselineRelationTask*>(&task)) return ExportRelation(*relation, max_bytes);
    return {BaselineStatus::kUnsupportedFormat, {}};
}
template <class T>
BaselineStatus CheckpointAccess::RestoreScalar(T& task, const BaselineCheckpointRestoreV1& restore,
                                               uint64_t max_bytes) {
    try {
        auto doc = DecodeEnvelope(restore.content, max_bytes);
        const auto expected = Capture(task.spec_, task.compiled_event_calendar_);
        PreflightScalar(doc["payload"], expected, *task.state_limits_, task.kind_, max_bytes);
        ScalarState prepared{};
        Read(doc["payload"], prepared);
        ValidateScalar(prepared, expected, *task.state_limits_, task.kind_, max_bytes);
        task.artifacts_by_series_.swap(prepared.artifacts);
        task.seeds_by_series_.swap(prepared.seeds);
        task.rolling_states_.swap(prepared.rolling);
        task.checkpoint_restored_ = true;
        task.state_operations_started_ = true;
        return BaselineStatus::kOk;
    } catch (const std::exception&) {
        return BaselineStatus::kInvalidArgument;
    }
}
BaselineStatus CheckpointAccess::Restore(BaselineTaskBase& task, const BaselineCheckpointRestoreV1& restore,
                                         uint64_t max_bytes) {
    if (!ValidBaselineCheckpointRestoreV1(restore, max_bytes) || task.closed_ || !task.checkpoint_bound_ ||
        !task.state_limits_ || task.state_operations_started_ || task.checkpoint_restored_)
        return BaselineStatus::kInvalidArgument;
    if (auto value = dynamic_cast<BaselineValueTask*>(&task)) return RestoreScalar(*value, restore, max_bytes);
    if (auto ratio = dynamic_cast<BaselineRatioTask*>(&task)) return RestoreScalar(*ratio, restore, max_bytes);
    if (auto relation = dynamic_cast<BaselineRelationTask*>(&task))
        return RestoreRelation(*relation, restore, max_bytes);
    return BaselineStatus::kInvalidArgument;
}
}  // namespace flowsql::baseline

namespace flowsql::baseline {
namespace checkpoint {
template <>
struct Fields<RelationStreamBasisAccumulator> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("config", v.config_);
        a("valid_bucket_count", v.valid_bucket_count_);
        a("total_mass", v.total_mass_);
        a("first_bucket_id", v.first_bucket_id_);
        a("last_bucket_id", v.last_bucket_id_);
        a("groups", v.groups_);
    }
};
template <>
struct Fields<RelationBasisRuntimeState> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("config", v.config_);
        a("accumulator", v.accumulator_);
        a("basis_status", v.basis_status_);
        a("has_active_basis", v.has_active_basis_);
        a("active_basis", v.active_basis_);
        a("last_refresh_bucket", v.last_refresh_bucket_);
        a("handover_start_bucket", v.handover_start_bucket_);
        a("stable_refresh_counts", v.stable_refresh_counts_);
    }
};
template <>
struct Fields<CheckpointAccess::RelationShard> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("seeds", v.routed_seeds_by_series);
        a("specs", v.routed_specs_by_series);
        a("rolling", v.routed_rolling_states);
    }
};
template <>
struct Fields<BaselineRelationTask> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("artifacts", v.artifacts_by_series_);
        a("seeds", v.seeds_by_series_);
        a("shards", v.routed_shards_);
        a("basis", v.basis_states_);
        a("processed", v.last_processed_by_source_);
        a("fusion", v.fusion_states_);
        a("fusion_update_seq", v.fusion_update_seq_);
        a("fusion_cleanup_bucket_cursor", v.fusion_cleanup_bucket_cursor_);
        a("fusion_state_evicted_total", v.fusion_state_evicted_total_);
        a("fusion_state_evicted_ttl_total", v.fusion_state_evicted_ttl_total_);
        a("fusion_state_evicted_capacity_total", v.fusion_state_evicted_capacity_total_);
        a("fusion_persistence_key_evicted_total", v.fusion_persistence_key_evicted_total_);
        a("fusion_cleanup_last_scan_count", v.fusion_cleanup_last_scan_count_);
        a("fusion_cleanup_last_evicted_count", v.fusion_cleanup_last_evicted_count_);
        a("fusion_cleanup_watermark_bucket_id", v.fusion_cleanup_watermark_bucket_id_);
    }
};
struct RelationCompatibility {
    RelationTaskCreateSpec spec;
    BaselineRelationRollingConfig config;
    RelationBasisRuntimeConfig basis_config;
    Compatibility value;
    Compatibility ratio;
};
template <>
struct Fields<RelationCompatibility> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("spec", v.spec);
        a("config", v.config);
        a("basis_config", v.basis_config);
        a("value", v.value);
        a("ratio", v.ratio);
    }
};
struct RoutedReference {
    std::string key;
    size_t shard = 0;
    size_t metric_index = 0;
    uint64_t basis_version = 0;
};
template <>
struct Fields<RoutedReference> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("key", v.key);
        a("shard", v.shard);
        a("metric_index", v.metric_index);
        a("basis_version", v.basis_version);
    }
};
struct ManagedSource {
    std::vector<RoutedReference> routed;
    std::vector<std::vector<uint64_t>> versions;
};
template <>
struct Fields<ManagedSource> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("routed", v.routed);
        a("versions", v.versions);
    }
};
using ManagedSources = std::unordered_map<std::string, ManagedSource>;
struct FusionLayoutEntry {
    std::string key;
    size_t bucket = 0;
};
template <>
struct Fields<FusionLayoutEntry> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("key", v.key);
        a("bucket", v.bucket);
    }
};
struct FusionLayout {
    size_t buckets = 1;
    std::vector<FusionLayoutEntry> iteration;
};
template <>
struct Fields<FusionLayout> {
    template <class A, class V>
    static void Visit(A& a, V& v) {
        a("buckets", v.buckets);
        a("iteration", v.iteration);
    }
};
RelationCompatibility CaptureRelation(const RelationTaskCreateSpec& spec, const BaselineRelationRollingConfig& config,
                                      const RelationBasisRuntimeConfig& basis_config,
                                      const std::shared_ptr<const CompiledEventCalendar>& calendar) {
    const auto& metric = spec.task_spec.metrics.front();
    return {spec, config, basis_config,
            Capture(MakeRoutedSummaryTaskSpec(spec, metric, "entropy_shannon", BaselineTaskKind::kValue), calendar),
            Capture(MakeRoutedSummaryTaskSpec(spec, metric, "headk_share", BaselineTaskKind::kRatio), calendar)};
}
void MapLimit(const rapidjson::Value& object, const char* name, uint64_t limit) {
    Require(object.IsObject() && object.HasMember(name) && object[name].IsArray() && object[name].Size() <= limit,
            "checkpoint relation map capacity");
}
uint64_t ProductLimit(uint64_t a, uint64_t b) {
    return b && a > std::numeric_limits<uint64_t>::max() / b ? std::numeric_limits<uint64_t>::max() : a * b;
}
uint64_t SumLimit(uint64_t a, uint64_t b) {
    return a > std::numeric_limits<uint64_t>::max() - b ? std::numeric_limits<uint64_t>::max() : a + b;
}
void PreflightRelation(const rapidjson::Value& payload, const RelationCompatibility& expected,
                       const BaselineStateLimitsV1& limits, size_t shards, uint64_t max_bytes) {
    Require(payload.IsObject() && payload.MemberCount() == 5 && payload.HasMember("kind") &&
                payload.HasMember("compatibility") && payload.HasMember("native") && payload.HasMember("managed") &&
                payload.HasMember("fusion_layout"),
            "checkpoint relation header");
    BaselineTaskKind kind{};
    Read(payload["kind"], kind);
    RelationCompatibility compatibility{};
    Read(payload["compatibility"], compatibility);
    Require(kind == BaselineTaskKind::kRelation && Encode(compatibility, max_bytes) == Encode(expected, max_bytes),
            "checkpoint relation configuration/calendar/group mismatch");
    const auto& native = payload["native"];
    MapLimit(native, "artifacts", limits.max_model_identities);
    MapLimit(native, "seeds", limits.max_model_identities);
    MapLimit(native, "processed", limits.max_runtime_identities);
    MapLimit(native, "fusion", limits.max_runtime_identities);
    MapLimit(native, "basis", ProductLimit(limits.max_runtime_identities, expected.spec.task_spec.metrics.size()));
    MapLimit(payload, "managed", limits.max_runtime_identities);
    Require(native.HasMember("shards") && native["shards"].IsArray() && native["shards"].Size() == shards,
            "checkpoint relation shard count");
    const auto max_children = ProductLimit(
        ProductLimit(limits.max_runtime_identities, expected.spec.task_spec.metrics.size()),
        SumLimit(4, ProductLimit(limits.max_basis_versions_per_metric,
                                 3 + static_cast<uint64_t>(expected.spec.task_spec.summary_policy.k_stable))));
    uint64_t children = 0;
    for (const auto& shard : native["shards"].GetArray()) {
        MapLimit(shard, "specs", max_children);
        MapLimit(shard, "seeds", max_children);
        MapLimit(shard, "rolling", max_children);
        Require(shard["specs"].Size() <= max_children - children, "checkpoint total routed capacity");
        children += shard["specs"].Size();
    }
    const auto& layout = payload["fusion_layout"];
    Require(layout.IsObject() && layout.HasMember("buckets") && layout["buckets"].IsUint64() &&
                layout["buckets"].GetUint64() > 0 && layout["buckets"].GetUint64() <= max_bytes / sizeof(void*) + 1,
            "checkpoint fusion layout allocation bound");
}
std::string SummaryName(const RelationTaskCreateSpec& parent, const BaselineTaskSpec& child, size_t metric) {
    const std::string prefix = parent.task_spec.task_id + "::" + parent.task_spec.metrics[metric] + "::";
    Require(child.task_id.compare(0, prefix.size(), prefix) == 0, "checkpoint routed task identity");
    const std::string summary = child.task_id.substr(prefix.size());
    const auto kind = child.task_kind == "value" ? BaselineTaskKind::kValue : BaselineTaskKind::kRatio;
    bool valid =
        kind == BaselineTaskKind::kValue
            ? summary == "entropy_shannon" || summary == "distinct_group_count" || summary == "stable_headk_mix_drift"
            : summary == "headk_share" || summary == "top1_share" || summary == "out_of_support_share" ||
                  summary == "stable_headk_coverage";
    constexpr std::string_view stable_prefix = "stable_g_share_";
    if (kind == BaselineTaskKind::kRatio && summary.compare(0, stable_prefix.size(), stable_prefix) == 0) {
        const auto digits = summary.substr(stable_prefix.size());
        valid =
            !digits.empty() && std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; });
        if (valid) {
            const auto group = std::stoull(digits);
            valid = group < static_cast<uint64_t>(parent.task_spec.summary_policy.k_stable) &&
                    summary == "stable_g_share_" + std::to_string(group);
        }
    }
    Require(valid, "checkpoint routed summary kind");
    return summary;
}
template <class B>
void ValidateBasis(const B& basis, const RelationTaskCreateSpec& spec, const std::string& metric) {
    const auto& task = spec.task_spec;
    Require(basis.basis_version > 0 && basis.metric_name == metric && basis.group_space_id == task.group_space_id &&
                basis.group_space_version == task.group_space_version.value_or("") &&
                basis.feature_base == (task.feature_base.empty() ? task.feature_id : task.feature_base) &&
                basis.k_head == task.summary_policy.k_head && basis.other_group_idxs == task.other_group_idxs &&
                basis.support_explicit.size() <= static_cast<size_t>(task.support_policy.k_support) &&
                basis.stable_head.size() <= static_cast<size_t>(task.summary_policy.k_stable) &&
                basis.stable_head.size() == basis.head_proto_q.size(),
            "checkpoint basis identity/dimensions");
    std::unordered_set<uint32_t> support, head;
    for (auto group : basis.support_explicit)
        Require(support.insert(group).second, "checkpoint duplicate support group");
    for (auto group : basis.stable_head)
        Require(head.insert(group).second && support.count(group), "checkpoint stable head group");
    for (double q : basis.head_proto_q) Require(q >= 0 && q <= 1, "checkpoint basis probability");
}
}  // namespace checkpoint

BaselineSerializationResult CheckpointAccess::ExportRelation(const BaselineRelationTask& task, uint64_t max_bytes) {
    try {
        const auto compatibility = CaptureRelation(task.spec_, task.relation_rolling_config_,
                                                   task.MakeBasisRuntimeConfig(), task.compiled_event_calendar_);
        BoundedStream stream(max_bytes);
        Writer writer(stream);
        writer.StartObject();
        WriteFields fields{writer};
        fields("kind", task.kind_);
        fields("compatibility", compatibility);
        fields("native", task);
        ManagedSources managed;
        size_t key_bytes = 0;
        for (const auto& [source, index] : task.managed_sources_) {
            auto& out = managed[source];
            out.versions = index.versions_by_metric;
            for (const auto& ref : index.routed) {
                Require(ref.key && ref.key->size() <= max_bytes - key_bytes, "checkpoint managed key budget");
                key_bytes += ref.key->size();
                out.routed.push_back({*ref.key, ref.shard, ref.metric_index, ref.basis_version});
            }
        }
        fields("managed", managed);
        FusionLayout layout;
        layout.buckets = task.fusion_states_.bucket_count();
        for (const auto& [key, state] : task.fusion_states_)
            layout.iteration.push_back({key, task.fusion_states_.bucket(key)});
        fields("fusion_layout", layout);
        writer.EndObject();
        return {BaselineStatus::kOk, Envelope(stream.content, max_bytes)};
    } catch (const std::exception&) {
        return {BaselineStatus::kSerializationFailed, {}};
    }
}

void CheckpointAccess::ValidateRelation(BaselineRelationTask& p, uint64_t max_bytes) {
    const auto& limits = *p.state_limits_;
    Require(p.artifacts_by_series_.size() <= limits.max_model_identities &&
                p.seeds_by_series_.size() == p.artifacts_by_series_.size() &&
                p.managed_sources_.size() <= limits.max_runtime_identities &&
                p.SeedsFitRuntimeLimits(p.seeds_by_series_),
            "checkpoint relation identity capacity");
    BootstrapEngine engine;
    for (const auto& [key, artifact] : p.artifacts_by_series_) {
        Require(!key.empty() && artifact.series_key == key &&
                    engine.ValidateArtifactCompatibility(artifact, p.spec_) == BaselineStatus::kOk &&
                    !artifact.value_model && !artifact.ratio_model,
                "checkpoint parent artifact");
        for (const auto& basis : artifact.relation_basis_by_metric) ValidateBasis(basis, p.spec_, basis.metric_name);
        for (const auto& child : artifact.relation_routed_summary_artifacts) {
            BootstrapArtifact model;
            model.artifact_kind = child.task_kind == BaselineTaskKind::kValue ? BootstrapArtifactKind::kValue
                                                                              : BootstrapArtifactKind::kRatio;
            model.value_model = child.value_model;
            model.ratio_model = child.ratio_model;
            ValidateScalarModels(model);
        }
        const auto seed = p.seeds_by_series_.find(key);
        BootstrapSeed derived;
        Require(seed != p.seeds_by_series_.end() && engine.ExportSeed(artifact, &derived) == BaselineStatus::kOk &&
                    Encode(derived, max_bytes) == Encode(seed->second, max_bytes),
                "checkpoint relation parent seed");
    }
    std::unordered_set<std::string> seen_children, seen_basis;
    for (const auto& [source, index] : p.managed_sources_) {
        Require(!source.empty() && index.versions_by_metric.size() == p.spec_.task_spec.metrics.size(),
                "checkpoint managed metric dimensions");
        for (size_t m = 0; m < index.versions_by_metric.size(); ++m) {
            const auto& versions = index.versions_by_metric[m];
            std::unordered_set<uint64_t> distinct;
            Require(versions.size() <= limits.max_basis_versions_per_metric, "checkpoint retained version capacity");
            for (auto version : versions)
                Require(version > 0 && distinct.insert(version).second, "checkpoint retained version identity");
            const auto basis_key = source + "::" + p.spec_.task_spec.metrics[m];
            const auto found = p.basis_states_.find(basis_key);
            if (found == p.basis_states_.end()) continue;
            Require(seen_basis.insert(basis_key).second, "checkpoint basis source ambiguity");
            const auto& state = found->second;
            const auto& acc = state.accumulator_;
            Require(Encode(state.config_, max_bytes) == Encode(p.MakeBasisRuntimeConfig(), max_bytes) &&
                        Encode(acc.config_, max_bytes) == Encode(state.config_.stream, max_bytes) &&
                        acc.groups_.size() <= acc.config_.max_groups &&
                        state.stable_refresh_counts_.size() <= acc.config_.max_groups && acc.total_mass_ >= 0 &&
                        static_cast<int>(state.basis_status_) >= 0 && static_cast<int>(state.basis_status_) <= 4,
                    "checkpoint basis config/capacity/status");
            if (acc.valid_bucket_count_)
                Require(acc.first_bucket_id_ <= acc.last_bucket_id_, "checkpoint accumulator cursor");
            for (const auto& [group, estimate] : acc.groups_)
                Require(group == estimate.group_idx && estimate.estimated_mass >= 0 &&
                            estimate.mass_error_upper_bound >= 0 &&
                            estimate.mass_error_upper_bound <= estimate.estimated_mass &&
                            estimate.active_bucket_count <= acc.valid_bucket_count_ &&
                            estimate.last_seen_bucket <= acc.last_bucket_id_,
                        "checkpoint group estimate");
            if (state.has_active_basis_) {
                ValidateBasis(state.active_basis_, p.spec_, p.spec_.task_spec.metrics[m]);
                Require(distinct.count(state.active_basis_.basis_version), "checkpoint active basis not retained");
            } else
                Require(state.basis_status_ == RelationBasisStatus::kNoBasis ||
                            state.basis_status_ == RelationBasisStatus::kCollecting,
                        "checkpoint basis status requires active basis");
            const auto cursor = p.last_processed_by_source_.find(source);
            if (acc.valid_bucket_count_ && cursor != p.last_processed_by_source_.end())
                Require(acc.last_bucket_id_ <= cursor->second, "checkpoint basis/source consumed cursor");
        }
        std::vector<std::unordered_map<uint64_t, size_t>> fanout(index.versions_by_metric.size());
        for (const auto& ref : index.routed) {
            Require(ref.key && ref.shard < p.routed_shards_.size() && ref.metric_index < fanout.size() &&
                        p.RoutedShardIndex(*ref.key) == ref.shard && seen_children.insert(*ref.key).second,
                    "checkpoint routed index/shard");
            const auto& shard = *p.routed_shards_[ref.shard];
            const auto child = shard.routed_specs_by_series.find(*ref.key);
            Require(child != shard.routed_specs_by_series.end(), "checkpoint routed spec missing");
            const auto summary = SummaryName(p.spec_, child->second, ref.metric_index);
            const auto kind = child->second.task_kind == "value" ? BaselineTaskKind::kValue : BaselineTaskKind::kRatio;
            const auto expected =
                MakeRoutedSummaryTaskSpec(p.spec_, p.spec_.task_spec.metrics[ref.metric_index], summary, kind);
            const auto identity = MakeRelationRoutedSummaryIdentity(source, p.spec_.task_spec.metrics[ref.metric_index],
                                                                    summary, kind, ref.basis_version);
            Require(Encode(child->second, max_bytes) == Encode(expected, max_bytes) &&
                        identity.routed_series_key == *ref.key && identity.basis_version == ref.basis_version &&
                        (!identity.basis_scoped || ref.basis_version > 0),
                    "checkpoint routed identity/config");
            const auto& versions = index.versions_by_metric[ref.metric_index];
            if (ref.basis_version > 0)
                Require(std::find(versions.begin(), versions.end(), ref.basis_version) != versions.end(),
                        "checkpoint child version not retained");
            const auto bound = ref.basis_version == 0
                                   ? uint64_t{4}
                                   : 3 + static_cast<uint64_t>(p.spec_.task_spec.summary_policy.k_stable);
            Require(++fanout[ref.metric_index][ref.basis_version] <= static_cast<size_t>(bound),
                    "checkpoint per-version fanout");
            const auto rolling = shard.routed_rolling_states.find(*ref.key);
            BaselineRollingConfig config;
            Require(ResolveBaselineRollingConfig(expected, &config) == BaselineStatus::kOk,
                    "checkpoint routed config unavailable");
            if (rolling != shard.routed_rolling_states.end()) {
                ValidateRolling(*ref.key, rolling->second, config);
                const auto cursor = p.last_processed_by_source_.find(source);
                Require(cursor != p.last_processed_by_source_.end() &&
                            rolling->second.last_processed_bucket <= cursor->second,
                        "checkpoint routed/source consumed cursor");
            }
            const auto seed = shard.routed_seeds_by_series.find(*ref.key);
            if (seed != shard.routed_seeds_by_series.end()) {
                Require(seed->second.series_key == *ref.key, "checkpoint routed seed key");
                bool matched = false;
                const auto parent = p.seeds_by_series_.find(source);
                if (parent != p.seeds_by_series_.end())
                    for (const auto& routed : parent->second.relation_routed_summary_seeds) {
                        RelationRoutedBootstrapSeedMaterialization materialized;
                        uint64_t fallback = 0;
                        for (const auto& basis : parent->second.relation_basis_by_metric)
                            if (basis.metric_name == routed.metric_name) fallback = basis.basis_version;
                        if (MaterializeRelationRoutedBootstrapSeed(p.spec_, source, routed, fallback, &materialized) ==
                                BaselineStatus::kOk &&
                            materialized.routed_series_key == *ref.key &&
                            Encode(materialized.seed, max_bytes) == Encode(seed->second, max_bytes))
                            matched = true;
                    }
                Require(matched, "checkpoint routed/parent seed mismatch");
            }
        }
    }
    size_t spec_count = 0;
    for (const auto& shard : p.routed_shards_) {
        Require(static_cast<bool>(shard), "checkpoint null shard");
        spec_count += shard->routed_specs_by_series.size();
        for (const auto& [key, value] : shard->routed_rolling_states)
            Require(seen_children.count(key), "checkpoint orphan rolling");
        for (const auto& [key, value] : shard->routed_seeds_by_series)
            Require(seen_children.count(key), "checkpoint orphan routed seed");
    }
    Require(spec_count == seen_children.size() && seen_basis.size() == p.basis_states_.size(),
            "checkpoint unindexed child/basis");
    for (const auto& [source, bucket] : p.last_processed_by_source_)
        Require(p.managed_sources_.count(source), "checkpoint orphan source cursor");
    const auto& fusion_config = p.relation_rolling_config_.relation_fusion;
    for (const auto& [source, state] : p.fusion_states_) {
        const auto cursor = p.last_processed_by_source_.find(source);
        Require(p.managed_sources_.count(source) && cursor != p.last_processed_by_source_.end() &&
                    (!state.has_last_bucket || state.last_bucket_id <= cursor->second) &&
                    state.last_touched_update_seq <= p.fusion_update_seq_ &&
                    state.persistence_by_evidence_dir.size() <= fusion_config.fusion_persistence_max_keys_per_source &&
                    (!state.has_last_bucket || (state.last_result.source_series_key == source &&
                                                state.last_result.bucket_id == state.last_bucket_id)),
                "checkpoint fusion identity/cursor/capacity");
    }
    Require(p.fusion_cleanup_bucket_cursor_ < p.fusion_states_.bucket_count(), "checkpoint fusion cleanup cursor");
}

BaselineStatus CheckpointAccess::RestoreRelation(BaselineRelationTask& task, const BaselineCheckpointRestoreV1& restore,
                                                 uint64_t max_bytes) {
    try {
        auto doc = DecodeEnvelope(restore.content, max_bytes);
        const auto compatibility = CaptureRelation(task.spec_, task.relation_rolling_config_,
                                                   task.MakeBasisRuntimeConfig(), task.compiled_event_calendar_);
        PreflightRelation(doc["payload"], compatibility, *task.state_limits_, task.runtime_shard_count_, max_bytes);
        BaselineRelationTask prepared(nullptr, task.task_id_, task.task_name_, task.config_json_, task.spec_,
                                      task.compiled_event_calendar_);
        prepared.state_limits_ = task.state_limits_;
        Read(doc["payload"]["native"], prepared);
        ManagedSources managed;
        Read(doc["payload"]["managed"], managed);
        for (const auto& [source, index] : managed) {
            auto& out = prepared.managed_sources_[source];
            out.versions_by_metric = index.versions;
            for (const auto& ref : index.routed) {
                Require(ref.shard < prepared.routed_shards_.size() && prepared.routed_shards_[ref.shard],
                        "checkpoint referenced shard");
                const auto& specs = prepared.routed_shards_[ref.shard]->routed_specs_by_series;
                const auto found = specs.find(ref.key);
                Require(found != specs.end(), "checkpoint referenced routed key");
                out.routed.push_back({&found->first, ref.shard, ref.metric_index, ref.basis_version});
            }
        }
        FusionLayout layout;
        Read(doc["payload"]["fusion_layout"], layout);
        Require(layout.iteration.size() == prepared.fusion_states_.size(), "checkpoint fusion layout count");
        decltype(prepared.fusion_states_) ordered;
        if (layout.buckets != ordered.bucket_count()) ordered.rehash(layout.buckets);
        Require(ordered.bucket_count() == layout.buckets, "checkpoint fusion hash implementation mismatch");
        for (auto it = layout.iteration.rbegin(); it != layout.iteration.rend(); ++it) {
            const auto found = prepared.fusion_states_.find(it->key);
            Require(found != prepared.fusion_states_.end() && ordered.bucket(it->key) == it->bucket,
                    "checkpoint fusion layout key/hash");
            ordered.emplace(found->first, std::move(found->second));
            prepared.fusion_states_.erase(found);
        }
        auto expected = layout.iteration.begin();
        for (const auto& [key, state] : ordered) {
            Require(expected != layout.iteration.end() && key == expected->key, "checkpoint fusion iteration mismatch");
            ++expected;
        }
        prepared.fusion_states_.swap(ordered);
        ValidateRelation(prepared, max_bytes);
        task.artifacts_by_series_.swap(prepared.artifacts_by_series_);
        task.seeds_by_series_.swap(prepared.seeds_by_series_);
        task.routed_shards_.swap(prepared.routed_shards_);
        task.basis_states_.swap(prepared.basis_states_);
        task.last_processed_by_source_.swap(prepared.last_processed_by_source_);
        task.fusion_states_.swap(prepared.fusion_states_);
        task.managed_sources_.swap(prepared.managed_sources_);
        task.fusion_update_seq_ = prepared.fusion_update_seq_;
        task.fusion_cleanup_bucket_cursor_ = prepared.fusion_cleanup_bucket_cursor_;
        task.fusion_state_evicted_total_ = prepared.fusion_state_evicted_total_;
        task.fusion_state_evicted_ttl_total_ = prepared.fusion_state_evicted_ttl_total_;
        task.fusion_state_evicted_capacity_total_ = prepared.fusion_state_evicted_capacity_total_;
        task.fusion_persistence_key_evicted_total_ = prepared.fusion_persistence_key_evicted_total_;
        task.fusion_cleanup_last_scan_count_ = prepared.fusion_cleanup_last_scan_count_;
        task.fusion_cleanup_last_evicted_count_ = prepared.fusion_cleanup_last_evicted_count_;
        task.fusion_cleanup_watermark_bucket_id_ = prepared.fusion_cleanup_watermark_bucket_id_;
        task.checkpoint_restored_ = true;
        task.state_operations_started_ = true;
        return BaselineStatus::kOk;
    } catch (const std::exception&) {
        return BaselineStatus::kInvalidArgument;
    }
}
}  // namespace flowsql::baseline
