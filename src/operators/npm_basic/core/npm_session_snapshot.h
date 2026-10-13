// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include <memory>
#include <vector>
#include "npm_session_key.h"

namespace flowsql::npm {

enum class NpmSessionTableError : uint8_t {
    kNone = 0,
    kNullOutput,
    kNotFound,
    kInvalidDirection,
    kSessionLimitExceeded,
    kSessionIdExhausted,
    kLatePacket,
    kSessionInstanceMismatch,
    kInvalidPacketView,
    kSessionBudgetExceeded,
    kAllocationFailed,
};

struct NpmSessionSnapshotFacts {
    uint64_t session_id = 0;
    int64_t first_ns = 0;
    int64_t last_ns = 0;
    uint64_t packets_ab = 0;
    uint64_t packets_ba = 0;
    uint64_t wire_bytes_ab = 0;
    uint64_t wire_bytes_ba = 0;
    uint32_t primary_label_id = 0;
    NpmProtocolStatus protocol_status = NpmProtocolStatus::kPending;
    std::optional<uint16_t> protocol_id;
    std::optional<uint16_t> protocol_sub_id;
    NpmSessionEndReason end_reason = NpmSessionEndReason::kIdleTimeout;
};

// Embedded input-budget owner; arrays/strings are released before this lease.
class NpmSnapshotInputLease {
 public:
    NpmSnapshotInputLease() = default;
    ~NpmSnapshotInputLease();
    NpmSnapshotInputLease(const NpmSnapshotInputLease&) = delete;
    NpmSnapshotInputLease& operator=(const NpmSnapshotInputLease&) = delete;
    NpmSnapshotInputLease(NpmSnapshotInputLease&&) noexcept;
    NpmSnapshotInputLease& operator=(NpmSnapshotInputLease&&) noexcept;
    NpmBudgetError Reserve(std::shared_ptr<INpmTaskBudget> budget, uint64_t bytes);

 private:
    friend class NpmSessionSnapshotBatch;
    void Release(uint64_t bytes) noexcept;
    std::shared_ptr<INpmTaskBudget> budget_;
    uint64_t bytes_ = 0;
};

struct NpmSnapshotRow {
    NpmSessionSnapshotFacts facts;
    packet::AddressFamily family = packet::AddressFamily::kNone;
    uint32_t key_index = 0;
    int64_t observed_at_ns = 0;
};

// Views borrow until the next append/reset/move. All growth is completed before synchronous callbacks.
class NpmSessionSnapshotBatch {
 public:
    NpmSessionSnapshotBatch() = default;
    NpmSessionSnapshotBatch(const NpmSessionSnapshotBatch&) = delete;
    NpmSessionSnapshotBatch& operator=(const NpmSessionSnapshotBatch&) = delete;
    NpmSessionSnapshotBatch(NpmSessionSnapshotBatch&&) noexcept;
    NpmSessionSnapshotBatch& operator=(NpmSessionSnapshotBatch&&) noexcept;
    size_t size() const noexcept { return rows_.size(); }
    bool empty() const noexcept { return rows_.empty(); }
    void Clear() noexcept;
    NpmSessionView View(size_t row) const&;
    NpmSessionView View(size_t row) const&& = delete;
    const NpmSnapshotRow& Row(size_t row) const& { return rows_.at(row); }
    const NpmSnapshotRow& Row(size_t row) const&& = delete;
    NpmSessionSnapshotFacts& MutableFacts(size_t row) & { return rows_.at(row).facts; }
    void SetObservedAt(int64_t observed_at_ns) noexcept;
    NpmSessionTableError Clone(NpmSessionSnapshotBatch* output) const;
    NpmSessionTableError CloneRow(size_t row, NpmSessionSnapshotBatch* output) const;
    static NpmSessionTableError Capture(const NpmSessionView& view, NpmSessionEndReason reason, int64_t observed_at_ns,
                                        std::shared_ptr<INpmTaskBudget> budget, NpmSessionSnapshotBatch* output);

 private:
    friend class NpmSessionTable;
    friend struct NpmThroughputTestAccess;
    void Prepare(size_t count4, size_t count6, size_t rows, std::shared_ptr<INpmTaskBudget> budget);
    void Append(const NpmSessionView& view, NpmSessionEndReason reason, int64_t observed_at_ns);
    void Truncate(size_t rows) noexcept;
    uint64_t StorageBytes() const noexcept;
    uint64_t string_bytes_ = 0;
    NpmSnapshotInputLease lease_;
    std::vector<NpmSessionKey4> keys4_;
    std::vector<NpmSessionKey6> keys6_;
    std::vector<NpmSnapshotRow> rows_;
};

}  // namespace flowsql::npm
