// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <channels/kafka/kafka_plugin.h>
#include <channels/kafka/kafka_writer.h>
#include <librdkafka/rdkafka_mock.h>
#include <tests/common/test_environment.h>
#include <atomic>
#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <thread>
using namespace flowsql;
using namespace flowsql::channels::kafka;
namespace {
SinkConfig Config() {
    return ParseSinkConfig(Parse(
        R"({"encoding":"json","entity":"evt","schema_version":1,"record_kind":"event","queue_max_messages":2,"delivery_timeout_ms":2000})"));
}
std::shared_ptr<arrow::RecordBatch> Batch(int rows = 5) {
    arrow::Int64Builder b;
    for (int i = 0; i < rows; ++i) assert(b.Append(i).ok());
    return arrow::RecordBatch::Make(arrow::schema({arrow::field("n", arrow::int64())}), rows,
                                    {b.Finish().ValueOrDie()});
}
struct Fake final : IKafkaClientSessionV1, IKafkaProducerSessionV1 {
    KafkaBindingSnapshotV1 snapshot;
    std::atomic<bool> cancelled{false}, entered{false};
    bool wait = false;
    int calls = 0, drains = 0, fail_call = -1, drain_rc = 0, probe_rc = 0;
    uint64_t confirmed = 0;
    const KafkaBindingSnapshotV1& Snapshot() const override { return snapshot; }
    int Probe(uint32_t, std::string*) override { return probe_rc; }
    void Cancel() override { cancelled = true; }
    int QueryInterface(const Guid&, void** out) override {
        *out = static_cast<IKafkaProducerSessionV1*>(this);
        return 0;
    }
    int Produce(const std::vector<KafkaProduceMessageV1>& m, uint32_t timeout, KafkaDeliveryV1* out) override {
        assert(m.size() <= 2 && timeout <= 2000);
        entered = true;
        while (wait && !cancelled) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (cancelled || calls++ == fail_call) {
            *out = {1, 0, m.size() - 1};
            return cancelled ? ECANCELED : EIO;
        }
        *out = {m.size(), 0, 0};
        confirmed += m.size();
        return 0;
    }
    int Drain(uint32_t) override {
        ++drains;
        return drain_rc;
    }
};
std::shared_ptr<Client> Session(const char* brokers, const std::string& topic = "events") {
    Binding b;
    b.connection = Parse(R"({"name":"mock","bootstrap_servers":[],"security_protocol":"PLAINTEXT"})");
    b.connection["bootstrap_servers"].PushBack(rapidjson::Value(brokers, b.connection.GetAllocator()),
                                               b.connection.GetAllocator());
    b.snapshot.usage = "sink";
    b.snapshot.topic = topic;
    b.sink_options_json =
        R"({"encoding":"json","entity":"evt","schema_version":1,"record_kind":"event","queue_max_messages":2,"delivery_timeout_ms":2000})";
    return std::make_shared<Client>(std::move(b), Deployment{});
}
void Balanced(const ResultSinkReportV1& r) {
    assert(r.input_rows == r.confirmed_rows + r.not_sent_rows + r.unknown_rows);
}
}  // namespace
int main() {
    {
        auto fake = std::make_shared<Fake>();
        fake->probe_rc = EACCES;
        ResultWriter w(Config(), {1, "kafka.events", "t"}, fake);
        assert(w.Open(Batch()->schema()) == EACCES);
        auto report = w.Report();
        assert(report.stage == "sink_open" && report.state == "failed");
        assert(std::strlen(Parse(report.binding_json)["schema_fingerprint"].GetString()) == 64);
    }
    {
        auto fake = std::make_shared<Fake>();
        ResultWriter w(Config(), {1, "kafka.events", "t"}, fake);
        w.Cancel();
        assert(w.Report().stage == "sink_resolve" && w.Report().code == "CANCELLED");
    }
    for (int fail : {-1, 0, 1}) {
        auto fake = std::make_shared<Fake>();
        fake->fail_call = fail;
        ResultWriter w(Config(), {1, "kafka.events", "t"}, fake);
        auto batch = Batch();
        assert(w.Open(batch->schema()) == 0);
        int rc = w.Write(batch);
        auto r = w.Report();
        Balanced(r);
        assert(r.input_rows == 5);
        if (fail < 0) {
            assert(rc == 0 && r.confirmed_rows == 5 && fake->calls == 3);
            assert(w.Finish() == 0 && w.Report().state == "finished");
        } else {
            assert(rc != 0 && r.confirmed_rows == uint64_t(fail * 2 + 1) && r.unknown_rows == 1 &&
                   r.not_sent_rows == uint64_t(5 - (fail + 1) * 2));
            assert(w.Write(batch) != 0 && w.Finish() != 0 && fake->drains == 0);
        }
    }
    {
        auto fake = std::make_shared<Fake>();
        ResultWriter w(Config(), {1, "kafka.events", "t"}, fake);
        assert(w.Open(Batch()->schema()) == 0);
        auto drift =
            arrow::RecordBatch::Make(arrow::schema({arrow::field("x", arrow::int64())}), 5, Batch()->columns());
        assert(w.Write(drift) != 0);
        assert(fake->calls == 0 && w.Report().not_sent_rows == 5);
        Balanced(w.Report());
    }
    {
        auto fake = std::make_shared<Fake>();
        fake->drain_rc = ETIMEDOUT;
        ResultWriter w(Config(), {1, "kafka.events", "t"}, fake);
        assert(w.Open(Batch()->schema()) == 0 && w.Write(Batch(0)) == 0);
        assert(w.Finish() == ETIMEDOUT && w.Report().stage == "sink_finish");
        Balanced(w.Report());
    }
    {
        auto fake = std::make_shared<Fake>();
        fake->wait = true;
        ResultWriter w(Config(), {1, "kafka.events", "t"}, fake);
        assert(w.Open(Batch()->schema()) == 0);
        std::thread worker([&] { assert(w.Write(Batch()) == ECANCELED); });
        while (!fake->entered) std::this_thread::yield();
        auto started = std::chrono::steady_clock::now();
        w.Cancel();
        w.Cancel();
        worker.join();
        assert(std::chrono::steady_clock::now() - started < std::chrono::seconds(1));
        Balanced(w.Report());
        assert(w.Report().state == "cancelled" && fake->drains == 0);
    }
    auto* config = rd_kafka_conf_new();
    char error[512];
    rd_kafka_conf_set(config, "log_level", "0", error, sizeof(error));
    auto* owner = rd_kafka_new(RD_KAFKA_PRODUCER, config, error, sizeof(error));
    assert(owner);
    auto* cluster = rd_kafka_mock_cluster_new(owner, 1);
    if (!cluster) {
        std::cerr << "execution_denied: mock broker loopback unavailable\n";
        return 1;
    }
    const auto* brokers = rd_kafka_mock_cluster_bootstraps(cluster);
    assert(rd_kafka_mock_topic_create(cluster, "events", 1, 1) == 0);
    {
        auto session = Session(brokers);
        ResultWriter w(Config(), {1, "kafka.events", "mock"}, session);
        auto b = Batch();
        assert(w.Open(b->schema()) == 0);
        assert(w.Write(b) == 0);
        assert(w.Finish() == 0);
        Balanced(w.Report());
        assert(w.Report().confirmed_rows == 5);
        KafkaDeliveryV1 d;
        assert(session->Produce({{"k", "v"}, {"k", "v"}, {"k", "v"}}, 1000, &d) == EINVAL && d.not_sent == 3);
    }
    {
        auto session = Session(brokers);
        rd_kafka_mock_broker_set_rtt(cluster, 1, 1000);
        KafkaDeliveryV1 d;
        auto start = std::chrono::steady_clock::now();
        assert(session->Produce({{"k", "v"}}, 100, &d) == ETIMEDOUT);
        assert(d.confirmed == 0 && d.not_sent + d.unknown == 1);
        auto snapshot = d;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        assert(d.confirmed == snapshot.confirmed);
        assert(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(700));
        rd_kafka_mock_broker_set_rtt(cluster, 1, 0);
    }
    {
        auto session = Session(brokers);
        rd_kafka_mock_broker_set_down(cluster, 1);
        KafkaDeliveryV1 d;
        std::thread worker([&] { assert(session->Produce({{"k", "v"}}, 2000, &d) == ECANCELED); });
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        session->Cancel();
        worker.join();
        assert(d.confirmed + d.not_sent + d.unknown == 1);
        rd_kafka_mock_broker_set_up(cluster, 1);
    }
    {
        std::array<unsigned char, 32> key{};
        const auto kp = flowsql::test::WorkspacePath("sink.key");
        {
            std::ofstream f(kp, std::ios::binary);
            f.write(reinterpret_cast<const char*>(key.data()), key.size());
        }
        auto options = Parse("{}");
        auto& a = options.GetAllocator();
        String(options, "db_path", flowsql::test::WorkspacePath("sink.db"), a);
        String(options, "master_key_file", kp, a);
        String(options, "runtime_directory", flowsql::test::WorkspacePath("runtime"), a);
        Plugin plugin;
        assert(plugin.Option(Encode(options).c_str()) == 0 && plugin.Load(nullptr) == 0);
        auto conn = Parse(R"({"name":"mock","bootstrap_servers":[],"security_protocol":"PLAINTEXT"})");
        conn["bootstrap_servers"].PushBack(rapidjson::Value(brokers, conn.GetAllocator()), conn.GetAllocator());
        plugin.Definitions().Mutate("connections", "add", std::move(conn));
        plugin.Definitions().Mutate(
            "channels", "add",
            Parse(
                R"({"name":"events","connection_name":"mock","role":"sink","topic":"events","payload_kind":"structured","payload_options":{"sink":{"encoding":"json","entity":"evt","schema_version":1,"record_kind":"event"}}})"));
        std::unique_ptr<IResultSinkWriterV1> w1, w2;
        assert(plugin.CreateWriter({1, "kafka.events", "t"}, &w1) == 0 &&
               plugin.CreateWriter({1, "kafka.events", "t"}, &w2) == 0);
        assert(w1->Report().publication_id != w2->Report().publication_id);
        assert(plugin.Definitions().Query("channels", "events")["items"][0]["in_use"].GetBool());
        auto b = Batch();
        assert(w1->Open(b->schema()) == 0 && w1->Write(b) == 0 && w1->Finish() == 0);
        w1.reset();
        assert(plugin.Definitions().Query("channels", "events")["items"][0]["in_use"].GetBool());
        w2.reset();
        assert(!plugin.Definitions().Query("channels", "events")["items"][0]["in_use"].GetBool());
        assert(plugin.Unload() == 0);
    }
    rd_kafka_mock_cluster_destroy(cluster);
    rd_kafka_destroy(owner);
    std::cout << "writer/delivery/lifetime contracts passed\n";
}
