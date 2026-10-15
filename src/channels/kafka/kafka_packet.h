// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <arrow/result.h>
#include <arrow/type_fwd.h>
#include <framework/core/capture_progress_tracker.h>
#include <framework/interfaces/icapture_block_stream_reader.h>
#include <framework/interfaces/ikafka_consumer_session.h>
#include <framework/interfaces/ipacket.h>
#include <rapidjson/document.h>
#include <array>
#include <functional>
#include <string>

namespace flowsql::channels::kafka {
struct PacketInput {
    uint32_t source_id = 0, queue_id = 0;
    std::string source_name;
};
struct PacketSourceConfig {
    std::string stream_id, offset_reset = "earliest", commit_policy = "after_success";
    uint64_t observation_domain_id = 0;
    std::vector<PacketInput> inputs;
    uint32_t max_packets_per_batch = 256, max_bytes_per_batch = 1048576;
    uint32_t max_message_bytes = 2097152, max_wait_ms = 100;
};
struct PacketSinkConfig {
    std::string stream_id;
    uint32_t max_message_bytes = 2097152, queue_max_messages = 64, queue_max_bytes = 8388608,
             delivery_timeout_ms = 10000;
};
PacketSinkConfig ParsePacketSinkConfig(const rapidjson::Value& value);
struct PacketWireRecord {
    packet::PacketMeta meta;
    std::string bytes;
};
struct PacketEnvelope {
    std::array<uint8_t, 16> stream_epoch{};
    uint64_t observation_domain_id = 0, capture_generation = 0, poll_sequence = 0;
    std::vector<PacketWireRecord> packets;
    // generation is local-only and is never serialized; all other fact fields are preserved.
    std::vector<CaptureProgressV1> progress;
};
// Throws invalid_argument("INVALID_PACKET_SOURCE"), without input values.
PacketSourceConfig ParsePacketSourceConfig(const rapidjson::Value& value);
// Strict preflight precedes raw-byte allocation. Both functions enforce the same wire contract.
arrow::Result<PacketEnvelope> DecodePacket(const PacketSourceConfig& config, const KafkaMessageV1& message);
arrow::Result<KafkaMessageV1> EncodePacket(const PacketSourceConfig& config, const PacketEnvelope& envelope);
// Task-owned encoder. Source strings are copied; no layer/NPI state crosses the wire.
class PacketSinkCodec {
 public:
    explicit PacketSinkCodec(PacketSinkConfig config) : config_(std::move(config)) {}
    void Open(const CaptureSourceSetV2&, const std::shared_ptr<arrow::Schema>&);
    arrow::Result<KafkaMessageV1> Encode(const CapturePollEventV2&, const std::array<uint8_t, 16>& epoch,
                                         uint64_t poll_sequence, const std::function<bool()>& interrupted = {});
    const PacketSourceConfig& WireConfig() const { return wire_; }
    uint64_t Generation() const { return generation_; }
    const CaptureSourceSetV2& Sources() const { return sources_; }

 private:
    PacketSinkConfig config_;
    PacketSourceConfig wire_;
    CaptureSourceSetV2 sources_;
    uint64_t generation_ = 0, last_packet_sequence_ = 0;
    std::map<uint32_t, CaptureProgressTrackerV1> trackers_;
};
}  // namespace flowsql::channels::kafka
