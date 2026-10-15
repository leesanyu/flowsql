// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <arrow/api.h>
#include <channels/kafka/kafka_client.h>
#include <channels/kafka/kafka_packet_reader.h>
#include <framework/core/packet_family_codec.h>
#include <librdkafka/rdkafka_mock.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cassert>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <iostream>
#include <thread>
using namespace flowsql;
using namespace flowsql::channels::kafka;
using Clock = std::chrono::steady_clock;
namespace {
PacketSourceConfig Config() {
    return ParsePacketSourceConfig(Parse(
        R"({"encoding":"flowsql_packet_v1","stream_id":"probe","observation_domain_id":"7","inputs":[{"source_id":0,"queue_id":2,"source_name":"a"},
      {"source_id":1,"queue_id":3,"source_name":"b"}]})"));
}
PacketEnvelope Envelope(uint64_t poll = 8, bool idle = false) {
    PacketEnvelope e;
    e.stream_epoch[0] = 1;
    e.observation_domain_id = 7;
    e.capture_generation = 45;
    e.poll_sequence = poll;
    if (!idle) e.packets.push_back({{100, 3, 64, 1, 0, poll * 2}, std::string("\0\xff\0", 3)});
    CaptureProgressV1 fact;
    fact.source_id = 0;
    fact.queue_id = 2;
    fact.fact_sequence = poll;
    fact.capture_time_ns = 100;
    fact.packet_observed = !idle;
    fact.source_idle_confirmed = idle;
    fact.backlog = CaptureBacklogV1::kEmpty;
    e.progress.push_back(fact);
    return e;
}
KafkaMessageV1 Message(const PacketEnvelope& e, int64_t offset) {
    auto m = EncodePacket(Config(), e).ValueOrDie();
    m.topic = "packets";
    m.offset = offset;
    return m;
}
struct Fake final : IKafkaClientSessionV1, IKafkaConsumerSessionV1 {
    KafkaBindingSnapshotV1 snapshot;
    std::deque<KafkaMessageV1> messages;
    std::atomic<bool> cancelled{false};
    int commits = 0, commit_error = 0, poll_error = 0;
    int64_t committed = -1;
    bool wait = false;
    std::mutex mutex;
    std::condition_variable wake;
    Fake() {
        snapshot.topic = "packets";
        snapshot.payload_kind = "packet";
    }
    const KafkaBindingSnapshotV1& Snapshot() const override { return snapshot; }
    int Probe(uint32_t, std::string*) override { return ENOTSUP; }
    int QueryInterface(const Guid& iid, void** p) override {
        assert(std::memcmp(&iid, &IID_KAFKA_CONSUMER_SESSION_V1, sizeof(Guid)) == 0);
        *p = static_cast<IKafkaConsumerSessionV1*>(this);
        return 0;
    }
    void Cancel() override {
        cancelled = true;
        wake.notify_all();
    }
    int StartConsumer(const char*, uint32_t) override { return 0; }
    int PollMessage(uint32_t timeout, KafkaMessageV1* m) override {
        std::unique_lock<std::mutex> lock(mutex);
        if (wait) wake.wait_for(lock, std::chrono::milliseconds(timeout), [&] { return cancelled.load(); });
        if (cancelled) return ECANCELED;
        if (poll_error) return poll_error;
        if (messages.empty()) return EAGAIN;
        *m = std::move(messages.front());
        messages.pop_front();
        return 0;
    }
    int CommitOffsets(const std::vector<KafkaOffsetV1>& offsets, uint32_t) override {
        assert(offsets.size() == 1 && offsets[0].partition == 0);
        committed = offsets[0].next_offset;
        ++commits;
        return commit_error;
    }
};
void FakeCases() {
    for (int mode = 0; mode < 5; ++mode) {
        auto fake = std::make_shared<Fake>();
        fake->messages.push_back(Message(Envelope(), 89));  // middle of a segment is a valid start.
        auto config = Config();
        if (mode == 2) config.commit_policy = "none";
        if (mode == 3) fake->commit_error = ETIMEDOUT;
        std::shared_ptr<arrow::RecordBatch> retained;
        {
            PacketReader reader("fake", config, fake, nullptr);
            CaptureSourceSetV2 sources;
            assert(reader.DescribeSources(&sources) == 0);
            assert(ValidateCaptureSourceSetV2(sources) == CaptureDescriptionErrorV1::kNone);
            assert(sources.inputs.size() == 2 && sources.limits.max_outstanding_batches == 1);
            assert(reader.Open() == 0);
            auto event = reader.PollCapture(0);
            retained = event.block.batch;
            assert(event.block.kind == BlockPollEvent::kData && retained->num_rows() == 1);
            assert(packet::ValidatePacketFamilyBatch(*retained).ok());
            assert(std::static_pointer_cast<arrow::UInt8Array>(retained->GetColumnByName("layer_status"))->Value(0) ==
                   static_cast<uint8_t>(packet::LayerStatus::kTruncated));
            assert(event.progress.size() == 1 && event.progress[0].generation == sources.inputs[0].generation);
            assert(event.progress[0].generation != 45 && event.progress[0].fact_sequence == 8);
            assert(reader.PollCapture(0).block.err == EBUSY && reader.AcknowledgeBlock(nullptr) == EINVAL);
            if (mode == 4) reader.Cancel();
            if (mode == 0 || mode == 2) {
                assert(reader.AcknowledgeBlock(retained) == 0);
                assert(reader.AcknowledgeBlock(retained) == EINVAL);
            } else if (mode == 3) {
                assert(reader.AcknowledgeBlock(retained) == ETIMEDOUT && reader.IsFinished());
                assert(reader.Diagnostics().find("commit_uncertain=1") != std::string::npos);
                assert(reader.AcknowledgeBlock(retained) == ETIMEDOUT && fake->commits == 1);
            } else if (mode == 4)
                assert(reader.AcknowledgeBlock(retained) == ECANCELED);
            assert(reader.ReleaseBlock(retained) == 0 && reader.ReleaseBlock(retained) == EINVAL);
            CaptureCountersV1 counters;
            assert(reader.ReadInputCounters(0, &counters) == 0 && counters.delivered_packets == 1);
            assert(!(counters.available_mask & kCaptureSourceDroppedPacketsAvailable));
            assert(fake->commits == ((mode == 0 || mode == 3) ? 1 : 0));
            if (mode == 0) assert(fake->committed == 90);
        }
        assert(std::static_pointer_cast<arrow::BinaryArray>(retained->GetColumnByName("raw_data"))->GetString(0) ==
               std::string("\0\xff\0", 3));
        assert(retained->ValidateFull().ok());
    }
    for (int mode = 0; mode < 9; ++mode) {
        auto fake = std::make_shared<Fake>();
        auto next = Envelope(9);
        if (mode == 0) next.stream_epoch[1] = 2;
        if (mode == 1) ++next.capture_generation;
        if (mode == 2) ++next.poll_sequence;
        if (mode == 3) next.poll_sequence = 8;
        if (mode == 4) next.packets[0].meta.sequence = 16;
        if (mode == 5) next.progress[0].fact_sequence = 8;
        fake->messages.push_back(Message(Envelope(), 89));
        auto second = Message(next, 90);
        if (mode == 6) second.partition = 1;
        if (mode == 7) second.offset = 89;
        if (mode == 8) second.value += 'x';
        fake->messages.push_back(second);
        PacketReader reader("order", Config(), fake, nullptr);
        assert(reader.Open() == 0);
        auto first = reader.PollCapture(0);
        assert(reader.AcknowledgeBlock(first.block.batch) == 0 && reader.ReleaseBlock(first.block.batch) == 0);
        assert(reader.PollCapture(0).block.kind == BlockPollEvent::kError && reader.IsFinished());
        assert(fake->commits == 1);
    }
    {
        auto fake = std::make_shared<Fake>();
        PacketReader reader("idle", Config(), fake, nullptr);
        assert(reader.Open() == 0);
        auto timeout = reader.PollCapture(0);
        assert(timeout.block.kind == BlockPollEvent::kTimeout && timeout.progress.empty());
        fake->messages.push_back(Message(Envelope(88, true), 5));
        auto idle = reader.PollCapture(0);
        assert(idle.block.kind == BlockPollEvent::kData && idle.block.batch->num_rows() == 0);
        assert(packet::ValidatePacketFamilyBatch(*idle.block.batch).ok() && idle.progress[0].source_idle_confirmed);
        assert(reader.AcknowledgeBlock(idle.block.batch) == 0 && fake->committed == 6);
        assert(reader.ReleaseBlock(idle.block.batch) == 0);
    }
    {
        auto fake = std::make_shared<Fake>();
        fake->wait = true;
        PacketReader reader("cancel", Config(), fake, nullptr);
        assert(reader.Open() == 0);
        std::thread cancel([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            reader.Cancel();
        });
        auto start = Clock::now();
        assert(reader.PollCapture(30000).block.kind == BlockPollEvent::kCancelled);
        cancel.join();
        assert(Clock::now() - start < std::chrono::seconds(1) && fake->commits == 0);
    }
}
std::shared_ptr<Client> Session(const char* brokers, const std::string& group, const char* topic) {
    Binding b;
    b.connection = Parse(R"({"name":"mock","bootstrap_servers":[],"security_protocol":"PLAINTEXT"})");
    b.connection["bootstrap_servers"].PushBack(rapidjson::Value(brokers, b.connection.GetAllocator()),
                                               b.connection.GetAllocator());
    b.snapshot.usage = "source";
    b.snapshot.topic = topic;
    b.snapshot.consumer_group = group;
    b.snapshot.payload_kind = "packet";
    return std::make_shared<Client>(std::move(b), Deployment{});
}
void MockCases() {
    char error[512];
    auto* conf = rd_kafka_conf_new();
    rd_kafka_conf_set(conf, "log_level", "0", error, sizeof(error));
    auto* producer = rd_kafka_new(RD_KAFKA_PRODUCER, conf, error, sizeof(error));
    assert(producer);
    auto* cluster = rd_kafka_mock_cluster_new(producer, 1);
    assert(cluster);
    const auto* brokers = rd_kafka_mock_cluster_bootstraps(cluster);
    assert(!rd_kafka_mock_topic_create(cluster, "packets", 1, 1));
    assert(!rd_kafka_mock_topic_create(cluster, "invalid", 2, 1));
    rd_kafka_brokers_add(producer, brokers);
    auto m = Message(Envelope(), 0);
    assert(!rd_kafka_producev(producer, RD_KAFKA_V_TOPIC("packets"), RD_KAFKA_V_PARTITION(0),
                              RD_KAFKA_V_MSGFLAGS(RD_KAFKA_MSG_F_COPY), RD_KAFKA_V_KEY(m.key.data(), m.key.size()),
                              RD_KAFKA_V_VALUE(m.value.data(), m.value.size()), RD_KAFKA_V_END));
    assert(!rd_kafka_flush(producer, 5000));
    {
        rd_kafka_mock_topic_set_error(cluster, "packets", RD_KAFKA_RESP_ERR_LEADER_NOT_AVAILABLE);
        std::thread recovered([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            rd_kafka_mock_topic_set_error(cluster, "packets", RD_KAFKA_RESP_ERR_NO_ERROR);
        });
        PacketReader reader("mock", Config(), Session(brokers, "packet-valid", "packets"), nullptr);
        const auto opened_at = Clock::now();
        const auto opened = reader.Open();
        recovered.join();
        assert(opened == 0 && Clock::now() - opened_at < std::chrono::seconds(4));
        CapturePollEventV2 event;
        const auto deadline = Clock::now() + std::chrono::seconds(15);
        do {
            event = reader.PollCapture(100);
        } while (event.block.kind == BlockPollEvent::kTimeout && Clock::now() < deadline);
        assert(event.block.kind == BlockPollEvent::kData);
        assert(reader.AcknowledgeBlock(event.block.batch) == 0);
        assert(reader.ReleaseBlock(event.block.batch) == 0);
        assert(reader.PollCapture(10).block.kind == BlockPollEvent::kTimeout);
    }
    {
        PacketReader reader("invalid", Config(), Session(brokers, "packet-invalid", "invalid"), nullptr);
        assert(reader.Open() == EPROTO);
    }
    {
        auto config = Config();
        config.max_message_bytes = 1024;
        PacketReader reader("small", config, Session(brokers, "packet-small", "packets"), nullptr);
        assert(reader.Open() == 0);
    }
    {
        PacketReader first("ownership", Config(), Session(brokers, "packet-owner", "packets"), nullptr);
        assert(first.Open() == 0);
        auto deadline = Clock::now() + std::chrono::seconds(15);
        CapturePollEventV2 data;
        do {
            data = first.PollCapture(100);
        } while (data.block.kind == BlockPollEvent::kTimeout && Clock::now() < deadline);
        assert(data.block.kind == BlockPollEvent::kData);
        assert(first.AcknowledgeBlock(data.block.batch) == 0 && first.ReleaseBlock(data.block.batch) == 0);
        auto second = Session(brokers, "packet-owner", "packets");
        assert(second->StartConsumer("earliest", Config().max_message_bytes) == 0);
        deadline = Clock::now() + std::chrono::seconds(20);
        BlockPollEvent event;
        do {
            KafkaMessageV1 message;
            second->PollMessage(100, &message);
            event = first.PollBlock(100);
        } while (event.kind == BlockPollEvent::kTimeout && Clock::now() < deadline);
        assert(event.kind == BlockPollEvent::kError && event.err == ESTALE);
    }
    rd_kafka_mock_cluster_destroy(cluster);
    rd_kafka_destroy(producer);
}
}  // namespace
int main() {
    FakeCases();
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
    MockCases();
    std::cout << "PASS packet Capture V2, middle/replay/order/epoch, explicit commit/none/uncertain, cancellation, "
                 "mock topology\n";
}
