// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <framework/interfaces/ioperator_output_observation.h>
#include <cerrno>
#include <set>
#include "operator_contract_execution.hpp"
namespace flowsql::contracts {
inline dataset::Value ObservedOutput(const std::string& port, const std::shared_ptr<arrow::Schema>& schema,
                                     const std::string& identity, int64_t rows, dataset::Alloc& a) {
    using namespace dataset;
    Value output(rapidjson::kObjectType);
    Set(output, "port_id", port, a);
    Set(output, "state", schema ? "observed" : "not_observed", a);
    if (schema) {
        Value locator(rapidjson::kObjectType), metadata(rapidjson::kObjectType);
        Set(locator, "kind", "dataframe", a);
        Set(locator, "name", identity, a);
        auto snapshot = Snapshot("operator_output", Hash(identity), locator, ArrowFields(*schema, a), metadata, "1");
        output.AddMember("snapshot", Copy(snapshot, a), a);
        if (rows >= 0) output.AddMember("row_count", rows, a);
    }
    return output;
}
inline int ProducerOutputs(IOperatorOutputObservationV1* producer, dataset::Json* outputs, DataSemanticsErrorV1* e) {
    outputs->SetArray();
    if (!producer) return 0;
    std::string text;
    const int rc = producer->OutputObservations(&text);
    if (rc == ENOTSUP) return 0;
    if (rc || text.size() > 12 * 1024 * 1024 || !JsonNestingWithin(text, 64))
        return dataset::Fail(e, "CONTRACT_INVALID", "producer output observations unavailable or too large");
    outputs->Parse(text.c_str());
    if (!outputs->IsArray())
        return dataset::Fail(e, "CONTRACT_INVALID", "producer output observations must be an array");
    std::set<std::string> ports;
    for (const auto& output : outputs->GetArray())
        if (dataset::Get(output, "port_id").empty() || !ports.insert(dataset::Get(output, "port_id")).second)
            return dataset::Fail(e, "CONTRACT_INVALID", "producer output port must be nonempty and unique");
    return 0;
}
inline int Observe(IQuerier* querier, Invocation* invocation, const dataset::Value& actual, const std::string& outcome,
                   DataSemanticsErrorV1* e) {
    using namespace dataset;
    if (invocation->component.contract_state != "declared") return 0;
    auto& check = invocation->check;
    auto& ca = check.GetAllocator();
    Set(check, "data_state", "produced", ca);
    auto failure = [&] {
        Set(check, "metadata_state", "failed", ca);
        Value issue(rapidjson::kObjectType);
        Set(issue, "code", e->code, ca);
        Set(issue, "message", e->message, ca);
        Set(check, "metadata_error", std::move(issue), ca);
        if (e->code != "OUTPUT_SCHEMA_MISMATCH") e->code = "METADATA_PUBLICATION_FAILED";
        return -1;
    };
    auto* service =
        querier ? static_cast<IOperatorDataContractV1*>(querier->First(IID_OPERATOR_DATA_CONTRACT_V1)) : nullptr;
    if (!service) {
        Fail(e, "UNAVAILABLE", "output metadata service unavailable");
        return failure();
    }
    auto& resolved = invocation->resolved;
    auto& context = resolved["execution_context"];
    if (Get(context, "contract_run_id").empty()) Set(context, "contract_run_id", Token(), resolved.GetAllocator());
    const auto run = Get(context, "contract_run_id");
    const auto stage = context.HasMember("contract_stage_index") ? context["contract_stage_index"].GetUint() : 0;
    Json request(rapidjson::kObjectType);
    auto& a = request.GetAllocator();
    Set(request, "name", "r_" + run + "_s_" + std::to_string(stage), a);
    uint64_t revision = 0;
    const auto ref = Get(check, "use_ref");
    if (!ref.empty()) revision = std::stoull(ref.substr(ref.rfind('@') + 1));
    request.AddMember("expected_current_revision", revision, a);
    Value document(rapidjson::kObjectType), identity(rapidjson::kObjectType);
    Set(document, "schema_id", "flowsql.operator-data-use.v1", a);
    Set(document, "title", "Observed operator stage", a);
    Set(document, "run_id", run, a);
    document.AddMember("stage_index", stage, a);
    Set(identity, "category", invocation->component.category, a);
    Set(identity, "name", invocation->component.name, a);
    Set(identity, "version", invocation->component.operator_version, a);
    document.AddMember("operator_ref", identity, a);
    for (const char* k : {"contract_ref", "component_revision", "effective_parameters", "normalized_config",
                          "config_refs", "execution_context"})
        if (resolved.HasMember(k)) document.AddMember(Value(k, a), Copy(resolved[k], a), a);
    document.AddMember("input_snapshots", Copy(resolved["inputs"], a), a)
        .AddMember("role_map", Copy(check["role_map"], a), a)
        .AddMember("checks", Copy(check, a), a);
    Value outputs(rapidjson::kArrayType);
    for (const auto& preview : check["output_previews"].GetArray()) {
        const auto port = Get(preview, "port_id");
        const Value* observed = nullptr;
        for (const auto& item : actual.GetArray())
            if (Get(item, "port_id") == port) observed = &item;
        if (observed)
            outputs.PushBack(Copy(*observed, a), a);
        else {
            Value absent(rapidjson::kObjectType);
            Set(absent, "port_id", port, a);
            Set(absent, "state", Get(preview, "availability") == "absent" ? "absent" : "not_observed", a);
            outputs.PushBack(absent, a);
        }
    }
    // Preserve undeclared ports so the trusted service rejects them explicitly.
    for (const auto& item : actual.GetArray()) {
        bool declared = false;
        for (const auto& preview : check["output_previews"].GetArray())
            declared |= Get(preview, "port_id") == Get(item, "port_id");
        if (!declared) outputs.PushBack(Copy(item, a), a);
    }
    document.AddMember("outputs", outputs, a);
    Set(document, "outcome", outcome, a);
    request.AddMember("document", document, a);
    OperatorContractSnapshotV1 recorded;
    if (service->RecordUse(Encode(request).c_str(), &recorded, e)) return failure();
    Json used;
    used.Parse(recorded.document_json->c_str());
    Set(check, "use_ref", recorded.exact_reference, ca);
    Set(check, "outputs", Copy(used["outputs"], ca), ca);
    Set(check, "metadata_state", "persisted", ca);
    bool observed = false;
    for (const auto& output : used["outputs"].GetArray()) observed |= Get(output, "state") == "observed";
    Set(check, "output_check", observed ? "matched" : "not_observed", ca);
    return 0;
}
inline int ObservePrimary(IQuerier* querier, Invocation* invocation, const std::shared_ptr<arrow::Schema>& schema,
                          int64_t rows, IOperatorOutputObservationV1* producer, const std::string& outcome,
                          DataSemanticsErrorV1* e) {
    using namespace dataset;
    if (invocation->component.contract_state != "declared") return 0;
    auto& context = invocation->resolved["execution_context"];
    if (Get(context, "contract_run_id").empty())
        Set(context, "contract_run_id", Token(), invocation->resolved.GetAllocator());
    Json actual;
    if (ProducerOutputs(producer, &actual, e)) return -1;
    const auto port = Get(invocation->check["output_previews"][0], "port_id");
    bool provided = false;
    for (const auto& output : actual.GetArray()) provided |= Get(output, "port_id") == port;
    const auto stage = context.HasMember("contract_stage_index") ? context["contract_stage_index"].GetUint() : 0;
    if (!provided)
        actual.PushBack(
            ObservedOutput(port, schema, Get(context, "contract_run_id") + "/" + std::to_string(stage) + "/" + port,
                           rows, actual.GetAllocator()),
            actual.GetAllocator());
    return Observe(querier, invocation, actual, outcome, e);
}
// Called after a named result is registered. F2 supplies the new object's identity.
inline int BindNamedOutput(IQuerier* querier, const std::string& name, std::string* checks_json,
                           DataSemanticsErrorV1* e, const std::string& port = "") {
    using namespace dataset;
    Json checks;
    checks.Parse(checks_json->c_str());
    if (!checks.IsArray() || checks.Empty() || Get(checks[checks.Size() - 1], "use_ref").empty()) return 0;
    auto& check = checks[checks.Size() - 1];
    auto failed = [&] {
        Set(check, "data_state", "produced", checks.GetAllocator());
        Set(check, "metadata_state", "failed", checks.GetAllocator());
        Value issue(rapidjson::kObjectType);
        Set(issue, "code", e->code, checks.GetAllocator());
        Set(issue, "message", e->message, checks.GetAllocator());
        Set(check, "metadata_error", std::move(issue), checks.GetAllocator());
        *checks_json = Encode(checks);
        if (e->code != "OUTPUT_SCHEMA_MISMATCH") e->code = "METADATA_PUBLICATION_FAILED";
        return -1;
    };
    auto* registry =
        querier ? static_cast<IDataSemanticsRegistryV1*>(querier->First(IID_DATA_SEMANTICS_REGISTRY_V1)) : nullptr;
    auto* service =
        querier ? static_cast<IOperatorDataContractV1*>(querier->First(IID_OPERATOR_DATA_CONTRACT_V1)) : nullptr;
    if (!registry || !service) {
        Fail(e, "UNAVAILABLE", "named output metadata service unavailable");
        return failed();
    }
    DataSemanticsSnapshotV1 previous;
    if (registry->Resolve(Get(check, "use_ref").c_str(), &previous, e)) return failed();
    Json document;
    document.Parse(previous.document_json->c_str());
    Json locator(rapidjson::kObjectType), snapshot;
    std::string binding;
    Set(locator, "kind", "dataframe", locator.GetAllocator());
    Set(locator, "name", name, locator.GetAllocator());
    if (DescribeInput(querier, locator, nullptr, &snapshot, &binding, e)) return failed();
    auto& a = document.GetAllocator();
    Value* selected = nullptr;
    for (auto& output : document["outputs"].GetArray())
        if (port.empty() ? &output == &document["outputs"][0] : Get(output, "port_id") == port) selected = &output;
    if (!selected || Get(*selected, "state") != "observed") {
        Fail(e, "CONTRACT_INVALID", "named output has no producer observation");
        return failed();
    }
    Set(*selected, "snapshot", Copy(snapshot, a), a);
    Json request(rapidjson::kObjectType);
    auto& ra = request.GetAllocator();
    const auto exact = previous.exact_reference;
    const auto dot = exact.find('.'), at = exact.rfind('@');
    Set(request, "name", exact.substr(dot + 1, at - dot - 1), ra);
    request.AddMember("expected_current_revision", static_cast<uint64_t>(std::stoull(exact.substr(at + 1))), ra)
        .AddMember("document", Copy(document, ra), ra);
    OperatorContractSnapshotV1 recorded;
    if (service->RecordUse(Encode(request).c_str(), &recorded, e)) return failed();
    Json used;
    used.Parse(recorded.document_json->c_str());
    Set(check, "use_ref", recorded.exact_reference, checks.GetAllocator());
    Set(check, "outputs", Copy(used["outputs"], checks.GetAllocator()), checks.GetAllocator());
    *checks_json = Encode(checks);
    return 0;
}
}  // namespace flowsql::contracts
