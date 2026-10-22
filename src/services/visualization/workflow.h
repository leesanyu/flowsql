// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#pragma once
#include <framework/interfaces/idatabase_exploration.h>
#include <rapidjson/document.h>

#include <stdexcept>

namespace flowsql::visualization {
struct FieldV1 {
    std::string id;
    std::string name;
    std::string type;
    std::string native_type;
    bool nullable = true;
};
struct SourceSchemaV1 {
    std::string table;
    std::vector<FieldV1> fields;
    std::string dialect = "sqlite";
    std::string schema;
};
// Aggregate groupBy accepts a field ID or this source-calendar bucket, with a unique output alias.
struct TimeBucketV1 {
    std::string field;
    std::string unit;  // day | month; no timezone conversion
    std::string as_field_key;
};
bool SupportsTimeBucket(const FieldV1& field, const std::string& dialect);
struct NumericInputV1 {
    FieldV1 field;
    std::string aggregate;
    std::string output_id;
};
struct SqlPlanV1 {
    std::string sql;
    std::vector<ExplorationValueV1> parameters;
    std::vector<FieldV1> fields;
    std::string scope = "complete";
    bool aggregate = false;
    std::string filtered_from;
    bool filtered = false;
    std::vector<NumericInputV1> numeric_inputs;
};
struct QueryFailure : std::runtime_error {
    std::string code;
    explicit QueryFailure(std::string error, const std::string& message) : std::runtime_error(message), code(error) {}
};
std::string QuoteIdentifier(const std::string& identifier, const std::string& dialect = "sqlite");
SqlPlanV1 CompileWorkflow(const SourceSchemaV1& source, const rapidjson::Value& payload, const std::string& detail_mode,
                          uint32_t max_rows);
std::vector<DatabaseParameterV1> BindValues(const std::vector<ExplorationValueV1>& values);
}  // namespace flowsql::visualization
