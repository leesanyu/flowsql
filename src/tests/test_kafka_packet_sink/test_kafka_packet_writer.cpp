// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <channels/kafka/kafka_plugin.h>
#include <librdkafka/rdkafka_mock.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <tests/common/test_environment.h>
#include <unistd.h>
#include <common/loader.hpp>
#include <fstream>
#include <iostream>
#include "packet_sink_test_support.h"
using namespace packet_sink_test;
namespace {
std::shared_ptr<Client> Session(const char* brokers, const char* topic = "packets", const char* options = nullptr) {
    Binding b;
    b.connection = Parse(R"({"name":"mock","bootstrap_servers":[],"security_protocol":"PLAINTEXT"})");
    b.connection["bootstrap_servers"].PushBack(rapidjson::Value(brokers, b.connection.GetAllocator()),
                                               b.connection.GetAllocator());
    b.snapshot.usage = "sink";
    b.snapshot.payload_kind = "packet";
    b.snapshot.topic = topic;
    b.sink_options_json =
        R"({"encoding":"flowsql_packet_v1","stream_id":"probe-a","queue_max_messages":1,"delivery_timeout_ms":2000})";
    if (options) b.sink_options_json = options;
    return std::make_shared<Client>(std::move(b), Deployment{});
}
void FakeCases() {
    for (int mode = 0; mode < 6; ++mode) {
        auto session = std::make_shared<FakeSession>();
        if (mode == 1) session->fail_call = 1;
        if (mode == 2) session->drain_rc = ETIMEDOUT;
        if (mode == 3) session->probe_rc = EACCES;
        PacketWriter w(Config(), {1, "kafka.packets", "t"}, session);
        if (mode == 3) {
            assert(w.Open(Sources(), packet::PacketFamilySchema()) == EACCES);
            assert(w.Report().stage == "sink_open");
            continue;
        }
        assert(!w.Open(Sources(), packet::PacketFamilySchema()));
        assert(!w.Write(CapturePollEventV2{}));
        assert(w.Report().messages.input == 0);
        auto e = Event();
        assert(!w.Write(e));
        auto next = Event(2);
        if (mode == 4) next.progress[0].generation++;
        if (mode == 5) next.progress[0].fact_sequence = 1;
        int rc = w.Write(next);
        Balanced(w.Report());
        if (mode == 1 || mode == 4 || mode == 5) {
            assert(rc != 0 && w.Write(Event(3)) != 0 && w.Finish() != 0);
            assert(w.Report().packets.confirmed == 3 && session->drains == 0);
            assert(w.Report().packets.unknown == (mode == 1 ? 3u : 0u));
            assert(w.Report().packets.not_sent == (mode == 1 ? 0u : 3u));
        } else {
            assert(!rc && !w.Write(Event(3, 0)));
            Balanced(w.Report());
            assert(w.Report().messages.confirmed == 3 && w.Report().packets.confirmed == 6);
            rc = w.Finish();
            assert(rc == (mode == 2 ? ETIMEDOUT : 0));
            auto binding = Parse(w.Report().binding_json);
            assert(binding["queue_peak_messages"].GetUint64() == 1 && binding["inputs"].Size() == 2);
            assert(binding["poll_sequence"].GetUint64() == 3);
        }
    }
    {
        auto session = std::make_shared<FakeSession>();
        PacketWriter w(Config(), {1, "kafka.packets", "t"}, session);
        auto s = Sources();
        std::string name = "owned-source";
        s.inputs[0].source_name = name.c_str();
        assert(!w.Open(s, packet::PacketFamilySchema()));
        name.assign(200, 'x');
        assert(Text(Parse(w.Report().binding_json)["inputs"][0], "source_name") == "owned-source");
        session->wait = true;
        auto e = Event();
        std::thread work([&] { assert(w.Write(e) == ECANCELED); });
        while (!session->entered) std::this_thread::yield();
        auto start = std::chrono::steady_clock::now();
        w.Cancel();
        w.Cancel();
        work.join();
        assert(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
        Balanced(w.Report());
        assert(w.Report().state == "cancelled" && w.Report().packets.unknown == 3 && session->drains == 0);
        assert(e.block.batch->ValidateFull().ok());
    }
    {
        auto session = std::make_shared<FakeSession>();
        {
            PacketWriter w(Config(), {1, "kafka.packets", "t"}, session);
            assert(!w.Open(Sources(), packet::PacketFamilySchema()));
        }
        assert(session->calls == 0 && session->drains == 0);
    }
}
void MockCases() {
    char error[512];
    auto* conf = rd_kafka_conf_new();
    rd_kafka_conf_set(conf, "log_level", "0", error, sizeof(error));
    auto* owner = rd_kafka_new(RD_KAFKA_PRODUCER, conf, error, sizeof(error));
    assert(owner);
    auto* cluster = rd_kafka_mock_cluster_new(owner, 1);
    assert(cluster);
    assert(!rd_kafka_mock_topic_create(cluster, "packets", 1, 1));
    assert(!rd_kafka_mock_topic_create(cluster, "invalid", 2, 1));
    const char* brokers = rd_kafka_mock_cluster_bootstraps(cluster);
    {
        auto s = Session(brokers);
        KafkaDeliveryV1 d;
        assert(!s->Produce({{"probe-a", "raw"}}, 2000, &d) && d.confirmed == 1);
        assert(s->Produce({{"probe-a", "raw"}, {"probe-a", "raw"}}, 2000, &d) == EINVAL && d.not_sent == 2);
        assert(s->Produce({{"k", std::string(2097153, 'x')}}, 2000, &d) == EMSGSIZE && d.not_sent == 1);
        assert(!s->Drain(2000));
        auto* cc = rd_kafka_conf_new();
        rd_kafka_conf_set(cc, "bootstrap.servers", brokers, error, sizeof(error));
        rd_kafka_conf_set(cc, "group.id", "packet-sink-test", error, sizeof(error));
        rd_kafka_conf_set(cc, "log_level", "0", error, sizeof(error));
        auto* consumer = rd_kafka_new(RD_KAFKA_CONSUMER, cc, error, sizeof(error));
        assert(consumer);
        rd_kafka_poll_set_consumer(consumer);
        auto* assignment = rd_kafka_topic_partition_list_new(1);
        rd_kafka_topic_partition_list_add(assignment, "packets", 0)->offset = RD_KAFKA_OFFSET_BEGINNING;
        assert(!rd_kafka_assign(consumer, assignment));
        rd_kafka_topic_partition_list_destroy(assignment);
        bool received = false;
        for (int n = 0; n < 60 && !received; ++n) {
            auto* m = rd_kafka_consumer_poll(consumer, 100);
            if (m) {
                if (!m->err) {
                    assert(m->partition == 0 && std::string(static_cast<char*>(m->payload), m->len) == "raw");
                    received = true;
                }
                rd_kafka_message_destroy(m);
            }
        }
        assert(received);
        rd_kafka_consumer_close(consumer);
        rd_kafka_destroy(consumer);
    }
    {
        auto s = Session(
            brokers, "packets",
            R"({"encoding":"flowsql_packet_v1","stream_id":"probe-a","max_message_bytes":1048576,"queue_max_bytes":1048576,"queue_max_messages":64,"delivery_timeout_ms":2000})");
        KafkaDeliveryV1 d;
        assert(s->Produce({{"k", std::string(600000, 'a')}, {"k", std::string(600000, 'b')}}, 2000, &d) == EMSGSIZE &&
               d.not_sent == 2);
    }
    {
        auto s = Session(brokers);
        KafkaDeliveryV1 d;
        assert(!s->Produce({{"k", "warm"}}, 2000, &d));
        assert(!rd_kafka_mock_broker_push_request_error_rtts(cluster, 1, 0, 1, RD_KAFKA_RESP_ERR_NO_ERROR, 500));
        assert(s->Produce({{"k", "late"}}, 100, &d) == ETIMEDOUT && d.unknown == 1 && d.confirmed == 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(550));
        assert(d.confirmed == 0 && d.unknown == 1 && s->Drain(100) != 0);
    }
    {
        auto s = Session(brokers, "invalid");
        KafkaDeliveryV1 d;
        assert(s->Produce({{"k", "v"}}, 2000, &d) == EPROTO && d.not_sent == 1);
    }
    {
        auto s = Session(brokers);
        rd_kafka_mock_broker_set_rtt(cluster, 1, 1000);
        KafkaDeliveryV1 d;
        const auto start = std::chrono::steady_clock::now();
        assert(s->Produce({{"k", "v"}}, 100, &d) != 0);
        assert(d.confirmed + d.not_sent + d.unknown == 1 &&
               std::chrono::steady_clock::now() - start < std::chrono::milliseconds(500));
        const auto before = d;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        assert(d.confirmed == before.confirmed);
        rd_kafka_mock_broker_set_rtt(cluster, 1, 0);
    }
    {
        const auto key = flowsql::test::WorkspacePath("packet-sink.key");
        {
            std::ofstream f(key, std::ios::binary);
            std::string bytes(32, 'k');
            f << bytes;
        }
        auto options = Parse("{}");
        auto& a = options.GetAllocator();
        String(options, "db_path", flowsql::test::WorkspacePath("packet-sink.db"), a);
        String(options, "master_key_file", key, a);
        String(options, "runtime_directory", flowsql::test::WorkspacePath("packet-runtime"), a);
        Plugin p;
        assert(!p.Option(Encode(options).c_str()) && !p.Load(nullptr));
        auto c = Parse(R"({"name":"mock","bootstrap_servers":[],"security_protocol":"PLAINTEXT"})");
        c["bootstrap_servers"].PushBack(rapidjson::Value(brokers, c.GetAllocator()), c.GetAllocator());
        p.Definitions().Mutate("connections", "add", std::move(c));
        p.Definitions().Mutate(
            "channels", "add",
            Parse(
                R"({"name":"packets","connection_name":"mock","role":"sink","topic":"packets","payload_kind":"packet","payload_options":{"sink":{"encoding":"flowsql_packet_v1","stream_id":"probe-a"}}})"));
        assert(p.Definitions().Query("channels", "packets")["items"][0]["adapter_status"]["sink"] == "ready");
        std::unique_ptr<IPacketSinkWriterV1> first, second;
        PacketSinkContextV1 context{1, "kafka.packets", "t"};
        assert(!p.CreateWriter(context, &first) && p.CreateWriter(context, &second) == EBUSY && !second);
        assert(p.Definitions().Query("channels", "packets")["items"][0]["in_use"].GetBool());
        auto epoch = Text(Parse(first->Report().binding_json), "stream_epoch");
        first.reset();
        assert(!p.Definitions().Query("channels", "packets")["items"][0]["in_use"].GetBool());
        assert(!p.CreateWriter(context, &second));
        assert(Text(Parse(second->Report().binding_json), "stream_epoch") != epoch);
        second.reset();
        assert(!p.Unload());
        auto* loader = PluginLoader::Single();
        struct Libraries {
            PluginLoader* loader;
            ~Libraries() {
                loader->StopAll();
                loader->Unload();
            }
        } libraries{loader};
        const char* names[] = {"libflowsql_kafka.so"};
        const std::string text = Encode(options);
        const char* opts[] = {text.c_str()};
        assert(!loader->Load(get_absolute_process_path(), names, opts, 1));
        assert(!loader->StartAll());
        auto* factory = static_cast<IPacketSinkFactoryV1*>(loader->First(IID_PACKET_SINK_FACTORY_V1));
        assert(factory);
        std::unique_ptr<IPacketSinkWriterV1> dynamic_writer;
        assert(!factory->CreateWriter(context, &dynamic_writer));
        dynamic_writer->Cancel();
        assert(dynamic_writer->Report().state == "cancelled");
        // dynamic_writer is destroyed before libraries; its session/lease callbacks still live in the .so.
    }
    rd_kafka_mock_cluster_destroy(cluster);
    rd_kafka_destroy(owner);
}
}  // namespace
int main() {
    FakeCases();
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (listener < 0 || bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address))) {
        if (listener >= 0) close(listener);
        std::cerr << "execution_denied: loopback listener required for Kafka mock\n";
        return 1;
    }
    close(listener);
    MockCases();
    std::cout
        << "PASS packet writer delivery/unknown/not_sent, bounded queues, lease, cancellation and mock partition0\n";
}
