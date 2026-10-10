// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include "evaluation.h"
namespace flowsql::baseliner {
int EncodeModelParameters(const ConfigSnapshot& config, const std::vector<ModelParametersRow>& rows, uint64_t max_bytes,
                          std::shared_ptr<arrow::RecordBatch>* output, std::string* error);
int DecodeObservations(const std::shared_ptr<arrow::RecordBatch>& batch, uint64_t max_bytes,
                       std::vector<Observation>* output, std::string* error);
int EncodeResults(const ConfigSnapshot& config, const std::vector<EvaluationRow>& rows, uint64_t max_bytes,
                  std::shared_ptr<arrow::RecordBatch>* output, std::string* error);
int EncodeFusion(const ConfigSnapshot& config, const Observation& input, const RelationRollingResult& relation,
                 uint64_t max_bytes, std::shared_ptr<arrow::RecordBatch>* output, std::string* error);
int EncodeMaintenance(const ConfigSnapshot& config, const std::vector<MaintenanceRow>& rows, uint64_t max_bytes,
                      std::shared_ptr<arrow::RecordBatch>* output, std::string* error);
}  // namespace flowsql::baseliner
