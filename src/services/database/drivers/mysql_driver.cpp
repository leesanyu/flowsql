#include "mysql_driver.h"

#include "../relation_adapters.h"

#include <arrow/api.h>
#include <arrow/io/memory.h>
#include <arrow/ipc/reader.h>
#include <arrow/ipc/writer.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <common/log.h>
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

const char* MysqlResultSet::FieldName(int index) const {
    if (!result_) return nullptr;
    MYSQL_FIELD* fields = mysql_fetch_fields(result_);
    return (index >= 0 && index < mysql_num_fields(result_)) ? fields[index].name : nullptr;
}

int MysqlResultSet::FieldType(int index) const {
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
        fetched_ = false;
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
    char* end = nullptr;
    errno = 0;
    long v = std::strtol(current_row_[index], &end, 10);
    if (errno != 0 || end == current_row_[index] || v < INT_MIN || v > INT_MAX) return -1;
    *value = static_cast<int>(v);
    return 0;
}

int MysqlResultSet::GetInt64(int index, int64_t* value) {
    if (!current_row_ || !fetched_) return -1;
    if (index < 0 || index >= mysql_num_fields(result_)) return -1;
    if (!current_row_[index]) return -1;
    char* end = nullptr;
    errno = 0;
    long long v = std::strtoll(current_row_[index], &end, 10);
    if (errno != 0 || end == current_row_[index]) return -1;
    *value = static_cast<int64_t>(v);
    return 0;
}

int MysqlResultSet::GetDouble(int index, double* value) {
    if (!current_row_ || !fetched_) return -1;
    if (index < 0 || index >= mysql_num_fields(result_)) return -1;
    if (!current_row_[index]) return -1;
    char* end = nullptr;
    errno = 0;
    double v = std::strtod(current_row_[index], &end);
    if (errno != 0 || end == current_row_[index]) return -1;
    *value = v;
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

// 使用简单 API 执行查询（覆盖基类的 prepared statement 路径）
// 设计说明：MySQL 的简单 API（mysql_query）对于动态 SQL 已足够，
// 且避免了 prepared statement 的额外往返开销。
// 写入路径（InsertBatch）通过 ExecuteSql 同样走简单 API，保持一致。
int MysqlSession::ExecuteQuery(const char* sql, IResultSet** result) {
    last_error_.clear();
    if (mysql_query(conn_, sql) != 0) {
        last_error_ = mysql_error(conn_);
        return -1;
    }
    MYSQL_RES* res = mysql_store_result(conn_);
    if (!res) {
        last_error_ = mysql_error(conn_);
        return -1;
    }
    *result = new MysqlResultSet(res, [](MYSQL_RES* r) { mysql_free_result(r); });
    return 0;
}

int MysqlSession::ExecuteSql(const char* sql) {
    last_error_.clear();
    if (mysql_query(conn_, sql) != 0) {
        last_error_ = mysql_error(conn_);
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


IBatchReader* MysqlSession::CreateBatchReader(IResultSet* result,
                                               std::shared_ptr<arrow::Schema> schema) {
    return new RelationBatchReader(shared_from_this(), result, schema);
}

// ==================== MysqlBatchWriter 实现 ====================

class MysqlBatchWriter : public RelationBatchWriterBase {
public:
    MysqlBatchWriter(std::shared_ptr<IDbSession> session, const char* table)
        : RelationBatchWriterBase(std::move(session), table) {}

protected:
    // MySQL 用反引号包裹标识符，内部反引号用 `` 转义
    std::string QuoteIdentifier(const std::string& name) override {
        std::string result = "`";
        for (char c : name) {
            if (c == '`') result += "``";
            else result += c;
        }
        result += "`";
        return result;
    }

    int CreateTable(std::shared_ptr<arrow::Schema> schema) override {
        std::string ddl = "CREATE TABLE IF NOT EXISTS " + QuoteIdentifier(table_) + " (";
        for (int i = 0; i < schema->num_fields(); ++i) {
            if (i > 0) ddl += ", ";
            ddl += QuoteIdentifier(schema->field(i)->name()) + " ";
            auto type_id = schema->field(i)->type()->id();
            if (type_id == arrow::Type::INT32)  ddl += "INT";
            else if (type_id == arrow::Type::INT64)  ddl += "BIGINT";
            else if (type_id == arrow::Type::FLOAT)  ddl += "FLOAT";
            else if (type_id == arrow::Type::DOUBLE) ddl += "DOUBLE";
            else ddl += "TEXT";
        }
        ddl += ")";
        if (session_->ExecuteSql(ddl.c_str()) < 0) {
            last_error_ = std::string("CREATE TABLE failed: ") + session_->GetLastError();
            return -1;
        }
        return 0;
    }

    /* MySQL 分块多值 INSERT（每次最多 1000 行，性能优化）
       字符串转义：' → \'，\ → \\
    */
    int InsertBatch(std::shared_ptr<arrow::RecordBatch> batch) override {
        if (batch->num_rows() == 0) return 0;

        auto quote_string = [](const std::string& s) -> std::string {
            std::string r = "'";
            for (char c : s) {
                if (c == '\'') r += "\\'";
                else if (c == '\\') r += "\\\\";
                else r += c;
            }
            r += "'";
            return r;
        };

        const int64_t chunk_size = 1000;
        std::string prefix = "INSERT INTO " + QuoteIdentifier(table_) + " VALUES ";

        for (int64_t start = 0; start < batch->num_rows(); start += chunk_size) {
            int64_t end = std::min(start + chunk_size, batch->num_rows());
            std::string sql = prefix;
            for (int64_t row = start; row < end; ++row) {
                if (row > start) sql += ", ";
                sql += BuildRowValues(batch, row, quote_string);
            }
            if (session_->ExecuteSql(sql.c_str()) < 0) {
                last_error_ = std::string("INSERT failed: ") + session_->GetLastError();
                return -1;
            }
            rows_written_ += (end - start);
        }
        return 0;
    }
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

    // 探测连接：验证凭据有效，避免 Connect() 对错误密码返回 0
    MYSQL* probe = nullptr;
    std::string probe_error;
    if (!pool_->Acquire(&probe, &probe_error)) {
        last_error_ = probe_error;
        pool_.reset();
        return -1;
    }
    pool_->Return(probe);

    LOG_INFO("MysqlDriver: initialized connection pool for %s:%d/%s", host_.c_str(), port_, database_.c_str());
    return 0;
}

int MysqlDriver::Disconnect() {
    pool_.reset();
    LOG_INFO("MysqlDriver: disconnected");
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
