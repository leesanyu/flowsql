#include <cassert>
#include <cstdio>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unistd.h>

#include <common/error_code.h>
#include <framework/interfaces/irouter_handle.h>
#include <services/task/task_plugin.h>
#include <rapidjson/document.h>

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

int main() {
    std::puts("=== TaskPlugin Tests ===");
    const std::string dir = MakeTempDir("basic");

    std::unordered_map<std::string, fnRouterHandler> routes;
    std::string task_id;

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
                                               std::string("{\"task_id\":\"") + task_id + "\"}",
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
                                               std::string("{\"task_id\":\"") + task_id + "\"}",
                                               rsp),
                  error::OK);
        rapidjson::Document d;
        d.Parse(rsp.c_str());
        ASSERT_TRUE(!d.HasParseError());
        ASSERT_EQ(std::string(d["status"].GetString()), "failed");
        ASSERT_EQ(std::string(d["error_code"].GetString()), "PROCESS_RESTART");

        ASSERT_EQ(routes["POST:/tasks/delete"]("/tasks/delete",
                                               std::string("{\"task_id\":\"") + task_id + "\"}",
                                               rsp),
                  error::OK);
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

    std::puts("=== All TaskPlugin tests passed ===");
    return 0;
}
