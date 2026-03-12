#include <cassert>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
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
// Test 15: 多线程并发读 — 多线程同时对同一 IDatabaseChannel 调用 CreateReader
// ============================================================
void test_concurrent_readers() {
    printf("[TEST] Concurrent readers on same IDatabaseChannel...\n");

    PluginLoader* loader = PluginLoader::Single();
    auto* factory = static_cast<IDatabaseFactory*>(loader->First(IID_DATABASE_FACTORY));
    auto* db_ch = dynamic_cast<IDatabaseChannel*>(factory->Get("mysql", "testdb"));
    assert(db_ch != nullptr);

    // 准备数据表（使用全局后缀保证唯一）
    std::string table = "mt_readers_" + g_suffix;
    {
        auto schema = arrow::schema({arrow::field("id", arrow::int32()),
                                     arrow::field("name", arrow::utf8())});
        arrow::Int32Builder id_b;
        arrow::StringBuilder name_b;
        for (int i = 0; i < 20; ++i) {
            (void)id_b.Append(i);
            (void)name_b.Append("row_" + std::to_string(i));
        }
        std::shared_ptr<arrow::Array> id_arr, name_arr;
        (void)id_b.Finish(&id_arr);
        (void)name_b.Finish(&name_arr);
        auto batch = arrow::RecordBatch::Make(schema, 20, {id_arr, name_arr});
        WriteTestData(db_ch, table.c_str(), batch);
    }

    const int N_THREADS = 8;
    std::atomic<int> success{0};
    std::atomic<int> errors{0};
    std::mutex result_mutex;
    std::vector<int> row_counts;

    std::vector<std::thread> threads;
    for (int i = 0; i < N_THREADS; ++i) {
        threads.emplace_back([&, table]() {
            std::string query = "SELECT * FROM " + table;
            IBatchReader* reader = nullptr;
            // 多线程同时调用同一 db_ch 的 CreateReader
            int rc = db_ch->CreateReader(query.c_str(), &reader);
            if (rc != 0 || !reader) { errors++; return; }

            int rows = 0;
            const uint8_t* buf; size_t len;
            while (reader->Next(&buf, &len) == 0) {
                auto b = arrow::Buffer::Wrap(buf, static_cast<int64_t>(len));
                auto inp = std::make_shared<arrow::io::BufferReader>(b);
                auto s = arrow::ipc::RecordBatchStreamReader::Open(inp).ValueOrDie();
                std::shared_ptr<arrow::RecordBatch> batch;
                if (s->ReadNext(&batch).ok() && batch)
                    rows += static_cast<int>(batch->num_rows());
            }
            reader->Close();
            reader->Release();

            {
                std::lock_guard<std::mutex> lk(result_mutex);
                row_counts.push_back(rows);
            }
            success++;
        });
    }
    for (auto& t : threads) t.join();

    printf("  threads=%d success=%d errors=%d\n",
           N_THREADS, success.load(), errors.load());
    assert(errors == 0);
    assert(success == N_THREADS);
    for (int c : row_counts) {
        assert(c == 20);
    }
    printf("  all %d threads read 20 rows each, no crash\n", N_THREADS);

    printf("[PASS] Concurrent readers on same IDatabaseChannel\n");
}

// ============================================================
// Test 16: 多线程并发写 — 多线程同时对同一 IDatabaseChannel 调用 CreateWriter（不同表）
// ============================================================
void test_concurrent_writers() {
    printf("[TEST] Concurrent writers on same IDatabaseChannel...\n");

    PluginLoader* loader = PluginLoader::Single();
    auto* factory = static_cast<IDatabaseFactory*>(loader->First(IID_DATABASE_FACTORY));
    auto* db_ch = dynamic_cast<IDatabaseChannel*>(factory->Get("mysql", "testdb"));
    assert(db_ch != nullptr);

    const int N_THREADS = 6;
    const int ROWS_PER_THREAD = 30;
    std::atomic<int> success{0};
    std::atomic<int> errors{0};

    std::vector<std::string> tables;
    for (int i = 0; i < N_THREADS; ++i)
        tables.push_back("mt_writers_" + g_suffix + "_" + std::to_string(i));

    std::vector<std::thread> threads;
    for (int i = 0; i < N_THREADS; ++i) {
        threads.emplace_back([&, i]() {
            auto schema = arrow::schema({arrow::field("id", arrow::int32()),
                                         arrow::field("val", arrow::utf8())});
            arrow::Int32Builder id_b;
            arrow::StringBuilder val_b;
            for (int r = 0; r < ROWS_PER_THREAD; ++r) {
                (void)id_b.Append(r);
                (void)val_b.Append("t" + std::to_string(i) + "_r" + std::to_string(r));
            }
            std::shared_ptr<arrow::Array> id_arr, val_arr;
            (void)id_b.Finish(&id_arr);
            (void)val_b.Finish(&val_arr);
            auto batch = arrow::RecordBatch::Make(schema, ROWS_PER_THREAD, {id_arr, val_arr});
            auto buf = SerializeBatch(batch);

            IBatchWriter* writer = nullptr;
            // 多线程同时调用同一 db_ch 的 CreateWriter（各写不同表）
            int rc = db_ch->CreateWriter(tables[i].c_str(), &writer);
            if (rc != 0 || !writer) { errors++; return; }

            rc = writer->Write(buf->data(), static_cast<size_t>(buf->size()));
            BatchWriteStats stats;
            writer->Close(&stats);
            writer->Release();

            if (rc != 0) { errors++; return; }
            success++;
        });
    }
    for (auto& t : threads) t.join();

    printf("  threads=%d success=%d errors=%d\n",
           N_THREADS, success.load(), errors.load());
    assert(errors == 0);
    assert(success == N_THREADS);

    // 验证每张表行数正确
    for (int i = 0; i < N_THREADS; ++i) {
        std::string query = "SELECT COUNT(*) FROM " + tables[i];
        IBatchReader* reader = nullptr;
        assert(db_ch->CreateReader(query.c_str(), &reader) == 0);
        const uint8_t* buf; size_t len;
        int count = 0;
        if (reader->Next(&buf, &len) == 0) {
            auto b = arrow::Buffer::Wrap(buf, static_cast<int64_t>(len));
            auto inp = std::make_shared<arrow::io::BufferReader>(b);
            auto s = arrow::ipc::RecordBatchStreamReader::Open(inp).ValueOrDie();
            std::shared_ptr<arrow::RecordBatch> batch;
            if (s->ReadNext(&batch).ok() && batch && batch->num_rows() > 0)
                count = static_cast<int>(
                    std::static_pointer_cast<arrow::Int64Array>(batch->column(0))->Value(0));
        }
        reader->Close(); reader->Release();
        assert(count == ROWS_PER_THREAD);
        printf("  table %s: %d rows OK\n", tables[i].c_str(), count);
    }

    printf("[PASS] Concurrent writers on same IDatabaseChannel\n");
}

// ============================================================
// ClickHouse E2E 辅助
// ============================================================
static std::string g_ch_suffix;

static std::string MakeClickHouseOption(const std::string& name) {
    const char* host = getenv("CH_HOST")     ? getenv("CH_HOST")     : "127.0.0.1";
    const char* port = getenv("CH_PORT")     ? getenv("CH_PORT")     : "8123";
    const char* user = getenv("CH_USER")     ? getenv("CH_USER")     : "default";
    const char* pass = getenv("CH_PASSWORD") ? getenv("CH_PASSWORD") : "";
    const char* db   = getenv("CH_DATABASE") ? getenv("CH_DATABASE") : "default";
    return "type=clickhouse;name=" + name +
           ";host=" + host + ";port=" + port +
           ";user=" + user + ";password=" + pass + ";database=" + db;
}

static std::shared_ptr<arrow::RecordBatch> MakeChTypeMatrixBatch() {
    auto schema = arrow::schema({
        arrow::field("c_int32",   arrow::int32()),
        arrow::field("c_int64",   arrow::int64()),
        arrow::field("c_float32", arrow::float32()),
        arrow::field("c_float64", arrow::float64()),
        arrow::field("c_string",  arrow::utf8()),
        arrow::field("c_bool",    arrow::boolean()),
    });
    arrow::Int32Builder   b_i32;
    arrow::Int64Builder   b_i64;
    arrow::FloatBuilder   b_f32;
    arrow::DoubleBuilder  b_f64;
    arrow::StringBuilder  b_str;
    arrow::BooleanBuilder b_bool;
    (void)b_i32.AppendValues({1, 2, 3});
    (void)b_i64.AppendValues({100LL, 200LL, 300LL});
    (void)b_f32.AppendValues({1.1f, 2.2f, 3.3f});
    (void)b_f64.AppendValues({1.11, 2.22, 3.33});
    (void)b_str.AppendValues({"hello", "world", "clickhouse"});
    (void)b_bool.AppendValues(std::vector<bool>{true, false, true});
    std::shared_ptr<arrow::Array> a_i32, a_i64, a_f32, a_f64, a_str, a_bool;
    (void)b_i32.Finish(&a_i32); (void)b_i64.Finish(&a_i64);
    (void)b_f32.Finish(&a_f32); (void)b_f64.Finish(&a_f64);
    (void)b_str.Finish(&a_str); (void)b_bool.Finish(&a_bool);
    return arrow::RecordBatch::Make(schema, 3, {a_i32, a_i64, a_f32, a_f64, a_str, a_bool});
}

// ============================================================
// E1: ClickHouse 通道连接
// ============================================================
void test_clickhouse_connect(IDatabaseFactory* factory) {
    printf("[TEST] E1: ClickHouse connect via plugin...\n");

    auto* ch = factory->Get("clickhouse", "ch1");
    assert(ch != nullptr);
    assert(ch->IsConnected());
    assert(std::string(ch->Catelog()) == "clickhouse");
    assert(std::string(ch->Name()) == "ch1");
    printf("  factory->Get(clickhouse, ch1): connected OK\n");

    printf("[PASS] E1: ClickHouse connect\n");
}

// ============================================================
// E2: CreateArrowReader — 通过插件层读取 RecordBatch
// ============================================================
void test_clickhouse_create_arrow_reader(IDatabaseFactory* factory) {
    printf("[TEST] E2: ClickHouse CreateArrowReader...\n");

    auto* ch = dynamic_cast<IDatabaseChannel*>(factory->Get("clickhouse", "ch1"));
    assert(ch != nullptr);

    // 建表并写入数据
    std::string table = "e2e_ch_reader_" + g_ch_suffix;
    std::string error;
    {
        std::vector<std::shared_ptr<arrow::RecordBatch>> dummy;
        ch->ExecuteQueryArrow(("DROP TABLE IF EXISTS " + table).c_str(), &dummy, &error);
        ch->ExecuteQueryArrow(
            ("CREATE TABLE " + table + " (id Int32, val String)"
             " ENGINE = MergeTree() ORDER BY id").c_str(), &dummy, &error);
    }
    auto schema = arrow::schema({arrow::field("id", arrow::int32()),
                                  arrow::field("val", arrow::utf8())});
    arrow::Int32Builder b_id; arrow::StringBuilder b_val;
    (void)b_id.AppendValues({1, 2, 3});
    (void)b_val.AppendValues({"a", "b", "c"});
    std::shared_ptr<arrow::Array> a_id, a_val;
    (void)b_id.Finish(&a_id); (void)b_val.Finish(&a_val);
    auto batch = arrow::RecordBatch::Make(schema, 3, {a_id, a_val});
    ch->WriteArrowBatches(table.c_str(), {batch}, &error);

    // 通过 CreateArrowReader 读取
    IArrowReader* reader = nullptr;
    int rc = ch->CreateArrowReader(("SELECT * FROM " + table).c_str(), &reader);
    assert(rc == 0 && reader != nullptr);

    std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
    rc = reader->ExecuteQueryArrow(nullptr, &batches, &error);
    assert(rc == 0);
    assert(!batches.empty());
    int64_t total = 0;
    for (const auto& b : batches) total += b->num_rows();
    assert(total == 3);
    assert(batches[0]->num_columns() == 2);
    printf("  CreateArrowReader: %lld rows, %d columns\n",
           (long long)total, batches[0]->num_columns());
    reader->Release();

    // 清理
    std::vector<std::shared_ptr<arrow::RecordBatch>> dummy;
    ch->ExecuteQueryArrow(("DROP TABLE IF EXISTS " + table).c_str(), &dummy, &error);
    printf("[PASS] E2: ClickHouse CreateArrowReader\n");
}

// ============================================================
// E3: CreateArrowWriter — 通过插件层写入 RecordBatch
// ============================================================
void test_clickhouse_create_arrow_writer(IDatabaseFactory* factory) {
    printf("[TEST] E3: ClickHouse CreateArrowWriter...\n");

    auto* ch = dynamic_cast<IDatabaseChannel*>(factory->Get("clickhouse", "ch1"));
    assert(ch != nullptr);

    std::string table = "e2e_ch_writer_" + g_ch_suffix;
    std::string error;
    {
        std::vector<std::shared_ptr<arrow::RecordBatch>> dummy;
        ch->ExecuteQueryArrow(("DROP TABLE IF EXISTS " + table).c_str(), &dummy, &error);
        ch->ExecuteQueryArrow(
            ("CREATE TABLE " + table + " (id Int32)"
             " ENGINE = MergeTree() ORDER BY id").c_str(), &dummy, &error);
    }

    // 通过 CreateArrowWriter 写入
    IArrowWriter* writer = nullptr;
    int rc = ch->CreateArrowWriter(table.c_str(), &writer);
    assert(rc == 0 && writer != nullptr);

    auto schema = arrow::schema({arrow::field("id", arrow::int32())});
    arrow::Int32Builder b;
    (void)b.AppendValues({10, 20, 30, 40, 50});
    std::shared_ptr<arrow::Array> a;
    (void)b.Finish(&a);
    auto batch = arrow::RecordBatch::Make(schema, 5, {a});

    rc = writer->WriteBatches(table.c_str(), {batch}, &error);
    assert(rc == 0);
    writer->Release();

    // 回读验证行数
    std::vector<std::shared_ptr<arrow::RecordBatch>> read_batches;
    rc = ch->ExecuteQueryArrow(("SELECT * FROM " + table).c_str(), &read_batches, &error);
    assert(rc == 0);
    int64_t total = 0;
    for (const auto& b2 : read_batches) total += b2->num_rows();
    assert(total == 5);
    printf("  CreateArrowWriter: wrote and read back %lld rows\n", (long long)total);

    std::vector<std::shared_ptr<arrow::RecordBatch>> dummy;
    ch->ExecuteQueryArrow(("DROP TABLE IF EXISTS " + table).c_str(), &dummy, &error);
    printf("[PASS] E3: ClickHouse CreateArrowWriter\n");
}

// ============================================================
// E4: Arrow 类型矩阵 — 通过插件层写入/读取全类型
// ============================================================
void test_clickhouse_arrow_type_matrix(IDatabaseFactory* factory) {
    printf("[TEST] E4: ClickHouse Arrow type matrix via plugin...\n");

    auto* ch = dynamic_cast<IDatabaseChannel*>(factory->Get("clickhouse", "ch1"));
    assert(ch != nullptr);

    std::string table = "e2e_ch_types_" + g_ch_suffix;
    std::string error;
    {
        std::vector<std::shared_ptr<arrow::RecordBatch>> dummy;
        ch->ExecuteQueryArrow(("DROP TABLE IF EXISTS " + table).c_str(), &dummy, &error);
        ch->ExecuteQueryArrow(
            ("CREATE TABLE " + table +
             " (c_int32 Int32, c_int64 Int64, c_float32 Float32,"
             "  c_float64 Float64, c_string String, c_bool Bool)"
             " ENGINE = MergeTree() ORDER BY c_int32").c_str(), &dummy, &error);
    }

    auto batch = MakeChTypeMatrixBatch();
    int rc = ch->WriteArrowBatches(table.c_str(), {batch}, &error);
    assert(rc == 0);

    std::vector<std::shared_ptr<arrow::RecordBatch>> read_batches;
    rc = ch->ExecuteQueryArrow(
        ("SELECT * FROM " + table + " ORDER BY c_int32").c_str(), &read_batches, &error);
    assert(rc == 0);
    assert(!read_batches.empty());
    int64_t total = 0;
    for (const auto& b : read_batches) total += b->num_rows();
    assert(total == 3);
    assert(read_batches[0]->num_columns() == 6);

    // 验证 c_int32 和 c_string 两列（代表整数和字符串类型）
    auto col_i32 = std::static_pointer_cast<arrow::Int32Array>(read_batches[0]->column(0));
    assert(col_i32->Value(0) == 1 && col_i32->Value(1) == 2 && col_i32->Value(2) == 3);
    auto col_str = std::static_pointer_cast<arrow::StringArray>(read_batches[0]->column(4));
    assert(col_str->GetString(0) == "hello");
    printf("  type matrix: %lld rows, 6 columns, values verified\n", (long long)total);

    std::vector<std::shared_ptr<arrow::RecordBatch>> dummy;
    ch->ExecuteQueryArrow(("DROP TABLE IF EXISTS " + table).c_str(), &dummy, &error);
    printf("[PASS] E4: ClickHouse Arrow type matrix\n");
}

// ============================================================
// E5: ClickHouse 通道调用行式接口 CreateReader 应返回 -1
// ============================================================
void test_clickhouse_create_reader_unsupported(IDatabaseFactory* factory) {
    printf("[TEST] E5: ClickHouse CreateReader (row interface) should return -1...\n");

    auto* ch = dynamic_cast<IDatabaseChannel*>(factory->Get("clickhouse", "ch1"));
    assert(ch != nullptr);

    IBatchReader* reader = nullptr;
    int rc = ch->CreateReader("SELECT 1", &reader);
    assert(rc != 0);
    assert(reader == nullptr);
    printf("  CreateReader on ClickHouse channel returned %d (expected -1)\n", rc);

    printf("[PASS] E5: ClickHouse CreateReader unsupported\n");
}

// ============================================================
// E6: 8 线程并发 CreateArrowReader
// ============================================================
void test_concurrent_arrow_readers(IDatabaseFactory* factory) {
    printf("[TEST] E6: Concurrent CreateArrowReader (8 threads)...\n");

    auto* ch = dynamic_cast<IDatabaseChannel*>(factory->Get("clickhouse", "ch1"));
    assert(ch != nullptr);

    // 准备共享表
    std::string table = "e2e_ch_conc_r_" + g_ch_suffix;
    std::string error;
    {
        std::vector<std::shared_ptr<arrow::RecordBatch>> dummy;
        ch->ExecuteQueryArrow(("DROP TABLE IF EXISTS " + table).c_str(), &dummy, &error);
        ch->ExecuteQueryArrow(
            ("CREATE TABLE " + table + " (id Int32)"
             " ENGINE = MergeTree() ORDER BY id").c_str(), &dummy, &error);
        auto schema = arrow::schema({arrow::field("id", arrow::int32())});
        arrow::Int32Builder b;
        for (int i = 0; i < 10; ++i) (void)b.Append(i);
        std::shared_ptr<arrow::Array> a; (void)b.Finish(&a);
        auto batch = arrow::RecordBatch::Make(schema, 10, {a});
        ch->WriteArrowBatches(table.c_str(), {batch}, &error);
    }

    const int N = 8;
    std::atomic<int> success{0}, fail{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < N; ++i) {
        threads.emplace_back([&, ch, table]() {
            IArrowReader* reader = nullptr;
            std::string err;
            int rc = ch->CreateArrowReader(("SELECT * FROM " + table).c_str(), &reader);
            if (rc != 0 || !reader) { fail++; return; }
            std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
            rc = reader->ExecuteQueryArrow(nullptr, &batches, &err);
            reader->Release();
            if (rc != 0) { fail++; return; }
            int64_t total = 0;
            for (const auto& b : batches) total += b->num_rows();
            if (total == 10) success++;
            else fail++;
        });
    }
    for (auto& t : threads) t.join();

    printf("  %d/%d threads succeeded\n", success.load(), N);
    assert(fail == 0);
    assert(success == N);

    std::vector<std::shared_ptr<arrow::RecordBatch>> dummy;
    ch->ExecuteQueryArrow(("DROP TABLE IF EXISTS " + table).c_str(), &dummy, &error);
    printf("[PASS] E6: Concurrent CreateArrowReader\n");
}

// ============================================================
// E7: 6 线程并发 CreateArrowWriter（各写不同表）
// ============================================================
void test_concurrent_arrow_writers(IDatabaseFactory* factory) {
    printf("[TEST] E7: Concurrent CreateArrowWriter (6 threads)...\n");

    auto* ch = dynamic_cast<IDatabaseChannel*>(factory->Get("clickhouse", "ch1"));
    assert(ch != nullptr);

    const int N = 6;
    const int ROWS = 50;
    std::vector<std::string> tables;
    std::string error;

    // 预建表
    for (int i = 0; i < N; ++i) {
        std::string t = "e2e_ch_conc_w_" + g_ch_suffix + "_" + std::to_string(i);
        tables.push_back(t);
        std::vector<std::shared_ptr<arrow::RecordBatch>> dummy;
        ch->ExecuteQueryArrow(("DROP TABLE IF EXISTS " + t).c_str(), &dummy, &error);
        ch->ExecuteQueryArrow(
            ("CREATE TABLE " + t + " (id Int32)"
             " ENGINE = MergeTree() ORDER BY id").c_str(), &dummy, &error);
    }

    std::atomic<int> success{0}, fail{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < N; ++i) {
        threads.emplace_back([&, ch, i]() {
            IArrowWriter* writer = nullptr;
            std::string err;
            int rc = ch->CreateArrowWriter(tables[i].c_str(), &writer);
            if (rc != 0 || !writer) { fail++; return; }

            auto schema = arrow::schema({arrow::field("id", arrow::int32())});
            arrow::Int32Builder b;
            for (int r = 0; r < ROWS; ++r) (void)b.Append(r);
            std::shared_ptr<arrow::Array> a; (void)b.Finish(&a);
            auto batch = arrow::RecordBatch::Make(schema, ROWS, {a});

            rc = writer->WriteBatches(tables[i].c_str(), {batch}, &err);
            writer->Release();
            if (rc == 0) success++;
            else fail++;
        });
    }
    for (auto& t : threads) t.join();

    printf("  %d/%d threads wrote successfully\n", success.load(), N);
    assert(fail == 0);
    assert(success == N);

    // 验证每张表行数
    for (int i = 0; i < N; ++i) {
        std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
        ch->ExecuteQueryArrow(("SELECT * FROM " + tables[i]).c_str(), &batches, &error);
        int64_t total = 0;
        for (const auto& b : batches) total += b->num_rows();
        assert(total == ROWS);
        std::vector<std::shared_ptr<arrow::RecordBatch>> dummy;
        ch->ExecuteQueryArrow(("DROP TABLE IF EXISTS " + tables[i]).c_str(), &dummy, &error);
    }
    printf("  all %d tables verified (%d rows each)\n", N, ROWS);
    printf("[PASS] E7: Concurrent CreateArrowWriter\n");
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
    test_concurrent_readers();
    test_concurrent_writers();

    // 清理 MySQL 测试产生的所有临时表
    {
        auto* factory = static_cast<IDatabaseFactory*>(loader->First(IID_DATABASE_FACTORY));
        auto* db_ch = dynamic_cast<IDatabaseChannel*>(factory->Get("mysql", "testdb"));
        if (db_ch) {
            std::string err;
            // 全局表
            for (const auto& t : {TABLE_USERS, TABLE_AUTO, TABLE_CITIES, TABLE_COPY, TABLE_ADULTS}) {
                db_ch->ExecuteSql(("DROP TABLE IF EXISTS " + t).c_str(), &err);
            }
            // 并发读测试表
            db_ch->ExecuteSql(("DROP TABLE IF EXISTS mt_readers_" + g_suffix).c_str(), &err);
            // 并发写测试表（6 张）
            for (int i = 0; i < 6; ++i) {
                db_ch->ExecuteSql(
                    ("DROP TABLE IF EXISTS mt_writers_" + g_suffix + "_" + std::to_string(i)).c_str(), &err);
            }
            printf("  [CLEANUP] MySQL temp tables dropped\n");
        }
    }

    loader->StopAll();
    loader->Unload();

    printf("\n=== All database plugin E2E tests passed ===\n");

    // ============================================================
    // ClickHouse E2E（E1-E7）— 复用同一 loader，重新加载插件
    // ============================================================
    printf("\n=== FlowSQL Database Plugin E2E Tests (ClickHouse) ===\n\n");

    auto ts2 = std::chrono::system_clock::now().time_since_epoch().count();
    g_ch_suffix = std::to_string(ts2 % 1000000);
    printf("  Test suffix: %s\n\n", g_ch_suffix.c_str());

    std::string ch_opt = MakeClickHouseOption("ch1");
    const char* ch_relapath[] = {plugin_name.c_str()};
    const char* ch_options[]  = {ch_opt.c_str()};
    int ch_ret = loader->Load(plugin_dir.c_str(), ch_relapath, ch_options, 1);
    if (ch_ret != 0) {
        printf("[SKIP] Plugin not found for ClickHouse tests\n");
        return 0;
    }
    loader->StartAll();

    auto* ch_factory = static_cast<IDatabaseFactory*>(loader->First(IID_DATABASE_FACTORY));
    if (!ch_factory) {
        printf("[SKIP] No database factory for ClickHouse\n");
        loader->StopAll(); loader->Unload();
        return 0;
    }
    auto* ch_ch = ch_factory->Get("clickhouse", "ch1");
    if (!ch_ch || !ch_ch->IsConnected()) {
        printf("[SKIP] ClickHouse not available. Set CH_HOST/CH_PORT/CH_USER/CH_PASSWORD/CH_DATABASE\n");
        loader->StopAll(); loader->Unload();
        return 0;
    }

    test_clickhouse_connect(ch_factory);
    test_clickhouse_create_arrow_reader(ch_factory);
    test_clickhouse_create_arrow_writer(ch_factory);
    test_clickhouse_arrow_type_matrix(ch_factory);
    test_clickhouse_create_reader_unsupported(ch_factory);
    test_concurrent_arrow_readers(ch_factory);
    test_concurrent_arrow_writers(ch_factory);

    loader->StopAll();
    loader->Unload();

    printf("\n=== All ClickHouse E2E tests passed (E1-E7) ===\n");
    return 0;
}
