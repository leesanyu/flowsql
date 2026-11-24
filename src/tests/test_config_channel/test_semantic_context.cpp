// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <rapidjson/schema.h>
#include <cassert>
#include <common/semantic_context.hpp>
#include <cstdio>
#include <fstream>
#include <iterator>

using namespace flowsql;
using namespace flowsql::dataset;
using namespace flowsql::semantics;

int main() {
    Json source;
    source.Parse(R"({"subject":{"kind":"channel","locator":{"kind":"dataframe","name":"sample"}},
      "basis":{"object_key":null,"source_revision":"one","schema_fingerprint":"shape","binding_ref":null,
      "dictionary_ref":"dictionary:sample@1","component_revision":null,"contract_ref":null,"description_ref":null},
      "structure":{"fields":[{"name":"latency"},{"name":"unspecified"}]},
      "description":{"state":"present","origin":"manual","notes":null,"document":{
        "entity":{"limitations":{"state":"declared","value":["累计修订不可直接相加"]}},
        "fields":[{"field_id":"stable_latency","name":"latency","definition":{"state":"declared","value":"DNS latency"},
          "unit":{"state":"declared","value":{"code":"ms"}},"validity":{"state":"declared","value":"matched only"}},
          {"field_id":"stable_unknown","name":"unspecified","definition":{"state":"unknown","value":null}}]}},
      "context":{}})");
    assert(!source.HasParseError());
    Json subject;
    subject.Parse(R"({"kind":"query","execution_id":"actual-result"})");
    std::ifstream schema_file(FLOWSQL_SOURCE_ROOT "/config/semantics/api.schema.json");
    const std::string schema_text((std::istreambuf_iterator<char>(schema_file)), std::istreambuf_iterator<char>());
    Json schema;
    schema.Parse(schema_text.c_str());
    schema.AddMember("$ref", "#/$defs/semanticContext", schema.GetAllocator());
    rapidjson::SchemaDocument compiled(schema);
    auto result = [&](const std::string& sql, const std::vector<std::string>& names, bool changed = false,
                      const DataSemanticsError* error = nullptr) {
        SqlParser parser;
        const auto statement = parser.Parse(sql);
        if (!statement.error.empty()) std::fprintf(stderr, "%s: %s\n", sql.c_str(), statement.error.c_str());
        assert(statement.error.empty());
        auto context = ResultContext(subject, source, names, QueryProjections(statement), sql, changed, error);
        rapidjson::SchemaValidator validator(compiled);
        assert(context.Accept(validator));
        return context;
    };
    auto star = result("SELECT * FROM dataframe.sample", {"latency", "unspecified"});
    assert(Get(star, "state") == "partial" && star["fields"].Size() == 2);
    assert(Get(star["fields"][0], "source_field_id") == "stable_latency");
    assert(Get(star["fields"][1], "state") == "unknown" && star["fields"][1]["source_index"] == 0);
    assert(Get(star["fields"][0], "result_unit") == "ms");
    auto aliases =
        result("SELECT latency AS repeated, unspecified AS repeated FROM sqlite.db.sample", {"repeated", "repeated"});
    assert(aliases["fields"][0]["output_index"] == 0 && aliases["fields"][1]["output_index"] == 1);
    assert(Get(aliases["fields"][0], "source_field_id") == "stable_latency");
    assert(Get(aliases["fields"][1], "source_field_id") == "stable_unknown");
    assert(Get(aliases["fields"][0]["operation"], "kind") == "alias");
    auto counts = result("SELECT count(latency), count(*), sum(latency), avg(latency) FROM sqlite.db.sample",
                         {"n", "rows", "total", "mean"});
    assert(Get(counts["fields"][0], "result_unit") == "个" && Get(counts["fields"][1], "result_unit") == "个");
    assert(counts["fields"][1]["source_field_id"].IsNull());
    assert(counts["fields"][2]["result_unit"].IsNull() && counts["fields"][3]["result_unit"].IsNull());
    assert(Get(counts["fields"][3]["operation"], "function") == "avg");
    assert(counts["sources"][0]["description"] == source["description"]);
    for (const auto* sql :
         {"SELECT latency+1 AS latency FROM sqlite.db.sample", "SELECT latency FROM sqlite.db.sample JOIN other ON 1=1",
          "SELECT latency FROM dataframe.a, dataframe.b",
          "SELECT (SELECT latency FROM other) AS latency FROM sqlite.db.sample"}) {
        auto unknown = result(sql, {"latency"});
        assert(Get(unknown["fields"][0], "state") == "unknown");
        assert(unknown["fields"][0]["source_field_id"].IsNull());
    }
    // Zero rows do not erase the schema; mapping is independent of business rows.
    auto empty = result("SELECT latency FROM sqlite.db.sample WHERE 1=0", {"latency"});
    assert(Get(empty["fields"][0], "state") == "known");
    auto changed = result("SELECT * FROM dataframe.sample", {"other"}, true);
    assert(Get(changed["fields"][0], "state") == "conflict");
    assert(changed["fields"][0]["source_field_id"].IsNull() && changed["fields"][0]["result_unit"].IsNull());
    auto mismatch = result("SELECT * FROM dataframe.sample", {"latency"});
    assert(Get(mismatch["fields"][0], "state") == "conflict");
    DataSemanticsError error{"SOURCE_UNAVAILABLE", "read failed", ""};
    auto failed = result("SELECT latency FROM dataframe.sample", {"latency"}, false, &error);
    assert(Get(failed, "state") == "error" && Get(failed["error"], "code") == "SOURCE_UNAVAILABLE");
    assert(Get(failed["error"], "phase") == "description");
    assert(Get(empty["sources"][0]["basis"], "dictionary_ref") == "dictionary:sample@1");
    Set(source["basis"], "dictionary_ref", "dictionary:sample@2", source.GetAllocator());
    assert(Get(empty["sources"][0]["basis"], "dictionary_ref") == "dictionary:sample@1");
    Json binding;
    binding.Parse(R"({"field_map":{"stable_latency":"latency","stable_unknown":"unspecified"}})");
    source["context"].AddMember("binding", binding, source.GetAllocator());
    Set(source["description"]["document"]["fields"][0], "name", "dictionary_display_name", source.GetAllocator());
    auto mapped = result("SELECT latency AS shown FROM dataframe.sample", {"shown"});
    assert(Get(mapped["fields"][0], "source_field_id") == "stable_latency");
    return 0;
}
