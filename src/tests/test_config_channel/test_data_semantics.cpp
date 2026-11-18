// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <channels/config/compiled_semantics_schema.h>
#include <channels/config/data_semantics_registry.h>
#include <sqlite3.h>
#include <unistd.h>
#include <atomic>
#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <thread>

using namespace flowsql;
using namespace flowsql::channels::config;
std::string ReadFile(const std::string& path) {
    std::ifstream stream(path);
    assert(stream.good());
    return {std::istreambuf_iterator<char>(stream), {}};
}
int main() {
    {
        rapidjson::Document document;
        DataSemanticsError error;
        assert(ParseSemanticsJson(R"([-9223372036854775808,18446744073709551615,1.25e3])", &document, &error) == 0);
        assert(document[0].GetInt64() == std::numeric_limits<int64_t>::min());
        assert(document[1].GetUint64() == std::numeric_limits<uint64_t>::max());
        assert(document[2].GetDouble() == 1250.0);
        assert(ParseSemanticsJson(R"({"a":[{"key":1},{"key":2}],"key":3})", &document, &error) == 0);
        for (const std::string invalid :
             {R"({"key":1,"key":2})", R"({"key":1,"\u006bey":2})", R"({"a":[{"key":1,"key":2}]})",
              "18446744073709551616", "-9223372036854775809", "[1,]", "{} trailing", "1e9999"}) {
            assert(ParseSemanticsJson(invalid, &document, &error) != 0);
            assert(error.code == "INVALID_DOCUMENT");
        }
        const auto invalid_utf8 = std::string("{\"key\":\"") + char(0xc3) + "(\"}";
        assert(ParseSemanticsJson(invalid_utf8, &document, &error) != 0 && error.code == "INVALID_DOCUMENT");
        assert(ParseSemanticsJson(std::string(64, '[') + "0" + std::string(64, ']'), &document, &error) == 0);
        assert(ParseSemanticsJson(std::string(65, '[') + "0" + std::string(65, ']'), &document, &error) != 0);
        assert(error.code == "INVALID_DOCUMENT");
    }
    const auto concurrent_standard = ReadFile(FLOWSQL_SOURCE_ROOT "/examples/semantics/standard.json");
    std::atomic<unsigned> ready{0};
    std::atomic<bool> start{false};
    std::vector<std::thread> validators;
    for (unsigned i = 0; i < 4; ++i) {
        validators.emplace_back([&, i] {
            ++ready;
            while (!start.load()) std::this_thread::yield();
            for (unsigned repeat = 0; repeat < 8; ++repeat) {
                rapidjson::Document document;
                std::vector<SemanticsReference> references;
                DataSemanticsError validation_error;
                assert(NormalizeSemantics("standard", concurrent_standard, &document, &references, &validation_error) ==
                       0);
                document["title"].SetInt(42);
                const auto invalid = SemanticsJson(document);
                assert(NormalizeSemantics("standard", invalid, &document, &references, &validation_error) != 0);
                assert(validation_error.code == "INVALID_DOCUMENT");
                const char* definition = i % 2 ? "saveRequest" : "resolveRequest";
                const std::string request =
                    i % 2 ? R"({"kind":"standard","name":"concurrent","expected_current_revision":0,"document":)" +
                                concurrent_standard + "}"
                          : R"({"exact_reference":"standard.concurrent@1"})";
                document.Parse(request.c_str());
                const auto matches = [&](const rapidjson::Value& value) {
                    rapidjson::SchemaValidator validator(CompiledSemanticsApiSchema(definition));
                    return value.Accept(validator);
                };
                assert(matches(document));
                document.RemoveMember(i % 2 ? "document" : "exact_reference");
                assert(!matches(document));
            }
        });
    }
    while (ready.load() != validators.size()) std::this_thread::yield();
    start = true;
    for (auto& validator : validators) validator.join();
    char file[] = "/tmp/flowsql_semantics_XXXXXX";
    int fd = mkstemp(file);
    assert(fd >= 0);
    close(fd);
    DataSemanticsRegistry registry;
    DataSemanticsError error;
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
    {
        rapidjson::Document repeated, preview;
        repeated.Parse(dictionary.document_json.c_str());
        auto& allocator = repeated.GetAllocator();
        for (unsigned index = 0; index < 32; ++index) {
            rapidjson::Value field(repeated["fields"][1], allocator);
            const auto name = "elapsed_" + std::to_string(index);
            field["field_id"].SetString(name.c_str(), allocator);
            field["name"].SetString(name.c_str(), allocator);
            repeated["fields"].PushBack(field, allocator);
        }
        std::string check;
        assert(registry.Preview("dictionary", SemanticsJson(repeated), &preview, &check, &error) == 0);
        repeated["fields"][33]["standard_refs"]["value"][0].SetString("standard.request.duration@99", allocator);
        assert(registry.Preview("dictionary", SemanticsJson(repeated), &preview, &check, &error) != 0);
        assert(error.code == "NOT_FOUND" && error.field_path.find("/fields/33/") == 0);
        repeated["fields"][33]["standard_refs"]["value"][0].SetString("standard.request.duration@1", allocator);
        sqlite3* storage = nullptr;
        assert(sqlite3_open(file, &storage) == SQLITE_OK);
        // A later invocation must re-read and reject corrupted content, not reuse an earlier snapshot.
        assert(sqlite3_exec(storage,
                            "BEGIN IMMEDIATE; UPDATE semantics_revision SET document='{}' "
                            "WHERE kind='standard' AND name='request.duration' AND revision=1; COMMIT;",
                            nullptr, nullptr, nullptr) == SQLITE_OK);
        assert(registry.Preview("dictionary", SemanticsJson(repeated), &preview, &check, &error) != 0);
        assert(error.code == "STORAGE_ERROR");
        sqlite3_stmt* restore = nullptr;
        assert(sqlite3_prepare_v2(storage,
                                  "UPDATE semantics_revision SET document=? WHERE kind='standard' "
                                  "AND name='request.duration' AND revision=1",
                                  -1, &restore, nullptr) == SQLITE_OK);
        assert(sqlite3_bind_text(restore, 1, held.document_json->c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK);
        assert(sqlite3_step(restore) == SQLITE_DONE);
        sqlite3_finalize(restore);
        sqlite3_close(storage);
        assert(registry.Preview("dictionary", SemanticsJson(repeated), &preview, &check, &error) == 0);
    }
    const std::string minimal = R"({"schema_id":"flowsql.data-standard","title":"minimal"})";
    for (const std::string bad :
         {R"({"schema_id":"flowsql.data-standard.v1","title":"old format"})",
          R"({"schema_id":"flowsql.data-standard"})", R"({"schema_id":"flowsql.data-standard","title":"x","extra":1})",
          R"({"schema_id":"flowsql.data-standard","title":"x","unit":{"state":"unknown","value":"ms"}})",
          R"({"schema_id":"flowsql.data-standard","title":"x","unit":{"state":"declared","value":{"code":"ms"}}})",
          R"({"schema_id":"flowsql.data-standard","title":"x","title":"y"})",
          R"({"schema_id":"flowsql.data-standard","title":"x","definition":{"state":"declared","value":""}})"}) {
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
    DataSemanticsSnapshot snapshot = held;
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
                               std::string("{\"schema_id\":\"flowsql.data-standard\",\"title\":\"") + title + "\"}"};
        r.expected_current_revision = 3;
        SemanticsSaveResult result;
        DataSemanticsError e;
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
    DataSemanticsSnapshot old_binding = out.snapshot;
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
