// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <arrow/memory_pool.h>
#include <operators/npm_basic/npm_protocol_contract.h>
#include <atomic>
#include <functional>

namespace flowsql::npm {

/** Task-private synchronous router. Only observing rows survive Emit. */
class NpmResultRouter final {
 public:
    using Invoke = std::function<int(const std::function<int()>&)>;
    NpmResultRouter(std::vector<NpmEntityDescriptorV1> entities, std::string observing, NpmResultContextV1 context,
                    std::shared_ptr<INpmTaskBudget> budget, std::unique_ptr<INpmResultConsumerV1> consumer,
                    Invoke invoke);
    ~NpmResultRouter();
    int Emit(std::string_view module, std::string_view entity, const arrow::RecordBatch& rows);
    int Drain(std::shared_ptr<arrow::RecordBatch>* output);
    int Finish();
    int FailRun(int32_t code, std::string stage, std::string message) noexcept;
    std::string ResultJson() const;
    int Reject(int code, std::string message) { return Fail(code, std::move(message)); }
    void Cancel() noexcept;
    void Discard() noexcept;
    size_t pending_results() const noexcept { return pending_rows_; }
    arrow::MemoryPool* pool() const { return pool_.get(); }
    const std::string& LastError() const { return error_; }
    const NpmResultContextV1& Context() const { return context_; }

 private:
    int Fail(int code, std::string message);
    std::vector<NpmEntityDescriptorV1> entities_;
    std::string observing_;
    NpmResultContextV1 context_;
    std::shared_ptr<arrow::MemoryPool> pool_;
    std::unique_ptr<INpmResultConsumerV1> consumer_;
    Invoke invoke_;
    std::vector<std::shared_ptr<arrow::RecordBatch>> pending_;
    size_t pending_rows_ = 0;
    int error_code_ = 0;
    std::string error_;
    std::atomic<bool> cancelled_{false};
    std::atomic<bool> finished_{false};
    std::atomic<bool> failure_finalized_{false};
};

NpmResultContextV1 MakeNpmResultContext(std::string task_id);
}  // namespace flowsql::npm
