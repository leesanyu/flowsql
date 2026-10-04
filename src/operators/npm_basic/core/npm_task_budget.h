// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_OPERATORS_NPM_BASIC_NPM_TASK_BUDGET_H_
#define _FLOWSQL_OPERATORS_NPM_BASIC_NPM_TASK_BUDGET_H_

#include <operators/npm_basic/npm_analysis_contract.h>

#include <mutex>

namespace flowsql::npm {

struct NpmBudgetHighWaterMarks {
    uint64_t tracked_bytes = 0;
    uint64_t input_batch_bytes = 0;
    uint64_t pending_output_bytes = 0;
};

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
    // Simultaneous tracked/pending totals, and the largest transient input reservation.
    NpmBudgetHighWaterMarks HighWaterMarks() const;

 private:
    NpmAnalysisConfig config_;
    mutable std::mutex mutex_;
    NpmBudgetUsage usage_;
    NpmBudgetHighWaterMarks high_water_;
};

}  // namespace flowsql::npm

#endif  // _FLOWSQL_OPERATORS_NPM_BASIC_NPM_TASK_BUDGET_H_
