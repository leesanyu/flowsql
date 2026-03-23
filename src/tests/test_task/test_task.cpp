#include <cassert>
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
#include <framework/interfaces/irouter_handle.h>
#include <services/task/task_plugin.h>
#include <rapidjson/document.h>
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

class MockRouterHandle : public IRouterHandle {
 public:
    explicit MockRouterHandle(std::vector<RouteItem> items) : items_(std::move(items)) {}
    void EnumRoutes(std::function<void(const RouteItem&)> cb) override {
        for (auto& item : items_) cb(item);
    }

 private:
    std::vector<RouteItem> items_;
};

class MockQuerier : public IQuerier {
 public:
    void AddHandle(IRouterHandle* h) { handles_.push_back(h); }

    int Traverse(const Guid& iid, fntraverse proc) override {
        if (memcmp(&iid, &IID_ROUTER_HANDLE, sizeof(Guid)) != 0) return 0;
        for (auto* h : handles_) {
            if (proc(h) == -1) break;
        }
        return 0;
    }

    void* First(const Guid&) override { return nullptr; }

 private:
    std::vector<IRouterHandle*> handles_;
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
        ASSERT_TRUE(routes.count("POST:/tasks/delete") == 1);

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

    std::puts("=== All TaskPlugin tests passed ===");
    return 0;
}
