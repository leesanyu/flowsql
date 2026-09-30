// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_dns_module.h"

#include <arpa/inet.h>

#include <cerrno>
#include <new>
#include <utility>

namespace flowsql::npm {
namespace {

constexpr uint64_t kSessionIdentityCharge = 512;

bool FormatAddress(packet::AddressFamily family, const packet::IpAddress& address, std::string* output) {
    char text[INET6_ADDRSTRLEN]{};
    const void* bytes = nullptr;
    int system_family = AF_UNSPEC;
    if (family == packet::AddressFamily::kIPv4) {
        const auto* value = std::get_if<packet::IPv4Address>(&address);
        if (!value) return false;
        bytes = value->bytes;
        system_family = AF_INET;
    } else if (family == packet::AddressFamily::kIPv6) {
        const auto* value = std::get_if<packet::IPv6Address>(&address);
        if (!value) return false;
        bytes = value->bytes;
        system_family = AF_INET6;
    } else {
        return false;
    }
    if (!inet_ntop(system_family, bytes, text, sizeof(text))) return false;
    *output = text;
    return true;
}

}  // namespace

NpmDnsProtocolModuleV1::NpmDnsProtocolModuleV1(NpmDnsConfigV1 config, std::shared_ptr<INpmTaskBudget> budget)
    : budget_(std::move(budget)), transactions_(std::move(config), budget_), framer_(&transactions_, budget_) {}

NpmDnsProtocolModuleV1::~NpmDnsProtocolModuleV1() { Abort(); }

int NpmDnsProtocolModuleV1::RememberSession(const NpmSessionView& session) {
    if (!budget_ || !session.key || session.session_id == 0) return EINVAL;
    if (identities_.find(session.session_id) != identities_.end()) return 0;
    if (budget_->Reserve(NpmBudgetCategory::kModuleState, kSessionIdentityCharge) != NpmBudgetError::kNone)
        return ENOSPC;
    try {
        NpmDnsSessionIdentityV1 identity;
        identity.session_id = session.session_id;
        identity.observation_domain_id = session.key->observation_domain_id;
        identity.transport_protocol = session.key->transport_protocol;
        identity.a_port = session.key->a.port;
        identity.b_port = session.key->b.port;
        if ((identity.transport_protocol != 6 && identity.transport_protocol != 17) ||
            !FormatAddress(session.key->ip_family, session.key->a.ip, &identity.a_ip) ||
            !FormatAddress(session.key->ip_family, session.key->b.ip, &identity.b_ip)) {
            budget_->Release(NpmBudgetCategory::kModuleState, kSessionIdentityCharge);
            return EINVAL;
        }
        identities_.emplace(session.session_id, std::move(identity));
        return 0;
    } catch (const std::bad_alloc&) {
        budget_->Release(NpmBudgetCategory::kModuleState, kSessionIdentityCharge);
        return ENOMEM;
    }
}

void NpmDnsProtocolModuleV1::ForgetSession(uint64_t session_id) noexcept {
    if (identities_.erase(session_id)) budget_->Release(NpmBudgetCategory::kModuleState, kSessionIdentityCharge);
}

int NpmDnsProtocolModuleV1::EmitRows(std::vector<NpmDnsUdpResultV1>* rows, INpmResultEmitterV1& emitter) {
    int result = 0;
    for (const auto& row : *rows) {
        const auto identity = identities_.find(row.session_id);
        if (identity == identities_.end()) {
            result = EINVAL;
            break;
        }
        result = EmitNpmDnsTransactionV1(row, identity->second, budget_, emitter);
        if (result) break;
    }
    transactions_.ReleaseResults(rows);
    return result;
}

int NpmDnsProtocolModuleV1::OnInput(const NpmInputEventV1& input, INpmResultEmitterV1& emitter) {
    if (finished_ || aborted_ || !input.session || !input.transport) return EINVAL;
    const int remembered = RememberSession(*input.session);
    if (remembered) return remembered;
    if (input.kind == NpmInputKindV1::kTcpPacket) return 0;
    if (input.kind != NpmInputKindV1::kUdpDatagram || input.session->key->transport_protocol != 17) return EINVAL;
    std::vector<NpmDnsUdpResultV1> rows;
    const int error = transactions_.OnDatagram(input.session->session_id, input.transport->direction, input.body,
                                               input.body_complete, input.packet.meta.timestamp_ns, &rows);
    if (error) {
        transactions_.ReleaseResults(&rows);
        return error;
    }
    return EmitRows(&rows, emitter);
}

int NpmDnsProtocolModuleV1::OnSessionSnapshot(const NpmSessionView&, int64_t, INpmResultEmitterV1&) { return 0; }

int NpmDnsProtocolModuleV1::OnSessionEnd(const NpmSessionView& session, NpmSessionEndReason, int64_t observed_at_ns,
                                         INpmResultEmitterV1& emitter) {
    if (finished_ || aborted_) return EINVAL;
    std::vector<NpmDnsUdpResultV1> rows;
    const int error = framer_.OnSessionEnd(session.session_id, observed_at_ns, &rows);
    if (error) {
        transactions_.ReleaseResults(&rows);
        return error;
    }
    const int emit_error = EmitRows(&rows, emitter);
    if (emit_error) return emit_error;
    ForgetSession(session.session_id);
    return 0;
}

std::optional<int64_t> NpmDnsProtocolModuleV1::NextEventDeadlineNs() const {
    return finished_ || aborted_ ? std::nullopt : transactions_.NextEventDeadlineNs();
}

int NpmDnsProtocolModuleV1::OnTime(const NpmModuleTimeV1& time, INpmResultEmitterV1& emitter) {
    if (finished_ || aborted_) return EINVAL;
    if (!time.watermark_ns) return 0;
    std::vector<NpmDnsUdpResultV1> rows;
    const int error = transactions_.OnCaptureWatermark(*time.watermark_ns, &rows);
    if (error) {
        transactions_.ReleaseResults(&rows);
        return error;
    }
    return EmitRows(&rows, emitter);
}

int NpmDnsProtocolModuleV1::Finish(int64_t, INpmResultEmitterV1&) {
    if (finished_ || aborted_) return EINVAL;
    if (!identities_.empty()) return EINVAL;
    finished_ = true;
    return 0;
}

void NpmDnsProtocolModuleV1::Abort() noexcept {
    if (aborted_) return;
    aborted_ = true;
    framer_.Abort();
    transactions_.Abort();
    while (!identities_.empty()) ForgetSession(identities_.begin()->first);
}

int NpmDnsProtocolModuleV1::OnTcpStreamReadable(const NpmTcpStreamContextV1& context, INpmTcpStreamCursorV1& cursor,
                                                INpmResultEmitterV1& emitter) {
    if (finished_ || aborted_ || !context.session) return EINVAL;
    const int remembered = RememberSession(*context.session);
    if (remembered) return remembered;
    std::vector<NpmDnsUdpResultV1> rows;
    const int error = framer_.OnReadable(context, cursor, &rows);
    if (error) {
        transactions_.ReleaseResults(&rows);
        return error;
    }
    return EmitRows(&rows, emitter);
}

}  // namespace flowsql::npm
