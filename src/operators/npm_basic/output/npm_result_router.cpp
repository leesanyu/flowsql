// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include "npm_result_router.h"
#include <arrow/api.h>
#include <cerrno>
#include <iomanip>
#include <random>
#include <sstream>

namespace flowsql::npm {
namespace {
class ResultMemoryPool final : public arrow::MemoryPool {
 public:
    explicit ResultMemoryPool(std::shared_ptr<INpmTaskBudget> budget)
        : budget_(std::move(budget)), backing_(arrow::default_memory_pool()) {}
    arrow::Status Allocate(int64_t size, int64_t alignment, uint8_t** out) override {
        if (budget_->Reserve(NpmBudgetCategory::kPendingOutput, size) != NpmBudgetError::kNone)
            return arrow::Status::OutOfMemory("NPM pending output budget exceeded");
        auto status = backing_.Allocate(size, alignment, out);
        if (!status.ok()) budget_->Release(NpmBudgetCategory::kPendingOutput, size);
        return status;
    }
    arrow::Status Reallocate(int64_t old_size, int64_t new_size, int64_t alignment, uint8_t** ptr) override {
        const auto extra = std::max<int64_t>(0, new_size - old_size);
        if (budget_->Reserve(NpmBudgetCategory::kPendingOutput, extra) != NpmBudgetError::kNone)
            return arrow::Status::OutOfMemory("NPM pending output budget exceeded");
        auto status = backing_.Reallocate(old_size, new_size, alignment, ptr);
        budget_->Release(NpmBudgetCategory::kPendingOutput,
                         status.ok() ? std::max<int64_t>(0, old_size - new_size) : extra);
        return status;
    }
    void Free(uint8_t* ptr, int64_t size, int64_t alignment) override {
        backing_.Free(ptr, size, alignment);
        budget_->Release(NpmBudgetCategory::kPendingOutput, size);
    }
    int64_t bytes_allocated() const override { return backing_.bytes_allocated(); }
    int64_t max_memory() const override { return backing_.max_memory(); }
    int64_t total_bytes_allocated() const override { return backing_.total_bytes_allocated(); }
    int64_t num_allocations() const override { return backing_.num_allocations(); }
    std::string backend_name() const override { return "npm_result_budget"; }

 private:
    std::shared_ptr<INpmTaskBudget> budget_;
    arrow::ProxyMemoryPool backing_;
};

// Each buffer keeps the allocator alive, including when callers retain just an Array or sliced Buffer.
struct BufferOwner {
    std::shared_ptr<arrow::MemoryPool> pool;
    std::shared_ptr<arrow::Buffer> buffer;
};
void LeaseBuffers(const std::shared_ptr<arrow::ArrayData>& data, const std::shared_ptr<arrow::MemoryPool>& pool) {
    for (auto& buffer : data->buffers) {
        if (!buffer) continue;
        auto owner = std::make_shared<BufferOwner>(BufferOwner{pool, buffer});
        buffer = std::shared_ptr<arrow::Buffer>(owner, owner->buffer.get());
    }
    for (auto& child : data->child_data) LeaseBuffers(child, pool);
    if (data->dictionary) LeaseBuffers(data->dictionary, pool);
}
}  // namespace

NpmResultContextV1 MakeNpmResultContext(std::string task_id) {
    static std::atomic<uint64_t> sequence{0};
    std::random_device random;
    std::ostringstream id;
    id << std::hex << std::setfill('0');
    for (int i = 0; i < 4; ++i) id << std::setw(8) << random();
    id << '-' << ++sequence;
    return {std::move(task_id), id.str()};
}
NpmResultRouter::NpmResultRouter(std::vector<NpmEntityDescriptorV1> entities, std::string observing,
                                 NpmResultContextV1 context, std::shared_ptr<INpmTaskBudget> budget,
                                 std::unique_ptr<INpmResultConsumerV1> consumer, Invoke invoke)
    : entities_(std::move(entities)),
      observing_(std::move(observing)),
      context_(std::move(context)),
      pool_(std::make_shared<ResultMemoryPool>(std::move(budget))),
      consumer_(std::move(consumer)),
      invoke_(std::move(invoke)) {}
NpmResultRouter::~NpmResultRouter() { Cancel(); }
int NpmResultRouter::Fail(int code, std::string message) {
    if (!error_code_) {
        error_code_ = code;
        error_ = std::move(message);
    }
    return error_code_;
}
int NpmResultRouter::Emit(std::string_view module, std::string_view entity_id, const arrow::RecordBatch& rows) {
    if (cancelled_) return ECANCELED;
    if (error_code_) return error_code_;
    const NpmEntityDescriptorV1* entity = nullptr;
    for (const auto& candidate : entities_)
        if (candidate.entity_id == entity_id) entity = &candidate;
    if (!entity) return Fail(EINVAL, std::string(module) + "/" + std::string(entity_id) + ": unknown entity");
    const auto status = ValidateNpmEntityRowsV1(module, *entity, rows);
    if (status.error != NpmProtocolContractErrorV1::kNone)
        return Fail(EINVAL, entity->entity_id + "/" + status.field + ": invalid entity/Schema/rows");
    if (rows.num_rows() == 0) return 0;
    if (entity_id == observing_) {
        if (!pending_) {
            auto builder = arrow::RecordBatchBuilder::Make(entity->schema, pool_.get(), 0);
            if (!builder.ok()) return Fail(ENOMEM, entity->entity_id + ": " + builder.status().ToString());
            pending_ = std::move(*builder);
        }
        for (int index = 0; index < rows.num_columns(); ++index) {
            const auto appended = pending_->GetField(index)->AppendArraySlice(
                arrow::ArraySpan(*rows.column(index)->data()), 0, rows.num_rows());
            if (!appended.ok()) return Fail(ENOMEM, entity->entity_id + ": " + appended.ToString());
        }
        pending_rows_ += rows.num_rows();
    }
    if (consumer_) {
        const int error = invoke_([&] { return consumer_->Consume(context_, *entity, rows); });
        if (error) return Fail(error, entity->entity_id + ": consumer Consume failed: " + std::to_string(error));
    }
    return cancelled_ ? ECANCELED : 0;
}
int NpmResultRouter::Drain(std::shared_ptr<arrow::RecordBatch>* output) {
    if (cancelled_) return ECANCELED;
    if (error_code_) return error_code_;
    std::shared_ptr<arrow::Schema> schema;
    for (const auto& entity : entities_)
        if (entity.entity_id == observing_) schema = entity.schema;
    std::shared_ptr<arrow::RecordBatch> rows;
    if (pending_) {
        // Finish transfers the compact column buffers; there is no second full-result concatenation.
        auto result = pending_->Flush(false);
        if (!result.ok()) return Fail(ENOMEM, result.status().ToString());
        rows = arrow::RecordBatch::Make(schema, (*result)->num_rows(), (*result)->columns());
    } else {
        auto result = arrow::RecordBatch::MakeEmpty(schema, pool_.get());
        if (!result.ok()) return Fail(ENOMEM, result.status().ToString());
        rows = std::move(*result);
    }
    for (const auto& column : rows->columns()) LeaseBuffers(column->data(), pool_);
    *output = std::move(rows);
    Discard();
    return 0;
}

int NpmResultRouter::PublishProgress(const NpmResultProgressV1& progress) {
    if (cancelled_) return ECANCELED;
    if (error_code_) return error_code_;
    if (finished_) return EPIPE;
    if (progress.contract_version != 1 || progress.entity_id != "basic" || progress.period_ns <= 0 ||
        progress.closed_before_ns < 0 || progress.closed_before_ns % progress.period_ns != 0 || pending_rows_ != 0)
        return Fail(EINVAL, "invalid or undrained NPM result progress");
    if (progress.closed_before_ns < closed_before_ns_) return Fail(EINVAL, "regressing NPM result progress");
    if (progress.closed_before_ns == closed_before_ns_) return 0;
    auto* publisher = dynamic_cast<INpmResultProgressConsumerV1*>(consumer_.get());
    const int rc = publisher ? invoke_([&] { return publisher->PublishProgress(context_, progress); }) : 0;
    if (rc) return Fail(rc, "consumer progress publication failed");
    if (cancelled_) return ECANCELED;
    closed_before_ns_ = progress.closed_before_ns;
    return 0;
}
int NpmResultRouter::Finish() {
    if (cancelled_) return ECANCELED;
    if (error_code_) return error_code_;
    if (finished_) return 0;
    const int error = consumer_ ? invoke_([&] { return consumer_->Finish(); }) : 0;
    if (error) return Fail(error, "consumer Finish failed: " + std::to_string(error));
    if (cancelled_) return ECANCELED;
    finished_ = true;
    return 0;
}
int NpmResultRouter::FailRun(int32_t code, std::string stage, std::string message) noexcept {
    if (finished_ || failure_finalized_.exchange(true)) return 0;
    auto* managed = dynamic_cast<INpmManagedResultConsumerV1*>(consumer_.get());
    if (!managed) return 0;
    NpmResultFailureV1 failure;
    failure.code = code;
    failure.stage = stage.c_str();
    failure.message = message.c_str();
    try {
        return invoke_([&] { return managed->FailRun(failure); });
    } catch (...) {
        return EFAULT;
    }
}
std::string NpmResultRouter::ResultJson() const {
    const auto* managed = dynamic_cast<const INpmManagedResultConsumerV1*>(consumer_.get());
    return managed ? managed->ResultJson() : std::string();
}
void NpmResultRouter::Cancel() noexcept {
    if (!finished_ && !cancelled_.exchange(true) && consumer_) consumer_->Cancel();
}
void NpmResultRouter::Discard() noexcept {
    pending_.reset();
    pending_rows_ = 0;
}
}  // namespace flowsql::npm
