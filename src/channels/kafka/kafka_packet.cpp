// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include "kafka_packet.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <map>
#include <regex>
#include <set>
#include <stdexcept>

namespace flowsql::channels::kafka {
namespace {
void Require(bool ok) {
    if (!ok) throw std::invalid_argument("INVALID_PACKET_SOURCE");
}
void Keys(const rapidjson::Value& value, const std::set<std::string>& allowed) {
    Require(value.IsObject());
    std::set<std::string> seen;
    for (const auto& member : value.GetObject()) {
        std::string key(member.name.GetString(), member.name.GetStringLength());
        Require(allowed.count(key) && seen.insert(key).second);
    }
}
std::string String(const rapidjson::Value& value, const char* name) {
    Require(value.HasMember(name) && value[name].IsString());
    const auto& field = value[name];
    std::string result(field.GetString(), field.GetStringLength());
    Require(!result.empty() && result.find('\0') == std::string::npos);
    return result;
}
uint64_t Get(const std::string& bytes, size_t at, size_t size) {
    uint64_t value = 0;
    for (size_t i = 0; i < size; ++i) value = (value << 8) | static_cast<uint8_t>(bytes[at + i]);
    return value;
}
void Put(std::string& bytes, uint64_t value, size_t size) {
    for (size_t i = size; i > 0; --i) bytes.push_back(static_cast<char>(value >> ((i - 1) * 8)));
}
const PacketInput* Input(const PacketSourceConfig& config, uint32_t id) {
    for (const auto& input : config.inputs)
        if (input.source_id == id) return &input;
    return nullptr;
}
arrow::Status Invalid() { return arrow::Status::Invalid("INVALID_PACKET_ENVELOPE"); }
// No allocation proportional to wire counts or raw bytes until this scan succeeds.
arrow::Status Preflight(const PacketSourceConfig& c, const KafkaMessageV1& m) {
    const auto& b = m.value;
    if (m.value_is_null || m.key_is_null || m.key != c.stream_id || m.key.size() > c.max_message_bytes ||
        b.size() > c.max_message_bytes - m.key.size() || b.size() < 56 || b.compare(0, 4, "FSPK") ||
        Get(b, 4, 2) != 1 || Get(b, 6, 2) != 0 ||
        std::all_of(b.begin() + 8, b.begin() + 24, [](char v) { return v == 0; }) ||
        Get(b, 24, 8) != c.observation_domain_id || !Get(b, 32, 8) || !Get(b, 40, 8))
        return Invalid();
    const auto packets = Get(b, 48, 4), facts = Get(b, 52, 4);
    if (packets > 4096 || packets > c.max_packets_per_batch || facts > 32 || (!packets && !facts)) return Invalid();
    size_t at = 56;
    uint64_t raw_bytes = 0, sequence = 0;
    std::map<uint32_t, int64_t> observed;
    for (uint64_t i = 0; i < packets; ++i) {
        if (b.size() - at < 32) return Invalid();
        const auto source = static_cast<uint32_t>(Get(b, at, 4));
        const auto time = Get(b, at + 4, 8), captured = Get(b, at + 12, 4), wire = Get(b, at + 16, 4);
        const auto next = Get(b, at + 24, 8);
        if (!Input(c, source) || time > INT64_MAX || wire < captured || Get(b, at + 20, 4) != 1 || !next ||
            next <= sequence || captured > b.size() - at - 32)
            return Invalid();
        raw_bytes += captured;
        if (raw_bytes > c.max_bytes_per_batch) return Invalid();
        observed[source] = std::max(observed[source], static_cast<int64_t>(time));
        sequence = next;
        at += 32 + captured;
    }
    if (b.size() - at != facts * 28) return Invalid();
    std::set<uint32_t> seen;
    for (uint64_t i = 0; i < facts; ++i, at += 28) {
        const auto source = static_cast<uint32_t>(Get(b, at, 4));
        const auto* input = Input(c, source);
        const auto time = Get(b, at + 16, 8), flags = Get(b, at + 24, 1);
        if (!input || input->queue_id != Get(b, at + 4, 4) || !seen.insert(source).second || !Get(b, at + 8, 8) ||
            time > INT64_MAX || flags > 3 || flags == 3 || Get(b, at + 25, 1) > 2 || Get(b, at + 26, 2) ||
            bool(flags & 1) != bool(observed.count(source)) ||
            ((flags & 1) && time > static_cast<uint64_t>(observed[source])))
            return Invalid();
    }
    return arrow::Status::OK();
}
}  // namespace

PacketSourceConfig ParsePacketSourceConfig(const rapidjson::Value& v) {
    Keys(v, {"encoding", "stream_id", "observation_domain_id", "inputs", "offset_reset", "commit_policy",
             "max_packets_per_batch", "max_bytes_per_batch", "max_message_bytes", "max_wait_ms"});
    Require(String(v, "encoding") == "flowsql_packet_v1");
    PacketSourceConfig c;
    c.stream_id = String(v, "stream_id");
    Require(std::regex_match(c.stream_id, std::regex("[A-Za-z0-9_.-]{1,64}")));
    const auto domain = String(v, "observation_domain_id");
    const auto parsed = std::from_chars(domain.data(), domain.data() + domain.size(), c.observation_domain_id);
    Require(parsed.ec == std::errc{} && parsed.ptr == domain.data() + domain.size() && c.observation_domain_id);
    if (v.HasMember("offset_reset")) c.offset_reset = String(v, "offset_reset");
    if (v.HasMember("commit_policy")) c.commit_policy = String(v, "commit_policy");
    Require(c.offset_reset == "earliest" || c.offset_reset == "latest" || c.offset_reset == "error");
    Require(c.commit_policy == "none" || c.commit_policy == "after_success");
    auto bound = [&](const char* name, uint32_t low, uint32_t high, uint32_t& target) {
        if (!v.HasMember(name)) return;
        Require(v[name].IsUint() && v[name].GetUint() >= low && v[name].GetUint() <= high);
        target = v[name].GetUint();
    };
    bound("max_packets_per_batch", 1, 4096, c.max_packets_per_batch);
    bound("max_bytes_per_batch", 1024, 16777216, c.max_bytes_per_batch);
    bound("max_message_bytes", 1024, 16777216, c.max_message_bytes);
    bound("max_wait_ms", 1, 1000, c.max_wait_ms);
    Require(v.HasMember("inputs") && v["inputs"].IsArray() && v["inputs"].Size() && v["inputs"].Size() <= 32);
    std::set<uint32_t> ids;
    for (const auto& input : v["inputs"].GetArray()) {
        Keys(input, {"source_id", "queue_id", "source_name"});
        Require(input.HasMember("source_id") && input["source_id"].IsUint() && input.HasMember("queue_id") &&
                input["queue_id"].IsUint());
        PacketInput item{input["source_id"].GetUint(), input["queue_id"].GetUint(), String(input, "source_name")};
        // Validate UTF-8 even for callers passing a DOM assembled without a validating parser.
        rapidjson::Document check;
        std::string quoted = "\"";
        for (unsigned char ch : item.source_name) {
            if (ch < 32 || ch == '"' || ch == '\\')
                quoted += " ";
            else
                quoted += static_cast<char>(ch);
        }
        quoted += '"';
        check.Parse<rapidjson::kParseValidateEncodingFlag>(quoted.data(), quoted.size());
        Require(item.source_name.size() <= 128 && !check.HasParseError() && ids.insert(item.source_id).second);
        c.inputs.push_back(std::move(item));
    }
    return c;
}

arrow::Result<PacketEnvelope> DecodePacket(const PacketSourceConfig& c, const KafkaMessageV1& m) {
    ARROW_RETURN_NOT_OK(Preflight(c, m));
    const auto& b = m.value;
    PacketEnvelope e;
    std::memcpy(e.stream_epoch.data(), b.data() + 8, 16);
    e.observation_domain_id = Get(b, 24, 8);
    e.capture_generation = Get(b, 32, 8);
    e.poll_sequence = Get(b, 40, 8);
    e.packets.reserve(Get(b, 48, 4));
    e.progress.reserve(Get(b, 52, 4));
    size_t at = 56;
    for (uint64_t i = 0; i < Get(b, 48, 4); ++i) {
        PacketWireRecord row;
        row.meta.source_id = Get(b, at, 4);
        row.meta.timestamp_ns = Get(b, at + 4, 8);
        row.meta.captured_len = Get(b, at + 12, 4);
        row.meta.wire_len = Get(b, at + 16, 4);
        row.meta.link_type = Get(b, at + 20, 4);
        row.meta.sequence = Get(b, at + 24, 8);
        row.bytes.assign(b.data() + at + 32, row.meta.captured_len);
        at += 32 + row.meta.captured_len;
        e.packets.push_back(std::move(row));
    }
    for (uint64_t i = 0; i < Get(b, 52, 4); ++i, at += 28) {
        CaptureProgressV1 fact;
        fact.source_id = Get(b, at, 4);
        fact.queue_id = Get(b, at + 4, 4);
        fact.fact_sequence = Get(b, at + 8, 8);
        fact.capture_time_ns = Get(b, at + 16, 8);
        fact.packet_observed = Get(b, at + 24, 1) & 1;
        fact.source_idle_confirmed = Get(b, at + 24, 1) & 2;
        fact.backlog = static_cast<CaptureBacklogV1>(Get(b, at + 25, 1));
        e.progress.push_back(fact);
    }
    return e;
}

arrow::Result<KafkaMessageV1> EncodePacket(const PacketSourceConfig& c, const PacketEnvelope& e) {
    if (e.packets.size() > 4096 || e.packets.size() > c.max_packets_per_batch || e.progress.size() > 32)
        return Invalid();
    uint64_t size = 56 + e.progress.size() * 28;
    uint64_t raw = 0;
    for (const auto& row : e.packets) {
        if (row.bytes.size() != row.meta.captured_len) return Invalid();
        raw += row.bytes.size();
        size += 32 + row.bytes.size();
        if (raw > c.max_bytes_per_batch || size + c.stream_id.size() > c.max_message_bytes) return Invalid();
    }
    if (size + c.stream_id.size() > c.max_message_bytes) return Invalid();
    KafkaMessageV1 m;
    m.key = c.stream_id;
    m.key_is_null = false;
    auto& b = m.value;
    b.reserve(size);
    b = "FSPK";
    Put(b, 1, 2);
    Put(b, 0, 2);
    b.append(reinterpret_cast<const char*>(e.stream_epoch.data()), 16);
    Put(b, e.observation_domain_id, 8);
    Put(b, e.capture_generation, 8);
    Put(b, e.poll_sequence, 8);
    Put(b, e.packets.size(), 4);
    Put(b, e.progress.size(), 4);
    for (const auto& row : e.packets) {
        Put(b, row.meta.source_id, 4);
        Put(b, static_cast<uint64_t>(row.meta.timestamp_ns), 8);
        Put(b, row.meta.captured_len, 4);
        Put(b, row.meta.wire_len, 4);
        Put(b, row.meta.link_type, 4);
        Put(b, row.meta.sequence, 8);
        b += row.bytes;
    }
    for (const auto& f : e.progress) {
        Put(b, f.source_id, 4);
        Put(b, f.queue_id, 4);
        Put(b, f.fact_sequence, 8);
        Put(b, static_cast<uint64_t>(f.capture_time_ns), 8);
        Put(b, (f.packet_observed ? 1 : 0) | (f.source_idle_confirmed ? 2 : 0), 1);
        Put(b, static_cast<uint8_t>(f.backlog), 1);
        Put(b, 0, 2);
    }
    ARROW_RETURN_NOT_OK(Preflight(c, m));
    return m;
}
}  // namespace flowsql::channels::kafka
