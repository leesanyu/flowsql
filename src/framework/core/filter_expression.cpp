// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "filter_expression.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace flowsql {
namespace {

enum class FilterTokenKind {
    kInvalid,
    kEnd,
    kIdentifier,
    kInteger,
    kFloating,
    kString,
    kTrue,
    kFalse,
    kAnd,
    kOr,
    kNot,
    kIn,
    kBetween,
    kIs,
    kNull,
    kLeftParen,
    kRightParen,
    kComma,
    kEqual,
    kNotEqual,
    kLess,
    kLessEqual,
    kGreater,
    kGreaterEqual,
};

struct FilterToken {
    FilterTokenKind kind = FilterTokenKind::kInvalid;
    std::string text;
    size_t offset = 0;
};

std::string UpperAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return value;
}

std::string LowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

class FilterLexer {
 public:
    explicit FilterLexer(const std::string& input) : input_(input) {}

    FilterToken Next(std::string* error) {
        if (error) error->clear();
        while (pos_ < input_.size() && std::isspace(static_cast<unsigned char>(input_[pos_]))) ++pos_;
        if (pos_ == input_.size()) return {FilterTokenKind::kEnd, "", pos_};

        const size_t start = pos_;
        const unsigned char c = static_cast<unsigned char>(input_[pos_]);
        if (std::isalpha(c) || c == '_') return ReadIdentifier();
        if (std::isdigit(c) || c == '+' || c == '-' || c == '.') return ReadNumber(error);
        if (c == '\'') return ReadString(error);

        ++pos_;
        switch (c) {
            case '(':
                return {FilterTokenKind::kLeftParen, "(", start};
            case ')':
                return {FilterTokenKind::kRightParen, ")", start};
            case ',':
                return {FilterTokenKind::kComma, ",", start};
            case '=':
                if (pos_ < input_.size() && input_[pos_] == '=') {
                    ++pos_;
                    return Invalid("operator == is not supported", start, error);
                }
                return {FilterTokenKind::kEqual, "=", start};
            case '!':
                if (pos_ < input_.size() && input_[pos_] == '=') {
                    ++pos_;
                    return {FilterTokenKind::kNotEqual, "!=", start};
                }
                return Invalid("expected = after !", start, error);
            case '<':
                if (pos_ < input_.size() && input_[pos_] == '=') {
                    ++pos_;
                    return {FilterTokenKind::kLessEqual, "<=", start};
                }
                return {FilterTokenKind::kLess, "<", start};
            case '>':
                if (pos_ < input_.size() && input_[pos_] == '=') {
                    ++pos_;
                    return {FilterTokenKind::kGreaterEqual, ">=", start};
                }
                return {FilterTokenKind::kGreater, ">", start};
            case '"':
                return Invalid("double-quoted strings are not supported", start, error);
            default:
                return Invalid(std::string("unexpected character '") + static_cast<char>(c) + "'", start, error);
        }
    }

 private:
    FilterToken Invalid(const std::string& message, size_t offset, std::string* error) const {
        if (error) *error = message + " at offset " + std::to_string(offset);
        return {FilterTokenKind::kInvalid, "", offset};
    }

    FilterToken ReadIdentifier() {
        const size_t start = pos_++;
        while (pos_ < input_.size()) {
            const unsigned char c = static_cast<unsigned char>(input_[pos_]);
            if (!std::isalnum(c) && c != '_') break;
            ++pos_;
        }
        std::string text = input_.substr(start, pos_ - start);
        const std::string keyword = UpperAscii(text);
        if (keyword == "TRUE") return {FilterTokenKind::kTrue, "TRUE", start};
        if (keyword == "FALSE") return {FilterTokenKind::kFalse, "FALSE", start};
        if (keyword == "AND") return {FilterTokenKind::kAnd, keyword, start};
        if (keyword == "OR") return {FilterTokenKind::kOr, keyword, start};
        if (keyword == "NOT") return {FilterTokenKind::kNot, keyword, start};
        if (keyword == "IN") return {FilterTokenKind::kIn, keyword, start};
        if (keyword == "BETWEEN") return {FilterTokenKind::kBetween, keyword, start};
        if (keyword == "IS") return {FilterTokenKind::kIs, keyword, start};
        if (keyword == "NULL") return {FilterTokenKind::kNull, keyword, start};
        return {FilterTokenKind::kIdentifier, std::move(text), start};
    }

    FilterToken ReadNumber(std::string* error) {
        const size_t start = pos_;
        if (input_[pos_] == '+' || input_[pos_] == '-') ++pos_;

        size_t whole_digits = 0;
        while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
            ++whole_digits;
            ++pos_;
        }

        bool floating = false;
        size_t fraction_digits = 0;
        if (pos_ < input_.size() && input_[pos_] == '.') {
            floating = true;
            ++pos_;
            while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
                ++fraction_digits;
                ++pos_;
            }
        }
        if (whole_digits == 0 && fraction_digits == 0) {
            return Invalid("invalid numeric literal", start, error);
        }

        if (pos_ < input_.size() && (input_[pos_] == 'e' || input_[pos_] == 'E')) {
            floating = true;
            ++pos_;
            if (pos_ < input_.size() && (input_[pos_] == '+' || input_[pos_] == '-')) ++pos_;
            const size_t exponent_start = pos_;
            while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) ++pos_;
            if (pos_ == exponent_start) return Invalid("invalid numeric exponent", start, error);
        }

        return {floating ? FilterTokenKind::kFloating : FilterTokenKind::kInteger,
                input_.substr(start, pos_ - start), start};
    }

    FilterToken ReadString(std::string* error) {
        const size_t start = pos_++;
        std::string decoded;
        while (pos_ < input_.size()) {
            const char c = input_[pos_++];
            if (c != '\'') {
                decoded.push_back(c);
                continue;
            }
            if (pos_ < input_.size() && input_[pos_] == '\'') {
                decoded.push_back('\'');
                ++pos_;
                continue;
            }
            return {FilterTokenKind::kString, std::move(decoded), start};
        }
        return Invalid("unterminated string literal", start, error);
    }

    const std::string& input_;
    size_t pos_ = 0;
};

class FilterExpressionParser {
 public:
    explicit FilterExpressionParser(const std::string& input) : lexer_(input) {}

    bool Parse(std::shared_ptr<FilterExpr>* output, std::string* error) {
        if (!output) {
            if (error) *error = "output must not be null";
            return false;
        }
        output->reset();
        if (error) error->clear();
        if (!Advance()) return FinishError(error);

        auto expression = ParseOr();
        if (!expression) return FinishError(error);
        if (current_.kind != FilterTokenKind::kEnd) {
            SetError("unexpected token '" + current_.text + "'", current_.offset);
            return FinishError(error);
        }

        uint32_t next_node_id = 1;
        AssignNodeIds(expression, &next_node_id);
        *output = std::move(expression);
        return true;
    }

 private:
    bool FinishError(std::string* error) {
        if (error) *error = error_.empty() ? "invalid filter expression" : error_;
        return false;
    }

    bool Advance() {
        std::string lexer_error;
        current_ = lexer_.Next(&lexer_error);
        if (current_.kind == FilterTokenKind::kInvalid) {
            error_ = std::move(lexer_error);
            return false;
        }
        return true;
    }

    void SetError(const std::string& message, size_t offset) {
        if (error_.empty()) error_ = message + " at offset " + std::to_string(offset);
    }

    bool Consume(FilterTokenKind kind, const char* expected) {
        if (current_.kind != kind) {
            SetError(std::string("expected ") + expected, current_.offset);
            return false;
        }
        return Advance();
    }

    std::shared_ptr<FilterExpr> MakeNode(
        FilterExprKind kind, std::vector<std::shared_ptr<FilterExpr>> operands = {}) {
        auto node = std::make_shared<FilterExpr>();
        node->kind = kind;
        node->operands = std::move(operands);
        return node;
    }

    std::shared_ptr<FilterExpr> MakeField(std::string name) {
        auto node = MakeNode(FilterExprKind::kField);
        node->field_name = std::move(name);
        return node;
    }

    std::shared_ptr<FilterExpr> MakeLiteral(
        FilterLiteralKind kind, std::string text, std::string type_name = "") {
        auto node = MakeNode(FilterExprKind::kLiteral);
        node->literal.kind = kind;
        node->literal.type_name = std::move(type_name);
        node->literal.text = std::move(text);
        return node;
    }

    std::shared_ptr<FilterExpr> MakeNot(std::shared_ptr<FilterExpr> operand) {
        return MakeNode(FilterExprKind::kNot, {std::move(operand)});
    }

    std::shared_ptr<FilterExpr> ParseOr() {
        auto left = ParseAnd();
        while (left && current_.kind == FilterTokenKind::kOr) {
            if (!Advance()) return nullptr;
            auto right = ParseAnd();
            if (!right) return nullptr;
            left = MakeNode(FilterExprKind::kOr, {std::move(left), std::move(right)});
        }
        return left;
    }

    std::shared_ptr<FilterExpr> ParseAnd() {
        auto left = ParseNot();
        while (left && current_.kind == FilterTokenKind::kAnd) {
            if (!Advance()) return nullptr;
            auto right = ParseNot();
            if (!right) return nullptr;
            left = MakeNode(FilterExprKind::kAnd, {std::move(left), std::move(right)});
        }
        return left;
    }

    std::shared_ptr<FilterExpr> ParseNot() {
        if (current_.kind == FilterTokenKind::kNot) {
            if (!Advance()) return nullptr;
            auto operand = ParseNot();
            return operand ? MakeNot(std::move(operand)) : nullptr;
        }
        if (current_.kind == FilterTokenKind::kLeftParen) {
            if (!Advance()) return nullptr;
            auto expression = ParseOr();
            if (!expression || !Consume(FilterTokenKind::kRightParen, ")")) return nullptr;
            return expression;
        }
        return ParsePredicate();
    }

    std::shared_ptr<FilterExpr> ParsePredicate() {
        if (current_.kind == FilterTokenKind::kIdentifier) {
            std::string name = current_.text;
            if (!Advance()) return nullptr;
            if (current_.kind == FilterTokenKind::kLeftParen) return ParseCall(std::move(name));
            return ParsePredicateTail(ParseIdentifierValue(std::move(name)));
        }

        auto literal = ParseLiteral();
        if (literal) return ParsePredicateTail(std::move(literal));
        if (error_.empty()) SetError("expected predicate", current_.offset);
        return nullptr;
    }

    std::shared_ptr<FilterExpr> ParseCall(std::string function_name) {
        if (!Consume(FilterTokenKind::kLeftParen, "(")) return nullptr;
        std::vector<std::shared_ptr<FilterExpr>> arguments;
        if (current_.kind != FilterTokenKind::kRightParen) {
            while (true) {
                if (current_.kind == FilterTokenKind::kIdentifier) {
                    std::string name = current_.text;
                    if (!Advance()) return nullptr;
                    arguments.push_back(ParseIdentifierValue(std::move(name)));
                } else {
                    auto literal = ParseLiteral();
                    if (!literal) {
                        if (error_.empty()) SetError("expected function argument", current_.offset);
                        return nullptr;
                    }
                    arguments.push_back(std::move(literal));
                }
                if (current_.kind != FilterTokenKind::kComma) break;
                if (!Advance()) return nullptr;
            }
        }
        if (!Consume(FilterTokenKind::kRightParen, ")")) return nullptr;
        auto call = MakeNode(FilterExprKind::kCall, std::move(arguments));
        call->function_name = LowerAscii(std::move(function_name));
        return call;
    }

    std::shared_ptr<FilterExpr> ParsePredicateTail(std::shared_ptr<FilterExpr> first) {
        if (!first) return nullptr;
        const bool first_is_field = first->kind == FilterExprKind::kField;
        if (current_.kind == FilterTokenKind::kNot) {
            if (!first_is_field) {
                SetError("NOT IN/BETWEEN requires a field", current_.offset);
                return nullptr;
            }
            const size_t not_offset = current_.offset;
            if (!Advance()) return nullptr;
            if (current_.kind == FilterTokenKind::kIn) return WrapNot(ParseIn(std::move(first)));
            if (current_.kind == FilterTokenKind::kBetween) return WrapNot(ParseBetween(std::move(first)));
            SetError("expected IN or BETWEEN after NOT", not_offset);
            return nullptr;
        }
        if (current_.kind == FilterTokenKind::kIn) {
            if (!first_is_field) {
                SetError("IN requires a field", current_.offset);
                return nullptr;
            }
            return ParseIn(std::move(first));
        }
        if (current_.kind == FilterTokenKind::kBetween) {
            if (!first_is_field) {
                SetError("BETWEEN requires a field", current_.offset);
                return nullptr;
            }
            return ParseBetween(std::move(first));
        }
        if (current_.kind == FilterTokenKind::kIs) {
            if (!first_is_field) {
                SetError("IS NULL requires a field", current_.offset);
                return nullptr;
            }
            return ParseIsNull(std::move(first));
        }

        const FilterCompareOp compare_op = CurrentCompareOp();
        if (compare_op != FilterCompareOp::kNone) {
            if (!Advance()) return nullptr;
            auto second = ParseValue();
            if (!second) {
                if (error_.empty()) SetError("expected comparison value", current_.offset);
                return nullptr;
            }
            if (!first_is_field && second->kind != FilterExprKind::kField) {
                SetError("comparison requires at least one field", current_.offset);
                return nullptr;
            }
            auto compare = MakeNode(FilterExprKind::kCompare, {std::move(first), std::move(second)});
            compare->compare_op = compare_op;
            return compare;
        }

        if (first_is_field) return first;
        SetError("literal is not a boolean predicate", current_.offset);
        return nullptr;
    }

    std::shared_ptr<FilterExpr> ParseIn(std::shared_ptr<FilterExpr> field) {
        if (!Consume(FilterTokenKind::kIn, "IN") || !Consume(FilterTokenKind::kLeftParen, "(")) {
            return nullptr;
        }
        std::vector<std::shared_ptr<FilterExpr>> operands;
        operands.push_back(std::move(field));
        auto literal = ParseLiteral();
        if (!literal) {
            if (error_.empty()) SetError("IN list must contain at least one literal", current_.offset);
            return nullptr;
        }
        operands.push_back(std::move(literal));
        while (current_.kind == FilterTokenKind::kComma) {
            if (!Advance()) return nullptr;
            literal = ParseLiteral();
            if (!literal) {
                if (error_.empty()) SetError("expected literal after comma", current_.offset);
                return nullptr;
            }
            operands.push_back(std::move(literal));
        }
        if (!Consume(FilterTokenKind::kRightParen, ")")) return nullptr;
        return MakeNode(FilterExprKind::kIn, std::move(operands));
    }

    std::shared_ptr<FilterExpr> ParseBetween(std::shared_ptr<FilterExpr> field) {
        if (!Consume(FilterTokenKind::kBetween, "BETWEEN")) return nullptr;
        auto lower = ParseLiteral();
        if (!lower) {
            if (error_.empty()) SetError("expected lower BETWEEN literal", current_.offset);
            return nullptr;
        }
        if (!Consume(FilterTokenKind::kAnd, "AND")) return nullptr;
        auto upper = ParseLiteral();
        if (!upper) {
            if (error_.empty()) SetError("expected upper BETWEEN literal", current_.offset);
            return nullptr;
        }
        return MakeNode(FilterExprKind::kBetween,
                        {std::move(field), std::move(lower), std::move(upper)});
    }

    std::shared_ptr<FilterExpr> ParseIsNull(std::shared_ptr<FilterExpr> field) {
        if (!Consume(FilterTokenKind::kIs, "IS")) return nullptr;
        bool negate = false;
        if (current_.kind == FilterTokenKind::kNot) {
            negate = true;
            if (!Advance()) return nullptr;
        }
        if (!Consume(FilterTokenKind::kNull, "NULL")) return nullptr;
        auto is_null = MakeNode(FilterExprKind::kIsNull, {std::move(field)});
        return negate ? MakeNot(std::move(is_null)) : is_null;
    }

    std::shared_ptr<FilterExpr> WrapNot(std::shared_ptr<FilterExpr> operand) {
        return operand ? MakeNot(std::move(operand)) : nullptr;
    }

    std::shared_ptr<FilterExpr> ParseIdentifierValue(std::string name) {
        if (current_.kind != FilterTokenKind::kString) return MakeField(std::move(name));
        auto literal = MakeLiteral(
            FilterLiteralKind::kTyped, current_.text, LowerAscii(std::move(name)));
        return Advance() ? literal : nullptr;
    }

    std::shared_ptr<FilterExpr> ParseValue() {
        if (current_.kind == FilterTokenKind::kIdentifier) {
            std::string name = current_.text;
            if (!Advance()) return nullptr;
            return ParseIdentifierValue(std::move(name));
        }
        return ParseLiteral();
    }

    std::shared_ptr<FilterExpr> ParseLiteral() {
        if (current_.kind == FilterTokenKind::kIdentifier) {
            const size_t type_offset = current_.offset;
            std::string type_name = current_.text;
            if (!Advance()) return nullptr;
            if (current_.kind != FilterTokenKind::kString) {
                SetError("expected single-quoted value after typed literal " + type_name, type_offset);
                return nullptr;
            }
            auto literal = MakeLiteral(
                FilterLiteralKind::kTyped, current_.text, LowerAscii(std::move(type_name)));
            return Advance() ? literal : nullptr;
        }

        FilterLiteralKind kind = FilterLiteralKind::kNone;
        switch (current_.kind) {
            case FilterTokenKind::kInteger:
                kind = FilterLiteralKind::kInteger;
                break;
            case FilterTokenKind::kFloating:
                kind = FilterLiteralKind::kFloating;
                break;
            case FilterTokenKind::kString:
                kind = FilterLiteralKind::kString;
                break;
            case FilterTokenKind::kTrue:
            case FilterTokenKind::kFalse:
                kind = FilterLiteralKind::kBoolean;
                break;
            default:
                return nullptr;
        }
        auto literal = MakeLiteral(kind, current_.text);
        return Advance() ? literal : nullptr;
    }

    FilterCompareOp CurrentCompareOp() const {
        switch (current_.kind) {
            case FilterTokenKind::kEqual:
                return FilterCompareOp::kEqual;
            case FilterTokenKind::kNotEqual:
                return FilterCompareOp::kNotEqual;
            case FilterTokenKind::kLess:
                return FilterCompareOp::kLess;
            case FilterTokenKind::kLessEqual:
                return FilterCompareOp::kLessEqual;
            case FilterTokenKind::kGreater:
                return FilterCompareOp::kGreater;
            case FilterTokenKind::kGreaterEqual:
                return FilterCompareOp::kGreaterEqual;
            default:
                return FilterCompareOp::kNone;
        }
    }

    static void AssignNodeIds(const std::shared_ptr<FilterExpr>& expression,
                              uint32_t* next_node_id) {
        if (!expression || !next_node_id) return;
        expression->node_id = (*next_node_id)++;
        for (const auto& operand : expression->operands) AssignNodeIds(operand, next_node_id);
    }

    FilterLexer lexer_;
    FilterToken current_;
    std::string error_;
};

}  // namespace

bool ParseFilterExpression(const std::string& expression,
                           std::shared_ptr<FilterExpr>* output,
                           std::string* error) {
    FilterExpressionParser parser(expression);
    return parser.Parse(output, error);
}

}  // namespace flowsql
