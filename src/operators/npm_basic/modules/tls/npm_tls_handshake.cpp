// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_tls_handshake.h"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace flowsql::npm {
namespace {

using Reason = NpmTlsIncompleteReason;
using Outcome = NpmTlsOutcome;

bool OffersVersion(const NpmTlsHelloFacts& hello, uint16_t version) {
    return std::find(hello.offered_versions.begin(), hello.offered_versions.end(), version) !=
           hello.offered_versions.end();
}

bool ValidClientHello(const NpmTlsHelloFacts& hello) {
    if (hello.legacy_version != 0x0303) return false;
    return hello.offered_versions.empty() || OffersVersion(hello, 0x0303) || OffersVersion(hello, 0x0304);
}

bool ValidCipherSuite(uint16_t cipher) { return cipher != 0 && cipher != 0x00ff && cipher != 0x5600; }

Reason FramerReason(NpmTlsFramerError error, NpmPacketDirection direction, NpmPacketDirection client) {
    switch (error) {
        case NpmTlsFramerError::kGap:
            return direction == client ? Reason::kClientPathGap : Reason::kServerPathGap;
        case NpmTlsFramerError::kCaptureTruncation:
            return Reason::kCaptureTruncation;
        case NpmTlsFramerError::kMalformedRecord:
            return Reason::kMalformedRecord;
        case NpmTlsFramerError::kMalformedHello:
            return Reason::kMalformedHello;
        case NpmTlsFramerError::kHelloLimit:
            return Reason::kHelloLimitExceeded;
        case NpmTlsFramerError::kIncomplete:
            return Reason::kClientHelloIncomplete;
        default:
            return Reason::kUnsupportedFraming;
    }
}

}  // namespace

NpmTlsHandshakeTracker::NpmTlsHandshakeTracker(uint64_t session_id, NpmTlsConfig config)
    : session_id_(session_id), config_(std::move(config)), framer_(config_.max_hello_bytes) {}

std::optional<int64_t> NpmTlsHandshakeTracker::NextEventDeadlineNs() const noexcept {
    return closed_ || !candidate_ ? std::nullopt : candidate_->deadline_ns;
}

void NpmTlsHandshakeTracker::Close(Outcome outcome, Reason reason, int64_t observed_at_ns,
                                   std::optional<NpmTlsHandshakeResult>* result) {
    if (closed_ || !candidate_) return;
    NpmTlsHandshakeResult next;
    next.handshake = std::move(*candidate_);
    next.handshake.phase = NpmTlsHandshakePhase::kClosed;
    next.outcome = outcome;
    next.incomplete_reason = reason;
    next.observed_at_ns = observed_at_ns;
    if (next.handshake.first_client_hello && next.handshake.final_server_hello) {
        const auto client_at = next.handshake.first_client_hello->complete_at_ns;
        const auto server_at = next.handshake.final_server_hello->complete_at_ns;
        if (client_at && server_at && *server_at >= *client_at) {
            next.server_hello_latency_ns = *server_at - *client_at;
        }
    }
    result->emplace(std::move(next));
    candidate_.reset();
    closed_ = true;
}

void NpmTlsHandshakeTracker::Fail(Reason reason, int64_t observed_at_ns, std::optional<NpmTlsHandshakeResult>* result) {
    Close(Outcome::kIncomplete, reason, observed_at_ns, result);
}

NpmTlsTrackerError NpmTlsHandshakeTracker::OnFramedEvent(const NpmTlsFramedEvent& event, int64_t observed_at_ns,
                                                         std::optional<NpmTlsHandshakeResult>* result) {
    if (!candidate_ || closed_) return NpmTlsTrackerError::kNone;
    const bool client = event.direction == candidate_->client_direction;
    const auto phase = candidate_->phase;
    if (!candidate_->first_client_hello && event.kind != NpmTlsFramedKind::kClientHello) {
        Fail(Reason::kClientHelloIncomplete, observed_at_ns, result);
        return NpmTlsTrackerError::kNone;
    }
    if (event.kind == NpmTlsFramedKind::kAlert) {
        if (event.record_length == 2 && event.control_bytes[0] == 2) {
            Close(Outcome::kFatalAlertObserved, Reason::kNone, event.complete_at_ns.value_or(observed_at_ns), result);
            result->value().alert_level = 2;
            result->value().alert_description = event.control_bytes[1];
        }
        return NpmTlsTrackerError::kNone;
    }
    if (event.kind == NpmTlsFramedKind::kChangeCipherSpec) {
        if (event.record_length != 1 || event.control_bytes[0] != 1) {
            Fail(Reason::kUnsupportedFraming, observed_at_ns, result);
        } else if (phase == NpmTlsHandshakePhase::kAwaitingTls12Boundary) {
            Close(Outcome::kServerHelloObserved, Reason::kEncryptedAfterChangeCipherSpec, observed_at_ns, result);
        }
        return NpmTlsTrackerError::kNone;
    }
    if (event.kind == NpmTlsFramedKind::kApplicationData) {
        if (!client || phase == NpmTlsHandshakePhase::kAwaitingTls12Boundary) {
            Fail(Reason::kUnsupportedFraming, observed_at_ns, result);
        }
        return NpmTlsTrackerError::kNone;
    }
    if (event.kind == NpmTlsFramedKind::kOtherHandshake) {
        if (phase == NpmTlsHandshakePhase::kAwaitingRetryClientHello ||
            (phase == NpmTlsHandshakePhase::kAwaitingFinalServerHello && client)) {
            Fail(Reason::kUnsupportedFraming, observed_at_ns, result);
        }
        return NpmTlsTrackerError::kNone;
    }
    if (!event.hello) {
        Fail(Reason::kMalformedHello, observed_at_ns, result);
        return NpmTlsTrackerError::kNone;
    }
    if (event.kind == NpmTlsFramedKind::kClientHello) {
        if (!client || !ValidClientHello(*event.hello)) {
            Fail(client ? Reason::kUnsupportedVersion : Reason::kUnsupportedFraming, observed_at_ns, result);
        } else if (phase == NpmTlsHandshakePhase::kCandidate) {
            candidate_->first_client_hello = *event.hello;
            candidate_->phase = NpmTlsHandshakePhase::kAwaitingServerHello;
        } else if (phase == NpmTlsHandshakePhase::kAwaitingRetryClientHello && OffersVersion(*event.hello, 0x0304)) {
            candidate_->retry_client_hello = *event.hello;
            candidate_->phase = NpmTlsHandshakePhase::kAwaitingFinalServerHello;
        } else {
            Fail(Reason::kUnsupportedFraming, observed_at_ns, result);
        }
        return NpmTlsTrackerError::kNone;
    }
    if (client || (phase != NpmTlsHandshakePhase::kAwaitingServerHello &&
                   phase != NpmTlsHandshakePhase::kAwaitingFinalServerHello)) {
        Fail(Reason::kUnsupportedFraming, observed_at_ns, result);
        return NpmTlsTrackerError::kNone;
    }
    const auto& hello = *event.hello;
    if (hello.legacy_version != 0x0303 || !hello.cipher_suite || !ValidCipherSuite(*hello.cipher_suite)) {
        Fail(Reason::kUnsupportedVersion, observed_at_ns, result);
        return NpmTlsTrackerError::kNone;
    }
    if (hello.hello_retry_request) {
        if (phase != NpmTlsHandshakePhase::kAwaitingServerHello || hello.selected_version != 0x0304 ||
            !OffersVersion(*candidate_->first_client_hello, 0x0304) || hello.selected_alpn) {
            Fail(Reason::kUnsupportedFraming, observed_at_ns, result);
        } else {
            candidate_->hello_retry_count = 1;
            candidate_->phase = NpmTlsHandshakePhase::kAwaitingRetryClientHello;
        }
        return NpmTlsTrackerError::kNone;
    }
    const bool tls13 = hello.selected_version == 0x0304;
    if (hello.selected_version && !tls13 && hello.selected_version != 0x0303) {
        Fail(Reason::kUnsupportedVersion, observed_at_ns, result);
        return NpmTlsTrackerError::kNone;
    }
    if (phase == NpmTlsHandshakePhase::kAwaitingFinalServerHello && !tls13) {
        Fail(Reason::kUnsupportedVersion, observed_at_ns, result);
        return NpmTlsTrackerError::kNone;
    }
    if (tls13 && (!OffersVersion(*candidate_->first_client_hello, 0x0304) || hello.selected_alpn)) {
        Fail(Reason::kUnsupportedVersion, observed_at_ns, result);
        return NpmTlsTrackerError::kNone;
    }
    candidate_->final_server_hello = hello;
    if (tls13) {
        Close(Outcome::kServerHelloObserved, Reason::kEncryptedAfterServerHello, observed_at_ns, result);
    } else {
        candidate_->phase = NpmTlsHandshakePhase::kAwaitingTls12Boundary;
    }
    return NpmTlsTrackerError::kNone;
}

NpmTlsTrackerError NpmTlsHandshakeTracker::Consume(const NpmTcpStreamContext& context, const NpmTcpStreamEvent& event,
                                                   std::optional<NpmTlsHandshakeResult>* result) {
    if (!result || !context.session || context.session->session_id != session_id_) {
        return NpmTlsTrackerError::kInvalidInput;
    }
    result->reset();
    if (closed_) return NpmTlsTrackerError::kNone;
    try {
        std::vector<NpmTlsFramedEvent> events;
        const auto framed = framer_.Consume(context, event, &events);
        if (framed == NpmTlsFramerError::kAllocationFailed) return NpmTlsTrackerError::kAllocationFailed;
        if (framed == NpmTlsFramerError::kInvalidInput) return NpmTlsTrackerError::kInvalidInput;
        if (!candidate_ && framer_.HasCandidate()) {
            candidate_.emplace();
            candidate_->session_id = session_id_;
            candidate_->client_direction = framer_.ClientDirection();
            candidate_->first_client_byte_at_ns = framer_.FirstClientByteAtNs();
            if (candidate_->first_client_byte_at_ns) {
                const int64_t first = *candidate_->first_client_byte_at_ns;
                const int64_t maximum = std::numeric_limits<int64_t>::max();
                candidate_->deadline_ns =
                    first > maximum - config_.handshake_timeout_ns ? maximum : first + config_.handshake_timeout_ns;
            }
        }
        for (const auto& item : events) {
            const auto error = OnFramedEvent(item, context.observed_at_ns, result);
            if (error != NpmTlsTrackerError::kNone || closed_) return error;
        }
        if (framed != NpmTlsFramerError::kNone && candidate_) {
            Reason reason = FramerReason(framed, context.direction, candidate_->client_direction);
            if (framed == NpmTlsFramerError::kIncomplete) {
                if (candidate_->phase == NpmTlsHandshakePhase::kAwaitingRetryClientHello ||
                    candidate_->phase == NpmTlsHandshakePhase::kAwaitingFinalServerHello) {
                    reason = Reason::kHelloRetryRequestUnfinished;
                } else if (candidate_->first_client_hello && context.direction != candidate_->client_direction) {
                    reason = Reason::kServerStreamEnd;
                }
            }
            if (candidate_->final_server_hello && candidate_->phase == NpmTlsHandshakePhase::kAwaitingTls12Boundary &&
                framed == NpmTlsFramerError::kIncomplete && event.end_reason == NpmTcpStreamEndReason::kSessionEnd) {
                Close(Outcome::kServerHelloObserved, Reason::kSessionEndAfterServerHello, context.observed_at_ns,
                      result);
            } else {
                Fail(reason, context.observed_at_ns, result);
            }
        }
        if (!closed_ && candidate_ && event.kind == NpmTcpStreamEventKind::kEnd &&
            event.end_reason == NpmTcpStreamEndReason::kFin && context.direction != candidate_->client_direction &&
            !candidate_->final_server_hello) {
            Fail(Reason::kServerStreamEnd, context.observed_at_ns, result);
        }
        return NpmTlsTrackerError::kNone;
    } catch (const std::bad_alloc&) {
        return NpmTlsTrackerError::kAllocationFailed;
    }
}

NpmTlsTrackerError NpmTlsHandshakeTracker::OnCaptureWatermark(int64_t watermark_ns,
                                                              std::optional<NpmTlsHandshakeResult>* result) {
    if (!result) return NpmTlsTrackerError::kInvalidInput;
    result->reset();
    if (closed_ || (last_watermark_ns_ && watermark_ns <= *last_watermark_ns_)) {
        return NpmTlsTrackerError::kNone;
    }
    last_watermark_ns_ = watermark_ns;
    if (!candidate_ || !candidate_->deadline_ns || watermark_ns < *candidate_->deadline_ns) {
        return NpmTlsTrackerError::kNone;
    }
    if (candidate_->final_server_hello) {
        Close(Outcome::kServerHelloObserved, Reason::kDeadlineAfterServerHello, watermark_ns, result);
    } else if (candidate_->phase == NpmTlsHandshakePhase::kAwaitingRetryClientHello ||
               candidate_->phase == NpmTlsHandshakePhase::kAwaitingFinalServerHello) {
        Fail(Reason::kHelloRetryRequestUnfinished, watermark_ns, result);
    } else if (!candidate_->first_client_hello) {
        Fail(Reason::kClientHelloIncomplete, watermark_ns, result);
    } else {
        Fail(Reason::kServerHelloNotObservedByDeadline, watermark_ns, result);
    }
    return NpmTlsTrackerError::kNone;
}

NpmTlsTrackerError NpmTlsHandshakeTracker::OnSessionEnd(NpmSessionEndReason, int64_t observed_at_ns,
                                                        std::optional<NpmTlsHandshakeResult>* result) {
    if (!result) return NpmTlsTrackerError::kInvalidInput;
    result->reset();
    if (closed_ || !candidate_) return NpmTlsTrackerError::kNone;
    if (candidate_->final_server_hello) {
        Close(Outcome::kServerHelloObserved, Reason::kSessionEndAfterServerHello, observed_at_ns, result);
    } else if (candidate_->phase == NpmTlsHandshakePhase::kAwaitingRetryClientHello ||
               candidate_->phase == NpmTlsHandshakePhase::kAwaitingFinalServerHello) {
        Fail(Reason::kHelloRetryRequestUnfinished, observed_at_ns, result);
    } else if (!candidate_->first_client_hello) {
        Fail(Reason::kClientHelloIncomplete, observed_at_ns, result);
    } else {
        Fail(Reason::kSessionEnd, observed_at_ns, result);
    }
    return NpmTlsTrackerError::kNone;
}

void NpmTlsHandshakeTracker::Abort() noexcept {
    candidate_.reset();
    closed_ = true;
}

const char* NpmTlsOutcomeName(NpmTlsOutcome outcome) noexcept {
    switch (outcome) {
        case Outcome::kServerHelloObserved:
            return "server_hello_observed";
        case Outcome::kFatalAlertObserved:
            return "fatal_alert_observed";
        case Outcome::kIncomplete:
            return "incomplete";
    }
    return "incomplete";
}

const char* NpmTlsIncompleteReasonName(NpmTlsIncompleteReason reason) noexcept {
    switch (reason) {
        case Reason::kNone:
            return "";
        case Reason::kEncryptedAfterServerHello:
            return "encrypted_after_server_hello";
        case Reason::kEncryptedAfterChangeCipherSpec:
            return "encrypted_after_change_cipher_spec";
        case Reason::kDeadlineAfterServerHello:
            return "deadline_after_server_hello";
        case Reason::kSessionEndAfterServerHello:
            return "session_end_after_server_hello";
        case Reason::kClientHelloIncomplete:
            return "client_hello_incomplete";
        case Reason::kServerHelloNotObservedByDeadline:
            return "server_hello_not_observed_by_deadline";
        case Reason::kHelloRetryRequestUnfinished:
            return "hello_retry_request_unfinished";
        case Reason::kClientPathGap:
            return "client_path_gap";
        case Reason::kServerPathGap:
            return "server_path_gap";
        case Reason::kCaptureTruncation:
            return "capture_truncation";
        case Reason::kMalformedRecord:
            return "malformed_record";
        case Reason::kMalformedHello:
            return "malformed_hello";
        case Reason::kHelloLimitExceeded:
            return "hello_limit_exceeded";
        case Reason::kUnsupportedVersion:
            return "unsupported_version";
        case Reason::kUnsupportedFraming:
            return "unsupported_framing";
        case Reason::kServerStreamEnd:
            return "server_stream_end";
        case Reason::kSessionEnd:
            return "session_end";
    }
    return "unsupported_framing";
}

}  // namespace flowsql::npm
