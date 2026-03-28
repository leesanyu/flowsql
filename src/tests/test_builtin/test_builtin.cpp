#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <unistd.h>

#include <common/error_code.h>
#include <framework/core/dataframe.h>
#include <framework/core/dataframe_channel.h>
#include <framework/interfaces/ibinaddon_host.h>
#include <framework/interfaces/ioperator.h>
#include <framework/interfaces/ioperator_catalog.h>
#include <framework/interfaces/ioperator_registry.h>
#include <services/binaddon/binaddon_host_plugin.h>
#include <services/catalog/catalog_plugin.h>
#include <framework/interfaces/irouter_handle.h>
#include <rapidjson/document.h>

using namespace flowsql;
using namespace flowsql::binaddon;
using namespace flowsql::catalog;

#ifndef FIXTURE_CPP_SO_PATH
#define FIXTURE_CPP_SO_PATH ""
#endif

#ifndef FIXTURE_BAD_ABI_SO_PATH
#define FIXTURE_BAD_ABI_SO_PATH ""
#endif

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
                              ("flowsql_catalog_test_" + std::string(suffix) + "_" + std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    std::filesystem::create_directories(p, ec);
    return p.string();
}

static std::string MakeCatalogOption(const std::string& data_dir) {
    return "data_dir=" + data_dir + ";operator_db_dir=" + data_dir + "/catalog";
}

static std::string MakeCatalogOptionWithDbPath(const std::string& data_dir, const std::string& db_path) {
    return "data_dir=" + data_dir + ";operator_db_path=" + db_path;
}

static std::shared_ptr<DataFrameChannel> MakeChannel(const std::string& name, int rows = 2) {
    auto ch = std::make_shared<DataFrameChannel>("dataframe", name);
    ch->Open();

    DataFrame df;
    df.SetSchema({
        {"id", DataType::INT64, 0, ""},
        {"score", DataType::DOUBLE, 0, ""},
        {"name", DataType::STRING, 0, ""},
    });
    for (int i = 0; i < rows; ++i) {
        df.AppendRow({int64_t(i + 1), double(i) + 0.5, std::string("u") + std::to_string(i + 1)});
    }
    ASSERT_EQ(ch->Write(&df), 0);
    return ch;
}

static int ChannelRowCount(IDataFrameChannel* ch) {
    DataFrame out;
    if (!ch || ch->Read(&out) != 0) return -1;
    return out.RowCount();
}

static std::shared_ptr<IDataFrameChannel> GetDataFrame(CatalogPlugin& p, const char* name) {
    return std::dynamic_pointer_cast<IDataFrameChannel>(p.Get(name));
}

static std::string CopyToTmp(const std::string& src_path, const std::string& dir, const std::string& filename) {
    std::filesystem::path dst = std::filesystem::path(dir) / filename;
    std::error_code ec;
    std::filesystem::copy_file(src_path, dst, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) return "";
    return dst.string();
}

static std::string RuntimeOperatorsDir() {
    char exe_path[1024];
    ssize_t len = ::readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len > 0) {
        exe_path[len] = '\0';
        std::string exe_dir(exe_path);
        size_t pos = exe_dir.find_last_of('/');
        if (pos != std::string::npos) return exe_dir.substr(0, pos) + "/operators";
    }
    return "operators";
}

static std::string RuntimeOperatorPyPath(const std::string& category, const std::string& name) {
    std::filesystem::path p(RuntimeOperatorsDir());
    p /= (category + "_" + name + ".py");
    return p.string();
}

class TestQuerier : public IQuerier {
 public:
    IOperatorRegistry* op_registry = nullptr;
    IBinAddonHost* binaddon_host = nullptr;

    int Traverse(const Guid&, fntraverse) override { return 0; }

    void* First(const Guid& iid) override {
        if (memcmp(&iid, &IID_OPERATOR_REGISTRY, sizeof(Guid)) == 0) {
            return op_registry;
        }
        if (memcmp(&iid, &IID_BINADDON_HOST, sizeof(Guid)) == 0) {
            return binaddon_host;
        }
        return nullptr;
    }
};

static void TestRegistryBasic() {
    std::puts("[TEST] T04-T11 registry basic ...");
    const std::string dir = MakeTempDir("basic");

    CatalogPlugin p;
    const std::string opt = MakeCatalogOption(dir);
    ASSERT_EQ(p.Option(opt.c_str()), 0);
    ASSERT_EQ(p.Load(nullptr), 0);
    ASSERT_EQ(p.Start(), 0);

    auto ch1 = MakeChannel("result");

    // T04
    ASSERT_EQ(p.Register("result", std::static_pointer_cast<IChannel>(ch1)), 0);
    auto got = GetDataFrame(p, "result");
    ASSERT_TRUE(got != nullptr);
    ASSERT_EQ(ChannelRowCount(got.get()), 2);

    // T05
    ASSERT_EQ(p.Register("result", std::static_pointer_cast<IChannel>(MakeChannel("result_dup"))), -1);
    ASSERT_EQ(ChannelRowCount(GetDataFrame(p, "result").get()), 2);

    // T06
    ASSERT_TRUE(GetDataFrame(p, "missing") == nullptr);

    // T11
    std::set<std::string> names;
    p.List([&](const char* name, std::shared_ptr<IChannel>) { names.insert(name); });
    ASSERT_EQ(names.size(), size_t(1));
    ASSERT_TRUE(names.count("result") == 1);

    // T09
    ASSERT_EQ(p.Rename("result", "result_v2"), 0);
    ASSERT_TRUE(GetDataFrame(p, "result") == nullptr);
    ASSERT_TRUE(GetDataFrame(p, "result_v2") != nullptr);
    ASSERT_TRUE(std::filesystem::exists(std::filesystem::path(dir) / "result_v2.csv"));

    // T10
    ASSERT_EQ(p.Register("another", std::static_pointer_cast<IChannel>(MakeChannel("another"))), 0);
    ASSERT_EQ(p.Rename("result_v2", "another"), -1);
    ASSERT_TRUE(GetDataFrame(p, "result_v2") != nullptr);

    // T07
    ASSERT_EQ(p.Unregister("result_v2"), 0);
    ASSERT_TRUE(GetDataFrame(p, "result_v2") == nullptr);
    ASSERT_TRUE(!std::filesystem::exists(std::filesystem::path(dir) / "result_v2.csv"));

    // T08
    ASSERT_EQ(p.Unregister("not_exists"), -1);

    ASSERT_EQ(p.Stop(), 0);
    std::puts("[PASS] T04-T11 registry basic");
}

static void TestRestartRecover() {
    std::puts("[TEST] T12 restart recover ...");
    const std::string dir = MakeTempDir("recover");

    {
        CatalogPlugin p1;
        const std::string opt = MakeCatalogOption(dir);
        ASSERT_EQ(p1.Option(opt.c_str()), 0);
        ASSERT_EQ(p1.Load(nullptr), 0);
        ASSERT_EQ(p1.Start(), 0);
        ASSERT_EQ(p1.Register("daily", std::static_pointer_cast<IChannel>(MakeChannel("daily", 3))), 0);
    }

    CatalogPlugin p2;
    const std::string opt = MakeCatalogOption(dir);
    ASSERT_EQ(p2.Option(opt.c_str()), 0);
    ASSERT_EQ(p2.Load(nullptr), 0);
    ASSERT_EQ(p2.Start(), 0);

    auto got = GetDataFrame(p2, "daily");
    ASSERT_TRUE(got != nullptr);
    ASSERT_EQ(ChannelRowCount(got.get()), 3);
    std::puts("[PASS] T12 restart recover");
}

static void TestConcurrency() {
    std::puts("[TEST] T13 concurrent registry ...");
    const std::string dir = MakeTempDir("concurrency");

    CatalogPlugin p;
    const std::string opt = MakeCatalogOption(dir);
    ASSERT_EQ(p.Option(opt.c_str()), 0);
    ASSERT_EQ(p.Load(nullptr), 0);
    ASSERT_EQ(p.Start(), 0);

    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&p, t]() {
            const std::string name = "ch_" + std::to_string(t);
            auto ch = MakeChannel(name, 1);
            ASSERT_EQ(p.Register(name.c_str(), std::static_pointer_cast<IChannel>(ch)), 0);
            auto got = GetDataFrame(p, name.c_str());
            ASSERT_TRUE(got != nullptr);
            ASSERT_EQ(ChannelRowCount(got.get()), 1);
            ASSERT_EQ(p.Unregister(name.c_str()), 0);
        });
    }
    for (auto& th : threads) th.join();

    size_t cnt = 0;
    p.List([&](const char*, std::shared_ptr<IChannel>) { ++cnt; });
    ASSERT_EQ(cnt, size_t(0));
    std::puts("[PASS] T13 concurrent registry");
}

static void TestPersistFail() {
    std::puts("[TEST] T14 persist fail ...");
    CatalogPlugin p;
    ASSERT_EQ(p.Option("data_dir=/proc/flowsql_catalog_test_no_permission"), 0);
    ASSERT_EQ(p.Load(nullptr), 0);

    // Start 可能失败（目录不可创建），但 Register 也必须失败且不产生内存注册。
    (void)p.Start();
    ASSERT_EQ(p.Register("bad", std::static_pointer_cast<IChannel>(MakeChannel("bad"))), -1);
    ASSERT_TRUE(GetDataFrame(p, "bad") == nullptr);
    std::puts("[PASS] T14 persist fail");
}

static void TestOperatorRegistry() {
    std::puts("[TEST] T15-T17 operator registry ...");
    const std::string dir = MakeTempDir("operator");

    CatalogPlugin p;
    const std::string opt = MakeCatalogOption(dir);
    ASSERT_EQ(p.Option(opt.c_str()), 0);
    ASSERT_EQ(p.Load(nullptr), 0);  // Load 自动注册 passthrough
    ASSERT_EQ(p.Start(), 0);

    // T15
    IOperator* op = p.Create("passthrough");
    ASSERT_TRUE(op != nullptr);
    ASSERT_EQ(op->Name(), std::string("passthrough"));
    delete op;

    // T16
    ASSERT_TRUE(p.Create("not_exists") == nullptr);

    // T17
    std::set<std::string> names;
    p.List([&](const char* name) { names.insert(name); });
    ASSERT_TRUE(names.count("passthrough") == 1);

    std::puts("[PASS] T15-T17 operator registry");
}

static void TestOperatorCatalog() {
    std::puts("[TEST] T18-T23 operator catalog ...");
    const std::string dir = MakeTempDir("op_catalog");
    const std::string upload = MakeTempDir("op_catalog_upload");

    CatalogPlugin p;
    const std::string opt = MakeCatalogOption(dir);
    ASSERT_EQ(p.Option(opt.c_str()), 0);
    ASSERT_EQ(p.Load(nullptr), 0);
    ASSERT_EQ(p.Start(), 0);

    std::vector<OperatorMeta> batch;
    batch.push_back({"ml", "clean", "python", "bridge", "clean op", "data"});
    UpsertResult upsert = p.UpsertBatch(batch);
    ASSERT_EQ(upsert.success_count, 1);
    ASSERT_EQ(upsert.failed_count, 0);
    ASSERT_EQ(p.QueryStatus("ml", "clean"), OperatorStatus::kDeactivated);

    std::unordered_map<std::string, fnRouterHandler> routes;
    p.EnumRoutes([&](const RouteItem& item) {
        routes[item.method + ":" + item.uri] = item.handler;
    });
    ASSERT_TRUE(routes.count("POST:/operators/list") == 1);
    ASSERT_TRUE(routes.count("POST:/operators/upload") == 1);
    ASSERT_TRUE(routes.count("POST:/operators/delete") == 1);
    ASSERT_TRUE(routes.count("POST:/operators/detail") == 1);
    ASSERT_TRUE(routes.count("POST:/operators/activate") == 1);
    ASSERT_TRUE(routes.count("POST:/operators/deactivate") == 1);
    ASSERT_TRUE(routes.count("POST:/operators/update") == 1);
    ASSERT_TRUE(routes.count("POST:/operators/upsert_batch") == 1);
    std::string rsp;

    const std::string tmp_py = upload + "/ml_clean.py";
    {
        FILE* fp = fopen(tmp_py.c_str(), "wb");
        ASSERT_TRUE(fp != nullptr);
        const char* code = "def work(df):\n    return df\n";
        fwrite(code, 1, strlen(code), fp);
        fclose(fp);
    }
    const std::string upload_req =
        "{\"type\":\"python\",\"filename\":\"ml_clean.py\",\"tmp_path\":\"" + tmp_py + "\"}";
    ASSERT_EQ(routes["POST:/operators/upload"]("/operators/upload", upload_req, rsp), error::OK);

    ASSERT_EQ(routes["POST:/operators/activate"]("/operators/activate", R"({"name":"ml.clean"})", rsp), error::OK);
    ASSERT_EQ(p.QueryStatus("ml", "clean"), OperatorStatus::kActive);

    // upsert 不覆盖 active
    std::vector<OperatorMeta> batch2;
    batch2.push_back({"ml", "clean", "python", "bridge", "clean op v2", "storage"});
    upsert = p.UpsertBatch(batch2);
    ASSERT_EQ(upsert.success_count, 1);
    ASSERT_EQ(upsert.failed_count, 0);
    ASSERT_EQ(p.QueryStatus("ml", "clean"), OperatorStatus::kActive);

    ASSERT_EQ(routes["POST:/operators/detail"]("/operators/detail", R"({"name":"ml.clean"})", rsp), error::OK);
    {
        rapidjson::Document d;
        d.Parse(rsp.c_str());
        ASSERT_TRUE(d.IsObject());
        ASSERT_EQ(std::string(d["description"].GetString()), "clean op v2");
        ASSERT_EQ(std::string(d["position"].GetString()), "storage");
        ASSERT_EQ(d["active"].GetInt(), 1);
    }

    ASSERT_EQ(routes["POST:/operators/update"]("/operators/update",
              R"({"name":"ml.clean","description":"updated desc","position":"data"})", rsp), error::OK);
    ASSERT_EQ(routes["POST:/operators/detail"]("/operators/detail", R"({"name":"ml.clean"})", rsp), error::OK);
    {
        rapidjson::Document d;
        d.Parse(rsp.c_str());
        ASSERT_TRUE(d.IsObject());
        ASSERT_EQ(std::string(d["description"].GetString()), "updated desc");
        ASSERT_EQ(std::string(d["position"].GetString()), "data");
    }

    ASSERT_EQ(routes["POST:/operators/deactivate"]("/operators/deactivate", R"({"name":"ml.clean"})", rsp), error::OK);
    ASSERT_EQ(p.QueryStatus("ml", "clean"), OperatorStatus::kDeactivated);

    ASSERT_EQ(routes["POST:/operators/list"]("/operators/list", R"({"type":"python"})", rsp), error::OK);
    {
        rapidjson::Document d;
        d.Parse(rsp.c_str());
        ASSERT_TRUE(d.IsObject() && d.HasMember("operators") && d["operators"].IsArray());
        ASSERT_EQ(d["operators"].Size(), rapidjson::SizeType(1));
    }

    ASSERT_EQ(routes["POST:/operators/upsert_batch"]("/operators/upsert_batch",
              R"({"operators":[{"category":"ml","name":"x"}]})", rsp), error::BAD_REQUEST);

    const std::string py_path = RuntimeOperatorPyPath("ml", "clean");
    ASSERT_TRUE(std::filesystem::exists(py_path));

    ASSERT_EQ(routes["POST:/operators/activate"]("/operators/activate", R"({"name":"ml.clean"})", rsp), error::OK);
    ASSERT_EQ(routes["POST:/operators/delete"]("/operators/delete",
              R"({"type":"python","name":"ml.clean"})", rsp), error::CONFLICT);
    ASSERT_EQ(routes["POST:/operators/deactivate"]("/operators/deactivate", R"({"name":"ml.clean"})", rsp), error::OK);
    ASSERT_EQ(routes["POST:/operators/delete"]("/operators/delete",
              R"({"type":"python","name":"ml.clean"})", rsp), error::OK);
    ASSERT_EQ(p.QueryStatus("ml", "clean"), OperatorStatus::kNotFound);
    ASSERT_TRUE(!std::filesystem::exists(py_path));
    ASSERT_EQ(routes["POST:/operators/detail"]("/operators/detail", R"({"name":"ml.clean"})", rsp), error::NOT_FOUND);
    ASSERT_EQ(routes["POST:/operators/delete"]("/operators/delete",
              R"({"type":"python","name":"ml.clean"})", rsp), error::NOT_FOUND);
    ASSERT_EQ(routes["POST:/operators/delete"]("/operators/delete",
              R"({"type":"builtin","name":"builtin.passthrough"})", rsp), error::BAD_REQUEST);

    std::puts("[PASS] T18-T23 operator catalog");
}

static void TestCppPluginLifecycle() {
    std::puts("[TEST] T42-T48 cpp plugin lifecycle ...");
    const std::string fixture_so = FIXTURE_CPP_SO_PATH;
    const std::string bad_abi_so = FIXTURE_BAD_ABI_SO_PATH;
    ASSERT_TRUE(!fixture_so.empty());
    ASSERT_TRUE(!bad_abi_so.empty());
    ASSERT_TRUE(std::filesystem::exists(fixture_so));
    ASSERT_TRUE(std::filesystem::exists(bad_abi_so));

    const std::string dir = MakeTempDir("cpp_plugin");
    const std::string upload = MakeTempDir("cpp_upload");

    CatalogPlugin p;
    BinAddonHostPlugin binaddon;
    TestQuerier querier;
    querier.op_registry = static_cast<IOperatorRegistry*>(&p);
    querier.binaddon_host = static_cast<IBinAddonHost*>(&binaddon);

    const std::string opt = MakeCatalogOption(dir);
    const std::string db_path = (std::filesystem::path(dir) / "catalog" / "operator_catalog.db").string();
    const std::string binaddon_opt = "operator_db_path=" + db_path + ";upload_dir=" + (std::filesystem::path(upload) / "binaddon").string();
    ASSERT_EQ(p.Option(opt.c_str()), 0);
    ASSERT_EQ(binaddon.Option(binaddon_opt.c_str()), 0);
    ASSERT_EQ(p.Load(&querier), 0);
    ASSERT_EQ(binaddon.Load(&querier), 0);
    ASSERT_EQ(p.Start(), 0);
    ASSERT_EQ(binaddon.Start(), 0);

    std::unordered_map<std::string, fnRouterHandler> routes;
    p.EnumRoutes([&](const RouteItem& item) {
        routes[item.method + ":" + item.uri] = item.handler;
    });
    ASSERT_TRUE(routes.count("POST:/operators/upload") == 1);
    ASSERT_TRUE(routes.count("POST:/operators/list") == 1);
    ASSERT_TRUE(routes.count("POST:/operators/activate") == 1);
    ASSERT_TRUE(routes.count("POST:/operators/detail") == 1);
    ASSERT_TRUE(routes.count("POST:/operators/deactivate") == 1);
    ASSERT_TRUE(routes.count("POST:/operators/delete") == 1);

    const std::string good_filename = "fixture_cpp_operator.so";
    const std::string good_tmp = CopyToTmp(fixture_so, upload, good_filename);
    ASSERT_TRUE(!good_tmp.empty());

    std::string rsp;
    std::string req = "{\"type\":\"cpp\",\"filename\":\"" + good_filename + "\",\"tmp_path\":\"" + good_tmp + "\"}";
    ASSERT_EQ(routes["POST:/operators/upload"]("/operators/upload", req, rsp), error::OK);

    std::string plugin_id;
    {
        rapidjson::Document d;
        d.Parse(rsp.c_str());
        ASSERT_TRUE(d.IsObject());
        ASSERT_TRUE(d.HasMember("plugin_id") && d["plugin_id"].IsString());
        ASSERT_EQ(std::string(d["status"].GetString()), "uploaded");
        plugin_id = d["plugin_id"].GetString();
        ASSERT_EQ(plugin_id.size(), size_t(64));
    }

    ASSERT_EQ(routes["POST:/operators/list"]("/operators/list", R"({"type":"cpp"})", rsp), error::OK);
    {
        rapidjson::Document d;
        d.Parse(rsp.c_str());
        ASSERT_TRUE(d.IsObject() && d["operators"].IsArray());
        ASSERT_EQ(d["operators"].Size(), rapidjson::SizeType(1));
        const auto& row = d["operators"][0];
        ASSERT_EQ(std::string(row["plugin_id"].GetString()), plugin_id);
        ASSERT_EQ(std::string(row["plugin"]["status"].GetString()), "uploaded");
        ASSERT_TRUE(row["plugin"]["abi_version"].IsNull());
        ASSERT_TRUE(row["plugin"]["operator_count"].IsNull());
        ASSERT_TRUE(row["plugin"]["operators"].IsNull());
    }

    req = "{\"type\":\"cpp\",\"plugin_id\":\"" + plugin_id + "\"}";
    ASSERT_EQ(routes["POST:/operators/activate"]("/operators/activate", req, rsp), error::OK);
    ASSERT_EQ(p.QueryStatus("cppdemo", "echo"), OperatorStatus::kActive);

    IOperator* op = p.Create("cppdemo.echo");
    ASSERT_TRUE(op != nullptr);
    ASSERT_EQ(op->Category(), std::string("cppdemo"));
    ASSERT_EQ(op->Name(), std::string("echo"));
    delete op;

    ASSERT_EQ(routes["POST:/operators/list"]("/operators/list", R"({"type":"cpp"})", rsp), error::OK);
    {
        rapidjson::Document d;
        d.Parse(rsp.c_str());
        ASSERT_TRUE(d.IsObject() && d["operators"].IsArray());
        const auto& row = d["operators"][0];
        ASSERT_EQ(std::string(row["plugin"]["status"].GetString()), "activated");
        ASSERT_EQ(row["plugin"]["operator_count"].GetInt(), 1);
        ASSERT_TRUE(row["plugin"]["operators"].IsArray());
        ASSERT_EQ(std::string(row["plugin"]["operators"][0].GetString()), "echo");
    }

    ASSERT_EQ(routes["POST:/operators/detail"]("/operators/detail", req, rsp), error::OK);
    {
        rapidjson::Document d;
        d.Parse(rsp.c_str());
        ASSERT_TRUE(d.IsObject());
        ASSERT_EQ(std::string(d["type"].GetString()), "cpp");
        ASSERT_EQ(std::string(d["plugin_id"].GetString()), plugin_id);
        ASSERT_TRUE(d["plugin"].IsObject());
        ASSERT_TRUE(d["plugin"]["path"].IsString());
    }

    ASSERT_EQ(routes["POST:/operators/deactivate"]("/operators/deactivate", req, rsp), error::OK);
    ASSERT_EQ(p.QueryStatus("cppdemo", "echo"), OperatorStatus::kDeactivated);
    ASSERT_TRUE(p.Create("cppdemo.echo") == nullptr);

    ASSERT_EQ(routes["POST:/operators/delete"]("/operators/delete", req, rsp), error::OK);
    ASSERT_EQ(p.QueryStatus("cppdemo", "echo"), OperatorStatus::kNotFound);
    ASSERT_EQ(routes["POST:/operators/list"]("/operators/list", R"({"type":"cpp"})", rsp), error::OK);
    {
        rapidjson::Document d;
        d.Parse(rsp.c_str());
        ASSERT_TRUE(d.IsObject() && d["operators"].IsArray());
        ASSERT_EQ(d["operators"].Size(), rapidjson::SizeType(0));
    }

    const std::string bad_filename = "fixture_bad_abi.so";
    const std::string bad_tmp = CopyToTmp(bad_abi_so, upload, bad_filename);
    ASSERT_TRUE(!bad_tmp.empty());
    req = "{\"type\":\"cpp\",\"filename\":\"" + bad_filename + "\",\"tmp_path\":\"" + bad_tmp + "\"}";
    ASSERT_EQ(routes["POST:/operators/upload"]("/operators/upload", req, rsp), error::OK);
    std::string bad_plugin_id;
    {
        rapidjson::Document d;
        d.Parse(rsp.c_str());
        bad_plugin_id = d["plugin_id"].GetString();
    }
    req = "{\"type\":\"cpp\",\"plugin_id\":\"" + bad_plugin_id + "\"}";
    ASSERT_EQ(routes["POST:/operators/activate"]("/operators/activate", req, rsp), error::BAD_REQUEST);
    ASSERT_EQ(routes["POST:/operators/list"]("/operators/list", R"({"type":"cpp"})", rsp), error::OK);
    {
        rapidjson::Document d;
        d.Parse(rsp.c_str());
        ASSERT_TRUE(d.IsObject() && d["operators"].IsArray());
        ASSERT_EQ(d["operators"].Size(), rapidjson::SizeType(1));
        ASSERT_EQ(std::string(d["operators"][0]["plugin"]["status"].GetString()), "broken");
        ASSERT_TRUE(d["operators"][0]["plugin"]["last_error"].IsString());
    }
    ASSERT_EQ(routes["POST:/operators/delete"]("/operators/delete", req, rsp), error::OK);
    ASSERT_EQ(binaddon.Stop(), 0);
    ASSERT_EQ(p.Stop(), 0);
    std::puts("[PASS] T42-T48 cpp plugin lifecycle");
}

static void TestOperatorDbPathOption() {
    std::puts("[TEST] T41 operator_db_path option ...");
    const std::string dir = MakeTempDir("op_db_path");
    const std::string db_path = dir + "/meta/shared.db";

    CatalogPlugin p;
    const std::string opt = MakeCatalogOptionWithDbPath(dir, db_path);
    ASSERT_EQ(p.Option(opt.c_str()), 0);
    ASSERT_EQ(p.Load(nullptr), 0);
    ASSERT_EQ(p.Start(), 0);

    std::vector<OperatorMeta> batch;
    batch.push_back({"ml", "clean", "python", "bridge", "clean op", "data"});
    UpsertResult upsert = p.UpsertBatch(batch);
    ASSERT_EQ(upsert.success_count, 1);
    ASSERT_EQ(upsert.failed_count, 0);
    ASSERT_TRUE(std::filesystem::exists(db_path));
    ASSERT_TRUE(!std::filesystem::exists(std::filesystem::path(dir) / "catalog" / "operator_catalog.db"));
    ASSERT_EQ(p.Stop(), 0);
    std::puts("[PASS] T41 operator_db_path option");
}

static void TestHttpRoutes() {
    std::puts("[TEST] T24-T35 http routes ...");
    const std::string dir = MakeTempDir("http");
    const std::string upload = MakeTempDir("http_upload");

    CatalogPlugin p;
    const std::string opt = MakeCatalogOption(dir);
    ASSERT_EQ(p.Option(opt.c_str()), 0);
    ASSERT_EQ(p.Load(nullptr), 0);
    ASSERT_EQ(p.Start(), 0);

    std::unordered_map<std::string, fnRouterHandler> routes;
    p.EnumRoutes([&](const RouteItem& item) {
        routes[item.method + ":" + item.uri] = item.handler;
    });
    ASSERT_TRUE(routes.count("GET:/channels/dataframe") == 1);
    ASSERT_TRUE(routes.count("POST:/channels/dataframe/import") == 1);
    ASSERT_TRUE(routes.count("POST:/channels/dataframe/preview") == 1);
    ASSERT_TRUE(routes.count("POST:/channels/dataframe/rename") == 1);
    ASSERT_TRUE(routes.count("POST:/channels/dataframe/delete") == 1);

    std::string rsp;

    // T25: 无通道时列表为空
    ASSERT_EQ(routes["GET:/channels/dataframe"]("/channels/dataframe", "", rsp), error::OK);
    {
        rapidjson::Document d;
        d.Parse(rsp.c_str());
        ASSERT_TRUE(!d.HasParseError() && d.IsObject());
        ASSERT_TRUE(d.HasMember("channels") && d["channels"].IsArray());
        ASSERT_EQ(d["channels"].Size(), rapidjson::SizeType(0));
    }

    ASSERT_EQ(routes["GET:/channels/dataframe"]("/channels/dataframe", "", rsp), error::OK);
    ASSERT_TRUE(rsp.find("\"channels\"") != std::string::npos);

    // import
    const std::string tmp_csv = upload + "/users.csv";
    {
        FILE* fp = fopen(tmp_csv.c_str(), "wb");
        ASSERT_TRUE(fp != nullptr);
        const char* csv = "id,name,score\n1,Alice,1.5\n2,Bob,2.0\n";
        fwrite(csv, 1, strlen(csv), fp);
        fclose(fp);
    }
    std::string req = "{\"filename\":\"users.csv\",\"tmp_path\":\"" + tmp_csv + "\"}";
    ASSERT_EQ(routes["POST:/channels/dataframe/import"]("/channels/dataframe/import", req, rsp), error::OK);
    ASSERT_TRUE(rsp.find("\"name\":\"users\"") != std::string::npos);

    // T27: 冲突时自动追加时间戳后缀
    const std::string tmp_csv_2 = upload + "/users_dup.csv";
    {
        FILE* fp = fopen(tmp_csv_2.c_str(), "wb");
        ASSERT_TRUE(fp != nullptr);
        const char* csv = "id,name,score\n3,Coco,3.5\n";
        fwrite(csv, 1, strlen(csv), fp);
        fclose(fp);
    }
    req = "{\"filename\":\"users.csv\",\"tmp_path\":\"" + tmp_csv_2 + "\"}";
    ASSERT_EQ(routes["POST:/channels/dataframe/import"]("/channels/dataframe/import", req, rsp), error::OK);
    ASSERT_TRUE(rsp.find("\"name\":\"users_") != std::string::npos);

    // preview
    ASSERT_EQ(routes["POST:/channels/dataframe/preview"]("/channels/dataframe/preview", R"({"name":"users"})", rsp),
              error::OK);
    ASSERT_TRUE(rsp.find("\"columns\"") != std::string::npos);
    ASSERT_TRUE(rsp.find("\"rows\":2") != std::string::npos);
    // T30: 预览不存在通道 -> 404
    ASSERT_EQ(routes["POST:/channels/dataframe/preview"]("/channels/dataframe/preview", R"({"name":"not_found"})", rsp),
              error::NOT_FOUND);

    // rename
    ASSERT_EQ(routes["POST:/channels/dataframe/rename"]("/channels/dataframe/rename",
              R"({"name":"users","new_name":"users_v2"})", rsp), error::OK);
    ASSERT_TRUE(rsp.find("users_v2") != std::string::npos);
    // T32: rename 源不存在 -> 404
    ASSERT_EQ(routes["POST:/channels/dataframe/rename"]("/channels/dataframe/rename",
              R"({"name":"missing","new_name":"x"})", rsp), error::NOT_FOUND);
    // T33: rename 目标已存在 -> 409
    ASSERT_EQ(p.Register("exists_name", std::static_pointer_cast<IChannel>(MakeChannel("exists_name", 1))), 0);
    ASSERT_EQ(routes["POST:/channels/dataframe/rename"]("/channels/dataframe/rename",
              R"({"name":"users_v2","new_name":"exists_name"})", rsp), error::CONFLICT);

    // delete
    ASSERT_EQ(routes["POST:/channels/dataframe/delete"]("/channels/dataframe/delete",
              R"({"name":"users_v2"})", rsp), error::OK);
    ASSERT_TRUE(rsp.find("\"ok\":true") != std::string::npos);
    ASSERT_TRUE(GetDataFrame(p, "users_v2") == nullptr);
    // T35: 删除不存在 -> 404
    ASSERT_EQ(routes["POST:/channels/dataframe/delete"]("/channels/dataframe/delete",
              R"({"name":"users_v2"})", rsp), error::NOT_FOUND);

    // T27a: import with bad tmp_path -> 400
    ASSERT_EQ(routes["POST:/channels/dataframe/import"]("/channels/dataframe/import",
              R"({"filename":"bad.csv","tmp_path":"/tmp/not_exists.csv"})", rsp), error::BAD_REQUEST);

    // T36: 全整数列 -> INT64
    {
        const std::string f = upload + "/ints.csv";
        FILE* fp = fopen(f.c_str(), "wb");
        ASSERT_TRUE(fp != nullptr);
        const char* csv = "v\n1\n2\n3\n";
        fwrite(csv, 1, strlen(csv), fp);
        fclose(fp);
        req = "{\"filename\":\"ints.csv\",\"tmp_path\":\"" + f + "\"}";
        ASSERT_EQ(routes["POST:/channels/dataframe/import"]("/channels/dataframe/import", req, rsp), error::OK);
        rapidjson::Document d;
        d.Parse(rsp.c_str());
        ASSERT_TRUE(d.IsObject() && d.HasMember("schema"));
        ASSERT_EQ(d["schema"][0]["type"].GetInt(), static_cast<int>(DataType::INT64));
    }
    // T37: 含小数 -> DOUBLE
    {
        const std::string f = upload + "/double.csv";
        FILE* fp = fopen(f.c_str(), "wb");
        ASSERT_TRUE(fp != nullptr);
        const char* csv = "v\n1.5\n2.0\n";
        fwrite(csv, 1, strlen(csv), fp);
        fclose(fp);
        req = "{\"filename\":\"double.csv\",\"tmp_path\":\"" + f + "\"}";
        ASSERT_EQ(routes["POST:/channels/dataframe/import"]("/channels/dataframe/import", req, rsp), error::OK);
        rapidjson::Document d;
        d.Parse(rsp.c_str());
        ASSERT_EQ(d["schema"][0]["type"].GetInt(), static_cast<int>(DataType::DOUBLE));
    }
    // T38: 混合整数+字符串 -> STRING
    {
        const std::string f = upload + "/mixed.csv";
        FILE* fp = fopen(f.c_str(), "wb");
        ASSERT_TRUE(fp != nullptr);
        const char* csv = "v\n1\nabc\n";
        fwrite(csv, 1, strlen(csv), fp);
        fclose(fp);
        req = "{\"filename\":\"mixed.csv\",\"tmp_path\":\"" + f + "\"}";
        ASSERT_EQ(routes["POST:/channels/dataframe/import"]("/channels/dataframe/import", req, rsp), error::OK);
        rapidjson::Document d;
        d.Parse(rsp.c_str());
        ASSERT_EQ(d["schema"][0]["type"].GetInt(), static_cast<int>(DataType::STRING));
    }
    // T39: 含空值 -> STRING
    {
        const std::string f = upload + "/empty_cell.csv";
        FILE* fp = fopen(f.c_str(), "wb");
        ASSERT_TRUE(fp != nullptr);
        const char* csv = "v\n1\n\n3\n";
        fwrite(csv, 1, strlen(csv), fp);
        fclose(fp);
        req = "{\"filename\":\"empty_cell.csv\",\"tmp_path\":\"" + f + "\"}";
        ASSERT_EQ(routes["POST:/channels/dataframe/import"]("/channels/dataframe/import", req, rsp), error::OK);
        rapidjson::Document d;
        d.Parse(rsp.c_str());
        ASSERT_EQ(d["schema"][0]["type"].GetInt(), static_cast<int>(DataType::STRING));
    }
    // T40: 仅表头无数据 -> rows=0
    {
        const std::string f = upload + "/header_only.csv";
        FILE* fp = fopen(f.c_str(), "wb");
        ASSERT_TRUE(fp != nullptr);
        const char* csv = "a,b,c\n";
        fwrite(csv, 1, strlen(csv), fp);
        fclose(fp);
        req = "{\"filename\":\"header_only.csv\",\"tmp_path\":\"" + f + "\"}";
        ASSERT_EQ(routes["POST:/channels/dataframe/import"]("/channels/dataframe/import", req, rsp), error::OK);
        rapidjson::Document d;
        d.Parse(rsp.c_str());
        ASSERT_TRUE(d.IsObject() && d.HasMember("rows"));
        ASSERT_EQ(d["rows"].GetInt(), 0);
    }

    std::puts("[PASS] T24-T40 http routes");
}

int main() {
    std::puts("=== CatalogPlugin Tests ===");

    TestRegistryBasic();
    TestRestartRecover();
    TestConcurrency();
    TestPersistFail();
    TestOperatorRegistry();
    TestOperatorCatalog();
    TestCppPluginLifecycle();
    TestOperatorDbPathOption();
    TestHttpRoutes();

    std::puts("=== All CatalogPlugin tests passed ===");
    return 0;
}
