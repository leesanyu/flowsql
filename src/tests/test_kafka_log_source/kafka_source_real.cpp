// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <channels/kafka/kafka_store.h>
#include <common/error_code.h>
#include <framework/core/dataframe.h>
#include <framework/interfaces/iblock_stream_acknowledgement.h>
#include <framework/interfaces/iblock_stream_reader.h>
#include <framework/interfaces/iblock_transform_operator.h>
#include <framework/interfaces/ichannel_registry.h>
#include <framework/interfaces/idataframe_channel.h>
#include <framework/interfaces/irouter_handle.h>
#include <librdkafka/rdkafka.h>
#include <chrono>
#include <common/loader.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <thread>
using namespace flowsql;
using namespace flowsql::channels::kafka;
using Clock = std::chrono::steady_clock;
namespace {
void Check(bool value, const char* stage) {
    if (!value) throw std::runtime_error(stage);
}
Json ReadJson(const std::string& path) {
    std::ifstream input(path);
    Check(bool(input), "private_plan_missing");
    return Parse(std::string((std::istreambuf_iterator<char>(input)), {}));
}
void Set(rd_kafka_conf_t* conf, const char* key, const std::string& value) {
    char error[512];
    Check(rd_kafka_conf_set(conf, key, value.c_str(), error, sizeof(error)) == RD_KAFKA_CONF_OK, "fixture_config");
}
// The fixture's plaintext admin identity owns only this run's derived Topics/groups.
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
    void Topic(const std::string& name, int partitions = 2) {
        char error[512];
        auto* item = rd_kafka_NewTopic_new(name.c_str(), partitions, 1, error, sizeof(error));
        Check(item, "fixture_topic_config");
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
    void Produce(const std::string& topic, int partition, const std::string& value, bool tombstone = false) {
        const char key[] = {'k', '\0', 'x'};
        delivery_ = RD_KAFKA_RESP_ERR__IN_PROGRESS;
        Check(rd_kafka_producev(
                  producer_, RD_KAFKA_V_TOPIC(topic.c_str()), RD_KAFKA_V_PARTITION(partition),
                  RD_KAFKA_V_MSGFLAGS(RD_KAFKA_MSG_F_COPY),
                  RD_KAFKA_V_VALUE(tombstone ? nullptr : const_cast<char*>(value.data()), tombstone ? 0 : value.size()),
                  RD_KAFKA_V_KEY(key, sizeof(key)), RD_KAFKA_V_TIMESTAMP(1700000000000LL),
                  RD_KAFKA_V_OPAQUE(&delivery_), RD_KAFKA_V_END) == 0,
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
};
// Test-only analysis provider: preserve schema/data, stop on positive rows, or fail on request.
class Collector final : public IBlockTransformOperatorV1 {
    class Task final : public IBlockTransformTaskV1 {
     public:
        explicit Task(bool fail) : fail_(fail) {}
        int Open(std::shared_ptr<arrow::Schema> input, std::shared_ptr<arrow::Schema>* output) override {
            if (!input || input->GetFieldIndex("value") < 0 || input->GetFieldIndex("__kafka_offset") < 0) return -1;
            *output = input;
            return 0;
        }
        int ProcessBlock(const std::shared_ptr<arrow::RecordBatch>& input, int64_t timestamp,
                         std::vector<BlockTransformOutputV1>* outputs) override {
            if (fail_) return -1;
            if (!input->num_rows()) return 0;
            outputs->push_back({input, timestamp});
            return 1;
        }
        int Flush(std::vector<BlockTransformOutputV1>*) override { return 0; }
        void Cancel() override {}
        std::string LastError() const override { return "requested fixture failure"; }

     private:
        bool fail_;
    };

 public:
    std::string Category() const override { return "test"; }
    std::string Name() const override { return "kafka_collect"; }
    std::string Description() const override { return "Kafka acceptance fixture"; }
    int CreateTask(const BlockTransformTaskConfigV1& config, IBlockTransformTaskV1** output) override {
        auto params = Parse(config.with_params_json ? config.with_params_json : "{}");
        *output = new Task(Text(params, "fail") == "yes");
        return 0;
    }
    void ReleaseTask(IBlockTransformTaskV1* task) override { delete task; }
};
class Runtime {
 public:
    Runtime(const rapidjson::Value& profile, const std::string& workspace) {
        auto root = std::filesystem::path(workspace);
        std::ofstream(root / "key", std::ios::binary).write("12345678901234567890123456789012", 32);
        auto options = Parse("{}");
        auto& a = options.GetAllocator();
        String(options, "db_path", (root / "kafka.db").string(), a);
        String(options, "master_key_file", (root / "key").string(), a);
        String(options, "runtime_directory", (root / "runtime").string(), a);
        rapidjson::Value aliases;
        aliases.CopyFrom(profile["files"], a);
        options.AddMember("files", aliases, a);
        if (Text(profile["connection"], "sasl_mechanism") == "GSSAPI") {
            auto path = std::filesystem::path(Text(profile["files"], "keytab")).parent_path() / "krb5.conf";
            setenv("KRB5_CONFIG", path.c_str(), 1);
        }
        const auto kafka = Encode(options);
        const auto catalog =
            "data_dir=" + (root / "dataframes").string() + ";operator_db_path=" + (root / "operators.db").string();
        const auto task = "db_path=" + (root / "tasks.db").string();
        const char* libraries[] = {"libflowsql_kafka.so", "libflowsql_builtin.so", "libflowsql_catalog.so",
                                   "libflowsql_scheduler.so", "libflowsql_task.so"};
        const char* configs[] = {kafka.c_str(), nullptr, catalog.c_str(), "stream_workers=1;batch_workers=1",
                                 task.c_str()};
        loader->Regist(IID_BLOCK_TRANSFORM_OPERATOR_V1, &collector_);
        Check(loader->Load(get_absolute_process_path(), libraries, configs, 5) == 0, "runtime_load");
        loaded_ = true;
        Check(loader->StartAll() == 0, "runtime_start");
        auto connection = Parse(Encode(profile["connection"]));
        Call("/channels/kafka/connections/add", connection);
        factory = static_cast<IBlockStreamReaderFactoryV1*>(loader->First(IID_BLOCK_STREAM_READER_FACTORY_V1));
        Check(factory, "reader_factory_missing");
    }
    ~Runtime() {
        if (loaded_) {
            loader->StopAll();
            loader->Unload();
        }
    }
    Json Call(const std::string& path, const rapidjson::Value& request, int expected = error::OK) {
        fnRouterHandler handler;
        loader->Traverse(IID_ROUTER_HANDLE, [&](void* p) {
            static_cast<IRouterHandle*>(p)->EnumRoutes([&](const RouteItem& item) {
                if (item.method == "POST" && item.uri == path) handler = item.handler;
            });
            return handler ? -1 : 0;
        });
        Check(bool(handler), "route_missing");
        std::string response;
        int rc = handler(path, Encode(request), response);
        if (rc != expected) std::cerr << "route=" << path << " rc=" << rc << "\n";
        Check(rc == expected, "route_status");
        return Parse(response);
    }
    void Add(const std::string& name, const std::string& topic, const std::string& group,
             const char* policy = "after_success", unsigned limit = 1048576, const char* connection = "real") {
        auto request = Parse(
            R"({"name":"source","connection_name":"real","role":"source","topic":"events","consumer_group":"group","payload_kind":"structured","payload_options":{"source":{"encoding":"json","entity":"metrics","max_batch_rows":1,"fields":[{"name":"value","path":"/v","type":"int64","nullable":false,"unit":"bytes"},{"name":"time_ms","path":"/t","type":"int64","nullable":true,"unit":"unix_ms"}]}}})");
        auto& a = request.GetAllocator();
        String(request, "name", name, a);
        String(request, "connection_name", connection, a);
        String(request, "topic", topic, a);
        String(request, "consumer_group", group, a);
        auto& source = request["payload_options"]["source"];
        String(source, "commit_policy", policy, a);
        source.AddMember("max_message_bytes", limit, a);
        Call("/channels/kafka/channels/add", request);
    }
    IBlockStreamReaderFactoryV1* factory = nullptr;
    PluginLoader* loader = PluginLoader::Single();

 private:
    Collector collector_;
    bool loaded_ = false;
};
class Reader {
 public:
    Reader(Runtime& runtime, const char* name) : factory_(runtime.factory) {
        BlockStreamReaderConfigV1 config;
        config.source_category = "kafka";
        config.source_name = name;
        config.task_id = "acceptance";
        config.pushed_filter_plan_json = R"({"version":1,"root":null})";
        Check(factory_->CreateReader(config, &reader_) == 0 && reader_, "create_reader");
        Check(reader_->IsOpened(), "factory_open_reader");
    }
    ~Reader() { factory_->ReleaseReader(reader_); }
    IBlockStreamChannel* operator->() { return reader_; }
    BlockPollEvent Next(int seconds = 15) {
        auto deadline = Clock::now() + std::chrono::seconds(seconds);
        BlockPollEvent result;
        do {
            result = reader_->PollBlock(100);
        } while (result.kind == BlockPollEvent::kTimeout && Clock::now() < deadline);
        return result;
    }
    void Ack(const BlockPollEvent& event) {
        auto* ack = dynamic_cast<IBlockStreamAcknowledgementV1*>(reader_);
        Check(ack && ack->AcknowledgeBlock(event.batch) == 0, "acknowledge");
        Check(reader_->ReleaseBlock(event.batch) == 0, "release");
    }

 private:
    IBlockStreamReaderFactoryV1* factory_;
    IBlockStreamChannel* reader_ = nullptr;
};
std::pair<int32_t, int64_t> Identity(const BlockPollEvent& event) {
    if (event.kind != BlockPollEvent::kData) std::cerr << "poll_kind=" << event.kind << " error=" << event.err << '\n';
    Check(event.kind == BlockPollEvent::kData && event.batch->num_rows() == 1, "expected_data");
    auto batch = event.batch;
    Check(batch->schema()->field(1)->metadata()->Get("unit").ValueOrDie() == "unix_ms", "time_unit");
    auto key = std::static_pointer_cast<arrow::BinaryArray>(batch->GetColumnByName("__kafka_key"));
    Check(key->GetString(0) == std::string("k\0x", 3), "binary_key");
    Check(std::static_pointer_cast<arrow::Int64Array>(batch->GetColumnByName("__kafka_timestamp_ms"))->Value(0) ==
              1700000000000LL,
          "message_time");
    return {std::static_pointer_cast<arrow::Int32Array>(batch->GetColumnByName("__kafka_partition"))->Value(0),
            std::static_pointer_cast<arrow::Int64Array>(batch->GetColumnByName("__kafka_offset"))->Value(0)};
}
void Sql(Runtime& runtime, Broker& broker, const std::string& topic, const std::string& group) {
    broker.Topic(topic, 1);
    broker.Produce(topic, 0, R"({"v":-1})");
    broker.Produce(topic, 0, R"({"v":42,"t":1700000000000})");
    runtime.Add("analysis", topic, group);
    const auto sql =
        "SELECT * FROM kafka.analysis WHERE value > 0 USING test.kafka_collect INTO dataframe.kafka_result";
    auto request = Parse("{}");
    auto& a = request.GetAllocator();
    String(request, "sql_text", sql, a);
    String(request, "mode", "sync", a);
    auto rejected = runtime.Call("/tasks/batch/execute", request, error::BAD_REQUEST);
    Check(Text(rejected, "error_code") == "ASYNC_EXECUTION_REQUIRED", "sync_source_rejected");
    String(request, "mode", "async", a);
    auto started = runtime.Call("/tasks/batch/execute", request);
    auto identity = Parse("{}");
    String(identity, "runtime_task_id", Text(started, "runtime_task_id"), identity.GetAllocator());
    auto deadline = Clock::now() + std::chrono::seconds(20);
    std::string status;
    do {
        auto result = runtime.Call("/scheduler/batch/status", identity);
        status = Text(result, "status");
        if (status == "failed") std::cerr << Encode(result) << "\n";
        if (status == "completed" || status == "failed" || status == "stopped") break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (Clock::now() < deadline);
    Check(status == "completed" || status == "stopped", "sql_completion");
    auto* registry = static_cast<IChannelRegistry*>(runtime.loader->First(IID_CHANNEL_REGISTRY));
    Check(registry, "result_registry");
    {
        auto output = std::dynamic_pointer_cast<IDataFrameChannel>(registry->Get("kafka_result"));
        Check(bool(output), "result_channel");
        DataFrame result;
        Check(output->Read(&result) == 0 && result.RowCount() == 1, "sql_filtered_rows");
        auto values = std::static_pointer_cast<arrow::Int64Array>(result.ToArrow()->GetColumnByName("value"));
        Check(values->Value(0) == 42, "sql_result_value");
    }
    Check(broker.Committed(topic, group, 0) == 2, "sql_success_offsets");
    // A real processing failure must leave its own group at the beginning.
    runtime.Add("failed_analysis", topic, group + "_failed");
    auto submit = Parse("{}");
    String(submit, "runtime_task_id", "intentional-failure", submit.GetAllocator());
    String(submit, "sql_text",
           "SELECT * FROM kafka.failed_analysis USING test.kafka_collect WITH fail=yes INTO dataframe.failed_result",
           submit.GetAllocator());
    runtime.Call("/scheduler/batch/submit", submit);
    auto failed = Parse(R"({"runtime_task_id":"intentional-failure"})");
    deadline = Clock::now() + std::chrono::seconds(20);
    do {
        auto result = runtime.Call("/scheduler/batch/status", failed);
        status = Text(result, "status");
        if (status == "failed") break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (Clock::now() < deadline);
    Check(status == "failed" && broker.Committed(topic, group + "_failed", 0) == RD_KAFKA_OFFSET_INVALID,
          "sql_failure_offsets");
    {
        Reader replay(runtime, "failed_analysis");
        auto event = replay.Next();
        Check(Identity(event).second == 0, "sql_failed_replay");
    }
    std::cout << "PASS: real Kafka async SQL, residual filter, DataFrame, successful offsets and failure replay\n";
}
void ContinuousTransfer(Runtime& runtime, Broker& broker, const std::string& topic, const std::string& group) {
    broker.Topic(topic, 1);
    broker.Produce(topic, 0, R"({"v":7})");
    for (const std::string mode : {"cancel", "stop"}) {
        const auto name = "raw_" + mode;
        runtime.Add(name, topic, group + mode);
        auto submit = Parse("{}");
        String(submit, "runtime_task_id", name, submit.GetAllocator());
        String(submit, "sql_text", "SELECT * FROM kafka." + name + " INTO dataframe." + name, submit.GetAllocator());
        runtime.Call("/scheduler/batch/submit", submit);
        auto identity = Parse("{}");
        String(identity, "runtime_task_id", name, identity.GetAllocator());
        auto deadline = Clock::now() + std::chrono::seconds(15);
        while (broker.Committed(topic, group + mode, 0) != 1 && Clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        Check(broker.Committed(topic, group + mode, 0) == 1, "raw_transfer_processed");
        String(identity, "mode", mode, identity.GetAllocator());
        runtime.Call("/scheduler/batch/stop", identity);
        deadline = Clock::now() + std::chrono::seconds(2);
        std::string status;
        do {
            auto result = runtime.Call("/scheduler/batch/status", identity);
            status = Text(result, "status");
            if (status == "cancelled" || status == "stopped") break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } while (Clock::now() < deadline);
        if (status != "cancelled" && status != "stopped") {
            std::cerr << "FAIL: continuous transfer did not stop within budget\n" << std::flush;
            std::_Exit(1);  // Avoid a stuck worker masking the failure during runtime teardown.
        }
        auto* registry = static_cast<IChannelRegistry*>(runtime.loader->First(IID_CHANNEL_REGISTRY));
        Check(bool(registry->Get(name.c_str())) == (mode == "stop"), "raw_transfer_publication");
        // Reader lease was released at the terminal state.
        { Reader released(runtime, name.c_str()); }
    }
    std::cout << "PASS: real continuous transfer cancel/stop, result publication and released leases\n";
}
void Run(const Json& plan) {
    auto profiles = ReadJson(Text(plan, "manifest"));
    const auto label = Text(plan, "profile"), topic = Text(plan, "topic"), group = Text(plan, "group"),
               workspace = Text(plan, "workspace");
    Broker broker(profiles["PLAINTEXT"]);
    broker.Topic(topic);
    broker.Produce(topic, 0, R"({"v":1,"t":1700000000000})");
    broker.Produce(topic, 1, R"({"v":2,"t":1700000000000})");
    {
        Runtime runtime(profiles[label.c_str()], workspace);
        runtime.Add("events", topic, group);
        std::set<std::pair<int32_t, int64_t>> original;
        std::cout << "CHECK: initial read " << label << std::endl;
        {
            Reader reader(runtime, "events");
            auto event = reader.Next();
            original.insert(Identity(event));
            Check(reader->ReleaseBlock(event.batch) == 0, "unconfirmed_release");
        }
        for (int p = 0; p < 2; ++p)
            Check(broker.Committed(topic, group, p) == RD_KAFKA_OFFSET_INVALID, "no_implicit_commit");
        {
            Reader reader(runtime, "events");
            std::cout << "CHECK: same-group replay " << label << std::endl;
            std::set<std::pair<int32_t, int64_t>> replay;
            for (int i = 0; i < 2; ++i) {
                auto event = reader.Next();
                replay.insert(Identity(event));
                reader.Ack(event);
            }
            Check(replay.count(*original.begin()) && replay.size() == 2, "same_group_replay");
        }
        for (int p = 0; p < 2; ++p) Check(broker.Committed(topic, group, p) == 1, "partition_commit");
        {
            Reader reader(runtime, "events");
            std::cout << "CHECK: resume " << label << std::endl;
            // Resume is proven by a new offset, not by an unassigned consumer timing out.
            broker.Produce(topic, 0, R"({"v":3,"t":1700000000000})");
            auto event = reader.Next();
            Check(Identity(event) == std::make_pair(int32_t(0), int64_t(1)), "resume_committed");
            reader.Ack(event);
            if (label == "GSSAPI" || label == "OAUTHBEARER") {
                const auto until = Clock::now() + std::chrono::seconds(label == "GSSAPI" ? 70 : 35);
                while (Clock::now() < until)
                    Check(reader->PollBlock(100).kind == BlockPollEvent::kTimeout, "refresh_idle");
                broker.Produce(topic, 0, R"({"v":4,"t":1700000000000})");
                auto refreshed = reader.Next();
                Check(Identity(refreshed).second == 2, "refreshed_consumer");
                reader.Ack(refreshed);
            }
            const auto before = Clock::now();
            reader->Cancel();
            Check(reader->PollBlock(30000).kind == BlockPollEvent::kCancelled, "cancel_poll");
            Check(Clock::now() - before < std::chrono::seconds(1), "bounded_cancel");
        }
        if (label == "PLAIN") {
            auto connection = Parse(Encode(profiles[label.c_str()]["connection"]));
            String(connection, "name", "invalid", connection.GetAllocator());
            String(connection["credential_bindings"]["password"], "value", "intentional-invalid-source-password",
                   connection.GetAllocator());
            runtime.Call("/channels/kafka/connections/add", connection);
            runtime.Add("wrong", topic, group + "_wrong", "after_success", 1048576, "invalid");
            Reader reader(runtime, "wrong");
            Check(reader.Next().kind == BlockPollEvent::kError, "wrong_auth_fails");
            Check(broker.Committed(topic, group + "_wrong", 0) == RD_KAFKA_OFFSET_INVALID, "wrong_auth_no_commit");
        }
        if (label == "PLAINTEXT") {
            runtime.Add("none", topic, group + "_none", "none");
            for (int i = 0; i < 2; ++i) {
                Reader reader(runtime, "none");
                auto event = reader.Next();
                Check(Identity(event).second == 0, "none_replay");
                reader.Ack(event);
            }
            for (int p = 0; p < 2; ++p)
                Check(broker.Committed(topic, group + "_none", p) == RD_KAFKA_OFFSET_INVALID, "none_no_commit");
            for (int i = 0; i < 3; ++i) {
                auto bad = topic + "_bad" + std::to_string(i);
                broker.Topic(bad, 1);
                broker.Produce(bad, 0, i == 0 ? "{broken" : i == 1 ? "" : std::string(256, 'x'), i == 1);
                auto name = "bad" + std::to_string(i);
                runtime.Add(name, bad, group + name, "after_success", 128);
                {
                    Reader reader(runtime, name.c_str());
                    auto error = reader.Next();
                    Check(error.kind == BlockPollEvent::kError, "bad_message_fails");
                }
                Check(broker.Committed(bad, group + name, 0) == RD_KAFKA_OFFSET_INVALID, "bad_message_no_commit");
            }
            auto missing = topic + "_missing";
            runtime.Add("missing", missing, group + "_missing");
            {
                Reader reader(runtime, "missing");
                Check(reader.Next().kind == BlockPollEvent::kError, "missing_topic_fails");
            }
            Check(!broker.Exists(missing), "no_auto_create");
            Sql(runtime, broker, topic + "_sql", group + "_sql");
            ContinuousTransfer(runtime, broker, topic + "_raw", group + "_raw");
        }
    }
    std::cout << "PASS: " << label
              << " structured source, same-group replay, partition offsets, resume and cancellation\n";
}
}  // namespace
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    try {
        Run(ReadJson(argv[1]));
        return 0;
    } catch (const Failure& error) {
        std::cerr << "FAIL: " << error.code << " stage=" << error.stage << '\n';
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
    }
    return 1;
}
