// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <libpq-fe.h>
#include <mysql/mysql.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <charconv>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include "atomic_target.h"
namespace flowsql::database {
namespace {
using Code = DatabaseAtomicCodeV1;
using Status = DatabaseAtomicStatusV1;
using Outcome = DatabaseCommitOutcomeV1;
using Clock = std::chrono::steady_clock;
constexpr uint64_t kReadLimit = 1024ULL * 1024 * 1024;
using Cell = std::optional<std::string>;
Status BuildBatch(const std::vector<std::shared_ptr<arrow::Field>>& fields, const std::vector<std::vector<Cell>>& rows,
                  std::shared_ptr<arrow::RecordBatch>* output) {
    std::vector<std::shared_ptr<arrow::Array>> arrays;
    for (size_t column = 0; column < fields.size(); ++column) {
        auto made = arrow::MakeBuilder(fields[column]->type());
        if (!made.ok()) return {Code::kError, made.status().ToString()};
        auto builder = std::move(*made);
        for (const auto& row : rows) {
            arrow::Status s;
            if (!row[column])
                s = builder->AppendNull();
            else {
                auto& text = *row[column];
                auto begin = text.data();
                auto end = begin + text.size();
                switch (fields[column]->type()->id()) {
                    case arrow::Type::INT64: {
                        int64_t v;
                        auto r = std::from_chars(begin, end, v);
                        if (r.ec != std::errc{} || r.ptr != end) return {Code::kError, "lossy int64 result"};
                        s = static_cast<arrow::Int64Builder*>(builder.get())->Append(v);
                        break;
                    }
                    case arrow::Type::UINT64: {
                        uint64_t v;
                        auto r = std::from_chars(begin, end, v);
                        if (r.ec != std::errc{} || r.ptr != end) return {Code::kError, "lossy uint64 result"};
                        s = static_cast<arrow::UInt64Builder*>(builder.get())->Append(v);
                        break;
                    }
                    case arrow::Type::DOUBLE: {
                        double v;
                        auto r = std::from_chars(begin, end, v);
                        if (r.ec != std::errc{} || r.ptr != end) return {Code::kError, "invalid double result"};
                        s = static_cast<arrow::DoubleBuilder*>(builder.get())->Append(v);
                        break;
                    }
                    case arrow::Type::BINARY:
                        s = static_cast<arrow::BinaryBuilder*>(builder.get())->Append(text);
                        break;
                    default:
                        s = static_cast<arrow::StringBuilder*>(builder.get())->Append(text);
                }
            }
            if (!s.ok()) return {Code::kError, s.ToString()};
        }
        auto a = builder->Finish();
        if (!a.ok()) return {Code::kError, a.status().ToString()};
        arrays.push_back(*a);
    }
    *output = arrow::RecordBatch::Make(arrow::schema(fields), rows.size(), arrays);
    return {};
}
class ServerAtomic : public IDatabaseAtomicSessionV1 {
 public:
    explicit ServerAtomic(uint32_t timeout) : timeout_(timeout) {}
    Status Begin() override {
        if (auto s = Available(); !s.ok()) return s;
        if (active_) return {Code::kInvalidArgument, "nested transaction"};
        auto s = Run("BEGIN", nullptr, 0, nullptr, nullptr);
        if (s.ok()) active_ = true;
        return s;
    }
    Status ExecutePrepared(const char* sql, const DatabaseParameterV1* p, size_t count, uint64_t* n) override {
        if (n) *n = 0;
        if (!n || !active_) return {Code::kInvalidArgument, "active transaction/affected required"};
        if (auto s = Available(); !s.ok()) return s;
        if (auto s = Validate(sql, p, count); !s.ok()) return s;
        return Run(sql, p, count, n, nullptr);
    }
    Status CreateReader(const char* sql, const DatabaseParameterV1* p, size_t count, IBatchReader** out) override {
        if (!out) return {Code::kInvalidArgument, "reader required"};
        *out = nullptr;
        if (auto s = Available(); !s.ok()) return s;
        if (auto s = Validate(sql, p, count); !s.ok()) return s;
        if (std::string_view(sql).substr(0, 7) != "SELECT ") return {Code::kInvalidArgument, "SELECT required"};
        std::shared_ptr<arrow::RecordBatch> batch;
        auto s = Run(sql, p, count, nullptr, &batch);
        if (!s.ok()) return s;
        *out = MakeAtomicBufferedReader(std::move(batch), readers_);
        return {};
    }
    Status ReadDatabaseTime(int64_t* out) override {
        if (!out) return {Code::kInvalidArgument, "time required"};
        if (auto s = Available(); !s.ok()) return s;
        std::shared_ptr<arrow::RecordBatch> batch;
        auto s = Run(TimeSql(), nullptr, 0, nullptr, &batch);
        if (!s.ok()) return s;
        if (batch->num_rows() != 1 || batch->column(0)->type_id() != arrow::Type::INT64)
            return {Code::kError, "invalid server time result"};
        *out = std::static_pointer_cast<arrow::Int64Array>(batch->column(0))->Value(0);
        return {};
    }
    DatabaseCommitResultV1 Commit() override {
        if (!active_) return {{Code::kInvalidArgument, "transaction required"}, Outcome::kRolledBack};
        if (*readers_) {
            auto s = Rollback();
            if (s.ok()) return {{Code::kInvalidArgument, "close readers before Commit"}, Outcome::kRolledBack};
            poisoned_ = true;
            return {{Code::kCommitUnknown, s.message}, Outcome::kUnknown};
        }
        auto s = Available();
        if (s.ok()) s = Run("COMMIT", nullptr, 0, nullptr, nullptr);
        if (s.ok()) {
            active_ = false;
            return {{}, Outcome::kCommitted};
        }
        if (!Connected() || cancelled_ || s.code == Code::kTimeout) {
            poisoned_ = true;
            return {{Code::kCommitUnknown, s.message}, Outcome::kUnknown};
        }
        auto rollback = Run("ROLLBACK", nullptr, 0, nullptr, nullptr);
        if (rollback.ok()) {
            active_ = false;
            return {s, Outcome::kRolledBack};
        }
        poisoned_ = true;
        return {{Code::kCommitUnknown, s.message}, Outcome::kUnknown};
    }
    Status Rollback() override {
        if (!active_ || poisoned_ || *readers_) return {Code::kError, "rollback unavailable"};
        auto s = Run("ROLLBACK", nullptr, 0, nullptr, nullptr);
        if (s.ok()) active_ = false;
        return s;
    }
    void Cancel() override {
        cancelled_ = true;
        int fd = socket_;
        if (fd >= 0) shutdown(fd, SHUT_RDWR);
    }
    void Close() override {
        if (closed_) return;
        if (active_ && !poisoned_ && !cancelled_ && Connected()) Run("ROLLBACK", nullptr, 0, nullptr, nullptr);
        socket_ = -1;
        closed_ = true;
        active_ = false;
        Disconnect();
    }

 protected:
    virtual Status Run(const char*, const DatabaseParameterV1*, size_t, uint64_t*,
                       std::shared_ptr<arrow::RecordBatch>*) = 0;
    virtual const char* TimeSql() const = 0;
    virtual bool Connected() const = 0;
    virtual void Disconnect() = 0;
    Status Available() {
        if (closed_ || poisoned_) return {Code::kError, "closed/poisoned session"};
        if (cancelled_) return {Code::kCancelled, "cancelled"};
        return {};
    }
    Status Validate(const char* sql, const DatabaseParameterV1* p, size_t count) {
        if (!sql || (count && !p) || count > INT_MAX) return {Code::kInvalidArgument, "invalid parameters"};
        for (size_t i = 0; i < count; ++i) {
            if (p[i].size > INT_MAX || (p[i].size && !p[i].data) ||
                static_cast<int>(p[i].kind) > static_cast<int>(DatabaseParameterKindV1::kBlob))
                return {Code::kInvalidArgument, "invalid parameter size/kind"};
            if (p[i].kind == DatabaseParameterKindV1::kDouble && !std::isfinite(p[i].double_value))
                return {Code::kInvalidArgument, "nonfinite parameter"};
        }
        return {};
    }
    uint32_t timeout_;
    std::atomic<bool> cancelled_{false};
    std::atomic<int> socket_{-1};
    bool active_ = false, closed_ = false, poisoned_ = false;
    std::shared_ptr<std::atomic<uint32_t>> readers_ = std::make_shared<std::atomic<uint32_t>>(0);
};
std::string Parameter(const std::unordered_map<std::string, std::string>& p, const std::string& name,
                      const std::string& fallback) {
    auto i = p.find(name);
    return i == p.end() ? fallback : i->second;
}
class MysqlAtomic final : public ServerAtomic {
 public:
    MysqlAtomic(MYSQL* db, uint32_t timeout) : ServerAtomic(timeout), db_(db) { socket_ = db_->net.fd; }
    ~MysqlAtomic() override { Close(); }

 protected:
    const char* TimeSql() const override { return "SELECT CAST(UNIX_TIMESTAMP(SYSDATE(3))*1000 AS SIGNED) AS unix_ms"; }
    bool Connected() const override { return db_ && !broken_; }
    void Disconnect() override {
        if (db_) mysql_close(db_);
        db_ = nullptr;
    }
    Status Error(unsigned int code, const char* message) {
        if (code == 2006 || code == 2013 || code == 2055) broken_ = true;
        return {cancelled_ ? Code::kCancelled : code == 1205 || broken_ ? Code::kTimeout : Code::kError, message};
    }
    Status Run(const char* sql, const DatabaseParameterV1* p, size_t count, uint64_t* affected,
               std::shared_ptr<arrow::RecordBatch>* output) override {
        if (!db_ || cancelled_) return {Code::kCancelled, "connection cancelled"};
        // Socket timeout bounds native prepared operations; cancellation permanently wakes this socket.
        timeval tv{static_cast<time_t>(timeout_ / 1000), static_cast<suseconds_t>((timeout_ % 1000) * 1000)};
        setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(socket_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (count == 0 && !output) {
            auto deadline = Clock::now() + std::chrono::milliseconds(timeout_);
            net_async_status async;
            while ((async = mysql_real_query_nonblocking(db_, sql, std::strlen(sql))) == NET_ASYNC_NOT_READY) {
                if (cancelled_ || Clock::now() >= deadline) {
                    broken_ = true;
                    shutdown(socket_, SHUT_RDWR);
                    return {cancelled_ ? Code::kCancelled : Code::kTimeout, "command deadline/cancelled"};
                }
                pollfd fd{socket_, POLLIN | POLLOUT, 0};
                poll(&fd, 1, 1);
            }
            if (async != NET_ASYNC_COMPLETE) return Error(mysql_errno(db_), mysql_error(db_));
            if (mysql_field_count(db_)) return {Code::kError, "command returned result"};
            auto n = mysql_affected_rows(db_);
            if (affected) *affected = n == static_cast<my_ulonglong>(-1) ? 0 : n;
            return {};
        }
        MYSQL_STMT* raw = mysql_stmt_init(db_);
        if (!raw) return {Code::kError, "statement allocation failed"};
        std::unique_ptr<MYSQL_STMT, decltype(&mysql_stmt_close)> stmt(raw, mysql_stmt_close);
        if (mysql_stmt_prepare(raw, sql, std::strlen(sql))) return Error(mysql_stmt_errno(raw), mysql_stmt_error(raw));
        if (mysql_stmt_param_count(raw) != count) return {Code::kInvalidArgument, "parameter count mismatch"};
        std::vector<MYSQL_BIND> bindings(count);
        auto nulls = std::make_unique<bool[]>(count);
        std::vector<unsigned long> lengths(count);
        for (size_t i = 0; i < count; ++i) {
            auto& b = bindings[i];
            auto& v = p[i];
            b.is_null = &nulls[i];
            switch (v.kind) {
                case DatabaseParameterKindV1::kNull:
                    b.buffer_type = MYSQL_TYPE_NULL;
                    nulls[i] = true;
                    break;
                case DatabaseParameterKindV1::kInt64:
                    b.buffer_type = MYSQL_TYPE_LONGLONG;
                    b.buffer = const_cast<int64_t*>(&v.int64_value);
                    break;
                case DatabaseParameterKindV1::kUInt64:
                    b.buffer_type = MYSQL_TYPE_LONGLONG;
                    b.is_unsigned = 1;
                    b.buffer = const_cast<uint64_t*>(&v.uint64_value);
                    break;
                case DatabaseParameterKindV1::kDouble:
                    b.buffer_type = MYSQL_TYPE_DOUBLE;
                    b.buffer = const_cast<double*>(&v.double_value);
                    break;
                default:
                    b.buffer_type = v.kind == DatabaseParameterKindV1::kString ? MYSQL_TYPE_STRING : MYSQL_TYPE_BLOB;
                    lengths[i] = v.size;
                    b.buffer = const_cast<void*>(v.data ? v.data : static_cast<const void*>(""));
                    b.buffer_length = v.size;
                    b.length = &lengths[i];
            }
        }
        if (count && mysql_stmt_bind_param(raw, bindings.data()))
            return Error(mysql_stmt_errno(raw), mysql_stmt_error(raw));
        if (mysql_stmt_execute(raw)) return Error(mysql_stmt_errno(raw), mysql_stmt_error(raw));
        if (!output) {
            auto n = mysql_stmt_affected_rows(raw);
            if (n == static_cast<my_ulonglong>(-1)) return {Code::kError, "command returned a result set"};
            if (affected) *affected = n;
            return {};
        }
        MYSQL_RES* metadata = mysql_stmt_result_metadata(raw);
        if (!metadata) return {Code::kError, "result metadata missing"};
        std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> meta(metadata, mysql_free_result);
        const auto n = mysql_num_fields(metadata);
        auto* fields = mysql_fetch_fields(metadata);
        std::vector<std::shared_ptr<arrow::Field>> schema;
        std::vector<MYSQL_BIND> result(n);
        std::vector<unsigned long> sizes(n);
        auto is_null = std::make_unique<bool[]>(n);
        auto truncated = std::make_unique<bool[]>(n);
        std::vector<char> scratch(n);
        std::vector<double> native_doubles(n);
        for (unsigned int i = 0; i < n; ++i) {
            auto t = fields[i].type;
            bool integer = t == MYSQL_TYPE_TINY || t == MYSQL_TYPE_SHORT || t == MYSQL_TYPE_LONG ||
                           t == MYSQL_TYPE_LONGLONG || t == MYSQL_TYPE_INT24;
            bool real = t == MYSQL_TYPE_FLOAT || t == MYSQL_TYPE_DOUBLE;
            bool binary = (fields[i].flags & BINARY_FLAG) &&
                          (t == MYSQL_TYPE_BLOB || t == MYSQL_TYPE_LONG_BLOB || t == MYSQL_TYPE_MEDIUM_BLOB ||
                           t == MYSQL_TYPE_TINY_BLOB || t == MYSQL_TYPE_VAR_STRING || t == MYSQL_TYPE_STRING);
            schema.push_back(arrow::field(
                fields[i].name, integer  ? ((fields[i].flags & UNSIGNED_FLAG) ? arrow::uint64() : arrow::int64())
                                : real   ? arrow::float64()
                                : binary ? arrow::binary()
                                         : arrow::utf8()));
            result[i].buffer_type = real ? MYSQL_TYPE_DOUBLE : MYSQL_TYPE_STRING;
            result[i].buffer = real ? static_cast<void*>(&native_doubles[i]) : static_cast<void*>(&scratch[i]);
            result[i].buffer_length = real ? sizeof(double) : 1;
            result[i].length = &sizes[i];
            result[i].is_null = &is_null[i];
            result[i].error = &truncated[i];
        }
        if (mysql_stmt_bind_result(raw, result.data())) return Error(mysql_stmt_errno(raw), mysql_stmt_error(raw));
        std::vector<std::vector<Cell>> rows;
        uint64_t bytes = 0;
        int rc;
        while ((rc = mysql_stmt_fetch(raw)) == 0 || rc == MYSQL_DATA_TRUNCATED) {
            std::vector<Cell> row;
            for (unsigned int i = 0; i < n; ++i) {
                bytes += 16 + sizes[i];
                if (bytes > kReadLimit) return {Code::kError, "atomic result byte limit"};
                if (is_null[i]) {
                    row.emplace_back(std::nullopt);
                    continue;
                }
                if (schema[i]->type()->id() == arrow::Type::DOUBLE) {
                    char text[64];
                    auto converted =
                        std::to_chars(text, text + sizeof(text), native_doubles[i], std::chars_format::general,
                                      std::numeric_limits<double>::max_digits10);
                    if (converted.ec != std::errc{}) return {Code::kError, "double result conversion failed"};
                    row.emplace_back(std::string(text, converted.ptr));
                    continue;
                }
                std::string value(sizes[i], '\0');
                MYSQL_BIND b{};
                b.buffer_type = MYSQL_TYPE_STRING;
                b.buffer = value.data();
                b.buffer_length = value.size();
                b.length = &sizes[i];
                if (mysql_stmt_fetch_column(raw, &b, i, 0)) return Error(mysql_stmt_errno(raw), mysql_stmt_error(raw));
                row.emplace_back(std::move(value));
            }
            rows.push_back(std::move(row));
            if (cancelled_) return {Code::kCancelled, "cancelled"};
        }
        if (rc != MYSQL_NO_DATA) return Error(mysql_stmt_errno(raw), mysql_stmt_error(raw));
        return BuildBatch(schema, rows, output);
    }

 private:
    MYSQL* db_;
    bool broken_ = false;
};
std::string ConvertSql(std::string_view sql, size_t count, bool* valid) {
    bool single = false, quoted = false;
    size_t parameter = 0;
    std::string out;
    for (size_t i = 0; i < sql.size(); ++i) {
        auto c = sql[i];
        if (c == '\'' && !quoted) {
            if (single && i + 1 < sql.size() && sql[i + 1] == '\'') {
                out += "''";
                ++i;
                continue;
            }
            single = !single;
        } else if (c == '"' && !single)
            quoted = !quoted;
        if (c == '?' && !single && !quoted)
            out += "$" + std::to_string(++parameter);
        else
            out += c;
    }
    *valid = !single && !quoted && parameter == count;
    return out;
}
class PostgresAtomic final : public ServerAtomic {
 public:
    PostgresAtomic(PGconn* db, uint32_t timeout) : ServerAtomic(timeout), db_(db) {
        socket_ = PQsocket(db_);
        PQsetnonblocking(db_, 1);
    }
    ~PostgresAtomic() override { Close(); }

 protected:
    const char* TimeSql() const override {
        return "SELECT CAST(EXTRACT(EPOCH FROM clock_timestamp())*1000 AS BIGINT) AS unix_ms";
    }
    bool Connected() const override { return db_ && PQstatus(db_) == CONNECTION_OK; }
    void Disconnect() override {
        if (db_) PQfinish(db_);
        db_ = nullptr;
    }
    Status Wait(short events, Clock::time_point deadline) {
        while (true) {
            if (cancelled_) return {Code::kCancelled, "cancelled"};
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
            if (ms <= 0) {
                poisoned_ = true;
                shutdown(socket_, SHUT_RDWR);
                return {Code::kTimeout, "database deadline exceeded"};
            }
            pollfd p{socket_, events, 0};
            int rc = poll(&p, 1, static_cast<int>(std::min<int64_t>(ms, 50)));
            if (rc < 0 && errno != EINTR) return {Code::kError, "database poll failed"};
            if (rc > 0) return {};
        }
    }
    Status Run(const char* sql, const DatabaseParameterV1* p, size_t count, uint64_t* affected,
               std::shared_ptr<arrow::RecordBatch>* output) override {
        if (!db_ || cancelled_) return {Code::kCancelled, "connection cancelled"};
        bool valid;
        auto query = ConvertSql(sql, count, &valid);
        if (!valid) return {Code::kInvalidArgument, "parameter count mismatch"};
        std::vector<std::string> values(count);
        std::vector<const char*> pointers(count);
        std::vector<Oid> types(count);
        std::vector<int> lengths(count), formats(count);
        for (size_t i = 0; i < count; ++i) {
            auto& v = p[i];
            switch (v.kind) {
                case DatabaseParameterKindV1::kNull:
                    continue;
                case DatabaseParameterKindV1::kInt64:
                    values[i] = std::to_string(v.int64_value);
                    break;
                case DatabaseParameterKindV1::kUInt64:
                    types[i] = 1700;
                    values[i] = std::to_string(v.uint64_value);
                    break;
                case DatabaseParameterKindV1::kDouble: {
                    types[i] = 701;
                    std::ostringstream stream;
                    stream << std::setprecision(17) << v.double_value;
                    values[i] = stream.str();
                    break;
                }
                case DatabaseParameterKindV1::kString:
                    values[i].assign(v.data ? static_cast<const char*>(v.data) : "", v.size);
                    if (values[i].find('\0') != std::string::npos)
                        return {Code::kInvalidArgument, "NUL text parameter"};
                    types[i] = 25;
                    break;
                case DatabaseParameterKindV1::kBlob:
                    types[i] = 17;
                    pointers[i] = v.data ? static_cast<const char*>(v.data) : "";
                    lengths[i] = v.size;
                    formats[i] = 1;
                    continue;
            }
            pointers[i] = values[i].c_str();
        }
        auto deadline = Clock::now() + std::chrono::milliseconds(timeout_);
        if (!PQsendQueryParams(db_, query.c_str(), count, types.data(), pointers.data(), lengths.data(), formats.data(),
                               0))
            return {Code::kError, PQerrorMessage(db_)};
        int flush;
        while ((flush = PQflush(db_)) == 1) {
            auto s = Wait(POLLOUT, deadline);
            if (!s.ok()) return s;
        }
        if (flush < 0) return {Code::kError, PQerrorMessage(db_)};
        if (!PQsetSingleRowMode(db_)) return {Code::kError, "single row mode failed"};
        std::vector<std::shared_ptr<arrow::Field>> fields;
        std::vector<std::vector<Cell>> rows;
        uint64_t bytes = 0;
        Status status;
        while (true) {
            while (PQisBusy(db_)) {
                auto s = Wait(POLLIN, deadline);
                if (!s.ok()) return s;
                if (!PQconsumeInput(db_)) return {Code::kError, PQerrorMessage(db_)};
            }
            PGresult* raw = PQgetResult(db_);
            if (!raw) break;
            std::unique_ptr<PGresult, decltype(&PQclear)> result(raw, PQclear);
            if (!status.ok()) continue;
            auto code = PQresultStatus(raw);
            if (code == PGRES_COMMAND_OK) {
                if (output) {
                    status = {Code::kError, "selection returned no result"};
                    continue;
                }
                auto text = PQcmdTuples(raw);
                if (affected && text && *text) {
                    auto r = std::from_chars(text, text + std::strlen(text), *affected);
                    if (r.ec != std::errc{}) status = {Code::kError, "affected row overflow"};
                }
                if (std::string_view(sql) == "COMMIT" && std::string_view(PQcmdStatus(raw)) != "COMMIT")
                    status = {Code::kError, "server rolled back transaction"};
                continue;
            }
            if (code != PGRES_SINGLE_TUPLE && code != PGRES_TUPLES_OK) {
                status = {Code::kError, PQresultErrorMessage(raw)};
                continue;
            }
            if (!output) {
                status = {Code::kError, "command returned a result set"};
                continue;
            }
            if (fields.empty())
                for (int i = 0; i < PQnfields(raw); ++i) {
                    auto oid = PQftype(raw, i);
                    auto type = oid == 20 || oid == 21 || oid == 23 ? arrow::int64()
                                : oid == 700 || oid == 701          ? arrow::float64()
                                : oid == 17                         ? arrow::binary()
                                                                    : arrow::utf8();
                    fields.push_back(arrow::field(PQfname(raw, i), type));
                }
            for (int row = 0; row < PQntuples(raw); ++row) {
                std::vector<Cell> cells;
                for (int i = 0; i < PQnfields(raw); ++i) {
                    auto length = PQgetlength(raw, row, i);
                    bytes += 16 + length;
                    if (bytes > kReadLimit) {
                        status = {Code::kError, "atomic result byte limit"};
                        break;
                    }
                    if (PQgetisnull(raw, row, i))
                        cells.emplace_back(std::nullopt);
                    else if (PQftype(raw, i) == 17) {
                        size_t n = 0;
                        auto* data =
                            PQunescapeBytea(reinterpret_cast<const unsigned char*>(PQgetvalue(raw, row, i)), &n);
                        if (!data) {
                            status = {Code::kError, "bytea decode failed"};
                            break;
                        }
                        cells.emplace_back(std::string(reinterpret_cast<const char*>(data), n));
                        PQfreemem(data);
                    } else
                        cells.emplace_back(std::string(PQgetvalue(raw, row, i), length));
                }
                if (status.ok()) rows.push_back(std::move(cells));
            }
        }
        if (!status.ok()) return status;
        return output ? BuildBatch(fields, rows, output) : Status{};
    }

 private:
    PGconn* db_;
};
}  // namespace
DatabaseAtomicStatusV1 MakeServerAtomicSession(const std::string& backend,
                                               const std::unordered_map<std::string, std::string>& p,
                                               const DatabaseAtomicSessionOptionsV1& options,
                                               std::shared_ptr<IDatabaseAtomicSessionV1>* output) {
    if (backend == "mysql") {
        MYSQL* db = mysql_init(nullptr);
        if (!db) return {Code::kError, "connection allocation failed"};
        unsigned int seconds = std::max(1U, (options.operation_timeout_ms + 999) / 1000);

        mysql_options(db, MYSQL_OPT_CONNECT_TIMEOUT, &seconds);
        mysql_options(db, MYSQL_OPT_READ_TIMEOUT, &seconds);
        mysql_options(db, MYSQL_OPT_WRITE_TIMEOUT, &seconds);

        auto charset = Parameter(p, "charset", "utf8mb4");
        mysql_options(db, MYSQL_SET_CHARSET_NAME, charset.c_str());
        unsigned int port = 3306;
        auto port_text = Parameter(p, "port", "3306");
        auto r = std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
        if (r.ec != std::errc{} || port > 65535) {
            mysql_close(db);
            return {Code::kInvalidArgument, "invalid port"};
        }
        if (!mysql_real_connect(db, Parameter(p, "host", "localhost").c_str(), Parameter(p, "user", "root").c_str(),
                                Parameter(p, "password", "").c_str(), Parameter(p, "database", "").c_str(), port,
                                nullptr, CLIENT_FOUND_ROWS)) {
            std::string error = mysql_error(db);
            mysql_close(db);
            return {Code::kError, error};
        }
        *output = std::make_shared<MysqlAtomic>(db, options.operation_timeout_ms);
        return {};
    }
    if (backend == "postgres") {
        auto host = Parameter(p, "host", "127.0.0.1"), port = Parameter(p, "port", "5432"),
             user = Parameter(p, "user", "postgres"), password = Parameter(p, "password", ""),
             database = Parameter(p, "database", "postgres"),
             timeout = std::to_string(std::max(1U, (options.operation_timeout_ms + 999) / 1000));
        const char* keys[] = {"host", "port", "user", "password", "dbname", "connect_timeout", nullptr};
        const char* values[] = {host.c_str(),     port.c_str(),    user.c_str(), password.c_str(),
                                database.c_str(), timeout.c_str(), nullptr};
        PGconn* db = PQconnectdbParams(keys, values, 0);
        if (!db || PQstatus(db) != CONNECTION_OK) {
            std::string error = db ? PQerrorMessage(db) : "connection allocation failed";
            if (db) PQfinish(db);
            return {Code::kError, error};
        }
        *output = std::make_shared<PostgresAtomic>(db, options.operation_timeout_ms);
        return {};
    }
    return {Code::kUnsupported, "backend has no atomic target"};
}
}  // namespace flowsql::database
