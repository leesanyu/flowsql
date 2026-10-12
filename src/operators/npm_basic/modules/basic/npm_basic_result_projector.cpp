// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_basic_result_projector.h"

#include <operators/npm_basic/core/npm_protocol_context.h>

#include <limits>
#include <new>
#include <string>
#include <utility>

namespace flowsql::npm {
namespace {

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

template <typename Address>
NpmBasicProjectionError NpmBasicResultProjector::ProjectActive(const NpmSessionView& session, int64_t observed_at,
                                                               NpmBasicFamilyResult<Address>* output) {
    return Project(session, observed_at, false, NpmSessionEndReason::kEof, output);
}

template <typename Address>
NpmBasicProjectionError NpmBasicResultProjector::ProjectFinal(const NpmSessionView& session, NpmSessionEndReason reason,
                                                              int64_t observed_at,
                                                              NpmBasicFamilyResult<Address>* output) {
    return Project(session, observed_at, true, reason, output);
}

size_t NpmBasicResultProjector::tracked_sessions() const noexcept { return revisions_.size(); }

template <typename Address>
NpmBasicProjectionError NpmBasicResultProjector::Project(const NpmSessionView& session, int64_t observed_at,
                                                         bool is_final, NpmSessionEndReason reason,
                                                         NpmBasicFamilyResult<Address>* output) {
    if (output == nullptr) return NpmBasicProjectionError::kNullOutput;
    if (!session.key || session.session_id == 0 || session.key->Namespace().empty() ||
        session.first_ns > session.last_ns || !ValidSessionProtocol(session)) {
        return NpmBasicProjectionError::kInvalidSession;
    }

    const auto revision = revisions_.find(session.session_id);
    const uint64_t last_revision = revision == revisions_.end() ? 0 : revision->second;
    if (last_revision == std::numeric_limits<uint64_t>::max()) {
        return NpmBasicProjectionError::kRevisionExhausted;
    }

    try {
        NpmBasicFamilyResult<Address> next;
        next.session_id = session.session_id;
        next.observation_domain_id = session.key->Domain();
        next.revision = last_revision + 1;
        next.observed_at = observed_at;
        next.is_final = is_final;
        next.transport_protocol = session.key->Protocol();
        if constexpr (std::is_same_v<Address, network::IPv4Address>) {
            if (!session.key->Get4()) return NpmBasicProjectionError::kInvalidAddress;
            next.a_ip = session.key->Get4()->a_ip;
            next.b_ip = session.key->Get4()->b_ip;
        } else {
            if (!session.key->Get6()) return NpmBasicProjectionError::kInvalidAddress;
            next.a_ip = session.key->Get6()->a_ip;
            next.b_ip = session.key->Get6()->b_ip;
        }
        next.a_port = session.key->APort();
        next.b_port = session.key->BPort();
        next.first_ns = session.first_ns;
        next.last_ns = session.last_ns;
        next.packets_ab = session.packets_ab;
        next.packets_ba = session.packets_ba;
        next.wire_bytes_ab = session.wire_bytes_ab;
        next.wire_bytes_ba = session.wire_bytes_ba;
        if (session.wire_bytes_ba > std::numeric_limits<uint64_t>::max() - session.wire_bytes_ab) {
            return NpmBasicProjectionError::kInvalidResult;
        }
        next.wire_bytes_total = session.wire_bytes_ab + session.wire_bytes_ba;
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

NpmBasicProjectionError NpmBasicResultProjector::ProjectAndWrite(const NpmSessionView& session, int64_t observed_at,
                                                                 std::optional<NpmSessionEndReason> reason,
                                                                 INpmResultWriter& writer, int* write_error,
                                                                 std::optional<NpmBasicPeriodStats> period) {
    *write_error = 0;
    if (!session.key) return NpmBasicProjectionError::kInvalidSession;
    auto emit = [&](auto result) {
        const auto status = reason ? ProjectFinal(session, *reason, observed_at, &result)
                                   : ProjectActive(session, observed_at, &result);
        if (status == NpmBasicProjectionError::kNone) {
            result.period = period;
            *write_error = writer.WriteBasic(result);
        }
        return status;
    };
    return session.key->Get4() ? emit(NpmBasicResult4{}) : emit(NpmBasicResult6{});
}
template NpmBasicProjectionError NpmBasicResultProjector::ProjectActive(const NpmSessionView&, int64_t,
                                                                        NpmBasicResult4*);
template NpmBasicProjectionError NpmBasicResultProjector::ProjectFinal(const NpmSessionView&, NpmSessionEndReason,
                                                                       int64_t, NpmBasicResult4*);
template NpmBasicProjectionError NpmBasicResultProjector::ProjectActive(const NpmSessionView&, int64_t,
                                                                        NpmBasicResult6*);
template NpmBasicProjectionError NpmBasicResultProjector::ProjectFinal(const NpmSessionView&, NpmSessionEndReason,
                                                                       int64_t, NpmBasicResult6*);

}  // namespace flowsql::npm
