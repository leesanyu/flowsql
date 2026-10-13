// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <operators/npm_basic/core/npm_throughput_contract.h>
#include "npm_packet_test_support.h"

#include <common/network/netbase.h>
#include <operators/npm_basic/core/npm_task_budget.h>

#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <type_traits>
#include <unordered_map>

using namespace flowsql::npm;

static_assert(!std::is_copy_constructible_v<NpmSessionSnapshotBatch>);
static_assert(!std::is_copy_assignable_v<NpmSessionSnapshotBatch>);
static_assert(std::is_nothrow_move_constructible_v<NpmSessionSnapshotBatch>);
static_assert(std::is_nothrow_move_assignable_v<NpmSessionSnapshotBatch>);
static_assert(!std::is_copy_constructible_v<NpmSnapshotInputLease>);
static_assert(!std::is_copy_constructible_v<NpmSessionAccess>);
static_assert(std::is_same_v<decltype(NpmPacketBindingView::key), NpmSessionKeyView>);
static_assert(std::is_same_v<decltype(NpmSnapshotRow::key_index), uint32_t>);
static_assert(std::is_same_v<decltype(&NpmSessionSnapshotBatch::Clone),
                             NpmSessionTableError (NpmSessionSnapshotBatch::*)(NpmSessionSnapshotBatch*) const>);
static_assert(std::is_abstract_v<INpmAcceptedResultDelivery>);
static_assert(kNpmTypedEntityRowLimit == 512 && kNpmTypedStorageSoftLimit == 1024 * 1024);

namespace {
bool count_allocations = false;
size_t allocations = 0;
size_t key_lookups = 0;
int allocation_failure_after = -1;

template <typename Key>
void TestStackBinding(size_t namespace_bytes, size_t expected_allocations) {
    allocations = 0;
    count_allocations = true;
    Key key;
    key.input_namespace = std::string(namespace_bytes, 'n');
    NpmPacketBindingView binding{NpmSessionKeyView(key)};
    count_allocations = false;
    assert(allocations == expected_allocations);
    assert(binding.key.Namespace().data() == key.input_namespace.data());
}

namespace p = flowsql::packet;
struct Identifier : p::IPacketFamilyProtocolIdentifier {
    size_t calls = 0;
    p::PacketProtocolInfo Identify(const p::PacketView&, const p::PacketLayerFacts&) override {
        ++calls;
        return {};
    }
};

template <typename Key>
struct Fixture {
    std::array<uint8_t, 64> bytes{};
    std::conditional_t<std::is_same_v<Key, NpmSessionKey6>, p::PacketIpEndpoints6, p::PacketIpEndpoints4> endpoints;
    p::PacketFamilyLayerView layer;
    p::PacketView packet;
    Fixture(bool reverse = false) {
        constexpr bool six = std::is_same_v<Key, NpmSessionKey6>;
        const size_t network = six ? 40 : 20;
        const size_t length = network + 21;
        bytes[0] = six ? 0x60 : 0x45;
        if (six) {
            bytes[5] = 21;
            bytes[6] = 6;
        } else {
            bytes[3] = static_cast<uint8_t>(length);
            bytes[9] = 6;
        }
        bytes[network + 12] = 0x50;
        bytes[network + 13] = 0x10;  // ACK
        bytes[network + 20] = 1;
        decltype(Key{}.a_ip) a{}, b{};
        a.bytes[0] = 1;
        b.bytes[0] = 2;
        endpoints = {reverse ? b : a, reverse ? a : b};
        layer.endpoints = &endpoints;
        layer.address_state = p::PacketAddressState::kAvailable;
        layer.status = p::LayerStatus::kDecoded;
        layer.layer_count = 2;
        layer.layers[0] = {static_cast<uint16_t>(six ? flowsql::eLayer::IPv6 : flowsql::eLayer::IPv4), 0};
        layer.layers[1] = {static_cast<uint16_t>(flowsql::eLayer::TCP), static_cast<uint16_t>(network)};
        layer.network_layer_index = 0;
        layer.transport_layer_index = 1;
        layer.payload_offset = network + 20;
        layer.transport_protocol = 6;
        layer.ports_valid = 1;
        layer.src_port = reverse ? 443 : 50000;
        layer.dst_port = reverse ? 50000 : 443;
        packet.bytes = {bytes.data(), length};
        packet.meta.captured_len = length;
        packet.meta.wire_len = length;
        packet.meta.source_id = 7;
        packet.meta.timestamp_ns = 1;
    }
};

std::unique_ptr<NpmObservationDomainLookup> Lookup(size_t namespace_size) {
    NpmObservationDomainMap map;
    map.input_namespace = std::string(namespace_size, 'n');
    map.bindings = {{7, 19}};
    std::unique_ptr<NpmObservationDomainLookup> result;
    assert(NpmObservationDomainLookup::Create(
               map, std::make_shared<NpmTaskBudget>(DefaultNpmAnalysisConfig(NpmRunMode::kOffline)), &result) ==
           NpmObservationDomainLookupError::kNone);
    return result;
}

template <typename Key>
void TestProductionBuilder(size_t namespace_size) {
    auto domains = Lookup(namespace_size);
    Fixture<Key> forward, reverse(true);
    Key key, reverse_key;
    NpmPacketBindingView binding, reverse_binding;
    allocations = 0;
    count_allocations = true;
    const auto error = BuildNpmSessionPacketBinding(*domains, forward.packet, forward.layer, &key, &binding);
    count_allocations = false;
    assert(error == NpmSessionPacketError::kNone);
    assert(allocations == (namespace_size <= 15 ? 0 : 1));
    assert(binding.key.Namespace().data() == key.input_namespace.data());
    assert(binding.key.Namespace().size() == namespace_size && key.observation_domain_id == 19);
    assert(BuildNpmSessionPacketBinding(*domains, reverse.packet, reverse.layer, &reverse_key, &reverse_binding) ==
           NpmSessionPacketError::kNone);
    assert(NpmSessionKeyEqual{}(key, reverse_key));
    assert(binding.direction == NpmPacketDirection::kAToB && reverse_binding.direction == NpmPacketDirection::kBToA);
    const auto saved_port = key.a_port;
    forward.packet.meta.source_id = 99;
    assert(BuildNpmSessionPacketBinding(*domains, forward.packet, forward.layer, &key, &binding) ==
           NpmSessionPacketError::kUnknownSourceId);
    assert(key.a_port == saved_port && binding.key.Namespace().data() == key.input_namespace.data());
    // The owning conversion allocates just the object; it moves the already allocated namespace.
    const auto* namespace_data = key.input_namespace.data();
    allocations = 0;
    count_allocations = true;
    NpmOwnedSessionKey owned(std::move(key));
    count_allocations = false;
    assert(allocations == 1);
    if (namespace_size > 15) assert(owned.View().Namespace().data() == namespace_data);
}

template <typename Key>
void TestAccess() {
    auto config = DefaultNpmAnalysisConfig(NpmRunMode::kOffline);
    config.payload_sample_packets = 2;
    config.out_of_order_tolerance_ns = 0;
    config.tcp_idle_timeout_ns = 100;
    NpmSessionTable table(config), other(config);
    auto domains = Lookup(12);
    Fixture<Key> fixture;
    Key key;
    NpmPacketBindingView binding;
    assert(BuildNpmSessionPacketBinding(*domains, fixture.packet, fixture.layer, &key, &binding) ==
           NpmSessionPacketError::kNone);
    NpmPacketView packet;
    packet.packet = fixture.packet;
    packet.layer = &fixture.layer;
    packet.payload = binding.payload;
    packet.direction = binding.direction;
    packet.transport = binding.transport;
    Identifier identifier;
    NpmSessionView output;
    const auto observe = [&]() {
        NpmSessionObserveResult result;
        assert(table.Observe(binding, fixture.packet.meta, &result) == NpmSessionTableError::kNone);
        return result;
    };
    observe();  // warm admission
    key_lookups = 0;
    auto observed = observe();
    assert(table.SampleProtocol(std::move(observed.access), packet, identifier, &output) ==
           NpmSessionTableError::kNone);
    assert(key_lookups == 1 && identifier.calls == 1 && output.protocol_status == NpmProtocolStatus::kPending);
    assert(table.SampleProtocol(std::move(observed.access), packet, identifier, &output) ==
           NpmSessionTableError::kSessionInstanceMismatch);
    auto second = observe();
    NpmSessionAccess moved(std::move(second.access));
    assert(table.SampleProtocol(std::move(second.access), packet, identifier, &output) ==
           NpmSessionTableError::kSessionInstanceMismatch);
    NpmSessionAccess assigned;
    assigned = std::move(moved);
    assert(table.SampleProtocol(std::move(assigned), packet, identifier, &output) == NpmSessionTableError::kNone);
    assert(identifier.calls == 2 && output.protocol_status == NpmProtocolStatus::kUnknown);
    assert(table.SampleProtocol({}, packet, identifier, &output) == NpmSessionTableError::kSessionInstanceMismatch);
    auto cross = observe();
    assert(other.SampleProtocol(std::move(cross.access), packet, identifier, &output) ==
           NpmSessionTableError::kSessionInstanceMismatch);
    auto stale = observe();
    observe();
    assert(table.SampleProtocol(std::move(stale.access), packet, identifier, &output) ==
           NpmSessionTableError::kSessionInstanceMismatch);
    auto before_reuse = observe();
    const auto old_id = before_reuse.active_session.session_id;
    binding.transport.tcp.syn = true;
    binding.transport.tcp.ack = false;
    binding.transport.tcp.sequence = 5;
    auto reused = observe();
    assert(reused.ended_sessions.size() == 1 && reused.ended_sessions.Row(0).facts.session_id == old_id);
    assert(reused.active_session.session_id > old_id);
    assert(table.SampleProtocol(std::move(before_reuse.access), packet, identifier, &output) ==
           NpmSessionTableError::kSessionInstanceMismatch);
    assert(table.SampleProtocol(std::move(reused.access), packet, identifier, &output) == NpmSessionTableError::kNone);
    auto before_close = observe();
    binding.transport.tcp.syn = false;
    binding.transport.tcp.rst = true;
    auto closed = observe();
    assert(!closed.has_active_session && table.size() == 0);
    assert(table.SampleProtocol(std::move(before_close.access), packet, identifier, &output) ==
           NpmSessionTableError::kSessionInstanceMismatch);
    assert(table.SampleProtocol(std::move(closed.access), packet, identifier, &output) ==
           NpmSessionTableError::kSessionInstanceMismatch);
    binding.transport.tcp.rst = false;
    auto before_progress = observe();
    auto progress = table.AdvanceCaptureProgress({101, true});
    assert(progress.ended_sessions.size() == 1 && table.size() == 0);
    assert(table.SampleProtocol(std::move(before_progress.access), packet, identifier, &output) ==
           NpmSessionTableError::kSessionInstanceMismatch);
    fixture.packet.meta.timestamp_ns = 102;
    auto before_eof = observe();
    NpmSessionSnapshotBatch ended;
    assert(table.FinishAllAtEof(&ended) == NpmSessionTableError::kNone && ended.size() == 1);
    assert(table.SampleProtocol(std::move(before_eof.access), packet, identifier, &output) ==
           NpmSessionTableError::kSessionInstanceMismatch);
}

void TestDomainLookupNoAllocation() {
    NpmObservationDomainMap map;
    map.input_namespace = std::string(256, 'n');
    for (uint32_t source = 256; source != 0; --source) map.bindings.push_back({source, source + 1000});
    auto budget = std::make_shared<NpmTaskBudget>(DefaultNpmAnalysisConfig(NpmRunMode::kOffline));
    std::unique_ptr<NpmObservationDomainLookup> lookup;
    assert(NpmObservationDomainLookup::Create(map, budget, &lookup) == NpmObservationDomainLookupError::kNone);
    map.bindings.clear();
    allocations = 0;
    count_allocations = true;
    for (uint32_t repeat = 0; repeat < 100; ++repeat) {
        for (uint32_t source = 1; source <= 256; ++source) {
            uint64_t domain = 0;
            assert(lookup->Resolve(source, &domain) == NpmObservationDomainError::kNone && domain == source + 1000);
        }
    }
    count_allocations = false;
    assert(allocations == 0);
    lookup.reset();
    assert(NpmTrackedBudgetBytes(budget->Usage()) == 0);
}

template <typename Key>
Key SessionKey(size_t namespace_size = 12) {
    Key key;
    key.input_namespace = std::string(namespace_size, 'n');
    key.observation_domain_id = 19;
    key.a_ip.bytes[0] = 1;
    key.b_ip.bytes[0] = 2;
    key.a_port = 50000;
    key.b_port = 443;
    key.transport_protocol = 6;
    return key;
}

NpmPacketBindingView SessionBinding(NpmSessionKeyView key) {
    NpmPacketBindingView binding;
    binding.key = key;
    binding.transport.tcp.valid = true;
    binding.transport.tcp.ack = true;
    return binding;
}

template <typename Key>
void TestDeadlineAllocation(size_t namespace_size) {
    auto config = DefaultNpmAnalysisConfig(NpmRunMode::kOffline);
    config.tcp_idle_timeout_ns = 100;
    auto budget = std::make_shared<NpmTaskBudget>(config);
    NpmSessionTable table(config, budget);
    auto key = SessionKey<Key>(namespace_size);
    auto binding = SessionBinding(NpmSessionKeyView(key));
    p::PacketMeta meta;
    meta.timestamp_ns = 1;
    meta.wire_len = 96;
    NpmSessionObserveResult observed;
    assert(table.Observe(binding, meta, &observed) == NpmSessionTableError::kNone);
    const auto tracked = table.tracked_bytes();
    Identifier identifier;
    NpmPacketView packet;  // ACK without captured payload: sampling must also stay allocation-free.
    NpmSessionView sampled;
    allocations = 0;
    key_lookups = 0;
    count_allocations = true;
    allocation_failure_after = 0;
    constexpr int count = 10000;
    for (int phase = 0; phase < 2; ++phase) {
        for (int row = 0; row < count; ++row) {
            meta.timestamp_ns = phase == 0 ? 1 : row + 2;
            assert(table.Observe(binding, meta, &observed) == NpmSessionTableError::kNone);
            assert(table.SampleProtocol(std::move(observed.access), packet, identifier, &sampled) ==
                   NpmSessionTableError::kNone);
        }
    }
    allocation_failure_after = -1;
    count_allocations = false;
    assert(allocations == 0 && key_lookups == 2 * count);
    assert(table.NextEventDeadlineNs() == count + 101);
    assert(sampled.session_id == 1 && sampled.packets_ab == 2 * count + 1 && sampled.last_ns == count + 1);
    assert(table.tracked_bytes() == tracked && budget->Usage().session_state_bytes == tracked);
    assert(budget->Usage().input_batch_bytes == 0 && identifier.calls == 0);
}

void TestMixedDeadlineOrderAndLateRejection() {
    auto config = DefaultNpmAnalysisConfig(NpmRunMode::kOffline);
    config.tcp_idle_timeout_ns = 100;
    config.out_of_order_tolerance_ns = 0;
    NpmSessionTable table(config);
    auto key4 = SessionKey<NpmSessionKey4>();
    auto key6 = SessionKey<NpmSessionKey6>();
    auto binding4 = SessionBinding(NpmSessionKeyView(key4));
    auto binding6 = SessionBinding(NpmSessionKeyView(key6));
    p::PacketMeta meta;
    NpmSessionObserveResult result;
    meta.timestamp_ns = 1;
    assert(table.Observe(binding4, meta, &result) == NpmSessionTableError::kNone);
    meta.timestamp_ns = 5;
    assert(table.Observe(binding6, meta, &result) == NpmSessionTableError::kNone);
    assert(table.NextEventDeadlineNs() == 101);
    meta.timestamp_ns = 10;
    assert(table.Observe(binding4, meta, &result) == NpmSessionTableError::kNone);
    assert(table.NextEventDeadlineNs() == 105);
    auto progress = table.AdvanceCaptureProgress({105, true});
    assert(progress.ended_sessions.size() == 1 && progress.ended_sessions.View(0).key->Get6());
    assert(progress.ended_sessions.Row(0).facts.session_id == 2 && table.NextEventDeadlineNs() == 110);
    NpmSessionView before;
    assert(table.Find(NpmSessionKeyView(key4), &before) == NpmSessionTableError::kNone);
    const auto tracked = table.tracked_bytes();
    meta.timestamp_ns = 104;
    assert(table.Observe(binding4, meta, &result) == NpmSessionTableError::kLatePacket);
    NpmSessionView after;
    assert(table.Find(NpmSessionKeyView(key4), &after) == NpmSessionTableError::kNone);
    assert(after.session_id == before.session_id && after.packets_ab == before.packets_ab &&
           after.last_ns == before.last_ns);
    assert(table.NextEventDeadlineNs() == 110 && table.tracked_bytes() == tracked);
    progress = table.AdvanceCaptureProgress({110, true});
    assert(progress.ended_sessions.size() == 1 && progress.ended_sessions.View(0).key->Get4());
    assert(progress.ended_sessions.Row(0).facts.session_id == 1 && table.size() == 0 && !table.NextEventDeadlineNs());
    // Equal deadlines keep global session ID order across both families.
    meta.timestamp_ns = 111;
    assert(table.Observe(binding6, meta, &result) == NpmSessionTableError::kNone);
    assert(table.Observe(binding4, meta, &result) == NpmSessionTableError::kNone);
    progress = table.AdvanceCaptureProgress({211, true});
    assert(progress.ended_sessions.size() == 2);
    assert(progress.ended_sessions.Row(0).facts.session_id == 3 &&
           progress.ended_sessions.Row(1).facts.session_id == 4);
}

enum class FailureCase { kAdmission, kClose, kTupleReuse };
template <typename Key>
void TestAllocationFailureAtomicity(FailureCase operation) {
    size_t rejected_points = 0;
    bool reached_success = false;
    for (int fail_after = 0; fail_after < 16; ++fail_after) {
        auto config = DefaultNpmAnalysisConfig(NpmRunMode::kOffline);
        config.tcp_idle_timeout_ns = 100;
        auto budget = std::make_shared<NpmTaskBudget>(config);
        NpmSessionTable table(config, budget);
        auto key = SessionKey<Key>(256);
        auto binding = SessionBinding(NpmSessionKeyView(key));
        p::PacketMeta meta;
        meta.timestamp_ns = 1;
        meta.wire_len = 96;
        NpmSessionObserveResult initial;
        if (operation != FailureCase::kAdmission) {
            binding.transport.tcp.syn = true;
            binding.transport.tcp.ack = false;
            binding.transport.tcp.sequence = 1;
            assert(table.Observe(binding, meta, &initial) == NpmSessionTableError::kNone);
            meta.timestamp_ns = 2;
            if (operation == FailureCase::kClose) {
                binding.transport.tcp.syn = false;
                binding.transport.tcp.rst = true;
            } else
                binding.transport.tcp.sequence = 2;
        }
        const auto tracked = table.tracked_bytes();
        const auto deadline = table.NextEventDeadlineNs();
        NpmSessionObserveResult output;
        output.active_session.session_id = 999;
        allocation_failure_after = fail_after;
        const auto error = table.Observe(binding, meta, &output);
        allocation_failure_after = -1;
        if (error == NpmSessionTableError::kNone) {
            reached_success = true;
            break;
        }
        ++rejected_points;
        assert(error == NpmSessionTableError::kAllocationFailed);
        assert(output.active_session.session_id == 999 && output.ended_sessions.empty());
        assert(table.tracked_bytes() == tracked && table.NextEventDeadlineNs() == deadline);
        assert(budget->Usage().session_state_bytes == tracked && budget->Usage().input_batch_bytes == 0);
        NpmSessionView active;
        if (operation == FailureCase::kAdmission) {
            assert(table.size() == 0 && table.Find(NpmSessionKeyView(key), &active) == NpmSessionTableError::kNotFound);
        } else {
            assert(table.size() == 1 && table.Find(NpmSessionKeyView(key), &active) == NpmSessionTableError::kNone);
            assert(active.session_id == 1 && active.first_ns == 1 && active.last_ns == 1 && active.packets_ab == 1);
        }
        assert(table.Observe(binding, meta, &output) == NpmSessionTableError::kNone);
        if (operation == FailureCase::kAdmission)
            assert(output.active_session.session_id == 1);
        else if (operation == FailureCase::kClose) {
            assert(table.size() == 0 && output.ended_sessions.size() == 1 &&
                   output.ended_sessions.Row(0).facts.session_id == 1);
        } else {
            assert(output.active_session.session_id == 2 && output.ended_sessions.size() == 1);
            assert(output.ended_sessions.Row(0).facts.session_id == 1);
        }
    }
    assert(reached_success && rejected_points >= 3);
}

void TestSnapshotBatchOwnership(size_t namespace_bytes, bool timeout) {
    auto config = DefaultNpmAnalysisConfig(NpmRunMode::kOffline);
    config.tcp_idle_timeout_ns = 100;
    config.out_of_order_tolerance_ns = 0;
    auto budget = std::make_shared<NpmTaskBudget>(config);
    NpmSessionSnapshotBatch source;
    constexpr size_t count = 64;
    {
        NpmSessionTable table(config, budget);
        auto key4 = SessionKey<NpmSessionKey4>(namespace_bytes);
        auto key6 = SessionKey<NpmSessionKey6>(namespace_bytes);
        p::PacketMeta meta;
        meta.timestamp_ns = 1;
        meta.wire_len = 96;
        for (size_t i = 0; i < count; ++i) {
            key4.a_port = key6.a_port = static_cast<uint16_t>(1000 + i);
            NpmSessionObserveResult observed;
            auto binding = SessionBinding(i % 2 ? NpmSessionKeyView(key6) : NpmSessionKeyView(key4));
            assert(table.Observe(binding, meta, &observed) == NpmSessionTableError::kNone);
        }
        allocations = 0;
        count_allocations = true;
        if (timeout) {
            auto progress = table.AdvanceCaptureProgress({101, true});
            source = std::move(progress.ended_sessions);
        } else {
            assert(table.FinishAllAtEof(&source) == NpmSessionTableError::kNone);
            source.SetObservedAt(101);
        }
        count_allocations = false;
        // Three contiguous arrays; long namespaces require one string allocation per row.
        assert(allocations == 3 + (namespace_bytes > 15 ? count : 0));
        assert(table.size() == 0 && !table.NextEventDeadlineNs());
    }
    assert(source.size() == count && budget->Usage().session_state_bytes == 0);
    const auto capacities = NpmThroughputTestAccess::Capacities(source);
    assert(capacities[0] == count / 2 && capacities[1] == count / 2 && capacities[2] == count);
    uint64_t expected = sizeof(source) + capacities[0] * sizeof(NpmSessionKey4) +
                        capacities[1] * sizeof(NpmSessionKey6) + capacities[2] * sizeof(NpmSnapshotRow);
    for (size_t i = 0; i < count; ++i) {
        const auto view = source.View(i);
        const auto capacity =
            i % 2 ? view.key->Get6()->input_namespace.capacity() : view.key->Get4()->input_namespace.capacity();
        expected += capacity + 1;
        assert(view.session_id == i + 1 && view.packets_ab == 1 && view.wire_bytes_ab == 96);
        assert(view.key->Namespace() == std::string(namespace_bytes, 'n'));
        assert(source.Row(i).observed_at_ns == 101);
    }
    assert(budget->Usage().input_batch_bytes == expected);
    assert(NpmThroughputTestAccess::StorageBytes(source) == expected);
    NpmSessionSnapshotBatch moved(std::move(source));
    assert(source.empty() && NpmThroughputTestAccess::StorageBytes(source) == 0);
    assert(budget->Usage().input_batch_bytes == expected);
    NpmSessionSnapshotBatch clone, retained;
    assert(moved.Clone(&clone) == NpmSessionTableError::kNone);
    assert(moved.CloneRow(1, &retained) == NpmSessionTableError::kNone);
    const auto retained_bytes = NpmThroughputTestAccess::StorageBytes(retained);
    assert(budget->Usage().input_batch_bytes == 2 * expected + retained_bytes);
    assert(clone.View(1).key->Get6() != moved.View(1).key->Get6());
    assert(retained.View(0).key->Namespace().data() != moved.View(1).key->Namespace().data());
    moved.MutableFacts(1).session_id = 999;
    assert(clone.View(1).session_id == 2 && retained.View(0).session_id == 2);
    moved.Clear();
    assert(budget->Usage().input_batch_bytes == expected + retained_bytes);
    retained = std::move(clone);  // releases the replaced single-row owner exactly once
    assert(clone.empty() && retained.size() == count);
    assert(budget->Usage().input_batch_bytes == expected);
    assert(retained.View(1).key->Namespace() == std::string(namespace_bytes, 'n'));
    assert(retained.Row(1).observed_at_ns == 101);
    retained.Clear();
    assert(NpmTrackedBudgetBytes(budget->Usage()) == 0);
}

void TestSnapshotCloneFailureAndCapture() {
    auto config = DefaultNpmAnalysisConfig(NpmRunMode::kOffline);
    auto budget = std::make_shared<NpmTaskBudget>(config);
    auto key = SessionKey<NpmSessionKey6>(256);
    NpmSessionView view;
    view.key = NpmSessionKeyView(key);
    view.session_id = 7;
    view.protocol_status = NpmProtocolStatus::kPending;
    NpmSessionSnapshotBatch source;
    assert(NpmSessionSnapshotBatch::Capture(view, NpmSessionEndReason::kClosed, 42, budget, &source) ==
           NpmSessionTableError::kNone);
    assert(source.View(0).protocol_status == NpmProtocolStatus::kPending);
    key.input_namespace = "changed";
    assert(source.View(0).key->Namespace() == std::string(256, 'n'));
    NpmSessionSnapshotBatch output;
    assert(source.Clone(&output) == NpmSessionTableError::kNone);
    output.MutableFacts(0).session_id = 999;
    const auto charged = budget->Usage().input_batch_bytes;
    bool succeeded = false;
    for (int fail_after = 0; fail_after < 8; ++fail_after) {
        allocation_failure_after = fail_after;
        const auto error = source.Clone(&output);
        allocation_failure_after = -1;
        if (error == NpmSessionTableError::kNone) {
            succeeded = true;
            break;
        }
        assert(error == NpmSessionTableError::kAllocationFailed);
        assert(output.View(0).session_id == 999 && budget->Usage().input_batch_bytes == charged);
    }
    assert(succeeded && output.View(0).session_id == 7);
    const auto blocked = config.max_tracked_bytes - NpmTrackedBudgetBytes(budget->Usage());
    assert(budget->Reserve(NpmBudgetCategory::kModuleState, blocked) == NpmBudgetError::kNone);
    output.MutableFacts(0).session_id = 999;
    assert(source.Clone(&output) == NpmSessionTableError::kAllocationFailed);
    assert(source.CloneRow(0, &output) == NpmSessionTableError::kAllocationFailed);
    assert(output.View(0).session_id == 999 && budget->Usage().input_batch_bytes == charged);
    assert(source.CloneRow(1, &output) == NpmSessionTableError::kNotFound);
    assert(source.Clone(nullptr) == NpmSessionTableError::kNullOutput);
    assert(budget->Release(NpmBudgetCategory::kModuleState, blocked) == NpmBudgetError::kNone);
    // Self-cloning prepares all copies before replacing its own storage.
    assert(source.Clone(&source) == NpmSessionTableError::kNone);
    output.Clear();
    source.Clear();
    assert(NpmTrackedBudgetBytes(budget->Usage()) == 0);
}

void TestPacketEndTimestampAndAllocation() {
    auto config = DefaultNpmAnalysisConfig(NpmRunMode::kOffline);
    auto budget = std::make_shared<NpmTaskBudget>(config);
    NpmSessionTable table(config, budget);
    auto key = SessionKey<NpmSessionKey6>(256);
    auto binding = SessionBinding(NpmSessionKeyView(key));
    p::PacketMeta meta;
    meta.timestamp_ns = 20;
    NpmSessionObserveResult observed;
    assert(table.Observe(binding, meta, &observed) == NpmSessionTableError::kNone);
    binding.transport.tcp.rst = true;
    meta.timestamp_ns = 10;
    allocations = 0;
    count_allocations = true;
    assert(table.Observe(binding, meta, &observed) == NpmSessionTableError::kNone);
    count_allocations = false;
    assert(allocations == 3);  // one family array, one row array, one string; no temporary key
    assert(observed.ended_sessions.Row(0).observed_at_ns == 10);
    assert(observed.ended_sessions.View(0).first_ns == 10 && observed.ended_sessions.View(0).last_ns == 20);
}

void TestBulkEndFailureAtomicity(size_t namespace_bytes, bool timeout, bool budget_failure) {
    constexpr size_t count = 6;
    size_t rejected_points = 0;
    bool reached_success = false;
    const int attempts = budget_failure ? 2 : 16;
    for (int point = 0; point < attempts; ++point) {
        auto config = DefaultNpmAnalysisConfig(NpmRunMode::kOffline);
        config.tcp_idle_timeout_ns = 100;
        config.out_of_order_tolerance_ns = 0;
        auto budget = std::make_shared<NpmTaskBudget>(config);
        NpmSessionTable table(config, budget);
        std::array<NpmSessionKey4, count / 2> keys4;
        std::array<NpmSessionKey6, count / 2> keys6;
        p::PacketMeta meta;
        meta.timestamp_ns = 1;
        meta.wire_len = 96;
        for (size_t i = 0; i < count / 2; ++i) {
            keys4[i] = SessionKey<NpmSessionKey4>(namespace_bytes);
            keys6[i] = SessionKey<NpmSessionKey6>(namespace_bytes);
            keys4[i].a_port = keys6[i].a_port = static_cast<uint16_t>(1000 + i);
            for (auto key : {NpmSessionKeyView(keys4[i]), NpmSessionKeyView(keys6[i])}) {
                NpmSessionObserveResult observed;
                assert(table.Observe(SessionBinding(key), meta, &observed) == NpmSessionTableError::kNone);
            }
        }
        const auto tracked = table.tracked_bytes();
        const auto deadline = table.NextEventDeadlineNs();
        const auto watermark = table.WatermarkNs();
        auto ended = MakeNpmTestSnapshotBatch(1);  // independent ledger keeps the output sentinel alive
        ended.MutableFacts(0).session_id = 999;
        uint64_t blocked = 0;
        if (budget_failure) {
            const uint64_t arrays = sizeof(NpmSessionSnapshotBatch) + (count / 2) * sizeof(NpmSessionKey4) +
                                    (count / 2) * sizeof(NpmSessionKey6) + count * sizeof(NpmSnapshotRow);
            const uint64_t string_bytes = std::max(namespace_bytes, std::string{}.capacity()) + 1;
            // First reject before storage, then after five strings have already been staged.
            const auto available = point == 0 ? 0 : arrays + (count - 1) * string_bytes;
            blocked = config.max_tracked_bytes - tracked - available;
            assert(budget->Reserve(NpmBudgetCategory::kModuleState, blocked) == NpmBudgetError::kNone);
        } else
            allocation_failure_after = point;
        const auto finish = [&]() {
            if (!timeout) return table.FinishAllAtEof(&ended);
            try {
                auto progress = table.AdvanceCaptureProgress({101, true});
                ended = std::move(progress.ended_sessions);
                return NpmSessionTableError::kNone;
            } catch (const std::bad_alloc&) {
                return NpmSessionTableError::kAllocationFailed;
            }
        };
        const auto error = finish();
        allocation_failure_after = -1;
        if (error == NpmSessionTableError::kNone) {
            assert(!budget_failure);
            reached_success = true;
            break;
        }
        ++rejected_points;
        assert(error == NpmSessionTableError::kAllocationFailed);
        assert(ended.size() == 1 && ended.View(0).session_id == 999);
        assert(table.size() == count && table.tracked_bytes() == tracked && table.NextEventDeadlineNs() == deadline);
        assert(table.WatermarkNs() == watermark && budget->Usage().input_batch_bytes == 0);
        assert(budget->Usage().session_state_bytes == tracked && budget->Usage().module_state_bytes == blocked);
        for (size_t i = 0; i < count / 2; ++i) {
            NpmSessionView four, six;
            assert(table.Find(NpmSessionKeyView(keys4[i]), &four) == NpmSessionTableError::kNone);
            assert(table.Find(NpmSessionKeyView(keys6[i]), &six) == NpmSessionTableError::kNone);
            assert(four.session_id == 2 * i + 1 && six.session_id == 2 * i + 2);
            assert(four.packets_ab == 1 && six.packets_ab == 1 && four.last_ns == 1 && six.last_ns == 1);
        }
        if (blocked) assert(budget->Release(NpmBudgetCategory::kModuleState, blocked) == NpmBudgetError::kNone);
        assert(finish() == NpmSessionTableError::kNone);
        assert(ended.size() == count && table.size() == 0 && !table.NextEventDeadlineNs());
        for (size_t i = 0; i < count; ++i) {
            assert(ended.View(i).session_id == i + 1);
            assert(ended.Row(i).facts.end_reason ==
                   (timeout ? NpmSessionEndReason::kIdleTimeout : NpmSessionEndReason::kEof));
        }
        assert(budget->Usage().input_batch_bytes == NpmThroughputTestAccess::StorageBytes(ended));
        ended.Clear();
        assert(NpmTrackedBudgetBytes(budget->Usage()) == 0);
    }
    if (budget_failure)
        assert(rejected_points == 2);
    else
        assert(reached_success && rejected_points == 3 + (namespace_bytes > 15 ? count : 0));
}

void TestSharedBatchEventRangesAndRetention() {
    auto config = DefaultNpmAnalysisConfig(NpmRunMode::kOffline);
    config.tcp_idle_timeout_ns = 100;
    config.out_of_order_tolerance_ns = 0;
    auto budget = std::make_shared<NpmTaskBudget>(config);
    NpmSessionSnapshotBatch events, retained;
    {
        NpmSessionTable table(config, budget);
        auto key4 = SessionKey<NpmSessionKey4>(256);
        auto key6 = SessionKey<NpmSessionKey6>(256);
        auto four = SessionBinding(NpmSessionKeyView(key4));
        auto six = SessionBinding(NpmSessionKeyView(key6));
        six.transport.tcp.syn = true;
        six.transport.tcp.ack = false;
        six.transport.tcp.sequence = 1;
        p::PacketMeta meta;
        meta.timestamp_ns = 1;
        NpmSessionObserveResult observed;
        assert(table.Observe(four, meta, &observed, 0, &events) == NpmSessionTableError::kNone);
        assert(table.Observe(six, meta, &observed, 0, &events) == NpmSessionTableError::kNone);
        assert(events.empty() && budget->Usage().input_batch_bytes == 0);
        four.transport.tcp.rst = true;
        meta.timestamp_ns = 2;
        assert(table.Observe(four, meta, &observed, 0, &events) == NpmSessionTableError::kNone);
        assert(observed.ended_sessions.empty() && observed.ended_begin == 0 && observed.ended_end == 1);
        assert(events.CloneRow(0, &retained) == NpmSessionTableError::kNone);
        six.transport.tcp.sequence = 2;
        meta.timestamp_ns = 3;
        assert(table.Observe(six, meta, &observed, 0, &events) == NpmSessionTableError::kNone);
        assert(observed.ended_begin == 1 && observed.ended_end == 2 && observed.active_session.session_id == 3);
        six.transport.tcp.syn = false;
        six.transport.tcp.rst = true;
        meta.timestamp_ns = 4;
        assert(table.Observe(six, meta, &observed, 0, &events) == NpmSessionTableError::kNone);
        assert(observed.ended_begin == 2 && observed.ended_end == 3 && !observed.has_active_session);
        four.transport.tcp.rst = six.transport.tcp.rst = false;
        key4.a_port = key6.a_port = 2000;
        assert(table.Observe(four, meta, &observed, 0, &events) == NpmSessionTableError::kNone);
        assert(table.Observe(six, meta, &observed, 0, &events) == NpmSessionTableError::kNone);
        auto progress = table.AdvanceCaptureProgress({104, true}, &events);
        assert(progress.ended_sessions.empty() && progress.ended_begin == 3 && progress.ended_end == 5);
        assert(table.size() == 0 && events.size() == 5);
        const std::array<NpmSessionEndReason, 5> reasons{
            NpmSessionEndReason::kClosed, NpmSessionEndReason::kTupleReuse, NpmSessionEndReason::kClosed,
            NpmSessionEndReason::kIdleTimeout, NpmSessionEndReason::kIdleTimeout};
        const std::array<int64_t, 5> times{2, 3, 4, 104, 104};
        for (size_t i = 0; i < events.size(); ++i) {
            assert(events.View(i).session_id == i + 1 && events.Row(i).facts.end_reason == reasons[i]);
            assert(events.Row(i).observed_at_ns == times[i]);
        }
        assert(budget->Usage().input_batch_bytes ==
               NpmThroughputTestAccess::StorageBytes(events) + NpmThroughputTestAccess::StorageBytes(retained));
    }
    events.Clear();
    assert(retained.View(0).session_id == 1 && retained.View(0).key->Get4());
    assert(retained.View(0).key->Namespace() == std::string(256, 'n') && retained.Row(0).observed_at_ns == 2);
    retained.Clear();
    assert(NpmTrackedBudgetBytes(budget->Usage()) == 0);
}

struct CollidingHash {
    template <typename Key>
    size_t operator()(const Key&) const {
        return 0;
    }
};
template <typename Key>
void TestCollisionEquality() {
    std::unordered_map<Key, size_t, CollidingHash, NpmSessionKeyEqual> values;
    Key key;
    key.input_namespace = "short";
    key.observation_domain_id = 1;
    key.a_ip.bytes[0] = 1;
    key.b_ip.bytes[0] = 2;
    key.a_port = 1000;
    key.b_port = 443;
    key.transport_protocol = 6;
    std::vector<Key> keys(7, key);
    keys[1].input_namespace = "other";
    keys[2].observation_domain_id = 2;
    keys[3].a_ip.bytes[sizeof(key.a_ip.bytes) - 1] = 3;
    keys[4].b_ip.bytes[sizeof(key.b_ip.bytes) - 1] = 4;
    keys[5].a_port = 1001;
    keys[6].transport_protocol = 17;
    for (size_t i = 0; i < keys.size(); ++i) assert(values.emplace(keys[i], i).second);
    for (size_t i = 0; i < keys.size(); ++i) assert(values.find(keys[i])->second == i);
    key.b_port = 444;
    assert(values.find(key) == values.end());
}
}  // namespace

namespace flowsql::npm {
void NpmRecordCoreKeyLookupForTest() { ++key_lookups; }
}  // namespace flowsql::npm

// Isolated executable: ordinary-new counts are separate from logical lookups and total heap.
void* operator new(size_t size) {
    if (allocation_failure_after == 0) throw std::bad_alloc();
    if (allocation_failure_after > 0) --allocation_failure_after;
    void* value = std::malloc(size ? size : 1);
    if (!value) throw std::bad_alloc();
    if (count_allocations) ++allocations;
    return value;
}
void* operator new[](size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, size_t) noexcept { std::free(value); }
void operator delete[](void* value, size_t) noexcept { std::free(value); }

int main() {
    TestStackBinding<NpmSessionKey4>(12, 0);
    TestStackBinding<NpmSessionKey6>(12, 0);
    TestStackBinding<NpmSessionKey4>(16, 1);
    TestStackBinding<NpmSessionKey6>(256, 1);
    NpmSessionKey4 key4;
    NpmSessionKey6 key6;
    key4.input_namespace = "short.packet";
    key6.input_namespace = std::string(256, 'n');
    NpmPacketBindingView four{NpmSessionKeyView(key4)};
    NpmPacketBindingView six{NpmSessionKeyView(key6)};
    assert(four.key.Get4() == &key4 && !four.key.Get6());
    assert(six.key.Get6() == &key6 && !six.key.Get4());
    assert(six.key.Namespace().data() == key6.input_namespace.data());
    for (bool observing : {false, true}) {
        for (bool consuming : {false, true}) {
            NpmResultDeliveryEntity entity;
            entity.observing = observing;
            entity.consuming = consuming;
            assert(entity.RequiresEncoding() == (observing || consuming));
        }
    }
    for (size_t bytes : {12u, 16u, 256u}) {
        TestProductionBuilder<NpmSessionKey4>(bytes);
        TestProductionBuilder<NpmSessionKey6>(bytes);
    }
    TestDomainLookupNoAllocation();
    for (size_t bytes : {12u, 16u, 256u}) {
        TestDeadlineAllocation<NpmSessionKey4>(bytes);
        TestDeadlineAllocation<NpmSessionKey6>(bytes);
    }
    for (size_t bytes : {12u, 16u, 256u}) {
        TestSnapshotBatchOwnership(bytes, false);
        TestSnapshotBatchOwnership(bytes, true);
    }
    for (size_t bytes : {12u, 16u, 256u}) {
        for (bool timeout : {false, true}) {
            TestBulkEndFailureAtomicity(bytes, timeout, false);
            TestBulkEndFailureAtomicity(bytes, timeout, true);
        }
    }
    TestSharedBatchEventRangesAndRetention();
    std::puts("T3.2: all bulk-end allocation points, mid-copy budget rejection and shared event ranges passed");
    TestSnapshotCloneFailureAndCapture();
    TestPacketEndTimestampAndAllocation();
    std::puts("T3.1: contiguous family batches, moves, explicit deep copies and exact budget release passed");
    TestMixedDeadlineOrderAndLateRejection();
    for (auto operation : {FailureCase::kAdmission, FailureCase::kClose, FailureCase::kTupleReuse}) {
        TestAllocationFailureAtomicity<NpmSessionKey4>(operation);
        TestAllocationFailureAtomicity<NpmSessionKey6>(operation);
    }
    std::puts(
        "T2.3: 120000 warmed Observe/Sample calls have zero allocations; mixed deadlines and allocation failures "
        "passed");
    TestAccess<NpmSessionKey4>();
    TestAccess<NpmSessionKey6>();
    TestCollisionEquality<NpmSessionKey4>();
    TestCollisionEquality<NpmSessionKey6>();
    std::puts("T2.2: typed builder allocations, collision equality, one logical lookup and access lifetime passed");
    return 0;
}
