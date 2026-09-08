// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "pipeline.h"

#include <arrow/api.h>

#include <cstdio>
#include <common/log.h>
#include <exception>
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

    bool stopped = false;
    while (true) {
        if (cancel_requested_.load()) {
            result->terminal = BlockTransformPipelineTerminal::kCancelled;
            if (error) *error = "block transform pipeline was cancelled";
            return BlockTransformPipelineError::kCancelled;
        }

        BlockPollEvent event;
        try {
            event = config_.source->PollBlock(config_.poll_timeout_ms);
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
