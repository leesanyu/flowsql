// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_session_analysis_module.h"

#include <arpa/inet.h>

#include <cerrno>
#include <limits>
#include <new>
#include <string>
#include <utility>

namespace flowsql::npm {
namespace {

uint64_t RevisionCharge() noexcept {
    return sizeof(std::unordered_map<uint64_t, uint64_t>::value_type) + 4 * sizeof(void*);
}

int TrackerErrorToErrno(NpmTcpPerformanceError error) noexcept {
    switch (error) {
        case NpmTcpPerformanceError::kNone:
            return 0;
        case NpmTcpPerformanceError::kNullBudget:
            return ENODEV;
        case NpmTcpPerformanceError::kNotFound:
            return ENOENT;
        case NpmTcpPerformanceError::kBudgetExceeded:
        case NpmTcpPerformanceError::kRangeLimitExceeded:
            return ENOSPC;
        case NpmTcpPerformanceError::kAllocationFailed:
            return ENOMEM;
        case NpmTcpPerformanceError::kNullOutput:
        case NpmTcpPerformanceError::kInvalidSession:
        case NpmTcpPerformanceError::kInvalidPacket:
        case NpmTcpPerformanceError::kInvalidRangeLimit:
        case NpmTcpPerformanceError::kProtocolMismatch:
        case NpmTcpPerformanceError::kSessionViewMismatch:
            return EINVAL;
    }
    return EINVAL;
}

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

bool ValidSessionProtocol(const NpmSessionView& session) noexcept {
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

NpmSessionAnalysisModule::NpmSessionAnalysisModule(const NpmProtocolContext& protocol_context,
                                                   std::shared_ptr<INpmTaskBudget> budget,
                                                   uint32_t max_ranges_per_direction)
    : protocol_context_(protocol_context),
      budget_(std::move(budget)),
      tracker_(budget_, max_ranges_per_direction) {}

NpmSessionAnalysisModule::~NpmSessionAnalysisModule() {
    Clear();
}

int NpmSessionAnalysisModule::OnPacket(const NpmPacketView& packet,
                                       const NpmSessionView& session,
                                       INpmResultWriter& writer) {
    (void)writer;
    return TrackerErrorToErrno(tracker_.Observe(packet, session));
}

int NpmSessionAnalysisModule::OnSessionSnapshot(const NpmSessionView& session,
                                                int64_t observed_at_ns,
                                                INpmResultWriter& writer) {
    return Emit(session, false, NpmSessionEndReason::kEof, observed_at_ns, writer);
}

int NpmSessionAnalysisModule::OnSessionEnd(const NpmSessionView& session,
                                           NpmSessionEndReason reason,
                                           int64_t observed_at_ns,
                                           INpmResultWriter& writer) {
    return Emit(session, true, reason, observed_at_ns, writer);
}

int NpmSessionAnalysisModule::Emit(const NpmSessionView& session,
                                   bool is_final,
                                   NpmSessionEndReason reason,
                                   int64_t observed_at_ns,
                                   INpmResultWriter& writer) {
    NpmTcpPerformanceSnapshot performance;
    const int snapshot_error = TrackerErrorToErrno(tracker_.Snapshot(session, &performance));
    if (snapshot_error != 0) return snapshot_error;
    if (session.key == nullptr || session.session_id == 0 || session.key->input_namespace.empty() ||
        session.first_ns > session.last_ns || !ValidSessionProtocol(session)) {
        return EINVAL;
    }

    const uint64_t duration = static_cast<uint64_t>(session.last_ns) -
                              static_cast<uint64_t>(session.first_ns);
    if (duration > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) return EINVAL;

    auto revision = revisions_.find(session.session_id);
    const uint64_t last_revision = revision == revisions_.end() ? 0 : revision->second;
    if (last_revision == std::numeric_limits<uint64_t>::max()) return EOVERFLOW;

    try {
        NpmSessionResult result;
        result.session_id = session.session_id;
        result.observation_domain_id = session.key->observation_domain_id;
        result.revision = last_revision + 1;
        result.observed_at = observed_at_ns;
        result.is_final = is_final;
        result.ip_family = static_cast<uint8_t>(session.key->ip_family);
        result.transport_protocol = session.key->transport_protocol;
        if (!FormatIpAddress(session.key->ip_family, session.key->a.ip, &result.a_ip) ||
            !FormatIpAddress(session.key->ip_family, session.key->b.ip, &result.b_ip)) {
            return EINVAL;
        }
        result.a_port = session.key->a.port;
        result.b_port = session.key->b.port;
        result.first_ns = session.first_ns;
        result.last_ns = session.last_ns;
        result.duration_ns = static_cast<int64_t>(duration);
        result.protocol_status = session.protocol_status;
        if (session.protocol_status == NpmProtocolStatus::kIdentified) {
            const uint16_t protocol_id = *session.protocol_id;
            const uint16_t protocol_sub_id = session.protocol_sub_id.value_or(0);
            const char* protocol_name = protocol_context_.ResolveProtocolName(protocol_id, protocol_sub_id);
            if (protocol_name == nullptr) return EINVAL;
            result.protocol_id = protocol_id;
            result.protocol_sub_id = session.protocol_sub_id;
            result.protocol = std::string(protocol_name);
        }
        if (is_final) result.end_reason = reason;
        result.packets_ab = session.packets_ab;
        result.packets_ba = session.packets_ba;
        result.wire_bytes_ab = session.wire_bytes_ab;
        result.wire_bytes_ba = session.wire_bytes_ba;
        result.payload_bytes_ab = performance.payload_bytes_ab;
        result.payload_bytes_ba = performance.payload_bytes_ba;
        result.rate_status = performance.rate_status;
        result.wire_bps_ab = performance.wire_bps_ab;
        result.wire_bps_ba = performance.wire_bps_ba;
        result.payload_bps_ab = performance.payload_bps_ab;
        result.payload_bps_ba = performance.payload_bps_ba;
        result.tcp_unique_payload_bytes_ab = performance.unique_payload_bytes_ab;
        result.tcp_unique_payload_bytes_ba = performance.unique_payload_bytes_ba;
        result.tcp_unique_payload_bps_ab = performance.unique_payload_bps_ab;
        result.tcp_unique_payload_bps_ba = performance.unique_payload_bps_ba;
        result.tcp_handshake_status = performance.handshake_status;
        result.tcp_initiator = performance.initiator;
        result.tcp_handshake_duration_ns = performance.handshake_duration_ns;
        result.tcp_synack_rtt_ns = performance.synack_rtt_ns;
        result.tcp_rtt_status = performance.rtt_status;
        result.tcp_rtt_samples = performance.rtt_samples;
        result.tcp_rtt_min_ns = performance.rtt_min_ns;
        result.tcp_rtt_mean_ns = performance.rtt_mean_ns;
        result.tcp_rtt_max_ns = performance.rtt_max_ns;
        result.tcp_retransmission_status = performance.retransmission_status;
        result.tcp_retrans_packets_ab = performance.retrans_packets_ab;
        result.tcp_retrans_packets_ba = performance.retrans_packets_ba;
        result.tcp_retrans_payload_bytes_ab = performance.retrans_payload_bytes_ab;
        result.tcp_retrans_payload_bytes_ba = performance.retrans_payload_bytes_ba;
        result.measurement_flags = performance.measurement_flags;
        if (ValidateNpmSessionResult(result) != NpmSessionResultError::kNone) return EINVAL;

        bool inserted_revision = false;
        if (revision == revisions_.end()) {
            const uint64_t charge = RevisionCharge();
            if (!budget_) return ENODEV;
            if (budget_->Reserve(NpmBudgetCategory::kModuleState, charge) != NpmBudgetError::kNone) {
                return ENOSPC;
            }
            try {
                const auto inserted = revisions_.emplace(session.session_id, 0);
                revision = inserted.first;
                inserted_revision = inserted.second;
            } catch (const std::bad_alloc&) {
                budget_->Release(NpmBudgetCategory::kModuleState, charge);
                return ENOMEM;
            }
            if (!inserted_revision) {
                budget_->Release(NpmBudgetCategory::kModuleState, charge);
                return EINVAL;
            }
            revision_bytes_ += charge;
        }

        const int write_error = writer.WriteSession(result);
        if (write_error != 0) {
            if (inserted_revision) {
                revisions_.erase(revision);
                ReleaseRevision(RevisionCharge());
            }
            return write_error;
        }

        if (is_final) {
            revisions_.erase(revision);
            ReleaseRevision(RevisionCharge());
            tracker_.Remove(session.session_id);
        } else {
            revision->second = result.revision;
        }
        return 0;
    } catch (const std::bad_alloc&) {
        return ENOMEM;
    }
}

void NpmSessionAnalysisModule::Clear() noexcept {
    tracker_.Clear();
    revisions_.clear();
    ReleaseRevision(revision_bytes_);
}

size_t NpmSessionAnalysisModule::tracked_sessions() const noexcept {
    return tracker_.size();
}

uint64_t NpmSessionAnalysisModule::tracked_bytes() const noexcept {
    return tracker_.tracked_bytes() + revision_bytes_;
}

void NpmSessionAnalysisModule::ReleaseRevision(uint64_t bytes) noexcept {
    if (bytes == 0 || !budget_) return;
    if (budget_->Release(NpmBudgetCategory::kModuleState, bytes) == NpmBudgetError::kNone) {
        revision_bytes_ -= bytes;
    }
}

}  // namespace flowsql::npm
