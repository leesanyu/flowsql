// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_FRAMEWORK_CORE_FILTER_BINDING_H_
#define _FLOWSQL_FRAMEWORK_CORE_FILTER_BINDING_H_

#include <arrow/api.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "filter_expression.h"

namespace flowsql {

enum class FilterBindError : int32_t {
    kNone = 0,
    kInvalidArgument,
    kInvalidAst,
    kUnknownField,
    kAmbiguousField,
    kUnsupportedType,
    kTypeMismatch,
    kLiteralOutOfRange,
    kUnknownFunction,
    kNonBooleanRoot,
};

/**
 * Immutable-after-bind expression consumed by the Arrow filter evaluator.
 *
 * `value_type` is the value produced by this node. `comparison_type` is set on compare, IN, and
 * BETWEEN nodes and defines the common Arrow type used by the evaluator. Field nodes use
 * `field_index`; literal nodes own an already validated Arrow Scalar.
 */
struct BoundFilterExpr {
    FilterExprKind kind = FilterExprKind::kLiteral;
    uint32_t node_id = 0;
    std::shared_ptr<arrow::DataType> value_type;
    bool nullable = false;
    int32_t field_index = -1;
    std::shared_ptr<arrow::Scalar> literal;
    FilterCompareOp compare_op = FilterCompareOp::kNone;
    std::shared_ptr<arrow::DataType> comparison_type;
    std::vector<std::shared_ptr<const BoundFilterExpr>> operands;
};

/** Bind a parsed expression to exactly one stage output Schema. */
FilterBindError BindFilterExpression(
    const std::shared_ptr<arrow::Schema>& schema,
    const std::shared_ptr<FilterExpr>& expression,
    std::shared_ptr<const BoundFilterExpr>* output,
    std::string* error);

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_CORE_FILTER_BINDING_H_
