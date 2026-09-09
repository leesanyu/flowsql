// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_IFILTER_DOMAIN_RESOLVER_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_IFILTER_DOMAIN_RESOLVER_H_

#include <common/guid.h>
#include <common/typedef.h>

#include <cstdint>
#include <memory>
#include <string>

namespace arrow {
class Schema;
}

namespace flowsql {

struct FilterExpr;

// {c4d8e17b-92a6-4f31-b5ce-7a2049ed8361}
const Guid IID_FILTER_DOMAIN_RESOLVER_V1 = {
    0xc4d8e17b, 0x92a6, 0x4f31, {0xb5, 0xce, 0x7a, 0x20, 0x49, 0xed, 0x83, 0x61}};

constexpr uint32_t kFilterDomainResolverContractVersionV1 = 1;
constexpr uint32_t kFilterDomainSyntheticNodeIdBaseV1 = 0x80000000u;

enum class FilterDomainTargetKindV1 : uint32_t {
    kSource = 0,
    kTransform = 1,
};

/** Planner-owned input for resolving one stage's domain syntax. */
struct FilterDomainResolveRequestV1 {
    uint32_t contract_version = kFilterDomainResolverContractVersionV1;
    FilterDomainTargetKindV1 target_kind = FilterDomainTargetKindV1::kSource;
    const char* target_category = nullptr;
    const char* target_name = nullptr;
    std::shared_ptr<arrow::Schema> output_schema;
    std::shared_ptr<const FilterExpr> expression;
};

/** Resolver-owned result whose AST storage is transferred to the planner. */
struct FilterDomainResolveResultV1 {
    std::shared_ptr<FilterExpr> lowered_expression;
    std::string diagnostic;
};

/**
 * Stateless domain resolver discovered through IID_FILTER_DOMAIN_RESOLVER_V1.
 *
 * A successful resolver must return a complete expression that is semantically equivalent to the
 * input and contains only syntax understood by the common Schema binder. It must not mutate the
 * input or retain request pointers. The replacement root keeps the original node id; synthesized
 * descendants use ids at or above kFilterDomainSyntheticNodeIdBaseV1 and all output ids are unique.
 */
interface IFilterDomainResolverV1 {
    virtual ~IFilterDomainResolverV1() = default;

    /**
     * @return 0 for the owning target, ENOTSUP when this resolver does not own the target, or another
     *         nonzero error for an invalid domain expression. Nonzero returns leave no expression.
     */
    virtual int Resolve(const FilterDomainResolveRequestV1& request,
                        FilterDomainResolveResultV1* result) const = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_IFILTER_DOMAIN_RESOLVER_H_
