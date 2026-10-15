// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <channels/kafka/kafka_packet_reader.h>
#include <framework/core/packet_family_codec.h>
#include <framework/interfaces/iblock_stream_acknowledgement.h>
#include <framework/interfaces/iblock_stream_reader.h>
#include <librdkafka/rdkafka.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <random>
#include "packet_test_runtime.h"
using namespace packet_test;
using Clock = std::chrono::steady_clock;
namespace {
Json ReadJson(const std::string& path) {
    std::ifstream input(path);
    Check(bool(input), "private_plan_missing");
    return Parse(std::string((std::istreambuf_iterator<char>(input)), {}));
}
void Set(rd_kafka_conf_t* conf, const char* key, const std::string& value) {
    char error[512];
    Check(rd_kafka_conf_set(conf, key, value.c_str(), error, sizeof(error)) == RD_KAFKA_CONF_OK, "fixture_config");
}
class Broker {
 public:
    static void Delivery(rd_kafka_t*, const rd_kafka_message_t* message, void*) {
        *static_cast<rd_kafka_resp_err_t*>(message->_private) = message->err;
    }
    explicit Broker(const rapidjson::Value& plaintext) {
        auto* conf = rd_kafka_conf_new();
        servers_ = plaintext["connection"]["bootstrap_servers"][0].GetString();
        Set(conf, "bootstrap.servers", servers_);
        Set(conf, "log_level", "0");
        Set(conf, "message.timeout.ms", "5000");
        Set(conf, "allow.auto.create.topics", "false");
        rd_kafka_conf_set_dr_msg_cb(conf, Delivery);
        char error[512];
        producer_ = rd_kafka_new(RD_KAFKA_PRODUCER, conf, error, sizeof(error));
        Check(producer_, "fixture_producer");
    }
    ~Broker() {
        for (const auto& [type, name] : acls_) {
            char error[512];
            auto* filter = rd_kafka_AclBindingFilter_new(type, name.c_str(), RD_KAFKA_RESOURCE_PATTERN_LITERAL, nullptr,
                                                         nullptr, RD_KAFKA_ACL_OPERATION_ANY,
                                                         RD_KAFKA_ACL_PERMISSION_TYPE_ANY, error, sizeof(error));
            if (filter) {
                auto* queue = rd_kafka_queue_new(producer_);
                rd_kafka_DeleteAcls(producer_, &filter, 1, nullptr, queue);
                auto* event = rd_kafka_queue_poll(queue, 5000);
                if (event) rd_kafka_event_destroy(event);
                rd_kafka_queue_destroy(queue);
                rd_kafka_AclBinding_destroy(filter);
            }
        }
        for (const auto& topic : topics_) {
            auto* item = rd_kafka_DeleteTopic_new(topic.c_str());
            auto* queue = rd_kafka_queue_new(producer_);
            rd_kafka_DeleteTopics(producer_, &item, 1, nullptr, queue);
            auto* event = rd_kafka_queue_poll(queue, 5000);
            if (event) rd_kafka_event_destroy(event);
            rd_kafka_queue_destroy(queue);
            rd_kafka_DeleteTopic_destroy(item);
        }
        rd_kafka_destroy(producer_);
    }
    void Topic(const std::string& name, int partitions = 1) {
        char error[512];
        auto* item = rd_kafka_NewTopic_new(name.c_str(), partitions, 1, error, sizeof(error));
        Check(item, "fixture_topic_config");
        Check(!rd_kafka_NewTopic_set_config(item, "cleanup.policy", "delete"), "fixture_retention");
        auto* queue = rd_kafka_queue_new(producer_);
        rd_kafka_CreateTopics(producer_, &item, 1, nullptr, queue);
        auto* event = rd_kafka_queue_poll(queue, 10000);
        bool ok = event && !rd_kafka_event_error(event);
        if (ok) {
            size_t count;
            auto** results = rd_kafka_CreateTopics_result_topics(rd_kafka_event_CreateTopics_result(event), &count);
            ok = count == 1 && !rd_kafka_topic_result_error(results[0]);
        }
        if (event) rd_kafka_event_destroy(event);
        rd_kafka_queue_destroy(queue);
        rd_kafka_NewTopic_destroy(item);
        Check(ok, "fixture_create_topic");
        topics_.push_back(name);
    }
    void Produce(const std::string& topic, const KafkaMessageV1& message) {
        delivery_ = RD_KAFKA_RESP_ERR__IN_PROGRESS;
        Check(rd_kafka_producev(
                  producer_, RD_KAFKA_V_TOPIC(topic.c_str()), RD_KAFKA_V_PARTITION(0),
                  RD_KAFKA_V_MSGFLAGS(RD_KAFKA_MSG_F_COPY),
                  RD_KAFKA_V_VALUE(message.value_is_null ? nullptr : const_cast<char*>(message.value.data()),
                                   message.value_is_null ? 0 : message.value.size()),
                  RD_KAFKA_V_KEY(message.key.data(), message.key.size()), RD_KAFKA_V_OPAQUE(&delivery_),
                  RD_KAFKA_V_END) == 0,
              "fixture_produce");
        const int flush = rd_kafka_flush(producer_, 6000);
        if (flush != 0 || delivery_ != RD_KAFKA_RESP_ERR_NO_ERROR) {
            std::cerr << "fixture_delivery_error=" << rd_kafka_err2name(delivery_) << '\n';
            rd_kafka_purge(producer_, RD_KAFKA_PURGE_F_QUEUE | RD_KAFKA_PURGE_F_INFLIGHT);
            rd_kafka_poll(producer_, 0);
            Check(false, "fixture_delivery");
        }
    }
    int64_t Committed(const std::string& topic, const std::string& group, int partition) {
        auto* conf = rd_kafka_conf_new();
        Set(conf, "bootstrap.servers", servers_);
        Set(conf, "group.id", group);
        Set(conf, "log_level", "0");
        char error[512];
        auto* consumer = rd_kafka_new(RD_KAFKA_CONSUMER, conf, error, sizeof(error));
        Check(consumer, "fixture_offset_client");
        auto* list = rd_kafka_topic_partition_list_new(1);
        rd_kafka_topic_partition_list_add(list, topic.c_str(), partition);
        int rc = rd_kafka_committed(consumer, list, 5000);
        int64_t offset = list->elems[0].offset;
        rd_kafka_topic_partition_list_destroy(list);
        rd_kafka_destroy(consumer);
        Check(!rc, "fixture_query_offsets");
        return offset;
    }
    void DenyRead(const std::string& topic, const std::string& username,
                  rd_kafka_ResourceType_t resource = RD_KAFKA_RESOURCE_TOPIC) {
        char error[512];
        const auto principal = "User:" + username;
        std::vector<rd_kafka_AclBinding_t*> acl;
        acl.push_back(rd_kafka_AclBinding_new(resource, topic.c_str(), RD_KAFKA_RESOURCE_PATTERN_LITERAL, "User:*", "*",
                                              RD_KAFKA_ACL_OPERATION_DESCRIBE, RD_KAFKA_ACL_PERMISSION_TYPE_ALLOW,
                                              error, sizeof(error)));
        acl.push_back(rd_kafka_AclBinding_new(resource, topic.c_str(), RD_KAFKA_RESOURCE_PATTERN_LITERAL,
                                              principal.c_str(), "*", RD_KAFKA_ACL_OPERATION_READ,
                                              RD_KAFKA_ACL_PERMISSION_TYPE_DENY, error, sizeof(error)));
        for (auto* a : acl) Check(a, "fixture_acl_binding");
        auto* q = rd_kafka_queue_new(producer_);
        rd_kafka_CreateAcls(producer_, acl.data(), acl.size(), nullptr, q);
        auto* e = rd_kafka_queue_poll(q, 10000);
        bool ok = e && !rd_kafka_event_error(e);
        if (ok) {
            size_t count = 0;
            auto** r = rd_kafka_CreateAcls_result_acls(rd_kafka_event_CreateAcls_result(e), &count);
            ok = count == acl.size();
            for (size_t i = 0; i < count; ++i) ok = ok && !rd_kafka_acl_result_error(r[i]);
        }
        if (e) rd_kafka_event_destroy(e);
        rd_kafka_queue_destroy(q);
        for (auto* a : acl) rd_kafka_AclBinding_destroy(a);
        Check(ok, "fixture_create_acl");
        acls_.push_back({resource, topic});
    }
    void Increase(const std::string& topic) {
        char error[512];
        auto* item = rd_kafka_NewPartitions_new(topic.c_str(), 2, error, sizeof(error));
        Check(item, "fixture_partitions_config");
        auto* q = rd_kafka_queue_new(producer_);
        rd_kafka_CreatePartitions(producer_, &item, 1, nullptr, q);
        auto* e = rd_kafka_queue_poll(q, 10000);
        bool ok = e && !rd_kafka_event_error(e);
        if (ok) {
            size_t count = 0;
            auto** results = rd_kafka_CreatePartitions_result_topics(rd_kafka_event_CreatePartitions_result(e), &count);
            ok = count == 1 && !rd_kafka_topic_result_error(results[0]);
        }
        if (e) rd_kafka_event_destroy(e);
        rd_kafka_queue_destroy(q);
        rd_kafka_NewPartitions_destroy(item);
        Check(ok, "fixture_increase_partitions");
    }
    int64_t High(const std::string& topic) {
        int64_t low = 0, high = 0;
        Check(!rd_kafka_query_watermark_offsets(producer_, topic.c_str(), 0, &low, &high, 3000), "fixture_watermark");
        return high;
    }
    bool Exists(const std::string& name) {
        const rd_kafka_metadata_t* metadata = nullptr;
        Check(rd_kafka_metadata(producer_, 1, nullptr, &metadata, 5000) == 0, "fixture_metadata");
        bool found = false;
        for (int i = 0; i < metadata->topic_cnt; ++i) found |= name == metadata->topics[i].topic;
        rd_kafka_metadata_destroy(metadata);
        return found;
    }

 private:
    rd_kafka_t* producer_ = nullptr;
    rd_kafka_resp_err_t delivery_ = RD_KAFKA_RESP_ERR__IN_PROGRESS;
    std::string servers_;
    std::vector<std::string> topics_;
    std::vector<std::pair<rd_kafka_ResourceType_t, std::string>> acls_;
};
PacketSourceConfig Config() {
    return ParsePacketSourceConfig(Parse(
        R"({"encoding":"flowsql_packet_v1","stream_id":"probe","observation_domain_id":"7","inputs":[{"source_id":0,"queue_id":0,"source_name":"eth0"}]})"));
}
PacketEnvelope Envelope(uint64_t poll, size_t packets = 1, uint8_t epoch = 1, int64_t timestamp = 100000000000LL) {
    PacketEnvelope e;
    e.stream_epoch[0] = epoch;
    e.observation_domain_id = 7;
    e.capture_generation = 9;
    e.poll_sequence = poll;
    // Independent Ethernet/IPv4/UDP frame; no remote layer cache or NPI objects are published.
    std::string raw(96, '\0');
    raw[0] = 1;
    raw[6] = 2;
    raw[12] = 8;
    raw[14] = '\x45';
    raw[17] = 82;
    raw[22] = 64;
    raw[23] = 17;
    raw[26] = raw[30] = 10;
    raw[29] = 1;
    raw[33] = 2;
    raw[34] = '\xc0';
    raw[35] = 1;
    raw[37] = 9;
    raw[39] = 62;
    for (size_t i = 0; i < packets; ++i)
        e.packets.push_back({{timestamp, 96, 96, 1, 0, (poll - 1) * 256 + i + 1}, raw});
    CaptureProgressV1 f;
    f.fact_sequence = poll;
    f.capture_time_ns = timestamp;
    f.packet_observed = packets != 0;
    f.source_idle_confirmed = packets == 0;
    f.backlog = CaptureBacklogV1::kEmpty;
    e.progress.push_back(f);
    return e;
}
KafkaMessageV1 Message(uint64_t poll, size_t packets = 1, uint8_t epoch = 1, int64_t timestamp = 100000000000LL) {
    return EncodePacket(Config(), Envelope(poll, packets, epoch, timestamp)).ValueOrDie();
}
std::string KafkaOptions(const rapidjson::Value& profile, const std::filesystem::path& root) {
    std::filesystem::create_directories(root);
    chmod(root.c_str(), 0700);
    if (!std::filesystem::exists(root / "key")) {
        std::random_device random;
        std::ofstream key(root / "key", std::ios::binary);
        for (int i = 0; i < 32; ++i) key.put(static_cast<char>(random()));
        key.close();
        chmod((root / "key").c_str(), 0600);
    }
    auto options = Parse("{}");
    auto& a = options.GetAllocator();
    String(options, "db_path", (root / "kafka.db").string(), a);
    String(options, "master_key_file", (root / "key").string(), a);
    String(options, "runtime_directory", (root / "runtime").string(), a);
    options.AddMember("files", rapidjson::Value(profile["files"], a), a);
    if (Text(profile["connection"], "sasl_mechanism") == "GSSAPI") {
        const auto path = std::filesystem::path(Text(profile["files"], "keytab")).parent_path() / "krb5.conf";
        setenv("KRB5_CONFIG", path.c_str(), 1);
    }
    return Encode(options);
}
void Connection(Runtime& runtime, const rapidjson::Value& profile) {
    runtime.Call("/channels/kafka/connections/add", profile["connection"]);
}
void Add(Runtime& runtime, const std::string& name, const std::string& topic, const std::string& group,
         const char* policy = "after_success", uint32_t limit = 2097152) {
    auto request = Parse(
        R"({"name":"packets","connection_name":"real","role":"source","topic":"packets","consumer_group":"npm","payload_kind":"packet","payload_options":{"source":{"encoding":"flowsql_packet_v1","stream_id":"probe","observation_domain_id":"7","inputs":[{"source_id":0,"queue_id":0,"source_name":"eth0"}]}}})");
    auto& a = request.GetAllocator();
    String(request, "name", name, a);
    String(request, "topic", topic, a);
    String(request, "consumer_group", group, a);
    String(request["payload_options"]["source"], "commit_policy", policy, a);
    request["payload_options"]["source"].AddMember("max_message_bytes", limit, a);
    runtime.Call("/channels/kafka/channels/add", request);
}
class Reader {
 public:
    explicit Reader(Runtime& runtime, const char* name, bool require = true) {
        factory_ = static_cast<IBlockStreamReaderFactoryV1*>(runtime.loader->First(IID_BLOCK_STREAM_READER_FACTORY_V1));
        Check(factory_, "reader_factory");
        BlockStreamReaderConfigV1 c;
        c.source_category = "kafka";
        c.source_name = name;
        c.task_id = "packet-real";
        c.pushed_filter_plan_json = R"({"version":1,"root":null})";
        opened = factory_->CreateReader(c, &reader_);
        if (require && opened) std::cerr << "reader_open_error=" << opened << " name=" << name << '\n';
        if (require) Check(!opened && reader_, "create_packet_reader");
    }
    ~Reader() {
        if (reader_) factory_->ReleaseReader(reader_);
    }
    IBlockStreamChannel* operator->() { return reader_; }
    BlockPollEvent Next(int seconds = 15) {
        const auto deadline = Clock::now() + std::chrono::seconds(seconds);
        BlockPollEvent e;
        do {
            e = reader_->PollBlock(100);
        } while (e.kind == BlockPollEvent::kTimeout && Clock::now() < deadline);
        return e;
    }
    void Ack(const BlockPollEvent& e) {
        auto* a = dynamic_cast<IBlockStreamAcknowledgementV1*>(reader_);
        Check(a && !a->AcknowledgeBlock(e.batch), "packet_ack");
        Check(!reader_->ReleaseBlock(e.batch), "packet_release");
    }
    int opened = 0;

 private:
    IBlockStreamReaderFactoryV1* factory_ = nullptr;
    IBlockStreamChannel* reader_ = nullptr;
};
void Data(const BlockPollEvent& e, int rows = 1) {
    if (e.kind != BlockPollEvent::kData) std::cerr << "poll_kind=" << e.kind << " error=" << e.err << '\n';
    Check(e.kind == BlockPollEvent::kData && e.batch && e.batch->num_rows() == rows, "packet_data");
    Check(packet::ValidatePacketFamilyBatch(*e.batch).ok(), "packet_family_schema");
    if (rows) {
        Check(std::static_pointer_cast<arrow::UInt64Array>(e.batch->GetColumnByName("sequence"))->Value(0) != 0,
              "packet_sequence");
        Check(std::static_pointer_cast<arrow::BinaryArray>(e.batch->GetColumnByName("raw_data"))->GetString(0).size() ==
                  96,
              "raw_bytes_preserved");
    }
}
void RestartChild(const Json& plan, int generation) {
    const auto profiles = ReadJson(Text(plan, "manifest"));
    const auto root = std::filesystem::path(Text(plan, "workspace"));
    const auto& profile = profiles[Text(plan, "profile").c_str()];
    Runtime runtime(root / ("analysis-" + std::to_string(generation)),
                    KafkaOptions(profile, root / "analysis-provider"));
    if (generation == 1) {
        Connection(runtime, profile);
        Add(runtime, "analysis", Text(plan, "topic") + "_npm", Text(plan, "group") + "_npm");
    }
    const auto id = "packet-restart-" + std::to_string(generation);
    runtime.Submit(id,
                   "SELECT * FROM kafka.analysis USING npm.basic WITH features='basic,session' INTO sqlite.results");
    const auto deadline = Clock::now() + std::chrono::seconds(20);
    // Parent owns the topic and observes the commit before allowing stop, via a private ready marker.
    std::ofstream(root / ("ready-" + std::to_string(generation))) << "ready";
    while (!std::filesystem::exists(root / ("stop-" + std::to_string(generation)))) {
        Check(Clock::now() < deadline, "npm_parent_timeout");
        Check(Text(runtime.Status(id), "status") != "failed", "npm_before_commit_failed");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    runtime.Status(id, "stop");
    const auto result = runtime.Terminal(id);
    Check(Text(result, "status") == "stopped", "npm_restart_terminal");
    const auto run = Text(result["managed_result"], "run_id");
    auto total = runtime.Query("SELECT MAX(packets_ab+packets_ba) FROM npm_basic_ipv4_final_v1");
    Check(std::stoll(total.front()->column(0)->GetScalar(0).ValueOrDie()->ToString()) == 1, "old_session_state_reused");
    std::ofstream(root / ("run-" + std::to_string(generation))) << run;
}
void Restart(const Json& plan, Broker& broker, const std::string& executable, const std::string& plan_path) {
    const auto topic = Text(plan, "topic") + "_npm", group = Text(plan, "group") + "_npm";
    const auto root = std::filesystem::path(Text(plan, "workspace"));
    broker.Topic(topic);
    for (int generation : {1, 2}) {
        broker.Produce(topic, Message(generation));
        const auto number = std::to_string(generation);
        const auto pid = fork();
        Check(pid >= 0, "restart_fork");
        if (!pid) {
            execl(executable.c_str(), executable.c_str(), plan_path.c_str(), number.c_str(), nullptr);
            _exit(127);
        }
        bool stopped = false;
        int status = 0;
        const auto deadline = Clock::now() + std::chrono::seconds(30);
        while (Clock::now() < deadline) {
            if (waitpid(pid, &status, WNOHANG) == pid) {
                stopped = true;
                break;
            }
            if (std::filesystem::exists(root / ("ready-" + number)) && broker.Committed(topic, group, 0) == generation)
                std::ofstream(root / ("stop-" + number)) << "stop";
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (!stopped) {
            kill(pid, SIGTERM);
            waitpid(pid, &status, 0);
        }
        Check(stopped && WIFEXITED(status) && !WEXITSTATUS(status), "restart_child_failed");
    }
    std::ifstream one(root / "run-1"), two(root / "run-2");
    std::string a, b;
    one >> a;
    two >> b;
    Check(!a.empty() && !b.empty() && a != b, "restart_run_reused");
    std::cout << "PASS real process restart, persisted packet configuration, committed continuation, new NPM run/empty "
                 "session\n";
}
void Performance(Runtime& runtime, Broker& broker, const std::string& topic, const std::string& group) {
    broker.Topic(topic);
    for (int i = 1; i <= 64; ++i) {
        const auto utc =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();
        broker.Produce(topic, Message(i, 256, 1, utc));
    }
    Add(runtime, "performance", topic, group);
    rusage before{}, after{};
    getrusage(RUSAGE_SELF, &before);
    const auto started = Clock::now();
    std::vector<double> latency;
    Reader reader(runtime, "performance");
    const auto max_lag = broker.High(topic);
    uint64_t raw_bytes = 0;
    for (int i = 0; i < 64; ++i) {
        auto e = reader.Next();
        Data(e, 256);
        const auto capture = std::static_pointer_cast<arrow::Int64Array>(e.batch->column(0))->Value(0);
        const auto utc =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();
        latency.push_back((utc - capture) / 1e6);
        raw_bytes += e.batch->num_rows() * 96;
        reader.Ack(e);
    }
    const auto wall = std::chrono::duration<double>(Clock::now() - started).count();
    const auto remaining = broker.High(topic) - broker.Committed(topic, group, 0);
    Check(max_lag == 64 && remaining == 0, "performance_lag");
    getrusage(RUSAGE_SELF, &after);
    const auto cpu = [](const rusage& v) {
        return v.ru_utime.tv_sec + v.ru_stime.tv_sec + (v.ru_utime.tv_usec + v.ru_stime.tv_usec) / 1e6;
    };
    const auto cancel_at = Clock::now();
    reader->Cancel();
    Check(reader->PollBlock(30000).kind == BlockPollEvent::kCancelled, "performance_cancel");
    const auto cancel_ms = std::chrono::duration<double, std::milli>(Clock::now() - cancel_at).count();
    Check(cancel_ms < 1000, "cancel_budget");
    std::sort(latency.begin(), latency.end());
    std::cout << "METRIC {\"messages\":64,\"packets\":16384,\"frame_bytes\":96,\"batch_packets\":256,\"wall_seconds\":"
              << wall << ",\"packet_per_second\":" << 16384 / wall << ",\"message_per_second\":" << 64 / wall
              << ",\"raw_bytes_per_second\":" << raw_bytes / wall
              << ",\"wire_bytes_per_second\":" << (64 * (56 + 256 * (32 + 96) + 28 + 5)) / wall
              << ",\"initial_lag\":" << max_lag << ",\"final_lag\":" << remaining
              << ",\"cpu_seconds\":" << cpu(after) - cpu(before) << ",\"max_rss_kib\":" << after.ru_maxrss
              << ",\"preloaded_delivery_p50_ms\":" << latency[32] << ",\"preloaded_delivery_p95_ms\":" << latency[60]
              << ",\"cancel_ms\":" << cancel_ms << "}\n";
}
void Run(const Json& plan, const std::string& executable, const std::string& plan_path) {
    const auto profiles = ReadJson(Text(plan, "manifest"));
    const auto label = Text(plan, "profile"), topic = Text(plan, "topic"), group = Text(plan, "group");
    const auto root = std::filesystem::path(Text(plan, "workspace"));
    Broker broker(profiles["PLAINTEXT"]);
    broker.Topic(topic);
    broker.Produce(topic, Message(1));
    broker.Produce(topic, Message(2, 0));
    {
        Runtime runtime(root / "reader", KafkaOptions(profiles[label.c_str()], root / "provider"));
        Connection(runtime, profiles[label.c_str()]);
        Add(runtime, "packets", topic, group);
        {
            Reader reader(runtime, "packets");
            auto e = reader.Next();
            Data(e);
            Check(!reader->ReleaseBlock(e.batch), "unconfirmed_release");
        }
        Check(broker.Committed(topic, group, 0) == RD_KAFKA_OFFSET_INVALID, "implicit_commit");
        {
            Reader reader(runtime, "packets");
            auto e = reader.Next();
            Data(e);
            reader.Ack(e);
            auto idle = reader.Next();
            Data(idle, 0);
            reader.Ack(idle);
            Check(broker.Committed(topic, group, 0) == 2, "confirmed_offsets");
        }
        {
            Reader reader(runtime, "packets");
            broker.Produce(topic, Message(3));
            auto e = reader.Next();
            Data(e);
            reader.Ack(e);
            Check(broker.Committed(topic, group, 0) == 3, "continued_offset");
            if (label == "GSSAPI" || label == "OAUTHBEARER") {
                const auto until = Clock::now() + std::chrono::seconds(label == "GSSAPI" ? 70 : 35);
                while (Clock::now() < until)
                    Check(reader->PollBlock(100).kind == BlockPollEvent::kTimeout, "refresh_idle");
                broker.Produce(topic, Message(4));
                auto refreshed = reader.Next();
                Data(refreshed);
                reader.Ack(refreshed);
                Check(broker.Committed(topic, group, 0) == 4, "post_refresh_packet_commit");
            }
            const auto before = Clock::now();
            reader->Cancel();
            Check(reader->PollBlock(30000).kind == BlockPollEvent::kCancelled &&
                      Clock::now() - before < std::chrono::seconds(1),
                  "cancel_wakeup");
        }
        if (label == "PLAIN") {
            const auto denied = topic + "_denied";
            broker.Topic(denied);
            broker.Produce(denied, Message(1));
            const auto username = Text(profiles[label.c_str()]["connection"]["sasl"], "username");
            Check(!username.empty(), "acl_identity_missing");
            broker.DenyRead(denied, username);
            Add(runtime, "denied", denied, group + "_denied");
            Reader reader(runtime, "denied");
            const auto e = reader.Next();
            Check(e.kind == BlockPollEvent::kError, "read_acl_must_fail");
            Check(broker.Committed(denied, group + "_denied", 0) == RD_KAFKA_OFFSET_INVALID, "denied_commit");
            std::cout << "PASS real topic READ ACL denial, metadata allowed, no commit\n";
            const auto commit = topic + "_commit", commit_group = group + "_commit";
            broker.Topic(commit);
            broker.Produce(commit, Message(1));
            Add(runtime, "commit_failure", commit, commit_group);
            {
                Reader failed(runtime, "commit_failure");
                const auto block = failed.Next();
                Data(block);
                broker.DenyRead(commit_group, username, RD_KAFKA_RESOURCE_GROUP);
                auto* acknowledgement = dynamic_cast<IBlockStreamAcknowledgementV1*>(failed.operator->());
                Check(acknowledgement && acknowledgement->AcknowledgeBlock(block.batch) != 0, "commit_acl_accepted");
                auto* packet_reader = dynamic_cast<PacketReader*>(failed.operator->());
                Check(packet_reader && packet_reader->Diagnostics().find("commit_uncertain=1") != std::string::npos,
                      "commit_failure_not_terminal_uncertain");
            }
            Check(broker.Committed(commit, commit_group, 0) == RD_KAFKA_OFFSET_INVALID, "commit_failure_offset");
            std::cout << "PASS real commit failure, terminal uncertain diagnostic, no implicit retry\n";
        }
        if (label == "PLAINTEXT") {
            Add(runtime, "none", topic, group + "_none", "none");
            for (int i = 0; i < 2; ++i) {
                Reader reader(runtime, "none");
                auto e = reader.Next();
                Data(e);
                reader.Ack(e);
                Check(std::static_pointer_cast<arrow::UInt64Array>(e.batch->GetColumnByName("sequence"))->Value(0) == 1,
                      "none_replay");
            }
            Check(broker.Committed(topic, group + "_none", 0) == RD_KAFKA_OFFSET_INVALID, "none_stored_offset");
            const auto missing = topic + "_missing";
            Add(runtime, "missing", missing, group + "_missing");
            {
                Reader reader(runtime, "missing", false);
                Check(reader.opened != 0, "missing_topic_opened");
            }
            Check(!broker.Exists(missing), "auto_created_missing_topic");
            for (int i = 0; i < 4; ++i) {
                const auto suffix = "bad" + std::to_string(i), bad = topic + "_" + suffix;
                broker.Topic(bad);
                auto m = Message(1);
                if (i == 0) m.value = "malformed";
                if (i == 1) m.value_is_null = true;
                if (i == 2) m.key = "wrong-stream";
                if (i == 3) m.value.append(2048, 'x');
                broker.Produce(bad, m);
                Add(runtime, suffix, bad, group + suffix, "after_success", i == 3 ? 1024 : 2097152);
                {
                    Reader reader(runtime, suffix.c_str());
                    Check(reader.Next().kind == BlockPollEvent::kError, "bad_message_accepted");
                }
                Check(broker.Committed(bad, group + suffix, 0) == RD_KAFKA_OFFSET_INVALID, "bad_message_committed");
            }
            const auto multi = topic + "_multi";
            broker.Topic(multi, 2);
            Add(runtime, "multi", multi, group + "multi");
            {
                Reader reader(runtime, "multi", false);
                Check(reader.opened != 0, "multi_partition_opened");
            }
            const auto epoch = topic + "_epoch";
            broker.Topic(epoch);
            broker.Produce(epoch, Message(1));
            broker.Produce(epoch, Message(2, 1, 2));
            Add(runtime, "epoch", epoch, group + "epoch");
            {
                Reader reader(runtime, "epoch");
                auto e = reader.Next();
                Data(e);
                reader.Ack(e);
                Check(reader.Next().kind == BlockPollEvent::kError, "new_epoch_joined_old_state");
            }
            Check(broker.Committed(epoch, group + "epoch", 0) == 1, "epoch_boundary_committed");
            {
                Reader reader(runtime, "epoch");
                auto e = reader.Next();
                Data(e);
                reader.Ack(e);
            }
            Check(broker.Committed(epoch, group + "epoch", 0) == 2, "new_run_epoch_resume");
            const auto growth = topic + "_growth";
            broker.Topic(growth);
            broker.Produce(growth, Message(1));
            Add(runtime, "growth", growth, group + "growth");
            {
                Reader reader(runtime, "growth");
                auto e = reader.Next();
                Data(e);
                reader.Ack(e);
                broker.Increase(growth);
                Check(reader.Next().kind == BlockPollEvent::kError, "topology_change_ignored");
            }
            Check(broker.Committed(growth, group + "growth", 0) == 1, "topology_invalid_commit");
            const auto owner = topic + "_owner";
            broker.Topic(owner);
            broker.Produce(owner, Message(1));
            Add(runtime, "owner1", owner, group + "owner");
            Add(runtime, "owner2", owner, group + "owner");
            {
                Reader first(runtime, "owner1");
                auto e = first.Next();
                Data(e);
                first.Ack(e);
                Reader second(runtime, "owner2");
                const auto deadline = Clock::now() + std::chrono::seconds(20);
                BlockPollEvent event;
                do {
                    second->PollBlock(50);
                    event = first->PollBlock(50);
                } while (event.kind == BlockPollEvent::kTimeout && Clock::now() < deadline);
                Check(event.kind == BlockPollEvent::kError && event.err == ESTALE, "ownership_reused");
            }
            Performance(runtime, broker, topic + "_perf", group + "perf");
        }
    }
    if (label == "PLAINTEXT") Restart(plan, broker, executable, plan_path);
    std::cout << "PASS " << label
              << " real packet source, zero-row facts, explicit offsets, replay/resume and cancel\n";
}
}  // namespace
int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) return 2;
    try {
        const auto plan = ReadJson(argv[1]);
        if (argc == 3)
            RestartChild(plan, std::stoi(argv[2]));
        else
            Run(plan, std::filesystem::canonical(argv[0]).string(), argv[1]);
        return 0;
    } catch (const Failure& e) {
        std::cerr << "FAIL " << e.code << " stage=" << e.stage << '\n';
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << '\n';
    }
    return 1;
}
