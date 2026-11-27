// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <common/error_code.h>
#include <framework/interfaces/iblock_transform_dataframe_input.h>
#include <framework/interfaces/iblock_transform_operator.h>
#include <framework/interfaces/icpp_operator_contract_provider.h>
#include <framework/interfaces/imetadata_description.h>
#include <framework/interfaces/ipacket_family.h>
#include <framework/interfaces/ischeduler_control_service.h>
#include <cassert>
#include <common/operator_contract_execution.hpp>
#include <regex>
#include <set>
#include "access_store.h"
#include "data_access_plugin.h"

namespace flowsql::access {
using namespace dataset;
namespace {
struct Provider {
    CppOperatorCapabilityLease lease;
    IBlockTransformOperator* pointer = nullptr;
    OperatorComponentContract component;
};
Provider FindProvider(IQuerier* querier, const Value& identity) {
    Provider selected;
    size_t matches = 0;
    const auto category = Get(identity, "category"), name = Get(identity, "name");
    querier->Traverse(IID_BLOCK_TRANSFORM_OPERATOR, [&](void* pointer) {
        auto* provider = static_cast<IBlockTransformOperator*>(pointer);
        if (provider->Category() != category || provider->Name() != name) return 0;
        DataSemanticsError error;
        if (contracts::Component(querier, dynamic_cast<IOperatorContractIdentity*>(provider), category, name,
                                 &selected.component, &error))
            throw AccessError(error.code, "operator identity is unavailable", 503);
        selected.pointer = provider;
        ++matches;
        return 0;
    });
    querier->Traverse(IID_CPP_OPERATOR_PLUGIN_REGISTRY, [&](void* pointer) {
        auto* registry = static_cast<ICppOperatorPluginRegistry*>(pointer);
        auto* contracts = dynamic_cast<ICppOperatorContractProvider*>(registry);
        if (!contracts) return 0;
        CppOperatorCapabilityLease lease;
        OperatorComponentContract component;
        if (contracts->AcquireContract(category.c_str(), name.c_str(), IID_BLOCK_TRANSFORM_OPERATOR, &lease,
                                       &component))
            return 0;
        assert(lease.capability && lease.lifetime);
        selected.lease = std::move(lease);
        selected.pointer = static_cast<IBlockTransformOperator*>(selected.lease.capability);
        selected.component = std::move(component);
        ++matches;
        return 0;
    });
    if (matches > 1) throw AccessError("COMPONENT_CHANGED", "operator capability is ambiguous", 409);
    if (!selected.pointer) throw AccessError("OPERATOR_UNAVAILABLE", "finite block operator is unavailable", 503);
    if (selected.component.contract_state != "declared")
        throw AccessError("CONTRACT_UNAVAILABLE", "finite analysis requires a declared component contract", 422);
    return selected;
}
std::string Identifier(const std::string& value) {
    if (!std::regex_match(value, std::regex("[A-Za-z_][A-Za-z0-9_]*")))
        throw AccessError("UNSUPPORTED_SOURCE", "analysis requires SQL-addressable registered identities", 422);
    return value;
}
std::string SourceName(const Value& locator) {
    if (Get(locator, "kind") == "dataframe") return "dataframe." + Identifier(Get(locator, "name"));
    return Identifier(Get(locator, "type")) + "." + Identifier(Get(locator, "name"));
}
std::string QuoteValue(const std::string& value) {
    std::string quoted = "'";
    for (char byte : value) {
        quoted += byte;
        if (byte == '\'') quoted += '\'';
    }
    return quoted + "'";
}
const Value& Profile(const Subject& subject, const std::string& name) {
    if (subject.policy->HasMember("analyses"))
        for (const auto& profile : (*subject.policy)["analyses"].GetArray())
            if (Get(profile, "name") == name) return profile;
    throw AccessError("FORBIDDEN", "analysis profile is outside this subject's grant", 403);
}
ISchedulerControlService* Scheduler(IQuerier* querier) {
    auto* scheduler = static_cast<ISchedulerControlService*>(querier->First(IID_SCHEDULER_CONTROL_SERVICE));
    if (!scheduler) throw AccessError("EXECUTOR_UNAVAILABLE", "finite executor is unavailable", 503);
    return scheduler;
}
Json RuntimeRequest(const std::string& id) {
    Json request(rapidjson::kObjectType);
    Set(request, "runtime_task_id", id, request.GetAllocator());
    return request;
}
bool Terminal(const std::string& status) {
    return status == "completed" || status == "failed" || status == "cancelled" || status == "stopped" ||
           status == "timeout";
}
// Only published dataset locators in the task's own execution context qualify.
// Neither relation names nor domain-specific tables are reconstructed here.
void PublicOutputs(const Value& context, const Value& scope, Json* output) {
    auto& a = output->GetAllocator();
    if (context.IsObject()) {
        if (context.HasMember("locator") && context.HasMember("dictionary_ref")) {
            const auto& locator = context["locator"];
            if (Get(locator, "kind") == "database_table" && Get(locator, "type") == Get(scope, "type") &&
                Get(locator, "name") == Get(scope, "name")) {
                if (output->Size() >= 256) throw AccessError("RESULT_LIMIT", "too many derived relations", 413);
                output->PushBack(Copy(locator, a), a);
            }
        }
        for (const auto& member : context.GetObject()) PublicOutputs(member.value, scope, output);
    } else if (context.IsArray())
        for (const auto& child : context.GetArray()) PublicOutputs(child, scope, output);
}
}  // namespace

Source DataAccessPlugin::AnalysisInput(const Subject& subject, const Value& locator) {
    Json observation(rapidjson::kObjectType);
    observation.AddMember("operations", Value(rapidjson::kArrayType), observation.GetAllocator());
    const auto target = Describe(locator, observation);
    // Derived outputs are query-only. Analysis authorization inspects explicit
    // grants, without describing every previously produced result relation.
    for (const auto& grant : (*subject.policy)["sources"].GetArray()) {
        const auto source = Describe(grant["locator"], grant);
        if (source.id != target.id) continue;
        if (!Permitted(source, "analyze")) throw AccessError("FORBIDDEN", "operation is not granted for an input", 403);
        if (!source.available) throw AccessError("SOURCE_UNAVAILABLE", "analysis input is unavailable", 503);
        if (source.grant->HasMember("filters") && !(*source.grant)["filters"].Empty())
            throw AccessError("UNSUPPORTED_SCOPE", "filtered sources require the controlled query workflow", 422);
        return source;
    }
    throw AccessError("FORBIDDEN", "analysis input or configuration is outside the subject grant", 403);
}
Json DataAccessPlugin::OperationDescriptor(const Subject& subject, const Value& profile) {
    Json output(rapidjson::kObjectType);
    auto& a = output.GetAllocator();
    std::string revision = Canonical(profile), reason;
    try {
        auto source = AnalysisInput(subject, profile["source"]);
        revision += source.id + source.revision;
        if (source.kind != "database_table" && source.kind != "dataframe" &&
            !(source.kind == "block_stream" && source.provider == "pcapfile"))
            throw AccessError("UNSUPPORTED_SOURCE", "only finite datasets and registered capture files can be analyzed",
                              422);
        for (const auto& reference : profile["config_refs"].GetArray()) {
            Json locator(rapidjson::kObjectType);
            Set(locator, "kind", "config", locator.GetAllocator());
            Set(locator, "reference", Text(reference), locator.GetAllocator());
            revision += AnalysisInput(subject, locator).revision;
        }
        auto provider = FindProvider(querier_, profile["operator"]);
        revision += provider.component.component_revision + provider.component.contract_ref;
        Set(output, "component_revision", provider.component.component_revision, a);
        Set(output, "contract_ref", provider.component.contract_ref, a);
        if (source.kind == "dataframe") {
            auto* capability = dynamic_cast<IBlockTransformDataFrameInputProvider*>(provider.pointer);
            if (!capability || !capability->SupportsDataFrameInput())
                throw AccessError("UNSUPPORTED_SOURCE", "operator does not accept DataFrame snapshots", 422);
        }
        Scheduler(querier_);
        auto* descriptions = static_cast<IMetadataDescription*>(querier_->First(IID_METADATA_DESCRIPTION));
        if (descriptions) {
            Json request(rapidjson::kObjectType);
            auto& ra = request.GetAllocator();
            Value target(rapidjson::kObjectType);
            Set(target, "kind", "operator", ra);
            Set(target, "name", Get(profile["operator"], "category") + "." + Get(profile["operator"], "name"), ra);
            request.AddMember("target", target, ra);
            std::string text;
            if (!descriptions->Describe(Encode(request).c_str(), &text)) {
                const auto observed = Parse(text);
                output.AddMember("description", Copy(observed["data"], a), a);
            }
        }
    } catch (const AccessError& error) {
        reason = error.code;
    }
    Value reference(rapidjson::kObjectType);
    Set(reference, "operation_id", Get(profile, "name"), a);
    Set(reference, "operation_revision", Hash(revision), a);
    output.AddMember("operation_ref", reference, a);
    for (const auto* key : {"operator", "source", "parameters", "config_refs", "output"})
        output.AddMember(Value(key, a), Copy(profile[key], a), a);
    output.AddMember("available", reason.empty(), a);
    Set(output, "reason", reason, a);
    Set(output, "parameter_mode", "deployment_fixed", a);
    Set(output, "result_scope", "execution_manifest_and_public_derived_sources", a);
    return output;
}
Json DataAccessPlugin::DescribeOperation(const Subject& subject, const Value& reference) {
    auto descriptor = OperationDescriptor(subject, Profile(subject, Required(reference, "operation_id")));
    if (Get(descriptor["operation_ref"], "operation_revision") != Required(reference, "operation_revision"))
        throw AccessError("OPERATION_CHANGED", "analysis basis changed; discover again", 409);
    return descriptor;
}
Json DataAccessPlugin::Operations(const Subject& subject) {
    Json output(rapidjson::kObjectType);
    Value items(rapidjson::kArrayType);
    if (subject.policy->HasMember("analyses"))
        for (const auto& profile : (*subject.policy)["analyses"].GetArray())
            items.PushBack(Copy(OperationDescriptor(subject, profile), output.GetAllocator()), output.GetAllocator());
    output.AddMember("operations", items, output.GetAllocator());
    return output;
}
Json DataAccessPlugin::PrepareAnalysis(const Subject& subject, const Value& args) {
    const auto& profile = Profile(subject, Required(args["operation_ref"], "operation_id"));
    auto descriptor = OperationDescriptor(subject, profile);
    if (Get(descriptor["operation_ref"], "operation_revision") != Required(args["operation_ref"], "operation_revision"))
        throw AccessError("OPERATION_CHANGED", "analysis basis changed; discover again", 409);
    if (!descriptor["available"].GetBool())
        throw AccessError(Get(descriptor, "reason"), "approved analysis is unavailable",
                          Get(descriptor, "reason") == "FORBIDDEN"                             ? 403
                          : Get(descriptor, "reason").find("UNAVAILABLE") != std::string::npos ? 503
                                                                                               : 422);
    auto source = Resolve(subject, args["source_ref"]);
    if (source.id != AnalysisInput(subject, profile["source"]).id)
        throw AccessError("FORBIDDEN", "source does not belong to the selected analysis", 403);
    if (source.kind == "dataframe" && (!args.HasMember("capture_snapshot") || !args["capture_snapshot"].GetBool()))
        throw AccessError("SNAPSHOT_REQUIRED", "analysis requires explicit snapshot capture", 422);
    const auto source_description = Parse(source.description);
    if (source.kind == "block_stream" && source_description.HasMember("byte_count") &&
        source_description["byte_count"].GetUint64() > Bound(profile, "max_input_bytes", 67108864, 1, 67108864))
        throw AccessError("RESULT_LIMIT", "finite input exceeds the approved file budget", 413);
    auto provider = FindProvider(querier_, profile["operator"]);
    contracts::Invocation invocation;
    invocation.component = provider.component;
    DataSemanticsError issue;
    auto* service = static_cast<IOperatorDataContract*>(querier_->First(IID_OPERATOR_DATA_CONTRACT));
    if (!service) throw AccessError("CONTRACT_UNAVAILABLE", "operator contract service is unavailable", 503);
    OperatorContractSnapshot definition;
    if (service->Resolve(invocation.component.contract_ref.c_str(), &definition, &issue))
        throw AccessError(issue.code, "operator contract cannot be resolved", 422);
    Json inputs(rapidjson::kArrayType), context(rapidjson::kObjectType), snapshot;
    std::string binding;
    if (source.kind == "block_stream") {
        Json fields;
        const auto arrow_fields = ArrowFields(*packet::PacketFamilySchema(), fields.GetAllocator());
        snapshot = Snapshot("block_stream", source.id, Parse(source.locator), arrow_fields,
                            Value(rapidjson::kObjectType), "", "known", "known");
        Set(snapshot, "source_revision", source.revision, snapshot.GetAllocator());
    } else if (contracts::DescribeInput(querier_, Parse(source.locator), {}, &snapshot, &binding, &issue))
        throw AccessError(issue.code, "analysis input metadata cannot be resolved", 422);
    Value input(rapidjson::kObjectType);
    auto& ia = inputs.GetAllocator();
    const auto& ports = (*definition.document)["inputs"];
    Set(input, "port_id", ports.Empty() ? "input" : Get(ports[0], "port_id"), ia);
    input.AddMember("input_index", 0, ia).AddMember("snapshot", Copy(snapshot, ia), ia);
    if (!binding.empty()) Set(input, "binding_ref", binding, ia);
    inputs.PushBack(input, ia);
    auto& ca = context.GetAllocator();
    context.AddMember("input_shapes", Value(rapidjson::kArrayType), ca);
    context["input_shapes"].PushBack("record_batch", ca);
    context.AddMember("finite_source", true, ca).AddMember("preflight", true, ca);
    context.AddMember("managed_output", Get(profile["output"], "kind") == "managed_database", ca);
    Set(context, "source", SourceName(Parse(source.locator)), ca);
    if (contracts::CheckInvocation(querier_, &invocation, dynamic_cast<IOperatorConfigAdapter*>(provider.pointer),
                                   profile["parameters"], inputs, context, &issue))
        throw AccessError(issue.code, "approved invocation failed its actual operator preflight", 422);
    const std::set<std::string> configs = [&] {
        std::set<std::string> refs;
        for (const auto& reference : profile["config_refs"].GetArray()) refs.insert(Text(reference));
        return refs;
    }();
    for (const auto& reference : invocation.resolved["config_refs"].GetArray())
        if (!configs.count(Text(reference)))
            throw AccessError("FORBIDDEN", "operator resolved an unapproved configuration", 403);
    for (const auto& input : invocation.resolved["inputs"].GetArray()) {
        const auto& locator = input["snapshot"]["locator"];
        if (source.kind == "block_stream" && Canonical(locator) == source.locator) continue;
        AnalysisInput(subject, locator);
    }
    auto& a = descriptor.GetAllocator();
    descriptor.AddMember("resolved_invocation", Copy(invocation.resolved, a), a);
    descriptor.AddMember("check", Copy(invocation.check, a), a);
    descriptor.AddMember("source_basis", Copy(SourceJson(source), a), a);
    return descriptor;
}

Json DataAccessPlugin::AnalysisPayload(const Subject& subject, const Value& record, const Value& runtime) {
    const auto& plan = record["analysis_plan"];
    Resolve(subject, plan["source_basis"]["source_ref"]);
    for (const auto& input : plan["resolved_invocation"]["inputs"].GetArray()) {
        const auto& snapshot = input["snapshot"];
        if (Get(snapshot["locator"], "kind") == "block_stream") continue;
        const auto observed = AnalysisInput(subject, snapshot["locator"]);
        if (observed.revision != Get(snapshot, "source_revision"))
            throw AccessError("SOURCE_CHANGED", "an analysis input changed during execution", 409);
    }
    Json locators(rapidjson::kArrayType), grants(rapidjson::kArrayType);
    const auto& scope = plan["output"];
    if (Get(scope, "kind") == "dataframe") {
        Value locator(rapidjson::kObjectType);
        Set(locator, "kind", "dataframe", locators.GetAllocator());
        Set(locator, "name", Get(record, "output_name"), locators.GetAllocator());
        locators.PushBack(locator, locators.GetAllocator());
    }
    if (!runtime.HasMember("contract_checks") || runtime["contract_checks"].Empty())
        throw AccessError("RECONCILE_REQUIRED", "completed runtime has no frozen execution observation", 409);
    for (const auto& check : runtime["contract_checks"].GetArray()) {
        Json request(rapidjson::kObjectType), resolved;
        Set(request, "exact_reference", Get(check, "use_ref"), request.GetAllocator());
        Set(request, "view", "document", request.GetAllocator());
        DataSemanticsError issue;
        if (contracts::Control(querier_, "resolve", request, &resolved, &issue))
            throw AccessError("RECONCILE_REQUIRED", "runtime observation cannot yet be resolved", 409);
        const auto& used = resolved["data"]["document"];
        if (Get(used, "component_revision") != Get(plan, "component_revision"))
            throw AccessError("COMPONENT_CHANGED", "executed component differs from approved preflight", 409);
        if (Get(scope, "kind") == "managed_database" && used.HasMember("execution_context"))
            PublicOutputs(used["execution_context"], scope, &locators);
    }
    if (locators.Empty()) throw AccessError("RECONCILE_REQUIRED", "actual public outputs have not been published", 409);
    std::set<std::string> seen;
    for (const auto& locator : locators.GetArray()) {
        if (!seen.insert(Canonical(locator)).second) continue;
        Value grant(rapidjson::kObjectType);
        auto& ga = grants.GetAllocator();
        grant.AddMember("locator", Copy(locator, ga), ga).AddMember("output_scope", Copy(scope, ga), ga);
        Set(grant, "analysis_name", Get(plan["operation_ref"], "operation_id"), ga);
        const auto source = Describe(locator, grant);
        Value reference(rapidjson::kObjectType);
        Set(reference, "source_id", source.id, ga);
        Set(reference, "source_revision", source.revision, ga);
        grant.AddMember("source_ref", reference, ga);
        grants.PushBack(grant, ga);
    }
    auto current = store_->Find(subject.principal, Get(record, "operation_id"));
    Set(current, "derived_sources", Copy(grants, current.GetAllocator()), current.GetAllocator());
    Set(current, "underlying_status", "completed", current.GetAllocator());
    store_->Update(subject.principal, current);
    Json payload(rapidjson::kObjectType);
    auto& a = payload.GetAllocator();
    payload.AddMember("fields", Value(rapidjson::kArrayType), a).AddMember("rows", Value(rapidjson::kArrayType), a);
    Value basis(rapidjson::kObjectType);
    Set(basis, "scope", "analysis_manifest", a);
    Set(basis, "runtime_task_id", Get(record, "runtime_task_id"), a);
    Set(basis, "operation_id", Get(record, "operation_id"), a);
    Set(basis, "sql", Get(record, "sql"), a);
    basis.AddMember("preflight", Copy(plan, a), a).AddMember("derived_sources", Copy(grants, a), a);
    for (const auto* key :
         {"status", "result_row_count", "result_target", "contract_checks", "managed_result", "result_columns"})
        if (runtime.HasMember(key)) basis.AddMember(Value(key, a), Copy(runtime[key], a), a);
    payload.AddMember("basis", basis, a);
    const auto maximum =
        Bound(record, "max_bytes", Bound(*subject.policy, "max_bytes", 8388608, 1024, 67108864), 1024, 67108864);
    if (Encode(payload).size() > maximum)
        throw AccessError("RESULT_LIMIT", "analysis manifest exceeds result budget", 413);
    return payload;
}
Json DataAccessPlugin::ComputeAnalysis(const Subject& subject, const Value& args,
                                       const std::shared_ptr<Running>& task) {
    auto plan = PrepareAnalysis(subject, args);
    auto record = store_->Find(subject.principal, task->id);
    auto* scheduler = Scheduler(querier_);
    const auto& scope = plan["output"];
    const auto output_name = "access_" + task->id;
    const auto target = Get(scope, "kind") == "dataframe"
                            ? "dataframe." + output_name
                            : Identifier(Get(scope, "type")) + "." + Identifier(Get(scope, "name"));
    std::string sql = "SELECT * FROM " + SourceName(plan["source_basis"]["locator"]) + " USING " +
                      Get(plan["operator"], "category") + "." + Get(plan["operator"], "name");
    bool first = true;
    for (const auto& parameter : plan["parameters"].GetObject()) {
        sql += first ? " WITH " : ", ";
        first = false;
        sql += Text(parameter.name) + "=" + QuoteValue(Text(parameter.value));
    }
    sql += " INTO " + target;
    const auto runtime_id = "access_" + task->id;
    auto& a = record.GetAllocator();
    Set(record, "analysis_plan", Copy(plan, a), a);
    Set(record, "runtime_task_id", runtime_id, a);
    Set(record, "output_name", output_name, a);
    Set(record, "sql", sql, a);
    // This durable association is committed before the first scheduler submission.
    Set(record, "backend_submitted", Value(true), a);
    store_->Update(subject.principal, record);
    const auto timeout = Bound(args, "timeout_ms", Bound(*subject.policy, "timeout_ms", 5000, 1, 30000), 1, 30000);
    auto request = RuntimeRequest(runtime_id);
    request.AddMember("sqls", Value(rapidjson::kArrayType), request.GetAllocator());
    request["sqls"].PushBack(Value(sql.c_str(), request.GetAllocator()), request.GetAllocator());
    request.AddMember("timeout_s", static_cast<int>((timeout + 999) / 1000), request.GetAllocator());
    std::string text;
    if (*task->cancelled || task->timed_out) {
        Set(record, "backend_submitted", Value(false), a);
        store_->Update(subject.principal, record);
        throw AccessError(*task->cancelled ? "CANCELLED" : "TIMEOUT", "analysis stopped before submission", 409);
    }
    // Any ambiguous submit result is reconciled by this stable identity, never retried.
    const auto submitted = scheduler->SubmitBatch(Encode(request), &text);
    if (submitted == error::BAD_REQUEST) {
        auto rejected = store_->Find(subject.principal, task->id);
        Set(rejected, "underlying_status", "rejected", rejected.GetAllocator());
        Set(rejected, "backend_submitted", Value(false), rejected.GetAllocator());
        store_->Update(subject.principal, rejected);
        throw AccessError("ANALYSIS_REJECTED", "executor rejected the frozen analysis request", 422);
    }
    auto identity = RuntimeRequest(runtime_id);
    bool cancellation_sent = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout + 2000);
    for (;;) {
        if ((*task->cancelled || task->timed_out) && !cancellation_sent) {
            auto cancel = RuntimeRequest(runtime_id);
            Set(cancel, "mode", "cancel", cancel.GetAllocator());
            scheduler->StopBatch(Encode(cancel), &text);
            cancellation_sent = true;
        }
        if (scheduler->QueryBatchStatus(Encode(identity), &text))
            throw AccessError("RECONCILE_REQUIRED", "scheduler state is unknown; analysis was not resubmitted", 409);
        auto status = Parse(text);
        const auto state = Get(status, "status");
        if (Terminal(state)) {
            auto current = store_->Find(subject.principal, task->id);
            Set(current, "underlying_status", state, current.GetAllocator());
            store_->Update(subject.principal, current);
            if (state == "completed") return AnalysisPayload(subject, current, status);
            if (state == "cancelled" || state == "stopped")
                throw AccessError("CANCELLED_CONFIRMED", "runtime confirmed cancellation", 409);
            if (state == "timeout") throw AccessError("TIMEOUT_CONFIRMED", "runtime confirmed timeout", 408);
            throw AccessError("ANALYSIS_FAILED", "finite analysis failed in the executor", 422);
        }
        if (std::chrono::steady_clock::now() >= deadline)
            throw AccessError("RECONCILE_REQUIRED", "cancellation has not reached a confirmed terminal state", 409);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
Json DataAccessPlugin::ReconcileAnalysis(const Subject& subject, Json record) {
    const auto runtime_id = Get(record, "runtime_task_id");
    if (runtime_id.empty()) return record;
    auto* scheduler = static_cast<ISchedulerControlService*>(querier_->First(IID_SCHEDULER_CONTROL_SERVICE));
    if (!scheduler) return record;
    std::string text;
    if (scheduler->QueryBatchStatus(Encode(RuntimeRequest(runtime_id)), &text)) return record;
    auto runtime = Parse(text);
    const auto state = Get(runtime, "status");
    Set(record, "underlying_status", state, record.GetAllocator());
    if (state == "completed") {
        try {
            auto payload = AnalysisPayload(subject, record, runtime);
            return store_->Complete(subject.principal, Get(record, "operation_id"), payload,
                                    Bound(*subject.policy, "result_ttl_ms", 3600000, 1, 86400000));
        } catch (const AccessError& error) {
            // The output may already exist. Preserve the association for another
            // read/reconciliation; never turn this into a retryable submission.
            Set(record, "error_code", error.code, record.GetAllocator());
            Set(record, "status", "reconcile_required", record.GetAllocator());
        }
    } else if (Terminal(state)) {
        Set(record, "status", state == "cancelled" || state == "stopped" ? "cancelled" : "failed",
            record.GetAllocator());
        Set(record, "error_code",
            state == "timeout"  ? "TIMEOUT"
            : state == "failed" ? "ANALYSIS_FAILED"
                                : "CANCELLED",
            record.GetAllocator());
    }
    store_->Update(subject.principal, record);
    return record;
}
}  // namespace flowsql::access
