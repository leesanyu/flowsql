// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_tls_contract.h"

#include <arrow/api.h>
#include <rapidjson/document.h>

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace flowsql::npm {
namespace {

NpmTlsConfigStatusV1 Fail(NpmTlsConfigErrorV1 error, std::string path) { return {error, std::move(path)}; }

const rapidjson::Value* Find(const rapidjson::Value& object, const char* name) {
    const auto member = object.FindMember(name);
    return member == object.MemberEnd() ? nullptr : &member->value;
}

}  // namespace

NpmTlsConfigStatusV1 ParseNpmTlsConfigV1(std::string_view json, NpmTlsConfigV1* output) {
    if (output == nullptr) return Fail(NpmTlsConfigErrorV1::kInvalidType, "/tls");
    if (json.empty()) return Fail(NpmTlsConfigErrorV1::kMissing, "/tls");
    try {
        rapidjson::Document document;
        document.Parse<rapidjson::kParseValidateEncodingFlag>(json.data(), json.size());
        if (document.HasParseError() || !document.IsObject()) {
            return Fail(NpmTlsConfigErrorV1::kInvalidType, "/tls");
        }
        for (auto member = document.MemberBegin(); member != document.MemberEnd(); ++member) {
            const std::string_view name(member->name.GetString(), member->name.GetStringLength());
            const std::string path = "/tls/" + std::string(name);
            for (auto previous = document.MemberBegin(); previous != member; ++previous) {
                if (name == std::string_view(previous->name.GetString(), previous->name.GetStringLength())) {
                    return Fail(NpmTlsConfigErrorV1::kDuplicateField, path);
                }
            }
            if (name != "primary_label_ids" && name != "handshake_timeout_ns" && name != "max_hello_bytes") {
                return Fail(NpmTlsConfigErrorV1::kUnknownField, path);
            }
        }

        NpmTlsConfigV1 next;
        const auto* labels = Find(document, "primary_label_ids");
        if (labels == nullptr) return Fail(NpmTlsConfigErrorV1::kMissing, "/tls/primary_label_ids");
        if (!labels->IsArray()) return Fail(NpmTlsConfigErrorV1::kInvalidType, "/tls/primary_label_ids");
        if (labels->Empty() || labels->Size() > kNpmTlsMaxPrimaryLabelsV1) {
            return Fail(NpmTlsConfigErrorV1::kInvalidRange, "/tls/primary_label_ids");
        }
        next.primary_label_ids.reserve(labels->Size());
        for (rapidjson::SizeType index = 0; index < labels->Size(); ++index) {
            const auto& label = (*labels)[index];
            const std::string path = "/tls/primary_label_ids/" + std::to_string(index);
            if (!label.IsUint64()) return Fail(NpmTlsConfigErrorV1::kInvalidType, path);
            if (label.GetUint64() == 0 || label.GetUint64() > std::numeric_limits<uint32_t>::max() ||
                std::find(next.primary_label_ids.begin(), next.primary_label_ids.end(), label.GetUint64()) !=
                    next.primary_label_ids.end()) {
                return Fail(NpmTlsConfigErrorV1::kInvalidRange, path);
            }
            next.primary_label_ids.push_back(static_cast<uint32_t>(label.GetUint64()));
        }
        if (const auto* timeout = Find(document, "handshake_timeout_ns")) {
            if (!timeout->IsInt64()) return Fail(NpmTlsConfigErrorV1::kInvalidType, "/tls/handshake_timeout_ns");
            next.handshake_timeout_ns = timeout->GetInt64();
            if (next.handshake_timeout_ns < kNpmTlsMinHandshakeTimeoutNsV1 ||
                next.handshake_timeout_ns > kNpmTlsMaxHandshakeTimeoutNsV1) {
                return Fail(NpmTlsConfigErrorV1::kInvalidRange, "/tls/handshake_timeout_ns");
            }
        }
        if (const auto* maximum = Find(document, "max_hello_bytes")) {
            if (!maximum->IsUint64()) return Fail(NpmTlsConfigErrorV1::kInvalidType, "/tls/max_hello_bytes");
            if (maximum->GetUint64() < kNpmTlsMinHelloBytesV1 || maximum->GetUint64() > kNpmTlsMaxHelloBytesV1) {
                return Fail(NpmTlsConfigErrorV1::kInvalidRange, "/tls/max_hello_bytes");
            }
            next.max_hello_bytes = static_cast<uint32_t>(maximum->GetUint64());
        }
        *output = std::move(next);
        return {};
    } catch (const std::bad_alloc&) {
        return Fail(NpmTlsConfigErrorV1::kAllocationFailed, "/tls");
    }
}

NpmEntityDescriptorV1 NpmTlsHandshakeEntityDescriptorV1() {
    static const std::shared_ptr<arrow::Schema> schema = arrow::schema(
        {arrow::field("entity_instance_id", arrow::uint64(), false),
         arrow::field("revision", arrow::uint64(), false),
         arrow::field("observed_at", arrow::int64(), false),
         arrow::field("is_final", arrow::boolean(), false),
         arrow::field("session_id", arrow::uint64(), false),
         arrow::field("observation_domain_id", arrow::uint64(), false),
         arrow::field("a_ip", arrow::utf8(), false),
         arrow::field("b_ip", arrow::utf8(), false),
         arrow::field("a_port", arrow::uint16(), false),
         arrow::field("b_port", arrow::uint16(), false),
         arrow::field("outcome", arrow::utf8(), false),
         arrow::field("hello_retry_count", arrow::uint8(), false),
         arrow::field("incomplete_reason", arrow::utf8(), true),
         arrow::field("client_direction", arrow::uint8(), false),
         arrow::field("client_hello_at_ns", arrow::int64(), true),
         arrow::field("server_hello_at_ns", arrow::int64(), true),
         arrow::field("server_hello_latency_ns", arrow::int64(), true),
         arrow::field("client_sni", arrow::utf8(), true),
         arrow::field("client_alpn_protocols", arrow::utf8(), true),
         arrow::field("client_offered_tls13", arrow::boolean(), true),
         arrow::field("selected_version", arrow::uint16(), true),
         arrow::field("cipher_suite", arrow::uint16(), true),
         arrow::field("selected_alpn", arrow::utf8(), true),
         arrow::field("alert_level", arrow::uint8(), true),
         arrow::field("alert_description", arrow::uint8(), true)},
        arrow::key_value_metadata({"flowsql.entity", "flowsql.schema_version", "flowsql.timestamp_unit",
                                   "flowsql.revision_semantics", "flowsql.measurement_scope"},
                                  {"tls_handshake", "1", "ns", "event", "single_capture_observed_packets"}));
    NpmEntityDescriptorV1 entity;
    entity.entity_id = "tls_handshake";
    entity.module_id = "tls";
    entity.schema = schema;
    entity.revision_semantics = NpmRevisionSemanticsV1::kEvent;
    return entity;
}

NpmModulePlanV1 NpmTlsModulePlanV1(const NpmTlsConfigV1& config) {
    NpmModulePlanV1 plan;
    plan.module_id = "tls";
    plan.input_mask = static_cast<uint8_t>(NpmInputKindV1::kTcpPacket);
    plan.requires_labeling = true;
    plan.requires_tcp_stream = true;
    plan.primary_label_ids = config.primary_label_ids;
    plan.entities.push_back(NpmTlsHandshakeEntityDescriptorV1());
    return plan;
}

}  // namespace flowsql::npm
