#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <arrow/ipc/api.h>

#include <common/loader.hpp>
#include <framework/core/dataframe.h>
#include <framework/core/sql_parser.h>
#include <framework/interfaces/idatabase_channel.h>
#include <framework/interfaces/idatabase_factory.h>

using namespace flowsql;

// ============================================================
// 全局：本次测试运行的唯一后缀（避免重跑时表名冲突）
// ============================================================
static std::string g_suffix;
static std::string TABLE_USERS;   // e2e_users_<suffix>
static std::string TABLE_AUTO;    // e2e_auto_<suffix>
static std::string TABLE_CITIES;  // e2e_cities_<suffix>
static std::string TABLE_COPY;    // e2e_copy_<suffix>
static std::string TABLE_ADULTS;  // e2e_adults_<suffix>

// ============================================================
// 辅助：获取 MySQL 连接参数（从环境变量读取）
// ============================================================
static std::unordered_map<std::string, std::string> GetMysqlParams() {
    std::unordered_map<std::string, std::string> p;
    p["host"]     = getenv("MYSQL_HOST")     ? getenv("MYSQL_HOST")     : "127.0.0.1";
    p["port"]     = getenv("MYSQL_PORT")     ? getenv("MYSQL_PORT")     : "3306";
    p["user"]     = getenv("MYSQL_USER")     ? getenv("MYSQL_USER")     : "flowsql_user";
    p["password"] = getenv("MYSQL_PASSWORD") ? getenv("MYSQL_PASSWORD") : "flowSQL@user";
    p["database"] = getenv("MYSQL_DATABASE") ? getenv("MYSQL_DATABASE") : "flowsql_db";
    p["charset"]  = "utf8mb4";
    return p;
}

// 辅助：构建 plugin option 字符串
static std::string MakeMysqlOption(const std::string& name) {
    auto p = GetMysqlParams();
    return "type=mysql;name=" + name +
           ";host=" + p["host"] +
           ";port=" + p["port"] +
           ";user=" + p["user"] +
           ";password=" + p["password"] +
           ";database=" + p["database"] +
           ";charset=" + p["charset"];
}

// 辅助：将 Arrow RecordBatch 序列化为 IPC buffer
static std::shared_ptr<arrow::Buffer> SerializeBatch(
        const std::shared_ptr<arrow::RecordBatch>& batch) {
    auto sink = arrow::io::BufferOutputStream::Create().ValueOrDie();
    auto ipc_w = arrow::ipc::MakeStreamWriter(sink, batch->schema()).ValueOrDie();
    (void)ipc_w->WriteRecordBatch(*batch);
    (void)ipc_w->Close();
    return sink->Finish().ValueOrDie();
}

// 辅助：通过 IDatabaseChannel 写入一批数据（数据准备用）
static void WriteTestData(IDatabaseChannel* db_ch, const char* table,
                          const std::shared_ptr<arrow::RecordBatch>& batch) {
    auto buf = SerializeBatch(batch);
    IBatchWriter* writer = nullptr;
    int rc = db_ch->CreateWriter(table, &writer);
    assert(rc == 0 && writer != nullptr);
    rc = writer->Write(buf->data(), static_cast<size_t>(buf->size()));
    assert(rc == 0);
    BatchWriteStats stats;
    writer->Close(&stats);
    writer->Release();
}

// ============================================================
// Test 1: DatabasePlugin Option 配置解析（MySQL）
// ============================================================
void test_option_parsing() {
    printf("[TEST] DatabasePlugin Option parsing (MySQL)...\n");

    PluginLoader* loader = PluginLoader::Single();
    auto* factory = static_cast<IDatabaseFactory*>(loader->First(IID_DATABASE_FACTORY));
    assert(factory != nullptr);
    printf("  factory found via IQuerier\n");

    int count = 0;
    factory->List([&count](const char* type, const char* name) {
        printf("  configured: %s.%s\n", type, name);
        assert(std::string(type) == "mysql");
        assert(std::string(name) == "testdb");
        ++count;
    });
    assert(count == 1);

    printf("[PASS] DatabasePlugin Option parsing\n");
}

// ============================================================
// Test 2: MySQL 连接 + 通道属性
// ============================================================
void test_mysql_connect() {
    printf("[TEST] MySQL connect...\n");

    PluginLoader* loader = PluginLoader::Single();
    auto* factory = static_cast<IDatabaseFactory*>(loader->First(IID_DATABASE_FACTORY));
    assert(factory != nullptr);

    auto* ch = factory->Get("mysql", "testdb");
    assert(ch != nullptr);
    assert(std::string(ch->Type()) == "database");
    assert(std::string(ch->Catelog()) == "mysql");
    assert(std::string(ch->Name()) == "testdb");
    assert(ch->IsConnected());
    assert(ch->IsOpened());

    auto* ch2 = factory->Get("mysql", "nonexistent");
    assert(ch2 == nullptr);
    printf("  Get(nonexistent) returned nullptr, LastError: %s\n", factory->LastError());

    printf("[PASS] MySQL connect\n");
}

// ============================================================
// Test 3: CreateReader 执行 SELECT
// ============================================================
void test_create_reader() {
    printf("[TEST] CreateReader (SELECT)...\n");

    PluginLoader* loader = PluginLoader::Single();
    auto* factory = static_cast<IDatabaseFactory*>(loader->First(IID_DATABASE_FACTORY));
    auto* db_ch = dynamic_cast<IDatabaseChannel*>(factory->Get("mysql", "testdb"));
    assert(db_ch != nullptr);

    // 数据准备：通过 CreateWriter 写入测试数据（自动建表）
    {
        auto schema = arrow::schema({
            arrow::field("id",    arrow::int64()),
            arrow::field("name",  arrow::utf8()),
            arrow::field("score", arrow::float64()),
        });
        arrow::Int64Builder  id_b;
        arrow::StringBuilder name_b;
        arrow::DoubleBuilder  score_b;
        (void)id_b.Append(1); (void)id_b.Append(2); (void)id_b.Append(3);
        (void)name_b.Append("Alice"); (void)name_b.Append("Bob"); (void)name_b.Append("Charlie");
        (void)score_b.Append(95.5); (void)score_b.Append(87.3); (void)score_b.Append(92.1);
        auto batch = arrow::RecordBatch::Make(schema, 3, {
            id_b.Finish().ValueOrDie(),
            name_b.Finish().ValueOrDie(),
            score_b.Finish().ValueOrDie(),
        });
        WriteTestData(db_ch, TABLE_USERS.c_str(), batch);
    }

    IBatchReader* reader = nullptr;
    int rc = db_ch->CreateReader(("SELECT * FROM " + TABLE_USERS).c_str(), &reader);
    assert(rc == 0 && reader != nullptr);

    const uint8_t* buf = nullptr;
    size_t len = 0;
    int next_rc = reader->Next(&buf, &len);
    assert(next_rc == 0);
    assert(buf != nullptr && len > 0);
    printf("  Read batch: %zu bytes\n", len);

    next_rc = reader->Next(&buf, &len);
    assert(next_rc == 1);  // 已读完

    reader->Close();
    reader->Release();

    printf("[PASS] CreateReader\n");
}

// ============================================================
// Test 4: CreateWriter 写入 + 自动建表
// ============================================================
void test_create_writer() {
    printf("[TEST] CreateWriter (auto create table)...\n");

    PluginLoader* loader = PluginLoader::Single();
    auto* factory = static_cast<IDatabaseFactory*>(loader->First(IID_DATABASE_FACTORY));
    auto* db_ch = dynamic_cast<IDatabaseChannel*>(factory->Get("mysql", "testdb"));
    assert(db_ch != nullptr);

    IBatchWriter* writer = nullptr;
    int rc = db_ch->CreateWriter(TABLE_AUTO.c_str(), &writer);
    assert(rc == 0 && writer != nullptr);

    auto schema = arrow::schema({
        arrow::field("key",    arrow::int64()),
        arrow::field("value",  arrow::utf8()),
        arrow::field("amount", arrow::float64()),
    });

    arrow::Int64Builder  key_b;
    arrow::StringBuilder val_b;
    arrow::DoubleBuilder  amt_b;
    (void)key_b.Append(100); (void)key_b.Append(200);
    (void)val_b.Append("hello"); (void)val_b.Append("world");
    (void)amt_b.Append(3.14); (void)amt_b.Append(2.71);

    auto batch = arrow::RecordBatch::Make(schema, 2, {
        key_b.Finish().ValueOrDie(),
        val_b.Finish().ValueOrDie(),
        amt_b.Finish().ValueOrDie(),
    });

    auto buf = SerializeBatch(batch);
    rc = writer->Write(buf->data(), static_cast<size_t>(buf->size()));
    assert(rc == 0);

    BatchWriteStats stats;
    writer->Close(&stats);
    assert(stats.rows_written == 2);
    writer->Release();
    printf("  Written: %lld rows\n", stats.rows_written);

    // 验证数据
    IBatchReader* reader = nullptr;
    rc = db_ch->CreateReader(("SELECT * FROM " + TABLE_AUTO).c_str(), &reader);
    assert(rc == 0 && reader != nullptr);
    const uint8_t* rbuf = nullptr; size_t rlen = 0;
    assert(reader->Next(&rbuf, &rlen) == 0);
    printf("  Verified: read back %zu bytes\n", rlen);
    reader->Close();
    reader->Release();

    printf("[PASS] CreateWriter\n");
}

// ============================================================
// Test 5: 错误路径
// ============================================================
void test_error_paths() {
    printf("[TEST] Error paths...\n");

    PluginLoader* loader = PluginLoader::Single();
    auto* factory = static_cast<IDatabaseFactory*>(loader->First(IID_DATABASE_FACTORY));
    auto* db_ch = dynamic_cast<IDatabaseChannel*>(factory->Get("mysql", "testdb"));
    assert(db_ch != nullptr);

    // SQL 语法错误
    IBatchReader* reader = nullptr;
    int rc = db_ch->CreateReader("INVALID SQL SYNTAX", &reader);
    assert(rc != 0);
    printf("  SQL syntax error handled correctly\n");

    // 不支持的数据库类型
    auto* ch2 = factory->Get("unknown_db", "test");
    assert(ch2 == nullptr);
    printf("  Unsupported type: %s\n", factory->LastError());

    printf("[PASS] Error paths\n");
}

// ============================================================
// Test 6: SQL 解析器 WHERE 子句
// ============================================================
void test_sql_parser_where() {
    printf("[TEST] SQL parser WHERE clause...\n");
    SqlParser parser;

    {
        auto stmt = parser.Parse("SELECT * FROM mysql.mydb.users WHERE age>18");
        assert(stmt.error.empty());
        assert(stmt.source == "mysql.mydb.users");
        assert(stmt.where_clause == "age>18");
        assert(!stmt.HasOperator());
    }
    {
        auto stmt = parser.Parse("SELECT * FROM mysql.mydb.users WHERE age>18 INTO result");
        assert(stmt.error.empty());
        assert(stmt.where_clause == "age>18");
        assert(stmt.dest == "result");
    }
    {
        auto stmt = parser.Parse(
            "SELECT col1, col2 FROM mysql.mydb.users WHERE age>18 USING explore.chisquare");
        assert(stmt.error.empty());
        assert(stmt.where_clause == "age>18");
        assert(stmt.columns.size() == 2);
        assert(stmt.HasOperator());
        assert(stmt.op_name == "chisquare");
    }
    {
        auto stmt = parser.Parse(
            "SELECT * FROM mysql.db1.orders WHERE status='pending' AND amount>1000 INTO result");
        assert(stmt.error.empty());
        assert(stmt.where_clause == "status='pending' AND amount>1000");
        assert(stmt.dest == "result");
    }
    {
        auto stmt = parser.Parse(
            "SELECT * FROM mysql.mydb.users WHERE age>18; DROP TABLE users");
        assert(!stmt.error.empty());
        printf("  Injection rejected: %s\n", stmt.error.c_str());
    }
    {
        auto stmt = parser.Parse("SELECT * FROM test.data INTO result");
        assert(stmt.error.empty());
        assert(stmt.where_clause.empty());
    }

    printf("[PASS] SQL parser WHERE clause\n");
}

// ============================================================
// Test 7: DataFrame Filter
// ============================================================
void test_dataframe_filter() {
    printf("[TEST] DataFrame Filter...\n");

    DataFrame df;
    df.SetSchema({
        {"name",  DataType::STRING, 0, ""},
        {"age",   DataType::INT32,  0, ""},
        {"score", DataType::DOUBLE, 0, ""},
    });
    df.AppendRow({std::string("Alice"),   int32_t(25), double(95.5)});
    df.AppendRow({std::string("Bob"),     int32_t(17), double(87.3)});
    df.AppendRow({std::string("Charlie"), int32_t(30), double(92.1)});
    df.AppendRow({std::string("Diana"),   int32_t(15), double(88.0)});
    assert(df.RowCount() == 4);

    {
        DataFrame df2; df2.FromArrow(df.ToArrow());
        assert(df2.Filter("age>18") == 0);
        assert(df2.RowCount() == 2);
        printf("  age>18: %d rows\n", df2.RowCount());
    }
    {
        DataFrame df2; df2.FromArrow(df.ToArrow());
        assert(df2.Filter("name=Bob") == 0);
        assert(df2.RowCount() == 1);
        printf("  name=Bob: %d rows\n", df2.RowCount());
    }
    {
        DataFrame df2; df2.FromArrow(df.ToArrow());
        assert(df2.Filter("score>=90") == 0);
        assert(df2.RowCount() == 2);
        printf("  score>=90: %d rows\n", df2.RowCount());
    }
    {
        DataFrame df2; df2.FromArrow(df.ToArrow());
        assert(df2.Filter("nonexistent=1") == -1);
        printf("  nonexistent column: error handled\n");
    }

    printf("[PASS] DataFrame Filter\n");
}

// ============================================================
// Test 8: 安全基线（SQL 注入防护）
// ============================================================
void test_security() {
    printf("[TEST] Security baseline...\n");

    assert(SqlParser::ValidateWhereClause("age>18") == true);
    assert(SqlParser::ValidateWhereClause("name='Alice'") == true);
    assert(SqlParser::ValidateWhereClause("age>18 AND score>90") == true);
    assert(SqlParser::ValidateWhereClause("age>18; DROP TABLE users") == false);
    assert(SqlParser::ValidateWhereClause("age>18 -- comment") == true);
    assert(SqlParser::ValidateWhereClause("age>18 /* comment */") == true);
    assert(SqlParser::ValidateWhereClause("age>18 -- DROP TABLE users") == true);
    assert(SqlParser::ValidateWhereClause("age>18 /* DROP TABLE users */") == true);
    assert(SqlParser::ValidateWhereClause("1=1; DELETE FROM users") == false);
    assert(SqlParser::ValidateWhereClause("name='test' INSERT INTO x") == false);
    assert(SqlParser::ValidateWhereClause("TRUNCATE TABLE users") == false);
    assert(SqlParser::ValidateWhereClause("backdrop='red'") == true);
    printf("  SQL injection protection: all checks passed\n");

    printf("[PASS] Security baseline\n");
}

// ============================================================
// Test 9: 端到端 — Database → DataFrame（通过 Reader）
// ============================================================
void test_e2e_db_to_df() {
    printf("[TEST] E2E: Database → DataFrame...\n");

    PluginLoader* loader = PluginLoader::Single();
    auto* factory = static_cast<IDatabaseFactory*>(loader->First(IID_DATABASE_FACTORY));
    auto* db_ch = dynamic_cast<IDatabaseChannel*>(factory->Get("mysql", "testdb"));
    assert(db_ch != nullptr);

    IBatchReader* reader = nullptr;
    int rc = db_ch->CreateReader(("SELECT * FROM " + TABLE_USERS).c_str(), &reader);
    assert(rc == 0 && reader != nullptr);

    const uint8_t* buf = nullptr;
    size_t len = 0;
    assert(reader->Next(&buf, &len) == 0);

    auto arrow_buf = arrow::Buffer::Wrap(buf, static_cast<int64_t>(len));
    auto input = std::make_shared<arrow::io::BufferReader>(arrow_buf);
    auto stream_result = arrow::ipc::RecordBatchStreamReader::Open(input);
    assert(stream_result.ok());
    std::shared_ptr<arrow::RecordBatch> batch;
    assert((*stream_result)->ReadNext(&batch).ok() && batch);

    DataFrame result;
    result.FromArrow(batch);
    assert(result.RowCount() == 3);
    printf("  Read %d rows from %s\n", result.RowCount(), TABLE_USERS.c_str());

    reader->Close();
    reader->Release();
    printf("[PASS] E2E: Database → DataFrame\n");
}

// ============================================================
// Test 10: 端到端 — Database + WHERE → DataFrame
// ============================================================
void test_e2e_db_where_to_df() {
    printf("[TEST] E2E: Database + WHERE → DataFrame...\n");

    PluginLoader* loader = PluginLoader::Single();
    auto* factory = static_cast<IDatabaseFactory*>(loader->First(IID_DATABASE_FACTORY));
    auto* db_ch = dynamic_cast<IDatabaseChannel*>(factory->Get("mysql", "testdb"));

    IBatchReader* reader = nullptr;
    int rc = db_ch->CreateReader(
        ("SELECT * FROM " + TABLE_USERS + " WHERE score>90").c_str(), &reader);
    assert(rc == 0 && reader != nullptr);

    const uint8_t* buf = nullptr;
    size_t len = 0;
    assert(reader->Next(&buf, &len) == 0);

    auto arrow_buf = arrow::Buffer::Wrap(buf, static_cast<int64_t>(len));
    auto input = std::make_shared<arrow::io::BufferReader>(arrow_buf);
    auto stream_result = arrow::ipc::RecordBatchStreamReader::Open(input);
    assert(stream_result.ok());
    std::shared_ptr<arrow::RecordBatch> batch;
    assert((*stream_result)->ReadNext(&batch).ok() && batch);

    DataFrame result;
    result.FromArrow(batch);
    // WHERE score>90 应匹配 Alice(95.5) 和 Charlie(92.1)
    assert(result.RowCount() == 2);
    printf("  Read %d rows with WHERE score>90\n", result.RowCount());

    reader->Close();
    reader->Release();
    printf("[PASS] E2E: Database + WHERE → DataFrame\n");
}

// ============================================================
// Test 11: 端到端 — DataFrame → Database（通过 Writer）
// ============================================================
void test_e2e_df_to_db() {
    printf("[TEST] E2E: DataFrame → Database...\n");

    PluginLoader* loader = PluginLoader::Single();
    auto* factory = static_cast<IDatabaseFactory*>(loader->First(IID_DATABASE_FACTORY));
    auto* db_ch = dynamic_cast<IDatabaseChannel*>(factory->Get("mysql", "testdb"));

    DataFrame df;
    df.SetSchema({{"city", DataType::STRING, 0, ""}, {"population", DataType::INT64, 0, ""}});
    df.AppendRow({std::string("Beijing"),  int64_t(21540000)});
    df.AppendRow({std::string("Shanghai"), int64_t(24870000)});
    df.AppendRow({std::string("Shenzhen"), int64_t(17560000)});

    auto batch = df.ToArrow();
    auto buf = SerializeBatch(batch);

    IBatchWriter* writer = nullptr;
    int rc = db_ch->CreateWriter(TABLE_CITIES.c_str(), &writer);
    assert(rc == 0);
    rc = writer->Write(buf->data(), static_cast<size_t>(buf->size()));
    assert(rc == 0);
    BatchWriteStats stats;
    writer->Close(&stats);
    assert(stats.rows_written == 3);
    writer->Release();

    printf("  Wrote %lld rows to %s\n", stats.rows_written, TABLE_CITIES.c_str());
    printf("[PASS] E2E: DataFrame → Database\n");
}

// ============================================================
// Test 12: 端到端 — Database → Database（跨表复制）
// ============================================================
void test_e2e_db_to_db() {
    printf("[TEST] E2E: Database → Database (cross-table)...\n");

    PluginLoader* loader = PluginLoader::Single();
    auto* factory = static_cast<IDatabaseFactory*>(loader->First(IID_DATABASE_FACTORY));
    auto* db_ch = dynamic_cast<IDatabaseChannel*>(factory->Get("mysql", "testdb"));

    IBatchReader* reader = nullptr;
    int rc = db_ch->CreateReader(("SELECT * FROM " + TABLE_USERS).c_str(), &reader);
    assert(rc == 0);
    const uint8_t* buf = nullptr;
    size_t len = 0;
    assert(reader->Next(&buf, &len) == 0);

    IBatchWriter* writer = nullptr;
    rc = db_ch->CreateWriter(TABLE_COPY.c_str(), &writer);
    assert(rc == 0);
    rc = writer->Write(buf, len);
    assert(rc == 0);
    BatchWriteStats stats;
    writer->Close(&stats);
    writer->Release();
    reader->Close();
    reader->Release();

    assert(stats.rows_written == 3);
    printf("  Copied %lld rows to %s\n", stats.rows_written, TABLE_COPY.c_str());
    printf("[PASS] E2E: Database → Database\n");
}

// ============================================================
// Test 13: 端到端 — DataFrame + Filter → Database
// ============================================================
void test_e2e_df_filter_to_db() {
    printf("[TEST] E2E: DataFrame + Filter → Database...\n");

    PluginLoader* loader = PluginLoader::Single();
    auto* factory = static_cast<IDatabaseFactory*>(loader->First(IID_DATABASE_FACTORY));
    auto* db_ch = dynamic_cast<IDatabaseChannel*>(factory->Get("mysql", "testdb"));

    DataFrame df;
    df.SetSchema({{"name", DataType::STRING, 0, ""}, {"age", DataType::INT32, 0, ""}});
    df.AppendRow({std::string("Alice"),   int32_t(25)});
    df.AppendRow({std::string("Bob"),     int32_t(17)});
    df.AppendRow({std::string("Charlie"), int32_t(30)});
    df.Filter("age>18");
    assert(df.RowCount() == 2);

    auto batch = df.ToArrow();
    auto buf = SerializeBatch(batch);

    IBatchWriter* writer = nullptr;
    int rc = db_ch->CreateWriter(TABLE_ADULTS.c_str(), &writer);
    assert(rc == 0);
    rc = writer->Write(buf->data(), static_cast<size_t>(buf->size()));
    assert(rc == 0);
    BatchWriteStats stats;
    writer->Close(&stats);
    writer->Release();

    assert(stats.rows_written == 2);
    printf("  Filtered and wrote %lld adults to %s\n", stats.rows_written, TABLE_ADULTS.c_str());
    printf("[PASS] E2E: DataFrame + Filter → Database\n");
}

// ============================================================
// Test 14: 端到端 — 错误路径 + 断线重连
// ============================================================
void test_e2e_error_paths() {
    printf("[TEST] E2E: Error paths...\n");

    PluginLoader* loader = PluginLoader::Single();
    auto* factory = static_cast<IDatabaseFactory*>(loader->First(IID_DATABASE_FACTORY));

    auto* db_ch = dynamic_cast<IDatabaseChannel*>(factory->Get("mysql", "testdb"));
    IBatchReader* reader = nullptr;
    int rc = db_ch->CreateReader("SELECT * FROM nonexistent_table_xyz", &reader);
    assert(rc != 0);
    printf("  Nonexistent table: error handled\n");

    factory->Release("mysql", "testdb");
    auto* ch2 = factory->Get("mysql", "testdb");
    assert(ch2 != nullptr && ch2->IsConnected());
    printf("  Reconnect after release: success\n");

    printf("[PASS] E2E: Error paths\n");
}

// ============================================================
// main
// ============================================================
int main(int argc, char* argv[]) {
    printf("=== FlowSQL Database Plugin E2E Tests (MySQL) ===\n\n");

    // 生成唯一后缀，避免重跑时表名冲突
    auto ts = std::chrono::system_clock::now().time_since_epoch().count();
    g_suffix    = std::to_string(ts % 1000000);
    TABLE_USERS  = "e2e_users_"  + g_suffix;
    TABLE_AUTO   = "e2e_auto_"   + g_suffix;
    TABLE_CITIES = "e2e_cities_" + g_suffix;
    TABLE_COPY   = "e2e_copy_"   + g_suffix;
    TABLE_ADULTS = "e2e_adults_" + g_suffix;
    printf("  Test suffix: %s\n\n", g_suffix.c_str());

    std::string plugin_dir = get_absolute_process_path();

    // 加载插件
    PluginLoader* loader = PluginLoader::Single();
    std::string plugin_name = "libflowsql_database.so";
    const char* relapath[] = {plugin_name.c_str()};
    std::string opt = MakeMysqlOption("testdb");
    const char* options[] = {opt.c_str()};
    int ret = loader->Load(plugin_dir.c_str(), relapath, options, 1);
    if (ret != 0) {
        printf("[SKIP] Plugin not found: %s\n", plugin_name.c_str());
        return 0;
    }
    loader->StartAll();

    // 检查 MySQL 可用性（通过插件层）
    auto* factory = static_cast<IDatabaseFactory*>(loader->First(IID_DATABASE_FACTORY));
    if (!factory) {
        printf("[SKIP] No database factory\n");
        return 0;
    }
    auto* ch = factory->Get("mysql", "testdb");
    if (!ch || !ch->IsConnected()) {
        auto p = GetMysqlParams();
        printf("[SKIP] MySQL not available at %s:%s (user=%s database=%s)\n",
               p["host"].c_str(), p["port"].c_str(),
               p["user"].c_str(), p["database"].c_str());
        printf("       Set MYSQL_HOST/MYSQL_PORT/MYSQL_USER/MYSQL_PASSWORD/MYSQL_DATABASE\n");
        return 0;
    }

    test_option_parsing();
    test_mysql_connect();
    test_create_reader();
    test_create_writer();
    test_error_paths();
    test_sql_parser_where();
    test_dataframe_filter();
    test_security();

    // 端到端测试
    test_e2e_db_to_df();
    test_e2e_db_where_to_df();
    test_e2e_df_to_db();
    test_e2e_db_to_db();
    test_e2e_df_filter_to_db();
    test_e2e_error_paths();

    loader->StopAll();
    loader->Unload();

    printf("\n=== All database plugin E2E tests passed ===\n");
    return 0;
}
