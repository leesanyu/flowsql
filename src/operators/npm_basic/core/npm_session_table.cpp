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
#include <type_traits>
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

template <typename Key>
int64_t IdleTimeout(const NpmAnalysisConfig& config, const Key& key) {
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

namespace {
template <typename Key>
size_t HashKey(const Key& key) noexcept {
    size_t hash = kHashOffset;
    HashBytes(&hash, key.input_namespace.data(), key.input_namespace.size());
    HashValue(&hash, key.observation_domain_id);
    HashValue(&hash, key.transport_protocol);
    HashBytes(&hash, key.a_ip.bytes, sizeof(key.a_ip.bytes));
    HashValue(&hash, key.a_port);
    HashBytes(&hash, key.b_ip.bytes, sizeof(key.b_ip.bytes));
    HashValue(&hash, key.b_port);
    return hash;
}
template <typename Key>
bool EqualKey(const Key& left, const Key& right) noexcept {
    return left.input_namespace == right.input_namespace && left.observation_domain_id == right.observation_domain_id &&
           left.transport_protocol == right.transport_protocol && left.a_port == right.a_port &&
           left.b_port == right.b_port && left.a_ip == right.a_ip && left.b_ip == right.b_ip;
}
}  // namespace
size_t NpmSessionKeyHash::operator()(const NpmSessionKey4& key) const noexcept { return HashKey(key); }
size_t NpmSessionKeyHash::operator()(const NpmSessionKey6& key) const noexcept { return HashKey(key); }
bool NpmSessionKeyEqual::operator()(const NpmSessionKey4& left, const NpmSessionKey4& right) const noexcept {
    return EqualKey(left, right);
}
bool NpmSessionKeyEqual::operator()(const NpmSessionKey6& left, const NpmSessionKey6& right) const noexcept {
    return EqualKey(left, right);
}

struct NpmSessionSnapshot::Lease {
    std::shared_ptr<INpmTaskBudget> budget;
    uint64_t bytes;
    Lease(std::shared_ptr<INpmTaskBudget> owner, uint64_t charge) : budget(std::move(owner)), bytes(charge) {}
    ~Lease() { budget->Release(NpmBudgetCategory::kInputBatch, bytes); }
};
void NpmSessionSnapshot::ReserveBudget(std::shared_ptr<INpmTaskBudget> budget, uint64_t bytes) {
    if (budget->Reserve(NpmBudgetCategory::kInputBatch, bytes) != NpmBudgetError::kNone) throw std::bad_alloc();
    try {
        lease_ = std::make_shared<Lease>(budget, bytes);
    } catch (...) {
        budget->Release(NpmBudgetCategory::kInputBatch, bytes);
        throw;
    }
}
NpmSessionSnapshot::NpmSessionSnapshot(const NpmSessionSnapshot& other) : NpmSessionSnapshotFacts(other) {
    if (other.lease_) ReserveBudget(other.lease_->budget, other.lease_->bytes);
    key = other.key;
}
NpmSessionSnapshot& NpmSessionSnapshot::operator=(const NpmSessionSnapshot& other) {
    if (this != &other) {
        NpmSessionSnapshot copy(other);
        *this = std::move(copy);
    }
    return *this;
}

NpmSessionView NpmSessionSnapshot::View() const& {
    NpmSessionView view;
    view.session_id = session_id;
    if (key) view.key = key.View();
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

NpmSessionTable::~NpmSessionTable() {
    sessions4_.clear();
    sessions6_.clear();
    deadlines_.clear();
    ids_.clear();
    ReleaseSession(tracked_session_bytes_);
}

template <typename Entry>
NpmSessionView NpmSessionTable::MakeView(const Entry& entry) {
    NpmSessionView view;
    view.session_id = entry.second.session_id;
    view.key = NpmSessionKeyView(entry.first);
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

template <typename Entry>
NpmSessionSnapshot NpmSessionTable::MakeSnapshot(const Entry& entry, NpmSessionEndReason reason) {
    NpmSessionSnapshot snapshot;
    const auto key_view = NpmSessionKeyView(entry.first);
    snapshot.ReserveBudget(budget_, 2 * sizeof(NpmSessionSnapshot) + sizeof(std::shared_ptr<INpmTaskBudget>) +
                                        sizeof(uint64_t) + 2 * sizeof(void*) + key_view.OwnedKeyBytes());
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

template <typename Key>
uint64_t NpmSessionTable::EstimateTrackedBytes(const Key& key) noexcept {
    // Concrete map node, ID index, deadline tree node, and conservative bucket/node links. No duplicated key.
    constexpr uint64_t fixed = sizeof(typename FamilyMap<Key>::value_type) + sizeof(IdMap::value_type) +
                               sizeof(DeadlineMap::value_type) + 10 * sizeof(void*);
    return fixed + key.input_namespace.capacity() + 1;
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
                                              NpmSessionObserveResult* output, uint32_t label) {
    if (!binding.key) return NpmSessionTableError::kInvalidPacketView;
    if (const auto* key = binding.key.Get4()) return ObserveFamily(binding, *key, sessions4_, meta, output, label);
    return ObserveFamily(binding, *binding.key.Get6(), sessions6_, meta, output, label);
}

template <typename Key>
NpmSessionTableError NpmSessionTable::ObserveFamily(const NpmSessionPacketBinding& binding, const Key& key,
                                                    FamilyMap<Key>& sessions, const packet::PacketMeta& meta,
                                                    NpmSessionObserveResult* output,
                                                    uint32_t new_session_primary_label_id) {
    if (!output) return NpmSessionTableError::kNullOutput;
    if (binding.direction != NpmPacketDirection::kAToB && binding.direction != NpmPacketDirection::kBToA) {
        return NpmSessionTableError::kInvalidDirection;
    }
    if (watermark_initialized_ && meta.timestamp_ns < watermark_ns_) {
        return NpmSessionTableError::kLatePacket;
    }

    const auto& tcp = binding.transport.tcp;
    const bool is_tcp = key.transport_protocol == ipv4::eNext::TCP && tcp.valid;
    const bool bare_syn = is_tcp && tcp.syn && !tcp.ack;
    auto iterator = sessions.find(key);
    const bool tuple_reuse =
        iterator != sessions.end() && bare_syn &&
        (!iterator->second.initial_syn_observed || iterator->second.initial_syn_direction != binding.direction ||
         iterator->second.initial_syn_sequence != tcp.sequence);

    const bool new_key = iterator == sessions.end();
    if (new_key || tuple_reuse) {
        if (new_key && size() >= max_active_sessions_) {
            return NpmSessionTableError::kSessionLimitExceeded;
        }
        if (next_session_id_ == 0) return NpmSessionTableError::kSessionIdExhausted;

        const uint64_t charge = new_key ? EstimateTrackedBytes(key) : iterator->second.tracked_bytes;
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
            state.idle_deadline_ns = SaturatingAdd(meta.timestamp_ns, IdleTimeout(config_, key));
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
                typename FamilyMap<Key>::value_type staged_entry(key, state);
                result.ended_sessions.push_back(MakeSnapshot(staged_entry, NpmSessionEndReason::kClosed));
                if (tuple_reuse) {
                    deadlines_.erase(DeadlineKey{iterator->second.idle_deadline_ns, iterator->second.session_id});
                    ids_.erase(iterator->second.session_id);
                    sessions.erase(iterator);
                }
                ReleaseSession(charge);
                reserved = false;
                ++next_session_id_;
                *output = std::move(result);
                return NpmSessionTableError::kNone;
            }

            if (new_key) {
                iterator = sessions.emplace(key, state).first;
                try {
                    deadlines_.emplace(DeadlineKey{state.idle_deadline_ns, state.session_id}, state.session_id);
                    ids_.emplace(state.session_id, NpmSessionKeyView(iterator->first));
                } catch (const std::bad_alloc&) {
                    deadlines_.erase(DeadlineKey{state.idle_deadline_ns, state.session_id});
                    ids_.erase(state.session_id);
                    sessions.erase(iterator);
                    throw;
                }
                reserved = false;
            } else {
                const DeadlineKey replacement{state.idle_deadline_ns, state.session_id};
                deadlines_.emplace(replacement, state.session_id);
                try {
                    ids_.emplace(state.session_id, NpmSessionKeyView(iterator->first));
                } catch (...) {
                    deadlines_.erase(replacement);
                    throw;
                }
                deadlines_.erase(DeadlineKey{iterator->second.idle_deadline_ns, iterator->second.session_id});
                ids_.erase(iterator->second.session_id);
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
            typename FamilyMap<Key>::value_type staged_entry(iterator->first, state);
            result.ended_sessions.push_back(MakeSnapshot(staged_entry, NpmSessionEndReason::kClosed));
        } catch (const std::bad_alloc&) {
            return NpmSessionTableError::kAllocationFailed;
        }
        const uint64_t charge = iterator->second.tracked_bytes;
        deadlines_.erase(old_deadline);
        ids_.erase(iterator->second.session_id);
        sessions.erase(iterator);
        ReleaseSession(charge);
    } else {
        if (deadline_changed) {
            const DeadlineKey new_deadline{state.idle_deadline_ns, state.session_id};
            if (new_deadline != old_deadline) {
                try {
                    deadlines_.emplace(new_deadline, state.session_id);
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

NpmSessionAdmissionPlanner::~NpmSessionAdmissionPlanner() {
    states4_.clear();
    states6_.clear();
    if (charged_bytes_) sessions_->Budget().Release(NpmBudgetCategory::kInputBatch, charged_bytes_);
}

NpmSessionTableError NpmSessionAdmissionPlanner::ObserveAndAdvance(const NpmSessionPacketBinding& binding,
                                                                   const packet::PacketMeta& meta,
                                                                   bool* requires_admission) {
    if (!binding.key) return NpmSessionTableError::kInvalidPacketView;
    if (const auto* key = binding.key.Get4()) return ObserveFamily(binding, *key, meta, requires_admission);
    return ObserveFamily(binding, *binding.key.Get6(), meta, requires_admission);
}

template <typename Key>
NpmSessionTableError NpmSessionAdmissionPlanner::ObserveFamily(const NpmSessionPacketBinding& binding, const Key& key,
                                                               const packet::PacketMeta& meta,
                                                               bool* requires_admission) {
    auto* states_ptr = [&]() {
        if constexpr (std::is_same_v<Key, NpmSessionKey4>)
            return &states4_;
        else
            return &states6_;
    }();
    auto& states = *states_ptr;
    const auto* actual_ptr = [&]() {
        if constexpr (std::is_same_v<Key, NpmSessionKey4>)
            return &sessions_->sessions4_;
        else
            return &sessions_->sessions6_;
    }();
    const auto& actual = *actual_ptr;
    if (requires_admission == nullptr) return NpmSessionTableError::kNullOutput;
    if (binding.direction != NpmPacketDirection::kAToB && binding.direction != NpmPacketDirection::kBToA) {
        return NpmSessionTableError::kInvalidDirection;
    }
    const auto progress_error = AdvanceControl(meta.timestamp_ns);
    if (progress_error != NpmSessionTableError::kNone) return progress_error;

    try {
        uint64_t reserved = 0;
        if (states.find(key) == states.end()) {
            reserved =
                sizeof(typename FamilyStates<Key>::value_type) + 4 * sizeof(void*) + key.input_namespace.capacity() + 1;
            if (sessions_->Budget().Reserve(NpmBudgetCategory::kInputBatch, reserved) != NpmBudgetError::kNone)
                return NpmSessionTableError::kSessionBudgetExceeded;
        }
        typename FamilyStates<Key>::iterator iterator;
        bool inserted;
        try {
            auto result = states.try_emplace(key);
            iterator = result.first;
            inserted = result.second;
        } catch (...) {
            if (reserved) sessions_->Budget().Release(NpmBudgetCategory::kInputBatch, reserved);
            throw;
        }
        charged_bytes_ += reserved;
        State& state = iterator->second;
        if (inserted) {
            const auto existing = actual.find(key);
            if (existing != actual.end()) {
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
        const bool is_tcp = key.transport_protocol == ipv4::eNext::TCP && tcp.valid;
        const bool bare_syn = is_tcp && tcp.syn && !tcp.ack;
        const bool tuple_reuse = state.active && bare_syn &&
                                 (!state.initial_syn_observed || state.initial_syn_direction != binding.direction ||
                                  state.initial_syn_sequence != tcp.sequence);
        *requires_admission = !state.active || tuple_reuse;

        if (*requires_admission) {
            state = {};
            state.active = true;
            state.last_ns = meta.timestamp_ns;
            state.idle_deadline_ns = SaturatingAdd(meta.timestamp_ns, IdleTimeout(sessions_->config_, key));
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
                state.idle_deadline_ns = SaturatingAdd(state.last_ns, IdleTimeout(sessions_->config_, key));
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

        return NpmSessionTableError::kNone;
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

template <typename Key>
NpmSessionTableError NpmSessionTable::SampleFamily(const Key& key, FamilyMap<Key>& sessions, uint64_t session_id,
                                                   const NpmPacketView& packet,
                                                   packet::IPacketFamilyProtocolIdentifier& identifier,
                                                   NpmSessionView* output) {
    if (!output) return NpmSessionTableError::kNullOutput;
    auto iterator = sessions.find(key);
    if (iterator == sessions.end()) return NpmSessionTableError::kNotFound;

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

NpmSessionTableError NpmSessionTable::SampleProtocol(NpmSessionKeyView key, uint64_t id, const NpmPacketView& packet,
                                                     packet::IPacketFamilyProtocolIdentifier& identifier,
                                                     NpmSessionView* output) {
    if (key.Get4()) return SampleFamily(*key.Get4(), sessions4_, id, packet, identifier, output);
    return SampleFamily(*key.Get6(), sessions6_, id, packet, identifier, output);
}

NpmSessionTableError NpmSessionTable::Find(NpmSessionKeyView key, NpmSessionView* output) const {
    if (!output) return NpmSessionTableError::kNullOutput;
    const auto find = [&](const auto& concrete, const auto& sessions) {
        const auto iterator = sessions.find(concrete);
        if (iterator == sessions.end()) return NpmSessionTableError::kNotFound;
        *output = MakeView(*iterator);
        return NpmSessionTableError::kNone;
    };
    return key.Get4() ? find(*key.Get4(), sessions4_) : find(*key.Get6(), sessions6_);
}

NpmSessionTableError NpmSessionTable::SnapshotActive(std::vector<NpmSessionView>* output) const {
    if (!output) return NpmSessionTableError::kNullOutput;
    try {
        std::vector<NpmSessionView> views;
        views.reserve(size());
        for (const auto& session : sessions4_) views.push_back(MakeView(session));
        for (const auto& session : sessions6_) views.push_back(MakeView(session));
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

    // Stage all owning snapshots before mutating event time or either family table.
    for (auto deadline = deadlines_.begin();
         deadline != deadlines_.end() && deadline->first.first <= candidate_watermark; ++deadline) {
        const auto handle = ids_.find(deadline->second);
        if (handle == ids_.end()) continue;
        const auto stage = [&](const auto& key, const auto& sessions) {
            const auto session = sessions.find(key);
            if (session != sessions.end() && session->second.session_id == deadline->second)
                result.ended_sessions.push_back(MakeSnapshot(*session, NpmSessionEndReason::kIdleTimeout));
        };
        if (handle->second.Get4())
            stage(*handle->second.Get4(), sessions4_);
        else
            stage(*handle->second.Get6(), sessions6_);
    }
    watermark_initialized_ = true;
    watermark_ns_ = candidate_watermark;
    result.disposition = NpmCaptureProgressDisposition::kAdvanced;
    result.watermark_initialized = true;
    result.watermark_ns = watermark_ns_;

    auto deadline = deadlines_.begin();
    while (deadline != deadlines_.end() && deadline->first.first <= watermark_ns_) {
        const auto handle = ids_.find(deadline->second);
        if (handle != ids_.end()) {
            const auto retire = [&](const auto& key, auto& sessions) {
                const auto session = sessions.find(key);
                if (session != sessions.end() && session->second.session_id == deadline->second) {
                    const uint64_t charge = session->second.tracked_bytes;
                    sessions.erase(session);
                    ReleaseSession(charge);
                }
            };
            if (handle->second.Get4())
                retire(*handle->second.Get4(), sessions4_);
            else
                retire(*handle->second.Get6(), sessions6_);
            ids_.erase(handle);
        }
        deadline = deadlines_.erase(deadline);
    }
    return result;
}

NpmSessionTableError NpmSessionTable::FinishAllAtEof(std::vector<NpmSessionSnapshot>* output) {
    if (output == nullptr) return NpmSessionTableError::kNullOutput;

    try {
        std::vector<NpmSessionSnapshot> snapshots;
        snapshots.reserve(size());
        for (const auto& session : sessions4_) {
            snapshots.push_back(MakeSnapshot(session, NpmSessionEndReason::kEof));
        }
        for (const auto& session : sessions6_) snapshots.push_back(MakeSnapshot(session, NpmSessionEndReason::kEof));
        std::sort(snapshots.begin(), snapshots.end(),
                  [](const auto& left, const auto& right) { return left.session_id < right.session_id; });

        const uint64_t tracked_bytes = tracked_session_bytes_;
        sessions4_.clear();
        sessions6_.clear();
        ids_.clear();
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
