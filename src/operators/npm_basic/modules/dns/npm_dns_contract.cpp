// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_dns_contract.h"
#include <operators/npm_basic/npm_family_result_contract.h>

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

NpmEntityDescriptorV1 NpmDnsTransactionEntityDescriptorV1(packet::AddressFamily family) {
    static const auto v4 = NpmFamilyEntityDescriptor(NpmFamilyEntityKind::kDns, packet::AddressFamily::kIPv4);
    static const auto v6 = NpmFamilyEntityDescriptor(NpmFamilyEntityKind::kDns, packet::AddressFamily::kIPv6);
    return family == packet::AddressFamily::kIPv4 ? v4 : v6;
}

NpmModulePlanV1 NpmDnsModulePlanV1(const NpmDnsConfigV1& config) {
    NpmModulePlanV1 plan;
    plan.module_id = "dns";
    plan.input_mask =
        static_cast<uint8_t>(NpmInputKindV1::kTcpPacket) | static_cast<uint8_t>(NpmInputKindV1::kUdpDatagram);
    plan.requires_labeling = true;
    plan.requires_tcp_stream = true;
    plan.primary_label_ids = config.primary_label_ids;
    plan.entities = {NpmDnsTransactionEntityDescriptorV1(packet::AddressFamily::kIPv4),
                     NpmDnsTransactionEntityDescriptorV1(packet::AddressFamily::kIPv6)};
    return plan;
}

}  // namespace flowsql::npm
