// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include "snapshot_reader.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <set>
#include <stdexcept>

namespace flowsql::baseliner {
namespace {
std::string Quote(const std::string& name, const std::string& backend) {
    const char q = backend == "mysql" || backend == "clickhouse" ? '`' : '"';
    std::string out(1, q);
    for (char c : name) {
        if (c == '\0') throw std::runtime_error("NUL identifier");
        out += c;
        if (c == q) out += c;
    }
    return out + q;
}
std::vector<DatabaseParameterV1> Parameters(const std::vector<KeyValue>& values) {
    std::vector<DatabaseParameterV1> out;
    for (const auto& value : values) {
        DatabaseParameterV1 p;
        std::visit(
            [&](const auto& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, std::string>) {
                    p.kind = DatabaseParameterKindV1::kString;
                    p.data = v.data();
                    p.size = v.size();
                } else if constexpr (std::is_same_v<T, uint64_t>) {
                    p.kind = DatabaseParameterKindV1::kUInt64;
                    p.uint64_value = v;
                } else if constexpr (std::is_same_v<T, double>) {
                    p.kind = DatabaseParameterKindV1::kDouble;
                    p.double_value = v;
                } else {
                    p.kind = DatabaseParameterKindV1::kInt64;
                    p.int64_value = v;
                }
            },
            value);
        out.push_back(p);
    }
    return out;
}
KeyValue Cell(const std::shared_ptr<arrow::Array>& a, int64_t row) {
    if (a->IsNull(row)) throw std::runtime_error("NULL pagination identity");
    switch (a->type_id()) {
        case arrow::Type::BOOL:
            return std::static_pointer_cast<arrow::BooleanArray>(a)->Value(row);
        case arrow::Type::INT64:
            return std::static_pointer_cast<arrow::Int64Array>(a)->Value(row);
        case arrow::Type::UINT64:
            return std::static_pointer_cast<arrow::UInt64Array>(a)->Value(row);
        case arrow::Type::DOUBLE: {
            double v = std::static_pointer_cast<arrow::DoubleArray>(a)->Value(row);
            if (!std::isfinite(v)) throw std::runtime_error("nonfinite pagination identity");
            return v;
        }
        case arrow::Type::STRING:
            return std::static_pointer_cast<arrow::StringArray>(a)->GetString(row);
        default:
            throw std::runtime_error("unsupported pagination identity");
    }
}
std::string Join(const std::vector<std::string>& parts) {
    std::string result;
    for (const auto& part : parts) {
        if (!result.empty()) result += ",";
        result += part;
    }
    return result;
}
}  // namespace
std::shared_ptr<arrow::DataType> ArrowType(LogicalType type) {
    switch (type) {
        case LogicalType::kBoolean:
            return arrow::boolean();
        case LogicalType::kInt64:
            return arrow::int64();
        case LogicalType::kUInt64:
            return arrow::uint64();
        case LogicalType::kFloat64:
            return arrow::float64();
        case LogicalType::kUtf8:
            return arrow::utf8();
    }
    return nullptr;
}
int64_t BucketWidth(const TaskConfig& config, TimeUnit unit) {
    int64_t factor = unit == TimeUnit::kNs   ? 1000000000
                     : unit == TimeUnit::kUs ? 1000000
                     : unit == TimeUnit::kMs ? 1000
                                             : 1;
    if (unit == TimeUnit::kBucketId) return 1;
    if (config.bucket_seconds <= 0 || config.bucket_seconds > std::numeric_limits<int64_t>::max() / factor)
        throw std::runtime_error("bucket width overflow");
    return config.bucket_seconds * factor;
}
int64_t ToBucket(int64_t time, int64_t width) { return time / width - (time % width < 0 ? 1 : 0); }
int SnapshotReader::Read(Plan& plan, uint32_t limit, std::shared_ptr<arrow::RecordBatch>* batch) {
    const std::string sql = plan.sql + " LIMIT " + std::to_string(limit);
    auto parameters = Parameters(plan.values);
    if (session_->ReadPage(sql.c_str(), parameters.data(), parameters.size(), plan.schema, limit,
                           config_.read.max_pending_bytes, batch) != 0) {
        error_ = session_->LastError();
        return -1;
    }
    return 0;
}
int SnapshotReader::ReadNext(Plan& plan, std::shared_ptr<arrow::RecordBatch>* batch) {
    try {
        for (;;) {
            Plan query = plan;
            if (!plan.cursor.empty()) {
                std::vector<std::string> placeholders(plan.keys.size(), "?");
                if (plan.reading_ties) {
                    for (size_t i = 0; i < plan.keys.size(); ++i) {
                        query.sql += " AND " + plan.ordered_keys[i] + "=?";
                        query.values.push_back(plan.cursor[i]);
                    }
                } else {
                    // Row comparison has the same lexicographic ordering as ORDER BY.
                    if (plan.keys.size() == 1)
                        query.sql += " AND " + plan.ordered_keys[0] + ">?";
                    else
                        query.sql += " AND (" + Join(plan.ordered_keys) + ")>(" + Join(placeholders) + ")";
                    query.values.insert(query.values.end(), plan.cursor.begin(), plan.cursor.end());
                }
            }
            query.sql += " ORDER BY " + (plan.reading_ties ? plan.tie_order : Join(plan.ordered_keys));
            if (plan.reading_ties) {
                query.sql +=
                    " LIMIT " + std::to_string(config_.read.page_rows) + " OFFSET " + std::to_string(plan.tie_offset);
                auto parameters = Parameters(query.values);
                if (session_->ReadPage(query.sql.c_str(), parameters.data(), parameters.size(), plan.schema,
                                       config_.read.page_rows, config_.read.max_pending_bytes, batch) != 0) {
                    error_ = session_->LastError();
                    return -1;
                }
                plan.tie_offset += (*batch)->num_rows();
                if ((*batch)->num_rows() < config_.read.page_rows) plan.reading_ties = false;
                if ((*batch)->num_rows()) return 0;
                continue;
            }
            if (Read(query, config_.read.page_rows, batch) != 0) return -1;
            const int64_t rows = (*batch)->num_rows();
            if (!rows) return 0;
            auto key = [&](int64_t row) {
                std::vector<KeyValue> result;
                for (const auto& name : plan.keys) result.push_back(Cell((*batch)->GetColumnByName(name), row));
                return result;
            };
            auto last = key(rows - 1);
            int64_t tail = rows - 1;
            while (tail > 0 && key(tail - 1) == last) --tail;
            plan.cursor = std::move(last);
            if (rows == config_.read.page_rows) {
                // Do not publish a partially read equal-key group. Read that group separately;
                // only this exceptional duplicate group needs an OFFSET and payload ordering.
                plan.reading_ties = true;
                plan.tie_offset = 0;
                *batch = (*batch)->Slice(0, tail);
                if (!tail) continue;
            }
            return 0;
        }
    } catch (const std::exception& ex) {
        error_ = ex.what();
        batch->reset();
        return -1;
    }
}
bool MatchesDatabaseSource(const TaskConfig& config, IDatabaseChannel* source) {
    if (!source || !config.database_source || config.dataframe_source) return false;
    const auto channel = std::string(source->Category()) + "." + source->Name();
    if (config.source_relation.empty()) return config.source == channel;
    return config.source == channel + "." + config.source_relation && config.datasets.size() == 1 &&
           config.datasets[0].table == config.source_relation;
}
int SnapshotReader::Open(TaskConfig config, std::shared_ptr<IDatabaseChannel> source, std::string_view exact_source) {
    if (opened_ || cancelled_ || !source || !source->IsOpened() || !source->IsConnected() ||
        config.mode != Mode::kSnapshot || !config.database_source || config.source != exact_source ||
        !MatchesDatabaseSource(config, source.get())) {
        error_ = "invalid finite database source";
        return -1;
    }
    struct Cleanup {
        std::function<void()> action;
        ~Cleanup() { action(); }
    } cleanup{[this] {
        if (!opened_) {
            std::atomic_store(&session_, std::shared_ptr<IDatabaseSnapshotSessionV1>{});
            source_.reset();
            plans_.clear();
        }
    }};
    config_ = std::move(config);
    config_.state.idle_timeout_ms = 0;
    source_ = std::move(source);
    auto* capability = dynamic_cast<IDatabaseSnapshotSourceV1*>(source_.get());
    if (!capability) {
        error_ = "source lacks snapshot capability";
        return -1;
    }
    const std::string backend = source_->Category();
    DatabaseSnapshotOptionsV1 options;
    options.operation_timeout_ms = config_.persistence.operation_timeout_ms;
    options.consistent_transaction = backend != "clickhouse";
    if (backend == "clickhouse" && std::any_of(config_.datasets.begin(), config_.datasets.end(), [](const auto& d) {
            return d.scope.consistency == "consistent_snapshot";
        })) {
        error_ = "ClickHouse consistent_snapshot unsupported";
        return -1;
    }
    std::shared_ptr<IDatabaseSnapshotSessionV1> created;
    if (capability->CreateSnapshotSession(options, &created) != 0) {
        error_ = source_->GetLastError();
        return -1;
    }
    std::atomic_store(&session_, created);
    if (cancelled_) {
        created->Cancel();
        error_ = "snapshot reader cancelled during open";
        return -1;
    }
    try {
        for (auto& d : config_.datasets) {
            if (d.row_semantics == "npm_period_increment") {
                const std::map<std::string, LogicalType> required{
                    {"period_start_ns", LogicalType::kInt64},      {"period_end_ns", LogicalType::kInt64},
                    {"period_complete", LogicalType::kBoolean},    {"is_final", LogicalType::kBoolean},
                    {"interval_packets_ab", LogicalType::kUInt64}, {"interval_packets_ba", LogicalType::kUInt64}};
                for (const auto& [name, type] : required) {
                    auto [it, inserted] = d.fields.emplace(name, type);
                    if (!inserted && it->second != type) throw std::runtime_error("NPM canonical field type: " + name);
                }
                if (d.bucket_column != "period_start_ns" || d.unit != TimeUnit::kNs)
                    throw std::runtime_error("NPM increments require period_start_ns in ns");
            }
            auto quote = [&](const std::string& n) { return Quote(n, backend); };
            Plan plan;
            std::vector<std::shared_ptr<arrow::Field>> fields;
            plan.sql = "SELECT ";
            for (const auto& [name, type] : d.fields) {
                if (!fields.empty()) plan.sql += ",";
                plan.sql += quote(name);
                fields.push_back(arrow::field(name, ArrowType(type)));
            }
            plan.schema = arrow::schema(fields);
            const size_t selection_end = plan.sql.size();
            plan.sql += " FROM " + (d.schema.empty() ? "" : quote(d.schema) + ".") + quote(d.table) + " WHERE 1=1";
            if (!d.scope.run_ids.empty()) {
                if (!d.fields.count("__npm_run_id")) throw std::runtime_error("run scope requires __npm_run_id field");
                plan.sql += " AND " + quote("__npm_run_id") + " IN (";
                for (size_t i = 0; i < d.scope.run_ids.size(); ++i) {
                    if (i) plan.sql += ",";
                    plan.sql += "?";
                    plan.values.emplace_back(d.scope.run_ids[i]);
                }
                plan.sql += ")";
            }
            if (d.scope.begin_bucket) {
                const auto width = BucketWidth(config_, d.unit);
                auto bound = [&](int64_t bucket) {
                    if (bucket > std::numeric_limits<int64_t>::max() / width ||
                        bucket < std::numeric_limits<int64_t>::min() / width)
                        throw std::runtime_error("snapshot range conversion overflow");
                    return bucket * width;
                };
                plan.sql += " AND " + quote(d.bucket_column) + ">=? AND " + quote(d.bucket_column) + "<?";
                plan.values.emplace_back(bound(*d.scope.begin_bucket));
                plan.values.emplace_back(bound(*d.scope.end_bucket));
            }
            for (const auto& f : d.filter) {
                const std::map<std::string, std::string> ops{{"eq", "="},  {"ne", "<>"}, {"lt", "<"},
                                                             {"le", "<="}, {"gt", ">"},  {"ge", ">="}};
                std::string column = quote(f.column);
                if (d.fields.at(f.column) == LogicalType::kUtf8) {
                    if (backend == "mysql")
                        column = "BINARY " + column;
                    else if (backend == "postgres")
                        column += " COLLATE \"C\"";
                    else if (backend == "sqlite")
                        column += " COLLATE BINARY";
                }
                if (backend == "sqlite" && d.fields.at(f.column) == LogicalType::kUInt64) {
                    plan.sql += " AND (" + column + " IS NOT NULL AND printf('%020s',ltrim(" + column +
                                ",'0')) COLLATE BINARY" + ops.at(f.op) + "printf('%020s',ltrim(?,'0')) COLLATE BINARY)";
                } else
                    plan.sql += " AND " + column + ops.at(f.op) + "?";
                plan.values.push_back(f.value);
            }
            auto ordered = [&](const std::string& name) {
                std::string column = quote(name);
                if (d.fields.at(name) == LogicalType::kUtf8) {
                    if (backend == "mysql")
                        column = "BINARY " + column;
                    else if (backend == "postgres")
                        column += " COLLATE \"C\"";
                    else if (backend == "sqlite")
                        column += " COLLATE BINARY";
                }
                return column;
            };
            if (d.row_semantics == "latest_revision") {
                std::string rank = "__baseliner_rank";
                while (d.fields.count(rank)) rank += "_";
                std::vector<std::string> partition;
                for (const auto& key : d.deduplicate_keys) partition.push_back(ordered(key));
                std::string revision = quote(d.revision_column);
                std::string order =
                    backend == "sqlite" ? "length(" + revision + ") DESC," + revision + " DESC" : revision + " DESC";
                const auto from = selection_end;
                plan.sql = plan.sql.substr(0, from) + " FROM (" + plan.sql.substr(0, from) +
                           ",DENSE_RANK() OVER(PARTITION BY " + Join(partition) + " ORDER BY " + order + ") AS " +
                           quote(rank) + plan.sql.substr(from) + ") AS ranked WHERE " + quote(rank) + "=1";
            }
            plan.keys = {d.bucket_column};
            for (const auto& name : d.deduplicate_keys)
                if (std::find(plan.keys.begin(), plan.keys.end(), name) == plan.keys.end()) plan.keys.push_back(name);
            if (!d.revision_column.empty() &&
                std::find(plan.keys.begin(), plan.keys.end(), d.revision_column) == plan.keys.end())
                plan.keys.push_back(d.revision_column);
            for (const auto& name : plan.keys) plan.ordered_keys.push_back(ordered(name));
            std::vector<std::string> ties = plan.ordered_keys;
            for (const auto& [name, type] : d.fields)
                if (std::find(plan.keys.begin(), plan.keys.end(), name) == plan.keys.end())
                    ties.push_back(ordered(name));
            plan.tie_order = Join(ties);
            std::shared_ptr<arrow::RecordBatch> empty;
            if (Read(plan, 0, &empty) != 0) return -1;
            if (ValidateRuns(d) != 0) return -1;
            plans_.push_back(std::move(plan));
        }
        opened_ = true;
        return 0;
    } catch (const std::exception& ex) {
        error_ = ex.what();
        return -1;
    }
}
int SnapshotReader::ValidateRuns(const Dataset& dataset) {
    if (dataset.scope.consistency != "npm_completed") return 0;
    const std::string backend = source_->Category();
    auto quote = [&](const std::string& n) { return Quote(n, backend); };
    try {
        if (dataset.scope.run_ids.empty()) throw std::runtime_error("completed run IDs required");
        Plan runs;
        runs.schema =
            arrow::schema({arrow::field("status", arrow::utf8()), arrow::field("metadata_status", arrow::utf8()),
                           arrow::field("expires_at_ns", arrow::int64())});
        runs.sql = "SELECT " + quote("status") + "," + quote("metadata_status") + "," + quote("expires_at_ns") +
                   " FROM " + (dataset.schema.empty() ? "" : quote(dataset.schema) + ".") + quote("npm_result_runs") +
                   " WHERE " + quote("run_id") + "=?";
        for (const auto& id : dataset.scope.run_ids) {
            runs.values = {id};
            std::shared_ptr<arrow::RecordBatch> run;
            if (Read(runs, 2, &run) != 0) return -1;
            const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
            if (run->num_rows() != 1 || run->column(0)->IsNull(0) || run->column(1)->IsNull(0) ||
                std::static_pointer_cast<arrow::StringArray>(run->column(0))->GetString(0) != "completed" ||
                std::static_pointer_cast<arrow::StringArray>(run->column(1))->GetString(0) != "known" ||
                (!run->column(2)->IsNull(0) &&
                 std::static_pointer_cast<arrow::Int64Array>(run->column(2))->Value(0) <= now))
                throw std::runtime_error("NPM run incomplete, expired, missing or purging: " + id);
        }
        return 0;
    } catch (const std::exception& ex) {
        error_ = ex.what();
        return -1;
    }
}

int SnapshotReader::Next(SnapshotPage* output) {
    if (!output) return -1;
    *output = {};
    if (!opened_ || cancelled_) {
        error_ = "snapshot closed/cancelled";
        return -1;
    }
    if (dataset_ == plans_.size()) {
        output->eof = true;
        return 0;
    }
    auto& plan = plans_[dataset_];
    output->dataset_index = dataset_;
    if (ValidateRuns(config_.datasets[dataset_]) != 0) return -1;
    if (ReadNext(plan, &output->batch) != 0) return -1;
    if (output->batch->num_rows() == 0) {
        output->eof = true;
        ++dataset_;
    }
    return 0;
}
void SnapshotReader::Cancel() {
    cancelled_ = true;
    auto session = std::atomic_load(&session_);
    if (session) session->Cancel();
}
}  // namespace flowsql::baseliner
