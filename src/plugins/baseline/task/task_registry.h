// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_PLUGINS_BASELINE_TASK_TASK_REGISTRY_H_
#define _FLOWSQL_PLUGINS_BASELINE_TASK_TASK_REGISTRY_H_

#include <framework/interfaces/ibaseline_types.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace flowsql {
namespace baseline {

class BaselineTaskBase;

class TaskRegistry {
 public:
    TaskRegistry();
    ~TaskRegistry();

    std::string AllocateTaskId(BaselineTaskKind kind);
    int Register(const std::shared_ptr<BaselineTaskBase>& task);
    void Unregister(const std::string& task_id, const BaselineTaskBase* expected);

    std::vector<std::shared_ptr<BaselineTaskBase>> Snapshot() const;
    void List(std::function<void(const char* task_id,
                                 const char* task_name,
                                 BaselineTaskKind kind)> cb) const;
    size_t Size() const;
    bool Owns(const BaselineTaskBase* task) const;

 private:
    static const char* KindPrefix(BaselineTaskKind kind);

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<BaselineTaskBase>> tasks_;
    uint64_t next_seq_ = 0;
};

}  // namespace baseline
}  // namespace flowsql

#endif  // _FLOWSQL_PLUGINS_BASELINE_TASK_TASK_REGISTRY_H_
