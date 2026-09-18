// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_OPERATORS_NPM_BASIC_NPM_ANALYSIS_CONTRACT_H_
#define _FLOWSQL_OPERATORS_NPM_BASIC_NPM_ANALYSIS_CONTRACT_H_

#include <common/typedef.h>
#include <framework/interfaces/ipacket.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace arrow {
class Schema;
}

namespace flowsql::npm {

enum class NpmRunMode : uint8_t {
    kOffline = 0,
    kRealtime = 1,
};

enum class NpmResultMode : uint8_t {
    kFinal = 0,
    kPeriodicSnapshot = 1,
};

enum class NpmOverloadPolicy : uint8_t {
    kFail = 0,
};

constexpr int64_t kNpmNanosecondsPerSecond = 1000LL * 1000LL * 1000LL;
constexpr int64_t kNpmMinOutputIntervalNs = 10LL * 1000LL * 1000LL;
constexpr int64_t kNpmDefaultOutputIntervalNs = kNpmNanosecondsPerSecond;
constexpr int64_t kNpmMaxOutputIntervalNs = 60LL * 60LL * kNpmNanosecondsPerSecond;

constexpr uint32_t kNpmMinPayloadSamplePackets = 1;
constexpr uint32_t kNpmDefaultPayloadSamplePackets = 8;
constexpr uint32_t kNpmMaxPayloadSamplePackets = 64;

constexpr int64_t kNpmMinIdleTimeoutNs = kNpmNanosecondsPerSecond;
constexpr int64_t kNpmDefaultTcpIdleTimeoutNs = 60LL * kNpmNanosecondsPerSecond;
constexpr int64_t kNpmDefaultUdpIdleTimeoutNs = 30LL * kNpmNanosecondsPerSecond;
constexpr int64_t kNpmMaxIdleTimeoutNs = 24LL * 60LL * 60LL * kNpmNanosecondsPerSecond;

constexpr int64_t kNpmMinOutOfOrderToleranceNs = 0;
constexpr int64_t kNpmDefaultOutOfOrderToleranceNs = kNpmNanosecondsPerSecond;
constexpr int64_t kNpmMaxOutOfOrderToleranceNs = 60LL * kNpmNanosecondsPerSecond;

constexpr uint64_t kNpmMinActiveSessions = 1;
constexpr uint64_t kNpmDefaultActiveSessions = 100000;
constexpr uint64_t kNpmMaxActiveSessions = 10ULL * 1000ULL * 1000ULL;

constexpr uint64_t kNpmMebibyte = 1024ULL * 1024ULL;
constexpr uint64_t kNpmMinTrackedBytes = kNpmMebibyte;
constexpr uint64_t kNpmDefaultTrackedBytes = 256ULL * kNpmMebibyte;
constexpr uint64_t kNpmMaxTrackedBytes = 1024ULL * 1024ULL * kNpmMebibyte;
constexpr uint64_t kNpmMinPendingOutputBytes = kNpmMebibyte;
constexpr uint64_t kNpmDefaultPendingOutputBytes = 64ULL * kNpmMebibyte;
constexpr uint64_t kNpmMaxPendingOutputBytes = 1024ULL * 1024ULL * kNpmMebibyte;

struct NpmAnalysisConfig {
    NpmRunMode run_mode = NpmRunMode::kOffline;
    NpmResultMode result_mode = NpmResultMode::kFinal;
    int64_t output_interval_ns = kNpmDefaultOutputIntervalNs;
    uint32_t payload_sample_packets = kNpmDefaultPayloadSamplePackets;
    int64_t tcp_idle_timeout_ns = kNpmDefaultTcpIdleTimeoutNs;
    int64_t udp_idle_timeout_ns = kNpmDefaultUdpIdleTimeoutNs;
    int64_t out_of_order_tolerance_ns = kNpmDefaultOutOfOrderToleranceNs;
    uint64_t max_active_sessions = kNpmDefaultActiveSessions;
    uint64_t max_tracked_bytes = kNpmDefaultTrackedBytes;
    uint64_t max_pending_output_bytes = kNpmDefaultPendingOutputBytes;
    NpmOverloadPolicy overload_policy = NpmOverloadPolicy::kFail;
};

enum class NpmAnalysisConfigError : uint8_t {
    kNone = 0,
    kInvalidRunMode,
    kInvalidResultMode,
    kOutputIntervalOutOfRange,
    kPayloadSamplePacketsOutOfRange,
    kTcpIdleTimeoutOutOfRange,
    kUdpIdleTimeoutOutOfRange,
    kOutOfOrderToleranceOutOfRange,
    kActiveSessionsOutOfRange,
    kTrackedBytesOutOfRange,
    kPendingOutputBytesOutOfRange,
    kUnsupportedOverloadPolicy,
};

NpmAnalysisConfig DefaultNpmAnalysisConfig(NpmRunMode mode);
NpmAnalysisConfigError ValidateNpmAnalysisConfig(const NpmAnalysisConfig& config);

struct NpmSourceDomainBinding {
    uint32_t source_id = 0;
    uint64_t observation_domain_id = 0;
};

struct NpmObservationDomainMap {
    std::string input_namespace;
    std::vector<NpmSourceDomainBinding> bindings;
};

enum class NpmObservationDomainError : uint8_t {
    kNone = 0,
    kNullOutput,
    kEmptyInputNamespace,
    kEmptyBindings,
    kDuplicateSourceId,
    kUnknownSourceId,
};

NpmObservationDomainError ValidateNpmObservationDomainMap(const NpmObservationDomainMap& domain_map);
NpmObservationDomainError ResolveNpmObservationDomain(const NpmObservationDomainMap& domain_map,
                                                      uint32_t source_id,
                                                      uint64_t* observation_domain_id);

enum class NpmProtocolStatus : uint8_t {
    kPending = 0,
    kIdentified = 1,
    kUnknown = 2,
};

enum class NpmSessionEndReason : uint8_t {
    kClosed = 0,
    kIdleTimeout = 1,
    kTupleReuse = 2,
    kEof = 3,
};

struct NpmBasicResult {
    uint64_t session_id = 0;
    uint64_t observation_domain_id = 0;
    uint64_t revision = 0;
    int64_t observed_at = 0;
    bool is_final = false;
    uint8_t ip_family = 0;
    uint8_t transport_protocol = 0;
    std::string a_ip;
    std::string b_ip;
    uint16_t a_port = 0;
    uint16_t b_port = 0;
    int64_t first_ns = 0;
    int64_t last_ns = 0;
    uint64_t packets_ab = 0;
    uint64_t packets_ba = 0;
    uint64_t wire_bytes_ab = 0;
    uint64_t wire_bytes_ba = 0;
    NpmProtocolStatus protocol_status = NpmProtocolStatus::kPending;
    std::optional<uint16_t> protocol_id;
    std::optional<uint16_t> protocol_sub_id;
    std::optional<std::string> protocol;
    std::optional<NpmSessionEndReason> end_reason;
};

enum class NpmBasicResultError : uint8_t {
    kNone = 0,
    kInvalidProtocolStatus,
    kInvalidEndReason,
    kPendingFinalResult,
    kProtocolFieldsMismatch,
    kMissingFinalEndReason,
    kUnexpectedActiveEndReason,
};

const char* NpmProtocolStatusName(NpmProtocolStatus status);
const char* NpmSessionEndReasonName(NpmSessionEndReason reason);
NpmBasicResultError ValidateNpmBasicResult(const NpmBasicResult& result);
std::shared_ptr<arrow::Schema> NpmBasicResultSchema();

enum class NpmRateStatus : uint8_t {
    kValid = 0,
    kInsufficientSpan = 1,
};

enum class NpmTcpHandshakeStatus : uint8_t {
    kComplete = 0,
    kPartial = 1,
    kNotObserved = 2,
    kAmbiguous = 3,
    kNotApplicable = 4,
};

enum class NpmTcpRttStatus : uint8_t {
    kValid = 0,
    kNoSample = 1,
    kAmbiguous = 2,
    kNotApplicable = 3,
};

enum class NpmTcpRetransmissionStatus : uint8_t {
    kValid = 0,
    kAmbiguous = 1,
    kNotApplicable = 2,
};

enum class NpmTcpInitiator : uint8_t {
    kA = 0,
    kB = 1,
};

constexpr uint32_t kNpmMeasurementMidstreamStart = 1u << 0;
constexpr uint32_t kNpmMeasurementTruncatedPayload = 1u << 1;
constexpr uint32_t kNpmMeasurementTimestampRegression = 1u << 2;
constexpr uint32_t kNpmMeasurementSequenceAmbiguous = 1u << 3;
constexpr uint32_t kNpmMeasurementSynRetransmitted = 1u << 4;
constexpr uint32_t kNpmMeasurementKnownFlags = kNpmMeasurementMidstreamStart |
                                               kNpmMeasurementTruncatedPayload |
                                               kNpmMeasurementTimestampRegression |
                                               kNpmMeasurementSequenceAmbiguous |
                                               kNpmMeasurementSynRetransmitted;

struct NpmSessionResult {
    uint64_t session_id = 0;
    uint64_t observation_domain_id = 0;
    uint64_t revision = 0;
    int64_t observed_at = 0;
    bool is_final = false;
    uint8_t ip_family = 0;
    uint8_t transport_protocol = 0;
    std::string a_ip;
    std::string b_ip;
    uint16_t a_port = 0;
    uint16_t b_port = 0;
    int64_t first_ns = 0;
    int64_t last_ns = 0;
    int64_t duration_ns = 0;
    NpmProtocolStatus protocol_status = NpmProtocolStatus::kPending;
    std::optional<uint16_t> protocol_id;
    std::optional<uint16_t> protocol_sub_id;
    std::optional<std::string> protocol;
    std::optional<NpmSessionEndReason> end_reason;
    uint64_t packets_ab = 0;
    uint64_t packets_ba = 0;
    uint64_t wire_bytes_ab = 0;
    uint64_t wire_bytes_ba = 0;
    uint64_t payload_bytes_ab = 0;
    uint64_t payload_bytes_ba = 0;
    NpmRateStatus rate_status = NpmRateStatus::kInsufficientSpan;
    std::optional<double> wire_bps_ab;
    std::optional<double> wire_bps_ba;
    std::optional<double> payload_bps_ab;
    std::optional<double> payload_bps_ba;
    std::optional<uint64_t> tcp_unique_payload_bytes_ab;
    std::optional<uint64_t> tcp_unique_payload_bytes_ba;
    std::optional<double> tcp_unique_payload_bps_ab;
    std::optional<double> tcp_unique_payload_bps_ba;
    NpmTcpHandshakeStatus tcp_handshake_status = NpmTcpHandshakeStatus::kNotApplicable;
    std::optional<NpmTcpInitiator> tcp_initiator;
    std::optional<int64_t> tcp_handshake_duration_ns;
    std::optional<int64_t> tcp_synack_rtt_ns;
    NpmTcpRttStatus tcp_rtt_status = NpmTcpRttStatus::kNotApplicable;
    std::optional<uint64_t> tcp_rtt_samples;
    std::optional<int64_t> tcp_rtt_min_ns;
    std::optional<int64_t> tcp_rtt_mean_ns;
    std::optional<int64_t> tcp_rtt_max_ns;
    NpmTcpRetransmissionStatus tcp_retransmission_status =
        NpmTcpRetransmissionStatus::kNotApplicable;
    std::optional<uint64_t> tcp_retrans_packets_ab;
    std::optional<uint64_t> tcp_retrans_packets_ba;
    std::optional<uint64_t> tcp_retrans_payload_bytes_ab;
    std::optional<uint64_t> tcp_retrans_payload_bytes_ba;
    uint32_t measurement_flags = 0;
};

enum class NpmSessionResultError : uint8_t {
    kNone = 0,
    kInvalidIdentity,
    kInvalidProtocolStatus,
    kInvalidEndReason,
    kPendingFinalResult,
    kProtocolFieldsMismatch,
    kMissingFinalEndReason,
    kUnexpectedActiveEndReason,
    kInvalidTimeRange,
    kInvalidRateStatus,
    kRateFieldsMismatch,
    kInvalidRateValue,
    kTrafficTotalsMismatch,
    kInvalidTransportProtocol,
    kInvalidTcpHandshakeStatus,
    kHandshakeFieldsMismatch,
    kInvalidTcpRttStatus,
    kRttFieldsMismatch,
    kInvalidTcpRetransmissionStatus,
    kRetransmissionFieldsMismatch,
    kTcpFieldsMismatch,
    kInvalidMeasurementFlags,
};

const char* NpmRateStatusName(NpmRateStatus status);
const char* NpmTcpHandshakeStatusName(NpmTcpHandshakeStatus status);
const char* NpmTcpRttStatusName(NpmTcpRttStatus status);
const char* NpmTcpRetransmissionStatusName(NpmTcpRetransmissionStatus status);
const char* NpmTcpInitiatorName(NpmTcpInitiator initiator);
NpmSessionResultError ValidateNpmSessionResult(const NpmSessionResult& result);
std::shared_ptr<arrow::Schema> NpmSessionResultSchema();

enum class NpmBudgetCategory : uint8_t {
    kSessionState = 0,
    kModuleState = 1,
    kInputBatch = 2,
    kPendingOutput = 3,
};

struct NpmBudgetUsage {
    uint64_t session_state_bytes = 0;
    uint64_t module_state_bytes = 0;
    uint64_t input_batch_bytes = 0;
    uint64_t pending_output_bytes = 0;
};

enum class NpmBudgetError : uint8_t {
    kNone = 0,
    kNullUsage,
    kInvalidCategory,
    kTrackedLimitExceeded,
    kPendingOutputLimitExceeded,
    kReleaseUnderflow,
};

uint64_t NpmTrackedBudgetBytes(const NpmBudgetUsage& usage);
NpmBudgetError ReserveNpmBudget(const NpmAnalysisConfig& config,
                                NpmBudgetCategory category,
                                uint64_t bytes,
                                NpmBudgetUsage* usage);
NpmBudgetError ReleaseNpmBudget(NpmBudgetCategory category, uint64_t bytes, NpmBudgetUsage* usage);

struct NpmEndpoint {
    packet::IpAddress ip;
    uint16_t port = 0;
};

struct NpmSessionKey {
    std::string input_namespace;
    uint64_t observation_domain_id = 0;
    packet::AddressFamily ip_family = packet::AddressFamily::kNone;
    uint8_t transport_protocol = 0;
    NpmEndpoint a;
    NpmEndpoint b;
};

enum class NpmPacketDirection : uint8_t {
    kAToB = 0,
    kBToA = 1,
};

/** TCP facts copied once from a fully validated transport header. Integer fields use host byte order. */
struct NpmTcpPacketFacts {
    bool valid = false;
    bool syn = false;
    bool ack = false;
    bool fin = false;
    bool rst = false;
    uint32_t sequence = 0;
    uint32_t acknowledgment = 0;
    uint16_t window = 0;
};

/** Transport facts derived from declared protocol lengths and the safely captured payload span. */
struct NpmTransportPacketFacts {
    uint32_t payload_wire_bytes = 0;
    uint32_t payload_captured_bytes = 0;
    bool payload_complete = true;
    NpmTcpPacketFacts tcp;
};

/** Borrowed view valid only during the current module callback. It never owns packet bytes. */
struct NpmPacketView {
    packet::PacketView packet;
    const packet::PacketLayerInfo* layer = nullptr;
    Span<const uint8_t> payload;
    NpmPacketDirection direction = NpmPacketDirection::kAToB;
    NpmTransportPacketFacts transport;
};

/** Borrowed view valid only during the current module callback. The engine owns key and state. */
struct NpmSessionView {
    uint64_t session_id = 0;
    const NpmSessionKey* key = nullptr;
    int64_t first_ns = 0;
    int64_t last_ns = 0;
    uint64_t packets_ab = 0;
    uint64_t packets_ba = 0;
    uint64_t wire_bytes_ab = 0;
    uint64_t wire_bytes_ba = 0;
    NpmProtocolStatus protocol_status = NpmProtocolStatus::kPending;
    std::optional<uint16_t> protocol_id;
    std::optional<uint16_t> protocol_sub_id;
};

/** Task-owned synchronous budget service injected into modules; no global provider IID is registered. */
interface INpmTaskBudget {
    virtual ~INpmTaskBudget() = default;
    virtual NpmBudgetError Reserve(NpmBudgetCategory category, uint64_t bytes) = 0;
    virtual NpmBudgetError Release(NpmBudgetCategory category, uint64_t bytes) = 0;
    virtual NpmBudgetUsage Usage() const = 0;
};

interface INpmResultWriter {
    virtual ~INpmResultWriter() = default;

    /** Copies or encodes the call-borrowed result before returning 0; nonzero means it was not accepted. */
    virtual int WriteBasic(const NpmBasicResult& result) = 0;
    virtual int WriteSession(const NpmSessionResult& result) = 0;
};

interface INpmAnalysisModule {
    virtual ~INpmAnalysisModule() = default;

    /**
     * Do not retain views or their pointers. observed_at_ns is the callback trigger/emission time, not session.last_ns.
     * Return 0 on success; every nonzero value aborts the task.
     */
    virtual int OnPacket(const NpmPacketView& packet,
                         const NpmSessionView& session,
                         INpmResultWriter& writer) = 0;
    virtual int OnSessionSnapshot(const NpmSessionView& session,
                                  int64_t observed_at_ns,
                                  INpmResultWriter& writer) = 0;
    virtual int OnSessionEnd(const NpmSessionView& session,
                             NpmSessionEndReason reason,
                             int64_t observed_at_ns,
                             INpmResultWriter& writer) = 0;
};

struct NpmTimeCapabilities {
    bool monotonic_time_drive = false;
    bool capture_time_progress = false;
    bool source_idle_confirmation = false;
    bool source_backlog_state = false;
};

enum class NpmTimeCapabilityError : uint8_t {
    kNone = 0,
    kInvalidRunMode,
    kMissingMonotonicTimeDrive,
    kMissingCaptureTimeProgress,
    kMissingSourceIdleConfirmation,
    kMissingSourceBacklogState,
};

NpmTimeCapabilityError ValidateNpmTimeCapabilities(const NpmAnalysisConfig& config,
                                                   const NpmTimeCapabilities& capabilities);

}  // namespace flowsql::npm

#endif  // _FLOWSQL_OPERATORS_NPM_BASIC_NPM_ANALYSIS_CONTRACT_H_
