// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <framework/core/sql_parser.h>
#include <cctype>
#include <common/channel_dataset.hpp>
#include <string>
#include <vector>

namespace flowsql::semantics {
struct SemanticProjection {
    std::string source_field;
    std::string operation = "unknown";
    std::string function;
    std::string configuration_json = "{}";
    std::string alias;
    bool all_fields = false;
};
// Parse only proven single-source projections. Empty projections mean no reliable mapping.
inline std::vector<SemanticProjection> QueryProjections(const SqlStatement& statement);
// observation is a captured describe result (data), never a fresh lookup of an old result.
// An empty observation represents absent evidence; read errors remain separate from unknown.
inline dataset::Json ResultContext(const dataset::Value& subject, const dataset::Value& observation,
                                   const std::vector<std::string>& output_names,
                                   const std::vector<SemanticProjection>& projections, const std::string& scope,
                                   bool source_changed = false, const DataSemanticsError* read_error = nullptr);
}  // namespace flowsql::semantics

namespace flowsql::semantics {
namespace detail {
struct Token {
    std::string text;
    bool identifier = false;
    bool quoted = false;
};
inline std::string Lower(std::string value) {
    for (auto& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}
inline std::vector<Token> Tokens(const std::string& sql) {
    std::vector<Token> result;
    size_t i = 0;
    while (i < sql.size()) {
        if (std::isspace(static_cast<unsigned char>(sql[i]))) {
            ++i;
            continue;
        }
        if (sql.compare(i, 2, "--") == 0) {
            while (i < sql.size() && sql[i] != '\n') ++i;
            continue;
        }
        if (sql.compare(i, 2, "/*") == 0) {
            const auto end = sql.find("*/", i + 2);
            if (end == std::string::npos) return {};
            i = end + 2;
            continue;
        }
        Token token;
        char c = sql[i++];
        if (c == '"' || c == '`' || c == '\'') {
            token.quoted = true;
            token.identifier = c != '\'';
            bool closed = false;
            while (i < sql.size()) {
                const char next = sql[i++];
                if (next == c) {
                    if (i == sql.size() || sql[i] != c) {
                        closed = true;
                        break;
                    }
                    ++i;
                }
                token.text += next;
            }
            if (!closed) return {};
        } else {
            token.text += c;
            token.identifier = std::isalpha(static_cast<unsigned char>(c)) || c == '_';
            if (token.identifier) {
                while (i < sql.size() && (std::isalnum(static_cast<unsigned char>(sql[i])) || sql[i] == '_'))
                    token.text += sql[i++];
            }
        }
        result.push_back(std::move(token));
    }
    return result;
}
inline bool Keyword(const Token& token, const char* keyword) {
    return token.identifier && !token.quoted && Lower(token.text) == keyword;
}
inline SemanticProjection Projection(const std::string& text) {
    const auto tokens = Tokens(text);
    SemanticProjection out;
    if (tokens.empty() || !tokens[0].identifier) return out;
    size_t i = 0;
    bool aggregate = tokens.size() > 1 && tokens[1].text == "(" && !tokens[1].quoted;
    bool distinct = false;
    if (aggregate) {
        out.function = Lower(tokens[0].text);
        if (out.function != "count" && out.function != "distinctcount" && out.function != "sum" &&
            out.function != "avg" && out.function != "mean" && out.function != "min" && out.function != "max")
            return {};
        i = 2;
        if (i < tokens.size() && Keyword(tokens[i], "distinct")) {
            distinct = true;
            ++i;
        }
    }
    if (i < tokens.size() && aggregate && out.function == "count" && tokens[i].text == "*" && !tokens[i].quoted) {
        out.source_field = "*";
        ++i;
    } else {
        if (i >= tokens.size() || !tokens[i].identifier) return {};
        out.source_field = tokens[i++].text;
        if (i < tokens.size() && tokens[i].text == "." && !tokens[i].quoted) {
            ++i;
            if (i >= tokens.size() || !tokens[i].identifier) return {};
            out.source_field = tokens[i++].text;
        }
    }
    if (aggregate && (i >= tokens.size() || tokens[i].text != ")" || tokens[i++].quoted)) return {};
    if (i < tokens.size()) {
        if (Keyword(tokens[i], "as")) ++i;
        if (i >= tokens.size() || !tokens[i].identifier) return {};
        out.alias = tokens[i++].text;
    }
    if (i != tokens.size()) return {};
    out.operation = aggregate ? "aggregate" : out.alias.empty() ? "identity" : "alias";
    if (aggregate && (out.function == "count" || out.function == "distinctcount")) {
        dataset::Json configuration(rapidjson::kObjectType);
        auto& a = configuration.GetAllocator();
        dataset::Set(configuration, "count", out.source_field == "*" ? "rows" : "non_null", a);
        configuration.AddMember("distinct", distinct || out.function == "distinctcount", a);
        out.configuration_json = dataset::Encode(configuration);
    }
    return out;
}
}  // namespace detail

inline std::vector<SemanticProjection> QueryProjections(const SqlStatement& statement) {
    if (statement.sources.size() != 1 || statement.HasOperator()) return {};
    size_t selects = 0;
    for (const auto& token : detail::Tokens(statement.sql_part)) {
        if (detail::Keyword(token, "select")) ++selects;
        if (detail::Keyword(token, "join") || detail::Keyword(token, "union") || detail::Keyword(token, "intersect") ||
            detail::Keyword(token, "except"))
            return {};
    }
    if (selects != 1) return {};
    if (statement.columns.empty()) return {{"*", "identity", "", "{}", "", true}};
    std::vector<SemanticProjection> projections;
    for (size_t i = 0; i < statement.columns.size(); ++i) {
        auto projection = detail::Projection(statement.columns[i]);
        if (projection.operation != "unknown" && i < statement.column_aliases.size() &&
            !statement.column_aliases[i].empty()) {
            projection.alias = statement.column_aliases[i];
            if (projection.operation == "identity") projection.operation = "alias";
        }
        projections.push_back(std::move(projection));
    }
    return projections;
}

inline dataset::Json ResultContext(const dataset::Value& subject, const dataset::Value& observation,
                                   const std::vector<std::string>& output_names,
                                   const std::vector<SemanticProjection>& projections, const std::string& scope,
                                   bool source_changed, const DataSemanticsError* read_error) {
    using namespace dataset;
    Json context(rapidjson::kObjectType);
    auto& a = context.GetAllocator();
    context.AddMember("subject", Copy(subject, a), a);
    Set(context, "observed_at", std::to_string(Now()), a);
    Set(context, "scope", scope, a);
    Value sources(rapidjson::kArrayType), fields(rapidjson::kArrayType);
    const Value* structure = nullptr;
    const Value* definitions = nullptr;
    if (observation.IsObject()) {
        Value source(rapidjson::kObjectType);
        source.AddMember("target", Copy(observation["subject"], a), a);
        for (const char* key : {"basis", "description", "context", "structure"})
            source.AddMember(Value(key, a), Copy(observation[key], a), a);
        sources.PushBack(source, a);
        if (observation["structure"].HasMember("fields") && observation["structure"]["fields"].IsArray())
            structure = &observation["structure"]["fields"];
        const auto& document = observation["description"]["document"];
        if (document.IsObject() && document.HasMember("fields")) definitions = &document["fields"];
        source_changed = source_changed || Get(observation["description"], "state") == "needs_review";
    }
    const bool star = projections.size() == 1 && projections[0].all_fields;
    const bool mismatch =
        structure && !projections.empty() && (star ? structure->Size() : projections.size()) != output_names.size();
    size_t known_count = 0, conflict_count = 0;
    for (size_t i = 0; i < output_names.size(); ++i) {
        SemanticProjection projection;
        if (star && structure && i < structure->Size())
            projection = {Get((*structure)[i], "name"), "identity"};
        else if (!star && i < projections.size())
            projection = projections[i];
        const Value* definition = nullptr;
        bool located = false;
        if (structure && projection.operation != "unknown") {
            for (const auto& field : structure->GetArray())
                located = located || Get(field, "name") == projection.source_field;
            if (projection.source_field == "*" && projection.operation == "aggregate") located = true;
        }
        if (located && definitions)
            for (const auto& field : definitions->GetArray())
                if ((observation["context"].HasMember("binding") && observation["context"]["binding"].IsObject() &&
                             observation["context"]["binding"].HasMember("field_map")
                         ? Get(observation["context"]["binding"]["field_map"], Get(field, "field_id").c_str())
                         : Get(field, "name")) == projection.source_field) {
                    definition = &field;
                    break;
                }
        const bool known = definition && definition->HasMember("definition") &&
                           Get((*definition)["definition"], "state") == "declared";
        const bool conflict =
            source_changed || mismatch || (star && located && projection.source_field != output_names[i]);
        const std::string state = read_error ? "unknown" : conflict ? "conflict" : known ? "known" : "unknown";
        known_count += state == "known";
        conflict_count += state == "conflict";
        Value field(rapidjson::kObjectType), operation(rapidjson::kObjectType);
        field.AddMember("output_index", static_cast<unsigned>(i), a);
        Set(field, "output_name", output_names[i], a);
        Set(field, "state", state, a);
        Set(field, "reason",
            read_error ? "description_error"
            : conflict ? "structure_changed"
            : known    ? ""
            : located  ? "undeclared"
                       : "unsupported_expression_or_missing_basis",
            a);
        field.AddMember("source_index", located && !conflict ? Value(0) : Value(), a);
        field.AddMember("source_field_id", definition && !conflict ? Copy((*definition)["field_id"], a) : Value(), a);
        Set(operation, "kind", projection.operation, a);
        Set(operation, "function", projection.function, a);
        Json config;
        config.Parse(projection.configuration_json.c_str());
        operation.AddMember("configuration", Copy(config, a), a);
        field.AddMember("operation", operation, a);
        Value unit;
        if (projection.function == "count" || projection.function == "distinctcount")
            unit.SetString("个", a);
        else if (!conflict && !read_error && (projection.operation == "identity" || projection.operation == "alias") &&
                 definition && definition->HasMember("unit") && Get((*definition)["unit"], "state") == "declared")
            unit.SetString(Get((*definition)["unit"]["value"], "code").c_str(), a);
        field.AddMember("result_unit", unit, a);
        fields.PushBack(field, a);
    }
    Set(context, "state",
        read_error                                          ? "error"
        : conflict_count                                    ? "partial"
        : known_count == output_names.size() && known_count ? "available"
        : known_count                                       ? "partial"
                                                            : "unknown",
        a);
    context.AddMember("sources", sources, a).AddMember("fields", fields, a);
    if (read_error) {
        Value error(rapidjson::kObjectType);
        Set(error, "code", read_error->code, a);
        Set(error, "message", read_error->message, a);
        Set(error, "phase", "description", a);
        context.AddMember("error", error, a);
    }
    return context;
}
}  // namespace flowsql::semantics
