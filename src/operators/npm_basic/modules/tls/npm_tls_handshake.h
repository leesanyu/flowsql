// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef FLOWSQL_NPM_TLS_HANDSHAKE_H_
#define FLOWSQL_NPM_TLS_HANDSHAKE_H_

#include "npm_tls_framer.h"

namespace flowsql::npm {

enum class NpmTlsOutcome : uint8_t { kServerHelloObserved, kFatalAlertObserved, kIncomplete };

enum class NpmTlsIncompleteReason : uint8_t {
    kNone,
    kEncryptedAfterServerHello,
    kEncryptedAfterChangeCipherSpec,
    kDeadlineAfterServerHello,
    kSessionEndAfterServerHello,
    kClientHelloIncomplete,
    kServerHelloNotObservedByDeadline,
    kHelloRetryRequestUnfinished,
    kClientPathGap,
    kServerPathGap,
    kCaptureTruncation,
    kMalformedRecord,
    kMalformedHello,
    kHelloLimitExceeded,
    kUnsupportedVersion,
    kUnsupportedFraming,
    kServerStreamEnd,
    kSessionEnd,
};

struct NpmTlsHandshakeResult {
    NpmTlsHandshake handshake;
    NpmTlsOutcome outcome = NpmTlsOutcome::kIncomplete;
    NpmTlsIncompleteReason incomplete_reason = NpmTlsIncompleteReason::kNone;
    int64_t observed_at_ns = 0;
    std::optional<int64_t> server_hello_latency_ns;
    std::optional<uint8_t> alert_level;
    std::optional<uint8_t> alert_description;
};

enum class NpmTlsTrackerError : uint8_t { kNone, kInvalidInput, kAllocationFailed };

/** One session; stream views are borrowed only during Consume. A final result is moved out at most once. */
class NpmTlsHandshakeTracker final {
 public:
    NpmTlsHandshakeTracker(uint64_t session_id, NpmTlsConfig config);

    NpmTlsTrackerError Consume(const NpmTcpStreamContext& context, const NpmTcpStreamEvent& event,
                               std::optional<NpmTlsHandshakeResult>* result);
    NpmTlsTrackerError OnCaptureWatermark(int64_t watermark_ns, std::optional<NpmTlsHandshakeResult>* result);
    NpmTlsTrackerError OnSessionEnd(NpmSessionEndReason reason, int64_t observed_at_ns,
                                    std::optional<NpmTlsHandshakeResult>* result);
    void Abort() noexcept;
    bool HasCandidate() const noexcept { return candidate_.has_value(); }
    bool Closed() const noexcept { return closed_; }
    bool Rejected() const noexcept { return !candidate_ && framer_.Closed(); }
    std::optional<int64_t> NextEventDeadlineNs() const noexcept;

 private:
    NpmTlsTrackerError OnFramedEvent(const NpmTlsFramedEvent& event, int64_t observed_at_ns,
                                     std::optional<NpmTlsHandshakeResult>* result);
    void Close(NpmTlsOutcome outcome, NpmTlsIncompleteReason reason, int64_t observed_at_ns,
               std::optional<NpmTlsHandshakeResult>* result);
    void Fail(NpmTlsIncompleteReason reason, int64_t observed_at_ns, std::optional<NpmTlsHandshakeResult>* result);

    uint64_t session_id_;
    NpmTlsConfig config_;
    NpmTlsFramer framer_;
    std::optional<NpmTlsHandshake> candidate_;
    std::optional<int64_t> last_watermark_ns_;
    bool closed_ = false;
};

const char* NpmTlsOutcomeName(NpmTlsOutcome outcome) noexcept;
const char* NpmTlsIncompleteReasonName(NpmTlsIncompleteReason reason) noexcept;

}  // namespace flowsql::npm

#endif  // FLOWSQL_NPM_TLS_HANDSHAKE_H_
