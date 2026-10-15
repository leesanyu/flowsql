// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <arrow/api.h>
#include <channels/kafka/kafka_packet_writer.h>
#include <channels/kafka/kafka_store.h>
#include <framework/core/packet_family_codec.h>
#include <cassert>
#include <thread>
namespace packet_sink_test {
using namespace flowsql;
using namespace flowsql::channels::kafka;
inline PacketSinkConfig Config() {
    return ParsePacketSinkConfig(
        Parse(R"({"encoding":"flowsql_packet_v1","stream_id":"probe-a","delivery_timeout_ms":2000})"));
}
inline CaptureSourceSetV2 Sources() {
    CaptureSourceSetV2 s;
    CaptureQueueIdentityV1 i;
    i.observation_domain_id = 7;
    i.source_id = 0;
    i.queue_id = 2;
    i.source_name = "eth0";
    i.generation = 9;
    i.link_type = 1;
    s.inputs.push_back(i);
    i.source_id = 1;
    i.queue_id = 3;
    i.source_name = "eth1";
    s.inputs.push_back(i);
    s.limits = {sizeof(CaptureReaderLimitsV1), 1, 256, 1048576, 100, 1};
    return s;
}
inline CapturePollEventV2 Event(uint64_t sequence = 1, int rows = 3) {
    packet::PacketFamilyBatch records;
    for (int n = 0; n < rows; ++n) {
        auto owner = std::make_shared<std::string>(std::string("\x00\xff\x00", 3));
        packet::PacketFamilyRecord row;
        row.meta = {100, 3, 64, 1, 0, sequence * 100 + uint64_t(n)};
        row.raw_data = {owner, reinterpret_cast<const uint8_t*>(owner->data()), 3};
        records.rows.push_back(row);
    }
    CapturePollEventV2 e;
    if (rows) {
        e.block.kind = BlockPollEvent::kData;
        assert(packet::EncodePacketFamilyBatch(records, &e.block.batch) == packet::PacketBatchError::kNone);
    }
    CaptureProgressV1 fact;
    fact.source_id = 0;
    fact.queue_id = 2;
    fact.generation = 9;
    fact.fact_sequence = sequence;
    fact.capture_time_ns = 100;
    fact.packet_observed = rows > 0;
    fact.source_idle_confirmed = !rows;
    fact.backlog = CaptureBacklogV1::kEmpty;
    e.progress.push_back(fact);
    return e;
}
struct FakeSession final : IKafkaClientSessionV1, IKafkaProducerSessionV1 {
    KafkaBindingSnapshotV1 snapshot;
    std::atomic<bool> cancelled{false}, entered{false}, wait{false};
    int calls = 0, drains = 0, fail_call = -1, drain_rc = 0, probe_rc = 0;
    std::vector<KafkaProduceMessageV1> messages;
    const KafkaBindingSnapshotV1& Snapshot() const override { return snapshot; }
    int Probe(uint32_t, std::string*) override { return cancelled ? ECANCELED : probe_rc; }
    void Cancel() override { cancelled = true; }
    int QueryInterface(const Guid&, void** out) override {
        *out = static_cast<IKafkaProducerSessionV1*>(this);
        return 0;
    }
    int Produce(const std::vector<KafkaProduceMessageV1>& m, uint32_t timeout, KafkaDeliveryV1* out) override {
        assert(m.size() == 1 && timeout > 0 && timeout <= 2000);
        entered = true;
        while (wait && !cancelled) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (cancelled || calls++ == fail_call) {
            *out = {0, 0, 1};
            return cancelled ? ECANCELED : EIO;
        }
        messages.insert(messages.end(), m.begin(), m.end());
        *out = {1, 0, 0};
        return 0;
    }
    int Drain(uint32_t) override {
        ++drains;
        return drain_rc;
    }
};
inline void Balanced(const PacketSinkReportV1& r) {
    for (const auto* c : {&r.messages, &r.packets}) assert(c->input == c->confirmed + c->not_sent + c->unknown);
}
}  // namespace packet_sink_test
