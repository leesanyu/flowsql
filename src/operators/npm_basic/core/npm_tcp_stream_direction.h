// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef FLOWSQL_OPERATORS_NPM_BASIC_CORE_NPM_TCP_STREAM_DIRECTION_H_
#define FLOWSQL_OPERATORS_NPM_BASIC_CORE_NPM_TCP_STREAM_DIRECTION_H_

#include <operators/npm_basic/npm_tcp_stream_contract.h>

#include <list>

namespace flowsql::npm {

enum class NpmTcpStreamError {
    kNone,
    kInvalidConfig,
    kInvalidInput,
    kDirectionLimitExceeded,
    kTaskBudgetExceeded,
    kAllocationFailed,
    kSinkError,
    kAborted,
    kInvalidConsume,
    kConsumerError,
    kEmitterError,
    kNotDrained,
    kReentrantCall
};

/** The event and its bytes are borrowed until Emit returns. Nonzero stops the direction; no prefix rollback. */
interface INpmTcpStreamEventSink {
    virtual ~INpmTcpStreamEventSink() = default;
    virtual int Emit(const NpmTcpStreamEventV1& event) = 0;
    /** Optional internal transfer: move data only on success and assume its existing budget charge. */
    virtual int EmitOwned(const NpmTcpStreamEventV1& event, std::unique_ptr<uint8_t[]>&, uint64_t) {
        return Emit(event);
    }
};

/** Single-direction capture-view core. Its caller owns session identity, direction routing and serialization. */
class NpmTcpStreamDirection final {
 public:
    NpmTcpStreamDirection(NpmTcpStreamConfigV1 config, std::shared_ptr<INpmTaskBudget> budget);
    /** Embedded core: caller accounts for this object's storage and outlives it with the borrowed budget. */
    NpmTcpStreamDirection(NpmTcpStreamConfigV1 config, INpmTaskBudget& budget);
    ~NpmTcpStreamDirection();
    NpmTcpStreamDirection(const NpmTcpStreamDirection&) = delete;
    NpmTcpStreamDirection& operator=(const NpmTcpStreamDirection&) = delete;

    NpmTcpStreamError Push(const NpmPacketView& packet, INpmTcpStreamEventSink& sink);
    NpmTcpStreamError AdvanceWatermark(int64_t watermark_ns, INpmTcpStreamEventSink& sink);
    NpmTcpStreamError End(NpmTcpStreamEndReasonV1 reason, std::optional<NpmSessionEndReason> session_reason,
                          INpmTcpStreamEventSink& sink);
    void Abort() noexcept;

    NpmTcpStreamError Status() const { return error_; }
    NpmTcpStreamOriginV1 Origin() const { return origin_; }
    bool CaptureTruncationSeen() const { return truncated_; }
    bool Ended() const { return ended_; }
    uint64_t PublishedEnd() const { return published_; }
    uint64_t TrackedBytes() const { return tracked_bytes_; }
    std::optional<int64_t> NextEventDeadlineNs() const;

 private:
    struct Range {
        uint64_t begin = 0;
        uint64_t end = 0;
        int64_t timestamp_ns = 0;  // Data capture time or first gap proof's deadline.
        bool truncated = false;
        uint64_t allocated_bytes = 0;
        std::unique_ptr<uint8_t[]> data;
    };
    using Ranges = std::list<Range>;
    using Iterator = Ranges::iterator;
    static constexpr uint64_t kRangeCharge = sizeof(Range) + 2 * sizeof(void*);

    void Reserve(uint64_t bytes);
    void Initialize(bool charge_object);
    void Release(uint64_t bytes) noexcept;
    void Clear() noexcept;
    NpmTcpStreamError Fail(NpmTcpStreamError error) noexcept;
    Iterator AddGap(Iterator before, uint64_t begin, uint64_t end, int64_t deadline, bool truncated);
    Iterator SplitGap(Iterator at, uint64_t position);
    Iterator Erase(Iterator at) noexcept;
    void Fill(uint64_t begin, uint64_t end, const uint8_t* bytes, int64_t captured_at);
    void MarkTruncated(uint64_t begin, uint64_t end);
    void TrimAtFin(uint64_t position);
    bool MapSequence(uint32_t sequence, uint64_t* begin, uint64_t* prefix) const;
    void Ingest(const NpmPacketView& packet, INpmTcpStreamEventSink& sink);
    void Emit(const NpmTcpStreamEventV1& event, INpmTcpStreamEventSink& sink);
    void Drain(bool forced, INpmTcpStreamEventSink& sink);
    void Finish(NpmTcpStreamEndReasonV1 reason, std::optional<NpmSessionEndReason> session_reason,
                INpmTcpStreamEventSink& sink);

    NpmTcpStreamConfigV1 config_;
    std::shared_ptr<INpmTaskBudget> budget_owner_;
    INpmTaskBudget* budget_ = nullptr;
    Ranges ranges_;
    uint64_t tracked_bytes_ = 0;
    uint64_t published_ = 0;
    uint64_t extent_ = 0;
    uint64_t progress_ = 0;
    uint32_t base_sequence_ = 0;
    std::optional<uint64_t> fin_;
    std::optional<int64_t> watermark_;
    NpmTcpStreamOriginV1 origin_ = NpmTcpStreamOriginV1::kUnknown;
    NpmTcpStreamError error_ = NpmTcpStreamError::kNone;
    bool truncated_ = false;
    bool ended_ = false;
};

}  // namespace flowsql::npm

#endif  // FLOWSQL_OPERATORS_NPM_BASIC_CORE_NPM_TCP_STREAM_DIRECTION_H_
