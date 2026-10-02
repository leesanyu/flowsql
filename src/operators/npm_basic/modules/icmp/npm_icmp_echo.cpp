// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_icmp_echo.h"

#include <arpa/inet.h>

#include <cerrno>
#include <cstring>
#include <limits>
#include <new>
#include <tuple>
#include <utility>

namespace flowsql::npm {
namespace {

std::string FormatIp(const packet::IpAddress& address) {
    char text[INET6_ADDRSTRLEN] = {};
    if (const auto* ipv4 = std::get_if<packet::IPv4Address>(&address)) {
        if (inet_ntop(AF_INET, ipv4->bytes, text, sizeof(text))) return text;
    } else if (const auto* ipv6 = std::get_if<packet::IPv6Address>(&address)) {
        if (inet_ntop(AF_INET6, ipv6->bytes, text, sizeof(text))) return text;
    }
    return {};
}

int64_t Deadline(int64_t start, int64_t timeout) {
    return start > std::numeric_limits<int64_t>::max() - timeout ? std::numeric_limits<int64_t>::max()
                                                                 : start + timeout;
}

NpmIcmpEchoKeyV1 MakeKey(const NpmIcmpParsedV1& message, uint64_t domain, bool reverse) {
    NpmIcmpEchoKeyV1 key;
    key.observation_domain_id = domain;
    key.ip_family = message.ip_family == 4 ? packet::AddressFamily::kIPv4 : packet::AddressFamily::kIPv6;
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

bool NpmIcmpEchoTrackerV1::EchoKeyLess::operator()(const NpmIcmpEchoKeyV1& left, const NpmIcmpEchoKeyV1& right) const {
    return std::tie(left.observation_domain_id, left.ip_family, left.src_ip, left.dst_ip, left.identifier,
                    left.sequence) < std::tie(right.observation_domain_id, right.ip_family, right.src_ip, right.dst_ip,
                                              right.identifier, right.sequence);
}

uint64_t NpmIcmpEchoTrackerV1::AllocateEventId() noexcept {
    if (next_event_id_ == 0) return 0;
    return next_event_id_++;
}

NpmIcmpEventV1 NpmIcmpEchoTrackerV1::MakeEchoEvent(const NpmIcmpEchoKeyV1& key, const NpmIcmpPendingEchoV1& pending,
                                                   int64_t observed_at_ns, std::string outcome) const {
    NpmIcmpEventV1 event;
    event.entity_instance_id = pending.entity_instance_id;
    event.observed_at = observed_at_ns;
    event.observation_domain_id = key.observation_domain_id;
    event.ip_family = key.ip_family == packet::AddressFamily::kIPv4 ? 4 : 6;
    event.src_ip = FormatIp(key.src_ip);
    event.dst_ip = FormatIp(key.dst_ip);
    event.outcome = std::move(outcome);
    event.icmp_type = event.ip_family == 4 ? 8 : 128;
    event.echo_id = key.identifier;
    event.echo_sequence = key.sequence;
    event.echo_retries = pending.retries;
    event.request_at_ns = pending.request_at_ns;
    return event;
}

void NpmIcmpEchoTrackerV1::Erase(const NpmIcmpEchoKeyV1& key, const NpmIcmpPendingEchoV1& pending) noexcept {
    deadlines_.erase({pending.deadline_ns, pending.entity_instance_id});
    pending_.erase(key);
    if (budget_) budget_->Release(NpmBudgetCategory::kModuleState, kPendingCharge);
}

int NpmIcmpEchoTrackerV1::OnEcho(const NpmIcmpParsedV1& message, uint64_t observation_domain_id, int64_t captured_at_ns,
                                 const Emit& emit) {
    if (finished_ || !emit ||
        (message.kind != NpmIcmpMessageKindV1::kEchoRequest && message.kind != NpmIcmpMessageKindV1::kEchoReply))
        return EINVAL;
    const bool request = message.kind == NpmIcmpMessageKindV1::kEchoRequest;
    const auto key = MakeKey(message, observation_domain_id, !request);
    if (message.outer_truncated) {
        const uint64_t id = AllocateEventId();
        if (id == 0) return EOVERFLOW;
        NpmIcmpEventV1 event;
        event.entity_instance_id = id;
        event.observed_at = captured_at_ns;
        event.observation_domain_id = observation_domain_id;
        event.ip_family = message.ip_family;
        event.src_ip = FormatIp(message.src_ip);
        event.dst_ip = FormatIp(message.dst_ip);
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
    auto found = pending_.find(key);
    if (request) {
        if (found != pending_.end()) {
            if (found->second.retries == std::numeric_limits<uint32_t>::max()) return EOVERFLOW;
            ++found->second.retries;
            return 0;
        }
        if (pending_.size() >= config_.max_pending_echo) return ENOSPC;
        if (!budget_ || budget_->Reserve(NpmBudgetCategory::kModuleState, kPendingCharge) != NpmBudgetError::kNone)
            return ENOMEM;
        NpmIcmpPendingEchoV1 pending;
        pending.entity_instance_id = AllocateEventId();
        if (pending.entity_instance_id == 0) {
            budget_->Release(NpmBudgetCategory::kModuleState, kPendingCharge);
            return EOVERFLOW;
        }
        pending.request_at_ns = captured_at_ns;
        pending.deadline_ns = Deadline(captured_at_ns, config_.echo_timeout_ns);
        try {
            pending_.emplace(key, pending);
            deadlines_.emplace(DeadlineKey{pending.deadline_ns, pending.entity_instance_id}, key);
        } catch (const std::bad_alloc&) {
            pending_.erase(key);
            budget_->Release(NpmBudgetCategory::kModuleState, kPendingCharge);
            return ENOMEM;
        }
        return 0;
    }
    if (found == pending_.end()) {
        const uint64_t id = AllocateEventId();
        if (id == 0) return EOVERFLOW;
        NpmIcmpEventV1 event;
        event.entity_instance_id = id;
        event.observed_at = captured_at_ns;
        event.observation_domain_id = observation_domain_id;
        event.ip_family = message.ip_family;
        event.src_ip = FormatIp(message.src_ip);
        event.dst_ip = FormatIp(message.dst_ip);
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

int NpmIcmpEchoTrackerV1::OnTime(std::optional<int64_t> watermark_ns, const Emit& emit) {
    if (finished_ || !watermark_ns || (last_watermark_ns_ && *watermark_ns <= *last_watermark_ns_)) return 0;
    if (!emit) return EINVAL;
    last_watermark_ns_ = watermark_ns;
    while (!deadlines_.empty() && deadlines_.begin()->first.first <= *watermark_ns) {
        const auto key = deadlines_.begin()->second;
        auto pending = pending_.find(key);
        if (pending == pending_.end()) return EINVAL;
        auto event = MakeEchoEvent(key, pending->second, *watermark_ns, "echo_request_only");
        event.incomplete_reason = "response_not_observed_by_deadline";
        const int result = emit(event);
        if (result != 0) return result;
        Erase(key, pending->second);
    }
    return 0;
}

int NpmIcmpEchoTrackerV1::Finish(int64_t observed_at_ns, const Emit& emit) {
    if (finished_) return 0;
    if (!emit) return EINVAL;
    while (!pending_.empty()) {
        const auto found = pending_.begin();
        auto event = MakeEchoEvent(found->first, found->second, observed_at_ns, "echo_request_only");
        event.incomplete_reason = "task_eof";
        const int result = emit(event);
        if (result != 0) return result;
        Erase(found->first, found->second);
    }
    finished_ = true;
    return 0;
}

void NpmIcmpEchoTrackerV1::Abort() noexcept {
    if (budget_) {
        for (size_t i = 0; i < pending_.size(); ++i) budget_->Release(NpmBudgetCategory::kModuleState, kPendingCharge);
    }
    pending_.clear();
    deadlines_.clear();
    finished_ = true;
}

std::optional<int64_t> NpmIcmpEchoTrackerV1::NextEventDeadlineNs() const {
    if (deadlines_.empty()) return {};
    return deadlines_.begin()->first.first;
}

}  // namespace flowsql::npm
