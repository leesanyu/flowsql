// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <arrow/record_batch.h>
#include <framework/interfaces/ibaseline_types.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace flowsql::baseliner {
// Internal contract only. Batches never cross a callback, Schema, or timestamp boundary.
struct ResultBatchLimits {
    uint32_t max_rows = 4096;
};
struct ResultBatchStats {
    uint64_t result_rows = 0;
    uint64_t result_batches = 0;
    uint64_t encoded_bytes = 0;
    uint64_t peak_staging_bytes = 0;
};
struct ConfigSnapshot;
struct EvaluationRow;
struct Observation;
class ResultBatchEncoder {
 public:
    ResultBatchEncoder(const ConfigSnapshot& config, uint64_t max_bytes, ResultBatchLimits limits = {});
    ~ResultBatchEncoder();
    int Append(const EvaluationRow& row, std::vector<std::shared_ptr<arrow::RecordBatch>>* batches, std::string* error);
    int AppendFusion(const Observation& input, const RelationRollingResult& relation,
                     std::vector<std::shared_ptr<arrow::RecordBatch>>* batches, std::string* error);
    int Flush(std::vector<std::shared_ptr<arrow::RecordBatch>>* batches, std::string* error);
    ResultBatchStats Stats() const;

 private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace flowsql::baseliner
