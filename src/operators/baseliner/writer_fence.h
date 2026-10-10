// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <arrow/api.h>
#include <framework/interfaces/idatabase_atomic_target.h>
namespace flowsql::baseliner {
DatabaseParameterV1 TextParameter(const std::string& text);
DatabaseParameterV1 IntParameter(int64_t value);
DatabaseAtomicStatusV1 ReadAtomicBatch(IDatabaseAtomicSessionV1& session, const std::string& sql,
                                       const std::vector<DatabaseParameterV1>& parameters,
                                       std::shared_ptr<arrow::RecordBatch>* batch);
// Serialized, task-private fencing. Storage performs publication in the transaction opened by Stage.
class WriterFence {
 public:
    ~WriterFence();
    int Acquire(std::shared_ptr<IDatabaseAtomicSessionV1> session, const std::string& backend, std::string task,
                std::string owner, std::string fingerprint, int64_t lease_ms);
    int Renew();
    int Stage();
    int Confirm(const DatabaseCommitResultV1& result);
    void Release();
    int64_t Generation() const { return generation_; }
    int64_t Epoch() const { return epoch_; }
    const std::string& LastError() const { return error_; }
    IDatabaseAtomicSessionV1& Session() { return *session_; }

 private:
    int Fail(std::string error);
    int Change(bool publish);
    std::shared_ptr<IDatabaseAtomicSessionV1> session_;
    std::string task_, owner_, error_;
    int64_t epoch_ = 0, generation_ = 0, lease_ms_ = 0;
    bool staged_ = false, failed_ = false;
};
}  // namespace flowsql::baseliner
