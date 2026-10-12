// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_family_output_contract.h"

#include <arrow/api.h>

#include <limits>
#include <set>

namespace flowsql::npm {
namespace {
const NpmFamilyEntityKind kKinds[] = {NpmFamilyEntityKind::kBasic, NpmFamilyEntityKind::kSession,
                                      NpmFamilyEntityKind::kDns,   NpmFamilyEntityKind::kHttp1,
                                      NpmFamilyEntityKind::kTls,   NpmFamilyEntityKind::kIcmp};

arrow::Result<std::string> PhysicalType(NpmFamilyDatabaseBackend backend, const arrow::DataType& type) {
    const char* names[4]{};
    switch (type.id()) {
        case arrow::Type::BOOL:
            names[0] = "INTEGER";
            names[1] = "TINYINT(1)";
            names[2] = "BOOLEAN";
            names[3] = "Bool";
            break;
        case arrow::Type::UINT8:
            names[0] = "INTEGER";
            names[1] = "TINYINT UNSIGNED";
            names[2] = "SMALLINT";
            names[3] = "UInt8";
            break;
        case arrow::Type::UINT16:
            names[0] = "INTEGER";
            names[1] = "SMALLINT UNSIGNED";
            names[2] = "INTEGER";
            names[3] = "UInt16";
            break;
        case arrow::Type::UINT32:
            names[0] = "INTEGER";
            names[1] = "INT UNSIGNED";
            names[2] = "BIGINT";
            names[3] = "UInt32";
            break;
        case arrow::Type::UINT64:
            names[0] = "TEXT";
            names[1] = "BIGINT UNSIGNED";
            names[2] = "NUMERIC(20,0)";
            names[3] = "UInt64";
            break;
        case arrow::Type::INT64:
            names[0] = "INTEGER";
            names[1] = "BIGINT";
            names[2] = "BIGINT";
            names[3] = "Int64";
            break;
        case arrow::Type::DOUBLE:
            names[0] = "REAL";
            names[1] = "DOUBLE";
            names[2] = "DOUBLE PRECISION";
            names[3] = "Float64";
            break;
        case arrow::Type::STRING:
            names[0] = "TEXT";
            names[1] = "TEXT";
            names[2] = "TEXT";
            names[3] = "String";
            break;
        case arrow::Type::FIXED_SIZE_BINARY:
            if (static_cast<const arrow::FixedSizeBinaryType&>(type).byte_width() != 16)
                return arrow::Status::Invalid("NPM IPv6 requires exactly 16 bytes");
            names[0] = "BLOB";
            names[1] = "BINARY(16)";
            names[2] = "BYTEA";
            names[3] = "FixedString(16)";
            break;
        default:
            return arrow::Status::NotImplemented("NPM family database type: ", type.ToString());
    }
    const auto index = static_cast<size_t>(backend);
    if (index >= 4) return arrow::Status::Invalid("Unknown NPM database backend");
    return std::string(names[index]);
}

bool SameKey(const NpmFamilyPublicationKey& a, const NpmFamilyPublicationKey& b) {
    return a.entity_id == b.entity_id && a.relation_name == b.relation_name && a.run_id == b.run_id;
}
arrow::Status ValidateRequest(const NpmFamilyPublicationRequest& request) {
    if ((request.key.entity_id != "basic_ipv4" && request.key.entity_id != "basic_ipv6") ||
        request.key.relation_name != "npm_" + request.key.entity_id + "_history_v1" || request.key.run_id.empty() ||
        request.period_ns <= 0 || request.closed_before_ns < 0 || request.closed_before_ns % request.period_ns != 0)
        return arrow::Status::Invalid(
            "NPM publication requires a concrete basic history relation/run and period boundary");
    return arrow::Status::OK();
}
arrow::Status ValidateFact(const NpmFamilyPublishedProgress& fact) {
    ARROW_RETURN_NOT_OK(ValidateRequest(fact.publication));
    if (fact.epoch != fact.publication.key.run_id || fact.committed_position <= 0)
        return arrow::Status::Invalid("NPM publication requires a durable position and matching run epoch");
    return arrow::Status::OK();
}
}  // namespace

arrow::Result<NpmFamilyOutputSelection> ResolveNpmFamilyOutput(const NpmFamilyOutputOptions& options) {
    NpmFamilyOutputSelection selection;
    const auto features = options.features.empty() ? std::vector<std::string>{"basic"} : options.features;
    std::set<std::string> enabled;
    for (const auto& feature : features) {
        if (!enabled.insert(feature).second) return arrow::Status::Invalid("Duplicate NPM module: ", feature);
        bool found = false;
        for (auto kind : kKinds) {
            auto ipv4 = NpmFamilyEntityDescriptor(kind, packet::AddressFamily::kIPv4);
            if (ipv4.module_id != feature) continue;
            selection.enabled_entities.push_back(std::move(ipv4));
            selection.enabled_entities.push_back(NpmFamilyEntityDescriptor(kind, packet::AddressFamily::kIPv6));
            found = true;
            break;
        }
        if (!found) return arrow::Status::Invalid("Unknown NPM module: ", feature);
    }
    if (options.managed) selection.persisted_entities = selection.enabled_entities;
    if (options.observing) {
        for (const auto& entity : selection.enabled_entities) {
            if (entity.entity_id == *options.observing) selection.observed_entity = entity;
        }
        if (!selection.observed_entity)
            return arrow::Status::Invalid("observing must select an enabled concrete NPM family entity: ",
                                          *options.observing);
        selection.foreground_schema = selection.observed_entity->schema;
    } else {
        if (!options.managed) return arrow::Status::Invalid("NPM foreground output requires observing");
        if (options.has_result_where || options.has_result_projection)
            return arrow::Status::Invalid("NPM result WHERE/projection requires observing");
        selection.foreground_schema = arrow::schema({});
    }
    return selection;
}

arrow::Result<NpmFamilyDatabaseContract> NpmFamilyDatabaseMapping(NpmFamilyDatabaseBackend backend,
                                                                  NpmFamilyEntityKind kind,
                                                                  packet::AddressFamily family) {
    if (family != packet::AddressFamily::kIPv4 && family != packet::AddressFamily::kIPv6)
        return arrow::Status::Invalid("NPM database entity requires IPv4 or IPv6");
    NpmFamilyDatabaseContract contract;
    contract.backend = backend;
    contract.entity = NpmFamilyEntityDescriptor(kind, family);
    const auto prefix = "npm_" + contract.entity.entity_id;
    contract.relations = {prefix + "_v1_data", prefix + "_history_v1", prefix + "_latest_v1", prefix + "_final_v1"};
    for (const auto& field : contract.entity.schema->fields()) {
        ARROW_ASSIGN_OR_RAISE(auto type, PhysicalType(backend, *field->type()));
        if (backend == NpmFamilyDatabaseBackend::kClickHouse && field->nullable()) type = "Nullable(" + type + ')';
        const bool needs_check =
            backend == NpmFamilyDatabaseBackend::kSqlite || backend == NpmFamilyDatabaseBackend::kPostgres;
        contract.columns.push_back({field, std::move(type), needs_check && field->type()->id() == arrow::Type::UINT32,
                                    needs_check && field->type()->id() == arrow::Type::FIXED_SIZE_BINARY ? 16u : 0u});
    }
    return contract;
}

DatabaseParameterV1 NpmFamilyAddressParameter(network::IPv4Address address) {
    DatabaseParameterV1 parameter;
    parameter.kind = DatabaseParameterKindV1::kUInt64;
    parameter.uint64_value = address.ToUInt32();
    return parameter;
}
DatabaseParameterV1 NpmFamilyAddressParameter(const network::IPv6Address& address) {
    DatabaseParameterV1 parameter;
    parameter.kind = DatabaseParameterKindV1::kBlob;
    parameter.data = address.bytes;
    parameter.size = sizeof(address.bytes);
    return parameter;
}

arrow::Result<DatabaseParameterV1> NpmFamilyDatabaseParameter(const arrow::Array& values, int64_t row) {
    if (row < 0 || row >= values.length()) return arrow::Status::IndexError("NPM database parameter row");
    DatabaseParameterV1 parameter;
    if (values.IsNull(row)) return parameter;
    switch (values.type_id()) {
        case arrow::Type::BOOL:
            parameter.kind = DatabaseParameterKindV1::kInt64;
            parameter.int64_value = static_cast<const arrow::BooleanArray&>(values).Value(row);
            break;
        case arrow::Type::UINT8:
            parameter.kind = DatabaseParameterKindV1::kUInt64;
            parameter.uint64_value = static_cast<const arrow::UInt8Array&>(values).Value(row);
            break;
        case arrow::Type::UINT16:
            parameter.kind = DatabaseParameterKindV1::kUInt64;
            parameter.uint64_value = static_cast<const arrow::UInt16Array&>(values).Value(row);
            break;
        case arrow::Type::UINT32:
            parameter.kind = DatabaseParameterKindV1::kUInt64;
            parameter.uint64_value = static_cast<const arrow::UInt32Array&>(values).Value(row);
            break;
        case arrow::Type::UINT64:
            parameter.kind = DatabaseParameterKindV1::kUInt64;
            parameter.uint64_value = static_cast<const arrow::UInt64Array&>(values).Value(row);
            break;
        case arrow::Type::INT64:
            parameter.kind = DatabaseParameterKindV1::kInt64;
            parameter.int64_value = static_cast<const arrow::Int64Array&>(values).Value(row);
            break;
        case arrow::Type::DOUBLE:
            parameter.kind = DatabaseParameterKindV1::kDouble;
            parameter.double_value = static_cast<const arrow::DoubleArray&>(values).Value(row);
            break;
        case arrow::Type::STRING: {
            const auto bytes = static_cast<const arrow::StringArray&>(values).GetView(row);
            parameter.kind = DatabaseParameterKindV1::kString;
            parameter.data = bytes.data();
            parameter.size = bytes.size();
            break;
        }
        case arrow::Type::FIXED_SIZE_BINARY: {
            const auto& binary = static_cast<const arrow::FixedSizeBinaryArray&>(values);
            if (binary.byte_width() != 16) return arrow::Status::Invalid("NPM IPv6 parameter requires 16 bytes");
            parameter.kind = DatabaseParameterKindV1::kBlob;
            parameter.data = binary.GetValue(row);
            parameter.size = 16;
            break;
        }
        default:
            return arrow::Status::NotImplemented("NPM family parameter: ", values.type()->ToString());
    }
    return parameter;
}

arrow::Status ValidateNpmFamilyDatabaseReadback(const NpmFamilyDatabaseContract& contract,
                                                const arrow::RecordBatch& rows) {
    return ValidateNpmFamilyEntityRows(contract.entity, rows);
}

arrow::Status ValidateNpmFamilyPublication(const NpmFamilyPublicationCursor& cursor,
                                           const NpmFamilyPublicationRequest& request,
                                           const NpmFamilyPublicationProof& proof) {
    ARROW_RETURN_NOT_OK(ValidateRequest(request));
    if (cursor.terminal_failure || proof.terminal_failure)
        return arrow::Status::Invalid("NPM publication stopped after terminal failure/unknown commit");
    if (!proof.confirmed_source || proof.source_closed_before_ns < request.closed_before_ns || !proof.writes_drained ||
        !proof.foreground_drained)
        return arrow::Status::Invalid("NPM publication requires confirmed source progress and drained outputs");
    if (cursor.last_confirmed) {
        const auto& previous = *cursor.last_confirmed;
        ARROW_RETURN_NOT_OK(ValidateFact(previous));
        if (!SameKey(previous.publication.key, request.key) || previous.publication.period_ns != request.period_ns ||
            previous.publication.closed_before_ns >= request.closed_before_ns ||
            previous.committed_position == std::numeric_limits<int64_t>::max())
            return arrow::Status::Invalid("NPM publication must advance its own relation/run/period cursor");
    }
    return arrow::Status::OK();
}

arrow::Result<NpmFamilyPublicationCursor> CompleteNpmFamilyPublication(
    const NpmFamilyPublicationCursor& cursor, const NpmFamilyPublicationRequest& request,
    const NpmFamilyPublicationProof& proof, NpmFamilyPublicationOutcome outcome,
    const std::optional<NpmFamilyPublishedProgress>& confirmed) {
    ARROW_RETURN_NOT_OK(ValidateNpmFamilyPublication(cursor, request, proof));
    auto next = cursor;
    if (outcome == NpmFamilyPublicationOutcome::kFailed || outcome == NpmFamilyPublicationOutcome::kCommitUnknown) {
        next.terminal_failure = true;
        return next;
    }
    if (outcome != NpmFamilyPublicationOutcome::kCommitted || !confirmed)
        return arrow::Status::Invalid("NPM publication lacks a confirmed commit");
    ARROW_RETURN_NOT_OK(ValidateFact(*confirmed));
    const int64_t position = cursor.last_confirmed ? cursor.last_confirmed->committed_position + 1 : 1;
    if (!SameKey(request.key, confirmed->publication.key) || request.period_ns != confirmed->publication.period_ns ||
        request.closed_before_ns != confirmed->publication.closed_before_ns ||
        confirmed->committed_position != position)
        return arrow::Status::Invalid("NPM confirmed publication does not match its request/next position");
    next.last_confirmed = confirmed;
    return next;
}

arrow::Result<PublishedDatasetProgressV1> NpmFamilyDatasetProgress(const NpmFamilyPublishedProgress& fact,
                                                                   const PublishedDatasetRequestV1& request) {
    ARROW_RETURN_NOT_OK(ValidateFact(fact));
    if (request.dataset_id.empty() || request.run_id != fact.publication.key.run_id ||
        request.relation != fact.publication.key.relation_name || !request.npm_period_increment ||
        request.bucket_ns <= 0 || request.bucket_ns % fact.publication.period_ns != 0)
        return arrow::Status::Invalid(
            "NPM downstream publication requires its exact relation/run and compatible buckets");
    PublishedDatasetProgressV1 progress;
    progress.progress = {request.dataset_id, fact.epoch, std::to_string(fact.committed_position),
                         fact.publication.closed_before_ns / request.bucket_ns};
    progress.source_period_ns = fact.publication.period_ns;
    return progress;
}

}  // namespace flowsql::npm
