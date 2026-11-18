// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_FRAMEWORK_CORE_CAPTURE_PROGRESS_TRACKER_H_
#define _FLOWSQL_FRAMEWORK_CORE_CAPTURE_PROGRESS_TRACKER_H_

#include <framework/interfaces/icapture_block_stream_reader.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>

namespace flowsql {

enum class CaptureProgressError : uint8_t {
    kNone = 0,
    kInvalidVersion,
    kWrongQueue,
    kInvalidSequence,
    kInvalidTime,
    kUnprocessedPacket,
    kInvalidFact,
    kInvalidBacklog,
};

enum class CaptureProgressDisposition : uint8_t {
    kAdvanced = 0,
    kUnchanged,
    kBacklogUnknown,
    kBacklogged,
    kIdleUnconfirmed,
    kTimeRegressed,
};

struct CaptureProgressResult {
    CaptureProgressError error = CaptureProgressError::kNone;
    CaptureProgressDisposition disposition = CaptureProgressDisposition::kUnchanged;
    std::optional<int64_t> capture_progress_ns;
};

/** One task, one queue, one generation. Call Observe only after the corresponding batch was processed and released. */
class SingleInputCaptureProgressTracker final {
 public:
    explicit SingleInputCaptureProgressTracker(const CaptureQueueIdentity& identity)
        : source_id_(identity.source_id), queue_id_(identity.queue_id), generation_(identity.generation) {}

    CaptureProgressResult Observe(const CaptureProgress& fact,
                                  std::optional<int64_t> processed_packet_max_time_ns = std::nullopt) {
        CaptureProgressResult result;
        result.capture_progress_ns = capture_progress_ns_;
        if (fact.struct_size < sizeof(CaptureProgress) || fact.contract_version != kCaptureFactContractVersion) {
            result.error = CaptureProgressError::kInvalidVersion;
            return result;
        }
        if (fact.source_id != source_id_ || fact.queue_id != queue_id_ || fact.generation != generation_) {
            result.error = CaptureProgressError::kWrongQueue;
            return result;
        }
        if (fact.fact_sequence == 0 || fact.fact_sequence <= last_sequence_) {
            result.error = CaptureProgressError::kInvalidSequence;
            return result;
        }
        if (fact.capture_time_ns < 0) {
            result.error = CaptureProgressError::kInvalidTime;
            return result;
        }
        if (fact.backlog != CaptureBacklog::kUnknown && fact.backlog != CaptureBacklog::kEmpty &&
            fact.backlog != CaptureBacklog::kPresent) {
            result.error = CaptureProgressError::kInvalidBacklog;
            return result;
        }
        if (fact.packet_observed != processed_packet_max_time_ns.has_value() ||
            (processed_packet_max_time_ns && fact.capture_time_ns > *processed_packet_max_time_ns)) {
            result.error = CaptureProgressError::kUnprocessedPacket;
            return result;
        }
        if (fact.packet_observed && fact.source_idle_confirmed) {
            result.error = CaptureProgressError::kInvalidFact;
            return result;
        }

        last_sequence_ = fact.fact_sequence;
        if (last_seen_time_ns_ && fact.capture_time_ns < *last_seen_time_ns_) {
            result.disposition = CaptureProgressDisposition::kTimeRegressed;
            return result;
        }
        last_seen_time_ns_ = fact.capture_time_ns;
        if (fact.backlog == CaptureBacklog::kUnknown) {
            result.disposition = CaptureProgressDisposition::kBacklogUnknown;
            return result;
        }
        if (fact.backlog == CaptureBacklog::kPresent) {
            result.disposition = CaptureProgressDisposition::kBacklogged;
            return result;
        }
        if (!fact.packet_observed && !fact.source_idle_confirmed) {
            result.disposition = CaptureProgressDisposition::kIdleUnconfirmed;
            return result;
        }
        if (!capture_progress_ns_ || fact.capture_time_ns > *capture_progress_ns_) {
            capture_progress_ns_ = fact.capture_time_ns;
            result.capture_progress_ns = capture_progress_ns_;
            result.disposition = CaptureProgressDisposition::kAdvanced;
        }
        return result;
    }

    std::optional<int64_t> ProgressNs() const { return capture_progress_ns_; }

 private:
    uint32_t source_id_ = 0;
    uint32_t queue_id_ = 0;
    uint64_t generation_ = 0;
    uint64_t last_sequence_ = 0;
    std::optional<int64_t> last_seen_time_ns_;
    std::optional<int64_t> capture_progress_ns_;
};

/** Transactional multi-input safety gate. A blocked input suspends the common candidate. */
class MultiInputCaptureProgressTracker final {
 public:
    explicit MultiInputCaptureProgressTracker(const CaptureSourceSet& sources) {
        for (const auto& input : sources.inputs)
            inputs_.emplace(input.source_id, Input{SingleInputCaptureProgressTracker(input)});
    }
    CaptureProgressError Observe(const std::vector<CaptureProgress>& facts) {
        auto next = inputs_;
        std::set<uint32_t> seen;
        for (const auto& fact : facts) {
            auto it = next.find(fact.source_id);
            if (it == next.end() || !seen.insert(fact.source_id).second) return CaptureProgressError::kWrongQueue;
            auto result = it->second.tracker.Observe(
                fact, fact.packet_observed ? std::optional<int64_t>(fact.capture_time_ns) : std::nullopt);
            if (result.error != CaptureProgressError::kNone) return result.error;
            it->second.safe = fact.backlog == CaptureBacklog::kEmpty &&
                              (fact.packet_observed || fact.source_idle_confirmed) &&
                              result.disposition != CaptureProgressDisposition::kTimeRegressed;
        }
        inputs_ = std::move(next);
        return CaptureProgressError::kNone;
    }
    std::optional<int64_t> CommonCandidateNs() const {
        std::optional<int64_t> candidate;
        for (const auto& entry : inputs_) {
            const auto progress = entry.second.tracker.ProgressNs();
            if (!entry.second.safe || !progress) return std::nullopt;
            candidate = candidate ? std::min(*candidate, *progress) : progress;
        }
        return candidate;
    }
    std::vector<uint32_t> BlockedInputs() const {
        std::vector<uint32_t> result;
        for (const auto& entry : inputs_)
            if (!entry.second.safe || !entry.second.tracker.ProgressNs()) result.push_back(entry.first);
        return result;
    }

 private:
    struct Input {
        SingleInputCaptureProgressTracker tracker;
        bool safe = false;
    };
    std::map<uint32_t, Input> inputs_;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_CORE_CAPTURE_PROGRESS_TRACKER_H_
