#ifndef _FLOWSQL_SERVICES_TASK_TASK_PLUGIN_H_
#define _FLOWSQL_SERVICES_TASK_TASK_PLUGIN_H_

#include <common/iplugin.h>
#include <framework/interfaces/irouter_handle.h>
#include <framework/interfaces/itask_store.h>

#include <sqlite3.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <set>

namespace flowsql {
namespace task {

class __attribute__((visibility("default"))) TaskPlugin : public IPlugin, public IRouterHandle, public ITaskStore {
 public:
    TaskPlugin() = default;
    ~TaskPlugin() override = default;

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
    int EnsureSchema();
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
                           const std::string& sqls_json,
                           int sql_count,
                           int timeout_s,
                           std::string* task_id,
                           bool enqueue);
    void CleanupIntermediateChannels(const std::set<std::string>& channels);
    std::string BuildDbPath() const;
    static const char* StatusName(TaskStatus s);
    static TaskStatus ParseStatus(const std::string& s);
    static bool IsTerminal(TaskStatus s);
    static std::string MakeNowTaskId(uint64_t seq);
    static std::string JsonError(const std::string& error);
    std::string DequeueTask();
    void WorkerLoop();
    void TimeoutLoop();
    int ExecuteOneTask(const std::string& task_id, std::string* execute_rsp = nullptr);
    int32_t HandleSubmit(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleList(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleDetail(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleDelete(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleCancel(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleDiagnostics(const std::string& uri, const std::string& req, std::string& rsp);

    IQuerier* querier_ = nullptr;
    sqlite3* db_ = nullptr;
    std::string db_dir_ = "./taskdb";
    std::string db_path_;
    int worker_threads_ = 4;
    int retention_days_ = 0;
    int retention_max_count_ = 0;
    bool disable_worker_ = false;
    std::atomic<bool> running_{false};
    std::vector<std::thread> workers_;
    std::thread timeout_thread_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::string> queue_;
    std::atomic<uint64_t> seq_{0};
};

}  // namespace task
}  // namespace flowsql

#endif  // _FLOWSQL_SERVICES_TASK_TASK_PLUGIN_H_
