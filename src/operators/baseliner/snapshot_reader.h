// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#ifndef FLOWSQL_BASELINER_SNAPSHOT_READER_H_
#define FLOWSQL_BASELINER_SNAPSHOT_READER_H_
#include <arrow/api.h>
#include <framework/interfaces/idatabase_snapshot_source.h>
#include <atomic>
#include "baseliner_contract.h"

namespace flowsql::baseliner {
// Exact SQL source selects a relation; the lease identifies its database channel.
bool MatchesDatabaseSource(const TaskConfig& config, IDatabaseChannel* source);
struct SnapshotPage {
    size_t dataset_index = 0;
    std::shared_ptr<arrow::RecordBatch> batch;
    bool eof = false;
};
// Finite raw-page reader. Owns config, source lease and exclusive session; no TTL or algorithm calls.
// Open validates all configured selections before Next; Cancel may run concurrently.
class SnapshotReader {
 public:
    int Open(TaskConfig config, std::shared_ptr<IDatabaseChannel> source, std::string_view exact_source);
    int Next(SnapshotPage* output);
    void Cancel();
    const std::string& LastError() const { return error_; }
    const TaskConfig& Config() const { return config_; }

 private:
    struct Plan {
        std::string sql;
        std::vector<KeyValue> values;
        std::shared_ptr<arrow::Schema> schema;
        std::vector<std::string> keys;
        std::vector<std::string> ordered_keys;
        std::string tie_order;
        std::vector<KeyValue> cursor;
        uint64_t tie_offset = 0;
        bool reading_ties = false;
    };
    int ValidateRuns(const Dataset& dataset);
    int Read(Plan& plan, uint32_t limit, std::shared_ptr<arrow::RecordBatch>* batch);
    int ReadNext(Plan& plan, std::shared_ptr<arrow::RecordBatch>* batch);
    TaskConfig config_;
    std::shared_ptr<IDatabaseChannel> source_;
    std::shared_ptr<IDatabaseSnapshotSessionV1> session_;
    std::vector<Plan> plans_;
    size_t dataset_ = 0;
    bool opened_ = false;
    std::atomic<bool> cancelled_{false};
    std::string error_;
};
int64_t BucketWidth(const TaskConfig& config, TimeUnit unit);
int64_t ToBucket(int64_t time, int64_t width);
std::shared_ptr<arrow::DataType> ArrowType(LogicalType type);
}  // namespace flowsql::baseliner
#endif
