#include "task_plugin.h"

#include <common/error_code.h>
#include <common/log.h>
#include <framework/interfaces/irouter_handle.h>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <algorithm>
#include <cctype>

namespace fs = std::filesystem;

namespace flowsql {
namespace task {

namespace {

static const char* kSchemaSql =
    "CREATE TABLE IF NOT EXISTS tasks ("
    "task_id TEXT PRIMARY KEY,"
    "request_sql TEXT NOT NULL,"
    "status TEXT NOT NULL,"
    "error_code TEXT NOT NULL DEFAULT '',"
    "error_message TEXT NOT NULL DEFAULT '',"
    "error_stage TEXT NOT NULL DEFAULT '',"
    "result_row_count INTEGER NOT NULL DEFAULT 0,"
    "result_col_count INTEGER NOT NULL DEFAULT 0,"
    "result_target TEXT NOT NULL DEFAULT '',"
    "created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"
    "started_at DATETIME,"
    "updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"
    "finished_at DATETIME"
    ");"
    "CREATE TABLE IF NOT EXISTS task_events ("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "task_id TEXT NOT NULL,"
    "from_status TEXT,"
    "to_status TEXT NOT NULL,"
    "event_time DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"
    "message TEXT"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_task_events_task_id ON task_events(task_id);";

static std::string TruncateSql(const std::string& sql) {
    static constexpr size_t kMaxSql = 4096;
    if (sql.size() <= kMaxSql) return sql;
    return sql.substr(0, kMaxSql);
}

}  // namespace

int TaskPlugin::Option(const char* arg) {
    if (!arg || !*arg) return 0;
    std::string opts(arg);
    size_t pos = 0;
    while (pos < opts.size()) {
        size_t eq = opts.find('=', pos);
        if (eq == std::string::npos) break;
        size_t end = opts.find(';', eq);
        if (end == std::string::npos) end = opts.size();
        std::string key = opts.substr(pos, eq - pos);
        std::string val = opts.substr(eq + 1, end - eq - 1);
        if (key == "db_dir" && !val.empty()) db_dir_ = val;
        if (key == "db_path" && !val.empty()) db_path_ = val;
        if (key == "disable_worker" && (val == "1" || val == "true")) disable_worker_ = true;
        pos = (end < opts.size()) ? end + 1 : opts.size();
    }
    return 0;
}

int TaskPlugin::Load(IQuerier* querier) {
    querier_ = querier;
    return 0;
}

int TaskPlugin::Unload() {
    Stop();
    return 0;
}

int TaskPlugin::EnsureSchema() {
    if (!db_) return -1;
    if (sqlite3_exec(db_, kSchemaSql, nullptr, nullptr, nullptr) != SQLITE_OK) return -1;
    // 向后兼容：老库没有 result_col_count 列时自动补齐。
    (void)sqlite3_exec(db_, "ALTER TABLE tasks ADD COLUMN result_col_count INTEGER NOT NULL DEFAULT 0;",
                       nullptr, nullptr, nullptr);
    // 向后兼容：老库没有 started_at 列时自动补齐。
    (void)sqlite3_exec(db_, "ALTER TABLE tasks ADD COLUMN started_at DATETIME;",
                       nullptr, nullptr, nullptr);
    return 0;
}

std::string TaskPlugin::BuildDbPath() const {
    if (!db_path_.empty()) return db_path_;
    fs::path p(db_dir_);
    p /= "task_store.db";
    return p.string();
}

int TaskPlugin::EnsureDb() {
    if (db_) return 0;
    std::error_code ec;
    const std::string db_path = BuildDbPath();
    fs::path parent = fs::path(db_path).parent_path();
    if (!parent.empty()) {
        fs::create_directories(parent, ec);
        if (ec) return -1;
    } else if (db_path_.empty()) {
        fs::create_directories(db_dir_, ec);
        if (ec) return -1;
    }
    if (sqlite3_open(db_path.c_str(), &db_) != SQLITE_OK) {
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        return -1;
    }
    (void)sqlite3_exec(db_, "PRAGMA journal_mode=WAL", nullptr, nullptr, nullptr);
    return EnsureSchema();
}

int TaskPlugin::CleanupOrphans() {
    if (!db_) return -1;
    // 查出所有非终态任务
    sqlite3_stmt* sel = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT task_id, status FROM tasks WHERE status IN ('pending','running');",
                           -1, &sel, nullptr) != SQLITE_OK) return -1;
    std::vector<std::pair<std::string, std::string>> orphans;
    while (sqlite3_step(sel) == SQLITE_ROW) {
        const unsigned char* id  = sqlite3_column_text(sel, 0);
        const unsigned char* st  = sqlite3_column_text(sel, 1);
        if (id && st) orphans.push_back({reinterpret_cast<const char*>(id), reinterpret_cast<const char*>(st)});
    }
    sqlite3_finalize(sel);
    if (orphans.empty()) return 0;

    // 批量置为 failed
    const char* upd_sql =
        "UPDATE tasks "
        "SET status='failed', "
        "error_code='PROCESS_RESTART', "
        "error_message='task interrupted by process restart', "
        "error_stage='bootstrap', "
        "updated_at=CURRENT_TIMESTAMP, "
        "finished_at=CURRENT_TIMESTAMP "
        "WHERE status IN ('pending','running');";
    if (sqlite3_exec(db_, upd_sql, nullptr, nullptr, nullptr) != SQLITE_OK) return -1;

    // 为每条孤儿任务写入事件记录
    for (const auto& [id, from_st] : orphans) {
        WriteTaskEvent(id, from_st, "failed", "PROCESS_RESTART");
    }
    return 0;
}

int TaskPlugin::Start() {
    if (EnsureDb() != 0) return -1;
    if (CleanupOrphans() != 0) return -1;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT task_id FROM tasks ORDER BY task_id DESC LIMIT 1;", -1, &stmt, nullptr) ==
        SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char* txt = sqlite3_column_text(stmt, 0);
            if (txt) {
                std::string last(reinterpret_cast<const char*>(txt));
                size_t pos = last.rfind('_');
                if (pos != std::string::npos)
                    seq_.store(static_cast<uint64_t>(std::strtoull(last.c_str() + pos + 1, nullptr, 10)));
            }
        }
        sqlite3_finalize(stmt);
    }

    running_.store(true);
    if (!disable_worker_) worker_ = std::thread([this]() { WorkerLoop(); });
    return 0;
}

int TaskPlugin::Stop() {
    running_.store(false);
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        queue_.clear();
    }
    return 0;
}

const char* TaskPlugin::StatusName(TaskStatus s) {
    switch (s) {
        case TaskStatus::kPending: return "pending";
        case TaskStatus::kRunning: return "running";
        case TaskStatus::kCompleted: return "completed";
        case TaskStatus::kFailed: return "failed";
        case TaskStatus::kCancelled: return "cancelled";
        case TaskStatus::kTimeout: return "timeout";
        default: return "failed";
    }
}

TaskStatus TaskPlugin::ParseStatus(const std::string& s) {
    if (s == "pending") return TaskStatus::kPending;
    if (s == "running") return TaskStatus::kRunning;
    if (s == "completed") return TaskStatus::kCompleted;
    if (s == "failed") return TaskStatus::kFailed;
    if (s == "cancelled") return TaskStatus::kCancelled;
    if (s == "timeout") return TaskStatus::kTimeout;
    return TaskStatus::kFailed;
}

bool TaskPlugin::IsTerminal(TaskStatus s) {
    return s == TaskStatus::kCompleted || s == TaskStatus::kFailed || s == TaskStatus::kCancelled || s == TaskStatus::kTimeout;
}

int TaskPlugin::WriteTaskEvent(const std::string& task_id, const std::string& from_status,
                               const std::string& to_status, const std::string& message) {
    if (!db_ || task_id.empty() || to_status.empty()) return -1;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO task_events(task_id, from_status, to_status, message) VALUES(?1, ?2, ?3, ?4);";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, task_id.c_str(), -1, SQLITE_TRANSIENT);
    if (from_status.empty()) sqlite3_bind_null(stmt, 2);
    else sqlite3_bind_text(stmt, 2, from_status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, to_status.c_str(), -1, SQLITE_TRANSIENT);
    if (message.empty()) sqlite3_bind_null(stmt, 4);
    else sqlite3_bind_text(stmt, 4, message.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return 0;
}

std::string TaskPlugin::MakeNowTaskId(uint64_t seq) {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_now;
#ifdef _WIN32
    localtime_s(&tm_now, &t);
#else
    localtime_r(&t, &tm_now);
#endif
    char ts[32] = {0};
    std::strftime(ts, sizeof(ts), "%Y%m%d%H%M%S", &tm_now);
    return std::string("tsk_") + ts + "_" + std::to_string(seq);
}

std::string TaskPlugin::JsonError(const std::string& error_text) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("error");
    w.String(error_text.c_str());
    w.EndObject();
    return buf.GetString();
}

int TaskPlugin::CreateTask(const std::string& request_sql, std::string* task_id) {
    return CreateTaskInternal(request_sql, task_id, true);
}

int TaskPlugin::CreateTaskInternal(const std::string& request_sql, std::string* task_id, bool enqueue) {
    if (!task_id || request_sql.empty()) return -1;
    if (EnsureDb() != 0) return -1;
    const std::string id = MakeNowTaskId(++seq_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO tasks(task_id, request_sql, status) VALUES(?1, ?2, 'pending');";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    const std::string truncated = TruncateSql(request_sql);
    sqlite3_bind_text(stmt, 2, truncated.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -1;
    *task_id = id;
    if (enqueue) {
        std::lock_guard<std::mutex> lock(mu_);
        queue_.push_back(id);
        cv_.notify_one();
    }
    return 0;
}

int TaskPlugin::UpdateStatus(const std::string& task_id,
                             TaskStatus new_status,
                             const std::string& error_code,
                             const std::string& error_message,
                             const std::string& error_stage,
                             int64_t result_row_count,
                             int64_t result_col_count,
                             const std::string& result_target) {
    if (task_id.empty() || !db_) return -1;

    // 先查当前状态，用于终态保护和事件记录
    std::string cur_status_str;
    {
        sqlite3_stmt* sel = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT status FROM tasks WHERE task_id=?1;", -1, &sel, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(sel, 1, task_id.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(sel) == SQLITE_ROW) {
                const unsigned char* v = sqlite3_column_text(sel, 0);
                if (v) cur_status_str = reinterpret_cast<const char*>(v);
            }
            sqlite3_finalize(sel);
        }
    }
    if (cur_status_str.empty()) return -1;  // 任务不存在
    if (IsTerminal(ParseStatus(cur_status_str))) return 1;  // 终态保护，返回 1 表示冲突

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "UPDATE tasks SET status=?1, error_code=?2, error_message=?3, error_stage=?4, "
        "result_row_count=?5, result_col_count=?6, result_target=?7, updated_at=CURRENT_TIMESTAMP, "
        "started_at=CASE WHEN ?8=1 AND started_at IS NULL THEN CURRENT_TIMESTAMP ELSE started_at END, "
        "finished_at=CASE WHEN ?9=1 THEN CURRENT_TIMESTAMP ELSE finished_at END "
        "WHERE task_id=?10 AND status NOT IN ('completed','failed','cancelled','timeout');";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, StatusName(new_status), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, error_code.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, error_message.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, error_stage.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 5, result_row_count);
    sqlite3_bind_int64(stmt, 6, result_col_count);
    sqlite3_bind_text(stmt, 7, result_target.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 8, (new_status == TaskStatus::kRunning) ? 1 : 0);
    sqlite3_bind_int(stmt, 9, IsTerminal(new_status) ? 1 : 0);
    sqlite3_bind_text(stmt, 10, task_id.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(stmt);
    const int changed = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE || changed <= 0) return -1;

    WriteTaskEvent(task_id, cur_status_str, StatusName(new_status), error_code);
    return 0;
}

int TaskPlugin::GetTask(const std::string& task_id, TaskRecord* out) {
    if (task_id.empty() || !out || !db_) return -1;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT task_id, request_sql, status, error_code, error_message, error_stage, "
        "result_row_count, result_col_count, result_target, created_at, IFNULL(started_at,''), updated_at, IFNULL(finished_at,'') "
        "FROM tasks WHERE task_id=?1;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, task_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return -1;
    }
    auto txt = [stmt](int idx) -> std::string {
        const unsigned char* v = sqlite3_column_text(stmt, idx);
        return v ? reinterpret_cast<const char*>(v) : "";
    };
    out->task_id = txt(0);
    out->request_sql = txt(1);
    out->status = ParseStatus(txt(2));
    out->error_code = txt(3);
    out->error_message = txt(4);
    out->error_stage = txt(5);
    out->result_row_count = sqlite3_column_int64(stmt, 6);
    out->result_col_count = sqlite3_column_int64(stmt, 7);
    out->result_target = txt(8);
    out->created_at = txt(9);
    out->started_at = txt(10);
    out->updated_at = txt(11);
    out->finished_at = txt(12);
    sqlite3_finalize(stmt);
    return 0;
}

int TaskPlugin::ListTasks(int page,
                          int page_size,
                          const std::string& status_filter,
                          std::vector<TaskRecord>* items,
                          int64_t* total) {
    if (!items || !total || !db_) return -1;
    if (page < 1) page = 1;
    if (page_size < 1) page_size = 20;
    if (page_size > 100) page_size = 100;
    items->clear();
    *total = 0;

    std::string where;
    if (!status_filter.empty()) where = " WHERE status=?1";

    sqlite3_stmt* cnt = nullptr;
    std::string cnt_sql = "SELECT COUNT(1) FROM tasks" + where + ";";
    if (sqlite3_prepare_v2(db_, cnt_sql.c_str(), -1, &cnt, nullptr) != SQLITE_OK) return -1;
    if (!status_filter.empty()) sqlite3_bind_text(cnt, 1, status_filter.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(cnt) == SQLITE_ROW) *total = sqlite3_column_int64(cnt, 0);
    sqlite3_finalize(cnt);

    sqlite3_stmt* stmt = nullptr;
    std::string sql =
        "SELECT task_id, request_sql, status, error_code, error_message, error_stage, "
        "result_row_count, result_col_count, result_target, created_at, IFNULL(started_at,''), updated_at, IFNULL(finished_at,'') "
        "FROM tasks" + where + " ORDER BY created_at DESC LIMIT ? OFFSET ?;";
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return -1;
    int bind_idx = 1;
    if (!status_filter.empty()) sqlite3_bind_text(stmt, bind_idx++, status_filter.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, bind_idx++, page_size);
    sqlite3_bind_int(stmt, bind_idx, (page - 1) * page_size);

    auto txt = [stmt](int idx) -> std::string {
        const unsigned char* v = sqlite3_column_text(stmt, idx);
        return v ? reinterpret_cast<const char*>(v) : "";
    };
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        TaskRecord r;
        r.task_id = txt(0);
        r.request_sql = txt(1);
        r.status = ParseStatus(txt(2));
        r.error_code = txt(3);
        r.error_message = txt(4);
        r.error_stage = txt(5);
        r.result_row_count = sqlite3_column_int64(stmt, 6);
        r.result_col_count = sqlite3_column_int64(stmt, 7);
        r.result_target = txt(8);
        r.created_at = txt(9);
        r.started_at = txt(10);
        r.updated_at = txt(11);
        r.finished_at = txt(12);
        items->push_back(std::move(r));
    }
    sqlite3_finalize(stmt);
    return 0;
}

int TaskPlugin::DeleteTask(const std::string& task_id) {
    TaskRecord r;
    if (GetTask(task_id, &r) != 0) return -1;
    if (!IsTerminal(r.status)) return 1;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM tasks WHERE task_id=?1;", -1, &stmt, nullptr) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, task_id.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(stmt);
    const int changed = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE || changed <= 0) return -1;
    return 0;
}

std::string TaskPlugin::DequeueTask() {
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait(lock, [this]() { return !running_.load() || !queue_.empty(); });
    if (!running_.load() || queue_.empty()) return "";
    std::string id = queue_.front();
    queue_.pop_front();
    return id;
}

void TaskPlugin::WorkerLoop() {
    while (running_.load()) {
        std::string id = DequeueTask();
        if (id.empty()) continue;
        (void)ExecuteOneTask(id);
    }
}

int TaskPlugin::ExecuteOneTask(const std::string& task_id, std::string* execute_rsp) {
    TaskRecord rec;
    if (GetTask(task_id, &rec) != 0) return -1;
    if (UpdateStatus(task_id, TaskStatus::kRunning, "", "", "", 0, 0, "") != 0) return -1;

    fnRouterHandler exec_handler;
    if (querier_) {
        querier_->Traverse(IID_ROUTER_HANDLE, [&](void* p) -> int {
            auto* rh = static_cast<IRouterHandle*>(p);
            rh->EnumRoutes([&](const RouteItem& item) {
                if (item.method == "POST" && item.uri == "/tasks/instant/execute") exec_handler = item.handler;
            });
            return exec_handler ? -1 : 0;
        });
    }
    if (!exec_handler) {
        return UpdateStatus(task_id, TaskStatus::kFailed, "SCHEDULER_UNAVAILABLE", "execute route not found", "dispatch", 0, 0, "");
    }

    rapidjson::StringBuffer req_buf;
    rapidjson::Writer<rapidjson::StringBuffer> req_w(req_buf);
    req_w.StartObject();
    req_w.Key("sql");
    req_w.String(rec.request_sql.c_str());
    req_w.EndObject();

    std::string rsp;
    int32_t rc = exec_handler("/tasks/instant/execute", req_buf.GetString(), rsp);
    if (rc != error::OK) {
        rapidjson::Document d;
        d.Parse(rsp.c_str());
        std::string err = "execution failed";
        if (!d.HasParseError() && d.IsObject() && d.HasMember("error") && d["error"].IsString()) err = d["error"].GetString();
        if (execute_rsp) *execute_rsp = rsp;
        return UpdateStatus(task_id, TaskStatus::kFailed, "EXECUTION_FAILED", err, "execute", 0, 0, "");
    }

    rapidjson::Document d;
    d.Parse(rsp.c_str());
    int64_t rows = 0;
    int64_t cols = 0;
    int64_t derived_rows = 0;
    int64_t derived_cols = 0;
    if (!d.HasParseError() && d.IsObject() && d.HasMember("result_row_count") && d["result_row_count"].IsInt64()) {
        rows = d["result_row_count"].GetInt64();
    } else if (!d.HasParseError() && d.IsObject() && d.HasMember("rows") && d["rows"].IsInt64()) {
        rows = d["rows"].GetInt64();
    }
    if (!d.HasParseError() && d.IsObject() && d.HasMember("data")) {
        const auto& data = d["data"];
        if (data.IsObject()) {
            if (data.HasMember("columns") && data["columns"].IsArray()) {
                derived_cols = static_cast<int64_t>(data["columns"].Size());
            }
            if (data.HasMember("data") && data["data"].IsArray()) {
                derived_rows = static_cast<int64_t>(data["data"].Size());
            }
        } else if (data.IsArray()) {
            derived_rows = static_cast<int64_t>(data.Size());
            if (data.Size() > 0 && data[0].IsObject()) {
                derived_cols = static_cast<int64_t>(data[0].MemberCount());
            }
        }
    }
    if (rows <= 0 && derived_rows > 0) rows = derived_rows;
    cols = std::max<int64_t>(cols, derived_cols);
    if (execute_rsp) *execute_rsp = rsp;
    return UpdateStatus(task_id, TaskStatus::kCompleted, "", "", "", rows, cols, "");
}

int32_t TaskPlugin::HandleSubmit(const std::string&, const std::string& req, std::string& rsp) {
    rapidjson::Document d;
    d.Parse(req.c_str());
    if (d.HasParseError() || !d.IsObject() || !d.HasMember("sql") || !d["sql"].IsString()) {
        rsp = JsonError("invalid request, expected {\"sql\":\"...\"}");
        return error::BAD_REQUEST;
    }
    std::string mode = "async";
    if (d.HasMember("mode")) {
        if (!d["mode"].IsString()) {
            rsp = JsonError("invalid request, mode must be string: sync|async");
            return error::BAD_REQUEST;
        }
        mode = d["mode"].GetString();
        std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (mode != "sync" && mode != "async") {
            rsp = JsonError("invalid mode, expected sync|async");
            return error::BAD_REQUEST;
        }
    }
    const bool sync = (mode == "sync");

    std::string task_id;
    if (CreateTaskInternal(d["sql"].GetString(), &task_id, !sync) != 0) {
        rsp = JsonError("failed to create task");
        return error::INTERNAL_ERROR;
    }

    if (!sync) {
        rapidjson::StringBuffer buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(buf);
        w.StartObject();
        w.Key("task_id");
        w.String(task_id.c_str());
        w.Key("status");
        w.String("pending");
        w.EndObject();
        rsp = buf.GetString();
        return error::OK;
    }

    std::string exec_rsp;
    if (ExecuteOneTask(task_id, &exec_rsp) != 0) {
        rsp = JsonError("failed to execute task");
        return error::INTERNAL_ERROR;
    }

    TaskRecord rec;
    if (GetTask(task_id, &rec) != 0) {
        rsp = JsonError("failed to fetch task result");
        return error::INTERNAL_ERROR;
    }

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("task_id");
    w.String(task_id.c_str());
    w.Key("status");
    w.String(StatusName(rec.status));
    if (rec.status == TaskStatus::kCompleted) {
        w.Key("rows");
        w.Int64(rec.result_row_count);
        w.Key("data");
        rapidjson::Document exec_doc;
        exec_doc.Parse(exec_rsp.c_str());
        if (!exec_doc.HasParseError() && exec_doc.IsObject() && exec_doc.HasMember("data") && exec_doc["data"].IsArray()) {
            exec_doc["data"].Accept(w);
        } else if (!exec_doc.HasParseError() && exec_doc.IsObject() && exec_doc.HasMember("data") && exec_doc["data"].IsObject()) {
            const auto& df = exec_doc["data"];
            if (df.HasMember("columns") && df["columns"].IsArray() && df.HasMember("data") && df["data"].IsArray()) {
                const auto& cols = df["columns"];
                const auto& rows = df["data"];
                w.StartArray();
                for (rapidjson::SizeType r = 0; r < rows.Size(); ++r) {
                    if (!rows[r].IsArray()) continue;
                    const auto& row = rows[r];
                    const rapidjson::SizeType n = std::min(cols.Size(), row.Size());
                    w.StartObject();
                    for (rapidjson::SizeType i = 0; i < n; ++i) {
                        if (!cols[i].IsString()) continue;
                        w.Key(cols[i].GetString());
                        row[i].Accept(w);
                    }
                    w.EndObject();
                }
                w.EndArray();
            } else {
                w.StartArray();
                w.EndArray();
            }
        } else {
            w.StartArray();
            w.EndArray();
        }
    } else if (rec.status == TaskStatus::kFailed) {
        w.Key("error");
        w.String(rec.error_message.empty() ? "execution failed" : rec.error_message.c_str());
        w.Key("error_code");
        w.String(rec.error_code.c_str());
    }
    w.EndObject();
    rsp = buf.GetString();
    return error::OK;
}

int32_t TaskPlugin::HandleList(const std::string&, const std::string& req, std::string& rsp) {
    rapidjson::Document d;
    d.Parse(req.c_str());
    int page = 1;
    int page_size = 20;
    std::string status_filter;
    if (!d.HasParseError() && d.IsObject()) {
        if (d.HasMember("page") && d["page"].IsInt()) page = d["page"].GetInt();
        if (d.HasMember("page_size") && d["page_size"].IsInt()) page_size = d["page_size"].GetInt();
        if (d.HasMember("status") && d["status"].IsString()) status_filter = d["status"].GetString();
    }
    std::vector<TaskRecord> items;
    int64_t total = 0;
    if (ListTasks(page, page_size, status_filter, &items, &total) != 0) {
        rsp = JsonError("failed to list tasks");
        return error::INTERNAL_ERROR;
    }
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("total");
    w.Int64(total);
    w.Key("items");
    w.StartArray();
    for (const auto& it : items) {
        w.StartObject();
        w.Key("id");
        w.String(it.task_id.c_str());
        w.Key("task_id");
        w.String(it.task_id.c_str());
        w.Key("sql_text");
        w.String(it.request_sql.c_str());
        w.Key("status");
        w.String(StatusName(it.status));
        w.Key("error_code");
        w.String(it.error_code.c_str());
        w.Key("error_message");
        w.String(it.error_message.c_str());
        w.Key("result_row_count");
        w.Int64(it.result_row_count);
        w.Key("result_col_count");
        w.Int64(it.result_col_count);
        w.Key("created_at");
        w.String(it.created_at.c_str());
        w.Key("started_at");
        w.String(it.started_at.c_str());
        w.Key("updated_at");
        w.String(it.updated_at.c_str());
        w.Key("finished_at");
        w.String(it.finished_at.c_str());
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    rsp = buf.GetString();
    return error::OK;
}

int32_t TaskPlugin::HandleDetail(const std::string&, const std::string& req, std::string& rsp) {
    rapidjson::Document d;
    d.Parse(req.c_str());
    if (d.HasParseError() || !d.IsObject() || !d.HasMember("task_id") || !d["task_id"].IsString()) {
        rsp = JsonError("invalid request, expected {\"task_id\":\"...\"}");
        return error::BAD_REQUEST;
    }
    TaskRecord rec;
    if (GetTask(d["task_id"].GetString(), &rec) != 0) {
        rsp = JsonError("task not found");
        return error::NOT_FOUND;
    }
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("task_id");
    w.String(rec.task_id.c_str());
    w.Key("status");
    w.String(StatusName(rec.status));
    w.Key("sql_text");
    w.String(rec.request_sql.c_str());
    w.Key("result_row_count");
    w.Int64(rec.result_row_count);
    w.Key("result_col_count");
    w.Int64(rec.result_col_count);
    w.Key("result_target");
    w.String(rec.result_target.c_str());
    w.Key("error_code");
    w.String(rec.error_code.c_str());
    w.Key("error_message");
    w.String(rec.error_message.c_str());
    w.Key("error_stage");
    w.String(rec.error_stage.c_str());
    w.Key("created_at");
    w.String(rec.created_at.c_str());
    w.Key("started_at");
    w.String(rec.started_at.c_str());
    w.Key("updated_at");
    w.String(rec.updated_at.c_str());
    w.Key("finished_at");
    w.String(rec.finished_at.c_str());
    w.EndObject();
    rsp = buf.GetString();
    return error::OK;
}

int32_t TaskPlugin::HandleDelete(const std::string&, const std::string& req, std::string& rsp) {
    rapidjson::Document d;
    d.Parse(req.c_str());
    if (d.HasParseError() || !d.IsObject() || !d.HasMember("task_id") || !d["task_id"].IsString()) {
        rsp = JsonError("invalid request, expected {\"task_id\":\"...\"}");
        return error::BAD_REQUEST;
    }
    const int rc = DeleteTask(d["task_id"].GetString());
    if (rc == 1) {
        rsp = JsonError("non-terminal task cannot be deleted");
        return error::CONFLICT;
    }
    if (rc != 0) {
        rsp = JsonError("task not found");
        return error::NOT_FOUND;
    }
    rsp = R"({"ok":true})";
    return error::OK;
}

void TaskPlugin::EnumRoutes(std::function<void(const RouteItem&)> callback) {
    callback({"POST", "/tasks/submit",
              [this](const std::string& u, const std::string& req, std::string& rsp) {
                  return HandleSubmit(u, req, rsp);
              }});
    callback({"POST", "/tasks/list",
              [this](const std::string& u, const std::string& req, std::string& rsp) {
                  return HandleList(u, req, rsp);
              }});
    callback({"POST", "/tasks/detail",
              [this](const std::string& u, const std::string& req, std::string& rsp) {
                  return HandleDetail(u, req, rsp);
              }});
    callback({"POST", "/tasks/delete",
              [this](const std::string& u, const std::string& req, std::string& rsp) {
                  return HandleDelete(u, req, rsp);
              }});
}

}  // namespace task
}  // namespace flowsql
