// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef FLOWSQL_OPERATORS_NPM_BASIC_NPM_TCP_STREAM_CONTRACT_H_
#define FLOWSQL_OPERATORS_NPM_BASIC_NPM_TCP_STREAM_CONTRACT_H_

#include "npm_analysis_contract.h"

namespace flowsql::npm {

interface INpmResultEmitterV1;

constexpr uint64_t kNpmMinTcpStreamBufferedBytesPerDirection = 65536;
constexpr uint64_t kNpmDefaultTcpStreamBufferedBytesPerDirection = 1048576;
constexpr uint64_t kNpmMaxTcpStreamBufferedBytesPerDirection = 67108864;
constexpr int64_t kNpmMinTcpStreamGapTimeoutNs = 0;
constexpr int64_t kNpmDefaultTcpStreamGapTimeoutNs = kNpmNanosecondsPerSecond;
constexpr int64_t kNpmMaxTcpStreamGapTimeoutNs = 60 * kNpmNanosecondsPerSecond;

struct NpmTcpStreamConfigV1 {
    uint64_t max_buffered_bytes_per_direction = kNpmDefaultTcpStreamBufferedBytesPerDirection;
    int64_t gap_timeout_ns = kNpmDefaultTcpStreamGapTimeoutNs;
};

enum class NpmTcpStreamOriginV1 : uint8_t { kUnknown, kSyn, kMidstream };
enum class NpmTcpStreamEventKindV1 : uint8_t { kData, kGap, kEnd };
enum class NpmTcpStreamGapReasonV1 : uint8_t { kWaitExpired, kTermination };
enum class NpmTcpStreamEndReasonV1 : uint8_t { kFin, kReset, kSessionEnd, kSequenceAmbiguous };

/** Immutable capture-view facts, borrowed during a consumer callback; never an application message boundary. */
struct NpmTcpStreamEventV1 {
    NpmTcpStreamEventKindV1 kind = NpmTcpStreamEventKindV1::kData;
    uint64_t begin = 0;
    uint64_t end = 0;                                   // Half-open byte interval; End has begin == end.
    Span<const uint8_t> bytes;                          // Data only: nonempty, size == end - begin.
    std::optional<int64_t> captured_at_ns;              // Data only: capture time of the packet supplying these bytes.
    std::optional<NpmTcpStreamGapReasonV1> gap_reason;  // Gap only.
    bool includes_capture_truncation = false;           // Gap only: missing bytes include known capture truncation.
    std::optional<NpmTcpStreamEndReasonV1> end_reason;  // End only.
    std::optional<NpmSessionEndReason> session_end_reason;  // End caused by session termination only.
};

struct NpmTcpStreamContextV1 {
    const NpmSessionView* session = nullptr;  // Callback-borrowed; stream identity is (session_id, direction).
    NpmPacketDirection direction = NpmPacketDirection::kAToB;
    NpmTcpStreamOriginV1 origin = NpmTcpStreamOriginV1::kUnknown;
    bool capture_truncation_seen = false;  // Remains true even if retransmissions fill the missing bytes.
    bool final_drain = false;
    int64_t observed_at_ns = 0;  // Notification trigger time, not the capture time of the delivered bytes.
};

/** Callback-borrowed, per-consumer cursor. Retaining the cursor or its byte spans is forbidden. */
interface INpmTcpStreamCursorV1 {
    virtual ~INpmTcpStreamCursorV1() = default;
    /** Non-null output required. False means temporarily unreadable; Peek never advances the cursor. */
    virtual bool Peek(NpmTcpStreamEventV1 * event) const = 0;
    /**
     * Data: consume 1..bytes.size; Gap/End: acknowledge with 0. Invalid operations return nonzero and
     * latch a provider failure. Consume invalidates prior spans; callback return invalidates all borrowed views.
     */
    virtual int Consume(uint64_t bytes) = 0;
};

/** Implemented by the same object as INpmProtocolModuleV1, without extending the existing V1 vtable. */
interface INpmTcpStreamConsumerV1 {
    virtual ~INpmTcpStreamConsumerV1() = default;
    /**
     * Synchronous, serial, non-reentrant. Loop over Peek/Consume; nonzero aborts the task.
     * A normal callback may leave unread events; final_drain requires consuming through and acknowledging End.
     * Bytes shared with other consumers are reclaimed only after every consumer has advanced past them.
     */
    virtual int OnTcpStreamReadable(const NpmTcpStreamContextV1& context, INpmTcpStreamCursorV1& cursor,
                                    INpmResultEmitterV1& emitter) = 0;
};

}  // namespace flowsql::npm

#endif  // FLOWSQL_OPERATORS_NPM_BASIC_NPM_TCP_STREAM_CONTRACT_H_
