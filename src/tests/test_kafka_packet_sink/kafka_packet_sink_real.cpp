// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <common/error_code.h>
#include <framework/interfaces/irouter_handle.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <common/loader.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <thread>
#include "packet_sink_broker.h"
#include "packet_sink_test_support.h"
using namespace packet_sink_real;
namespace {
Json Read(const std::string& path) {
    std::ifstream file(path);
    Check(bool(file), "private_plan_missing");
    return Parse(std::string(std::istreambuf_iterator<char>(file), {}));
}
class Runtime {
    PluginLoader* loader_ = PluginLoader::Single();
    bool loaded_ = false;

 public:
    IPacketSinkFactoryV1* factory = nullptr;
    IKafkaConnectionServiceV1* sessions = nullptr;
    Runtime(const rapidjson::Value& profile, const std::string& workspace) {
        const auto root = std::filesystem::path(workspace);
        std::filesystem::create_directories(root);
        chmod(root.c_str(), 0700);
        if (!std::filesystem::exists(root / "key")) {
            std::random_device random;
            std::ofstream key(root / "key", std::ios::binary);
            for (int i = 0; i < 32; ++i) key.put(static_cast<char>(random()));
            key.close();
            chmod((root / "key").c_str(), 0600);
        }
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
        if (!std::filesystem::exists(root / "configured")) {
            Call("/channels/kafka/connections/add", profile["connection"]);
            std::ofstream(root / "configured").put('1');
        }
        factory = static_cast<IPacketSinkFactoryV1*>(loader_->First(IID_PACKET_SINK_FACTORY_V1));
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
    void Add(const std::string& name, const std::string& topic, const char* connection = "real") {
        auto j = Parse(
            R"({"name":"out","connection_name":"real","role":"sink","topic":"events","payload_kind":"packet","payload_options":{"sink":{"encoding":"flowsql_packet_v1","stream_id":"probe-a","delivery_timeout_ms":5000}}})");
        auto& a = j.GetAllocator();
        String(j, "name", name, a);
        String(j, "topic", topic, a);
        String(j, "connection_name", connection, a);
        Call("/channels/kafka/channels/add", j);
    }
    std::unique_ptr<IPacketSinkWriterV1> Writer(const char* name) {
        std::unique_ptr<IPacketSinkWriterV1> w;
        Check(factory->CreateWriter({1, std::string("kafka.") + name, "real-acceptance"}, &w) == 0 && w,
              "create_writer");
        return w;
    }
};
void Balanced(const PacketSinkReportV1& r) {
    for (const auto* c : {&r.messages, &r.packets})
        Check(c->input == c->confirmed + c->not_sent + c->unknown, "report_conservation");
}
uint64_t Get(const std::string& b, size_t at, size_t n) {
    Check(at + n <= b.size(), "wire_bounds");
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) v = (v << 8) | uint8_t(b[at + i]);
    return v;
}
std::string Compare(const KafkaProduceMessageV1& message, const CapturePollEventV2& e, uint64_t poll) {
    const auto& b = message.value;
    const uint64_t rows = e.block.batch ? e.block.batch->num_rows() : 0;
    Check(message.key == "probe-a" && b.size() >= 56 && b.compare(0, 4, "FSPK") == 0 && Get(b, 4, 2) == 1 &&
              Get(b, 6, 2) == 0 && Get(b, 24, 8) == 7 && Get(b, 32, 8) == 9 && Get(b, 40, 8) == poll &&
              Get(b, 48, 4) == rows && Get(b, 52, 4) == e.progress.size(),
          "wire_header");
    size_t at = 56;
    for (uint64_t row = 0; row < rows; ++row) {
        const auto& a = e.block.batch;
        const auto raw = static_cast<const arrow::BinaryArray&>(*a->column(6)).GetView(row);
        Check(Get(b, at, 4) == static_cast<const arrow::UInt32Array&>(*a->column(4)).Value(row) &&
                  Get(b, at + 4, 8) == uint64_t(static_cast<const arrow::Int64Array&>(*a->column(0)).Value(row)) &&
                  Get(b, at + 12, 4) == raw.size() &&
                  Get(b, at + 16, 4) == static_cast<const arrow::UInt32Array&>(*a->column(2)).Value(row) &&
                  Get(b, at + 20, 4) == 1 &&
                  Get(b, at + 24, 8) == static_cast<const arrow::UInt64Array&>(*a->column(5)).Value(row),
              "wire_metadata");
        Check(b.compare(at + 32, raw.size(), raw.data(), raw.size()) == 0, "wire_raw_bytes");
        at += 32 + raw.size();
    }
    for (const auto& f : e.progress) {
        Check(Get(b, at, 4) == f.source_id && Get(b, at + 4, 4) == f.queue_id && Get(b, at + 8, 8) == f.fact_sequence &&
                  Get(b, at + 16, 8) == uint64_t(f.capture_time_ns) &&
                  Get(b, at + 24, 1) == uint64_t(f.packet_observed | (f.source_idle_confirmed << 1)) &&
                  Get(b, at + 25, 1) == uint64_t(f.backlog) && Get(b, at + 26, 2) == 0,
              "wire_fact");
        at += 28;
    }
    Check(at == b.size(), "wire_trailing_bytes");
    return b.substr(8, 16);
}
CapturePollEventV2 Large(uint64_t sequence, size_t bytes = 4096) {
    auto event = packet_sink_test::Event(sequence, 1);
    packet::PacketFamilyBatch records;
    auto owner = std::make_shared<std::string>(bytes, '\0');
    for (size_t i = 0; i < bytes; ++i) (*owner)[i] = char(i * 17);
    packet::PacketFamilyRecord record;
    record.meta = {100, uint32_t(bytes), uint32_t(bytes), 1, 0, sequence * 100};
    record.raw_data = {owner, reinterpret_cast<const uint8_t*>(owner->data()), uint32_t(bytes)};
    records.rows.push_back(record);
    Check(packet::EncodePacketFamilyBatch(records, &event.block.batch) == packet::PacketBatchError::kNone,
          "fixture_large_batch");
    return event;
}
double Cpu(const rusage& r) {
    return r.ru_utime.tv_sec + r.ru_utime.tv_usec / 1e6 + r.ru_stime.tv_sec + r.ru_stime.tv_usec / 1e6;
}
void RestartChild(const Json& plan, const Json& manifest, int generation) {
    Check(generation == 1 || generation == 2, "restart_generation");
    Runtime runtime(manifest[Text(plan, "profile").c_str()], Text(plan, "workspace") + "/restart-state");
    if (generation == 1) runtime.Add("restart", Text(plan, "topic") + "_restart");
    auto w = runtime.Writer("restart");
    Check(!w->Open(packet_sink_test::Sources(), packet::PacketFamilySchema()) &&
              !w->Write(packet_sink_test::Event(generation)) && !w->Finish(),
          "restart_publish");
    Balanced(w->Report());
    Check(w->Report().packets.confirmed == 3, "restart_confirmed");
}
void Restart(const Json& plan, Broker& broker, const std::string& executable, const std::string& plan_path) {
    const auto topic = Text(plan, "topic") + "_restart";
    broker.Topic(topic);
    for (int generation : {1, 2}) {
        const auto number = std::to_string(generation);
        const auto child = fork();
        Check(child >= 0, "restart_fork");
        if (!child) {
            execl(executable.c_str(), executable.c_str(), plan_path.c_str(), number.c_str(), nullptr);
            _exit(127);
        }
        int status = 0;
        bool finished = false;
        const auto deadline = Clock::now() + std::chrono::seconds(20);
        while (Clock::now() < deadline) {
            if (waitpid(child, &status, WNOHANG) == child) {
                finished = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!finished) {
            kill(child, SIGTERM);
            waitpid(child, &status, 0);
        }
        Check(finished && WIFEXITED(status) && !WEXITSTATUS(status), "restart_child_failed");
    }
    const auto messages = broker.Consume(topic, Text(plan, "group") + "_restart");
    Check(messages.size() == 2, "restart_message_count");
    const auto first = Compare(messages[0], packet_sink_test::Event(1), 1);
    const auto second = Compare(messages[1], packet_sink_test::Event(2), 1);
    Check(first != second, "process_restart_new_epoch");
    std::cout
        << "PASS real process restart persisted configuration/master key, new epoch, poll sequence restarts at 1\n";
}
}  // namespace
int main(int argc, char** argv) {
    try {
        Check(argc == 2 || argc == 3, "private_plan_required");
        auto plan = Read(argv[1]);
        auto manifest = Read(Text(plan, "manifest"));
        const auto label = Text(plan, "profile"), topic = Text(plan, "topic"), group = Text(plan, "group");
        Check(manifest.HasMember(label.c_str()), "profile_missing");
        if (argc == 3) {
            RestartChild(plan, manifest, std::stoi(argv[2]));
            return 0;
        }
        Broker broker(manifest["PLAINTEXT"]);
        broker.Topic(topic);
        Runtime runtime(manifest[label.c_str()], Text(plan, "workspace"));
        runtime.Add("out", topic);
        const auto sources = packet_sink_test::Sources();
        const auto schema = packet::PacketFamilySchema();
        if (plan.HasMember("disconnect") && plan["disconnect"].GetBool()) {
            auto w = runtime.Writer("out");
            auto first = packet_sink_test::Event(1);
            Check(!w->Open(sources, schema) && !w->Write(first), "disconnect_initial");
            auto observed = broker.Consume(topic, group);
            Check(observed.size() == 1, "disconnect_initial_count");
            Compare(observed[0], first, 1);
            broker.ForgetTopicsForBrokerShutdown();
            const auto workspace = Text(plan, "workspace");
            std::ofstream(workspace + "/ready").put('1');
            const auto deadline = Clock::now() + std::chrono::seconds(30);
            while (!std::filesystem::exists(workspace + "/disconnected") && Clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            Check(std::filesystem::exists(workspace + "/disconnected"), "disconnect_signal");
            auto second = packet_sink_test::Event(2);
            auto start = Clock::now();
            Check(w->Write(second) != 0, "disconnect_must_fail");
            auto r = w->Report();
            Balanced(r);
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
            Check(r.state == "failed" && r.packets.input == 6 && r.packets.confirmed == 3 &&
                      r.packets.unknown + r.packets.not_sent == 3 && elapsed < 6500,
                  "disconnect_partial_budget");
            Check(w->Write(second) != 0 && w->Finish() != 0, "disconnect_no_replay");
            std::cout << "PASS controlled_real_disconnect confirmed=" << r.packets.confirmed
                      << " unknown=" << r.packets.unknown << " not_sent=" << r.packets.not_sent
                      << " elapsed_ms=" << elapsed << '\n';
            return 0;
        }
        std::vector<CapturePollEventV2> events;
        std::vector<uint64_t> sequences;
        std::vector<size_t> starts;
        std::vector<std::string> epochs;
        for (int run = 0; run < 2; ++run) {
            starts.push_back(events.size());
            auto w = runtime.Writer("out");
            Check(!w->Open(sources, schema), "authenticated_open");
            uint64_t seq = 0;
            auto publish = [&](CapturePollEventV2 e) {
                Check(!w->Write(e), "authenticated_publish");
                events.push_back(std::move(e));
                sequences.push_back(++seq);
            };
            publish(packet_sink_test::Event(1));
            publish(packet_sink_test::Event(2, 0));
            CapturePollEventV2 empty;
            Check(!w->Write(empty) && w->Report().messages.input == 2, "empty_timeout_no_message");
            if (!run && (label == "GSSAPI" || label == "OAUTHBEARER")) {
                const int seconds = label == "GSSAPI" ? 70 : 35;
                const auto deadline = Clock::now() + std::chrono::seconds(seconds);
                while (Clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(100));
                publish(packet_sink_test::Event(3));
                std::cout << "PASS renewed_identity_publish profile=" << label << '\n';
            }
            Check(!w->Finish(), "publish_finish");
            const auto r = w->Report();
            Balanced(r);
            Check(r.state == "finished" && !r.packets.unknown && r.messages.confirmed == seq, "publish_report");
            epochs.push_back(Text(Parse(r.binding_json), "stream_epoch"));
        }
        auto observed = broker.Consume(topic, group);
        Check(observed.size() == events.size() && epochs[0] != epochs[1], "publication_isolation");
        std::string epoch;
        for (size_t i = 0; i < observed.size(); ++i) {
            auto next = Compare(observed[i], events[i], sequences[i]);
            if (i == starts[1])
                Check(next != epoch, "wire_new_epoch");
            else if (i)
                Check(next == epoch, "wire_stable_epoch");
            epoch = next;
        }
        std::cout << "PASS profile=" << label << " messages=" << observed.size()
                  << " independent_raw_metadata_facts_order=verified new_epoch=verified\n"
                  << std::flush;
        if (label == "PLAIN") {
            const auto denied = topic + "_denied";
            broker.Topic(denied);
            broker.DenyWrite(denied);
            runtime.Add("denied", denied);
            auto w = runtime.Writer("denied");
            Check(!w->Open(sources, schema), "deny_metadata_should_pass");
            Check(w->Write(packet_sink_test::Event()) != 0, "write_acl_must_fail");
            Balanced(w->Report());
            Check(w->Report().packets.confirmed == 0 && w->Report().packets.unknown == 3, "acl_unknown");
            Check(broker.Consume(denied, group).empty(), "denied_publication");
        }
        if (label == "PLAINTEXT") {
            runtime.Add("missing", topic + "_missing");
            {
                auto w = runtime.Writer("missing");
                Check(w->Open(sources, schema) != 0 && w->Report().stage == "sink_open", "missing_topic_must_fail");
            }
            Check(!broker.Exists(topic + "_missing"), "missing_topic_created");
            broker.Topic(topic + "_small", true);
            runtime.Add("small", topic + "_small");
            {
                auto w = runtime.Writer("small");
                const int opened = w->Open(sources, schema);
                const int written = opened ? opened : w->Write(Large(1));
                if (opened || !written) {
                    const auto report = w->Report();
                    std::cerr << "broker_limit_open=" << opened << " write=" << written << " state=" << report.state
                              << " stage=" << report.stage << " code=" << report.code << '\n';
                }
                Check(!opened && written != 0, "broker_size_rejection");
                Balanced(w->Report());
                Check(w->Report().packets.unknown == 1, "broker_size_unknown");
            }
            Check(broker.Consume(topic + "_small", group).empty(), "broker_size_side_effect");
            broker.Topic(topic + "_partial");
            runtime.Add("partial", topic + "_partial");
            {
                auto w = runtime.Writer("partial");
                Check(!w->Open(sources, schema) && !w->Write(packet_sink_test::Event()), "partial_initial");
                Check(w->Write(packet_sink_test::Event()) != 0, "partial_sequence_error");
                auto r = w->Report();
                Balanced(r);
                Check(r.packets.confirmed == 3 && r.packets.not_sent == 3, "partial_count");
                Check(broker.Consume(topic + "_partial", group).size() == 1, "partial_visibility");
            }
            broker.Topic(topic + "_cancel");
            runtime.Add("cancel", topic + "_cancel");
            {
                auto w = runtime.Writer("cancel");
                Check(!w->Open(sources, schema) && !w->Write(packet_sink_test::Event()), "cancel_initial");
                w->Cancel();
                Check(w->Write(packet_sink_test::Event(2)) == ECANCELED && w->Finish() == ECANCELED,
                      "cancel_no_send_or_finish");
                Balanced(w->Report());
                Check(w->Report().state == "cancelled" && broker.Consume(topic + "_cancel", group).size() == 1,
                      "cancel_visibility");
            }
            broker.Topic(topic + "_cost");
            runtime.Add("cost", topic + "_cost");
            auto w = runtime.Writer("cost");
            Check(!w->Open(sources, schema), "cost_open");
            rusage before{}, after{};
            getrusage(RUSAGE_SELF, &before);
            const auto start = Clock::now();
            std::vector<int64_t> latency;
            for (uint64_t i = 1; i <= 100; ++i) {
                auto began = Clock::now();
                Check(!w->Write(Large(i)), "cost_write");
                latency.push_back(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - began).count());
            }
            Check(!w->Finish(), "cost_finish");
            double seconds = std::chrono::duration<double>(Clock::now() - start).count();
            getrusage(RUSAGE_SELF, &after);
            auto messages = broker.Consume(topic + "_cost", group);
            Check(messages.size() == 100, "cost_count");
            uint64_t bytes = 0;
            for (size_t i = 0; i < messages.size(); ++i) {
                Compare(messages[i], Large(i + 1), i + 1);
                bytes += messages[i].key.size() + messages[i].value.size();
            }
            std::sort(latency.begin(), latency.end());
            std::cout << "COST packets=100 frame_bytes=4096 elapsed_s=" << seconds << " packets_s=" << 100 / seconds
                      << " messages_s=" << 100 / seconds << " payload_bytes_s=" << bytes / seconds
                      << " confirm_p95_us=" << latency[95] << " cpu_s=" << Cpu(after) - Cpu(before)
                      << " cpu_percent=" << (Cpu(after) - Cpu(before)) / seconds * 100
                      << " peak_rss_kib=" << after.ru_maxrss << " binding=" << w->Report().binding_json << '\n';
        }
        if (label == "PLAINTEXT") Restart(plan, broker, std::filesystem::absolute(argv[0]).string(), argv[1]);
        std::cout << "PASS packet sink profile=" << label << " complete\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL packet sink acceptance stage=" << e.what() << '\n';
        return 1;
    }
}
