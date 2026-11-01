// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <arrow/api.h>
#include <channels/config/operator_data_contract_model.h>
#include <channels/config/operator_usage.h>
#include <operators/npm_basic/config/npm_basic_task_config.h>
#include <operators/npm_basic/npm_family_result_contract.h>
#include <rapidjson/pointer.h>
#include <cassert>
#include <iostream>

extern "C" const char* flowsql_operator_data_contract_v1(int index);
using namespace flowsql;
using namespace flowsql::dataset;
using namespace flowsql::channels::config;
int main() {
    const char* text = flowsql_operator_data_contract_v1(0);
    assert(text && std::string(text) == flowsql_operator_data_contract_v1(1));
    assert(!flowsql_operator_data_contract_v1(-1) && !flowsql_operator_data_contract_v1(2));
    Json doc;
    std::vector<SemanticsReference> references;
    DataSemanticsErrorV1 error;
    const int rc = NormalizeSemantics("operator_contract", text, &doc, &references, &error);
    if (rc) std::cerr << error.field_path << " " << error.message << '\n';
    assert(rc == 0);
    assert(Get(doc["operator"], "category") == "npm" && Get(doc["operator"], "name") == "basic");
    assert(doc["configuration"]["invocation_schema"]["properties"].MemberCount() == 17);
    assert(doc["outputs"].Size() == 2 && doc["outputs"][0]["variants"].Size() == 13);
    assert(doc["resources"]["managed_results"]["entities"].Size() == 12);
    for (const auto kind :
         {npm::NpmFamilyEntityKind::kBasic, npm::NpmFamilyEntityKind::kSession, npm::NpmFamilyEntityKind::kDns,
          npm::NpmFamilyEntityKind::kHttp1, npm::NpmFamilyEntityKind::kTls, npm::NpmFamilyEntityKind::kIcmp}) {
        for (const auto family : {packet::AddressFamily::kIPv4, packet::AddressFamily::kIPv6}) {
            const auto entity = npm::NpmFamilyEntityDescriptor(kind, family);
            const auto& structure = doc["resources"][entity.entity_id.c_str()];
            const auto& fields = structure["fields"];
            assert(fields.Size() == entity.schema->num_fields());
            for (rapidjson::SizeType i = 0; i < fields.Size(); ++i) {
                assert(Get(fields[i], "name") == entity.schema->field(i)->name());
                assert(Get(fields[i]["type"], "name") == entity.schema->field(i)->type()->ToString());
                assert(fields[i]["nullable"].GetBool() == entity.schema->field(i)->nullable());
            }
            assert(Get(structure, "identity_column") == entity.identity_column);
            assert(Get(structure["metadata"], "flowsql.timestamp_unit") == "ns");
        }
    }
    auto check = [&](const char* parameters, bool accepted) {
        Json supplied;
        supplied.Parse(parameters);
        assert(!supplied.HasParseError());
        npm::NpmBasicTaskConfig production;
        const auto parsed = npm::ParseNpmBasicTaskConfig(parameters, &production);
        assert((parsed.error == npm::NpmBasicTaskConfigError::kNone) == accepted);
        assert((ValidateOperatorParameters(doc, supplied, &error) == 0) == accepted);
    };
    check(R"({"observing":"basic_ipv4"})", true);
    check(R"({"observing":"basic_ipv4","source_domains":"0:7;1:7"})", true);
    check(
        R"({"features":"basic,session","observing":"session_ipv6","parameters":"{\"schema_version\":1,\"core\":{\"result_mode\":\"final\"}}"})",
        true);
    for (const char* key :
         {"run_mode", "result_mode", "overload_policy", "output_interval_ns", "payload_sample_packets",
          "tcp_idle_timeout_ns", "udp_idle_timeout_ns", "out_of_order_tolerance_ns", "max_active_sessions",
          "max_tracked_bytes", "max_pending_output_bytes", "session_max_tcp_ranges_per_direction"}) {
        Json supplied;
        supplied.Parse(R"({"observing":"basic_ipv4","parameters":"{\"schema_version\":1}"})");
        Set(supplied, key, "1", supplied.GetAllocator());
        check(Encode(supplied).c_str(), false);
    }
    check(R"({"observing":"basic_ipv4","parameters":"{\"schema_version\":2}"})", false);
    check(R"({"observing":"basic_ipv4","parameters":"\"{}\""})", false);
    check(R"({"observing":"basic_ipv4","unknown":"1"})", false);
    check(R"({"observing":"basic_ipv4","result_mode":null})", false);
    check(R"({"observing":"basic_ipv4","parameters":"{\"schema_version\":1,\"core\":{\"max_active_sessions\":0}}"})",
          false);
    // Unconsumed module objects are allowed by production; do not invent a stricter global schema.
    check(R"({"observing":"basic_ipv4","parameters":"{\"schema_version\":1,\"unused_module\":{\"future\":42}}"})",
          true);
    Json subject, basis, contract, notes;
    Json context(rapidjson::kObjectType);
    subject.Parse(R"({"kind":"operator","name":"npm.basic"})");
    basis.Parse(R"({"contract_ref":"operator_contract.npm.basic@1"})");
    contract.Parse(R"({"state":"declared"})");
    Set(contract, "document", Copy(doc, contract.GetAllocator()), contract.GetAllocator());
    auto usage = operator_usage::Build(subject, basis, contract, notes, context);
    assert(usage["usage"]["identity"]["purposes"][0]["text"] == doc["description"]);
    assert(usage["usage"]["parameters"]["description"] == doc["configuration"]["description"]);
    assert(usage["usage"]["inputs"][0]["description"] == doc["inputs"][0]["description"]);
    assert(usage["usage"]["outputs"][0]["description"] == doc["outputs"][0]["description"]);
    assert(Get(usage["usage"]["outputs"][0], "fields_state") == "dynamic");
    for (const auto& gap : usage["gaps"].GetArray()) assert(Get(gap, "code") != "OUTPUT_FIELDS_UNDECLARED");
    for (const auto& link : usage["details"].GetArray())
        assert(rapidjson::Pointer(Get(link, "resource_pointer").c_str()).Get(doc));
    Json invalid;
    invalid.CopyFrom(doc, invalid.GetAllocator());
    Set(invalid["inputs"][0], "structure_ref", "#/resources/missing", invalid.GetAllocator());
    assert(NormalizeSemantics("operator_contract", Encode(invalid), &notes, &references, &error) != 0);
    invalid.CopyFrom(doc, invalid.GetAllocator());
    Set(invalid["configuration"], "description", Value(1), invalid.GetAllocator());
    assert(NormalizeSemantics("operator_contract", Encode(invalid), &notes, &references, &error) != 0);
    std::cout << "PASS: real NPM dual-ABI contract, 12 production output schemas, invocation boundaries and exact "
                 "usage projection\n";
}
