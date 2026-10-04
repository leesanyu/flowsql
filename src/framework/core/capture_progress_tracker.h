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

enum class CaptureProgressErrorV1 : uint8_t {
    kNone = 0,
    kInvalidVersion,
    kWrongQueue,
    kInvalidSequence,
    kInvalidTime,
    kUnprocessedPacket,
    kInvalidFact,
    kInvalidBacklog,
};

enum class CaptureProgressDispositionV1 : uint8_t {
    kAdvanced = 0,
    kUnchanged,
    kBacklogUnknown,
    kBacklogged,
    kIdleUnconfirmed,
    kTimeRegressed,
};

struct CaptureProgressResultV1 {
    CaptureProgressErrorV1 error = CaptureProgressErrorV1::kNone;
    CaptureProgressDispositionV1 disposition = CaptureProgressDispositionV1::kUnchanged;
    std::optional<int64_t> capture_progress_ns;
};

/** One task, one queue, one generation. Call Observe only after the corresponding batch was processed and released. */
class CaptureProgressTrackerV1 final {
 public:
    explicit CaptureProgressTrackerV1(const CaptureQueueIdentityV1& identity)
        : source_id_(identity.source_id), queue_id_(identity.queue_id), generation_(identity.generation) {}

    CaptureProgressResultV1 Observe(const CaptureProgressV1& fact,
                                    std::optional<int64_t> processed_packet_max_time_ns = std::nullopt) {
        CaptureProgressResultV1 result;
        result.capture_progress_ns = capture_progress_ns_;
        if (fact.struct_size < sizeof(CaptureProgressV1) ||
            fact.contract_version != kCaptureBlockStreamContractVersionV1) {
            result.error = CaptureProgressErrorV1::kInvalidVersion;
            return result;
        }
        if (fact.source_id != source_id_ || fact.queue_id != queue_id_ || fact.generation != generation_) {
            result.error = CaptureProgressErrorV1::kWrongQueue;
            return result;
        }
        if (fact.fact_sequence == 0 || fact.fact_sequence <= last_sequence_) {
            result.error = CaptureProgressErrorV1::kInvalidSequence;
            return result;
        }
        if (fact.capture_time_ns < 0) {
            result.error = CaptureProgressErrorV1::kInvalidTime;
            return result;
        }
        if (fact.backlog != CaptureBacklogV1::kUnknown && fact.backlog != CaptureBacklogV1::kEmpty &&
            fact.backlog != CaptureBacklogV1::kPresent) {
            result.error = CaptureProgressErrorV1::kInvalidBacklog;
            return result;
        }
        if (fact.packet_observed != processed_packet_max_time_ns.has_value() ||
            (processed_packet_max_time_ns && fact.capture_time_ns > *processed_packet_max_time_ns)) {
            result.error = CaptureProgressErrorV1::kUnprocessedPacket;
            return result;
        }
        if (fact.packet_observed && fact.source_idle_confirmed) {
            result.error = CaptureProgressErrorV1::kInvalidFact;
            return result;
        }

        last_sequence_ = fact.fact_sequence;
        if (last_seen_time_ns_ && fact.capture_time_ns < *last_seen_time_ns_) {
            result.disposition = CaptureProgressDispositionV1::kTimeRegressed;
            return result;
        }
        last_seen_time_ns_ = fact.capture_time_ns;
        if (fact.backlog == CaptureBacklogV1::kUnknown) {
            result.disposition = CaptureProgressDispositionV1::kBacklogUnknown;
            return result;
        }
        if (fact.backlog == CaptureBacklogV1::kPresent) {
            result.disposition = CaptureProgressDispositionV1::kBacklogged;
            return result;
        }
        if (!fact.packet_observed && !fact.source_idle_confirmed) {
            result.disposition = CaptureProgressDispositionV1::kIdleUnconfirmed;
            return result;
        }
        if (!capture_progress_ns_ || fact.capture_time_ns > *capture_progress_ns_) {
            capture_progress_ns_ = fact.capture_time_ns;
            result.capture_progress_ns = capture_progress_ns_;
            result.disposition = CaptureProgressDispositionV1::kAdvanced;
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
class CaptureProgressTrackerV2 final {
 public:
    explicit CaptureProgressTrackerV2(const CaptureSourceSetV2& sources) {
        for (const auto& input : sources.inputs)
            inputs_.emplace(input.source_id, Input{CaptureProgressTrackerV1(input)});
    }
    CaptureProgressErrorV1 Observe(const std::vector<CaptureProgressV1>& facts) {
        auto next = inputs_;
        std::set<uint32_t> seen;
        for (const auto& fact : facts) {
            auto it = next.find(fact.source_id);
            if (it == next.end() || !seen.insert(fact.source_id).second) return CaptureProgressErrorV1::kWrongQueue;
            auto result = it->second.tracker.Observe(
                fact, fact.packet_observed ? std::optional<int64_t>(fact.capture_time_ns) : std::nullopt);
            if (result.error != CaptureProgressErrorV1::kNone) return result.error;
            it->second.safe = fact.backlog == CaptureBacklogV1::kEmpty &&
                              (fact.packet_observed || fact.source_idle_confirmed) &&
                              result.disposition != CaptureProgressDispositionV1::kTimeRegressed;
        }
        inputs_ = std::move(next);
        return CaptureProgressErrorV1::kNone;
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
        CaptureProgressTrackerV1 tracker;
        bool safe = false;
    };
    std::map<uint32_t, Input> inputs_;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_CORE_CAPTURE_PROGRESS_TRACKER_H_
