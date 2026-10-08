// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <map>
#include <string>
#include <thread>
#include <type_traits>

#include <common/error_code.h>
#include <framework/interfaces/ibaseline_service.h>
#include <framework/interfaces/ibaseline_state_control.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <common/loader.hpp>

#include "plugins/baseline/config/runtime_config.h"

using namespace flowsql;

namespace {

struct LoadedBaselineService {
    PluginLoader* loader = nullptr;
    IBaselineService* service = nullptr;

    ~LoadedBaselineService() {
        if (!loader) return;
        loader->StopAll();
        loader->Unload();
    }
};

LoadedBaselineService LoadBaselineService(const std::string& option = "") {
    LoadedBaselineService env;
    env.loader = PluginLoader::Single();

    std::string plugin_dir = get_absolute_process_path();
    std::string plugin_name = "libflowsql_baseline.so";
    const char* relapath[] = {plugin_name.c_str()};
    const char* option_value = option.empty() ? nullptr : option.c_str();
    const char* options[] = {option_value};

    const int ret = env.loader->Load(plugin_dir.c_str(), relapath, options, 1);
    assert(ret == 0);
    assert(env.loader->StartAll() == 0);

    env.service = static_cast<IBaselineService*>(env.loader->First(IID_BASELINE_SERVICE));
    assert(env.service != nullptr);
    return env;
}

const char* ValueTaskConfig() {
    return R"({
        "schema_version": 1,
        "task_id": "baseline_task_bps",
        "task_name": "link bps baseline",
        "task_kind": "value",
        "feature_id": "bps",
        "feature_type": "value_basic",
        "profile": "default",
        "clock_spec": {
            "bucket_seconds": 60,
            "timezone": "Asia/Shanghai"
        },
        "calendar_ref": {
            "calendar_id": "cn-holiday",
            "calendar_version": "2026.1"
        }
    })";
}

const char* SampledValueTaskConfig() {
    return R"({
        "schema_version": 1,
        "task_id": "baseline_task_sampled_bps",
        "task_name": "sampled link bps baseline",
        "task_kind": "value",
        "feature_id": "sampled_bps",
        "feature_type": "value_sampled",
        "profile": "cont_core",
        "clock_spec": {
            "bucket_seconds": 60,
            "timezone": "Asia/Shanghai"
        },
        "calendar_ref": {
            "calendar_id": "cn-holiday",
            "calendar_version": "2026.1"
        }
    })";
}

const char* OtherValueTaskConfig() {
    return R"({
        "schema_version": 1,
        "task_id": "baseline_task_other_bps",
        "task_name": "other link bps baseline",
        "task_kind": "value",
        "feature_id": "other_bps",
        "feature_type": "value_basic",
        "profile": "default",
        "clock_spec": {
            "bucket_seconds": 60,
            "timezone": "Asia/Shanghai"
        },
        "calendar_ref": {
            "calendar_id": "cn-holiday",
            "calendar_version": "2026.1"
        }
    })";
}

const char* LegacyValueTaskConfig() {
    return R"({
        "schema_version": 1,
        "task_id": "baseline_task_legacy_value",
        "task_name": "legacy value baseline",
        "task_kind": "value",
        "feature_id": "legacy_bps",
        "feature_type": "value",
        "profile": "default",
        "clock_spec": {
            "bucket_seconds": 60,
            "timezone": "Asia/Shanghai"
        },
        "calendar_ref": {
            "calendar_id": "cn-holiday",
            "calendar_version": "2026.1"
        }
    })";
}

const char* BasicValueWithSampledProfileConfig() {
    return R"({
        "schema_version": 1,
        "task_id": "baseline_task_basic_with_sampled_profile",
        "task_name": "invalid basic sampled profile",
        "task_kind": "value",
        "feature_id": "basic_bps",
        "feature_type": "value_basic",
        "profile": "cont_core",
        "clock_spec": {
            "bucket_seconds": 60,
            "timezone": "Asia/Shanghai"
        },
        "calendar_ref": {
            "calendar_id": "cn-holiday",
            "calendar_version": "2026.1"
        }
    })";
}

const char* SampledValueWithDefaultProfileConfig() {
    return R"({
        "schema_version": 1,
        "task_id": "baseline_task_sampled_with_default_profile",
        "task_name": "invalid sampled default profile",
        "task_kind": "value",
        "feature_id": "sampled_bps",
        "feature_type": "value_sampled",
        "profile": "default",
        "clock_spec": {
            "bucket_seconds": 60,
            "timezone": "Asia/Shanghai"
        },
        "calendar_ref": {
            "calendar_id": "cn-holiday",
            "calendar_version": "2026.1"
        }
    })";
}

const char* RatioTaskConfig() {
    return R"({
        "schema_version": 1,
        "task_id": "baseline_task_success_rate",
        "task_name": "success rate baseline",
        "task_kind": "ratio",
        "feature_id": "success_rate",
        "feature_type": "ratio",
        "profile": "rate_core",
        "clock_spec": {
            "bucket_seconds": 60,
            "timezone": "Asia/Shanghai"
        },
        "calendar_ref": {
            "calendar_id": "cn-holiday",
            "calendar_version": "2026.1"
        }
    })";
}

const char* RelationTaskConfig() {
    return R"({
        "schema_version": 1,
        "task_id": "baseline_task_client_mix",
        "task_name": "client mix basis",
        "task_kind": "relation",
        "feature_id": "client_mix",
        "feature_type": "relation",
        "profile": "default",
        "group_space_id": "client_group",
        "group_space_version": "v1",
        "metrics": ["bps"],
        "support_policy": {
            "k_support": 2,
            "min_hist_share": 0.01,
            "min_active_ratio": 0.1
        },
        "summary_policy": {
            "k_head": 2,
            "k_stable": 1
        },
        "clock_spec": {
            "bucket_seconds": 60,
            "timezone": "Asia/Shanghai"
        },
        "calendar_ref": {
            "calendar_id": "cn-holiday",
            "calendar_version": "2026.1"
        }
    })";
}

const char* MultiMetricRelationTaskConfig() {
    return R"({
        "schema_version": 1,
        "task_id": "baseline_task_multi_metric_client_mix",
        "task_name": "multi metric client mix basis",
        "task_kind": "relation",
        "feature_id": "client_mix_multi",
        "feature_type": "relation",
        "profile": "default",
        "group_space_id": "client_group",
        "group_space_version": "v1",
        "metrics": ["bps", "pps"],
        "support_policy": {
            "k_support": 2,
            "min_hist_share": 0.01,
            "min_active_ratio": 0.1
        },
        "summary_policy": {
            "k_head": 2,
            "k_stable": 1
        },
        "clock_spec": {
            "bucket_seconds": 60,
            "timezone": "Asia/Shanghai"
        },
        "calendar_ref": {
            "calendar_id": "cn-holiday",
            "calendar_version": "2026.1"
        }
    })";
}

void TestEventCalendarSchemaRejectsTaskScopedFields() {
    std::printf("[TEST] B1 calendar schema rejects task-scoped fields...\n");

    const std::string config_path = "/tmp/flowsql_baseline_calendar_schema_test.yaml";
    {
        std::ofstream file(config_path);
        file << R"(
calendars:
  - calendar_id: "cn-holiday"
    calendar_version: "2026.1"
    entries:
      - event_code: "promo"
        scope_type: "feature"
        feature: "bps"
        alignment_mode: "absolute_utc"
        start_ts: 1200
        end_ts: 1500
baseline:
  parser:
    tz_default: "UTC"
  shared_profile_config:
    daily_harmonic_order: 2
    weekly_harmonic_order: 1
    dme_max: 7
    m_month_enable: 4
    month_cov_min: 0.8
    lambda_season: 1.0
    lambda_dom: 4.0
    lambda_dme: 2.0
    lambda_lwd: 1.0
    lambda_event: 0.1
  value_sampled_profiles:
    cont_core:
      n_train_min: 50
      transform_name_override: "log1p"
  ratio_profiles:
    global:
      eps_logit: 1.0e-4
      m_floor: 1.0e-4
      v_floor: 0.25
    rate_core:
      d_min_train: 50
      s_prior: 2.0
      phi_over: 1.5
  solver_constants:
    solver_name: "weighted_huber_ridge_irls"
    c_huber: 1.5
    s_min_fit: 1.0e-3
    max_iter_fit: 15
    tol_obj_rel: 1.0e-4
    tol_beta_inf: 1.0e-5
    cond_max: 1.0e8
)";
    }

    std::string err;
    const int rc = flowsql::baseline::LoadBaselineRuntimeConfigFromYaml(config_path, true, &err);
    assert(rc == error::BAD_REQUEST);
    assert(err.find("not allowed") != std::string::npos || err.find("feature") != std::string::npos);
    flowsql::baseline::ResetBaselineRuntimeConfig();

    std::printf("[PASS] B1 calendar schema rejects task-scoped fields\n");
}

void AssertSnapshotHasTask(const std::string& json,
                           const char* expected_task_id,
                           uint64_t expected_count) {
    rapidjson::Document doc;
    doc.Parse(json.c_str());
    assert(!doc.HasParseError());
    assert(doc.IsObject());
    assert(doc.HasMember("task_count"));
    assert(doc["task_count"].GetUint64() == expected_count);
    assert(doc.HasMember("tasks"));
    assert(doc["tasks"].IsArray());

    bool found = false;
    for (const auto& item : doc["tasks"].GetArray()) {
        assert(item.IsObject());
        assert(item.HasMember("task_id"));
        if (std::string(item["task_id"].GetString()) == expected_task_id) {
            found = true;
            assert(item.HasMember("kind"));
            assert(std::string(item["kind"].GetString()) == "value");
        }
    }
    assert(found == (expected_count > 0));
}

void TestCreateTaskUsesConfigIdentity() {
    std::printf("[TEST] B1 task config identity...\n");
    auto env = LoadBaselineService();

    auto [status, task] = env.service->CreateValueTask(
        ValueTaskConfig(), BaselineSerializationFormat::kJson);
    assert(status == BaselineStatus::kOk);
    assert(task != nullptr);
    assert(std::string(task->Id()) == "baseline_task_bps");
    assert(std::string(task->Name()) == "link bps baseline");

    auto [snapshot_status, snapshot] =
        env.service->QueryServiceSnapshot(BaselineSerializationFormat::kJson);
    assert(snapshot_status == BaselineStatus::kOk);
    AssertSnapshotHasTask(snapshot, "baseline_task_bps", 1);

    auto [dup_status, dup_task] = env.service->CreateValueTask(
        ValueTaskConfig(), BaselineSerializationFormat::kJson);
    assert(dup_status == BaselineStatus::kInvalidArgument);
    assert(dup_task == nullptr);

    assert(task->Close() == BaselineStatus::kOk);
    auto [closed_snapshot_status, closed_snapshot] =
        env.service->QueryServiceSnapshot(BaselineSerializationFormat::kJson);
    assert(closed_snapshot_status == BaselineStatus::kOk);
    AssertSnapshotHasTask(closed_snapshot, "baseline_task_bps", 0);

    std::printf("[PASS] B1 task config identity\n");
}

void TestInvalidTaskConfigRejected() {
    std::printf("[TEST] B1 invalid task config rejected...\n");
    auto env = LoadBaselineService();

    auto [status, task] = env.service->CreateValueTask(
        R"({"schema_version":1})", BaselineSerializationFormat::kJson);
    assert(status == BaselineStatus::kParseFailed);
    assert(task == nullptr);

    auto [legacy_status, legacy_task] = env.service->CreateValueTask(
        LegacyValueTaskConfig(), BaselineSerializationFormat::kJson);
    assert(legacy_status == BaselineStatus::kParseFailed);
    assert(legacy_task == nullptr);

    auto [basic_sampled_status, basic_sampled_task] = env.service->CreateValueTask(
        BasicValueWithSampledProfileConfig(), BaselineSerializationFormat::kJson);
    assert(basic_sampled_status == BaselineStatus::kParseFailed);
    assert(basic_sampled_task == nullptr);

    auto [sampled_default_status, sampled_default_task] = env.service->CreateValueTask(
        SampledValueWithDefaultProfileConfig(), BaselineSerializationFormat::kJson);
    assert(sampled_default_status == BaselineStatus::kParseFailed);
    assert(sampled_default_task == nullptr);

    std::printf("[PASS] B1 invalid task config rejected\n");
}

void TestStateControlBindingContract() {
    auto env = LoadBaselineService();
    auto* management =
        static_cast<IBaselineStateControlServiceV1*>(env.loader->First(IID_BASELINE_STATE_CONTROL_SERVICE_V1));
    assert(management != nullptr);
    const BaselineStateLimitsV1 limits{1, 2, 2};
    assert(management->Bind(nullptr, limits).first == BaselineStatus::kInvalidArgument);
    auto created = env.service->CreateValueTask(ValueTaskConfig(), BaselineSerializationFormat::kJson);
    assert(created.first == BaselineStatus::kOk);
    auto value = created.second;
    assert(value->PredictRolling("missing", 1).status == BaselineStatus::kNotTrained);
    assert(management->Bind(value, {0, 2, 2}).first == BaselineStatus::kInvalidArgument);
    assert(management->Bind(value, {1, 0, 2}).first == BaselineStatus::kInvalidArgument);
    assert(management->Bind(value, {1, 2, 0}).first == BaselineStatus::kInvalidArgument);
    auto bound = management->Bind(value, limits);
    assert(bound.first == BaselineStatus::kOk && bound.second);
    assert(management->Bind(value, limits).first == BaselineStatus::kInvalidArgument);
    auto control = bound.second;
    auto usage = control->QueryUsage();
    assert(usage.first == BaselineStatus::kOk && usage.second.runtime_identities == 0);
    assert(usage.second.model_identities == 0 && usage.second.routed_states == 0);
    assert(control->ReleaseIdentity("", BaselineStateReleaseScopeV1::kRuntimeOnly) == BaselineStatus::kInvalidArgument);
    assert(control->ReleaseIdentity("missing", static_cast<BaselineStateReleaseScopeV1>(99)) ==
           BaselineStatus::kInvalidArgument);
    assert(control->ReleaseIdentity("missing", BaselineStateReleaseScopeV1::kAllState) == BaselineStatus::kOk);
    auto used = env.service->CreateValueTask(OtherValueTaskConfig(), BaselineSerializationFormat::kJson);
    assert(used.first == BaselineStatus::kOk);
    assert(used.second->SubmitObservation({"", 1, 1.0, 1}, {}).status == BaselineStatus::kInvalidArgument);
    assert(management->Bind(used.second, limits).first == BaselineStatus::kInvalidArgument);
    assert(value->Close() == BaselineStatus::kOk);
    assert(control->QueryUsage().first == BaselineStatus::kInvalidArgument);
    assert(control->ReleaseIdentity("missing", BaselineStateReleaseScopeV1::kRuntimeOnly) ==
           BaselineStatus::kInvalidArgument);
    std::printf("[PASS] optional state control binding contract\n");
}

ValueBootstrapInput BuildValueHistoryForSeries(const std::string& series_key,
                                               double base_value) {
    ValueBootstrapInput input;
    input.series_key = series_key;
    for (int64_t bucket = 0; bucket < 200; ++bucket) {
        input.observations.push_back(
            ValueBootstrapPoint{bucket, base_value + static_cast<double>(bucket % 10), 1});
    }
    return input;
}

ValueBootstrapInput BuildValueHistory() {
    return BuildValueHistoryForSeries("svc-a", 100.0);
}

ValueBootstrapInput BuildEventValueHistory() {
    ValueBootstrapInput input;
    input.series_key = "svc-event";
    for (int64_t bucket = 0; bucket < 200; ++bucket) {
        const bool event_bucket =
            (bucket >= 20 && bucket < 25) ||
            (bucket >= 80 && bucket < 85) ||
            (bucket >= 140 && bucket < 145);
        const double value = 100.0 + static_cast<double>(bucket % 3) +
                             (event_bucket ? 400.0 : 0.0);
        input.observations.push_back(ValueBootstrapPoint{bucket, value, 1});
    }
    return input;
}

RatioBootstrapInput BuildRatioHistory() {
    RatioBootstrapInput input;
    input.series_key = "svc-a";
    for (int64_t bucket = 0; bucket < 200; ++bucket) {
        input.observations.push_back(
            RatioBootstrapPoint{bucket, 95.0 + static_cast<double>(bucket % 3), 100.0});
    }
    return input;
}

ValueBootstrapInput BuildSampledValueHistory() {
    ValueBootstrapInput input = BuildValueHistory();
    for (auto& point : input.observations) {
        point.sample_count = 50;
    }
    return input;
}

RelationBootstrapInput BuildRelationHistory() {
    RelationBootstrapInput input;
    input.series_key = "svc-a";
    for (int64_t bucket = 0; bucket < 10; ++bucket) {
        RelationBootstrapBlock block;
        block.bucket_id = bucket;
        block.group_idx = {1, 2, 3};
        RelationBootstrapMetric metric;
        metric.metric = "bps";
        metric.total = 100.0;
        metric.values_by_group = {60.0, 30.0, 10.0};
        block.metrics.push_back(metric);
        input.blocks.push_back(block);
    }
    return input;
}

RelationRollingObservation BuildRelationObservation(int64_t bucket,
                                                    double g1,
                                                    double g2,
                                                    double g3) {
    RelationRollingObservation obs;
    obs.series_key = "svc-a";
    obs.bucket_id = bucket;
    obs.group_idx = {1, 2, 3};
    RelationBootstrapMetric metric;
    metric.metric = "bps";
    metric.total = g1 + g2 + g3;
    metric.active_count = 3;
    metric.values_by_group = {g1, g2, g3};
    obs.metrics.push_back(metric);
    return obs;
}

RelationRollingObservation BuildMismatchedRelationMetricOrderObservation() {
    RelationRollingObservation obs;
    obs.series_key = "svc-multi";
    obs.bucket_id = 100;
    obs.group_idx = {1, 2, 3};

    RelationBootstrapMetric pps;
    pps.metric = "pps";
    pps.total = 100.0;
    pps.active_count = 3;
    pps.values_by_group = {60.0, 30.0, 10.0};
    obs.metrics.push_back(pps);

    RelationBootstrapMetric bps;
    bps.metric = "bps";
    bps.total = 100.0;
    bps.active_count = 3;
    bps.values_by_group = {50.0, 30.0, 20.0};
    obs.metrics.push_back(bps);
    return obs;
}

template <typename Task, typename MakeHistory, typename MakeObservation>
void CheckManagedTaskLifecycle(Task* task, IBaselineTaskStateControlV1* control, MakeHistory make_history,
                               MakeObservation make_observation, int64_t train_end) {
    const auto json = BaselineSerializationFormat::kJson;
    const auto runtime = BaselineStateReleaseScopeV1::kRuntimeOnly;
    const auto all = BaselineStateReleaseScopeV1::kAllState;
    auto usage = [&](uint64_t online, uint64_t models) {
        auto current = control->QueryUsage();
        assert(current.first == BaselineStatus::kOk);
        assert(current.second.runtime_identities == online && current.second.model_identities == models);
    };
    assert(task->Bootstrap(make_history("a")).status == BaselineStatus::kOk);
    usage(1, 1);
    const auto artifact_a = task->ExportBootstrapArtifact(json);
    assert(artifact_a.first == BaselineStatus::kOk);
    assert(task->SubmitObservation(make_observation("a", 250), {}).status == BaselineStatus::kOk);
    assert(control->ReleaseIdentity("a", runtime) == BaselineStatus::kOk);
    assert(control->ReleaseIdentity("a", runtime) == BaselineStatus::kOk);
    usage(0, 1);
    assert(task->QuerySeriesSnapshot("a", json).first == BaselineStatus::kNotTrained);
    assert(task->ExportBootstrapArtifact(json) == artifact_a);
    assert(task->SubmitObservation(make_observation("a", train_end), {}).status == BaselineStatus::kInvalidArgument);
    usage(0, 1);
    auto invalid = make_observation("a", train_end + 1);
    using Observation = decltype(invalid);
    if constexpr (std::is_same_v<Observation, RelationRollingObservation>) {
        invalid.metrics[0].values_by_group[0] = -1;
    } else if constexpr (std::is_same_v<Observation, ValueRollingObservation>) {
        invalid.value = std::numeric_limits<double>::quiet_NaN();
    } else {
        invalid.numerator = std::numeric_limits<double>::quiet_NaN();
    }
    assert(task->SubmitObservation(invalid, {}).status == BaselineStatus::kInvalidArgument);
    usage(0, 1);
    assert(task->QuerySeriesSnapshot("a", json).first == BaselineStatus::kNotTrained);
    if constexpr (!std::is_same_v<Task, IBaselineRelationTask>) {
        assert(task->PredictBootstrap("a", 250, {}).status == BaselineStatus::kOk);
    }
    assert(task->Bootstrap(make_history("b")).status == BaselineStatus::kOk);
    usage(1, 2);
    assert(task->QuerySeriesSnapshot("a", json).first == BaselineStatus::kNotTrained);
    auto replace_b = make_history("b");
    replace_b.options.force_replace_existing_artifact = true;
    assert(task->Bootstrap(replace_b).status == BaselineStatus::kOk);
    usage(1, 2);
    assert(task->QuerySeriesSnapshot("a", json).first == BaselineStatus::kNotTrained);
    const auto artifact_ab = task->ExportBootstrapArtifact(json);
    const auto seed_ab = task->ExportBootstrapSeed(json);
    const auto before = task->QueryTaskSnapshot(json);
    const auto b_before = task->QuerySeriesSnapshot("b", json);
    assert(task->Bootstrap(make_history("c")).status == BaselineStatus::kInvalidArgument);
    assert(task->LoadBootstrapArtifact(artifact_ab.second, json) == BaselineStatus::kInvalidArgument);
    assert(task->LoadBootstrapArtifact("{", json) != BaselineStatus::kOk);
    assert(task->SubmitObservation(make_observation("c", 250), {}).status == BaselineStatus::kInvalidArgument);
    usage(1, 2);
    assert(task->ExportBootstrapArtifact(json) == artifact_ab && task->ExportBootstrapSeed(json) == seed_ab);
    assert(task->QueryTaskSnapshot(json) == before && task->QuerySeriesSnapshot("b", json) == b_before);
    assert(control->ReleaseIdentity("b", runtime) == BaselineStatus::kOk);
    usage(0, 2);
    // Model capacity is independent of the now-empty runtime capacity.
    assert(task->Bootstrap(make_history("c")).status == BaselineStatus::kInvalidArgument);
    usage(0, 2);
    assert(task->ExportBootstrapArtifact(json) == artifact_ab && task->ExportBootstrapSeed(json) == seed_ab);
    // Known identities continue at capacity; releasing the other parent keeps this source unchanged.
    assert(task->SubmitObservation(make_observation("b", 250), {}).status == BaselineStatus::kOk);
    const auto b_after = task->QuerySeriesSnapshot("b", json);
    assert(control->ReleaseIdentity("a", runtime) == BaselineStatus::kOk);
    assert(task->QuerySeriesSnapshot("b", json) == b_after);
    assert(control->ReleaseIdentity("b", all) == BaselineStatus::kOk);
    usage(0, 1);
    // The same previously consumed online bucket starts a new lifecycle after RuntimeOnly.
    assert(task->SubmitObservation(make_observation("a", 250), {}).status == BaselineStatus::kOk);
    usage(1, 1);
    assert(task->SubmitObservation(make_observation("a", 250), {}).status == BaselineStatus::kInvalidArgument);
    assert(control->ReleaseIdentity("a", all) == BaselineStatus::kOk);
    usage(0, 0);
    assert(task->ExportBootstrapArtifact(json).first == BaselineStatus::kNotTrained);
    if constexpr (!std::is_same_v<Task, IBaselineRelationTask>) {
        assert(task->PredictBootstrap("a", 250, {}).status == BaselineStatus::kNotTrained);
    }
    assert(task->SubmitObservation(make_observation("a", 1), {}).status == BaselineStatus::kOk);
    usage(1, 0);
    assert(task->SubmitObservation(make_observation("b", 1), {}).status == BaselineStatus::kInvalidArgument);
    // A successful import replaces runtime and indexes rather than merging old identities.
    assert(task->LoadBootstrapArtifact(artifact_a.second, json) == BaselineStatus::kOk);
    usage(1, 1);
    assert(control->ReleaseIdentity("a", all) == BaselineStatus::kOk);
    assert(task->SubmitObservation(make_observation("b", 1), {}).status == BaselineStatus::kOk);
    usage(1, 0);
}

void TestManagedStateLifecycleAndCapacity() {
    auto env = LoadBaselineService();
    auto* management =
        static_cast<IBaselineStateControlServiceV1*>(env.loader->First(IID_BASELINE_STATE_CONTROL_SERVICE_V1));
    const BaselineStateLimitsV1 limits{1, 2, 2};
    auto value = env.service->CreateValueTask(ValueTaskConfig(), BaselineSerializationFormat::kJson).second;
    auto ratio = env.service->CreateRatioTask(RatioTaskConfig(), BaselineSerializationFormat::kJson).second;
    auto relation = env.service->CreateRelationTask(RelationTaskConfig(), BaselineSerializationFormat::kJson).second;
    assert(value && ratio && relation);
    assert(management->Bind(relation, {1, 2, 1}).first == BaselineStatus::kInvalidArgument);
    auto vcontrol = management->Bind(value, limits).second;
    auto rcontrol = management->Bind(ratio, limits).second;
    auto relcontrol = management->Bind(relation, limits).second;
    assert(vcontrol && rcontrol && relcontrol);
    CheckManagedTaskLifecycle(
        value.get(), vcontrol.get(), [](const std::string& key) { return BuildValueHistoryForSeries(key, 100); },
        [](const std::string& key, int64_t bucket) { return ValueRollingObservation{key, bucket, 105, 1}; }, 199);
    CheckManagedTaskLifecycle(
        ratio.get(), rcontrol.get(),
        [](const std::string& key) {
            auto h = BuildRatioHistory();
            h.series_key = key;
            return h;
        },
        [](const std::string& key, int64_t bucket) { return RatioRollingObservation{key, bucket, 96, 100}; }, 199);
    CheckManagedTaskLifecycle(
        relation.get(), relcontrol.get(),
        [](const std::string& key) {
            auto h = BuildRelationHistory();
            h.series_key = key;
            return h;
        },
        [](const std::string& key, int64_t bucket) {
            auto obs = BuildRelationObservation(bucket, 60, 30, 10);
            obs.series_key = key;
            return obs;
        },
        9);
    const auto restored = relation->SubmitObservation(
        [] {
            auto obs = BuildRelationObservation(2, 60, 30, 10);
            obs.series_key = "b";
            return obs;
        }(),
        {});
    assert(restored.status == BaselineStatus::kOk && !restored.routed_results.empty());
    assert(relcontrol->ReleaseIdentity(restored.routed_results.front().routed_series_key,
                                       BaselineStateReleaseScopeV1::kAllState) == BaselineStatus::kInvalidArgument);
    assert(relcontrol->ReleaseIdentity("b", BaselineStateReleaseScopeV1::kAllState) == BaselineStatus::kOk);
    const auto empty = relcontrol->QueryUsage().second;
    assert(empty.runtime_identities == 0 && empty.model_identities == 0 && empty.routed_states == 0 &&
           empty.retained_basis_versions == 0);
    std::printf("[PASS] managed Value/Ratio/Relation lifecycle, quotas and atomic replacement\n");
}

void TestManagedRelationVersionRetirement() {
    const std::string path = "/tmp/flowsql_test_managed_relation_versions.yaml";
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
      basis_handover_warmup_buckets: 1
      basis_threshold_margin: 1.0
      basis_min_stable_refresh_count: 1
)";
    auto env = LoadBaselineService("config_file=" + path + ";strict=false");
    auto* management =
        static_cast<IBaselineStateControlServiceV1*>(env.loader->First(IID_BASELINE_STATE_CONTROL_SERVICE_V1));
    auto task = env.service->CreateRelationTask(RelationTaskConfig(), BaselineSerializationFormat::kJson).second;
    auto control = management->Bind(task, {2, 2, 2}).second;
    assert(control);
    auto other = BuildRelationObservation(1, 60, 30, 10);
    other.series_key = "other::source";
    assert(task->SubmitObservation(other, {}).status == BaselineStatus::kOk);
    const auto other_before = task->QuerySeriesSnapshot(other.series_key, BaselineSerializationFormat::kJson);
    std::map<uint64_t, RelationRoutedSummaryQuery> versions;
    int64_t bucket = 100;
    for (uint32_t phase = 0; phase < 12; ++phase) {
        for (int j = 0; j < 3; ++j) {
            auto obs = BuildRelationObservation(bucket++, 80 * std::pow(20., phase), 20 * std::pow(20., phase), 0);
            obs.series_key = "changing::source";
            obs.group_idx = {1 + phase * 3, 2 + phase * 3, 3 + phase * 3};
            const auto result = task->SubmitObservation(obs, {});
            assert(result.status == BaselineStatus::kOk);
            for (const auto& child : result.routed_results) {
                if (child.summary == "out_of_support_share")
                    versions[child.basis_version] = {obs.series_key, child.metric, child.summary, child.feature_type,
                                                     child.basis_version};
            }
            auto usage = control->QueryUsage().second;
            assert(usage.runtime_identities == 2 && usage.retained_basis_versions <= 4);
            assert(usage.routed_states <= 48);
            if (result.handover_active) {
                for (const auto& child : result.routed_results) {
                    if (!child.basis_scoped) continue;
                    RelationRoutedSummaryQuery query{obs.series_key, child.metric, child.summary, child.feature_type,
                                                     child.basis_version};
                    assert(task->QueryRoutedSummarySnapshot(query, BaselineSerializationFormat::kJson).first ==
                           BaselineStatus::kOk);
                }
            }
        }
    }
    assert(versions.size() >= 8);
    uint64_t retained = 0;
    for (const auto& version : versions) {
        const auto query_status =
            task->QueryRoutedSummarySnapshot(version.second, BaselineSerializationFormat::kJson).first;
        const auto predict_status = task->PredictRoutedSummary(version.second, bucket + 1).status;
        assert(query_status == BaselineStatus::kNotTrained || query_status == BaselineStatus::kOk);
        assert(predict_status == query_status);
        retained += query_status == BaselineStatus::kOk;
    }
    assert(retained > 0 && retained <= 2);
    assert(task->PredictRoutedSummary(versions.begin()->second, bucket + 1).status == BaselineStatus::kNotTrained);
    assert(task->QuerySeriesSnapshot(other.series_key, BaselineSerializationFormat::kJson) == other_before);
    assert(control->ReleaseIdentity("changing::source", BaselineStateReleaseScopeV1::kAllState) == BaselineStatus::kOk);
    assert(control->QueryUsage().second.runtime_identities == 1);
    assert(task->QuerySeriesSnapshot(other.series_key, BaselineSerializationFormat::kJson) == other_before);
    assert(task->Close() == BaselineStatus::kOk);
    control.reset();
    // The same refresh sequence remains fully queryable for an unbound legacy task.
    task = env.service->CreateRelationTask(RelationTaskConfig(), BaselineSerializationFormat::kJson).second;
    assert(task);
    versions.clear();
    bucket = 100;
    for (uint32_t phase = 0; phase < 12; ++phase) {
        for (int j = 0; j < 3; ++j) {
            auto obs = BuildRelationObservation(bucket++, 80 * std::pow(20., phase), 20 * std::pow(20., phase), 0);
            obs.series_key = "changing::source";
            obs.group_idx = {1 + phase * 3, 2 + phase * 3, 3 + phase * 3};
            const auto result = task->SubmitObservation(obs, {});
            assert(result.status == BaselineStatus::kOk);
            for (const auto& child : result.routed_results) {
                if (child.summary == "out_of_support_share")
                    versions[child.basis_version] = {obs.series_key, child.metric, child.summary, child.feature_type,
                                                     child.basis_version};
            }
        }
    }
    assert(versions.size() >= 8);
    for (const auto& version : versions) {
        assert(task->QueryRoutedSummarySnapshot(version.second, BaselineSerializationFormat::kJson).first ==
               BaselineStatus::kOk);
        assert(task->PredictRoutedSummary(version.second, bucket + 1).status == BaselineStatus::kOk);
    }
    std::printf("[PASS] managed Relation bounds basis history and isolates source retirement/release\n");
}

void TestManagedRelationImportVersionLimit() {
    auto env = LoadBaselineService();
    auto* management =
        static_cast<IBaselineStateControlServiceV1*>(env.loader->First(IID_BASELINE_STATE_CONTROL_SERVICE_V1));
    auto task = env.service->CreateRelationTask(RelationTaskConfig(), BaselineSerializationFormat::kJson).second;
    auto control = management->Bind(task, {2, 1, 2}).second;
    assert(control && task->Bootstrap(BuildRelationHistory()).status == BaselineStatus::kOk);
    const auto json = BaselineSerializationFormat::kJson;
    const auto artifact = task->ExportBootstrapArtifact(json);
    const auto seed = task->ExportBootstrapSeed(json);
    const auto before = task->QueryTaskSnapshot(json);
    const auto source_before = task->QuerySeriesSnapshot("svc-a", json);
    rapidjson::Document document;
    document.Parse(artifact.second.c_str());
    auto& series = document["series_artifacts"];
    auto& routed = series[0]["relation_routed_summary_artifacts"];
    rapidjson::Value child;
    for (const auto& item : routed.GetArray()) {
        if (std::string(item["summary"].GetString()) == "out_of_support_share") {
            child.CopyFrom(item, document.GetAllocator());
            break;
        }
    }
    assert(child.IsObject());
    for (uint64_t version = 2; version <= 3; ++version) {
        rapidjson::Value extra;
        extra.CopyFrom(child, document.GetAllocator());
        extra["basis_version"].SetUint64(version);
        routed.PushBack(extra, document.GetAllocator());
    }
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    document.Accept(writer);
    const std::string oversized = buffer.GetString();
    assert(task->LoadBootstrapArtifact(oversized, json) == BaselineStatus::kInvalidArgument);
    assert(task->ExportBootstrapArtifact(json) == artifact && task->ExportBootstrapSeed(json) == seed);
    assert(task->QueryTaskSnapshot(json) == before && task->QuerySeriesSnapshot("svc-a", json) == source_before);
    assert(control->QueryUsage().second.retained_basis_versions <= 2);
    document.Parse(artifact.second.c_str());
    auto& oversized_children = document["series_artifacts"][0]["relation_routed_summary_artifacts"];
    for (uint32_t i = 0; i < 16; ++i) {
        rapidjson::Value extra;
        extra.CopyFrom(child, document.GetAllocator());
        const std::string summary = "stable_g_share_" + std::to_string(100 + i);
        extra["summary"].SetString(summary.c_str(), document.GetAllocator());
        const std::string task_id = "baseline_task_client_mix::bps::" + summary;
        const std::string feature_id = "client_mix.bps." + summary;
        extra["task_identity"]["task_id"].SetString(task_id.c_str(), document.GetAllocator());
        extra["task_identity"]["feature_id"].SetString(feature_id.c_str(), document.GetAllocator());
        oversized_children.PushBack(extra, document.GetAllocator());
    }
    buffer.Clear();
    rapidjson::Writer<rapidjson::StringBuffer> fanout_writer(buffer);
    document.Accept(fanout_writer);
    const std::string oversized_fanout = buffer.GetString();
    assert(task->LoadBootstrapArtifact(oversized_fanout, json) == BaselineStatus::kInvalidArgument);
    assert(task->ExportBootstrapArtifact(json) == artifact && task->ExportBootstrapSeed(json) == seed);
    assert(task->QueryTaskSnapshot(json) == before && task->QuerySeriesSnapshot("svc-a", json) == source_before);
    // Runtime capacity can accommodate two sources, while the frozen model limit remains one.
    auto history = BuildRelationHistory();
    history.series_key = "second";
    assert(task->Bootstrap(history).status == BaselineStatus::kInvalidArgument);
    assert(task->Close() == BaselineStatus::kOk);
    control.reset();
    task = env.service->CreateRelationTask(RelationTaskConfig(), json).second;
    assert(task->LoadBootstrapArtifact(oversized, json) == BaselineStatus::kOk);
    assert(task->LoadBootstrapArtifact(oversized_fanout, json) == BaselineStatus::kOk);
    std::printf(
        "[PASS] managed Relation rejects oversized imported history before committing; legacy import preserved\n");
}

void TestTaskBootstrapPredictAndExport() {
    std::printf("[TEST] B1 task bootstrap predict export...\n");
    auto env = LoadBaselineService();

    auto [value_status, value_task] = env.service->CreateValueTask(
        ValueTaskConfig(), BaselineSerializationFormat::kJson);
    assert(value_status == BaselineStatus::kOk);
    assert(value_task != nullptr);
    const BootstrapTrainResult value_train = value_task->Bootstrap(BuildValueHistory());
    assert(value_train.status == BaselineStatus::kOk);
    const BootstrapPrediction value_prediction =
        value_task->PredictBootstrap("svc-a", 220, BootstrapPredictionOptions{});
    assert(value_prediction.status == BaselineStatus::kOk);
    const BootstrapPredictionSequence value_sequence =
        value_task->PredictBootstrap("svc-a", 220, 3, BootstrapPredictionOptions{});
    assert(value_sequence.status == BaselineStatus::kOk);
    assert(value_sequence.series_key == "svc-a");
    assert(value_sequence.start_bucket_id == 220);
    assert(value_sequence.point_count == 3);
    assert(value_sequence.predictions.size() == 3);
    assert(value_sequence.predictions[0].bucket_id == 220);
    assert(value_sequence.predictions[1].bucket_id == 221);
    assert(value_sequence.predictions[2].bucket_id == 222);
    assert(value_sequence.predictions[0].status == BaselineStatus::kOk);
    assert(value_sequence.predictions[1].status == BaselineStatus::kOk);
    assert(value_sequence.predictions[2].status == BaselineStatus::kOk);
    auto [value_artifact_status, value_artifact] =
        value_task->ExportBootstrapArtifact(BaselineSerializationFormat::kJson);
    assert(value_artifact_status == BaselineStatus::kOk);
    assert(value_artifact.find("\"document_kind\":\"bootstrap_artifact\"") != std::string::npos);
    auto [value_seed_status, value_seed] =
        value_task->ExportBootstrapSeed(BaselineSerializationFormat::kJson);
    assert(value_seed_status == BaselineStatus::kOk);
    assert(value_seed.find("\"document_kind\":\"bootstrap_seed\"") != std::string::npos);
    assert(value_seed.find("\"algorithm_version\":\"b1-bootstrap-v1\"") != std::string::npos);
    assert(value_seed.find("\"feature_type\":\"value_basic\"") != std::string::npos);
    assert(value_seed.find("\"calendar_ref\"") != std::string::npos);
    assert(value_seed.find("\"calendar_id\":\"cn-holiday\"") != std::string::npos);
    assert(value_seed.find("\"calendar_version\":\"2026.1\"") != std::string::npos);
    assert(value_seed.find("\"clock_spec\"") != std::string::npos);
    assert(value_seed.find("\"bucket_seconds\":60") != std::string::npos);
    assert(value_seed.find("\"timezone\":\"Asia/Shanghai\"") != std::string::npos);
    assert(value_seed.find("\"seeded_components\"") != std::string::npos);
    assert(value_seed.find("\"enabled_components\"") != std::string::npos);
    assert(value_seed.find("\"level\"") != std::string::npos);
    assert(value_seed.find("\"trend\"") != std::string::npos);
    assert(value_seed.find("\"daily\"") != std::string::npos);
    assert(value_seed.find("\"weekly\"") != std::string::npos);
    assert(value_seed.find("\"core\"") == std::string::npos);
    assert(value_seed.find("\"theta_init\"") != std::string::npos);
    assert(value_seed.find("\"sigma_init\"") != std::string::npos);
    assert(value_seed.find("\"uncertainty_init\"") != std::string::npos);
    assert(value_seed.find("\"component_uncertainty\"") != std::string::npos);
    assert(value_seed.find("\"maturity_init\"") != std::string::npos);
    assert(value_seed.find("\"model\"") == std::string::npos);

    auto [sampled_value_status, sampled_value_task] = env.service->CreateValueTask(
        SampledValueTaskConfig(), BaselineSerializationFormat::kJson);
    assert(sampled_value_status == BaselineStatus::kOk);
    assert(sampled_value_task != nullptr);
    const BootstrapTrainResult sampled_value_train =
        sampled_value_task->Bootstrap(BuildSampledValueHistory());
    assert(sampled_value_train.status == BaselineStatus::kOk);

    auto [ratio_status, ratio_task] = env.service->CreateRatioTask(
        RatioTaskConfig(), BaselineSerializationFormat::kJson);
    assert(ratio_status == BaselineStatus::kOk);
    assert(ratio_task != nullptr);
    const BootstrapTrainResult ratio_train = ratio_task->Bootstrap(BuildRatioHistory());
    assert(ratio_train.status == BaselineStatus::kOk);
    const BootstrapPrediction ratio_prediction =
        ratio_task->PredictBootstrap("svc-a", 220, BootstrapPredictionOptions{});
    assert(ratio_prediction.status == BaselineStatus::kOk);
    assert(ratio_prediction.baseline_mu >= 0.0);
    assert(ratio_prediction.baseline_mu <= 1.0);
    const BootstrapPredictionSequence ratio_sequence =
        ratio_task->PredictBootstrap("svc-a", 220, 2, BootstrapPredictionOptions{});
    assert(ratio_sequence.status == BaselineStatus::kOk);
    assert(ratio_sequence.predictions.size() == 2);
    assert(ratio_sequence.predictions[0].bucket_id == 220);
    assert(ratio_sequence.predictions[1].bucket_id == 221);
    assert(ratio_sequence.predictions[0].baseline_mu >= 0.0);
    assert(ratio_sequence.predictions[0].baseline_mu <= 1.0);
    auto [ratio_seed_status, ratio_seed] =
        ratio_task->ExportBootstrapSeed(BaselineSerializationFormat::kJson);
    assert(ratio_seed_status == BaselineStatus::kOk);
    assert(ratio_seed.find("\"feature_type\":\"ratio\"") != std::string::npos);
    assert(ratio_seed.find("\"calendar_ref\"") != std::string::npos);
    assert(ratio_seed.find("\"clock_spec\"") != std::string::npos);
    assert(ratio_seed.find("\"seeded_components\"") != std::string::npos);
    assert(ratio_seed.find("\"enabled_components\"") != std::string::npos);
    assert(ratio_seed.find("\"theta_init\"") != std::string::npos);
    assert(ratio_seed.find("\"sigma_init\"") != std::string::npos);
    assert(ratio_seed.find("\"uncertainty_init\"") != std::string::npos);
    assert(ratio_seed.find("\"component_uncertainty\"") != std::string::npos);
    assert(ratio_seed.find("\"maturity_init\"") != std::string::npos);
    assert(ratio_seed.find("\"ratio_prior_init\"") != std::string::npos);
    assert(ratio_seed.find("\"model\"") == std::string::npos);

    auto [relation_status, relation_task] = env.service->CreateRelationTask(
        RelationTaskConfig(), BaselineSerializationFormat::kJson);
    assert(relation_status == BaselineStatus::kOk);
    assert(relation_task != nullptr);
    const BootstrapTrainResult relation_train =
        relation_task->Bootstrap(BuildRelationHistory());
    assert(relation_train.status == BaselineStatus::kOk);
    auto [relation_snapshot_status, relation_snapshot] =
        relation_task->QueryTaskSnapshot(BaselineSerializationFormat::kJson);
    assert(relation_snapshot_status == BaselineStatus::kOk);
    rapidjson::Document relation_snapshot_doc;
    relation_snapshot_doc.Parse(relation_snapshot.c_str());
    assert(!relation_snapshot_doc.HasParseError());
    assert(relation_snapshot_doc.HasMember("document_kind"));
    assert(std::string(relation_snapshot_doc["document_kind"].GetString()) ==
           "relation_task_snapshot");
    assert(relation_snapshot_doc.HasMember("relation_runtime"));
    const auto& relation_runtime = relation_snapshot_doc["relation_runtime"];
    assert(relation_runtime["routed_shard_count"].GetUint64() == 16);
    assert(relation_runtime["source_state_count"].GetUint64() == 1);
    assert(relation_runtime["routed_seed_count"].GetUint64() > 0);
    auto [basis_status, basis_json] =
        relation_task->QueryBootstrapBasis(BaselineSerializationFormat::kJson);
    assert(basis_status == BaselineStatus::kOk);
    assert(basis_json.find("\"support_explicit\"") != std::string::npos);
    auto [relation_seed_status, relation_seed] =
        relation_task->ExportBootstrapSeed(BaselineSerializationFormat::kJson);
    assert(relation_seed_status == BaselineStatus::kOk);
    assert(relation_seed.find("\"feature_type\":\"relation\"") != std::string::npos);
    assert(relation_seed.find("\"calendar_ref\"") != std::string::npos);
    assert(relation_seed.find("\"feature_type\":\"value_basic\"") != std::string::npos);
    assert(relation_seed.find("\"feature_type\":\"ratio\"") != std::string::npos);
    assert(relation_seed.find("\"clock_spec\"") != std::string::npos);
    assert(relation_seed.find("\"seeded_components\"") != std::string::npos);
    assert(relation_seed.find("\"enabled_components\"") != std::string::npos);
    assert(relation_seed.find("\"relation_basis\"") != std::string::npos);
    assert(relation_seed.find("\"relation_routed_summary_seeds\"") != std::string::npos);
    assert(relation_seed.find("\"relation_basis_by_metric\"") != std::string::npos);
    assert(relation_seed.find("\"summary\":\"entropy_shannon\"") != std::string::npos);
    assert(relation_seed.find("\"summary\":\"top1_share\"") != std::string::npos);
    assert(relation_seed.find("\"theta_init\"") != std::string::npos);
    assert(relation_seed.find("\"sigma_init\"") != std::string::npos);
    assert(relation_seed.find("\"uncertainty_init\"") != std::string::npos);
    assert(relation_seed.find("\"component_uncertainty\"") != std::string::npos);
    assert(relation_seed.find("\"maturity_init\"") != std::string::npos);
    assert(relation_seed.find("\"ratio_prior_init\"") != std::string::npos);
    assert(relation_seed.find("\"model\"") == std::string::npos);

    std::printf("[PASS] B1 task bootstrap predict export\n");
}

void TestValueTaskKeepsBootstrapPerSeriesAndExportsAll() {
    std::printf("[TEST] B1 value task keeps bootstrap per series and exports all...\n");

    auto env = LoadBaselineService();
    auto [value_status, value_task] = env.service->CreateValueTask(
        ValueTaskConfig(), BaselineSerializationFormat::kJson);
    assert(value_status == BaselineStatus::kOk);
    assert(value_task != nullptr);

    const BootstrapTrainResult train_a =
        value_task->Bootstrap(BuildValueHistoryForSeries("svc-a", 100.0));
    assert(train_a.status == BaselineStatus::kOk);
    ValueBootstrapInput no_replace_input = BuildValueHistoryForSeries("svc-a", 120.0);
    no_replace_input.options.force_replace_existing_artifact = false;
    const BootstrapTrainResult no_replace_result =
        value_task->Bootstrap(no_replace_input);
    assert(no_replace_result.status == BaselineStatus::kInvalidArgument);
    const BootstrapTrainResult train_b =
        value_task->Bootstrap(BuildValueHistoryForSeries("svc-b", 500.0));
    assert(train_b.status == BaselineStatus::kOk);

    const BootstrapPrediction prediction_a =
        value_task->PredictBootstrap("svc-a", 220, BootstrapPredictionOptions{});
    const BootstrapPrediction prediction_b =
        value_task->PredictBootstrap("svc-b", 220, BootstrapPredictionOptions{});
    assert(prediction_a.status == BaselineStatus::kOk);
    assert(prediction_b.status == BaselineStatus::kOk);
    assert(prediction_a.series_key == "svc-a");
    assert(prediction_b.series_key == "svc-b");
    assert(prediction_b.baseline_mu > prediction_a.baseline_mu + 300.0);

    auto [artifact_status, artifact_json] =
        value_task->ExportBootstrapArtifact(BaselineSerializationFormat::kJson);
    assert(artifact_status == BaselineStatus::kOk);
    assert(artifact_json.find("\"series_artifacts\"") != std::string::npos);
    assert(artifact_json.find("\"series_key\":\"svc-a\"") != std::string::npos);
    assert(artifact_json.find("\"series_key\":\"svc-b\"") != std::string::npos);

    auto [seed_status, seed_json] =
        value_task->ExportBootstrapSeed(BaselineSerializationFormat::kJson);
    assert(seed_status == BaselineStatus::kOk);
    assert(seed_json.find("\"series_seeds\"") != std::string::npos);
    assert(seed_json.find("\"series_key\":\"svc-a\"") != std::string::npos);
    assert(seed_json.find("\"series_key\":\"svc-b\"") != std::string::npos);

    auto [reload_status, reload_task] = env.service->CreateValueTask(
        R"({
            "schema_version": 1,
            "task_id": "baseline_task_bps_reload",
            "task_name": "link bps baseline reload",
            "task_kind": "value",
            "feature_id": "bps",
            "feature_type": "value_basic",
            "profile": "default",
            "clock_spec": {
                "bucket_seconds": 60,
                "timezone": "Asia/Shanghai"
            },
            "calendar_ref": {
                "calendar_id": "cn-holiday",
                "calendar_version": "2026.1"
            }
        })",
        BaselineSerializationFormat::kJson);
    assert(reload_status == BaselineStatus::kOk);
    assert(reload_task != nullptr);
    assert(reload_task->LoadBootstrapArtifact(
               artifact_json, BaselineSerializationFormat::kJson) ==
           BaselineStatus::kIncompatibleArtifact);

    assert(value_task->Close() == BaselineStatus::kOk);
    auto [restored_status, restored_task] = env.service->CreateValueTask(
        ValueTaskConfig(), BaselineSerializationFormat::kJson);
    assert(restored_status == BaselineStatus::kOk);
    assert(restored_task != nullptr);
    assert(restored_task->LoadBootstrapArtifact(
               artifact_json, BaselineSerializationFormat::kJson) == BaselineStatus::kOk);
    const BootstrapPrediction reloaded_prediction_a =
        restored_task->PredictBootstrap("svc-a", 220, BootstrapPredictionOptions{});
    const BootstrapPrediction reloaded_prediction_b =
        restored_task->PredictBootstrap("svc-b", 220, BootstrapPredictionOptions{});
    assert(reloaded_prediction_a.status == BaselineStatus::kOk);
    assert(reloaded_prediction_b.status == BaselineStatus::kOk);
    assert(reloaded_prediction_b.baseline_mu > reloaded_prediction_a.baseline_mu + 300.0);

    std::printf("[PASS] B1 value task keeps bootstrap per series and exports all\n");
}

void TestB4RelationSubmitObservation() {
    std::printf("[TEST] B4 relation submit observation routes summaries...\n");

    auto env = LoadBaselineService();
    auto [status, relation_task] = env.service->CreateRelationTask(
        RelationTaskConfig(), BaselineSerializationFormat::kJson);
    assert(status == BaselineStatus::kOk);
    assert(relation_task != nullptr);

    RelationRollingSubmitOptions options;
    RelationRollingResult cold_result =
        relation_task->SubmitObservation(BuildRelationObservation(100, 60, 30, 10), options);
    assert(cold_result.status == BaselineStatus::kOk);
    assert(cold_result.series_key == "svc-a");
    assert(cold_result.bucket_id == 100);
    assert(cold_result.basis_status == "collecting");

    bool has_entropy = false;
    bool has_top1 = false;
    bool has_basis_scoped = false;
    std::string top1_routed_series_key;
    for (const auto& routed : cold_result.routed_results) {
        if (routed.summary == "entropy_shannon") {
            has_entropy = true;
            assert(!routed.basis_scoped);
            assert(routed.rolling.status == BaselineStatus::kOk);
        }
        if (routed.summary == "top1_share") {
            has_top1 = true;
            assert(!routed.basis_scoped);
            assert(routed.rolling.status == BaselineStatus::kOk);
            top1_routed_series_key = routed.routed_series_key;
        }
        if (routed.basis_scoped) has_basis_scoped = true;
    }
    assert(has_entropy);
    assert(has_top1);
    assert(!has_basis_scoped);
    assert(!top1_routed_series_key.empty());

    auto [snapshot_status, snapshot] =
        relation_task->QueryTaskSnapshot(BaselineSerializationFormat::kJson);
    assert(snapshot_status == BaselineStatus::kOk);
    rapidjson::Document snapshot_doc;
    snapshot_doc.Parse(snapshot.c_str());
    assert(!snapshot_doc.HasParseError());
    assert(snapshot_doc["relation_runtime"]["source_state_count"].GetUint64() == 1);
    assert(snapshot_doc["relation_runtime"]["routed_state_count"].GetUint64() > 0);

    auto [source_snapshot_status, source_snapshot] =
        relation_task->QuerySeriesSnapshot("svc-a", BaselineSerializationFormat::kJson);
    assert(source_snapshot_status == BaselineStatus::kOk);
    rapidjson::Document source_snapshot_doc;
    source_snapshot_doc.Parse(source_snapshot.c_str());
    assert(!source_snapshot_doc.HasParseError());
    assert(std::string(source_snapshot_doc["document_kind"].GetString()) ==
           "relation_series_snapshot");
    assert(source_snapshot_doc["series_key"].GetString() == std::string("svc-a"));
    assert(source_snapshot_doc["source_series_key"].GetString() == std::string("svc-a"));
    assert(source_snapshot_doc.HasMember("basis_by_metric"));
    assert(source_snapshot_doc["basis_by_metric"].IsArray());
    assert(source_snapshot_doc.HasMember("routed_summaries"));
    assert(source_snapshot_doc["routed_summaries"].IsArray());
    assert(source_snapshot_doc["routed_summaries"].Size() > 0);

    auto [wrong_source_status, wrong_source_snapshot] =
        relation_task->QuerySeriesSnapshot(top1_routed_series_key,
                                           BaselineSerializationFormat::kJson);
    assert(wrong_source_status == BaselineStatus::kInvalidArgument);
    assert(wrong_source_snapshot.empty());

    RelationRoutedSummaryQuery routed_query;
    routed_query.source_series_key = "svc-a";
    routed_query.metric = "bps";
    routed_query.summary = "top1_share";
    routed_query.feature_type = "ratio";
    RollingPrediction routed_prediction =
        relation_task->PredictRoutedSummary(routed_query, 101);
    assert(routed_prediction.status == BaselineStatus::kOk);

    auto [routed_snapshot_status, routed_snapshot] =
        relation_task->QueryRoutedSummarySnapshot(routed_query,
                                                  BaselineSerializationFormat::kJson);
    assert(routed_snapshot_status == BaselineStatus::kOk);
    rapidjson::Document routed_snapshot_doc;
    routed_snapshot_doc.Parse(routed_snapshot.c_str());
    assert(!routed_snapshot_doc.HasParseError());
    assert(std::string(routed_snapshot_doc["document_kind"].GetString()) ==
           "rolling_series_snapshot");
    assert(routed_snapshot_doc["last_seen_bucket"].GetInt64() == 100);

    auto [multi_status, multi_task] = env.service->CreateRelationTask(
        MultiMetricRelationTaskConfig(), BaselineSerializationFormat::kJson);
    assert(multi_status == BaselineStatus::kOk);
    assert(multi_task != nullptr);
    RelationRollingSubmitOptions mismatch_options;
    mismatch_options.include_diagnostics = true;
    RelationRollingResult mismatch_result = multi_task->SubmitObservation(
        BuildMismatchedRelationMetricOrderObservation(), mismatch_options);
    assert(mismatch_result.status == BaselineStatus::kInvalidArgument);
    assert(mismatch_result.routed_results.empty());

    std::string seeded_config = RelationTaskConfig();
    const std::string old_task_id = "baseline_task_client_mix";
    const std::size_t task_id_pos = seeded_config.find(old_task_id);
    assert(task_id_pos != std::string::npos);
    seeded_config.replace(task_id_pos, old_task_id.size(), "baseline_task_client_mix_seeded");
    auto [seeded_status, seeded_task] = env.service->CreateRelationTask(
        seeded_config, BaselineSerializationFormat::kJson);
    assert(seeded_status == BaselineStatus::kOk);
    assert(seeded_task != nullptr);
    assert(seeded_task->Bootstrap(BuildRelationHistory()).status == BaselineStatus::kOk);
    RelationRollingResult seeded_result =
        seeded_task->SubmitObservation(BuildRelationObservation(20, 50, 25, 25), options);
    assert(seeded_result.status == BaselineStatus::kOk);

    bool has_out_of_support = false;
    for (const auto& routed : seeded_result.routed_results) {
        if (routed.summary == "out_of_support_share") {
            has_out_of_support = true;
            assert(routed.basis_scoped);
            assert(routed.basis_version > 0);
            assert(routed.rolling.status == BaselineStatus::kOk);
        }
    }
    assert(has_out_of_support);

    std::printf("[PASS] B4 relation submit observation routes summaries\n");
}

void TestB4RelationRollingConfigSwitches() {
    std::printf("[TEST] B4 relation rolling config switches close design contract...\n");

    const std::string routed_disabled_config = "/tmp/flowsql_b4_relation_routed_disabled.yaml";
    {
        std::ofstream file(routed_disabled_config);
        file << R"(
baseline:
  rolling_config:
    relation_rolling:
      enable_routed_rolling: false
      enable_stream_basis: true
      include_universal_summaries_without_basis: true
      basis_collect_min_buckets: 1
      basis_ready_min_buckets: 1
      basis_refresh_interval_buckets: 1
      basis_candidate_min_coverage_ratio: 0.01
      basis_min_stable_refresh_count: 1
      routed_state_shard_count: 4
)";
        assert(file.good());
    }
    {
        auto env = LoadBaselineService("config_file=" + routed_disabled_config + ";strict=false");
        auto [status, relation_task] = env.service->CreateRelationTask(
            RelationTaskConfig(), BaselineSerializationFormat::kJson);
        assert(status == BaselineStatus::kOk);
        assert(relation_task != nullptr);

        RelationRollingSubmitOptions options;
        const auto before = relation_task->QueryTaskSnapshot(BaselineSerializationFormat::kJson);
        auto invalid = BuildRelationObservation(100, 60, 30, 10);
        invalid.metrics[0].values_by_group[1] = -1.0;
        assert(relation_task->SubmitObservation(invalid, options).status == BaselineStatus::kInvalidArgument);
        assert(relation_task->QueryTaskSnapshot(BaselineSerializationFormat::kJson) == before);
        RelationRollingResult result =
            relation_task->SubmitObservation(BuildRelationObservation(100, 60, 30, 10), options);
        assert(result.status == BaselineStatus::kOk);
        assert(result.routed_results.empty());
        assert(result.basis_version == 1);

        auto [task_snapshot_status, task_snapshot] =
            relation_task->QueryTaskSnapshot(BaselineSerializationFormat::kJson);
        assert(task_snapshot_status == BaselineStatus::kOk);
        rapidjson::Document task_doc;
        task_doc.Parse(task_snapshot.c_str());
        assert(!task_doc.HasParseError());
        assert(task_doc["relation_runtime"]["routed_shard_count"].GetUint64() == 4);
        assert(task_doc["relation_runtime"]["routed_state_count"].GetUint64() == 0);

        auto [source_status, source_snapshot] =
            relation_task->QuerySeriesSnapshot("svc-a", BaselineSerializationFormat::kJson);
        assert(source_status == BaselineStatus::kOk);
        rapidjson::Document source_doc;
        source_doc.Parse(source_snapshot.c_str());
        assert(!source_doc.HasParseError());
        assert(source_doc["basis_by_metric"].IsArray());
        assert(source_doc["basis_by_metric"].Size() == 1);
        assert(source_doc["routed_summaries"].IsArray());
        assert(source_doc["routed_summaries"].Empty());
    }
    flowsql::baseline::ResetBaselineRuntimeConfig();

    const std::string stream_disabled_config = "/tmp/flowsql_b4_relation_stream_disabled.yaml";
    {
        auto env = LoadBaselineService("config_file=" + routed_disabled_config + ";strict=false");
        auto [status, task] = env.service->CreateRelationTask(RelationTaskConfig(), BaselineSerializationFormat::kJson);
        assert(status == BaselineStatus::kOk);
        RelationRollingSubmitOptions options;
        options.allow_basis_update = false;
        const auto before = task->QueryTaskSnapshot(BaselineSerializationFormat::kJson);
        auto obs = BuildRelationObservation(100, 60, 30, 10);
        obs.metrics[0].values_by_group[1] = -1.0;
        assert(task->SubmitObservation(obs, options).status == BaselineStatus::kInvalidArgument);
        assert(task->QueryTaskSnapshot(BaselineSerializationFormat::kJson) == before);
        obs.metrics[0].values_by_group[1] = 30.0;
        assert(task->SubmitObservation(obs, options).status == BaselineStatus::kOk);
    }
    flowsql::baseline::ResetBaselineRuntimeConfig();
    {
        std::ofstream file(stream_disabled_config);
        file << R"(
baseline:
  rolling_config:
    relation_rolling:
      enable_routed_rolling: true
      enable_stream_basis: false
      include_universal_summaries_without_basis: true
      routed_state_shard_count: 4
)";
        assert(file.good());
    }
    {
        auto env = LoadBaselineService("config_file=" + stream_disabled_config + ";strict=false");
        auto [status, relation_task] = env.service->CreateRelationTask(
            RelationTaskConfig(), BaselineSerializationFormat::kJson);
        assert(status == BaselineStatus::kOk);
        assert(relation_task != nullptr);

        RelationRollingSubmitOptions options;
        RelationRollingResult result =
            relation_task->SubmitObservation(BuildRelationObservation(100, 60, 30, 10), options);
        assert(result.status == BaselineStatus::kOk);
        assert(!result.routed_results.empty());

        auto [source_status, source_snapshot] =
            relation_task->QuerySeriesSnapshot("svc-a", BaselineSerializationFormat::kJson);
        assert(source_status == BaselineStatus::kOk);
        rapidjson::Document source_doc;
        source_doc.Parse(source_snapshot.c_str());
        assert(!source_doc.HasParseError());
        assert(source_doc["basis_by_metric"].IsArray());
        assert(source_doc["basis_by_metric"].Empty());
        assert(source_doc["routed_summaries"].IsArray());
        assert(source_doc["routed_summaries"].Size() > 0);
    }
    flowsql::baseline::ResetBaselineRuntimeConfig();

    const std::string universal_disabled_config =
        "/tmp/flowsql_b4_relation_universal_disabled.yaml";
    {
        std::ofstream file(universal_disabled_config);
        file << R"(
baseline:
  rolling_config:
    relation_rolling:
      enable_routed_rolling: true
      enable_stream_basis: false
      include_universal_summaries_without_basis: false
      routed_state_shard_count: 4
)";
        assert(file.good());
    }
    {
        auto env = LoadBaselineService("config_file=" + universal_disabled_config + ";strict=false");
        auto [status, relation_task] = env.service->CreateRelationTask(
            RelationTaskConfig(), BaselineSerializationFormat::kJson);
        assert(status == BaselineStatus::kOk);
        assert(relation_task != nullptr);

        RelationRollingSubmitOptions options;
        RelationRollingResult result =
            relation_task->SubmitObservation(BuildRelationObservation(100, 60, 30, 10), options);
        assert(result.status == BaselineStatus::kOk);
        assert(result.routed_results.empty());

        auto [source_status, source_snapshot] =
            relation_task->QuerySeriesSnapshot("svc-a", BaselineSerializationFormat::kJson);
        assert(source_status == BaselineStatus::kNotTrained);
        assert(source_snapshot.empty());
    }
    flowsql::baseline::ResetBaselineRuntimeConfig();

    std::printf("[PASS] B4 relation rolling config switches close design contract\n");
}

void TestB5RelationFusionSubmitAndSnapshot() {
    std::printf("[TEST] B5 relation fusion submit and snapshot...\n");

    {
        auto env = LoadBaselineService();
        auto [status, relation_task] = env.service->CreateRelationTask(
            RelationTaskConfig(), BaselineSerializationFormat::kJson);
        assert(status == BaselineStatus::kOk);
        assert(relation_task != nullptr);
        assert(relation_task->Bootstrap(BuildRelationHistory()).status == BaselineStatus::kOk);

        RelationRollingSubmitOptions options;
        options.include_routed_results = false;
        options.include_fusion_result = true;
        RelationRollingResult result =
            relation_task->SubmitObservation(BuildRelationObservation(20, 5, 5, 90), options);
        assert(result.status == BaselineStatus::kOk);
        assert(result.routed_results.empty());
        assert(result.has_fusion_result);
        assert(result.fusion_result.status == BaselineStatus::kOk);
        assert(result.fusion_result.source_series_key == "svc-a");
        assert(result.fusion_result.feature_base == "client_mix");
        assert(result.fusion_result.bucket_id == 20);
        assert(!result.fusion_result.dominant_single.empty());

        auto [source_status, source_snapshot] =
            relation_task->QuerySeriesSnapshot("svc-a", BaselineSerializationFormat::kJson);
        assert(source_status == BaselineStatus::kOk);
        rapidjson::Document source_doc;
        source_doc.Parse(source_snapshot.c_str());
        assert(!source_doc.HasParseError());
        assert(source_doc.HasMember("relation_fusion"));
        assert(source_doc["relation_fusion"].IsObject());
        assert(source_doc["relation_fusion"]["enabled"].GetBool());
        assert(source_doc["relation_fusion"]["bucket_id"].GetInt64() == 20);
        assert(source_doc["relation_fusion"].HasMember("dominant_single"));
        assert(source_doc["relation_fusion"]["dominant_single"].IsArray());
        assert(!source_doc["relation_fusion"]["dominant_single"].Empty());

        RelationRollingObservation missing_metric_obs;
        missing_metric_obs.series_key = "svc-a";
        missing_metric_obs.bucket_id = 21;
        missing_metric_obs.group_idx = {1, 2, 3};
        RelationRollingResult missing_metric_result =
            relation_task->SubmitObservation(missing_metric_obs, options);
        assert(!missing_metric_result.has_fusion_result);
        auto [missing_metric_status, missing_metric_snapshot] =
            relation_task->QuerySeriesSnapshot("svc-a", BaselineSerializationFormat::kJson);
        assert(missing_metric_status == BaselineStatus::kOk);
        rapidjson::Document missing_metric_doc;
        missing_metric_doc.Parse(missing_metric_snapshot.c_str());
        assert(!missing_metric_doc.HasParseError());
        assert(missing_metric_doc["relation_fusion"]["bucket_id"].GetInt64() == 21);
        assert(missing_metric_doc["relation_fusion"]["relation_risk"].GetDouble() == 0.0);
        assert(missing_metric_doc["relation_fusion"].HasMember("diagnostics"));
        assert(std::string(missing_metric_doc["relation_fusion"]["diagnostics"].GetString())
                   .find("relation_fusion_no_available_evidence") != std::string::npos);

        auto [task_snapshot_status, task_snapshot] =
            relation_task->QueryTaskSnapshot(BaselineSerializationFormat::kJson);
        assert(task_snapshot_status == BaselineStatus::kOk);
        rapidjson::Document task_doc;
        task_doc.Parse(task_snapshot.c_str());
        assert(!task_doc.HasParseError());
        assert(task_doc.HasMember("relation_fusion"));
        assert(task_doc["relation_fusion"]["enabled"].GetBool());
        assert(task_doc["relation_fusion"]["source_state_count"].GetUint64() == 1);
        assert(task_doc["relation_fusion"]["pattern_count"].GetUint() == 4);

        RelationRoutedSummaryQuery routed_query;
        routed_query.source_series_key = "svc-a";
        routed_query.metric = "bps";
        routed_query.summary = "top1_share";
        routed_query.feature_type = "ratio";
        auto [routed_status, routed_snapshot] =
            relation_task->QueryRoutedSummarySnapshot(routed_query,
                                                      BaselineSerializationFormat::kJson);
        assert(routed_status == BaselineStatus::kOk);
        rapidjson::Document routed_doc;
        routed_doc.Parse(routed_snapshot.c_str());
        assert(!routed_doc.HasParseError());
        assert(!routed_doc.HasMember("relation_fusion"));

        assert(relation_task->Bootstrap(BuildRelationHistory()).status == BaselineStatus::kOk);
        auto [rebuilt_status, rebuilt_snapshot] =
            relation_task->QuerySeriesSnapshot("svc-a", BaselineSerializationFormat::kJson);
        assert(rebuilt_status == BaselineStatus::kOk);
        rapidjson::Document rebuilt_doc;
        rebuilt_doc.Parse(rebuilt_snapshot.c_str());
        assert(!rebuilt_doc.HasParseError());
        assert(rebuilt_doc.HasMember("relation_fusion"));
        assert(rebuilt_doc["relation_fusion"]["enabled"].GetBool());
        assert(!rebuilt_doc["relation_fusion"].HasMember("bucket_id"));
    }

    const std::string fusion_disabled_config =
        "/tmp/flowsql_b5_relation_fusion_disabled.yaml";
    {
        std::ofstream file(fusion_disabled_config);
        file << R"(
baseline:
  rolling_config:
    relation_rolling:
      relation_fusion:
        enable_relation_fusion: false
)";
        assert(file.good());
    }
    {
        auto disabled_env =
            LoadBaselineService("config_file=" + fusion_disabled_config + ";strict=false");
        auto [disabled_status, disabled_task] =
            disabled_env.service->CreateRelationTask(
                RelationTaskConfig(), BaselineSerializationFormat::kJson);
        assert(disabled_status == BaselineStatus::kOk);
        assert(disabled_task != nullptr);
        assert(disabled_task->Bootstrap(BuildRelationHistory()).status == BaselineStatus::kOk);
        RelationRollingSubmitOptions disabled_options;
        disabled_options.include_fusion_result = true;
        RelationRollingResult disabled_result = disabled_task->SubmitObservation(
            BuildRelationObservation(20, 5, 5, 90), disabled_options);
        assert(disabled_result.status == BaselineStatus::kOk);
        assert(!disabled_result.has_fusion_result);
        auto [disabled_snapshot_status, disabled_snapshot] =
            disabled_task->QuerySeriesSnapshot("svc-a", BaselineSerializationFormat::kJson);
        assert(disabled_snapshot_status == BaselineStatus::kOk);
        rapidjson::Document disabled_doc;
        disabled_doc.Parse(disabled_snapshot.c_str());
        assert(!disabled_doc.HasParseError());
        assert(disabled_doc.HasMember("relation_fusion"));
        assert(!disabled_doc["relation_fusion"]["enabled"].GetBool());
        assert(!disabled_doc["relation_fusion"].HasMember("bucket_id"));
    }
    flowsql::baseline::ResetBaselineRuntimeConfig();

    std::printf("[PASS] B5 relation fusion submit and snapshot\n");
}

void TestB7RelationFusionStateCleanup() {
    std::printf("[TEST] B7 relation fusion state cleanup...\n");

    const std::string cleanup_config =
        "/tmp/flowsql_b7_relation_fusion_cleanup.yaml";
    {
        std::ofstream file(cleanup_config);
        file << R"(
baseline:
  rolling_config:
    relation_rolling:
      relation_fusion:
        fusion_state_ttl_buckets: 5
        fusion_state_max_sources: 2
        fusion_state_cleanup_interval_updates: 1
        fusion_state_cleanup_scan_limit: 2
        fusion_persistence_max_keys_per_source: 32
)";
        assert(file.good());
    }

    auto env = LoadBaselineService("config_file=" + cleanup_config + ";strict=false");
    auto [status, relation_task] = env.service->CreateRelationTask(
        RelationTaskConfig(), BaselineSerializationFormat::kJson);
    assert(status == BaselineStatus::kOk);
    assert(relation_task != nullptr);

    RelationRollingSubmitOptions options;
    options.include_routed_results = false;
    options.include_fusion_result = true;

    RelationRollingObservation old_a = BuildRelationObservation(10, 5, 5, 90);
    old_a.series_key = "svc-old-a";
    RelationRollingResult old_a_result =
        relation_task->SubmitObservation(old_a, options);
    assert(old_a_result.status == BaselineStatus::kOk);

    RelationRollingObservation old_b = BuildRelationObservation(11, 5, 5, 90);
    old_b.series_key = "svc-old-b";
    RelationRollingResult old_b_result =
        relation_task->SubmitObservation(old_b, options);
    assert(old_b_result.status == BaselineStatus::kOk);

    auto [before_status, before_snapshot] =
        relation_task->QueryTaskSnapshot(BaselineSerializationFormat::kJson);
    assert(before_status == BaselineStatus::kOk);
    rapidjson::Document before_doc;
    before_doc.Parse(before_snapshot.c_str());
    assert(!before_doc.HasParseError());
    assert(before_doc["relation_fusion"]["source_state_count"].GetUint64() == 2);

    RelationRollingObservation current = BuildRelationObservation(100, 5, 5, 90);
    current.series_key = "svc-current";
    RelationRollingResult current_result =
        relation_task->SubmitObservation(current, options);
    assert(current_result.status == BaselineStatus::kOk);

    auto [after_status, after_snapshot] =
        relation_task->QueryTaskSnapshot(BaselineSerializationFormat::kJson);
    assert(after_status == BaselineStatus::kOk);
    rapidjson::Document after_doc;
    after_doc.Parse(after_snapshot.c_str());
    assert(!after_doc.HasParseError());
    const auto& fusion = after_doc["relation_fusion"];
    assert(fusion["source_state_count"].GetUint64() <= 2);
    assert(fusion["source_state_max"].GetUint64() == 2);
    assert(fusion["state_evicted_total"].GetUint64() > 0);
    assert(fusion["state_evicted_ttl_total"].GetUint64() > 0);
    assert(fusion["cleanup_last_scan_count"].GetUint64() > 0);
    assert(fusion["cleanup_last_evicted_count"].GetUint64() > 0);
    assert(fusion["cleanup_watermark_bucket_id"].GetInt64() == 100);
    assert(fusion["cleanup_ttl_buckets"].GetUint64() == 5);
    assert(fusion["cleanup_scan_limit"].GetUint64() == 2);
    assert(fusion["persistence_key_max_per_source"].GetUint64() == 32);

    auto [current_source_status, current_source_snapshot] =
        relation_task->QuerySeriesSnapshot("svc-current", BaselineSerializationFormat::kJson);
    assert(current_source_status == BaselineStatus::kOk);
    rapidjson::Document current_source_doc;
    current_source_doc.Parse(current_source_snapshot.c_str());
    assert(!current_source_doc.HasParseError());
    assert(current_source_doc["relation_fusion"]["enabled"].GetBool());
    assert(current_source_doc["relation_fusion"]["bucket_id"].GetInt64() == 100);

    flowsql::baseline::ResetBaselineRuntimeConfig();
    std::printf("[PASS] B7 relation fusion state cleanup\n");
}

void TestB7RelationFusionStateCapacityCleanup() {
    std::printf("[TEST] B7 relation fusion capacity cleanup...\n");

    const std::string cleanup_config =
        "/tmp/flowsql_b7_relation_fusion_capacity_cleanup.yaml";
    {
        std::ofstream file(cleanup_config);
        file << R"(
baseline:
  rolling_config:
    relation_rolling:
      relation_fusion:
        fusion_state_ttl_buckets: 1000
        fusion_state_max_sources: 2
        fusion_state_cleanup_interval_updates: 1000
        fusion_state_cleanup_scan_limit: 2
        fusion_persistence_max_keys_per_source: 32
)";
        assert(file.good());
    }

    auto env = LoadBaselineService("config_file=" + cleanup_config + ";strict=false");
    auto [status, relation_task] = env.service->CreateRelationTask(
        RelationTaskConfig(), BaselineSerializationFormat::kJson);
    assert(status == BaselineStatus::kOk);
    assert(relation_task != nullptr);

    RelationRollingSubmitOptions options;
    options.include_routed_results = false;
    options.include_fusion_result = true;

    RelationRollingObservation first = BuildRelationObservation(10, 5, 5, 90);
    first.series_key = "svc-capacity-a";
    assert(relation_task->SubmitObservation(first, options).status ==
           BaselineStatus::kOk);

    RelationRollingObservation second = BuildRelationObservation(11, 5, 5, 90);
    second.series_key = "svc-capacity-b";
    assert(relation_task->SubmitObservation(second, options).status ==
           BaselineStatus::kOk);

    RelationRollingObservation third = BuildRelationObservation(12, 5, 5, 90);
    third.series_key = "svc-capacity-c";
    assert(relation_task->SubmitObservation(third, options).status ==
           BaselineStatus::kOk);

    auto [snapshot_status, snapshot_json] =
        relation_task->QueryTaskSnapshot(BaselineSerializationFormat::kJson);
    assert(snapshot_status == BaselineStatus::kOk);
    rapidjson::Document snapshot;
    snapshot.Parse(snapshot_json.c_str());
    assert(!snapshot.HasParseError());
    const auto& fusion = snapshot["relation_fusion"];
    assert(fusion["source_state_count"].GetUint64() <= 2);
    assert(fusion["source_state_max"].GetUint64() == 2);
    assert(fusion["state_evicted_total"].GetUint64() > 0);
    assert(fusion["state_evicted_capacity_total"].GetUint64() > 0);
    assert(fusion["state_evicted_ttl_total"].GetUint64() == 0);
    assert(fusion["cleanup_last_scan_count"].GetUint64() > 0);
    assert(fusion["cleanup_last_evicted_count"].GetUint64() > 0);
    assert(fusion["cleanup_watermark_bucket_id"].GetInt64() == 12);

    flowsql::baseline::ResetBaselineRuntimeConfig();
    std::printf("[PASS] B7 relation fusion capacity cleanup\n");
}

void TestBootstrapUsesConfiguredEventCalendar() {
    std::printf("[TEST] B1 bootstrap uses configured event calendar...\n");

    const std::string config_path = "/tmp/flowsql_baseline_event_calendar_test.yaml";
    {
        std::ofstream file(config_path);
        file << R"(
calendars:
  - calendar_id: "cn-holiday"
    calendar_version: "2026.1"
    entries:
      - event_code: "holiday"
        alignment_mode: "absolute_utc"
        start_ts: 1200
        end_ts: 1500
      - event_code: "holiday"
        alignment_mode: "absolute_utc"
        start_ts: 4800
        end_ts: 5100
      - event_code: "holiday"
        alignment_mode: "absolute_utc"
        start_ts: 8400
        end_ts: 8700
      - event_code: "holiday"
        alignment_mode: "absolute_utc"
        start_ts: 13200
        end_ts: 13500
baseline:
  parser:
    tz_default: "UTC"
  shared_profile_config:
    daily_harmonic_order: 2
    weekly_harmonic_order: 1
    dme_max: 7
    m_month_enable: 4
    month_cov_min: 0.8
    lambda_season: 1.0
    lambda_dom: 4.0
    lambda_dme: 2.0
    lambda_lwd: 1.0
    lambda_event: 0.1
  value_sampled_profiles:
    cont_core:
      n_train_min: 50
      transform_name_override: "log1p"
  ratio_profiles:
    global:
      eps_logit: 1.0e-4
      m_floor: 1.0e-4
      v_floor: 0.25
    rate_core:
      d_min_train: 50
      s_prior: 2.0
      phi_over: 1.5
  solver_constants:
    solver_name: "weighted_huber_ridge_irls"
    c_huber: 1.5
    s_min_fit: 1.0e-3
    max_iter_fit: 15
    tol_obj_rel: 1.0e-4
    tol_beta_inf: 1.0e-5
    cond_max: 1.0e8
)";
        assert(file.good());
    }

    auto env = LoadBaselineService("config_file=" + config_path + ";strict=true");
    auto [value_status, value_task] = env.service->CreateValueTask(
        ValueTaskConfig(), BaselineSerializationFormat::kJson);
    assert(value_status == BaselineStatus::kOk);
    assert(value_task != nullptr);

    const BootstrapTrainResult train = value_task->Bootstrap(BuildEventValueHistory());
    assert(train.status == BaselineStatus::kOk);

    auto [artifact_status, artifact_json] =
        value_task->ExportBootstrapArtifact(BaselineSerializationFormat::kJson);
    assert(artifact_status == BaselineStatus::kOk);
    assert(artifact_json.find("\"event_block\"") != std::string::npos);
    assert(artifact_json.find("\"active_event_codes\"") != std::string::npos);
    assert(artifact_json.find("\"holiday\"") != std::string::npos);

    auto [seed_status, seed_json] =
        value_task->ExportBootstrapSeed(BaselineSerializationFormat::kJson);
    assert(seed_status == BaselineStatus::kOk);
    assert(seed_json.find("\"event_hint\"") != std::string::npos);
    assert(seed_json.find("\"event\"") != std::string::npos);
    assert(seed_json.find("\"holiday\"") != std::string::npos);

    const BootstrapPrediction event_prediction =
        value_task->PredictBootstrap("svc-event", 220, BootstrapPredictionOptions{});
    const BootstrapPrediction normal_prediction =
        value_task->PredictBootstrap("svc-event", 230, BootstrapPredictionOptions{});
    assert(event_prediction.status == BaselineStatus::kOk);
    assert(normal_prediction.status == BaselineStatus::kOk);
    assert(event_prediction.baseline_mu > normal_prediction.baseline_mu + 100.0);

    std::printf("[PASS] B1 bootstrap uses configured event calendar\n");
}

void AssertSnapshotMatchesSubmit(const BaselineSerializationResult& snapshot, const RollingBaselineResult& result) {
    assert(snapshot.first == BaselineStatus::kOk && result.status == BaselineStatus::kOk);
    rapidjson::Document doc;
    doc.Parse(snapshot.second.c_str());
    assert(!doc.HasParseError() && doc["schema_version"].GetInt() == 1);
    const auto near = [](double a, double b) { assert(std::fabs(a - b) <= 1e-12 * (1.0 + std::fabs(b))); };
    near(doc["band"]["baseline_mu"].GetDouble(), result.baseline_mu);
    near(doc["band"]["baseline_lower"].GetDouble(), result.baseline_lower);
    near(doc["band"]["baseline_upper"].GetDouble(), result.baseline_upper);
    near(doc["band"]["band_width"].GetDouble(), result.band_width);
    assert(doc["control"]["can_score"].GetBool() == result.can_score);
    assert(doc["control"]["can_update"].GetBool() == result.can_update);
    assert(doc["control"]["update_weight"].GetDouble() == result.update_weight);
    assert(doc["score_trust"]["can_alert"].GetBool() == result.can_alert);
    const std::string diagnostics = doc["diagnostics"].GetString();
    assert(diagnostics.find("snapshot_state_view=current_state;") != std::string::npos);
    assert(diagnostics.find("snapshot_band_view=last_submit;") != std::string::npos);
    assert(diagnostics.find("snapshot_control_view=last_submit;") != std::string::npos);
    assert(diagnostics.find("snapshot_alert_view=last_submit;") != std::string::npos);
    assert(diagnostics.find("snapshot_bucket=" + std::to_string(result.bucket_id) + ";") != std::string::npos);
}

void AssertSnapshotWithoutSubmit(const BaselineSerializationResult& snapshot) {
    assert(snapshot.first == BaselineStatus::kOk);
    rapidjson::Document doc;
    doc.Parse(snapshot.second.c_str());
    assert(!doc.HasParseError() && doc["has_seen_observation"].GetBool());
    assert(doc["band"]["baseline_lower"].GetDouble() <= doc["band"]["baseline_mu"].GetDouble());
    assert(doc["band"]["baseline_upper"].GetDouble() >= doc["band"]["baseline_mu"].GetDouble());
    assert(!doc["control"]["can_score"].GetBool() && !doc["control"]["can_update"].GetBool());
    assert(doc["control"]["update_weight"].GetDouble() == 0.0 && !doc["score_trust"]["can_alert"].GetBool());
    const std::string diagnostics = doc["diagnostics"].GetString();
    assert(diagnostics.find("snapshot_band_view=bootstrap_parameter;") != std::string::npos);
    assert(diagnostics.find("snapshot_control_view=no_online_submit;") != std::string::npos);
    assert(diagnostics.find("snapshot_alert_view=no_online_submit;") != std::string::npos);
    assert(diagnostics.find("snapshot_bucket=") == std::string::npos);
}

void TestReviewSnapshotTracksLastSubmit() {
    std::printf("[TEST] Review snapshot tracks last successful Submit and runtime replacement...\n");
    auto env = LoadBaselineService();
    {
        auto [status, task] = env.service->CreateValueTask(ValueTaskConfig(), BaselineSerializationFormat::kJson);
        assert(status == BaselineStatus::kOk);
        ValueRollingObservation obs{"snapshot-value", 100, 100, 1};
        auto result = task->SubmitObservation(obs, {});
        assert(!result.can_score && result.can_update);
        AssertSnapshotMatchesSubmit(task->QuerySeriesSnapshot(obs.series_key, BaselineSerializationFormat::kJson),
                                    result);
        obs.bucket_id = 101;
        obs.value = 110;
        result = task->SubmitObservation(obs, {});
        assert(result.can_score && result.can_update && !result.can_alert);
        AssertSnapshotMatchesSubmit(task->QuerySeriesSnapshot(obs.series_key, BaselineSerializationFormat::kJson),
                                    result);
        const auto predicted = task->PredictRolling(obs.series_key, 102);
        assert(predicted.status == BaselineStatus::kOk);
        const double model_std =
            (std::log1p(predicted.baseline_upper) - std::log1p(predicted.baseline_mu)) / predicted.band_z;
        obs.bucket_id = 102;
        obs.value = std::expm1(std::log1p(predicted.baseline_mu) + 4.0 * model_std);
        result = task->SubmitObservation(obs, {});
        assert(result.can_update && result.update_weight > 0 && result.update_weight < 1);
        AssertSnapshotMatchesSubmit(task->QuerySeriesSnapshot(obs.series_key, BaselineSerializationFormat::kJson),
                                    result);
        obs.bucket_id = 103;
        obs.value = 1e100;
        result = task->SubmitObservation(obs, {});
        assert(result.can_score && !result.can_update);
        const auto snapshot = task->QuerySeriesSnapshot(obs.series_key, BaselineSerializationFormat::kJson);
        AssertSnapshotMatchesSubmit(snapshot, result);
        assert(task->SubmitObservation(obs, {}).status == BaselineStatus::kInvalidArgument);
        obs.bucket_id = 102;
        assert(task->SubmitObservation(obs, {}).status == BaselineStatus::kInvalidArgument);
        obs.bucket_id = 104;
        obs.value = -1;
        assert(task->SubmitObservation(obs, {}).status == BaselineStatus::kInvalidArgument);
        assert(task->PredictRolling(obs.series_key, 104).status == BaselineStatus::kOk);
        assert(task->PredictRolling(obs.series_key, 104, 4).status == BaselineStatus::kOk);
        assert(task->LoadBootstrapArtifact("{}", BaselineSerializationFormat::kJson) != BaselineStatus::kOk);
        assert(task->QuerySeriesSnapshot(obs.series_key, BaselineSerializationFormat::kJson) == snapshot);

        auto history = BuildValueHistory();
        history.series_key = "snapshot-seeded";
        assert(task->Bootstrap(history).status == BaselineStatus::kOk);
        const auto seeded = task->QuerySeriesSnapshot(history.series_key, BaselineSerializationFormat::kJson);
        AssertSnapshotWithoutSubmit(seeded);
        const auto artifact = task->ExportBootstrapArtifact(BaselineSerializationFormat::kJson);
        assert(artifact.first == BaselineStatus::kOk);
        result = task->SubmitObservation({history.series_key, 200, 100, 1}, {});
        AssertSnapshotMatchesSubmit(task->QuerySeriesSnapshot(history.series_key, BaselineSerializationFormat::kJson),
                                    result);
        history.options.force_replace_existing_artifact = true;
        assert(task->Bootstrap(history).status == BaselineStatus::kOk);
        assert(task->QuerySeriesSnapshot(history.series_key, BaselineSerializationFormat::kJson) == seeded);
        assert(task->QuerySeriesSnapshot(obs.series_key, BaselineSerializationFormat::kJson) == snapshot);
        result = task->SubmitObservation({history.series_key, 200, 100, 1}, {});
        const auto submitted = task->QuerySeriesSnapshot(history.series_key, BaselineSerializationFormat::kJson);
        AssertSnapshotMatchesSubmit(submitted, result);
        auto empty_history = history;
        empty_history.observations.clear();
        assert(task->Bootstrap(empty_history).status != BaselineStatus::kOk);
        assert(task->QuerySeriesSnapshot(history.series_key, BaselineSerializationFormat::kJson) == submitted);
        assert(task->LoadBootstrapArtifact(artifact.second, BaselineSerializationFormat::kJson) == BaselineStatus::kOk);
        assert(task->QuerySeriesSnapshot(history.series_key, BaselineSerializationFormat::kJson) == seeded);
        assert(task->QuerySeriesSnapshot(obs.series_key, BaselineSerializationFormat::kJson).first ==
               BaselineStatus::kNotTrained);
    }
    {
        auto [status, task] =
            env.service->CreateValueTask(SampledValueTaskConfig(), BaselineSerializationFormat::kJson);
        assert(status == BaselineStatus::kOk);
        ValueRollingObservation obs{"snapshot-sampled", 100, 100, 20};
        assert(task->SubmitObservation(obs, {}).status == BaselineStatus::kOk);
        for (uint64_t count : {2, 5, 20}) {
            ++obs.bucket_id;
            obs.sample_count = count;
            const auto result = task->SubmitObservation(obs, {});
            assert(result.can_score == (count >= 3) && result.can_update == (count >= 3));
            assert(result.update_weight == (count == 2 ? 0.0 : count == 5 ? 0.5 : 1.0));
            AssertSnapshotMatchesSubmit(task->QuerySeriesSnapshot(obs.series_key, BaselineSerializationFormat::kJson),
                                        result);
        }
    }
    {
        auto [status, task] = env.service->CreateRatioTask(RatioTaskConfig(), BaselineSerializationFormat::kJson);
        assert(status == BaselineStatus::kOk);
        RatioRollingObservation obs{"snapshot-ratio", 100, 50, 100};
        const auto first = task->SubmitObservation(obs, {});
        AssertSnapshotMatchesSubmit(task->QuerySeriesSnapshot(obs.series_key, BaselineSerializationFormat::kJson),
                                    first);
        for (double denominator : {5, 50, 100}) {
            ++obs.bucket_id;
            obs.denominator = denominator;
            obs.numerator = denominator / 2;
            const auto result = task->SubmitObservation(obs, {});
            assert(result.can_score == (denominator >= 10) && result.can_update == (denominator >= 10));
            assert(result.update_weight == (denominator == 5 ? 0.0 : denominator == 50 ? 0.5 : 1.0));
            AssertSnapshotMatchesSubmit(task->QuerySeriesSnapshot(obs.series_key, BaselineSerializationFormat::kJson),
                                        result);
        }
        auto history = BuildRatioHistory();
        assert(task->Bootstrap(history).status == BaselineStatus::kOk);
        AssertSnapshotWithoutSubmit(task->QuerySeriesSnapshot(history.series_key, BaselineSerializationFormat::kJson));
    }
    {
        auto [status, task] = env.service->CreateRelationTask(RelationTaskConfig(), BaselineSerializationFormat::kJson);
        assert(status == BaselineStatus::kOk);
        for (int64_t bucket : {100, 101}) {
            const auto result = task->SubmitObservation(BuildRelationObservation(bucket, 60, 30, 10), {});
            assert(result.status == BaselineStatus::kOk && result.routed_results.size() == 4);
            for (const auto& child : result.routed_results) {
                RelationRoutedSummaryQuery query{child.source_series_key, child.metric, child.summary,
                                                 child.feature_type, child.basis_version};
                AssertSnapshotMatchesSubmit(task->QueryRoutedSummarySnapshot(query, BaselineSerializationFormat::kJson),
                                            child.rolling);
            }
        }
    }
    std::printf("[PASS] Review snapshot tracks last successful Submit and runtime replacement\n");
}

void TestReviewSnapshotUsesDetectionConfiguration() {
    std::printf("[TEST] Review snapshot preserves detection configuration and extreme alert...\n");
    for (double z : {1.0, 2.0}) {
        for (double cap : {0.1, 0.5}) {
            const std::string path = "/tmp/flowsql_baseline_b11_snapshot.yaml";
            {
                std::ofstream file(path);
                file << "baseline:\n  rolling_config:\n    band_z: " << z << "\n    detection_band_std_cap: " << cap
                     << "\n    min_warming_updates: 1\n    level_ready_min_updates: 2\n"
                        "    score_warming_min_updates: 2\n    score_ready_min_updates: 3\n"
                        "    calibration_warmup_min_updates: 1\n    calibration_coverage_floor: 0.01\n"
                        "    calibration_tail3_limit: 1\n    calibration_tail5_limit: 1\n"
                        "    score_drift_degrade_start: 100\n";
                assert(file.good());
            }
            auto env = LoadBaselineService("config_file=" + path + ";strict=false");
            auto [status, task] = env.service->CreateValueTask(ValueTaskConfig(), BaselineSerializationFormat::kJson);
            assert(status == BaselineStatus::kOk);
            ValueRollingObservation obs{"snapshot-config", 100, 100, 1};
            for (; obs.bucket_id < 116; ++obs.bucket_id) {
                const auto result = task->SubmitObservation(obs, {});
                AssertSnapshotMatchesSubmit(
                    task->QuerySeriesSnapshot(obs.series_key, BaselineSerializationFormat::kJson), result);
                assert(std::fabs((result.model_upper - result.model_mu) - z * result.band_std) < 1e-12);
                if (obs.bucket_id == 101 && cap == 0.1) assert(result.band_std == cap);
            }
            obs.value = 1e100;
            const auto result = task->SubmitObservation(obs, {});
            assert(result.can_score && !result.can_update && result.can_alert);
            assert(result.maturity_status == "level_ready" || result.maturity_status == "daily_warming");
            const auto snapshot = task->QuerySeriesSnapshot(obs.series_key, BaselineSerializationFormat::kJson);
            AssertSnapshotMatchesSubmit(snapshot, result);
            baseline::ResetBaselineRuntimeConfig();
            assert(task->QuerySeriesSnapshot(obs.series_key, BaselineSerializationFormat::kJson) == snapshot);
        }
    }
    std::printf("[PASS] Review snapshot preserves detection configuration and extreme alert\n");
}

void AssertEventStateJsonNear(const rapidjson::Value& left, const rapidjson::Value& right) {
    if (left.IsNumber() && right.IsNumber()) {
        assert(std::fabs(left.GetDouble() - right.GetDouble()) <= 1e-9 * (1.0 + std::fabs(left.GetDouble())));
        return;
    }
    assert(left.GetType() == right.GetType());
    if (left.IsObject()) {
        assert(left.MemberCount() == right.MemberCount());
        for (auto it = left.MemberBegin(); it != left.MemberEnd(); ++it) {
            assert(right.HasMember(it->name.GetString()));
            AssertEventStateJsonNear(it->value, right[it->name.GetString()]);
        }
    } else if (left.IsArray()) {
        assert(left.Size() == right.Size());
        for (rapidjson::SizeType i = 0; i < left.Size(); ++i) AssertEventStateJsonNear(left[i], right[i]);
    } else {
        assert(left == right);
    }
}

void TestReviewRollingConsumesEventHint() {
    std::printf("[TEST] Review Rolling consumes event hints without learning event effects...\n");
    for (const std::string alignment : {"absolute_utc", "local_wall_clock"}) {
        const std::string path = "/tmp/baseline-b09-evaluation/rolling/public_events.yaml";
        {
            std::ofstream file(path);
            file << "calendars:\n  - calendar_id: cn-holiday\n    calendar_version: '2026.1'\n    entries:\n";
            for (int64_t bucket : {20, 80, 140, 220, 220}) {
                file << "      - event_code: holiday\n        alignment_mode: " << alignment
                     << "\n        start_ts: " << bucket * 60 << "\n        end_ts: " << (bucket + 5) * 60 << "\n";
            }
            file << "baseline:\n  shared_profile_config:\n    lambda_event: 0.1\n";
            assert(file.good());
        }
        auto env = LoadBaselineService("config_file=" + path + ";strict=false");
        auto [value_status, value] =
            env.service->CreateValueTask(ValueTaskConfig(), BaselineSerializationFormat::kJson);
        auto [ratio_status, ratio] =
            env.service->CreateRatioTask(RatioTaskConfig(), BaselineSerializationFormat::kJson);
        assert(value_status == BaselineStatus::kOk && ratio_status == BaselineStatus::kOk);
        assert(value->Bootstrap(BuildEventValueHistory()).status == BaselineStatus::kOk);
        RatioBootstrapInput ratio_history;
        ratio_history.series_key = "svc-event";
        for (int64_t bucket = 0; bucket < 200; ++bucket) {
            const bool event =
                (bucket >= 20 && bucket < 25) || (bucket >= 80 && bucket < 85) || (bucket >= 140 && bucket < 145);
            ratio_history.observations.push_back({bucket, (event ? 800.0 : 200.0) + bucket % 3, 1000.0});
        }
        assert(ratio->Bootstrap(ratio_history).status == BaselineStatus::kOk);
        const auto verify = [&](const auto& task, bool is_ratio, const auto& submit) {
            const auto to_model = [=](double observed) {
                return is_ratio ? std::log(observed / (1.0 - observed)) : std::log1p(observed);
            };
            const auto to_observed = [=](double model) {
                return is_ratio ? 1.0 / (1.0 + std::exp(-model)) : std::expm1(model);
            };
            const auto artifact = task->ExportBootstrapArtifact(BaselineSerializationFormat::kJson);
            const auto seed = task->ExportBootstrapSeed(BaselineSerializationFormat::kJson);
            assert(artifact.first == BaselineStatus::kOk && seed.first == BaselineStatus::kOk);
            rapidjson::Document seed_doc;
            seed_doc.Parse(seed.second.c_str());
            const auto& hint = seed_doc["series_seeds"][0]["event_hint"];
            assert(hint["available"].GetBool());
            const double effect = hint["coeff"][0].GetDouble();
            assert(effect > 0.1);
            const auto before = task->QuerySeriesSnapshot("svc-event", BaselineSerializationFormat::kJson);
            const auto sequence = task->PredictRolling("svc-event", 219, 12);
            assert(sequence.status == BaselineStatus::kOk && sequence.predictions.size() == 12);
            for (std::size_t i = 0; i < sequence.predictions.size(); ++i) {
                const auto point = task->PredictRolling("svc-event", 219 + static_cast<int64_t>(i));
                assert(point.status == BaselineStatus::kOk);
                assert(std::fabs(point.baseline_mu - sequence.predictions[i].baseline_mu) < 1e-9);
            }
            assert(task->QuerySeriesSnapshot("svc-event", BaselineSerializationFormat::kJson) == before);
            rapidjson::Document plain_artifact;
            plain_artifact.Parse(artifact.second.c_str());
            plain_artifact["series_artifacts"][0]["model"]["event_block"]["enabled"].SetBool(false);
            rapidjson::StringBuffer buffer;
            rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
            plain_artifact.Accept(writer);
            assert(task->LoadBootstrapArtifact(buffer.GetString(), BaselineSerializationFormat::kJson) ==
                   BaselineStatus::kOk);
            for (std::size_t i = 0; i < sequence.predictions.size(); ++i) {
                const int64_t bucket = 219 + static_cast<int64_t>(i);
                const auto plain = task->PredictRolling("svc-event", bucket);
                const double expected = bucket >= 220 && bucket < 225 ? effect : 0.0;
                assert(std::fabs(to_model(sequence.predictions[i].baseline_mu) - to_model(plain.baseline_mu) -
                                 expected) < 1e-8);
                assert(std::fabs(to_model(sequence.predictions[i].baseline_lower) - to_model(plain.baseline_lower) -
                                 expected) < 1e-8);
                assert(std::fabs(to_model(sequence.predictions[i].baseline_upper) - to_model(plain.baseline_upper) -
                                 expected) < 1e-8);
            }
            const double observed = sequence.predictions[1].baseline_mu;
            const auto plain_submit = submit(to_observed(to_model(observed) - effect));
            assert(plain_submit.status == BaselineStatus::kOk);
            const auto plain_snapshot = task->QuerySeriesSnapshot("svc-event", BaselineSerializationFormat::kJson);
            AssertSnapshotMatchesSubmit(plain_snapshot, plain_submit);
            const auto plain_future = task->PredictRolling("svc-event", 230, 4);
            assert(plain_future.status == BaselineStatus::kOk);
            assert(task->LoadBootstrapArtifact(artifact.second, BaselineSerializationFormat::kJson) ==
                   BaselineStatus::kOk);
            const auto event_submit = submit(observed);
            assert(event_submit.status == BaselineStatus::kOk && !event_submit.is_outside_band);
            assert(std::fabs(event_submit.observed - observed) < 1e-9);
            assert(std::fabs(event_submit.observed_model - to_model(observed)) < 1e-9);
            assert(std::fabs(event_submit.model_mu - plain_submit.model_mu - effect) < 1e-8);
            assert(std::fabs(event_submit.residual - plain_submit.residual) < 1e-8);
            rapidjson::Document plain_state, event_state;
            plain_state.Parse(plain_snapshot.second.c_str());
            const auto event_snapshot = task->QuerySeriesSnapshot("svc-event", BaselineSerializationFormat::kJson);
            event_state.Parse(event_snapshot.second.c_str());
            AssertSnapshotMatchesSubmit(event_snapshot, event_submit);
            const auto event_future = task->PredictRolling("svc-event", 230, 4);
            assert(event_future.status == BaselineStatus::kOk);
            for (std::size_t i = 0; i < plain_future.predictions.size(); ++i) {
                assert(std::fabs(to_model(plain_future.predictions[i].baseline_mu) -
                                 to_model(event_future.predictions[i].baseline_mu)) < 1e-8);
                assert(std::fabs(to_model(plain_future.predictions[i].baseline_lower) -
                                 to_model(event_future.predictions[i].baseline_lower)) < 1e-8);
                assert(std::fabs(to_model(plain_future.predictions[i].baseline_upper) -
                                 to_model(event_future.predictions[i].baseline_upper)) < 1e-8);
            }
            plain_state.RemoveMember("band");
            event_state.RemoveMember("band");
            AssertEventStateJsonNear(plain_state, event_state);
            assert(submit(observed).status == BaselineStatus::kInvalidArgument);
            assert(task->QuerySeriesSnapshot("svc-event", BaselineSerializationFormat::kJson) == event_snapshot);
            std::printf("[PASS] Rolling %s/%s event effect=%.9f residual=%.9f\n", is_ratio ? "ratio" : "value",
                        alignment.c_str(), effect, event_submit.residual);
        };
        verify(value, false, [&](double observed) {
            return value->SubmitObservation(ValueRollingObservation{"svc-event", 220, observed, 1}, {});
        });
        verify(ratio, true, [&](double observed) {
            return ratio->SubmitObservation(RatioRollingObservation{"svc-event", 220, observed * 1000.0, 1000.0}, {});
        });
        auto [relation_status, relation] =
            env.service->CreateRelationTask(RelationTaskConfig(), BaselineSerializationFormat::kJson);
        assert(relation_status == BaselineStatus::kOk);
        RelationBootstrapInput relation_history;
        relation_history.series_key = "svc-a";
        for (int64_t bucket = 0; bucket < 200; ++bucket) {
            const bool event =
                (bucket >= 20 && bucket < 25) || (bucket >= 80 && bucket < 85) || (bucket >= 140 && bucket < 145);
            const double total = (event ? 500.0 : 100.0) + bucket % 3;
            const double share = event ? 0.85 : 0.60;
            auto obs = BuildRelationObservation(bucket, total * share, total * (0.95 - share), total * 0.05);
            relation_history.blocks.push_back({bucket, obs.group_idx, obs.metrics});
        }
        assert(relation->Bootstrap(relation_history).status == BaselineStatus::kOk);
        const auto relation_artifact = relation->ExportBootstrapArtifact(BaselineSerializationFormat::kJson);
        const auto relation_seed = relation->ExportBootstrapSeed(BaselineSerializationFormat::kJson);
        // Relation creates routed rolling states lazily on the first online submission.
        const auto warmup = BuildRelationObservation(200, 60.6, 35.35, 5.05);
        assert(relation->SubmitObservation(warmup, {}).status == BaselineStatus::kOk);
        rapidjson::Document relation_seed_doc;
        relation_seed_doc.Parse(relation_seed.second.c_str());
        struct RoutedCheck {
            RelationRoutedSummaryQuery query;
            double effect;
            RollingPrediction prediction;
        };
        std::vector<RoutedCheck> checks;
        int value_count = 0, ratio_count = 0;
        for (const auto& seed : relation_seed_doc["series_seeds"][0]["relation_routed_summary_seeds"].GetArray()) {
            const auto& hint = seed["event_hint"];
            if (!hint["available"].GetBool()) continue;
            RelationRoutedSummaryQuery query;
            query.source_series_key = "svc-a";
            query.metric = seed["metric"].GetString();
            query.summary = seed["summary"].GetString();
            query.feature_type = std::string(seed["task_kind"].GetString()) == "ratio" ? "ratio" : "value_basic";
            query.basis_version = seed["basis_version"].GetUint64();
            if (query.feature_type == "ratio")
                ++ratio_count;
            else
                ++value_count;
            const auto prediction = relation->PredictRoutedSummary(query, 220);
            assert(prediction.status == BaselineStatus::kOk);
            checks.push_back({query, hint["coeff"][0].GetDouble(), prediction});
        }
        assert(value_count > 0 && ratio_count > 0);
        rapidjson::Document plain_relation;
        plain_relation.Parse(relation_artifact.second.c_str());
        for (auto& routed : plain_relation["series_artifacts"][0]["relation_routed_summary_artifacts"].GetArray()) {
            routed["model"]["event_block"]["enabled"].SetBool(false);
        }
        rapidjson::StringBuffer relation_buffer;
        rapidjson::Writer<rapidjson::StringBuffer> relation_writer(relation_buffer);
        plain_relation.Accept(relation_writer);
        assert(relation->LoadBootstrapArtifact(relation_buffer.GetString(), BaselineSerializationFormat::kJson) ==
               BaselineStatus::kOk);
        assert(relation->SubmitObservation(warmup, {}).status == BaselineStatus::kOk);
        for (const auto& check : checks) {
            const auto plain = relation->PredictRoutedSummary(check.query, 220);
            const auto model = [&](double observed) {
                return check.query.feature_type == "ratio" ? std::log(observed / (1.0 - observed))
                                                           : std::log1p(observed);
            };
            assert(std::fabs(model(check.prediction.baseline_mu) - model(plain.baseline_mu) - check.effect) < 1e-8);
        }
        const auto obs = BuildRelationObservation(220, 425.0, 50.0, 25.0);
        const auto plain_result = relation->SubmitObservation(obs, {});
        assert(plain_result.status == BaselineStatus::kOk);
        assert(relation->LoadBootstrapArtifact(relation_artifact.second, BaselineSerializationFormat::kJson) ==
               BaselineStatus::kOk);
        assert(relation->SubmitObservation(warmup, {}).status == BaselineStatus::kOk);
        const auto event_result = relation->SubmitObservation(obs, {});
        assert(event_result.status == BaselineStatus::kOk);
        for (const auto& check : checks) {
            const RollingBaselineResult* plain = nullptr;
            const RollingBaselineResult* event = nullptr;
            for (const auto& routed : plain_result.routed_results) {
                if (routed.summary == check.query.summary && routed.metric == check.query.metric)
                    plain = &routed.rolling;
            }
            for (const auto& routed : event_result.routed_results) {
                if (routed.summary == check.query.summary && routed.metric == check.query.metric)
                    event = &routed.rolling;
            }
            assert(plain && event && event->status == BaselineStatus::kOk);
            assert(std::fabs(event->model_mu - plain->model_mu - check.effect) < 1e-8);
            assert(std::fabs(event->residual - plain->residual + check.effect) < 1e-8);
            assert(event->observed_model == plain->observed_model);
        }
        std::printf("[PASS] Rolling relation/%s: %zu routed event models\n", alignment.c_str(), checks.size());
    }
}

void TestTaskBoundEventCalendarPreservesDstAndTaskTimezone() {
    std::printf("[TEST] Task-bound event calendar preserves DST and inherited timezone...\n");
    constexpr int64_t base = 1793491200;  // 2026-11-01 00:00 UTC, New York fall-back day.
    const std::string path = "/tmp/flowsql_baseline_bound_calendar_test.yaml";
    {
        std::ofstream file(path);
        file << "calendars:\n";
        for (bool explicit_timezone : {false, true}) {
            file << "  - calendar_id: " << (explicit_timezone ? "explicit-ny" : "inherited")
                 << "\n    calendar_version: v1\n    entries:\n";
            for (int64_t minute : {20, 80, 140, 315}) {
                file << "      - event_code: holiday\n        alignment_mode: local_wall_clock\n        start_ts: "
                     << base + minute * 60 << "\n        end_ts: " << base + (minute + (minute == 315 ? 30 : 5)) * 60
                     << "\n";
                if (explicit_timezone) file << "        tz: America/New_York\n";
            }
        }
        file << "baseline:\n  shared_profile_config:\n    lambda_event: 0.1\n";
        assert(file.good());
    }
    auto env = LoadBaselineService("config_file=" + path + ";strict=false");
    for (int mode = 0; mode < 3; ++mode) {
        const std::string timezone = mode == 0 ? "America/New_York" : "UTC";
        const std::string calendar = mode == 2 ? "explicit-ny" : "inherited";
        const std::string config =
            "{\"schema_version\":1,\"task_id\":\"bound-calendar-" + std::to_string(mode) +
            "\",\"task_name\":\"bound calendar\",\"task_kind\":\"value\",\"feature_id\":\"metric\","
            "\"feature_type\":\"value_basic\",\"profile\":\"default\",\"clock_spec\":{\"bucket_seconds\":60,"
            "\"timezone\":\"" +
            timezone + "\"},\"calendar_ref\":{\"calendar_id\":\"" + calendar + "\",\"calendar_version\":\"v1\"}}";
        auto [create_status, task] = env.service->CreateValueTask(config, BaselineSerializationFormat::kJson);
        assert(create_status == BaselineStatus::kOk && task);
        ValueBootstrapInput input;
        input.series_key = "svc-clock";
        for (int64_t minute = 0; minute < 200; ++minute) {
            const bool event =
                (minute >= 20 && minute < 25) || (minute >= 80 && minute < 85) || (minute >= 140 && minute < 145);
            input.observations.push_back(
                {base / 60 + minute, 100.0 + static_cast<double>(minute % 3) + (event ? 400.0 : 0.0), 1});
        }
        assert(task->Bootstrap(input).status == BaselineStatus::kOk);
        const auto seed = task->ExportBootstrapSeed(BaselineSerializationFormat::kJson);
        assert(seed.first == BaselineStatus::kOk);
        rapidjson::Document seed_doc;
        seed_doc.Parse(seed.second.c_str());
        assert(!seed_doc.HasParseError());
        const auto& hint = seed_doc["series_seeds"][0]["event_hint"];
        assert(hint["available"].GetBool());
        const double effect = hint["coeff"][0].GetDouble();
        assert(effect > 0.1);
        const auto artifact = task->ExportBootstrapArtifact(BaselineSerializationFormat::kJson);
        assert(artifact.first == BaselineStatus::kOk);
        const int64_t start = base / 60 + 300;
        const auto sequence = task->PredictBootstrap(input.series_key, start, 121, {});
        const auto rolling_sequence = task->PredictRolling(input.series_key, start, 121);
        assert(sequence.status == BaselineStatus::kOk && sequence.predictions.size() == 121);
        assert(rolling_sequence.status == BaselineStatus::kOk && rolling_sequence.predictions.size() == 121);
        for (std::size_t i = 0; i < sequence.predictions.size(); ++i) {
            const auto point = task->PredictBootstrap(input.series_key, start + static_cast<int64_t>(i), {});
            assert(point.status == BaselineStatus::kOk);
            assert(std::fabs(point.baseline_mu - sequence.predictions[i].baseline_mu) < 1e-9);
            const auto rolling = task->PredictRolling(input.series_key, start + static_cast<int64_t>(i));
            assert(rolling.status == BaselineStatus::kOk);
            assert(std::fabs(rolling.baseline_mu - rolling_sequence.predictions[i].baseline_mu) < 1e-9);
        }
        rapidjson::Document without_event;
        without_event.Parse(artifact.second.c_str());
        assert(!without_event.HasParseError());
        without_event["series_artifacts"][0]["model"]["event_block"]["enabled"].SetBool(false);
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        without_event.Accept(writer);
        assert(task->LoadBootstrapArtifact(buffer.GetString(), BaselineSerializationFormat::kJson) ==
               BaselineStatus::kOk);
        for (std::size_t i = 0; i < sequence.predictions.size(); ++i) {
            const auto plain = task->PredictBootstrap(input.series_key, start + static_cast<int64_t>(i), {});
            assert(plain.status == BaselineStatus::kOk);
            const int64_t minute = 300 + static_cast<int64_t>(i);
            const bool first_hour = minute >= 315 && minute < 345;
            const bool repeated_hour = mode != 1 && minute >= 375 && minute < 405;
            const double expected = first_hour || repeated_hour ? effect : 0.0;
            assert(std::fabs(std::log1p(sequence.predictions[i].baseline_mu) - std::log1p(plain.baseline_mu) -
                             expected) < 1e-9);
            const auto rolling_plain = task->PredictRolling(input.series_key, start + static_cast<int64_t>(i));
            assert(rolling_plain.status == BaselineStatus::kOk);
            assert(std::fabs(std::log1p(rolling_sequence.predictions[i].baseline_mu) -
                             std::log1p(rolling_plain.baseline_mu) - expected) < 1e-9);
        }
        assert(task->LoadBootstrapArtifact(artifact.second, BaselineSerializationFormat::kJson) == BaselineStatus::kOk);
    }
    std::printf("[PASS] Task-bound event calendar preserves DST and inherited timezone\n");
}

void TestTaskRejectsIncompatibleBootstrapArtifact() {
    std::printf("[TEST] B1 task rejects incompatible bootstrap artifact...\n");

    auto env = LoadBaselineService();
    auto [value_status, value_task] = env.service->CreateValueTask(
        ValueTaskConfig(), BaselineSerializationFormat::kJson);
    assert(value_status == BaselineStatus::kOk);
    assert(value_task != nullptr);
    const BootstrapTrainResult train = value_task->Bootstrap(BuildValueHistory());
    assert(train.status == BaselineStatus::kOk);
    auto [artifact_status, artifact_json] =
        value_task->ExportBootstrapArtifact(BaselineSerializationFormat::kJson);
    assert(artifact_status == BaselineStatus::kOk);
    assert(value_task->LoadBootstrapArtifact(
               artifact_json, BaselineSerializationFormat::kJson) == BaselineStatus::kOk);

    auto [other_status, other_task] = env.service->CreateValueTask(
        OtherValueTaskConfig(), BaselineSerializationFormat::kJson);
    assert(other_status == BaselineStatus::kOk);
    assert(other_task != nullptr);
    assert(other_task->LoadBootstrapArtifact(
               artifact_json, BaselineSerializationFormat::kJson) ==
           BaselineStatus::kIncompatibleArtifact);

    std::string bad_schema_json = artifact_json;
    const std::string schema_needle = "\"schema_version\":1";
    const std::size_t schema_pos = bad_schema_json.find(schema_needle);
    assert(schema_pos != std::string::npos);
    bad_schema_json.replace(schema_pos, schema_needle.size(), "\"schema_version\":2");
    assert(value_task->LoadBootstrapArtifact(
               bad_schema_json, BaselineSerializationFormat::kJson) ==
           BaselineStatus::kIncompatibleArtifact);

    std::string bad_algorithm_json = artifact_json;
    const std::string algorithm_needle = "\"algorithm_version\":\"b1-bootstrap-v1\"";
    const std::size_t algorithm_pos = bad_algorithm_json.find(algorithm_needle);
    assert(algorithm_pos != std::string::npos);
    bad_algorithm_json.replace(algorithm_pos,
                               algorithm_needle.size(),
                               "\"algorithm_version\":\"unknown\"");
    assert(value_task->LoadBootstrapArtifact(
               bad_algorithm_json, BaselineSerializationFormat::kJson) ==
           BaselineStatus::kIncompatibleArtifact);

    std::string bad_top_task_json = artifact_json;
    const std::string top_task_needle =
        "\"task_identity\":{\"task_id\":\"baseline_task_bps\"";
    const std::size_t top_task_pos = bad_top_task_json.find(top_task_needle);
    assert(top_task_pos != std::string::npos);
    bad_top_task_json.replace(
        top_task_pos,
        top_task_needle.size(),
        "\"task_identity\":{\"task_id\":\"baseline_task_other\"");
    assert(value_task->LoadBootstrapArtifact(
               bad_top_task_json, BaselineSerializationFormat::kJson) ==
           BaselineStatus::kIncompatibleArtifact);

    std::printf("[PASS] B1 task rejects incompatible bootstrap artifact\n");
}

void TestRuntimeConfigHarmonicOrders() {
    std::printf("[TEST] B1 runtime config harmonic orders...\n");

    const std::string config_path = "/tmp/flowsql_baseline_harmonic_order_test.yaml";
    {
        std::ofstream file(config_path);
        file << R"(
baseline:
  parser:
    tz_default: "UTC"
  shared_profile_config:
    daily_harmonic_order: 8
    weekly_harmonic_order: 5
    dme_max: 7
    m_month_enable: 4
    month_cov_min: 0.8
    lambda_season: 1.0
    lambda_dom: 4.0
    lambda_dme: 2.0
    lambda_lwd: 1.0
    lambda_event: 2.0
  value_sampled_profiles:
    cont_core:
      n_train_min: 50
      transform_name_override: "log1p"
  ratio_profiles:
    global:
      eps_logit: 1.0e-4
      m_floor: 1.0e-4
      v_floor: 0.25
    rate_core:
      d_min_train: 50
      s_prior: 2.0
      phi_over: 1.5
  solver_constants:
    solver_name: "weighted_huber_ridge_irls"
    c_huber: 1.5
    s_min_fit: 1.0e-3
    max_iter_fit: 15
    tol_obj_rel: 1.0e-4
    tol_beta_inf: 1.0e-5
    cond_max: 1.0e8
)";
        assert(file.good());
    }

    auto env = LoadBaselineService("config_file=" + config_path + ";strict=true");
    auto [value_status, value_task] = env.service->CreateValueTask(
        ValueTaskConfig(), BaselineSerializationFormat::kJson);
    assert(value_status == BaselineStatus::kOk);
    assert(value_task != nullptr);
    const BootstrapTrainResult train = value_task->Bootstrap(BuildValueHistory());
    assert(train.status == BaselineStatus::kOk);

    auto [seed_status, seed_json] =
        value_task->ExportBootstrapSeed(BaselineSerializationFormat::kJson);
    assert(seed_status == BaselineStatus::kOk);

    rapidjson::Document doc;
    doc.Parse(seed_json.c_str());
    assert(!doc.HasParseError());
    assert(doc.HasMember("series_seeds"));
    assert(doc["series_seeds"].IsArray());
    assert(doc["series_seeds"].Size() == 1);
    const auto& seed_item = doc["series_seeds"][0];
    assert(seed_item.HasMember("theta_init"));
    assert(seed_item["theta_init"].HasMember("daily_harmonic"));
    assert(seed_item["theta_init"].HasMember("weekly_harmonic"));
    assert(seed_item["theta_init"]["daily_harmonic"].IsArray());
    assert(seed_item["theta_init"]["weekly_harmonic"].IsArray());
    assert(seed_item["theta_init"]["daily_harmonic"].Size() == 8);
    assert(seed_item["theta_init"]["weekly_harmonic"].Size() == 5);

    std::printf("[PASS] B1 runtime config harmonic orders\n");
}

void TestRuntimeConfigDefaultDailyHarmonicOrder() {
    std::printf("[TEST] B1 runtime config default daily harmonic order...\n");

    auto env = LoadBaselineService();
    auto [value_status, value_task] = env.service->CreateValueTask(
        ValueTaskConfig(), BaselineSerializationFormat::kJson);
    assert(value_status == BaselineStatus::kOk);
    assert(value_task != nullptr);
    const BootstrapTrainResult train = value_task->Bootstrap(BuildValueHistory());
    assert(train.status == BaselineStatus::kOk);

    auto [seed_status, seed_json] =
        value_task->ExportBootstrapSeed(BaselineSerializationFormat::kJson);
    assert(seed_status == BaselineStatus::kOk);

    rapidjson::Document doc;
    doc.Parse(seed_json.c_str());
    assert(!doc.HasParseError());
    assert(doc.HasMember("series_seeds"));
    assert(doc["series_seeds"].IsArray());
    assert(doc["series_seeds"].Size() == 1);
    const auto& seed_item = doc["series_seeds"][0];
    assert(seed_item.HasMember("theta_init"));
    assert(seed_item["theta_init"].HasMember("daily_harmonic"));
    assert(seed_item["theta_init"]["daily_harmonic"].IsArray());
    assert(seed_item["theta_init"]["daily_harmonic"].Size() == 6);

    std::printf("[PASS] B1 runtime config default daily harmonic order\n");
}

void TestB2ValueRollingEmptyStart() {
    std::printf("[TEST] B2 value rolling empty start...\n");

    auto env = LoadBaselineService();
    auto [status, task] =
        env.service->CreateValueTask(ValueTaskConfig(), BaselineSerializationFormat::kJson);
    assert(status == BaselineStatus::kOk);
    assert(task != nullptr);

    ValueRollingObservation first;
    first.series_key = "link-a";
    first.bucket_id = 100;
    first.value = 99.0;
    RollingBaselineResult first_result = task->SubmitObservation(first, RollingSubmitOptions{});
    assert(first_result.status == BaselineStatus::kOk);
    assert(first_result.series_key == "link-a");
    assert(first_result.bucket_id == 100);
    assert(!first_result.can_score);
    assert(first_result.can_update);
    assert(first_result.state_status == "cold_learning");
    assert(first_result.maturity_status == "cold_learning");
    assert(first_result.score_trust_status == "score_untrusted");
    assert(first_result.calibration_status == "uncalibrated");
    assert(first_result.learning_confidence > 0.0);
    assert(first_result.score_confidence == 0.0);
    assert(first_result.effective_confidence == 0.0);
    assert(!first_result.can_alert);
    assert(first_result.enabled_components.empty());
    assert(!first_result.component_readiness.empty());
    assert(first_result.band_width > 0.0);
    assert(first_result.baseline_lower >= 0.0);

    ValueRollingObservation second = first;
    second.bucket_id = 101;
    second.value = 110.0;
    RollingBaselineResult second_result =
        task->SubmitObservation(second, RollingSubmitOptions{});
    assert(second_result.status == BaselineStatus::kOk);
    assert(second_result.can_score);
    assert(second_result.can_update);
    assert(!second_result.maturity_status.empty());
    assert(!second_result.score_trust_status.empty());
    assert(!second_result.calibration_status.empty());
    assert(!second_result.can_alert);
    assert(second_result.update_weight > 0.0);
    assert(second_result.band_width > 0.0);

    std::printf("[PASS] B2 value rolling empty start\n");
}

void TestB2SampledValueLowSupportStart() {
    std::printf("[TEST] B2 sampled value low support start...\n");

    auto env = LoadBaselineService();
    auto [status, task] = env.service->CreateValueTask(SampledValueTaskConfig(),
                                                       BaselineSerializationFormat::kJson);
    assert(status == BaselineStatus::kOk);
    assert(task != nullptr);

    ValueRollingObservation low;
    low.series_key = "sampled-a";
    low.bucket_id = 100;
    low.value = 10.0;
    low.sample_count = 2;
    RollingBaselineResult low_result = task->SubmitObservation(low, RollingSubmitOptions{});
    assert(low_result.status == BaselineStatus::kInsufficientData);
    assert(low_result.skipped_low_sample_count);

    ValueRollingObservation ok = low;
    ok.bucket_id = 101;
    ok.sample_count = 20;
    RollingBaselineResult ok_result = task->SubmitObservation(ok, RollingSubmitOptions{});
    assert(ok_result.status == BaselineStatus::kOk);
    assert(ok_result.can_update);
    assert(ok_result.state_status == "cold_learning");

    std::printf("[PASS] B2 sampled value low support start\n");
}

void TestB2RatioRollingInputAndEmptyStart() {
    std::printf("[TEST] B2 ratio rolling input and empty start...\n");

    auto env = LoadBaselineService();
    auto [status, task] =
        env.service->CreateRatioTask(RatioTaskConfig(), BaselineSerializationFormat::kJson);
    assert(status == BaselineStatus::kOk);
    assert(task != nullptr);

    RatioRollingObservation invalid;
    invalid.series_key = "svc-a";
    invalid.bucket_id = 100;
    invalid.numerator = 11.0;
    invalid.denominator = 10.0;
    RollingBaselineResult invalid_result =
        task->SubmitObservation(invalid, RollingSubmitOptions{});
    assert(invalid_result.status == BaselineStatus::kInvalidArgument);

    RatioRollingObservation first;
    first.series_key = "svc-a";
    first.bucket_id = 101;
    first.numerator = 95.0;
    first.denominator = 100.0;
    RollingBaselineResult first_result = task->SubmitObservation(first, RollingSubmitOptions{});
    assert(first_result.status == BaselineStatus::kOk);
    assert(!first_result.can_score);
    assert(first_result.can_update);
    assert(first_result.baseline_lower >= 0.0);
    assert(first_result.baseline_upper <= 1.0);
    assert(first_result.band_width > 0.0);

    std::printf("[PASS] B2 ratio rolling input and empty start\n");
}

void TestB2RollingSnapshotAndBootstrapWarmup() {
    std::printf("[TEST] B2 rolling snapshot and bootstrap warm-up...\n");

    auto env = LoadBaselineService();
    auto [status, task] =
        env.service->CreateValueTask(ValueTaskConfig(), BaselineSerializationFormat::kJson);
    assert(status == BaselineStatus::kOk);
    assert(task != nullptr);

    const BootstrapTrainResult train = task->Bootstrap(BuildValueHistory());
    assert(train.status == BaselineStatus::kOk);

    auto [task_snapshot_status, task_snapshot_json] =
        task->QueryTaskSnapshot(BaselineSerializationFormat::kJson);
    assert(task_snapshot_status == BaselineStatus::kOk);
    rapidjson::Document task_doc;
    task_doc.Parse(task_snapshot_json.c_str());
    assert(!task_doc.HasParseError());
    assert(std::string(task_doc["document_kind"].GetString()) == "rolling_task_snapshot");
    assert(task_doc["rolling_series_count"].GetUint64() == 1);
    assert(task_doc.HasMember("state_status_counts"));
    assert(task_doc.HasMember("maturity_status_counts"));
    assert(task_doc.HasMember("score_trust_status_counts"));
    assert(task_doc.HasMember("calibration_status_counts"));
    assert(task_doc.HasMember("rolling_state_memory_estimate_bytes"));

    auto [series_snapshot_status, series_snapshot_json] =
        task->QuerySeriesSnapshot("svc-a", BaselineSerializationFormat::kJson);
    assert(series_snapshot_status == BaselineStatus::kOk);
    rapidjson::Document series_doc;
    series_doc.Parse(series_snapshot_json.c_str());
    assert(!series_doc.HasParseError());
    assert(std::string(series_doc["document_kind"].GetString()) == "rolling_series_snapshot");
    assert(series_doc["series_identity"]["series_key"] == "svc-a");
    assert(series_doc.HasMember("band"));
    assert(series_doc.HasMember("control"));
    assert(series_doc.HasMember("maturity"));
    assert(series_doc["maturity"].HasMember("status"));
    assert(series_doc["maturity"].HasMember("enabled_components"));
    assert(series_doc["maturity"].HasMember("component_readiness"));
    assert(series_doc["maturity"].HasMember("coverage"));
    assert(series_doc.HasMember("score_trust"));
    assert(series_doc["score_trust"].HasMember("status"));
    assert(series_doc["score_trust"].HasMember("can_alert"));
    assert(series_doc.HasMember("calibration"));
    assert(series_doc["calibration"].HasMember("band_multiplier"));
    assert(series_doc["calibration"].HasMember("calibration_update_count"));
    assert(series_doc.HasMember("monthpos"));
    assert(series_doc["monthpos"].HasMember("status"));

    ValueRollingObservation stream;
    stream.series_key = "svc-a";
    stream.bucket_id = 200;
    stream.value = 105.0;
    RollingBaselineResult result = task->SubmitObservation(stream, RollingSubmitOptions{});
    assert(result.status == BaselineStatus::kOk);
    assert(result.can_score);
    assert(result.state_status == "warming");
    assert(!result.maturity_status.empty());
    assert(!result.score_trust_status.empty());
    assert(!result.calibration_status.empty());
    assert(result.score_trust_status != "score_ready");
    assert(!result.can_alert);

    std::printf("[PASS] B2 rolling snapshot and bootstrap warm-up\n");
}

void TestB2RollingPredict() {
    std::printf("[TEST] B2 rolling predict...\n");

    auto env = LoadBaselineService();
    auto [status, task] =
        env.service->CreateValueTask(ValueTaskConfig(), BaselineSerializationFormat::kJson);
    assert(status == BaselineStatus::kOk);
    assert(task != nullptr);

    RollingPrediction missing = task->PredictRolling("link-predict", 101);
    assert(missing.status == BaselineStatus::kNotTrained);

    ValueRollingObservation first;
    first.series_key = "link-predict";
    first.bucket_id = 100;
    first.value = 100.0;
    assert(task->SubmitObservation(first, RollingSubmitOptions{}).status ==
           BaselineStatus::kOk);

    RollingPrediction past = task->PredictRolling("link-predict", 99);
    assert(past.status == BaselineStatus::kInvalidArgument);

    RollingPrediction prediction = task->PredictRolling("link-predict", 101);
    assert(prediction.status == BaselineStatus::kOk);
    assert(prediction.baseline_mu > 0.0);
    assert(prediction.baseline_lower <= prediction.baseline_mu);
    assert(prediction.baseline_upper >= prediction.baseline_mu);
    assert(prediction.baseline_upper > prediction.baseline_lower);
    assert(prediction.band_z == 3.0);
    const RollingPredictionSequence sequence =
        task->PredictRolling("link-predict", 101, 3);
    assert(sequence.status == BaselineStatus::kOk);
    assert(sequence.series_key == "link-predict");
    assert(sequence.start_bucket_id == 101);
    assert(sequence.point_count == 3);
    assert(sequence.predictions.size() == 3);
    assert(sequence.predictions[0].bucket_id == 101);
    assert(sequence.predictions[1].bucket_id == 102);
    assert(sequence.predictions[2].bucket_id == 103);
    assert(sequence.predictions[0].status == BaselineStatus::kOk);
    assert(sequence.predictions[1].status == BaselineStatus::kOk);
    assert(sequence.predictions[2].status == BaselineStatus::kOk);
    const RollingPredictionSequence empty_sequence =
        task->PredictRolling("link-predict", 101, 0);
    assert(empty_sequence.status == BaselineStatus::kInvalidArgument);

    ValueRollingObservation second = first;
    second.bucket_id = 101;
    second.value = 102.0;
    RollingBaselineResult update = task->SubmitObservation(second, RollingSubmitOptions{});
    assert(update.status == BaselineStatus::kOk);
    assert(update.can_score);

    auto [ratio_status, ratio_task] =
        env.service->CreateRatioTask(RatioTaskConfig(), BaselineSerializationFormat::kJson);
    assert(ratio_status == BaselineStatus::kOk);
    assert(ratio_task != nullptr);

    RatioRollingObservation ratio_first;
    ratio_first.series_key = "ratio-predict";
    ratio_first.bucket_id = 100;
    ratio_first.numerator = 80.0;
    ratio_first.denominator = 100.0;
    assert(ratio_task->SubmitObservation(ratio_first, RollingSubmitOptions{}).status ==
           BaselineStatus::kOk);

    RollingPrediction ratio_prediction = ratio_task->PredictRolling("ratio-predict", 101);
    assert(ratio_prediction.status == BaselineStatus::kOk);
    assert(ratio_prediction.baseline_lower >= 0.0);
    assert(ratio_prediction.baseline_upper <= 1.0);
    assert(ratio_prediction.baseline_lower <= ratio_prediction.baseline_mu);
    assert(ratio_prediction.baseline_upper >= ratio_prediction.baseline_mu);
    assert(ratio_prediction.band_z == 3.0);
    const RollingPredictionSequence ratio_sequence =
        ratio_task->PredictRolling("ratio-predict", 101, 2);
    assert(ratio_sequence.status == BaselineStatus::kOk);
    assert(ratio_sequence.predictions.size() == 2);
    assert(ratio_sequence.predictions[0].bucket_id == 101);
    assert(ratio_sequence.predictions[1].bucket_id == 102);

    std::printf("[PASS] B2 rolling predict\n");
}

void TestB2RollingFailureSemantics() {
    std::printf("[TEST] B2 rolling failure semantics...\n");

    {
        auto env = LoadBaselineService();
        auto [status, task] =
            env.service->CreateValueTask(ValueTaskConfig(), BaselineSerializationFormat::kJson);
        assert(status == BaselineStatus::kOk);
        ValueRollingObservation obs;
        obs.series_key = "link-disabled";
        obs.bucket_id = 10;
        obs.value = 1.0;
        RollingSubmitOptions options;
        options.allow_auto_init_from_bootstrap = false;
        options.allow_auto_init_from_empty = false;
        RollingBaselineResult result = task->SubmitObservation(obs, options);
        assert(result.status == BaselineStatus::kNotTrained);
    }

    {
        auto env = LoadBaselineService();
        auto [status, task] =
            env.service->CreateValueTask(ValueTaskConfig(), BaselineSerializationFormat::kJson);
        assert(status == BaselineStatus::kOk);
        ValueRollingObservation obs;
        obs.series_key = "link-dup";
        obs.bucket_id = 10;
        obs.value = 10.0;
        assert(task->SubmitObservation(obs, RollingSubmitOptions{}).status ==
               BaselineStatus::kOk);
        assert(task->SubmitObservation(obs, RollingSubmitOptions{}).status ==
               BaselineStatus::kInvalidArgument);
    }

    {
        auto env = LoadBaselineService();
        auto [status, task] = env.service->CreateValueTask(SampledValueTaskConfig(),
                                                           BaselineSerializationFormat::kJson);
        assert(status == BaselineStatus::kOk);
        ValueRollingObservation obs;
        obs.series_key = "sampled-existing";
        obs.bucket_id = 10;
        obs.value = 10.0;
        obs.sample_count = 20;
        assert(task->SubmitObservation(obs, RollingSubmitOptions{}).status ==
               BaselineStatus::kOk);
        obs.bucket_id = 11;
        obs.sample_count = 2;
        RollingBaselineResult low = task->SubmitObservation(obs, RollingSubmitOptions{});
        assert(low.status == BaselineStatus::kOk);
        assert(low.skipped_low_sample_count);
        assert(!low.can_update);
    }

    {
        auto env = LoadBaselineService();
        auto [status, task] =
            env.service->CreateRatioTask(RatioTaskConfig(), BaselineSerializationFormat::kJson);
        assert(status == BaselineStatus::kOk);
        RatioRollingObservation obs;
        obs.series_key = "ratio-low-den";
        obs.bucket_id = 10;
        obs.numerator = 1.0;
        obs.denominator = 5.0;
        RollingBaselineResult result = task->SubmitObservation(obs, RollingSubmitOptions{});
        assert(result.status == BaselineStatus::kInsufficientData);
        assert(result.skipped_low_denominator);
    }

    std::printf("[PASS] B2 rolling failure semantics\n");
}

template <typename Task, typename MakeHistory, typename MakeObservation>
void CheckReviewBootstrapRuntimeReplacement(Task* task, MakeHistory make_history, MakeObservation make_observation,
                                            double initial, double trained, double retrained) {
    assert(task->Bootstrap(make_history("seeded", trained, 0)).status == BaselineStatus::kOk);
    const auto seeded_initial = task->QuerySeriesSnapshot("seeded", BaselineSerializationFormat::kJson);
    for (const std::string series : {"seeded", "online", "target"}) {
        assert(task->SubmitObservation(make_observation(series, 500, initial), {}).status == BaselineStatus::kOk);
    }
    const auto seeded = task->QuerySeriesSnapshot("seeded", BaselineSerializationFormat::kJson);
    const auto online = task->QuerySeriesSnapshot("online", BaselineSerializationFormat::kJson);
    const auto check_other_series = [&]() {
        assert(task->QuerySeriesSnapshot("seeded", BaselineSerializationFormat::kJson) == seeded);
        assert(task->QuerySeriesSnapshot("online", BaselineSerializationFormat::kJson) == online);
        for (const std::string series : {"seeded", "online"}) {
            assert(task->SubmitObservation(make_observation(series, 500, initial), {}).status ==
                   BaselineStatus::kInvalidArgument);
        }
    };
    const auto check_target = [&](int64_t last_bucket, double expected) {
        const auto snapshot = task->QuerySeriesSnapshot("target", BaselineSerializationFormat::kJson);
        assert(snapshot.first == BaselineStatus::kOk);
        rapidjson::Document doc;
        doc.Parse(snapshot.second.c_str());
        assert(!doc.HasParseError());
        if (doc["last_seen_bucket"].GetInt64() != last_bucket) {
            std::fprintf(stderr, "B10 runtime replacement expected bucket=%lld actual=%lld\n",
                         static_cast<long long>(last_bucket),
                         static_cast<long long>(doc["last_seen_bucket"].GetInt64()));
        }
        assert(doc["last_seen_bucket"].GetInt64() == last_bucket);
        assert(std::fabs(doc["band"]["baseline_mu"].GetDouble() - expected) < 0.01 * std::max(1.0, expected));
        AssertSnapshotWithoutSubmit(snapshot);
        return snapshot;
    };
    assert(task->Bootstrap(make_history("target", trained, 0)).status == BaselineStatus::kOk);
    const auto trained_snapshot = check_target(199, trained);
    check_other_series();
    const auto saved_artifact = task->ExportBootstrapArtifact(BaselineSerializationFormat::kJson);
    assert(saved_artifact.first == BaselineStatus::kOk);
    const auto check_rejected_operation = [&](auto operation) {
        const auto artifact = task->ExportBootstrapArtifact(BaselineSerializationFormat::kJson);
        const auto seed = task->ExportBootstrapSeed(BaselineSerializationFormat::kJson);
        const auto task_snapshot = task->QueryTaskSnapshot(BaselineSerializationFormat::kJson);
        const auto snapshot = task->QuerySeriesSnapshot("target", BaselineSerializationFormat::kJson);
        const auto config = task->ExportConfig(BaselineSerializationFormat::kJson);
        assert(operation() != BaselineStatus::kOk);
        assert(task->ExportBootstrapArtifact(BaselineSerializationFormat::kJson) == artifact);
        assert(task->ExportBootstrapSeed(BaselineSerializationFormat::kJson) == seed);
        assert(task->QueryTaskSnapshot(BaselineSerializationFormat::kJson) == task_snapshot);
        assert(task->QuerySeriesSnapshot("target", BaselineSerializationFormat::kJson) == snapshot);
        assert(task->ExportConfig(BaselineSerializationFormat::kJson) == config);
        check_other_series();
    };
    auto no_replace = make_history("target", retrained, 200);
    no_replace.options.force_replace_existing_artifact = false;
    check_rejected_operation([&]() { return task->Bootstrap(no_replace).status; });
    check_rejected_operation([&]() { return task->Bootstrap(make_history("target", -1.0, 200)).status; });
    assert(task->SubmitObservation(make_observation("target", 200, trained), {}).status == BaselineStatus::kOk);
    assert(task->Bootstrap(make_history("target", retrained, 200)).status == BaselineStatus::kOk);
    check_target(399, retrained);
    check_other_series();
    assert(task->SubmitObservation(make_observation("target", 399, retrained), {}).status ==
           BaselineStatus::kInvalidArgument);
    assert(task->SubmitObservation(make_observation("target", 400, retrained), {}).status == BaselineStatus::kOk);
    assert(task->SubmitObservation(make_observation("target", 400, retrained), {}).status ==
           BaselineStatus::kInvalidArgument);
    check_rejected_operation([&]() { return task->LoadBootstrapArtifact("{", BaselineSerializationFormat::kJson); });
    std::string incompatible = saved_artifact.second;
    const std::string needle = "\"task_id\":\"";
    const auto position = incompatible.find(needle);
    assert(position != std::string::npos);
    incompatible.insert(position + needle.size(), "incompatible-");
    check_rejected_operation(
        [&]() { return task->LoadBootstrapArtifact(incompatible, BaselineSerializationFormat::kJson); });
    assert(task->LoadBootstrapArtifact(saved_artifact.second, BaselineSerializationFormat::kJson) ==
           BaselineStatus::kOk);
    const auto loaded_artifact = task->ExportBootstrapArtifact(BaselineSerializationFormat::kJson);
    assert(loaded_artifact.first == BaselineStatus::kOk);
    rapidjson::Document loaded_doc;
    loaded_doc.Parse(loaded_artifact.second.c_str());
    assert(!loaded_doc.HasParseError() && loaded_doc["series_artifacts"].Size() == 2);
    const auto restored_prediction = task->PredictBootstrap("target", 200, BootstrapPredictionOptions{});
    assert(restored_prediction.status == BaselineStatus::kOk);
    assert(std::fabs(restored_prediction.baseline_mu - trained) < 0.01 * std::max(1.0, trained));
    assert(task->QuerySeriesSnapshot("target", BaselineSerializationFormat::kJson) == trained_snapshot);
    assert(task->QuerySeriesSnapshot("seeded", BaselineSerializationFormat::kJson) == seeded_initial);
    assert(task->QuerySeriesSnapshot("online", BaselineSerializationFormat::kJson).first ==
           BaselineStatus::kNotTrained);
    assert(task->SubmitObservation(make_observation("target", 200, trained), {}).status == BaselineStatus::kOk);
    assert(task->SubmitObservation(make_observation("target", 200, trained), {}).status ==
           BaselineStatus::kInvalidArgument);
}

void TestReviewValueBootstrapRuntimeReplacement() {
    std::printf("[TEST] review value bootstrap/runtime replacement...\n");
    auto env = LoadBaselineService();
    auto [status, task] = env.service->CreateValueTask(ValueTaskConfig(), BaselineSerializationFormat::kJson);
    assert(status == BaselineStatus::kOk);
    const auto history = [](const std::string& series, double value, int64_t start) {
        ValueBootstrapInput input;
        input.series_key = series;
        for (int64_t bucket = start; bucket < start + 200; ++bucket) {
            input.observations.push_back({bucket, value, 1});
        }
        return input;
    };
    const auto observation = [](const std::string& series, int64_t bucket, double value) {
        return ValueRollingObservation{series, bucket, value, 1};
    };
    CheckReviewBootstrapRuntimeReplacement(task.get(), history, observation, 10.0, 400.0, 800.0);
    std::printf("[PASS] review value bootstrap/runtime replacement\n");
}

void TestReviewRatioBootstrapRuntimeReplacement() {
    std::printf("[TEST] review ratio bootstrap/runtime replacement...\n");
    auto env = LoadBaselineService();
    auto [status, task] = env.service->CreateRatioTask(RatioTaskConfig(), BaselineSerializationFormat::kJson);
    assert(status == BaselineStatus::kOk);
    const auto history = [](const std::string& series, double probability, int64_t start) {
        RatioBootstrapInput input;
        input.series_key = series;
        for (int64_t bucket = start; bucket < start + 200; ++bucket) {
            input.observations.push_back({bucket, 100.0 * probability, 100.0});
        }
        return input;
    };
    const auto observation = [](const std::string& series, int64_t bucket, double probability) {
        return RatioRollingObservation{series, bucket, 100.0 * probability, 100.0};
    };
    CheckReviewBootstrapRuntimeReplacement(task.get(), history, observation, 0.1, 0.4, 0.8);
    std::printf("[PASS] review ratio bootstrap/runtime replacement\n");
}

void TestReviewValueIdentityTransform() {
    const std::string path = "/tmp/flowsql_b05_identity.yaml";
    {
        std::ofstream file(path);
        file << "baseline:\n  value_sampled_profiles:\n    cont_core:\n      n_train_min: 50\n"
                "      transform_name_override: identity\n";
    }
    {
        auto env = LoadBaselineService("config_file=" + path + ";strict=false");
        auto [status, task] =
            env.service->CreateValueTask(SampledValueTaskConfig(), BaselineSerializationFormat::kJson);
        assert(status == BaselineStatus::kOk);
        auto history = BuildSampledValueHistory();
        for (auto& point : history.observations) point.value = 100.0;
        assert(task->Bootstrap(history).status == BaselineStatus::kOk);
        BootstrapPredictionOptions options;
        options.include_model_space_debug = true;
        const auto bootstrap = task->PredictBootstrap(history.series_key, 200, options);
        assert(bootstrap.status == BaselineStatus::kOk);
        assert(std::fabs(bootstrap.model_space_mu - 100.0) < 0.01);
        assert(std::fabs(bootstrap.baseline_mu - 100.0) < 0.01);
        const auto sequence = task->PredictBootstrap(history.series_key, 200, 3, options);
        assert(sequence.status == BaselineStatus::kOk && sequence.predictions.size() == 3);
        for (const auto& point : sequence.predictions) assert(std::fabs(point.baseline_mu - 100.0) < 0.01);
        const auto artifact = task->ExportBootstrapArtifact(BaselineSerializationFormat::kJson);
        assert(artifact.first == BaselineStatus::kOk);
        ValueRollingObservation obs{history.series_key, 200, 100.0, 50};
        const auto rolling = task->SubmitObservation(obs, {});
        assert(rolling.status == BaselineStatus::kOk);
        assert(std::fabs(rolling.observed_model - 100.0) < 1.0e-9);
        assert(std::fabs(rolling.baseline_mu - 100.0) < 0.01);
        assert(std::fabs(rolling.z_score) < 0.01);
        assert(std::fabs(task->PredictRolling(history.series_key, 201).baseline_mu - 100.0) < 0.01);
        obs.series_key = "identity-cold";
        assert(std::fabs(task->SubmitObservation(obs, {}).baseline_mu - 100.0) < 0.01);
        const auto cold_sequence = task->PredictRolling(obs.series_key, 201, 3);
        assert(cold_sequence.status == BaselineStatus::kOk);
        for (const auto& point : cold_sequence.predictions) assert(std::fabs(point.baseline_mu - 100.0) < 0.01);
        const auto snapshot = task->QuerySeriesSnapshot(obs.series_key, BaselineSerializationFormat::kJson);
        assert(snapshot.first == BaselineStatus::kOk);
        rapidjson::Document doc;
        doc.Parse(snapshot.second.c_str());
        assert(!doc.HasParseError());
        assert(std::fabs(doc["band"]["baseline_mu"].GetDouble() - 100.0) < 0.01);
        assert(task->Close() == BaselineStatus::kOk);
        auto [reload_status, reload] =
            env.service->CreateValueTask(SampledValueTaskConfig(), BaselineSerializationFormat::kJson);
        assert(reload_status == BaselineStatus::kOk);
        assert(reload->LoadBootstrapArtifact(artifact.second, BaselineSerializationFormat::kJson) ==
               BaselineStatus::kOk);
        obs.series_key = history.series_key;
        const auto loaded = reload->SubmitObservation(obs, {});
        assert(loaded.status == BaselineStatus::kOk);
        assert(std::fabs(loaded.observed_model - 100.0) < 1.0e-9);
        assert(std::fabs(loaded.z_score) < 0.01);
        const auto before_snapshot = reload->QuerySeriesSnapshot(obs.series_key, BaselineSerializationFormat::kJson);
        const auto before_artifact = reload->ExportBootstrapArtifact(BaselineSerializationFormat::kJson);
        std::string incompatible = artifact.second;
        const std::string identity = "\"identity\"";
        std::size_t pos = 0;
        while ((pos = incompatible.find(identity, pos)) != std::string::npos) {
            incompatible.replace(pos, identity.size(), "\"log1p\"");
            pos += 7;
        }
        assert(reload->LoadBootstrapArtifact(incompatible, BaselineSerializationFormat::kJson) ==
               BaselineStatus::kIncompatibleArtifact);
        assert(reload->QuerySeriesSnapshot(obs.series_key, BaselineSerializationFormat::kJson) == before_snapshot);
        assert(reload->ExportBootstrapArtifact(BaselineSerializationFormat::kJson) == before_artifact);
    }
    flowsql::baseline::ResetBaselineRuntimeConfig();
    std::printf("[PASS] review value identity transform\n");
}

void TestReviewRatioConfiguredClip() {
    const std::string path = "/tmp/flowsql_b05_ratio_clip.yaml";
    {
        std::ofstream file(path);
        file << "baseline:\n  ratio_profiles:\n    global:\n      eps_logit: 0.1\n"
                "    rate_core:\n      d_min_train: 50\n      s_prior: 2.0\n      phi_over: 1.5\n";
    }
    {
        auto env = LoadBaselineService("config_file=" + path + ";strict=false");
        auto [status, task] = env.service->CreateRatioTask(RatioTaskConfig(), BaselineSerializationFormat::kJson);
        assert(status == BaselineStatus::kOk);
        auto history = BuildRatioHistory();
        for (auto& point : history.observations) point.numerator = point.denominator;
        assert(task->Bootstrap(history).status == BaselineStatus::kOk);
        RatioRollingObservation obs{history.series_key, 200, 100.0, 100.0};
        const auto rolling = task->SubmitObservation(obs, {});
        assert(rolling.status == BaselineStatus::kOk);
        assert(std::fabs(rolling.observed_model - std::log(9.0)) < 1.0e-9);
        assert(std::fabs(rolling.z_score) < 0.01);
        assert(std::fabs(rolling.baseline_mu - 0.9) < 0.001);
        obs.series_key = "ratio-cold";
        obs.numerator = 0.0;
        const auto cold = task->SubmitObservation(obs, {});
        assert(cold.status == BaselineStatus::kOk);
        assert(std::fabs(cold.observed_model + std::log(9.0)) < 1.0e-9);
    }
    flowsql::baseline::ResetBaselineRuntimeConfig();
    std::printf("[PASS] review ratio configured clip\n");
}

void TestReviewObservationValidation() {
    std::printf("[TEST] review observation validation...\n");
    auto env = LoadBaselineService();
    auto [value_status, value_task] =
        env.service->CreateValueTask(ValueTaskConfig(), BaselineSerializationFormat::kJson);
    assert(value_status == BaselineStatus::kOk);
    auto value_history = BuildValueHistory();
    value_history.observations.push_back({0, -100.0, 1});
    value_history.observations.push_back({201, std::numeric_limits<double>::max(), 2});
    const auto value_train = value_task->Bootstrap(value_history);
    assert(value_train.status == BaselineStatus::kOk);
    assert(value_train.accepted_count == 200 && value_train.rejected_count == 2);

    auto [sampled_status, sampled_task] =
        env.service->CreateValueTask(SampledValueTaskConfig(), BaselineSerializationFormat::kJson);
    assert(sampled_status == BaselineStatus::kOk);
    auto sampled_history = BuildSampledValueHistory();
    sampled_history.observations.push_back({0, 100000.0, 0});
    const auto sampled_train = sampled_task->Bootstrap(sampled_history);
    assert(sampled_train.status == BaselineStatus::kOk);
    assert(sampled_train.accepted_count == 200 && sampled_train.rejected_count == 1);

    auto [ratio_status, ratio_task] =
        env.service->CreateRatioTask(RatioTaskConfig(), BaselineSerializationFormat::kJson);
    assert(ratio_status == BaselineStatus::kOk);
    auto ratio_history = BuildRatioHistory();
    ratio_history.observations.push_back({0, 2.0, 1.0});
    ratio_history.observations.push_back({201, 0.0, std::numeric_limits<double>::max()});
    ratio_history.observations.push_back(ratio_history.observations.back());
    const auto ratio_train = ratio_task->Bootstrap(ratio_history);
    assert(ratio_train.status == BaselineStatus::kOk);
    assert(ratio_train.accepted_count == 200 && ratio_train.rejected_count == 2);

    auto good = BuildRelationObservation(100, 30, 20, 10);
    good.metrics[0].total = 100.0;  // Explicit groups may cover only part of the total.
    std::vector<RelationBootstrapMetric> invalid_metrics;
    for (double bad_mass : {-1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        auto metric = good.metrics[0];
        metric.values_by_group[1] = bad_mass;
        invalid_metrics.push_back(metric);
    }
    auto metric = good.metrics[0];
    metric.total = std::numeric_limits<double>::infinity();
    invalid_metrics.push_back(metric);
    metric = good.metrics[0];
    metric.metric = "wrong";
    invalid_metrics.push_back(metric);
    metric = good.metrics[0];
    metric.values_by_group.pop_back();
    invalid_metrics.push_back(metric);
    metric = good.metrics[0];
    metric.values_by_group.push_back(0.0);
    invalid_metrics.push_back(metric);

    auto [relation_status, relation_task] =
        env.service->CreateRelationTask(RelationTaskConfig(), BaselineSerializationFormat::kJson);
    assert(relation_status == BaselineStatus::kOk);
    auto relation_history = BuildRelationHistory();
    for (const auto& invalid_metric : invalid_metrics) {
        auto block = relation_history.blocks.front();
        block.metrics[0] = invalid_metric;
        relation_history.blocks.push_back(block);
    }
    auto overflow_block = relation_history.blocks.front();
    overflow_block.bucket_id = 201;
    overflow_block.metrics[0].total = std::numeric_limits<double>::max();
    overflow_block.metrics[0].values_by_group = {std::numeric_limits<double>::max(), 0.0, 0.0};
    relation_history.blocks.push_back(overflow_block);
    relation_history.blocks.push_back(overflow_block);
    const auto relation_train = relation_task->Bootstrap(relation_history);
    assert(relation_train.status == BaselineStatus::kOk);
    assert(relation_train.accepted_count == 10);
    assert(relation_train.rejected_count == invalid_metrics.size() + 1);

    auto [multi_status, multi_task] =
        env.service->CreateRelationTask(MultiMetricRelationTaskConfig(), BaselineSerializationFormat::kJson);
    assert(multi_status == BaselineStatus::kOk);
    good.metrics.push_back(good.metrics[0]);
    good.metrics[1].metric = "pps";
    for (const std::string source : {"existing", "new"}) {
        good.series_key = source;
        good.bucket_id = 100;
        if (source == "existing") {
            assert(multi_task->SubmitObservation(good, {}).status == BaselineStatus::kOk);
            good.bucket_id = 101;
        }
        const auto before_task = multi_task->QueryTaskSnapshot(BaselineSerializationFormat::kJson);
        const auto before_source = multi_task->QuerySeriesSnapshot(source, BaselineSerializationFormat::kJson);
        for (auto invalid_metric : invalid_metrics) {
            if (invalid_metric.metric == "bps") invalid_metric.metric = "pps";
            auto bad = good;
            bad.metrics[1] = invalid_metric;
            const auto rejected = multi_task->SubmitObservation(bad, {});
            assert(rejected.status == BaselineStatus::kInvalidArgument);
            assert(rejected.routed_results.empty());
            assert(multi_task->QueryTaskSnapshot(BaselineSerializationFormat::kJson) == before_task);
            assert(multi_task->QuerySeriesSnapshot(source, BaselineSerializationFormat::kJson) == before_source);
        }
        assert(multi_task->SubmitObservation(good, {}).status == BaselineStatus::kOk);
    }
    std::printf("[PASS] review observation validation\n");
}

void TestReviewObservationOrder() {
    std::printf("[TEST] review observation order rejects skipped and routed duplicates...\n");
    auto env = LoadBaselineService();
    {
        auto [status, task] = env.service->CreateValueTask(ValueTaskConfig(), BaselineSerializationFormat::kJson);
        assert(status == BaselineStatus::kOk);
        ValueRollingObservation obs{"ordered-value", 100, 100.0, 1};
        assert(task->SubmitObservation(obs, {}).status == BaselineStatus::kOk);
        obs.bucket_id = 103;
        obs.value = 1e100;
        const auto skipped = task->SubmitObservation(obs, {});
        assert(skipped.status == BaselineStatus::kOk && !skipped.can_update);
        const auto snapshot = task->QuerySeriesSnapshot(obs.series_key, BaselineSerializationFormat::kJson);
        assert(task->SubmitObservation(obs, {}).status == BaselineStatus::kInvalidArgument);
        obs.bucket_id = 102;
        obs.value = 100.0;
        assert(task->SubmitObservation(obs, {}).status == BaselineStatus::kInvalidArgument);
        assert(task->QuerySeriesSnapshot(obs.series_key, BaselineSerializationFormat::kJson) == snapshot);
        obs.bucket_id = 104;
        assert(task->SubmitObservation(obs, {}).status == BaselineStatus::kOk);
    }
    {
        auto [status, task] =
            env.service->CreateValueTask(SampledValueTaskConfig(), BaselineSerializationFormat::kJson);
        assert(status == BaselineStatus::kOk);
        ValueRollingObservation obs{"ordered-sampled", 100, 100.0, 20};
        assert(task->SubmitObservation(obs, {}).status == BaselineStatus::kOk);
        const auto prediction = task->PredictRolling(obs.series_key, 104);
        obs.bucket_id = 103;
        obs.sample_count = 2;
        const auto skipped = task->SubmitObservation(obs, {});
        assert(skipped.status == BaselineStatus::kOk && skipped.skipped_low_sample_count && !skipped.can_update);
        const auto snapshot = task->QuerySeriesSnapshot(obs.series_key, BaselineSerializationFormat::kJson);
        assert(task->SubmitObservation(obs, {}).status == BaselineStatus::kInvalidArgument);
        obs.bucket_id = 102;
        obs.sample_count = 20;
        assert(task->SubmitObservation(obs, {}).status == BaselineStatus::kInvalidArgument);
        assert(task->QuerySeriesSnapshot(obs.series_key, BaselineSerializationFormat::kJson) == snapshot);
        const auto after = task->PredictRolling(obs.series_key, 104);
        assert(prediction.status == BaselineStatus::kOk && after.status == BaselineStatus::kOk);
        assert(prediction.baseline_mu == after.baseline_mu);
        assert(prediction.baseline_lower == after.baseline_lower);
        assert(prediction.baseline_upper == after.baseline_upper);
        obs.bucket_id = 104;
        assert(task->SubmitObservation(obs, {}).status == BaselineStatus::kOk);
    }
    {
        auto [status, task] = env.service->CreateRatioTask(RatioTaskConfig(), BaselineSerializationFormat::kJson);
        assert(status == BaselineStatus::kOk);
        RatioRollingObservation obs{"ordered-ratio", 100, 50.0, 100.0};
        assert(task->SubmitObservation(obs, {}).status == BaselineStatus::kOk);
        obs.bucket_id = 103;
        obs.numerator = 1.0;
        obs.denominator = 5.0;
        const auto skipped = task->SubmitObservation(obs, {});
        assert(skipped.status == BaselineStatus::kOk && skipped.skipped_low_denominator && !skipped.can_update);
        const auto snapshot = task->QuerySeriesSnapshot(obs.series_key, BaselineSerializationFormat::kJson);
        assert(task->SubmitObservation(obs, {}).status == BaselineStatus::kInvalidArgument);
        obs.bucket_id = 102;
        assert(task->SubmitObservation(obs, {}).status == BaselineStatus::kInvalidArgument);
        assert(task->QuerySeriesSnapshot(obs.series_key, BaselineSerializationFormat::kJson) == snapshot);
    }
    {
        auto [status, task] = env.service->CreateRelationTask(RelationTaskConfig(), BaselineSerializationFormat::kJson);
        assert(status == BaselineStatus::kOk);
        auto obs = BuildRelationObservation(100, 60, 30, 10);
        assert(task->SubmitObservation(obs, {}).status == BaselineStatus::kOk);
        const auto source_snapshot = task->QuerySeriesSnapshot(obs.series_key, BaselineSerializationFormat::kJson);
        const auto task_snapshot = task->QueryTaskSnapshot(BaselineSerializationFormat::kJson);
        const auto duplicate = task->SubmitObservation(obs, {});
        assert(duplicate.status == BaselineStatus::kInvalidArgument);
        assert(duplicate.routed_results.empty() && !duplicate.has_fusion_result);
        obs.bucket_id = 99;
        assert(task->SubmitObservation(obs, {}).status == BaselineStatus::kInvalidArgument);
        assert(task->QuerySeriesSnapshot(obs.series_key, BaselineSerializationFormat::kJson) == source_snapshot);
        assert(task->QueryTaskSnapshot(BaselineSerializationFormat::kJson) == task_snapshot);
        RelationRollingObservation missing;
        missing.series_key = obs.series_key;
        missing.bucket_id = 103;
        (void)task->SubmitObservation(missing, {});
        const auto missing_snapshot = task->QuerySeriesSnapshot(obs.series_key, BaselineSerializationFormat::kJson);
        obs.bucket_id = 103;
        assert(task->SubmitObservation(obs, {}).status == BaselineStatus::kInvalidArgument);
        assert(task->QuerySeriesSnapshot(obs.series_key, BaselineSerializationFormat::kJson) == missing_snapshot);
        obs.bucket_id = 104;
        assert(task->SubmitObservation(obs, {}).status == BaselineStatus::kOk);
        assert(task->Close() == BaselineStatus::kOk);
    }
    {
        auto [status, task] = env.service->CreateRelationTask(RelationTaskConfig(), BaselineSerializationFormat::kJson);
        assert(status == BaselineStatus::kOk);
        auto history = BuildRelationHistory();
        for (auto& block : history.blocks) block.bucket_id -= 10;
        assert(task->Bootstrap(history).status == BaselineStatus::kOk);
        auto obs = BuildRelationObservation(0, 60, 30, 10);
        assert(task->SubmitObservation(obs, {}).status == BaselineStatus::kOk);
        assert(task->SubmitObservation(obs, {}).status == BaselineStatus::kInvalidArgument);
    }
    std::printf("[PASS] review observation order\n");
}

void TestReviewRelationBootstrapIsolation() {
    std::printf("[TEST] review relation bootstrap keeps other sources...\n");
    auto env = LoadBaselineService();
    auto [status, task] = env.service->CreateRelationTask(RelationTaskConfig(), BaselineSerializationFormat::kJson);
    assert(status == BaselineStatus::kOk);
    auto seeded = BuildRelationHistory();
    seeded.series_key = "seeded";
    assert(task->Bootstrap(seeded).status == BaselineStatus::kOk);
    for (const std::string source : {"online", "seeded", "target::child"}) {
        auto obs = BuildRelationObservation(100, 60, 30, 10);
        obs.series_key = source;
        assert(task->SubmitObservation(obs, {}).status == BaselineStatus::kOk);
    }
    std::vector<BaselineSerializationResult> snapshots;
    for (const std::string source : {"online", "seeded", "target::child"})
        snapshots.push_back(task->QuerySeriesSnapshot(source, BaselineSerializationFormat::kJson));
    auto target = BuildRelationHistory();
    target.series_key = "target";
    assert(task->Bootstrap(target).status == BaselineStatus::kOk);
    auto target_obs = BuildRelationObservation(100, 20, 30, 50);
    target_obs.series_key = target.series_key;
    assert(task->SubmitObservation(target_obs, {}).status == BaselineStatus::kOk);
    target.options.force_replace_existing_artifact = true;
    assert(task->Bootstrap(target).status == BaselineStatus::kOk);
    std::size_t i = 0;
    for (const std::string source : {"online", "seeded", "target::child"}) {
        assert(task->QuerySeriesSnapshot(source, BaselineSerializationFormat::kJson) == snapshots[i++]);
        auto obs = BuildRelationObservation(100, 60, 30, 10);
        obs.series_key = source;
        assert(task->SubmitObservation(obs, {}).status == BaselineStatus::kInvalidArgument);
        obs.bucket_id = 101;
        assert(task->SubmitObservation(obs, {}).status == BaselineStatus::kOk);
    }
    assert(task->SubmitObservation(target_obs, {}).status == BaselineStatus::kOk);
    std::printf("[PASS] review relation bootstrap isolation\n");
}

void TestReviewRelationArtifactCompatibility() {
    std::printf("[TEST] review relation artifact compatibility...\n");
    auto env = LoadBaselineService();
    auto [status, task] = env.service->CreateRelationTask(RelationTaskConfig(), BaselineSerializationFormat::kJson);
    assert(status == BaselineStatus::kOk);
    assert(task->Bootstrap(BuildRelationHistory()).status == BaselineStatus::kOk);
    assert(task->SubmitObservation(BuildRelationObservation(100, 60, 30, 10), {}).status == BaselineStatus::kOk);
    const auto snapshot = task->QuerySeriesSnapshot("svc-a", BaselineSerializationFormat::kJson);
    const auto artifact = task->ExportBootstrapArtifact(BaselineSerializationFormat::kJson);
    const auto config = task->ExportConfig(BaselineSerializationFormat::kJson);
    for (const std::string field : {"group_space_id", "group_space_version", "metric", "metric_name", "feature_base"}) {
        std::string incompatible = artifact.second;
        const std::string needle = "\"" + field + "\":\"";
        const auto pos = incompatible.find(needle);
        assert(pos != std::string::npos);
        incompatible.insert(pos + needle.size(), "incompatible-");
        const auto incompatible_status = task->LoadBootstrapArtifact(incompatible, BaselineSerializationFormat::kJson);
        if (incompatible_status != BaselineStatus::kIncompatibleArtifact)
            std::fprintf(stderr, "compatibility field=%s status=%d\n", field.c_str(),
                         static_cast<int>(incompatible_status));
        assert(incompatible_status == BaselineStatus::kIncompatibleArtifact);
        assert(task->ExportBootstrapArtifact(BaselineSerializationFormat::kJson) == artifact);
        assert(task->ExportConfig(BaselineSerializationFormat::kJson) == config);
        assert(task->QuerySeriesSnapshot("svc-a", BaselineSerializationFormat::kJson) == snapshot);
    }
    for (const auto& replacement : std::vector<std::pair<std::string, std::string>>{
             {"\"k_head\":2", "\"k_head\":1"}, {"\"k_head\":2", "\"other_group_idxs\":[99],\"k_head\":2"}}) {
        std::string incompatible = artifact.second;
        const auto pos = incompatible.find(replacement.first);
        assert(pos != std::string::npos);
        incompatible.replace(pos, replacement.first.size(), replacement.second);
        assert(task->LoadBootstrapArtifact(incompatible, BaselineSerializationFormat::kJson) ==
               BaselineStatus::kIncompatibleArtifact);
        assert(task->ExportBootstrapArtifact(BaselineSerializationFormat::kJson) == artifact);
        assert(task->QuerySeriesSnapshot("svc-a", BaselineSerializationFormat::kJson) == snapshot);
    }
    assert(task->LoadBootstrapArtifact(artifact.second, BaselineSerializationFormat::kJson) == BaselineStatus::kOk);
    std::printf("[PASS] review relation artifact compatibility\n");
}

void TestB3RollingResultUsesPreUpdateTrustSnapshot() {
    std::printf("[TEST] B3 rolling result uses pre-update trust snapshot...\n");

    const std::string config_path = "/tmp/flowsql_baseline_b3_pre_update_test.yaml";
    {
        std::ofstream file(config_path);
        file << R"(
baseline:
  rolling_config:
    min_warming_updates: 1
    min_ready_hint_updates: 3
    level_ready_min_updates: 3
    score_warming_min_updates: 4
    score_ready_min_updates: 5
    score_recovery_min_updates: 2
    calibration_warmup_min_updates: 1
)";
        assert(file.good());
    }

    auto env = LoadBaselineService("config_file=" + config_path + ";strict=false");
    auto [status, task] =
        env.service->CreateValueTask(ValueTaskConfig(), BaselineSerializationFormat::kJson);
    assert(status == BaselineStatus::kOk);
    assert(task != nullptr);

    ValueRollingObservation obs;
    obs.series_key = "pre-update-link";
    obs.bucket_id = 100;
    obs.value = 100.0;
    assert(task->SubmitObservation(obs, RollingSubmitOptions{}).status == BaselineStatus::kOk);
    obs.bucket_id = 101;
    assert(task->SubmitObservation(obs, RollingSubmitOptions{}).status == BaselineStatus::kOk);

    obs.bucket_id = 102;
    RollingBaselineResult boundary = task->SubmitObservation(obs, RollingSubmitOptions{});
    assert(boundary.status == BaselineStatus::kOk);
    assert(boundary.maturity_status == "cold_learning");
    assert(boundary.score_trust_status == "score_untrusted");
    assert(!boundary.can_alert);

    auto [snapshot_status, snapshot_json] =
        task->QuerySeriesSnapshot("pre-update-link", BaselineSerializationFormat::kJson);
    assert(snapshot_status == BaselineStatus::kOk);
    rapidjson::Document snapshot;
    snapshot.Parse(snapshot_json.c_str());
    assert(!snapshot.HasParseError());
    assert(std::string(snapshot["maturity_status"].GetString()) != "cold_learning");
    AssertSnapshotMatchesSubmit({snapshot_status, snapshot_json}, boundary);

    std::printf("[PASS] B3 rolling result uses pre-update trust snapshot\n");
}

void TestB6SameTaskSerializedCrossThreadHandoff() {
    std::printf("[TEST] B6 same task serialized cross-thread handoff...\n");

    auto env = LoadBaselineService();
    auto [status, task] =
        env.service->CreateValueTask(ValueTaskConfig(), BaselineSerializationFormat::kJson);
    assert(status == BaselineStatus::kOk);
    assert(task != nullptr);

    std::string task_id;
    std::string task_name;
    BaselineTaskKind task_kind = BaselineTaskKind::kRelation;
    std::thread identity_thread([&]() {
        task_id = task->Id();
        task_name = task->Name();
        task_kind = task->Kind();
    });
    identity_thread.join();
    assert(task_id == "baseline_task_bps");
    assert(task_name == "link bps baseline");
    assert(task_kind == BaselineTaskKind::kValue);

    BootstrapTrainResult train;
    std::thread bootstrap_thread([&]() { train = task->Bootstrap(BuildValueHistory()); });
    bootstrap_thread.join();
    assert(train.status == BaselineStatus::kOk);

    BaselineSerializationResult exported_artifact;
    std::thread export_thread([&]() {
        exported_artifact =
            task->ExportBootstrapArtifact(BaselineSerializationFormat::kJson);
    });
    export_thread.join();
    assert(exported_artifact.first == BaselineStatus::kOk);
    assert(!exported_artifact.second.empty());

    BaselineStatus load_status = BaselineStatus::kParseFailed;
    std::thread load_thread([&]() {
        load_status = task->LoadBootstrapArtifact(
            exported_artifact.second, BaselineSerializationFormat::kJson);
    });
    load_thread.join();
    assert(load_status == BaselineStatus::kOk);

    RollingBaselineResult submit_result;
    std::thread submit_thread([&]() {
        ValueRollingObservation obs;
        obs.series_key = "svc-a";
        obs.bucket_id = 220;
        obs.value = 110.0;
        submit_result = task->SubmitObservation(obs, RollingSubmitOptions{});
    });
    submit_thread.join();
    assert(submit_result.status == BaselineStatus::kOk);
    assert(submit_result.series_key == "svc-a");
    assert(submit_result.bucket_id == 220);

    BaselineSerializationResult series_snapshot;
    std::thread query_thread([&]() {
        series_snapshot =
            task->QuerySeriesSnapshot("svc-a", BaselineSerializationFormat::kJson);
    });
    query_thread.join();
    assert(series_snapshot.first == BaselineStatus::kOk);
    assert(series_snapshot.second.find("\"series_key\":\"svc-a\"") != std::string::npos);

    BaselineSerializationResult config_export;
    std::thread config_thread([&]() {
        config_export = task->ExportConfig(BaselineSerializationFormat::kJson);
    });
    config_thread.join();
    assert(config_export.first == BaselineStatus::kOk);
    assert(config_export.second.find("baseline_task_bps") != std::string::npos);

    BaselineStatus close_status = BaselineStatus::kParseFailed;
    std::thread close_thread([&]() { close_status = task->Close(); });
    close_thread.join();
    assert(close_status == BaselineStatus::kOk);

    RollingPrediction after_close_prediction;
    std::thread after_close_thread([&]() {
        after_close_prediction = task->PredictRolling("svc-a", 221);
    });
    after_close_thread.join();
    assert(after_close_prediction.status == BaselineStatus::kInvalidArgument);

    std::printf("[PASS] B6 same task serialized cross-thread handoff\n");
}

void TestB6RelationSerializedLifecycleSequence() {
    std::printf("[TEST] B6 relation serialized lifecycle sequence...\n");

    auto env = LoadBaselineService();
    auto [status, task] =
        env.service->CreateRelationTask(RelationTaskConfig(),
                                        BaselineSerializationFormat::kJson);
    assert(status == BaselineStatus::kOk);
    assert(task != nullptr);

    RelationRollingSubmitOptions options;
    RelationRollingResult cold_submit;
    std::thread cold_submit_thread([&]() {
        cold_submit =
            task->SubmitObservation(BuildRelationObservation(100, 60, 30, 10), options);
    });
    cold_submit_thread.join();
    assert(cold_submit.status == BaselineStatus::kOk);
    assert(cold_submit.series_key == "svc-a");
    assert(!cold_submit.routed_results.empty());

    BootstrapTrainResult train;
    std::thread bootstrap_thread([&]() { train = task->Bootstrap(BuildRelationHistory()); });
    bootstrap_thread.join();
    assert(train.status == BaselineStatus::kOk);

    BaselineSerializationResult artifact;
    std::thread export_thread([&]() {
        artifact = task->ExportBootstrapArtifact(BaselineSerializationFormat::kJson);
    });
    export_thread.join();
    assert(artifact.first == BaselineStatus::kOk);
    assert(artifact.second.find("\"document_kind\":\"bootstrap_artifact\"") !=
           std::string::npos);

    BaselineStatus load_status = BaselineStatus::kParseFailed;
    std::thread load_thread([&]() {
        load_status = task->LoadBootstrapArtifact(
            artifact.second, BaselineSerializationFormat::kJson);
    });
    load_thread.join();
    assert(load_status == BaselineStatus::kOk);

    RelationRollingResult seeded_submit;
    std::thread seeded_submit_thread([&]() {
        seeded_submit =
            task->SubmitObservation(BuildRelationObservation(20, 50, 25, 25), options);
    });
    seeded_submit_thread.join();
    assert(seeded_submit.status == BaselineStatus::kOk);
    bool has_basis_scoped_routed = false;
    for (const auto& routed : seeded_submit.routed_results) {
        if (routed.basis_scoped) has_basis_scoped_routed = true;
    }
    assert(has_basis_scoped_routed);

    BaselineSerializationResult series_snapshot;
    std::thread snapshot_thread([&]() {
        series_snapshot =
            task->QuerySeriesSnapshot("svc-a", BaselineSerializationFormat::kJson);
    });
    snapshot_thread.join();
    assert(series_snapshot.first == BaselineStatus::kOk);
    rapidjson::Document snapshot_doc;
    snapshot_doc.Parse(series_snapshot.second.c_str());
    assert(!snapshot_doc.HasParseError());
    assert(std::string(snapshot_doc["document_kind"].GetString()) ==
           "relation_series_snapshot");
    assert(snapshot_doc["routed_summaries"].Size() > 0);

    BaselineStatus close_status = BaselineStatus::kParseFailed;
    std::thread close_thread([&]() { close_status = task->Close(); });
    close_thread.join();
    assert(close_status == BaselineStatus::kOk);

    RelationRollingResult after_close;
    std::thread after_close_thread([&]() {
        after_close =
            task->SubmitObservation(BuildRelationObservation(21, 50, 25, 25), options);
    });
    after_close_thread.join();
    assert(after_close.status == BaselineStatus::kInvalidArgument);

    std::printf("[PASS] B6 relation serialized lifecycle sequence\n");
}

void TestB6DifferentTasksMayRunInParallel() {
    std::printf("[TEST] B6 different tasks may run in parallel...\n");

    auto env = LoadBaselineService();
    auto [value_status, value_task] =
        env.service->CreateValueTask(ValueTaskConfig(), BaselineSerializationFormat::kJson);
    assert(value_status == BaselineStatus::kOk);
    assert(value_task != nullptr);
    auto [relation_status, relation_task] =
        env.service->CreateRelationTask(RelationTaskConfig(),
                                        BaselineSerializationFormat::kJson);
    assert(relation_status == BaselineStatus::kOk);
    assert(relation_task != nullptr);

    std::atomic<int> ready{0};
    std::atomic<bool> start{false};
    RollingBaselineResult value_result;
    RelationRollingResult relation_result;

    std::thread value_thread([&]() {
        ready.fetch_add(1, std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        ValueRollingObservation obs;
        obs.series_key = "parallel-value";
        obs.bucket_id = 100;
        obs.value = 100.0;
        value_result = value_task->SubmitObservation(obs, RollingSubmitOptions{});
    });
    std::thread relation_thread([&]() {
        ready.fetch_add(1, std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        RelationRollingSubmitOptions options;
        relation_result = relation_task->SubmitObservation(
            BuildRelationObservation(100, 60, 30, 10), options);
    });

    while (ready.load(std::memory_order_acquire) < 2) std::this_thread::yield();
    start.store(true, std::memory_order_release);
    value_thread.join();
    relation_thread.join();

    assert(value_result.status == BaselineStatus::kOk);
    assert(value_result.series_key == "parallel-value");
    assert(relation_result.status == BaselineStatus::kOk);
    assert(relation_result.series_key == "svc-a");

    auto [snapshot_status, snapshot_json] =
        env.service->QueryServiceSnapshot(BaselineSerializationFormat::kJson);
    assert(snapshot_status == BaselineStatus::kOk);
    rapidjson::Document snapshot;
    snapshot.Parse(snapshot_json.c_str());
    assert(!snapshot.HasParseError());
    assert(snapshot["task_count"].GetUint64() == 2);

    std::printf("[PASS] B6 different tasks may run in parallel\n");
}

}  // namespace

int main() {
    TestManagedRelationImportVersionLimit();
    TestManagedStateLifecycleAndCapacity();
    TestManagedRelationVersionRetirement();
    TestStateControlBindingContract();
    TestEventCalendarSchemaRejectsTaskScopedFields();
    TestCreateTaskUsesConfigIdentity();
    TestInvalidTaskConfigRejected();
    TestTaskBootstrapPredictAndExport();
    TestValueTaskKeepsBootstrapPerSeriesAndExportsAll();
    TestBootstrapUsesConfiguredEventCalendar();
    TestReviewSnapshotTracksLastSubmit();
    TestReviewSnapshotUsesDetectionConfiguration();
    TestReviewRollingConsumesEventHint();
    TestTaskBoundEventCalendarPreservesDstAndTaskTimezone();
    TestTaskRejectsIncompatibleBootstrapArtifact();
    TestRuntimeConfigDefaultDailyHarmonicOrder();
    TestRuntimeConfigHarmonicOrders();
    TestB2ValueRollingEmptyStart();
    TestB2SampledValueLowSupportStart();
    TestB2RatioRollingInputAndEmptyStart();
    TestB2RollingSnapshotAndBootstrapWarmup();
    TestB2RollingPredict();
    TestB2RollingFailureSemantics();
    TestReviewObservationOrder();
    TestReviewObservationValidation();
    TestReviewValueBootstrapRuntimeReplacement();
    TestReviewRatioBootstrapRuntimeReplacement();
    TestReviewValueIdentityTransform();
    TestReviewRatioConfiguredClip();
    TestReviewRelationBootstrapIsolation();
    TestReviewRelationArtifactCompatibility();
    TestB3RollingResultUsesPreUpdateTrustSnapshot();
    TestB6SameTaskSerializedCrossThreadHandoff();
    TestB6RelationSerializedLifecycleSequence();
    TestB6DifferentTasksMayRunInParallel();
    TestB4RelationSubmitObservation();
    TestB4RelationRollingConfigSwitches();
    TestB5RelationFusionSubmitAndSnapshot();
    TestB7RelationFusionStateCleanup();
    TestB7RelationFusionStateCapacityCleanup();
    return 0;
}
