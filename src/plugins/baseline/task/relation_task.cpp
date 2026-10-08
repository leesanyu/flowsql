// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "relation_task.h"

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

#include "plugins/baseline/model/observation_validation.h"
#include "plugins/baseline/relation/routed_summary.h"
#include "plugins/baseline/rolling/rolling_config.h"
#include "plugins/baseline/serialization/json_serialization.h"

namespace flowsql {
namespace baseline {

namespace {

void AppendDiagnostic(bool enabled, const std::string& item, std::string* diagnostics) {
    if (!enabled || !diagnostics || item.empty()) return;
    if (!diagnostics->empty()) *diagnostics += ";";
    *diagnostics += item;
}

template <typename Writer>
void WriteFusionSingleEvidence(Writer* writer, const RelationFusionSingleEvidence& evidence) {
    writer->StartObject();
    WriteStringField(writer, "metric", evidence.metric);
    WriteStringField(writer, "summary", evidence.summary);
    WriteStringField(writer, "direction", evidence.direction);
    writer->Key("basis_version");
    writer->Uint64(evidence.basis_version);
    writer->Key("basis_scoped");
    writer->Bool(evidence.basis_scoped);
    writer->Key("normalized_score");
    writer->Double(evidence.normalized_score);
    writer->Key("confidence");
    writer->Double(evidence.confidence);
    writer->Key("persistence");
    writer->Uint(evidence.persistence);
    writer->Key("evidence_strength");
    writer->Double(evidence.evidence_strength);
    writer->Key("available");
    writer->Bool(evidence.available);
    writer->Key("can_alert");
    writer->Bool(evidence.can_alert);
    WriteStringField(writer, "score_trust_status", evidence.score_trust_status);
    WriteStringField(writer, "metric_basis_status", evidence.metric_basis_status);
    if (!evidence.unavailable_reason.empty()) {
        WriteStringField(writer, "unavailable_reason", evidence.unavailable_reason);
    }
    WriteStringField(writer, "routed_series_key", evidence.routed_series_key);
    writer->EndObject();
}

template <typename Writer>
void WriteFusionPatternScore(Writer* writer, const RelationFusionPatternScore& pattern) {
    writer->StartObject();
    WriteStringField(writer, "pattern", pattern.pattern);
    writer->Key("score");
    writer->Double(pattern.score);
    writer->Key("weighted_score");
    writer->Double(pattern.weighted_score);
    writer->Key("pattern_weight");
    writer->Double(pattern.pattern_weight);
    writer->Key("metrics_hit");
    writer->StartArray();
    for (const auto& metric : pattern.metrics_hit) writer->String(metric.c_str());
    writer->EndArray();
    writer->Key("supporting_features");
    writer->StartArray();
    for (const auto& feature : pattern.supporting_features) writer->String(feature.c_str());
    writer->EndArray();
    if (!pattern.diagnostics.empty()) {
        WriteStringField(writer, "diagnostics", pattern.diagnostics);
    }
    writer->EndObject();
}

template <typename Writer>
void WriteFusionResult(Writer* writer, const RelationFusionResult& fusion) {
    writer->StartObject();
    writer->Key("enabled");
    writer->Bool(true);
    writer->Key("bucket_id");
    writer->Int64(fusion.bucket_id);
    writer->Key("relation_risk");
    writer->Double(fusion.relation_risk);
    writer->Key("single_risk");
    writer->Double(fusion.single_risk);
    writer->Key("pattern_risk");
    writer->Double(fusion.pattern_risk);
    writer->Key("dominant_single");
    writer->StartArray();
    for (const auto& evidence : fusion.dominant_single) {
        WriteFusionSingleEvidence(writer, evidence);
    }
    writer->EndArray();
    writer->Key("dominant_pattern");
    writer->StartArray();
    for (const auto& pattern : fusion.dominant_pattern) {
        WriteFusionPatternScore(writer, pattern);
    }
    writer->EndArray();
    writer->Key("pattern_scores");
    writer->StartArray();
    for (const auto& pattern : fusion.pattern_scores) {
        WriteFusionPatternScore(writer, pattern);
    }
    writer->EndArray();
    if (!fusion.diagnostics.empty()) {
        WriteStringField(writer, "diagnostics", fusion.diagnostics);
    }
    writer->EndObject();
}

BaselineTaskSpec MakeRelationRollingConfigSpec(const RelationTaskCreateSpec& spec) {
    BaselineTaskSpec rolling_spec;
    rolling_spec.task_id = spec.task_spec.task_id;
    rolling_spec.task_kind = "relation";
    rolling_spec.feature_type = "relation";
    rolling_spec.feature_id = spec.task_spec.feature_id;
    rolling_spec.profile = spec.task_spec.profile;
    rolling_spec.clock_spec.bucket_seconds = spec.clock_spec.delta;
    rolling_spec.clock_spec.timezone = spec.clock_spec.tz;
    rolling_spec.delta = spec.clock_spec.delta;
    rolling_spec.tz = spec.clock_spec.tz;
    return rolling_spec;
}

BaselineRelationRollingConfig ResolveRelationRollingConfigForSpec(
    const RelationTaskCreateSpec& spec) {
    BaselineRollingConfig rolling_config;
    (void)ResolveBaselineRollingConfig(
        MakeRelationRollingConfigSpec(spec), &rolling_config, nullptr);
    return rolling_config.relation_rolling;
}

std::size_t NormalizeRuntimeShardCount(uint32_t shard_count) {
    return std::max<std::size_t>(1, static_cast<std::size_t>(shard_count));
}

RelationServiceBasis BasisFromSeed(const BootstrapRelationBasisSeed& seed) {
    RelationServiceBasis basis;
    basis.basis_version = seed.basis_version;
    basis.feature_base = seed.feature_base;
    basis.metric_name = seed.metric_name;
    basis.group_space_id = seed.group_space_id;
    basis.group_space_version = seed.group_space_version;
    basis.k_head = seed.k_head;
    basis.other_group_idxs = seed.other_group_idxs;
    basis.support_explicit = seed.support_explicit;
    basis.stable_head = seed.stable_head;
    basis.head_proto_q = seed.head_proto_q;
    return basis;
}

RelationBasisStatus BasisStatusFromSeedStatus(BootstrapSeedStatus status) {
    return status == BootstrapSeedStatus::kFull ? RelationBasisStatus::kBasisReady
                                                : RelationBasisStatus::kBasisWarming;
}

std::string BasisStateKey(std::string_view source_series_key, std::string_view metric_name) {
    return std::string(source_series_key) + "::" + std::string(metric_name);
}

bool KeyBelongsToSource(const std::string& key, std::string_view source_series_key) {
    const std::string prefix = std::string(source_series_key) + "::";
    return key.size() > prefix.size() && key.compare(0, prefix.size(), prefix) == 0;
}

struct ParsedRoutedSeriesKey {
    std::string metric;
    std::string summary;
    std::string feature_type;
    uint64_t basis_version = 0;
    bool basis_scoped = false;
};

bool ParseRoutedSeriesKeyForSource(const std::string& key,
                                   std::string_view source_series_key,
                                   ParsedRoutedSeriesKey* out) {
    if (!out) return false;
    const std::string prefix = std::string(source_series_key) + "::";
    if (key.size() <= prefix.size() || key.compare(0, prefix.size(), prefix) != 0) {
        return false;
    }

    std::vector<std::string> parts;
    std::size_t pos = prefix.size();
    while (pos <= key.size()) {
        const std::size_t next = key.find("::", pos);
        if (next == std::string::npos) {
            parts.push_back(key.substr(pos));
            break;
        }
        parts.push_back(key.substr(pos, next - pos));
        pos = next + 2;
    }
    if (parts.size() != 3 && parts.size() != 4) return false;

    ParsedRoutedSeriesKey parsed;
    parsed.metric = parts[0];
    parsed.summary = parts[1];
    parsed.feature_type = parts[2];
    if (parts.size() == 4) {
        constexpr char kBasisPrefix[] = "basis:";
        if (parts[3].compare(0, sizeof(kBasisPrefix) - 1, kBasisPrefix) != 0) {
            return false;
        }
        parsed.basis_scoped = true;
        parsed.basis_version =
            static_cast<uint64_t>(std::strtoull(
                parts[3].c_str() + sizeof(kBasisPrefix) - 1, nullptr, 10));
    }
    *out = std::move(parsed);
    return !out->metric.empty() && !out->summary.empty() && !out->feature_type.empty();
}

uint64_t BasisVersionForMetric(const BootstrapSeed& seed, const std::string& metric_name) {
    for (const auto& basis : seed.relation_basis_by_metric) {
        if (basis.metric_name == metric_name) return basis.basis_version;
    }
    return 0;
}

BaselineTaskKind TaskKindFromFeatureType(std::string_view feature_type) {
    return feature_type == "ratio" ? BaselineTaskKind::kRatio : BaselineTaskKind::kValue;
}

const char* RelationBasisStatusName(RelationBasisStatus status) {
    switch (status) {
        case RelationBasisStatus::kNoBasis:
            return "no_basis";
        case RelationBasisStatus::kCollecting:
            return "collecting";
        case RelationBasisStatus::kBasisWarming:
            return "basis_warming";
        case RelationBasisStatus::kBasisReady:
            return "basis_ready";
        case RelationBasisStatus::kHandoverWarming:
            return "handover_warming";
    }
    return "unknown";
}

std::string MetricNameForIndex(const RelationTaskCreateSpec& spec,
                               std::size_t metric_index) {
    if (metric_index < spec.task_spec.metrics.size()) return spec.task_spec.metrics[metric_index];
    return "";
}

RelationBasisBuildInput MakeBasisBuildInput(const RelationTaskCreateSpec& spec,
                                            const std::string& metric_name,
                                            uint64_t basis_version) {
    RelationBasisBuildInput input;
    input.basis_version = basis_version;
    input.feature_base = spec.task_spec.feature_base.empty()
                             ? spec.task_spec.feature_id
                             : spec.task_spec.feature_base;
    input.metric_name = metric_name;
    input.group_space_id = spec.task_spec.group_space_id;
    input.group_space_version = spec.task_spec.group_space_version.value_or("");
    input.other_group_idxs = spec.task_spec.other_group_idxs;
    input.support_policy = spec.task_spec.support_policy;
    input.summary_policy = spec.task_spec.summary_policy;
    return input;
}

}  // namespace

BaselineRelationTask::BaselineRelationTask(TaskRegistry* registry,
                         std::string task_id,
                         std::string task_name,
                         std::string config_content,
                         RelationTaskCreateSpec spec,
                         std::shared_ptr<const CompiledEventCalendar> compiled_event_calendar)
    : BaselineTaskBase(registry,
                       std::move(task_id),
                       BaselineTaskKind::kRelation,
                       std::move(task_name),
                       std::move(config_content)),
      spec_(std::move(spec)),
      compiled_event_calendar_(std::move(compiled_event_calendar)) {
    relation_rolling_config_ = ResolveRelationRollingConfigForSpec(spec_);
    runtime_shard_count_ =
        NormalizeRuntimeShardCount(relation_rolling_config_.routed_state_shard_count);
    routed_shards_.reserve(runtime_shard_count_);
    for (std::size_t i = 0; i < runtime_shard_count_; ++i) {
        routed_shards_.push_back(std::make_unique<RelationRoutedRuntimeShard>());
    }
}

const char* BaselineRelationTask::Id() const { return BaselineTaskBase::Id(); }
const char* BaselineRelationTask::Name() const { return BaselineTaskBase::Name(); }
BaselineTaskKind BaselineRelationTask::Kind() const { return BaselineTaskBase::Kind(); }

BaselineSerializationResult BaselineRelationTask::ExportConfig(
    BaselineSerializationFormat format) const {
    return BaselineTaskBase::ExportConfig(format);
}

BaselineSerializationResult BaselineRelationTask::QueryTaskSnapshot(
    BaselineSerializationFormat format) const {
    if (format != BaselineSerializationFormat::kJson) {
        return {BaselineStatus::kUnsupportedFormat, ""};
    }

    uint64_t source_state_count = 0;
    uint64_t routed_seed_count = 0;
    uint64_t routed_state_count = 0;
    uint64_t fusion_source_state_count = 0;
    uint64_t fusion_persistence_state_count = 0;
    const BaselineStatus status = EnsureOpen();
    if (status != BaselineStatus::kOk) return {status, ""};
    source_state_count = static_cast<uint64_t>(basis_states_.size());
    for (const auto& shard_ptr : routed_shards_) {
        const RelationRoutedRuntimeShard& shard = *shard_ptr;
        routed_seed_count +=
            static_cast<uint64_t>(shard.routed_seeds_by_series.size());
        routed_state_count +=
            static_cast<uint64_t>(shard.routed_rolling_states.size());
    }
    fusion_source_state_count = static_cast<uint64_t>(fusion_states_.size());
    for (const auto& entry : fusion_states_) {
        fusion_persistence_state_count += static_cast<uint64_t>(
            entry.second.persistence_by_evidence_dir.size());
    }

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buf);
    writer.StartObject();
    writer.Key("schema_version");
    writer.Int(1);
    WriteStringField(&writer, "document_kind", "relation_task_snapshot");
    WriteStringField(&writer, "task_id", spec_.task_spec.task_id);
    WriteStringField(&writer, "name", spec_.task_spec.name);
    WriteStringField(&writer, "kind", "relation");
    writer.Key("relation_runtime");
    writer.StartObject();
    writer.Key("source_state_count");
    writer.Uint64(source_state_count);
    writer.Key("routed_shard_count");
    writer.Uint64(static_cast<uint64_t>(runtime_shard_count_));
    writer.Key("routed_seed_count");
    writer.Uint64(routed_seed_count);
    writer.Key("routed_state_count");
    writer.Uint64(routed_state_count);
    writer.EndObject();
    writer.Key("relation_fusion");
    writer.StartObject();
    writer.Key("enabled");
    writer.Bool(MakeFusionRuntimeConfig().enable_relation_fusion);
    writer.Key("source_state_count");
    writer.Uint64(fusion_source_state_count);
    writer.Key("source_state_max");
    writer.Uint64(relation_rolling_config_.relation_fusion.fusion_state_max_sources);
    writer.Key("persistence_state_count");
    writer.Uint64(fusion_persistence_state_count);
    writer.Key("persistence_key_max_per_source");
    writer.Uint64(
        relation_rolling_config_.relation_fusion.fusion_persistence_max_keys_per_source);
    writer.Key("state_evicted_total");
    writer.Uint64(fusion_state_evicted_total_);
    writer.Key("state_evicted_ttl_total");
    writer.Uint64(fusion_state_evicted_ttl_total_);
    writer.Key("state_evicted_capacity_total");
    writer.Uint64(fusion_state_evicted_capacity_total_);
    writer.Key("persistence_key_evicted_total");
    writer.Uint64(fusion_persistence_key_evicted_total_);
    writer.Key("cleanup_last_scan_count");
    writer.Uint64(fusion_cleanup_last_scan_count_);
    writer.Key("cleanup_last_evicted_count");
    writer.Uint64(fusion_cleanup_last_evicted_count_);
    writer.Key("cleanup_watermark_bucket_id");
    writer.Int64(fusion_cleanup_watermark_bucket_id_);
    writer.Key("cleanup_ttl_buckets");
    writer.Uint64(relation_rolling_config_.relation_fusion.fusion_state_ttl_buckets);
    writer.Key("cleanup_scan_limit");
    writer.Uint64(relation_rolling_config_.relation_fusion.fusion_state_cleanup_scan_limit);
    writer.Key("pattern_count");
    writer.Uint(4);
    writer.EndObject();
    writer.EndObject();
    return {BaselineStatus::kOk, buf.GetString()};
}

BaselineSerializationResult BaselineRelationTask::QuerySeriesSnapshot(
    std::string_view series_key,
    BaselineSerializationFormat format) const {
    if (format != BaselineSerializationFormat::kJson) {
        return {BaselineStatus::kUnsupportedFormat, ""};
    }
    if (series_key.empty()) return {BaselineStatus::kInvalidArgument, ""};
    const BaselineStatus status = EnsureOpen();
    if (status != BaselineStatus::kOk) return {status, ""};

    struct BasisSnapshotEntry {
        std::string metric;
        uint64_t basis_version = 0;
        RelationBasisStatus status = RelationBasisStatus::kNoBasis;
        uint64_t support_size = 0;
        uint64_t stable_head_size = 0;
        uint64_t stats_group_count = 0;
        bool handover_active = false;
    };
    struct RoutedSnapshotEntry {
        std::string metric;
        std::string summary;
        std::string feature_type;
        std::string routed_series_key;
        uint64_t basis_version = 0;
        std::string maturity_status = "not_started";
        std::string score_trust_status = "not_started";
    };

    const std::string key(series_key);
    std::vector<BasisSnapshotEntry> basis_entries;
    std::vector<RoutedSnapshotEntry> routed_entries;
    uint64_t routed_seed_count = 0;
    uint64_t routed_state_count = 0;
    bool has_fusion_snapshot = false;
    RelationFusionResult fusion_snapshot;
    for (const auto& entry : basis_states_) {
        if (!KeyBelongsToSource(entry.first, series_key)) continue;

        BasisSnapshotEntry snapshot;
        const std::string prefix = key + "::";
        snapshot.metric = entry.first.substr(prefix.size());
        snapshot.status = entry.second.basis_status();
        snapshot.handover_active =
            snapshot.status == RelationBasisStatus::kHandoverWarming;
        snapshot.stats_group_count =
            static_cast<uint64_t>(entry.second.accumulator().group_count());
        if (const RelationServiceBasis* basis = entry.second.active_basis()) {
            snapshot.basis_version = basis->basis_version;
            snapshot.support_size = static_cast<uint64_t>(basis->support_explicit.size());
            snapshot.stable_head_size = static_cast<uint64_t>(basis->stable_head.size());
        }
        basis_entries.push_back(std::move(snapshot));
    }
    for (const auto& shard_ptr : routed_shards_) {
        const RelationRoutedRuntimeShard& shard = *shard_ptr;
        if (shard.routed_specs_by_series.find(key) != shard.routed_specs_by_series.end() ||
            shard.routed_seeds_by_series.find(key) != shard.routed_seeds_by_series.end() ||
            shard.routed_rolling_states.find(key) != shard.routed_rolling_states.end()) {
            return {BaselineStatus::kInvalidArgument, ""};
        }

        std::unordered_set<std::string> routed_keys;
        routed_keys.reserve(shard.routed_specs_by_series.size() +
                            shard.routed_seeds_by_series.size() +
                            shard.routed_rolling_states.size());
        for (const auto& entry : shard.routed_specs_by_series) {
            if (KeyBelongsToSource(entry.first, series_key)) routed_keys.insert(entry.first);
        }
        for (const auto& entry : shard.routed_seeds_by_series) {
            if (KeyBelongsToSource(entry.first, series_key)) {
                routed_keys.insert(entry.first);
                ++routed_seed_count;
            }
        }
        for (const auto& entry : shard.routed_rolling_states) {
            if (KeyBelongsToSource(entry.first, series_key)) {
                routed_keys.insert(entry.first);
                ++routed_state_count;
            }
        }

        for (const std::string& routed_key : routed_keys) {
            ParsedRoutedSeriesKey parsed;
            if (!ParseRoutedSeriesKeyForSource(routed_key, series_key, &parsed)) continue;

            RoutedSnapshotEntry snapshot;
            snapshot.metric = std::move(parsed.metric);
            snapshot.summary = std::move(parsed.summary);
            snapshot.feature_type = std::move(parsed.feature_type);
            snapshot.routed_series_key = routed_key;
            snapshot.basis_version = parsed.basis_scoped ? parsed.basis_version : 0;
            const auto state_it = shard.routed_rolling_states.find(routed_key);
            if (state_it != shard.routed_rolling_states.end()) {
                snapshot.maturity_status =
                    RollingMaturityStatusName(state_it->second.maturity_status);
                snapshot.score_trust_status =
                    ScoreTrustStatusName(state_it->second.score_trust_status);
            }
            routed_entries.push_back(std::move(snapshot));
        }
    }
    const auto fusion_it = fusion_states_.find(key);
    if (fusion_it != fusion_states_.end() &&
        fusion_it->second.last_result.status == BaselineStatus::kOk) {
        fusion_snapshot = fusion_it->second.last_result;
        has_fusion_snapshot = true;
    }
    if (basis_entries.empty() && routed_entries.empty() && !has_fusion_snapshot) {
        return {BaselineStatus::kNotTrained, ""};
    }

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buf);
    writer.StartObject();
    writer.Key("schema_version");
    writer.Int(1);
    WriteStringField(&writer, "document_kind", "relation_series_snapshot");
    WriteStringField(&writer, "series_key", key);
    WriteStringField(&writer, "source_series_key", key);
    writer.Key("basis_state_count");
    writer.Uint64(static_cast<uint64_t>(basis_entries.size()));
    writer.Key("routed_seed_count");
    writer.Uint64(routed_seed_count);
    writer.Key("routed_state_count");
    writer.Uint64(routed_state_count);
    writer.Key("basis_by_metric");
    writer.StartArray();
    for (const auto& entry : basis_entries) {
        writer.StartObject();
        WriteStringField(&writer, "metric", entry.metric);
        writer.Key("basis_version");
        writer.Uint64(entry.basis_version);
        WriteStringField(&writer, "basis_status", RelationBasisStatusName(entry.status));
        writer.Key("support_size");
        writer.Uint64(entry.support_size);
        writer.Key("stable_head_size");
        writer.Uint64(entry.stable_head_size);
        writer.Key("stats_group_count");
        writer.Uint64(entry.stats_group_count);
        writer.Key("handover_active");
        writer.Bool(entry.handover_active);
        writer.EndObject();
    }
    writer.EndArray();
    writer.Key("routed_summaries");
    writer.StartArray();
    for (const auto& entry : routed_entries) {
        writer.StartObject();
        WriteStringField(&writer, "metric", entry.metric);
        WriteStringField(&writer, "summary", entry.summary);
        WriteStringField(&writer, "feature_type", entry.feature_type);
        WriteStringField(&writer, "routed_series_key", entry.routed_series_key);
        writer.Key("basis_version");
        writer.Uint64(entry.basis_version);
        WriteStringField(&writer, "maturity_status", entry.maturity_status);
        WriteStringField(&writer, "score_trust_status", entry.score_trust_status);
        writer.EndObject();
    }
    writer.EndArray();
    writer.Key("relation_fusion");
    if (has_fusion_snapshot && MakeFusionRuntimeConfig().enable_relation_fusion) {
        WriteFusionResult(&writer, fusion_snapshot);
    } else {
        writer.StartObject();
        writer.Key("enabled");
        writer.Bool(MakeFusionRuntimeConfig().enable_relation_fusion);
        writer.EndObject();
    }
    writer.EndObject();
    return {BaselineStatus::kOk, buf.GetString()};
}

BaselineStatus BaselineRelationTask::Close() { return BaselineTaskBase::Close(); }

BaselineStateUsageV1 BaselineRelationTask::DoQueryStateUsage() const {
    BaselineStateUsageV1 usage;
    usage.runtime_identities = static_cast<uint64_t>(managed_sources_.size());
    usage.model_identities = static_cast<uint64_t>(artifacts_by_series_.size());
    for (const auto& source : managed_sources_) {
        for (const auto& versions : source.second.versions_by_metric) usage.retained_basis_versions += versions.size();
    }
    // Specs own every routed identity, including seed-only children not yet materialized as rolling states.
    for (const auto& shard : routed_shards_) usage.routed_states += shard->routed_specs_by_series.size();
    return usage;
}

bool BaselineRelationTask::HasIdentityCapacity(const std::string& source, bool include_model) const {
    const auto* limits = StateLimits();
    if (!limits) return true;
    return (managed_sources_.find(source) != managed_sources_.end() ||
            managed_sources_.size() < limits->max_runtime_identities) &&
           (!include_model || artifacts_by_series_.find(source) != artifacts_by_series_.end() ||
            artifacts_by_series_.size() < limits->max_model_identities);
}

BaselineRelationTask::ManagedSourceRuntimeIndex& BaselineRelationTask::TrackManagedSource(const std::string& source) {
    auto inserted = managed_sources_.try_emplace(source);
    if (inserted.second) inserted.first->second.versions_by_metric.resize(spec_.task_spec.metrics.size());
    return inserted.first->second;
}

bool BaselineRelationTask::SeedsFitRuntimeLimits(const BootstrapSeedStore& seeds) const {
    const auto* limits = StateLimits();
    if (!limits) return true;
    for (const auto& entry : seeds) {
        for (const auto& basis : entry.second.relation_basis_by_metric) {
            if (basis.support_explicit.size() > static_cast<uint64_t>(spec_.task_spec.support_policy.k_support) ||
                basis.stable_head.size() > static_cast<uint64_t>(spec_.task_spec.summary_policy.k_stable))
                return false;
        }
        for (const auto& metric : spec_.task_spec.metrics) {
            std::unordered_set<uint64_t> versions;
            std::unordered_map<uint64_t, uint64_t> children_by_version;
            const uint64_t fallback = BasisVersionForMetric(entry.second, metric);
            if (fallback > 0) versions.insert(fallback);
            if (relation_rolling_config_.enable_routed_rolling) {
                for (const auto& routed : entry.second.relation_routed_summary_seeds) {
                    if (routed.metric_name != metric) continue;
                    const uint64_t version = IsBasisScopedRelationSummary(routed.summary_name)
                                                 ? (routed.basis_version > 0 ? routed.basis_version : fallback)
                                                 : 0;
                    // Four universal summaries; each scoped version adds three plus k_stable group shares.
                    const uint64_t fanout =
                        version == 0 ? 4 : 3 + static_cast<uint64_t>(spec_.task_spec.summary_policy.k_stable);
                    if (++children_by_version[version] > fanout) return false;
                    if (version > 0) versions.insert(version);
                    if (versions.size() > limits->max_basis_versions_per_metric) return false;
                }
            }
        }
    }
    return true;
}

void BaselineRelationTask::TrackManagedVersion(const std::string& source, std::size_t metric_index, uint64_t version) {
    if (!StateLimits()) return;
    auto& index = TrackManagedSource(source);
    if (version == 0) return;
    auto& versions = index.versions_by_metric[metric_index];
    if (std::find(versions.begin(), versions.end(), version) == versions.end()) versions.push_back(version);
}

void BaselineRelationTask::TrackManagedRouted(const std::string& source, std::size_t metric_index, uint64_t version,
                                              std::size_t shard, const std::string& key) {
    if (!StateLimits()) return;
    TrackManagedVersion(source, metric_index, version);
    TrackManagedSource(source).routed.push_back({&key, shard, metric_index, version});
}

void BaselineRelationTask::ReleaseRuntimeForSource(const std::string& source) {
    const auto index = managed_sources_.find(source);
    if (index == managed_sources_.end()) return;
    for (const auto& child : index->second.routed) {
        auto& shard = *routed_shards_[child.shard];
        shard.routed_rolling_states.erase(*child.key);
        shard.routed_seeds_by_series.erase(*child.key);
        shard.routed_specs_by_series.erase(*child.key);  // The key reference is no longer used after this erase.
    }
    for (const auto& metric : spec_.task_spec.metrics) basis_states_.erase(BasisStateKey(source, metric));
    fusion_states_.erase(source);
    last_processed_by_source_.erase(source);
    managed_sources_.erase(index);
}

BaselineStatus BaselineRelationTask::DoReleaseIdentity(std::string_view key, BaselineStateReleaseScopeV1 scope) {
    const std::string source(key);
    if (managed_sources_.find(source) == managed_sources_.end() &&
        artifacts_by_series_.find(source) == artifacts_by_series_.end()) {
        const auto& shard = *routed_shards_[RoutedShardIndex(source)];
        if (shard.routed_specs_by_series.find(source) != shard.routed_specs_by_series.end()) {
            return BaselineStatus::kInvalidArgument;
        }
    }
    ReleaseRuntimeForSource(source);
    if (scope == BaselineStateReleaseScopeV1::kAllState) {
        seeds_by_series_.erase(source);
        artifacts_by_series_.erase(source);
    }
    return BaselineStatus::kOk;
}

void BaselineRelationTask::RetireManagedVersions(const std::string& source) {
    const auto* limits = StateLimits();
    if (!limits) return;
    auto found = managed_sources_.find(source);
    if (found == managed_sources_.end()) return;
    auto& index = found->second;
    for (std::size_t metric_index = 0; metric_index < index.versions_by_metric.size(); ++metric_index) {
        auto& versions = index.versions_by_metric[metric_index];
        if (versions.size() <= limits->max_basis_versions_per_metric) continue;
        const auto basis = basis_states_.find(BasisStateKey(source, spec_.task_spec.metrics[metric_index]));
        const uint64_t active = basis != basis_states_.end() && basis->second.active_basis()
                                    ? basis->second.active_basis()->basis_version
                                    : 0;
        // The runtime holds only its active basis. Keep its immediately preceding version throughout handover.
        uint64_t previous = 0;
        if (basis != basis_states_.end() && basis->second.basis_status() == RelationBasisStatus::kHandoverWarming) {
            for (const auto version : versions)
                if (version < active) previous = std::max(previous, version);
        }
        while (versions.size() > limits->max_basis_versions_per_metric) {
            auto oldest = versions.end();
            for (auto it = versions.begin(); it != versions.end(); ++it) {
                if (*it != active && *it != previous && (oldest == versions.end() || *it < *oldest)) oldest = it;
            }
            // At most active and previous are protected; Bind requires a limit of at least two.
            const uint64_t retired = *oldest;
            const auto removed = std::remove_if(index.routed.begin(), index.routed.end(), [&](const auto& child) {
                if (child.metric_index != metric_index || child.basis_version != retired) return false;
                auto& shard = *routed_shards_[child.shard];
                shard.routed_rolling_states.erase(*child.key);
                shard.routed_seeds_by_series.erase(*child.key);
                shard.routed_specs_by_series.erase(*child.key);
                return true;
            });
            index.routed.erase(removed, index.routed.end());
            versions.erase(oldest);
        }
    }
}

void BaselineRelationTask::OnClosing() {
    decltype(managed_sources_){}.swap(managed_sources_);
    decltype(routed_shards_){}.swap(routed_shards_);
    RelationBasisStateMap{}.swap(basis_states_);
    decltype(last_processed_by_source_){}.swap(last_processed_by_source_);
    decltype(fusion_states_){}.swap(fusion_states_);
    BootstrapSeedStore{}.swap(seeds_by_series_);
    BootstrapArtifactStore{}.swap(artifacts_by_series_);
    compiled_event_calendar_.reset();
}

std::size_t BaselineRelationTask::RoutedShardIndex(
    std::string_view routed_series_key) const {
    return std::hash<std::string_view>{}(routed_series_key) % runtime_shard_count_;
}

void BaselineRelationTask::RebuildRuntimeFromRelationSeeds() {
    managed_sources_.clear();
    basis_states_.clear();
    last_processed_by_source_.clear();
    for (auto& shard_ptr : routed_shards_) {
        RelationRoutedRuntimeShard& shard = *shard_ptr;
        shard.routed_seeds_by_series.clear();
        shard.routed_specs_by_series.clear();
        shard.routed_rolling_states.clear();
    }
    fusion_states_.clear();
    ResetFusionCleanupRuntime();

    for (const auto& entry : seeds_by_series_) InitializeRuntimeFromRelationSeed(entry.second);
}

void BaselineRelationTask::RebuildRuntimeForSource(std::string_view source_series_key) {
    const std::string source(source_series_key);
    if (StateLimits()) {
        ReleaseRuntimeForSource(source);
        const auto seed = seeds_by_series_.find(source);
        if (seed != seeds_by_series_.end()) InitializeRuntimeFromRelationSeed(seed->second);
        return;
    }
    for (const std::string& metric : spec_.task_spec.metrics) basis_states_.erase(BasisStateKey(source, metric));
    for (auto& shard_ptr : routed_shards_) {
        auto erase_source = [&](auto& states) {
            for (auto it = states.begin(); it != states.end();) {
                ParsedRoutedSeriesKey parsed;
                if (ParseRoutedSeriesKeyForSource(it->first, source, &parsed)) {
                    it = states.erase(it);
                } else {
                    ++it;
                }
            }
        };
        erase_source(shard_ptr->routed_seeds_by_series);
        erase_source(shard_ptr->routed_specs_by_series);
        erase_source(shard_ptr->routed_rolling_states);
    }
    fusion_states_.erase(source);
    last_processed_by_source_.erase(source);
    const auto seed_it = seeds_by_series_.find(source);
    if (seed_it != seeds_by_series_.end()) InitializeRuntimeFromRelationSeed(seed_it->second);
}

void BaselineRelationTask::InitializeRuntimeFromRelationSeed(const BootstrapSeed& seed) {
    if (StateLimits()) TrackManagedSource(seed.series_key);
    last_processed_by_source_.insert_or_assign(seed.series_key, seed.coverage_report.train_end_bucket);
    for (const auto& basis_seed : seed.relation_basis_by_metric) {
        RelationBasisRuntimeState runtime(MakeBasisRuntimeConfig());
        (void)runtime.LoadSeedBasis(BasisFromSeed(basis_seed), BasisStatusFromSeedStatus(seed.seed_status));
        basis_states_.insert_or_assign(BasisStateKey(seed.series_key, basis_seed.metric_name), std::move(runtime));
        const auto metric =
            std::find(spec_.task_spec.metrics.begin(), spec_.task_spec.metrics.end(), basis_seed.metric_name);
        TrackManagedVersion(seed.series_key, static_cast<std::size_t>(metric - spec_.task_spec.metrics.begin()),
                            basis_seed.basis_version);
    }

    if (!relation_rolling_config_.enable_routed_rolling) return;

    for (const auto& routed_seed : seed.relation_routed_summary_seeds) {
        const uint64_t fallback_basis_version = BasisVersionForMetric(seed, routed_seed.metric_name);
        RelationRoutedBootstrapSeedMaterialization materialized;
        const BaselineStatus status = MaterializeRelationRoutedBootstrapSeed(spec_, seed.series_key, routed_seed,
                                                                             fallback_basis_version, &materialized);
        if (status != BaselineStatus::kOk) continue;

        RelationRoutedRuntimeShard& shard = *routed_shards_[RoutedShardIndex(materialized.routed_series_key)];
        shard.routed_seeds_by_series.insert_or_assign(materialized.routed_series_key, std::move(materialized.seed));
        const auto inserted = shard.routed_specs_by_series.insert_or_assign(materialized.routed_series_key,
                                                                            std::move(materialized.task_spec));
        if (inserted.second) {
            const auto metric =
                std::find(spec_.task_spec.metrics.begin(), spec_.task_spec.metrics.end(), routed_seed.metric_name);
            TrackManagedRouted(
                seed.series_key, static_cast<std::size_t>(metric - spec_.task_spec.metrics.begin()),
                IsBasisScopedRelationSummary(routed_seed.summary_name)
                    ? (routed_seed.basis_version > 0 ? routed_seed.basis_version : fallback_basis_version)
                    : 0,
                RoutedShardIndex(materialized.routed_series_key), inserted.first->first);
        }
    }
}

RelationBasisRuntimeConfig BaselineRelationTask::MakeBasisRuntimeConfig() const {
    RelationBasisRuntimeConfig config;
    const int64_t delta = spec_.clock_spec.delta > 0 ? spec_.clock_spec.delta : 60;
    const uint64_t day_buckets = static_cast<uint64_t>(std::max<int64_t>(1, 86400 / delta));
    const auto& relation_config = relation_rolling_config_;
    config.stream.max_groups = static_cast<std::size_t>(relation_config.basis_stats_max_groups);
    config.stream.threshold_margin = relation_config.basis_threshold_margin;
    config.collect_min_buckets =
        relation_config.basis_collect_min_buckets == 0 ? day_buckets : relation_config.basis_collect_min_buckets;
    config.ready_min_buckets =
        relation_config.basis_ready_min_buckets == 0 ? 3 * day_buckets : relation_config.basis_ready_min_buckets;
    config.refresh_interval_buckets =
        relation_config.basis_refresh_interval_buckets == 0
            ? day_buckets
            : relation_config.basis_refresh_interval_buckets;
    config.candidate_min_coverage_ratio =
        relation_config.basis_candidate_min_coverage_ratio;
    config.replacement_cap_ratio = relation_config.basis_replacement_cap_ratio;
    config.replacement_cap_max = relation_config.basis_replacement_cap_max;
    config.handover_warmup_buckets =
        relation_config.basis_handover_warmup_buckets == 0
            ? day_buckets
            : relation_config.basis_handover_warmup_buckets;
    config.min_stable_refresh_count = relation_config.basis_min_stable_refresh_count;
    return config;
}

RelationFusionRuntimeConfig BaselineRelationTask::MakeFusionRuntimeConfig() const {
    RelationFusionRuntimeConfig config;
    const auto& fusion = relation_rolling_config_.relation_fusion;
    config.enable_relation_fusion = fusion.enable_relation_fusion;
    config.fusion_z_score_cap = fusion.fusion_z_score_cap;
    config.fusion_min_evidence_score = fusion.fusion_min_evidence_score;
    config.fusion_persistence_window = fusion.fusion_persistence_window;
    config.fusion_warming_weight = fusion.fusion_warming_weight;
    config.fusion_degraded_weight = fusion.fusion_degraded_weight;
    config.fusion_support_weight = fusion.fusion_support_weight;
    config.fusion_oppose_weight = fusion.fusion_oppose_weight;
    config.basic_pattern_weight = fusion.basic_pattern_weight;
    config.stable_head_pattern_weight = fusion.stable_head_pattern_weight;
    config.dominant_single_cap = fusion.dominant_single_cap;
    config.dominant_pattern_cap = fusion.dominant_pattern_cap;
    config.fusion_persistence_max_keys_per_source =
        fusion.fusion_persistence_max_keys_per_source;
    return config;
}

bool BaselineRelationTask::IsFusionStateExpired(
    const RelationFusionRuntimeState& state) const {
    const uint64_t ttl = relation_rolling_config_.relation_fusion.fusion_state_ttl_buckets;
    return state.has_last_bucket &&
           fusion_cleanup_watermark_bucket_id_ > state.last_touched_bucket_id &&
           static_cast<uint64_t>(fusion_cleanup_watermark_bucket_id_ -
                                 state.last_touched_bucket_id) > ttl;
}

void BaselineRelationTask::MaybeCleanupFusionStates(
    std::string_view current_source,
    bool make_room_for_current_source) {
    fusion_cleanup_last_scan_count_ = 0;
    fusion_cleanup_last_evicted_count_ = 0;
    if (fusion_states_.empty()) return;
    const std::string current_key(current_source);

    const auto& fusion_config = relation_rolling_config_.relation_fusion;
    const uint64_t max_sources = fusion_config.fusion_state_max_sources;
    const uint64_t target_size =
        make_room_for_current_source && max_sources > 0 ? max_sources - 1 : max_sources;
    const uint64_t scan_limit = fusion_config.fusion_state_cleanup_scan_limit;
    const std::size_t bucket_count = fusion_states_.bucket_count();
    if (bucket_count == 0 || scan_limit == 0) return;
    if (fusion_cleanup_bucket_cursor_ >= bucket_count) {
        fusion_cleanup_bucket_cursor_ = 0;
    }

    std::vector<std::string> expired_keys;
    std::string oldest_key;
    uint64_t oldest_update_seq = 0;
    bool has_oldest = false;
    std::size_t visited_buckets = 0;

    while (fusion_cleanup_last_scan_count_ < scan_limit &&
           visited_buckets < bucket_count) {
        const std::size_t bucket = fusion_cleanup_bucket_cursor_;
        fusion_cleanup_bucket_cursor_ = (fusion_cleanup_bucket_cursor_ + 1) % bucket_count;
        ++visited_buckets;
        for (auto it = fusion_states_.begin(bucket);
             it != fusion_states_.end(bucket) &&
             fusion_cleanup_last_scan_count_ < scan_limit;
             ++it) {
            ++fusion_cleanup_last_scan_count_;
            if (it->first == current_key) continue;
            if (IsFusionStateExpired(it->second)) {
                expired_keys.push_back(it->first);
                continue;
            }
            if (!has_oldest ||
                it->second.last_touched_update_seq < oldest_update_seq) {
                oldest_key = it->first;
                oldest_update_seq = it->second.last_touched_update_seq;
                has_oldest = true;
            }
        }
    }

    for (const std::string& key : expired_keys) {
        const auto it = fusion_states_.find(key);
        if (it == fusion_states_.end()) continue;
        fusion_states_.erase(it);
        ++fusion_state_evicted_total_;
        ++fusion_state_evicted_ttl_total_;
        ++fusion_cleanup_last_evicted_count_;
    }

    if (fusion_states_.size() > target_size && has_oldest) {
        const auto it = fusion_states_.find(oldest_key);
        if (it != fusion_states_.end() && it->first != current_key) {
            fusion_states_.erase(it);
            ++fusion_state_evicted_total_;
            ++fusion_state_evicted_capacity_total_;
            ++fusion_cleanup_last_evicted_count_;
        }
    }
}

void BaselineRelationTask::ResetFusionCleanupRuntime() {
    fusion_update_seq_ = 0;
    fusion_cleanup_bucket_cursor_ = 0;
    fusion_state_evicted_total_ = 0;
    fusion_state_evicted_ttl_total_ = 0;
    fusion_state_evicted_capacity_total_ = 0;
    fusion_persistence_key_evicted_total_ = 0;
    fusion_cleanup_last_scan_count_ = 0;
    fusion_cleanup_last_evicted_count_ = 0;
    fusion_cleanup_watermark_bucket_id_ = 0;
}

RelationRollingResult BaselineRelationTask::SubmitObservation(
    const RelationRollingObservation& obs,
    const RelationRollingSubmitOptions& options) {
    RelationRollingResult result;
    result.series_key = obs.series_key;
    result.bucket_id = obs.bucket_id;
    result.status = EnsureOpen();
    if (result.status != BaselineStatus::kOk) return result;
    MarkStateOperation();
    if (obs.series_key.empty()) {
        result.status = BaselineStatus::kInvalidArgument;
        return result;
    }

    if (!HasIdentityCapacity(obs.series_key, false)) {
        result.status = BaselineStatus::kInvalidArgument;
        AppendDiagnostic(options.include_diagnostics, "baseline_state_capacity_reached", &result.diagnostics);
        return result;
    }

    auto cursor_it = last_processed_by_source_.find(obs.series_key);
    if (cursor_it != last_processed_by_source_.end() && obs.bucket_id <= cursor_it->second) {
        result.status = BaselineStatus::kInvalidArgument;
        return result;
    }

    const BootstrapSeed* restore_seed = nullptr;
    if (StateLimits() && cursor_it == last_processed_by_source_.end()) {
        const auto seed = seeds_by_series_.find(obs.series_key);
        if (seed != seeds_by_series_.end()) {
            restore_seed = &seed->second;
            if (obs.bucket_id <= restore_seed->coverage_report.train_end_bucket) {
                result.status = BaselineStatus::kInvalidArgument;
                return result;
            }
        }
    }

    const RelationBasisRuntimeConfig runtime_config = MakeBasisRuntimeConfig();
    const bool routed_enabled = relation_rolling_config_.enable_routed_rolling;
    const bool stream_basis_enabled =
        relation_rolling_config_.enable_stream_basis && options.allow_basis_update;
    bool saw_ok_routed_result = false;
    bool saw_ok_basis_result = false;
    bool saw_valid_metric = false;
    bool advanced_fusion = false;
    bool versions_changed = false;
    const std::string feature_base = spec_.task_spec.feature_base.empty()
                                         ? spec_.task_spec.feature_id
                                         : spec_.task_spec.feature_base;
    std::vector<RelationFusionMetricContext> fusion_metric_contexts;
    std::vector<RelationFusionRoutedInput> fusion_inputs;

    struct PreparedRelationMetric {
        std::size_t metric_index;
        std::string metric_name;
        RelationBasisStatus basis_status;
        uint64_t basis_version;
        std::vector<RelationProjectedSummary> summaries;
    };
    const std::size_t metric_count = spec_.task_spec.metrics.size();
    std::vector<PreparedRelationMetric> prepared_metrics;
    prepared_metrics.reserve(metric_count);
    fusion_metric_contexts.reserve(metric_count);
    for (std::size_t metric_index = 0; metric_index < metric_count; ++metric_index) {
        const bool metric_present = metric_index < obs.metrics.size();
        const RelationBootstrapMetric* metric =
            metric_present ? &obs.metrics[metric_index] : nullptr;
        const std::string metric_name = MetricNameForIndex(spec_, metric_index);
        if (metric_name.empty()) continue;

        RelationFusionMetricContext fusion_metric;
        fusion_metric.metric = metric_name;
        fusion_metric.present = metric_present;
        fusion_metric.valid = metric_present && IsValidRelationMetricHeader(*metric, obs.group_idx.size(), metric_name);
        fusion_metric.active_count_from_upstream =
            metric_present && metric->active_count > 0;
        if (!fusion_metric.present) {
            fusion_metric.unavailable_reason = "metric_missing";
            fusion_metric_contexts.push_back(std::move(fusion_metric));
            continue;
        }
        if (!fusion_metric.valid) {
            result.status = BaselineStatus::kInvalidArgument;
            return result;
        }
        saw_valid_metric = true;

        std::optional<RelationServiceBasis> active_basis;
        RelationBasisStatus basis_status = RelationBasisStatus::kCollecting;
        uint64_t active_basis_version = 0;
        const auto basis_it = basis_states_.find(BasisStateKey(obs.series_key, metric_name));
        if (basis_it != basis_states_.end()) {
            basis_status = basis_it->second.basis_status();
            if (const RelationServiceBasis* basis = basis_it->second.active_basis()) {
                active_basis = *basis;
                active_basis_version = basis->basis_version;
            }
        } else if (restore_seed) {
            for (const auto& basis_seed : restore_seed->relation_basis_by_metric) {
                if (basis_seed.metric_name != metric_name) continue;
                active_basis = BasisFromSeed(basis_seed);
                active_basis_version = basis_seed.basis_version;
                basis_status = BasisStatusFromSeedStatus(restore_seed->seed_status);
                break;
            }
        }
        fusion_metric.has_active_basis = active_basis.has_value();
        fusion_metric.basis_version = active_basis_version;
        if (active_basis) {
            fusion_metric.stable_head_size =
                static_cast<uint32_t>(active_basis->stable_head.size());
            fusion_metric.stable_head_mix_drift_expected =
                active_basis->stable_head.size() >= 2 &&
                active_basis->head_proto_q.size() == active_basis->stable_head.size();
        }
        fusion_metric_contexts.push_back(fusion_metric);
        if (result.basis_status.empty()) {
            result.basis_status = RelationBasisStatusName(basis_status);
            result.basis_version = active_basis_version;
        }

        PreparedRelationMetric prepared{metric_index, metric_name, basis_status, active_basis_version, {}};
        if (routed_enabled &&
            (active_basis ||
             relation_rolling_config_.include_universal_summaries_without_basis)) {
            RelationSummaryProjectionOptions summary_options;
            summary_options.summary_policy = spec_.task_spec.summary_policy;
            summary_options.other_group_idxs = spec_.task_spec.other_group_idxs;
            summary_options.basis = active_basis ? &(*active_basis) : nullptr;
            summary_options.include_basis_scoped = active_basis.has_value();
            if (!ProjectRelationMetricSummaries(obs, metric_index, metric_name, summary_options, &prepared.summaries)) {
                result.status = BaselineStatus::kInvalidArgument;
                return result;
            }
        } else if (!IsValidRelationMetric(*metric, obs.group_idx.size(), metric_name)) {
            result.status = BaselineStatus::kInvalidArgument;
            return result;
        }
        prepared_metrics.push_back(std::move(prepared));
    }

    RollingEventContext events{compiled_event_calendar_.get()};
    // Every present metric has passed validation before any child state changes.
    if (restore_seed && prepared_metrics.empty() &&
        !(routed_enabled && MakeFusionRuntimeConfig().enable_relation_fusion)) {
        result.status = BaselineStatus::kInvalidArgument;
        return result;
    }
    if (restore_seed) {
        InitializeRuntimeFromRelationSeed(*restore_seed);
        cursor_it = last_processed_by_source_.find(obs.series_key);
    }
    for (const auto& prepared : prepared_metrics) {
        const std::size_t metric_index = prepared.metric_index;
        const std::string& metric_name = prepared.metric_name;
        const RelationBasisStatus basis_status = prepared.basis_status;
        const uint64_t active_basis_version = prepared.basis_version;
        if (!prepared.summaries.empty()) {
            for (const RelationProjectedSummary& summary : prepared.summaries) {
                const RelationRoutedSummaryIdentity identity =
                    MakeRelationRoutedSummaryIdentity(obs.series_key,
                                                      metric_name,
                                                      summary.summary_name,
                                                      summary.task_kind,
                                                      summary.basis_version);
                RelationRoutedRuntimeShard& shard =
                    *routed_shards_[RoutedShardIndex(identity.routed_series_key)];
                auto spec_it = shard.routed_specs_by_series.find(identity.routed_series_key);
                if (spec_it == shard.routed_specs_by_series.end()) {
                    spec_it = shard.routed_specs_by_series
                                  .emplace(identity.routed_series_key,
                                           MakeRoutedSummaryTaskSpec(spec_,
                                                                    metric_name,
                                                                    summary.summary_name,
                                                                    summary.task_kind))
                                  .first;
                    TrackManagedRouted(obs.series_key, metric_index, identity.basis_version,
                                       RoutedShardIndex(identity.routed_series_key), spec_it->first);
                    versions_changed = true;
                }

                RollingBaselineResult rolling;
                if (summary.task_kind == BaselineTaskKind::kValue) {
                    ValueRollingObservation routed_obs;
                    routed_obs.series_key = identity.routed_series_key;
                    routed_obs.bucket_id = obs.bucket_id;
                    routed_obs.value = summary.value;
                    routed_obs.sample_count = 1;
                    rolling = RunValueRollingSubmit(spec_it->second, shard.routed_seeds_by_series,
                                                    &shard.routed_rolling_states, routed_obs, options.routed_options,
                                                    &events);
                } else if (summary.task_kind == BaselineTaskKind::kRatio) {
                    RatioRollingObservation routed_obs;
                    routed_obs.series_key = identity.routed_series_key;
                    routed_obs.bucket_id = obs.bucket_id;
                    routed_obs.numerator = summary.numerator;
                    routed_obs.denominator = summary.denominator;
                    rolling = RunRatioRollingSubmit(spec_it->second, shard.routed_seeds_by_series,
                                                    &shard.routed_rolling_states, routed_obs, options.routed_options,
                                                    &events);
                } else {
                    continue;
                }

                if (identity.basis_scoped && basis_status != RelationBasisStatus::kBasisReady) {
                    rolling.can_alert = false;
                    AppendDiagnostic(options.include_diagnostics,
                                     "relation_basis_not_ready",
                                     &rolling.diagnostics);
                }
                if (rolling.status == BaselineStatus::kOk) saw_ok_routed_result = true;

                RelationRoutedSummaryResult routed_result;
                routed_result.source_series_key = obs.series_key;
                routed_result.routed_series_key = identity.routed_series_key;
                routed_result.metric = metric_name;
                routed_result.summary = summary.summary_name;
                routed_result.feature_type = identity.feature_type;
                routed_result.basis_version = identity.basis_version;
                routed_result.basis_scoped = identity.basis_scoped;
                routed_result.rolling = rolling;

                RelationFusionRoutedInput fusion_input;
                fusion_input.routed = routed_result;
                fusion_input.feature_base = feature_base;
                fusion_input.metric_basis_status = RelationBasisStatusName(basis_status);
                fusion_input.active_count_from_upstream =
                    summary.active_count_from_upstream;
                fusion_inputs.push_back(std::move(fusion_input));

                if (options.include_routed_results) {
                    result.routed_results.push_back(std::move(routed_result));
                }
            }
        } else if (!routed_enabled) {
            AppendDiagnostic(options.include_diagnostics,
                             "relation_routed_rolling_disabled",
                             &result.diagnostics);
        }

        if (stream_basis_enabled) {
            const std::string state_key = BasisStateKey(obs.series_key, metric_name);
            auto basis_it = basis_states_.find(state_key);
            if (basis_it == basis_states_.end()) {
                auto inserted = basis_states_.emplace(
                    state_key, RelationBasisRuntimeState(runtime_config));
                basis_it = inserted.first;
                if (StateLimits()) TrackManagedSource(obs.series_key);
            }
            const BaselineStatus observe_status = basis_it->second.ObserveValidated(obs, metric_index);
            if (observe_status == BaselineStatus::kOk) {
                saw_ok_basis_result = true;
                RelationBasisBuildInput build_input =
                    MakeBasisBuildInput(spec_, metric_name, active_basis_version + 1);
                const RelationBasisRefreshDecision decision =
                    basis_it->second.MaybeRefresh(build_input, obs.bucket_id);
                if (decision.status == BaselineStatus::kOk) {
                    if (decision.basis_updated) {
                        TrackManagedVersion(obs.series_key, metric_index, decision.basis_version);
                        versions_changed = true;
                    }
                    result.basis_updated = result.basis_updated || decision.basis_updated;
                    result.handover_active =
                        result.handover_active ||
                        decision.basis_status == RelationBasisStatus::kHandoverWarming;
                    result.basis_version = decision.basis_version;
                    result.basis_status = RelationBasisStatusName(decision.basis_status);
                }
            }
            if (result.basis_status.empty()) {
                result.basis_status =
                    RelationBasisStatusName(basis_it->second.basis_status());
                if (const RelationServiceBasis* basis = basis_it->second.active_basis()) {
                    result.basis_version = basis->basis_version;
                }
            }
        } else {
            AppendDiagnostic(options.include_diagnostics,
                             "relation_stream_basis_disabled",
                             &result.diagnostics);
        }
    }

    const bool has_fusion_routed_inputs = !fusion_inputs.empty();
    const RelationFusionRuntimeConfig fusion_config = MakeFusionRuntimeConfig();
    bool has_existing_fusion_state = false;
    if (routed_enabled && fusion_config.enable_relation_fusion && !has_fusion_routed_inputs) {
        has_existing_fusion_state = fusion_states_.find(obs.series_key) != fusion_states_.end();
    }
    if (routed_enabled && fusion_config.enable_relation_fusion &&
        !fusion_metric_contexts.empty() &&
        (has_fusion_routed_inputs || !saw_valid_metric || has_existing_fusion_state)) {
        if (obs.bucket_id > fusion_cleanup_watermark_bucket_id_) {
            fusion_cleanup_watermark_bucket_id_ = obs.bucket_id;
        }
        const bool is_new_fusion_source =
            fusion_states_.find(obs.series_key) == fusion_states_.end();
        bool ran_pre_insert_cleanup = false;
        if (is_new_fusion_source &&
            fusion_states_.size() >=
                relation_rolling_config_.relation_fusion.fusion_state_max_sources) {
            MaybeCleanupFusionStates(obs.series_key, true);
            ran_pre_insert_cleanup = true;
        }

        RelationFusionUpdateInput fusion_update;
        fusion_update.source_series_key = obs.series_key;
        fusion_update.feature_base = feature_base;
        fusion_update.bucket_id = obs.bucket_id;
        fusion_update.config = fusion_config;
        fusion_update.metrics = std::move(fusion_metric_contexts);
        fusion_update.routed_inputs = std::move(fusion_inputs);

        RelationFusionResult fusion_result;
        RelationFusionRuntimeState& fusion_state = fusion_states_[obs.series_key];
        uint64_t evicted_persistence_keys = 0;
        const BaselineStatus fusion_status =
            UpdateRelationFusion(fusion_update,
                                 &fusion_state,
                                 &fusion_result,
                                 &evicted_persistence_keys);
        fusion_persistence_key_evicted_total_ += evicted_persistence_keys;
        if (fusion_status != BaselineStatus::kOk) {
            if (is_new_fusion_source && !fusion_state.has_last_bucket &&
                fusion_state.persistence_by_evidence_dir.empty()) {
                fusion_states_.erase(obs.series_key);
            }
            AppendDiagnostic(options.include_diagnostics,
                             "relation_fusion_update_failed",
                             &result.diagnostics);
        } else if (fusion_state.has_last_bucket &&
                   fusion_state.last_bucket_id == obs.bucket_id) {
            advanced_fusion = true;
            fusion_state.last_touched_bucket_id = obs.bucket_id;
            fusion_state.last_touched_update_seq = ++fusion_update_seq_;
            const bool over_capacity =
                fusion_states_.size() >
                relation_rolling_config_.relation_fusion.fusion_state_max_sources;
            const bool interval_due =
                fusion_update_seq_ %
                    relation_rolling_config_.relation_fusion
                        .fusion_state_cleanup_interval_updates ==
                0;
            if (over_capacity || (interval_due && !ran_pre_insert_cleanup)) {
                MaybeCleanupFusionStates(obs.series_key,
                                         over_capacity);
            }
        }
        if (options.include_fusion_result &&
            has_fusion_routed_inputs &&
            fusion_result.status == BaselineStatus::kOk) {
            result.has_fusion_result = true;
            result.fusion_result = std::move(fusion_result);
        }
    }

    if (!saw_ok_routed_result && !saw_ok_basis_result && !saw_valid_metric) {
        result.status = BaselineStatus::kInvalidArgument;
    } else {
        result.status = BaselineStatus::kOk;
    }
    if (result.status == BaselineStatus::kOk || advanced_fusion) {
        if (StateLimits() && cursor_it == last_processed_by_source_.end()) TrackManagedSource(obs.series_key);
        if (cursor_it != last_processed_by_source_.end()) {
            cursor_it->second = obs.bucket_id;
        } else {
            last_processed_by_source_.emplace(obs.series_key, obs.bucket_id);
        }
        if (versions_changed) RetireManagedVersions(obs.series_key);
    }
    return result;
}

RollingPrediction BaselineRelationTask::PredictRoutedSummary(
    const RelationRoutedSummaryQuery& query,
    int64_t bucket_id) const {
    RollingPrediction prediction;
    if (query.source_series_key.empty() || query.metric.empty() || query.summary.empty()) {
        prediction.status = BaselineStatus::kInvalidArgument;
        return prediction;
    }
    prediction.status = EnsureOpen();
    if (prediction.status != BaselineStatus::kOk) return prediction;

    uint64_t basis_version = query.basis_version;
    if (IsBasisScopedRelationSummary(query.summary) && basis_version == 0) {
        const auto it = basis_states_.find(BasisStateKey(query.source_series_key, query.metric));
        if (it == basis_states_.end() || !it->second.active_basis()) {
            prediction.status = BaselineStatus::kNotTrained;
            return prediction;
        }
        basis_version = it->second.active_basis()->basis_version;
    }

    const BaselineTaskKind task_kind = TaskKindFromFeatureType(query.feature_type);
    const RelationRoutedSummaryIdentity identity =
        MakeRelationRoutedSummaryIdentity(query.source_series_key,
                                          query.metric,
                                          query.summary,
                                          task_kind,
                                          basis_version);
    const RelationRoutedRuntimeShard& shard =
        *routed_shards_[RoutedShardIndex(identity.routed_series_key)];
    const auto spec_it = shard.routed_specs_by_series.find(identity.routed_series_key);
    if (spec_it == shard.routed_specs_by_series.end()) {
        prediction.status = BaselineStatus::kNotTrained;
        return prediction;
    }
    return PredictRollingForSeries(spec_it->second, shard.routed_seeds_by_series, shard.routed_rolling_states,
                                   identity.routed_series_key, bucket_id, compiled_event_calendar_.get());
}

BaselineSerializationResult BaselineRelationTask::QueryRoutedSummarySnapshot(
    const RelationRoutedSummaryQuery& query,
    BaselineSerializationFormat format) const {
    if (format != BaselineSerializationFormat::kJson) {
        return {BaselineStatus::kUnsupportedFormat, ""};
    }
    if (query.source_series_key.empty() || query.metric.empty() || query.summary.empty()) {
        return {BaselineStatus::kInvalidArgument, ""};
    }
    const BaselineStatus status = EnsureOpen();
    if (status != BaselineStatus::kOk) return {status, ""};

    uint64_t basis_version = query.basis_version;
    if (IsBasisScopedRelationSummary(query.summary) && basis_version == 0) {
        const auto it = basis_states_.find(BasisStateKey(query.source_series_key, query.metric));
        if (it == basis_states_.end() || !it->second.active_basis()) {
            return {BaselineStatus::kNotTrained, ""};
        }
        basis_version = it->second.active_basis()->basis_version;
    }

    const BaselineTaskKind task_kind = TaskKindFromFeatureType(query.feature_type);
    const RelationRoutedSummaryIdentity identity =
        MakeRelationRoutedSummaryIdentity(query.source_series_key,
                                          query.metric,
                                          query.summary,
                                          task_kind,
                                          basis_version);
    const RelationRoutedRuntimeShard& shard =
        *routed_shards_[RoutedShardIndex(identity.routed_series_key)];
    const auto spec_it = shard.routed_specs_by_series.find(identity.routed_series_key);
    if (spec_it == shard.routed_specs_by_series.end()) {
        return {BaselineStatus::kNotTrained, ""};
    }
    return QueryRollingSeriesSnapshot(spec_it->second,
                                      shard.routed_rolling_states,
                                      identity.routed_series_key,
                                      format);
}

BootstrapTrainResult BaselineRelationTask::Bootstrap(const RelationBootstrapInput& input) {
    BootstrapTrainResult result;
    result.status = EnsureOpen();
    if (result.status != BaselineStatus::kOk) return result;
    MarkStateOperation();
    if (!HasIdentityCapacity(input.series_key, true)) {
        result.status = BaselineStatus::kInvalidArgument;
        if (input.options.include_diagnostics) result.diagnostics = "baseline_state_capacity_reached";
        return result;
    }
    if (!input.options.force_replace_existing_artifact &&
        FindBootstrapArtifact(artifacts_by_series_, input.series_key)) {
        result.status = BaselineStatus::kInvalidArgument;
        if (input.options.include_diagnostics) {
            result.diagnostics = "bootstrap artifact already exists for series_key";
        }
        return result;
    }
    BootstrapArtifact artifact;
    result = bootstrap_engine_.TrainRelation(
        spec_, input, &artifact, compiled_event_calendar_.get());
    if (result.status == BaselineStatus::kOk) {
        const BaselineStatus seed_status =
            StoreBootstrapArtifact(input.series_key,
                                   std::move(artifact),
                                   bootstrap_engine_,
                                   &artifacts_by_series_,
                                   &seeds_by_series_);
        if (seed_status != BaselineStatus::kOk) {
            result.status = seed_status;
            return result;
        }
        RebuildRuntimeForSource(input.series_key);
    }
    return result;
}

BaselineSerializationResult BaselineRelationTask::ExportBootstrapArtifact(
    BaselineSerializationFormat format) const {
    const BaselineStatus status = EnsureOpen();
    if (status != BaselineStatus::kOk) return {status, ""};
    return ExportBootstrapArtifactStore(artifacts_by_series_, bootstrap_engine_, format);
}

BaselineStatus BaselineRelationTask::LoadBootstrapArtifact(
    std::string_view content,
    BaselineSerializationFormat format) {
    const BaselineStatus status = EnsureOpen();
    if (status != BaselineStatus::kOk) return status;
    MarkStateOperation();
    BootstrapArtifactStore loaded_artifacts;
    BootstrapSeedStore loaded_seeds;
    const BaselineStatus load_status =
        LoadRelationBootstrapArtifactStore(content, format, bootstrap_engine_, spec_, &loaded_artifacts, &loaded_seeds);
    if (load_status == BaselineStatus::kOk) {
        const auto* limits = StateLimits();
        if (limits && (loaded_artifacts.size() > limits->max_model_identities ||
                       loaded_seeds.size() > limits->max_runtime_identities || !SeedsFitRuntimeLimits(loaded_seeds))) {
            return BaselineStatus::kInvalidArgument;
        }
        artifacts_by_series_.swap(loaded_artifacts);
        seeds_by_series_.swap(loaded_seeds);
        RebuildRuntimeFromRelationSeeds();
    }
    return load_status;
}

BaselineSerializationResult BaselineRelationTask::ExportBootstrapSeed(
    BaselineSerializationFormat format) const {
    const BaselineStatus status = EnsureOpen();
    if (status != BaselineStatus::kOk) return {status, ""};
    return ExportBootstrapSeedStore(seeds_by_series_, bootstrap_engine_, format);
}

BaselineSerializationResult BaselineRelationTask::QueryBootstrapBasis(
    BaselineSerializationFormat format) const {
    const BaselineStatus status = EnsureOpen();
    if (status != BaselineStatus::kOk) return {status, ""};
    if (format != BaselineSerializationFormat::kJson) {
        return {BaselineStatus::kUnsupportedFormat, ""};
    }
    if (artifacts_by_series_.empty()) {
        return {BaselineStatus::kNotTrained, ""};
    }

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buf);
    writer.StartObject();
    writer.Key("series_bases");
    writer.StartArray();
    for (const BootstrapArtifact* artifact : SortedBootstrapArtifacts(artifacts_by_series_)) {
        if (!artifact) continue;
        writer.StartObject();
        writer.Key("series_identity");
        writer.StartObject();
        writer.Key("series_key");
        writer.String(artifact->series_key.c_str());
        writer.EndObject();
        writer.Key("basis_by_metric");
        writer.StartArray();
        for (const auto& basis : artifact->relation_basis_by_metric) {
            writer.StartObject();
            writer.Key("metric");
            writer.String(basis.metric_name.c_str());
            writer.Key("support_explicit");
            writer.StartArray();
            for (uint32_t group_idx : basis.support_explicit) writer.Uint(group_idx);
            writer.EndArray();
            writer.Key("stable_head");
            writer.StartArray();
            for (uint32_t group_idx : basis.stable_head) writer.Uint(group_idx);
            writer.EndArray();
            writer.EndObject();
        }
        writer.EndArray();
        writer.EndObject();
    }
    writer.EndArray();
    writer.EndObject();
    return {BaselineStatus::kOk, buf.GetString()};
}

}  // namespace baseline
}  // namespace flowsql
