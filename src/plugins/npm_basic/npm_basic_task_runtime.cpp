// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_basic_task_runtime.h"

#include <framework/core/packet_codec.h>

#include <arrow/api.h>

#include <new>
#include <utility>

namespace flowsql::npm {

NpmBasicTaskRuntimeStatus NpmBasicTaskRuntime::Create(
    const NpmBasicTaskConfig& config,
    IQuerier* querier,
    const std::shared_ptr<arrow::Schema>& input_schema,
    std::shared_ptr<arrow::Schema>* output_schema,
    std::unique_ptr<NpmBasicTaskRuntime>* output) {
    NpmBasicTaskRuntimeStatus status;
    if (!input_schema) {
        status.error = NpmBasicTaskRuntimeError::kNullInputSchema;
        return status;
    }
    if (!output_schema) {
        status.error = NpmBasicTaskRuntimeError::kNullOutputSchema;
        return status;
    }
    if (!output) {
        status.error = NpmBasicTaskRuntimeError::kNullRuntimeOutput;
        return status;
    }
    const auto packet_schema = packet::PacketSchema();
    if (!packet_schema || !input_schema->Equals(*packet_schema, true)) {
        status.error = NpmBasicTaskRuntimeError::kSchemaMismatch;
        return status;
    }

    status.time_error = ValidateNpmTimeCapabilities(config.analysis, {});
    if (status.time_error != NpmTimeCapabilityError::kNone) {
        status.error = NpmBasicTaskRuntimeError::kTimeCapabilityError;
        return status;
    }

    std::unique_ptr<NpmProtocolContext> protocol_context;
    status.protocol_error = NpmProtocolContext::Create(querier, &protocol_context);
    if (status.protocol_error != NpmProtocolContextError::kNone) {
        status.error = NpmBasicTaskRuntimeError::kProtocolContextError;
        return status;
    }

    try {
        auto budget = std::make_shared<NpmTaskBudget>(config.analysis);
        std::unique_ptr<NpmBasicTaskRuntime> runtime(
            new NpmBasicTaskRuntime(config, std::move(protocol_context), std::move(budget)));
        auto result_schema = NpmBasicResultSchema();
        *output_schema = std::move(result_schema);
        *output = std::move(runtime);
        return status;
    } catch (const std::bad_alloc&) {
        status.error = NpmBasicTaskRuntimeError::kAllocationFailed;
        return status;
    }
}

NpmBasicTaskRuntime::NpmBasicTaskRuntime(NpmBasicTaskConfig config,
                                         std::unique_ptr<NpmProtocolContext> protocol_context,
                                         std::shared_ptr<NpmTaskBudget> budget)
    : config_(std::move(config)),
      protocol_context_(std::move(protocol_context)),
      budget_(std::move(budget)),
      sessions_(config_.analysis, budget_),
      projector_(*protocol_context_) {}

const NpmBasicTaskConfig& NpmBasicTaskRuntime::Config() const noexcept {
    return config_;
}

NpmProtocolContext& NpmBasicTaskRuntime::ProtocolContext() noexcept {
    return *protocol_context_;
}

std::shared_ptr<INpmTaskBudget> NpmBasicTaskRuntime::Budget() const {
    return budget_;
}

NpmSessionTable& NpmBasicTaskRuntime::Sessions() noexcept {
    return sessions_;
}

NpmBasicResultCollector& NpmBasicTaskRuntime::Collector() noexcept {
    return collector_;
}

NpmBasicResultProjector& NpmBasicTaskRuntime::Projector() noexcept {
    return projector_;
}

NpmEofFlusher& NpmBasicTaskRuntime::EofFlusher() noexcept {
    return eof_flusher_;
}

const std::vector<INpmAnalysisModule*>& NpmBasicTaskRuntime::Modules() const noexcept {
    return modules_;
}

}  // namespace flowsql::npm
