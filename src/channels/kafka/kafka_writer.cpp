// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include "kafka_writer.h"
#include <openssl/rand.h>
#include <cerrno>
#include <optional>
#include "kafka_plugin.h"
namespace flowsql::channels::kafka {
namespace {
using Clock = std::chrono::steady_clock;
std::string Publication() {
    unsigned char bytes[16];
    if (RAND_bytes(bytes, sizeof(bytes)) != 1) throw Failure(500, "CRYPTO_FAILURE");
    std::string id;
    const char hex[] = "0123456789abcdef";
    for (auto b : bytes) {
        id += hex[b >> 4];
        id += hex[b & 15];
    }
    return id;
}
}  // namespace
ResultWriter::ResultWriter(SinkConfig c, ResultSinkContextV1 context, std::shared_ptr<IKafkaClientSessionV1> session)
    : config_(std::move(c)), context_(std::move(context)), session_(std::move(session)) {
    report_.publication_id = Publication();
    report_.state = "created";
    report_.code = "OK";
    report_.stage = "sink_resolve";
    codec_ = std::make_unique<ResultCodec>(config_, report_.publication_id, context_.task_id);
    Binding();
}
void ResultWriter::Binding() {
    auto& s = session_->Snapshot();
    auto j = Parse("{}");
    auto& a = j.GetAllocator();
    String(j, "target", context_.target, a);
    j.AddMember("channel_revision", s.channel_revision, a);
    j.AddMember("connection_revision", s.connection_revision, a);
    String(j, "schema_fingerprint", codec_->Fingerprint(), a);
    report_.binding_json = Encode(j);
}
int ResultWriter::Fail(int rc, const std::string& code, const char* stage) {
    std::lock_guard<std::mutex> lock(mutex_);
    report_.state = cancelled_ ? "cancelled" : "failed";
    report_.code = cancelled_ ? "CANCELLED" : code;
    report_.stage = stage;
    return cancelled_ ? ECANCELED : rc;
}
int ResultWriter::Open(std::shared_ptr<arrow::Schema> schema) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (report_.state != "created") return EINVAL;
    }
    if (cancelled_) return Fail(ECANCELED, "CANCELLED", "sink_open");
    try {
        codec_->Open(schema);
        std::lock_guard<std::mutex> lock(mutex_);
        Binding();
        report_.stage = "sink_open";
    } catch (const std::invalid_argument& e) {
        return Fail(EINVAL, e.what(), "sink_open");
    }
    void* capability = nullptr;
    int rc = session_->QueryInterface(IID_KAFKA_PRODUCER_SESSION_V1, &capability);
    if (rc || !capability) return Fail(rc ? rc : ENOTSUP, "PRODUCER_UNAVAILABLE", "sink_open");
    producer_ = static_cast<IKafkaProducerSessionV1*>(capability);
    // This verifies authentication/Topic metadata without any probe publication.
    std::string probe;
    rc = session_->Probe(std::min(config_.delivery_timeout_ms, 30000u), &probe);
    if (rc) return Fail(rc, "SINK_CONNECTION_FAILED", "sink_open");
    std::lock_guard<std::mutex> lock(mutex_);
    report_.state = cancelled_ ? "cancelled" : "open";
    return cancelled_ ? ECANCELED : 0;
}
int ResultWriter::Write(const std::shared_ptr<arrow::RecordBatch>& batch) {
    uint64_t ordinal;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (report_.state != "open") return cancelled_ ? ECANCELED : EINVAL;
        if (!batch) return EINVAL;
        ordinal = report_.input_rows;
        report_.input_rows += batch->num_rows();
        report_.state = "writing";
        report_.stage = "sink_encode";
    }
    const auto deadline = Clock::now() + std::chrono::milliseconds(config_.delivery_timeout_ms);
    uint64_t accounted = 0;
    auto remainder = [&] {
        std::lock_guard<std::mutex> lock(mutex_);
        report_.not_sent_rows += batch->num_rows() - accounted;
    };
    try {
        codec_->Validate(*batch);
        int64_t row = 0;
        std::optional<KafkaProduceMessageV1> pending;
        while (row < batch->num_rows() || pending) {
            if (cancelled_ || Clock::now() >= deadline) {
                remainder();
                return Fail(cancelled_ ? ECANCELED : ETIMEDOUT, "SINK_TIMEOUT", "sink_deliver");
            }
            std::vector<KafkaProduceMessageV1> window;
            size_t bytes = 0;
            while (window.size() < config_.queue_max_messages && (row < batch->num_rows() || pending)) {
                auto message = pending ? std::move(*pending) : codec_->Row(*batch, row, ordinal + row);
                if (pending)
                    pending.reset();
                else
                    ++row;
                size_t n = message.key.size() + message.value.size();
                if (!window.empty() && bytes + n > config_.queue_max_bytes) {
                    pending = std::move(message);
                    break;
                }
                bytes += n;
                window.push_back(std::move(message));
                if (cancelled_ || Clock::now() >= deadline) break;
            }
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
            if (cancelled_ || remaining <= 0) {
                remainder();
                return Fail(cancelled_ ? ECANCELED : ETIMEDOUT, "SINK_TIMEOUT", "sink_deliver");
            }
            KafkaDeliveryV1 delivery;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                report_.stage = "sink_deliver";
            }
            int rc = producer_->Produce(window, remaining, &delivery);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                report_.confirmed_rows += delivery.confirmed;
                report_.not_sent_rows += delivery.not_sent;
                report_.unknown_rows += delivery.unknown;
            }
            accounted += window.size();
            if (rc) {
                remainder();
                return Fail(rc, "SINK_DELIVERY_FAILED", "sink_deliver");
            }
        }
    } catch (const std::invalid_argument& e) {
        remainder();
        return Fail(EINVAL, e.what(), "sink_encode");
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        report_.state = cancelled_ ? "cancelled" : "open";
    }
    return cancelled_ ? ECANCELED : 0;
}
int ResultWriter::Finish() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (report_.state != "open") return cancelled_ ? ECANCELED : EINVAL;
    }
    if (cancelled_) return Fail(ECANCELED, "CANCELLED", "sink_finish");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        report_.stage = "sink_finish";
    }
    int rc = producer_->Drain(config_.delivery_timeout_ms);
    if (rc) return Fail(rc, "SINK_DRAIN_FAILED", "sink_finish");
    std::lock_guard<std::mutex> lock(mutex_);
    report_.state = cancelled_ ? "cancelled" : "finished";
    return cancelled_ ? ECANCELED : 0;
}
void ResultWriter::Cancel() {
    cancelled_ = true;
    session_->Cancel();
    std::lock_guard<std::mutex> lock(mutex_);
    if (report_.state != "finished" && report_.state != "failed") {
        report_.state = "cancelled";
        report_.code = "CANCELLED";
    }
}
ResultSinkReportV1 ResultWriter::Report() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return report_;
}
int Plugin::CreateWriter(const ResultSinkContextV1& context, std::unique_ptr<IResultSinkWriterV1>* out) {
    if (!out) return EINVAL;
    out->reset();
    if (context.target.rfind("kafka.", 0) != 0) return ENOTSUP;
    if (context.contract_version != 1 || context.target.size() <= 6 || context.target.find('.', 6) != std::string::npos)
        return EINVAL;
    const auto name = context.target.substr(6);
    KafkaChannelDescriptionV1 description;
    int rc = DescribeChannel(name.c_str(), &description);
    if (rc) return rc;
    try {
        if (Text(store_.Query("channels", name)["items"][0], "payload_kind") == "packet") return ENOTSUP;
    } catch (const Failure& e) {
        return e.status == 404 ? ENOENT : EINVAL;
    }
    if (description.role == "source") return EINVAL;
    if (!description.sink_adapter_available) return ENOTSUP;
    IKafkaClientSessionV1* raw = nullptr;
    std::string error;
    rc = OpenSession(name.c_str(), "sink", &raw, &error);
    if (rc) return rc;
    std::shared_ptr<IKafkaClientSessionV1> session(raw, [this](auto* p) { ReleaseSession(p); });
    try {
        auto* client = static_cast<Client*>(raw);
        auto config = ParseSinkConfig(Parse(client->SinkOptions()));
        *out = std::make_unique<ResultWriter>(std::move(config), context, std::move(session));
        return 0;
    } catch (...) {
        return EIO;
    }
}
}  // namespace flowsql::channels::kafka
