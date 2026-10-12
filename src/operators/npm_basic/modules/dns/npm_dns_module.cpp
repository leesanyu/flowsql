// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_dns_module.h"

#include <cerrno>
#include <new>
#include <utility>

namespace flowsql::npm {
namespace {

constexpr uint64_t kSessionIdentityCharge = sizeof(NpmTransactionIdentity) + 6 * sizeof(void*);

}  // namespace

NpmDnsProtocolModuleV1::NpmDnsProtocolModuleV1(NpmDnsConfigV1 config, std::shared_ptr<INpmTaskBudget> budget)
    : budget_(std::move(budget)), transactions_(std::move(config), budget_), framer_(&transactions_, budget_) {}

NpmDnsProtocolModuleV1::~NpmDnsProtocolModuleV1() { Abort(); }

int NpmDnsProtocolModuleV1::RememberSession(const NpmSessionView& session) {
    if (!budget_ || !session.key || session.session_id == 0) return EINVAL;
    if (identities_.find(session.session_id) != identities_.end()) return 0;
    const uint64_t charge = kSessionIdentityCharge + session.key->OwnedKeyBytes();
    if (budget_->Reserve(NpmBudgetCategory::kModuleState, charge) != NpmBudgetError::kNone) return ENOSPC;
    try {
        if (session.key->Protocol() != 6 && session.key->Protocol() != 17) {
            budget_->Release(NpmBudgetCategory::kModuleState, charge);
            return EINVAL;
        }
        NpmTransactionIdentity identity;
        identity.session_id = session.session_id;
        identity.key = *session.key;
        identity.charge = charge;
        identities_.emplace(session.session_id, std::move(identity));
        return 0;
    } catch (const std::bad_alloc&) {
        budget_->Release(NpmBudgetCategory::kModuleState, charge);
        return ENOMEM;
    }
}

void NpmDnsProtocolModuleV1::ForgetSession(uint64_t session_id) noexcept {
    const auto found = identities_.find(session_id);
    if (found == identities_.end()) return;
    budget_->Release(NpmBudgetCategory::kModuleState, found->second.charge);
    identities_.erase(found);
}

int NpmDnsProtocolModuleV1::EmitRows(std::vector<NpmDnsUdpResultV1>* rows, INpmResultEmitterV1& emitter) {
    int result = 0;
    for (auto& row : *rows) {
        const auto identity = identities_.find(row.session_id);
        if (identity == identities_.end()) {
            result = EINVAL;
            break;
        }
        auto emit = [&](auto family_result, const auto& key) {
            family_result.a_ip = key.a_ip;
            family_result.b_ip = key.b_ip;
            family_result.observation_domain_id = key.observation_domain_id;
            family_result.a_port = key.a_port;
            family_result.b_port = key.b_port;
            family_result.transport_protocol = key.transport_protocol;
            family_result.transaction = std::move(row);
            return EmitNpmDnsTransactionV1(family_result, budget_, emitter);
        };
        const auto& key = identity->second.key;
        result = key.Get4() ? emit(NpmDnsResult4{}, *key.Get4()) : emit(NpmDnsResult6{}, *key.Get6());
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
    if (input.kind != NpmInputKindV1::kUdpDatagram || input.session->key->Protocol() != 17) return EINVAL;
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
