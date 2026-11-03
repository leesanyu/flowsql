// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include "npm_basic_operator.h"

#include <framework/interfaces/iconfig_channel_registry.h>
#include <framework/interfaces/iflow_labeling.h>
#include <operators/npm_basic/config/npm_basic_task_config.h>
#include <operators/npm_basic/core/npm_basic_task_runtime.h>
#include <operators/npm_basic/modules/dns/npm_dns_contract.h>
#include <operators/npm_basic/modules/http1/npm_http1_contract.h>
#include <operators/npm_basic/modules/icmp/npm_icmp_contract.h>
#include <operators/npm_basic/modules/tls/npm_tls_contract.h>
#include <common/channel_dataset.hpp>

#include <cassert>
#include <cstring>

namespace flowsql::npm {
namespace {
using namespace dataset;
Value Core(const NpmBasicTaskConfig& config, bool mode_known, Alloc& a) {
    Value core(rapidjson::kObjectType);
    const auto& c = config.analysis;
    if (mode_known) Set(core, "run_mode", c.run_mode == NpmRunMode::kOffline ? "offline" : "realtime", a);
    Set(core, "result_mode", c.result_mode == NpmResultMode::kFinal ? "final" : "periodic_snapshot", a);
    Set(core, "overload_policy", "fail", a);
#define NUMBER(name) core.AddMember(#name, c.name, a)
    NUMBER(output_interval_ns);
    NUMBER(payload_sample_packets);
    NUMBER(tcp_idle_timeout_ns);
    NUMBER(udp_idle_timeout_ns);
    NUMBER(out_of_order_tolerance_ns);
    NUMBER(max_active_sessions);
    NUMBER(max_tracked_bytes);
    NUMBER(max_pending_output_bytes);
#undef NUMBER
    Value stream(rapidjson::kObjectType);
    stream.AddMember("max_buffered_bytes_per_direction", config.tcp_stream.max_buffered_bytes_per_direction, a)
        .AddMember("gap_timeout_ns", config.tcp_stream.gap_timeout_ns, a);
    core.AddMember("tcp_stream", stream, a);
    if (config.result_retention_days) core.AddMember("result_retention_days", *config.result_retention_days, a);
    if (config.features.labeling_enabled) {
        Set(core, "labeling", config.labeling_reference, a);
        core.AddMember("labeling_memory_mib", config.labeling_memory_mib, a);
    }
    return core;
}
// Preparation already validates these values. Serialization uses typed production
// results, including defaults, and never creates a module or executes its factory.
Value Module(const std::string& id, const std::string& json, Alloc& a) {
    Value out(rapidjson::kObjectType);
#define PARAM(name) out.AddMember(#name, c.name, a)
    if (id == "dns") {
        NpmDnsConfigV1 c;
        const auto status = ParseNpmDnsConfigV1(json, &c);
        assert(status.error == NpmDnsConfigErrorV1::kNone);
        PARAM(response_timeout_ns);
        PARAM(max_pending_per_session);
    } else if (id == "http1") {
        NpmHttp1ConfigV1 c;
        const auto status = ParseNpmHttp1ConfigV1(json, &c);
        assert(status.error == NpmHttp1ConfigErrorV1::kNone);
        PARAM(response_timeout_ns);
        PARAM(max_pending_per_session);
        PARAM(max_header_bytes);
    } else if (id == "tls") {
        NpmTlsConfigV1 c;
        const auto status = ParseNpmTlsConfigV1(json, &c);
        assert(status.error == NpmTlsConfigErrorV1::kNone);
        PARAM(handshake_timeout_ns);
        PARAM(max_hello_bytes);
    } else if (id == "icmp") {
        NpmIcmpConfigV1 c;
        const auto status = ParseNpmIcmpConfigV1(json, &c);
        assert(status.error == NpmIcmpConfigErrorV1::kNone);
        PARAM(echo_timeout_ns);
        PARAM(max_pending_echo);
    }
#undef PARAM
    return out;
}
}  // namespace

int NpmBasicTask::ResolveInvocation(const char* text, std::string* result, DataSemanticsErrorV1* error) {
    using namespace dataset;
    if (result) result->clear();
    Json request;
    if (!text || !result || !JsonNestingWithin(text, 64) || request.Parse(text).HasParseError() ||
        !request.IsObject() || !request.HasMember("operator") || !request["operator"].IsObject() ||
        !request.HasMember("execution_context") || !request["execution_context"].IsObject())
        return dataset::Fail(error, "CONTRACT_INVALID", "invalid NPM task invocation");
    const auto& identity = request["operator"];
    const auto ref = Get(identity, "contract_ref"), revision = Get(identity, "component_revision");
    if (!ref.empty() || !revision.empty()) {
        if (ref.empty() || revision.empty())
            return dataset::Fail(error, "CONTRACT_INVALID", "incomplete NPM component identity");
        if (component_.contract_state == "declared" &&
            (component_.contract_ref != ref || component_.component_revision != revision))
            return dataset::Fail(error, "COMPONENT_CHANGED", "NPM task component identity changed");
        component_.category = "npm";
        component_.name = "basic";
        component_.contract_state = "declared";
        component_.contract_ref = ref;
        component_.component_revision = revision;
    }
    const auto runtime = Runtime();
    if (runtime) {
        auto& a = request.GetAllocator();
        auto& context = request["execution_context"];
        Set(context, "finite_source", Value(runtime->Config().analysis.run_mode == NpmRunMode::kOffline), a);
        Set(context, "managed_output", Value(managed_channel_ != nullptr), a);
        Set(context, "source", input_source_.empty() ? task_id_ : input_source_, a);
    }
    if (owner_->ResolveInvocation(Encode(request).c_str(), result, error)) return -1;
    if (runtime) {
        Json resolved;
        resolved.Parse(result->c_str());
        auto& a = resolved.GetAllocator();
        const auto& config = runtime->Config();
        auto& normalized = resolved["normalized_config"];
        Set(normalized, "core", Core(config, true, a), a);
        Set(normalized, "input_namespace", config.domains.input_namespace, a);
        Set(normalized, "source_id_as_domain", Value(config.domains.source_id_as_domain), a);
        Value domains(rapidjson::kArrayType);
        for (const auto& domain : config.domains.bindings) {
            Value binding(rapidjson::kObjectType);
            binding.AddMember("source_id", domain.source_id, a)
                .AddMember("observation_domain_id", domain.observation_domain_id, a);
            domains.PushBack(binding, a);
        }
        Set(normalized, "source_domains", std::move(domains), a);
        if (runtime->Config().features.labeling_enabled)
            Set(resolved["execution_context"], "label_selection_state", "checked", a);
        if (!semantics_context_.empty()) {
            Json context;
            context.Parse(semantics_context_.c_str());
            Set(resolved["execution_context"], "npm", Copy(context, a), a);
        }
        *result = Encode(resolved);
    }
    return 0;
}

int NpmBasicOperator::ResolveInvocation(const char* text, std::string* result, DataSemanticsErrorV1* error) {
    if (result) result->clear();
    if (error) *error = {};
    Json request;
    if (!text || !result || std::strlen(text) > 12 * 1024 * 1024 || !JsonNestingWithin(text, 64) ||
        request.Parse(text).HasParseError() || !request.IsObject() || !request.HasMember("parameters") ||
        !request["parameters"].IsObject() || !request.HasMember("inputs") || !request["inputs"].IsArray() ||
        !request.HasMember("execution_context") || !request["execution_context"].IsObject())
        return dataset::Fail(error, "CONTRACT_INVALID", "invalid NPM invocation");
    const auto& context = request["execution_context"];
    const bool managed_known = context.HasMember("managed_output") && context["managed_output"].IsBool();
    const bool managed = !managed_known || context["managed_output"].GetBool();
    const auto with = Encode(request["parameters"]);
    const bool labeling = NpmBasicTaskRequestsLabeling(with.c_str());
    NpmBasicTaskConfig config;
    const auto status = ParseNpmBasicTaskConfig(with.c_str(), &config, labeling, ProductionNpmModuleCatalogV1(),
                                                Get(context, "source", "unresolved"), managed);
    if (status.error != NpmBasicTaskConfigError::kNone)
        return dataset::Fail(error, "CONTRACT_INCOMPATIBLE", "NPM production parameter parser rejected invocation",
                             status.parameter_status.path.empty() ? "/parameters/" + status.field
                                                                  : "/parameters" + status.parameter_status.path);
    std::vector<NpmPreparedModuleV1> prepared;
    const auto modules = PrepareNpmModulesV1(config, ProductionNpmModuleCatalogV1(), &prepared);
    if (modules.error != NpmProtocolContractErrorV1::kNone)
        return dataset::Fail(error, "CONTRACT_INCOMPATIBLE", "NPM production module parser rejected invocation",
                             "/parameters/" + modules.field);
    for (const auto& module : prepared)
        if (module.plan.requires_labeling && !labeling)
            return dataset::Fail(error, "CONTRACT_INCOMPATIBLE", "module requires labeling", "/features/labeling");
    if (config.result_retention_days && managed_known && !managed)
        return dataset::Fail(error, "CONTRACT_INCOMPATIBLE", "retention requires a managed sink", "/managed_output");
    bool mode_known = config.run_mode_explicit;
    if (context.HasMember("finite_source") && context["finite_source"].IsBool()) {
        const auto mode = context["finite_source"].GetBool() ? NpmRunMode::kOffline : NpmRunMode::kRealtime;
        if (mode_known && mode != config.analysis.run_mode)
            return dataset::Fail(error, "CONTRACT_INCOMPATIBLE", "run_mode conflicts with source capability",
                                 "/parameters/run_mode");
        config.analysis.run_mode = mode;
        mode_known = true;
    }
    Json response(rapidjson::kObjectType);
    auto& a = response.GetAllocator();
    response.AddMember("effective_parameters", Copy(request["parameters"], a), a)
        .AddMember("inputs", Copy(request["inputs"], a), a)
        .AddMember("execution_context", Copy(context, a), a)
        .AddMember("config_refs", Value(rapidjson::kArrayType), a);
    if (labeling) {
        auto* provider =
            querier_ ? static_cast<IFlowLabelingProviderV1*>(querier_->First(IID_FLOW_LABELING_PROVIDER_V1)) : nullptr;
        FlowLabelingDiagnosticV1 diagnostic;
        if (!provider || provider->RuntimeStatus(&diagnostic) != FlowLabelingErrorV1::kNone)
            return dataset::Fail(error, "UNAVAILABLE", "NPM labeling provider unavailable", "/features/labeling");
        auto* registry = querier_
                             ? static_cast<IConfigChannelRegistryV1*>(querier_->First(IID_CONFIG_CHANNEL_REGISTRY_V1))
                             : nullptr;
        if (!registry) return dataset::Fail(error, "UNAVAILABLE", "config registry unavailable", "/core/labeling");
        ConfigChannelSnapshot snapshot;
        std::string detail;
        if (registry->Resolve(config.labeling_reference.c_str(), &snapshot, &detail))
            return dataset::Fail(error, "UNAVAILABLE", detail, "/core/labeling");
        response["config_refs"].PushBack(Value(config.labeling_reference.c_str(), a), a);
        Set(response["execution_context"], "label_selection_state", "not_checked", a);
    }
    Value normalized(rapidjson::kObjectType);
    normalized.AddMember("schema_version", 1, a).AddMember("core", Core(config, mode_known, a), a);
    if (!config.features.observing_entity.empty()) Set(normalized, "observing", config.features.observing_entity, a);
    if (Get(context, "source") != "" || request["parameters"].HasMember("input_namespace"))
        Set(normalized, "input_namespace", config.domains.input_namespace, a);
    Value domains(rapidjson::kArrayType);
    for (const auto& domain : config.domains.bindings) {
        Value binding(rapidjson::kObjectType);
        binding.AddMember("source_id", domain.source_id, a)
            .AddMember("observation_domain_id", domain.observation_domain_id, a);
        domains.PushBack(binding, a);
    }
    normalized.AddMember("source_id_as_domain", config.domains.source_id_as_domain, a)
        .AddMember("source_domains", domains, a);
    Value enabled(rapidjson::kArrayType);
    Json supplied;
    if (!config.parameters_json.empty()) supplied.Parse(config.parameters_json.c_str());
    for (const auto& module : prepared) {
        enabled.PushBack(Value(module.plan.module_id.c_str(), a), a);
        const auto& id = module.plan.module_id;
        const std::string json =
            supplied.IsObject() && supplied.HasMember(id.c_str()) ? Encode(supplied[id.c_str()]) : "{}";
        auto values = Module(id, json, a);
        if (module.plan.primary_label_ids) {
            Value ids(rapidjson::kArrayType);
            for (auto label : *module.plan.primary_label_ids) ids.PushBack(label, a);
            values.AddMember("primary_label_ids", ids, a);
        }
        if (id == "session")
            values.AddMember("max_tcp_ranges_per_direction", config.features.session_max_tcp_ranges_per_direction, a);
        Set(normalized, id.c_str(), std::move(values), a);
    }
    if (labeling) enabled.PushBack("labeling", a);
    normalized.AddMember("features", enabled, a);
    response.AddMember("normalized_config", normalized, a);
    *result = Encode(response);
    return 0;
}
}  // namespace flowsql::npm
