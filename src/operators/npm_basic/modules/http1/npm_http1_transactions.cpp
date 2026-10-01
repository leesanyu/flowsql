// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_http1_transactions.h"

#include <cerrno>
#include <limits>
#include <new>
#include <utility>

namespace flowsql::npm {
namespace {

using Reason = NpmHttp1IncompleteReasonV1;

size_t DirectionIndex(NpmPacketDirection direction) { return direction == NpmPacketDirection::kAToB ? 0 : 1; }

NpmPacketDirection Opposite(NpmPacketDirection direction) {
    return direction == NpmPacketDirection::kAToB ? NpmPacketDirection::kBToA : NpmPacketDirection::kAToB;
}

bool IsFinal(uint16_t status) { return status == 101 || status >= 200; }

std::optional<int64_t> Deadline(std::optional<int64_t> request_at_ns, int64_t timeout_ns) {
    if (!request_at_ns) return std::nullopt;
    if (*request_at_ns > std::numeric_limits<int64_t>::max() - timeout_ns) return std::numeric_limits<int64_t>::max();
    return *request_at_ns + timeout_ns;
}

std::optional<int64_t> Latency(std::optional<int64_t> request_at_ns, std::optional<int64_t> response_at_ns) {
    if (!request_at_ns || !response_at_ns || *response_at_ns < *request_at_ns) return std::nullopt;
    const uint64_t delta = static_cast<uint64_t>(*response_at_ns) - static_cast<uint64_t>(*request_at_ns);
    if (delta > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) return std::nullopt;
    return static_cast<int64_t>(delta);
}

Reason FramingReason(NpmHttp1FramerErrorV1 error, bool response) {
    if (error == NpmHttp1FramerErrorV1::kHeaderLimit) return Reason::kHeaderLimitExceeded;
    if (error == NpmHttp1FramerErrorV1::kUnsupported) return Reason::kFramingUnsupported;
    return response ? Reason::kMalformedResponse : Reason::kFramingUnsupported;
}

}  // namespace

const char* NpmHttp1OutcomeNameV1(NpmHttp1OutcomeV1 outcome) noexcept {
    switch (outcome) {
        case NpmHttp1OutcomeV1::kMatched:
            return "matched";
        case NpmHttp1OutcomeV1::kRequestOnly:
            return "request_only";
        case NpmHttp1OutcomeV1::kResponseOnly:
            return "response_only";
    }
    return nullptr;
}

const char* NpmHttp1IncompleteReasonNameV1(NpmHttp1IncompleteReasonV1 reason) noexcept {
    switch (reason) {
        case Reason::kNone:
            return nullptr;
        case Reason::kNoPendingRequest:
            return "no_pending_request";
        case Reason::kResponseNotObservedByDeadline:
            return "response_not_observed_by_deadline";
        case Reason::kPipelineAlignmentLost:
            return "pipeline_alignment_lost";
        case Reason::kRequestPathGap:
            return "request_path_gap";
        case Reason::kResponsePathGap:
            return "response_path_gap";
        case Reason::kCaptureTruncation:
            return "capture_truncation";
        case Reason::kFramingUnsupported:
            return "framing_unsupported";
        case Reason::kHeaderLimitExceeded:
            return "header_limit_exceeded";
        case Reason::kMalformedResponse:
            return "malformed_response";
        case Reason::kResponseStreamEnd:
            return "response_stream_end";
        case Reason::kSessionEnd:
            return "session_end";
    }
    return nullptr;
}

uint64_t NpmHttp1TransactionTrackerV1::NextId() noexcept {
    if (next_id_ == 0) return 0;
    return next_id_++;
}

bool NpmHttp1TransactionTrackerV1::Reserve(uint64_t bytes) {
    return !budget_ || budget_->Reserve(NpmBudgetCategory::kModuleState, bytes) == NpmBudgetError::kNone;
}

void NpmHttp1TransactionTrackerV1::Release(uint64_t bytes) noexcept {
    if (budget_ && bytes) budget_->Release(NpmBudgetCategory::kModuleState, bytes);
}

uint64_t NpmHttp1TransactionTrackerV1::SessionCharge() const noexcept { return 4096 + 4ULL * config_.max_header_bytes; }

uint64_t NpmHttp1TransactionTrackerV1::PendingCharge() const noexcept { return 1024 + 2ULL * config_.max_header_bytes; }

uint64_t NpmHttp1TransactionTrackerV1::EventCharge(size_t bytes) const noexcept {
    // A minimum HTTP start line consumes far more than eight bytes; this also covers
    // the temporary vector, copied heads and result rows produced by one event.
    if (bytes > (std::numeric_limits<uint64_t>::max() - 4096) / 32) return 0;
    return 4096 + 32ULL * bytes;
}

NpmHttp1ResponseContextV1 NpmHttp1TransactionTrackerV1::ResponseContext(
    const Session& session, const std::vector<NpmHttp1FramedMessageV1>& messages) const {
    size_t final_headers_in_batch = 0;
    for (const auto& message : messages) {
        if (message.headers_complete && message.head.is_response && message.head.status_code &&
            IsFinal(*message.head.status_code)) {
            ++final_headers_in_batch;
        }
    }
    for (const auto& pending : session.pending) {
        if (pending.response) continue;
        if (final_headers_in_batch != 0) {
            --final_headers_in_batch;
            continue;
        }
        const std::string& method = pending.request.method.value();
        return {method == "HEAD", method == "CONNECT"};
    }
    return {};
}

void NpmHttp1TransactionTrackerV1::Seal(uint64_t session_id, Session* session, Reason first_reason, Reason later_reason,
                                        int64_t observed_at_ns, std::vector<NpmHttp1TransactionResultV1>* results) {
    if (session->sealed) return;
    bool first = true;
    while (!session->pending.empty()) {
        Pending pending = std::move(session->pending.front());
        session->pending.pop_front();
        NpmHttp1TransactionResultV1 row;
        row.entity_instance_id = pending.entity_instance_id;
        row.session_id = session_id;
        row.outcome = NpmHttp1OutcomeV1::kRequestOnly;
        row.incomplete_reason = first ? first_reason : later_reason;
        row.observed_at_ns = observed_at_ns;
        row.request_direction = session->request_direction;
        row.request = std::move(pending.request);
        row.informational_count = pending.informational_count;
        results->push_back(std::move(row));
        session->reserved_bytes -= PendingCharge();
        result_charges_ += PendingCharge();
        first = false;
    }
    if (session->orphan) {
        session->orphan.reset();
        Release(PendingCharge());
        session->reserved_bytes -= PendingCharge();
    }
    session->sealed = true;
}

int NpmHttp1TransactionTrackerV1::EmitReady(uint64_t session_id, Session* session, int64_t observed_at_ns,
                                            std::vector<NpmHttp1TransactionResultV1>* results) {
    while (!session->pending.empty()) {
        Pending& pending = session->pending.front();
        if (!pending.request_body_complete || !pending.response || !pending.response_body_complete) break;
        NpmHttp1TransactionResultV1 row;
        row.entity_instance_id = pending.entity_instance_id;
        row.session_id = session_id;
        row.outcome = NpmHttp1OutcomeV1::kMatched;
        row.observed_at_ns = pending.response->complete_at_ns.value_or(observed_at_ns);
        row.request_direction = session->request_direction;
        row.response_direction = Opposite(*session->request_direction);
        row.request = std::move(pending.request);
        row.response = std::move(pending.response);
        row.informational_count = pending.informational_count;
        row.latency_ns = Latency(row.request->complete_at_ns, row.response->complete_at_ns);
        results->push_back(std::move(row));
        session->pending.pop_front();
        session->reserved_bytes -= PendingCharge();
        result_charges_ += PendingCharge();
    }
    if (session->orphan && session->orphan->body_complete) {
        NpmHttp1TransactionResultV1 row;
        row.entity_instance_id = session->orphan->entity_instance_id;
        row.session_id = session_id;
        row.outcome = NpmHttp1OutcomeV1::kResponseOnly;
        row.incomplete_reason = Reason::kNoPendingRequest;
        row.observed_at_ns = session->orphan->response.complete_at_ns.value_or(observed_at_ns);
        row.response_direction = Opposite(*session->request_direction);
        row.response = std::move(session->orphan->response);
        results->push_back(std::move(row));
        session->orphan.reset();
        session->reserved_bytes -= PendingCharge();
        result_charges_ += PendingCharge();
    }
    return 0;
}

int NpmHttp1TransactionTrackerV1::ProcessMessages(uint64_t session_id, Session* session, NpmPacketDirection direction,
                                                  const std::vector<NpmHttp1FramedMessageV1>& messages,
                                                  int64_t observed_at_ns,
                                                  std::vector<NpmHttp1TransactionResultV1>* results) {
    for (const auto& message : messages) {
        if (session->sealed) break;
        if (message.headers_complete) {
            if (!session->request_direction) {
                session->request_direction = message.head.is_response ? Opposite(direction) : direction;
            }
            const bool response_direction = direction != *session->request_direction;
            if (response_direction != message.head.is_response) {
                const Reason reason = response_direction ? Reason::kMalformedResponse : Reason::kFramingUnsupported;
                Seal(session_id, session, reason, reason, observed_at_ns, results);
                break;
            }
            if (!response_direction) {
                if (session->pending.size() >= config_.max_pending_per_session) return ENOSPC;
                if (!Reserve(PendingCharge())) return ENOSPC;
                Pending pending;
                pending.entity_instance_id = NextId();
                if (pending.entity_instance_id == 0) {
                    Release(PendingCharge());
                    return EOVERFLOW;
                }
                try {
                    pending.request_message_id = message.message_id;
                    pending.request = message.head;
                    pending.deadline_ns = Deadline(message.head.complete_at_ns, config_.response_timeout_ns);
                    session->pending.push_back(std::move(pending));
                } catch (...) {
                    Release(PendingCharge());
                    throw;
                }
                session->reserved_bytes += PendingCharge();
            } else {
                const uint16_t status = message.head.status_code.value();
                if (!IsFinal(status)) {
                    if (!session->pending.empty()) {
                        if (session->pending.front().informational_count == std::numeric_limits<uint32_t>::max())
                            return EOVERFLOW;
                        ++session->pending.front().informational_count;
                    }
                    continue;
                }
                Pending* match = nullptr;
                for (auto& pending : session->pending) {
                    if (!pending.response) {
                        match = &pending;
                        break;
                    }
                }
                const bool tunnel =
                    status == 101 || (match && match->request.method == "CONNECT" && status >= 200 && status < 300);
                if (match) {
                    match->response_message_id = message.message_id;
                    match->response = message.head;
                    match->response_body_complete = message.head.framing == NpmHttp1FramingV1::kCloseDelimited;
                } else {
                    if (session->orphan) return EINVAL;
                    if (!Reserve(PendingCharge())) return ENOSPC;
                    Orphan orphan;
                    orphan.entity_instance_id = NextId();
                    if (orphan.entity_instance_id == 0) {
                        Release(PendingCharge());
                        return EOVERFLOW;
                    }
                    try {
                        orphan.message_id = message.message_id;
                        orphan.response = message.head;
                        orphan.body_complete = message.head.framing == NpmHttp1FramingV1::kCloseDelimited;
                        session->orphan = std::move(orphan);
                    } catch (...) {
                        Release(PendingCharge());
                        throw;
                    }
                    session->reserved_bytes += PendingCharge();
                }
                EmitReady(session_id, session, observed_at_ns, results);
                if (tunnel) session->seal_after_tunnel = true;
            }
        } else if (message.body_complete) {
            if (!session->request_direction) return EINVAL;
            const bool response_direction = direction != *session->request_direction;
            if (!response_direction) {
                for (auto& pending : session->pending) {
                    if (pending.request_message_id == message.message_id) {
                        pending.request_body_complete = true;
                        break;
                    }
                }
            } else if (session->orphan && session->orphan->message_id == message.message_id) {
                session->orphan->body_complete = true;
            } else {
                for (auto& pending : session->pending) {
                    if (pending.response_message_id == message.message_id) {
                        pending.response_body_complete = true;
                        break;
                    }
                }
            }
            EmitReady(session_id, session, observed_at_ns, results);
            if (session->seal_after_tunnel && response_direction) {
                Seal(session_id, session, Reason::kPipelineAlignmentLost, Reason::kPipelineAlignmentLost,
                     observed_at_ns, results);
            }
        }
    }
    return 0;
}

int NpmHttp1TransactionTrackerV1::OnStreamEvent(uint64_t session_id, NpmPacketDirection direction,
                                                NpmTcpStreamOriginV1 origin, const NpmTcpStreamEventV1& event,
                                                int64_t observed_at_ns,
                                                std::vector<NpmHttp1TransactionResultV1>* results) {
    if (aborted_ || !results || session_id == 0 ||
        (direction != NpmPacketDirection::kAToB && direction != NpmPacketDirection::kBToA))
        return EINVAL;
    try {
        const uint64_t event_charge = EventCharge(event.kind == NpmTcpStreamEventKindV1::kData ? event.bytes.size : 0);
        if (!event_charge || !Reserve(event_charge)) return ENOSPC;
        struct EventRelease {
            const std::shared_ptr<INpmTaskBudget>& budget;
            uint64_t bytes;
            ~EventRelease() {
                if (budget) budget->Release(NpmBudgetCategory::kModuleState, bytes);
            }
        } event_release{budget_, event_charge};
        auto found = sessions_.find(session_id);
        if (found == sessions_.end()) {
            if (!Reserve(SessionCharge())) return ENOSPC;
            try {
                found = sessions_.try_emplace(session_id, config_.max_header_bytes).first;
            } catch (...) {
                Release(SessionCharge());
                throw;
            }
            found->second.reserved_bytes = SessionCharge();
        }
        Session& session = found->second;
        if (session.sealed) return 0;
        std::vector<NpmHttp1FramedMessageV1> messages;
        auto response_context = [&] { return ResponseContext(session, messages); };
        const auto error =
            session.framers[DirectionIndex(direction)].Consume(event, origin, &messages, response_context);
        const int processed = ProcessMessages(session_id, &session, direction, messages, observed_at_ns, results);
        if (processed) return processed;
        if (error == NpmHttp1FramerErrorV1::kAllocationFailed) return ENOMEM;
        if (error == NpmHttp1FramerErrorV1::kInvalidOutput) return EINVAL;
        if (session.sealed) return 0;
        if (event.kind == NpmTcpStreamEventKindV1::kGap) {
            const bool response = session.request_direction && direction != *session.request_direction;
            const Reason reason = event.includes_capture_truncation ? Reason::kCaptureTruncation
                                  : response                        ? Reason::kResponsePathGap
                                                                    : Reason::kRequestPathGap;
            Seal(session_id, &session, reason, reason, observed_at_ns, results);
        } else if (error != NpmHttp1FramerErrorV1::kNone) {
            const bool response = session.request_direction && direction != *session.request_direction;
            const Reason reason =
                event.kind == NpmTcpStreamEventKindV1::kEnd && error == NpmHttp1FramerErrorV1::kIncomplete && response
                    ? Reason::kResponseStreamEnd
                    : FramingReason(error, response);
            Seal(session_id, &session, reason, reason, observed_at_ns, results);
        } else if (event.kind == NpmTcpStreamEventKindV1::kEnd) {
            const bool response = session.request_direction && direction != *session.request_direction;
            if (event.end_reason != NpmTcpStreamEndReasonV1::kFin) {
                Seal(session_id, &session, Reason::kSessionEnd, Reason::kSessionEnd, observed_at_ns, results);
            } else if (response) {
                Seal(session_id, &session, Reason::kResponseStreamEnd, Reason::kResponseStreamEnd, observed_at_ns,
                     results);
            }
        }
        return 0;
    } catch (const std::bad_alloc&) {
        return ENOMEM;
    }
}

std::optional<int64_t> NpmHttp1TransactionTrackerV1::NextEventDeadlineNs() const noexcept {
    std::optional<int64_t> next;
    for (const auto& entry : sessions_) {
        const Session& session = entry.second;
        if (session.sealed || session.pending.empty()) continue;
        const Pending& pending = session.pending.front();
        if (pending.response || !pending.deadline_ns) continue;
        if (!next || *pending.deadline_ns < *next) next = pending.deadline_ns;
    }
    return next;
}

int NpmHttp1TransactionTrackerV1::OnCaptureWatermark(std::optional<int64_t> watermark_ns,
                                                     std::vector<NpmHttp1TransactionResultV1>* results) {
    if (aborted_ || !results) return EINVAL;
    if (!watermark_ns || (watermark_ns_ && *watermark_ns <= *watermark_ns_)) return 0;
    watermark_ns_ = watermark_ns;
    try {
        for (auto& entry : sessions_) {
            Session& session = entry.second;
            if (session.sealed || session.pending.empty()) continue;
            const Pending& pending = session.pending.front();
            if (!pending.response && pending.deadline_ns && *pending.deadline_ns <= *watermark_ns) {
                Seal(entry.first, &session, Reason::kResponseNotObservedByDeadline, Reason::kPipelineAlignmentLost,
                     *watermark_ns, results);
            }
        }
        return 0;
    } catch (const std::bad_alloc&) {
        return ENOMEM;
    }
}

int NpmHttp1TransactionTrackerV1::OnSessionEnd(uint64_t session_id, NpmSessionEndReason, int64_t observed_at_ns,
                                               std::vector<NpmHttp1TransactionResultV1>* results) {
    if (aborted_ || !results || session_id == 0) return EINVAL;
    const auto found = sessions_.find(session_id);
    if (found == sessions_.end()) return 0;
    try {
        Seal(session_id, &found->second, Reason::kSessionEnd, Reason::kSessionEnd, observed_at_ns, results);
        Release(found->second.reserved_bytes);
        sessions_.erase(found);
        return 0;
    } catch (const std::bad_alloc&) {
        return ENOMEM;
    }
}

void NpmHttp1TransactionTrackerV1::Abort() noexcept {
    aborted_ = true;
    for (const auto& entry : sessions_) Release(entry.second.reserved_bytes);
    sessions_.clear();
    Release(result_charges_);
    result_charges_ = 0;
}

void NpmHttp1TransactionTrackerV1::ReleaseResults(std::vector<NpmHttp1TransactionResultV1>* results) noexcept {
    if (results) results->clear();
    Release(result_charges_);
    result_charges_ = 0;
}

}  // namespace flowsql::npm
