// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <cassert>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <thread>
#include <unistd.h>

#include <common/error_code.h>
#include <common/iquerier.hpp>
#include <framework/core/scheduler_control_client.h>
#include <framework/interfaces/ichannel_registry.h>
#include <framework/interfaces/irouter_handle.h>
#include <framework/interfaces/ischeduler_control_service.h>
#include <services/task/task_plugin.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <sqlite3.h>

using namespace flowsql;
using namespace flowsql::task;

#define ASSERT_TRUE(expr)                                                                   \
    do {                                                                                    \
        if (!(expr)) {                                                                      \
            std::printf("[FAIL] %s:%d %s\n", __FILE__, __LINE__, #expr);                   \
            std::fflush(stdout);                                                            \
            assert(false);                                                                  \
        }                                                                                   \
    } while (0)

#define ASSERT_EQ(a, b)                                                                     \
    do {                                                                                    \
        auto _a = (a);                                                                      \
        auto _b = (b);                                                                      \
        if (!(_a == _b)) {                                                                  \
            std::printf("[FAIL] %s:%d %s != %s\n", __FILE__, __LINE__, #a, #b);            \
            std::fflush(stdout);                                                            \
            assert(false);                                                                  \
        }                                                                                   \
    } while (0)

static std::string MakeTempDir(const char* suffix) {
    std::filesystem::path p = std::filesystem::temp_directory_path() /
                              ("flowsql_task_test_" + std::string(suffix) + "_" + std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    std::filesystem::create_directories(p, ec);
    return p.string();
}

static std::string MakeTaskIdReq(const std::string& task_id) {
    return std::string("{\"task_id\":\"") + task_id + "\"}";
}

static RouteItem MakeSqlClassifyRoute() {
    return {"POST", "/scheduler/sql/classify",
            [](const std::string&, const std::string& req, std::string& rsp) {
                rapidjson::Document d;
                d.Parse(req.c_str());
                if (d.HasParseError() || !d.IsObject() || !d.HasMember("sql") || !d["sql"].IsString()) {
                    rsp = R"({"error":"invalid request, expected {\"sql\":\"...\"}"})";
                    return error::BAD_REQUEST;
                }
                const std::string sql = d["sql"].GetString();
                const bool is_stream = (sql.find("_stream") != std::string::npos) ||
                                       (sql.find("tcp_session_mock.") != std::string::npos) ||
                                       (sql.find("stream.") != std::string::npos);
                if (sql.find("netadapter.") != std::string::npos) {
                    rsp = R"({"task_kind":"batch","requires_async":true})";
                } else {
                    rsp = is_stream ? R"({"task_kind":"stream"})" : R"({"task_kind":"batch"})";
                }
                return error::OK;
            }};
}

static int64_t CountTaskEvents(const std::string& db_path, const std::string& task_id) {
    sqlite3* db = nullptr;
    if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return -1;
    }
    sqlite3_stmt* stmt = nullptr;
    int64_t count = -1;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(1) FROM task_events WHERE task_id=?1;", -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, task_id.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int64(stmt, 0);
    }
    if (stmt) sqlite3_finalize(stmt);
    sqlite3_close(db);
    return count;
}

static int64_t CountTasks(const std::string& db_path) {
    sqlite3* db = nullptr;
    if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return -1;
    }
    sqlite3_stmt* stmt = nullptr;
    int64_t count = -1;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(1) FROM tasks;", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int64(stmt, 0);
    }
    if (stmt) sqlite3_finalize(stmt);
    sqlite3_close(db);
    return count;
}

static bool TaskExists(const std::string& db_path, const std::string& task_id) {
    sqlite3* db = nullptr;
    if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return false;
    }
    sqlite3_stmt* stmt = nullptr;
    bool exists = false;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM tasks WHERE task_id=?1 LIMIT 1;", -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, task_id.c_str(), -1, SQLITE_TRANSIENT);
        exists = (sqlite3_step(stmt) == SQLITE_ROW);
    }
    if (stmt) sqlite3_finalize(stmt);
    sqlite3_close(db);
    return exists;
}

static bool UpdateTaskCreatedAt(const std::string& db_path, const std::string& task_id, const std::string& created_at) {
    sqlite3* db = nullptr;
    if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return false;
    }
    sqlite3_stmt* stmt = nullptr;
    bool ok = false;
    if (sqlite3_prepare_v2(db, "UPDATE tasks SET created_at=?1 WHERE task_id=?2;", -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, created_at.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, task_id.c_str(), -1, SQLITE_TRANSIENT);
        ok = (sqlite3_step(stmt) == SQLITE_DONE);
    }
    if (stmt) sqlite3_finalize(stmt);
    sqlite3_close(db);
    return ok;
}

static bool DeleteTaskSqlPayload(const std::string& db_path, const std::string& task_id) {
    sqlite3* db = nullptr;
    if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return false;
    }
    sqlite3_stmt* stmt = nullptr;
    bool ok = false;
    if (sqlite3_prepare_v2(db, "DELETE FROM task_sql_payloads WHERE task_id=?1;", -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, task_id.c_str(), -1, SQLITE_TRANSIENT);
        ok = (sqlite3_step(stmt) == SQLITE_DONE);
    }
    if (stmt) sqlite3_finalize(stmt);
    sqlite3_close(db);
    return ok;
}

template <class Fn>
static bool WaitUntil(Fn&& pred, int timeout_ms, int interval_ms = 50) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
    }
    return pred();
}

static std::unordered_map<std::string, fnRouterHandler> CollectRoutes(IRouterHandle* router) {
    std::unordered_map<std::string, fnRouterHandler> routes;
    if (!router) return routes;
    router->EnumRoutes([&](const RouteItem& item) {
        routes[item.method + ":" + item.uri] = item.handler;
    });
    return routes;
}

class MockRouterHandle : public IRouterHandle {
 public:
    explicit MockRouterHandle(std::vector<RouteItem> items) : items_(std::move(items)) {
        InstallBatchCompatRuntimeRoutes();
    }
    void EnumRoutes(std::function<void(const RouteItem&)> cb) override {
        for (auto& item : items_) cb(item);
    }

 private:
    struct BatchCompatTask {
        std::string runtime_task_id;
        std::vector<std::string> sqls;
        int timeout_s = 0;
        std::string status = "pending";
        std::string error_code;
        std::string error_message;
        std::string error_stage;
        int current_sql_index = 0;
        int sql_count = 0;
        int64_t result_row_count = 0;
        int64_t result_col_count = 0;
        std::string result_target;
        int64_t created_ms = 0;
        int64_t started_ms = 0;
        int64_t last_active_ms = 0;
        int64_t finished_ms = 0;
        bool stop_requested = false;
    };

    struct BatchCompatRuntimeState {
        std::mutex mu;
        std::unordered_map<std::string, BatchCompatTask> tasks;
        fnRouterHandler execute_handler;
        std::atomic<uint64_t> seq{0};
    };

    static int64_t NowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    static void ParseExecuteResult(const std::string& rsp,
                                   int64_t* rows,
                                   int64_t* cols,
                                   std::string* result_target) {
        if (rows) *rows = 0;
        if (cols) *cols = 0;
        if (result_target) result_target->clear();
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

    static void ParseExecuteError(const std::string& rsp,
                                  std::string* error_code,
                                  std::string* error_message,
                                  std::string* error_stage) {
        if (error_code) error_code->clear();
        if (error_message) error_message->clear();
        if (error_stage) error_stage->clear();
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

    static std::string BuildBatchStatusJson(const BatchCompatTask& t) {
        rapidjson::StringBuffer out;
        rapidjson::Writer<rapidjson::StringBuffer> w(out);
        w.StartObject();
        w.Key("runtime_task_id");
        w.String(t.runtime_task_id.c_str());
        w.Key("runtime_kind");
        w.String("batch");
        w.Key("status");
        w.String(t.status.c_str());
        w.Key("error_code");
        w.String(t.error_code.c_str());
        w.Key("error_message");
        w.String(t.error_message.c_str());
        w.Key("error_stage");
        w.String(t.error_stage.c_str());
        w.Key("current_sql_index");
        w.Int(t.current_sql_index);
        w.Key("sql_count");
        w.Int(t.sql_count);
        w.Key("timeout_s");
        w.Int(t.timeout_s);
        w.Key("result_row_count");
        w.Int64(t.result_row_count);
        w.Key("result_col_count");
        w.Int64(t.result_col_count);
        w.Key("result_target");
        w.String(t.result_target.c_str());
        w.Key("created_ms");
        w.Int64(t.created_ms);
        w.Key("started_ms");
        w.Int64(t.started_ms);
        w.Key("last_active_ms");
        w.Int64(t.last_active_ms);
        w.Key("finished_ms");
        w.Int64(t.finished_ms);
        w.EndObject();
        return out.GetString();
    }

    static void RunBatchCompatTask(const std::shared_ptr<BatchCompatRuntimeState>& state,
                                   const std::string& runtime_task_id) {
        std::vector<std::string> sqls;
        int timeout_s = 0;
        int64_t started_ms = 0;
        {
            std::lock_guard<std::mutex> lock(state->mu);
            auto it = state->tasks.find(runtime_task_id);
            if (it == state->tasks.end()) return;
            it->second.status = "running";
            it->second.started_ms = NowMs();
            it->second.last_active_ms = it->second.started_ms;
            sqls = it->second.sqls;
            timeout_s = it->second.timeout_s;
            started_ms = it->second.started_ms;
        }

        for (size_t i = 0; i < sqls.size(); ++i) {
            {
                std::lock_guard<std::mutex> lock(state->mu);
                auto it = state->tasks.find(runtime_task_id);
                if (it == state->tasks.end()) return;
                if (it->second.stop_requested) {
                    it->second.status = "cancelled";
                    it->second.finished_ms = NowMs();
                    it->second.last_active_ms = it->second.finished_ms;
                    return;
                }
                if (timeout_s > 0 && NowMs() - started_ms >= static_cast<int64_t>(timeout_s) * 1000) {
                    it->second.status = "timeout";
                    it->second.error_code = "TIMEOUT";
                    it->second.error_message = "batch runtime task timeout";
                    it->second.error_stage = "timeout";
                    it->second.finished_ms = NowMs();
                    it->second.last_active_ms = it->second.finished_ms;
                    return;
                }
                it->second.current_sql_index = static_cast<int>(i);
                it->second.last_active_ms = NowMs();
            }

            rapidjson::StringBuffer req_buf;
            rapidjson::Writer<rapidjson::StringBuffer> req_w(req_buf);
            req_w.StartObject();
            req_w.Key("sql");
            req_w.String(sqls[i].c_str());
            req_w.EndObject();

            std::string exec_rsp;
            const int32_t rc = state->execute_handler("/scheduler/batch/execute", req_buf.GetString(), exec_rsp);
            if (rc != error::OK) {
                std::string err_code;
                std::string err_message;
                std::string err_stage;
                ParseExecuteError(exec_rsp, &err_code, &err_message, &err_stage);
                if (err_code.empty()) err_code = "EXECUTION_FAILED";
                if (err_message.empty()) err_message = "batch SQL execute failed";
                if (err_stage.empty()) err_stage = "execute";
                std::lock_guard<std::mutex> lock(state->mu);
                auto it = state->tasks.find(runtime_task_id);
                if (it == state->tasks.end()) return;
                it->second.status = "failed";
                it->second.error_code = err_code;
                it->second.error_message = err_message;
                it->second.error_stage = err_stage;
                it->second.finished_ms = NowMs();
                it->second.last_active_ms = it->second.finished_ms;
                return;
            }

            int64_t rows = 0;
            int64_t cols = 0;
            std::string target;
            ParseExecuteResult(exec_rsp, &rows, &cols, &target);
            {
                std::lock_guard<std::mutex> lock(state->mu);
                auto it = state->tasks.find(runtime_task_id);
                if (it == state->tasks.end()) return;
                it->second.result_row_count = rows;
                it->second.result_col_count = cols;
                it->second.result_target = target;
                it->second.last_active_ms = NowMs();
                if (timeout_s > 0 && NowMs() - started_ms >= static_cast<int64_t>(timeout_s) * 1000) {
                    it->second.status = "timeout";
                    it->second.error_code = "TIMEOUT";
                    it->second.error_message = "batch runtime task timeout";
                    it->second.error_stage = "timeout";
                    it->second.finished_ms = NowMs();
                    it->second.last_active_ms = it->second.finished_ms;
                    return;
                }
            }
        }

        std::lock_guard<std::mutex> lock(state->mu);
        auto it = state->tasks.find(runtime_task_id);
        if (it == state->tasks.end()) return;
        it->second.current_sql_index = it->second.sql_count;
        if (it->second.stop_requested) {
            it->second.status = "cancelled";
        } else {
            it->second.status = "completed";
        }
        it->second.finished_ms = NowMs();
        it->second.last_active_ms = it->second.finished_ms;
    }

    void InstallBatchCompatRuntimeRoutes() {
        fnRouterHandler execute_handler;
        bool has_submit = false;
        bool has_status = false;
        bool has_stop = false;
        for (const auto& item : items_) {
            if (item.method != "POST") continue;
            if (item.uri == "/scheduler/batch/execute") execute_handler = item.handler;
            if (item.uri == "/scheduler/batch/submit") has_submit = true;
            if (item.uri == "/scheduler/batch/status") has_status = true;
            if (item.uri == "/scheduler/batch/stop") has_stop = true;
        }
        if (!execute_handler) return;
        if (has_submit && has_status && has_stop) return;

        batch_runtime_state_ = std::make_shared<BatchCompatRuntimeState>();
        batch_runtime_state_->execute_handler = execute_handler;
        auto state = batch_runtime_state_;

        if (!has_submit) {
            items_.push_back({"POST", "/scheduler/batch/submit",
                              [state](const std::string&, const std::string& req, std::string& rsp) {
                                  rapidjson::Document d;
                                  d.Parse(req.c_str());
                                  if (d.HasParseError() || !d.IsObject()) {
                                      rsp = R"({"error":"invalid request body"})";
                                      return error::BAD_REQUEST;
                                  }

                                  std::string runtime_task_id;
                                  if (d.HasMember("runtime_task_id") && d["runtime_task_id"].IsString()) {
                                      runtime_task_id = d["runtime_task_id"].GetString();
                                  }
                                  if (runtime_task_id.empty()) {
                                      const uint64_t seq = state->seq.fetch_add(1) + 1;
                                      runtime_task_id = "batch_rt_" + std::to_string(seq);
                                  }

                                  int timeout_s = 0;
                                  if (d.HasMember("timeout_s") && d["timeout_s"].IsInt()) {
                                      timeout_s = d["timeout_s"].GetInt();
                                  }

                                  std::vector<std::string> sqls;
                                  if (!d.HasMember("sqls") || !d["sqls"].IsArray() || d["sqls"].Empty()) {
                                      rsp = R"({"error":"sqls must be non-empty string array"})";
                                      return error::BAD_REQUEST;
                                  }
                                  for (const auto& v : d["sqls"].GetArray()) {
                                      if (!v.IsString()) {
                                          rsp = R"({"error":"sqls must be non-empty string array"})";
                                          return error::BAD_REQUEST;
                                      }
                                      sqls.emplace_back(v.GetString());
                                  }

                                  {
                                      std::lock_guard<std::mutex> lock(state->mu);
                                      if (state->tasks.find(runtime_task_id) != state->tasks.end()) {
                                          rsp = R"({"error":"runtime_task_id already exists"})";
                                          return error::CONFLICT;
                                      }
                                      BatchCompatTask task;
                                      task.runtime_task_id = runtime_task_id;
                                      task.sqls = std::move(sqls);
                                      task.timeout_s = timeout_s;
                                      task.sql_count = static_cast<int>(task.sqls.size());
                                      task.current_sql_index = 0;
                                      task.created_ms = NowMs();
                                      task.last_active_ms = task.created_ms;
                                      state->tasks[runtime_task_id] = std::move(task);
                                  }

                                  std::thread([state, runtime_task_id]() {
                                      RunBatchCompatTask(state, runtime_task_id);
                                  }).detach();

                                  rapidjson::StringBuffer out;
                                  rapidjson::Writer<rapidjson::StringBuffer> w(out);
                                  w.StartObject();
                                  w.Key("status");
                                  w.String("submitted");
                                  w.Key("runtime_task_id");
                                  w.String(runtime_task_id.c_str());
                                  w.Key("runtime_kind");
                                  w.String("batch");
                                  w.EndObject();
                                  rsp = out.GetString();
                                  return error::OK;
                              }});
        }

        if (!has_status) {
            items_.push_back({"POST", "/scheduler/batch/status",
                              [state](const std::string&, const std::string& req, std::string& rsp) {
                                  rapidjson::Document d;
                                  d.Parse(req.c_str());
                                  if (d.HasParseError() || !d.IsObject()) {
                                      rsp = R"({"error":"invalid request body"})";
                                      return error::BAD_REQUEST;
                                  }
                                  std::string runtime_task_id;
                                  if (d.HasMember("runtime_task_id") && d["runtime_task_id"].IsString()) {
                                      runtime_task_id = d["runtime_task_id"].GetString();
                                  } else if (d.HasMember("task_id") && d["task_id"].IsString()) {
                                      runtime_task_id = d["task_id"].GetString();
                                  }
                                  if (runtime_task_id.empty()) {
                                      rsp = R"({"error":"runtime_task_id is required"})";
                                      return error::BAD_REQUEST;
                                  }

                                  BatchCompatTask snapshot;
                                  {
                                      std::lock_guard<std::mutex> lock(state->mu);
                                      auto it = state->tasks.find(runtime_task_id);
                                      if (it == state->tasks.end()) {
                                          rsp = R"({"error":"batch runtime task not found"})";
                                          return error::NOT_FOUND;
                                      }
                                      snapshot = it->second;
                                  }
                                  rsp = BuildBatchStatusJson(snapshot);
                                  return error::OK;
                              }});
        }

        if (!has_stop) {
            items_.push_back({"POST", "/scheduler/batch/stop",
                              [state](const std::string&, const std::string& req, std::string& rsp) {
                                  rapidjson::Document d;
                                  d.Parse(req.c_str());
                                  if (d.HasParseError() || !d.IsObject()) {
                                      rsp = R"({"error":"invalid request body"})";
                                      return error::BAD_REQUEST;
                                  }
                                  std::string runtime_task_id;
                                  if (d.HasMember("runtime_task_id") && d["runtime_task_id"].IsString()) {
                                      runtime_task_id = d["runtime_task_id"].GetString();
                                  } else if (d.HasMember("task_id") && d["task_id"].IsString()) {
                                      runtime_task_id = d["task_id"].GetString();
                                  }
                                  if (runtime_task_id.empty()) {
                                      rsp = R"({"error":"runtime_task_id is required"})";
                                      return error::BAD_REQUEST;
                                  }

                                  BatchCompatTask snapshot;
                                  {
                                      std::lock_guard<std::mutex> lock(state->mu);
                                      auto it = state->tasks.find(runtime_task_id);
                                      if (it == state->tasks.end()) {
                                          rsp = R"({"error":"batch runtime task not found"})";
                                          return error::NOT_FOUND;
                                      }
                                      it->second.stop_requested = true;
                                      if (it->second.status == "pending") {
                                          it->second.status = "cancelled";
                                          it->second.finished_ms = NowMs();
                                          it->second.last_active_ms = it->second.finished_ms;
                                      } else if (it->second.status == "running") {
                                          it->second.status = "stopping";
                                          it->second.last_active_ms = NowMs();
                                      }
                                      snapshot = it->second;
                                  }
                                  rsp = BuildBatchStatusJson(snapshot);
                                  return error::OK;
                              }});
        }
    }

    std::vector<RouteItem> items_;
    std::shared_ptr<BatchCompatRuntimeState> batch_runtime_state_;
};

class MockChannelRegistry : public IChannelRegistry {
 public:
    int Register(const char*, std::shared_ptr<IChannel>) override { return 0; }
    std::shared_ptr<IChannel> Get(const char*) override { return nullptr; }
    int Unregister(const char* name) override {
        if (name && *name) removed_names_.push_back(name);
        return 0;
    }
    int Rename(const char*, const char*) override { return 0; }
    void List(std::function<void(const char*, std::shared_ptr<IChannel>)>) override {}

    const std::vector<std::string>& removed_names() const { return removed_names_; }

 private:
    std::vector<std::string> removed_names_;
};

class MockQuerier : public IQuerier {
 public:
    void AddHandle(IRouterHandle* h) {
        handles_.push_back(h);
        if (!scheduler_control_service_ && h) {
            route_scheduler_control_service_ = std::make_unique<RouterBackedSchedulerControlService>(h);
            scheduler_control_service_ = route_scheduler_control_service_.get();
        }
    }
    void SetChannelRegistry(IChannelRegistry* r) { channel_registry_ = r; }
    void SetSchedulerControlService(ISchedulerControlService* s) {
        route_scheduler_control_service_.reset();
        scheduler_control_service_ = s;
    }

    int Traverse(const Guid& iid, fntraverse proc) override {
        if (memcmp(&iid, &IID_ROUTER_HANDLE, sizeof(Guid)) != 0) return 0;
        for (auto* h : handles_) {
            if (proc(h) == -1) break;
        }
        return 0;
    }

    void* First(const Guid& iid) override {
        if (memcmp(&iid, &IID_CHANNEL_REGISTRY, sizeof(Guid)) == 0) return channel_registry_;
        if (memcmp(&iid, &IID_SCHEDULER_CONTROL_SERVICE, sizeof(Guid)) == 0) return scheduler_control_service_;
        return nullptr;
    }

 private:
    std::vector<IRouterHandle*> handles_;
    IChannelRegistry* channel_registry_ = nullptr;
    std::unique_ptr<RouterBackedSchedulerControlService> route_scheduler_control_service_;
    ISchedulerControlService* scheduler_control_service_ = nullptr;
};

class MockSchedulerControlService : public ISchedulerControlService {
 public:
    int32_t ClassifySql(const std::string& req_json, std::string* rsp_json) override {
        ++classify_calls;
        if (!rsp_json) return error::INTERNAL_ERROR;
        rapidjson::Document d;
        d.Parse(req_json.c_str());
        if (d.HasParseError() || !d.IsObject() || !d.HasMember("sql") || !d["sql"].IsString()) {
            *rsp_json = R"({"error":"invalid request, expected {\"sql\":\"...\"}"})";
            return error::BAD_REQUEST;
        }
        *rsp_json = R"({"task_kind":"batch"})";
        return error::OK;
    }

    int32_t ExecuteBatch(const std::string& req_json, std::string* rsp_json) override {
        ++execute_batch_calls;
        if (!rsp_json) return error::INTERNAL_ERROR;
        rapidjson::Document d;
        d.Parse(req_json.c_str());
        if (d.HasParseError() || !d.IsObject() || !d.HasMember("sql") || !d["sql"].IsString()) {
            *rsp_json = R"({"error":"invalid request, expected {\"sql\":\"...\"}"})";
            return error::BAD_REQUEST;
        }
        *rsp_json = R"({"status":"completed","result_row_count":9,"result_col_count":1,"result_target":"dataframe.iface","data":[]})";
        return error::OK;
    }

    int32_t ExecuteStream(const std::string&, std::string* rsp_json) override {
        if (rsp_json) *rsp_json = R"({"error":"not implemented"})";
        return error::BAD_REQUEST;
    }

    int32_t StopStream(const std::string&, std::string* rsp_json) override {
        if (rsp_json) *rsp_json = R"({"error":"not implemented"})";
        return error::BAD_REQUEST;
    }

    int32_t QueryStreamStatus(const std::string&, std::string* rsp_json) override {
        if (rsp_json) *rsp_json = R"({"error":"not implemented"})";
        return error::BAD_REQUEST;
    }

    int classify_calls = 0;
    int execute_batch_calls = 0;
};

int main() {
    {
        const std::string policy_dir = MakeTempDir("execution_policy");
        MockRouterHandle scheduler({
            MakeSqlClassifyRoute(),
            {"POST", "/scheduler/batch/execute",
             [](const std::string&, const std::string&, std::string& rsp) {
                 rsp = R"({"status":"completed","result_row_count":0,"data":[]})";
                 return error::OK;
             }},
        });
        MockQuerier querier;
        querier.AddHandle(&scheduler);
        TaskPlugin plugin;
        ASSERT_EQ(plugin.Option(("db_dir=" + policy_dir + ";disable_worker=1").c_str()), 0);
        ASSERT_EQ(plugin.Load(&querier), 0);
        ASSERT_EQ(plugin.Start(), 0);
        auto policy_routes = CollectRoutes(&plugin);
        std::string response;
        ASSERT_EQ(
            policy_routes["POST:/tasks/sql/analyze"]("", R"({"sql_text":"SELECT * FROM netadapter.eth0"})", response),
            error::OK);
        rapidjson::Document analysis;
        analysis.Parse(response.c_str());
        ASSERT_TRUE(analysis.HasMember("requires_async") && analysis["requires_async"].GetBool());
        ASSERT_EQ(std::string(analysis["task_kind"].GetString()), "batch");
        ASSERT_EQ(policy_routes["POST:/tasks/batch/execute"](
                      "", R"({"sql_text":"SELECT * FROM netadapter.eth0","mode":"sync"})", response),
                  error::BAD_REQUEST);
        rapidjson::Document rejected;
        rejected.Parse(response.c_str());
        ASSERT_EQ(std::string(rejected["error_code"].GetString()), "ASYNC_EXECUTION_REQUIRED");
        ASSERT_EQ(rejected["sql_index"].GetUint(), 0u);
        ASSERT_TRUE(std::string(rejected["error"].GetString()).find("async") != std::string::npos);
        ASSERT_EQ(CountTasks(policy_dir + "/task_store.db"), 0);
        ASSERT_EQ(
            policy_routes["POST:/tasks/sql/analyze"]("", R"({"sql_text":"SELECT * FROM pcapfile.input"})", response),
            error::OK);
        analysis.Parse(response.c_str());
        ASSERT_TRUE(!analysis["requires_async"].GetBool());
        ASSERT_EQ(policy_routes["POST:/tasks/batch/execute"](
                      "", R"({"sql_text":"SELECT * FROM pcapfile.input","mode":"sync"})", response),
                  error::OK);
        ASSERT_EQ(policy_routes["POST:/tasks/sql/analyze"](
                      "", R"({"sql_text":"SELECT 1;SELECT * FROM netadapter.eth0"})", response),
                  error::OK);
        analysis.Parse(response.c_str());
        ASSERT_TRUE(analysis["requires_async"].GetBool());
        ASSERT_EQ(policy_routes["POST:/tasks/batch/execute"](
                      "", R"({"sql_text":"SELECT 1;SELECT * FROM netadapter.eth0","mode":"sync"})", response),
                  error::BAD_REQUEST);
        rejected.Parse(response.c_str());
        ASSERT_EQ(rejected["sql_index"].GetUint(), 1u);
        ASSERT_EQ(CountTasks(policy_dir + "/task_store.db"), 1);
        ASSERT_EQ(policy_routes["POST:/tasks/batch/execute"](
                      "", R"({"sql_text":"SELECT * FROM netadapter.eth0","mode":"async"})", response),
                  error::OK);
        rapidjson::Document submitted;
        submitted.Parse(response.c_str());
        ASSERT_TRUE(submitted.HasMember("runtime_task_id") &&
                    !std::string(submitted["runtime_task_id"].GetString()).empty());
        ASSERT_EQ(std::string(submitted["status"].GetString()), "pending");
        ASSERT_EQ(plugin.Stop(), 0);
    }
    std::puts("=== TaskPlugin Tests ===");
    const std::string dir = MakeTempDir("basic");

    std::unordered_map<std::string, fnRouterHandler> routes;
    std::string task_id;

    const std::string db_path = dir + "/task_store.db";

    {
        const std::string iface_dir = MakeTempDir("scheduler_control_service");
        MockSchedulerControlService scheduler_service;
        MockQuerier querier;
        querier.SetSchedulerControlService(&scheduler_service);

        TaskPlugin p;
        const std::string opt = "db_dir=" + iface_dir + ";disable_worker=1";
        ASSERT_EQ(p.Option(opt.c_str()), 0);
        ASSERT_EQ(p.Load(&querier), 0);
        ASSERT_EQ(p.Start(), 0);

        auto local_routes = CollectRoutes(&p);

        std::string rsp;
        ASSERT_EQ(local_routes["POST:/tasks/batch/execute"](
                      "/tasks/batch/execute",
                      R"({"sql_text":"SELECT 123","mode":"sync"})",
                      rsp),
                  error::OK);
        rapidjson::Document d;
        d.Parse(rsp.c_str());
        ASSERT_TRUE(!d.HasParseError() && d.IsObject());
        ASSERT_TRUE(d.HasMember("status") && d["status"].IsString());
        ASSERT_EQ(std::string(d["status"].GetString()), "completed");
        ASSERT_TRUE(d.HasMember("rows") && d["rows"].IsInt64());
        ASSERT_EQ(d["rows"].GetInt64(), 9);
        ASSERT_EQ(scheduler_service.classify_calls, 1);
        ASSERT_EQ(scheduler_service.execute_batch_calls, 1);

        ASSERT_EQ(p.Stop(), 0);
    }

    {
        MockRouterHandle scheduler({
            MakeSqlClassifyRoute(),
            {"POST", "/scheduler/batch/execute",
             [](const std::string&, const std::string&, std::string& rsp) {
                 std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                 rsp = R"({"status":"completed","result_row_count":1,"result_col_count":1,"result_target":"dataframe.pending","data":[]})";
                 return error::OK;
             }},
        });
        MockQuerier querier;
        querier.AddHandle(&scheduler);

        TaskPlugin p;
        const std::string opt = "db_dir=" + dir + ";disable_worker=1";
        ASSERT_EQ(p.Option(opt.c_str()), 0);
        ASSERT_EQ(p.Load(&querier), 0);
        ASSERT_EQ(p.Start(), 0);
        routes = CollectRoutes(&p);
        ASSERT_TRUE(routes.count("POST:/tasks/batch/execute") == 1);
        ASSERT_TRUE(routes.count("POST:/tasks/sql/classify") == 1);
        ASSERT_TRUE(routes.count("POST:/tasks/sql/analyze") == 1);
        ASSERT_TRUE(routes.count("POST:/tasks/list") == 1);
        ASSERT_TRUE(routes.count("POST:/tasks/detail") == 1);
        ASSERT_TRUE(routes.count("POST:/tasks/diagnostics") == 1);
        ASSERT_TRUE(routes.count("POST:/tasks/delete") == 1);
        ASSERT_TRUE(routes.count("POST:/tasks/cancel") == 1);

        std::string rsp;
        ASSERT_EQ(routes["POST:/tasks/batch/execute"]("/tasks/batch/execute", R"({"sql_text":"SELECT 1","mode":"async"})", rsp), error::OK);
        rapidjson::Document d;
        d.Parse(rsp.c_str());
        ASSERT_TRUE(!d.HasParseError());
        ASSERT_TRUE(d.HasMember("task_id") && d["task_id"].IsString());
        task_id = d["task_id"].GetString();
        ASSERT_TRUE(!task_id.empty());

        ASSERT_EQ(routes["POST:/tasks/list"]("/tasks/list", "{}", rsp), error::OK);
        rapidjson::Document list;
        list.Parse(rsp.c_str());
        ASSERT_TRUE(!list.HasParseError());
        ASSERT_TRUE(list.HasMember("items") && list["items"].IsArray());
        ASSERT_EQ(list["items"].Size(), rapidjson::SizeType(1));
        ASSERT_TRUE(list["items"][0].HasMember("task_id") && list["items"][0]["task_id"].IsString());
        ASSERT_EQ(std::string(list["items"][0]["task_id"].GetString()), task_id);

        ASSERT_EQ(routes["POST:/tasks/delete"]("/tasks/delete",
                                               MakeTaskIdReq(task_id),
                                               rsp),
                  error::CONFLICT);

        ASSERT_EQ(p.Stop(), 0);
    }

    {
        TaskPlugin p;
        const std::string opt = "db_dir=" + dir + ";disable_worker=1";
        ASSERT_EQ(p.Option(opt.c_str()), 0);
        ASSERT_EQ(p.Load(nullptr), 0);
        ASSERT_EQ(p.Start(), 0);
        routes = CollectRoutes(&p);

        std::string rsp;
        ASSERT_EQ(routes["POST:/tasks/detail"]("/tasks/detail",
                                               MakeTaskIdReq(task_id),
                                               rsp),
                  error::OK);
        rapidjson::Document d;
        d.Parse(rsp.c_str());
        ASSERT_TRUE(!d.HasParseError());
        ASSERT_EQ(std::string(d["status"].GetString()), "failed");
        ASSERT_EQ(std::string(d["error_code"].GetString()), "PROCESS_RESTART");
        ASSERT_EQ(std::string(d["error_stage"].GetString()), "bootstrap");
        ASSERT_TRUE(CountTaskEvents(db_path, task_id) >= 1);

        ASSERT_EQ(routes["POST:/tasks/delete"]("/tasks/delete",
                                               MakeTaskIdReq(task_id),
                                               rsp),
                  error::OK);
        ASSERT_EQ(CountTaskEvents(db_path, task_id), 0);
        ASSERT_EQ(p.Stop(), 0);
    }

    {
        const std::string dir2 = MakeTempDir("db_path");
        const std::string db_path = dir2 + "/meta/shared.db";
        MockRouterHandle scheduler({MakeSqlClassifyRoute()});
        MockQuerier querier;
        querier.AddHandle(&scheduler);

        TaskPlugin p;
        const std::string opt = "db_path=" + db_path + ";disable_worker=1";
        ASSERT_EQ(p.Option(opt.c_str()), 0);
        ASSERT_EQ(p.Load(&querier), 0);
        ASSERT_EQ(p.Start(), 0);

        auto local_routes = CollectRoutes(&p);
        std::string rsp;
        ASSERT_EQ(local_routes["POST:/tasks/batch/execute"]("/tasks/batch/execute", R"({"sql_text":"SELECT 1","mode":"sync"})", rsp), error::OK);
        ASSERT_TRUE(std::filesystem::exists(db_path));
        ASSERT_EQ(p.Stop(), 0);
    }

    {
        const std::string async_dir = MakeTempDir("async_worker");
        const std::string async_db_path = async_dir + "/task_store.db";

        MockRouterHandle scheduler({
            MakeSqlClassifyRoute(),
            {"POST", "/scheduler/batch/execute",
             [](const std::string&, const std::string&, std::string& rsp) {
                 std::this_thread::sleep_for(std::chrono::milliseconds(120));
                 rsp = R"({"status":"completed","result_row_count":7,"result_col_count":2,"result_target":"dataframe.async_out","data":[]})";
                 return error::OK;
             }},
        });
        MockQuerier querier;
        querier.AddHandle(&scheduler);

        TaskPlugin p;
        const std::string opt = "db_dir=" + async_dir;
        ASSERT_EQ(p.Option(opt.c_str()), 0);
        ASSERT_EQ(p.Load(&querier), 0);
        ASSERT_EQ(p.Start(), 0);

        auto local_routes = CollectRoutes(&p);
        ASSERT_TRUE(local_routes.count("POST:/tasks/batch/execute") == 1);
        ASSERT_TRUE(local_routes.count("POST:/tasks/detail") == 1);
        ASSERT_TRUE(local_routes.count("POST:/tasks/delete") == 1);

        std::string rsp;
        ASSERT_EQ(local_routes["POST:/tasks/batch/execute"]("/tasks/batch/execute", R"({"sql_text":"SELECT 42","mode":"async"})", rsp), error::OK);
        rapidjson::Document submit;
        submit.Parse(rsp.c_str());
        ASSERT_TRUE(!submit.HasParseError() && submit.IsObject());
        ASSERT_TRUE(submit.HasMember("task_id") && submit["task_id"].IsString());
        ASSERT_TRUE(submit.HasMember("status") && submit["status"].IsString());
        ASSERT_EQ(std::string(submit["status"].GetString()), "pending");
        const std::string async_task_id = submit["task_id"].GetString();

        bool done = false;
        bool saw_running = false;
        for (int i = 0; i < 80; ++i) {
            ASSERT_EQ(local_routes["POST:/tasks/detail"]("/tasks/detail", MakeTaskIdReq(async_task_id), rsp), error::OK);
            rapidjson::Document detail;
            detail.Parse(rsp.c_str());
            ASSERT_TRUE(!detail.HasParseError() && detail.IsObject());
            ASSERT_TRUE(detail.HasMember("status") && detail["status"].IsString());
            const std::string status = detail["status"].GetString();
            if (status == "running") saw_running = true;
            if (status == "completed") {
                ASSERT_TRUE(detail.HasMember("result_row_count") && detail["result_row_count"].IsInt64());
                ASSERT_TRUE(detail.HasMember("result_col_count") && detail["result_col_count"].IsInt64());
                ASSERT_TRUE(detail.HasMember("result_target") && detail["result_target"].IsString());
                ASSERT_EQ(detail["result_row_count"].GetInt64(), 7);
                ASSERT_EQ(detail["result_col_count"].GetInt64(), 2);
                ASSERT_EQ(std::string(detail["result_target"].GetString()), "dataframe.async_out");
                done = true;
                break;
            }
            if (status == "failed") {
                ASSERT_TRUE(false);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        ASSERT_TRUE(done);
        ASSERT_TRUE(saw_running);

        ASSERT_EQ(local_routes["POST:/tasks/delete"]("/tasks/delete", MakeTaskIdReq(async_task_id), rsp), error::OK);
        ASSERT_EQ(CountTaskEvents(async_db_path, async_task_id), 0);
        ASSERT_EQ(p.Stop(), 0);
    }

    {
        const std::string async_fail_dir = MakeTempDir("async_worker_fail");
        const std::string async_fail_db_path = async_fail_dir + "/task_store.db";

        MockRouterHandle scheduler({
            MakeSqlClassifyRoute(),
            {"POST", "/scheduler/batch/execute",
             [](const std::string&, const std::string&, std::string& rsp) {
                 std::this_thread::sleep_for(std::chrono::milliseconds(80));
                 rsp = R"({"error":"operator builtin.mock execution failed","error_code":"OP_EXEC_FAIL","error_stage":"mock"})";
                 return error::INTERNAL_ERROR;
             }},
        });
        MockQuerier querier;
        querier.AddHandle(&scheduler);

        TaskPlugin p;
        const std::string opt = "db_dir=" + async_fail_dir;
        ASSERT_EQ(p.Option(opt.c_str()), 0);
        ASSERT_EQ(p.Load(&querier), 0);
        ASSERT_EQ(p.Start(), 0);

        auto local_routes = CollectRoutes(&p);

        std::string rsp;
        ASSERT_EQ(local_routes["POST:/tasks/batch/execute"]("/tasks/batch/execute", R"({"sql_text":"SELECT 99","mode":"async"})", rsp), error::OK);
        rapidjson::Document submit;
        submit.Parse(rsp.c_str());
        ASSERT_TRUE(!submit.HasParseError() && submit.IsObject());
        const std::string task_id = submit["task_id"].GetString();

        bool done = false;
        for (int i = 0; i < 80; ++i) {
            ASSERT_EQ(local_routes["POST:/tasks/detail"]("/tasks/detail", MakeTaskIdReq(task_id), rsp), error::OK);
            rapidjson::Document detail;
            detail.Parse(rsp.c_str());
            ASSERT_TRUE(!detail.HasParseError() && detail.IsObject());
            const std::string status = detail["status"].GetString();
            if (status == "failed") {
                ASSERT_TRUE(detail.HasMember("error_code") && detail["error_code"].IsString());
                ASSERT_TRUE(detail.HasMember("error_stage") && detail["error_stage"].IsString());
                ASSERT_EQ(std::string(detail["error_code"].GetString()), "OP_EXEC_FAIL");
                ASSERT_EQ(std::string(detail["error_stage"].GetString()), "mock");
                done = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        ASSERT_TRUE(done);
        ASSERT_EQ(local_routes["POST:/tasks/delete"]("/tasks/delete", MakeTaskIdReq(task_id), rsp), error::OK);
        ASSERT_EQ(CountTaskEvents(async_fail_db_path, task_id), 0);
        ASSERT_EQ(p.Stop(), 0);
    }

    {
        const std::string multi_dir = MakeTempDir("multi_sql");
        std::vector<std::string> captured_sqls;
        MockChannelRegistry channel_registry;

        MockRouterHandle scheduler({
            MakeSqlClassifyRoute(),
            {"POST", "/scheduler/batch/execute",
             [&captured_sqls](const std::string&, const std::string& req, std::string& rsp) {
                 rapidjson::Document d;
                 d.Parse(req.c_str());
                 ASSERT_TRUE(!d.HasParseError() && d.IsObject());
                 ASSERT_TRUE(d.HasMember("sql") && d["sql"].IsString());
                 const std::string sql = d["sql"].GetString();
                 captured_sqls.push_back(sql);
                 if (captured_sqls.size() == 1) {
                     rsp = R"({"status":"completed","result_row_count":2,"result_col_count":3,"result_target":"dataframe.tmp","data":[]})";
                 } else {
                     rsp = R"({"status":"completed","result_row_count":5,"result_col_count":4,"result_target":"dataframe.final","data":[]})";
                 }
                 return error::OK;
             }},
        });

        MockQuerier querier;
        querier.AddHandle(&scheduler);
        querier.SetChannelRegistry(&channel_registry);

        TaskPlugin p;
        const std::string opt = "db_dir=" + multi_dir;
        ASSERT_EQ(p.Option(opt.c_str()), 0);
        ASSERT_EQ(p.Load(&querier), 0);
        ASSERT_EQ(p.Start(), 0);

        auto local_routes = CollectRoutes(&p);

        std::string rsp;
        ASSERT_EQ(local_routes["POST:/tasks/batch/execute"](
                      "/tasks/batch/execute",
                      R"({"mode":"sync","sql_text":"SELECT * FROM sqlite.local.src INTO dataframe.tmp;SELECT * FROM dataframe.tmp USING builtin.passthrough INTO dataframe.final;"})",
                      rsp),
                  error::OK);
        rapidjson::Document submit;
        submit.Parse(rsp.c_str());
        ASSERT_TRUE(!submit.HasParseError() && submit.IsObject());
        ASSERT_TRUE(submit.HasMember("task_id") && submit["task_id"].IsString());
        const std::string task_id = submit["task_id"].GetString();

        bool completed = false;
        for (int i = 0; i < 80; ++i) {
            ASSERT_EQ(local_routes["POST:/tasks/detail"]("/tasks/detail", MakeTaskIdReq(task_id), rsp), error::OK);
            rapidjson::Document detail;
            detail.Parse(rsp.c_str());
            ASSERT_TRUE(!detail.HasParseError() && detail.IsObject());
            ASSERT_TRUE(detail.HasMember("status") && detail["status"].IsString());
            if (std::string(detail["status"].GetString()) == "completed") {
                ASSERT_EQ(detail["result_row_count"].GetInt64(), 5);
                ASSERT_EQ(detail["result_col_count"].GetInt64(), 4);
                ASSERT_EQ(std::string(detail["result_target"].GetString()), "dataframe.final");
                completed = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        ASSERT_TRUE(completed);
        ASSERT_EQ(captured_sqls.size(), size_t(2));
        ASSERT_EQ(captured_sqls[0], "SELECT * FROM sqlite.local.src INTO dataframe.tmp");
        ASSERT_EQ(captured_sqls[1], "SELECT * FROM dataframe.tmp USING builtin.passthrough INTO dataframe.final");

        const auto& removed = channel_registry.removed_names();
        ASSERT_TRUE(!removed.empty());
        bool has_tmp = false;
        for (const auto& name : removed) {
            if (name == "tmp") {
                has_tmp = true;
                break;
            }
        }
        ASSERT_TRUE(has_tmp);

        ASSERT_EQ(local_routes["POST:/tasks/delete"]("/tasks/delete", MakeTaskIdReq(task_id), rsp), error::OK);
        ASSERT_EQ(p.Stop(), 0);
    }

    {
        const std::string diag_dir = MakeTempDir("diagnostics");
        std::atomic<int> call_no{0};
        MockRouterHandle scheduler({
            MakeSqlClassifyRoute(),
            {"POST", "/scheduler/batch/execute",
             [&call_no](const std::string&, const std::string&, std::string& rsp) {
                 int n = call_no.fetch_add(1);
                 if (n == 0) {
                     rsp = R"({"status":"completed","result_row_count":3,"result_col_count":2,"result_target":"dataframe.tmp","data":[]})";
                 } else {
                     rsp = R"({"status":"completed","result_row_count":5,"result_col_count":2,"result_target":"dataframe.final","data":[]})";
                 }
                 return error::OK;
             }},
        });
        MockQuerier querier;
        querier.AddHandle(&scheduler);

        TaskPlugin p;
        const std::string opt = "db_dir=" + diag_dir + ";worker_threads=1";
        ASSERT_EQ(p.Option(opt.c_str()), 0);
        ASSERT_EQ(p.Load(&querier), 0);
        ASSERT_EQ(p.Start(), 0);

        auto local_routes = CollectRoutes(&p);

        std::string rsp;
        ASSERT_EQ(local_routes["POST:/tasks/batch/execute"](
                      "/tasks/batch/execute",
                      R"({"mode":"sync","sql_text":"SELECT * FROM sqlite.local.src INTO dataframe.tmp;SELECT * FROM dataframe.tmp USING builtin.passthrough INTO dataframe.final;"})",
                      rsp),
                  error::OK);
        rapidjson::Document submit;
        submit.Parse(rsp.c_str());
        ASSERT_TRUE(!submit.HasParseError() && submit.IsObject());
        const std::string task_id = submit["task_id"].GetString();

        bool completed = false;
        for (int i = 0; i < 120; ++i) {
            ASSERT_EQ(local_routes["POST:/tasks/detail"]("/tasks/detail", MakeTaskIdReq(task_id), rsp), error::OK);
            rapidjson::Document detail;
            detail.Parse(rsp.c_str());
            ASSERT_TRUE(!detail.HasParseError() && detail.IsObject());
            if (std::string(detail["status"].GetString()) == "completed") {
                completed = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        ASSERT_TRUE(completed);

        ASSERT_EQ(local_routes["POST:/tasks/diagnostics"]("/tasks/diagnostics", MakeTaskIdReq(task_id), rsp), error::OK);
        rapidjson::Document diag;
        diag.Parse(rsp.c_str());
        ASSERT_TRUE(!diag.HasParseError() && diag.IsObject());
        ASSERT_TRUE(diag.HasMember("items") && diag["items"].IsArray());
        ASSERT_EQ(diag["items"].Size(), rapidjson::SizeType(2));
        ASSERT_EQ(diag["items"][0]["sql_index"].GetInt(), 0);
        ASSERT_EQ(diag["items"][0]["sink_rows"].GetInt64(), 3);
        ASSERT_EQ(std::string(diag["items"][0]["operator_chain"].GetString()), "");
        ASSERT_EQ(diag["items"][1]["sql_index"].GetInt(), 1);
        ASSERT_EQ(diag["items"][1]["source_rows"].GetInt64(), 3);
        ASSERT_EQ(diag["items"][1]["sink_rows"].GetInt64(), 5);
        ASSERT_EQ(std::string(diag["items"][1]["operator_chain"].GetString()), "builtin.passthrough");

        ASSERT_EQ(local_routes["POST:/tasks/delete"]("/tasks/delete", MakeTaskIdReq(task_id), rsp), error::OK);
        ASSERT_EQ(local_routes["POST:/tasks/diagnostics"]("/tasks/diagnostics", MakeTaskIdReq(task_id), rsp), error::NOT_FOUND);
        ASSERT_EQ(p.Stop(), 0);
    }

    {
        const std::string multi_fail_dir = MakeTempDir("multi_sql_fail");
        std::vector<std::string> captured_sqls;
        MockChannelRegistry channel_registry;

        MockRouterHandle scheduler({
            MakeSqlClassifyRoute(),
            {"POST", "/scheduler/batch/execute",
             [&captured_sqls](const std::string&, const std::string& req, std::string& rsp) {
                 rapidjson::Document d;
                 d.Parse(req.c_str());
                 ASSERT_TRUE(!d.HasParseError() && d.IsObject());
                 ASSERT_TRUE(d.HasMember("sql") && d["sql"].IsString());
                 captured_sqls.push_back(d["sql"].GetString());
                 if (captured_sqls.size() == 1) {
                    rsp = R"({"status":"completed","result_row_count":1,"result_col_count":1,"result_target":"dataframe.tmp","data":[]})";
                    return error::OK;
                 }
                 rsp = R"({"error":"operator builtin.concat execution failed","error_code":"OP_EXEC_FAIL","error_stage":"concat"})";
                 return error::INTERNAL_ERROR;
             }},
        });

        MockQuerier querier;
        querier.AddHandle(&scheduler);
        querier.SetChannelRegistry(&channel_registry);

        TaskPlugin p;
        const std::string opt = "db_dir=" + multi_fail_dir;
        ASSERT_EQ(p.Option(opt.c_str()), 0);
        ASSERT_EQ(p.Load(&querier), 0);
        ASSERT_EQ(p.Start(), 0);

        auto local_routes = CollectRoutes(&p);

        std::string rsp;
        ASSERT_EQ(local_routes["POST:/tasks/batch/execute"](
                      "/tasks/batch/execute",
                      R"({"mode":"sync","sql_text":"SELECT * FROM s1 INTO dataframe.tmp;SELECT * FROM dataframe.tmp,dataframe.tmp USING builtin.concat INTO dataframe.final;"})",
                      rsp),
                  error::OK);
        rapidjson::Document submit;
        submit.Parse(rsp.c_str());
        ASSERT_TRUE(!submit.HasParseError() && submit.IsObject());
        const std::string task_id = submit["task_id"].GetString();

        bool failed = false;
        for (int i = 0; i < 80; ++i) {
            ASSERT_EQ(local_routes["POST:/tasks/detail"]("/tasks/detail", MakeTaskIdReq(task_id), rsp), error::OK);
            rapidjson::Document detail;
            detail.Parse(rsp.c_str());
            ASSERT_TRUE(!detail.HasParseError() && detail.IsObject());
            if (std::string(detail["status"].GetString()) == "failed") {
                ASSERT_EQ(std::string(detail["error_code"].GetString()), "OP_EXEC_FAIL");
                ASSERT_EQ(std::string(detail["error_stage"].GetString()), "concat");
                failed = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        ASSERT_TRUE(failed);
        ASSERT_EQ(captured_sqls.size(), size_t(2));
        const auto& removed = channel_registry.removed_names();
        ASSERT_EQ(removed.size(), size_t(1));
        ASSERT_EQ(removed[0], "tmp");

        ASSERT_EQ(local_routes["POST:/tasks/delete"]("/tasks/delete", MakeTaskIdReq(task_id), rsp), error::OK);
        ASSERT_EQ(p.Stop(), 0);
    }

    {
        const std::string pool_dir = MakeTempDir("thread_pool");
        std::atomic<int> in_flight{0};
        std::atomic<int> max_in_flight{0};

        MockRouterHandle scheduler({
            MakeSqlClassifyRoute(),
            {"POST", "/scheduler/batch/execute",
             [&in_flight, &max_in_flight](const std::string&, const std::string& req, std::string& rsp) {
                 rapidjson::Document d;
                 d.Parse(req.c_str());
                 ASSERT_TRUE(!d.HasParseError() && d.IsObject());
                 ASSERT_TRUE(d.HasMember("sql") && d["sql"].IsString());
                 const std::string sql = d["sql"].GetString();
                 std::string suffix = "x";
                 if (!sql.empty()) suffix = std::to_string(sql.back() - '0');

                 const int cur = in_flight.fetch_add(1) + 1;
                 int observed = max_in_flight.load();
                 while (cur > observed && !max_in_flight.compare_exchange_weak(observed, cur)) {
                 }
                 std::this_thread::sleep_for(std::chrono::milliseconds(220));
                 in_flight.fetch_sub(1);
                 rsp = std::string(R"({"status":"completed","result_row_count":1,"result_col_count":1,"result_target":"dataframe.pool_)")
                     + suffix + R"(","data":[]})";
                 return error::OK;
             }},
        });
        MockQuerier querier;
        querier.AddHandle(&scheduler);

        TaskPlugin p;
        const std::string opt = "db_dir=" + pool_dir + ";worker_threads=2";
        ASSERT_EQ(p.Option(opt.c_str()), 0);
        ASSERT_EQ(p.Load(&querier), 0);
        ASSERT_EQ(p.Start(), 0);

        auto local_routes = CollectRoutes(&p);

        std::vector<std::string> task_ids;
        std::vector<std::string> expected_targets;
        std::string rsp;
        for (int i = 0; i < 4; ++i) {
            const std::string sql = "SELECT " + std::to_string(i + 1);
            rapidjson::StringBuffer req_buf;
            rapidjson::Writer<rapidjson::StringBuffer> w(req_buf);
            w.StartObject();
            w.Key("sql_text");
            w.String(sql.c_str());
            w.Key("mode");
            w.String("async");
            w.EndObject();
            ASSERT_EQ(local_routes["POST:/tasks/batch/execute"]("/tasks/batch/execute", req_buf.GetString(), rsp), error::OK);
            rapidjson::Document submit;
            submit.Parse(rsp.c_str());
            ASSERT_TRUE(!submit.HasParseError() && submit.IsObject());
            task_ids.push_back(submit["task_id"].GetString());
            expected_targets.push_back("dataframe.pool_" + std::to_string(i + 1));
        }

        bool all_done = false;
        for (int round = 0; round < 300; ++round) {
            int completed = 0;
            for (const auto& id : task_ids) {
                ASSERT_EQ(local_routes["POST:/tasks/detail"]("/tasks/detail", MakeTaskIdReq(id), rsp), error::OK);
                rapidjson::Document detail;
                detail.Parse(rsp.c_str());
                ASSERT_TRUE(!detail.HasParseError() && detail.IsObject());
                const std::string status = detail["status"].GetString();
                if (status == "completed") {
                    ASSERT_TRUE(detail.HasMember("result_target") && detail["result_target"].IsString());
                    ++completed;
                } else if (status == "failed") {
                    ASSERT_TRUE(false);
                }
            }
            if (completed == static_cast<int>(task_ids.size())) {
                all_done = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        ASSERT_TRUE(all_done);
        ASSERT_TRUE(max_in_flight.load() >= 2);

        for (size_t i = 0; i < task_ids.size(); ++i) {
            ASSERT_EQ(local_routes["POST:/tasks/detail"]("/tasks/detail", MakeTaskIdReq(task_ids[i]), rsp), error::OK);
            rapidjson::Document detail;
            detail.Parse(rsp.c_str());
            ASSERT_TRUE(!detail.HasParseError() && detail.IsObject());
            ASSERT_EQ(std::string(detail["result_target"].GetString()), expected_targets[i]);
        }

        for (const auto& id : task_ids) {
            ASSERT_EQ(local_routes["POST:/tasks/delete"]("/tasks/delete", MakeTaskIdReq(id), rsp), error::OK);
        }
        ASSERT_EQ(p.Stop(), 0);
    }

    {
        const std::string cancel_pending_dir = MakeTempDir("cancel_pending");
        MockRouterHandle scheduler({
            MakeSqlClassifyRoute(),
            {"POST", "/scheduler/batch/execute",
             [](const std::string&, const std::string&, std::string& rsp) {
                 std::this_thread::sleep_for(std::chrono::milliseconds(200));
                 rsp = R"({"status":"completed","result_row_count":1,"result_col_count":1,"result_target":"dataframe.pending","data":[]})";
                 return error::OK;
             }},
        });
        MockQuerier querier;
        querier.AddHandle(&scheduler);

        TaskPlugin p;
        const std::string opt = "db_dir=" + cancel_pending_dir + ";disable_worker=1";
        ASSERT_EQ(p.Option(opt.c_str()), 0);
        ASSERT_EQ(p.Load(&querier), 0);
        ASSERT_EQ(p.Start(), 0);

        auto local_routes = CollectRoutes(&p);

        std::string rsp;
        ASSERT_EQ(local_routes["POST:/tasks/batch/execute"]("/tasks/batch/execute", R"({"sql_text":"SELECT 1","mode":"async"})", rsp), error::OK);
        rapidjson::Document submit;
        submit.Parse(rsp.c_str());
        ASSERT_TRUE(!submit.HasParseError() && submit.IsObject());
        const std::string task_id = submit["task_id"].GetString();

        ASSERT_EQ(local_routes["POST:/tasks/cancel"]("/tasks/cancel", MakeTaskIdReq(task_id), rsp), error::OK);
        ASSERT_TRUE(rsp.find("cancelled") != std::string::npos || rsp.find("cancelling") != std::string::npos);

        bool cancelled = false;
        for (int i = 0; i < 120; ++i) {
            ASSERT_EQ(local_routes["POST:/tasks/detail"]("/tasks/detail", MakeTaskIdReq(task_id), rsp), error::OK);
            rapidjson::Document detail;
            detail.Parse(rsp.c_str());
            ASSERT_TRUE(!detail.HasParseError() && detail.IsObject());
            if (std::string(detail["status"].GetString()) == "cancelled") {
                ASSERT_EQ(std::string(detail["error_code"].GetString()), "CANCELLED");
                cancelled = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        ASSERT_TRUE(cancelled);

        ASSERT_EQ(local_routes["POST:/tasks/delete"]("/tasks/delete", MakeTaskIdReq(task_id), rsp), error::OK);
        ASSERT_EQ(p.Stop(), 0);
    }

    {
        const std::string cancel_running_dir = MakeTempDir("cancel_running");
        std::atomic<int> exec_count{0};

        MockRouterHandle scheduler({
            MakeSqlClassifyRoute(),
            {"POST", "/scheduler/batch/execute",
             [&exec_count](const std::string&, const std::string&, std::string& rsp) {
                 exec_count.fetch_add(1);
                 std::this_thread::sleep_for(std::chrono::milliseconds(280));
                 rsp = R"({"status":"completed","result_row_count":1,"result_col_count":1,"result_target":"dataframe.tmp","data":[]})";
                 return error::OK;
             }},
        });
        MockQuerier querier;
        querier.AddHandle(&scheduler);

        TaskPlugin p;
        const std::string opt = "db_dir=" + cancel_running_dir + ";worker_threads=1";
        ASSERT_EQ(p.Option(opt.c_str()), 0);
        ASSERT_EQ(p.Load(&querier), 0);
        ASSERT_EQ(p.Start(), 0);

        auto local_routes = CollectRoutes(&p);

        std::string rsp;
        ASSERT_EQ(local_routes["POST:/tasks/batch/execute"](
                      "/tasks/batch/execute",
                      R"({"mode":"async","sql_text":"SELECT * FROM sqlite.local.src INTO dataframe.tmp;SELECT * FROM dataframe.tmp INTO dataframe.out;"})",
                      rsp),
                  error::OK);
        rapidjson::Document submit;
        submit.Parse(rsp.c_str());
        ASSERT_TRUE(!submit.HasParseError() && submit.IsObject());
        const std::string task_id = submit["task_id"].GetString();

        bool saw_running = false;
        for (int i = 0; i < 80; ++i) {
            ASSERT_EQ(local_routes["POST:/tasks/detail"]("/tasks/detail", MakeTaskIdReq(task_id), rsp), error::OK);
            rapidjson::Document detail;
            detail.Parse(rsp.c_str());
            ASSERT_TRUE(!detail.HasParseError() && detail.IsObject());
            if (std::string(detail["status"].GetString()) == "running") {
                saw_running = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        ASSERT_TRUE(saw_running);

        // running 状态早于首条 SQL 真正调用，先等待首条执行启动，避免取消竞态导致 exec_count=0。
        bool first_sql_started = false;
        for (int i = 0; i < 80; ++i) {
            if (exec_count.load() >= 1) {
                first_sql_started = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        ASSERT_TRUE(first_sql_started);

        ASSERT_EQ(local_routes["POST:/tasks/cancel"]("/tasks/cancel", MakeTaskIdReq(task_id), rsp), error::OK);
        ASSERT_TRUE(rsp.find("cancelling") != std::string::npos);

        bool cancelled = false;
        for (int i = 0; i < 120; ++i) {
            ASSERT_EQ(local_routes["POST:/tasks/detail"]("/tasks/detail", MakeTaskIdReq(task_id), rsp), error::OK);
            rapidjson::Document detail;
            detail.Parse(rsp.c_str());
            ASSERT_TRUE(!detail.HasParseError() && detail.IsObject());
            if (std::string(detail["status"].GetString()) == "cancelled") {
                cancelled = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        ASSERT_TRUE(cancelled);
        ASSERT_EQ(exec_count.load(), 1);

        ASSERT_EQ(local_routes["POST:/tasks/delete"]("/tasks/delete", MakeTaskIdReq(task_id), rsp), error::OK);
        ASSERT_EQ(p.Stop(), 0);
    }

    {
        const std::string timeout_dir = MakeTempDir("timeout_task");
        MockRouterHandle scheduler({
            MakeSqlClassifyRoute(),
            {"POST", "/scheduler/batch/execute",
             [](const std::string&, const std::string&, std::string& rsp) {
                 std::this_thread::sleep_for(std::chrono::milliseconds(1500));
                 rsp = R"({"status":"completed","result_row_count":1,"result_col_count":1,"result_target":"dataframe.timeout","data":[]})";
                 return error::OK;
             }},
        });
        MockQuerier querier;
        querier.AddHandle(&scheduler);

        TaskPlugin p;
        const std::string opt = "db_dir=" + timeout_dir + ";worker_threads=1";
        ASSERT_EQ(p.Option(opt.c_str()), 0);
        ASSERT_EQ(p.Load(&querier), 0);
        ASSERT_EQ(p.Start(), 0);

        auto local_routes = CollectRoutes(&p);

        std::string rsp;
        ASSERT_EQ(local_routes["POST:/tasks/batch/execute"](
                      "/tasks/batch/execute",
                      R"({"sql_text":"SELECT 1","mode":"async","timeout_s":1})",
                      rsp),
                  error::OK);
        rapidjson::Document submit;
        submit.Parse(rsp.c_str());
        ASSERT_TRUE(!submit.HasParseError() && submit.IsObject());
        const std::string task_id = submit["task_id"].GetString();

        bool timed_out = false;
        for (int i = 0; i < 200; ++i) {
            ASSERT_EQ(local_routes["POST:/tasks/detail"]("/tasks/detail", MakeTaskIdReq(task_id), rsp), error::OK);
            rapidjson::Document detail;
            detail.Parse(rsp.c_str());
            ASSERT_TRUE(!detail.HasParseError() && detail.IsObject());
            const std::string status = detail["status"].GetString();
            if (status == "timeout") {
                ASSERT_EQ(std::string(detail["error_code"].GetString()), "TIMEOUT");
                ASSERT_EQ(std::string(detail["error_stage"].GetString()), "timeout");
                timed_out = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        ASSERT_TRUE(timed_out);

        ASSERT_EQ(local_routes["POST:/tasks/delete"]("/tasks/delete", MakeTaskIdReq(task_id), rsp), error::OK);
        ASSERT_EQ(p.Stop(), 0);
    }

    {
        const std::string stream_dir = MakeTempDir("stream_routes");
        std::atomic<int> stream_execute_calls{0};
        std::atomic<int> stream_status_calls{0};
        std::atomic<int> stream_stop_calls{0};
        std::atomic<bool> stream_stopped{false};

        MockRouterHandle scheduler({
            MakeSqlClassifyRoute(),
            {"POST", "/scheduler/stream/execute",
             [&stream_execute_calls](const std::string&, const std::string& req, std::string& rsp) {
                 rapidjson::Document d;
                 d.Parse(req.c_str());
                 ASSERT_TRUE(!d.HasParseError() && d.IsObject());
                 ASSERT_TRUE(d.HasMember("execution_kind") && d["execution_kind"].IsString());
                 ASSERT_EQ(std::string(d["execution_kind"].GetString()), "single");
                 ASSERT_TRUE(d.HasMember("sql_text") && d["sql_text"].IsString());
                 stream_execute_calls.fetch_add(1);
                 rsp = R"({"runtime_task_id":"stream_task_1001"})";
                 return error::OK;
             }},
            {"POST", "/scheduler/stream/status",
             [&stream_status_calls, &stream_stopped](const std::string&, const std::string& req, std::string& rsp) {
                 rapidjson::Document d;
                 d.Parse(req.c_str());
                 ASSERT_TRUE(!d.HasParseError() && d.IsObject());
                 ASSERT_TRUE(d.HasMember("task_id") && d["task_id"].IsString());
                 ASSERT_EQ(std::string(d["task_id"].GetString()), "stream_task_1001");
                 stream_status_calls.fetch_add(1);
                 if (stream_stopped.load()) {
                     rsp = R"({
                         "task_id":"stream_task_1001",
                         "status":"stopped",
                         "processed_rows":12,
                         "output_rows":8,
                         "shared_hub_id":"hub_stream_1001",
                         "shared_source_keys":["ring.tcp_src"],
                         "subscriber_count":2,
                         "subscriber_stats":[
                             {
                                 "subscriber_id":"sub1",
                                 "runtime_task_id":"stream_task_1001",
                                 "logical_node_id":"",
                                 "active":true,
                                 "ready":true,
                                 "delivered_batches":12,
                                 "delivered_rows":12,
                                 "dropped_batches":3,
                                 "dropped_rows":3,
                                 "last_delivered_seq":12,
                                 "last_dropped_seq":15,
                                 "lag":0
                             },
                             {
                                 "subscriber_id":"sub2",
                                 "runtime_task_id":"stream_task_1002",
                                 "logical_node_id":"",
                                 "active":true,
                                 "ready":true,
                                 "delivered_batches":12,
                                 "delivered_rows":12,
                                 "dropped_batches":1,
                                 "dropped_rows":1,
                                 "last_delivered_seq":12,
                                 "last_dropped_seq":13,
                                 "lag":0
                             }
                         ],
                         "op_stats":{"shards":2}
                     })";
                 } else {
                     rsp = R"({
                         "task_id":"stream_task_1001",
                         "status":"running",
                         "processed_rows":12,
                         "output_rows":8,
                         "shared_hub_id":"hub_stream_1001",
                         "shared_source_keys":["ring.tcp_src"],
                         "subscriber_count":2,
                         "subscriber_stats":[
                             {
                                 "subscriber_id":"sub1",
                                 "runtime_task_id":"stream_task_1001",
                                 "logical_node_id":"",
                                 "active":true,
                                 "ready":true,
                                 "delivered_batches":12,
                                 "delivered_rows":12,
                                 "dropped_batches":3,
                                 "dropped_rows":3,
                                 "last_delivered_seq":12,
                                 "last_dropped_seq":15,
                                 "lag":4
                             },
                             {
                                 "subscriber_id":"sub2",
                                 "runtime_task_id":"stream_task_1002",
                                 "logical_node_id":"",
                                 "active":true,
                                 "ready":true,
                                 "delivered_batches":12,
                                 "delivered_rows":12,
                                 "dropped_batches":1,
                                 "dropped_rows":1,
                                 "last_delivered_seq":12,
                                 "last_dropped_seq":13,
                                 "lag":2
                             }
                         ],
                         "op_stats":{"shards":2}
                     })";
                 }
                 return error::OK;
             }},
            {"POST", "/scheduler/stream/stop",
             [&stream_stop_calls, &stream_stopped](const std::string&, const std::string& req, std::string& rsp) {
                 rapidjson::Document d;
                 d.Parse(req.c_str());
                 ASSERT_TRUE(!d.HasParseError() && d.IsObject());
                 ASSERT_TRUE(d.HasMember("task_id") && d["task_id"].IsString());
                 ASSERT_EQ(std::string(d["task_id"].GetString()), "stream_task_1001");
                 stream_stop_calls.fetch_add(1);
                 stream_stopped.store(true);
                 rsp = R"({"task_id":"stream_task_1001","status":"stopped"})";
                 return error::OK;
             }},
        });
        MockQuerier querier;
        querier.AddHandle(&scheduler);

        TaskPlugin p;
        const std::string opt = "db_dir=" + stream_dir + ";disable_worker=1";
        ASSERT_EQ(p.Option(opt.c_str()), 0);
        ASSERT_EQ(p.Load(&querier), 0);
        ASSERT_EQ(p.Start(), 0);

        auto local_routes = CollectRoutes(&p);
        ASSERT_TRUE(local_routes.count("POST:/tasks/stream/execute") == 1);
        ASSERT_TRUE(local_routes.count("POST:/tasks/stream/stop") == 1);
        ASSERT_TRUE(local_routes.count("POST:/tasks/stream/status") == 1);
        ASSERT_TRUE(local_routes.count("POST:/tasks/stream/list") == 1);

        std::string rsp;

        // 非法 timeout 参数应在调度前拦截。
        ASSERT_EQ(local_routes["POST:/tasks/stream/execute"](
                      "/tasks/stream/execute",
                      R"({"sql_text":"SELECT * FROM tcp_session_mock.tcp_src USING builtin.tcp_service_merge_stream INTO dataframe.svc","timeout_s":-1})",
                      rsp),
                  error::BAD_REQUEST);
        ASSERT_EQ(stream_execute_calls.load(), 0);

        ASSERT_EQ(local_routes["POST:/tasks/stream/execute"](
                      "/tasks/stream/execute",
                      R"({"sql_text":"SELECT * FROM tcp_session_mock.tcp_src USING builtin.tcp_service_merge_stream INTO dataframe.svc","timeout_s":30})",
                      rsp),
                  error::OK);
        rapidjson::Document exec_ret;
        exec_ret.Parse(rsp.c_str());
        ASSERT_TRUE(!exec_ret.HasParseError() && exec_ret.IsObject());
        ASSERT_TRUE(exec_ret.HasMember("task_id") && exec_ret["task_id"].IsString());
        ASSERT_TRUE(exec_ret.HasMember("runtime_task_id") && exec_ret["runtime_task_id"].IsString());
        ASSERT_TRUE(exec_ret.HasMember("status") && exec_ret["status"].IsString());
        ASSERT_EQ(std::string(exec_ret["runtime_task_id"].GetString()), "stream_task_1001");
        ASSERT_EQ(std::string(exec_ret["status"].GetString()), "submitted");
        const std::string local_stream_task_id = exec_ret["task_id"].GetString();

        ASSERT_EQ(local_routes["POST:/tasks/stream/list"]("/tasks/stream/list", "{}", rsp), error::OK);
        rapidjson::Document list_ret;
        list_ret.Parse(rsp.c_str());
        ASSERT_TRUE(!list_ret.HasParseError() && list_ret.IsObject());
        ASSERT_TRUE(list_ret.HasMember("tasks") && list_ret["tasks"].IsArray());
        ASSERT_EQ(list_ret["tasks"].Size(), rapidjson::SizeType(1));
        ASSERT_TRUE(list_ret["tasks"][0].HasMember("runtime_task_id"));
        ASSERT_EQ(std::string(list_ret["tasks"][0]["runtime_task_id"].GetString()), "stream_task_1001");
        ASSERT_TRUE(list_ret["tasks"][0].HasMember("shared_hub_id"));
        ASSERT_EQ(std::string(list_ret["tasks"][0]["shared_hub_id"].GetString()), "hub_stream_1001");
        ASSERT_TRUE(list_ret["tasks"][0].HasMember("shared_source_keys"));
        ASSERT_TRUE(list_ret["tasks"][0]["shared_source_keys"].IsArray());
        ASSERT_EQ(list_ret["tasks"][0]["shared_source_keys"].Size(), rapidjson::SizeType(1));
        ASSERT_TRUE(list_ret["tasks"][0].HasMember("subscriber_count"));
        ASSERT_EQ(list_ret["tasks"][0]["subscriber_count"].GetUint(), 2u);
        ASSERT_TRUE(list_ret["tasks"][0].HasMember("subscriber_stats"));
        ASSERT_TRUE(list_ret["tasks"][0]["subscriber_stats"].IsArray());
        ASSERT_EQ(list_ret["tasks"][0]["subscriber_stats"].Size(), rapidjson::SizeType(2));
        ASSERT_TRUE(list_ret["tasks"][0]["subscriber_stats"][0].HasMember("lag"));
        ASSERT_TRUE(list_ret["tasks"][0]["subscriber_stats"][0]["lag"].IsUint64());

        ASSERT_EQ(local_routes["POST:/tasks/stream/status"](
                      "/tasks/stream/status", MakeTaskIdReq(local_stream_task_id), rsp),
                  error::OK);
        rapidjson::Document status_ret_1;
        status_ret_1.Parse(rsp.c_str());
        ASSERT_TRUE(!status_ret_1.HasParseError() && status_ret_1.IsObject());
        ASSERT_EQ(std::string(status_ret_1["status"].GetString()), "running");
        ASSERT_EQ(std::string(status_ret_1["runtime_status"].GetString()), "running");
        ASSERT_EQ(status_ret_1["processed_rows"].GetInt64(), 12);
        ASSERT_EQ(status_ret_1["output_rows"].GetInt64(), 8);
        ASSERT_TRUE(status_ret_1.HasMember("shared_hub_id"));
        ASSERT_EQ(std::string(status_ret_1["shared_hub_id"].GetString()), "hub_stream_1001");
        ASSERT_TRUE(status_ret_1.HasMember("shared_source_keys"));
        ASSERT_TRUE(status_ret_1["shared_source_keys"].IsArray());
        ASSERT_EQ(status_ret_1["shared_source_keys"].Size(), rapidjson::SizeType(1));
        ASSERT_TRUE(status_ret_1.HasMember("subscriber_count"));
        ASSERT_EQ(status_ret_1["subscriber_count"].GetUint(), 2u);
        ASSERT_TRUE(status_ret_1.HasMember("subscriber_stats"));
        ASSERT_TRUE(status_ret_1["subscriber_stats"].IsArray());
        ASSERT_EQ(status_ret_1["subscriber_stats"].Size(), rapidjson::SizeType(2));
        ASSERT_TRUE(status_ret_1["subscriber_stats"][0].HasMember("lag"));
        ASSERT_TRUE(status_ret_1["subscriber_stats"][0]["lag"].IsUint64());
        ASSERT_EQ(status_ret_1["subscriber_stats"][0]["lag"].GetUint64(), 4u);

        ASSERT_EQ(local_routes["POST:/tasks/stream/stop"](
                      "/tasks/stream/stop", MakeTaskIdReq(local_stream_task_id), rsp),
                  error::OK);
        rapidjson::Document stop_ret;
        stop_ret.Parse(rsp.c_str());
        ASSERT_TRUE(!stop_ret.HasParseError() && stop_ret.IsObject());
        ASSERT_EQ(std::string(stop_ret["status"].GetString()), "stopped");
        ASSERT_EQ(std::string(stop_ret["runtime_status"].GetString()), "stopped");

        ASSERT_EQ(local_routes["POST:/tasks/stream/status"](
                      "/tasks/stream/status", MakeTaskIdReq(local_stream_task_id), rsp),
                  error::OK);
        rapidjson::Document status_ret_2;
        status_ret_2.Parse(rsp.c_str());
        ASSERT_TRUE(!status_ret_2.HasParseError() && status_ret_2.IsObject());
        ASSERT_EQ(std::string(status_ret_2["status"].GetString()), "stopped");
        ASSERT_EQ(std::string(status_ret_2["runtime_status"].GetString()), "stopped");

        ASSERT_EQ(local_routes["POST:/tasks/cancel"](
                      "/tasks/cancel", MakeTaskIdReq(local_stream_task_id), rsp),
                  error::CONFLICT);

        ASSERT_EQ(stream_execute_calls.load(), 1);
        ASSERT_TRUE(stream_status_calls.load() >= 2);
        ASSERT_EQ(stream_stop_calls.load(), 1);

        ASSERT_EQ(local_routes["POST:/tasks/delete"](
                      "/tasks/delete", MakeTaskIdReq(local_stream_task_id), rsp),
                  error::OK);
        ASSERT_EQ(p.Stop(), 0);
    }

    {
        const std::string runtime_graph_dir = MakeTempDir("runtime_graph_routes");
        std::atomic<int> graph_query_calls{0};

        MockRouterHandle scheduler({
            MakeSqlClassifyRoute(),
            {"POST", "/scheduler/stream/execute",
             [](const std::string&, const std::string&, std::string& rsp) {
                 rsp = R"({"runtime_task_id":"stream_runtime_graph_001"})";
                 return error::OK;
             }},
            {"POST", "/scheduler/runtime/graph/query",
             [&graph_query_calls](const std::string&, const std::string& req, std::string& rsp) {
                 rapidjson::Document d;
                 d.Parse(req.c_str());
                 ASSERT_TRUE(!d.HasParseError() && d.IsObject());
                 ASSERT_TRUE(d.HasMember("task_id") && d["task_id"].IsString());
                 ASSERT_EQ(std::string(d["task_id"].GetString()), "stream_runtime_graph_001");
                 ASSERT_TRUE(d.HasMember("task_kind") && d["task_kind"].IsString());
                 ASSERT_EQ(std::string(d["task_kind"].GetString()), "stream");
                 ASSERT_TRUE(d.HasMember("sqls") && d["sqls"].IsArray());
                 ASSERT_EQ(d["sqls"].Size(), rapidjson::SizeType(1));
                 graph_query_calls.fetch_add(1);
                 rsp = R"({
                     "task_id":"stream_runtime_graph_001",
                     "runtime_task_id":"stream_runtime_graph_001",
                     "task_kind":"stream",
                     "runtime_kind":"single",
                     "status":"running",
                     "snapshot_time_ms":1000,
                     "nodes":[
                         {"id":"channel:ring.in","kind":"channel","name":"ring.in","sql_index":0,"status":"running","phase":"","processed_rows":10,"output_rows":10,"error_code":"","error_message":"","start_at_ms":0,"end_at_ms":0},
                         {"id":"operator:sql0","kind":"operator","name":"builtin.passthrough_stream","sql_index":0,"status":"running","phase":"on_data","processed_rows":10,"output_rows":10,"error_code":"","error_message":"","start_at_ms":0,"end_at_ms":0}
                     ],
                     "edges":[
                         {"id":"e1","from":"channel:ring.in","to":"operator:sql0","edge_kind":"data","trigger":"on_data","status":"active","rows":10,"fire_count":1,"last_fire_at_ms":1000}
                     ],
                     "events":[],
                     "next_cursor":0
                 })";
                 return error::OK;
             }},
        });
        MockQuerier querier;
        querier.AddHandle(&scheduler);

        TaskPlugin p;
        const std::string opt = "db_dir=" + runtime_graph_dir + ";disable_worker=1";
        ASSERT_EQ(p.Option(opt.c_str()), 0);
        ASSERT_EQ(p.Load(&querier), 0);
        ASSERT_EQ(p.Start(), 0);

        auto local_routes = CollectRoutes(&p);
        ASSERT_TRUE(local_routes.count("POST:/tasks/runtime/graph/query") == 1);

        std::string rsp;
        ASSERT_EQ(local_routes["POST:/tasks/stream/execute"](
                      "/tasks/stream/execute",
                      R"({"sql_text":"SELECT * FROM ring.in USING builtin.passthrough_stream INTO stream.out"})",
                      rsp),
                  error::OK);
        rapidjson::Document exec_doc;
        exec_doc.Parse(rsp.c_str());
        ASSERT_TRUE(!exec_doc.HasParseError() && exec_doc.IsObject());
        ASSERT_TRUE(exec_doc.HasMember("task_id") && exec_doc["task_id"].IsString());
        const std::string task_id = exec_doc["task_id"].GetString();

        ASSERT_EQ(local_routes["POST:/tasks/runtime/graph/query"](
                      "/tasks/runtime/graph/query",
                      (std::string("{\"task_id\":\"") + task_id + "\",\"cursor\":0,\"include_events\":true}"),
                      rsp),
                  error::OK);
        rapidjson::Document graph_doc;
        graph_doc.Parse(rsp.c_str());
        ASSERT_TRUE(!graph_doc.HasParseError() && graph_doc.IsObject());
        ASSERT_TRUE(graph_doc.HasMember("task_id") && graph_doc["task_id"].IsString());
        ASSERT_EQ(std::string(graph_doc["task_id"].GetString()), task_id);
        ASSERT_TRUE(graph_doc.HasMember("snapshot_time_ms") && graph_doc["snapshot_time_ms"].IsInt64());
        ASSERT_TRUE(!graph_doc.HasMember("version"));
        ASSERT_TRUE(!graph_doc.HasMember("generated_at_ms"));
        ASSERT_TRUE(graph_doc.HasMember("runtime_task_id") && graph_doc["runtime_task_id"].IsString());
        ASSERT_EQ(std::string(graph_doc["runtime_task_id"].GetString()), "stream_runtime_graph_001");
        ASSERT_TRUE(graph_doc.HasMember("graph_source") && graph_doc["graph_source"].IsString());
        ASSERT_EQ(std::string(graph_doc["graph_source"].GetString()), "live");
        ASSERT_TRUE(graph_doc.HasMember("degraded") && graph_doc["degraded"].IsBool());
        ASSERT_TRUE(!graph_doc["degraded"].GetBool());
        ASSERT_TRUE(graph_doc.HasMember("degrade_reason") && graph_doc["degrade_reason"].IsString());
        ASSERT_EQ(std::string(graph_doc["degrade_reason"].GetString()), "");
        ASSERT_TRUE(graph_doc.HasMember("sql_text") && graph_doc["sql_text"].IsString());
        ASSERT_TRUE(std::string(graph_doc["sql_text"].GetString()).find("SELECT * FROM ring.in") != std::string::npos);
        ASSERT_TRUE(graph_doc.HasMember("sqls") && graph_doc["sqls"].IsArray());
        ASSERT_EQ(graph_doc["sqls"].Size(), rapidjson::SizeType(1));
        ASSERT_TRUE(graph_doc.HasMember("nodes") && graph_doc["nodes"].IsArray());
        ASSERT_EQ(graph_doc["nodes"].Size(), rapidjson::SizeType(2));
        ASSERT_TRUE(graph_doc.HasMember("edges") && graph_doc["edges"].IsArray());
        ASSERT_EQ(graph_doc["edges"].Size(), rapidjson::SizeType(1));

        ASSERT_EQ(local_routes["POST:/tasks/runtime/graph/query"](
                      "/tasks/runtime/graph/query",
                      R"({"task_id":"not_exist"})",
                      rsp),
                  error::NOT_FOUND);
        rapidjson::Document not_found_doc;
        not_found_doc.Parse(rsp.c_str());
        ASSERT_TRUE(!not_found_doc.HasParseError() && not_found_doc.IsObject());
        ASSERT_TRUE(not_found_doc.HasMember("error_code") && not_found_doc["error_code"].IsString());
        ASSERT_EQ(std::string(not_found_doc["error_code"].GetString()), "RUNTIME_GRAPH_TASK_NOT_FOUND");

        ASSERT_EQ(local_routes["POST:/tasks/runtime/graph/query"](
                      "/tasks/runtime/graph/query",
                      (std::string("{\"task_id\":\"") + task_id + "\",\"cursor\":\"bad\"}"),
                      rsp),
                  error::BAD_REQUEST);
        rapidjson::Document bad_req_doc;
        bad_req_doc.Parse(rsp.c_str());
        ASSERT_TRUE(!bad_req_doc.HasParseError() && bad_req_doc.IsObject());
        ASSERT_TRUE(bad_req_doc.HasMember("error_code") && bad_req_doc["error_code"].IsString());
        ASSERT_EQ(std::string(bad_req_doc["error_code"].GetString()), "RUNTIME_GRAPH_CURSOR_INVALID");

        ASSERT_EQ(graph_query_calls.load(), 1);
        ASSERT_EQ(p.Stop(), 0);
    }

    {
        const std::string runtime_graph_not_found_dir = MakeTempDir("runtime_graph_not_found");
        std::atomic<int> graph_query_calls{0};

        MockRouterHandle scheduler({
            MakeSqlClassifyRoute(),
            {"POST", "/scheduler/stream/execute",
             [](const std::string&, const std::string&, std::string& rsp) {
                 rsp = R"({"runtime_task_id":"stream_runtime_graph_missing_001"})";
                 return error::OK;
             }},
            {"POST", "/scheduler/runtime/graph/query",
             [&graph_query_calls](const std::string&, const std::string&, std::string& rsp) {
                 graph_query_calls.fetch_add(1);
                 rsp = R"({"error":"runtime task not found"})";
                 return error::NOT_FOUND;
             }},
        });
        MockQuerier querier;
        querier.AddHandle(&scheduler);

        TaskPlugin p;
        const std::string opt = "db_dir=" + runtime_graph_not_found_dir + ";disable_worker=1";
        ASSERT_EQ(p.Option(opt.c_str()), 0);
        ASSERT_EQ(p.Load(&querier), 0);
        ASSERT_EQ(p.Start(), 0);

        auto local_routes = CollectRoutes(&p);
        ASSERT_TRUE(local_routes.count("POST:/tasks/runtime/graph/query") == 1);

        std::string rsp;
        ASSERT_EQ(local_routes["POST:/tasks/stream/execute"](
                      "/tasks/stream/execute",
                      R"({"sql_text":"SELECT * FROM ring.in USING builtin.passthrough_stream INTO stream.out"})",
                      rsp),
                  error::OK);
        rapidjson::Document exec_doc;
        exec_doc.Parse(rsp.c_str());
        ASSERT_TRUE(!exec_doc.HasParseError() && exec_doc.IsObject());
        ASSERT_TRUE(exec_doc.HasMember("task_id") && exec_doc["task_id"].IsString());
        const std::string task_id = exec_doc["task_id"].GetString();

        ASSERT_EQ(local_routes["POST:/tasks/runtime/graph/query"](
                      "/tasks/runtime/graph/query",
                      (std::string("{\"task_id\":\"") + task_id + "\"}"),
                      rsp),
                  error::OK);
        rapidjson::Document graph_doc;
        graph_doc.Parse(rsp.c_str());
        ASSERT_TRUE(!graph_doc.HasParseError() && graph_doc.IsObject());
        ASSERT_TRUE(graph_doc.HasMember("task_id") && graph_doc["task_id"].IsString());
        ASSERT_EQ(std::string(graph_doc["task_id"].GetString()), task_id);
        ASSERT_TRUE(graph_doc.HasMember("runtime_kind") && graph_doc["runtime_kind"].IsString());
        ASSERT_EQ(std::string(graph_doc["runtime_kind"].GetString()), "single");
        ASSERT_TRUE(graph_doc.HasMember("nodes") && graph_doc["nodes"].IsArray());
        ASSERT_TRUE(graph_doc["nodes"].Size() >= rapidjson::SizeType(2));
        ASSERT_TRUE(graph_doc.HasMember("edges") && graph_doc["edges"].IsArray());
        ASSERT_TRUE(graph_doc["edges"].Size() >= rapidjson::SizeType(2));
        ASSERT_TRUE(graph_doc.HasMember("degraded") && graph_doc["degraded"].IsBool());
        ASSERT_TRUE(graph_doc["degraded"].GetBool());
        ASSERT_TRUE(graph_doc.HasMember("degrade_reason") && graph_doc["degrade_reason"].IsString());
        ASSERT_EQ(std::string(graph_doc["degrade_reason"].GetString()), "runtime_not_found");
        ASSERT_TRUE(graph_doc.HasMember("graph_source") && graph_doc["graph_source"].IsString());
        ASSERT_EQ(std::string(graph_doc["graph_source"].GetString()), "reconstructed");
        ASSERT_TRUE(graph_doc.HasMember("sqls") && graph_doc["sqls"].IsArray());
        ASSERT_EQ(graph_doc["sqls"].Size(), rapidjson::SizeType(1));

        ASSERT_EQ(graph_query_calls.load(), 1);
        ASSERT_EQ(p.Stop(), 0);
    }

    {
        const std::string runtime_graph_chain_dir = MakeTempDir("runtime_graph_chain_edges");
        std::atomic<int> graph_query_calls{0};

        MockRouterHandle scheduler({
            MakeSqlClassifyRoute(),
            {"POST", "/scheduler/stream/execute",
             [](const std::string&, const std::string&, std::string& rsp) {
                 rsp = R"({"runtime_task_id":"stream_runtime_graph_chain_missing_001","runtime_kind":"group","group_mode":"dag","node_count":2})";
                 return error::OK;
             }},
            {"POST", "/scheduler/runtime/graph/query",
             [&graph_query_calls](const std::string&, const std::string&, std::string& rsp) {
                 graph_query_calls.fetch_add(1);
                 rsp = R"({"error":"runtime task not found"})";
                 return error::NOT_FOUND;
             }},
        });
        MockQuerier querier;
        querier.AddHandle(&scheduler);

        TaskPlugin p;
        const std::string opt = "db_dir=" + runtime_graph_chain_dir + ";disable_worker=1";
        ASSERT_EQ(p.Option(opt.c_str()), 0);
        ASSERT_EQ(p.Load(&querier), 0);
        ASSERT_EQ(p.Start(), 0);

        auto local_routes = CollectRoutes(&p);
        ASSERT_TRUE(local_routes.count("POST:/tasks/runtime/graph/query") == 1);

        std::string rsp;
        ASSERT_EQ(local_routes["POST:/tasks/stream/execute"](
                      "/tasks/stream/execute",
                      R"({"execution_kind":"group","group_mode":"dag","sql_text":"select * from dataframe.VNAT using builtin.dataframe_dispatch_stream into stream_hub.testHub2;select * from stream_hub.testHub2[*] USING builtin.passthrough_stream into dataframe.testall;"})",
                      rsp),
                  error::OK);
        rapidjson::Document exec_doc;
        exec_doc.Parse(rsp.c_str());
        ASSERT_TRUE(!exec_doc.HasParseError() && exec_doc.IsObject());
        ASSERT_TRUE(exec_doc.HasMember("task_id") && exec_doc["task_id"].IsString());
        const std::string task_id = exec_doc["task_id"].GetString();

        ASSERT_EQ(local_routes["POST:/tasks/runtime/graph/query"](
                      "/tasks/runtime/graph/query",
                      (std::string("{\"task_id\":\"") + task_id + "\"}"),
                      rsp),
                  error::OK);
        rapidjson::Document graph_doc;
        graph_doc.Parse(rsp.c_str());
        ASSERT_TRUE(!graph_doc.HasParseError() && graph_doc.IsObject());
        ASSERT_TRUE(graph_doc.HasMember("degraded") && graph_doc["degraded"].IsBool());
        ASSERT_TRUE(graph_doc["degraded"].GetBool());
        ASSERT_TRUE(graph_doc.HasMember("snapshot_time_ms") && graph_doc["snapshot_time_ms"].IsInt64());
        ASSERT_TRUE(!graph_doc.HasMember("version"));
        ASSERT_TRUE(!graph_doc.HasMember("generated_at_ms"));
        ASSERT_TRUE(graph_doc.HasMember("edges") && graph_doc["edges"].IsArray());
        ASSERT_EQ(graph_doc["edges"].Size(), rapidjson::SizeType(5));
        bool has_on_finish = false;
        for (const auto& edge : graph_doc["edges"].GetArray()) {
            ASSERT_TRUE(edge.IsObject());
            ASSERT_TRUE(edge.HasMember("edge_kind") && edge["edge_kind"].IsString());
            ASSERT_TRUE(edge.HasMember("trigger") && edge["trigger"].IsString());
            const std::string edge_kind = edge["edge_kind"].GetString();
            const std::string trigger = edge["trigger"].GetString();
            if (edge_kind == "control" && trigger == "on_finish") {
                has_on_finish = true;
                ASSERT_TRUE(edge.HasMember("from") && edge["from"].IsString());
                ASSERT_TRUE(edge.HasMember("to") && edge["to"].IsString());
                ASSERT_EQ(std::string(edge["from"].GetString()), "operator:sql0");
                ASSERT_EQ(std::string(edge["to"].GetString()), "operator:sql1");
            }
        }
        ASSERT_TRUE(has_on_finish);

        ASSERT_TRUE(graph_doc.HasMember("nodes") && graph_doc["nodes"].IsArray());
        bool has_hub_base = false;
        bool has_hub_wildcard = false;
        for (const auto& node : graph_doc["nodes"].GetArray()) {
            if (!node.IsObject()) continue;
            if (!node.HasMember("name") || !node["name"].IsString()) continue;
            const std::string name = node["name"].GetString();
            if (name == "stream_hub.testHub2") {
                has_hub_base = true;
                ASSERT_TRUE(node.HasMember("sql_index") && node["sql_index"].IsInt());
                ASSERT_EQ(node["sql_index"].GetInt(), 0);
            }
            if (name == "stream_hub.testHub2[*]") {
                has_hub_wildcard = true;
            }
        }
        ASSERT_TRUE(has_hub_base);
        ASSERT_TRUE(!has_hub_wildcard);

        ASSERT_EQ(graph_query_calls.load(), 1);
        ASSERT_EQ(p.Stop(), 0);
    }

    {
        const std::string sql_payload_guard_dir = MakeTempDir("sql_payload_guard");
        const std::string sql_payload_db = sql_payload_guard_dir + "/task_store.db";

        MockRouterHandle scheduler({
            MakeSqlClassifyRoute(),
            {"POST", "/scheduler/stream/execute",
             [](const std::string&, const std::string&, std::string& rsp) {
                 rsp = R"({"runtime_task_id":"stream_payload_guard_001","runtime_kind":"group","group_mode":"dag","node_count":2})";
                 return error::OK;
             }},
        });
        MockQuerier querier;
        querier.AddHandle(&scheduler);

        TaskPlugin p;
        const std::string opt = "db_dir=" + sql_payload_guard_dir + ";disable_worker=1";
        ASSERT_EQ(p.Option(opt.c_str()), 0);
        ASSERT_EQ(p.Load(&querier), 0);
        ASSERT_EQ(p.Start(), 0);

        auto local_routes = CollectRoutes(&p);
        std::string rsp;
        ASSERT_EQ(local_routes["POST:/tasks/stream/execute"](
                      "/tasks/stream/execute",
                      R"({"execution_kind":"group","group_mode":"dag","sql_text":"SELECT * FROM ring.in USING builtin.passthrough_stream INTO stream.mid;SELECT * FROM stream.mid USING builtin.passthrough_stream INTO stream.out;"})",
                      rsp),
                  error::OK);
        rapidjson::Document exec_doc;
        exec_doc.Parse(rsp.c_str());
        ASSERT_TRUE(!exec_doc.HasParseError() && exec_doc.IsObject());
        ASSERT_TRUE(exec_doc.HasMember("task_id") && exec_doc["task_id"].IsString());
        const std::string task_id = exec_doc["task_id"].GetString();
        ASSERT_TRUE(DeleteTaskSqlPayload(sql_payload_db, task_id));

        ASSERT_EQ(local_routes["POST:/tasks/detail"](
                      "/tasks/detail", MakeTaskIdReq(task_id), rsp),
                  error::INTERNAL_ERROR);
        rapidjson::Document detail_doc;
        detail_doc.Parse(rsp.c_str());
        ASSERT_TRUE(!detail_doc.HasParseError() && detail_doc.IsObject());
        ASSERT_TRUE(detail_doc.HasMember("error_code") && detail_doc["error_code"].IsString());
        ASSERT_EQ(std::string(detail_doc["error_code"].GetString()), "TASK_SQL_PAYLOAD_INVALID");

        ASSERT_EQ(local_routes["POST:/tasks/runtime/graph/query"](
                      "/tasks/runtime/graph/query",
                      (std::string("{\"task_id\":\"") + task_id + "\"}"),
                      rsp),
                  error::INTERNAL_ERROR);
        rapidjson::Document graph_doc;
        graph_doc.Parse(rsp.c_str());
        ASSERT_TRUE(!graph_doc.HasParseError() && graph_doc.IsObject());
        ASSERT_TRUE(graph_doc.HasMember("error_code") && graph_doc["error_code"].IsString());
        ASSERT_EQ(std::string(graph_doc["error_code"].GetString()), "RUNTIME_GRAPH_SQL_PAYLOAD_INVALID");

        ASSERT_EQ(local_routes["POST:/tasks/list"]("/tasks/list", "{}", rsp), error::INTERNAL_ERROR);
        rapidjson::Document list_doc;
        list_doc.Parse(rsp.c_str());
        ASSERT_TRUE(!list_doc.HasParseError() && list_doc.IsObject());
        ASSERT_TRUE(list_doc.HasMember("error_code") && list_doc["error_code"].IsString());
        ASSERT_EQ(std::string(list_doc["error_code"].GetString()), "TASK_SQL_PAYLOAD_INVALID");

        ASSERT_EQ(p.Stop(), 0);
    }

    {
        const std::string classify_dir = MakeTempDir("classify_routes");
        std::atomic<int> classify_calls{0};
        std::atomic<int> batch_execute_calls{0};
        std::atomic<int> stream_execute_calls{0};

        MockRouterHandle scheduler({
            MakeSqlClassifyRoute(),
            {"POST", "/scheduler/sql/classify",
             [&classify_calls](const std::string&, const std::string& req, std::string& rsp) {
                 rapidjson::Document d;
                 d.Parse(req.c_str());
                 ASSERT_TRUE(!d.HasParseError() && d.IsObject());
                 ASSERT_TRUE(d.HasMember("sql") && d["sql"].IsString());
                 const std::string sql = d["sql"].GetString();
                 classify_calls.fetch_add(1);
                 if (sql.find("tcp_session_mock.tcp_src") != std::string::npos) {
                     rsp = R"({"task_kind":"stream"})";
                 } else {
                     rsp = R"({"task_kind":"batch"})";
                 }
                 return error::OK;
             }},
            {"POST", "/scheduler/batch/execute",
             [&batch_execute_calls](const std::string&, const std::string& req, std::string& rsp) {
                 rapidjson::Document d;
                 d.Parse(req.c_str());
                 ASSERT_TRUE(!d.HasParseError() && d.IsObject());
                 ASSERT_TRUE(d.HasMember("sql") && d["sql"].IsString());
                 batch_execute_calls.fetch_add(1);
                 rsp = R"({"status":"completed","rows":0,"result_row_count":0,"result_target":"dataframe.tmp","data":[]})";
                 return error::OK;
             }},
            {"POST", "/scheduler/stream/execute",
             [&stream_execute_calls](const std::string&, const std::string& req, std::string& rsp) {
                 rapidjson::Document d;
                 d.Parse(req.c_str());
                 ASSERT_TRUE(!d.HasParseError() && d.IsObject());
                 ASSERT_TRUE(d.HasMember("execution_kind") && d["execution_kind"].IsString());
                 ASSERT_EQ(std::string(d["execution_kind"].GetString()), "single");
                 ASSERT_TRUE(d.HasMember("sql_text") && d["sql_text"].IsString());
                 stream_execute_calls.fetch_add(1);
                 rsp = R"({"runtime_task_id":"stream_task_classify_001"})";
                 return error::OK;
             }},
            {"POST", "/scheduler/stream/stop",
             [](const std::string&, const std::string&, std::string& rsp) {
                 rsp = R"({"task_id":"stream_task_classify_001","status":"stopped"})";
                 return error::OK;
             }},
        });
        MockQuerier querier;
        querier.AddHandle(&scheduler);

        TaskPlugin p;
        const std::string opt = "db_dir=" + classify_dir + ";disable_worker=1";
        ASSERT_EQ(p.Option(opt.c_str()), 0);
        ASSERT_EQ(p.Load(&querier), 0);
        ASSERT_EQ(p.Start(), 0);

        auto local_routes = CollectRoutes(&p);
        ASSERT_TRUE(local_routes.count("POST:/tasks/sql/classify") == 1);
        ASSERT_TRUE(local_routes.count("POST:/tasks/batch/execute") == 1);
        ASSERT_TRUE(local_routes.count("POST:/tasks/stream/execute") == 1);

        std::string rsp;

        ASSERT_EQ(local_routes["POST:/tasks/sql/classify"](
                      "/tasks/sql/classify",
                      R"({"sql":"SELECT * FROM sqlite.local.src INTO dataframe.tmp"})",
                      rsp),
                  error::OK);
        rapidjson::Document classify_batch;
        classify_batch.Parse(rsp.c_str());
        ASSERT_TRUE(!classify_batch.HasParseError() && classify_batch.IsObject());
        ASSERT_TRUE(classify_batch.HasMember("task_kind") && classify_batch["task_kind"].IsString());
        ASSERT_EQ(std::string(classify_batch["task_kind"].GetString()), "batch");

        ASSERT_EQ(local_routes["POST:/tasks/sql/classify"](
                      "/tasks/sql/classify",
                      R"({"sql":"SELECT * FROM tcp_session_mock.tcp_src USING builtin.tcp_service_merge_stream INTO dataframe.svc"})",
                      rsp),
                  error::OK);
        rapidjson::Document classify_stream;
        classify_stream.Parse(rsp.c_str());
        ASSERT_TRUE(!classify_stream.HasParseError() && classify_stream.IsObject());
        ASSERT_TRUE(classify_stream.HasMember("task_kind") && classify_stream["task_kind"].IsString());
        ASSERT_EQ(std::string(classify_stream["task_kind"].GetString()), "stream");

        ASSERT_EQ(local_routes["POST:/tasks/batch/execute"](
                      "/tasks/batch/execute",
                      R"({"sql_text":"SELECT * FROM tcp_session_mock.tcp_src USING builtin.tcp_service_merge_stream INTO dataframe.svc","mode":"async"})",
                      rsp),
                  error::BAD_REQUEST);
        rapidjson::Document batch_err;
        batch_err.Parse(rsp.c_str());
        ASSERT_TRUE(!batch_err.HasParseError() && batch_err.IsObject());
        ASSERT_TRUE(batch_err.HasMember("error_code") && batch_err["error_code"].IsString());
        ASSERT_EQ(std::string(batch_err["error_code"].GetString()), "STREAM_SQL_USE_STREAM_API");
        ASSERT_EQ(batch_execute_calls.load(), 0);

        ASSERT_EQ(local_routes["POST:/tasks/stream/execute"](
                      "/tasks/stream/execute",
                      R"({"sql_text":"SELECT * FROM sqlite.local.src INTO dataframe.tmp"})",
                      rsp),
                  error::BAD_REQUEST);
        rapidjson::Document stream_err;
        stream_err.Parse(rsp.c_str());
        ASSERT_TRUE(!stream_err.HasParseError() && stream_err.IsObject());
        ASSERT_TRUE(stream_err.HasMember("error_code") && stream_err["error_code"].IsString());
        ASSERT_EQ(std::string(stream_err["error_code"].GetString()), "BATCH_SQL_USE_BATCH_API");
        ASSERT_EQ(stream_execute_calls.load(), 0);

        ASSERT_EQ(local_routes["POST:/tasks/batch/execute"](
                      "/tasks/batch/execute",
                      R"({"sql_text":"SELECT * FROM sqlite.local.src INTO dataframe.tmp","mode":"sync"})",
                      rsp),
                  error::OK);
        ASSERT_EQ(batch_execute_calls.load(), 1);

        ASSERT_EQ(local_routes["POST:/tasks/batch/execute"](
                      "/tasks/batch/execute",
                      R"({"sql":"SELECT * FROM sqlite.local.src INTO dataframe.tmp","mode":"sync"})",
                      rsp),
                  error::BAD_REQUEST);
        ASSERT_TRUE(rsp.find("sql_text") != std::string::npos);

        ASSERT_EQ(local_routes["POST:/tasks/batch/execute"](
                      "/tasks/batch/execute",
                      R"({"sqls":["SELECT * FROM sqlite.local.src INTO dataframe.tmp"],"mode":"sync"})",
                      rsp),
                  error::BAD_REQUEST);
        ASSERT_TRUE(rsp.find("sql_text") != std::string::npos);

        ASSERT_EQ(local_routes["POST:/tasks/stream/execute"](
                      "/tasks/stream/execute",
                      R"({"sql_text":"SELECT * FROM tcp_session_mock.tcp_src USING builtin.tcp_service_merge_stream INTO dataframe.svc"})",
                      rsp),
                  error::OK);
        ASSERT_EQ(stream_execute_calls.load(), 1);
        ASSERT_TRUE(classify_calls.load() >= 4);

        ASSERT_EQ(p.Stop(), 0);
    }

    {
        const std::string stream_errcode_dir = MakeTempDir("stream_errcode_routes");
        std::atomic<int> stream_execute_calls{0};
        std::atomic<int> stream_status_calls{0};
        std::atomic<int> stream_stop_calls{0};

        MockRouterHandle scheduler({
            {"POST", "/scheduler/sql/classify",
             [](const std::string&, const std::string&, std::string& rsp) {
                 rsp = R"({"task_kind":"stream"})";
                 return error::OK;
             }},
            {"POST", "/scheduler/stream/execute",
             [&stream_execute_calls](const std::string&, const std::string&, std::string& rsp) {
                 stream_execute_calls.fetch_add(1);
                 rsp = R"({"runtime_task_id":"stream_task_errcode_1"})";
                 return error::OK;
             }},
            {"POST", "/scheduler/stream/status",
             [&stream_status_calls](const std::string&, const std::string& req, std::string& rsp) {
                 rapidjson::Document d;
                 d.Parse(req.c_str());
                 ASSERT_TRUE(!d.HasParseError() && d.IsObject());
                 ASSERT_TRUE(d.HasMember("task_id") && d["task_id"].IsString());
                 ASSERT_EQ(std::string(d["task_id"].GetString()), "stream_task_errcode_1");
                 stream_status_calls.fetch_add(1);
                 rsp = R"({
                     "task_id":"stream_task_errcode_1",
                     "runtime_task_id":"stream_task_errcode_1",
                     "status":"failed",
                     "error_code":"STREAM_GROUP_TIMEOUT",
                     "error_message":"stream group timeout: 1s, unfinished_nodes=n1"
                 })";
                 return error::OK;
             }},
            {"POST", "/scheduler/stream/stop",
             [&stream_stop_calls](const std::string&, const std::string& req, std::string& rsp) {
                 rapidjson::Document d;
                 d.Parse(req.c_str());
                 ASSERT_TRUE(!d.HasParseError() && d.IsObject());
                 ASSERT_TRUE(d.HasMember("task_id") && d["task_id"].IsString());
                 ASSERT_EQ(std::string(d["task_id"].GetString()), "stream_task_errcode_1");
                 stream_stop_calls.fetch_add(1);
                 rsp = R"({
                     "task_id":"stream_task_errcode_1",
                     "status":"failed",
                     "error_code":"STREAM_GROUP_TIMEOUT",
                     "error_message":"stream group timeout: 1s, unfinished_nodes=n1"
                 })";
                 return error::OK;
             }},
        });
        MockQuerier querier;
        querier.AddHandle(&scheduler);

        TaskPlugin p;
        const std::string opt = "db_dir=" + stream_errcode_dir + ";disable_worker=1";
        ASSERT_EQ(p.Option(opt.c_str()), 0);
        ASSERT_EQ(p.Load(&querier), 0);
        ASSERT_EQ(p.Start(), 0);

        auto local_routes = CollectRoutes(&p);

        std::string rsp;
        ASSERT_EQ(local_routes["POST:/tasks/stream/execute"](
                      "/tasks/stream/execute",
                      R"({"sql_text":"SELECT * FROM tcp_session_mock.tcp_src USING builtin.tcp_service_merge_stream INTO dataframe.svc"})",
                      rsp),
                  error::OK);
        rapidjson::Document exec_doc;
        exec_doc.Parse(rsp.c_str());
        ASSERT_TRUE(!exec_doc.HasParseError() && exec_doc.IsObject());
        ASSERT_TRUE(exec_doc.HasMember("task_id") && exec_doc["task_id"].IsString());
        const std::string task_id = exec_doc["task_id"].GetString();

        ASSERT_EQ(local_routes["POST:/tasks/stream/status"](
                      "/tasks/stream/status", MakeTaskIdReq(task_id), rsp),
                  error::OK);
        rapidjson::Document status_doc;
        status_doc.Parse(rsp.c_str());
        ASSERT_TRUE(!status_doc.HasParseError() && status_doc.IsObject());
        ASSERT_EQ(std::string(status_doc["status"].GetString()), "failed");
        ASSERT_TRUE(status_doc.HasMember("error_code") && status_doc["error_code"].IsString());
        ASSERT_EQ(std::string(status_doc["error_code"].GetString()), "STREAM_GROUP_TIMEOUT");

        ASSERT_EQ(local_routes["POST:/tasks/stream/stop"](
                      "/tasks/stream/stop", MakeTaskIdReq(task_id), rsp),
                  error::OK);
        rapidjson::Document stop_doc;
        stop_doc.Parse(rsp.c_str());
        ASSERT_TRUE(!stop_doc.HasParseError() && stop_doc.IsObject());
        ASSERT_EQ(std::string(stop_doc["status"].GetString()), "failed");
        ASSERT_TRUE(stop_doc.HasMember("error_code") && stop_doc["error_code"].IsString());
        ASSERT_EQ(std::string(stop_doc["error_code"].GetString()), "STREAM_GROUP_TIMEOUT");

        ASSERT_EQ(stream_execute_calls.load(), 1);
        ASSERT_TRUE(stream_status_calls.load() >= 1);
        ASSERT_EQ(stream_stop_calls.load(), 1);
        ASSERT_EQ(p.Stop(), 0);
    }

    {
        const std::string stream_group_dir = MakeTempDir("stream_group_routes");
        std::atomic<int> classify_calls{0};
        std::atomic<int> stream_execute_calls{0};
        std::atomic<int> stream_status_calls{0};
        std::atomic<int> stream_stop_calls{0};

        MockRouterHandle scheduler({
            {"POST", "/scheduler/sql/classify",
             [&classify_calls](const std::string&, const std::string&, std::string& rsp) {
                 classify_calls.fetch_add(1);
                 rsp = R"({"task_kind":"stream"})";
                 return error::OK;
             }},
            {"POST", "/scheduler/stream/execute",
             [&stream_execute_calls](const std::string&, const std::string& req, std::string& rsp) {
                 rapidjson::Document d;
                 d.Parse(req.c_str());
                 ASSERT_TRUE(!d.HasParseError() && d.IsObject());
                 ASSERT_TRUE(d.HasMember("execution_kind") && d["execution_kind"].IsString());
                 ASSERT_EQ(std::string(d["execution_kind"].GetString()), "group");
                 ASSERT_TRUE(d.HasMember("group_mode") && d["group_mode"].IsString());
                 ASSERT_EQ(std::string(d["group_mode"].GetString()), "dag");
                 ASSERT_TRUE(d.HasMember("sql_text") && d["sql_text"].IsString());
                 ASSERT_TRUE(std::string(d["sql_text"].GetString()).find(';') != std::string::npos);
                 stream_execute_calls.fetch_add(1);
                 rsp = R"({
                     "runtime_task_id":"stream_group_2001",
                     "runtime_kind":"group",
                     "group_mode":"dag",
                     "node_count":2,
                     "share_set_count":0
                 })";
                 return error::OK;
             }},
            {"POST", "/scheduler/stream/status",
             [&stream_status_calls](const std::string&, const std::string& req, std::string& rsp) {
                 rapidjson::Document d;
                 d.Parse(req.c_str());
                 ASSERT_TRUE(!d.HasParseError() && d.IsObject());
                 ASSERT_TRUE(d.HasMember("task_id") && d["task_id"].IsString());
                 ASSERT_EQ(std::string(d["task_id"].GetString()), "stream_group_2001");
                 const int n = stream_status_calls.fetch_add(1);
                 if (n == 0) {
                     rsp = R"({
                         "task_id":"stream_group_2001",
                         "runtime_task_id":"stream_group_2001",
                         "runtime_kind":"group",
                         "group_mode":"dag",
                         "status":"running",
                         "nodes":[{"id":"n1","status":"running"},{"id":"n2","status":"pending"}],
                         "share_sets":[],
                         "resolved_sources":[]
                     })";
                 } else {
                     rsp = R"({
                         "task_id":"stream_group_2001",
                         "runtime_task_id":"stream_group_2001",
                         "runtime_kind":"group",
                         "group_mode":"dag",
                         "status":"stopped",
                         "nodes":[{"id":"n1","status":"stopped"},{"id":"n2","status":"stopped"}],
                         "share_sets":[],
                         "resolved_sources":[]
                     })";
                 }
                 return error::OK;
             }},
            {"POST", "/scheduler/stream/stop",
             [&stream_stop_calls](const std::string&, const std::string& req, std::string& rsp) {
                 rapidjson::Document d;
                 d.Parse(req.c_str());
                 ASSERT_TRUE(!d.HasParseError() && d.IsObject());
                 ASSERT_TRUE(d.HasMember("task_id") && d["task_id"].IsString());
                 ASSERT_EQ(std::string(d["task_id"].GetString()), "stream_group_2001");
                 stream_stop_calls.fetch_add(1);
                 rsp = R"({
                     "task_id":"stream_group_2001",
                     "runtime_task_id":"stream_group_2001",
                     "runtime_kind":"group",
                     "group_mode":"dag",
                     "status":"stopped"
                 })";
                 return error::OK;
             }},
        });

        MockQuerier querier;
        querier.AddHandle(&scheduler);

        TaskPlugin p;
        const std::string opt = "db_dir=" + stream_group_dir + ";disable_worker=1";
        ASSERT_EQ(p.Option(opt.c_str()), 0);
        ASSERT_EQ(p.Load(&querier), 0);
        ASSERT_EQ(p.Start(), 0);

        auto local_routes = CollectRoutes(&p);
        ASSERT_TRUE(local_routes.count("POST:/tasks/stream/execute") == 1);
        ASSERT_TRUE(local_routes.count("POST:/tasks/stream/status") == 1);
        ASSERT_TRUE(local_routes.count("POST:/tasks/stream/stop") == 1);

        std::string rsp;
        ASSERT_EQ(local_routes["POST:/tasks/stream/execute"](
                      "/tasks/stream/execute",
                      R"({
                          "execution_kind":"group",
                          "group_mode":"dag",
                          "timeout_s":30,
                          "sql_text":"SELECT * FROM ring.in USING builtin.passthrough_stream INTO stream.mid;SELECT * FROM stream.mid USING builtin.passthrough_stream INTO stream.out;"
                      })",
                      rsp),
                  error::OK);
        rapidjson::Document submit_doc;
        submit_doc.Parse(rsp.c_str());
        ASSERT_TRUE(!submit_doc.HasParseError() && submit_doc.IsObject());
        ASSERT_TRUE(submit_doc.HasMember("task_id") && submit_doc["task_id"].IsString());
        ASSERT_TRUE(submit_doc.HasMember("runtime_task_id") && submit_doc["runtime_task_id"].IsString());
        ASSERT_TRUE(submit_doc.HasMember("runtime_kind") && submit_doc["runtime_kind"].IsString());
        ASSERT_EQ(std::string(submit_doc["runtime_kind"].GetString()), "group");
        ASSERT_TRUE(submit_doc.HasMember("group_mode") && submit_doc["group_mode"].IsString());
        ASSERT_EQ(std::string(submit_doc["group_mode"].GetString()), "dag");
        ASSERT_TRUE(submit_doc.HasMember("node_count") && submit_doc["node_count"].IsUint());
        ASSERT_EQ(submit_doc["node_count"].GetUint(), 2u);
        const std::string group_task_id = submit_doc["task_id"].GetString();
        ASSERT_TRUE(!group_task_id.empty());

        ASSERT_EQ(local_routes["POST:/tasks/detail"](
                      "/tasks/detail", MakeTaskIdReq(group_task_id), rsp),
                  error::OK);
        rapidjson::Document detail_doc;
        detail_doc.Parse(rsp.c_str());
        ASSERT_TRUE(!detail_doc.HasParseError() && detail_doc.IsObject());
        ASSERT_TRUE(detail_doc.HasMember("sql_text") && detail_doc["sql_text"].IsString());
        const std::string detail_sql = detail_doc["sql_text"].GetString();
        ASSERT_TRUE(detail_sql.find("SELECT * FROM ring.in USING builtin.passthrough_stream INTO stream.mid") != std::string::npos);
        ASSERT_TRUE(detail_sql.find("SELECT * FROM stream.mid USING builtin.passthrough_stream INTO stream.out") != std::string::npos);
        ASSERT_TRUE(detail_sql.find("[group]") == std::string::npos);

        ASSERT_EQ(local_routes["POST:/tasks/list"]("/tasks/list", "{}", rsp), error::OK);
        rapidjson::Document list_doc;
        list_doc.Parse(rsp.c_str());
        ASSERT_TRUE(!list_doc.HasParseError() && list_doc.IsObject());
        ASSERT_TRUE(list_doc.HasMember("items") && list_doc["items"].IsArray());
        bool found_group_task = false;
        for (const auto& item : list_doc["items"].GetArray()) {
            if (!item.IsObject() || !item.HasMember("task_id") || !item["task_id"].IsString()) continue;
            if (std::string(item["task_id"].GetString()) != group_task_id) continue;
            found_group_task = true;
            ASSERT_TRUE(item.HasMember("sql_text") && item["sql_text"].IsString());
            const std::string list_sql = item["sql_text"].GetString();
            ASSERT_TRUE(list_sql.find("SELECT * FROM ring.in USING builtin.passthrough_stream INTO stream.mid") != std::string::npos);
            ASSERT_TRUE(list_sql.find("[group]") == std::string::npos);
            ASSERT_TRUE(item.HasMember("task_kind") && item["task_kind"].IsString());
            ASSERT_EQ(std::string(item["task_kind"].GetString()), "stream");
        }
        ASSERT_TRUE(found_group_task);

        ASSERT_EQ(local_routes["POST:/tasks/stream/status"](
                      "/tasks/stream/status", MakeTaskIdReq(group_task_id), rsp),
                  error::OK);
        rapidjson::Document status_doc;
        status_doc.Parse(rsp.c_str());
        ASSERT_TRUE(!status_doc.HasParseError() && status_doc.IsObject());
        ASSERT_TRUE(status_doc.HasMember("runtime_kind") && status_doc["runtime_kind"].IsString());
        ASSERT_EQ(std::string(status_doc["runtime_kind"].GetString()), "group");
        ASSERT_TRUE(status_doc.HasMember("nodes") && status_doc["nodes"].IsArray());
        ASSERT_EQ(status_doc["nodes"].Size(), rapidjson::SizeType(2));
        ASSERT_TRUE(status_doc.HasMember("share_sets") && status_doc["share_sets"].IsArray());
        ASSERT_TRUE(status_doc.HasMember("resolved_sources") && status_doc["resolved_sources"].IsArray());

        ASSERT_EQ(local_routes["POST:/tasks/stream/stop"](
                      "/tasks/stream/stop", MakeTaskIdReq(group_task_id), rsp),
                  error::OK);
        rapidjson::Document stop_doc;
        stop_doc.Parse(rsp.c_str());
        ASSERT_TRUE(!stop_doc.HasParseError() && stop_doc.IsObject());
        ASSERT_TRUE(stop_doc.HasMember("runtime_kind") && stop_doc["runtime_kind"].IsString());
        ASSERT_EQ(std::string(stop_doc["runtime_kind"].GetString()), "group");
        ASSERT_EQ(std::string(stop_doc["status"].GetString()), "stopped");

        ASSERT_EQ(stream_execute_calls.load(), 1);
        ASSERT_TRUE(stream_status_calls.load() >= 1);
        ASSERT_EQ(stream_stop_calls.load(), 1);
        ASSERT_EQ(classify_calls.load(), 0);

        ASSERT_EQ(local_routes["POST:/tasks/delete"](
                      "/tasks/delete", MakeTaskIdReq(group_task_id), rsp),
                  error::OK);
        ASSERT_EQ(p.Stop(), 0);
    }

    {
        const std::string stream_contract_dir = MakeTempDir("stream_contract_guard");
        std::atomic<int> stream_execute_calls{0};
        std::atomic<int> classify_calls{0};

        MockRouterHandle scheduler({
            {"POST", "/scheduler/sql/classify",
             [&classify_calls](const std::string&, const std::string&, std::string& rsp) {
                 classify_calls.fetch_add(1);
                 rsp = R"({"task_kind":"stream"})";
                 return error::OK;
             }},
            {"POST", "/scheduler/stream/execute",
             [&stream_execute_calls](const std::string&, const std::string&, std::string& rsp) {
                 stream_execute_calls.fetch_add(1);
                 rsp = R"({"runtime_task_id":"stream_contract_guard_1"})";
                 return error::OK;
             }},
        });
        MockQuerier querier;
        querier.AddHandle(&scheduler);

        TaskPlugin p;
        const std::string opt = "db_dir=" + stream_contract_dir + ";disable_worker=1";
        ASSERT_EQ(p.Option(opt.c_str()), 0);
        ASSERT_EQ(p.Load(&querier), 0);
        ASSERT_EQ(p.Start(), 0);

        auto local_routes = CollectRoutes(&p);
        ASSERT_TRUE(local_routes.count("POST:/tasks/stream/execute") == 1);

        std::string rsp;
        ASSERT_EQ(local_routes["POST:/tasks/stream/execute"](
                      "/tasks/stream/execute",
                      R"({
                          "execution_kind":"group",
                          "group_mode":"dag",
                          "sql_text":"SELECT * FROM ring.a USING builtin.passthrough_stream INTO stream.b;SELECT * FROM stream.b USING builtin.passthrough_stream INTO stream.c;",
                          "dag":{"nodes":[]}
                      })",
                      rsp),
                  error::BAD_REQUEST);
        ASSERT_TRUE(rsp.find("STREAM_GROUP_SQL_TEXT_INVALID") != std::string::npos);
        ASSERT_EQ(stream_execute_calls.load(), 0);

        ASSERT_EQ(local_routes["POST:/tasks/stream/execute"](
                      "/tasks/stream/execute",
                      R"({
                          "execution_kind":"group",
                          "group_mode":"dag",
                          "sql_text":"SELECT * FROM ring.a USING builtin.passthrough_stream INTO stream.b;"
                      })",
                      rsp),
                  error::BAD_REQUEST);
        ASSERT_TRUE(rsp.find("STREAM_GROUP_SQL_TEXT_INVALID") != std::string::npos);
        ASSERT_EQ(stream_execute_calls.load(), 0);

        ASSERT_EQ(local_routes["POST:/tasks/stream/execute"](
                      "/tasks/stream/execute",
                      R"({
                          "execution_kind":"single",
                          "group_mode":"dag",
                          "sql_text":"SELECT * FROM ring.a USING builtin.passthrough_stream INTO stream.b"
                      })",
                      rsp),
                  error::BAD_REQUEST);
        ASSERT_TRUE(rsp.find("STREAM_GROUP_SQL_TEXT_INVALID") != std::string::npos);
        ASSERT_EQ(stream_execute_calls.load(), 0);
        ASSERT_EQ(classify_calls.load(), 0);

        ASSERT_EQ(local_routes["POST:/tasks/stream/execute"](
                      "/tasks/stream/execute",
                      R"({
                          "execution_kind":"single",
                          "sql_text":"SELECT * FROM ring.a USING builtin.passthrough_stream INTO stream.b;SELECT * FROM ring.c USING builtin.passthrough_stream INTO stream.d;"
                      })",
                      rsp),
                  error::BAD_REQUEST);
        ASSERT_TRUE(rsp.find("STREAM_GROUP_SQL_TEXT_INVALID") != std::string::npos);
        ASSERT_EQ(stream_execute_calls.load(), 0);
        ASSERT_EQ(classify_calls.load(), 0);

        ASSERT_EQ(p.Stop(), 0);
    }

    {
        const std::string analyze_dir = MakeTempDir("sql_analyze_routes");
        std::atomic<int> classify_calls{0};

        MockRouterHandle scheduler({
            {"POST", "/scheduler/sql/classify",
             [&classify_calls](const std::string&, const std::string& req, std::string& rsp) {
                 rapidjson::Document d;
                 d.Parse(req.c_str());
                 ASSERT_TRUE(!d.HasParseError() && d.IsObject());
                 ASSERT_TRUE(d.HasMember("sql") && d["sql"].IsString());
                 const std::string sql = d["sql"].GetString();
                 classify_calls.fetch_add(1);
                 if (sql.find("BROKEN") != std::string::npos) {
                     rsp = R"({"error":"bad sql","error_code":"BAD_SQL"})";
                     return error::BAD_REQUEST;
                 }
                 if (sql.find("tcp_session_mock.tcp_src") != std::string::npos ||
                     sql.find("stream.") != std::string::npos) {
                     rsp = R"({"task_kind":"stream"})";
                 } else {
                     rsp = R"({"task_kind":"batch"})";
                 }
                 return error::OK;
             }},
        });
        MockQuerier querier;
        querier.AddHandle(&scheduler);

        TaskPlugin p;
        const std::string opt = "db_dir=" + analyze_dir + ";disable_worker=1";
        ASSERT_EQ(p.Option(opt.c_str()), 0);
        ASSERT_EQ(p.Load(&querier), 0);
        ASSERT_EQ(p.Start(), 0);

        auto local_routes = CollectRoutes(&p);
        ASSERT_TRUE(local_routes.count("POST:/tasks/sql/analyze") == 1);

        std::string rsp;
        ASSERT_EQ(local_routes["POST:/tasks/sql/analyze"](
                      "/tasks/sql/analyze",
                      R"({"sql":"SELECT 1"})",
                      rsp),
                  error::BAD_REQUEST);
        rapidjson::Document invalid_req_doc;
        invalid_req_doc.Parse(rsp.c_str());
        ASSERT_TRUE(!invalid_req_doc.HasParseError() && invalid_req_doc.IsObject());
        ASSERT_TRUE(invalid_req_doc.HasMember("error_code") && invalid_req_doc["error_code"].IsString());
        ASSERT_EQ(std::string(invalid_req_doc["error_code"].GetString()), "SQL_TEXT_INVALID");

        ASSERT_EQ(local_routes["POST:/tasks/sql/analyze"](
                      "/tasks/sql/analyze",
                      R"({"sql_text":"SELECT * FROM sqlite.local.src INTO dataframe.tmp;SELECT * FROM tcp_session_mock.tcp_src USING builtin.tcp_service_merge_stream INTO dataframe.svc;"})",
                      rsp),
                  error::OK);
        rapidjson::Document ok_doc;
        ok_doc.Parse(rsp.c_str());
        ASSERT_TRUE(!ok_doc.HasParseError() && ok_doc.IsObject());
        ASSERT_TRUE(ok_doc.HasMember("statement_count") && ok_doc["statement_count"].IsUint());
        ASSERT_EQ(ok_doc["statement_count"].GetUint(), 2u);
        ASSERT_TRUE(ok_doc.HasMember("statements") && ok_doc["statements"].IsArray());
        ASSERT_EQ(ok_doc["statements"].Size(), rapidjson::SizeType(2));
        ASSERT_TRUE(ok_doc.HasMember("statement_kinds") && ok_doc["statement_kinds"].IsArray());
        ASSERT_EQ(ok_doc["statement_kinds"].Size(), rapidjson::SizeType(2));
        ASSERT_EQ(std::string(ok_doc["statement_kinds"][0].GetString()), "batch");
        ASSERT_EQ(std::string(ok_doc["statement_kinds"][1].GetString()), "stream");
        ASSERT_TRUE(ok_doc.HasMember("task_kind") && ok_doc["task_kind"].IsString());
        ASSERT_EQ(std::string(ok_doc["task_kind"].GetString()), "mixed");

        ASSERT_EQ(local_routes["POST:/tasks/sql/analyze"](
                      "/tasks/sql/analyze",
                      R"({"sql_text":"SELECT 1;;SELECT 2;"})",
                      rsp),
                  error::BAD_REQUEST);
        rapidjson::Document split_err_doc;
        split_err_doc.Parse(rsp.c_str());
        ASSERT_TRUE(!split_err_doc.HasParseError() && split_err_doc.IsObject());
        ASSERT_TRUE(split_err_doc.HasMember("error_code") && split_err_doc["error_code"].IsString());
        ASSERT_EQ(std::string(split_err_doc["error_code"].GetString()), "SQL_TEXT_INVALID");
        ASSERT_TRUE(split_err_doc.HasMember("sql_index") && split_err_doc["sql_index"].IsInt());
        ASSERT_EQ(split_err_doc["sql_index"].GetInt(), 1);

        ASSERT_EQ(local_routes["POST:/tasks/sql/analyze"](
                      "/tasks/sql/analyze",
                      R"({"sql_text":"SELECT * FROM sqlite.local.src INTO dataframe.tmp;SELECT BROKEN;"})",
                      rsp),
                  error::BAD_REQUEST);
        rapidjson::Document classify_err_doc;
        classify_err_doc.Parse(rsp.c_str());
        ASSERT_TRUE(!classify_err_doc.HasParseError() && classify_err_doc.IsObject());
        ASSERT_TRUE(classify_err_doc.HasMember("error_code") && classify_err_doc["error_code"].IsString());
        ASSERT_EQ(std::string(classify_err_doc["error_code"].GetString()), "BAD_SQL");
        ASSERT_TRUE(classify_err_doc.HasMember("sql_index") && classify_err_doc["sql_index"].IsInt());
        ASSERT_EQ(classify_err_doc["sql_index"].GetInt(), 1);

        ASSERT_TRUE(classify_calls.load() >= 3);
        ASSERT_EQ(p.Stop(), 0);
    }

    {
        const std::string retention_count_dir = MakeTempDir("retention_count");
        const std::string retention_count_db = retention_count_dir + "/task_store.db";
        MockRouterHandle scheduler({MakeSqlClassifyRoute()});
        MockQuerier querier;
        querier.AddHandle(&scheduler);

        TaskPlugin p;
        const std::string opt = "db_dir=" + retention_count_dir + ";disable_worker=1;retention_max_count=2";
        ASSERT_EQ(p.Option(opt.c_str()), 0);
        ASSERT_EQ(p.Load(&querier), 0);
        ASSERT_EQ(p.Start(), 0);

        auto local_routes = CollectRoutes(&p);

        std::string rsp;
        for (int i = 0; i < 4; ++i) {
            ASSERT_EQ(local_routes["POST:/tasks/batch/execute"]("/tasks/batch/execute", R"({"sql_text":"SELECT 1","mode":"sync"})", rsp), error::OK);
        }
        ASSERT_TRUE(WaitUntil([&]() {
            return CountTasks(retention_count_db) == 2;
        }, 4000, 100));
        ASSERT_EQ(p.Stop(), 0);
    }

    {
        const std::string retention_days_dir = MakeTempDir("retention_days");
        const std::string retention_days_db = retention_days_dir + "/task_store.db";
        MockRouterHandle scheduler({MakeSqlClassifyRoute()});
        MockQuerier querier;
        querier.AddHandle(&scheduler);

        TaskPlugin p;
        const std::string opt = "db_dir=" + retention_days_dir + ";disable_worker=1;retention_days=1";
        ASSERT_EQ(p.Option(opt.c_str()), 0);
        ASSERT_EQ(p.Load(&querier), 0);
        ASSERT_EQ(p.Start(), 0);

        auto local_routes = CollectRoutes(&p);

        std::string rsp;
        ASSERT_EQ(local_routes["POST:/tasks/batch/execute"]("/tasks/batch/execute", R"({"sql_text":"SELECT 1","mode":"sync"})", rsp), error::OK);
        rapidjson::Document first_submit;
        first_submit.Parse(rsp.c_str());
        ASSERT_TRUE(!first_submit.HasParseError() && first_submit.IsObject());
        const std::string old_task_id = first_submit["task_id"].GetString();
        ASSERT_TRUE(UpdateTaskCreatedAt(retention_days_db, old_task_id, "2000-01-01 00:00:00"));

        // 再创建一个终态任务，触发 retention 清理
        ASSERT_EQ(local_routes["POST:/tasks/batch/execute"]("/tasks/batch/execute", R"({"sql_text":"SELECT 1","mode":"sync"})", rsp), error::OK);
        ASSERT_TRUE(WaitUntil([&]() {
            return !TaskExists(retention_days_db, old_task_id);
        }, 4000, 100));
        ASSERT_TRUE(WaitUntil([&]() {
            return CountTasks(retention_days_db) == 1;
        }, 4000, 100));
        ASSERT_EQ(p.Stop(), 0);
    }

    std::puts("=== All TaskPlugin tests passed ===");
    return 0;
}
