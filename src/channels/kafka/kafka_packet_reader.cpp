// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include "kafka_packet_reader.h"
#include <arrow/api.h>
#include <framework/core/packet_family_codec.h>
#include <plugins/npi/packet_decoder.h>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <sstream>

namespace flowsql::channels::kafka {
namespace {
std::atomic<uint64_t> next_generation{0};
}
PacketReader::PacketReader(std::string name, PacketSourceConfig config, std::shared_ptr<IKafkaClientSessionV1> session,
                           IProtocol* protocol)
    : name_(std::move(name)), config_(std::move(config)), session_(std::move(session)), protocol_(protocol) {
    schema_text_ = ArrowSchema()->ToString(true);
    sources_.limits.max_packets_per_batch = config_.max_packets_per_batch;
    sources_.limits.max_bytes_per_batch = config_.max_bytes_per_batch;
    sources_.limits.max_wait_ms = config_.max_wait_ms;
    sources_.limits.max_outstanding_batches = 1;
    sources_.max_inspected_packets_per_poll = config_.max_packets_per_batch;
    sources_.max_inspected_bytes_per_poll = config_.max_message_bytes;
    sources_.max_poll_work_ms = config_.max_wait_ms;
    const auto generation = ++next_generation;
    for (const auto& input : config_.inputs) {
        CaptureQueueIdentityV1 identity;
        identity.observation_domain_id = config_.observation_domain_id;
        identity.source_name = input.source_name.c_str();
        identity.source_id = input.source_id;
        identity.queue_id = input.queue_id;
        identity.generation = generation;
        identity.link_type = 1;
        sources_.inputs.push_back(identity);
        auto& counters = counters_[input.source_id];
        counters.generation = generation;
        counters.available_mask =
            kCaptureReceivedPacketsAvailable | kCaptureDeliveredPacketsAvailable | kCaptureDeliveredBytesAvailable;
    }
}
PacketReader::~PacketReader() {
    Close();
    std::fprintf(stderr, "kafka_packet %s\n", Diagnostics().c_str());
}
std::shared_ptr<arrow::Schema> PacketReader::ArrowSchema() const { return packet::PacketFamilySchema(); }
int PacketReader::DescribeSources(CaptureSourceSetV2* output) const {
    if (!output || output->struct_size < sizeof(*output) || output->contract_version != 2) return EINVAL;
    *output = sources_;
    return 0;
}
int PacketReader::Open() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (opened_) return EALREADY;
    if (cancelled_ || !session_) return ECANCELED;
    void* capability = nullptr;
    const int query = session_->QueryInterface(IID_KAFKA_CONSUMER_SESSION_V1, &capability);
    if (query || !capability) return query ? query : ENOTSUP;
    consumer_ = static_cast<IKafkaConsumerSessionV1*>(capability);
    const int rc = consumer_->StartConsumer(config_.offset_reset.c_str(), config_.max_message_bytes);
    if (rc)
        Error(rc, "consumer_open");
    else {
        opened_ = true;
        stage_ = "poll";
    }
    return rc;
}
void PacketReader::Cancel() {
    cancelled_ = true;
    if (session_) session_->Cancel();
}
int PacketReader::Close() {
    Cancel();
    std::lock_guard<std::mutex> lock(mutex_);
    outstanding_.reset();
    opened_ = false;
    return 0;
}
BlockPollEvent PacketReader::Error(int error, const char* stage) {
    terminal_error_ = error;
    stage_ = stage;
    terminal_ = true;
    return {BlockPollEvent::kError, nullptr, error};
}
CapturePollEventV2 PacketReader::PollCapture(int timeout_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    CapturePollEventV2 event;
    if (cancelled_)
        event.block = {BlockPollEvent::kCancelled, nullptr, ECANCELED};
    else if (terminal_)
        event.block = {BlockPollEvent::kError, nullptr, terminal_error_};
    else if (!opened_ || timeout_ms < 0 || timeout_ms > 30000)
        event.block = Error(EINVAL, "poll");
    else if (outstanding_)
        event.block = {BlockPollEvent::kError, nullptr, EBUSY};
    else {
        KafkaMessageV1 message;
        const int rc = consumer_->PollMessage(std::min<uint32_t>(timeout_ms, config_.max_wait_ms), &message);
        if (rc == EAGAIN) return event;  // kTimeout, no invented progress.
        if (rc == ECANCELED) {
            event.block = {BlockPollEvent::kCancelled, nullptr, rc};
            return event;
        }
        if (rc) {
            event.block = Error(rc, "consume");
            return event;
        }
        if (message.topic != session_->Snapshot().topic || message.partition != 0 || message.offset < 0 ||
            message.offset == INT64_MAX || (bound_ && message.offset <= current_offset_)) {
            event.block = Error(EPROTO, "message_identity");
            return event;
        }
        auto decoded = DecodePacket(config_, message);
        if (!decoded.ok()) {
            event.block = Error(EILSEQ, "decode");
            return event;
        }
        auto envelope = std::make_shared<PacketEnvelope>(std::move(*decoded));
        if (bound_ && (envelope->stream_epoch != epoch_ || envelope->capture_generation != remote_generation_ ||
                       poll_sequence_ == UINT64_MAX || envelope->poll_sequence != poll_sequence_ + 1)) {
            event.block = Error(ESTALE, "stream_identity");
            return event;
        }
        auto sequence = packet_sequence_;
        for (const auto& row : envelope->packets) {
            if (row.meta.sequence <= sequence) {
                event.block = Error(EPROTO, "packet_order");
                return event;
            }
            sequence = row.meta.sequence;
        }
        for (auto& fact : envelope->progress) {
            if (fact.fact_sequence <= fact_sequences_[fact.source_id]) {
                event.block = Error(EPROTO, "fact_order");
                return event;
            }
            fact.generation = sources_.inputs.front().generation;
        }
        protocol::NpiPacketLayerDecoder decoder(protocol_);
        size_t index = 0;
        std::shared_ptr<arrow::RecordBatch> batch;
        const auto encoded = packet::EncodePacketFamilyStream(
            [&](packet::PacketFamilyBatch* scratch) {
                const auto& row = envelope->packets[index++];
                packet::PacketView view;
                view.meta = row.meta;
                view.bytes = {reinterpret_cast<const uint8_t*>(row.bytes.data()), row.bytes.size()};
                packet::PacketFamilyRecord record;
                record.meta = row.meta;
                record.raw_data = {envelope, view.bytes.data, row.meta.captured_len};
                const int result = decoder.Decode(view,
                                                  {packet::kExtractMac | packet::kExtractIp | packet::kExtractPort,
                                                   packet::EndpointScope::kInnermost},
                                                  *scratch, &record);
                if (!result) scratch->rows.push_back(std::move(record));
                return result;
            },
            envelope->packets.size(), arrow::default_memory_pool(), &batch);
        if (encoded != packet::PacketBatchError::kNone) {
            event.block = Error(EILSEQ, "layer_decode");
            return event;
        }
        epoch_ = envelope->stream_epoch;
        remote_generation_ = envelope->capture_generation;
        poll_sequence_ = envelope->poll_sequence;
        packet_sequence_ = sequence;
        if (!bound_) first_offset_ = message.offset;
        bound_ = true;
        current_offset_ = message.offset;
        for (const auto& row : envelope->packets) {
            auto& c = counters_[row.meta.source_id];
            ++c.received_packets;
            ++c.delivered_packets;
            c.delivered_bytes += row.meta.captured_len;
        }
        for (const auto& fact : envelope->progress) fact_sequences_[fact.source_id] = fact.fact_sequence;
        event.progress = std::move(envelope->progress);
        outstanding_ = std::move(batch);
        acknowledged_ = false;
        event.block = {BlockPollEvent::kData, outstanding_, 0};
    }
    return event;
}
int PacketReader::AcknowledgeBlock(const std::shared_ptr<arrow::RecordBatch>& batch) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (cancelled_) return ECANCELED;
    if (terminal_) return terminal_error_;
    if (!batch || batch != outstanding_ || acknowledged_) return EINVAL;
    const int rc = config_.commit_policy == "none" ? 0 : consumer_->CommitOffsets({{0, current_offset_ + 1}}, 3000);
    if (rc) {
        commit_uncertain_ = true;
        Error(rc, "commit_uncertain");
        return rc;
    }
    acknowledged_ = true;
    acknowledged_offset_ = current_offset_;
    return 0;
}
int PacketReader::ReleaseBlock(const std::shared_ptr<arrow::RecordBatch>& batch) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!batch || batch != outstanding_) return EINVAL;
    if (!acknowledged_ && !terminal_) Error(ECANCELED, "unacknowledged");
    outstanding_.reset();
    return 0;
}
int PacketReader::ReadInputCounters(uint32_t source, CaptureCountersV1* output) const {
    if (!output || output->struct_size < sizeof(*output) || output->contract_version != 1) return EINVAL;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = counters_.find(source);
    if (found == counters_.end()) return ENOENT;
    *output = found->second;
    return 0;
}
std::string PacketReader::Diagnostics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream out;
    out << "stage=" << stage_ << " partition=0 first_offset=" << first_offset_ << " offset=" << current_offset_
        << " acknowledged_offset=" << acknowledged_offset_ << " poll_sequence=" << poll_sequence_
        << " commit_uncertain=" << commit_uncertain_ << " remote_generation=" << remote_generation_ << " epoch=";
    const char* digits = "0123456789abcdef";
    for (auto byte : epoch_) out << digits[byte >> 4] << digits[byte & 15];
    return out.str();
}
}  // namespace flowsql::channels::kafka
