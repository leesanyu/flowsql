// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_tls_contract.h"
#include <operators/npm_basic/npm_family_result_contract.h>

#include <arrow/api.h>
#include <rapidjson/document.h>

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace flowsql::npm {
namespace {

NpmTlsConfigStatus Fail(NpmTlsConfigError error, std::string path) { return {error, std::move(path)}; }

const rapidjson::Value* Find(const rapidjson::Value& object, const char* name) {
    const auto member = object.FindMember(name);
    return member == object.MemberEnd() ? nullptr : &member->value;
}

}  // namespace

NpmTlsConfigStatus ParseNpmTlsConfig(std::string_view json, NpmTlsConfig* output) {
    if (output == nullptr) return Fail(NpmTlsConfigError::kInvalidType, "/tls");
    if (json.empty()) return Fail(NpmTlsConfigError::kMissing, "/tls");
    try {
        rapidjson::Document document;
        document.Parse<rapidjson::kParseValidateEncodingFlag>(json.data(), json.size());
        if (document.HasParseError() || !document.IsObject()) {
            return Fail(NpmTlsConfigError::kInvalidType, "/tls");
        }
        for (auto member = document.MemberBegin(); member != document.MemberEnd(); ++member) {
            const std::string_view name(member->name.GetString(), member->name.GetStringLength());
            const std::string path = "/tls/" + std::string(name);
            for (auto previous = document.MemberBegin(); previous != member; ++previous) {
                if (name == std::string_view(previous->name.GetString(), previous->name.GetStringLength())) {
                    return Fail(NpmTlsConfigError::kDuplicateField, path);
                }
            }
            if (name != "primary_label_ids" && name != "handshake_timeout_ns" && name != "max_hello_bytes") {
                return Fail(NpmTlsConfigError::kUnknownField, path);
            }
        }

        NpmTlsConfig next;
        const auto* labels = Find(document, "primary_label_ids");
        if (labels == nullptr) return Fail(NpmTlsConfigError::kMissing, "/tls/primary_label_ids");
        if (!labels->IsArray()) return Fail(NpmTlsConfigError::kInvalidType, "/tls/primary_label_ids");
        if (labels->Empty() || labels->Size() > kNpmTlsMaxPrimaryLabels) {
            return Fail(NpmTlsConfigError::kInvalidRange, "/tls/primary_label_ids");
        }
        next.primary_label_ids.reserve(labels->Size());
        for (rapidjson::SizeType index = 0; index < labels->Size(); ++index) {
            const auto& label = (*labels)[index];
            const std::string path = "/tls/primary_label_ids/" + std::to_string(index);
            if (!label.IsUint64()) return Fail(NpmTlsConfigError::kInvalidType, path);
            if (label.GetUint64() == 0 || label.GetUint64() > std::numeric_limits<uint32_t>::max() ||
                std::find(next.primary_label_ids.begin(), next.primary_label_ids.end(), label.GetUint64()) !=
                    next.primary_label_ids.end()) {
                return Fail(NpmTlsConfigError::kInvalidRange, path);
            }
            next.primary_label_ids.push_back(static_cast<uint32_t>(label.GetUint64()));
        }
        if (const auto* timeout = Find(document, "handshake_timeout_ns")) {
            if (!timeout->IsInt64()) return Fail(NpmTlsConfigError::kInvalidType, "/tls/handshake_timeout_ns");
            next.handshake_timeout_ns = timeout->GetInt64();
            if (next.handshake_timeout_ns < kNpmTlsMinHandshakeTimeoutNs ||
                next.handshake_timeout_ns > kNpmTlsMaxHandshakeTimeoutNs) {
                return Fail(NpmTlsConfigError::kInvalidRange, "/tls/handshake_timeout_ns");
            }
        }
        if (const auto* maximum = Find(document, "max_hello_bytes")) {
            if (!maximum->IsUint64()) return Fail(NpmTlsConfigError::kInvalidType, "/tls/max_hello_bytes");
            if (maximum->GetUint64() < kNpmTlsMinHelloBytes || maximum->GetUint64() > kNpmTlsMaxHelloBytes) {
                return Fail(NpmTlsConfigError::kInvalidRange, "/tls/max_hello_bytes");
            }
            next.max_hello_bytes = static_cast<uint32_t>(maximum->GetUint64());
        }
        *output = std::move(next);
        return {};
    } catch (const std::bad_alloc&) {
        return Fail(NpmTlsConfigError::kAllocationFailed, "/tls");
    }
}

NpmEntityDescriptor NpmTlsHandshakeEntityDescriptor(packet::AddressFamily family) {
    static const auto v4 = NpmFamilyEntityDescriptor(NpmFamilyEntityKind::kTls, packet::AddressFamily::kIPv4);
    static const auto v6 = NpmFamilyEntityDescriptor(NpmFamilyEntityKind::kTls, packet::AddressFamily::kIPv6);
    return family == packet::AddressFamily::kIPv4 ? v4 : v6;
}

NpmModulePlan NpmTlsModulePlan(const NpmTlsConfig& config) {
    NpmModulePlan plan;
    plan.module_id = "tls";
    plan.input_mask = static_cast<uint8_t>(NpmInputKind::kTcpPacket);
    plan.requires_labeling = true;
    plan.requires_tcp_stream = true;
    plan.primary_label_ids = config.primary_label_ids;
    plan.entities = {NpmTlsHandshakeEntityDescriptor(packet::AddressFamily::kIPv4),
                     NpmTlsHandshakeEntityDescriptor(packet::AddressFamily::kIPv6)};
    return plan;
}

}  // namespace flowsql::npm
