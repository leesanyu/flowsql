// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include "npm_result_semantics.h"
#include "npm_result_dictionary.h"

#include <framework/interfaces/ioperator_data_contract.h>
#include <common/operator_contract_control.hpp>
#include "npm_family_result_contract.h"

#include <set>

namespace flowsql::npm {
namespace {
using namespace dataset;
Value Locator(const std::string& category, const std::string& name, const std::string& table, Alloc& a) {
    Value out(rapidjson::kObjectType);
    Set(out, "kind", "database_table", a);
    Set(out, "type", category, a);
    Set(out, "name", name, a);
    Set(out, "table", table, a);
    return out;
}
void PreserveManual(const Value& old, Value& next, Alloc& a) {
    for (const auto& item : old.GetObject()) {
        const auto& value = item.value;
        if (value.IsObject() && value.HasMember("state") && Get(value, "origin") != "producer" &&
            Get(value, "origin") != "derived")
            Set(next, item.name.GetString(), Copy(value, a), a);
    }
}
const Value* NamedField(const Value& fields, const std::string& name) {
    for (const auto& field : fields.GetArray())
        if (Get(field, "name") == name) return &field;
    return nullptr;
}
int Bind(IQuerier* querier, const Value& locator, const Value* domain, const std::string& view,
         const std::string& marker, const std::string& run_dictionary, const std::string& session_target,
         Value* relation, Alloc& a, DataSemanticsErrorV1* error) {
    Json request(rapidjson::kObjectType), preview;
    request.AddMember("locator", Copy(locator, request.GetAllocator()), request.GetAllocator());
    if (contracts::Control(querier, "channels/preview", request, &preview, error)) return -1;
    const auto& data = preview["data"];
    const auto& source = data["source_snapshot"];
    if (Get(source, "schema_state") != "known")
        return Fail(error, "SOURCE_UNAVAILABLE", "NPM relation structure must be known");
    const bool bound = data["binding_ref"].IsString();
    const auto& old = data["dictionary"];
    if (bound) {
        Json check;
        if (contracts::Control(querier, "channels/check", request, &check, error)) return -1;
        const auto& metadata = source["producer_metadata"];
        const bool same_component =
            Get(metadata, "default_contract_ref") + " / " + Get(metadata, "default_component_revision") == marker;
        if (Get(check["data"], "state") != "matched" || !same_component)
            return Fail(error, "SOURCE_CHANGED", "existing NPM description requires source/component review");
    }
    if (domain && source["fields"].Size() != (*domain)["fields"].Size() + 3)
        return Fail(error, "SOURCE_CHANGED", "managed relation differs from the created entity structure");
    Json dictionary = result_dictionary::Build(domain, source["fields"], view, marker, run_dictionary, session_target);
    auto& da = dictionary.GetAllocator();
    Set(dictionary, "title", Get(locator, "table"), da);
    Value field_map(rapidjson::kObjectType);
    if (bound) {
        PreserveManual(old, dictionary, da);
        PreserveManual(old["entity"], dictionary["entity"], da);
    }
    for (auto& field : dictionary["fields"].GetArray()) {
        const auto name = Get(field, "name");
        if (domain && !NamedField((*domain)["fields"], name) && name != "__npm_run_id" && name != "__npm_task_id" &&
            name != "__npm_run_status")
            return Fail(error, "SOURCE_CHANGED", "unexpected field in NPM public relation", name);
        if (bound) {
            const auto* previous = NamedField(old["fields"], name);
            if (previous) {
                if (Get(*previous, "field_id") != name)
                    return Fail(error, "SOURCE_CHANGED", "existing field identity requires review", name);
                PreserveManual(*previous, field, da);
            }
        }
        Set(field_map, name.c_str(), name, da);
    }
    // Immutable content-addressed versions allow retries and preserve all prior
    // references; the ordinary editor still creates its own object-local copies.
    Json save(rapidjson::kObjectType), saved;
    auto& sa = save.GetAllocator();
    Set(save, "kind", "dictionary", sa);
    Set(save, "name", "npm." + Hash(Canonical(source["object_key"]) + Canonical(dictionary)), sa);
    save.AddMember("expected_current_revision", uint64_t{0}, sa).AddMember("document", Copy(dictionary, sa), sa);
    if (contracts::Control(querier, "save", save, &saved, error)) return -1;
    const auto exact = Get(saved["data"], "exact_reference");
    Json apply(rapidjson::kObjectType), applied;
    auto& aa = apply.GetAllocator();
    apply.AddMember("locator", Copy(locator, aa), aa);
    Set(apply, "dictionary_ref", exact, aa);
    apply.AddMember("field_map", Copy(field_map, aa), aa);
    uint64_t revision = 0;
    std::string binding_name = "npm." + Hash(Canonical(source["object_key"]));
    if (bound) {
        const auto ref = Get(data, "binding_ref");
        const auto separator = ref.rfind('@');
        revision = std::stoull(ref.substr(separator + 1));
        binding_name = ref.substr(8, separator - 8);  // Exact reference prefix: binding.
    }
    Set(apply, "name", binding_name, aa);
    apply.AddMember("expected_current_revision", revision, aa)
        .AddMember("confirmations", Copy(data["required_confirmations"], aa), aa);
    Value basis(rapidjson::kObjectType);
    for (const char* key : {"object_key", "source_revision", "schema_fingerprint"})
        basis.AddMember(Value(key, aa), Copy(source[key], aa), aa);
    apply.AddMember("source_basis", basis, aa);
    if (contracts::Control(querier, "channels/apply", apply, &applied, error)) return -1;
    relation->SetObject();
    Set(*relation, "view", view, a);
    relation->AddMember("locator", Copy(locator, a), a);
    Set(*relation, "dictionary_ref", exact, a);
    Set(*relation, "binding_ref", Get(applied["data"], "exact_reference"), a);
    return 0;
}
}  // namespace

int PublishNpmResultSemantics(IQuerier* querier, const OperatorComponentContractV1& component,
                              const std::string& category, const std::string& name, const std::string& input_namespace,
                              const std::string& actual_result_json, std::string* context_json,
                              DataSemanticsErrorV1* error) {
    context_json->clear();
    if (error) *error = {};
    auto* service =
        querier ? static_cast<IOperatorDataContractV1*>(querier->First(IID_OPERATOR_DATA_CONTRACT_V1)) : nullptr;
    if (!service) return Fail(error, "UNAVAILABLE", "NPM contract service unavailable");
    OperatorContractSnapshotV1 snapshot;
    if (service->Resolve(component.contract_ref.c_str(), &snapshot, error)) return -1;
    Json contract, result;
    contract.Parse(snapshot.document_json->c_str());
    result.Parse(actual_result_json.c_str());
    if (!result.IsObject() || Get(result, "run_id").empty() || !result.HasMember("entities") ||
        !result["entities"].IsArray())
        return Fail(error, "CONTRACT_INVALID", "NPM publication requires this run's managed catalog");
    const auto marker = component.contract_ref + " / " + component.component_revision;
    Json context(rapidjson::kObjectType);
    auto& a = context.GetAllocator();
    Set(context, "run_id", Get(result, "run_id"), a);
    Set(context, "input_namespace", input_namespace, a);
    Value run(rapidjson::kObjectType);
    if (Bind(querier, Locator(category, name, "npm_result_runs", a), nullptr, "runs", marker, "", "", &run, a, error))
        return -1;
    context.AddMember("run_catalog", Copy(run, a), a);
    Value entities(rapidjson::kArrayType);
    for (const auto& item : result["entities"].GetArray()) {
        const auto id = Get(item, "entity_id");
        Value entity(rapidjson::kObjectType), relations(rapidjson::kArrayType);
        Set(entity, "entity_id", id, a);
        entity.AddMember("schema_version", Copy(item["schema_version"], a), a);
        if (!contract["resources"].HasMember(id.c_str()) ||
            !contract["resources"][id.c_str()].HasMember("dictionary")) {
            Set(entity, "description_state", "pending", a);
        } else {
            const auto& dictionary = contract["resources"][id.c_str()]["dictionary"];
            for (const char* view : {"history", "latest", "final"}) {
                const auto table = Get(item, (std::string(view) + "_relation").c_str());
                if (table.empty()) return Fail(error, "CONTRACT_INVALID", "actual NPM relation missing", id);
                Value relation(rapidjson::kObjectType);
                std::string target_id, target_ref;
                for (const auto family : {packet::AddressFamily::kIPv4, packet::AddressFamily::kIPv6}) {
                    const auto basic = NpmFamilyEntityDescriptor(NpmFamilyEntityKind::kBasic, family).entity_id;
                    const auto session = NpmFamilyEntityDescriptor(NpmFamilyEntityKind::kSession, family).entity_id;
                    const auto dns = NpmFamilyEntityDescriptor(NpmFamilyEntityKind::kDns, family).entity_id;
                    if (id == session) target_id = basic;
                    if (id == dns) target_id = session;
                    for (const auto kind : {NpmFamilyEntityKind::kHttp1, NpmFamilyEntityKind::kTls})
                        if (id == NpmFamilyEntityDescriptor(kind, family).entity_id) target_id = session;
                }
                // The managed catalog is produced in module dependency order. Only
                // this execution's successfully published targets are eligible.
                for (const auto& published : entities.GetArray())
                    if (Get(published, "entity_id") == target_id)
                        for (const auto& target : published["relations"].GetArray())
                            if (Get(target, "view") == view) target_ref = Get(target, "dictionary_ref");
                if (Bind(querier, Locator(category, name, table, a), &dictionary, view, marker,
                         Get(run, "dictionary_ref"), target_ref, &relation, a, error))
                    return -1;
                relations.PushBack(relation, a);
            }
            Set(entity, "description_state", "published", a);
        }
        entity.AddMember("relations", relations, a);
        entities.PushBack(entity, a);
    }
    context.AddMember("entities", entities, a);
    *context_json = Encode(context);
    return 0;
}
}  // namespace flowsql::npm
