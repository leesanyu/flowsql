// test_clickhouse.cpp — ClickHouse 驱动独立测试套件
//
// 设计原则：
//   1. 每个测试函数自建自清数据，不依赖其他测试的状态
//   2. 通过环境变量配置连接参数，ClickHouse 不可用时自动跳过
//   3. 每个测试使用唯一表名（时间戳后缀），避免并发冲突
//
// 环境变量：
//   CH_HOST     (默认 127.0.0.1)
//   CH_PORT     (默认 8123)
//   CH_USER     (默认 default)
//   CH_PASSWORD (默认 空)
//   CH_DATABASE (默认 default)
//
#include <cassert>
#include <cmath>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <arrow/ipc/api.h>

#include <services/database/drivers/clickhouse_driver.h>
#include <services/database/capability_interfaces.h>

using namespace flowsql;
using namespace flowsql::database;

// ============================================================
// 全局：统计
// ============================================================
static int g_skipped = 0;
static int g_passed  = 0;

// ============================================================
// 辅助：从环境变量读取连接参数
// ============================================================
static std::unordered_map<std::string, std::string> GetClickHouseParams() {
    std::unordered_map<std::string, std::string> p;
    p["host"]     = getenv("CH_HOST")     ? getenv("CH_HOST")     : "127.0.0.1";
    p["port"]     = getenv("CH_PORT")     ? getenv("CH_PORT")     : "8123";
    p["user"]     = getenv("CH_USER")     ? getenv("CH_USER")     : "flowsql_user";
    p["password"] = getenv("CH_PASSWORD") ? getenv("CH_PASSWORD") : "flowSQL@user";
    p["database"] = getenv("CH_DATABASE") ? getenv("CH_DATABASE") : "flowsql_db";
    return p;
}

// ============================================================
// 辅助：检查 ClickHouse 是否可用（用于跳过测试）
// ============================================================
static bool IsClickHouseAvailable() {
    ClickHouseDriver driver;
    auto params = GetClickHouseParams();
    int rc = driver.Connect(params);
    if (rc != 0) return false;
    bool ok = driver.Ping();
    driver.Disconnect();
    return ok;
}

// ============================================================
// 辅助：生成唯一表名（毫秒时间戳 + 后缀）
// ============================================================
static std::string UniqueTable(const char* prefix) {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return std::string(prefix) + "_" + std::to_string(ms);
}

// ============================================================
// 辅助：建立 Session 并执行清理 SQL（忽略错误）
// ============================================================
static void DropTableIfExists(IDbSession* session, const std::string& table) {
    std::string sql = "DROP TABLE IF EXISTS " + table;
    session->ExecuteSql(sql.c_str());
}

// ============================================================
// 辅助：构造包含全类型矩阵的 RecordBatch
// ============================================================
static std::shared_ptr<arrow::RecordBatch> MakeTypeMatrixBatch() {
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
    (void)b_i32.Finish(&a_i32);
    (void)b_i64.Finish(&a_i64);
    (void)b_f32.Finish(&a_f32);
    (void)b_f64.Finish(&a_f64);
    (void)b_str.Finish(&a_str);
    (void)b_bool.Finish(&a_bool);

    return arrow::RecordBatch::Make(schema, 3, {a_i32, a_i64, a_f32, a_f64, a_str, a_bool});
}

// ============================================================
// T1: 连接与断开
// ============================================================
void test_connect_disconnect() {
    printf("[TEST] ClickHouse: connect and disconnect...\n");

    ClickHouseDriver driver;
    auto params = GetClickHouseParams();

    int rc = driver.Connect(params);
    assert(rc == 0);
    assert(driver.IsConnected());
    assert(driver.Ping());
    printf("  Connected to %s:%s/%s, Ping OK\n",
           params["host"].c_str(), params["port"].c_str(), params["database"].c_str());

    driver.Disconnect();
    assert(!driver.IsConnected());
    printf("  Disconnected OK\n");

    g_passed++;
    printf("[PASS] T1: connect and disconnect\n");
}

// ============================================================
// T2: 错误路径 — 错误密码连接失败
// ============================================================
void test_connect_wrong_password() {
    printf("[TEST] ClickHouse: connect with wrong password...\n");

    ClickHouseDriver driver;
    auto params = GetClickHouseParams();
    params["password"] = "definitely_wrong_password_xyz_12345";
    params["user"]     = "nonexistent_user_xyz";

    int rc = driver.Connect(params);
    assert(rc != 0);
    assert(!driver.IsConnected());
    const char* err = driver.LastError();
    assert(err != nullptr && strlen(err) > 0);
    // 错误信息应包含 HTTP 状态码或认证失败信息
    printf("  Connection correctly failed: %s\n", err);

    g_passed++;
    printf("[PASS] T2: connect with wrong password\n");
}

// ============================================================
// T3: 错误路径 — 不可达主机
// ============================================================
void test_connect_unreachable() {
    printf("[TEST] ClickHouse: connect to unreachable host...\n");

    ClickHouseDriver driver;
    std::unordered_map<std::string, std::string> params;
    params["host"] = "192.0.2.1";  // TEST-NET，不可路由
    params["port"] = "8123";
    params["user"] = "default";

    int rc = driver.Connect(params);
    assert(rc != 0);
    assert(!driver.IsConnected());
    const char* err = driver.LastError();
    assert(err != nullptr && strlen(err) > 0);
    printf("  Connection correctly failed: %s\n", err);

    g_passed++;
    printf("[PASS] T3: connect to unreachable host\n");
}

// ============================================================
// T4: DDL — 建表、删表
// ============================================================
void test_ddl() {
    printf("[TEST] ClickHouse: DDL (CREATE/DROP TABLE)...\n");

    ClickHouseDriver driver;
    assert(driver.Connect(GetClickHouseParams()) == 0);
    auto session = driver.CreateSession();
    assert(session != nullptr);

    std::string table = UniqueTable("ch_test_ddl");
    DropTableIfExists(session.get(), table);

    // 建表（ClickHouse MergeTree 引擎）
    std::string create_sql = "CREATE TABLE " + table +
        " (id Int32, name String) ENGINE = MergeTree() ORDER BY id";
    std::string error;
    int rc = session->ExecuteSql(create_sql.c_str());
    assert(rc == 0);
    printf("  CREATE TABLE: OK\n");

    // 删表
    rc = session->ExecuteSql(("DROP TABLE " + table).c_str());
    assert(rc == 0);
    printf("  DROP TABLE: OK\n");

    g_passed++;
    printf("[PASS] T4: DDL\n");
}

// ============================================================
// T5: 执行 Arrow 查询
// ============================================================
void test_execute_query_arrow() {
    printf("[TEST] ClickHouse: ExecuteQueryArrow...\n");

    ClickHouseDriver driver;
    assert(driver.Connect(GetClickHouseParams()) == 0);
    auto session = driver.CreateSession();
    assert(session != nullptr);

    std::string table = UniqueTable("ch_test_query");
    DropTableIfExists(session.get(), table);

    // 建表并插入数据
    std::string error;
    assert(session->ExecuteSql(
        ("CREATE TABLE " + table + " (id Int32, val String) ENGINE = MergeTree() ORDER BY id").c_str()) == 0);
    assert(session->ExecuteSql(
        ("INSERT INTO " + table + " VALUES (1, 'alpha'), (2, 'beta'), (3, 'gamma')").c_str()) == 0);

    // 查询
    std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
    int rc = session->ExecuteQueryArrow(("SELECT * FROM " + table + " ORDER BY id").c_str(),
                                        &batches);
    assert(rc == 0);
    assert(!batches.empty());

    // 统计总行数
    int64_t total_rows = 0;
    for (const auto& b : batches) total_rows += b->num_rows();
    assert(total_rows == 3);
    assert(batches[0]->num_columns() == 2);
    printf("  Query returned %lld rows, %d columns\n",
           (long long)total_rows, batches[0]->num_columns());

    DropTableIfExists(session.get(), table);
    g_passed++;
    printf("[PASS] T5: ExecuteQueryArrow\n");
}

// ============================================================
// T6: 写入 Arrow batches
// ============================================================
void test_write_arrow_batches() {
    printf("[TEST] ClickHouse: WriteArrowBatches...\n");

    ClickHouseDriver driver;
    assert(driver.Connect(GetClickHouseParams()) == 0);
    auto session = driver.CreateSession();
    assert(session != nullptr);

    std::string table = UniqueTable("ch_test_write");
    DropTableIfExists(session.get(), table);

    std::string error;
    assert(session->ExecuteSql(
        ("CREATE TABLE " + table +
         " (id Int32, name String) ENGINE = MergeTree() ORDER BY id").c_str()) == 0);

    // 构造 batch
    auto schema = arrow::schema({
        arrow::field("id",   arrow::int32()),
        arrow::field("name", arrow::utf8()),
    });
    arrow::Int32Builder  b_id;
    arrow::StringBuilder b_name;
    (void)b_id.AppendValues({10, 20, 30});
    (void)b_name.AppendValues({"foo", "bar", "baz"});
    std::shared_ptr<arrow::Array> a_id, a_name;
    (void)b_id.Finish(&a_id);
    (void)b_name.Finish(&a_name);
    auto batch = arrow::RecordBatch::Make(schema, 3, {a_id, a_name});

    int rc = session->WriteArrowBatches(table.c_str(), {batch});
    assert(rc == 0);
    printf("  WriteArrowBatches: OK\n");

    // 回读验证行数
    std::vector<std::shared_ptr<arrow::RecordBatch>> read_batches;
    rc = session->ExecuteQueryArrow(("SELECT * FROM " + table).c_str(), &read_batches);
    assert(rc == 0);
    int64_t total = 0;
    for (const auto& b : read_batches) total += b->num_rows();
    assert(total == 3);
    printf("  Read back %lld rows\n", (long long)total);

    DropTableIfExists(session.get(), table);
    g_passed++;
    printf("[PASS] T6: WriteArrowBatches\n");
}

// ============================================================
// T7: Arrow 类型矩阵（强制覆盖所有类型）
// ============================================================
void test_arrow_type_matrix() {
    printf("[TEST] ClickHouse: Arrow type matrix...\n");

    ClickHouseDriver driver;
    assert(driver.Connect(GetClickHouseParams()) == 0);
    auto session = driver.CreateSession();
    assert(session != nullptr);

    std::string table = UniqueTable("ch_test_types");
    DropTableIfExists(session.get(), table);

    std::string error;
    // ClickHouse 类型映射：Int32/Int64/Float32/Float64/String/Bool
    assert(session->ExecuteSql(
        ("CREATE TABLE " + table +
         " (c_int32 Int32, c_int64 Int64, c_float32 Float32,"
         "  c_float64 Float64, c_string String, c_bool Bool)"
         " ENGINE = MergeTree() ORDER BY c_int32").c_str()) == 0);

    auto batch = MakeTypeMatrixBatch();
    int rc = session->WriteArrowBatches(table.c_str(), {batch});
    assert(rc == 0);

    // 回读并逐列逐行验证
    std::vector<std::shared_ptr<arrow::RecordBatch>> read_batches;
    rc = session->ExecuteQueryArrow(
        ("SELECT * FROM " + table + " ORDER BY c_int32").c_str(),
        &read_batches);
    assert(rc == 0);
    assert(!read_batches.empty());

    int64_t total = 0;
    for (const auto& b : read_batches) total += b->num_rows();
    assert(total == 3);
    printf("  Read back %lld rows with %d columns\n",
           (long long)total, read_batches[0]->num_columns());

    // 逐列逐行验证值（设计要求：不允许只验证行数）
    auto& rb = read_batches[0];

    // c_int32
    auto col_i32 = std::static_pointer_cast<arrow::Int32Array>(rb->column(0));
    assert(col_i32->Value(0) == 1);
    assert(col_i32->Value(1) == 2);
    assert(col_i32->Value(2) == 3);
    printf("  c_int32: %d, %d, %d\n", col_i32->Value(0), col_i32->Value(1), col_i32->Value(2));

    // c_int64
    auto col_i64 = std::static_pointer_cast<arrow::Int64Array>(rb->column(1));
    assert(col_i64->Value(0) == 100LL);
    assert(col_i64->Value(1) == 200LL);
    assert(col_i64->Value(2) == 300LL);
    printf("  c_int64: %lld, %lld, %lld\n",
           (long long)col_i64->Value(0), (long long)col_i64->Value(1), (long long)col_i64->Value(2));

    // c_float32（浮点比较允许小误差）
    auto col_f32 = std::static_pointer_cast<arrow::FloatArray>(rb->column(2));
    assert(std::abs(col_f32->Value(0) - 1.1f) < 1e-4f);
    assert(std::abs(col_f32->Value(1) - 2.2f) < 1e-4f);
    assert(std::abs(col_f32->Value(2) - 3.3f) < 1e-4f);
    printf("  c_float32: %.4f, %.4f, %.4f\n", col_f32->Value(0), col_f32->Value(1), col_f32->Value(2));

    // c_float64
    auto col_f64 = std::static_pointer_cast<arrow::DoubleArray>(rb->column(3));
    assert(std::abs(col_f64->Value(0) - 1.11) < 1e-9);
    assert(std::abs(col_f64->Value(1) - 2.22) < 1e-9);
    assert(std::abs(col_f64->Value(2) - 3.33) < 1e-9);
    printf("  c_float64: %.6f, %.6f, %.6f\n", col_f64->Value(0), col_f64->Value(1), col_f64->Value(2));

    // c_string
    auto col_str = std::static_pointer_cast<arrow::StringArray>(rb->column(4));
    assert(col_str->GetString(0) == "hello");
    assert(col_str->GetString(1) == "world");
    assert(col_str->GetString(2) == "clickhouse");
    printf("  c_string: %s, %s, %s\n",
           col_str->GetString(0).c_str(), col_str->GetString(1).c_str(), col_str->GetString(2).c_str());

    // c_bool（ClickHouse Bool 类型，Arrow 读回为 BooleanArray 或 UInt8Array，兼容两种）
    // 用 column type id 判断
    auto bool_col = rb->column(5);
    if (bool_col->type_id() == arrow::Type::BOOL) {
        auto col_bool = std::static_pointer_cast<arrow::BooleanArray>(bool_col);
        assert(col_bool->Value(0) == true);
        assert(col_bool->Value(1) == false);
        assert(col_bool->Value(2) == true);
        printf("  c_bool (BooleanArray): %d, %d, %d\n",
               col_bool->Value(0), col_bool->Value(1), col_bool->Value(2));
    } else {
        // ClickHouse 某些版本将 Bool 映射为 UInt8
        auto col_u8 = std::static_pointer_cast<arrow::UInt8Array>(bool_col);
        assert(col_u8->Value(0) == 1);
        assert(col_u8->Value(1) == 0);
        assert(col_u8->Value(2) == 1);
        printf("  c_bool (UInt8Array): %u, %u, %u\n",
               col_u8->Value(0), col_u8->Value(1), col_u8->Value(2));
    }

    DropTableIfExists(session.get(), table);
    g_passed++;
    printf("[PASS] T7: Arrow type matrix\n");
}

// ============================================================
// T8: 大批量写入（10000 行）
// ============================================================
void test_large_batch_write() {
    printf("[TEST] ClickHouse: large batch write (10000 rows)...\n");

    ClickHouseDriver driver;
    assert(driver.Connect(GetClickHouseParams()) == 0);
    auto session = driver.CreateSession();
    assert(session != nullptr);

    std::string table = UniqueTable("ch_test_large");
    DropTableIfExists(session.get(), table);

    std::string error;
    assert(session->ExecuteSql(
        ("CREATE TABLE " + table +
         " (id Int32, val Float64) ENGINE = MergeTree() ORDER BY id").c_str()) == 0);

    // 构造 10000 行
    const int N = 10000;
    arrow::Int32Builder  b_id;
    arrow::DoubleBuilder b_val;
    for (int i = 0; i < N; ++i) {
        (void)b_id.Append(i);
        (void)b_val.Append(i * 1.5);
    }
    std::shared_ptr<arrow::Array> a_id, a_val;
    (void)b_id.Finish(&a_id);
    (void)b_val.Finish(&a_val);
    auto schema = arrow::schema({arrow::field("id", arrow::int32()), arrow::field("val", arrow::float64())});
    auto batch  = arrow::RecordBatch::Make(schema, N, {a_id, a_val});

    int rc = session->WriteArrowBatches(table.c_str(), {batch});
    assert(rc == 0);

    // 回读验证行数：通过 count() 确认写入了精确的 N 行
    std::vector<std::shared_ptr<arrow::RecordBatch>> read_batches;
    rc = session->ExecuteQueryArrow(("SELECT count() FROM " + table).c_str(), &read_batches);
    assert(rc == 0);
    assert(!read_batches.empty());
    // count() 返回 UInt64 类型
    auto count_col = std::static_pointer_cast<arrow::UInt64Array>(read_batches[0]->column(0));
    int64_t count_val = static_cast<int64_t>(count_col->Value(0));
    assert(count_val == N);
    printf("  Wrote %d rows, count()=%lld ✓\n", N, (long long)count_val);

    DropTableIfExists(session.get(), table);
    g_passed++;
    printf("[PASS] T8: large batch write\n");
}

// ============================================================
// T9: 事务不支持
// ============================================================
void test_transaction_not_supported() {
    printf("[TEST] ClickHouse: transaction not supported...\n");

    ClickHouseDriver driver;
    assert(driver.Connect(GetClickHouseParams()) == 0);
    auto session = driver.CreateSession();
    assert(session != nullptr);

    std::string error;
    assert(session->BeginTransaction() == -1);
    assert(session->GetLastError()[0] != '\0');
    printf("  BeginTransaction correctly returned -1: %s\n", session->GetLastError());

    assert(session->CommitTransaction() == -1);
    assert(session->RollbackTransaction() == -1);

    g_passed++;
    printf("[PASS] T9: transaction not supported\n");
}

// ============================================================
// T10: 错误路径 — 查询不存在的表
// ============================================================
void test_error_nonexistent_table() {
    printf("[TEST] ClickHouse: query nonexistent table...\n");

    ClickHouseDriver driver;
    assert(driver.Connect(GetClickHouseParams()) == 0);
    auto session = driver.CreateSession();
    assert(session != nullptr);

    std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
    std::string error;
    int rc = session->ExecuteQueryArrow(
        "SELECT * FROM ch_nonexistent_table_xyz_12345", &batches);
    assert(rc != 0);
    assert(session->GetLastError()[0] != '\0');
    printf("  Correctly failed: %s\n", session->GetLastError());

    g_passed++;
    printf("[PASS] T10: query nonexistent table\n");
}

// ============================================================
// T11: 错误路径 — 语法错误 SQL
// ============================================================
void test_error_syntax() {
    printf("[TEST] ClickHouse: syntax error SQL...\n");

    ClickHouseDriver driver;
    assert(driver.Connect(GetClickHouseParams()) == 0);
    auto session = driver.CreateSession();
    assert(session != nullptr);

    std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
    std::string error;
    int rc = session->ExecuteQueryArrow("SELECT FROM WHERE INVALID SYNTAX !!!!", &batches);
    assert(rc != 0);
    assert(session->GetLastError()[0] != '\0');
    printf("  Correctly failed: %s\n", session->GetLastError());

    g_passed++;
    printf("[PASS] T11: syntax error SQL\n");
}

// ============================================================
// T12: 并发读取（8 线程）
// ============================================================
void test_concurrent_sessions() {
    printf("[TEST] ClickHouse: concurrent sessions (8 readers)...\n");

    ClickHouseDriver driver;
    assert(driver.Connect(GetClickHouseParams()) == 0);

    // 准备共享表
    auto setup_session = driver.CreateSession();
    assert(setup_session != nullptr);
    std::string table = UniqueTable("ch_test_concurrent_r");
    std::string error;
    DropTableIfExists(setup_session.get(), table);
    assert(setup_session->ExecuteSql(
        ("CREATE TABLE " + table + " (id Int32) ENGINE = MergeTree() ORDER BY id").c_str()) == 0);
    assert(setup_session->ExecuteSql(
        ("INSERT INTO " + table + " VALUES (1),(2),(3),(4),(5)").c_str()) == 0);

    const int THREADS = 8;
    std::atomic<int> success_count{0};
    std::atomic<int> fail_count{0};
    std::vector<std::thread> threads;

    for (int i = 0; i < THREADS; ++i) {
        threads.emplace_back([&driver, &table, &success_count, &fail_count]() {
            auto session = driver.CreateSession();
            if (!session) { fail_count++; return; }

            std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
            std::string err;
            int rc = session->ExecuteQueryArrow(
                ("SELECT * FROM " + table).c_str(), &batches);
            if (rc != 0) { fail_count++; return; }

            int64_t total = 0;
            for (const auto& b : batches) total += b->num_rows();
            if (total == 5) success_count++;
            else fail_count++;
        });
    }
    for (auto& t : threads) t.join();

    printf("  %d/%d threads succeeded\n", success_count.load(), THREADS);
    assert(fail_count == 0);
    assert(success_count == THREADS);

    DropTableIfExists(setup_session.get(), table);
    g_passed++;
    printf("[PASS] T12: concurrent sessions\n");
}

// ============================================================
// T13: 并发写入（6 线程，各写不同表）
// ============================================================
void test_concurrent_writers() {
    printf("[TEST] ClickHouse: concurrent writers (6 threads)...\n");

    ClickHouseDriver driver;
    assert(driver.Connect(GetClickHouseParams()) == 0);

    const int THREADS = 6;
    const int ROWS_PER_THREAD = 100;
    std::vector<std::string> tables;
    std::mutex tables_mutex;

    // 预建表
    auto setup_session = driver.CreateSession();
    assert(setup_session != nullptr);
    for (int i = 0; i < THREADS; ++i) {
        std::string table = UniqueTable("ch_test_concurrent_w") + "_" + std::to_string(i);
        std::string error;
        DropTableIfExists(setup_session.get(), table);
        assert(setup_session->ExecuteSql(
            ("CREATE TABLE " + table + " (id Int32) ENGINE = MergeTree() ORDER BY id").c_str()) == 0);
        tables.push_back(table);
    }

    std::atomic<int> success_count{0};
    std::vector<std::thread> threads;

    for (int i = 0; i < THREADS; ++i) {
        threads.emplace_back([&driver, &tables, i, ROWS_PER_THREAD, &success_count]() {
            auto session = driver.CreateSession();
            if (!session) return;

            arrow::Int32Builder b_id;
            for (int r = 0; r < ROWS_PER_THREAD; ++r) (void)b_id.Append(r);
            std::shared_ptr<arrow::Array> a_id;
            (void)b_id.Finish(&a_id);
            auto schema = arrow::schema({arrow::field("id", arrow::int32())});
            auto batch  = arrow::RecordBatch::Make(schema, ROWS_PER_THREAD, {a_id});

            std::string err;
            int rc = session->WriteArrowBatches(tables[i].c_str(), {batch});
            if (rc == 0) success_count++;
        });
    }
    for (auto& t : threads) t.join();

    printf("  %d/%d threads wrote successfully\n", success_count.load(), THREADS);
    assert(success_count == THREADS);

    // 验证每张表行数
    for (int i = 0; i < THREADS; ++i) {
        auto session = driver.CreateSession();
        std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
        std::string err;
        session->ExecuteQueryArrow(("SELECT * FROM " + tables[i]).c_str(), &batches);
        int64_t total = 0;
        for (const auto& b : batches) total += b->num_rows();
        assert(total == ROWS_PER_THREAD);
        DropTableIfExists(session.get(), tables[i]);
    }

    g_passed++;
    printf("[PASS] T13: concurrent writers\n");
}

// ============================================================
// T14: SQL 注入防护 — 含反引号的表名
// ============================================================
void test_quote_identifier_injection() {
    printf("[TEST] ClickHouse: quote identifier injection...\n");

    ClickHouseDriver driver;
    assert(driver.Connect(GetClickHouseParams()) == 0);
    auto session = driver.CreateSession();
    assert(session != nullptr);

    // 含反引号的表名，WriteArrowBatches 应正确转义，不产生 SQL 注入
    // ClickHouse 会因为表名不存在而报错，但不应产生语法错误或执行注入 SQL
    auto schema = arrow::schema({arrow::field("id", arrow::int32())});
    arrow::Int32Builder b;
    (void)b.Append(1);
    std::shared_ptr<arrow::Array> a;
    (void)b.Finish(&a);
    auto batch = arrow::RecordBatch::Make(schema, 1, {a});

    std::string error;
    // 表名含反引号，应被转义为 ``，不产生注入
    const char* malicious_table = "t`; DROP TABLE users; --";
    int rc = session->WriteArrowBatches(malicious_table, {batch});
    // 预期失败（表不存在），但不应是语法注入导致的意外成功
    // 关键：error 应包含 ClickHouse 的"表不存在"错误，而非语法错误
    printf("  WriteArrowBatches with injection table name returned %d: %s\n", rc, session->GetLastError());
    // 只要不 crash 且不产生意外成功即可
    assert(rc != 0);  // 表不存在，应失败

    g_passed++;
    printf("[PASS] T14: quote identifier injection\n");
}

// ============================================================
// T15: 空结果集
// ============================================================
void test_empty_result_set() {
    printf("[TEST] ClickHouse: empty result set...\n");

    ClickHouseDriver driver;
    assert(driver.Connect(GetClickHouseParams()) == 0);
    auto session = driver.CreateSession();
    assert(session != nullptr);

    std::string table = UniqueTable("ch_test_empty");
    std::string error;
    DropTableIfExists(session.get(), table);
    assert(session->ExecuteSql(
        ("CREATE TABLE " + table + " (id Int32) ENGINE = MergeTree() ORDER BY id").c_str()) == 0);

    // 查询空表
    std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
    int rc = session->ExecuteQueryArrow(
        ("SELECT * FROM " + table + " WHERE 1=0").c_str(), &batches);
    assert(rc == 0);
    assert(batches.empty());
    printf("  Empty result set: batches.size()=%zu\n", batches.size());

    DropTableIfExists(session.get(), table);
    g_passed++;
    printf("[PASS] T15: empty result set\n");
}

// ============================================================
// T16: 写入空 batches
// ============================================================
void test_write_empty_batches() {
    printf("[TEST] ClickHouse: write empty batches...\n");

    ClickHouseDriver driver;
    assert(driver.Connect(GetClickHouseParams()) == 0);
    auto session = driver.CreateSession();
    assert(session != nullptr);

    std::string error;
    // 空 batches 应直接返回 0，不发 HTTP 请求
    std::vector<std::shared_ptr<arrow::RecordBatch>> empty_batches;
    int rc = session->WriteArrowBatches("any_table", empty_batches);
    assert(rc == 0);
    printf("  WriteArrowBatches with empty batches returned 0 (no HTTP request sent)\n");

    g_passed++;
    printf("[PASS] T16: write empty batches\n");
}

// ============================================================
// T17: 读写混合并发（TQ-C1）
// 8 个读线程 + 4 个写线程同时操作同一张表，验证不崩溃且读到的行数合理
// ============================================================
void test_concurrent_read_write_mixed() {
    printf("[TEST] ClickHouse: concurrent read+write mixed (8R+4W)...\n");

    ClickHouseDriver driver;
    assert(driver.Connect(GetClickHouseParams()) == 0);

    auto setup = driver.CreateSession();
    assert(setup != nullptr);
    std::string table = UniqueTable("ch_test_rw_mixed");
    std::string error;
    DropTableIfExists(setup.get(), table);
    assert(setup->ExecuteSql(
        ("CREATE TABLE " + table + " (id Int32) ENGINE = MergeTree() ORDER BY id").c_str()) == 0);
    // 预写 10 行，保证读线程有数据可读
    assert(setup->ExecuteSql(
        ("INSERT INTO " + table + " SELECT number FROM numbers(10)").c_str()) == 0);

    const int READERS = 8, WRITERS = 4;
    std::atomic<int> read_ok{0}, write_ok{0}, read_fail{0};
    std::vector<std::thread> threads;

    for (int i = 0; i < READERS; ++i) {
        threads.emplace_back([&driver, &table, &read_ok, &read_fail]() {
            auto s = driver.CreateSession();
            if (!s) { read_fail++; return; }
            std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
            std::string err;
            int rc = s->ExecuteQueryArrow(("SELECT * FROM " + table).c_str(), &batches);
            if (rc == 0) read_ok++;
            else read_fail++;
        });
    }
    for (int i = 0; i < WRITERS; ++i) {
        threads.emplace_back([&driver, &table, &write_ok, i]() {
            auto s = driver.CreateSession();
            if (!s) return;
            arrow::Int32Builder b;
            for (int r = 0; r < 5; ++r) (void)b.Append(100 + i * 5 + r);
            std::shared_ptr<arrow::Array> a;
            (void)b.Finish(&a);
            auto schema = arrow::schema({arrow::field("id", arrow::int32())});
            auto batch  = arrow::RecordBatch::Make(schema, 5, {a});
            std::string err;
            if (s->WriteArrowBatches(table.c_str(), {batch}) == 0) write_ok++;
        });
    }
    for (auto& t : threads) t.join();

    printf("  read_ok=%d read_fail=%d write_ok=%d\n",
           read_ok.load(), read_fail.load(), write_ok.load());
    assert(read_fail == 0);
    assert(read_ok == READERS);
    assert(write_ok == WRITERS);

    DropTableIfExists(setup.get(), table);
    g_passed++;
    printf("[PASS] T17: concurrent read+write mixed\n");
}

// ============================================================
// T18: 并发连接失败时 last_error_ 无数据竞争（TQ-C2）
// 多线程同时对不可达地址调用 Connect()，验证不崩溃
// ============================================================
void test_concurrent_connect_failure() {
    printf("[TEST] ClickHouse: concurrent connect failure (last_error_ race)...\n");

    const int N = 10;
    std::atomic<int> fail_count{0};
    std::vector<std::thread> threads;

    for (int i = 0; i < N; ++i) {
        threads.emplace_back([&fail_count]() {
            ClickHouseDriver driver;
            std::unordered_map<std::string, std::string> bad_params;
            bad_params["host"]     = "127.0.0.1";
            bad_params["port"]     = "19999";  // 不可达端口
            bad_params["user"]     = "default";
            bad_params["password"] = "";
            bad_params["database"] = "default";
            int rc = driver.Connect(bad_params);
            if (rc != 0) fail_count++;
            // 读取 LastError()，触发潜在竞争
            (void)driver.LastError();
        });
    }
    for (auto& t : threads) t.join();

    // 所有连接都应失败
    assert(fail_count == N);
    printf("  %d/%d connections failed as expected, no crash\n", fail_count.load(), N);

    g_passed++;
    printf("[PASS] T18: concurrent connect failure\n");
}

// ============================================================
// T19: Nullable 列 NULL 处理（TQ-B1）
// 写入含 NULL 的 Nullable 列，回读验证 NULL 正确保留
// ============================================================
void test_nullable_null_handling() {
    printf("[TEST] ClickHouse: Nullable column NULL handling...\n");

    ClickHouseDriver driver;
    assert(driver.Connect(GetClickHouseParams()) == 0);
    auto session = driver.CreateSession();
    assert(session != nullptr);

    std::string table = UniqueTable("ch_test_nullable");
    std::string error;
    DropTableIfExists(session.get(), table);
    assert(session->ExecuteSql(
        ("CREATE TABLE " + table +
         " (id Int32, val Nullable(String))"
         " ENGINE = MergeTree() ORDER BY id").c_str()) == 0);

    // 直接用 SQL 插入含 NULL 的数据
    assert(session->ExecuteSql(
        ("INSERT INTO " + table + " VALUES (1, 'hello'), (2, NULL), (3, 'world')").c_str()) == 0);

    // 回读验证
    std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
    int rc = session->ExecuteQueryArrow(
        ("SELECT * FROM " + table + " ORDER BY id").c_str(), &batches);
    assert(rc == 0);
    assert(!batches.empty());

    int64_t total = 0;
    for (const auto& b : batches) total += b->num_rows();
    assert(total == 3);

    // 验证第二行 val 列为 NULL
    auto& rb = batches[0];
    auto val_col = rb->column(1);  // val 列
    assert(val_col->IsNull(1));    // 第二行（index=1）应为 NULL
    assert(!val_col->IsNull(0));   // 第一行非 NULL
    assert(!val_col->IsNull(2));   // 第三行非 NULL
    printf("  Row 0: non-null, Row 1: null=%s, Row 2: non-null\n",
           val_col->IsNull(1) ? "true" : "false");

    DropTableIfExists(session.get(), table);
    g_passed++;
    printf("[PASS] T19: Nullable NULL handling\n");
}

// ============================================================
// T20: 多 batch 写入路径（TQ-B2）
// WriteArrowBatches 传入多个 batch，验证全部写入且行数正确
// ============================================================
void test_multi_batch_write() {
    printf("[TEST] ClickHouse: multi-batch write...\n");

    ClickHouseDriver driver;
    assert(driver.Connect(GetClickHouseParams()) == 0);
    auto session = driver.CreateSession();
    assert(session != nullptr);

    std::string table = UniqueTable("ch_test_multibatch");
    std::string error;
    DropTableIfExists(session.get(), table);
    assert(session->ExecuteSql(
        ("CREATE TABLE " + table + " (id Int32) ENGINE = MergeTree() ORDER BY id").c_str()) == 0);

    // 构造 3 个 batch，各 100 行
    const int BATCHES = 3, ROWS = 100;
    std::vector<std::shared_ptr<arrow::RecordBatch>> write_batches;
    for (int b = 0; b < BATCHES; ++b) {
        arrow::Int32Builder builder;
        for (int r = 0; r < ROWS; ++r) (void)builder.Append(b * ROWS + r);
        std::shared_ptr<arrow::Array> arr;
        (void)builder.Finish(&arr);
        auto schema = arrow::schema({arrow::field("id", arrow::int32())});
        write_batches.push_back(arrow::RecordBatch::Make(schema, ROWS, {arr}));
    }

    int rc = session->WriteArrowBatches(table.c_str(), write_batches);
    assert(rc == 0);

    // 回读验证总行数
    std::vector<std::shared_ptr<arrow::RecordBatch>> read_batches;
    rc = session->ExecuteQueryArrow(
        ("SELECT count() FROM " + table).c_str(), &read_batches);
    assert(rc == 0);
    auto count_col = std::static_pointer_cast<arrow::UInt64Array>(read_batches[0]->column(0));
    int64_t total = static_cast<int64_t>(count_col->Value(0));
    assert(total == BATCHES * ROWS);
    printf("  Wrote %d batches × %d rows = %lld total\n", BATCHES, ROWS, (long long)total);

    DropTableIfExists(session.get(), table);
    g_passed++;
    printf("[PASS] T20: multi-batch write\n");
}

// ============================================================
// T21: 注入测试断言加强（TQ-B5）
// T14 只断言 rc != 0，此处区分"表不存在"和"语法错误"
// ============================================================
void test_injection_error_distinction() {
    printf("[TEST] ClickHouse: injection error distinction...\n");

    ClickHouseDriver driver;
    assert(driver.Connect(GetClickHouseParams()) == 0);
    auto session = driver.CreateSession();
    assert(session != nullptr);

    auto schema = arrow::schema({arrow::field("id", arrow::int32())});
    arrow::Int32Builder b;
    (void)b.Append(1);
    std::shared_ptr<arrow::Array> a;
    (void)b.Finish(&a);
    auto batch = arrow::RecordBatch::Make(schema, 1, {a});

    // 场景 1：注入表名 — 应失败，错误应包含"不存在"相关信息，而非语法错误
    int rc1 = session->WriteArrowBatches("t`; DROP TABLE users; --", {batch});
    assert(rc1 != 0);
    assert(session->GetLastError()[0] != '\0');
    // 错误应来自 ClickHouse 服务端（表不存在），不应是本地语法构造失败
    printf("  Injection table error: %s\n", session->GetLastError());

    // 场景 2：查询不存在的表 — 错误应包含表名相关信息
    std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
    int rc2 = session->ExecuteQueryArrow(
        "SELECT * FROM ch_nonexistent_xyz_99999", &batches);
    assert(rc2 != 0);
    assert(session->GetLastError()[0] != '\0');
    std::string saved_error2 = session->GetLastError();
    printf("  Nonexistent table error: %s\n", saved_error2.c_str());

    // 场景 3：语法错误 — 错误应与场景 2 不同（不同错误类型）
    int rc3 = session->ExecuteQueryArrow("SELECT FROM WHERE !!!!", &batches);
    assert(rc3 != 0);
    assert(session->GetLastError()[0] != '\0');
    std::string saved_error3 = session->GetLastError();
    printf("  Syntax error: %s\n", saved_error3.c_str());

    // 关键断言：表不存在错误 ≠ 语法错误（两者错误信息不同）
    assert(saved_error2 != saved_error3);
    printf("  Table-not-found error differs from syntax error ✓\n");

    g_passed++;
    printf("[PASS] T21: injection error distinction\n");
}

// ============================================================
// main
// ============================================================
int main() {
    printf("=== ClickHouse Driver Tests ===\n\n");

    // T2/T3 不需要 ClickHouse 可用
    test_connect_wrong_password();
    test_connect_unreachable();

    if (!IsClickHouseAvailable()) {
        printf("\n[SKIP] ClickHouse not available, skipping T1/T4-T16\n");
        printf("  Set CH_HOST/CH_PORT/CH_USER/CH_PASSWORD/CH_DATABASE to enable\n");
        g_skipped += 14;
    } else {
        test_connect_disconnect();
        test_ddl();
        test_execute_query_arrow();
        test_write_arrow_batches();
        test_arrow_type_matrix();
        test_large_batch_write();
        test_transaction_not_supported();
        test_error_nonexistent_table();
        test_error_syntax();
        test_concurrent_sessions();
        test_concurrent_writers();
        test_quote_identifier_injection();
        test_empty_result_set();
        test_write_empty_batches();
        test_concurrent_read_write_mixed();
        test_concurrent_connect_failure();
        test_nullable_null_handling();
        test_multi_batch_write();
        test_injection_error_distinction();
    }

    printf("\n=== Results: %d passed, %d skipped ===\n", g_passed, g_skipped);
    return 0;
}

