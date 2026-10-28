// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <channels/config/channel_dictionary.h>
#include <sqlite3.h>
#include <tests/common/test_environment.h>
#include <atomic>
#include <cassert>
#include <common/channel_dataset.hpp>
#include <condition_variable>
#include <future>
#include <iostream>
#include <thread>

using namespace flowsql;
using namespace flowsql::dataset;
using namespace flowsql::channels::config;

Json Call(ChannelDictionaryService& service, const std::string& op, const Json& request,
          const char* expected = nullptr) {
    std::string response;
    const int rc = service.Control(op, Encode(request), response);
    Json d;
    d.Parse(response.c_str());
    if (expected) {
        if (rc == 0 || Get(d["error"], "code") != expected)
            std::cerr << "expected " << expected << ": " << response << std::endl;
        assert(rc != 0 && Get(d["error"], "code") == expected);
    } else {
        if (rc) std::cerr << response << std::endl;
        assert(rc == 0);
    }
    return d;
}
Json ApplyRequest(const Json& request, const Value& preview, const std::string& name, const std::string& ref,
                  uint64_t expected) {
    Json result;
    result.CopyFrom(request, result.GetAllocator());
    auto& a = result.GetAllocator();
    Set(result, "name", name, a);
    Set(result, "dictionary_ref", ref, a);
    result.AddMember("expected_current_revision", expected, a);
    result.AddMember("field_map", Copy(preview["field_map"], a), a);
    result.AddMember("confirmations", Copy(preview["required_confirmations"], a), a);
    Value basis(rapidjson::kObjectType);
    for (const char* key : {"object_key", "source_revision", "schema_fingerprint"})
        basis.AddMember(Value(key, a), Copy(preview["source_snapshot"][key], a), a);
    result.AddMember("source_basis", basis, a);
    return result;
}
int main() {
    const auto path = test::WorkspacePath("channel-consistency.db");
    DataSemanticsRegistry registry, second;
    DataSemanticsErrorV1 error;
    assert(registry.Open(path, &error) == 0 && second.Open(path, &error) == 0);
    Json locator;
    locator.Parse(R"({"kind":"dataframe","name":"sample"})");
    Json fields(rapidjson::kArrayType), metadata(rapidjson::kObjectType);
    fields.PushBack(Field("id", Type("arrow", "int64", fields.GetAllocator()), false, fields.GetAllocator()),
                    fields.GetAllocator());
    auto source = Snapshot("dataframe", "sample-id", locator, fields, metadata, "1");
    auto describe = [&](const auto&, auto* out, auto*) {
        out->document_json = std::make_shared<const std::string>(Encode(source));
        return 0;
    };
    ChannelDictionaryService service(registry, describe), peer(second, describe);
    Json request(rapidjson::kObjectType);
    request.AddMember("locator", Copy(locator, request.GetAllocator()), request.GetAllocator());
    const auto generated = Call(service, "preview", request);
    SemanticsSaveRequest dictionary{"dictionary", "sample", Encode(generated["data"]["dictionary"])};
    SemanticsSaveResult saved;
    assert(registry.Save(dictionary, &saved, &error) == 0);
    const auto dictionary_ref = saved.snapshot.exact_reference;
    // A fully self-consistent imported snapshot/confirmation still has no server observation authority.
    Json forged(rapidjson::kObjectType);
    auto& a = forged.GetAllocator();
    Set(forged, "schema_id", "flowsql.data-binding.v2", a);
    Set(forged, "object_label", "sample", a);
    Set(forged, "dictionary_ref", dictionary_ref, a);
    forged.AddMember("object_key", Copy(source["object_key"], a), a);
    forged.AddMember("source_snapshot", Copy(source, a), a);
    forged.AddMember("field_map", Copy(generated["data"]["field_map"], a), a);
    forged.AddMember("declared_schema", Value(), a);
    Value confirmation(rapidjson::kObjectType), items(rapidjson::kArrayType);
    items.PushBack("observation", a);
    Set(confirmation, "source_revision", Get(source, "source_revision"), a);
    confirmation.AddMember("items", items, a);
    forged.AddMember("confirmation", confirmation, a);
    SemanticsSaveRequest import{"binding", "sample", Encode(forged)};
    assert(registry.Save(import, &saved, &error) == 0);
    assert(Get(Call(service, "check", request)["data"], "state") == "needs_confirmation");
    bool trusted = true;
    assert(registry.ObservationTrusted("binding.sample@1", &trusted, &error) == 0 && !trusted);
    auto preview = Call(service, "preview", request);
    auto apply = ApplyRequest(request, preview["data"], "sample", dictionary_ref, 1);
    sqlite3* db = nullptr;
    assert(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
    auto inject = [&] {
        assert(sqlite3_exec(db,
                            "CREATE TRIGGER reject_observation BEFORE INSERT ON semantics_observation BEGIN "
                            "SELECT RAISE(ABORT,'injected observation failure'); END",
                            nullptr, nullptr, nullptr) == SQLITE_OK);
    };
    auto clear = [&] {
        assert(sqlite3_exec(db, "DROP TRIGGER reject_observation", nullptr, nullptr, nullptr) == SQLITE_OK);
    };
    inject();
    Call(service, "apply", apply, "STORAGE_ERROR");
    assert(registry.ObservationTrusted("binding.sample@1", &trusted, &error) == 0 && !trusted);
    clear();
    auto applied = Call(service, "apply", apply);
    assert(!applied["data"]["created_revision"]
                .GetBool());  // authority can be established without a fake content revision
    assert(Get(Call(service, "check", request)["data"], "state") == "matched");
    // Failure after revision/current/reference writes rolls back the entire new binding and observation.
    source = Snapshot("dataframe", "failed-id", locator, fields, metadata, "1");
    preview = Call(service, "preview", request);
    apply = ApplyRequest(request, preview["data"], "failed", dictionary_ref, 0);
    apply["field_map"].CopyFrom(generated["data"]["field_map"], apply.GetAllocator());
    inject();
    Call(service, "apply", apply, "STORAGE_ERROR");
    DataSemanticsSnapshotV1 snapshot;
    assert(registry.Resolve("binding.failed@1", &snapshot, &error) != 0 && error.code == "NOT_FOUND");
    assert(Get(Call(service, "check", request)["data"], "state") == "unbound");
    assert(registry.ObservationTrusted("binding.failed@1", &trusted, &error) == 0 && !trusted);
    clear();
    sqlite3_close(db);
    // Concurrent independent clients cannot both publish differing applications from the same expected revision.
    source = Snapshot("dataframe", "sample-id", locator, fields, metadata, "1");
    Json alternative;
    alternative.CopyFrom(generated["data"]["dictionary"], alternative.GetAllocator());
    Set(alternative, "title", "alternative", alternative.GetAllocator());
    SemanticsSaveRequest other{"dictionary", "alternative", Encode(alternative)};
    assert(registry.Save(other, &saved, &error) == 0);
    const auto alternative_ref = saved.snapshot.exact_reference;
    other.name = "alternative_b";
    assert(registry.Save(other, &saved, &error) == 0);
    preview = Call(service, "preview", request);
    auto one = ApplyRequest(request, preview["data"], "sample", alternative_ref, 1);
    auto two = ApplyRequest(request, preview["data"], "sample", saved.snapshot.exact_reference, 1);
    std::atomic<int> passes{0}, conflicts{0};
    auto publish = [&](ChannelDictionaryService& target, const Json& command) {
        std::string response;
        const int rc = target.Control("apply", Encode(command), response);
        Json d;
        d.Parse(response.c_str());
        if (!rc)
            ++passes;
        else {
            assert(Get(d["error"], "code") == "REVISION_CONFLICT");
            ++conflicts;
        }
    };
    std::thread first(publish, std::ref(service), std::cref(one)), another(publish, std::ref(peer), std::cref(two));
    first.join();
    another.join();
    assert(passes == 1 && conflicts == 1);
    // Explicit v1 upgrade retains both its historical declaration and the separate observed structure.
    source = Snapshot("dataframe", "legacy-id", locator, fields, metadata, "1");
    Json legacy;
    legacy.CopyFrom(forged, legacy.GetAllocator());
    Set(legacy, "schema_id", "flowsql.data-binding.v1", legacy.GetAllocator());
    legacy.RemoveMember("source_snapshot");
    legacy.RemoveMember("confirmation");
    legacy["object_key"].CopyFrom(source["object_key"], legacy.GetAllocator());
    legacy["declared_schema"].CopyFrom(fields, legacy.GetAllocator());
    SemanticsSaveRequest old{"binding", "legacy", Encode(legacy)};
    assert(registry.Save(old, &saved, &error) == 0);
    preview = Call(service, "preview", request);
    apply = ApplyRequest(request, preview["data"], "legacy", dictionary_ref, 1);
    Call(service, "apply", apply);
    assert(registry.Resolve("binding.legacy@2", &snapshot, &error) == 0);
    Json upgraded;
    upgraded.Parse(snapshot.document_json->c_str());
    assert(upgraded["declared_schema"] == fields && upgraded.HasMember("source_snapshot"));
    assert(registry.Resolve("binding.legacy@1", &snapshot, &error) == 0);
    Json historical;
    historical.Parse(snapshot.document_json->c_str());
    assert(Get(historical, "schema_id") == "flowsql.data-binding.v1" && !historical.HasMember("source_snapshot"));
    source = Snapshot("dataframe", "sample-id", locator, fields, metadata, "1");
    // A blocked provider does not hold the registry write lock.
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, released = false;
    ChannelDictionaryService blocked(registry, [&](const auto& r, auto* out, auto* e) {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        changed.notify_all();
        changed.wait(lock, [&] { return released; });
        return describe(r, out, e);
    });
    auto reader = std::async(std::launch::async, [&] { return Call(blocked, "check", request); });
    {
        std::unique_lock<std::mutex> lock(mutex);
        assert(changed.wait_for(lock, std::chrono::seconds(2), [&] { return entered; }));
    }
    auto writer = std::async(std::launch::async, [&] {
        SemanticsSaveRequest r{"standard", "unrelated",
                               R"({"schema_id":"flowsql.data-standard.v1","title":"parallel"})"};
        SemanticsSaveResult result;
        DataSemanticsErrorV1 e;
        return registry.Save(r, &result, &e);
    });
    const bool completed = writer.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    {
        std::lock_guard<std::mutex> lock(mutex);
        released = true;
    }
    changed.notify_all();
    assert(completed && writer.get() == 0);
    reader.get();
    // Preserve the authority and immutable exact documents across independent registry reopen.
    second.Close();
    registry.Close();
    assert(registry.Open(path, &error) == 0);
    assert(registry.ObservationTrusted("binding.sample@1", &trusted, &error) == 0 && trusted);
    assert(Get(Call(service, "check", request)["data"], "state") == "matched");
    std::cout << "PASS A7: imported claims, atomic observation rollback, concurrent apply, no source write lock and "
                 "recovery\n";
}
