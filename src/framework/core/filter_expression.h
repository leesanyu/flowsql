// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_FRAMEWORK_CORE_FILTER_EXPRESSION_H_
#define _FLOWSQL_FRAMEWORK_CORE_FILTER_EXPRESSION_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace flowsql {

enum class FilterExprKind {
    kField,
    kLiteral,
    kAnd,
    kOr,
    kNot,
    kCompare,
    kCall,
    kIn,
    kBetween,
    kIsNull,
};

enum class FilterLiteralKind {
    kNone,
    kInteger,
    kFloating,
    kString,
    kBoolean,
    kTyped,
};

enum class FilterCompareOp {
    kNone,
    kEqual,
    kNotEqual,
    kLess,
    kLessEqual,
    kGreater,
    kGreaterEqual,
};

struct FilterLiteral {
    FilterLiteralKind kind = FilterLiteralKind::kNone;
    // Lower-case type identifier for kTyped; empty for all other literal kinds.
    std::string type_name;
    // Exact numeric token, decoded string/typed text, or canonical TRUE/FALSE.
    std::string text;
};

/**
 * Typed, schema-independent filter syntax tree.
 *
 * Invariants by kind:
 * - kField uses field_name and has no operands.
 * - kLiteral uses literal and has no operands.
 * - kAnd/kOr/kCompare have two operands; kNot/kIsNull have one.
 * - kCall uses function_name and its operands are arguments.
 * - kIn has a field followed by one or more literals.
 * - kBetween has a field followed by lower and upper literals.
 * node_id is assigned in deterministic pre-order, starting from one.
 */
struct FilterExpr {
    FilterExprKind kind = FilterExprKind::kLiteral;
    uint32_t node_id = 0;
    std::string field_name;
    std::string function_name;
    FilterLiteral literal;
    FilterCompareOp compare_op = FilterCompareOp::kNone;
    std::vector<std::shared_ptr<FilterExpr>> operands;
};

/** Parse domain-independent filter syntax into a schema-independent AST. */
bool ParseFilterExpression(const std::string& expression,
                           std::shared_ptr<FilterExpr>* output,
                           std::string* error);

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_CORE_FILTER_EXPRESSION_H_
