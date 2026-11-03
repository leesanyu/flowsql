// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <arrow/api.h>
#include <channels/config/operator_data_contract_model.h>
#include <channels/config/operator_usage.h>
#include <framework/interfaces/iflow_labeling.h>
#include <framework/interfaces/ioperator_data_contract.h>
#include <operators/npm_basic/config/npm_basic_task_config.h>
#include <operators/npm_basic/npm_basic_operator.h>
#include <operators/npm_basic/npm_family_result_contract.h>
#include <rapidjson/pointer.h>
#include <cassert>
#include <iostream>
#include <set>

extern "C" const char* flowsql_operator_data_contract_v1(int index);
using namespace flowsql;
using namespace flowsql::dataset;
using namespace flowsql::channels::config;

namespace {
struct ConfigurationServices : IQuerier, IFlowLabelingProviderV1, IConfigChannelRegistryV1 {
    bool available = true;
    int resolves = 0;
    int Traverse(const Guid&, fntraverse) override { return 0; }
    void* First(const Guid& id) override {
        if (memcmp(&id, &IID_FLOW_LABELING_PROVIDER_V1, sizeof(Guid)) == 0)
            return static_cast<IFlowLabelingProviderV1*>(this);
        if (memcmp(&id, &IID_CONFIG_CHANNEL_REGISTRY_V1, sizeof(Guid)) == 0)
            return static_cast<IConfigChannelRegistryV1*>(this);
        return nullptr;
    }
    FlowLabelingErrorV1 RuntimeStatus(FlowLabelingDiagnosticV1*) const override {
        return available ? FlowLabelingErrorV1::kNone : FlowLabelingErrorV1::kUnavailable;
    }
    FlowLabelingErrorV1 CreateMatcher(const FlowLabelingCompileRequestV1&, IFlowLabelMatcherV1**,
                                      FlowLabelingDiagnosticV1*) override {
        assert(false && "metadata resolution must not compile a matcher");
        return FlowLabelingErrorV1::kUnavailable;
    }
    int Resolve(const char* ref, ConfigChannelSnapshot* out, std::string*) override {
        assert(std::string(ref) == "config.labels@7");
        ++resolves;
        out->revision = 7;
        out->content = std::make_shared<const std::string>("labeling snapshot");
        return 0;
    }
};

void CheckConfigAdapter() {
    npm::NpmBasicOperator provider(nullptr);
    IPlugin* plugin = &provider;
    auto* adapter = dynamic_cast<IOperatorConfigAdapterV1*>(plugin);
    assert(adapter && "NPM must expose production configuration resolution");
    Json request;
    request.Parse(
        R"({"operator":{"category":"npm","name":"basic"},"parameters":{"observing":"basic_ipv6"},"inputs":[],"execution_context":{"source":"pcapfile.sample","managed_output":false,"finite_source":true}})");
    std::string response;
    DataSemanticsErrorV1 error;
    assert(adapter->ResolveInvocation(Encode(request).c_str(), &response, &error) == 0);
    Json resolved;
    resolved.Parse(response.c_str());
    const auto& normalized = resolved["normalized_config"];
    assert(normalized["observing"] == "basic_ipv6");
    assert(normalized["core"]["run_mode"] == "offline");
    assert(normalized["core"]["output_interval_ns"].GetInt64() == npm::kNpmDefaultOutputIntervalNs);
    assert(normalized["core"]["max_active_sessions"].GetUint64() == npm::kNpmDefaultActiveSessions);
    assert(normalized["input_namespace"] == "pcapfile.sample");
    assert(resolved["inputs"] == request["inputs"] && resolved["config_refs"].Empty());
    assert(ValidateOperatorParameters(Json().Parse(flowsql_operator_data_contract_v1(0)),
                                      resolved["effective_parameters"], &error) == 0);
    // No guessed source mode in a metadata-only request without capability facts.
    request["execution_context"].RemoveMember("finite_source");
    assert(adapter->ResolveInvocation(Encode(request).c_str(), &response, &error) == 0);
    resolved.Parse(response.c_str());
    assert(!resolved["normalized_config"]["core"].HasMember("run_mode"));
    request["parameters"].RemoveMember("observing");
    request["execution_context"]["managed_output"].SetBool(true);
    assert(adapter->ResolveInvocation(Encode(request).c_str(), &response, &error) == 0);
    resolved.Parse(response.c_str());
    assert(!resolved["normalized_config"].HasMember("observing"));
    request["execution_context"]["managed_output"].SetBool(false);
    assert(adapter->ResolveInvocation(Encode(request).c_str(), &response, &error) != 0 && response.empty());
    assert(error.code == "CONTRACT_INCOMPATIBLE");
    Set(request["parameters"], "observing", "dns_transaction_ipv4", request.GetAllocator());
    Set(request["parameters"], "features", "dns", request.GetAllocator());
    Set(request["parameters"], "parameters", R"({"schema_version":1,"dns":{"primary_label_ids":[53]}})",
        request.GetAllocator());
    assert(adapter->ResolveInvocation(Encode(request).c_str(), &response, &error) != 0);
    assert(error.field_path.find("labeling") != std::string::npos);
    Set(request["parameters"], "features", "icmp", request.GetAllocator());
    Set(request["parameters"], "observing", "icmp_event_ipv4", request.GetAllocator());
    assert(adapter->ResolveInvocation(Encode(request).c_str(), &response, &error) == 0);
    resolved.Parse(response.c_str());
    assert(resolved["normalized_config"]["icmp"]["echo_timeout_ns"].GetInt64() == 5000000000LL);
    Set(request["parameters"], "parameters", R"({"schema_version":1,"icmp":{"max_pending_echo":0}})",
        request.GetAllocator());
    assert(adapter->ResolveInvocation(Encode(request).c_str(), &response, &error) != 0 && response.empty());
    assert(error.field_path.find("icmp") != std::string::npos);
    ConfigurationServices services;
    npm::NpmBasicOperator configured(&services);
    request.Parse(
        R"({"parameters":{"features":"dns,labeling","observing":"dns_transaction_ipv6","parameters":"{\"schema_version\":1,\"core\":{\"labeling\":\"config.labels@7\"},\"dns\":{\"primary_label_ids\":[53]}}"},"inputs":[{"port_id":"packets","input_index":0,"snapshot":{"source_revision":"actual-revision"}}],"execution_context":{"source":"pcapfile.fixture","managed_output":true,"finite_source":true}})");
    assert(configured.ResolveInvocation(Encode(request).c_str(), &response, &error) == 0);
    resolved.Parse(response.c_str());
    assert(services.resolves == 1 && resolved["config_refs"][0] == "config.labels@7");
    assert(resolved["inputs"] == request["inputs"]);
    assert(resolved["normalized_config"]["dns"]["response_timeout_ns"].GetInt64() == 5000000000LL);
    assert(resolved["normalized_config"]["dns"]["primary_label_ids"][0].GetUint() == 53);
    assert(resolved["execution_context"]["label_selection_state"] == "not_checked");
    services.available = false;
    assert(configured.ResolveInvocation(Encode(request).c_str(), &response, &error) != 0 && response.empty());
    assert(error.code == "UNAVAILABLE");
    request.Parse(
        R"({"parameters":{"observing":"basic_ipv4","output_interval_ns":"12000000","max_active_sessions":"42","run_mode":"offline"},"inputs":[],"execution_context":{"finite_source":true,"managed_output":false}})");
    assert(adapter->ResolveInvocation(Encode(request).c_str(), &response, &error) == 0);
    resolved.Parse(response.c_str());
    assert(resolved["normalized_config"]["core"]["output_interval_ns"].GetInt64() == 12000000);
    assert(resolved["normalized_config"]["core"]["max_active_sessions"].GetUint64() == 42);
    request["execution_context"]["finite_source"].SetBool(false);
    assert(adapter->ResolveInvocation(Encode(request).c_str(), &response, &error) != 0);
    assert(error.field_path == "/parameters/run_mode");
    for (const char* params :
         {R"({"observing":"basic_ipv4","max_active_sessions":"0"})", R"({"observing":"basic_ipv4","result_mode":1})",
          R"({"observing":"session_ipv4"})", R"({"observing":"basic_ipv4","features":"basic,basic"})"}) {
        Json supplied;
        supplied.Parse(params);
        npm::NpmBasicTaskConfig production;
        assert(npm::ParseNpmBasicTaskConfig(params, &production).error != npm::NpmBasicTaskConfigError::kNone);
        Set(request, "parameters", Copy(supplied, request.GetAllocator()), request.GetAllocator());
        assert(adapter->ResolveInvocation(Encode(request).c_str(), &response, &error) != 0 && response.empty());
    }
}

const Value& Field(const Value& dictionary, const char* name) {
    for (const auto& field : dictionary["fields"].GetArray())
        if (Get(field, "field_id") == name) return field;
    assert(false && "missing field semantics");
    std::abort();
}

std::set<std::string> Codes(const Value& field) {
    std::set<std::string> codes;
    for (const auto& item : field["value_domain"]["value"]["items"].GetArray())
        assert(codes.insert(Get(item, "code")).second);
    return codes;
}

void CheckDnsDictionary(const Value& dictionary, const npm::NpmEntityDescriptorV1& entity) {
    Json normalized;
    std::vector<SemanticsReference> references;
    DataSemanticsErrorV1 error;
    const auto rc = NormalizeSemantics("dictionary", Encode(dictionary), &normalized, &references, &error);
    if (rc) std::cerr << error.field_path << " " << error.message << '\n';
    assert(rc == 0 && references.empty());
    assert(normalized["fields"].Size() == 25);
    assert(dictionary["entity"]["row_semantics"]["value"] == "event");
    assert(dictionary["entity"]["identity_fields"]["value"].Size() == 1);
    assert(dictionary["entity"]["identity_fields"]["value"][0] == "entity_instance_id");
    assert(dictionary["entity"]["revision_field"]["value"] == "revision");
    assert(dictionary["entity"]["relations"]["state"] == "unknown");
    assert(Encode(dictionary["entity"]["limitations"]).find("run_id") != std::string::npos);
    for (int i = 0; i < entity.schema->num_fields(); ++i) {
        const auto& field = normalized["fields"][i];
        const auto& physical = entity.schema->field(i);
        assert(Get(field, "field_id") == physical->name() && Get(field, "name") == physical->name());
        assert(Get(field["type"], "system") == "arrow");
        assert(Get(field["type"], "name") == physical->type()->ToString());
        assert(field["nullable"].GetBool() == physical->nullable());
        assert(field["definition"]["state"] == "declared" && !Get(field["definition"], "value").empty());
        assert(field["definition"]["origin"] == "producer" && field["role"]["state"] == "declared");
    }
    assert(Codes(Field(dictionary, "outcome")) == (std::set<std::string>{"matched", "query_only", "response_only",
                                                                         "truncated_response", "unsupported_message"}));
    assert(Codes(Field(dictionary, "query_direction")) == (std::set<std::string>{"0", "1"}));
    assert(Codes(Field(dictionary, "response_direction")) == (std::set<std::string>{"0", "1"}));
    assert(Codes(Field(dictionary, "revision")) == (std::set<std::string>{"1"}));
    assert(Codes(Field(dictionary, "is_final")) == (std::set<std::string>{"true"}));
    for (const char* name : {"observed_at", "query_at_ns", "response_at_ns", "latency_ns"}) {
        const auto& field = Field(dictionary, name);
        assert(field["unit"]["value"]["code"] == "ns");
        assert(field["time_semantics"]["value"]["kind"] ==
               (std::string(name) == "latency_ns" ? "duration" : "instant"));
    }
    const auto& latency = Field(dictionary, "latency_ns");
    assert(latency["validity"]["value"]["mode"] == "descriptive");
    assert(Get(latency["validity"]["value"], "description").find("truncated_response") != std::string::npos);
    assert(latency["validity"]["value"]["field_ids"].Size() == 3);
    assert(Get(latency["null_meaning"], "value").find("零") != std::string::npos);
    assert(Get(Field(dictionary, "response_rcode")["definition"], "value").find("EDNS") != std::string::npos);
    assert(Get(Field(dictionary, "response_tc")["definition"], "value").find("采集截断") != std::string::npos);
    assert(Get(Field(dictionary, "query_retries")["definition"], "value").find("不含首次") != std::string::npos);
    assert(Get(Field(dictionary, "a_ip")["definition"], "value").find("客户端") != std::string::npos);
    assert(Codes(Field(dictionary, "incomplete_reason")) ==
           (std::set<std::string>{"response_not_observed_by_deadline", "response_path_gap", "capture_truncation",
                                  "malformed_response", "session_end", "no_pending_query", "dns_tc_set",
                                  "unsupported_opcode_or_transfer"}));
    // Field references in descriptive rules are checked by the real dictionary validator.
    Json invalid;
    invalid.CopyFrom(dictionary, invalid.GetAllocator());
    for (auto& field : invalid["fields"].GetArray())
        if (Get(field, "field_id") == "latency_ns")
            field["validity"]["value"]["field_ids"][0].SetString("absent", invalid.GetAllocator());
    assert(NormalizeSemantics("dictionary", Encode(invalid), &normalized, &references, &error) != 0);
}
void CheckSessionDictionary(const Value& dictionary, const npm::NpmEntityDescriptorV1& entity) {
    Json normalized;
    std::vector<SemanticsReference> references;
    DataSemanticsErrorV1 error;
    const int rc = NormalizeSemantics("dictionary", Encode(dictionary), &normalized, &references, &error);
    if (rc) std::cerr << error.field_path << " " << error.message << '\n';
    assert(rc == 0 && references.empty());
    assert(dictionary["entity"]["identity_fields"]["value"][0] == "session_id");
    assert(dictionary["fields"].Size() == entity.schema->num_fields());
    for (const auto& field : normalized["fields"].GetArray()) {
        assert(field["definition"]["state"] == "declared");
        assert(field["role"]["state"] == "declared");
        assert(!Get(field["definition"], "value").empty());
    }
    assert(Codes(Field(dictionary, "protocol_status")) == (std::set<std::string>{"pending", "identified", "unknown"}));
    if (entity.module_id == "basic") {
        assert(Field(dictionary, "interval_packets_ab")["measure_kind"]["value"] == "delta");
        assert(Field(dictionary, "packets_ab")["measure_kind"]["value"] == "cumulative");
        assert(Get(Field(dictionary, "period_complete")["definition"], "value").find("水位") != std::string::npos);
        assert(Get(Field(dictionary, "period_start_ns")["null_meaning"], "value").find("final") != std::string::npos);
    } else {
        assert(Codes(Field(dictionary, "tcp_rtt_status")) ==
               (std::set<std::string>{"valid", "no_sample", "ambiguous", "not_applicable"}));
        assert(Get(Field(dictionary, "tcp_rtt_samples")["null_meaning"], "value").find("0") != std::string::npos);
        assert(Field(dictionary, "wire_bps_ab")["unit"]["value"]["code"] == "bit/s");
        assert(Get(Field(dictionary, "measurement_flags")["definition"], "value").find("16") != std::string::npos);
    }
}
void CheckProtocolDictionary(const Value& dictionary, const npm::NpmEntityDescriptorV1& entity) {
    Json normalized;
    std::vector<SemanticsReference> references;
    DataSemanticsErrorV1 error;
    const int rc = NormalizeSemantics("dictionary", Encode(dictionary), &normalized, &references, &error);
    if (rc) std::cerr << error.field_path << " " << error.message << '\n';
    assert(rc == 0 && references.empty());
    assert(normalized["fields"].Size() == entity.schema->num_fields());
    assert(dictionary["entity"]["row_semantics"]["value"] == "event");
    for (const auto& field : normalized["fields"].GetArray()) {
        assert(field["definition"]["state"] == "declared" && field["role"]["state"] == "declared");
        assert(!Get(field["definition"], "value").empty());
    }
    if (entity.module_id == "http1") {
        assert(Codes(Field(dictionary, "outcome")) ==
               (std::set<std::string>{"matched", "request_only", "response_only"}));
        assert(Get(Field(dictionary, "outcome")["definition"], "value").find("不代表") != std::string::npos);
        assert(Get(Field(dictionary, "latency_ns")["definition"], "value").find("正文") != std::string::npos);
    } else if (entity.module_id == "tls") {
        assert(Codes(Field(dictionary, "outcome")) ==
               (std::set<std::string>{"server_hello_observed", "fatal_alert_observed", "incomplete"}));
        assert(Get(Field(dictionary, "selected_alpn")["definition"], "value").find("JSON") != std::string::npos);
        assert(Get(Field(dictionary, "server_hello_latency_ns")["definition"], "value").find("不是完整握手") !=
               std::string::npos);
    } else {
        assert(!entity.schema->GetFieldByName("session_id"));
        assert(Codes(Field(dictionary, "quote_status")) ==
               (std::set<std::string>{"unavailable", "invalid", "ip_only", "tuple"}));
        assert(Get(Field(dictionary, "active_quoted_session_id")["definition"], "value").find("不创建") !=
               std::string::npos);
    }
}
}  // namespace

int main() {
    CheckConfigAdapter();
    const char* text = flowsql_operator_data_contract_v1(0);
    assert(text && std::string(text) == flowsql_operator_data_contract_v1(1));
    assert(!flowsql_operator_data_contract_v1(-1) && !flowsql_operator_data_contract_v1(2));
    Json doc;
    std::vector<SemanticsReference> references;
    DataSemanticsErrorV1 error;
    const int rc = NormalizeSemantics("operator_contract", text, &doc, &references, &error);
    if (rc) std::cerr << error.field_path << " " << error.message << '\n';
    assert(rc == 0);
    assert(Get(doc["operator"], "category") == "npm" && Get(doc["operator"], "name") == "basic");
    assert(doc["configuration"]["invocation_schema"]["properties"].MemberCount() == 17);
    assert(doc["outputs"].Size() == 2 && doc["outputs"][0]["variants"].Size() == 13);
    assert(doc["resources"]["managed_results"]["entities"].Size() == 12);
    // Static definitions are present at component load, before any task or database exists.
    const auto& defaults = doc["resources"]["managed_dictionaries"];
    assert(defaults.MemberCount() == 50);
    for (const auto& item : defaults.GetObject()) {
        Json dictionary;
        std::vector<SemanticsReference> links;
        assert(item.value.HasMember("schema_canonical"));
        assert(NormalizeSemantics("dictionary", Encode(item.value["dictionary"]), &dictionary, &links, &error) == 0);
        for (const auto& field : dictionary["fields"].GetArray()) {
            assert(field["definition"]["state"] == "declared");
            const auto& time = field["time_semantics"];
            if (Get(time, "state") != "declared") continue;
            assert(Get(time["value"]["unit"]["value"], "code") == "ns");
            if (Get(time["value"], "kind") == "instant") {
                assert(Get(time["value"]["epoch"], "value") == "1970-01-01T00:00:00Z");
                assert(Get(time["value"]["timezone"], "value") == "UTC");
            } else {
                assert(Get(time["value"]["epoch"], "state") == "not_applicable");
                assert(Get(time["value"]["timezone"], "state") == "not_applicable");
            }
        }
        assert(links.empty());
    }
    std::set<std::string> inventory;
    for (const auto& item : doc["resources"]["managed_results"]["entities"].GetArray()) {
        const auto id = Get(item, "entity");
        assert(inventory.insert(id).second && item.HasMember("schema_version"));
        assert(item["schema_version"].GetUint() == 1);
        assert(Get(item, "dictionary_state") == "ready");
        assert(!item.HasMember("dictionary_ref") && !item.HasMember("binding_ref"));
        assert(item.HasMember("dictionary_pointer"));
        const auto& structure = doc["resources"][id.c_str()];
        assert(structure.HasMember("dictionary"));
        assert(rapidjson::Pointer(Get(item, "dictionary_pointer").c_str()).Get(doc) == &structure["dictionary"]);
    }
    for (const auto kind :
         {npm::NpmFamilyEntityKind::kBasic, npm::NpmFamilyEntityKind::kSession, npm::NpmFamilyEntityKind::kDns,
          npm::NpmFamilyEntityKind::kHttp1, npm::NpmFamilyEntityKind::kTls, npm::NpmFamilyEntityKind::kIcmp}) {
        for (const auto family : {packet::AddressFamily::kIPv4, packet::AddressFamily::kIPv6}) {
            const auto entity = npm::NpmFamilyEntityDescriptor(kind, family);
            const auto& structure = doc["resources"][entity.entity_id.c_str()];
            const auto& fields = structure["fields"];
            assert(fields.Size() == entity.schema->num_fields());
            for (rapidjson::SizeType i = 0; i < fields.Size(); ++i) {
                assert(Get(fields[i], "name") == entity.schema->field(i)->name());
                assert(Get(fields[i]["type"], "name") == entity.schema->field(i)->type()->ToString());
                assert(fields[i]["nullable"].GetBool() == entity.schema->field(i)->nullable());
            }
            assert(Get(structure, "identity_column") == entity.identity_column);
            assert(Get(structure["metadata"], "flowsql.timestamp_unit") == "ns");
            if (kind == npm::NpmFamilyEntityKind::kDns) CheckDnsDictionary(structure["dictionary"], entity);
            if (kind == npm::NpmFamilyEntityKind::kBasic || kind == npm::NpmFamilyEntityKind::kSession)
                CheckSessionDictionary(structure["dictionary"], entity);
            if (kind == npm::NpmFamilyEntityKind::kHttp1 || kind == npm::NpmFamilyEntityKind::kTls ||
                kind == npm::NpmFamilyEntityKind::kIcmp)
                CheckProtocolDictionary(structure["dictionary"], entity);
        }
    }
    auto check = [&](const char* parameters, bool accepted) {
        Json supplied;
        supplied.Parse(parameters);
        assert(!supplied.HasParseError());
        npm::NpmBasicTaskConfig production;
        const auto parsed = npm::ParseNpmBasicTaskConfig(parameters, &production);
        assert((parsed.error == npm::NpmBasicTaskConfigError::kNone) == accepted);
        assert((ValidateOperatorParameters(doc, supplied, &error) == 0) == accepted);
    };
    check(R"({"observing":"basic_ipv4"})", true);
    check(R"({"observing":"basic_ipv4","source_domains":"0:7;1:7"})", true);
    check(
        R"({"features":"basic,session","observing":"session_ipv6","parameters":"{\"schema_version\":1,\"core\":{\"result_mode\":\"final\"}}"})",
        true);
    for (const char* key :
         {"run_mode", "result_mode", "overload_policy", "output_interval_ns", "payload_sample_packets",
          "tcp_idle_timeout_ns", "udp_idle_timeout_ns", "out_of_order_tolerance_ns", "max_active_sessions",
          "max_tracked_bytes", "max_pending_output_bytes", "session_max_tcp_ranges_per_direction"}) {
        Json supplied;
        supplied.Parse(R"({"observing":"basic_ipv4","parameters":"{\"schema_version\":1}"})");
        Set(supplied, key, "1", supplied.GetAllocator());
        check(Encode(supplied).c_str(), false);
    }
    check(R"({"observing":"basic_ipv4","parameters":"{\"schema_version\":2}"})", false);
    check(R"({"observing":"basic_ipv4","parameters":"\"{}\""})", false);
    check(R"({"observing":"basic_ipv4","unknown":"1"})", false);
    check(R"({"observing":"basic_ipv4","result_mode":null})", false);
    check(R"({"observing":"basic_ipv4","parameters":"{\"schema_version\":1,\"core\":{\"max_active_sessions\":0}}"})",
          false);
    // Unconsumed module objects are allowed by production; do not invent a stricter global schema.
    check(R"({"observing":"basic_ipv4","parameters":"{\"schema_version\":1,\"unused_module\":{\"future\":42}}"})",
          true);
    Json subject, basis, contract, notes;
    Json context(rapidjson::kObjectType);
    subject.Parse(R"({"kind":"operator","name":"npm.basic"})");
    basis.Parse(R"({"contract_ref":"operator_contract.npm.basic@1"})");
    contract.Parse(R"({"state":"declared"})");
    Set(contract, "document", Copy(doc, contract.GetAllocator()), contract.GetAllocator());
    auto usage = operator_usage::Build(subject, basis, contract, notes, context);
    assert(usage["usage"]["identity"]["purposes"][0]["text"] == doc["description"]);
    assert(usage["usage"]["parameters"]["description"] == doc["configuration"]["description"]);
    assert(usage["usage"]["inputs"][0]["description"] == doc["inputs"][0]["description"]);
    assert(usage["usage"]["outputs"][0]["description"] == doc["outputs"][0]["description"]);
    assert(Get(usage["usage"]["outputs"][0], "fields_state") == "dynamic");
    for (const auto& gap : usage["gaps"].GetArray()) assert(Get(gap, "code") != "OUTPUT_FIELDS_UNDECLARED");
    for (const auto& link : usage["details"].GetArray())
        assert(rapidjson::Pointer(Get(link, "resource_pointer").c_str()).Get(doc));
    Json invalid;
    invalid.CopyFrom(doc, invalid.GetAllocator());
    Set(invalid["inputs"][0], "structure_ref", "#/resources/missing", invalid.GetAllocator());
    assert(NormalizeSemantics("operator_contract", Encode(invalid), &notes, &references, &error) != 0);
    invalid.CopyFrom(doc, invalid.GetAllocator());
    Set(invalid["configuration"], "description", Value(1), invalid.GetAllocator());
    assert(NormalizeSemantics("operator_contract", Encode(invalid), &notes, &references, &error) != 0);
    std::cout << "PASS: real NPM contract, 12 production output schemas and explicit domain inventory, "
                 "twelve complete domain dictionaries, invocation boundaries and exact usage projection\n";
}
