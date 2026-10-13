// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include <operators/npm_basic/core/npm_session_table.h>
#include <operators/npm_basic/core/npm_task_budget.h>
#include <array>
#include <cassert>

namespace flowsql::npm {

// Test fixtures retain an owning key between assertions. Production uses only synchronous borrowed bindings.
struct NpmTestPacketBinding {
    NpmOwnedSessionKey key;
    NpmPacketDirection direction = NpmPacketDirection::kAToB;
    Span<const uint8_t> payload;
    NpmTransportPacketFacts transport;
    FlowLabelFactsV1 label_facts;
    operator NpmPacketBindingView() const {
        NpmPacketBindingView view;
        if (key) view.key = key.View();
        view.direction = direction;
        view.payload = payload;
        view.transport = transport;
        view.label_facts = label_facts;
        return view;
    }
};

inline NpmSessionPacketError BuildNpmTestPacketBinding(const NpmObservationDomainLookup& domains,
                                                       const packet::PacketView& packet,
                                                       const packet::PacketFamilyLayerView& layer,
                                                       NpmTestPacketBinding* output) {
    if (!output) return NpmSessionPacketError::kNullOutput;
    const auto build = [&](auto key) {
        NpmPacketBindingView view;
        const auto error = BuildNpmSessionPacketBinding(domains, packet, layer, &key, &view);
        if (error != NpmSessionPacketError::kNone) return error;
        NpmTestPacketBinding result;
        result.key = std::move(key);
        result.direction = view.direction;
        result.payload = view.payload;
        result.transport = view.transport;
        result.label_facts = view.label_facts;
        *output = std::move(result);
        return NpmSessionPacketError::kNone;
    };
    return layer.Get4() ? build(NpmSessionKey4{}) : build(NpmSessionKey6{});
}

// Mutable setup-only fixtures are captured into a real batch before crossing production APIs.
struct NpmTestSnapshot : NpmSessionSnapshotFacts {
    NpmOwnedSessionKey key;
    int64_t observed_at = 0;
    NpmSessionView View() const {
        NpmSessionView view;
        if (key) view.key = key.View();
        view.session_id = session_id;
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
};

// Existing sampling assertions create a token for their chosen fixture instance explicitly.
// Dedicated throughput tests exercise only Observe-returned tokens and invalidation/consumption.
struct NpmThroughputTestAccess {
    static uint64_t StorageBytes(const NpmSessionSnapshotBatch& batch) { return batch.StorageBytes(); }
    static std::array<size_t, 3> Capacities(const NpmSessionSnapshotBatch& batch) {
        return {batch.keys4_.capacity(), batch.keys6_.capacity(), batch.rows_.capacity()};
    }
    static NpmSessionSnapshotBatch Batch(std::initializer_list<NpmTestSnapshot> snapshots) {
        auto budget = std::make_shared<NpmTaskBudget>(DefaultNpmAnalysisConfig(NpmRunMode::kOffline));
        NpmSessionSnapshotBatch batch;
        size_t count4 = 0, count6 = 0;
        for (const auto& snapshot : snapshots) {
            if (snapshot.key.Get4())
                ++count4;
            else
                ++count6;
        }
        batch.Prepare(count4, count6, snapshots.size(), budget);
        for (const auto& snapshot : snapshots) {
            batch.Append(snapshot.View(), snapshot.end_reason, snapshot.observed_at);
        }
        return batch;
    }
    static NpmSessionAccess Token(NpmSessionTable& table, NpmSessionKeyView key, uint64_t id) {
        NpmSessionAccess result;
        const auto find = [&](auto& sessions, const auto& concrete) {
            const auto found = sessions.find(concrete);
            if (found != sessions.end()) result = table.MakeAccess(*found);
        };
        if (key.Get4())
            find(table.sessions4_, *key.Get4());
        else if (key.Get6())
            find(table.sessions6_, *key.Get6());
        result.session_id_ = id;
        return result;
    }
};

inline NpmSessionSnapshotBatch MakeNpmTestSnapshotBatch(std::initializer_list<NpmTestSnapshot> snapshots) {
    return NpmThroughputTestAccess::Batch(snapshots);
}
inline NpmSessionSnapshotBatch MakeNpmTestSnapshotBatch(size_t count) {
    NpmTestSnapshot snapshot;
    snapshot.key = NpmSessionKey4{};
    if (count == 1) return MakeNpmTestSnapshotBatch({snapshot});
    assert(count == 2);
    return MakeNpmTestSnapshotBatch({snapshot, snapshot});
}

}  // namespace flowsql::npm
