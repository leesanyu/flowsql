// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "task_registry.h"

#include <common/error_code.h>

#include "baseline_task_base.h"

namespace flowsql {
namespace baseline {

TaskRegistry::TaskRegistry() = default;
TaskRegistry::~TaskRegistry() = default;

std::string TaskRegistry::AllocateTaskId(BaselineTaskKind kind) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++next_seq_;
    return std::string(KindPrefix(kind)) + "-" + std::to_string(next_seq_);
}

int TaskRegistry::Register(const std::shared_ptr<BaselineTaskBase>& task) {
    if (!task) return error::BAD_REQUEST;

    std::lock_guard<std::mutex> lock(mutex_);
    auto [it, inserted] = tasks_.emplace(task->TaskId(), task);
    return inserted ? error::OK : error::CONFLICT;
}

void TaskRegistry::Unregister(const std::string& task_id,
                              const BaselineTaskBase* expected) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tasks_.find(task_id);
    if (it == tasks_.end()) return;
    if (expected && it->second.get() != expected) return;
    tasks_.erase(it);
}

std::vector<std::shared_ptr<BaselineTaskBase>> TaskRegistry::Snapshot() const {
    std::vector<std::shared_ptr<BaselineTaskBase>> snapshot;
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot.reserve(tasks_.size());
    for (const auto& [_, task] : tasks_) {
        snapshot.push_back(task);
    }
    return snapshot;
}

void TaskRegistry::List(std::function<void(const char* task_id,
                                           const char* task_name,
                                           BaselineTaskKind kind)> cb) const {
    if (!cb) return;

    const auto snapshot = Snapshot();
    for (const auto& task : snapshot) {
        cb(task->Id(), task->Name(), task->Kind());
    }
}

size_t TaskRegistry::Size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tasks_.size();
}

bool TaskRegistry::Owns(const BaselineTaskBase* task) const {
    if (!task) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = tasks_.find(task->TaskId());
    return it != tasks_.end() && it->second.get() == task;
}

const char* TaskRegistry::KindPrefix(BaselineTaskKind kind) {
    switch (kind) {
        case BaselineTaskKind::kValue:
            return "baseline-value";
        case BaselineTaskKind::kRatio:
            return "baseline-ratio";
        case BaselineTaskKind::kRelation:
            return "baseline-relation";
    }
    return "baseline-task";
}

}  // namespace baseline
}  // namespace flowsql
