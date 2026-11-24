// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "framework/core/sql_text_splitter.h"

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <cctype>

namespace flowsql {
namespace {

enum class LexState {
    kNormal,
    kSingleQuote,
    kDoubleQuote,
    kBacktick,
    kLineComment,
    kBlockComment,
};

bool IsWhitespace(char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }

std::string Trim(const std::string& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && IsWhitespace(text[begin])) ++begin;
    while (end > begin && IsWhitespace(text[end - 1])) --end;
    return text.substr(begin, end - begin);
}

void SetError(SqlTextSplitError* err, size_t statement_index, const std::string& message) {
    if (!err) return;
    err->statement_index = statement_index;
    err->message = message;
}

}  // namespace

int SplitSqlText(const std::string& sql_text, std::vector<std::string>* statements, SqlTextSplitError* err) {
    if (!statements) return -1;
    statements->clear();
    if (err) {
        err->statement_index = 0;
        err->message.clear();
    }
    if (Trim(sql_text).empty()) {
        SetError(err, 0, "sql_text is empty");
        return -1;
    }

    std::string current;
    current.reserve(sql_text.size());
    bool has_content = false;
    LexState state = LexState::kNormal;

    for (size_t i = 0; i < sql_text.size(); ++i) {
        const char ch = sql_text[i];
        const char next = (i + 1 < sql_text.size()) ? sql_text[i + 1] : '\0';

        switch (state) {
            case LexState::kLineComment:
                current.push_back(ch);
                if (ch == '\n') state = LexState::kNormal;
                continue;
            case LexState::kBlockComment:
                current.push_back(ch);
                if (ch == '*' && next == '/') {
                    current.push_back(next);
                    ++i;
                    state = LexState::kNormal;
                }
                continue;
            case LexState::kSingleQuote:
                current.push_back(ch);
                if (ch == '\'' && next == '\'') {
                    current.push_back(next);
                    ++i;
                    continue;
                }
                if (ch == '\'') state = LexState::kNormal;
                has_content = true;
                continue;
            case LexState::kDoubleQuote:
                current.push_back(ch);
                if (ch == '"' && next == '"') {
                    current.push_back(next);
                    ++i;
                    continue;
                }
                if (ch == '"') state = LexState::kNormal;
                has_content = true;
                continue;
            case LexState::kBacktick:
                current.push_back(ch);
                if (ch == '`' && next == '`') {
                    current.push_back(next);
                    ++i;
                    continue;
                }
                if (ch == '`') state = LexState::kNormal;
                has_content = true;
                continue;
            case LexState::kNormal:
                break;
        }

        if (ch == '-' && next == '-') {
            current.push_back(ch);
            current.push_back(next);
            ++i;
            state = LexState::kLineComment;
            continue;
        }
        if (ch == '/' && next == '*') {
            current.push_back(ch);
            current.push_back(next);
            ++i;
            state = LexState::kBlockComment;
            continue;
        }

        if (ch == '\'') {
            current.push_back(ch);
            state = LexState::kSingleQuote;
            has_content = true;
            continue;
        }
        if (ch == '"') {
            current.push_back(ch);
            state = LexState::kDoubleQuote;
            has_content = true;
            continue;
        }
        if (ch == '`') {
            current.push_back(ch);
            state = LexState::kBacktick;
            has_content = true;
            continue;
        }

        if (ch == ';') {
            if (!has_content) {
                SetError(err, statements->size(), "empty SQL statement");
                return -1;
            }
            const std::string stmt = Trim(current);
            if (stmt.empty()) {
                SetError(err, statements->size(), "empty SQL statement");
                return -1;
            }
            statements->push_back(stmt);
            current.clear();
            has_content = false;
            continue;
        }

        current.push_back(ch);
        if (!IsWhitespace(ch)) has_content = true;
    }

    if (state != LexState::kNormal && state != LexState::kLineComment) {
        SetError(err, statements->size(), "unterminated quote or comment in sql_text");
        return -1;
    }
    if (has_content) {
        const std::string tail = Trim(current);
        if (tail.empty()) {
            SetError(err, statements->size(), "empty SQL statement");
            return -1;
        }
        statements->push_back(tail);
    }
    if (statements->empty()) {
        SetError(err, 0, "sql_text is empty");
        return -1;
    }
    return 0;
}

namespace {
// DESC has a deliberately small grammar; comments and quoted identifiers are tokens,
// never string-prefix heuristics. SplitSqlText already validates quote/comment closure.
class CommandTokens {
 public:
    explicit CommandTokens(const std::string& text) : text_(text) { Next(); }
    bool Keyword(const char* word) {
        if (quoted_ || token_ != word) return false;
        Next();
        return true;
    }
    std::string Identifier() {
        if (!identifier_) return {};
        auto value = value_;
        Next();
        return value;
    }
    bool End() const { return end_; }

 private:
    void Next() {
        while (position_ < text_.size()) {
            if (IsWhitespace(text_[position_])) {
                ++position_;
                continue;
            }
            if (text_.compare(position_, 2, "--") == 0) {
                while (position_ < text_.size() && text_[position_] != '\n') ++position_;
                continue;
            }
            if (text_.compare(position_, 2, "/*") == 0) {
                position_ = text_.find("*/", position_ + 2) + 2;
                continue;
            }
            break;
        }
        value_.clear();
        token_.clear();
        quoted_ = false;
        identifier_ = false;
        end_ = position_ == text_.size();
        if (end_) return;
        char ch = text_[position_++];
        if (ch == '"') {
            quoted_ = true;
            while (position_ < text_.size()) {
                ch = text_[position_++];
                if (ch == '"') {
                    if (position_ == text_.size() || text_[position_] != '"') break;
                    ++position_;
                }
                value_ += ch;
            }
            identifier_ = !value_.empty();
        } else {
            value_ += ch;
            identifier_ = std::isalnum(static_cast<unsigned char>(ch)) || ch == '_';
            if (identifier_) {
                while (position_ < text_.size()) {
                    ch = text_[position_];
                    if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_') break;
                    value_ += ch;
                    ++position_;
                }
            }
        }
        token_ = value_;
        for (auto& c : token_) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    const std::string& text_;
    size_t position_ = 0;
    std::string value_, token_;
    bool quoted_ = false, identifier_ = false, end_ = false;
};
}  // namespace

SqlCommand ParseSqlCommand(const std::string& sql_text) {
    SqlCommand command;
    if (sql_text.find('\0') != std::string::npos) {
        command.error.message = "NUL is not allowed in SQL";
        return command;
    }
    if (SplitSqlText(sql_text, &command.statements, &command.error) != 0) return command;
    for (size_t i = 0; i < command.statements.size(); ++i) {
        CommandTokens tokens(command.statements[i]);
        if (!tokens.Keyword("DESC")) continue;
        command.kind = SqlCommandKind::kDescribe;
        command.error.statement_index = i;
        if (command.statements.size() != 1) {
            command.error.message = "DESC must be the only statement";
            return command;
        }
        const bool channel = tokens.Keyword("CHANNEL");
        if (!channel && !tokens.Keyword("OPERATOR")) {
            command.error.message = "expected CHANNEL or OPERATOR after DESC";
            return command;
        }
        const auto type = tokens.Identifier();
        const bool dot = tokens.Keyword(".");
        const auto name = tokens.Identifier();
        if (type.empty() || !dot || name.empty()) {
            command.error.message = "expected <type>.<name>";
            return command;
        }
        std::string table, database, schema;
        if (channel && type != "dataframe" && type != "kafka") {
            if (tokens.Keyword("TABLE")) table = tokens.Identifier();
            if (table.empty()) {
                command.error.message = "database channel requires TABLE <table>";
                return command;
            }
            if (tokens.Keyword("DATABASE")) {
                database = tokens.Identifier();
                if (database.empty()) {
                    command.error.message = "expected database name";
                    return command;
                }
            }
            if (tokens.Keyword("SCHEMA")) {
                schema = tokens.Identifier();
                if (schema.empty()) {
                    command.error.message = "expected schema name";
                    return command;
                }
            }
        }
        if (!tokens.End()) {
            command.error.message = "unexpected syntax after DESC target";
            return command;
        }
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        const auto member = [&](const char* key, const std::string& value) {
            writer.Key(key);
            writer.String(value.data(), static_cast<rapidjson::SizeType>(value.size()));
        };
        writer.StartObject();
        member("kind", channel ? "channel" : "operator");
        if (!channel) {
            member("name", type + "." + name);
        } else {
            writer.Key("locator");
            writer.StartObject();
            member("kind", type == "dataframe" ? "dataframe" : type == "kafka" ? "kafka_source" : "database_table");
            member("name", name);
            if (!table.empty()) {
                member("type", type);
                member("table", table);
            }
            if (!database.empty()) member("database", database);
            if (!schema.empty()) member("schema", schema);
            writer.EndObject();
        }
        writer.EndObject();
        command.target_json = buffer.GetString();
    }
    return command;
}

}  // namespace flowsql
