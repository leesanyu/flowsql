// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <operators/npm_basic/npm_family_output_contract.h>

#include <arrow/api.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <limits>
#include <type_traits>

namespace {
namespace npm = flowsql::npm;
namespace packet = flowsql::packet;
namespace network = flowsql::network;
using Backend = npm::NpmFamilyDatabaseBackend;
using ParameterKind = flowsql::DatabaseParameterKindV1;
using Outcome = npm::NpmFamilyPublicationOutcome;
static_assert(std::is_abstract_v<npm::INpmFamilyProgressConsumer>);
template <typename T, typename = void>
struct CanBindAddress : std::false_type {};
template <typename T>
struct CanBindAddress<T, std::void_t<decltype(npm::NpmFamilyAddressParameter(std::declval<T>()))>> : std::true_type {};
static_assert(CanBindAddress<network::IPv4Address>::value);
static_assert(CanBindAddress<const network::IPv6Address&>::value);
static_assert(!CanBindAddress<network::IPv6Address>::value);
static_assert(!CanBindAddress<const network::IPv6Address>::value);

void Check(const arrow::Status& status) {
    if (!status.ok()) std::fprintf(stderr, "%s\n", status.ToString().c_str());
    assert(status.ok());
}
template <typename T>
T Get(arrow::Result<T> result) {
    Check(result.status());
    return std::move(result).ValueOrDie();
}

void TestOutputSelection() {
    npm::NpmFamilyOutputOptions options;
    assert(!npm::ResolveNpmFamilyOutput(options).ok());
    options.observing = "basic_ipv4";
    auto selection = Get(npm::ResolveNpmFamilyOutput(options));
    assert(selection.enabled_entities.size() == 2 && selection.persisted_entities.empty());
    assert(selection.observed_entity->entity_id == "basic_ipv4");
    assert(selection.foreground_schema->Equals(*selection.observed_entity->schema, true));
    options.managed = true;
    options.has_result_where = true;
    options.has_result_projection = true;
    selection = Get(npm::ResolveNpmFamilyOutput(options));
    assert(selection.persisted_entities.size() == 2);
    assert(selection.persisted_entities[1].entity_id == "basic_ipv6");
    options.observing.reset();
    assert(!npm::ResolveNpmFamilyOutput(options).ok());
    options.has_result_where = false;
    assert(!npm::ResolveNpmFamilyOutput(options).ok());
    options.has_result_projection = false;
    selection = Get(npm::ResolveNpmFamilyOutput(options));
    assert(!selection.observed_entity && selection.foreground_schema->num_fields() == 0);
    auto empty = arrow::RecordBatch::Make(selection.foreground_schema, 0, arrow::ArrayVector{});
    Check(empty->ValidateFull());
    assert(empty->num_rows() == 0 && empty->num_columns() == 0);
    options.features = {"dns"};
    selection = Get(npm::ResolveNpmFamilyOutput(options));
    assert(selection.persisted_entities.size() == 2);
    assert(selection.persisted_entities[0].entity_id == "dns_transaction_ipv4");
    assert(selection.persisted_entities[1].entity_id == "dns_transaction_ipv6");
    for (const char* invalid : {"basic", "dns", "basic_ipv4", "dns_transaction", ""}) {
        options.observing = invalid;
        assert(!npm::ResolveNpmFamilyOutput(options).ok());
    }
    options.features = {"basic", "session", "dns", "http1", "tls", "icmp"};
    options.observing.reset();
    selection = Get(npm::ResolveNpmFamilyOutput(options));
    assert(selection.persisted_entities.size() == 12);
    for (const auto& entity : selection.enabled_entities) {
        options.observing = entity.entity_id;
        const auto selected = Get(npm::ResolveNpmFamilyOutput(options));
        assert(selected.persisted_entities.size() == 12);
        assert(selected.observed_entity->entity_id == entity.entity_id);
    }
    options.observing.reset();
    for (const auto& invalid : std::vector<std::vector<std::string>>{{"basic", "basic"}, {"basic_ipv4"}, {""}}) {
        options.features = invalid;
        assert(!npm::ResolveNpmFamilyOutput(options).ok());
    }
}

std::shared_ptr<arrow::RecordBatch> Fixture(const npm::NpmEntityDescriptorV1& entity) {
    arrow::ArrayVector columns;
    for (const auto& field : entity.schema->fields()) {
        auto builder = Get(arrow::MakeBuilder(field->type()));
        if (field->type()->id() == arrow::Type::UINT32) {
            Check(static_cast<arrow::UInt32Builder&>(*builder).AppendValues({0xffffffffu, 0x01020304u}));
        } else if (field->type()->id() == arrow::Type::FIXED_SIZE_BINARY) {
            const uint8_t bytes[16] = {0x20, 1, 0x0d, 0xb8, 0, 0xff, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 1};
            Check(static_cast<arrow::FixedSizeBinaryBuilder&>(*builder).Append(bytes));
            if (field->nullable())
                Check(builder->AppendNull());
            else
                Check(static_cast<arrow::FixedSizeBinaryBuilder&>(*builder).Append(bytes));
        } else if (field->name() == entity.identity_column || field->name() == "revision") {
            Check(static_cast<arrow::UInt64Builder&>(*builder).AppendValues({1, 1}));
        } else if (field->name() == "is_final") {
            Check(static_cast<arrow::BooleanBuilder&>(*builder).AppendValues(std::vector<uint8_t>{1, 1}));
        } else if (field->nullable()) {
            Check(builder->AppendNulls(2));
        } else {
            Check(builder->AppendEmptyValues(2));
        }
        columns.push_back(Get(builder->Finish()));
    }
    return arrow::RecordBatch::Make(entity.schema, 2, columns);
}

// Expectations are independent of the mapping function and include all scalar types in the twelve Schemas.
std::string ExpectedType(Backend backend, const arrow::Field& field) {
    const char* types[4]{};
    switch (field.type()->id()) {
        case arrow::Type::BOOL:
            types[0] = "INTEGER";
            types[1] = "TINYINT(1)";
            types[2] = "BOOLEAN";
            types[3] = "Bool";
            break;
        case arrow::Type::UINT8:
            types[0] = "INTEGER";
            types[1] = "TINYINT UNSIGNED";
            types[2] = "SMALLINT";
            types[3] = "UInt8";
            break;
        case arrow::Type::UINT16:
            types[0] = "INTEGER";
            types[1] = "SMALLINT UNSIGNED";
            types[2] = "INTEGER";
            types[3] = "UInt16";
            break;
        case arrow::Type::UINT32:
            types[0] = "INTEGER";
            types[1] = "INT UNSIGNED";
            types[2] = "BIGINT";
            types[3] = "UInt32";
            break;
        case arrow::Type::UINT64:
            types[0] = "TEXT";
            types[1] = "BIGINT UNSIGNED";
            types[2] = "NUMERIC(20,0)";
            types[3] = "UInt64";
            break;
        case arrow::Type::INT64:
            types[0] = "INTEGER";
            types[1] = "BIGINT";
            types[2] = "BIGINT";
            types[3] = "Int64";
            break;
        case arrow::Type::DOUBLE:
            types[0] = "REAL";
            types[1] = "DOUBLE";
            types[2] = "DOUBLE PRECISION";
            types[3] = "Float64";
            break;
        case arrow::Type::STRING:
            types[0] = "TEXT";
            types[1] = "TEXT";
            types[2] = "TEXT";
            types[3] = "String";
            break;
        case arrow::Type::FIXED_SIZE_BINARY:
            types[0] = "BLOB";
            types[1] = "BINARY(16)";
            types[2] = "BYTEA";
            types[3] = "FixedString(16)";
            break;
        default:
            assert(false);
    }
    std::string type = types[static_cast<size_t>(backend)];
    return backend == Backend::kClickHouse && field.nullable() ? "Nullable(" + type + ")" : type;
}

void TestAllBackendContracts() {
    for (auto backend : {Backend::kSqlite, Backend::kMysql, Backend::kPostgres, Backend::kClickHouse}) {
        for (auto kind :
             {npm::NpmFamilyEntityKind::kBasic, npm::NpmFamilyEntityKind::kSession, npm::NpmFamilyEntityKind::kDns,
              npm::NpmFamilyEntityKind::kHttp1, npm::NpmFamilyEntityKind::kTls, npm::NpmFamilyEntityKind::kIcmp}) {
            for (auto family : {packet::AddressFamily::kIPv4, packet::AddressFamily::kIPv6}) {
                const auto contract = Get(npm::NpmFamilyDatabaseMapping(backend, kind, family));
                const auto prefix = "npm_" + contract.entity.entity_id;
                assert(contract.relations.data == prefix + "_v1_data");
                assert(contract.relations.history == prefix + "_history_v1");
                assert(contract.relations.latest == prefix + "_latest_v1");
                assert(contract.relations.final == prefix + "_final_v1");
                assert(contract.columns.size() == static_cast<size_t>(contract.entity.schema->num_fields()));
                auto batch = Fixture(contract.entity);
                Check(npm::ValidateNpmFamilyDatabaseReadback(contract, *batch));
                auto slice = batch->Slice(1, 1);
                Check(npm::ValidateNpmFamilyDatabaseReadback(contract, *slice));
                for (size_t index = 0; index < contract.columns.size(); ++index) {
                    const auto& column = contract.columns[index];
                    assert(column.field->Equals(*contract.entity.schema->field(index), true));
                    assert(column.physical_type == ExpectedType(backend, *column.field));
                    const bool needs_check = backend == Backend::kSqlite || backend == Backend::kPostgres;
                    assert(column.check_uint32_range ==
                           (needs_check && column.field->type()->id() == arrow::Type::UINT32));
                    assert(column.check_binary_length ==
                           (needs_check && column.field->type()->id() == arrow::Type::FIXED_SIZE_BINARY ? 16u : 0u));
                    auto values = batch->column(index);
                    auto first = Get(npm::NpmFamilyDatabaseParameter(*values, 0));
                    auto second = Get(npm::NpmFamilyDatabaseParameter(*slice->column(index), 0));
                    if (column.field->type()->id() == arrow::Type::UINT32) {
                        assert(first.kind == ParameterKind::kUInt64 && first.uint64_value == 0xffffffffu);
                        assert(second.kind == ParameterKind::kUInt64 && second.uint64_value == 0x01020304u);
                    } else if (column.field->type()->id() == arrow::Type::FIXED_SIZE_BINARY) {
                        const auto* bytes = static_cast<const uint8_t*>(first.data);
                        assert(first.kind == ParameterKind::kBlob && first.size == 16);
                        assert(bytes[4] == 0 && bytes[5] == 0xff && bytes[6] == 0x80 && bytes[15] == 1);
                        assert(first.data == static_cast<const arrow::FixedSizeBinaryArray&>(*values).GetValue(0));
                        if (column.field->nullable()) assert(second.kind == ParameterKind::kNull);
                    }
                }
                auto fields = contract.entity.schema->fields();
                for (auto& field : fields) {
                    if (field->metadata() && field->metadata()->Contains("flowsql.semantic"))
                        field = field->RemoveMetadata();
                }
                auto stripped = arrow::schema(fields, contract.entity.schema->metadata());
                assert(!npm::ValidateNpmFamilyDatabaseReadback(contract,
                                                               *arrow::RecordBatch::Make(stripped, 2, batch->columns()))
                            .ok());
            }
        }
    }
    auto parameter = npm::NpmFamilyAddressParameter(network::IPv4Address::FromUInt32(0x01020304u));
    assert(parameter.kind == ParameterKind::kUInt64 && parameter.uint64_value == 0x01020304u && !parameter.data);
    network::IPv6Address address{{0x20, 1, 0xd, 0xb8, 0, 0xff, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 1}};
    parameter = npm::NpmFamilyAddressParameter(address);
    assert(parameter.kind == ParameterKind::kBlob && parameter.data == address.bytes && parameter.size == 16);
    arrow::StringBuilder strings;
    Check(strings.Append(std::string("a\0b", 3)));
    auto text = Get(strings.Finish());
    parameter = Get(npm::NpmFamilyDatabaseParameter(*text, 0));
    assert(parameter.kind == ParameterKind::kString && parameter.size == 3);
    assert(std::memcmp(parameter.data, "a\0b", 3) == 0);
    assert(!npm::NpmFamilyDatabaseParameter(*text, 1).ok());
    assert(!npm::NpmFamilyDatabaseParameter(*text, -1).ok());
    arrow::FixedSizeBinaryBuilder short_binary(arrow::fixed_size_binary(15));
    Check(short_binary.Append(reinterpret_cast<const uint8_t*>("123456789012345")));
    auto short_array = Get(short_binary.Finish());
    assert(!npm::NpmFamilyDatabaseParameter(*short_array, 0).ok());
}

npm::NpmFamilyPublishedProgress Fact(const npm::NpmFamilyPublicationRequest& request, int64_t position) {
    return {request, request.key.run_id, position};
}
void TestPublication() {
    npm::NpmFamilyPublicationRequest request4{{"basic_ipv4", "npm_basic_ipv4_history_v1", "run-1"}, 10, 20};
    npm::NpmFamilyPublicationRequest request6{{"basic_ipv6", "npm_basic_ipv6_history_v1", "run-1"}, 10, 20};
    npm::NpmFamilyPublicationProof proof{true, 30, true, true, false};
    npm::NpmFamilyPublicationCursor cursor4, cursor6;
    Check(npm::ValidateNpmFamilyPublication(cursor4, request4, proof));
    Check(npm::ValidateNpmFamilyPublication(cursor6, request6, proof));  // Empty IPv6 still uses true source progress.
    for (int missing = 0; missing < 5; ++missing) {
        auto blocked = proof;
        if (missing == 0) blocked.confirmed_source = false;
        if (missing == 1) blocked.writes_drained = false;
        if (missing == 2) blocked.foreground_drained = false;
        if (missing == 3) blocked.terminal_failure = true;
        if (missing == 4) blocked.source_closed_before_ns = 10;
        assert(!npm::ValidateNpmFamilyPublication(cursor4, request4, blocked).ok());
    }
    cursor4 = Get(npm::CompleteNpmFamilyPublication(cursor4, request4, proof, Outcome::kCommitted, Fact(request4, 1)));
    cursor6 = Get(npm::CompleteNpmFamilyPublication(cursor6, request6, proof, Outcome::kCommitted, Fact(request6, 1)));
    assert(cursor4.last_confirmed->publication.key.entity_id == "basic_ipv4");
    assert(cursor6.last_confirmed->publication.key.entity_id == "basic_ipv6");
    assert(cursor4.last_confirmed->publication.closed_before_ns ==
           cursor6.last_confirmed->publication.closed_before_ns);
    assert(!npm::ValidateNpmFamilyPublication(cursor4, request4, proof)
                .ok());  // No duplicate publication within a relation.
    request4.closed_before_ns = 30;
    assert(!npm::ValidateNpmFamilyPublication(cursor6, request4, proof).ok());
    const auto before = cursor4.last_confirmed;
    for (auto outcome : {Outcome::kFailed, Outcome::kCommitUnknown}) {
        const auto failed = Get(npm::CompleteNpmFamilyPublication(cursor4, request4, proof, outcome, std::nullopt));
        assert(failed.terminal_failure && failed.last_confirmed->committed_position == 1);
        assert(failed.last_confirmed->publication.closed_before_ns == 20);
        assert(!npm::ValidateNpmFamilyPublication(failed, request4, proof).ok());
        assert(cursor4.last_confirmed->committed_position == before->committed_position);
    }
    assert(!npm::CompleteNpmFamilyPublication(cursor4, request4, proof, Outcome::kCommitted, std::nullopt).ok());
    auto bad = Fact(request4, 2);
    bad.epoch = "other-run";
    assert(!npm::CompleteNpmFamilyPublication(cursor4, request4, proof, Outcome::kCommitted, bad).ok());
    bad = Fact(request4, 3);
    assert(!npm::CompleteNpmFamilyPublication(cursor4, request4, proof, Outcome::kCommitted, bad).ok());
    bad = Fact(request6, 2);
    assert(!npm::CompleteNpmFamilyPublication(cursor4, request4, proof, Outcome::kCommitted, bad).ok());
    cursor4 = Get(npm::CompleteNpmFamilyPublication(cursor4, request4, proof, Outcome::kCommitted, Fact(request4, 2)));
    assert(cursor4.last_confirmed->committed_position == 2 && cursor6.last_confirmed->committed_position == 1);
    flowsql::PublishedDatasetRequestV1 selected{"source", "", request4.key.relation_name, "run-1", 20, true};
    const auto progress = Get(npm::NpmFamilyDatasetProgress(*cursor4.last_confirmed, selected));
    assert(progress.progress.dataset_id == "source" && progress.progress.epoch == "run-1");
    assert(progress.progress.committed_position == "2" && progress.progress.closed_before_bucket == 1);
    assert(progress.source_period_ns == 10);
    selected.relation = request6.key.relation_name;
    assert(!npm::NpmFamilyDatasetProgress(*cursor4.last_confirmed, selected).ok());
    assert(Get(npm::NpmFamilyDatasetProgress(*cursor6.last_confirmed, selected)).progress.committed_position == "1");
    selected.run_id = "other-run";
    assert(!npm::NpmFamilyDatasetProgress(*cursor6.last_confirmed, selected).ok());
    selected.run_id = "run-1";
    selected.bucket_ns = 15;
    assert(!npm::NpmFamilyDatasetProgress(*cursor6.last_confirmed, selected).ok());
    selected.bucket_ns = 20;
    selected.npm_period_increment = false;
    assert(!npm::NpmFamilyDatasetProgress(*cursor6.last_confirmed, selected).ok());
}
}  // namespace

int main() {
    TestOutputSelection();
    TestAllBackendContracts();
    TestPublication();
    std::puts("NPM family output contract: selection, 48 backend/entity mappings and independent publication passed");
}
