// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_FRAMEWORK_CORE_FILTER_PLANNER_H_
#define _FLOWSQL_FRAMEWORK_CORE_FILTER_PLANNER_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <common/iquerier.hpp>
#include <framework/interfaces/iblock_transform_operator.h>
#include <framework/interfaces/ifilter_pushdown.h>

#include "filter_binding.h"

namespace flowsql {

enum class FilterPlanError : int32_t {
    kNone = 0,
    kInvalidArgument,
    kInvalidBoundExpression,
    kUnknownAcceptedNode,
    kNonCandidateAcceptedNode,
    kDuplicateAcceptedNode,
    kCanonicalPlanError,
    kProviderNegotiationFailed,
    kQuerierError,
    kInvalidNegotiation,
    kTaskSessionCreationFailed,
};

constexpr const char kEmptyCanonicalFilterPlanV1[] =
    "{\"version\":1,\"root\":null}";

struct FilterPushdownSplit {
    std::vector<uint32_t> candidate_node_ids;
    std::vector<uint32_t> accepted_node_ids;
    std::shared_ptr<const BoundFilterExpr> pushed_expression;
    std::shared_ptr<const BoundFilterExpr> residual_expression;
};

struct FilterPushdownTarget {
    FilterPushdownTargetKindV1 kind = FilterPushdownTargetKindV1::kSource;
    std::string category;
    std::string name;
    std::shared_ptr<arrow::Schema> output_schema;
};

struct FilterPushdownNegotiation {
    bool provider_matched = false;
    std::string canonical_plan_json;
    std::string provider_diagnostic;
    FilterPushdownSplit split;
};

enum class FilterTaskIsolation : uint32_t {
    kExclusive = 0,
    kSharedSource = 1,
};

/** Immutable-after-build filter state owned by exactly one runtime task. */
struct FilterTaskSessionPlan {
    bool pushdown_enabled = false;
    std::string pushed_filter_plan_json;
    std::vector<uint32_t> accepted_node_ids;
    std::shared_ptr<const BoundFilterExpr> pushed_expression;
    std::shared_ptr<const BoundFilterExpr> residual_expression;
};

/** Split top-level conjuncts without changing the original bound expression. */
FilterPlanError SplitFilterForPushdown(
    const std::shared_ptr<const BoundFilterExpr>& expression,
    const std::vector<uint32_t>& accepted_node_ids,
    FilterPushdownSplit* output,
    std::string* error);

/** Serialize a bound expression and its Schema as a deterministic version-1 JSON plan. */
FilterPlanError BuildCanonicalFilterPlan(
    const std::shared_ptr<arrow::Schema>& output_schema,
    const std::shared_ptr<const BoundFilterExpr>& expression,
    std::string* output,
    std::string* error);

/** Discover the target owner by IID and negotiate its exact pushdown subset. */
FilterPlanError NegotiateFilterPushdown(
    IQuerier* querier,
    const FilterPushdownTarget& target,
    const std::shared_ptr<const BoundFilterExpr>& expression,
    FilterPushdownNegotiation* output,
    std::string* error);

/** Copy an exact negotiated subset into task-owned state without touching a channel/operator. */
FilterPlanError MaterializeFilterTaskSessionPlan(
    const std::shared_ptr<arrow::Schema>& output_schema,
    const std::shared_ptr<const BoundFilterExpr>& original_expression,
    const FilterPushdownNegotiation& negotiation,
    FilterTaskIsolation isolation,
    FilterTaskSessionPlan* output,
    std::string* error);

/** Create one exclusive transform session; the provider must copy all raw-pointer config fields. */
FilterPlanError CreateBlockTransformTaskSession(
    IBlockTransformOperatorV1* provider,
    const std::string& task_id,
    const std::string& with_params_json,
    const FilterTaskSessionPlan& filter_plan,
    IBlockTransformTaskV1** task,
    std::string* error);

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_CORE_FILTER_PLANNER_H_
