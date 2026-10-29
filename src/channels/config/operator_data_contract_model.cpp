// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include "operator_data_contract_model.h"
#include <rapidjson/pointer.h>
#include <algorithm>
#include <regex>
#include <set>

namespace flowsql::channels::config {
namespace {
using V = rapidjson::Value;
std::string S(const V& v) { return {v.GetString(), v.GetStringLength()}; }
std::string Escape(const std::string& k) {
    std::string s;
    for (char c : k) s += c == '~' ? "~0" : c == '/' ? "~1" : std::string(1, c);
    return s;
}
const V* Local(const V& root, const std::string& p) {
    rapidjson::Pointer pointer(p.c_str());
    return pointer.IsValid() ? pointer.Get(root) : nullptr;
}
bool MatchType(const V& v, const std::string& t) {
    return t == "object"    ? v.IsObject()
           : t == "array"   ? v.IsArray()
           : t == "string"  ? v.IsString()
           : t == "integer" ? (v.IsInt64() || v.IsUint64())
           : t == "number"  ? v.IsNumber()
           : t == "boolean" ? v.IsBool()
           : t == "null"    ? v.IsNull()
                            : false;
}
bool Check(const V& schema, const V& v, const V& schema_root, const V& doc, std::string* path, unsigned* budget,
           unsigned depth) {
    if (!*budget || depth > 64) {
        *budget = 0;
        return false;
    }
    --*budget;
    if (schema.IsBool()) return schema.GetBool();
    if (!schema.IsObject()) return false;
    if (schema.HasMember("$ref")) {
        auto ref = S(schema["$ref"]);
        const V* target = Local(schema_root, ref);
        if (!target) target = Local(doc, ref);
        if (!target || !Check(*target, v, schema_root, doc, path, budget, depth + 1)) return false;
    }
    if (schema.HasMember("type")) {
        const auto& t = schema["type"];
        bool valid = false;
        if (t.IsString())
            valid = MatchType(v, S(t));
        else if (t.IsArray())
            for (const auto& x : t.GetArray()) valid |= x.IsString() && MatchType(v, S(x));
        if (!valid) return false;
    }
    if (schema.HasMember("const") && v != schema["const"]) return false;
    if (schema.HasMember("enum")) {
        bool ok = false;
        for (const auto& x : schema["enum"].GetArray()) ok |= x == v;
        if (!ok) return false;
    }
    for (const char* key : {"allOf", "anyOf", "oneOf"})
        if (schema.HasMember(key)) {
            unsigned matches = 0;
            for (const auto& x : schema[key].GetArray()) {
                std::string ignored = *path;
                matches += Check(x, v, schema_root, doc, &ignored, budget, depth + 1);
            }
            if ((std::string(key) == "allOf" && matches != schema[key].Size()) ||
                (std::string(key) == "anyOf" && !matches) || (std::string(key) == "oneOf" && matches != 1))
                return false;
        }
    if (schema.HasMember("not")) {
        std::string ignored = *path;
        if (Check(schema["not"], v, schema_root, doc, &ignored, budget, depth + 1)) return false;
    }
    if (schema.HasMember("if")) {
        std::string ignored = *path;
        const char* branch = Check(schema["if"], v, schema_root, doc, &ignored, budget, depth + 1) ? "then" : "else";
        if (schema.HasMember(branch) && !Check(schema[branch], v, schema_root, doc, path, budget, depth + 1))
            return false;
    }
    if (v.IsObject()) {
        if (schema.HasMember("required"))
            for (const auto& k : schema["required"].GetArray())
                if (!v.HasMember(k.GetString())) {
                    *path += "/" + Escape(S(k));
                    return false;
                }
        for (const auto& m : v.GetObject()) {
            const V* child = nullptr;
            if (schema.HasMember("properties") && schema["properties"].HasMember(m.name))
                child = &schema["properties"][m.name];
            else if (schema.HasMember("additionalProperties"))
                child = &schema["additionalProperties"];
            if (child) {
                std::string nested = *path + "/" + Escape(S(m.name));
                if (!Check(*child, m.value, schema_root, doc, &nested, budget, depth + 1)) {
                    *path = nested;
                    return false;
                }
            }
        }
    }
    if (v.IsArray()) {
        if (schema.HasMember("minItems") && v.Size() < schema["minItems"].GetUint64()) return false;
        if (schema.HasMember("maxItems") && v.Size() > schema["maxItems"].GetUint64()) return false;
        for (rapidjson::SizeType i = 0; i < v.Size(); ++i) {
            if (!*budget) return false;
            --*budget;
            const V* child = nullptr;
            if (schema.HasMember("prefixItems") && i < schema["prefixItems"].Size())
                child = &schema["prefixItems"][i];
            else if (schema.HasMember("items"))
                child = &schema["items"];
            if (child) {
                std::string nested = *path + "/" + std::to_string(i);
                if (!Check(*child, v[i], schema_root, doc, &nested, budget, depth + 1)) {
                    *path = nested;
                    return false;
                }
            }
            if (schema.HasMember("uniqueItems") && schema["uniqueItems"].IsTrue())
                for (rapidjson::SizeType j = 0; j < i; ++j) {
                    if (!*budget) return false;
                    --*budget;
                    if (v[i] == v[j]) return false;
                }
        }
    }
    if (v.IsString()) {
        if (schema.HasMember("minLength") && v.GetStringLength() < schema["minLength"].GetUint64()) return false;
        if (schema.HasMember("maxLength") && v.GetStringLength() > schema["maxLength"].GetUint64()) return false;
        if (schema.HasMember("pattern")) {
            try {
                if (!std::regex_search(S(v), std::regex(S(schema["pattern"])))) return false;
            } catch (const std::regex_error&) {
                return false;
            }
        }
    }
    if (v.IsNumber()) {
        const double n = v.GetDouble();
        if (schema.HasMember("minimum") && n < schema["minimum"].GetDouble()) return false;
        if (schema.HasMember("maximum") && n > schema["maximum"].GetDouble()) return false;
        if (schema.HasMember("exclusiveMinimum") && n <= schema["exclusiveMinimum"].GetDouble()) return false;
        if (schema.HasMember("exclusiveMaximum") && n >= schema["exclusiveMaximum"].GetDouble()) return false;
    }
    return true;
}
int SchemaDefinition(const V& schema, const V& scope, const V& doc, const std::string& path, DataSemanticsErrorV1* e,
                     unsigned depth = 0, std::set<const V*>* visited = nullptr) {
    std::set<const V*> owned_visited;
    if (!visited) visited = &owned_visited;
    auto fail = [&](const std::string& message) { return SemanticsFail(e, "CONTRACT_INVALID", message, path); };
    if (depth > 64) return fail("configuration schema nesting limit");
    if (schema.IsBool()) return 0;
    if (!schema.IsObject()) return fail("configuration schema must be object or boolean");
    if (!visited->insert(&schema).second) return 0;
    const std::set<std::string> keywords{"$schema",
                                         "$id",
                                         "$ref",
                                         "$defs",
                                         "title",
                                         "description",
                                         "default",
                                         "examples",
                                         "deprecated",
                                         "readOnly",
                                         "writeOnly",
                                         "type",
                                         "enum",
                                         "const",
                                         "properties",
                                         "required",
                                         "additionalProperties",
                                         "items",
                                         "prefixItems",
                                         "minItems",
                                         "maxItems",
                                         "uniqueItems",
                                         "minLength",
                                         "maxLength",
                                         "minimum",
                                         "maximum",
                                         "exclusiveMinimum",
                                         "exclusiveMaximum",
                                         "pattern",
                                         "allOf",
                                         "anyOf",
                                         "oneOf",
                                         "not",
                                         "if",
                                         "then",
                                         "else"};
    for (const auto& m : schema.GetObject()) {
        const auto key = S(m.name);
        if (!keywords.count(key)) return fail("unsupported configuration schema keyword: " + key);
        if (key == "$ref") {
            if (!m.value.IsString() || S(m.value).empty() || S(m.value).front() != '#')
                return fail("unclosed schema reference");
            const V* target = Local(scope, S(m.value));
            const V* target_scope = &scope;
            if (!target) {
                target = Local(doc, S(m.value));
                target_scope = target;
            }
            if (!target) return fail("unclosed schema reference");
            if (SchemaDefinition(*target, *target_scope, doc, S(m.value), e, depth + 1, visited)) return -1;
        } else if (key == "type") {
            auto valid_type = [](const V& t) {
                return t.IsString() &&
                       std::set<std::string>{"object", "array", "string", "integer", "number", "boolean", "null"}.count(
                           S(t));
            };
            if (m.value.IsString()) {
                if (!valid_type(m.value)) return fail("invalid schema type");
            } else if (m.value.IsArray() && !m.value.Empty()) {
                std::set<std::string> seen;
                for (const auto& t : m.value.GetArray())
                    if (!valid_type(t) || !seen.insert(S(t)).second) return fail("invalid type union");
            } else
                return fail("invalid schema type");
        } else if (key == "properties" || key == "$defs") {
            if (!m.value.IsObject()) return fail("schema map required");
            for (const auto& child : m.value.GetObject())
                if (SchemaDefinition(child.value, scope, doc, path + "/" + key + "/" + Escape(S(child.name)), e,
                                     depth + 1, visited))
                    return -1;
        } else if (key == "required") {
            if (!m.value.IsArray()) return fail("required must be an array");
            std::set<std::string> names;
            for (const auto& name : m.value.GetArray())
                if (!name.IsString() || !names.insert(S(name)).second) return fail("invalid required names");
        } else if (key == "enum") {
            if (!m.value.IsArray() || m.value.Empty()) return fail("enum must be nonempty array");
        } else if (key == "allOf" || key == "anyOf" || key == "oneOf" || key == "prefixItems") {
            if (!m.value.IsArray() || m.value.Empty()) return fail("schema list required");
            for (const auto& child : m.value.GetArray())
                if (SchemaDefinition(child, scope, doc, path + "/" + key, e, depth + 1, visited)) return -1;
        } else if (key == "items" || key == "additionalProperties" || key == "not" || key == "if" || key == "then" ||
                   key == "else") {
            if (SchemaDefinition(m.value, scope, doc, path + "/" + key, e, depth + 1, visited)) return -1;
        } else if (key == "minItems" || key == "maxItems" || key == "minLength" || key == "maxLength") {
            if (!m.value.IsUint64()) return fail("nonnegative schema bound required");
        } else if (key == "minimum" || key == "maximum" || key == "exclusiveMinimum" || key == "exclusiveMaximum") {
            if (!m.value.IsNumber()) return fail("numeric schema bound required");
        } else if (key == "uniqueItems" && !m.value.IsBool())
            return fail("uniqueItems must be boolean");
        else if (key == "pattern") {
            if (!m.value.IsString() || m.value.GetStringLength() > 256) return fail("bounded pattern required");
            try {
                std::regex pattern(S(m.value));
            } catch (const std::regex_error&) {
                return fail("invalid pattern");
            }
        }
    }
    if (schema.HasMember("default")) {
        unsigned budget = 100000;
        std::string where = path;
        if (!Check(schema, schema["default"], scope, doc, &where, &budget, 0) || !budget)
            return fail("default violates its own schema");
    }
    return 0;
}
int Walk(const V& v, const V& root, const std::string& p, std::vector<SemanticsReference>* refs,
         DataSemanticsErrorV1* e, const V* scope = nullptr) {
    if (v.IsObject()) {
        if (v.HasMember("$defs")) scope = &v;
        for (const auto& m : v.GetObject()) {
            const auto key = S(m.name), path = p + "/" + Escape(key);
            if (m.value.IsString() && (key == "$ref" || key == "structure_ref" || key == "schema_ref")) {
                const auto value = S(m.value);
                if (value.empty() || value[0] != '#' || (!Local(root, value) && (!scope || !Local(*scope, value))))
                    return SemanticsFail(e, "CONTRACT_INVALID", "reference must resolve inside this immutable document",
                                         path);
            }
            if (m.value.IsString() && (key == "dictionary_ref" || key == "contract_ref" || key == "binding_ref")) {
                SemanticsIdentity id;
                const auto value = S(m.value);
                if (!ParseSemanticsReference(value, &id) || (key == "dictionary_ref" && id.kind != "dictionary") ||
                    (key == "binding_ref" && id.kind != "binding") ||
                    (key == "contract_ref" && id.kind != "operator_contract"))
                    return SemanticsFail(e, "CONTRACT_INVALID", "exact reference of the expected kind required", path);
                refs->push_back({value, path});
            }
            if (key == "standard_refs" && m.value.IsArray())
                for (rapidjson::SizeType i = 0; i < m.value.Size(); ++i) {
                    SemanticsIdentity id;
                    if (!m.value[i].IsString() || !ParseSemanticsReference(S(m.value[i]), &id) || id.kind != "standard")
                        return SemanticsFail(e, "CONTRACT_INVALID", "exact standard reference required", path);
                    refs->push_back({S(m.value[i]), path + "/" + std::to_string(i)});
                }
            if (Walk(m.value, root, path, refs, e, scope)) return -1;
        }
    } else if (v.IsArray()) {
        for (rapidjson::SizeType i = 0; i < v.Size(); ++i)
            if (Walk(v[i], root, p + "/" + std::to_string(i), refs, e, scope)) return -1;
    }
    return 0;
}
int ValidateInputTree(const V& value, DataSemanticsErrorV1* e) {
    if (value.IsArray()) {
        for (const auto& child : value.GetArray())
            if (ValidateInputTree(child, e)) return -1;
    } else if (value.IsObject()) {
        for (const auto& member : value.GetObject()) {
            const auto key = S(member.name);
            if (key == "context_semantics") {
                for (const char* required : {"repeat_from_config_pointer", "item_key_pointer", "metric_pointer"})
                    if (!member.value.IsObject() || !member.value.HasMember(required) ||
                        !member.value[required].IsString())
                        return SemanticsFail(e, "CONTRACT_INVALID", "incomplete context semantics template");
                if (!member.value.HasMember("identity_columns") || !member.value["identity_columns"].IsArray() ||
                    member.value["identity_columns"].Size() != 2)
                    return SemanticsFail(e, "CONTRACT_INVALID", "context requires two identity columns");
                for (const auto& column : member.value["identity_columns"].GetArray())
                    if (!column.IsString() || !column.GetStringLength())
                        return SemanticsFail(e, "CONTRACT_INVALID", "invalid context identity column");
            }
            if (key == "roles" && member.value.IsArray()) {
                std::set<std::string> ids;
                for (const auto& role : member.value.GetArray()) {
                    if (!ids.insert(S(role["role_id"])).second)
                        return SemanticsFail(e, "CONTRACT_INVALID", "duplicate nested role identity");
                    const auto& selector = role["selector"];
                    const auto kind = S(selector["kind"]);
                    if ((kind == "column" && !selector.HasMember("column")) ||
                        (kind == "parameter" && !selector.HasMember("key")) ||
                        (kind == "config" && (!selector.HasMember("scope") || !selector.HasMember("pointer") ||
                                              !selector.HasMember("cardinality"))))
                        return SemanticsFail(e, "CONTRACT_INVALID", "incomplete nested role selector");
                }
            }
            if (key == "pointer" || key == "repeat_from_pointer" || key == "repeat_from_config_pointer" ||
                key == "item_key_pointer" || key == "discriminator_pointer" || key == "metric_pointer" ||
                key == "metric_key_pointer" || key == "submetrics_pointer") {
                if (!member.value.IsString() || !rapidjson::Pointer(member.value.GetString()).IsValid() ||
                    (member.value.GetStringLength() && member.value.GetString()[0] != '/'))
                    return SemanticsFail(e, "CONTRACT_INVALID", "invalid input JSON Pointer");
            }
            if (ValidateInputTree(member.value, e)) return -1;
        }
    }
    return 0;
}
int Unique(const V& list, const char* key, DataSemanticsErrorV1* e) {
    std::set<std::string> names;
    for (const auto& v : list.GetArray())
        if (!names.insert(S(v[key])).second)
            return SemanticsFail(e, "CONTRACT_INVALID", std::string("duplicate ") + key);
    return 0;
}
}  // namespace
int ValidateOperatorConfiguration(const V& schema, const V& instance, const V& contract, DataSemanticsErrorV1* e) {
    unsigned budget = 100000;
    std::string path;
    if (Check(schema, instance, schema, contract, &path, &budget, 0) && budget) return 0;
    return SemanticsFail(
        e, budget ? "CONTRACT_INCOMPATIBLE" : "LIMIT_EXCEEDED",
        budget ? "configuration does not satisfy its declared schema" : "configuration validation budget exhausted",
        path);
}
int ValidateOperatorParameters(const V& contract, const V& parameters, DataSemanticsErrorV1* e) {
    const auto& configuration = contract["configuration"];
    if (ValidateOperatorConfiguration(configuration["invocation_schema"], parameters, contract, e)) return -1;
    if (!configuration.HasMember("decoded_parameters")) return 0;
    for (const auto& entry : configuration["decoded_parameters"].GetArray()) {
        const auto key = S(entry["key"]);
        if (!parameters.HasMember(key.c_str())) continue;
        if (!parameters[key.c_str()].IsString())
            return SemanticsFail(e, "CONTRACT_INCOMPATIBLE", "JSON-text parameter must be a string", "/" + key);
        rapidjson::Document decoded;
        if (ParseSemanticsJson(S(parameters[key.c_str()]), &decoded, e)) {
            if (e) {
                e->code = "CONTRACT_INCOMPATIBLE";
                e->field_path = "/" + key + e->field_path;
            }
            return -1;
        }
        const auto* schema = Local(contract, S(entry["schema_ref"]));
        if (!schema) return SemanticsFail(e, "CONTRACT_INVALID", "decoded parameter schema is missing", "/" + key);
        if (ValidateOperatorConfiguration(*schema, decoded, contract, e)) {
            if (e) e->field_path = "/" + key + e->field_path;
            return -1;
        }
    }
    return 0;
}
int NormalizeOperatorDocument(const std::string& kind, rapidjson::Document* d, std::vector<SemanticsReference>* refs,
                              DataSemanticsErrorV1* e) {
    if (kind == "operator_contract") {
        const auto& configuration = (*d)["configuration"];
        for (const char* key : {"invocation_schema", "normalized_config_schema"})
            if (configuration.HasMember(key)) {
                const auto& schema = configuration[key];
                if (SchemaDefinition(schema, schema, *d, std::string("/configuration/") + key, e)) return -1;
            }

        if (ValidateInputTree((*d)["inputs"], e) || ValidateInputTree((*d)["outputs"], e)) return -1;
        for (const auto& item : configuration["affects"].GetObject())
            for (const auto& pointer : item.value.GetArray())
                if (S(pointer).empty() || S(pointer).front() != '#' || !Local(*d, S(pointer)))
                    return SemanticsFail(e, "CONTRACT_INVALID", "unclosed affected resource reference",
                                         "/configuration/affects");
        if (configuration.HasMember("decoded_parameters"))
            for (const auto& entry : configuration["decoded_parameters"].GetArray()) {
                const V* schema = Local(*d, S(entry["schema_ref"]));
                if (!schema || SchemaDefinition(*schema, *schema, *d, S(entry["schema_ref"]), e)) return -1;
            }
        if (Unique((*d)["inputs"], "port_id", e) || Unique((*d)["outputs"], "port_id", e)) return -1;
        if (!SemanticsName(S((*d)["operator"]["category"])) || !SemanticsName(S((*d)["operator"]["name"])))
            return SemanticsFail(e, "CONTRACT_INVALID", "invalid operator identity", "/operator");
        for (const auto& input : (*d)["inputs"].GetArray()) {
            if (input["min_count"].GetUint64() > input["max_count"].GetUint64() || Unique(input["roles"], "role_id", e))
                return SemanticsFail(e, "CONTRACT_INVALID", "invalid input bounds or roles", "/inputs");
            for (const auto& role : input["roles"].GetArray()) {
                const auto& s = role["selector"];
                const auto k = S(s["kind"]);
                if ((k == "column" && !s.HasMember("column")) || (k == "parameter" && !s.HasMember("key")) ||
                    (k == "config" &&
                     (!s.HasMember("scope") || !s.HasMember("pointer") || !s.HasMember("cardinality"))))
                    return SemanticsFail(e, "CONTRACT_INVALID", "incomplete selector", "/inputs");
                if (s.HasMember("pointer") && !rapidjson::Pointer(s["pointer"].GetString()).IsValid())
                    return SemanticsFail(e, "CONTRACT_INVALID", "invalid selector JSON Pointer", "/inputs");
            }
        }
    }
    return Walk(*d, *d, "", refs, e);
}
}  // namespace flowsql::channels::config
