// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_icmp_module.h"

#include "npm_icmp_result_encoder.h"

#include <cerrno>
#include <cstring>
#include <utility>

namespace flowsql::npm {
NpmIcmpProtocolModuleV1::NpmIcmpProtocolModuleV1(NpmIcmpConfigV1 config, std::shared_ptr<INpmTaskBudget> budget)
    : budget_(std::move(budget)), echoes_(config, budget_) {}

void NpmIcmpProtocolModuleV1::BindSessions(const NpmSessionTable* sessions, std::string input_namespace) {
    sessions_ = sessions;
    input_namespace_ = std::move(input_namespace);
}

template <typename Address>
std::optional<uint64_t> NpmIcmpProtocolModuleV1::Lookup(const NpmIcmpFamilyQuotedFlow<Address>& flow) const {
    if (!sessions_ || input_namespace_.empty() || (flow.protocol != 6 && flow.protocol != 17)) return {};
    std::conditional_t<sizeof(Address) == 4, NpmSessionKey4, NpmSessionKey6> key;
    const int order = std::memcmp(flow.src_ip.bytes, flow.dst_ip.bytes, sizeof(Address));
    const bool forward = order < 0 || (order == 0 && flow.src_port <= flow.dst_port);
    key.input_namespace = input_namespace_;
    key.observation_domain_id = flow.observation_domain_id;
    key.transport_protocol = flow.protocol;
    key.a_ip = forward ? flow.src_ip : flow.dst_ip;
    key.b_ip = forward ? flow.dst_ip : flow.src_ip;
    key.a_port = forward ? flow.src_port : flow.dst_port;
    key.b_port = forward ? flow.dst_port : flow.src_port;
    NpmSessionView session;
    if (sessions_->Find(NpmSessionKeyView(key), &session) != NpmSessionTableError::kNone) return {};
    return session.session_id;
}
std::optional<uint64_t> NpmIcmpProtocolModuleV1::FindActive(const NpmIcmpQuotedFlow4& flow) const {
    return Lookup(flow);
}
std::optional<uint64_t> NpmIcmpProtocolModuleV1::FindActive(const NpmIcmpQuotedFlow6& flow) const {
    return Lookup(flow);
}

template <typename Address>
int NpmIcmpProtocolModuleV1::Emit(const NpmIcmpFamilyEvent<Address>& event, INpmResultEmitterV1& emitter) {
    return EmitNpmIcmpEventV1(event, budget_, emitter);
}

template <typename Address>
int NpmIcmpProtocolModuleV1::OnFamilyInput(const NpmInputEventV1& input, INpmResultEmitterV1& emitter) {
    NpmIcmpFamilyParsed<Address> parsed;
    if (ParseNpmIcmpControlV1(input, &parsed) != NpmIcmpParseErrorV1::kNone) return EINVAL;
    const auto emit = [&](const auto& event) { return Emit(event, emitter); };
    if (parsed.kind == NpmIcmpMessageKindV1::kEchoRequest || parsed.kind == NpmIcmpMessageKindV1::kEchoReply)
        return echoes_.OnEcho(parsed, input.observation_domain_id, input.packet.meta.timestamp_ns, emit);
    if (parsed.kind != NpmIcmpMessageKindV1::kError) return 0;
    NpmIcmpFamilyEvent<Address> event;
    event.entity_instance_id = echoes_.AllocateEventId();
    if (event.entity_instance_id == 0) return EOVERFLOW;
    event.observed_at = input.packet.meta.timestamp_ns;
    event.observation_domain_id = input.observation_domain_id;
    event.src_ip = parsed.src_ip;
    event.dst_ip = parsed.dst_ip;
    event.outcome = "icmp_error";
    event.icmp_type = parsed.icmp_type;
    event.icmp_code = parsed.icmp_code;
    event.outer_truncated = parsed.outer_truncated;
    event.quote_status = parsed.quote_status;
    event.quoted_ip_family = parsed.quoted_ip_family;
    event.quoted_protocol = parsed.quoted_protocol;
    if (parsed.quoted_src_ip) event.quoted_src_ip = *parsed.quoted_src_ip;
    if (parsed.quoted_dst_ip) event.quoted_dst_ip = *parsed.quoted_dst_ip;
    if (parsed.quoted_flow) {
        event.quoted_src_port = parsed.quoted_flow->src_port;
        event.quoted_dst_port = parsed.quoted_flow->dst_port;
        event.active_quoted_session_id = FindActive(*parsed.quoted_flow);
    }
    event.next_hop_mtu = parsed.next_hop_mtu;
    return Emit(event, emitter);
}

int NpmIcmpProtocolModuleV1::OnInput(const NpmInputEventV1& input, INpmResultEmitterV1& emitter) {
    if (!input.layer) return EINVAL;
    if (input.layer->Get4()) return OnFamilyInput<network::IPv4Address>(input, emitter);
    if (input.layer->Get6()) return OnFamilyInput<network::IPv6Address>(input, emitter);
    return EINVAL;
}

std::optional<int64_t> NpmIcmpProtocolModuleV1::NextEventDeadlineNs() const { return echoes_.NextEventDeadlineNs(); }

int NpmIcmpProtocolModuleV1::OnTime(const NpmModuleTimeV1& time, INpmResultEmitterV1& emitter) {
    return echoes_.OnTime(time.watermark_ns, [&](const auto& event) { return Emit(event, emitter); });
}

int NpmIcmpProtocolModuleV1::Finish(int64_t observed_at_ns, INpmResultEmitterV1& emitter) {
    return echoes_.Finish(observed_at_ns, [&](const auto& event) { return Emit(event, emitter); });
}

void NpmIcmpProtocolModuleV1::Abort() noexcept { echoes_.Abort(); }

}  // namespace flowsql::npm
