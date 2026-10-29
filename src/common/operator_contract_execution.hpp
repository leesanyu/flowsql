// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <framework/interfaces/ioperator_contract_catalog.h>
#include <framework/interfaces/ioperator_data_contract.h>
#include <common/channel_dataset_arrow.hpp>
#include <common/operator_contract_control.hpp>

namespace flowsql::contracts {
struct Invocation {
    OperatorComponentContractV1 component;
    dataset::Json resolved, check;
};
inline int Component(IQuerier* querier, IOperatorContractIdentityV1* identity, const std::string& category,
                     const std::string& name, OperatorComponentContractV1* out, DataSemanticsErrorV1* error) {
    *out = {};
    if (identity) {
        *out = identity->ContractIdentity();
    } else if (querier) {
        auto* directory = static_cast<IOperatorContractCatalogV1*>(querier->First(IID_OPERATOR_CONTRACT_CATALOG_V1));
        if (directory && directory->Lookup(category.c_str(), name.c_str(), out, error)) return -1;
        if (out->contract_state == "declared")
            return dataset::Fail(error, "COMPONENT_CHANGED", "operator instance has no frozen component identity");
    }
    if (out->contract_state == "unavailable")
        return dataset::Fail(error, "UNAVAILABLE", "declared operator contract is unavailable");
    if (out->contract_state == "declared" && (out->contract_ref.empty() || out->component_revision.empty()))
        return dataset::Fail(error, "CONTRACT_INVALID", "incomplete component contract identity");
    if (out->category.empty()) out->category = category;
    if (out->name.empty()) out->name = name;
    return 0;
}
inline int CheckInvocation(IQuerier* querier, Invocation* invocation, IOperatorConfigAdapterV1* adapter,
                           const dataset::Value& parameters, const dataset::Value& inputs,
                           const dataset::Value& execution_context, DataSemanticsErrorV1* error) {
    using namespace dataset;
    auto& component = invocation->component;
    invocation->resolved.SetObject();
    auto& a = invocation->resolved.GetAllocator();
    invocation->resolved.AddMember("effective_parameters", Copy(parameters, a), a)
        .AddMember("inputs", Copy(inputs, a), a)
        .AddMember("config_refs", Value(rapidjson::kArrayType), a)
        .AddMember("execution_context", Copy(execution_context, a), a);
    if (adapter) {
        Json request(rapidjson::kObjectType);
        auto& ra = request.GetAllocator();
        Value identity(rapidjson::kObjectType);
        Set(identity, "category", component.category, ra);
        Set(identity, "name", component.name, ra);
        request.AddMember("operator", identity, ra)
            .AddMember("parameters", Copy(parameters, ra), ra)
            .AddMember("inputs", Copy(inputs, ra), ra)
            .AddMember("execution_context", Copy(execution_context, ra), ra);
        std::string text;
        if (adapter->ResolveInvocation(Encode(request).c_str(), &text, error)) return -1;
        Json adapted;
        if (text.size() > 12 * 1024 * 1024 || !JsonNestingWithin(text, 64))
            return Fail(error, "LIMIT_EXCEEDED", "invocation adapter response limit");
        adapted.Parse(text.c_str());
        if (!adapted.IsObject() || !adapted.HasMember("effective_parameters") || !adapted.HasMember("inputs") ||
            !adapted.HasMember("config_refs"))
            return Fail(error, "CONTRACT_INVALID", "invalid adapter response");
        for (const char* key :
             {"effective_parameters", "inputs", "config_refs", "normalized_config", "execution_context"})
            if (adapted.HasMember(key)) Set(invocation->resolved, key, Copy(adapted[key], a), a);
    }
    invocation->check.SetObject();
    if (component.contract_state != "declared") {
        auto& ca = invocation->check.GetAllocator();
        Set(invocation->check, "state", "undetermined", ca);
        Set(invocation->check, "data_validation", "not_run", ca);
        Set(invocation->check, "output_check", "not_observed", ca);
        Set(invocation->check, "contract_state", "missing", ca);
        return 0;
    }
    Set(invocation->resolved, "contract_ref", component.contract_ref, a);
    Set(invocation->resolved, "component_revision", component.component_revision, a);
    auto* service =
        querier ? static_cast<IOperatorDataContractV1*>(querier->First(IID_OPERATOR_DATA_CONTRACT_V1)) : nullptr;
    if (!service) return Fail(error, "UNAVAILABLE", "declared operator contract service unavailable");
    std::string checked;
    if (service->CheckAndDerive(Encode(invocation->resolved).c_str(), &checked, error)) return -1;
    invocation->check.Parse(checked.c_str());
    if (!invocation->check.IsObject()) return Fail(error, "UNAVAILABLE", "invalid contract check response");
    Set(invocation->check, "output_check", "not_observed", invocation->check.GetAllocator());
    if (Get(invocation->check, "state") == "incompatible")
        return Fail(error, "CONTRACT_INCOMPATIBLE", "input violates the declared operator contract");
    return 0;
}
// Reads metadata only. Real provider snapshots are retained; transient stages get a
// distinct identity and no borrowed binding from an unrelated same-named object.
inline int DescribeInput(IQuerier* querier, const dataset::Value& locator, const std::shared_ptr<arrow::Schema>& schema,
                         dataset::Json* snapshot, std::string* binding_ref, DataSemanticsErrorV1* error) {
    using namespace dataset;
    binding_ref->clear();
    if (querier && !locator.IsNull()) {
        Json request(rapidjson::kObjectType), response;
        request.AddMember("locator", Copy(locator, request.GetAllocator()), request.GetAllocator());
        if (Control(querier, "channels/describe", request, &response, error)) return -1;
        snapshot->CopyFrom(response["data"]["source_snapshot"], snapshot->GetAllocator());
        if (schema && Get(*snapshot, "schema_state") == "known") {
            Json fields;
            const auto actual = ArrowFields(*schema, fields.GetAllocator());
            if ((*snapshot)["fields"] != actual)
                return Fail(error, "SOURCE_CHANGED", "actual stage input differs from described source");
        }
        if (Control(querier, "channels/check", request, &response, error)) return -1;
        *binding_ref = Get(response["data"], "binding_ref");
        return 0;
    }
    Json owner;
    auto& a = owner.GetAllocator();
    Value fields = schema ? ArrowFields(*schema, a) : Value();
    Value metadata(rapidjson::kObjectType), temporary(rapidjson::kObjectType);
    Set(temporary, "kind", "dataframe", a);
    Set(temporary, "name", "temporary", a);
    *snapshot =
        Snapshot("operator_stage", Token(), temporary, fields, metadata, "", "known", schema ? "known" : "pending");
    return 0;
}
}  // namespace flowsql::contracts
