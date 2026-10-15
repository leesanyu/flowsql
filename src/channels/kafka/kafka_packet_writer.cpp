// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include "kafka_packet_writer.h"
#include <arrow/api.h>
#include <openssl/rand.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include "kafka_plugin.h"
namespace flowsql::channels::kafka {
namespace {
using Clock = std::chrono::steady_clock;
}
PacketWriter::PacketWriter(PacketSinkConfig c, PacketSinkContextV1 context,
                           std::shared_ptr<IKafkaClientSessionV1> session, std::shared_ptr<void> lease)
    : config_(std::move(c)),
      context_(std::move(context)),
      lease_(std::move(lease)),
      session_(std::move(session)),
      codec_(config_) {
    if (RAND_bytes(epoch_.data(), epoch_.size()) != 1) throw Failure(500, "CRYPTO_FAILURE");
    report_.state = "created";
    report_.code = "OK";
    report_.stage = "sink_resolve";
    Binding();
}
void PacketWriter::Binding() {
    const auto& s = session_->Snapshot();
    auto j = Parse("{}");
    auto& a = j.GetAllocator();
    String(j, "target", context_.target, a);
    j.AddMember("channel_revision", s.channel_revision, a);
    j.AddMember("connection_revision", s.connection_revision, a);
    String(j, "stream_id", config_.stream_id, a);
    std::string epoch;
    const char hex[] = "0123456789abcdef";
    for (auto b : epoch_) {
        epoch += hex[b >> 4];
        epoch += hex[b & 15];
    }
    String(j, "stream_epoch", epoch, a);
    String(j, "observation_domain_id", std::to_string(codec_.WireConfig().observation_domain_id), a);
    j.AddMember("capture_generation", codec_.Generation(), a);
    j.AddMember("partition", 0, a);
    j.AddMember("poll_sequence", poll_sequence_, a);
    rapidjson::Value inputs(rapidjson::kArrayType);
    for (const auto& i : codec_.WireConfig().inputs) {
        rapidjson::Value item(rapidjson::kObjectType);
        item.AddMember("source_id", i.source_id, a);
        item.AddMember("queue_id", i.queue_id, a);
        String(item, "source_name", i.source_name, a);
        inputs.PushBack(item, a);
    }
    j.AddMember("inputs", inputs, a);
    j.AddMember("max_packets_per_batch", codec_.WireConfig().max_packets_per_batch, a);
    j.AddMember("max_bytes_per_batch", codec_.WireConfig().max_bytes_per_batch, a);
    j.AddMember("max_message_bytes", config_.max_message_bytes, a);
    j.AddMember("queue_max_messages", config_.queue_max_messages, a);
    j.AddMember("queue_max_bytes", config_.queue_max_bytes, a);
    j.AddMember("delivery_timeout_ms", config_.delivery_timeout_ms, a);
    j.AddMember("queue_peak_messages", queue_peak_messages_, a);
    j.AddMember("queue_peak_bytes", queue_peak_bytes_, a);
    j.AddMember("encoded_peak_bytes", encoded_peak_bytes_, a);
    j.AddMember("max_inflight_polls", 1, a);
    const auto& source = codec_.Sources();
    j.AddMember("max_wait_ms", source.limits.max_wait_ms, a);
    j.AddMember("max_outstanding_batches", source.limits.max_outstanding_batches, a);
    j.AddMember("max_inspected_packets_per_poll", source.max_inspected_packets_per_poll, a);
    j.AddMember("max_inspected_bytes_per_poll", source.max_inspected_bytes_per_poll, a);
    j.AddMember("max_poll_work_ms", source.max_poll_work_ms, a);
    report_.binding_json = Encode(j);
}
int PacketWriter::Fail(int rc, const char* code, const char* stage) {
    session_->Cancel();
    std::lock_guard<std::mutex> lock(mutex_);
    report_.state = cancelled_ ? "cancelled" : "failed";
    report_.code = cancelled_ ? "CANCELLED" : code;
    report_.stage = stage;
    return cancelled_ ? ECANCELED : rc;
}
int PacketWriter::Open(const CaptureSourceSetV2& sources, std::shared_ptr<arrow::Schema> schema) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (report_.state != "created") return cancelled_ ? ECANCELED : EINVAL;
    }
    if (cancelled_) return Fail(ECANCELED, "CANCELLED", "sink_open");
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        codec_.Open(sources, schema);
        Binding();
        report_.stage = "sink_open";
    } catch (const std::invalid_argument& e) {
        return Fail(EINVAL, e.what(), "sink_open");
    }
    void* p = nullptr;
    int rc = session_->QueryInterface(IID_KAFKA_PRODUCER_SESSION_V1, &p);
    if (rc || !p) return Fail(rc ? rc : ENOTSUP, "PACKET_PRODUCER_UNAVAILABLE", "sink_open");
    producer_ = static_cast<IKafkaProducerSessionV1*>(p);
    std::string probe;
    rc = session_->Probe(std::min(config_.delivery_timeout_ms, 30000u), &probe);
    if (rc) return Fail(rc, "PACKET_SINK_CONNECTION_FAILED", "sink_open");
    std::lock_guard<std::mutex> lock(mutex_);
    report_.state = cancelled_ ? "cancelled" : "open";
    return cancelled_ ? ECANCELED : 0;
}
int PacketWriter::Write(const CapturePollEventV2& event) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(config_.delivery_timeout_ms);
    const uint64_t packets = event.block.batch ? std::max<int64_t>(0, event.block.batch->num_rows()) : 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (report_.state != "open") return cancelled_ ? ECANCELED : EINVAL;
        // A plain timeout has neither a message nor a sequence number.
        if (event.block.kind == BlockPollEvent::kTimeout && !event.block.batch && event.progress.empty() &&
            event.struct_size >= sizeof(CapturePollEventV2) && event.contract_version == 2 && !event.block.err)
            return 0;
        ++report_.messages.input;
        report_.packets.input += packets;
        report_.state = "writing";
        report_.stage = "sink_encode";
    }
    KafkaDeliveryV1 delivery{0, 1, 0};
    int rc = 0;
    const char* code = "INVALID_PACKET_ENVELOPE";
    const char* stage = "sink_encode";
    try {
        if (poll_sequence_ == UINT64_MAX) rc = EOVERFLOW;
        auto encoded = rc ? arrow::Result<KafkaMessageV1>(arrow::Status::Invalid("PACKET_SEQUENCE_EXHAUSTED"))
                          : codec_.Encode(event, epoch_, poll_sequence_ + 1,
                                          [&] { return cancelled_ || Clock::now() >= deadline; });
        if (!encoded.ok()) {
            rc = EINVAL;
            if (cancelled_ || Clock::now() >= deadline) {
                rc = cancelled_ ? ECANCELED : ETIMEDOUT;
                code = "PACKET_SINK_TIMEOUT";
            }
        } else {
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
            if (cancelled_ || remaining <= 0) {
                rc = cancelled_ ? ECANCELED : ETIMEDOUT;
                code = "PACKET_SINK_TIMEOUT";
            } else {
                std::vector<KafkaProduceMessageV1> messages;
                messages.push_back({std::move(encoded->key), std::move(encoded->value)});
                const auto bytes = messages[0].key.size() + messages[0].value.size();
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    report_.stage = "sink_deliver";
                    encoded_peak_bytes_ = std::max<uint64_t>(encoded_peak_bytes_, bytes);
                }
                stage = "sink_deliver";
                code = "PACKET_SINK_DELIVERY_FAILED";
                rc = producer_->Produce(messages, remaining, &delivery);
                std::lock_guard<std::mutex> lock(mutex_);
                if (delivery.confirmed + delivery.unknown) {
                    ++poll_sequence_;
                    queue_peak_messages_ = 1;
                    queue_peak_bytes_ = std::max<uint64_t>(queue_peak_bytes_, bytes);
                }
                Binding();
            }
        }
    } catch (...) {
        rc = EIO;
        code = "PACKET_SINK_ENCODING_FAILED";
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        report_.messages.confirmed += delivery.confirmed;
        report_.messages.not_sent += delivery.not_sent;
        report_.messages.unknown += delivery.unknown;
        report_.packets.confirmed += packets * delivery.confirmed;
        report_.packets.not_sent += packets * delivery.not_sent;
        report_.packets.unknown += packets * delivery.unknown;
        if (!rc && !cancelled_) {
            report_.state = "open";
            report_.code = "OK";
        }
    }
    if (rc || cancelled_) return Fail(rc ? rc : ECANCELED, code, stage);
    return 0;
}
int PacketWriter::Finish() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (report_.state != "open") return cancelled_ ? ECANCELED : EINVAL;
        report_.stage = "sink_finish";
    }
    int rc = producer_->Drain(config_.delivery_timeout_ms);
    if (rc) return Fail(rc, "PACKET_SINK_DRAIN_FAILED", "sink_finish");
    std::lock_guard<std::mutex> lock(mutex_);
    report_.state = cancelled_ ? "cancelled" : "finished";
    return cancelled_ ? ECANCELED : 0;
}
void PacketWriter::Cancel() {
    cancelled_ = true;
    session_->Cancel();
    std::lock_guard<std::mutex> lock(mutex_);
    if (report_.state != "finished" && report_.state != "failed") {
        report_.state = "cancelled";
        report_.code = "CANCELLED";
    }
}
PacketSinkReportV1 PacketWriter::Report() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return report_;
}
int Plugin::CreateWriter(const PacketSinkContextV1& context, std::unique_ptr<IPacketSinkWriterV1>* out) {
    if (!out) return EINVAL;
    out->reset();
    if (context.target.rfind("kafka.", 0) != 0) return ENOTSUP;
    if (context.contract_version != 1 || context.target.size() <= 6 || context.target.find('.', 6) != std::string::npos)
        return EINVAL;
    const auto name = context.target.substr(6);
    KafkaChannelDescriptionV1 d;
    int rc = DescribeChannel(name.c_str(), &d);
    if (rc) return rc;
    try {
        if (Text(store_.Query("channels", name)["items"][0], "payload_kind") != "packet") return ENOTSUP;
    } catch (const Failure& e) {
        return e.status == 404 ? ENOENT : EINVAL;
    }
    if (d.role == "source") return EINVAL;
    if (!d.sink_adapter_available) return ENOTSUP;
    {
        std::lock_guard<std::mutex> lock(readers_mutex_);
        if (!writer_names_.insert(name).second) return EBUSY;
    }
    std::shared_ptr<void> lease(reinterpret_cast<void*>(1), [this, name](void*) {
        std::lock_guard<std::mutex> lock(readers_mutex_);
        writer_names_.erase(name);
    });
    IKafkaClientSessionV1* raw = nullptr;
    std::string error;
    rc = OpenSession(name.c_str(), "sink", &raw, &error);
    if (rc) return rc;
    std::shared_ptr<IKafkaClientSessionV1> session(raw, [this](auto* p) { ReleaseSession(p); });
    try {
        auto config = ParsePacketSinkConfig(Parse(static_cast<Client*>(raw)->SinkOptions()));
        *out = std::make_unique<PacketWriter>(config, context, std::move(session), std::move(lease));
        return 0;
    } catch (...) {
        return EIO;
    }
}
}  // namespace flowsql::channels::kafka
