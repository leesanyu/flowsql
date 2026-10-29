// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include "baseliner_data_contract.h"
#include <cerrno>
#include <common/database_snapshot_types.hpp>
#include <common/operator_contract_execution.hpp>
#include <cstring>
#include "dataframe_reader.h"

namespace flowsql::baseliner {
namespace {
using namespace dataset;
bool Matches(const Value& source, const Value& wanted) {
    if (!source.IsObject()) return false;
    for (const auto& m : wanted.GetObject())
        if (Get(source, Text(m.name).c_str()) != Text(m.value)) return false;
    return true;
}
std::string PathToken(const std::string& text) {
    std::string escaped;
    for (const char c : text) escaped += c == '~' ? "~0" : c == '/' ? "~1" : std::string(1, c);
    return escaped;
}
int Inputs(IQuerier* querier, const TaskConfig& config, const Value& request, Json* response,
           DataSemanticsErrorV1* error) {
    auto& a = response->GetAllocator();
    Value inputs(rapidjson::kArrayType), shapes(rapidjson::kArrayType);
    const auto dot = config.source.find('.'), next = config.source.find('.', dot + 1);
    const auto category = config.source.substr(0, dot);
    const auto name = config.source.substr(dot + 1, next == std::string::npos ? std::string::npos : next - dot - 1);
    for (size_t i = 0; i < config.datasets.size(); ++i) {
        const auto& d = config.datasets[i];
        Value locator(rapidjson::kObjectType);
        Set(locator, "kind", config.dataframe_source ? "dataframe" : "database_table", a);
        Set(locator, "name", name, a);
        if (config.database_source) {
            Set(locator, "type", category, a);
            Set(locator, "table", d.table, a);
            if (!d.schema.empty()) Set(locator, "schema", d.schema, a);
        }
        Json snapshot;
        std::string binding;
        // Retain a caller's frozen real snapshot, rather than rediscovering an
        // object under its name after the execution has captured its buffers.
        for (const auto& input : request["inputs"].GetArray()) {
            if (!input.IsObject() || !input.HasMember("snapshot")) continue;
            const auto& observed = input["snapshot"];
            if (observed.HasMember("locator") && Matches(observed["locator"], locator)) {
                snapshot.CopyFrom(observed, snapshot.GetAllocator());
                binding = Get(input, "binding_ref");
                break;
            }
        }
        if (snapshot.IsNull()) {
            if (config.database_source || config.dataframe_source) {
                if (contracts::DescribeInput(querier, locator, {}, &snapshot, &binding, error)) return -1;
            } else {
                Value metadata(rapidjson::kObjectType), fields;
                Set(metadata, "observation", "raw dataset unavailable at this entry", a);
                snapshot = Snapshot("unobserved_operator_input", Hash(config.source + "/" + d.id), locator, fields,
                                    metadata, "", "unknown", "pending");
            }
        }
        Value adaptations(rapidjson::kArrayType);
        if (Get(snapshot, "schema_state") == "known") {
            for (const auto& [column, logical] : d.fields) {
                const Value* actual = nullptr;
                for (const auto& field : snapshot["fields"].GetArray())
                    if (Get(field, "name") == column) actual = &field;
                const auto path = "/datasets/" + std::to_string(i) + "/fields/" + PathToken(column);
                if (!actual)
                    return dataset::Fail(error, "CONTRACT_INCOMPATIBLE", "configured input column missing", path);
                if ((*actual)["type"].IsNull()) continue;
                const auto& physical = (*actual)["type"];
                const auto target = ArrowType(logical);
                const auto& context = request["execution_context"];
                const bool empty = context.HasMember("source_row_count") && context["source_row_count"].IsUint64() &&
                                   context["source_row_count"].GetUint64() == 0;
                const bool convertible =
                    config.dataframe_source
                        ? Get(physical, "system") == "arrow" &&
                              CanConvertDataFrameType(Get(physical, "name"), logical, empty)
                        : config.database_source && Get(physical, "system") == "database" &&
                              DatabaseSnapshotPhysicalType(category, Get(physical, "name"), target->id());
                if (!convertible)
                    return dataset::Fail(error, "CONTRACT_INCOMPATIBLE", "production reader cannot adapt input type",
                                         path);
                Value adaptation(rapidjson::kObjectType);
                Set(adaptation, "column", column, a);
                Set(adaptation, "source_type", Copy(physical, a), a);
                Set(adaptation, "target_type", Type("arrow", target->ToString(), a), a);
                Set(adaptation, "target_nullable", Value(true), a);
                Set(adaptation, "basis",
                    config.dataframe_source ? "baseliner.DataFrameReader" : "database.snapshot_reader", a);
                adaptations.PushBack(adaptation, a);
            }
        }
        Value input(rapidjson::kObjectType);
        Set(input, "port_id", "datasets", a);
        Set(input, "item_key", d.id, a);
        input.AddMember("input_index", static_cast<uint64_t>(i), a)
            .AddMember("snapshot", Copy(snapshot, a), a)
            .AddMember("adaptations", adaptations, a);
        if (!binding.empty()) Set(input, "binding_ref", binding, a);
        inputs.PushBack(input, a);
        shapes.PushBack("record_batch", a);
    }
    Set(*response, "inputs", std::move(inputs), a);
    Set((*response)["execution_context"], "input_shapes", std::move(shapes), a);
    return 0;
}
}  // namespace
int ResolveContractInvocation(IQuerier* querier, const ConfigSnapshot* frozen, const char* text, std::string* result,
                              DataSemanticsErrorV1* error) {
    using namespace dataset;
    if (result) result->clear();
    if (error) *error = {};
    Json request;
    if (!text || !result || std::strlen(text) > 12 * 1024 * 1024 || !JsonNestingWithin(text, 64) ||
        request.Parse(text).HasParseError() || !request.IsObject() || !request.HasMember("parameters") ||
        !request["parameters"].IsObject() || !request.HasMember("inputs") || !request["inputs"].IsArray() ||
        !request.HasMember("execution_context") || !request["execution_context"].IsObject())
        return dataset::Fail(error, "CONTRACT_INVALID", "invalid Baseliner invocation");
    Json response(rapidjson::kObjectType);
    auto& a = response.GetAllocator();
    response.AddMember("effective_parameters", Copy(request["parameters"], a), a)
        .AddMember("inputs", Copy(request["inputs"], a), a)
        .AddMember("config_refs", Value(rapidjson::kArrayType), a)
        .AddMember("execution_context", Copy(request["execution_context"], a), a);
    ConfigSnapshot parsed;
    if (!frozen) {
        auto source = Get(request["execution_context"], "source");
        if (source.empty()) {
            *result = Encode(response);
            return 0;
        }
        auto* registry =
            querier ? static_cast<IConfigChannelRegistryV1*>(querier->First(IID_CONFIG_CHANNEL_REGISTRY_V1)) : nullptr;
        const bool preflight =
            request["execution_context"].HasMember("preflight") && request["execution_context"]["preflight"].IsTrue();
        if (preflight && !request["inputs"].Empty()) {
            // Read the existing configuration source, then let the production
            // parser validate every value. A collection may bind a channel while
            // the F2 request names its first concrete table.
            Json configured;
            const auto inline_json = Get(request["parameters"], "parameters");
            if (!inline_json.empty()) configured.Parse(inline_json.c_str());
            const auto reference = Get(request["parameters"], "config");
            if (!reference.empty() && registry) {
                ConfigChannelSnapshot snapshot;
                std::string detail;
                const int rc = registry->Resolve(reference.c_str(), &snapshot, &detail);
                if (rc) return dataset::Fail(error, rc == ENOENT ? "NOT_FOUND" : "UNAVAILABLE", detail, "/config");
                if (snapshot.content) configured.Parse(snapshot.content->c_str());
            }
            const auto declared_source = Get(configured, "source");
            if (!declared_source.empty()) {
                const auto& locator = request["inputs"][0]["snapshot"]["locator"];
                const auto base =
                    (Get(locator, "kind") == "dataframe" ? std::string("dataframe") : Get(locator, "type")) + "." +
                    Get(locator, "name");
                if (declared_source != base && declared_source != source)
                    return dataset::Fail(error, "CONTRACT_INCOMPATIBLE",
                                         "configured source differs from selected input", "/inputs/0/locator");
                source = declared_source;
                Set(response["execution_context"], "source", source, a);
            }
        }
        struct Resolution : IConfigChannelRegistryV1 {
            IConfigChannelRegistryV1* registry = nullptr;
            bool attempted = false;
            int rc = 0;
            int Resolve(const char* ref, ConfigChannelSnapshot* out, std::string* error) override {
                attempted = true;
                if (!registry) {
                    if (error) *error = "Config registry unavailable";
                    return rc = EIO;
                }
                return rc = registry->Resolve(ref, out, error);
            }
        } resolution;
        resolution.registry = registry;
        const auto status = ResolveConfig(Encode(request["parameters"]), source, &resolution, &parsed);
        if (!status.ok()) {
            const bool reference = resolution.attempted && resolution.rc;
            return dataset::Fail(error,
                                 reference ? (registry && resolution.rc == ENOENT ? "NOT_FOUND" : "UNAVAILABLE")
                                           : "CONTRACT_INCOMPATIBLE",
                                 status.message, status.path);
        }
        frozen = &parsed;
    }
    Value effective(rapidjson::kObjectType);
    if (frozen->exact_reference.empty())
        Set(effective, "parameters", frozen->original_json, a);
    else {
        Set(effective, "config", frozen->exact_reference, a);
        response["config_refs"].PushBack(Value(frozen->exact_reference.c_str(), a), a);
    }
    if (!frozen->model_output.empty()) Set(effective, "model_output", frozen->model_output, a);
    Set(response, "effective_parameters", std::move(effective), a);
    Json normalized;
    normalized.Parse(EffectiveConfigJson(frozen->config).c_str());
    response.AddMember("normalized_config", Copy(normalized, a), a);
    auto& context = response["execution_context"];
    Set(context, "model_output_enabled", Value(!frozen->model_output.empty()), a);
    bool relation = false;
    for (const auto& dataset : frozen->config.datasets)
        for (const auto& metric : dataset.metrics) relation |= metric.kind == BaselineTaskKind::kRelation;
    const bool managed = context.HasMember("managed_output") && context["managed_output"].IsTrue();
    if (context.HasMember("managed_output") || !relation)
        Set(context, "relation_fusion_enabled", Value(managed && relation), a);
    if (Inputs(querier, frozen->config, request, &response, error)) return -1;
    if (context.HasMember("preflight") && context["preflight"].IsTrue()) {
        for (rapidjson::SizeType i = 0; i < request["inputs"].Size(); ++i) {
            if (i >= response["inputs"].Size() ||
                !Matches(request["inputs"][i]["snapshot"]["locator"], response["inputs"][i]["snapshot"]["locator"]))
                return dataset::Fail(error, "CONTRACT_INCOMPATIBLE", "selected input differs from configured dataset",
                                     "/inputs");
        }
    }
    *result = Encode(response);
    return 0;
}
}  // namespace flowsql::baseliner
