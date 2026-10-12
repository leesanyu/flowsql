// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_icmp_echo.h"

#include <cerrno>
#include <cstring>
#include <limits>
#include <new>
#include <tuple>
#include <utility>

namespace flowsql::npm {
namespace {

int64_t Deadline(int64_t start, int64_t timeout) {
    return start > std::numeric_limits<int64_t>::max() - timeout ? std::numeric_limits<int64_t>::max()
                                                                 : start + timeout;
}

template <typename Address>
NpmIcmpFamilyEchoKey<Address> MakeKey(const NpmIcmpFamilyParsed<Address>& message, uint64_t domain, bool reverse) {
    NpmIcmpFamilyEchoKey<Address> key;
    key.observation_domain_id = domain;
    key.src_ip = reverse ? message.dst_ip : message.src_ip;
    key.dst_ip = reverse ? message.src_ip : message.dst_ip;
    key.identifier = message.echo_id;
    key.sequence = message.echo_sequence;
    return key;
}

}  // namespace

NpmIcmpEchoTrackerV1::NpmIcmpEchoTrackerV1(NpmIcmpConfigV1 config, std::shared_ptr<INpmTaskBudget> budget)
    : config_(config), budget_(std::move(budget)) {}

NpmIcmpEchoTrackerV1::~NpmIcmpEchoTrackerV1() { Abort(); }

template <typename Address>
bool NpmIcmpEchoTrackerV1::EchoKeyLess<Address>::operator()(const NpmIcmpFamilyEchoKey<Address>& left,
                                                            const NpmIcmpFamilyEchoKey<Address>& right) const {
    if (left.observation_domain_id != right.observation_domain_id)
        return left.observation_domain_id < right.observation_domain_id;
    const int src = std::memcmp(left.src_ip.bytes, right.src_ip.bytes, sizeof(Address));
    if (src) return src < 0;
    const int dst = std::memcmp(left.dst_ip.bytes, right.dst_ip.bytes, sizeof(Address));
    if (dst) return dst < 0;
    return std::tie(left.identifier, left.sequence) < std::tie(right.identifier, right.sequence);
}

uint64_t NpmIcmpEchoTrackerV1::AllocateEventId() noexcept {
    if (next_event_id_ == 0) return 0;
    return next_event_id_++;
}

template <typename Address>
NpmIcmpFamilyEvent<Address> NpmIcmpEchoTrackerV1::MakeEchoEvent(const NpmIcmpFamilyEchoKey<Address>& key,
                                                                const NpmIcmpPendingEchoV1& pending,
                                                                int64_t observed_at_ns, std::string outcome) const {
    NpmIcmpFamilyEvent<Address> event;
    event.entity_instance_id = pending.entity_instance_id;
    event.observed_at = observed_at_ns;
    event.observation_domain_id = key.observation_domain_id;
    event.src_ip = key.src_ip;
    event.dst_ip = key.dst_ip;
    event.outcome = std::move(outcome);
    event.icmp_type = sizeof(Address) == 4 ? 8 : 128;
    event.echo_id = key.identifier;
    event.echo_sequence = key.sequence;
    event.echo_retries = pending.retries;
    event.request_at_ns = pending.request_at_ns;
    return event;
}

template <typename Address>
void NpmIcmpEchoTrackerV1::Erase(const NpmIcmpFamilyEchoKey<Address>& key,
                                 const NpmIcmpPendingEchoV1& pending) noexcept {
    deadlines_.erase({pending.deadline_ns, pending.entity_instance_id});
    Pending<Address>().erase(key);
    if (budget_) budget_->Release(NpmBudgetCategory::kModuleState, PendingCharge<Address>());
}

template <typename Address>
int NpmIcmpEchoTrackerV1::OnEcho(const NpmIcmpFamilyParsed<Address>& message, uint64_t observation_domain_id,
                                 int64_t captured_at_ns, const Emit& emit) {
    if (finished_ || !emit ||
        (message.kind != NpmIcmpMessageKindV1::kEchoRequest && message.kind != NpmIcmpMessageKindV1::kEchoReply))
        return EINVAL;
    const bool request = message.kind == NpmIcmpMessageKindV1::kEchoRequest;
    const auto key = MakeKey(message, observation_domain_id, !request);
    if (message.outer_truncated) {
        const uint64_t id = AllocateEventId();
        if (id == 0) return EOVERFLOW;
        NpmIcmpFamilyEvent<Address> event;
        event.entity_instance_id = id;
        event.observed_at = captured_at_ns;
        event.observation_domain_id = observation_domain_id;
        event.src_ip = message.src_ip;
        event.dst_ip = message.dst_ip;
        event.outcome = request ? "echo_request_only" : "echo_reply_only";
        event.icmp_type = message.icmp_type;
        event.icmp_code = message.icmp_code;
        event.outer_truncated = true;
        event.incomplete_reason = "capture_truncation";
        event.echo_id = message.echo_id;
        event.echo_sequence = message.echo_sequence;
        if (request) {
            event.echo_retries = 0;
            event.request_at_ns = captured_at_ns;
        } else {
            event.reply_at_ns = captured_at_ns;
        }
        return emit(event);
    }
    auto& pending_map = Pending<Address>();
    auto found = pending_map.find(key);
    if (request) {
        if (found != pending_map.end()) {
            if (found->second.retries == std::numeric_limits<uint32_t>::max()) return EOVERFLOW;
            ++found->second.retries;
            return 0;
        }
        if (PendingCount() >= config_.max_pending_echo) return ENOSPC;
        if (!budget_ ||
            budget_->Reserve(NpmBudgetCategory::kModuleState, PendingCharge<Address>()) != NpmBudgetError::kNone)
            return ENOMEM;
        NpmIcmpPendingEchoV1 pending;
        pending.entity_instance_id = AllocateEventId();
        if (pending.entity_instance_id == 0) {
            budget_->Release(NpmBudgetCategory::kModuleState, PendingCharge<Address>());
            return EOVERFLOW;
        }
        pending.request_at_ns = captured_at_ns;
        pending.deadline_ns = Deadline(captured_at_ns, config_.echo_timeout_ns);
        try {
            const auto node = pending_map.emplace(key, pending).first;
            deadlines_.emplace(DeadlineKey{pending.deadline_ns, pending.entity_instance_id}, &node->first);
        } catch (const std::bad_alloc&) {
            Pending<Address>().erase(key);
            budget_->Release(NpmBudgetCategory::kModuleState, PendingCharge<Address>());
            return ENOMEM;
        }
        return 0;
    }
    if (found == pending_map.end()) {
        const uint64_t id = AllocateEventId();
        if (id == 0) return EOVERFLOW;
        NpmIcmpFamilyEvent<Address> event;
        event.entity_instance_id = id;
        event.observed_at = captured_at_ns;
        event.observation_domain_id = observation_domain_id;
        event.src_ip = message.src_ip;
        event.dst_ip = message.dst_ip;
        event.outcome = "echo_reply_only";
        event.icmp_type = message.icmp_type;
        event.icmp_code = message.icmp_code;
        event.incomplete_reason = "no_pending_request";
        event.echo_id = message.echo_id;
        event.echo_sequence = message.echo_sequence;
        event.reply_at_ns = captured_at_ns;
        return emit(event);
    }
    auto event = MakeEchoEvent(key, found->second, captured_at_ns, "echo_matched");
    event.reply_at_ns = captured_at_ns;
    if (captured_at_ns >= found->second.request_at_ns) {
        const uint64_t elapsed =
            static_cast<uint64_t>(captured_at_ns) - static_cast<uint64_t>(found->second.request_at_ns);
        if (elapsed <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
            event.latency_ns = static_cast<int64_t>(elapsed);
    }
    const int result = emit(event);
    if (result == 0) Erase(key, found->second);
    return result;
}

int NpmIcmpEchoTrackerV1::EmitDeadline(int64_t observed_at_ns, const char* reason, const Emit& emit) {
    const auto target = deadlines_.begin()->second;
    return std::visit(
        [&](const auto* key) {
            using Address = std::decay_t<decltype(key->src_ip)>;
            auto& pending = Pending<Address>();
            const auto found = pending.find(*key);
            if (found == pending.end()) return EINVAL;
            auto event = MakeEchoEvent(*key, found->second, observed_at_ns, "echo_request_only");
            event.incomplete_reason = reason;
            const int error = emit(event);
            if (!error) Erase(*key, found->second);
            return error;
        },
        target);
}

int NpmIcmpEchoTrackerV1::OnTime(std::optional<int64_t> watermark_ns, const Emit& emit) {
    if (finished_ || !watermark_ns || (last_watermark_ns_ && *watermark_ns <= *last_watermark_ns_)) return 0;
    if (!emit) return EINVAL;
    last_watermark_ns_ = watermark_ns;
    while (!deadlines_.empty() && deadlines_.begin()->first.first <= *watermark_ns) {
        const int error = EmitDeadline(*watermark_ns, "response_not_observed_by_deadline", emit);
        if (error) return error;
    }
    return 0;
}

int NpmIcmpEchoTrackerV1::Finish(int64_t observed_at_ns, const Emit& emit) {
    if (finished_) return 0;
    if (!emit) return EINVAL;
    while (!deadlines_.empty()) {
        const int error = EmitDeadline(observed_at_ns, "task_eof", emit);
        if (error) return error;
    }
    finished_ = true;
    return 0;
}

void NpmIcmpEchoTrackerV1::Abort() noexcept {
    if (budget_) {
        budget_->Release(NpmBudgetCategory::kModuleState, pending4_.size() * PendingCharge<network::IPv4Address>());
        budget_->Release(NpmBudgetCategory::kModuleState, pending6_.size() * PendingCharge<network::IPv6Address>());
    }
    deadlines_.clear();
    pending4_.clear();
    pending6_.clear();
    finished_ = true;
}

std::optional<int64_t> NpmIcmpEchoTrackerV1::NextEventDeadlineNs() const {
    if (deadlines_.empty()) return {};
    return deadlines_.begin()->first.first;
}

template int NpmIcmpEchoTrackerV1::OnEcho(const NpmIcmpParsed4&, uint64_t, int64_t, const Emit&);
template int NpmIcmpEchoTrackerV1::OnEcho(const NpmIcmpParsed6&, uint64_t, int64_t, const Emit&);

}  // namespace flowsql::npm
