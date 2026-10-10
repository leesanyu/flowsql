// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <dlfcn.h>
#include <framework/interfaces/cpp_operator_plugin_abi.h>
#include <framework/interfaces/iblock_transform_operator.h>
#include <operators/baseliner/durable_store.h>
#include <operators/baseliner/evaluation.h>
#include <operators/baseliner/result_codec.h>
#include <plugins/baseline/baseline_plugin.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <services/database/database_plugin.h>
#include <unistd.h>
#include <cassert>
#include <chrono>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
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
    Environment() { assert(plugin.Option(nullptr) == 0 && plugin.Load(nullptr) == 0 && plugin.Start() == 0); }
    ~Environment() {
        plugin.Stop();
        plugin.Unload();
    }
};
void Ok(int rc, const EvaluationEngine& e) {
    if (rc) std::cerr << e.LastError() << '\n';
    assert(rc == 0);
}
Observation Input(BaselineTaskKind kind, int64_t bucket, uint64_t id = 1) {
    Observation o;
    o.dataset_id = "d";
    o.kind = kind;
    o.metric_id = kind == BaselineTaskKind::kValue ? "v" : kind == BaselineTaskKind::kRatio ? "r" : "dist";
    o.source_epoch = "epoch";
    o.bucket = bucket;
    assert(EncodeIdentity("sqlite.source", "d", o.metric_id, kind, {id}, &o.identity).ok());
    if (kind == BaselineTaskKind::kValue) {
        o.value = 100.;
        o.sample_count = 1;
    } else if (kind == BaselineTaskKind::kRatio) {
        o.numerator = 800.;
        o.denominator = 1000.;
    } else {
        o.groups = {1, 2, 3};
        o.metrics = {{"m", 1000., 3, {500., 300., 200.}}};
    }
    return o;
}
ConfigSnapshot Online(bool relation = false) {
    auto c = relation ? RelationConfig() : Config();
    c.config.mode = Mode::kPoll;
    c.config.state.idle_timeout_ms = 100;
    c.config.state.maintenance_max_releases = 1;
    return c;
}
void ClockAndBudget() {
    Environment env;
    auto c = Online();
    EvaluationEngine e;
    Ok(e.Open(c, &env.plugin, &env.plugin, &env.plugin), e);
    e.SetTime(1000000000, 10000000000);
    EvaluationOutput out;
    auto o = Input(BaselineTaskKind::kValue, 0);
    Ok(e.Submit(o, &out), e);
    assert(e.NextMaintenanceDeadline() == 1100000000);
    e.SetTime(1050000000, 10050000000);
    assert(e.Submit(o, &out) != 0);
    o.bucket = 1;
    o.value = -1;
    assert(e.Submit(o, &out) != 0);
    assert(e.QuerySeriesSnapshot("d", "v", o.identity).first == BaselineStatus::kOk);
    assert(e.NextMaintenanceDeadline() == 1100000000);
    std::vector<MaintenanceRow> rows;
    Ok(e.Maintain(&rows), e);
    assert(rows.empty());
    e.SetTime(1100000000, 10100000000);
    Ok(e.Maintain(&rows), e);
    assert(rows.size() == 1 && rows[0].reason == "RuntimeOnly" && rows[0].usage.runtime_identities == 0);
    assert(!e.NextMaintenanceDeadline());
    assert(e.QuerySeriesSnapshot("d", "v", o.identity).first != BaselineStatus::kOk);
    assert(e.QueryUsage().second.runtime_identities == 0);
    o.value = 100;
    assert(e.Submit(o, &out) != 0);  // Arrow same epoch requires upstream progress.
    o.source_epoch = "new-epoch";
    Ok(e.Submit(o, &out), e);
    assert(out.results[0].values.status == BaselineStatus::kNotTrained);
}
void Capacity() {
    for (bool evict : {false, true}) {
        Environment env;
        auto c = Online();
        c.config.state.evict_idle = evict;
        c.config.state.limits.max_runtime_identities = 1;
        EvaluationEngine e;
        Ok(e.Open(c, &env.plugin, &env.plugin), e);
        e.SetTime(1000000000, 10000000000);
        EvaluationOutput out;
        auto old = Input(BaselineTaskKind::kValue, 0);
        Ok(e.Submit(old, &out), e);
        auto next = Input(BaselineTaskKind::kValue, 1, 2);
        assert(e.ValidateBatch({next}) != 0 && e.Submit(next, &out) != 0);
        assert(e.QueryUsage().second.runtime_identities == 1);
        e.SetTime(1100000000, 10100000000);
        if (evict) {
            Ok(e.ValidateBatch({next}), e);
            Ok(e.Submit(next, &out), e);
            assert(e.QueryUsage().second.runtime_identities == 1);
            std::vector<MaintenanceRow> rows;
            Ok(e.Maintain(&rows), e);
            assert(rows.size() == 1);
        } else
            assert(e.ValidateBatch({next}) != 0 && e.Submit(next, &out) != 0);
    }
    Environment env;
    auto c = Online();
    c.config.state.maintenance_max_bytes = 1;
    EvaluationEngine e;
    Ok(e.Open(c, &env.plugin, &env.plugin), e);
    e.SetTime(1000000000, 10000000000);
    EvaluationOutput out;
    auto old = Input(BaselineTaskKind::kValue, 0);
    Ok(e.Submit(old, &out), e);
    e.SetTime(1100000000, 10100000000);
    std::vector<MaintenanceRow> rows;
    assert(e.Maintain(&rows) != 0 && rows.empty());
    assert(e.QueryUsage().second.runtime_identities == 1);
    assert(e.QuerySeriesSnapshot("d", "v", old.identity).first == BaselineStatus::kOk);
}
void CheckpointWorkBudget() {
    Environment sample;
    auto c = Online();
    std::string checkpoint;
    {
        EvaluationEngine e;
        Ok(e.Open(c, &sample.plugin, &sample.plugin, &sample.plugin), e);
        e.SetTime(1000000000, 10000000000);
        EvaluationOutput out;
        Ok(e.Submit(Input(BaselineTaskKind::kValue, 0), &out), e);
        Ok(e.ExportCheckpoint(&checkpoint), e);
    }
    c.config.state.maintenance_max_bytes = checkpoint.size() * 2 + 4096;
    EvaluationEngine e;
    Ok(e.Open(c, &sample.plugin, &sample.plugin, &sample.plugin), e);
    e.SetTime(1000000000, 10000000000);
    EvaluationOutput out;
    auto o = Input(BaselineTaskKind::kValue, 0);
    Ok(e.Submit(o, &out), e);
    auto before = e.QuerySeriesSnapshot("d", "v", o.identity);
    e.SetTime(1100000000, 10100000000);
    std::vector<MaintenanceRow> rows;
    assert(e.Maintain(&rows) != 0 && rows.empty());
    assert(e.QueryUsage().second.runtime_identities == 1 && e.QuerySeriesSnapshot("d", "v", o.identity) == before);
}
void RecoveryAge() {
    for (int mode : {0, 1, 2}) {
        Environment source, target;
        auto c = Online();
        EvaluationEngine first, second;
        Ok(first.Open(c, &source.plugin, &source.plugin, &source.plugin), first);
        first.SetTime(1000000000, 10000000000);
        EvaluationOutput out;
        Ok(first.Submit(Input(BaselineTaskKind::kValue, 0), &out), first);
        first.SetTime(1040000000, 10040000000);
        std::string checkpoint;
        Ok(first.ExportCheckpoint(&checkpoint), first);
        Ok(second.Open(c, &target.plugin, &target.plugin, &target.plugin), second);
        second.SetTime(5000000000, mode == 0 ? 10100000000 : mode == 1 ? 10060000000 : 9000000000);
        Ok(second.RestoreCheckpoint(checkpoint), second);
        std::vector<MaintenanceRow> rows;
        Ok(second.Maintain(&rows), second);
        if (mode == 0) assert(rows.size() == 1 && second.QueryUsage().second.runtime_identities == 0);
        if (mode == 1) {
            assert(rows.empty() && second.NextMaintenanceDeadline() == 5040000000);
            second.SetTime(5040000000, 10100000000);
            Ok(second.Maintain(&rows), second);
            assert(rows.size() == 1);
        }
        if (mode == 2) {
            assert(rows.size() == 1 && rows[0].event_kind == "deadline_reset");
            assert(second.NextMaintenanceDeadline() == 5100000000);
        }
    }
    Environment env;
    auto c = Config();
    EvaluationEngine e;
    Ok(e.Open(c, &env.plugin, &env.plugin), e);
    EvaluationOutput out;
    Ok(e.Submit(Input(BaselineTaskKind::kValue, 0), &out), e);
    e.SetTime(10000000000000, 20000000000000);
    std::vector<MaintenanceRow> rows;
    Ok(e.Maintain(&rows), e);
    assert(rows.empty() && !e.NextMaintenanceDeadline());
    assert(e.QueryUsage().second.runtime_identities == 1);
}
void HistoryAndFanout() {
    for (auto kind : {BaselineTaskKind::kValue, BaselineTaskKind::kRatio, BaselineTaskKind::kRelation}) {
        for (auto scope : {BaselineStateReleaseScopeV1::kRuntimeOnly, BaselineStateReleaseScopeV1::kAllState}) {
            Environment source, target;
            auto c = Online(kind == BaselineTaskKind::kRelation);
            c.config.state.release_scope = scope;
            c.config.bootstrap = {true, 0, 40, 20, false};
            EvaluationEngine e;
            Ok(e.Open(c, &source.plugin, &source.plugin, &source.plugin), e);
            e.SetTime(1000000000, 10000000000);
            EvaluationOutput out;
            for (int64_t t = 0; t <= 40; ++t) Ok(e.Submit(Input(kind, t), &out), e);
            assert(e.QueryUsage().second.model_identities == 1);
            if (kind == BaselineTaskKind::kRelation) assert(e.QueryUsage().second.routed_states > 0);
            Ok(e.SetSourceProgress({1, {{"d", "epoch", "position", 41}}}), e);
            e.SetTime(1100000000, 10100000000);
            std::vector<MaintenanceRow> rows;
            Ok(e.Maintain(&rows), e);
            assert(rows.size() == 1 && rows[0].usage.runtime_identities == 0 && rows[0].usage.routed_states == 0 &&
                   rows[0].usage.retained_basis_versions == 0);
            assert(rows[0].usage.model_identities == (scope == BaselineStateReleaseScopeV1::kRuntimeOnly ? 1u : 0u));
            std::string checkpoint;
            Ok(e.ExportCheckpoint(&checkpoint), e);
            EvaluationEngine resumed;
            Ok(resumed.Open(c, &target.plugin, &target.plugin, &target.plugin), resumed);
            resumed.SetTime(2000000000, 10200000000);
            Ok(resumed.SetSourceProgress({1, {{"d", "epoch", "position", 41}}}), resumed);
            Ok(resumed.RestoreCheckpoint(checkpoint), resumed);
            assert(resumed.QueryUsage().second.runtime_identities == 0 &&
                   resumed.QueryUsage().second.model_identities == rows[0].usage.model_identities);
            auto old = Input(kind, 40);
            assert(resumed.ClassifyReplay(old) == 1);
            assert(resumed.Submit(old, &out) != 0);
            auto next = Input(kind, 41);
            Ok(resumed.Submit(next, &out), resumed);
            if (kind != BaselineTaskKind::kRelation)
                assert(out.results[0].values.status == (scope == BaselineStateReleaseScopeV1::kRuntimeOnly
                                                            ? BaselineStatus::kOk
                                                            : BaselineStatus::kNotTrained));
        }
    }
}

void MultipleDeadlinesAndModels() {
    Environment env;
    auto c = Online();
    c.config.state.maintenance_max_releases = 2;
    EvaluationEngine e;
    Ok(e.Open(c, &env.plugin, &env.plugin, &env.plugin), e);
    e.SetTime(1000000000, 10000000000);
    EvaluationOutput out;
    for (uint64_t id = 1; id <= 5; ++id) Ok(e.Submit(Input(BaselineTaskKind::kValue, 0, id), &out), e);
    e.SetTime(1050000000, 10050000000);
    Ok(e.Submit(Input(BaselineTaskKind::kValue, 1, 5), &out), e);
    e.SetTime(1100000000, 10100000000);
    std::vector<MaintenanceRow> rows;
    Ok(e.Maintain(&rows), e);
    assert(rows.size() == 2 && e.QueryUsage().second.runtime_identities == 3);
    Ok(e.Maintain(&rows), e);
    assert(rows.size() == 2 && e.QueryUsage().second.runtime_identities == 1);
    assert(e.NextMaintenanceDeadline() == 1150000000);
    Ok(e.Maintain(&rows), e);
    assert(rows.empty());
    auto stale = Input(BaselineTaskKind::kValue, 0);
    assert(e.SetSourceProgress({1, {{"d", "epoch", "position", 2}}}) == 0);
    assert(e.ClassifyReplay(stale) == 1 && e.ValidateBatch({stale}) != 0);
    std::string payload;
    Ok(e.ExportCheckpoint(&payload), e);
    assert(payload.find("replay") == std::string::npos);  // Removed identities have no tombstones.
}
void FanoutMaintenanceLatency() {
    Environment env;
    auto c = Online(true);
    c.config.state.maintenance_max_releases = 4;
    c.config.state.limits.max_runtime_identities = 64;
    EvaluationEngine e;
    Ok(e.Open(c, &env.plugin, &env.plugin, &env.plugin), e);
    e.SetTime(1000000000, 10000000000);
    EvaluationOutput out;
    for (uint64_t id = 1; id <= 64; ++id)
        for (int64_t t = 0; t < 8; ++t) Ok(e.Submit(Input(BaselineTaskKind::kRelation, t, id), &out), e);
    auto usage = e.QueryUsage().second;
    assert(usage.runtime_identities == 64 && usage.routed_states > 64);
    e.SetTime(1100000000, 10100000000);
    int64_t worst = 0;
    uint64_t released = 0;
    while (e.NextMaintenanceDeadline()) {
        std::vector<MaintenanceRow> rows;
        auto begin = std::chrono::steady_clock::now();
        Ok(e.Maintain(&rows), e);
        auto ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count();
        worst = std::max(worst, ns);
        assert(!rows.empty() && rows.size() <= 4);
        released += rows.size();
    }
    assert(released == 64 && e.QueryUsage().second.runtime_identities == 0 && e.QueryUsage().second.routed_states == 0);
    std::cout << "MEASURE relation_sources=64 routed_states=" << usage.routed_states
              << " release_budget=4 worst_maintenance_ns=" << worst << '\n';
}
struct FailingControl : IBaselineStateControlServiceV1 {
    IBaselineStateControlServiceV1* service;
    struct Handle : IBaselineTaskStateControlV1 {
        std::shared_ptr<IBaselineTaskStateControlV1> original;
        BaselineStatus ReleaseIdentity(std::string_view, BaselineStateReleaseScopeV1) override {
            return BaselineStatus::kInvalidArgument;
        }
        std::pair<BaselineStatus, BaselineStateUsageV1> QueryUsage() const override { return original->QueryUsage(); }
    };
    explicit FailingControl(IBaselineStateControlServiceV1* s) : service(s) {}
    std::pair<BaselineStatus, std::shared_ptr<IBaselineTaskStateControlV1>> Bind(
        std::shared_ptr<IBaselineTask> task, const BaselineStateLimitsV1& limits) override {
        auto bound = service->Bind(task, limits);
        auto h = std::make_shared<Handle>();
        h->original = bound.second;
        return {bound.first, h};
    }
};
void FailedRelease() {
    Environment env;
    FailingControl control(&env.plugin);
    auto c = Online();
    EvaluationEngine e;
    Ok(e.Open(c, &env.plugin, &control), e);
    e.SetTime(1000000000, 10000000000);
    EvaluationOutput out;
    auto o = Input(BaselineTaskKind::kValue, 0);
    Ok(e.Submit(o, &out), e);
    auto snapshot = e.QuerySeriesSnapshot("d", "v", o.identity);
    e.SetTime(1100000000, 10100000000);
    std::vector<MaintenanceRow> rows;
    assert(e.Maintain(&rows) != 0 && rows.empty());
    assert(e.QueryUsage().second.runtime_identities == 1);
    assert(e.QuerySeriesSnapshot("d", "v", o.identity) == snapshot && e.NextMaintenanceDeadline() == 1100000000);
}

void Check(bool valid, const std::string& message) {
    if (!valid) std::cerr << message << '\n';
    assert(valid);
}
std::string Configuration(const std::string& source, const std::string& table, bool poll, const std::string& key) {
    std::string result =
        R"({"schema_version":1,"task_key":")" + key + R"(","source":")" + source + R"(","mode":")" +
        (poll ? "poll" : "snapshot") + R"(",
"clock":{"bucket_seconds":60,"timezone":"UTC"},"calendar":{"calendar_id":"cn-holiday","calendar_version":"2026.1"},
"forecast":{"horizon_buckets":2},"read_policy":{"page_rows":3,"poll_interval_ms":1},
"persistence":{"checkpoint_every_buckets":3,"retain_generations":2},
"datasets":[{"id":"d","table":")" +
        table +
        R"(","fields":{"id":"uint64","g":"uint64","t":"int64","v":"float64","n":"float64","den":"float64","x":"float64"},
"scope":{"begin_bucket":0,"end_bucket":40,"consistency":"immutable_range"},"series_keys":["id"],
"deduplicate":{"keys":["id","g","t"],"on_duplicate":"require_equal"},"bucket":{"column":"t","unit":"bucket_id"},
"metrics":[{"id":"v","kind":"value","column":"v","aggregate":"sum","feature_type":"value_basic","profile":"default"},
{"id":"r","kind":"ratio","feature_type":"ratio","profile":"rate_core","numerator":{"column":"n","aggregate":"sum"},"denominator":{"column":"den","aggregate":"sum"}},
{"id":"dist","kind":"relation","feature_type":"relation","profile":"default","group_space":{"id":"groups","version":"v1","column":"g","unknown":"reject"},
"metrics":[{"id":"m","column":"x","aggregate":"sum"}],"support_policy":{"k_support":3,"min_hist_share":0.01,"min_active_ratio":0.1},"summary_policy":{"k_head":2,"k_stable":1}}]}]})";
    if (poll) {
        const std::string selection = ",\"consistency\":\"immutable_range\"";
        auto value = result.find(selection);
        result.erase(value, selection.size());
    }
    return result;
}
struct Fault {
    int mode = 0;
};
class FaultSession final : public IDatabaseAtomicSessionV1 {
 public:
    FaultSession(std::shared_ptr<IDatabaseAtomicSessionV1> session, std::shared_ptr<Fault> fault)
        : session_(std::move(session)), fault_(std::move(fault)) {}
    DatabaseAtomicStatusV1 Begin() override { return session_->Begin(); }
    DatabaseAtomicStatusV1 ExecutePrepared(const char* sql, const DatabaseParameterV1* p, size_t n,
                                           uint64_t* affected) override {
        if (fault_->mode == 1 && std::string_view(sql).find("INSERT INTO baseline_model_versions") == 0) {
            fault_->mode = 0;
            *affected = 0;
            return {DatabaseAtomicCodeV1::kError, "crash before model write"};
        }
        return session_->ExecutePrepared(sql, p, n, affected);
    }
    DatabaseAtomicStatusV1 CreateReader(const char* sql, const DatabaseParameterV1* p, size_t n,
                                        IBatchReader** r) override {
        return session_->CreateReader(sql, p, n, r);
    }
    DatabaseAtomicStatusV1 ReadDatabaseTime(int64_t* t) override { return session_->ReadDatabaseTime(t); }
    DatabaseCommitResultV1 Commit() override {
        int mode = fault_->mode;
        fault_->mode = 0;
        if (mode == 2) {
            auto status = session_->Rollback();
            Check(status.ok(), status.message);
            return {{DatabaseAtomicCodeV1::kError, "crash before commit"}, DatabaseCommitOutcomeV1::kRolledBack};
        }
        if (mode == 3 || mode == 4) {
            if (mode == 3) {
                auto result = session_->Commit();
                Check(result.status.ok(), result.status.message);
            } else
                Check(session_->Rollback().ok(), "unknown rollback fixture");
            session_->Close();
            return {{DatabaseAtomicCodeV1::kCommitUnknown, "lost commit confirmation"},
                    DatabaseCommitOutcomeV1::kUnknown};
        }
        return session_->Commit();
    }
    DatabaseAtomicStatusV1 Rollback() override { return session_->Rollback(); }
    void Cancel() override { session_->Cancel(); }
    void Close() override { session_->Close(); }

 private:
    std::shared_ptr<IDatabaseAtomicSessionV1> session_;
    std::shared_ptr<Fault> fault_;
};
class FaultTarget final : public IDatabaseChannel, public IDatabaseAtomicTargetV1 {
 public:
    explicit FaultTarget(std::shared_ptr<IDatabaseChannel> target) : target_(std::move(target)) {}
    std::shared_ptr<Fault> fault = std::make_shared<Fault>();
    const char* Category() override { return target_->Category(); }
    const char* Name() override { return target_->Name(); }
    const char* Type() override { return ChannelType::kDatabase; }
    const char* Schema() override { return ""; }
    int Open() override { return 0; }
    int Close() override { return 0; }
    int Flush() override { return 0; }
    bool IsOpened() const override { return target_->IsOpened(); }
    bool IsConnected() override { return target_->IsConnected(); }
    const char* GetLastError() override { return target_->GetLastError(); }
    int CreateReader(const char*, IBatchReader**) override { return -1; }
    int CreateWriter(const char*, IBatchWriter**) override { return -1; }
    int CreateArrowReader(const char*, IArrowReader**) override { return -1; }
    int CreateArrowWriter(const char*, IArrowWriter**) override { return -1; }
    int ExecuteQueryArrow(const char*, std::vector<std::shared_ptr<arrow::RecordBatch>>*) override { return -1; }
    int WriteArrowBatches(const char*, const std::vector<std::shared_ptr<arrow::RecordBatch>>&) override { return -1; }
    int ExecuteSql(const char*) override { return -1; }
    DatabaseAtomicStatusV1 AcquireAtomicSession(const DatabaseAtomicSessionOptionsV1& options,
                                                std::shared_ptr<IDatabaseAtomicSessionV1>* output) override {
        std::shared_ptr<IDatabaseAtomicSessionV1> session;
        auto s = dynamic_cast<IDatabaseAtomicTargetV1*>(target_.get())->AcquireAtomicSession(options, &session);
        if (s.ok()) *output = std::make_shared<FaultSession>(session, fault);
        return s;
    }

 private:
    std::shared_ptr<IDatabaseChannel> target_;
};

struct Querier : IQuerier {
    baseline::BaselinePlugin baseline;
    Querier() { assert(baseline.Option(nullptr) == 0 && baseline.Load(this) == 0 && baseline.Start() == 0); }
    ~Querier() {
        baseline.Stop();
        baseline.Unload();
    }
    void* First(const Guid& id) override {
        if (std::memcmp(&id, &IID_BASELINE_SERVICE, sizeof(id)) == 0) return static_cast<IBaselineService*>(&baseline);
        if (std::memcmp(&id, &IID_BASELINE_STATE_CONTROL_SERVICE_V1, sizeof(id)) == 0)
            return static_cast<IBaselineStateControlServiceV1*>(&baseline);
        if (std::memcmp(&id, &IID_BASELINE_CHECKPOINT_SERVICE_V1, sizeof(id)) == 0)
            return static_cast<IBaselineCheckpointServiceV1*>(&baseline);
        return nullptr;
    }
    int Traverse(const Guid& id, fntraverse f) override {
        auto* p = First(id);
        return p ? f(p) : 0;
    }
};
struct Loaded {
    void* handle = nullptr;
    IBlockTransformOperatorV2* provider = nullptr;
    CppOperatorPluginDestroyCapabilityV2Fn destroy = nullptr;
    explicit Loaded(IQuerier& q) {
        handle = dlopen("./libflowsql_baseliner.so", RTLD_NOW | RTLD_LOCAL);
        if (!handle) std::cerr << dlerror() << '\n';
        assert(handle);
        auto version =
            reinterpret_cast<CppOperatorPluginAbiVersionFn>(dlsym(handle, kCppOperatorPluginAbiVersionSymbol));
        auto count = reinterpret_cast<CppOperatorPluginCountFn>(dlsym(handle, kCppOperatorPluginCountSymbol));
        auto describe =
            reinterpret_cast<CppOperatorPluginDescribeV2Fn>(dlsym(handle, kCppOperatorPluginDescribeV2Symbol));
        auto create = reinterpret_cast<CppOperatorPluginCreateCapabilityV2Fn>(
            dlsym(handle, kCppOperatorPluginCreateCapabilityV2Symbol));
        destroy = reinterpret_cast<CppOperatorPluginDestroyCapabilityV2Fn>(
            dlsym(handle, kCppOperatorPluginDestroyCapabilityV2Symbol));
        assert(version && count && describe && create && destroy && version() == 2 && count() == 1);
        CppOperatorDescriptorV2 descriptor{};
        descriptor.struct_size = kCppOperatorDescriptorV2Size;
        assert(describe(0, &descriptor) == 0 && std::string(descriptor.category) == "explore" &&
               std::string(descriptor.name) == "baseliner");
        assert(std::memcmp(&descriptor.contract_iid, &IID_BLOCK_TRANSFORM_OPERATOR_V2, sizeof(Guid)) == 0 &&
               describe(1, &descriptor) != 0);
        provider = static_cast<IBlockTransformOperatorV2*>(create(0, &q));
        assert(provider);
    }
    ~Loaded() {
        if (provider) destroy(0, provider);
        if (handle) dlclose(handle);
    }
};
ConfigSnapshot ManagedConfiguration(bool all) {
    rapidjson::Document d;
    auto raw =
        Configuration("sqlite.source", "facts", true,
                      "maintenance-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
    d.Parse(raw.c_str());
    auto& a = d.GetAllocator();
    d["forecast"]["horizon_buckets"].SetUint(0);
    d["persistence"]["checkpoint_every_buckets"].SetUint(1000000);
    rapidjson::Value state(rapidjson::kObjectType);
    state.AddMember("idle_timeout_ms", 60000, a);
    state.AddMember("maintenance_max_releases", 3, a);
    state.AddMember("release_scope", rapidjson::Value(all ? "AllState" : "RuntimeOnly", a), a);
    d.AddMember("state_policy", state, a);
    rapidjson::Value bootstrap(rapidjson::kObjectType);
    bootstrap.AddMember("mode", "history", a);
    bootstrap.AddMember("begin_bucket", 0, a);
    bootstrap.AddMember("end_bucket", 40, a);
    bootstrap.AddMember("min_observations", 20, a);
    bootstrap.AddMember("insufficient", "fail", a);
    d.AddMember("bootstrap", bootstrap, a);
    rapidjson::StringBuffer b;
    rapidjson::Writer<rapidjson::StringBuffer> w(b);
    d.Accept(w);
    ConfigSnapshot c;
    auto status = ParseConfig({b.GetString(), b.GetSize()}, &c);
    Check(status.ok(), status.path + ":" + status.message);
    return c;
}
std::string With(const ConfigSnapshot& c) {
    rapidjson::StringBuffer b;
    rapidjson::Writer<rapidjson::StringBuffer> w(b);
    w.StartObject();
    w.Key("parameters");
    w.String(c.original_json.c_str());
    w.EndObject();
    return {b.GetString(), b.GetSize()};
}
using TaskOwner = std::unique_ptr<IBlockTransformTaskV2, std::function<void(IBlockTransformTaskV2*)>>;
TaskOwner ManagedTask(const std::shared_ptr<Loaded>& op, const ConfigSnapshot& c, IDatabaseChannel* target) {
    auto with = With(c);
    BlockTransformTaskConfigV2 config{kBlockTransformTaskConfigV2Size, kBlockTransformContractVersionV2,
                                      "maintenance-execution", with.c_str(), ""};
    IBlockTransformTaskV2* raw = nullptr;
    Check(op->provider->CreateTask(config, &raw) == 0 && raw, "create managed maintenance task");
    TaskOwner task(raw, [op](auto* p) { op->provider->ReleaseTask(p); });
    Check(dynamic_cast<IBlockTransformInputSourceTaskV1*>(raw)->BindInputSource(c.config.source.c_str()) == 0,
          "bind source");
    auto exact = std::string(target->Category()) + "." + target->Name();
    BlockTransformManagedSinkBindingV1 sink;
    sink.sink_channel = target;
    sink.target = exact.c_str();
    sink.category = target->Category();
    sink.name = target->Name();
    Check(dynamic_cast<IBlockTransformManagedSinkTaskV1*>(raw)->BindManagedSink(sink) == 0, raw->LastError());
    std::shared_ptr<arrow::Schema> output;
    Check(raw->Open(MakeSchema(SchemaKind::kObservation), &output) == 0, raw->LastError());
    return task;
}
void Feed(IBlockTransformTaskV2* task, bool history, int64_t bucket = 41) {
    std::vector<Observation> rows;
    for (int64_t t = history ? 0 : bucket; t <= (history ? 40 : bucket); ++t)
        for (auto kind : {BaselineTaskKind::kValue, BaselineTaskKind::kRatio, BaselineTaskKind::kRelation})
            rows.push_back(Input(kind, t));
    std::shared_ptr<arrow::RecordBatch> batch;
    std::string error;
    Check(MakeObservationBatch(rows, 1 << 24, &batch, &error) == 0, error);
    std::vector<BlockTransformOutputV1> outputs;
    Check(task->ProcessBlock(batch, 0, &outputs) == 0, task->LastError());
    assert(outputs.empty());
    Check(dynamic_cast<IBlockTransformInputProgressTaskV1*>(task)->AcceptInputProgress(
              {1, {{"d", "epoch", std::to_string(history ? 41 : bucket + 1), history ? 41 : bucket + 1}}}) == 0,
          task->LastError());
}
int Expire(IBlockTransformTaskV2* task, int64_t* latency = nullptr) {
    BlockTransformTimeDriveStateV1 state{};
    Check(task->GetTimeDriveState(&state) == 0 && state.armed, "idle time drive is armed");
    auto wall =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    BlockTransformTimeEventV1 event{kBlockTransformTimeEventV1Size, kBlockTransformTimeDriveVersionV1,
                                    state.deadline_ns + 61000000000, wall};
    std::vector<BlockTransformOutputV1> outputs;
    auto begin = std::chrono::steady_clock::now();
    int rc = task->OnTime(event, &outputs);
    assert(outputs.empty());
    if (latency)
        *latency =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count();
    return rc;
}
StoredGeneration Load(IDatabaseChannel* target, const ConfigSnapshot& c) {
    DurableStore store;
    Check(store.Open(c, target, "verify") == 0, store.LastError());
    StoredGeneration saved;
    Check(store.Load(&saved) == 0, store.LastError());
    store.Close();
    return saved;
}
uint64_t MaintenanceCount(IDatabaseChannel* target, const ConfigSnapshot& c) {
    std::shared_ptr<IDatabaseAtomicSessionV1> session;
    Check(dynamic_cast<IDatabaseAtomicTargetV1*>(target)->AcquireAtomicSession({}, &session).ok(),
          "query maintenance session");
    std::shared_ptr<arrow::RecordBatch> rows;
    Check(ReadAtomicBatch(*session, "SELECT COUNT(*) FROM baseline_maintenance_v1 WHERE task_key=?",
                          {TextParameter(c.config.task_key)}, &rows)
              .ok(),
          "query maintenance count");
    auto n = std::stoull(rows->column(0)->GetScalar(0).ValueOrDie()->ToString());
    session->Close();
    return n;
}
void AssertState(const StoredGeneration& saved, const ConfigSnapshot& c, uint64_t identities, uint64_t models) {
    rapidjson::Document envelope;
    envelope.Parse<rapidjson::kParseFullPrecisionFlag>(saved.checkpoint.c_str());
    assert(!envelope.HasParseError() && envelope.HasMember("engine"));
    Environment env;
    EvaluationEngine e;
    Ok(e.Open(c, &env.plugin, &env.plugin, &env.plugin), e);
    e.SetTime(1000000000,
              std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
                  .count());
    Ok(e.SetSourceProgress(saved.progress), e);
    Ok(e.RestoreCheckpoint({envelope["engine"].GetString(), envelope["engine"].GetStringLength()}), e);
    auto usage = e.QueryUsage().second;
    assert(usage.runtime_identities == identities && usage.model_identities == models);
    if (!identities) assert(!usage.routed_states && !usage.retained_basis_versions);
    for (auto kind : {BaselineTaskKind::kValue, BaselineTaskKind::kRatio, BaselineTaskKind::kRelation})
        assert(e.ClassifyReplay(Input(kind, 40)) == 1);
}
void ManagedMatrix(std::shared_ptr<IDatabaseChannel> target) {
    for (bool all : {false, true}) {
        auto c = ManagedConfiguration(all);
        Querier q;
        auto op = std::make_shared<Loaded>(q);
        int64_t latency = 0;
        {
            auto task = ManagedTask(op, c, target.get());
            Feed(task.get(), true);
            Check(Expire(task.get(), &latency) == 0, task->LastError());
        }
        auto saved = Load(target.get(), c);
        assert(saved.generation == 2 && MaintenanceCount(target.get(), c) == 3);
        AssertState(saved, c, 0, all ? 0 : 3);
        {
            auto task = ManagedTask(op, c, target.get());
            Feed(task.get(), false);
            Check(Expire(task.get()) == 0, task->LastError());
        }
        saved = Load(target.get(), c);
        assert(saved.generation == 4 && MaintenanceCount(target.get(), c) == 6);
        AssertState(saved, c, 0, all ? 0 : 3);
        std::cout << "PASS managed maintenance " << target->Category() << " " << (all ? "AllState" : "RuntimeOnly")
                  << " idle_publish_ns=" << latency << std::endl;
    }
    auto c = ManagedConfiguration(true);
    auto fault = std::make_shared<FaultTarget>(target);
    Querier q;
    auto op = std::make_shared<Loaded>(q);
    {
        auto task = ManagedTask(op, c, fault.get());
        Feed(task.get(), true);
        fault->fault->mode = 1;
        assert(Expire(task.get()) != 0);
    }
    {  // A failed/crashed writer retains its fencing lease; takeover is legal only after expiry.
        DurableStore blocked;
        assert(blocked.Open(c, target.get(), "premature-takeover") != 0);
        std::shared_ptr<IDatabaseAtomicSessionV1> inspection;
        Check(dynamic_cast<IDatabaseAtomicTargetV1*>(target.get())->AcquireAtomicSession({}, &inspection).ok(),
              "fault lease inspection");
        Check(inspection->Begin().ok(), "simulate expired failed writer");
        int64_t now = 0;
        Check(inspection->ReadDatabaseTime(&now).ok(), "expiry target clock");
        auto p = std::vector<DatabaseParameterV1>{IntParameter(now), TextParameter(c.config.task_key)};
        uint64_t affected = 0;
        Check(inspection
                      ->ExecutePrepared("UPDATE baseline_tasks SET lease_deadline=? WHERE task_key=?", p.data(),
                                        p.size(), &affected)
                      .ok() &&
                  affected == 1,
              "fault lease expiry");
        Check(inspection->Commit().status.ok(), "fault lease expiry commit");
        inspection->Close();
    }
    auto saved = Load(target.get(), c);
    assert(saved.generation == 1 && MaintenanceCount(target.get(), c) == 0);
    AssertState(saved, c, 3, 3);
    {
        auto task = ManagedTask(op, c, target.get());
        Check(Expire(task.get()) == 0, task->LastError());
    }
    saved = Load(target.get(), c);
    assert(saved.generation == 2 && MaintenanceCount(target.get(), c) == 3);
    AssertState(saved, c, 0, 0);
    std::cout << "PASS failed maintenance preserves generation " << target->Category() << std::endl;
}
void ManagedDatabases() {
    const char* path = std::getenv("BASELINER_TEST_DB_OPTIONS");
    assert(path);
    std::ifstream input(path);
    std::ostringstream options;
    options << input.rdbuf();
    database::DatabasePlugin db;
    std::cout << std::flush;
    int saved = dup(STDOUT_FILENO);
    FILE* quiet = fopen("/dev/null", "w");
    dup2(fileno(quiet), STDOUT_FILENO);
    int rc = db.Option(options.str().c_str());
    fflush(stdout);
    dup2(saved, STDOUT_FILENO);
    close(saved);
    fclose(quiet);
    assert(rc == 0 && db.Load(nullptr) == 0 && db.Start() == 0);
    for (auto backend : {"sqlite", "mysql", "postgres"}) {
        auto target = db.AcquireChannel(backend, ("t2" + std::string(backend)).c_str());
        assert(target);
        ManagedMatrix(target);
    }
    db.Stop();
    db.Unload();
}
}  // namespace
int main() {
    ClockAndBudget();
    Capacity();
    CheckpointWorkBudget();
    RecoveryAge();
    HistoryAndFanout();
    FailedRelease();
    MultipleDeadlinesAndModels();
    FanoutMaintenanceLatency();
    ManagedDatabases();
    std::cout << "PASS maintenance time/capacity/recovery/fanout/failure\n";
}
