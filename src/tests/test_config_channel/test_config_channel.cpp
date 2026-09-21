// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <channels/config/config_channel_provider.h>
#include <channels/config/config_channel_plugin.h>
#include <common/loader.hpp>
#include <common/error_code.h>

#include <rapidjson/document.h>
#include <sqlite3.h>

#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <unistd.h>

using flowsql::ConfigChannelSnapshot;
using flowsql::channels::config::ConfigChannelProvider;
using flowsql::channels::config::ConfigChannelListItem;
using flowsql::channels::config::ConfigPublishRequest;
using flowsql::channels::config::ConfigPublishResult;
using flowsql::channels::config::ConfigRevisionListItem;

namespace {

constexpr size_t kMaxContentBytes = 8 * 1024 * 1024;
constexpr size_t kMaxControlRequestBytes = 12 * 1024 * 1024;

std::string NestedJson(size_t depth) {
    std::string value;
    value.reserve(depth * 6 + 1);
    for (size_t i = 0; i < depth; ++i) value += "{\"a\":";
    value += '0';
    value.append(depth, '}');
    return value;
}

std::string NestedYaml(size_t depth) {
    return std::string(depth, '[') + "0" + std::string(depth, ']');
}

std::string NestedXml(size_t depth) {
    std::string value;
    value.reserve(depth * 7 + 1);
    for (size_t i = 0; i < depth; ++i) value += "<n>";
    value += 'x';
    for (size_t i = 0; i < depth; ++i) value += "</n>";
    return value;
}

}  // namespace

int main() {
    char name[] = "/tmp/flowsql_config_t1_XXXXXX";
    const int fd = mkstemp(name);
    assert(fd >= 0);
    close(fd);
    const std::string path(name);
    std::string error;
    ConfigChannelProvider provider;
    assert(provider.Open(path, &error) == 0);

    ConfigPublishRequest validation;
    validation.name = "validation";
    validation.schema_id = "syntax-v1";
    ConfigPublishResult validation_result;
    const std::vector<std::pair<std::string, std::string>> rejected = {
        {"json", "{\"a\":1,\"a\":2}"},
        {"json", NestedJson(65)},
        {"json", NestedJson(10000)},
        {"yaml", "a: 1\na: 2\n"},
        {"yaml", "a: 1\n!!str a: 2\n"},
        {"yaml", "a: &shared 1\nb: *shared\n"},
        {"yaml", "a: !custom value\n"},
        {"yaml", "a: !!python/object/apply:os.system value\n"},
        {"yaml", NestedYaml(65)},
        {"yaml", NestedYaml(10000)},
        {"xml", "<!DOCTYPE root><root/>"},
        {"xml", "<!DOCTYPE root [<!ENTITY x SYSTEM 'file:///etc/passwd'>]><root>&x;</root>"},
        {"xml", "<root xmlns:x='http://www.w3.org/2001/XInclude'><x:include href='file'/></root>"},
        {"xml", "<?fetch resource?><root/>"},
        {"xml", "<root>&undefined;</root>"},
        {"xml", std::string("<root/>") + std::string(1, '\0') + "<extra/>"},
        {"xml", "<a/><b/>"},
        {"xml", NestedXml(65)},
        {"xml", NestedXml(10000)},
    };
    for (const auto& [format, content] : rejected) {
        validation.format = format;
        validation.content = content;
        const int rc = provider.Publish(validation, &validation_result, &error);
        if (rc != EINVAL) {
            std::fprintf(stderr, "unexpected validation result for %s: %s\n", format.c_str(), content.c_str());
        }
        assert(rc == EINVAL);
    }
    validation.format = "json";
    validation.content = NestedJson(64);
    assert(provider.Publish(validation, &validation_result, &error) == 0);
    assert(validation_result.revision == 1);
    validation.name = "valid-json-string-depth";
    validation.content = std::string(R"({"text":")") + std::string(100, '[') + R"("})";
    assert(provider.Publish(validation, &validation_result, &error) == 0);
    validation.name = "valid-yaml";
    validation.format = "yaml";
    validation.content = "root:\n  values: [1, 2, 3]\n";
    assert(provider.Publish(validation, &validation_result, &error) == 0);
    assert(validation_result.revision == 1);
    validation.name = "valid-xml";
    validation.format = "xml";
    validation.content = "<?xml version='1.0'?><root><value>1</value></root>";
    assert(provider.Publish(validation, &validation_result, &error) == 0);
    assert(validation_result.revision == 1);
    validation.name = "valid-yaml-depth";
    validation.format = "yaml";
    validation.content = NestedYaml(64);
    assert(provider.Publish(validation, &validation_result, &error) == 0);
    validation.name = "valid-xml-depth";
    validation.format = "xml";
    validation.content = NestedXml(64);
    assert(provider.Publish(validation, &validation_result, &error) == 0);
    validation.name = "valid-xml-entities";
    validation.content = "<root attr='&quot;'>&amp;&#65;&#x4e2d;<![CDATA[&raw;<n>]]></root>";
    assert(provider.Publish(validation, &validation_result, &error) == 0);
    validation.name = "valid-max-size";
    validation.format = "json";
    validation.content = std::string(R"({"pad":")") + std::string(kMaxContentBytes - 10, 'x') + R"("})";
    assert(validation.content.size() == kMaxContentBytes);
    assert(provider.Publish(validation, &validation_result, &error) == 0);
    ConfigChannelSnapshot maximum;
    assert(provider.Resolve("config.valid-max-size@1", &maximum, &error) == 0);
    assert(maximum.content_bytes == kMaxContentBytes && *maximum.content == validation.content);
    assert(maximum.sha256_hex == validation_result.sha256_hex && maximum.sha256_hex.size() == 64);

    ConfigPublishRequest request;
    request.name = "rules";
    request.format = "json";
    request.schema_id = "rules-v1";
    request.content = "{}";
    ConfigPublishResult result;
    assert(provider.Publish(request, &result, &error) == 0);
    assert(result.created_revision && result.revision == 1 && result.sha256_hex.size() == 64);
    const std::string original_hash = result.sha256_hex;
    ConfigChannelSnapshot held;
    assert(provider.Resolve("config.rules@1", &held, &error) == 0);
    assert(held.revision == 1 && held.schema_id == "rules-v1" && held.format == "json");
    assert(held.content_bytes == 2 && *held.content == "{}" && held.sha256_hex == original_hash);

    assert(provider.Publish(request, &result, &error) == 0);
    assert(!result.created_revision && result.revision == 1);
    request.expected_current_revision = 1;
    request.content = "{\"version\":2}";
    assert(provider.Publish(request, &result, &error) == 0);
    assert(result.created_revision && result.revision == 2);
    assert(*held.content == "{}" && held.sha256_hex == original_hash);

    request.content = "{\"different\":true}";
    assert(provider.Publish(request, &result, &error) == EAGAIN);
    assert(result.revision == 0);
    request.name = "INVALID";
    assert(provider.Publish(request, &result, &error) == EINVAL);
    request.name = "rules";
    request.content.assign(kMaxContentBytes + 1, 'x');
    assert(provider.Publish(request, &result, &error) == EFBIG);
    ConfigChannelSnapshot unchanged;
    assert(provider.Resolve("config.rules@2", &unchanged, &error) == 0);
    assert(*unchanged.content == R"({"version":2})");
    assert(provider.Resolve("config.rules@3", &unchanged, &error) == ENOENT);
    request.content.assign(1, static_cast<char>(0xff));
    assert(provider.Publish(request, &result, &error) == EINVAL);
    request.content = "{\"restore\":1}";
    request.expected_current_revision = 2;
    request.base_revision = 1;
    request.change_note = "restore";
    assert(provider.Publish(request, &result, &error) == 0);
    assert(result.revision == 3);

    sqlite3* reader = nullptr;
    assert(sqlite3_open_v2(path.c_str(), &reader, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK);
    sqlite3_stmt* base = nullptr;
    assert(sqlite3_prepare_v2(reader, "SELECT base_revision FROM config_channel_revision WHERE name='rules' "
                                      "AND revision=3", -1, &base, nullptr) == SQLITE_OK);
    assert(sqlite3_step(base) == SQLITE_ROW && sqlite3_column_int(base, 0) == 1);
    sqlite3_finalize(base);
    assert(sqlite3_exec(reader, "CREATE TRIGGER deny_revision BEFORE INSERT ON config_channel_revision "
                                "BEGIN SELECT RAISE(ABORT,'denied'); END", nullptr, nullptr, nullptr) == SQLITE_OK);
    request.expected_current_revision = 3;
    request.base_revision = 0;
    request.content = "{\"failed\":true}";
    assert(provider.Publish(request, &result, &error) == EIO);
    assert(sqlite3_exec(reader, "DROP TRIGGER deny_revision", nullptr, nullptr, nullptr) == SQLITE_OK);
    assert(provider.Publish(request, &result, &error) == 0 && result.revision == 4);
    sqlite3_close(reader);

    ConfigChannelSnapshot snapshot;
    for (const char* invalid : {"rules@1", "config.rules", "config.rules@latest", "config.rules@0",
                                "config.rules@01", "config.rules@-1", "config.rules@1x"}) {
        assert(provider.Resolve(invalid, &snapshot, &error) == EINVAL);
    }
    assert(provider.Resolve("config.rules@9", &snapshot, &error) == ENOENT);
    assert(provider.Resolve("config.rules@1", &snapshot, &error) == 0);
    assert(snapshot.sha256_hex == original_hash && *snapshot.content == "{}");

    ConfigChannelProvider other;
    assert(other.Open(path, &error) == 0);
    ConfigPublishRequest concurrent = request;
    concurrent.expected_current_revision = 4;
    concurrent.content = "\"concurrent-a\"";
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    int first_rc = -1;
    int second_rc = -1;
    auto publish = [&](ConfigChannelProvider* instance, const char* value, int* output) {
        auto attempt = concurrent;
        attempt.content = value;
        ConfigPublishResult published;
        std::string detail;
        ready.fetch_add(1);
        while (!go.load()) std::this_thread::yield();
        *output = instance->Publish(attempt, &published, &detail);
    };
    std::thread first(publish, &provider, "\"concurrent-a\"", &first_rc);
    std::thread second(publish, &other, "\"concurrent-b\"", &second_rc);
    while (ready.load() != 2) std::this_thread::yield();
    go.store(true);
    first.join();
    second.join();
    assert((first_rc == 0 && second_rc == EAGAIN) || (first_rc == EAGAIN && second_rc == 0));
    assert(provider.Resolve("config.rules@5", &snapshot, &error) == 0);
    assert(provider.Resolve("config.rules@6", &snapshot, &error) == ENOENT);

    std::vector<ConfigChannelListItem> channels;
    std::string channel_cursor;
    assert(provider.ListChannels("", 0, &channels, &channel_cursor, &error) == EINVAL);
    assert(provider.ListChannels("", 101, &channels, &channel_cursor, &error) == EINVAL);
    assert(provider.ListChannels("", 2, &channels, &channel_cursor, &error) == 0);
    assert(channels.size() == 2 && !channel_cursor.empty());
    assert(channels[0].name < channels[1].name && channel_cursor == channels[1].name);
    const std::string previous_name = channels[1].name;
    assert(provider.ListChannels(channel_cursor, 2, &channels, &channel_cursor, &error) == 0);
    assert(!channels.empty() && previous_name < channels[0].name);
    for (const auto& channel : channels) {
        assert(channel.current_revision >= 1 && channel.sha256_hex.size() == 64);
        assert(channel.content_bytes >= 1 && channel.updated_at_unix_ms > 0);
    }

    std::vector<ConfigRevisionListItem> history;
    uint64_t history_cursor = 0;
    assert(provider.ListHistory("missing", 0, 2, &history, &history_cursor, &error) == ENOENT);
    assert(provider.ListHistory("rules", 0, 0, &history, &history_cursor, &error) == EINVAL);
    assert(provider.ListHistory("rules", 0, 2, &history, &history_cursor, &error) == 0);
    assert(history.size() == 2 && history[0].revision == 5 && history[1].revision == 4);
    assert(history_cursor == 4);
    assert(provider.ListHistory("rules", history_cursor, 2, &history, &history_cursor, &error) == 0);
    assert(history.size() == 2 && history[0].revision == 3 && history[1].revision == 2);
    assert(history[0].base_revision == 1 && history[0].change_note == "restore");
    assert(history_cursor == 2);

    std::vector<ConfigChannelSnapshot> snapshots_before_restart;
    for (uint64_t revision = 1; revision <= 5; ++revision) {
        ConfigChannelSnapshot revision_snapshot;
        const std::string reference = "config.rules@" + std::to_string(revision);
        assert(provider.Resolve(reference.c_str(), &revision_snapshot, &error) == 0);
        snapshots_before_restart.push_back(std::move(revision_snapshot));
    }

    other.Close();
    provider.Close();
    assert(*held.content == "{}" && held.sha256_hex == original_hash);
    ConfigChannelProvider restarted;
    assert(restarted.Open(path, &error) == 0);
    channels.clear();
    channel_cursor.clear();
    assert(restarted.ListChannels("", 100, &channels, &channel_cursor, &error) == 0);
    bool found_restarted_rules = false;
    for (const auto& channel : channels) {
        if (channel.name != "rules") continue;
        found_restarted_rules = true;
        assert(channel.current_revision == 5);
        assert(channel.sha256_hex == snapshots_before_restart.back().sha256_hex);
    }
    assert(found_restarted_rules && channel_cursor.empty());

    history.clear();
    history_cursor = 0;
    assert(restarted.ListHistory("rules", 0, 100, &history, &history_cursor, &error) == 0);
    assert(history.size() == snapshots_before_restart.size() && history_cursor == 0);
    for (size_t index = 0; index < history.size(); ++index) {
        assert(history[index].revision == snapshots_before_restart.size() - index);
    }

    for (const auto& before : snapshots_before_restart) {
        ConfigChannelSnapshot after;
        const std::string reference = "config.rules@" + std::to_string(before.revision);
        assert(restarted.Resolve(reference.c_str(), &after, &error) == 0);
        assert(after.channel_name == before.channel_name && after.revision == before.revision);
        assert(after.format == before.format && after.schema_id == before.schema_id);
        assert(after.sha256_hex == before.sha256_hex && after.content_bytes == before.content_bytes);
        assert(after.created_at_unix_ms == before.created_at_unix_ms);
        assert(after.content && before.content && *after.content == *before.content);
    }
    restarted.Close();

    auto* loader = flowsql::PluginLoader::Single();
    const char* libraries[] = {FLOWSQL_CONFIG_CHANNEL_PLUGIN_PATH};
    const char* missing_options[] = {nullptr};
    assert(loader->Load(".", libraries, missing_options, 1) != 0);
    assert(loader->First(flowsql::IID_CONFIG_CHANNEL_REGISTRY_V1) == nullptr);
    char corrupt_name[] = "/tmp/flowsql_config_corrupt_XXXXXX";
    const int corrupt_fd = mkstemp(corrupt_name);
    assert(corrupt_fd >= 0);
    constexpr char invalid_db[] = "not a SQLite database";
    assert(write(corrupt_fd, invalid_db, sizeof(invalid_db)) == sizeof(invalid_db));
    close(corrupt_fd);
    const std::string invalid_option = std::string("db_path=") + corrupt_name;
    const char* bad_options[] = {invalid_option.c_str()};
    assert(loader->Load(".", libraries, bad_options, 1) != 0);
    assert(loader->First(flowsql::IID_CONFIG_CHANNEL_REGISTRY_V1) == nullptr);
    assert(unlink(corrupt_name) == 0);
    const std::string option = "db_path=" + path;
    const char* options[] = {option.c_str()};
    assert(loader->Load(".", libraries, options, 1) == 0);
    assert(loader->StartAll() == 0);
    auto* registry = static_cast<flowsql::IConfigChannelRegistryV1*>(
        loader->First(flowsql::IID_CONFIG_CHANNEL_REGISTRY_V1));
    assert(registry);
    auto* route_registry = static_cast<flowsql::IRouterHandle*>(loader->First(flowsql::IID_ROUTER_HANDLE));
    assert(route_registry);
    std::unordered_map<std::string, flowsql::fnRouterHandler> discovered_routes;
    route_registry->EnumRoutes([&](const flowsql::RouteItem& item) {
        discovered_routes.emplace(item.uri, item.handler);
    });
    assert(discovered_routes.size() == 4);
    assert(discovered_routes.count("/channels/config/publish") == 1);
    assert(registry->Resolve("config.rules@1", &snapshot, &error) == 0);
    assert(*snapshot.content == "{}" && snapshot.sha256_hex == original_hash);
    loader->StopAll();
    assert(loader->Unload() == 0);
    assert(*snapshot.content == "{}");

    flowsql::channels::config::ConfigChannelPlugin control;
    assert(control.Option(option.c_str()) == 0);
    assert(control.Load(nullptr) == 0);
    std::unordered_map<std::string, flowsql::fnRouterHandler> routes;
    control.EnumRoutes([&](const flowsql::RouteItem& item) {
        assert(item.method == "POST");
        routes.emplace(item.uri, item.handler);
    });
    assert(routes.size() == 4);
    std::string response;
    const auto call = [&](const char* route, const std::string& body) {
        response.clear();
        return routes.at(route)(route, body, response);
    };
    assert(call("/channels/config/resolve", R"({"exact_reference":"config.rules@1"})") == flowsql::error::OK);
    rapidjson::Document reply;
    reply.Parse(response.c_str());
    assert(!reply.HasParseError() && reply["content_base64"].IsString());
    assert(std::string(reply["content_base64"].GetString()) == "e30=");
    assert(call("/channels/config/resolve", R"({"exact_reference":"config.rules@latest"})") ==
           flowsql::error::BAD_REQUEST);
    assert(call("/channels/config/resolve", R"({"exact_reference":"config.rules@9"})") ==
           flowsql::error::NOT_FOUND);
    assert(call("/channels/config/history", R"({"name":"missing"})") == flowsql::error::NOT_FOUND);
    assert(call("/channels/config/list", R"({"limit":1})") == flowsql::error::OK);
    reply.Parse(response.c_str());
    assert(!reply.HasParseError() && reply["items"].Size() == 1);
    assert(!reply["items"][0].HasMember("content") && !reply["items"][0].HasMember("content_base64"));
    assert(call("/channels/config/history", R"({"name":"rules","limit":1})") == flowsql::error::OK);
    reply.Parse(response.c_str());
    assert(!reply.HasParseError() && reply["items"].Size() == 1);
    assert(!reply["items"][0].HasMember("content") && !reply["items"][0].HasMember("content_base64"));
    assert(call("/channels/config/list", R"({"limit":101})") == flowsql::error::BAD_REQUEST);
    assert(call("/channels/config/list", R"({"limit":1,"limit":2})") == flowsql::error::BAD_REQUEST);
    assert(call("/channels/config/list", R"({"unknown":1})") == flowsql::error::BAD_REQUEST);
    assert(call("/channels/config/list", std::string("{\"cursor\":\"") + static_cast<char>(0xff) + "\"}") ==
           flowsql::error::BAD_REQUEST);
    assert(call("/channels/config/list", std::string(kMaxControlRequestBytes + 1, ' ')) ==
           flowsql::error::PAYLOAD_TOO_LARGE);
    assert(call("/channels/config/list", NestedJson(10000)) == flowsql::error::BAD_REQUEST);
    assert(call("/channels/config/history", R"({"name":"rules","cursor":"01"})") ==
           flowsql::error::BAD_REQUEST);
    assert(call("/channels/config/publish",
                R"({"name":"wire","expected_current_revision":0,"format":"json",)"
                R"("schema_id":"wire-v1","content_base64":"e30="})") == flowsql::error::OK);
    reply.Parse(response.c_str());
    assert(!reply.HasParseError() && reply["revision"].GetUint64() == 1);
    assert(call("/channels/config/publish",
                R"({"name":"wire","expected_current_revision":0,"format":"json",)"
                R"("schema_id":"wire-v1","content_base64":"e30="})") == flowsql::error::OK);
    reply.Parse(response.c_str());
    assert(!reply["created_revision"].GetBool() && reply["revision"].GetUint64() == 1);
    assert(call("/channels/config/publish",
                R"({"name":"wire","expected_current_revision":0,"format":"json",)"
                R"("schema_id":"wire-v1","content_base64":"e30K"})") == flowsql::error::CONFLICT);
    assert(call("/channels/config/publish",
                R"({"name":"bad","expected_current_revision":0,"format":"json",)"
                R"("schema_id":"wire-v1","content_base64":"!!!!"})") == flowsql::error::BAD_REQUEST);
    assert(call("/channels/config/publish",
                R"({"name":"bad-utf8","expected_current_revision":0,"format":"json",)"
                R"("schema_id":"wire-v1","content_base64":"/w=="})") == flowsql::error::BAD_REQUEST);
    assert(call("/channels/config/publish", R"({"name":"bad","expected_current_revision":0,"format":"json",)"
                                            R"("schema_id":"wire-v1","content_base64":")" +
                                                std::string(((kMaxContentBytes + 2) / 3) * 4, 'A') + R"("})") ==
           flowsql::error::PAYLOAD_TOO_LARGE);
    assert(call("/channels/config/publish",
                R"({"name":"bad","expected_current_revision":0,"format":"json",)"
                R"("schema_id":"wire-v1","content_base64":"e30="})") == flowsql::error::OK);
    assert(control.Unload() == 0);

    assert(unlink(path.c_str()) == 0);
    std::puts("[PASS] config channel SQLite snapshots and revisions");
    return 0;
}
