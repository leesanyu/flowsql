// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#pragma once
#include <framework/interfaces/idatabase_exploration.h>
#include <rapidjson/document.h>

#include <stdexcept>

namespace flowsql::visualization {
struct Field {
    std::string id;
    std::string name;
    std::string type;
    std::string native_type;
    bool nullable = true;
};
struct SourceSchema {
    std::string table;
    std::vector<Field> fields;
    std::string dialect = "sqlite";
    std::string schema;
};
// Aggregate groupBy accepts a field ID or this source-calendar bucket, with a unique output alias.
struct TimeBucket {
    std::string field;
    std::string unit;  // day | month; no timezone conversion
    std::string as_field_key;
};
bool SupportsTimeBucket(const Field& field, const std::string& dialect);
struct NumericInput {
    Field field;
    std::string aggregate;
    std::string output_id;
};
struct SqlPlan {
    std::string sql;
    std::vector<ExplorationValue> parameters;
    std::vector<Field> fields;
    std::string scope = "complete";
    bool aggregate = false;
    std::string filtered_from;
    bool filtered = false;
    std::vector<NumericInput> numeric_inputs;
};
struct QueryFailure : std::runtime_error {
    std::string code;
    explicit QueryFailure(std::string error, const std::string& message) : std::runtime_error(message), code(error) {}
};
std::string QuoteIdentifier(const std::string& identifier, const std::string& dialect = "sqlite");
SqlPlan CompileWorkflow(const SourceSchema& source, const rapidjson::Value& payload, const std::string& detail_mode,
                        uint32_t max_rows);
std::vector<DatabaseParameter> BindValues(const std::vector<ExplorationValue>& values);
}  // namespace flowsql::visualization
