// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <dlfcn.h>
#include <framework/core/pipeline.h>
#include <framework/interfaces/cpp_operator_plugin_abi.h>
#include <framework/interfaces/iblock_transform_execution_policy.h>
#include <framework/interfaces/iblock_transform_operator.h>
#include <operators/baseliner/evaluation.h>
#include <operators/baseliner/poll_input.h>
#include <operators/baseliner/result_codec.h>
#include <operators/baseliner/writer_fence.h>
#include <plugins/baseline/baseline_plugin.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <services/database/database_channel.h>
#include <services/database/database_plugin.h>
#include <unistd.h>
#include <cassert>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
using namespace flowsql;
using namespace flowsql::baseliner;
namespace {
constexpr auto json = R"({"schema_version":1,"task_key":"operator","source":"sqlite.t4","mode":"snapshot",
"clock":{"bucket_seconds":60,"timezone":"UTC"},"calendar":{"calendar_id":"cn-holiday","calendar_version":"2026.1"},
"forecast":{"horizon_buckets":2},
"datasets":[{"id":"d","table":"facts","fields":{"id":"uint64","t":"int64","v":"float64"},
"scope":{"begin_bucket":0,"end_bucket":40,"consistency":"consistent_snapshot"},"series_keys":["id"],
"deduplicate":{"keys":["id","t"],"on_duplicate":"require_equal"},"bucket":{"column":"t","unit":"bucket_id"},
"metrics":[{"id":"v","kind":"value","column":"v","feature_type":"value_basic","profile":"default"}]}]})";
std::string With(const std::string& config) {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    writer.StartObject();
    writer.Key("parameters");
    writer.String(config.data(), config.size());
    writer.EndObject();
    return {buffer.GetString(), buffer.GetSize()};
}
struct Querier : IQuerier, IConfigChannelRegistryV1 {
    baseline::BaselinePlugin baseline;
    ConfigSnapshot config;
    int lookups = 0;
    Querier() {
        assert(ParseConfig(json, &config).ok());
        assert(baseline.Option(nullptr) == 0 && baseline.Load(this) == 0 && baseline.Start() == 0);
    }
    ~Querier() {
        baseline.Stop();
        baseline.Unload();
    }
    void* First(const Guid& id) override {
        if (std::memcmp(&id, &IID_BASELINE_SERVICE, sizeof(Guid)) == 0)
            return static_cast<IBaselineService*>(&baseline);
        if (std::memcmp(&id, &IID_BASELINE_STATE_CONTROL_SERVICE_V1, sizeof(Guid)) == 0)
            return static_cast<IBaselineStateControlServiceV1*>(&baseline);
        if (std::memcmp(&id, &IID_BASELINE_CHECKPOINT_SERVICE_V1, sizeof(Guid)) == 0)
            return static_cast<IBaselineCheckpointServiceV1*>(&baseline);
        if (std::memcmp(&id, &IID_CONFIG_CHANNEL_REGISTRY_V1, sizeof(Guid)) == 0)
            return static_cast<IConfigChannelRegistryV1*>(this);
        return nullptr;
    }
    int Traverse(const Guid& id, fntraverse fn) override {
        auto* value = First(id);
        return value ? fn(value) : 0;
    }
    int Resolve(const char* ref, ConfigChannelSnapshot* out, std::string*) override {
        ++lookups;
        if (std::string(ref) != "config.t4@1") return -1;
        *out = {"t4",
                1,
                "json",
                "baseliner.task.v1",
                config.sha256_hex,
                config.original_json.size(),
                0,
                std::make_shared<const std::string>(config.original_json)};
        return 0;
    }
};
struct Loaded {
    void* handle = nullptr;
    IBlockTransformOperatorV2* provider = nullptr;
    CppOperatorPluginDestroyCapabilityV2Fn destroy = nullptr;
    explicit Loaded(Querier& q) {
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
auto Task(const std::shared_ptr<Loaded>& owner, const std::string& with = With(json)) {
    BlockTransformTaskConfigV2 c{kBlockTransformTaskConfigV2Size, kBlockTransformContractVersionV2, "execution",
                                 with.c_str(), ""};
    IBlockTransformTaskV2* raw = nullptr;
    assert(owner->provider->CreateTask(c, &raw) == 0 && raw);
    return std::unique_ptr<IBlockTransformTaskV2, std::function<void(IBlockTransformTaskV2*)>>(
        raw, [owner](auto* task) { owner->provider->ReleaseTask(task); });
}
Observation Input(int64_t bucket) {
    Observation o;
    o.dataset_id = "d";
    o.metric_id = "v";
    o.kind = BaselineTaskKind::kValue;
    o.source_epoch = "epoch";
    o.bucket = bucket;
    o.value = 100 + bucket % 3;
    o.sample_count = 1;
    assert(EncodeIdentity("sqlite.t4", "d", "v", o.kind, {uint64_t{1}}, &o.identity).ok());
    return o;
}
std::shared_ptr<arrow::RecordBatch> Batch(const std::vector<Observation>& rows) {
    std::shared_ptr<arrow::RecordBatch> batch;
    std::string error;
    assert(MakeObservationBatch(rows, 1 << 24, &batch, &error) == 0);
    return batch;
}
struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false, cancelled = false;
    int Block() {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        cv.notify_all();
        // Bounded failure makes a missed wake reproducible without hanging CTest.
        assert(cv.wait_for(lock, std::chrono::seconds(3), [&] { return cancelled; }));
        return -1;
    }
    void Wait() {
        std::unique_lock<std::mutex> lock(mutex);
        assert(cv.wait_for(lock, std::chrono::seconds(3), [&] { return entered; }));
    }
    void Cancel() {
        std::lock_guard<std::mutex> lock(mutex);
        cancelled = true;
        cv.notify_all();
    }
};
struct ReadSession : IDatabaseSnapshotSessionV1 {
    std::shared_ptr<IDatabaseSnapshotSessionV1> inner;
    Gate* gate;
    std::atomic<int>* live;
    ReadSession(std::shared_ptr<IDatabaseSnapshotSessionV1> s, Gate* g, std::atomic<int>* l)
        : inner(std::move(s)), gate(g), live(l) {
        ++*live;
    }
    ~ReadSession() override { --*live; }
    int ReadPage(const char* sql, const DatabaseParameterV1* p, size_t n, std::shared_ptr<arrow::Schema> schema,
                 uint32_t rows, uint64_t bytes, std::shared_ptr<arrow::RecordBatch>* output) override {
        output->reset();
        return gate ? gate->Block() : inner->ReadPage(sql, p, n, std::move(schema), rows, bytes, output);
    }
    void Cancel() override {
        if (gate) gate->Cancel();
        inner->Cancel();
    }
    std::string LastError() const override { return "cancelled test session"; }
};
struct ProgressReader : IDatabasePublishedProgressReaderV1 {
    Gate* gate;
    explicit ProgressReader(Gate* g) : gate(g) {}
    int ReadProgress(const PublishedDatasetRequestV1& r, const std::string&, PublishedDatasetProgressV1* out) override {
        *out = {};
        if (gate) return gate->Block();
        out->progress = {r.dataset_id, "epoch", "1", 40};
        out->source_period_ns = 60000000000LL;
        return 0;
    }
    void Cancel() override {
        if (gate) gate->Cancel();
    }
    std::string LastError() const override { return "cancelled progress"; }
};
struct AtomicSession : IDatabaseAtomicSessionV1 {
    std::shared_ptr<IDatabaseAtomicSessionV1> inner;
    Gate* gate;
    std::atomic<int>* live;
    AtomicSession(std::shared_ptr<IDatabaseAtomicSessionV1> s, Gate* g, std::atomic<int>* l)
        : inner(std::move(s)), gate(g), live(l) {
        ++*live;
    }
    ~AtomicSession() override { --*live; }
    DatabaseAtomicStatusV1 Begin() override { return inner->Begin(); }
    DatabaseAtomicStatusV1 ExecutePrepared(const char* sql, const DatabaseParameterV1* p, size_t n,
                                           uint64_t* rows) override {
        if (gate && std::string(sql).find("INSERT INTO baseline_model_versions") != std::string::npos) {
            *rows = 0;
            gate->Block();
            return {DatabaseAtomicCodeV1::kCancelled, "cancelled publication"};
        }
        return inner->ExecutePrepared(sql, p, n, rows);
    }
    DatabaseAtomicStatusV1 CreateReader(const char* s, const DatabaseParameterV1* p, size_t n,
                                        IBatchReader** r) override {
        return inner->CreateReader(s, p, n, r);
    }
    DatabaseAtomicStatusV1 ReadDatabaseTime(int64_t* t) override { return inner->ReadDatabaseTime(t); }
    DatabaseCommitResultV1 Commit() override { return inner->Commit(); }
    DatabaseAtomicStatusV1 Rollback() override { return inner->Rollback(); }
    void Cancel() override {
        if (gate) gate->Cancel();
        inner->Cancel();
    }
    void Close() override { inner->Close(); }
};
struct Source : IDatabaseChannel, IDatabaseSnapshotSourceV1, IDatabasePublishedSourceV1, IDatabaseAtomicTargetV1 {
    std::shared_ptr<IDatabaseChannel> real;
    Gate* snapshot_gate = nullptr;
    Gate* progress_gate = nullptr;
    Gate* atomic_gate = nullptr;
    std::atomic<int> atomic_live{0};
    int block_session = 1, session_number = 0;
    std::atomic<int> live{0};
    explicit Source(std::shared_ptr<IDatabaseChannel> s) : real(std::move(s)) {}
    const char* Category() override { return "sqlite"; }
    const char* Name() override { return "t4"; }
    const char* Type() override { return ChannelType::kDatabase; }
    const char* Schema() override { return "[]"; }
    int Open() override { return 0; }
    int Close() override { return 0; }
    int Flush() override { return 0; }
    int CreateReader(const char* q, IBatchReader** p) override { return real->CreateReader(q, p); }
    int CreateWriter(const char* q, IBatchWriter** p) override { return real->CreateWriter(q, p); }
    int CreateArrowReader(const char* q, IArrowReader** p) override { return real->CreateArrowReader(q, p); }
    int CreateArrowWriter(const char* q, IArrowWriter** p) override { return real->CreateArrowWriter(q, p); }
    int ExecuteQueryArrow(const char* q, std::vector<std::shared_ptr<arrow::RecordBatch>>* p) override {
        return real->ExecuteQueryArrow(q, p);
    }
    int WriteArrowBatches(const char* q, const std::vector<std::shared_ptr<arrow::RecordBatch>>& p) override {
        return real->WriteArrowBatches(q, p);
    }
    int ExecuteSql(const char* q) override { return real->ExecuteSql(q); }
    const char* GetLastError() override { return real->GetLastError(); }
    bool IsOpened() const override { return true; }
    bool IsConnected() override { return true; }
    DatabaseAtomicStatusV1 AcquireAtomicSession(const DatabaseAtomicSessionOptionsV1& options,
                                                std::shared_ptr<IDatabaseAtomicSessionV1>* out) override {
        std::shared_ptr<IDatabaseAtomicSessionV1> session;
        auto status = dynamic_cast<IDatabaseAtomicTargetV1*>(real.get())->AcquireAtomicSession(options, &session);
        if (status.ok()) *out = std::make_shared<AtomicSession>(std::move(session), atomic_gate, &atomic_live);
        return status;
    }
    int CreateSnapshotSession(const DatabaseSnapshotOptionsV1& options,
                              std::shared_ptr<IDatabaseSnapshotSessionV1>* out) override {
        std::shared_ptr<IDatabaseSnapshotSessionV1> s;
        int rc = dynamic_cast<IDatabaseSnapshotSourceV1*>(real.get())->CreateSnapshotSession(options, &s);
        if (rc) return rc;
        *out = std::make_shared<ReadSession>(std::move(s), ++session_number == block_session ? snapshot_gate : nullptr,
                                             &live);
        return 0;
    }
    int CreatePublishedProgressReader(const DatabaseSnapshotOptionsV1&,
                                      std::shared_ptr<IDatabasePublishedProgressReaderV1>* out) override {
        *out = std::make_shared<ProgressReader>(progress_gate);
        return 0;
    }
};
ConfigSnapshot Config(bool poll = false) {
    ConfigSnapshot c;
    assert(ParseConfig(json, &c).ok());
    if (poll) {
        c.config.mode = Mode::kPoll;
        c.config.datasets[0].scope.consistency = "immutable_range";
    }
    return c;
}
void ReaderCancellation(const std::shared_ptr<IDatabaseChannel>& real) {
    for (int kind = 0; kind < 5; ++kind) {
        Gate gate;
        auto source = std::make_shared<Source>(real);
        if (kind == 1)
            source->progress_gate = &gate;
        else
            source->snapshot_gate = &gate;
        if (kind == 3) source->block_session = 2;  // poll window Open, after selection probe
        auto begin = std::chrono::steady_clock::now();
        int rc = 0;
        if (kind == 0) {
            SnapshotInput input;
            std::thread worker([&] { rc = input.Initialize(Config(), source, "sqlite.t4"); });
            gate.Wait();
            input.Cancel();
            worker.join();
            assert(rc != 0);
            input.Close();
            input.Close();
        } else if (kind < 4) {
            PollInput input;
            if (kind == 3) assert(input.Initialize(Config(true), source, "sqlite.t4") == 0);
            std::thread worker([&] {
                rc = kind == 3 ? (input.PollBlock(100).kind == BlockPollEvent::kCancelled ? -1 : 0)
                               : input.Initialize(Config(true), source, "sqlite.t4");
            });
            gate.Wait();
            input.Cancel();
            worker.join();
            assert(rc != 0);
            input.Close();
            input.Close();
            assert(input.PendingStarts().empty() && input.SourceEpochs().empty());
        } else {
            Querier q;
            auto owner = std::make_shared<Loaded>(q);
            auto task = Task(owner);
            assert(dynamic_cast<IBlockTransformInputSourceTaskV1*>(task.get())->BindInputSource("sqlite.t4") == 0);
            auto* capability = dynamic_cast<IBlockTransformDatabaseInputTaskV1*>(task.get());
            BlockDatabaseInputV1 input;
            BlockDatabaseInputBindingV1 binding;
            binding.source = source;
            binding.exact_source = "sqlite.t4";
            std::thread worker([&] { rc = capability->CreateDatabaseInput(binding, &input); });
            gate.Wait();
            task->Cancel();
            worker.join();
            assert(rc != 0 && !input.input && !input.schema);
            task.reset();
        }
        assert(source->live == 0);
        assert(std::chrono::steady_clock::now() - begin < std::chrono::seconds(2));
    }
    std::cout << "PASS cancellation during snapshot/task/progress/selection/window initialization, join before close\n";
}
class SerialTask : public IBlockTransformTaskV2 {
 public:
    int processes = 0, times = 0, flushes = 0;
    int active = 0;
    bool stop = false, fail = false, timed = false;
    Gate* gate = nullptr;
    int Open(std::shared_ptr<arrow::Schema> in, std::shared_ptr<arrow::Schema>* out) override {
        *out = in;
        return 0;
    }
    int ProcessBlock(const std::shared_ptr<arrow::RecordBatch>& b, int64_t t,
                     std::vector<BlockTransformOutputV1>* out) override {
        assert(active++ == 0);
        ++processes;
        if (gate) {
            gate->Block();
            --active;
            return -ECANCELED;
        }
        --active;
        if (fail) return -EIO;
        out->push_back({b, t});
        return static_cast<int>(stop ? BlockTransformStatusV1::kStop : BlockTransformStatusV1::kContinue);
    }
    int Flush(std::vector<BlockTransformOutputV1>*) override {
        assert(active++ == 0);
        ++flushes;
        --active;
        return 0;
    }
    void Cancel() override {
        if (gate) gate->Cancel();
    }
    std::string LastError() const override { return "test failure"; }
    int GetTimeDriveState(BlockTransformTimeDriveStateV1* s) override {
        s->armed = timed && !times;
        s->deadline_ns = 0;
        return 0;
    }
    int OnTime(const BlockTransformTimeEventV1&, std::vector<BlockTransformOutputV1>*) override {
        assert(active++ == 0);
        ++times;
        --active;
        return 0;
    }
};
class ArrowSource : public IBlockStreamChannel {
 public:
    std::shared_ptr<arrow::RecordBatch> batch = Batch({Input(0)});
    std::atomic<bool> cancelled{false};
    bool sent = false, released = false, error = false;
    const char* Category() override { return "test"; }
    const char* Name() override { return "lifecycle"; }
    const char* Type() override { return ChannelType::kBlockStream; }
    const char* Schema() override { return "[]"; }
    int Open() override { return 0; }
    int Close() override { return 0; }
    bool IsOpened() const override { return true; }
    int Flush() override { return 0; }
    bool IsFinished() const override { return sent; }
    BlockPollEvent PollBlock(int) override {
        if (cancelled) return {BlockPollEvent::kCancelled, nullptr, ECANCELED};
        if (error) return {BlockPollEvent::kError, nullptr, EIO};
        if (sent) return {BlockPollEvent::kEof, nullptr, 0};
        sent = true;
        return {BlockPollEvent::kData, batch};
    }
    int ReleaseBlock(const std::shared_ptr<arrow::RecordBatch>& b) override {
        assert(b == batch);
        released = true;
        return 0;
    }
    void Cancel() override { cancelled = true; }
};
void PipelineLifecycle() {
    for (int mode = 0; mode < 6; ++mode) {
        ArrowSource source;
        SerialTask task;
        Gate gate;
        task.stop = mode == 1;
        task.fail = mode == 3;
        task.timed = true;
        source.error = mode == 2;
        if (mode == 5) task.gate = &gate;
        BlockTransformPipelineConfig c;
        c.source = &source;
        c.source_schema = source.batch->schema();
        c.transform = &task;
        c.time_transform = &task;
        c.control = std::make_shared<BlockTransformRunControl>();
        c.output_consumer = [&](const auto&) { return mode == 4 ? EIO : 0; };
        BlockTransformPipelineRunner runner(c);
        BlockTransformPipelineResult result;
        std::string error;
        BlockTransformPipelineError rc;
        std::thread worker([&] { rc = runner.Run(&result, &error); });
        if (mode == 5) {
            gate.Wait();
            c.control->Request(true);
        }
        worker.join();
        assert(task.active == 0 && task.times == 1);
        assert(task.flushes == (mode < 2 ? 1 : 0));
        assert((rc == BlockTransformPipelineError::kNone) == (mode < 2));
        if (mode != 2) assert(source.released);
        // Callback lease has been unbound before task/source leave scope.
        c.control->Request(true);
    }
    std::cout << "PASS EOF/Stop once Flush, source/transform/sink errors and concurrent Cancel without Flush, serial "
                 "callbacks\n";
}
class CountTask : public IBlockTransformTaskV2 {
 public:
    IBlockTransformTaskV2* inner;
    std::shared_ptr<BlockTransformRunControl> control;
    int flushes = 0;
    bool request_stop = false, request_cancel = false;
    explicit CountTask(IBlockTransformTaskV2* t) : inner(t) {}
    int Open(std::shared_ptr<arrow::Schema> s, std::shared_ptr<arrow::Schema>* o) override { return inner->Open(s, o); }
    int ProcessBlock(const std::shared_ptr<arrow::RecordBatch>& b, int64_t t,
                     std::vector<BlockTransformOutputV1>* o) override {
        int rc = inner->ProcessBlock(b, t, o);
        if (request_stop && rc >= 0) control->Request(false);
        if (request_cancel && rc >= 0) control->Request(true);
        return rc;
    }
    int Flush(std::vector<BlockTransformOutputV1>* o) override {
        ++flushes;
        return inner->Flush(o);
    }
    void Cancel() override { inner->Cancel(); }
    std::string LastError() const override { return inner->LastError(); }
    int GetTimeDriveState(BlockTransformTimeDriveStateV1* s) override { return inner->GetTimeDriveState(s); }
    int OnTime(const BlockTransformTimeEventV1& e, std::vector<BlockTransformOutputV1>* o) override {
        return inner->OnTime(e, o);
    }
};
void ManagedLifecycle(const std::shared_ptr<IDatabaseChannel>& real) {
    Querier q;
    auto owner = std::make_shared<Loaded>(q);
    for (int mode = 0; mode < 7; ++mode) {
        auto source = std::make_shared<Source>(real);
        Gate gate;
        auto text = std::string(json);
        const auto key = "lifecycle_" + std::to_string(mode);
        text.replace(text.find("\"task_key\":\"operator\""), 21, "\"task_key\":\"" + key + "\"");
        const auto persistence =
            "\"persistence\":{\"checkpoint_every_buckets\":" + std::to_string(mode == 4 ? 1 : 1000) + "},";
        text.insert(text.find("\"forecast\":"), persistence);
        if (mode == 3) text.insert(text.find("\"forecast\":"), "\"state_policy\":{\"max_runtime_identities\":1},");
        if (mode >= 5) {
            text.replace(text.find("\"snapshot\""), std::strlen("\"snapshot\""), "\"poll\"");
            text.erase(text.find(",\"consistency\":\"consistent_snapshot\""),
                       std::strlen(",\"consistency\":\"consistent_snapshot\""));
        }
        auto task = Task(owner, With(text));
        assert(dynamic_cast<IBlockTransformInputSourceTaskV1*>(task.get())->BindInputSource("sqlite.t4") == 0);
        if (mode == 4) source->atomic_gate = &gate;
        BlockTransformManagedSinkBindingV1 sink;
        sink.sink_channel = source.get();
        sink.target = "sqlite.t4";
        sink.category = "sqlite";
        sink.name = "t4";
        assert(dynamic_cast<IBlockTransformManagedSinkTaskV1*>(task.get())->BindManagedSink(sink) == 0);
        ArrowSource input;
        input.error = mode == 2;
        if (mode == 3) {
            auto second = Input(0);
            assert(EncodeIdentity("sqlite.t4", "d", "v", second.kind, {uint64_t{2}}, &second.identity).ok());
            input.batch = Batch({Input(0), second});
        }
        CountTask counted(task.get());
        counted.request_stop = mode == 1 || mode == 5;
        counted.request_cancel = mode == 6;
        BlockTransformPipelineConfig c;
        c.source = &input;
        c.source_schema = input.batch->schema();
        BlockDatabaseInputV1 database_input;
        auto* input_capability = dynamic_cast<IBlockTransformDatabaseInputTaskV1*>(task.get());
        if (mode >= 5) {
            BlockDatabaseInputBindingV1 binding;
            binding.source = source;
            binding.exact_source = "sqlite.t4";
            assert(input_capability->CreateDatabaseInput(binding, &database_input) == 0);
            c.source = database_input.input;
            c.source_schema = database_input.schema;
            c.input_progress_task = dynamic_cast<IBlockTransformInputProgressTaskV1*>(task.get());
        }
        c.transform = &counted;
        c.time_transform = &counted;
        c.control = std::make_shared<BlockTransformRunControl>();
        counted.control = c.control;
        c.output_consumer = [](const auto&) { return 0; };
        BlockTransformPipelineRunner runner(c);
        BlockTransformPipelineResult result;
        std::string error;
        BlockTransformPipelineError rc;
        std::thread worker([&] { rc = runner.Run(&result, &error); });
        if (mode == 4) {
            gate.Wait();
            c.control->Request(true);
        }
        worker.join();
        if ((mode < 2 || mode == 5) && rc != BlockTransformPipelineError::kNone)
            std::cerr << "managed mode=" << mode << " " << error << std::endl;
        assert(counted.flushes == (mode < 2 || mode == 5 ? 1 : 0));
        assert((rc == BlockTransformPipelineError::kNone) == (mode < 2 || mode == 5));
        if (mode >= 5) {
            input_capability->ReleaseDatabaseInput(database_input.input);
            database_input.schema.reset();
        }
        if (mode == 6) assert(result.terminal == BlockTransformPipelineTerminal::kCancelled);
        task.reset();
        assert(source->live == 0);
        assert(source->atomic_live == 0);
        std::shared_ptr<IDatabaseAtomicSessionV1> session;
        assert(dynamic_cast<IDatabaseAtomicTargetV1*>(real.get())->AcquireAtomicSession({}, &session).ok());
        assert(session->Begin().ok());
        std::shared_ptr<arrow::RecordBatch> generation;
        assert(ReadAtomicBatch(*session, "SELECT current_generation FROM baseline_tasks WHERE task_key='" + key + "'",
                               {}, &generation)
                   .ok());
        assert(generation->num_rows() == 1);
        assert(std::static_pointer_cast<arrow::Int64Array>(generation->column(0))->Value(0) ==
               (mode < 2 || mode == 5 ? 1 : 0));
        generation.reset();
        assert(session->Rollback().ok());
        session->Close();
    }
    std::cout << "PASS managed EOF/Stop including in-flight poll durable tail; source/budget/Cancel no generation, "
                 "cancelled publication joins "
                 "before handle release\n";
}
void LibraryOwnership() {
    Querier q;
    std::weak_ptr<Loaded> weak;
    struct Products {
        std::shared_ptr<Loaded> owner;
        std::shared_ptr<arrow::Schema> schema;
        std::vector<BlockTransformOutputV1> outputs;
    };
    std::unique_ptr<Products> products;
    {
        auto owner = std::make_shared<Loaded>(q);
        weak = owner;
        auto task = Task(owner);
        assert(dynamic_cast<IBlockTransformInputSourceTaskV1*>(task.get())->BindInputSource("sqlite.t4") == 0);
        products = std::make_unique<Products>();
        products->owner = owner;
        owner.reset();
        assert(!weak.expired());
        assert(task->Open(MakeSchema(SchemaKind::kObservation), &products->schema) == 0);
        assert(task->ProcessBlock(Batch({Input(0)}), 0, &products->outputs) == 0);
        assert(!products->outputs.empty());
    }
    assert(!weak.expired());
    assert(products->outputs[0].batch->ValidateFull().ok());
    products.reset();
    assert(weak.expired());
    std::cout << "PASS task and owned Arrow products destroyed before plugin library RAII owner\n";
}
void FailedOutputIsEmpty() {
    Querier q;
    auto owner = std::make_shared<Loaded>(q);
    auto text = std::string(json);
    text.replace(text.find("\"mode\":\"snapshot\""), 17, "\"mode\":\"poll\"");
    text.insert(text.find("\"forecast\":"), "\"state_policy\":{\"idle_timeout_ms\":1},");
    text.erase(text.find(",\"consistency\":\"consistent_snapshot\""),
               std::strlen(",\"consistency\":\"consistent_snapshot\""));
    auto task = Task(owner, With(text));
    assert(dynamic_cast<IBlockTransformInputSourceTaskV1*>(task.get())->BindInputSource("sqlite.t4") == 0);
    std::shared_ptr<arrow::Schema> schema;
    assert(task->Open(MakeSchema(SchemaKind::kObservation), &schema) == 0);
    std::vector<BlockTransformOutputV1> out;
    assert(task->ProcessBlock(Batch({Input(0)}), 0, &out) == 0);
    out.clear();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    auto bad = Input(1);
    bad.metric_id = "unknown";
    assert(task->ProcessBlock(Batch({bad}), 0, &out) < 0 && out.empty());
    assert(task->Flush(&out) != 0 && out.empty());
    std::cout << "PASS maintenance followed by validation failure leaves outputs empty and forbids Flush\n";
}
void RealDatabaseCancellation() {
    const char* path = std::getenv("BASELINER_TEST_DB_OPTIONS");
    assert(path);
    std::ifstream input(path);
    std::ostringstream options;
    options << input.rdbuf();
    database::DatabasePlugin plugin;
    int saved = dup(STDOUT_FILENO);
    FILE* quiet = fopen("/dev/null", "w");
    fflush(stdout);
    dup2(fileno(quiet), STDOUT_FILENO);
    int option_rc = plugin.Option(options.str().c_str());
    fflush(stdout);
    dup2(saved, STDOUT_FILENO);
    close(saved);
    fclose(quiet);
    assert(option_rc == 0 && plugin.Load(nullptr) == 0 && plugin.Start() == 0);
    for (const std::string backend : {"sqlite", "mysql", "postgres", "clickhouse"}) {
        auto source = plugin.AcquireChannel(backend.c_str(), ("t2" + backend).c_str());
        assert(source);
        const auto table = "baseliner_cancel_" + std::to_string(getpid());
        assert(source->ExecuteSql(("DROP TABLE IF EXISTS " + table).c_str()) >= 0);
        const auto ddl = backend == "clickhouse"
                             ? "CREATE TABLE " + table + " (value Int64) ENGINE=MergeTree ORDER BY tuple()"
                             : "CREATE TABLE " + table + " (value BIGINT)";
        assert(source->ExecuteSql(ddl.c_str()) >= 0);
        assert(source->ExecuteSql(("INSERT INTO " + table + " VALUES(1)").c_str()) >= 0);
        std::string sql = "SELECT value FROM " + table;
        if (backend == "sqlite")
            sql +=
                " WHERE (WITH RECURSIVE n(x) AS (VALUES(0) UNION ALL SELECT x+1 FROM n WHERE x<1000000000) SELECT "
                "SUM(x) FROM n)>0";
        else if (backend == "mysql")
            sql += " WHERE SLEEP(10)=0";
        else if (backend == "postgres")
            sql += ", pg_sleep(10)";
        else
            sql += " WHERE sleep(3)=0";
        DatabaseSnapshotOptionsV1 o;
        o.operation_timeout_ms = 15000;
        o.consistent_transaction = backend != "clickhouse";
        std::shared_ptr<IDatabaseSnapshotSessionV1> session;
        assert(dynamic_cast<IDatabaseSnapshotSourceV1*>(source.get())->CreateSnapshotSession(o, &session) == 0);
        std::atomic<bool> done{false};
        int rc = 0;
        std::shared_ptr<arrow::RecordBatch> batch;
        std::thread worker([&] {
            rc = session->ReadPage(sql.c_str(), nullptr, 0, arrow::schema({arrow::field("value", arrow::int64())}), 10,
                                   65536, &batch);
            done = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        assert(!done);
        auto begin = std::chrono::steady_clock::now();
        session->Cancel();
        worker.join();
        auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count();
        assert(rc != 0 && !batch && ms < 1500);
        session.reset();
        std::cout << "PASS real snapshot cancellation " << backend << " wake_ms=" << ms << '\n';
        if (backend != "clickhouse") {
            std::shared_ptr<IDatabaseAtomicSessionV1> atomic;
            DatabaseAtomicSessionOptionsV1 a;
            a.operation_timeout_ms = 15000;
            assert(dynamic_cast<IDatabaseAtomicTargetV1*>(source.get())->AcquireAtomicSession(a, &atomic).ok());
            assert(atomic->Begin().ok());
            done = false;
            DatabaseAtomicStatusV1 status;
            std::thread active([&] {
                status = ReadAtomicBatch(*atomic, sql, {}, &batch);
                done = true;
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            assert(!done);
            begin = std::chrono::steady_clock::now();
            atomic->Cancel();
            active.join();
            ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count();
            assert(!status.ok() && !batch && ms < 1500);
            atomic->Close();
            atomic->Close();
            atomic.reset();
            std::cout << "PASS real atomic cancellation " << backend << " wake_ms=" << ms << '\n';
        }
        assert(source->ExecuteSql(("DROP TABLE " + table).c_str()) >= 0);
    }
    plugin.Stop();
    plugin.Unload();
}
void EngineClose() {
    Querier q;
    EvaluationEngine engine;
    assert(engine.Open(q.config, &q.baseline, &q.baseline, &q.baseline) == 0);
    EvaluationOutput output;
    assert(engine.Submit(Input(0), &output) == 0);
    assert(engine.QueryUsage().second.runtime_identities > 0);
    engine.Close();
    engine.Close();
    auto usage = engine.QueryUsage().second;
    assert(usage.runtime_identities == 0 && usage.model_identities == 0 && usage.routed_states == 0 &&
           usage.retained_basis_versions == 0);
    std::cout << "PASS repeated engine Close releases usage\n";
}
}  // namespace
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    char path[] = "/tmp/baseliner-lifecycle-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    database::DatabasePlugin database;
    auto options = std::string("type=sqlite;name=t4;path=") + path;
    assert(database.Option(options.c_str()) == 0 && database.Load(nullptr) == 0 && database.Start() == 0);
    auto real = database.AcquireChannel("sqlite", "t4");
    assert(real && real->ExecuteSql("CREATE TABLE facts(id BIGINT,t BIGINT,v DOUBLE)") >= 0);
    assert(real->ExecuteSql("INSERT INTO facts VALUES(1,0,100),(1,1,110),(1,2,120)") >= 0);
    ReaderCancellation(real);
    PipelineLifecycle();
    ManagedLifecycle(real);
    FailedOutputIsEmpty();
    LibraryOwnership();
    EngineClose();
    real.reset();
    database.Stop();
    database.Unload();
    unlink(path);
    RealDatabaseCancellation();
}
