#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <chrono>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <arrow/ipc/api.h>

#include <services/database/db_session.h>
#include <services/database/drivers/sqlite_driver.h>

using namespace flowsql;
using namespace flowsql::database;

// ============================================================
// Test 1: SQLite Session 创建和销毁
// ============================================================
void test_session_create_destroy() {
    printf("[TEST] SQLite Session: create and destroy...\n");

    SqliteDriver driver;
    std::unordered_map<std::string, std::string> params;
    params["path"] = ":memory:";

    int rc = driver.Connect(params);
    assert(rc == 0);
    assert(driver.IsConnected());
    printf("  Driver connected\n");

    // 创建 Session
    auto session = driver.CreateSession();
    assert(session != nullptr);
    printf("  Session created\n");

    // 验证 Ping
    bool ping_ok = session->Ping();
    assert(ping_ok);
    printf("  Session ping: OK\n");

    // Session 析构时自动归还连接到池
    session.reset();
    printf("  Session destroyed (connection returned to pool)\n");

    driver.Disconnect();
    printf("[PASS] SQLite Session: create and destroy\n");
}

// ============================================================
// Test 2: SQLite Session 执行查询
// ============================================================
void test_session_execute_query() {
    printf("[TEST] SQLite Session: execute query...\n");

    SqliteDriver driver;
    std::unordered_map<std::string, std::string> params;
    params["path"] = ":memory:";

    driver.Connect(params);
    auto session = driver.CreateSession();
    assert(session != nullptr);

    // 建表
    std::string error;
    int rc = session->ExecuteSql("CREATE TABLE users (id INTEGER, name TEXT, score REAL)", &error);
    assert(rc == 0);
    printf("  Table created\n");

    // 插入数据
    rc = session->ExecuteSql(
        "INSERT INTO users VALUES (1, 'Alice', 95.5), (2, 'Bob', 87.3), (3, 'Charlie', 92.1)",
        &error);
    assert(rc == 0);
    printf("  Data inserted\n");

    // 查询数据
    IResultSet* result = nullptr;
    rc = session->ExecuteQuery("SELECT * FROM users", &result, &error);
    assert(rc == 0 && result != nullptr);
    printf("  Query executed\n");

    // 验证结果集
    assert(result->FieldCount() == 3);
    printf("  Field count: %d\n", result->FieldCount());

    // 遍历结果
    int row_count = 0;
    while (result->Next()) {
        row_count++;
        int64_t id;
        const char* name;
        size_t name_len;
        double score;

        result->GetInt64(0, &id);
        result->GetString(1, &name, &name_len);
        result->GetDouble(2, &score);

        printf("  Row %d: id=%lld, name=%.*s, score=%.1f\n",
               row_count, id, (int)name_len, name, score);
    }
    assert(row_count == 3);

    delete result;
    printf("[PASS] SQLite Session: execute query\n");
}

// ============================================================
// Test 3: SQLite Session 事务管理
// ============================================================
void test_session_transaction() {
    printf("[TEST] SQLite Session: transaction...\n");

    SqliteDriver driver;
    std::unordered_map<std::string, std::string> params;
    params["path"] = ":memory:";

    driver.Connect(params);
    auto session = driver.CreateSession();
    assert(session != nullptr);

    std::string error;

    // 建表
    session->ExecuteSql("CREATE TABLE accounts (id INTEGER, balance INTEGER)", &error);

    // 开始事务
    int rc = session->BeginTransaction(&error);
    assert(rc == 0);
    printf("  Transaction began\n");

    // 插入数据
    session->ExecuteSql("INSERT INTO accounts VALUES (1, 1000)", &error);
    session->ExecuteSql("INSERT INTO accounts VALUES (2, 500)", &error);
    printf("  Data inserted in transaction\n");

    // 验证数据（事务内可见）
    IResultSet* result = nullptr;
    rc = session->ExecuteQuery("SELECT SUM(balance) FROM accounts", &result, &error);
    assert(rc == 0 && result != nullptr);
    assert(result->Next());
    int64_t sum;
    result->GetInt64(0, &sum);
    assert(sum == 1500);
    printf("  Sum in transaction: %lld\n", sum);
    delete result;

    // 提交事务
    rc = session->CommitTransaction(&error);
    assert(rc == 0);
    printf("  Transaction committed\n");

    // 验证数据（提交后持久化）
    result = nullptr;
    rc = session->ExecuteQuery("SELECT SUM(balance) FROM accounts", &result, &error);
    assert(rc == 0 && result != nullptr);
    assert(result->Next());
    result->GetInt64(0, &sum);
    assert(sum == 1500);
    printf("  Sum after commit: %lld\n", sum);
    delete result;

    // 测试回滚
    session->BeginTransaction(&error);
    session->ExecuteSql("DELETE FROM accounts", &error);
    printf("  Data deleted in uncommitted transaction\n");

    // 验证删除（事务内）
    result = nullptr;
    session->ExecuteQuery("SELECT COUNT(*) FROM accounts", &result, &error);
    assert(result->Next());
    int64_t count;
    result->GetInt64(0, &count);
    assert(count == 0);
    delete result;

    // 回滚
    session->RollbackTransaction(&error);
    printf("  Transaction rolled back\n");

    // 验证数据恢复
    result = nullptr;
    session->ExecuteQuery("SELECT COUNT(*) FROM accounts", &result, &error);
    assert(result->Next());
    result->GetInt64(0, &count);
    assert(count == 2);  // 数据恢复
    printf("  Count after rollback: %lld (data restored)\n", count);
    delete result;

    printf("[PASS] SQLite Session: transaction\n");
}

// ============================================================
// Test 4: SQLite Session 连接池复用
// ============================================================
void test_session_pool_reuse() {
    printf("[TEST] SQLite Session: connection pool reuse...\n");

    SqliteDriver driver;
    std::unordered_map<std::string, std::string> params;
    params["path"] = ":memory:";

    driver.Connect(params);

    // 多次创建 Session，验证连接复用
    auto session1 = driver.CreateSession();
    auto session2 = driver.CreateSession();

    // 两个 Session 应该使用不同的连接（因为同时在使用）
    assert(session1 != nullptr);
    assert(session2 != nullptr);
    printf("  Created 2 sessions\n");

    // 释放 session1
    session1.reset();
    printf("  Released session1\n");

    // 创建 session3，应该复用 session1 的连接
    auto session3 = driver.CreateSession();
    assert(session3 != nullptr);
    printf("  Created session3 (should reuse connection)\n");

    // 验证 session3 可以正常执行查询
    session3->ExecuteSql("CREATE TABLE test_reuse (id INTEGER)", nullptr);
    IResultSet* result = nullptr;
    session3->ExecuteQuery("SELECT 1", &result, nullptr);
    assert(result != nullptr);
    delete result;

    printf("[PASS] SQLite Session: connection pool reuse\n");
}

// ============================================================
// Test 5: SQLite 批量读取器（通过 Session）
// ============================================================
void test_batch_reader() {
    printf("[TEST] SQLite Batch Reader...\n");

    SqliteDriver driver;
    std::unordered_map<std::string, std::string> params;
    params["path"] = ":memory:";

    driver.Connect(params);
    auto session = driver.CreateSession();
    assert(session != nullptr);

    std::string error;

    // 建表插入数据
    session->ExecuteSql("CREATE TABLE products (id INTEGER, name TEXT, price REAL)", &error);
    session->ExecuteSql(
        "INSERT INTO products VALUES "
        "(1, 'Apple', 1.50), (2, 'Banana', 0.75), (3, 'Orange', 2.00), "
        "(4, 'Grape', 3.50), (5, 'Mango', 4.25)",
        &error);

    // 创建 Reader
    IBatchReader* reader = nullptr;
    auto* batch_readable = dynamic_cast<IBatchReadable*>(session.get());
    assert(batch_readable != nullptr);

    int rc = batch_readable->CreateReader("SELECT * FROM products", &reader);
    assert(rc == 0 && reader != nullptr);
    printf("  Batch reader created\n");

    // 获取 Schema
    const uint8_t* schema_data = nullptr;
    size_t schema_size = 0;
    reader->GetSchema(&schema_data, &schema_size);
    assert(schema_data != nullptr);
    printf("  Schema: %zu bytes\n", schema_size);

    // 读取数据
    const uint8_t* batch_data = nullptr;
    size_t batch_size = 0;
    int next_rc = reader->Next(&batch_data, &batch_size);
    assert(next_rc == 0);
    assert(batch_data != nullptr);
    printf("  Batch data: %zu bytes\n", batch_size);

    // 反序列化验证
    auto buffer = arrow::Buffer::Wrap(batch_data, static_cast<int64_t>(batch_size));
    auto input = std::make_shared<arrow::io::BufferReader>(buffer);
    auto stream = arrow::ipc::RecordBatchStreamReader::Open(input).ValueOrDie();
    std::shared_ptr<arrow::RecordBatch> batch;
    assert(stream->ReadNext(&batch).ok() && batch);
    printf("  RecordBatch: %lld rows, %d columns\n", batch->num_rows(), batch->num_columns());

    reader->Close();
    reader->Release();

    printf("[PASS] SQLite Batch Reader\n");
}

// ============================================================
// Test 6: SQLite 批量写入器（通过 Session）
// ============================================================
void test_batch_writer() {
    printf("[TEST] SQLite Batch Writer...\n");

    SqliteDriver driver;
    std::unordered_map<std::string, std::string> params;
    params["path"] = ":memory:";

    driver.Connect(params);
    auto session = driver.CreateSession();
    assert(session != nullptr);

    // 创建 Writer
    IBatchWriter* writer = nullptr;
    auto* batch_writable = dynamic_cast<IBatchWritable*>(session.get());
    assert(batch_writable != nullptr);

    int rc = batch_writable->CreateWriter("test_writer", &writer);
    assert(rc == 0 && writer != nullptr);
    printf("  Batch writer created\n");

    // 构建 Arrow RecordBatch
    auto schema = arrow::schema({
        arrow::field("key", arrow::int64()),
        arrow::field("value", arrow::utf8()),
    });

    arrow::Int64Builder key_builder;
    arrow::StringBuilder value_builder;

    key_builder.Append(100);
    key_builder.Append(200);
    key_builder.Append(300);
    value_builder.Append("foo");
    value_builder.Append("bar");
    value_builder.Append("baz");

    auto key_arr = key_builder.Finish().ValueOrDie();
    auto val_arr = value_builder.Finish().ValueOrDie();
    auto batch = arrow::RecordBatch::Make(schema, 3, {key_arr, val_arr});

    // 序列化
    auto sink = arrow::io::BufferOutputStream::Create().ValueOrDie();
    auto ipc_writer = arrow::ipc::MakeStreamWriter(sink, schema).ValueOrDie();
    ipc_writer->WriteRecordBatch(*batch);
    ipc_writer->Close();
    auto buffer = sink->Finish().ValueOrDie();

    // 写入
    rc = writer->Write(buffer->data(), static_cast<size_t>(buffer->size()));
    assert(rc == 0);
    printf("  Data written\n");

    // 关闭并获取统计
    BatchWriteStats stats;
    writer->Close(&stats);
    printf("  Rows written: %lld\n", stats.rows_written);
    assert(stats.rows_written == 3);
    writer->Release();

    // 验证数据
    IResultSet* result = nullptr;
    session->ExecuteQuery("SELECT * FROM test_writer", &result, nullptr);
    assert(result != nullptr);

    int count = 0;
    while (result->Next()) {
        count++;
    }
    assert(count == 3);
    printf("  Verified: %d rows in database\n", count);
    delete result;

    printf("[PASS] SQLite Batch Writer\n");
}

// ============================================================
// Test 7: 健康检查（Ping）
// ============================================================
void test_health_check() {
    printf("[TEST] Health check (Ping)...\n");

    SqliteDriver driver;
    std::unordered_map<std::string, std::string> params;
    params["path"] = ":memory:";

    driver.Connect(params);
    auto session = driver.CreateSession();

    // 多次 Ping 验证
    for (int i = 0; i < 5; ++i) {
        bool ok = session->Ping();
        assert(ok);
    }
    printf("  Ping x5: all passed\n");

    // 验证驱动级别的 Ping
    bool driver_ping = driver.Ping();
    assert(driver_ping);
    printf("  Driver Ping: passed\n");

    driver.Disconnect();

    // 断开后 Ping 应失败
    bool disconnected_ping = driver.Ping();
    assert(!disconnected_ping);
    printf("  Ping after disconnect: correctly failed\n");

    printf("[PASS] Health check (Ping)\n");
}

// ============================================================
// main
// ============================================================
int main() {
    printf("=== FlowSQL SQLite Session E2E Tests ===\n\n");

    test_session_create_destroy();
    test_session_execute_query();
    test_session_transaction();
    test_session_pool_reuse();
    test_batch_reader();
    test_batch_writer();
    test_health_check();

    printf("\n=== All SQLite session E2E tests passed ===\n");
    return 0;
}
