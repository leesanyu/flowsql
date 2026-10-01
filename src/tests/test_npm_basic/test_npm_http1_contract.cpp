// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <operators/npm_basic/modules/http1/npm_http1_contract.h>

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

static_assert(std::is_same_v<decltype(std::declval<npm::NpmHttp1DirectionStateV1>().header_bytes), std::string>);
static_assert(std::is_same_v<decltype(std::declval<npm::NpmHttp1TransactionV1>().fifo_position), uint64_t>);

void TestOwnedInternalState() {
    npm::NpmHttp1DirectionStateV1 direction;
    assert(direction.origin == npm::NpmTcpStreamOriginV1::kUnknown);
    assert(direction.header_bytes.empty() && direction.header_bytes_seen == 0);
    assert(direction.chunk_line_value == 0 && direction.chunk_line_bytes == 0 && direction.trailer_bytes_seen == 0);
    assert(!direction.gap_seen && !direction.ended && !direction.alignment_lost);
    std::string input = "GET / HTTP/1.1\r\n";
    direction.header_bytes = input;
    input.clear();
    assert(direction.header_bytes == "GET / HTTP/1.1\r\n");
    npm::NpmHttp1TransactionV1 transaction;
    assert(transaction.fifo_position == 0 && transaction.informational_count == 0);
}

template <typename Builder, typename Value>
std::shared_ptr<arrow::Array> Values(std::initializer_list<std::optional<Value>> values) {
    Builder builder;
    for (const auto& value : values) assert((value ? builder.Append(*value) : builder.AppendNull()).ok());
    std::shared_ptr<arrow::Array> result;
    assert(builder.Finish(&result).ok());
    return result;
}

void TestConfigAndPlan() {
    using Error = npm::NpmHttp1ConfigErrorV1;
    npm::NpmHttp1ConfigV1 config;
    config.primary_label_ids = {77};
    const auto reject = [&](std::string_view json, Error error, std::string_view path) {
        const auto status = npm::ParseNpmHttp1ConfigV1(json, &config);
        assert(status.error == error && status.path == path);
        assert(config.primary_label_ids == std::vector<uint32_t>{77});
        assert(config.response_timeout_ns == npm::kNpmHttp1DefaultResponseTimeoutNsV1);
    };
    reject("", Error::kMissing, "/http1");
    reject("null", Error::kInvalidType, "/http1");
    reject("{}", Error::kMissing, "/http1/primary_label_ids");
    reject(R"({"primary_label_ids":null})", Error::kInvalidType, "/http1/primary_label_ids");
    reject(R"({"primary_label_ids":[]})", Error::kInvalidRange, "/http1/primary_label_ids");
    reject(R"({"primary_label_ids":[0]})", Error::kInvalidRange, "/http1/primary_label_ids/0");
    reject(R"({"primary_label_ids":[1,1]})", Error::kInvalidRange, "/http1/primary_label_ids/1");
    reject(R"({"primary_label_ids":[4294967296]})", Error::kInvalidRange, "/http1/primary_label_ids/0");
    reject(R"({"primary_label_ids":["1"]})", Error::kInvalidType, "/http1/primary_label_ids/0");
    reject(R"({"primary_label_ids":[1],"extra":0})", Error::kUnknownField, "/http1/extra");
    reject(R"({"primary_label_ids":[1],"primary_label_ids":[2]})", Error::kDuplicateField, "/http1/primary_label_ids");
    reject(R"({"primary_label_ids":[1],"response_timeout_ns":null})", Error::kInvalidType,
           "/http1/response_timeout_ns");
    reject(R"({"primary_label_ids":[1],"response_timeout_ns":999999})", Error::kInvalidRange,
           "/http1/response_timeout_ns");
    reject(R"({"primary_label_ids":[1],"response_timeout_ns":300000000001})", Error::kInvalidRange,
           "/http1/response_timeout_ns");
    reject(R"({"primary_label_ids":[1],"max_pending_per_session":0})", Error::kInvalidRange,
           "/http1/max_pending_per_session");
    reject(R"({"primary_label_ids":[1],"max_pending_per_session":4097})", Error::kInvalidRange,
           "/http1/max_pending_per_session");
    reject(R"({"primary_label_ids":[1],"max_header_bytes":1023})", Error::kInvalidRange, "/http1/max_header_bytes");
    reject(R"({"primary_label_ids":[1],"max_header_bytes":1048577})", Error::kInvalidRange, "/http1/max_header_bytes");
    reject(R"({"primary_label_ids":[1],"max_header_bytes":"1024"})", Error::kInvalidType, "/http1/max_header_bytes");
    std::string labels_json = R"({"primary_label_ids":[)";
    for (uint32_t id = 1; id <= 257; ++id) {
        if (id != 1) labels_json += ',';
        labels_json += std::to_string(id);
    }
    labels_json += "]}";
    reject(labels_json, Error::kInvalidRange, "/http1/primary_label_ids");
    labels_json.erase(labels_json.rfind(",257"), 4);
    npm::NpmHttp1ConfigV1 boundary;
    assert(npm::ParseNpmHttp1ConfigV1(labels_json, &boundary).error == Error::kNone);
    assert(boundary.primary_label_ids.size() == 256);
    std::string json = R"({"primary_label_ids":[1001],"response_timeout_ns":1000000,)"
                       R"("max_pending_per_session":4096,"max_header_bytes":1048576})";
    assert(npm::ParseNpmHttp1ConfigV1(json, &config).error == Error::kNone);
    json.assign(json.size(), 'x');
    assert(config.primary_label_ids == std::vector<uint32_t>{1001});
    assert(config.response_timeout_ns == 1'000'000 && config.max_pending_per_session == 4096);
    assert(config.max_header_bytes == 1048576);
    assert(npm::ParseNpmHttp1ConfigV1(R"({"primary_label_ids":[1001]})", &boundary).error == Error::kNone);
    assert(boundary.response_timeout_ns == 5'000'000'000 && boundary.max_pending_per_session == 128);
    assert(boundary.max_header_bytes == 65536);

    const auto plan = npm::NpmHttp1ModulePlanV1(config);
    assert(plan.module_id == "http1" && plan.input_mask == 1);
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

void TestSchemaAndRows() {
    const auto entity = npm::NpmHttp1TransactionEntityDescriptorV1();
    using Error = npm::NpmProtocolContractErrorV1;
    assert(npm::ValidateNpmEntityDescriptorV1(entity).error == Error::kNone);
    assert(entity.entity_id == "http1_transaction" && entity.module_id == "http1");
    assert(entity.schema_version == 1 && entity.revision_semantics == npm::NpmRevisionSemanticsV1::kEvent);
    for (const auto& [key, value] : std::vector<std::pair<const char*, const char*>>{
             {"flowsql.entity", "http1_transaction"},
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
        {"informational_count", arrow::Type::UINT32, false},
        {"request_direction", arrow::Type::UINT8, true},
        {"response_direction", arrow::Type::UINT8, true},
        {"method", arrow::Type::STRING, true},
        {"target", arrow::Type::STRING, true},
        {"host", arrow::Type::STRING, true},
        {"status_code", arrow::Type::UINT16, true},
        {"request_headers_at_ns", arrow::Type::INT64, true},
        {"response_headers_at_ns", arrow::Type::INT64, true},
        {"latency_ns", arrow::Type::INT64, true},
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
        } else if (type == arrow::Type::UINT16) {
            columns.push_back(Values<arrow::UInt16Builder, uint16_t>({80}));
        } else if (type == arrow::Type::UINT32) {
            columns.push_back(Values<arrow::UInt32Builder, uint32_t>({0}));
        } else {
            columns.push_back(Values<arrow::StringBuilder, std::string>({std::string("x")}));
        }
    }
    auto rows = arrow::RecordBatch::Make(entity.schema, 1, columns);
    assert(npm::ValidateNpmEntityRowsV1("http1", entity, *rows).error == Error::kNone);
    columns[0] = Values<arrow::UInt64Builder, uint64_t>({0});
    assert(npm::ValidateNpmEntityRowsV1("http1", entity, *arrow::RecordBatch::Make(entity.schema, 1, columns)).error ==
           Error::kInvalidRows);
    columns[0] = Values<arrow::UInt64Builder, uint64_t>({1});
    columns[1] = Values<arrow::UInt64Builder, uint64_t>({2});
    assert(npm::ValidateNpmEntityRowsV1("http1", entity, *arrow::RecordBatch::Make(entity.schema, 1, columns)).error ==
           Error::kInvalidRows);
    columns[1] = Values<arrow::UInt64Builder, uint64_t>({1});
    columns[3] = Values<arrow::BooleanBuilder, bool>({false});
    assert(npm::ValidateNpmEntityRowsV1("http1", entity, *arrow::RecordBatch::Make(entity.schema, 1, columns)).error ==
           Error::kInvalidRows);
}

}  // namespace

int main() {
    TestOwnedInternalState();
    TestConfigAndPlan();
    TestSchemaAndRows();
}
