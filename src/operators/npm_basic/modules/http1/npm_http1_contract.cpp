// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_http1_contract.h"
#include <operators/npm_basic/npm_family_result_contract.h>

#include <arrow/api.h>
#include <rapidjson/document.h>

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace flowsql::npm {
namespace {

NpmHttp1ConfigStatusV1 Fail(NpmHttp1ConfigErrorV1 error, std::string path) { return {error, std::move(path)}; }

const rapidjson::Value* Find(const rapidjson::Value& object, const char* name) {
    const auto member = object.FindMember(name);
    return member == object.MemberEnd() ? nullptr : &member->value;
}

}  // namespace

NpmHttp1ConfigStatusV1 ParseNpmHttp1ConfigV1(std::string_view json, NpmHttp1ConfigV1* output) {
    if (!output) return Fail(NpmHttp1ConfigErrorV1::kInvalidType, "/http1");
    if (json.empty()) return Fail(NpmHttp1ConfigErrorV1::kMissing, "/http1");
    try {
        rapidjson::Document document;
        document.Parse<rapidjson::kParseValidateEncodingFlag>(json.data(), json.size());
        if (document.HasParseError() || !document.IsObject()) {
            return Fail(NpmHttp1ConfigErrorV1::kInvalidType, "/http1");
        }
        for (auto member = document.MemberBegin(); member != document.MemberEnd(); ++member) {
            const std::string_view name(member->name.GetString(), member->name.GetStringLength());
            const std::string path = "/http1/" + std::string(name);
            for (auto previous = document.MemberBegin(); previous != member; ++previous) {
                if (name == std::string_view(previous->name.GetString(), previous->name.GetStringLength())) {
                    return Fail(NpmHttp1ConfigErrorV1::kDuplicateField, path);
                }
            }
            if (name != "primary_label_ids" && name != "response_timeout_ns" && name != "max_pending_per_session" &&
                name != "max_header_bytes") {
                return Fail(NpmHttp1ConfigErrorV1::kUnknownField, path);
            }
        }

        NpmHttp1ConfigV1 next;
        const rapidjson::Value* labels = Find(document, "primary_label_ids");
        if (!labels) return Fail(NpmHttp1ConfigErrorV1::kMissing, "/http1/primary_label_ids");
        if (!labels->IsArray()) return Fail(NpmHttp1ConfigErrorV1::kInvalidType, "/http1/primary_label_ids");
        if (labels->Empty() || labels->Size() > kNpmHttp1MaxPrimaryLabelsV1) {
            return Fail(NpmHttp1ConfigErrorV1::kInvalidRange, "/http1/primary_label_ids");
        }
        next.primary_label_ids.reserve(labels->Size());
        for (rapidjson::SizeType index = 0; index < labels->Size(); ++index) {
            const auto& label = (*labels)[index];
            const std::string path = "/http1/primary_label_ids/" + std::to_string(index);
            if (!label.IsUint64()) return Fail(NpmHttp1ConfigErrorV1::kInvalidType, path);
            if (label.GetUint64() == 0 || label.GetUint64() > std::numeric_limits<uint32_t>::max() ||
                std::find(next.primary_label_ids.begin(), next.primary_label_ids.end(), label.GetUint64()) !=
                    next.primary_label_ids.end()) {
                return Fail(NpmHttp1ConfigErrorV1::kInvalidRange, path);
            }
            next.primary_label_ids.push_back(static_cast<uint32_t>(label.GetUint64()));
        }
        if (const auto* timeout = Find(document, "response_timeout_ns")) {
            if (!timeout->IsInt64()) return Fail(NpmHttp1ConfigErrorV1::kInvalidType, "/http1/response_timeout_ns");
            next.response_timeout_ns = timeout->GetInt64();
            if (next.response_timeout_ns < kNpmHttp1MinResponseTimeoutNsV1 ||
                next.response_timeout_ns > kNpmHttp1MaxResponseTimeoutNsV1) {
                return Fail(NpmHttp1ConfigErrorV1::kInvalidRange, "/http1/response_timeout_ns");
            }
        }
        if (const auto* pending = Find(document, "max_pending_per_session")) {
            if (!pending->IsUint64()) {
                return Fail(NpmHttp1ConfigErrorV1::kInvalidType, "/http1/max_pending_per_session");
            }
            if (pending->GetUint64() == 0 || pending->GetUint64() > kNpmHttp1MaxPendingPerSessionV1) {
                return Fail(NpmHttp1ConfigErrorV1::kInvalidRange, "/http1/max_pending_per_session");
            }
            next.max_pending_per_session = static_cast<uint32_t>(pending->GetUint64());
        }
        if (const auto* header = Find(document, "max_header_bytes")) {
            if (!header->IsUint64()) return Fail(NpmHttp1ConfigErrorV1::kInvalidType, "/http1/max_header_bytes");
            if (header->GetUint64() < kNpmHttp1MinHeaderBytesV1 || header->GetUint64() > kNpmHttp1MaxHeaderBytesV1) {
                return Fail(NpmHttp1ConfigErrorV1::kInvalidRange, "/http1/max_header_bytes");
            }
            next.max_header_bytes = static_cast<uint32_t>(header->GetUint64());
        }
        *output = std::move(next);
        return {};
    } catch (const std::bad_alloc&) {
        return Fail(NpmHttp1ConfigErrorV1::kAllocationFailed, "/http1");
    }
}

NpmEntityDescriptorV1 NpmHttp1TransactionEntityDescriptorV1(packet::AddressFamily family) {
    static const auto v4 = NpmFamilyEntityDescriptor(NpmFamilyEntityKind::kHttp1, packet::AddressFamily::kIPv4);
    static const auto v6 = NpmFamilyEntityDescriptor(NpmFamilyEntityKind::kHttp1, packet::AddressFamily::kIPv6);
    return family == packet::AddressFamily::kIPv4 ? v4 : v6;
}

NpmModulePlanV1 NpmHttp1ModulePlanV1(const NpmHttp1ConfigV1& config) {
    NpmModulePlanV1 plan;
    plan.module_id = "http1";
    plan.input_mask = static_cast<uint8_t>(NpmInputKindV1::kTcpPacket);
    plan.requires_labeling = true;
    plan.requires_tcp_stream = true;
    plan.primary_label_ids = config.primary_label_ids;
    plan.entities = {NpmHttp1TransactionEntityDescriptorV1(packet::AddressFamily::kIPv4),
                     NpmHttp1TransactionEntityDescriptorV1(packet::AddressFamily::kIPv6)};
    return plan;
}

}  // namespace flowsql::npm
