// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <operators/baseliner/evaluation.h>
#include <operators/baseliner/result_codec.h>
#include <plugins/baseline/baseline_plugin.h>
#include <rapidjson/document.h>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
using namespace flowsql;
using namespace flowsql::baseliner;
namespace {
constexpr auto json = R"({"schema_version":1,"task_key":"evaluation","source":"sqlite.source","mode":"snapshot",
"clock":{"bucket_seconds":60,"timezone":"UTC"},"calendar":{"calendar_id":"cn-holiday","calendar_version":"2026.1"},
"datasets":[{"id":"d","table":"facts","fields":{"id":"uint64","t":"int64","v":"float64","n":"float64","den":"float64"},
"scope":{"begin_bucket":0,"end_bucket":10000,"consistency":"immutable_range"},"series_keys":["id"],
"deduplicate":{"keys":["id","t"],"on_duplicate":"require_equal"},"bucket":{"column":"t","unit":"bucket_id"},
"metrics":[{"id":"v","kind":"value","column":"v","feature_type":"value_basic","profile":"default"},
{"id":"r","kind":"ratio","feature_type":"ratio","profile":"rate_core","numerator":{"column":"n"},"denominator":{"column":"den"}}]}]})";
ConfigSnapshot Config() {
    ConfigSnapshot c;
    auto status = ParseConfig(json, &c);
    if (!status.ok()) std::cerr << status.path << ':' << status.message << '\n';
    assert(status.ok());
    return c;
}
constexpr auto relation_json =
    R"({"schema_version":1,"task_key":"relation-evaluation","source":"sqlite.source","mode":"snapshot",
"clock":{"bucket_seconds":60,"timezone":"UTC"},"calendar":{"calendar_id":"cn-holiday","calendar_version":"2026.1"},
"datasets":[{"id":"d","table":"facts","fields":{"g":"uint64","t":"int64","x":"float64"},"scope":{"begin_bucket":0,"end_bucket":10000,"consistency":"immutable_range"},
"series_keys":["g"],"deduplicate":{"keys":["g","t"],"on_duplicate":"require_equal"},"bucket":{"column":"t","unit":"bucket_id"},
"metrics":[{"id":"dist","kind":"relation","feature_type":"relation","profile":"default","group_space":{"id":"groups","version":"v1","column":"g","unknown":"other","other_group_idx":99},
"metrics":[{"id":"m","column":"x","aggregate":"sum"}],"support_policy":{"k_support":3,"min_hist_share":0.01,"min_active_ratio":0.1},"summary_policy":{"k_head":2,"k_stable":1}}]}]})";
ConfigSnapshot RelationConfig() {
    ConfigSnapshot c;
    auto status = ParseConfig(relation_json, &c);
    if (!status.ok()) std::cerr << status.path << ':' << status.message << '\n';
    assert(status.ok());
    return c;
}
struct Environment {
    baseline::BaselinePlugin plugin;
    explicit Environment(const char* options = nullptr) {
        assert(plugin.Option(options) == 0 && plugin.Load(nullptr) == 0 && plugin.Start() == 0);
    }
    ~Environment() {
        plugin.Stop();
        plugin.Unload();
    }
};
void Ok(int rc, const EvaluationEngine& e) {
    if (rc) std::cerr << e.LastError() << '\n';
    assert(rc == 0);
}
std::string Key(const std::string& identity) {
    const char* digits = "0123456789abcdef";
    std::string key;
    for (unsigned char c : identity) {
        key += digits[c >> 4];
        key += digits[c & 15];
    }
    return key;
}
Observation Input(BaselineTaskKind kind, int64_t bucket, double value = 100) {
    Observation o;
    o.dataset_id = "d";
    o.metric_id = kind == BaselineTaskKind::kValue ? "v" : "r";
    o.kind = kind;
    o.source_epoch = "source-1";
    o.bucket = bucket;
    assert(EncodeIdentity("sqlite.source", "d", o.metric_id, kind, {uint64_t{1}}, &o.identity).ok());
    if (kind == BaselineTaskKind::kValue) {
        o.value = value;
        o.sample_count = 1;
    } else {
        o.numerator = value;
        o.denominator = 1000;
    }
    return o;
}
void Equal(const EvaluationRow& row, const RollingBaselineResult& direct, bool cold = false) {
    assert(row.rolling.status == direct.status);
    assert(row.rolling.can_update == direct.can_update && row.rolling.update_weight == direct.update_weight);
    assert(row.rolling.can_alert == direct.can_alert && row.rolling.z_score == direct.z_score);
    assert(row.values.observed == direct.observed);
    if (cold || direct.status != BaselineStatus::kOk) {
        assert(!row.values.expected && !row.values.lower && !row.values.upper);
        assert(row.values.status != BaselineStatus::kOk && row.values.can_alert == false);
    } else {
        assert(row.values.expected == direct.baseline_mu && row.values.lower == direct.baseline_lower &&
               row.values.upper == direct.baseline_upper);
        assert(row.values.status == direct.status && row.values.can_alert == direct.can_alert);
    }
    assert(ValidateResult(row.values).ok());
}
void ColdAndDirect() {
    Environment wrapped, direct;
    auto c = Config();
    EvaluationEngine e;
    Ok(e.Open(c, &wrapped.plugin, &wrapped.plugin), e);
    auto v =
        direct.plugin
            .CreateValueTask(c.config.datasets[0].metrics[0].algorithm_config_json, BaselineSerializationFormat::kJson)
            .second;
    auto r =
        direct.plugin
            .CreateRatioTask(c.config.datasets[0].metrics[1].algorithm_config_json, BaselineSerializationFormat::kJson)
            .second;
    assert(v && r);
    bool skipped = false;
    for (int64_t t = 0; t < 80; ++t) {
        for (auto kind : {BaselineTaskKind::kValue, BaselineTaskKind::kRatio}) {
            auto o =
                Input(kind, t, kind == BaselineTaskKind::kValue ? (t == 30 ? 1e12 : 100 + t % 3) : (t == 40 ? 1 : 800));
            EvaluationOutput out;
            Ok(e.Submit(o, &out), e);
            assert(out.results.size() == 1 && !out.relation);
            auto result = kind == BaselineTaskKind::kValue
                              ? v->SubmitObservation({Key(o.identity), t, *o.value, 1}, {})
                              : r->SubmitObservation({Key(o.identity), t, *o.numerator, *o.denominator}, {});
            Equal(out.results[0], result, t == 0);
            skipped |= result.status == BaselineStatus::kOk && !result.can_update;
            assert(out.results[0].values.target_bucket == t && out.results[0].values.issued_after_bucket == t);
            assert(out.results[0].band_kind == "detection" && !out.results[0].model_basis_id.empty());
            EvaluationOutput bad;
            auto before = e.QuerySeriesSnapshot(o.dataset_id, o.metric_id, o.identity);
            assert(e.Submit(o, &bad) != 0 && bad.results.empty());
            auto invalid = o;
            invalid.bucket = t + 1;
            if (kind == BaselineTaskKind::kValue)
                invalid.value = std::numeric_limits<double>::quiet_NaN();
            else
                invalid.denominator = 0;
            assert(e.Submit(invalid, &bad) != 0 && bad.results.empty());
            assert(before == e.QuerySeriesSnapshot(o.dataset_id, o.metric_id, o.identity));
        }
    }
    assert(skipped);
    Ok(e.Finish(), e);
    v->Close();
    r->Close();
}
void History() {
    for (auto kind : {BaselineTaskKind::kValue, BaselineTaskKind::kRatio}) {
        Environment wrapped, direct;
        auto c = Config();
        c.config.bootstrap = {true, 0, 200, 100, false};
        EvaluationEngine e;
        Ok(e.Open(c, &wrapped.plugin, &wrapped.plugin), e);
        ValueBootstrapInput vi;
        RatioBootstrapInput ri;
        vi.options.min_observation_count = ri.options.min_observation_count = 100;
        for (int64_t t = 0; t < 200; ++t) {
            auto o = Input(kind, t, kind == BaselineTaskKind::kValue ? 100 + t % 10 : 800 + t % 10);
            vi.series_key = ri.series_key = Key(o.identity);
            if (kind == BaselineTaskKind::kValue)
                vi.observations.push_back({t, *o.value, 1});
            else
                ri.observations.push_back({t, *o.numerator, *o.denominator});
            EvaluationOutput out;
            Ok(e.Submit(o, &out), e);
            assert(out.results.empty());
        }
        const auto& m = c.config.datasets[0].metrics[kind == BaselineTaskKind::kValue ? 0 : 1];
        auto v = kind == BaselineTaskKind::kValue
                     ? direct.plugin.CreateValueTask(m.algorithm_config_json, BaselineSerializationFormat::kJson).second
                     : nullptr;
        auto r = kind == BaselineTaskKind::kRatio
                     ? direct.plugin.CreateRatioTask(m.algorithm_config_json, BaselineSerializationFormat::kJson).second
                     : nullptr;
        assert((v ? v->Bootstrap(vi) : r->Bootstrap(ri)).ok());
        auto o = Input(kind, 200, kind == BaselineTaskKind::kValue ? 150 : 950);
        EvaluationOutput out;
        Ok(e.Submit(o, &out), e);
        assert(out.results.size() == 1);
        auto result = v ? v->SubmitObservation({vi.series_key, 200, *o.value, 1}, {})
                        : r->SubmitObservation({ri.series_key, 200, *o.numerator, *o.denominator}, {});
        Equal(out.results[0], result);
        assert(out.results[0].model_basis_id.find("bootstrap") != std::string::npos);
        EvaluationOutput bad;
        assert(e.Submit(Input(kind, 199), &bad) != 0);
        Ok(e.Finish(), e);
        if (v) v->Close();
        if (r) r->Close();
    }
    for (bool cold : {false, true}) {
        Environment env;
        auto c = Config();
        c.config.bootstrap = {true, 0, 10, 10, cold};
        EvaluationEngine e;
        Ok(e.Open(c, &env.plugin, &env.plugin), e);
        EvaluationOutput out;
        Ok(e.Submit(Input(BaselineTaskKind::kValue, 0), &out), e);
        int rc = e.Submit(Input(BaselineTaskKind::kValue, 10), &out);
        if (cold) {
            Ok(rc, e);
            assert(out.results.size() == 1 && out.results[0].values.status == BaselineStatus::kNotTrained);
        } else
            assert(rc != 0 && out.results.empty());
    }
    Environment env;
    auto c = Config();
    c.config.bootstrap = {true, 0, 10, 10, false};
    EvaluationEngine e;
    Ok(e.Open(c, &env.plugin, &env.plugin), e);
    EvaluationOutput out;
    Ok(e.Submit(Input(BaselineTaskKind::kValue, 0), &out), e);
    assert(e.Finish() != 0);
}
void BoundsAndLowSupport() {
    Environment env;
    auto c = Config();
    c.config.bootstrap = {true, 2, 12, 10, true};
    c.config.state.limits.max_runtime_identities = 1;
    EvaluationEngine e;
    Ok(e.Open(c, &env.plugin, &env.plugin), e);
    EvaluationOutput out;
    Ok(e.Submit(Input(BaselineTaskKind::kValue, 0), &out), e);
    assert(out.results.empty());
    auto o = Input(BaselineTaskKind::kRatio, 12, 1);
    o.denominator = 1;
    Ok(e.Submit(o, &out), e);
    assert(out.results.size() == 1 && !out.results[0].values.expected);
    assert(e.Submit(Input(BaselineTaskKind::kValue, 12), &out) != 0 && out.results.empty());
    auto wrong = o;
    wrong.bucket = 13;
    wrong.source_epoch = "changed";
    assert(e.Submit(wrong, &out) != 0 && out.results.empty());
    o.bucket = 13;
    o.numerator = 800;
    o.denominator = 1000;
    Ok(e.Submit(o, &out), e);
    assert(out.results[0].values.status == BaselineStatus::kNotTrained);
    Ok(e.Finish(), e);
}
Observation RelationInput(int64_t bucket) {
    Observation o;
    o.dataset_id = "d";
    o.metric_id = "dist";
    o.kind = BaselineTaskKind::kRelation;
    o.source_epoch = "source-1";
    o.bucket = bucket;
    assert(EncodeIdentity("sqlite.source", "d", "dist", BaselineTaskKind::kRelation, {uint64_t{1}}, &o.identity).ok());
    o.groups = {1, 2, 3};
    o.metrics = {{"m", 1000.0 + bucket, 3, {500.0, 300.0, 200.0 + bucket}}};
    return o;
}
void RelationParity() {
    Environment wrapped, direct;
    auto c = RelationConfig();
    EvaluationEngine e;
    Ok(e.Open(c, &wrapped.plugin, &wrapped.plugin), e);
    auto task = direct.plugin.CreateRelationTask(c.config.datasets[0].metrics[0].algorithm_config_json,
                                                 BaselineSerializationFormat::kJson);
    assert(task.first == BaselineStatus::kOk && task.second);
    for (int64_t bucket = 0; bucket < 8; ++bucket) {
        auto input = RelationInput(bucket);
        EvaluationOutput output;
        Ok(e.Submit(input, &output), e);
        RelationRollingObservation direct_input;
        direct_input.series_key = Key(input.identity);
        direct_input.bucket_id = bucket;
        direct_input.group_idx = input.groups;
        direct_input.metrics = {{"m", 1000.0 + bucket, 3, {500.0, 300.0, 200.0 + bucket}}};
        auto expected = task.second->SubmitObservation(direct_input, {});
        assert(output.relation && output.relation->status == expected.status);
        assert(output.relation->basis_version == expected.basis_version);
        assert(output.relation->basis_status == expected.basis_status);
        if (bucket == 0) assert(expected.basis_version == 0 && expected.basis_status == "collecting");
        assert(output.results.size() == expected.routed_results.size());
        for (size_t i = 0; i < output.results.size(); ++i) {
            assert(output.results[i].summary_id == expected.routed_results[i].summary);
            assert(output.results[i].basis_id == (expected.routed_results[i].basis_scoped
                                                      ? std::to_string(expected.routed_results[i].basis_version)
                                                      : ""));
            assert(output.results[i].rolling.status == expected.routed_results[i].rolling.status);
            assert(output.results[i].values.target_bucket == bucket);
            const auto& r = expected.routed_results[i].rolling;
            bool cold = std::find(r.uncertainty_source.begin(), r.uncertainty_source.end(), "first_observation") !=
                        r.uncertainty_source.end();
            Equal(output.results[i], r, cold);
            assert(output.results[i].input.metric_id == expected.routed_results[i].metric);
            assert(ValidateResult(output.results[i].values).ok());
        }
        assert(output.relation->has_fusion_result == expected.has_fusion_result);
        if (expected.has_fusion_result) {
            assert(output.relation->fusion_result.relation_risk == expected.fusion_result.relation_risk);
            assert(output.relation->fusion_result.single_risk == expected.fusion_result.single_risk);
            assert(output.relation->fusion_result.pattern_risk == expected.fusion_result.pattern_risk);
            assert(output.relation->fusion_result.dominant_single.size() ==
                   expected.fusion_result.dominant_single.size());
        }
        EvaluationOutput duplicate;
        auto before = e.QuerySeriesSnapshot(input.dataset_id, input.metric_id, input.identity);
        assert(e.Submit(input, &duplicate) != 0 && duplicate.results.empty() && !duplicate.relation);
        auto invalid = input;
        invalid.bucket += 1;
        invalid.metrics.front().metric = "wrong";
        assert(e.Submit(invalid, &duplicate) != 0 && duplicate.results.empty() && !duplicate.relation);
        assert(before == e.QuerySeriesSnapshot(input.dataset_id, input.metric_id, input.identity));
    }
    Ok(e.Finish(), e);
    assert(task.second->Close() == BaselineStatus::kOk);
}
void ForecastReadOnly() {
    Environment wrapped, direct;
    auto c = Config();
    c.config.horizon_buckets = 3;
    EvaluationEngine e;
    Ok(e.Open(c, &wrapped.plugin, &wrapped.plugin), e);
    auto task = direct.plugin.CreateValueTask(c.config.datasets[0].metrics[0].algorithm_config_json,
                                              BaselineSerializationFormat::kJson);
    assert(task.first == BaselineStatus::kOk && task.second);
    const auto key = Key(Input(BaselineTaskKind::kValue, 0).identity);
    for (int64_t bucket = 0; bucket <= 32; ++bucket) {
        auto input = Input(BaselineTaskKind::kValue, bucket, 100 + bucket % 4);
        EvaluationOutput output;
        Ok(e.Submit(input, &output), e);
        auto expected = task.second->SubmitObservation({key, bucket, *input.value, 1}, {});
        Equal(output.results.front(), expected, bucket == 0);
        assert(e.QuerySeriesSnapshot("d", "v", input.identity) ==
               task.second->QuerySeriesSnapshot(key, BaselineSerializationFormat::kJson));
        assert(output.results.size() == 1 && output.results[0].values.forecast == false);
        if (bucket == 32) {
            assert(output.forecasts.size() == 3);
            const auto before = task.second->QuerySeriesSnapshot(key, BaselineSerializationFormat::kJson);
            auto direct = task.second->PredictRolling(key, 33, 3);
            const auto after = task.second->QuerySeriesSnapshot(key, BaselineSerializationFormat::kJson);
            assert(before == after);
            for (size_t i = 0; i < output.forecasts.size(); ++i) {
                assert(output.forecasts[i].values.forecast && output.forecasts[i].band_kind == "forecast");
                assert(!output.forecasts[i].values.observed && !output.forecasts[i].values.can_alert);
                assert(output.forecasts[i].values.target_bucket == 33 + static_cast<int64_t>(i));
                assert(output.forecasts[i].values.issued_after_bucket == 32);
                assert(output.forecasts[i].model_basis_id != output.results[0].model_basis_id);
                assert(output.forecasts[i].values.status == direct.status);
                if (direct.status == BaselineStatus::kOk) {
                    assert(*output.forecasts[i].values.expected == direct.predictions[i].baseline_mu);
                    assert(*output.forecasts[i].values.lower == direct.predictions[i].baseline_lower);
                    assert(*output.forecasts[i].values.upper == direct.predictions[i].baseline_upper);
                }
                assert(ValidateResult(output.forecasts[i].values).ok());
            }
        }
        (void)expected;
    }
    Ok(e.Finish(), e);
    assert(task.second->Close() == BaselineStatus::kOk);

    auto relation_config = RelationConfig();
    relation_config.config.horizon_buckets = 2;
    Environment relation_env;
    EvaluationEngine relation_engine;
    Ok(relation_engine.Open(relation_config, &relation_env.plugin, &relation_env.plugin), relation_engine);
    EvaluationOutput relation_output;
    for (int64_t bucket = 0; bucket < 4; ++bucket) {
        Ok(relation_engine.Submit(RelationInput(bucket), &relation_output), relation_engine);
        if (bucket == 3) {
            assert(relation_output.relation && !relation_output.results.empty());
            assert(relation_output.forecasts.size() == relation_output.results.size() * 2);
            for (const auto& forecast : relation_output.forecasts) {
                assert(forecast.values.forecast && forecast.summary_id.size() > 0);
                assert(!forecast.values.observed && !forecast.values.can_alert);
                assert(forecast.values.issued_after_bucket == 3);
                assert(ValidateResult(forecast.values).ok());
            }
        }
    }
    Ok(relation_engine.Finish(), relation_engine);
}
void RatioForecastAndFailedPrediction() {
    Environment wrapped, direct;
    auto c = Config();
    c.config.horizon_buckets = 3;
    EvaluationEngine e;
    Ok(e.Open(c, &wrapped.plugin, &wrapped.plugin), e);
    auto task =
        direct.plugin
            .CreateRatioTask(c.config.datasets[0].metrics[1].algorithm_config_json, BaselineSerializationFormat::kJson)
            .second;
    assert(task);
    for (int64_t t = 0; t < 40; ++t) {
        auto input = Input(BaselineTaskKind::kRatio, t, 800 + t % 3);
        if (t == 0) {
            input.numerator = 1;
            input.denominator = 1;
        }
        EvaluationOutput output;
        Ok(e.Submit(input, &output), e);
        const auto key = Key(input.identity);
        task->SubmitObservation({key, t, *input.numerator, *input.denominator}, {});
        auto before = task->QuerySeriesSnapshot(key, BaselineSerializationFormat::kJson);
        auto predictions = task->PredictRolling(key, t + 1, 3);
        assert(before == task->QuerySeriesSnapshot(key, BaselineSerializationFormat::kJson));
        assert(before == e.QuerySeriesSnapshot("d", "r", input.identity));
        assert(output.forecasts.size() == 3);
        for (size_t i = 0; i < 3; ++i) {
            const auto& row = output.forecasts[i];
            assert(row.values.target_bucket == t + 1 + static_cast<int64_t>(i));
            assert(row.values.issued_after_bucket == t && !row.values.observed && !row.values.can_alert);
            if (predictions.status == BaselineStatus::kOk) {
                const auto& expected = predictions.predictions[i];
                assert(row.values.status == expected.status);
                assert(row.values.expected == expected.baseline_mu && row.values.lower == expected.baseline_lower &&
                       row.values.upper == expected.baseline_upper);
            } else {
                assert(row.values.status == predictions.status);
                assert(!row.values.expected && !row.values.lower && !row.values.upper);
            }
            if (t == 0) assert(row.values.status != BaselineStatus::kOk && !row.values.expected);
        }
    }
    auto invalid = Input(BaselineTaskKind::kRatio, std::numeric_limits<int64_t>::max() - 2);
    auto before = e.QuerySeriesSnapshot("d", "r", invalid.identity);
    EvaluationOutput output;
    assert(e.Submit(invalid, &output) != 0 && output.forecasts.empty());
    assert(before == e.QuerySeriesSnapshot("d", "r", invalid.identity));
    Ok(e.Finish(), e);
    task->Close();
}
void SampledAndDownweight() {
    for (bool sampled : {false, true}) {
        Environment wrapped, direct;
        auto text = std::string(json);
        if (sampled) {
            const auto pos = text.find("\"feature_type\":\"value_basic\"");
            text.replace(
                pos, std::string("\"feature_type\":\"value_basic\"").size(),
                "\"feature_type\":\"value_sampled\",\"sample_count\":{\"column\":\"id\",\"aggregate\":\"sum\"}");
            const auto profile = text.find("\"profile\":\"default\"");
            text.replace(profile, std::string("\"profile\":\"default\"").size(), "\"profile\":\"cont_core\"");
        }
        ConfigSnapshot c;
        assert(ParseConfig(text, &c).ok());
        EvaluationEngine e;
        Ok(e.Open(c, &wrapped.plugin, &wrapped.plugin), e);
        auto task = direct.plugin
                        .CreateValueTask(c.config.datasets[0].metrics[0].algorithm_config_json,
                                         BaselineSerializationFormat::kJson)
                        .second;
        assert(task);
        bool downweighted = false, skipped = false;
        for (int64_t t = 0; t < 5; ++t) {
            auto input = Input(BaselineTaskKind::kValue, t, t == 1 ? 110 : 100);
            input.sample_count = sampled ? (t == 2 ? 2 : t == 3 ? 5 : 20) : 1;
            const auto key = Key(input.identity);
            if (!sampled && t == 2) {
                const auto p = task->PredictRolling(key, t);
                const double sigma = (std::log1p(p.baseline_upper) - std::log1p(p.baseline_mu)) / p.band_z;
                input.value = std::expm1(std::log1p(p.baseline_mu) + 4 * sigma);
            }
            if (!sampled && t == 3) input.value = 1e100;
            EvaluationOutput output;
            Ok(e.Submit(input, &output), e);
            const auto expected = task->SubmitObservation({key, t, *input.value, *input.sample_count}, {});
            Equal(output.results.front(), expected, t == 0);
            skipped |= !expected.can_update;
            downweighted |= expected.can_update && expected.update_weight > 0 && expected.update_weight < 1;
        }
        assert(downweighted && skipped);
        Ok(e.Finish(), e);
        task->Close();
    }
}
void RelationVersionsAndForecast() {
    const std::string path = "/tmp/baseline-operator-t4/relation-fast.yaml";
    std::ofstream(path) << R"(baseline:
  rolling_config:
    relation_rolling:
      basis_stats_max_groups: 2
      basis_collect_min_buckets: 1
      basis_ready_min_buckets: 1
      basis_refresh_interval_buckets: 1
      basis_candidate_min_coverage_ratio: 0.01
      basis_replacement_cap_ratio: 1.0
      basis_replacement_cap_max: 8
      basis_handover_warmup_buckets: 2
      basis_threshold_margin: 1.0
      basis_min_stable_refresh_count: 1
)";
    const auto options = "config_file=" + path + ";strict=false";
    Environment wrapped(options.c_str()), direct(options.c_str());
    auto text = std::string(relation_json);
    const auto pos = text.find("{\"id\":\"m\",\"column\":\"x\",\"aggregate\":\"sum\"}");
    text.insert(pos, "{\"id\":\"m2\",\"column\":\"x\",\"aggregate\":\"sum\"},");
    ConfigSnapshot c;
    assert(ParseConfig(text, &c).ok());
    c.config.horizon_buckets = 2;
    c.config.state.limits.max_basis_versions_per_metric = 2;
    EvaluationEngine e;
    Ok(e.Open(c, &wrapped.plugin, &wrapped.plugin), e);
    auto task = direct.plugin
                    .CreateRelationTask(c.config.datasets[0].metrics[0].algorithm_config_json,
                                        BaselineSerializationFormat::kJson)
                    .second;
    assert(task);
    auto control = direct.plugin.Bind(task, c.config.state.limits).second;
    assert(control);
    std::set<uint64_t> versions;
    bool handover = false;
    for (int64_t t = 0; t < 36; ++t) {
        const uint32_t phase = t / 3;
        auto input = RelationInput(t);
        input.groups = {1 + phase * 3, 2 + phase * 3, 3 + phase * 3};
        const double scale = std::pow(20., phase);
        input.metrics = {{"m2", 100 * scale, 2, {80 * scale, 20 * scale, 0}},
                         {"m", 200 * scale, 2, {160 * scale, 40 * scale, 0}}};
        auto before = e.QuerySeriesSnapshot("d", "dist", input.identity);
        EvaluationOutput rejected;
        auto invalid = input;
        std::swap(invalid.metrics[0], invalid.metrics[1]);
        assert(e.Submit(invalid, &rejected) != 0);
        invalid = input;
        invalid.metrics.pop_back();
        assert(e.Submit(invalid, &rejected) != 0);
        invalid = input;
        invalid.metrics[1].total = 0;
        assert(e.Submit(invalid, &rejected) != 0);
        assert(before == e.QuerySeriesSnapshot("d", "dist", input.identity));
        EvaluationOutput output;
        Ok(e.Submit(input, &output), e);
        RelationRollingObservation observation;
        observation.series_key = Key(input.identity);
        observation.bucket_id = t;
        observation.group_idx = input.groups;
        for (const auto& m : input.metrics)
            observation.metrics.push_back({m.metric, m.total, m.active_count, m.values_by_group});
        const auto expected = task->SubmitObservation(observation, {});
        assert(output.relation && expected.status == BaselineStatus::kOk);
        assert(output.relation->basis_status == expected.basis_status &&
               output.relation->basis_version == expected.basis_version &&
               output.relation->handover_active == expected.handover_active);
        handover |= expected.handover_active;
        assert(output.results.size() == expected.routed_results.size());
        assert(output.forecasts.size() == output.results.size() * 2);
        std::set<std::tuple<std::string, std::string, std::string>> keys;
        for (size_t i = 0; i < output.results.size(); ++i) {
            const auto& routed = expected.routed_results[i];
            const auto& row = output.results[i];
            bool cold = std::find(routed.rolling.uncertainty_source.begin(), routed.rolling.uncertainty_source.end(),
                                  "first_observation") != routed.rolling.uncertainty_source.end();
            Equal(row, routed.rolling, cold);
            assert(row.input.metric_id == routed.metric && row.summary_id == routed.summary);
            assert(keys.emplace(row.input.metric_id, row.summary_id, row.basis_id).second);
            if (routed.basis_scoped) versions.insert(routed.basis_version);
            RelationRoutedSummaryQuery query{observation.series_key, routed.metric, routed.summary, routed.feature_type,
                                             routed.basis_version};
            const auto snapshot = task->QueryRoutedSummarySnapshot(query, BaselineSerializationFormat::kJson);
            for (int j = 0; j < 2; ++j) {
                const auto p = task->PredictRoutedSummary(query, t + 1 + j);
                const auto& f = output.forecasts[i * 2 + j];
                assert(f.values.status == p.status && f.values.target_bucket == t + 1 + j);
                assert(f.input.metric_id == routed.metric && f.summary_id == routed.summary &&
                       f.basis_id == row.basis_id);
                if (p.status == BaselineStatus::kOk) {
                    assert(f.values.expected == p.baseline_mu && f.values.lower == p.baseline_lower &&
                           f.values.upper == p.baseline_upper);
                } else
                    assert(!f.values.expected && !f.values.lower && !f.values.upper);
            }
            assert(snapshot == task->QueryRoutedSummarySnapshot(query, BaselineSerializationFormat::kJson));
        }
        assert(output.relation->has_fusion_result == expected.has_fusion_result);
        if (expected.has_fusion_result)
            assert(output.relation->fusion_result.relation_risk == expected.fusion_result.relation_risk);
        assert(e.QuerySeriesSnapshot("d", "dist", input.identity) ==
               task->QuerySeriesSnapshot(observation.series_key, BaselineSerializationFormat::kJson));
    }
    assert(versions.size() >= 8 && handover);
    Ok(e.Finish(), e);
    task->Close();
}
void FinalModelParameters() {
    const std::string path = "/tmp/baseline-operator-t4/model-parameters.yaml";
    std::ofstream(path) << R"(baseline:
  rolling_config:
    relation_rolling:
      basis_collect_min_buckets: 1
      basis_ready_min_buckets: 1
      basis_refresh_interval_buckets: 1
      basis_candidate_min_coverage_ratio: 0.01
      basis_min_stable_refresh_count: 1
)";
    const auto options = "config_file=" + path + ";strict=false";
    Environment environment(options.c_str());
    for (bool relation : {false, true}) {
        auto config = relation ? RelationConfig() : Config();
        EvaluationEngine engine;
        Ok(engine.Open(config, &environment.plugin, &environment.plugin, &environment.plugin), engine);
        std::vector<ModelParametersRow> rows;
        assert(engine.ExportModelParameters(&rows) != 0);  // Only normal final EOF.
        EvaluationOutput evaluated;
        for (int64_t bucket = 0; bucket < 50; ++bucket) {
            if (relation)
                Ok(engine.Submit(RelationInput(bucket), &evaluated), engine);
            else
                for (auto kind : {BaselineTaskKind::kValue, BaselineTaskKind::kRatio})
                    Ok(engine.Submit(Input(kind, bucket), &evaluated), engine);
        }
        Ok(engine.Finish(), engine);
        std::string before, after;
        // A diagnostic as well as a prediction-independent final basis remains unchanged.
        const auto input = relation ? RelationInput(49) : Input(BaselineTaskKind::kValue, 49);
        const auto snapshot = engine.QuerySeriesSnapshot("d", relation ? "dist" : "v", input.identity);
        Ok(engine.ExportModelParameters(&rows), engine);
        assert(rows.size() == (relation ? 1 : 2));
        for (const auto& row : rows) {
            assert(row.as_of_bucket == 49 && row.parameters.parameters_version == 1);
            assert(row.parameters.status == BaselineStatus::kOk && !row.parameters.parameters_json.empty());
            rapidjson::Document json;
            json.Parse(row.parameters.parameters_json.c_str());
            assert(!json.HasParseError() && json["parameters_version"].GetUint() == 1);
            assert(!json.HasMember("checkpoint") && !json.HasMember("config"));
            if (relation) {
                assert(json["basis"].IsArray() && json["basis"].Size() == 1);
                assert(json["basis"][0]["active"]["basis_version"].GetUint64() > 0);
                assert(json["routed"].IsArray() && !json["routed"].Empty());
            } else {
                assert(json["rolling"]["theta"]["level"].IsDouble());
                const double expected =
                    row.identity.kind == BaselineTaskKind::kValue ? std::log1p(100.0) : std::log(0.1 / 0.9);
                assert(std::abs(json["rolling"]["theta"]["level"].GetDouble() - expected) < 1e-10);
                assert(json["rolling"]["sigma"].GetDouble() > 0);
            }
        }
        auto first = rows.front().parameters.parameters_json;
        Ok(engine.ExportCheckpoint(&before), engine);
        Ok(engine.ExportModelParameters(&rows), engine);
        Ok(engine.ExportCheckpoint(&after), engine);
        assert(before == after);
        assert(first == rows.front().parameters.parameters_json);
        assert(snapshot == engine.QuerySeriesSnapshot("d", relation ? "dist" : "v", input.identity));
        std::shared_ptr<arrow::RecordBatch> batch;
        std::string error;
        assert(EncodeModelParameters(config, rows, config.config.read.max_pending_bytes, &batch, &error) == 0);
        assert(batch->schema()->Equals(*MakeSchema(SchemaKind::kModelParameters), true));
        assert(batch->num_rows() == int64_t(rows.size()));
        assert(EncodeModelParameters(config, rows, 1, &batch, &error) != 0);
    }
    auto config = Config();
    EvaluationEngine cold;
    Ok(cold.Open(config, &environment.plugin, &environment.plugin), cold);
    EvaluationOutput out;
    Ok(cold.Submit(Input(BaselineTaskKind::kValue, 0), &out), cold);
    Ok(cold.Finish(), cold);
    std::vector<ModelParametersRow> rows;
    Ok(cold.ExportModelParameters(&rows), cold);
    assert(rows.size() == 1 && rows[0].parameters.status == BaselineStatus::kNotTrained);
    assert(rows[0].parameters.parameters_json.empty());
    cold.Close();
    EvaluationEngine empty;
    Ok(empty.Open(config, &environment.plugin, &environment.plugin), empty);
    Ok(empty.Finish(), empty);
    Ok(empty.ExportModelParameters(&rows), empty);
    assert(rows.empty());
    std::shared_ptr<arrow::RecordBatch> batch;
    std::string error;
    assert(EncodeModelParameters(config, rows, 1024, &batch, &error) == 0 && batch->num_rows() == 0);
    empty.Close();
    config.config.persistence.max_checkpoint_bytes = 16;
    EvaluationEngine budget;
    Ok(budget.Open(config, &environment.plugin, &environment.plugin), budget);
    for (int64_t bucket = 0; bucket < 5; ++bucket)
        Ok(budget.Submit(Input(BaselineTaskKind::kValue, bucket), &out), budget);
    Ok(budget.Finish(), budget);
    assert(budget.ExportModelParameters(&rows) != 0);
}
void HistoryAndForecastBudgets() {
    Environment environment;
    auto config = Config();
    config.config.read.max_pending_bytes = 1;
    config.config.bootstrap = {true, 0, 10, 10, false};
    EvaluationEngine history;
    Ok(history.Open(config, &environment.plugin, &environment.plugin), history);
    EvaluationOutput output;
    const auto input = Input(BaselineTaskKind::kValue, 0);
    const auto before = history.QuerySeriesSnapshot("d", "v", input.identity);
    assert(history.Submit(input, &output) != 0 && output.results.empty());
    assert(before == history.QuerySeriesSnapshot("d", "v", input.identity));
    history.Close();
    config = Config();
    config.config.read.max_pending_bytes = 4096;
    config.config.horizon_buckets = kMaxForecastBuckets;
    EvaluationEngine forecast;
    Ok(forecast.Open(config, &environment.plugin, &environment.plugin), forecast);
    const auto untrained = forecast.QuerySeriesSnapshot("d", "v", input.identity);
    assert(forecast.Submit(input, &output) != 0 && output.forecasts.empty());
    assert(untrained == forecast.QuerySeriesSnapshot("d", "v", input.identity));
}
}  // namespace
int main() {
    FinalModelParameters();
    ColdAndDirect();
    History();
    BoundsAndLowSupport();
    RelationParity();
    ForecastReadOnly();
    RatioForecastAndFailedPrediction();
    SampledAndDownweight();
    RelationVersionsAndForecast();
    HistoryAndForecastBudgets();
    std::cout << "PASS scalar evaluation, history separation, NULL cold start and direct parity\n";
}
