// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "scheduler_batch_runtime.h"

#include <rapidjson/document.h>

#include <algorithm>
#include <cerrno>
#include <chrono>

namespace flowsql {
namespace scheduler {

namespace {
thread_local std::shared_ptr<BlockTransformRunControl> current_control;

BatchRuntimeStatus ParseExecuteTerminalStatus(const std::string& rsp) {
    if (rsp.empty()) return BatchRuntimeStatus::kCompleted;
    rapidjson::Document d;
    d.Parse(rsp.c_str());
    if (d.HasParseError() || !d.IsObject() ||
        !d.HasMember("status") || !d["status"].IsString()) {
        return BatchRuntimeStatus::kCompleted;
    }
    const std::string status = d["status"].GetString();
    if (status == "stopped") return BatchRuntimeStatus::kStopped;
    if (status == "cancelled") return BatchRuntimeStatus::kCancelled;
    return BatchRuntimeStatus::kCompleted;
}

}  // namespace

const char* BatchRuntimeStatusName(BatchRuntimeStatus status) {
    switch (status) {
        case BatchRuntimeStatus::kPending: return "pending";
        case BatchRuntimeStatus::kRunning: return "running";
        case BatchRuntimeStatus::kStopping: return "stopping";
        case BatchRuntimeStatus::kStopped: return "stopped";
        case BatchRuntimeStatus::kCancelled: return "cancelled";
        case BatchRuntimeStatus::kCompleted: return "completed";
        case BatchRuntimeStatus::kFailed: return "failed";
        case BatchRuntimeStatus::kTimeout: return "timeout";
        default: return "failed";
    }
}

bool IsTerminalBatchRuntimeStatus(BatchRuntimeStatus status) {
    return status == BatchRuntimeStatus::kStopped ||
           status == BatchRuntimeStatus::kCancelled ||
           status == BatchRuntimeStatus::kCompleted ||
           status == BatchRuntimeStatus::kFailed ||
           status == BatchRuntimeStatus::kTimeout;
}

std::shared_ptr<BlockTransformRunControl> SchedulerBatchRuntime::CurrentControl() { return current_control; }

SchedulerBatchRuntime::~SchedulerBatchRuntime() {
    Stop();
}

int SchedulerBatchRuntime::Start(size_t worker_count, ExecuteSqlFn exec_fn) {
    if (!exec_fn) return EINVAL;
    std::lock_guard<std::mutex> lock(mu_);
    if (running_) return 0;
    running_ = true;
    exec_fn_ = std::move(exec_fn);
    if (worker_count == 0) worker_count = 1;
    workers_.reserve(worker_count);
    for (size_t i = 0; i < worker_count; ++i) {
        workers_.emplace_back([this]() { WorkerLoop(); });
    }
    return 0;
}

void SchedulerBatchRuntime::Stop() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!running_) return;
        running_ = false;
        for (auto& kv : tasks_) {
            if (!kv.second) continue;
            kv.second->stop_requested.store(true, std::memory_order_release);
            kv.second->control->Request(true);
            if (!IsTerminalBatchRuntimeStatus(kv.second->snapshot.status)) {
                kv.second->snapshot.status = BatchRuntimeStatus::kStopping;
                kv.second->snapshot.last_active_ms = CurrentTimeMs();
            }
        }
    }
    cv_.notify_all();
    for (auto& th : workers_) {
        if (th.joinable()) th.join();
    }
    workers_.clear();
}

int SchedulerBatchRuntime::Submit(const std::string& runtime_task_id,
                                  std::vector<std::string> sqls,
                                  int timeout_s,
                                  std::string* err_msg) {
    if (runtime_task_id.empty()) {
        if (err_msg) *err_msg = "runtime_task_id is empty";
        return EINVAL;
    }
    if (sqls.empty()) {
        if (err_msg) *err_msg = "batch runtime SQL list is empty";
        return EINVAL;
    }
    if (timeout_s < 0) {
        if (err_msg) *err_msg = "timeout_s must be >= 0";
        return EINVAL;
    }

    auto task = std::make_shared<BatchRuntimeTask>();
    task->runtime_task_id = runtime_task_id;
    task->sqls = std::move(sqls);
    task->timeout_s = timeout_s;
    task->snapshot.runtime_task_id = runtime_task_id;
    task->snapshot.status = BatchRuntimeStatus::kPending;
    task->snapshot.sql_count = static_cast<int>(task->sqls.size());
    task->snapshot.timeout_s = timeout_s;
    task->snapshot.current_sql_index = 0;
    const int64_t now = CurrentTimeMs();
    task->snapshot.created_ms = now;
    task->snapshot.last_active_ms = now;

    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!running_) {
            if (err_msg) *err_msg = "batch runtime is not running";
            return EBUSY;
        }
        if (tasks_.find(runtime_task_id) != tasks_.end()) {
            if (err_msg) *err_msg = "runtime_task_id already exists";
            return EEXIST;
        }
        tasks_[runtime_task_id] = task;
        queue_.push_back(runtime_task_id);
    }
    cv_.notify_one();
    return 0;
}

int SchedulerBatchRuntime::Query(const std::string& runtime_task_id, BatchRuntimeSnapshot* out) const {
    if (!out || runtime_task_id.empty()) return EINVAL;
    std::lock_guard<std::mutex> lock(mu_);
    auto it = tasks_.find(runtime_task_id);
    if (it == tasks_.end() || !it->second) return ENOENT;
    *out = it->second->snapshot;
    out->managed_result_json = it->second->control->Snapshot();
    return 0;
}

int SchedulerBatchRuntime::RequestStop(const std::string& runtime_task_id, std::string* err_msg, bool cancel) {
    if (runtime_task_id.empty()) {
        if (err_msg) *err_msg = "runtime_task_id is empty";
        return EINVAL;
    }
    std::lock_guard<std::mutex> lock(mu_);
    auto it = tasks_.find(runtime_task_id);
    if (it == tasks_.end() || !it->second) {
        if (err_msg) *err_msg = "batch runtime task not found";
        return ENOENT;
    }
    auto task = it->second;
    task->stop_requested.store(true, std::memory_order_release);
    task->control->Request(cancel);
    if (IsTerminalBatchRuntimeStatus(task->snapshot.status)) return 0;
    if (task->snapshot.status == BatchRuntimeStatus::kPending) {
        task->snapshot.status = BatchRuntimeStatus::kCancelled;
        task->snapshot.finished_ms = CurrentTimeMs();
    } else {
        task->snapshot.status = BatchRuntimeStatus::kStopping;
    }
    task->snapshot.last_active_ms = CurrentTimeMs();
    return 0;
}

void SchedulerBatchRuntime::SweepFinished(int64_t now_ms, int retention_s, size_t max_count) {
    const int64_t now = now_ms > 0 ? now_ms : CurrentTimeMs();
    std::vector<std::shared_ptr<BatchRuntimeTask>> terminal;
    {
        std::lock_guard<std::mutex> lock(mu_);
        terminal.reserve(tasks_.size());
        for (const auto& kv : tasks_) {
            if (kv.second && IsTerminalBatchRuntimeStatus(kv.second->snapshot.status)) {
                terminal.push_back(kv.second);
            }
        }
    }
    if (terminal.empty()) return;

    std::vector<std::string> remove_ids;
    const int64_t retention_ms = static_cast<int64_t>(retention_s) * 1000;
    if (retention_s == 0) {
        for (const auto& task : terminal) {
            remove_ids.push_back(task->runtime_task_id);
        }
    } else {
        for (const auto& task : terminal) {
            const int64_t finished = task->snapshot.finished_ms > 0
                                         ? task->snapshot.finished_ms
                                         : task->snapshot.last_active_ms;
            if (finished > 0 && now - finished >= retention_ms) {
                remove_ids.push_back(task->runtime_task_id);
            }
        }
    }

    if (max_count > 0 && terminal.size() > max_count) {
        std::vector<std::pair<std::string, int64_t>> keep;
        keep.reserve(terminal.size());
        for (const auto& task : terminal) {
            keep.push_back({task->runtime_task_id, task->snapshot.last_active_ms});
        }
        std::sort(keep.begin(), keep.end(),
                  [](const auto& a, const auto& b) {
                      if (a.second != b.second) return a.second < b.second;
                      return a.first < b.first;
                  });
        const size_t over = keep.size() - max_count;
        for (size_t i = 0; i < over; ++i) {
            remove_ids.push_back(keep[i].first);
        }
    }

    if (remove_ids.empty()) return;
    std::sort(remove_ids.begin(), remove_ids.end());
    remove_ids.erase(std::unique(remove_ids.begin(), remove_ids.end()), remove_ids.end());

    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& id : remove_ids) {
        tasks_.erase(id);
    }
}

void SchedulerBatchRuntime::WorkerLoop() {
    while (true) {
        std::shared_ptr<BatchRuntimeTask> task;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [this]() {
                return !running_ || !queue_.empty();
            });
            if (!running_ && queue_.empty()) return;
            if (queue_.empty()) continue;
            const std::string runtime_task_id = queue_.front();
            queue_.pop_front();
            auto it = tasks_.find(runtime_task_id);
            if (it == tasks_.end() || !it->second) continue;
            task = it->second;
            if (IsTerminalBatchRuntimeStatus(task->snapshot.status)) {
                continue;
            }
            if (task->stop_requested.load(std::memory_order_acquire)) {
                task->snapshot.status = BatchRuntimeStatus::kCancelled;
                task->snapshot.finished_ms = CurrentTimeMs();
                task->snapshot.last_active_ms = task->snapshot.finished_ms;
                continue;
            }
            task->snapshot.status = BatchRuntimeStatus::kRunning;
            const int64_t now = CurrentTimeMs();
            task->snapshot.started_ms = now;
            task->snapshot.last_active_ms = now;
        }
        ExecuteTask(task);
    }
}

void SchedulerBatchRuntime::ExecuteTask(const std::shared_ptr<BatchRuntimeTask>& task) {
    if (!task) return;
    for (size_t i = 0; i < task->sqls.size(); ++i) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (task->stop_requested.load(std::memory_order_acquire)) {
                task->snapshot.status = BatchRuntimeStatus::kCancelled;
                task->snapshot.finished_ms = CurrentTimeMs();
                task->snapshot.last_active_ms = task->snapshot.finished_ms;
                return;
            }
            if (task->timeout_s > 0 &&
                task->snapshot.started_ms > 0 &&
                CurrentTimeMs() - task->snapshot.started_ms >= static_cast<int64_t>(task->timeout_s) * 1000) {
                task->snapshot.status = BatchRuntimeStatus::kTimeout;
                task->snapshot.error_code = "TIMEOUT";
                task->snapshot.error_stage = "timeout";
                task->snapshot.error_message = "batch runtime task timeout";
                task->snapshot.finished_ms = CurrentTimeMs();
                task->snapshot.last_active_ms = task->snapshot.finished_ms;
                return;
            }
            task->snapshot.current_sql_index = static_cast<int>(i);
            task->snapshot.last_active_ms = CurrentTimeMs();
        }

        std::string exec_rsp;
        current_control = task->control;
        const int32_t rc = exec_fn_(task->sqls[i], &exec_rsp);
        current_control.reset();
        if (rc != error::OK) {
            std::string err_code;
            std::string err_message;
            std::string err_stage;
            ParseExecuteError(exec_rsp, &err_code, &err_message, &err_stage);
            if (err_code.empty()) err_code = rc == error::UNAVAILABLE ? "SCHEDULER_UNAVAILABLE" : "EXECUTION_FAILED";
            if (err_stage.empty()) err_stage = rc == error::UNAVAILABLE ? "dispatch" : "execute";
            if (err_message.empty()) err_message = "batch SQL execute failed";

            std::lock_guard<std::mutex> lock(mu_);
            task->snapshot.status =
                task->control->cancel_requested.load() ? BatchRuntimeStatus::kCancelled : BatchRuntimeStatus::kFailed;
            task->snapshot.error_code = err_code;
            task->snapshot.error_message = err_message;
            task->snapshot.error_stage = err_stage;
            task->snapshot.finished_ms = CurrentTimeMs();
            task->snapshot.last_active_ms = task->snapshot.finished_ms;
            return;
        }

        int64_t rows = 0;
        int64_t cols = 0;
        std::string target;
        const BatchRuntimeStatus execute_status = ParseExecuteTerminalStatus(exec_rsp);
        ParseExecuteResult(exec_rsp, &rows, &cols, &target);
        {
            std::lock_guard<std::mutex> lock(mu_);
            task->snapshot.result_row_count = rows;
            task->snapshot.result_col_count = cols;
            task->snapshot.result_target = target;
            task->snapshot.last_active_ms = CurrentTimeMs();
            if (task->timeout_s > 0 &&
                task->snapshot.started_ms > 0 &&
                CurrentTimeMs() - task->snapshot.started_ms >= static_cast<int64_t>(task->timeout_s) * 1000) {
                task->snapshot.status = BatchRuntimeStatus::kTimeout;
                task->snapshot.error_code = "TIMEOUT";
                task->snapshot.error_stage = "timeout";
                task->snapshot.error_message = "batch runtime task timeout";
                task->snapshot.finished_ms = CurrentTimeMs();
                task->snapshot.last_active_ms = task->snapshot.finished_ms;
                return;
            }
            if (execute_status == BatchRuntimeStatus::kStopped ||
                execute_status == BatchRuntimeStatus::kCancelled) {
                task->snapshot.status = execute_status;
                task->snapshot.finished_ms = CurrentTimeMs();
                task->snapshot.last_active_ms = task->snapshot.finished_ms;
                return;
            }
        }
    }

    std::lock_guard<std::mutex> lock(mu_);
    if (task->timeout_s > 0 &&
        task->snapshot.started_ms > 0 &&
        CurrentTimeMs() - task->snapshot.started_ms >= static_cast<int64_t>(task->timeout_s) * 1000) {
        task->snapshot.status = BatchRuntimeStatus::kTimeout;
        task->snapshot.error_code = "TIMEOUT";
        task->snapshot.error_stage = "timeout";
        task->snapshot.error_message = "batch runtime task timeout";
    } else if (task->stop_requested.load(std::memory_order_acquire)) {
        task->snapshot.status = BatchRuntimeStatus::kCancelled;
    } else if (task->snapshot.status == BatchRuntimeStatus::kStopping) {
        task->snapshot.status = BatchRuntimeStatus::kStopped;
    } else if (task->snapshot.status != BatchRuntimeStatus::kFailed &&
               task->snapshot.status != BatchRuntimeStatus::kTimeout) {
        task->snapshot.status = BatchRuntimeStatus::kCompleted;
    }
    task->snapshot.current_sql_index = task->snapshot.sql_count;
    task->snapshot.finished_ms = CurrentTimeMs();
    task->snapshot.last_active_ms = task->snapshot.finished_ms;
}

void SchedulerBatchRuntime::ParseExecuteResult(const std::string& rsp,
                                               int64_t* rows,
                                               int64_t* cols,
                                               std::string* result_target) {
    if (rows) *rows = 0;
    if (cols) *cols = 0;
    if (result_target) result_target->clear();
    if (rsp.empty()) return;
    rapidjson::Document d;
    d.Parse(rsp.c_str());
    if (d.HasParseError() || !d.IsObject()) return;
    if (rows) {
        if (d.HasMember("result_row_count") && d["result_row_count"].IsInt64()) {
            *rows = d["result_row_count"].GetInt64();
        } else if (d.HasMember("rows") && d["rows"].IsInt64()) {
            *rows = d["rows"].GetInt64();
        }
    }
    if (cols) {
        if (d.HasMember("result_col_count") && d["result_col_count"].IsInt64()) {
            *cols = d["result_col_count"].GetInt64();
        } else if (d.HasMember("cols") && d["cols"].IsInt64()) {
            *cols = d["cols"].GetInt64();
        }
    }
    if (result_target && d.HasMember("result_target") && d["result_target"].IsString()) {
        *result_target = d["result_target"].GetString();
    }
}

void SchedulerBatchRuntime::ParseExecuteError(const std::string& rsp,
                                              std::string* error_code,
                                              std::string* error_message,
                                              std::string* error_stage) {
    if (error_code) error_code->clear();
    if (error_message) error_message->clear();
    if (error_stage) error_stage->clear();
    if (rsp.empty()) return;
    rapidjson::Document d;
    d.Parse(rsp.c_str());
    if (d.HasParseError() || !d.IsObject()) return;
    if (error_code && d.HasMember("error_code") && d["error_code"].IsString()) {
        *error_code = d["error_code"].GetString();
    }
    if (error_message && d.HasMember("error") && d["error"].IsString()) {
        *error_message = d["error"].GetString();
    }
    if (error_stage && d.HasMember("error_stage") && d["error_stage"].IsString()) {
        *error_stage = d["error_stage"].GetString();
    }
}

int64_t SchedulerBatchRuntime::CurrentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

}  // namespace scheduler
}  // namespace flowsql
