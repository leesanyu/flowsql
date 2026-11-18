// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_icmp_contract.h"

#include <arrow/api.h>
#include <rapidjson/document.h>

#include <new>
#include <utility>

namespace flowsql::npm {
namespace {

NpmIcmpConfigStatus Fail(NpmIcmpConfigError error, std::string path) { return {error, std::move(path)}; }

const rapidjson::Value* Find(const rapidjson::Value& object, const char* name) {
    const auto member = object.FindMember(name);
    return member == object.MemberEnd() ? nullptr : &member->value;
}

}  // namespace

NpmIcmpConfigStatus ParseNpmIcmpConfig(std::string_view json, NpmIcmpConfig* output) {
    if (!output) return Fail(NpmIcmpConfigError::kInvalidType, "/icmp");
    try {
        rapidjson::Document document;
        document.Parse<rapidjson::kParseValidateEncodingFlag>(json.data(), json.size());
        if (document.HasParseError() || !document.IsObject()) {
            return Fail(NpmIcmpConfigError::kInvalidType, "/icmp");
        }
        for (auto member = document.MemberBegin(); member != document.MemberEnd(); ++member) {
            const std::string_view name(member->name.GetString(), member->name.GetStringLength());
            const std::string path = "/icmp/" + std::string(name);
            for (auto previous = document.MemberBegin(); previous != member; ++previous) {
                if (name == std::string_view(previous->name.GetString(), previous->name.GetStringLength())) {
                    return Fail(NpmIcmpConfigError::kDuplicateField, path);
                }
            }
            if (name != "echo_timeout_ns" && name != "max_pending_echo") {
                return Fail(NpmIcmpConfigError::kUnknownField, path);
            }
        }
        NpmIcmpConfig next;
        if (const auto* timeout = Find(document, "echo_timeout_ns")) {
            if (!timeout->IsInt64()) return Fail(NpmIcmpConfigError::kInvalidType, "/icmp/echo_timeout_ns");
            next.echo_timeout_ns = timeout->GetInt64();
            if (next.echo_timeout_ns < kNpmIcmpMinEchoTimeoutNs || next.echo_timeout_ns > kNpmIcmpMaxEchoTimeoutNs) {
                return Fail(NpmIcmpConfigError::kInvalidRange, "/icmp/echo_timeout_ns");
            }
        }
        if (const auto* pending = Find(document, "max_pending_echo")) {
            if (!pending->IsUint64()) return Fail(NpmIcmpConfigError::kInvalidType, "/icmp/max_pending_echo");
            if (pending->GetUint64() == 0 || pending->GetUint64() > kNpmIcmpDefaultMaxPendingEcho) {
                return Fail(NpmIcmpConfigError::kInvalidRange, "/icmp/max_pending_echo");
            }
            next.max_pending_echo = static_cast<uint32_t>(pending->GetUint64());
        }
        *output = next;
        return {};
    } catch (const std::bad_alloc&) {
        return Fail(NpmIcmpConfigError::kAllocationFailed, "/icmp");
    }
}

NpmEntityDescriptor NpmIcmpEventEntityDescriptor(packet::AddressFamily family) {
    static const auto ipv4 = NpmFamilyEntityDescriptor(NpmFamilyEntityKind::kIcmp, packet::AddressFamily::kIPv4);
    static const auto ipv6 = NpmFamilyEntityDescriptor(NpmFamilyEntityKind::kIcmp, packet::AddressFamily::kIPv6);
    return family == packet::AddressFamily::kIPv4 ? ipv4 : ipv6;
}

NpmModulePlan NpmIcmpModulePlan() {
    NpmModulePlan plan;
    plan.module_id = "icmp";
    plan.input_mask = static_cast<uint8_t>(NpmInputKind::kControlPacket);
    plan.entities = {NpmIcmpEventEntityDescriptor(packet::AddressFamily::kIPv4),
                     NpmIcmpEventEntityDescriptor(packet::AddressFamily::kIPv6)};
    return plan;
}

}  // namespace flowsql::npm
