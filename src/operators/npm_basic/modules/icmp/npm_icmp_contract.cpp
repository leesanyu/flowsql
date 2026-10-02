// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_icmp_contract.h"

#include <arrow/api.h>
#include <rapidjson/document.h>

#include <new>
#include <utility>

namespace flowsql::npm {
namespace {

NpmIcmpConfigStatusV1 Fail(NpmIcmpConfigErrorV1 error, std::string path) { return {error, std::move(path)}; }

const rapidjson::Value* Find(const rapidjson::Value& object, const char* name) {
    const auto member = object.FindMember(name);
    return member == object.MemberEnd() ? nullptr : &member->value;
}

}  // namespace

NpmIcmpConfigStatusV1 ParseNpmIcmpConfigV1(std::string_view json, NpmIcmpConfigV1* output) {
    if (!output) return Fail(NpmIcmpConfigErrorV1::kInvalidType, "/icmp");
    try {
        rapidjson::Document document;
        document.Parse<rapidjson::kParseValidateEncodingFlag>(json.data(), json.size());
        if (document.HasParseError() || !document.IsObject()) {
            return Fail(NpmIcmpConfigErrorV1::kInvalidType, "/icmp");
        }
        for (auto member = document.MemberBegin(); member != document.MemberEnd(); ++member) {
            const std::string_view name(member->name.GetString(), member->name.GetStringLength());
            const std::string path = "/icmp/" + std::string(name);
            for (auto previous = document.MemberBegin(); previous != member; ++previous) {
                if (name == std::string_view(previous->name.GetString(), previous->name.GetStringLength())) {
                    return Fail(NpmIcmpConfigErrorV1::kDuplicateField, path);
                }
            }
            if (name != "echo_timeout_ns" && name != "max_pending_echo") {
                return Fail(NpmIcmpConfigErrorV1::kUnknownField, path);
            }
        }
        NpmIcmpConfigV1 next;
        if (const auto* timeout = Find(document, "echo_timeout_ns")) {
            if (!timeout->IsInt64()) return Fail(NpmIcmpConfigErrorV1::kInvalidType, "/icmp/echo_timeout_ns");
            next.echo_timeout_ns = timeout->GetInt64();
            if (next.echo_timeout_ns < kNpmIcmpMinEchoTimeoutNsV1 ||
                next.echo_timeout_ns > kNpmIcmpMaxEchoTimeoutNsV1) {
                return Fail(NpmIcmpConfigErrorV1::kInvalidRange, "/icmp/echo_timeout_ns");
            }
        }
        if (const auto* pending = Find(document, "max_pending_echo")) {
            if (!pending->IsUint64()) return Fail(NpmIcmpConfigErrorV1::kInvalidType, "/icmp/max_pending_echo");
            if (pending->GetUint64() == 0 || pending->GetUint64() > kNpmIcmpDefaultMaxPendingEchoV1) {
                return Fail(NpmIcmpConfigErrorV1::kInvalidRange, "/icmp/max_pending_echo");
            }
            next.max_pending_echo = static_cast<uint32_t>(pending->GetUint64());
        }
        *output = next;
        return {};
    } catch (const std::bad_alloc&) {
        return Fail(NpmIcmpConfigErrorV1::kAllocationFailed, "/icmp");
    }
}

NpmEntityDescriptorV1 NpmIcmpEventEntityDescriptorV1() {
    static const std::shared_ptr<arrow::Schema> schema =
        arrow::schema({arrow::field("entity_instance_id", arrow::uint64(), false),
                       arrow::field("revision", arrow::uint64(), false),
                       arrow::field("observed_at", arrow::int64(), false),
                       arrow::field("is_final", arrow::boolean(), false),
                       arrow::field("observation_domain_id", arrow::uint64(), false),
                       arrow::field("ip_family", arrow::uint8(), false),
                       arrow::field("src_ip", arrow::utf8(), false),
                       arrow::field("dst_ip", arrow::utf8(), false),
                       arrow::field("outcome", arrow::utf8(), false),
                       arrow::field("icmp_type", arrow::uint8(), false),
                       arrow::field("icmp_code", arrow::uint8(), false),
                       arrow::field("outer_truncated", arrow::boolean(), false),
                       arrow::field("incomplete_reason", arrow::utf8(), true),
                       arrow::field("echo_id", arrow::uint16(), true),
                       arrow::field("echo_sequence", arrow::uint16(), true),
                       arrow::field("echo_retries", arrow::uint32(), true),
                       arrow::field("request_at_ns", arrow::int64(), true),
                       arrow::field("reply_at_ns", arrow::int64(), true),
                       arrow::field("latency_ns", arrow::int64(), true),
                       arrow::field("quote_status", arrow::utf8(), true),
                       arrow::field("quoted_ip_family", arrow::uint8(), true),
                       arrow::field("quoted_protocol", arrow::uint8(), true),
                       arrow::field("quoted_src_ip", arrow::utf8(), true),
                       arrow::field("quoted_dst_ip", arrow::utf8(), true),
                       arrow::field("quoted_src_port", arrow::uint16(), true),
                       arrow::field("quoted_dst_port", arrow::uint16(), true),
                       arrow::field("active_quoted_session_id", arrow::uint64(), true),
                       arrow::field("next_hop_mtu", arrow::uint32(), true)},
                      arrow::key_value_metadata({"flowsql.entity", "flowsql.schema_version", "flowsql.timestamp_unit",
                                                 "flowsql.revision_semantics", "flowsql.measurement_scope"},
                                                {"icmp_event", "1", "ns", "event", "single_capture_observed_packets"}));
    NpmEntityDescriptorV1 entity;
    entity.entity_id = "icmp_event";
    entity.module_id = "icmp";
    entity.schema = schema;
    entity.revision_semantics = NpmRevisionSemanticsV1::kEvent;
    return entity;
}

NpmModulePlanV1 NpmIcmpModulePlanV1() {
    NpmModulePlanV1 plan;
    plan.module_id = "icmp";
    plan.input_mask = static_cast<uint8_t>(NpmInputKindV1::kControlPacket);
    plan.entities.push_back(NpmIcmpEventEntityDescriptorV1());
    return plan;
}

}  // namespace flowsql::npm
