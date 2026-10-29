// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <common/error_code.h>
#include <framework/interfaces/iblock_transform_dataframe_input.h>
#include <algorithm>
#include <common/operator_contract_execution.hpp>
#include <set>
#include "scheduler_plugin.h"

namespace flowsql::scheduler {
namespace {
using namespace dataset;
bool Keys(const Value& v, std::initializer_list<const char*> allowed) {
    if (!v.IsObject()) return false;
    std::set<std::string> names;
    for (const auto& m : v.GetObject())
        if (!names.insert(Text(m.name)).second ||
            std::none_of(allowed.begin(), allowed.end(), [&](const char* k) { return m.name == k; }))
            return false;
    return true;
}
std::string Source(const Value& locator) {
    const auto kind = Get(locator, "kind"), name = Get(locator, "name");
    if (kind == "dataframe") return "dataframe." + name;
    if (kind == "kafka_source") return "kafka." + name;
    return Get(locator, "type") + "." + name + "." +
           (Get(locator, "schema").empty() ? "" : Get(locator, "schema") + ".") + Get(locator, "table");
}
int Failure(const DataSemanticsErrorV1& error, std::string* response) {
    Json body(rapidjson::kObjectType);
    auto& a = body.GetAllocator();
    Value issue(rapidjson::kObjectType);
    Set(issue, "code", error.code, a);
    Set(issue, "message", error.message, a);
    Set(issue, "field_path", error.field_path, a);
    issue.AddMember("details", Value(rapidjson::kObjectType), a);
    body.AddMember("error", issue, a);
    *response = Encode(body);
    if (error.code == "UNAVAILABLE" || error.code == "SOURCE_UNAVAILABLE") return error::UNAVAILABLE;
    if (error.code == "COMPONENT_CHANGED" || error.code == "SOURCE_CHANGED" || error.code == "REVISION_CONFLICT")
        return error::CONFLICT;
    if (error.code == "NOT_FOUND") return error::NOT_FOUND;
    if (error.code == "LIMIT_EXCEEDED") return error::PAYLOAD_TOO_LARGE;
    return error::BAD_REQUEST;
}
}  // namespace
int32_t SchedulerPlugin::HandleOperatorContractCheck(const std::string&, const std::string& text, std::string& rsp) {
    using namespace dataset;
    if (text.size() > 12 * 1024 * 1024) return Failure({"LIMIT_EXCEEDED", "request exceeds 12 MiB", ""}, &rsp);
    Json request;
    if (!JsonNestingWithin(text, 64) || request.Parse(text.c_str()).HasParseError() ||
        !Keys(request, {"operator", "parameters", "inputs", "output_locator"}) || !request.HasMember("operator") ||
        !Keys(request["operator"], {"category", "name"}) || Get(request["operator"], "category").empty() ||
        Get(request["operator"], "name").empty() || !request.HasMember("parameters") ||
        !request["parameters"].IsObject() || !request.HasMember("inputs") || !request["inputs"].IsArray() ||
        request["inputs"].Empty() || request["inputs"].Size() > 1024)
        return Failure(
            {"CONTRACT_INVALID", "expected operator, existing string parameters and actual input locators", ""}, &rsp);
    for (const auto& parameter : request["parameters"].GetObject())
        if (!parameter.value.IsString())
            return Failure({"CONTRACT_INVALID", "algorithm parameters must match SQL string values", "/parameters"},
                           &rsp);
    for (rapidjson::SizeType index = 0; index < request["inputs"].Size(); ++index) {
        const auto& input = request["inputs"][index];
        if (!Keys(input, {"input_index", "locator"}) || !input.HasMember("input_index") ||
            !input["input_index"].IsUint64() || input["input_index"].GetUint64() != index ||
            !input.HasMember("locator") || !input["locator"].IsObject())
            return Failure({"CONTRACT_INVALID", "inputs require consecutive indices and F2 locators", "/inputs"}, &rsp);
    }
    std::set<std::string> parameter_names;
    for (const auto& parameter : request["parameters"].GetObject())
        if (!parameter_names.insert(Text(parameter.name)).second)
            return Failure({"CONTRACT_INVALID", "duplicate algorithm parameter", "/parameters"}, &rsp);
    DataSemanticsErrorV1 issue;
    contracts::Invocation invocation;
    const auto category = Get(request["operator"], "category"), name = Get(request["operator"], "name");
    CppOperatorCapabilityLeaseV1 lease;
    bool ambiguous = false;
    int discovery_error = 0;
    auto provider = FindBlockTransformOperator(category, name, &lease, &ambiguous, &discovery_error);
    if (ambiguous) return Failure({"COMPONENT_CHANGED", "operator capability is ambiguous", "/operator"}, &rsp);
    if (discovery_error) return Failure({"UNAVAILABLE", "operator capability discovery failed", "/operator"}, &rsp);
    std::shared_ptr<IOperator> holder;
    IOperatorConfigAdapterV1* adapter = nullptr;
    if (provider && Get(request["inputs"][0]["locator"], "kind") == "dataframe") {
        auto* capability = provider.v2 ? dynamic_cast<IBlockTransformDataFrameInputProviderV1*>(provider.v2) : nullptr;
        if (!capability || !capability->SupportsDataFrameInput()) provider = {};
    }
    if (provider) {
        invocation.component = provider.component;
        adapter = provider.v2 ? dynamic_cast<IOperatorConfigAdapterV1*>(provider.v2)
                              : dynamic_cast<IOperatorConfigAdapterV1*>(provider.v1);
    } else {
        holder = FindOperator(category, name);
        if (!holder) return Failure({"NOT_FOUND", "operator not found", "/operator"}, &rsp);
        if (contracts::Component(querier_, dynamic_cast<IOperatorContractIdentityV1*>(holder.get()), category, name,
                                 &invocation.component, &issue))
            return Failure(issue, &rsp);
        for (const auto& parameter : request["parameters"].GetObject())
            if (holder->Configure(parameter.name.GetString(), parameter.value.GetString()))
                return Failure({"CONTRACT_INVALID", "operator rejected algorithm parameter", "/parameters"}, &rsp);
        adapter = dynamic_cast<IOperatorConfigAdapterV1*>(holder.get());
    }
    Json contract, inputs(rapidjson::kArrayType), context(rapidjson::kObjectType);
    auto& a = inputs.GetAllocator();
    auto& ca = context.GetAllocator();
    if (invocation.component.contract_state == "declared") {
        auto* service =
            querier_ ? static_cast<IOperatorDataContractV1*>(querier_->First(IID_OPERATOR_DATA_CONTRACT_V1)) : nullptr;
        OperatorContractSnapshotV1 definition;
        if (!service) return Failure({"UNAVAILABLE", "declared contract service unavailable", ""}, &rsp);
        if (service->Resolve(invocation.component.contract_ref.c_str(), &definition, &issue))
            return Failure(issue, &rsp);
        contract.Parse(definition.document_json->c_str());
    } else if (invocation.component.contract_state == "unavailable") {
        return Failure({"UNAVAILABLE", "declared contract unavailable", ""}, &rsp);
    }
    Value shapes(rapidjson::kArrayType);
    size_t port_index = 0, port_count = 0;
    for (rapidjson::SizeType index = 0; index < request["inputs"].Size(); ++index) {
        const auto& input = request["inputs"][index];
        Json snapshot;
        std::string binding;
        if (contracts::DescribeInput(querier_, input["locator"], {}, &snapshot, &binding, &issue))
            return Failure(issue, &rsp);
        Value actual(rapidjson::kObjectType);
        if (contract.IsObject() && !contract["inputs"].Empty()) {
            const auto& ports = contract["inputs"];
            while (port_index + 1 < ports.Size() && port_count >= ports[port_index]["max_count"].GetUint64()) {
                ++port_index;
                port_count = 0;
            }
            ++port_count;
            Set(actual, "port_id", Get(ports[port_index], "port_id"), a);
        } else
            Set(actual, "port_id", "input", a);
        actual.AddMember("input_index", index, a).AddMember("snapshot", Copy(snapshot, a), a);
        if (!binding.empty()) Set(actual, "binding_ref", binding, a);
        inputs.PushBack(actual, a);
        shapes.PushBack(Value(provider ? "record_batch" : "dataframe", ca), ca);
    }
    context.AddMember("input_shapes", shapes, ca);
    context.AddMember("preflight", true, ca);
    Set(context, "source", Source(request["inputs"][0]["locator"]), ca);
    if (request.HasMember("output_locator")) {
        // F2 locators identify explicit finite objects. A database_table target
        // follows the ordinary table sink path, never the managed channel path.
        const auto& output = request["output_locator"];
        if (!output.IsObject()) return Failure({"CONTRACT_INVALID", "invalid output locator", "/output_locator"}, &rsp);
        const auto kind = Get(output, "kind"), type = Get(output, "type");
        bool valid = !Get(output, "name").empty();
        if (kind == "dataframe")
            valid &= Keys(output, {"kind", "name"});
        else if (kind == "database_table")
            valid &= Keys(output, {"kind", "type", "name", "database", "schema", "table"}) &&
                     !Get(output, "table").empty() &&
                     (type == "sqlite" || type == "mysql" || type == "postgres" || type == "clickhouse");
        else
            valid = false;
        for (const auto& member : output.GetObject())
            valid &= member.value.IsString() && !Text(member.value).empty() &&
                     Text(member.value).find('\0') == std::string::npos;
        if (!valid)
            return Failure({"CONTRACT_INVALID", "output_locator must identify a finite F2 target", "/output_locator"},
                           &rsp);
        Set(context, "managed_output", Value(false), ca);
    }
    const int rc =
        contracts::CheckInvocation(querier_, &invocation, adapter, request["parameters"], inputs, context, &issue);
    if (rc && issue.code != "CONTRACT_INCOMPATIBLE") return Failure(issue, &rsp);
    if (rc && Get(invocation.check, "state") != "incompatible") {
        invocation.check.SetObject();
        auto& ra = invocation.check.GetAllocator();
        Set(invocation.check, "state", "incompatible", ra);
        Set(invocation.check, "data_validation", "not_run", ra);
        Set(invocation.check, "output_check", "not_observed", ra);
        Value reasons(rapidjson::kArrayType), reason(rapidjson::kObjectType);
        Set(reason, "code", issue.code, ra);
        Set(reason, "pointer", issue.field_path, ra);
        Set(reason, "basis", issue.message, ra);
        reason.AddMember("expected", Value(), ra).AddMember("observed", Value(), ra);
        reasons.PushBack(reason, ra);
        for (const char* key : {"checked_refs", "output_previews", "semantic_gaps"})
            invocation.check.AddMember(Value(key, ra), Value(rapidjson::kArrayType), ra);
        invocation.check.AddMember("role_map", Value(rapidjson::kObjectType), ra);
        invocation.check.AddMember("reasons", reasons, ra);
    }
    // Check for a concurrent replacement without loading lazy business rows.
    for (rapidjson::SizeType i = 0; i < invocation.resolved["inputs"].Size(); ++i) {
        Json after;
        std::string binding;
        if (contracts::DescribeInput(querier_, invocation.resolved["inputs"][i]["snapshot"]["locator"], {}, &after,
                                     &binding, &issue))
            return Failure(issue, &rsp);
        const auto& before = invocation.resolved["inputs"][i]["snapshot"];
        if (before["object_key"] != after["object_key"] ||
            Get(before, "source_revision") != Get(after, "source_revision"))
            return Failure({"SOURCE_CHANGED", "source changed during preflight", "/inputs"}, &rsp);
    }
    Json result(rapidjson::kObjectType);
    auto& ra = result.GetAllocator();
    Value data(rapidjson::kObjectType), component(rapidjson::kObjectType);
    Set(component, "category", invocation.component.category, ra);
    Set(component, "name", invocation.component.name, ra);
    Set(component, "component_revision", invocation.component.component_revision, ra);
    Set(component, "operator_version", invocation.component.operator_version, ra);
    Set(component, "version_state", invocation.component.operator_version.empty() ? "unknown" : "declared", ra);
    Set(component, "contract_state", invocation.component.contract_state, ra);
    Set(component, "contract_ref", invocation.component.contract_ref, ra);
    data.AddMember("component", component, ra)
        .AddMember("resolved_invocation", Copy(invocation.resolved, ra), ra)
        .AddMember("check", Copy(invocation.check, ra), ra);
    result.AddMember("data", data, ra);
    rsp = Encode(result);
    if (rsp.size() > 12 * 1024 * 1024)
        return Failure({"LIMIT_EXCEEDED", "preflight response exceeds 12 MiB", ""}, &rsp);
    return error::OK;
}
}  // namespace flowsql::scheduler
