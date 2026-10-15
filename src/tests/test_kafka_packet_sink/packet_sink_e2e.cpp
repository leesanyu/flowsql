// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <arpa/inet.h>
#include <framework/interfaces/iblock_stream_manager.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstring>
#include <random>
#include "../test_kafka_packet_source/packet_test_runtime.h"
#include "packet_sink_broker.h"
using namespace packet_test;
using Clock = std::chrono::steady_clock;
namespace {
Json Read(const std::string& path) {
    std::ifstream file(path);
    Check(bool(file), "private_plan_missing");
    return Parse(std::string(std::istreambuf_iterator<char>(file), {}));
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
void LinkDown(const char* name) {
    const auto child = fork();
    Check(child >= 0, "link_fork");
    if (!child) {
        execlp("ip", "ip", "link", "set", name, "down", nullptr);
        _exit(127);
    }
    int status = 0;
    Check(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status), "isolated_link_down");
}
void Send(const char* name) {
    const int fd = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, htons(ETH_P_ALL));
    Check(fd >= 0, "raw_socket_permission");
    sockaddr_ll target{};
    target.sll_family = AF_PACKET;
    target.sll_ifindex = if_nametoindex(name);
    target.sll_halen = 6;
    std::memset(target.sll_addr, 0xff, 6);
    for (int n = 0; n < 100; ++n) {
        const bool six = n % 2;
        std::string frame(64, '\0');
        std::memset(frame.data(), 0xff, 6);
        frame[6] = 2;
        frame[12] = six ? '\x86' : '\x08';
        frame[13] = six ? '\xdd' : 0;
        frame[14] = six ? '\x60' : '\x45';
        const auto udp = six ? 54 : 34;
        if (six) {
            frame[19] = 10;
            frame[20] = 17;
            frame[21] = 64;
            frame[22] = frame[38] = '\x20';
            frame[37] = 1;
            frame[53] = 2;
        } else {
            frame[17] = 50;
            frame[22] = 64;
            frame[23] = 17;
            frame[26] = frame[30] = 10;
            frame[29] = 1;
            frame[33] = 2;
        }
        frame[udp] = '\xc0';
        frame[udp + 1] = 1;
        frame[udp + 3] = 9;
        frame[udp + 5] = six ? 10 : 30;
        target.sll_protocol = htons(six ? ETH_P_IPV6 : ETH_P_IP);
        Check(sendto(fd, frame.data(), frame.size(), 0, reinterpret_cast<sockaddr*>(&target), sizeof(target)) == 64,
              "fixture_send");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    close(fd);
}
Json WaitPublication(Runtime& runtime, const std::string& id, uint64_t packets = 0) {
    const auto deadline = Clock::now() + std::chrono::seconds(15);
    while (Clock::now() < deadline) {
        auto status = runtime.Status(id);
        if (Text(status, "status") == "failed") {
            std::cerr << Encode(status) << '\n';
            throw std::runtime_error("publisher_failed");
        }
        if (status.HasMember("managed_result") && status["managed_result"].HasMember("publication")) {
            const auto& p = status["managed_result"]["publication"];
            if (p["messages"]["confirmed"].GetUint64() && p["packets"]["confirmed"].GetUint64() >= packets)
                return Parse(Encode(p));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::cerr << Encode(runtime.Status(id)) << '\n';
    throw std::runtime_error("publisher_timeout");
}
int64_t Scalar(Runtime& runtime, const std::string& sql) {
    auto rows = runtime.Query(sql);
    Check(rows.size() == 1 && rows[0]->num_rows() == 1, "query_shape");
    const auto value = rows[0]->column(0)->GetScalar(0).ValueOrDie();
    return value->is_valid ? std::stoll(value->ToString()) : 0;
}
void CheckNpm(Runtime& runtime, const std::string& run, const std::string& family) {
    const auto where = " WHERE __npm_run_id='" + run + "'";
    for (const auto* entity : {"basic_", "session_"}) {
        const auto table = "npm_" + std::string(entity) + family + "_final_v1";
        const auto count = Scalar(runtime, "SELECT SUM(packets_ab+packets_ba) FROM " + table + where);
        const auto bytes = Scalar(runtime, "SELECT SUM(wire_bytes_ab+wire_bytes_ba) FROM " + table + where);
        if (count != 50 || bytes != 3200)
            std::cerr << "npm_family=" << family << " table=" << table << " packets=" << count << " bytes=" << bytes
                      << '\n';
        Check(count == 50 && bytes == 3200, "npm_same_load_mismatch");
    }
}
}  // namespace
int main(int argc, char** argv) {
    try {
        Check(argc == 4, "e2e_arguments");
        const auto plan = Read(argv[3]), profile = Read(Text(plan, "manifest"));
        const auto root = std::filesystem::path(Text(plan, "workspace"));
        const auto topic = Text(plan, "topic"), group = Text(plan, "group");
        packet_sink_real::Broker broker(profile["PLAINTEXT"]);
        broker.Topic(topic);
        Runtime runtime(root / "runtime", KafkaOptions(profile["PLAINTEXT"], root / "provider"), 2);
        const char* libs[] = {"libflowsql_capture_af_packet.so", "libflowsql_netadapter.so"};
        const auto adapter = std::string("{\"db_path\":\"") + (root / "adapter.db").string() + "\"}";
        const char* opts[] = {nullptr, adapter.c_str()};
        Check(!runtime.loader->Load(get_absolute_process_path(), libs, opts, 2) && !runtime.loader->StartAll(),
              "capture_plugins");
        auto* manager = static_cast<IBlockStreamManager*>(runtime.loader->First(IID_BLOCK_STREAM_MANAGER));
        Check(manager, "capture_manager");
        auto capture = Parse("{}");
        auto& a = capture.GetAllocator();
        String(capture, "backend", "af_packet", a);
        rapidjson::Value interfaces(rapidjson::kArrayType);
        interfaces.PushBack(rapidjson::Value(argv[1], a), a);
        capture.AddMember("interfaces", interfaces, a);
        capture.AddMember("buffer_mib", 16, a);
        capture.AddMember("promiscuous", false, a);
        Check(!manager->AddChannel("netadapter", "edge", Encode(capture)) &&
                  !manager->AddChannel("netadapter", "local", Encode(capture)),
              "capture_channels");
        runtime.Call("/channels/kafka/connections/add", profile["PLAINTEXT"]["connection"]);
        auto sink = Parse(
            R"({"name":"out","connection_name":"real","role":"sink","topic":"events","payload_kind":"packet","payload_options":{"sink":{"encoding":"flowsql_packet_v1","stream_id":"probe-a","delivery_timeout_ms":5000}}})");
        String(sink, "topic", topic, sink.GetAllocator());
        runtime.Call("/channels/kafka/channels/add", sink);
        runtime.Submit("publish", "SELECT * FROM netadapter.edge INTO kafka.out");
        const auto ready = WaitPublication(runtime, "publish");
        const auto& binding = ready["binding"];
        auto source = Parse(
            R"({"name":"in","connection_name":"real","role":"source","topic":"events","consumer_group":"analysis","payload_kind":"packet","payload_options":{"source":{"encoding":"flowsql_packet_v1","stream_id":"probe-a"}}})");
        auto& sa = source.GetAllocator();
        String(source, "topic", topic, sa);
        String(source, "consumer_group", group, sa);
        auto& config = source["payload_options"]["source"];
        config.AddMember("observation_domain_id", rapidjson::Value(binding["observation_domain_id"], sa), sa);
        config.AddMember("inputs", rapidjson::Value(binding["inputs"], sa), sa);
        config.AddMember("max_packets_per_batch", binding["max_packets_per_batch"].GetUint(), sa);
        config.AddMember("max_bytes_per_batch", binding["max_bytes_per_batch"].GetUint(), sa);
        config.AddMember("max_message_bytes", binding["max_message_bytes"].GetUint(), sa);
        runtime.Call("/channels/kafka/channels/add", source);
        runtime.Submit(
            "local",
            "SELECT * FROM netadapter.local USING npm.basic WITH features='basic,session' INTO sqlite.results");
        const auto deadline = Clock::now() + std::chrono::seconds(15);
        while (!Scalar(runtime, "SELECT COUNT(*) FROM sqlite_master WHERE name='npm_result_runs'") ||
               Scalar(runtime, "SELECT COUNT(*) FROM npm_result_runs") < 1) {
            Check(Clock::now() < deadline, "npm_start_timeout");
            for (const auto* id : {"local"}) {
                const auto status = runtime.Status(id);
                if (Text(status, "status") == "failed") std::cerr << Encode(status) << '\n';
                Check(Text(status, "status") != "failed", "npm_start_failed");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        Send(argv[2]);
        const auto publication = WaitPublication(runtime, "publish", 100);
        runtime.Status("publish", "stop");
        auto finished = runtime.Terminal("publish");
        Check(Text(finished, "status") == "stopped", "publisher_stop");
        const auto& result = finished["managed_result"]["publication"];
        Check(result["packets"]["confirmed"].GetUint64() == 100 &&
                  result["capture_counters"][0]["source_dropped_packets"].GetUint64() == 0,
              "capture_no_loss");
        runtime.Status("local", "stop");
        const auto local = runtime.Terminal("local");
        if (Text(local, "status") != "stopped") std::cerr << Encode(local) << '\n';
        Check(Text(local, "status") == "stopped", "local_npm_terminal");
        const auto local_run = Text(local["managed_result"], "run_id");
        CheckNpm(runtime, local_run, "ipv4");
        CheckNpm(runtime, local_run, "ipv6");
        // Local and remote runs use the same captured frames, with exclusive SQLite maintenance ownership.
        runtime.Submit("remote",
                       "SELECT * FROM kafka.in USING npm.basic WITH features='basic,session' INTO sqlite.results");
        // Wait for source acknowledgement, not an elapsed-time guess or a fabricated remote EOF.
        auto* factory = static_cast<IKafkaConnectionServiceV1*>(runtime.loader->First(IID_KAFKA_CONNECTION_SERVICE_V1));
        Check(factory, "sessions");
        const auto consumed_deadline = Clock::now() + std::chrono::seconds(15);
        for (;;) {
            auto status = runtime.Status("remote");
            const auto state = Text(status, "status");
            if (state != "running" && state != "pending") std::cerr << Encode(status) << '\n';
            Check(state == "running" || state == "pending", "remote_stopped_on_probe_stop");
            if (status.HasMember("managed_result") && status["managed_result"].HasMember("input_rows") &&
                status["managed_result"]["input_rows"].GetUint64() >= 100)
                break;
            // The source commits each released poll; use the actual group's committed offset below.
            auto* c = rd_kafka_conf_new();
            packet_sink_real::Set(c, "bootstrap.servers",
                                  profile["PLAINTEXT"]["connection"]["bootstrap_servers"][0].GetString());
            packet_sink_real::Set(c, "group.id", group);
            packet_sink_real::Set(c, "log_level", "0");
            char error[512];
            auto* client = rd_kafka_new(RD_KAFKA_CONSUMER, c, error, sizeof(error));
            Check(client, "offset_consumer");
            auto* list = rd_kafka_topic_partition_list_new(1);
            rd_kafka_topic_partition_list_add(list, topic.c_str(), 0);
            const auto rc = rd_kafka_committed(client, list, 2000);
            const auto offset = list->elems[0].offset;
            rd_kafka_topic_partition_list_destroy(list);
            rd_kafka_destroy(client);
            if (!rc && offset >= int64_t(result["messages"]["confirmed"].GetUint64())) break;
            Check(Clock::now() < consumed_deadline, "remote_commit_timeout");
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        for (const auto* id : {"remote"}) {
            runtime.Status(id, "stop");
            const auto terminal = runtime.Terminal(id);
            if (Text(terminal, "status") != "stopped") std::cerr << Encode(terminal) << '\n';
            Check(Text(terminal, "status") == "stopped", "npm_terminal");
            const auto run = Text(terminal["managed_result"], "run_id");
            CheckNpm(runtime, run, "ipv4");
            CheckNpm(runtime, run, "ipv6");
        }
        std::cout << "PASS real AF_PACKET -> Scheduler -> Kafka -> packet source -> NPM; local/remote IPv4/IPv6 "
                     "basic/session 50 packets/3200 bytes each; probe stop leaves remote running\n";
        std::cout << "METRIC e2e_publication=" << Encode(result) << '\n';
        runtime.Submit("fault", "SELECT * FROM netadapter.edge INTO kafka.out");
        const auto reopened = WaitPublication(runtime, "fault");
        Check(reopened["binding"]["stream_epoch"] != binding["stream_epoch"], "reopen_epoch");
        LinkDown(argv[1]);
        const auto failed = runtime.Terminal("fault");
        if (Text(failed, "status") != "failed") std::cerr << Encode(failed) << '\n';
        Check(Text(failed, "status") == "failed" &&
                  Text(failed["managed_result"]["publication"], "stage") == "source_capture",
              "source_failure_stage");
        std::cout << "PASS real device fault -> source_capture, no invented idle/EOF\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL capture e2e stage=" << e.what() << '\n';
        return 1;
    }
}
