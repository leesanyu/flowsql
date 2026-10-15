// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <channels/kafka/kafka_plugin.h>
#include <common/error_code.h>
#include <unistd.h>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <map>
#include <thread>
using namespace flowsql::channels::kafka;
int main() {
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
    assert(capabilities["adapter_status"]["packet"]["sink"] == "not_installed");
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
