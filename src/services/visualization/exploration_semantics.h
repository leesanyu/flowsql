// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <httplib.h>
#include <algorithm>
#include <cassert>
#include <common/semantic_capture.hpp>

#include "workflow.h"

namespace flowsql::visualization {
// Input is the workflow already accepted by the executing compiler. Field IDs remain server-owned.
inline std::vector<semantics::SemanticProjection> WorkflowProjections(const std::vector<Field>& fields,
                                                                      const rapidjson::Value& payload,
                                                                      const std::string& dialect) {
    using namespace dataset;
    std::vector<semantics::SemanticProjection> out;
    const auto name = [&](const std::string& id) {
        if (id == "*" || id == "gw_count_fid") return std::string("*");
        const auto found = std::find_if(fields.begin(), fields.end(), [&](const auto& f) { return f.id == id; });
        assert(found != fields.end());
        return found->name;
    };
    for (const auto& step : payload["workflow"].GetArray()) {
        if (Get(step, "type") != "view") continue;
        const auto& query = step["query"][0];
        const auto op = Get(query, "op");
        if (op == "raw") {
            for (const auto& field : query["fields"].GetArray()) out.push_back({name(field.GetString()), "identity"});
        } else if (op == "histogram") {
            for (const auto* role : {"bin_index", "bin_lower", "bin_upper", "bin_count"}) {
                Json config(rapidjson::kObjectType);
                auto& a = config.GetAllocator();
                config.AddMember("bins", Copy(query["bins"], a), a);
                Set(config, "column", role, a);
                Set(config, "scope", "non_null_finite_values; equal_width", a);
                out.push_back({name(Get(query, "field")), "histogram", std::string(role) == "bin_count" ? "count" : "",
                               Encode(config)});
            }
        } else if (op == "aggregate") {
            for (const auto& field : query["groupBy"].GetArray()) {
                if (field.IsString()) {
                    out.push_back({name(field.GetString()), "identity"});
                } else {
                    Json config;
                    config.CopyFrom(field, config.GetAllocator());
                    Set(config, "calendar", dialect == "dataframe" ? "snapshot_temporal_values" : "source_calendar",
                        config.GetAllocator());
                    out.push_back({name(Get(field, "field")), "time_bucket", "", Encode(config)});
                }
            }
            for (const auto& measure : query["measures"].GetArray()) {
                auto function = Get(measure, "agg");
                if (function == "distinctCount") function = "distinctcount";
                Json config(rapidjson::kObjectType);
                auto& a = config.GetAllocator();
                if (function == "count" || function == "distinctcount") {
                    Set(config, "count", name(Get(measure, "field")) == "*" ? "rows" : "non_null", a);
                    config.AddMember("distinct", function == "distinctcount", a);
                }
                out.push_back({name(Get(measure, "field")), "aggregate", function, Encode(config)});
            }
        }
    }
    return out;
}
inline std::vector<std::string> SemanticNames(const std::vector<Field>& fields) {
    std::vector<std::string> names;
    for (const auto& field : fields) names.push_back(field.id);
    return names;
}
inline std::vector<semantics::SemanticProjection> IdentityProjections(const std::vector<Field>& fields) {
    std::vector<semantics::SemanticProjection> projections;
    for (const auto& field : fields) projections.push_back({field.name, "identity"});
    return projections;
}
inline dataset::Json SemanticSubject(const std::string& source_id, const std::string& revision,
                                     const std::string& snapshot_id = {}) {
    dataset::Json subject(rapidjson::kObjectType);
    auto& a = subject.GetAllocator();
    dataset::Set(subject, "source_id", source_id, a);
    dataset::Set(subject, "schema_revision", revision, a);
    if (!snapshot_id.empty()) dataset::Set(subject, "snapshot_id", snapshot_id, a);
    return subject;
}
inline semantics::Capture::Read HttpDescription(const std::string& upstream, uint32_t timeout_ms) {
    return [upstream, timeout_ms](const std::string& request, std::string* response) {
        *response = R"({"error":{"code":"SOURCE_UNAVAILABLE","message":"configured description service unavailable"}})";
        if (upstream.empty()) return -1;
        httplib::Client client(upstream);
        const auto budget = std::chrono::milliseconds(std::min(timeout_ms, 5000U));
        client.set_connection_timeout(budget);
        client.set_read_timeout(budget);
        client.set_write_timeout(budget);
        const auto result = client.Post("/semantics/describe", request, "application/json");
        if (!result) return -1;
        if (result->status == 200) {
            if (result->body.size() > 12 * 1024 * 1024 || !flowsql::JsonNestingWithin(result->body, 64)) {
                *response = R"({"error":{"code":"LIMIT_EXCEEDED","message":"description exceeds HTTP budget"}})";
                return -1;
            }
            dataset::Json wire;
            wire.Parse(result->body.c_str());
            bool valid = wire.IsObject() && wire.HasMember("data") && wire["data"].IsObject();
            if (valid) {
                const auto& data = wire["data"];
                for (const auto* key : {"subject", "basis", "description", "context", "structure"})
                    valid = valid && data.HasMember(key) && data[key].IsObject();
                if (valid) {
                    for (const auto* key : {"object_key", "source_revision", "schema_fingerprint"})
                        valid = valid && data["basis"].HasMember(key);
                    valid = valid && data["description"].HasMember("document") &&
                            data["structure"].HasMember("fields") &&
                            (data["structure"]["fields"].IsNull() || data["structure"]["fields"].IsArray());
                }
            }
            if (valid) {
                const auto& data = wire["data"];
                dataset::Json requested;
                requested.Parse(request.c_str());
                valid = data["subject"] == requested["target"];
                const auto& fields = data["structure"]["fields"];
                if (fields.IsArray())
                    for (const auto& field : fields.GetArray())
                        valid = valid && field.IsObject() && field.HasMember("name") && field["name"].IsString();
                const auto& document = data["description"]["document"];
                valid = valid && (document.IsNull() || document.IsObject());
                if (document.IsObject() && document.HasMember("fields")) {
                    valid = valid && document["fields"].IsArray();
                    if (document["fields"].IsArray())
                        for (const auto& field : document["fields"].GetArray()) {
                            valid = valid && field.IsObject() && field.HasMember("field_id") &&
                                    field["field_id"].IsString();
                            if (field.IsObject() && field.HasMember("unit") &&
                                dataset::Get(field["unit"], "state") == "declared")
                                valid = valid && field["unit"].HasMember("value") &&
                                        field["unit"]["value"].IsObject() && field["unit"]["value"].HasMember("code") &&
                                        field["unit"]["value"]["code"].IsString();
                        }
                }
                const auto& context = data["context"];
                if (context.HasMember("binding") && !context["binding"].IsNull()) {
                    valid = valid && context["binding"].IsObject();
                    if (context["binding"].IsObject() && context["binding"].HasMember("field_map")) {
                        const auto& mapping = context["binding"]["field_map"];
                        valid = valid && mapping.IsObject();
                        if (mapping.IsObject())
                            for (const auto& field : mapping.GetObject()) valid = valid && field.value.IsString();
                    }
                }
            }
            if (!valid) {
                *response = R"({"error":{"code":"INVALID_DOCUMENT","message":"invalid description response"}})";
                return -1;
            }
        }
        *response = result->body;
        return result->status == 200 ? 0 : -1;
    };
}
inline dataset::Json SnapshotResultContext(const dataset::Value& snapshot, const dataset::Value& subject,
                                           const std::vector<Field>& fields,
                                           const std::vector<semantics::SemanticProjection>& projections,
                                           const std::string& scope) {
    using namespace dataset;
    Json observation;
    if (!snapshot["sources"].Empty()) {
        observation.CopyFrom(snapshot["sources"][0], observation.GetAllocator());
        observation.AddMember("subject", Copy(observation["target"], observation.GetAllocator()),
                              observation.GetAllocator());
        observation.RemoveMember("target");
    }
    DataSemanticsError error;
    if (snapshot.HasMember("error")) error = {Get(snapshot["error"], "code"), Get(snapshot["error"], "message"), ""};
    bool conflict = false;
    for (const auto& field : snapshot["fields"].GetArray()) conflict = conflict || Get(field, "state") == "conflict";
    auto context = semantics::ResultContext(subject, observation, SemanticNames(fields), projections, scope, conflict,
                                            error.code.empty() ? nullptr : &error);
    Set(context, "observed_at", Get(snapshot, "observed_at"), context.GetAllocator());
    return context;
}
// Reduce only metadata when its projection exceeds the remaining response or snapshot budget.
inline dataset::Json BudgetContext(dataset::Json context, uint64_t available) {
    if (dataset::Encode(context).size() <= available) return context;
    const DataSemanticsError error{"LIMIT_EXCEEDED", "description exceeds result or snapshot budget", ""};
    std::vector<std::string> names;
    for (const auto& field : context["fields"].GetArray()) names.push_back(dataset::Get(field, "output_name"));
    return semantics::ResultContext(context["subject"], dataset::Value(), names, {}, "metadata_budget", false, &error);
}
inline void AppendContext(std::string* body, dataset::Json context, uint64_t max_bytes) {
    constexpr uint64_t reserved = 1024;
    context = BudgetContext(std::move(context),
                            max_bytes > body->size() + reserved ? max_bytes - body->size() - reserved : 0);
    body->pop_back();
    *body += ",\"semantic_context\":" + dataset::Encode(context) + "}";
    if (body->size() > max_bytes) throw QueryFailure("RESULT_LIMIT", "结果与说明错误信封超过字节上限");
}
}  // namespace flowsql::visualization
