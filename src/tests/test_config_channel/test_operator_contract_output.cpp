// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <channels/config/operator_contract_service.h>
#include <sqlite3.h>
#include <unistd.h>
#include <cassert>
#include <common/channel_dataset.hpp>
#include <iostream>
using namespace flowsql;
using namespace flowsql::dataset;
using namespace flowsql::channels::config;
Json Parse(const char* text) {
    Json d;
    d.Parse(text);
    assert(!d.HasParseError());
    return d;
}
int main() {
    char path[] = "/tmp/flowsql_output_contract_XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    DataSemanticsRegistry registry;
    DataSemanticsErrorV1 error;
    assert(registry.Open(path, &error) == 0);
    OperatorContractService service(registry);
    auto fields = Parse(
        R"([{"name":"latency","type":{"system":"arrow","name":"double","parameters":{}},"nullable":false},{"name":"id","type":{"system":"arrow","name":"int64","parameters":{}},"nullable":false}])");
    auto locator = Parse(R"({"kind":"dataframe","name":"source"})");
    auto metadata = Parse("{}");
    auto source = Snapshot("fixture", "input", locator, fields, metadata, "1");
    auto dictionary = Parse(
        R"({"schema_id":"flowsql.data-dictionary.v1","title":"requests","entity":{"identity_fields":{"state":"declared","value":["identity"]},"row_semantics":{"state":"declared","value":"event"}},"fields":[{"field_id":"latency","name":"latency","title":"latency","type":{"system":"arrow","name":"double","parameters":{}},"nullable":false,"unit":{"state":"declared","value":{"code":"ms","label":"milliseconds"}}},{"field_id":"identity","name":"id","title":"id","type":{"system":"arrow","name":"int64","parameters":{}},"nullable":false}]})");
    auto contextual_validity = Parse(
        R"({"state":"declared","value":{"mode":"descriptive","description":"requires event identity","field_ids":["identity"]}})");
    Set(dictionary["fields"][0], "validity", Copy(contextual_validity, dictionary.GetAllocator()),
        dictionary.GetAllocator());
    SemanticsSaveResult saved;
    assert(registry.Save({"dictionary", "input", Encode(dictionary)}, &saved, &error) == 0);
    auto binding = Parse(
        R"({"schema_id":"flowsql.data-binding.v2","object_label":"source","dictionary_ref":"dictionary.input@1","field_map":{"latency":"latency","identity":"id"},"declared_schema":null,"confirmation":{"source_revision":"","items":[]}})");
    auto& ba = binding.GetAllocator();
    binding.AddMember("object_key", Copy(source["object_key"], ba), ba)
        .AddMember("source_snapshot", Copy(source, ba), ba);
    Set(binding["confirmation"], "source_revision", Get(source, "source_revision"), ba);
    SemanticsSaveRequest bound{"binding", "input", Encode(binding)};
    bound.source_observed = true;
    assert(registry.Save(bound, &saved, &error) == 0);
    auto contract = Parse(
        R"({"schema_id":"flowsql.operator-data-contract.v1","title":"transform","operator":{"category":"fixture","name":"transform","version":"1"},"configuration":{"invocation_schema":{"type":"object","additionalProperties":true},"affects":{}},"inputs":[{"port_id":"input","min_count":1,"max_count":2,"shape":"dataframe","roles":[{"role_id":"measure","required":true,"types":[],"nullable":null,"semantic_requirements":{},"selector":{"kind":"column","column":"latency"}}]}],"outputs":[{"port_id":"main","shape":"dataframe","mode":"derived","rules":[{"kind":"copy","source_port":"input","all_fields":true}]}],"limitations":[]})");
    auto invocation = Parse(
        R"({"contract_ref":"","component_revision":"actual-digest","effective_parameters":{},"config_refs":[],"execution_context":{"input_shapes":["dataframe","dataframe"]},"inputs":[{"port_id":"input","input_index":0,"binding_ref":"binding.input@1"}]})");
    auto& ia = invocation.GetAllocator();
    invocation["inputs"][0].AddMember("snapshot", Copy(source, ia), ia);
    uint64_t revision = 0;
    auto check = [&]() {
        SemanticsSaveRequest request{"operator_contract", "fixture.transform", Encode(contract)};
        request.expected_current_revision = revision;
        int rc = registry.Save(request, &saved, &error);
        if (rc) std::cerr << error.code << " " << error.message << '\n';
        assert(rc == 0);
        SemanticsIdentity id;
        assert(ParseSemanticsReference(saved.snapshot.exact_reference, &id));
        revision = id.revision;
        Set(invocation, "contract_ref", saved.snapshot.exact_reference, ia);
        std::string text;
        assert(service.CheckAndDerive(Encode(invocation).c_str(), &text, &error) == 0);
        return Parse(text.c_str());
    };
    auto result = check();
    auto again = check();
    assert(result == again);
    assert(result["output_previews"][0]["dictionary"]["fields"][0]["field_id"] == "latency");
    assert(result["output_previews"][0]["dictionary"]["fields"][0]["unit"]["value"]["code"] == "ms");
    auto& ca = contract.GetAllocator();
    auto& port = contract["outputs"][0];
    auto rename =
        Parse(R"({"kind":"rename","source_port":"input","source_roles":["measure"],"output_field":"duration"})");
    port["rules"].Clear();
    port["rules"].PushBack(Copy(rename, ca), ca);
    result = check();
    const auto& projected = result["output_previews"][0];
    assert(projected["dictionary"]["fields"].Size() == 1 &&
           projected["dictionary"]["fields"][0]["field_id"] == "latency");
    assert(projected["dictionary"]["fields"][0]["name"] == "duration");
    assert(projected["dictionary"]["entity"]["identity_fields"]["state"] == "unknown");
    assert(projected["dictionary"]["fields"][0]["validity"]["state"] == "unknown");
    assert(!projected["semantic_gaps"].Empty());
    // The same source field identity used in distinct inputs needs distinct output identities.
    auto other_port = Parse(Encode(contract["inputs"][0]).c_str());
    Set(other_port, "port_id", "other", other_port.GetAllocator());
    contract["inputs"].PushBack(Copy(other_port, ca), ca);
    invocation["inputs"].PushBack(Copy(invocation["inputs"][0], ia), ia);
    Set(invocation["inputs"][1], "port_id", "other", ia);
    invocation["inputs"][1]["input_index"].SetUint(1);
    Set(port["rules"][0], "output_field", "left_value", ca);
    auto right_rule = Parse(Encode(port["rules"][0]).c_str());
    Set(right_rule, "source_port", "other", right_rule.GetAllocator());
    Set(right_rule, "output_field", "right_value", right_rule.GetAllocator());
    port["rules"].PushBack(Copy(right_rule, ca), ca);
    result = check();
    const auto& pair = result["output_previews"][0];
    assert(pair["dictionary"]["fields"].Size() == 2);
    assert(pair["dictionary"]["fields"][0]["field_id"] != pair["dictionary"]["fields"][1]["field_id"]);
    assert(pair["lineage"][0]["input_index"].GetUint() == 0 && pair["lineage"][1]["input_index"].GetUint() == 1);
    assert(pair["lineage"][0]["output_field_id"] == pair["dictionary"]["fields"][0]["field_id"]);
    invocation["inputs"].PopBack();
    contract["inputs"].PopBack();
    port["rules"].PopBack();
    auto conversion = Parse(
        R"({"kind":"unit_convert","source_port":"input","source_roles":["measure"],"output_field":"seconds","unit_rule":{"source_unit":{"code":"ms","label":"milliseconds"},"target_unit":{"code":"s","label":"seconds"},"scale":0.001,"offset":0,"type":{"system":"arrow","name":"double","parameters":{}},"rounding":"none","null_policy":"propagate","overflow_policy":"error"}})");
    port["rules"][0].CopyFrom(conversion, ca);
    result = check();
    assert(result["output_previews"][0]["dictionary"]["fields"][0]["unit"]["value"]["code"] == "s");
    assert(result["output_previews"][0]["dictionary"]["fields"][0]["field_id"] != "latency");
    auto aggregate = Parse(
        R"({"kind":"aggregate","source_port":"input","source_roles":["measure"],"output_field":"value","aggregation_rule":{"operation":"sum","group_by":[],"null_policy":"ignore","type":{"system":"arrow","name":"double","parameters":{}},"prerequisites_confirmed":true}})");
    for (const char* op : {"sum", "count", "mean", "min", "max"}) {
        Set(aggregate["aggregation_rule"], "operation", op, aggregate.GetAllocator());
        port["rules"][0].CopyFrom(aggregate, ca);
        result = check();
        const auto& f = result["output_previews"][0]["dictionary"]["fields"][0];
        assert(f["measure_kind"]["value"] == "statistic");
        assert(f["unit"]["value"]["code"] == (std::string(op) == "count" ? "1" : "ms"));
    }
    port["rules"][0]["aggregation_rule"]["prerequisites_confirmed"].SetBool(false);
    result = check();
    assert(!result["output_previews"][0]["dictionary"]["fields"][0].HasMember("unit"));
    // Unknown same-named fields must never borrow the input's milliseconds.
    Set(port, "mode", "unknown", ca);
    // Even a stale copy rule cannot make mode=unknown inherit semantics.
    result = check();
    assert(result["output_previews"][0]["dictionary"]["fields"].Empty());
    port["rules"].Clear();
    result = check();
    auto observation = [&](const std::string& name, const Value& actual_fields) {
        auto request = Parse(
            R"({"name":"","expected_current_revision":0,"document":{"schema_id":"flowsql.operator-data-use.v1","title":"execution","run_id":"run","stage_index":0,"operator_ref":{"category":"fixture","name":"transform","version":"1"},"component_revision":"actual-digest","contract_ref":"","input_snapshots":[],"role_map":{},"effective_parameters":{},"config_refs":[],"execution_context":{"input_shapes":["dataframe"]},"checks":{},"outputs":[],"outcome":"complete"}})");
        auto& a = request.GetAllocator();
        Set(request, "name", name, a);
        auto& d = request["document"];
        Set(d, "contract_ref", Get(invocation, "contract_ref"), a);
        Set(d, "input_snapshots", Copy(invocation["inputs"], a), a);
        auto snapshot = Snapshot("operator_output", name, locator, actual_fields, metadata, "1");
        Value output(rapidjson::kObjectType);
        Set(output, "port_id", "main", a);
        Set(output, "state", "observed", a);
        output.AddMember("snapshot", Copy(snapshot, a), a).AddMember("row_count", 0, a);
        d["outputs"].PushBack(output, a);
        return request;
    };
    auto request = observation("unknown", fields);
    OperatorContractSnapshotV1 use;
    int rc = service.RecordUse(Encode(request).c_str(), &use, &error);
    if (rc) std::cerr << error.code << " " << error.message << " " << error.field_path << '\n';
    assert(rc == 0);
    auto used = Parse(use.document_json->c_str());
    assert(!used["role_map"].ObjectEmpty());
    assert(used["outputs"][0]["output_check"] == "matched");
    DataSemanticsSnapshotV1 output_dictionary;
    assert(registry.Resolve(Get(used["outputs"][0], "dictionary_ref").c_str(), &output_dictionary, &error) == 0);
    auto unknown = Parse(output_dictionary.document_json->c_str());
    assert(unknown["fields"][0]["unit"]["state"] == "unknown");
    const auto exact = use.exact_reference;
    assert(service.RecordUse(Encode(request).c_str(), &use, &error) == 0 && use.exact_reference == exact);
    assert(registry.Save({"operator_use", "forged", Encode(used)}, &saved, &error) != 0 &&
           error.code == "CONTRACT_INVALID");
    // Declared structure is checked even for zero rows. Drift writes no use/binding.
    Set(port, "mode", "derived", ca);
    auto copy = Parse(R"({"kind":"copy","source_port":"input","all_fields":true})");
    port["rules"].PushBack(Copy(copy, ca), ca);
    result = check();
    request = observation("zero", fields);
    assert(service.RecordUse(Encode(request).c_str(), &use, &error) == 0);
    auto drift = Parse(Encode(fields).c_str());
    drift[0]["type"]["name"].SetString("int64", drift.GetAllocator());
    request = observation("drift", drift);
    assert(service.RecordUse(Encode(request).c_str(), &use, &error) != 0 && error.code == "OUTPUT_SCHEMA_MISMATCH" &&
           !use.document_json);
    DataSemanticsSnapshotV1 missing;
    assert(registry.Resolve("operator_use.drift@1", &missing, &error) != 0);
    // Conditions, two entities, and row declarations survive without being row validation.
    auto secondary = Parse(
        R"({"port_id":"secondary","shape":"dataframe","mode":"unknown","rules":[],"availability":{"scope":"execution_context","pointer":"/secondary","equals":true},"row_cases":[{"case_id":"forecast","when":{"scope":"row","pointer":"/kind","equals":"forecast"},"field_rules":{"observed":{"null_meaning":"no observation"}}}]})");
    contract["outputs"].PushBack(Copy(secondary, ca), ca);
    result = check();
    assert(result["output_previews"][1]["availability"] == "undetermined");
    invocation["execution_context"].AddMember("secondary", false, ia);
    result = check();
    assert(result["output_previews"][1]["availability"] == "absent");
    request = observation("conditional", fields);
    auto& a = request.GetAllocator();
    request["document"]["execution_context"].AddMember("secondary", false, a);
    auto absent = Parse(R"({"port_id":"secondary","state":"absent"})");
    request["document"]["outputs"].PushBack(Copy(absent, a), a);
    assert(service.RecordUse(Encode(request).c_str(), &use, &error) == 0);
    used = Parse(use.document_json->c_str());
    assert(used["outputs"][1]["state"] == "absent" && !used["outputs"][1].HasMember("binding_ref") &&
           used["outputs"][1].HasMember("row_cases"));
    assert(used["checks"]["data_validation"] == "not_run");
    request = observation("two_outputs", fields);
    Set(request["document"]["execution_context"], "secondary", Value(true), request.GetAllocator());
    auto second_snapshot = Snapshot("operator_output", "secondary_object", locator, fields, metadata, "1");
    Value observed(rapidjson::kObjectType);
    Set(observed, "port_id", "secondary", request.GetAllocator());
    Set(observed, "state", "observed", request.GetAllocator());
    observed.AddMember("snapshot", Copy(second_snapshot, request.GetAllocator()), request.GetAllocator());
    request["document"]["outputs"].PushBack(observed, request.GetAllocator());
    assert(service.RecordUse(Encode(request).c_str(), &use, &error) == 0);
    used = Parse(use.document_json->c_str());
    assert(used["outputs"][0]["binding_ref"] != used["outputs"][1]["binding_ref"]);
    assert(used["outputs"][0]["dictionary_ref"] != used["outputs"][1]["dictionary_ref"]);
    Set(contract, "title", "rollback-version", ca);
    result = check();
    // Fail at the last write, after fresh dictionaries/bindings have been staged.
    sqlite3* database = nullptr;
    assert(sqlite3_open(path, &database) == SQLITE_OK);
    auto count = [&]() {
        sqlite3_stmt* stmt = nullptr;
        assert(sqlite3_prepare_v2(database, "SELECT count(*) FROM semantics_revision", -1, &stmt, nullptr) ==
               SQLITE_OK);
        assert(sqlite3_step(stmt) == SQLITE_ROW);
        int n = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        return n;
    };
    int before = count();
    assert(sqlite3_exec(database,
                        "CREATE TRIGGER fail_use BEFORE INSERT ON semantics_revision WHEN NEW.kind='operator_use' "
                        "BEGIN SELECT RAISE(ABORT,'injected'); END",
                        nullptr, nullptr, nullptr) == SQLITE_OK);
    request = observation("rollback", fields);
    assert(service.RecordUse(Encode(request).c_str(), &use, &error) != 0 && error.code == "STORAGE_ERROR" &&
           !use.document_json);
    assert(count() == before);
    assert(sqlite3_exec(database, "DROP TRIGGER fail_use", nullptr, nullptr, nullptr) == SQLITE_OK);
    request = observation("conflict", fields);
    request["expected_current_revision"].SetUint(99);
    before = count();
    assert(service.RecordUse(Encode(request).c_str(), &use, &error) != 0 && error.code == "REVISION_CONFLICT");
    assert(count() == before);
    sqlite3_close(database);
    // Independently expected source units remain isolated by dataset and metric.
    auto other_dictionary = Parse(Encode(dictionary).c_str());
    Set(other_dictionary["fields"][0]["unit"]["value"], "code", "B", other_dictionary.GetAllocator());
    assert(registry.Save({"dictionary", "bytes", Encode(other_dictionary)}, &saved, &error) == 0);
    auto other_source = Snapshot("fixture", "bytes", locator, fields, metadata, "1");
    auto other_binding = Parse(Encode(binding).c_str());
    Set(other_binding, "dictionary_ref", "dictionary.bytes@1", other_binding.GetAllocator());
    Set(other_binding, "object_key", Copy(other_source["object_key"], other_binding.GetAllocator()),
        other_binding.GetAllocator());
    Set(other_binding, "source_snapshot", Copy(other_source, other_binding.GetAllocator()),
        other_binding.GetAllocator());
    Set(other_binding["confirmation"], "source_revision", Get(other_source, "source_revision"),
        other_binding.GetAllocator());
    SemanticsSaveRequest other_bound{"binding", "bytes", Encode(other_binding)};
    other_bound.source_observed = true;
    assert(registry.Save(other_bound, &saved, &error) == 0);
    auto contextual = Parse(R"({"schema_id":"flowsql.operator-data-contract.v1","title":"contexts",
      "operator":{"category":"fixture","name":"contexts","version":"1"},
      "configuration":{"invocation_schema":{"type":"object","additionalProperties":true},"affects":{}},
      "inputs":[{"port_id":"datasets","min_count":1,"max_count":2,"shape":"dataframe","roles":[],
        "repeat_from_config_pointer":"/datasets","item_key_pointer":"/id","role_templates":[{
        "repeat_from_pointer":"/metrics","item_key_pointer":"/id","discriminator_pointer":"/kind","cases":{
        "value":{"roles":[{"role_id":"measure","required":true,"types":[],"nullable":null,"semantic_requirements":{},"selector":{"kind":"config","scope":"role_item","pointer":"/column","cardinality":"one"}}]},
        "ratio":{"roles":[{"role_id":"numerator","required":true,"types":[],"nullable":null,"semantic_requirements":{},"selector":{"kind":"config","scope":"role_item","pointer":"/numerator","cardinality":"one"}},
        {"role_id":"denominator","required":true,"types":[],"nullable":null,"semantic_requirements":{},"selector":{"kind":"config","scope":"role_item","pointer":"/denominator","cardinality":"one"}}]}}}]}],
      "outputs":[{"port_id":"results","shape":"dataframe","mode":"unknown","rules":[],"context_semantics":{
      "input_port":"datasets","repeat_from_config_pointer":"/datasets","item_key_pointer":"/id","metric_pointer":"/metrics",
      "metric_key_pointer":"/id","identity_columns":["dataset_id","metric_id"]}}],"limitations":[]})");
    assert(registry.Save({"operator_contract", "fixture.contexts", Encode(contextual)}, &saved, &error) == 0);
    auto context_call =
        Parse(R"({"contract_ref":"operator_contract.fixture.contexts@1","component_revision":"actual-digest",
      "effective_parameters":{},"config_refs":[],"execution_context":{"input_shapes":["dataframe","dataframe"]},
      "normalized_config":{"datasets":[{"id":"a","metrics":[{"id":"latency","kind":"value","column":"latency"}]},
      {"id":"b","metrics":[{"id":"size","kind":"value","column":"latency"},{"id":"rate","kind":"ratio","numerator":"latency","denominator":"id"}]}]},
      "inputs":[{"port_id":"datasets","item_key":"a","input_index":0,"binding_ref":"binding.input@1"},
      {"port_id":"datasets","item_key":"b","input_index":1,"binding_ref":"binding.bytes@1"}]})");
    auto& cxa = context_call.GetAllocator();
    context_call["inputs"][0].AddMember("snapshot", Copy(source, cxa), cxa);
    context_call["inputs"][1].AddMember("snapshot", Copy(other_source, cxa), cxa);
    std::string context_result;
    assert(service.CheckAndDerive(Encode(context_call).c_str(), &context_result, &error) == 0);
    auto explained = Parse(context_result.c_str());
    const auto& contexts = explained["output_previews"][0]["semantic_contexts"];
    assert(contexts.Size() == 3 && explained["output_previews"][0]["context_state"] == "complete");
    assert(contexts[0]["roles"][0]["source_semantics"]["unit"]["value"]["code"] == "ms");
    assert(contexts[1]["roles"][0]["source_semantics"]["unit"]["value"]["code"] == "B");
    assert(contexts[0]["required_context_refs"][0] == "dictionary.input@1");
    assert(contexts[1]["required_context_refs"][0] == "dictionary.bytes@1");
    assert(contexts[2]["roles"].Size() == 2 && contexts[2]["unit"]["state"] == "unknown");
    context_call["inputs"][1].RemoveMember("binding_ref");
    assert(service.CheckAndDerive(Encode(context_call).c_str(), &context_result, &error) == 0);
    explained = Parse(context_result.c_str());
    assert(explained["output_previews"][0]["context_state"] == "incomplete");
    assert(explained["output_previews"][0]["semantic_contexts"][0]["state"] == "complete");
    assert(explained["output_previews"][0]["semantic_contexts"][1]["state"] == "incomplete");
    registry.Close();
    assert(registry.Open(path, &error) == 0);
    assert(registry.Resolve(exact.c_str(), &missing, &error) == 0);
    registry.Close();
    unlink(path);
    std::cout << "PASS output derivation, zero rows, drift, conditions, unknown, rollback, exact restart history\n";
}
