// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_OPERATORS_NPM_BASIC_NPM_BASIC_TASK_RUNTIME_H_
#define _FLOWSQL_OPERATORS_NPM_BASIC_NPM_BASIC_TASK_RUNTIME_H_

#include "npm_basic_result_collector.h"
#include "npm_basic_task_config.h"
#include "npm_eof_flusher.h"
#include "npm_protocol_context.h"
#include "npm_session_analysis_module.h"
#include "npm_session_table.h"
#include "npm_task_budget.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace arrow {
class RecordBatch;
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
        const NpmBasicTaskConfig& config,
        IQuerier* querier,
        const std::shared_ptr<arrow::Schema>& input_schema,
        std::shared_ptr<arrow::Schema>* output_schema,
        std::unique_ptr<NpmBasicTaskRuntime>* output);
    static NpmBasicTaskRuntimeStatus CreateWithTimeCapabilities(
        const NpmBasicTaskConfig& config,
        IQuerier* querier,
        const std::shared_ptr<arrow::Schema>& input_schema,
        const NpmTimeCapabilities& time_capabilities,
        std::shared_ptr<arrow::Schema>* output_schema,
        std::unique_ptr<NpmBasicTaskRuntime>* output);

    ~NpmBasicTaskRuntime() = default;
    NpmBasicTaskRuntime(const NpmBasicTaskRuntime&) = delete;
    NpmBasicTaskRuntime& operator=(const NpmBasicTaskRuntime&) = delete;
    NpmBasicTaskRuntime(NpmBasicTaskRuntime&&) = delete;
    NpmBasicTaskRuntime& operator=(NpmBasicTaskRuntime&&) = delete;

    const NpmBasicTaskConfig& Config() const noexcept;
    NpmBasicOfflineBatchStatus ProcessOfflineBatch(
        const std::shared_ptr<arrow::RecordBatch>& input,
        std::shared_ptr<arrow::RecordBatch>* output);
    NpmBasicRealtimeMaintenanceStatus DriveRealtimeMaintenance(
        const NpmBasicRealtimeMaintenanceInput& input,
        std::shared_ptr<arrow::RecordBatch>* output);
    NpmEofFlushStatus FlushOffline(int64_t observed_at,
                                   std::shared_ptr<arrow::RecordBatch>* output);
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
    NpmBasicTaskRuntime(NpmBasicTaskConfig config,
                        std::unique_ptr<NpmProtocolContext> protocol_context,
                        std::shared_ptr<NpmTaskBudget> budget);
    void SetLastErrorOnce(const char* error) noexcept;
    void ReleaseResources() noexcept;

    NpmBasicTaskConfig config_;
    std::unique_ptr<NpmProtocolContext> protocol_context_;
    std::shared_ptr<NpmTaskBudget> budget_;
    std::unique_ptr<NpmSessionTable> sessions_;
    std::unique_ptr<NpmBasicResultCollector> collector_;
    std::unique_ptr<NpmBasicResultProjector> projector_;
    NpmEofFlusher eof_flusher_;
    std::unique_ptr<NpmSessionAnalysisModule> session_module_;
    std::vector<INpmAnalysisModule*> modules_;
    bool realtime_clock_initialized_ = false;
    int64_t last_realtime_drive_ns_ = 0;
    int64_t last_realtime_snapshot_ns_ = 0;
    mutable std::mutex operation_mutex_;
    std::atomic<bool> operation_active_{false};
    std::atomic<NpmEofFlushState> state_{NpmEofFlushState::kOpen};
    std::atomic<const char*> last_error_{nullptr};
};

}  // namespace flowsql::npm

#endif  // _FLOWSQL_OPERATORS_NPM_BASIC_NPM_BASIC_TASK_RUNTIME_H_
