// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <channels/config/operator_data_contract_model.h>
#include <operators/baseliner/baseliner_contract.h>
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
