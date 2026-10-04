// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_task_budget.h"

#include <algorithm>

namespace flowsql::npm {

NpmTaskBudget::NpmTaskBudget(const NpmAnalysisConfig& config) : config_(config) {}

NpmBudgetError NpmTaskBudget::Reserve(NpmBudgetCategory category, uint64_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto result = ReserveNpmBudget(config_, category, bytes, &usage_);
    if (result == NpmBudgetError::kNone) {
        high_water_.tracked_bytes = std::max(high_water_.tracked_bytes, NpmTrackedBudgetBytes(usage_));
        high_water_.input_batch_bytes = std::max(high_water_.input_batch_bytes, usage_.input_batch_bytes);
        high_water_.pending_output_bytes = std::max(high_water_.pending_output_bytes, usage_.pending_output_bytes);
    }
    return result;
}

NpmBudgetError NpmTaskBudget::Release(NpmBudgetCategory category, uint64_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    return ReleaseNpmBudget(category, bytes, &usage_);
}

NpmBudgetUsage NpmTaskBudget::Usage() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return usage_;
}

NpmBudgetHighWaterMarks NpmTaskBudget::HighWaterMarks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return high_water_;
}

}  // namespace flowsql::npm
