// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_OPERATORS_NPM_BASIC_NPM_BASIC_TASK_RUNTIME_H_
#define _FLOWSQL_OPERATORS_NPM_BASIC_NPM_BASIC_TASK_RUNTIME_H_

#include "npm_eof_flusher.h"
#include "npm_protocol_context.h"
#include "npm_session_table.h"
#include "npm_task_budget.h"

#include <framework/interfaces/iflow_labeling.h>
#include <operators/npm_basic/config/npm_basic_task_config.h>
#include <operators/npm_basic/modules/session/npm_session_analysis_module.h>
#include <operators/npm_basic/output/npm_basic_result_collector.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace arrow {
class RecordBatch;
class Schema;
}  // namespace arrow

namespace flowsql::npm {

enum class NpmBasicTaskRuntimeError : uint8_t {
    kNone = 0,
    kNullInputSchema,
    kNullOutputSchema,
    kNullRuntimeOutput,
    kSchemaMismatch,
    kTimeCapabilityError,
    kProtocolContextError,
    kLabelingMatcherMissing,
    kAllocationFailed,
    kModulePlanError,
    kModuleCreateError,
};

struct NpmBasicTaskRuntimeStatus {
    NpmBasicTaskRuntimeError error = NpmBasicTaskRuntimeError::kNone;
    NpmTimeCapabilityError time_error = NpmTimeCapabilityError::kNone;
    NpmProtocolContextError protocol_error = NpmProtocolContextError::kNone;
    NpmProtocolContractStatusV1 module_status;
};

enum class NpmBasicOfflineBatchError : uint8_t {
    kNone = 0,
    kTerminalState,
    kCancelled,
    kNullInput,
    kNullOutput,
    kInvalidBufferSize,
    kInputBudgetError,
    kBatchViewError,
    kBatchProcessError,
    kDrainError,
    kAllocationFailed,
};

struct NpmBasicOfflineBatchStatus {
    NpmBasicOfflineBatchError error = NpmBasicOfflineBatchError::kNone;
    NpmEofFlushState runtime_state = NpmEofFlushState::kOpen;
    uint64_t input_bytes = 0;
    NpmBudgetError budget_error = NpmBudgetError::kNone;
    NpmPacketBatchError batch_view_error = NpmPacketBatchError::kNone;
    NpmPacketBatchProcessStatus process_status;
    NpmBasicDrainStatus drain_status;
};

struct NpmBasicRealtimeMaintenanceInput {
    int64_t monotonic_now_ns = 0;
    int64_t observed_at_ns = 0;
    NpmCaptureProgressUpdate capture_progress;
};

enum class NpmBasicRealtimeMaintenanceError : uint8_t {
    kNone = 0,
    kTerminalState,
    kCancelled,
    kInvalidRunMode,
    kNullOutput,
    kMonotonicTimeRegression,
    kSessionError,
    kModuleError,
    kProjectionError,
    kWriterError,
    kDrainError,
    kAllocationFailed,
};

struct NpmBasicRealtimeMaintenanceStatus {
    NpmBasicRealtimeMaintenanceError error = NpmBasicRealtimeMaintenanceError::kNone;
    NpmEofFlushState runtime_state = NpmEofFlushState::kOpen;
    NpmCaptureProgressDisposition progress_disposition = NpmCaptureProgressDisposition::kUnchanged;
    bool snapshot_due = false;
    bool emitted = false;
    size_t active_sessions = 0;
    size_t ended_sessions = 0;
    int64_t active_session_index = -1;
    NpmSessionTableError session_error = NpmSessionTableError::kNone;
    int module_error = 0;
    NpmBasicProjectionError projection_error = NpmBasicProjectionError::kNone;
    int writer_error = 0;
    NpmBasicDrainStatus drain_status;
};

/** Fully initialized task-private state published atomically by Create(). */
class NpmBasicTaskRuntime final {
 public:
    static NpmBasicTaskRuntimeStatus Create(
        const NpmBasicTaskConfig& config, IQuerier* querier, const std::shared_ptr<arrow::Schema>& input_schema,
        std::shared_ptr<arrow::Schema>* output_schema, std::unique_ptr<NpmBasicTaskRuntime>* output,
        std::shared_ptr<NpmTaskBudget> budget = {}, IFlowLabelMatcherV1* matcher = nullptr,
        const NpmModuleCatalogV1& catalog = ProductionNpmModuleCatalogV1(),
        std::unique_ptr<INpmResultConsumerV1> consumer = {}, std::string task_id = {});
    static NpmBasicTaskRuntimeStatus CreateWithTimeCapabilities(
        const NpmBasicTaskConfig& config, IQuerier* querier, const std::shared_ptr<arrow::Schema>& input_schema,
        const NpmTimeCapabilities& time_capabilities, std::shared_ptr<arrow::Schema>* output_schema,
        std::unique_ptr<NpmBasicTaskRuntime>* output, std::shared_ptr<NpmTaskBudget> budget = {},
        IFlowLabelMatcherV1* matcher = nullptr, const NpmModuleCatalogV1& catalog = ProductionNpmModuleCatalogV1(),
        std::unique_ptr<INpmResultConsumerV1> consumer = {}, std::string task_id = {});

    ~NpmBasicTaskRuntime();
    const NpmResultContextV1& ResultContext() const { return router_->Context(); }
    NpmBasicTaskRuntime(const NpmBasicTaskRuntime&) = delete;
    NpmBasicTaskRuntime& operator=(const NpmBasicTaskRuntime&) = delete;
    NpmBasicTaskRuntime(NpmBasicTaskRuntime&&) = delete;
    NpmBasicTaskRuntime& operator=(NpmBasicTaskRuntime&&) = delete;

    const NpmBasicTaskConfig& Config() const noexcept;
    NpmBasicOfflineBatchStatus ProcessOfflineBatch(const std::shared_ptr<arrow::RecordBatch>& input,
                                                   std::shared_ptr<arrow::RecordBatch>* output);
    NpmBasicRealtimeMaintenanceStatus DriveRealtimeMaintenance(const NpmBasicRealtimeMaintenanceInput& input,
                                                               std::shared_ptr<arrow::RecordBatch>* output);
    NpmEofFlushStatus FlushOffline(int64_t observed_at, std::shared_ptr<arrow::RecordBatch>* output);
    NpmMaintenancePlanV1 MaintenancePlan() const;
    void Cancel() noexcept;
    std::string LastError() const;
    /** Canonical lifecycle state; unlike component accessors, valid after resources are released. */
    NpmEofFlushState State() const noexcept;
    /** The following component accessors are valid only while State() is kOpen. */
    NpmProtocolContext& ProtocolContext() noexcept;
    std::shared_ptr<INpmTaskBudget> Budget() const;
    NpmSessionTable& Sessions() noexcept;
    NpmBasicResultCollector& Collector() noexcept;
    NpmBasicResultProjector& Projector() noexcept;
    NpmEofFlusher& EofFlusher() noexcept;
    const std::vector<INpmAnalysisModule*>& Modules() const noexcept;

 private:
    struct MatcherReleaser {
        void operator()(IFlowLabelMatcherV1* matcher) const noexcept {
            if (matcher != nullptr) matcher->Release();
        }
    };
    using MatcherLease = std::unique_ptr<IFlowLabelMatcherV1, MatcherReleaser>;

    NpmBasicTaskRuntime(NpmBasicTaskConfig config, std::unique_ptr<NpmProtocolContext> protocol_context,
                        std::shared_ptr<NpmTaskBudget> budget, MatcherLease matcher);
    void SetLastErrorOnce(const char* error) noexcept;
    void ReleaseResources() noexcept;

    std::shared_ptr<NpmResultRouter> router_;
    NpmBasicTaskConfig config_;
    std::unique_ptr<NpmProtocolContext> protocol_context_;
    std::shared_ptr<NpmTaskBudget> budget_;
    MatcherLease matcher_;
    std::unique_ptr<NpmSessionTable> sessions_;
    std::unique_ptr<NpmBasicResultCollector> collector_;
    std::unique_ptr<NpmBasicResultProjector> projector_;
    NpmEofFlusher eof_flusher_;
    std::vector<NpmPreparedModuleV1> prepared_modules_;
    std::vector<std::unique_ptr<INpmAnalysisModule>> owned_modules_;
    std::vector<NpmProtocolModuleAdapter*> protocol_modules_;
    std::vector<INpmAnalysisModule*> modules_;
    bool realtime_clock_initialized_ = false;
    int64_t last_realtime_drive_ns_ = 0;
    int64_t last_realtime_snapshot_ns_ = 0;
    mutable std::mutex operation_mutex_;
    std::condition_variable operation_done_;
    std::atomic<bool> cancellation_requested_{false};
    std::atomic<bool> operation_active_{false};
    std::atomic<NpmEofFlushState> state_{NpmEofFlushState::kOpen};
    std::atomic<const char*> last_error_{nullptr};
};

}  // namespace flowsql::npm

#endif  // _FLOWSQL_OPERATORS_NPM_BASIC_NPM_BASIC_TASK_RUNTIME_H_
