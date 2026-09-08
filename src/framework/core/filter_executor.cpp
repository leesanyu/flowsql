// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "filter_executor.h"

#include <arrow/array/util.h>
#include <arrow/compute/api.h>

#include <utility>

namespace flowsql {
namespace {

const std::shared_ptr<arrow::DataType> DatumType(const arrow::Datum& datum) {
    if (datum.is_array()) return datum.array()->type;
    if (datum.is_scalar()) return datum.scalar()->type;
    return nullptr;
}

class FilterEvaluator {
 public:
    FilterEvaluator(std::shared_ptr<arrow::RecordBatch> batch, std::string* error)
        : batch_(std::move(batch)), error_(error) {}

    FilterEvalError Evaluate(const std::shared_ptr<const BoundFilterExpr>& expression,
                             std::shared_ptr<arrow::BooleanArray>* output) {
        if (!expression->value_type || expression->value_type->id() != arrow::Type::BOOL) {
            return Fail(FilterEvalError::kInvalidBoundExpression,
                        "filter root must produce boolean", expression->node_id);
        }

        arrow::Datum result;
        auto eval_error = EvaluateNode(expression, &result);
        if (eval_error != FilterEvalError::kNone) return eval_error;
        const auto result_type = DatumType(result);
        if (!result_type || result_type->id() != arrow::Type::BOOL) {
            return Fail(FilterEvalError::kInvalidBoundExpression,
                        "filter result is not boolean", expression->node_id);
        }

        std::shared_ptr<arrow::Array> result_array;
        if (result.is_array()) {
            result_array = result.make_array();
        } else if (result.is_scalar()) {
            auto broadcast = arrow::MakeArrayFromScalar(*result.scalar(), batch_->num_rows());
            if (!broadcast.ok()) {
                return Fail(FilterEvalError::kArrowError,
                            "cannot broadcast filter scalar: " +
                                broadcast.status().ToString(),
                            expression->node_id);
            }
            result_array = *broadcast;
        } else {
            return Fail(FilterEvalError::kArrowError,
                        "Arrow filter kernel returned an unsupported datum",
                        expression->node_id);
        }

        if (!result_array || result_array->length() != batch_->num_rows() ||
            result_array->type_id() != arrow::Type::BOOL) {
            return Fail(FilterEvalError::kArrowError,
                        "Arrow filter kernel returned an invalid mask",
                        expression->node_id);
        }
        *output = std::static_pointer_cast<arrow::BooleanArray>(result_array);
        return FilterEvalError::kNone;
    }

 private:
    FilterEvalError Fail(FilterEvalError code,
                         const std::string& message,
                         uint32_t node_id) {
        if (error_) {
            *error_ = message;
            if (node_id != 0) *error_ += " at node " + std::to_string(node_id);
        }
        return code;
    }

    FilterEvalError EvaluateNode(
        const std::shared_ptr<const BoundFilterExpr>& expression,
        arrow::Datum* output) {
        if (!expression || !output || !expression->value_type) {
            return Fail(FilterEvalError::kInvalidBoundExpression,
                        "bound expression node is incomplete",
                        expression ? expression->node_id : 0);
        }
        switch (expression->kind) {
            case FilterExprKind::kField:
                return EvaluateField(expression, output);
            case FilterExprKind::kLiteral:
                return EvaluateLiteral(expression, output);
            case FilterExprKind::kAnd:
                return EvaluateLogical(expression, "and_kleene", 2, output);
            case FilterExprKind::kOr:
                return EvaluateLogical(expression, "or_kleene", 2, output);
            case FilterExprKind::kNot:
                return EvaluateLogical(expression, "invert", 1, output);
            case FilterExprKind::kCompare:
                return EvaluateCompare(expression, output);
            case FilterExprKind::kIn:
                return EvaluateIn(expression, output);
            case FilterExprKind::kBetween:
                return EvaluateBetween(expression, output);
            case FilterExprKind::kIsNull:
                return EvaluateIsNull(expression, output);
            case FilterExprKind::kCall:
                return Fail(FilterEvalError::kUnsupportedFunction,
                            "bound function evaluation is not available",
                            expression->node_id);
        }
        return Fail(FilterEvalError::kInvalidBoundExpression,
                    "unknown bound expression kind", expression->node_id);
    }

    FilterEvalError EvaluateField(
        const std::shared_ptr<const BoundFilterExpr>& expression,
        arrow::Datum* output) {
        if (!expression->operands.empty() || expression->literal ||
            expression->field_index < 0) {
            return Fail(FilterEvalError::kInvalidBoundExpression,
                        "bound field node is invalid", expression->node_id);
        }
        if (expression->field_index >= batch_->num_columns()) {
            return Fail(FilterEvalError::kSchemaMismatch,
                        "bound field index is outside the batch Schema",
                        expression->node_id);
        }
        const auto& column = batch_->column(expression->field_index);
        if (!column || !column->type()->Equals(expression->value_type)) {
            return Fail(FilterEvalError::kSchemaMismatch,
                        "batch column type does not match the bound field",
                        expression->node_id);
        }
        *output = arrow::Datum(column);
        return FilterEvalError::kNone;
    }

    FilterEvalError EvaluateLiteral(
        const std::shared_ptr<const BoundFilterExpr>& expression,
        arrow::Datum* output) {
        if (!expression->operands.empty() || !expression->literal ||
            !expression->literal->is_valid ||
            !expression->literal->type->Equals(expression->value_type)) {
            return Fail(FilterEvalError::kInvalidBoundExpression,
                        "bound literal node is invalid", expression->node_id);
        }
        *output = arrow::Datum(expression->literal);
        return FilterEvalError::kNone;
    }

    FilterEvalError EvaluateLogical(
        const std::shared_ptr<const BoundFilterExpr>& expression,
        const char* function,
        size_t arity,
        arrow::Datum* output) {
        if (expression->value_type->id() != arrow::Type::BOOL ||
            expression->operands.size() != arity) {
            return Fail(FilterEvalError::kInvalidBoundExpression,
                        "bound logical node has invalid type or arity",
                        expression->node_id);
        }
        std::vector<arrow::Datum> operands;
        operands.reserve(arity);
        for (const auto& operand : expression->operands) {
            arrow::Datum value;
            auto eval_error = EvaluateNode(operand, &value);
            if (eval_error != FilterEvalError::kNone) return eval_error;
            const auto type = DatumType(value);
            if (!type || type->id() != arrow::Type::BOOL) {
                return Fail(FilterEvalError::kInvalidBoundExpression,
                            "logical operand is not boolean",
                            operand ? operand->node_id : expression->node_id);
            }
            operands.push_back(std::move(value));
        }
        return CallBoolean(function, operands, expression->node_id, output);
    }

    FilterEvalError EvaluateCompare(
        const std::shared_ptr<const BoundFilterExpr>& expression,
        arrow::Datum* output) {
        if (expression->operands.size() != 2 || !expression->comparison_type) {
            return Fail(FilterEvalError::kInvalidBoundExpression,
                        "bound comparison node is invalid", expression->node_id);
        }
        const char* function = ComparisonFunction(expression->compare_op);
        if (!function) {
            return Fail(FilterEvalError::kInvalidBoundExpression,
                        "bound comparison operator is invalid", expression->node_id);
        }

        std::vector<arrow::Datum> operands(2);
        for (size_t i = 0; i < operands.size(); ++i) {
            arrow::Datum value;
            auto eval_error = EvaluateNode(expression->operands[i], &value);
            if (eval_error != FilterEvalError::kNone) return eval_error;
            eval_error = CastForComparison(
                value, expression->comparison_type, expression->node_id, &operands[i]);
            if (eval_error != FilterEvalError::kNone) return eval_error;
        }
        return CallBoolean(function, operands, expression->node_id, output);
    }

    FilterEvalError EvaluateIn(
        const std::shared_ptr<const BoundFilterExpr>& expression,
        arrow::Datum* output) {
        if (expression->operands.size() < 2 || !expression->comparison_type ||
            !expression->operands[0] ||
            expression->operands[0]->kind != FilterExprKind::kField) {
            return Fail(FilterEvalError::kInvalidBoundExpression,
                        "bound IN node is invalid", expression->node_id);
        }

        arrow::Datum field;
        auto eval_error = EvaluateNode(expression->operands[0], &field);
        if (eval_error != FilterEvalError::kNone) return eval_error;
        arrow::Datum comparable_field;
        eval_error = CastForComparison(
            field, expression->comparison_type, expression->node_id, &comparable_field);
        if (eval_error != FilterEvalError::kNone) return eval_error;

        arrow::Datum accumulated;
        for (size_t i = 1; i < expression->operands.size(); ++i) {
            if (!expression->operands[i] ||
                expression->operands[i]->kind != FilterExprKind::kLiteral) {
                return Fail(FilterEvalError::kInvalidBoundExpression,
                            "bound IN value is not a literal", expression->node_id);
            }
            arrow::Datum literal;
            eval_error = EvaluateNode(expression->operands[i], &literal);
            if (eval_error != FilterEvalError::kNone) return eval_error;
            arrow::Datum comparable_literal;
            eval_error = CastForComparison(
                literal, expression->comparison_type, expression->node_id,
                &comparable_literal);
            if (eval_error != FilterEvalError::kNone) return eval_error;

            arrow::Datum equals;
            eval_error = CallBoolean(
                "equal", {comparable_field, comparable_literal},
                expression->node_id, &equals);
            if (eval_error != FilterEvalError::kNone) return eval_error;
            if (i == 1) {
                accumulated = std::move(equals);
            } else {
                arrow::Datum combined;
                eval_error = CallBoolean(
                    "or_kleene", {accumulated, equals}, expression->node_id, &combined);
                if (eval_error != FilterEvalError::kNone) return eval_error;
                accumulated = std::move(combined);
            }
        }
        *output = std::move(accumulated);
        return FilterEvalError::kNone;
    }

    FilterEvalError EvaluateBetween(
        const std::shared_ptr<const BoundFilterExpr>& expression,
        arrow::Datum* output) {
        if (expression->operands.size() != 3 || !expression->comparison_type ||
            !expression->operands[0] ||
            expression->operands[0]->kind != FilterExprKind::kField ||
            !expression->operands[1] ||
            expression->operands[1]->kind != FilterExprKind::kLiteral ||
            !expression->operands[2] ||
            expression->operands[2]->kind != FilterExprKind::kLiteral) {
            return Fail(FilterEvalError::kInvalidBoundExpression,
                        "bound BETWEEN node is invalid", expression->node_id);
        }

        std::vector<arrow::Datum> operands(3);
        for (size_t i = 0; i < operands.size(); ++i) {
            arrow::Datum value;
            auto eval_error = EvaluateNode(expression->operands[i], &value);
            if (eval_error != FilterEvalError::kNone) return eval_error;
            eval_error = CastForComparison(
                value, expression->comparison_type, expression->node_id, &operands[i]);
            if (eval_error != FilterEvalError::kNone) return eval_error;
        }

        arrow::Datum lower_match;
        auto eval_error = CallBoolean(
            "greater_equal", {operands[0], operands[1]}, expression->node_id,
            &lower_match);
        if (eval_error != FilterEvalError::kNone) return eval_error;
        arrow::Datum upper_match;
        eval_error = CallBoolean(
            "less_equal", {operands[0], operands[2]}, expression->node_id,
            &upper_match);
        if (eval_error != FilterEvalError::kNone) return eval_error;
        return CallBoolean(
            "and_kleene", {lower_match, upper_match}, expression->node_id, output);
    }

    FilterEvalError EvaluateIsNull(
        const std::shared_ptr<const BoundFilterExpr>& expression,
        arrow::Datum* output) {
        if (expression->operands.size() != 1 || !expression->operands[0] ||
            expression->operands[0]->kind != FilterExprKind::kField) {
            return Fail(FilterEvalError::kInvalidBoundExpression,
                        "bound IS NULL node is invalid", expression->node_id);
        }
        arrow::Datum operand;
        auto eval_error = EvaluateNode(expression->operands[0], &operand);
        if (eval_error != FilterEvalError::kNone) return eval_error;
        return CallBoolean("is_null", {operand}, expression->node_id, output);
    }

    FilterEvalError CastForComparison(
        const arrow::Datum& input,
        const std::shared_ptr<arrow::DataType>& target_type,
        uint32_t node_id,
        arrow::Datum* output) {
        const auto input_type = DatumType(input);
        if (!input_type || !target_type || !output) {
            return Fail(FilterEvalError::kInvalidBoundExpression,
                        "comparison value has no Arrow type", node_id);
        }
        if (input_type->Equals(target_type)) {
            *output = input;
            return FilterEvalError::kNone;
        }
        auto cast = arrow::compute::Cast(
            input, arrow::TypeHolder(target_type), arrow::compute::CastOptions::Safe());
        if (!cast.ok()) {
            return Fail(FilterEvalError::kArrowError,
                        "safe comparison cast failed: " + cast.status().ToString(),
                        node_id);
        }
        *output = *cast;
        return FilterEvalError::kNone;
    }

    FilterEvalError CallBoolean(
        const char* function,
        const std::vector<arrow::Datum>& operands,
        uint32_t node_id,
        arrow::Datum* output) {
        auto result = arrow::compute::CallFunction(function, operands);
        if (!result.ok()) {
            return Fail(FilterEvalError::kArrowError,
                        std::string("Arrow compute '") + function +
                            "' failed: " + result.status().ToString(),
                        node_id);
        }
        const auto type = DatumType(*result);
        if (!type || type->id() != arrow::Type::BOOL ||
            (!result->is_array() && !result->is_scalar())) {
            return Fail(FilterEvalError::kArrowError,
                        std::string("Arrow compute '") + function +
                            "' did not return boolean values",
                        node_id);
        }
        *output = *result;
        return FilterEvalError::kNone;
    }

    const char* ComparisonFunction(FilterCompareOp compare_op) const {
        switch (compare_op) {
            case FilterCompareOp::kEqual:
                return "equal";
            case FilterCompareOp::kNotEqual:
                return "not_equal";
            case FilterCompareOp::kLess:
                return "less";
            case FilterCompareOp::kLessEqual:
                return "less_equal";
            case FilterCompareOp::kGreater:
                return "greater";
            case FilterCompareOp::kGreaterEqual:
                return "greater_equal";
            case FilterCompareOp::kNone:
                return nullptr;
        }
        return nullptr;
    }

    std::shared_ptr<arrow::RecordBatch> batch_;
    std::string* error_ = nullptr;
};

}  // namespace

FilterEvalError EvaluateFilterMask(
    const std::shared_ptr<arrow::RecordBatch>& batch,
    const std::shared_ptr<const BoundFilterExpr>& expression,
    std::shared_ptr<arrow::BooleanArray>* output,
    std::string* error) {
    if (output) output->reset();
    if (error) error->clear();
    if (!batch || !expression || !output) {
        if (error) *error = "batch, expression, and output must not be null";
        return FilterEvalError::kInvalidArgument;
    }

    static const arrow::Status initialize_status = arrow::compute::Initialize();
    if (!initialize_status.ok()) {
        if (error) {
            *error = "Arrow compute initialization failed: " +
                     initialize_status.ToString();
        }
        return FilterEvalError::kArrowError;
    }

    FilterEvaluator evaluator(batch, error);
    return evaluator.Evaluate(expression, output);
}

FilterEvalError FilterRecordBatch(
    const std::shared_ptr<arrow::RecordBatch>& batch,
    const std::shared_ptr<const BoundFilterExpr>& expression,
    std::shared_ptr<arrow::RecordBatch>* output,
    std::string* error) {
    if (output) output->reset();
    if (error) error->clear();
    if (!output) {
        if (error) *error = "output must not be null";
        return FilterEvalError::kInvalidArgument;
    }

    std::shared_ptr<arrow::BooleanArray> mask;
    const auto eval_error = EvaluateFilterMask(batch, expression, &mask, error);
    if (eval_error != FilterEvalError::kNone) return eval_error;

    const arrow::compute::FilterOptions options(
        arrow::compute::FilterOptions::DROP);
    auto filtered = arrow::compute::Filter(
        arrow::Datum(batch), arrow::Datum(mask), options);
    if (!filtered.ok()) {
        if (error) *error = "Arrow RecordBatch filter failed: " + filtered.status().ToString();
        return FilterEvalError::kArrowError;
    }
    if (filtered->kind() != arrow::Datum::RECORD_BATCH ||
        !filtered->record_batch()) {
        if (error) *error = "Arrow filter did not return a RecordBatch";
        return FilterEvalError::kArrowError;
    }

    const auto& result = filtered->record_batch();
    if (!result->schema()->Equals(*batch->schema(), true)) {
        if (error) *error = "Arrow filter changed the RecordBatch Schema or metadata";
        return FilterEvalError::kArrowError;
    }
    *output = result;
    return FilterEvalError::kNone;
}

}  // namespace flowsql
