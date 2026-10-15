// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <channels/kafka/kafka_packet.h>
#include <channels/kafka/kafka_store.h>
#include <framework/core/packet_family_codec.h>
#include <framework/interfaces/ipacket_sink.h>
#include <cassert>
#include <fstream>
#include <iostream>
using namespace flowsql;
using namespace flowsql::channels::kafka;
namespace {
const char* kSink = R"({"encoding":"flowsql_packet_v1","stream_id":"probe-a"})";
CaptureSourceSetV2 Sources() {
    CaptureSourceSetV2 s;
    CaptureQueueIdentityV1 i;
    i.observation_domain_id = UINT64_MAX;
    i.source_id = UINT32_MAX;
    i.queue_id = UINT32_MAX - 1;
    i.source_name = "probe/eth0";
    i.generation = UINT64_MAX - 1;
    i.link_type = 1;
    s.inputs.push_back(i);
    s.limits = {sizeof(CaptureReaderLimitsV1), 1, 256, 1048576, 100, 1};
    return s;
}
CapturePollEventV2 Event(const std::string& raw, uint64_t sequence = UINT64_MAX - 3) {
    auto owner = std::make_shared<std::string>(raw);
    packet::PacketFamilyRecord row;
    row.meta = {INT64_MAX, uint32_t(raw.size()), 65535, 1, UINT32_MAX, sequence};
    row.raw_data = {owner, reinterpret_cast<const uint8_t*>(owner->data()), uint32_t(raw.size())};
    packet::PacketFamilyBatch records;
    records.rows.push_back(row);
    CapturePollEventV2 event;
    event.block.kind = BlockPollEvent::kData;
    assert(packet::EncodePacketFamilyBatch(records, &event.block.batch) == packet::PacketBatchError::kNone);
    CaptureProgressV1 fact;
    fact.source_id = UINT32_MAX;
    fact.queue_id = UINT32_MAX - 1;
    fact.generation = UINT64_MAX - 1;
    fact.fact_sequence = UINT64_MAX - 4;
    fact.capture_time_ns = INT64_MAX;
    fact.packet_observed = true;
    fact.backlog = CaptureBacklogV1::kEmpty;
    event.progress.push_back(fact);
    return event;
}
std::array<uint8_t, 16> Epoch() {
    std::array<uint8_t, 16> a{};
    for (size_t i = 0; i < a.size(); ++i) a[i] = i + 1;
    return a;
}
void Config() {
    auto c = ParsePacketSinkConfig(Parse(kSink));
    assert(c.max_message_bytes == 2097152 && c.queue_max_messages == 64 && c.queue_max_bytes == 8388608 &&
           c.delivery_timeout_ms == 10000);
    auto reject = [](const rapidjson::Value& v) {
        bool bad = false;
        try {
            ParsePacketSinkConfig(v);
        } catch (const std::invalid_argument& e) {
            bad = std::string(e.what()) == "INVALID_PACKET_SINK";
        }
        assert(bad);
    };
    for (const char* field : {"encoding", "stream_id"}) {
        auto v = Parse(kSink);
        v.RemoveMember(field);
        reject(v);
    }
    for (const char* field : {"max_message_bytes", "queue_max_messages", "queue_max_bytes", "delivery_timeout_ms"}) {
        for (const char* value : {"0", "-1", "true", "\"1000\"", "268435457", "1.2"}) {
            auto v = Parse(kSink);
            rapidjson::Document n;
            n.Parse(value);
            v.AddMember(rapidjson::Value(field, v.GetAllocator()), rapidjson::Value(n, v.GetAllocator()),
                        v.GetAllocator());
            reject(v);
        }
    }
    for (const char* json :
         {R"({"encoding":"flowsql_packet_v1","stream_id":"x","surprise":1})",
          R"({"encoding":"flowsql_packet_v1","stream_id":"x","stream_id":"y"})",
          R"({"encoding":"flowsql_packet_v1","stream_id":"x","max_message_bytes":16777216,"queue_max_bytes":1048576})",
          R"({"encoding":"json","stream_id":"x"})", R"({"encoding":"flowsql_packet_v1","stream_id":"x y"})"}) {
        rapidjson::Document v;
        v.Parse(json);
        reject(v);
    }
    Store store;
    std::array<unsigned char, 32> key{};
    store.Open(":memory:", key);
    store.Mutate("connections", "add",
                 Parse(R"({"name":"local","bootstrap_servers":["localhost:1"],"security_protocol":"PLAINTEXT"})"));
    auto channel = Parse(
        R"({"name":"packet","connection_name":"local","role":"both","topic":"packet","consumer_group":"group","payload_kind":"packet","payload_options":{"source":{"encoding":"flowsql_packet_v1","stream_id":"probe-a","observation_domain_id":"1","inputs":[{"source_id":0,"queue_id":0,"source_name":"remote"}]},"sink":{}}})");
    channel["payload_options"]["sink"].CopyFrom(Parse(kSink), channel.GetAllocator());
    store.Mutate("channels", "add", std::move(channel));
    const auto saved = store.Query("channels", "packet");
    assert(saved["items"][0]["payload_options"]["sink"]["stream_id"] == "probe-a");
    assert(saved["items"][0]["payload_options"]["source"]["inputs"].Size() == 1);
}
void Codec() {
    std::ifstream f(PACKET_GOLDEN_PATH);
    std::string hex, golden;
    assert(f >> hex);
    for (size_t i = 0; i < hex.size(); i += 2) golden += char(std::stoul(hex.substr(i, 2), nullptr, 16));
    std::string raw = golden.substr(88, 15);
    auto sources = Sources();
    PacketSinkCodec codec(ParsePacketSinkConfig(Parse(kSink)));
    codec.Open(sources, packet::PacketFamilySchema());
    auto event = Event(raw);
    auto message = codec.Encode(event, Epoch(), UINT64_MAX - 2).ValueOrDie();
    assert(message.key == "probe-a" && message.value == golden);
    assert(!codec.Encode(event, Epoch(), UINT64_MAX - 1).ok());  // original sequence cannot repeat
    for (const std::string& bytes : {std::string(), std::string(7, '\xff'), std::string(65535, 'x'),
                                     std::string("\0\0\x08\0\x45", 5), std::string("\0\0\x86\xdd\x60", 5)}) {
        PacketSinkCodec c(ParsePacketSinkConfig(Parse(kSink)));
        c.Open(sources, packet::PacketFamilySchema());
        auto m = c.Encode(Event(bytes), Epoch(), 1).ValueOrDie();
        // Independent wire field reads; no DecodePacket round-trip.
        assert(m.value.substr(88, bytes.size()) == bytes && m.value.size() == 56 + 32 + bytes.size() + 28);
        auto u32 = [&](size_t at) {
            uint32_t n = 0;
            for (size_t j = 0; j < 4; ++j) n = (n << 8) | uint8_t(m.value[at + j]);
            return n;
        };
        assert(u32(48) == 1 && u32(52) == 1 && u32(56) == UINT32_MAX && u32(68) == bytes.size());
    }
    CapturePollEventV2 timeout;
    timeout.progress = event.progress;
    timeout.progress[0].packet_observed = false;
    timeout.progress[0].source_idle_confirmed = true;
    timeout.progress[0].fact_sequence++;
    assert(codec.Encode(timeout, Epoch(), UINT64_MAX - 1).ValueOrDie().value.size() == 84);
    assert(!codec.Encode(CapturePollEventV2{}, Epoch(), 1).ok());
    for (int mode = 0; mode < 4; ++mode) {
        bool bad = false;
        try {
            PacketSinkCodec c(ParsePacketSinkConfig(Parse(kSink)));
            auto s = sources;
            if (mode == 0) s.limits.max_bytes_per_batch = 2097152;
            if (mode == 1) s.inputs[0].link_type = 113;
            if (mode == 2) s.inputs[0].observation_domain_id = 0;
            c.Open(s, mode == 3 ? arrow::schema({}) : packet::PacketFamilySchema());
        } catch (const std::invalid_argument&) {
            bad = true;
        }
        assert(bad);
    }
    for (int mode = 0; mode < 4; ++mode) {
        PacketSinkCodec c(ParsePacketSinkConfig(Parse(kSink)));
        c.Open(sources, packet::PacketFamilySchema());
        auto e = event;
        if (mode == 0) e.progress[0].generation--;
        if (mode == 1) e.progress[0].queue_id--;
        if (mode == 2) e.block.kind = BlockPollEvent::kEof;
        if (mode == 3) e.progress.push_back(e.progress[0]);
        assert(!c.Encode(e, Epoch(), 1).ok());
    }
}
}  // namespace
int main() {
    Config();
    Codec();
    std::cout << "PASS packet sink configuration and independent golden/raw/fact/budget contracts\n";
}
