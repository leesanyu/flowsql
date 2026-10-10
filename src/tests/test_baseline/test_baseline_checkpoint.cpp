// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <dlfcn.h>
#include <framework/interfaces/ibaseline_checkpoint.h>
#include <openssl/sha.h>
#include <plugins/baseline/baseline_plugin.h>
#include <plugins/baseline/checkpoint/state_codec.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <cassert>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <set>
#include <vector>
using namespace flowsql;
namespace {
constexpr auto value_config =
    R"({"schema_version":1,"task_id":"value-checkpoint","task_name":"value checkpoint","task_kind":"value","feature_id":"v","feature_type":"value_basic","profile":"default","clock_spec":{"bucket_seconds":60,"timezone":"UTC"},"calendar_ref":{"calendar_id":"cn-holiday","calendar_version":"2026.1"}})";
constexpr auto ratio_config =
    R"({"schema_version":1,"task_id":"ratio-checkpoint","task_name":"ratio checkpoint","task_kind":"ratio","feature_id":"r","feature_type":"ratio","profile":"rate_core","clock_spec":{"bucket_seconds":60,"timezone":"UTC"},"calendar_ref":{"calendar_id":"cn-holiday","calendar_version":"2026.1"}})";
struct Environment {
    baseline::BaselinePlugin plugin;
    explicit Environment(const char* option = nullptr) {
        assert(plugin.Option(option) == 0 && plugin.Load(nullptr) == 0 && plugin.Start() == 0);
    }
    ~Environment() {
        plugin.Stop();
        plugin.Unload();
    }
};
std::shared_ptr<IBaselineCheckpointV1> Bind(Environment& env, std::shared_ptr<IBaselineTask> task,
                                            uint64_t max_bytes = 1 << 24, BaselineStateLimitsV1 limits = {8, 8, 2}) {
    auto controlled = env.plugin.Bind(task, limits);
    assert(controlled.first == BaselineStatus::kOk);
    BaselineCheckpointBindingV1 binding;
    binding.max_payload_bytes = max_bytes;
    auto checkpoint = env.plugin.Bind(task, controlled.second, binding);
    assert(checkpoint.first == BaselineStatus::kOk && checkpoint.second);
    return checkpoint.second;
}
void Equal(const RollingBaselineResult& a, const RollingBaselineResult& b) {
    assert(baseline::checkpoint::Encode(a, 1 << 24) == baseline::checkpoint::Encode(b, 1 << 24));
}
std::string Snapshot(const std::shared_ptr<IBaselineTask>& task) {
    const auto result = task->QuerySeriesSnapshot("series", BaselineSerializationFormat::kJson);
    assert(result.first == BaselineStatus::kOk);
    rapidjson::Document doc;
    doc.Parse<rapidjson::kParseFullPrecisionFlag>(result.second.data(), result.second.size());
    assert(!doc.HasParseError());
    // Allocator capacity is diagnostic and may differ after materializing the same state.
    doc.RemoveMember("state_size_bytes");
    rapidjson::StringBuffer out;
    rapidjson::Writer<rapidjson::StringBuffer> writer(out);
    doc.Accept(writer);
    return {out.GetString(), out.GetSize()};
}
void Roundtrip(bool ratio) {
    Environment source, target;
    auto value =
        ratio ? nullptr : source.plugin.CreateValueTask(value_config, BaselineSerializationFormat::kJson).second;
    auto rate =
        ratio ? source.plugin.CreateRatioTask(ratio_config, BaselineSerializationFormat::kJson).second : nullptr;
    auto task = ratio ? std::static_pointer_cast<IBaselineTask>(rate) : std::static_pointer_cast<IBaselineTask>(value);
    auto checkpoint = Bind(source, task);
    for (int64_t t = 0; t < 80; ++t) {
        if (ratio)
            rate->SubmitObservation({"series", t, t == 70 ? 1. : 800., 1000}, {});
        else
            value->SubmitObservation({"series", t, t == 70 ? 1e100 : 100. + t % 3, 1}, {});
    }
    const auto exported = checkpoint->ExportCheckpoint(BaselineSerializationFormat::kJson);
    assert(exported.first == BaselineStatus::kOk && !exported.second.empty());
    auto restored_value =
        ratio ? nullptr : target.plugin.CreateValueTask(value_config, BaselineSerializationFormat::kJson).second;
    auto restored_rate =
        ratio ? target.plugin.CreateRatioTask(ratio_config, BaselineSerializationFormat::kJson).second : nullptr;
    auto restored_task = ratio ? std::static_pointer_cast<IBaselineTask>(restored_rate)
                               : std::static_pointer_cast<IBaselineTask>(restored_value);
    auto restored = Bind(target, restored_task);
    BaselineCheckpointRestoreV1 restore;
    restore.content = exported.second;
    assert(restored->RestoreCheckpoint(restore) == BaselineStatus::kOk);
    assert(Snapshot(task) == Snapshot(restored_task));
    assert(exported == restored->ExportCheckpoint(BaselineSerializationFormat::kJson));
    auto predictions = ratio ? rate->PredictRolling("series", 80, 4) : value->PredictRolling("series", 80, 4);
    auto recovered_predictions =
        ratio ? restored_rate->PredictRolling("series", 80, 4) : restored_value->PredictRolling("series", 80, 4);
    assert(predictions.status == recovered_predictions.status && predictions.predictions.size() == 4);
    for (size_t i = 0; i < 4; ++i) {
        const auto& a = predictions.predictions[i];
        const auto& b = recovered_predictions.predictions[i];
        assert(a.status == b.status && a.baseline_mu == b.baseline_mu && a.baseline_lower == b.baseline_lower &&
               a.baseline_upper == b.baseline_upper);
    }
    assert(exported == checkpoint->ExportCheckpoint(BaselineSerializationFormat::kJson));
    assert(restored->RestoreCheckpoint(restore) != BaselineStatus::kOk);
    for (int64_t t = 80; t < 100; ++t) {
        Equal(ratio ? rate->SubmitObservation({"series", t, 800, 1000}, {})
                    : value->SubmitObservation({"series", t, 100, 1}, {}),
              ratio ? restored_rate->SubmitObservation({"series", t, 800, 1000}, {})
                    : restored_value->SubmitObservation({"series", t, 100, 1}, {}));
        assert(Snapshot(task) == Snapshot(restored_task));
    }
}
std::string Mutate(const std::string& text, const std::function<void(rapidjson::Document&)>& mutate,
                   bool resign = false) {
    rapidjson::Document doc;
    doc.Parse<rapidjson::kParseFullPrecisionFlag>(text.data(), text.size());
    assert(!doc.HasParseError());
    mutate(doc);
    if (resign) {
        rapidjson::StringBuffer payload;
        rapidjson::Writer<rapidjson::StringBuffer> writer(payload);
        doc["payload"].Accept(writer);
        unsigned char bytes[SHA256_DIGEST_LENGTH];
        SHA256(reinterpret_cast<const unsigned char*>(payload.GetString()), payload.GetSize(), bytes);
        constexpr char digits[] = "0123456789abcdef";
        std::string digest;
        for (unsigned char c : bytes) {
            digest += digits[c >> 4];
            digest += digits[c & 15];
        }
        doc["payload_bytes"].SetUint64(payload.GetSize());
        doc["sha256"].SetString(digest.data(), digest.size(), doc.GetAllocator());
    }
    rapidjson::StringBuffer out;
    rapidjson::Writer<rapidjson::StringBuffer> writer(out);
    doc.Accept(writer);
    return {out.GetString(), out.GetSize()};
}
void RejectAndRetry() {
    Environment source, target;
    auto v = source.plugin.CreateValueTask(value_config, BaselineSerializationFormat::kJson).second;
    auto cp = Bind(source, v);
    for (int64_t t = 0; t < 12; ++t)
        assert(v->SubmitObservation({"series", t, 100, 1}, {}).status == BaselineStatus::kOk);
    auto exported = cp->ExportCheckpoint(BaselineSerializationFormat::kJson);
    assert(exported.first == BaselineStatus::kOk);
    auto recovered = target.plugin.CreateValueTask(value_config, BaselineSerializationFormat::kJson).second;
    auto dest = Bind(target, recovered);
    const auto before = dest->ExportCheckpoint(BaselineSerializationFormat::kJson);
    assert(before.first == BaselineStatus::kOk);
    std::vector<std::string> invalid = {"{}", exported.second.substr(0, exported.second.size() / 2)};
    invalid.push_back(Mutate(exported.second, [](auto& d) { d["payload_version"].SetUint(2); }));
    invalid.push_back(Mutate(exported.second, [](auto& d) { d["schema_version"].SetUint(2); }));
    invalid.push_back(
        Mutate(exported.second, [](auto& d) { d["algorithm_version"].SetString("other", d.GetAllocator()); }));
    invalid.push_back(Mutate(exported.second, [](auto& d) { d["payload_bytes"].SetUint64(1); }));
    invalid.push_back(Mutate(exported.second, [](auto& d) { d["sha256"].SetString("bad", d.GetAllocator()); }));
    invalid.push_back(
        Mutate(exported.second, [](auto& d) { d["payload"]["rolling"][0][1]["sigma"].SetDouble(-1); }, true));
    invalid.push_back(Mutate(
        exported.second, [](auto& d) { d["payload"]["rolling"][0][1]["theta"]["daily"]["sin_coeff"].SetArray(); },
        true));
    invalid.push_back(
        Mutate(exported.second, [](auto& d) { d["payload"]["rolling"][0][1]["maturity_status"].SetInt(999); }, true));
    invalid.push_back(Mutate(
        exported.second, [](auto& d) { d["payload"]["compatibility"]["rolling"]["band_z"].SetDouble(99); }, true));
    invalid.push_back(Mutate(
        exported.second,
        [](auto& d) { d["payload"].AddMember("rolling", rapidjson::Value(rapidjson::kArrayType), d.GetAllocator()); },
        true));
    for (const auto& content : invalid) {
        BaselineCheckpointRestoreV1 request;
        request.content = content;
        assert(dest->RestoreCheckpoint(request) != BaselineStatus::kOk);
        assert(before == dest->ExportCheckpoint(BaselineSerializationFormat::kJson));
    }
    BaselineCheckpointRestoreV1 request;
    request.content = exported.second;
    request.payload_version = 2;
    assert(dest->RestoreCheckpoint(request) != BaselineStatus::kOk);
    request.payload_version = 1;
    request.struct_size = 0;
    assert(dest->RestoreCheckpoint(request) != BaselineStatus::kOk);
    request.struct_size = sizeof(request);
    request.contract_version = 2;
    assert(dest->RestoreCheckpoint(request) != BaselineStatus::kOk);
    request.contract_version = 1;
    assert(before == dest->ExportCheckpoint(BaselineSerializationFormat::kJson));
    assert(dest->RestoreCheckpoint(request) == BaselineStatus::kOk);
    assert(exported == dest->ExportCheckpoint(BaselineSerializationFormat::kJson));
    recovered->Close();
    assert(dest->ExportCheckpoint(BaselineSerializationFormat::kJson).first != BaselineStatus::kOk);
    assert(dest->RestoreCheckpoint(request) != BaselineStatus::kOk);
}
void CapacityAndBinding() {
    Environment source, target;
    auto v = source.plugin.CreateValueTask(value_config, BaselineSerializationFormat::kJson).second;
    auto r = source.plugin.CreateRatioTask(ratio_config, BaselineSerializationFormat::kJson).second;
    auto c1 = source.plugin.Bind(v, {8, 8, 2}).second;
    auto c2 = source.plugin.Bind(r, {8, 8, 2}).second;
    BaselineCheckpointBindingV1 binding;
    assert(source.plugin.Bind(v, c2, binding).first != BaselineStatus::kOk);
    assert(target.plugin.Bind(v, c1, binding).first != BaselineStatus::kOk);
    binding.max_payload_bytes = 0;
    assert(source.plugin.Bind(v, c1, binding).first != BaselineStatus::kOk);
    binding.max_payload_bytes = 1 << 24;
    binding.contract_version = 2;
    assert(source.plugin.Bind(v, c1, binding).first != BaselineStatus::kOk);
    binding.contract_version = 1;
    auto cp = source.plugin.Bind(v, c1, binding).second;
    assert(cp);
    assert(!source.plugin.Bind(v, c1, binding).second);
    const auto empty = cp->ExportCheckpoint(BaselineSerializationFormat::kJson);
    assert(empty.first == BaselineStatus::kOk);
    for (const char* id : {"first", "second"}) v->SubmitObservation({id, 0, 100, 1}, {});
    const auto full = cp->ExportCheckpoint(BaselineSerializationFormat::kJson);
    assert(full.first == BaselineStatus::kOk);
    auto dest_task = target.plugin.CreateValueTask(value_config, BaselineSerializationFormat::kJson).second;
    auto dest = Bind(target, dest_task, 1 << 24, {1, 1, 2});
    const auto before = dest->ExportCheckpoint(BaselineSerializationFormat::kJson);
    BaselineCheckpointRestoreV1 request;
    request.content = full.second;
    assert(dest->RestoreCheckpoint(request) != BaselineStatus::kOk);
    assert(before == dest->ExportCheckpoint(BaselineSerializationFormat::kJson));
    request.content = empty.second;
    assert(dest->RestoreCheckpoint(request) == BaselineStatus::kOk);
    assert(dest->RestoreCheckpoint(request) != BaselineStatus::kOk);
    dest_task->Close();
    dest.reset();
    dest_task.reset();
    dest_task = target.plugin.CreateValueTask(value_config, BaselineSerializationFormat::kJson).second;
    auto tiny = Bind(target, dest_task, 64);
    assert(tiny->ExportCheckpoint(BaselineSerializationFormat::kJson).first == BaselineStatus::kSerializationFailed);
    assert(tiny->RestoreCheckpoint(request) != BaselineStatus::kOk);
    assert(v->PredictRolling("first", 1).status == BaselineStatus::kOk);
}
void EligibilityAndCompatibility() {
    Environment source, target;
    auto task = source.plugin.CreateValueTask(value_config, BaselineSerializationFormat::kJson).second;
    auto cp = Bind(source, task);
    task->SubmitObservation({"series", 0, 100, 1}, {});
    BaselineCheckpointRestoreV1 request;
    const auto exported = cp->ExportCheckpoint(BaselineSerializationFormat::kJson);
    request.content = exported.second;
    for (const auto& edit :
         {std::make_pair(std::string("\"bucket_seconds\":60"), std::string("\"bucket_seconds\":120")),
          std::make_pair(std::string("\"timezone\":\"UTC\""), std::string("\"timezone\":\"Asia/Shanghai\"")),
          std::make_pair(std::string("2026.1"), std::string("2027.1"))}) {
        std::string config(value_config);
        config.replace(config.find(edit.first), edit.first.size(), edit.second);
        auto changed = target.plugin.CreateValueTask(config, BaselineSerializationFormat::kJson);
        if (changed.first != BaselineStatus::kOk) {
            // A missing calendar version is rejected by task creation itself.
            assert(edit.second == "2027.1");
            continue;
        }
        auto dest = Bind(target, changed.second);
        const auto before = dest->ExportCheckpoint(BaselineSerializationFormat::kJson);
        assert(dest->RestoreCheckpoint(request) != BaselineStatus::kOk);
        assert(before == dest->ExportCheckpoint(BaselineSerializationFormat::kJson));
        changed.second->Close();
    }
    auto dest_task = target.plugin.CreateValueTask(value_config, BaselineSerializationFormat::kJson).second;
    auto control = target.plugin.Bind(dest_task, {8, 8, 2}).second;
    assert(control);
    assert(control->ReleaseIdentity("absent", BaselineStateReleaseScopeV1::kAllState) == BaselineStatus::kOk);
    assert(target.plugin.Bind(dest_task, control, {}).first != BaselineStatus::kOk);
    dest_task->Close();
    dest_task = target.plugin.CreateValueTask(value_config, BaselineSerializationFormat::kJson).second;
    auto dest = Bind(target, dest_task);
    dest_task->SubmitObservation({"series", 0, 100, 1}, {});
    const auto before = dest->ExportCheckpoint(BaselineSerializationFormat::kJson);
    assert(dest->RestoreCheckpoint(request) != BaselineStatus::kOk);
    assert(before == dest->ExportCheckpoint(BaselineSerializationFormat::kJson));
}
void LowSupportRoundtrip(bool ratio) {
    Environment source, target;
    auto v = ratio ? nullptr : source.plugin.CreateValueTask(value_config, BaselineSerializationFormat::kJson).second;
    auto r = ratio ? source.plugin.CreateRatioTask(ratio_config, BaselineSerializationFormat::kJson).second : nullptr;
    auto task = ratio ? std::static_pointer_cast<IBaselineTask>(r) : std::static_pointer_cast<IBaselineTask>(v);
    auto cp = Bind(source, task);
    for (int64_t t = 0; t < 6; ++t) {
        if (ratio)
            r->SubmitObservation({"series", t, 1, t == 0 ? 1000. : 1.}, {});
        else
            v->SubmitObservation({"series", t, 100, t == 0 ? 1u : 0u}, {});
    }
    auto dv = ratio ? nullptr : target.plugin.CreateValueTask(value_config, BaselineSerializationFormat::kJson).second;
    auto dr = ratio ? target.plugin.CreateRatioTask(ratio_config, BaselineSerializationFormat::kJson).second : nullptr;
    auto dt = ratio ? std::static_pointer_cast<IBaselineTask>(dr) : std::static_pointer_cast<IBaselineTask>(dv);
    auto dest = Bind(target, dt);
    auto payload = cp->ExportCheckpoint(BaselineSerializationFormat::kJson);
    BaselineCheckpointRestoreV1 request;
    request.content = payload.second;
    assert(dest->RestoreCheckpoint(request) == BaselineStatus::kOk);
    assert(payload == dest->ExportCheckpoint(BaselineSerializationFormat::kJson));
    for (int64_t t : {5, 4, 6}) {
        Equal(
            ratio ? r->SubmitObservation({"series", t, 1, 1}, {}) : v->SubmitObservation({"series", t, 100, 0}, {}),
            ratio ? dr->SubmitObservation({"series", t, 1, 1}, {}) : dv->SubmitObservation({"series", t, 100, 0}, {}));
        assert(cp->ExportCheckpoint(BaselineSerializationFormat::kJson) ==
               dest->ExportCheckpoint(BaselineSerializationFormat::kJson));
    }
}
constexpr auto relation_config =
    R"({"schema_version":1,"task_id":"relation-checkpoint","task_name":"relation checkpoint","task_kind":"relation","feature_id":"dist","feature_type":"relation","profile":"default","group_space_id":"groups","group_space_version":"v1","metrics":["m2","m"],"support_policy":{"k_support":2,"min_hist_share":0.01,"min_active_ratio":0.1},"summary_policy":{"k_head":2,"k_stable":1},"clock_spec":{"bucket_seconds":60,"timezone":"UTC"},"calendar_ref":{"calendar_id":"cn-holiday","calendar_version":"2026.1"}})";
RelationRollingObservation RelationObservation(int64_t t, std::string source = "source") {
    const uint32_t phase = static_cast<uint32_t>(t / 3);
    const double scale = std::pow(20., phase);
    RelationRollingObservation o;
    o.series_key = std::move(source);
    o.bucket_id = t;
    o.group_idx = {1 + phase * 3, 2 + phase * 3, 3 + phase * 3};
    o.metrics = {{"m2", 100 * scale, 2, {80 * scale, 20 * scale, 0}},
                 {"m", 200 * scale, 2, {160 * scale, 40 * scale, 0}}};
    return o;
}
std::string RelationOptions() {
    const auto path = "/tmp/baseline-operator-t5/relation-fast.yaml";
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
    return std::string("config_file=") + path + ";strict=false";
}
void RelationRoundtrip() {
    const auto options = RelationOptions();
    Environment source(options.c_str()), target(options.c_str());
    auto task = source.plugin.CreateRelationTask(relation_config, BaselineSerializationFormat::kJson).second;
    auto cp = Bind(source, task);
    std::set<uint64_t> versions;
    bool handover = false;
    RelationRollingResult last;
    const std::string key("binary::source\0tail", 19);
    for (int64_t t = 0; t < 16; ++t) {
        const auto result = task->SubmitObservation(RelationObservation(t, key), {});
        assert(result.status == BaselineStatus::kOk);
        handover |= result.handover_active;
        last = result;
        for (const auto& row : result.routed_results)
            if (row.basis_scoped) versions.insert(row.basis_version);
    }
    assert(handover && versions.size() > 2);
    const auto exported = cp->ExportCheckpoint(BaselineSerializationFormat::kJson);
    assert(exported.first == BaselineStatus::kOk);
    auto dt = target.plugin.CreateRelationTask(relation_config, BaselineSerializationFormat::kJson).second;
    auto dest = Bind(target, dt);
    BaselineCheckpointRestoreV1 request;
    request.content = exported.second;
    assert(dest->RestoreCheckpoint(request) == BaselineStatus::kOk);
    assert(exported == dest->ExportCheckpoint(BaselineSerializationFormat::kJson));
    assert(last.handover_active);
    for (const auto& row : last.routed_results) {
        const RelationRoutedSummaryQuery query{key, row.metric, row.summary, row.feature_type, row.basis_version};
        for (int64_t bucket : {16, 17, 18}) {
            const auto a = task->PredictRoutedSummary(query, bucket), b = dt->PredictRoutedSummary(query, bucket);
            assert(a.status == b.status && a.baseline_mu == b.baseline_mu && a.baseline_lower == b.baseline_lower &&
                   a.baseline_upper == b.baseline_upper && a.band_z == b.band_z);
        }
    }
    assert(exported == cp->ExportCheckpoint(BaselineSerializationFormat::kJson));
    assert(exported == dest->ExportCheckpoint(BaselineSerializationFormat::kJson));
    for (int64_t t : {15, 14, 16, 17, 18, 19, 20, 21, 22, 23}) {
        auto a = task->SubmitObservation(RelationObservation(t, key), {});
        auto b = dt->SubmitObservation(RelationObservation(t, key), {});
        assert(baseline::checkpoint::Encode(a, 1 << 24) == baseline::checkpoint::Encode(b, 1 << 24));
        assert(cp->ExportCheckpoint(BaselineSerializationFormat::kJson) ==
               dest->ExportCheckpoint(BaselineSerializationFormat::kJson));
    }
}
void EqualRelation(const RelationRollingResult& a, const RelationRollingResult& b) {
    assert(baseline::checkpoint::Encode(a, 1 << 24) == baseline::checkpoint::Encode(b, 1 << 24));
}
void RelationModelsAndRelease(std::optional<BaselineStateReleaseScopeV1> scope) {
    Environment source, target;
    auto task = source.plugin.CreateRelationTask(relation_config, BaselineSerializationFormat::kJson).second;
    auto control = source.plugin.Bind(task, {8, 8, 2}).second;
    auto cp = source.plugin.Bind(task, control, {}).second;
    RelationBootstrapInput input;
    input.series_key = "history";
    input.options.min_observation_count = 100;
    for (int64_t t = 0; t < 200; ++t) {
        RelationBootstrapBlock block;
        block.bucket_id = t;
        block.group_idx = {1, 2, 3};
        block.metrics = {{"m2", 100, 2, {80, 20, 0}}, {"m", 200, 2, {160, 40, 0}}};
        input.blocks.push_back(std::move(block));
    }
    assert(task->Bootstrap(input).status == BaselineStatus::kOk);
    for (int64_t t = 200; t < 220; ++t) {
        auto observation = RelationObservation(t, "history");
        observation.group_idx = {1, 2, 3};
        observation.metrics = {{"m2", 100, 2, {80, 20, 0}}, {"m", 200, 2, {160, 40, 0}}};
        assert(task->SubmitObservation(observation, {}).status == BaselineStatus::kOk);
    }
    if (scope) assert(control->ReleaseIdentity("history", *scope) == BaselineStatus::kOk);
    const auto exported = cp->ExportCheckpoint(BaselineSerializationFormat::kJson);
    assert(exported.first == BaselineStatus::kOk);
    auto dt = target.plugin.CreateRelationTask(relation_config, BaselineSerializationFormat::kJson).second;
    auto dc = target.plugin.Bind(dt, {8, 8, 2}).second;
    auto dest = target.plugin.Bind(dt, dc, {}).second;
    BaselineCheckpointRestoreV1 request;
    request.content = exported.second;
    if (!scope) {
        const auto before = dest->ExportCheckpoint(BaselineSerializationFormat::kJson);
        for (auto mutate : std::vector<std::function<void(rapidjson::Document&)>>{
                 [](auto& d) {
                     d["payload"]["native"]["seeds"][0][1]["relation_basis_by_metric"][0]["basis_version"].SetUint(
                         9999);
                 },
                 [](auto& d) {
                     bool changed = false;
                     for (auto& model :
                          d["payload"]["native"]["artifacts"][0][1]["relation_routed_summary_artifacts"].GetArray())
                         if (!model["value_model"].IsNull()) {
                             model["value_model"]["core_block"]["day_cos"].SetArray();
                             changed = true;
                             break;
                         }
                     assert(changed);
                 },
                 [](auto& d) {
                     d["payload"]["native"]["artifacts"][0][1]["relation_basis_by_metric"][0]["head_proto_q"]
                         .SetArray();
                 }}) {
            auto text = Mutate(exported.second, mutate, true);
            BaselineCheckpointRestoreV1 bad;
            bad.content = text;
            assert(dest->RestoreCheckpoint(bad) != BaselineStatus::kOk);
            assert(before == dest->ExportCheckpoint(BaselineSerializationFormat::kJson));
        }
    }
    assert(dest->RestoreCheckpoint(request) == BaselineStatus::kOk);
    assert(exported == dest->ExportCheckpoint(BaselineSerializationFormat::kJson));
    assert(task->ExportBootstrapArtifact(BaselineSerializationFormat::kJson) ==
           dt->ExportBootstrapArtifact(BaselineSerializationFormat::kJson));
    assert(task->ExportBootstrapSeed(BaselineSerializationFormat::kJson) ==
           dt->ExportBootstrapSeed(BaselineSerializationFormat::kJson));
    assert(task->QueryBootstrapBasis(BaselineSerializationFormat::kJson) ==
           dt->QueryBootstrapBasis(BaselineSerializationFormat::kJson));
    const auto a = control->QueryUsage().second, b = dc->QueryUsage().second;
    assert(a.runtime_identities == b.runtime_identities && a.model_identities == b.model_identities &&
           a.routed_states == b.routed_states && a.retained_basis_versions == b.retained_basis_versions);
    for (int64_t t = 220; t < 225; ++t) {
        auto obs = RelationObservation(t, "history");
        obs.group_idx = {1, 2, 3};
        obs.metrics = {{"m2", 100, 2, {80, 20, 0}}, {"m", 200, 2, {160, 40, 0}}};
        EqualRelation(task->SubmitObservation(obs, {}), dt->SubmitObservation(obs, {}));
        assert(cp->ExportCheckpoint(BaselineSerializationFormat::kJson) ==
               dest->ExportCheckpoint(BaselineSerializationFormat::kJson));
    }
    for (auto released : {BaselineStateReleaseScopeV1::kRuntimeOnly, BaselineStateReleaseScopeV1::kAllState}) {
        assert(control->ReleaseIdentity("history", released) == BaselineStatus::kOk &&
               dc->ReleaseIdentity("history", released) == BaselineStatus::kOk);
        assert(cp->ExportCheckpoint(BaselineSerializationFormat::kJson) ==
               dest->ExportCheckpoint(BaselineSerializationFormat::kJson));
    }
}
void RelationRejectAndRetry() {
    const auto options = RelationOptions();
    Environment source(options.c_str()), target(options.c_str());
    auto task = source.plugin.CreateRelationTask(relation_config, BaselineSerializationFormat::kJson).second;
    auto cp = Bind(source, task);
    for (int64_t t = 0; t < 16; ++t)
        assert(task->SubmitObservation(RelationObservation(t), {}).status == BaselineStatus::kOk);
    const auto exported = cp->ExportCheckpoint(BaselineSerializationFormat::kJson);
    assert(exported.first == BaselineStatus::kOk);
    auto dt = target.plugin.CreateRelationTask(relation_config, BaselineSerializationFormat::kJson).second;
    auto dest = Bind(target, dt);
    const auto empty = dest->ExportCheckpoint(BaselineSerializationFormat::kJson);
    auto invalid = std::vector<std::string>{};
    invalid.push_back(Mutate(
        exported.second, [](auto& d) { d["payload"]["managed"][0][1]["routed"][0]["shard"].SetUint(9999); }, true));
    invalid.push_back(Mutate(
        exported.second, [](auto& d) { d["payload"]["managed"][0][1]["routed"][0]["metric_index"].SetUint(9999); },
        true));
    invalid.push_back(Mutate(
        exported.second,
        [](auto& d) { d["payload"]["managed"][0][1]["routed"][0]["key"].SetString("", d.GetAllocator()); }, true));
    invalid.push_back(Mutate(
        exported.second, [](auto& d) { d["payload"]["managed"][0][1]["versions"][0].PushBack(9999, d.GetAllocator()); },
        true));
    invalid.push_back(Mutate(
        exported.second,
        [](auto& d) { d["payload"]["native"]["basis"][0][1]["accumulator"]["total_mass"].SetDouble(-1); }, true));
    invalid.push_back(Mutate(
        exported.second,
        [](auto& d) { d["payload"]["native"]["basis"][0][1]["active_basis"]["basis_version"].SetUint64(9999); }, true));
    invalid.push_back(
        Mutate(exported.second, [](auto& d) { d["payload"]["native"]["processed"][0][1].SetInt(-1); }, true));
    invalid.push_back(Mutate(
        exported.second,
        [](auto& d) {
            d["payload"]["compatibility"]["spec"]["task_spec"]["group_space_version"].SetString("626164",
                                                                                                d.GetAllocator());
        },
        true));
    invalid.push_back(Mutate(
        exported.second, [](auto& d) { d["payload"]["fusion_layout"]["iteration"][0]["bucket"].SetUint(9999); }, true));
    invalid.push_back(
        Mutate(exported.second, [](auto& d) { d["payload"]["fusion_layout"]["buckets"].SetUint64(1ull << 40); }, true));
    for (const auto& text : invalid) {
        BaselineCheckpointRestoreV1 request;
        request.content = text;
        assert(dest->RestoreCheckpoint(request) != BaselineStatus::kOk);
        assert(empty == dest->ExportCheckpoint(BaselineSerializationFormat::kJson));
    }
    BaselineCheckpointRestoreV1 request;
    request.content = exported.second;
    assert(dest->RestoreCheckpoint(request) == BaselineStatus::kOk);
    assert(dest->RestoreCheckpoint(request) != BaselineStatus::kOk);
    assert(exported == dest->ExportCheckpoint(BaselineSerializationFormat::kJson));
    EqualRelation(task->SubmitObservation(RelationObservation(16), {}),
                  dt->SubmitObservation(RelationObservation(16), {}));
}
void RelationCapacityAndEmpty() {
    const auto options = RelationOptions();
    Environment source(options.c_str()), target(options.c_str());
    auto task = source.plugin.CreateRelationTask(relation_config, BaselineSerializationFormat::kJson).second;
    auto cp = Bind(source, task);
    const auto empty = cp->ExportCheckpoint(BaselineSerializationFormat::kJson);
    assert(empty.first == BaselineStatus::kOk);
    auto dt = target.plugin.CreateRelationTask(relation_config, BaselineSerializationFormat::kJson).second;
    auto dest = Bind(target, dt, 1 << 24, {1, 1, 2});
    const auto before = dest->ExportCheckpoint(BaselineSerializationFormat::kJson);
    for (int64_t t = 0; t < 6; ++t)
        for (const char* key : {"a", "b"})
            assert(task->SubmitObservation(RelationObservation(t, key), {}).status == BaselineStatus::kOk);
    const auto full = cp->ExportCheckpoint(BaselineSerializationFormat::kJson);
    assert(full.first == BaselineStatus::kOk);
    BaselineCheckpointRestoreV1 request;
    request.content = full.second;
    assert(dest->RestoreCheckpoint(request) != BaselineStatus::kOk);
    assert(before == dest->ExportCheckpoint(BaselineSerializationFormat::kJson));
    request.content = empty.second;
    assert(dest->RestoreCheckpoint(request) == BaselineStatus::kOk);
    assert(empty == dest->ExportCheckpoint(BaselineSerializationFormat::kJson));
    assert(dest->RestoreCheckpoint(request) != BaselineStatus::kOk);
    dt->Close();
    assert(dest->ExportCheckpoint(BaselineSerializationFormat::kJson).first != BaselineStatus::kOk);
    assert(dest->RestoreCheckpoint(request) != BaselineStatus::kOk);
    for (const char* key : {"a", "b"}) {
        RelationBootstrapInput history;
        history.series_key = key;
        history.options.min_observation_count = 100;
        for (int64_t t = 0; t < 200; ++t) {
            RelationBootstrapBlock block;
            block.bucket_id = t;
            block.group_idx = {1, 2, 3};
            block.metrics = {{"m2", 100, 2, {80, 20, 0}}, {"m", 200, 2, {160, 40, 0}}};
            history.blocks.push_back(std::move(block));
        }
        assert(task->Bootstrap(history).status == BaselineStatus::kOk);
    }
    // Runtime capacity is eight; the two parent models alone exceed the model capacity.
    auto limited_task = target.plugin.CreateRelationTask(relation_config, BaselineSerializationFormat::kJson).second;
    auto limited = Bind(target, limited_task, 1 << 24, {8, 1, 2});
    const auto model_before = limited->ExportCheckpoint(BaselineSerializationFormat::kJson);
    const auto models = cp->ExportCheckpoint(BaselineSerializationFormat::kJson);
    assert(models.first == BaselineStatus::kOk);
    request.content = models.second;
    assert(limited->RestoreCheckpoint(request) != BaselineStatus::kOk);
    assert(model_before == limited->ExportCheckpoint(BaselineSerializationFormat::kJson));
    request.content = empty.second;
    assert(limited->RestoreCheckpoint(request) == BaselineStatus::kOk);
    std::string large_config(relation_config);
    for (const auto& change :
         {std::make_pair(std::string("relation-checkpoint"), std::string("large-capacity-checkpoint")),
          std::make_pair(std::string("\"k_support\":2"), std::string("\"k_support\":2147483647")),
          std::make_pair(std::string("\"k_stable\":1"), std::string("\"k_stable\":2147483647"))})
        large_config.replace(large_config.find(change.first), change.first.size(), change.second);
    auto large_task = source.plugin.CreateRelationTask(large_config, BaselineSerializationFormat::kJson).second;
    auto large = Bind(source, large_task, 1 << 24, {8, 8, std::numeric_limits<uint32_t>::max()});
    auto large_target = target.plugin.CreateRelationTask(large_config, BaselineSerializationFormat::kJson).second;
    auto large_dest = Bind(target, large_target, 1 << 24, {8, 8, std::numeric_limits<uint32_t>::max()});
    const auto large_empty = large->ExportCheckpoint(BaselineSerializationFormat::kJson);
    assert(large_empty.first == BaselineStatus::kOk);
    request.content = large_empty.second;
    assert(large_dest->RestoreCheckpoint(request) == BaselineStatus::kOk);
    assert(large_empty == large_dest->ExportCheckpoint(BaselineSerializationFormat::kJson));
}
void RelationFusionCleanup() {
    auto options = RelationOptions();
    std::ofstream("/tmp/baseline-operator-t5/relation-fast.yaml", std::ios::app) << R"(      relation_fusion:
        fusion_state_max_sources: 2
        fusion_state_ttl_buckets: 3
        fusion_state_cleanup_interval_updates: 1
        fusion_state_cleanup_scan_limit: 1
        fusion_persistence_max_keys_per_source: 3
)";
    Environment source(options.c_str()), target(options.c_str());
    auto task = source.plugin.CreateRelationTask(relation_config, BaselineSerializationFormat::kJson).second;
    auto cp = Bind(source, task);
    for (int64_t t = 0; t < 20; ++t) task->SubmitObservation(RelationObservation(t, "s" + std::to_string(t % 5)), {});
    auto exported = cp->ExportCheckpoint(BaselineSerializationFormat::kJson);
    assert(exported.first == BaselineStatus::kOk);
    auto dt = target.plugin.CreateRelationTask(relation_config, BaselineSerializationFormat::kJson).second;
    auto dest = Bind(target, dt);
    BaselineCheckpointRestoreV1 request;
    request.content = exported.second;
    assert(dest->RestoreCheckpoint(request) == BaselineStatus::kOk);
    assert(exported == dest->ExportCheckpoint(BaselineSerializationFormat::kJson));
    for (int64_t t = 20; t < 50; ++t) {
        const auto obs = RelationObservation(t, "s" + std::to_string(t % 5));
        EqualRelation(task->SubmitObservation(obs, {}), dt->SubmitObservation(obs, {}));
        assert(cp->ExportCheckpoint(BaselineSerializationFormat::kJson) ==
               dest->ExportCheckpoint(BaselineSerializationFormat::kJson));
    }
}
void DynamicDiscovery() {
    struct Library : IRegister, IQuerier {
        void* handle = dlopen("./libflowsql_baseline.so", RTLD_NOW | RTLD_LOCAL);
        IPlugin* plugin = nullptr;
        std::vector<std::pair<Guid, void*>> interfaces;
        Library() {
            assert(handle);
            using Register = IPlugin* (*)(IRegister*, const char*);
            auto reg = reinterpret_cast<Register>(dlsym(handle, "pluginregist"));
            assert(reg);
            plugin = reg(this, nullptr);
            assert(plugin && plugin->Load(this) == 0 && plugin->Start() == 0);
        }
        ~Library() {
            plugin->Stop();
            plugin->Unload();
            dlclose(handle);
        }
        void Regist(const Guid& id, void* pointer) override { interfaces.emplace_back(id, pointer); }
        void* First(const Guid& id) override {
            for (const auto& item : interfaces)
                if (std::memcmp(&id, &item.first, sizeof(Guid)) == 0) return item.second;
            return nullptr;
        }
        int Traverse(const Guid& id, fntraverse callback) override {
            auto* pointer = First(id);
            return pointer ? callback(pointer) : 0;
        }
    } library;
    auto* service = static_cast<IBaselineService*>(library.First(IID_BASELINE_SERVICE));
    auto* control = static_cast<IBaselineStateControlServiceV1*>(library.First(IID_BASELINE_STATE_CONTROL_SERVICE_V1));
    auto* checkpoint = static_cast<IBaselineCheckpointServiceV1*>(library.First(IID_BASELINE_CHECKPOINT_SERVICE_V1));
    assert(service && control && checkpoint);
    auto task = service->CreateValueTask(value_config, BaselineSerializationFormat::kJson).second;
    auto bound = control->Bind(task, {8, 8, 2});
    auto cp = checkpoint->Bind(task, bound.second, {});
    assert(cp.first == BaselineStatus::kOk);
    task->SubmitObservation({"series", 0, 100, 1}, {});
    task.reset();
    bound.second.reset();
    assert(cp.second->ExportCheckpoint(BaselineSerializationFormat::kJson).first == BaselineStatus::kOk);
}
void BootstrapRoundtrip(bool ratio) {
    Environment source, target;
    auto v = ratio ? nullptr : source.plugin.CreateValueTask(value_config, BaselineSerializationFormat::kJson).second;
    auto r = ratio ? source.plugin.CreateRatioTask(ratio_config, BaselineSerializationFormat::kJson).second : nullptr;
    auto task = ratio ? std::static_pointer_cast<IBaselineTask>(r) : std::static_pointer_cast<IBaselineTask>(v);
    auto cp = Bind(source, task);
    ValueBootstrapInput vi;
    RatioBootstrapInput ri;
    vi.series_key = ri.series_key = "history";
    vi.options.min_observation_count = ri.options.min_observation_count = 100;
    for (int64_t t = 0; t < 200; ++t) {
        vi.observations.push_back({t, 100. + t % 10, 1});
        ri.observations.push_back({t, 800. + t % 10, 1000});
    }
    assert((ratio ? r->Bootstrap(ri) : v->Bootstrap(vi)).status == BaselineStatus::kOk);
    for (int64_t t = 200; t < 220; ++t) {
        if (ratio)
            r->SubmitObservation({"history", t, 800, 1000}, {});
        else
            v->SubmitObservation({"history", t, 100, 1}, {});
    }
    const auto exported = cp->ExportCheckpoint(BaselineSerializationFormat::kJson);
    assert(exported.first == BaselineStatus::kOk);
    auto dv = ratio ? nullptr : target.plugin.CreateValueTask(value_config, BaselineSerializationFormat::kJson).second;
    auto dr = ratio ? target.plugin.CreateRatioTask(ratio_config, BaselineSerializationFormat::kJson).second : nullptr;
    auto restored_task =
        ratio ? std::static_pointer_cast<IBaselineTask>(dr) : std::static_pointer_cast<IBaselineTask>(dv);
    auto dest = Bind(target, restored_task);
    BaselineCheckpointRestoreV1 request;
    request.content = exported.second;
    assert(dest->RestoreCheckpoint(request) == BaselineStatus::kOk);
    assert(exported == dest->ExportCheckpoint(BaselineSerializationFormat::kJson));
    assert((ratio ? r->ExportBootstrapArtifact(BaselineSerializationFormat::kJson)
                  : v->ExportBootstrapArtifact(BaselineSerializationFormat::kJson)) ==
           (ratio ? dr->ExportBootstrapArtifact(BaselineSerializationFormat::kJson)
                  : dv->ExportBootstrapArtifact(BaselineSerializationFormat::kJson)));
    assert((ratio ? r->ExportBootstrapSeed(BaselineSerializationFormat::kJson)
                  : v->ExportBootstrapSeed(BaselineSerializationFormat::kJson)) ==
           (ratio ? dr->ExportBootstrapSeed(BaselineSerializationFormat::kJson)
                  : dv->ExportBootstrapSeed(BaselineSerializationFormat::kJson)));
    Equal(ratio ? r->SubmitObservation({"history", 220, 1, 1000}, {})
                : v->SubmitObservation({"history", 220, 1e100, 1}, {}),
          ratio ? dr->SubmitObservation({"history", 220, 1, 1000}, {})
                : dv->SubmitObservation({"history", 220, 1e100, 1}, {}));
    assert(cp->ExportCheckpoint(BaselineSerializationFormat::kJson) ==
           dest->ExportCheckpoint(BaselineSerializationFormat::kJson));
}
}  // namespace
int main() {
    Roundtrip(false);
    Roundtrip(true);
    RejectAndRetry();
    CapacityAndBinding();
    EligibilityAndCompatibility();
    LowSupportRoundtrip(false);
    LowSupportRoundtrip(true);
    BootstrapRoundtrip(false);
    BootstrapRoundtrip(true);
    RelationRoundtrip();
    RelationModelsAndRelease(std::nullopt);
    RelationModelsAndRelease(BaselineStateReleaseScopeV1::kRuntimeOnly);
    RelationModelsAndRelease(BaselineStateReleaseScopeV1::kAllState);
    RelationRejectAndRetry();
    RelationCapacityAndEmpty();
    RelationFusionCleanup();
    DynamicDiscovery();
    std::cout << "PASS full Value/Ratio/Relation checkpoint and continuation parity\n";
}
