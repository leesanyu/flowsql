// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_dns_tcp.h"

#include <algorithm>
#include <cerrno>
#include <new>

namespace flowsql::npm {
namespace {

void ObserveTime(std::optional<int64_t> captured_at_ns, bool* all_known, std::optional<int64_t>* maximum) {
    if (!captured_at_ns) {
        *all_known = false;
    } else if (!*maximum || *captured_at_ns > **maximum) {
        *maximum = captured_at_ns;
    }
}

}  // namespace

namespace {
constexpr uint64_t kDirectionCharge = 256;
}

void NpmDnsTcpFramerV1::ForgetFrame(DirectionState* state) noexcept {
    std::vector<uint8_t> empty;
    state->frame.swap(empty);
    if (budget_ && state->frame_charge) budget_->Release(NpmBudgetCategory::kModuleState, state->frame_charge);
    state->frame_charge = 0;
}

void NpmDnsTcpFramerV1::EraseDirection(const DirectionKey& key) noexcept {
    const auto found = directions_.find(key);
    if (found == directions_.end()) return;
    ForgetFrame(&found->second);
    directions_.erase(found);
    if (budget_) budget_->Release(NpmBudgetCategory::kModuleState, kDirectionCharge);
}

int NpmDnsTcpFramerV1::ConsumeData(DirectionState* state, const NpmTcpStreamContextV1& context,
                                   const NpmTcpStreamEventV1& event, std::vector<NpmDnsUdpResultV1>* results) {
    if (event.bytes.size == 0 || !event.bytes.data) return EINVAL;
    size_t position = 0;
    while (position < event.bytes.size && !state->disabled) {
        if (state->prefix_size < 2) {
            const size_t count = std::min<size_t>(2 - state->prefix_size, event.bytes.size - position);
            std::copy_n(event.bytes.data + position, count, state->prefix + state->prefix_size);
            state->prefix_size += static_cast<uint8_t>(count);
            position += count;
            ObserveTime(event.captured_at_ns, &state->all_times_known, &state->max_time_ns);
            if (state->prefix_size < 2) break;
            state->frame_length = static_cast<uint16_t>((static_cast<uint16_t>(state->prefix[0]) << 8) |
                                                        static_cast<uint16_t>(state->prefix[1]));
            if (state->frame_length == 0) {
                state->disabled = true;
                break;
            }
            if (budget_ &&
                budget_->Reserve(NpmBudgetCategory::kModuleState, state->frame_length) != NpmBudgetError::kNone)
                return ENOSPC;
            state->frame_charge = budget_ ? state->frame_length : 0;
            state->frame.reserve(state->frame_length);
        }
        const size_t count = std::min<size_t>(state->frame_length - state->frame.size(), event.bytes.size - position);
        state->frame.insert(state->frame.end(), event.bytes.data + position, event.bytes.data + position + count);
        position += count;
        if (count != 0) ObserveTime(event.captured_at_ns, &state->all_times_known, &state->max_time_ns);
        if (state->frame.size() != state->frame_length) break;
        const auto complete_at_ns = state->all_times_known ? state->max_time_ns : std::nullopt;
        const int error = transactions_->OnMessage(context.session->session_id, context.direction,
                                                   {state->frame.data(), state->frame.size()}, true, complete_at_ns,
                                                   complete_at_ns.value_or(context.observed_at_ns), results);
        if (error != 0) return error;
        state->prefix_size = 0;
        state->frame_length = 0;
        ForgetFrame(state);
        state->all_times_known = true;
        state->max_time_ns.reset();
    }
    return 0;
}

int NpmDnsTcpFramerV1::OnReadable(const NpmTcpStreamContextV1& context, INpmTcpStreamCursorV1& cursor,
                                  std::vector<NpmDnsUdpResultV1>* results) {
    if (!transactions_ || !results || !context.session || context.session->session_id == 0 ||
        (context.direction != NpmPacketDirection::kAToB && context.direction != NpmPacketDirection::kBToA))
        return EINVAL;
    try {
        const DirectionKey key{context.session->session_id, context.direction};
        auto found = directions_.find(key);
        if (found == directions_.end()) {
            if (budget_ && budget_->Reserve(NpmBudgetCategory::kModuleState, kDirectionCharge) != NpmBudgetError::kNone)
                return ENOSPC;
            try {
                found = directions_.try_emplace(key).first;
            } catch (...) {
                if (budget_) budget_->Release(NpmBudgetCategory::kModuleState, kDirectionCharge);
                throw;
            }
        }
        auto& state = found->second;
        if (context.origin != NpmTcpStreamOriginV1::kSyn) state.disabled = true;
        NpmTcpStreamEventV1 event;
        while (cursor.Peek(&event)) {
            if (event.kind == NpmTcpStreamEventKindV1::kData) {
                if (!state.disabled) {
                    const int error = ConsumeData(&state, context, event, results);
                    if (error != 0) return error;
                }
                if (event.bytes.size == 0 || cursor.Consume(event.bytes.size) != 0) return EINVAL;
            } else if (event.kind == NpmTcpStreamEventKindV1::kGap) {
                state.disabled = true;
                state.prefix_size = 0;
                ForgetFrame(&state);
                const int error = transactions_->OnResponsePathGap(context.session->session_id, context.direction,
                                                                   context.observed_at_ns, results);
                if (error != 0 || cursor.Consume(0) != 0) return error != 0 ? error : EINVAL;
            } else if (event.kind == NpmTcpStreamEventKindV1::kEnd) {
                state.disabled = true;
                state.prefix_size = 0;
                ForgetFrame(&state);
                if (cursor.Consume(0) != 0) return EINVAL;
            } else {
                return EINVAL;
            }
        }
        if (state.disabled && context.final_drain) EraseDirection(key);
        return 0;
    } catch (const std::bad_alloc&) {
        return ENOMEM;
    }
}

int NpmDnsTcpFramerV1::OnSessionEnd(uint64_t session_id, int64_t observed_at_ns,
                                    std::vector<NpmDnsUdpResultV1>* results) {
    if (!transactions_ || !results || session_id == 0) return EINVAL;
    const int error = transactions_->OnSessionEnd(session_id, observed_at_ns, results);
    if (error != 0) return error;
    EraseDirection({session_id, NpmPacketDirection::kAToB});
    EraseDirection({session_id, NpmPacketDirection::kBToA});
    return 0;
}

void NpmDnsTcpFramerV1::Abort() noexcept {
    for (auto& direction : directions_) {
        ForgetFrame(&direction.second);
        if (budget_) budget_->Release(NpmBudgetCategory::kModuleState, kDirectionCharge);
    }
    directions_.clear();
}

}  // namespace flowsql::npm
