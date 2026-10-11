// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <arrow/api.h>
#include <framework/core/packet_codec.h>
#include <framework/interfaces/ibaseline_service.h>
#include <framework/interfaces/iblock_stream_factory.h>
#include <framework/interfaces/iblock_stream_reader.h>
#include <framework/interfaces/iblock_transform_operator.h>
#include <framework/interfaces/ischeduler_control_service.h>
#include <operators/baseliner/model_output.h>
#include <services/database/database_plugin.h>
#include <unistd.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <common/loader.hpp>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <thread>
using namespace flowsql;
using namespace flowsql::baseliner;
namespace {
struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false, released = false;
    void Block() {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        cv.notify_all();
        assert(cv.wait_for(lock, std::chrono::seconds(10), [&] { return released; }));
    }
    void Wait() {
        std::unique_lock<std::mutex> lock(mutex);
        assert(cv.wait_for(lock, std::chrono::seconds(10), [&] { return entered; }));
    }
    void Release() {
        std::lock_guard<std::mutex> lock(mutex);
        released = true;
        cv.notify_all();
    }
};
struct Trace {
    std::atomic<int> closed{0}, cancelled{0}, in_cancel{0}, channel_destroyed{0};
};
struct Session final : IDatabaseAtomicSessionV1 {
    std::shared_ptr<IDatabaseAtomicSessionV1> inner;
    std::shared_ptr<Trace> trace;
    Gate* gate;
    Session(std::shared_ptr<IDatabaseAtomicSessionV1> s, std::shared_ptr<Trace> t, Gate* g)
        : inner(std::move(s)), trace(std::move(t)), gate(g) {}
    DatabaseAtomicStatusV1 Begin() override { return inner->Begin(); }
    DatabaseAtomicStatusV1 ExecutePrepared(const char* s, const DatabaseParameterV1* p, size_t n,
                                           uint64_t* rows) override {
        return inner->ExecutePrepared(s, p, n, rows);
    }
    DatabaseAtomicStatusV1 CreateReader(const char* s, const DatabaseParameterV1* p, size_t n,
                                        IBatchReader** r) override {
        return inner->CreateReader(s, p, n, r);
    }
    DatabaseAtomicStatusV1 ReadDatabaseTime(int64_t* t) override { return inner->ReadDatabaseTime(t); }
    DatabaseCommitResultV1 Commit() override { return inner->Commit(); }
    DatabaseAtomicStatusV1 Rollback() override { return inner->Rollback(); }
    void Cancel() override {
        ++trace->in_cancel;
        ++trace->cancelled;
        if (gate) gate->Block();
        inner->Cancel();
        --trace->in_cancel;
    }
    void Close() override {
        assert(trace->in_cancel == 0);
        assert(++trace->closed == 1);
        inner->Close();
    }
};
struct Channel final : IDatabaseChannel, IDatabaseAtomicTargetV1 {
    std::shared_ptr<IDatabaseChannel> real;
    std::shared_ptr<Trace> trace = std::make_shared<Trace>();
    Gate* cancel_gate = nullptr;
    Gate* acquire_gate = nullptr;
    bool acquired = false;
    explicit Channel(std::shared_ptr<IDatabaseChannel> c) : real(std::move(c)) {}
    ~Channel() override {
        assert(!acquired || trace->closed == 1);
        assert(trace->in_cancel == 0);
        ++trace->channel_destroyed;
    }
    const char* Category() override { return real->Category(); }
    const char* Name() override { return real->Name(); }
    const char* Type() override { return ChannelType::kDatabase; }
    const char* Schema() override { return real->Schema(); }
    int Open() override { return 0; }
    int Close() override { return 0; }
    int Flush() override { return 0; }
    bool IsOpened() const override { return true; }
    bool IsConnected() override { return true; }
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
    DatabaseAtomicStatusV1 AcquireAtomicSession(const DatabaseAtomicSessionOptionsV1& options,
                                                std::shared_ptr<IDatabaseAtomicSessionV1>* out) override {
        std::shared_ptr<IDatabaseAtomicSessionV1> session;
        auto status = dynamic_cast<IDatabaseAtomicTargetV1*>(real.get())->AcquireAtomicSession(options, &session);
        if (status.ok()) {
            acquired = true;
            *out = std::make_shared<Session>(std::move(session), trace, cancel_gate);
            if (acquire_gate) acquire_gate->Block();
        }
        return status;
    }
};
void SessionSafety(const std::shared_ptr<IDatabaseChannel>& real) {
    ConfigSnapshot config;
    config.config.read.max_pending_bytes = 1 << 20;
    for (bool result_first : {false, true}) {
        Gate gate;
        auto a = std::make_shared<Channel>(real), b = std::make_shared<Channel>(real);
        b->cancel_gate = &gate;
        auto ta = a->trace, tb = b->trace;
        ModelOutputStore result, model;
        assert(result.Open(config, a, "safety_results", SchemaKind::kResults) == 0);
        assert(model.Open(config, b, "safety_models") == 0);
        std::thread cancel([&] { model.Cancel(); });
        gate.Wait();
        assert(result.Join(model) == 0 && result.SharesSession(model));
        assert(tb->closed == 0 && ta->cancelled > 0);
        assert(result.Write(std::vector<std::shared_ptr<arrow::RecordBatch>>{}) != 0);
        a.reset();
        b.reset();
        assert(tb->channel_destroyed == 0);
        (result_first ? result : model).Close();
        assert(ta->closed == 0);
        (result_first ? model : result).Close();
        assert(ta->closed == 1 && ta->channel_destroyed == 1);
        assert(tb->closed == 0 && tb->channel_destroyed == 0);
        gate.Release();
        cancel.join();
        assert(tb->closed == 1 && tb->channel_destroyed == 1);
        result.Close();
        model.Close();
        model.Cancel();
        assert(ta->closed == 1 && tb->closed == 1);
    }
    for (bool before_join : {false, true}) {
        auto a = std::make_shared<Channel>(real), b = std::make_shared<Channel>(real);
        auto ta = a->trace, tb = b->trace;
        ModelOutputStore result, model;
        assert(result.Open(config, a, "safety_results", SchemaKind::kResults) == 0);
        assert(model.Open(config, b, "safety_models") == 0);
        if (before_join) model.Cancel();
        assert(result.Join(model) == 0);
        if (!before_join) model.Cancel();
        model.Cancel();
        assert(ta->cancelled > 0 && tb->closed == 1);
        assert(result.Write(std::vector<std::shared_ptr<arrow::RecordBatch>>{}, &model) != 0);
        model.Close();
        assert(ta->closed == 0);
        result.Close();
        assert(ta->closed == 1);
    }
    // No cancellation: either joined store may close first, and the surviving store can commit.
    for (bool result_first : {false, true}) {
        auto a = std::make_shared<Channel>(real), b = std::make_shared<Channel>(real);
        auto ta = a->trace, tb = b->trace;
        ModelOutputStore result, model;
        assert(result.Open(config, a, "safety_results", SchemaKind::kResults) == 0);
        assert(model.Open(config, b, "safety_models") == 0 && result.Join(model) == 0);
        (result_first ? result : model).Close();
        assert(ta->closed == 0 && tb->closed == 1);
        auto& surviving = result_first ? model : result;
        assert(surviving.Write(std::vector<std::shared_ptr<arrow::RecordBatch>>{}) == 0);
        assert(surviving.Write(std::vector<std::shared_ptr<arrow::RecordBatch>>{}) != 0);
        surviving.Close();
        assert(ta->closed == 1);
    }
    // Cancel before/during Open is sticky, including the gap before publishing a new owner.
    ModelOutputStore pre_cancelled;
    pre_cancelled.Cancel();
    assert(pre_cancelled.Open(config, real, "safety_pre_cancelled") != 0);
    Gate acquire;
    auto channel = std::make_shared<Channel>(real);
    channel->acquire_gate = &acquire;
    auto trace = channel->trace;
    ModelOutputStore opening;
    int rc = 0;
    std::thread open([&] { rc = opening.Open(config, channel, "safety_opening"); });
    acquire.Wait();
    opening.Cancel();
    acquire.Release();
    open.join();
    assert(rc != 0 && trace->cancelled > 0);
    opening.Close();
    assert(trace->closed == 1);
    std::cout << "PASS session handoff, both release orders, sticky cancellation and channel lease\n";
}

constexpr auto task_config = R"({"schema_version":1,"task_id":"safety","task_name":"safety",
"task_kind":"value","feature_id":"v","feature_type":"value_basic","profile":"default",
"clock_spec":{"bucket_seconds":60,"timezone":"UTC"},
"calendar_ref":{"calendar_id":"cn-holiday","calendar_version":"2026.1"}})";
struct Execution {
    Gate process, cancel;
    std::atomic<bool> submitted{false}, completed{false};
};
struct Transform final : IBlockTransformOperatorV1 {
    IBaselineService* service;
    Execution* execution;
    Transform(IBaselineService* s, Execution* e) : service(s), execution(e) {}
    struct Task final : IBlockTransformTaskV1 {
        Transform* provider;
        std::shared_ptr<IBaselineValueTask> baseline;
        std::atomic<bool> processing{false};
        explicit Task(Transform* p) : provider(p) {}
        int Open(std::shared_ptr<arrow::Schema> input, std::shared_ptr<arrow::Schema>* output) override {
            *output = input;
            return 0;
        }
        int ProcessBlock(const std::shared_ptr<arrow::RecordBatch>&, int64_t,
                         std::vector<BlockTransformOutputV1>*) override {
            auto created = provider->service->CreateValueTask(task_config, BaselineSerializationFormat::kJson);
            assert(created.first == BaselineStatus::kOk);
            baseline = std::move(created.second);
            assert(baseline->SubmitObservation({"series", 1, 100, 1}, {}).status == BaselineStatus::kOk);
            processing = true;
            provider->execution->process.Block();
            // The real Scheduler Stop is blocked inside Cancel, so Baseline must still be open.
            assert(baseline->SubmitObservation({"series", 2, 101, 1}, {}).status == BaselineStatus::kOk);
            provider->execution->submitted = true;
            return -1;
        }
        int Flush(std::vector<BlockTransformOutputV1>*) override { return 0; }
        void Cancel() override {
            if (processing) provider->execution->cancel.Block();
        }
        std::string LastError() const override { return "safety cancelled"; }
        ~Task() override {
            if (baseline) {
                assert(baseline->Close() == BaselineStatus::kOk);
                baseline.reset();
                provider->execution->completed = true;
            }
        }
    };
    std::string Category() const override { return "safety"; }
    std::string Name() const override { return "hold"; }
    std::string Description() const override { return "deterministic active Baseline task"; }
    int CreateTask(const BlockTransformTaskConfigV1&, IBlockTransformTaskV1** task) override {
        *task = new Task(this);
        return 0;
    }
    void ReleaseTask(IBlockTransformTaskV1* task) override { delete task; }
};
struct Source final : IBlockStreamChannel {
    std::atomic<bool> cancelled{false};
    bool sent = false;
    const char* Category() override { return "safety"; }
    const char* Name() override { return "input"; }
    const char* Type() override { return ChannelType::kBlockStream; }
    const char* Schema() override { return "[]"; }
    int Open() override { return 0; }
    int Close() override { return 0; }
    int Flush() override { return 0; }
    bool IsOpened() const override { return true; }
    bool IsFinished() const override { return false; }
    BlockPollEvent PollBlock(int) override {
        if (cancelled) return {BlockPollEvent::kCancelled, nullptr, 0};
        if (sent) return {BlockPollEvent::kEof, nullptr, 0};
        sent = true;
        // Scheduler's packet source schema is provided to Open; use that schema in the fixture.
        return {BlockPollEvent::kData, batch, 0};
    }
    int ReleaseBlock(const std::shared_ptr<arrow::RecordBatch>&) override { return 0; }
    void Cancel() override { cancelled = true; }
    std::shared_ptr<arrow::RecordBatch> batch;
};
struct Factory final : IBlockStreamFactory, IBlockStreamReaderFactoryV1 {
    Source source;
    IBlockStreamChannel* Get(const char* category, const char* name) override {
        return std::string(category) == "safety" && std::string(name) == "input" ? &source : nullptr;
    }
    void List(std::function<void(const char*, const char*, IBlockStreamChannel*)> fn) override {
        fn("safety", "input", &source);
    }
    int CreateReader(const BlockStreamReaderConfigV1&, IBlockStreamChannel** out) override {
        auto* reader = new Source;
        reader->batch = source.batch;
        *out = reader;
        return 0;
    }
    void ReleaseReader(IBlockStreamChannel* reader) override { delete reader; }
};
void RuntimeSafety(const std::string& root, bool scheduler_first, bool direct_unload) {
    auto* loader = PluginLoader::Single();
    const char* libs[] = {"libflowsql_builtin.so", "libflowsql_catalog.so",
                          scheduler_first ? "libflowsql_scheduler.so" : "libflowsql_baseline.so",
                          scheduler_first ? "libflowsql_baseline.so" : "libflowsql_scheduler.so"};
    const std::string catalog = "data_dir=" + root + ";operator_db_path=" + root + "/operators.db";
    const char* scheduler = "batch_workers=1;stream_workers=1";
    const char* options[] = {nullptr, catalog.c_str(), scheduler_first ? scheduler : nullptr,
                             scheduler_first ? nullptr : scheduler};
    assert(loader->Load(".", libs, options, 4) == 0);
    auto* runtime = loader->First(IID_PLUGIN_TASK_RUNTIME);
    assert(runtime);
    auto* control = static_cast<ISchedulerControlService*>(loader->First(IID_SCHEDULER_CONTROL_SERVICE));
    assert(control && dynamic_cast<ISchedulerControlService*>(static_cast<IPlugin*>(runtime)) == control);
    Execution execution;
    Transform transform(static_cast<IBaselineService*>(loader->First(IID_BASELINE_SERVICE)), &execution);
    Factory factory;
    // Packet schema is exported by the common packet contract.
    auto schema = packet::PacketSchema();
    std::vector<std::shared_ptr<arrow::Array>> arrays;
    for (const auto& field : schema->fields()) arrays.push_back(arrow::MakeArrayOfNull(field->type(), 1).ValueOrDie());
    factory.source.batch = arrow::RecordBatch::Make(schema, 1, arrays);
    loader->Regist(IID_BLOCK_STREAM_FACTORY, static_cast<IBlockStreamFactory*>(&factory));
    loader->Regist(IID_BLOCK_STREAM_READER_FACTORY_V1, static_cast<IBlockStreamReaderFactoryV1*>(&factory));
    loader->Regist(IID_BLOCK_TRANSFORM_OPERATOR_V1, static_cast<IBlockTransformOperatorV1*>(&transform));
    assert(loader->StartAll() == 0);
    std::string response;
    assert(
        control->SubmitBatch(
            R"({"runtime_task_id":"safety","sql_text":"SELECT * FROM safety.input USING safety.hold INTO dataframe.safety"})",
            &response) == 0);
    execution.process.Wait();
    std::atomic<bool> stopped{false};
    std::thread stop([&] {
        if (direct_unload)
            loader->Unload();
        else
            loader->StopAll();
        stopped = true;
    });
    execution.cancel.Wait();
    assert(!stopped);
    execution.process.Release();
    // Observe completed Submit without racing access to the Baseline task itself.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!execution.submitted && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    assert(execution.submitted && !stopped && !execution.completed);
    execution.cancel.Release();
    stop.join();
    assert(stopped && execution.completed);
    factory.source.batch.reset();
    loader->StopAll();
    loader->Unload();
    std::cout << "PASS actual Scheduler/Baseline order=" << scheduler_first << " direct_unload=" << direct_unload
              << '\n';
}
}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    const std::string mode = argc > 1 ? argv[1] : "all";
    const std::string root = "/tmp/baseliner-contract-safety-p1/safety-" + std::to_string(getpid());
    std::filesystem::create_directories(root);
    database::DatabasePlugin database;
    const auto options = "type=sqlite;name=safety;path=" + root + "/output.db";
    assert(database.Option(options.c_str()) == 0 && database.Load(nullptr) == 0 && database.Start() == 0);
    {
        auto channel = database.AcquireChannel("sqlite", "safety");
        assert(channel);
        if (mode != "runtime" && mode != "runtime-stop") SessionSafety(channel);
    }
    database.Stop();
    database.Unload();
    if (mode != "store")
        for (bool scheduler_first : {false, true})
            for (bool direct_unload : {false, true}) {
                if (mode == "runtime-stop" && direct_unload) continue;
                RuntimeSafety(root, scheduler_first, direct_unload);
            }
}
