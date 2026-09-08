// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "filter_planner.h"

#include <arrow/type.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cerrno>
#include <unordered_set>
#include <utility>

namespace flowsql {
namespace {

bool ProducesBoolean(FilterExprKind kind) {
    switch (kind) {
        case FilterExprKind::kAnd:
        case FilterExprKind::kOr:
        case FilterExprKind::kNot:
        case FilterExprKind::kCompare:
        case FilterExprKind::kCall:
        case FilterExprKind::kIn:
        case FilterExprKind::kBetween:
        case FilterExprKind::kIsNull:
            return true;
        case FilterExprKind::kField:
        case FilterExprKind::kLiteral:
            return false;
    }
    return false;
}

bool HasValidArity(const BoundFilterExpr& expression) {
    switch (expression.kind) {
        case FilterExprKind::kField:
        case FilterExprKind::kLiteral:
            return expression.operands.empty();
        case FilterExprKind::kAnd:
        case FilterExprKind::kOr:
        case FilterExprKind::kCompare:
            return expression.operands.size() == 2;
        case FilterExprKind::kNot:
        case FilterExprKind::kIsNull:
            return expression.operands.size() == 1;
        case FilterExprKind::kCall:
            return true;
        case FilterExprKind::kIn:
            return expression.operands.size() >= 2;
        case FilterExprKind::kBetween:
            return expression.operands.size() == 3;
    }
    return false;
}

const char* FilterKindName(FilterExprKind kind) {
    switch (kind) {
        case FilterExprKind::kField:
            return "field";
        case FilterExprKind::kLiteral:
            return "literal";
        case FilterExprKind::kAnd:
            return "and";
        case FilterExprKind::kOr:
            return "or";
        case FilterExprKind::kNot:
            return "not";
        case FilterExprKind::kCompare:
            return "compare";
        case FilterExprKind::kCall:
            return "call";
        case FilterExprKind::kIn:
            return "in";
        case FilterExprKind::kBetween:
            return "between";
        case FilterExprKind::kIsNull:
            return "is_null";
    }
    return nullptr;
}

const char* CompareOpName(FilterCompareOp compare_op) {
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

class CanonicalFilterPlanWriter {
 public:
    CanonicalFilterPlanWriter(std::shared_ptr<arrow::Schema> schema,
                              std::string* error)
        : schema_(std::move(schema)), writer_(buffer_), error_(error) {}

    bool Write(const std::shared_ptr<const BoundFilterExpr>& expression,
               std::string* output) {
        writer_.StartObject();
        writer_.Key("version");
        writer_.Uint(kFilterPushdownContractVersionV1);
        writer_.Key("root");
        if (!WriteNode(expression)) return false;
        writer_.EndObject();
        if (!writer_.IsComplete()) return Fail("canonical JSON writer did not complete", 0);
        output->assign(buffer_.GetString(), buffer_.GetSize());
        return true;
    }

 private:
    bool Fail(const std::string& message, uint32_t node_id) {
        if (error_) {
            *error_ = message;
            if (node_id != 0) *error_ += " at node " + std::to_string(node_id);
        }
        return false;
    }

    void WriteString(const std::string& value) {
        writer_.String(value.data(), static_cast<rapidjson::SizeType>(value.size()));
    }

    bool WriteNode(const std::shared_ptr<const BoundFilterExpr>& expression) {
        if (!expression || !expression->value_type) {
            return Fail("canonical node is incomplete", expression ? expression->node_id : 0);
        }
        const char* kind_name = FilterKindName(expression->kind);
        if (!kind_name) return Fail("canonical node kind is unknown", expression->node_id);

        writer_.StartObject();
        writer_.Key("node_id");
        writer_.Uint(expression->node_id);
        writer_.Key("kind");
        writer_.String(kind_name);
        writer_.Key("type");
        WriteString(expression->value_type->ToString());
        writer_.Key("nullable");
        writer_.Bool(expression->nullable);

        switch (expression->kind) {
            case FilterExprKind::kField:
                if (!WriteField(*expression)) return false;
                break;
            case FilterExprKind::kLiteral:
                if (!WriteLiteral(*expression)) return false;
                break;
            case FilterExprKind::kCompare:
                if (!WriteComparison(*expression)) return false;
                break;
            case FilterExprKind::kIn:
            case FilterExprKind::kBetween:
                if (!WriteComparisonType(*expression)) return false;
                break;
            case FilterExprKind::kCall:
                return Fail("bound call nodes are not supported by canonical plan version 1",
                            expression->node_id);
            case FilterExprKind::kAnd:
            case FilterExprKind::kOr:
            case FilterExprKind::kNot:
            case FilterExprKind::kIsNull:
                break;
        }

        if (!expression->operands.empty()) {
            writer_.Key("operands");
            writer_.StartArray();
            for (const auto& operand : expression->operands) {
                if (!WriteNode(operand)) return false;
            }
            writer_.EndArray();
        }
        writer_.EndObject();
        return true;
    }

    bool WriteField(const BoundFilterExpr& expression) {
        if (expression.field_index < 0 || expression.field_index >= schema_->num_fields()) {
            return Fail("bound field index is outside the output Schema", expression.node_id);
        }
        const auto& field = schema_->field(expression.field_index);
        if (!field || !field->type() ||
            !field->type()->Equals(*expression.value_type) ||
            field->nullable() != expression.nullable) {
            return Fail("bound field does not match the output Schema", expression.node_id);
        }
        writer_.Key("field_index");
        writer_.Int(expression.field_index);
        writer_.Key("field_name");
        WriteString(field->name());
        return true;
    }

    bool WriteLiteral(const BoundFilterExpr& expression) {
        if (!expression.literal || !expression.literal->type ||
            !expression.literal->type->Equals(*expression.value_type) ||
            !expression.literal->is_valid) {
            return Fail("bound literal is incomplete or has the wrong type", expression.node_id);
        }
        writer_.Key("value");
        WriteString(expression.literal->ToString());
        return true;
    }

    bool WriteComparisonType(const BoundFilterExpr& expression) {
        if (!expression.comparison_type) {
            return Fail("bound comparison type is missing", expression.node_id);
        }
        writer_.Key("comparison_type");
        WriteString(expression.comparison_type->ToString());
        return true;
    }

    bool WriteComparison(const BoundFilterExpr& expression) {
        const char* compare_op = CompareOpName(expression.compare_op);
        if (!compare_op) return Fail("bound compare operator is missing", expression.node_id);
        writer_.Key("compare_op");
        writer_.String(compare_op);
        return WriteComparisonType(expression);
    }

    std::shared_ptr<arrow::Schema> schema_;
    rapidjson::StringBuffer buffer_;
    rapidjson::Writer<rapidjson::StringBuffer> writer_;
    std::string* error_ = nullptr;
};

class FilterSplitter {
 public:
    explicit FilterSplitter(std::string* error) : error_(error) {}

    FilterPlanError Split(
        const std::shared_ptr<const BoundFilterExpr>& expression,
        const std::vector<uint32_t>& requested_accepted,
        FilterPushdownSplit* output) {
        if (!expression->value_type ||
            expression->value_type->id() != arrow::Type::BOOL) {
            return Fail(FilterPlanError::kInvalidBoundExpression,
                        "filter root must produce boolean", expression->node_id);
        }
        if (!ValidateTree(expression)) return FilterPlanError::kInvalidBoundExpression;

        std::vector<std::shared_ptr<const BoundFilterExpr>> candidates;
        CollectCandidates(expression, &candidates);
        std::unordered_set<uint32_t> candidate_ids;
        candidate_ids.reserve(candidates.size());
        for (const auto& candidate : candidates) {
            candidate_ids.insert(candidate->node_id);
        }

        std::unordered_set<uint32_t> accepted_set;
        accepted_set.reserve(requested_accepted.size());
        for (uint32_t node_id : requested_accepted) {
            if (!accepted_set.insert(node_id).second) {
                return Fail(FilterPlanError::kDuplicateAcceptedNode,
                            "accepted node id is duplicated", node_id);
            }
        }
        for (uint32_t node_id : requested_accepted) {
            if (all_node_ids_.find(node_id) == all_node_ids_.end()) {
                return Fail(FilterPlanError::kUnknownAcceptedNode,
                            "accepted node id is unknown", node_id);
            }
            if (candidate_ids.find(node_id) == candidate_ids.end()) {
                return Fail(FilterPlanError::kNonCandidateAcceptedNode,
                            "accepted node id is not a pushdown candidate", node_id);
            }
        }

        FilterPushdownSplit result;
        result.candidate_node_ids.reserve(candidates.size());
        result.accepted_node_ids.reserve(accepted_set.size());
        for (const auto& candidate : candidates) {
            result.candidate_node_ids.push_back(candidate->node_id);
            if (accepted_set.find(candidate->node_id) != accepted_set.end()) {
                result.accepted_node_ids.push_back(candidate->node_id);
            }
        }
        result.pushed_expression = Project(expression, accepted_set, true);
        result.residual_expression = Project(expression, accepted_set, false);
        *output = std::move(result);
        return FilterPlanError::kNone;
    }

 private:
    FilterPlanError Fail(FilterPlanError code,
                         const std::string& message,
                         uint32_t node_id) {
        if (error_) {
            *error_ = message;
            if (node_id != 0) *error_ += " at node " + std::to_string(node_id);
        }
        return code;
    }

    bool ValidateTree(const std::shared_ptr<const BoundFilterExpr>& expression) {
        if (!expression || expression->node_id == 0 || !expression->value_type) {
            Fail(FilterPlanError::kInvalidBoundExpression,
                 "bound expression node is incomplete",
                 expression ? expression->node_id : 0);
            return false;
        }
        if (!all_node_ids_.insert(expression->node_id).second) {
            Fail(FilterPlanError::kInvalidBoundExpression,
                 "bound expression node id is duplicated", expression->node_id);
            return false;
        }
        if (!HasValidArity(*expression)) {
            Fail(FilterPlanError::kInvalidBoundExpression,
                 "bound expression node has invalid arity", expression->node_id);
            return false;
        }
        if (ProducesBoolean(expression->kind) &&
            expression->value_type->id() != arrow::Type::BOOL) {
            Fail(FilterPlanError::kInvalidBoundExpression,
                 "predicate node must produce boolean", expression->node_id);
            return false;
        }
        for (const auto& operand : expression->operands) {
            if (!ValidateTree(operand)) return false;
        }
        return true;
    }

    void CollectCandidates(
        const std::shared_ptr<const BoundFilterExpr>& expression,
        std::vector<std::shared_ptr<const BoundFilterExpr>>* candidates) const {
        if (expression->kind != FilterExprKind::kAnd) {
            candidates->push_back(expression);
            return;
        }
        CollectCandidates(expression->operands[0], candidates);
        CollectCandidates(expression->operands[1], candidates);
    }

    std::shared_ptr<const BoundFilterExpr> Project(
        const std::shared_ptr<const BoundFilterExpr>& expression,
        const std::unordered_set<uint32_t>& accepted,
        bool select_accepted) const {
        if (expression->kind != FilterExprKind::kAnd) {
            const bool is_accepted = accepted.find(expression->node_id) != accepted.end();
            return is_accepted == select_accepted ? expression : nullptr;
        }

        auto left = Project(expression->operands[0], accepted, select_accepted);
        auto right = Project(expression->operands[1], accepted, select_accepted);
        if (!left) return right;
        if (!right) return left;
        if (left == expression->operands[0] && right == expression->operands[1]) {
            return expression;
        }

        auto projected = std::make_shared<BoundFilterExpr>(*expression);
        projected->operands = {std::move(left), std::move(right)};
        return projected;
    }

    std::unordered_set<uint32_t> all_node_ids_;
    std::string* error_ = nullptr;
};

}  // namespace

FilterPlanError SplitFilterForPushdown(
    const std::shared_ptr<const BoundFilterExpr>& expression,
    const std::vector<uint32_t>& accepted_node_ids,
    FilterPushdownSplit* output,
    std::string* error) {
    if (output) *output = FilterPushdownSplit{};
    if (error) error->clear();
    if (!expression || !output) {
        if (error) *error = "expression and output must not be null";
        return FilterPlanError::kInvalidArgument;
    }

    FilterSplitter splitter(error);
    return splitter.Split(expression, accepted_node_ids, output);
}

FilterPlanError BuildCanonicalFilterPlan(
    const std::shared_ptr<arrow::Schema>& output_schema,
    const std::shared_ptr<const BoundFilterExpr>& expression,
    std::string* output,
    std::string* error) {
    if (output) output->clear();
    if (error) error->clear();
    if (!output_schema || !expression || !output) {
        if (error) *error = "output_schema, expression, and output must not be null";
        return FilterPlanError::kInvalidArgument;
    }

    FilterPushdownSplit validated;
    const auto validation = SplitFilterForPushdown(expression, {}, &validated, error);
    if (validation != FilterPlanError::kNone) return validation;

    CanonicalFilterPlanWriter writer(output_schema, error);
    if (!writer.Write(expression, output)) {
        output->clear();
        return FilterPlanError::kCanonicalPlanError;
    }
    return FilterPlanError::kNone;
}

FilterPlanError NegotiateFilterPushdown(
    IQuerier* querier,
    const FilterPushdownTarget& target,
    const std::shared_ptr<const BoundFilterExpr>& expression,
    FilterPushdownNegotiation* output,
    std::string* error) {
    if (output) *output = FilterPushdownNegotiation{};
    if (error) error->clear();
    if (!querier || !target.output_schema || target.category.empty() ||
        target.name.empty() || !expression || !output) {
        if (error) {
            *error = "querier, target identity, output Schema, expression, and output are required";
        }
        return FilterPlanError::kInvalidArgument;
    }

    FilterPushdownNegotiation result;
    auto plan_error = SplitFilterForPushdown(expression, {}, &result.split, error);
    if (plan_error != FilterPlanError::kNone) return plan_error;
    plan_error = BuildCanonicalFilterPlan(
        target.output_schema, expression, &result.canonical_plan_json, error);
    if (plan_error != FilterPlanError::kNone) return plan_error;

    FilterPushdownRequestV1 request;
    request.target_kind = target.kind;
    request.target_category = target.category.c_str();
    request.target_name = target.name.c_str();
    request.output_schema = target.output_schema;
    request.canonical_plan_json = result.canonical_plan_json.c_str();
    request.candidate_node_ids = result.split.candidate_node_ids;

    bool stopped = false;
    int provider_return_code = ENOTSUP;
    FilterPushdownResultV1 provider_result;
    const int traverse_return_code = querier->Traverse(
        IID_FILTER_PUSHDOWN_V1,
        [&](void* iface) {
            if (!iface) {
                stopped = true;
                provider_return_code = EINVAL;
                provider_result.diagnostic = "IID traversal returned a null provider";
                return -1;
            }
            FilterPushdownResultV1 current;
            const int current_return_code =
                static_cast<IFilterPushdownV1*>(iface)->EvaluatePushdown(request, &current);
            if (current_return_code == ENOTSUP) return 0;
            stopped = true;
            provider_return_code = current_return_code;
            provider_result = std::move(current);
            return -1;
        });
    if (traverse_return_code != 0) {
        if (error) {
            *error = "IFilterPushdownV1 traversal failed with code " +
                     std::to_string(traverse_return_code);
        }
        return FilterPlanError::kQuerierError;
    }
    if (!stopped) {
        *output = std::move(result);
        return FilterPlanError::kNone;
    }
    if (provider_return_code != 0) {
        if (error) {
            *error = "IFilterPushdownV1 provider failed with code " +
                     std::to_string(provider_return_code);
            if (!provider_result.diagnostic.empty()) {
                *error += ": " + provider_result.diagnostic;
            }
        }
        return FilterPlanError::kProviderNegotiationFailed;
    }

    FilterPushdownSplit accepted_split;
    plan_error = SplitFilterForPushdown(
        expression, provider_result.accepted_node_ids, &accepted_split, error);
    if (plan_error != FilterPlanError::kNone) return plan_error;
    result.provider_matched = true;
    result.provider_diagnostic = std::move(provider_result.diagnostic);
    result.split = std::move(accepted_split);
    *output = std::move(result);
    return FilterPlanError::kNone;
}

FilterPlanError MaterializeFilterTaskSessionPlan(
    const std::shared_ptr<arrow::Schema>& output_schema,
    const std::shared_ptr<const BoundFilterExpr>& original_expression,
    const FilterPushdownNegotiation& negotiation,
    FilterTaskIsolation isolation,
    FilterTaskSessionPlan* output,
    std::string* error) {
    if (output) *output = FilterTaskSessionPlan{};
    if (error) error->clear();
    if (!output_schema || !original_expression || !output) {
        if (error) *error = "output_schema, original_expression, and output must not be null";
        return FilterPlanError::kInvalidArgument;
    }

    std::string expected_canonical_plan;
    auto plan_error = BuildCanonicalFilterPlan(
        output_schema, original_expression, &expected_canonical_plan, error);
    if (plan_error != FilterPlanError::kNone) return plan_error;
    if (negotiation.canonical_plan_json != expected_canonical_plan) {
        if (error) *error = "pushdown negotiation does not match the current Schema/expression";
        return FilterPlanError::kInvalidNegotiation;
    }
    if (!negotiation.provider_matched && !negotiation.split.accepted_node_ids.empty()) {
        if (error) *error = "an unmatched pushdown negotiation cannot accept predicates";
        return FilterPlanError::kInvalidNegotiation;
    }

    FilterPushdownSplit validated_split;
    plan_error = SplitFilterForPushdown(
        original_expression,
        negotiation.split.accepted_node_ids,
        &validated_split,
        error);
    if (plan_error != FilterPlanError::kNone) return plan_error;
    if (validated_split.candidate_node_ids != negotiation.split.candidate_node_ids ||
        validated_split.accepted_node_ids != negotiation.split.accepted_node_ids) {
        if (error) *error = "pushdown negotiation candidate/accepted ids are inconsistent";
        return FilterPlanError::kInvalidNegotiation;
    }

    FilterTaskSessionPlan result;
    if (isolation == FilterTaskIsolation::kSharedSource) {
        plan_error = SplitFilterForPushdown(original_expression, {}, &validated_split, error);
        if (plan_error != FilterPlanError::kNone) return plan_error;
    } else if (isolation != FilterTaskIsolation::kExclusive) {
        if (error) *error = "unknown filter task isolation mode";
        return FilterPlanError::kInvalidArgument;
    }

    result.accepted_node_ids = validated_split.accepted_node_ids;
    result.pushed_expression = validated_split.pushed_expression;
    result.residual_expression = validated_split.residual_expression;
    result.pushdown_enabled = result.pushed_expression != nullptr;
    if (result.pushdown_enabled) {
        plan_error = BuildCanonicalFilterPlan(
            output_schema,
            result.pushed_expression,
            &result.pushed_filter_plan_json,
            error);
        if (plan_error != FilterPlanError::kNone) return plan_error;
    } else {
        result.pushed_filter_plan_json = kEmptyCanonicalFilterPlanV1;
    }

    *output = std::move(result);
    return FilterPlanError::kNone;
}

FilterPlanError CreateBlockTransformTaskSession(
    IBlockTransformOperatorV1* provider,
    const std::string& task_id,
    const std::string& with_params_json,
    const FilterTaskSessionPlan& filter_plan,
    IBlockTransformTaskV1** task,
    std::string* error) {
    if (task) *task = nullptr;
    if (error) error->clear();
    if (!provider || task_id.empty() || with_params_json.empty() ||
        filter_plan.pushed_filter_plan_json.empty() || !task) {
        if (error) {
            *error = "provider, task id, WITH JSON, filter plan, and task output are required";
        }
        return FilterPlanError::kInvalidArgument;
    }

    const std::string owned_task_id = task_id;
    const std::string owned_with_params_json = with_params_json;
    const std::string owned_filter_plan_json = filter_plan.pushed_filter_plan_json;
    BlockTransformTaskConfigV1 config;
    config.task_id = owned_task_id.c_str();
    config.with_params_json = owned_with_params_json.c_str();
    config.pushed_filter_plan_json = owned_filter_plan_json.c_str();

    IBlockTransformTaskV1* created_task = nullptr;
    const int create_return_code = provider->CreateTask(config, &created_task);
    if (create_return_code != 0 || !created_task) {
        if (created_task) provider->ReleaseTask(created_task);
        if (error) {
            *error = "block transform task creation failed";
            if (create_return_code != 0) {
                *error += " with code " + std::to_string(create_return_code);
            } else {
                *error += ": provider returned a null task";
            }
        }
        return FilterPlanError::kTaskSessionCreationFailed;
    }

    *task = created_task;
    return FilterPlanError::kNone;
}

}  // namespace flowsql
