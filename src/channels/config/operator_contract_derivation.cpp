// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include "operator_contract_derivation.h"
#include <rapidjson/pointer.h>
#include <rapidjson/schema.h>
#include <map>
#include <set>
#include "data_semantics_api_schema.h"
#include "operator_contract_service.h"
namespace flowsql::channels::config {
namespace {
using namespace dataset;
const Value kNull;
const Value& At(const Value& v, const char* k) { return v.IsObject() && v.HasMember(k) ? v[k] : kNull; }
const Value* Ptr(const Value& v, const std::string& p) {
    rapidjson::Pointer pointer(p.c_str());
    return pointer.IsValid() ? pointer.Get(v) : nullptr;
}
Value Semantic(const Value& value, Alloc& a) {
    Value s(rapidjson::kObjectType);
    s.AddMember("state", "declared", a).AddMember("value", Copy(value, a), a).AddMember("origin", "derived", a);
    return s;
}
Value Unknown(Alloc& a) {
    Value s(rapidjson::kObjectType);
    s.AddMember("state", "unknown", a).AddMember("value", Value(), a).AddMember("origin", "derived", a);
    return s;
}
Value Dictionary(const std::string& title, Alloc& a) {
    Value d(rapidjson::kObjectType);
    Set(d, "schema_id", "flowsql.data-dictionary.v1", a);
    Set(d, "title", title, a);
    d.AddMember("entity", Value(rapidjson::kObjectType), a).AddMember("fields", Value(rapidjson::kArrayType), a);
    return d;
}
Value OutputField(const Value& physical, const std::string& id, Alloc& a) {
    Value f(physical, a);
    Set(f, "field_id", id, a);
    Set(f, "title", Get(f, "name"), a);
    return f;
}
// Missing exists predicates are false; missing equality operands are unknown.
int Condition(const Value& condition, const Value& invocation) {
    if (At(condition, "all").IsArray()) {
        int state = 1;
        for (const auto& child : condition["all"].GetArray()) {
            const int next = Condition(child, invocation);
            if (next == 0) return 0;
            if (next < 0) state = -1;
        }
        return state;
    }
    const auto scope = Get(condition, "scope");
    const auto& root = At(invocation, scope == "config" ? "normalized_config" : "execution_context");
    if (root.IsNull() || scope == "row") return -1;
    const auto* value = Ptr(root, Get(condition, "pointer"));
    if (At(condition, "exists").IsBool()) return bool(value) == condition["exists"].GetBool();
    return value && condition.HasMember("equals") ? int(*value == condition["equals"]) : -1;
}
struct Source {
    const Value* input;
    const Value* physical;
    Json dictionary, binding;
    const Value* semantic = nullptr;
};
int SourceField(DataSemanticsRegistry& registry, const Value& input, const Value& physical, Source* out,
                DataSemanticsErrorV1* error) {
    out->input = &input;
    out->physical = &physical;
    if (!input.HasMember("binding_ref")) return 0;
    bool trusted = false;
    if (registry.ObservationTrusted(Get(input, "binding_ref"), &trusted, error)) return -1;
    if (!trusted) return 0;
    DataSemanticsSnapshotV1 snapshot;
    if (registry.Resolve(Get(input, "binding_ref").c_str(), &snapshot, error)) return -1;
    out->binding.Parse(snapshot.document_json->c_str());
    if (registry.Resolve(Get(out->binding, "dictionary_ref").c_str(), &snapshot, error)) return -1;
    out->dictionary.Parse(snapshot.document_json->c_str());
    for (const auto& f : out->dictionary["fields"].GetArray())
        if (Get(out->binding["field_map"], Get(f, "field_id").c_str()) == Get(physical, "name")) {
            out->semantic = &f;
            break;
        }
    return 0;
}
void Lineage(Value& preview, const Value& field, const Source& source, const Value& rule, Alloc& a) {
    Value link(rapidjson::kObjectType);
    Set(link, "output_field_id", Get(field, "field_id"), a);
    link.AddMember("input_index", Copy((*source.input)["input_index"], a), a).AddMember("rule", Copy(rule, a), a);
    if (source.semantic) {
        Set(link, "dictionary_ref", Get(source.binding, "dictionary_ref"), a);
        Set(link, "field_id", Get(*source.semantic, "field_id"), a);
    }
    preview["lineage"].PushBack(link, a);
}
bool Conversion(const Value& rule) {
    for (const char* key :
         {"source_unit", "target_unit", "scale", "offset", "type", "rounding", "null_policy", "overflow_policy"})
        if (!rule.HasMember(key)) return false;
    return rule["scale"].IsNumber() && rule["offset"].IsNumber() && rule["type"].IsObject();
}
void DeriveField(Value& f, const Source& source, const Value& rule, Value& preview, Alloc& a) {
    const auto kind = Get(rule, "kind"), name = Get(rule, "output_field", Get(*source.physical, "name"));
    if (source.semantic && (kind == "copy" || kind == "rename")) f.CopyFrom(*source.semantic, a);
    for (const char* key : {"definition", "role", "measure_kind", "unit", "time_semantics", "null_meaning", "validity",
                            "aggregation", "value_domain", "standard_refs"})
        if (At(f, key).IsObject() && f[key].HasMember("state")) Set(f[key], "origin", "derived", a);
    Set(f, "name", name, a);
    Set(f, "title", name, a);
    const auto& detail = At(rule, kind == "unit_convert" ? "unit_rule" : "aggregation_rule");
    if (kind == "unit_convert" || kind == "aggregate") {
        // Transformations change field identity and invalidate source-specific constraints.
        Set(f, "field_id", "f_" + Hash(Canonical(rule) + Canonical(*source.input)).substr(0, 32), a);
        for (const char* key : {"standard_refs", "validity", "value_domain", "time_semantics", "aggregation"})
            Set(f, key, Unknown(a), a);
        if (At(detail, "type").IsObject()) Set(f, "type", Copy(detail["type"], a), a);
        if (kind == "unit_convert") {
            const auto& unit = source.semantic ? At(*source.semantic, "unit") : kNull;
            if (Conversion(detail) && Get(unit, "state") == "declared" && unit["value"] == detail["source_unit"])
                Set(f, "unit", Semantic(detail["target_unit"], a), a);
            else
                preview["semantic_gaps"].PushBack("UNIT_CONVERSION_UNCONFIRMED", a);
        } else {
            const auto operation = Get(detail, "operation");
            const bool supported = std::set<std::string>{"sum", "count", "mean", "min", "max"}.count(operation);
            const bool confirmed = supported && At(detail, "group_by").IsArray() && At(detail, "type").IsObject() &&
                                   At(detail, "null_policy").IsString() &&
                                   At(detail, "prerequisites_confirmed").IsTrue();
            if (confirmed) {
                if (operation == "count") {
                    Json unit;
                    unit.Parse(R"({"code":"1","label":"count"})");
                    Set(f, "unit", Semantic(unit, a), a);
                } else if (source.semantic && At(*source.semantic, "unit").IsObject())
                    Set(f, "unit", Copy((*source.semantic)["unit"], a), a);
                Set(f, "measure_kind", Semantic(Value("statistic", a), a), a);
            } else
                preview["semantic_gaps"].PushBack("AGGREGATION_PREREQUISITES_UNKNOWN", a);
        }
    }
}
void Entity(Value& dictionary, const Source& source, const Value& rule, Alloc& a) {
    if (!source.semantic || (Get(rule, "kind") != "copy" && Get(rule, "kind") != "rename")) return;
    if (dictionary["entity"].ObjectEmpty()) dictionary["entity"].CopyFrom(source.dictionary["entity"], a);
}
void PruneEntity(Value& dictionary, Value& gaps, Alloc& a) {
    std::set<std::string> ids;
    for (const auto& f : dictionary["fields"].GetArray()) ids.insert(Get(f, "field_id"));
    for (auto& field : dictionary["fields"].GetArray()) {
        for (const char* key : {"validity", "aggregation"}) {
            if (!field.HasMember(key)) continue;
            const auto& semantic = field[key];
            if (Get(semantic, "state") != "declared") continue;
            bool closed = true;
            auto condition_closed = [&](const Value& condition) {
                if (At(condition, "field_ids").IsArray())
                    for (const auto& id : condition["field_ids"].GetArray())
                        if (!ids.count(Text(id))) closed = false;
            };
            if (std::string(key) == "validity")
                condition_closed(semantic["value"]);
            else
                for (const auto& rule : semantic["value"].GetArray()) condition_closed(rule["condition"]);
            if (!closed) {
                Set(field, key, Unknown(a), a);
                gaps.PushBack("FIELD_CONTEXT_PRUNED", a);
            }
        }
    }
    auto& entity = dictionary["entity"];
    for (const char* key : {"identity_fields", "time_fields", "revision_field", "relations"}) {
        const auto& semantic = At(entity, key);
        if (Get(semantic, "state") != "declared") continue;
        bool valid = true;
        if (std::string(key) == "relations") {
            for (const auto& relation : semantic["value"].GetArray())
                for (const auto& mapping : relation["field_map"].GetObject()) valid &= ids.count(Text(mapping.name));
        } else if (semantic["value"].IsArray()) {
            for (const auto& id : semantic["value"].GetArray()) valid &= ids.count(Text(id));
        } else
            valid = semantic["value"].IsString() && ids.count(Text(semantic["value"]));
        if (!valid) {
            Set(entity, key, Unknown(a), a);
            gaps.PushBack("CONTEXT_PRUNED", a);
        }
    }
}
// Context expansion is driven by the published template and resolved roles.
// It preserves source semantics without inventing a unit for a heterogeneous result column.
int Contexts(DataSemanticsRegistry& registry, const Value& spec, const Value& invocation, const Value& role_map,
             Value& preview, Alloc& a, unsigned* budget, DataSemanticsErrorV1* error) {
    const auto* items = Ptr(At(invocation, "normalized_config"), Get(spec, "repeat_from_config_pointer"));
    Set(preview, "context_state", "incomplete", a);
    if (!items || !items->IsArray()) {
        preview["semantic_gaps"].PushBack("CONTEXT_CONFIG_UNAVAILABLE", a);
        return 0;
    }
    bool complete = true;
    std::set<std::string> required;
    for (const auto& item : items->GetArray()) {
        const auto* key = Ptr(item, Get(spec, "item_key_pointer"));
        const auto* metrics = Ptr(item, Get(spec, "metric_pointer"));
        if (!key || !key->IsString() || !metrics || !metrics->IsArray()) {
            complete = false;
            continue;
        }
        for (const auto& metric : metrics->GetArray()) {
            const auto* metric_key =
                Ptr(metric, Get(spec, "metric_key_pointer").empty() ? "/id" : Get(spec, "metric_key_pointer"));
            if (!metric_key || !metric_key->IsString()) {
                complete = false;
                continue;
            }
            const auto* children = Ptr(metric, Get(spec, "submetrics_pointer"));
            const bool expand = At(spec, "include_submetrics").IsTrue() && children && children->IsArray();
            std::vector<const Value*> instances;
            if (expand)
                for (const auto& child : children->GetArray()) instances.push_back(&child);
            else
                instances.push_back(&metric);
            for (const auto* instance : instances) {
                if (!*budget) return Fail(error, "LIMIT_EXCEEDED", "output context expansion budget exceeded");
                --*budget;
                Value context(rapidjson::kObjectType), roles(rapidjson::kArrayType), refs(rapidjson::kArrayType);
                Value when(rapidjson::kObjectType);
                const auto& identity = At(spec, "identity_columns");
                if (!identity.IsArray() || identity.Size() != 2 || !identity[0].IsString() || !identity[1].IsString())
                    return Fail(error, "CONTRACT_INVALID", "context requires two identity columns");
                Set(when, Text(identity[0]).c_str(), Text(*key), a);
                Set(when, Text(identity[1]).c_str(), Get(*instance, "id"), a);
                Set(context, "when", std::move(when), a);
                Set(context, "metric_config", Copy(metric, a), a);
                if (expand) Set(context, "parent_metric_id", Text(*metric_key), a);
                bool bound = false, closed = true;
                std::set<std::string> local_refs;
                for (const auto& input : invocation["inputs"].GetArray()) {
                    if (Get(input, "item_key") != Text(*key) ||
                        (!Get(spec, "input_port").empty() && Get(input, "port_id") != Get(spec, "input_port")))
                        continue;
                    const auto prefix =
                        Get(input, "port_id") + "/" + Text(*key) + "/" + std::to_string(input["input_index"].GetUint());
                    if (!role_map.HasMember(prefix.c_str())) continue;
                    for (const auto& mapped : role_map[prefix.c_str()].GetArray()) {
                        const auto path = Get(mapped, "role_path");
                        const auto metric_prefix = prefix + "/" + Text(*metric_key) + "/";
                        // Shared input roles, this metric, and (for routed rows) this submetric only.
                        if (path.rfind(prefix + "/", 0) != 0) continue;
                        const auto suffix = path.substr(prefix.size() + 1);
                        const auto separator = suffix.find('/');
                        const bool common =
                            separator != std::string::npos && suffix.find('/', separator + 1) == std::string::npos;
                        if (!common && path.rfind(metric_prefix, 0) != 0) continue;
                        if (expand && !common) {
                            const auto child_prefix = metric_prefix + Get(*instance, "id") + "/";
                            // Keep the relation's group role as common context; isolate child expressions.
                            const auto tail = path.substr(metric_prefix.size());
                            if (tail.find('/') != tail.rfind('/') && path.rfind(child_prefix, 0) != 0) continue;
                        }
                        Value role(mapped, a);
                        const auto ref = Get(mapped, "dictionary_ref");
                        bound = true;
                        if (ref.empty())
                            closed = false;
                        else {
                            DataSemanticsSnapshotV1 dictionary;
                            if (registry.Resolve(ref.c_str(), &dictionary, error)) return -1;
                            Json doc;
                            doc.Parse(dictionary.document_json->c_str());
                            for (const auto& field : doc["fields"].GetArray())
                                if (Get(field, "field_id") == Get(mapped, "field_id"))
                                    Set(role, "source_semantics", Copy(field, a), a);
                            local_refs.insert(ref);
                            required.insert(ref);
                        }
                        if (input.HasMember("binding_ref")) Set(role, "binding_ref", Get(input, "binding_ref"), a);
                        roles.PushBack(role, a);
                    }
                }
                for (const auto& ref : local_refs) refs.PushBack(Value(ref.c_str(), a), a);
                Set(context, "roles", std::move(roles), a);
                Set(context, "required_context_refs", std::move(refs), a);
                Set(context, "unit", Unknown(a), a);
                Set(context, "state", bound && closed ? "complete" : "incomplete", a);
                Set(context, "data_validation", "not_run", a);
                complete &= bound && closed;
                preview["semantic_contexts"].PushBack(context, a);
            }
        }
    }
    for (const auto& ref : required) preview["required_context_refs"].PushBack(Value(ref.c_str(), a), a);
    complete &= !preview["semantic_contexts"].Empty();
    Set(preview, "context_state", complete ? "complete" : "incomplete", a);
    if (!complete) preview["semantic_gaps"].PushBack("INPUT_SEMANTIC_CONTEXT_INCOMPLETE", a);
    return 0;
}
}  // namespace
int DeriveOperatorOutputs(DataSemanticsRegistry& registry, const Value& contract, const Value& invocation,
                          const Value& role_map, Value* previews, Alloc& a, DataSemanticsErrorV1* error) {
    previews->SetArray();
    unsigned budget = 100000;
    for (const auto& declared : contract["outputs"].GetArray()) {
        Value port(declared, a), preview(rapidjson::kObjectType);
        Set(preview, "port_id", Get(port, "port_id"), a);
        int availability = port.HasMember("availability") ? Condition(port["availability"], invocation) : 1;
        Set(preview, "availability", availability > 0 ? "available" : availability == 0 ? "absent" : "undetermined", a);
        for (const char* key : {"lineage", "semantic_gaps", "semantic_contexts", "required_context_refs"})
            preview.AddMember(Value(key, a), Value(rapidjson::kArrayType), a);
        if (port.HasMember("variants")) {
            const Value* selected = nullptr;
            for (const auto& variant : port["variants"].GetArray())
                if (Condition(At(variant, "when"), invocation) == 1) {
                    if (selected) return Fail(error, "CONTRACT_INVALID", "ambiguous output variants");
                    selected = &variant;
                }
            if (selected) {
                for (const auto& m : selected->GetObject())
                    if (Text(m.name) != "when") Set(port, Text(m.name).c_str(), Copy(m.value, a), a);
            } else {
                Set(port, "mode", "unknown", a);
                port["rules"].Clear();
                port.RemoveMember("structure_ref");
                port.RemoveMember("dictionary_ref");
            }
        }
        Value dictionary = Dictionary(Get(contract, "title") + ": " + Get(port, "port_id"), a);
        const auto* structure =
            port.HasMember("structure_ref") ? Ptr(contract, Get(port, "structure_ref").substr(1)) : nullptr;
        if (port.HasMember("dictionary_ref")) {
            DataSemanticsSnapshotV1 snapshot;
            if (registry.Resolve(Get(port, "dictionary_ref").c_str(), &snapshot, error)) return -1;
            Json d;
            d.Parse(snapshot.document_json->c_str());
            dictionary.CopyFrom(d, a);
        } else if (structure && At(*structure, "fields").IsArray()) {
            for (const auto& f : (*structure)["fields"].GetArray())
                dictionary["fields"].PushBack(
                    OutputField(f, "f_" + Hash(Get(port, "port_id") + "/" + Get(f, "name")).substr(0, 32), a), a);
        }
        std::set<std::string> output_names;
        std::set<unsigned> source_indices;
        const Value* aggregation = nullptr;
        std::map<std::string, std::pair<size_t, std::string>> identities;
        for (const auto& f : dictionary["fields"].GetArray()) output_names.insert(Get(f, "name"));
        for (const auto& rule : port["rules"].GetArray()) {
            if (Get(port, "mode") == "unknown" || Get(port, "mode") == "fixed") break;
            if (Get(rule, "kind") == "unknown") continue;
            if (Get(rule, "kind") == "aggregate") aggregation = &rule["aggregation_rule"];
            for (const auto& input : invocation["inputs"].GetArray()) {
                if (Get(input, "port_id") != Get(rule, "source_port") || !At(input["snapshot"], "fields").IsArray())
                    continue;
                std::set<std::string> selected;
                if (At(rule, "all_fields").IsTrue())
                    for (const auto& f : input["snapshot"]["fields"].GetArray()) selected.insert(Get(f, "name"));
                else if (At(rule, "source_roles").IsArray()) {
                    for (const auto& roles : role_map.GetObject())
                        for (const auto& mapped : roles.value.GetArray()) {
                            if (mapped["input_index"] != input["input_index"]) continue;
                            for (const auto& role : rule["source_roles"].GetArray())
                                if (Text(role) == Get(mapped, "role_id")) selected.insert(Get(mapped, "column"));
                        }
                }
                for (const auto& physical : input["snapshot"]["fields"].GetArray()) {
                    if (!selected.count(Get(physical, "name"))) continue;
                    if (!budget--) return Fail(error, "LIMIT_EXCEEDED", "output expansion budget exceeded");
                    source_indices.insert(input["input_index"].GetUint());
                    Source source;
                    if (SourceField(registry, input, physical, &source, error)) return -1;
                    Value field = OutputField(
                        physical,
                        "f_" + Hash(Canonical(input["snapshot"]["object_key"]) + Get(physical, "name")).substr(0, 32),
                        a);
                    DeriveField(field, source, rule, preview, a);
                    const auto name = Get(field, "name"), id = Get(field, "field_id");
                    if (!output_names.insert(name).second)
                        return Fail(error, "CONTRACT_INVALID", "duplicate derived output column", "/outputs");
                    const auto identity =
                        Get(source.binding, "dictionary_ref") + "/" + std::to_string(input["input_index"].GetUint());
                    auto old = identities.find(id);
                    if (old != identities.end() && old->second.second != identity) {
                        Set(field, "field_id", "f_" + Hash(id + Canonical(input)).substr(0, 32), a);
                        auto& prior = dictionary["fields"][old->second.first];
                        Set(prior, "field_id", "f_" + Hash(id + old->second.second).substr(0, 32), a);
                        for (auto& link : preview["lineage"].GetArray())
                            if (Get(link, "output_field_id") == id)
                                Set(link, "output_field_id", Get(prior, "field_id"), a);
                        preview["semantic_gaps"].PushBack("FIELD_IDENTITY_CONFLICT", a);
                    } else
                        identities[id] = {dictionary["fields"].Size(), identity};
                    Entity(dictionary, source, rule, a);
                    Lineage(preview, field, source, rule, a);
                    dictionary["fields"].PushBack(field, a);
                }
            }
        }
        if (source_indices.size() > 1 || aggregation) {
            dictionary["entity"].SetObject();
            preview["semantic_gaps"].PushBack("ROW_CONTEXT_CHANGED", a);
        }
        if (aggregation && At(*aggregation, "group_by").IsArray()) {
            Set(dictionary["entity"], "row_semantics", Semantic(Value("aggregate", a), a), a);
            Value group_ids(rapidjson::kArrayType);
            bool closed = true;
            for (const auto& key : (*aggregation)["group_by"].GetArray()) {
                bool found = false;
                for (const auto& field : dictionary["fields"].GetArray())
                    if (key.IsString() && Get(field, "name") == Text(key)) {
                        group_ids.PushBack(Copy(field["field_id"], a), a);
                        found = true;
                    }
                closed &= found;
            }
            if (closed) Set(dictionary["entity"], "identity_fields", Semantic(group_ids, a), a);
        }
        PruneEntity(dictionary, preview["semantic_gaps"], a);
        Value expected(rapidjson::kArrayType);
        for (const auto& f : dictionary["fields"].GetArray()) {
            Value physical(rapidjson::kObjectType);
            for (const char* k : {"name", "type", "nullable"}) physical.AddMember(Value(k, a), Copy(f[k], a), a);
            expected.PushBack(physical, a);
        }
        preview.AddMember("expected_fields", expected.Empty() && !structure ? Value() : std::move(expected), a);
        preview.AddMember("dictionary", dictionary, a);
        if (port.HasMember("row_cases")) preview.AddMember("row_cases", Copy(port["row_cases"], a), a);
        if (port.HasMember("context_semantics")) {
            preview.AddMember("context_template", Copy(port["context_semantics"], a), a);
            if (Contexts(registry, port["context_semantics"], invocation, role_map, preview, a, &budget, error))
                return -1;
        }
        previews->PushBack(preview, a);
    }
    return 0;
}

int OperatorContractService::RecordUse(const char* text, OperatorContractSnapshotV1* out, DataSemanticsErrorV1* e) {
    if (out) *out = {};
    if (e) *e = {};
    if (!out) return Fail(e, "CONTRACT_INVALID", "observation output required");
    Json request;
    if (ParseSemanticsJson(text ? text : "", &request, e)) return -1;
    Json schema;
    schema.Parse(kSemanticsApiSchema);
    Set(schema, "$ref", "#/%24defs/operatorObservationRequest", schema.GetAllocator());
    rapidjson::SchemaDocument compiled(schema);
    rapidjson::SchemaValidator validator(compiled);
    if (!request.Accept(validator)) return Fail(e, "CONTRACT_INVALID", "invalid output observation");
    Json document;
    document.CopyFrom(request["document"], document.GetAllocator());
    auto& a = document.GetAllocator();
    OperatorContractSnapshotV1 contract_snapshot;
    if (Resolve(Get(document, "contract_ref").c_str(), &contract_snapshot, e)) return -1;
    Json contract;
    contract.Parse(contract_snapshot.document_json->c_str());
    if (document["operator_ref"] != contract["operator"])
        return Fail(e, "COMPONENT_CHANGED", "observed operator differs from declaration");
    Json invocation(rapidjson::kObjectType);
    auto& ia = invocation.GetAllocator();
    for (const char* k : {"contract_ref", "component_revision", "effective_parameters", "normalized_config",
                          "config_refs", "execution_context"})
        if (document.HasMember(k)) invocation.AddMember(Value(k, ia), Copy(document[k], ia), ia);
    invocation.AddMember("inputs", Copy(document["input_snapshots"], ia), ia);
    std::string checked;
    if (CheckAndDerive(Encode(invocation).c_str(), &checked, e)) return -1;
    Json checks;
    checks.Parse(checked.c_str());
    if (checks["state"] == "incompatible")
        return Fail(e, "CONTRACT_INCOMPATIBLE", "observed invocation is incompatible");
    Set(document, "checks", Copy(checks, a), a);
    Set(document, "role_map", Copy(checks["role_map"], a), a);
    Value outputs(rapidjson::kArrayType);
    std::vector<SemanticsSaveRequest> writes;
    std::set<std::string> seen;
    for (const auto& actual : document["outputs"].GetArray())
        if (!seen.insert(Get(actual, "port_id")).second)
            return Fail(e, "CONTRACT_INVALID", "duplicate observed output port");
    for (const auto& preview : checks["output_previews"].GetArray()) {
        const auto port = Get(preview, "port_id");
        const Value* actual = nullptr;
        // Output arrays use port_id rather than field name.
        for (const auto& item : document["outputs"].GetArray())
            if (Get(item, "port_id") == port) actual = &item;
        seen.erase(port);
        Value output(preview, a);
        const auto state = actual ? Get(*actual, "state") : "not_observed";
        Set(output, "state", state, a);
        if (state == "absent") {
            if (Get(preview, "availability") == "available")
                return Fail(e, "OUTPUT_SCHEMA_MISMATCH", "required output was absent", "/outputs/" + port);
        } else if (state == "observed") {
            const auto& snapshot = At(*actual, "snapshot");
            Json snapshot_schema;
            snapshot_schema.Parse(kSemanticsApiSchema);
            Set(snapshot_schema, "$ref", "#/%24defs/datasetSnapshot", snapshot_schema.GetAllocator());
            rapidjson::SchemaDocument snapshot_compiled(snapshot_schema);
            rapidjson::SchemaValidator snapshot_validator(snapshot_compiled);
            if (!snapshot.Accept(snapshot_validator) || !snapshot.IsObject() ||
                Get(snapshot, "schema_state") != "known" || !At(snapshot, "fields").IsArray() ||
                Get(snapshot, "schema_fingerprint") != Hash(Canonical(snapshot["fields"])))
                return Fail(e, "CONTRACT_INVALID", "observed output requires a complete real snapshot",
                            "/outputs/" + port);
            if (Get(preview, "availability") == "absent" ||
                (!preview["expected_fields"].IsNull() && preview["expected_fields"] != snapshot["fields"]))
                return Fail(e, "OUTPUT_SCHEMA_MISMATCH", "actual output differs from declaration", "/outputs/" + port);
            Value dictionary(preview["dictionary"], a);
            if (preview["expected_fields"].IsNull()) {
                dictionary["fields"].Clear();
                for (const auto& f : snapshot["fields"].GetArray())
                    dictionary["fields"].PushBack(
                        OutputField(f, "f_" + Hash(port + "/" + Get(f, "name")).substr(0, 32), a), a);
            }
            Json normalized;
            std::vector<SemanticsReference> refs;
            if (NormalizeSemantics("dictionary", Encode(dictionary), &normalized, &refs, e)) return -1;
            dictionary.CopyFrom(normalized, a);
            const auto dictionary_name = "out_" + Hash(Canonical(dictionary));
            const auto dictionary_ref = "dictionary." + dictionary_name + "@1";
            writes.push_back({"dictionary", dictionary_name, Encode(dictionary)});
            Value binding(rapidjson::kObjectType), map(rapidjson::kObjectType), confirmation(rapidjson::kObjectType);
            Set(binding, "schema_id", "flowsql.data-binding.v2", a);
            Set(binding, "object_label", port, a);
            Set(binding, "dictionary_ref", dictionary_ref, a);
            for (const auto& f : dictionary["fields"].GetArray())
                Set(map, Get(f, "field_id").c_str(), Get(f, "name"), a);
            binding.AddMember("object_key", Copy(snapshot["object_key"], a), a)
                .AddMember("source_snapshot", Copy(snapshot, a), a)
                .AddMember("field_map", map, a)
                .AddMember("declared_schema", Copy(snapshot["fields"], a), a);
            Set(confirmation, "source_revision", Get(snapshot, "source_revision"), a);
            confirmation.AddMember("items", Value(rapidjson::kArrayType), a);
            binding.AddMember("confirmation", confirmation, a);
            std::string binding_name = "out_" + Hash(Canonical(snapshot["object_key"]));
            uint64_t current = 0;
            DataSemanticsSnapshotV1 previous;
            if (registry_.BindingLookup(Get(snapshot["object_key"], "provider_id"),
                                        Get(snapshot["object_key"], "object_id"), &previous, e) == 0) {
                SemanticsIdentity identity;
                ParseSemanticsReference(previous.exact_reference, &identity);
                binding_name = identity.name;
                current = identity.revision;
                Json prior;
                prior.Parse(previous.document_json->c_str());
                if (Canonical(prior) == Canonical(binding)) --current;
            } else if (!e || e->code != "NOT_FOUND")
                return -1;
            if (e) *e = {};
            SemanticsSaveRequest save{"binding", binding_name, Encode(binding)};
            save.expected_current_revision = current;
            save.source_observed = true;
            writes.push_back(save);
            Set(output, "dictionary_ref", dictionary_ref, a);
            Set(output, "binding_ref", "binding." + binding_name + "@" + std::to_string(current + 1), a);
            output.AddMember("snapshot", Copy(snapshot, a), a);
            if (actual->HasMember("row_count")) output.AddMember("row_count", Copy((*actual)["row_count"], a), a);
            Set(output, "output_check", "matched", a);
        } else if (state != "not_observed")
            return Fail(e, "CONTRACT_INVALID", "invalid output observation state");
        output.RemoveMember("dictionary");
        outputs.PushBack(output, a);
    }
    if (!seen.empty()) return Fail(e, "CONTRACT_INVALID", "undeclared output port");
    document["outputs"] = std::move(outputs);
    SemanticsSaveRequest use{"operator_use", Get(request, "name"), Encode(document)};
    use.expected_current_revision = request["expected_current_revision"].GetUint64();
    writes.push_back(use);
    std::vector<SemanticsSaveResult> saved;
    if (registry_.SaveBatch(writes, &saved, e)) return -1;
    const auto& snapshot = saved.back().snapshot;
    *out = {snapshot.exact_reference, snapshot.sha256, snapshot.document_json};
    return 0;
}
}  // namespace flowsql::channels::config
