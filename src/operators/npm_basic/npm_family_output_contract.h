// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include <arrow/result.h>
#include <framework/interfaces/idatabase_channel.h>
#include <framework/interfaces/idatabase_published_source.h>
#include <operators/npm_basic/npm_family_result_contract.h>

namespace arrow {
class Array;
class Field;
}  // namespace arrow

namespace flowsql::npm {

struct NpmFamilyOutputOptions {
    std::vector<std::string> features;  // Empty selects basic. Modules enable both families.
    std::optional<std::string> observing;
    bool managed = false;
    bool has_result_where = false;
    bool has_result_projection = false;
};
struct NpmFamilyOutputSelection {
    std::vector<NpmEntityDescriptorV1> enabled_entities;
    std::vector<NpmEntityDescriptorV1> persisted_entities;
    std::optional<NpmEntityDescriptorV1> observed_entity;
    std::shared_ptr<arrow::Schema> foreground_schema;
    // No observed entity means a zero-column Schema and zero rows; never a placeholder row.
};
arrow::Result<NpmFamilyOutputSelection> ResolveNpmFamilyOutput(const NpmFamilyOutputOptions& options);

enum class NpmFamilyDatabaseBackend : uint8_t { kSqlite, kMysql, kPostgres, kClickHouse };
struct NpmFamilyRelations {
    std::string data;
    std::string history;
    std::string latest;
    std::string final;
};
struct NpmFamilyDatabaseColumn {
    std::shared_ptr<arrow::Field> field;  // Exact readback type, nullability and address metadata.
    std::string physical_type;
    bool check_uint32_range = false;   // SQLite/PostgreSQL: 0 <= value <= 4294967295.
    uint32_t check_binary_length = 0;  // SQLite/PostgreSQL IPv6: length(value) == 16, NULL permitted if nullable.
};
struct NpmFamilyDatabaseContract {
    NpmFamilyDatabaseBackend backend;
    NpmEntityDescriptorV1 entity;
    NpmFamilyRelations relations;
    std::vector<NpmFamilyDatabaseColumn> columns;
    // Physical data rows additionally contain run_id/task_id text. Entity readback uses entity.schema exactly.
};
arrow::Result<NpmFamilyDatabaseContract> NpmFamilyDatabaseMapping(NpmFamilyDatabaseBackend backend,
                                                                  NpmFamilyEntityKind kind,
                                                                  packet::AddressFamily family);
// Borrowed buffers must outlive the synchronous prepared command. No text/hex address intermediary.
DatabaseParameterV1 NpmFamilyAddressParameter(network::IPv4Address address);
DatabaseParameterV1 NpmFamilyAddressParameter(const network::IPv6Address& address);
DatabaseParameterV1 NpmFamilyAddressParameter(network::IPv6Address&&) = delete;
DatabaseParameterV1 NpmFamilyAddressParameter(const network::IPv6Address&&) = delete;
// Input arrays have passed RecordBatch::ValidateFull; the owner survives through prepared-command return.
arrow::Result<DatabaseParameterV1> NpmFamilyDatabaseParameter(const arrow::Array& values, int64_t row);
arrow::Result<DatabaseParameterV1> NpmFamilyDatabaseParameter(arrow::Array&&, int64_t) = delete;
arrow::Result<DatabaseParameterV1> NpmFamilyDatabaseParameter(const arrow::Array&&, int64_t) = delete;
arrow::Status ValidateNpmFamilyDatabaseReadback(const NpmFamilyDatabaseContract& contract,
                                                const arrow::RecordBatch& rows);

// Publication facts own all strings. Each basic family/history relation has its own cursor within a run.
struct NpmFamilyPublicationKey {
    std::string entity_id;
    std::string relation_name;
    std::string run_id;
};
struct NpmFamilyPublicationRequest {
    NpmFamilyPublicationKey key;
    int64_t period_ns = 0;
    int64_t closed_before_ns = 0;
};
struct NpmFamilyPublicationProof {
    bool confirmed_source = false;
    int64_t source_closed_before_ns = 0;
    bool writes_drained = false;
    bool foreground_drained = false;
    bool terminal_failure = false;
};
struct NpmFamilyPublishedProgress {
    NpmFamilyPublicationRequest publication;
    std::string epoch;               // run_id for this contract.
    int64_t committed_position = 0;  // Positive, relation-local durable position.
};
struct NpmFamilyPublicationCursor {
    std::optional<NpmFamilyPublishedProgress> last_confirmed;
    bool terminal_failure = false;
};
enum class NpmFamilyPublicationOutcome : uint8_t { kCommitted, kFailed, kCommitUnknown };

interface INpmFamilyProgressConsumer {
    virtual ~INpmFamilyProgressConsumer() = default;
    // Serialized, after gate validation. 0 proves a durable fact; failure clears output and is never retried.
    virtual int PublishProgress(const NpmFamilyPublicationRequest&, NpmFamilyPublishedProgress* confirmed) = 0;
};
arrow::Status ValidateNpmFamilyPublication(const NpmFamilyPublicationCursor& cursor,
                                           const NpmFamilyPublicationRequest& request,
                                           const NpmFamilyPublicationProof& proof);
// Pure transition: only a confirmed commit advances the cursor. Failure/unknown preserves its prefix and stops it.
arrow::Result<NpmFamilyPublicationCursor> CompleteNpmFamilyPublication(
    const NpmFamilyPublicationCursor& cursor, const NpmFamilyPublicationRequest& request,
    const NpmFamilyPublicationProof& proof, NpmFamilyPublicationOutcome outcome,
    const std::optional<NpmFamilyPublishedProgress>& confirmed);
// Exact relation/run binding; only source-confirmed immutable period increments become downstream progress.
arrow::Result<PublishedDatasetProgressV1> NpmFamilyDatasetProgress(const NpmFamilyPublishedProgress& fact,
                                                                   const PublishedDatasetRequestV1& request);

}  // namespace flowsql::npm
