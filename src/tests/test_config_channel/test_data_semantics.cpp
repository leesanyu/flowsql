// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <channels/config/data_semantics_registry.h>
#include <sqlite3.h>
#include <unistd.h>
#include <atomic>
#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>

using namespace flowsql;
using namespace flowsql::channels::config;
std::string ReadFile(const std::string& path) {
    std::ifstream stream(path);
    assert(stream.good());
    return {std::istreambuf_iterator<char>(stream), {}};
}
int main() {
    char file[] = "/tmp/flowsql_semantics_XXXXXX";
    int fd = mkstemp(file);
    assert(fd >= 0);
    close(fd);
    DataSemanticsRegistry registry;
    DataSemanticsErrorV1 error;
    assert(registry.Open(file, &error) == 0);
    SemanticsSaveResult out;
    SemanticsSaveRequest standard{"standard", "request.duration",
                                  ReadFile(FLOWSQL_SOURCE_ROOT "/examples/semantics/standard.json")};
    auto save = [&](const SemanticsSaveRequest& r) {
        int rc = registry.Save(r, &out, &error);
        if (rc) std::cerr << error.code << ": " << error.message << " at " << error.field_path << '\n';
        assert(rc == 0);
    };
    save(standard);
    assert(out.created_revision && out.snapshot.exact_reference == "standard.request.duration@1");
    const auto held = out.snapshot;
    rapidjson::Document normalized;
    normalized.Parse(held.document_json->c_str());
    assert(normalized["quantity"]["state"] == "unknown");
    assert(normalized["quantity"]["value"].IsNull());
    assert(SemanticsGaps(normalized).Size() == 3);
    standard.document_json = *held.document_json;
    save(standard);
    assert(!out.created_revision);
    SemanticsSaveRequest dictionary{"dictionary", "app.requests",
                                    ReadFile(FLOWSQL_SOURCE_ROOT "/examples/semantics/dictionary.json")};
    save(dictionary);
    assert(out.created_revision);
    const std::string minimal = R"({"schema_id":"flowsql.data-standard.v1","title":"minimal"})";
    for (const std::string bad :
         {R"({"schema_id":"flowsql.data-standard.v1"})",
          R"({"schema_id":"flowsql.data-standard.v1","title":"x","extra":1})",
          R"({"schema_id":"flowsql.data-standard.v1","title":"x","unit":{"state":"unknown","value":"ms"}})",
          R"({"schema_id":"flowsql.data-standard.v1","title":"x","unit":{"state":"declared","value":{"code":"ms"}}})",
          R"({"schema_id":"flowsql.data-standard.v1","title":"x","title":"y"})",
          R"({"schema_id":"flowsql.data-standard.v1","title":"x","definition":{"state":"declared","value":""}})"}) {
        auto r = standard;
        r.name = "negative";
        r.document_json = bad;
        assert(registry.Save(r, &out, &error) != 0 && error.code == "INVALID_DOCUMENT");
    }
    auto invalid_dict = dictionary;
    rapidjson::Document d;
    d.Parse(dictionary.document_json.c_str());
    d["fields"][1]["field_id"].SetString("request_id", d.GetAllocator());
    invalid_dict.document_json = SemanticsJson(d);
    assert(registry.Save(invalid_dict, &out, &error) != 0 && error.code == "INVALID_DOCUMENT");
    d.Parse(dictionary.document_json.c_str());
    d["entity"]["identity_fields"]["value"][0].SetString("absent", d.GetAllocator());
    invalid_dict.document_json = SemanticsJson(d);
    assert(registry.Save(invalid_dict, &out, &error) != 0 && error.code == "INVALID_DOCUMENT");
    d.Parse(dictionary.document_json.c_str());
    d["fields"][1]["standard_refs"]["value"][0].SetString("standard.request.duration@99", d.GetAllocator());
    invalid_dict.document_json = SemanticsJson(d);
    assert(registry.Save(invalid_dict, &out, &error) != 0 && error.code == "NOT_FOUND");
    d["fields"][1]["standard_refs"]["value"][0].SetString("dictionary.app.requests@1", d.GetAllocator());
    invalid_dict.document_json = SemanticsJson(d);
    assert(registry.Save(invalid_dict, &out, &error) != 0 && error.code == "INVALID_REFERENCE");
    DataSemanticsSnapshotV1 snapshot = held;
    assert(registry.Resolve("standard.request.duration@latest", &snapshot, &error) != 0 && !snapshot.document_json);
    assert(registry.Resolve("standard.request.duration@9007199254740992", &snapshot, &error) != 0);
    auto next = standard;
    next.expected_current_revision = 1;
    next.document_json = minimal;
    save(next);
    assert(out.snapshot.exact_reference == "standard.request.duration@2");
    standard.base_revision = 1;
    standard.expected_current_revision = 2;
    save(standard);
    assert(out.snapshot.exact_reference == "standard.request.duration@3");
    assert(*held.document_json == standard.document_json);
    next.base_revision = 99;
    next.expected_current_revision = 3;
    assert(registry.Save(next, &out, &error) != 0 && error.code == "NOT_FOUND");
    DataSemanticsRegistry second;
    assert(second.Open(file, &error) == 0);
    std::atomic<int> passed{0}, conflicts{0};
    auto write = [&](DataSemanticsRegistry& provider, const char* title) {
        SemanticsSaveRequest r{"standard", "request.duration",
                               std::string("{\"schema_id\":\"flowsql.data-standard.v1\",\"title\":\"") + title + "\"}"};
        r.expected_current_revision = 3;
        SemanticsSaveResult result;
        DataSemanticsErrorV1 e;
        if (provider.Save(r, &result, &e) == 0)
            ++passed;
        else {
            assert(e.code == "REVISION_CONFLICT");
            ++conflicts;
        }
    };
    std::thread a(write, std::ref(registry), "a"), b(write, std::ref(second), "b");
    a.join();
    b.join();
    assert(passed == 1 && conflicts == 1);
    sqlite3* db = nullptr;
    assert(sqlite3_open(file, &db) == SQLITE_OK);
    assert(sqlite3_exec(db,
                        "CREATE TRIGGER semantics_failure BEFORE INSERT ON semantics_revision BEGIN SELECT "
                        "RAISE(ABORT,'injected'); END;",
                        nullptr, nullptr, nullptr) == SQLITE_OK);
    next.expected_current_revision = 4;
    next.base_revision = 0;
    assert(registry.Save(next, &out, &error) != 0 && error.code == "STORAGE_ERROR");
    assert(registry.Resolve("standard.request.duration@5", &snapshot, &error) != 0 && error.code == "NOT_FOUND");
    assert(sqlite3_exec(db, "DROP TRIGGER semantics_failure", nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(db);
    second.Close();
    registry.Close();
    assert(*held.document_json == standard.document_json);
    assert(registry.Open(file, &error) == 0);
    assert(registry.Resolve("dictionary.app.requests@1", &snapshot, &error) == 0);
    assert(registry.Resolve("standard.request.duration@4", &snapshot, &error) == 0);
    SemanticsSaveRequest binding{"binding", "app.requests",
                                 ReadFile(FLOWSQL_SOURCE_ROOT "/examples/semantics/binding.json")};
    save(binding);
    assert(out.structure_check == "matched" && out.data_validation == "not_run");
    DataSemanticsSnapshotV1 old_binding = out.snapshot;
    assert(registry.BindingLookup("manual", "app.requests", &snapshot, &error) == 0 &&
           snapshot.exact_reference == "binding.app.requests@1");
    auto duplicate_binding = binding;
    duplicate_binding.name = "duplicate";
    assert(registry.Save(duplicate_binding, &out, &error) != 0 && error.code == "BINDING_MISMATCH");
    rapidjson::Document binding_doc;
    binding_doc.Parse(binding.document_json.c_str());
    binding_doc["object_key"]["object_id"].SetString("different", binding_doc.GetAllocator());
    auto invalid_binding = binding;
    invalid_binding.document_json = SemanticsJson(binding_doc);
    invalid_binding.expected_current_revision = 1;
    assert(registry.Save(invalid_binding, &out, &error) != 0 && error.code == "BINDING_MISMATCH");
    for (int mode = 0; mode < 6; ++mode) {
        binding_doc.Parse(binding.document_json.c_str());
        if (mode == 0) binding_doc["field_map"].RemoveMember("elapsed");
        if (mode == 1) binding_doc["field_map"]["elapsed"].SetString("id", binding_doc.GetAllocator());
        if (mode == 2) binding_doc["declared_schema"][1]["nullable"].SetBool(false);
        if (mode == 3) binding_doc["declared_schema"][1]["type"]["name"].SetString("int64", binding_doc.GetAllocator());
        if (mode == 4) binding_doc["declared_schema"].PopBack();
        if (mode == 5) binding_doc["declared_schema"][1]["name"].SetString("id", binding_doc.GetAllocator());
        invalid_binding.document_json = SemanticsJson(binding_doc);
        assert(registry.Save(invalid_binding, &out, &error) != 0 && error.code == "BINDING_MISMATCH");
    }
    binding_doc.Parse(binding.document_json.c_str());
    binding_doc["declared_schema"].SetNull();
    binding.document_json = SemanticsJson(binding_doc);
    binding.expected_current_revision = 1;
    save(binding);
    assert(out.structure_check == "unknown" && out.data_validation == "not_run");
    dictionary.expected_current_revision = 1;
    d.Parse(dictionary.document_json.c_str());
    d["title"].SetString("new dictionary semantics", d.GetAllocator());
    dictionary.document_json = SemanticsJson(d);
    save(dictionary);
    assert(out.snapshot.exact_reference == "dictionary.app.requests@2");
    assert(registry.BindingLookup("manual", "app.requests", &snapshot, &error) == 0);
    d.Parse(snapshot.document_json->c_str());
    assert(d["dictionary_ref"] == "dictionary.app.requests@1");
    std::vector<SemanticsReferenceItem> refs;
    assert(registry.References("dictionary.app.requests@1", false, &refs, &error) == 0 && refs.size() == 1 &&
           refs[0].is_current);
    binding_doc["dictionary_ref"].SetString("dictionary.app.requests@2", binding_doc.GetAllocator());
    binding.document_json = SemanticsJson(binding_doc);
    binding.expected_current_revision = 2;
    save(binding);
    assert(registry.References("dictionary.app.requests@1", false, &refs, &error) == 0 && refs.empty());
    assert(registry.References("dictionary.app.requests@1", true, &refs, &error) == 0 && refs.size() == 2);
    for (const auto& ref : refs) assert(!ref.is_current && ref.field_path == "/dictionary_ref");
    assert(registry.References("standard.request.duration@1", false, &refs, &error) == 0 && refs.size() == 1 &&
           refs[0].source_reference == "dictionary.app.requests@2");
    assert(registry.References("standard.request.duration@1", true, &refs, &error) == 0 && refs.size() == 2);
    assert(registry.Resolve(old_binding.exact_reference.c_str(), &snapshot, &error) == 0 &&
           *snapshot.document_json == *old_binding.document_json);
    registry.Close();
    assert(registry.Open(file, &error) == 0);
    assert(registry.BindingLookup("manual", "app.requests", &snapshot, &error) == 0 &&
           snapshot.exact_reference == "binding.app.requests@3");
    registry.Close();
    unlink(file);
    std::cout << "A1/A2/A3: definitions, versions, rollback, binding, references, recovery passed\n";
}
