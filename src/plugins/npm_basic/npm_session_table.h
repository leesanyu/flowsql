// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_PLUGINS_NPM_BASIC_NPM_SESSION_TABLE_H_
#define _FLOWSQL_PLUGINS_NPM_BASIC_NPM_SESSION_TABLE_H_

#include "npm_session_key.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace flowsql::npm {

struct NpmSessionKeyHash {
    size_t operator()(const NpmSessionKey& key) const noexcept;
};

struct NpmSessionKeyEqual {
    bool operator()(const NpmSessionKey& left, const NpmSessionKey& right) const noexcept;
};

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

enum class NpmCaptureProgressDisposition : uint8_t {
    kAdvanced = 0,
    kUnchanged,
    kDeferredBacklogUnknown,
    kDeferredBacklogged,
    kDeferredIdleUnconfirmed,
};

struct NpmCaptureProgressUpdate {
    int64_t capture_time_ns = 0;
    bool packet_observed = false;
    bool source_idle_confirmed = false;
    bool source_backlog_known = false;
    bool source_has_backlog = false;
};

struct NpmSessionSnapshot {
    NpmSessionKey key;
    uint64_t session_id = 0;
    int64_t first_ns = 0;
    int64_t last_ns = 0;
    uint64_t packets_ab = 0;
    uint64_t packets_ba = 0;
    uint64_t wire_bytes_ab = 0;
    uint64_t wire_bytes_ba = 0;
    NpmProtocolStatus protocol_status = NpmProtocolStatus::kPending;
    std::optional<uint16_t> protocol_id;
    std::optional<uint16_t> protocol_sub_id;
    NpmSessionEndReason end_reason = NpmSessionEndReason::kIdleTimeout;

    NpmSessionView View() const;
};

/** One packet may retire an old instance and either expose a replacement or leave no active session. */
struct NpmSessionObserveResult {
    bool has_active_session = false;
    NpmSessionView active_session;
    std::vector<NpmSessionSnapshot> ended_sessions;
};

struct NpmCaptureProgressResult {
    NpmCaptureProgressDisposition disposition = NpmCaptureProgressDisposition::kUnchanged;
    bool watermark_initialized = false;
    int64_t watermark_ns = 0;
    std::vector<NpmSessionSnapshot> ended_sessions;
};

class NpmSessionTable {
 public:
    explicit NpmSessionTable(uint64_t max_active_sessions);
    explicit NpmSessionTable(const NpmAnalysisConfig& config);
    NpmSessionTable(const NpmAnalysisConfig& config, std::shared_ptr<INpmTaskBudget> budget);
    ~NpmSessionTable();

    /** Observes one normalized packet. Active keys borrow from this table; ended snapshots own their keys. */
    NpmSessionTableError Observe(const NpmSessionPacketBinding& binding,
                                 const packet::PacketMeta& meta,
                                 NpmSessionObserveResult* output);

    /** Samples one active session payload. Output keys borrow from this table. */
    NpmSessionTableError SampleProtocol(const NpmSessionKey& key,
                                        uint64_t session_id,
                                        const NpmPacketView& packet,
                                        packet::IPacketProtocolIdentifier& identifier,
                                        NpmSessionView* output);

    /** Returns a borrowed snapshot view without updating the session. */
    NpmSessionTableError Find(const NpmSessionKey& key, NpmSessionView* output) const;

    /** Advances event time explicitly and returns owned snapshots for sessions retired as idle. */
    NpmCaptureProgressResult AdvanceCaptureProgress(const NpmCaptureProgressUpdate& update);

    /** Atomically retires every active session as EOF in ascending session ID order. */
    NpmSessionTableError FinishAllAtEof(std::vector<NpmSessionSnapshot>* output);

    size_t size() const noexcept { return sessions_.size(); }
    uint64_t tracked_bytes() const noexcept { return tracked_session_bytes_; }

 private:
    struct State {
        uint64_t session_id = 0;
        int64_t first_ns = 0;
        int64_t last_ns = 0;
        uint64_t packets_ab = 0;
        uint64_t packets_ba = 0;
        uint64_t wire_bytes_ab = 0;
        uint64_t wire_bytes_ba = 0;
        uint32_t payload_samples = 0;
        NpmProtocolStatus protocol_status = NpmProtocolStatus::kPending;
        std::optional<uint16_t> protocol_id;
        std::optional<uint16_t> protocol_sub_id;
        int64_t idle_deadline_ns = 0;
        bool initial_syn_observed = false;
        NpmPacketDirection initial_syn_direction = NpmPacketDirection::kAToB;
        uint32_t initial_syn_sequence = 0;
        bool fin_ab = false;
        bool fin_ba = false;
        uint64_t tracked_bytes = 0;
    };

    using SessionMap = std::unordered_map<NpmSessionKey, State, NpmSessionKeyHash, NpmSessionKeyEqual>;
    using DeadlineKey = std::pair<int64_t, uint64_t>;
    using DeadlineMap = std::map<DeadlineKey, NpmSessionKey>;

    static NpmSessionView MakeView(const SessionMap::value_type& entry);
    static NpmSessionSnapshot MakeSnapshot(const SessionMap::value_type& entry, NpmSessionEndReason reason);
    static uint64_t EstimateTrackedBytes(const NpmSessionKey& key) noexcept;

    NpmBudgetError ReserveSession(uint64_t bytes);
    void ReleaseSession(uint64_t bytes) noexcept;

    NpmAnalysisConfig config_;
    uint64_t max_active_sessions_ = 0;
    uint64_t next_session_id_ = 1;
    bool watermark_initialized_ = false;
    int64_t watermark_ns_ = 0;
    SessionMap sessions_;
    DeadlineMap deadlines_;
    std::shared_ptr<INpmTaskBudget> budget_;
    uint64_t tracked_session_bytes_ = 0;
};

/** Synchronously notifies every module in registration order for each owned ended-session snapshot. */
int NotifyNpmSessionEnd(const std::vector<NpmSessionSnapshot>& ended_sessions,
                        const std::vector<INpmAnalysisModule*>& modules,
                        INpmResultWriter& writer);

}  // namespace flowsql::npm

#endif  // _FLOWSQL_PLUGINS_NPM_BASIC_NPM_SESSION_TABLE_H_
