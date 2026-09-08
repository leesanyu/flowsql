// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "filter_binding.h"

#include <arrow/type_traits.h>

#include <utility>

namespace flowsql {
namespace {

bool IsStringType(const arrow::DataType& type) {
    return type.id() == arrow::Type::STRING || type.id() == arrow::Type::LARGE_STRING;
}

bool IsBinaryType(const arrow::DataType& type) {
    return type.id() == arrow::Type::BINARY || type.id() == arrow::Type::LARGE_BINARY ||
           type.id() == arrow::Type::FIXED_SIZE_BINARY;
}

bool IsComparableType(const arrow::DataType& type) {
    return arrow::is_integer(type) || arrow::is_floating(type) || IsStringType(type) ||
           IsBinaryType(type) || type.id() == arrow::Type::BOOL;
}

std::shared_ptr<arrow::DataType> CommonComparisonType(
    const std::shared_ptr<arrow::DataType>& lhs,
    const std::shared_ptr<arrow::DataType>& rhs) {
    if (!lhs || !rhs || !IsComparableType(*lhs) || !IsComparableType(*rhs)) return nullptr;
    if (lhs->Equals(rhs)) return lhs;
    const bool lhs_numeric = arrow::is_integer(*lhs) || arrow::is_floating(*lhs);
    const bool rhs_numeric = arrow::is_integer(*rhs) || arrow::is_floating(*rhs);
    if (!lhs_numeric || !rhs_numeric) return nullptr;
    if (arrow::is_floating(*lhs) || arrow::is_floating(*rhs)) {
        const auto& integer_type = arrow::is_integer(*lhs) ? lhs : rhs;
        if (arrow::is_integer(*integer_type) &&
            static_cast<const arrow::IntegerType&>(*integer_type).bit_width() > 32) {
            return nullptr;
        }
        return arrow::float64();
    }

    const bool lhs_signed = arrow::is_signed_integer(*lhs);
    const bool rhs_signed = arrow::is_signed_integer(*rhs);
    if (lhs_signed == rhs_signed) return lhs_signed ? arrow::int64() : arrow::uint64();

    const auto& unsigned_type = lhs_signed ? rhs : lhs;
    if (unsigned_type->id() == arrow::Type::UINT64) return nullptr;
    return arrow::int64();
}

class FilterBinder {
 public:
    FilterBinder(std::shared_ptr<arrow::Schema> schema, std::string* error)
        : schema_(std::move(schema)), error_(error) {}

    FilterBindError Bind(const std::shared_ptr<FilterExpr>& expression,
                         std::shared_ptr<const BoundFilterExpr>* output) {
        auto result = BindNode(expression, nullptr, output);
        if (result != FilterBindError::kNone) return result;
        if (!*output || !(*output)->value_type ||
            (*output)->value_type->id() != arrow::Type::BOOL) {
            output->reset();
            return Fail(FilterBindError::kNonBooleanRoot,
                        "filter root must produce boolean", expression ? expression->node_id : 0);
        }
        return FilterBindError::kNone;
    }

 private:
    FilterBindError Fail(FilterBindError code, const std::string& message, uint32_t node_id) {
        if (error_) {
            *error_ = message;
            if (node_id != 0) *error_ += " at node " + std::to_string(node_id);
        }
        return code;
    }

    FilterBindError BindNode(const std::shared_ptr<FilterExpr>& expression,
                             const std::shared_ptr<arrow::DataType>& expected_type,
                             std::shared_ptr<const BoundFilterExpr>* output) {
        if (!expression || !output) {
            return Fail(FilterBindError::kInvalidAst, "expression node is null", 0);
        }
        output->reset();
        switch (expression->kind) {
            case FilterExprKind::kField:
                return BindField(expression, output);
            case FilterExprKind::kLiteral:
                return BindLiteral(expression, expected_type, output);
            case FilterExprKind::kAnd:
            case FilterExprKind::kOr:
                return BindLogical(expression, 2, output);
            case FilterExprKind::kNot:
                return BindLogical(expression, 1, output);
            case FilterExprKind::kCompare:
                return BindCompare(expression, output);
            case FilterExprKind::kIn:
                return BindIn(expression, output);
            case FilterExprKind::kBetween:
                return BindBetween(expression, output);
            case FilterExprKind::kIsNull:
                return BindIsNull(expression, output);
            case FilterExprKind::kCall:
                return Fail(FilterBindError::kUnknownFunction,
                            "unknown filter function: " + expression->function_name,
                            expression->node_id);
        }
        return Fail(FilterBindError::kInvalidAst, "unknown expression kind", expression->node_id);
    }

    FilterBindError BindField(const std::shared_ptr<FilterExpr>& expression,
                              std::shared_ptr<const BoundFilterExpr>* output) {
        if (!expression->operands.empty() || expression->field_name.empty()) {
            return Fail(FilterBindError::kInvalidAst, "invalid field node", expression->node_id);
        }
        const auto indices = schema_->GetAllFieldIndices(expression->field_name);
        if (indices.empty()) {
            return Fail(FilterBindError::kUnknownField,
                        "unknown field: " + expression->field_name, expression->node_id);
        }
        if (indices.size() != 1) {
            return Fail(FilterBindError::kAmbiguousField,
                        "ambiguous field: " + expression->field_name, expression->node_id);
        }

        const auto& field = schema_->field(indices[0]);
        auto bound = std::make_shared<BoundFilterExpr>();
        bound->kind = expression->kind;
        bound->node_id = expression->node_id;
        bound->value_type = field->type();
        bound->nullable = field->nullable();
        bound->field_index = indices[0];
        *output = std::move(bound);
        return FilterBindError::kNone;
    }

    FilterBindError BindLiteral(const std::shared_ptr<FilterExpr>& expression,
                                const std::shared_ptr<arrow::DataType>& expected_type,
                                std::shared_ptr<const BoundFilterExpr>* output) {
        if (!expression->operands.empty() ||
            expression->literal.kind == FilterLiteralKind::kNone) {
            return Fail(FilterBindError::kInvalidAst, "invalid literal node", expression->node_id);
        }

        std::shared_ptr<arrow::DataType> type = expected_type;
        if (!type) {
            switch (expression->literal.kind) {
                case FilterLiteralKind::kInteger:
                    type = arrow::int64();
                    break;
                case FilterLiteralKind::kFloating:
                    type = arrow::float64();
                    break;
                case FilterLiteralKind::kString:
                    type = arrow::utf8();
                    break;
                case FilterLiteralKind::kBoolean:
                    type = arrow::boolean();
                    break;
                case FilterLiteralKind::kNone:
                    break;
            }
        }
        if (!type || !LiteralMatchesType(expression->literal.kind, *type)) {
            return Fail(FilterBindError::kTypeMismatch,
                        "literal is incompatible with expected Arrow type " +
                            (type ? type->ToString() : std::string("<none>")),
                        expression->node_id);
        }

        std::string scalar_text = expression->literal.text;
        if (expression->literal.kind == FilterLiteralKind::kBoolean) {
            scalar_text = scalar_text == "TRUE" ? "true" : "false";
        }
        auto scalar_result = arrow::Scalar::Parse(type, scalar_text);
        if (!scalar_result.ok()) {
            const FilterBindError code =
                (arrow::is_integer(*type) || arrow::is_floating(*type))
                                             ? FilterBindError::kLiteralOutOfRange
                                             : FilterBindError::kTypeMismatch;
            return Fail(code,
                        "invalid literal for Arrow type " + type->ToString() + ": " +
                            scalar_result.status().ToString(),
                        expression->node_id);
        }

        auto bound = std::make_shared<BoundFilterExpr>();
        bound->kind = expression->kind;
        bound->node_id = expression->node_id;
        bound->value_type = type;
        bound->literal = *scalar_result;
        *output = std::move(bound);
        return FilterBindError::kNone;
    }

    bool LiteralMatchesType(FilterLiteralKind literal_kind,
                            const arrow::DataType& type) const {
        if (arrow::is_integer(type)) return literal_kind == FilterLiteralKind::kInteger;
        if (arrow::is_floating(type)) {
            return literal_kind == FilterLiteralKind::kInteger ||
                   literal_kind == FilterLiteralKind::kFloating;
        }
        if (IsStringType(type)) return literal_kind == FilterLiteralKind::kString;
        if (type.id() == arrow::Type::BOOL) return literal_kind == FilterLiteralKind::kBoolean;
        return false;
    }

    FilterBindError BindLogical(const std::shared_ptr<FilterExpr>& expression,
                                size_t arity,
                                std::shared_ptr<const BoundFilterExpr>* output) {
        if (expression->operands.size() != arity) {
            return Fail(FilterBindError::kInvalidAst,
                        "logical node has invalid arity", expression->node_id);
        }
        auto bound = std::make_shared<BoundFilterExpr>();
        bound->kind = expression->kind;
        bound->node_id = expression->node_id;
        bound->value_type = arrow::boolean();
        for (const auto& operand : expression->operands) {
            std::shared_ptr<const BoundFilterExpr> bound_operand;
            auto result = BindNode(operand, nullptr, &bound_operand);
            if (result != FilterBindError::kNone) return result;
            if (!bound_operand->value_type ||
                bound_operand->value_type->id() != arrow::Type::BOOL) {
                return Fail(FilterBindError::kTypeMismatch,
                            "logical operand must be boolean", operand ? operand->node_id : 0);
            }
            bound->nullable = bound->nullable || bound_operand->nullable;
            bound->operands.push_back(std::move(bound_operand));
        }
        *output = std::move(bound);
        return FilterBindError::kNone;
    }

    FilterBindError BindCompare(const std::shared_ptr<FilterExpr>& expression,
                                std::shared_ptr<const BoundFilterExpr>* output) {
        if (expression->operands.size() != 2 ||
            expression->compare_op == FilterCompareOp::kNone) {
            return Fail(FilterBindError::kInvalidAst,
                        "comparison node is invalid", expression->node_id);
        }
        const auto& lhs_ast = expression->operands[0];
        const auto& rhs_ast = expression->operands[1];
        if (!lhs_ast || !rhs_ast) {
            return Fail(FilterBindError::kInvalidAst,
                        "comparison operand is null", expression->node_id);
        }

        std::shared_ptr<const BoundFilterExpr> lhs;
        std::shared_ptr<const BoundFilterExpr> rhs;
        FilterBindError result;
        if (lhs_ast->kind == FilterExprKind::kLiteral &&
            rhs_ast->kind != FilterExprKind::kLiteral) {
            result = BindNode(rhs_ast, nullptr, &rhs);
            if (result != FilterBindError::kNone) return result;
            result = BindNode(lhs_ast, rhs->value_type, &lhs);
        } else {
            result = BindNode(lhs_ast, nullptr, &lhs);
            if (result != FilterBindError::kNone) return result;
            const auto expected = rhs_ast->kind == FilterExprKind::kLiteral
                                      ? lhs->value_type
                                      : std::shared_ptr<arrow::DataType>();
            result = BindNode(rhs_ast, expected, &rhs);
        }
        if (result != FilterBindError::kNone) return result;

        auto comparison_type = CommonComparisonType(lhs->value_type, rhs->value_type);
        if (!comparison_type) {
            const bool unsupported =
                (lhs->value_type && !IsComparableType(*lhs->value_type)) ||
                (rhs->value_type && !IsComparableType(*rhs->value_type));
            return Fail(unsupported ? FilterBindError::kUnsupportedType
                                    : FilterBindError::kTypeMismatch,
                        "comparison operands are not type-compatible", expression->node_id);
        }
        if (comparison_type->id() == arrow::Type::BOOL &&
            expression->compare_op != FilterCompareOp::kEqual &&
            expression->compare_op != FilterCompareOp::kNotEqual) {
            return Fail(FilterBindError::kTypeMismatch,
                        "boolean values only support = and !=", expression->node_id);
        }

        auto bound = std::make_shared<BoundFilterExpr>();
        bound->kind = expression->kind;
        bound->node_id = expression->node_id;
        bound->value_type = arrow::boolean();
        bound->nullable = lhs->nullable || rhs->nullable;
        bound->compare_op = expression->compare_op;
        bound->comparison_type = std::move(comparison_type);
        bound->operands = {std::move(lhs), std::move(rhs)};
        *output = std::move(bound);
        return FilterBindError::kNone;
    }

    FilterBindError BindIn(const std::shared_ptr<FilterExpr>& expression,
                           std::shared_ptr<const BoundFilterExpr>* output) {
        if (expression->operands.size() < 2 || !expression->operands[0] ||
            expression->operands[0]->kind != FilterExprKind::kField) {
            return Fail(FilterBindError::kInvalidAst, "IN node is invalid", expression->node_id);
        }
        std::shared_ptr<const BoundFilterExpr> field;
        auto result = BindNode(expression->operands[0], nullptr, &field);
        if (result != FilterBindError::kNone) return result;
        if (!field->value_type || !IsComparableType(*field->value_type)) {
            return Fail(FilterBindError::kUnsupportedType,
                        "IN field type is not supported", expression->node_id);
        }

        auto bound = MakePredicateNode(expression, field);
        for (size_t i = 1; i < expression->operands.size(); ++i) {
            if (!expression->operands[i] ||
                expression->operands[i]->kind != FilterExprKind::kLiteral) {
                return Fail(FilterBindError::kInvalidAst,
                            "IN values must be literals", expression->node_id);
            }
            std::shared_ptr<const BoundFilterExpr> literal;
            result = BindNode(expression->operands[i], field->value_type, &literal);
            if (result != FilterBindError::kNone) return result;
            bound->operands.push_back(std::move(literal));
        }
        *output = std::move(bound);
        return FilterBindError::kNone;
    }

    FilterBindError BindBetween(const std::shared_ptr<FilterExpr>& expression,
                                std::shared_ptr<const BoundFilterExpr>* output) {
        if (expression->operands.size() != 3 || !expression->operands[0] ||
            expression->operands[0]->kind != FilterExprKind::kField) {
            return Fail(FilterBindError::kInvalidAst,
                        "BETWEEN node is invalid", expression->node_id);
        }
        std::shared_ptr<const BoundFilterExpr> field;
        auto result = BindNode(expression->operands[0], nullptr, &field);
        if (result != FilterBindError::kNone) return result;
        if (!field->value_type || !IsComparableType(*field->value_type) ||
            field->value_type->id() == arrow::Type::BOOL) {
            return Fail(FilterBindError::kUnsupportedType,
                        "BETWEEN field type is not supported", expression->node_id);
        }

        auto bound = MakePredicateNode(expression, field);
        for (size_t i = 1; i < expression->operands.size(); ++i) {
            if (!expression->operands[i] ||
                expression->operands[i]->kind != FilterExprKind::kLiteral) {
                return Fail(FilterBindError::kInvalidAst,
                            "BETWEEN bounds must be literals", expression->node_id);
            }
            std::shared_ptr<const BoundFilterExpr> literal;
            result = BindNode(expression->operands[i], field->value_type, &literal);
            if (result != FilterBindError::kNone) return result;
            bound->operands.push_back(std::move(literal));
        }
        *output = std::move(bound);
        return FilterBindError::kNone;
    }

    std::shared_ptr<BoundFilterExpr> MakePredicateNode(
        const std::shared_ptr<FilterExpr>& expression,
        const std::shared_ptr<const BoundFilterExpr>& field) const {
        auto bound = std::make_shared<BoundFilterExpr>();
        bound->kind = expression->kind;
        bound->node_id = expression->node_id;
        bound->value_type = arrow::boolean();
        bound->nullable = field->nullable;
        bound->comparison_type = field->value_type;
        bound->operands.push_back(field);
        return bound;
    }

    FilterBindError BindIsNull(const std::shared_ptr<FilterExpr>& expression,
                               std::shared_ptr<const BoundFilterExpr>* output) {
        if (expression->operands.size() != 1 || !expression->operands[0] ||
            expression->operands[0]->kind != FilterExprKind::kField) {
            return Fail(FilterBindError::kInvalidAst,
                        "IS NULL node is invalid", expression->node_id);
        }
        std::shared_ptr<const BoundFilterExpr> field;
        auto result = BindNode(expression->operands[0], nullptr, &field);
        if (result != FilterBindError::kNone) return result;

        auto bound = std::make_shared<BoundFilterExpr>();
        bound->kind = expression->kind;
        bound->node_id = expression->node_id;
        bound->value_type = arrow::boolean();
        bound->operands.push_back(std::move(field));
        *output = std::move(bound);
        return FilterBindError::kNone;
    }

    std::shared_ptr<arrow::Schema> schema_;
    std::string* error_ = nullptr;
};

}  // namespace

FilterBindError BindFilterExpression(
    const std::shared_ptr<arrow::Schema>& schema,
    const std::shared_ptr<FilterExpr>& expression,
    std::shared_ptr<const BoundFilterExpr>* output,
    std::string* error) {
    if (output) output->reset();
    if (error) error->clear();
    if (!schema || !expression || !output) {
        if (error) *error = "schema, expression, and output must not be null";
        return FilterBindError::kInvalidArgument;
    }
    FilterBinder binder(schema, error);
    return binder.Bind(expression, output);
}

}  // namespace flowsql
