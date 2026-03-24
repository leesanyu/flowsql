#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <unistd.h>

#include <arrow/io/api.h>
#include <arrow/ipc/api.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <common/error_code.h>
#include <common/loader.hpp>
#include <framework/core/dataframe.h>
#include <framework/core/dataframe_channel.h>
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

static fnRouterHandler FindRouteHandler(PluginLoader* loader, const char* method, const char* uri) {
    fnRouterHandler h;
    loader->Traverse(IID_ROUTER_HANDLE, [&](void* p) -> int {
        auto* rh = static_cast<IRouterHandle*>(p);
        rh->EnumRoutes([&](const RouteItem& item) {
            if (item.method == method && item.uri == uri) {
                h = item.handler;
            }
        });
        return h ? -1 : 0;
    });
    return h;
}

static fnRouterHandler FindExecuteHandler(PluginLoader* loader) {
    return FindRouteHandler(loader, "POST", "/tasks/instant/execute");
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
    const std::filesystem::path operator_db_dir = std::filesystem::temp_directory_path() / ("flowsql_s9_3_catalog_" + suffix);
    std::filesystem::remove(db_path);
    std::filesystem::create_directories(data_dir);
    std::filesystem::create_directories(operator_db_dir);

    PluginLoader* loader = PluginLoader::Single();
    const char* libs[] = {"libflowsql_database.so", "libflowsql_catalog.so", "libflowsql_scheduler.so"};
    std::string db_opt = "type=sqlite;name=local;path=" + db_path.string();
    std::string catalog_opt = "data_dir=" + data_dir.string() + ";operator_db_dir=" + operator_db_dir.string();
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
    auto activate = FindRouteHandler(loader, "POST", "/operators/activate");
    auto deactivate = FindRouteHandler(loader, "POST", "/operators/deactivate");
    ASSERT_TRUE(exec != nullptr);
    ASSERT_TRUE(activate != nullptr);
    ASSERT_TRUE(deactivate != nullptr);
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
        ASSERT_TRUE(doc.HasMember("result_row_count"));
        ASSERT_EQ(doc["rows"].GetInt64(), 3);
        ASSERT_EQ(doc["result_row_count"].GetInt64(), 3);
        size_t after = 0;
        registry->List([&](const char*, std::shared_ptr<IChannel>) { ++after; });
        ASSERT_EQ(after, before);
    }
    std::puts("[PASS] T22");

    // T23: 跨通道链路 + 内置算子 passthrough
    {
        std::string activate_rsp;
        ASSERT_EQ(activate("/operators/activate", R"({"name":"builtin.passthrough"})", activate_rsp), error::OK);

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

    // T24: 去激活仅阻止新任务，不中断已 running 任务
    {
        std::string rsp;
        ASSERT_EQ(activate("/operators/activate", R"({"name":"builtin.passthrough"})", rsp), error::OK);

        int32_t running_rc = error::INTERNAL_ERROR;
        std::thread worker([&]() {
            std::string local_rsp;
            running_rc = exec("/tasks/instant/execute",
                              MakeReq("SELECT * FROM sqlite.local.src USING builtin.passthrough "
                                      "WITH delay_ms=800 INTO dataframe.running_out"),
                              local_rsp);
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        ASSERT_EQ(deactivate("/operators/deactivate", R"({"name":"builtin.passthrough"})", rsp), error::OK);

        worker.join();
        ASSERT_EQ(running_rc, error::OK);
        ASSERT_TRUE(std::dynamic_pointer_cast<IDataFrameChannel>(registry->Get("running_out")) != nullptr);

        ASSERT_EQ(exec("/tasks/instant/execute",
                       MakeReq("SELECT * FROM sqlite.local.src USING builtin.passthrough INTO dataframe.blocked"), rsp),
                  error::CONFLICT);
    }
    std::puts("[PASS] T24");

    // T25: 双算子串行链路成功（每个算子独立 WITH）
    {
        std::string rsp;
        ASSERT_EQ(activate("/operators/activate", R"({"name":"builtin.passthrough"})", rsp), error::OK);
        ASSERT_EQ(exec("/tasks/instant/execute",
                       MakeReq("SELECT * FROM sqlite.local.src "
                               "USING builtin.passthrough WITH delay_ms=10 "
                               "THEN builtin.passthrough WITH delay_ms=0 "
                               "INTO dataframe.chain_ok"),
                       rsp),
                  error::OK);
        auto ch = std::dynamic_pointer_cast<IDataFrameChannel>(registry->Get("chain_ok"));
        ASSERT_TRUE(ch != nullptr);
        DataFrame out;
        ASSERT_EQ(ch->Read(&out), 0);
        ASSERT_EQ(out.RowCount(), 3);
    }
    std::puts("[PASS] T25");

    // T26: 双算子链第 2 步失败（独立 WITH 不复用）
    {
        std::string rsp;
        ASSERT_EQ(activate("/operators/activate", R"({"name":"builtin.passthrough"})", rsp), error::OK);
        size_t before_cnt = 0;
        registry->List([&](const char*, std::shared_ptr<IChannel>) { ++before_cnt; });
        int32_t rc = exec("/tasks/instant/execute",
                          MakeReq("SELECT * FROM sqlite.local.src "
                                  "USING builtin.passthrough WITH force_fail=0 "
                                  "THEN builtin.passthrough WITH force_fail=1 "
                                  "INTO dataframe.chain_fail"),
                          rsp);
        ASSERT_EQ(rc, error::INTERNAL_ERROR);
        ASSERT_TRUE(std::dynamic_pointer_cast<IDataFrameChannel>(registry->Get("chain_fail")) == nullptr);

        size_t after_cnt = 0;
        registry->List([&](const char*, std::shared_ptr<IChannel>) { ++after_cnt; });
        ASSERT_EQ(after_cnt, before_cnt);  // 失败后不应新增/泄漏具名通道

        // 失败后再次执行，验证执行器状态未污染
        ASSERT_EQ(exec("/tasks/instant/execute",
                       MakeReq("SELECT * FROM sqlite.local.src "
                               "USING builtin.passthrough WITH force_fail=0 "
                               "THEN builtin.passthrough WITH force_fail=0 "
                               "INTO dataframe.chain_after_fail"),
                       rsp),
                  error::OK);
        ASSERT_TRUE(std::dynamic_pointer_cast<IDataFrameChannel>(registry->Get("chain_after_fail")) != nullptr);
    }
    std::puts("[PASS] T26");

    // T27: 多源 + USING builtin.passthrough 走统一多输入入口（默认回退到首输入）
    {
        std::string rsp;
        int32_t rc = exec("/tasks/instant/execute",
                          MakeReq("SELECT * FROM dataframe.result,dataframe.out "
                                  "USING builtin.passthrough INTO dataframe.multi_ok"),
                          rsp);
        ASSERT_EQ(rc, error::OK);
        auto ch = std::dynamic_pointer_cast<IDataFrameChannel>(registry->Get("multi_ok"));
        ASSERT_TRUE(ch != nullptr);

        DataFrame out;
        ASSERT_EQ(ch->Read(&out), 0);
        ASSERT_EQ(out.RowCount(), 1);  // dataframe.result 在 T21 被覆盖为 1 行
        ASSERT_EQ(std::get<int64_t>(out.GetRow(0)[0]), 3);
    }
    std::puts("[PASS] T27");

    // T28: 多源无 USING 算子应报 BAD_REQUEST
    {
        std::string rsp;
        int32_t rc = exec("/tasks/instant/execute",
                          MakeReq("SELECT * FROM dataframe.result,dataframe.out INTO dataframe.multi_no_op"),
                          rsp);
        ASSERT_EQ(rc, error::BAD_REQUEST);
        ASSERT_TRUE(rsp.find("multi-source FROM requires USING operator") != std::string::npos);
    }
    std::puts("[PASS] T28");

    // T29: 多源包含非 dataframe.* 应报 BAD_REQUEST
    {
        std::string rsp;
        int32_t rc = exec("/tasks/instant/execute",
                          MakeReq("SELECT * FROM sqlite.local.src,dataframe.result "
                                  "USING builtin.passthrough INTO dataframe.multi_mixed"),
                          rsp);
        ASSERT_EQ(rc, error::BAD_REQUEST);
        ASSERT_TRUE(rsp.find("multi-source FROM only supports dataframe.* in Sprint 10") != std::string::npos);
    }
    std::puts("[PASS] T29");

    // T30: 多源 + WHERE（Sprint10 V1）应报 BAD_REQUEST
    {
        std::string rsp;
        int32_t rc = exec("/tasks/instant/execute",
                          MakeReq("SELECT * FROM dataframe.result,dataframe.out "
                                  "WHERE id > 1 USING builtin.passthrough INTO dataframe.multi_where"),
                          rsp);
        ASSERT_EQ(rc, error::BAD_REQUEST);
        ASSERT_TRUE(rsp.find("multi-source FROM does not support WHERE in Sprint 10") != std::string::npos);
    }
    std::puts("[PASS] T30");

    // T31: INTO 非法目标（未限定名）应报 BAD_REQUEST
    {
        std::string rsp;
        int32_t rc = exec("/tasks/instant/execute",
                          MakeReq("SELECT * FROM sqlite.local.src INTO t2"),
                          rsp);
        ASSERT_EQ(rc, error::BAD_REQUEST);
        ASSERT_TRUE(rsp.find("invalid INTO destination") != std::string::npos);
    }
    std::puts("[PASS] T31");

    // T32: 激活 concat/hstack
    {
        std::string rsp;
        ASSERT_EQ(activate("/operators/activate", R"({"name":"builtin.concat"})", rsp), error::OK);
        ASSERT_EQ(activate("/operators/activate", R"({"name":"builtin.hstack"})", rsp), error::OK);
    }
    std::puts("[PASS] T32");

    // T33: concat 成功（按行合并）
    {
        std::string rsp;
        int32_t rc = exec("/tasks/instant/execute",
                          MakeReq("SELECT * FROM dataframe.out,dataframe.chain_ok "
                                  "USING builtin.concat INTO dataframe.concat_ok"),
                          rsp);
        ASSERT_EQ(rc, error::OK);
        auto ch = std::dynamic_pointer_cast<IDataFrameChannel>(registry->Get("concat_ok"));
        ASSERT_TRUE(ch != nullptr);

        DataFrame out;
        ASSERT_EQ(ch->Read(&out), 0);
        ASSERT_EQ(out.RowCount(), 6);
        ASSERT_EQ(out.GetSchema().size(), 3u);
    }
    std::puts("[PASS] T33");

    // T34: concat schema 不兼容应失败
    {
        std::string rsp;
        ASSERT_EQ(exec("/tasks/instant/execute",
                       MakeReq("SELECT id FROM sqlite.local.src INTO dataframe.only_id"),
                       rsp),
                  error::OK);

        int32_t rc = exec("/tasks/instant/execute",
                          MakeReq("SELECT * FROM dataframe.out,dataframe.only_id "
                                  "USING builtin.concat INTO dataframe.concat_bad"),
                          rsp);
        ASSERT_EQ(rc, error::INTERNAL_ERROR);
        ASSERT_TRUE(rsp.find("concat schema mismatch") != std::string::npos);
    }
    std::puts("[PASS] T34");

    // T35: hstack 成功（按列合并）
    {
        std::string rsp;
        int32_t rc = exec("/tasks/instant/execute",
                          MakeReq("SELECT * FROM dataframe.out,dataframe.chain_ok "
                                  "USING builtin.hstack INTO dataframe.hstack_ok"),
                          rsp);
        ASSERT_EQ(rc, error::OK);
        auto ch = std::dynamic_pointer_cast<IDataFrameChannel>(registry->Get("hstack_ok"));
        ASSERT_TRUE(ch != nullptr);

        DataFrame out;
        ASSERT_EQ(ch->Read(&out), 0);
        ASSERT_EQ(out.RowCount(), 3);
        ASSERT_EQ(out.GetSchema().size(), 6u);
    }
    std::puts("[PASS] T35");

    // T36: hstack 行数不一致应失败
    {
        std::string rsp;
        int32_t rc = exec("/tasks/instant/execute",
                          MakeReq("SELECT * FROM dataframe.result,dataframe.out "
                                  "USING builtin.hstack INTO dataframe.hstack_bad"),
                          rsp);
        ASSERT_EQ(rc, error::INTERNAL_ERROR);
        ASSERT_TRUE(rsp.find("hstack row count mismatch") != std::string::npos);
    }
    std::puts("[PASS] T36");

    // T37: concat 覆盖多类型（INT32/INT64/FLOAT/DOUBLE/STRING/BOOL）
    {
        auto build_typed_channel = [&](const char* name, int32_t base) {
            auto ch = std::make_shared<DataFrameChannel>("dataframe", name);
            ch->Open();

            DataFrame df;
            df.SetSchema({
                {"c_i32", DataType::INT32, 0, ""},
                {"c_i64", DataType::INT64, 0, ""},
                {"c_f32", DataType::FLOAT, 0, ""},
                {"c_f64", DataType::DOUBLE, 0, ""},
                {"c_str", DataType::STRING, 0, ""},
                {"c_bool", DataType::BOOLEAN, 0, ""},
            });
            df.AppendRow({base + 1, int64_t(base + 1000), float(base + 0.5f), double(base + 0.25), std::string("n") + std::to_string(base + 1), true});
            df.AppendRow({base + 2, int64_t(base + 2000), float(base + 1.5f), double(base + 1.25), std::string("n") + std::to_string(base + 2), false});
            ASSERT_EQ(ch->Write(&df), 0);

            (void)registry->Unregister(name);
            ASSERT_EQ(registry->Register(name, std::static_pointer_cast<IChannel>(ch)), 0);
        };

        build_typed_channel("typed_a", 10);
        build_typed_channel("typed_b", 20);

        std::string rsp;
        int32_t rc = exec("/tasks/instant/execute",
                          MakeReq("SELECT * FROM dataframe.typed_a,dataframe.typed_b "
                                  "USING builtin.concat INTO dataframe.concat_types"),
                          rsp);
        ASSERT_EQ(rc, error::OK);
        auto ch = std::dynamic_pointer_cast<IDataFrameChannel>(registry->Get("concat_types"));
        ASSERT_TRUE(ch != nullptr);

        DataFrame out;
        ASSERT_EQ(ch->Read(&out), 0);
        ASSERT_EQ(out.RowCount(), 4);
        auto schema = out.GetSchema();
        ASSERT_EQ(schema.size(), size_t(6));
        ASSERT_EQ(schema[0].type, DataType::INT32);
        ASSERT_EQ(schema[1].type, DataType::INT64);
        ASSERT_EQ(schema[2].type, DataType::FLOAT);
        ASSERT_EQ(schema[3].type, DataType::DOUBLE);
        ASSERT_EQ(schema[4].type, DataType::STRING);
        ASSERT_EQ(schema[5].type, DataType::BOOLEAN);

        auto row3 = out.GetRow(3);
        ASSERT_EQ(std::get<int32_t>(row3[0]), 22);
        ASSERT_EQ(std::get<int64_t>(row3[1]), 2020);
        ASSERT_EQ(std::get<std::string>(row3[4]), "n22");
        ASSERT_EQ(std::get<bool>(row3[5]), false);
    }
    std::puts("[PASS] T37");

    // T38: hstack 覆盖多类型（按列合并）
    {
        auto left = std::make_shared<DataFrameChannel>("dataframe", "hleft");
        auto right = std::make_shared<DataFrameChannel>("dataframe", "hright");
        left->Open();
        right->Open();

        DataFrame ldf;
        ldf.SetSchema({
            {"a_i32", DataType::INT32, 0, ""},
            {"a_str", DataType::STRING, 0, ""},
            {"a_bool", DataType::BOOLEAN, 0, ""},
        });
        ldf.AppendRow({int32_t(1), std::string("x"), true});
        ldf.AppendRow({int32_t(2), std::string("y"), false});
        ASSERT_EQ(left->Write(&ldf), 0);

        DataFrame rdf;
        rdf.SetSchema({
            {"b_i64", DataType::INT64, 0, ""},
            {"b_f32", DataType::FLOAT, 0, ""},
            {"b_f64", DataType::DOUBLE, 0, ""},
        });
        rdf.AppendRow({int64_t(100), float(1.5f), double(10.25)});
        rdf.AppendRow({int64_t(200), float(2.5f), double(20.25)});
        ASSERT_EQ(right->Write(&rdf), 0);

        (void)registry->Unregister("hleft");
        (void)registry->Unregister("hright");
        ASSERT_EQ(registry->Register("hleft", std::static_pointer_cast<IChannel>(left)), 0);
        ASSERT_EQ(registry->Register("hright", std::static_pointer_cast<IChannel>(right)), 0);

        std::string rsp;
        int32_t rc = exec("/tasks/instant/execute",
                          MakeReq("SELECT * FROM dataframe.hleft,dataframe.hright "
                                  "USING builtin.hstack INTO dataframe.hstack_types"),
                          rsp);
        ASSERT_EQ(rc, error::OK);
        auto ch = std::dynamic_pointer_cast<IDataFrameChannel>(registry->Get("hstack_types"));
        ASSERT_TRUE(ch != nullptr);

        DataFrame out;
        ASSERT_EQ(ch->Read(&out), 0);
        ASSERT_EQ(out.RowCount(), 2);
        auto schema = out.GetSchema();
        ASSERT_EQ(schema.size(), size_t(6));
        ASSERT_EQ(schema[0].type, DataType::INT32);
        ASSERT_EQ(schema[1].type, DataType::STRING);
        ASSERT_EQ(schema[2].type, DataType::BOOLEAN);
        ASSERT_EQ(schema[3].type, DataType::INT64);
        ASSERT_EQ(schema[4].type, DataType::FLOAT);
        ASSERT_EQ(schema[5].type, DataType::DOUBLE);

        auto row0 = out.GetRow(0);
        ASSERT_EQ(std::get<int32_t>(row0[0]), 1);
        ASSERT_EQ(std::get<std::string>(row0[1]), "x");
        ASSERT_EQ(std::get<bool>(row0[2]), true);
        ASSERT_EQ(std::get<int64_t>(row0[3]), 100);
    }
    std::puts("[PASS] T38");

    exec = fnRouterHandler();
    loader->StopAll();
    loader->Unload();
    std::filesystem::remove(db_path);
    std::filesystem::remove_all(data_dir);
    std::filesystem::remove_all(operator_db_dir);

    std::puts("=== All Scheduler E2E tests passed ===");
    return 0;
}
