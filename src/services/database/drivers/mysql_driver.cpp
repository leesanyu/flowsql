#include "mysql_driver.h"

#include <arrow/api.h>
#include <arrow/io/memory.h>
#include <arrow/ipc/reader.h>
#include <arrow/ipc/writer.h>

#include <cstdio>
#include <cstring>

namespace flowsql {
namespace database {

// ==================== MysqlResultSet 实现 ====================

MysqlResultSet::MysqlResultSet(MYSQL_RES* result, std::function<void(MYSQL_RES*)> free_func)
    : result_(result), free_func_(free_func), has_next_(true), fetched_(false) {}

MysqlResultSet::~MysqlResultSet() {
    if (result_ && free_func_) {
        free_func_(result_);
    }
}

int MysqlResultSet::FieldCount() {
    return result_ ? mysql_num_fields(result_) : 0;
}

const char* MysqlResultSet::FieldName(int index) {
    if (!result_) return nullptr;
    MYSQL_FIELD* fields = mysql_fetch_fields(result_);
    return (index >= 0 && index < mysql_num_fields(result_)) ? fields[index].name : nullptr;
}

int MysqlResultSet::FieldType(int index) {
    if (!result_) return 0;
    MYSQL_FIELD* fields = mysql_fetch_fields(result_);
    return (index >= 0 && index < mysql_num_fields(result_)) ? fields[index].type : 0;
}

int MysqlResultSet::FieldLength(int index) {
    if (!result_) return 0;
    MYSQL_FIELD* fields = mysql_fetch_fields(result_);
    return (index >= 0 && index < mysql_num_fields(result_)) ? fields[index].length : 0;
}

bool MysqlResultSet::HasNext() {
    return has_next_;
}

bool MysqlResultSet::Next() {
    if (!result_ || !has_next_) return false;

    MYSQL_ROW row = mysql_fetch_row(result_);
    if (!row) {
        has_next_ = false;
        return false;
    }

    current_row_ = row;
    fetched_ = true;
    return true;
}

int MysqlResultSet::GetInt(int index, int* value) {
    if (!current_row_ || !fetched_) return -1;
    if (index < 0 || index >= mysql_num_fields(result_)) return -1;
    if (!current_row_[index]) return -1;
    *value = std::atoi(current_row_[index]);
    return 0;
}

int MysqlResultSet::GetInt64(int index, int64_t* value) {
    if (!current_row_ || !fetched_) return -1;
    if (index < 0 || index >= mysql_num_fields(result_)) return -1;
    if (!current_row_[index]) return -1;
    *value = std::atoll(current_row_[index]);
    return 0;
}

int MysqlResultSet::GetDouble(int index, double* value) {
    if (!current_row_ || !fetched_) return -1;
    if (index < 0 || index >= mysql_num_fields(result_)) return -1;
    if (!current_row_[index]) return -1;
    *value = std::atof(current_row_[index]);
    return 0;
}

int MysqlResultSet::GetString(int index, const char** val, size_t* len) {
    if (!current_row_ || !fetched_) return -1;
    if (index < 0 || index >= mysql_num_fields(result_)) return -1;
    if (!current_row_[index]) return -1;
    *val = current_row_[index];
    if (len) {
        unsigned long* lengths = mysql_fetch_lengths(result_);
        *len = lengths ? lengths[index] : strlen(current_row_[index]);
    }
    return 0;
}

bool MysqlResultSet::IsNull(int index) {
    if (!current_row_ || !fetched_) return true;
    if (index < 0 || index >= mysql_num_fields(result_)) return true;
    return current_row_[index] == nullptr;
}

// ==================== MysqlSession 实现 ====================

MysqlSession::MysqlSession(MysqlDriver* driver, MYSQL* conn)
    : RelationDbSessionBase<MysqlTraits>(driver, conn) {}

MysqlSession::~MysqlSession() {
    if (conn_) {
        ReturnConnection(conn_);
        conn_ = nullptr;
    }
}

MYSQL_STMT* MysqlSession::PrepareStatement(MYSQL* conn, const char* sql, std::string* error) {
    if (!conn) {
        *error = "null connection";
        return nullptr;
    }

    MYSQL_STMT* stmt = mysql_stmt_init(conn);
    if (!stmt) {
        *error = "mysql_stmt_init failed: " + std::string(mysql_error(conn));
        return nullptr;
    }

    if (mysql_stmt_prepare(stmt, sql, strlen(sql)) != 0) {
        *error = "mysql_stmt_prepare failed: " + std::string(mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return nullptr;
    }

    return stmt;
}

int MysqlSession::ExecuteStatement(MYSQL_STMT* stmt, std::string* error) {
    if (!stmt) {
        *error = "null statement";
        return -1;
    }

    if (mysql_stmt_execute(stmt) != 0) {
        *error = "mysql_stmt_execute failed: " + std::string(mysql_stmt_error(stmt));
        return -1;
    }

    return 0;
}

MYSQL_RES* MysqlSession::GetResultMetadata(MYSQL_STMT* stmt, std::string* error) {
    (void)error;
    if (!stmt) return nullptr;

    MYSQL_RES* metadata = mysql_stmt_result_metadata(stmt);
    if (!metadata) {
        return nullptr;
    }

    return metadata;
}

int MysqlSession::GetAffectedRows(MYSQL_STMT* stmt) {
    if (!stmt) return 0;
    return static_cast<int>(mysql_stmt_affected_rows(stmt));
}

std::string MysqlSession::GetDriverError(MYSQL* conn) {
    if (!conn) return "null connection";
    return mysql_error(conn);
}

void MysqlSession::FreeResult(MYSQL_RES* result) {
    if (result) {
        mysql_free_result(result);
    }
}

void MysqlSession::FreeStatement(MYSQL_STMT* stmt) {
    if (stmt) {
        mysql_stmt_close(stmt);
    }
}

bool MysqlSession::PingImpl(MYSQL* conn) {
    if (!conn) return false;
    return mysql_ping(conn) == 0;
}

void MysqlSession::ReturnConnection(MYSQL* conn) {
    auto* mysql_driver = static_cast<MysqlDriver*>(this->driver_);
    if (mysql_driver) {
        mysql_driver->ReturnToPool(conn);
    }
}

IResultSet* MysqlSession::CreateResultSet(MYSQL_RES* result,
                                          std::function<void(MYSQL_RES*)> free_func) {
    return new MysqlResultSet(result, free_func);
}

// 使用简单 API 执行查询（替代 prepared statement 路径）
int MysqlSession::ExecuteQuery(const char* sql, IResultSet** result, std::string* error) {
    if (mysql_query(conn_, sql) != 0) {
        if (error) *error = mysql_error(conn_);
        return -1;
    }
    MYSQL_RES* res = mysql_store_result(conn_);
    if (!res) {
        if (error) *error = mysql_error(conn_);
        return -1;
    }
    *result = new MysqlResultSet(res, [](MYSQL_RES* r) { mysql_free_result(r); });
    return 0;
}

int MysqlSession::ExecuteSql(const char* sql, std::string* error) {
    if (mysql_query(conn_, sql) != 0) {
        if (error) *error = mysql_error(conn_);
        return -1;
    }
    return static_cast<int>(mysql_affected_rows(conn_));
}

std::shared_ptr<arrow::Schema> MysqlSession::InferSchema(IResultSet* result, std::string* error) {
    auto* rs = static_cast<MysqlResultSet*>(result);
    MYSQL_RES* mysql_res = rs->GetResult();
    int n = mysql_num_fields(mysql_res);
    MYSQL_FIELD* fields = mysql_fetch_fields(mysql_res);
    std::vector<std::shared_ptr<arrow::Field>> arrow_fields;
    for (int i = 0; i < n; ++i) {
        std::shared_ptr<arrow::DataType> type;
        switch (fields[i].type) {
            case MYSQL_TYPE_TINY:
            case MYSQL_TYPE_SHORT:
            case MYSQL_TYPE_LONG:
            case MYSQL_TYPE_INT24:   type = arrow::int32();   break;
            case MYSQL_TYPE_LONGLONG: type = arrow::int64();  break;
            case MYSQL_TYPE_FLOAT:   type = arrow::float32(); break;
            case MYSQL_TYPE_DOUBLE:
            case MYSQL_TYPE_DECIMAL:
            case MYSQL_TYPE_NEWDECIMAL: type = arrow::float64(); break;
            case MYSQL_TYPE_BLOB:
            case MYSQL_TYPE_TINY_BLOB:
            case MYSQL_TYPE_MEDIUM_BLOB:
            case MYSQL_TYPE_LONG_BLOB: type = arrow::binary(); break;
            default:                 type = arrow::utf8();    break;
        }
        arrow_fields.push_back(arrow::field(fields[i].name, type));
    }
    return arrow::schema(arrow_fields);
}

// ==================== MysqlBatchReader 实现 ====================

class MysqlBatchReader : public IBatchReader {
public:
    MysqlBatchReader(std::shared_ptr<IDbSession> session,
                     IResultSet* result,
                     std::shared_ptr<arrow::Schema> schema)
        : session_(session), result_(result), schema_(std::move(schema)) {}

    ~MysqlBatchReader() override = default;

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

                if (arrow_type->id() == arrow::Type::INT32) {
                    int value;
                    if (result_->GetInt(col, &value) == 0)
                        static_cast<arrow::Int32Builder*>(builder)->Append(value);
                    else
                        builder->AppendNull();
                } else if (arrow_type->id() == arrow::Type::INT64) {
                    int64_t value;
                    if (result_->GetInt64(col, &value) == 0)
                        static_cast<arrow::Int64Builder*>(builder)->Append(value);
                    else
                        builder->AppendNull();
                } else if (arrow_type->id() == arrow::Type::FLOAT) {
                    double value;
                    if (result_->GetDouble(col, &value) == 0)
                        static_cast<arrow::FloatBuilder*>(builder)->Append(static_cast<float>(value));
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
                } else {
                    // 未知类型，填 NULL 保持列长度一致
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

IBatchReader* MysqlSession::CreateBatchReader(IResultSet* result,
                                               std::shared_ptr<arrow::Schema> schema) {
    return new MysqlBatchReader(shared_from_this(), result, schema);
}

// ==================== MysqlBatchWriter 实现 ====================

class MysqlBatchWriter : public IBatchWriter {
public:
    MysqlBatchWriter(std::shared_ptr<IDbSession> session, const char* table)
        : session_(session), table_(table) {}

    ~MysqlBatchWriter() override {
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
            else if (type_id == arrow::Type::DOUBLE) ddl += "DOUBLE";
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

IBatchWriter* MysqlSession::CreateBatchWriter(const char* table) {
    return new MysqlBatchWriter(shared_from_this(), table);
}

// ==================== MysqlDriver 实现 ====================

MysqlDriver::~MysqlDriver() = default;

int MysqlDriver::Connect(const std::unordered_map<std::string, std::string>& params) {
    ConnectionPoolConfig config;
    config.max_connections = 10;
    config.min_connections = 0;
    config.idle_timeout = std::chrono::seconds(300);
    config.health_check_interval = std::chrono::seconds(60);

    auto it = params.find("host");
    host_ = (it != params.end()) ? it->second : "localhost";

    it = params.find("port");
    if (it != params.end()) port_ = std::stoi(it->second);

    it = params.find("user");
    user_ = (it != params.end()) ? it->second : "root";

    it = params.find("password");
    password_ = (it != params.end()) ? it->second : "";

    it = params.find("database");
    database_ = (it != params.end()) ? it->second : "";

    it = params.find("charset");
    if (it != params.end()) charset_ = it->second;

    it = params.find("timeout");
    if (it != params.end()) timeout_ = std::stoi(it->second);

    auto factory = [this](std::string* error) -> MYSQL* {
        MYSQL* conn = mysql_init(nullptr);
        if (!conn) {
            *error = "mysql_init failed";
            return nullptr;
        }
        mysql_options(conn, MYSQL_OPT_CONNECT_TIMEOUT, &timeout_);
        mysql_options(conn, MYSQL_SET_CHARSET_NAME, charset_.c_str());
        if (!mysql_real_connect(conn, host_.c_str(), user_.c_str(), password_.c_str(),
                                database_.c_str(), port_, nullptr, 0)) {
            *error = mysql_error(conn);
            mysql_close(conn);
            return nullptr;
        }
        return conn;
    };

    auto closer = [](MYSQL* conn) { if (conn) mysql_close(conn); };
    auto pinger = [](MYSQL* conn) -> bool { return conn && mysql_ping(conn) == 0; };

    pool_ = std::make_unique<ConnectionPool<MYSQL*>>(config, factory, closer, pinger);
    printf("MysqlDriver: initialized connection pool for %s:%d/%s\n", host_.c_str(), port_, database_.c_str());
    return 0;
}

int MysqlDriver::Disconnect() {
    pool_.reset();
    printf("MysqlDriver: disconnected\n");
    return 0;
}

bool MysqlDriver::Ping() {
    return pool_ != nullptr;
}

std::shared_ptr<IDbSession> MysqlDriver::CreateSession() {
    if (!pool_) {
        last_error_ = "Driver not connected";
        return nullptr;
    }

    MYSQL* conn = nullptr;
    std::string error;
    if (!pool_->Acquire(&conn, &error)) {
        last_error_ = error;
        return nullptr;
    }

    return std::make_shared<MysqlSession>(this, conn);
}

void MysqlDriver::ReturnToPool(MYSQL* conn) {
    if (pool_ && conn) {
        pool_->Return(conn);
    }
}

}  // namespace database
}  // namespace flowsql
