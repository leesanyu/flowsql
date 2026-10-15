// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <common/error_code.h>
#include <sys/stat.h>
#include <algorithm>
#include <cerrno>
#include "kafka_plugin.h"

namespace flowsql::channels::kafka {
namespace {
using Clock = std::chrono::steady_clock;
void Require(bool condition, const char* code = "INVALID_REQUEST", int status = 400) {
    if (!condition) throw Failure(status, code);
}
void Keys(const Json& request, std::initializer_list<const char*> keys) {
    for (auto i = request.MemberBegin(); i != request.MemberEnd(); ++i)
        Require(std::any_of(keys.begin(), keys.end(), [&](auto key) { return i->name == key; }), "UNKNOWN_FIELD");
}
int32_t Status(int status) {
    switch (status) {
        case 202:
            return error::ACCEPTED;
        case 400:
            return error::BAD_REQUEST;
        case 404:
            return error::NOT_FOUND;
        case 409:
            return error::CONFLICT;
        case 413:
            return error::PAYLOAD_TOO_LARGE;
        case 429:
            return error::TOO_MANY_REQUESTS;
        case 503:
            return error::UNAVAILABLE;
        default:
            return error::INTERNAL_ERROR;
    }
}
Json Failed(const char* code, const char* stage) {
    Json result;
    result.SetObject();
    String(result, "status", "failed", result.GetAllocator());
    String(result, "code", code, result.GetAllocator());
    String(result, "stage", stage, result.GetAllocator());
    String(result, "read_permission", "unverified", result.GetAllocator());
    String(result, "write_permission", "unverified", result.GetAllocator());
    return result;
}
}  // namespace
void Plugin::EnumRoutes(std::function<void(const RouteItem&)> callback) {
    for (const char* operation : {"definitions/query", "connections/query", "connections/add", "connections/modify",
                                  "connections/remove", "channels/query", "channels/add", "channels/modify",
                                  "channels/remove", "tests/start", "tests/query", "tests/cancel"}) {
        callback({"POST", std::string("/channels/kafka/") + operation,
                  [this, operation](const std::string&, const std::string& request, std::string& response) {
                      return Handle(operation, request, response);
                  }});
    }
}
void Plugin::CheckConnection(const rapidjson::Value& connection) {
    auto mechanism = Text(connection, "sasl_mechanism");
    if (!mechanism.empty()) {
        bool known = false, found = false;
        for (auto& available : capabilities_["sasl_mechanisms"].GetArray())
            if (Text(available, "name") == mechanism) {
                known = true;
                found = available["available"].GetBool();
            }
        Require(known, "INVALID_SASL_MECHANISM");
        Require(found, "AUTH_MECHANISM_UNAVAILABLE", 503);
    }
    auto reference = [this](const std::string& value) {
        if (value.rfind("file:", 0) == 0)
            Require(deployment_.files.count(value.substr(5)), "CREDENTIAL_ALIAS_UNAVAILABLE");
        else if (value.rfind("env:", 0) == 0)
            Require(deployment_.environments.count(value.substr(4)), "CREDENTIAL_ALIAS_UNAVAILABLE");
        else
            throw Failure(400, "INVALID_CREDENTIAL_REFERENCE");
    };
    if (connection.HasMember("tls") && connection["tls"].IsObject())
        for (auto i = connection["tls"].MemberBegin(); i != connection["tls"].MemberEnd(); ++i)
            if (i->value.IsString()) reference(i->value.GetString());
    if (connection.HasMember("sasl") && connection["sasl"].IsObject())
        for (auto field : {"keytab_ref", "ca_ref"})
            if (connection["sasl"].HasMember(field)) reference(Text(connection["sasl"], field));
    if (connection.HasMember("credential_bindings") && connection["credential_bindings"].IsObject())
        for (auto i = connection["credential_bindings"].MemberBegin();
             i != connection["credential_bindings"].MemberEnd(); ++i)
            if (i->value.IsObject() && i->value.HasMember("reference")) reference(Text(i->value, "reference"));
}
Json Plugin::DefinitionsJson() {
    auto result = Parse(Encode(capabilities_));
    auto& a = result.GetAllocator();
    auto names = [&](std::initializer_list<const char*> values) {
        rapidjson::Value array(rapidjson::kArrayType);
        for (auto name : values) array.PushBack(rapidjson::Value(name, a), a);
        return array;
    };
    result.AddMember("security_protocols", names({"PLAINTEXT", "SSL", "SASL_PLAINTEXT", "SASL_SSL"}), a);
    result.AddMember("roles", names({"source", "sink", "both"}), a);
    result.AddMember("payload_kinds", names({"structured", "packet"}), a);
    result.AddMember("tls_fields", names({"ca_ref", "certificate_ref", "private_key_ref"}), a);
    rapidjson::Value fields(rapidjson::kObjectType);
    fields.AddMember("PLAIN", names({"username", "password"}), a);
    fields.AddMember("SCRAM-SHA-256", names({"username", "password"}), a);
    fields.AddMember("SCRAM-SHA-512", names({"username", "password"}), a);
    fields.AddMember("GSSAPI", names({"principal", "service_name", "keytab_ref"}), a);
    fields.AddMember("OAUTHBEARER", names({"token_endpoint", "client_id", "client_secret", "scope", "ca_ref"}), a);
    result.AddMember("mechanism_fields", fields, a);
    rapidjson::Value aliases(rapidjson::kObjectType), files(rapidjson::kArrayType), envs(rapidjson::kArrayType);
    for (auto& [name, path] : deployment_.files) files.PushBack(rapidjson::Value(("file:" + name).c_str(), a), a);
    for (auto& [name, variable] : deployment_.environments)
        envs.PushBack(rapidjson::Value(("env:" + name).c_str(), a), a);
    aliases.AddMember("files", files, a);
    aliases.AddMember("environments", envs, a);
    result.AddMember("credential_aliases", aliases, a);
    auto adapters = Parse(
        R"({"structured":{"source":"available","sink":"available"},"packet":{"source":"available","sink":"available"}})");
    result.AddMember("adapter_status", rapidjson::Value(adapters, a), a);
    // Documents own their allocator; moving a temporary Document's Value would leave dangling members.
    auto source = Parse(
        R"({"encodings":["json"],"types":["bool","int64","uint64","float64","utf8"],"offset_reset":["earliest","latest","error"],"commit_policy":["after_success","none"],"requires_async":true})");
    result.AddMember("structured_source", rapidjson::Value(source, a), a);
    auto packet_source = Parse(
        R"({"encodings":["flowsql_packet_v1"],"max_inputs":32,"link_types":[1],"partitions":1,"offset_reset":["earliest","latest","error"],"commit_policy":["after_success","none"],"requires_async":true,"read_permission":"unverified"})");
    result.AddMember("packet_source", rapidjson::Value(packet_source, a), a);
    auto packet_sink = Parse(
        R"({"encodings":["flowsql_packet_v1"],"max_inputs":32,"link_types":[1],"partitions":1,"partition":0,"requires_async":true,"write_permission":"unverified"})");
    result.AddMember("packet_sink", rapidjson::Value(packet_sink, a), a);
    auto sink = Parse(
        R"({"encodings":["json"],"record_kinds":["event","upsert"],"envelope_version":1,"max_key_fields":8,"types":["bool","int8","int16","int32","int64","uint8","uint16","uint32","uint64","float32","float64","utf8","binary","fixed_size_binary","timestamp"],"write_permission":"unverified"})");
    result.AddMember("structured_sink", rapidjson::Value(sink, a), a);
    return result;
}
std::string Plugin::FilesStamp() const {
    std::string result;
    for (auto& [name, path] : deployment_.files) {
        struct stat info {};
        result += name + ":";
        if (stat(path.c_str(), &info))
            result += "unavailable";
        else
            result += std::to_string(info.st_dev) + ":" + std::to_string(info.st_ino) + ":" +
                      std::to_string(info.st_size) + ":" + std::to_string(info.st_mtim.tv_sec) + ":" +
                      std::to_string(info.st_mtim.tv_nsec);
        result += ';';
    }
    return result;
}
void Plugin::PruneTests() {
    for (auto i = tests_.begin(); i != tests_.end();) {
        auto job = i->second;
        bool expired;
        {
            std::lock_guard<std::mutex> lock(job->mutex);
            expired = job->done && Clock::now() - job->completed >= std::chrono::minutes(5);
        }
        if (expired) {
            if (job->worker.joinable()) job->worker.join();
            i = tests_.erase(i);
        } else
            ++i;
    }
}
Json Plugin::StartTest(const Json& request) {
    Keys(request, {"target", "name", "expected_revision", "draft", "usage", "topic", "timeout_ms"});
    auto started = Clock::now();
    auto target = Text(request, "target");
    Require(target == "connection" || target == "channel", "INVALID_TEST_TARGET");
    uint32_t budget = 10000;
    if (request.HasMember("timeout_ms")) {
        Require(request["timeout_ms"].IsUint() && request["timeout_ms"].GetUint() >= 1 &&
                    request["timeout_ms"].GetUint() <= 30000,
                "INVALID_TIMEOUT");
        budget = request["timeout_ms"].GetUint();
    }
    std::unique_lock<std::mutex> lock(tests_mutex_);
    Require(!tests_stopping_, "PROVIDER_STOPPING", 503);
    PruneTests();
    size_t running = 0;
    for (auto& [id, job] : tests_) {
        std::lock_guard<std::mutex> job_lock(job->mutex);
        if (!job->done) ++running;
    }
    Require(running < 8, "VERIFICATION_LIMIT", 429);
    Binding binding;
    if (request.HasMember("draft")) {
        Require(request["draft"].IsObject() && !request.HasMember("name") && !request.HasMember("expected_revision"));
        auto draft = Parse(Encode(request["draft"]));
        if (target == "connection") {
            CheckConnection(draft);
            binding = store_.Draft(std::move(draft));
        } else {
            Store::ValidateChannel(draft);
            binding = store_.AcquireConnection(Text(draft, "connection_name"));
            binding.snapshot.channel_name = Text(draft, "name");
            binding.snapshot.topic = Text(draft, "topic");
            binding.snapshot.usage = Text(draft, "role") == "sink" ? "sink" : "source";
        }
    } else {
        auto name = Text(request, "name");
        if (target == "connection")
            binding = store_.AcquireConnection(name);
        else {
            auto rows = store_.Query("channels", name);
            Require(!rows["items"].Empty(), "CONFIGURATION_NOT_FOUND", 404);
            auto usage = Text(request, "usage", Text(rows["items"][0], "role") == "sink" ? "sink" : "source");
            binding = store_.Acquire(name, usage);
        }
    }
    auto snapshot = binding.snapshot;
    bool leased = binding.leased;
    try {
        if (request.HasMember("expected_revision")) {
            auto revision = target == "connection" ? snapshot.connection_revision : snapshot.channel_revision;
            Require(request["expected_revision"].IsUint64() && request["expected_revision"].GetUint64() == revision,
                    "REVISION_CONFLICT", 409);
        }
        CheckConnection(binding.connection);
        if (target == "connection" && request.HasMember("topic")) {
            auto channel =
                Parse(R"({"name":"probe","connection_name":"probe","role":"sink","payload_kind":"structured"})");
            String(channel, "topic", Text(request, "topic"), channel.GetAllocator());
            Store::ValidateChannel(channel);
            binding.snapshot.topic = Text(request, "topic");
        }
        auto id = std::to_string(++next_test_);
        id.insert(0, 20 - id.size(), '0');
        auto job = std::make_shared<Verification>();
        job->snapshot = snapshot;
        job->draft = request.HasMember("draft");
        job->name = target == "connection" ? snapshot.connection_name : snapshot.channel_name;
        job->target = target;
        job->files_stamp = FilesStamp();
        job->result = Parse(R"({"status":"running","stage":"configuration"})");
        auto accepted = Parse("{\"test_id\":\"" + id + "\",\"status\":\"running\"}");
        tests_[id] = job;
        try {
            job->worker = std::thread([this, job, budget, started, leased, binding = std::move(binding)]() mutable {
                Json result;
                try {
                    auto client = std::make_shared<Client>(std::move(binding), deployment_);
                    {
                        std::lock_guard<std::mutex> guard(job->mutex);
                        job->client = client;
                        if (job->cancelled) client->Cancel();
                    }
                    auto remaining =
                        budget - std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
                    std::string output;
                    if (remaining > 0) {
                        client->Probe(static_cast<uint32_t>(remaining), &output);
                        result = Parse(output);
                    } else
                        result = Failed("TIMEOUT", "configuration");
                    {
                        std::lock_guard<std::mutex> guard(job->mutex);
                        job->client.reset();
                    }
                    client.reset();
                } catch (const Failure& failure) {
                    result = Failed(failure.code.c_str(), failure.stage.c_str());
                } catch (...) {
                    result = Failed("VERIFICATION_FAILED", "configuration");
                }
                std::shared_ptr<Client> retiring;
                {
                    std::lock_guard<std::mutex> guard(job->mutex);
                    retiring = std::move(job->client);
                }
                retiring.reset();
                if (leased) store_.Release(job->snapshot);
                std::lock_guard<std::mutex> guard(job->mutex);
                if (job->cancelled) result = Failed("CANCELLED", "cancel");
                auto& a = result.GetAllocator();
                result.AddMember("connection_revision", job->snapshot.connection_revision, a);
                result.AddMember("channel_revision", job->snapshot.channel_revision, a);
                if (!result.HasMember("verified_at"))
                    result.AddMember("verified_at",
                                     std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::system_clock::now().time_since_epoch())
                                         .count(),
                                     a);
                result.RemoveMember("timeout_ms");
                result.AddMember("timeout_ms", budget, a);
                job->result = std::move(result);
                job->completed = Clock::now();
                job->done = true;
            });
        } catch (...) {
            tests_.erase(id);
            throw;
        }
        return accepted;
    } catch (...) {
        if (leased) store_.Release(snapshot);
        throw;
    }
}
Json Plugin::Test(const Json& request, bool cancel) {
    Keys(request, {"test_id"});
    std::lock_guard<std::mutex> lock(tests_mutex_);
    PruneTests();
    auto found = tests_.find(Text(request, "test_id"));
    Require(found != tests_.end(), "VERIFICATION_NOT_FOUND", 404);
    auto job = found->second;
    std::lock_guard<std::mutex> guard(job->mutex);
    if (cancel && !job->done) {
        job->cancelled = true;
        if (job->client) job->client->Cancel();
    }
    auto result = Parse(Encode(job->result));
    String(result, "test_id", found->first, result.GetAllocator());
    return result;
}
int Plugin::Start() {
    if (tests_cleanup_.joinable()) return 0;
    tests_cleanup_ = std::thread([this] {
        std::unique_lock<std::mutex> lock(tests_mutex_);
        while (!tests_stopping_) {
            PruneTests();
            tests_changed_.wait_for(lock, std::chrono::seconds(1), [this] { return tests_stopping_; });
        }
    });
    return 0;
}
void Plugin::StopTests() {
    std::map<std::string, std::shared_ptr<Verification>> jobs;
    {
        std::lock_guard<std::mutex> lock(tests_mutex_);
        tests_stopping_ = true;
        jobs.swap(tests_);
        for (auto& [id, job] : jobs) {
            std::lock_guard<std::mutex> guard(job->mutex);
            job->cancelled = true;
            if (job->client) job->client->Cancel();
        }
    }
    tests_changed_.notify_all();
    if (tests_cleanup_.joinable()) tests_cleanup_.join();
    for (auto& [id, job] : jobs)
        if (job->worker.joinable()) job->worker.join();
}
void Plugin::AttachVerification(Json& result, const std::string& kind) {
    std::lock_guard<std::mutex> lock(tests_mutex_);
    PruneTests();
    auto stamp = FilesStamp();
    for (auto& row : result["items"].GetArray()) {
        auto verification = Parse(R"({"status":"unverified"})");
        for (auto i = tests_.rbegin(); i != tests_.rend(); ++i) {
            auto job = i->second;
            std::lock_guard<std::mutex> guard(job->mutex);
            if (job->draft || job->target + "s" != kind || job->name != Text(row, "name")) continue;
            if (row["revision"].GetUint64() !=
                    (kind == "connections" ? job->snapshot.connection_revision : job->snapshot.channel_revision) ||
                (kind == "channels" && row["connection_revision"].GetUint64() != job->snapshot.connection_revision) ||
                stamp != job->files_stamp)
                break;
            verification = Parse(Encode(job->result));
            break;
        }
        row.AddMember("verification", rapidjson::Value(verification, result.GetAllocator()), result.GetAllocator());
    }
}
int Plugin::DescribeChannel(const char* name, KafkaChannelDescriptionV1* description) {
    if (!name || !description) return EINVAL;
    *description = {};
    try {
        auto rows = store_.Query("channels", name);
        if (rows["items"].Empty()) return ENOENT;
        description->role = Text(rows["items"][0], "role");
        description->revision = rows["items"][0]["revision"].GetUint64();
        description->source_adapter_available = Text(rows["items"][0]["adapter_status"], "source") == "ready";
        description->sink_adapter_available = Text(rows["items"][0]["adapter_status"], "sink") == "ready";
        return 0;
    } catch (...) {
        return EIO;
    }
}
int32_t Plugin::Handle(const std::string& operation, const std::string& text, std::string& response) {
    try {
        auto request = Parse(text);
        Json result;
        int32_t status = error::OK;
        if (operation == "definitions/query") {
            Keys(request, {});
            result = DefinitionsJson();
        } else if (operation == "tests/start") {
            result = StartTest(request);
            status = error::ACCEPTED;
        } else if (operation == "tests/query" || operation == "tests/cancel") {
            result = Test(request, operation == "tests/cancel");
        } else {
            auto slash = operation.find('/');
            auto kind = operation.substr(0, slash), action = operation.substr(slash + 1);
            if (action == "query") {
                Keys(request, {"name", "after", "limit"});
                size_t limit = 100;
                if (request.HasMember("limit")) {
                    Require(request["limit"].IsUint(), "INVALID_PAGE_LIMIT");
                    limit = request["limit"].GetUint();
                }
                result = store_.Query(kind, Text(request, "name"), Text(request, "after"), limit);
                AttachVerification(result, kind);
                String(result, "next_cursor",
                       result["items"].Size() == limit ? Text(result["items"][limit - 1], "name") : "",
                       result.GetAllocator());
            } else {
                if (kind == "connections" && action != "remove") CheckConnection(request);
                result = store_.Mutate(kind, action, std::move(request));
            }
        }
        response = Encode(result);
        return status;
    } catch (const Failure& failure) {
        auto result = Failed(failure.code.c_str(), failure.stage.c_str());
        response = Encode(result);
        return Status(failure.status);
    } catch (...) {
        response = R"({"status":"failed","code":"INTERNAL_ERROR","stage":"persistence"})";
        return error::INTERNAL_ERROR;
    }
}
}  // namespace flowsql::channels::kafka
