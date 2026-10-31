// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <algorithm>
#include <cctype>
#include <common/channel_dataset.hpp>
#include <regex>
#include <set>

namespace flowsql::channels::config::description_time {
using namespace dataset;
inline Value Semantic(Value value, const char* origin, Alloc& a) {
    Value result(rapidjson::kObjectType);
    Set(result, "state", value.IsNull() ? "unknown" : "declared", a);
    Set(result, "origin", origin, a);
    Set(result, "value", std::move(value), a);
    return result;
}
inline Value TextValue(const std::string& text, const char* origin, Alloc& a) {
    return Semantic(text.empty() ? Value() : Value(text.c_str(), a), origin, a);
}
inline Value Unit(const std::string& code, const char* origin, Alloc& a) {
    Value value;
    if (!code.empty()) {
        value.SetObject();
        Set(value, "code", code, a);
        Set(value, "label", code, a);
    }
    return Semantic(std::move(value), origin, a);
}
inline bool Known(const Value& v) { return v.IsObject() && Get(v, "state") == "declared"; }
inline std::string Code(const Value& v) {
    return Known(v) && v.HasMember("value") && v["value"].IsObject() ? Get(v["value"], "code") : "";
}
inline Json Facts(const Value& physical, const Value& source, const std::string& name) {
    static const std::regex pattern_0(
        R"((u?int(8|16|32|64)?|integer|bigint|smallint|tinyint|mediumint|float(16|32|64)?|half_float|double( precision)?|real|decimal(128|256)?(\(.*\))?|numeric(\(.*\))?)( unsigned)?)");
    static const std::regex pattern_1(R"(timestamp\[(s|ms|us|ns)(, tz=(.+))?\])");
    static const std::regex pattern_2(R"(duration\[(s|ms|us|ns)\])");
    static const std::regex pattern_3(
        R"((date|date32|datetime|datetime64|timestamp|timestamp with time zone|timestamp without time zone)(\([0-9]+\))?|(timestamp\([0-9]+\))( with(out)? time zone)?|datetime64\([0-9]+, *'[^']+'\))");
    static const std::regex pattern_4(R"(\(([0369])(, *'([^']+)')?\))");
    Json facts(rapidjson::kObjectType);
    auto& a = facts.GetAllocator();
    Value modes(rapidjson::kArrayType), time, unit;
    const auto& type = physical["type"];
    const auto system = type.IsObject() ? Get(type, "system") : "";
    const auto native = type.IsObject() ? Get(type, "name") : "";
    std::string lower = native;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
    const bool supported = system == "arrow" || system == "database";
    const bool number = supported && std::regex_match(lower, pattern_0);
    std::smatch match;
    const bool timestamp = system == "arrow" && std::regex_match(native, match, pattern_1);
    std::string code, zone;
    if (timestamp) {
        code = match[1];
        zone = match[3];
    }
    const bool duration = system == "arrow" && std::regex_match(native, match, pattern_2);
    if (duration) code = match[1];
    const bool date = supported && (timestamp || lower == "date32[day]" || lower == "date64[ms]" ||
                                    (system == "database" && std::regex_match(lower, pattern_3)));
    if (system == "database" && date) {
        if (std::regex_search(native, match, pattern_4)) {
            const std::string precision = match[1];
            code = precision == "0" ? "s" : precision == "3" ? "ms" : precision == "6" ? "us" : "ns";
            zone = match[3];
        }
    }
    if (number) modes.PushBack("unix_timestamp", a);
    if (date) modes.PushBack("datetime", a);
    if (number || duration) modes.PushBack("duration", a);
    if (date || duration) {
        Value value(rapidjson::kObjectType);
        Set(value, "kind", duration ? "duration" : "instant", a);
        Set(value, "unit", Unit(code, "derived", a), a);
        Set(value, "epoch", TextValue(timestamp ? "1970-01-01T00:00:00Z" : "", "derived", a), a);
        Set(value, "timezone", TextValue(zone, "derived", a), a);
        Set(value, "description", TextValue("", "derived", a), a);
        time = Semantic(std::move(value), "derived", a);
        if (!code.empty()) unit = Unit(code, "derived", a);
    }
    const auto& metadata = source["producer_metadata"];
    if (metadata.HasMember("fields") && metadata["fields"].IsObject() && metadata["fields"].HasMember(name.c_str())) {
        const auto& producer = metadata["fields"][name.c_str()];
        if (producer.HasMember("unit")) unit = Semantic(Copy(producer["unit"], a), "producer", a);
        if (producer.HasMember("time_semantics")) {
            auto value = Copy(producer["time_semantics"], a);
            for (const char* k : {"unit", "epoch", "timezone", "description"}) {
                if (!value.HasMember(k))
                    Set(value, k, Semantic(Value(), "producer", a), a);
                else if (value[k].IsObject() && !value[k].HasMember("origin"))
                    Set(value[k], "origin", "producer", a);
            }
            if (!Known(value["unit"]) && Known(unit)) Set(value, "unit", Copy(unit, a), a);
            time = Semantic(std::move(value), "producer", a);
        }
    }
    Set(facts, "modes", std::move(modes), a);
    Set(facts, "unit", std::move(unit), a);
    Set(facts, "time_semantics", std::move(time), a);
    return facts;
}
inline bool Conflict(const Value& field, const Value& facts) {
    for (const char* k : {"unit", "time_semantics"}) {
        if (!field.HasMember(k) || !Known(field[k]) || !Known(facts[k])) continue;
        if (std::string(k) == "unit") {
            if (Code(field[k]) != Code(facts[k])) return true;
            continue;
        }
        const auto& old = field[k]["value"];
        const auto& fresh = facts[k]["value"];
        if (Get(old, "kind") != Get(fresh, "kind")) return true;
        for (const char* part : {"unit", "epoch", "timezone"})
            if (old.HasMember(part) && fresh.HasMember(part) && Known(old[part]) && Known(fresh[part]) &&
                old[part]["value"] != fresh[part]["value"])
                return true;
    }
    return false;
}
inline int Apply(Value& field, const Value& facts, const Value* edit, bool first, bool* touched, Alloc& a,
                 DataSemanticsErrorV1* e) {
    *touched = edit && edit->HasMember("time");
    const auto path = "/editor_changes/fields/" + Get(field, "name");
    if (*touched) {
        const auto& input = (*edit)["time"];
        if (input.IsNull())
            Set(field, "time_semantics", Semantic(Value(), "manual", a), a);
        else {
            const auto mode = Get(input, "mode");
            bool allowed = false;
            for (const auto& m : facts["modes"].GetArray()) allowed |= Text(m) == mode;
            if (!allowed)
                return Fail(e, "INVALID_DOCUMENT", "time mode does not apply to the actual source type",
                            path + "/time/mode");
            Value value(rapidjson::kObjectType);
            Set(value, "kind", mode == "duration" ? "duration" : "instant", a);
            Value next_unit = mode == "datetime" && Known(facts["unit"]) ? Copy(facts["unit"], a)
                              : mode == "datetime"                       ? Unit("", "derived", a)
                                                                         : Unit(Get(input, "unit"), "manual", a);
            if (edit->HasMember("unit") && Code(field["unit"]) != Code(next_unit))
                return Fail(e, "INVALID_DOCUMENT", "field unit contradicts time unit", path + "/unit");
            if (Known(next_unit)) {
                if (field.HasMember("unit") && Code(field["unit"]) == Code(next_unit))
                    next_unit = Copy(field["unit"], a);
                Set(field, "unit", Copy(next_unit, a), a);
            }
            Set(value, "unit", std::move(next_unit), a);
            Set(value, "epoch",
                mode == "unix_timestamp" ? TextValue("1970-01-01T00:00:00Z", "manual", a)
                : mode == "datetime" && Known(facts["time_semantics"])
                    ? Copy(facts["time_semantics"]["value"]["epoch"], a)
                    : TextValue("", "manual", a),
                a);
            Set(value, "timezone",
                input.HasMember("timezone") ? TextValue(Get(input, "timezone"), "manual", a)
                : mode == "datetime" && Known(facts["time_semantics"])
                    ? Copy(facts["time_semantics"]["value"]["timezone"], a)
                    : TextValue("", "manual", a),
                a);
            Set(value, "description",
                field.HasMember("time_semantics") && Known(field["time_semantics"]) &&
                        field["time_semantics"]["value"].HasMember("description")
                    ? Copy(field["time_semantics"]["value"]["description"], a)
                    : TextValue("", "manual", a),
                a);
            Set(field, "time_semantics", Semantic(std::move(value), "manual", a), a);
        }
    }
    // Only the new editor branch fills missing facts; known manual/imported values remain intact.
    if (first || edit) {
        for (const char* k : {"unit", "time_semantics"})
            if (Known(facts[k]) && (!field.HasMember(k) || Get(field[k], "state") == "unknown" ||
                                    (first && Get(field[k], "origin") == "producer"))) {
                Set(field, k, Copy(facts[k], a), a);
                if (std::string(k) == "time_semantics") *touched = true;
            }
    }
    if (edit && (edit->HasMember("unit") || edit->HasMember("time")) && field.HasMember("time_semantics") &&
        Known(field["time_semantics"]) && field["time_semantics"]["value"].HasMember("unit")) {
        const auto& time_unit = field["time_semantics"]["value"]["unit"];
        if (Known(time_unit) && Code(field["unit"]) != Code(time_unit))
            return Fail(e, "INVALID_DOCUMENT", "field unit contradicts effective time unit", path + "/unit");
    }
    return 0;
}
inline void Summary(Value& entity, const Value& fields, const std::set<std::string>& touched, Alloc& a) {
    if (touched.empty()) return;
    auto& summary = entity["time_fields"];
    Value ids(rapidjson::kArrayType);
    if (Known(summary))
        for (const auto& id : summary["value"].GetArray())
            if (!touched.count(Text(id))) ids.PushBack(Copy(id, a), a);
    for (const auto& f : fields.GetArray())
        if (touched.count(Get(f, "field_id")) && Known(f["time_semantics"]) &&
            Get(f["time_semantics"]["value"], "kind") == "instant")
            ids.PushBack(Copy(f["field_id"], a), a);
    const auto origin = Known(summary) ? Get(summary, "origin", "derived") : "derived";
    summary = Semantic(ids.Empty() ? Value() : std::move(ids), origin.c_str(), a);
}
}  // namespace flowsql::channels::config::description_time
