// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include "data_access_plugin.h"
#include "access_store.h"

#include <openssl/crypto.h>
#include <rapidjson/schema.h>

#include <cstdlib>
#include <fstream>
#include <regex>
#include <set>

namespace flowsql::access {
using namespace dataset;
Json Parse(const std::string& text) {
    Json value;
    if (text.size() <= 1024 * 1024 && JsonNestingWithin(text, 32)) value.Parse(text.data(), text.size());
    if (!value.IsObject() || value.HasParseError())
        throw AccessError("INVALID_REQUEST", "expected a bounded JSON object");
    return value;
}
std::string Required(const Value& value, const char* key) {
    const auto result = Get(value, key);
    if (result.empty() || result.size() > 1024 || result.find('\0') != std::string::npos)
        throw AccessError("INVALID_REQUEST", std::string("invalid ") + key);
    return result;
}
uint64_t Bound(const Value& value, const char* key, uint64_t fallback, uint64_t low, uint64_t high) {
    if (!value.HasMember(key)) return fallback;
    if (!value[key].IsUint64() || value[key].GetUint64() < low || value[key].GetUint64() > high)
        throw AccessError("INVALID_REQUEST", std::string("invalid budget: ") + key);
    return value[key].GetUint64();
}
void Keys(const Value& value, std::initializer_list<const char*> allowed) {
    if (!value.IsObject()) throw AccessError("INVALID_REQUEST", "expected an object");
    const std::set<std::string> names(allowed.begin(), allowed.end());
    std::set<std::string> seen;
    for (const auto& member : value.GetObject())
        if (!names.count(Text(member.name)) || !seen.insert(Text(member.name)).second)
            throw AccessError("INVALID_REQUEST", "unknown or duplicate property");
}
static Json ReadFile(const std::string& path) {
    std::ifstream stream(path);
    if (!stream) throw AccessError("CONFIGURATION_ERROR", "cannot open configured contract/policy");
    return Parse(std::string(std::istreambuf_iterator<char>(stream), {}));
}
DataAccessPlugin::DataAccessPlugin() = default;
DataAccessPlugin::~DataAccessPlugin() { Stop(); }
int DataAccessPlugin::Option(const char* text) {
    try {
        const auto option = Parse(text ? text : "{}");
        Keys(option, {"host", "port", "policy_file", "contract_file", "state_path"});
        host_ = Get(option, "host", host_);
        port_ = Bound(option, "port", port_, 1, 65535);
        state_path_ = Required(option, "state_path");
        contract_path_ = Required(option, "contract_file");
        contract_ = ReadFile(contract_path_);
        policy_ = ReadFile(Required(option, "policy_file"));
        Keys(policy_, {"subjects"});
        if (!policy_.HasMember("subjects") || !policy_["subjects"].IsArray() || policy_["subjects"].Empty())
            throw AccessError("CONFIGURATION_ERROR", "at least one subject is required");
        if (!contract_.HasMember("tools") || !contract_["tools"].IsArray())
            throw AccessError("CONFIGURATION_ERROR", "tool contracts are required");
        subjects_.clear();
        std::set<std::string> principals, tokens;
        for (const auto& item : policy_["subjects"].GetArray()) {
            Keys(item, {"principal", "token_env", "sources", "analyses", "max_rows", "max_bytes", "timeout_ms",
                        "result_ttl_ms"});
            Subject subject;
            subject.principal = Required(item, "principal");
            const auto environment = Required(item, "token_env");
            const char* token = std::getenv(environment.c_str());
            if (!token || std::strlen(token) < 32 || !principals.insert(subject.principal).second ||
                !tokens.insert(token).second)
                throw AccessError("CONFIGURATION_ERROR", "missing, short or duplicate subject credential");
            subject.token = token;
            if (!item.HasMember("sources") || !item["sources"].IsArray())
                throw AccessError("CONFIGURATION_ERROR", "explicit source grants are required");
            for (const auto& grant : item["sources"].GetArray()) {
                Keys(grant, {"locator", "operations", "filters"});
                if (!grant.HasMember("locator") || !grant["locator"].IsObject() || !grant.HasMember("operations") ||
                    !grant["operations"].IsArray())
                    throw AccessError("CONFIGURATION_ERROR", "invalid source grant");
                const auto kind = Required(grant["locator"], "kind");
                if (kind == "database_table")
                    Keys(grant["locator"], {"kind", "type", "name", "schema", "database", "table"});
                else if (kind == "dataframe")
                    Keys(grant["locator"], {"kind", "name"});
                else if (kind == "stream" || kind == "block_stream")
                    Keys(grant["locator"], {"kind", "type", "name"});
                else if (kind == "config")
                    Keys(grant["locator"], {"kind", "reference"});
                else
                    throw AccessError("CONFIGURATION_ERROR", "unknown source kind");
                if (grant.HasMember("filters") && !grant["filters"].IsArray())
                    throw AccessError("CONFIGURATION_ERROR", "invalid policy filters");
                for (const auto& operation : grant["operations"].GetArray())
                    if (!operation.IsString() || (Text(operation) != "query" && Text(operation) != "analyze"))
                        throw AccessError("CONFIGURATION_ERROR", "unknown granted operation");
            }
            if (item.HasMember("analyses")) {
                if (!item["analyses"].IsArray() || item["analyses"].Size() > 32)
                    throw AccessError("CONFIGURATION_ERROR", "analyses must contain at most 32 fixed profiles");
                std::set<std::string> names;
                const std::regex identifier("[A-Za-z_][A-Za-z0-9_]*");
                for (const auto& profile : item["analyses"].GetArray()) {
                    Keys(profile,
                         {"name", "operator", "source", "parameters", "config_refs", "output", "max_input_bytes"});
                    const auto name = Required(profile, "name");
                    if (!names.insert(name).second || !std::regex_match(name, identifier) ||
                        !profile.HasMember("source") || !profile["source"].IsObject() ||
                        !profile.HasMember("operator") || !profile.HasMember("parameters") ||
                        !profile["parameters"].IsObject() || !profile.HasMember("config_refs") ||
                        !profile["config_refs"].IsArray() || !profile.HasMember("output"))
                        throw AccessError("CONFIGURATION_ERROR", "invalid analysis profile");
                    Keys(profile["operator"], {"category", "name"});
                    for (const auto* key : {"category", "name"})
                        if (!std::regex_match(Required(profile["operator"], key), identifier))
                            throw AccessError("CONFIGURATION_ERROR", "invalid operator identity");
                    for (const auto& parameter : profile["parameters"].GetObject())
                        if (!parameter.value.IsString() || !std::regex_match(Text(parameter.name), identifier))
                            throw AccessError("CONFIGURATION_ERROR", "algorithm parameters require string values");
                    for (const auto& reference : profile["config_refs"].GetArray())
                        if (!reference.IsString() ||
                            !std::regex_match(Text(reference), std::regex("config\\.[A-Za-z0-9_.-]+@[1-9][0-9]*")))
                            throw AccessError("CONFIGURATION_ERROR", "exact config references are required");
                    const auto kind = Required(profile["output"], "kind");
                    if (kind == "dataframe")
                        Keys(profile["output"], {"kind"});
                    else if (kind == "managed_database") {
                        Keys(profile["output"], {"kind", "type", "name"});
                        const auto type = Required(profile["output"], "type");
                        if ((type != "sqlite" && type != "mysql" && type != "postgres" && type != "clickhouse") ||
                            !std::regex_match(Required(profile["output"], "name"), identifier))
                            throw AccessError("CONFIGURATION_ERROR", "invalid managed output channel");
                    } else
                        throw AccessError("CONFIGURATION_ERROR", "unsupported analysis output");
                    Bound(profile, "max_input_bytes", 67108864, 1, 67108864);
                }
            }
            Bound(item, "max_rows", 1000, 1, 65536);
            Bound(item, "max_bytes", 8 * 1024 * 1024, 1024, 64 * 1024 * 1024);
            Bound(item, "timeout_ms", 5000, 1, 30000);
            Bound(item, "result_ttl_ms", 3600000, 1, 86400000);
            auto owned = std::make_shared<Json>();
            owned->CopyFrom(item, owned->GetAllocator());
            subject.policy = std::move(owned);
            subjects_.push_back(std::move(subject));
        }
        std::map<std::string, std::string> output_owners;
        for (const auto& subject : subjects_) {
            if (!subject.policy->HasMember("analyses")) continue;
            for (const auto& profile : (*subject.policy)["analyses"].GetArray()) {
                const auto& output = profile["output"];
                if (Get(output, "kind") != "managed_database") continue;
                const auto key = Get(output, "type") + "." + Get(output, "name");
                const auto inserted = output_owners.emplace(key, subject.principal);
                if (!inserted.second && inserted.first->second != subject.principal)
                    throw AccessError("CONFIGURATION_ERROR", "analysis output channel is shared across subjects");
                for (const auto& other : subjects_)
                    for (const auto& grant : (*other.policy)["sources"].GetArray()) {
                        const auto& locator = grant["locator"];
                        if (Get(locator, "kind") == "database_table" &&
                            Get(locator, "type") + "." + Get(locator, "name") == key)
                            throw AccessError("CONFIGURATION_ERROR",
                                              "analysis output cannot be a business source channel");
                    }
            }
        }
        return 0;
    } catch (const AccessError& error) {
        std::fprintf(stderr, "DataAccess configuration: %s\n", error.what());
        return -1;
    }
}
int DataAccessPlugin::Load(IQuerier* querier) {
    querier_ = querier;
    return 0;
}
const Subject& DataAccessPlugin::Authenticate(const httplib::Request& request) const {
    const auto header = request.get_header_value("Authorization");
    if (header.rfind("Bearer ", 0) == 0) {
        const auto token = header.substr(7);
        for (const auto& subject : subjects_)
            if (token.size() == subject.token.size() &&
                CRYPTO_memcmp(token.data(), subject.token.data(), token.size()) == 0)
                return subject;
    }
    throw AccessError("UNAUTHENTICATED", "a credential issued for this data-access service is required", 401);
}
void DataAccessPlugin::Route(const httplib::Request& request, httplib::Response& response) {
    try {
        const auto& subject = Authenticate(request);
        if (request.has_header("Origin")) throw AccessError("FORBIDDEN", "browser origin is not allowed", 403);
        Json output;
        if (request.path == "/data-access/tools") {
            output.SetObject();
            output.AddMember("tools", Copy(contract_["tools"], output.GetAllocator()), output.GetAllocator());
        } else {
            const auto input = Parse(request.body);
            Keys(input, {"name", "arguments"});
            const auto tool = Required(input, "name");
            if (!input.HasMember("arguments") || !input["arguments"].IsObject())
                throw AccessError("INVALID_REQUEST", "object arguments are required");
            const Value* specification = nullptr;
            for (const auto& item : contract_["tools"].GetArray())
                if (Get(item, "name") == tool) specification = &item;
            if (!specification) throw AccessError("UNSUPPORTED_OPERATION", "unknown tool", 422);
            rapidjson::SchemaDocument schema((*specification)["inputSchema"]);
            rapidjson::SchemaValidator validator(schema);
            if (!input["arguments"].Accept(validator))
                throw AccessError("INVALID_REQUEST", "arguments do not match the published tool schema");
            output = Call(subject, tool, input["arguments"]);
        }
        response.set_content(Encode(output), "application/json");
    } catch (const AccessError& error) {
        Json output(rapidjson::kObjectType);
        auto& a = output.GetAllocator();
        Value detail(rapidjson::kObjectType);
        Set(detail, "code", error.code, a);
        Set(detail, "message", error.what(), a);
        output.AddMember("error", detail, a);
        response.status = error.status;
        if (error.status == 401) response.set_header("WWW-Authenticate", "Bearer");
        response.set_content(Encode(output), "application/json");
    }
}
int DataAccessPlugin::Start() {
    if (subjects_.empty()) return -1;
    try {
        store_ = std::make_unique<AccessStore>(state_path_);
    } catch (const AccessError& error) {
        std::fprintf(stderr, "DataAccess startup: %s\n", error.what());
        return -1;
    }
    server_.set_payload_max_length(1024 * 1024);
    server_.set_read_timeout(5);
    server_.set_write_timeout(5);
    server_.Get("/health",
                [](const auto&, auto& response) { response.set_content(R"({"status":"ok"})", "application/json"); });
    server_.Get("/data-access/tools", [this](const auto& req, auto& rsp) { Route(req, rsp); });
    server_.Post("/data-access/call", [this](const auto& req, auto& rsp) { Route(req, rsp); });
    if (!server_.bind_to_port(host_, port_)) return -1;
    listener_ = std::thread([this] { server_.listen_after_bind(); });
    server_.wait_until_ready();
    return 0;
}
int DataAccessPlugin::Stop() {
    server_.stop();
    if (listener_.joinable()) listener_.join();
    for (const auto& task : tasks_) {
        *task->cancelled = true;
        std::lock_guard<std::mutex> lock(task->mutex);
        if (task->session) task->session->Cancel();
    }
    for (const auto& task : tasks_)
        if (task->thread.joinable()) task->thread.join();
    tasks_.clear();
    store_.reset();
    return 0;
}
int DataAccessPlugin::Unload() { return Stop(); }
Json DataAccessPlugin::Call(const Subject& subject, const std::string& tool, const Value& args) {
    if (tool == "list_sources") {
        auto sources = Sources(subject);
        const auto offset = Bound(args, "offset", 0, 0, INT64_MAX);
        const auto limit = Bound(args, "limit", 32, 1, 100);
        Json output(rapidjson::kObjectType);
        auto& a = output.GetAllocator();
        Value items(rapidjson::kArrayType);
        for (uint64_t i = std::min<uint64_t>(offset, sources.size()); i < sources.size() && i - offset < limit; ++i)
            items.PushBack(Copy(SourceJson(sources[i]), a), a);
        output.AddMember("sources", items, a);
        output.AddMember("total_sources", static_cast<uint64_t>(sources.size()), a);
        output.AddMember("offset", offset, a);
        output.AddMember("has_more", offset < sources.size() && limit < sources.size() - offset, a);
        if (Encode(output).size() > 1024 * 1024)
            throw AccessError("RESULT_PAGE_LIMIT", "source page exceeds 1 MiB; request fewer sources", 413);
        return output;
    }
    if (tool == "describe_source") return SourceJson(Resolve(subject, args["source_ref"]));
    return ExecuteCall(subject, tool, args);
}
}  // namespace flowsql::access

BEGIN_PLUGIN_REGIST(flowsql::access::DataAccessPlugin)
____INTERFACE(flowsql::IID_PLUGIN, flowsql::IPlugin)
____INTERFACE(flowsql::IID_PLUGIN_EXTERNAL_ENTRY, flowsql::IPlugin)
END_PLUGIN_REGIST()
