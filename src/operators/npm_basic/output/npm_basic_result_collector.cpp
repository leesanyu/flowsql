// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_basic_result_collector.h"
#include <arrow/api.h>

#include <cerrno>
#include <new>
#include <utility>

namespace flowsql::npm {

NpmBasicResultCollector::NpmBasicResultCollector() : NpmBasicResultCollector(NpmBasicFeatureConfig{}) {}

NpmBasicResultCollector::NpmBasicResultCollector(NpmBasicFeatureConfig features) : features_(features) {}

int NpmBasicResultCollector::WriteBasic(const NpmBasicResult& result) {
    if (!features_.basic_enabled) return ENOTSUP;
    if (ValidateNpmBasicResult(result) != NpmBasicResultError::kNone) return EINVAL;
    if (features_.observing != NpmResultEntity::kBasic) return 0;
    try {
        pending_basic_.push_back(result);
        return 0;
    } catch (const std::bad_alloc&) {
        return ENOMEM;
    }
}

int NpmBasicResultCollector::WriteSession(const NpmSessionResult& result) {
    if (!features_.session_enabled) return ENOTSUP;
    if (ValidateNpmSessionResult(result) != NpmSessionResultError::kNone) return EINVAL;
    if (features_.observing != NpmResultEntity::kSession) return 0;
    try {
        pending_session_.push_back(result);
        return 0;
    } catch (const std::bad_alloc&) {
        return ENOMEM;
    }
}

NpmBasicDrainStatus NpmBasicResultCollector::Drain(const std::vector<NpmSessionEndEvent>& events,
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
        const bool observing_basic = features_.observing == NpmResultEntity::kBasic;
        std::vector<NpmBasicResult> basic_results;
        if (observing_basic) {
            basic_results = pending_basic_;
            basic_results.reserve(basic_results.size() + events.size());
        }

        if (features_.basic_enabled) {
            for (size_t index = 0; index < events.size(); ++index) {
                NpmBasicResult result;
                const auto& event = events[index];
                status.projection_error = projector.ProjectFinal(event.snapshot.View(), event.snapshot.end_reason,
                                                                 event.observed_at, &result);
                if (status.projection_error != NpmBasicProjectionError::kNone) {
                    status.error = NpmBasicDrainError::kProjectionError;
                    status.event_index = static_cast<int64_t>(index);
                    return status;
                }
                if (observing_basic) basic_results.push_back(std::move(result));
            }
        }

        if (observing_basic) {
            status.encode_error =
                EncodeNpmBasicResultsWithBudget(basic_results, budget, output, nullptr, features_.labeling_enabled);
            if (status.encode_error != NpmBasicEncodeError::kNone) {
                status.error = NpmBasicDrainError::kEncodeError;
                return status;
            }
            pending_basic_.clear();
            return status;
        }

        if (features_.observing == NpmResultEntity::kProtocol) {
            auto empty = arrow::RecordBatch::MakeEmpty(features_.protocol_schema);
            if (!empty.ok()) {
                status.error = NpmBasicDrainError::kEncodeError;
                return status;
            }
            *output = *empty;
            return status;
        }
        status.session_encode_error =
            EncodeNpmSessionResultsWithBudget(pending_session_, budget, output, nullptr, features_.labeling_enabled);
        if (status.session_encode_error != NpmSessionEncodeError::kNone) {
            status.error = NpmBasicDrainError::kEncodeError;
            return status;
        }
        pending_session_.clear();
        return status;
    } catch (const std::bad_alloc&) {
        status.error = NpmBasicDrainError::kAllocationFailed;
        return status;
    }
}

size_t NpmBasicResultCollector::pending_results() const noexcept {
    return features_.observing == NpmResultEntity::kBasic ? pending_basic_.size() : pending_session_.size();
}

}  // namespace flowsql::npm
