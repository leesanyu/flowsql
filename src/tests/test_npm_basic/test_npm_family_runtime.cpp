// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <operators/npm_basic/core/npm_task_budget.h>
#include <cassert>
#include <cstdio>
#include <type_traits>
#include "npm_packet_test_support.h"

using namespace flowsql::npm;
namespace p = flowsql::packet;

struct Identifier : p::IPacketFamilyProtocolIdentifier {
    p::PacketProtocolInfo Identify(const p::PacketView&, const p::PacketLayerFacts&) override { return {}; }
};

template <typename Key>
Key MakeKey(uint16_t port = 1000, uint8_t protocol = 17) {
    Key key;
    key.input_namespace = "mixed-capture";
    key.observation_domain_id = 7;
    key.a_ip.bytes[0] = 1;
    key.a_ip.bytes[1] = 2;
    key.a_ip.bytes[2] = 3;
    key.a_ip.bytes[3] = 4;
    key.b_ip.bytes[0] = 2;
    key.a_port = port;
    key.b_port = 53;
    key.transport_protocol = protocol;
    return key;
}
NpmSessionObserveResult Observe(NpmSessionTable& table, const NpmTestPacketBinding& binding, int64_t time) {
    NpmSessionObserveResult result;
    p::PacketMeta meta;
    meta.timestamp_ns = time;
    meta.wire_len = 100;
    assert(table.Observe(binding, meta, &result) == NpmSessionTableError::kNone);
    return result;
}

void TestSharedCapacityAndSnapshots() {
    auto config = DefaultNpmAnalysisConfig(NpmRunMode::kOffline);
    config.max_active_sessions = 2;
    auto budget = std::make_shared<NpmTaskBudget>(config);
    NpmSessionTable table(config, budget);
    NpmTestPacketBinding four, six;
    four.key = MakeKey<NpmSessionKey4>();
    six.key = MakeKey<NpmSessionKey6>();
    auto r4 = Observe(table, four, 10);
    const auto id4 = r4.active_session.session_id;
    const auto first_charge = table.tracked_bytes();
    auto r6 = Observe(table, six, 11);
    const auto id6 = r6.active_session.session_id;
    assert(id4 != id6 && id4 == 1 && id6 == 2 && table.size() == 2);
    assert(table.tracked_bytes() - first_charge >= first_charge + sizeof(NpmSessionKey6) - sizeof(NpmSessionKey4));
    assert(r4.active_session.key->Get4() && r6.active_session.key->Get6());
    four.direction = NpmPacketDirection::kBToA;
    auto reverse = Observe(table, four, 12);
    assert(reverse.active_session.session_id == id4 && reverse.active_session.packets_ab == 1 &&
           reverse.active_session.packets_ba == 1);
    NpmTestPacketBinding third;
    third.key = MakeKey<NpmSessionKey4>(1001);
    NpmSessionObserveResult denied;
    p::PacketMeta meta;
    meta.timestamp_ns = 13;
    const auto usage = table.tracked_bytes();
    assert(table.Observe(third, meta, &denied) == NpmSessionTableError::kSessionLimitExceeded);
    assert(table.size() == 2 && table.tracked_bytes() == usage);
    std::vector<NpmSessionView> active;
    assert(table.SnapshotActive(&active) == NpmSessionTableError::kNone && active.size() == 2);
    assert(active[0].session_id == id4 && active[1].session_id == id6);
    NpmSessionSnapshotBatch ended;
    assert(table.FinishAllAtEof(&ended) == NpmSessionTableError::kNone && ended.size() == 2);
    assert(table.size() == 0 && table.tracked_bytes() == 0 && budget->Usage().session_state_bytes == 0 &&
           !table.NextEventDeadlineNs());
    four.key = {};
    six.key = {};
    assert(ended.View(0).key->Get4()->a_ip.ToUInt32() == 0x01020304 && ended.View(1).key->Get6()->a_ip.bytes[3] == 4);
    const auto retained = budget->Usage().input_batch_bytes;
    assert(retained > 0);
    NpmSessionSnapshotBatch copy;
    assert(ended.Clone(&copy) == NpmSessionTableError::kNone);
    assert(budget->Usage().input_batch_bytes == 2 * retained);
    assert(copy.View(0).key->Get4() != ended.View(0).key->Get4());
    ended.Clear();
    assert(copy.View(0).key->Namespace() == "mixed-capture");
    assert(table.FinishAllAtEof(&ended) == NpmSessionTableError::kNone && ended.empty());
    copy.Clear();
    assert(budget->Usage().input_batch_bytes == 0);
}

template <typename Key>
void TestLifecycle() {
    auto config = DefaultNpmAnalysisConfig(NpmRunMode::kOffline);
    config.tcp_idle_timeout_ns = 100;
    config.udp_idle_timeout_ns = 100;
    config.out_of_order_tolerance_ns = 0;
    auto budget = std::make_shared<NpmTaskBudget>(config);
    NpmSessionTable table(config, budget);
    NpmTestPacketBinding binding;
    binding.key = MakeKey<Key>(1000, 6);
    binding.transport.tcp = {true, true, false, false, false, 1, 0, 1};
    auto first = Observe(table, binding, 1);
    const auto id = first.active_session.session_id;
    assert(Observe(table, binding, 2).active_session.session_id == id);
    binding.transport.tcp.sequence = 2;
    auto reused = Observe(table, binding, 3);
    assert(reused.ended_sessions.size() == 1 && reused.ended_sessions.Row(0).facts.session_id == id &&
           reused.ended_sessions.Row(0).facts.end_reason == NpmSessionEndReason::kTupleReuse);
    assert(reused.active_session.session_id > id);
    Identifier identifier;
    NpmSessionView invalid;
    NpmPacketView packet;
    assert(table.SampleProtocol(NpmThroughputTestAccess::Token(table, binding.key.View(), id), packet, identifier,
                                &invalid) == NpmSessionTableError::kSessionInstanceMismatch);
    auto quiet = table.AdvanceCaptureProgress({102, true});
    assert(quiet.ended_sessions.empty() && table.size() == 1);  // retired deadline must not retire replacement
    auto timeout = table.AdvanceCaptureProgress({103, true});
    assert(timeout.ended_sessions.size() == 1 &&
           timeout.ended_sessions.Row(0).facts.session_id == reused.active_session.session_id);
    assert(table.tracked_bytes() == 0 && !table.NextEventDeadlineNs());
    NpmSessionObserveResult rejected;
    p::PacketMeta late;
    late.timestamp_ns = 102;
    assert(table.Observe(binding, late, &rejected) == NpmSessionTableError::kLatePacket && table.size() == 0);
    binding.transport.tcp.syn = false;
    binding.transport.tcp.fin = true;
    Observe(table, binding, 104);
    binding.direction = NpmPacketDirection::kBToA;
    auto fin = Observe(table, binding, 105);
    assert(!fin.has_active_session && fin.ended_sessions.size() == 1 &&
           fin.ended_sessions.Row(0).facts.packets_ab == 1 && fin.ended_sessions.Row(0).facts.packets_ba == 1);
    binding.transport.tcp.fin = false;
    binding.transport.tcp.rst = true;
    auto rst = Observe(table, binding, 106);
    assert(!rst.has_active_session && rst.ended_sessions.size() == 1 &&
           rst.ended_sessions.Row(0).facts.session_id > fin.ended_sessions.Row(0).facts.session_id);
    assert(table.size() == 0 && budget->Usage().session_state_bytes == 0);
}

void TestMixedTimeoutAndNodeStability() {
    auto config = DefaultNpmAnalysisConfig(NpmRunMode::kRealtime);
    config.udp_idle_timeout_ns = 100;
    config.out_of_order_tolerance_ns = 0;
    auto budget = std::make_shared<NpmTaskBudget>(config);
    {
        NpmSessionTable table(config, budget);
        NpmTestPacketBinding four, six;
        four.key = MakeKey<NpmSessionKey4>();
        six.key = MakeKey<NpmSessionKey6>();
        const auto v6 = Observe(table, six, 1).active_session;
        const auto* saved = v6.key->Get6();
        Observe(table, four, 2);
        for (uint16_t port = 1001; port < 3000; ++port) {
            four.key = MakeKey<NpmSessionKey4>(port);
            Observe(table, four, 50);
        }
        assert(saved->a_ip.bytes[0] == 1);  // family map rehash leaves node/key references stable
        auto unknown = table.AdvanceCaptureProgress({102});
        assert(unknown.disposition == NpmCaptureProgressDisposition::kDeferredBacklogUnknown &&
               unknown.ended_sessions.empty());
        auto progress = table.AdvanceCaptureProgress({102, false, true, true, false});
        assert(progress.ended_sessions.size() == 2 && progress.ended_sessions.Row(0).facts.session_id == 1 &&
               progress.ended_sessions.Row(1).facts.session_id == 2);
        assert(progress.ended_sessions.View(0).key->Get6() && progress.ended_sessions.View(1).key->Get4());
        assert(table.size() == 1999);
    }
    assert(budget->Usage().session_state_bytes == 0);
}
void TestAdmissionBudget() {
    auto config = DefaultNpmAnalysisConfig(NpmRunMode::kOffline);
    config.out_of_order_tolerance_ns = 0;
    auto budget = std::make_shared<NpmTaskBudget>(config);
    NpmSessionTable table(config, budget);
    NpmTestPacketBinding four, six;
    four.key = MakeKey<NpmSessionKey4>();
    six.key = MakeKey<NpmSessionKey6>();
    const auto used = budget->Usage().input_batch_bytes;
    {
        NpmSessionAdmissionPlanner planner(table);
        p::PacketMeta meta;
        meta.timestamp_ns = 1;
        bool admission = false;
        assert(planner.ObserveAndAdvance(four, meta, &admission) == NpmSessionTableError::kNone && admission);
        const auto first = budget->Usage().input_batch_bytes;
        assert(first > used);
        assert(planner.ObserveAndAdvance(four, meta, &admission) == NpmSessionTableError::kNone && !admission);
        assert(budget->Usage().input_batch_bytes == first);
        assert(planner.ObserveAndAdvance(six, meta, &admission) == NpmSessionTableError::kNone && admission);
        assert(budget->Usage().input_batch_bytes > first);
        assert(table.size() == 0);
    }
    assert(budget->Usage().input_batch_bytes == used);
}

void TestSnapshotBudgetFailureIsAtomic() {
    auto config = DefaultNpmAnalysisConfig(NpmRunMode::kOffline);
    config.udp_idle_timeout_ns = 100;
    config.out_of_order_tolerance_ns = 0;
    auto budget = std::make_shared<NpmTaskBudget>(config);
    NpmSessionTable table(config, budget);
    NpmTestPacketBinding binding;
    binding.key = MakeKey<NpmSessionKey6>();
    const auto id = Observe(table, binding, 1).active_session.session_id;
    const auto charged = table.tracked_bytes();
    const auto deadline = table.NextEventDeadlineNs();
    const auto retained = config.max_tracked_bytes - charged;
    assert(budget->Reserve(NpmBudgetCategory::kModuleState, retained) == NpmBudgetError::kNone);
    bool rejected = false;
    try {
        table.AdvanceCaptureProgress({101, true});
    } catch (const std::bad_alloc&) {
        rejected = true;
    }
    assert(rejected && table.size() == 1 && table.tracked_bytes() == charged && table.WatermarkNs() == 0 &&
           table.NextEventDeadlineNs() == deadline);
    NpmSessionSnapshotBatch unchanged;
    assert(table.FinishAllAtEof(&unchanged) == NpmSessionTableError::kAllocationFailed && unchanged.empty());
    assert(table.size() == 1 && budget->Usage().input_batch_bytes == 0);
    budget->Release(NpmBudgetCategory::kModuleState, retained);
    auto ended = table.AdvanceCaptureProgress({101, true});
    assert(ended.ended_sessions.size() == 1 && ended.ended_sessions.Row(0).facts.session_id == id && table.size() == 0);
    ended.ended_sessions.Clear();
    assert(NpmTrackedBudgetBytes(budget->Usage()) == 0);
}

int main() {
    static_assert(sizeof(NpmOwnedSessionKey) == 16);
    static_assert(!std::is_copy_constructible_v<NpmSessionTable>);
    TestAdmissionBudget();
    TestSnapshotBudgetFailureIsAtomic();
    TestSharedCapacityAndSnapshots();
    TestLifecycle<NpmSessionKey4>();
    TestLifecycle<NpmSessionKey6>();
    TestMixedTimeoutAndNodeStability();
    std::puts(
        "family session runtime: shared capacity/IDs, stable handles, both lifecycles and owning snapshots passed");
}
