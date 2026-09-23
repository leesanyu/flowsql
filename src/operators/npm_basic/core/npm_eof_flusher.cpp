// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_eof_flusher.h"

#include <new>
#include <utility>

namespace flowsql::npm {
namespace {

NpmEofFlushStatus TerminalStatus(NpmEofFlushState state) {
    NpmEofFlushStatus status;
    if (state == NpmEofFlushState::kFlushed) {
        status.error = NpmEofFlushError::kAlreadyFlushed;
    } else if (state == NpmEofFlushState::kCancelled) {
        status.error = NpmEofFlushError::kCancelled;
    } else if (state == NpmEofFlushState::kFailed) {
        status.error = NpmEofFlushError::kFailedState;
    }
    return status;
}

}  // namespace

void NpmEofFlusher::Cancel() noexcept {
    if (state_ == NpmEofFlushState::kOpen) state_ = NpmEofFlushState::kCancelled;
}

void NpmEofFlusher::MarkFailed() noexcept {
    if (state_ == NpmEofFlushState::kOpen) state_ = NpmEofFlushState::kFailed;
}

NpmEofFlushStatus NpmEofFlusher::Flush(int64_t observed_at, NpmSessionTable& sessions,
                                       const std::vector<INpmAnalysisModule*>& modules,
                                       const std::vector<NpmProtocolModuleAdapter*>& protocol_modules,
                                       NpmBasicResultCollector& collector, NpmBasicResultProjector& projector,
                                       const std::shared_ptr<INpmTaskBudget>& budget,
                                       std::shared_ptr<arrow::RecordBatch>* output) {
    if (state_ != NpmEofFlushState::kOpen) return TerminalStatus(state_);

    NpmEofFlushStatus status;
    if (output == nullptr) {
        status.error = NpmEofFlushError::kNullOutput;
        state_ = NpmEofFlushState::kFailed;
        return status;
    }
    if (budget == nullptr) {
        status.error = NpmEofFlushError::kNullBudget;
        state_ = NpmEofFlushState::kFailed;
        return status;
    }
    for (auto* module : modules) {
        if (module == nullptr) {
            status.error = NpmEofFlushError::kNullModule;
            state_ = NpmEofFlushState::kFailed;
            return status;
        }
    }

    try {
        std::vector<NpmSessionSnapshot> snapshots;
        status.session_error = sessions.FinishAllAtEof(&snapshots);
        if (status.session_error != NpmSessionTableError::kNone) {
            status.error = NpmEofFlushError::kSessionError;
            state_ = NpmEofFlushState::kFailed;
            return status;
        }

        std::vector<NpmSessionEndEvent> events;
        events.reserve(snapshots.size());
        status.module_error = NotifyNpmSessionEnd(snapshots, modules, observed_at, collector);
        if (status.module_error != 0) {
            status.error = NpmEofFlushError::kModuleError;
            state_ = NpmEofFlushState::kFailed;
            return status;
        }
        for (auto& snapshot : snapshots) {
            events.push_back(NpmSessionEndEvent{std::move(snapshot), observed_at});
        }
        for (auto* module : protocol_modules) {
            status.module_error = module->Finish(observed_at);
            if (status.module_error != 0) {
                status.error = NpmEofFlushError::kModuleError;
                state_ = NpmEofFlushState::kFailed;
                return status;
            }
        }

        status.drain_status = collector.Drain(events, projector, budget, output);
        if (status.drain_status.error != NpmBasicDrainError::kNone) {
            status.error = NpmEofFlushError::kDrainError;
            state_ = NpmEofFlushState::kFailed;
            return status;
        }

        state_ = NpmEofFlushState::kFlushed;
        return status;
    } catch (const std::bad_alloc&) {
        status.error = NpmEofFlushError::kAllocationFailed;
        state_ = NpmEofFlushState::kFailed;
        return status;
    }
}

}  // namespace flowsql::npm
