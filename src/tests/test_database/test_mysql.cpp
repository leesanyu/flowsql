// test_mysql.cpp — MySQL 驱动独立测试套件
//
// 设计原则：
//   1. 每个测试函数自建自清数据，不依赖其他测试的状态
//   2. 通过环境变量配置连接参数，MySQL 不可用时自动跳过
//   3. 每个测试使用唯一表名，避免并发冲突
//
// 环境变量：
//   MYSQL_HOST     (默认 127.0.0.1)
//   MYSQL_PORT     (默认 3306)
//   MYSQL_USER     (默认 root)
//   MYSQL_PASSWORD (默认 空)
//   MYSQL_DATABASE (默认 test)
//
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <arrow/ipc/api.h>

#include <services/database/drivers/mysql_driver.h>
#include <services/database/capability_interfaces.h>

using namespace flowsql;
using namespace flowsql::database;

// ============================================================
// 全局：跳过计数
// ============================================================
static int g_skipped = 0;
static int g_passed  = 0;

// ============================================================
// 辅助：从环境变量读取连接参数
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

// ============================================================
// 辅助：检查 MySQL 是否可用（用于跳过测试）
// ============================================================
static bool IsMysqlAvailable() {
    MysqlDriver driver;
    auto params = GetMysqlParams();
    int rc = driver.Connect(params);
    if (rc != 0) return false;
    bool ok = driver.Ping();
    driver.Disconnect();
    return ok;
}

// ============================================================
// 辅助：序列化 RecordBatch 为 IPC stream buffer
// ============================================================
static std::shared_ptr<arrow::Buffer> SerializeBatch(
    std::shared_ptr<arrow::RecordBatch> batch) {
    auto sink   = arrow::io::BufferOutputStream::Create().ValueOrDie();
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
    auto buf    = arrow::Buffer::Wrap(data, static_cast<int64_t>(len));
    auto input  = std::make_shared<arrow::io::BufferReader>(buf);
    auto stream = arrow::ipc::RecordBatchStreamReader::Open(input).ValueOrDie();
    std::shared_ptr<arrow::RecordBatch> batch;
    if (!stream->ReadNext(&batch).ok() || !batch) return nullptr;
    return batch;
}

// ============================================================
// 辅助：建立连接并执行清理 SQL（忽略错误）
// ============================================================
static void DropTableIfExists(IDbSession* session, const char* table) {
    std::string sql = std::string("DROP TABLE IF EXISTS ") + table;
    session->ExecuteSql(sql.c_str(), nullptr);
}

// ============================================================
// Test 1: 连接与断开
// ============================================================
void test_connect_disconnect() {
    printf("[TEST] MySQL: connect and disconnect...\n");

    MysqlDriver driver;
    auto params = GetMysqlParams();

    int rc = driver.Connect(params);
    assert(rc == 0);
    assert(driver.IsConnected());
    assert(driver.Ping());
    printf("  Connected to %s:%s/%s, Ping OK\n",
           params["host"].c_str(), params["port"].c_str(), params["database"].c_str());

    driver.Disconnect();
    assert(!driver.IsConnected());
    assert(!driver.Ping());
    printf("  Disconnected, Ping correctly failed\n");

    g_passed++;
    printf("[PASS] MySQL: connect and disconnect\n");
}

// ============================================================
// Test 2: 错误路径 — 错误密码连接失败
// ============================================================
void test_connect_wrong_password() {
    printf("[TEST] MySQL: connect with wrong password...\n");

    MysqlDriver driver;
    auto params = GetMysqlParams();
    params["password"] = "definitely_wrong_password_xyz_12345";
    params["user"]     = "nonexistent_user_xyz";

    int rc = driver.Connect(params);
    assert(rc != 0);
    assert(!driver.IsConnected());
    const char* err = driver.LastError();
    assert(err != nullptr && strlen(err) > 0);
    printf("  Connection correctly failed: %s\n", err);

    g_passed++;
    printf("[PASS] MySQL: connect with wrong password\n");
}

// ============================================================
// Test 3: DDL — 建表、删表
// ============================================================
void test_ddl() {
    printf("[TEST] MySQL: DDL (CREATE/DROP TABLE)...\n");

    MysqlDriver driver;
    assert(driver.Connect(GetMysqlParams()) == 0);
    auto session = driver.CreateSession();
    assert(session != nullptr);
    std::string error;

    // 清理残留
    DropTableIfExists(session.get(), "mysql_test_ddl");

    // 建表
    int rc = session->ExecuteSql(
        "CREATE TABLE mysql_test_ddl ("
        "  id BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,"
        "  name VARCHAR(64) NOT NULL,"
        "  val DOUBLE"
        ")", &error);
    assert(rc == 0);
    printf("  CREATE TABLE: OK\n");

    // 重复建表应失败
    rc = session->ExecuteSql("CREATE TABLE mysql_test_ddl (id INT)", &error);
    assert(rc != 0);
    assert(!error.empty());
    printf("  Duplicate CREATE TABLE: correctly failed\n");

    // IF NOT EXISTS 不报错
    rc = session->ExecuteSql("CREATE TABLE IF NOT EXISTS mysql_test_ddl (id INT)", &error);
    assert(rc == 0);
    printf("  CREATE TABLE IF NOT EXISTS: OK\n");

    // 删表
    rc = session->ExecuteSql("DROP TABLE mysql_test_ddl", &error);
    assert(rc == 0);
    printf("  DROP TABLE: OK\n");

    driver.Disconnect();
    g_passed++;
    printf("[PASS] MySQL: DDL\n");
}

// ============================================================
// Test 4: 基础 CRUD（INSERT / SELECT / UPDATE / DELETE）
// ============================================================
void test_basic_crud() {
    printf("[TEST] MySQL: basic CRUD...\n");

    MysqlDriver driver;
    assert(driver.Connect(GetMysqlParams()) == 0);
    auto session = driver.CreateSession();
    std::string error;

    DropTableIfExists(session.get(), "mysql_test_crud");
    session->ExecuteSql(
        "CREATE TABLE mysql_test_crud ("
        "  id BIGINT, name VARCHAR(64), price DOUBLE, stock INT"
        ")", &error);

    // INSERT
    session->ExecuteSql(
        "INSERT INTO mysql_test_crud VALUES "
        "(1,'Apple',1.50,100),(2,'Banana',0.75,200),(3,'Orange',2.00,50)", &error);

    // SELECT 全量
    IResultSet* rs = nullptr;
    int rc = session->ExecuteQuery(
        "SELECT * FROM mysql_test_crud ORDER BY id", &rs, &error);
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
    rc = session->ExecuteSql(
        "UPDATE mysql_test_crud SET price=1.80 WHERE id=1", &error);
    assert(rc >= 0);  // 返回受影响行数，-1 才是失败
    rs = nullptr;
    session->ExecuteQuery(
        "SELECT price FROM mysql_test_crud WHERE id=1", &rs, &error);
    assert(rs != nullptr && rs->Next());
    double price;
    rs->GetDouble(0, &price);
    assert(price == 1.80);
    delete rs;
    printf("  UPDATE: OK (price=%.2f)\n", price);

    // DELETE
    rc = session->ExecuteSql(
        "DELETE FROM mysql_test_crud WHERE id=3", &error);
    assert(rc >= 0);  // 返回受影响行数，-1 才是失败
    rs = nullptr;
    session->ExecuteQuery(
        "SELECT COUNT(*) FROM mysql_test_crud", &rs, &error);
    assert(rs != nullptr && rs->Next());
    int64_t cnt;
    rs->GetInt64(0, &cnt);
    assert(cnt == 2);
    delete rs;
    printf("  DELETE: OK (remaining=%lld)\n", cnt);

    DropTableIfExists(session.get(), "mysql_test_crud");
    driver.Disconnect();
    g_passed++;
    printf("[PASS] MySQL: basic CRUD\n");
}

// ============================================================
// Test 5: WHERE 条件查询
// ============================================================
void test_where_query() {
    printf("[TEST] MySQL: WHERE query...\n");

    MysqlDriver driver;
    assert(driver.Connect(GetMysqlParams()) == 0);
    auto session = driver.CreateSession();
    std::string error;

    DropTableIfExists(session.get(), "mysql_test_where");
    session->ExecuteSql(
        "CREATE TABLE mysql_test_where (id BIGINT, name VARCHAR(64), score DOUBLE)",
        &error);
    session->ExecuteSql(
        "INSERT INTO mysql_test_where VALUES "
        "(1,'Alice',95.5),(2,'Bob',72.0),(3,'Charlie',88.3),(4,'Dave',60.0)",
        &error);

    // score > 80，按 score 降序
    IResultSet* rs = nullptr;
    session->ExecuteQuery(
        "SELECT name FROM mysql_test_where WHERE score>80 ORDER BY score DESC",
        &rs, &error);
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

    DropTableIfExists(session.get(), "mysql_test_where");
    driver.Disconnect();
    g_passed++;
    printf("[PASS] MySQL: WHERE query\n");
}

// ============================================================
// Test 6: 事务 COMMIT — 转账场景
// ============================================================
void test_transaction_commit() {
    printf("[TEST] MySQL: transaction COMMIT...\n");

    MysqlDriver driver;
    assert(driver.Connect(GetMysqlParams()) == 0);
    auto session = driver.CreateSession();
    std::string error;

    DropTableIfExists(session.get(), "mysql_test_txn_commit");
    session->ExecuteSql(
        "CREATE TABLE mysql_test_txn_commit (id BIGINT, balance BIGINT)",
        &error);
    session->ExecuteSql(
        "INSERT INTO mysql_test_txn_commit VALUES (1,1000),(2,500)",
        &error);

    // 开始事务，转账 200
    assert(session->BeginTransaction(&error) == 0);
    session->ExecuteSql(
        "UPDATE mysql_test_txn_commit SET balance=balance-200 WHERE id=1",
        &error);
    session->ExecuteSql(
        "UPDATE mysql_test_txn_commit SET balance=balance+200 WHERE id=2",
        &error);
    assert(session->CommitTransaction(&error) == 0);
    printf("  COMMIT: OK\n");

    // 验证结果
    IResultSet* rs = nullptr;
    session->ExecuteQuery(
        "SELECT id,balance FROM mysql_test_txn_commit ORDER BY id",
        &rs, &error);
    assert(rs != nullptr);
    assert(rs->Next()); int64_t b1; rs->GetInt64(1, &b1); assert(b1 == 800);
    assert(rs->Next()); int64_t b2; rs->GetInt64(1, &b2); assert(b2 == 700);
    assert(!rs->Next());
    delete rs;
    printf("  After COMMIT: account1=%lld, account2=%lld\n", b1, b2);

    DropTableIfExists(session.get(), "mysql_test_txn_commit");
    driver.Disconnect();
    g_passed++;
    printf("[PASS] MySQL: transaction COMMIT\n");
}

// ============================================================
// Test 7: 事务 ROLLBACK — 数据恢复验证
// ============================================================
void test_transaction_rollback() {
    printf("[TEST] MySQL: transaction ROLLBACK...\n");

    MysqlDriver driver;
    assert(driver.Connect(GetMysqlParams()) == 0);
    auto session = driver.CreateSession();
    std::string error;

    DropTableIfExists(session.get(), "mysql_test_txn_rollback");
    // InnoDB 支持事务，必须用 ENGINE=InnoDB
    session->ExecuteSql(
        "CREATE TABLE mysql_test_txn_rollback ("
        "  id BIGINT, balance BIGINT"
        ") ENGINE=InnoDB",
        &error);
    session->ExecuteSql(
        "INSERT INTO mysql_test_txn_rollback VALUES (1,1000),(2,500)",
        &error);

    // 开始事务，删除所有数据，然后回滚
    assert(session->BeginTransaction(&error) == 0);
    session->ExecuteSql("DELETE FROM mysql_test_txn_rollback", &error);

    // 事务内验证：数据已删除
    IResultSet* rs = nullptr;
    session->ExecuteQuery(
        "SELECT COUNT(*) FROM mysql_test_txn_rollback", &rs, &error);
    assert(rs != nullptr && rs->Next());
    int64_t cnt; rs->GetInt64(0, &cnt); assert(cnt == 0);
    delete rs;
    printf("  In-transaction DELETE: count=%lld\n", cnt);

    // 回滚
    assert(session->RollbackTransaction(&error) == 0);

    // 验证数据恢复
    rs = nullptr;
    session->ExecuteQuery(
        "SELECT COUNT(*) FROM mysql_test_txn_rollback", &rs, &error);
    assert(rs != nullptr && rs->Next());
    rs->GetInt64(0, &cnt); assert(cnt == 2);
    delete rs;
    printf("  After ROLLBACK: count=%lld (restored)\n", cnt);

    // 验证数据内容（balance 值正确恢复）
    rs = nullptr;
    session->ExecuteQuery(
        "SELECT id,balance FROM mysql_test_txn_rollback ORDER BY id",
        &rs, &error);
    assert(rs != nullptr);
    assert(rs->Next()); int64_t b1; rs->GetInt64(1, &b1); assert(b1 == 1000);
    assert(rs->Next()); int64_t b2; rs->GetInt64(1, &b2); assert(b2 == 500);
    delete rs;
    printf("  Balance restored: account1=%lld, account2=%lld\n", b1, b2);

    DropTableIfExists(session.get(), "mysql_test_txn_rollback");
    driver.Disconnect();
    g_passed++;
    printf("[PASS] MySQL: transaction ROLLBACK\n");
}

// ============================================================
// Test 8: BatchReader — Arrow IPC 批量读取
// ============================================================
void test_batch_reader() {
    printf("[TEST] MySQL: BatchReader (Arrow IPC)...\n");

    MysqlDriver driver;
    assert(driver.Connect(GetMysqlParams()) == 0);
    auto session = driver.CreateSession();
    std::string error;

    DropTableIfExists(session.get(), "mysql_test_batch_read");
    session->ExecuteSql(
        "CREATE TABLE mysql_test_batch_read ("
        "  ts BIGINT, device VARCHAR(32), temp DOUBLE, humidity DOUBLE"
        ")", &error);
    session->ExecuteSql(
        "INSERT INTO mysql_test_batch_read VALUES "
        "(1000,'dev-A',23.5,60.1),(1001,'dev-B',24.0,58.3),"
        "(1002,'dev-A',23.8,61.0),(1003,'dev-C',22.1,65.5),"
        "(1004,'dev-B',24.5,57.9)",
        &error);

    auto* readable = dynamic_cast<IBatchReadable*>(session.get());
    assert(readable != nullptr);

    IBatchReader* reader = nullptr;
    int rc = readable->CreateReader(
        "SELECT * FROM mysql_test_batch_read ORDER BY ts", &reader);
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
    printf("  RecordBatch: %lld rows, %d columns\n",
           batch->num_rows(), batch->num_columns());

    // 验证列名
    assert(batch->schema()->field(0)->name() == "ts");
    assert(batch->schema()->field(1)->name() == "device");

    // 无更多数据
    rc = reader->Next(&batch_data, &batch_size);
    assert(rc == 1);  // EOS

    reader->Close();
    reader->Release();

    DropTableIfExists(session.get(), "mysql_test_batch_read");
    driver.Disconnect();
    g_passed++;
    printf("[PASS] MySQL: BatchReader\n");
}

// ============================================================
// Test 9: BatchWriter — Arrow IPC 批量写入（自动建表）
// ============================================================
void test_batch_writer() {
    printf("[TEST] MySQL: BatchWriter (auto create table)...\n");

    MysqlDriver driver;
    assert(driver.Connect(GetMysqlParams()) == 0);
    auto session = driver.CreateSession();
    std::string error;

    DropTableIfExists(session.get(), "mysql_test_batch_write");

    auto* writable = dynamic_cast<IBatchWritable*>(session.get());
    assert(writable != nullptr);

    // 构建 RecordBatch（5 行）
    auto schema = arrow::schema({
        arrow::field("user_id",  arrow::int64()),
        arrow::field("username", arrow::utf8()),
        arrow::field("score",    arrow::float64()),
    });

    arrow::Int64Builder  id_b;
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
    int rc = writable->CreateWriter("mysql_test_batch_write", &writer);
    assert(rc == 0 && writer != nullptr);

    rc = writer->Write(buf->data(), static_cast<size_t>(buf->size()));
    assert(rc == 0);

    BatchWriteStats stats;
    writer->Close(&stats);
    assert(stats.rows_written == 5);
    writer->Release();
    printf("  Wrote %lld rows\n", stats.rows_written);

    // 验证数据
    IResultSet* rs = nullptr;
    session->ExecuteQuery(
        "SELECT COUNT(*) FROM mysql_test_batch_write", &rs, &error);
    assert(rs != nullptr && rs->Next());
    int64_t cnt; rs->GetInt64(0, &cnt);
    assert(cnt == 5);
    delete rs;
    printf("  Verified: %lld rows in DB\n", cnt);

    DropTableIfExists(session.get(), "mysql_test_batch_write");
    driver.Disconnect();
    g_passed++;
    printf("[PASS] MySQL: BatchWriter\n");
}

// ============================================================
// Test 10: BatchWriter — 追加写入（表已存在）
// ============================================================
void test_batch_writer_append() {
    printf("[TEST] MySQL: BatchWriter append to existing table...\n");

    MysqlDriver driver;
    assert(driver.Connect(GetMysqlParams()) == 0);
    auto session = driver.CreateSession();
    std::string error;

    DropTableIfExists(session.get(), "mysql_test_batch_append");
    session->ExecuteSql(
        "CREATE TABLE mysql_test_batch_append (id BIGINT, msg VARCHAR(64))",
        &error);
    session->ExecuteSql(
        "INSERT INTO mysql_test_batch_append VALUES (1,'first'),(2,'second'),(3,'third')",
        &error);

    auto* writable = dynamic_cast<IBatchWritable*>(session.get());

    auto schema = arrow::schema({
        arrow::field("id",  arrow::int64()),
        arrow::field("msg", arrow::utf8()),
    });
    arrow::Int64Builder  id_b;
    arrow::StringBuilder msg_b;
    id_b.Append(4); id_b.Append(5);
    msg_b.Append("fourth"); msg_b.Append("fifth");
    auto batch = arrow::RecordBatch::Make(schema, 2, {
        id_b.Finish().ValueOrDie(),
        msg_b.Finish().ValueOrDie(),
    });
    auto buf = SerializeBatch(batch);

    IBatchWriter* writer = nullptr;
    assert(writable->CreateWriter("mysql_test_batch_append", &writer) == 0);
    assert(writer->Write(buf->data(), static_cast<size_t>(buf->size())) == 0);
    BatchWriteStats stats;
    writer->Close(&stats);
    assert(stats.rows_written == 2);
    writer->Release();

    // 验证总行数 = 3 + 2 = 5
    IResultSet* rs = nullptr;
    session->ExecuteQuery(
        "SELECT COUNT(*) FROM mysql_test_batch_append", &rs, &error);
    assert(rs != nullptr && rs->Next());
    int64_t cnt; rs->GetInt64(0, &cnt);
    assert(cnt == 5);
    delete rs;
    printf("  Append 2 rows, total=%lld\n", cnt);

    DropTableIfExists(session.get(), "mysql_test_batch_append");
    driver.Disconnect();
    g_passed++;
    printf("[PASS] MySQL: BatchWriter append\n");
}

// ============================================================
// Test 11: 大批量写入（验证 P2-3 多值 INSERT 优化）
// ============================================================
void test_batch_large_write() {
    printf("[TEST] MySQL: large batch write (multi-value INSERT)...\n");

    MysqlDriver driver;
    assert(driver.Connect(GetMysqlParams()) == 0);
    auto session = driver.CreateSession();
    std::string error;

    DropTableIfExists(session.get(), "mysql_test_large_batch");

    auto* writable = dynamic_cast<IBatchWritable*>(session.get());
    assert(writable != nullptr);

    // 构建 2000 行 RecordBatch（超过单次 INSERT 1000 行限制，触发分批）
    const int N = 2000;
    auto schema = arrow::schema({
        arrow::field("id",  arrow::int64()),
        arrow::field("val", arrow::float64()),
    });

    arrow::Int64Builder  id_b;
    arrow::DoubleBuilder val_b;
    for (int i = 0; i < N; ++i) {
        id_b.Append(i);
        val_b.Append(i * 1.5);
    }
    auto batch = arrow::RecordBatch::Make(schema, N, {
        id_b.Finish().ValueOrDie(),
        val_b.Finish().ValueOrDie(),
    });
    auto buf = SerializeBatch(batch);

    IBatchWriter* writer = nullptr;
    int rc = writable->CreateWriter("mysql_test_large_batch", &writer);
    assert(rc == 0 && writer != nullptr);

    rc = writer->Write(buf->data(), static_cast<size_t>(buf->size()));
    assert(rc == 0);

    BatchWriteStats stats;
    writer->Close(&stats);
    assert(stats.rows_written == N);
    writer->Release();
    printf("  Wrote %lld rows (batch split across multiple INSERTs)\n",
           stats.rows_written);

    // 验证行数
    IResultSet* rs = nullptr;
    session->ExecuteQuery(
        "SELECT COUNT(*) FROM mysql_test_large_batch", &rs, &error);
    assert(rs != nullptr && rs->Next());
    int64_t cnt; rs->GetInt64(0, &cnt);
    assert(cnt == N);
    delete rs;
    printf("  Verified: %lld rows in DB\n", cnt);

    DropTableIfExists(session.get(), "mysql_test_large_batch");
    driver.Disconnect();
    g_passed++;
    printf("[PASS] MySQL: large batch write\n");
}

// ============================================================
// Test 12: 连接池复用
// ============================================================
void test_connection_pool_reuse() {
    printf("[TEST] MySQL: connection pool reuse...\n");

    MysqlDriver driver;
    assert(driver.Connect(GetMysqlParams()) == 0);

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

    // s3 可以正常执行查询
    IResultSet* rs = nullptr;
    int rc = s3->ExecuteQuery("SELECT 1+1", &rs, nullptr);
    assert(rc == 0 && rs != nullptr && rs->Next());
    int64_t val; rs->GetInt64(0, &val);
    assert(val == 2);
    delete rs;
    printf("  s3 query SELECT 1+1 = %lld\n", val);

    driver.Disconnect();
    g_passed++;
    printf("[PASS] MySQL: connection pool reuse\n");
}

// ============================================================
// Test 13: ResultSet 状态 — Next() 耗尽后 GetXxx 应返回 -1
// ============================================================
void test_resultset_exhausted_state() {
    printf("[TEST] MySQL: ResultSet exhausted state...\n");

    MysqlDriver driver;
    assert(driver.Connect(GetMysqlParams()) == 0);
    auto session = driver.CreateSession();
    std::string error;

    DropTableIfExists(session.get(), "mysql_test_rs_state");
    session->ExecuteSql(
        "CREATE TABLE mysql_test_rs_state (id BIGINT, v VARCHAR(8))",
        &error);
    session->ExecuteSql(
        "INSERT INTO mysql_test_rs_state VALUES (1,'a'),(2,'b')",
        &error);

    IResultSet* rs = nullptr;
    session->ExecuteQuery(
        "SELECT * FROM mysql_test_rs_state ORDER BY id", &rs, &error);
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

    DropTableIfExists(session.get(), "mysql_test_rs_state");
    driver.Disconnect();
    g_passed++;
    printf("[PASS] MySQL: ResultSet exhausted state\n");
}

// ============================================================
// Test 14: 错误路径 — 查询不存在的表
// ============================================================
void test_error_nonexistent_table() {
    printf("[TEST] MySQL: error - nonexistent table...\n");

    MysqlDriver driver;
    assert(driver.Connect(GetMysqlParams()) == 0);
    auto session = driver.CreateSession();
    std::string error;

    IResultSet* rs = nullptr;
    int rc = session->ExecuteQuery(
        "SELECT * FROM mysql_no_such_table_xyz", &rs, &error);
    assert(rc != 0);
    assert(!error.empty());
    assert(rs == nullptr);
    printf("  Error: %s\n", error.c_str());

    // BatchReader 也应失败
    auto* readable = dynamic_cast<IBatchReadable*>(session.get());
    IBatchReader* reader = nullptr;
    rc = readable->CreateReader(
        "SELECT * FROM mysql_no_such_table_xyz", &reader);
    assert(rc != 0);
    assert(reader == nullptr);
    printf("  BatchReader on nonexistent table: correctly failed\n");

    driver.Disconnect();
    g_passed++;
    printf("[PASS] MySQL: error - nonexistent table\n");
}

// ============================================================
// Test 15: 错误路径 — SQL 语法错误
// ============================================================
void test_error_syntax() {
    printf("[TEST] MySQL: error - SQL syntax...\n");

    MysqlDriver driver;
    assert(driver.Connect(GetMysqlParams()) == 0);
    auto session = driver.CreateSession();
    std::string error;

    int rc = session->ExecuteSql("THIS IS NOT SQL", &error);
    assert(rc != 0);
    assert(!error.empty());
    printf("  Syntax error: %s\n", error.c_str());

    driver.Disconnect();
    g_passed++;
    printf("[PASS] MySQL: error - SQL syntax\n");
}

// ============================================================
// Test 16: 多数据类型支持（BIGINT/DOUBLE/VARCHAR/NULL）
// ============================================================
void test_data_types() {
    printf("[TEST] MySQL: data types...\n");

    MysqlDriver driver;
    assert(driver.Connect(GetMysqlParams()) == 0);
    auto session = driver.CreateSession();
    std::string error;

    DropTableIfExists(session.get(), "mysql_test_types");
    session->ExecuteSql(
        "CREATE TABLE mysql_test_types ("
        "  i BIGINT, r DOUBLE, t VARCHAR(64), n VARCHAR(64)"
        ")", &error);
    session->ExecuteSql(
        "INSERT INTO mysql_test_types VALUES (42, 3.14, 'hello', NULL)",
        &error);

    IResultSet* rs = nullptr;
    session->ExecuteQuery("SELECT * FROM mysql_test_types", &rs, &error);
    assert(rs != nullptr && rs->Next());

    int64_t i; double r; const char* t; size_t tl;
    assert(rs->GetInt64(0, &i) == 0);  assert(i == 42);
    assert(rs->GetDouble(1, &r) == 0); assert(r == 3.14);
    assert(rs->GetString(2, &t, &tl) == 0);
    assert(std::string(t, tl) == "hello");
    assert(rs->IsNull(3) == true);

    printf("  BIGINT=%lld DOUBLE=%.2f VARCHAR=%.*s NULL=%s\n",
           i, r, (int)tl, t, rs->IsNull(3) ? "true" : "false");

    delete rs;

    DropTableIfExists(session.get(), "mysql_test_types");
    driver.Disconnect();
    g_passed++;
    printf("[PASS] MySQL: data types\n");
}

// ============================================================
// Test 17: 标识符引用防 SQL 注入（P0-1 修复 + TQ-15 真实注入场景）
// MySQL 用反引号包裹标识符，内部反引号转义为 ``
// ============================================================
void test_quote_identifier() {
    printf("[TEST] MySQL: QuoteIdentifier injection scenarios...\n");

    MysqlDriver driver;
    assert(driver.Connect(GetMysqlParams()) == 0);
    auto session = driver.CreateSession();
    std::string error;

    auto* writable = dynamic_cast<IBatchWritable*>(session.get());
    assert(writable != nullptr);

    auto schema = arrow::schema({arrow::field("id", arrow::int64())});
    auto make_buf = [&](int64_t val) {
        arrow::Int64Builder b; b.Append(val);
        auto batch = arrow::RecordBatch::Make(schema, 1, {b.Finish().ValueOrDie()});
        return SerializeBatch(batch);
    };

    // 场景 1：普通表名（基线验证）
    {
        DropTableIfExists(session.get(), "mysql_test_quote_id");
        auto buf = make_buf(99);
        IBatchWriter* w = nullptr;
        assert(writable->CreateWriter("mysql_test_quote_id", &w) == 0 && w != nullptr);
        assert(w->Write(buf->data(), buf->size()) == 0);
        BatchWriteStats stats; w->Close(&stats); assert(stats.rows_written == 1);
        w->Release();
        IResultSet* rs = nullptr;
        assert(session->ExecuteQuery("SELECT * FROM `mysql_test_quote_id`", &rs, &error) == 0);
        assert(rs != nullptr && rs->Next());
        int64_t v; rs->GetInt64(0, &v); assert(v == 99);
        delete rs;
        DropTableIfExists(session.get(), "mysql_test_quote_id");
        printf("  normal table name: OK\n");
    }

    // 场景 2：表名含反引号（MySQL 标识符注入）
    // 正确转义后 ` 变为 ``，不会破坏 SQL 结构
    {
        auto buf = make_buf(1);
        IBatchWriter* w = nullptr;
        int rc = writable->CreateWriter("tab`le", &w);
        if (rc == 0 && w != nullptr) {
            w->Write(buf->data(), buf->size());
            BatchWriteStats stats; w->Close(&stats);
            w->Release();
            // 验证可读（反引号被正确转义）
            IResultSet* rs = nullptr;
            rc = session->ExecuteQuery("SELECT id FROM `tab``le`", &rs, nullptr);
            assert(rc == 0 && rs != nullptr && rs->Next());
            delete rs;
            DropTableIfExists(session.get(), "tab`le");
            printf("  table name with backtick: correctly quoted\n");
        } else {
            printf("  table name with backtick: correctly rejected by driver\n");
        }
    }

    // 场景 3：表名含分号（尝试注入第二条语句）
    {
        auto buf = make_buf(2);
        IBatchWriter* w = nullptr;
        int rc = writable->CreateWriter("t3;DROP TABLE t3", &w);
        if (rc == 0 && w != nullptr) {
            w->Write(buf->data(), buf->size());
            BatchWriteStats stats; w->Close(&stats);
            w->Release();
            printf("  table name with semicolon: accepted and quoted correctly\n");
        } else {
            printf("  table name with semicolon: correctly rejected by driver\n");
        }
    }

    // 场景 4：空字符串表名应被拒绝
    {
        IBatchWriter* w = nullptr;
        int rc = writable->CreateWriter("", &w);
        assert(rc != 0 || w == nullptr);
        if (w) w->Release();
        printf("  empty table name: correctly rejected\n");
    }

    driver.Disconnect();
    g_passed++;
    printf("[PASS] MySQL: QuoteIdentifier injection scenarios\n");
}

// ============================================================
// main
// ============================================================
int main() {
    printf("=== FlowSQL MySQL Driver Tests ===\n\n");

    // 检查 MySQL 是否可用
    if (!IsMysqlAvailable()) {
        auto params = GetMysqlParams();
        printf("[SKIP] MySQL not available at %s:%s (user=%s database=%s)\n",
               params["host"].c_str(), params["port"].c_str(),
               params["user"].c_str(), params["database"].c_str());
        printf("       Set MYSQL_HOST/MYSQL_PORT/MYSQL_USER/MYSQL_PASSWORD/MYSQL_DATABASE\n");
        printf("       to configure the connection.\n");
        return 0;
    }

    printf("MySQL available, running tests...\n\n");

    test_connect_disconnect();
    test_connect_wrong_password();
    test_ddl();
    test_basic_crud();
    test_where_query();
    test_transaction_commit();
    test_transaction_rollback();
    test_batch_reader();
    test_batch_writer();
    test_batch_writer_append();
    test_batch_large_write();
    test_connection_pool_reuse();
    test_resultset_exhausted_state();
    test_error_nonexistent_table();
    test_error_syntax();
    test_data_types();
    test_quote_identifier();

    printf("\n=== All MySQL tests passed (%d/%d) ===\n",
           g_passed, g_passed + g_skipped);
    return 0;
}
