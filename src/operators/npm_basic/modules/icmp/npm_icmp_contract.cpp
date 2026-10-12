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

NpmEntityDescriptorV1 NpmIcmpEventEntityDescriptorV1(packet::AddressFamily family) {
    static const auto ipv4 = NpmFamilyEntityDescriptor(NpmFamilyEntityKind::kIcmp, packet::AddressFamily::kIPv4);
    static const auto ipv6 = NpmFamilyEntityDescriptor(NpmFamilyEntityKind::kIcmp, packet::AddressFamily::kIPv6);
    return family == packet::AddressFamily::kIPv4 ? ipv4 : ipv6;
}

NpmModulePlanV1 NpmIcmpModulePlanV1() {
    NpmModulePlanV1 plan;
    plan.module_id = "icmp";
    plan.input_mask = static_cast<uint8_t>(NpmInputKindV1::kControlPacket);
    plan.entities = {NpmIcmpEventEntityDescriptorV1(packet::AddressFamily::kIPv4),
                     NpmIcmpEventEntityDescriptorV1(packet::AddressFamily::kIPv6)};
    return plan;
}

}  // namespace flowsql::npm
