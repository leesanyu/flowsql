// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <common/channel_dataset.hpp>

namespace flowsql::npm::result_dictionary {
using namespace dataset;
inline Value Declared(Value value, Alloc& a) {
    Value out(rapidjson::kObjectType);
    Set(out, "state", "declared", a);
    Set(out, "origin", "producer", a);
    out.AddMember("value", value, a);
    return out;
}
inline Value Declared(const std::string& text, Alloc& a) { return Declared(Value(text.c_str(), a), a); }
inline void Unknowns(Value& parent, std::initializer_list<const char*> keys, Alloc& a) {
    for (const char* key : keys) {
        if (parent.HasMember(key)) continue;
        Value value(rapidjson::kObjectType);
        Set(value, "state", "unknown", a);
        Set(value, "origin", "producer", a);
        value.AddMember("value", Value(), a);
        Set(parent, key, std::move(value), a);
    }
}

inline const Value* NamedField(const Value& fields, const std::string& name) {
    for (const auto& field : fields.GetArray())
        if (Get(field, "name") == name) return &field;
    return nullptr;
}
inline Json Build(const Value* domain, const Value& physical_fields, const std::string& view,
                  const std::string& marker = "", const std::string& run_dictionary = "",
                  const std::string& session_target = "") {
    Json dictionary(rapidjson::kObjectType);
    auto& da = dictionary.GetAllocator();
    Set(dictionary, "schema_id", "flowsql.data-dictionary.v1", da);
    Set(dictionary, "title", domain ? Get(*domain, "title") : "NPM 受管目录", da);
    Value entity(rapidjson::kObjectType);
    if (domain) entity.CopyFrom((*domain)["entity"], da);
    const std::string view_rule = view == "data"
                                      ? "内部存储行保留同一运行内全部修订；公开查询优先使用 history/latest/final。"
                                  : view == "history" ? "history 保留同一运行内全部修订，累计修订不能直接相加。"
                                  : view == "latest" ? "latest 是每个运行内每实例最新修订，不表示最近一次运行。"
                                  : view == "final" ? "final 仅保留 is_final 终态；终态不等于协议成功。"
                                                    : "一行表示一次 NPM 受管运行；run_id 区分重复执行的 task_id。";
    Value limits(rapidjson::kArrayType);
    if (entity.HasMember("limitations") && entity["limitations"]["value"].IsArray())
        limits.CopyFrom(entity["limitations"]["value"], da);
    limits.PushBack(Value(view_rule.c_str(), da), da);
    if (!marker.empty()) limits.PushBack(Value(("说明依据：" + marker).c_str(), da), da);
    Set(entity, "limitations", Declared(std::move(limits), da), da);
    if (!domain) {
        Set(entity, "row_meaning", Declared(view_rule, da), da);
        Value ids(rapidjson::kArrayType);
        ids.PushBack("run_id", da);
        Set(entity, "identity_fields", Declared(std::move(ids), da), da);
    } else {
        Value ids(rapidjson::kArrayType);
        ids.PushBack("__npm_run_id", da);
        for (const auto& id : (*domain)["entity"]["identity_fields"]["value"].GetArray())
            ids.PushBack(Copy(id, da), da);
        Set(entity, "identity_fields", Declared(std::move(ids), da), da);
        Value links(rapidjson::kArrayType), link(rapidjson::kObjectType), map(rapidjson::kObjectType);
        Set(link, "dictionary_ref", run_dictionary, da);
        Set(map, "__npm_run_id", "run_id", da);
        link.AddMember("field_map", map, da);
        if (!run_dictionary.empty()) links.PushBack(link, da);
        if (!session_target.empty()) {
            Value target(rapidjson::kObjectType), fields(rapidjson::kObjectType);
            Set(target, "dictionary_ref", session_target, da);
            for (const char* id : {"__npm_run_id", "session_id", "observation_domain_id"}) Set(fields, id, id, da);
            target.AddMember("field_map", fields, da);
            links.PushBack(target, da);
            entity["limitations"]["value"].PushBack(
                Value("会话关系仅在同一 run、IP 族、input_namespace "
                      "与观测域内适用；目标可有多个独立修订，不是一对一或同 revision 连接。",
                      da),
                da);
        }
        Set(entity, "relations", Declared(std::move(links), da), da);
    }
    Unknowns(entity,
             {"row_meaning", "row_semantics", "identity_fields", "revision_field", "time_fields", "relations",
              "limitations"},
             da);
    dictionary.AddMember("entity", entity, da);
    Unknowns(dictionary, {"description"}, da);
    Value fields(rapidjson::kArrayType);
    for (const auto& physical : physical_fields.GetArray()) {
        const auto name = Get(physical, "name");
        Value field(physical, da);
        const auto* declaration = domain ? NamedField((*domain)["fields"], name) : nullptr;
        if (declaration) field.CopyFrom(*declaration, da);
        Set(field, "field_id", name, da);
        Set(field, "title", name, da);
        Set(field, "type", Copy(physical["type"], da), da);
        Set(field, "nullable", Copy(physical["nullable"], da), da);
        const std::string meaning =
            name == "run_id" || name == "__npm_run_id"
                ? "本次受管运行的精确身份；与运行目录 run_id 对应，跨运行不可混用。"
            : name == "task_id" || name == "__npm_task_id" ? "调用任务身份；同一 task_id 可以产生不同 run_id。"
            : name == "input_namespace" ? "生产配置冻结的输入命名空间；结合观察域解释会话身份。"
            : name == "status" || name == "__npm_run_status"
                ? "受管运行状态 writing/completed/incomplete；清理期间可为 purging，不表示协议业务成功。"
                : "";
        if (!meaning.empty()) {
            Set(field, "definition", Declared(meaning, da), da);
            const std::string role = name == "status" || name == "__npm_run_status" ? "state"
                                     : name == "input_namespace"                    ? "dimension"
                                                                                    : "identity";
            Set(field, "role", Declared(role, da), da);
        }
        Unknowns(field,
                 {"definition", "role", "measure_kind", "unit", "time_semantics", "null_meaning", "validity",
                  "aggregation", "value_domain", "standard_refs"},
                 da);
        fields.PushBack(field, da);
    }
    dictionary.AddMember("fields", fields, da);
    return dictionary;
}
}  // namespace flowsql::npm::result_dictionary
