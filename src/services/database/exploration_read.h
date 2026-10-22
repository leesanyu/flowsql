// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <framework/interfaces/idatabase_exploration.h>

#include <cctype>
#include <charconv>
#include <cmath>
#include <stdexcept>

namespace flowsql::database {
inline bool ExplorationSelect(const char* sql, const std::string& dialect) {
    if (!sql) return false;
    std::string token;
    bool first = true;
    auto accept = [&] {
        if (token.empty()) return true;
        if (first && token != "SELECT") return false;
        first = false;
        const bool ok = token != "INTO" && token != "SETTINGS" && token != "FORMAT";
        token.clear();
        return ok;
    };
    char quote = 0;
    for (size_t i = 0; sql[i]; ++i) {
        const char c = sql[i];
        if (quote) {
            if (c == '\\' && sql[i + 1] && dialect != "postgres" && (quote != '`' || dialect == "clickhouse"))
                ++i;
            else if (c == quote) {
                if (sql[i + 1] == quote)
                    ++i;
                else
                    quote = 0;
            }
        } else if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
            token += std::toupper(static_cast<unsigned char>(c));
        } else {
            if (!accept()) return false;
            if (c == ';' || (c == '-' && sql[i + 1] == '-') || (c == '/' && sql[i + 1] == '*')) return false;
            if (c == '\'' || c == '"' || c == '`') quote = c;
        }
    }
    return !quote && accept() && !first;
}
inline ExplorationValueV1 ExplorationText(const std::string& text, const std::string& type) {
    if (type == "integer") {
        int64_t value = 0;
        auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size()) return value;
        return ExplorationDecimalV1{text};
    }
    if (type == "decimal") return ExplorationDecimalV1{text};
    if (type == "number") {
        size_t end = 0;
        auto value = std::stod(text, &end);
        if (end != text.size()) throw std::runtime_error("invalid numeric result");
        return value;
    }
    if (type == "boolean") return int64_t(text == "t" || text == "true" || text == "1");
    if (type == "binary") return std::vector<uint8_t>(text.begin(), text.end());
    return text;
}
}  // namespace flowsql::database
