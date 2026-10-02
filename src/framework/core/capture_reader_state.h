// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_FRAMEWORK_CORE_CAPTURE_READER_STATE_H_
#define _FLOWSQL_FRAMEWORK_CORE_CAPTURE_READER_STATE_H_

#include <framework/core/packet_codec.h>
#include <framework/interfaces/icapture_block_stream_reader.h>

#include <arrow/api.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <unordered_map>

namespace flowsql {

/** Reusable per-reader admission and buffer-lifetime guard; the backend still owns its RX buffers. */
class CaptureReaderStateV1 final {
 public:
    enum class Terminal : uint8_t { kOpen, kEof, kCancelled, kError };

    CaptureReaderStateV1() = default;
    CaptureReaderStateV1(const CaptureReaderStateV1&) = delete;
    CaptureReaderStateV1& operator=(const CaptureReaderStateV1&) = delete;

    int Open(const CaptureQueueIdentityV1& identity, const CaptureReaderLimitsV1& limits) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (opened_) return EALREADY;
        if (ValidateCaptureReaderDescriptionV1(identity, limits) != CaptureDescriptionErrorV1::kNone) return EINVAL;
        if (identity.generation <= last_generation_) return EINVAL;
        source_name_ = identity.source_name;
        identity_ = identity;
        identity_.source_name = source_name_.c_str();
        limits_ = limits;
        last_packet_sequence_.reset();
        counters_ = {};
        counters_.generation = identity.generation;
        counters_.available_mask = kCaptureReceivedPacketsAvailable | kCaptureDeliveredPacketsAvailable |
                                   kCaptureDeliveredBytesAvailable | kCaptureQueueDroppedPacketsAvailable |
                                   kCaptureBackpressureEventsAvailable;
        terminal_ = Terminal::kOpen;
        last_generation_ = identity.generation;
        opened_ = true;
        return 0;
    }

    /** Returns -EINVAL for invalid requests. The caller may use zero for a nonblocking poll. */
    int EffectiveWaitMs(int requested_ms) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!opened_ || requested_ms < 0) return -EINVAL;
        return std::min(requested_ms,
                        static_cast<int>(std::min<uint32_t>(limits_.max_wait_ms, std::numeric_limits<int>::max())));
    }

    /**
     * owner must be held by the Arrow buffers that expose the slot's bytes. The guard tracks it weakly
     * after Release, so the backend cannot reuse a slot while a downstream slice still retains bytes.
     */
    int Borrow(uint64_t slot, const std::shared_ptr<arrow::RecordBatch>& batch, uint64_t captured_bytes,
               const std::shared_ptr<const void>& owner) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!opened_ || terminal_ != Terminal::kOpen) return EPIPE;
        if (!batch || !owner || !batch->schema() || !batch->schema()->Equals(*packet::PacketSchema(), true) ||
            batch->num_rows() <= 0) {
            return EINVAL;
        }
        if (static_cast<uint64_t>(batch->num_rows()) > limits_.max_packets_per_batch) return EOVERFLOW;
        const auto lengths = std::static_pointer_cast<arrow::UInt32Array>(batch->column(1));
        const auto wires = std::static_pointer_cast<arrow::UInt32Array>(batch->column(2));
        const auto links = std::static_pointer_cast<arrow::UInt32Array>(batch->column(3));
        const auto sources = std::static_pointer_cast<arrow::UInt32Array>(batch->column(4));
        const auto sequences = std::static_pointer_cast<arrow::UInt64Array>(batch->column(5));
        const auto raw = std::static_pointer_cast<arrow::BinaryArray>(batch->column(6));
        uint64_t actual_bytes = 0;
        std::optional<uint64_t> next_sequence = last_packet_sequence_;
        for (int64_t row = 0; row < batch->num_rows(); ++row) {
            if (lengths->IsNull(row) || wires->IsNull(row) || links->IsNull(row) || sources->IsNull(row) ||
                sequences->IsNull(row) || raw->IsNull(row) || wires->Value(row) < lengths->Value(row) ||
                links->Value(row) != identity_.link_type ||
                static_cast<uint32_t>(raw->value_length(row)) != lengths->Value(row) ||
                sources->Value(row) != identity_.source_id ||
                (next_sequence && sequences->Value(row) <= *next_sequence)) {
                return EINVAL;
            }
            actual_bytes += lengths->Value(row);
            next_sequence = sequences->Value(row);
        }
        if (actual_bytes != captured_bytes) return EINVAL;
        if (captured_bytes > limits_.max_bytes_per_batch) return EOVERFLOW;
        if (outstanding_.size() >= limits_.max_outstanding_batches) return EAGAIN;
        if (outstanding_.count(batch.get()) != 0 || active_slots_.count(slot) != 0) return EBUSY;
        const auto retired = retired_slots_.find(slot);
        if (retired != retired_slots_.end()) {
            if (!retired->second.expired()) return EBUSY;
            retired_slots_.erase(retired);
        }
        try {
            outstanding_.emplace(batch.get(), Lease{slot, owner});
            active_slots_.emplace(slot, batch.get());
        } catch (const std::bad_alloc&) {
            outstanding_.erase(batch.get());
            return ENOMEM;
        }
        counters_.delivered_packets = SaturatingAdd(counters_.delivered_packets, batch->num_rows());
        counters_.delivered_bytes = SaturatingAdd(counters_.delivered_bytes, captured_bytes);
        last_packet_sequence_ = next_sequence;
        return 0;
    }

    int Release(const std::shared_ptr<arrow::RecordBatch>& batch) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!opened_ || !batch) return EINVAL;
        const auto it = outstanding_.find(batch.get());
        if (it == outstanding_.end()) return EINVAL;
        try {
            retired_slots_[it->second.slot] = it->second.owner;
        } catch (const std::bad_alloc&) {
            return ENOMEM;
        }
        active_slots_.erase(it->second.slot);
        outstanding_.erase(it);
        return 0;
    }

    bool CanReuse(uint64_t slot) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!opened_ || active_slots_.count(slot) != 0) return false;
        const auto retired = retired_slots_.find(slot);
        return retired == retired_slots_.end() || retired->second.expired();
    }

    void Finish() { End(Terminal::kEof); }
    void Cancel() { End(Terminal::kCancelled); }
    void Fail() { End(Terminal::kError); }

    /** Terminal reader resources may be closed only after all outstanding and retained owners leave. */
    int Close() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!opened_) return EINVAL;
        if (terminal_ == Terminal::kOpen || !outstanding_.empty()) return EBUSY;
        for (const auto& entry : retired_slots_) {
            if (!entry.second.expired()) return EBUSY;
        }
        retired_slots_.clear();
        source_name_.clear();
        identity_ = {};
        limits_ = {};
        opened_ = false;
        return 0;
    }

    size_t OutstandingCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return outstanding_.size();
    }

    Terminal State() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return terminal_;
    }

    void CountReceived(uint64_t packets) {
        std::lock_guard<std::mutex> lock(mutex_);
        counters_.received_packets = SaturatingAdd(counters_.received_packets, packets);
    }

    void CountQueueDropped(uint64_t packets) {
        std::lock_guard<std::mutex> lock(mutex_);
        counters_.queue_dropped_packets = SaturatingAdd(counters_.queue_dropped_packets, packets);
    }

    void CountBackpressure() {
        std::lock_guard<std::mutex> lock(mutex_);
        counters_.backpressure_events = SaturatingAdd(counters_.backpressure_events, 1);
    }

    /** Backend measurements may be reported only when actually available. */
    void SetSourceDropped(uint64_t packets) {
        std::lock_guard<std::mutex> lock(mutex_);
        counters_.source_dropped_packets = std::max(counters_.source_dropped_packets, packets);
        counters_.available_mask |= kCaptureSourceDroppedPacketsAvailable;
    }

    int ReadCounters(CaptureCountersV1* output) const {
        if (!output || output->struct_size < sizeof(CaptureCountersV1) ||
            output->contract_version != kCaptureBlockStreamContractVersionV1) {
            return EINVAL;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!opened_) return EPIPE;
        *output = counters_;
        return 0;
    }

 private:
    static uint64_t SaturatingAdd(uint64_t current, uint64_t added) {
        return added > std::numeric_limits<uint64_t>::max() - current ? std::numeric_limits<uint64_t>::max()
                                                                      : current + added;
    }

    struct Lease {
        uint64_t slot;
        std::weak_ptr<const void> owner;
    };

    void End(Terminal terminal) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (opened_ && terminal_ == Terminal::kOpen) terminal_ = terminal;
    }

    mutable std::mutex mutex_;
    bool opened_ = false;
    uint64_t last_generation_ = 0;
    Terminal terminal_ = Terminal::kOpen;
    std::string source_name_;
    CaptureQueueIdentityV1 identity_;
    CaptureReaderLimitsV1 limits_;
    CaptureCountersV1 counters_;
    std::optional<uint64_t> last_packet_sequence_;
    std::unordered_map<const arrow::RecordBatch*, Lease> outstanding_;
    std::unordered_map<uint64_t, const arrow::RecordBatch*> active_slots_;
    std::unordered_map<uint64_t, std::weak_ptr<const void>> retired_slots_;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_CORE_CAPTURE_READER_STATE_H_
