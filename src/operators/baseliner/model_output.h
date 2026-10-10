// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <atomic>
#include "baseliner_contract.h"
#include "writer_fence.h"
namespace flowsql::baseliner {
// Standalone model/results tables, independent of the durable generation/checkpoint protocol.
class ModelOutputStore {
 public:
    ~ModelOutputStore() { Close(); }
    int Open(const ConfigSnapshot& config, std::shared_ptr<IDatabaseChannel> channel, std::string table,
             SchemaKind kind = SchemaKind::kModelParameters);
    int ValidateRelations(IDatabaseChannel* source, const std::vector<Dataset>& datasets);
    int Write(const std::shared_ptr<arrow::RecordBatch>& batch);
    // A matching actual database shares one session; distinct tables remain independent.
    int Join(ModelOutputStore& model);
    bool SharesSession(const ModelOutputStore& model) const;
    int Write(const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches, ModelOutputStore* model = nullptr,
              const std::shared_ptr<arrow::RecordBatch>& parameters = nullptr);
    void Cancel();
    void Close();
    int64_t RowsWritten() const { return rows_written_; }
    const std::string& State() const { return state_; }
    const std::string& DefaultSchema() const { return default_schema_; }
    const std::string& LastError() const { return error_; }

 private:
    int Fail(std::string error);
    int ValidateSchema();
    int Insert(const std::shared_ptr<arrow::RecordBatch>& batch);
    std::string Quote(const std::string& id) const;
    std::string Type(const arrow::DataType& type) const;
    std::shared_ptr<IDatabaseChannel> channel_;  // Must outlive the task-private session.
    std::shared_ptr<IDatabaseAtomicSessionV1> session_;
    std::string backend_, table_, default_schema_, database_identity_, error_, state_ = "pending";
    uint64_t max_bytes_ = 0;
    int64_t rows_written_ = 0;
    SchemaKind kind_ = SchemaKind::kModelParameters;
    bool owns_session_ = true;
    std::atomic<bool> cancelled_{false};
};
}  // namespace flowsql::baseliner
