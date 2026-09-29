// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef FLOWSQL_OPERATORS_NPM_BASIC_CORE_NPM_TCP_STREAM_SHARED_H_
#define FLOWSQL_OPERATORS_NPM_BASIC_CORE_NPM_TCP_STREAM_SHARED_H_

#include "npm_tcp_stream_direction.h"

#include <operators/npm_basic/npm_protocol_contract.h>

namespace flowsql::npm {

/** Names and consumers belong to the frozen task catalog and outlive the direction and its failure report. */
struct NpmTcpStreamSubscription {
    std::string_view module_id;
    INpmTcpStreamConsumerV1* consumer = nullptr;
    INpmResultEmitterV1* emitter = nullptr;
};

struct NpmTcpStreamFailure {
    NpmTcpStreamError error = NpmTcpStreamError::kNone;
    uint64_t session_id = 0;
    NpmPacketDirection direction = NpmPacketDirection::kAToB;
    std::string_view retaining_consumer;  // First consumer at the earliest retained event/byte.
    std::string_view failing_consumer;
};

/** One admitted session/direction. Calls are serialized by the runtime operation gate; no external call holds a lock.
 * Session views are borrowed per operation, never retained. The provider owns metadata needed on the time path.
 * Create reserves object storage before allocation; no subscriptions produces no object or budget charge.
 */
class NpmTcpStreamSharedDirection final : private INpmTcpStreamEventSink {
 public:
    static NpmTcpStreamError Create(NpmTcpStreamConfigV1 config, std::shared_ptr<INpmTaskBudget> budget,
                                    uint64_t session_id, NpmPacketDirection direction,
                                    Span<const NpmTcpStreamSubscription> subscriptions,
                                    std::unique_ptr<NpmTcpStreamSharedDirection>* output,
                                    NpmTcpStreamFailure* failure = nullptr);
    ~NpmTcpStreamSharedDirection();
    NpmTcpStreamSharedDirection(const NpmTcpStreamSharedDirection&) = delete;
    NpmTcpStreamSharedDirection& operator=(const NpmTcpStreamSharedDirection&) = delete;

    NpmTcpStreamError Push(const NpmSessionView& session, const NpmPacketView& packet);
    NpmTcpStreamError AdvanceWatermark(const NpmSessionView& session, int64_t watermark_ns);
    NpmTcpStreamError End(const NpmSessionView& session, NpmTcpStreamEndReasonV1 reason,
                          std::optional<NpmSessionEndReason> session_reason, int64_t observed_at_ns);
    void Abort() noexcept;

    const NpmTcpStreamFailure& Failure() const { return failure_; }
    uint64_t TrackedBytes() const { return budget_.used; }
    bool Ended() const { return ended_; }
    std::optional<int64_t> NextEventDeadlineNs() const;

 private:
    struct Event {
        NpmTcpStreamEventV1 view;
        std::unique_ptr<uint8_t[]> data;
        uint64_t allocated_bytes = 0;
        size_t remaining = 0;
        Event* next = nullptr;
    };
    struct Reader {
        NpmTcpStreamSubscription subscription;
        Event* event = nullptr;
        uint64_t offset = 0;
        bool end_seen = false;
    };
    class Budget final : public INpmTaskBudget {
     public:
        Budget(std::shared_ptr<INpmTaskBudget> task, uint64_t limit);
        NpmBudgetError Reserve(NpmBudgetCategory category, uint64_t bytes) override;
        NpmBudgetError Release(NpmBudgetCategory category, uint64_t bytes) override;
        NpmBudgetUsage Usage() const override;
        std::shared_ptr<INpmTaskBudget> task;
        uint64_t limit = 0, used = 0;
        NpmTcpStreamError error = NpmTcpStreamError::kNone;
    };
    class Cursor;
    class Emitter;

    NpmTcpStreamSharedDirection(NpmTcpStreamConfigV1 config, std::shared_ptr<INpmTaskBudget> budget,
                                uint64_t session_id, NpmPacketDirection direction);
    void Initialize(NpmTcpStreamConfigV1 config, Span<const NpmTcpStreamSubscription> subscriptions);
    int Emit(const NpmTcpStreamEventV1& event) override;
    int EmitOwned(const NpmTcpStreamEventV1& event, std::unique_ptr<uint8_t[]>& data,
                  uint64_t allocated_bytes) override;
    void Reserve(uint64_t bytes);
    void Latch(NpmTcpStreamError error, std::string_view consumer = {}) noexcept;
    bool Begin(const NpmSessionView& session);
    NpmTcpStreamError Complete(NpmTcpStreamError core_error, const NpmSessionView& session, int64_t observed_at_ns);
    void Notify(const NpmSessionView& session, int64_t observed_at_ns);
    void Reclaim() noexcept;
    void Clear() noexcept;

    Budget budget_;
    std::optional<NpmTcpStreamDirection> core_;
    std::unique_ptr<Reader[]> readers_;
    size_t reader_count_ = 0;
    Event* head_ = nullptr;
    Event* tail_ = nullptr;
    NpmTcpStreamFailure failure_;
    std::optional<int64_t> watermark_;
    bool operating_ = false;
    bool ended_ = false;
};

}  // namespace flowsql::npm

#endif  // FLOWSQL_OPERATORS_NPM_BASIC_CORE_NPM_TCP_STREAM_SHARED_H_
