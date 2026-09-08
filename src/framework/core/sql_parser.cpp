// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "sql_parser.h"

#include <algorithm>
#include <cctype>
#include <cstring>

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

    std::shared_ptr<FilterExpr> MakeLiteral(FilterLiteralKind kind, std::string text) {
        auto node = MakeNode(FilterExprKind::kLiteral);
        node->literal.kind = kind;
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
            return ParsePredicateTail(MakeField(std::move(name)));
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
                    arguments.push_back(MakeField(current_.text));
                    if (!Advance()) return nullptr;
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
        if (!Consume(FilterTokenKind::kIn, "IN") ||
            !Consume(FilterTokenKind::kLeftParen, "(")) {
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

    std::shared_ptr<FilterExpr> ParseValue() {
        if (current_.kind == FilterTokenKind::kIdentifier) {
            auto field = MakeField(current_.text);
            return Advance() ? field : nullptr;
        }
        return ParseLiteral();
    }

    std::shared_ptr<FilterExpr> ParseLiteral() {
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

bool SqlParser::ParseFilterExpression(const std::string& expression,
                                      std::shared_ptr<FilterExpr>* output,
                                      std::string* error) {
    FilterExpressionParser parser(expression);
    return parser.Parse(output, error);
}

void SqlParser::SkipWhitespace() {
    while (pos_ < end_ && std::isspace(*pos_)) ++pos_;
}

bool SqlParser::MatchKeyword(const char* keyword) {
    SkipWhitespace();
    size_t len = strlen(keyword);

    // 剩余输入不足以容纳关键字
    if (pos_ + len > end_) return false;

    // 大小写不敏感匹配
    for (size_t i = 0; i < len; ++i) {
        if (std::toupper(static_cast<unsigned char>(pos_[i])) !=
            std::toupper(static_cast<unsigned char>(keyword[i]))) return false;
    }

    // 关键字后必须结束标识符，防止前缀误匹配（如 "SELECT" 匹配 "SELECT_FOO"）
    const char* after = pos_ + len;
    if (after < end_ &&
        (std::isalnum(static_cast<unsigned char>(*after)) || *after == '_')) {
        return false;
    }

    pos_ += len;
    return true;
}

// 读取标识符：字母、数字、下划线、点、连字符
std::string SqlParser::ReadIdentifier() {
    SkipWhitespace();
    const char* start = pos_;
    while (pos_ < end_ && (std::isalnum(*pos_) || *pos_ == '_' || *pos_ == '.' || *pos_ == '-')) {
        ++pos_;
    }
    return std::string(start, pos_);
}

std::string SqlParser::ReadChannelRef(std::string* err) {
    if (err) err->clear();
    const std::string base = ReadIdentifier();
    if (base.empty()) return "";

    const char* saved = pos_;
    SkipWhitespace();
    if (pos_ >= end_ || *pos_ != '[') {
        pos_ = saved;
        return base;
    }

    ++pos_;  // skip '['
    SkipWhitespace();
    if (pos_ >= end_) {
        if (err) *err = "invalid channel selector: missing ']'";
        return "";
    }

    std::string selector;
    if (*pos_ == '*') {
        selector = "[*]";
        ++pos_;
    } else {
        const char* digits_start = pos_;
        while (pos_ < end_ && std::isdigit(static_cast<unsigned char>(*pos_))) {
            ++pos_;
        }
        if (digits_start == pos_) {
            if (err) *err = "invalid channel selector: expected '*' or index";
            return "";
        }
        selector = "[";
        selector.append(digits_start, pos_);
        selector.push_back(']');
    }

    SkipWhitespace();
    if (pos_ >= end_ || *pos_ != ']') {
        if (err) *err = "invalid channel selector: missing ']'";
        return "";
    }
    ++pos_;  // skip ']'

    SkipWhitespace();
    if (pos_ < end_ && *pos_ == '[') {
        if (err) *err = "invalid channel selector: duplicate selector is not allowed";
        return "";
    }
    return base + selector;
}

// 读取值：支持带引号的字符串或普通标识符
std::string SqlParser::ReadValue() {
    SkipWhitespace();
    if (pos_ < end_ && (*pos_ == '"' || *pos_ == '\'')) {
        char quote = *pos_++;
        const char* start = pos_;
        while (pos_ < end_ && *pos_ != quote) ++pos_;
        std::string val(start, pos_);
        if (pos_ < end_) ++pos_;  // 跳过结束引号
        return val;
    }
    // 无引号：读到逗号、空白或结尾
    const char* start = pos_;
    while (pos_ < end_ && !std::isspace(*pos_) && *pos_ != ',') ++pos_;
    return std::string(start, pos_);
}

static bool IsSqlWordChar(char c) {
    const unsigned char value = static_cast<unsigned char>(c);
    return std::isalnum(value) || c == '_';
}

static const char* SkipAsciiWhitespace(const char* pos, const char* end) {
    while (pos < end && std::isspace(static_cast<unsigned char>(*pos))) ++pos;
    return pos;
}

static bool KeywordAt(const char* pos, const char* begin, const char* end, const char* keyword) {
    const size_t length = std::strlen(keyword);
    if (pos + length > end || (pos > begin && IsSqlWordChar(pos[-1]))) return false;
    for (size_t i = 0; i < length; ++i) {
        if (std::toupper(static_cast<unsigned char>(pos[i])) !=
            std::toupper(static_cast<unsigned char>(keyword[i]))) {
            return false;
        }
    }
    return pos + length == end || !IsSqlWordChar(pos[length]);
}

static const char* FindTopLevelKeyword(const char* begin, const char* end, const char* keyword) {
    int parenthesis_depth = 0;
    char quote = 0;
    for (const char* pos = begin; pos < end; ++pos) {
        if (quote != 0) {
            if (*pos == quote) {
                if (pos + 1 < end && pos[1] == quote) {
                    ++pos;
                } else {
                    quote = 0;
                }
            }
            continue;
        }
        if (*pos == '\'' || *pos == '"') {
            quote = *pos;
            continue;
        }
        if (*pos == '(') {
            ++parenthesis_depth;
            continue;
        }
        if (*pos == ')') {
            if (parenthesis_depth > 0) --parenthesis_depth;
            continue;
        }
        if (parenthesis_depth == 0 && KeywordAt(pos, begin, end, keyword)) return pos;
    }
    return nullptr;
}

static std::string TrimRange(const char* begin, const char* end) {
    begin = SkipAsciiWhitespace(begin, end);
    while (end > begin && std::isspace(static_cast<unsigned char>(end[-1]))) --end;
    return std::string(begin, end);
}

static bool IsNativeJoinUsing(const char* pos, const char* end) {
    pos = SkipAsciiWhitespace(pos, end);
    return pos < end && *pos == '(';
}

static const char* Earlier(const char* lhs, const char* rhs) {
    if (!lhs) return rhs;
    if (!rhs) return lhs;
    return lhs < rhs ? lhs : rhs;
}

static const char* FindFlowExtensionStart(const char* begin, const char* end) {
    const char* extension = nullptr;

    const char* using_pos = FindTopLevelKeyword(begin, end, "USING");
    while (using_pos && IsNativeJoinUsing(using_pos + std::strlen("USING"), end)) {
        using_pos = FindTopLevelKeyword(using_pos + std::strlen("USING"), end, "USING");
    }
    extension = Earlier(extension, using_pos);

    const char* with_pos = FindTopLevelKeyword(begin, end, "WITH");
    extension = Earlier(extension, with_pos);

    const char* into_pos = FindTopLevelKeyword(begin, end, "INTO");
    return Earlier(extension, into_pos);
}

static const char* FindOperatorFilterEnd(const char* begin, const char* end) {
    const char* then_pos = FindTopLevelKeyword(begin, end, "THEN");
    const char* into_pos = FindTopLevelKeyword(begin, end, "INTO");
    const char* delimiter = Earlier(then_pos, into_pos);
    return delimiter ? delimiter : end;
}

static bool HasDatabaseReferenceShape(const std::string& source) {
    return std::count(source.begin(), source.end(), '.') >= 2;
}

static bool HasNativeSqlClause(const std::string& text) {
    static const char* keywords[] = {
        "GROUP", "HAVING", "ORDER", "LIMIT", "JOIN", "UNION", "OFFSET", "FETCH", "FOR",
    };
    for (const char* keyword : keywords) {
        if (FindTopLevelKeyword(text.data(), text.data() + text.size(), keyword)) return true;
    }
    return false;
}

SqlStatement SqlParser::Parse(const std::string& sql) {
    // 逻辑链：
    // 1) 顺序解析 SELECT/FROM/WHERE 主体；
    // 2) 识别 USING/THEN 算子链、WITH 参数与 INTO 目标；
    // 3) 构建 sources/operators/with_params 的统一结构；
    // 4) 在每个阶段失败即返回带 error 的 SqlStatement。
    SqlStatement stmt;
    pos_ = sql.c_str();
    end_ = pos_ + sql.size();

    // SELECT
    if (!MatchKeyword("SELECT")) {
        stmt.error = "expected SELECT";
        return stmt;
    }

    // 列选择：* 或 col1, col2, ...（支持函数调用）
    SkipWhitespace();
    if (pos_ < end_ && *pos_ == '*') {
        ++pos_;
    } else {
        while (true) {
            SkipWhitespace();
            const char* col_start = pos_;
            int paren_depth = 0;
            bool in_string = false;
            char string_char = 0;

            while (pos_ < end_) {
                char c = *pos_;

                if (!in_string && (c == '\'' || c == '"')) {
                    in_string = true;
                    string_char = c;
                    ++pos_;
                    continue;
                }
                if (in_string && c == string_char) {
                    in_string = false;
                    ++pos_;
                    continue;
                }
                if (in_string) {
                    ++pos_;
                    continue;
                }

                if (c == '(') {
                    ++paren_depth;
                    ++pos_;
                    continue;
                }
                if (c == ')') {
                    if (paren_depth > 0) {
                        --paren_depth;
                        ++pos_;
                        continue;
                    }
                    break;
                }

                if (paren_depth == 0 && (c == ',' || std::isspace(static_cast<unsigned char>(c)))) {
                    break;
                }

                ++pos_;
            }

            std::string col(col_start, pos_);
            if (col.empty()) {
                stmt.error = "expected column name or * after SELECT";
                return stmt;
            }
            stmt.columns.push_back(col);

            SkipWhitespace();
            if (pos_ < end_ && *pos_ == ',') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    // FROM <source>[,<source>...]
    if (!MatchKeyword("FROM")) {
        stmt.error = "expected FROM";
        return stmt;
    }
    stmt.sources.clear();
    std::string channel_err;
    stmt.sources.push_back(ReadChannelRef(&channel_err));
    if (stmt.sources[0].empty()) {
        if (!channel_err.empty()) {
            stmt.error = channel_err;
            return stmt;
        }
        stmt.error = "expected source channel name after FROM";
        return stmt;
    }
    SkipWhitespace();
    while (pos_ < end_ && *pos_ == ',') {
        ++pos_;  // 跳过逗号
        SkipWhitespace();
        std::string src = ReadChannelRef(&channel_err);
        if (src.empty()) {
            if (!channel_err.empty()) {
                stmt.error = channel_err;
                return stmt;
            }
            stmt.error = "expected source channel name after ','";
            return stmt;
        }
        stmt.sources.push_back(std::move(src));
        SkipWhitespace();
    }
    stmt.source = stmt.sources[0];  // 向后兼容

    const char* source_tail_begin = pos_;
    const char* extension_start = FindFlowExtensionStart(source_tail_begin, end_);
    const char* source_tail_end = extension_start ? extension_start : end_;
    stmt.sql_part = TrimRange(sql.data(), source_tail_end);

    const char* source_clause_begin = SkipAsciiWhitespace(source_tail_begin, source_tail_end);
    if (source_clause_begin < source_tail_end) {
        if (KeywordAt(source_clause_begin, source_clause_begin, source_tail_end, "WHERE")) {
            const char* expression_begin = source_clause_begin + std::strlen("WHERE");
            stmt.where_clause = TrimRange(expression_begin, source_tail_end);
            if (stmt.where_clause.empty()) {
                stmt.error = "source-stage WHERE expression must not be empty";
                return stmt;
            }
            if (!ValidateWhereClause(stmt.where_clause)) {
                stmt.error = "WHERE clause contains forbidden keywords";
                return stmt;
            }
            if (stmt.sources.size() != 1) {
                stmt.error = "source-stage WHERE does not support multiple sources";
                return stmt;
            }

            std::shared_ptr<FilterExpr> expression;
            std::string filter_error;
            if (ParseFilterExpression(stmt.where_clause, &expression, &filter_error)) {
                stmt.stage_filters.push_back({0, std::move(expression)});
            } else if (!HasDatabaseReferenceShape(stmt.source) &&
                       !HasNativeSqlClause(stmt.where_clause)) {
                stmt.error = "invalid source-stage WHERE: " + filter_error;
                return stmt;
            }
        } else {
            const std::string source_tail = TrimRange(source_clause_begin, source_tail_end);
            if (!HasDatabaseReferenceShape(stmt.source) && !HasNativeSqlClause(source_tail)) {
                stmt.error = "unexpected source SQL tail: " + source_tail;
                return stmt;
            }
        }
    }

    pos_ = extension_start ? extension_start : end_;

    auto parse_with_params = [this](std::unordered_map<std::string, std::string>* out,
                                    std::string* err) -> bool {
        if (!out || !err) return false;
        out->clear();
        if (!MatchKeyword("WITH")) return true;
        bool parsed_any = false;
        while (true) {
            std::string key = ReadIdentifier();
            if (key.empty()) {
                *err = parsed_any ? "expected WITH key after ','" : "expected key after WITH";
                return false;
            }

            SkipWhitespace();
            if (pos_ >= end_ || *pos_ != '=') {
                *err = "expected = after WITH key: " + key;
                return false;
            }
            ++pos_;

            std::string val = ReadValue();
            if (val.empty()) {
                *err = "expected value after WITH key: " + key;
                return false;
            }
            (*out)[key] = val;
            parsed_any = true;

            SkipWhitespace();
            if (pos_ < end_ && *pos_ == ',') {
                ++pos_;
            } else {
                break;
            }
        }
        return true;
    };

    // [USING <category.name> [WITH ...] [WHERE ...]
    //        (THEN <category.name> [WITH ...] [WHERE ...])*]
    const char* saved_pos = pos_;
    if (MatchKeyword("USING")) {
        uint32_t stage = 1;
        while (true) {
            std::string op_full = ReadIdentifier();
            auto dot = op_full.find('.');
            if (dot == std::string::npos || dot == 0 || dot == op_full.size() - 1) {
                stmt.error = "expected category.name format after USING/THEN, got: " + op_full;
                return stmt;
            }
            OperatorRef op_ref;
            op_ref.category = op_full.substr(0, dot);
            op_ref.name = op_full.substr(dot + 1);
            stmt.operators.push_back(std::move(op_ref));

            std::unordered_map<std::string, std::string> op_params;
            if (!parse_with_params(&op_params, &stmt.error)) {
                return stmt;
            }
            stmt.operator_with_params.push_back(std::move(op_params));

            const char* where_pos = pos_;
            if (MatchKeyword("WHERE")) {
                const char* expression_start = pos_;
                const char* expression_end = FindOperatorFilterEnd(expression_start, end_);
                const std::string filter_text = TrimRange(expression_start, expression_end);
                std::shared_ptr<FilterExpr> expression;
                std::string filter_error;
                if (!ParseFilterExpression(filter_text, &expression, &filter_error)) {
                    stmt.error = "invalid operator-stage WHERE: " + filter_error;
                    return stmt;
                }
                stmt.stage_filters.push_back({stage, std::move(expression)});
                pos_ = expression_end;
            } else {
                pos_ = where_pos;
            }

            const char* then_pos = pos_;
            if (!MatchKeyword("THEN")) {
                pos_ = then_pos;
                break;
            }
            ++stage;
        }

        if (!stmt.operators.empty()) {
            stmt.op_category = stmt.operators[0].category;
            stmt.op_name = stmt.operators[0].name;
            if (!stmt.operator_with_params.empty()) stmt.with_params = stmt.operator_with_params[0];
        }
    } else {
        pos_ = saved_pos;
    }

    if (!stmt.operators.empty()) {
        // 每个算子必须在自身后跟 WITH（可选），不支持链路级全局 WITH。
        const char* with_pos = pos_;
        if (MatchKeyword("WITH")) {
            stmt.error = "global WITH is not supported; use WITH after each operator";
            return stmt;
        }
        pos_ = with_pos;
    } else {
        // 兼容无算子场景下的历史 WITH 语法。
        if (!parse_with_params(&stmt.with_params, &stmt.error)) {
            return stmt;
        }
    }

    // [INTO <dest>]
    if (MatchKeyword("INTO")) {
        std::string dest_err;
        stmt.dest = ReadChannelRef(&dest_err);
        if (stmt.dest.empty()) {
            if (!dest_err.empty()) {
                stmt.error = dest_err;
                return stmt;
            }
            stmt.error = "expected destination channel name after INTO";
            return stmt;
        }
    }

    SkipWhitespace();
    if (pos_ != end_) {
        stmt.error = "unexpected trailing input: " + TrimRange(pos_, end_);
        return stmt;
    }

    return stmt;
}

bool SqlParser::ValidateWhereClause(const std::string& clause) {
    // 先剥离注释，防止注释内的关键字绕过检查
    std::string stripped;
    stripped.reserve(clause.size());
    size_t i = 0;
    while (i < clause.size()) {
        // 块注释 /* ... */
        if (i + 1 < clause.size() && clause[i] == '/' && clause[i + 1] == '*') {
            i += 2;
            while (i + 1 < clause.size() && !(clause[i] == '*' && clause[i + 1] == '/')) ++i;
            if (i + 1 < clause.size()) i += 2;  // 跳过 */
            stripped += ' ';
            continue;
        }
        // 行注释 -- ...
        if (i + 1 < clause.size() && clause[i] == '-' && clause[i + 1] == '-') {
            while (i < clause.size() && clause[i] != '\n') ++i;
            stripped += ' ';
            continue;
        }
        stripped += clause[i++];
    }

    std::string upper = stripped;
    std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);

    static const char* forbidden[] = {
        "DROP", "DELETE", "INSERT", "UPDATE", "ALTER", "CREATE",
        "EXEC", "EXECUTE", "TRUNCATE", "GRANT", "REVOKE",
        "--", "/*", "*/", ";",
    };

    for (const char* kw : forbidden) {
        std::string keyword(kw);
        auto pos = upper.find(keyword);
        if (pos != std::string::npos) {
            if (std::isalpha(keyword[0])) {
                bool word_start = (pos == 0 || !std::isalnum(upper[pos - 1]));
                bool word_end = (pos + keyword.size() >= upper.size() ||
                                 !std::isalnum(upper[pos + keyword.size()]));
                if (word_start && word_end) return false;
            } else {
                return false;
            }
        }
    }
    return true;
}

}  // namespace flowsql
