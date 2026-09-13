// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_PLUGINS_NPM_BASIC_NPM_BASIC_TASK_RUNTIME_H_
#define _FLOWSQL_PLUGINS_NPM_BASIC_NPM_BASIC_TASK_RUNTIME_H_

#include "npm_basic_result_collector.h"
#include "npm_basic_task_config.h"
#include "npm_eof_flusher.h"
#include "npm_protocol_context.h"
#include "npm_session_table.h"
#include "npm_task_budget.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace arrow {
class Schema;
}

namespace flowsql::npm {

enum class NpmBasicTaskRuntimeError : uint8_t {
    kNone = 0,
    kNullInputSchema,
    kNullOutputSchema,
    kNullRuntimeOutput,
    kSchemaMismatch,
    kTimeCapabilityError,
    kProtocolContextError,
    kAllocationFailed,
};

struct NpmBasicTaskRuntimeStatus {
    NpmBasicTaskRuntimeError error = NpmBasicTaskRuntimeError::kNone;
    NpmTimeCapabilityError time_error = NpmTimeCapabilityError::kNone;
    NpmProtocolContextError protocol_error = NpmProtocolContextError::kNone;
};

/** Fully initialized task-private state published atomically by Create(). */
class NpmBasicTaskRuntime final {
 public:
    static NpmBasicTaskRuntimeStatus Create(
        const NpmBasicTaskConfig& config,
        IQuerier* querier,
        const std::shared_ptr<arrow::Schema>& input_schema,
        std::shared_ptr<arrow::Schema>* output_schema,
        std::unique_ptr<NpmBasicTaskRuntime>* output);

    ~NpmBasicTaskRuntime() = default;
    NpmBasicTaskRuntime(const NpmBasicTaskRuntime&) = delete;
    NpmBasicTaskRuntime& operator=(const NpmBasicTaskRuntime&) = delete;
    NpmBasicTaskRuntime(NpmBasicTaskRuntime&&) = delete;
    NpmBasicTaskRuntime& operator=(NpmBasicTaskRuntime&&) = delete;

    const NpmBasicTaskConfig& Config() const noexcept;
    NpmProtocolContext& ProtocolContext() noexcept;
    std::shared_ptr<INpmTaskBudget> Budget() const;
    NpmSessionTable& Sessions() noexcept;
    NpmBasicResultCollector& Collector() noexcept;
    NpmBasicResultProjector& Projector() noexcept;
    NpmEofFlusher& EofFlusher() noexcept;
    const std::vector<INpmAnalysisModule*>& Modules() const noexcept;

 private:
    NpmBasicTaskRuntime(NpmBasicTaskConfig config,
                        std::unique_ptr<NpmProtocolContext> protocol_context,
                        std::shared_ptr<NpmTaskBudget> budget);

    NpmBasicTaskConfig config_;
    std::unique_ptr<NpmProtocolContext> protocol_context_;
    std::shared_ptr<NpmTaskBudget> budget_;
    NpmSessionTable sessions_;
    NpmBasicResultCollector collector_;
    NpmBasicResultProjector projector_;
    NpmEofFlusher eof_flusher_;
    std::vector<INpmAnalysisModule*> modules_;
};

}  // namespace flowsql::npm

#endif  // _FLOWSQL_PLUGINS_NPM_BASIC_NPM_BASIC_TASK_RUNTIME_H_
