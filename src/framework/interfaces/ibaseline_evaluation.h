// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <framework/interfaces/ibaseline_types.h>
#include <cstdint>
#include <vector>

namespace flowsql {
// Per-submit evaluation using the baseline available before learning this observation.
// Independent optional task capabilities; existing task vtables and result layouts stay unchanged.
// Calls inherit the old task's serialized-call contract and plugin-library lifetime requirements.
enum class BaselineEvaluationReadinessV1 : uint32_t {
    kUnavailable = 0,  // Rejected input, failed prediction or failed initialization.
    kColdStart = 1,    // Initialized successfully from this observation; no prior baseline.
    kReady = 2,        // Evaluated successfully against a prior baseline, with result.status == kOk.
};
struct BaselineEvaluationResultV1 {
    RollingBaselineResult result;
    BaselineEvaluationReadinessV1 readiness = BaselineEvaluationReadinessV1::kUnavailable;
};
struct BaselineRelationEvaluationResultV1 {
    RelationRollingResult result;
    // Same length/order as result.routed_results; both empty when routed output is disabled.
    std::vector<BaselineEvaluationReadinessV1> routed_readiness;
};
// Discover via dynamic_cast from the service-created task. Each call submits exactly once.
// Readiness is independent of scoring/updating/alert permission and is never persisted.
interface IBaselineValueEvaluationV1 {
    virtual ~IBaselineValueEvaluationV1() = default;
    virtual BaselineEvaluationResultV1 SubmitObservationWithReadiness(const ValueRollingObservation& obs,
                                                                      const RollingSubmitOptions& options) = 0;
};
interface IBaselineRatioEvaluationV1 {
    virtual ~IBaselineRatioEvaluationV1() = default;
    virtual BaselineEvaluationResultV1 SubmitObservationWithReadiness(const RatioRollingObservation& obs,
                                                                      const RollingSubmitOptions& options) = 0;
};
interface IBaselineRelationEvaluationV1 {
    virtual ~IBaselineRelationEvaluationV1() = default;
    virtual BaselineRelationEvaluationResultV1 SubmitObservationWithReadiness(
        const RelationRollingObservation& obs, const RelationRollingSubmitOptions& options) = 0;
};
}  // namespace flowsql
