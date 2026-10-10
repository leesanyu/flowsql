// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include "atomic_target.h"
#include <arrow/api.h>
#include <arrow/ipc/writer.h>
#include <sqlite3.h>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <memory>
namespace flowsql::database {
namespace {
using Code = DatabaseAtomicCodeV1;
using Status = DatabaseAtomicStatusV1;
using Outcome = DatabaseCommitOutcomeV1;
class MaterializedReader final : public IBatchReader {
 public:
    MaterializedReader(std::shared_ptr<arrow::Schema> schema, std::shared_ptr<arrow::RecordBatch> batch,
                       std::shared_ptr<std::atomic<uint32_t>> readers)
        : readers_(std::move(readers)) {
        ++*readers_;
        auto s = arrow::ipc::SerializeSchema(*schema);
        auto b = arrow::ipc::SerializeRecordBatch(*batch, arrow::ipc::IpcWriteOptions::Defaults());
        if (s.ok() && b.ok()) {
            schema_ = *s;
            if (batch->num_rows()) batch_ = *b;
        } else
            error_ = "atomic reader IPC serialization failed";
    }
    ~MaterializedReader() override { Close(); }
    int GetSchema(const uint8_t** p, size_t* n) override {
        if (!p || !n || cancelled_ || !schema_) return -1;
        *p = schema_->data();
        *n = schema_->size();
        return 0;
    }
    int Next(const uint8_t** p, size_t* n) override {
        if (!p || !n || cancelled_ || closed_ || !error_.empty()) return -1;
        *p = nullptr;
        *n = 0;
        if (read_ || !batch_) return 1;
        read_ = true;
        *p = batch_->data();
        *n = batch_->size();
        return 0;
    }
    void Cancel() override { cancelled_ = true; }
    void Close() override {
        if (!closed_) {
            closed_ = true;
            --*readers_;
        }
    }
    const char* GetLastError() override { return error_.c_str(); }
    void Release() override { delete this; }

 private:
    std::shared_ptr<arrow::Buffer> schema_, batch_;
    std::shared_ptr<std::atomic<uint32_t>> readers_;
    std::atomic<bool> cancelled_{false};
    bool closed_ = false, read_ = false;
    std::string error_;
};
class SqliteAtomic final : public IDatabaseAtomicSessionV1 {
 public:
    SqliteAtomic(sqlite3* db, uint32_t timeout) : db_(db), timeout_(timeout) {
        sqlite3_busy_handler(
            db_,
            [](void* p, int) {
                auto* s = static_cast<SqliteAtomic*>(p);
                if (s->cancelled_ || std::chrono::steady_clock::now() >= s->deadline_) return 0;
                sqlite3_sleep(1);
                return 1;
            },
            this);
        sqlite3_progress_handler(
            db_, 1000,
            [](void* p) {
                auto* s = static_cast<SqliteAtomic*>(p);
                return s->cancelled_ || std::chrono::steady_clock::now() >= s->deadline_ ? 1 : 0;
            },
            this);
    }
    ~SqliteAtomic() override { Close(); }
    Status Begin() override {
        if (auto s = Available(); !s.ok()) return s;
        if (active_) return {Code::kInvalidArgument, "nested transaction"};
        auto s = Command("BEGIN");
        if (s.ok()) active_ = true;
        return s;
    }
    Status ExecutePrepared(const char* sql, const DatabaseParameterV1* p, size_t count, uint64_t* affected) override {
        if (affected) *affected = 0;
        if (!affected || !active_) return {Code::kInvalidArgument, "transaction/affected required"};
        if (auto s = Available(); !s.ok()) return s;
        sqlite3_stmt* raw = nullptr;
        auto s = Prepare(sql, p, count, &raw);
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(raw, sqlite3_finalize);
        if (!s.ok()) return s;
        const auto before = sqlite3_total_changes64(db_);
        auto rc = sqlite3_step(raw);
        if (rc != SQLITE_DONE) return Error(rc);
        *affected = sqlite3_stmt_readonly(raw) || sqlite3_total_changes64(db_) == before
                        ? 0
                        : static_cast<uint64_t>(sqlite3_changes64(db_));
        return {};
    }
    Status CreateReader(const char* sql, const DatabaseParameterV1* p, size_t count, IBatchReader** out) override {
        if (!out) return {Code::kInvalidArgument, "reader required"};
        *out = nullptr;
        if (auto s = Available(); !s.ok()) return s;
        sqlite3_stmt* raw = nullptr;
        auto s = Prepare(sql, p, count, &raw);
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(raw, sqlite3_finalize);
        if (!s.ok()) return s;
        if (!sqlite3_stmt_readonly(raw)) return {Code::kInvalidArgument, "read-only selection required"};
        // SQL logical integers remain exact int64; uint64 decimal TEXT remains exact UTF8.
        std::vector<std::shared_ptr<arrow::Field>> fields;
        std::vector<std::unique_ptr<arrow::ArrayBuilder>> builders;
        int rc = sqlite3_step(raw);
        for (int i = 0; i < sqlite3_column_count(raw); ++i) {
            int t = sqlite3_column_type(raw, i);
            auto type = t == SQLITE_INTEGER ? arrow::int64()
                        : t == SQLITE_FLOAT ? arrow::float64()
                        : t == SQLITE_BLOB  ? arrow::binary()
                                            : arrow::utf8();
            fields.push_back(arrow::field(sqlite3_column_name(raw, i), type));
            builders.push_back(arrow::MakeBuilder(type).ValueOrDie());
        }
        int64_t rows = 0;
        uint64_t bytes = 0;
        while (rc == SQLITE_ROW) {
            for (int i = 0; i < sqlite3_column_count(raw); ++i) {
                arrow::Status st;
                int t = sqlite3_column_type(raw, i);
                bytes += 16 + sqlite3_column_bytes(raw, i);
                if (bytes > 1024ULL * 1024 * 1024) return {Code::kError, "atomic reader byte limit"};
                if (t == SQLITE_NULL)
                    st = builders[i]->AppendNull();
                else
                    switch (fields[i]->type()->id()) {
                        case arrow::Type::INT64:
                            if (t != SQLITE_INTEGER) return {Code::kError, "mixed/lossy integer column"};
                            st = static_cast<arrow::Int64Builder*>(builders[i].get())
                                     ->Append(sqlite3_column_int64(raw, i));
                            break;
                        case arrow::Type::DOUBLE:
                            st = static_cast<arrow::DoubleBuilder*>(builders[i].get())
                                     ->Append(sqlite3_column_double(raw, i));
                            break;
                        case arrow::Type::BINARY:
                            st = static_cast<arrow::BinaryBuilder*>(builders[i].get())
                                     ->Append(static_cast<const uint8_t*>(sqlite3_column_blob(raw, i)),
                                              sqlite3_column_bytes(raw, i));
                            break;
                        default:
                            st = static_cast<arrow::StringBuilder*>(builders[i].get())
                                     ->Append(reinterpret_cast<const char*>(sqlite3_column_text(raw, i)),
                                              sqlite3_column_bytes(raw, i));
                    }
                if (!st.ok()) return {Code::kError, st.ToString()};
            }
            ++rows;
            rc = sqlite3_step(raw);
        }
        if (rc != SQLITE_DONE) return Error(rc);
        std::vector<std::shared_ptr<arrow::Array>> arrays;
        for (auto& b : builders) {
            auto a = b->Finish();
            if (!a.ok()) return {Code::kError, a.status().ToString()};
            arrays.push_back(*a);
        }
        auto schema = arrow::schema(fields);
        *out = new MaterializedReader(schema, arrow::RecordBatch::Make(schema, rows, arrays), readers_);
        return {};
    }
    Status ReadDatabaseTime(int64_t* out) override {
        if (!out) return {Code::kInvalidArgument, "time required"};
        if (auto s = Available(); !s.ok()) return s;
        sqlite3_stmt* raw = nullptr;
        auto s = Prepare("SELECT CAST((julianday('now')-2440587.5)*86400000 AS INTEGER)", nullptr, 0, &raw);
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(raw, sqlite3_finalize);
        if (!s.ok()) return s;
        auto rc = sqlite3_step(raw);
        if (rc != SQLITE_ROW) return Error(rc);
        *out = sqlite3_column_int64(raw, 0);
        return {};
    }
    DatabaseCommitResultV1 Commit() override {
        if (!active_) return {{Code::kInvalidArgument, "active transaction required"}, Outcome::kRolledBack};
        if (*readers_) {
            Deadline();
            if (sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr) == SQLITE_OK) {
                active_ = false;
                return {{Code::kInvalidArgument, "close readers before Commit"}, Outcome::kRolledBack};
            }
            poisoned_ = true;
            return {{Code::kCommitUnknown, "reader lifecycle violation/rollback failed"}, Outcome::kUnknown};
        }
        auto s = Available();
        if (s.ok()) s = Command("COMMIT");
        if (s.ok()) {
            active_ = false;
            return {{}, Outcome::kCommitted};
        }
        // SQLite COMMIT error is known uncommitted while autocommit remains off.
        if (!sqlite3_get_autocommit(db_)) {
            Deadline();
            int rc = sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
            if (rc == SQLITE_OK) {
                active_ = false;
                return {s, Outcome::kRolledBack};
            }
        }
        poisoned_ = true;
        return {{Code::kCommitUnknown, s.message}, Outcome::kUnknown};
    }
    Status Rollback() override {
        if (!db_ || poisoned_ || !active_ || *readers_) return {Code::kError, "rollback unavailable"};
        Deadline();
        auto rc = sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        if (rc != SQLITE_OK) return Error(rc);
        active_ = false;
        return {};
    }
    void Cancel() override {
        cancelled_ = true;
        if (db_) sqlite3_interrupt(db_);
    }
    void Close() override {
        if (!db_) return;
        sqlite3_progress_handler(db_, 0, nullptr, nullptr);
        if (active_ && !poisoned_) sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        sqlite3_close_v2(db_);
        db_ = nullptr;
        active_ = false;
    }

 private:
    void Deadline() { deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_); }
    Status Available() {
        if (!db_ || poisoned_) return {Code::kError, "closed/poisoned session"};
        if (cancelled_) return {Code::kCancelled, "cancelled"};
        Deadline();
        return {};
    }
    Status Error(int rc) {
        auto c = cancelled_                                                             ? Code::kCancelled
                 : (rc == SQLITE_BUSY || rc == SQLITE_LOCKED || rc == SQLITE_INTERRUPT) ? Code::kTimeout
                                                                                        : Code::kError;
        return {c, sqlite3_errmsg(db_)};
    }
    Status Command(const char* sql) {
        Deadline();
        int rc = sqlite3_exec(db_, sql, nullptr, nullptr, nullptr);
        return rc == SQLITE_OK ? Status{} : Error(rc);
    }
    Status Prepare(const char* sql, const DatabaseParameterV1* p, size_t count, sqlite3_stmt** out) {
        if (!sql || (count && !p) || count > INT_MAX) return {Code::kInvalidArgument, "invalid parameters"};
        Deadline();
        const char* tail = nullptr;
        auto rc = sqlite3_prepare_v2(db_, sql, -1, out, &tail);
        if (rc != SQLITE_OK) return Error(rc);
        if (!*out || (tail && *tail) || sqlite3_bind_parameter_count(*out) != static_cast<int>(count))
            return {Code::kInvalidArgument, "single statement/parameter count required"};
        for (size_t i = 0; i < count; ++i) {
            auto& v = p[i];
            int n = i + 1;
            if (v.size > INT_MAX || (v.size && !v.data)) return {Code::kInvalidArgument, "invalid parameter size"};
            switch (v.kind) {
                case DatabaseParameterKindV1::kNull:
                    rc = sqlite3_bind_null(*out, n);
                    break;
                case DatabaseParameterKindV1::kInt64:
                    rc = sqlite3_bind_int64(*out, n, v.int64_value);
                    break;
                case DatabaseParameterKindV1::kUInt64: {
                    auto text = std::to_string(v.uint64_value);
                    rc = sqlite3_bind_text(*out, n, text.c_str(), text.size(), SQLITE_TRANSIENT);
                    break;
                }
                case DatabaseParameterKindV1::kDouble:
                    if (!std::isfinite(v.double_value)) return {Code::kInvalidArgument, "nonfinite parameter"};
                    rc = sqlite3_bind_double(*out, n, v.double_value);
                    break;
                case DatabaseParameterKindV1::kString:
                    rc = sqlite3_bind_text(*out, n, v.data ? static_cast<const char*>(v.data) : "", v.size,
                                           SQLITE_TRANSIENT);
                    break;
                case DatabaseParameterKindV1::kBlob:
                    rc = sqlite3_bind_blob(*out, n, v.data ? v.data : "", v.size, SQLITE_TRANSIENT);
                    break;
                default:
                    return {Code::kInvalidArgument, "invalid parameter kind"};
            }
            if (rc != SQLITE_OK) return Error(rc);
        }
        return {};
    }
    sqlite3* db_;
    uint32_t timeout_;
    std::shared_ptr<std::atomic<uint32_t>> readers_ = std::make_shared<std::atomic<uint32_t>>(0);
    std::atomic<bool> cancelled_{false};
    bool active_ = false, poisoned_ = false;
    std::chrono::steady_clock::time_point deadline_;
};
}  // namespace
IBatchReader* MakeAtomicBufferedReader(std::shared_ptr<arrow::RecordBatch> batch,
                                       std::shared_ptr<std::atomic<uint32_t>> readers) {
    auto schema = batch->schema();
    return new MaterializedReader(schema, std::move(batch), std::move(readers));
}
DatabaseAtomicStatusV1 MakeAtomicSession(const std::string& backend,
                                         const std::unordered_map<std::string, std::string>& params,
                                         const DatabaseAtomicSessionOptionsV1& options,
                                         std::shared_ptr<IDatabaseAtomicSessionV1>* output) {
    if (!output) return {Code::kInvalidArgument, "session required"};
    output->reset();
    if (!ValidDatabaseAtomicSessionOptionsV1(options)) return {Code::kInvalidArgument, "invalid session options"};
    if (backend != "sqlite") return MakeServerAtomicSession(backend, params, options, output);
    auto path = params.find("path");
    if (path == params.end() || path->second == ":memory:")
        return {Code::kUnsupported, "atomic target requires shared durable database"};
    sqlite3* db = nullptr;
    int rc = sqlite3_open_v2(path->second.c_str(), &db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (rc != SQLITE_OK) {
        std::string error = db ? sqlite3_errmsg(db) : "open failed";
        if (db) sqlite3_close_v2(db);
        return {Code::kError, error};
    }
    sqlite3_exec(db, "PRAGMA foreign_keys=ON", nullptr, nullptr, nullptr);
    *output = std::make_shared<SqliteAtomic>(db, options.operation_timeout_ms);
    return {};
}
}  // namespace flowsql::database
