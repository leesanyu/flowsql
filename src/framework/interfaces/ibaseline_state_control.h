// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_IBASELINE_STATE_CONTROL_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_IBASELINE_STATE_CONTROL_H_

#include <framework/interfaces/ibaseline_service.h>

#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>

namespace flowsql {

// {c36d6711-4ae0-46ca-b284-75b36a4db5cc}
const Guid IID_BASELINE_STATE_CONTROL_SERVICE_V1 = {
    0xc36d6711, 0x4ae0, 0x46ca, {0xb2, 0x84, 0x75, 0xb3, 0x6a, 0x4d, 0xb5, 0xcc}};

struct BaselineStateLimitsV1 {
    uint64_t max_runtime_identities = 0;
    uint64_t max_model_identities = 0;
    // Per source and metric. Relation requires at least two versions; Value/Ratio ignore the positive value.
    uint32_t max_basis_versions_per_metric = 0;
};

enum class BaselineStateReleaseScopeV1 : int32_t {
    kRuntimeOnly = 0,
    kAllState = 1,
};

struct BaselineStateUsageV1 {
    uint64_t runtime_identities = 0;
    uint64_t model_identities = 0;
    uint64_t routed_states = 0;
    uint64_t retained_basis_versions = 0;
};

// All calls share the associated task's non-overlapping call contract, including queries and Close.
// The handle owns that task. Its plugin library must outlive the handle and all returned plugin products.
interface IBaselineTaskStateControlV1 {
    virtual ~IBaselineTaskStateControlV1() = default;

    // Value/Ratio: series key; Relation: source key. Missing identities succeed idempotently.
    // Both scopes reset online cursors. RuntimeOnly retains parent models for a new online lifecycle.
    // Cross-lifecycle late-input rejection is the caller's responsibility. Closed tasks reject all calls.
    virtual BaselineStatus ReleaseIdentity(std::string_view key, BaselineStateReleaseScopeV1 scope) = 0;
    virtual std::pair<BaselineStatus, BaselineStateUsageV1> QueryUsage() const = 0;
};

// Optional independent capability: the existing service/task ABI and serialized formats are unchanged.
interface IBaselineStateControlServiceV1 {
    virtual ~IBaselineStateControlServiceV1() = default;

    // Only an open, unbound task owned by this service, before any Submit/Bootstrap/Load call, is accepted.
    // Limits must be positive and are immutable after binding. All old task entrypoints enforce them.
    // Capacity rejects new identities with kInvalidArgument; no other identity is implicitly evicted.
    virtual std::pair<BaselineStatus, std::shared_ptr<IBaselineTaskStateControlV1>> Bind(
        std::shared_ptr<IBaselineTask> task, const BaselineStateLimitsV1& limits) = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_IBASELINE_STATE_CONTROL_H_
