// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_eof_flusher.h"

#include "npm_tcp_stream_provider.h"

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
                                       std::shared_ptr<arrow::RecordBatch>* output, NpmTcpStreamProvider* streams) {
    struct Cleanup {
        NpmEofFlusher& flusher;
        NpmBasicResultCollector& collector;
        ~Cleanup() {
            if (flusher.state() == NpmEofFlushState::kFailed || flusher.state() == NpmEofFlushState::kCancelled)
                collector.Abort();
        }
    } cleanup{*this, collector};
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
        NpmSessionSnapshotBatch snapshots;
        status.session_error = sessions.FinishAllAtEof(&snapshots);
        if (status.session_error != NpmSessionTableError::kNone) {
            status.error = NpmEofFlushError::kSessionError;
            state_ = NpmEofFlushState::kFailed;
            return status;
        }

        snapshots.SetObservedAt(observed_at);
        if (streams) {
            for (size_t row = 0; row < snapshots.size(); ++row) {
                if (streams->OnSessionEnd(snapshots.View(row), snapshots.Row(row).facts.end_reason, observed_at) != 0) {
                    status.error = NpmEofFlushError::kModuleError;
                    status.module_error = EIO;
                    state_ = NpmEofFlushState::kFailed;
                    return status;
                }
            }
        }
        status.module_error = NotifyNpmSessionEnd(snapshots, modules, observed_at, collector);
        if (status.module_error != 0) {
            status.error = NpmEofFlushError::kModuleError;
            state_ = NpmEofFlushState::kFailed;
            return status;
        }
        {
            status.drain_status = collector.RouteEnds(snapshots, projector);
            if (status.drain_status.error != NpmBasicDrainError::kNone) {
                status.error = NpmEofFlushError::kDrainError;
                state_ = NpmEofFlushState::kFailed;
                return status;
            }
            snapshots.Clear();
        }
        for (auto* module : modules) {
            status.module_error = module->OnFinish(observed_at, collector);
            if (status.module_error != 0) {
                status.error = NpmEofFlushError::kModuleError;
                state_ = NpmEofFlushState::kFailed;
                return status;
            }
        }
        for (auto* module : protocol_modules) {
            status.module_error = module->Finish(observed_at);
            if (status.module_error != 0) {
                status.error = NpmEofFlushError::kModuleError;
                state_ = NpmEofFlushState::kFailed;
                return status;
            }
        }

        status.drain_status = collector.Drain(snapshots, projector, output);
        if (status.drain_status.error != NpmBasicDrainError::kNone) {
            status.error = NpmEofFlushError::kDrainError;
            state_ = NpmEofFlushState::kFailed;
            return status;
        }

        status.drain_status.router_error = collector.Finish();
        if (status.drain_status.router_error) {
            status.drain_status.error = NpmBasicDrainError::kRouterError;
            status.error = NpmEofFlushError::kDrainError;
            state_ = NpmEofFlushState::kFailed;
            output->reset();
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
