// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <arrow/api.h>
#include <arrow/io/api.h>
#include <arrow/ipc/api.h>
#include <dlfcn.h>
#include <framework/core/dataframe_channel.h>
#include <framework/interfaces/cpp_operator_plugin_abi.h>
#include <framework/interfaces/iblock_stream_channel.h>
#include <framework/interfaces/iblock_transform_model_output.h>
#include <framework/interfaces/iblock_transform_operator.h>
#include <framework/interfaces/iblock_transform_result_output.h>
#include <framework/interfaces/iblock_transform_source_config.h>
#include <operators/baseliner/result_batch.h>
#include <operators/baseliner/result_codec.h>
#include <plugins/baseline/baseline_plugin.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <chrono>
#include <cstring>
#include <functional>
#include <stdexcept>

namespace flowsql::baseliner::batch_test {
using Batch = std::shared_ptr<arrow::RecordBatch>;
using Clock = std::chrono::steady_clock;
inline double Seconds(Clock::time_point start) { return std::chrono::duration<double>(Clock::now() - start).count(); }
inline void Check(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}
template <typename T>
T Unwrap(arrow::Result<T> value) {
    Check(value.ok(), value.status().ToString());
    return std::move(value).ValueOrDie();
}
inline std::string Json(const rapidjson::Value& value) {
    rapidjson::StringBuffer text;
    rapidjson::Writer<rapidjson::StringBuffer> writer(text);
    value.Accept(writer);
    return {text.GetString(), text.GetSize()};
}
inline std::string With(const std::string& parameters, bool model = true) {
    rapidjson::Document doc(rapidjson::kObjectType);
    doc.AddMember("parameters", rapidjson::Value(parameters.c_str(), doc.GetAllocator()), doc.GetAllocator());
    if (model) doc.AddMember("model_output", "dataframe.models", doc.GetAllocator());
    return Json(doc);
}
struct Querier : IQuerier {
    baseline::BaselinePlugin plugin;
    IBaselineService* service = nullptr;
    IBaselineStateControlServiceV1* control = nullptr;
    Querier() {
        Check(plugin.Option(nullptr) == 0 && plugin.Load(this) == 0 && plugin.Start() == 0, "baseline lifecycle");
    }
    ~Querier() {
        plugin.Stop();
        plugin.Unload();
    }
    void* First(const Guid& id) override {
        if (std::memcmp(&id, &IID_BASELINE_SERVICE, sizeof(Guid)) == 0)
            return service ? service : static_cast<IBaselineService*>(&plugin);
        if (std::memcmp(&id, &IID_BASELINE_STATE_CONTROL_SERVICE_V1, sizeof(Guid)) == 0)
            return control ? control : static_cast<IBaselineStateControlServiceV1*>(&plugin);
        if (std::memcmp(&id, &IID_BASELINE_CHECKPOINT_SERVICE_V1, sizeof(Guid)) == 0)
            return static_cast<IBaselineCheckpointServiceV1*>(&plugin);
        return nullptr;
    }
    int Traverse(const Guid& id, fntraverse callback) override {
        auto* found = First(id);
        return found ? callback(found) : 0;
    }
};
struct Library {
    std::shared_ptr<Querier> querier = std::make_shared<Querier>();
    void* handle = nullptr;
    IBlockTransformOperatorV2* provider = nullptr;
    CppOperatorPluginDestroyCapabilityV2Fn destroy = nullptr;
    explicit Library(const char* path = "./libflowsql_baseliner.so") {
        handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        Check(handle != nullptr, std::string("load baseliner: ") + path);
        auto version =
            reinterpret_cast<CppOperatorPluginAbiVersionFn>(dlsym(handle, kCppOperatorPluginAbiVersionSymbol));
        auto create = reinterpret_cast<CppOperatorPluginCreateCapabilityV2Fn>(
            dlsym(handle, kCppOperatorPluginCreateCapabilityV2Symbol));
        destroy = reinterpret_cast<CppOperatorPluginDestroyCapabilityV2Fn>(
            dlsym(handle, kCppOperatorPluginDestroyCapabilityV2Symbol));
        Check(version && version() == 2 && create && destroy, "baseliner ABI");
        provider = static_cast<IBlockTransformOperatorV2*>(create(0, querier.get()));
        Check(provider != nullptr, "baseliner provider");
    }
    ~Library() {
        if (provider) destroy(0, provider);
        if (handle) dlclose(handle);
    }
};
using TaskOwner = std::unique_ptr<IBlockTransformTaskV2, std::function<void(IBlockTransformTaskV2*)>>;
inline TaskOwner Task(const std::shared_ptr<Library>& library, const ConfigSnapshot& config, bool model = true) {
    auto with = With(config.original_json, model);
    BlockTransformTaskConfigV2 c{kBlockTransformTaskConfigV2Size, kBlockTransformContractVersionV2, "batch-test",
                                 with.c_str(), ""};
    IBlockTransformTaskV2* task = nullptr;
    Check(library->provider->CreateTask(c, &task) == 0 && task, "create task");
    return TaskOwner(task, [library](auto* value) { library->provider->ReleaseTask(value); });
}
inline ConfigSnapshot Config(uint32_t horizon = 0, uint64_t budget = 268435456, std::string kind = "value") {
    std::string metric =
        R"({"id":"v","kind":"value","column":"v","aggregate":"mean","feature_type":"value_basic","profile":"default"})";
    if (kind == "ratio")
        metric =
            R"({"id":"v","kind":"ratio","numerator":{"column":"n","aggregate":"sum"},"denominator":{"column":"den","aggregate":"sum"},"feature_type":"ratio","profile":"rate_core"})";
    if (kind == "relation")
        metric =
            R"({"id":"v","kind":"relation","feature_type":"relation","profile":"default","group_space":{"id":"g","version":"1","column":"g","unknown":"reject"},"metrics":[{"id":"mass","column":"v","aggregate":"sum"}],"support_policy":{"k_support":3,"min_hist_share":0.01,"min_active_ratio":0.1},"summary_policy":{"k_head":2,"k_stable":1}})";
    const std::string json =
        R"({"task_key":"batch-test","clock":{"bucket_seconds":60,"timezone":"Asia/Shanghai"},"calendar":{"calendar_id":"cn-holiday","calendar_version":"2026.1"},"bootstrap":{"mode":"cold"},"forecast":{"horizon_buckets":)" +
        std::to_string(horizon) + R"(},"read_policy":{"page_rows":4096,"max_pending_bytes":)" + std::to_string(budget) +
        R"(},"dataset":{"fields":{"t":"int64","id":"uint64","g":"uint64","v":"float64","n":"float64","den":"float64"},"series_keys":["id"],"deduplicate":{"keys":["t","id","g"],"on_duplicate":"require_equal"},"bucket":{"column":"t","unit":"bucket_id"},"metrics":[)" +
        metric + "]}}";
    ConfigSnapshot config;
    const auto status = NormalizeInlineConfig(json, "dataframe.samples", &config);
    Check(status.ok(), status.message);
    return config;
}
inline Observation Input(const ConfigSnapshot& config, int64_t bucket, uint64_t series = 1, size_t text = 0) {
    const auto& dataset = config.config.datasets.front();
    const auto& metric = dataset.metrics.front();
    Observation row;
    row.dataset_id = dataset.id;
    row.metric_id = metric.id;
    row.kind = metric.kind;
    row.source_epoch = "batch-test-epoch" + std::string(text, 'x');
    row.bucket = bucket;
    Check(EncodeIdentity(config.config.source, dataset.id, metric.id, metric.kind, {series}, &row.identity).ok(),
          "identity");
    if (metric.kind == BaselineTaskKind::kValue) {
        row.value = 100 + bucket % 3;
        row.sample_count = 1;
    } else if (metric.kind == BaselineTaskKind::kRatio) {
        row.numerator = 100 + bucket % 3;
        row.denominator = 1000;
    } else {
        row.groups = {1, 2, 3};
        row.metrics = {{"mass", 1000, 3, {500, 300, 200}}};
    }
    return row;
}
inline Batch Wire(const std::vector<Observation>& rows) {
    Batch batch;
    std::string error;
    Check(MakeObservationBatch(rows, 268435456, &batch, &error) == 0, error);
    return batch;
}
inline void Equal(const Batch& actual, const Batch& expected) {
    Check(actual && expected && actual->ValidateFull().ok(), "valid result");
    Check(actual->schema()->Equals(expected->schema(), true), "Schema equality");
    Check(actual->num_rows() == expected->num_rows(), "row equality");
    for (int i = 0; i < actual->num_columns(); ++i)
        Check(actual->column(i)->Equals(expected->column(i)), "column equality: " + actual->column_name(i));
}
inline Batch Merge(const std::vector<Batch>& batches, SchemaKind empty_kind = SchemaKind::kResults) {
    if (batches.empty()) {
        std::vector<std::shared_ptr<arrow::Array>> arrays;
        auto schema = MakeSchema(empty_kind);
        for (const auto& field : schema->fields()) arrays.push_back(Unwrap(arrow::MakeArrayOfNull(field->type(), 0)));
        return arrow::RecordBatch::Make(schema, 0, std::move(arrays));
    }
    return Unwrap(arrow::ConcatenateRecordBatches(batches));
}
inline void Write(const std::string& path, const Batch& batch) {
    auto file = Unwrap(arrow::io::FileOutputStream::Open(path));
    auto writer = Unwrap(arrow::ipc::MakeFileWriter(file.get(), batch->schema()));
    Check(writer->WriteRecordBatch(*batch).ok() && writer->Close().ok() && file->Close().ok(), "write reference");
}
inline Batch Read(const std::string& path) {
    auto file = Unwrap(arrow::io::ReadableFile::Open(path));
    auto reader = Unwrap(arrow::ipc::RecordBatchFileReader::Open(file));
    return Unwrap(reader->ReadRecordBatch(0));
}
}  // namespace flowsql::baseliner::batch_test
