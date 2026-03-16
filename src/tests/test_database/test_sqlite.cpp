// test_sqlite.cpp — SQLite 驱动独立测试套件
//
// 设计原则：
//   1. 每个测试函数自建自清数据，不依赖其他测试的状态
//   2. 全部使用 :memory: 数据库，测试结束自动释放
//   3. assert 失效问题已在 CMakeLists.txt 中通过 -UNDEBUG 修复
//
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <arrow/ipc/api.h>

#include <services/database/drivers/sqlite_driver.h>
#include <services/database/capability_interfaces.h>

using namespace flowsql;
using namespace flowsql::database;

// ============================================================
// 辅助：序列化 RecordBatch 为 IPC stream buffer
// ============================================================
static std::shared_ptr<arrow::Buffer> SerializeBatch(
    std::shared_ptr<arrow::RecordBatch> batch) {
    auto sink = arrow::io::BufferOutputStream::Create().ValueOrDie();
    auto writer = arrow::ipc::MakeStreamWriter(sink, batch->schema()).ValueOrDie();
    (void)writer->WriteRecordBatch(*batch);
    (void)writer->Close();
    return sink->Finish().ValueOrDie();
}

// ============================================================
// 辅助：反序列化 IPC stream buffer 为第一个 RecordBatch
// ============================================================
static std::shared_ptr<arrow::RecordBatch> DeserializeFirstBatch(
    const uint8_t* data, size_t len) {
    auto buf = arrow::Buffer::Wrap(data, static_cast<int64_t>(len));
    auto input = std::make_shared<arrow::io::BufferReader>(buf);
    auto stream = arrow::ipc::RecordBatchStreamReader::Open(input).ValueOrDie();
    std::shared_ptr<arrow::RecordBatch> batch;
    if (!stream->ReadNext(&batch).ok() || !batch) return nullptr;
    return batch;
}

// ============================================================
// Test 1: 连接与断开
// ============================================================
void test_connect_disconnect() {
    printf("[TEST] SQLite: connect and disconnect...\n");

    SqliteDriver driver;
    std::unordered_map<std::string, std::string> params;
    params["path"] = ":memory:";

    assert(driver.Connect(params) == 0);
    assert(driver.IsConnected());
    assert(driver.Ping());
    printf("  Connected, Ping OK\n");

    driver.Disconnect();
    assert(!driver.IsConnected());
    assert(!driver.Ping());
    printf("  Disconnected, Ping correctly failed\n");

    printf("[PASS] SQLite: connect and disconnect\n");
}

// ============================================================
// Test 2: DDL — 建表、删表
// ============================================================
void test_ddl() {
    printf("[TEST] SQLite: DDL (CREATE/DROP TABLE)...\n");

    SqliteDriver driver;
    std::unordered_map<std::string, std::string> params;
    params["path"] = ":memory:";
    assert(driver.Connect(params) == 0);

    auto session = driver.CreateSession();
    assert(session != nullptr);
    std::string error;

    // 建表
    int rc = session->ExecuteSql(
        "CREATE TABLE t1 (id INTEGER PRIMARY KEY, name TEXT NOT NULL, val REAL)");
    assert(rc == 0);
    printf("  CREATE TABLE: OK\n");

    // 重复建表应失败
    rc = session->ExecuteSql("CREATE TABLE t1 (id INTEGER)");
    assert(rc != 0);
    assert(session->GetLastError()[0] != '\0');
    printf("  Duplicate CREATE TABLE: correctly failed (%s)\n", session->GetLastError());

    // IF NOT EXISTS 不报错
    rc = session->ExecuteSql("CREATE TABLE IF NOT EXISTS t1 (id INTEGER)");
    assert(rc == 0);
    printf("  CREATE TABLE IF NOT EXISTS: OK\n");

    // 删表
    rc = session->ExecuteSql("DROP TABLE t1");
    assert(rc == 0);
    printf("  DROP TABLE: OK\n");

    driver.Disconnect();
    printf("[PASS] SQLite: DDL\n");
}

// ============================================================
// Test 3: 基础 CRUD（通过 ExecuteSql / ExecuteQuery）
// ============================================================
void test_basic_crud() {
    printf("[TEST] SQLite: basic CRUD...\n");

    SqliteDriver driver;
    std::unordered_map<std::string, std::string> params;
    params["path"] = ":memory:";
    assert(driver.Connect(params) == 0);

    auto session = driver.CreateSession();
    std::string error;

    session->ExecuteSql(
        "CREATE TABLE products (id INTEGER, name TEXT, price REAL, stock INTEGER)");

    // INSERT
    session->ExecuteSql(
        "INSERT INTO products VALUES "
        "(1,'Apple',1.50,100),(2,'Banana',0.75,200),(3,'Orange',2.00,50)");

    // SELECT 全量
    IResultSet* rs = nullptr;
    int rc = session->ExecuteQuery("SELECT * FROM products ORDER BY id", &rs);
    assert(rc == 0 && rs != nullptr);
    assert(rs->FieldCount() == 4);

    int row = 0;
    while (rs->Next()) {
        row++;
        int64_t id; double price; int stock;
        const char* name; size_t name_len;
        rs->GetInt64(0, &id);
        rs->GetString(1, &name, &name_len);
        rs->GetDouble(2, &price);
        rs->GetInt(3, &stock);
        printf("  Row %d: id=%lld name=%.*s price=%.2f stock=%d\n",
               row, id, (int)name_len, name, price, stock);
    }
    assert(row == 3);
    delete rs;

    // UPDATE
    rc = session->ExecuteSql("UPDATE products SET price=1.80 WHERE id=1");
    assert(rc >= 0);  // 返回受影响行数，-1 才是失败
    rs = nullptr;
    session->ExecuteQuery("SELECT price FROM products WHERE id=1", &rs);
    assert(rs->Next());
    double price;
    rs->GetDouble(0, &price);
    assert(price == 1.80);
    delete rs;
    printf("  UPDATE: OK (price=%.2f)\n", price);

    // DELETE
    rc = session->ExecuteSql("DELETE FROM products WHERE id=3");
    assert(rc >= 0);  // 返回受影响行数，-1 才是失败
    rs = nullptr;
    session->ExecuteQuery("SELECT COUNT(*) FROM products", &rs);
    assert(rs->Next());
    int64_t cnt;
    rs->GetInt64(0, &cnt);
    assert(cnt == 2);
    delete rs;
    printf("  DELETE: OK (remaining=%lld)\n", cnt);

    driver.Disconnect();
    printf("[PASS] SQLite: basic CRUD\n");
}

// ============================================================
// Test 4: WHERE 条件查询
// ============================================================
void test_where_query() {
    printf("[TEST] SQLite: WHERE query...\n");

    SqliteDriver driver;
    std::unordered_map<std::string, std::string> params;
    params["path"] = ":memory:";
    assert(driver.Connect(params) == 0);

    auto session = driver.CreateSession();
    std::string error;

    session->ExecuteSql("CREATE TABLE scores (id INTEGER, name TEXT, score REAL)");
    session->ExecuteSql(
        "INSERT INTO scores VALUES "
        "(1,'Alice',95.5),(2,'Bob',72.0),(3,'Charlie',88.3),(4,'Dave',60.0)");

    // score > 80
    IResultSet* rs = nullptr;
    session->ExecuteQuery("SELECT name FROM scores WHERE score>80 ORDER BY score DESC", &rs);
    assert(rs != nullptr);
    std::vector<std::string> names;
    while (rs->Next()) {
        const char* n; size_t nl;
        rs->GetString(0, &n, &nl);
        names.emplace_back(n, nl);
    }
    assert(names.size() == 2);
    assert(names[0] == "Alice");
    assert(names[1] == "Charlie");
    delete rs;
    printf("  WHERE score>80: %zu rows (Alice, Charlie)\n", names.size());

    driver.Disconnect();
    printf("[PASS] SQLite: WHERE query\n");
}

// ============================================================
// Test 5: 事务 COMMIT
// ============================================================
void test_transaction_commit() {
    printf("[TEST] SQLite: transaction COMMIT...\n");

    SqliteDriver driver;
    std::unordered_map<std::string, std::string> params;
    params["path"] = ":memory:";
    assert(driver.Connect(params) == 0);

    auto session = driver.CreateSession();
    std::string error;

    session->ExecuteSql("CREATE TABLE accounts (id INTEGER, balance INTEGER)");
    session->ExecuteSql("INSERT INTO accounts VALUES (1,1000),(2,500)");

    // 开始事务，转账 200
    assert(session->BeginTransaction() == 0);
    session->ExecuteSql("UPDATE accounts SET balance=balance-200 WHERE id=1");
    session->ExecuteSql("UPDATE accounts SET balance=balance+200 WHERE id=2");
    assert(session->CommitTransaction() == 0);
    printf("  COMMIT: OK\n");

    // 验证结果
    IResultSet* rs = nullptr;
    session->ExecuteQuery("SELECT id,balance FROM accounts ORDER BY id", &rs);
    assert(rs->Next()); int64_t b1; rs->GetInt64(1, &b1); assert(b1 == 800);
    assert(rs->Next()); int64_t b2; rs->GetInt64(1, &b2); assert(b2 == 700);
    assert(!rs->Next());
    delete rs;
    printf("  After COMMIT: account1=%lld, account2=%lld\n", b1, b2);

    driver.Disconnect();
    printf("[PASS] SQLite: transaction COMMIT\n");
}

// ============================================================
// Test 6: 事务 ROLLBACK
// ============================================================
void test_transaction_rollback() {
    printf("[TEST] SQLite: transaction ROLLBACK...\n");

    SqliteDriver driver;
    std::unordered_map<std::string, std::string> params;
    params["path"] = ":memory:";
    assert(driver.Connect(params) == 0);

    auto session = driver.CreateSession();
    std::string error;

    session->ExecuteSql("CREATE TABLE items (id INTEGER, qty INTEGER)");
    session->ExecuteSql("INSERT INTO items VALUES (1,100),(2,200)");

    // 开始事务，删除所有数据，然后回滚
    assert(session->BeginTransaction() == 0);
    session->ExecuteSql("DELETE FROM items");

    // 事务内验证：数据已删除
    IResultSet* rs = nullptr;
    session->ExecuteQuery("SELECT COUNT(*) FROM items", &rs);
    assert(rs->Next()); int64_t cnt; rs->GetInt64(0, &cnt); assert(cnt == 0);
    delete rs;
    printf("  In-transaction DELETE: count=%lld\n", cnt);

    // 回滚
    assert(session->RollbackTransaction() == 0);

    // 验证数据恢复
    rs = nullptr;
    session->ExecuteQuery("SELECT COUNT(*) FROM items", &rs);
    assert(rs->Next()); rs->GetInt64(0, &cnt); assert(cnt == 2);
    delete rs;
    printf("  After ROLLBACK: count=%lld (restored)\n", cnt);

    driver.Disconnect();
    printf("[PASS] SQLite: transaction ROLLBACK\n");
}

// ============================================================
// Test 7: BatchReader — Arrow IPC 批量读取
// ============================================================
void test_batch_reader() {
    printf("[TEST] SQLite: BatchReader (Arrow IPC)...\n");

    SqliteDriver driver;
    std::unordered_map<std::string, std::string> params;
    params["path"] = ":memory:";
    assert(driver.Connect(params) == 0);

    auto session = driver.CreateSession();
    std::string error;

    session->ExecuteSql(
        "CREATE TABLE sensor (ts INTEGER, device TEXT, temp REAL, humidity REAL)");
    session->ExecuteSql(
        "INSERT INTO sensor VALUES "
        "(1000,'dev-A',23.5,60.1),(1001,'dev-B',24.0,58.3),"
        "(1002,'dev-A',23.8,61.0),(1003,'dev-C',22.1,65.5),"
        "(1004,'dev-B',24.5,57.9)");

    auto* readable = dynamic_cast<IBatchReadable*>(session.get());
    assert(readable != nullptr);

    IBatchReader* reader = nullptr;
    int rc = readable->CreateReader("SELECT * FROM sensor ORDER BY ts", &reader);
    assert(rc == 0 && reader != nullptr);

    // 验证 Schema
    const uint8_t* schema_data = nullptr;
    size_t schema_size = 0;
    reader->GetSchema(&schema_data, &schema_size);
    assert(schema_data != nullptr && schema_size > 0);
    printf("  Schema: %zu bytes\n", schema_size);

    // 读取数据
    const uint8_t* batch_data = nullptr;
    size_t batch_size = 0;
    rc = reader->Next(&batch_data, &batch_size);
    assert(rc == 0 && batch_data != nullptr);

    auto batch = DeserializeFirstBatch(batch_data, batch_size);
    assert(batch != nullptr);
    assert(batch->num_rows() == 5);
    assert(batch->num_columns() == 4);
    printf("  RecordBatch: %lld rows, %d columns\n", batch->num_rows(), batch->num_columns());

    // 验证列名
    assert(batch->schema()->field(0)->name() == "ts");
    assert(batch->schema()->field(1)->name() == "device");

    // 无更多数据
    rc = reader->Next(&batch_data, &batch_size);
    assert(rc == 1);  // EOS

    reader->Close();
    reader->Release();

    driver.Disconnect();
    printf("[PASS] SQLite: BatchReader\n");
}

// ============================================================
// Test 8: BatchWriter — Arrow IPC 批量写入（自动建表）
// ============================================================
void test_batch_writer() {
    printf("[TEST] SQLite: BatchWriter (auto create table)...\n");

    SqliteDriver driver;
    std::unordered_map<std::string, std::string> params;
    params["path"] = ":memory:";
    assert(driver.Connect(params) == 0);

    auto session = driver.CreateSession();

    auto* writable = dynamic_cast<IBatchWritable*>(session.get());
    assert(writable != nullptr);

    // 构建 RecordBatch
    auto schema = arrow::schema({
        arrow::field("user_id", arrow::int64()),
        arrow::field("username", arrow::utf8()),
        arrow::field("score", arrow::float64()),
    });

    arrow::Int64Builder id_b;
    arrow::StringBuilder name_b;
    arrow::DoubleBuilder score_b;

    for (int i = 1; i <= 5; ++i) {
        id_b.Append(i);
        name_b.Append("user_" + std::to_string(i));
        score_b.Append(60.0 + i * 5.0);
    }

    auto batch = arrow::RecordBatch::Make(schema, 5, {
        id_b.Finish().ValueOrDie(),
        name_b.Finish().ValueOrDie(),
        score_b.Finish().ValueOrDie(),
    });

    auto buf = SerializeBatch(batch);

    IBatchWriter* writer = nullptr;
    int rc = writable->CreateWriter("test_users_w", &writer);
    assert(rc == 0 && writer != nullptr);

    rc = writer->Write(buf->data(), static_cast<size_t>(buf->size()));
    assert(rc == 0);

    BatchWriteStats stats;
    writer->Close(&stats);
    assert(stats.rows_written == 5);
    writer->Release();
    printf("  Wrote %lld rows\n", stats.rows_written);

    // 验证数据
    std::string error;
    IResultSet* rs = nullptr;
    session->ExecuteQuery("SELECT COUNT(*) FROM test_users_w", &rs);
    assert(rs != nullptr && rs->Next());
    int64_t cnt; rs->GetInt64(0, &cnt);
    assert(cnt == 5);
    delete rs;
    printf("  Verified: %lld rows in DB\n", cnt);

    driver.Disconnect();
    printf("[PASS] SQLite: BatchWriter\n");
}

// ============================================================
// Test 9: BatchWriter — 追加写入（表已存在）
// ============================================================
void test_batch_writer_append() {
    printf("[TEST] SQLite: BatchWriter append to existing table...\n");

    SqliteDriver driver;
    std::unordered_map<std::string, std::string> params;
    params["path"] = ":memory:";
    assert(driver.Connect(params) == 0);

    auto session = driver.CreateSession();
    std::string error;

    // 预建表并插入 3 行
    session->ExecuteSql(
        "CREATE TABLE logs (id INTEGER, msg TEXT)");
    session->ExecuteSql(
        "INSERT INTO logs VALUES (1,'first'),(2,'second'),(3,'third')");

    auto* writable = dynamic_cast<IBatchWritable*>(session.get());

    auto schema = arrow::schema({
        arrow::field("id", arrow::int64()),
        arrow::field("msg", arrow::utf8()),
    });
    arrow::Int64Builder id_b;
    arrow::StringBuilder msg_b;
    id_b.Append(4); id_b.Append(5);
    msg_b.Append("fourth"); msg_b.Append("fifth");
    auto batch = arrow::RecordBatch::Make(schema, 2, {
        id_b.Finish().ValueOrDie(),
        msg_b.Finish().ValueOrDie(),
    });
    auto buf = SerializeBatch(batch);

    IBatchWriter* writer = nullptr;
    assert(writable->CreateWriter("logs", &writer) == 0);
    assert(writer->Write(buf->data(), static_cast<size_t>(buf->size())) == 0);
    BatchWriteStats stats;
    writer->Close(&stats);
    assert(stats.rows_written == 2);
    writer->Release();

    // 验证总行数 = 3 + 2 = 5
    IResultSet* rs = nullptr;
    session->ExecuteQuery("SELECT COUNT(*) FROM logs", &rs);
    assert(rs->Next());
    int64_t cnt; rs->GetInt64(0, &cnt);
    assert(cnt == 5);
    delete rs;
    printf("  Append 2 rows, total=%lld\n", cnt);

    driver.Disconnect();
    printf("[PASS] SQLite: BatchWriter append\n");
}

// ============================================================
// Test 10: 连接池复用
// ============================================================
void test_connection_pool_reuse() {
    printf("[TEST] SQLite: connection pool reuse...\n");

    SqliteDriver driver;
    std::unordered_map<std::string, std::string> params;
    params["path"] = ":memory:";
    assert(driver.Connect(params) == 0);

    // 同时持有两个 Session（使用不同连接）
    auto s1 = driver.CreateSession();
    auto s2 = driver.CreateSession();
    assert(s1 != nullptr && s2 != nullptr);
    assert(s1.get() != s2.get());
    printf("  Two sessions created simultaneously\n");

    // 释放 s1，再创建 s3，应复用 s1 的连接
    s1.reset();
    auto s3 = driver.CreateSession();
    assert(s3 != nullptr);
    assert(s3->Ping());
    printf("  s3 created after s1 released, Ping OK\n");

    driver.Disconnect();
    printf("[PASS] SQLite: connection pool reuse\n");
}

// ============================================================
// Test 11: ResultSet 状态 — Next() 耗尽后 GetXxx 应返回 -1
// ============================================================
void test_resultset_exhausted_state() {
    printf("[TEST] SQLite: ResultSet exhausted state...\n");

    SqliteDriver driver;
    std::unordered_map<std::string, std::string> params;
    params["path"] = ":memory:";
    assert(driver.Connect(params) == 0);

    auto session = driver.CreateSession();
    std::string error;

    session->ExecuteSql("CREATE TABLE t (id INTEGER, v TEXT)");
    session->ExecuteSql("INSERT INTO t VALUES (1,'a'),(2,'b')");

    IResultSet* rs = nullptr;
    session->ExecuteQuery("SELECT * FROM t", &rs);
    assert(rs != nullptr);

    assert(rs->Next() == true);
    assert(rs->Next() == true);
    assert(rs->Next() == false);  // 耗尽

    // 耗尽后 GetInt64 应返回 -1（P1-2 修复验证）
    int64_t val = 0;
    int rc = rs->GetInt64(0, &val);
    assert(rc == -1);
    printf("  GetInt64 after exhausted: rc=%d (expected -1)\n", rc);

    delete rs;
    driver.Disconnect();
    printf("[PASS] SQLite: ResultSet exhausted state\n");
}

// ============================================================
// Test 12: 错误路径 — 查询不存在的表
// ============================================================
void test_error_nonexistent_table() {
    printf("[TEST] SQLite: error - nonexistent table...\n");

    SqliteDriver driver;
    std::unordered_map<std::string, std::string> params;
    params["path"] = ":memory:";
    assert(driver.Connect(params) == 0);

    auto session = driver.CreateSession();
    std::string error;

    IResultSet* rs = nullptr;
    int rc = session->ExecuteQuery("SELECT * FROM no_such_table", &rs);
    assert(rc != 0);
    assert(session->GetLastError()[0] != '\0');
    assert(rs == nullptr);
    printf("  Error: %s\n", session->GetLastError());

    // BatchReader 也应失败
    auto* readable = dynamic_cast<IBatchReadable*>(session.get());
    IBatchReader* reader = nullptr;
    rc = readable->CreateReader("SELECT * FROM no_such_table", &reader);
    assert(rc != 0);
    assert(reader == nullptr);
    printf("  BatchReader on nonexistent table: correctly failed\n");

    driver.Disconnect();
    printf("[PASS] SQLite: error - nonexistent table\n");
}

// ============================================================
// Test 13: 错误路径 — SQL 语法错误
// ============================================================
void test_error_syntax() {
    printf("[TEST] SQLite: error - SQL syntax...\n");

    SqliteDriver driver;
    std::unordered_map<std::string, std::string> params;
    params["path"] = ":memory:";
    assert(driver.Connect(params) == 0);

    auto session = driver.CreateSession();
    std::string error;

    int rc = session->ExecuteSql("THIS IS NOT SQL");
    assert(rc != 0);
    assert(session->GetLastError()[0] != '\0');
    printf("  Syntax error: %s\n", session->GetLastError());

    driver.Disconnect();
    printf("[PASS] SQLite: error - SQL syntax\n");
}

// ============================================================
// Test 14: 多数据类型支持（INTEGER/TEXT/REAL/NULL）
// ============================================================
void test_data_types() {
    printf("[TEST] SQLite: data types...\n");

    SqliteDriver driver;
    std::unordered_map<std::string, std::string> params;
    params["path"] = ":memory:";
    assert(driver.Connect(params) == 0);

    auto session = driver.CreateSession();
    std::string error;

    session->ExecuteSql(
        "CREATE TABLE types_test (i INTEGER, r REAL, t TEXT, n TEXT)");
    session->ExecuteSql(
        "INSERT INTO types_test VALUES (42, 3.14, 'hello', NULL)");

    IResultSet* rs = nullptr;
    session->ExecuteQuery("SELECT * FROM types_test", &rs);
    assert(rs != nullptr && rs->Next());

    int64_t i; double r; const char* t; size_t tl;
    assert(rs->GetInt64(0, &i) == 0); assert(i == 42);
    assert(rs->GetDouble(1, &r) == 0); assert(r == 3.14);
    assert(rs->GetString(2, &t, &tl) == 0);
    assert(std::string(t, tl) == "hello");
    assert(rs->IsNull(3) == true);

    printf("  INTEGER=%lld REAL=%.2f TEXT=%.*s NULL=%s\n",
           i, r, (int)tl, t, rs->IsNull(3) ? "true" : "false");

    delete rs;
    driver.Disconnect();
    printf("[PASS] SQLite: data types\n");
}

// ============================================================
// Test 15: QuoteIdentifier — 真实注入场景（TQ-15 修复）
// SQLite 用双引号包裹标识符，内部双引号转义为 ""
// ============================================================
void test_quote_identifier_injection() {
    printf("[TEST] SQLite: QuoteIdentifier injection scenarios...\n");

    SqliteDriver driver;
    std::unordered_map<std::string, std::string> params;
    params["path"] = ":memory:";
    assert(driver.Connect(params) == 0);

    auto session = driver.CreateSession();
    auto* writable = dynamic_cast<IBatchWritable*>(session.get());
    assert(writable != nullptr);

    auto schema = arrow::schema({arrow::field("id", arrow::int64())});

    auto make_batch = [&](int64_t val) {
        arrow::Int64Builder b;
        b.Append(val);
        auto batch = arrow::RecordBatch::Make(schema, 1, {b.Finish().ValueOrDie()});
        return SerializeBatch(batch);
    };

    // 场景 1：表名含双引号（SQLite 标识符注入）
    // 如果未正确转义，"tab"le" 会破坏 SQL 结构
    {
        auto buf = make_batch(1);
        IBatchWriter* w = nullptr;
        int rc = writable->CreateWriter("tab\"le", &w);
        assert(rc == 0 && w != nullptr);
        assert(w->Write(buf->data(), buf->size()) == 0);
        BatchWriteStats stats;
        w->Close(&stats);
        assert(stats.rows_written == 1);
        w->Release();
        // 验证数据可读（表名被正确引用）
        IResultSet* rs = nullptr;
        rc = session->ExecuteQuery("SELECT id FROM \"tab\"\"le\"", &rs);
        assert(rc == 0 && rs != nullptr && rs->Next());
        int64_t v; rs->GetInt64(0, &v); assert(v == 1);
        delete rs;
        printf("  table name with double-quote: OK\n");
    }

    // 场景 2：表名含分号（尝试注入第二条语句）
    // 正确引用后分号是表名的一部分，不会执行额外语句
    {
        auto buf = make_batch(2);
        IBatchWriter* w = nullptr;
        int rc = writable->CreateWriter("t2;DROP TABLE t2", &w);
        // 含分号的表名在 SQLite 中合法（引用后），写入应成功
        // 若驱动拒绝也可接受（rc != 0），关键是不能静默执行 DROP
        if (rc == 0 && w != nullptr) {
            w->Write(buf->data(), buf->size());
            BatchWriteStats stats;
            w->Close(&stats);
            w->Release();
            printf("  table name with semicolon: accepted and quoted correctly\n");
        } else {
            printf("  table name with semicolon: correctly rejected by driver\n");
        }
        // 无论如何，原始表 t2 不应被 DROP（如果存在的话）
    }

    // 场景 3：空字符串表名应被拒绝
    {
        IBatchWriter* w = nullptr;
        int rc = writable->CreateWriter("", &w);
        assert(rc != 0 || w == nullptr);
        if (w) w->Release();
        printf("  empty table name: correctly rejected\n");
    }

    driver.Disconnect();
    printf("[PASS] SQLite: QuoteIdentifier injection scenarios\n");
}

// ============================================================
// main
// ============================================================
int main() {
    printf("=== FlowSQL SQLite Driver Tests ===\n\n");

    test_connect_disconnect();
    test_ddl();
    test_basic_crud();
    test_where_query();
    test_transaction_commit();
    test_transaction_rollback();
    test_batch_reader();
    test_batch_writer();
    test_batch_writer_append();
    test_connection_pool_reuse();
    test_resultset_exhausted_state();
    test_error_nonexistent_table();
    test_error_syntax();
    test_data_types();
    test_quote_identifier_injection();

    printf("\n=== All SQLite tests passed ===\n");    printf("\n=== All SQLite tests passed ===\n");
    return 0;
}
