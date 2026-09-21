// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_basic_result_projector.h"

#include "npm_protocol_context.h"

#include <arpa/inet.h>

#include <limits>
#include <new>
#include <string>
#include <utility>

namespace flowsql::npm {
namespace {

bool FormatIpAddress(packet::AddressFamily family, const packet::IpAddress& address, std::string* output) {
    char text[INET6_ADDRSTRLEN]{};
    const void* bytes = nullptr;
    int system_family = AF_UNSPEC;
    if (family == packet::AddressFamily::kIPv4) {
        const auto* value = std::get_if<packet::IPv4Address>(&address);
        if (value == nullptr) return false;
        bytes = value->bytes;
        system_family = AF_INET;
    } else if (family == packet::AddressFamily::kIPv6) {
        const auto* value = std::get_if<packet::IPv6Address>(&address);
        if (value == nullptr) return false;
        bytes = value->bytes;
        system_family = AF_INET6;
    } else {
        return false;
    }
    if (inet_ntop(system_family, bytes, text, sizeof(text)) == nullptr) return false;
    *output = text;
    return true;
}

bool ValidSessionProtocol(const NpmSessionView& session) {
    switch (session.protocol_status) {
        case NpmProtocolStatus::kPending:
        case NpmProtocolStatus::kUnknown:
            return !session.protocol_id.has_value() && !session.protocol_sub_id.has_value();
        case NpmProtocolStatus::kIdentified:
            return session.protocol_id.has_value() && *session.protocol_id != 0 &&
                   (!session.protocol_sub_id.has_value() || *session.protocol_sub_id != 0);
    }
    return false;
}

}  // namespace

NpmBasicResultProjector::NpmBasicResultProjector(const NpmProtocolContext& protocol_context)
    : protocol_context_(protocol_context) {}

NpmBasicProjectionError NpmBasicResultProjector::ProjectActive(const NpmSessionView& session,
                                                               int64_t observed_at,
                                                               NpmBasicResult* output) {
    return Project(session, observed_at, false, NpmSessionEndReason::kEof, output);
}

NpmBasicProjectionError NpmBasicResultProjector::ProjectFinal(const NpmSessionView& session,
                                                              NpmSessionEndReason reason,
                                                              int64_t observed_at,
                                                              NpmBasicResult* output) {
    return Project(session, observed_at, true, reason, output);
}

size_t NpmBasicResultProjector::tracked_sessions() const noexcept {
    return revisions_.size();
}

NpmBasicProjectionError NpmBasicResultProjector::Project(const NpmSessionView& session,
                                                         int64_t observed_at,
                                                         bool is_final,
                                                         NpmSessionEndReason reason,
                                                         NpmBasicResult* output) {
    if (output == nullptr) return NpmBasicProjectionError::kNullOutput;
    if (session.key == nullptr || session.session_id == 0 || session.key->input_namespace.empty() ||
        session.first_ns > session.last_ns || !ValidSessionProtocol(session)) {
        return NpmBasicProjectionError::kInvalidSession;
    }

    const auto revision = revisions_.find(session.session_id);
    const uint64_t last_revision = revision == revisions_.end() ? 0 : revision->second;
    if (last_revision == std::numeric_limits<uint64_t>::max()) {
        return NpmBasicProjectionError::kRevisionExhausted;
    }

    try {
        NpmBasicResult next;
        next.session_id = session.session_id;
        next.observation_domain_id = session.key->observation_domain_id;
        next.revision = last_revision + 1;
        next.observed_at = observed_at;
        next.is_final = is_final;
        next.ip_family = static_cast<uint8_t>(session.key->ip_family);
        next.transport_protocol = session.key->transport_protocol;
        if (!FormatIpAddress(session.key->ip_family, session.key->a.ip, &next.a_ip) ||
            !FormatIpAddress(session.key->ip_family, session.key->b.ip, &next.b_ip)) {
            return NpmBasicProjectionError::kInvalidAddress;
        }
        next.a_port = session.key->a.port;
        next.b_port = session.key->b.port;
        next.first_ns = session.first_ns;
        next.last_ns = session.last_ns;
        next.packets_ab = session.packets_ab;
        next.packets_ba = session.packets_ba;
        next.wire_bytes_ab = session.wire_bytes_ab;
        next.wire_bytes_ba = session.wire_bytes_ba;
        next.primary_label_id = session.primary_label_id;
        next.protocol_status = session.protocol_status;
        if (session.protocol_status == NpmProtocolStatus::kIdentified) {
            const uint16_t protocol_id = *session.protocol_id;
            const uint16_t protocol_sub_id = session.protocol_sub_id.value_or(0);
            const char* protocol_name = protocol_context_.ResolveProtocolName(protocol_id, protocol_sub_id);
            if (protocol_name == nullptr) return NpmBasicProjectionError::kProtocolNameUnavailable;
            next.protocol_id = protocol_id;
            next.protocol_sub_id = session.protocol_sub_id;
            next.protocol = std::string(protocol_name);
        }
        if (is_final) next.end_reason = reason;

        if (ValidateNpmBasicResult(next) != NpmBasicResultError::kNone) {
            return NpmBasicProjectionError::kInvalidResult;
        }

        if (is_final) {
            if (revision != revisions_.end()) revisions_.erase(revision);
        } else if (revision == revisions_.end()) {
            revisions_.emplace(session.session_id, next.revision);
        } else {
            revision->second = next.revision;
        }
        *output = std::move(next);
        return NpmBasicProjectionError::kNone;
    } catch (const std::bad_alloc&) {
        return NpmBasicProjectionError::kAllocationFailed;
    }
}

}  // namespace flowsql::npm
