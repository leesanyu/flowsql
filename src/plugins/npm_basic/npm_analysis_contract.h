// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_PLUGINS_NPM_BASIC_NPM_ANALYSIS_CONTRACT_H_
#define _FLOWSQL_PLUGINS_NPM_BASIC_NPM_ANALYSIS_CONTRACT_H_

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

/** Borrowed view valid only during the current module callback. It never owns packet bytes. */
struct NpmPacketView {
    packet::PacketView packet;
    const packet::PacketLayerInfo* layer = nullptr;
    Span<const uint8_t> payload;
    NpmPacketDirection direction = NpmPacketDirection::kAToB;
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
};

interface INpmAnalysisModule {
    virtual ~INpmAnalysisModule() = default;

    /** Do not retain views or their pointers. Return 0 on success; every nonzero value aborts the task. */
    virtual int OnPacket(const NpmPacketView& packet,
                         const NpmSessionView& session,
                         INpmResultWriter& writer) = 0;
    virtual int OnSessionEnd(const NpmSessionView& session,
                             NpmSessionEndReason reason,
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

#endif  // _FLOWSQL_PLUGINS_NPM_BASIC_NPM_ANALYSIS_CONTRACT_H_
