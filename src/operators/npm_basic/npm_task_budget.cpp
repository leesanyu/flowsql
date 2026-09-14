// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_task_budget.h"

namespace flowsql::npm {

NpmTaskBudget::NpmTaskBudget(const NpmAnalysisConfig& config) : config_(config) {}

NpmBudgetError NpmTaskBudget::Reserve(NpmBudgetCategory category, uint64_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    return ReserveNpmBudget(config_, category, bytes, &usage_);
}

NpmBudgetError NpmTaskBudget::Release(NpmBudgetCategory category, uint64_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    return ReleaseNpmBudget(category, bytes, &usage_);
}

NpmBudgetUsage NpmTaskBudget::Usage() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return usage_;
}

}  // namespace flowsql::npm
