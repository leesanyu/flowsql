#include <cassert>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>
#include <thread>
#include <unistd.h>

#include <common/error_code.h>
#include <common/iquerier.hpp>
#include <framework/interfaces/ichannel_registry.h>
#include <framework/interfaces/irouter_handle.h>
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

class MockRouterHandle : public IRouterHandle {
 public:
    explicit MockRouterHandle(std::vector<RouteItem> items) : items_(std::move(items)) {}
    void EnumRoutes(std::function<void(const RouteItem&)> cb) override {
        for (auto& item : items_) cb(item);
    }

 private:
    std::vector<RouteItem> items_;
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
    void AddHandle(IRouterHandle* h) { handles_.push_back(h); }
    void SetChannelRegistry(IChannelRegistry* r) { channel_registry_ = r; }

    int Traverse(const Guid& iid, fntraverse proc) override {
        if (memcmp(&iid, &IID_ROUTER_HANDLE, sizeof(Guid)) != 0) return 0;
        for (auto* h : handles_) {
            if (proc(h) == -1) break;
        }
        return 0;
    }

    void* First(const Guid& iid) override {
        if (memcmp(&iid, &IID_CHANNEL_REGISTRY, sizeof(Guid)) == 0) return channel_registry_;
        return nullptr;
    }

 private:
    std::vector<IRouterHandle*> handles_;
    IChannelRegistry* channel_registry_ = nullptr;
};

int main() {
    std::puts("=== TaskPlugin Tests ===");
    const std::string dir = MakeTempDir("basic");

    std::unordered_map<std::string, fnRouterHandler> routes;
    std::string task_id;

    const std::string db_path = dir + "/task_store.db";

    {
        TaskPlugin p;
        const std::string opt = "db_dir=" + dir + ";disable_worker=1";
        ASSERT_EQ(p.Option(opt.c_str()), 0);
        ASSERT_EQ(p.Load(nullptr), 0);
        ASSERT_EQ(p.Start(), 0);
        p.EnumRoutes([&](const RouteItem& item) { routes[item.method + ":" + item.uri] = item.handler; });
        ASSERT_TRUE(routes.count("POST:/tasks/submit") == 1);
        ASSERT_TRUE(routes.count("POST:/tasks/list") == 1);
        ASSERT_TRUE(routes.count("POST:/tasks/detail") == 1);
        ASSERT_TRUE(routes.count("POST:/tasks/diagnostics") == 1);
        ASSERT_TRUE(routes.count("POST:/tasks/delete") == 1);
        ASSERT_TRUE(routes.count("POST:/tasks/cancel") == 1);

        std::string rsp;
        ASSERT_EQ(routes["POST:/tasks/submit"]("/tasks/submit", R"({"sql":"SELECT 1"})", rsp), error::OK);
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

        ASSERT_EQ(routes["POST:/tasks/submit"]("/tasks/submit", R"({"sql":"SELECT 1","mode":"sync"})", rsp), error::OK);
        rapidjson::Document sync_ret;
        sync_ret.Parse(rsp.c_str());
        ASSERT_TRUE(!sync_ret.HasParseError() && sync_ret.IsObject());
        ASSERT_TRUE(sync_ret.HasMember("task_id") && sync_ret["task_id"].IsString());
        ASSERT_TRUE(sync_ret.HasMember("status") && sync_ret["status"].IsString());
        ASSERT_EQ(std::string(sync_ret["status"].GetString()), "failed");
        ASSERT_TRUE(sync_ret.HasMember("error_code") && sync_ret["error_code"].IsString());
        ASSERT_EQ(std::string(sync_ret["error_code"].GetString()), "SCHEDULER_UNAVAILABLE");

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
        routes.clear();
        p.EnumRoutes([&](const RouteItem& item) { routes[item.method + ":" + item.uri] = item.handler; });

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
        TaskPlugin p;
        const std::string opt = "db_path=" + db_path + ";disable_worker=1";
        ASSERT_EQ(p.Option(opt.c_str()), 0);
        ASSERT_EQ(p.Load(nullptr), 0);
        ASSERT_EQ(p.Start(), 0);

        std::unordered_map<std::string, fnRouterHandler> local_routes;
        p.EnumRoutes([&](const RouteItem& item) { local_routes[item.method + ":" + item.uri] = item.handler; });
        std::string rsp;
        ASSERT_EQ(local_routes["POST:/tasks/submit"]("/tasks/submit", R"({"sql":"SELECT 1"})", rsp), error::OK);
        ASSERT_TRUE(std::filesystem::exists(db_path));
        ASSERT_EQ(p.Stop(), 0);
    }

    {
        const std::string async_dir = MakeTempDir("async_worker");
        const std::string async_db_path = async_dir + "/task_store.db";

        MockRouterHandle scheduler({
            {"POST", "/tasks/instant/execute",
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

        std::unordered_map<std::string, fnRouterHandler> local_routes;
        p.EnumRoutes([&](const RouteItem& item) { local_routes[item.method + ":" + item.uri] = item.handler; });
        ASSERT_TRUE(local_routes.count("POST:/tasks/submit") == 1);
        ASSERT_TRUE(local_routes.count("POST:/tasks/detail") == 1);
        ASSERT_TRUE(local_routes.count("POST:/tasks/delete") == 1);

        std::string rsp;
        ASSERT_EQ(local_routes["POST:/tasks/submit"]("/tasks/submit", R"({"sql":"SELECT 42","mode":"async"})", rsp), error::OK);
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
            {"POST", "/tasks/instant/execute",
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

        std::unordered_map<std::string, fnRouterHandler> local_routes;
        p.EnumRoutes([&](const RouteItem& item) { local_routes[item.method + ":" + item.uri] = item.handler; });

        std::string rsp;
        ASSERT_EQ(local_routes["POST:/tasks/submit"]("/tasks/submit", R"({"sql":"SELECT 99","mode":"async"})", rsp), error::OK);
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
            {"POST", "/tasks/instant/execute",
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

        std::unordered_map<std::string, fnRouterHandler> local_routes;
        p.EnumRoutes([&](const RouteItem& item) { local_routes[item.method + ":" + item.uri] = item.handler; });

        std::string rsp;
        ASSERT_EQ(local_routes["POST:/tasks/submit"](
                      "/tasks/submit",
                      R"({"mode":"async","sqls":["SELECT * FROM sqlite.local.src INTO dataframe.tmp","SELECT * FROM dataframe.tmp USING builtin.passthrough INTO dataframe.final"]})",
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
        ASSERT_EQ(removed.size(), size_t(1));
        ASSERT_EQ(removed[0], "tmp");

        ASSERT_EQ(local_routes["POST:/tasks/delete"]("/tasks/delete", MakeTaskIdReq(task_id), rsp), error::OK);
        ASSERT_EQ(p.Stop(), 0);
    }

    {
        const std::string diag_dir = MakeTempDir("diagnostics");
        std::atomic<int> call_no{0};
        MockRouterHandle scheduler({
            {"POST", "/tasks/instant/execute",
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

        std::unordered_map<std::string, fnRouterHandler> local_routes;
        p.EnumRoutes([&](const RouteItem& item) { local_routes[item.method + ":" + item.uri] = item.handler; });

        std::string rsp;
        ASSERT_EQ(local_routes["POST:/tasks/submit"](
                      "/tasks/submit",
                      R"({"mode":"async","sqls":["SELECT * FROM sqlite.local.src INTO dataframe.tmp","SELECT * FROM dataframe.tmp USING builtin.passthrough INTO dataframe.final"]})",
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
            {"POST", "/tasks/instant/execute",
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

        std::unordered_map<std::string, fnRouterHandler> local_routes;
        p.EnumRoutes([&](const RouteItem& item) { local_routes[item.method + ":" + item.uri] = item.handler; });

        std::string rsp;
        ASSERT_EQ(local_routes["POST:/tasks/submit"](
                      "/tasks/submit",
                      R"({"mode":"async","sqls":["SELECT * FROM s1 INTO dataframe.tmp","SELECT * FROM dataframe.tmp,dataframe.tmp USING builtin.concat INTO dataframe.final"]})",
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
            {"POST", "/tasks/instant/execute",
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

        std::unordered_map<std::string, fnRouterHandler> local_routes;
        p.EnumRoutes([&](const RouteItem& item) { local_routes[item.method + ":" + item.uri] = item.handler; });

        std::vector<std::string> task_ids;
        std::vector<std::string> expected_targets;
        std::string rsp;
        for (int i = 0; i < 4; ++i) {
            const std::string sql = "SELECT " + std::to_string(i + 1);
            rapidjson::StringBuffer req_buf;
            rapidjson::Writer<rapidjson::StringBuffer> w(req_buf);
            w.StartObject();
            w.Key("sql");
            w.String(sql.c_str());
            w.Key("mode");
            w.String("async");
            w.EndObject();
            ASSERT_EQ(local_routes["POST:/tasks/submit"]("/tasks/submit", req_buf.GetString(), rsp), error::OK);
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
        TaskPlugin p;
        const std::string opt = "db_dir=" + cancel_pending_dir + ";disable_worker=1";
        ASSERT_EQ(p.Option(opt.c_str()), 0);
        ASSERT_EQ(p.Load(nullptr), 0);
        ASSERT_EQ(p.Start(), 0);

        std::unordered_map<std::string, fnRouterHandler> local_routes;
        p.EnumRoutes([&](const RouteItem& item) { local_routes[item.method + ":" + item.uri] = item.handler; });

        std::string rsp;
        ASSERT_EQ(local_routes["POST:/tasks/submit"]("/tasks/submit", R"({"sql":"SELECT 1","mode":"async"})", rsp), error::OK);
        rapidjson::Document submit;
        submit.Parse(rsp.c_str());
        ASSERT_TRUE(!submit.HasParseError() && submit.IsObject());
        const std::string task_id = submit["task_id"].GetString();

        ASSERT_EQ(local_routes["POST:/tasks/cancel"]("/tasks/cancel", MakeTaskIdReq(task_id), rsp), error::OK);
        ASSERT_TRUE(rsp.find("cancelled") != std::string::npos);

        ASSERT_EQ(local_routes["POST:/tasks/detail"]("/tasks/detail", MakeTaskIdReq(task_id), rsp), error::OK);
        rapidjson::Document detail;
        detail.Parse(rsp.c_str());
        ASSERT_TRUE(!detail.HasParseError() && detail.IsObject());
        ASSERT_EQ(std::string(detail["status"].GetString()), "cancelled");
        ASSERT_EQ(std::string(detail["error_code"].GetString()), "CANCELLED");

        ASSERT_EQ(local_routes["POST:/tasks/delete"]("/tasks/delete", MakeTaskIdReq(task_id), rsp), error::OK);
        ASSERT_EQ(p.Stop(), 0);
    }

    {
        const std::string cancel_running_dir = MakeTempDir("cancel_running");
        std::atomic<int> exec_count{0};

        MockRouterHandle scheduler({
            {"POST", "/tasks/instant/execute",
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

        std::unordered_map<std::string, fnRouterHandler> local_routes;
        p.EnumRoutes([&](const RouteItem& item) { local_routes[item.method + ":" + item.uri] = item.handler; });

        std::string rsp;
        ASSERT_EQ(local_routes["POST:/tasks/submit"](
                      "/tasks/submit",
                      R"({"mode":"async","sqls":["SELECT * FROM sqlite.local.src INTO dataframe.tmp","SELECT * FROM dataframe.tmp INTO dataframe.out"]})",
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
            {"POST", "/tasks/instant/execute",
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

        std::unordered_map<std::string, fnRouterHandler> local_routes;
        p.EnumRoutes([&](const RouteItem& item) { local_routes[item.method + ":" + item.uri] = item.handler; });

        std::string rsp;
        ASSERT_EQ(local_routes["POST:/tasks/submit"](
                      "/tasks/submit",
                      R"({"sql":"SELECT 1","mode":"async","timeout_s":1})",
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
        const std::string retention_count_dir = MakeTempDir("retention_count");
        const std::string retention_count_db = retention_count_dir + "/task_store.db";
        TaskPlugin p;
        const std::string opt = "db_dir=" + retention_count_dir + ";disable_worker=1;retention_max_count=2";
        ASSERT_EQ(p.Option(opt.c_str()), 0);
        ASSERT_EQ(p.Load(nullptr), 0);
        ASSERT_EQ(p.Start(), 0);

        std::unordered_map<std::string, fnRouterHandler> local_routes;
        p.EnumRoutes([&](const RouteItem& item) { local_routes[item.method + ":" + item.uri] = item.handler; });

        std::string rsp;
        for (int i = 0; i < 4; ++i) {
            ASSERT_EQ(local_routes["POST:/tasks/submit"]("/tasks/submit", R"({"sql":"SELECT 1","mode":"sync"})", rsp), error::OK);
        }
        ASSERT_EQ(CountTasks(retention_count_db), 2);
        ASSERT_EQ(p.Stop(), 0);
    }

    {
        const std::string retention_days_dir = MakeTempDir("retention_days");
        const std::string retention_days_db = retention_days_dir + "/task_store.db";
        TaskPlugin p;
        const std::string opt = "db_dir=" + retention_days_dir + ";disable_worker=1;retention_days=1";
        ASSERT_EQ(p.Option(opt.c_str()), 0);
        ASSERT_EQ(p.Load(nullptr), 0);
        ASSERT_EQ(p.Start(), 0);

        std::unordered_map<std::string, fnRouterHandler> local_routes;
        p.EnumRoutes([&](const RouteItem& item) { local_routes[item.method + ":" + item.uri] = item.handler; });

        std::string rsp;
        ASSERT_EQ(local_routes["POST:/tasks/submit"]("/tasks/submit", R"({"sql":"SELECT 1","mode":"sync"})", rsp), error::OK);
        rapidjson::Document first_submit;
        first_submit.Parse(rsp.c_str());
        ASSERT_TRUE(!first_submit.HasParseError() && first_submit.IsObject());
        const std::string old_task_id = first_submit["task_id"].GetString();
        ASSERT_TRUE(UpdateTaskCreatedAt(retention_days_db, old_task_id, "2000-01-01 00:00:00"));

        // 再创建一个终态任务，触发 retention 清理
        ASSERT_EQ(local_routes["POST:/tasks/submit"]("/tasks/submit", R"({"sql":"SELECT 1","mode":"sync"})", rsp), error::OK);
        ASSERT_TRUE(!TaskExists(retention_days_db, old_task_id));
        ASSERT_EQ(CountTasks(retention_days_db), 1);
        ASSERT_EQ(p.Stop(), 0);
    }

    std::puts("=== All TaskPlugin tests passed ===");
    return 0;
}
