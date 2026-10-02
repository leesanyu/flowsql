// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <operators/npm_basic/modules/icmp/npm_icmp_contract.h>

#include <arrow/api.h>

#include <cassert>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

namespace npm = flowsql::npm;

namespace {

class EmptyLookup final : public npm::INpmIcmpActiveSessionLookupV1 {
 public:
    std::optional<uint64_t> FindActive(const npm::NpmIcmpQuotedFlowV1&) const override { return std::nullopt; }
};

void TestConfig() {
    npm::NpmIcmpConfigV1 config;
    using Error = npm::NpmIcmpConfigErrorV1;
    const auto reject = [&](std::string_view json, Error error, std::string_view path) {
        const auto status = npm::ParseNpmIcmpConfigV1(json, &config);
        assert(status.error == error && status.path == path);
        assert(config.echo_timeout_ns == npm::kNpmIcmpDefaultEchoTimeoutNsV1);
        assert(config.max_pending_echo == npm::kNpmIcmpDefaultMaxPendingEchoV1);
    };
    reject("", Error::kInvalidType, "/icmp");
    reject("null", Error::kInvalidType, "/icmp");
    reject(R"({"unknown":1})", Error::kUnknownField, "/icmp/unknown");
    reject(R"({"echo_timeout_ns":1,"echo_timeout_ns":2})", Error::kDuplicateField, "/icmp/echo_timeout_ns");
    reject(R"({"echo_timeout_ns":null})", Error::kInvalidType, "/icmp/echo_timeout_ns");
    reject(R"({"echo_timeout_ns":999999})", Error::kInvalidRange, "/icmp/echo_timeout_ns");
    reject(R"({"echo_timeout_ns":300000000001})", Error::kInvalidRange, "/icmp/echo_timeout_ns");
    reject(R"({"max_pending_echo":"1"})", Error::kInvalidType, "/icmp/max_pending_echo");
    reject(R"({"max_pending_echo":0})", Error::kInvalidRange, "/icmp/max_pending_echo");
    reject(R"({"max_pending_echo":4097})", Error::kInvalidRange, "/icmp/max_pending_echo");
    reject(R"({"max_pending_echo":1,"max_pending_echo":2})", Error::kDuplicateField, "/icmp/max_pending_echo");
    assert(npm::ParseNpmIcmpConfigV1("{}", nullptr).error == Error::kInvalidType);
    assert(npm::ParseNpmIcmpConfigV1("{}", &config).error == Error::kNone);
    assert(config.echo_timeout_ns == 5'000'000'000 && config.max_pending_echo == 4096);
    assert(npm::ParseNpmIcmpConfigV1(R"({"echo_timeout_ns":1000000,"max_pending_echo":1})", &config).error ==
           Error::kNone);
    assert(config.echo_timeout_ns == 1'000'000 && config.max_pending_echo == 1);
    assert(npm::ParseNpmIcmpConfigV1(R"({"echo_timeout_ns":300000000000,"max_pending_echo":4096})", &config).error ==
           Error::kNone);
}

void TestPlanAndSchema() {
    static_assert(std::is_same_v<decltype(npm::NpmIcmpEventV1{}.latency_ns), std::optional<int64_t>>);
    static_assert(std::is_same_v<decltype(npm::NpmIcmpQuotedFlowV1{}.src_ip), flowsql::packet::IpAddress>);
    EmptyLookup lookup;
    assert(!lookup.FindActive({}));

    const auto plan = npm::NpmIcmpModulePlanV1();
    assert(plan.module_id == "icmp");
    assert(plan.input_mask == static_cast<uint8_t>(npm::NpmInputKindV1::kControlPacket));
    assert(!plan.requires_labeling && !plan.requires_tcp_stream && !plan.primary_label_ids);
    assert(npm::ValidateNpmModulePlanV1(plan, {}).error == npm::NpmProtocolContractErrorV1::kNone);
    const auto& entity = plan.entities.at(0);
    assert(entity.entity_id == "icmp_event" && entity.module_id == "icmp");
    assert(entity.schema_version == 1 && entity.revision_semantics == npm::NpmRevisionSemanticsV1::kEvent);
    assert(npm::ValidateNpmEntityDescriptorV1(entity).error == npm::NpmProtocolContractErrorV1::kNone);
    for (const auto& [key, value] : std::vector<std::pair<const char*, const char*>>{
             {"flowsql.entity", "icmp_event"},
             {"flowsql.schema_version", "1"},
             {"flowsql.timestamp_unit", "ns"},
             {"flowsql.revision_semantics", "event"},
             {"flowsql.measurement_scope", "single_capture_observed_packets"}}) {
        assert(entity.schema->metadata()->Get(key).ValueOrDie() == value);
    }
    const std::vector<std::tuple<const char*, arrow::Type::type, bool>> fields = {
        {"entity_instance_id", arrow::Type::UINT64, false},
        {"revision", arrow::Type::UINT64, false},
        {"observed_at", arrow::Type::INT64, false},
        {"is_final", arrow::Type::BOOL, false},
        {"observation_domain_id", arrow::Type::UINT64, false},
        {"ip_family", arrow::Type::UINT8, false},
        {"src_ip", arrow::Type::STRING, false},
        {"dst_ip", arrow::Type::STRING, false},
        {"outcome", arrow::Type::STRING, false},
        {"icmp_type", arrow::Type::UINT8, false},
        {"icmp_code", arrow::Type::UINT8, false},
        {"outer_truncated", arrow::Type::BOOL, false},
        {"incomplete_reason", arrow::Type::STRING, true},
        {"echo_id", arrow::Type::UINT16, true},
        {"echo_sequence", arrow::Type::UINT16, true},
        {"echo_retries", arrow::Type::UINT32, true},
        {"request_at_ns", arrow::Type::INT64, true},
        {"reply_at_ns", arrow::Type::INT64, true},
        {"latency_ns", arrow::Type::INT64, true},
        {"quote_status", arrow::Type::STRING, true},
        {"quoted_ip_family", arrow::Type::UINT8, true},
        {"quoted_protocol", arrow::Type::UINT8, true},
        {"quoted_src_ip", arrow::Type::STRING, true},
        {"quoted_dst_ip", arrow::Type::STRING, true},
        {"quoted_src_port", arrow::Type::UINT16, true},
        {"quoted_dst_port", arrow::Type::UINT16, true},
        {"active_quoted_session_id", arrow::Type::UINT64, true},
        {"next_hop_mtu", arrow::Type::UINT32, true},
    };
    assert(entity.schema->num_fields() == 28);
    for (size_t i = 0; i < fields.size(); ++i) {
        const auto& [name, type, nullable] = fields[i];
        const auto& field = entity.schema->field(static_cast<int>(i));
        assert(field->name() == name && field->type()->id() == type && field->nullable() == nullable);
    }
}

}  // namespace

int main() {
    TestConfig();
    TestPlanAndSchema();
}
