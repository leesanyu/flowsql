// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_ICAPTURE_BLOCK_STREAM_READER_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_ICAPTURE_BLOCK_STREAM_READER_H_

#include <common/guid.h>
#include <common/typedef.h>

#include <cstdint>
#include <vector>

#include "iblock_stream_channel.h"

namespace flowsql {

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

constexpr uint32_t kCaptureBlockStreamContractVersionV2 = 2;

// {091315e4-64fd-4e70-8f14-4e91a30cc902}
const Guid IID_CAPTURE_BLOCK_STREAM_READER_V2 = {
    0x091315e4, 0x64fd, 0x4e70, {0x8f, 0x14, 0x4e, 0x91, 0xa3, 0x0c, 0xc9, 0x02}};

struct CaptureSourceSetV2 {
    uint32_t struct_size = sizeof(CaptureSourceSetV2);
    uint32_t contract_version = kCaptureBlockStreamContractVersionV2;
    std::vector<CaptureQueueIdentityV1> inputs;
    CaptureReaderLimitsV1 limits;
    uint32_t max_inspected_packets_per_poll = 1024;
    uint64_t max_inspected_bytes_per_poll = 4 * 1024 * 1024;
    uint32_t max_poll_work_ms = 2;
};

struct CapturePollEventV2 {
    uint32_t struct_size = sizeof(CapturePollEventV2);
    uint32_t contract_version = kCaptureBlockStreamContractVersionV2;
    BlockPollEvent block;
    std::vector<CaptureProgressV1> progress;
};

inline CaptureDescriptionErrorV1 ValidateCaptureSourceSetV2(const CaptureSourceSetV2& sources) {
    if (sources.struct_size < sizeof(CaptureSourceSetV2) ||
        sources.contract_version != kCaptureBlockStreamContractVersionV2)
        return CaptureDescriptionErrorV1::kInvalidVersion;
    if (sources.inputs.empty()) return CaptureDescriptionErrorV1::kInvalidIdentity;
    if (!sources.max_inspected_packets_per_poll || !sources.max_inspected_bytes_per_poll || !sources.max_poll_work_ms)
        return CaptureDescriptionErrorV1::kInvalidLimits;
    const auto& first = sources.inputs.front();
    for (size_t i = 0; i < sources.inputs.size(); ++i) {
        const auto& input = sources.inputs[i];
        const auto error = ValidateCaptureReaderDescriptionV1(input, sources.limits);
        if (error != CaptureDescriptionErrorV1::kNone) return error;
        if (input.observation_domain_id != first.observation_domain_id || input.generation != first.generation ||
            input.link_type != 1)
            return CaptureDescriptionErrorV1::kInvalidIdentity;
        for (size_t j = 0; j < i; ++j)
            if (input.source_id == sources.inputs[j].source_id) return CaptureDescriptionErrorV1::kInvalidIdentity;
    }
    return CaptureDescriptionErrorV1::kNone;
}

interface ICaptureBlockStreamReaderV2 : IBlockStreamChannel {
    virtual ~ICaptureBlockStreamReaderV2() = default;
    virtual int DescribeSources(CaptureSourceSetV2 * sources) const = 0;
    virtual CapturePollEventV2 PollCapture(int timeout_ms) = 0;
    virtual int ReadInputCounters(uint32_t source_id, CaptureCountersV1 * counters) const = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_ICAPTURE_BLOCK_STREAM_READER_H_
