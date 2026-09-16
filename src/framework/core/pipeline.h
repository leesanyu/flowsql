// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_FRAMEWORK_CORE_PIPELINE_H_
#define _FLOWSQL_FRAMEWORK_CORE_PIPELINE_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "framework/core/filter_executor.h"
#include "framework/interfaces/ichannel.h"
#include "framework/interfaces/iblock_stream_channel.h"
#include "framework/interfaces/iblock_transform_operator.h"
#include "framework/interfaces/ioperator.h"

namespace flowsql {

enum class PipelineState : int32_t {
    IDLE = 0,
    RUNNING,
    STOPPED,
    FAILED
};

// Pipeline — 纯连接器，只负责将 source 和 sink 通道交给算子
class Pipeline {
 public:
    Pipeline() = default;
    ~Pipeline() = default;

    void Run();
    void Stop();
    PipelineState State() const { return state_.load(); }
    const std::string& ErrorMessage() const { return error_message_; }

 private:
    friend class PipelineBuilder;

    IChannel* source_ = nullptr;
    IOperator* operator_ = nullptr;
    IChannel* sink_ = nullptr;
    std::atomic<PipelineState> state_{PipelineState::IDLE};
    std::string error_message_;
};

class PipelineBuilder {
 public:
    PipelineBuilder& SetSource(IChannel* channel);
    PipelineBuilder& SetOperator(IOperator* op);
    PipelineBuilder& SetSink(IChannel* channel);
    std::unique_ptr<Pipeline> Build();

 private:
    IChannel* source_ = nullptr;
    IOperator* operator_ = nullptr;
    IChannel* sink_ = nullptr;
};

/** Runtime residual-filter stage. One instance belongs to one pipeline stage. */
class BlockFilterStage {
 public:
    explicit BlockFilterStage(std::shared_ptr<const BoundFilterExpr> residual_expression);

    /** Open exactly once and expose the unchanged output Schema before the first block. */
    FilterEvalError Open(const std::shared_ptr<arrow::Schema>& input_schema,
                         std::shared_ptr<arrow::Schema>* output_schema,
                         std::string* error);

    /** Produce exactly one data block, including when filtering leaves zero rows. */
    FilterEvalError ProcessBlock(const std::shared_ptr<arrow::RecordBatch>& input,
                                 int64_t ts_ms,
                                 BlockTransformOutputV1* output,
                                 std::string* error) const;

    bool IsOpen() const { return opened_; }

 private:
    std::shared_ptr<const BoundFilterExpr> residual_expression_;
    std::shared_ptr<arrow::Schema> schema_;
    bool opened_ = false;
};

enum class BlockTransformPipelineError : int32_t {
    kNone = 0,
    kInvalidArgument,
    kAlreadyRun,
    kFilterOpenFailed,
    kTransformOpenFailed,
    kSourcePollFailed,
    kSourceReleaseFailed,
    kTransformFailed,
    kTransformContractViolation,
    kOutputFilterFailed,
    kOutputConsumerFailed,
    kCancelled,
};

enum class BlockTransformPipelineTerminal : int32_t {
    kCompleted = 0,
    kStopped,
    kCancelled,
    kFailed,
};

struct BlockTransformPipelineConfig {
    IBlockStreamChannel* source = nullptr;
    std::shared_ptr<arrow::Schema> source_schema;
    IBlockTransformTaskV1* transform = nullptr;
    // Optional time interface of the same execution task as transform. Null preserves V1 behavior.
    IBlockTransformTimeDrivenTaskV1* time_transform = nullptr;
    std::shared_ptr<const BoundFilterExpr> source_residual;
    std::shared_ptr<const BoundFilterExpr> transform_residual;
    std::function<int(const BlockTransformOutputV1&)> output_consumer;
    // Injectable clocks for deterministic scheduling tests; empty functions use system clocks.
    std::function<int64_t()> monotonic_clock_ns;
    std::function<int64_t()> wall_clock_ns;
    int poll_timeout_ms = 100;
};

struct BlockTransformPipelineResult {
    BlockTransformPipelineTerminal terminal = BlockTransformPipelineTerminal::kFailed;
    uint64_t input_blocks = 0;
    int64_t input_rows = 0;
    uint64_t output_blocks = 0;
    int64_t output_rows = 0;
};

/** Synchronous transform chain with optional, stage-aligned time capabilities. */
class SynchronousBlockTransformChainTask final : public IBlockTransformTaskV2 {
 public:
    SynchronousBlockTransformChainTask(
        std::vector<IBlockTransformTaskV1*> tasks,
        std::vector<IBlockTransformTimeDrivenTaskV1*> time_tasks,
        std::vector<std::shared_ptr<arrow::Schema>> expected_output_schemas,
        const std::vector<std::shared_ptr<const BoundFilterExpr>>& residuals);

    int Open(std::shared_ptr<arrow::Schema> input_schema,
             std::shared_ptr<arrow::Schema>* output_schema) override;
    int ProcessBlock(const std::shared_ptr<arrow::RecordBatch>& input,
                     int64_t ts_ms,
                     std::vector<BlockTransformOutputV1>* outputs) override;
    int Flush(std::vector<BlockTransformOutputV1>* outputs) override;
    void Cancel() override;
    std::string LastError() const override;

    int GetTimeDriveState(BlockTransformTimeDriveStateV1* state) override;
    int OnTime(const BlockTransformTimeEventV1& event,
               std::vector<BlockTransformOutputV1>* outputs) override;

 private:
    int PropagateFrom(size_t first_stage,
                      std::vector<BlockTransformOutputV1> inputs,
                      std::vector<BlockTransformOutputV1>* outputs,
                      bool* stopped);
    int FilterStageOutputs(size_t stage,
                           const std::vector<BlockTransformOutputV1>& inputs,
                           std::vector<BlockTransformOutputV1>* outputs);
    int QueryTimeState(size_t stage, BlockTransformTimeDriveStateV1* state);
    int SetStageError(size_t stage, std::string detail, int rc);
    std::string TaskError(size_t stage) const;

    std::vector<IBlockTransformTaskV1*> tasks_;
    std::vector<IBlockTransformTimeDrivenTaskV1*> time_tasks_;
    std::vector<std::shared_ptr<arrow::Schema>> expected_output_schemas_;
    std::vector<BlockFilterStage> filters_;
    bool open_attempted_ = false;
    bool opened_ = false;
    bool flush_started_ = false;
    std::atomic<bool> cancel_started_{false};
    std::string last_error_;
};

/** Run one exclusive block transform task between reusable residual Filter Stages. */
class BlockTransformPipelineRunner {
 public:
    explicit BlockTransformPipelineRunner(BlockTransformPipelineConfig config);

    BlockTransformPipelineError Run(BlockTransformPipelineResult* result,
                                    std::string* error);
    void Cancel();

 private:
    BlockTransformPipelineConfig config_;
    std::atomic<bool> run_started_{false};
    std::atomic<bool> cancel_requested_{false};
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_CORE_PIPELINE_H_
