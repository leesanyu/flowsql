// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_OPERATORS_NPM_BASIC_NPM_TCP_PERFORMANCE_TRACKER_H_
#define _FLOWSQL_OPERATORS_NPM_BASIC_NPM_TCP_PERFORMANCE_TRACKER_H_

#include <operators/npm_basic/npm_analysis_contract.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace flowsql::npm {

enum class NpmTcpPerformanceError : uint8_t {
    kNone = 0,
    kNullBudget,
    kNullOutput,
    kInvalidSession,
    kInvalidPacket,
    kNotFound,
    kBudgetExceeded,
    kAllocationFailed,
    kRangeLimitExceeded,
    kInvalidRangeLimit,
    kProtocolMismatch,
    kSessionViewMismatch,
};

struct NpmTcpPerformanceSnapshot {
    uint64_t payload_bytes_ab = 0;
    uint64_t payload_bytes_ba = 0;
    NpmRateStatus rate_status = NpmRateStatus::kInsufficientSpan;
    std::optional<double> wire_bps_ab;
    std::optional<double> wire_bps_ba;
    std::optional<double> payload_bps_ab;
    std::optional<double> payload_bps_ba;
    std::optional<double> unique_payload_bps_ab;
    std::optional<double> unique_payload_bps_ba;
    NpmTcpHandshakeStatus handshake_status = NpmTcpHandshakeStatus::kNotObserved;
    std::optional<NpmTcpInitiator> initiator;
    std::optional<int64_t> handshake_duration_ns;
    std::optional<int64_t> synack_rtt_ns;
    NpmTcpRttStatus rtt_status = NpmTcpRttStatus::kNoSample;
    std::optional<uint64_t> rtt_samples;
    std::optional<int64_t> rtt_min_ns;
    std::optional<int64_t> rtt_mean_ns;
    std::optional<int64_t> rtt_max_ns;
    std::optional<uint64_t> unique_payload_bytes_ab;
    std::optional<uint64_t> unique_payload_bytes_ba;
    NpmTcpRetransmissionStatus retransmission_status = NpmTcpRetransmissionStatus::kValid;
    std::optional<uint64_t> retrans_packets_ab;
    std::optional<uint64_t> retrans_packets_ba;
    std::optional<uint64_t> retrans_payload_bytes_ab;
    std::optional<uint64_t> retrans_payload_bytes_ba;
    uint32_t measurement_flags = 0;
};

/** Task-private TCP/UDP performance state. TCP bounds history + outstanding ranges per direction. */
class NpmTcpPerformanceTracker final {
 public:
    explicit NpmTcpPerformanceTracker(std::shared_ptr<INpmTaskBudget> budget, uint32_t max_ranges_per_direction = 1024);
    ~NpmTcpPerformanceTracker();

    NpmTcpPerformanceTracker(const NpmTcpPerformanceTracker&) = delete;
    NpmTcpPerformanceTracker& operator=(const NpmTcpPerformanceTracker&) = delete;
    NpmTcpPerformanceTracker(NpmTcpPerformanceTracker&&) = delete;
    NpmTcpPerformanceTracker& operator=(NpmTcpPerformanceTracker&&) = delete;

    NpmTcpPerformanceError Observe(const NpmPacketView& packet, const NpmSessionView& session);
    NpmTcpPerformanceError Snapshot(uint64_t session_id, NpmTcpPerformanceSnapshot* output) const;
    NpmTcpPerformanceError Snapshot(const NpmSessionView& session, NpmTcpPerformanceSnapshot* output) const;

    void Remove(uint64_t session_id) noexcept;
    void Clear() noexcept;

    size_t size() const noexcept;
    size_t outstanding_segments() const noexcept;
    uint64_t tracked_bytes() const noexcept;

 private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace flowsql::npm

#endif  // _FLOWSQL_OPERATORS_NPM_BASIC_NPM_TCP_PERFORMANCE_TRACKER_H_
