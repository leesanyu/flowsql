// test_postgres.cpp — PostgreSQL 驱动独立测试套件
//
// 环境变量：
//   PG_HOST     (默认 127.0.0.1)
//   PG_PORT     (默认 5432)
//   PG_USER     (默认 flowsql_user)
//   PG_PASSWORD (默认 flowSQL@postgres)
//   PG_DATABASE (默认 flowsql_db)
//
#include <cassert>
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

#include <services/database/capability_interfaces.h>
#include <services/database/drivers/postgres_driver.h>

using namespace flowsql;
using namespace flowsql::database;

static int g_passed = 0;
static int g_skipped = 0;

static std::unordered_map<std::string, std::string> GetPostgresParams() {
    std::unordered_map<std::string, std::string> p;
    p["host"] = getenv("PG_HOST") ? getenv("PG_HOST") : "127.0.0.1";
    p["port"] = getenv("PG_PORT") ? getenv("PG_PORT") : "5432";
    p["user"] = getenv("PG_USER") ? getenv("PG_USER") : "flowsql_user";
    p["password"] = getenv("PG_PASSWORD") ? getenv("PG_PASSWORD") : "flowSQL@postgres";
    p["database"] = getenv("PG_DATABASE") ? getenv("PG_DATABASE") : "flowsql_db";
    p["timeout"] = "5";
    return p;
}

static std::string UniqueName(const char* prefix) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return std::string(prefix) + "_" + std::to_string(ms);
}

static bool IsPostgresAvailable() {
    PostgresDriver driver;
    auto params = GetPostgresParams();
    if (driver.Connect(params) != 0) return false;
    const bool ok = driver.Ping();
    driver.Disconnect();
    return ok;
}

static void DropTableIfExists(IDbSession* session, const std::string& table) {
    session->ExecuteSql(("DROP TABLE IF EXISTS " + table).c_str());
}

static std::shared_ptr<arrow::Buffer> SerializeBatch(const std::shared_ptr<arrow::RecordBatch>& batch) {
    auto sink = arrow::io::BufferOutputStream::Create().ValueOrDie();
    auto writer = arrow::ipc::MakeStreamWriter(sink, batch->schema()).ValueOrDie();
    (void)writer->WriteRecordBatch(*batch);
    (void)writer->Close();
    return sink->Finish().ValueOrDie();
}

static std::shared_ptr<arrow::RecordBatch> DeserializeFirstBatch(const uint8_t* data, size_t len) {
    auto buf = arrow::Buffer::Wrap(data, static_cast<int64_t>(len));
    auto input = std::make_shared<arrow::io::BufferReader>(buf);
    auto stream = arrow::ipc::RecordBatchStreamReader::Open(input).ValueOrDie();
    std::shared_ptr<arrow::RecordBatch> batch;
    if (!stream->ReadNext(&batch).ok() || !batch) return nullptr;
    return batch;
}

static void test_connect_disconnect() {
    printf("[TEST] PostgreSQL: connect and disconnect...\n");
    PostgresDriver driver;
    auto params = GetPostgresParams();

    assert(driver.Connect(params) == 0);
    assert(driver.IsConnected());
    assert(driver.Ping());

    assert(driver.Disconnect() == 0);
    assert(!driver.IsConnected());
    assert(!driver.Ping());
    g_passed++;
    printf("[PASS] PostgreSQL: connect and disconnect\n");
}

static void test_connect_wrong_password() {
    printf("[TEST] PostgreSQL: connect with wrong password...\n");
    PostgresDriver driver;
    auto params = GetPostgresParams();
    params["user"] = "nonexistent_user_xyz";
    params["password"] = "definitely_wrong_password_xyz_12345";

    assert(driver.Connect(params) != 0);
    assert(!driver.IsConnected());
    const char* err = driver.LastError();
    assert(err != nullptr && std::strlen(err) > 0);
    g_passed++;
    printf("[PASS] PostgreSQL: connect with wrong password\n");
}

static void test_basic_crud() {
    printf("[TEST] PostgreSQL: basic CRUD...\n");
    PostgresDriver driver;
    assert(driver.Connect(GetPostgresParams()) == 0);
    auto session = driver.CreateSession();
    assert(session != nullptr);

    const std::string table = UniqueName("pg_test_crud");
    DropTableIfExists(session.get(), table);
    assert(session->ExecuteSql(("CREATE TABLE " + table +
                                " (id BIGINT, name TEXT, price DOUBLE PRECISION, stock INTEGER)").c_str()) == 0);
    assert(session->ExecuteSql(("INSERT INTO " + table +
                                " VALUES (1,'Apple',1.50,100),(2,'Banana',0.75,200),(3,'Orange',2.00,50)")
                                   .c_str()) == 3);

    IResultSet* rs = nullptr;
    assert(session->ExecuteQuery(("SELECT COUNT(*) FROM " + table).c_str(), &rs) == 0);
    assert(rs != nullptr && rs->Next());
    int64_t cnt = 0;
    assert(rs->GetInt64(0, &cnt) == 0);
    assert(cnt == 3);
    delete rs;

    assert(session->ExecuteSql(("UPDATE " + table + " SET price=1.80 WHERE id=1").c_str()) == 1);
    assert(session->ExecuteSql(("DELETE FROM " + table + " WHERE id=3").c_str()) == 1);

    rs = nullptr;
    assert(session->ExecuteQuery(("SELECT COUNT(*) FROM " + table).c_str(), &rs) == 0);
    assert(rs != nullptr && rs->Next());
    assert(rs->GetInt64(0, &cnt) == 0);
    assert(cnt == 2);
    delete rs;

    DropTableIfExists(session.get(), table);
    driver.Disconnect();
    g_passed++;
    printf("[PASS] PostgreSQL: basic CRUD\n");
}

static void test_transaction_commit_and_rollback() {
    printf("[TEST] PostgreSQL: transaction COMMIT/ROLLBACK...\n");
    PostgresDriver driver;
    assert(driver.Connect(GetPostgresParams()) == 0);
    auto session = driver.CreateSession();
    assert(session != nullptr);

    const std::string table = UniqueName("pg_test_txn");
    DropTableIfExists(session.get(), table);
    assert(session->ExecuteSql(("CREATE TABLE " + table + " (id BIGINT, balance BIGINT)").c_str()) == 0);
    assert(session->ExecuteSql(("INSERT INTO " + table + " VALUES (1,1000),(2,500)").c_str()) == 2);

    assert(session->BeginTransaction() == 0);
    assert(session->ExecuteSql(("UPDATE " + table + " SET balance=balance-200 WHERE id=1").c_str()) == 1);
    assert(session->ExecuteSql(("UPDATE " + table + " SET balance=balance+200 WHERE id=2").c_str()) == 1);
    assert(session->CommitTransaction() == 0);

    IResultSet* rs = nullptr;
    assert(session->ExecuteQuery(("SELECT balance FROM " + table + " WHERE id=1").c_str(), &rs) == 0);
    assert(rs != nullptr && rs->Next());
    int64_t b1 = 0;
    assert(rs->GetInt64(0, &b1) == 0);
    assert(b1 == 800);
    delete rs;

    assert(session->BeginTransaction() == 0);
    assert(session->ExecuteSql(("UPDATE " + table + " SET balance=balance-300 WHERE id=1").c_str()) == 1);
    assert(session->RollbackTransaction() == 0);

    rs = nullptr;
    assert(session->ExecuteQuery(("SELECT balance FROM " + table + " WHERE id=1").c_str(), &rs) == 0);
    assert(rs != nullptr && rs->Next());
    assert(rs->GetInt64(0, &b1) == 0);
    assert(b1 == 800);
    delete rs;

    DropTableIfExists(session.get(), table);
    driver.Disconnect();
    g_passed++;
    printf("[PASS] PostgreSQL: transaction COMMIT/ROLLBACK\n");
}

static void test_reader_writer_type_matrix() {
    printf("[TEST] PostgreSQL: CreateReader/CreateWriter type matrix...\n");
    PostgresDriver driver;
    assert(driver.Connect(GetPostgresParams()) == 0);
    auto session = driver.CreateSession();
    assert(session != nullptr);

    const std::string table = UniqueName("pg_test_types");
    DropTableIfExists(session.get(), table);

    auto* writable = dynamic_cast<IBatchWritable*>(session.get());
    assert(writable != nullptr);
    IBatchWriter* writer = nullptr;
    assert(writable->CreateWriter(table.c_str(), &writer) == 0);
    assert(writer != nullptr);

    auto schema = arrow::schema({
        arrow::field("c_i32", arrow::int32()),
        arrow::field("c_i64", arrow::int64()),
        arrow::field("c_f32", arrow::float32()),
        arrow::field("c_f64", arrow::float64()),
        arrow::field("c_str", arrow::utf8()),
        arrow::field("c_bool", arrow::boolean()),
        arrow::field("c_bin", arrow::binary()),
    });

    arrow::Int32Builder b_i32;
    arrow::Int64Builder b_i64;
    arrow::FloatBuilder b_f32;
    arrow::DoubleBuilder b_f64;
    arrow::StringBuilder b_str;
    arrow::BooleanBuilder b_bool;
    arrow::BinaryBuilder b_bin;

    (void)b_i32.AppendValues({1, 2, 3});
    (void)b_i64.AppendValues({100LL, 200LL, 300LL});
    (void)b_f32.AppendValues({1.5f, 2.5f, 3.5f});
    (void)b_f64.AppendValues({10.1, 20.2, 30.3});
    (void)b_str.AppendValues({"a", "b", "c"});
    (void)b_bool.AppendValues(std::vector<bool>{true, false, true});
    (void)b_bin.Append(reinterpret_cast<const uint8_t*>("A"), 1);
    (void)b_bin.Append(reinterpret_cast<const uint8_t*>("BC"), 2);
    const uint8_t bytes3[] = {0x01, 0x00, 0x02};
    (void)b_bin.Append(bytes3, sizeof(bytes3));

    auto batch = arrow::RecordBatch::Make(schema, 3, {
        b_i32.Finish().ValueOrDie(),
        b_i64.Finish().ValueOrDie(),
        b_f32.Finish().ValueOrDie(),
        b_f64.Finish().ValueOrDie(),
        b_str.Finish().ValueOrDie(),
        b_bool.Finish().ValueOrDie(),
        b_bin.Finish().ValueOrDie(),
    });

    auto ipc = SerializeBatch(batch);
    assert(writer->Write(ipc->data(), static_cast<size_t>(ipc->size())) == 0);
    BatchWriteStats stats;
    writer->Close(&stats);
    writer->Release();
    assert(stats.rows_written == 3);

    auto* readable = dynamic_cast<IBatchReadable*>(session.get());
    assert(readable != nullptr);
    IBatchReader* reader = nullptr;
    assert(readable->CreateReader(("SELECT * FROM " + table + " ORDER BY c_i32").c_str(), &reader) == 0);
    assert(reader != nullptr);

    const uint8_t* data = nullptr;
    size_t len = 0;
    assert(reader->Next(&data, &len) == 0);
    auto got = DeserializeFirstBatch(data, len);
    assert(got != nullptr);
    assert(got->num_rows() == 3);
    assert(got->num_columns() == 7);
    assert(got->schema()->field(5)->type()->id() == arrow::Type::BOOL);
    assert(got->schema()->field(6)->type()->id() == arrow::Type::BINARY);

    auto bool_arr = std::static_pointer_cast<arrow::BooleanArray>(got->column(5));
    assert(bool_arr->Value(0) == true);
    assert(bool_arr->Value(1) == false);
    assert(bool_arr->Value(2) == true);

    auto bin_arr = std::static_pointer_cast<arrow::BinaryArray>(got->column(6));
    auto v0 = bin_arr->GetView(0);
    auto v1 = bin_arr->GetView(1);
    auto v2 = bin_arr->GetView(2);
    assert(v0.size() == 1 && v0.data()[0] == 'A');
    assert(v1.size() == 2 && v1.data()[0] == 'B' && v1.data()[1] == 'C');
    assert(v2.size() == 3 &&
           static_cast<unsigned char>(v2.data()[0]) == 0x01 &&
           static_cast<unsigned char>(v2.data()[1]) == 0x00 &&
           static_cast<unsigned char>(v2.data()[2]) == 0x02);

    reader->Close();
    reader->Release();

    DropTableIfExists(session.get(), table);
    driver.Disconnect();
    g_passed++;
    printf("[PASS] PostgreSQL: CreateReader/CreateWriter type matrix\n");
}

static void test_concurrent_readers() {
    printf("[TEST] PostgreSQL: concurrent readers...\n");
    PostgresDriver driver;
    assert(driver.Connect(GetPostgresParams()) == 0);
    auto session = driver.CreateSession();
    assert(session != nullptr);

    const std::string table = UniqueName("pg_test_mt_r");
    DropTableIfExists(session.get(), table);
    assert(session->ExecuteSql(("CREATE TABLE " + table + " (id INTEGER, name TEXT)").c_str()) == 0);
    assert(session->ExecuteSql(("INSERT INTO " + table + " SELECT generate_series(1,100), 'x'").c_str()) == 100);

    std::atomic<int> failures{0};
    std::vector<std::thread> workers;
    for (int i = 0; i < 8; ++i) {
        workers.emplace_back([&]() {
            auto s = driver.CreateSession();
            if (!s) {
                failures.fetch_add(1);
                return;
            }
            IResultSet* rs = nullptr;
            if (s->ExecuteQuery(("SELECT COUNT(*) FROM " + table).c_str(), &rs) != 0 || !rs) {
                failures.fetch_add(1);
                return;
            }
            int64_t cnt = 0;
            if (!rs->Next() || rs->GetInt64(0, &cnt) != 0 || cnt != 100) {
                failures.fetch_add(1);
            }
            delete rs;
        });
    }
    for (auto& t : workers) t.join();

    assert(failures.load() == 0);
    DropTableIfExists(session.get(), table);
    driver.Disconnect();
    g_passed++;
    printf("[PASS] PostgreSQL: concurrent readers\n");
}

static void test_concurrent_writers() {
    printf("[TEST] PostgreSQL: concurrent writers...\n");
    PostgresDriver driver;
    assert(driver.Connect(GetPostgresParams()) == 0);

    std::atomic<int> failures{0};
    std::mutex tables_mu;
    std::vector<std::string> tables;
    std::vector<std::thread> workers;

    for (int i = 0; i < 6; ++i) {
        workers.emplace_back([&, i]() {
            auto s = driver.CreateSession();
            if (!s) {
                failures.fetch_add(1);
                return;
            }
            auto* writable = dynamic_cast<IBatchWritable*>(s.get());
            if (!writable) {
                failures.fetch_add(1);
                return;
            }

            const std::string prefix = "pg_test_mt_w_" + std::to_string(i);
            std::string table = UniqueName(prefix.c_str());
            {
                std::lock_guard<std::mutex> lock(tables_mu);
                tables.push_back(table);
            }
            IBatchWriter* writer = nullptr;
            if (writable->CreateWriter(table.c_str(), &writer) != 0 || !writer) {
                failures.fetch_add(1);
                return;
            }

            arrow::Int64Builder id_b;
            arrow::StringBuilder name_b;
            for (int j = 0; j < 10; ++j) {
                (void)id_b.Append(j);
                (void)name_b.Append("v");
            }
            auto schema = arrow::schema({arrow::field("id", arrow::int64()), arrow::field("name", arrow::utf8())});
            auto batch = arrow::RecordBatch::Make(schema, 10, {id_b.Finish().ValueOrDie(), name_b.Finish().ValueOrDie()});
            auto ipc = SerializeBatch(batch);
            if (writer->Write(ipc->data(), static_cast<size_t>(ipc->size())) != 0) {
                writer->Release();
                failures.fetch_add(1);
                return;
            }
            BatchWriteStats stats;
            writer->Close(&stats);
            writer->Release();
            if (stats.rows_written != 10) {
                failures.fetch_add(1);
                return;
            }

            IResultSet* rs = nullptr;
            if (s->ExecuteQuery(("SELECT COUNT(*) FROM " + table).c_str(), &rs) != 0 || !rs) {
                failures.fetch_add(1);
                return;
            }
            int64_t cnt = 0;
            if (!rs->Next() || rs->GetInt64(0, &cnt) != 0 || cnt != 10) {
                failures.fetch_add(1);
            }
            delete rs;
        });
    }
    for (auto& t : workers) t.join();
    assert(failures.load() == 0);

    auto cleanup = driver.CreateSession();
    assert(cleanup != nullptr);
    for (const auto& t : tables) {
        DropTableIfExists(cleanup.get(), t);
    }
    driver.Disconnect();
    g_passed++;
    printf("[PASS] PostgreSQL: concurrent writers\n");
}

int main() {
    printf("=== FlowSQL PostgreSQL Driver Tests ===\n\n");

    if (!IsPostgresAvailable()) {
        auto p = GetPostgresParams();
        printf("[SKIP] PostgreSQL not available at %s:%s (user=%s database=%s)\n",
               p["host"].c_str(), p["port"].c_str(), p["user"].c_str(), p["database"].c_str());
        printf("       Set PG_HOST/PG_PORT/PG_USER/PG_PASSWORD/PG_DATABASE\n");
        g_skipped++;
        printf("\n=== Summary: passed=%d skipped=%d ===\n", g_passed, g_skipped);
        return 0;
    }

    test_connect_disconnect();
    test_connect_wrong_password();
    test_basic_crud();
    test_transaction_commit_and_rollback();
    test_reader_writer_type_matrix();
    test_concurrent_readers();
    test_concurrent_writers();

    printf("\n=== All PostgreSQL tests passed (%d) ===\n", g_passed);
    return 0;
}
