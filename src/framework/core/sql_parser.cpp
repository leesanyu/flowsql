// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "sql_parser.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace flowsql {

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
            if (stmt.sources.size() != 1) {
                stmt.error = "source-stage WHERE does not support multiple sources";
                return stmt;
            }

            if (!HasDatabaseReferenceShape(stmt.source)) {
                stmt.stage_filters.push_back({0, stmt.where_clause});
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
                if (filter_text.empty()) {
                    stmt.error = "operator-stage WHERE expression must not be empty";
                    return stmt;
                }
                if (FindTopLevelKeyword(filter_text.data(),
                                        filter_text.data() + filter_text.size(),
                                        "WHERE")) {
                    stmt.error = "operator-stage WHERE clause contains unexpected WHERE";
                    return stmt;
                }
                stmt.stage_filters.push_back({stage, filter_text});
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
