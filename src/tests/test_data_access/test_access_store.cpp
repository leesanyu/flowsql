// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <services/data_access/access_store.h>
#include <services/data_access/data_access_plugin.h>
#include <unistd.h>

#include <cassert>
#include <filesystem>

using namespace flowsql::access;
using namespace flowsql::dataset;
template <class F>
void Error(const std::string& code, F action) {
    bool rejected = false;
    try {
        action();
    } catch (const AccessError& e) {
        rejected = e.code == code;
    }
    assert(rejected);
}
int main() {
    char directory[] = "/tmp/flowsql-access-store-XXXXXX";
    assert(mkdtemp(directory));
    const std::string path = std::string(directory) + "/state.db";
    std::string operation, result, lease, pending, analysis;
    {
        AccessStore store(path);
        bool created;
        auto record = store.Register("alice", "request", "hash", "query", &created);
        assert(created);
        operation = Get(record, "operation_id");
        auto retry = store.Register("alice", "request", "hash", "query", &created);
        assert(!created && Get(retry, "operation_id") == operation);
        Error("REQUEST_CONFLICT", [&] { store.Register("alice", "request", "other", "query", &created); });
        Error("NOT_FOUND", [&] { store.Find("bob", operation); });
        auto payload = Parse(
            R"({"fields":[{"name":"exact"}],"rows":[[{"type":"int64","value":"9223372036854775807"}],[null],[{"type":"decimal","value":"123.0000000000000000001"}]],"basis":{"scope":"complete"}})");
        auto completed = store.Complete("alice", operation, payload, 40);
        result = Get(completed, "result_ref");
        auto first = store.Read("alice", result, 0, 1);
        assert(Get(first["rows"][0][0], "value") == "9223372036854775807");
        assert(first["total_rows"].GetUint64() == 3);
        auto held = store.AcquireLease("alice", result, 30000);
        lease = Get(held, "lease_id");
        Error("NOT_FOUND", [&] { store.ReleaseLease("bob", lease); });
        Error("LEASE_UNAVAILABLE", [&] { store.Read("alice", result, 0, 1, "unowned-lease"); });
        Error("NOT_FOUND", [&] { store.Read("bob", result, 0, 1); });
        auto next = store.Read("alice", result, 1, 2, lease);
        assert(next["rows"][0][0].IsNull());
        assert(Get(first, "content_sha256") == Get(next, "content_sha256"));
        std::string copied = Get(first, "integrity_header") + "\n";
        for (const auto* page : {&first, &next})
            for (const auto& row : (*page)["row_json"].GetArray()) copied += Text(row) + "\n";
        assert(Hash(copied) == Get(first, "content_sha256"));
        analysis = Get(store.Register("alice", "analysis", "analysis-hash", "analysis", &created), "operation_id");
        pending = Get(store.Register("alice", "pending", "pending-hash", "query", &created), "operation_id");
    }
    usleep(50000);
    {
        AccessStore store(path);
        assert(Get(store.Find("alice", operation), "status") == "completed");
        assert(Get(store.Find("alice", pending), "status") == "interrupted");
        assert(Get(store.Find("alice", analysis), "status") == "reconcile_required");
        assert(store.Outputs("alice").Empty());
        Error("RESULT_EXPIRED", [&] { store.Read("alice", result, 0, 1); });
        auto page = store.Read("alice", result, 2, 1, lease);
        assert(Get(page["rows"][0][0], "value") == "123.0000000000000000001");
        store.ReleaseLease("alice", lease);
        Error("LEASE_UNAVAILABLE", [&] { store.Read("alice", result, 0, 1, lease); });
        Error("RESULT_EXPIRED", [&] { store.AcquireLease("alice", result, 1000); });
    }
    for (const auto& entry : std::filesystem::directory_iterator(directory)) std::filesystem::remove(entry.path());
    rmdir(directory);
    std::puts("[PASS] durable identities, immutable paging, exact values, leases, ownership and restart");
}
