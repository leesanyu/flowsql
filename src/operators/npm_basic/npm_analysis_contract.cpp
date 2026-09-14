// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_analysis_contract.h"

#include <arrow/api.h>

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

std::shared_ptr<arrow::Schema> NpmBasicResultSchema() {
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
    return schema;
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
