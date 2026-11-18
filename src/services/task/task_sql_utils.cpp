// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "task_sql_utils.h"

#include <framework/core/sql_parser.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace flowsql {
namespace task {

std::string TruncateSql(const std::string& sql) {
    static constexpr size_t kMaxSql = 4096;
    if (sql.size() <= kMaxSql) return sql;
    return sql.substr(0, kMaxSql);
}

std::string TruncateSummary(const std::string& sql) {
    static constexpr size_t kMaxSummary = 200;
    if (sql.size() <= kMaxSummary) return sql;
    return sql.substr(0, kMaxSummary);
}

std::string BuildSqlsJson(const std::vector<std::string>& sqls) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartArray();
    for (const auto& sql : sqls) w.String(sql.c_str());
    w.EndArray();
    return buf.GetString();
}

bool ParseSqlsJson(const std::string& sqls_json, std::vector<std::string>* sqls) {
    if (!sqls) return false;
    sqls->clear();
    if (sqls_json.empty()) return false;

    rapidjson::Document doc;
    doc.Parse(sqls_json.c_str());
    if (doc.HasParseError() || !doc.IsArray()) return false;
    for (rapidjson::SizeType i = 0; i < doc.Size(); ++i) {
        if (!doc[i].IsString()) return false;
        std::string sql = doc[i].GetString();
        if (sql.empty()) return false;
        sqls->push_back(std::move(sql));
    }
    return !sqls->empty();
}

bool IsDataFrameRef(const std::string& name) {
    static const std::string prefix = "dataframe.";
    if (name.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(name[i])) != prefix[i]) return false;
    }
    return true;
}

std::string DataFrameNamePart(const std::string& full_name) {
    static const std::string prefix = "dataframe.";
    if (!IsDataFrameRef(full_name)) return full_name;
    return full_name.substr(prefix.size());
}

std::string ExtractStageFromErrorMessage(const std::string& error_message) {
    // Pipeline 错误格式：operator <category>.<name> execution failed
    static constexpr const char* kPrefix = "operator ";
    static constexpr const char* kSuffix = " execution failed";
    if (error_message.size() <= std::strlen(kPrefix) + std::strlen(kSuffix)) return "";
    if (error_message.rfind(kPrefix, 0) != 0) return "";
    if (error_message.size() < std::strlen(kSuffix)) return "";
    if (error_message.compare(error_message.size() - std::strlen(kSuffix), std::strlen(kSuffix), kSuffix) != 0) {
        return "";
    }
    const size_t begin = std::strlen(kPrefix);
    const size_t end = error_message.size() - std::strlen(kSuffix);
    if (end <= begin) return "";
    return error_message.substr(begin, end - begin);
}

std::string BuildOperatorChainFromSql(const std::string& sql) {
    SqlParser p;
    auto stmt = p.Parse(sql);
    if (!stmt.error.empty()) return "";

    std::vector<std::string> chain;
    if (!stmt.operators.empty()) {
        chain.reserve(stmt.operators.size());
        for (const auto& op : stmt.operators) {
            chain.push_back(op.category + "." + op.name);
        }
    }

    if (chain.empty()) return "";
    std::string out;
    for (size_t i = 0; i < chain.size(); ++i) {
        if (i != 0) out += "->";
        out += chain[i];
    }
    return out;
}

void ParseRuntimeErrorCode(const rapidjson::Value& value,
                           std::string* error_code_out,
                           int* error_no_out) {
    if (error_code_out) error_code_out->clear();
    if (error_no_out) *error_no_out = 0;
    if (value.IsString()) {
        if (error_code_out) *error_code_out = value.GetString();
        return;
    }
    if (value.IsInt()) {
        const int v = value.GetInt();
        if (error_no_out) *error_no_out = v;
        if (error_code_out && v != 0) *error_code_out = std::to_string(v);
        return;
    }
    if (value.IsUint()) {
        const int v = static_cast<int>(value.GetUint());
        if (error_no_out) *error_no_out = v;
        if (error_code_out && v != 0) *error_code_out = std::to_string(v);
    }
}

}  // namespace task
}  // namespace flowsql
