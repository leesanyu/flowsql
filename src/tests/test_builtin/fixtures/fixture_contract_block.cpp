// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <arrow/api.h>
#include <framework/interfaces/cpp_operator_plugin_abi.h>
#include <framework/interfaces/iblock_transform_operator.h>
#include <common/channel_dataset.hpp>
#include <fstream>
using namespace flowsql;
namespace {
class Task final : public IBlockTransformTaskV1 {
 public:
    std::string marker, drift;
    std::shared_ptr<arrow::RecordBatch> Wrong() {
        arrow::StringBuilder builder;
        auto values = builder.Finish().ValueOrDie();
        return arrow::RecordBatch::Make(arrow::schema({arrow::field("x", arrow::utf8())}), 0, {values});
    }
    int Open(std::shared_ptr<arrow::Schema> input, std::shared_ptr<arrow::Schema>* output) override {
        if (!marker.empty()) std::ofstream(marker, std::ios::app) << "open\n";
        *output = input;
        return 0;
    }
    int ProcessBlock(const std::shared_ptr<arrow::RecordBatch>& input, int64_t time,
                     std::vector<BlockTransformOutputV1>* outputs) override {
        outputs->push_back({drift == "process" ? Wrong() : input, time});
        return 0;
    }
    int Flush(std::vector<BlockTransformOutputV1>* outputs) override {
        if (drift == "flush") outputs->push_back({Wrong(), 0});
        return 0;
    }
    void Cancel() override {}
    std::string LastError() const override { return {}; }
};
class Provider final : public IBlockTransformOperatorV1 {
 public:
    std::string Category() const override { return "contractdemo"; }
    std::string Name() const override { return "block"; }
    std::string Description() const override { return "Actual block contract fixture"; }
    int CreateTask(const BlockTransformTaskConfigV1& config, IBlockTransformTaskV1** out) override {
        auto* task = new Task;
        dataset::Json params;
        params.Parse(config.with_params_json);
        task->marker = dataset::Get(params, "marker");
        task->drift = dataset::Get(params, "drift");
        *out = task;
        return 0;
    }
    void ReleaseTask(IBlockTransformTaskV1* task) override { delete task; }
};
}  // namespace
extern "C" int flowsql_abi_version() { return 2; }
extern "C" int flowsql_operator_count() { return 1; }
extern "C" int flowsql_describe_operator(int index, CppOperatorDescriptorV2* d) {
    if (index || !d || d->struct_size != kCppOperatorDescriptorV2Size) return -1;
    *d = {kCppOperatorDescriptorV2Size, "contractdemo", "block", "Actual block contract fixture",
          IID_BLOCK_TRANSFORM_OPERATOR_V1};
    return 0;
}
extern "C" void* flowsql_create_operator_capability(int index, IQuerier*) {
    return index ? nullptr : static_cast<IBlockTransformOperatorV1*>(new Provider);
}
extern "C" void flowsql_destroy_operator_capability(int, void* value) {
    delete static_cast<IBlockTransformOperatorV1*>(value);
}
extern "C" const char* flowsql_operator_data_contract_v1(int index) {
    return index ? nullptr : R"({"schema_id":"flowsql.operator-data-contract.v1","title":"Actual block fixture",
      "operator":{"category":"contractdemo","name":"block","version":"1"},
      "configuration":{"invocation_schema":{"type":"object","additionalProperties":true},"affects":{}},
      "inputs":[{"port_id":"input","min_count":1,"max_count":1,"shape":"record_batch","roles":[
        {"role_id":"measure","required":true,"types":[{"system":"arrow","name":"int64","parameters":{}}],"nullable":null,"semantic_requirements":{},"selector":{"kind":"parameter","key":"column"}}]}],
      "outputs":[{"port_id":"output","shape":"record_batch","mode":"derived","rules":[{"kind":"copy","source_port":"input","all_fields":true}]}],"limitations":[]})";
}
