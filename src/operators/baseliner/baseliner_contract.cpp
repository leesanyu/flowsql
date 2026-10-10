// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "baseliner_contract.h"

#include <arrow/api.h>
#include <openssl/sha.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <unicode/timezone.h>
#include <common/json_depth.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>

namespace flowsql::baseliner {
namespace {
using Json = rapidjson::Value;
struct Invalid {
    ConfigStatus status;
};
[[noreturn]] void Fail(ConfigError code, const std::string& path, const std::string& message) {
    throw Invalid{{code, path, message}};
}
void Require(bool condition, ConfigError code, const std::string& path, const std::string& message) {
    if (!condition) Fail(code, path, message);
}
std::string Text(const Json& value) { return {value.GetString(), value.GetStringLength()}; }
std::string Path(const std::string& parent, const std::string& token) {
    std::string result = parent + "/";
    for (char c : token) result += c == '~' ? "~0" : c == '/' ? "~1" : std::string(1, c);
    return result;
}
const Json* Find(const Json& value, const char* name) {
    auto it = value.FindMember(name);
    return it == value.MemberEnd() ? nullptr : &it->value;
}
const Json& Required(const Json& value, const char* name, const std::string& path) {
    const auto* child = Find(value, name);
    Require(child, ConfigError::kMissing, Path(path, name), "required field");
    return *child;
}
void Object(const Json& value, const std::string& path, std::initializer_list<std::string_view> allowed) {
    Require(value.IsObject(), ConfigError::kType, path, "expected object");
    for (auto it = value.MemberBegin(); it != value.MemberEnd(); ++it) {
        const auto name = Text(it->name);
        Require(std::find(allowed.begin(), allowed.end(), name) != allowed.end(), ConfigError::kUnknown,
                Path(path, name), "unknown field");
    }
}
void Tree(const Json& value, const std::string& path) {
    if (value.IsObject()) {
        std::set<std::string> names;
        for (auto it = value.MemberBegin(); it != value.MemberEnd(); ++it) {
            const auto name = Text(it->name);
            Require(names.insert(name).second, ConfigError::kDuplicate, Path(path, name), "duplicate key");
            Tree(it->value, Path(path, name));
        }
    } else if (value.IsArray()) {
        for (rapidjson::SizeType i = 0; i < value.Size(); ++i) Tree(value[i], Path(path, std::to_string(i)));
    }
}
void Document(std::string_view json, rapidjson::Document* doc, size_t max_bytes = kMaxConfigBytes) {
    Require(!json.empty() && json.size() <= max_bytes, ConfigError::kLimit, "", "JSON byte limit");
    Require(json.find('\0') == std::string_view::npos, ConfigError::kJson, "", "embedded NUL");
    Require(JsonNestingWithin(std::string(json), kMaxJsonDepth), ConfigError::kLimit, "", "JSON depth limit");
    doc->Parse<rapidjson::kParseValidateEncodingFlag>(json.data(), json.size());
    Require(!doc->HasParseError(), ConfigError::kJson, "", "invalid UTF-8 JSON");
    Tree(*doc, "");
}
std::string String(const Json& value, const std::string& path, size_t limit = 256) {
    Require(value.IsString(), ConfigError::kType, path, "expected string");
    auto result = Text(value);
    Require(!result.empty() && result.size() <= limit && result.find('\0') == std::string::npos, ConfigError::kValue,
            path, "empty, too long, or NUL string");
    return result;
}
std::string Str(const Json& value, const char* name, const std::string& path) {
    return String(Required(value, name, path), Path(path, name));
}
std::string OptStr(const Json& value, const char* name, const std::string& path, std::string fallback = {}) {
    auto child = Find(value, name);
    return child ? String(*child, Path(path, name)) : std::move(fallback);
}
uint64_t UInt(const Json& value, const std::string& path, uint64_t min, uint64_t max) {
    Require(value.IsUint64(), ConfigError::kType, path, "expected unsigned integer");
    const auto number = value.GetUint64();
    Require(number >= min && number <= max, ConfigError::kLimit, path, "integer outside finite bounds");
    return number;
}
uint64_t U(const Json& value, const char* name, const std::string& path, uint64_t fallback, uint64_t min,
           uint64_t max) {
    const auto child = Find(value, name);
    return child ? UInt(*child, Path(path, name), min, max) : fallback;
}
int64_t Int(const Json& value, const std::string& path) {
    Require(value.IsInt64(), ConfigError::kType, path, "expected int64");
    return value.GetInt64();
}
double Number(const Json& value, const std::string& path) {
    Require(value.IsNumber(), ConfigError::kType, path, "expected number");
    double number = value.GetDouble();
    Require(std::isfinite(number), ConfigError::kValue, path, "nonfinite number");
    return number;
}
double N(const Json& value, const char* name, const std::string& path, double fallback) {
    const auto child = Find(value, name);
    return child ? Number(*child, Path(path, name)) : fallback;
}
bool Bool(const Json& value, const char* name, const std::string& path, bool fallback) {
    const auto child = Find(value, name);
    if (!child) return fallback;
    Require(child->IsBool(), ConfigError::kType, Path(path, name), "expected boolean");
    return child->GetBool();
}
void OneOf(const std::string& value, std::initializer_list<std::string_view> allowed, const std::string& path) {
    Require(std::find(allowed.begin(), allowed.end(), value) != allowed.end(), ConfigError::kValue, path,
            "unsupported value");
}
std::vector<std::string> Strings(const Json& value, const std::string& path, uint32_t max, bool allow_empty = false) {
    Require(value.IsArray(), ConfigError::kType, path, "expected array");
    Require(value.Size() <= max && (allow_empty || !value.Empty()), ConfigError::kLimit, path, "array bounds");
    std::vector<std::string> result;
    std::set<std::string> seen;
    for (rapidjson::SizeType i = 0; i < value.Size(); ++i) {
        auto item = String(value[i], Path(path, std::to_string(i)));
        Require(seen.insert(item).second, ConfigError::kValue, path, "duplicate array identity");
        result.push_back(std::move(item));
    }
    return result;
}
bool Name(std::string_view text) {
    if (text.empty() || text.size() > 64) return false;
    for (char c : text)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return false;
    return true;
}
void BindSource(TaskConfig* config) {
    const auto dot = config->source.find('.');
    Require(dot != std::string::npos, ConfigError::kValue, "/source", "exact category.name[.table] source");
    const auto category = config->source.substr(0, dot);
    const auto relation_dot = config->source.find('.', dot + 1);
    const auto name =
        config->source.substr(dot + 1, relation_dot == std::string::npos ? std::string::npos : relation_dot - dot - 1);
    Require(Name(category) && Name(name), ConfigError::kValue, "/source", "invalid source identity");
    config->database_source =
        category == "sqlite" || category == "mysql" || category == "postgres" || category == "clickhouse";
    config->dataframe_source = category == "dataframe";
    if (relation_dot != std::string::npos) {
        config->source_relation = config->source.substr(relation_dot + 1);
        Require(config->database_source && Name(config->source_relation), ConfigError::kValue, "/source",
                "three-part source requires one database relation");
    }
}
uint64_t Reference(std::string_view ref) {
    constexpr std::string_view prefix = "config.";
    auto at = ref.find('@');
    Require(ref.substr(0, prefix.size()) == prefix && at != std::string_view::npos && at > prefix.size() &&
                Name(ref.substr(prefix.size(), at - prefix.size())) && ref[prefix.size()] >= 'a' &&
                ref[prefix.size()] <= 'z',
            ConfigError::kReference, "/config", "exact config reference");
    auto digits = ref.substr(at + 1);
    uint64_t revision = 0;
    Require(!digits.empty() && digits.front() != '0', ConfigError::kReference, "/config", "positive exact revision");
    auto result = std::from_chars(digits.data(), digits.data() + digits.size(), revision);
    Require(result.ec == std::errc{} && result.ptr == digits.data() + digits.size() && revision > 0 &&
                revision <= uint64_t(std::numeric_limits<int64_t>::max()),
            ConfigError::kReference, "/config", "exact revision");
    return revision;
}
std::string Hash(std::string_view value) {
    std::array<unsigned char, SHA256_DIGEST_LENGTH> bytes{};
    SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), bytes.data());
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (unsigned char byte : bytes) {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 15]);
    }
    return result;
}
LogicalType Type(const std::string& type, const std::string& path) {
    OneOf(type, {"boolean", "int64", "uint64", "float64", "utf8"}, path);
    if (type == "boolean") return LogicalType::kBoolean;
    if (type == "int64") return LogicalType::kInt64;
    if (type == "uint64") return LogicalType::kUInt64;
    if (type == "float64") return LogicalType::kFloat64;
    return LogicalType::kUtf8;
}
LogicalType Field(const Dataset& d, const std::string& name, const std::string& path) {
    auto it = d.fields.find(name);
    Require(it != d.fields.end(), ConfigError::kValue, path, "undeclared field: " + name);
    return it->second;
}
bool Numeric(LogicalType type) {
    return type == LogicalType::kInt64 || type == LogicalType::kUInt64 || type == LogicalType::kFloat64;
}
KeyValue TypedValue(const Json& value, LogicalType type, const std::string& path) {
    switch (type) {
        case LogicalType::kBoolean:
            Require(value.IsBool(), ConfigError::kType, path, "boolean predicate/key");
            return value.GetBool();
        case LogicalType::kInt64:
            return Int(value, path);
        case LogicalType::kUInt64:
            Require(value.IsUint64(), ConfigError::kType, path, "uint64 predicate/key");
            return value.GetUint64();
        case LogicalType::kFloat64:
            return Number(value, path);
        case LogicalType::kUtf8:
            Require(value.IsString(), ConfigError::kType, path, "utf8 predicate/key");
            return Text(value);
    }
    Fail(ConfigError::kType, path, "logical type");
}
Expression Expr(const Json& obj, const Dataset& d, const std::string& path, bool nested) {
    if (nested) Object(obj, path, {"column", "aggregate", "scale", "divisor"});
    Expression e;
    const auto aggregate = OptStr(obj, "aggregate", path, "column");
    OneOf(aggregate, {"column", "sum", "count", "mean", "min", "max"}, Path(path, "aggregate"));
    e.aggregate = aggregate == "sum"     ? Aggregate::kSum
                  : aggregate == "count" ? Aggregate::kCount
                  : aggregate == "mean"  ? Aggregate::kMean
                  : aggregate == "min"   ? Aggregate::kMin
                  : aggregate == "max"   ? Aggregate::kMax
                                         : Aggregate::kColumn;
    e.column = OptStr(obj, "column", path);
    if (e.column.empty())
        Require(e.aggregate == Aggregate::kCount, ConfigError::kMissing, Path(path, "column"), "column required");
    else {
        auto type = Field(d, e.column, Path(path, "column"));
        Require(e.aggregate == Aggregate::kCount || Numeric(type), ConfigError::kValue, path, "numeric expression");
    }
    e.scale = N(obj, "scale", path, 1.0);
    e.divisor = N(obj, "divisor", path, 1.0);
    Require(e.divisor > 0 && std::isfinite(e.scale / e.divisor), ConfigError::kValue, Path(path, "divisor"),
            "positive finite divisor");
    return e;
}
InvalidPolicy Policy(const Json& obj, const char* name, const std::string& path) {
    auto value = OptStr(obj, name, path, "fail");
    OneOf(value, {"fail", "skip"}, Path(path, name));
    return value == "skip" ? InvalidPolicy::kSkip : InvalidPolicy::kFail;
}
GroupSpace Groups(const Json& obj, const Dataset& d, const std::string& path) {
    Object(obj, path, {"id", "version", "column", "dictionary", "unknown", "other_group_idx"});
    GroupSpace groups;
    groups.id = Str(obj, "id", path);
    groups.version = Str(obj, "version", path);
    groups.column = Str(obj, "column", path);
    Require(groups.version != "latest", ConfigError::kReference, Path(path, "version"), "exact group version");
    const auto type = Field(d, groups.column, path);
    if (auto dict = Find(obj, "dictionary")) {
        Require(dict->IsArray(), ConfigError::kType, Path(path, "dictionary"), "dictionary array");
        Require(!dict->Empty() && dict->Size() <= kMaxGroupDictionary, ConfigError::kLimit, path, "dictionary bounds");
        std::set<std::string> keys;
        std::set<uint32_t> indices;
        for (rapidjson::SizeType i = 0; i < dict->Size(); ++i) {
            auto entry_path = Path(Path(path, "dictionary"), std::to_string(i));
            auto& entry = (*dict)[i];
            Object(entry, entry_path, {"key", "group_idx"});
            auto key = TypedValue(Required(entry, "key", entry_path), type, entry_path);
            uint32_t index = UInt(Required(entry, "group_idx", entry_path), entry_path, 0, UINT32_MAX);
            std::string encoded;
            Require(
                EncodeIdentity("group", groups.id, groups.version, BaselineTaskKind::kRelation, {key}, &encoded).ok(),
                ConfigError::kValue, entry_path, "dictionary key");
            Require(keys.insert(encoded).second && indices.insert(index).second, ConfigError::kValue, entry_path,
                    "duplicate group/key");
            groups.dictionary.emplace_back(std::move(key), index);
        }
    } else
        Require(type == LogicalType::kUInt64, ConfigError::kValue, path, "numeric group_idx requires uint64");
    const auto unknown = OptStr(obj, "unknown", path, "reject");
    OneOf(unknown, {"reject", "other"}, path);
    if (unknown == "other")
        groups.other = UInt(Required(obj, "other_group_idx", path), path, 0, UINT32_MAX);
    else
        Require(!Find(obj, "other_group_idx"), ConfigError::kValue, path, "other requires unknown=other");
    return groups;
}
Metric ParseMetric(const Json& obj, const Dataset& d, const std::string& path) {
    Object(obj, path,
           {"id", "kind", "feature_type", "profile", "null_policy", "invalid_policy", "column", "aggregate", "scale",
            "divisor", "sample_count", "numerator", "denominator", "group_space", "metrics", "support_policy",
            "summary_policy"});
    Metric m;
    m.id = Str(obj, "id", path);
    auto kind = Str(obj, "kind", path);
    OneOf(kind, {"value", "ratio", "relation"}, Path(path, "kind"));
    m.kind = kind == "value"   ? BaselineTaskKind::kValue
             : kind == "ratio" ? BaselineTaskKind::kRatio
                               : BaselineTaskKind::kRelation;
    m.feature_type = Str(obj, "feature_type", path);
    m.profile = Str(obj, "profile", path);
    m.null_policy = Policy(obj, "null_policy", path);
    m.invalid_policy = Policy(obj, "invalid_policy", path);
    if (m.kind == BaselineTaskKind::kValue) {
        OneOf(m.feature_type, {"value_basic", "value_sampled"}, Path(path, "feature_type"));
        m.value = Expr(obj, d, path, false);
        if (auto sample = Find(obj, "sample_count")) {
            m.sample_count = Expr(*sample, d, Path(path, "sample_count"), true);
            const auto& e = *m.sample_count;
            Require((e.aggregate == Aggregate::kCount ||
                     (!e.column.empty() && Field(d, e.column, path) == LogicalType::kUInt64 &&
                      (e.aggregate == Aggregate::kColumn || e.aggregate == Aggregate::kSum))) &&
                        e.scale == 1 && e.divisor == 1,
                    ConfigError::kValue, path, "lossless sample_count");
        }
        Require(m.feature_type != "value_sampled" || m.sample_count.has_value(), ConfigError::kValue, path,
                "sampled requires sample_count");
        Require(m.feature_type != "value_basic" || m.profile == "default", ConfigError::kValue, path,
                "basic profile is default");
    } else if (m.kind == BaselineTaskKind::kRatio) {
        Require(m.feature_type == "ratio", ConfigError::kValue, path, "ratio feature_type");
        m.numerator = Expr(Required(obj, "numerator", path), d, Path(path, "numerator"), true);
        m.denominator = Expr(Required(obj, "denominator", path), d, Path(path, "denominator"), true);
    } else {
        Require(m.feature_type == "relation" && m.profile == "default", ConfigError::kValue, path,
                "relation feature/profile");
        m.group_space = Groups(Required(obj, "group_space", path), d, Path(path, "group_space"));
        auto& metrics = Required(obj, "metrics", path);
        Require(metrics.IsArray(), ConfigError::kType, path, "relation metrics array");
        Require(!metrics.Empty() && metrics.Size() <= kMaxMetrics, ConfigError::kLimit, path, "relation metric bounds");
        std::set<std::string> ids;
        for (rapidjson::SizeType i = 0; i < metrics.Size(); ++i) {
            auto mp = Path(Path(path, "metrics"), std::to_string(i));
            auto& child = metrics[i];
            Object(child, mp, {"id", "column", "aggregate", "scale", "divisor"});
            RelationMetric item{Str(child, "id", mp), Expr(child, d, mp, false)};
            Require(ids.insert(item.id).second, ConfigError::kValue, mp, "duplicate relation metric");
            m.relation_metrics.push_back(std::move(item));
        }
        if (auto support = Find(obj, "support_policy")) {
            Object(*support, path, {"k_support", "min_hist_share", "min_active_ratio"});
            m.k_support = U(*support, "k_support", path, 2, 1, kMaxGroupDictionary);
            m.min_hist_share = N(*support, "min_hist_share", path, 0.01);
            m.min_active_ratio = N(*support, "min_active_ratio", path, 0.1);
            Require(
                m.min_hist_share >= 0 && m.min_hist_share <= 1 && m.min_active_ratio >= 0 && m.min_active_ratio <= 1,
                ConfigError::kValue, path, "support fractions");
        }
        if (auto summary = Find(obj, "summary_policy")) {
            Object(*summary, path, {"k_head", "k_stable"});
            m.k_head = U(*summary, "k_head", path, 2, 1, kMaxGroupDictionary);
            m.k_stable = U(*summary, "k_stable", path, 1, 1, kMaxGroupDictionary);
        }
        Require(m.k_stable <= m.k_head && m.k_head <= m.k_support, ConfigError::kValue, path,
                "stable <= head <= support");
    }
    for (const char* field : {"column", "aggregate", "scale", "divisor", "sample_count", "numerator", "denominator",
                              "group_space", "metrics", "support_policy", "summary_policy"}) {
        if (!Find(obj, field)) continue;
        bool value_field = std::string_view(field) == "column" || std::string_view(field) == "aggregate" ||
                           std::string_view(field) == "scale" || std::string_view(field) == "divisor" ||
                           std::string_view(field) == "sample_count";
        bool ratio_field = std::string_view(field) == "numerator" || std::string_view(field) == "denominator";
        Require((m.kind == BaselineTaskKind::kValue && value_field) ||
                    (m.kind == BaselineTaskKind::kRatio && ratio_field) ||
                    (m.kind == BaselineTaskKind::kRelation && !value_field && !ratio_field),
                ConfigError::kValue, Path(path, field), "field incompatible with kind");
    }
    return m;
}
Dataset ParseDataset(const Json& obj, const TaskConfig& config, const std::string& path) {
    Object(obj, path,
           {"id", "table", "schema", "fields", "scope", "series_keys", "row_semantics", "revision_column",
            "deduplicate", "bucket", "filter", "metrics", "fill_missing_zero"});
    Dataset d;
    d.id = Str(obj, "id", path);
    d.table = OptStr(obj, "table", path);
    d.schema = OptStr(obj, "schema", path);
    if (!config.source_relation.empty()) {
        Require(d.table.empty() || d.table == config.source_relation, ConfigError::kValue, Path(path, "table"),
                "dataset table and SQL source relation mismatch");
        d.table = config.source_relation;
    }
    Require(!config.dataframe_source || (d.table.empty() && d.schema.empty()), ConfigError::kValue, path,
            "DataFrame source has no table or schema");
    Require(!config.database_source || !d.table.empty(), ConfigError::kMissing, Path(path, "table"),
            "database relation required");
    auto& fields = Required(obj, "fields", path);
    Require(fields.IsObject(), ConfigError::kType, Path(path, "fields"), "field type object");
    Require(fields.MemberCount() > 0 && fields.MemberCount() <= kMaxFields, ConfigError::kLimit, path, "field count");
    for (auto it = fields.MemberBegin(); it != fields.MemberEnd(); ++it) {
        auto name = String(it->name, Path(path, "fields"));
        d.fields.emplace(name, Type(String(it->value, path), Path(Path(path, "fields"), name)));
    }
    d.series_keys = Strings(Required(obj, "series_keys", path), Path(path, "series_keys"), kMaxFields);
    for (const auto& name : d.series_keys) Field(d, name, Path(path, "series_keys"));
    d.row_semantics = OptStr(obj, "row_semantics", path, "immutable");
    OneOf(d.row_semantics, {"immutable", "latest_revision", "npm_period_increment"}, Path(path, "row_semantics"));
    d.revision_column = OptStr(obj, "revision_column", path);
    auto& dedup = Required(obj, "deduplicate", path);
    Object(dedup, Path(path, "deduplicate"), {"keys", "on_duplicate"});
    d.deduplicate_keys = Strings(Required(dedup, "keys", path), Path(path, "deduplicate"), kMaxFields);
    for (const auto& name : d.deduplicate_keys) Field(d, name, path);
    Require(Str(dedup, "on_duplicate", path) == "require_equal", ConfigError::kValue, path,
            "duplicate rows must agree");
    if (d.row_semantics == "npm_period_increment") {
        for (const auto& key : {"__npm_run_id", "session_id", "revision"}) {
            Require(std::find(d.deduplicate_keys.begin(), d.deduplicate_keys.end(), key) != d.deduplicate_keys.end(),
                    ConfigError::kValue, path, "NPM increments require run/session/revision identity");
        }
        Require(Field(d, "__npm_run_id", path) == LogicalType::kUtf8 &&
                    Field(d, "session_id", path) == LogicalType::kUInt64 &&
                    Field(d, "revision", path) == LogicalType::kUInt64,
                ConfigError::kValue, path, "NPM identity types");
    }
    if (d.row_semantics == "latest_revision")
        Require(!d.revision_column.empty() && Field(d, d.revision_column, path) == LogicalType::kUInt64,
                ConfigError::kValue, path, "latest revision uint64 column");
    else
        Require(d.revision_column.empty(), ConfigError::kValue, path, "revision_column only for latest_revision");
    auto& bucket = Required(obj, "bucket", path);
    Object(bucket, Path(path, "bucket"), {"column", "unit"});
    d.bucket_column = Str(bucket, "column", path);
    Require(Field(d, d.bucket_column, path) == LogicalType::kInt64, ConfigError::kValue, path,
            "bucket/time column must be int64");
    auto unit = Str(bucket, "unit", path);
    OneOf(unit, {"bucket_id", "ns", "us", "ms", "s"}, path);
    d.unit = unit == "ns"   ? TimeUnit::kNs
             : unit == "us" ? TimeUnit::kUs
             : unit == "ms" ? TimeUnit::kMs
             : unit == "s"  ? TimeUnit::kS
                            : TimeUnit::kBucketId;
    d.fill_missing_zero = Bool(obj, "fill_missing_zero", path, false);
    if (auto scope = Find(obj, "scope")) {
        Object(*scope, Path(path, "scope"), {"run_ids", "begin_bucket", "end_bucket", "consistency"});
        if (auto ids = Find(*scope, "run_ids")) d.scope.run_ids = Strings(*ids, path, 1024);
        if (auto begin = Find(*scope, "begin_bucket")) d.scope.begin_bucket = Int(*begin, path);
        if (auto end = Find(*scope, "end_bucket")) d.scope.end_bucket = Int(*end, path);
        d.scope.consistency = OptStr(*scope, "consistency", path);
    }
    if (d.scope.consistency.empty()) {
        if (config.dataframe_source)
            d.scope.consistency = "dataframe_snapshot";
        else if (d.row_semantics == "npm_period_increment")
            d.scope.consistency = "npm_completed";
    }
    Require(d.scope.begin_bucket.has_value() == d.scope.end_bucket.has_value(), ConfigError::kValue, path,
            "paired half-open range");
    if (d.scope.begin_bucket)
        Require(*d.scope.begin_bucket < *d.scope.end_bucket, ConfigError::kValue, path, "begin < end");
    if (config.dataframe_source) {
        Require(d.scope.consistency == "dataframe_snapshot" && d.scope.run_ids.empty(), ConfigError::kValue,
                Path(path, "scope"), "DataFrame requires its own finite snapshot");
    } else if (config.mode == Mode::kSnapshot) {
        OneOf(d.scope.consistency, {"npm_completed", "consistent_snapshot", "immutable_range"}, Path(path, "scope"));
        Require(!d.scope.run_ids.empty() || d.scope.begin_bucket.has_value(), ConfigError::kValue, path,
                "finite snapshot scope");
        if (d.scope.consistency == "npm_completed")
            Require(!d.scope.run_ids.empty(), ConfigError::kValue, path, "completed runs required");
    } else
        Require(d.scope.consistency.empty() || d.scope.consistency == "npm_completed", ConfigError::kValue, path,
                "poll uses source progress");
    if (!d.scope.run_ids.empty())
        Require(Field(d, "__npm_run_id", path) == LogicalType::kUtf8, ConfigError::kValue, path,
                "NPM run filter field");
    if (auto filter = Find(obj, "filter")) {
        Require(filter->IsArray(), ConfigError::kType, path, "filter array");
        Require(filter->Size() <= kMaxFields, ConfigError::kLimit, path, "filter count");
        for (const auto& item : filter->GetArray()) {
            Object(item, Path(path, "filter"), {"column", "op", "value"});
            Predicate predicate;
            predicate.column = Str(item, "column", path);
            predicate.op = Str(item, "op", path);
            OneOf(predicate.op, {"eq", "ne", "lt", "le", "gt", "ge"}, path);
            auto type = Field(d, predicate.column, path);
            Require(type != LogicalType::kBoolean || predicate.op == "eq" || predicate.op == "ne", ConfigError::kValue,
                    path, "boolean comparison");
            predicate.value = TypedValue(Required(item, "value", path), type, Path(path, "filter"));
            d.filter.push_back(std::move(predicate));
        }
    }
    auto& metrics = Required(obj, "metrics", path);
    Require(metrics.IsArray(), ConfigError::kType, Path(path, "metrics"), "metrics array");
    Require(!metrics.Empty() && metrics.Size() <= kMaxMetrics, ConfigError::kLimit, path, "metric count");
    std::set<std::string> metric_ids;
    for (rapidjson::SizeType i = 0; i < metrics.Size(); ++i) {
        auto mp = Path(Path(path, "metrics"), std::to_string(i));
        auto m = ParseMetric(metrics[i], d, mp);
        Require(metric_ids.insert(m.id).second, ConfigError::kValue, mp, "duplicate metric id");
        d.metrics.push_back(std::move(m));
    }
    return d;
}
std::string AlgorithmConfig(const TaskConfig& c, const Dataset& d, const Metric& m) {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> w(buffer);
    auto text = [&](const char* key, const std::string& value) {
        w.Key(key);
        w.String(value.data(), value.size());
    };
    w.StartObject();
    w.Key("schema_version");
    w.Uint(1);
    std::string identity;
    EncodeIdentity(c.source, d.id, m.id, m.kind, {c.task_key}, &identity);
    text("task_id", "baseliner-" + Hash(identity));
    text("task_name", m.id);
    text("feature_id", m.id);
    text("task_kind", m.kind == BaselineTaskKind::kValue   ? "value"
                      : m.kind == BaselineTaskKind::kRatio ? "ratio"
                                                           : "relation");
    text("feature_type", m.feature_type);
    text("profile", m.profile);
    w.Key("clock_spec");
    w.StartObject();
    w.Key("bucket_seconds");
    w.Int64(c.bucket_seconds);
    text("timezone", c.timezone);
    w.EndObject();
    w.Key("calendar_ref");
    w.StartObject();
    text("calendar_id", c.calendar_id);
    text("calendar_version", c.calendar_version);
    w.EndObject();
    if (m.group_space) {
        text("group_space_id", m.group_space->id);
        text("group_space_version", m.group_space->version);
        w.Key("metrics");
        w.StartArray();
        for (const auto& child : m.relation_metrics) w.String(child.id.data(), child.id.size());
        w.EndArray();
        w.Key("other_group_idxs");
        w.StartArray();
        if (m.group_space->other) w.Uint(*m.group_space->other);
        w.EndArray();
        w.Key("support_policy");
        w.StartObject();
        w.Key("k_support");
        w.Uint(m.k_support);
        w.Key("min_hist_share");
        w.Double(m.min_hist_share);
        w.Key("min_active_ratio");
        w.Double(m.min_active_ratio);
        w.EndObject();
        w.Key("summary_policy");
        w.StartObject();
        w.Key("k_head");
        w.Uint(m.k_head);
        w.Key("k_stable");
        w.Uint(m.k_stable);
        w.EndObject();
    }
    w.EndObject();
    return {buffer.GetString(), buffer.GetSize()};
}
TaskConfig Parse(const Json& obj) {
    Object(obj, "",
           {"schema_version", "task_key", "source", "mode", "datasets", "clock", "calendar", "bootstrap", "forecast",
            "read_policy", "state_policy", "persistence"});
    TaskConfig c;
    const auto& version = Required(obj, "schema_version", "");
    Require(version.IsUint(), ConfigError::kType, "/schema_version", "uint schema_version");
    Require(version.GetUint() == kContractVersion, ConfigError::kValue, "/schema_version", "unsupported version");
    c.task_key = Str(obj, "task_key", "");
    c.source = Str(obj, "source", "");
    BindSource(&c);
    auto mode = Str(obj, "mode", "");
    OneOf(mode, {"snapshot", "poll"}, "/mode");
    c.mode = mode == "snapshot" ? Mode::kSnapshot : Mode::kPoll;
    Require(!c.dataframe_source || c.mode == Mode::kSnapshot, ConfigError::kValue, "/mode",
            "DataFrame source only supports snapshot");
    auto& clock = Required(obj, "clock", "");
    Object(clock, "/clock", {"bucket_seconds", "timezone"});
    c.bucket_seconds = UInt(Required(clock, "bucket_seconds", "/clock"), "/clock/bucket_seconds", 1, 86400);
    c.timezone = Str(clock, "timezone", "/clock");
    std::unique_ptr<icu::TimeZone> zone(icu::TimeZone::createTimeZone(icu::UnicodeString::fromUTF8(c.timezone)));
    Require(*zone != icu::TimeZone::getUnknown(), ConfigError::kValue, "/clock/timezone", "unknown timezone");
    auto& calendar = Required(obj, "calendar", "");
    Object(calendar, "/calendar", {"calendar_id", "calendar_version"});
    c.calendar_id = Str(calendar, "calendar_id", "/calendar");
    c.calendar_version = Str(calendar, "calendar_version", "/calendar");
    Require(c.calendar_version != "latest", ConfigError::kReference, "/calendar/calendar_version",
            "exact calendar version");
    if (auto bootstrap = Find(obj, "bootstrap")) {
        Object(*bootstrap, "/bootstrap", {"mode", "begin_bucket", "end_bucket", "min_observations", "insufficient"});
        auto kind = Str(*bootstrap, "mode", "/bootstrap");
        OneOf(kind, {"cold", "history"}, "/bootstrap/mode");
        c.bootstrap.history = kind == "history";
        if (c.bootstrap.history) {
            c.bootstrap.begin_bucket =
                Int(Required(*bootstrap, "begin_bucket", "/bootstrap"), "/bootstrap/begin_bucket");
            c.bootstrap.end_bucket = Int(Required(*bootstrap, "end_bucket", "/bootstrap"), "/bootstrap/end_bucket");
            c.bootstrap.min_observations = UInt(Required(*bootstrap, "min_observations", "/bootstrap"),
                                                "/bootstrap/min_observations", 1, 10000000);
            auto policy = Str(*bootstrap, "insufficient", "/bootstrap");
            OneOf(policy, {"fail", "cold"}, "/bootstrap/insufficient");
            c.bootstrap.insufficient_cold = policy == "cold";
            Require(c.bootstrap.begin_bucket < c.bootstrap.end_bucket, ConfigError::kValue, "/bootstrap",
                    "half-open training window");
        } else
            Require(bootstrap->MemberCount() == 1, ConfigError::kValue, "/bootstrap", "cold has no training fields");
    }
    if (auto forecast = Find(obj, "forecast")) {
        Object(*forecast, "/forecast", {"horizon_buckets"});
        c.horizon_buckets = U(*forecast, "horizon_buckets", "/forecast", 0, 0, kMaxForecastBuckets);
    }
    if (auto read = Find(obj, "read_policy")) {
        Object(*read, "/read_policy", {"page_rows", "poll_interval_ms", "max_pending_bytes"});
        c.read.page_rows = U(*read, "page_rows", "/read_policy", c.read.page_rows, 1, 65536);
        c.read.poll_interval_ms = U(*read, "poll_interval_ms", "/read_policy", c.read.poll_interval_ms, 1, 60000);
        c.read.max_pending_bytes =
            U(*read, "max_pending_bytes", "/read_policy", c.read.max_pending_bytes, 1, kMaxBufferBytes);
    }
    if (auto state = Find(obj, "state_policy")) {
        Object(*state, "/state_policy",
               {"max_runtime_identities", "max_model_identities", "max_basis_versions", "capacity", "release_scope",
                "idle_timeout_ms", "maintenance_max_releases", "maintenance_max_bytes"});
        c.state.limits.max_runtime_identities =
            U(*state, "max_runtime_identities", "/state_policy", 1024, 1, kMaxIdentities);
        c.state.limits.max_model_identities =
            U(*state, "max_model_identities", "/state_policy", 1024, 1, kMaxIdentities);
        c.state.limits.max_basis_versions_per_metric =
            U(*state, "max_basis_versions", "/state_policy", 2, 1, kMaxBasisVersions);
        auto capacity = OptStr(*state, "capacity", "/state_policy", "reject");
        OneOf(capacity, {"reject", "evict_idle"}, "/state_policy/capacity");
        c.state.evict_idle = capacity == "evict_idle";
        auto scope = OptStr(*state, "release_scope", "/state_policy", "RuntimeOnly");
        OneOf(scope, {"RuntimeOnly", "AllState"}, "/state_policy/release_scope");
        c.state.release_scope =
            scope == "RuntimeOnly" ? BaselineStateReleaseScopeV1::kRuntimeOnly : BaselineStateReleaseScopeV1::kAllState;
        c.state.idle_timeout_ms = U(*state, "idle_timeout_ms", "/state_policy", 0, 0, 365ULL * 86400000);
        c.state.maintenance_max_releases = U(*state, "maintenance_max_releases", "/state_policy", 16, 1, 4096);
        c.state.maintenance_max_bytes =
            U(*state, "maintenance_max_bytes", "/state_policy", 16 * 1024 * 1024, 1, kMaxBufferBytes);
    }
    Require(c.mode != Mode::kSnapshot || c.state.idle_timeout_ms == 0, ConfigError::kValue,
            "/state_policy/idle_timeout_ms", "snapshot disables processing-time TTL");
    Require(!c.state.evict_idle || c.state.idle_timeout_ms > 0, ConfigError::kValue, "/state_policy/capacity",
            "eviction requires online idle deadline");
    if (auto persistence = Find(obj, "persistence")) {
        Object(*persistence, "/persistence",
               {"checkpoint_every_buckets", "checkpoint_interval_ms", "lease_ms", "renew_ms", "operation_timeout_ms",
                "max_checkpoint_bytes", "retain_generations", "restore"});
        auto& p = c.persistence;
        p.checkpoint_every_buckets = U(*persistence, "checkpoint_every_buckets", "/persistence", 1, 1, 1000000);
        p.checkpoint_interval_ms = U(*persistence, "checkpoint_interval_ms", "/persistence", 1000, 1, 3600000);
        p.lease_ms = U(*persistence, "lease_ms", "/persistence", 30000, 1, 86400000);
        p.renew_ms = U(*persistence, "renew_ms", "/persistence", 10000, 1, 86400000);
        p.operation_timeout_ms = U(*persistence, "operation_timeout_ms", "/persistence", 5000, 1, 3600000);
        p.max_checkpoint_bytes =
            U(*persistence, "max_checkpoint_bytes", "/persistence", 16 * 1024 * 1024, 1, kMaxBufferBytes);
        p.retain_generations = U(*persistence, "retain_generations", "/persistence", 2, 1, 64);
        p.restore = OptStr(*persistence, "restore", "/persistence", "if_exists");
        OneOf(p.restore, {"require", "if_exists", "fresh"}, "/persistence/restore");
    }
    Require(uint64_t(c.persistence.renew_ms) + c.persistence.operation_timeout_ms < c.persistence.lease_ms,
            ConfigError::kValue, "/persistence", "renew + operation timeout must be below lease");
    auto& datasets = Required(obj, "datasets", "");
    Require(datasets.IsArray(), ConfigError::kType, "/datasets", "dataset array");
    Require(!datasets.Empty() && datasets.Size() <= kMaxDatasets, ConfigError::kLimit, "/datasets", "dataset count");
    Require((!c.dataframe_source && c.source_relation.empty()) || datasets.Size() == 1, ConfigError::kValue,
            "/datasets", "single source requires exactly one dataset");
    std::set<std::string> ids;
    for (rapidjson::SizeType i = 0; i < datasets.Size(); ++i) {
        auto path = Path("/datasets", std::to_string(i));
        auto d = ParseDataset(datasets[i], c, path);
        Require(ids.insert(d.id).second, ConfigError::kValue, path, "duplicate dataset id");
        if (c.bootstrap.history && d.scope.begin_bucket)
            Require(*d.scope.begin_bucket <= c.bootstrap.begin_bucket && *d.scope.end_bucket >= c.bootstrap.end_bucket,
                    ConfigError::kValue, path, "training must lie in source range");
        for (auto& m : d.metrics) {
            Require(m.kind != BaselineTaskKind::kRelation || c.state.limits.max_basis_versions_per_metric >= 2,
                    ConfigError::kLimit, "/state_policy/max_basis_versions", "Relation requires two basis versions");
            m.algorithm_config_json = AlgorithmConfig(c, d, m);
        }
        c.datasets.push_back(std::move(d));
    }
    return c;
}
void AppendUInt(uint64_t value, std::string* out) {
    for (int shift = 56; shift >= 0; shift -= 8) out->push_back(static_cast<char>(value >> shift));
}
void AppendText(std::string_view value, std::string* out) {
    AppendUInt(value.size(), out);
    out->append(value.data(), value.size());
}
}  // namespace

ConfigStatus ParseConfig(std::string_view json, ConfigSnapshot* output) {
    if (!output) return {ConfigError::kValue, "", "null output"};
    try {
        rapidjson::Document doc;
        Document(json, &doc);
        ConfigSnapshot result;
        result.config = Parse(doc);
        result.original_json.assign(json);
        result.sha256_hex = Hash(json);
        *output = std::move(result);
        return {};
    } catch (const Invalid& error) {
        return error.status;
    }
}
ConfigStatus NormalizeInlineConfig(std::string_view json, std::string_view sql_source, ConfigSnapshot* output) {
    if (!output) return {ConfigError::kValue, "", "null output"};
    try {
        rapidjson::Document doc;
        Document(json, &doc);
        Require(doc.IsObject(), ConfigError::kType, "", "expected object");
        ConfigSnapshot result;
        if (doc.HasMember("dataset")) {
            Require(!doc.HasMember("datasets"), ConfigError::kValue, "/dataset", "dataset and datasets conflict");
            Object(doc, "",
                   {"schema_version", "task_key", "source", "mode", "dataset", "clock", "calendar", "bootstrap",
                    "forecast", "read_policy", "state_policy", "persistence"});
            TaskConfig binding;
            binding.source.assign(sql_source);
            BindSource(&binding);
            Require(binding.dataframe_source || !binding.source_relation.empty(), ConfigError::kValue, "/source",
                    "single-dataset shorthand requires a table or DataFrame source");
            auto& allocator = doc.GetAllocator();
            if (!doc.HasMember("source"))
                doc.AddMember("source", rapidjson::Value(sql_source.data(), sql_source.size(), allocator), allocator);
            Require(Str(doc, "source", "") == sql_source, ConfigError::kValue, "/source",
                    "SQL FROM and config source mismatch");
            if (!doc.HasMember("schema_version")) doc.AddMember("schema_version", 1, allocator);
            if (!doc.HasMember("mode")) doc.AddMember("mode", "snapshot", allocator);
            auto& dataset = doc["dataset"];
            Require(dataset.IsObject(), ConfigError::kType, "/dataset", "expected object");
            if (!dataset.HasMember("id")) dataset.AddMember("id", "source", allocator);
            if (binding.dataframe_source && !dataset.HasMember("scope")) {
                rapidjson::Value scope(rapidjson::kObjectType);
                scope.AddMember("consistency", "dataframe_snapshot", allocator);
                dataset.AddMember("scope", scope, allocator);
            }
            rapidjson::Value datasets(rapidjson::kArrayType);
            datasets.PushBack(dataset, allocator);
            doc.RemoveMember("dataset");
            doc.AddMember("datasets", datasets, allocator);
            rapidjson::StringBuffer buffer;
            rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
            // Stable object ordering makes shorthand independent of JSON member order.
            auto canonical = [&](const auto& self, const Json& value) -> void {
                if (value.IsObject()) {
                    std::map<std::string, const Json*> members;
                    for (auto it = value.MemberBegin(); it != value.MemberEnd(); ++it)
                        members.emplace(Text(it->name), &it->value);
                    writer.StartObject();
                    for (const auto& [key, child] : members) {
                        writer.Key(key.data(), key.size());
                        self(self, *child);
                    }
                    writer.EndObject();
                } else if (value.IsArray()) {
                    writer.StartArray();
                    for (const auto& child : value.GetArray()) self(self, child);
                    writer.EndArray();
                } else
                    value.Accept(writer);
            };
            canonical(canonical, doc);
            auto status = ParseConfig({buffer.GetString(), buffer.GetSize()}, &result);
            if (!status.ok()) return status;
        } else {
            result.config = Parse(doc);
            result.original_json.assign(json);
            result.sha256_hex = Hash(json);
        }
        Require(result.config.source == sql_source, ConfigError::kValue, "/source",
                "SQL FROM and config source mismatch");
        *output = std::move(result);
        return {};
    } catch (const Invalid& error) {
        return error.status;
    }
}
ConfigStatus ResolveConfig(std::string_view with_json, std::string_view sql_source, IConfigChannelRegistryV1* registry,
                           ConfigSnapshot* output) {
    if (!output) return {ConfigError::kValue, "", "null output"};
    try {
        rapidjson::Document with;
        Document(with_json, &with, kMaxConfigBytes + 65536);
        Object(with, "", {"config", "parameters", "model_output"});
        auto ref = Find(with, "config");
        auto parameters = Find(with, "parameters");
        Require((ref != nullptr) != (parameters != nullptr), ConfigError::kValue, "",
                "exactly one config or parameters");
        ConfigSnapshot result;
        if (parameters) {
            Require(parameters->IsString(), ConfigError::kType, "/parameters", "JSON string parameters");
            auto status = NormalizeInlineConfig(Text(*parameters), sql_source, &result);
            if (!status.ok()) return status;
        } else {
            const auto reference = String(*ref, "/config");
            const uint64_t revision = Reference(reference);
            Require(registry, ConfigError::kReference, "/config", "Config registry unavailable");
            ConfigChannelSnapshot snapshot{};
            std::string error;
            Require(registry->Resolve(reference.c_str(), &snapshot, &error) == 0, ConfigError::kReference, "/config",
                    error);
            Require(snapshot.content && snapshot.format == "json" && snapshot.revision == revision &&
                        reference == "config." + snapshot.channel_name + "@" + std::to_string(revision),
                    ConfigError::kReference, "/config", "snapshot identity/format mismatch");
            auto status = ParseConfig(*snapshot.content, &result);
            if (!status.ok()) return status;
            Require(snapshot.content_bytes == result.original_json.size() && snapshot.sha256_hex == result.sha256_hex,
                    ConfigError::kReference, "/config", "snapshot length/hash mismatch");
            result.exact_reference = reference;
            result.revision = revision;
        }
        Require(result.config.source == sql_source, ConfigError::kValue, "/source",
                "SQL FROM and config source mismatch");
        if (const auto* target = Find(with, "model_output")) {
            result.model_output = String(*target, "/model_output");
            TaskConfig binding;
            binding.source = result.model_output;
            BindSource(&binding);
            Require(binding.dataframe_source || (binding.database_source && !binding.source_relation.empty() &&
                                                 result.model_output.compare(0, 11, "clickhouse.") != 0),
                    ConfigError::kValue, "/model_output", "model output requires a writable table or DataFrame");
            Require(result.config.mode == Mode::kSnapshot, ConfigError::kValue, "/model_output",
                    "model output requires snapshot mode");
        }
        *output = std::move(result);
        return {};
    } catch (const Invalid& error) {
        return error.status;
    }
}
ConfigStatus EncodeIdentity(std::string_view source_namespace, std::string_view dataset, std::string_view metric,
                            BaselineTaskKind kind, const std::vector<KeyValue>& keys, std::string* output) {
    if (!output || source_namespace.empty() || dataset.empty() || metric.empty() || keys.empty() ||
        keys.size() > kMaxFields || kind < BaselineTaskKind::kValue || kind > BaselineTaskKind::kRelation)
        return {ConfigError::kValue, "", "invalid model identity"};
    std::string result("\x01", 1);
    AppendText(source_namespace, &result);
    AppendText(dataset, &result);
    AppendText(metric, &result);
    result.push_back(static_cast<char>(kind));
    AppendUInt(keys.size(), &result);
    for (const auto& key : keys) {
        result.push_back(static_cast<char>(key.index()));
        if (const auto* value = std::get_if<bool>(&key))
            result.push_back(*value ? 1 : 0);
        else if (const auto* value = std::get_if<int64_t>(&key))
            AppendUInt(static_cast<uint64_t>(*value), &result);
        else if (const auto* value = std::get_if<uint64_t>(&key))
            AppendUInt(*value, &result);
        else if (const auto* value = std::get_if<double>(&key)) {
            if (!std::isfinite(*value)) return {ConfigError::kValue, "", "nonfinite business key"};
            const double normalized = *value == 0 ? 0 : *value;
            uint64_t bits;
            std::memcpy(&bits, &normalized, sizeof(bits));
            AppendUInt(bits, &result);
        } else
            AppendText(std::get<std::string>(key), &result);
    }
    *output = std::move(result);
    return {};
}

std::shared_ptr<arrow::Schema> MakeSchema(SchemaKind kind) {
    using arrow::field;
    std::vector<std::shared_ptr<arrow::Field>> fields;
    std::string name;
    if (kind == SchemaKind::kModelParameters) {
        name = "model_parameters";
        fields = {field("task_key", arrow::utf8(), false),
                  field("dataset_id", arrow::utf8(), false),
                  field("metric_id", arrow::utf8(), false),
                  field("series_key", arrow::binary(), false),
                  field("source_epoch", arrow::utf8(), false),
                  field("config_hash", arrow::utf8(), false),
                  field("model_kind", arrow::utf8(), false),
                  field("model_basis_id", arrow::utf8(), false),
                  field("as_of_bucket", arrow::int64()),
                  field("status", arrow::int32(), false),
                  field("maturity", arrow::utf8()),
                  field("parameters_version", arrow::uint32(), false),
                  field("parameters_json", arrow::utf8())};
    } else if (kind == SchemaKind::kProgress) {
        name = "progress";
        fields = {field("dataset_id", arrow::utf8(), false), field("source_epoch", arrow::utf8(), false),
                  field("committed_position", arrow::binary(), false),
                  field("closed_before_bucket", arrow::int64(), false)};
    } else if (kind == SchemaKind::kObservation) {
        name = "observation";
        fields = {field("dataset_id", arrow::utf8(), false),
                  field("metric_id", arrow::utf8(), false),
                  field("kind", arrow::uint8(), false),
                  field("identity", arrow::binary(), false),
                  field("source_epoch", arrow::utf8(), false),
                  field("bucket_id", arrow::int64(), false),
                  field("value", arrow::float64()),
                  field("sample_count", arrow::uint64()),
                  field("numerator", arrow::float64()),
                  field("denominator", arrow::float64()),
                  field("group_idx", arrow::list(field("item", arrow::uint32(), false))),
                  field("metrics",
                        arrow::list(field(
                            "item",
                            arrow::struct_(
                                {field("metric", arrow::utf8(), false), field("total", arrow::float64(), false),
                                 field("active_count", arrow::uint32(), false),
                                 field("values_by_group", arrow::list(field("item", arrow::float64(), false)), false)}),
                            false)))};
    } else {
        fields = {field("task_key", arrow::utf8(), false),     field("dataset_id", arrow::utf8(), false),
                  field("metric_id", arrow::utf8(), false),    field("series_key", arrow::binary(), false),
                  field("source_epoch", arrow::utf8(), false), field("target_bucket", arrow::int64(), false),
                  field("config_hash", arrow::utf8(), false),  field("published_generation", arrow::uint64(), false)};
        if (kind == SchemaKind::kResults) {
            name = "results";
            for (auto f :
                 {field("result_kind", arrow::utf8(), false), field("summary_id", arrow::utf8()),
                  field("basis_id", arrow::utf8()),           field("issued_after_bucket", arrow::int64(), false),
                  field("model_basis_id", arrow::utf8()),     field("bucket_seconds", arrow::int64(), false),
                  field("timezone", arrow::utf8(), false),    field("unit", arrow::utf8(), false),
                  field("observed", arrow::float64()),        field("expected", arrow::float64()),
                  field("lower", arrow::float64()),           field("upper", arrow::float64()),
                  field("status", arrow::int32(), false),     field("band_kind", arrow::utf8()),
                  field("confidence", arrow::float64()),      field("maturity", arrow::utf8()),
                  field("score", arrow::float64()),           field("is_outside_band", arrow::boolean()),
                  field("can_score", arrow::boolean()),       field("can_update", arrow::boolean()),
                  field("update_weight", arrow::float64()),   field("can_alert", arrow::boolean())})
                fields.push_back(f);
        } else if (kind == SchemaKind::kRelationFusion) {
            name = "relation_fusion";
            fields.push_back(field("status", arrow::int32(), false));
            fields.push_back(field("fusion_score", arrow::float64()));
            fields.push_back(field("basis_id", arrow::utf8()));
            fields.push_back(field("can_alert", arrow::boolean()));
        } else if (kind == SchemaKind::kMaintenance) {
            name = "maintenance";
            for (auto f :
                 {field("event_kind", arrow::utf8(), false), field("reason", arrow::utf8(), false),
                  field("runtime_identities", arrow::uint64(), false),
                  field("model_identities", arrow::uint64(), false), field("routed_states", arrow::uint64(), false),
                  field("retained_basis_versions", arrow::uint64(), false)})
                fields.push_back(f);
        } else
            return {};
    }
    return arrow::schema(
        fields, arrow::key_value_metadata({"flowsql.baseliner.version", "flowsql.baseliner.kind"}, {"1", name}));
}
std::vector<std::string> ResultKeyFields(bool forecast, bool routed) {
    std::vector<std::string> fields{"task_key",     "dataset_id",  "metric_id",    "series_key",
                                    "source_epoch", "result_kind", "target_bucket"};
    if (forecast) fields.push_back("issued_after_bucket");
    if (routed) {
        fields.push_back("summary_id");
        fields.push_back("basis_id");
    }
    return fields;
}
ConfigStatus ValidateResult(const ScalarResult& r) {
    if (r.status < BaselineStatus::kOk || r.status > BaselineStatus::kSerializationFailed)
        return {ConfigError::kValue, "", "unknown algorithm status"};
    for (const auto& value : {r.observed, r.expected, r.lower, r.upper})
        if (value && !std::isfinite(*value)) return {ConfigError::kValue, "", "nonfinite result"};
    if (r.forecast && (r.observed || r.can_alert || r.target_bucket <= r.issued_after_bucket))
        return {ConfigError::kValue, "", "forecast has actual/alert or nonfuture bucket"};
    if (!r.forecast && r.target_bucket != r.issued_after_bucket)
        return {ConfigError::kValue, "", "evaluation issue bucket"};
    if (!r.forecast && !r.observed) return {ConfigError::kValue, "", "evaluation requires actual value"};
    bool all = r.expected && r.lower && r.upper;
    bool any = r.expected || r.lower || r.upper;
    if (r.status == BaselineStatus::kOk ? !all : any) return {ConfigError::kValue, "", "status/band NULL mismatch"};
    if (all && (*r.lower > *r.expected || *r.expected > *r.upper)) return {ConfigError::kValue, "", "unordered band"};
    return {};
}
ConfigStatus ValidateProgress(const ProgressEnvelope& progress, const std::vector<std::string>& datasets) {
    if (progress.contract_version != kContractVersion || progress.datasets.size() > kMaxDatasets)
        return {ConfigError::kValue, "", "progress version/count"};
    std::set<std::string> seen;
    for (const auto& p : progress.datasets) {
        if (std::find(datasets.begin(), datasets.end(), p.dataset_id) == datasets.end() ||
            !seen.insert(p.dataset_id).second || p.epoch.empty() || p.committed_position.empty() ||
            p.committed_position.size() > kMaxConfigBytes)
            return {ConfigError::kValue, "", "invalid dataset progress"};
    }
    return {};
}
}  // namespace flowsql::baseliner
