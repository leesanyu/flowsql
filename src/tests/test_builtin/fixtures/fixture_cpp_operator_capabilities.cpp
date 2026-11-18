// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <framework/interfaces/cpp_operator_plugin_abi.h>
#include <framework/interfaces/iblock_transform_operator.h>
#include <framework/interfaces/icpp_operator_plugin_registry.h>
#include <framework/interfaces/ioperator.h>

#include <cstdlib>
#include <string>
#include <utility>

namespace {

bool CaseDuplicateEnabled() { return std::getenv("FLOWSQL_FIXTURE_CPP_OPERATOR_CASE_DUPLICATE") != nullptr; }
bool DuplicateTransformEnabled() { return std::getenv("FLOWSQL_FIXTURE_CPP_OPERATOR_DUPLICATE_TRANSFORM") != nullptr; }

class FixtureEchoOperator final : public flowsql::IOperator {
 public:
    explicit FixtureEchoOperator(flowsql::IQuerier* querier) : querier_(querier) {}

    std::string Category() override { return "fixture"; }
    std::string Name() override { return "echo"; }
    std::string Description() override { return "Current fixture classic operator"; }
    flowsql::OperatorPosition Position() override { return flowsql::OperatorPosition::DATA; }
    int Work(flowsql::IChannel*, flowsql::IChannel*) override { return querier_ ? 0 : -1; }
    int Configure(const char*, const char*) override { return 0; }

 private:
    flowsql::IQuerier* querier_ = nullptr;
};

class FixtureTransformTask final : public flowsql::IBlockTransformTask {
 public:
    int Open(std::shared_ptr<arrow::Schema> input_schema, std::shared_ptr<arrow::Schema>* output_schema) override {
        if (!output_schema) return -1;
        *output_schema = std::move(input_schema);
        return 0;
    }

    int ProcessBlock(const std::shared_ptr<arrow::RecordBatch>&, int64_t,
                     std::vector<flowsql::BlockTransformOutput>* outputs) override {
        if (!outputs || !outputs->empty()) return -1;
        return static_cast<int>(flowsql::BlockTransformStatus::kContinue);
    }

    int Flush(std::vector<flowsql::BlockTransformOutput>* outputs) override {
        return outputs && outputs->empty() ? 0 : -1;
    }

    void Cancel() override {}
    std::string LastError() const override { return ""; }
};

class FixtureTransformOperator final : public flowsql::IBlockTransformOperator {
 public:
    explicit FixtureTransformOperator(flowsql::IQuerier* querier) : querier_(querier) {}

    ~FixtureTransformOperator() override {
        auto* registry = querier_ ? static_cast<flowsql::ICppOperatorPluginRegistry*>(
                                        querier_->First(flowsql::IID_CPP_OPERATOR_PLUGIN_REGISTRY))
                                  : nullptr;
        flowsql::CppOperatorCapabilityLease lease;
        if (registry) {
            (void)registry->Acquire("fixture", "transform", flowsql::IID_BLOCK_TRANSFORM_OPERATOR, &lease);
        }
    }

    std::string Category() const override { return CaseDuplicateEnabled() ? "Fixture" : "fixture"; }
    std::string Name() const override { return CaseDuplicateEnabled() ? "echo" : "transform"; }
    std::string Description() const override { return "Current fixture block transform operator"; }

    int CreateTask(const flowsql::BlockTransformTaskConfig&, flowsql::IBlockTransformTask** task) override {
        if (!task) return -1;
        *task = nullptr;
        if (!querier_) return -1;
        *task = new FixtureTransformTask();
        return 0;
    }

    void ReleaseTask(flowsql::IBlockTransformTask* task) override { delete task; }

 private:
    flowsql::IQuerier* querier_ = nullptr;
};

class FixtureTimeDrivenTransformTask final : public flowsql::IBlockTransformTask,
                                             public flowsql::IBlockTransformTimeDrivenTask {
 public:
    explicit FixtureTimeDrivenTransformTask(const flowsql::BlockTransformTaskConfig& config)
        : task_id_(config.task_id),
          with_params_json_(config.with_params_json),
          pushed_filter_plan_json_(config.pushed_filter_plan_json) {}

    int Open(std::shared_ptr<arrow::Schema> input_schema, std::shared_ptr<arrow::Schema>* output_schema) override {
        if (!input_schema || !output_schema || opened_) return -1;
        schema_ = std::move(input_schema);
        *output_schema = schema_;
        opened_ = true;
        return 0;
    }

    int ProcessBlock(const std::shared_ptr<arrow::RecordBatch>&, int64_t,
                     std::vector<flowsql::BlockTransformOutput>* outputs) override {
        if (!opened_ || cancelled_ || !outputs || !outputs->empty()) return -1;
        return static_cast<int>(flowsql::BlockTransformStatus::kContinue);
    }

    int Flush(std::vector<flowsql::BlockTransformOutput>* outputs) override {
        return opened_ && !cancelled_ && outputs && outputs->empty() ? 0 : -1;
    }

    void Cancel() override { cancelled_ = true; }
    std::string LastError() const override { return ""; }

    int GetTimeDriveState(flowsql::BlockTransformTimeDriveState* state) override {
        if (!opened_ || cancelled_ || !state || state->struct_size < flowsql::kBlockTransformTimeDriveStateSize ||
            state->contract_version != flowsql::kBlockTransformTimeDriveVersion) {
            return -1;
        }
        flowsql::BlockTransformTimeDriveState next{};
        next.struct_size = flowsql::kBlockTransformTimeDriveStateSize;
        next.contract_version = flowsql::kBlockTransformTimeDriveVersion;
        next.armed = 1;
        next.deadline_ns = deadline_ns_;
        *state = next;
        return 0;
    }

    int OnTime(const flowsql::BlockTransformTimeEvent& event,
               std::vector<flowsql::BlockTransformOutput>* outputs) override {
        if (!opened_ || cancelled_ || !outputs || !outputs->empty() ||
            event.struct_size < flowsql::kBlockTransformTimeEventSize ||
            event.contract_version != flowsql::kBlockTransformTimeDriveVersion || event.monotonic_now_ns < 0) {
            return -1;
        }
        deadline_ns_ = event.monotonic_now_ns + 1;
        return static_cast<int>(flowsql::BlockTransformStatus::kContinue);
    }

 private:
    std::string task_id_;
    std::string with_params_json_;
    std::string pushed_filter_plan_json_;
    bool opened_ = false;
    bool cancelled_ = false;
    int64_t deadline_ns_ = 0;
    std::shared_ptr<arrow::Schema> schema_;
};

class FixtureTimeDrivenTransformOperator final : public flowsql::IBlockTransformOperator {
 public:
    explicit FixtureTimeDrivenTransformOperator(flowsql::IQuerier* querier) : querier_(querier) {}

    std::string Category() const override { return "fixture"; }
    std::string Name() const override { return DuplicateTransformEnabled() ? "transform" : "time_transform"; }
    std::string Description() const override { return "Current fixture time-driven block transform operator"; }

    int CreateTask(const flowsql::BlockTransformTaskConfig& config, flowsql::IBlockTransformTask** task) override {
        if (!task) return -1;
        *task = nullptr;
        if (!querier_ || config.struct_size < flowsql::kBlockTransformTaskConfigSize ||
            config.contract_version != flowsql::kBlockTransformContractVersion || !config.task_id ||
            !config.with_params_json || !config.pushed_filter_plan_json) {
            return -1;
        }
        try {
            *task = new FixtureTimeDrivenTransformTask(config);
        } catch (...) {
            *task = nullptr;
            return -1;
        }
        return 0;
    }

    void ReleaseTask(flowsql::IBlockTransformTask* task) override { delete task; }

 private:
    flowsql::IQuerier* querier_ = nullptr;
};

}  // namespace

extern "C" {

int flowsql_abi_version() { return flowsql::kCppOperatorPluginAbiVersion; }

int flowsql_operator_count() { return 3; }

int flowsql_describe_operator(int index, flowsql::CppOperatorDescriptor* descriptor) {
    if (!descriptor || descriptor->struct_size < flowsql::kCppOperatorDescriptorSize) return -1;

    switch (index) {
        case 0:
            descriptor->category = "fixture";
            descriptor->name = "echo";
            descriptor->description = "Current fixture classic operator";
            descriptor->contract_iid = flowsql::IID_OPERATOR;
            return 0;
        case 1:
            descriptor->category = CaseDuplicateEnabled() ? "Fixture" : "fixture";
            descriptor->name = CaseDuplicateEnabled() ? "echo" : "transform";
            descriptor->description = "Current fixture block transform operator";
            descriptor->contract_iid = flowsql::IID_BLOCK_TRANSFORM_OPERATOR;
            return 0;
        case 2:
            descriptor->category = "fixture";
            descriptor->name = DuplicateTransformEnabled() ? "transform" : "time_transform";
            descriptor->description = "Current fixture time-driven block transform operator";
            descriptor->contract_iid = flowsql::IID_BLOCK_TRANSFORM_OPERATOR;
            return 0;
        default:
            return -1;
    }
}

void* flowsql_create_operator_capability(int index, flowsql::IQuerier* querier) {
    if (!querier) return nullptr;
    try {
        switch (index) {
            case 0:
                return static_cast<flowsql::IOperator*>(new FixtureEchoOperator(querier));
            case 1:
                return static_cast<flowsql::IBlockTransformOperator*>(new FixtureTransformOperator(querier));
            case 2:
                return static_cast<flowsql::IBlockTransformOperator*>(new FixtureTimeDrivenTransformOperator(querier));
            default:
                return nullptr;
        }
    } catch (...) {
        return nullptr;
    }
}

void flowsql_destroy_operator_capability(int index, void* capability) {
    if (!capability) return;
    switch (index) {
        case 0:
            delete static_cast<flowsql::IOperator*>(capability);
            break;
        case 1:
            delete static_cast<flowsql::IBlockTransformOperator*>(capability);
            break;
        case 2:
            delete static_cast<flowsql::IBlockTransformOperator*>(capability);
            break;
        default:
            break;
    }
}

}  // extern "C"
