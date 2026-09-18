// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <common/loader.hpp>
#include <framework/interfaces/iconfig_channel_registry.h>

#include <httplib.h>
#include <openssl/evp.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <tuple>

namespace {

namespace fs = std::filesystem;

int ReservePort() {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
    socklen_t size = sizeof(address);
    assert(getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) == 0);
    const int port = ntohs(address.sin_port);
    close(fd);
    return port;
}

std::string Base64(const std::string& bytes) {
    std::string encoded(4 * ((bytes.size() + 2) / 3) + 1, '\0');
    const int size = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded.data()),
                                     reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
    assert(size >= 0);
    encoded.resize(static_cast<size_t>(size));
    return encoded;
}

std::string PublishRequest(const std::string& name, uint64_t expected, const std::string& content,
                           const std::string& format = "json", uint64_t base = 0) {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    writer.StartObject();
    writer.Key("name");
    writer.String(name.c_str());
    writer.Key("expected_current_revision");
    writer.Uint64(expected);
    writer.Key("format");
    writer.String(format.c_str());
    writer.Key("schema_id");
    writer.String("e2e-v1");
    writer.Key("content_base64");
    writer.String(Base64(content).c_str());
    if (base) {
        writer.Key("base_revision");
        writer.Uint64(base);
    }
    writer.EndObject();
    return buffer.GetString();
}

rapidjson::Document Parse(const std::string& json) {
    rapidjson::Document document;
    document.Parse(json.c_str());
    assert(!document.HasParseError() && document.IsObject());
    return document;
}

class Runtime {
 public:
    explicit Runtime(const fs::path& root) {
        const int gateway_port = ReservePort();
        int router_port = ReservePort();
        while (router_port == gateway_port) router_port = ReservePort();
        web_port_ = ReservePort();
        while (web_port_ == gateway_port || web_port_ == router_port) web_port_ = ReservePort();
        const std::string gateway = "host=127.0.0.1;port=" + std::to_string(gateway_port);
        const std::string router = "host=127.0.0.1;port=" + std::to_string(router_port) +
                                   ";gateway=127.0.0.1:" + std::to_string(gateway_port);
        const std::string config = "db_path=" + (root / "config.db").string();
        const std::string web = "host=127.0.0.1;port=" + std::to_string(web_port_) +
                                ";gateway=127.0.0.1:" + std::to_string(gateway_port) +
                                ";db_path=:memory:;upload_dir=" + (root / "uploads").string();
        const char* libraries[] = {"libflowsql_gateway.so", "libflowsql_router.so",
                                    "libflowsql_config_channel.so", "libflowsql_web.so"};
        const char* options[] = {gateway.c_str(), router.c_str(), config.c_str(), web.c_str()};
        assert(loader_->Load(flowsql::get_absolute_process_path(), libraries, options, 4) == 0);
        assert(loader_->StartAll() == 0);
        bool ready = false;
        for (int attempt = 0; attempt < 200 && !ready; ++attempt) {
            httplib::Client client("127.0.0.1", web_port_);
            const auto result = client.Post("/api/channels/config/list", "{}", "application/json");
            ready = result && result->status == 200;
            if (!ready) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        assert(ready && "real Web/Gateway/Router/Config chain must become ready");
    }

    ~Runtime() {
        loader_->StopAll();
        assert(loader_->Unload() == 0);
    }

    std::string Post(const std::string& operation, const std::string& request, int status = 200) const {
        httplib::Client client("127.0.0.1", web_port_);
        client.set_read_timeout(10);
        const auto result = client.Post("/api/channels/config/" + operation, request, "application/json");
        if (!result || result->status != status) {
            std::fprintf(stderr, "%s: expected HTTP %d, got %d: %s\n", operation.c_str(), status,
                         result ? result->status : 0, result ? result->body.c_str() : "transport failure");
        }
        assert(result && result->status == status);
        return result->body;
    }

    int PublishStatus(const std::string& request) const {
        httplib::Client client("127.0.0.1", web_port_);
        client.set_read_timeout(10);
        const auto result = client.Post("/api/channels/config/publish", request, "application/json");
        assert(result);
        return result->status;
    }

    flowsql::IConfigChannelRegistryV1* Registry() const {
        auto* registry = static_cast<flowsql::IConfigChannelRegistryV1*>(
            loader_->First(flowsql::IID_CONFIG_CHANNEL_REGISTRY_V1));
        assert(registry);
        return registry;
    }

 private:
    flowsql::PluginLoader* loader_ = flowsql::PluginLoader::Single();
    int web_port_ = 0;
};

// This test consumer resolves and compiles exactly once at task opening; events only read typed memory.
struct BoundTask {
    flowsql::ConfigChannelSnapshot snapshot;
    uint64_t value = 0;
    unsigned resolve_calls = 0;

    void Open(flowsql::IConfigChannelRegistryV1* registry) {
        std::string error;
        ++resolve_calls;
        assert(registry->Resolve("config.rules@1", &snapshot, &error) == 0);
        assert(snapshot.schema_id == "e2e-v1" && snapshot.format == "json");
        value = Parse(*snapshot.content)["value"].GetUint64();
    }

    uint64_t ProcessEvent() const { return value; }
};

std::string SnapshotRequest(uint64_t revision) {
    return "{\"exact_reference\":\"config.rules@" + std::to_string(revision) + "\"}";
}

void Save(const fs::path& path, const std::string& value) {
    std::ofstream output(path, std::ios::binary);
    output << value;
    assert(output.good());
}

std::string Read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    assert(input.is_open());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void Seed(const fs::path& root) {
    BoundTask task;
    {
        Runtime runtime(root);
        const std::string first = PublishRequest("rules", 0, R"({"value":1})");
        assert(Parse(runtime.Post("publish", first))["revision"].GetUint64() == 1);
        task.Open(runtime.Registry());
        assert(!Parse(runtime.Post("publish", first))["created_revision"].GetBool());

        std::atomic<int> ready{0};
        std::atomic<bool> go{false};
        int statuses[2] = {0, 0};
        // Query response status directly because either request may be the winner.
        auto race = [&](int index) {
            ready.fetch_add(1);
            while (!go.load()) std::this_thread::yield();
            const std::string content = "{\"value\":" + std::to_string(index + 2) + "}";
            statuses[index] = runtime.PublishStatus(PublishRequest("rules", 1, content));
        };
        std::thread a(race, 0);
        std::thread b(race, 1);
        while (ready.load() != 2) std::this_thread::yield();
        go.store(true);
        a.join();
        b.join();
        assert((statuses[0] == 200 && statuses[1] == 409) || (statuses[0] == 409 && statuses[1] == 200));
        runtime.Post("publish", PublishRequest("rules", 2, R"({"bad":1,"bad":2})"), 400);
        runtime.Post("publish", PublishRequest("rules", 2, std::string(512 * 1024 + 1, 'x')), 413);
        assert(Parse(runtime.Post("publish", PublishRequest("rules", 2, R"({"value":1})", "json", 1)))
                   ["revision"].GetUint64() == 3);
        for (const auto& [name, format, content] : {
                 std::tuple{"yaml-rules", "yaml", "value: 1\n"},
                 std::tuple{"xml-rules", "xml", "<root><value>1</value></root>"}}) {
            assert(Parse(runtime.Post("publish", PublishRequest(name, 0, content, format)))
                       ["revision"].GetUint64() == 1);
        }
        const std::string max_content = "\"" + std::string(512 * 1024 - 2, 'x') + "\"";
        runtime.Post("publish", PublishRequest("maximum", 0, max_content));
        runtime.Post("list", std::string(1024 * 1024 + 1, ' '), 413);
        runtime.Post("resolve", R"({"exact_reference":"config.rules@latest"})", 400);
        runtime.Post("resolve", SnapshotRequest(4), 404);
        const auto page = Parse(runtime.Post("list", R"({"limit":1})"));
        assert(page["items"].Size() == 1 && !page["items"][0].HasMember("content_base64"));
        assert(std::string(page["next_cursor"].GetString()).size() > 0);
        const auto history = Parse(runtime.Post("history", R"({"name":"rules"})"));
        assert(history["items"].Size() == 3 && history["items"][0]["base_revision"].GetUint64() == 1);
        for (uint64_t revision = 1; revision <= 3; ++revision) {
            Save(root / ("revision-" + std::to_string(revision)), runtime.Post("resolve", SnapshotRequest(revision)));
        }
        Save(root / "history", runtime.Post("history", R"({"name":"rules"})"));
        Save(root / "list", runtime.Post("list", "{}"));
        assert(task.resolve_calls == 1 && task.ProcessEvent() == 1);
    }
    assert(task.ProcessEvent() == 1 && task.resolve_calls == 1 && *task.snapshot.content == R"({"value":1})");
}

void Recover(const fs::path& root) {
    Runtime runtime(root);
    assert(runtime.Post("list", "{}") == Read(root / "list"));
    assert(runtime.Post("history", R"({"name":"rules"})") == Read(root / "history"));
    for (uint64_t revision = 1; revision <= 3; ++revision) {
        assert(runtime.Post("resolve", SnapshotRequest(revision)) ==
               Read(root / ("revision-" + std::to_string(revision))));
    }
    BoundTask task;
    task.Open(runtime.Registry());
    assert(task.ProcessEvent() == 1 && task.resolve_calls == 1);
    assert(Parse(runtime.Post("publish", PublishRequest("rules", 3, R"({"value":4})")))
               ["revision"].GetUint64() == 4);
    assert(runtime.Post("resolve", SnapshotRequest(1)) == Read(root / "revision-1"));
    for (int event = 0; event < 1000; ++event) assert(task.ProcessEvent() == 1);
    assert(task.resolve_calls == 1);
}

void RunChild(const fs::path& root, const char* phase) {
    const std::string executable = fs::canonical("/proc/self/exe").string();
    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        execl(executable.c_str(), executable.c_str(), phase, root.c_str(), nullptr);
        _exit(127);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3) {
        if (std::string(argv[1]) == "seed") Seed(argv[2]);
        else if (std::string(argv[1]) == "recover") Recover(argv[2]);
        else return 2;
        return 0;
    }
    char pattern[] = "/tmp/flowsql-config-e2e-XXXXXX";
    assert(mkdtemp(pattern));
    const fs::path root(pattern);
    RunChild(root, "seed");
    RunChild(root, "recover");
    fs::remove_all(root);
    std::puts("Config Channel real HTTP/process-restart/immutable-consumer E2E passed");
    return 0;
}
