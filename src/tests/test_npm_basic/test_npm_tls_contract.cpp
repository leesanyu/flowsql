// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <operators/npm_basic/modules/tls/npm_tls_contract.h>

#include <arrow/api.h>

#include <cassert>
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace npm = flowsql::npm;

namespace {

static_assert(std::is_same_v<decltype(std::declval<npm::NpmTlsDirectionStateV1>().hello_bytes), std::string>);
static_assert(std::is_same_v<decltype(std::declval<npm::NpmTlsHandshakeV1>().first_client_hello),
                             std::optional<npm::NpmTlsHelloFactsV1>>);

void TestOwnedInternalState() {
    npm::NpmTlsDirectionStateV1 direction;
    assert(direction.origin == npm::NpmTcpStreamOriginV1::kUnknown);
    assert(direction.record_header_bytes == 0 && direction.handshake_header_bytes == 0);
    assert(direction.record_remaining == 0 && direction.handshake_remaining == 0);
    assert(!direction.first_record_checked && !direction.gap_seen && !direction.ended && !direction.encrypted_boundary);
    std::string borrowed = "hello";
    direction.hello_bytes = borrowed;
    npm::NpmTlsHelloFactsV1 hello;
    hello.sni = borrowed;
    hello.offered_alpn.push_back(borrowed);
    borrowed.clear();
    assert(direction.hello_bytes == "hello" && hello.sni == "hello" && hello.offered_alpn[0] == "hello");
    npm::NpmTlsHandshakeV1 candidate;
    candidate.first_client_hello = hello;
    assert(candidate.phase == npm::NpmTlsHandshakePhaseV1::kCandidate && candidate.hello_retry_count == 0);
    assert(candidate.first_client_hello->sni == "hello" && !candidate.retry_client_hello);
}

void TestConfigAndPlan() {
    using Error = npm::NpmTlsConfigErrorV1;
    npm::NpmTlsConfigV1 config;
    config.primary_label_ids = {77};
    const auto reject = [&](std::string_view json, Error error, std::string_view path) {
        const auto status = npm::ParseNpmTlsConfigV1(json, &config);
        assert(status.error == error && status.path == path);
        assert(config.primary_label_ids == std::vector<uint32_t>{77});
        assert(config.handshake_timeout_ns == npm::kNpmTlsDefaultHandshakeTimeoutNsV1);
    };
    reject("", Error::kMissing, "/tls");
    reject("null", Error::kInvalidType, "/tls");
    reject("{}", Error::kMissing, "/tls/primary_label_ids");
    reject(R"({"primary_label_ids":null})", Error::kInvalidType, "/tls/primary_label_ids");
    reject(R"({"primary_label_ids":[]})", Error::kInvalidRange, "/tls/primary_label_ids");
    reject(R"({"primary_label_ids":[0]})", Error::kInvalidRange, "/tls/primary_label_ids/0");
    reject(R"({"primary_label_ids":[1,1]})", Error::kInvalidRange, "/tls/primary_label_ids/1");
    reject(R"({"primary_label_ids":[4294967296]})", Error::kInvalidRange, "/tls/primary_label_ids/0");
    reject(R"({"primary_label_ids":["1"]})", Error::kInvalidType, "/tls/primary_label_ids/0");
    reject(R"({"primary_label_ids":[1],"extra":0})", Error::kUnknownField, "/tls/extra");
    reject(R"({"primary_label_ids":[1],"primary_label_ids":[2]})", Error::kDuplicateField, "/tls/primary_label_ids");
    reject(R"({"primary_label_ids":[1],"handshake_timeout_ns":null})", Error::kInvalidType,
           "/tls/handshake_timeout_ns");
    reject(R"({"primary_label_ids":[1],"handshake_timeout_ns":999999})", Error::kInvalidRange,
           "/tls/handshake_timeout_ns");
    reject(R"({"primary_label_ids":[1],"handshake_timeout_ns":300000000001})", Error::kInvalidRange,
           "/tls/handshake_timeout_ns");
    reject(R"({"primary_label_ids":[1],"max_hello_bytes":4095})", Error::kInvalidRange, "/tls/max_hello_bytes");
    reject(R"({"primary_label_ids":[1],"max_hello_bytes":1048577})", Error::kInvalidRange, "/tls/max_hello_bytes");
    reject(R"({"primary_label_ids":[1],"max_hello_bytes":"4096"})", Error::kInvalidType, "/tls/max_hello_bytes");
    reject(R"({"primary_label_ids":[1],"max_hello_bytes":4096,"max_hello_bytes":8192})", Error::kDuplicateField,
           "/tls/max_hello_bytes");
    assert(npm::ParseNpmTlsConfigV1("{}", nullptr).error == Error::kInvalidType);

    std::string labels_json = R"({"primary_label_ids":[)";
    for (uint32_t id = 1; id <= 257; ++id) {
        if (id != 1) labels_json += ',';
        labels_json += std::to_string(id);
    }
    labels_json += "]}";
    reject(labels_json, Error::kInvalidRange, "/tls/primary_label_ids");
    labels_json.erase(labels_json.rfind(",257"), 4);
    npm::NpmTlsConfigV1 boundary;
    assert(npm::ParseNpmTlsConfigV1(labels_json, &boundary).error == Error::kNone);
    assert(boundary.primary_label_ids.size() == 256);

    std::string json = R"({"primary_label_ids":[1001],"handshake_timeout_ns":1000000,"max_hello_bytes":1048576})";
    assert(npm::ParseNpmTlsConfigV1(json, &config).error == Error::kNone);
    json.assign(json.size(), 'x');
    assert(config.primary_label_ids == std::vector<uint32_t>{1001});
    assert(config.handshake_timeout_ns == 1'000'000 && config.max_hello_bytes == 1'048'576);
    assert(npm::ParseNpmTlsConfigV1(R"({"primary_label_ids":[1001]})", &boundary).error == Error::kNone);
    assert(boundary.handshake_timeout_ns == 5'000'000'000 && boundary.max_hello_bytes == 65'536);
    assert(npm::ParseNpmTlsConfigV1(
               R"({"primary_label_ids":[1001],"handshake_timeout_ns":300000000000,"max_hello_bytes":4096})", &boundary)
               .error == Error::kNone);

    const auto plan = npm::NpmTlsModulePlanV1(config);
    assert(plan.module_id == "tls" && plan.input_mask == static_cast<uint8_t>(npm::NpmInputKindV1::kTcpPacket));
    assert(plan.requires_labeling && plan.requires_tcp_stream && plan.primary_label_ids == config.primary_label_ids);
    npm::NpmModuleCapabilitiesV1 capabilities;
    capabilities.available_label_ids = {1001};
    using ContractError = npm::NpmProtocolContractErrorV1;
    assert(npm::ValidateNpmModulePlanV1(plan, capabilities).error == ContractError::kUnavailableCapability);
    capabilities.labeling_enabled = true;
    assert(npm::ValidateNpmModulePlanV1(plan, capabilities).error == ContractError::kUnavailableCapability);
    capabilities.labeling_available = true;
    assert(npm::ValidateNpmModulePlanV1(plan, capabilities).error == ContractError::kUnavailableCapability);
    capabilities.tcp_stream_available = true;
    assert(npm::ValidateNpmModulePlanV1(plan, capabilities).error == ContractError::kNone);
    capabilities.available_label_ids.clear();
    assert(npm::ValidateNpmModulePlanV1(plan, capabilities).error == ContractError::kInvalidLabelSelection);
    capabilities.available_label_ids = {1001};
    for (const auto& labels : std::vector<std::vector<uint32_t>>{{}, {0}, {1001, 1001}, {999}}) {
        auto invalid = plan;
        invalid.primary_label_ids = labels;
        assert(npm::ValidateNpmModulePlanV1(invalid, capabilities).error == ContractError::kInvalidLabelSelection);
    }
}

template <typename Builder, typename Value>
std::shared_ptr<arrow::Array> Values(std::initializer_list<std::optional<Value>> values) {
    Builder builder;
    for (const auto& value : values) assert((value ? builder.Append(*value) : builder.AppendNull()).ok());
    std::shared_ptr<arrow::Array> result;
    assert(builder.Finish(&result).ok());
    return result;
}

void TestSchemaAndRows() {
    const auto entity = npm::NpmTlsHandshakeEntityDescriptorV1();
    using Error = npm::NpmProtocolContractErrorV1;
    assert(npm::ValidateNpmEntityDescriptorV1(entity).error == Error::kNone);
    assert(entity.entity_id == "tls_handshake" && entity.module_id == "tls");
    assert(entity.schema_version == 1 && entity.revision_semantics == npm::NpmRevisionSemanticsV1::kEvent);
    for (const auto& [key, value] : std::vector<std::pair<const char*, const char*>>{
             {"flowsql.entity", "tls_handshake"},
             {"flowsql.schema_version", "1"},
             {"flowsql.timestamp_unit", "ns"},
             {"flowsql.revision_semantics", "event"},
             {"flowsql.measurement_scope", "single_capture_observed_packets"}}) {
        assert(entity.schema->metadata()->Get(key).ValueOrDie() == value);
    }
    const std::vector<std::tuple<const char*, arrow::Type::type, bool>> fields = {
        {"entity_instance_id", arrow::Type::UINT64, false},
        {"revision", arrow::Type::UINT64, false},
        {"observed_at", arrow::Type::INT64, false},
        {"is_final", arrow::Type::BOOL, false},
        {"session_id", arrow::Type::UINT64, false},
        {"observation_domain_id", arrow::Type::UINT64, false},
        {"a_ip", arrow::Type::STRING, false},
        {"b_ip", arrow::Type::STRING, false},
        {"a_port", arrow::Type::UINT16, false},
        {"b_port", arrow::Type::UINT16, false},
        {"outcome", arrow::Type::STRING, false},
        {"hello_retry_count", arrow::Type::UINT8, false},
        {"incomplete_reason", arrow::Type::STRING, true},
        {"client_direction", arrow::Type::UINT8, false},
        {"client_hello_at_ns", arrow::Type::INT64, true},
        {"server_hello_at_ns", arrow::Type::INT64, true},
        {"server_hello_latency_ns", arrow::Type::INT64, true},
        {"client_sni", arrow::Type::STRING, true},
        {"client_alpn_protocols", arrow::Type::STRING, true},
        {"client_offered_tls13", arrow::Type::BOOL, true},
        {"selected_version", arrow::Type::UINT16, true},
        {"cipher_suite", arrow::Type::UINT16, true},
        {"selected_alpn", arrow::Type::STRING, true},
        {"alert_level", arrow::Type::UINT8, true},
        {"alert_description", arrow::Type::UINT8, true}};
    assert(entity.schema->num_fields() == 25 && fields.size() == 25);
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
        } else if (type == arrow::Type::UINT16) {
            columns.push_back(Values<arrow::UInt16Builder, uint16_t>({80}));
        } else if (type == arrow::Type::UINT8) {
            columns.push_back(Values<arrow::UInt8Builder, uint8_t>({0}));
        } else {
            columns.push_back(Values<arrow::StringBuilder, std::string>({std::string("incomplete")}));
        }
    }
    auto rows = arrow::RecordBatch::Make(entity.schema, 1, columns);
    assert(npm::ValidateNpmEntityRowsV1("tls", entity, *rows).error == Error::kNone);
    columns[0] = Values<arrow::UInt64Builder, uint64_t>({0});
    assert(npm::ValidateNpmEntityRowsV1("tls", entity, *arrow::RecordBatch::Make(entity.schema, 1, columns)).error ==
           Error::kInvalidRows);
    columns[0] = Values<arrow::UInt64Builder, uint64_t>({1});
    columns[1] = Values<arrow::UInt64Builder, uint64_t>({2});
    assert(npm::ValidateNpmEntityRowsV1("tls", entity, *arrow::RecordBatch::Make(entity.schema, 1, columns)).error ==
           Error::kInvalidRows);
    columns[1] = Values<arrow::UInt64Builder, uint64_t>({1});
    columns[3] = Values<arrow::BooleanBuilder, bool>({false});
    assert(npm::ValidateNpmEntityRowsV1("tls", entity, *arrow::RecordBatch::Make(entity.schema, 1, columns)).error ==
           Error::kInvalidRows);
}

}  // namespace

int main() {
    TestOwnedInternalState();
    TestConfigAndPlan();
    TestSchemaAndRows();
}
