// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_SERVICES_SCHEDULER_SCHEDULER_BATCH_RUNTIME_H_
#define _FLOWSQL_SERVICES_SCHEDULER_SCHEDULER_BATCH_RUNTIME_H_

#include <common/error_code.h>
#include <framework/core/pipeline.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace flowsql {
namespace scheduler {

enum class BatchRuntimeStatus {
    kPending,
    kRunning,
    kStopping,
    kStopped,
    kCancelled,
    kCompleted,
    kFailed,
    kTimeout,
};

struct BatchRuntimeSnapshot {
    std::string runtime_task_id;
    BatchRuntimeStatus status = BatchRuntimeStatus::kPending;
    std::string error_code;
    std::string error_message;
    std::string error_stage;
    int current_sql_index = 0;
    int sql_count = 0;
    int timeout_s = 0;
    int64_t result_row_count = 0;
    int64_t result_col_count = 0;
    std::string result_target;
    std::string managed_result_json;
    int64_t created_ms = 0;
    int64_t started_ms = 0;
    int64_t last_active_ms = 0;
    int64_t finished_ms = 0;
};

const char* BatchRuntimeStatusName(BatchRuntimeStatus status);
bool IsTerminalBatchRuntimeStatus(BatchRuntimeStatus status);

class SchedulerBatchRuntime final {
 public:
    using ExecuteSqlFn = std::function<int32_t(const std::string& sql, std::string* rsp)>;

    SchedulerBatchRuntime() = default;
    ~SchedulerBatchRuntime();

    int Start(size_t worker_count, ExecuteSqlFn exec_fn);
    void Stop();

    int Submit(const std::string& runtime_task_id,
               std::vector<std::string> sqls,
               int timeout_s,
               std::string* err_msg);
    int Query(const std::string& runtime_task_id, BatchRuntimeSnapshot* out) const;
    int RequestStop(const std::string& runtime_task_id, std::string* err_msg, bool cancel = false);
    static std::shared_ptr<BlockTransformRunControl> CurrentControl();
    void SweepFinished(int64_t now_ms, int retention_s, size_t max_count);

 private:
    struct BatchRuntimeTask {
        std::string runtime_task_id;
        std::vector<std::string> sqls;
        int timeout_s = 0;
        std::atomic<bool> stop_requested{false};
        std::shared_ptr<BlockTransformRunControl> control = std::make_shared<BlockTransformRunControl>();
        BatchRuntimeSnapshot snapshot;
    };

    void WorkerLoop();
    void ExecuteTask(const std::shared_ptr<BatchRuntimeTask>& task);
    static void ParseExecuteResult(const std::string& rsp,
                                   int64_t* rows,
                                   int64_t* cols,
                                   std::string* result_target);
    static void ParseExecuteError(const std::string& rsp,
                                  std::string* error_code,
                                  std::string* error_message,
                                  std::string* error_stage);
    static int64_t CurrentTimeMs();

 private:
    mutable std::mutex mu_;
    std::condition_variable cv_;
    bool running_ = false;
    ExecuteSqlFn exec_fn_;
    std::deque<std::string> queue_;
    std::unordered_map<std::string, std::shared_ptr<BatchRuntimeTask>> tasks_;
    std::vector<std::thread> workers_;
};

}  // namespace scheduler
}  // namespace flowsql

#endif  // _FLOWSQL_SERVICES_SCHEDULER_SCHEDULER_BATCH_RUNTIME_H_
