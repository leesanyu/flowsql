// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <arrow/api.h>
#include <channels/config/operator_data_contract_model.h>
#include <operators/baseliner/baseliner_contract.h>
#include <operators/baseliner/result_codec.h>
#include <rapidjson/pointer.h>
#include <cassert>
#include <common/channel_dataset.hpp>
#include <fstream>
#include <iostream>
using namespace flowsql;
using namespace flowsql::dataset;
using namespace flowsql::channels::config;
Json Read(const char* relative) {
    std::ifstream input(std::string(FLOWSQL_SOURCE_ROOT) + relative);
    assert(input);
    const std::string text{std::istreambuf_iterator<char>(input), {}};
    Json document;
    document.Parse(text.c_str());
    assert(!document.HasParseError());
    return document;
}
size_t CheckDefaults(const Value& schema, const Value* supplied, const Value& effective, const Json& contract) {
    if (!schema.IsObject()) return 0;
    if (schema.HasMember("$ref")) {
        const auto ref = Get(schema, "$ref");
        const auto* resolved = rapidjson::Pointer(ref.substr(1).c_str()).Get(contract);
        assert(resolved);
        return CheckDefaults(*resolved, supplied, effective, contract);
    }
    size_t checked = 0;
    if (schema.HasMember("oneOf")) {
        size_t matches = 0;
        for (const auto& branch : schema["oneOf"].GetArray()) {
            DataSemanticsErrorV1 error;
            if (ValidateOperatorConfiguration(branch, effective, contract, &error) == 0) {
                ++matches;
                checked += CheckDefaults(branch, supplied, effective, contract);
            }
        }
        assert(matches == 1);
    }
    if (effective.IsObject() && schema.HasMember("properties")) {
        for (const auto& property : schema["properties"].GetObject()) {
            const auto* original = supplied && supplied->IsObject() && supplied->HasMember(property.name)
                                       ? &(*supplied)[property.name]
                                       : nullptr;
            if (!effective.HasMember(property.name)) continue;
            const auto& actual = effective[property.name];
            if (!original && property.value.IsObject() && property.value.HasMember("default")) {
                if (actual != property.value["default"]) std::cerr << "Default mismatch: " << Text(property.name);
                assert(actual == property.value["default"]);
                ++checked;
            }
            checked += CheckDefaults(property.value, original, actual, contract);
        }
    }
    if (effective.IsArray() && schema.HasMember("items")) {
        for (rapidjson::SizeType i = 0; i < effective.Size(); ++i) {
            const auto* original = supplied && supplied->IsArray() && i < supplied->Size() ? &(*supplied)[i] : nullptr;
            checked += CheckDefaults(schema["items"], original, effective[i], contract);
        }
    }
    return checked;
}
void CheckOutputs(const Json& contract, const baseliner::ConfigSnapshot& config) {
    using namespace baseliner;
    for (const auto& entry : {std::make_pair("results", SchemaKind::kResults),
                              std::make_pair("model_parameters", SchemaKind::kModelParameters),
                              std::make_pair("relation_fusion", SchemaKind::kRelationFusion),
                              std::make_pair("maintenance", SchemaKind::kMaintenance)}) {
        const auto actual = MakeSchema(entry.second);
        const auto resource = std::string(entry.first) + "_structure";
        const auto& fields = contract["resources"][resource.c_str()]["fields"];
        assert(fields.Size() == actual->num_fields());
        for (rapidjson::SizeType i = 0; i < fields.Size(); ++i) {
            assert(Get(fields[i], "name") == actual->field(i)->name());
            assert(Get(fields[i]["type"], "name") == actual->field(i)->type()->ToString());
            assert(fields[i]["type"]["parameters"].ObjectEmpty());
            assert(fields[i]["nullable"].GetBool() == actual->field(i)->nullable());
        }
    }
    EvaluationRow evaluation;
    evaluation.values.status = BaselineStatus::kNotTrained;
    evaluation.values.observed = 42.0;
    evaluation.values.target_bucket = evaluation.values.issued_after_bucket = 10;
    evaluation.unit = "value";
    EvaluationRow forecast = evaluation;
    forecast.values.forecast = true;
    forecast.values.target_bucket = 11;
    forecast.values.observed.reset();
    forecast.values.can_alert.reset();
    std::shared_ptr<arrow::RecordBatch> batch;
    std::string error;
    assert(EncodeResults(config, {evaluation, forecast}, 1 << 20, &batch, &error) == 0);
    auto kinds = std::static_pointer_cast<arrow::StringArray>(batch->GetColumnByName("result_kind"));
    assert(kinds->GetString(0) == "evaluation" && kinds->GetString(1) == "forecast");
    auto observed = std::static_pointer_cast<arrow::DoubleArray>(batch->GetColumnByName("observed"));
    assert(observed->Value(0) == 42.0 && observed->IsNull(1));
    for (const char* band : {"expected", "lower", "upper"}) assert(batch->GetColumnByName(band)->IsNull(0));
    const auto& rules = contract["outputs"][0]["row_cases"][1]["field_rules"];
    for (const char* name : {"observed", "confidence", "maturity", "score", "is_outside_band", "can_score",
                             "can_update", "update_weight", "can_alert"}) {
        assert(batch->GetColumnByName(name)->IsNull(1));
        assert(rules.HasMember(name) && rules[name]["must_be_null"].IsTrue());
    }
    ModelParametersRow cold;
    cold.parameters.status = BaselineStatus::kNotTrained;
    assert(EncodeModelParameters(config, {cold}, 1 << 20, &batch, &error) == 0);
    assert(batch->GetColumnByName("parameters_json")->IsNull(0));
    assert(contract["outputs"][1]["row_cases"][0]["field_rules"]["parameters_json"]["must_be_null"].IsTrue());
}
int main() {
    auto contract = Read("/config/semantics/examples/operator-contract-baseliner.json");
    auto fixtures = Read("/src/tests/test_config_channel/fixtures/operator-baseliner-configs.json");
    DataSemanticsErrorV1 issue;
    for (const auto& fixture : fixtures.GetObject()) {
        const auto& value = fixture.value;
        baseliner::ConfigSnapshot production;
        const auto parsed =
            baseliner::NormalizeInlineConfig(Encode(value["config"]), Get(value, "source"), &production);
        if (!parsed.ok()) std::cerr << parsed.path << " " << parsed.message << '\n';
        assert(parsed.ok());
        Json parameters(rapidjson::kObjectType);
        Set(parameters, "parameters", Encode(value["config"]), parameters.GetAllocator());
        if (ValidateOperatorParameters(contract, parameters, &issue)) std::cerr << issue.message << issue.field_path;
        assert(ValidateOperatorParameters(contract, parameters, &issue) == 0);
        Json normalized;
        normalized.Parse(production.original_json.c_str());
        assert(ValidateOperatorConfiguration(contract["configuration"]["normalized_config_schema"], normalized,
                                             contract, &issue) == 0);
        assert(production.config.read.page_rows == 4096 && production.config.persistence.lease_ms == 30000);
        const auto effective_text = baseliner::EffectiveConfigJson(production.config);
        Json effective;
        effective.Parse(effective_text.c_str());
        if (ValidateOperatorConfiguration(contract["configuration"]["normalized_config_schema"], effective, contract,
                                          &issue))
            std::cerr << issue.field_path << " " << issue.message << '\n';
        assert(ValidateOperatorConfiguration(contract["configuration"]["normalized_config_schema"], effective, contract,
                                             &issue) == 0);
        assert(effective["read_policy"]["poll_interval_ms"].GetUint() == 1000);
        assert(effective["state_policy"]["max_runtime_identities"].GetUint() == 1024);
        assert(effective["persistence"]["renew_ms"].GetUint() == 10000);
        assert(effective["datasets"][0]["metrics"][0]["null_policy"] == "fail");
        assert(CheckDefaults(contract["resources"]["task_config"], &normalized, effective, contract) >= 20);
        CheckOutputs(contract, production);
        baseliner::ConfigSnapshot roundtrip;
        const auto status = baseliner::ParseConfig(effective_text, &roundtrip);
        if (!status.ok()) std::cerr << status.path << " " << status.message << '\n';
        assert(status.ok() && baseliner::EffectiveConfigJson(roundtrip.config) == effective_text);
        for (size_t d = 0; d < production.config.datasets.size(); ++d)
            for (size_t m = 0; m < production.config.datasets[d].metrics.size(); ++m)
                assert(roundtrip.config.datasets[d].metrics[m].algorithm_config_json ==
                       production.config.datasets[d].metrics[m].algorithm_config_json);
        auto customized = production.config;
        customized.read.page_rows = 17;
        customized.persistence.restore = "fresh";
        customized.datasets[0].metrics[0].null_policy = baseliner::InvalidPolicy::kSkip;
        Json observed;
        observed.Parse(baseliner::EffectiveConfigJson(customized).c_str());
        assert(observed["read_policy"]["page_rows"].GetUint() == 17 && observed["persistence"]["restore"] == "fresh" &&
               observed["datasets"][0]["metrics"][0]["null_policy"] == "skip");
    }
    // Independent negative inputs: both the public declaration and the actual parser must reject them.
    auto reject = [&](const Json& configuration) {
        baseliner::ConfigSnapshot production;
        assert(!baseliner::ParseConfig(Encode(configuration), &production).ok());
        assert(ValidateOperatorConfiguration(contract["resources"]["task_config"], configuration, contract, &issue) !=
               0);
    };
    // Own the allocator of every mutated fixture.
    Json invalid;
    invalid.CopyFrom(fixtures["value"]["config"], invalid.GetAllocator());
    invalid["datasets"][0]["metrics"][0]["feature_type"].SetString("value_sampled", invalid.GetAllocator());
    reject(invalid);
    invalid.CopyFrom(fixtures["value"]["config"], invalid.GetAllocator());
    invalid["datasets"][0]["metrics"][0].AddMember("numerator", rapidjson::Value(rapidjson::kObjectType),
                                                   invalid.GetAllocator());
    reject(invalid);
    invalid.CopyFrom(fixtures["multi"]["config"], invalid.GetAllocator());
    invalid["datasets"][1]["metrics"][0].RemoveMember("denominator");
    reject(invalid);
    invalid.CopyFrom(fixtures["relation"]["config"], invalid.GetAllocator());
    invalid["datasets"][0]["metrics"][0].RemoveMember("metrics");
    reject(invalid);
    invalid.CopyFrom(fixtures["value"]["config"], invalid.GetAllocator());
    rapidjson::Value forecast(rapidjson::kObjectType);
    forecast.AddMember("horizon_buckets", 4097, invalid.GetAllocator());
    invalid.AddMember("forecast", forecast, invalid.GetAllocator());
    reject(invalid);
    invalid.CopyFrom(fixtures["value"]["config"], invalid.GetAllocator());
    invalid["clock"]["bucket_seconds"].SetInt(0);
    reject(invalid);
    std::cout << "PASS: closed Baseliner schema and production parser agree for Value, sampled, shorthand, "
                 "multi-dataset Ratio/Relation and illegal branches\n";
}
