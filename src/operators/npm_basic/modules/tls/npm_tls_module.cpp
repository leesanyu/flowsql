// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_tls_module.h"

#include <arpa/inet.h>

#include <algorithm>
#include <cerrno>
#include <new>
#include <utility>

namespace flowsql::npm {
namespace {

constexpr uint64_t kIdentityCharge = 512;
constexpr uint64_t kTrackerBaseCharge = 64 * 1024;
constexpr uint64_t kMaxParseChunk = 256;

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

NpmTlsProtocolModuleV1::NpmTlsProtocolModuleV1(NpmTlsConfigV1 config, std::shared_ptr<INpmTaskBudget> budget)
    : config_(std::move(config)), budget_(std::move(budget)) {}

NpmTlsProtocolModuleV1::~NpmTlsProtocolModuleV1() { Abort(); }

int NpmTlsProtocolModuleV1::RememberSession(const NpmSessionView& session) {
    if (!budget_ || !session.key || !session.session_id) return EINVAL;
    if (sessions_.find(session.session_id) != sessions_.end()) return 0;
    const uint64_t bytes = kIdentityCharge + kTrackerBaseCharge + 8ULL * config_.max_hello_bytes;
    if (budget_->Reserve(NpmBudgetCategory::kModuleState, bytes) != NpmBudgetError::kNone) return ENOSPC;
    try {
        SessionState state;
        state.identity.session_id = session.session_id;
        state.identity.observation_domain_id = session.key->observation_domain_id;
        state.identity.a_port = session.key->a.port;
        state.identity.b_port = session.key->b.port;
        if (session.key->transport_protocol != 6 ||
            !FormatAddress(session.key->ip_family, session.key->a.ip, &state.identity.a_ip) ||
            !FormatAddress(session.key->ip_family, session.key->b.ip, &state.identity.b_ip)) {
            budget_->Release(NpmBudgetCategory::kModuleState, bytes);
            return EINVAL;
        }
        state.tracker = std::make_unique<NpmTlsHandshakeTrackerV1>(session.session_id, config_);
        state.reserved_bytes = bytes;
        sessions_.emplace(session.session_id, std::move(state));
        return 0;
    } catch (const std::bad_alloc&) {
        budget_->Release(NpmBudgetCategory::kModuleState, bytes);
        return ENOMEM;
    }
}

void NpmTlsProtocolModuleV1::ReleaseTracker(SessionState& state) noexcept {
    if (!state.tracker) return;
    state.tracker->Abort();
    state.tracker.reset();
    budget_->Release(NpmBudgetCategory::kModuleState, state.reserved_bytes - kIdentityCharge);
    state.reserved_bytes = kIdentityCharge;
}

void NpmTlsProtocolModuleV1::ForgetSession(uint64_t session_id) noexcept {
    const auto found = sessions_.find(session_id);
    if (found == sessions_.end()) return;
    if (found->second.tracker) ReleaseTracker(found->second);
    budget_->Release(NpmBudgetCategory::kModuleState, found->second.reserved_bytes);
    sessions_.erase(found);
}

int NpmTlsProtocolModuleV1::EmitResult(SessionState& state, std::optional<NpmTlsHandshakeResultV1>* result,
                                       INpmResultEmitterV1& emitter) {
    if (!*result) return 0;
    result->value().handshake.entity_instance_id = state.identity.session_id;
    const int error = EmitNpmTlsHandshakeV1(**result, state.identity, budget_, emitter);
    result->reset();
    ReleaseTracker(state);
    return error;
}

int NpmTlsProtocolModuleV1::OnInput(const NpmInputEventV1& input, INpmResultEmitterV1&) {
    return finished_ || aborted_ || input.kind != NpmInputKindV1::kTcpPacket || !input.session || !input.transport
               ? EINVAL
               : 0;
}

int NpmTlsProtocolModuleV1::OnSessionSnapshot(const NpmSessionView&, int64_t, INpmResultEmitterV1&) { return 0; }

int NpmTlsProtocolModuleV1::OnSessionEnd(const NpmSessionView& session, NpmSessionEndReason reason,
                                         int64_t observed_at_ns, INpmResultEmitterV1& emitter) {
    if (finished_ || aborted_) return EINVAL;
    const auto found = sessions_.find(session.session_id);
    if (found == sessions_.end()) return 0;
    int error = 0;
    if (found->second.tracker) {
        std::optional<NpmTlsHandshakeResultV1> result;
        const auto tracked = found->second.tracker->OnSessionEnd(reason, observed_at_ns, &result);
        error = tracked == NpmTlsTrackerErrorV1::kNone ? EmitResult(found->second, &result, emitter) : EINVAL;
    }
    ForgetSession(session.session_id);
    return error;
}

std::optional<int64_t> NpmTlsProtocolModuleV1::NextEventDeadlineNs() const {
    std::optional<int64_t> minimum;
    if (finished_ || aborted_) return minimum;
    for (const auto& item : sessions_) {
        if (!item.second.tracker) continue;
        const auto deadline = item.second.tracker->NextEventDeadlineNs();
        if (deadline && (!minimum || *deadline < *minimum)) minimum = *deadline;
    }
    return minimum;
}

int NpmTlsProtocolModuleV1::OnTime(const NpmModuleTimeV1& time, INpmResultEmitterV1& emitter) {
    if (finished_ || aborted_) return EINVAL;
    if (!time.watermark_ns) return 0;
    for (auto& item : sessions_) {
        auto& state = item.second;
        if (!state.tracker) continue;
        std::optional<NpmTlsHandshakeResultV1> result;
        const auto tracked = state.tracker->OnCaptureWatermark(*time.watermark_ns, &result);
        if (tracked != NpmTlsTrackerErrorV1::kNone) return EINVAL;
        const int error = EmitResult(state, &result, emitter);
        if (error) return error;
    }
    return 0;
}

int NpmTlsProtocolModuleV1::Finish(int64_t, INpmResultEmitterV1&) {
    if (finished_ || aborted_ || !sessions_.empty()) return EINVAL;
    finished_ = true;
    return 0;
}

void NpmTlsProtocolModuleV1::Abort() noexcept {
    if (aborted_) return;
    aborted_ = true;
    while (!sessions_.empty()) ForgetSession(sessions_.begin()->first);
}

int NpmTlsProtocolModuleV1::OnTcpStreamReadable(const NpmTcpStreamContextV1& context, INpmTcpStreamCursorV1& cursor,
                                                INpmResultEmitterV1& emitter) {
    if (finished_ || aborted_ || !context.session) return EINVAL;
    const uint64_t session_id = context.session->session_id;
    NpmTcpStreamEventV1 event;
    while (cursor.Peek(&event)) {
        auto found = sessions_.find(session_id);
        if (found == sessions_.end() && context.origin == NpmTcpStreamOriginV1::kSyn &&
            event.kind == NpmTcpStreamEventKindV1::kData) {
            const int remembered = RememberSession(*context.session);
            if (remembered) return remembered;
            found = sessions_.find(session_id);
        }
        const uint64_t consumed_bytes =
            event.kind == NpmTcpStreamEventKindV1::kData ? std::min<uint64_t>(event.bytes.size, kMaxParseChunk) : 0;
        if (found != sessions_.end() && found->second.tracker) {
            NpmTcpStreamEventV1 slice = event;
            if (consumed_bytes && consumed_bytes != event.bytes.size) {
                slice.bytes.size = consumed_bytes;
                slice.end = slice.begin + consumed_bytes;
            }
            std::optional<NpmTlsHandshakeResultV1> result;
            const auto tracked = found->second.tracker->Consume(context, slice, &result);
            if (tracked == NpmTlsTrackerErrorV1::kAllocationFailed) return ENOMEM;
            if (tracked != NpmTlsTrackerErrorV1::kNone) return EINVAL;
            const int consumed = cursor.Consume(consumed_bytes);
            if (consumed) return consumed;
            const int emitted = EmitResult(found->second, &result, emitter);
            if (emitted) return emitted;
            if (found->second.tracker && (found->second.tracker->Closed() || found->second.tracker->Rejected()))
                ReleaseTracker(found->second);
        } else {
            const int consumed = cursor.Consume(consumed_bytes);
            if (consumed) return consumed;
        }
    }
    return 0;
}

}  // namespace flowsql::npm
