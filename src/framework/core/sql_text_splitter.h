// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_FRAMEWORK_CORE_SQL_TEXT_SPLITTER_H_
#define _FLOWSQL_FRAMEWORK_CORE_SQL_TEXT_SPLITTER_H_

#include <string>
#include <vector>

namespace flowsql {

struct SqlTextSplitError {
    size_t statement_index = 0;  // 0-based index
    std::string message;
};

enum class SqlCommandKind { kExecute, kDescribe };

struct SqlCommand {
    SqlCommandKind kind = SqlCommandKind::kExecute;
    std::vector<std::string> statements;
    // Unified metadata descriptionTarget, populated only for a valid DESC.
    std::string target_json;
    SqlTextSplitError error;
};

// Validate the whole input before dispatch. DESC must be the only statement.
// Execute statements retain their existing parser/driver validation.
SqlCommand ParseSqlCommand(const std::string& sql_text);

// Split `sql_text` by semicolon in lexical-normal state.
// Returns 0 on success; non-zero on invalid sql_text.
int SplitSqlText(const std::string& sql_text, std::vector<std::string>* statements, SqlTextSplitError* err = nullptr);

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_CORE_SQL_TEXT_SPLITTER_H_
