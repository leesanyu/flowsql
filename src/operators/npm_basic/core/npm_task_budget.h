// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_OPERATORS_NPM_BASIC_NPM_TASK_BUDGET_H_
#define _FLOWSQL_OPERATORS_NPM_BASIC_NPM_TASK_BUDGET_H_

#include <operators/npm_basic/npm_analysis_contract.h>

#include <mutex>

namespace flowsql::npm {

/** Task-private synchronized budget ledger. The configured limits are copied at construction. */
class NpmTaskBudget final : public INpmTaskBudget {
 public:
    explicit NpmTaskBudget(const NpmAnalysisConfig& config);

    NpmTaskBudget(const NpmTaskBudget&) = delete;
    NpmTaskBudget& operator=(const NpmTaskBudget&) = delete;
    NpmTaskBudget(NpmTaskBudget&&) = delete;
    NpmTaskBudget& operator=(NpmTaskBudget&&) = delete;

    NpmBudgetError Reserve(NpmBudgetCategory category, uint64_t bytes) override;
    NpmBudgetError Release(NpmBudgetCategory category, uint64_t bytes) override;
    NpmBudgetUsage Usage() const override;

 private:
    NpmAnalysisConfig config_;
    mutable std::mutex mutex_;
    NpmBudgetUsage usage_;
};

}  // namespace flowsql::npm

#endif  // _FLOWSQL_OPERATORS_NPM_BASIC_NPM_TASK_BUDGET_H_
