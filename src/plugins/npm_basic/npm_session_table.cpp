// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_session_table.h"

#include <common/network/netbase.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
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

void CountPacket(const NpmSessionPacketBinding& binding,
                 const packet::PacketMeta& meta,
                 uint64_t* packets_ab,
                 uint64_t* packets_ba,
                 uint64_t* wire_bytes_ab,
                 uint64_t* wire_bytes_ba) {
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
    return left.input_namespace == right.input_namespace &&
           left.observation_domain_id == right.observation_domain_id && left.ip_family == right.ip_family &&
           left.transport_protocol == right.transport_protocol && EndpointsEqual(left.a, right.a) &&
           EndpointsEqual(left.b, right.b);
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
    view.protocol_status = protocol_status;
    return view;
}

NpmSessionTable::NpmSessionTable(uint64_t max_active_sessions)
    : NpmSessionTable(ConfigWithSessionLimit(max_active_sessions)) {}

NpmSessionTable::NpmSessionTable(const NpmAnalysisConfig& config)
    : config_(config), max_active_sessions_(config.max_active_sessions) {}

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
    view.protocol_status = NpmProtocolStatus::kPending;
    return view;
}

NpmSessionSnapshot NpmSessionTable::MakeSnapshot(const SessionMap::value_type& entry,
                                                 NpmSessionEndReason reason) {
    NpmSessionSnapshot snapshot;
    snapshot.key = entry.first;
    snapshot.session_id = entry.second.session_id;
    snapshot.first_ns = entry.second.first_ns;
    snapshot.last_ns = entry.second.last_ns;
    snapshot.packets_ab = entry.second.packets_ab;
    snapshot.packets_ba = entry.second.packets_ba;
    snapshot.wire_bytes_ab = entry.second.wire_bytes_ab;
    snapshot.wire_bytes_ba = entry.second.wire_bytes_ba;
    snapshot.protocol_status = NpmProtocolStatus::kPending;
    snapshot.end_reason = reason;
    return snapshot;
}

NpmSessionTableError NpmSessionTable::Observe(const NpmSessionPacketBinding& binding,
                                              const packet::PacketMeta& meta,
                                              NpmSessionObserveResult* output) {
    if (!output) return NpmSessionTableError::kNullOutput;
    if (binding.direction != NpmPacketDirection::kAToB && binding.direction != NpmPacketDirection::kBToA) {
        return NpmSessionTableError::kInvalidDirection;
    }
    if (watermark_initialized_ && meta.timestamp_ns < watermark_ns_) {
        return NpmSessionTableError::kLatePacket;
    }

    const bool is_tcp = binding.key.transport_protocol == ipv4::eNext::TCP && binding.tcp.valid;
    const bool bare_syn = is_tcp && binding.tcp.syn && !binding.tcp.ack;
    auto iterator = sessions_.find(binding.key);
    const bool tuple_reuse =
        iterator != sessions_.end() && bare_syn &&
        (!iterator->second.initial_syn_observed || iterator->second.initial_syn_direction != binding.direction ||
         iterator->second.initial_syn_sequence != binding.tcp.sequence);

    if (iterator == sessions_.end() || tuple_reuse) {
        if (iterator == sessions_.end() && sessions_.size() >= max_active_sessions_) {
            return NpmSessionTableError::kSessionLimitExceeded;
        }
        if (next_session_id_ == 0) return NpmSessionTableError::kSessionIdExhausted;

        NpmSessionObserveResult result;
        if (tuple_reuse) {
            result.ended_sessions.push_back(MakeSnapshot(*iterator, NpmSessionEndReason::kTupleReuse));
            deadlines_.erase(DeadlineKey{iterator->second.idle_deadline_ns, iterator->second.session_id});
            sessions_.erase(iterator);
        }

        State state;
        state.session_id = next_session_id_;
        state.first_ns = meta.timestamp_ns;
        state.last_ns = meta.timestamp_ns;
        state.idle_deadline_ns = SaturatingAdd(meta.timestamp_ns, IdleTimeout(config_, binding.key));
        if (bare_syn) {
            state.initial_syn_observed = true;
            state.initial_syn_direction = binding.direction;
            state.initial_syn_sequence = binding.tcp.sequence;
        }
        if (is_tcp && binding.tcp.fin) {
            state.fin_ab = binding.direction == NpmPacketDirection::kAToB;
            state.fin_ba = binding.direction == NpmPacketDirection::kBToA;
        }
        CountPacket(binding,
                    meta,
                    &state.packets_ab,
                    &state.packets_ba,
                    &state.wire_bytes_ab,
                    &state.wire_bytes_ba);
        iterator = sessions_.emplace(binding.key, state).first;
        deadlines_.emplace(DeadlineKey{state.idle_deadline_ns, state.session_id}, iterator->first);
        ++next_session_id_;

        if (is_tcp && (binding.tcp.rst || (state.fin_ab && state.fin_ba))) {
            result.ended_sessions.push_back(MakeSnapshot(*iterator, NpmSessionEndReason::kClosed));
            deadlines_.erase(DeadlineKey{state.idle_deadline_ns, state.session_id});
            sessions_.erase(iterator);
        } else {
            result.has_active_session = true;
            result.active_session = MakeView(*iterator);
        }
        *output = std::move(result);
        return NpmSessionTableError::kNone;
    } else {
        State& state = iterator->second;
        state.first_ns = std::min(state.first_ns, meta.timestamp_ns);
        if (meta.timestamp_ns > state.last_ns) {
            deadlines_.erase(DeadlineKey{state.idle_deadline_ns, state.session_id});
            state.last_ns = meta.timestamp_ns;
            state.idle_deadline_ns = SaturatingAdd(state.last_ns, IdleTimeout(config_, iterator->first));
            deadlines_.emplace(DeadlineKey{state.idle_deadline_ns, state.session_id}, iterator->first);
        }
        CountPacket(binding,
                    meta,
                    &state.packets_ab,
                    &state.packets_ba,
                    &state.wire_bytes_ab,
                    &state.wire_bytes_ba);
        if (is_tcp && binding.tcp.fin) {
            if (binding.direction == NpmPacketDirection::kAToB) {
                state.fin_ab = true;
            } else {
                state.fin_ba = true;
            }
        }
    }

    NpmSessionObserveResult result;
    const State& state = iterator->second;
    if (is_tcp && (binding.tcp.rst || (state.fin_ab && state.fin_ba))) {
        result.ended_sessions.push_back(MakeSnapshot(*iterator, NpmSessionEndReason::kClosed));
        deadlines_.erase(DeadlineKey{state.idle_deadline_ns, state.session_id});
        sessions_.erase(iterator);
    } else {
        result.has_active_session = true;
        result.active_session = MakeView(*iterator);
    }
    *output = std::move(result);
    return NpmSessionTableError::kNone;
}

NpmSessionTableError NpmSessionTable::Find(const NpmSessionKey& key, NpmSessionView* output) const {
    if (!output) return NpmSessionTableError::kNullOutput;
    const auto iterator = sessions_.find(key);
    if (iterator == sessions_.end()) return NpmSessionTableError::kNotFound;
    *output = MakeView(*iterator);
    return NpmSessionTableError::kNone;
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

    const int64_t candidate_watermark =
        SaturatingSubtract(update.capture_time_ns, config_.out_of_order_tolerance_ns);
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
            sessions_.erase(session);
        }
        deadline = deadlines_.erase(deadline);
    }
    return result;
}

int NotifyNpmSessionEnd(const std::vector<NpmSessionSnapshot>& ended_sessions,
                        const std::vector<INpmAnalysisModule*>& modules,
                        INpmResultWriter& writer) {
    for (const auto& snapshot : ended_sessions) {
        const auto view = snapshot.View();
        for (auto* module : modules) {
            const int error = module->OnSessionEnd(view, snapshot.end_reason, writer);
            if (error != 0) return error;
        }
    }
    return 0;
}

}  // namespace flowsql::npm
