// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_dns_contract.h"

#include <arrow/api.h>
#include <rapidjson/document.h>

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace flowsql::npm {
namespace {

NpmDnsConfigStatusV1 Fail(NpmDnsConfigErrorV1 error, std::string path) { return {error, std::move(path)}; }

const rapidjson::Value* Find(const rapidjson::Value& object, const char* name) {
    const auto member = object.FindMember(name);
    return member == object.MemberEnd() ? nullptr : &member->value;
}

}  // namespace

NpmDnsConfigStatusV1 ParseNpmDnsConfigV1(std::string_view json, NpmDnsConfigV1* output) {
    if (!output) return Fail(NpmDnsConfigErrorV1::kInvalidType, "/dns");
    if (json.empty()) return Fail(NpmDnsConfigErrorV1::kMissing, "/dns");
    try {
        rapidjson::Document document;
        document.Parse<rapidjson::kParseValidateEncodingFlag>(json.data(), json.size());
        if (document.HasParseError() || !document.IsObject()) {
            return Fail(NpmDnsConfigErrorV1::kInvalidType, "/dns");
        }
        for (auto member = document.MemberBegin(); member != document.MemberEnd(); ++member) {
            const std::string_view name(member->name.GetString(), member->name.GetStringLength());
            const std::string path = "/dns/" + std::string(name);
            for (auto previous = document.MemberBegin(); previous != member; ++previous) {
                if (name == std::string_view(previous->name.GetString(), previous->name.GetStringLength())) {
                    return Fail(NpmDnsConfigErrorV1::kDuplicateField, path);
                }
            }
            if (name != "primary_label_ids" && name != "response_timeout_ns" && name != "max_pending_per_session") {
                return Fail(NpmDnsConfigErrorV1::kUnknownField, path);
            }
        }

        NpmDnsConfigV1 next;
        const rapidjson::Value* labels = Find(document, "primary_label_ids");
        if (!labels) return Fail(NpmDnsConfigErrorV1::kMissing, "/dns/primary_label_ids");
        if (!labels->IsArray()) return Fail(NpmDnsConfigErrorV1::kInvalidType, "/dns/primary_label_ids");
        if (labels->Empty() || labels->Size() > kNpmDnsMaxPrimaryLabelsV1) {
            return Fail(NpmDnsConfigErrorV1::kInvalidRange, "/dns/primary_label_ids");
        }
        next.primary_label_ids.reserve(labels->Size());
        for (rapidjson::SizeType index = 0; index < labels->Size(); ++index) {
            const auto& label = (*labels)[index];
            const std::string path = "/dns/primary_label_ids/" + std::to_string(index);
            if (!label.IsUint64()) return Fail(NpmDnsConfigErrorV1::kInvalidType, path);
            if (label.GetUint64() == 0 || label.GetUint64() > std::numeric_limits<uint32_t>::max() ||
                std::find(next.primary_label_ids.begin(), next.primary_label_ids.end(), label.GetUint64()) !=
                    next.primary_label_ids.end()) {
                return Fail(NpmDnsConfigErrorV1::kInvalidRange, path);
            }
            next.primary_label_ids.push_back(static_cast<uint32_t>(label.GetUint64()));
        }
        if (const auto* timeout = Find(document, "response_timeout_ns")) {
            if (!timeout->IsInt64()) return Fail(NpmDnsConfigErrorV1::kInvalidType, "/dns/response_timeout_ns");
            next.response_timeout_ns = timeout->GetInt64();
            if (next.response_timeout_ns < kNpmDnsMinResponseTimeoutNsV1 ||
                next.response_timeout_ns > kNpmDnsMaxResponseTimeoutNsV1) {
                return Fail(NpmDnsConfigErrorV1::kInvalidRange, "/dns/response_timeout_ns");
            }
        }
        if (const auto* pending = Find(document, "max_pending_per_session")) {
            if (!pending->IsUint64()) return Fail(NpmDnsConfigErrorV1::kInvalidType, "/dns/max_pending_per_session");
            if (pending->GetUint64() == 0 || pending->GetUint64() > kNpmDnsMaxPendingPerSessionV1) {
                return Fail(NpmDnsConfigErrorV1::kInvalidRange, "/dns/max_pending_per_session");
            }
            next.max_pending_per_session = static_cast<uint32_t>(pending->GetUint64());
        }
        *output = std::move(next);
        return {};
    } catch (const std::bad_alloc&) {
        return Fail(NpmDnsConfigErrorV1::kAllocationFailed, "/dns");
    }
}

NpmEntityDescriptorV1 NpmDnsTransactionEntityDescriptorV1() {
    static const std::shared_ptr<arrow::Schema> schema = arrow::schema(
        {arrow::field("entity_instance_id", arrow::uint64(), false),
         arrow::field("revision", arrow::uint64(), false),
         arrow::field("observed_at", arrow::int64(), false),
         arrow::field("is_final", arrow::boolean(), false),
         arrow::field("session_id", arrow::uint64(), false),
         arrow::field("observation_domain_id", arrow::uint64(), false),
         arrow::field("transport_protocol", arrow::uint8(), false),
         arrow::field("a_ip", arrow::utf8(), false),
         arrow::field("b_ip", arrow::utf8(), false),
         arrow::field("a_port", arrow::uint16(), false),
         arrow::field("b_port", arrow::uint16(), false),
         arrow::field("dns_id", arrow::uint16(), false),
         arrow::field("outcome", arrow::utf8(), false),
         arrow::field("query_retries", arrow::uint32(), false),
         arrow::field("query_direction", arrow::uint8(), true),
         arrow::field("response_direction", arrow::uint8(), true),
         arrow::field("qname", arrow::utf8(), true),
         arrow::field("qtype", arrow::uint16(), true),
         arrow::field("qclass", arrow::uint16(), true),
         arrow::field("query_at_ns", arrow::int64(), true),
         arrow::field("response_at_ns", arrow::int64(), true),
         arrow::field("latency_ns", arrow::int64(), true),
         arrow::field("response_rcode", arrow::uint16(), true),
         arrow::field("response_tc", arrow::boolean(), true),
         arrow::field("incomplete_reason", arrow::utf8(), true)},
        arrow::key_value_metadata({"flowsql.entity", "flowsql.schema_version", "flowsql.timestamp_unit",
                                   "flowsql.revision_semantics", "flowsql.measurement_scope"},
                                  {"dns_transaction", "1", "ns", "event", "single_capture_observed_packets"}));
    NpmEntityDescriptorV1 entity;
    entity.entity_id = "dns_transaction";
    entity.module_id = "dns";
    entity.schema = schema;
    entity.revision_semantics = NpmRevisionSemanticsV1::kEvent;
    return entity;
}

NpmModulePlanV1 NpmDnsModulePlanV1(const NpmDnsConfigV1& config) {
    NpmModulePlanV1 plan;
    plan.module_id = "dns";
    plan.input_mask =
        static_cast<uint8_t>(NpmInputKindV1::kTcpPacket) | static_cast<uint8_t>(NpmInputKindV1::kUdpDatagram);
    plan.requires_labeling = true;
    plan.requires_tcp_stream = true;
    plan.primary_label_ids = config.primary_label_ids;
    plan.entities.push_back(NpmDnsTransactionEntityDescriptorV1());
    return plan;
}

}  // namespace flowsql::npm
