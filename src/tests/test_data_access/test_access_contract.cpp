// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <services/data_access/data_access_plugin.h>
#include <services/database/database_plugin.h>
#include <unistd.h>
#include <common/iquerier.hpp>

#include <cassert>
#include <filesystem>
#include <fstream>

using namespace flowsql;
using namespace flowsql::dataset;

class TestQuerier : public IQuerier {
 public:
    database::DatabasePlugin database;
    int Traverse(const Guid& iid, fntraverse visitor) override {
        if (auto* pointer = First(iid)) return visitor(pointer);
        return 0;
    }
    void* First(const Guid& iid) override {
        if (!std::memcmp(&iid, &IID_DATABASE_FACTORY, sizeof(Guid))) return static_cast<IDatabaseFactory*>(&database);
        if (!std::memcmp(&iid, &IID_DATABASE_CHANNEL_LEASE_PROVIDER, sizeof(Guid)))
            return static_cast<IDatabaseChannelLeaseProvider*>(&database);
        if (!std::memcmp(&iid, &IID_CHANNEL_DATASET_DESCRIPTOR, sizeof(Guid)))
            return static_cast<IChannelDatasetDescriptor*>(&database);
        return nullptr;
    }
};

int main() {
    char directory[] = "/tmp/flowsql-access-contract-XXXXXX";
    assert(mkdtemp(directory));
    const std::string root = directory;
    TestQuerier querier;
    const auto database_file = root + "/fixture.db";
    assert(querier.database.Option(("type=sqlite;name=business;path=" + database_file).c_str()) == 0);
    assert(querier.database.Load(&querier) == 0);
    assert(querier.database.Start() == 0);
    auto channel = querier.database.AcquireChannel("sqlite", "business");
    assert(channel);
    assert(channel->ExecuteSql("CREATE TABLE sales (id INTEGER, amount REAL, business_date TEXT)") >= 0);
    assert(channel->ExecuteSql("INSERT INTO sales VALUES(1,10.5,'2026-11-01'),(2,20.5,'2026-11-02')") >= 0);
    assert(channel->ExecuteSql("CREATE VIEW slow_sales AS WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM "
                               "n WHERE x<10000) SELECT sum(a.x*b.x) AS amount FROM n a,n b") >= 0);
    channel.reset();
    assert(setenv("FLOWSQL_ACCESS_TEST_ALICE", "alice-secret-for-contract-test-only-32", 1) == 0);
    std::ofstream(root + "/policy.json")
        << R"({"subjects":[{"principal":"alice","token_env":"FLOWSQL_ACCESS_TEST_ALICE",
       "sources":[{"locator":{"kind":"database_table","type":"sqlite","name":"business","schema":"main","table":"sales"},"operations":["query"],"filters":[{"fid":"business_date","rule":{"type":"comparison","op":"gte","value":"2026-11-02"}}]},
       {"locator":{"kind":"block_stream","type":"netadapter","name":"absent"},"operations":[]},
       {"locator":{"kind":"database_table","type":"sqlite","name":"business","schema":"main","table":"slow_sales"},"operations":["query"]}]}]})";

    httplib::Server reservation;
    const int port = reservation.bind_to_any_port("127.0.0.1");
    assert(port > 0);
    std::thread reserving([&] { reservation.listen_after_bind(); });
    reservation.wait_until_ready();
    reservation.stop();
    reserving.join();
    Json options(rapidjson::kObjectType);
    auto& a = options.GetAllocator();
    Set(options, "host", "127.0.0.1", a);
    options.AddMember("port", port, a);
    Set(options, "policy_file", root + "/policy.json", a);
    Set(options, "contract_file", std::string(FLOWSQL_SOURCE_DIR) + "/../config/agent-data-access/api.schema.json", a);
    Set(options, "state_path", root + "/state.db", a);

    access::DataAccessPlugin plugin;
    assert(plugin.Option(Encode(options).c_str()) == 0);
    assert(plugin.Load(&querier) == 0);
    assert(plugin.Start() == 0);
    httplib::Client client("127.0.0.1", port);
    client.set_read_timeout(10);
    const httplib::Headers headers{{"Authorization", "Bearer alice-secret-for-contract-test-only-32"}};
    auto unauthenticated = client.Get("/data-access/tools");
    if (!unauthenticated)
        std::fprintf(stderr, "access transport failed: %s\n", httplib::to_string(unauthenticated.error()).c_str());
    assert(unauthenticated && unauthenticated->status == 401);
    auto list =
        client.Post("/data-access/call", headers, R"({"name":"list_sources","arguments":{}})", "application/json");
    assert(list && list->status == 200);
    auto result = access::Parse(list->body);
    assert(result["sources"].Size() == 3);
    const auto& source = result["sources"][0];
    assert(Get(source["description"], "schema_state") == "known");
    assert(source["description"]["fields"].Size() == 3);
    assert(source["capabilities"]["query"]["available"].GetBool());
    assert(!result["sources"][1]["capabilities"]["capture"]["available"].GetBool());
    assert(list->body.find("password") == std::string::npos);
    assert(list->body.find(database_file) == std::string::npos);
    assert(list->body.find("10.5") == std::string::npos);
    auto operation_listing =
        client.Post("/data-access/call", headers, R"({"name":"list_operations","arguments":{}})", "application/json");
    assert(operation_listing && operation_listing->status == 200);
    assert(access::Parse(operation_listing->body)["operations"].Empty());
    auto forged_analysis = client.Post("/data-access/call", headers,
                                       R"({"name":"submit_analysis","arguments":{"request_id":"forged-analysis",
            "operation_ref":{"operation_id":"ungranted","operation_revision":"unknown"},
            "source_ref":{"source_id":"forged","source_revision":"forged"},"output":"/tmp/forged"}})",
                                       "application/json");
    assert(forged_analysis && forged_analysis->status == 400);
    auto page = client.Post("/data-access/call", headers,
                            R"({"name":"list_sources","arguments":{"limit":1,"offset":1}})", "application/json");
    assert(page && page->status == 200);
    auto page_json = access::Parse(page->body);
    assert(page_json["sources"].Size() == 1 && page_json["has_more"].GetBool());
    assert(page_json["total_sources"].GetUint64() == 3);
    auto rejected = client.Post("/data-access/call", headers,
                                R"({"name":"list_sources","arguments":{"principal":"other"}})", "application/json");
    assert(rejected && rejected->status == 400);
    auto bypass = client.Post("/channels/database/explore", headers, "{}", "application/json");
    assert(bypass && bypass->status == 404);
    auto call = [&](const char* name, const Value& args, int status = 200) {
        Json body(rapidjson::kObjectType);
        Set(body, "name", name, body.GetAllocator());
        body.AddMember("arguments", Copy(args, body.GetAllocator()), body.GetAllocator());
        auto response = client.Post("/data-access/call", headers, Encode(body), "application/json");
        if (!response || response->status != status)
            std::fprintf(stderr, "tool %s failed: %s\n", name, response ? response->body.c_str() : "transport");
        assert(response && response->status == status);
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
    auto query = access::Parse(
        R"({"request_id":"sum","workflow":[{"type":"view","query":[{"op":"aggregate","groupBy":[],"measures":[{"field":"f1","agg":"sum","asFieldKey":"total"},{"field":"*","agg":"count","asFieldKey":"count"}]}]}]})");
    query.AddMember("source_ref", Copy(source["source_ref"], query.GetAllocator()), query.GetAllocator());
    auto submitted = call("query", query);
    auto completed = wait(Get(submitted, "operation_id"));
    assert(Get(completed, "status") == "completed");
    assert(Get(call("query", query), "operation_id") == Get(submitted, "operation_id"));
    auto read_args = access::Parse("{}");
    Set(read_args, "result_ref", Get(completed, "result_ref"), read_args.GetAllocator());
    auto data = call("read_result", read_args);
    assert(Get(data["rows"][0][0], "value") == "20.5");
    assert(Get(data["rows"][0][1], "value") == "1");
    assert(Get(data["basis"], "sql").find(" WHERE ") != std::string::npos);
    query.AddMember("limit", 1, query.GetAllocator());
    assert(Get(call("query", query, 409)["error"], "code") == "REQUEST_CONFLICT");
    auto sql = access::Parse(R"json({"request_id":"sql","sql":"SELECT load_extension('bad')","workflow":[]})json");
    sql.AddMember("source_ref", Copy(source["source_ref"], sql.GetAllocator()), sql.GetAllocator());
    call("query", sql, 400);
    auto slow = access::Parse(
        R"({"request_id":"slow","timeout_ms":10,"workflow":[{"type":"view","query":[{"op":"raw","fields":["f0"]}]}]})");
    slow.AddMember("source_ref", Copy(result["sources"][2]["source_ref"], slow.GetAllocator()), slow.GetAllocator());
    auto timed = wait(Get(call("query", slow), "operation_id"));
    assert(Get(timed, "status") == "failed" && Get(timed, "error_code") == "TIMEOUT");
    Set(slow, "request_id", "cancel", slow.GetAllocator());
    slow["timeout_ms"].SetInt(5000);
    auto cancel_args = access::Parse("{}");
    const auto cancelled_id = Get(call("query", slow), "operation_id");
    Set(cancel_args, "operation_id", cancelled_id, cancel_args.GetAllocator());
    while (Get(call("get_operation", cancel_args), "status") == "queued") usleep(1000);
    usleep(30000);
    assert(Get(call("get_operation", cancel_args), "status") == "running");
    const auto cancellation_started = std::chrono::steady_clock::now();
    call("cancel_operation", cancel_args);
    assert(Get(wait(cancelled_id), "status") == "cancelled");
    assert(std::chrono::steady_clock::now() - cancellation_started < std::chrono::seconds(2));
    Set(query, "request_id", "after_cancel", query.GetAllocator());
    assert(Get(wait(Get(call("query", query), "operation_id")), "status") == "completed");
    plugin.Stop();
    querier.database.Stop();
    // The private test fixture owns only files inside its unique temporary root.
    for (const auto& entry : std::filesystem::directory_iterator(root)) std::filesystem::remove(entry.path());
    rmdir(root.c_str());
    std::puts("[PASS] controlled access authentication, source basis, schema and route isolation");
}
