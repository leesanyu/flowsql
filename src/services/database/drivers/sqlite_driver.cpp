#include "sqlite_driver.h"

#include <arrow/api.h>
#include <arrow/io/memory.h>
#include <arrow/ipc/reader.h>
#include <arrow/ipc/writer.h>

#include <cstdio>
#include <cstring>

namespace flowsql {
namespace database {

// ==================== SqliteResultSet 实现 ====================

SqliteResultSet::SqliteResultSet(sqlite3_stmt* stmt, std::function<void(sqlite3_stmt*)> free_func)
    : stmt_(stmt), free_func_(free_func), has_next_(true), fetched_(false) {}

SqliteResultSet::~SqliteResultSet() {
    if (stmt_ && free_func_) {
        free_func_(stmt_);
    }
}

int SqliteResultSet::FieldCount() {
    return stmt_ ? sqlite3_column_count(stmt_) : 0;
}

const char* SqliteResultSet::FieldName(int index) {
    if (!stmt_) return nullptr;
    return (index >= 0 && index < sqlite3_column_count(stmt_))
           ? sqlite3_column_name(stmt_, index) : nullptr;
}

int SqliteResultSet::FieldType(int index) {
    if (!stmt_) return 0;
    return (index >= 0 && index < sqlite3_column_count(stmt_))
           ? sqlite3_column_type(stmt_, index) : 0;
}

int SqliteResultSet::FieldLength(int index) {
    return 0;
}

bool SqliteResultSet::HasNext() {
    return has_next_;
}

bool SqliteResultSet::Next() {
    if (!stmt_ || !has_next_) return false;

    int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) {
        fetched_ = true;
        return true;
    } else if (rc == SQLITE_DONE) {
        has_next_ = false;
        return false;
    } else {
        has_next_ = false;
        return false;
    }
}

int SqliteResultSet::GetInt(int index, int* value) {
    if (!stmt_ || !fetched_) return -1;
    if (index < 0 || index >= sqlite3_column_count(stmt_)) return -1;
    if (sqlite3_column_type(stmt_, index) == SQLITE_NULL) return -1;
    *value = sqlite3_column_int(stmt_, index);
    return 0;
}

int SqliteResultSet::GetInt64(int index, int64_t* value) {
    if (!stmt_ || !fetched_) return -1;
    if (index < 0 || index >= sqlite3_column_count(stmt_)) return -1;
    if (sqlite3_column_type(stmt_, index) == SQLITE_NULL) return -1;
    *value = sqlite3_column_int64(stmt_, index);
    return 0;
}

int SqliteResultSet::GetDouble(int index, double* value) {
    if (!stmt_ || !fetched_) return -1;
    if (index < 0 || index >= sqlite3_column_count(stmt_)) return -1;
    if (sqlite3_column_type(stmt_, index) == SQLITE_NULL) return -1;
    *value = sqlite3_column_double(stmt_, index);
    return 0;
}

int SqliteResultSet::GetString(int index, const char** val, size_t* len) {
    if (!stmt_ || !fetched_) return -1;
    if (index < 0 || index >= sqlite3_column_count(stmt_)) return -1;
    if (sqlite3_column_type(stmt_, index) == SQLITE_NULL) return -1;
    *val = reinterpret_cast<const char*>(sqlite3_column_text(stmt_, index));
    if (len) *len = sqlite3_column_bytes(stmt_, index);
    return 0;
}

bool SqliteResultSet::IsNull(int index) {
    if (!stmt_ || !fetched_) return true;
    if (index < 0 || index >= sqlite3_column_count(stmt_)) return true;
    return sqlite3_column_type(stmt_, index) == SQLITE_NULL;
}

// ==================== SqliteSession 实现 ====================

SqliteSession::SqliteSession(SqliteDriver* driver, sqlite3* db)
    : RelationDbSessionBase<SqliteTraits>(driver, db) {}

SqliteSession::~SqliteSession() = default;

sqlite3_stmt* SqliteSession::PrepareStatement(sqlite3* db, const char* sql, std::string* error) {
    if (!db) {
        *error = "null connection";
        return nullptr;
    }

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        *error = sqlite3_errmsg(db);
        return nullptr;
    }

    return stmt;
}

int SqliteSession::ExecuteStatement(sqlite3_stmt* stmt, std::string* error) {
    if (!stmt) {
        *error = "null statement";
        return -1;
    }

    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
        *error = sqlite3_errmsg(sqlite3_db_handle(stmt));
        return -1;
    }

    return 0;
}

sqlite3_stmt* SqliteSession::GetResultMetadata(sqlite3_stmt* stmt, std::string* error) {
    (void)error;
    return stmt;
}

std::string SqliteSession::GetDriverError(sqlite3* db) {
    if (!db) return "null connection";
    return sqlite3_errmsg(db);
}

void SqliteSession::FreeResult(sqlite3_stmt* result) {
    if (result) {
        sqlite3_finalize(result);
    }
}

void SqliteSession::FreeStatement(sqlite3_stmt* stmt) {
    if (stmt) {
        sqlite3_finalize(stmt);
    }
}

bool SqliteSession::PingImpl(sqlite3* db) {
    if (!db) return false;
    char* errmsg = nullptr;
    int rc = sqlite3_exec(db, "SELECT 1;", nullptr, nullptr, &errmsg);
    if (errmsg) sqlite3_free(errmsg);
    return rc == SQLITE_OK;
}

void SqliteSession::ReturnConnection(sqlite3* db) {
    auto* sqlite_driver = static_cast<SqliteDriver*>(this->driver_);
    if (sqlite_driver) {
        sqlite_driver->ReturnToPool(db);
    }
}

IResultSet* SqliteSession::CreateResultSet(sqlite3_stmt* stmt,
                                           std::function<void(sqlite3_stmt*)> free_func) {
    return new SqliteResultSet(stmt, free_func);
}

// ==================== SqliteBatchReader 实现 ====================

class SqliteBatchReader : public IBatchReader {
public:
    SqliteBatchReader(std::shared_ptr<IDbSession> session,
                      IResultSet* result,
                      std::shared_ptr<arrow::Schema> schema)
        : session_(session), result_(result), schema_(std::move(schema)) {}

    ~SqliteBatchReader() override = default;

    int GetSchema(const uint8_t** data, size_t* size) override {
        if (schema_buffer_) {
            *data = schema_buffer_->data();
            *size = schema_buffer_->size();
            return 0;
        }

        auto buffer_output_result = arrow::io::BufferOutputStream::Create();
        if (!buffer_output_result.ok()) return -1;
        auto buffer_output = buffer_output_result.ValueOrDie();

        auto writer_result = arrow::ipc::MakeStreamWriter(buffer_output, schema_);
        if (!writer_result.ok()) return -1;
        auto writer = writer_result.ValueOrDie();

        if (!writer->Close().ok()) return -1;
        auto buffer_result = buffer_output->Finish();
        if (!buffer_result.ok()) return -1;
        schema_buffer_ = buffer_result.ValueOrDie();

        *data = schema_buffer_->data();
        *size = schema_buffer_->size();
        return 0;
    }

    int Next(const uint8_t** data, size_t* size) override {
        if (done_) {
            *data = nullptr;
            *size = 0;
            return 1;
        }

        std::vector<std::unique_ptr<arrow::ArrayBuilder>> builders;
        for (const auto& field : schema_->fields()) {
            std::unique_ptr<arrow::ArrayBuilder> builder;
            auto status = arrow::MakeBuilder(arrow::default_memory_pool(), field->type(), &builder);
            if (!status.ok()) return -1;
            builders.push_back(std::move(builder));
        }

        const int batch_size = 1024;
        int row_count = 0;

        for (int i = 0; i < batch_size; ++i) {
            if (!result_->Next()) {
                done_ = true;
                break;
            }

            for (int col = 0; col < schema_->num_fields(); ++col) {
                auto* builder = builders[col].get();
                auto arrow_type = builder->type();

                if (result_->IsNull(col)) {
                    builder->AppendNull();
                    continue;
                }

                if (arrow_type->id() == arrow::Type::INT64) {
                    int64_t value;
                    if (result_->GetInt64(col, &value) == 0)
                        static_cast<arrow::Int64Builder*>(builder)->Append(value);
                    else
                        builder->AppendNull();
                } else if (arrow_type->id() == arrow::Type::DOUBLE) {
                    double value;
                    if (result_->GetDouble(col, &value) == 0)
                        static_cast<arrow::DoubleBuilder*>(builder)->Append(value);
                    else
                        builder->AppendNull();
                } else if (arrow_type->id() == arrow::Type::STRING) {
                    const char* str;
                    size_t len;
                    if (result_->GetString(col, &str, &len) == 0)
                        static_cast<arrow::StringBuilder*>(builder)->Append(str, len);
                    else
                        builder->AppendNull();
                } else if (arrow_type->id() == arrow::Type::BINARY) {
                    const char* str;
                    size_t len;
                    if (result_->GetString(col, &str, &len) == 0)
                        static_cast<arrow::BinaryBuilder*>(builder)->Append(
                            reinterpret_cast<const uint8_t*>(str), len);
                    else
                        builder->AppendNull();
                }
            }
            ++row_count;
        }

        if (row_count == 0) {
            *data = nullptr;
            *size = 0;
            return 1;
        }

        std::vector<std::shared_ptr<arrow::Array>> arrays;
        for (auto& builder : builders) {
            std::shared_ptr<arrow::Array> array;
            if (!builder->Finish(&array).ok()) return -1;
            arrays.push_back(array);
        }

        auto batch = arrow::RecordBatch::Make(schema_, row_count, arrays);

        auto buffer_output_result = arrow::io::BufferOutputStream::Create();
        if (!buffer_output_result.ok()) return -1;
        auto buffer_output = buffer_output_result.ValueOrDie();

        auto writer_result = arrow::ipc::MakeStreamWriter(buffer_output, schema_);
        if (!writer_result.ok()) return -1;
        auto writer = writer_result.ValueOrDie();

        if (!writer->WriteRecordBatch(*batch).ok()) return -1;
        if (!writer->Close().ok()) return -1;
        auto buffer_result = buffer_output->Finish();
        if (!buffer_result.ok()) return -1;
        batch_buffer_ = buffer_result.ValueOrDie();

        *data = batch_buffer_->data();
        *size = batch_buffer_->size();
        return 0;
    }

    void Cancel() override { cancelled_ = true; }
    void Close() override {}
    const char* GetLastError() override { return last_error_.c_str(); }
    void Release() override { delete this; }

private:
    std::shared_ptr<IDbSession> session_;
    IResultSet* result_;
    std::shared_ptr<arrow::Schema> schema_;
    std::shared_ptr<arrow::Buffer> schema_buffer_;
    std::shared_ptr<arrow::Buffer> batch_buffer_;
    std::string last_error_;
    bool done_ = false;
    bool cancelled_ = false;
};

IBatchReader* SqliteSession::CreateBatchReader(IResultSet* result,
                                                std::shared_ptr<arrow::Schema> schema) {
    auto self = std::shared_ptr<IDbSession>(this, [](IDbSession*){});
    return new SqliteBatchReader(self, result, schema);
}

// ==================== SqliteBatchWriter 实现 ====================

class SqliteBatchWriter : public IBatchWriter {
public:
    SqliteBatchWriter(std::shared_ptr<IDbSession> session, const char* table)
        : session_(session), table_(table) {}

    ~SqliteBatchWriter() override {
        if (transaction_started_ && !committed_) {
            std::string error;
            session_->RollbackTransaction(&error);
        }
    }

    int Write(const uint8_t* data, size_t size) override {
        auto buffer = arrow::Buffer::Wrap(data, size);
        auto buffer_reader = std::make_shared<arrow::io::BufferReader>(buffer);
        auto reader_result = arrow::ipc::RecordBatchStreamReader::Open(buffer_reader);
        if (!reader_result.ok()) return -1;
        auto reader = reader_result.ValueOrDie();

        std::shared_ptr<arrow::RecordBatch> batch;
        if (!reader->ReadNext(&batch).ok() || !batch) return -1;

        if (!transaction_started_) {
            std::string error;
            if (session_->BeginTransaction(&error) != 0) {
                last_error_ = "BeginTransaction failed: " + error;
                return -1;
            }
            transaction_started_ = true;
            if (CreateTable(batch->schema()) != 0) return -1;
        }

        if (InsertBatch(batch) != 0) return -1;
        return 0;
    }

    int Flush() override { return 0; }

    void Close(BatchWriteStats* stats) override {
        if (transaction_started_ && !committed_) {
            std::string error;
            if (session_->CommitTransaction(&error) != 0) {
                last_error_ = "CommitTransaction failed: " + error;
            } else {
                committed_ = true;
            }
        }
        if (stats) {
            stats->rows_written = rows_written_;
            stats->bytes_written = bytes_written_;
            stats->elapsed_ms = 0;
        }
    }

    const char* GetLastError() override { return last_error_.c_str(); }
    void Release() override { delete this; }

private:
    int CreateTable(std::shared_ptr<arrow::Schema> schema) {
        std::string ddl = "CREATE TABLE IF NOT EXISTS " + table_ + " (";
        for (int i = 0; i < schema->num_fields(); ++i) {
            if (i > 0) ddl += ", ";
            ddl += schema->field(i)->name() + " ";
            auto type_id = schema->field(i)->type()->id();
            if (type_id == arrow::Type::INT64) ddl += "BIGINT";
            else if (type_id == arrow::Type::DOUBLE) ddl += "REAL";
            else if (type_id == arrow::Type::BINARY) ddl += "BLOB";
            else ddl += "TEXT";
        }
        ddl += ")";
        std::string error;
        if (session_->ExecuteSql(ddl.c_str(), &error) != 0) {
            last_error_ = "CREATE TABLE failed: " + error;
            return -1;
        }
        return 0;
    }

    int InsertBatch(std::shared_ptr<arrow::RecordBatch> batch) {
        for (int64_t row = 0; row < batch->num_rows(); ++row) {
            std::string sql = "INSERT INTO " + table_ + " VALUES (";
            for (int col = 0; col < batch->num_columns(); ++col) {
                if (col > 0) sql += ", ";
                auto array = batch->column(col);
                if (array->IsNull(row)) {
                    sql += "NULL";
                } else if (array->type()->id() == arrow::Type::INT64) {
                    auto int_array = std::static_pointer_cast<arrow::Int64Array>(array);
                    sql += std::to_string(int_array->Value(row));
                } else if (array->type()->id() == arrow::Type::DOUBLE) {
                    auto double_array = std::static_pointer_cast<arrow::DoubleArray>(array);
                    sql += std::to_string(double_array->Value(row));
                } else if (array->type()->id() == arrow::Type::BINARY) {
                    auto blob_array = std::static_pointer_cast<arrow::BinaryArray>(array);
                    auto blob = blob_array->GetView(row);
                    sql += "X'";
                    for (size_t i = 0; i < blob.size(); ++i) {
                        char hex[3];
                        snprintf(hex, sizeof(hex), "%02X", blob.data()[i]);
                        sql += hex;
                    }
                    sql += "'";
                } else {
                    auto string_array = std::static_pointer_cast<arrow::StringArray>(array);
                    std::string value = string_array->GetString(row);
                    sql += "'";
                    for (char c : value) {
                        if (c == '\'') sql += "''";
                        else sql += c;
                    }
                    sql += "'";
                }
            }
            sql += ")";
            std::string error;
            if (session_->ExecuteSql(sql.c_str(), &error) != 0) {
                last_error_ = "INSERT failed: " + error;
                return -1;
            }
            ++rows_written_;
        }
        return 0;
    }

    std::shared_ptr<IDbSession> session_;
    std::string table_;
    std::string last_error_;
    int64_t rows_written_ = 0;
    int64_t bytes_written_ = 0;
    bool transaction_started_ = false;
    bool committed_ = false;
};

IBatchWriter* SqliteSession::CreateBatchWriter(const char* table) {
    auto self = std::shared_ptr<IDbSession>(this, [](IDbSession*){});
    return new SqliteBatchWriter(self, table);
}

// ==================== SqliteDriver 实现 ====================

SqliteDriver::~SqliteDriver() = default;

int SqliteDriver::Connect(const std::unordered_map<std::string, std::string>& params) {
    ConnectionPoolConfig config;
    config.max_connections = 10;
    config.min_connections = 0;
    config.idle_timeout = std::chrono::seconds(300);
    config.health_check_interval = std::chrono::seconds(60);

    auto it = params.find("path");
    db_path_ = (it != params.end()) ? it->second : ":memory:";

    bool readonly = (params.find("readonly") != params.end() &&
                     params.at("readonly") == "true");

    auto factory = [this, readonly](std::string* error) -> sqlite3* {
        sqlite3* db = nullptr;
        int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
        if (readonly) {
            flags = SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX;
        }

        int rc = sqlite3_open_v2(db_path_.c_str(), &db, flags, nullptr);
        if (rc != SQLITE_OK) {
            *error = db ? sqlite3_errmsg(db) : "sqlite3_open_v2 failed";
            if (db) sqlite3_close(db);
            return nullptr;
        }

        sqlite3_exec(db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
        return db;
    };

    auto closer = [](sqlite3* db) { if (db) sqlite3_close(db); };
    auto pinger = [](sqlite3* db) -> bool {
        if (!db) return false;
        char* errmsg = nullptr;
        int rc = sqlite3_exec(db, "SELECT 1;", nullptr, nullptr, &errmsg);
        if (errmsg) sqlite3_free(errmsg);
        return rc == SQLITE_OK;
    };

    pool_ = std::make_unique<ConnectionPool<sqlite3*>>(config, factory, closer, pinger);
    printf("SqliteDriver: initialized connection pool for %s\n", db_path_.c_str());
    return 0;
}

int SqliteDriver::Disconnect() {
    pool_.reset();
    printf("SqliteDriver: disconnected\n");
    return 0;
}

bool SqliteDriver::Ping() {
    return pool_ != nullptr;
}

std::shared_ptr<IDbSession> SqliteDriver::CreateSession() {
    if (!pool_) {
        last_error_ = "Driver not connected";
        return nullptr;
    }

    sqlite3* db = nullptr;
    std::string error;
    if (!pool_->Acquire(&db, &error)) {
        last_error_ = error;
        return nullptr;
    }

    return std::make_shared<SqliteSession>(this, db);
}

void SqliteDriver::ReturnToPool(sqlite3* db) {
    if (pool_ && db) {
        pool_->Return(db);
    }
}

int SqliteDriver::CreateReader(const char* query, IBatchReader** reader) {
    auto session = CreateSession();
    if (!session) return -1;

    auto* batch_readable = dynamic_cast<IBatchReadable*>(session.get());
    if (!batch_readable) return -1;

    return batch_readable->CreateReader(query, reader);
}

int SqliteDriver::CreateWriter(const char* table, IBatchWriter** writer) {
    auto session = CreateSession();
    if (!session) return -1;

    auto* batch_writable = dynamic_cast<IBatchWritable*>(session.get());
    if (!batch_writable) return -1;

    return batch_writable->CreateWriter(table, writer);
}

int SqliteDriver::BeginTransaction(std::string* error) {
    auto session = CreateSession();
    if (!session) { *error = "Failed to create session"; return -1; }
    return session->BeginTransaction(error);
}

int SqliteDriver::CommitTransaction(std::string* error) {
    auto session = CreateSession();
    if (!session) { *error = "Failed to create session"; return -1; }
    return session->CommitTransaction(error);
}

int SqliteDriver::RollbackTransaction(std::string* error) {
    auto session = CreateSession();
    if (!session) { *error = "Failed to create session"; return -1; }
    return session->RollbackTransaction(error);
}

}  // namespace database
}  // namespace flowsql
