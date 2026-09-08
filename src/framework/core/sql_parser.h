// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_FRAMEWORK_CORE_SQL_PARSER_H_
#define _FLOWSQL_FRAMEWORK_CORE_SQL_PARSER_H_

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
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
    // Exact numeric token, decoded string, or canonical TRUE/FALSE.
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

struct StageFilter {
    uint32_t after_stage = 0;
    std::shared_ptr<FilterExpr> expression;
};

struct OperatorRef {
    std::string category;
    std::string name;
};

// SQL 解析结果
struct SqlStatement {
    std::string source;       // FROM 后的源通道名
    std::vector<std::string> sources;  // FROM 后的全部源通道名（source=sources[0]）
    std::string op_category;   // USING 后的算子 category（可选，空表示无算子）
    std::string op_name;      // USING 后的算子 name（可选）
    std::vector<OperatorRef> operators;  // USING/THEN 算子链
    std::vector<std::unordered_map<std::string, std::string>> operator_with_params;  // 与 operators 对齐
    std::unordered_map<std::string, std::string> with_params;  // WITH key=val,...
    std::string dest;         // INTO 后的目标通道名（可选，空表示直接返回结果）
    std::vector<std::string> columns;  // SELECT 后的列名（空表示 *）
    std::string where_clause; // WHERE 后的过滤条件（可选，空表示无过滤）
    std::vector<StageFilter> stage_filters;  // source stage 0，operator stage 1..N
    std::string sql_part;     // 完整 SQL 部分（不含 USING/WITH/INTO），数据库通道直接使用
    std::string error;        // 解析错误信息（空表示成功）

    // 是否有算子
    bool HasOperator() const {
        if (!operators.empty()) return true;
        return !op_category.empty() && !op_name.empty();
    }
};

// 递归下降 SQL 解析器
// 语法：SELECT [* | col1, col2, ...] FROM <source> [WHERE <condition>]
//       [USING <category.name> [WITH key=val,...] (THEN <category.name> [WITH ...])*] [INTO <dest>]
class SqlParser {
 public:
    SqlStatement Parse(const std::string& sql);

    static bool ParseFilterExpression(const std::string& expression,
                                      std::shared_ptr<FilterExpr>* output,
                                      std::string* error);

    // 验证 WHERE 子句安全性（拒绝 SQL 注入关键字）
    static bool ValidateWhereClause(const std::string& clause);

 private:
    // 词法辅助
    void SkipWhitespace();
    bool MatchKeyword(const char* keyword);
    std::string ReadIdentifier();
    std::string ReadChannelRef(std::string* err);
    std::string ReadValue();

    const char* pos_ = nullptr;
    const char* end_ = nullptr;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_CORE_SQL_PARSER_H_
