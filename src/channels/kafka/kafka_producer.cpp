// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <cerrno>
#include "kafka_client.h"
#include "kafka_packet.h"
#include "kafka_result_codec.h"
namespace flowsql::channels::kafka {
namespace {
using Clock = std::chrono::steady_clock;
SinkConfig Limits(const Binding& b) {
    if (b.snapshot.payload_kind != "packet") return ParseSinkConfig(Parse(b.sink_options_json));
    const auto p = ParsePacketSinkConfig(Parse(b.sink_options_json));
    SinkConfig c;
    c.max_message_bytes = p.max_message_bytes;
    c.queue_max_messages = p.queue_max_messages;
    c.queue_max_bytes = p.queue_max_bytes;
    c.delivery_timeout_ms = p.delivery_timeout_ms;
    return c;
}
struct Receipt {
    std::mutex mutex;
    uint64_t completed = 0, confirmed = 0;
    bool closed = false, error = false;
    Clock::time_point deadline;
};
void Delivered(rd_kafka_t*, const rd_kafka_message_t* message, void*) {
    std::unique_ptr<std::shared_ptr<Receipt>> tag(static_cast<std::shared_ptr<Receipt>*>(message->_private));
    if (!tag) return;
    auto& receipt = **tag;
    std::lock_guard<std::mutex> lock(receipt.mutex);
    if (receipt.closed) return;
    ++receipt.completed;
    if (!message->err && Clock::now() < receipt.deadline)
        ++receipt.confirmed;
    else
        receipt.error = true;
}
void Set(rd_kafka_conf_t* c, const char* key, int64_t value) {
    char error[512];
    if (rd_kafka_conf_set(c, key, std::to_string(value).c_str(), error, sizeof(error)) != RD_KAFKA_CONF_OK)
        throw Failure(503, "PRODUCER_CAPABILITY_UNAVAILABLE", "sink_open");
}
}  // namespace
void Client::ConfigureProducer(rd_kafka_conf_t* config) {
    if (binding_.snapshot.usage != "sink" || binding_.sink_options_json.empty()) return;
    auto c = Limits(binding_);
    Set(config, "enable.idempotence", 1);
    Set(config, "acks", -1);
    // kbytes is a driver ceiling; Produce also enforces the exact payload byte ceiling.
    Set(config, "queue.buffering.max.messages", c.queue_max_messages);
    Set(config, "queue.buffering.max.kbytes", (c.queue_max_bytes + 1023) / 1024);
    Set(config, "message.max.bytes", c.max_message_bytes + 4096);
    Set(config, "message.timeout.ms", std::max<uint32_t>(1000, c.delivery_timeout_ms));
    char error[512];
    if (rd_kafka_conf_set(config, "partitioner", "murmur2", error, sizeof(error)) != RD_KAFKA_CONF_OK)
        throw Failure(503, "PRODUCER_CAPABILITY_UNAVAILABLE", "sink_open");
    rd_kafka_conf_set_dr_msg_cb(config, Delivered);
}
int Client::Produce(const std::vector<KafkaProduceMessageV1>& messages, uint32_t timeout_ms, KafkaDeliveryV1* out) {
    if (!out) return EINVAL;
    *out = {0, messages.size(), 0};
    std::lock_guard<std::mutex> operation(operation_mutex_);
    if (cancelled_) return ECANCELED;
    if (producer_failed_ || binding_.snapshot.usage != "sink" || binding_.sink_options_json.empty()) return EINVAL;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    const auto c = Limits(binding_);
    if (!timeout_ms || timeout_ms > c.delivery_timeout_ms || messages.size() > c.queue_max_messages) return EINVAL;
    size_t bytes = 0;
    for (const auto& m : messages) {
        bytes += m.key.size() + m.value.size();
        if (m.key.size() > 4096 || m.key.size() + m.value.size() > c.max_message_bytes || bytes > c.queue_max_bytes)
            return EMSGSIZE;
    }
    if (binding_.snapshot.payload_kind == "packet") {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        const int topology = remaining > 0 ? CheckPacketTopology(remaining) : ETIMEDOUT;
        if (topology) {
            producer_failed_ = true;
            return topology;
        }
    }
    auto receipt = std::make_shared<Receipt>();
    receipt->deadline = deadline;
    uint64_t accepted = 0;
    int rc = 0;
    for (const auto& m : messages) {
        while (true) {
            if (cancelled_ || Clock::now() >= receipt->deadline) {
                rc = cancelled_ ? ECANCELED : ETIMEDOUT;
                break;
            }
            if (oauth_failed_ || callback_error_ == RD_KAFKA_RESP_ERR__AUTHENTICATION ||
                callback_error_ == RD_KAFKA_RESP_ERR__SSL) {
                rc = EACCES;
                break;
            }
            auto tag = std::make_unique<std::shared_ptr<Receipt>>(receipt);
            auto err = rd_kafka_producev(
                client_, RD_KAFKA_V_TOPIC(binding_.snapshot.topic.c_str()),
                RD_KAFKA_V_PARTITION((binding_.snapshot.payload_kind == "packet" ? 0 : RD_KAFKA_PARTITION_UA)),
                RD_KAFKA_V_MSGFLAGS(RD_KAFKA_MSG_F_COPY), RD_KAFKA_V_KEY(m.key.data(), m.key.size()),
                RD_KAFKA_V_VALUE(const_cast<char*>(m.value.data()), m.value.size()), RD_KAFKA_V_OPAQUE(tag.get()),
                RD_KAFKA_V_END);
            if (!err) {
                tag.release();
                ++accepted;
                break;
            }
            if (err != RD_KAFKA_RESP_ERR__QUEUE_FULL) {
                rc = EIO;
                break;
            }
            rd_kafka_poll(client_, 10);
        }
        if (rc) break;
    }
    while (!rc) {
        {
            std::lock_guard<std::mutex> lock(receipt->mutex);
            if (receipt->error) {
                rc = EIO;
                break;
            }
            if (receipt->completed == accepted) break;
        }
        if (cancelled_ || Clock::now() >= receipt->deadline) {
            rc = cancelled_ ? ECANCELED : ETIMEDOUT;
            break;
        }
        rd_kafka_poll(client_, 10);
    }
    {
        std::lock_guard<std::mutex> lock(receipt->mutex);
        receipt->closed = true;
        out->confirmed = receipt->confirmed;
        out->not_sent = messages.size() - accepted;
        out->unknown = accepted - out->confirmed;
        if (receipt->error && !rc) rc = EIO;
    }
    if (rc) {
        producer_failed_ = true;
        rd_kafka_purge(client_, RD_KAFKA_PURGE_F_QUEUE | RD_KAFKA_PURGE_F_INFLIGHT | RD_KAFKA_PURGE_F_NON_BLOCKING);
        rd_kafka_poll(client_, 0);
    }
    return rc;
}
int Client::Drain(uint32_t timeout_ms) {
    std::lock_guard<std::mutex> operation(operation_mutex_);
    if (cancelled_) return ECANCELED;
    if (producer_failed_) return EIO;
    auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (rd_kafka_outq_len(client_)) {
        if (cancelled_) return ECANCELED;
        if (Clock::now() >= deadline) return ETIMEDOUT;
        rd_kafka_poll(client_, 10);
    }
    return 0;
}
}  // namespace flowsql::channels::kafka
