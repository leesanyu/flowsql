#include <cassert>
#include <cstdio>
#include <filesystem>
#include <string>
#include <unistd.h>

#include <arrow/io/api.h>
#include <arrow/ipc/api.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <common/error_code.h>
#include <common/loader.hpp>
#include <framework/core/dataframe.h>
#include <framework/interfaces/idatabase_channel.h>
#include <framework/interfaces/idatabase_factory.h>
#include <framework/interfaces/ichannel_registry.h>
#include <framework/interfaces/idataframe_channel.h>
#include <framework/interfaces/irouter_handle.h>

using namespace flowsql;

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

static std::shared_ptr<arrow::Buffer> SerializeBatch(const std::shared_ptr<arrow::RecordBatch>& batch) {
    auto sink = arrow::io::BufferOutputStream::Create().ValueOrDie();
    auto ipc_w = arrow::ipc::MakeStreamWriter(sink, batch->schema()).ValueOrDie();
    (void)ipc_w->WriteRecordBatch(*batch);
    (void)ipc_w->Close();
    return sink->Finish().ValueOrDie();
}

static int64_t CountRowsInIpc(const uint8_t* data, size_t len) {
    auto buf = std::make_shared<arrow::Buffer>(data, len);
    auto reader = arrow::ipc::RecordBatchStreamReader::Open(std::make_shared<arrow::io::BufferReader>(buf)).ValueOrDie();
    int64_t rows = 0;
    while (true) {
        auto batch = reader->Next().ValueOrDie();
        if (!batch) break;
        rows += batch->num_rows();
    }
    return rows;
}

static std::string MakeReq(const std::string& sql) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("sql");
    w.String(sql.c_str());
    w.EndObject();
    return buf.GetString();
}

static fnRouterHandler FindExecuteHandler(PluginLoader* loader) {
    fnRouterHandler h;
    loader->Traverse(IID_ROUTER_HANDLE, [&](void* p) -> int {
        auto* rh = static_cast<IRouterHandle*>(p);
        rh->EnumRoutes([&](const RouteItem& item) {
            if (item.method == "POST" && item.uri == "/tasks/instant/execute") {
                h = item.handler;
            }
        });
        return h ? -1 : 0;
    });
    return h;
}

static void SeedSourceTable(IDatabaseChannel* db, const char* table) {
    auto schema = arrow::schema({
        arrow::field("id", arrow::int64()),
        arrow::field("name", arrow::utf8()),
        arrow::field("score", arrow::float64()),
    });
    arrow::Int64Builder id_b;
    arrow::StringBuilder name_b;
    arrow::DoubleBuilder score_b;
    (void)id_b.Append(1); (void)id_b.Append(2); (void)id_b.Append(3);
    (void)name_b.Append("a"); (void)name_b.Append("b"); (void)name_b.Append("c");
    (void)score_b.Append(10.0); (void)score_b.Append(20.0); (void)score_b.Append(30.0);
    auto batch = arrow::RecordBatch::Make(schema, 3, {
        id_b.Finish().ValueOrDie(),
        name_b.Finish().ValueOrDie(),
        score_b.Finish().ValueOrDie(),
    });

    IBatchWriter* writer = nullptr;
    ASSERT_EQ(db->CreateWriter(table, &writer), 0);
    ASSERT_TRUE(writer != nullptr);
    auto buf = SerializeBatch(batch);
    ASSERT_EQ(writer->Write(buf->data(), static_cast<size_t>(buf->size())), 0);
    BatchWriteStats stats;
    writer->Close(&stats);
    writer->Release();
    ASSERT_EQ(stats.rows_written, 3);
}

static int64_t QueryCount(IDatabaseChannel* db, const std::string& query) {
    IBatchReader* reader = nullptr;
    if (db->CreateReader(query.c_str(), &reader) != 0 || !reader) return -1;
    int64_t rows = 0;
    while (true) {
        const uint8_t* data = nullptr;
        size_t len = 0;
        int rc = reader->Next(&data, &len);
        if (rc == 1) break;
        if (rc != 0) {
            rows = -1;
            break;
        }
        rows += CountRowsInIpc(data, len);
    }
    reader->Close();
    reader->Release();
    return rows;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::puts("=== Scheduler E2E Tests (Story 9.3) ===");

    const std::string suffix = std::to_string(::getpid());
    const std::filesystem::path db_path = std::filesystem::temp_directory_path() / ("flowsql_s9_3_" + suffix + ".db");
    const std::filesystem::path data_dir = std::filesystem::temp_directory_path() / ("flowsql_s9_3_df_" + suffix);
    std::filesystem::remove(db_path);
    std::filesystem::create_directories(data_dir);

    PluginLoader* loader = PluginLoader::Single();
    const char* libs[] = {"libflowsql_database.so", "libflowsql_catalog.so", "libflowsql_scheduler.so"};
    std::string db_opt = "type=sqlite;name=local;path=" + db_path.string();
    std::string catalog_opt = "data_dir=" + data_dir.string();
    const char* opts[] = {db_opt.c_str(), catalog_opt.c_str(), nullptr};
    ASSERT_EQ(loader->Load(get_absolute_process_path(), libs, opts, 3), 0);
    std::puts("[INFO] plugins loaded");
    ASSERT_EQ(loader->StartAll(), 0);
    std::puts("[INFO] plugins started");

    auto* factory = static_cast<IDatabaseFactory*>(loader->First(IID_DATABASE_FACTORY));
    auto* registry = static_cast<IChannelRegistry*>(loader->First(IID_CHANNEL_REGISTRY));
    ASSERT_TRUE(factory != nullptr);
    ASSERT_TRUE(registry != nullptr);
    auto* db = dynamic_cast<IDatabaseChannel*>(factory->Get("sqlite", "local"));
    ASSERT_TRUE(db != nullptr);

    SeedSourceTable(db, "src");
    std::puts("[INFO] source table seeded");
    auto exec = FindExecuteHandler(loader);
    ASSERT_TRUE(exec != nullptr);
    std::puts("[INFO] execute handler ready");

    // T18: INTO dataframe.result 后可通过 Registry 读取
    {
        std::string rsp;
        int32_t rc = exec("/tasks/instant/execute",
                          MakeReq("SELECT * FROM sqlite.local.src INTO dataframe.result"), rsp);
        ASSERT_EQ(rc, error::OK);
        auto ch = std::dynamic_pointer_cast<IDataFrameChannel>(registry->Get("result"));
        ASSERT_TRUE(ch != nullptr);
        ASSERT_EQ(QueryCount(db, "SELECT * FROM src"), 3);
    }
    std::puts("[PASS] T18");

    // T19: FROM dataframe.result INTO sqlite.local.t2
    {
        std::string rsp;
        int32_t rc = exec("/tasks/instant/execute",
                          MakeReq("SELECT * FROM dataframe.result INTO sqlite.local.t2"), rsp);
        ASSERT_EQ(rc, error::OK);
        ASSERT_EQ(QueryCount(db, "SELECT * FROM t2"), 3);
    }
    std::puts("[PASS] T19");

    // T20: FROM dataframe.<不存在> 返回 NOT_FOUND
    {
        std::string rsp;
        int32_t rc = exec("/tasks/instant/execute",
                          MakeReq("SELECT * FROM dataframe.not_exists INTO sqlite.local.t3"), rsp);
        ASSERT_EQ(rc, error::NOT_FOUND);
    }
    std::puts("[PASS] T20");

    // T21: INTO dataframe.result 覆盖语义（第二次覆盖第一次）
    {
        std::string rsp;
        ASSERT_EQ(exec("/tasks/instant/execute",
                       MakeReq("SELECT * FROM sqlite.local.src WHERE id <= 2 INTO dataframe.result"), rsp),
                  error::OK);
        auto ch1 = std::dynamic_pointer_cast<IDataFrameChannel>(registry->Get("result"));
        ASSERT_TRUE(ch1 != nullptr);

        ASSERT_EQ(exec("/tasks/instant/execute",
                       MakeReq("SELECT * FROM sqlite.local.src WHERE id > 2 INTO dataframe.result"), rsp),
                  error::OK);
        auto ch2 = std::dynamic_pointer_cast<IDataFrameChannel>(registry->Get("result"));
        ASSERT_TRUE(ch2 != nullptr);

        DataFrame out;
        ASSERT_EQ(ch2->Read(&out), 0);
        ASSERT_EQ(out.RowCount(), 1);
    }
    std::puts("[PASS] T21");

    // T22: 无 INTO 的匿名结果行为不变（仅响应返回，不落入 Registry 新名称）
    {
        std::string rsp;
        size_t before = 0;
        registry->List([&](const char*, std::shared_ptr<IChannel>) { ++before; });
        ASSERT_EQ(exec("/tasks/instant/execute", MakeReq("SELECT * FROM sqlite.local.src"), rsp), error::OK);
        rapidjson::Document doc;
        doc.Parse(rsp.c_str());
        ASSERT_TRUE(!doc.HasParseError() && doc.IsObject());
        ASSERT_TRUE(doc.HasMember("rows"));
        ASSERT_EQ(doc["rows"].GetInt64(), 3);
        size_t after = 0;
        registry->List([&](const char*, std::shared_ptr<IChannel>) { ++after; });
        ASSERT_EQ(after, before);
    }
    std::puts("[PASS] T22");

    // T23: 跨通道链路 + 内置算子 passthrough
    {
        std::string rsp;
        ASSERT_EQ(exec("/tasks/instant/execute",
                       MakeReq("SELECT * FROM sqlite.local.src USING builtin.passthrough INTO dataframe.out"), rsp),
                  error::OK);
        ASSERT_TRUE(std::dynamic_pointer_cast<IDataFrameChannel>(registry->Get("out")) != nullptr);
        ASSERT_EQ(exec("/tasks/instant/execute",
                       MakeReq("SELECT * FROM dataframe.out INTO sqlite.local.t_passthrough"), rsp),
                  error::OK);
        ASSERT_EQ(QueryCount(db, "SELECT * FROM t_passthrough"), 3);
    }
    std::puts("[PASS] T23");

    exec = fnRouterHandler();
    loader->StopAll();
    loader->Unload();
    std::filesystem::remove(db_path);
    std::filesystem::remove_all(data_dir);

    std::puts("=== All Scheduler E2E tests passed ===");
    return 0;
}
