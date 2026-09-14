// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_basic_result_collector.h"

#include <cerrno>
#include <new>
#include <utility>

namespace flowsql::npm {

int NpmBasicResultCollector::WriteBasic(const NpmBasicResult& result) {
    if (ValidateNpmBasicResult(result) != NpmBasicResultError::kNone) return EINVAL;
    try {
        pending_.push_back(result);
        return 0;
    } catch (const std::bad_alloc&) {
        return ENOMEM;
    }
}

NpmBasicDrainStatus NpmBasicResultCollector::Drain(
    const std::vector<NpmSessionEndEvent>& events,
    NpmBasicResultProjector& projector,
    const std::shared_ptr<INpmTaskBudget>& budget,
    std::shared_ptr<arrow::RecordBatch>* output) {
    NpmBasicDrainStatus status;
    if (output == nullptr) {
        status.error = NpmBasicDrainError::kNullOutput;
        return status;
    }
    if (budget == nullptr) {
        status.error = NpmBasicDrainError::kNullBudget;
        return status;
    }

    try {
        std::vector<NpmBasicResult> results = pending_;
        results.reserve(results.size() + events.size());
        for (size_t index = 0; index < events.size(); ++index) {
            NpmBasicResult result;
            const auto& event = events[index];
            status.projection_error = projector.ProjectFinal(
                event.snapshot.View(), event.snapshot.end_reason, event.observed_at, &result);
            if (status.projection_error != NpmBasicProjectionError::kNone) {
                status.error = NpmBasicDrainError::kProjectionError;
                status.event_index = static_cast<int64_t>(index);
                return status;
            }
            results.push_back(std::move(result));
        }

        status.encode_error = EncodeNpmBasicResultsWithBudget(results, budget, output);
        if (status.encode_error != NpmBasicEncodeError::kNone) {
            status.error = NpmBasicDrainError::kEncodeError;
            return status;
        }
        pending_.clear();
        return status;
    } catch (const std::bad_alloc&) {
        status.error = NpmBasicDrainError::kAllocationFailed;
        return status;
    }
}

}  // namespace flowsql::npm
