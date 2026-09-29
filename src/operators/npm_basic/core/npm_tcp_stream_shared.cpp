// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_tcp_stream_shared.h"

#include <cerrno>
#include <limits>
#include <new>

namespace flowsql::npm {

NpmTcpStreamSharedDirection::Budget::Budget(std::shared_ptr<INpmTaskBudget> task_budget, uint64_t direction_limit)
    : task(std::move(task_budget)), limit(direction_limit), used(sizeof(NpmTcpStreamSharedDirection)) {}

NpmBudgetError NpmTcpStreamSharedDirection::Budget::Reserve(NpmBudgetCategory category, uint64_t bytes) {
    if (bytes > limit - used) {
        error = NpmTcpStreamError::kDirectionLimitExceeded;
        return NpmBudgetError::kTrackedLimitExceeded;
    }
    const auto result = task->Reserve(category, bytes);
    if (result != NpmBudgetError::kNone) {
        error = NpmTcpStreamError::kTaskBudgetExceeded;
        return result;
    }
    used += bytes;
    return NpmBudgetError::kNone;
}

NpmBudgetError NpmTcpStreamSharedDirection::Budget::Release(NpmBudgetCategory category, uint64_t bytes) {
    const auto result = task->Release(category, bytes);
    if (result == NpmBudgetError::kNone) used -= bytes;
    return result;
}

NpmBudgetUsage NpmTcpStreamSharedDirection::Budget::Usage() const { return task->Usage(); }

class NpmTcpStreamSharedDirection::Cursor final : public INpmTcpStreamCursorV1 {
 public:
    Cursor(NpmTcpStreamSharedDirection& owner, Reader& reader) : owner_(owner), reader_(reader) {}
    bool Peek(NpmTcpStreamEventV1* event) const override {
        if (!event) owner_.Latch(NpmTcpStreamError::kInvalidConsume, reader_.subscription.module_id);
        if (owner_.failure_.error != NpmTcpStreamError::kNone || !reader_.event) return false;
        *event = reader_.event->view;
        if (event->kind == NpmTcpStreamEventKindV1::kData) {
            event->begin += reader_.offset;
            event->bytes.data += reader_.offset;
            event->bytes.size -= reader_.offset;
        }
        return true;
    }
    int Consume(uint64_t bytes) override {
        if (owner_.failure_.error != NpmTcpStreamError::kNone) return EIO;
        auto* event = reader_.event;
        const bool data = event && event->view.kind == NpmTcpStreamEventKindV1::kData;
        if (!event || (data ? (bytes == 0 || bytes > event->view.bytes.size - reader_.offset) : bytes != 0)) {
            owner_.Latch(NpmTcpStreamError::kInvalidConsume, reader_.subscription.module_id);
            return EINVAL;
        }
        reader_.offset += bytes;
        if (data && reader_.offset < event->view.bytes.size) return 0;
        reader_.end_seen = event->view.kind == NpmTcpStreamEventKindV1::kEnd;
        reader_.event = event->next;
        reader_.offset = 0;
        --event->remaining;
        owner_.Reclaim();
        return 0;
    }

 private:
    NpmTcpStreamSharedDirection& owner_;
    Reader& reader_;
};

class NpmTcpStreamSharedDirection::Emitter final : public INpmResultEmitterV1 {
 public:
    Emitter(NpmTcpStreamSharedDirection& owner, Reader& reader) : owner_(owner), reader_(reader) {}
    int Emit(std::string_view entity_id, const arrow::RecordBatch& rows) override {
        if (owner_.failure_.error != NpmTcpStreamError::kNone) return EIO;
        int result = EIO;
        try {
            result = reader_.subscription.emitter->Emit(entity_id, rows);
            if (result != 0) owner_.Latch(NpmTcpStreamError::kEmitterError, reader_.subscription.module_id);
        } catch (const std::bad_alloc&) {
            owner_.Latch(NpmTcpStreamError::kAllocationFailed, reader_.subscription.module_id);
        } catch (...) {
            owner_.Latch(NpmTcpStreamError::kEmitterError, reader_.subscription.module_id);
        }
        return owner_.failure_.error == NpmTcpStreamError::kNone ? result : EIO;
    }

 private:
    NpmTcpStreamSharedDirection& owner_;
    Reader& reader_;
};

NpmTcpStreamError NpmTcpStreamSharedDirection::Create(NpmTcpStreamConfigV1 config,
                                                      std::shared_ptr<INpmTaskBudget> budget, uint64_t session_id,
                                                      NpmPacketDirection direction,
                                                      Span<const NpmTcpStreamSubscription> subscriptions,
                                                      std::unique_ptr<NpmTcpStreamSharedDirection>* output,
                                                      NpmTcpStreamFailure* failure) {
    auto report = [&](NpmTcpStreamError error) {
        if (failure) *failure = {error, session_id, direction, {}, {}};
        return error;
    };
    if (!output || *output || !budget || (subscriptions.size && !subscriptions.data) ||
        direction > NpmPacketDirection::kBToA) {
        return report(NpmTcpStreamError::kInvalidInput);
    }
    if (config.max_buffered_bytes_per_direction < kNpmMinTcpStreamBufferedBytesPerDirection ||
        config.max_buffered_bytes_per_direction > kNpmMaxTcpStreamBufferedBytesPerDirection ||
        config.gap_timeout_ns < kNpmMinTcpStreamGapTimeoutNs || config.gap_timeout_ns > kNpmMaxTcpStreamGapTimeoutNs) {
        return report(NpmTcpStreamError::kInvalidConfig);
    }
    for (size_t i = 0; i < subscriptions.size; ++i) {
        const auto& sub = subscriptions.data[i];
        if (sub.module_id.empty() || !sub.consumer || !sub.emitter) return report(NpmTcpStreamError::kInvalidInput);
        for (size_t j = 0; j < i; ++j) {
            if (sub.module_id == subscriptions.data[j].module_id || sub.consumer == subscriptions.data[j].consumer) {
                return report(NpmTcpStreamError::kInvalidInput);
            }
        }
    }
    if (!subscriptions.size) return report(NpmTcpStreamError::kNone);
    if (budget->Reserve(NpmBudgetCategory::kModuleState, sizeof(NpmTcpStreamSharedDirection)) !=
        NpmBudgetError::kNone) {
        return report(NpmTcpStreamError::kTaskBudgetExceeded);
    }
    std::unique_ptr<NpmTcpStreamSharedDirection> stream;
    try {
        stream.reset(new NpmTcpStreamSharedDirection(config, budget, session_id, direction));
        stream->Initialize(config, subscriptions);
    } catch (NpmTcpStreamError error) {
        stream->Latch(error);
    } catch (const std::bad_alloc&) {
        if (!stream) {
            budget->Release(NpmBudgetCategory::kModuleState, sizeof(NpmTcpStreamSharedDirection));
            return report(NpmTcpStreamError::kAllocationFailed);
        }
        stream->Latch(NpmTcpStreamError::kAllocationFailed);
    }
    if (failure) *failure = stream->failure_;
    const auto result = stream->failure_.error;
    if (result == NpmTcpStreamError::kNone) *output = std::move(stream);
    return result;
}

NpmTcpStreamSharedDirection::NpmTcpStreamSharedDirection(NpmTcpStreamConfigV1 config,
                                                         std::shared_ptr<INpmTaskBudget> budget, uint64_t session_id,
                                                         NpmPacketDirection direction)
    : budget_(std::move(budget), config.max_buffered_bytes_per_direction),
      failure_{NpmTcpStreamError::kNone, session_id, direction, {}, {}} {}

void NpmTcpStreamSharedDirection::Initialize(NpmTcpStreamConfigV1 config,
                                             Span<const NpmTcpStreamSubscription> subscriptions) {
    if (subscriptions.size > std::numeric_limits<uint64_t>::max() / sizeof(Reader)) {
        throw NpmTcpStreamError::kDirectionLimitExceeded;
    }
    Reserve(subscriptions.size * sizeof(Reader));
    readers_ = std::make_unique<Reader[]>(subscriptions.size);
    reader_count_ = subscriptions.size;
    for (size_t i = 0; i < reader_count_; ++i) readers_[i].subscription = subscriptions.data[i];
    core_.emplace(config, budget_);
    if (core_->Status() != NpmTcpStreamError::kNone) throw core_->Status();
}

NpmTcpStreamSharedDirection::~NpmTcpStreamSharedDirection() { Clear(); }

void NpmTcpStreamSharedDirection::Reserve(uint64_t bytes) {
    Reclaim();
    if (budget_.Reserve(NpmBudgetCategory::kModuleState, bytes) != NpmBudgetError::kNone) throw budget_.error;
}

void NpmTcpStreamSharedDirection::Latch(NpmTcpStreamError error, std::string_view consumer) noexcept {
    if (failure_.error != NpmTcpStreamError::kNone) return;
    failure_.error = error;
    failure_.failing_consumer = consumer;
    const Reader* earliest = nullptr;
    for (size_t i = 0; i < reader_count_; ++i) {
        const auto& reader = readers_[i];
        if (!reader.event) continue;
        if (!earliest || reader.event->view.begin + reader.offset < earliest->event->view.begin + earliest->offset) {
            earliest = &reader;
        }
    }
    if (earliest) failure_.retaining_consumer = earliest->subscription.module_id;
}

int NpmTcpStreamSharedDirection::Emit(const NpmTcpStreamEventV1& event) {
    std::unique_ptr<uint8_t[]> empty;
    return EmitOwned(event, empty, 0);
}

int NpmTcpStreamSharedDirection::EmitOwned(const NpmTcpStreamEventV1& event, std::unique_ptr<uint8_t[]>& data,
                                           uint64_t allocated_bytes) {
    Reserve(sizeof(Event));
    auto node = std::make_unique<Event>();
    node->view = event;
    node->remaining = reader_count_;
    node->allocated_bytes = allocated_bytes;
    node->data = std::move(data);
    if (tail_)
        tail_->next = node.get();
    else
        head_ = node.get();
    for (size_t i = 0; i < reader_count_; ++i) {
        if (!readers_[i].event) readers_[i].event = node.get();
    }
    tail_ = node.release();
    return 0;
}

void NpmTcpStreamSharedDirection::Reclaim() noexcept {
    while (head_ && head_->remaining == 0) {
        auto* node = head_;
        head_ = node->next;
        const auto charge = sizeof(Event) + node->allocated_bytes;
        delete node;
        budget_.Release(NpmBudgetCategory::kModuleState, charge);
    }
    if (!head_) tail_ = nullptr;
}

void NpmTcpStreamSharedDirection::Clear() noexcept {
    core_.reset();
    while (head_) {
        auto* node = head_;
        head_ = node->next;
        delete node;
    }
    tail_ = nullptr;
    readers_.reset();
    reader_count_ = 0;
    // Also releases reservations whose allocation failed before publication into the container.
    if (budget_.used) budget_.Release(NpmBudgetCategory::kModuleState, budget_.used);
}

bool NpmTcpStreamSharedDirection::Begin(const NpmSessionView& session) {
    if (operating_) {
        Latch(NpmTcpStreamError::kReentrantCall);
        return false;
    }
    if (failure_.error != NpmTcpStreamError::kNone || ended_) return false;
    if (session.session_id != failure_.session_id) {
        Latch(NpmTcpStreamError::kInvalidInput);
        Clear();
        return false;
    }
    operating_ = true;
    Reclaim();
    return true;
}

void NpmTcpStreamSharedDirection::Notify(const NpmSessionView& session, int64_t observed_at_ns) {
    NpmTcpStreamContextV1 context;
    context.session = &session;
    context.direction = failure_.direction;
    context.origin = core_->Origin();
    context.capture_truncation_seen = core_->CaptureTruncationSeen();
    context.final_drain = core_->Ended();
    context.observed_at_ns = observed_at_ns;
    for (size_t i = 0; i < reader_count_ && failure_.error == NpmTcpStreamError::kNone; ++i) {
        auto& reader = readers_[i];
        if (!reader.event) continue;
        Cursor cursor(*this, reader);
        Emitter emitter(*this, reader);
        try {
            if (reader.subscription.consumer->OnTcpStreamReadable(context, cursor, emitter) != 0) {
                Latch(NpmTcpStreamError::kConsumerError, reader.subscription.module_id);
            }
        } catch (const std::bad_alloc&) {
            Latch(NpmTcpStreamError::kAllocationFailed, reader.subscription.module_id);
        } catch (...) {
            Latch(NpmTcpStreamError::kConsumerError, reader.subscription.module_id);
        }
        if (context.final_drain && !reader.end_seen)
            Latch(NpmTcpStreamError::kNotDrained, reader.subscription.module_id);
    }
}

NpmTcpStreamError NpmTcpStreamSharedDirection::Complete(NpmTcpStreamError core_error, const NpmSessionView& session,
                                                        int64_t observed_at_ns) {
    if (core_error != NpmTcpStreamError::kNone) {
        Latch(budget_.error == NpmTcpStreamError::kNone ? core_error : budget_.error);
    }
    if (failure_.error == NpmTcpStreamError::kNone) Notify(session, observed_at_ns);
    if (failure_.error == NpmTcpStreamError::kNone) ended_ = core_->Ended();
    operating_ = false;
    if (failure_.error != NpmTcpStreamError::kNone || ended_) Clear();
    return failure_.error;
}

NpmTcpStreamError NpmTcpStreamSharedDirection::Push(const NpmSessionView& session, const NpmPacketView& packet) {
    if (!Begin(session)) return failure_.error;
    if (packet.direction != failure_.direction) {
        return Complete(NpmTcpStreamError::kInvalidInput, session, packet.packet.meta.timestamp_ns);
    }
    return Complete(core_->Push(packet, *this), session, packet.packet.meta.timestamp_ns);
}

NpmTcpStreamError NpmTcpStreamSharedDirection::AdvanceWatermark(const NpmSessionView& session, int64_t watermark_ns) {
    if (!Begin(session)) return failure_.error;
    if (watermark_ && watermark_ns <= *watermark_) {
        operating_ = false;
        return failure_.error;
    }
    watermark_ = watermark_ns;
    return Complete(core_->AdvanceWatermark(watermark_ns, *this), session, watermark_ns);
}

NpmTcpStreamError NpmTcpStreamSharedDirection::End(const NpmSessionView& session, NpmTcpStreamEndReasonV1 reason,
                                                   std::optional<NpmSessionEndReason> session_reason,
                                                   int64_t observed_at_ns) {
    if (!Begin(session)) return failure_.error;
    return Complete(core_->End(reason, session_reason, *this), session, observed_at_ns);
}

void NpmTcpStreamSharedDirection::Abort() noexcept {
    if (!ended_) Latch(NpmTcpStreamError::kAborted);
    if (!operating_) Clear();  // A reentrant cancellation must not free the active callback's borrowed cursor.
}

std::optional<int64_t> NpmTcpStreamSharedDirection::NextEventDeadlineNs() const {
    return core_ ? core_->NextEventDeadlineNs() : std::optional<int64_t>{};
}

}  // namespace flowsql::npm
