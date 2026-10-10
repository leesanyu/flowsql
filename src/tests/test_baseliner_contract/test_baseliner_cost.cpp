// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <dlfcn.h>
#include <framework/core/dataframe_channel.h>
#include <framework/interfaces/cpp_operator_plugin_abi.h>
#include <framework/interfaces/iblock_transform_operator.h>
#include <operators/baseliner/dataframe_reader.h>
#include <operators/baseliner/durable_store.h>
#include <operators/baseliner/model_output.h>
#include <operators/baseliner/result_codec.h>
#include <plugins/baseline/baseline_plugin.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <services/database/database_plugin.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cassert>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
using namespace flowsql;
using namespace flowsql::baseliner;
namespace {
void Check(bool ok, const std::string& error) {
    if (!ok) {
        std::cerr << error << std::endl;
        std::abort();
    }
}
using Clock = std::chrono::steady_clock;
double Ms(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }
uint64_t Rss() {
    std::ifstream f("/proc/self/statm");
    uint64_t size = 0, resident = 0;
    f >> size >> resident;
    return resident * sysconf(_SC_PAGESIZE);
}
struct Totals {
    int64_t count = 0;
    double observed = 0, expected = 0, lower = 0, upper = 0;
    void Add(const EvaluationRow& r) {
        ++count;
        observed += r.values.observed.value_or(0);
        expected += r.values.expected.value_or(0);
        lower += r.values.lower.value_or(0);
        upper += r.values.upper.value_or(0);
    }
    void Add(const arrow::RecordBatch& b) {
        if (!b.GetColumnByName("result_kind")) return;
        count += b.num_rows();
        for (auto item : {std::make_pair("observed", &observed), std::make_pair("expected", &expected),
                          std::make_pair("lower", &lower), std::make_pair("upper", &upper)})
            for (int64_t r = 0; r < b.num_rows(); ++r)
                if (!b.GetColumnByName(item.first)->IsNull(r))
                    *item.second += std::stod(b.GetColumnByName(item.first)->GetScalar(r).ValueOrDie()->ToString());
    }
};
class DelaySession final : public IDatabaseAtomicSessionV1 {
 public:
    DelaySession(std::shared_ptr<IDatabaseAtomicSessionV1> s, int delay, uint64_t* commits)
        : s_(std::move(s)), delay_(delay), commits_(commits) {}
    DatabaseAtomicStatusV1 Begin() override { return s_->Begin(); }
    DatabaseAtomicStatusV1 ExecutePrepared(const char* q, const DatabaseParameterV1* p, size_t n,
                                           uint64_t* a) override {
        return s_->ExecutePrepared(q, p, n, a);
    }
    DatabaseAtomicStatusV1 CreateReader(const char* q, const DatabaseParameterV1* p, size_t n,
                                        IBatchReader** r) override {
        return s_->CreateReader(q, p, n, r);
    }
    DatabaseAtomicStatusV1 ReadDatabaseTime(int64_t* t) override { return s_->ReadDatabaseTime(t); }
    DatabaseCommitResultV1 Commit() override {
        ++*commits_;
        if (delay_) std::this_thread::sleep_for(std::chrono::milliseconds(delay_));
        return s_->Commit();
    }
    DatabaseAtomicStatusV1 Rollback() override { return s_->Rollback(); }
    void Cancel() override { s_->Cancel(); }
    void Close() override { s_->Close(); }

 private:
    std::shared_ptr<IDatabaseAtomicSessionV1> s_;
    int delay_;
    uint64_t* commits_;
};
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
std::string Configuration(const std::string& source, const std::string& table, bool poll, const std::string& key) {
    std::string result =
        R"({"schema_version":1,"task_key":")" + key + R"(","source":")" + source + R"(","mode":")" +
        (poll ? "poll" : "snapshot") + R"(",
"clock":{"bucket_seconds":60,"timezone":"UTC"},"calendar":{"calendar_id":"cn-holiday","calendar_version":"2026.1"},
"forecast":{"horizon_buckets":2},"read_policy":{"page_rows":256,"poll_interval_ms":1},
"persistence":{"checkpoint_every_buckets":7,"checkpoint_interval_ms":1000,"retain_generations":2},
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
class DelayTarget final : public IDatabaseChannel, public IDatabaseAtomicTargetV1 {
 public:
    explicit DelayTarget(std::shared_ptr<IDatabaseChannel> target) : target_(std::move(target)) {}
    int delay_ms = 0;
    uint64_t commits = 0;
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
        if (s.ok()) *output = std::make_shared<DelaySession>(session, delay_ms, &commits);
        return s;
    }

 private:
    std::shared_ptr<IDatabaseChannel> target_;
};

std::vector<Observation> Inputs(const ConfigSnapshot& c, int bucket) {
    std::vector<Observation> rows;
    for (uint64_t id = 1; id <= 8; ++id)
        for (auto kind : {BaselineTaskKind::kValue, BaselineTaskKind::kRatio, BaselineTaskKind::kRelation}) {
            Observation o;
            o.dataset_id = "d";
            o.metric_id = kind == BaselineTaskKind::kValue ? "v" : kind == BaselineTaskKind::kRatio ? "r" : "dist";
            o.kind = kind;
            o.source_epoch = "cost-epoch";
            o.bucket = bucket;
            assert(EncodeIdentity(c.config.source, "d", o.metric_id, kind, {id}, &o.identity).ok());
            if (kind == BaselineTaskKind::kValue) {
                o.value = 100 + bucket % 3;
                o.sample_count = 1;
            } else if (kind == BaselineTaskKind::kRatio) {
                o.numerator = 800 + bucket % 3;
                o.denominator = 1000;
            } else {
                o.groups = {1, 2, 3};
                o.metrics = {{"m", 1000. + bucket, 3, {500., 300., 200. + bucket}}};
            }
            rows.push_back(std::move(o));
        }
    return rows;
}
void Scenario(int mode, const std::string& root) {
    Querier q;
    ConfigSnapshot c;
    assert(ParseConfig(Configuration("sqlite.cost", "facts", false, "cost"), &c).ok());
    std::vector<std::vector<Observation>> inputs;
    std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
    for (int bucket = 0; bucket < 40; ++bucket) {
        inputs.push_back(Inputs(c, bucket));
        std::shared_ptr<arrow::RecordBatch> b;
        std::string e;
        assert(MakeObservationBatch(inputs.back(), 1 << 24, &b, &e) == 0);
        batches.push_back(b);
    }
    database::DatabasePlugin db;
    auto dbopt = "type=sqlite;name=cost;path=" + root + "/cost-" + std::to_string(mode) + ".db";
    assert(db.Option(dbopt.c_str()) == 0 && db.Load(nullptr) == 0 && db.Start() == 0);
    Totals totals;
    uint64_t checkpoint_bytes = 0, commits = 0;
    double elapsed = 0, maintenance = 0, max_batch = 0, cpu = 0;
    auto rss_before = Rss();
    {
        auto channel = db.AcquireChannel("sqlite", "cost");
        assert(channel);
        DelayTarget target(channel);
        target.delay_ms = mode == 3 ? 2 : 0;
        auto owner = std::make_shared<Loaded>(q);
        auto task = Task(owner, With(c.original_json));
        EvaluationEngine engine;
        if (mode == 0) {
            assert(engine.Open(c, &q.baseline, &q.baseline, &q.baseline) == 0);
        } else {
            assert(dynamic_cast<IBlockTransformInputSourceTaskV1*>(task.get())->BindInputSource("sqlite.cost") == 0);
            if (mode >= 2) {
                BlockTransformManagedSinkBindingV1 binding;
                binding.sink_channel = &target;
                binding.target = "sqlite.cost";
                binding.category = "sqlite";
                binding.name = "cost";
                Check(dynamic_cast<IBlockTransformManagedSinkTaskV1*>(task.get())->BindManagedSink(binding) == 0,
                      task->LastError());
            }
            std::shared_ptr<arrow::Schema> output;
            Check(task->Open(MakeSchema(SchemaKind::kObservation), &output) == 0, task->LastError());
        }
        auto start = Clock::now();
        auto cpu_start = std::clock();
        double verification_ms = 0, verification_cpu_ms = 0;
        for (size_t bucket = 0; bucket < inputs.size(); ++bucket) {
            auto begin = Clock::now();
            auto previous_verification = verification_ms;
            if (mode == 0) {
                for (const auto& o : inputs[bucket]) {
                    EvaluationOutput out;
                    Check(engine.Submit(o, &out) == 0, engine.LastError());
                    auto vstart = Clock::now();
                    auto vcpu = std::clock();
                    for (const auto& row : out.results) totals.Add(row);
                    for (const auto& row : out.forecasts) totals.Add(row);
                    verification_ms += Ms(Clock::now() - vstart);
                    verification_cpu_ms += 1000. * (std::clock() - vcpu) / CLOCKS_PER_SEC;
                }
            } else {
                std::vector<BlockTransformOutputV1> outputs;
                Check(task->ProcessBlock(batches[bucket], 0, &outputs) == 0, task->LastError());
                auto vstart = Clock::now();
                auto vcpu = std::clock();
                for (const auto& out : outputs) totals.Add(*out.batch);
                verification_ms += Ms(Clock::now() - vstart);
                verification_cpu_ms += 1000. * (std::clock() - vcpu) / CLOCKS_PER_SEC;
            }
            max_batch = std::max(max_batch, Ms(Clock::now() - begin) - (verification_ms - previous_verification));
        }
        // A no-data timer tick includes the pending checkpoint in managed modes.
        auto mstart = Clock::now();
        if (mode == 0) {
            std::vector<MaintenanceRow> rows;
            assert(engine.Maintain(&rows) == 0);
        } else {
            BlockTransformTimeEventV1 event{};
            event.struct_size = sizeof(event);
            event.contract_version = 1;
            event.monotonic_now_ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count() +
                2000000000;
            event.wall_now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count() +
                                2000000000;
            std::vector<BlockTransformOutputV1> outputs;
            Check(task->OnTime(event, &outputs) == 0, task->LastError());
        }
        maintenance = Ms(Clock::now() - mstart);
        if (mode == 0)
            assert(engine.Finish() == 0);
        else {
            std::vector<BlockTransformOutputV1> outputs;
            Check(task->Flush(&outputs) == 0, task->LastError());
        }
        elapsed = Ms(Clock::now() - start) - verification_ms;
        cpu = 1000. * (std::clock() - cpu_start) / CLOCKS_PER_SEC - verification_cpu_ms;
        task.reset();
        engine.Close();
        owner.reset();
        commits = target.commits;
        if (mode >= 2) {
            std::shared_ptr<IDatabaseAtomicSessionV1> session;
            assert(dynamic_cast<IDatabaseAtomicTargetV1*>(channel.get())->AcquireAtomicSession({}, &session).ok());
            std::shared_ptr<arrow::RecordBatch> b;
            assert(ReadAtomicBatch(*session,
                                   "SELECT COUNT(*),SUM(observed),SUM(expected),SUM(lower),SUM(upper) FROM "
                                   "baseline_results_v1 WHERE task_key='cost'",
                                   {}, &b)
                       .ok());
            totals.count = std::stoll(b->column(0)->GetScalar(0).ValueOrDie()->ToString());
            double* values[] = {&totals.observed, &totals.expected, &totals.lower, &totals.upper};
            for (int i = 0; i < 4; ++i) *values[i] = std::stod(b->column(i + 1)->GetScalar(0).ValueOrDie()->ToString());
            session->Close();
            DurableStore store;
            Check(store.Open(c, channel.get(), "measure") == 0, store.LastError());
            StoredGeneration saved;
            assert(store.Load(&saved) == 0);
            checkpoint_bytes = saved.checkpoint.size();
            store.Close();
        }
    }
    db.Stop();
    db.Unload();
    auto service = q.baseline.QueryServiceSnapshot(BaselineSerializationFormat::kJson);
    assert(service.first == BaselineStatus::kOk);
    rapidjson::Document state;
    state.Parse(service.second.c_str());
    assert(!state.HasParseError() && state["task_count"].GetUint64() == 0);

    rusage usage{};
    assert(getrusage(RUSAGE_SELF, &usage) == 0);
    std::ofstream f(root + "/mode-" + std::to_string(mode) + ".json");
    f.precision(17);
    f << "{\"mode\":" << mode
      << ",\"observations\":960,\"identities\":24,\"relation_groups\":3,\"relation_metrics\":1,\"forecast_horizon\":2,"
         "\"batch_observations\":24,\"commit_delay_ms\":"
      << (mode == 3 ? 2 : 0) << ",\"wall_ms\":" << elapsed << ",\"cpu_ms\":" << cpu
      << ",\"observations_per_second\":" << 960000. / elapsed << ",\"rss_before_bytes\":" << rss_before
      << ",\"rss_after_bytes\":" << Rss() << ",\"peak_rss_bytes\":" << usage.ru_maxrss * 1024
      << ",\"max_batch_ms\":" << max_batch << ",\"no_data_maintenance_ms\":" << maintenance
      << ",\"commits\":" << commits << ",\"checkpoint_bytes\":" << checkpoint_bytes
      << ",\"result_rows\":" << totals.count << ",\"sum_observed\":" << totals.observed
      << ",\"sum_expected\":" << totals.expected << ",\"sum_lower\":" << totals.lower
      << ",\"sum_upper\":" << totals.upper << "}\n";
}

// Raw-source costs use the same rows in every mode. Fixture creation and verification are outside
// the timed region; Open includes fingerprinting, filtering, conversion and stable sorting together.
std::shared_ptr<arrow::RecordBatch> RawInput(int mode) {
    const bool narrow = mode >= 2;
    DataFrame frame;
    frame.SetSchema({{"id", narrow ? DataType::UINT32 : DataType::UINT64, 0, ""},
                     {"g", narrow ? DataType::UINT32 : DataType::UINT64, 0, ""},
                     {"t", narrow ? DataType::INT32 : DataType::INT64, 0, ""},
                     {"v", narrow ? DataType::FLOAT : DataType::DOUBLE, 0, ""},
                     {"n", narrow ? DataType::FLOAT : DataType::DOUBLE, 0, ""},
                     {"den", narrow ? DataType::FLOAT : DataType::DOUBLE, 0, ""},
                     {"x", narrow ? DataType::FLOAT : DataType::DOUBLE, 0, ""}});
    for (int i = 0; i < 960; ++i) {
        const int row = mode == 0 ? i : 959 - i;
        const int bucket = row / 24, id = (row % 24) / 3 + 1, group = row % 3 + 1;
        const double value = group == 1 ? 100 + bucket % 3 : 0;
        const double numerator = group == 1 ? 800 + bucket % 3 : 0;
        const double denominator = group == 1 ? 1000 : 0;
        const double x = group == 1 ? 500 : group == 2 ? 300 : 200 + bucket;
        Check((narrow ? frame.AppendRow({uint32_t(id), uint32_t(group), int32_t(bucket), float(value), float(numerator),
                                         float(denominator), float(x)})
                      : frame.AppendRow(
                            {uint64_t(id), uint64_t(group), int64_t(bucket), value, numerator, denominator, x})) == 0,
              "raw cost fixture append");
    }
    return frame.ToArrow();
}

void InputScenario(int mode, const std::string& root) {
    Querier q;
    rapidjson::Document document;
    document.Parse(Configuration("dataframe.costinput", "facts", false, "cost-input").c_str());
    document["datasets"][0].RemoveMember("table");
    document["datasets"][0]["scope"]["consistency"].SetString("dataframe_snapshot", document.GetAllocator());
    rapidjson::StringBuffer text;
    rapidjson::Writer<rapidjson::StringBuffer> writer(text);
    document.Accept(writer);
    ConfigSnapshot c;
    Check(ParseConfig({text.GetString(), text.GetSize()}, &c).ok(), "raw cost configuration");
    auto raw = RawInput(mode);
    auto source = std::make_shared<DataFrameChannel>("dataframe", "costinput");
    Check(source->Open() == 0, "raw source open");
    DataFrame original;
    original.FromArrow(raw);
    Check(source->Write(&original) == 0, "raw source write");
    BlockDataFrameInputBindingV1 binding;
    binding.source = source;
    binding.exact_source = "dataframe.costinput";
    database::DatabasePlugin db;
    auto options = "type=sqlite;name=cost;path=" + root + "/input-" + std::to_string(mode) + ".db";
    Check(db.Option(options.c_str()) == 0 && db.Load(nullptr) == 0 && db.Start() == 0, "raw target database");
    Totals totals;
    uint64_t observations = 0, model_rows = 0;
    double open_ms = 0, export_ms = 0, publish_ms = 0, setup_ms = 0, wall_ms = 0, cpu_ms = 0;
    DataFrameReadStats stats;
    const auto rss_before = Rss();
    {
        auto channel = db.AcquireChannel("sqlite", "cost");
        ModelOutputStore results, models;
        DataFrameChannel result_frame("dataframe", "results"), model_frame("dataframe", "models");
        Check(result_frame.Open() == 0 && model_frame.Open() == 0, "output frames open");
        auto setup = Clock::now();
        if (mode == 4) {
            Check(models.Open(c, channel, "models") == 0, models.LastError());
            Check(results.Open(c, channel, "results", SchemaKind::kResults) == 0, results.LastError());
            Check(results.Join(models) == 0 && results.SharesSession(models), results.LastError());
        }
        setup_ms = Ms(Clock::now() - setup);
        EvaluationEngine engine;
        Check(engine.Open(c, &q.baseline, &q.baseline, &q.baseline) == 0, engine.LastError());
        DataFrameReader reader;
        BucketAggregator aggregate(c.config, "cost-input-epoch");
        std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
        std::shared_ptr<arrow::RecordBatch> parameters;
        const auto start = Clock::now();
        const auto cpu_start = std::clock();
        Check(reader.Open(c.config, binding, raw) == 0, reader.LastError());
        open_ms = Ms(Clock::now() - start);
        for (;;) {
            SnapshotPage page;
            Check(reader.Next(&page) == 0, reader.LastError());
            std::vector<Observation> inputs;
            Check((page.eof ? aggregate.FinishDataset(page.dataset_index, &inputs)
                            : aggregate.Push(page.dataset_index, page.batch, &inputs)) == 0,
                  aggregate.LastError());
            observations += inputs.size();
            std::vector<EvaluationRow> rows;
            for (const auto& input : inputs) {
                EvaluationOutput output;
                Check(engine.Submit(input, &output) == 0, engine.LastError());
                rows.insert(rows.end(), output.results.begin(), output.results.end());
                rows.insert(rows.end(), output.forecasts.begin(), output.forecasts.end());
            }
            if (!rows.empty()) {
                std::shared_ptr<arrow::RecordBatch> batch;
                std::string error;
                Check(EncodeResults(c, rows, c.config.read.max_pending_bytes, &batch, &error) == 0, error);
                batches.push_back(std::move(batch));
            }
            if (page.eof) break;
        }
        Check(engine.Finish() == 0, engine.LastError());
        stats = reader.Stats();
        if (mode >= 3) {
            auto begin = Clock::now();
            std::vector<ModelParametersRow> rows;
            Check(engine.ExportModelParameters(&rows) == 0, engine.LastError());
            std::string error;
            Check(EncodeModelParameters(c, rows, c.config.read.max_pending_bytes, &parameters, &error) == 0, error);
            export_ms = Ms(Clock::now() - begin);
            model_rows = rows.size();
        }
        auto publish = Clock::now();
        if (mode == 4) {
            Check(results.Write(batches, &models, parameters) == 0, results.LastError());
        } else {
            for (const auto& batch : batches) {
                DataFrame frame;
                frame.FromArrow(batch);
                Check(result_frame.Append(&frame) == 0, "result frame append");
            }
            if (parameters) {
                DataFrame frame;
                frame.FromArrow(parameters);
                Check(model_frame.Write(&frame) == 0, "model frame write");
            }
        }
        publish_ms = Ms(Clock::now() - publish);
        wall_ms = Ms(Clock::now() - start);
        cpu_ms = 1000. * (std::clock() - cpu_start) / CLOCKS_PER_SEC;
        // Verify real published products after stopping both timers.
        for (const auto& batch : batches) totals.Add(*batch);
        if (mode == 4) {
            Check(results.RowsWritten() == totals.count && models.RowsWritten() == int64_t(model_rows),
                  "specified dual table row counts");
            results.Close();
            models.Close();
            std::shared_ptr<IDatabaseAtomicSessionV1> session;
            Check(dynamic_cast<IDatabaseAtomicTargetV1*>(channel.get())->AcquireAtomicSession({}, &session).ok(),
                  "published result inspection");
            std::shared_ptr<arrow::RecordBatch> values;
            Check(ReadAtomicBatch(*session,
                                  "SELECT COUNT(*),SUM(observed),SUM(expected),SUM(lower),SUM(upper) "
                                  "FROM results",
                                  {}, &values)
                      .ok(),
                  "read published results");
            const double expected[] = {double(totals.count), totals.observed, totals.expected, totals.lower,
                                       totals.upper};
            for (int i = 0; i < 5; ++i)
                Check(std::abs(std::stod(values->column(i)->GetScalar(0).ValueOrDie()->ToString()) - expected[i]) <=
                          1e-8 * std::max(1., std::abs(expected[i])),
                      "published result value differs");
            session->Close();
        } else {
            DataFrame frame;
            Check(result_frame.Read(&frame) == 0 && frame.RowCount() == size_t(totals.count), "published frame rows");
            if (parameters) {
                Check(model_frame.Read(&frame) == 0 && frame.ToArrow()->Equals(*parameters), "published model frame");
            }
        }
        if (parameters) {
            Check(model_rows == 24, "three metric final models");
            auto buckets = std::static_pointer_cast<arrow::Int64Array>(parameters->GetColumnByName("as_of_bucket"));
            for (int64_t row = 0; row < parameters->num_rows(); ++row)
                Check(!buckets->IsNull(row) && buckets->Value(row) == 39, "model final processed bucket");
        }
        DataFrame after;
        Check(source->Read(&after) == 0 && after.ToArrow()->Equals(*raw), "cost input changed");
        Check(observations == 960 && totals.count == 5760 && stats.selected_rows == 960 &&
                  stats.peak_buffer_bytes <= c.config.read.max_pending_bytes,
              "raw cost load or budget differs");
        engine.Close();
    }
    db.Stop();
    db.Unload();
    auto service = q.baseline.QueryServiceSnapshot(BaselineSerializationFormat::kJson);
    rapidjson::Document state;
    state.Parse(service.second.c_str());
    Check(service.first == BaselineStatus::kOk && !state.HasParseError() && state["task_count"].GetUint64() == 0,
          "raw cost task cleanup");
    rusage usage{};
    Check(getrusage(RUSAGE_SELF, &usage) == 0, "raw peak RSS");
    std::ofstream f(root + "/input-mode-" + std::to_string(mode) + ".json");
    f.precision(17);
    f << "{\"family\":\"dataframe\",\"mode\":" << mode << ",\"raw_rows\":960,\"observations\":" << observations
      << ",\"identities\":24,\"page_rows\":256" << ",\"wall_ms\":" << wall_ms << ",\"cpu_ms\":" << cpu_ms
      << ",\"input_open_ms\":" << open_ms << ",\"model_export_encode_ms\":" << export_ms
      << ",\"publication_ms\":" << publish_ms << ",\"target_setup_ms\":" << setup_ms
      << ",\"source_buffer_bytes\":" << stats.source_buffer_bytes
      << ",\"reader_peak_buffer_bytes\":" << stats.peak_buffer_bytes << ",\"rss_before_bytes\":" << rss_before
      << ",\"rss_after_bytes\":" << Rss() << ",\"peak_rss_bytes\":" << usage.ru_maxrss * 1024
      << ",\"model_rows\":" << model_rows << ",\"result_rows\":" << totals.count
      << ",\"sum_observed\":" << totals.observed << ",\"sum_expected\":" << totals.expected
      << ",\"sum_lower\":" << totals.lower << ",\"sum_upper\":" << totals.upper << "}\n";
}
}  // namespace
int main() {
    auto root = "/tmp/baseline-operator-t73/cost-" +
                std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    assert(std::filesystem::create_directories(root));
    std::vector<rapidjson::Document> measurements;
    for (int mode = 0; mode < 4; ++mode) {
        pid_t pid = fork();
        assert(pid >= 0);
        if (pid == 0) {
            Scenario(mode, root);
            std::exit(0);
        }
        int status;
        assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
        std::ifstream f(root + "/mode-" + std::to_string(mode) + ".json");
        std::ostringstream text;
        text << f.rdbuf();
        rapidjson::Document d;
        d.Parse(text.str().c_str());
        assert(!d.HasParseError());
        if (!measurements.empty())
            for (const char* field : {"result_rows", "sum_observed", "sum_expected", "sum_lower", "sum_upper"})
                Check(std::abs(d[field].GetDouble() - measurements[0][field].GetDouble()) <=
                          1e-8 * std::max(1., std::abs(measurements[0][field].GetDouble())),
                      std::string("mode output differs: ") + field);
        std::cout << "COST " << text.str() << std::flush;
        measurements.push_back(std::move(d));
    }
    assert(measurements[2]["commits"].GetUint64() == measurements[3]["commits"].GetUint64());
    std::cout
        << "PASS same-load engine / Arrow operator / managed SQLite / managed SQLite +2ms per commit; measurements "
        << root << std::endl;
    std::vector<rapidjson::Document> input_measurements;
    for (int mode = 0; mode < 5; ++mode) {
        const auto pid = fork();
        Check(pid >= 0, "input cost fork");
        if (pid == 0) {
            InputScenario(mode, root);
            std::exit(0);
        }
        int status;
        Check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0, "input cost child");
        std::ifstream f(root + "/input-mode-" + std::to_string(mode) + ".json");
        std::ostringstream text;
        text << f.rdbuf();
        rapidjson::Document d;
        d.Parse(text.str().c_str());
        Check(!d.HasParseError(), "input cost JSON");
        if (!input_measurements.empty())
            for (const char* field : {"result_rows", "sum_observed", "sum_expected", "sum_lower", "sum_upper"})
                Check(std::abs(d[field].GetDouble() - input_measurements[0][field].GetDouble()) <=
                          1e-8 * std::max(1., std::abs(input_measurements[0][field].GetDouble())),
                      std::string("raw mode output differs: ") + field);
        std::cout << "COST_INPUT " << text.str() << std::flush;
        input_measurements.push_back(std::move(d));
    }
    std::cout << "PASS same raw rows: ordered/reversed wide, reversed narrow, DataFrame models, joint SQLite outputs; "
                 "source preserved and no live models\n";
}
