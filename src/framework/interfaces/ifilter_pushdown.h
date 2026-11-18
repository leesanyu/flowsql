// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_IFILTER_PUSHDOWN_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_IFILTER_PUSHDOWN_H_

#include <common/guid.h>
#include <common/typedef.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace arrow {
class Schema;
}

namespace flowsql {

// {0xa3f4182d-7c91-4b65-8a2e-13d7406f95bc}
const Guid IID_FILTER_PUSHDOWN = {0xa3f4182d, 0x7c91, 0x4b65, {0x8a, 0x2e, 0x13, 0xd7, 0x40, 0x6f, 0x95, 0xbc}};

constexpr uint32_t kFilterPushdownContractVersion = 1;

enum class FilterPushdownTargetKind : uint32_t {
    kSource = 0,
    kTransform = 1,
};

/** Planner-owned, immutable input for exact pushdown negotiation. */
struct FilterPushdownRequest {
    uint32_t contract_version = kFilterPushdownContractVersion;
    FilterPushdownTargetKind target_kind = FilterPushdownTargetKind::kSource;
    const char* target_category = nullptr;
    const char* target_name = nullptr;
    std::shared_ptr<arrow::Schema> output_schema;
    const char* canonical_plan_json = nullptr;
    std::vector<uint32_t> candidate_node_ids;
};

/** Provider output. The planner validates this subset and rebuilds residual from the original AST. */
struct FilterPushdownResult {
    std::vector<uint32_t> accepted_node_ids;
    std::string diagnostic;
};

/**
 * Stateless planning capability discovered through IID_FILTER_PUSHDOWN.
 *
 * candidate_node_ids contains only planner-approved exact-pushdown candidates. A successful empty
 * accepted_node_ids means that no predicate is pushed and is not a query error. Implementations must
 * not mutate a shared channel/operator or retain request pointers. Returning an unknown or duplicate
 * node id violates the contract and must be rejected by the planner.
 */
interface IFilterPushdown {
    virtual ~IFilterPushdown() = default;

    /**
     * @return 0 after evaluating the named target, ENOTSUP when this provider does not own the target,
     *         or another nonzero error for a failed negotiation.
     */
    virtual int EvaluatePushdown(const FilterPushdownRequest& request, FilterPushdownResult* result) const = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_IFILTER_PUSHDOWN_H_
