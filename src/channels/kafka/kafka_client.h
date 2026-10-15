// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <framework/interfaces/ikafka_consumer_session.h>
#include <librdkafka/rdkafka.h>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <thread>
#include <vector>
#include "kafka_store.h"

namespace flowsql::channels::kafka {
struct Deployment {
    std::map<std::string, std::string> files, environments;
    std::filesystem::path runtime_directory;
};
class Client final : public IKafkaClientSessionV1, public IKafkaConsumerSessionV1 {
 public:
    Client(Binding binding, const Deployment& deployment);
    ~Client() override;
    const KafkaBindingSnapshotV1& Snapshot() const override { return binding_.snapshot; }
    int Probe(uint32_t timeout_ms, std::string* result) override;
    void Cancel() override;
    int QueryInterface(const Guid&, void** capability) override;
    static Json Capabilities();
    const std::string& SourceOptions() const { return binding_.source_options_json; }
    int StartConsumer(const char* reset, uint32_t max_message_bytes) override;
    int PollMessage(uint32_t timeout_ms, KafkaMessageV1* message) override;
    int CommitOffsets(const std::vector<KafkaOffsetV1>& offsets, uint32_t timeout_ms) override;

 private:
    Binding binding_;
    Deployment deployment_;
    rd_kafka_t* client_ = nullptr;
    rd_kafka_t* consumer_ = nullptr;
    std::unique_ptr<rd_kafka_conf_t, decltype(&rd_kafka_conf_destroy)> consumer_config_{nullptr, rd_kafka_conf_destroy};
    uint32_t message_limit_ = 0;
    std::filesystem::path temporary_;
    std::atomic<bool> cancelled_{false};
    std::atomic<int> callback_error_{0}, consumer_error_{0};
    std::mutex wait_mutex_, operation_mutex_;
    std::condition_variable wake_;
    std::string ccache_;
    std::atomic<bool> oauth_failed_{false};
    int64_t ticket_expires_ = 0;
    std::thread maintenance_;
    std::atomic<bool> ticket_ready_{false};
    std::atomic<int64_t> probe_deadline_ms_{0};
    std::string File(const std::string& reference, size_t limit);
    std::string Secret(const std::string& reference);
    void Configure(rd_kafka_conf_t* configuration);
    bool Ticket(std::chrono::steady_clock::time_point deadline);
    static void OAuth(rd_kafka_t*, const char*, void* opaque);
    static void Error(rd_kafka_t*, int error, const char*, void* opaque);
    static void ConsumerError(rd_kafka_t*, int error, const char*, void* opaque);
    static void Log(const rd_kafka_t*, int, const char*, const char*) {}
};
}  // namespace flowsql::channels::kafka
