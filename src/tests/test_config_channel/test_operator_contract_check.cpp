// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <channels/config/channel_dictionary.h>
#include <channels/config/operator_contract_service.h>
#include <unistd.h>
#include <cassert>
#include <common/channel_dataset.hpp>
#include <iostream>

using namespace flowsql;
using namespace flowsql::dataset;
using namespace flowsql::channels::config;
int main() {
    char path[] = "/tmp/flowsql_contract_check_XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    DataSemanticsRegistry registry;
    DataSemanticsErrorV1 error;
    assert(registry.Open(path, &error) == 0);
    OperatorContractService service(registry);
    Json contract;
    contract.Parse(R"({"schema_id":"flowsql.operator-data-contract.v1","title":"checked",
      "operator":{"category":"fixture","name":"checked","version":"1"},
      "configuration":{"invocation_schema":{"type":"object","properties":{"column":{"type":"string","default":"latency"}},"additionalProperties":true},"affects":{}},
      "inputs":[{"port_id":"input","min_count":1,"max_count":2,"shape":"dataframe","roles":[
        {"role_id":"value","required":true,"types":[{"system":"arrow","name":"double","parameters":{}}],"nullable":false,"semantic_requirements":{},"selector":{"kind":"parameter","key":"column"}}]}],
      "outputs":[{"port_id":"output","shape":"dataframe","mode":"unknown","rules":[]}],"limitations":[]})");
    SemanticsSaveResult saved;
    uint64_t revision = 0;
    auto save = [&] {
        SemanticsSaveRequest request{"operator_contract", "fixture.checked", Encode(contract)};
        request.expected_current_revision = revision;
        int rc = registry.Save(request, &saved, &error);
        if (rc) std::cerr << error.code << ": " << error.message << " " << error.field_path << '\n';
        assert(rc == 0);
        revision = std::stoull(saved.snapshot.exact_reference.substr(saved.snapshot.exact_reference.rfind('@') + 1));
    };
    save();
    Json locator, fields, metadata(rapidjson::kObjectType);
    locator.Parse(R"({"kind":"dataframe","name":"requests"})");
    fields.Parse(R"([{"name":"latency","type":{"system":"arrow","name":"double","parameters":{}},"nullable":false}])");
    auto source = Snapshot("dataframe", "requests-id", locator, fields, metadata, "1");
    Json invocation;
    invocation.Parse(
        R"({"contract_ref":"operator_contract.fixture.checked@1","component_revision":"actual-digest","inputs":[{"port_id":"input","input_index":0}],"effective_parameters":{},"config_refs":[],"execution_context":{"input_shapes":["dataframe","dataframe"]}})");
    auto& a = invocation.GetAllocator();
    invocation["inputs"][0].AddMember("snapshot", Copy(source, a), a);
    auto check = [&](const std::string& state) {
        Set(invocation, "contract_ref", saved.snapshot.exact_reference, a);
        std::string response;
        int rc = service.CheckAndDerive(Encode(invocation).c_str(), &response, &error);
        if (rc) std::cerr << error.code << " " << error.message << " " << error.field_path << '\n';
        assert(rc == 0);
        Json result;
        result.Parse(response.c_str());
        if (Get(result, "state") != state) std::cerr << response << '\n';
        assert(Get(result, "state") == state);
        assert(result["data_validation"] == "not_run");
        return result;
    };
    {
        invocation["config_refs"].PushBack("dictionary.wrong@1", a);
        std::string output;
        assert(service.CheckAndDerive(Encode(invocation).c_str(), &output, &error) != 0 &&
               error.code == "CONTRACT_INVALID");
        invocation["config_refs"][0].SetString("config.required@1", a);
        assert(service.CheckAndDerive(Encode(invocation).c_str(), &output, &error) != 0 && error.code == "UNAVAILABLE");
        invocation["config_refs"].Clear();
    }
    // Schema's default is an annotation. The production adapter must provide it.
    assert(check("incompatible")["reasons"][0]["code"] == "SELECTOR_MISSING");
    Set(invocation["effective_parameters"], "column", "latency", a);
    auto good = check("compatible");
    assert(good["role_map"]["input//0"][0]["column"] == "latency");
    Set(invocation["effective_parameters"], "column", "missing", a);
    check("incompatible");
    Set(invocation["effective_parameters"], "column", "latency", a);
    auto& actual = invocation["inputs"][0]["snapshot"];
    actual["fields"][0]["type"]["name"].SetString("float", a);
    check("incompatible");
    Json adaptations;
    adaptations.Parse(
        R"([{"column":"latency","source_type":{"system":"arrow","name":"float","parameters":{}},"target_type":{"system":"arrow","name":"double","parameters":{}},"basis":"production reader"}])");
    invocation["inputs"][0].AddMember("adaptations", Copy(adaptations, a), a);
    check("compatible");
    invocation["inputs"][0]["adaptations"][0]["source_type"]["name"].SetString("int64", a);
    check("incompatible");
    invocation["inputs"][0].RemoveMember("adaptations");
    actual["fields"][0]["type"]["name"].SetString("double", a);
    actual["fields"][0]["nullable"].SetBool(true);
    check("incompatible");
    actual["fields"][0]["nullable"].SetBool(false);
    actual["fields"][0]["nullable"].SetNull();
    check("undetermined");
    actual["fields"][0]["nullable"].SetBool(false);
    Set(actual, "schema_state", "pending", a);
    check("undetermined");
    Set(actual, "schema_state", "known", a);
    invocation["inputs"].PushBack(Copy(invocation["inputs"][0], a), a);
    invocation["inputs"][1]["input_index"].SetUint(1);
    auto paired = check("compatible");
    assert(paired["role_map"].MemberCount() == 2);
    invocation["inputs"].PopBack();

    // Only trusted F2 bindings for the same observed source may prove semantics.
    ChannelDictionaryService dictionaries(registry, [&](const auto&, auto* out, auto*) {
        out->document_json = std::make_shared<const std::string>(Encode(source));
        return 0;
    });
    std::string response;
    assert(dictionaries.Control("preview", R"({"locator":{"kind":"dataframe","name":"requests"}})", response) == 0);
    Json preview;
    preview.Parse(response.c_str());
    Json unit;
    unit.Parse(R"({"state":"declared","origin":"manual","value":{"code":"ms","label":"milliseconds"}})");
    Set(preview["data"]["dictionary"]["fields"][0], "unit", Copy(unit, preview.GetAllocator()), preview.GetAllocator());
    SemanticsSaveRequest dictionary_request{"dictionary", "requests", Encode(preview["data"]["dictionary"])};
    SemanticsSaveResult dictionary;
    assert(registry.Save(dictionary_request, &dictionary, &error) == 0);
    Json binding;
    binding.Parse(R"({"schema_id":"flowsql.data-binding.v2","object_label":"requests","declared_schema":null})");
    auto& ba = binding.GetAllocator();
    Set(binding, "dictionary_ref", dictionary.snapshot.exact_reference, ba);
    binding.AddMember("object_key", Copy(source["object_key"], ba), ba)
        .AddMember("source_snapshot", Copy(source, ba), ba)
        .AddMember("field_map", Copy(preview["data"]["field_map"], ba), ba);
    Value confirmation(rapidjson::kObjectType);
    confirmation.AddMember("source_revision", Copy(source["source_revision"], ba), ba)
        .AddMember("items", Value(rapidjson::kArrayType), ba);
    binding.AddMember("confirmation", confirmation, ba);
    SemanticsSaveRequest binding_request{"binding", "requests", Encode(binding)};
    binding_request.source_observed = true;
    SemanticsSaveResult bound;
    int binding_rc = registry.Save(binding_request, &bound, &error);
    if (binding_rc) std::cerr << error.code << " " << error.message << " " << error.field_path << '\n';
    assert(binding_rc == 0);
    Set(invocation["inputs"][0], "binding_ref", bound.snapshot.exact_reference, a);
    auto& ca = contract.GetAllocator();
    contract["inputs"][0]["roles"][0]["semantic_requirements"].AddMember("unit", Copy(unit["value"], ca), ca);
    save();
    check("compatible");
    Set(contract["inputs"][0]["roles"][0]["semantic_requirements"]["unit"], "code", "s", ca);
    save();
    check("incompatible");
    Set(invocation["inputs"][0]["snapshot"], "source_revision", "changed", a);
    assert(service.CheckAndDerive(Encode(invocation).c_str(), &response, &error) != 0 &&
           error.code == "SOURCE_CHANGED");
    assert(response.empty());
    invocation["inputs"][0].RemoveMember("binding_ref");
    check("undetermined");

    // Nested items have independent role scope, even when identities/column names repeat.
    contract["inputs"][0]["roles"].Clear();
    Set(contract["inputs"][0], "repeat_from_config_pointer", "/datasets", ca);
    Set(contract["inputs"][0], "item_key_pointer", "/id", ca);
    Json templates;
    templates.Parse(
        R"([{"repeat_from_pointer":"/metrics","item_key_pointer":"/id","discriminator_pointer":"/kind","cases":{"relation":{"roles":[],"role_templates":[{"repeat_from_pointer":"/metrics","item_key_pointer":"/id","cases":{"default":{"roles":[{"role_id":"value","required":true,"types":[],"nullable":null,"semantic_requirements":{},"selector":{"kind":"config","scope":"role_item","pointer":"/column","cardinality":"one"}}]}}}]},"value":{"roles":[{"role_id":"value","required":true,"types":[],"nullable":null,"semantic_requirements":{},"selector":{"kind":"config","scope":"role_item","pointer":"/column","cardinality":"one"}}]}}}])");
    contract["inputs"][0].AddMember("role_templates", Copy(templates, ca), ca);
    save();
    Json config;
    config.Parse(
        R"({"datasets":[{"id":"a","metrics":[{"id":"v","kind":"value","column":"latency"}]},{"id":"b","metrics":[{"id":"r","kind":"relation","metrics":[{"id":"v","column":"latency"}]}]}]})");
    invocation.AddMember("normalized_config", Copy(config, a), a);
    Set(invocation["inputs"][0], "item_key", "a", a);
    invocation["inputs"].PushBack(Copy(invocation["inputs"][0], a), a);
    Set(invocation["inputs"][1], "item_key", "b", a);
    invocation["inputs"][1]["input_index"].SetUint(1);
    auto nested = check("compatible");
    assert(nested["role_map"]["input/a/0"][0]["role_path"] == "input/a/0/v/value/0");
    assert(nested["role_map"]["input/b/1"][0]["role_path"] == "input/b/1/r/v/value/0");
    Set(invocation, "contract_ref", "operator_contract.fixture.checked@9999", a);
    assert(service.CheckAndDerive(Encode(invocation).c_str(), &response, &error) != 0 && error.code == "NOT_FOUND");
    assert(response.empty());
    registry.Close();
    unlink(path);
    std::cout << "operator input checks passed\n";
}
