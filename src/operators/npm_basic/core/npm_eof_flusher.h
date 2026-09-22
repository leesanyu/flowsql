// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_OPERATORS_NPM_BASIC_NPM_EOF_FLUSHER_H_
#define _FLOWSQL_OPERATORS_NPM_BASIC_NPM_EOF_FLUSHER_H_

#include <operators/npm_basic/output/npm_basic_result_collector.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace flowsql::npm {

enum class NpmEofFlushState : uint8_t {
    kOpen = 0,
    kFlushed,
    kFailed,
    kCancelled,
};

enum class NpmEofFlushError : uint8_t {
    kNone = 0,
    kNullOutput,
    kNullBudget,
    kNullModule,
    kAlreadyFlushed,
    kCancelled,
    kFailedState,
    kSessionError,
    kModuleError,
    kDrainError,
    kAllocationFailed,
};

struct NpmEofFlushStatus {
    NpmEofFlushError error = NpmEofFlushError::kNone;
    NpmSessionTableError session_error = NpmSessionTableError::kNone;
    int module_error = 0;
    NpmBasicDrainStatus drain_status;
};

/** Single-threaded task terminal: normal EOF may flush exactly once while open. */
class NpmEofFlusher final {
 public:
    NpmEofFlusher() = default;
    NpmEofFlusher(const NpmEofFlusher&) = delete;
    NpmEofFlusher& operator=(const NpmEofFlusher&) = delete;
    NpmEofFlusher(NpmEofFlusher&&) = delete;
    NpmEofFlusher& operator=(NpmEofFlusher&&) = delete;

    void Cancel() noexcept;
    void MarkFailed() noexcept;
    NpmEofFlushState state() const noexcept { return state_; }

    NpmEofFlushStatus Flush(int64_t observed_at, NpmSessionTable& sessions,
                            const std::vector<INpmAnalysisModule*>& modules, NpmBasicResultCollector& collector,
                            NpmBasicResultProjector& projector, const std::shared_ptr<INpmTaskBudget>& budget,
                            std::shared_ptr<arrow::RecordBatch>* output);

 private:
    NpmEofFlushState state_ = NpmEofFlushState::kOpen;
};

}  // namespace flowsql::npm

#endif  // _FLOWSQL_OPERATORS_NPM_BASIC_NPM_EOF_FLUSHER_H_
