// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_tcp_stream_direction.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>

namespace flowsql::npm {
namespace {
int64_t Deadline(int64_t timestamp, int64_t timeout) {
    return timestamp > std::numeric_limits<int64_t>::max() - timeout ? std::numeric_limits<int64_t>::max()
                                                                     : timestamp + timeout;
}
}  // namespace

NpmTcpStreamDirection::NpmTcpStreamDirection(NpmTcpStreamConfigV1 config, std::shared_ptr<INpmTaskBudget> budget)
    : config_(config), budget_owner_(std::move(budget)), budget_(budget_owner_.get()) {
    Initialize(true);
}

NpmTcpStreamDirection::NpmTcpStreamDirection(NpmTcpStreamConfigV1 config, INpmTaskBudget& budget)
    : config_(config), budget_(&budget) {
    Initialize(false);
}

void NpmTcpStreamDirection::Initialize(bool charge_object) {
    if (!budget_ || config_.max_buffered_bytes_per_direction < kNpmMinTcpStreamBufferedBytesPerDirection ||
        config_.max_buffered_bytes_per_direction > kNpmMaxTcpStreamBufferedBytesPerDirection ||
        config_.gap_timeout_ns < kNpmMinTcpStreamGapTimeoutNs ||
        config_.gap_timeout_ns > kNpmMaxTcpStreamGapTimeoutNs) {
        error_ = NpmTcpStreamError::kInvalidConfig;
        return;
    }
    try {
        if (charge_object) Reserve(sizeof(*this));
    } catch (NpmTcpStreamError error) {
        error_ = error;
    }
}

NpmTcpStreamDirection::~NpmTcpStreamDirection() { Clear(); }

void NpmTcpStreamDirection::Reserve(uint64_t bytes) {
    if (bytes > config_.max_buffered_bytes_per_direction - tracked_bytes_) {
        throw NpmTcpStreamError::kDirectionLimitExceeded;
    }
    if (budget_->Reserve(NpmBudgetCategory::kModuleState, bytes) != NpmBudgetError::kNone) {
        throw NpmTcpStreamError::kTaskBudgetExceeded;
    }
    tracked_bytes_ += bytes;
}

void NpmTcpStreamDirection::Release(uint64_t bytes) noexcept {
    if (bytes == 0) return;
    budget_->Release(NpmBudgetCategory::kModuleState, bytes);
    tracked_bytes_ -= bytes;
}

void NpmTcpStreamDirection::Clear() noexcept {
    ranges_.clear();
    Release(tracked_bytes_);
}

NpmTcpStreamError NpmTcpStreamDirection::Fail(NpmTcpStreamError error) noexcept {
    error_ = error;
    Clear();
    return error_;
}

void NpmTcpStreamDirection::Abort() noexcept {
    if (error_ == NpmTcpStreamError::kNone && !ended_) error_ = NpmTcpStreamError::kAborted;
    Clear();
}

NpmTcpStreamDirection::Iterator NpmTcpStreamDirection::AddGap(Iterator before, uint64_t begin, uint64_t end,
                                                              int64_t deadline, bool truncated) {
    Reserve(kRangeCharge);
    // Any allocation failure is caught at the operation boundary, which releases the entire failed state.
    return ranges_.insert(before, Range{begin, end, deadline, truncated, 0, {}});
}

NpmTcpStreamDirection::Iterator NpmTcpStreamDirection::SplitGap(Iterator at, uint64_t position) {
    if (position == at->begin) return at;
    auto right = AddGap(std::next(at), position, at->end, at->timestamp_ns, at->truncated);
    at->end = position;
    return right;
}

NpmTcpStreamDirection::Iterator NpmTcpStreamDirection::Erase(Iterator at) noexcept {
    const uint64_t charge = kRangeCharge + at->allocated_bytes;
    auto next = ranges_.erase(at);
    Release(charge);
    return next;
}

void NpmTcpStreamDirection::Fill(uint64_t begin, uint64_t end, const uint8_t* bytes, int64_t captured_at) {
    for (auto at = ranges_.begin(); at != ranges_.end() && at->begin < end;) {
        if (at->end <= begin || at->data) {
            ++at;
            continue;
        }
        const auto left = std::max(begin, at->begin);
        const auto right = std::min(end, at->end);
        at = SplitGap(at, left);
        if (right < at->end) SplitGap(at, right);
        const uint64_t size = right - left;
        Reserve(size);
        auto data = std::make_unique<uint8_t[]>(size);
        std::memcpy(data.get(), bytes + (left - begin), size);
        at->data = std::move(data);
        at->allocated_bytes = size;
        at->timestamp_ns = captured_at;
        at->truncated = false;
        ++at;
    }
}

void NpmTcpStreamDirection::MarkTruncated(uint64_t begin, uint64_t end) {
    for (auto at = ranges_.begin(); at != ranges_.end() && at->begin < end;) {
        if (at->end <= begin || at->data || at->truncated) {
            ++at;
            continue;
        }
        at = SplitGap(at, std::max(begin, at->begin));
        if (end < at->end) SplitGap(at, end);
        at->truncated = true;
        ++at;
    }
}

void NpmTcpStreamDirection::TrimAtFin(uint64_t position) {
    for (auto at = ranges_.begin(); at != ranges_.end();) {
        if (at->begin >= position) {
            at = Erase(at);
        } else {
            if (at->end > position) at->end = position;
            ++at;
        }
    }
    extent_ = std::min(extent_, position);
}

bool NpmTcpStreamDirection::MapSequence(uint32_t sequence, uint64_t* begin, uint64_t* prefix) const {
    const uint32_t reference = base_sequence_ + static_cast<uint32_t>(progress_);
    const uint32_t delta = sequence - reference;
    if (delta == 0x80000000U) return false;
    *prefix = 0;
    if (delta < 0x80000000U) {
        if (progress_ > std::numeric_limits<uint64_t>::max() - delta) return false;
        *begin = progress_ + delta;
    } else {
        const uint64_t backwards = uint32_t{0} - delta;
        if (backwards > progress_) {
            *prefix = backwards - progress_;
            *begin = 0;
        } else {
            *begin = progress_ - backwards;
        }
    }
    return true;
}

void NpmTcpStreamDirection::Emit(const NpmTcpStreamEventV1& event, INpmTcpStreamEventSink& sink) {
    if (sink.Emit(event) != 0) throw NpmTcpStreamError::kSinkError;
    published_ = event.end;
}

void NpmTcpStreamDirection::Drain(bool forced, INpmTcpStreamEventSink& sink) {
    while (!ranges_.empty()) {
        auto at = ranges_.begin();
        NpmTcpStreamEventV1 event;
        event.begin = at->begin;
        event.end = at->end;
        if (at->data) {
            event.kind = NpmTcpStreamEventKindV1::kData;
            event.bytes = {at->data.get(), static_cast<size_t>(at->end - at->begin)};
            event.captured_at_ns = at->timestamp_ns;
        } else {
            if (!forced && (!watermark_ || *watermark_ < at->timestamp_ns)) break;
            event.kind = NpmTcpStreamEventKindV1::kGap;
            event.gap_reason = forced ? NpmTcpStreamGapReasonV1::kTermination : NpmTcpStreamGapReasonV1::kWaitExpired;
            event.includes_capture_truncation = at->truncated;
        }
        if (at->data) {
            if (sink.EmitOwned(event, at->data, at->allocated_bytes) != 0) throw NpmTcpStreamError::kSinkError;
            if (!at->data) {
                // The sink owns the payload and its existing ledger charge; only local bookkeeping moves.
                tracked_bytes_ -= at->allocated_bytes;
                at->allocated_bytes = 0;
            }
            published_ = event.end;
        } else {
            Emit(event, sink);
        }
        Erase(at);
    }
}

void NpmTcpStreamDirection::Finish(NpmTcpStreamEndReasonV1 reason, std::optional<NpmSessionEndReason> session_reason,
                                   INpmTcpStreamEventSink& sink) {
    Drain(true, sink);
    NpmTcpStreamEventV1 event;
    event.kind = NpmTcpStreamEventKindV1::kEnd;
    event.begin = event.end = published_;
    event.end_reason = reason;
    event.session_end_reason = session_reason;
    Emit(event, sink);
    ended_ = true;
}

void NpmTcpStreamDirection::Ingest(const NpmPacketView& packet, INpmTcpStreamEventSink& sink) {
    const auto& facts = packet.transport;
    const auto& tcp = facts.tcp;
    if (!tcp.valid || packet.payload.size != facts.payload_captured_bytes ||
        facts.payload_captured_bytes > facts.payload_wire_bytes ||
        facts.payload_complete != (facts.payload_captured_bytes == facts.payload_wire_bytes) ||
        (packet.payload.size != 0 && packet.payload.data == nullptr)) {
        throw NpmTcpStreamError::kInvalidInput;
    }
    truncated_ = truncated_ || !facts.payload_complete;
    if (!tcp.syn && !tcp.fin && facts.payload_wire_bytes == 0) {
        if (tcp.rst) Finish(NpmTcpStreamEndReasonV1::kReset, {}, sink);
        return;
    }
    const uint32_t sequence = tcp.sequence + static_cast<uint32_t>(tcp.syn);
    if (origin_ == NpmTcpStreamOriginV1::kUnknown) {
        base_sequence_ = sequence;
        origin_ = tcp.syn ? NpmTcpStreamOriginV1::kSyn : NpmTcpStreamOriginV1::kMidstream;
    }
    uint64_t begin = 0, prefix = 0;
    const uint64_t wire = facts.payload_wire_bytes;
    if (wire + static_cast<unsigned>(tcp.syn) + static_cast<unsigned>(tcp.fin) >= 0x80000000ULL ||
        !MapSequence(sequence, &begin, &prefix) || begin > std::numeric_limits<uint64_t>::max() - wire) {
        Finish(NpmTcpStreamEndReasonV1::kSequenceAmbiguous, {}, sink);
        return;
    }
    if (prefix > wire) {
        if (tcp.rst) Finish(NpmTcpStreamEndReasonV1::kReset, {}, sink);
        return;  // Captured before the immutable origin; no negative offsets or invented prefix.
    }
    const uint64_t declared_end = begin + wire - prefix;
    const uint64_t bounded_end = fin_ ? std::min(declared_end, *fin_) : declared_end;
    if (bounded_end >= published_ && bounded_end - published_ >= 0x80000000ULL) {
        Finish(NpmTcpStreamEndReasonV1::kSequenceAmbiguous, {}, sink);
        return;
    }
    if (tcp.fin && ((fin_ && *fin_ != declared_end) || declared_end < published_)) {
        Finish(NpmTcpStreamEndReasonV1::kSequenceAmbiguous, {}, sink);
        return;
    }
    if (tcp.fin && !fin_) {
        fin_ = declared_end;
        TrimAtFin(*fin_);
    }
    const uint64_t end = fin_ ? std::min(declared_end, *fin_) : declared_end;
    if (end > extent_) {
        AddGap(ranges_.end(), extent_, end, Deadline(packet.packet.meta.timestamp_ns, config_.gap_timeout_ns), false);
        extent_ = end;
    }
    progress_ = std::max(progress_, end);
    const uint64_t captured = facts.payload_captured_bytes;
    const uint64_t captured_end = begin + (captured > prefix ? captured - prefix : 0);
    const uint64_t data_begin = std::max(begin, published_);
    const uint64_t data_end = std::min(captured_end, end);
    if (data_begin < data_end) {
        Fill(data_begin, data_end, packet.payload.data + prefix + (data_begin - begin),
             packet.packet.meta.timestamp_ns);
    }
    if (!facts.payload_complete) MarkTruncated(std::max(captured_end, published_), end);
    if (tcp.rst) {
        Finish(NpmTcpStreamEndReasonV1::kReset, {}, sink);
    } else {
        Drain(false, sink);
        if (fin_ && published_ == *fin_) Finish(NpmTcpStreamEndReasonV1::kFin, {}, sink);
    }
}

NpmTcpStreamError NpmTcpStreamDirection::Push(const NpmPacketView& packet, INpmTcpStreamEventSink& sink) {
    if (error_ != NpmTcpStreamError::kNone || ended_) return error_;
    try {
        Ingest(packet, sink);
    } catch (NpmTcpStreamError error) {
        return Fail(error);
    } catch (const std::bad_alloc&) {
        return Fail(NpmTcpStreamError::kAllocationFailed);
    }
    return error_;
}

NpmTcpStreamError NpmTcpStreamDirection::AdvanceWatermark(int64_t watermark_ns, INpmTcpStreamEventSink& sink) {
    if (error_ != NpmTcpStreamError::kNone || ended_) return error_;
    if (watermark_ && watermark_ns <= *watermark_) return error_;
    watermark_ = watermark_ns;
    try {
        Drain(false, sink);
        if (fin_ && published_ == *fin_) Finish(NpmTcpStreamEndReasonV1::kFin, {}, sink);
    } catch (NpmTcpStreamError error) {
        return Fail(error);
    } catch (const std::bad_alloc&) {
        return Fail(NpmTcpStreamError::kAllocationFailed);
    }
    return error_;
}

NpmTcpStreamError NpmTcpStreamDirection::End(NpmTcpStreamEndReasonV1 reason,
                                             std::optional<NpmSessionEndReason> session_reason,
                                             INpmTcpStreamEventSink& sink) {
    if (error_ != NpmTcpStreamError::kNone || ended_) return error_;
    if (reason > NpmTcpStreamEndReasonV1::kSequenceAmbiguous ||
        (reason == NpmTcpStreamEndReasonV1::kSessionEnd) != session_reason.has_value() ||
        (session_reason && *session_reason > NpmSessionEndReason::kEof) || reason == NpmTcpStreamEndReasonV1::kFin) {
        return Fail(NpmTcpStreamError::kInvalidInput);
    }
    try {
        Finish(reason, session_reason, sink);
    } catch (NpmTcpStreamError error) {
        return Fail(error);
    } catch (const std::bad_alloc&) {
        return Fail(NpmTcpStreamError::kAllocationFailed);
    }
    return error_;
}

std::optional<int64_t> NpmTcpStreamDirection::NextEventDeadlineNs() const {
    // Only the front gap can advance the ordered stream; later deadlines cannot make it readable.
    if (ended_ || error_ != NpmTcpStreamError::kNone || ranges_.empty()) return {};
    return ranges_.front().timestamp_ns;
}

}  // namespace flowsql::npm
