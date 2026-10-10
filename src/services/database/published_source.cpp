// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include "published_source.h"
#include <arrow/api.h>
#include <atomic>
#include <charconv>
#include <chrono>
#include <stdexcept>

namespace flowsql::database {
namespace {
class NpmPublishedReader final : public IDatabasePublishedProgressReaderV1 {
 public:
    NpmPublishedReader(IDatabaseChannel* source, DatabaseSnapshotOptionsV1 options)
        : source_(source), options_(options) {}
    int ReadProgress(const PublishedDatasetRequestV1& request, const std::string& position,
                     PublishedDatasetProgressV1* output) override {
        if (!output) return -1;
        *output = {};
        if (cancelled_) return Fail("published source cancelled");
        if (request.dataset_id.empty() || request.relation.empty() || request.run_id.empty() ||
            request.bucket_ns <= 0 || !request.npm_period_increment)
            return Fail("NPM publication requires an explicit run and period-increment relation");
        auto* capability = dynamic_cast<IDatabaseSnapshotSourceV1*>(source_);
        std::shared_ptr<IDatabaseSnapshotSessionV1> session;
        if (!capability || capability->CreateSnapshotSession(options_, &session) != 0)
            return Fail(source_->GetLastError());
        std::atomic_store(&active_, session);
        struct Clear {
            std::shared_ptr<IDatabaseSnapshotSessionV1>* active;
            ~Clear() { std::atomic_store(active, std::shared_ptr<IDatabaseSnapshotSessionV1>{}); }
        } clear{&active_};
        if (cancelled_) {
            session->Cancel();
            return Fail("published source cancelled");
        }
        try {
            const std::string backend = source_->Category();
            const char q = backend == "mysql" || backend == "clickhouse" ? '`' : '"';
            auto quote = [q](const std::string& name) {
                std::string value(1, q);
                for (char c : name) {
                    if (c == '\0') throw std::runtime_error("NUL identifier");
                    value += c;
                    if (c == q) value += q;
                }
                return value + q;
            };
            auto relation = [&](const std::string& name) {
                return (request.schema.empty() ? "" : quote(request.schema) + ".") + quote(name);
            };
            auto text = [&](const std::string& name) {
                return (backend == "mysql" ? "BINARY " : "") + quote(name) +
                       (backend == "sqlite"     ? " COLLATE BINARY"
                        : backend == "postgres" ? " COLLATE \"C\""
                                                : "");
            };
            DatabaseParameterV1 run;
            run.kind = DatabaseParameterKindV1::kString;
            run.data = request.run_id.data();
            run.size = request.run_id.size();
            auto read = [&](const std::string& sql, const std::vector<DatabaseParameterV1>& values,
                            std::shared_ptr<arrow::Schema> schema) {
                std::shared_ptr<arrow::RecordBatch> rows;
                if (session->ReadPage(sql.c_str(), values.data(), values.size(), schema, 2, 65536, &rows) != 0)
                    throw std::runtime_error("publication capability unavailable: " + session->LastError());
                return rows;
            };
            auto runs = read(
                "SELECT status,metadata_status,expires_at_ns FROM " + relation("npm_result_runs") + " WHERE " +
                    text("run_id") + "=? LIMIT 2",
                {run},
                arrow::schema({arrow::field("status", arrow::utf8()), arrow::field("metadata_status", arrow::utf8()),
                               arrow::field("expires_at_ns", arrow::int64())}));
            const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
            if (runs->num_rows() != 1 || runs->column(0)->IsNull(0) || runs->column(1)->IsNull(0))
                throw std::runtime_error("NPM run missing or ambiguous");
            auto string = [](const std::shared_ptr<arrow::Array>& array, int64_t row) {
                if (array->IsNull(row)) throw std::runtime_error("NULL publication field");
                return std::static_pointer_cast<arrow::StringArray>(array)->GetString(row);
            };
            const auto status = string(runs->column(0), 0);
            if ((status != "writing" && status != "completed") || string(runs->column(1), 0) != "known" ||
                (!runs->column(2)->IsNull(0) &&
                 std::static_pointer_cast<arrow::Int64Array>(runs->column(2))->Value(0) <= now))
                throw std::runtime_error("NPM run incomplete, expired or purging");
            DatabaseParameterV1 selected_relation;
            selected_relation.kind = DatabaseParameterKindV1::kString;
            selected_relation.data = request.relation.data();
            selected_relation.size = request.relation.size();
            std::vector<DatabaseParameterV1> values{run, selected_relation};
            std::string sql = "SELECT epoch,position,period_ns,closed_before_ns FROM " +
                              relation("npm_result_progress_v1") + " WHERE " + text("run_id") + "=? AND " +
                              text("relation_name") + "=? AND entity_id='basic'";
            if (!position.empty()) {
                int64_t n;
                auto parsed = std::from_chars(position.data(), position.data() + position.size(), n);
                if (parsed.ec != std::errc{} || parsed.ptr != position.data() + position.size() || n <= 0 ||
                    std::to_string(n) != position)
                    throw std::runtime_error("invalid publication position");
                DatabaseParameterV1 p;
                p.kind = DatabaseParameterKindV1::kInt64;
                p.int64_value = n;
                values.push_back(p);
                sql += " AND position=?";
            }
            auto rows =
                read(sql + " ORDER BY position DESC LIMIT 2", values,
                     arrow::schema({arrow::field("epoch", arrow::utf8()), arrow::field("position", arrow::int64()),
                                    arrow::field("period_ns", arrow::int64()),
                                    arrow::field("closed_before_ns", arrow::int64())}));
            if (!rows->num_rows() || (!position.empty() && rows->num_rows() != 1))
                throw std::runtime_error("missing NPM publication capability/position");
            auto integer = [&](int column, int row) {
                if (rows->column(column)->IsNull(row)) throw std::runtime_error("NULL publication field");
                return std::static_pointer_cast<arrow::Int64Array>(rows->column(column))->Value(row);
            };
            const int64_t n = integer(1, 0), period = integer(2, 0), closed = integer(3, 0);
            if (n <= 0 || period <= 0 || closed < 0 || closed % period || request.bucket_ns % period ||
                string(rows->column(0), 0) != request.run_id || (rows->num_rows() == 2 && integer(1, 1) >= n))
                throw std::runtime_error("incompatible or ambiguous NPM publication record");
            if (cancelled_) return Fail("published source cancelled");
            output->progress = {request.dataset_id, request.run_id, std::to_string(n), closed / request.bucket_ns};
            output->source_period_ns = period;
            return 0;
        } catch (const std::exception& ex) {
            return Fail(ex.what());
        }
    }
    void Cancel() override {
        cancelled_ = true;
        auto active = std::atomic_load(&active_);
        if (active) active->Cancel();
    }
    std::string LastError() const override { return error_; }

 private:
    int Fail(std::string error) {
        error_ = std::move(error);
        return -1;
    }
    IDatabaseChannel* source_;
    DatabaseSnapshotOptionsV1 options_;
    std::shared_ptr<IDatabaseSnapshotSessionV1> active_;
    std::atomic<bool> cancelled_{false};
    std::string error_;
};
}  // namespace
std::shared_ptr<IDatabasePublishedProgressReaderV1> MakeNpmPublishedProgressReader(IDatabaseChannel* source,
                                                                                   DatabaseSnapshotOptionsV1 options) {
    options.consistent_transaction = std::string(source->Category()) != "clickhouse";
    return std::make_shared<NpmPublishedReader>(source, options);
}
}  // namespace flowsql::database
