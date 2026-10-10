// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef FLOWSQL_OPERATORS_BASELINER_CONTRACT_H_
#define FLOWSQL_OPERATORS_BASELINER_CONTRACT_H_

#include <framework/interfaces/ibaseline_state_control.h>
#include <framework/interfaces/iblock_transform_database_input.h>
#include <framework/interfaces/iconfig_channel_registry.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace arrow {
class Schema;
}

namespace flowsql::baseliner {

constexpr uint32_t kContractVersion = 1;
constexpr size_t kMaxConfigBytes = 1024 * 1024;
constexpr size_t kMaxJsonDepth = 32;
constexpr uint32_t kMaxDatasets = 64;
constexpr uint32_t kMaxMetrics = 256;
constexpr uint32_t kMaxFields = 256;
constexpr uint32_t kMaxForecastBuckets = 4096;
constexpr uint64_t kMaxBufferBytes = 1024ULL * 1024 * 1024;
constexpr uint64_t kMaxIdentities = 1000000;
constexpr uint32_t kMaxBasisVersions = 64;
constexpr uint32_t kMaxGroupDictionary = 65536;

enum class ConfigError : uint8_t { kNone, kJson, kDuplicate, kUnknown, kMissing, kType, kValue, kLimit, kReference };
struct ConfigStatus {
    ConfigError code = ConfigError::kNone;
    std::string path;
    std::string message;
    bool ok() const { return code == ConfigError::kNone; }
};

enum class LogicalType : uint8_t { kBoolean, kInt64, kUInt64, kFloat64, kUtf8 };
enum class Mode : uint8_t { kSnapshot, kPoll };
enum class Aggregate : uint8_t { kColumn, kSum, kCount, kMean, kMin, kMax };
enum class InvalidPolicy : uint8_t { kFail, kSkip };
enum class TimeUnit : uint8_t { kBucketId, kNs, kUs, kMs, kS };
using KeyValue = std::variant<bool, int64_t, uint64_t, double, std::string>;

struct Expression {
    // column requires one retained row per identity/bucket; multiple rows must choose an aggregate.
    // count without column counts rows; count(column) counts non-NULL values. Scale/divisor follow aggregation.
    std::string column;
    Aggregate aggregate = Aggregate::kColumn;
    double scale = 1.0;
    double divisor = 1.0;
};
struct Predicate {
    std::string column;
    std::string op;
    KeyValue value;
};
struct GroupSpace {
    std::string id;
    std::string version;
    std::string column;
    // Empty dictionary means the column is an existing uint64 group_idx (validated before narrowing).
    std::vector<std::pair<KeyValue, uint32_t>> dictionary;
    std::optional<uint32_t> other;
};
struct RelationMetric {
    std::string id;
    Expression value;
};
struct Metric {
    std::string id;
    BaselineTaskKind kind = BaselineTaskKind::kValue;
    std::string feature_type;
    std::string profile;
    InvalidPolicy null_policy = InvalidPolicy::kFail;
    InvalidPolicy invalid_policy = InvalidPolicy::kFail;
    Expression value;
    Expression numerator;
    Expression denominator;
    std::optional<Expression> sample_count;
    std::optional<GroupSpace> group_space;
    std::vector<RelationMetric> relation_metrics;
    uint32_t k_support = 2;
    uint32_t k_head = 2;
    uint32_t k_stable = 1;
    double min_hist_share = 0.01;
    double min_active_ratio = 0.1;
    // Owned algorithm creation JSON; profile/calendar resolution stays with the algorithm service.
    std::string algorithm_config_json;
};
struct Scope {
    std::vector<std::string> run_ids;
    std::optional<int64_t> begin_bucket;
    std::optional<int64_t> end_bucket;
    // npm_completed, consistent_snapshot, or immutable_range. Verification is the reader's duty.
    std::string consistency;
};
struct Dataset {
    std::string id;
    std::string table;
    std::string schema;
    std::map<std::string, LogicalType> fields;
    Scope scope;
    std::vector<std::string> series_keys;
    std::vector<std::string> deduplicate_keys;
    std::string row_semantics = "immutable";
    std::string revision_column;
    std::string bucket_column;
    TimeUnit unit = TimeUnit::kBucketId;
    bool fill_missing_zero = false;
    std::vector<Predicate> filter;
    std::vector<Metric> metrics;
};
struct BootstrapPolicy {
    bool history = false;
    int64_t begin_bucket = 0;
    int64_t end_bucket = 0;
    uint32_t min_observations = 0;
    bool insufficient_cold = false;
};
struct ReadPolicy {
    uint32_t page_rows = 4096;
    uint32_t poll_interval_ms = 1000;
    uint64_t max_pending_bytes = 16 * 1024 * 1024;
};
struct StatePolicy {
    BaselineStateLimitsV1 limits{1024, 1024, 2};
    bool evict_idle = false;
    BaselineStateReleaseScopeV1 release_scope = BaselineStateReleaseScopeV1::kRuntimeOnly;
    uint64_t idle_timeout_ms = 0;
    uint32_t maintenance_max_releases = 16;
    uint64_t maintenance_max_bytes = 16 * 1024 * 1024;
};
struct PersistencePolicy {
    uint32_t checkpoint_every_buckets = 1;
    uint32_t checkpoint_interval_ms = 1000;
    uint32_t lease_ms = 30000;
    uint32_t renew_ms = 10000;
    uint32_t operation_timeout_ms = 5000;
    uint64_t max_checkpoint_bytes = 16 * 1024 * 1024;
    uint32_t retain_generations = 2;
    std::string restore = "if_exists";
};
struct TaskConfig {
    uint32_t schema_version = kContractVersion;
    std::string task_key;
    std::string source;
    bool database_source = true;
    std::string source_relation;  // Nonempty for an exact three-part database source.
    bool dataframe_source = false;
    Mode mode = Mode::kSnapshot;
    std::vector<Dataset> datasets;
    int64_t bucket_seconds = 60;
    std::string timezone;
    std::string calendar_id;
    std::string calendar_version;
    BootstrapPolicy bootstrap;
    uint32_t horizon_buckets = 0;
    ReadPolicy read;
    StatePolicy state;
    PersistencePolicy persistence;
};
struct ConfigSnapshot {
    TaskConfig config;
    std::string original_json;
    std::string exact_reference;
    uint64_t revision = 0;  // Inline parameters have no Config Channel revision.
    std::string sha256_hex;
    std::string model_output;  // Execution option, never part of TaskConfig/configuration hashes.
};

// Pure validation/Resolve only: no source or sink is opened. Failed calls leave output untouched.
ConfigStatus ParseConfig(std::string_view json, ConfigSnapshot* output);
// Accepts full JSON unchanged or a single-dataset inline shorthand bound to SQL FROM.
// No I/O; shorthand is serialized to owned full JSON before validation/hash generation.
ConfigStatus NormalizeInlineConfig(std::string_view json, std::string_view sql_source, ConfigSnapshot* output);
ConfigStatus ResolveConfig(std::string_view with_json, std::string_view sql_source, IConfigChannelRegistryV1* registry,
                           ConfigSnapshot* output);

// Length-delimited, type-tagged binary identity. Epoch/run are deliberately outside model identity.
ConfigStatus EncodeIdentity(std::string_view source_namespace, std::string_view dataset, std::string_view metric,
                            BaselineTaskKind kind, const std::vector<KeyValue>& keys, std::string* output);

enum class SchemaKind : uint8_t { kObservation, kProgress, kResults, kRelationFusion, kMaintenance, kModelParameters };
// All fields and metadata are owned. Observation batches carry a separate typed progress envelope.
std::shared_ptr<arrow::Schema> MakeSchema(SchemaKind kind);
std::vector<std::string> ResultKeyFields(bool forecast, bool routed);

struct ScalarResult {
    bool forecast = false;
    int64_t target_bucket = 0;
    int64_t issued_after_bucket = 0;
    BaselineStatus status = BaselineStatus::kNotTrained;
    std::optional<double> observed;
    std::optional<double> expected;
    std::optional<double> lower;
    std::optional<double> upper;
    std::optional<bool> can_alert;
};
ConfigStatus ValidateResult(const ScalarResult& result);

using DatasetProgress = BlockDatasetProgressV1;
using ProgressEnvelope = BlockInputProgressV1;
ConfigStatus ValidateProgress(const ProgressEnvelope& progress, const std::vector<std::string>& datasets);

}  // namespace flowsql::baseliner
#endif
