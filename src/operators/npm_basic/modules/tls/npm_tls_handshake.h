// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef FLOWSQL_NPM_TLS_HANDSHAKE_H_
#define FLOWSQL_NPM_TLS_HANDSHAKE_H_

#include "npm_tls_framer.h"

namespace flowsql::npm {

enum class NpmTlsOutcomeV1 : uint8_t { kServerHelloObserved, kFatalAlertObserved, kIncomplete };

enum class NpmTlsIncompleteReasonV1 : uint8_t {
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

struct NpmTlsHandshakeResultV1 {
    NpmTlsHandshakeV1 handshake;
    NpmTlsOutcomeV1 outcome = NpmTlsOutcomeV1::kIncomplete;
    NpmTlsIncompleteReasonV1 incomplete_reason = NpmTlsIncompleteReasonV1::kNone;
    int64_t observed_at_ns = 0;
    std::optional<int64_t> server_hello_latency_ns;
    std::optional<uint8_t> alert_level;
    std::optional<uint8_t> alert_description;
};

enum class NpmTlsTrackerErrorV1 : uint8_t { kNone, kInvalidInput, kAllocationFailed };

/** One session; stream views are borrowed only during Consume. A final result is moved out at most once. */
class NpmTlsHandshakeTrackerV1 final {
 public:
    NpmTlsHandshakeTrackerV1(uint64_t session_id, NpmTlsConfigV1 config);

    NpmTlsTrackerErrorV1 Consume(const NpmTcpStreamContextV1& context, const NpmTcpStreamEventV1& event,
                                 std::optional<NpmTlsHandshakeResultV1>* result);
    NpmTlsTrackerErrorV1 OnCaptureWatermark(int64_t watermark_ns, std::optional<NpmTlsHandshakeResultV1>* result);
    NpmTlsTrackerErrorV1 OnSessionEnd(NpmSessionEndReason reason, int64_t observed_at_ns,
                                      std::optional<NpmTlsHandshakeResultV1>* result);
    void Abort() noexcept;
    bool HasCandidate() const noexcept { return candidate_.has_value(); }
    bool Closed() const noexcept { return closed_; }
    bool Rejected() const noexcept { return !candidate_ && framer_.Closed(); }
    std::optional<int64_t> NextEventDeadlineNs() const noexcept;

 private:
    NpmTlsTrackerErrorV1 OnFramedEvent(const NpmTlsFramedEventV1& event, int64_t observed_at_ns,
                                       std::optional<NpmTlsHandshakeResultV1>* result);
    void Close(NpmTlsOutcomeV1 outcome, NpmTlsIncompleteReasonV1 reason, int64_t observed_at_ns,
               std::optional<NpmTlsHandshakeResultV1>* result);
    void Fail(NpmTlsIncompleteReasonV1 reason, int64_t observed_at_ns, std::optional<NpmTlsHandshakeResultV1>* result);

    uint64_t session_id_;
    NpmTlsConfigV1 config_;
    NpmTlsFramerV1 framer_;
    std::optional<NpmTlsHandshakeV1> candidate_;
    std::optional<int64_t> last_watermark_ns_;
    bool closed_ = false;
};

const char* NpmTlsOutcomeNameV1(NpmTlsOutcomeV1 outcome) noexcept;
const char* NpmTlsIncompleteReasonNameV1(NpmTlsIncompleteReasonV1 reason) noexcept;

}  // namespace flowsql::npm

#endif  // FLOWSQL_NPM_TLS_HANDSHAKE_H_
