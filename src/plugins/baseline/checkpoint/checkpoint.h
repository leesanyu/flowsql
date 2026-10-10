// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <framework/interfaces/ibaseline_checkpoint.h>
#include "plugins/baseline/task/baseline_task_base.h"
#include "plugins/baseline/task/relation_task.h"
namespace flowsql::baseline {
class CheckpointAccess {
 public:
    using RelationShard = BaselineRelationTask::RelationRoutedRuntimeShard;
    static BaselineStatus Bind(BaselineTaskBase& task);
    static BaselineStatus ModelParameters(const BaselineTaskBase& task, std::string_view key, uint64_t max_bytes,
                                          BaselineModelParametersV1* output);
    static BaselineSerializationResult Export(const BaselineTaskBase& task, uint64_t max_bytes);
    static BaselineStatus Restore(BaselineTaskBase& task, const BaselineCheckpointRestoreV1& restore,
                                  uint64_t max_bytes);

 private:
    template <class T>
    static BaselineModelParametersV1 ScalarParameters(const T& task, const std::string& key, uint64_t max_bytes);
    static BaselineModelParametersV1 RelationParameters(const BaselineRelationTask& task, const std::string& key,
                                                        uint64_t max_bytes);
    static BaselineSerializationResult ExportRelation(const BaselineRelationTask& task, uint64_t max_bytes);
    static BaselineStatus RestoreRelation(BaselineRelationTask& task, const BaselineCheckpointRestoreV1& restore,
                                          uint64_t max_bytes);
    static void ValidateRelation(BaselineRelationTask& prepared, uint64_t max_bytes);
    template <class T>
    static BaselineSerializationResult ExportScalar(const T& task, uint64_t max_bytes);
    template <class T>
    static BaselineStatus RestoreScalar(T& task, const BaselineCheckpointRestoreV1& restore, uint64_t max_bytes);
};
class BaselineCheckpoint final : public IBaselineCheckpointV1 {
 public:
    BaselineCheckpoint(std::shared_ptr<BaselineTaskBase> task, std::shared_ptr<IBaselineTaskStateControlV1> control,
                       uint64_t max_bytes)
        : task_(std::move(task)), control_(std::move(control)), max_bytes_(max_bytes) {}
    BaselineSerializationResult ExportCheckpoint(BaselineSerializationFormat format) const override {
        if (format != BaselineSerializationFormat::kJson) return {BaselineStatus::kUnsupportedFormat, {}};
        return CheckpointAccess::Export(*task_, max_bytes_);
    }
    BaselineStatus RestoreCheckpoint(const BaselineCheckpointRestoreV1& restore) override {
        return CheckpointAccess::Restore(*task_, restore, max_bytes_);
    }

 private:
    std::shared_ptr<BaselineTaskBase> task_;
    std::shared_ptr<IBaselineTaskStateControlV1> control_;
    uint64_t max_bytes_;
};
}  // namespace flowsql::baseline
