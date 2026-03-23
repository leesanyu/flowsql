#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_ITASK_STORE_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_ITASK_STORE_H_

#include <common/guid.h>
#include <common/typedef.h>

#include <cstdint>
#include <string>
#include <vector>

namespace flowsql {

// {3f5b7601-92a4-4c5d-8b9d-26f5bb6ebd31}
const Guid IID_TASK_STORE = {
    0x3f5b7601, 0x92a4, 0x4c5d, {0x8b, 0x9d, 0x26, 0xf5, 0xbb, 0x6e, 0xbd, 0x31}
};

enum class TaskStatus : int32_t {
    kPending = 0,
    kRunning = 1,
    kCompleted = 2,
    kFailed = 3,
    kCancelled = 4,
    kTimeout = 5,
};

struct TaskRecord {
    std::string task_id;
    std::string request_sql;
    TaskStatus status = TaskStatus::kPending;
    int64_t result_row_count = 0;
    int64_t result_col_count = 0;
    std::string result_target;
    std::string error_code;
    std::string error_message;
    std::string error_stage;
    std::string created_at;
    std::string started_at;
    std::string updated_at;
    std::string finished_at;
};

interface ITaskStore {
    virtual ~ITaskStore() = default;

    virtual int CreateTask(const std::string& request_sql, std::string* task_id) = 0;
    virtual int UpdateStatus(const std::string& task_id,
                             TaskStatus new_status,
                             const std::string& error_code,
                             const std::string& error_message,
                             const std::string& error_stage,
                             int64_t result_row_count,
                             int64_t result_col_count,
                             const std::string& result_target) = 0;
    virtual int GetTask(const std::string& task_id, TaskRecord* out) = 0;
    virtual int ListTasks(int page,
                          int page_size,
                          const std::string& status_filter,
                          std::vector<TaskRecord>* items,
                          int64_t* total) = 0;
    virtual int DeleteTask(const std::string& task_id) = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_ITASK_STORE_H_
