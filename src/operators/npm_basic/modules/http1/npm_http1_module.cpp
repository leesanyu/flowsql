// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_http1_module.h"

#include <cerrno>
#include <new>
#include <utility>

namespace flowsql::npm {
namespace {

constexpr uint64_t kSessionIdentityCharge = sizeof(NpmTransactionIdentity) + 6 * sizeof(void*);

}  // namespace

NpmHttp1ProtocolModuleV1::NpmHttp1ProtocolModuleV1(NpmHttp1ConfigV1 config, std::shared_ptr<INpmTaskBudget> budget)
    : budget_(std::move(budget)), transactions_(std::move(config), budget_) {}

NpmHttp1ProtocolModuleV1::~NpmHttp1ProtocolModuleV1() { Abort(); }

int NpmHttp1ProtocolModuleV1::RememberSession(const NpmSessionView& session) {
    if (!budget_ || !session.key || session.session_id == 0) return EINVAL;
    if (identities_.find(session.session_id) != identities_.end()) return 0;
    const uint64_t charge = kSessionIdentityCharge + session.key->OwnedKeyBytes();
    if (budget_->Reserve(NpmBudgetCategory::kModuleState, charge) != NpmBudgetError::kNone) return ENOSPC;
    try {
        if (session.key->Protocol() != 6) {
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

void NpmHttp1ProtocolModuleV1::ForgetSession(uint64_t session_id) noexcept {
    const auto found = identities_.find(session_id);
    if (found == identities_.end()) return;
    budget_->Release(NpmBudgetCategory::kModuleState, found->second.charge);
    identities_.erase(found);
}

int NpmHttp1ProtocolModuleV1::EmitRows(std::vector<NpmHttp1TransactionResultV1>* rows, INpmResultEmitterV1& emitter) {
    int error = 0;
    for (auto& row : *rows) {
        const auto identity = identities_.find(row.session_id);
        if (identity == identities_.end()) {
            error = EINVAL;
            break;
        }
        auto emit = [&](auto family_result, const auto& key) {
            family_result.a_ip = key.a_ip;
            family_result.b_ip = key.b_ip;
            family_result.observation_domain_id = key.observation_domain_id;
            family_result.a_port = key.a_port;
            family_result.b_port = key.b_port;

            family_result.transaction = std::move(row);
            return EmitNpmHttp1TransactionV1(family_result, budget_, emitter);
        };
        const auto& key = identity->second.key;
        error = key.Get4() ? emit(NpmHttp1Result4{}, *key.Get4()) : emit(NpmHttp1Result6{}, *key.Get6());
        if (error) break;
    }
    transactions_.ReleaseResults(rows);
    return error;
}

int NpmHttp1ProtocolModuleV1::OnInput(const NpmInputEventV1& input, INpmResultEmitterV1&) {
    if (finished_ || aborted_ || input.kind != NpmInputKindV1::kTcpPacket || !input.session || !input.transport)
        return EINVAL;
    return RememberSession(*input.session);
}

int NpmHttp1ProtocolModuleV1::OnSessionSnapshot(const NpmSessionView&, int64_t, INpmResultEmitterV1&) { return 0; }

int NpmHttp1ProtocolModuleV1::OnSessionEnd(const NpmSessionView& session, NpmSessionEndReason reason,
                                           int64_t observed_at_ns, INpmResultEmitterV1& emitter) {
    if (finished_ || aborted_) return EINVAL;
    std::vector<NpmHttp1TransactionResultV1> rows;
    const int error = transactions_.OnSessionEnd(session.session_id, reason, observed_at_ns, &rows);
    if (error) {
        transactions_.ReleaseResults(&rows);
        return error;
    }
    const int emit_error = EmitRows(&rows, emitter);
    if (emit_error) return emit_error;
    ForgetSession(session.session_id);
    return 0;
}

std::optional<int64_t> NpmHttp1ProtocolModuleV1::NextEventDeadlineNs() const {
    return finished_ || aborted_ ? std::nullopt : transactions_.NextEventDeadlineNs();
}

int NpmHttp1ProtocolModuleV1::OnTime(const NpmModuleTimeV1& time, INpmResultEmitterV1& emitter) {
    if (finished_ || aborted_) return EINVAL;
    std::vector<NpmHttp1TransactionResultV1> rows;
    const int error = transactions_.OnCaptureWatermark(time.watermark_ns, &rows);
    if (error) {
        transactions_.ReleaseResults(&rows);
        return error;
    }
    return EmitRows(&rows, emitter);
}

int NpmHttp1ProtocolModuleV1::Finish(int64_t, INpmResultEmitterV1&) {
    if (finished_ || aborted_ || !identities_.empty() || transactions_.ActiveSessions() != 0) return EINVAL;
    finished_ = true;
    return 0;
}

void NpmHttp1ProtocolModuleV1::Abort() noexcept {
    if (aborted_) return;
    aborted_ = true;
    transactions_.Abort();
    while (!identities_.empty()) ForgetSession(identities_.begin()->first);
}

int NpmHttp1ProtocolModuleV1::OnTcpStreamReadable(const NpmTcpStreamContextV1& context, INpmTcpStreamCursorV1& cursor,
                                                  INpmResultEmitterV1& emitter) {
    if (finished_ || aborted_ || !context.session) return EINVAL;
    const int remembered = RememberSession(*context.session);
    if (remembered) return remembered;
    NpmTcpStreamEventV1 event;
    while (cursor.Peek(&event)) {
        std::vector<NpmHttp1TransactionResultV1> rows;
        const int error = transactions_.OnStreamEvent(context.session->session_id, context.direction, context.origin,
                                                      event, context.observed_at_ns, &rows);
        if (error) {
            transactions_.ReleaseResults(&rows);
            return error;
        }
        const int consumed = cursor.Consume(event.kind == NpmTcpStreamEventKindV1::kData ? event.bytes.size : 0);
        if (consumed) {
            transactions_.ReleaseResults(&rows);
            return consumed;
        }
        const int emitted = EmitRows(&rows, emitter);
        if (emitted) return emitted;
    }
    return 0;
}

}  // namespace flowsql::npm
