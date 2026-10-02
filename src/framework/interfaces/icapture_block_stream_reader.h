// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_ICAPTURE_BLOCK_STREAM_READER_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_ICAPTURE_BLOCK_STREAM_READER_H_

#include <common/guid.h>
#include <common/typedef.h>

#include <cstdint>

#include "iblock_stream_channel.h"

namespace flowsql {

// {d51a78b9-8f60-48b0-a23b-15c42f7d9e06}
const Guid IID_CAPTURE_BLOCK_STREAM_READER_V1 = {
    0xd51a78b9, 0x8f60, 0x48b0, {0xa2, 0x3b, 0x15, 0xc4, 0x2f, 0x7d, 0x9e, 0x06}};

constexpr uint32_t kCaptureBlockStreamContractVersionV1 = 1;

/** Immutable effective limits for one task-exclusive reader. All four values must be positive. */
struct CaptureReaderLimitsV1 {
    uint32_t struct_size = sizeof(CaptureReaderLimitsV1);
    uint32_t contract_version = kCaptureBlockStreamContractVersionV1;
    uint32_t max_packets_per_batch = 0;
    uint64_t max_bytes_per_batch = 0;
    uint32_t max_wait_ms = 0;
    uint32_t max_outstanding_batches = 0;
};

/** The source_name pointer is borrowed until the reader is released; consumers copy it at binding. */
struct CaptureQueueIdentityV1 {
    uint32_t struct_size = sizeof(CaptureQueueIdentityV1);
    uint32_t contract_version = kCaptureBlockStreamContractVersionV1;
    uint64_t observation_domain_id = 0;
    const char* source_name = nullptr;
    uint32_t source_id = 0;
    uint32_t queue_id = 0;
    uint64_t generation = 0;
    uint32_t link_type = 0;
};

enum class CaptureBacklogV1 : uint8_t { kUnknown = 0, kEmpty = 1, kPresent = 2 };

/** Ordered source fact. A timeout without an explicit progress fact proves nothing about idleness. */
struct CaptureProgressV1 {
    uint32_t struct_size = sizeof(CaptureProgressV1);
    uint32_t contract_version = kCaptureBlockStreamContractVersionV1;
    uint32_t source_id = 0;
    uint32_t queue_id = 0;
    uint64_t generation = 0;
    uint64_t fact_sequence = 0;
    int64_t capture_time_ns = 0;
    bool packet_observed = false;
    bool source_idle_confirmed = false;
    CaptureBacklogV1 backlog = CaptureBacklogV1::kUnknown;
};

enum CaptureCounterAvailabilityV1 : uint32_t {
    kCaptureReceivedPacketsAvailable = 1u << 0,
    kCaptureDeliveredPacketsAvailable = 1u << 1,
    kCaptureDeliveredBytesAvailable = 1u << 2,
    kCaptureSourceDroppedPacketsAvailable = 1u << 3,
    kCaptureQueueDroppedPacketsAvailable = 1u << 4,
    kCaptureBackpressureEventsAvailable = 1u << 5,
};

/** Cumulative within one generation. A zero value is measured only when its availability bit is set. */
struct CaptureCountersV1 {
    uint32_t struct_size = sizeof(CaptureCountersV1);
    uint32_t contract_version = kCaptureBlockStreamContractVersionV1;
    uint64_t generation = 0;
    uint64_t received_packets = 0;
    uint64_t delivered_packets = 0;
    uint64_t delivered_bytes = 0;
    uint64_t source_dropped_packets = 0;
    uint64_t queue_dropped_packets = 0;
    uint64_t backpressure_events = 0;
    uint32_t available_mask = 0;
};

/** Data and its progress fact are returned together, but the consumer applies the fact after release. */
struct CapturePollEventV1 {
    uint32_t struct_size = sizeof(CapturePollEventV1);
    uint32_t contract_version = kCaptureBlockStreamContractVersionV1;
    BlockPollEvent block;
    bool has_progress = false;
    CaptureProgressV1 progress;
};

enum class CaptureDescriptionErrorV1 : uint8_t {
    kNone = 0,
    kInvalidVersion,
    kInvalidIdentity,
    kInvalidLimits,
};

inline CaptureDescriptionErrorV1 ValidateCaptureReaderDescriptionV1(const CaptureQueueIdentityV1& identity,
                                                                    const CaptureReaderLimitsV1& limits) {
    if (identity.struct_size < sizeof(CaptureQueueIdentityV1) ||
        identity.contract_version != kCaptureBlockStreamContractVersionV1 ||
        limits.struct_size < sizeof(CaptureReaderLimitsV1) ||
        limits.contract_version != kCaptureBlockStreamContractVersionV1) {
        return CaptureDescriptionErrorV1::kInvalidVersion;
    }
    if (identity.source_name == nullptr || identity.source_name[0] == '\0' || identity.generation == 0) {
        return CaptureDescriptionErrorV1::kInvalidIdentity;
    }
    if (limits.max_packets_per_batch == 0 || limits.max_bytes_per_batch == 0 || limits.max_wait_ms == 0 ||
        limits.max_outstanding_batches == 0) {
        return CaptureDescriptionErrorV1::kInvalidLimits;
    }
    return CaptureDescriptionErrorV1::kNone;
}

/** Optional extension of the existing task-exclusive IBlockStreamReaderFactoryV1 reader. */
interface ICaptureBlockStreamReaderV1 : IBlockStreamChannel {
    virtual ~ICaptureBlockStreamReaderV1() = default;
    /** Caller initializes struct_size and contract_version in both output structures. */
    virtual int Describe(CaptureQueueIdentityV1 * identity, CaptureReaderLimitsV1 * limits) const = 0;
    /** Do not mix PollCapture and PollBlock in one reader run. The effective wait is capped by Describe(). */
    virtual CapturePollEventV1 PollCapture(int timeout_ms) = 0;
    /** Caller initializes struct_size and contract_version; a snapshot is cumulative within generation. */
    virtual int ReadCounters(CaptureCountersV1 * counters) const = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_ICAPTURE_BLOCK_STREAM_READER_H_
