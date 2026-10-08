// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "config_parser.h"

#include <common/error_code.h>

#include <rapidjson/document.h>

#include <utility>

#include "model/profile_config.h"

namespace flowsql {
namespace baseline {

namespace {

int ParseObject(const char* config_json, rapidjson::Document* doc, std::string* err) {
    if (!config_json || !doc) {
        if (err) *err = "config_content must not be null";
        return error::BAD_REQUEST;
    }
    doc->Parse(config_json);
    if (doc->HasParseError() || !doc->IsObject()) {
        if (err) *err = "config_content must be a valid json object";
        return error::BAD_REQUEST;
    }
    return error::OK;
}

int RequireString(const rapidjson::Value& obj,
                  const char* name,
                  std::string* out,
                  std::string* err) {
    if (!out) return error::BAD_REQUEST;
    if (!obj.HasMember(name) || !obj[name].IsString()) {
        if (err) *err = std::string("missing string field: ") + name;
        return error::BAD_REQUEST;
    }
    *out = obj[name].GetString();
    if (out->empty()) {
        if (err) *err = std::string("field must not be empty: ") + name;
        return error::BAD_REQUEST;
    }
    return error::OK;
}

int RequirePositiveInt64(const rapidjson::Value& obj,
                         const char* name,
                         int64_t* out,
                         std::string* err) {
    if (!out) return error::BAD_REQUEST;
    if (!obj.HasMember(name) || !obj[name].IsInt64()) {
        if (err) *err = std::string("missing int64 field: ") + name;
        return error::BAD_REQUEST;
    }
    const int64_t value = obj[name].GetInt64();
    if (value <= 0) {
        if (err) *err = std::string("field must be > 0: ") + name;
        return error::BAD_REQUEST;
    }
    *out = value;
    return error::OK;
}

int RequirePositiveInt32(const rapidjson::Value& obj,
                         const char* name,
                         int32_t* out,
                         std::string* err) {
    if (!out) return error::BAD_REQUEST;
    if (!obj.HasMember(name) || !obj[name].IsInt()) {
        if (err) *err = std::string("missing int field: ") + name;
        return error::BAD_REQUEST;
    }
    const int value = obj[name].GetInt();
    if (value <= 0) {
        if (err) *err = std::string("field must be > 0: ") + name;
        return error::BAD_REQUEST;
    }
    *out = value;
    return error::OK;
}

int RequireProbability(const rapidjson::Value& obj,
                       const char* name,
                       double* out,
                       std::string* err) {
    if (!out) return error::BAD_REQUEST;
    if (!obj.HasMember(name) || !obj[name].IsNumber()) {
        if (err) *err = std::string("missing number field: ") + name;
        return error::BAD_REQUEST;
    }
    const double value = obj[name].GetDouble();
    if (!(value > 0.0 && value <= 1.0)) {
        if (err) *err = std::string("field must be in (0,1]: ") + name;
        return error::BAD_REQUEST;
    }
    *out = value;
    return error::OK;
}

int RequireSchemaVersion(const rapidjson::Value& obj, std::string* err) {
    if (!obj.HasMember("schema_version") || !obj["schema_version"].IsInt()) {
        if (err) *err = "missing int field: schema_version";
        return error::BAD_REQUEST;
    }
    if (obj["schema_version"].GetInt() != 1) {
        if (err) *err = "unsupported schema_version";
        return error::BAD_REQUEST;
    }
    return error::OK;
}

int ParseClockSpec(const rapidjson::Value& obj,
                   BaselineClockSpec* out,
                   std::string* err) {
    if (!out) return error::BAD_REQUEST;
    if (!obj.HasMember("clock_spec") || !obj["clock_spec"].IsObject()) {
        if (err) *err = "missing object field: clock_spec";
        return error::BAD_REQUEST;
    }
    const auto& clock = obj["clock_spec"];
    int rc = RequirePositiveInt64(clock, "bucket_seconds", &out->bucket_seconds, err);
    if (rc != error::OK) return rc;
    return RequireString(clock, "timezone", &out->timezone, err);
}

int ParseCalendarRef(const rapidjson::Value& obj,
                     BaselineCalendarRef* out,
                     std::string* err) {
    if (!out) return error::BAD_REQUEST;
    if (!obj.HasMember("calendar_ref") || !obj["calendar_ref"].IsObject()) {
        if (err) *err = "missing object field: calendar_ref";
        return error::BAD_REQUEST;
    }
    const auto& calendar = obj["calendar_ref"];
    int rc = RequireString(calendar, "calendar_id", &out->calendar_id, err);
    if (rc != error::OK) return rc;
    return RequireString(calendar, "calendar_version", &out->calendar_version, err);
}

int ParseCommonTask(const char* config_json,
                    const char* expected_kind,
                    BaselineTaskSpec* out,
                    std::string* err) {
    if (!out) return error::BAD_REQUEST;

    rapidjson::Document doc;
    int rc = ParseObject(config_json, &doc, err);
    if (rc != error::OK) return rc;
    if ((rc = RequireSchemaVersion(doc, err)) != error::OK) return rc;

    BaselineTaskSpec spec;
    if ((rc = RequireString(doc, "task_id", &spec.task_id, err)) != error::OK) return rc;
    if ((rc = RequireString(doc, "task_name", &spec.name, err)) != error::OK) return rc;
    if ((rc = RequireString(doc, "task_kind", &spec.task_kind, err)) != error::OK) return rc;
    if ((rc = RequireString(doc, "feature_id", &spec.feature_id, err)) != error::OK) return rc;
    if ((rc = RequireString(doc, "feature_type", &spec.feature_type, err)) != error::OK) {
        return rc;
    }
    if ((rc = RequireString(doc, "profile", &spec.profile, err)) != error::OK) return rc;
    if ((rc = ParseClockSpec(doc, &spec.clock_spec, err)) != error::OK) return rc;
    if ((rc = ParseCalendarRef(doc, &spec.calendar_ref, err)) != error::OK) return rc;

    const std::string expected = expected_kind ? expected_kind : "";
    if (spec.task_kind != expected) {
        if (err) *err = "task_kind must match requested task kind";
        return error::BAD_REQUEST;
    }
    if (expected == "value") {
        if (spec.feature_type == "value_basic") {
            if (spec.profile != "default") {
                if (err) *err = "value_basic requires profile=default";
                return error::BAD_REQUEST;
            }
        } else if (spec.feature_type == "value_sampled") {
            ValueSampledProfileConfig sampled_profile;
            if (spec.profile == "default" ||
                !GetValueSampledProfileConfig(spec.profile, &sampled_profile)) {
                if (err) *err = "value_sampled requires a value_sampled_profiles profile";
                return error::BAD_REQUEST;
            }
            spec.value_identity_transform = sampled_profile.transform_name_override == "identity";
        } else {
            if (err) *err = "value task feature_type must be value_basic or value_sampled";
            return error::BAD_REQUEST;
        }
    } else if (expected == "ratio") {
        RatioProfileConfig ratio_profile;
        if (spec.feature_type != "ratio" ||
            !GetRatioProfileConfig(spec.profile, &ratio_profile)) {
            if (err) *err = "ratio task feature_type/profile is invalid";
            return error::BAD_REQUEST;
        }
    } else if (expected == "relation") {
        if (spec.feature_type != "relation" || spec.profile != "default") {
            if (err) *err = "relation task requires feature_type=relation and profile=default";
            return error::BAD_REQUEST;
        }
    } else {
        if (err) *err = "unsupported task kind";
        return error::BAD_REQUEST;
    }

    spec.feature = spec.feature_id;
    spec.delta = spec.clock_spec.bucket_seconds;
    spec.tz = spec.clock_spec.timezone;
    spec.config_json = config_json;

    *out = std::move(spec);
    return error::OK;
}

int ParseStringArray(const rapidjson::Value& obj,
                     const char* name,
                     std::vector<std::string>* out,
                     std::string* err) {
    if (!out) return error::BAD_REQUEST;
    if (!obj.HasMember(name) || !obj[name].IsArray()) {
        if (err) *err = std::string("missing string array field: ") + name;
        return error::BAD_REQUEST;
    }
    out->clear();
    for (const auto& item : obj[name].GetArray()) {
        if (!item.IsString() || std::string(item.GetString()).empty()) {
            if (err) *err = std::string("invalid string array item: ") + name;
            return error::BAD_REQUEST;
        }
        out->push_back(item.GetString());
    }
    if (out->empty()) {
        if (err) *err = std::string("field must not be empty: ") + name;
        return error::BAD_REQUEST;
    }
    return error::OK;
}

int ParseRelationPolicies(const rapidjson::Value& doc,
                          RelationSupportPolicySpec* support,
                          RelationSummaryPolicySpec* summary,
                          std::string* err) {
    if (!support || !summary) return error::BAD_REQUEST;
    if (!doc.HasMember("support_policy") || !doc["support_policy"].IsObject()) {
        if (err) *err = "missing object field: support_policy";
        return error::BAD_REQUEST;
    }
    if (!doc.HasMember("summary_policy") || !doc["summary_policy"].IsObject()) {
        if (err) *err = "missing object field: summary_policy";
        return error::BAD_REQUEST;
    }
    int rc = RequirePositiveInt32(doc["support_policy"], "k_support", &support->k_support, err);
    if (rc != error::OK) return rc;
    rc = RequireProbability(
        doc["support_policy"], "min_hist_share", &support->min_hist_share, err);
    if (rc != error::OK) return rc;
    rc = RequireProbability(
        doc["support_policy"], "min_active_ratio", &support->min_active_ratio, err);
    if (rc != error::OK) return rc;
    rc = RequirePositiveInt32(doc["summary_policy"], "k_head", &summary->k_head, err);
    if (rc != error::OK) return rc;
    return RequirePositiveInt32(doc["summary_policy"], "k_stable", &summary->k_stable, err);
}

}  // namespace

int ConfigParser::ParseValueTask(const char* config_json,
                                 BaselineTaskSpec* out,
                                 std::string* err) {
    return ParseCommonTask(config_json, "value", out, err);
}

int ConfigParser::ParseRatioTask(const char* config_json,
                                 BaselineTaskSpec* out,
                                 std::string* err) {
    return ParseCommonTask(config_json, "ratio", out, err);
}

int ConfigParser::ParseRelationTask(const char* config_json,
                                    RelationTaskCreateSpec* out,
                                    std::string* err) {
    if (!out) return error::BAD_REQUEST;

    rapidjson::Document doc;
    int rc = ParseObject(config_json, &doc, err);
    if (rc != error::OK) return rc;

    BaselineTaskSpec common;
    rc = ParseCommonTask(config_json, "relation", &common, err);
    if (rc != error::OK) return rc;

    RelationTaskCreateSpec create_spec;
    create_spec.task_spec.task_id = common.task_id;
    create_spec.task_spec.name = common.name;
    create_spec.task_spec.task_kind = common.task_kind;
    create_spec.task_spec.feature_id = common.feature_id;
    create_spec.task_spec.profile = common.profile;
    create_spec.task_spec.calendar_ref = common.calendar_ref;
    create_spec.task_spec.feature_base = common.feature_id;
    if (doc.HasMember("feature_base") && doc["feature_base"].IsString()) {
        create_spec.task_spec.feature_base = doc["feature_base"].GetString();
    }
    rc = RequireString(doc, "group_space_id", &create_spec.task_spec.group_space_id, err);
    if (rc != error::OK) return rc;
    if (doc.HasMember("group_space_version") && doc["group_space_version"].IsString()) {
        create_spec.task_spec.group_space_version = doc["group_space_version"].GetString();
    }
    rc = ParseStringArray(doc, "metrics", &create_spec.task_spec.metrics, err);
    if (rc != error::OK) return rc;
    rc = ParseRelationPolicies(
        doc,
        &create_spec.task_spec.support_policy,
        &create_spec.task_spec.summary_policy,
        err);
    if (rc != error::OK) return rc;
    if (create_spec.task_spec.support_policy.k_support <
        create_spec.task_spec.summary_policy.k_stable) {
        if (err) *err = "support_policy.k_support must be >= summary_policy.k_stable";
        return error::BAD_REQUEST;
    }
    create_spec.task_spec.config_json = common.config_json;
    create_spec.clock_spec.delta = common.clock_spec.bucket_seconds;
    create_spec.clock_spec.tz = common.clock_spec.timezone;

    *out = std::move(create_spec);
    return error::OK;
}

}  // namespace baseline
}  // namespace flowsql
