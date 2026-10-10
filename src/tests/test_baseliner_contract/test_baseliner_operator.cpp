// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <dlfcn.h>
#include <framework/core/dataframe_channel.h>
#include <framework/core/pipeline.h>
#include <framework/interfaces/cpp_operator_plugin_abi.h>
#include <framework/interfaces/iblock_transform_dataframe_input.h>
#include <framework/interfaces/iblock_transform_execution_policy.h>
#include <framework/interfaces/iblock_transform_model_output.h>
#include <framework/interfaces/iblock_transform_operator.h>
#include <framework/interfaces/iblock_transform_result_output.h>
#include <framework/interfaces/iblock_transform_source_config.h>
#include <operators/baseliner/durable_store.h>
#include <operators/baseliner/result_codec.h>
#include <plugins/baseline/baseline_plugin.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <services/database/database_plugin.h>
#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <limits>
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
void Inspect(const std::vector<BlockTransformOutputV1>& outputs) {
    for (const auto& output : outputs) {
        assert(output.batch->schema()->Equals(*MakeSchema(SchemaKind::kResults), true) &&
               output.batch->ValidateFull().ok());
        auto kinds = std::static_pointer_cast<arrow::StringArray>(output.batch->GetColumnByName("result_kind"));
        auto actual = std::static_pointer_cast<arrow::DoubleArray>(output.batch->GetColumnByName("observed"));
        auto generation =
            std::static_pointer_cast<arrow::UInt64Array>(output.batch->GetColumnByName("published_generation"));
        for (int64_t row = 0; row < output.batch->num_rows(); ++row) {
            assert(generation->Value(row) == 0);
            if (kinds->GetString(row) == "forecast") {
                assert(actual->IsNull(row));
                for (const char* name :
                     {"score", "can_score", "can_update", "update_weight", "can_alert", "is_outside_band"})
                    assert(output.batch->GetColumnByName(name)->IsNull(row));
            } else
                assert(!actual->IsNull(row));
        }
    }
}
void ArrowPath() {
    Querier q;
    auto owner = std::make_shared<Loaded>(q);
    auto* policy = dynamic_cast<IBlockTransformExecutionPolicyProviderV1*>(owner->provider);
    assert(policy);
    bool async = true;
    assert(policy->RequiresAsyncExecution(With(json).c_str(), "sqlite.t4", &async) == 0 && !async);
    assert(policy->RequiresAsyncExecution(With(json).c_str(), "sqlite.wrong", &async) != 0);
    auto before = q.baseline.QueryServiceSnapshot(BaselineSerializationFormat::kJson);
    assert(policy->RequiresAsyncExecution(R"({"config":"config.t4@1"})", "sqlite.t4", &async) == 0);
    assert(before == q.baseline.QueryServiceSnapshot(BaselineSerializationFormat::kJson));
    const int lookups = q.lookups;
    auto exact = Task(owner, R"({"config":"config.t4@1"})");
    assert(q.lookups == lookups + 1);
    exact.reset();
    for (const char* with : {R"({"config":"config.t4@latest"})", R"({"config":"config.t4@1","config":"config.t4@1"})",
                             R"({"config":"config.t4@1","parameters":"{}"})"}) {
        BlockTransformTaskConfigV2 invalid{kBlockTransformTaskConfigV2Size, kBlockTransformContractVersionV2, "invalid",
                                           with, ""};
        IBlockTransformTaskV2* raw = nullptr;
        const auto count = q.lookups;
        assert(owner->provider->CreateTask(invalid, &raw) != 0 && !raw && count == q.lookups);
    }
    const auto with = With(json);
    BlockTransformTaskConfigV2 filtered{kBlockTransformTaskConfigV2Size, kBlockTransformContractVersionV2, "filtered",
                                        with.c_str(), R"({"version":1,"root":{"predicate":true}})"};
    IBlockTransformTaskV2* raw = nullptr;
    assert(owner->provider->CreateTask(filtered, &raw) != 0 && !raw);
    std::vector<Observation> inputs;
    for (int64_t t = 0; t < 40; ++t) inputs.push_back(Input(t));
    std::vector<std::shared_ptr<arrow::RecordBatch>> reference;
    for (bool whole : {false, true}) {
        auto task = Task(owner, R"({"config":"config.t4@1"})");
        auto* source = dynamic_cast<IBlockTransformInputSourceTaskV1*>(task.get());
        assert(source && source->BindInputSource("sqlite.t4") == 0);
        std::shared_ptr<arrow::Schema> schema;
        assert(task->Open(MakeSchema(SchemaKind::kObservation), &schema) == 0);
        std::vector<BlockTransformOutputV1> outputs;
        if (whole)
            assert(task->ProcessBlock(Batch(inputs), 123, &outputs) == 0);
        else
            for (const auto& o : inputs) {
                std::vector<BlockTransformOutputV1> one;
                assert(task->ProcessBlock(Batch({o}), 123, &one) == 0);
                outputs.insert(outputs.end(), one.begin(), one.end());
            }
        Inspect(outputs);
        assert(outputs.size() == 40);
        for (size_t i = 0; i < outputs.size(); ++i) {
            if (!whole)
                reference.push_back(outputs[i].batch);
            else
                assert(reference[i]->Equals(*outputs[i].batch));
        }
        std::vector<BlockTransformOutputV1> tail;
        assert(task->Flush(&tail) == 0 && tail.empty());
        assert(task->Flush(&tail) != 0);
    }
    reference.clear();
    auto task = Task(owner);
    auto* source = dynamic_cast<IBlockTransformInputSourceTaskV1*>(task.get());
    assert(source->BindInputSource("sqlite.t4") == 0);
    std::shared_ptr<arrow::Schema> schema;
    assert(task->Open(MakeSchema(SchemaKind::kObservation), &schema) == 0);
    std::vector<BlockTransformOutputV1> out;
    before = q.baseline.QueryServiceSnapshot(BaselineSerializationFormat::kJson);
    assert(task->ProcessBlock(Batch({Input(0), Input(0)}), 0, &out) < 0 && out.empty());
    assert(before == q.baseline.QueryServiceSnapshot(BaselineSerializationFormat::kJson));
    assert(task->Flush(&out) != 0);
    task.reset();
    auto cancelled = Task(owner);
    cancelled->Cancel();
    assert(cancelled->Open(MakeSchema(SchemaKind::kObservation), &schema) != 0);
}
void RelationArrow() {
    constexpr auto relation = R"({"schema_version":1,"task_key":"relation-arrow","source":"sqlite.t4","mode":"snapshot",
"clock":{"bucket_seconds":60,"timezone":"UTC"},"calendar":{"calendar_id":"cn-holiday","calendar_version":"2026.1"},
"forecast":{"horizon_buckets":2},"datasets":[{"id":"d","table":"facts","fields":{"g":"uint64","t":"int64","x":"float64"},
"scope":{"begin_bucket":0,"end_bucket":40,"consistency":"immutable_range"},"series_keys":["g"],
"deduplicate":{"keys":["g","t"],"on_duplicate":"require_equal"},"bucket":{"column":"t","unit":"bucket_id"},
"metrics":[{"id":"dist","kind":"relation","feature_type":"relation","profile":"default",
"group_space":{"id":"groups","version":"v1","column":"g","unknown":"other","other_group_idx":99},
"metrics":[{"id":"m","column":"x","aggregate":"sum"}],"support_policy":{"k_support":3,"min_hist_share":0.01,"min_active_ratio":0.1},
"summary_policy":{"k_head":2,"k_stable":1}}]}]})";
    Querier q;
    auto owner = std::make_shared<Loaded>(q);
    auto* provider = dynamic_cast<IBlockTransformResultOutputProviderV1*>(owner->provider);
    assert(provider && provider->SupportsResultOutput());
    Querier selected_querier;
    auto selected_owner = std::make_shared<Loaded>(selected_querier);
    auto selected = Task(selected_owner, With(relation));
    auto* result_task = dynamic_cast<IBlockTransformResultOutputTaskV1*>(selected.get());
    assert(result_task);
    BlockResultOutputBindingV1 binding;
    binding.target = "dataframe.results";
    assert(result_task->BindResultOutput(binding) == 0);
    assert(dynamic_cast<IBlockTransformInputSourceTaskV1*>(selected.get())->BindInputSource("sqlite.t4") == 0);
    std::shared_ptr<arrow::Schema> selected_schema;
    assert(selected->Open(MakeSchema(SchemaKind::kObservation), &selected_schema) == 0);
    auto task = Task(owner, With(relation));
    assert(dynamic_cast<IBlockTransformInputSourceTaskV1*>(task.get())->BindInputSource("sqlite.t4") == 0);
    std::shared_ptr<arrow::Schema> schema;
    assert(task->Open(MakeSchema(SchemaKind::kObservation), &schema) == 0);
    ConfigSnapshot config;
    assert(ParseConfig(relation, &config).ok());
    baseline::BaselinePlugin algorithm;
    assert(algorithm.Option(nullptr) == 0 && algorithm.Load(nullptr) == 0 && algorithm.Start() == 0);
    {
        EvaluationEngine reference;
        assert(reference.Open(config, &algorithm, &algorithm) == 0);
        for (int64_t t = 0; t < 8; ++t) {
            Observation input;
            input.dataset_id = "d";
            input.metric_id = "dist";
            input.source_epoch = "epoch";
            input.kind = BaselineTaskKind::kRelation;
            input.bucket = t;
            input.groups = {1, 2, 3};
            input.metrics = {{"m", 1000.0, 3, {500.0, 300.0, 200.0}}};
            assert(EncodeIdentity("sqlite.t4", "d", "dist", input.kind, {uint64_t{1}}, &input.identity).ok());
            EvaluationOutput expected;
            assert(reference.Submit(input, &expected) == 0 && expected.relation);
            std::vector<BlockTransformOutputV1> outputs;
            assert(task->ProcessBlock(Batch({input}), 0, &outputs) == 0 && outputs.size() == 2);
            std::vector<BlockTransformOutputV1> specified;
            assert(selected->ProcessBlock(Batch({input}), 0, &specified) == 0 && specified.size() == 1);
            assert(specified[0].batch->Equals(*outputs[0].batch));
            Inspect({outputs[0]});
            assert(outputs[1].batch->schema()->Equals(*MakeSchema(SchemaKind::kRelationFusion), true));
            expected.results.insert(expected.results.end(), expected.forecasts.begin(), expected.forecasts.end());
            std::shared_ptr<arrow::RecordBatch> results, fusion;
            std::string error;
            assert(EncodeResults(config, expected.results, 1 << 24, &results, &error) == 0);
            assert(EncodeFusion(config, input, *expected.relation, 1 << 24, &fusion, &error) == 0);
            assert(results->Equals(*outputs[0].batch) && fusion->Equals(*outputs[1].batch));
            assert(fusion->GetColumnByName("can_alert")->IsNull(0));
        }
    }
    std::vector<BlockTransformOutputV1> tail;
    assert(selected->Flush(&tail) == 0 && tail.empty());
    selected->Cancel();  // A completed registration remains visible even if confirmation sees cancellation.
    assert(result_task->CompleteResultOutputPublication(true) != 0);
    auto* summary = dynamic_cast<IBlockTransformManagedSinkTaskV1*>(selected.get());
    rapidjson::Document completed;
    completed.Parse(summary->ManagedSinkResultJson().c_str());
    assert(std::string(completed["results_output"]["status"].GetString()) == "committed" &&
           completed["results_output"]["rows_written"].GetInt64() > 0);
    selected.reset();
    task.reset();
    algorithm.Stop();
    algorithm.Unload();
}
void PollPolicyAndProgress() {
    auto text = std::string(json);
    text.replace(text.find("\"snapshot\""), std::strlen("\"snapshot\""), "\"poll\"");
    text.erase(text.find(",\"consistency\":\"consistent_snapshot\""),
               std::strlen(",\"consistency\":\"consistent_snapshot\""));
    Querier q;
    auto owner = std::make_shared<Loaded>(q);
    bool async = false;
    auto with = With(text);
    auto* policy = dynamic_cast<IBlockTransformExecutionPolicyProviderV1*>(owner->provider);
    const auto before = q.baseline.QueryServiceSnapshot(BaselineSerializationFormat::kJson);
    assert(policy->RequiresAsyncExecution(with.c_str(), "sqlite.t4", &async) == 0 && async);
    assert(before == q.baseline.QueryServiceSnapshot(BaselineSerializationFormat::kJson));
    auto task = Task(owner, with);
    assert(dynamic_cast<IBlockTransformInputSourceTaskV1*>(task.get())->BindInputSource("sqlite.t4") == 0);
    std::shared_ptr<arrow::Schema> schema;
    assert(task->Open(MakeSchema(SchemaKind::kObservation), &schema) == 0);
    auto* progress = dynamic_cast<IBlockTransformInputProgressTaskV1*>(task.get());
    assert(progress);
    BlockInputProgressV1 p;
    p.datasets.push_back({"d", "epoch", "position-1", 1});
    assert(progress->AcceptInputProgress(p) == 0);
    std::vector<BlockTransformOutputV1> outputs;
    assert(task->ProcessBlock(Batch({Input(0)}), 0, &outputs) == 0);
    outputs.clear();
    p.datasets[0].closed_before_bucket = 2;
    p.datasets[0].committed_position = "position-2";
    assert(progress->AcceptInputProgress(p) == 0);
    p.datasets[0].closed_before_bucket = 1;
    assert(progress->AcceptInputProgress(p) != 0);
    assert(task->ProcessBlock(Batch({Input(1)}), 0, &outputs) != 0 && outputs.empty());
    task->Cancel();
    assert(task->Flush(&outputs) != 0);
}
void DatabasePath() {
    Querier q;
    auto owner = std::make_shared<Loaded>(q);
    database::DatabasePlugin database;
    assert(database.Option("type=sqlite;name=t4;path=:memory:") == 0 && database.Load(nullptr) == 0 &&
           database.Start() == 0);
    auto source = database.AcquireChannel("sqlite", "t4");
    assert(source && source->ExecuteSql("CREATE TABLE facts(id BIGINT,t BIGINT,v DOUBLE)") >= 0);
    assert(source->ExecuteSql("INSERT INTO facts VALUES(1,0,100),(1,1,110),(1,2,120)") >= 0);
    auto task = Task(owner);
    auto* binding = dynamic_cast<IBlockTransformInputSourceTaskV1*>(task.get());
    assert(binding->BindInputSource("sqlite.t4") == 0);
    auto* input_task = dynamic_cast<IBlockTransformDatabaseInputTaskV1*>(task.get());
    assert(input_task);
    BlockDatabaseInputBindingV1 bind;
    bind.source = source;
    bind.exact_source = "sqlite.t4";
    BlockDatabaseInputV1 input;
    assert(input_task->CreateDatabaseInput(bind, &input) == 0 && input.input);
    BlockTransformPipelineConfig config;
    config.source = input.input;
    config.source_schema = input.schema;
    config.transform = task.get();
    config.time_transform = task.get();
    config.output_consumer = [&](const auto& output) {
        Inspect({output});
        return 0;
    };
    BlockTransformPipelineRunner runner(config);
    BlockTransformPipelineResult result;
    std::string error;
    auto rc = runner.Run(&result, &error);
    if (rc != BlockTransformPipelineError::kNone) std::cerr << error << '\n';
    assert(rc == BlockTransformPipelineError::kNone && result.input_rows == 3);
    input_task->ReleaseDatabaseInput(input.input);
    input.schema.reset();
    task.reset();
    source.reset();
    database.Stop();
    database.Unload();
}
void DataFrameTaskPath() {
    Querier q;
    auto owner = std::make_shared<Loaded>(q);
    auto* capability = dynamic_cast<IBlockTransformDataFrameInputProviderV1*>(owner->provider);
    assert(capability && capability->SupportsDataFrameInput());
    rapidjson::Document doc;
    doc.Parse(json);
    auto& allocator = doc.GetAllocator();
    doc["source"].SetString("dataframe.samples", allocator);
    doc["datasets"][0].RemoveMember("table");
    doc["datasets"][0]["scope"]["consistency"].SetString("dataframe_snapshot", allocator);
    doc.AddMember("read_policy", rapidjson::Value(rapidjson::kObjectType).AddMember("page_rows", 1, allocator),
                  allocator);
    doc.AddMember("persistence",
                  rapidjson::Value(rapidjson::kObjectType).AddMember("checkpoint_every_buckets", 1, allocator),
                  allocator);
    rapidjson::StringBuffer text;
    rapidjson::Writer<rapidjson::StringBuffer> writer(text);
    doc.Accept(writer);
    const std::string configuration(text.GetString(), text.GetSize());
    auto source = std::make_shared<DataFrameChannel>("dataframe", "samples");
    assert(source->Open() == 0);
    DataFrame data;
    data.SetSchema({{"id", DataType::UINT32, 0, ""}, {"t", DataType::INT32, 0, ""}, {"v", DataType::FLOAT, 0, ""}});
    for (int t = 39; t >= 0; --t) assert(data.AppendRow({uint32_t{1}, int32_t{t}, float(100 + t % 3)}) == 0);
    assert(source->Write(&data) == 0);
    auto original = data.ToArrow();
    BlockDataFrameInputBindingV1 binding;
    binding.source = source;
    binding.exact_source = "dataframe.samples";
    for (int invalid = 0; invalid < 6; ++invalid) {
        std::string rejected_config = configuration;
        if (invalid == 5) {
            rapidjson::Document budget;
            budget.Parse(configuration.c_str());
            budget["read_policy"].AddMember("max_pending_bytes", 1, budget.GetAllocator());
            rapidjson::StringBuffer text;
            rapidjson::Writer<rapidjson::StringBuffer> writer(text);
            budget.Accept(writer);
            rejected_config.assign(text.GetString(), text.GetSize());
        }
        auto task = Task(owner, With(rejected_config));
        assert(dynamic_cast<IBlockTransformInputSourceTaskV1*>(task.get())->BindInputSource("dataframe.samples") == 0);
        auto* input_task = dynamic_cast<IBlockTransformDataFrameInputTaskV1*>(task.get());
        assert(input_task);
        BlockDataFrameInputV1 input;
        auto bind = binding;
        if (invalid == 0) bind.contract_version = 99;
        if (invalid == 1) input.struct_size = 1;
        if (invalid == 2) task->Cancel();
        if (invalid == 3) bind.exact_source = "dataframe.other";
        if (invalid == 4) bind.source.reset();
        input.source_fingerprint = "stale";
        input.schema = original->schema();
        assert(input_task->CreateDataFrameInput(bind, &input) != 0 && !input.input && !input.schema &&
               input.source_fingerprint.empty());
    }
    database::DatabasePlugin database;
    const std::string database_options = "type=sqlite;name=t4;path=/tmp/baseliner-dataframe-operator-" +
                                         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                                         ".db";
    assert(database.Option(database_options.c_str()) == 0 && database.Load(nullptr) == 0 && database.Start() == 0);
    auto target = database.AcquireChannel("sqlite", "t4");
    BlockTransformManagedSinkBindingV1 managed;
    managed.sink_channel = target.get();
    managed.target = "sqlite.t4";
    managed.category = "sqlite";
    managed.name = "t4";
    auto create = [&]() {
        auto task = Task(owner, With(configuration));
        assert(dynamic_cast<IBlockTransformInputSourceTaskV1*>(task.get())->BindInputSource("dataframe.samples") == 0);
        auto* sink = dynamic_cast<IBlockTransformManagedSinkTaskV1*>(task.get());
        assert(sink);
        const int rc = sink->BindManagedSink(managed);
        if (rc != 0) std::cerr << "DataFrame managed bind: " << task->LastError() << '\n';
        assert(rc == 0);
        return task;
    };
    auto saved = [&]() {
        ConfigSnapshot config;
        assert(ParseConfig(configuration, &config).ok());
        DurableStore store;
        assert(store.Open(config, target.get(), "inspect") == 0);
        StoredGeneration generation;
        assert(store.Load(&generation) == 0);
        store.Close();
        return generation;
    };
    std::string snapshot_fingerprint;
    {
        auto task = create();
        auto* input_task = dynamic_cast<IBlockTransformDataFrameInputTaskV1*>(task.get());
        BlockDataFrameInputV1 input;
        assert(input_task->CreateDataFrameInput(binding, &input) == 0);
        std::shared_ptr<arrow::Schema> output_schema;
        assert(task->Open(input.schema, &output_schema) == 0);
        auto event = input.input->PollBlock();
        std::vector<BlockTransformOutputV1> outputs;
        assert(event.kind == BlockPollEvent::kData && task->ProcessBlock(event.batch, 0, &outputs) == 0 &&
               outputs.empty());
        assert(input.input->ReleaseBlock(event.batch) == 0);
        snapshot_fingerprint = input.source_fingerprint;
        task->Cancel();
        input_task->ReleaseDataFrameInput(input.input);
        input.schema.reset();
        event.batch.reset();
        output_schema.reset();
    }
    // Simulate the cancelled writer lease expiring, as in the existing fenced recovery tests.
    assert(target->ExecuteSql("UPDATE baseline_tasks SET lease_deadline=0 WHERE task_key='operator'") >= 0);
    const auto partial = saved();
    assert(partial.generation > 0 && partial.progress.datasets.size() == 1 &&
           partial.progress.datasets[0].epoch == snapshot_fingerprint &&
           partial.progress.datasets[0].closed_before_bucket == std::numeric_limits<int64_t>::min());
    data.FromArrow(original->ReplaceSchemaMetadata(arrow::key_value_metadata({"test"}, {"changed"})));
    assert(source->Write(&data) == 0);
    {
        auto task = create();
        BlockDataFrameInputV1 input;
        assert(dynamic_cast<IBlockTransformDataFrameInputTaskV1*>(task.get())->CreateDataFrameInput(binding, &input) !=
               0);
        assert(std::string(task->LastError()).find("fingerprint mismatch") != std::string::npos && !input.input);
    }
    auto rejected = saved();
    assert(rejected.generation == partial.generation && rejected.checkpoint == partial.checkpoint &&
           rejected.progress.datasets[0].epoch == partial.progress.datasets[0].epoch);
    data.FromArrow(original);
    assert(source->Write(&data) == 0);
    for (bool replay : {false, true}) {
        auto task = create();
        auto* input_task = dynamic_cast<IBlockTransformDataFrameInputTaskV1*>(task.get());
        BlockDataFrameInputV1 input;
        assert(input_task->CreateDataFrameInput(binding, &input) == 0);
        BlockTransformPipelineConfig config;
        config.source = input.input;
        config.source_schema = input.schema;
        config.transform = task.get();
        config.time_transform = task.get();
        config.input_progress_task = dynamic_cast<IBlockTransformInputProgressTaskV1*>(task.get());
        config.output_consumer = [](const auto&) { return 0; };
        {
            BlockTransformPipelineRunner runner(config);
            BlockTransformPipelineResult result;
            std::string error;
            const auto rc = runner.Run(&result, &error);
            if (rc != BlockTransformPipelineError::kNone) std::cerr << error << '\n';
            assert(rc == BlockTransformPipelineError::kNone);
        }
        rapidjson::Document summary;
        summary.Parse(dynamic_cast<IBlockTransformManagedSinkTaskV1*>(task.get())->ManagedSinkResultJson().c_str());
        assert(replay ? summary["rows_written"].GetInt64() == 0 : summary["rows_written"].GetInt64() == 117);
        assert(summary["source_positions"].Size() == 1 &&
               summary["source_positions"][0]["closed_before_bucket"].GetInt64() == 40 &&
               std::string(summary["source_positions"][0]["committed_position"].GetString()) ==
                   "eof:" + input.source_fingerprint);
        config.source_schema.reset();
        input_task->ReleaseDataFrameInput(input.input);
        input.schema.reset();
    }
    binding.source.reset();
    source.reset();
    original.reset();
    data.Clear();
    target.reset();
    database.Stop();
    database.Unload();
    std::cout << "PASS DataFrame task contract, cancelled checkpoint fingerprint and managed replay\n";
}
void ResultCancellation() {
    Querier querier;
    auto owner = std::make_shared<Loaded>(querier);
    for (bool before_flush : {true, false}) {
        auto task = Task(owner);
        auto* results = dynamic_cast<IBlockTransformResultOutputTaskV1*>(task.get());
        BlockResultOutputBindingV1 binding;
        binding.target = "dataframe.results";
        assert(results && results->BindResultOutput(binding) == 0);
        assert(dynamic_cast<IBlockTransformInputSourceTaskV1*>(task.get())->BindInputSource("sqlite.t4") == 0);
        std::shared_ptr<arrow::Schema> schema;
        assert(task->Open(MakeSchema(SchemaKind::kObservation), &schema) == 0);
        std::vector<BlockTransformOutputV1> outputs;
        assert(task->ProcessBlock(Batch({Input(0)}), 0, &outputs) == 0);
        outputs.clear();
        if (before_flush) {
            task->Cancel();
            assert(task->Flush(&outputs) != 0);
        } else {
            assert(task->Flush(&outputs) == 0);
            task->Cancel();
            assert(results->CompleteResultOutputPublication(false) != 0);
        }
        rapidjson::Document summary;
        summary.Parse(dynamic_cast<IBlockTransformManagedSinkTaskV1*>(task.get())->ManagedSinkResultJson().c_str());
        assert(std::string(summary["results_output"]["status"].GetString()) == "cancelled" &&
               summary["results_output"]["rows_written"].GetInt64() == 0);
    }
}
void ModelOutputTaskPath() {
    Querier querier;
    auto owner = std::make_shared<Loaded>(querier);
    auto with = With(json);
    with.pop_back();
    with += R"(,"model_output":"dataframe.models"})";
    auto* normalizer = dynamic_cast<IBlockTransformSourceConfigProviderV1*>(owner->provider);
    auto* policy = dynamic_cast<IBlockTransformExecutionPolicyProviderV1*>(owner->provider);
    assert(normalizer && policy);
    std::string normalized, error;
    assert(normalizer->NormalizeSourceConfig(with.c_str(), "sqlite.t4", &normalized, &error) == 0);
    rapidjson::Document parsed;
    parsed.Parse(normalized.c_str());
    assert(std::string(parsed["model_output"].GetString()) == "dataframe.models");
    assert(std::string(parsed["parameters"].GetString()) == json);
    bool async = true;
    assert(policy->RequiresAsyncExecution(with.c_str(), "sqlite.t4", &async) == 0 && !async);
    for (int scenario = 0; scenario < 7; ++scenario) {
        auto task = Task(owner, with);
        auto* models = dynamic_cast<IBlockTransformModelOutputTaskV1*>(task.get());
        assert(models && models->ModelOutputTarget() == "dataframe.models");
        auto stage = std::make_shared<DataFrameChannel>("dataframe", "models");
        assert(stage->Open() == 0);
        BlockModelOutputBindingV1 binding;
        binding.primary_target = scenario == 0 ? "dataframe.models" : "dataframe.results";
        binding.dataframe = stage;
        if (scenario == 1) binding.contract_version = 99;
        if (scenario == 2) task->Cancel();
        if (scenario <= 2) {
            assert(models->BindModelOutput(binding) != 0);
            continue;
        }
        assert(models->BindModelOutput(binding) == 0);
        std::weak_ptr<DataFrameChannel> lifetime = stage;
        binding.dataframe.reset();
        stage.reset();
        assert(!lifetime.expired());  // The task owns the output lease through EOF/publication.
        assert(dynamic_cast<IBlockTransformInputSourceTaskV1*>(task.get())->BindInputSource("sqlite.t4") == 0);
        std::shared_ptr<arrow::Schema> schema;
        assert(task->Open(MakeSchema(SchemaKind::kObservation), &schema) == 0);
        std::vector<BlockTransformOutputV1> outputs;
        if (scenario != 5) {
            assert(task->ProcessBlock(Batch({Input(0), Input(1)}), 0, &outputs) == 0);
            outputs.clear();
        }
        if (scenario == 3) {
            task->Cancel();
            assert(task->Flush(&outputs) != 0);
            DataFrame frame;
            stage = lifetime.lock();
            assert(stage->Read(&frame) != 0 || frame.RowCount() == 0);
            rapidjson::Document summary;
            summary.Parse(dynamic_cast<IBlockTransformManagedSinkTaskV1*>(task.get())->ManagedSinkResultJson().c_str());
            assert(std::string(summary["model_output"]["status"].GetString()) == "cancelled");
            continue;
        }
        assert(task->Flush(&outputs) == 0 && outputs.empty());
        DataFrame frame;
        stage = lifetime.lock();
        assert(stage->Read(&frame) == 0);
        auto batch = frame.ToArrow();
        assert(batch && batch->schema()->Equals(*MakeSchema(SchemaKind::kModelParameters), true));
        assert(batch->num_rows() == (scenario == 5 ? 0 : 1));
        if (scenario == 6) task->Cancel();  // Registration succeeded before the cancellation raced with confirmation.
        assert(scenario == 6 ? models->CompleteModelOutputPublication(true) != 0
                             : models->CompleteModelOutputPublication(true) == 0);
        rapidjson::Document summary;
        summary.Parse(dynamic_cast<IBlockTransformManagedSinkTaskV1*>(task.get())->ManagedSinkResultJson().c_str());
        assert(std::string(summary["model_output"]["status"].GetString()) == "committed");
        task.reset();
        stage.reset();
        assert(lifetime.expired());
    }
    auto plain = Task(owner);
    assert(dynamic_cast<IBlockTransformModelOutputTaskV1*>(plain.get())->ModelOutputTarget().empty());
}
}  // namespace
int main() {
    ResultCancellation();
    ModelOutputTaskPath();
    ArrowPath();
    RelationArrow();
    PollPolicyAndProgress();
    DatabasePath();
    DataFrameTaskPath();
    std::cout << "PASS dynamic baseliner V2, exact config, Arrow partition parity and database pipeline\n";
}
