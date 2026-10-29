// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <framework/interfaces/iblock_transform_operator.h>
#include <cerrno>
#include "operator_contract_observation.hpp"

namespace flowsql::contracts {
struct BlockInvocation {
    IQuerier* querier = nullptr;
    Invocation invocation;
    dataset::Json parameters{rapidjson::kObjectType}, inputs{rapidjson::kArrayType}, context{rapidjson::kObjectType};
    DataSemanticsErrorV1 error;
    std::shared_ptr<BlockInvocation> previous;
    int Check(IBlockTransformTaskV1* task, IOperatorConfigAdapterV1* fallback,
              const std::shared_ptr<arrow::Schema>& schema) {
        using namespace dataset;
        error = {};
        invocation.check.SetObject();
        Json actual;
        actual.CopyFrom(inputs, actual.GetAllocator());
        Json execution;
        execution.CopyFrom(context, execution.GetAllocator());
        if (invocation.component.contract_state == "declared" && actual.Empty()) {
            auto* service = querier
                                ? static_cast<IOperatorDataContractV1*>(querier->First(IID_OPERATOR_DATA_CONTRACT_V1))
                                : nullptr;
            OperatorContractSnapshotV1 definition;
            if (!service) return Fail(&error, "UNAVAILABLE", "declared operator contract service unavailable");
            if (service->Resolve(invocation.component.contract_ref.c_str(), &definition, &error)) return -1;
            Json document;
            document.Parse(definition.document_json->c_str());
            if (!document["inputs"].Empty()) {
                Json snapshot;
                std::string binding;
                if (previous && previous->invocation.check.HasMember("outputs") &&
                    !previous->invocation.check["outputs"].Empty() &&
                    previous->invocation.check["outputs"][0].HasMember("binding_ref")) {
                    const auto& output = previous->invocation.check["outputs"][0];
                    snapshot.CopyFrom(output["snapshot"], snapshot.GetAllocator());
                    binding = Get(output, "binding_ref");
                } else if (DescribeInput(nullptr, Value(), schema, &snapshot, &binding, &error))
                    return -1;
                Value input(rapidjson::kObjectType);
                auto& a = actual.GetAllocator();
                Set(input, "port_id", Get(document["inputs"][0], "port_id"), a);
                input.AddMember("input_index", 0, a).AddMember("snapshot", Copy(snapshot, a), a);
                if (!binding.empty()) Set(input, "binding_ref", binding, a);
                actual.PushBack(input, a);
                Value shapes(rapidjson::kArrayType);
                shapes.PushBack("record_batch", execution.GetAllocator());
                Set(execution, "input_shapes", std::move(shapes), execution.GetAllocator());
            }
        }
        auto* adapter = dynamic_cast<IOperatorConfigAdapterV1*>(task);
        Json previous;
        if (invocation.resolved.IsObject() && invocation.resolved.HasMember("inputs"))
            previous.CopyFrom(invocation.resolved["inputs"], previous.GetAllocator());
        const int rc =
            CheckInvocation(querier, &invocation, adapter ? adapter : fallback, parameters, actual, execution, &error);
        if (!rc && previous.IsArray() && invocation.resolved.HasMember("inputs")) {
            const auto& current = invocation.resolved["inputs"];
            if (previous.Size() != current.Size())
                return Fail(&error, "SOURCE_CHANGED", "block input collection changed");
            for (rapidjson::SizeType i = 0; i < previous.Size(); ++i) {
                const auto& old_snapshot = previous[i]["snapshot"];
                const auto& new_snapshot = current[i]["snapshot"];
                // Anonymous block-stage snapshots are recreated from the same
                // schema. Named source identities must remain frozen.
                if (Get(old_snapshot["locator"], "name") == "temporary") continue;
                if (old_snapshot["object_key"] != new_snapshot["object_key"] ||
                    old_snapshot["source_revision"] != new_snapshot["source_revision"])
                    return Fail(&error, "SOURCE_CHANGED", "block input source changed after checking");
            }
        }
        if (rc && !invocation.check.HasMember("state")) {
            auto& a = invocation.check.GetAllocator();
            Set(invocation.check, "state", error.code == "CONTRACT_INCOMPATIBLE" ? "incompatible" : "undetermined", a);
            Set(invocation.check, "data_validation", "not_run", a);
            Set(invocation.check, "output_check", "not_observed", a);
            Value reason(rapidjson::kObjectType), reasons(rapidjson::kArrayType);
            Set(reason, "code", error.code, a);
            Set(reason, "pointer", error.field_path, a);
            Set(reason, "message", error.message, a);
            reasons.PushBack(reason, a);
            Set(invocation.check, "reasons", std::move(reasons), a);
        }
        return rc;
    }
    std::string ErrorText() const { return error.code + ": " + error.message; }
};

// Keeps the unchanged execution ABI. Checks the actual stage input immediately
// before Open; ProcessBlock/time/flush continue on that same exclusive task.
class CheckedBlockTask final : public IBlockTransformTaskV1, public IBlockTransformTimeDrivenTaskV1 {
 public:
    CheckedBlockTask(IBlockTransformTaskV1* task, IOperatorConfigAdapterV1* fallback,
                     std::shared_ptr<BlockInvocation> state)
        : task_(task), fallback_(fallback), state_(std::move(state)) {}
    int Open(std::shared_ptr<arrow::Schema> input, std::shared_ptr<arrow::Schema>* output) override {
        if (state_->Check(task_, fallback_, input)) {
            if (output) output->reset();
            return EINVAL;
        }
        if (auto* publisher = dynamic_cast<IOperatorOutputPublicationV1*>(task_)) {
            publisher->SetOutputPublicationObserver([state = state_, task = task_] {
                dataset::Json actual;
                if (ProducerOutputs(dynamic_cast<IOperatorOutputObservationV1*>(task), &actual, &state->error))
                    return -1;
                return Observe(state->querier, &state->invocation, actual, "partial", &state->error);
            });
        }
        const auto rc = task_->Open(std::move(input), output);
        if (rc) return rc;
        output_schema_ = *output;
        if (ObservePrimary(state_->querier, &state_->invocation, output_schema_, -1, nullptr, "partial",
                           &state_->error))
            return EINVAL;
        return 0;
    }
    int ProcessBlock(const std::shared_ptr<arrow::RecordBatch>& input, int64_t time,
                     std::vector<BlockTransformOutputV1>* output) override {
        const auto rc = task_->ProcessBlock(input, time, output);
        if (rc < 0) return rc;
        return Validate(output) ? EINVAL : rc;
    }
    int Flush(std::vector<BlockTransformOutputV1>* output) override {
        const auto rc = task_->Flush(output);
        if (rc) return rc;
        if (Validate(output)) return EINVAL;
        return ObservePrimary(state_->querier, &state_->invocation, output_schema_, rows_,
                              dynamic_cast<IOperatorOutputObservationV1*>(task_), "complete", &state_->error)
                   ? EINVAL
                   : 0;
    }
    int GetTimeDriveState(BlockTransformTimeDriveStateV1* state) override {
        auto* time = dynamic_cast<IBlockTransformTimeDrivenTaskV1*>(task_);
        return time ? time->GetTimeDriveState(state) : ENOTSUP;
    }
    int OnTime(const BlockTransformTimeEventV1& event, std::vector<BlockTransformOutputV1>* output) override {
        auto* time = dynamic_cast<IBlockTransformTimeDrivenTaskV1*>(task_);
        if (!time) return ENOTSUP;
        const auto rc = time->OnTime(event, output);
        return rc < 0 ? rc : Validate(output) ? EINVAL : rc;
    }
    void Cancel() override { task_->Cancel(); }
    std::string LastError() const override {
        return state_->error.code.empty() ? task_->LastError() : state_->ErrorText();
    }

 private:
    int Validate(std::vector<BlockTransformOutputV1>* output) {
        if (state_->invocation.component.contract_state != "declared") return 0;
        for (const auto& item : *output) {
            if (!item.batch || !output_schema_ || !item.batch->schema()->Equals(*output_schema_)) {
                dataset::Json unobserved(rapidjson::kArrayType);
                DataSemanticsErrorV1 ignored;
                (void)Observe(state_->querier, &state_->invocation, unobserved, "failed", &ignored);
                output->clear();
                return dataset::Fail(&state_->error, "OUTPUT_SCHEMA_MISMATCH", "block output schema changed");
            }
            rows_ += item.batch->num_rows();
        }
        return 0;
    }
    std::shared_ptr<arrow::Schema> output_schema_;
    int64_t rows_ = 0;
    IBlockTransformTaskV1* task_;
    IOperatorConfigAdapterV1* fallback_;
    std::shared_ptr<BlockInvocation> state_;
};
}  // namespace flowsql::contracts
