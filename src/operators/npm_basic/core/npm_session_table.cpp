// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_session_table.h"

#include "npm_task_budget.h"

#include <common/network/netbase.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace flowsql::npm {
namespace {

constexpr size_t kHashOffset = static_cast<size_t>(1469598103934665603ULL);
constexpr size_t kHashPrime = static_cast<size_t>(1099511628211ULL);

void HashBytes(size_t* hash, const void* data, size_t size) noexcept {
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t index = 0; index < size; ++index) {
        *hash ^= bytes[index];
        *hash *= kHashPrime;
    }
}

template <typename T>
void HashValue(size_t* hash, const T& value) noexcept {
    HashBytes(hash, &value, sizeof(value));
}

void HashIpAddress(size_t* hash, const packet::IpAddress& address) noexcept {
    const size_t address_kind = address.index();
    HashValue(hash, address_kind);
    if (const auto* ipv4 = std::get_if<packet::IPv4Address>(&address)) {
        HashBytes(hash, ipv4->bytes, sizeof(ipv4->bytes));
    } else if (const auto* ipv6 = std::get_if<packet::IPv6Address>(&address)) {
        HashBytes(hash, ipv6->bytes, sizeof(ipv6->bytes));
    }
}

bool IpAddressesEqual(const packet::IpAddress& left, const packet::IpAddress& right) noexcept {
    if (left.index() != right.index()) return false;
    if (const auto* left_ipv4 = std::get_if<packet::IPv4Address>(&left)) {
        const auto* right_ipv4 = std::get_if<packet::IPv4Address>(&right);
        return std::memcmp(left_ipv4->bytes, right_ipv4->bytes, sizeof(left_ipv4->bytes)) == 0;
    }
    if (const auto* left_ipv6 = std::get_if<packet::IPv6Address>(&left)) {
        const auto* right_ipv6 = std::get_if<packet::IPv6Address>(&right);
        return std::memcmp(left_ipv6->bytes, right_ipv6->bytes, sizeof(left_ipv6->bytes)) == 0;
    }
    return true;
}

bool EndpointsEqual(const NpmEndpoint& left, const NpmEndpoint& right) noexcept {
    return left.port == right.port && IpAddressesEqual(left.ip, right.ip);
}

int64_t SaturatingAdd(int64_t left, int64_t right) {
    if (right > 0 && left > std::numeric_limits<int64_t>::max() - right) {
        return std::numeric_limits<int64_t>::max();
    }
    if (right < 0 && left < std::numeric_limits<int64_t>::min() - right) {
        return std::numeric_limits<int64_t>::min();
    }
    return left + right;
}

int64_t SaturatingSubtract(int64_t left, int64_t right) {
    if (right > 0 && left < std::numeric_limits<int64_t>::min() + right) {
        return std::numeric_limits<int64_t>::min();
    }
    if (right < 0 && left > std::numeric_limits<int64_t>::max() + right) {
        return std::numeric_limits<int64_t>::max();
    }
    return left - right;
}

NpmAnalysisConfig ConfigWithSessionLimit(uint64_t max_active_sessions) {
    auto config = DefaultNpmAnalysisConfig(NpmRunMode::kOffline);
    config.max_active_sessions = max_active_sessions;
    return config;
}

int64_t IdleTimeout(const NpmAnalysisConfig& config, const NpmSessionKey& key) {
    return key.transport_protocol == ipv4::eNext::TCP ? config.tcp_idle_timeout_ns : config.udp_idle_timeout_ns;
}

void CountPacket(const NpmSessionPacketBinding& binding, const packet::PacketMeta& meta, uint64_t* packets_ab,
                 uint64_t* packets_ba, uint64_t* wire_bytes_ab, uint64_t* wire_bytes_ba) {
    if (binding.direction == NpmPacketDirection::kAToB) {
        ++*packets_ab;
        *wire_bytes_ab += meta.wire_len;
    } else {
        ++*packets_ba;
        *wire_bytes_ba += meta.wire_len;
    }
}

}  // namespace

size_t NpmSessionKeyHash::operator()(const NpmSessionKey& key) const noexcept {
    size_t hash = kHashOffset;
    HashBytes(&hash, key.input_namespace.data(), key.input_namespace.size());
    HashValue(&hash, key.observation_domain_id);
    HashValue(&hash, key.ip_family);
    HashValue(&hash, key.transport_protocol);
    HashIpAddress(&hash, key.a.ip);
    HashValue(&hash, key.a.port);
    HashIpAddress(&hash, key.b.ip);
    HashValue(&hash, key.b.port);
    return hash;
}

bool NpmSessionKeyEqual::operator()(const NpmSessionKey& left, const NpmSessionKey& right) const noexcept {
    return left.input_namespace == right.input_namespace && left.observation_domain_id == right.observation_domain_id &&
           left.ip_family == right.ip_family && left.transport_protocol == right.transport_protocol &&
           EndpointsEqual(left.a, right.a) && EndpointsEqual(left.b, right.b);
}

NpmSessionView NpmSessionSnapshot::View() const {
    NpmSessionView view;
    view.session_id = session_id;
    view.key = &key;
    view.first_ns = first_ns;
    view.last_ns = last_ns;
    view.packets_ab = packets_ab;
    view.packets_ba = packets_ba;
    view.wire_bytes_ab = wire_bytes_ab;
    view.wire_bytes_ba = wire_bytes_ba;
    view.primary_label_id = primary_label_id;
    view.protocol_status = protocol_status;
    view.protocol_id = protocol_id;
    view.protocol_sub_id = protocol_sub_id;
    return view;
}

NpmSessionTable::NpmSessionTable(uint64_t max_active_sessions)
    : NpmSessionTable(ConfigWithSessionLimit(max_active_sessions)) {}

NpmSessionTable::NpmSessionTable(const NpmAnalysisConfig& config)
    : NpmSessionTable(config, std::make_shared<NpmTaskBudget>(config)) {}

NpmSessionTable::NpmSessionTable(const NpmAnalysisConfig& config, std::shared_ptr<INpmTaskBudget> budget)
    : config_(config), max_active_sessions_(config.max_active_sessions), budget_(std::move(budget)) {
    if (!budget_) budget_ = std::make_shared<NpmTaskBudget>(config);
}

NpmSessionTable::~NpmSessionTable() { ReleaseSession(tracked_session_bytes_); }

NpmSessionView NpmSessionTable::MakeView(const SessionMap::value_type& entry) {
    NpmSessionView view;
    view.session_id = entry.second.session_id;
    view.key = &entry.first;
    view.first_ns = entry.second.first_ns;
    view.last_ns = entry.second.last_ns;
    view.packets_ab = entry.second.packets_ab;
    view.packets_ba = entry.second.packets_ba;
    view.wire_bytes_ab = entry.second.wire_bytes_ab;
    view.wire_bytes_ba = entry.second.wire_bytes_ba;
    view.primary_label_id = entry.second.primary_label_id;
    view.protocol_status = entry.second.protocol_status;
    view.protocol_id = entry.second.protocol_id;
    view.protocol_sub_id = entry.second.protocol_sub_id;
    return view;
}

NpmSessionSnapshot NpmSessionTable::MakeSnapshot(const SessionMap::value_type& entry, NpmSessionEndReason reason) {
    NpmSessionSnapshot snapshot;
    snapshot.key = entry.first;
    snapshot.session_id = entry.second.session_id;
    snapshot.first_ns = entry.second.first_ns;
    snapshot.last_ns = entry.second.last_ns;
    snapshot.packets_ab = entry.second.packets_ab;
    snapshot.packets_ba = entry.second.packets_ba;
    snapshot.wire_bytes_ab = entry.second.wire_bytes_ab;
    snapshot.wire_bytes_ba = entry.second.wire_bytes_ba;
    snapshot.primary_label_id = entry.second.primary_label_id;
    snapshot.protocol_status = entry.second.protocol_status;
    if (snapshot.protocol_status == NpmProtocolStatus::kPending) {
        snapshot.protocol_status = NpmProtocolStatus::kUnknown;
    }
    snapshot.protocol_id = entry.second.protocol_id;
    snapshot.protocol_sub_id = entry.second.protocol_sub_id;
    snapshot.end_reason = reason;
    return snapshot;
}

uint64_t NpmSessionTable::EstimateTrackedBytes(const NpmSessionKey& key) noexcept {
    constexpr uint64_t fixed_bytes =
        sizeof(SessionMap::value_type) + sizeof(DeadlineMap::value_type) + 8 * sizeof(void*);
    const uint64_t namespace_bytes = static_cast<uint64_t>(key.input_namespace.size());
    if (namespace_bytes >= (std::numeric_limits<uint64_t>::max() - fixed_bytes) / 2) {
        return std::numeric_limits<uint64_t>::max();
    }
    return fixed_bytes + 2 * (namespace_bytes + 1);
}

NpmBudgetError NpmSessionTable::ReserveSession(uint64_t bytes) {
    const auto result = budget_->Reserve(NpmBudgetCategory::kSessionState, bytes);
    if (result == NpmBudgetError::kNone) tracked_session_bytes_ += bytes;
    return result;
}

void NpmSessionTable::ReleaseSession(uint64_t bytes) noexcept {
    if (bytes == 0) return;
    if (budget_->Release(NpmBudgetCategory::kSessionState, bytes) == NpmBudgetError::kNone) {
        tracked_session_bytes_ -= bytes;
    }
}

NpmSessionTableError NpmSessionTable::Observe(const NpmSessionPacketBinding& binding, const packet::PacketMeta& meta,
                                              NpmSessionObserveResult* output, uint32_t new_session_primary_label_id) {
    if (!output) return NpmSessionTableError::kNullOutput;
    if (binding.direction != NpmPacketDirection::kAToB && binding.direction != NpmPacketDirection::kBToA) {
        return NpmSessionTableError::kInvalidDirection;
    }
    if (watermark_initialized_ && meta.timestamp_ns < watermark_ns_) {
        return NpmSessionTableError::kLatePacket;
    }

    const auto& tcp = binding.transport.tcp;
    const bool is_tcp = binding.key.transport_protocol == ipv4::eNext::TCP && tcp.valid;
    const bool bare_syn = is_tcp && tcp.syn && !tcp.ack;
    auto iterator = sessions_.find(binding.key);
    const bool tuple_reuse =
        iterator != sessions_.end() && bare_syn &&
        (!iterator->second.initial_syn_observed || iterator->second.initial_syn_direction != binding.direction ||
         iterator->second.initial_syn_sequence != tcp.sequence);

    const bool new_key = iterator == sessions_.end();
    if (new_key || tuple_reuse) {
        if (new_key && sessions_.size() >= max_active_sessions_) {
            return NpmSessionTableError::kSessionLimitExceeded;
        }
        if (next_session_id_ == 0) return NpmSessionTableError::kSessionIdExhausted;

        const uint64_t charge = new_key ? EstimateTrackedBytes(binding.key) : iterator->second.tracked_bytes;
        bool reserved = false;
        if (new_key) {
            if (ReserveSession(charge) != NpmBudgetError::kNone) {
                return NpmSessionTableError::kSessionBudgetExceeded;
            }
            reserved = true;
        }

        try {
            NpmSessionObserveResult result;
            if (tuple_reuse) {
                result.ended_sessions.push_back(MakeSnapshot(*iterator, NpmSessionEndReason::kTupleReuse));
            }

            State state;
            state.session_id = next_session_id_;
            state.first_ns = meta.timestamp_ns;
            state.last_ns = meta.timestamp_ns;
            state.primary_label_id = new_session_primary_label_id;
            state.idle_deadline_ns = SaturatingAdd(meta.timestamp_ns, IdleTimeout(config_, binding.key));
            state.tracked_bytes = charge;
            if (bare_syn) {
                state.initial_syn_observed = true;
                state.initial_syn_direction = binding.direction;
                state.initial_syn_sequence = tcp.sequence;
            }
            if (is_tcp && tcp.fin) {
                state.fin_ab = binding.direction == NpmPacketDirection::kAToB;
                state.fin_ba = binding.direction == NpmPacketDirection::kBToA;
            }
            CountPacket(binding, meta, &state.packets_ab, &state.packets_ba, &state.wire_bytes_ab,
                        &state.wire_bytes_ba);

            const bool closed = is_tcp && (tcp.rst || (state.fin_ab && state.fin_ba));
            if (closed) {
                SessionMap::value_type staged_entry(binding.key, state);
                result.ended_sessions.push_back(MakeSnapshot(staged_entry, NpmSessionEndReason::kClosed));
                if (tuple_reuse) {
                    deadlines_.erase(DeadlineKey{iterator->second.idle_deadline_ns, iterator->second.session_id});
                    sessions_.erase(iterator);
                }
                ReleaseSession(charge);
                reserved = false;
                ++next_session_id_;
                *output = std::move(result);
                return NpmSessionTableError::kNone;
            }

            if (new_key) {
                iterator = sessions_.emplace(binding.key, state).first;
                try {
                    deadlines_.emplace(DeadlineKey{state.idle_deadline_ns, state.session_id}, iterator->first);
                } catch (const std::bad_alloc&) {
                    sessions_.erase(iterator);
                    throw;
                }
                reserved = false;
            } else {
                deadlines_.emplace(DeadlineKey{state.idle_deadline_ns, state.session_id}, iterator->first);
                deadlines_.erase(DeadlineKey{iterator->second.idle_deadline_ns, iterator->second.session_id});
                iterator->second = state;
            }

            ++next_session_id_;
            result.has_active_session = true;
            result.active_session = MakeView(*iterator);
            *output = std::move(result);
            return NpmSessionTableError::kNone;
        } catch (const std::bad_alloc&) {
            if (reserved) ReleaseSession(charge);
            return NpmSessionTableError::kAllocationFailed;
        }
    }

    State state = iterator->second;
    state.first_ns = std::min(state.first_ns, meta.timestamp_ns);
    const bool deadline_changed = meta.timestamp_ns > state.last_ns;
    const DeadlineKey old_deadline{state.idle_deadline_ns, state.session_id};
    if (deadline_changed) {
        state.last_ns = meta.timestamp_ns;
        state.idle_deadline_ns = SaturatingAdd(state.last_ns, IdleTimeout(config_, iterator->first));
    }
    CountPacket(binding, meta, &state.packets_ab, &state.packets_ba, &state.wire_bytes_ab, &state.wire_bytes_ba);
    if (is_tcp && tcp.fin) {
        if (binding.direction == NpmPacketDirection::kAToB) {
            state.fin_ab = true;
        } else {
            state.fin_ba = true;
        }
    }

    NpmSessionObserveResult result;
    if (is_tcp && (tcp.rst || (state.fin_ab && state.fin_ba))) {
        try {
            SessionMap::value_type staged_entry(iterator->first, state);
            result.ended_sessions.push_back(MakeSnapshot(staged_entry, NpmSessionEndReason::kClosed));
        } catch (const std::bad_alloc&) {
            return NpmSessionTableError::kAllocationFailed;
        }
        const uint64_t charge = iterator->second.tracked_bytes;
        deadlines_.erase(old_deadline);
        sessions_.erase(iterator);
        ReleaseSession(charge);
    } else {
        if (deadline_changed) {
            const DeadlineKey new_deadline{state.idle_deadline_ns, state.session_id};
            if (new_deadline != old_deadline) {
                try {
                    deadlines_.emplace(new_deadline, iterator->first);
                } catch (const std::bad_alloc&) {
                    return NpmSessionTableError::kAllocationFailed;
                }
                deadlines_.erase(old_deadline);
            }
        }
        iterator->second = state;
        result.has_active_session = true;
        result.active_session = MakeView(*iterator);
    }
    *output = std::move(result);
    return NpmSessionTableError::kNone;
}

NpmSessionAdmissionPlanner::NpmSessionAdmissionPlanner(const NpmSessionTable& sessions) noexcept
    : sessions_(&sessions),
      watermark_initialized_(sessions.watermark_initialized_),
      watermark_ns_(sessions.watermark_ns_) {}

NpmSessionTableError NpmSessionAdmissionPlanner::ObserveAndAdvance(const NpmSessionPacketBinding& binding,
                                                                   const packet::PacketMeta& meta,
                                                                   bool* requires_admission) {
    if (requires_admission == nullptr) return NpmSessionTableError::kNullOutput;
    if (binding.direction != NpmPacketDirection::kAToB && binding.direction != NpmPacketDirection::kBToA) {
        return NpmSessionTableError::kInvalidDirection;
    }
    if (watermark_initialized_ && meta.timestamp_ns < watermark_ns_) {
        return NpmSessionTableError::kLatePacket;
    }

    try {
        auto [iterator, inserted] = states_.try_emplace(binding.key);
        State& state = iterator->second;
        if (inserted) {
            const auto existing = sessions_->sessions_.find(binding.key);
            if (existing != sessions_->sessions_.end()) {
                state.active = true;
                state.last_ns = existing->second.last_ns;
                state.idle_deadline_ns = existing->second.idle_deadline_ns;
                state.initial_syn_observed = existing->second.initial_syn_observed;
                state.initial_syn_direction = existing->second.initial_syn_direction;
                state.initial_syn_sequence = existing->second.initial_syn_sequence;
                state.fin_ab = existing->second.fin_ab;
                state.fin_ba = existing->second.fin_ba;
            }
        }
        if (state.active && watermark_initialized_ && state.idle_deadline_ns <= watermark_ns_) {
            state = {};
        }

        const auto& tcp = binding.transport.tcp;
        const bool is_tcp = binding.key.transport_protocol == ipv4::eNext::TCP && tcp.valid;
        const bool bare_syn = is_tcp && tcp.syn && !tcp.ack;
        const bool tuple_reuse = state.active && bare_syn &&
                                 (!state.initial_syn_observed || state.initial_syn_direction != binding.direction ||
                                  state.initial_syn_sequence != tcp.sequence);
        *requires_admission = !state.active || tuple_reuse;

        if (*requires_admission) {
            state = {};
            state.active = true;
            state.last_ns = meta.timestamp_ns;
            state.idle_deadline_ns = SaturatingAdd(meta.timestamp_ns, IdleTimeout(sessions_->config_, binding.key));
            if (bare_syn) {
                state.initial_syn_observed = true;
                state.initial_syn_direction = binding.direction;
                state.initial_syn_sequence = tcp.sequence;
            }
            if (is_tcp && tcp.fin) {
                state.fin_ab = binding.direction == NpmPacketDirection::kAToB;
                state.fin_ba = binding.direction == NpmPacketDirection::kBToA;
            }
        } else {
            if (meta.timestamp_ns > state.last_ns) {
                state.last_ns = meta.timestamp_ns;
                state.idle_deadline_ns = SaturatingAdd(state.last_ns, IdleTimeout(sessions_->config_, binding.key));
            }
            if (is_tcp && tcp.fin) {
                if (binding.direction == NpmPacketDirection::kAToB) {
                    state.fin_ab = true;
                } else {
                    state.fin_ba = true;
                }
            }
        }

        if (is_tcp && (tcp.rst || (state.fin_ab && state.fin_ba))) state = {};

        return AdvanceControl(meta.timestamp_ns);
    } catch (const std::bad_alloc&) {
        return NpmSessionTableError::kAllocationFailed;
    }
}

NpmSessionTableError NpmSessionAdmissionPlanner::AdvanceControl(int64_t timestamp_ns) {
    if (watermark_initialized_ && timestamp_ns < watermark_ns_) return NpmSessionTableError::kLatePacket;
    if (sessions_->config_.run_mode == NpmRunMode::kOffline) {
        const int64_t candidate = SaturatingSubtract(timestamp_ns, sessions_->config_.out_of_order_tolerance_ns);
        if (!watermark_initialized_ || candidate > watermark_ns_) {
            watermark_initialized_ = true;
            watermark_ns_ = candidate;
        }
    }
    return NpmSessionTableError::kNone;
}

NpmSessionTableError NpmSessionTable::SampleProtocol(const NpmSessionKey& key, uint64_t session_id,
                                                     const NpmPacketView& packet,
                                                     packet::IPacketProtocolIdentifier& identifier,
                                                     NpmSessionView* output) {
    if (!output) return NpmSessionTableError::kNullOutput;
    auto iterator = sessions_.find(key);
    if (iterator == sessions_.end()) return NpmSessionTableError::kNotFound;

    State& state = iterator->second;
    if (state.session_id != session_id) return NpmSessionTableError::kSessionInstanceMismatch;
    if (state.protocol_status != NpmProtocolStatus::kPending || packet.payload.empty()) {
        *output = MakeView(*iterator);
        return NpmSessionTableError::kNone;
    }
    if (packet.payload.data == nullptr || packet.layer == nullptr) {
        return NpmSessionTableError::kInvalidPacketView;
    }

    const auto identified = identifier.Identify(packet.packet, *packet.layer);
    ++state.payload_samples;
    if (identified.status == packet::ProtocolStatus::kIdentified && identified.id != 0) {
        state.protocol_status = NpmProtocolStatus::kIdentified;
        state.protocol_id = identified.id;
        if (identified.sub_id != 0) {
            state.protocol_sub_id = identified.sub_id;
        } else {
            state.protocol_sub_id.reset();
        }
    } else if (state.payload_samples >= config_.payload_sample_packets) {
        state.protocol_status = NpmProtocolStatus::kUnknown;
        state.protocol_id.reset();
        state.protocol_sub_id.reset();
    }

    *output = MakeView(*iterator);
    return NpmSessionTableError::kNone;
}

NpmSessionTableError NpmSessionTable::Find(const NpmSessionKey& key, NpmSessionView* output) const {
    if (!output) return NpmSessionTableError::kNullOutput;
    const auto iterator = sessions_.find(key);
    if (iterator == sessions_.end()) return NpmSessionTableError::kNotFound;
    *output = MakeView(*iterator);
    return NpmSessionTableError::kNone;
}

NpmSessionTableError NpmSessionTable::SnapshotActive(std::vector<NpmSessionView>* output) const {
    if (!output) return NpmSessionTableError::kNullOutput;
    try {
        std::vector<NpmSessionView> views;
        views.reserve(sessions_.size());
        for (const auto& session : sessions_) views.push_back(MakeView(session));
        std::sort(views.begin(), views.end(), [](const NpmSessionView& left, const NpmSessionView& right) {
            return left.session_id < right.session_id;
        });
        *output = std::move(views);
        return NpmSessionTableError::kNone;
    } catch (const std::bad_alloc&) {
        return NpmSessionTableError::kAllocationFailed;
    }
}

NpmCaptureProgressResult NpmSessionTable::AdvanceCaptureProgress(const NpmCaptureProgressUpdate& update) {
    NpmCaptureProgressResult result;
    result.watermark_initialized = watermark_initialized_;
    result.watermark_ns = watermark_ns_;

    if (config_.run_mode == NpmRunMode::kRealtime) {
        if (!update.source_backlog_known) {
            result.disposition = NpmCaptureProgressDisposition::kDeferredBacklogUnknown;
            return result;
        }
        if (update.source_has_backlog) {
            result.disposition = NpmCaptureProgressDisposition::kDeferredBacklogged;
            return result;
        }
        if (!update.packet_observed && !update.source_idle_confirmed) {
            result.disposition = NpmCaptureProgressDisposition::kDeferredIdleUnconfirmed;
            return result;
        }
    }

    const int64_t candidate_watermark = SaturatingSubtract(update.capture_time_ns, config_.out_of_order_tolerance_ns);
    if (watermark_initialized_ && candidate_watermark <= watermark_ns_) {
        result.disposition = NpmCaptureProgressDisposition::kUnchanged;
        return result;
    }

    watermark_initialized_ = true;
    watermark_ns_ = candidate_watermark;
    result.disposition = NpmCaptureProgressDisposition::kAdvanced;
    result.watermark_initialized = true;
    result.watermark_ns = watermark_ns_;

    auto deadline = deadlines_.begin();
    while (deadline != deadlines_.end() && deadline->first.first <= watermark_ns_) {
        const auto session = sessions_.find(deadline->second);
        if (session != sessions_.end()) {
            result.ended_sessions.push_back(MakeSnapshot(*session, NpmSessionEndReason::kIdleTimeout));
            const uint64_t charge = session->second.tracked_bytes;
            sessions_.erase(session);
            ReleaseSession(charge);
        }
        deadline = deadlines_.erase(deadline);
    }
    return result;
}

NpmSessionTableError NpmSessionTable::FinishAllAtEof(std::vector<NpmSessionSnapshot>* output) {
    if (output == nullptr) return NpmSessionTableError::kNullOutput;

    try {
        std::vector<NpmSessionSnapshot> snapshots;
        snapshots.reserve(sessions_.size());
        for (const auto& session : sessions_) {
            snapshots.push_back(MakeSnapshot(session, NpmSessionEndReason::kEof));
        }
        std::sort(snapshots.begin(), snapshots.end(),
                  [](const auto& left, const auto& right) { return left.session_id < right.session_id; });

        const uint64_t tracked_bytes = tracked_session_bytes_;
        sessions_.clear();
        deadlines_.clear();
        ReleaseSession(tracked_bytes);
        *output = std::move(snapshots);
        return NpmSessionTableError::kNone;
    } catch (const std::bad_alloc&) {
        return NpmSessionTableError::kAllocationFailed;
    }
}

int NotifyNpmSessionEnd(const std::vector<NpmSessionSnapshot>& ended_sessions,
                        const std::vector<INpmAnalysisModule*>& modules, int64_t observed_at_ns,
                        INpmResultWriter& writer) {
    for (const auto& snapshot : ended_sessions) {
        const auto view = snapshot.View();
        for (auto* module : modules) {
            const int error = module->OnSessionEnd(view, snapshot.end_reason, observed_at_ns, writer);
            if (error != 0) return error;
        }
    }
    return 0;
}

}  // namespace flowsql::npm
