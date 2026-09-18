// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_tcp_performance_tracker.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <limits>
#include <list>
#include <new>
#include <unordered_map>
#include <utility>

namespace flowsql::npm {
namespace {

constexpr uint8_t kTcpProtocol = 6;
constexpr uint8_t kUdpProtocol = 17;

bool ValidDirection(NpmPacketDirection direction) noexcept {
    return direction == NpmPacketDirection::kAToB || direction == NpmPacketDirection::kBToA;
}

bool IsBareSyn(const NpmTcpPacketFacts& tcp) noexcept {
    return tcp.syn && !tcp.ack;
}

NpmTcpInitiator InitiatorFor(NpmPacketDirection direction) noexcept {
    return direction == NpmPacketDirection::kAToB ? NpmTcpInitiator::kA : NpmTcpInitiator::kB;
}

NpmTcpInitiator OppositeInitiatorFor(NpmPacketDirection direction) noexcept {
    return direction == NpmPacketDirection::kAToB ? NpmTcpInitiator::kB : NpmTcpInitiator::kA;
}

NpmPacketDirection OppositeDirection(NpmPacketDirection direction) noexcept {
    return direction == NpmPacketDirection::kAToB ? NpmPacketDirection::kBToA
                                                   : NpmPacketDirection::kAToB;
}

std::optional<int64_t> NonnegativeDelta(int64_t end_ns, int64_t start_ns) noexcept {
    if (end_ns < start_ns) return std::nullopt;
    const uint64_t delta = static_cast<uint64_t>(end_ns) - static_cast<uint64_t>(start_ns);
    if (delta > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) return std::nullopt;
    return static_cast<int64_t>(delta);
}

double BitsPerSecond(uint64_t bytes, int64_t duration_ns) noexcept {
    return static_cast<double>(bytes) * 8.0 * static_cast<double>(kNpmNanosecondsPerSecond) /
           static_cast<double>(duration_ns);
}

}  // namespace

struct NpmTcpPerformanceTracker::Impl {
    struct OutstandingSegment {
        int64_t sequence_begin = 0;
        int64_t sequence_end = 0;
        int64_t sent_at_ns = 0;
        bool retransmitted = false;
    };

    using SegmentList = std::list<OutstandingSegment>;

    struct DirectionState {
        bool initialized = false;
        int64_t progress = 0;
        std::optional<int64_t> highest_ack;
        SegmentList history;
        SegmentList outstanding;
        uint64_t unique_bytes = 0;
        uint64_t retrans_packets = 0;
        uint64_t retrans_bytes = 0;
    };

    struct SessionState {
        uint8_t transport_protocol = 0;
        int64_t min_timestamp_ns = 0;
        int64_t max_timestamp_ns = 0;
        uint64_t packets_ab = 0;
        uint64_t packets_ba = 0;
        uint64_t observed_wire_bytes_ab = 0;
        uint64_t observed_wire_bytes_ba = 0;
        uint64_t payload_bytes_ab = 0;
        uint64_t payload_bytes_ba = 0;
        NpmTcpHandshakeStatus handshake_status = NpmTcpHandshakeStatus::kNotObserved;
        std::optional<NpmTcpInitiator> initiator;
        std::optional<int64_t> handshake_duration_ns;
        std::optional<int64_t> synack_rtt_ns;
        bool initial_syn_observed = false;
        NpmPacketDirection initial_syn_direction = NpmPacketDirection::kAToB;
        uint32_t initial_syn_sequence = 0;
        int64_t initial_syn_at_ns = 0;
        bool synack_observed = false;
        uint32_t synack_sequence = 0;
        NpmTcpRttStatus rtt_status = NpmTcpRttStatus::kNoSample;
        uint64_t rtt_samples = 0;
        uint64_t rtt_sum_ns = 0;
        int64_t rtt_min_ns = 0;
        int64_t rtt_max_ns = 0;
        uint32_t measurement_flags = 0;
        bool timestamp_regressed = false;
        std::array<DirectionState, 2> directions;
        uint64_t tracked_bytes = 0;
    };

    using SessionMap = std::unordered_map<uint64_t, SessionState>;

    Impl(std::shared_ptr<INpmTaskBudget> task_budget, uint32_t range_limit)
        : budget(std::move(task_budget)), max_ranges_per_direction(range_limit) {}

    static uint64_t SessionCharge() noexcept {
        return sizeof(SessionMap::value_type) + 4 * sizeof(void*);
    }

    static uint64_t SegmentCharge() noexcept {
        return sizeof(OutstandingSegment) + 2 * sizeof(void*);
    }

    NpmTcpPerformanceError Reserve(uint64_t bytes) noexcept {
        if (!budget) return NpmTcpPerformanceError::kNullBudget;
        if (budget->Reserve(NpmBudgetCategory::kModuleState, bytes) != NpmBudgetError::kNone) {
            return NpmTcpPerformanceError::kBudgetExceeded;
        }
        tracked_bytes += bytes;
        return NpmTcpPerformanceError::kNone;
    }

    void Release(uint64_t bytes) noexcept {
        if (bytes == 0 || !budget) return;
        if (budget->Release(NpmBudgetCategory::kModuleState, bytes) == NpmBudgetError::kNone) {
            tracked_bytes -= bytes;
        }
    }

    DirectionState& Outgoing(SessionState* state, NpmPacketDirection direction) noexcept {
        return state->directions[direction == NpmPacketDirection::kAToB ? 0 : 1];
    }

    DirectionState& Acknowledged(SessionState* state, NpmPacketDirection direction) noexcept {
        return Outgoing(state, OppositeDirection(direction));
    }

    void ReleaseSegments(SessionState* state, size_t count) noexcept {
        if (count == 0) return;
        const uint64_t bytes = static_cast<uint64_t>(count) * SegmentCharge();
        Release(bytes);
        state->tracked_bytes -= bytes;
    }

    void ClearOutstanding(SessionState* state) noexcept {
        size_t count = 0;
        for (auto& direction : state->directions) {
            count += direction.outstanding.size();
            direction.outstanding.clear();
        }
        ReleaseSegments(state, count);
    }

    void MarkSequenceAmbiguous(SessionState* state) noexcept {
        state->measurement_flags |= kNpmMeasurementSequenceAmbiguous;
        MarkRttAmbiguous(state);
        size_t count = 0;
        for (auto& direction : state->directions) {
            count += direction.history.size();
            direction.history.clear();
        }
        ReleaseSegments(state, count);
    }

    static std::optional<int64_t> Unwrap(uint32_t sequence, int64_t reference) noexcept {
        constexpr uint32_t half_space = uint32_t{1} << 31;
        const uint32_t distance = sequence - static_cast<uint32_t>(reference);
        if (distance == half_space) return std::nullopt;
        const int64_t delta = distance < half_space ? static_cast<int64_t>(distance)
                                                   : static_cast<int64_t>(distance) - (int64_t{1} << 32);
        if ((delta > 0 && reference > std::numeric_limits<int64_t>::max() - delta) ||
            (delta < 0 && reference < std::numeric_limits<int64_t>::min() - delta)) {
            return std::nullopt;
        }
        return reference + delta;
    }

    void MarkHandshakeAmbiguous(SessionState* state) noexcept {
        state->handshake_status = NpmTcpHandshakeStatus::kAmbiguous;
        state->initiator.reset();
        state->handshake_duration_ns.reset();
        state->synack_rtt_ns.reset();
        state->synack_observed = false;
    }

    void MarkRttAmbiguous(SessionState* state) noexcept {
        state->rtt_status = NpmTcpRttStatus::kAmbiguous;
        state->rtt_samples = 0;
        state->rtt_sum_ns = 0;
        state->rtt_min_ns = 0;
        state->rtt_max_ns = 0;
        ClearOutstanding(state);
    }

    void MarkTimestampRegression(SessionState* state) noexcept {
        state->measurement_flags |= kNpmMeasurementTimestampRegression;
        state->timestamp_regressed = true;
        if (state->transport_protocol == kTcpProtocol) {
            MarkHandshakeAmbiguous(state);
            MarkRttAmbiguous(state);
        }
    }

    void InitializeHandshake(SessionState* state,
                             const NpmPacketView& packet,
                             int64_t timestamp_ns) noexcept {
        const auto& tcp = packet.transport.tcp;
        if (IsBareSyn(tcp)) {
            state->handshake_status = NpmTcpHandshakeStatus::kPartial;
            state->initiator = InitiatorFor(packet.direction);
            state->initial_syn_observed = true;
            state->initial_syn_direction = packet.direction;
            state->initial_syn_sequence = tcp.sequence;
            state->initial_syn_at_ns = timestamp_ns;
            return;
        }
        state->measurement_flags |= kNpmMeasurementMidstreamStart;
        if (tcp.syn && tcp.ack) {
            state->handshake_status = NpmTcpHandshakeStatus::kPartial;
            state->initiator = OppositeInitiatorFor(packet.direction);
            return;
        }
        state->handshake_status = NpmTcpHandshakeStatus::kNotObserved;
    }

    void ObserveHandshake(SessionState* state, const NpmPacketView& packet) noexcept {
        if (state->timestamp_regressed) return;
        const auto& tcp = packet.transport.tcp;
        const int64_t timestamp_ns = packet.packet.meta.timestamp_ns;
        if (state->initial_syn_observed && IsBareSyn(tcp)) {
            if (packet.direction == state->initial_syn_direction &&
                tcp.sequence == state->initial_syn_sequence) {
                state->measurement_flags |= kNpmMeasurementSynRetransmitted;
            }
            MarkHandshakeAmbiguous(state);
            return;
        }
        if (state->handshake_status == NpmTcpHandshakeStatus::kAmbiguous ||
            state->handshake_status == NpmTcpHandshakeStatus::kNotObserved ||
            !state->initial_syn_observed) {
            return;
        }
        if (!state->synack_observed) {
            if (packet.direction != OppositeDirection(state->initial_syn_direction) || !tcp.syn || !tcp.ack ||
                tcp.acknowledgment != state->initial_syn_sequence + 1) {
                return;
            }
            const auto delta = NonnegativeDelta(timestamp_ns, state->initial_syn_at_ns);
            if (!delta.has_value()) {
                MarkHandshakeAmbiguous(state);
                return;
            }
            state->synack_observed = true;
            state->synack_sequence = tcp.sequence;
            state->synack_rtt_ns = *delta;
            return;
        }
        if (state->handshake_status == NpmTcpHandshakeStatus::kComplete ||
            packet.direction != state->initial_syn_direction || tcp.syn || !tcp.ack ||
            tcp.acknowledgment != state->synack_sequence + 1) {
            return;
        }
        const auto delta = NonnegativeDelta(timestamp_ns, state->initial_syn_at_ns);
        if (!delta.has_value()) {
            MarkHandshakeAmbiguous(state);
            return;
        }
        state->handshake_status = NpmTcpHandshakeStatus::kComplete;
        state->handshake_duration_ns = *delta;
    }

    void AddRttSample(SessionState* state, int64_t sample_ns) noexcept {
        if (state->rtt_status == NpmTcpRttStatus::kAmbiguous) return;
        const uint64_t sample = static_cast<uint64_t>(sample_ns);
        if (state->rtt_samples == std::numeric_limits<uint64_t>::max() ||
            state->rtt_sum_ns > std::numeric_limits<uint64_t>::max() - sample) {
            MarkRttAmbiguous(state);
            return;
        }
        if (state->rtt_samples == 0) {
            state->rtt_min_ns = sample_ns;
            state->rtt_max_ns = sample_ns;
        } else {
            state->rtt_min_ns = std::min(state->rtt_min_ns, sample_ns);
            state->rtt_max_ns = std::max(state->rtt_max_ns, sample_ns);
        }
        ++state->rtt_samples;
        state->rtt_sum_ns += sample;
        state->rtt_status = NpmTcpRttStatus::kValid;
    }

    void ObserveAcknowledgment(SessionState* state,
                               DirectionState* direction,
                               int64_t acknowledgment,
                               int64_t timestamp_ns) noexcept {
        if (!direction->highest_ack.has_value() || acknowledgment > *direction->highest_ack) {
            direction->highest_ack = acknowledgment;
        } else {
            return;
        }
        if (state->rtt_status == NpmTcpRttStatus::kAmbiguous) return;
        std::optional<int64_t> latest_sent_at_ns;
        size_t removed = 0;
        for (auto iterator = direction->outstanding.begin(); iterator != direction->outstanding.end();) {
            if (acknowledgment < iterator->sequence_end) {
                ++iterator;
                continue;
            }
            if (!iterator->retransmitted &&
                (!latest_sent_at_ns.has_value() || iterator->sent_at_ns > *latest_sent_at_ns)) {
                latest_sent_at_ns = iterator->sent_at_ns;
            }
            iterator = direction->outstanding.erase(iterator);
            ++removed;
        }
        ReleaseSegments(state, removed);
        if (!latest_sent_at_ns.has_value()) return;
        const auto sample = NonnegativeDelta(timestamp_ns, *latest_sent_at_ns);
        if (!sample.has_value()) {
            MarkRttAmbiguous(state);
            return;
        }
        AddRttSample(state, *sample);
    }

    NpmTcpPerformanceError ObserveRanges(SessionState* state, const NpmPacketView& packet) noexcept {
        if ((state->measurement_flags & kNpmMeasurementSequenceAmbiguous) != 0) {
            return NpmTcpPerformanceError::kNone;
        }
        const auto& tcp = packet.transport.tcp;
        auto& outgoing = Outgoing(state, packet.direction);
        auto& acknowledged = Acknowledged(state, packet.direction);
        const auto sequence = outgoing.initialized
                                  ? Unwrap(tcp.sequence, outgoing.progress)
                                  : std::optional<int64_t>(tcp.sequence);
        std::optional<int64_t> ack;
        if (tcp.ack && acknowledged.initialized) ack = Unwrap(tcp.acknowledgment, acknowledged.progress);
        const int64_t sequence_length = static_cast<int64_t>(packet.transport.payload_wire_bytes) +
                                        static_cast<int64_t>(tcp.syn) + static_cast<int64_t>(tcp.fin);
        if (!sequence.has_value() || (tcp.ack && acknowledged.initialized && !ack.has_value()) ||
            sequence_length >= (int64_t{1} << 31) ||
            *sequence > std::numeric_limits<int64_t>::max() - sequence_length) {
            MarkSequenceAmbiguous(state);
            return NpmTcpPerformanceError::kNone;
        }
        const int64_t begin = *sequence + static_cast<int64_t>(tcp.syn);
        const int64_t end = begin + packet.transport.payload_wire_bytes;
        const bool has_payload = end > begin;
        uint64_t overlap = 0;
        size_t merged_ranges = 0;
        int64_t merged_begin = begin;
        int64_t merged_end = end;
        auto first = outgoing.history.end();
        if (has_payload) {
            for (auto iterator = outgoing.history.begin(); iterator != outgoing.history.end(); ++iterator) {
                if (iterator->sequence_end < begin) continue;
                if (iterator->sequence_begin > end) {
                    if (first == outgoing.history.end()) first = iterator;
                    break;
                }
                if (merged_ranges == 0) first = iterator;
                ++merged_ranges;
                merged_begin = std::min(merged_begin, iterator->sequence_begin);
                merged_end = std::max(merged_end, iterator->sequence_end);
                const int64_t intersection_begin = std::max(begin, iterator->sequence_begin);
                const int64_t intersection_end = std::min(end, iterator->sequence_end);
                if (intersection_end > intersection_begin) {
                    overlap += static_cast<uint64_t>(intersection_end - intersection_begin);
                }
            }
        }

        const auto duplicate = std::find_if(
            outgoing.outstanding.begin(), outgoing.outstanding.end(), [&](const OutstandingSegment& segment) {
                return segment.sequence_begin == begin && segment.sequence_end == end;
            });
        const bool regressed = state->timestamp_regressed ||
                               packet.packet.meta.timestamp_ns < state->max_timestamp_ns;
        const bool new_history = has_payload && merged_ranges == 0;
        const bool new_outstanding =
            has_payload && !regressed && state->rtt_status != NpmTcpRttStatus::kAmbiguous &&
            duplicate == outgoing.outstanding.end() &&
            (!outgoing.highest_ack.has_value() || end > *outgoing.highest_ack);
        const size_t history_count = outgoing.history.size() + static_cast<size_t>(new_history) -
                                     (merged_ranges > 0 ? merged_ranges - 1 : 0);
        const size_t total_ranges = history_count + outgoing.outstanding.size() +
                                    static_cast<size_t>(new_outstanding);
        if (total_ranges > max_ranges_per_direction) return NpmTcpPerformanceError::kRangeLimitExceeded;

        SegmentList staged_history;
        SegmentList staged_outstanding;
        uint64_t reserved_bytes = 0;
        auto stage = [&](SegmentList* segments, bool needed) {
            if (!needed) return NpmTcpPerformanceError::kNone;
            const auto error = Reserve(SegmentCharge());
            if (error != NpmTcpPerformanceError::kNone) return error;
            reserved_bytes += SegmentCharge();
            segments->push_back({begin, end, packet.packet.meta.timestamp_ns, overlap != 0});
            return NpmTcpPerformanceError::kNone;
        };
        try {
            auto error = stage(&staged_history, new_history);
            if (error == NpmTcpPerformanceError::kNone) error = stage(&staged_outstanding, new_outstanding);
            if (error != NpmTcpPerformanceError::kNone) {
                Release(reserved_bytes);
                return error;
            }
        } catch (const std::bad_alloc&) {
            Release(reserved_bytes);
            return NpmTcpPerformanceError::kAllocationFailed;
        }

        // All allocations and reservations precede mutation. Commit uses only erase/splice/scalars.
        state->tracked_bytes += reserved_bytes;
        if (new_history) {
            outgoing.history.splice(first, staged_history);
        } else if (merged_ranges != 0) {
            first->sequence_begin = merged_begin;
            first->sequence_end = merged_end;
            auto iterator = std::next(first);
            for (size_t index = 1; index < merged_ranges; ++index) iterator = outgoing.history.erase(iterator);
            ReleaseSegments(state, merged_ranges - 1);
        }
        if (overlap != 0) {
            ++outgoing.retrans_packets;
            outgoing.retrans_bytes += overlap;
            for (auto& segment : outgoing.outstanding) {
                if (segment.sequence_begin < end && begin < segment.sequence_end) segment.retransmitted = true;
            }
        }
        outgoing.unique_bytes += packet.transport.payload_wire_bytes - overlap;
        outgoing.outstanding.splice(outgoing.outstanding.end(), staged_outstanding);
        const int64_t progress = *sequence + sequence_length;
        outgoing.progress = outgoing.initialized ? std::max(outgoing.progress, progress) : progress;
        outgoing.initialized = true;
        if (ack.has_value() && !regressed) {
            ObserveAcknowledgment(state, &acknowledged, *ack, packet.packet.meta.timestamp_ns);
        }
        return NpmTcpPerformanceError::kNone;
    }

    static bool CounterCanAdd(uint64_t current, uint64_t value) noexcept {
        return value <= std::numeric_limits<uint64_t>::max() - current;
    }

    static bool CanCountPacket(const SessionState& state, const NpmPacketView& packet) noexcept {
        const bool ab = packet.direction == NpmPacketDirection::kAToB;
        return CounterCanAdd(ab ? state.packets_ab : state.packets_ba, 1) &&
               CounterCanAdd(ab ? state.observed_wire_bytes_ab : state.observed_wire_bytes_ba,
                             packet.packet.meta.wire_len) &&
               CounterCanAdd(ab ? state.payload_bytes_ab : state.payload_bytes_ba,
                             packet.transport.payload_wire_bytes);
    }

    static void CountPacket(SessionState* state, const NpmPacketView& packet) noexcept {
        if (packet.direction == NpmPacketDirection::kAToB) {
            ++state->packets_ab;
            state->observed_wire_bytes_ab += packet.packet.meta.wire_len;
            state->payload_bytes_ab += packet.transport.payload_wire_bytes;
        } else {
            ++state->packets_ba;
            state->observed_wire_bytes_ba += packet.packet.meta.wire_len;
            state->payload_bytes_ba += packet.transport.payload_wire_bytes;
        }
        if (!packet.transport.payload_complete) {
            state->measurement_flags |= kNpmMeasurementTruncatedPayload;
        }
    }

    NpmTcpPerformanceError ObserveNew(const NpmPacketView& packet,
                                      uint64_t session_id,
                                      uint8_t protocol) noexcept {
        const auto error = Reserve(SessionCharge());
        if (error != NpmTcpPerformanceError::kNone) return error;
        SessionState state;
        state.transport_protocol = protocol;
        state.min_timestamp_ns = packet.packet.meta.timestamp_ns;
        state.max_timestamp_ns = packet.packet.meta.timestamp_ns;
        state.tracked_bytes = SessionCharge();
        if (protocol == kTcpProtocol) {
            InitializeHandshake(&state, packet, packet.packet.meta.timestamp_ns);
            const auto ranges_error = ObserveRanges(&state, packet);
            if (ranges_error != NpmTcpPerformanceError::kNone) {
                Release(state.tracked_bytes);
                return ranges_error;
            }
        }
        CountPacket(&state, packet);
        const uint64_t bytes = state.tracked_bytes;
        try {
            sessions.emplace(session_id, std::move(state));
        } catch (const std::bad_alloc&) {
            Release(bytes);
            return NpmTcpPerformanceError::kAllocationFailed;
        }
        return NpmTcpPerformanceError::kNone;
    }

    NpmTcpPerformanceError ObserveExisting(SessionState* state, const NpmPacketView& packet) noexcept {
        if (!CanCountPacket(*state, packet)) return NpmTcpPerformanceError::kInvalidPacket;
        if (state->transport_protocol == kTcpProtocol) {
            const auto error = ObserveRanges(state, packet);
            if (error != NpmTcpPerformanceError::kNone) return error;
        }
        const int64_t timestamp_ns = packet.packet.meta.timestamp_ns;
        CountPacket(state, packet);
        state->min_timestamp_ns = std::min(state->min_timestamp_ns, timestamp_ns);
        if (timestamp_ns < state->max_timestamp_ns) {
            MarkTimestampRegression(state);
            return NpmTcpPerformanceError::kNone;
        }
        state->max_timestamp_ns = std::max(state->max_timestamp_ns, timestamp_ns);
        if (state->transport_protocol == kTcpProtocol) ObserveHandshake(state, packet);
        return NpmTcpPerformanceError::kNone;
    }

    std::shared_ptr<INpmTaskBudget> budget;
    SessionMap sessions;
    uint64_t tracked_bytes = 0;
    uint32_t max_ranges_per_direction = 0;
};

NpmTcpPerformanceTracker::NpmTcpPerformanceTracker(std::shared_ptr<INpmTaskBudget> budget,
                                                   uint32_t max_ranges_per_direction)
    : impl_(std::make_unique<Impl>(std::move(budget), max_ranges_per_direction)) {}

NpmTcpPerformanceTracker::~NpmTcpPerformanceTracker() {
    Clear();
}

NpmTcpPerformanceError NpmTcpPerformanceTracker::Observe(const NpmPacketView& packet,
                                                          const NpmSessionView& session) {
    if (!impl_->budget) return NpmTcpPerformanceError::kNullBudget;
    if (impl_->max_ranges_per_direction < 8 || impl_->max_ranges_per_direction > 65536) {
        return NpmTcpPerformanceError::kInvalidRangeLimit;
    }
    if (session.session_id == 0) return NpmTcpPerformanceError::kInvalidSession;
    if (!ValidDirection(packet.direction)) return NpmTcpPerformanceError::kInvalidPacket;
    uint8_t protocol = 0;
    if (packet.transport.tcp.valid) {
        protocol = kTcpProtocol;
    } else if (session.key != nullptr && session.key->transport_protocol == kUdpProtocol) {
        protocol = kUdpProtocol;
    } else {
        return NpmTcpPerformanceError::kInvalidPacket;
    }
    if (session.key != nullptr && session.key->transport_protocol != protocol) {
        return NpmTcpPerformanceError::kProtocolMismatch;
    }
    const auto iterator = impl_->sessions.find(session.session_id);
    if (iterator == impl_->sessions.end()) return impl_->ObserveNew(packet, session.session_id, protocol);
    if (iterator->second.transport_protocol != protocol) {
        return NpmTcpPerformanceError::kProtocolMismatch;
    }
    return impl_->ObserveExisting(&iterator->second, packet);
}

NpmTcpPerformanceError NpmTcpPerformanceTracker::Snapshot(uint64_t session_id,
                                                           NpmTcpPerformanceSnapshot* output) const {
    if (!output) return NpmTcpPerformanceError::kNullOutput;
    const auto iterator = impl_->sessions.find(session_id);
    if (iterator == impl_->sessions.end()) return NpmTcpPerformanceError::kNotFound;
    const auto& state = iterator->second;
    NpmTcpPerformanceSnapshot snapshot;
    snapshot.payload_bytes_ab = state.payload_bytes_ab;
    snapshot.payload_bytes_ba = state.payload_bytes_ba;
    if (state.transport_protocol == kUdpProtocol) {
        snapshot.handshake_status = NpmTcpHandshakeStatus::kNotApplicable;
        snapshot.rtt_status = NpmTcpRttStatus::kNotApplicable;
        snapshot.retransmission_status = NpmTcpRetransmissionStatus::kNotApplicable;
    } else {
        snapshot.handshake_status = state.handshake_status;
        snapshot.initiator = state.initiator;
        snapshot.handshake_duration_ns = state.handshake_duration_ns;
        snapshot.synack_rtt_ns = state.synack_rtt_ns;
        snapshot.rtt_status = state.rtt_status;
        if (state.rtt_status == NpmTcpRttStatus::kNoSample) {
            snapshot.rtt_samples = 0;
        } else if (state.rtt_status == NpmTcpRttStatus::kValid) {
            snapshot.rtt_samples = state.rtt_samples;
            snapshot.rtt_min_ns = state.rtt_min_ns;
            snapshot.rtt_mean_ns = static_cast<int64_t>(state.rtt_sum_ns / state.rtt_samples);
            snapshot.rtt_max_ns = state.rtt_max_ns;
        }
        if ((state.measurement_flags & kNpmMeasurementSequenceAmbiguous) != 0) {
            snapshot.retransmission_status = NpmTcpRetransmissionStatus::kAmbiguous;
        } else {
            snapshot.unique_payload_bytes_ab = state.directions[0].unique_bytes;
            snapshot.unique_payload_bytes_ba = state.directions[1].unique_bytes;
            snapshot.retrans_packets_ab = state.directions[0].retrans_packets;
            snapshot.retrans_packets_ba = state.directions[1].retrans_packets;
            snapshot.retrans_payload_bytes_ab = state.directions[0].retrans_bytes;
            snapshot.retrans_payload_bytes_ba = state.directions[1].retrans_bytes;
        }
    }
    snapshot.measurement_flags = state.measurement_flags;
    *output = snapshot;
    return NpmTcpPerformanceError::kNone;
}

NpmTcpPerformanceError NpmTcpPerformanceTracker::Snapshot(
    const NpmSessionView& session,
    NpmTcpPerformanceSnapshot* output) const {
    if (!output) return NpmTcpPerformanceError::kNullOutput;
    if (session.session_id == 0 || session.key == nullptr) {
        return NpmTcpPerformanceError::kSessionViewMismatch;
    }
    const auto iterator = impl_->sessions.find(session.session_id);
    if (iterator == impl_->sessions.end()) return NpmTcpPerformanceError::kNotFound;
    const auto& state = iterator->second;
    if (session.key->transport_protocol != state.transport_protocol) {
        return NpmTcpPerformanceError::kProtocolMismatch;
    }
    const auto duration = NonnegativeDelta(session.last_ns, session.first_ns);
    if (!duration.has_value() || session.first_ns > state.min_timestamp_ns ||
        session.last_ns < state.max_timestamp_ns || session.packets_ab < state.packets_ab ||
        session.packets_ba < state.packets_ba ||
        session.wire_bytes_ab < state.observed_wire_bytes_ab ||
        session.wire_bytes_ba < state.observed_wire_bytes_ba) {
        return NpmTcpPerformanceError::kSessionViewMismatch;
    }

    NpmTcpPerformanceSnapshot snapshot;
    const auto snapshot_error = Snapshot(session.session_id, &snapshot);
    if (snapshot_error != NpmTcpPerformanceError::kNone) return snapshot_error;
    if (*duration > 0) {
        snapshot.rate_status = NpmRateStatus::kValid;
        snapshot.wire_bps_ab = BitsPerSecond(session.wire_bytes_ab, *duration);
        snapshot.wire_bps_ba = BitsPerSecond(session.wire_bytes_ba, *duration);
        snapshot.payload_bps_ab = BitsPerSecond(state.payload_bytes_ab, *duration);
        snapshot.payload_bps_ba = BitsPerSecond(state.payload_bytes_ba, *duration);
        if (snapshot.unique_payload_bytes_ab.has_value()) {
            snapshot.unique_payload_bps_ab = BitsPerSecond(*snapshot.unique_payload_bytes_ab, *duration);
            snapshot.unique_payload_bps_ba = BitsPerSecond(*snapshot.unique_payload_bytes_ba, *duration);
        }
    }
    *output = std::move(snapshot);
    return NpmTcpPerformanceError::kNone;
}

void NpmTcpPerformanceTracker::Remove(uint64_t session_id) noexcept {
    const auto iterator = impl_->sessions.find(session_id);
    if (iterator == impl_->sessions.end()) return;
    const uint64_t bytes = iterator->second.tracked_bytes;
    impl_->sessions.erase(iterator);
    impl_->Release(bytes);
}

void NpmTcpPerformanceTracker::Clear() noexcept {
    uint64_t bytes = 0;
    for (const auto& entry : impl_->sessions) bytes += entry.second.tracked_bytes;
    impl_->sessions.clear();
    impl_->Release(bytes);
}

size_t NpmTcpPerformanceTracker::size() const noexcept {
    return impl_->sessions.size();
}

size_t NpmTcpPerformanceTracker::outstanding_segments() const noexcept {
    size_t count = 0;
    for (const auto& entry : impl_->sessions) {
        for (const auto& direction : entry.second.directions) count += direction.outstanding.size();
    }
    return count;
}

uint64_t NpmTcpPerformanceTracker::tracked_bytes() const noexcept {
    return impl_->tracked_bytes;
}

}  // namespace flowsql::npm
