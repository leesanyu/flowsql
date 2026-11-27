// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <framework/interfaces/ischeduler_control_service.h>
#include "access_store.h"
#include "data_access_plugin.h"

#include <framework/interfaces/iblock_stream_channel_descriptor.h>
#include <framework/interfaces/iblock_stream_manager.h>
#include <framework/interfaces/ichannel_registry.h>
#include <framework/interfaces/iconfig_channel_registry.h>
#include <framework/interfaces/imetadata_description.h>
#include <framework/interfaces/irouter_handle.h>
#include <framework/interfaces/istream_channel.h>
#include <framework/interfaces/istream_factory.h>
#include <framework/interfaces/istream_manager.h>
#include <sys/stat.h>
#include <set>

namespace flowsql::access {
using namespace dataset;
bool DataAccessPlugin::Permitted(const Source& source, const std::string& operation) const {
    for (const auto& name : (*source.grant)["operations"].GetArray())
        if (name == operation.c_str()) return true;
    return false;
}
Source DataAccessPlugin::Describe(const Value& locator, const Value& grant) {
    Source source;
    source.kind = Required(locator, "kind");
    source.provider = Get(locator, "type", source.kind == "database_table" ? "database" : source.kind);
    source.name = Get(locator, "name", Get(locator, "reference"));
    source.locator = Canonical(locator);
    auto owned = std::make_shared<Json>();
    owned->CopyFrom(grant, owned->GetAllocator());
    source.grant = std::move(owned);
    Json detail(rapidjson::kObjectType);
    auto& a = detail.GetAllocator();
    Set(detail, "state", "provider_unavailable", a);
    Set(detail, "identity_state", "unknown", a);
    Set(detail, "schema_state", "unknown", a);
    Set(detail, "data_shape", source.kind == "config" ? "configuration" : "unknown", a);
    detail.AddMember("roles", Value(rapidjson::kArrayType), a);
    if (querier_ && (source.kind == "database_table" || source.kind == "dataframe")) {
        IChannelDatasetDescriptor* provider = nullptr;
        const auto provider_id = source.kind == "database_table" ? "database" : "dataframe";
        querier_->Traverse(IID_CHANNEL_DATASET_DESCRIPTOR, [&](void* pointer) {
            auto* candidate = static_cast<IChannelDatasetDescriptor*>(pointer);
            if (std::string(candidate->ProviderId()) == provider_id) {
                provider = candidate;
                return 1;
            }
            return 0;
        });
        if (provider) {
            ChannelDatasetSnapshot snapshot;
            DataSemanticsError error;
            const auto rc = provider->Describe({source.locator, 5000}, &snapshot, &error);
            if (rc == 0 && snapshot.document_json) {
                detail = Parse(*snapshot.document_json);
                auto& allocator = detail.GetAllocator();
                Set(detail, "state", "available", allocator);
                Set(detail, "data_shape", "tabular", allocator);
                Value roles(rapidjson::kArrayType);
                roles.PushBack("source", allocator);
                detail.AddMember("roles", roles, allocator);
                source.id = Get(detail["object_key"], "provider_id") + ":" + Get(detail["object_key"], "object_id");
                source.revision = Get(detail, "source_revision");
                source.available = true;
            } else {
                Set(detail, "state", error.code.empty() ? "source_unavailable" : error.code, a);
            }
        }
    } else if (querier_ && source.kind == "config") {
        auto* configs = static_cast<IConfigChannelRegistry*>(querier_->First(IID_CONFIG_CHANNEL_REGISTRY));
        if (configs) {
            ConfigChannelSnapshot snapshot;
            std::string error;
            if (configs->Resolve(Required(locator, "reference").c_str(), &snapshot, &error) == 0) {
                Set(detail, "state", "available", a);
                Set(detail, "format", snapshot.format, a);
                Set(detail, "schema_id", snapshot.schema_id, a);
                Set(detail, "reference", "config." + snapshot.channel_name + "@" + std::to_string(snapshot.revision),
                    a);
                Set(detail, "identity_state", "known", a);
                source.revision = snapshot.sha256_hex;
                source.available = true;
            } else
                Set(detail, "state", "source_unavailable", a);
        }
    } else if (querier_ && source.kind == "stream") {
        querier_->Traverse(IID_STREAM_FACTORY, [&](void* pointer) {
            static_cast<IStreamFactory*>(pointer)->List(
                [&](const char* type, const char* name, IStreamChannel* channel) {
                    if (source.provider != type || source.name != name) return;
                    source.available = true;
                    Set(detail, "state", "available", a);
                    Set(detail, "data_shape", "structured", a);
                    detail["roles"].PushBack("source", a);
                    detail.AddMember("finite", channel->IsFinite(), a);
                    Set(detail, "schema_state", channel->GetOutputSchema() ? "known" : "dynamic", a);
                    if (const auto schema = channel->GetOutputSchema()) Set(detail, "schema", schema->ToString(), a);
                    detail.AddMember("hub", channel->IsHubChannel(), a);
                });
            return 0;
        });
        querier_->Traverse(IID_STREAM_MANAGER, [&](void* pointer) {
            static_cast<IStreamManager*>(pointer)->QueryChannels(
                [&](const auto& type, const auto& name, const auto& options, const auto&) {
                    if (source.provider != type || source.name != name) return;
                    const auto config = Parse(options);
                    detail["roles"].Clear();
                    detail["roles"].PushBack(Value(Get(config, "role", "source").c_str(), a), a);
                    source.revision = Hash(source.locator + options);
                });
            return 0;
        });
    } else if (querier_ && source.kind == "block_stream") {
        querier_->Traverse(IID_BLOCK_STREAM_MANAGER, [&](void* pointer) {
            static_cast<IBlockStreamManager*>(pointer)->QueryChannels(
                [&](const auto& type, const auto& name, const auto& options, const auto& status) {
                    if (source.provider != type || source.name != name) return;
                    source.available = true;
                    Set(detail, "state", "available", a);
                    Set(detail, "runtime_state", status, a);
                    Set(detail, "data_shape", "packet", a);
                    Json config;
                    config.Parse(options.c_str());
                    const auto role = Get(config, "role", "source");
                    detail["roles"].PushBack(Value(role.c_str(), a), a);
                    source.revision = Hash(source.locator + options);
                    if (type == "pcapfile") {
                        detail.AddMember("finite", true, a);
                        detail.AddMember("supports_reset", true, a);
                        Set(detail, "schema_state", "known", a);
                        Set(detail, "schema", "flowsql.packet-family", a);
                        Set(detail, "format", Get(config, "format", "auto"), a);
                        struct stat file {};
                        if (stat(Get(config, "path").c_str(), &file) == 0) {
                            detail.AddMember("byte_count", static_cast<uint64_t>(file.st_size), a);
                            source.revision =
                                Hash(source.revision + std::to_string(file.st_dev) + ":" + std::to_string(file.st_ino) +
                                     ":" + std::to_string(file.st_size) + ":" + std::to_string(file.st_mtim.tv_sec) +
                                     ":" + std::to_string(file.st_mtim.tv_nsec));
                        } else {
                            source.available = false;
                            Set(detail, "state", "source_unavailable", a);
                        }
                    }
                    // Only public operating metadata leaves the provider. No options,
                    // paths, connection credentials or authentication profiles escape.
                    Set(detail, "channel_type", type, a);
                });
            return 0;
        });
        // Kafka owns a separate public definition catalog, including sink-only
        // objects which intentionally have no readable block-stream instance.
        if (source.provider == "kafka") {
            fnRouterHandler handler;
            const std::string path = "/channels/kafka/channels/query";
            querier_->Traverse(IID_ROUTER_HANDLE, [&](void* pointer) {
                static_cast<IRouterHandle*>(pointer)->EnumRoutes([&](const RouteItem& route) {
                    if (route.method == "POST" && route.uri == path) handler = route.handler;
                });
                return handler ? 1 : 0;
            });
            if (handler) {
                Json request(rapidjson::kObjectType);
                Set(request, "name", source.name, request.GetAllocator());
                std::string text;
                if (!handler(path, Encode(request), text)) {
                    auto catalog = Parse(text);
                    if (!catalog["items"].Empty()) {
                        const auto& definition = catalog["items"][0];
                        source.available = true;
                        Set(detail, "state", "available", a);
                        Set(detail, "data_shape", Get(definition, "payload_kind"), a);
                        detail["roles"].Clear();
                        detail["roles"].PushBack(Value(Get(definition, "role").c_str(), a), a);
                        detail.AddMember("finite", false, a);
                        Set(detail, "schema_state", Get(definition, "source_schema").empty() ? "unknown" : "known", a);
                        if (!Get(definition, "source_schema").empty())
                            Set(detail, "schema", Get(definition, "source_schema"), a);
                        source.revision = Hash(source.locator + Canonical(definition["revision"]) + ":" +
                                               Canonical(definition["connection_revision"]));
                    }
                } else
                    Set(detail, "state", "source_unavailable", a);
            }
        }
        querier_->Traverse(IID_BLOCK_STREAM_CHANNEL_DESCRIPTOR, [&](void* pointer) {
            static_cast<IBlockStreamChannelDescriptor*>(pointer)->DescribeChannelTypes([&](const auto& descriptor) {
                if (descriptor.channel_type == source.provider) {
                    if (!detail.HasMember("finite")) detail.AddMember("finite", descriptor.is_finite, a);
                    detail.AddMember("supports_reset", descriptor.supports_reset, a);
                }
            });
            return 0;
        });
    }
    if (source.id.empty()) source.id = source.kind + ":" + Hash(source.locator);
    if (source.revision.empty()) source.revision = Hash(Canonical(detail));
    // Metadata descriptions are a separate capability; absence is observable and
    // never replaced by invented units, field meanings or domain formulas.
    if (querier_ && source.available && (source.kind == "database_table" || source.kind == "dataframe")) {
        auto* descriptions = static_cast<IMetadataDescription*>(querier_->First(IID_METADATA_DESCRIPTION));
        if (descriptions) {
            Json request(rapidjson::kObjectType);
            auto& ra = request.GetAllocator();
            Value target(rapidjson::kObjectType);
            Set(target, "kind", "channel", ra);
            target.AddMember("locator", Copy(locator, ra), ra);
            request.AddMember("target", target, ra);
            std::string response;
            const auto rc = descriptions->Describe(Encode(request).c_str(), &response);
            if (!rc) {
                Json observed = Parse(response);
                if (observed.HasMember("data"))
                    detail.AddMember("semantics", Copy(observed["data"], detail.GetAllocator()), detail.GetAllocator());
            }
        }
    }
    source.description = Encode(detail);
    return source;
}
std::vector<Source> DataAccessPlugin::Sources(const Subject& subject) {
    std::vector<Source> result;
    for (const auto& grant : (*subject.policy)["sources"].GetArray())
        result.push_back(Describe(grant["locator"], grant));
    if (store_ && subject.policy->HasMember("analyses")) {
        auto outputs = store_->Outputs(subject.principal);
        std::set<std::string> seen;
        for (const auto& source : result) seen.insert(source.id);
        for (const auto& output : outputs.GetArray()) {
            for (const auto& profile : (*subject.policy)["analyses"].GetArray()) {
                if (Get(profile, "name") != Get(output, "analysis_name") || profile["output"] != output["output_scope"])
                    continue;
                Json grant(rapidjson::kObjectType);
                grant.AddMember("operations", Value(rapidjson::kArrayType), grant.GetAllocator());
                grant["operations"].PushBack("query", grant.GetAllocator());
                auto source = Describe(output["locator"], grant);
                if (seen.insert(source.id).second) result.push_back(std::move(source));
            }
        }
    }
    return result;
}
Source DataAccessPlugin::Resolve(const Subject& subject, const Value& reference) {
    const auto id = Required(reference, "source_id"), revision = Required(reference, "source_revision");
    for (const auto& source : Sources(subject)) {
        if (source.id != id) continue;
        if (source.revision != revision)
            throw AccessError("SOURCE_CHANGED", "source revision changed; discover again", 409);
        return source;
    }
    throw AccessError("FORBIDDEN", "source is outside this subject's grant", 403);
}
Json DataAccessPlugin::SourceJson(const Source& source) {
    Json output(rapidjson::kObjectType);
    auto& a = output.GetAllocator();
    Value ref(rapidjson::kObjectType);
    Set(ref, "source_id", source.id, a);
    Set(ref, "source_revision", source.revision, a);
    output.AddMember("source_ref", ref, a);
    Set(output, "kind", source.kind, a);
    Set(output, "provider", source.provider, a);
    Set(output, "name", source.name, a);
    output.AddMember("locator", Copy(Parse(source.locator), a), a);
    output.AddMember("description", Copy(Parse(source.description), a), a);
    auto& description = output["description"];
    if (description.HasMember("fields") && description["fields"].IsArray()) {
        size_t index = 0;
        for (auto& field : description["fields"].GetArray()) Set(field, "field_id", "f" + std::to_string(index++), a);
    }
    Value capabilities(rapidjson::kObjectType);
    const bool query = source.kind == "database_table" || source.kind == "dataframe";
    for (const auto* operation : {"describe", "query", "analyze", "consume", "capture", "publish", "write"}) {
        Value capability(rapidjson::kObjectType);
        const bool analysis = query || (source.kind == "block_stream" && source.provider == "pcapfile");
        const bool supported = std::string(operation) == "describe" || (std::string(operation) == "query" && query) ||
                               (std::string(operation) == "analyze" && analysis);
        const bool execution_ready =
            std::string(operation) != "analyze" || (querier_ && querier_->First(IID_SCHEDULER_CONTROL_SERVICE));
        capability.AddMember("supported", supported, a);
        const bool authorized = std::string(operation) == "describe" || Permitted(source, operation);
        capability.AddMember("authorized", authorized, a);
        capability.AddMember("available", supported && authorized && source.available && execution_ready, a);
        Set(capability, "reason",
            !supported          ? "UNSUPPORTED_OPERATION"
            : !authorized       ? "FORBIDDEN"
            : !source.available ? "SOURCE_UNAVAILABLE"
            : !execution_ready  ? "EXECUTOR_UNAVAILABLE"
                                : "",
            a);
        capabilities.AddMember(Value(operation, a), capability, a);
    }
    output.AddMember("capabilities", capabilities, a);
    return output;
}
}  // namespace flowsql::access
