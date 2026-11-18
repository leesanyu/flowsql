// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_module_catalog.h"
#include <operators/npm_basic/output/npm_result_router.h>

#include <operators/npm_basic/config/npm_basic_task_config.h>
#include <operators/npm_basic/modules/dns/npm_dns_contract.h>
#include <operators/npm_basic/modules/dns/npm_dns_module.h>
#include <operators/npm_basic/modules/http1/npm_http1_contract.h>
#include <operators/npm_basic/modules/http1/npm_http1_module.h>
#include <operators/npm_basic/modules/icmp/npm_icmp_contract.h>
#include <operators/npm_basic/modules/icmp/npm_icmp_module.h>
#include <operators/npm_basic/modules/session/npm_session_analysis_module.h>
#include <operators/npm_basic/modules/tls/npm_tls_contract.h>
#include <operators/npm_basic/modules/tls/npm_tls_module.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <cerrno>
#include <set>

namespace flowsql::npm {
namespace {
NpmProtocolContractStatus Invalid(std::string field) {
    return {NpmProtocolContractError::kInvalidPlan, std::move(field)};
}
}  // namespace

NpmProtocolContractStatus ValidateNpmModuleInstance(const NpmModulePlan& plan, const NpmModuleInstance& instance) {
    if ((instance.protocol && instance.analysis) ||
        (!instance.protocol && !instance.analysis && plan.module_id != "basic")) {
        return Invalid(plan.module_id);
    }
    if (plan.requires_tcp_stream != (instance.tcp_stream_consumer != nullptr) ||
        (instance.tcp_stream_consumer != nullptr &&
         dynamic_cast<INpmTcpStreamConsumer*>(instance.protocol.get()) != instance.tcp_stream_consumer)) {
        return Invalid(plan.module_id + "/tcp_stream_consumer");
    }
    return {};
}

const NpmModuleCatalog& ProductionNpmModuleCatalog() {
    static const NpmModuleCatalog catalog = {
        {"basic",
         true,
         {"basic_ipv4", "basic_ipv6"},
         [](const NpmBasicTaskConfig& config, std::string_view, NpmPreparedModule* out) {
             out->plan.module_id = "basic";
             out->plan.input_mask = 3;
             out->plan.entities = {NpmBasicEntityDescriptor(packet::AddressFamily::kIPv4),
                                   NpmBasicEntityDescriptor(packet::AddressFamily::kIPv6)};
             out->create = [](NpmProtocolContext&, std::shared_ptr<INpmTaskBudget>) { return NpmModuleInstance{}; };
             return NpmProtocolContractStatus{};
         }},
        {"session",
         true,
         {"session_ipv4", "session_ipv6"},
         [](const NpmBasicTaskConfig& config, std::string_view, NpmPreparedModule* out) {
             out->plan.module_id = "session";
             out->plan.input_mask = 3;
             out->plan.entities = {NpmSessionEntityDescriptor(packet::AddressFamily::kIPv4),
                                   NpmSessionEntityDescriptor(packet::AddressFamily::kIPv6)};
             const auto ranges = config.features.session_max_tcp_ranges_per_direction;
             out->create = [ranges](NpmProtocolContext& context, std::shared_ptr<INpmTaskBudget> budget) {
                 NpmModuleInstance result;
                 result.analysis = std::make_unique<NpmSessionAnalysisModule>(context, std::move(budget), ranges);
                 return result;
             };
             return NpmProtocolContractStatus{};
         }},
        {"dns",
         true,
         {"dns_transaction_ipv4", "dns_transaction_ipv6"},
         [](const NpmBasicTaskConfig&, std::string_view json, NpmPreparedModule* out) {
             NpmDnsConfig config;
             const auto parsed = ParseNpmDnsConfig(json, &config);
             if (parsed.error != NpmDnsConfigError::kNone) {
                 const std::string prefix = "/dns/";
                 return Invalid(parsed.path == "/dns" ? "config" : parsed.path.substr(prefix.size()));
             }
             out->plan = NpmDnsModulePlan(config);
             out->create = [config = std::move(config)](NpmProtocolContext&, std::shared_ptr<INpmTaskBudget> budget) {
                 NpmModuleInstance result;
                 auto module = std::make_unique<NpmDnsProtocolModule>(config, std::move(budget));
                 result.tcp_stream_consumer = module.get();
                 result.protocol = std::move(module);
                 return result;
             };
             return NpmProtocolContractStatus{};
         }},
        {"http1",
         true,
         {"http1_transaction_ipv4", "http1_transaction_ipv6"},
         [](const NpmBasicTaskConfig&, std::string_view json, NpmPreparedModule* out) {
             NpmHttp1Config config;
             const auto parsed = ParseNpmHttp1Config(json, &config);
             if (parsed.error != NpmHttp1ConfigError::kNone) {
                 const std::string prefix = "/http1/";
                 return Invalid(parsed.path == "/http1" ? "config" : parsed.path.substr(prefix.size()));
             }
             out->plan = NpmHttp1ModulePlan(config);
             out->create = [config = std::move(config)](NpmProtocolContext&, std::shared_ptr<INpmTaskBudget> budget) {
                 NpmModuleInstance result;
                 auto module = std::make_unique<NpmHttp1ProtocolModule>(config, std::move(budget));
                 result.tcp_stream_consumer = module.get();
                 result.protocol = std::move(module);
                 return result;
             };
             return NpmProtocolContractStatus{};
         }},
        {"tls",
         true,
         {"tls_handshake_ipv4", "tls_handshake_ipv6"},
         [](const NpmBasicTaskConfig&, std::string_view json, NpmPreparedModule* out) {
             NpmTlsConfig config;
             const auto parsed = ParseNpmTlsConfig(json, &config);
             if (parsed.error != NpmTlsConfigError::kNone) {
                 const std::string prefix = "/tls/";
                 return Invalid(parsed.path == "/tls" ? "config" : parsed.path.substr(prefix.size()));
             }
             out->plan = NpmTlsModulePlan(config);
             out->create = [config = std::move(config)](NpmProtocolContext&, std::shared_ptr<INpmTaskBudget> budget) {
                 NpmModuleInstance result;
                 auto module = std::make_unique<NpmTlsProtocolModule>(config, std::move(budget));
                 result.tcp_stream_consumer = module.get();
                 result.protocol = std::move(module);
                 return result;
             };
             return NpmProtocolContractStatus{};
         }},
        {"icmp",
         true,
         {"icmp_event_ipv4", "icmp_event_ipv6"},
         [](const NpmBasicTaskConfig&, std::string_view json, NpmPreparedModule* out) {
             NpmIcmpConfig config;
             const auto parsed = ParseNpmIcmpConfig(json, &config);
             if (parsed.error != NpmIcmpConfigError::kNone) {
                 const std::string prefix = "/icmp/";
                 return Invalid(parsed.path == "/icmp" ? "config" : parsed.path.substr(prefix.size()));
             }
             out->plan = NpmIcmpModulePlan();
             out->create = [config](NpmProtocolContext&, std::shared_ptr<INpmTaskBudget> budget) {
                 NpmModuleInstance result;
                 result.protocol = std::make_unique<NpmIcmpProtocolModule>(config, std::move(budget));
                 return result;
             };
             return NpmProtocolContractStatus{};
         }}};
    return catalog;
}

NpmProtocolContractStatus ValidateNpmModuleCatalog(const NpmModuleCatalog& catalog) {
    std::set<std::string> modules, entities;
    for (const auto& entry : catalog) {
        if (entry.module_id.empty() || entry.module_id == "labeling" || !modules.insert(entry.module_id).second ||
            entry.entity_ids.empty() || !entry.prepare)
            return Invalid(entry.module_id);
        for (const auto& id : entry.entity_ids) {
            if (id.empty() || id == "labeling" || !entities.insert(id).second)
                return Invalid(entry.module_id + "/" + id);
        }
    }
    return {};
}

NpmProtocolContractStatus PrepareNpmModules(const NpmBasicTaskConfig& config, const NpmModuleCatalog& catalog,
                                            std::vector<NpmPreparedModule>* output) {
    auto status = ValidateNpmModuleCatalog(catalog);
    if (status.error != NpmProtocolContractError::kNone) return status;
    std::vector<std::string> ids = config.features.module_ids;
    if (ids.empty()) {
        if (config.features.basic_enabled) ids.push_back("basic");
        if (config.features.session_enabled) ids.push_back("session");
    }
    if (ids.empty()) return Invalid("features");
    std::set<std::string> enabled;
    for (const auto& id : ids) {
        const auto found =
            std::find_if(catalog.begin(), catalog.end(), [&](const auto& entry) { return entry.module_id == id; });
        if (!enabled.insert(id).second || found == catalog.end() || !found->available) return Invalid(id);
    }
    rapidjson::Document parameters;
    if (!config.parameters_json.empty()) {
        parameters.Parse(config.parameters_json.c_str());
        if (parameters.HasParseError() || !parameters.IsObject()) return Invalid("parameters");
    }
    std::vector<NpmPreparedModule> next;
    for (const auto& entry : catalog) {
        if (!enabled.count(entry.module_id)) continue;
        std::string json = "{}";
        if (parameters.IsObject() && parameters.HasMember(entry.module_id.c_str())) {
            rapidjson::StringBuffer buffer;
            rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
            parameters[entry.module_id.c_str()].Accept(writer);
            json.assign(buffer.GetString(), buffer.GetSize());
        }
        NpmPreparedModule prepared;
        status = entry.prepare(config, json, &prepared);
        if (status.error != NpmProtocolContractError::kNone) {
            status.field = entry.module_id + "/" + status.field;
            return status;
        }
        if (prepared.plan.module_id != entry.module_id || !prepared.create ||
            prepared.plan.entities.size() != entry.entity_ids.size())
            return Invalid(entry.module_id);
        for (const auto& entity : prepared.plan.entities) {
            if (std::find(entry.entity_ids.begin(), entry.entity_ids.end(), entity.entity_id) == entry.entity_ids.end())
                return Invalid(entry.module_id + "/" + entity.entity_id);
        }
        next.push_back(std::move(prepared));
    }
    *output = std::move(next);
    return {};
}

NpmProtocolModuleAdapter::NpmProtocolModuleAdapter(NpmModulePlan plan, std::unique_ptr<INpmProtocolModule> module,
                                                   const std::atomic<bool>* cancellation_requested,
                                                   NpmResultRouter* router)
    : router_(router),
      plan_(std::move(plan)),
      module_(std::move(module)),
      cancellation_requested_(cancellation_requested) {}
NpmProtocolModuleAdapter::~NpmProtocolModuleAdapter() { Abort(); }
bool NpmProtocolModuleAdapter::AcceptsControl() const { return (plan_.input_mask & 4) != 0; }
int NpmProtocolModuleAdapter::OnControl(const NpmInputEvent& event) {
    if (!AcceptsControl()) return 0;
    if (cancellation_requested_->load(std::memory_order_acquire)) return ECANCELED;
    const int error = module_->OnInput(event, *this);
    return error == 0 && cancellation_requested_->load(std::memory_order_acquire) ? ECANCELED : error;
}
std::optional<int64_t> NpmProtocolModuleAdapter::NextEventDeadlineNs() const {
    return finished_ || aborted_ ? std::nullopt : module_->NextEventDeadlineNs();
}
int NpmProtocolModuleAdapter::OnTime(const NpmModuleTime& time) {
    if (finished_ || aborted_ || cancellation_requested_->load(std::memory_order_acquire)) return ECANCELED;
    const int error = module_->OnTime(time, *this);
    return error == 0 && cancellation_requested_->load(std::memory_order_acquire) ? ECANCELED : error;
}
int NpmProtocolModuleAdapter::Finish(int64_t observed_at_ns) {
    if (finished_ || aborted_ || cancellation_requested_->load(std::memory_order_acquire)) return ECANCELED;
    const int error = module_->Finish(observed_at_ns, *this);
    if (error == 0 && !cancellation_requested_->load(std::memory_order_acquire)) {
        finished_ = true;
    } else if (error == 0) {
        return ECANCELED;
    }
    return error;
}
void NpmProtocolModuleAdapter::Abort() noexcept {
    if (finished_ || aborted_) return;
    aborted_ = true;
    module_->Abort();
}
bool NpmProtocolModuleAdapter::AcceptsSession(const NpmSessionView& session) const {
    const uint8_t mask = session.key->Protocol() == 6 ? 1 : 2;
    if ((plan_.input_mask & mask) == 0) return false;
    return !plan_.primary_label_ids || std::find(plan_.primary_label_ids->begin(), plan_.primary_label_ids->end(),
                                                 session.primary_label_id) != plan_.primary_label_ids->end();
}
int NpmProtocolModuleAdapter::OnPacket(const NpmPacketView& packet, const NpmSessionView& session, INpmResultWriter&) {
    if (!AcceptsSession(session)) return 0;
    if (cancellation_requested_->load(std::memory_order_acquire)) return ECANCELED;
    NpmInputEvent event;
    event.kind = session.key->Protocol() == 6 ? NpmInputKind::kTcpPacket : NpmInputKind::kUdpDatagram;
    event.observation_domain_id = session.key->Domain();
    event.packet = packet.packet;
    event.layer = packet.layer;
    event.body = packet.payload;
    event.body_complete = packet.transport.payload_complete;
    event.transport = &packet;
    event.session = &session;
    const int error = module_->OnInput(event, *this);
    return error == 0 && cancellation_requested_->load(std::memory_order_acquire) ? ECANCELED : error;
}
int NpmProtocolModuleAdapter::OnSessionSnapshot(const NpmSessionView& session, int64_t time, INpmResultWriter&) {
    if (!AcceptsSession(session)) return 0;
    if (cancellation_requested_->load(std::memory_order_acquire)) return ECANCELED;
    const int error = module_->OnSessionSnapshot(session, time, *this);
    return error == 0 && cancellation_requested_->load(std::memory_order_acquire) ? ECANCELED : error;
}
int NpmProtocolModuleAdapter::OnSessionEnd(const NpmSessionView& session, NpmSessionEndReason reason, int64_t time,
                                           INpmResultWriter&) {
    if (!AcceptsSession(session)) return 0;
    if (cancellation_requested_->load(std::memory_order_acquire)) return ECANCELED;
    const int error = module_->OnSessionEnd(session, reason, time, *this);
    return error == 0 && cancellation_requested_->load(std::memory_order_acquire) ? ECANCELED : error;
}
int NpmProtocolModuleAdapter::Emit(std::string_view entity, const arrow::RecordBatch& rows) {
    return router_->Emit(plan_.module_id, entity, rows);
}
}  // namespace flowsql::npm
