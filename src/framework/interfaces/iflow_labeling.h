// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_IFLOW_LABELING_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_IFLOW_LABELING_H_

#include <common/guid.h>
#include <common/typedef.h>
#include <framework/interfaces/iconfig_channel_registry.h>

#include <cstdint>

namespace flowsql {

// {e2451331-a639-4c4b-91b9-0e91427f0273}
const Guid IID_FLOW_LABELING_PROVIDER_V1 = {
    0xe2451331, 0xa639, 0x4c4b, {0x91, 0xb9, 0x0e, 0x91, 0x42, 0x7f, 0x02, 0x73}};

enum class FlowLabelingErrorV1 : uint8_t {
    kNone = 0,
    kUnavailable,
    kInvalidSnapshot,
    kInvalidConfig,
    kLimitExceeded,
    kUnsupportedAlgorithm,
    kBudgetExceeded,
    kDpdkFailure,
    kAllocationFailed,
};

/** Typed facts for one endpoint. Numeric values use host byte order; address bytes use network order. */
struct FlowLabelEndpointFactsV1 {
    uint8_t mac[6]{};
    uint8_t mac_valid = 0;
    uint8_t ip[16]{};
    uint8_t ip_valid = 0;
    uint16_t port = 0;
    uint8_t port_valid = 0;
};

/** One decoded VLAN tag. */
struct FlowLabelVlanFactsV1 {
    uint16_t tpid = 0;
    uint16_t vid = 0;
    uint8_t valid = 0;
};

/** Borrowed classification input for one unique new-session admission candidate. */
struct FlowLabelFactsV1 {
    uint32_t struct_size = sizeof(FlowLabelFactsV1);
    uint64_t observation_domain_id = 0;
    uint8_t ip_family = 0;
    uint8_t transport_protocol = 0;
    uint8_t transport_valid = 0;
    FlowLabelEndpointFactsV1 source;
    FlowLabelEndpointFactsV1 destination;
    FlowLabelVlanFactsV1 vlan[2];
};

constexpr uint32_t kFlowLabelFactsV1Size = static_cast<uint32_t>(sizeof(FlowLabelFactsV1));

/** Borrowed matcher creation input. The provider must not retain snapshot after CreateMatcher returns. */
struct FlowLabelingCompileRequestV1 {
    uint32_t struct_size = sizeof(FlowLabelingCompileRequestV1);
    const ConfigChannelSnapshot* snapshot = nullptr;
    uint64_t reserved_module_state_bytes = 0;
    uint32_t max_labels = 0;
    uint32_t max_logical_rules = 0;
    uint32_t max_compiled_rules = 0;
};

constexpr uint32_t kFlowLabelingCompileRequestV1Size = static_cast<uint32_t>(sizeof(FlowLabelingCompileRequestV1));

/** Matcher-owned label metadata. String pointers remain valid until matcher Release. */
struct FlowPrimaryLabelViewV1 {
    uint32_t label_id = 0;
    int32_t priority = 0;
    const char* name = nullptr;
    const char* display_name = nullptr;
    const char* description = nullptr;
};

/** Call-borrowed diagnostic. The caller copies strings before the provider method returns again. */
struct FlowLabelingDiagnosticV1 {
    FlowLabelingErrorV1 error = FlowLabelingErrorV1::kNone;
    const char* path = nullptr;
    const char* detail = nullptr;
};

/** Task-private immutable matcher lease. Release must precede provider Stop or plugin unload. */
interface IFlowLabelMatcherV1 {
    virtual ~IFlowLabelMatcherV1() = default;

    /** Returns zero for whole-batch success; on failure primary_label_ids is undefined. */
    virtual int ClassifyBatch(const FlowLabelFactsV1* facts, uint32_t count, uint32_t* primary_label_ids) const = 0;
    virtual bool FindLabel(uint32_t label_id, FlowPrimaryLabelViewV1 * output) const = 0;
    virtual void Release() noexcept = 0;
};

/** Process-local capability discovered through IID_FLOW_LABELING_PROVIDER_V1. */
interface IFlowLabelingProviderV1 {
    virtual ~IFlowLabelingProviderV1() = default;

    virtual FlowLabelingErrorV1 RuntimeStatus(FlowLabelingDiagnosticV1 * diagnostic) const = 0;

    /** A failure must leave output unchanged. */
    virtual FlowLabelingErrorV1 CreateMatcher(const FlowLabelingCompileRequestV1& request, IFlowLabelMatcherV1** output,
                                              FlowLabelingDiagnosticV1* diagnostic) = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_IFLOW_LABELING_H_
