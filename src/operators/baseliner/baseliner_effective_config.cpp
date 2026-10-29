// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <type_traits>
#include "baseliner_contract.h"

namespace flowsql::baseliner {
namespace {
using Writer = rapidjson::Writer<rapidjson::StringBuffer>;
void Text(Writer& w, const char* key, const std::string& value) {
    w.Key(key);
    w.String(value.data(), value.size());
}
void Number(Writer& w, const char* key, uint64_t value) {
    w.Key(key);
    w.Uint64(value);
}
void Signed(Writer& w, const char* key, int64_t value) {
    w.Key(key);
    w.Int64(value);
}
void Texts(Writer& w, const char* key, const std::vector<std::string>& values) {
    w.Key(key);
    w.StartArray();
    for (const auto& value : values) w.String(value.data(), value.size());
    w.EndArray();
}
void Scalar(Writer& w, const KeyValue& value) {
    std::visit(
        [&](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, bool>)
                w.Bool(v);
            else if constexpr (std::is_same_v<T, int64_t>)
                w.Int64(v);
            else if constexpr (std::is_same_v<T, uint64_t>)
                w.Uint64(v);
            else if constexpr (std::is_same_v<T, double>)
                w.Double(v);
            else
                w.String(v.data(), v.size());
        },
        value);
}
void ExpressionFields(Writer& w, const Expression& value) {
    if (!value.column.empty()) Text(w, "column", value.column);
    const char* names[]{"column", "sum", "count", "mean", "min", "max"};
    Text(w, "aggregate", names[static_cast<unsigned>(value.aggregate)]);
    w.Key("scale");
    w.Double(value.scale);
    w.Key("divisor");
    w.Double(value.divisor);
}
void ExpressionObject(Writer& w, const char* key, const Expression& value) {
    w.Key(key);
    w.StartObject();
    ExpressionFields(w, value);
    w.EndObject();
}
void MetricObject(Writer& w, const Metric& m) {
    w.StartObject();
    Text(w, "id", m.id);
    Text(w, "kind",
         m.kind == BaselineTaskKind::kValue   ? "value"
         : m.kind == BaselineTaskKind::kRatio ? "ratio"
                                              : "relation");
    Text(w, "feature_type", m.feature_type);
    Text(w, "profile", m.profile);
    Text(w, "null_policy", m.null_policy == InvalidPolicy::kFail ? "fail" : "skip");
    Text(w, "invalid_policy", m.invalid_policy == InvalidPolicy::kFail ? "fail" : "skip");
    if (m.kind == BaselineTaskKind::kValue) {
        ExpressionFields(w, m.value);
        if (m.sample_count) ExpressionObject(w, "sample_count", *m.sample_count);
    } else if (m.kind == BaselineTaskKind::kRatio) {
        ExpressionObject(w, "numerator", m.numerator);
        ExpressionObject(w, "denominator", m.denominator);
    } else {
        const auto& g = *m.group_space;
        w.Key("group_space");
        w.StartObject();
        Text(w, "id", g.id);
        Text(w, "version", g.version);
        Text(w, "column", g.column);
        Text(w, "unknown", g.other ? "other" : "reject");
        if (g.other) Number(w, "other_group_idx", *g.other);
        if (!g.dictionary.empty()) {
            w.Key("dictionary");
            w.StartArray();
            for (const auto& [key, index] : g.dictionary) {
                w.StartObject();
                w.Key("key");
                Scalar(w, key);
                Number(w, "group_idx", index);
                w.EndObject();
            }
            w.EndArray();
        }
        w.EndObject();
        w.Key("metrics");
        w.StartArray();
        for (const auto& child : m.relation_metrics) {
            w.StartObject();
            Text(w, "id", child.id);
            ExpressionFields(w, child.value);
            w.EndObject();
        }
        w.EndArray();
        w.Key("support_policy");
        w.StartObject();
        Number(w, "k_support", m.k_support);
        w.Key("min_hist_share");
        w.Double(m.min_hist_share);
        w.Key("min_active_ratio");
        w.Double(m.min_active_ratio);
        w.EndObject();
        w.Key("summary_policy");
        w.StartObject();
        Number(w, "k_head", m.k_head);
        Number(w, "k_stable", m.k_stable);
        w.EndObject();
    }
    w.EndObject();
}
}  // namespace
std::string EffectiveConfigJson(const TaskConfig& c) {
    rapidjson::StringBuffer buffer;
    Writer w(buffer);
    w.StartObject();
    Number(w, "schema_version", c.schema_version);
    Text(w, "task_key", c.task_key);
    Text(w, "source", c.source);
    Text(w, "mode", c.mode == Mode::kSnapshot ? "snapshot" : "poll");
    w.Key("clock");
    w.StartObject();
    Signed(w, "bucket_seconds", c.bucket_seconds);
    Text(w, "timezone", c.timezone);
    w.EndObject();
    w.Key("calendar");
    w.StartObject();
    Text(w, "calendar_id", c.calendar_id);
    Text(w, "calendar_version", c.calendar_version);
    w.EndObject();
    w.Key("bootstrap");
    w.StartObject();
    Text(w, "mode", c.bootstrap.history ? "history" : "cold");
    if (c.bootstrap.history) {
        Signed(w, "begin_bucket", c.bootstrap.begin_bucket);
        Signed(w, "end_bucket", c.bootstrap.end_bucket);
        Number(w, "min_observations", c.bootstrap.min_observations);
        Text(w, "insufficient", c.bootstrap.insufficient_cold ? "cold" : "fail");
    }
    w.EndObject();
    w.Key("forecast");
    w.StartObject();
    Number(w, "horizon_buckets", c.horizon_buckets);
    w.EndObject();
    w.Key("read_policy");
    w.StartObject();
    Number(w, "page_rows", c.read.page_rows);
    Number(w, "poll_interval_ms", c.read.poll_interval_ms);
    Number(w, "max_pending_bytes", c.read.max_pending_bytes);
    w.EndObject();
    w.Key("state_policy");
    w.StartObject();
    Number(w, "max_runtime_identities", c.state.limits.max_runtime_identities);
    Number(w, "max_model_identities", c.state.limits.max_model_identities);
    Number(w, "max_basis_versions", c.state.limits.max_basis_versions_per_metric);
    Text(w, "capacity", c.state.evict_idle ? "evict_idle" : "reject");
    Text(w, "release_scope",
         c.state.release_scope == BaselineStateReleaseScopeV1::kRuntimeOnly ? "RuntimeOnly" : "AllState");
    Number(w, "idle_timeout_ms", c.state.idle_timeout_ms);
    Number(w, "maintenance_max_releases", c.state.maintenance_max_releases);
    Number(w, "maintenance_max_bytes", c.state.maintenance_max_bytes);
    w.EndObject();
    w.Key("persistence");
    w.StartObject();
    Number(w, "checkpoint_every_buckets", c.persistence.checkpoint_every_buckets);
    Number(w, "checkpoint_interval_ms", c.persistence.checkpoint_interval_ms);
    Number(w, "lease_ms", c.persistence.lease_ms);
    Number(w, "renew_ms", c.persistence.renew_ms);
    Number(w, "operation_timeout_ms", c.persistence.operation_timeout_ms);
    Number(w, "max_checkpoint_bytes", c.persistence.max_checkpoint_bytes);
    Number(w, "retain_generations", c.persistence.retain_generations);
    Text(w, "restore", c.persistence.restore);
    w.EndObject();
    w.Key("datasets");
    w.StartArray();
    for (const auto& d : c.datasets) {
        w.StartObject();
        Text(w, "id", d.id);
        if (!d.table.empty()) Text(w, "table", d.table);
        if (!d.schema.empty()) Text(w, "schema", d.schema);
        w.Key("fields");
        w.StartObject();
        const char* names[]{"boolean", "int64", "uint64", "float64", "utf8"};
        for (const auto& [name, type] : d.fields) Text(w, name.c_str(), names[static_cast<unsigned>(type)]);
        w.EndObject();
        w.Key("scope");
        w.StartObject();
        if (!d.scope.consistency.empty()) Text(w, "consistency", d.scope.consistency);
        if (!d.scope.run_ids.empty()) Texts(w, "run_ids", d.scope.run_ids);
        if (d.scope.begin_bucket) {
            Signed(w, "begin_bucket", *d.scope.begin_bucket);
            Signed(w, "end_bucket", *d.scope.end_bucket);
        }
        w.EndObject();
        Texts(w, "series_keys", d.series_keys);
        Text(w, "row_semantics", d.row_semantics);
        if (!d.revision_column.empty()) Text(w, "revision_column", d.revision_column);
        w.Key("deduplicate");
        w.StartObject();
        Texts(w, "keys", d.deduplicate_keys);
        Text(w, "on_duplicate", "require_equal");
        w.EndObject();
        w.Key("bucket");
        w.StartObject();
        Text(w, "column", d.bucket_column);
        const char* units[]{"bucket_id", "ns", "us", "ms", "s"};
        Text(w, "unit", units[static_cast<unsigned>(d.unit)]);
        w.EndObject();
        w.Key("fill_missing_zero");
        w.Bool(d.fill_missing_zero);
        w.Key("filter");
        w.StartArray();
        for (const auto& p : d.filter) {
            w.StartObject();
            Text(w, "column", p.column);
            Text(w, "op", p.op);
            w.Key("value");
            Scalar(w, p.value);
            w.EndObject();
        }
        w.EndArray();
        w.Key("metrics");
        w.StartArray();
        for (const auto& m : d.metrics) MetricObject(w, m);
        w.EndArray();
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    return {buffer.GetString(), buffer.GetSize()};
}
}  // namespace flowsql::baseliner
