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
std::string Build() {
    Json doc;
    doc.Parse(kNpmDataContract);
    assert(!doc.HasParseError());
    auto& a = doc.GetAllocator();
    auto& resources = doc["resources"];
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
    for (const auto kind : {NpmFamilyEntityKind::kBasic, NpmFamilyEntityKind::kSession, NpmFamilyEntityKind::kDns,
                            NpmFamilyEntityKind::kHttp1, NpmFamilyEntityKind::kTls, NpmFamilyEntityKind::kIcmp}) {
        for (const auto family : {packet::AddressFamily::kIPv4, packet::AddressFamily::kIPv6}) {
            const auto entity = NpmFamilyEntityDescriptor(kind, family);
            auto structure = Structure(*entity.schema, a);
            Set(structure, "identity_column", entity.identity_column, a);
            Set(structure, "revision_column", entity.revision_column, a);
            Set(structure, "is_final_column", entity.is_final_column, a);
            Set(structure, "observed_at_column", entity.observed_at_column, a);
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
            Set(managed, "structure_ref", "#/resources/" + entity.entity_id, a);
            resources["managed_results"]["entities"].PushBack(managed, a);
        }
    }
    return Encode(doc);
}
}  // namespace
}  // namespace flowsql::npm

extern "C" const char* flowsql_operator_data_contract_v1(int index) {
    if (index != 0 && index != 1) return nullptr;
    static const std::string contract = flowsql::npm::Build();
    return contract.c_str();
}
