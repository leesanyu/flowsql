// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include "operator_contract_service.h"
#include <rapidjson/pointer.h>
#include <rapidjson/schema.h>
#include <cerrno>
#include <common/channel_dataset.hpp>
#include <regex>
#include <set>
#include "data_semantics_api_schema.h"
#include "operator_contract_derivation.h"
#include "operator_data_contract_model.h"

namespace flowsql::channels::config {
namespace {
using namespace dataset;
const Value kNull;
const Value& At(const Value& v, const char* key) { return v.IsObject() && v.HasMember(key) ? v[key] : kNull; }
const Value* Pointer(const Value& v, const std::string& pointer) {
    rapidjson::Pointer p(pointer.c_str());
    return p.IsValid() ? p.Get(v) : nullptr;
}
const Value* Find(const Value& fields, const std::string& name) {
    if (fields.IsArray())
        for (const auto& f : fields.GetArray())
            if (Get(f, "name") == name) return &f;
    return nullptr;
}
int Validate(const Value& request, DataSemanticsErrorV1* e) {
    Json schema;
    schema.Parse(kSemanticsApiSchema);
    Set(schema, "$ref", "#/%24defs/operatorResolvedInvocation", schema.GetAllocator());
    rapidjson::SchemaDocument compiled(schema);
    rapidjson::SchemaValidator validator(compiled);
    if (request.Accept(validator)) return 0;
    rapidjson::StringBuffer path;
    validator.GetInvalidDocumentPointer().Stringify(path);
    return Fail(e, "CONTRACT_INVALID", "invalid resolved invocation", path.GetString());
}
struct Check {
    DataSemanticsRegistry& registry;
    const Value& invocation;
    Json result{rapidjson::kObjectType};
    bool incompatible = false, unknown = false;
    unsigned budget = 100000;
    std::set<std::string> refs;
    explicit Check(DataSemanticsRegistry& r, const Value& i) : registry(r), invocation(i) {
        auto& a = result.GetAllocator();
        for (const char* key : {"reasons", "checked_refs", "output_previews", "semantic_gaps"})
            result.AddMember(Value(key, a), Value(rapidjson::kArrayType), a);
        result.AddMember("role_map", Value(rapidjson::kObjectType), a);
        Set(result, "data_validation", "not_run", a);
    }
    void Reason(bool violation, const std::string& code, const std::string& path, const Value& expected = kNull,
                const Value& observed = kNull, const std::string& basis = "observed schema and frozen declaration") {
        incompatible |= violation;
        unknown |= !violation;
        auto& a = result.GetAllocator();
        Value reason(rapidjson::kObjectType);
        Set(reason, "code", code, a);
        Set(reason, "pointer", path, a);
        Set(reason, "basis", basis, a);
        reason.AddMember("expected", Copy(expected, a), a).AddMember("observed", Copy(observed, a), a);
        if (!violation) result["semantic_gaps"].PushBack(Copy(reason, a), a);
        result["reasons"].PushBack(reason, a);
    }
    int Binding(const Value& input, Json* binding, Json* dictionary, DataSemanticsErrorV1* e) {
        if (!input.HasMember("binding_ref")) return 0;
        const auto ref = Get(input, "binding_ref");
        SemanticsIdentity id;
        if (!ParseSemanticsReference(ref, &id) || id.kind != "binding")
            return Fail(e, "INVALID_REFERENCE", "exact input binding required");
        DataSemanticsSnapshotV1 snapshot;
        if (registry.Resolve(ref.c_str(), &snapshot, e)) return -1;
        binding->Parse(snapshot.document_json->c_str());
        const auto& source = input["snapshot"];
        const auto& previous = At(*binding, "source_snapshot");
        if (At(*binding, "object_key") != source["object_key"] ||
            (!previous.IsNull() && (Get(previous, "source_revision") != Get(source, "source_revision") ||
                                    At(previous, "fields") != source["fields"])))
            return Fail(e, "SOURCE_CHANGED", "input source no longer matches its frozen binding", "/inputs");
        bool trusted = false;
        if (registry.ObservationTrusted(ref, &trusted, e)) return -1;
        refs.insert(ref);
        if (!trusted || previous.IsNull()) {
            Reason(false, "BINDING_UNCONFIRMED", "/inputs", kNull, kNull, "binding has no trusted observation");
            return 0;
        }
        const auto dictionary_ref = Get(*binding, "dictionary_ref");
        if (registry.Resolve(dictionary_ref.c_str(), &snapshot, e)) return -1;
        refs.insert(dictionary_ref);
        dictionary->Parse(snapshot.document_json->c_str());
        return 0;
    }
    void Role(const Value& role, const Value& input, const Value& input_item, const Value& role_item,
              const std::string& path, const Value& binding, const Value& dictionary, Value& mapped) {
        if (!budget) return;
        --budget;
        const auto& selector = role["selector"];
        const auto kind = Get(selector, "kind");
        const Value* selected = nullptr;
        if (kind == "column") selected = &selector["column"];
        if (kind == "parameter") selected = &At(invocation["effective_parameters"], Get(selector, "key").c_str());
        if (kind == "config") {
            const auto scope = Get(selector, "scope");
            const auto& root = scope == "root"         ? At(invocation, "normalized_config")
                               : scope == "input_item" ? input_item
                                                       : role_item;
            selected = Pointer(root, Get(selector, "pointer"));
            if (root.IsNull()) {
                Reason(false, "CONFIG_UNAVAILABLE", path);
                return;
            }
        }
        if (!selected || selected->IsNull()) {
            if (role["required"].GetBool()) Reason(true, "SELECTOR_MISSING", path, selector);
            return;
        }
        const bool many = Get(selector, "cardinality") == "many";
        if ((!many && !selected->IsString()) || (many && !selected->IsArray())) {
            Reason(true, "SELECTOR_TYPE", path, selector, *selected);
            return;
        }
        std::vector<std::string> names;
        if (!many)
            names.push_back(Text(*selected));
        else
            for (const auto& name : selected->GetArray()) {
                if (!name.IsString() || !name.GetStringLength()) {
                    Reason(true, "SELECTOR_TYPE", path, selector, name);
                    return;
                }
                names.push_back(Text(name));
            }
        auto& a = result.GetAllocator();
        for (size_t n = 0; n < names.size(); ++n) {
            if (!budget) return;
            --budget;
            const auto& name = names[n];
            const auto& source = input["snapshot"];
            Value mapping(rapidjson::kObjectType);
            Set(mapping, "role_id", Get(role, "role_id"), a);
            Set(mapping, "role_path", path + "/" + std::to_string(n), a);
            Set(mapping, "column", name, a);
            mapping.AddMember("input_index", Copy(input["input_index"], a), a);
            const auto* actual = Find(source["fields"], name);
            if (Get(source, "schema_state") != "known") {
                Reason(false, "SCHEMA_PENDING", path);
            } else if (!actual) {
                Reason(true, "COLUMN_MISSING", path, *selected);
            } else {
                Value adapted = Copy(*actual, a);
                const auto& adaptations = At(input, "adaptations");
                if (adaptations.IsArray())
                    for (const auto& change : adaptations.GetArray()) {
                        if (Get(change, "column") != name) continue;
                        if (At(change, "source_type") != (*actual)["type"] || !At(change, "target_type").IsObject()) {
                            Reason(true, "ADAPTATION_SOURCE_MISMATCH", path);
                            continue;
                        }
                        Set(adapted, "type", Copy(change["target_type"], a), a);
                        if (At(change, "target_nullable").IsBool())
                            Set(adapted, "nullable", Copy(change["target_nullable"], a), a);
                    }
                actual = &adapted;
                mapping.AddMember("field", Copy(*actual, a), a);
                bool type_matches = role["types"].Empty();
                for (const auto& t : role["types"].GetArray()) type_matches |= t == (*actual)["type"];
                if ((*actual)["type"].IsNull())
                    Reason(false, "TYPE_UNDETERMINED", path);
                else if (!type_matches)
                    Reason(true, "TYPE_MISMATCH", path, role["types"], (*actual)["type"]);
                if (role["nullable"].IsBool() && !role["nullable"].GetBool()) {
                    if (!(*actual)["nullable"].IsBool())
                        Reason(false, "NULLABILITY_UNDETERMINED", path);
                    else if ((*actual)["nullable"].GetBool())
                        Reason(true, "NULLABILITY_MISMATCH", path, role["nullable"], (*actual)["nullable"]);
                }
                const Value* semantic_field = nullptr;
                if (dictionary.IsObject())
                    for (const auto& f : dictionary["fields"].GetArray()) {
                        const auto id = Get(f, "field_id");
                        if (Get(binding["field_map"], id.c_str()) == name) {
                            semantic_field = &f;
                            Set(mapping, "dictionary_ref", Get(binding, "dictionary_ref"), a);
                            Set(mapping, "field_id", id, a);
                            break;
                        }
                    }
                for (const auto& requirement : role["semantic_requirements"].GetObject()) {
                    const auto key = Text(requirement.name);
                    const auto& observed = semantic_field ? At(*semantic_field, key.c_str()) : kNull;
                    const bool supported =
                        key == "unit" || key == "time_semantics" || key == "measure_kind" || key == "standard_refs";
                    if (!supported || observed.IsNull() || Get(observed, "state") == "unknown")
                        Reason(false, "SEMANTICS_UNDETERMINED", path + "/" + key, requirement.value, observed);
                    else {
                        const auto& value =
                            observed.IsObject() && observed.HasMember("value") ? observed["value"] : observed;
                        if (value != requirement.value)
                            Reason(true, "SEMANTICS_MISMATCH", path + "/" + key, requirement.value, value);
                    }
                }
            }
            mapped.PushBack(mapping, a);
        }
    }
    void Roles(const Value& container, const Value& input, const Value& input_item, const Value& role_item,
               const std::string& path, const Value& binding, const Value& dictionary, Value& mapped,
               unsigned depth = 0) {
        if (depth > 32 || !budget) {
            budget = 0;
            return;
        }
        for (const auto& role : container["roles"].GetArray())
            Role(role, input, input_item, role_item, path + "/" + Get(role, "role_id"), binding, dictionary, mapped);
        const auto& templates = At(container, "role_templates");
        if (!templates.IsArray()) return;
        for (const auto& t : templates.GetArray()) {
            const auto* items = Pointer(role_item, Get(t, "repeat_from_pointer"));
            if (!items || !items->IsArray()) {
                Reason(false, "TEMPLATE_UNAVAILABLE", path);
                continue;
            }
            std::set<std::string> keys;
            for (const auto& item : items->GetArray()) {
                if (!budget) return;
                --budget;
                const auto* key = Pointer(item, Get(t, "item_key_pointer"));
                if (!key || !key->IsString() || !keys.insert(Text(*key)).second) {
                    Reason(true, "ITEM_IDENTITY_INVALID", path);
                    continue;
                }
                const auto* discriminator =
                    t.HasMember("discriminator_pointer") ? Pointer(item, Get(t, "discriminator_pointer")) : nullptr;
                const auto branch = discriminator && discriminator->IsString() ? Text(*discriminator) : "default";
                if (!t["cases"].HasMember(branch.c_str())) {
                    Reason(false, "BRANCH_UNKNOWN", path);
                    continue;
                }
                Roles(t["cases"][branch.c_str()], input, input_item, item, path + "/" + Text(*key), binding, dictionary,
                      mapped, depth + 1);
            }
        }
    }
    int Inputs(const Value& contract, DataSemanticsErrorV1* e) {
        std::set<std::string> ports;
        for (const auto& port : contract["inputs"].GetArray()) {
            const auto id = Get(port, "port_id"), path = "/inputs/" + id;
            ports.insert(id);
            const auto* items =
                port.HasMember("repeat_from_config_pointer")
                    ? Pointer(At(invocation, "normalized_config"), Get(port, "repeat_from_config_pointer"))
                    : nullptr;
            if (port.HasMember("repeat_from_config_pointer") && (!items || !items->IsArray()))
                Reason(false, "INPUT_ITEMS_UNAVAILABLE", path);
            size_t count = 0;
            std::set<std::string> item_keys;
            for (const auto& input : invocation["inputs"].GetArray()) {
                if (Get(input, "port_id") != id) continue;
                ++count;
                const auto& source = input["snapshot"];
                const auto index = input["input_index"].GetUint();
                const auto map_key = id + "/" + Get(input, "item_key") + "/" + std::to_string(index);
                Value mapped(rapidjson::kArrayType);
                const auto& shapes = At(invocation["execution_context"], "input_shapes");
                const auto shape =
                    shapes.IsArray() && index < shapes.Size() && shapes[index].IsString() ? Text(shapes[index]) : "";
                if (shape.empty())
                    Reason(false, "SHAPE_UNDETERMINED", path);
                else if (shape != Get(port, "shape"))
                    Reason(true, "SHAPE_MISMATCH", path, port["shape"]);
                if (Get(source, "schema_state") != "known") Reason(false, "SCHEMA_PENDING", path);
                if (Get(source, "identity_state") != "known") Reason(false, "IDENTITY_UNKNOWN", path);
                const Value* item = nullptr;
                if (items && items->IsArray()) {
                    for (const auto& candidate : items->GetArray()) {
                        const auto* key = Pointer(candidate, Get(port, "item_key_pointer"));
                        if (key && key->IsString() && Text(*key) == Get(input, "item_key")) item = &candidate;
                    }
                    if (!item || !item_keys.insert(Get(input, "item_key")).second)
                        Reason(true, "INPUT_ITEM_MISMATCH", path);
                }
                Json binding, dictionary;
                if (Binding(input, &binding, &dictionary, e)) return -1;
                Roles(port, input, item ? *item : kNull, item ? *item : kNull, map_key, binding, dictionary, mapped);
                if (result["role_map"].HasMember(map_key.c_str()))
                    return Fail(e, "CONTRACT_INVALID", "duplicate input identity");
                result["role_map"].AddMember(Value(map_key.c_str(), result.GetAllocator()), mapped,
                                             result.GetAllocator());
            }
            if (count < port["min_count"].GetUint64() || count > port["max_count"].GetUint64())
                Reason(true, "INPUT_COUNT", path, port);
            if (items && items->IsArray() && count != items->Size()) Reason(true, "INPUT_ITEM_COUNT", path);
        }
        for (const auto& input : invocation["inputs"].GetArray())
            if (!ports.count(Get(input, "port_id"))) Reason(true, "UNDECLARED_INPUT", "/inputs");
        if (!budget) return Fail(e, "LIMIT_EXCEEDED", "role expansion budget exceeded");
        return 0;
    }
};
}  // namespace
int OperatorContractService::Resolve(const char* ref, OperatorContractSnapshotV1* out, DataSemanticsErrorV1* e) {
    if (out) *out = {};
    if (e) *e = {};
    SemanticsIdentity id;
    if (!out || !ParseSemanticsReference(ref ? ref : "", &id) || id.kind != "operator_contract")
        return dataset::Fail(e, "INVALID_REFERENCE", "exact operator contract required");
    DataSemanticsSnapshotV1 snapshot;
    if (registry_.Resolve(ref, &snapshot, e)) return -1;
    *out = {snapshot.exact_reference, snapshot.sha256, snapshot.document_json};
    return 0;
}
int OperatorContractService::CheckAndDerive(const char* text, std::string* out, DataSemanticsErrorV1* e) {
    if (out) out->clear();
    if (e) *e = {};
    if (!out) return dataset::Fail(e, "CONTRACT_INVALID", "result required");
    Json invocation;
    if (ParseSemanticsJson(text ? text : "", &invocation, e) || Validate(invocation, e)) return -1;
    OperatorContractSnapshotV1 snapshot;
    if (Resolve(Get(invocation, "contract_ref").c_str(), &snapshot, e)) return -1;
    Json contract;
    contract.Parse(snapshot.document_json->c_str());
    Check check(registry_, invocation);
    check.refs.insert(snapshot.exact_reference);
    for (rapidjson::SizeType i = 0; i < invocation["config_refs"].Size(); ++i) {
        const auto ref = Text(invocation["config_refs"][i]);
        const auto path = "/config_refs/" + std::to_string(i);
        if (!std::regex_match(ref, std::regex("config\\.[a-z][a-z0-9._-]*@[1-9][0-9]*")))
            return Fail(e, "CONTRACT_INVALID", "exact config reference required", path);
        if (!configs_) return Fail(e, "UNAVAILABLE", "config registry unavailable", path);
        ConfigChannelSnapshot config;
        std::string detail;
        const int rc = configs_->Resolve(ref.c_str(), &config, &detail);
        if (rc)
            return Fail(e,
                        rc == ENOENT   ? "NOT_FOUND"
                        : rc == EINVAL ? "CONTRACT_INVALID"
                                       : "UNAVAILABLE",
                        detail, path);
        check.refs.insert(ref);
    }

    DataSemanticsErrorV1 invalid;
    if (ValidateOperatorParameters(contract, invocation["effective_parameters"], &invalid))
        check.Reason(true, "PARAMETERS_INVALID", "/effective_parameters" + invalid.field_path);
    const auto& config_schema = At(contract["configuration"], "normalized_config_schema");
    if (!config_schema.IsNull()) {
        if (!invocation.HasMember("normalized_config"))
            check.Reason(false, "CONFIG_UNAVAILABLE", "/normalized_config");
        else if (ValidateOperatorConfiguration(config_schema, invocation["normalized_config"], contract, &invalid))
            check.Reason(true, "CONFIG_INVALID", "/normalized_config" + invalid.field_path);
    }
    if (check.Inputs(contract, e)) return -1;
    if (DeriveOperatorOutputs(registry_, contract, invocation, check.result["role_map"],
                              &check.result["output_previews"], check.result.GetAllocator(), e))
        return -1;
    auto& a = check.result.GetAllocator();
    Set(check.result, "state", check.incompatible ? "incompatible" : check.unknown ? "undetermined" : "compatible", a);
    for (const auto& ref : check.refs) check.result["checked_refs"].PushBack(Value(ref.c_str(), a), a);
    *out = Encode(check.result);
    if (out->size() > 12 * 1024 * 1024) {
        out->clear();
        return Fail(e, "LIMIT_EXCEEDED", "check response too large");
    }
    return 0;
}
}  // namespace flowsql::channels::config
