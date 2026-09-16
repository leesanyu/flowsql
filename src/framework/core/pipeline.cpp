// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "pipeline.h"

#include <arrow/api.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <common/log.h>
#include <exception>
#include <iterator>
#include <utility>
#include <vector>

namespace flowsql {

void Pipeline::Run() {
    state_ = PipelineState::RUNNING;
    error_message_.clear();

    if (!source_ || !operator_ || !sink_) {
        error_message_ = "missing source, operator, or sink";
        LOG_INFO("Pipeline::Run: %s", error_message_.c_str());
        state_ = PipelineState::FAILED;
        return;
    }

    // 纯连接器：直接将 source 和 sink 通道交给算子
    if (operator_->Work(source_, sink_) != 0) {
        error_message_ = "operator " + operator_->Category() + "." + operator_->Name() + " execution failed";
        state_ = PipelineState::FAILED;
        return;
    }

    state_ = PipelineState::STOPPED;
}

void Pipeline::Stop() {
    state_ = PipelineState::STOPPED;
}

// --- PipelineBuilder ---

PipelineBuilder& PipelineBuilder::SetSource(IChannel* channel) {
    source_ = channel;
    return *this;
}

PipelineBuilder& PipelineBuilder::SetOperator(IOperator* op) {
    operator_ = op;
    return *this;
}

PipelineBuilder& PipelineBuilder::SetSink(IChannel* channel) {
    sink_ = channel;
    return *this;
}

std::unique_ptr<Pipeline> PipelineBuilder::Build() {
    auto pipeline = std::make_unique<Pipeline>();
    pipeline->source_ = source_;
    pipeline->operator_ = operator_;
    pipeline->sink_ = sink_;
    return pipeline;
}

BlockFilterStage::BlockFilterStage(
    std::shared_ptr<const BoundFilterExpr> residual_expression)
    : residual_expression_(std::move(residual_expression)) {}

FilterEvalError BlockFilterStage::Open(
    const std::shared_ptr<arrow::Schema>& input_schema,
    std::shared_ptr<arrow::Schema>* output_schema,
    std::string* error) {
    if (output_schema) output_schema->reset();
    if (error) error->clear();
    if (!input_schema || !output_schema) {
        if (error) *error = "filter stage input and output Schema must not be null";
        return FilterEvalError::kInvalidArgument;
    }
    if (opened_) {
        if (error) *error = "filter stage is already open";
        return FilterEvalError::kInvalidArgument;
    }

    if (residual_expression_) {
        std::vector<std::shared_ptr<arrow::Array>> empty_columns;
        empty_columns.reserve(static_cast<size_t>(input_schema->num_fields()));
        for (const auto& field : input_schema->fields()) {
            if (!field || !field->type()) {
                if (error) *error = "filter stage input Schema contains an incomplete field";
                return FilterEvalError::kSchemaMismatch;
            }
            auto empty_array = arrow::MakeArrayOfNull(field->type(), 0);
            if (!empty_array.ok()) {
                if (error) {
                    *error = "filter stage could not build validation batch: " +
                             empty_array.status().ToString();
                }
                return FilterEvalError::kArrowError;
            }
            empty_columns.push_back(*empty_array);
        }
        auto validation_batch = arrow::RecordBatch::Make(input_schema, 0, empty_columns);
        std::shared_ptr<arrow::RecordBatch> ignored;
        const auto validation = FilterRecordBatch(
            validation_batch, residual_expression_, &ignored, error);
        if (validation != FilterEvalError::kNone) return validation;
    }

    schema_ = input_schema;
    opened_ = true;
    *output_schema = schema_;
    return FilterEvalError::kNone;
}

FilterEvalError BlockFilterStage::ProcessBlock(
    const std::shared_ptr<arrow::RecordBatch>& input,
    int64_t ts_ms,
    BlockTransformOutputV1* output,
    std::string* error) const {
    if (output) *output = BlockTransformOutputV1{};
    if (error) error->clear();
    if (!opened_ || !input || !output) {
        if (error) *error = "filter stage must be open and receive a data block/output";
        return FilterEvalError::kInvalidArgument;
    }
    if (!input->schema() || !input->schema()->Equals(*schema_, true)) {
        if (error) *error = "filter stage input batch does not match its opened Schema";
        return FilterEvalError::kSchemaMismatch;
    }

    std::shared_ptr<arrow::RecordBatch> filtered = input;
    if (residual_expression_) {
        const auto filter_result = FilterRecordBatch(
            input, residual_expression_, &filtered, error);
        if (filter_result != FilterEvalError::kNone) return filter_result;
    }
    output->batch = std::move(filtered);
    output->ts_ms = ts_ms;
    return FilterEvalError::kNone;
}

SynchronousBlockTransformChainTask::SynchronousBlockTransformChainTask(
    std::vector<IBlockTransformTaskV1*> tasks,
    std::vector<IBlockTransformTimeDrivenTaskV1*> time_tasks,
    std::vector<std::shared_ptr<arrow::Schema>> expected_output_schemas,
    const std::vector<std::shared_ptr<const BoundFilterExpr>>& residuals)
    : tasks_(std::move(tasks)),
      time_tasks_(std::move(time_tasks)),
      expected_output_schemas_(std::move(expected_output_schemas)) {
    filters_.reserve(residuals.size());
    for (const auto& residual : residuals) filters_.emplace_back(residual);
}

int SynchronousBlockTransformChainTask::Open(
    std::shared_ptr<arrow::Schema> input_schema,
    std::shared_ptr<arrow::Schema>* output_schema) {
    if (open_attempted_ || !input_schema || !output_schema || tasks_.empty() ||
        tasks_.size() != time_tasks_.size() ||
        tasks_.size() != expected_output_schemas_.size() ||
        tasks_.size() != filters_.size() ||
        std::any_of(tasks_.begin(), tasks_.end(), [](const auto* task) { return !task; })) {
        last_error_ = "multi transform chain received an invalid Open request";
        return EINVAL;
    }
    open_attempted_ = true;
    output_schema->reset();

    auto current_schema = std::move(input_schema);
    for (size_t i = 0; i < tasks_.size(); ++i) {
        std::shared_ptr<arrow::Schema> actual_output_schema;
        int open_rc = 0;
        try {
            open_rc = tasks_[i]->Open(current_schema, &actual_output_schema);
        } catch (const std::exception& ex) {
            return SetStageError(i, "Open threw: " + std::string(ex.what()), EFAULT);
        } catch (...) {
            return SetStageError(i, "Open threw an unknown exception", EFAULT);
        }
        if (open_rc != 0) {
            return SetStageError(i, "Open failed: " + TaskError(i), open_rc);
        }
        if (!actual_output_schema || !expected_output_schemas_[i] ||
            !actual_output_schema->Equals(*expected_output_schemas_[i], true)) {
            return SetStageError(i, "execution Schema differs from its planning Schema", EPROTO);
        }

        std::string filter_error;
        std::shared_ptr<arrow::Schema> filtered_schema;
        const auto filter_rc = filters_[i].Open(
            actual_output_schema, &filtered_schema, &filter_error);
        if (filter_rc != FilterEvalError::kNone) {
            if (filter_error.empty()) filter_error = "residual filter Open failed";
            return SetStageError(i, std::move(filter_error), EINVAL);
        }
        current_schema = std::move(filtered_schema);
    }

    opened_ = true;
    *output_schema = std::move(current_schema);
    return 0;
}

int SynchronousBlockTransformChainTask::ProcessBlock(
    const std::shared_ptr<arrow::RecordBatch>& input,
    int64_t ts_ms,
    std::vector<BlockTransformOutputV1>* outputs) {
    if (!opened_ || !input || !outputs || !outputs->empty() || flush_started_) {
        last_error_ = "multi transform chain received an invalid ProcessBlock request";
        return -EINVAL;
    }
    std::vector<BlockTransformOutputV1> initial = {{input, ts_ms}};
    std::vector<BlockTransformOutputV1> final_outputs;
    bool stopped = false;
    const int rc = PropagateFrom(0, std::move(initial), &final_outputs, &stopped);
    if (rc != 0) return rc;
    *outputs = std::move(final_outputs);
    return stopped ? static_cast<int>(BlockTransformStatusV1::kStop)
                   : static_cast<int>(BlockTransformStatusV1::kContinue);
}

int SynchronousBlockTransformChainTask::Flush(
    std::vector<BlockTransformOutputV1>* outputs) {
    if (!opened_ || !outputs || !outputs->empty() || flush_started_) {
        last_error_ = "multi transform chain received an invalid Flush request";
        return EINVAL;
    }
    flush_started_ = true;
    std::vector<BlockTransformOutputV1> final_outputs;

    for (size_t i = 0; i < tasks_.size(); ++i) {
        std::vector<BlockTransformOutputV1> stage_outputs;
        int flush_rc = 0;
        try {
            flush_rc = tasks_[i]->Flush(&stage_outputs);
        } catch (const std::exception& ex) {
            return SetStageError(i, "Flush threw: " + std::string(ex.what()), EFAULT);
        } catch (...) {
            return SetStageError(i, "Flush threw an unknown exception", EFAULT);
        }
        if (flush_rc != 0) {
            if (!stage_outputs.empty()) {
                return SetStageError(i, "Flush failed while returning output batches", EPROTO);
            }
            return SetStageError(i, "Flush failed: " + TaskError(i), flush_rc);
        }

        std::vector<BlockTransformOutputV1> filtered_outputs;
        const int filter_rc = FilterStageOutputs(i, stage_outputs, &filtered_outputs);
        if (filter_rc != 0) return filter_rc;

        std::vector<BlockTransformOutputV1> propagated_outputs;
        bool ignored_stop = false;
        const int propagate_rc = PropagateFrom(
            i + 1, std::move(filtered_outputs), &propagated_outputs, &ignored_stop);
        if (propagate_rc != 0) return propagate_rc;
        final_outputs.insert(final_outputs.end(),
                             std::make_move_iterator(propagated_outputs.begin()),
                             std::make_move_iterator(propagated_outputs.end()));
    }

    *outputs = std::move(final_outputs);
    return 0;
}

void SynchronousBlockTransformChainTask::Cancel() {
    bool expected = false;
    if (!cancel_started_.compare_exchange_strong(expected, true)) return;
    for (auto* task : tasks_) {
        if (!task) continue;
        try {
            task->Cancel();
        } catch (...) {
        }
    }
}

std::string SynchronousBlockTransformChainTask::LastError() const {
    return last_error_;
}

int SynchronousBlockTransformChainTask::GetTimeDriveState(
    BlockTransformTimeDriveStateV1* state) {
    if (!opened_ || flush_started_ || !state ||
        state->struct_size < kBlockTransformTimeDriveStateV1Size ||
        state->contract_version != kBlockTransformTimeDriveVersionV1) {
        last_error_ = "multi transform chain received an invalid time state request";
        return -EINVAL;
    }

    BlockTransformTimeDriveStateV1 aggregate{};
    aggregate.struct_size = kBlockTransformTimeDriveStateV1Size;
    aggregate.contract_version = kBlockTransformTimeDriveVersionV1;
    for (size_t i = 0; i < time_tasks_.size(); ++i) {
        if (!time_tasks_[i]) continue;
        BlockTransformTimeDriveStateV1 stage_state{};
        const int state_rc = QueryTimeState(i, &stage_state);
        if (state_rc != 0) return state_rc;
        if (stage_state.armed == 1 &&
            (aggregate.armed == 0 || stage_state.deadline_ns < aggregate.deadline_ns)) {
            aggregate.armed = 1;
            aggregate.deadline_ns = stage_state.deadline_ns;
        }
    }
    *state = aggregate;
    return 0;
}

int SynchronousBlockTransformChainTask::OnTime(
    const BlockTransformTimeEventV1& event,
    std::vector<BlockTransformOutputV1>* outputs) {
    if (!opened_ || flush_started_ || !outputs || !outputs->empty() ||
        event.struct_size < kBlockTransformTimeEventV1Size ||
        event.contract_version != kBlockTransformTimeDriveVersionV1 ||
        event.monotonic_now_ns < 0) {
        last_error_ = "multi transform chain received an invalid OnTime request";
        return -EINVAL;
    }

    std::vector<BlockTransformOutputV1> final_outputs;
    bool chain_stopped = false;
    for (size_t i = 0; i < time_tasks_.size(); ++i) {
        auto* time_task = time_tasks_[i];
        if (!time_task) continue;

        BlockTransformTimeDriveStateV1 state{};
        const int state_rc = QueryTimeState(i, &state);
        if (state_rc != 0) return state_rc;
        if (state.armed == 0 || state.deadline_ns > event.monotonic_now_ns) continue;

        std::vector<BlockTransformOutputV1> stage_outputs;
        int time_rc = 0;
        try {
            time_rc = time_task->OnTime(event, &stage_outputs);
        } catch (const std::exception& ex) {
            return SetStageError(i, "OnTime threw: " + std::string(ex.what()), EFAULT);
        } catch (...) {
            return SetStageError(i, "OnTime threw an unknown exception", EFAULT);
        }
        const bool valid_status =
            time_rc == static_cast<int>(BlockTransformStatusV1::kContinue) ||
            time_rc == static_cast<int>(BlockTransformStatusV1::kStop);
        if (!valid_status) {
            if (!stage_outputs.empty()) {
                return SetStageError(i, "OnTime failed while returning output batches", EPROTO);
            }
            return SetStageError(i, "OnTime failed: " + TaskError(i), time_rc);
        }
        if (time_rc == static_cast<int>(BlockTransformStatusV1::kContinue)) {
            BlockTransformTimeDriveStateV1 next_state{};
            const int next_state_rc = QueryTimeState(i, &next_state);
            if (next_state_rc != 0) return next_state_rc;
            if (next_state.armed == 1 && next_state.deadline_ns <= event.monotonic_now_ns) {
                return SetStageError(i, "OnTime made no deadline progress", EPROTO);
            }
        }

        std::vector<BlockTransformOutputV1> filtered_outputs;
        const int filter_rc = FilterStageOutputs(i, stage_outputs, &filtered_outputs);
        if (filter_rc != 0) return filter_rc;

        std::vector<BlockTransformOutputV1> propagated_outputs;
        bool downstream_stopped = false;
        const int propagate_rc = PropagateFrom(
            i + 1, std::move(filtered_outputs), &propagated_outputs, &downstream_stopped);
        if (propagate_rc != 0) return propagate_rc;
        final_outputs.insert(final_outputs.end(),
                             std::make_move_iterator(propagated_outputs.begin()),
                             std::make_move_iterator(propagated_outputs.end()));

        if (time_rc == static_cast<int>(BlockTransformStatusV1::kStop) || downstream_stopped) {
            chain_stopped = true;
            break;
        }
    }

    *outputs = std::move(final_outputs);
    return chain_stopped ? static_cast<int>(BlockTransformStatusV1::kStop)
                         : static_cast<int>(BlockTransformStatusV1::kContinue);
}

int SynchronousBlockTransformChainTask::PropagateFrom(
    size_t first_stage,
    std::vector<BlockTransformOutputV1> inputs,
    std::vector<BlockTransformOutputV1>* outputs,
    bool* stopped) {
    if (!outputs || !stopped || first_stage > tasks_.size()) return -EINVAL;
    outputs->clear();
    for (size_t i = first_stage; i < tasks_.size(); ++i) {
        std::vector<BlockTransformOutputV1> next_inputs;
        for (const auto& input : inputs) {
            if (!input.batch) return SetStageError(i, "received a null input batch", EPROTO);
            std::vector<BlockTransformOutputV1> stage_outputs;
            int process_rc = 0;
            try {
                process_rc = tasks_[i]->ProcessBlock(input.batch, input.ts_ms, &stage_outputs);
            } catch (const std::exception& ex) {
                return SetStageError(i, "ProcessBlock threw: " + std::string(ex.what()), EFAULT);
            } catch (...) {
                return SetStageError(i, "ProcessBlock threw an unknown exception", EFAULT);
            }
            const bool valid_status =
                process_rc == static_cast<int>(BlockTransformStatusV1::kContinue) ||
                process_rc == static_cast<int>(BlockTransformStatusV1::kStop);
            if (!valid_status) {
                if (!stage_outputs.empty()) {
                    return SetStageError(i, "ProcessBlock failed while returning output batches", EPROTO);
                }
                return SetStageError(i, "ProcessBlock failed: " + TaskError(i), process_rc);
            }
            if (process_rc == static_cast<int>(BlockTransformStatusV1::kStop)) *stopped = true;

            std::vector<BlockTransformOutputV1> filtered_outputs;
            const int filter_rc = FilterStageOutputs(i, stage_outputs, &filtered_outputs);
            if (filter_rc != 0) return filter_rc;
            next_inputs.insert(next_inputs.end(),
                               std::make_move_iterator(filtered_outputs.begin()),
                               std::make_move_iterator(filtered_outputs.end()));
        }
        inputs = std::move(next_inputs);
    }
    *outputs = std::move(inputs);
    return 0;
}

int SynchronousBlockTransformChainTask::FilterStageOutputs(
    size_t stage,
    const std::vector<BlockTransformOutputV1>& inputs,
    std::vector<BlockTransformOutputV1>* outputs) {
    if (!outputs || stage >= filters_.size()) return -EINVAL;
    outputs->clear();
    for (const auto& input : inputs) {
        if (!input.batch) return SetStageError(stage, "returned a null output batch", EPROTO);
        BlockTransformOutputV1 filtered;
        std::string filter_error;
        const auto filter_rc = filters_[stage].ProcessBlock(
            input.batch, input.ts_ms, &filtered, &filter_error);
        if (filter_rc != FilterEvalError::kNone) {
            if (filter_error.empty()) filter_error = "residual filter failed";
            return SetStageError(stage, std::move(filter_error), EIO);
        }
        outputs->push_back(std::move(filtered));
    }
    return 0;
}

int SynchronousBlockTransformChainTask::QueryTimeState(
    size_t stage,
    BlockTransformTimeDriveStateV1* state) {
    if (!state || stage >= time_tasks_.size() || !time_tasks_[stage]) return -EINVAL;
    *state = BlockTransformTimeDriveStateV1{};
    state->struct_size = kBlockTransformTimeDriveStateV1Size;
    state->contract_version = kBlockTransformTimeDriveVersionV1;
    int state_rc = 0;
    try {
        state_rc = time_tasks_[stage]->GetTimeDriveState(state);
    } catch (const std::exception& ex) {
        return SetStageError(stage, "time state query threw: " + std::string(ex.what()), EFAULT);
    } catch (...) {
        return SetStageError(stage, "time state query threw an unknown exception", EFAULT);
    }
    if (state_rc != 0) {
        return SetStageError(stage, "time state query failed with code " + std::to_string(state_rc), state_rc);
    }
    if (state->struct_size < kBlockTransformTimeDriveStateV1Size ||
        state->contract_version != kBlockTransformTimeDriveVersionV1 || state->armed > 1 ||
        std::any_of(std::begin(state->reserved), std::end(state->reserved),
                    [](uint8_t value) { return value != 0; }) ||
        (state->armed == 1 && state->deadline_ns < 0)) {
        return SetStageError(stage, "time state returned an invalid contract", EPROTO);
    }
    return 0;
}

int SynchronousBlockTransformChainTask::SetStageError(
    size_t stage,
    std::string detail,
    int rc) {
    last_error_ = "transform stage " + std::to_string(stage + 1) + ": " + detail;
    if (rc == 0) return -EIO;
    return rc > 0 ? -rc : rc;
}

std::string SynchronousBlockTransformChainTask::TaskError(size_t stage) const {
    if (stage >= tasks_.size() || !tasks_[stage]) return "task unavailable";
    try {
        const std::string detail = tasks_[stage]->LastError();
        return detail.empty() ? "task did not provide an error message" : detail;
    } catch (...) {
        return "task LastError threw";
    }
}

BlockTransformPipelineRunner::BlockTransformPipelineRunner(
    BlockTransformPipelineConfig config)
    : config_(std::move(config)) {}

void BlockTransformPipelineRunner::Cancel() {
    bool expected = false;
    if (!cancel_requested_.compare_exchange_strong(expected, true)) return;

    try {
        if (config_.source) config_.source->Cancel();
    } catch (...) {
    }
    try {
        if (config_.transform) config_.transform->Cancel();
    } catch (...) {
    }
}

BlockTransformPipelineError BlockTransformPipelineRunner::Run(
    BlockTransformPipelineResult* result,
    std::string* error) {
    if (error) error->clear();
    if (!result) {
        if (error) *error = "block transform pipeline result must not be null";
        return BlockTransformPipelineError::kInvalidArgument;
    }
    *result = BlockTransformPipelineResult{};

    if (!config_.source || !config_.source_schema || !config_.transform ||
        !config_.output_consumer || config_.poll_timeout_ms < 0) {
        if (error) {
            *error = "block transform pipeline requires source, source Schema, transform, "
                     "consumer, and a non-negative poll timeout";
        }
        return BlockTransformPipelineError::kInvalidArgument;
    }

    bool expected = false;
    if (!run_started_.compare_exchange_strong(expected, true)) {
        if (error) *error = "block transform pipeline can only run once";
        return BlockTransformPipelineError::kAlreadyRun;
    }
    if (cancel_requested_.load()) {
        result->terminal = BlockTransformPipelineTerminal::kCancelled;
        if (error) *error = "block transform pipeline was cancelled before Run";
        return BlockTransformPipelineError::kCancelled;
    }

    auto transform_error = [&]() {
        try {
            const std::string detail = config_.transform->LastError();
            if (!detail.empty()) return detail;
        } catch (...) {
        }
        return std::string("transform did not provide an error message");
    };
    auto fail = [&](BlockTransformPipelineError code, std::string message) {
        result->terminal = BlockTransformPipelineTerminal::kFailed;
        if (error) *error = std::move(message);
        Cancel();
        return code;
    };

    BlockFilterStage source_filter(config_.source_residual);
    std::shared_ptr<arrow::Schema> transform_input_schema;
    std::string stage_error;
    const auto source_open = source_filter.Open(
        config_.source_schema, &transform_input_schema, &stage_error);
    if (source_open != FilterEvalError::kNone) {
        if (stage_error.empty()) stage_error = "source residual filter could not open";
        return fail(BlockTransformPipelineError::kFilterOpenFailed,
                    "source residual filter Open failed: " + stage_error);
    }

    std::shared_ptr<arrow::Schema> transform_output_schema;
    int transform_open_rc = 0;
    try {
        transform_open_rc = config_.transform->Open(
            transform_input_schema, &transform_output_schema);
    } catch (const std::exception& ex) {
        return fail(BlockTransformPipelineError::kTransformOpenFailed,
                    std::string("transform Open threw: ") + ex.what());
    } catch (...) {
        return fail(BlockTransformPipelineError::kTransformOpenFailed,
                    "transform Open threw an unknown exception");
    }
    if (transform_open_rc != 0) {
        return fail(BlockTransformPipelineError::kTransformOpenFailed,
                    "transform Open failed: " + transform_error());
    }
    if (!transform_output_schema) {
        return fail(BlockTransformPipelineError::kTransformOpenFailed,
                    "transform Open succeeded without an output Schema");
    }

    BlockFilterStage output_filter(config_.transform_residual);
    std::shared_ptr<arrow::Schema> consumer_schema;
    stage_error.clear();
    const auto output_open = output_filter.Open(
        transform_output_schema, &consumer_schema, &stage_error);
    if (output_open != FilterEvalError::kNone) {
        if (stage_error.empty()) stage_error = "transform residual filter could not open";
        return fail(BlockTransformPipelineError::kFilterOpenFailed,
                    "transform residual filter Open failed: " + stage_error);
    }

    auto deliver_outputs = [&](const std::vector<BlockTransformOutputV1>& outputs,
                               std::string* delivery_error) {
        for (const auto& output : outputs) {
            if (!output.batch) {
                *delivery_error = "transform returned a null output batch";
                return BlockTransformPipelineError::kTransformContractViolation;
            }
        }
        for (const auto& output : outputs) {
            BlockTransformOutputV1 filtered_output;
            std::string filter_error;
            const auto filter_rc = output_filter.ProcessBlock(
                output.batch, output.ts_ms, &filtered_output, &filter_error);
            if (filter_rc != FilterEvalError::kNone) {
                if (filter_error.empty()) filter_error = "transform residual filter failed";
                *delivery_error = "transform residual filter failed: " + filter_error;
                return BlockTransformPipelineError::kOutputFilterFailed;
            }

            int consumer_rc = 0;
            try {
                consumer_rc = config_.output_consumer(filtered_output);
            } catch (const std::exception& ex) {
                *delivery_error = std::string("output consumer threw: ") + ex.what();
                return BlockTransformPipelineError::kOutputConsumerFailed;
            } catch (...) {
                *delivery_error = "output consumer threw an unknown exception";
                return BlockTransformPipelineError::kOutputConsumerFailed;
            }
            if (consumer_rc != 0) {
                *delivery_error = "output consumer failed with code " +
                                  std::to_string(consumer_rc);
                return BlockTransformPipelineError::kOutputConsumerFailed;
            }
            ++result->output_blocks;
            result->output_rows += filtered_output.batch->num_rows();
        }
        return BlockTransformPipelineError::kNone;
    };

    int64_t last_monotonic_now_ns = -1;
    auto read_monotonic_now = [&](int64_t* now_ns, std::string* clock_error) {
        int64_t value = -1;
        try {
            value = config_.monotonic_clock_ns
                        ? config_.monotonic_clock_ns()
                        : std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count();
        } catch (const std::exception& ex) {
            *clock_error = std::string("monotonic clock threw: ") + ex.what();
            return false;
        } catch (...) {
            *clock_error = "monotonic clock threw an unknown exception";
            return false;
        }
        if (value < 0) {
            *clock_error = "monotonic clock returned a negative timestamp";
            return false;
        }
        if (last_monotonic_now_ns >= 0 && value < last_monotonic_now_ns) {
            *clock_error = "monotonic clock moved backwards";
            return false;
        }
        last_monotonic_now_ns = value;
        *now_ns = value;
        return true;
    };
    auto read_wall_now = [&](int64_t* now_ns, std::string* clock_error) {
        try {
            *now_ns = config_.wall_clock_ns
                          ? config_.wall_clock_ns()
                          : std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
            return true;
        } catch (const std::exception& ex) {
            *clock_error = std::string("wall clock threw: ") + ex.what();
            return false;
        } catch (...) {
            *clock_error = "wall clock threw an unknown exception";
            return false;
        }
    };
    auto get_time_state = [&](BlockTransformTimeDriveStateV1* state,
                              std::string* state_error) {
        *state = BlockTransformTimeDriveStateV1{};
        state->struct_size = kBlockTransformTimeDriveStateV1Size;
        state->contract_version = kBlockTransformTimeDriveVersionV1;
        int state_rc = 0;
        try {
            state_rc = config_.time_transform->GetTimeDriveState(state);
        } catch (const std::exception& ex) {
            *state_error = std::string("time state query threw: ") + ex.what();
            return false;
        } catch (...) {
            *state_error = "time state query threw an unknown exception";
            return false;
        }
        if (state_rc != 0) {
            *state_error = "time state query failed with code " + std::to_string(state_rc);
            return false;
        }
        if (state->struct_size < kBlockTransformTimeDriveStateV1Size ||
            state->contract_version != kBlockTransformTimeDriveVersionV1 || state->armed > 1) {
            *state_error = "time state returned an invalid structure, version, or armed flag";
            return false;
        }
        if (std::any_of(std::begin(state->reserved), std::end(state->reserved),
                        [](uint8_t value) { return value != 0; })) {
            *state_error = "time state returned nonzero reserved bytes";
            return false;
        }
        if (state->armed == 1 && state->deadline_ns < 0) {
            *state_error = "time state returned a negative armed deadline";
            return false;
        }
        return true;
    };

    auto service_time = [&](int* poll_timeout_ms,
                            bool* fired,
                            bool* time_stopped,
                            std::string* time_error) {
        *poll_timeout_ms = config_.poll_timeout_ms;
        *fired = false;
        *time_stopped = false;
        if (!config_.time_transform) return BlockTransformPipelineError::kNone;

        int64_t monotonic_now_ns = 0;
        if (!read_monotonic_now(&monotonic_now_ns, time_error)) {
            return BlockTransformPipelineError::kTransformContractViolation;
        }
        BlockTransformTimeDriveStateV1 state{};
        if (!get_time_state(&state, time_error)) {
            return BlockTransformPipelineError::kTransformContractViolation;
        }
        if (state.armed == 0) return BlockTransformPipelineError::kNone;
        if (state.deadline_ns > monotonic_now_ns) {
            constexpr int64_t kNanosecondsPerMillisecond = 1000 * 1000;
            const int64_t deadline_wait_ms =
                (state.deadline_ns - monotonic_now_ns) / kNanosecondsPerMillisecond;
            *poll_timeout_ms = static_cast<int>(
                std::min<int64_t>(config_.poll_timeout_ms, deadline_wait_ms));
            return BlockTransformPipelineError::kNone;
        }

        int64_t wall_now_ns = 0;
        if (!read_wall_now(&wall_now_ns, time_error)) {
            return BlockTransformPipelineError::kTransformContractViolation;
        }
        BlockTransformTimeEventV1 event{};
        event.struct_size = kBlockTransformTimeEventV1Size;
        event.contract_version = kBlockTransformTimeDriveVersionV1;
        event.monotonic_now_ns = monotonic_now_ns;
        event.wall_now_ns = wall_now_ns;
        std::vector<BlockTransformOutputV1> outputs;
        int time_rc = 0;
        try {
            time_rc = config_.time_transform->OnTime(event, &outputs);
        } catch (const std::exception& ex) {
            *time_error = std::string("transform OnTime threw: ") + ex.what();
            return outputs.empty() ? BlockTransformPipelineError::kTransformFailed
                                   : BlockTransformPipelineError::kTransformContractViolation;
        } catch (...) {
            *time_error = "transform OnTime threw an unknown exception";
            return outputs.empty() ? BlockTransformPipelineError::kTransformFailed
                                   : BlockTransformPipelineError::kTransformContractViolation;
        }

        const bool valid_status =
            time_rc == static_cast<int>(BlockTransformStatusV1::kContinue) ||
            time_rc == static_cast<int>(BlockTransformStatusV1::kStop);
        if (!valid_status) {
            if (!outputs.empty()) {
                *time_error = "transform OnTime failed while returning output batches";
                return BlockTransformPipelineError::kTransformContractViolation;
            }
            *time_error = "transform OnTime failed: " + transform_error();
            return BlockTransformPipelineError::kTransformFailed;
        }

        if (time_rc == static_cast<int>(BlockTransformStatusV1::kContinue)) {
            BlockTransformTimeDriveStateV1 next_state{};
            if (!get_time_state(&next_state, time_error)) {
                return BlockTransformPipelineError::kTransformContractViolation;
            }
            if (next_state.armed == 1 && next_state.deadline_ns <= monotonic_now_ns) {
                *time_error = "transform OnTime made no deadline progress";
                return BlockTransformPipelineError::kTransformContractViolation;
            }
        }

        const auto delivery_rc = deliver_outputs(outputs, time_error);
        if (delivery_rc != BlockTransformPipelineError::kNone) return delivery_rc;
        *fired = true;
        *time_stopped = time_rc == static_cast<int>(BlockTransformStatusV1::kStop);
        return BlockTransformPipelineError::kNone;
    };

    bool stopped = false;
    while (true) {
        if (cancel_requested_.load()) {
            result->terminal = BlockTransformPipelineTerminal::kCancelled;
            if (error) *error = "block transform pipeline was cancelled";
            return BlockTransformPipelineError::kCancelled;
        }

        int poll_timeout_ms = config_.poll_timeout_ms;
        bool time_fired = false;
        bool time_stopped = false;
        std::string time_error;
        const auto time_rc = service_time(
            &poll_timeout_ms, &time_fired, &time_stopped, &time_error);
        if (time_rc != BlockTransformPipelineError::kNone) {
            return fail(time_rc, std::move(time_error));
        }
        if (time_stopped) {
            stopped = true;
            break;
        }
        if (time_fired) continue;

        BlockPollEvent event;
        try {
            event = config_.source->PollBlock(poll_timeout_ms);
        } catch (const std::exception& ex) {
            return fail(BlockTransformPipelineError::kSourcePollFailed,
                        std::string("block source PollBlock threw: ") + ex.what());
        } catch (...) {
            return fail(BlockTransformPipelineError::kSourcePollFailed,
                        "block source PollBlock threw an unknown exception");
        }

        if (event.kind == BlockPollEvent::kTimeout) continue;
        if (event.kind == BlockPollEvent::kEof) break;
        if (event.kind == BlockPollEvent::kCancelled) {
            result->terminal = BlockTransformPipelineTerminal::kCancelled;
            if (error) {
                *error = "block source was cancelled";
                if (event.err != 0) {
                    *error += " with code " + std::to_string(event.err);
                }
            }
            Cancel();
            return BlockTransformPipelineError::kCancelled;
        }
        if (event.kind == BlockPollEvent::kError) {
            return fail(BlockTransformPipelineError::kSourcePollFailed,
                        "block source PollBlock failed with code " +
                            std::to_string(event.err));
        }
        if (event.kind != BlockPollEvent::kData || !event.batch) {
            return fail(BlockTransformPipelineError::kSourcePollFailed,
                        "block source returned an invalid data event");
        }

        ++result->input_blocks;
        result->input_rows += event.batch->num_rows();

        BlockTransformPipelineError block_error = BlockTransformPipelineError::kNone;
        std::string block_error_message;
        int transform_rc = static_cast<int>(BlockTransformStatusV1::kContinue);
        std::vector<BlockTransformOutputV1> outputs;

        if (cancel_requested_.load()) {
            block_error = BlockTransformPipelineError::kCancelled;
            block_error_message = "block transform pipeline was cancelled";
        } else {
            BlockTransformOutputV1 filtered_input;
            stage_error.clear();
            const auto filter_rc = source_filter.ProcessBlock(
                event.batch, 0, &filtered_input, &stage_error);
            if (filter_rc != FilterEvalError::kNone) {
                if (stage_error.empty()) stage_error = "source residual filter failed";
                block_error = BlockTransformPipelineError::kOutputFilterFailed;
                block_error_message = "source residual filter failed: " + stage_error;
            } else {
                try {
                    transform_rc = config_.transform->ProcessBlock(
                        filtered_input.batch, 0, &outputs);
                } catch (const std::exception& ex) {
                    block_error = outputs.empty()
                                      ? BlockTransformPipelineError::kTransformFailed
                                      : BlockTransformPipelineError::kTransformContractViolation;
                    block_error_message = std::string("transform ProcessBlock threw: ") +
                                          ex.what();
                } catch (...) {
                    block_error = outputs.empty()
                                      ? BlockTransformPipelineError::kTransformFailed
                                      : BlockTransformPipelineError::kTransformContractViolation;
                    block_error_message = "transform ProcessBlock threw an unknown exception";
                }
            }
        }

        if (block_error == BlockTransformPipelineError::kNone) {
            const bool valid_status =
                transform_rc == static_cast<int>(BlockTransformStatusV1::kContinue) ||
                transform_rc == static_cast<int>(BlockTransformStatusV1::kStop);
            if (!valid_status) {
                if (!outputs.empty()) {
                    block_error = BlockTransformPipelineError::kTransformContractViolation;
                    block_error_message =
                        "transform ProcessBlock failed while returning output batches";
                } else {
                    block_error = BlockTransformPipelineError::kTransformFailed;
                    block_error_message = "transform ProcessBlock failed: " + transform_error();
                }
            } else {
                block_error = deliver_outputs(outputs, &block_error_message);
            }
        }

        int release_rc = 0;
        try {
            release_rc = config_.source->ReleaseBlock(event.batch);
        } catch (const std::exception& ex) {
            block_error = BlockTransformPipelineError::kSourceReleaseFailed;
            block_error_message = std::string("block source ReleaseBlock threw: ") + ex.what();
        } catch (...) {
            block_error = BlockTransformPipelineError::kSourceReleaseFailed;
            block_error_message = "block source ReleaseBlock threw an unknown exception";
        }
        if (release_rc != 0) {
            block_error = BlockTransformPipelineError::kSourceReleaseFailed;
            block_error_message = "block source ReleaseBlock failed with code " +
                                  std::to_string(release_rc);
        }

        if (block_error == BlockTransformPipelineError::kCancelled) {
            result->terminal = BlockTransformPipelineTerminal::kCancelled;
            if (error) *error = std::move(block_error_message);
            Cancel();
            return BlockTransformPipelineError::kCancelled;
        }
        if (block_error != BlockTransformPipelineError::kNone) {
            return fail(block_error, std::move(block_error_message));
        }
        if (transform_rc == static_cast<int>(BlockTransformStatusV1::kStop)) {
            stopped = true;
            break;
        }
    }

    if (cancel_requested_.load()) {
        result->terminal = BlockTransformPipelineTerminal::kCancelled;
        if (error) *error = "block transform pipeline was cancelled before Flush";
        return BlockTransformPipelineError::kCancelled;
    }

    std::vector<BlockTransformOutputV1> flush_outputs;
    int flush_rc = 0;
    try {
        flush_rc = config_.transform->Flush(&flush_outputs);
    } catch (const std::exception& ex) {
        return fail(flush_outputs.empty()
                        ? BlockTransformPipelineError::kTransformFailed
                        : BlockTransformPipelineError::kTransformContractViolation,
                    std::string("transform Flush threw: ") + ex.what());
    } catch (...) {
        return fail(flush_outputs.empty()
                        ? BlockTransformPipelineError::kTransformFailed
                        : BlockTransformPipelineError::kTransformContractViolation,
                    "transform Flush threw an unknown exception");
    }
    if (flush_rc != 0) {
        if (!flush_outputs.empty()) {
            return fail(BlockTransformPipelineError::kTransformContractViolation,
                        "transform Flush failed while returning output batches");
        }
        return fail(BlockTransformPipelineError::kTransformFailed,
                    "transform Flush failed: " + transform_error());
    }

    std::string delivery_error;
    const auto delivery_rc = deliver_outputs(flush_outputs, &delivery_error);
    if (delivery_rc != BlockTransformPipelineError::kNone) {
        return fail(delivery_rc, std::move(delivery_error));
    }

    result->terminal = stopped ? BlockTransformPipelineTerminal::kStopped
                               : BlockTransformPipelineTerminal::kCompleted;
    return BlockTransformPipelineError::kNone;
}

}  // namespace flowsql
