// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef FLOWSQL_FRAMEWORK_INTERFACES_IBASELINE_CHECKPOINT_H_
#define FLOWSQL_FRAMEWORK_INTERFACES_IBASELINE_CHECKPOINT_H_

#include <framework/interfaces/ibaseline_state_control.h>

#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>

namespace flowsql {
const Guid IID_BASELINE_CHECKPOINT_SERVICE_V1 = {
    0xc74b6e13, 0x2078, 0x4d16, {0xb2, 0x81, 0xb4, 0x0e, 0x49, 0x3a, 0x63, 0xc1}};
constexpr uint32_t kBaselineCheckpointContractVersionV1 = 1;
constexpr uint32_t kBaselineCheckpointPayloadVersionV1 = 1;
constexpr uint64_t kBaselineCheckpointMaxBytesV1 = 1024ULL * 1024 * 1024;

struct BaselineCheckpointBindingV1 {
    uint32_t struct_size = sizeof(BaselineCheckpointBindingV1);
    uint32_t contract_version = kBaselineCheckpointContractVersionV1;
    uint64_t max_payload_bytes = 16 * 1024 * 1024;
};
inline bool ValidBaselineCheckpointBindingV1(const BaselineCheckpointBindingV1& binding) {
    return binding.struct_size == sizeof(binding) && binding.contract_version == kBaselineCheckpointContractVersionV1 &&
           binding.max_payload_bytes > 0 && binding.max_payload_bytes <= kBaselineCheckpointMaxBytesV1;
}
struct BaselineCheckpointRestoreV1 {
    uint32_t struct_size = sizeof(BaselineCheckpointRestoreV1);
    uint32_t contract_version = kBaselineCheckpointContractVersionV1;
    uint32_t payload_version = kBaselineCheckpointPayloadVersionV1;
    BaselineSerializationFormat format = BaselineSerializationFormat::kJson;
    std::string_view content;  // Borrowed only for RestoreCheckpoint; every restored asset is owned.
};
inline bool ValidBaselineCheckpointRestoreV1(const BaselineCheckpointRestoreV1& restore, uint64_t max_bytes) {
    return restore.struct_size == sizeof(restore) && restore.contract_version == kBaselineCheckpointContractVersionV1 &&
           restore.payload_version == kBaselineCheckpointPayloadVersionV1 &&
           restore.format == BaselineSerializationFormat::kJson && max_bytes > 0 &&
           max_bytes <= kBaselineCheckpointMaxBytesV1 && !restore.content.empty() &&
           restore.content.size() <= max_bytes;
}

// Handle owns its task and capacity-control handle. Keep plugin library alive through all products.
// All calls serialized with task Submit/Bootstrap/Load/Release/Query/Close; no new state locks/threads.
interface IBaselineCheckpointV1 {
    virtual ~IBaselineCheckpointV1() = default;
    // Full state, not QueryTaskSnapshot: effective configuration/calendar/group versions, artifact/
    // seed, rolling/calibration/maturity/consumed cursors, Relation basis/routed/fusion and versions.
    // Owned content contains its own payload/schema/algorithm versions and validated integrity data.
    // Size limit is binding's max_payload_bytes; unsupported format/closed task returns failure.
    virtual BaselineSerializationResult ExportCheckpoint(BaselineSerializationFormat format) const = 0;
    // At most one successful restore into a newly created empty, capacity-bound task, before any
    // Submit/Bootstrap/Load. Failed validation leaves the entire state and eligibility unchanged.
    // Check outer AND embedded versions/lengths, kind/effective config/calendar/group identities,
    // all capacity/fanout limits and complete payload before swapping prepared state atomically.
    // Closed/nonempty/already-restored task rejects. No partial learning/model/registry replacement.
    virtual BaselineStatus RestoreCheckpoint(const BaselineCheckpointRestoreV1& restore) = 0;
};

// Discovered by independent IID, preserving old Baseline service/task/serialization virtual tables.
interface IBaselineCheckpointServiceV1 {
    virtual ~IBaselineCheckpointServiceV1() = default;
    // Only this service's new empty open task, after successful StateControl Bind and before any
    // stateful call. Verify state_control belongs to this same task, positive limits, size/version.
    // Bind exactly once; failure returns no handle and does not change task. Returned handle owns
    // both shared_ptrs. Existing task entrypoints remain capacity-bound and cannot bypass restore rules.
    virtual std::pair<BaselineStatus, std::shared_ptr<IBaselineCheckpointV1>> Bind(
        std::shared_ptr<IBaselineTask> task, std::shared_ptr<IBaselineTaskStateControlV1> state_control,
        const BaselineCheckpointBindingV1& binding) = 0;
};
}  // namespace flowsql
#endif
