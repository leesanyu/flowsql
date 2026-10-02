// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_icmp_module.h"

#include "npm_icmp_result_encoder.h"

#include <arpa/inet.h>

#include <cerrno>
#include <cstring>
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

int CompareIp(const packet::IpAddress& left, const packet::IpAddress& right, packet::AddressFamily family) {
    if (family == packet::AddressFamily::kIPv4) {
        return std::memcmp(std::get<packet::IPv4Address>(left).bytes, std::get<packet::IPv4Address>(right).bytes, 4);
    }
    return std::memcmp(std::get<packet::IPv6Address>(left).bytes, std::get<packet::IPv6Address>(right).bytes, 16);
}

}  // namespace

NpmIcmpProtocolModuleV1::NpmIcmpProtocolModuleV1(NpmIcmpConfigV1 config, std::shared_ptr<INpmTaskBudget> budget)
    : budget_(std::move(budget)), echoes_(config, budget_) {}

void NpmIcmpProtocolModuleV1::BindSessions(const NpmSessionTable* sessions, std::string input_namespace) {
    sessions_ = sessions;
    input_namespace_ = std::move(input_namespace);
}

std::optional<uint64_t> NpmIcmpProtocolModuleV1::FindActive(const NpmIcmpQuotedFlowV1& flow) const {
    if (!sessions_ || input_namespace_.empty() || (flow.protocol != 6 && flow.protocol != 17) ||
        (flow.ip_family != packet::AddressFamily::kIPv4 && flow.ip_family != packet::AddressFamily::kIPv6))
        return {};
    NpmSessionKey key;
    key.input_namespace = input_namespace_;
    key.observation_domain_id = flow.observation_domain_id;
    key.ip_family = flow.ip_family;
    key.transport_protocol = flow.protocol;
    const NpmEndpoint src{flow.src_ip, flow.src_port}, dst{flow.dst_ip, flow.dst_port};
    const int order = CompareIp(src.ip, dst.ip, flow.ip_family);
    if (order < 0 || (order == 0 && src.port <= dst.port)) {
        key.a = src;
        key.b = dst;
    } else {
        key.a = dst;
        key.b = src;
    }
    NpmSessionView session;
    if (sessions_->Find(key, &session) != NpmSessionTableError::kNone) return {};
    return session.session_id;
}

int NpmIcmpProtocolModuleV1::Emit(const NpmIcmpEventV1& event, INpmResultEmitterV1& emitter) {
    return EmitNpmIcmpEventV1(event, budget_, emitter);
}

int NpmIcmpProtocolModuleV1::OnInput(const NpmInputEventV1& input, INpmResultEmitterV1& emitter) {
    NpmIcmpParsedV1 parsed;
    if (ParseNpmIcmpControlV1(input, &parsed) != NpmIcmpParseErrorV1::kNone) return EINVAL;
    const auto emit = [&](const NpmIcmpEventV1& event) { return Emit(event, emitter); };
    if (parsed.kind == NpmIcmpMessageKindV1::kEchoRequest || parsed.kind == NpmIcmpMessageKindV1::kEchoReply)
        return echoes_.OnEcho(parsed, input.observation_domain_id, input.packet.meta.timestamp_ns, emit);
    if (parsed.kind != NpmIcmpMessageKindV1::kError) return 0;
    NpmIcmpEventV1 event;
    event.entity_instance_id = echoes_.AllocateEventId();
    if (event.entity_instance_id == 0) return EOVERFLOW;
    event.observed_at = input.packet.meta.timestamp_ns;
    event.observation_domain_id = input.observation_domain_id;
    event.ip_family = parsed.ip_family;
    event.src_ip = FormatIp(parsed.src_ip);
    event.dst_ip = FormatIp(parsed.dst_ip);
    event.outcome = "icmp_error";
    event.icmp_type = parsed.icmp_type;
    event.icmp_code = parsed.icmp_code;
    event.outer_truncated = parsed.outer_truncated;
    event.quote_status = parsed.quote_status;
    event.quoted_ip_family = parsed.quoted_ip_family;
    event.quoted_protocol = parsed.quoted_protocol;
    if (parsed.quoted_src_ip) event.quoted_src_ip = FormatIp(*parsed.quoted_src_ip);
    if (parsed.quoted_dst_ip) event.quoted_dst_ip = FormatIp(*parsed.quoted_dst_ip);
    if (parsed.quoted_flow) {
        event.quoted_src_port = parsed.quoted_flow->src_port;
        event.quoted_dst_port = parsed.quoted_flow->dst_port;
        event.active_quoted_session_id = FindActive(*parsed.quoted_flow);
    }
    event.next_hop_mtu = parsed.next_hop_mtu;
    return Emit(event, emitter);
}

std::optional<int64_t> NpmIcmpProtocolModuleV1::NextEventDeadlineNs() const { return echoes_.NextEventDeadlineNs(); }

int NpmIcmpProtocolModuleV1::OnTime(const NpmModuleTimeV1& time, INpmResultEmitterV1& emitter) {
    return echoes_.OnTime(time.watermark_ns, [&](const NpmIcmpEventV1& event) { return Emit(event, emitter); });
}

int NpmIcmpProtocolModuleV1::Finish(int64_t observed_at_ns, INpmResultEmitterV1& emitter) {
    return echoes_.Finish(observed_at_ns, [&](const NpmIcmpEventV1& event) { return Emit(event, emitter); });
}

void NpmIcmpProtocolModuleV1::Abort() noexcept { echoes_.Abort(); }

}  // namespace flowsql::npm
