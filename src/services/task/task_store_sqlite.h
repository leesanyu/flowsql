// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_SERVICES_TASK_TASK_STORE_SQLITE_H_
#define _FLOWSQL_SERVICES_TASK_TASK_STORE_SQLITE_H_

#include <framework/interfaces/itask_store.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct sqlite3;

namespace flowsql {
namespace task {

class TaskStoreSqlite {
 public:
    struct OpenOptions {
        std::string db_dir = "./taskdb";
        std::string db_path;
        int retention_days = 0;
        int retention_max_count = 0;
    };

    struct TaskCreateParams {
        std::string task_id;
        std::string request_sql;
        std::string raw_sql_text;
        std::string sqls_json;
        int sql_count = 1;
        int timeout_s = 0;
        std::string task_kind = "batch";
        std::string runtime_task_id;
    };

    struct TaskStatusUpdate {
        std::string task_id;
        TaskStatus new_status = TaskStatus::kPending;
        std::string error_code;
        std::string error_message;
        std::string error_stage;
        int64_t result_row_count = 0;
        int64_t result_col_count = 0;
        std::string result_target;
    };

    struct TaskDiagnostic {
        int sql_index = 0;
        std::string sql_text;
        int64_t duration_ms = 0;
        int64_t source_rows = 0;
        int64_t sink_rows = 0;
        std::string operator_chain;
        std::string created_at;
    };

    int Open(const OpenOptions& options);
    int Close();
    int CleanupOrphans();

    int QueryLastTaskId(std::string* task_id);

    int CreateTask(const TaskCreateParams& params);
    int UpdateStatus(const TaskStatusUpdate& update);
    int GetTask(const std::string& task_id, TaskRecord* out);
    int ListTasks(int page,
                  int page_size,
                  const std::string& status_filter,
                  std::vector<TaskRecord>* items,
                  int64_t* total);
    int ListTasksByKind(const std::string& task_kind,
                        int page,
                        int page_size,
                        const std::string& status_filter,
                        std::vector<TaskRecord>* items,
                        int64_t* total);
    // Internal opaque result summaries; keeps the public TaskRecord ABI unchanged.
    int SaveResultSummary(const std::string& task_id, const std::string& result_json);
    int GetResultSummary(const std::string& task_id, std::string* result_json);
    int DeleteTask(const std::string& task_id);
    int RunRetentionCleanup();

    int WriteTaskEvent(const std::string& task_id,
                       const std::string& from_status,
                       const std::string& to_status,
                       const std::string& message);
    int WriteDiagnostic(const std::string& task_id,
                        int sql_index,
                        const std::string& sql_text,
                        int64_t duration_ms,
                        int64_t source_rows,
                        int64_t sink_rows,
                        const std::string& operator_chain);

    int UpdateRuntimeTaskId(const std::string& task_id, const std::string& runtime_task_id);
    int UpdateTaskKindAndRuntimeId(const std::string& task_id,
                                   const std::string& task_kind,
                                   const std::string& runtime_task_id);

    int QueryTaskSqlPayload(const std::string& task_id,
                            std::string* raw_sql_text,
                            std::string* sqls_json,
                            int* sql_count = nullptr);
    int UpdateCurrentSqlIndex(const std::string& task_id, int index);
    int QueryTaskRuntimeFlags(const std::string& task_id, bool* terminal_now, bool* cancel_requested);
    int RequestCancelRunningTask(const std::string& task_id);
    int ListTimedOutTaskIds(std::vector<std::string>* timed_out_ids);
    int ListDiagnostics(const std::string& task_id, std::vector<TaskDiagnostic>* items);

 private:
    static const char* StatusName(TaskStatus s);
    static TaskStatus ParseStatus(const std::string& s);
    static bool IsTerminal(TaskStatus s);

    std::string BuildDbPathNoLock() const;
    int EnsureSchemaNoLock();
    int WriteTaskEventNoLock(const std::string& task_id,
                             const std::string& from_status,
                             const std::string& to_status,
                             const std::string& message);
    int DeleteTaskNoLock(const std::string& task_id);
    int RunRetentionCleanupNoLock();

    mutable std::mutex db_mu_;
    sqlite3* db_ = nullptr;
    std::string db_dir_ = "./taskdb";
    std::string db_path_;
    int retention_days_ = 0;
    int retention_max_count_ = 0;
};

}  // namespace task
}  // namespace flowsql

#endif  // _FLOWSQL_SERVICES_TASK_TASK_STORE_SQLITE_H_
