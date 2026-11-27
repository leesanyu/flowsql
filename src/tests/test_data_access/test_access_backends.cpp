// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <services/data_access/data_access_plugin.h>
#include <services/database/database_plugin.h>
#include <tests/common/test_environment.h>
#include <unistd.h>
#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>

using namespace flowsql;
using namespace flowsql::dataset;
class Runtime : public IQuerier {
 public:
    database::DatabasePlugin database;
    int Traverse(const Guid& iid, fntraverse visitor) override {
        if (auto* pointer = First(iid)) return visitor(pointer);
        return 0;
    }
    void* First(const Guid& iid) override {
        if (!std::memcmp(&iid, &IID_DATABASE_CHANNEL_LEASE_PROVIDER, sizeof(Guid)))
            return static_cast<IDatabaseChannelLeaseProvider*>(&database);
        if (!std::memcmp(&iid, &IID_CHANNEL_DATASET_DESCRIPTOR, sizeof(Guid)))
            return static_cast<IChannelDatasetDescriptor*>(&database);
        return nullptr;
    }
};
int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string backend = argv[1];
    auto parameters = test::DatabaseParameters(backend);
    setenv("FLOWSQL_ACCESS_BACKEND_PASSWORD", parameters.at("password").c_str(), 1);
    parameters["password"] = "${FLOWSQL_ACCESS_BACKEND_PASSWORD}";
    std::string options = "type=" + backend + ";name=business";
    for (const auto& [key, value] : parameters) options += ";" + key + "=" + value;
    Runtime runtime;
    assert(runtime.database.Option(options.c_str()) == 0 && runtime.database.Load(&runtime) == 0);
    assert(runtime.database.Start() == 0);
    auto channel = runtime.database.AcquireChannel(backend.c_str(), "business");
    assert(channel);
    const auto table = "access_" + std::to_string(getpid()), view = table + "_view", slow = table + "_slow";
    auto quote = [&](const std::string& name) {
        const std::string delimiter = backend == "postgres" ? "\"" : "`";
        return delimiter + name + delimiter;
    };
    auto execute = [&](const std::string& sql) { assert(channel->ExecuteSql(sql.c_str()) >= 0); };
    const auto definition =
        backend == "clickhouse"
            ? "(id Int64,amount Nullable(Float64),day String,precise Nullable(Decimal(30,10))) ENGINE=Memory"
            : "(id BIGINT,amount " + std::string(backend == "postgres" ? "DOUBLE PRECISION" : "DOUBLE") +
                  ",day VARCHAR(30),precise DECIMAL(30,10))";
    execute("CREATE TABLE " + quote(table) + definition);
    execute("INSERT INTO " + quote(table) +
            " VALUES(1,10.5,'2026-11-01',0.1),"
            "(9007199254740993,20.5,'2026-11-02',123.0000000001),(3,NULL,'2026-11-03',NULL)");
    execute("CREATE VIEW " + quote(view) + " AS SELECT * FROM " + quote(table));
    const auto slow_sql = backend == "postgres" ? "SELECT 1 AS amount FROM pg_sleep(10)"
                          : backend == "mysql"  ? "SELECT SLEEP(10) AS amount"
                                                : "SELECT sum(sipHash64(number)) AS amount FROM numbers(10000000000)";
    execute("CREATE VIEW " + quote(slow) + " AS " + slow_sql);
    const auto root = test::WorkspacePath(table);
    std::filesystem::create_directories(root);
    Json policy(rapidjson::kObjectType);
    auto& a = policy.GetAllocator();
    Value subject(rapidjson::kObjectType), grants(rapidjson::kArrayType), subjects(rapidjson::kArrayType);
    Set(subject, "principal", "alice", a);
    Set(subject, "token_env", "FLOWSQL_ACCESS_BACKEND_TOKEN", a);
    for (const auto& name : {table, view, slow}) {
        auto grant = access::Parse(R"({"locator":{"kind":"database_table","name":"business"},"operations":["query"]})");
        auto& ga = grant.GetAllocator();
        Set(grant["locator"], "type", backend, ga);
        Set(grant["locator"], "database", parameters.at("database"), ga);
        if (backend == "postgres") Set(grant["locator"], "schema", "public", ga);
        Set(grant["locator"], "table", name, ga);
        if (name != slow)
            grant.AddMember(
                "filters",
                Copy(access::Parse(
                         R"({"f":[{"fid":"day","rule":{"type":"comparison","op":"gte","value":"2026-11-02"}}]})")["f"],
                     ga),
                ga);
        grants.PushBack(Copy(grant, a), a);
    }
    subject.AddMember("sources", grants, a);
    subjects.PushBack(subject, a);
    policy.AddMember("subjects", subjects, a);
    std::ofstream(root + "/policy.json") << Encode(policy);
    const std::string token = "isolated-backend-test-credential-00000000";
    setenv("FLOWSQL_ACCESS_BACKEND_TOKEN", token.c_str(), 1);
    httplib::Server reservation;
    const auto port = reservation.bind_to_any_port("127.0.0.1");
    assert(port > 0);
    std::thread reserved([&] { reservation.listen_after_bind(); });
    reservation.wait_until_ready();
    reservation.stop();
    reserved.join();
    auto config = access::Parse("{}");
    auto& ca = config.GetAllocator();
    config.AddMember("port", port, ca);
    Set(config, "policy_file", root + "/policy.json", ca);
    Set(config, "state_path", root + "/state.db", ca);
    Set(config, "contract_file", std::string(FLOWSQL_SOURCE_DIR) + "/../config/agent-data-access/api.schema.json", ca);
    access::DataAccessPlugin plugin;
    assert(plugin.Option(Encode(config).c_str()) == 0 && plugin.Load(&runtime) == 0 && plugin.Start() == 0);
    httplib::Client client("127.0.0.1", port);
    client.set_read_timeout(10);
    auto call = [&](const char* name, const Value& args) {
        auto body = access::Parse("{}");
        Set(body, "name", name, body.GetAllocator());
        body.AddMember("arguments", Copy(args, body.GetAllocator()), body.GetAllocator());
        auto response =
            client.Post("/data-access/call", {{"Authorization", "Bearer " + token}}, Encode(body), "application/json");
        assert(response && response->status == 200);
        return access::Parse(response->body);
    };
    auto wait = [&](const std::string& id) {
        auto args = access::Parse("{}");
        Set(args, "operation_id", id, args.GetAllocator());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        while (true) {
            auto state = call("get_operation", args);
            const auto status = Get(state, "status");
            if (status != "queued" && status != "running" && status != "cancelling") return state;
            assert(std::chrono::steady_clock::now() < deadline);
            usleep(10000);
        }
    };
    auto listing = call("list_sources", access::Parse("{}"));
    assert(listing["sources"].Size() == 3);
    for (unsigned index : {0u, 1u}) {
        const auto& source = listing["sources"][index];
        assert(source["capabilities"]["query"]["available"].GetBool());
        auto query = access::Parse(
            R"({"request_id":"aggregate","workflow":[{"type":"view","query":[{"op":"aggregate","groupBy":[],"measures":[{"field":"f1","agg":"sum","asFieldKey":"s"},{"field":"*","agg":"count","asFieldKey":"c"}]}]}]})");
        Set(query, "request_id", "aggregate" + std::to_string(index), query.GetAllocator());
        query.AddMember("source_ref", Copy(source["source_ref"], query.GetAllocator()), query.GetAllocator());
        auto record = wait(Get(call("query", query), "operation_id"));
        if (Get(record, "status") != "completed") std::fprintf(stderr, "%s\n", Encode(record).c_str());
        assert(Get(record, "status") == "completed");
        auto args = access::Parse("{}");
        Set(args, "result_ref", Get(record, "result_ref"), args.GetAllocator());
        auto result = call("read_result", args);
        assert(std::stod(Get(result["rows"][0][0], "value")) == 20.5);
        assert(Get(result["rows"][0][1], "value") == "2");
        auto raw = access::Parse(
            R"({"request_id":"exact","workflow":[{"type":"view","query":[{"op":"raw","fields":["f0","f3"]}]},{"type":"sort","by":["f0"],"sort":"descending"}]})");
        Set(raw, "request_id", "exact" + std::to_string(index), raw.GetAllocator());
        raw.AddMember("source_ref", Copy(source["source_ref"], raw.GetAllocator()), raw.GetAllocator());
        record = wait(Get(call("query", raw), "operation_id"));
        assert(Get(record, "status") == "completed");
        Set(args, "result_ref", Get(record, "result_ref"), args.GetAllocator());
        result = call("read_result", args);
        assert(Get(result["rows"][0][0], "value") == "9007199254740993");
        assert(Get(result["rows"][0][1], "type") == "decimal");
        assert(Get(result["rows"][0][1], "value") == "123.0000000001");
        assert(result["rows"][1][1].IsNull());
        raw.AddMember("max_rows", 1, raw.GetAllocator());
        Set(raw, "request_id", "overflow" + std::to_string(index), raw.GetAllocator());
        record = wait(Get(call("query", raw), "operation_id"));
        assert(Get(record, "status") == "failed" && Get(record, "error_code") == "RESULT_LIMIT");
    }
    auto query = access::Parse(
        R"({"request_id":"timeout","timeout_ms":10,"workflow":[{"type":"view","query":[{"op":"raw","fields":["f0"]}]}]})");
    query.AddMember("source_ref", Copy(listing["sources"][2]["source_ref"], query.GetAllocator()),
                    query.GetAllocator());
    auto record = wait(Get(call("query", query), "operation_id"));
    assert(Get(record, "status") == "failed" && Get(record, "error_code") == "TIMEOUT");
    query["timeout_ms"].SetInt(5000);
    Set(query, "request_id", "cancel", query.GetAllocator());
    auto args = access::Parse("{}");
    Set(args, "operation_id", Get(call("query", query), "operation_id"), args.GetAllocator());
    while (Get(call("get_operation", args), "status") == "queued") usleep(1000);
    usleep(50000);
    assert(Get(call("get_operation", args), "status") == "running");
    call("cancel_operation", args);
    assert(Get(wait(Get(args, "operation_id")), "status") == "cancelled");
    plugin.Stop();
    execute("DROP VIEW " + quote(slow));
    execute("DROP VIEW " + quote(view));
    execute("DROP TABLE " + quote(table));
    channel.reset();
    runtime.database.Stop();
    std::puts("[PASS] controlled table/view scope, exact values, budget, timeout and cancellation");
}
