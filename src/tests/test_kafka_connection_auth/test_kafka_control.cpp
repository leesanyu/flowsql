// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <channels/kafka/kafka_plugin.h>
#include <common/error_code.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <map>
#include <thread>
using namespace flowsql::channels::kafka;
namespace {
std::string Read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}
void TestDefaultDeployment() {
    const char* previous = getenv("FLOWSQL_KAFKA_STATE_DIR");
    const std::string saved = previous ? previous : "";
    const auto cwd = std::filesystem::current_path();
    const auto root = std::filesystem::temp_directory_path() / ("kafka-default-" + std::to_string(getpid()));
    const auto state = root / "state";
    std::filesystem::create_directories(root / "first");
    std::filesystem::create_directories(root / "second");
    assert(setenv("FLOWSQL_KAFKA_STATE_DIR", state.c_str(), 1) == 0);
    std::filesystem::current_path(root / "first");
    {
        Plugin plugin;
        assert(plugin.Option(nullptr) == 0 && plugin.Load(nullptr) == 0);
        plugin.Definitions().Mutate(
            "connections", "add",
            Parse(
                R"({"name":"persisted","bootstrap_servers":["localhost:1"],"security_protocol":"SASL_PLAINTEXT","sasl_mechanism":"PLAIN","sasl":{"username":"test"},"credential_bindings":{"password":{"operation":"replace","value":"bootstrap-secret"}}})"));
        // Two startup modes must not concurrently own the same default database.
        Plugin competing;
        assert(competing.Option(nullptr) != 0);
    }
    const auto key_path = state / "secrets/kafka-master.key";
    const auto config_path = state / "config/kafka/provider.json";
    const auto key = Read(key_path), configuration = Read(config_path);
    assert(key.size() == 32);
    for (const auto& path : {state, state / "secrets", state / "runtime/kafka"}) {
        struct stat info {};
        assert(stat(path.c_str(), &info) == 0 && (info.st_mode & 0777) == 0700);
    }
    struct stat info {};
    assert(stat(key_path.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600);
    std::filesystem::current_path(root / "second");
    {
        Plugin restored;
        assert(restored.Option("{}") == 0 && restored.Load(nullptr) == 0);
        auto binding = restored.Definitions().AcquireConnection("persisted");
        assert(binding.secrets.at("password") == "bootstrap-secret");
        restored.Definitions().Release(binding.snapshot);
    }
    assert(Read(key_path) == key && Read(config_path) == configuration);
    // Existing state never silently receives a replacement key or default configuration.
    std::filesystem::rename(key_path, key_path.string() + ".saved");
    {
        Plugin missing;
        assert(missing.Option(nullptr) != 0 && !std::filesystem::exists(key_path));
    }
    std::filesystem::rename(key_path.string() + ".saved", key_path);
    std::ofstream(key_path, std::ios::binary).write("broken", 6);
    {
        Plugin broken;
        assert(broken.Option(nullptr) != 0 && Read(key_path) == "broken");
    }
    std::ofstream(key_path, std::ios::binary).write(key.data(), key.size());
    {
        const std::string wrong_key(32, 'x');
        std::ofstream(key_path, std::ios::binary).write(wrong_key.data(), wrong_key.size());
        Plugin mismatched;
        assert(mismatched.Option(nullptr) == 0 && mismatched.Load(nullptr) != 0);
        assert(Read(key_path) == wrong_key);
    }
    std::ofstream(key_path, std::ios::binary).write(key.data(), key.size());
    std::filesystem::rename(config_path, config_path.string() + ".saved");
    {
        Plugin missing;
        assert(missing.Option(nullptr) != 0 && !std::filesystem::exists(config_path));
    }
    std::filesystem::rename(config_path.string() + ".saved", config_path);
    std::ofstream(config_path) << "{invalid";
    {
        Plugin broken;
        assert(broken.Option(nullptr) != 0 && Read(config_path) == "{invalid");
    }
    std::ofstream(config_path) << configuration;
    assert(setenv("FLOWSQL_KAFKA_STATE_DIR", "relative/path", 1) == 0);
    {
        Plugin relative;
        assert(relative.Option(nullptr) != 0);
    }
    // Explicit deployment remains strict and must never fall back to generated defaults.
    {
        Plugin explicit_config;
        assert(explicit_config.Option("config_file=/missing/flowsql-provider.json") != 0);
    }
    const char* previous_xdg = getenv("XDG_STATE_HOME");
    const std::string saved_xdg = previous_xdg ? previous_xdg : "";
    assert(unsetenv("FLOWSQL_KAFKA_STATE_DIR") == 0);
    assert(setenv("XDG_STATE_HOME", (root / "xdg").c_str(), 1) == 0);
    {
        Plugin xdg;
        assert(xdg.Option("") == 0 && xdg.Load(nullptr) == 0);
        const auto other_key = Read(root / "xdg/flowsql/kafka/secrets/kafka-master.key");
        assert(other_key.size() == 32 && other_key != key);
    }
    if (previous_xdg)
        assert(setenv("XDG_STATE_HOME", saved_xdg.c_str(), 1) == 0);
    else
        assert(unsetenv("XDG_STATE_HOME") == 0);
    if (previous)
        assert(setenv("FLOWSQL_KAFKA_STATE_DIR", saved.c_str(), 1) == 0);
    else
        assert(unsetenv("FLOWSQL_KAFKA_STATE_DIR") == 0);
    std::filesystem::current_path(cwd);
    std::filesystem::remove_all(root);
}
}  // namespace
int main() {
    TestDefaultDeployment();
    auto root = std::filesystem::temp_directory_path() / ("kafka-control-" + std::to_string(getpid()));
    std::filesystem::create_directory(root);
    std::ofstream(root / "key", std::ios::binary).write("12345678901234567890123456789012", 32);
    Plugin plugin;
    auto options = Parse("{}");
    String(options, "db_path", (root / "db").string(), options.GetAllocator());
    String(options, "master_key_file", (root / "key").string(), options.GetAllocator());
    String(options, "runtime_directory", (root / "runtime").string(), options.GetAllocator());
    std::ofstream(root / "provider.json") << Encode(options);
    assert(plugin.Option(("config_file=" + (root / "provider.json").string()).c_str()) == 0 &&
           plugin.Load(nullptr) == 0);
    std::map<std::string, flowsql::fnRouterHandler> routes;
    plugin.EnumRoutes([&](const flowsql::RouteItem& route) { routes[route.uri] = route.handler; });
    auto call = [&](const char* operation, const std::string& request, int expected = 0) {
        auto path = std::string("/channels/kafka/") + operation;
        std::string response;
        assert(routes.count(path));
        int result = routes[path](path, request, response);
        if (result != expected) std::fprintf(stderr, "%s: %d %s\n", operation, result, response.c_str());
        assert(result == expected);
        return Parse(response);
    };
    auto capabilities = call("definitions/query", "{}");
    assert(capabilities.HasMember("sasl_mechanisms"));
    assert(capabilities["adapter_status"]["structured"]["sink"] == "available");
    assert(capabilities["adapter_status"]["packet"]["sink"] == "available");
    assert(capabilities["structured_sink"]["write_permission"] == "unverified");
    assert(capabilities["structured_sink"]["types"].Size() == 15);
    assert(capabilities["structured_source"]["requires_async"].GetBool());
    assert(capabilities["structured_source"]["types"].Size() == 5);
    const std::string connection =
        R"({"name":"offline","bootstrap_servers":["localhost:1"],"security_protocol":"SASL_SSL","sasl_mechanism":"PLAIN","sasl":{"username":"tester"},"credential_bindings":{"password":{"operation":"replace","value":"never-print-this-secret"}}})";
    auto saved = call("connections/add", connection);
    assert(Encode(saved).find("never-print") == std::string::npos);
    call(
        "channels/add",
        R"({"name":"events","connection_name":"offline","role":"both","topic":"events","consumer_group":"test","payload_kind":"structured"})");
    flowsql::KafkaChannelDescriptionV1 description;
    assert(plugin.DescribeChannel("events", &description) == 0 && description.role == "both" &&
           !description.source_adapter_available);
    auto id =
        Text(call("tests/start", R"({"target":"channel","name":"events","expected_revision":1,"timeout_ms":30000})",
                  flowsql::error::ACCEPTED),
             "test_id");
    call("channels/remove", R"({"name":"events","expected_revision":1})", flowsql::error::CONFLICT);
    auto cancel = std::string("{\"test_id\":\"") + id + "\"}";
    call("tests/cancel", cancel);
    Json terminal;
    for (int i = 0; i < 300; ++i) {
        terminal = call("tests/query", cancel);
        if (Text(terminal, "status") != "running") break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(Text(terminal, "status") == "failed" && Text(terminal, "code") == "CANCELLED");
    call("tests/cancel", cancel);
    // Draft keep decrypts the saved secret without changing definitions or revision.
    auto draft = Parse(connection);
    draft.AddMember("expected_revision", 1, draft.GetAllocator());
    draft["credential_bindings"]["password"].RemoveMember("value");
    String(draft["credential_bindings"]["password"], "operation", "keep", draft.GetAllocator());
    auto request = Parse(R"({"target":"connection","timeout_ms":1})");
    request.AddMember("draft", rapidjson::Value(draft, request.GetAllocator()), request.GetAllocator());
    call("tests/start", Encode(request), flowsql::error::ACCEPTED);
    assert(call("connections/query", "{}")["items"][0]["revision"].GetUint64() == 1);
    auto unchanged = plugin.Definitions().AcquireConnection("offline");
    assert(unchanged.secrets.at("password") == "never-print-this-secret");
    plugin.Definitions().Release(unchanged.snapshot);
    auto held = plugin.Definitions().Acquire("events", "source");
    auto draft_channel =
        R"({"target":"channel","timeout_ms":1,"draft":{"name":"events","connection_name":"offline","role":"source","topic":"events","consumer_group":"test","payload_kind":"structured"}})";
    auto draft_id = Text(call("tests/start", draft_channel, flowsql::error::ACCEPTED), "test_id");
    for (int i = 0; i < 100; ++i) {
        auto result = call("tests/query", "{\"test_id\":\"" + draft_id + "\"}");
        if (Text(result, "status") != "running") break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(call("channels/query", "{}")["items"][0]["in_use"].GetBool());
    plugin.Definitions().Release(held.snapshot);
    call("connections/query", R"({"limit":0})", flowsql::error::BAD_REQUEST);
    call("connections/query", R"({"name":"offline","name":"offline"})", flowsql::error::BAD_REQUEST);
    call(
        "connections/add",
        R"({"name":"unknown","bootstrap_servers":["localhost:1"],"security_protocol":"SSL","tls":{"ca_ref":"file:unmapped"}})",
        flowsql::error::BAD_REQUEST);
    auto update = Parse(connection);
    update.AddMember("expected_revision", 1, update.GetAllocator());
    update["credential_bindings"]["password"].RemoveMember("value");
    String(update["credential_bindings"]["password"], "operation", "keep", update.GetAllocator());
    assert(call("connections/modify", Encode(update))["revision"].GetUint64() == 2);
    assert(Text(call("channels/query", "{}")["items"][0]["verification"], "status") == "unverified");
    call("connections/remove", R"({"name":"offline","expected_revision":2})", flowsql::error::CONFLICT);
    auto both = Parse(
        R"({"name":"events","expected_revision":1,"connection_name":"offline","role":"both","topic":"events","consumer_group":"test","payload_kind":"structured","payload_options":{"source":{"encoding":"json","entity":"metric","fields":[{"name":"value","path":"/v","type":"int64","nullable":false}]},"sink":{"encoding":"json","entity":"alert","schema_version":1,"record_kind":"upsert","key_fields":["id"],"revision_field":"revision"}}})");
    auto configured = call("channels/modify", Encode(both));
    assert(configured["revision"].GetUint64() == 2);
    assert(configured["payload_options"]["source"]["entity"] == "metric");
    assert(configured["payload_options"]["sink"]["entity"] == "alert");
    assert(configured["adapter_status"]["source"] == "ready" && configured["adapter_status"]["sink"] == "ready");
    assert(plugin.DescribeChannel("events", &description) == 0 && description.source_adapter_available &&
           description.sink_adapter_available);
    call("channels/modify", Encode(both), flowsql::error::CONFLICT);
    auto sink_lease = plugin.Definitions().Acquire("events", "sink");
    both["expected_revision"].SetUint64(2);
    call("channels/modify", Encode(both), flowsql::error::CONFLICT);
    assert(!sink_lease.sink_options_json.empty() && sink_lease.snapshot.channel_revision == 2);
    plugin.Definitions().Release(sink_lease.snapshot);
    auto back = call("channels/query", "{}");
    assert(back["items"][0]["payload_options"]["sink"]["key_fields"][0] == "id");
    assert(Encode(back).find("never-print") == std::string::npos);

    assert(plugin.Stop() == 0 && plugin.Unload() == 0);
    std::filesystem::remove_all(root);
    puts("PASS: control contracts, encrypted draft keep, leases, asynchronous cancellation and discovery");
}
