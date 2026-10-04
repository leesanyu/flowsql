// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#ifndef FLOWSQL_CHANNELS_NETADAPTER_BUDGET_H_
#define FLOWSQL_CHANNELS_NETADAPTER_BUDGET_H_
#include <arrow/api.h>
#include <atomic>
#include <cstring>
#include <memory>
namespace flowsql::channels::netadapter {
/** Allocation capacity and copy peak, not only captured payload length. */
class CaptureMemoryPool final : public arrow::MemoryPool {
 public:
    explicit CaptureMemoryPool(int64_t limit) : limit_(limit) {}
    arrow::Status Allocate(int64_t size, int64_t alignment, uint8_t** out) override {
        if (size < 0 || used_ > limit_ - size) return arrow::Status::OutOfMemory("capture Arrow budget exceeded");
        const auto status = arrow::default_memory_pool()->Allocate(size, alignment, out);
        if (status.ok()) {
            used_ += size;
            total_ += size;
            ++allocations_;
            peak_ = std::max(peak_, used_.load());
        }
        return status;
    }
    arrow::Status Reallocate(int64_t old_size, int64_t size, int64_t alignment, uint8_t** ptr) override {
        uint8_t* next = nullptr;
        const auto status = Allocate(size, alignment, &next);
        if (!status.ok()) return status;
        if (old_size && size) std::memcpy(next, *ptr, std::min(old_size, size));
        Free(*ptr, old_size, alignment);
        *ptr = next;
        return arrow::Status::OK();
    }
    void Free(uint8_t* ptr, int64_t size, int64_t alignment) override {
        arrow::default_memory_pool()->Free(ptr, size, alignment);
        used_ -= size;
    }
    int64_t bytes_allocated() const override { return used_; }
    int64_t max_memory() const override { return peak_; }
    int64_t total_bytes_allocated() const override { return total_; }
    int64_t num_allocations() const override { return allocations_; }
    std::string backend_name() const override { return "capture_bounded"; }

 private:
    int64_t limit_, peak_ = 0, total_ = 0, allocations_ = 0;
    std::atomic<int64_t> used_{0};
};
/** Any retained Arrow buffer owns the allocator and channel/backend lease. */
class CaptureOwnedBuffer final : public arrow::Buffer {
 public:
    CaptureOwnedBuffer(std::shared_ptr<arrow::Buffer> parent, std::shared_ptr<void> lease,
                       std::shared_ptr<CaptureMemoryPool> pool)
        : arrow::Buffer(parent->data(), parent->size()),
          lease_(std::move(lease)),
          pool_(std::move(pool)),
          parent_(std::move(parent)) {}

 private:
    std::shared_ptr<void> lease_;
    std::shared_ptr<CaptureMemoryPool> pool_;
    std::shared_ptr<arrow::Buffer> parent_;
};
inline std::shared_ptr<arrow::ArrayData> RetainCaptureBuffers(const std::shared_ptr<arrow::ArrayData>& data,
                                                              const std::shared_ptr<void>& lease,
                                                              const std::shared_ptr<CaptureMemoryPool>& pool) {
    auto next = data->Copy();
    for (auto& buffer : next->buffers)
        if (buffer) buffer = std::make_shared<CaptureOwnedBuffer>(buffer, lease, pool);
    for (auto& child : next->child_data) child = RetainCaptureBuffers(child, lease, pool);
    return next;
}
}  // namespace flowsql::channels::netadapter
#endif
