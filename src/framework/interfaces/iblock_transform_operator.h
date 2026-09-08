// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_IBLOCK_TRANSFORM_OPERATOR_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_IBLOCK_TRANSFORM_OPERATOR_H_

#include <common/guid.h>
#include <common/typedef.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace arrow {
class RecordBatch;
class Schema;
}

namespace flowsql {

// {0x5de2079a-b463-48cf-917d-6ae82534cbf0}
const Guid IID_BLOCK_TRANSFORM_OPERATOR_V1 = {
    0x5de2079a, 0xb463, 0x48cf, {0x91, 0x7d, 0x6a, 0xe8, 0x25, 0x34, 0xcb, 0xf0}};

constexpr uint32_t kBlockTransformContractVersionV1 = 1;

enum class BlockTransformStatusV1 : int32_t {
    kContinue = 0,
    kStop = 1,
};

struct BlockTransformTaskConfigV1 {
    uint32_t contract_version = kBlockTransformContractVersionV1;
    const char* task_id = nullptr;
    const char* with_params_json = nullptr;
    // Exact pushed expression only. Use a versioned empty plan when no predicate was accepted.
    const char* pushed_filter_plan_json = nullptr;
};

struct BlockTransformOutputV1 {
    std::shared_ptr<arrow::RecordBatch> batch;
    int64_t ts_ms = 0;
};

/** Task-owned transform state. One session must never be shared by concurrent tasks. */
interface IBlockTransformTaskV1 {
    virtual ~IBlockTransformTaskV1() = default;

    /**
     * Called exactly once. Validates input_schema and makes the output schema available before the
     * first input block. A failed Open makes the session unusable except for Cancel/ReleaseTask.
     */
    virtual int Open(std::shared_ptr<arrow::Schema> input_schema,
                     std::shared_ptr<arrow::Schema>* output_schema) = 0;

    /**
     * Produces zero, one, or multiple outputs in caller-owned storage.
     * outputs must be empty on entry. The runtime drains every returned output before calling this
     * session again; this call-level handoff is the backpressure boundary. Return kContinue, kStop,
     * or a negative error. A negative return must leave outputs empty.
     */
    virtual int ProcessBlock(const std::shared_ptr<arrow::RecordBatch>& input,
                             int64_t ts_ms,
                             std::vector<BlockTransformOutputV1>* outputs) = 0;

    /**
     * Called exactly once after normal EOF or kStop; it may produce zero or more outputs. A nonzero
     * return must leave outputs empty. Flush is not called after cancellation or another task error.
     */
    virtual int Flush(std::vector<BlockTransformOutputV1>* outputs) = 0;

    /** Must be safe to call concurrently and must make an in-flight blocking call return promptly. */
    virtual void Cancel() = 0;
    virtual std::string LastError() const = 0;
};

/** Stateless provider discovered through IID_BLOCK_TRANSFORM_OPERATOR_V1. */
interface IBlockTransformOperatorV1 {
    virtual ~IBlockTransformOperatorV1() = default;

    virtual std::string Category() const = 0;
    virtual std::string Name() const = 0;
    virtual std::string Description() const = 0;

    /**
     * Creates an exclusive task session. The implementation must copy all config content before
     * returning and must not retain its raw pointers. The provider retains the allocation domain.
     */
    virtual int CreateTask(const BlockTransformTaskConfigV1& config,
                           IBlockTransformTaskV1** task) = 0;

    /** Called only after all task method calls have completed. */
    virtual void ReleaseTask(IBlockTransformTaskV1* task) = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_IBLOCK_TRANSFORM_OPERATOR_H_
