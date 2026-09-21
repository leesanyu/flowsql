// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_analysis_contract.h"

#include <arrow/api.h>

#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>

namespace flowsql::npm {
namespace {

template <typename T>
bool IsInRange(T value, T minimum, T maximum) {
    return value >= minimum && value <= maximum;
}

uint64_t SaturatingAdd(uint64_t left, uint64_t right) {
    if (right > std::numeric_limits<uint64_t>::max() - left) {
        return std::numeric_limits<uint64_t>::max();
    }
    return left + right;
}

uint64_t* BudgetCounter(NpmBudgetCategory category, NpmBudgetUsage* usage) {
    switch (category) {
        case NpmBudgetCategory::kSessionState:
            return &usage->session_state_bytes;
        case NpmBudgetCategory::kModuleState:
            return &usage->module_state_bytes;
        case NpmBudgetCategory::kInputBatch:
            return &usage->input_batch_bytes;
        case NpmBudgetCategory::kPendingOutput:
            return &usage->pending_output_bytes;
    }
    return nullptr;
}

bool ValidOptionalRate(const std::optional<double>& value) {
    return value.has_value() && std::isfinite(*value) && *value >= 0.0;
}

bool AnyTcpValue(const NpmSessionResult& result) {
    return result.tcp_unique_payload_bytes_ab.has_value() ||
           result.tcp_unique_payload_bytes_ba.has_value() ||
           result.tcp_unique_payload_bps_ab.has_value() ||
           result.tcp_unique_payload_bps_ba.has_value() || result.tcp_initiator.has_value() ||
           result.tcp_handshake_duration_ns.has_value() || result.tcp_synack_rtt_ns.has_value() ||
           result.tcp_rtt_samples.has_value() || result.tcp_rtt_min_ns.has_value() ||
           result.tcp_rtt_mean_ns.has_value() || result.tcp_rtt_max_ns.has_value() ||
           result.tcp_retrans_packets_ab.has_value() || result.tcp_retrans_packets_ba.has_value() ||
           result.tcp_retrans_payload_bytes_ab.has_value() ||
           result.tcp_retrans_payload_bytes_ba.has_value();
}

}  // namespace

NpmAnalysisConfig DefaultNpmAnalysisConfig(NpmRunMode mode) {
    NpmAnalysisConfig config;
    config.run_mode = mode;
    config.result_mode = mode == NpmRunMode::kRealtime ? NpmResultMode::kPeriodicSnapshot : NpmResultMode::kFinal;
    return config;
}

NpmAnalysisConfigError ValidateNpmAnalysisConfig(const NpmAnalysisConfig& config) {
    if (config.run_mode != NpmRunMode::kOffline && config.run_mode != NpmRunMode::kRealtime) {
        return NpmAnalysisConfigError::kInvalidRunMode;
    }
    if (config.result_mode != NpmResultMode::kFinal && config.result_mode != NpmResultMode::kPeriodicSnapshot) {
        return NpmAnalysisConfigError::kInvalidResultMode;
    }
    if (!IsInRange(config.output_interval_ns, kNpmMinOutputIntervalNs, kNpmMaxOutputIntervalNs)) {
        return NpmAnalysisConfigError::kOutputIntervalOutOfRange;
    }
    if (!IsInRange(config.payload_sample_packets, kNpmMinPayloadSamplePackets, kNpmMaxPayloadSamplePackets)) {
        return NpmAnalysisConfigError::kPayloadSamplePacketsOutOfRange;
    }
    if (!IsInRange(config.tcp_idle_timeout_ns, kNpmMinIdleTimeoutNs, kNpmMaxIdleTimeoutNs)) {
        return NpmAnalysisConfigError::kTcpIdleTimeoutOutOfRange;
    }
    if (!IsInRange(config.udp_idle_timeout_ns, kNpmMinIdleTimeoutNs, kNpmMaxIdleTimeoutNs)) {
        return NpmAnalysisConfigError::kUdpIdleTimeoutOutOfRange;
    }
    if (!IsInRange(config.out_of_order_tolerance_ns,
                   kNpmMinOutOfOrderToleranceNs,
                   kNpmMaxOutOfOrderToleranceNs)) {
        return NpmAnalysisConfigError::kOutOfOrderToleranceOutOfRange;
    }
    if (!IsInRange(config.max_active_sessions, kNpmMinActiveSessions, kNpmMaxActiveSessions)) {
        return NpmAnalysisConfigError::kActiveSessionsOutOfRange;
    }
    if (!IsInRange(config.max_tracked_bytes, kNpmMinTrackedBytes, kNpmMaxTrackedBytes)) {
        return NpmAnalysisConfigError::kTrackedBytesOutOfRange;
    }
    if (!IsInRange(config.max_pending_output_bytes,
                   kNpmMinPendingOutputBytes,
                   kNpmMaxPendingOutputBytes)) {
        return NpmAnalysisConfigError::kPendingOutputBytesOutOfRange;
    }
    if (config.overload_policy != NpmOverloadPolicy::kFail) {
        return NpmAnalysisConfigError::kUnsupportedOverloadPolicy;
    }
    return NpmAnalysisConfigError::kNone;
}

NpmObservationDomainError ValidateNpmObservationDomainMap(const NpmObservationDomainMap& domain_map) {
    if (domain_map.input_namespace.empty()) {
        return NpmObservationDomainError::kEmptyInputNamespace;
    }
    if (domain_map.bindings.empty()) {
        return NpmObservationDomainError::kEmptyBindings;
    }
    for (std::size_t i = 0; i < domain_map.bindings.size(); ++i) {
        for (std::size_t j = i + 1; j < domain_map.bindings.size(); ++j) {
            if (domain_map.bindings[i].source_id == domain_map.bindings[j].source_id) {
                return NpmObservationDomainError::kDuplicateSourceId;
            }
        }
    }
    return NpmObservationDomainError::kNone;
}

NpmObservationDomainError ResolveNpmObservationDomain(const NpmObservationDomainMap& domain_map,
                                                      uint32_t source_id,
                                                      uint64_t* observation_domain_id) {
    if (!observation_domain_id) {
        return NpmObservationDomainError::kNullOutput;
    }
    const auto validation_error = ValidateNpmObservationDomainMap(domain_map);
    if (validation_error != NpmObservationDomainError::kNone) {
        return validation_error;
    }
    for (const auto& binding : domain_map.bindings) {
        if (binding.source_id == source_id) {
            *observation_domain_id = binding.observation_domain_id;
            return NpmObservationDomainError::kNone;
        }
    }
    return NpmObservationDomainError::kUnknownSourceId;
}

const char* NpmProtocolStatusName(NpmProtocolStatus status) {
    switch (status) {
        case NpmProtocolStatus::kPending:
            return "pending";
        case NpmProtocolStatus::kIdentified:
            return "identified";
        case NpmProtocolStatus::kUnknown:
            return "unknown";
    }
    return nullptr;
}

const char* NpmSessionEndReasonName(NpmSessionEndReason reason) {
    switch (reason) {
        case NpmSessionEndReason::kClosed:
            return "closed";
        case NpmSessionEndReason::kIdleTimeout:
            return "idle_timeout";
        case NpmSessionEndReason::kTupleReuse:
            return "tuple_reuse";
        case NpmSessionEndReason::kEof:
            return "eof";
    }
    return nullptr;
}

NpmBasicResultError ValidateNpmBasicResult(const NpmBasicResult& result) {
    if (!NpmProtocolStatusName(result.protocol_status)) {
        return NpmBasicResultError::kInvalidProtocolStatus;
    }
    if (result.end_reason.has_value() && !NpmSessionEndReasonName(*result.end_reason)) {
        return NpmBasicResultError::kInvalidEndReason;
    }
    if (result.is_final && result.protocol_status == NpmProtocolStatus::kPending) {
        return NpmBasicResultError::kPendingFinalResult;
    }
    if (result.protocol_status == NpmProtocolStatus::kIdentified) {
        if (!result.protocol_id.has_value() || !result.protocol.has_value()) {
            return NpmBasicResultError::kProtocolFieldsMismatch;
        }
    } else if (result.protocol_id.has_value() || result.protocol_sub_id.has_value() || result.protocol.has_value()) {
        return NpmBasicResultError::kProtocolFieldsMismatch;
    }
    if (result.is_final && !result.end_reason.has_value()) {
        return NpmBasicResultError::kMissingFinalEndReason;
    }
    if (!result.is_final && result.end_reason.has_value()) {
        return NpmBasicResultError::kUnexpectedActiveEndReason;
    }
    return NpmBasicResultError::kNone;
}

std::shared_ptr<arrow::Schema> NpmBasicResultSchema(bool labeling_enabled) {
    static const std::shared_ptr<arrow::Schema> schema = [] {
        auto fields = std::vector<std::shared_ptr<arrow::Field>>{
            arrow::field("session_id", arrow::uint64(), false),
            arrow::field("observation_domain_id", arrow::uint64(), false),
            arrow::field("revision", arrow::uint64(), false),
            arrow::field("observed_at", arrow::int64(), false),
            arrow::field("is_final", arrow::boolean(), false),
            arrow::field("ip_family", arrow::uint8(), false),
            arrow::field("transport_protocol", arrow::uint8(), false),
            arrow::field("a_ip", arrow::utf8(), false),
            arrow::field("b_ip", arrow::utf8(), false),
            arrow::field("a_port", arrow::uint16(), false),
            arrow::field("b_port", arrow::uint16(), false),
            arrow::field("first_ns", arrow::int64(), false),
            arrow::field("last_ns", arrow::int64(), false),
            arrow::field("packets_ab", arrow::uint64(), false),
            arrow::field("packets_ba", arrow::uint64(), false),
            arrow::field("wire_bytes_ab", arrow::uint64(), false),
            arrow::field("wire_bytes_ba", arrow::uint64(), false),
            arrow::field("protocol_status", arrow::utf8(), false),
            arrow::field("protocol_id", arrow::uint16(), true),
            arrow::field("protocol_sub_id", arrow::uint16(), true),
            arrow::field("protocol", arrow::utf8(), true),
            arrow::field("end_reason", arrow::utf8(), true),
        };
        auto metadata = arrow::key_value_metadata(
            {"flowsql.entity", "flowsql.schema_version", "flowsql.timestamp_unit"},
            {"npm_basic_result", "1", "ns"});
        return arrow::schema(std::move(fields), std::move(metadata));
    }();
    static const std::shared_ptr<arrow::Schema> labeled_schema = [&] {
        auto fields = schema->fields();
        fields.insert(fields.begin() + 17, arrow::field("primary_label_id", arrow::uint32(), false));
        auto metadata = arrow::key_value_metadata(
            {"flowsql.entity", "flowsql.schema_version", "flowsql.timestamp_unit"}, {"npm_basic_result", "1", "ns"});
        return arrow::schema(std::move(fields), std::move(metadata));
    }();
    return labeling_enabled ? labeled_schema : schema;
}

const char* NpmRateStatusName(NpmRateStatus status) {
    switch (status) {
        case NpmRateStatus::kValid:
            return "valid";
        case NpmRateStatus::kInsufficientSpan:
            return "insufficient_span";
    }
    return nullptr;
}

const char* NpmTcpHandshakeStatusName(NpmTcpHandshakeStatus status) {
    switch (status) {
        case NpmTcpHandshakeStatus::kComplete:
            return "complete";
        case NpmTcpHandshakeStatus::kPartial:
            return "partial";
        case NpmTcpHandshakeStatus::kNotObserved:
            return "not_observed";
        case NpmTcpHandshakeStatus::kAmbiguous:
            return "ambiguous";
        case NpmTcpHandshakeStatus::kNotApplicable:
            return "not_applicable";
    }
    return nullptr;
}

const char* NpmTcpRttStatusName(NpmTcpRttStatus status) {
    switch (status) {
        case NpmTcpRttStatus::kValid:
            return "valid";
        case NpmTcpRttStatus::kNoSample:
            return "no_sample";
        case NpmTcpRttStatus::kAmbiguous:
            return "ambiguous";
        case NpmTcpRttStatus::kNotApplicable:
            return "not_applicable";
    }
    return nullptr;
}

const char* NpmTcpRetransmissionStatusName(NpmTcpRetransmissionStatus status) {
    switch (status) {
        case NpmTcpRetransmissionStatus::kValid:
            return "valid";
        case NpmTcpRetransmissionStatus::kAmbiguous:
            return "ambiguous";
        case NpmTcpRetransmissionStatus::kNotApplicable:
            return "not_applicable";
    }
    return nullptr;
}

const char* NpmTcpInitiatorName(NpmTcpInitiator initiator) {
    switch (initiator) {
        case NpmTcpInitiator::kA:
            return "a";
        case NpmTcpInitiator::kB:
            return "b";
    }
    return nullptr;
}

NpmSessionResultError ValidateNpmSessionResult(const NpmSessionResult& result) {
    if (result.session_id == 0 || result.revision == 0 ||
        (result.ip_family != 4 && result.ip_family != 6) || result.a_ip.empty() ||
        result.b_ip.empty()) {
        return NpmSessionResultError::kInvalidIdentity;
    }
    if (!NpmProtocolStatusName(result.protocol_status)) {
        return NpmSessionResultError::kInvalidProtocolStatus;
    }
    if (result.end_reason.has_value() && !NpmSessionEndReasonName(*result.end_reason)) {
        return NpmSessionResultError::kInvalidEndReason;
    }
    if (result.is_final && result.protocol_status == NpmProtocolStatus::kPending) {
        return NpmSessionResultError::kPendingFinalResult;
    }
    if (result.protocol_status == NpmProtocolStatus::kIdentified) {
        if (!result.protocol_id.has_value() || !result.protocol.has_value()) {
            return NpmSessionResultError::kProtocolFieldsMismatch;
        }
    } else if (result.protocol_id.has_value() || result.protocol_sub_id.has_value() ||
               result.protocol.has_value()) {
        return NpmSessionResultError::kProtocolFieldsMismatch;
    }
    if (result.is_final && !result.end_reason.has_value()) {
        return NpmSessionResultError::kMissingFinalEndReason;
    }
    if (!result.is_final && result.end_reason.has_value()) {
        return NpmSessionResultError::kUnexpectedActiveEndReason;
    }

    if (result.last_ns < result.first_ns || result.duration_ns < 0) {
        return NpmSessionResultError::kInvalidTimeRange;
    }
    const uint64_t duration = static_cast<uint64_t>(result.last_ns) -
                              static_cast<uint64_t>(result.first_ns);
    if (duration > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
        result.duration_ns != static_cast<int64_t>(duration)) {
        return NpmSessionResultError::kInvalidTimeRange;
    }
    if (!NpmRateStatusName(result.rate_status)) {
        return NpmSessionResultError::kInvalidRateStatus;
    }
    const bool all_rates = result.wire_bps_ab.has_value() && result.wire_bps_ba.has_value() &&
                           result.payload_bps_ab.has_value() && result.payload_bps_ba.has_value();
    const bool any_rate = result.wire_bps_ab.has_value() || result.wire_bps_ba.has_value() ||
                          result.payload_bps_ab.has_value() || result.payload_bps_ba.has_value();
    if ((result.rate_status == NpmRateStatus::kValid && (result.duration_ns == 0 || !all_rates)) ||
        (result.rate_status == NpmRateStatus::kInsufficientSpan &&
         (result.duration_ns != 0 || any_rate))) {
        return NpmSessionResultError::kRateFieldsMismatch;
    }
    if (all_rates &&
        (!ValidOptionalRate(result.wire_bps_ab) || !ValidOptionalRate(result.wire_bps_ba) ||
         !ValidOptionalRate(result.payload_bps_ab) || !ValidOptionalRate(result.payload_bps_ba))) {
        return NpmSessionResultError::kInvalidRateValue;
    }
    if (result.payload_bytes_ab > result.wire_bytes_ab ||
        result.payload_bytes_ba > result.wire_bytes_ba) {
        return NpmSessionResultError::kTrafficTotalsMismatch;
    }
    if ((result.measurement_flags & ~kNpmMeasurementKnownFlags) != 0) {
        return NpmSessionResultError::kInvalidMeasurementFlags;
    }

    constexpr uint8_t kTcpProtocol = 6;
    constexpr uint8_t kUdpProtocol = 17;
    if (result.transport_protocol != kTcpProtocol && result.transport_protocol != kUdpProtocol) {
        return NpmSessionResultError::kInvalidTransportProtocol;
    }
    if (!NpmTcpHandshakeStatusName(result.tcp_handshake_status)) {
        return NpmSessionResultError::kInvalidTcpHandshakeStatus;
    }
    if (!NpmTcpRttStatusName(result.tcp_rtt_status)) {
        return NpmSessionResultError::kInvalidTcpRttStatus;
    }
    if (!NpmTcpRetransmissionStatusName(result.tcp_retransmission_status)) {
        return NpmSessionResultError::kInvalidTcpRetransmissionStatus;
    }

    if (result.transport_protocol == kUdpProtocol) {
        const uint32_t tcp_flags = kNpmMeasurementMidstreamStart |
                                   kNpmMeasurementSequenceAmbiguous |
                                   kNpmMeasurementSynRetransmitted;
        if (result.tcp_handshake_status != NpmTcpHandshakeStatus::kNotApplicable ||
            result.tcp_rtt_status != NpmTcpRttStatus::kNotApplicable ||
            result.tcp_retransmission_status != NpmTcpRetransmissionStatus::kNotApplicable ||
            AnyTcpValue(result) || (result.measurement_flags & tcp_flags) != 0) {
            return NpmSessionResultError::kTcpFieldsMismatch;
        }
        return NpmSessionResultError::kNone;
    }

    if (result.tcp_handshake_status == NpmTcpHandshakeStatus::kNotApplicable ||
        result.tcp_rtt_status == NpmTcpRttStatus::kNotApplicable ||
        result.tcp_retransmission_status == NpmTcpRetransmissionStatus::kNotApplicable) {
        return NpmSessionResultError::kTcpFieldsMismatch;
    }

    const bool sequence_ambiguous =
        (result.measurement_flags & kNpmMeasurementSequenceAmbiguous) != 0;
    const bool any_unique = result.tcp_unique_payload_bytes_ab.has_value() ||
                            result.tcp_unique_payload_bytes_ba.has_value() ||
                            result.tcp_unique_payload_bps_ab.has_value() ||
                            result.tcp_unique_payload_bps_ba.has_value();
    if (sequence_ambiguous) {
        if (any_unique || result.tcp_rtt_status != NpmTcpRttStatus::kAmbiguous ||
            result.tcp_retransmission_status != NpmTcpRetransmissionStatus::kAmbiguous) {
            return NpmSessionResultError::kTcpFieldsMismatch;
        }
    } else {
        if (!result.tcp_unique_payload_bytes_ab.has_value() ||
            !result.tcp_unique_payload_bytes_ba.has_value()) {
            return NpmSessionResultError::kTcpFieldsMismatch;
        }
        if (*result.tcp_unique_payload_bytes_ab > result.payload_bytes_ab ||
            *result.tcp_unique_payload_bytes_ba > result.payload_bytes_ba) {
            return NpmSessionResultError::kTrafficTotalsMismatch;
        }
        const bool unique_rates = result.tcp_unique_payload_bps_ab.has_value() &&
                                  result.tcp_unique_payload_bps_ba.has_value();
        if ((result.rate_status == NpmRateStatus::kValid && !unique_rates) ||
            (result.rate_status == NpmRateStatus::kInsufficientSpan && unique_rates)) {
            return NpmSessionResultError::kRateFieldsMismatch;
        }
        if (unique_rates &&
            (!ValidOptionalRate(result.tcp_unique_payload_bps_ab) ||
             !ValidOptionalRate(result.tcp_unique_payload_bps_ba))) {
            return NpmSessionResultError::kInvalidRateValue;
        }
    }

    const bool valid_initiator =
        result.tcp_initiator.has_value() && NpmTcpInitiatorName(*result.tcp_initiator) != nullptr;
    switch (result.tcp_handshake_status) {
        case NpmTcpHandshakeStatus::kComplete:
            if (!valid_initiator || !result.tcp_handshake_duration_ns.has_value() ||
                !result.tcp_synack_rtt_ns.has_value() || *result.tcp_handshake_duration_ns < 0 ||
                *result.tcp_synack_rtt_ns < 0 ||
                *result.tcp_synack_rtt_ns > *result.tcp_handshake_duration_ns) {
                return NpmSessionResultError::kHandshakeFieldsMismatch;
            }
            break;
        case NpmTcpHandshakeStatus::kPartial:
            if (!valid_initiator || result.tcp_handshake_duration_ns.has_value() ||
                (result.tcp_synack_rtt_ns.has_value() && *result.tcp_synack_rtt_ns < 0)) {
                return NpmSessionResultError::kHandshakeFieldsMismatch;
            }
            break;
        case NpmTcpHandshakeStatus::kNotObserved:
        case NpmTcpHandshakeStatus::kAmbiguous:
            if (result.tcp_initiator.has_value() || result.tcp_handshake_duration_ns.has_value() ||
                result.tcp_synack_rtt_ns.has_value()) {
                return NpmSessionResultError::kHandshakeFieldsMismatch;
            }
            break;
        case NpmTcpHandshakeStatus::kNotApplicable:
            return NpmSessionResultError::kTcpFieldsMismatch;
    }
    if ((result.measurement_flags & kNpmMeasurementSynRetransmitted) != 0 &&
        result.tcp_handshake_status != NpmTcpHandshakeStatus::kAmbiguous) {
        return NpmSessionResultError::kHandshakeFieldsMismatch;
    }

    switch (result.tcp_rtt_status) {
        case NpmTcpRttStatus::kValid:
            if (!result.tcp_rtt_samples.has_value() || *result.tcp_rtt_samples == 0 ||
                !result.tcp_rtt_min_ns.has_value() || !result.tcp_rtt_mean_ns.has_value() ||
                !result.tcp_rtt_max_ns.has_value() || *result.tcp_rtt_min_ns < 0 ||
                *result.tcp_rtt_mean_ns < *result.tcp_rtt_min_ns ||
                *result.tcp_rtt_max_ns < *result.tcp_rtt_mean_ns) {
                return NpmSessionResultError::kRttFieldsMismatch;
            }
            break;
        case NpmTcpRttStatus::kNoSample:
            if (!result.tcp_rtt_samples.has_value() || *result.tcp_rtt_samples != 0 ||
                result.tcp_rtt_min_ns.has_value() || result.tcp_rtt_mean_ns.has_value() ||
                result.tcp_rtt_max_ns.has_value()) {
                return NpmSessionResultError::kRttFieldsMismatch;
            }
            break;
        case NpmTcpRttStatus::kAmbiguous:
            if (result.tcp_rtt_samples.has_value() || result.tcp_rtt_min_ns.has_value() ||
                result.tcp_rtt_mean_ns.has_value() || result.tcp_rtt_max_ns.has_value()) {
                return NpmSessionResultError::kRttFieldsMismatch;
            }
            break;
        case NpmTcpRttStatus::kNotApplicable:
            return NpmSessionResultError::kTcpFieldsMismatch;
    }

    const bool all_retrans = result.tcp_retrans_packets_ab.has_value() &&
                             result.tcp_retrans_packets_ba.has_value() &&
                             result.tcp_retrans_payload_bytes_ab.has_value() &&
                             result.tcp_retrans_payload_bytes_ba.has_value();
    const bool any_retrans = result.tcp_retrans_packets_ab.has_value() ||
                             result.tcp_retrans_packets_ba.has_value() ||
                             result.tcp_retrans_payload_bytes_ab.has_value() ||
                             result.tcp_retrans_payload_bytes_ba.has_value();
    if ((result.tcp_retransmission_status == NpmTcpRetransmissionStatus::kValid && !all_retrans) ||
        (result.tcp_retransmission_status == NpmTcpRetransmissionStatus::kAmbiguous && any_retrans)) {
        return NpmSessionResultError::kRetransmissionFieldsMismatch;
    }
    if (all_retrans &&
        (*result.tcp_retrans_packets_ab > result.packets_ab ||
         *result.tcp_retrans_packets_ba > result.packets_ba ||
         *result.tcp_retrans_payload_bytes_ab > result.payload_bytes_ab ||
         *result.tcp_retrans_payload_bytes_ba > result.payload_bytes_ba)) {
        return NpmSessionResultError::kTrafficTotalsMismatch;
    }
    return NpmSessionResultError::kNone;
}

std::shared_ptr<arrow::Schema> NpmSessionResultSchema(bool labeling_enabled) {
    static const std::shared_ptr<arrow::Schema> schema = [] {
        auto fields = std::vector<std::shared_ptr<arrow::Field>>{
            arrow::field("session_id", arrow::uint64(), false),
            arrow::field("observation_domain_id", arrow::uint64(), false),
            arrow::field("revision", arrow::uint64(), false),
            arrow::field("observed_at", arrow::int64(), false),
            arrow::field("is_final", arrow::boolean(), false),
            arrow::field("ip_family", arrow::uint8(), false),
            arrow::field("transport_protocol", arrow::uint8(), false),
            arrow::field("a_ip", arrow::utf8(), false),
            arrow::field("b_ip", arrow::utf8(), false),
            arrow::field("a_port", arrow::uint16(), false),
            arrow::field("b_port", arrow::uint16(), false),
            arrow::field("first_ns", arrow::int64(), false),
            arrow::field("last_ns", arrow::int64(), false),
            arrow::field("duration_ns", arrow::int64(), false),
            arrow::field("protocol_status", arrow::utf8(), false),
            arrow::field("protocol_id", arrow::uint16(), true),
            arrow::field("protocol_sub_id", arrow::uint16(), true),
            arrow::field("protocol", arrow::utf8(), true),
            arrow::field("end_reason", arrow::utf8(), true),
            arrow::field("packets_ab", arrow::uint64(), false),
            arrow::field("packets_ba", arrow::uint64(), false),
            arrow::field("wire_bytes_ab", arrow::uint64(), false),
            arrow::field("wire_bytes_ba", arrow::uint64(), false),
            arrow::field("payload_bytes_ab", arrow::uint64(), false),
            arrow::field("payload_bytes_ba", arrow::uint64(), false),
            arrow::field("rate_status", arrow::utf8(), false),
            arrow::field("wire_bps_ab", arrow::float64(), true),
            arrow::field("wire_bps_ba", arrow::float64(), true),
            arrow::field("payload_bps_ab", arrow::float64(), true),
            arrow::field("payload_bps_ba", arrow::float64(), true),
            arrow::field("tcp_unique_payload_bytes_ab", arrow::uint64(), true),
            arrow::field("tcp_unique_payload_bytes_ba", arrow::uint64(), true),
            arrow::field("tcp_unique_payload_bps_ab", arrow::float64(), true),
            arrow::field("tcp_unique_payload_bps_ba", arrow::float64(), true),
            arrow::field("tcp_handshake_status", arrow::utf8(), false),
            arrow::field("tcp_initiator", arrow::utf8(), true),
            arrow::field("tcp_handshake_duration_ns", arrow::int64(), true),
            arrow::field("tcp_synack_rtt_ns", arrow::int64(), true),
            arrow::field("tcp_rtt_status", arrow::utf8(), false),
            arrow::field("tcp_rtt_samples", arrow::uint64(), true),
            arrow::field("tcp_rtt_min_ns", arrow::int64(), true),
            arrow::field("tcp_rtt_mean_ns", arrow::int64(), true),
            arrow::field("tcp_rtt_max_ns", arrow::int64(), true),
            arrow::field("tcp_retransmission_status", arrow::utf8(), false),
            arrow::field("tcp_retrans_packets_ab", arrow::uint64(), true),
            arrow::field("tcp_retrans_packets_ba", arrow::uint64(), true),
            arrow::field("tcp_retrans_payload_bytes_ab", arrow::uint64(), true),
            arrow::field("tcp_retrans_payload_bytes_ba", arrow::uint64(), true),
            arrow::field("measurement_flags", arrow::uint32(), false),
        };
        auto metadata = arrow::key_value_metadata(
            {"flowsql.entity",
             "flowsql.schema_version",
             "flowsql.timestamp_unit",
             "flowsql.revision_semantics",
             "flowsql.measurement_scope"},
            {"npm_session_result", "1", "ns", "cumulative", "single_capture_observed_packets"});
        return arrow::schema(std::move(fields), std::move(metadata));
    }();
    static const std::shared_ptr<arrow::Schema> labeled_schema = [&] {
        auto fields = schema->fields();
        fields.insert(fields.begin() + 14, arrow::field("primary_label_id", arrow::uint32(), false));
        auto metadata = arrow::key_value_metadata(
            {"flowsql.entity", "flowsql.schema_version", "flowsql.timestamp_unit", "flowsql.revision_semantics",
             "flowsql.measurement_scope"},
            {"npm_session_result", "1", "ns", "cumulative", "single_capture_observed_packets"});
        return arrow::schema(std::move(fields), std::move(metadata));
    }();
    return labeling_enabled ? labeled_schema : schema;
}

uint64_t NpmTrackedBudgetBytes(const NpmBudgetUsage& usage) {
    return SaturatingAdd(SaturatingAdd(usage.session_state_bytes, usage.module_state_bytes), usage.input_batch_bytes);
}

NpmBudgetError ReserveNpmBudget(const NpmAnalysisConfig& config,
                                NpmBudgetCategory category,
                                uint64_t bytes,
                                NpmBudgetUsage* usage) {
    if (!usage) {
        return NpmBudgetError::kNullUsage;
    }
    uint64_t* counter = BudgetCounter(category, usage);
    if (!counter) {
        return NpmBudgetError::kInvalidCategory;
    }
    if (category == NpmBudgetCategory::kPendingOutput) {
        if (*counter > config.max_pending_output_bytes || bytes > config.max_pending_output_bytes - *counter) {
            return NpmBudgetError::kPendingOutputLimitExceeded;
        }
    } else {
        const uint64_t tracked_bytes = NpmTrackedBudgetBytes(*usage);
        if (tracked_bytes > config.max_tracked_bytes || bytes > config.max_tracked_bytes - tracked_bytes) {
            return NpmBudgetError::kTrackedLimitExceeded;
        }
    }
    *counter += bytes;
    return NpmBudgetError::kNone;
}

NpmBudgetError ReleaseNpmBudget(NpmBudgetCategory category, uint64_t bytes, NpmBudgetUsage* usage) {
    if (!usage) {
        return NpmBudgetError::kNullUsage;
    }
    uint64_t* counter = BudgetCounter(category, usage);
    if (!counter) {
        return NpmBudgetError::kInvalidCategory;
    }
    if (bytes > *counter) {
        return NpmBudgetError::kReleaseUnderflow;
    }
    *counter -= bytes;
    return NpmBudgetError::kNone;
}

NpmTimeCapabilityError ValidateNpmTimeCapabilities(const NpmAnalysisConfig& config,
                                                   const NpmTimeCapabilities& capabilities) {
    if (config.run_mode != NpmRunMode::kOffline && config.run_mode != NpmRunMode::kRealtime) {
        return NpmTimeCapabilityError::kInvalidRunMode;
    }
    if (config.run_mode == NpmRunMode::kOffline) {
        return NpmTimeCapabilityError::kNone;
    }
    if (!capabilities.monotonic_time_drive) {
        return NpmTimeCapabilityError::kMissingMonotonicTimeDrive;
    }
    if (!capabilities.capture_time_progress) {
        return NpmTimeCapabilityError::kMissingCaptureTimeProgress;
    }
    if (!capabilities.source_idle_confirmation) {
        return NpmTimeCapabilityError::kMissingSourceIdleConfirmation;
    }
    if (!capabilities.source_backlog_state) {
        return NpmTimeCapabilityError::kMissingSourceBacklogState;
    }
    return NpmTimeCapabilityError::kNone;
}

}  // namespace flowsql::npm
