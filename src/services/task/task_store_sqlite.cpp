// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "task_store_sqlite.h"

#include <sqlite3.h>

#include <filesystem>
#include <set>
#include <utility>

namespace fs = std::filesystem;

namespace flowsql {
namespace task {

namespace {

static const char* kSchemaSql =
    "CREATE TABLE IF NOT EXISTS tasks ("
    "task_id TEXT PRIMARY KEY,"
    "request_sql TEXT NOT NULL,"
    "sqls_json TEXT NOT NULL DEFAULT '',"
    "sql_count INTEGER NOT NULL DEFAULT 1,"
    "current_sql_index INTEGER NOT NULL DEFAULT 0,"
    "timeout_s INTEGER NOT NULL DEFAULT 0,"
    "cancel_requested INTEGER NOT NULL DEFAULT 0,"
    "task_kind TEXT NOT NULL DEFAULT 'batch',"
    "runtime_task_id TEXT NOT NULL DEFAULT '',"
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
    "CREATE TABLE IF NOT EXISTS task_sql_payloads ("
    "task_id TEXT PRIMARY KEY,"
    "raw_sql_text TEXT NOT NULL,"
    "sqls_json TEXT NOT NULL,"
    "sql_count INTEGER NOT NULL,"
    "created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"
    "FOREIGN KEY(task_id) REFERENCES tasks(task_id) ON DELETE CASCADE"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_task_sql_payloads_created_at ON task_sql_payloads(created_at);"
    "CREATE TABLE IF NOT EXISTS task_result_summaries ("
    "task_id TEXT PRIMARY KEY,"
    "result_json TEXT NOT NULL,"
    "FOREIGN KEY(task_id) REFERENCES tasks(task_id) ON DELETE CASCADE"
    ");"
    "CREATE TABLE IF NOT EXISTS task_events ("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "task_id TEXT NOT NULL,"
    "from_status TEXT,"
    "to_status TEXT NOT NULL,"
    "event_time DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"
    "message TEXT"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_task_events_task_id ON task_events(task_id);"
    "CREATE TABLE IF NOT EXISTS task_diagnostics ("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "task_id TEXT NOT NULL,"
    "sql_index INTEGER NOT NULL,"
    "sql_text TEXT NOT NULL,"
    "duration_ms INTEGER NOT NULL DEFAULT 0,"
    "source_rows INTEGER NOT NULL DEFAULT 0,"
    "sink_rows INTEGER NOT NULL DEFAULT 0,"
    "operator_chain TEXT NOT NULL DEFAULT '',"
    "created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_task_diag_task_id ON task_diagnostics(task_id, sql_index);";

}  // namespace

const char* TaskStoreSqlite::StatusName(TaskStatus s) {
    switch (s) {
        case TaskStatus::kPending: return "pending";
        case TaskStatus::kRunning: return "running";
        case TaskStatus::kCompleted: return "completed";
        case TaskStatus::kFailed: return "failed";
        case TaskStatus::kStopped: return "stopped";
        case TaskStatus::kCancelled: return "cancelled";
        case TaskStatus::kTimeout: return "timeout";
        default: return "failed";
    }
}

TaskStatus TaskStoreSqlite::ParseStatus(const std::string& s) {
    if (s == "pending") return TaskStatus::kPending;
    if (s == "running") return TaskStatus::kRunning;
    if (s == "completed") return TaskStatus::kCompleted;
    if (s == "failed") return TaskStatus::kFailed;
    if (s == "stopped") return TaskStatus::kStopped;
    if (s == "cancelled") return TaskStatus::kCancelled;
    if (s == "timeout") return TaskStatus::kTimeout;
    return TaskStatus::kFailed;
}

bool TaskStoreSqlite::IsTerminal(TaskStatus s) {
    return s == TaskStatus::kCompleted ||
           s == TaskStatus::kFailed ||
           s == TaskStatus::kStopped ||
           s == TaskStatus::kCancelled ||
           s == TaskStatus::kTimeout;
}

std::string TaskStoreSqlite::BuildDbPathNoLock() const {
    if (!db_path_.empty()) return db_path_;
    fs::path p(db_dir_);
    p /= "task_store.db";
    return p.string();
}

int TaskStoreSqlite::EnsureSchemaNoLock() {
    if (!db_) return -1;
    if (sqlite3_exec(db_, kSchemaSql, nullptr, nullptr, nullptr) != SQLITE_OK) return -1;
    return 0;
}

int TaskStoreSqlite::Open(const OpenOptions& options) {
    std::lock_guard<std::mutex> lock(db_mu_);
    if (db_) return 0;

    db_dir_ = options.db_dir.empty() ? "./taskdb" : options.db_dir;
    db_path_ = options.db_path;
    retention_days_ = options.retention_days;
    retention_max_count_ = options.retention_max_count;

    std::error_code ec;
    const std::string db_path = BuildDbPathNoLock();
    fs::path parent = fs::path(db_path).parent_path();
    if (!parent.empty()) {
        fs::create_directories(parent, ec);
        if (ec) return -1;
    } else if (db_path_.empty()) {
        fs::create_directories(db_dir_, ec);
        if (ec) return -1;
    }

    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    if (sqlite3_open_v2(db_path.c_str(), &db_, flags, nullptr) != SQLITE_OK) {
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        return -1;
    }
    (void)sqlite3_exec(db_, "PRAGMA foreign_keys=ON", nullptr, nullptr, nullptr);
    (void)sqlite3_exec(db_, "PRAGMA journal_mode=WAL", nullptr, nullptr, nullptr);
    return EnsureSchemaNoLock();
}

int TaskStoreSqlite::Close() {
    std::lock_guard<std::mutex> lock(db_mu_);
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
    return 0;
}

int TaskStoreSqlite::SaveResultSummary(const std::string& task_id, const std::string& result_json) {
    std::lock_guard<std::mutex> lock(db_mu_);
    if (!db_) return -1;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO task_result_summaries(task_id,result_json) VALUES(?1,?2) "
        "ON CONFLICT(task_id) DO UPDATE SET result_json=excluded.result_json;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, task_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, result_json.c_str(), static_cast<int>(result_json.size()), SQLITE_TRANSIENT);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

int TaskStoreSqlite::GetResultSummary(const std::string& task_id, std::string* result_json) {
    std::lock_guard<std::mutex> lock(db_mu_);
    if (!db_ || !result_json) return -1;
    result_json->clear();
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT result_json FROM task_result_summaries WHERE task_id=?1;", -1, &stmt,
                           nullptr) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(stmt, 1, task_id.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        const auto* value = sqlite3_column_text(stmt, 0);
        result_json->assign(reinterpret_cast<const char*>(value), sqlite3_column_bytes(stmt, 0));
    }
    sqlite3_finalize(stmt);
    return rc == SQLITE_ROW || rc == SQLITE_DONE ? 0 : -1;
}

int TaskStoreSqlite::WriteTaskEventNoLock(const std::string& task_id,
                                          const std::string& from_status,
                                          const std::string& to_status,
                                          const std::string& message) {
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

int TaskStoreSqlite::WriteTaskEvent(const std::string& task_id,
                                    const std::string& from_status,
                                    const std::string& to_status,
                                    const std::string& message) {
    std::lock_guard<std::mutex> lock(db_mu_);
    return WriteTaskEventNoLock(task_id, from_status, to_status, message);
}

int TaskStoreSqlite::CleanupOrphans() {
    std::lock_guard<std::mutex> lock(db_mu_);
    if (!db_) return -1;
    sqlite3_stmt* sel = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT task_id, status FROM tasks WHERE status IN ('pending','running');",
                           -1, &sel, nullptr) != SQLITE_OK) return -1;
    std::vector<std::pair<std::string, std::string>> orphans;
    while (sqlite3_step(sel) == SQLITE_ROW) {
        const unsigned char* id = sqlite3_column_text(sel, 0);
        const unsigned char* st = sqlite3_column_text(sel, 1);
        if (id && st) {
            orphans.emplace_back(reinterpret_cast<const char*>(id), reinterpret_cast<const char*>(st));
        }
    }
    sqlite3_finalize(sel);
    if (orphans.empty()) return 0;

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

    for (const auto& item : orphans) {
        WriteTaskEventNoLock(item.first, item.second, "failed", "PROCESS_RESTART");
    }
    return 0;
}

int TaskStoreSqlite::QueryLastTaskId(std::string* task_id) {
    if (!task_id) return -1;
    task_id->clear();
    std::lock_guard<std::mutex> lock(db_mu_);
    if (!db_) return -1;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT task_id FROM tasks ORDER BY task_id DESC LIMIT 1;", -1, &stmt, nullptr) !=
        SQLITE_OK) {
        return -1;
    }
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* txt = sqlite3_column_text(stmt, 0);
        if (txt) *task_id = reinterpret_cast<const char*>(txt);
    }
    sqlite3_finalize(stmt);
    return 0;
}

int TaskStoreSqlite::CreateTask(const TaskCreateParams& params) {
    if (params.task_id.empty() || params.request_sql.empty() ||
        params.raw_sql_text.empty() || params.sqls_json.empty() || params.sql_count <= 0) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(db_mu_);
    if (!db_) return -1;

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE TRANSACTION;", nullptr, nullptr, nullptr) != SQLITE_OK) return -1;

    bool ok = true;
    sqlite3_stmt* stmt = nullptr;
    const char* task_sql =
        "INSERT INTO tasks(task_id, request_sql, current_sql_index, timeout_s, task_kind, runtime_task_id, status) "
        "VALUES(?1, ?2, 0, ?3, ?4, ?5, 'pending');";
    if (sqlite3_prepare_v2(db_, task_sql, -1, &stmt, nullptr) != SQLITE_OK) {
        ok = false;
    } else {
        sqlite3_bind_text(stmt, 1, params.task_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, params.request_sql.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, params.timeout_s > 0 ? params.timeout_s : 0);
        sqlite3_bind_text(stmt, 4, params.task_kind.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, params.runtime_task_id.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) != SQLITE_DONE) ok = false;
        sqlite3_finalize(stmt);
        stmt = nullptr;
    }

    if (ok) {
        const char* payload_sql =
            "INSERT INTO task_sql_payloads(task_id, raw_sql_text, sqls_json, sql_count) "
            "VALUES(?1, ?2, ?3, ?4);";
        if (sqlite3_prepare_v2(db_, payload_sql, -1, &stmt, nullptr) != SQLITE_OK) {
            ok = false;
        } else {
            sqlite3_bind_text(stmt, 1, params.task_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, params.raw_sql_text.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, params.sqls_json.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 4, params.sql_count);
            if (sqlite3_step(stmt) != SQLITE_DONE) ok = false;
            sqlite3_finalize(stmt);
            stmt = nullptr;
        }
    }

    if (!ok) {
        if (stmt) sqlite3_finalize(stmt);
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return -1;
    }
    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return -1;
    }
    return 0;
}

int TaskStoreSqlite::UpdateStatus(const TaskStatusUpdate& update) {
    if (update.task_id.empty()) return -1;
    std::lock_guard<std::mutex> lock(db_mu_);
    if (!db_) return -1;

    std::string cur_status_str;
    sqlite3_stmt* sel = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT status FROM tasks WHERE task_id=?1;", -1, &sel, nullptr) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(sel, 1, update.task_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(sel) == SQLITE_ROW) {
        const unsigned char* v = sqlite3_column_text(sel, 0);
        if (v) cur_status_str = reinterpret_cast<const char*>(v);
    }
    sqlite3_finalize(sel);
    if (cur_status_str.empty()) return -1;
    if (IsTerminal(ParseStatus(cur_status_str))) return 1;

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "UPDATE tasks SET status=?1, error_code=?2, error_message=?3, error_stage=?4, "
        "result_row_count=?5, result_col_count=?6, result_target=?7, updated_at=CURRENT_TIMESTAMP, "
        "cancel_requested=CASE WHEN ?9=1 THEN 0 ELSE cancel_requested END, "
        "started_at=CASE WHEN ?8=1 AND started_at IS NULL THEN CURRENT_TIMESTAMP ELSE started_at END, "
        "finished_at=CASE WHEN ?9=1 THEN CURRENT_TIMESTAMP ELSE finished_at END "
        "WHERE task_id=?10 AND status NOT IN ('completed','failed','stopped','cancelled','timeout');";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, StatusName(update.new_status), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, update.error_code.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, update.error_message.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, update.error_stage.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 5, update.result_row_count);
    sqlite3_bind_int64(stmt, 6, update.result_col_count);
    sqlite3_bind_text(stmt, 7, update.result_target.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 8, (update.new_status == TaskStatus::kRunning) ? 1 : 0);
    sqlite3_bind_int(stmt, 9, IsTerminal(update.new_status) ? 1 : 0);
    sqlite3_bind_text(stmt, 10, update.task_id.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(stmt);
    const int changed = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE || changed <= 0) return -1;

    WriteTaskEventNoLock(update.task_id, cur_status_str, StatusName(update.new_status), update.error_code);
    return 0;
}

int TaskStoreSqlite::GetTask(const std::string& task_id, TaskRecord* out) {
    if (task_id.empty() || !out) return -1;
    std::lock_guard<std::mutex> lock(db_mu_);
    if (!db_) return -1;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT task_id, request_sql, IFNULL(task_kind,'batch'), IFNULL(runtime_task_id,''), status, "
        "error_code, error_message, error_stage, result_row_count, result_col_count, result_target, "
        "created_at, IFNULL(started_at,''), updated_at, IFNULL(finished_at,'') "
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
    out->task_kind = txt(2);
    out->runtime_task_id = txt(3);
    out->status = ParseStatus(txt(4));
    out->error_code = txt(5);
    out->error_message = txt(6);
    out->error_stage = txt(7);
    out->result_row_count = sqlite3_column_int64(stmt, 8);
    out->result_col_count = sqlite3_column_int64(stmt, 9);
    out->result_target = txt(10);
    out->created_at = txt(11);
    out->started_at = txt(12);
    out->updated_at = txt(13);
    out->finished_at = txt(14);
    sqlite3_finalize(stmt);
    return 0;
}

int TaskStoreSqlite::ListTasks(int page,
                               int page_size,
                               const std::string& status_filter,
                               std::vector<TaskRecord>* items,
                               int64_t* total) {
    if (!items || !total) return -1;
    std::lock_guard<std::mutex> lock(db_mu_);
    if (!db_) return -1;

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
        "SELECT task_id, request_sql, IFNULL(task_kind,'batch'), IFNULL(runtime_task_id,''), status, "
        "error_code, error_message, error_stage, result_row_count, result_col_count, result_target, "
        "created_at, IFNULL(started_at,''), updated_at, IFNULL(finished_at,'') "
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
        r.task_kind = txt(2);
        r.runtime_task_id = txt(3);
        r.status = ParseStatus(txt(4));
        r.error_code = txt(5);
        r.error_message = txt(6);
        r.error_stage = txt(7);
        r.result_row_count = sqlite3_column_int64(stmt, 8);
        r.result_col_count = sqlite3_column_int64(stmt, 9);
        r.result_target = txt(10);
        r.created_at = txt(11);
        r.started_at = txt(12);
        r.updated_at = txt(13);
        r.finished_at = txt(14);
        items->push_back(std::move(r));
    }
    sqlite3_finalize(stmt);
    return 0;
}

int TaskStoreSqlite::ListTasksByKind(const std::string& task_kind,
                                     int page,
                                     int page_size,
                                     const std::string& status_filter,
                                     std::vector<TaskRecord>* items,
                                     int64_t* total) {
    if (!items || !total || task_kind.empty()) return -1;
    std::lock_guard<std::mutex> lock(db_mu_);
    if (!db_) return -1;

    if (page < 1) page = 1;
    if (page_size < 1) page_size = 20;
    if (page_size > 100) page_size = 100;
    items->clear();
    *total = 0;

    const bool has_status = !status_filter.empty();
    const std::string where = has_status
        ? " WHERE task_kind=?1 AND status=?2"
        : " WHERE task_kind=?1";

    sqlite3_stmt* cnt = nullptr;
    std::string cnt_sql = "SELECT COUNT(1) FROM tasks" + where + ";";
    if (sqlite3_prepare_v2(db_, cnt_sql.c_str(), -1, &cnt, nullptr) != SQLITE_OK) return -1;
    sqlite3_bind_text(cnt, 1, task_kind.c_str(), -1, SQLITE_TRANSIENT);
    if (has_status) sqlite3_bind_text(cnt, 2, status_filter.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(cnt) == SQLITE_ROW) *total = sqlite3_column_int64(cnt, 0);
    sqlite3_finalize(cnt);

    sqlite3_stmt* stmt = nullptr;
    std::string sql =
        "SELECT task_id, request_sql, IFNULL(task_kind,'batch'), IFNULL(runtime_task_id,''), status, "
        "error_code, error_message, error_stage, result_row_count, result_col_count, result_target, "
        "created_at, IFNULL(started_at,''), updated_at, IFNULL(finished_at,'') "
        "FROM tasks" + where + " ORDER BY created_at DESC LIMIT ?3 OFFSET ?4;";
    if (!has_status) {
        sql =
            "SELECT task_id, request_sql, IFNULL(task_kind,'batch'), IFNULL(runtime_task_id,''), status, "
            "error_code, error_message, error_stage, result_row_count, result_col_count, result_target, "
            "created_at, IFNULL(started_at,''), updated_at, IFNULL(finished_at,'') "
            "FROM tasks" + where + " ORDER BY created_at DESC LIMIT ?2 OFFSET ?3;";
    }
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, task_kind.c_str(), -1, SQLITE_TRANSIENT);
    int bind_idx = 2;
    if (has_status) sqlite3_bind_text(stmt, bind_idx++, status_filter.c_str(), -1, SQLITE_TRANSIENT);
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
        r.task_kind = txt(2);
        r.runtime_task_id = txt(3);
        r.status = ParseStatus(txt(4));
        r.error_code = txt(5);
        r.error_message = txt(6);
        r.error_stage = txt(7);
        r.result_row_count = sqlite3_column_int64(stmt, 8);
        r.result_col_count = sqlite3_column_int64(stmt, 9);
        r.result_target = txt(10);
        r.created_at = txt(11);
        r.started_at = txt(12);
        r.updated_at = txt(13);
        r.finished_at = txt(14);
        items->push_back(std::move(r));
    }
    sqlite3_finalize(stmt);
    return 0;
}

int TaskStoreSqlite::DeleteTaskNoLock(const std::string& task_id) {
    if (!db_ || task_id.empty()) return -1;

    sqlite3_stmt* sel = nullptr;
    std::string status;
    if (sqlite3_prepare_v2(db_, "SELECT status FROM tasks WHERE task_id=?1;", -1, &sel, nullptr) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(sel, 1, task_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(sel) == SQLITE_ROW) {
        const unsigned char* v = sqlite3_column_text(sel, 0);
        if (v) status = reinterpret_cast<const char*>(v);
    }
    sqlite3_finalize(sel);
    if (status.empty()) return -1;
    if (!IsTerminal(ParseStatus(status))) return 1;

    if (sqlite3_exec(db_, "BEGIN IMMEDIATE TRANSACTION;", nullptr, nullptr, nullptr) != SQLITE_OK) return -1;

    bool ok = true;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM task_events WHERE task_id=?1;", -1, &stmt, nullptr) != SQLITE_OK) {
        ok = false;
    } else {
        sqlite3_bind_text(stmt, 1, task_id.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) != SQLITE_DONE) ok = false;
        sqlite3_finalize(stmt);
        stmt = nullptr;
    }

    int changed = 0;
    if (ok) {
        if (sqlite3_prepare_v2(db_, "DELETE FROM task_diagnostics WHERE task_id=?1;", -1, &stmt, nullptr) !=
            SQLITE_OK) {
            ok = false;
        } else {
            sqlite3_bind_text(stmt, 1, task_id.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt) != SQLITE_DONE) ok = false;
            sqlite3_finalize(stmt);
            stmt = nullptr;
        }
    }

    if (ok) {
        if (sqlite3_prepare_v2(db_, "DELETE FROM task_sql_payloads WHERE task_id=?1;", -1, &stmt, nullptr) !=
            SQLITE_OK) {
            ok = false;
        } else {
            sqlite3_bind_text(stmt, 1, task_id.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt) != SQLITE_DONE) ok = false;
            sqlite3_finalize(stmt);
            stmt = nullptr;
        }
    }

    if (ok) {
        if (sqlite3_prepare_v2(db_, "DELETE FROM tasks WHERE task_id=?1;", -1, &stmt, nullptr) != SQLITE_OK) {
            ok = false;
        } else {
            sqlite3_bind_text(stmt, 1, task_id.c_str(), -1, SQLITE_TRANSIENT);
            const int rc = sqlite3_step(stmt);
            changed = sqlite3_changes(db_);
            if (rc != SQLITE_DONE || changed <= 0) ok = false;
            sqlite3_finalize(stmt);
            stmt = nullptr;
        }
    }

    if (!ok) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return -1;
    }
    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return -1;
    }
    return 0;
}

int TaskStoreSqlite::DeleteTask(const std::string& task_id) {
    std::lock_guard<std::mutex> lock(db_mu_);
    return DeleteTaskNoLock(task_id);
}

int TaskStoreSqlite::RunRetentionCleanupNoLock() {
    if (!db_) return -1;
    if (retention_days_ <= 0 && retention_max_count_ <= 0) return 0;

    std::set<std::string> to_delete;
    auto add_ids = [&to_delete](sqlite3* db, const char* sql, int int_param) {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
        if (int_param >= 0) sqlite3_bind_int(stmt, 1, int_param);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char* txt = sqlite3_column_text(stmt, 0);
            if (txt) to_delete.insert(reinterpret_cast<const char*>(txt));
        }
        sqlite3_finalize(stmt);
    };

    if (retention_days_ > 0) {
        const char* sql_days =
            "SELECT task_id FROM tasks "
            "WHERE status IN ('completed','failed','stopped','cancelled','timeout') "
            "AND created_at < datetime('now', '-' || ?1 || ' days');";
        add_ids(db_, sql_days, retention_days_);
    }
    if (retention_max_count_ > 0) {
        const char* sql_count =
            "SELECT task_id FROM tasks "
            "WHERE status IN ('completed','failed','stopped','cancelled','timeout') "
            "ORDER BY datetime(created_at) DESC, task_id DESC "
            "LIMIT -1 OFFSET ?1;";
        add_ids(db_, sql_count, retention_max_count_);
    }

    static constexpr size_t kCleanupBatchLimit = 128;
    size_t cleaned = 0;
    for (const auto& id : to_delete) {
        if (cleaned >= kCleanupBatchLimit) break;
        if (DeleteTaskNoLock(id) == 0) ++cleaned;
    }
    return 0;
}

int TaskStoreSqlite::RunRetentionCleanup() {
    std::lock_guard<std::mutex> lock(db_mu_);
    return RunRetentionCleanupNoLock();
}

int TaskStoreSqlite::WriteDiagnostic(const std::string& task_id,
                                     int sql_index,
                                     const std::string& sql_text,
                                     int64_t duration_ms,
                                     int64_t source_rows,
                                     int64_t sink_rows,
                                     const std::string& operator_chain) {
    std::lock_guard<std::mutex> lock(db_mu_);
    if (!db_ || task_id.empty()) return -1;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO task_diagnostics(task_id, sql_index, sql_text, duration_ms, source_rows, sink_rows, operator_chain) "
        "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7);";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, task_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, sql_index);
    sqlite3_bind_text(stmt, 3, sql_text.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, duration_ms);
    sqlite3_bind_int64(stmt, 5, source_rows);
    sqlite3_bind_int64(stmt, 6, sink_rows);
    sqlite3_bind_text(stmt, 7, operator_chain.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

int TaskStoreSqlite::UpdateRuntimeTaskId(const std::string& task_id, const std::string& runtime_task_id) {
    std::lock_guard<std::mutex> lock(db_mu_);
    if (!db_ || task_id.empty()) return -1;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "UPDATE tasks SET runtime_task_id=?1, updated_at=CURRENT_TIMESTAMP WHERE task_id=?2;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, runtime_task_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, task_id.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(stmt);
    const int changed = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE && changed > 0) ? 0 : -1;
}

int TaskStoreSqlite::UpdateTaskKindAndRuntimeId(const std::string& task_id,
                                                const std::string& task_kind,
                                                const std::string& runtime_task_id) {
    std::lock_guard<std::mutex> lock(db_mu_);
    if (!db_ || task_id.empty()) return -1;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "UPDATE tasks SET task_kind=?1, runtime_task_id=?2, updated_at=CURRENT_TIMESTAMP WHERE task_id=?3;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, task_kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, runtime_task_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, task_id.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(stmt);
    const int changed = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE && changed > 0) ? 0 : -1;
}

int TaskStoreSqlite::QueryTaskSqlPayload(const std::string& task_id,
                                         std::string* raw_sql_text,
                                         std::string* sqls_json,
                                         int* sql_count) {
    if (task_id.empty() || !raw_sql_text || !sqls_json) return -1;
    std::lock_guard<std::mutex> lock(db_mu_);
    if (!db_) return -1;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT raw_sql_text, sqls_json, sql_count "
        "FROM task_sql_payloads WHERE task_id=?1;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, task_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return -1;
    }
    const unsigned char* raw = sqlite3_column_text(stmt, 0);
    const unsigned char* sqls = sqlite3_column_text(stmt, 1);
    if (!raw || !sqls) {
        sqlite3_finalize(stmt);
        return -1;
    }
    *raw_sql_text = reinterpret_cast<const char*>(raw);
    *sqls_json = reinterpret_cast<const char*>(sqls);
    if (sql_count) *sql_count = sqlite3_column_int(stmt, 2);
    sqlite3_finalize(stmt);
    return 0;
}

int TaskStoreSqlite::UpdateCurrentSqlIndex(const std::string& task_id, int index) {
    if (task_id.empty()) return -1;
    std::lock_guard<std::mutex> lock(db_mu_);
    if (!db_) return -1;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE tasks SET current_sql_index=?1, updated_at=CURRENT_TIMESTAMP WHERE task_id=?2;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return -1;
    sqlite3_bind_int(stmt, 1, index);
    sqlite3_bind_text(stmt, 2, task_id.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

int TaskStoreSqlite::QueryTaskRuntimeFlags(const std::string& task_id, bool* terminal_now, bool* cancel_requested) {
    if (task_id.empty() || !terminal_now || !cancel_requested) return -1;
    *terminal_now = false;
    *cancel_requested = false;
    std::lock_guard<std::mutex> lock(db_mu_);
    if (!db_) return -1;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT status, cancel_requested FROM tasks WHERE task_id=?1;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, task_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* status_txt = sqlite3_column_text(stmt, 0);
        const std::string status = status_txt ? reinterpret_cast<const char*>(status_txt) : "";
        *terminal_now = IsTerminal(ParseStatus(status));
        *cancel_requested = sqlite3_column_int(stmt, 1) == 1;
    }
    sqlite3_finalize(stmt);
    return 0;
}

int TaskStoreSqlite::RequestCancelRunningTask(const std::string& task_id) {
    if (task_id.empty()) return -1;
    std::lock_guard<std::mutex> lock(db_mu_);
    if (!db_) return -1;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "UPDATE tasks SET cancel_requested=1, updated_at=CURRENT_TIMESTAMP "
        "WHERE task_id=?1 AND status='running';";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, task_id.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(stmt);
    const int changed = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -1;
    return changed > 0 ? 0 : 1;
}

int TaskStoreSqlite::ListTimedOutTaskIds(std::vector<std::string>* timed_out_ids) {
    if (!timed_out_ids) return -1;
    timed_out_ids->clear();
    std::lock_guard<std::mutex> lock(db_mu_);
    if (!db_) return -1;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT task_id FROM tasks "
        "WHERE status='running' AND timeout_s > 0 AND started_at IS NOT NULL "
        "AND (strftime('%s','now') - strftime('%s', started_at)) >= timeout_s;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return -1;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* txt = sqlite3_column_text(stmt, 0);
        if (txt) timed_out_ids->emplace_back(reinterpret_cast<const char*>(txt));
    }
    sqlite3_finalize(stmt);
    return 0;
}

int TaskStoreSqlite::ListDiagnostics(const std::string& task_id, std::vector<TaskDiagnostic>* items) {
    if (task_id.empty() || !items) return -1;
    items->clear();
    std::lock_guard<std::mutex> lock(db_mu_);
    if (!db_) return -1;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT sql_index, sql_text, duration_ms, source_rows, sink_rows, operator_chain, created_at "
        "FROM task_diagnostics WHERE task_id=?1 ORDER BY sql_index ASC;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, task_id.c_str(), -1, SQLITE_TRANSIENT);

    auto txt = [stmt](int idx) -> std::string {
        const unsigned char* v = sqlite3_column_text(stmt, idx);
        return v ? reinterpret_cast<const char*>(v) : "";
    };
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        TaskDiagnostic item;
        item.sql_index = sqlite3_column_int(stmt, 0);
        item.sql_text = txt(1);
        item.duration_ms = sqlite3_column_int64(stmt, 2);
        item.source_rows = sqlite3_column_int64(stmt, 3);
        item.sink_rows = sqlite3_column_int64(stmt, 4);
        item.operator_chain = txt(5);
        item.created_at = txt(6);
        items->push_back(std::move(item));
    }
    sqlite3_finalize(stmt);
    return 0;
}

}  // namespace task
}  // namespace flowsql
