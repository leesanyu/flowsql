// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#ifndef FLOWSQL_BASELINER_DATAFRAME_READER_H_
#define FLOWSQL_BASELINER_DATAFRAME_READER_H_
#include <framework/interfaces/iblock_transform_dataframe_input.h>
#include "snapshot_reader.h"
namespace flowsql::baseliner {
struct DataFrameReadStats {
    uint64_t source_buffer_bytes = 0;
    uint64_t peak_buffer_bytes = 0;  // Source buffers plus bounded Arrow allocations, including sort index/pages.
    uint64_t selected_rows = 0;
};
// Accepts an already captured immutable snapshot. Does not read/write/close the shared source channel.
// Owns its source lease, snapshot, conversion buffers and row index; Next emits independent raw pages.
// Open/Next are serialized; Cancel may run concurrently. Failure is terminal and never reports EOF.
class DataFrameReader {
 public:
    DataFrameReader();
    ~DataFrameReader();
    int Open(TaskConfig config, const BlockDataFrameInputBindingV1& binding,
             std::shared_ptr<arrow::RecordBatch> snapshot);
    int Next(SnapshotPage* output);
    void Cancel();
    const std::string& LastError() const;
    const std::string& SourceFingerprint() const;
    const TaskConfig& Config() const;
    DataFrameReadStats Stats() const;

 private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace flowsql::baseliner
#endif
