#include "sqlite_driver.h"

#include "../relation_adapters.h"

#include <arrow/api.h>
#include <arrow/io/memory.h>
#include <arrow/ipc/reader.h>
#include <arrow/ipc/writer.h>

#include <cstdio>
#include <common/log.h>
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

const char* SqliteResultSet::FieldName(int index) const {
    if (!stmt_) return nullptr;
    return (index >= 0 && index < sqlite3_column_count(stmt_))
           ? sqlite3_column_name(stmt_, index) : nullptr;
}

int SqliteResultSet::FieldType(int index) const {
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
        fetched_ = false;
        return false;
    } else {
        has_next_ = false;
        fetched_ = false;
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

SqliteSession::~SqliteSession() {
    if (conn_) {
        ReturnConnection(conn_);
        conn_ = nullptr;
    }
}

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

    // SELECT 语句（有列）不预先 step，让 ResultSet 自己迭代
    if (sqlite3_column_count(stmt) > 0) return 0;

    // 非 SELECT（INSERT/UPDATE/DELETE/DDL）
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
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

IBatchReader* SqliteSession::CreateBatchReader(IResultSet* result,
                                                std::shared_ptr<arrow::Schema> schema) {
    return new RelationBatchReader(shared_from_this(), result, schema);
}

// ==================== SqliteBatchWriter 实现 ====================

class SqliteBatchWriter : public RelationBatchWriterBase {
public:
    SqliteBatchWriter(std::shared_ptr<IDbSession> session, const char* table)
        : RelationBatchWriterBase(std::move(session), table) {}

protected:
    // SQLite 用双引号包裹标识符，内部双引号用 "" 转义
    std::string QuoteIdentifier(const std::string& name) override {
        std::string result = "\"";
        for (char c : name) {
            if (c == '"') result += "\"\"";
            else result += c;
        }
        result += "\"";
        return result;
    }

    int CreateTable(std::shared_ptr<arrow::Schema> schema) override {
        std::string ddl = "CREATE TABLE IF NOT EXISTS " + QuoteIdentifier(table_) + " (";
        for (int i = 0; i < schema->num_fields(); ++i) {
            if (i > 0) ddl += ", ";
            ddl += QuoteIdentifier(schema->field(i)->name()) + " ";
            auto type_id = schema->field(i)->type()->id();
            if (type_id == arrow::Type::INT32 || type_id == arrow::Type::INT64)
                ddl += "BIGINT";
            else if (type_id == arrow::Type::FLOAT || type_id == arrow::Type::DOUBLE)
                ddl += "REAL";
            else if (type_id == arrow::Type::BINARY)
                ddl += "BLOB";
            else
                ddl += "TEXT";
        }
        ddl += ")";
        std::string error;
        if (session_->ExecuteSql(ddl.c_str(), &error) < 0) {
            last_error_ = "CREATE TABLE failed: " + error;
            return -1;
        }
        return 0;
    }

    int InsertBatch(std::shared_ptr<arrow::RecordBatch> batch) override {
        if (batch->num_rows() == 0) return 0;

        // SQLite 逐行 INSERT（不支持多值 INSERT 的旧版本兼容）
        // 字符串转义：' → ''（SQL 标准），BINARY 用 X'...' 十六进制字面量
        auto quote_string = [](const std::string& s) -> std::string {
            std::string r = "'";
            for (char c : s) {
                if (c == '\'') r += "''";
                else r += c;
            }
            r += "'";
            return r;
        };

        std::string prefix = "INSERT INTO " + QuoteIdentifier(table_) + " VALUES ";
        for (int64_t row = 0; row < batch->num_rows(); ++row) {
            // BINARY 列需要特殊处理（X'...' 格式），不能走通用 BuildRowValues
            std::string values = "(";
            for (int col = 0; col < batch->num_columns(); ++col) {
                if (col > 0) values += ", ";
                auto array = batch->column(col);
                if (array->IsNull(row)) {
                    values += "NULL";
                } else if (array->type()->id() == arrow::Type::BINARY) {
                    auto blob = std::static_pointer_cast<arrow::BinaryArray>(array)->GetView(row);
                    values += "X'";
                    for (size_t i = 0; i < blob.size(); ++i) {
                        char hex[3];
                        snprintf(hex, sizeof(hex), "%02X",
                                 static_cast<unsigned char>(blob.data()[i]));
                        values += hex;
                    }
                    values += "'";
                } else if (array->type()->id() == arrow::Type::INT32) {
                    values += std::to_string(
                        std::static_pointer_cast<arrow::Int32Array>(array)->Value(row));
                } else if (array->type()->id() == arrow::Type::INT64) {
                    values += std::to_string(
                        std::static_pointer_cast<arrow::Int64Array>(array)->Value(row));
                } else if (array->type()->id() == arrow::Type::FLOAT) {
                    values += std::to_string(
                        std::static_pointer_cast<arrow::FloatArray>(array)->Value(row));
                } else if (array->type()->id() == arrow::Type::DOUBLE) {
                    values += std::to_string(
                        std::static_pointer_cast<arrow::DoubleArray>(array)->Value(row));
                } else {
                    values += quote_string(
                        std::static_pointer_cast<arrow::StringArray>(array)->GetString(row));
                }
            }
            values += ")";
            std::string error;
            if (session_->ExecuteSql((prefix + values).c_str(), &error) < 0) {
                last_error_ = "INSERT failed: " + error;
                return -1;
            }
            ++rows_written_;
        }
        return 0;
    }
};

IBatchWriter* SqliteSession::CreateBatchWriter(const char* table) {
    return new SqliteBatchWriter(shared_from_this(), table);
}


// ==================== SqliteDriver 实现 ====================

std::shared_ptr<arrow::Schema> SqliteSession::InferSchema(IResultSet* result, std::string* error) {
    auto* rs = static_cast<SqliteResultSet*>(result);
    sqlite3_stmt* stmt = rs->GetStmt();
    int n = sqlite3_column_count(stmt);
    std::vector<std::shared_ptr<arrow::Field>> fields;
    for (int i = 0; i < n; ++i) {
        const char* name = sqlite3_column_name(stmt, i);
        const char* decl = sqlite3_column_decltype(stmt, i);
        std::shared_ptr<arrow::DataType> type = arrow::utf8();
        if (decl) {
            std::string t = decl;
            for (auto& c : t) c = static_cast<char>(toupper(c));
            if (t.find("INT") != std::string::npos)
                type = arrow::int64();
            else if (t.find("REAL") != std::string::npos || t.find("FLOAT") != std::string::npos ||
                     t.find("DOUBLE") != std::string::npos)
                type = arrow::float64();
            else if (t.find("NUMERIC") != std::string::npos || t.find("DECIMAL") != std::string::npos)
                type = arrow::float64();
            else if (t.find("BOOL") != std::string::npos)
                type = arrow::boolean();
            else if (t.find("BLOB") != std::string::npos)
                type = arrow::binary();
            else if (t.find("DATE") != std::string::npos || t.find("TIME") != std::string::npos)
                type = arrow::utf8();  // SQLite 无原生日期类型，以字符串存储
            // CHAR/VARCHAR/TEXT/CLOB 等均映射为 utf8（默认值）
        } else {
            // 无声明类型（如表达式列），根据运行时类型推断
            int runtime_type = sqlite3_column_type(stmt, i);
            if (runtime_type == SQLITE_INTEGER) type = arrow::int64();
            else if (runtime_type == SQLITE_FLOAT) type = arrow::float64();
            else if (runtime_type == SQLITE_BLOB) type = arrow::binary();
        }
        fields.push_back(arrow::field(name ? name : "", type));
    }
    return arrow::schema(fields);
}

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
    LOG_INFO("SqliteDriver: initialized connection pool for %s", db_path_.c_str());
    return 0;
}

int SqliteDriver::Disconnect() {
    pool_.reset();
    LOG_INFO("SqliteDriver: disconnected");
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

}  // namespace database
}  // namespace flowsql
