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

    // 关键字后必须是空白、结尾或 '\0'，防止前缀误匹配（如 "SELECT" 匹配 "SELECTFOO"）
    const char* after = pos_ + len;
    if (after < end_ && std::isalnum(static_cast<unsigned char>(*after))) return false;

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

// 辅助：从 SQL 中正向查找 USING/WITH/INTO 的最早出现位置
static size_t FindExtensionStart(const std::string& sql) {
    size_t earliest_pos = std::string::npos;
    const char* keywords[] = {"USING", "WITH", "INTO"};

    for (const char* kw : keywords) {
        size_t len = strlen(kw);
        size_t pos = 0;

        while (pos + len <= sql.size()) {
            // 跳过空白后检查关键字
            while (pos < sql.size() && std::isspace(static_cast<unsigned char>(sql[pos]))) ++pos;
            if (pos + len > sql.size()) break;

            // 检查是否匹配关键字
            bool match = true;
            for (size_t j = 0; j < len; ++j) {
                if (std::toupper(static_cast<unsigned char>(sql[pos + j])) != std::toupper(static_cast<unsigned char>(kw[j]))) {
                    match = false;
                    break;
                }
            }

            // 关键字后必须是空白或结尾
            if (match && (pos + len >= sql.size() || std::isspace(static_cast<unsigned char>(sql[pos + len])))) {
                // 关键字前必须是空白或开头
                if (pos == 0 || std::isspace(static_cast<unsigned char>(sql[pos - 1]))) {
                    if (earliest_pos == std::string::npos || pos < earliest_pos) {
                        earliest_pos = pos;
                    }
                    break;  // 找到这个关键字的第一个匹配，继续找下一个关键字
                }
            }

            ++pos;
        }
    }

    return earliest_pos;
}

SqlStatement SqlParser::Parse(const std::string& sql) {
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

    // FROM <source>
    if (!MatchKeyword("FROM")) {
        stmt.error = "expected FROM";
        return stmt;
    }
    stmt.source = ReadIdentifier();
    if (stmt.source.empty()) {
        stmt.error = "expected source channel name after FROM";
        return stmt;
    }

    // WHERE（可选）- 简单处理：读到行尾或 USING/WITH/INTO
    const char* saved = pos_;
    if (MatchKeyword("WHERE")) {
        SkipWhitespace();
        const char* where_start = pos_;

        // 读取 WHERE 条件直到遇到 USING/WITH/INTO 或结尾
        while (pos_ < end_) {
            const char* check = pos_;
            SkipWhitespace();

            // 检查是否遇到扩展关键字
            bool found_ext = false;
            const char* ext_keywords[] = {"USING", "WITH", "INTO"};
            for (const char* kw : ext_keywords) {
                size_t len = strlen(kw);
                if (pos_ + len <= end_) {
                    bool match = true;
                    for (size_t j = 0; j < len; ++j) {
                        if (std::toupper(static_cast<unsigned char>(pos_[j])) != std::toupper(static_cast<unsigned char>(kw[j]))) {
                            match = false;
                            break;
                        }
                    }
                    if (match && (pos_ + len >= end_ || !std::isalnum(static_cast<unsigned char>(pos_[len])))) {
                        found_ext = true;
                        break;
                    }
                }
            }

            if (found_ext) {
                pos_ = check;
                break;
            }
            ++pos_;
        }

        // 去除尾部空白
        const char* where_end = pos_;
        while (where_end > where_start && std::isspace(*(where_end - 1))) --where_end;
        stmt.where_clause = std::string(where_start, where_end);

        if (!stmt.where_clause.empty()) {
            if (!ValidateWhereClause(stmt.where_clause)) {
                stmt.error = "WHERE clause contains forbidden keywords";
                return stmt;
            }
        } else {
            // WHERE 子句为空，清除并回退 pos_
            stmt.where_clause.clear();
            pos_ = saved;
        }
    } else {
        pos_ = saved;
    }

    // 反向查找 USING/WITH/INTO 的位置，提取 sql_part
    size_t extension_start = FindExtensionStart(sql);
    if (extension_start != std::string::npos) {
        // 去除尾部空白
        size_t end = extension_start;
        while (end > 0 && std::isspace(static_cast<unsigned char>(sql[end - 1]))) --end;
        stmt.sql_part = sql.substr(0, end);

        // 设置 pos_ 到扩展关键字位置，供后续解析
        pos_ = sql.c_str() + extension_start;
    } else {
        // 无扩展关键字，去除尾部空白
        size_t end = sql.size();
        while (end > 0 && std::isspace(static_cast<unsigned char>(sql[end - 1]))) --end;
        stmt.sql_part = sql.substr(0, end);
    }

    auto parse_with_params = [this](std::unordered_map<std::string, std::string>* out, std::string* err) -> bool {
        if (!out || !err) return false;
        out->clear();
        if (!MatchKeyword("WITH")) return true;
        while (true) {
            std::string key = ReadIdentifier();
            if (key.empty()) break;

            SkipWhitespace();
            if (pos_ >= end_ || *pos_ != '=') {
                *err = "expected = after WITH key: " + key;
                return false;
            }
            ++pos_;

            std::string val = ReadValue();
            (*out)[key] = val;

            SkipWhitespace();
            if (pos_ < end_ && *pos_ == ',') {
                ++pos_;
            } else {
                break;
            }
        }
        return true;
    };

    // [USING <catelog.name> [WITH ...] (THEN <catelog.name> [WITH ...])*]
    const char* saved_pos = pos_;
    if (MatchKeyword("USING")) {
        while (true) {
            std::string op_full = ReadIdentifier();
            auto dot = op_full.find('.');
            if (dot == std::string::npos || dot == 0 || dot == op_full.size() - 1) {
                stmt.error = "expected catelog.name format after USING/THEN, got: " + op_full;
                return stmt;
            }
            OperatorRef op_ref;
            op_ref.catelog = op_full.substr(0, dot);
            op_ref.name = op_full.substr(dot + 1);
            stmt.operators.push_back(std::move(op_ref));

            std::unordered_map<std::string, std::string> op_params;
            if (!parse_with_params(&op_params, &stmt.error)) {
                return stmt;
            }
            stmt.operator_with_params.push_back(std::move(op_params));

            const char* then_pos = pos_;
            if (!MatchKeyword("THEN")) {
                pos_ = then_pos;
                break;
            }
        }

        if (!stmt.operators.empty()) {
            stmt.op_catelog = stmt.operators[0].catelog;
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
        stmt.dest = ReadIdentifier();
        if (stmt.dest.empty()) {
            stmt.error = "expected destination channel name after INTO";
            return stmt;
        }
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
