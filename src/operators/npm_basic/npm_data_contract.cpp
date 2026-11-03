// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <arrow/api.h>
#include <framework/core/packet_codec.h>
#include <framework/interfaces/cpp_operator_plugin_abi.h>
#include <common/channel_dataset.hpp>
#include "config/npm_basic_task_config.h"
#include "modules/http1/npm_http1_contract.h"
#include "modules/icmp/npm_icmp_contract.h"
#include "npm_data_contract_package.h"
#include "npm_family_result_contract.h"
#include "npm_result_dictionary.h"
#include "npm_result_schema.h"

namespace flowsql::npm {
namespace {
using namespace dataset;
Value Structure(const arrow::Schema& schema, Alloc& a) {
    Value result(rapidjson::kObjectType), fields(rapidjson::kArrayType), metadata(rapidjson::kObjectType);
    for (const auto& field : schema.fields()) {
        Value item(rapidjson::kObjectType);
        Set(item, "name", field->name(), a);
        Set(item, "type", Type("arrow", field->type()->ToString(), a), a);
        Set(item, "nullable", Value(field->nullable()), a);
        fields.PushBack(item, a);
    }
    if (schema.metadata())
        for (int i = 0; i < schema.metadata()->size(); ++i)
            Set(metadata, schema.metadata()->key(i).c_str(), schema.metadata()->value(i), a);
    Set(result, "fields", std::move(fields), a);
    Set(result, "metadata", std::move(metadata), a);
    return result;
}
void Number(Value& properties, const char* key, int64_t low, int64_t high, int64_t initial, Alloc& a) {
    Value schema(rapidjson::kObjectType);
    Set(schema, "type", "integer", a);
    Set(schema, "minimum", Value(low), a);
    Set(schema, "maximum", Value(high), a);
    Set(schema, "default", Value(initial), a);
    Set(properties, key, std::move(schema), a);
}
Value DomainDictionary(const Value& content, const NpmEntityDescriptorV1& entity, Alloc& a) {
    const auto& declarations = content["fields"];
    assert(declarations.MemberCount() == static_cast<unsigned>(entity.schema->num_fields()));
    Value dictionary(rapidjson::kObjectType), fields(rapidjson::kArrayType);
    Set(dictionary, "schema_id", "flowsql.data-dictionary.v1", a);
    Set(dictionary, "title", "NPM：" + entity.entity_id, a);
    Set(dictionary, "entity", Copy(content["entity"], a), a);
    for (const auto& physical : entity.schema->fields()) {
        const auto declaration = declarations.FindMember(physical->name().c_str());
        assert(declaration != declarations.MemberEnd());
        auto field = Copy(declaration->value, a);
        Set(field, "field_id", physical->name(), a);
        Set(field, "name", physical->name(), a);
        Set(field, "type", Type("arrow", physical->type()->ToString(), a), a);
        Set(field, "nullable", Value(physical->nullable()), a);
        fields.PushBack(field, a);
    }
    Set(dictionary, "fields", std::move(fields), a);
    return dictionary;
}
std::string Build() {
    Json doc;
    doc.Parse(kNpmDataContract);
    assert(!doc.HasParseError());
    auto& a = doc.GetAllocator();
    auto& resources = doc["resources"];
    Value domains(rapidjson::kObjectType);
    for (const char* module : {"basic", "session", "dns", "http1", "tls", "icmp"}) {
        const std::string key = std::string(module) + "_domain_content";
        Set(domains, module, Copy(resources[key.c_str()], a), a);
        resources.RemoveMember(key.c_str());
    }
    auto& core = resources["core_parameters"]["properties"];
#define CORE_NUMBER(key, Name) Number(core, key, kNpmMin##Name, kNpmMax##Name, kNpmDefault##Name, a)
    CORE_NUMBER("output_interval_ns", OutputIntervalNs);
    CORE_NUMBER("payload_sample_packets", PayloadSamplePackets);
    Number(core, "tcp_idle_timeout_ns", kNpmMinIdleTimeoutNs, kNpmMaxIdleTimeoutNs, kNpmDefaultTcpIdleTimeoutNs, a);
    Number(core, "udp_idle_timeout_ns", kNpmMinIdleTimeoutNs, kNpmMaxIdleTimeoutNs, kNpmDefaultUdpIdleTimeoutNs, a);
    CORE_NUMBER("out_of_order_tolerance_ns", OutOfOrderToleranceNs);
    CORE_NUMBER("max_active_sessions", ActiveSessions);
    Number(core, "max_tracked_bytes", kNpmMinTrackedBytes, kNpmMaxTrackedBytes, kNpmDefaultTrackedBytes, a);
    Number(core, "max_pending_output_bytes", kNpmMinPendingOutputBytes, kNpmMaxPendingOutputBytes,
           kNpmDefaultPendingOutputBytes, a);
#undef CORE_NUMBER
    auto& stream = core["tcp_stream"]["properties"];
    Number(stream, "max_buffered_bytes_per_direction", kNpmMinTcpStreamBufferedBytesPerDirection,
           kNpmMaxTcpStreamBufferedBytesPerDirection, kNpmDefaultTcpStreamBufferedBytesPerDirection, a);
    Number(stream, "gap_timeout_ns", kNpmMinTcpStreamGapTimeoutNs, kNpmMaxTcpStreamGapTimeoutNs,
           kNpmDefaultTcpStreamGapTimeoutNs, a);
    auto& modules = resources["module_parameters"];
    Number(modules["session"]["properties"], "max_tcp_ranges_per_direction", kNpmMinSessionTcpRangesPerDirection,
           kNpmMaxSessionTcpRangesPerDirection, kNpmDefaultSessionTcpRangesPerDirection, a);
    Number(modules["dns"]["properties"], "response_timeout_ns", kNpmDnsMinResponseTimeoutNsV1,
           kNpmDnsMaxResponseTimeoutNsV1, kNpmDnsDefaultResponseTimeoutNsV1, a);
    Number(modules["dns"]["properties"], "max_pending_per_session", 1, kNpmDnsMaxPendingPerSessionV1,
           kNpmDnsDefaultMaxPendingPerSessionV1, a);
    Number(modules["http1"]["properties"], "response_timeout_ns", kNpmHttp1MinResponseTimeoutNsV1,
           kNpmHttp1MaxResponseTimeoutNsV1, kNpmHttp1DefaultResponseTimeoutNsV1, a);
    Number(modules["http1"]["properties"], "max_pending_per_session", 1, kNpmHttp1MaxPendingPerSessionV1,
           kNpmHttp1DefaultMaxPendingPerSessionV1, a);
    Number(modules["http1"]["properties"], "max_header_bytes", kNpmHttp1MinHeaderBytesV1, kNpmHttp1MaxHeaderBytesV1,
           kNpmHttp1DefaultMaxHeaderBytesV1, a);
    Number(modules["tls"]["properties"], "handshake_timeout_ns", kNpmTlsMinHandshakeTimeoutNsV1,
           kNpmTlsMaxHandshakeTimeoutNsV1, kNpmTlsDefaultHandshakeTimeoutNsV1, a);
    Number(modules["tls"]["properties"], "max_hello_bytes", kNpmTlsMinHelloBytesV1, kNpmTlsMaxHelloBytesV1,
           kNpmTlsDefaultMaxHelloBytesV1, a);
    Number(modules["icmp"]["properties"], "echo_timeout_ns", kNpmIcmpMinEchoTimeoutNsV1, kNpmIcmpMaxEchoTimeoutNsV1,
           kNpmIcmpDefaultEchoTimeoutNsV1, a);
    Number(modules["icmp"]["properties"], "max_pending_echo", 1, kNpmIcmpDefaultMaxPendingEchoV1,
           kNpmIcmpDefaultMaxPendingEchoV1, a);
    auto& packets = resources["packet_inputs"]["batch_structures"];
    packets.PushBack(Structure(*packet::PacketSchema(), a), a);
    packets.PushBack(Structure(*packet::PacketFamilySchema(), a), a);
    Value templates(rapidjson::kObjectType);
    for (const auto kind : {NpmFamilyEntityKind::kBasic, NpmFamilyEntityKind::kSession, NpmFamilyEntityKind::kDns,
                            NpmFamilyEntityKind::kHttp1, NpmFamilyEntityKind::kTls, NpmFamilyEntityKind::kIcmp}) {
        for (const auto family : {packet::AddressFamily::kIPv4, packet::AddressFamily::kIPv6}) {
            const auto entity = NpmFamilyEntityDescriptor(kind, family);
            auto structure = Structure(*entity.schema, a);
            Set(structure, "identity_column", entity.identity_column, a);
            Set(structure, "revision_column", entity.revision_column, a);
            Set(structure, "is_final_column", entity.is_final_column, a);
            Set(structure, "observed_at_column", entity.observed_at_column, a);
            Set(structure, "dictionary", DomainDictionary(domains[entity.module_id.c_str()], entity, a), a);
            for (const char* view : {"history", "latest", "final", "data"}) {
                Value fields(rapidjson::kArrayType);
                for (const char* id : {"__npm_run_id", "__npm_task_id"})
                    fields.PushBack(Field(id, Type("arrow", "string", a), false, a), a);
                for (const auto& f : structure["fields"].GetArray()) fields.PushBack(Copy(f, a), a);
                if (std::string(view) != "data")
                    fields.PushBack(Field("__npm_run_status", Type("arrow", "string", a), false, a), a);
                auto dictionary = result_dictionary::Build(&structure["dictionary"], fields, view);
                Value entry(rapidjson::kObjectType);
                Set(entry, "schema_canonical", SchemaCanonical(*entity.schema), a);
                Set(entry, "dictionary", Copy(dictionary, a), a);
                const auto key = entity.entity_id + "_" + view + "_v" + std::to_string(entity.schema_version);
                Set(templates, key.c_str(), std::move(entry), a);
            }
            Set(resources, entity.entity_id.c_str(), std::move(structure), a);
            Value item(rapidjson::kObjectType), condition(rapidjson::kObjectType);
            Set(condition, "scope", "config", a);
            Set(condition, "pointer", "/observing", a);
            Set(condition, "equals", entity.entity_id, a);
            Set(item, "when", std::move(condition), a);
            Set(item, "mode", "fixed", a);
            Set(item, "structure_ref", "#/resources/" + entity.entity_id, a);
            doc["outputs"][0]["variants"].PushBack(item, a);
            Value managed(rapidjson::kObjectType);
            Set(managed, "module", entity.module_id, a);
            Set(managed, "entity", entity.entity_id, a);
            Set(managed, "schema_version", Value(entity.schema_version), a);
            Set(managed, "dictionary_state", "ready", a);
            Set(managed, "dictionary_pointer", "#/resources/" + entity.entity_id + "/dictionary", a);
            Set(managed, "structure_ref", "#/resources/" + entity.entity_id, a);
            resources["managed_results"]["entities"].PushBack(managed, a);
        }
    }
    for (const char* catalog : {"runs", "entities"}) {
        Value fields(rapidjson::kArrayType);
        const std::vector<std::pair<std::string, std::string>> columns =
            std::string(catalog) == "runs"
                ? std::vector<std::pair<
                      std::string,
                      std::string>>{{"run_id", "本次受管运行的精确身份。"},
                                    {"task_id", "调用任务身份；重复执行可产生不同 run_id。"},
                                    {"input_namespace", "生产配置冻结的输入命名空间。"},
                                    {"status", "受管运行生命周期状态，不表示协议业务成功。"},
                                    {"started_at_ns", "受管运行开始时刻，UTC Unix epoch 纳秒。"},
                                    {"completed_at_ns", "受管运行结束时刻，UTC Unix epoch 纳秒；未结束时为空。"},
                                    {"expires_at_ns", "保留期截止时刻，UTC Unix epoch 纳秒；为空表示未设置过期时刻。"},
                                    {"error_code", "受管执行错误码；没有记录错误时为空。"},
                                    {"error_stage", "发生执行错误的阶段；没有记录时为空。"},
                                    {"error_message", "执行错误详情；没有记录时为空。"},
                                    {"metadata_status",
                                     "受管状态落库的已知状态 known/unknown，不表示数据语义或业务质量通过验证。"},
                                    {"purge_cursor", "分批清理的内部进度游标；不用于分析统计。"}}
                : std::vector<std::pair<std::string, std::string>>{
                      {"entity_id", "NPM 分析实体身份，包含 IP 家族。"},
                      {"schema_version", "实体存储结构版本。"},
                      {"module_id", "产生该实体的分析模块。"},
                      {"revision_semantics", "实体的累计修订或事件语义。"},
                      {"identity_column", "实体实例身份列名。"},
                      {"revision_column", "实体修订列名。"},
                      {"observed_at_column", "实体观测时刻列名。"},
                      {"is_final_column", "实体终态标识列名。"},
                      {"schema_canonical", "生产 Arrow Schema 的规范表示，用于结构一致性核验。"},
                      {"schema_fingerprint", "生产 Schema 规范表示的存储指纹。"},
                      {"data_table", "实体内部物理存储表。"},
                      {"history_relation", "保留同一运行内全部修订的公开关系。"},
                      {"latest_relation", "每个运行内每实例最新修订的公开关系。"},
                      {"final_relation", "仅保留终态的公开关系；终态不表示协议成功。"}};
        for (const auto& [name, meaning] : columns) {
            const bool run_catalog = std::string(catalog) == "runs";
            const bool integer = name == "schema_version" || name == "started_at_ns" || name == "completed_at_ns" ||
                                 name == "expires_at_ns" || name == "error_code";
            const bool nullable =
                run_catalog && (name == "completed_at_ns" || name == "expires_at_ns" || name == "error_code" ||
                                name == "error_stage" || name == "error_message" || name == "purge_cursor");
            auto field = Field(name,
                               Type("arrow",
                                    name == "schema_version" ? "int32"
                                    : integer                ? "int64"
                                                             : "string",
                                    a),
                               nullable, a);
            Set(field, "definition", result_dictionary::Declared(meaning, a), a);
            fields.PushBack(field, a);
        }
        auto dictionary = result_dictionary::Build(nullptr, fields, "runs");
        if (std::string(catalog) == "entities") {
            Set(dictionary["entity"], "row_meaning",
                result_dictionary::Declared("一行表示一个 NPM 实体及其结构版本和受管关系目录。",
                                            dictionary.GetAllocator()),
                dictionary.GetAllocator());
            Value limits(rapidjson::kArrayType);
            limits.PushBack(
                Value("受管目录用于核验实体结构与关系身份；目录存在不代表分析行存在或业务数据质量通过验证。",
                      dictionary.GetAllocator()),
                dictionary.GetAllocator());
            Set(dictionary["entity"], "limitations",
                result_dictionary::Declared(std::move(limits), dictionary.GetAllocator()), dictionary.GetAllocator());
            Value ids(rapidjson::kArrayType);
            ids.PushBack("entity_id", a).PushBack("schema_version", a);
            Set(dictionary["entity"], "identity_fields",
                result_dictionary::Declared(std::move(ids), dictionary.GetAllocator()), dictionary.GetAllocator());
        }
        // Catalog fields retain declarations that are not entity analysis fields.
        for (rapidjson::SizeType i = 0; i < fields.Size(); ++i)
            Set(dictionary["fields"][i], "definition", Copy(fields[i]["definition"], dictionary.GetAllocator()),
                dictionary.GetAllocator());
        Value entry(rapidjson::kObjectType);
        Set(entry, "schema_canonical", "", a);
        Set(entry, "dictionary", Copy(dictionary, a), a);
        Set(templates, catalog, std::move(entry), a);
    }
    Set(resources, "managed_dictionaries", std::move(templates), a);
    return Encode(doc);
}
}  // namespace
}  // namespace flowsql::npm

extern "C" const char* flowsql_operator_data_contract_v1(int index) {
    if (index != 0 && index != 1) return nullptr;
    static const std::string contract = flowsql::npm::Build();
    return contract.c_str();
}
