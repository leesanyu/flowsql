// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_SERVICES_TASK_TASK_PLUGIN_H_
#define _FLOWSQL_SERVICES_TASK_TASK_PLUGIN_H_

#include <common/iplugin.h>
#include <framework/core/scheduler_control_client.h>
#include <framework/interfaces/irouter_handle.h>
#include <framework/interfaces/itask_store.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <set>
#include <vector>

namespace flowsql {
namespace task {

class TaskStoreSqlite;

class __attribute__((visibility("default"))) TaskPlugin : public IPlugin, public IRouterHandle, public ITaskStore {
 public:
    TaskPlugin();
    ~TaskPlugin() override;

    int Option(const char* arg) override;
    int Load(IQuerier* querier) override;
    int Unload() override;
    int Start() override;
    int Stop() override;

    void EnumRoutes(std::function<void(const RouteItem&)> callback) override;

    int CreateTask(const std::string& request_sql, std::string* task_id) override;
    int UpdateStatus(const std::string& task_id,
                     TaskStatus new_status,
                     const std::string& error_code,
                     const std::string& error_message,
                     const std::string& error_stage,
                     int64_t result_row_count,
                     int64_t result_col_count,
                     const std::string& result_target) override;
    int GetTask(const std::string& task_id, TaskRecord* out) override;
    int ListTasks(int page,
                  int page_size,
                  const std::string& status_filter,
                  std::vector<TaskRecord>* items,
                  int64_t* total) override;
    int DeleteTask(const std::string& task_id) override;

 private:
    int EnsureDb();
    int CleanupOrphans();
    int WriteTaskEvent(const std::string& task_id, const std::string& from_status,
                       const std::string& to_status, const std::string& message);
    int WriteDiagnostic(const std::string& task_id,
                        int sql_index,
                        const std::string& sql_text,
                        int64_t duration_ms,
                        int64_t source_rows,
                        int64_t sink_rows,
                        const std::string& operator_chain);
    int RunRetentionCleanup();
    int CreateTaskInternal(const std::string& request_sql,
                           const std::string& raw_sql_text,
                           const std::string& sqls_json,
                           int sql_count,
                           int timeout_s,
                           std::string* task_id,
                           const std::string& task_kind = "batch",
                           const std::string& runtime_task_id = "");
    void CleanupIntermediateChannels(const std::set<std::string>& channels);
    static const char* StatusName(TaskStatus s);
    static TaskStatus ParseStatus(const std::string& s);
    static const char* RuntimeStatusName(TaskStatus s);
    static TaskStatus MapStreamRuntimeStatus(const std::string& runtime_status);
    static TaskStatus MapBatchRuntimeStatus(const std::string& runtime_status);
    static bool IsTerminal(TaskStatus s);
    static std::string MakeNowTaskId(uint64_t seq);
    int UpdateRuntimeTaskId(const std::string& task_id, const std::string& runtime_task_id);
    int UpdateTaskKindAndRuntimeId(const std::string& task_id,
                                   const std::string& task_kind,
                                   const std::string& runtime_task_id);
    int ListTasksByKind(const std::string& task_kind,
                        int page,
                        int page_size,
                        const std::string& status_filter,
                        std::vector<TaskRecord>* items,
                        int64_t* total);
    void TimeoutLoop();
    int ExecuteOneTask(const std::string& task_id, std::string* execute_rsp = nullptr);
    int32_t HandleBatchExecute(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleSqlClassify(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleSqlAnalyze(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleList(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleDetail(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleDelete(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleCancel(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleDiagnostics(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleStreamExecute(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleStreamStop(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleStreamStatus(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleStreamList(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleRuntimeGraphQuery(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t ClassifySqlTaskKindViaScheduler(const std::string& sql, std::string* task_kind_out, std::string* err_rsp,
                                            bool* requires_async = nullptr);
    int SyncBatchRuntimeTask(TaskRecord* rec);

    IQuerier* querier_ = nullptr;
    SchedulerControlClient scheduler_client_;
    std::unique_ptr<TaskStoreSqlite> store_;
    std::string db_dir_ = "./taskdb";
    std::string db_path_;
    int retention_days_ = 0;
    int retention_max_count_ = 0;
    std::atomic<bool> running_{false};
    std::thread timeout_thread_;
    std::atomic<uint64_t> seq_{0};
};

}  // namespace task
}  // namespace flowsql

#endif  // _FLOWSQL_SERVICES_TASK_TASK_PLUGIN_H_
