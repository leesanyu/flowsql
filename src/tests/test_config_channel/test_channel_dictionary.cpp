// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <channels/config/channel_dictionary.h>
#include <sqlite3.h>
#include <unistd.h>
#include <cassert>
#include <common/channel_dataset.hpp>
#include <iostream>
using namespace flowsql;
using namespace flowsql::dataset;
using namespace flowsql::channels::config;

int main() {
    char file[] = "/tmp/flowsql_channel_dictionary_XXXXXX";
    const int fd = mkstemp(file);
    assert(fd >= 0);
    close(fd);
    DataSemanticsRegistry registry;
    DataSemanticsErrorV1 error;
    assert(registry.Open(file, &error) == 0);
    Json locator;
    locator.Parse(R"({"kind":"dataframe","name":"requests"})");
    Json fields(rapidjson::kArrayType);
    auto& fa = fields.GetAllocator();
    fields.PushBack(Field("id", Type("arrow", "int64", fa), false, fa), fa);
    fields.PushBack(Field("duration", Type("arrow", "double", fa), true, fa), fa);
    Json metadata(rapidjson::kObjectType);
    auto source = Snapshot("dataframe", "persistent-id", locator, fields, metadata, "1");
    int calls = 0;
    bool offline = false;
    ChannelDictionaryService service(registry, [&](const auto& r, auto* out, auto* e) {
        ++calls;
        assert(r.timeout_ms > 0 && r.timeout_ms <= 30000);
        if (offline) return Fail(e, "SOURCE_UNAVAILABLE", "injected failure");
        out->document_json = std::make_shared<const std::string>(Encode(source));
        return 0;
    });
    auto call = [&](const std::string& op, const Value& req, bool success = true, const std::string& code = "") {
        std::string response;
        const auto rc = service.Control(op, Encode(req), response);
        Json out;
        out.Parse(response.c_str());
        assert(!out.HasParseError());
        if (success && rc != 0) std::cerr << response << '\n';
        assert((rc == 0) == success);
        if (!success) {
            if (Get(out["error"], "code") != code) std::cerr << "expected " << code << ": " << response << '\n';
            assert(Get(out["error"], "code") == code);
        }
        return out;
    };
    Json req(rapidjson::kObjectType);
    req.AddMember("locator", Copy(locator, req.GetAllocator()), req.GetAllocator());
    auto description = call("describe", req);
    assert(description["data"]["source_snapshot"]["fields"].Size() == 2);
    auto preview = call("preview", req);
    assert(preview["data"]["dictionary"]["fields"].Size() == 2);
    assert(preview["data"]["dictionary"]["entity"]["row_meaning"]["state"] == "unknown");
    assert(preview["data"]["dictionary"]["fields"][1]["unit"]["state"] == "unknown");
    auto unbound = call("check", req);
    assert(unbound["data"]["state"] == "unbound");
    DataSemanticsSnapshotV1 absent;
    assert(registry.BindingLookup("dataframe", "persistent-id", &absent, &error) != 0 && error.code == "NOT_FOUND");
    Json dictionary;
    dictionary.CopyFrom(preview["data"]["dictionary"], dictionary.GetAllocator());
    auto& a = dictionary.GetAllocator();
    const auto duration_id = Get(dictionary["fields"][1], "field_id"), id_id = Get(dictionary["fields"][0], "field_id");
    Json manual;
    manual.Parse(R"({"state":"declared","value":"人工请求耗时说明","origin":"manual"})");
    Set(dictionary["fields"][1], "definition", Copy(manual, a), a);
    SemanticsSaveRequest save{"dictionary", "requests", Encode(dictionary)};
    SemanticsSaveResult saved;
    assert(registry.Save(save, &saved, &error) == 0);
    Json apply(rapidjson::kObjectType);
    auto& aa = apply.GetAllocator();
    apply.AddMember("locator", Copy(locator, aa), aa);
    Set(apply, "name", "requests", aa);
    Set(apply, "dictionary_ref", saved.snapshot.exact_reference, aa);
    apply.AddMember("field_map", Copy(preview["data"]["field_map"], aa), aa)
        .AddMember("expected_current_revision", 0, aa);
    Value basis(rapidjson::kObjectType);
    basis.AddMember("object_key", Copy(source["object_key"], aa), aa)
        .AddMember("source_revision", Copy(source["source_revision"], aa), aa)
        .AddMember("schema_fingerprint", Copy(source["schema_fingerprint"], aa), aa);
    apply.AddMember("source_basis", basis, aa).AddMember("confirmations", Value(rapidjson::kArrayType), aa);
    auto applied = call("apply", apply);
    assert(applied["data"]["created_revision"].GetBool());
    assert(Get(applied["data"]["check"], "state") == "matched");
    auto same = call("apply", apply);
    assert(!same["data"]["created_revision"].GetBool());
    assert(Get(call("check", req)["data"], "state") == "matched");
    const auto first_binding = Get(applied["data"], "exact_reference");
    auto p2 = call("preview", req);
    assert(Get(p2["data"]["dictionary"]["fields"][1], "field_id") == duration_id);
    assert(Get(p2["data"]["dictionary"]["fields"][1]["definition"], "value") == "人工请求耗时说明");
    // Source mutation after a preview cannot be applied with its stale basis.
    fields[1]["type"]["name"].SetString("int64", fa);
    source = Snapshot("dataframe", "persistent-id", locator, fields, metadata, "1");
    call("apply", apply, false, "SOURCE_CHANGED");
    assert(Get(call("check", req)["data"], "state") == "mismatch");
    auto changed = call("preview", req);
    assert(changed["data"]["required_confirmations"].Size() == 1);
    assert(changed["data"]["differences"][0]["kind"] == "type_changed");
    assert(changed["data"]["dictionary"]["fields"][1]["definition"] == manual);
    save.expected_current_revision = 1;
    save.document_json = Encode(changed["data"]["dictionary"]);
    assert(registry.Save(save, &saved, &error) == 0);
    Set(apply, "dictionary_ref", saved.snapshot.exact_reference, aa);
    apply["expected_current_revision"].SetUint64(1);
    apply["source_basis"]["source_revision"].CopyFrom(source["source_revision"], aa);
    apply["source_basis"]["schema_fingerprint"].CopyFrom(source["schema_fingerprint"], aa);
    call("apply", apply, false, "CONFIRMATION_REQUIRED");
    apply["confirmations"].CopyFrom(changed["data"]["required_confirmations"], aa);
    auto newer = call("apply", apply);
    assert(Get(newer["data"], "exact_reference") == "binding.requests@2");
    assert(Get(call("check", req)["data"], "state") == "matched");
    assert(!call("apply", apply)["data"]["created_revision"].GetBool());
    DataSemanticsSnapshotV1 old;
    assert(registry.Resolve(first_binding.c_str(), &old, &error) == 0);
    assert(old.document_json->find("dictionary.requests@1") != std::string::npos);
    // Explicit rename reuses the field ID and its manual semantics.
    fields[1]["name"].SetString("elapsed", fa);
    source = Snapshot("dataframe", "persistent-id", locator, fields, metadata, "1");
    auto removed = call("preview", req);
    assert(removed["data"]["required_confirmations"].Size() == 1);
    assert(removed["data"]["dictionary"]["fields"].Size() == 3);  // removed field is still available for review
    Value renames(rapidjson::kObjectType);
    Set(renames, duration_id.c_str(), "elapsed", req.GetAllocator());
    req.AddMember("renames", renames, req.GetAllocator());
    auto renamed = call("preview", req);
    assert(renamed["data"]["dictionary"]["fields"].Size() == 2);
    assert(Get(renamed["data"]["dictionary"]["fields"][1], "field_id") == duration_id);
    assert(renamed["data"]["dictionary"]["fields"][1]["definition"] == manual);
    req.RemoveMember("renames");
    // Extra source columns are allowed for reusing a narrower dictionary, but repeated mappings are rejected.
    Json bad;
    bad.CopyFrom(apply, bad.GetAllocator());
    Set(bad["field_map"], duration_id.c_str(), "id", bad.GetAllocator());
    bad["source_basis"]["source_revision"].CopyFrom(source["source_revision"], bad.GetAllocator());
    bad["source_basis"]["schema_fingerprint"].CopyFrom(source["schema_fingerprint"], bad.GetAllocator());
    bad["confirmations"].CopyFrom(removed["data"]["required_confirmations"], bad.GetAllocator());
    call("apply", bad, false, "BINDING_MISMATCH");
    auto has_diff = [](const Value& result, const std::string& kind) {
        for (const auto& diff : result["data"]["differences"].GetArray())
            if (Get(diff, "kind") == kind) return true;
        return false;
    };
    fields[1]["name"].SetString("duration", fa);
    fields[1]["nullable"].SetBool(false);
    source = Snapshot("dataframe", "persistent-id", locator, fields, metadata, "1");
    assert(has_diff(call("preview", req), "nullable_changed"));
    fields[0].Swap(fields[1]);
    source = Snapshot("dataframe", "persistent-id", locator, fields, metadata, "1");
    assert(has_diff(call("preview", req), "order_changed"));
    fields[0].Swap(fields[1]);
    metadata.Parse(R"({"fields":{"duration":{"unit":{"code":"ms","label":"milliseconds"},"path":"/elapsed"}}})");
    source = Snapshot("dataframe", "persistent-id", locator, fields, metadata, "1");
    auto producer = call("preview", req);
    assert(has_diff(producer, "producer_changed"));
    assert(producer["data"]["dictionary"]["fields"][1]["definition"] == manual);
    assert(producer["data"]["dictionary"]["fields"][1]["unit"]["origin"] == "producer");
    Json deletion;
    deletion.CopyFrom(dictionary, deletion.GetAllocator());
    Json identities;
    identities.Parse(R"({"state":"declared","value":[],"origin":"manual"})");
    identities["value"].PushBack(Value(id_id.c_str(), identities.GetAllocator()), identities.GetAllocator());
    Set(deletion["entity"], "identity_fields", Copy(identities, deletion.GetAllocator()), deletion.GetAllocator());
    SemanticsSaveRequest deletion_save{"dictionary", "deletion", Encode(deletion)};
    assert(registry.Save(deletion_save, &saved, &error) == 0);
    fields.Erase(fields.Begin());
    source = Snapshot("dataframe", "persistent-id", locator, fields, metadata, "1");
    Set(req, "base_dictionary_ref", "dictionary.deletion@1", req.GetAllocator());
    Value remove(rapidjson::kArrayType);
    remove.PushBack(Value(id_id.c_str(), req.GetAllocator()), req.GetAllocator());
    req.AddMember("remove_fields", remove, req.GetAllocator());
    call("preview", req, false, "INVALID_DOCUMENT");  // identity reference must be explicitly repaired first
    req.RemoveMember("base_dictionary_ref");
    req.RemoveMember("remove_fields");
    // Pending lookup must never force an unavailable loader or manufacture a dictionary.
    Json nullfields;
    source = Snapshot("dataframe", "pending-id", locator, nullfields, metadata, "1", "known", "pending");
    auto pending = call("preview", req);
    assert(pending["data"]["dictionary"].IsNull());
    assert(Get(call("check", req)["data"], "state") == "pending");
    offline = true;
    call("check", req, false, "SOURCE_UNAVAILABLE");
    offline = false;
    // A same-schema replacement does not inherit the old object's binding.
    source = Snapshot("dataframe", "replacement-id", locator, fields, metadata, "1");
    assert(Get(call("check", req)["data"], "state") == "unbound");
    // v1 remains readable, v2 survives restore and is not silently rewritten by checking.
    registry.Close();
    assert(registry.Open(file, &error) == 0);
    assert(registry.Resolve("binding.requests@2", &old, &error) == 0);
    Json restored;
    restored.Parse(old.document_json->c_str());
    assert(restored["schema_id"] == "flowsql.data-binding.v2");
    std::string response;
    assert(service.Control("describe", R"({"locator":{"kind":"dataframe","name":"x"},"timeout_ms":0})", response) != 0);
    source["schema_fingerprint"].SetString("forged", source.GetAllocator());
    call("describe", req, false, "SOURCE_UNAVAILABLE");
    assert(calls >= 20);
    registry.Close();
    unlink(file);
    std::cout << "PASS A1/A2: source facts, generation, no preview writes, manual preservation, versions, conflicts, "
                 "mapping, pending and identity boundaries\n";
}
