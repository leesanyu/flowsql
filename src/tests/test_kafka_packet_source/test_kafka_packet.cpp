// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <channels/kafka/kafka_packet.h>
#include <channels/kafka/kafka_store.h>
#include <cassert>
#include <fstream>
#include <iostream>
#include <sstream>

using namespace flowsql;
using namespace flowsql::channels::kafka;
namespace {
const char* kConfig = R"({"encoding":"flowsql_packet_v1","stream_id":"probe-a",
  "observation_domain_id":"18446744073709551615",
  "inputs":[{"source_id":4294967295,"queue_id":4294967294,"source_name":"探针/eth0"}]})";
std::string Golden() {
    std::ifstream file(PACKET_GOLDEN_PATH);
    std::string hex, bytes;
    assert(file >> hex);
    for (size_t i = 0; i < hex.size(); i += 2) bytes += static_cast<char>(std::stoul(hex.substr(i, 2), nullptr, 16));
    return bytes;
}
void Config() {
    auto value = Parse(kConfig);
    auto c = ParsePacketSourceConfig(value);
    assert(c.observation_domain_id == UINT64_MAX && c.inputs[0].source_id == UINT32_MAX);
    assert(c.max_packets_per_batch == 256 && c.max_wait_ms == 100 && c.commit_policy == "after_success");
    auto reject = [](const rapidjson::Value& v) {
        bool rejected = false;
        try {
            ParsePacketSourceConfig(v);
        } catch (const std::invalid_argument& e) {
            rejected = std::string(e.what()) == "INVALID_PACKET_SOURCE";
        }
        assert(rejected);
    };
    for (const char* name : {"encoding", "stream_id", "observation_domain_id", "inputs"}) {
        auto copy = Parse(kConfig);
        copy.RemoveMember(name);
        reject(copy);
    }
    for (const char* field : {"max_packets_per_batch", "max_bytes_per_batch", "max_message_bytes", "max_wait_ms"}) {
        for (int64_t n : {-1LL, 0LL, 16777217LL}) {
            auto copy = Parse(kConfig);
            copy.AddMember(rapidjson::Value(field, copy.GetAllocator()), rapidjson::Value(n), copy.GetAllocator());
            reject(copy);
        }
    }
    for (const char* domain : {"0", "-1", "+1", "1.0", "18446744073709551616", "1x"}) {
        auto copy = Parse(kConfig);
        copy["observation_domain_id"].SetString(domain, copy.GetAllocator());
        reject(copy);
    }
    auto duplicate = Parse(kConfig);
    duplicate.AddMember("encoding", "flowsql_packet_v1", duplicate.GetAllocator());
    reject(duplicate);
    auto unknown = Parse(kConfig);
    unknown.AddMember("surprise", 1, unknown.GetAllocator());
    reject(unknown);
    auto repeated = Parse(kConfig);
    repeated["inputs"].PushBack(rapidjson::Value(repeated["inputs"][0], repeated.GetAllocator()),
                                repeated.GetAllocator());
    reject(repeated);
    auto invalid_utf = Parse(kConfig);
    invalid_utf["inputs"][0]["source_name"].SetString("\xff", invalid_utf.GetAllocator());
    reject(invalid_utf);
    Store store;
    std::array<unsigned char, 32> key{};
    store.Open(":memory:", key);
    store.Mutate("connections", "add",
                 Parse(R"({"name":"local","bootstrap_servers":["localhost:1"],"security_protocol":"PLAINTEXT"})"));
    auto channel = Parse(
        R"({"name":"packets","connection_name":"local","role":"both","topic":"packets","consumer_group":"test","payload_kind":"packet","payload_options":{"source":{},"sink":{}}})");
    channel["payload_options"]["source"].CopyFrom(value, channel.GetAllocator());
    store.Mutate("channels", "add", std::move(channel));
    auto rows = store.Query("channels", "packets");
    const auto& options = rows["items"][0]["payload_options"];
    assert(options["source"]["inputs"].Size() == 1 && options["sink"].ObjectEmpty());
}
void Codec() {
    auto c = ParsePacketSourceConfig(Parse(kConfig));
    KafkaMessageV1 m;
    m.key = c.stream_id;
    m.key_is_null = false;
    m.value = Golden();
    auto e = DecodePacket(c, m).ValueOrDie();
    assert(e.capture_generation == UINT64_MAX - 1 && e.poll_sequence == UINT64_MAX - 2);
    assert(e.packets[0].meta.timestamp_ns == INT64_MAX && e.packets[0].meta.sequence == UINT64_MAX - 3);
    assert(e.progress[0].fact_sequence == UINT64_MAX - 4 && e.progress[0].queue_id == UINT32_MAX - 1);
    assert(EncodePacket(c, e).ValueOrDie().value == m.value);
    for (size_t n = 0; n < m.value.size(); ++n) {
        auto short_message = m;
        short_message.value.resize(n);
        assert(!DecodePacket(c, short_message).ok());
    }
    for (size_t at : {0u, 5u, 7u, 56u, 60u, 76u, 119u, 127u, 128u, 129u}) {
        auto corrupt = m;
        assert(at < corrupt.value.size());
        corrupt.value[at] ^= char(0xff);
        assert(!DecodePacket(c, corrupt).ok());
    }
    auto extra = m;
    extra.value += 'x';
    assert(!DecodePacket(c, extra).ok());
    extra = m;
    extra.value_is_null = true;
    assert(!DecodePacket(c, extra).ok());
    extra = m;
    extra.key_is_null = true;
    assert(!DecodePacket(c, extra).ok());
    extra.key_is_null = false;
    extra.key += 'x';
    assert(!DecodePacket(c, extra).ok());
    auto budget = c;
    budget.max_message_bytes = m.value.size() + m.key.size() - 1;
    assert(!DecodePacket(budget, m).ok());
    budget = c;
    budget.max_bytes_per_batch = 1;
    assert(!DecodePacket(budget, m).ok());
    // Full-range sequence and truncated/non-IP/IPv4/IPv6 bytes are preserved without protocol interpretation.
    for (const std::string& raw : {std::string(), std::string(7, '\xff'), std::string("\0\0\x08\0\x45", 5),
                                   std::string("\0\0\x86\xdd\x60", 5)}) {
        auto copy = e;
        copy.packets[0].bytes = raw;
        copy.packets[0].meta.captured_len = raw.size();
        copy.packets[0].meta.sequence = UINT64_MAX;
        auto encoded = EncodePacket(c, copy).ValueOrDie();
        auto decoded = DecodePacket(c, encoded).ValueOrDie();
        assert(decoded.packets[0].bytes == raw && decoded.packets[0].meta.sequence == UINT64_MAX);
    }
    e.packets.clear();
    e.progress[0].packet_observed = false;
    e.progress[0].source_idle_confirmed = true;
    assert(DecodePacket(c, EncodePacket(c, e).ValueOrDie()).ValueOrDie().packets.empty());
    e.progress.clear();
    assert(!EncodePacket(c, e).ok());
}
}  // namespace
int main() {
    Config();
    Codec();
    std::cout
        << "PASS packet config, independent golden, strict wire/budget/identity, full-width integers and raw bytes\n";
}
