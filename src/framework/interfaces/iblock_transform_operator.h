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

#include "icapture_block_stream_reader.h"

namespace arrow {
class RecordBatch;
class Schema;
}  // namespace arrow

namespace flowsql {

class IChannel;

// {0x9a1d3f74-6c2b-4e88-b715-43e26f910acd}
const Guid IID_BLOCK_TRANSFORM_OPERATOR = {
    0x9a1d3f74, 0x6c2b, 0x4e88, {0xb7, 0x15, 0x43, 0xe2, 0x6f, 0x91, 0x0a, 0xcd}};

constexpr uint32_t kBlockTransformContractVersion = 2;
constexpr uint32_t kBlockTransformTimeDriveVersion = 1;

enum class BlockTransformStatus : int32_t {
    kContinue = 0,
    kStop = 1,
};

/** Borrowed task creation parameters for a transform provider. The provider copies all required text. */
struct BlockTransformTaskConfig {
    uint32_t struct_size = sizeof(BlockTransformTaskConfig);
    uint32_t contract_version = kBlockTransformContractVersion;
    const char* task_id = nullptr;
    const char* with_params_json = nullptr;
    const char* pushed_filter_plan_json = nullptr;
};

constexpr uint32_t kBlockTransformTaskConfigSize = static_cast<uint32_t>(sizeof(BlockTransformTaskConfig));

struct BlockTransformOutput {
    std::shared_ptr<arrow::RecordBatch> batch;
    int64_t ts_ms = 0;
};

/** Side-effect-free snapshot of the task's earliest monotonic deadline. */
struct BlockTransformTimeDriveState {
    uint32_t struct_size;
    uint32_t contract_version;
    uint8_t armed;
    uint8_t reserved[7];
    int64_t deadline_ns;
};

constexpr uint32_t kBlockTransformTimeDriveStateSize = static_cast<uint32_t>(sizeof(BlockTransformTimeDriveState));

/** Runtime-owned clock snapshot delivered only when an armed deadline is due. */
struct BlockTransformTimeEvent {
    uint32_t struct_size;
    uint32_t contract_version;
    int64_t monotonic_now_ns;
    int64_t wall_now_ns;
};

constexpr uint32_t kBlockTransformTimeEventSize = static_cast<uint32_t>(sizeof(BlockTransformTimeEvent));

/** Task-owned transform state. One session must never be shared by concurrent tasks. */
interface IBlockTransformTask {
    virtual ~IBlockTransformTask() = default;

    /**
     * Called exactly once. Validates input_schema and makes the output schema available before the
     * first input block. A failed Open makes the session unusable except for Cancel/ReleaseTask.
     */
    virtual int Open(std::shared_ptr<arrow::Schema> input_schema, std::shared_ptr<arrow::Schema> * output_schema) = 0;

    /**
     * Produces zero, one, or multiple outputs in caller-owned storage.
     * outputs must be empty on entry. The runtime drains every returned output before calling this
     * session again; this call-level handoff is the backpressure boundary. Return kContinue, kStop,
     * or a negative error. A negative return must leave outputs empty.
     */
    virtual int ProcessBlock(const std::shared_ptr<arrow::RecordBatch>& input, int64_t ts_ms,
                             std::vector<BlockTransformOutput>* outputs) = 0;

    /**
     * Called exactly once after normal EOF or kStop; it may produce zero or more outputs. A nonzero
     * return must leave outputs empty. Flush is not called after cancellation or another task error.
     */
    virtual int Flush(std::vector<BlockTransformOutput> * outputs) = 0;

    /** Must be safe to call concurrently and must make an in-flight blocking call return promptly. */
    virtual void Cancel() = 0;
    virtual std::string LastError() const = 0;
};

/** Optional input identity. Bind once before Open; task copies the borrowed FROM source text. */
interface IBlockTransformInputSourceTask {
    virtual ~IBlockTransformInputSourceTask() = default;
    virtual int BindInputSource(const char* source) = 0;
};

/** Optional task-local time capability. Calls are serialized with ProcessBlock and Flush. */
interface IBlockTransformTimeDrivenTask {
    virtual ~IBlockTransformTimeDrivenTask() = default;

    virtual int GetTimeDriveState(BlockTransformTimeDriveState * state) = 0;
    virtual int OnTime(const BlockTransformTimeEvent& event, std::vector<BlockTransformOutput>* outputs) = 0;
};

/** Accept one poll's facts after packet processing, before output confirmation and source release. */
interface IBlockTransformCaptureFactTask {
    virtual ~IBlockTransformCaptureFactTask() = default;
    virtual int BindCaptureSources(const CaptureSourceSet& sources) = 0;
    virtual int AcceptCaptureFacts(const std::vector<CaptureProgress>& facts) = 0;
};

constexpr uint32_t kBlockTransformManagedSinkContractVersion = 1;

/** Borrowed binding; the Scheduler keeps sink_channel leased through ReleaseTask. */
struct BlockTransformManagedSinkBinding {
    uint32_t struct_size = sizeof(BlockTransformManagedSinkBinding);
    uint32_t contract_version = kBlockTransformManagedSinkContractVersion;
    IChannel* sink_channel = nullptr;
    const char* target = nullptr;
    const char* category = nullptr;
    const char* name = nullptr;
    const char* relation = nullptr;
};

/** Optional task capability. Bind once before Open; task copies all required text. */
interface IBlockTransformManagedSinkTask {
    virtual ~IBlockTransformManagedSinkTask() = default;
    virtual int BindManagedSink(const BlockTransformManagedSinkBinding& binding) = 0;
    virtual std::string ManagedSinkResultJson() const = 0;
};

/** Stateless provider discovered through IID_BLOCK_TRANSFORM_OPERATOR. */
interface IBlockTransformOperator {
    virtual ~IBlockTransformOperator() = default;

    virtual std::string Category() const = 0;
    virtual std::string Name() const = 0;
    virtual std::string Description() const = 0;

    /**
     * Creates an exclusive task. The provider must validate struct_size/version, copy borrowed
     * configuration text before returning, and retain the allocation domain until ReleaseTask.
     */
    virtual int CreateTask(const BlockTransformTaskConfig& config, IBlockTransformTask** task) = 0;

    /** Called only after all task method calls have completed. */
    virtual void ReleaseTask(IBlockTransformTask * task) = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_IBLOCK_TRANSFORM_OPERATOR_H_
