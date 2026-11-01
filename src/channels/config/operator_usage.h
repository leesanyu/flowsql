// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <rapidjson/pointer.h>
#include <common/channel_dataset.hpp>

namespace flowsql::channels::config::operator_usage {
using namespace dataset;

inline std::string Token(const std::string& text) {
    std::string result;
    for (const char c : text) result += c == '~' ? "~0" : c == '/' ? "~1" : std::string(1, c);
    return result;
}
inline void TextItem(Value& items, const std::string& text, const char* origin, const std::string& pointer, Alloc& a) {
    if (text.empty()) return;
    Value item(rapidjson::kObjectType);
    Set(item, "text", text, a);
    Set(item, "origin", origin, a);
    Set(item, "source_pointer", pointer, a);
    items.PushBack(item, a);
}
inline void Gap(Value& data, const char* section, const char* code, const std::string& message, Alloc& a) {
    Value item(rapidjson::kObjectType);
    Set(item, "section", section, a);
    Set(item, "code", code, a);
    Set(item, "message", message, a);
    data["gaps"].PushBack(item, a);
}
inline Value Detail(const Value& data, const char* section, const std::string& pointer, const char* reason, Alloc& a) {
    Value item(rapidjson::kObjectType);
    Set(item, "section", section, a);
    Set(item, "exact_reference", Get(data["basis"], "contract_ref"), a);
    Set(item, "resource_pointer", pointer, a);
    Set(item, "reason", reason, a);
    return item;
}
inline void AddDetail(Value& data, const char* section, const std::string& pointer, const char* reason, Alloc& a) {
    for (const auto& item : data["details"].GetArray())
        if (Get(item, "section") == section && Get(item, "resource_pointer") == pointer) return;
    data["details"].PushBack(Detail(data, section, pointer, reason, a), a);
}
inline bool Required(const Value& schema, const std::string& name) {
    if (!schema.IsObject() || !schema.HasMember("required")) return false;
    for (const auto& key : schema["required"].GetArray())
        if (Text(key) == name) return true;
    return false;
}
inline bool ConditionalRequired(const Value& schema, const std::string& name) {
    if (schema.IsObject()) {
        if (Required(schema, name)) return true;
        for (const auto& member : schema.GetObject())
            if (Text(member.name) != "properties" && ConditionalRequired(member.value, name)) return true;
    } else if (schema.IsArray()) {
        for (const auto& value : schema.GetArray())
            if (ConditionalRequired(value, name)) return true;
    }
    return false;
}
inline void Parameters(Value& data, Value& usage, const Value& doc, Alloc& a) {
    const auto& config = doc["configuration"];
    const auto& schema = config["invocation_schema"];
    auto& params = usage["parameters"];
    if (config.HasMember("description")) Set(params, "description", Copy(config["description"], a), a);
    Value constraints(rapidjson::kArrayType);
    if (schema.IsObject()) {
        for (const auto& m : schema.GetObject()) {
            const auto key = Text(m.name);
            if (key == "type" || key == "properties" || key == "required" || key == "additionalProperties" ||
                key == "title" || key == "description" || key == "$schema")
                continue;
            const auto pointer = "/configuration/invocation_schema/" + Token(key);
            auto detail = Detail(data, "parameters", pointer, "参数的组级约束，填写前按需读取", a);
            constraints.PushBack(Copy(detail, a), a);
            AddDetail(data, "parameters", pointer, "参数的组级约束，填写前按需读取", a);
        }
    } else {
        constraints.PushBack(Detail(data, "parameters", "/configuration/invocation_schema", "完整参数约束", a), a);
        AddDetail(data, "parameters", "/configuration/invocation_schema", "完整参数约束", a);
    }
    const bool grouped = !constraints.Empty();
    Set(params, "constraints", std::move(constraints), a);
    std::string additional = "allowed";
    if (schema.IsBool()) additional = schema.GetBool() ? "allowed" : "forbidden";
    if (schema.IsObject() && schema.HasMember("additionalProperties")) {
        const auto& ap = schema["additionalProperties"];
        additional = ap.IsBool() ? (ap.GetBool() ? "allowed" : "forbidden") : "conditional";
        if (!ap.IsBool())
            AddDetail(data, "parameters", "/configuration/invocation_schema/additionalProperties", "额外参数的取值约束",
                      a);
    }
    if (grouped && additional == "allowed") additional = "conditional";
    Set(params, "additional_parameters", additional, a);
    if (schema.IsObject() && schema.HasMember("properties")) {
        for (const auto& m : schema["properties"].GetObject()) {
            const auto name = Text(m.name);
            Value p(rapidjson::kObjectType), links(rapidjson::kArrayType), required;
            Set(p, "name", name, a);
            Set(p, "source_pointer", "/configuration/invocation_schema/properties/" + Token(name), a);
            if (Required(schema, name))
                required.SetBool(true);
            else if (!ConditionalRequired(schema, name))
                required.SetBool(false);
            p.AddMember("required", required, a);
            Set(p, "schema", Copy(m.value, a), a);
            for (const auto& input : doc["inputs"].GetArray())
                for (const auto& role : input["roles"].GetArray())
                    if (Get(role["selector"], "kind") == "parameter" && Get(role["selector"], "key") == name) {
                        Value link(rapidjson::kObjectType);
                        Set(link, "port_id", Get(input, "port_id"), a);
                        Set(link, "role_id", Get(role, "role_id"), a);
                        links.PushBack(link, a);
                    }
            std::string control = links.Empty() ? "text" : "field";
            if (control == "text" && m.value.IsObject() && m.value.HasMember("enum")) control = "select";
            Set(p, "wire_encoding", "string", a);
            if (config.HasMember("decoded_parameters"))
                for (const auto& decoded : config["decoded_parameters"].GetArray())
                    if (Get(decoded, "key") == name) {
                        const auto pointer = Get(decoded, "schema_ref").substr(1);
                        Set(p, "wire_encoding", "json", a);
                        Set(p, "decoded_schema_pointer", pointer, a);
                        AddDetail(data, "parameters", pointer, "该参数仅编码一次为 JSON 文本，按内部配置规则填写", a);
                        control = "json";
                    }
            if (m.value.IsBool() ||
                (m.value.IsObject() &&
                 (m.value.HasMember("$ref") || m.value.HasMember("oneOf") || m.value.HasMember("anyOf") ||
                  m.value.HasMember("allOf") || (m.value.HasMember("type") && Get(m.value, "type") != "string")))) {
                control = "advanced";
                AddDetail(data, "parameters", Get(p, "source_pointer"), "复杂参数按完整声明填写，调用仍提交字符串", a);
            }
            Set(p, "control", control, a);
            Set(p, "input_roles", std::move(links), a);
            params["items"].PushBack(p, a);
        }
    }
    if (config.HasMember("normalized_config_schema"))
        AddDetail(data, "parameters", "/configuration/normalized_config_schema", "生产解析后的有效配置结构与条件约束",
                  a);
    if (!config["affects"].ObjectEmpty())
        AddDetail(data, "parameters", "/configuration/affects", "参数影响的输入与输出范围", a);
}
inline void Outputs(Value& data, Value& usage, const Value& doc, Alloc& a) {
    rapidjson::SizeType i = 0;
    for (const auto& output : doc["outputs"].GetArray()) {
        Value item(rapidjson::kObjectType), fields(rapidjson::kArrayType);
        const auto pointer = "/outputs/" + std::to_string(i++);
        for (const auto* key :
             {"port_id", "shape", "mode", "availability", "row_cases", "context_semantics", "description"})
            if (output.HasMember(key)) Set(item, key, Copy(output[key], a), a);
        Set(item, "source_pointer", pointer, a);
        std::string summary, state = "unknown";
        if (output.HasMember("structure_ref")) {
            const auto ref = Get(output, "structure_ref");
            if (ref.rfind("#/", 0) == 0) {
                const auto path = ref.substr(1);
                const auto* structure = rapidjson::Pointer(path.c_str()).Get(doc);
                assert(structure);
                summary = Get(*structure, "description");
                AddDetail(data, "outputs", path, "输出声明结构；动态字段和条件按所属输出规则解释", a);
                // Resources are operator-owned objects, not a second validated field schema.
                // Project only the two declared representations; other shapes remain readable in details.
                bool physical =
                    structure->IsObject() && structure->HasMember("fields") && (*structure)["fields"].IsArray();
                if (physical)
                    for (const auto& field : (*structure)["fields"].GetArray())
                        if (Get(field, "name").empty()) physical = false;
                bool properties = structure->IsObject() && structure->HasMember("properties") &&
                                  (*structure)["properties"].IsObject();
                if (properties)
                    for (const auto& member : (*structure)["properties"].GetObject())
                        if (!member.name.GetStringLength() || (!member.value.IsObject() && !member.value.IsBool()))
                            properties = false;
                if (physical) {
                    rapidjson::SizeType n = 0;
                    for (const auto& field : (*structure)["fields"].GetArray()) {
                        Value f(rapidjson::kObjectType);
                        Set(f, "name", Get(field, "name"), a);
                        Set(f, "format", "physical", a);
                        Set(f, "source_pointer", path + "/fields/" + std::to_string(n++), a);
                        Set(f, "declaration", Copy(field, a), a);
                        fields.PushBack(f, a);
                    }
                    state = "declared";
                } else if (properties) {
                    for (const auto& m : (*structure)["properties"].GetObject()) {
                        Value f(rapidjson::kObjectType);
                        Set(f, "name", Text(m.name), a);
                        Set(f, "format", "json_schema", a);
                        Set(f, "source_pointer", path + "/properties/" + Token(Text(m.name)), a);
                        Set(f, "declaration", Copy(m.value, a), a);
                        fields.PushBack(f, a);
                    }
                    state = "declared";
                    if (structure->HasMember("patternProperties") || !structure->HasMember("additionalProperties") ||
                        !((*structure)["additionalProperties"].IsBool() &&
                          !(*structure)["additionalProperties"].GetBool()))
                        state = "dynamic";
                }
            } else {
                AddDetail(data, "outputs", pointer + "/structure_ref", "输出结构的引用需结合完整上下文读取", a);
            }
        }
        if (!output["rules"].Empty()) {
            AddDetail(data, "outputs", pointer + "/rules", "字段来源、转换与输出推导规则", a);
            if (output["rules"].Size() == 1 && Get(output["rules"][0], "kind") == "copy" &&
                output["rules"][0].HasMember("all_fields") && output["rules"][0]["all_fields"].GetBool())
                summary = "复制输入端口 " + Get(output["rules"][0], "source_port") + " 的全部字段。";
        }
        for (const auto* key : {"variants", "dictionary_ref"})
            if (output.HasMember(key)) AddDetail(data, "outputs", pointer + "/" + key, "输出的分支或精确语义依据", a);
        if (state == "unknown" && output.HasMember("variants") && !output["variants"].Empty()) state = "dynamic";
        if (state == "unknown" && output["rules"].Empty() && !output.HasMember("structure_ref"))
            Gap(data, "outputs", "OUTPUT_FIELDS_UNDECLARED", "输出 " + Get(output, "port_id") + " 的字段信息不足", a);
        Set(item, "summary", Get(output, "description", summary), a);
        Set(item, "fields_state", state, a);
        Set(item, "fields", std::move(fields), a);
        usage["outputs"].PushBack(item, a);
    }
}
inline Json Build(const Value& subject, const Value& basis, const Value& contract, const Value& notes,
                  const Value& context) {
    Json data(rapidjson::kObjectType);
    auto& a = data.GetAllocator();
    Set(data, "subject", Copy(subject, a), a);
    Set(data, "view", "usage", a);
    Set(data, "basis", Copy(basis, a), a);
    Set(data, "contract_state", Get(contract, "state"), a);
    for (const auto* key : {"gaps", "details", "required_context_refs"})
        data.AddMember(Value(key, a), Value(rapidjson::kArrayType), a);
    if (context.HasMember("required_context_refs"))
        Set(data, "required_context_refs", Copy(context["required_context_refs"], a), a);
    Value usage(rapidjson::kObjectType), identity(rapidjson::kObjectType), parameters(rapidjson::kObjectType);
    Set(identity, "name", Get(subject, "name"), a);
    Set(identity, "title", "", a);
    identity.AddMember("author_version", Value(), a).AddMember("purposes", Value(rapidjson::kArrayType), a);
    parameters.AddMember("items", Value(rapidjson::kArrayType), a)
        .AddMember("constraints", Value(rapidjson::kArrayType), a);
    Set(parameters, "additional_parameters", "unknown", a);
    usage.AddMember("identity", identity, a)
        .AddMember("inputs", Value(rapidjson::kArrayType), a)
        .AddMember("parameters", parameters, a)
        .AddMember("outputs", Value(rapidjson::kArrayType), a)
        .AddMember("limitations", Value(rapidjson::kArrayType), a)
        .AddMember("examples", Value(rapidjson::kArrayType), a);
    Value annotations(rapidjson::kObjectType);
    for (const auto* key : {"inputs", "parameters", "outputs"})
        annotations.AddMember(Value(key, a), notes.IsObject() ? Copy(notes[key], a) : Value(rapidjson::kArrayType), a);
    if (notes.IsObject())
        for (const auto* key : {"input_description", "parameter_description", "output_description"})
            if (notes.HasMember(key)) annotations.AddMember(Value(key, a), Copy(notes[key], a), a);
    usage.AddMember("annotations", annotations, a);
    if (notes.IsObject()) {
        TextItem(usage["identity"]["purposes"], Get(notes, "purpose"), "local", "/description/notes/purpose", a);
        TextItem(usage["limitations"], Get(notes, "usage_notes"), "local", "/description/notes/usage_notes", a);
        rapidjson::SizeType i = 0;
        for (const auto& example : notes["examples"].GetArray())
            TextItem(usage["examples"], Text(example), "local", "/description/notes/examples/" + std::to_string(i++),
                     a);
    }
    if (Get(contract, "state") == "missing") {
        Gap(data, "inputs", "CONTRACT_MISSING", "算子尚未声明结构化契约；输入、参数与输出要求未知", a);
    } else {
        const auto& doc = contract["document"];
        Set(usage["identity"], "title", Get(doc, "title"), a);
        Set(usage["identity"], "author_version", Get(doc["operator"], "version"), a);
        TextItem(usage["identity"]["purposes"], Get(doc, "description"), "author", "/description", a);
        if (doc.HasMember("examples")) {
            rapidjson::SizeType n = 0;
            for (const auto& example : doc["examples"].GetArray())
                TextItem(usage["examples"], Text(example), "author", "/examples/" + std::to_string(n++), a);
        }
        rapidjson::SizeType i = 0;
        for (const auto& input : doc["inputs"].GetArray()) {
            Value item(rapidjson::kObjectType);
            const auto pointer = "/inputs/" + std::to_string(i++);
            for (const auto* key : {"port_id", "min_count", "max_count", "shape", "roles"})
                Set(item, key, Copy(input[key], a), a);
            if (input.HasMember("description")) Set(item, "description", Copy(input["description"], a), a);
            Set(item, "source_pointer", pointer, a);
            usage["inputs"].PushBack(item, a);
            if (input.HasMember("structure_ref"))
                AddDetail(data, "inputs", Get(input, "structure_ref").substr(1), "输入结构及来源能力", a);
            for (const auto& m : input.GetObject()) {
                const auto key = Text(m.name);
                if (key != "port_id" && key != "min_count" && key != "max_count" && key != "shape" && key != "roles" &&
                    key != "description" && key != "structure_ref")
                    AddDetail(data, "inputs", pointer + "/" + Token(key), "输入集合及配置作用域规则", a);
            }
        }
        Parameters(data, usage, doc, a);
        Outputs(data, usage, doc, a);
        i = 0;
        for (const auto& limitation : doc["limitations"].GetArray())
            TextItem(usage["limitations"], Text(limitation), "author", "/limitations/" + std::to_string(i++), a);
    }
    if (usage["identity"]["purposes"].Empty())
        Gap(data, "identity", "PURPOSE_UNDECLARED", "尚未声明用途；可补充本地用途说明", a);
    data.AddMember("usage", usage, a);
    return data;
}
}  // namespace flowsql::channels::config::operator_usage
