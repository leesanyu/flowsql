// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_tcp_stream_provider.h"

#include "npm_module_catalog.h"

#include <algorithm>
#include <cerrno>
#include <new>

namespace flowsql::npm {
namespace {
NpmSessionSnapshot CopySnapshot(const NpmSessionView& session) {
    NpmSessionSnapshot snapshot;
    snapshot.key = *session.key;
    snapshot.session_id = session.session_id;
    snapshot.first_ns = session.first_ns;
    snapshot.last_ns = session.last_ns;
    snapshot.packets_ab = session.packets_ab;
    snapshot.packets_ba = session.packets_ba;
    snapshot.wire_bytes_ab = session.wire_bytes_ab;
    snapshot.wire_bytes_ba = session.wire_bytes_ba;
    snapshot.primary_label_id = session.primary_label_id;
    snapshot.protocol_status = session.protocol_status;
    snapshot.protocol_id = session.protocol_id;
    snapshot.protocol_sub_id = session.protocol_sub_id;
    return snapshot;
}
}  // namespace

int NpmTcpStreamProvider::ConsumerGate::OnTcpStreamReadable(const NpmTcpStreamContextV1& context,
                                                            INpmTcpStreamCursorV1& cursor,
                                                            INpmResultEmitterV1& emitter) {
    if (cancellation_requested && cancellation_requested->load(std::memory_order_acquire)) return ECANCELED;
    const int error = target->OnTcpStreamReadable(context, cursor, emitter);
    return error == 0 && cancellation_requested && cancellation_requested->load(std::memory_order_acquire) ? ECANCELED
                                                                                                           : error;
}

NpmTcpStreamProvider::NpmTcpStreamProvider(NpmTcpStreamConfigV1 config, std::shared_ptr<INpmTaskBudget> budget,
                                           const std::atomic<bool>* cancellation_requested)
    : config_(config),
      budget_(std::move(budget)),
      charged_(sizeof(*this)),
      cancellation_requested_(cancellation_requested) {}

NpmTcpStreamError NpmTcpStreamProvider::Create(NpmTcpStreamConfigV1 config, std::shared_ptr<INpmTaskBudget> budget,
                                               Span<const NpmTcpStreamRegistration> registrations,
                                               std::unique_ptr<NpmTcpStreamProvider>* output,
                                               const std::atomic<bool>* cancellation_requested) {
    if (!output || *output || !budget || (registrations.size && !registrations.data)) {
        return NpmTcpStreamError::kInvalidInput;
    }
    if (config.max_buffered_bytes_per_direction < kNpmMinTcpStreamBufferedBytesPerDirection ||
        config.max_buffered_bytes_per_direction > kNpmMaxTcpStreamBufferedBytesPerDirection ||
        config.gap_timeout_ns < kNpmMinTcpStreamGapTimeoutNs || config.gap_timeout_ns > kNpmMaxTcpStreamGapTimeoutNs) {
        return NpmTcpStreamError::kInvalidConfig;
    }
    if (!registrations.size) return NpmTcpStreamError::kNone;
    for (size_t i = 0; i < registrations.size; ++i) {
        const auto& registration = registrations.data[i];
        if (!registration.adapter || !registration.consumer || !registration.adapter->Plan().requires_tcp_stream ||
            !registration.adapter->Plan().primary_label_ids) {
            return NpmTcpStreamError::kInvalidInput;
        }
    }
    if (budget->Reserve(NpmBudgetCategory::kModuleState, sizeof(NpmTcpStreamProvider)) != NpmBudgetError::kNone) {
        return NpmTcpStreamError::kTaskBudgetExceeded;
    }
    std::unique_ptr<NpmTcpStreamProvider> provider;
    try {
        provider.reset(new NpmTcpStreamProvider(config, budget, cancellation_requested));
        provider->Initialize(registrations);
        *output = std::move(provider);
        return NpmTcpStreamError::kNone;
    } catch (NpmTcpStreamError error) {
        // The provider destructor returns all charges made after its object reservation.
        return error;
    } catch (const std::bad_alloc&) {
        if (!provider) budget->Release(NpmBudgetCategory::kModuleState, sizeof(NpmTcpStreamProvider));
        return NpmTcpStreamError::kAllocationFailed;
    }
}

NpmTcpStreamProvider::~NpmTcpStreamProvider() { Abort(); }

void NpmTcpStreamProvider::Reserve(uint64_t bytes) {
    if (budget_->Reserve(NpmBudgetCategory::kModuleState, bytes) != NpmBudgetError::kNone) {
        throw NpmTcpStreamError::kTaskBudgetExceeded;
    }
    charged_ += bytes;
}

void NpmTcpStreamProvider::Release(uint64_t bytes) noexcept {
    budget_->Release(NpmBudgetCategory::kModuleState, bytes);
    charged_ -= bytes;
}

void NpmTcpStreamProvider::Initialize(Span<const NpmTcpStreamRegistration> registrations) {
    Reserve(registrations.size *
            (sizeof(NpmTcpStreamRegistration) + sizeof(ConsumerGate) + sizeof(NpmTcpStreamSubscription)));
    registrations_.reserve(registrations.size);
    consumer_gates_.reserve(registrations.size);
    scratch_.reserve(registrations.size);
    registrations_.assign(registrations.data, registrations.data + registrations.size);
    for (const auto& registration : registrations_) {
        consumer_gates_.push_back({registration.consumer, cancellation_requested_});
    }
}

NpmTcpStreamProvider::Entries::iterator NpmTcpStreamProvider::AddSession(const NpmSessionView& session) {
    const uint64_t charge = sizeof(Entries::value_type) + 4 * sizeof(void*) + session.key->input_namespace.size() + 1;
    Reserve(charge);
    try {
        Entry entry;
        entry.snapshot = CopySnapshot(session);
        entry.charge = charge;
        return entries_.emplace(session.session_id, std::move(entry)).first;
    } catch (...) {
        Release(charge);
        throw;
    }
}

void NpmTcpStreamProvider::Refresh(Entry& entry, const NpmSessionView& session) noexcept {
    auto& snapshot = entry.snapshot;
    snapshot.last_ns = session.last_ns;
    snapshot.packets_ab = session.packets_ab;
    snapshot.packets_ba = session.packets_ba;
    snapshot.wire_bytes_ab = session.wire_bytes_ab;
    snapshot.wire_bytes_ba = session.wire_bytes_ba;
    snapshot.protocol_status = session.protocol_status;
    snapshot.protocol_id = session.protocol_id;
    snapshot.protocol_sub_id = session.protocol_sub_id;
}

void NpmTcpStreamProvider::Erase(Entries::iterator at) noexcept {
    const auto charge = at->second.charge;
    entries_.erase(at);
    Release(charge);
}

void NpmTcpStreamProvider::SaveFailure(const NpmTcpStreamSharedDirection& direction) noexcept {
    if (failure_.error == NpmTcpStreamError::kNone) failure_ = direction.Failure();
}

int NpmTcpStreamProvider::OnPacket(const NpmPacketView& packet, const NpmSessionView& session) {
    if (aborted_ || (cancellation_requested_ && cancellation_requested_->load(std::memory_order_acquire)))
        return ECANCELED;
    if (!session.key || session.key->transport_protocol != 6 || !packet.transport.tcp.valid) return 0;
    const auto& tcp = packet.transport.tcp;
    if (!tcp.syn && !tcp.fin && !tcp.rst && packet.transport.payload_wire_bytes == 0) return 0;
    scratch_.clear();
    for (size_t i = 0; i < registrations_.size(); ++i) {
        const auto& registration = registrations_[i];
        const auto& labels = *registration.adapter->Plan().primary_label_ids;
        if (session.primary_label_id != 0 &&
            std::find(labels.begin(), labels.end(), session.primary_label_id) != labels.end()) {
            scratch_.push_back(
                {registration.adapter->Plan().module_id, &consumer_gates_[i], registration.adapter->StreamEmitter()});
        }
    }
    if (scratch_.empty()) return 0;
    try {
        auto at = entries_.find(session.session_id);
        if (at == entries_.end()) at = AddSession(session);
        auto& entry = at->second;
        Refresh(entry, session);
        const size_t index = packet.direction == NpmPacketDirection::kAToB ? 0 : 1;
        if (entry.seen[index]) {
            if (!entry.directions[index]) return 0;
        } else {
            entry.seen[index] = true;
            NpmTcpStreamFailure failure;
            const auto error = NpmTcpStreamSharedDirection::Create(config_, budget_, session.session_id,
                                                                   packet.direction, {scratch_.data(), scratch_.size()},
                                                                   &entry.directions[index], &failure);
            if (error != NpmTcpStreamError::kNone) {
                failure_ = failure;
                return EIO;
            }
        }
        const auto view = entry.snapshot.View();
        const auto error = entry.directions[index]->Push(view, packet);
        if (error != NpmTcpStreamError::kNone) {
            SaveFailure(*entry.directions[index]);
            return EIO;
        }
        if (entry.directions[index]->Ended()) entry.directions[index].reset();
        return 0;
    } catch (NpmTcpStreamError error) {
        failure_ = {error, session.session_id, packet.direction, {}, {}};
        return EIO;
    } catch (const std::bad_alloc&) {
        failure_ = {NpmTcpStreamError::kAllocationFailed, session.session_id, packet.direction, {}, {}};
        return ENOMEM;
    }
}

int NpmTcpStreamProvider::OnSessionEnd(const NpmSessionSnapshot& session, int64_t observed_at_ns) {
    if (aborted_ || (cancellation_requested_ && cancellation_requested_->load(std::memory_order_acquire)))
        return ECANCELED;
    auto at = entries_.find(session.session_id);
    if (at == entries_.end()) return 0;
    for (size_t index = 0; index < 2; ++index) {
        auto& stream = at->second.directions[index];
        if (!stream) continue;
        const auto error =
            stream->End(session.View(), NpmTcpStreamEndReasonV1::kSessionEnd, session.end_reason, observed_at_ns);
        if (error != NpmTcpStreamError::kNone) {
            SaveFailure(*stream);
            return EIO;
        }
    }
    Erase(at);
    return 0;
}

int NpmTcpStreamProvider::AdvanceWatermark(int64_t watermark_ns) {
    if (aborted_ || (cancellation_requested_ && cancellation_requested_->load(std::memory_order_acquire)))
        return ECANCELED;
    for (auto& [id, entry] : entries_) {
        for (auto& stream : entry.directions) {
            if (!stream) continue;
            const auto error = stream->AdvanceWatermark(entry.snapshot.View(), watermark_ns);
            if (error != NpmTcpStreamError::kNone) {
                SaveFailure(*stream);
                return EIO;
            }
            if (stream->Ended()) stream.reset();
        }
    }
    return 0;
}

std::optional<int64_t> NpmTcpStreamProvider::NextEventDeadlineNs() const {
    std::optional<int64_t> next;
    for (const auto& [id, entry] : entries_) {
        for (const auto& stream : entry.directions) {
            if (!stream) continue;
            const auto deadline = stream->NextEventDeadlineNs();
            if (deadline && (!next || *deadline < *next)) next = deadline;
        }
    }
    return next;
}

size_t NpmTcpStreamProvider::ActiveDirections() const noexcept {
    size_t count = 0;
    for (const auto& [id, entry] : entries_) {
        count += static_cast<size_t>(entry.directions[0] != nullptr);
        count += static_cast<size_t>(entry.directions[1] != nullptr);
    }
    return count;
}

void NpmTcpStreamProvider::Abort() noexcept {
    if (aborted_) return;
    aborted_ = true;
    for (auto& [id, entry] : entries_) {
        for (auto& stream : entry.directions) {
            if (stream) stream->Abort();
        }
    }
    entries_.clear();
    registrations_.clear();
    consumer_gates_.clear();
    scratch_.clear();
    if (charged_) Release(charged_);
}

}  // namespace flowsql::npm
