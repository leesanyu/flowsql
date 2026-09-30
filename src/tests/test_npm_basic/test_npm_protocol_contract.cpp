// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <framework/interfaces/iblock_transform_operator.h>
#include <operators/npm_basic/modules/dns/npm_dns_contract.h>
#include <operators/npm_basic/npm_protocol_contract.h>

#include <arrow/api.h>

#include <array>
#include <cassert>
#include <cstdio>
#include <optional>
#include <tuple>
#include <type_traits>
#include <vector>

namespace npm = flowsql::npm;
using Error = npm::NpmProtocolContractErrorV1;

static_assert(std::is_abstract_v<npm::INpmProtocolModuleV1>);
static_assert(std::is_abstract_v<npm::INpmResultEmitterV1>);
static_assert(std::is_abstract_v<npm::INpmResultConsumerV1>);
static_assert(std::is_abstract_v<npm::INpmManagedResultConsumerV1>);
static_assert(std::is_abstract_v<npm::INpmResultConsumerFactoryV1>);
static_assert(std::is_abstract_v<flowsql::IBlockTransformManagedSinkTaskV1>);
static_assert(std::is_base_of_v<npm::INpmResultConsumerV1, npm::INpmManagedResultConsumerV1>);
static_assert(std::has_virtual_destructor_v<npm::INpmProtocolModuleV1>);
static_assert(std::has_virtual_destructor_v<npm::INpmResultEmitterV1>);
static_assert(std::has_virtual_destructor_v<npm::INpmResultConsumerV1>);
static_assert(std::has_virtual_destructor_v<npm::INpmManagedResultConsumerV1>);
static_assert(std::has_virtual_destructor_v<npm::INpmResultConsumerFactoryV1>);
static_assert(std::has_virtual_destructor_v<flowsql::IBlockTransformManagedSinkTaskV1>);

namespace {

void Expect(const npm::NpmProtocolContractStatusV1& status, Error error) {
    if (status.error != error) {
        std::fprintf(stderr, "expected error %u, got %u at %s row %lld\n", static_cast<unsigned>(error),
                     static_cast<unsigned>(status.error), status.field.c_str(), static_cast<long long>(status.row));
    }
    assert(status.error == error);
    if (error != Error::kNone) assert(!status.field.empty());
}

npm::NpmEntityDescriptorV1 Entity() {
    npm::NpmEntityDescriptorV1 entity;
    entity.entity_id = "test_transaction";
    entity.module_id = "test_module";
    entity.schema = arrow::schema(
        {arrow::field("entity_instance_id", arrow::uint64(), false), arrow::field("revision", arrow::uint64(), false),
         arrow::field("observed_at", arrow::int64(), false), arrow::field("is_final", arrow::boolean(), false),
         arrow::field("session_id", arrow::uint64(), true)});
    return entity;
}

template <typename Builder, typename Value>
std::shared_ptr<arrow::Array> Values(std::initializer_list<std::optional<Value>> values) {
    Builder builder;
    for (const auto& value : values) {
        assert((value ? builder.Append(*value) : builder.AppendNull()).ok());
    }
    std::shared_ptr<arrow::Array> result;
    assert(builder.Finish(&result).ok());
    return result;
}

std::shared_ptr<arrow::RecordBatch> Rows(const npm::NpmEntityDescriptorV1& entity) {
    return arrow::RecordBatch::Make(
        entity.schema, 3,
        {Values<arrow::UInt64Builder, uint64_t>({1, 2, 3}), Values<arrow::UInt64Builder, uint64_t>({1, 1, 1}),
         Values<arrow::Int64Builder, int64_t>({100, 101, 102}), Values<arrow::BooleanBuilder, bool>({true, true, true}),
         Values<arrow::UInt64Builder, uint64_t>({7, 7, std::nullopt})});
}

void TestEntitiesAndRows() {
    for (bool labeling : {false, true}) {
        const auto basic = npm::NpmBasicEntityDescriptorV1(labeling);
        const auto session = npm::NpmSessionEntityDescriptorV1(labeling);
        Expect(npm::ValidateNpmEntityDescriptorV1(basic), Error::kNone);
        Expect(npm::ValidateNpmEntityDescriptorV1(session), Error::kNone);
        assert(basic.entity_id == "basic" && basic.module_id == "basic");
        assert(session.entity_id == "session" && session.module_id == "session");
        assert(basic.identity_column == "session_id" && session.identity_column == "session_id");
        const uint32_t expected_version = labeling ? 2 : 1;
        const std::string expected_version_text = std::to_string(expected_version);
        assert(basic.schema_version == expected_version && session.schema_version == expected_version);
        assert(basic.schema == npm::NpmBasicResultSchema(labeling));
        assert(session.schema == npm::NpmSessionResultSchema(labeling));
        assert(basic.schema->metadata()->Get("flowsql.schema_version").ValueOrDie() == expected_version_text);
        assert(session.schema->metadata()->Get("flowsql.schema_version").ValueOrDie() == expected_version_text);
        assert((basic.schema->GetFieldIndex("primary_label_id") >= 0) == labeling);
        assert((session.schema->GetFieldIndex("primary_label_id") >= 0) == labeling);
    }

    const auto entity = Entity();
    Expect(npm::ValidateNpmEntityDescriptorV1(entity), Error::kNone);
    auto rows = Rows(entity);
    Expect(npm::ValidateNpmEntityRowsV1("test_module", entity, *rows), Error::kNone);
    Expect(npm::ValidateNpmEntityRowsV1("wrong_module", entity, *rows), Error::kWrongModule);
    Expect(npm::ValidateNpmEntityRowsV1("test_module", entity, *rows->Slice(0, 0)), Error::kNone);
    // Two independent transactions share one session; a third has no session at all.
    assert(rows->column(4)->null_count() == 1);

    for (int variation = 0; variation < 4; ++variation) {
        auto invalid = entity;
        if (variation == 0) invalid.entity_id.clear();
        if (variation == 1) invalid.module_id.clear();
        if (variation == 2) invalid.schema_version = 0;
        if (variation == 3) invalid.revision_semantics = static_cast<npm::NpmRevisionSemanticsV1>(99);
        Expect(npm::ValidateNpmEntityDescriptorV1(invalid), Error::kInvalidEntity);
    }
    auto invalid = entity;
    invalid.schema.reset();
    Expect(npm::ValidateNpmEntityDescriptorV1(invalid), Error::kInvalidSchema);
    invalid = entity;
    invalid.identity_column = "missing";
    Expect(npm::ValidateNpmEntityDescriptorV1(invalid), Error::kInvalidSchema);
    invalid.identity_column = "revision";
    Expect(npm::ValidateNpmEntityDescriptorV1(invalid), Error::kInvalidSchema);
    for (const auto& field : {arrow::field("entity_instance_id", arrow::utf8(), false),
                              arrow::field("entity_instance_id", arrow::uint64(), true)}) {
        invalid = entity;
        invalid.schema = entity.schema->SetField(0, field).ValueOrDie();
        Expect(npm::ValidateNpmEntityDescriptorV1(invalid), Error::kInvalidSchema);
    }
    invalid = entity;
    invalid.schema = entity.schema->AddField(5, entity.schema->field(0)).ValueOrDie();
    Expect(npm::ValidateNpmEntityDescriptorV1(invalid), Error::kInvalidSchema);

    for (int column : {0, 1, 2, 3}) {
        auto columns = rows->columns();
        if (column < 2) columns[column] = Values<arrow::UInt64Builder, uint64_t>({1, std::nullopt, 3});
        if (column == 2) columns[column] = Values<arrow::Int64Builder, int64_t>({1, std::nullopt, 3});
        if (column == 3) columns[column] = Values<arrow::BooleanBuilder, bool>({true, std::nullopt, true});
        auto bad_rows = arrow::RecordBatch::Make(entity.schema, 3, columns);
        Expect(npm::ValidateNpmEntityRowsV1("test_module", entity, *bad_rows), Error::kInvalidRows);
    }
    for (int column : {0, 1}) {
        auto columns = rows->columns();
        columns[column] = Values<arrow::UInt64Builder, uint64_t>({1, 0, 3});
        const auto bad_rows = arrow::RecordBatch::Make(entity.schema, 3, columns);
        const auto status = npm::ValidateNpmEntityRowsV1("test_module", entity, *bad_rows);
        Expect(status, Error::kInvalidRows);
        assert(status.row == 1);
    }
    auto columns = rows->columns();
    columns[0] = Values<arrow::UInt64Builder, uint64_t>({1});
    Expect(npm::ValidateNpmEntityRowsV1("test_module", entity, *arrow::RecordBatch::Make(entity.schema, 3, columns)),
           Error::kInvalidRows);
    auto changed_schema = entity.schema->WithMetadata(arrow::key_value_metadata({"test"}, {"different"}));
    Expect(npm::ValidateNpmEntityRowsV1("test_module", entity,
                                        *arrow::RecordBatch::Make(changed_schema, 3, rows->columns())),
           Error::kInvalidSchema);

    auto event = entity;
    event.revision_semantics = npm::NpmRevisionSemanticsV1::kEvent;
    Expect(npm::ValidateNpmEntityRowsV1("test_module", event, *rows), Error::kNone);
    columns = rows->columns();
    columns[1] = Values<arrow::UInt64Builder, uint64_t>({1, 2, 1});
    auto updated = arrow::RecordBatch::Make(entity.schema, 3, columns);
    Expect(npm::ValidateNpmEntityRowsV1("test_module", entity, *updated), Error::kNone);
    Expect(npm::ValidateNpmEntityRowsV1("test_module", event, *updated), Error::kInvalidRows);
    columns = rows->columns();
    columns[3] = Values<arrow::BooleanBuilder, bool>({true, false, true});
    updated = arrow::RecordBatch::Make(entity.schema, 3, columns);
    Expect(npm::ValidateNpmEntityRowsV1("test_module", entity, *updated), Error::kNone);
    Expect(npm::ValidateNpmEntityRowsV1("test_module", event, *updated), Error::kInvalidRows);
}

void TestPlans() {
    npm::NpmModulePlanV1 plan;
    plan.module_id = "test_module";
    plan.input_mask = 1;
    plan.entities = {Entity()};
    npm::NpmModuleCapabilitiesV1 capabilities;
    Expect(npm::ValidateNpmModulePlanV1(plan, capabilities), Error::kNone);
    auto invalid = plan;
    invalid.module_id.clear();
    Expect(npm::ValidateNpmModulePlanV1(invalid, capabilities), Error::kInvalidPlan);
    for (uint8_t mask : {0, 8, 255}) {
        invalid = plan;
        invalid.input_mask = mask;
        Expect(npm::ValidateNpmModulePlanV1(invalid, capabilities), Error::kInvalidPlan);
    }
    invalid = plan;
    invalid.entities.push_back(Entity());
    Expect(npm::ValidateNpmModulePlanV1(invalid, capabilities), Error::kInvalidEntity);
    invalid = plan;
    invalid.entities[0].module_id = "another";
    Expect(npm::ValidateNpmModulePlanV1(invalid, capabilities), Error::kWrongModule);
    invalid = plan;
    invalid.entities[0].schema.reset();
    Expect(npm::ValidateNpmModulePlanV1(invalid, capabilities), Error::kInvalidSchema);

    plan.requires_labeling = true;
    Expect(npm::ValidateNpmModulePlanV1(plan, capabilities), Error::kUnavailableCapability);
    capabilities.labeling_available = true;
    Expect(npm::ValidateNpmModulePlanV1(plan, capabilities), Error::kUnavailableCapability);
    capabilities.labeling_enabled = true;
    capabilities.available_label_ids = {10, 20};
    Expect(npm::ValidateNpmModulePlanV1(plan, capabilities), Error::kNone);
    for (const auto& labels : std::vector<std::vector<uint32_t>>{{}, {0}, {10, 10}, {30}}) {
        plan.primary_label_ids = labels;
        Expect(npm::ValidateNpmModulePlanV1(plan, capabilities), Error::kInvalidLabelSelection);
    }
    plan.primary_label_ids = {20, 10};
    Expect(npm::ValidateNpmModulePlanV1(plan, capabilities), Error::kNone);
    plan.requires_labeling = false;
    Expect(npm::ValidateNpmModulePlanV1(plan, capabilities), Error::kInvalidPlan);
    plan.requires_labeling = true;
    plan.input_mask = 4;
    Expect(npm::ValidateNpmModulePlanV1(plan, capabilities), Error::kInvalidPlan);
    plan.input_mask = 1;
    plan.requires_tcp_stream = true;
    Expect(npm::ValidateNpmModulePlanV1(plan, capabilities), Error::kUnavailableCapability);
    capabilities.tcp_stream_available = true;
    Expect(npm::ValidateNpmModulePlanV1(plan, capabilities), Error::kNone);
    plan.primary_label_ids.reset();
    Expect(npm::ValidateNpmModulePlanV1(plan, capabilities), Error::kInvalidPlan);
    plan.primary_label_ids = {10};
    plan.input_mask = 2;
    Expect(npm::ValidateNpmModulePlanV1(plan, capabilities), Error::kInvalidPlan);
}

void TestDnsConfigAndPlan() {
    using ConfigError = npm::NpmDnsConfigErrorV1;
    npm::NpmDnsConfigV1 config;
    config.primary_label_ids = {77};
    const auto reject = [&](std::string_view json, ConfigError error, std::string_view path) {
        const auto status = npm::ParseNpmDnsConfigV1(json, &config);
        assert(status.error == error && status.path == path);
        assert(config.primary_label_ids == std::vector<uint32_t>{77});
    };
    reject("", ConfigError::kMissing, "/dns");
    reject("null", ConfigError::kInvalidType, "/dns");
    reject("{}", ConfigError::kMissing, "/dns/primary_label_ids");
    reject(R"({"primary_label_ids":null})", ConfigError::kInvalidType, "/dns/primary_label_ids");
    reject(R"({"primary_label_ids":[]})", ConfigError::kInvalidRange, "/dns/primary_label_ids");
    reject(R"({"primary_label_ids":[0]})", ConfigError::kInvalidRange, "/dns/primary_label_ids/0");
    reject(R"({"primary_label_ids":[1,1]})", ConfigError::kInvalidRange, "/dns/primary_label_ids/1");
    reject(R"({"primary_label_ids":[4294967296]})", ConfigError::kInvalidRange, "/dns/primary_label_ids/0");
    reject(R"({"primary_label_ids":["1"]})", ConfigError::kInvalidType, "/dns/primary_label_ids/0");
    reject(R"({"primary_label_ids":[1],"extra":0})", ConfigError::kUnknownField, "/dns/extra");
    reject(R"({"primary_label_ids":[1],"primary_label_ids":[2]})", ConfigError::kDuplicateField,
           "/dns/primary_label_ids");
    reject(R"({"primary_label_ids":[1],"response_timeout_ns":null})", ConfigError::kInvalidType,
           "/dns/response_timeout_ns");
    reject(R"({"primary_label_ids":[1],"response_timeout_ns":999999})", ConfigError::kInvalidRange,
           "/dns/response_timeout_ns");
    reject(R"({"primary_label_ids":[1],"response_timeout_ns":300000000001})", ConfigError::kInvalidRange,
           "/dns/response_timeout_ns");
    reject(R"({"primary_label_ids":[1],"max_pending_per_session":0})", ConfigError::kInvalidRange,
           "/dns/max_pending_per_session");
    reject(R"({"primary_label_ids":[1],"max_pending_per_session":4097})", ConfigError::kInvalidRange,
           "/dns/max_pending_per_session");
    std::string labels_json = R"({"primary_label_ids":[)";
    for (uint32_t id = 1; id <= 257; ++id) {
        if (id != 1) labels_json += ',';
        labels_json += std::to_string(id);
    }
    labels_json += "]}";
    reject(labels_json, ConfigError::kInvalidRange, "/dns/primary_label_ids");
    labels_json.erase(labels_json.rfind(",257"), 4);
    npm::NpmDnsConfigV1 boundary;
    assert(npm::ParseNpmDnsConfigV1(labels_json, &boundary).error == ConfigError::kNone);
    assert(boundary.primary_label_ids.size() == 256);
    assert(npm::ParseNpmDnsConfigV1(R"({"primary_label_ids":[1001,2]})", &config).error == ConfigError::kNone);
    assert(config.primary_label_ids == (std::vector<uint32_t>{1001, 2}));
    assert(config.response_timeout_ns == 5'000'000'000 && config.max_pending_per_session == 256);
    std::string json = R"({"primary_label_ids":[1001],"response_timeout_ns":1000000,"max_pending_per_session":4096})";
    assert(npm::ParseNpmDnsConfigV1(json, &config).error == ConfigError::kNone);
    json.assign(json.size(), 'x');
    assert(config.primary_label_ids == std::vector<uint32_t>{1001});
    assert(config.response_timeout_ns == 1'000'000 && config.max_pending_per_session == 4096);

    const auto plan = npm::NpmDnsModulePlanV1(config);
    assert(plan.module_id == "dns" && plan.input_mask == 3);
    assert(plan.requires_labeling && plan.requires_tcp_stream);
    assert(plan.primary_label_ids == config.primary_label_ids);
    npm::NpmModuleCapabilitiesV1 capabilities;
    capabilities.available_label_ids = {1001};
    Expect(npm::ValidateNpmModulePlanV1(plan, capabilities), Error::kUnavailableCapability);
    capabilities.labeling_enabled = true;
    Expect(npm::ValidateNpmModulePlanV1(plan, capabilities), Error::kUnavailableCapability);
    capabilities.labeling_available = true;
    Expect(npm::ValidateNpmModulePlanV1(plan, capabilities), Error::kUnavailableCapability);
    capabilities.tcp_stream_available = true;
    Expect(npm::ValidateNpmModulePlanV1(plan, capabilities), Error::kNone);
    capabilities.available_label_ids.clear();
    Expect(npm::ValidateNpmModulePlanV1(plan, capabilities), Error::kInvalidLabelSelection);
}

void TestDnsEntitySchemaAndRows() {
    const auto entity = npm::NpmDnsTransactionEntityDescriptorV1();
    Expect(npm::ValidateNpmEntityDescriptorV1(entity), Error::kNone);
    assert(entity.entity_id == "dns_transaction" && entity.module_id == "dns");
    assert(entity.schema_version == 1 && entity.revision_semantics == npm::NpmRevisionSemanticsV1::kEvent);
    assert(entity.schema->metadata()->Get("flowsql.schema_version").ValueOrDie() == "1");
    assert(entity.schema->metadata()->Get("flowsql.revision_semantics").ValueOrDie() == "event");
    const std::vector<std::tuple<const char*, arrow::Type::type, bool>> fields = {
        {"entity_instance_id", arrow::Type::UINT64, false},
        {"revision", arrow::Type::UINT64, false},
        {"observed_at", arrow::Type::INT64, false},
        {"is_final", arrow::Type::BOOL, false},
        {"session_id", arrow::Type::UINT64, false},
        {"observation_domain_id", arrow::Type::UINT64, false},
        {"transport_protocol", arrow::Type::UINT8, false},
        {"a_ip", arrow::Type::STRING, false},
        {"b_ip", arrow::Type::STRING, false},
        {"a_port", arrow::Type::UINT16, false},
        {"b_port", arrow::Type::UINT16, false},
        {"dns_id", arrow::Type::UINT16, false},
        {"outcome", arrow::Type::STRING, false},
        {"query_retries", arrow::Type::UINT32, false},
        {"query_direction", arrow::Type::UINT8, true},
        {"response_direction", arrow::Type::UINT8, true},
        {"qname", arrow::Type::STRING, true},
        {"qtype", arrow::Type::UINT16, true},
        {"qclass", arrow::Type::UINT16, true},
        {"query_at_ns", arrow::Type::INT64, true},
        {"response_at_ns", arrow::Type::INT64, true},
        {"latency_ns", arrow::Type::INT64, true},
        {"response_rcode", arrow::Type::UINT16, true},
        {"response_tc", arrow::Type::BOOL, true},
        {"incomplete_reason", arrow::Type::STRING, true}};
    assert(entity.schema->num_fields() == static_cast<int>(fields.size()));
    std::vector<std::shared_ptr<arrow::Array>> columns;
    for (size_t index = 0; index < fields.size(); ++index) {
        const auto& [name, type, nullable] = fields[index];
        const auto& field = entity.schema->field(static_cast<int>(index));
        assert(field->name() == name && field->type()->id() == type && field->nullable() == nullable);
        if (nullable) {
            columns.push_back(arrow::MakeArrayOfNull(field->type(), 1).ValueOrDie());
        } else if (type == arrow::Type::UINT64) {
            columns.push_back(Values<arrow::UInt64Builder, uint64_t>({1}));
        } else if (type == arrow::Type::INT64) {
            columns.push_back(Values<arrow::Int64Builder, int64_t>({100}));
        } else if (type == arrow::Type::BOOL) {
            columns.push_back(Values<arrow::BooleanBuilder, bool>({true}));
        } else if (type == arrow::Type::UINT8) {
            columns.push_back(Values<arrow::UInt8Builder, uint8_t>({17}));
        } else if (type == arrow::Type::UINT16) {
            columns.push_back(Values<arrow::UInt16Builder, uint16_t>({53}));
        } else if (type == arrow::Type::UINT32) {
            columns.push_back(Values<arrow::UInt32Builder, uint32_t>({0}));
        } else {
            columns.push_back(Values<arrow::StringBuilder, std::string>({std::string("x")}));
        }
    }
    auto rows = arrow::RecordBatch::Make(entity.schema, 1, columns);
    Expect(npm::ValidateNpmEntityRowsV1("dns", entity, *rows), Error::kNone);
    columns[1] = Values<arrow::UInt64Builder, uint64_t>({2});
    Expect(npm::ValidateNpmEntityRowsV1("dns", entity, *arrow::RecordBatch::Make(entity.schema, 1, columns)),
           Error::kInvalidRows);
    columns[1] = Values<arrow::UInt64Builder, uint64_t>({1});
    columns[3] = Values<arrow::BooleanBuilder, bool>({false});
    Expect(npm::ValidateNpmEntityRowsV1("dns", entity, *arrow::RecordBatch::Make(entity.schema, 1, columns)),
           Error::kInvalidRows);
}

void TestInputRelationships() {
    std::array<uint8_t, 32> bytes{};
    flowsql::packet::PacketLayerInfo layer;
    layer.transport_protocol = 6;
    npm::NpmSessionKey key;
    key.transport_protocol = 6;
    key.observation_domain_id = 42;
    npm::NpmSessionView session;
    session.session_id = 1;
    session.key = &key;
    npm::NpmPacketView transport;
    transport.packet.bytes = {bytes.data(), bytes.size()};
    transport.packet.meta.captured_len = bytes.size();
    transport.layer = &layer;
    transport.payload = {bytes.data() + 24, 8};
    transport.transport.payload_wire_bytes = 8;
    transport.transport.payload_captured_bytes = 8;
    transport.transport.tcp.valid = true;
    npm::NpmInputEventV1 input;
    input.observation_domain_id = 42;
    input.packet = transport.packet;
    input.layer = &layer;
    input.body = transport.payload;
    input.transport = &transport;
    input.session = &session;
    Expect(npm::ValidateNpmInputEventV1(input), Error::kNone);
    auto invalid = input;
    invalid.session = nullptr;
    Expect(npm::ValidateNpmInputEventV1(invalid), Error::kInvalidInput);
    invalid = input;
    invalid.kind = static_cast<npm::NpmInputKindV1>(3);
    Expect(npm::ValidateNpmInputEventV1(invalid), Error::kInvalidInput);
    invalid = input;
    invalid.body = {bytes.data() + 31, 2};
    Expect(npm::ValidateNpmInputEventV1(invalid), Error::kInvalidInput);
    invalid = input;
    invalid.body = {nullptr, 1};
    Expect(npm::ValidateNpmInputEventV1(invalid), Error::kInvalidInput);
    invalid = input;
    invalid.observation_domain_id = 43;
    Expect(npm::ValidateNpmInputEventV1(invalid), Error::kInvalidInput);
    invalid = input;
    invalid.transport = nullptr;
    Expect(npm::ValidateNpmInputEventV1(invalid), Error::kInvalidInput);
    invalid = input;
    invalid.layer = nullptr;
    Expect(npm::ValidateNpmInputEventV1(invalid), Error::kInvalidInput);

    input.kind = npm::NpmInputKindV1::kUdpDatagram;
    Expect(npm::ValidateNpmInputEventV1(input), Error::kInvalidInput);
    key.transport_protocol = layer.transport_protocol = 17;
    transport.transport.tcp.valid = false;
    Expect(npm::ValidateNpmInputEventV1(input), Error::kNone);
    input.body_complete = false;
    transport.transport.payload_complete = false;
    transport.transport.payload_wire_bytes = 12;
    Expect(npm::ValidateNpmInputEventV1(input), Error::kNone);
    input.body_complete = true;
    Expect(npm::ValidateNpmInputEventV1(input), Error::kInvalidInput);
    input.body_complete = false;

    input.kind = npm::NpmInputKindV1::kControlPacket;
    layer.transport_protocol = 1;
    Expect(npm::ValidateNpmInputEventV1(input), Error::kInvalidInput);
    input.transport = nullptr;
    input.session = nullptr;
    Expect(npm::ValidateNpmInputEventV1(input), Error::kNone);
    layer.transport_protocol = 58;
    Expect(npm::ValidateNpmInputEventV1(input), Error::kNone);
    layer.transport_protocol = 17;
    Expect(npm::ValidateNpmInputEventV1(input), Error::kInvalidInput);
}

}  // namespace

int main() {
    TestEntitiesAndRows();
    TestPlans();
    TestDnsConfigAndPlan();
    TestDnsEntitySchemaAndRows();
    TestInputRelationships();
    std::puts("NPM protocol contract tests passed");
    return 0;
}
