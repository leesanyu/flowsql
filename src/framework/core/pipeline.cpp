#include "pipeline.h"

#include <cstdio>
#include <common/log.h>

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
        error_message_ = "operator " + operator_->Catelog() + "." + operator_->Name() + " execution failed";
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

}  // namespace flowsql
