// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <atomic>
#include "baseliner_contract.h"
#include "writer_fence.h"
namespace flowsql::baseliner {
// Exact config hash remains in result/model rows; only the restart policy is excluded here.
std::string RestoreCompatibilityHash(const ConfigSnapshot& config);
struct StoredGeneration {
    int64_t generation = 0;
    std::string checkpoint;
    BlockInputProgressV1 progress;
};
class DurableStore {
 public:
    int Open(const ConfigSnapshot& config, IDatabaseChannel* channel, const std::string& owner);
    int Load(StoredGeneration* output);
    int Publish(const std::string& checkpoint, const BlockInputProgressV1& progress,
                const std::vector<std::shared_ptr<arrow::RecordBatch>>& results);
    int Renew() { return fence_.Renew() == 0 ? 0 : Fail(fence_.LastError()); }
    void Close() {
        fence_.Release();
        std::atomic_store(&session_, std::shared_ptr<IDatabaseAtomicSessionV1>{});
    }
    void Cancel() {
        cancelled_ = true;
        auto s = std::atomic_load(&session_);
        if (s) s->Cancel();
    }
    int64_t Generation() const { return fence_.Generation(); }
    const std::string& LastError() const { return error_; }

 private:
    int Fail(std::string error);
    int Initialize();
    int InsertResults(const arrow::RecordBatch& results, int64_t generation);
    std::string Quote(const std::string& id) const;
    ConfigSnapshot config_;
    std::string backend_, error_;
    std::shared_ptr<IDatabaseAtomicSessionV1> session_;
    WriterFence fence_;
    std::atomic<bool> cancelled_{false};
    bool failed_ = false;
};
}  // namespace flowsql::baseliner
