// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <arpa/inet.h>
#include <channels/kafka/kafka_store.h>
#include <framework/interfaces/ikafka_connection_service.h>
#include <httplib.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cassert>
#include <common/loader.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <thread>
using namespace flowsql;
using namespace flowsql::channels::kafka;
namespace {
int Port() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    socklen_t size = sizeof(address);
    assert(getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) == 0);
    close(fd);
    return ntohs(address.sin_port);
}
class Runtime {
 public:
    Runtime(const std::filesystem::path& root) {
        std::set<int> ports;
        while (ports.size() < 3) ports.insert(Port());
        auto it = ports.begin();
        int gateway = *it++, router = *it++;
        gateway_ = gateway;
        web_ = *it;
        std::ofstream(root / "key", std::ios::binary).write("12345678901234567890123456789012", 32);
        auto options = Parse("{}");
        auto& a = options.GetAllocator();
        String(options, "db_path", (root / "kafka.db").string(), a);
        String(options, "master_key_file", (root / "key").string(), a);
        String(options, "runtime_directory", (root / "runtime").string(), a);
        rapidjson::Value aliases(rapidjson::kObjectType);
        for (auto [name, variable] : {std::pair{"ca", "FLOWSQL_KAFKA_CA"},
                                      {"certificate", "FLOWSQL_KAFKA_CERT"},
                                      {"key", "FLOWSQL_KAFKA_KEY"},
                                      {"keytab", "FLOWSQL_KAFKA_KEYTAB"},
                                      {"oauth_ca", "FLOWSQL_KAFKA_OAUTH_CA"}})
            if (const char* path = getenv(variable)) String(aliases, name, path, a);
        options.AddMember("files", aliases, a);
        auto provider = Encode(options);
        std::string gateway_options = "host=127.0.0.1;port=" + std::to_string(gateway);
        std::string router_options =
            "host=127.0.0.1;port=" + std::to_string(router) + ";gateway=127.0.0.1:" + std::to_string(gateway);
        std::string web_options = "host=127.0.0.1;port=" + std::to_string(web_) +
                                  ";gateway=127.0.0.1:" + std::to_string(gateway) +
                                  ";db_path=:memory:;upload_dir=" + (root / "uploads").string();
        const char* libraries[] = {"libflowsql_gateway.so", "libflowsql_router.so",    "libflowsql_kafka.so",
                                   "libflowsql_web.so",     "libflowsql_scheduler.so", "libflowsql_builtin.so",
                                   "libflowsql_catalog.so"};
        const auto catalog_options =
            "data_dir=" + (root / "frames").string() + ";operator_db_path=" + (root / "operators.db").string();
        const char* config[] = {gateway_options.c_str(),
                                router_options.c_str(),
                                provider.c_str(),
                                web_options.c_str(),
                                "stream_workers=1;batch_workers=1",
                                nullptr,
                                catalog_options.c_str()};
        assert(loader_->Load(get_absolute_process_path(), libraries, config, 7) == 0);
        assert(loader_->StartAll() == 0);
        bool ready = false;
        for (int i = 0; i < 300 && !ready; ++i) {
            httplib::Client client("127.0.0.1", web_);
            auto response = client.Post("/api/channels/kafka/definitions/query", "{}", "application/json");
            ready = response && response->status == 200;
            if (!ready) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        assert(ready);
    }
    ~Runtime() {
        loader_->StopAll();
        assert(loader_->Unload() == 0);
    }
    Json Post(const std::string& operation, const std::string& json, int expected = 200) {
        return Path("/api/channels/kafka/" + operation, json, expected);
    }
    Json Path(const std::string& path, const std::string& json, int expected) {
        httplib::Client client("127.0.0.1", path.rfind("/scheduler/", 0) == 0 ? gateway_ : web_);
        client.set_read_timeout(30);
        auto response = client.Post(path, json, "application/json");
        if (!response || response->status != expected)
            std::cerr << path << " expected=" << expected << " got=" << (response ? response->status : 0) << " "
                      << (response ? response->body : "transport") << std::endl;
        assert(response && response->status == expected);
        return Parse(response->body);
    }
    Json Wait(const std::string& id) {
        for (int i = 0; i < 4000; ++i) {
            auto result = Post("tests/query", "{\"test_id\":\"" + id + "\"}");
            if (Text(result, "status") != "running") return result;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        assert(false);
        return Parse("{}");
    }
    IKafkaConnectionServiceV1* Service() {
        return static_cast<IKafkaConnectionServiceV1*>(loader_->First(IID_KAFKA_CONNECTION_SERVICE_V1));
    }

 private:
    PluginLoader* loader_ = PluginLoader::Single();
    int web_ = 0, gateway_ = 0;
};
}  // namespace
int main() {
    auto root = std::filesystem::temp_directory_path() / ("flowsql-kafka-http-" + std::to_string(getpid()));
    std::filesystem::create_directory(root);
    {
        Runtime runtime(root);
        auto capabilities = runtime.Post("definitions/query", "{}");
        assert(Text(capabilities, "client_version") == "2.16.0");
        for (auto& mechanism : capabilities["sasl_mechanisms"].GetArray()) assert(mechanism["available"].GetBool());
        runtime.Post("connections/add",
                     R"({"name":"offline","bootstrap_servers":["localhost:1"],"security_protocol":"PLAINTEXT"})");
        runtime.Post(
            "channels/add",
            R"({"name":"events","connection_name":"offline","role":"source","topic":"events","consumer_group":"e2e","payload_kind":"structured"})");
        auto description = runtime.Post("channels/query", "{}");
        assert(Text(description["items"][0]["adapter_status"], "source") == "configuration_required");
        auto service = runtime.Service();
        assert(service);
        IKafkaClientSessionV1* session = nullptr;
        std::string error;
        assert(service->OpenSession("events", "sink", &session, &error) != 0 && !session);
        assert(service->OpenSession("events", "source", &session, &error) == 0 && session);
        runtime.Post("channels/remove", R"({"name":"events","expected_revision":1})", 409);
        service->ReleaseSession(session);
        std::vector<std::string> ids;
        for (int i = 0; i < 8; ++i)
            ids.push_back(
                Text(runtime.Post("tests/start", R"({"target":"channel","name":"events","timeout_ms":30000})", 202),
                     "test_id"));
        runtime.Post("tests/start", R"({"target":"channel","name":"events","timeout_ms":30000})", 429);
        for (auto& id : ids) runtime.Post("tests/cancel", "{\"test_id\":\"" + id + "\"}");
        for (auto& id : ids) assert(Text(runtime.Wait(id), "code") == "CANCELLED");
        assert(!runtime.Post("channels/query", "{}")["items"][0]["in_use"].GetBool());
        assert(Text(runtime.Path("/scheduler/batch/execute", R"({"sql":"SELECT * FROM kafka.events"})", 503),
                    "error_code") == "KAFKA_DATA_ADAPTER_UNAVAILABLE");
        runtime.Post(
            "channels/modify",
            R"({"name":"events","expected_revision":1,"connection_name":"offline","role":"source","topic":"events","consumer_group":"e2e","payload_kind":"structured","payload_options":{"source":{"encoding":"json","entity":"metrics","fields":[{"name":"value","path":"/value","type":"float64","nullable":false,"unit":"bytes"}]}}})");
        auto configured = runtime.Post("channels/query", R"({"name":"events"})");
        assert(Text(configured["items"][0]["adapter_status"], "source") == "ready");
        assert(Text(configured["items"][0], "source_schema").find("value") != std::string::npos);
        auto classification = runtime.Path("/scheduler/sql/classify", R"({"sql":"SELECT * FROM kafka.events"})", 200);
        assert(classification["requires_async"].GetBool());
        // Binding the source filter must use the configured entity Schema and reject unknown fields.
        runtime.Path("/scheduler/batch/execute", R"({"sql":"SELECT * FROM kafka.events WHERE missing > 0"})", 400);
        assert(Text(runtime.Path("/scheduler/batch/execute",
                                 R"({"sql":"SELECT * FROM dataframe.unused INTO kafka.events"})", 400),
                    "error_code") == "KAFKA_ROLE_MISMATCH");
        runtime.Post("channels/remove", R"({"name":"events","expected_revision":99})", 409);
        if (const char* fixture = getenv("FLOWSQL_KAFKA_CONNECTION_JSON")) {
            std::ifstream input(fixture);
            std::string json((std::istreambuf_iterator<char>(input)), {});
            runtime.Post("connections/add", json);
            auto id = Text(runtime.Post("tests/start",
                                        std::string(R"({"target":"connection","name":"real","topic":")") +
                                            (getenv("FLOWSQL_KAFKA_TOPIC") ? getenv("FLOWSQL_KAFKA_TOPIC") : "events") +
                                            R"(","timeout_ms":10000})",
                                        202),
                           "test_id");
            auto verified = runtime.Wait(id);
            std::cout << Encode(verified) << std::endl;
            assert(Text(verified, "status") == "passed");
        }
        runtime.Post("channels/remove", R"({"name":"events","expected_revision":2})");
        runtime.Post("connections/remove", R"({"name":"offline","expected_revision":1})");
    }
    {
        Runtime restored(root);
        assert(restored.Post("channels/query", "{}")["items"].Empty());
    }
    std::filesystem::remove_all(root);
    puts("PASS: real Web/Gateway/Router/Kafka HTTP, 202/429, IID leases, role and adapter SQL boundaries, restart");
}
