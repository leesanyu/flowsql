// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <dlfcn.h>
#include <framework/interfaces/cpp_operator_plugin_abi.h>
#include <framework/interfaces/iblock_transform_operator.h>
#include <operators/baseliner/durable_store.h>
#include <operators/baseliner/poll_input.h>
#include <operators/baseliner/result_codec.h>
#include <plugins/baseline/baseline_plugin.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <services/database/database_plugin.h>
#include <unistd.h>
#include <cassert>
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
using namespace flowsql;
using namespace flowsql::baseliner;
namespace {
void CheckAt(bool valid, const std::string& error, int line) {
    if (!valid) {
        std::cerr << "line " << line << ": " << error << std::endl;
        std::abort();
    }
}
#define Check(valid, error)                              \
    do {                                                 \
        if (!(valid)) CheckAt(false, (error), __LINE__); \
    } while (0)
std::string With(const std::string& json) {
    rapidjson::StringBuffer b;
    rapidjson::Writer<rapidjson::StringBuffer> w(b);
    w.StartObject();
    w.Key("parameters");
    w.String(json.data(), json.size());
    w.EndObject();
    return {b.GetString(), b.GetSize()};
}
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
        auto* value = First(id);
        return value ? f(value) : 0;
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
auto Task(const std::shared_ptr<Loaded>& owner, const std::string& with) {
    BlockTransformTaskConfigV2 c{kBlockTransformTaskConfigV2Size, kBlockTransformContractVersionV2, "execution",
                                 with.c_str(), ""};
    IBlockTransformTaskV2* raw = nullptr;
    assert(owner->provider->CreateTask(c, &raw) == 0 && raw);
    return std::unique_ptr<IBlockTransformTaskV2, std::function<void(IBlockTransformTaskV2*)>>(
        raw, [owner](auto* task) { owner->provider->ReleaseTask(task); });
}
struct GenericFacts {
    int generation = 1;
    int64_t closed = 40;
    int64_t first = 0;
    std::string epoch = "generic-epoch";
    int reads = 0;
    int fail_after = 0;
};
class GenericProgress final : public IDatabasePublishedProgressReaderV1 {
 public:
    explicit GenericProgress(std::shared_ptr<GenericFacts> facts) : facts_(std::move(facts)) {}
    int ReadProgress(const PublishedDatasetRequestV1& request, const std::string& position,
                     PublishedDatasetProgressV1* output) override {
        *output = {};
        if (cancelled_ || (facts_->fail_after && ++facts_->reads > facts_->fail_after)) return -1;
        if (!position.empty() && position != "opaque-" + std::to_string(facts_->generation)) return -1;
        output->progress = {request.dataset_id, facts_->epoch, "opaque-" + std::to_string(facts_->generation),
                            facts_->closed};
        output->first_bucket = facts_->first;
        return 0;
    }
    void Cancel() override { cancelled_ = true; }
    std::string LastError() const override { return "generic publication lost"; }

 private:
    std::shared_ptr<GenericFacts> facts_;
    std::atomic<bool> cancelled_{false};
};
// A normal business table is accepted only through its producer's explicit publication capability.
class GenericSource final : public IDatabaseChannel,
                            public IDatabaseSnapshotSourceV1,
                            public IDatabasePublishedSourceV1 {
 public:
    explicit GenericSource(std::shared_ptr<IDatabaseChannel> source) : source_(std::move(source)) {}
    std::shared_ptr<GenericFacts> facts = std::make_shared<GenericFacts>();
    const char* Category() override { return source_->Category(); }
    const char* Name() override { return source_->Name(); }
    const char* Type() override { return ChannelType::kDatabase; }
    const char* Schema() override { return ""; }
    int Open() override { return 0; }
    int Close() override { return 0; }
    int Flush() override { return 0; }
    bool IsOpened() const override { return source_->IsOpened(); }
    bool IsConnected() override { return source_->IsConnected(); }
    const char* GetLastError() override { return source_->GetLastError(); }
    int CreateReader(const char*, IBatchReader**) override { return -1; }
    int CreateWriter(const char*, IBatchWriter**) override { return -1; }
    int CreateArrowReader(const char*, IArrowReader**) override { return -1; }
    int CreateArrowWriter(const char*, IArrowWriter**) override { return -1; }
    int ExecuteQueryArrow(const char*, std::vector<std::shared_ptr<arrow::RecordBatch>>*) override { return -1; }
    int WriteArrowBatches(const char*, const std::vector<std::shared_ptr<arrow::RecordBatch>>&) override { return -1; }
    int ExecuteSql(const char*) override { return -1; }
    int CreateSnapshotSession(const DatabaseSnapshotOptionsV1& options,
                              std::shared_ptr<IDatabaseSnapshotSessionV1>* output) override {
        return dynamic_cast<IDatabaseSnapshotSourceV1*>(source_.get())->CreateSnapshotSession(options, output);
    }
    int CreatePublishedProgressReader(const DatabaseSnapshotOptionsV1&,
                                      std::shared_ptr<IDatabasePublishedProgressReaderV1>* output) override {
        *output = std::make_shared<GenericProgress>(facts);
        return 0;
    }

 private:
    std::shared_ptr<IDatabaseChannel> source_;
};
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
void RejectRestoredSource(const std::shared_ptr<Loaded>& owner, const ConfigSnapshot& config,
                          std::shared_ptr<IDatabaseChannel> source, IDatabaseChannel* target) {
    auto task = Task(owner, With(config.original_json));
    Check(
        dynamic_cast<IBlockTransformInputSourceTaskV1*>(task.get())->BindInputSource(config.config.source.c_str()) == 0,
        "bind source");
    auto exact = std::string(target->Category()) + "." + target->Name();
    BlockTransformManagedSinkBindingV1 sink;
    sink.sink_channel = target;
    sink.target = exact.c_str();
    sink.category = target->Category();
    sink.name = target->Name();
    Check(dynamic_cast<IBlockTransformManagedSinkTaskV1*>(task.get())->BindManagedSink(sink) == 0, task->LastError());
    BlockDatabaseInputBindingV1 binding;
    binding.source = std::move(source);
    binding.exact_source = config.config.source.c_str();
    BlockDatabaseInputV1 result;
    Check(dynamic_cast<IBlockTransformDatabaseInputTaskV1*>(task.get())->CreateDatabaseInput(binding, &result) != 0,
          "incompatible source restore rejected before consumption");
    assert(!result.input);
}
struct Session {
    decltype(Task(std::declval<std::shared_ptr<Loaded>>(), std::declval<std::string>())) task;
    IBlockStreamChannel* input = nullptr;
    IBlockTransformDatabaseInputTaskV1* provider = nullptr;
    Session(const std::shared_ptr<Loaded>& owner, const ConfigSnapshot& config,
            std::shared_ptr<IDatabaseChannel> source, IDatabaseChannel* target)
        : task(Task(owner, With(config.original_json))) {
        assert(dynamic_cast<IBlockTransformInputSourceTaskV1*>(task.get())
                   ->BindInputSource(config.config.source.c_str()) == 0);
        BlockTransformManagedSinkBindingV1 sink;
        sink.sink_channel = target;
        std::string exact = std::string(target->Category()) + "." + target->Name();
        sink.target = exact.c_str();
        sink.category = target->Category();
        sink.name = target->Name();
        auto* managed = dynamic_cast<IBlockTransformManagedSinkTaskV1*>(task.get());
        assert(managed);
        Check(managed->BindManagedSink(sink) == 0, task->LastError());
        provider = dynamic_cast<IBlockTransformDatabaseInputTaskV1*>(task.get());
        assert(provider);
        BlockDatabaseInputBindingV1 binding;
        binding.source = std::move(source);
        binding.exact_source = config.config.source.c_str();
        BlockDatabaseInputV1 result;
        Check(provider->CreateDatabaseInput(binding, &result) == 0, task->LastError());
        input = result.input;
        std::shared_ptr<arrow::Schema> schema;
        Check(task->Open(result.schema, &schema) == 0, task->LastError());
    }
    ~Session() {
        if (input) provider->ReleaseDatabaseInput(input);
    }
    bool Step() {
        auto event = input->PollBlock(5);
        if (event.kind == BlockPollEvent::kEof) return false;
        if (event.kind == BlockPollEvent::kTimeout) return false;
        Check(event.kind == BlockPollEvent::kData, task->LastError() + " source poll failed");
        std::vector<BlockTransformOutputV1> outputs;
        Check(task->ProcessBlock(event.batch, 0, &outputs) == 0, task->LastError());
        assert(outputs.empty());
        Check(input->ReleaseBlock(event.batch) == 0, "release input");
        if (auto* progress = dynamic_cast<IBlockStreamInputProgressV1*>(input)) {
            BlockInputProgressV1 p;
            Check(progress->ReadInputProgress(&p) == 0, "read progress");
            if (!p.datasets.empty())
                Check(dynamic_cast<IBlockTransformInputProgressTaskV1*>(task.get())->AcceptInputProgress(p) == 0,
                      task->LastError());
        }
        return true;
    }
    void Finish() {
        for (int i = 0; i < 5000 && Step(); ++i) {
        }
        std::vector<BlockTransformOutputV1> outputs;
        Check(task->Flush(&outputs) == 0, task->LastError());
        assert(outputs.empty());
    }
};
StoredGeneration ReadModel(const ConfigSnapshot& c, IDatabaseChannel* target) {
    DurableStore store;
    Check(store.Open(c, target, "inspect") == 0, store.LastError());
    StoredGeneration saved;
    Check(store.Load(&saved) == 0, store.LastError());
    store.Close();
    return saved;
}
Observation DirectInput(const ConfigSnapshot& c, BaselineTaskKind kind, int64_t t) {
    Observation o;
    o.dataset_id = "d";
    o.metric_id = kind == BaselineTaskKind::kValue ? "v" : kind == BaselineTaskKind::kRatio ? "r" : "dist";
    o.kind = kind;
    o.bucket = t;
    o.source_epoch = "generic-epoch";
    Check(EncodeIdentity(c.config.source, "d", o.metric_id, kind, {uint64_t{1}}, &o.identity).ok(), "identity");
    if (kind == BaselineTaskKind::kValue) {
        o.value = 100 + t % 3;
        o.sample_count = 1;
    } else if (kind == BaselineTaskKind::kRatio) {
        o.numerator = 800 + t % 3;
        o.denominator = 1000;
    } else {
        o.groups = {1, 2, 3};
        o.metrics = {{"m", 1000. + t, 3, {500., 300., 200. + t}}};
    }
    return o;
}
void InvalidRestore() {
    ConfigSnapshot c;
    Check(ParseConfig(Configuration("sqlite.test", "facts", false, "invalid-restore"), &c).ok(), "configuration");
    Querier producer;
    EvaluationEngine source;
    Check(source.Open(c, &producer.baseline, &producer.baseline, &producer.baseline) == 0, source.LastError());
    for (int64_t t = 0; t < 20; ++t)
        for (auto kind : {BaselineTaskKind::kValue, BaselineTaskKind::kRatio, BaselineTaskKind::kRelation}) {
            EvaluationOutput output;
            Check(source.Submit(DirectInput(c, kind, t), &output) == 0, source.LastError());
        }
    std::string checkpoint;
    Check(source.ExportCheckpoint(&checkpoint) == 0, source.LastError());
    Querier destination;
    EvaluationEngine target;
    Check(target.Open(c, &destination.baseline, &destination.baseline, &destination.baseline) == 0, target.LastError());
    std::string empty;
    Check(target.ExportCheckpoint(&empty) == 0, target.LastError());
    rapidjson::Document doc;
    doc.Parse(checkpoint.c_str());
    // The last module is corrupted after prior native modules have been prepared.
    auto& last = doc["modules"][doc["modules"].Size() - 1];
    last["model"].SetString("corrupt", doc.GetAllocator());
    rapidjson::StringBuffer b;
    rapidjson::Writer<rapidjson::StringBuffer> w(b);
    doc.Accept(w);
    Check(target.RestoreCheckpoint({b.GetString(), b.GetSize()}) != 0, "corrupt model rejected");
    std::string after;
    Check(target.ExportCheckpoint(&after) == 0 && after == empty,
          "failed restore preserves empty state and eligibility");
    Check(target.RestoreCheckpoint(checkpoint) == 0, target.LastError());
    Check(target.RestoreCheckpoint(checkpoint) != 0, "restore once");
    auto replay = DirectInput(c, BaselineTaskKind::kValue, 19);
    Check(target.ClassifyReplay(replay) == 1, "durable replay");
    replay.source_epoch = "changed";
    Check(target.ClassifyReplay(replay) == -1, "epoch mismatch");
    for (int64_t t = 20; t < 30; ++t)
        for (auto kind : {BaselineTaskKind::kValue, BaselineTaskKind::kRatio, BaselineTaskKind::kRelation}) {
            auto input = DirectInput(c, kind, t);
            EvaluationOutput left, right;
            Check(source.Submit(input, &left) == 0, source.LastError());
            Check(target.Submit(input, &right) == 0, target.LastError());
            left.results.insert(left.results.end(), left.forecasts.begin(), left.forecasts.end());
            right.results.insert(right.results.end(), right.forecasts.begin(), right.forecasts.end());
            std::shared_ptr<arrow::RecordBatch> l, r;
            std::string error;
            Check(EncodeResults(c, left.results, 1 << 24, &l, &error) == 0, error);
            Check(EncodeResults(c, right.results, 1 << 24, &r, &error) == 0, error);
            Check(l->Equals(*r), "continued evaluation/forecast equality");
            if (left.relation) {
                Check(EncodeFusion(c, input, *left.relation, 1 << 24, &l, &error) == 0, error);
                Check(EncodeFusion(c, input, *right.relation, 1 << 24, &r, &error) == 0, error);
                Check(l->Equals(*r), "continued relation fusion equality");
            }
        }
    std::string left, right;
    Check(source.ExportCheckpoint(&left) == 0, source.LastError());
    Check(target.ExportCheckpoint(&right) == 0, target.LastError());
    Check(left == right, "continued full checkpoint equality");
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
ConfigSnapshot WithRestore(const ConfigSnapshot& config, const char* policy) {
    rapidjson::Document doc;
    doc.Parse(config.original_json.c_str());
    auto& a = doc.GetAllocator();
    doc["persistence"].AddMember("restore", rapidjson::Value(policy, a), a);
    rapidjson::StringBuffer b;
    rapidjson::Writer<rapidjson::StringBuffer> w(b);
    doc.Accept(w);
    ConfigSnapshot output;
    auto status = ParseConfig({b.GetString(), b.GetSize()}, &output);
    Check(status.ok(), status.message);
    Check(output.sha256_hex != config.sha256_hex, "exact configuration hash changes");
    return output;
}
void FaultRecovery(std::shared_ptr<IDatabaseChannel> target) {
    for (int mode = 1; mode <= 4; ++mode) {
        auto unique = std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
        ConfigSnapshot c;
        Check(ParseConfig(Configuration("sqlite.test", "facts", false, "fault-" + unique), &c).ok(), "fault config");
        auto wrapped = std::make_shared<FaultTarget>(target);
        DurableStore store;
        Check(store.Open(c, wrapped.get(), "first") == 0, store.LastError());
        Querier owner;
        EvaluationEngine engine;
        Check(engine.Open(c, &owner.baseline, &owner.baseline, &owner.baseline) == 0, engine.LastError());
        auto publish = [&](int64_t bucket) {
            EvaluationOutput output;
            Check(engine.Submit(DirectInput(c, BaselineTaskKind::kValue, bucket), &output) == 0, engine.LastError());
            output.results.insert(output.results.end(), output.forecasts.begin(), output.forecasts.end());
            std::string checkpoint, error;
            Check(engine.ExportCheckpoint(&checkpoint) == 0, engine.LastError());
            std::shared_ptr<arrow::RecordBatch> batch;
            Check(EncodeResults(c, output.results, 1 << 24, &batch, &error) == 0, error);
            return store.Publish(checkpoint, {1, {{"d", "generic-epoch", std::to_string(bucket), bucket + 1}}},
                                 {batch});
        };
        Check(publish(0) == 0, store.LastError());
        wrapped->fault->mode = mode;
        Check(publish(1) != 0, "injected failure stops publication");
        Check(store.Generation() == 1, "confirmed generation unchanged");
        store.Close();
        engine.Close();
        std::shared_ptr<IDatabaseAtomicSessionV1> inspection;
        Check(dynamic_cast<IDatabaseAtomicTargetV1*>(target.get())->AcquireAtomicSession({}, &inspection).ok(),
              "inspection");
        std::shared_ptr<arrow::RecordBatch> rows;
        Check(ReadAtomicBatch(*inspection, "SELECT current_generation FROM baseline_tasks WHERE task_key=?",
                              {TextParameter(c.config.task_key)}, &rows)
                  .ok(),
              "durable generation");
        auto actual = std::static_pointer_cast<arrow::Int64Array>(rows->column(0))->Value(0);
        Check(actual == (mode == 3 ? 2 : 1), "known/unknown commit durable outcome");
        Check(inspection->Begin().ok(), "expire writer");
        auto p = TextParameter(c.config.task_key);
        uint64_t n;
        Check(
            inspection->ExecutePrepared("UPDATE baseline_tasks SET lease_deadline=0 WHERE task_key=?", &p, 1, &n).ok(),
            "expire lease");
        Check(inspection->Commit().status.ok(), "expire commit");
        inspection->Close();
        auto saved = ReadModel(c, target.get());
        Check(saved.generation == actual && saved.progress.datasets[0].closed_before_bucket == actual,
              "restore inspects actual atomic generation/positions");
        EvaluationEngine restarted;
        Check(restarted.Open(c, &owner.baseline, &owner.baseline, &owner.baseline) == 0, restarted.LastError());
        Check(restarted.RestoreCheckpoint(saved.checkpoint) == 0, restarted.LastError());
        Check(restarted.ClassifyReplay(DirectInput(c, BaselineTaskKind::kValue, 1)) == (mode == 3 ? 1 : 0),
              "unknown restart never repeats committed evaluation/forecast");
        auto require = WithRestore(c, "require");
        DurableStore required;
        Check(required.Open(require, target.get(), "required") == 0, required.LastError());
        StoredGeneration require_model;
        Check(required.Load(&require_model) == 0, required.LastError());
        {
            Querier require_owner;
            EvaluationEngine require_engine;
            Check(require_engine.Open(require, &require_owner.baseline, &require_owner.baseline,
                                      &require_owner.baseline) == 0,
                  require_engine.LastError());
            Check(require_engine.RestoreCheckpoint(require_model.checkpoint) == 0, require_engine.LastError());
        }
        required.Close();
        auto fresh = WithRestore(c, "fresh");
        DurableStore fresh_store;
        Check(fresh_store.Open(fresh, target.get(), "fresh") != 0, "fresh rejects existing model");
        fresh_store.Close();
        auto mismatch = c;
        auto at = mismatch.original_json.find("UTC");
        mismatch.original_json.replace(at, 3, "Asia/Shanghai");
        Check(ParseConfig(mismatch.original_json, &mismatch).ok(), "incompatible configuration");
        DurableStore invalid;
        Check(invalid.Open(mismatch, target.get(), "invalid") != 0, "config mismatch rejected");
        invalid.Close();
    }
    ConfigSnapshot c;
    Check(ParseConfig(
              Configuration("sqlite.test", "facts", false,
                            "missing-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count())),
              &c)
              .ok(),
          "missing config");
    c.config.persistence.restore = "require";
    DurableStore required;
    Check(required.Open(c, target.get(), "required") != 0, "require rejects missing model");
    required.Close();
    c.config.persistence.restore = "if_exists";
    DurableStore optional;
    Check(optional.Open(c, target.get(), "optional") == 0, optional.LastError());
    StoredGeneration empty;
    Check(optional.Load(&empty) == 0 && empty.generation == 0, "if_exists cold start");
    optional.Close();
    c.config.task_key += "-fresh";
    c.config.persistence.restore = "fresh";
    DurableStore fresh;
    Check(fresh.Open(c, target.get(), "fresh") == 0, fresh.LastError());
    fresh.Close();
}

void Matrix(database::DatabasePlugin& db, const std::shared_ptr<Loaded>& owner, Querier& querier,
            const std::string& source_type, const std::string& target_type, bool poll, bool history = false) {
    auto source = db.AcquireChannel(source_type.c_str(), ("t2" + source_type).c_str());
    auto target = db.AcquireChannel(target_type.c_str(), ("t2" + target_type).c_str());
    assert(source && target);
    auto unique = std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    auto table = "baseline_recovery_" + unique;
    auto u = source_type == "sqlite"     ? "TEXT"
             : source_type == "mysql"    ? "BIGINT UNSIGNED"
             : source_type == "postgres" ? "NUMERIC(20,0)"
                                         : "UInt64";
    auto f = source_type == "clickhouse" ? "Float64" : "DOUBLE PRECISION";
    auto i = source_type == "clickhouse" ? "Int64" : "BIGINT";
    auto ddl = std::string("CREATE TABLE ") + table + "(id " + u + ",g " + u + ",t " + i + ",v " + f + ",n " + f +
               ",den " + f + ",x " + f + ")" +
               (source_type == "clickhouse" ? " ENGINE=MergeTree ORDER BY(t,id,g)" : "");
    Check(source->ExecuteSql(ddl.c_str()) >= 0, source->GetLastError());
    std::string sql = "INSERT INTO " + table + " VALUES ";
    for (int64_t t = 0; t < 40; ++t)
        for (int g = 1; g <= 3; ++g) {
            if (t || g != 1) sql += ",";
            sql += "('1','" + std::to_string(g) + "'," + std::to_string(t) + ",100,800,1000," +
                   std::to_string(g * 100 + t) + ")";
        }
    Check(source->ExecuteSql(sql.c_str()) >= 0, source->GetLastError());
    std::shared_ptr<IDatabaseChannel> effective = source;
    if (poll) effective = std::make_shared<GenericSource>(source);
    ConfigSnapshot c;
    auto config_json = Configuration(source_type + ".t2" + source_type, table, poll, "recover-" + unique);
    if (history) {
        auto at = config_json.find("\"forecast\"");
        config_json.insert(at,
                           "\"bootstrap\":{\"mode\":\"history\",\"begin_bucket\":0,\"end_bucket\":20,\"min_"
                           "observations\":18,\"insufficient\":\"cold\"},");
    }
    auto parsed = ParseConfig(config_json, &c);
    Check(parsed.ok(), parsed.path + ":" + parsed.message);
    // Reference consumes identical standard observations without persistence or restart.
    Querier reference_owner;
    EvaluationEngine reference;
    Check(reference.Open(c, &reference_owner.baseline, &reference_owner.baseline, &reference_owner.baseline) == 0,
          reference.LastError());
    std::vector<std::shared_ptr<arrow::RecordBatch>> expected;
    auto accept = [&](const std::shared_ptr<arrow::RecordBatch>& batch) {
        std::vector<Observation> rows;
        std::string error;
        Check(DecodeObservations(batch, 1 << 24, &rows, &error) == 0, error);
        for (auto& o : rows) {
            EvaluationOutput output;
            Check(reference.Submit(o, &output) == 0, reference.LastError());
            output.results.insert(output.results.end(), output.forecasts.begin(), output.forecasts.end());
            if (!output.results.empty()) {
                std::shared_ptr<arrow::RecordBatch> b;
                Check(EncodeResults(c, output.results, 1 << 24, &b, &error) == 0, error);
                expected.push_back(b);
            }
        }
    };
    if (poll) {
        PollInput input;
        Check(input.Initialize(c, effective, c.config.source) == 0, input.LastError());
        for (int k = 0; k < 5000; ++k) {
            auto event = input.PollBlock(5);
            if (event.kind == BlockPollEvent::kTimeout) break;
            Check(event.kind == BlockPollEvent::kData, input.LastError());
            accept(event.batch);
            assert(input.ReleaseBlock(event.batch) == 0);
        }
    } else {
        SnapshotInput input;
        auto snapshot = c;
        snapshot.sha256_hex = RestoreCompatibilityHash(c);
        Check(input.Initialize(snapshot, effective, c.config.source) == 0, input.LastError());
        for (;;) {
            auto event = input.PollBlock();
            if (event.kind == BlockPollEvent::kEof) break;
            Check(event.kind == BlockPollEvent::kData, input.LastError());
            accept(event.batch);
            assert(input.ReleaseBlock(event.batch) == 0);
        }
    }
    Check(reference.Finish() == 0, reference.LastError());
    std::string expected_checkpoint;
    Check(reference.ExportCheckpoint(&expected_checkpoint) == 0, reference.LastError());
    // Crash midway through the input window, before released window progress is durable.
    {
        Session initial(owner, c, effective, target.get());
        for (int k = 0; k < 15; ++k) Check(initial.Step(), "initial partial window");
    }
    if (poll) {
        auto generic = std::static_pointer_cast<GenericSource>(effective);
        generic->facts->first = 1;
        RejectRestoredSource(owner, c, effective, target.get());
        generic->facts->first = 0;
        generic->facts->epoch = "changed";
        RejectRestoredSource(owner, c, effective, target.get());
        generic->facts->epoch = "generic-epoch";
    }
    {
        Session restarted(owner, c, effective, target.get());
        restarted.Finish();
    }
    auto saved = ReadModel(c, target.get());
    rapidjson::Document task_checkpoint;
    task_checkpoint.Parse(saved.checkpoint.c_str());
    Check(!task_checkpoint.HasParseError() && task_checkpoint.HasMember("engine"), "task checkpoint");
    Check(std::string(task_checkpoint["engine"].GetString(), task_checkpoint["engine"].GetStringLength()) ==
              expected_checkpoint,
          "restored checkpoint differs from continuous " + source_type + " -> " + target_type +
              (poll ? " poll" : " snapshot"));
    if (poll) assert(saved.progress.datasets.size() == 1 && saved.progress.datasets[0].closed_before_bucket == 40);
    std::shared_ptr<IDatabaseAtomicSessionV1> inspection;
    assert(dynamic_cast<IDatabaseAtomicTargetV1*>(target.get())->AcquireAtomicSession({}, &inspection).ok());
    std::shared_ptr<arrow::RecordBatch> rows;
    assert(ReadAtomicBatch(*inspection, "SELECT COUNT(*) AS n FROM baseline_results_v1 WHERE task_key=?",
                           {TextParameter(c.config.task_key)}, &rows)
               .ok());
    int64_t count = 0;
    for (const auto& b : expected) count += b->num_rows();
    assert(std::static_pointer_cast<arrow::Int64Array>(rows->column(0))->Value(0) == count);
    for (const auto& b : expected)
        for (int64_t row = 0; row < b->num_rows(); ++row) {
            auto text = [&](const char* name) {
                return std::static_pointer_cast<arrow::StringArray>(b->GetColumnByName(name))->GetString(row);
            };
            const auto bucket =
                           std::static_pointer_cast<arrow::Int64Array>(b->GetColumnByName("target_bucket"))->Value(row),
                       issued = std::static_pointer_cast<arrow::Int64Array>(b->GetColumnByName("issued_after_bucket"))
                                    ->Value(row);
            auto metric = text("metric_id"), kind = text("result_kind"), basis = text("model_basis_id");
            assert(ReadAtomicBatch(
                       *inspection,
                       "SELECT observed,expected,lower,upper FROM baseline_results_v1 WHERE task_key=? AND metric_id=? "
                       "AND result_kind=? AND target_bucket=? AND issued_after_bucket=? AND model_basis_id=?",
                       {TextParameter(c.config.task_key), TextParameter(metric), TextParameter(kind),
                        IntParameter(bucket), IntParameter(issued), TextParameter(basis)},
                       &rows)
                       .ok());
            // Relation summaries can share these predicates; scalar Value/Ratio have exactly one row.
            if (metric == "v" || metric == "r") {
                assert(rows->num_rows() == 1);
                for (int column = 0; column < 4; ++column) {
                    auto actual = rows->column(column), expected_value = b->GetColumnByName(std::vector<std::string>{
                                                            "observed", "expected", "lower", "upper"}[column]);
                    assert(actual->IsNull(0) == expected_value->IsNull(row));
                    if (!actual->IsNull(0))
                        assert(std::static_pointer_cast<arrow::DoubleArray>(actual)->Value(0) ==
                               std::static_pointer_cast<arrow::DoubleArray>(expected_value)->Value(row));
                }
            }
        }
    inspection->Close();
    // A second restart after publication adds no evaluation or forecast rows.
    {
        Session restart_after_commit(owner, c, effective, target.get());
        restart_after_commit.Finish();
    }
    assert(dynamic_cast<IDatabaseAtomicTargetV1*>(target.get())->AcquireAtomicSession({}, &inspection).ok());
    assert(ReadAtomicBatch(*inspection, "SELECT COUNT(*) AS n FROM baseline_results_v1 WHERE task_key=?",
                           {TextParameter(c.config.task_key)}, &rows)
               .ok());
    assert(std::static_pointer_cast<arrow::Int64Array>(rows->column(0))->Value(0) == count);
    inspection->Close();
    {
        auto require = WithRestore(c, "require");
        Session restarted_with_require(owner, require, effective, target.get());
        restarted_with_require.Finish();
    }
    Check(source->ExecuteSql(("DROP TABLE " + table).c_str()) >= 0, source->GetLastError());
    std::cout << "PASS recovery " << source_type << " -> " << target_type << (poll ? " poll" : " snapshot")
              << std::endl;
}
}  // namespace
int main() {
    const char* path = std::getenv("BASELINER_TEST_DB_OPTIONS");
    assert(path);
    std::ifstream input(path);
    std::ostringstream options;
    options << input.rdbuf();
    database::DatabasePlugin db;
    int saved = dup(STDOUT_FILENO);
    FILE* quiet = fopen("/dev/null", "w");
    dup2(fileno(quiet), STDOUT_FILENO);
    int rc = db.Option(options.str().c_str());
    fflush(stdout);
    dup2(saved, STDOUT_FILENO);
    close(saved);
    fclose(quiet);
    assert(rc == 0 && db.Load(nullptr) == 0 && db.Start() == 0);
    {
        Querier q;
        auto owner = std::make_shared<Loaded>(q);
        InvalidRestore();
        for (auto target : {"sqlite", "mysql", "postgres"})
            FaultRecovery(db.AcquireChannel(target, ("t2" + std::string(target)).c_str()));
        for (auto target : {"sqlite", "mysql", "postgres"})
            for (bool poll : {false, true}) Matrix(db, owner, q, "sqlite", target, poll, true);
        for (auto source : {"sqlite", "mysql", "postgres", "clickhouse"})
            for (auto target : {"sqlite", "mysql", "postgres"})
                for (bool poll : {false, true}) Matrix(db, owner, q, source, target, poll);
    }
    db.Stop();
    db.Unload();
}
