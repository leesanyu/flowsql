// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <framework/interfaces/ibaseline_types.h>
#include <cstdint>
#include <vector>

namespace flowsql {
// Per-submit evaluation using the baseline available before learning this observation.
// Every task submission returns this complete result exactly once.
// Calls follow the task serialized-call contract and plugin-library lifetime requirements.
// Readiness is independent of scoring/updating/alert permission and is never persisted.
enum class BaselineEvaluationReadiness : uint32_t {
    kUnavailable = 0,  // Rejected input, failed prediction or failed initialization.
    kColdStart = 1,    // Initialized successfully from this observation; no prior baseline.
    kReady = 2,        // Evaluated successfully against a prior baseline, with result.status == kOk.
};
struct BaselineEvaluationResult {
    RollingBaselineResult result;
    BaselineEvaluationReadiness readiness = BaselineEvaluationReadiness::kUnavailable;
};
struct BaselineRelationEvaluationResult {
    RelationRollingResult result;
    // Same length/order as result.routed_results; both empty when routed output is disabled.
    std::vector<BaselineEvaluationReadiness> routed_readiness;
};
}  // namespace flowsql
