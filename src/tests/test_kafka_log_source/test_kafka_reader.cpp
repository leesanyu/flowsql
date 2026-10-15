// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <channels/kafka/kafka_plugin.h>
#include <channels/kafka/kafka_reader.h>
#include <librdkafka/rdkafka_mock.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <tests/common/test_environment.h>
#include <unistd.h>
#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <thread>
using namespace flowsql;
using namespace flowsql::channels::kafka;
namespace {
StructuredConfig Config() {
    return ParseStructuredConfig(Parse(
        R"({"encoding":"json","entity":"events","max_batch_rows":1,"fields":[{"name":"value","path":"/v","type":"int64","nullable":false}]})"));
}
struct Fake final : IKafkaClientSessionV1, IKafkaConsumerSessionV1 {
    KafkaBindingSnapshotV1 snapshot;
    bool cancelled = false;
    int commits = 0, commit_error = 0, polls = 0;
    const KafkaBindingSnapshotV1& Snapshot() const override { return snapshot; }
    int Probe(uint32_t, std::string*) override { return ENOTSUP; }
    void Cancel() override { cancelled = true; }
    int QueryInterface(const Guid&, void** output) override {
        *output = static_cast<IKafkaConsumerSessionV1*>(this);
        return 0;
    }
    int StartConsumer(const char*, uint32_t) override { return 0; }
    int PollMessage(uint32_t, KafkaMessageV1* message) override {
        if (cancelled) return ECANCELED;
        if (polls++) return EAGAIN;
        message->value = R"({"v":42})";
        message->topic = "events";
        message->partition = 2;
        message->offset = 19;
        return 0;
    }
    int CommitOffsets(const std::vector<KafkaOffsetV1>& offsets, uint32_t) override {
        assert(offsets.size() == 1 && offsets[0].partition == 2 && offsets[0].next_offset == 20);
        ++commits;
        return commit_error;
    }
};
std::shared_ptr<Client> Session(const char* brokers, const std::string& group) {
    Binding binding;
    binding.connection = Parse(R"({"name":"mock","bootstrap_servers":[],"security_protocol":"PLAINTEXT"})");
    binding.connection["bootstrap_servers"].PushBack(rapidjson::Value(brokers, binding.connection.GetAllocator()),
                                                     binding.connection.GetAllocator());
    binding.snapshot.usage = "source";
    binding.snapshot.topic = "events";
    binding.snapshot.consumer_group = group;
    return std::make_shared<Client>(std::move(binding), Deployment{});
}
BlockPollEvent Read(StructuredReader& reader) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    BlockPollEvent event;
    do {
        event = reader.PollBlock(100);
    } while (event.kind == BlockPollEvent::kTimeout && std::chrono::steady_clock::now() < deadline);
    if (event.kind != BlockPollEvent::kData)
        std::cerr << "read failed: kind=" << event.kind << " err=" << event.err << '\n';
    assert(event.kind == BlockPollEvent::kData);
    return event;
}
int64_t Committed(const char* brokers, const char* group, int32_t partition) {
    auto* config = rd_kafka_conf_new();
    char error[512];
    assert(rd_kafka_conf_set(config, "bootstrap.servers", brokers, error, sizeof(error)) == RD_KAFKA_CONF_OK);
    assert(rd_kafka_conf_set(config, "group.id", group, error, sizeof(error)) == RD_KAFKA_CONF_OK);
    assert(rd_kafka_conf_set(config, "log_level", "0", error, sizeof(error)) == RD_KAFKA_CONF_OK);
    auto* client = rd_kafka_new(RD_KAFKA_CONSUMER, config, error, sizeof(error));
    assert(client);
    auto* offsets = rd_kafka_topic_partition_list_new(1);
    rd_kafka_topic_partition_list_add(offsets, "events", partition);
    assert(rd_kafka_committed(client, offsets, 3000) == 0);
    const auto result = offsets->elems[0].offset;
    rd_kafka_topic_partition_list_destroy(offsets);
    rd_kafka_destroy(client);
    return result;
}
}  // namespace
int main() {
    {
        std::array<unsigned char, 32> key{};
        const auto key_path = flowsql::test::WorkspacePath("reader.key");
        {
            std::ofstream file(key_path, std::ios::binary);
            file.write(reinterpret_cast<const char*>(key.data()), key.size());
        }
        auto options = Parse("{}");
        auto& allocator = options.GetAllocator();
        String(options, "db_path", flowsql::test::WorkspacePath("reader.db"), allocator);
        String(options, "master_key_file", key_path, allocator);
        String(options, "runtime_directory", flowsql::test::WorkspacePath("runtime"), allocator);
        Plugin plugin;
        assert(plugin.Option(Encode(options).c_str()) == 0 && plugin.Load(nullptr) == 0);
        plugin.Definitions().Mutate(
            "connections", "add",
            Parse(R"({"name":"offline","bootstrap_servers":["localhost:1"],"security_protocol":"PLAINTEXT"})"));
        auto channel = Parse(
            R"({"name":"metrics","connection_name":"offline","role":"source","topic":"events","consumer_group":"test","payload_kind":"structured","payload_options":{"source":{"encoding":"json","entity":"metrics","fields":[{"name":"v","path":"/v","type":"int64","nullable":false}]}}})");
        plugin.Definitions().Mutate("channels", "add", std::move(channel));
        auto* definition = plugin.Get("kafka", "metrics");
        assert(definition);
        assert(plugin.Get("packet", "metrics") == nullptr);
        BlockStreamReaderConfigV1 request;
        request.task_id = "test";
        request.source_category = "kafka";
        request.source_name = "metrics";
        request.pushed_filter_plan_json = R"({"version":1,"root":null})";
        IBlockStreamChannel* first = nullptr;
        IBlockStreamChannel* second = nullptr;
        assert(plugin.CreateReader(request, &first) == 0 && first && first != definition && first->IsOpened());
        assert(plugin.CreateReader(request, &second) == EBUSY && !second);
        assert(plugin.Definitions().Query("channels", "metrics")["items"][0]["in_use"].GetBool());
        plugin.ReleaseReader(first);
        assert(plugin.CreateReader(request, &second) == 0 && second);
        plugin.ReleaseReader(second);
        assert(!plugin.Definitions().Query("channels", "metrics")["items"][0]["in_use"].GetBool());
        assert(plugin.Unload() == 0);
    }
    for (bool confirm : {false, true}) {
        auto fake = std::make_shared<Fake>();
        StructuredReader reader("fake", Config(), fake);
        assert(reader.Open() == 0);
        auto event = reader.PollBlock(0);
        assert(event.kind == BlockPollEvent::kData);
        assert(reader.PollBlock(0).err == EBUSY);
        assert(reader.AcknowledgeBlock(nullptr) == EINVAL);
        if (confirm) {
            assert(reader.AcknowledgeBlock(event.batch) == 0);
            assert(reader.AcknowledgeBlock(event.batch) == EINVAL);
        }
        assert(reader.ReleaseBlock(event.batch) == 0 && fake->commits == (confirm ? 1 : 0));
        assert(reader.ReleaseBlock(event.batch) == EINVAL);
        reader.Close();
        assert(fake->commits == (confirm ? 1 : 0));
        assert(std::static_pointer_cast<arrow::Int64Array>(event.batch->column(0))->Value(0) == 42);
    }
    {
        auto fake = std::make_shared<Fake>();
        fake->commit_error = EIO;
        StructuredReader reader("failed", Config(), fake);
        assert(reader.Open() == 0);
        auto event = reader.PollBlock(0);
        assert(reader.AcknowledgeBlock(event.batch) == EIO && reader.IsFinished());
        reader.ReleaseBlock(event.batch);
    }
    {
        auto fake = std::make_shared<Fake>();
        auto config = Config();
        config.commit_policy = "none";
        StructuredReader reader("none", config, fake);
        assert(reader.Open() == 0);
        auto event = reader.PollBlock(0);
        assert(reader.AcknowledgeBlock(event.batch) == 0 && fake->commits == 0);
        reader.ReleaseBlock(event.batch);
    }
    // The real mock Broker needs loopback sockets; fail clearly before librdkafka initialization.
    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (listener < 0 || bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address))) {
        if (listener >= 0) close(listener);
        std::cerr << "execution_denied: loopback listener required for Kafka mock\n";
        return 1;
    }
    close(listener);
    auto* config = rd_kafka_conf_new();
    char error[512];
    rd_kafka_conf_set(config, "log_level", "0", error, sizeof(error));
    auto* producer = rd_kafka_new(RD_KAFKA_PRODUCER, config, error, sizeof(error));
    assert(producer);
    auto* cluster = rd_kafka_mock_cluster_new(producer, 1);
    assert(cluster);
    const char* brokers = rd_kafka_mock_cluster_bootstraps(cluster);
    assert(rd_kafka_mock_topic_create(cluster, "events", 2, 1) == 0);
    rd_kafka_brokers_add(producer, brokers);
    for (int p = 0; p < 2; ++p) {
        const char* payload = p ? R"({"v":2})" : R"({"v":1})";
        assert(rd_kafka_producev(producer, RD_KAFKA_V_TOPIC("events"), RD_KAFKA_V_PARTITION(p),
                                 RD_KAFKA_V_MSGFLAGS(RD_KAFKA_MSG_F_COPY),
                                 RD_KAFKA_V_VALUE(const_cast<char*>(payload), std::strlen(payload)),
                                 RD_KAFKA_V_KEY("key", 3), RD_KAFKA_V_END) == 0);
    }
    assert(rd_kafka_flush(producer, 5000) == 0);
    int32_t replay_partition = -1;
    {
        StructuredReader reader("replay", Config(), Session(brokers, "replay"));
        assert(reader.Open() == 0);
        auto event = Read(reader);
        replay_partition =
            std::static_pointer_cast<arrow::Int32Array>(event.batch->GetColumnByName("__kafka_partition"))->Value(0);
        assert(reader.ReleaseBlock(event.batch) == 0);  // Failure/resource release never commits.
    }
    assert(Committed(brokers, "replay", replay_partition) == RD_KAFKA_OFFSET_INVALID);
    {
        // A separate group independently replays the same messages. Same-group restart is
        // validated against real Kafka: mock classic groups deliberately delay the next election.
        StructuredReader reader("replay", Config(), Session(brokers, "confirmed"));
        assert(reader.Open() == 0);
        auto a = Read(reader);
        auto bpart =
            std::static_pointer_cast<arrow::Int32Array>(a.batch->GetColumnByName("__kafka_partition"))->Value(0);
        assert(std::static_pointer_cast<arrow::Int64Array>(a.batch->GetColumnByName("__kafka_offset"))->Value(0) == 0);
        assert(reader.AcknowledgeBlock(a.batch) == 0);
        reader.ReleaseBlock(a.batch);
        auto b = Read(reader);
        auto apart =
            std::static_pointer_cast<arrow::Int32Array>(b.batch->GetColumnByName("__kafka_partition"))->Value(0);
        assert(apart != bpart && (apart == replay_partition || bpart == replay_partition));
        assert(reader.AcknowledgeBlock(b.batch) == 0);
        reader.ReleaseBlock(b.batch);
    }
    assert(Committed(brokers, "confirmed", 0) == 1 && Committed(brokers, "confirmed", 1) == 1);
    {
        auto session = Session(brokers, "cancel");
        StructuredReader reader("committed", Config(), session);
        assert(reader.Open() == 0);
        assert(reader.PollBlock(1000).kind == BlockPollEvent::kTimeout);
        std::thread cancel([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            reader.Cancel();
        });
        const auto start = std::chrono::steady_clock::now();
        auto event = reader.PollBlock(30000);
        cancel.join();
        assert(event.kind == BlockPollEvent::kCancelled &&
               std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
    }
    {
        auto session = Session(brokers, "commit-error");
        StructuredReader reader("failed", Config(), session);
        assert(reader.Open() == 0);
        auto event = Read(reader);
        rd_kafka_mock_push_request_errors(cluster, 8, 1, RD_KAFKA_RESP_ERR_GROUP_AUTHORIZATION_FAILED);
        assert(reader.AcknowledgeBlock(event.batch) != 0 && reader.IsFinished());
        reader.ReleaseBlock(event.batch);
    }
    rd_kafka_mock_cluster_destroy(cluster);
    rd_kafka_destroy(producer);
    std::cout << "PASS: reader lifecycle, explicit confirmation, multiple partitions, mock commit/replay/error and "
                 "bounded cancellation\n";
}
