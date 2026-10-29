// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <channels/config/data_semantics_registry.h>
#include <channels/config/operator_data_contract_model.h>
#include <framework/interfaces/ioperator_data_contract.h>
#include <unistd.h>
#include <cassert>
#include <fstream>
#include <iostream>
using namespace flowsql;
using namespace flowsql::channels::config;
std::string Fixture(const char* name) {
    std::ifstream in(std::string(FLOWSQL_SOURCE_ROOT) + "/config/semantics/examples/" + name);
    assert(in.good());
    return {std::istreambuf_iterator<char>(in), {}};
}
int main() {
    char path[] = "/tmp/flowsql_contract_XXXXXX";
    const int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    DataSemanticsRegistry store;
    DataSemanticsErrorV1 e;
    assert(store.Open(path, &e) == 0);
    SemanticsSaveResult out;
    SemanticsSaveRequest r{"operator_contract", "builtin.passthrough", Fixture("operator-contract-passthrough.json")};
    auto save = [&] {
        const int rc = store.Save(r, &out, &e);
        if (rc) std::cerr << e.code << " " << e.message << " " << e.field_path << '\n';
        assert(rc == 0);
    };
    save();
    assert(out.created_revision);
    const auto held = out.snapshot;
    assert(held.exact_reference == "operator_contract.builtin.passthrough@1");
    save();
    assert(!out.created_revision);
    rapidjson::Document d;
    d.Parse(r.document_json.c_str());
    d["title"].SetString("Changed", d.GetAllocator());
    r.document_json = SemanticsJson(d);
    assert(store.Save(r, &out, &e) != 0 && e.code == "REVISION_CONFLICT");
    r.expected_current_revision = 1;
    save();
    assert(out.snapshot.exact_reference == "operator_contract.builtin.passthrough@2");
    assert(*held.document_json != *out.snapshot.document_json);
    r = {"operator_contract", "explore.baseliner", Fixture("operator-contract-baseliner.json")};
    save();
    d.Parse(r.document_json.c_str());
    d["resources"]["expression"].AddMember("unsupportedKeyword", true, d.GetAllocator());
    auto invalid_resource = r;
    invalid_resource.document_json = SemanticsJson(d);
    assert(store.Save(invalid_resource, &out, &e) != 0 && e.code == "CONTRACT_INVALID");
    for (const char* bad : {"#/resources/absent", "https://example.com/schema"}) {
        d.Parse(r.document_json.c_str());
        d["configuration"]["normalized_config_schema"]["$ref"].SetString(bad, d.GetAllocator());
        auto invalid = r;
        invalid.document_json = SemanticsJson(d);
        assert(store.Save(invalid, &out, &e) != 0 && e.code == "CONTRACT_INVALID");
    }
    d.Parse(r.document_json.c_str());
    d["inputs"].PushBack(rapidjson::Value(d["inputs"][0], d.GetAllocator()), d.GetAllocator());
    auto invalid = r;
    invalid.document_json = SemanticsJson(d);
    assert(store.Save(invalid, &out, &e) != 0 && e.code == "CONTRACT_INVALID");
    d.Parse(r.document_json.c_str());
    d["configuration"]["normalized_config_schema"]["$ref"].SetString("#/resources/task_config~2", d.GetAllocator());
    invalid.document_json = SemanticsJson(d);
    assert(store.Save(invalid, &out, &e) != 0);
    d.Parse(r.document_json.c_str());
    d["outputs"][0]["context_semantics"]["metric_pointer"].SetString("/bad~2", d.GetAllocator());
    invalid.document_json = SemanticsJson(d);
    assert(store.Save(invalid, &out, &e) != 0 && e.code == "CONTRACT_INVALID");
    invalid = {"operator_use", "fake", R"({"schema_id":"flowsql.operator-data-use.v1"})"};
    assert(store.Save(invalid, &out, &e) != 0);
    std::string response;
    assert(
        store.Control(
            "resolve",
            R"({"exact_reference":"operator_contract.explore.baseliner@1","view":"resource","resource_pointer":"/resources/task_config"})",
            response) == 0);
    d.Parse(response.c_str());
    assert(d["data"]["resource"]["type"] == "object");
    assert(store.Control("resolve", R"({"exact_reference":"operator_contract.explore.baseliner@1","view":"summary"})",
                         response) == 0);
    assert(
        store.Control(
            "resolve",
            R"({"exact_reference":"operator_contract.explore.baseliner@1","view":"resource","resource_pointer":"/absent"})",
            response) != 0);
    for (const char* schema_text :
         {R"({"type":"integer","default":"invalid"})", R"({"type":"object","required":"wrong"})",
          R"({"type":"object","unsupportedKeyword":true})"}) {
        d.Parse(r.document_json.c_str());
        rapidjson::Document bad_schema;
        bad_schema.Parse(schema_text);
        d["configuration"]["invocation_schema"].CopyFrom(bad_schema, d.GetAllocator());
        invalid = r;
        invalid.document_json = SemanticsJson(d);
        assert(store.Save(invalid, &out, &e) != 0 && e.code == "CONTRACT_INVALID");
    }
    d.Parse(r.document_json.c_str());
    const auto& invocation = d["configuration"]["invocation_schema"];
    rapidjson::Document params;
    params.Parse(R"({"config":"config.example@1"})");
    assert(ValidateOperatorConfiguration(invocation, params, d, &e) == 0);
    params.Parse(R"({"config":"config.example@1","parameters":"{}"})");
    assert(ValidateOperatorConfiguration(invocation, params, d, &e) != 0);
    params.Parse(R"({"parameters":"{\"dataset\":{}}"})");
    assert(ValidateOperatorParameters(d, params, &e) != 0);
    params.Parse(R"({"parameters":"{invalid"})");
    assert(ValidateOperatorParameters(d, params, &e) != 0 && e.code == "CONTRACT_INCOMPATIBLE");
    params.Parse(R"({"parameters":"{}"})");
    assert(ValidateOperatorParameters(d, params, &e) != 0);
    // A recursive negative branch must never turn exhausted validation into success.
    rapidjson::Document bounded;
    bounded.Parse(R"({"not":{"$ref":"#"}})");
    assert(ValidateOperatorConfiguration(bounded, params, bounded, &e) != 0);
    bounded.Parse(R"({"type":"array","uniqueItems":true})");
    rapidjson::Document large;
    large.SetArray();
    for (int i = 0; i < 700; ++i) large.PushBack(i, large.GetAllocator());
    assert(ValidateOperatorConfiguration(bounded, large, bounded, &e) != 0 && e.code == "LIMIT_EXCEEDED");
    rapidjson::Document conditions;
    conditions.Parse(
        R"({"type":"object","properties":{"mode":{"enum":["sampled","sum"]},"count":{"type":"integer","minimum":1}},"required":["mode"],"if":{"properties":{"mode":{"const":"sampled"}}},"then":{"required":["count"]},"additionalProperties":false})");
    params.Parse(R"({"mode":"sampled"})");
    assert(ValidateOperatorConfiguration(conditions, params, conditions, &e) != 0);
    params.Parse(R"({"mode":"sampled","count":1})");
    assert(ValidateOperatorConfiguration(conditions, params, conditions, &e) == 0);
    params.Parse(R"({"mode":"sampled","count":0})");
    assert(ValidateOperatorConfiguration(conditions, params, conditions, &e) != 0);
    params.Parse(R"({"mode":"sum"})");
    assert(ValidateOperatorConfiguration(conditions, params, conditions, &e) == 0);
    std::string yaml_response;
    const std::string yaml_request =
        R"({"kind":"operator_contract","name":"builtin.passthrough","format":"yaml","content":)" +
        SemanticsJson(rapidjson::Value(
            "schema_id: flowsql.operator-data-contract.v1\ntitle: imported\noperator: {category: builtin, name: "
            "passthrough, version: '1'}\nconfiguration: {invocation_schema: true, affects: {}}\ninputs: [{port_id: "
            "input, min_count: 1, max_count: 1, shape: dataframe, roles: []}]\noutputs: [{port_id: output, shape: "
            "dataframe, mode: unknown, rules: []}]\nlimitations: []\n")) +
        "}";
    assert(store.Control("import", yaml_request, yaml_response) == 0);
    d.Parse(yaml_response.c_str());
    assert(d["data"]["document"]["configuration"]["invocation_schema"].IsTrue());
    assert(store.Control("history", R"({"kind":"operator_contract","name":"builtin.passthrough"})", response) == 0);
    d.Parse(response.c_str());
    assert(d["data"]["items"].Size() == 2);
    invalid = r;
    invalid.name = "mismatch.name";
    assert(store.Save(invalid, &out, &e) != 0 && e.code == "CONTRACT_INVALID");
    store.Close();
    assert(store.Open(path, &e) == 0);
    DataSemanticsSnapshotV1 snapshot;
    assert(store.Resolve("operator_contract.explore.baseliner@1", &snapshot, &e) == 0);
    assert(store.Resolve("operator_contract.explore.baseliner@latest", &snapshot, &e) != 0 && !snapshot.document_json);
    assert(store.Resolve("operator_contract.explore.baseliner@999", &snapshot, &e) != 0);
    assert(held.document_json && held.exact_reference == "operator_contract.builtin.passthrough@1");
    store.Close();
    unlink(path);
    std::cout << "PASS: contract normalization, closed resources, immutable revisions, conflict, partial reads, "
                 "restart, no fake observations\n";
}
