/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "postgres_driver.h"

#include "../relation_adapters.h"

#include <arrow/api.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <sstream>

#include <common/log.h>

namespace flowsql {
namespace database {

namespace {
constexpr int kPostgresOidBool = 16;
constexpr int kPostgresOidBytea = 17;
constexpr int kPostgresOidInt8 = 20;
constexpr int kPostgresOidInt2 = 21;
constexpr int kPostgresOidInt4 = 23;
constexpr int kPostgresOidFloat4 = 700;
constexpr int kPostgresOidFloat8 = 701;
constexpr int kPostgresOidNumeric = 1700;
}  // namespace

PostgresResultSet::PostgresResultSet(PGresult* result, std::function<void(PGresult*)> free_func)
    : result_(result), free_func_(std::move(free_func)) {
    if (result_) {
        bytea_cache_.resize(static_cast<size_t>(PQnfields(result_)));
    }
}

PostgresResultSet::~PostgresResultSet() {
    if (result_ && free_func_) {
        free_func_(result_);
    }
}

int PostgresResultSet::FieldCount() { return result_ ? PQnfields(result_) : 0; }

const char* PostgresResultSet::FieldName(int index) const {
    if (!result_) return nullptr;
    if (index < 0 || index >= PQnfields(result_)) return nullptr;
    return PQfname(result_, index);
}

int PostgresResultSet::FieldType(int index) const {
    if (!result_) return 0;
    if (index < 0 || index >= PQnfields(result_)) return 0;
    return static_cast<int>(PQftype(result_, index));
}

int PostgresResultSet::FieldLength(int index) {
    if (!result_) return 0;
    if (index < 0 || index >= PQnfields(result_)) return 0;
    const int len = PQfsize(result_, index);
    return len > 0 ? len : 0;
}

bool PostgresResultSet::HasNext() {
    if (!result_) return false;
    return current_row_ + 1 < PQntuples(result_);
}

bool PostgresResultSet::Next() {
    if (!HasNext()) {
        fetched_ = false;
        return false;
    }
    ++current_row_;
    fetched_ = true;
    return true;
}

int PostgresResultSet::GetInt(int index, int* value) {
    if (!value) return -1;
    const char* raw = nullptr;
    size_t len = 0;
    if (GetString(index, &raw, &len) != 0 || !raw) return -1;
    char* end = nullptr;
    errno = 0;
    long v = std::strtol(raw, &end, 10);
    if (errno != 0 || end == raw || v < INT_MIN || v > INT_MAX) return -1;
    *value = static_cast<int>(v);
    return 0;
}

int PostgresResultSet::GetInt64(int index, int64_t* value) {
    if (!value) return -1;
    const char* raw = nullptr;
    size_t len = 0;
    if (GetString(index, &raw, &len) != 0 || !raw) return -1;
    char* end = nullptr;
    errno = 0;
    long long v = std::strtoll(raw, &end, 10);
    if (errno != 0 || end == raw) return -1;
    *value = static_cast<int64_t>(v);
    return 0;
}

int PostgresResultSet::GetDouble(int index, double* value) {
    if (!value) return -1;
    const char* raw = nullptr;
    size_t len = 0;
    if (GetString(index, &raw, &len) != 0 || !raw) return -1;
    char* end = nullptr;
    errno = 0;
    double v = std::strtod(raw, &end);
    if (errno != 0 || end == raw) return -1;
    *value = v;
    return 0;
}

int PostgresResultSet::GetString(int index, const char** value, size_t* len) {
    if (!value) return -1;
    if (!result_ || !fetched_ || current_row_ < 0) return -1;
    if (index < 0 || index >= PQnfields(result_)) return -1;
    if (PQgetisnull(result_, current_row_, index) == 1) return -1;

    if (static_cast<int>(PQftype(result_, index)) == kPostgresOidBytea) {
        return DecodeBytea(index, value, len);
    }

    const char* raw = PQgetvalue(result_, current_row_, index);
    if (!raw) return -1;
    *value = raw;
    if (len) *len = static_cast<size_t>(PQgetlength(result_, current_row_, index));
    return 0;
}

bool PostgresResultSet::IsNull(int index) {
    if (!result_ || !fetched_ || current_row_ < 0) return true;
    if (index < 0 || index >= PQnfields(result_)) return true;
    return PQgetisnull(result_, current_row_, index) == 1;
}

int PostgresResultSet::HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int PostgresResultSet::DecodeBytea(int index, const char** value, size_t* len) {
    if (!value) return -1;
    if (index < 0 || static_cast<size_t>(index) >= bytea_cache_.size()) return -1;

    const char* raw = PQgetvalue(result_, current_row_, index);
    const int raw_len = PQgetlength(result_, current_row_, index);
    if (!raw || raw_len < 0) return -1;

    std::string& cache = bytea_cache_[static_cast<size_t>(index)];
    cache.clear();

    // PostgreSQL 默认 bytea_output=hex，形如: \xDEADBEEF
    if (raw_len >= 2 && raw[0] == '\\' && raw[1] == 'x') {
        const int hex_len = raw_len - 2;
        if (hex_len % 2 != 0) return -1;
        cache.reserve(static_cast<size_t>(hex_len / 2));
        for (int i = 2; i < raw_len; i += 2) {
            const int hi = HexValue(raw[i]);
            const int lo = HexValue(raw[i + 1]);
            if (hi < 0 || lo < 0) return -1;
            cache.push_back(static_cast<char>((hi << 4) | lo));
        }
    } else {
        // 兼容非 hex 输出，按原始文本返回
        cache.assign(raw, raw + raw_len);
    }

    *value = cache.data();
    if (len) *len = cache.size();
    return 0;
}

PostgresSession::PostgresSession(PostgresDriver* driver, PGconn* conn)
    : RelationDbSessionBase<PostgresTraits>(driver, conn) {}

PostgresSession::~PostgresSession() {
    if (conn_) {
        ReturnConnection(conn_);
        conn_ = nullptr;
    }
}

int PostgresSession::ExecuteQuery(const char* sql, IResultSet** result) {
    if (!result || !sql) return -1;
    last_error_.clear();
    PGresult* res = PQexec(conn_, sql);
    if (!res) {
        last_error_ = PQerrorMessage(conn_);
        return -1;
    }
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        last_error_ = PQerrorMessage(conn_);
        PQclear(res);
        return -1;
    }
    *result = new PostgresResultSet(res, [](PGresult* r) { PQclear(r); });
    return 0;
}

int PostgresSession::ExecuteSql(const char* sql) {
    if (!sql) return -1;
    last_error_.clear();
    PGresult* res = PQexec(conn_, sql);
    if (!res) {
        last_error_ = PQerrorMessage(conn_);
        return -1;
    }

    const ExecStatusType status = PQresultStatus(res);
    if (status != PGRES_COMMAND_OK) {
        last_error_ = PQerrorMessage(conn_);
        PQclear(res);
        return -1;
    }

    int affected_rows = 0;
    const char* tuples = PQcmdTuples(res);
    if (tuples && tuples[0] != '\0') {
        affected_rows = std::atoi(tuples);
    }
    PQclear(res);
    return affected_rows;
}

namespace {

std::string PostgresParameterSql(std::string_view sql, size_t expected_parameters, bool* valid) {
    std::string converted;
    converted.reserve(sql.size() + expected_parameters * 2);
    size_t parameter = 0;
    bool single_quoted = false;
    bool double_quoted = false;
    for (size_t index = 0; index < sql.size(); ++index) {
        const char character = sql[index];
        if (character == '\'' && !double_quoted) {
            if (single_quoted && index + 1 < sql.size() && sql[index + 1] == '\'') {
                converted += "''";
                ++index;
                continue;
            }
            single_quoted = !single_quoted;
        } else if (character == '"' && !single_quoted) {
            if (double_quoted && index + 1 < sql.size() && sql[index + 1] == '"') {
                converted += "\"\"";
                ++index;
                continue;
            }
            double_quoted = !double_quoted;
        }
        if (character == '?' && !single_quoted && !double_quoted) {
            converted += '$' + std::to_string(++parameter);
        } else {
            converted += character;
        }
    }
    *valid = !single_quoted && !double_quoted && parameter == expected_parameters;
    return converted;
}

int ExecutePostgresParameters(PGconn* connection, const char* sql, const DatabaseParameterV1* parameters,
                              size_t parameter_count, std::string* error) {
    if (parameter_count > static_cast<size_t>(std::numeric_limits<int>::max())) {
        *error = "prepared command parameter count exceeds PostgreSQL limit";
        return -1;
    }
    bool valid_sql = false;
    const std::string converted = PostgresParameterSql(sql, parameter_count, &valid_sql);
    if (!valid_sql) {
        *error = "prepared command parameter count mismatch";
        return -1;
    }
    std::vector<std::string> values(parameter_count);
    std::vector<const char*> pointers(parameter_count);
    std::vector<int> lengths(parameter_count);
    std::vector<int> formats(parameter_count);
    std::vector<Oid> types(parameter_count);
    for (size_t index = 0; index < parameter_count; ++index) {
        const auto& parameter = parameters[index];
        switch (parameter.kind) {
            case DatabaseParameterKindV1::kNull:
                pointers[index] = nullptr;
                break;
            case DatabaseParameterKindV1::kInt64:
                values[index] = std::to_string(parameter.int64_value);
                break;
            case DatabaseParameterKindV1::kUInt64:
                types[index] = kPostgresOidNumeric;
                values[index] = std::to_string(parameter.uint64_value);
                break;
            case DatabaseParameterKindV1::kDouble: {
                types[index] = kPostgresOidFloat8;
                std::ostringstream text;
                text << std::setprecision(std::numeric_limits<double>::max_digits10) << parameter.double_value;
                values[index] = text.str();
                break;
            }
            case DatabaseParameterKindV1::kString:
                types[index] = 25;
                if (!parameter.data && parameter.size != 0) {
                    *error = "invalid PostgreSQL string parameter";
                    return -1;
                }
                values[index].assign(parameter.data ? static_cast<const char*>(parameter.data) : "", parameter.size);
                if (values[index].find('\0') != std::string::npos) {
                    *error = "PostgreSQL text parameter contains NUL";
                    return -1;
                }
                break;
            case DatabaseParameterKindV1::kBlob:
                types[index] = kPostgresOidBytea;
                if ((!parameter.data && parameter.size != 0) ||
                    parameter.size > static_cast<size_t>(std::numeric_limits<int>::max())) {
                    *error = "invalid PostgreSQL blob parameter";
                    return -1;
                }
                pointers[index] = parameter.data ? static_cast<const char*>(parameter.data) : "";
                lengths[index] = static_cast<int>(parameter.size);
                formats[index] = 1;
                continue;
        }
        if (parameter.kind != DatabaseParameterKindV1::kNull) pointers[index] = values[index].c_str();
    }
    PGresult* result = PQexecParams(connection, converted.c_str(), static_cast<int>(parameter_count), types.data(),
                                    pointers.data(), lengths.data(), formats.data(), 0);
    if (!result || PQresultStatus(result) != PGRES_COMMAND_OK) {
        *error = PQerrorMessage(connection);
        if (result) PQclear(result);
        return -1;
    }
    int changed = 0;
    const char* tuples = PQcmdTuples(result);
    if (tuples && tuples[0] != '\0') changed = std::atoi(tuples);
    PQclear(result);
    return changed;
}

}  // namespace

int PostgresSession::ExecutePrepared(const char* sql, const DatabaseParameterV1* parameters, size_t parameter_count) {
    last_error_.clear();
    if (!sql || (parameter_count != 0 && !parameters)) {
        last_error_ = "invalid prepared command";
        return -1;
    }
    return ExecutePostgresParameters(conn_, sql, parameters, parameter_count, &last_error_);
}

int PostgresSession::ExecutePreparedBatch(const char* sql, const DatabaseParameterV1* parameters,
                                          size_t parameters_per_execution, size_t execution_count) {
    last_error_.clear();
    if (!sql || (execution_count != 0 && (!parameters || parameters_per_execution == 0)) ||
        (execution_count != 0 && parameters_per_execution > std::numeric_limits<size_t>::max() / execution_count)) {
        last_error_ = "invalid prepared batch";
        return -1;
    }
    if (execution_count == 0) return 0;
    if (BeginTransaction() != 0) return -1;
    int64_t changed = 0;
    for (size_t execution = 0; execution < execution_count; ++execution) {
        const int result = ExecutePostgresParameters(conn_, sql, parameters + execution * parameters_per_execution,
                                                     parameters_per_execution, &last_error_);
        if (result < 0) {
            const std::string failure = last_error_;
            RollbackTransaction();
            last_error_ = failure;
            return -1;
        }
        changed += result;
    }
    if (CommitTransaction() != 0) {
        const std::string failure = last_error_;
        RollbackTransaction();
        last_error_ = failure;
        return -1;
    }
    return changed > std::numeric_limits<int>::max() ? std::numeric_limits<int>::max() : static_cast<int>(changed);
}

const char* PostgresSession::PrepareStatement(PGconn* conn, const char* sql, std::string* error) {
    if (!conn || !sql) {
        if (error) *error = "null connection or sql";
        return nullptr;
    }
    return sql;
}

int PostgresSession::ExecuteStatement(const char* stmt_name, std::string* error) {
    (void)stmt_name;
    if (error) *error = "prepared statement not implemented";
    return -1;
}

PGresult* PostgresSession::GetResultMetadata(const char* stmt_name, std::string* error) {
    (void)stmt_name;
    if (error) *error = "prepared statement not implemented";
    return nullptr;
}

int PostgresSession::GetAffectedRows(const char* stmt_name) {
    (void)stmt_name;
    return 0;
}

std::string PostgresSession::GetDriverError(PGconn* conn) {
    if (!conn) return "null connection";
    return PQerrorMessage(conn);
}

void PostgresSession::FreeResult(PGresult* result) {
    if (result) PQclear(result);
}

void PostgresSession::FreeStatement(const char* stmt_name) { (void)stmt_name; }

bool PostgresSession::PingImpl(PGconn* conn) {
    if (!conn) return false;
    PGresult* res = PQexec(conn, "SELECT 1");
    if (!res) return false;
    const bool ok = PQresultStatus(res) == PGRES_TUPLES_OK;
    PQclear(res);
    return ok;
}

void PostgresSession::ReturnConnection(PGconn* conn) {
    auto* driver = static_cast<PostgresDriver*>(driver_);
    if (driver && conn) {
        driver->ReturnToPool(conn);
    }
}

IResultSet* PostgresSession::CreateResultSet(PGresult* result, std::function<void(PGresult*)> free_func) {
    return new PostgresResultSet(result, std::move(free_func));
}

IBatchReader* PostgresSession::CreateBatchReader(IResultSet* result, std::shared_ptr<arrow::Schema> schema) {
    return new RelationBatchReader(shared_from_this(), result, std::move(schema));
}

class PostgresBatchWriter : public RelationBatchWriterBase {
 public:
    PostgresBatchWriter(std::shared_ptr<IDbSession> session, const char* table)
        : RelationBatchWriterBase(std::move(session), table) {}

 protected:
    std::string QuoteIdentifier(const std::string& name) override {
        std::string result = "\"";
        for (char c : name) {
            if (c == '"')
                result += "\"\"";
            else
                result += c;
        }
        result += "\"";
        return result;
    }

    int CreateTable(std::shared_ptr<arrow::Schema> schema) override {
        std::string ddl = "CREATE TABLE IF NOT EXISTS " + QuoteIdentifier(table_) + " (";
        for (int i = 0; i < schema->num_fields(); ++i) {
            if (i > 0) ddl += ", ";
            ddl += QuoteIdentifier(schema->field(i)->name()) + " ";
            const auto type_id = schema->field(i)->type()->id();
            if (type_id == arrow::Type::INT32)
                ddl += "INTEGER";
            else if (type_id == arrow::Type::INT64)
                ddl += "BIGINT";
            else if (type_id == arrow::Type::FLOAT)
                ddl += "REAL";
            else if (type_id == arrow::Type::DOUBLE)
                ddl += "DOUBLE PRECISION";
            else if (type_id == arrow::Type::BOOL)
                ddl += "BOOLEAN";
            else if (type_id == arrow::Type::BINARY)
                ddl += "BYTEA";
            else
                ddl += "TEXT";
        }
        ddl += ")";
        if (session_->ExecuteSql(ddl.c_str()) < 0) {
            last_error_ = std::string("CREATE TABLE failed: ") + session_->GetLastError();
            return -1;
        }
        return 0;
    }

    int InsertBatch(std::shared_ptr<arrow::RecordBatch> batch) override {
        if (batch->num_rows() == 0) return 0;

        auto quote_string = [](const std::string& s) -> std::string {
            std::string out = "'";
            for (char c : s) {
                if (c == '\'')
                    out += "''";
                else
                    out += c;
            }
            out += "'";
            return out;
        };

        const int64_t chunk_size = 1000;
        const std::string prefix = "INSERT INTO " + QuoteIdentifier(table_) + " VALUES ";

        for (int64_t start = 0; start < batch->num_rows(); start += chunk_size) {
            const int64_t end = std::min(start + chunk_size, batch->num_rows());
            std::string sql = prefix;
            for (int64_t row = start; row < end; ++row) {
                if (row > start) sql += ", ";
                sql += "(";
                for (int col = 0; col < batch->num_columns(); ++col) {
                    if (col > 0) sql += ", ";
                    auto array = batch->column(col);
                    if (array->IsNull(row)) {
                        sql += "NULL";
                        continue;
                    }

                    const auto type_id = array->type()->id();
                    if (type_id == arrow::Type::INT32) {
                        sql += std::to_string(std::static_pointer_cast<arrow::Int32Array>(array)->Value(row));
                    } else if (type_id == arrow::Type::INT64) {
                        sql += std::to_string(std::static_pointer_cast<arrow::Int64Array>(array)->Value(row));
                    } else if (type_id == arrow::Type::FLOAT) {
                        sql += std::to_string(std::static_pointer_cast<arrow::FloatArray>(array)->Value(row));
                    } else if (type_id == arrow::Type::DOUBLE) {
                        sql += std::to_string(std::static_pointer_cast<arrow::DoubleArray>(array)->Value(row));
                    } else if (type_id == arrow::Type::BOOL) {
                        sql += std::static_pointer_cast<arrow::BooleanArray>(array)->Value(row) ? "TRUE" : "FALSE";
                    } else if (type_id == arrow::Type::BINARY) {
                        auto blob = std::static_pointer_cast<arrow::BinaryArray>(array)->GetView(row);
                        sql += "decode('";
                        for (size_t i = 0; i < blob.size(); ++i) {
                            char hex[3];
                            std::snprintf(hex, sizeof(hex), "%02X", static_cast<unsigned char>(blob.data()[i]));
                            sql += hex;
                        }
                        sql += "','hex')";
                    } else {
                        sql += quote_string(std::static_pointer_cast<arrow::StringArray>(array)->GetString(row));
                    }
                }
                sql += ")";
            }

            if (session_->ExecuteSql(sql.c_str()) < 0) {
                last_error_ = std::string("INSERT failed: ") + session_->GetLastError();
                return -1;
            }
            rows_written_ += end - start;
        }
        return 0;
    }
};

IBatchWriter* PostgresSession::CreateBatchWriter(const char* table) {
    return new PostgresBatchWriter(shared_from_this(), table);
}

std::shared_ptr<arrow::Schema> PostgresSession::InferSchema(IResultSet* result, std::string* error) {
    (void)error;
    auto* postgres_result = static_cast<PostgresResultSet*>(result);
    PGresult* native_result = postgres_result ? postgres_result->GetResult() : nullptr;
    const int n = result ? result->FieldCount() : 0;
    std::vector<std::shared_ptr<arrow::Field>> fields;
    fields.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        std::shared_ptr<arrow::DataType> type = arrow::utf8();
        const int oid = result->FieldType(i);
        switch (oid) {
            case kPostgresOidBool:
                type = arrow::boolean();
                break;
            case kPostgresOidInt2:
            case kPostgresOidInt4:
                type = arrow::int32();
                break;
            case kPostgresOidInt8:
                type = arrow::int64();
                break;
            case kPostgresOidFloat4:
                type = arrow::float32();
                break;
            case kPostgresOidFloat8:
                type = arrow::float64();
                break;
            case kPostgresOidNumeric: {
                const int modifier = native_result ? PQfmod(native_result, i) : -1;
                const int encoded = modifier >= 4 ? modifier - 4 : -1;
                const int precision = encoded >= 0 ? (encoded >> 16) & 0xffff : -1;
                const int scale = encoded >= 0 ? encoded & 0xffff : -1;
                type = precision == 20 && scale == 0 ? arrow::uint64() : arrow::float64();
                break;
            }
            case kPostgresOidBytea:
                type = arrow::binary();
                break;
            default:
                type = arrow::utf8();
                break;
        }
        const char* name = result->FieldName(i);
        fields.push_back(arrow::field(name ? name : "", std::move(type)));
    }
    return arrow::schema(std::move(fields));
}

PostgresDriver::~PostgresDriver() = default;

std::string PostgresDriver::BuildConninfo() const {
    return "host=" + host_ + " port=" + std::to_string(port_) + " user=" + user_ + " password=" + password_ +
           " dbname=" + database_ + " connect_timeout=" + std::to_string(timeout_);
}

int PostgresDriver::Connect(const std::unordered_map<std::string, std::string>& params) {
    ConnectionPoolConfig config;
    config.max_connections = 10;
    config.min_connections = 0;
    config.idle_timeout = std::chrono::seconds(300);
    config.health_check_interval = std::chrono::seconds(60);

    auto it = params.find("host");
    host_ = (it != params.end()) ? it->second : "127.0.0.1";

    it = params.find("port");
    if (it != params.end()) port_ = std::stoi(it->second);

    it = params.find("user");
    user_ = (it != params.end()) ? it->second : "postgres";

    it = params.find("password");
    password_ = (it != params.end()) ? it->second : "";

    it = params.find("database");
    database_ = (it != params.end()) ? it->second : "postgres";

    it = params.find("timeout");
    if (it != params.end()) timeout_ = std::stoi(it->second);

    auto factory = [this](std::string* error) -> PGconn* {
        PGconn* conn = PQconnectdb(BuildConninfo().c_str());
        if (!conn || PQstatus(conn) != CONNECTION_OK) {
            if (error) *error = conn ? PQerrorMessage(conn) : "PQconnectdb failed";
            if (conn) PQfinish(conn);
            return nullptr;
        }
        return conn;
    };

    auto closer = [](PGconn* conn) {
        if (conn) PQfinish(conn);
    };

    auto pinger = [](PGconn* conn) -> bool {
        if (!conn) return false;
        PGresult* res = PQexec(conn, "SELECT 1");
        if (!res) return false;
        const bool ok = PQresultStatus(res) == PGRES_TUPLES_OK;
        PQclear(res);
        return ok;
    };

    pool_ = std::make_unique<ConnectionPool<PGconn*>>(config, factory, closer, pinger);

    PGconn* probe = nullptr;
    std::string probe_error;
    if (!pool_->Acquire(&probe, &probe_error)) {
        last_error_ = probe_error;
        pool_.reset();
        return -1;
    }
    pool_->Return(probe);
    LOG_INFO("PostgresDriver: initialized connection pool for %s:%d/%s", host_.c_str(), port_, database_.c_str());
    return 0;
}

int PostgresDriver::Disconnect() {
    pool_.reset();
    LOG_INFO("PostgresDriver: disconnected");
    return 0;
}

bool PostgresDriver::Ping() {
    if (!pool_) {
        last_error_ = "Driver not connected";
        return false;
    }

    PGconn* conn = PQconnectdb(BuildConninfo().c_str());
    if (!conn || PQstatus(conn) != CONNECTION_OK) {
        last_error_ = conn ? PQerrorMessage(conn) : "PQconnectdb failed";
        if (conn) PQfinish(conn);
        return false;
    }

    PGresult* res = PQexec(conn, "SELECT 1");
    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK) {
        last_error_ = res ? PQresultErrorMessage(res) : PQerrorMessage(conn);
        if (res) PQclear(res);
        PQfinish(conn);
        return false;
    }
    PQclear(res);
    PQfinish(conn);
    return true;
}

std::shared_ptr<IDbSession> PostgresDriver::CreateSession() {
    if (!pool_) {
        last_error_ = "Driver not connected";
        return nullptr;
    }

    PGconn* conn = nullptr;
    std::string error;
    if (!pool_->Acquire(&conn, &error)) {
        last_error_ = error;
        return nullptr;
    }
    return std::make_shared<PostgresSession>(this, conn);
}

void PostgresDriver::ReturnToPool(PGconn* conn) {
    if (pool_ && conn) {
        pool_->Return(conn);
    }
}

}  // namespace database
}  // namespace flowsql
