// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <channels/kafka/kafka_store.h>
#include <common/error_code.h>
#include <framework/core/dataframe.h>
#include <framework/core/dataframe_channel.h>
#include <framework/interfaces/ichannel_registry.h>
#include <framework/interfaces/ikafka_connection_service.h>
#include <framework/interfaces/ikafka_producer_session.h>
#include <framework/interfaces/iresult_sink.h>
#include <framework/interfaces/irouter_handle.h>
#include <librdkafka/rdkafka.h>
#include <sys/resource.h>
#include <chrono>
#include <common/loader.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
using namespace flowsql;
using namespace flowsql::channels::kafka;
using Clock = std::chrono::steady_clock;
namespace {
void Check(bool ok, const char* code) {
    if (!ok) throw std::runtime_error(code);
}
Json Read(const std::string& path) {
    std::ifstream f(path);
    Check(bool(f), "private_plan_missing");
    return Parse(std::string(std::istreambuf_iterator<char>(f), {}));
}
void Set(rd_kafka_conf_t* c, const char* key, const std::string& value) {
    char error[512];
    Check(rd_kafka_conf_set(c, key, value.c_str(), error, sizeof(error)) == RD_KAFKA_CONF_OK, "fixture_config");
}
class Broker {
    rd_kafka_t* client_ = nullptr;
    std::string servers_;
    std::vector<std::string> topics_;

 public:
    explicit Broker(const rapidjson::Value& plain) {
        servers_ = plain["connection"]["bootstrap_servers"][0].GetString();
        auto* c = rd_kafka_conf_new();
        Set(c, "bootstrap.servers", servers_);
        Set(c, "log_level", "0");
        Set(c, "allow.auto.create.topics", "false");
        char error[512];
        client_ = rd_kafka_new(RD_KAFKA_PRODUCER, c, error, sizeof(error));
        Check(client_, "fixture_admin");
    }
    void ForgetTopicsForBrokerShutdown() { topics_.clear(); }
    ~Broker() {
        for (const auto& topic : topics_) {
            auto* item = rd_kafka_DeleteTopic_new(topic.c_str());
            auto* q = rd_kafka_queue_new(client_);
            rd_kafka_DeleteTopics(client_, &item, 1, nullptr, q);
            auto* e = rd_kafka_queue_poll(q, 5000);
            if (e) rd_kafka_event_destroy(e);
            rd_kafka_queue_destroy(q);
            rd_kafka_DeleteTopic_destroy(item);
        }
        rd_kafka_destroy(client_);
    }
    void Topic(const std::string& name, bool small = false) {
        char error[512];
        auto* item = rd_kafka_NewTopic_new(name.c_str(), 1, 1, error, sizeof(error));
        Check(item, "fixture_topic");
        if (small) Check(rd_kafka_NewTopic_set_config(item, "max.message.bytes", "1024") == 0, "fixture_topic_limit");
        auto* q = rd_kafka_queue_new(client_);
        rd_kafka_CreateTopics(client_, &item, 1, nullptr, q);
        auto* e = rd_kafka_queue_poll(q, 10000);
        bool ok = e && !rd_kafka_event_error(e);
        if (ok) {
            size_t count = 0;
            auto** r = rd_kafka_CreateTopics_result_topics(rd_kafka_event_CreateTopics_result(e), &count);
            ok = count == 1 && !rd_kafka_topic_result_error(r[0]);
        }
        if (e) rd_kafka_event_destroy(e);
        rd_kafka_queue_destroy(q);
        rd_kafka_NewTopic_destroy(item);
        Check(ok, "fixture_create_topic");
        topics_.push_back(name);
    }
    void DenyWrite(const std::string& topic) {
        char error[512];
        std::vector<rd_kafka_AclBinding_t*> acl;
        acl.push_back(rd_kafka_AclBinding_new(RD_KAFKA_RESOURCE_TOPIC, topic.c_str(), RD_KAFKA_RESOURCE_PATTERN_LITERAL,
                                              "User:*", "*", RD_KAFKA_ACL_OPERATION_DESCRIBE,
                                              RD_KAFKA_ACL_PERMISSION_TYPE_ALLOW, error, sizeof(error)));
        acl.push_back(rd_kafka_AclBinding_new(RD_KAFKA_RESOURCE_TOPIC, topic.c_str(), RD_KAFKA_RESOURCE_PATTERN_LITERAL,
                                              "User:tester", "*", RD_KAFKA_ACL_OPERATION_WRITE,
                                              RD_KAFKA_ACL_PERMISSION_TYPE_DENY, error, sizeof(error)));
        for (auto* a : acl) Check(a, "fixture_acl_binding");
        auto* q = rd_kafka_queue_new(client_);
        rd_kafka_CreateAcls(client_, acl.data(), acl.size(), nullptr, q);
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
    }
    bool Exists(const std::string& topic) {
        const rd_kafka_metadata_t* m = nullptr;
        Check(rd_kafka_metadata(client_, 1, nullptr, &m, 5000) == 0, "fixture_metadata");
        bool found = false;
        for (int i = 0; i < m->topic_cnt; ++i) found |= topic == m->topics[i].topic;
        rd_kafka_metadata_destroy(m);
        return found;
    }
    std::vector<KafkaProduceMessageV1> Consume(const std::string& topic, const std::string& group) {
        auto* c = rd_kafka_conf_new();
        Set(c, "bootstrap.servers", servers_);
        Set(c, "group.id", group);
        Set(c, "log_level", "0");
        Set(c, "enable.auto.commit", "false");
        Set(c, "enable.auto.offset.store", "false");
        Set(c, "allow.auto.create.topics", "false");
        Set(c, "auto.offset.reset", "earliest");
        char error[512];
        auto* consumer = rd_kafka_new(RD_KAFKA_CONSUMER, c, error, sizeof(error));
        Check(consumer, "fixture_consumer");
        std::unique_ptr<rd_kafka_t, decltype(&rd_kafka_destroy)> owner(consumer, rd_kafka_destroy);
        rd_kafka_poll_set_consumer(consumer);
        int64_t low = 0, high = 0;
        Check(rd_kafka_query_watermark_offsets(consumer, topic.c_str(), 0, &low, &high, 5000) == 0,
              "fixture_watermark");
        Check(low == 0 && high >= 0, "fixture_offsets");
        auto* assignment = rd_kafka_topic_partition_list_new(1);
        rd_kafka_topic_partition_list_add(assignment, topic.c_str(), 0)->offset = 0;
        Check(rd_kafka_assign(consumer, assignment) == 0, "fixture_assign");
        rd_kafka_topic_partition_list_destroy(assignment);
        std::vector<KafkaProduceMessageV1> out;
        auto deadline = Clock::now() + std::chrono::seconds(10);
        while (out.size() < uint64_t(high) && Clock::now() < deadline) {
            auto* message = rd_kafka_consumer_poll(consumer, 100);
            if (!message) continue;
            if (message->err) {
                auto error = message->err;
                rd_kafka_message_destroy(message);
                if (error == RD_KAFKA_RESP_ERR__PARTITION_EOF) continue;
                throw std::runtime_error("fixture_consume");
            }
            Check(message->payload && message->key, "fixture_not_tombstone");
            out.push_back({std::string(static_cast<const char*>(message->key), message->key_len),
                           std::string(static_cast<const char*>(message->payload), message->len)});
            rd_kafka_message_destroy(message);
        }
        Check(out.size() == uint64_t(high), "fixture_count");
        rd_kafka_assign(consumer, nullptr);
        rd_kafka_consumer_close(consumer);
        return out;
    }
};
class Runtime {
    PluginLoader* loader_ = PluginLoader::Single();
    bool loaded_ = false;

 public:
    IResultSinkFactoryV1* factory = nullptr;
    IKafkaConnectionServiceV1* sessions = nullptr;
    Runtime(const rapidjson::Value& profile, const std::string& workspace) {
        const auto root = std::filesystem::path(workspace);
        std::ofstream(root / "key", std::ios::binary).write("12345678901234567890123456789012", 32);
        auto opts = Parse("{}");
        auto& a = opts.GetAllocator();
        String(opts, "db_path", (root / "kafka.db").string(), a);
        String(opts, "master_key_file", (root / "key").string(), a);
        String(opts, "runtime_directory", (root / "runtime").string(), a);
        opts.AddMember("files", rapidjson::Value(profile["files"], a), a);
        if (Text(profile["connection"], "sasl_mechanism") == "GSSAPI")
            setenv("KRB5_CONFIG",
                   (std::filesystem::path(Text(profile["files"], "keytab")).parent_path() / "krb5.conf").c_str(), 1);
        auto kafka = Encode(opts);
        auto catalog =
            "data_dir=" + (root / "frames").string() + ";operator_db_path=" + (root / "operators.db").string();
        const char* libs[] = {"libflowsql_kafka.so", "libflowsql_builtin.so", "libflowsql_catalog.so",
                              "libflowsql_scheduler.so"};
        const char* configs[] = {kafka.c_str(), nullptr, catalog.c_str(), "batch_workers=1"};
        Check(loader_->Load(get_absolute_process_path(), libs, configs, 4) == 0, "runtime_load");
        loaded_ = true;
        Check(loader_->StartAll() == 0, "runtime_start");
        Call("/channels/kafka/connections/add", profile["connection"]);
        factory = static_cast<IResultSinkFactoryV1*>(loader_->First(IID_RESULT_SINK_FACTORY_V1));
        sessions = static_cast<IKafkaConnectionServiceV1*>(loader_->First(IID_KAFKA_CONNECTION_SERVICE_V1));
        Check(factory && sessions, "factory_discovery");
    }
    ~Runtime() {
        if (loaded_) {
            loader_->StopAll();
            loader_->Unload();
        }
    }
    Json Call(const std::string& path, const rapidjson::Value& request, int expected = error::OK) {
        fnRouterHandler h;
        loader_->Traverse(IID_ROUTER_HANDLE, [&](void* p) {
            static_cast<IRouterHandle*>(p)->EnumRoutes([&](const RouteItem& item) {
                if (item.method == "POST" && item.uri == path) h = item.handler;
            });
            return h ? -1 : 0;
        });
        Check(bool(h), "route_missing");
        std::string response;
        auto rc = h(path, Encode(request), response);
        Check(rc == expected, "route_status");
        return Parse(response);
    }
    void Add(const std::string& name, const std::string& topic, const char* connection = "real",
             unsigned message_limit = 1048576) {
        auto j = Parse(
            R"({"name":"out","connection_name":"real","role":"sink","topic":"events","payload_kind":"structured","payload_options":{"sink":{"encoding":"json","entity":"alert","schema_version":1,"record_kind":"upsert","key_fields":["id"],"revision_field":"revision","queue_max_messages":128,"delivery_timeout_ms":5000}}})");
        auto& a = j.GetAllocator();
        String(j, "name", name, a);
        String(j, "topic", topic, a);
        String(j, "connection_name", connection, a);
        j["payload_options"]["sink"].AddMember("max_message_bytes", message_limit, a);
        Call("/channels/kafka/channels/add", j);
    }
    std::unique_ptr<IResultSinkWriterV1> Writer(const char* name) {
        std::unique_ptr<IResultSinkWriterV1> w;
        Check(factory->CreateWriter({1, std::string("kafka.") + name, "real-acceptance"}, &w) == 0 && w,
              "create_writer");
        return w;
    }
    Json Execute(const std::shared_ptr<arrow::RecordBatch>& batch, const std::string& sql) {
        auto* registry = static_cast<IChannelRegistry*>(loader_->First(IID_CHANNEL_REGISTRY));
        Check(registry, "registry_missing");
        auto frame = std::make_shared<DataFrameChannel>("dataframe", "input");
        DataFrame data;
        data.FromArrow(batch);
        Check(frame->Open() == 0 && frame->Write(&data) == 0 && registry->Register("input", frame) == 0,
              "snapshot_register");
        auto j = Parse("{}");
        String(j, "sql", sql, j.GetAllocator());
        auto result = Call("/scheduler/batch/execute", j);
        DataFrame after;
        Check(frame->Read(&after) == 0 && after.ToArrow()->Equals(*batch), "snapshot_mutated");
        registry->Unregister("input");
        return result;
    }
};
std::shared_ptr<arrow::RecordBatch> Batch(int n = 3, size_t width = 32) {
    arrow::Int64Builder id;
    arrow::UInt64Builder rev;
    arrow::StringBuilder text;
    for (int i = 0; i < n; ++i) {
        Check(id.Append(i + 1).ok() && rev.Append(UINT64_MAX).ok() && text.Append(std::string(width, 'x')).ok(),
              "fixture_array");
    }
    return arrow::RecordBatch::Make(
        arrow::schema({arrow::field("id", arrow::int64(), false), arrow::field("revision", arrow::uint64(), false),
                       arrow::field("message", arrow::utf8())}),
        n, {id.Finish().ValueOrDie(), rev.Finish().ValueOrDie(), text.Finish().ValueOrDie()});
}
void Balanced(const ResultSinkReportV1& r) {
    Check(r.input_rows == r.confirmed_rows + r.not_sent_rows + r.unknown_rows, "report_conservation");
}
double Cpu(const rusage& r) {
    return r.ru_utime.tv_sec + r.ru_utime.tv_usec / 1e6 + r.ru_stime.tv_sec + r.ru_stime.tv_usec / 1e6;
}
}  // namespace
int main(int argc, char** argv) {
    try {
        Check(argc == 2, "private_plan_required");
        auto plan = Read(argv[1]);
        auto manifest = Read(Text(plan, "manifest"));
        const auto label = Text(plan, "profile");
        Check(manifest.HasMember(label.c_str()), "profile_missing");
        const auto topic = Text(plan, "topic"), group = Text(plan, "group");
        Broker broker(manifest["PLAINTEXT"]);
        broker.Topic(topic);
        Runtime runtime(manifest[label.c_str()], Text(plan, "workspace"));
        runtime.Add("out", topic);
        auto batch = Batch();
        if (plan.HasMember("disconnect") && plan["disconnect"].GetBool()) {
            auto w = runtime.Writer("out");
            Check(w->Open(batch->schema()) == 0 && w->Write(batch) == 0, "disconnect_initial");
            Check(broker.Consume(topic, group).size() == 3, "disconnect_initial_visibility");
            broker.ForgetTopicsForBrokerShutdown();  // This isolated broker is about to be terminated by its owner.
            const auto workspace = Text(plan, "workspace");
            std::ofstream(workspace + "/ready").put('1');
            auto deadline = Clock::now() + std::chrono::seconds(30);
            while (!std::filesystem::exists(workspace + "/disconnected") && Clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            Check(std::filesystem::exists(workspace + "/disconnected"), "disconnect_signal_timeout");
            auto start = Clock::now();
            Check(w->Write(batch) != 0, "disconnect_must_fail");
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
            auto report = w->Report();
            Balanced(report);
            Check(report.state == "failed" && report.stage == "sink_deliver" && report.input_rows == 6 &&
                      report.confirmed_rows == 3 && report.not_sent_rows + report.unknown_rows == 3 && ms < 6500,
                  "disconnect_bounded_partial_report");
            Check(w->Write(batch) != 0 && w->Finish() != 0, "disconnect_no_application_retry");
            std::cout << "PASS controlled_real_disconnect confirmed=" << report.confirmed_rows
                      << " not_sent=" << report.not_sent_rows << " unknown=" << report.unknown_rows
                      << " elapsed_ms=" << ms << "\n";
            return 0;
        }
        std::vector<std::string> publications;
        for (int run = 0; run < 2; ++run) {
            auto w = runtime.Writer("out");
            Check(w->Open(batch->schema()) == 0 && w->Write(batch) == 0, "authenticated_publish");
            if (run == 0 && (label == "GSSAPI" || label == "OAUTHBEARER")) {
                int seconds = label == "GSSAPI" ? 70 : 35;
                auto deadline = Clock::now() + std::chrono::seconds(seconds);
                while (Clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(100));
                Check(w->Write(batch) == 0, "renewed_identity_publish");
            }
            Check(w->Finish() == 0, "publish_finish");
            auto r = w->Report();
            Balanced(r);
            Check(r.state == "finished" && r.unknown_rows == 0, "publish_report");
            publications.push_back(r.publication_id);
        }
        Check(publications[0] != publications[1], "publication_isolation");
        auto messages = broker.Consume(topic, group);
        const size_t expected = (label == "GSSAPI" || label == "OAUTHBEARER") ? 9 : 6;
        Check(messages.size() == expected, "authenticated_count");
        for (size_t i = 0; i < messages.size(); ++i) {
            auto value = Parse(messages[i].value);
            rapidjson::Document key;
            key.Parse<rapidjson::kParseValidateEncodingFlag>(messages[i].key.data(), messages[i].key.size());
            Check(!key.HasParseError(), "key_json");
            Check(key.IsArray() && key.Size() == 3 && key[0] == "alert" && key[1].GetUint() == 1, "typed_key");
            Check(value["envelope_version"].GetInt() == 1 && value["schema_version"].GetUint() == 1 &&
                      value["entity"] == "alert" && value["record_kind"] == "upsert",
                  "envelope");
            Check(value["revision"] == "18446744073709551615" && value["data"]["id"].IsString() &&
                      value["schema_fingerprint"].GetStringLength() == 64,
                  "lossless_data");
            Check(messages[i].key == messages[i % 3].key, "reexecution_key_stability");
        }
        auto counts = runtime.Execute(batch, "SELECT id, revision FROM dataframe.input WHERE id >= 2 INTO kafka.out");
        Check(counts["rows_affected"].GetUint64() == 2, "scheduler_real_rows");
        Check(broker.Consume(topic, group).size() == expected + 2, "scheduler_real_delivery");
        std::cout << "PASS profile=" << label << " confirmed=" << expected << " replay_duplicates=3 snapshot_rows=2\n"
                  << std::flush;
        if (label == "PLAINTEXT") {
            const auto missing = topic + "_missing";
            runtime.Add("missing", missing);
            {
                auto w = runtime.Writer("missing");
                Check(w->Open(batch->schema()) != 0, "missing_topic_must_fail");
                Check(w->Report().stage == "sink_open", "missing_stage");
            }
            Check(!broker.Exists(missing), "missing_topic_created");
            broker.Topic(topic + "_small", true);
            runtime.Add("small", topic + "_small");
            {
                auto large = Batch(1, 4096);
                auto w = runtime.Writer("small");
                Check(w->Open(large->schema()) == 0 && w->Write(large) != 0, "broker_size_rejection");
                Balanced(w->Report());
                Check(w->Report().unknown_rows == 1, "broker_error_unknown");
            }
            Check(broker.Consume(topic + "_small", group).empty(), "broker_size_side_effect");
            runtime.Add("limit", topic, "real", 1024);
            {
                auto w = runtime.Writer("limit");
                auto large = Batch(1, 4096);
                Check(w->Open(large->schema()) == 0 && w->Write(large) != 0, "local_size_rejection");
            }
        }
        if (label == "PLAIN") {
            const auto denied = topic + "_denied";
            broker.Topic(denied);
            broker.DenyWrite(denied);
            runtime.Add("denied", denied);
            {
                auto w = runtime.Writer("denied");
                Check(w->Open(batch->schema()) == 0, "deny_metadata_should_pass");
                Check(w->Write(batch) != 0, "write_acl_must_fail");
                Balanced(w->Report());
                Check(w->Report().confirmed_rows == 0 && w->Report().unknown_rows == 3, "acl_unknown");
            }
            Check(broker.Consume(denied, group).empty(), "denied_publication");
            auto bad = Parse(Encode(manifest["PLAIN"]["connection"]));
            String(bad, "name", "bad", bad.GetAllocator());
            String(bad["credential_bindings"]["password"], "value", "deliberately-invalid-credential",
                   bad.GetAllocator());
            runtime.Call("/channels/kafka/connections/add", bad);
            runtime.Add("bad", topic, "bad");
            auto w = runtime.Writer("bad");
            Check(w->Open(batch->schema()) != 0 && w->Report().confirmed_rows == 0, "bad_auth_must_fail");
        }
        if (label == "PLAINTEXT") {
            const auto partial = topic + "_partial";
            broker.Topic(partial);
            runtime.Add("partial", partial);
            auto w = runtime.Writer("partial");
            Check(w->Open(batch->schema()) == 0 && w->Write(batch) == 0, "partial_first_batch");
            auto invalid =
                batch->SetColumn(1, batch->schema()->field(1), arrow::MakeArrayOfNull(arrow::uint64(), 3).ValueOrDie())
                    .ValueOrDie();
            Check(w->Write(invalid) != 0, "partial_invalid");
            auto r = w->Report();
            Balanced(r);
            Check(r.confirmed_rows == 3 && r.not_sent_rows == 3, "partial_count");
            Check(broker.Consume(partial, group).size() == 3, "partial_visibility");
            w.reset();
            const auto cancel = topic + "_cancel";
            broker.Topic(cancel);
            runtime.Add("cancel", cancel);
            auto cw = runtime.Writer("cancel");
            Check(cw->Open(batch->schema()) == 0 && cw->Write(batch) == 0, "cancel_initial");
            auto many = Batch(20000, 128);
            std::thread worker([&] { Check(cw->Write(many) == ECANCELED, "cancel_return"); });
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            auto start = Clock::now();
            cw->Cancel();
            worker.join();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
            r = cw->Report();
            Balanced(r);
            Check(r.state == "cancelled" && ms < 1000, "cancel_budget");
            auto visible = broker.Consume(cancel, group).size();
            Check(visible >= r.confirmed_rows && visible <= r.input_rows - r.not_sent_rows, "cancel_visibility");
            const auto cost = topic + "_cost";
            broker.Topic(cost);
            runtime.Add("cost", cost);
            auto pw = runtime.Writer("cost");
            auto payload = Batch(1000, 128);
            Check(pw->Open(payload->schema()) == 0, "cost_open");
            rusage before{}, after{};
            getrusage(RUSAGE_SELF, &before);
            auto began = Clock::now();
            Check(pw->Write(payload) == 0 && pw->Finish() == 0, "cost_publish");
            double seconds = std::chrono::duration<double>(Clock::now() - began).count();
            getrusage(RUSAGE_SELF, &after);
            auto observed = broker.Consume(cost, group);
            Check(observed.size() == 1000, "cost_count");
            uint64_t bytes = 0;
            for (auto& m : observed) bytes += m.key.size() + m.value.size();
            std::cout << "COST rows=1000 text_bytes=128 elapsed_s=" << seconds << " messages_s=" << 1000 / seconds
                      << " payload_bytes_s=" << bytes / seconds << " cpu_s=" << Cpu(after) - Cpu(before)
                      << " cpu_percent=" << (Cpu(after) - Cpu(before)) / seconds * 100
                      << " peak_rss_kib=" << after.ru_maxrss << " cancel_ms=" << ms << "\n";
        }
        std::cout << "PASS sink profile=" << label << " complete\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL sink acceptance stage=" << e.what() << "\n";
        return 1;
    }
}
