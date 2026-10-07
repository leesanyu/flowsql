// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_basic_periodic_stats.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <limits>

namespace flowsql::npm {
namespace {
constexpr uint64_t kBucketCharge = 128;
bool Add(uint64_t& value, uint64_t increment) {
    if (increment > std::numeric_limits<uint64_t>::max() - value) return false;
    value += increment;
    return true;
}
int64_t End(int64_t start, int64_t period) {
    return start > std::numeric_limits<int64_t>::max() - period ? std::numeric_limits<int64_t>::max() : start + period;
}
}  // namespace

NpmBasicPeriodicStats::NpmBasicPeriodicStats(NpmAnalysisConfig config, std::shared_ptr<INpmTaskBudget> budget,
                                             NpmBasicResultProjector& projector)
    : config_(config), budget_(std::move(budget)), projector_(projector) {}
NpmBasicPeriodicStats::~NpmBasicPeriodicStats() {
    for (auto& item : states_) Release(item.second);
}
int NpmBasicPeriodicStats::Fail(std::string message) {
    if (error_.empty()) error_ = std::move(message);
    return EIO;
}
void NpmBasicPeriodicStats::Release(State& state) {
    budget_->Release(NpmBudgetCategory::kModuleState, state.charge);
    state.charge = 0;
}
void NpmBasicPeriodicStats::Schedule(const State& state) {
    int64_t due = End(state.next_start_ns, config_.output_interval_ns);
    if (state.end_ns) due = std::min(due, *state.end_ns);
    if (!next_due_ns_ || due < *next_due_ns_) next_due_ns_ = due;
}
int NpmBasicPeriodicStats::RetainMetadata(State& state, const NpmSessionView& view) {
    state.snapshot.session_id = view.session_id;
    state.snapshot.primary_label_id = view.primary_label_id;
    state.snapshot.protocol_status = view.protocol_status;
    state.snapshot.protocol_id = view.protocol_id;
    state.snapshot.protocol_sub_id = view.protocol_sub_id;
    return 0;
}
int NpmBasicPeriodicStats::OnPacket(const NpmPacketView& packet, const NpmSessionView& view, INpmResultWriter&) {
    const int64_t time = packet.packet.meta.timestamp_ns;
    if (time < 0 || !view.key) return Fail("invalid Basic periodic packet time/key");
    const int64_t start = time - time % config_.output_interval_ns;
    if (start > std::numeric_limits<int64_t>::max() - config_.output_interval_ns)
        return Fail("Basic period boundary overflow");
    if (watermark_ns_ && End(start, config_.output_interval_ns) <= *watermark_ns_) {
        return Fail("late Basic packet source=" + std::to_string(packet.packet.meta.source_id) +
                    " timestamp_ns=" + std::to_string(time) + " closed_boundary_ns=" + std::to_string(*watermark_ns_));
    }
    try {
        auto found = states_.find(view.session_id);
        if (found == states_.end()) {
            const uint64_t charge = sizeof(State) + 192 + view.key->input_namespace.size();
            if (budget_->Reserve(NpmBudgetCategory::kModuleState, charge) != NpmBudgetError::kNone)
                return Fail("Basic periodic state budget exceeded");
            try {
                State state;
                state.snapshot.key = *view.key;
                state.next_start_ns = start;
                state.charge = charge;
                found = states_.emplace(view.session_id, std::move(state)).first;
            } catch (...) {
                budget_->Release(NpmBudgetCategory::kModuleState, charge);
                throw;
            }
        }
        auto& state = found->second;
        if (state.end_ns || (state.last_period && start < state.next_start_ns))
            return Fail("Basic packet entered a closed session/period");
        RetainMetadata(state, view);
        if (!state.last_period) state.next_start_ns = std::min(state.next_start_ns, start);
        Schedule(state);
        auto bucket = state.buckets.find(start);
        if (bucket == state.buckets.end()) {
            if (budget_->Reserve(NpmBudgetCategory::kModuleState, kBucketCharge) != NpmBudgetError::kNone)
                return Fail("Basic periodic bucket budget exceeded");
            try {
                bucket = state.buckets.emplace(start, Bucket{0, 0, 0, 0, time, time}).first;
                state.charge += kBucketCharge;
            } catch (...) {
                budget_->Release(NpmBudgetCategory::kModuleState, kBucketCharge);
                throw;
            }
        }
        auto& b = bucket->second;
        b.first_ns = std::min(b.first_ns, time);
        b.last_ns = std::max(b.last_ns, time);
        const bool ab = packet.direction == NpmPacketDirection::kAToB;
        if (!Add(ab ? b.packets_ab : b.packets_ba, 1) ||
            !Add(ab ? b.bytes_ab : b.bytes_ba, packet.packet.meta.wire_len))
            return Fail("Basic periodic counter overflow");
        input_end_ns_ = input_end_ns_ ? std::max(*input_end_ns_, time) : time;
        return 0;
    } catch (const std::bad_alloc&) {
        return Fail("Basic periodic allocation failed");
    }
}
int NpmBasicPeriodicStats::Emit(State& state, bool complete, bool final, int64_t observed_at,
                                INpmResultWriter& writer) {
    NpmBasicPeriodStats period{state.next_start_ns, End(state.next_start_ns, config_.output_interval_ns), complete};
    auto found = state.buckets.find(state.next_start_ns);
    if (found != state.buckets.end()) {
        const auto& b = found->second;
        period.interval_packets_ab = b.packets_ab;
        period.interval_packets_ba = b.packets_ba;
        period.interval_wire_bytes_ab = b.bytes_ab;
        period.interval_wire_bytes_ba = b.bytes_ba;
        state.first_ns = state.first_ns ? std::min(*state.first_ns, b.first_ns) : b.first_ns;
        state.last_ns = state.last_ns ? std::max(*state.last_ns, b.last_ns) : b.last_ns;
    } else if (final && state.last_period && state.next_start_ns >= *state.end_ns) {
        period = *state.last_period;
        period.interval_packets_ab = period.interval_packets_ba = 0;
        period.interval_wire_bytes_ab = period.interval_wire_bytes_ba = period.interval_wire_bytes_total = 0;
    }
    if (!Add(state.packets_ab, period.interval_packets_ab) || !Add(state.packets_ba, period.interval_packets_ba) ||
        !Add(state.bytes_ab, period.interval_wire_bytes_ab) || !Add(state.bytes_ba, period.interval_wire_bytes_ba) ||
        period.interval_wire_bytes_ba > std::numeric_limits<uint64_t>::max() - period.interval_wire_bytes_ab)
        return Fail("Basic periodic total overflow");
    period.interval_wire_bytes_total = period.interval_wire_bytes_ab + period.interval_wire_bytes_ba;
    auto view = state.snapshot.View();
    view.first_ns = state.first_ns.value_or(state.snapshot.first_ns);
    view.last_ns = state.last_ns.value_or(view.first_ns);
    view.packets_ab = state.packets_ab;
    view.packets_ba = state.packets_ba;
    view.wire_bytes_ab = state.bytes_ab;
    view.wire_bytes_ba = state.bytes_ba;
    if (config_.run_mode == NpmRunMode::kOffline) {
        observed_at =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();
    }
    NpmBasicResult result;
    auto error = final ? projector_.ProjectFinal(view, state.snapshot.end_reason, observed_at, &result)
                       : projector_.ProjectActive(view, observed_at, &result);
    if (error != NpmBasicProjectionError::kNone) return Fail("Basic periodic projection failed");
    result.period = period;
    const int rc = writer.WriteBasic(result);
    if (rc != 0) return Fail("Basic periodic consumer failed");
    if (found != state.buckets.end()) {
        state.buckets.erase(found);
        budget_->Release(NpmBudgetCategory::kModuleState, kBucketCharge);
        state.charge -= kBucketCharge;
    }
    state.last_period = period;
    state.next_start_ns = period.period_end_ns;
    return 0;
}
int NpmBasicPeriodicStats::Drain(State& state, int64_t boundary, bool finish, int64_t observed_at,
                                 INpmResultWriter& writer) {
    const int64_t limit = state.end_ns ? std::min(boundary, *state.end_ns) : boundary;
    while (End(state.next_start_ns, config_.output_interval_ns) <= limit && state.next_start_ns < limit) {
        const int rc = Emit(state, true, false, observed_at, writer);
        if (rc != 0) return rc;
    }
    if (finish && state.end_ns && *state.end_ns <= boundary) {
        const int rc = Emit(state, false, true, observed_at, writer);
        if (rc != 0) return rc;
        if (!state.buckets.empty()) return Fail("Basic terminal boundary left uncounted packets");
    }
    return 0;
}
int NpmBasicPeriodicStats::OnSessionEnd(const NpmSessionView& view, NpmSessionEndReason reason, int64_t observed_at,
                                        INpmResultWriter& writer) {
    auto found = states_.find(view.session_id);
    if (found == states_.end()) return Fail("Basic terminal session has no statistics");
    auto& state = found->second;
    RetainMetadata(state, view);
    state.snapshot.first_ns = view.first_ns;
    state.snapshot.end_reason = reason;
    if (reason == NpmSessionEndReason::kIdleTimeout) {
        state.end_ns = End(view.last_ns, view.key->transport_protocol == 6 ? config_.tcp_idle_timeout_ns
                                                                           : config_.udp_idle_timeout_ns);
    } else if (reason == NpmSessionEndReason::kTupleReuse) {
        state.end_ns = std::max(view.last_ns, observed_at);
    } else if (reason == NpmSessionEndReason::kEof) {
        state.end_ns = std::max(view.last_ns, input_end_ns_.value_or(view.last_ns));
    } else {
        state.end_ns = view.last_ns;
    }
    Schedule(state);
    return 0;
}
int NpmBasicPeriodicStats::OnTime(int64_t watermark, int64_t observed_at, INpmResultWriter& writer) {
    if (watermark_ns_ && watermark < *watermark_ns_) return 0;
    watermark_ns_ = watermark;
    input_end_ns_ = std::max(input_end_ns_.value_or(watermark), End(watermark, config_.out_of_order_tolerance_ns));
    if (!next_due_ns_ || watermark < *next_due_ns_) return 0;
    next_due_ns_.reset();
    for (auto it = states_.begin(); it != states_.end();) {
        auto& state = it->second;
        const bool final = state.end_ns && *state.end_ns <= watermark;
        const int rc = Drain(state, watermark, final, observed_at, writer);
        if (rc != 0) return rc;
        if (final) {
            Release(state);
            it = states_.erase(it);
        } else {
            Schedule(state);
            ++it;
        }
    }
    return 0;
}
int NpmBasicPeriodicStats::OnFinish(int64_t observed_at, INpmResultWriter& writer) {
    for (auto& item : states_) {
        auto& state = item.second;
        if (!state.end_ns) return Fail("Basic EOF notification missing");
        const int rc = Drain(state, *state.end_ns, true, observed_at, writer);
        if (rc != 0) return rc;
        Release(state);
    }
    states_.clear();
    next_due_ns_.reset();
    return 0;
}
}  // namespace flowsql::npm
