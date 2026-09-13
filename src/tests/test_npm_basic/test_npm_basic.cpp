// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <common/network/netbase.h>
#include <plugins/npm_basic/npm_analysis_contract.h>
#include <plugins/npm_basic/npm_session_key.h>
#include <plugins/npm_basic/npm_session_table.h>

#include <arrow/api.h>

#include <arpa/inet.h>

#include <array>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

namespace npm = flowsql::npm;

namespace {

constexpr uint8_t kTcpFin = 0x01;
constexpr uint8_t kTcpSyn = 0x02;
constexpr uint8_t kTcpRst = 0x04;
constexpr uint8_t kTcpAck = 0x10;

struct PacketFixture {
    std::vector<uint8_t> bytes;
    flowsql::packet::PacketLayerInfo layer;

    flowsql::packet::PacketView View(uint32_t source_id, uint32_t wire_len = 0) const {
        flowsql::packet::PacketView view;
        view.meta.captured_len = static_cast<uint32_t>(bytes.size());
        view.meta.wire_len = wire_len == 0 ? view.meta.captured_len : wire_len;
        view.meta.source_id = source_id;
        view.bytes = flowsql::Span<const uint8_t>(bytes.data(), bytes.size());
        return view;
    }
};

flowsql::packet::IPv4Address ParseIpv4(const char* text) {
    flowsql::packet::IPv4Address address;
    assert(inet_pton(AF_INET, text, address.bytes) == 1);
    return address;
}

flowsql::packet::IPv6Address ParseIpv6(const char* text) {
    flowsql::packet::IPv6Address address;
    assert(inet_pton(AF_INET6, text, address.bytes) == 1);
    return address;
}

PacketFixture MakeIpv4TcpPacket(const char* src,
                                uint16_t src_port,
                                const char* dst,
                                uint16_t dst_port,
                                const std::vector<uint8_t>& payload,
                                uint8_t flags = 0,
                                uint32_t sequence = 0) {
    PacketFixture fixture;
    fixture.bytes.resize(sizeof(flowsql::Ipv4Header) + sizeof(flowsql::TcpHeader) + payload.size());
    auto* ip = reinterpret_cast<flowsql::Ipv4Header*>(fixture.bytes.data());
    ip->version = 4;
    ip->ihl = 5;
    ip->total_length = htons(static_cast<uint16_t>(fixture.bytes.size()));
    ip->protocol = flowsql::ipv4::eNext::TCP;
    ip->src_addr = ParseIpv4(src);
    ip->dst_addr = ParseIpv4(dst);
    auto* tcp = reinterpret_cast<flowsql::TcpHeader*>(fixture.bytes.data() + sizeof(flowsql::Ipv4Header));
    tcp->src_port = htons(src_port);
    tcp->dst_port = htons(dst_port);
    tcp->seq = htonl(sequence);
    tcp->offset = 5;
    tcp->flags.flags_byte = flags;
    std::memcpy(fixture.bytes.data() + sizeof(flowsql::Ipv4Header) + sizeof(flowsql::TcpHeader),
                payload.data(),
                payload.size());

    fixture.layer.status = flowsql::packet::LayerStatus::kDecoded;
    fixture.layer.layer_count = 2;
    fixture.layer.layers[0] = {static_cast<uint16_t>(flowsql::eLayer::IPv4), 0};
    fixture.layer.layers[1] = {static_cast<uint16_t>(flowsql::eLayer::TCP), sizeof(flowsql::Ipv4Header)};
    fixture.layer.network_layer_index = 0;
    fixture.layer.transport_layer_index = 1;
    fixture.layer.payload_offset = sizeof(flowsql::Ipv4Header) + sizeof(flowsql::TcpHeader);
    fixture.layer.src_ip = ip->src_addr;
    fixture.layer.dst_ip = ip->dst_addr;
    fixture.layer.transport_protocol = flowsql::ipv4::eNext::TCP;
    fixture.layer.src_port = src_port;
    fixture.layer.dst_port = dst_port;
    fixture.layer.ports_valid = 1;
    return fixture;
}

PacketFixture MakeIpv6UdpPacket(const char* src,
                                uint16_t src_port,
                                const char* dst,
                                uint16_t dst_port,
                                const std::vector<uint8_t>& payload,
                                bool fragmented = false,
                                uint16_t fragment_offset = 0) {
    const size_t fragment_header_size = fragmented ? 8 : 0;
    PacketFixture fixture;
    fixture.bytes.resize(sizeof(flowsql::Ipv6Header) + fragment_header_size + sizeof(flowsql::UdpHeader) +
                         payload.size());
    auto* ip = reinterpret_cast<flowsql::Ipv6Header*>(fixture.bytes.data());
    ip->version = 6;
    ip->payload = htons(static_cast<uint16_t>(fixture.bytes.size() - sizeof(flowsql::Ipv6Header)));
    ip->protocol = fragmented ? flowsql::ipv6::eNext::IPv6_EXT_FRAGMENT : flowsql::ipv6::eNext::UDP;
    ip->src_addr = ParseIpv6(src);
    ip->dst_addr = ParseIpv6(dst);

    size_t udp_offset = sizeof(flowsql::Ipv6Header);
    if (fragmented) {
        fixture.bytes[udp_offset] = flowsql::ipv6::eNext::UDP;
        const uint16_t fragment_field = htons(static_cast<uint16_t>(fragment_offset << 3));
        std::memcpy(fixture.bytes.data() + udp_offset + 2, &fragment_field, sizeof(fragment_field));
        udp_offset += fragment_header_size;
    }
    auto* udp = reinterpret_cast<flowsql::UdpHeader*>(fixture.bytes.data() + udp_offset);
    udp->src_port = htons(src_port);
    udp->dst_port = htons(dst_port);
    udp->length = htons(static_cast<uint16_t>(sizeof(flowsql::UdpHeader) + payload.size()));
    std::memcpy(fixture.bytes.data() + udp_offset + sizeof(flowsql::UdpHeader), payload.data(), payload.size());

    fixture.layer.status = flowsql::packet::LayerStatus::kDecoded;
    fixture.layer.layer_count = fragmented ? 3 : 2;
    fixture.layer.layers[0] = {static_cast<uint16_t>(flowsql::eLayer::IPv6), 0};
    if (fragmented) {
        fixture.layer.layers[1] = {static_cast<uint16_t>(flowsql::eLayer::IPv6_EXT_FRAGMENT),
                                   sizeof(flowsql::Ipv6Header)};
    }
    const uint8_t transport_index = fragmented ? 2 : 1;
    fixture.layer.layers[transport_index] = {static_cast<uint16_t>(flowsql::eLayer::UDP),
                                             static_cast<uint32_t>(udp_offset)};
    fixture.layer.network_layer_index = 0;
    fixture.layer.transport_layer_index = transport_index;
    fixture.layer.payload_offset = static_cast<uint32_t>(udp_offset + sizeof(flowsql::UdpHeader));
    fixture.layer.src_ip = ip->src_addr;
    fixture.layer.dst_ip = ip->dst_addr;
    fixture.layer.transport_protocol = flowsql::ipv6::eNext::UDP;
    fixture.layer.src_port = src_port;
    fixture.layer.dst_port = dst_port;
    fixture.layer.ports_valid = 1;
    return fixture;
}

bool SameIp(const flowsql::packet::IpAddress& left, const flowsql::packet::IpAddress& right) {
    if (left.index() != right.index()) return false;
    if (const auto* ipv4 = std::get_if<flowsql::packet::IPv4Address>(&left)) {
        return std::memcmp(ipv4->bytes, std::get<flowsql::packet::IPv4Address>(right).bytes, 4) == 0;
    }
    if (const auto* ipv6 = std::get_if<flowsql::packet::IPv6Address>(&left)) {
        return std::memcmp(ipv6->bytes, std::get<flowsql::packet::IPv6Address>(right).bytes, 16) == 0;
    }
    return true;
}

bool SameKey(const npm::NpmSessionKey& left, const npm::NpmSessionKey& right) {
    return left.input_namespace == right.input_namespace &&
           left.observation_domain_id == right.observation_domain_id && left.ip_family == right.ip_family &&
           left.transport_protocol == right.transport_protocol && SameIp(left.a.ip, right.a.ip) &&
           left.a.port == right.a.port && SameIp(left.b.ip, right.b.ip) && left.b.port == right.b.port;
}

void TestConfigDefaultsAndEnumContract() {
    static_assert(std::is_same_v<std::underlying_type_t<npm::NpmRunMode>, uint8_t>);
    static_assert(std::is_same_v<std::underlying_type_t<npm::NpmResultMode>, uint8_t>);
    static_assert(std::is_same_v<std::underlying_type_t<npm::NpmOverloadPolicy>, uint8_t>);
    static_assert(npm::kNpmMinOutputIntervalNs == 10'000'000LL);
    static_assert(npm::kNpmDefaultOutputIntervalNs == 1'000'000'000LL);
    static_assert(npm::kNpmMaxOutputIntervalNs == 3'600'000'000'000LL);
    static_assert(npm::kNpmMinPayloadSamplePackets == 1);
    static_assert(npm::kNpmDefaultPayloadSamplePackets == 8);
    static_assert(npm::kNpmMaxPayloadSamplePackets == 64);
    static_assert(npm::kNpmMinIdleTimeoutNs == 1'000'000'000LL);
    static_assert(npm::kNpmDefaultTcpIdleTimeoutNs == 60'000'000'000LL);
    static_assert(npm::kNpmDefaultUdpIdleTimeoutNs == 30'000'000'000LL);
    static_assert(npm::kNpmMaxIdleTimeoutNs == 86'400'000'000'000LL);
    static_assert(npm::kNpmMinOutOfOrderToleranceNs == 0);
    static_assert(npm::kNpmDefaultOutOfOrderToleranceNs == 1'000'000'000LL);
    static_assert(npm::kNpmMaxOutOfOrderToleranceNs == 60'000'000'000LL);
    static_assert(npm::kNpmMinActiveSessions == 1);
    static_assert(npm::kNpmDefaultActiveSessions == 100'000);
    static_assert(npm::kNpmMaxActiveSessions == 10'000'000);
    static_assert(npm::kNpmMinTrackedBytes == 1ULL * 1024ULL * 1024ULL);
    static_assert(npm::kNpmDefaultTrackedBytes == 256ULL * 1024ULL * 1024ULL);
    static_assert(npm::kNpmMaxTrackedBytes == 1024ULL * 1024ULL * 1024ULL * 1024ULL);
    static_assert(npm::kNpmMinPendingOutputBytes == 1ULL * 1024ULL * 1024ULL);
    static_assert(npm::kNpmDefaultPendingOutputBytes == 64ULL * 1024ULL * 1024ULL);
    static_assert(npm::kNpmMaxPendingOutputBytes == 1024ULL * 1024ULL * 1024ULL * 1024ULL);

    const auto offline = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline);
    assert(offline.run_mode == npm::NpmRunMode::kOffline);
    assert(offline.result_mode == npm::NpmResultMode::kFinal);
    assert(offline.output_interval_ns == npm::kNpmDefaultOutputIntervalNs);
    assert(offline.payload_sample_packets == npm::kNpmDefaultPayloadSamplePackets);
    assert(offline.tcp_idle_timeout_ns == npm::kNpmDefaultTcpIdleTimeoutNs);
    assert(offline.udp_idle_timeout_ns == npm::kNpmDefaultUdpIdleTimeoutNs);
    assert(offline.out_of_order_tolerance_ns == npm::kNpmDefaultOutOfOrderToleranceNs);
    assert(offline.max_active_sessions == npm::kNpmDefaultActiveSessions);
    assert(offline.max_tracked_bytes == npm::kNpmDefaultTrackedBytes);
    assert(offline.max_pending_output_bytes == npm::kNpmDefaultPendingOutputBytes);
    assert(offline.overload_policy == npm::NpmOverloadPolicy::kFail);
    assert(npm::ValidateNpmAnalysisConfig(offline) == npm::NpmAnalysisConfigError::kNone);

    const auto realtime = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kRealtime);
    assert(realtime.run_mode == npm::NpmRunMode::kRealtime);
    assert(realtime.result_mode == npm::NpmResultMode::kPeriodicSnapshot);
    assert(npm::ValidateNpmAnalysisConfig(realtime) == npm::NpmAnalysisConfigError::kNone);
}

void TestConfigRangesAndUnsupportedValues() {
    auto config = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline);

    config.run_mode = static_cast<npm::NpmRunMode>(2);
    assert(npm::ValidateNpmAnalysisConfig(config) == npm::NpmAnalysisConfigError::kInvalidRunMode);
    config = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline);
    config.result_mode = static_cast<npm::NpmResultMode>(2);
    assert(npm::ValidateNpmAnalysisConfig(config) == npm::NpmAnalysisConfigError::kInvalidResultMode);
    config = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline);
    config.overload_policy = static_cast<npm::NpmOverloadPolicy>(1);
    assert(npm::ValidateNpmAnalysisConfig(config) == npm::NpmAnalysisConfigError::kUnsupportedOverloadPolicy);

    config = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline);
    config.output_interval_ns = npm::kNpmMinOutputIntervalNs - 1;
    assert(npm::ValidateNpmAnalysisConfig(config) == npm::NpmAnalysisConfigError::kOutputIntervalOutOfRange);
    config.output_interval_ns = npm::kNpmMaxOutputIntervalNs + 1;
    assert(npm::ValidateNpmAnalysisConfig(config) == npm::NpmAnalysisConfigError::kOutputIntervalOutOfRange);
    config.output_interval_ns = npm::kNpmMinOutputIntervalNs;
    assert(npm::ValidateNpmAnalysisConfig(config) == npm::NpmAnalysisConfigError::kNone);
    config.output_interval_ns = npm::kNpmMaxOutputIntervalNs;
    assert(npm::ValidateNpmAnalysisConfig(config) == npm::NpmAnalysisConfigError::kNone);

    config = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline);
    config.payload_sample_packets = npm::kNpmMinPayloadSamplePackets - 1;
    assert(npm::ValidateNpmAnalysisConfig(config) == npm::NpmAnalysisConfigError::kPayloadSamplePacketsOutOfRange);
    config.payload_sample_packets = npm::kNpmMaxPayloadSamplePackets + 1;
    assert(npm::ValidateNpmAnalysisConfig(config) == npm::NpmAnalysisConfigError::kPayloadSamplePacketsOutOfRange);
    config.payload_sample_packets = npm::kNpmMinPayloadSamplePackets;
    assert(npm::ValidateNpmAnalysisConfig(config) == npm::NpmAnalysisConfigError::kNone);
    config.payload_sample_packets = npm::kNpmMaxPayloadSamplePackets;
    assert(npm::ValidateNpmAnalysisConfig(config) == npm::NpmAnalysisConfigError::kNone);

    config = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline);
    config.tcp_idle_timeout_ns = npm::kNpmMinIdleTimeoutNs - 1;
    assert(npm::ValidateNpmAnalysisConfig(config) == npm::NpmAnalysisConfigError::kTcpIdleTimeoutOutOfRange);
    config.tcp_idle_timeout_ns = npm::kNpmMaxIdleTimeoutNs + 1;
    assert(npm::ValidateNpmAnalysisConfig(config) == npm::NpmAnalysisConfigError::kTcpIdleTimeoutOutOfRange);
    config.tcp_idle_timeout_ns = npm::kNpmMinIdleTimeoutNs;
    assert(npm::ValidateNpmAnalysisConfig(config) == npm::NpmAnalysisConfigError::kNone);
    config.tcp_idle_timeout_ns = npm::kNpmMaxIdleTimeoutNs;
    assert(npm::ValidateNpmAnalysisConfig(config) == npm::NpmAnalysisConfigError::kNone);
    config = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline);
    config.udp_idle_timeout_ns = npm::kNpmMinIdleTimeoutNs - 1;
    assert(npm::ValidateNpmAnalysisConfig(config) == npm::NpmAnalysisConfigError::kUdpIdleTimeoutOutOfRange);
    config.udp_idle_timeout_ns = npm::kNpmMaxIdleTimeoutNs + 1;
    assert(npm::ValidateNpmAnalysisConfig(config) == npm::NpmAnalysisConfigError::kUdpIdleTimeoutOutOfRange);
    config.udp_idle_timeout_ns = npm::kNpmMinIdleTimeoutNs;
    assert(npm::ValidateNpmAnalysisConfig(config) == npm::NpmAnalysisConfigError::kNone);
    config.udp_idle_timeout_ns = npm::kNpmMaxIdleTimeoutNs;
    assert(npm::ValidateNpmAnalysisConfig(config) == npm::NpmAnalysisConfigError::kNone);

    config = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline);
    config.out_of_order_tolerance_ns = npm::kNpmMinOutOfOrderToleranceNs - 1;
    assert(npm::ValidateNpmAnalysisConfig(config) == npm::NpmAnalysisConfigError::kOutOfOrderToleranceOutOfRange);
    config.out_of_order_tolerance_ns = npm::kNpmMaxOutOfOrderToleranceNs + 1;
    assert(npm::ValidateNpmAnalysisConfig(config) == npm::NpmAnalysisConfigError::kOutOfOrderToleranceOutOfRange);
    config.out_of_order_tolerance_ns = npm::kNpmMinOutOfOrderToleranceNs;
    assert(npm::ValidateNpmAnalysisConfig(config) == npm::NpmAnalysisConfigError::kNone);
    config.out_of_order_tolerance_ns = npm::kNpmMaxOutOfOrderToleranceNs;
    assert(npm::ValidateNpmAnalysisConfig(config) == npm::NpmAnalysisConfigError::kNone);

    config = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline);
    config.max_active_sessions = npm::kNpmMinActiveSessions - 1;
    assert(npm::ValidateNpmAnalysisConfig(config) == npm::NpmAnalysisConfigError::kActiveSessionsOutOfRange);
    config.max_active_sessions = npm::kNpmMaxActiveSessions + 1;
    assert(npm::ValidateNpmAnalysisConfig(config) == npm::NpmAnalysisConfigError::kActiveSessionsOutOfRange);
    config.max_active_sessions = npm::kNpmMinActiveSessions;
    assert(npm::ValidateNpmAnalysisConfig(config) == npm::NpmAnalysisConfigError::kNone);
    config.max_active_sessions = npm::kNpmMaxActiveSessions;
    assert(npm::ValidateNpmAnalysisConfig(config) == npm::NpmAnalysisConfigError::kNone);

    config = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline);
    config.max_tracked_bytes = npm::kNpmMinTrackedBytes - 1;
    assert(npm::ValidateNpmAnalysisConfig(config) == npm::NpmAnalysisConfigError::kTrackedBytesOutOfRange);
    config.max_tracked_bytes = npm::kNpmMaxTrackedBytes + 1;
    assert(npm::ValidateNpmAnalysisConfig(config) == npm::NpmAnalysisConfigError::kTrackedBytesOutOfRange);
    config.max_tracked_bytes = npm::kNpmMinTrackedBytes;
    assert(npm::ValidateNpmAnalysisConfig(config) == npm::NpmAnalysisConfigError::kNone);
    config.max_tracked_bytes = npm::kNpmMaxTrackedBytes;
    assert(npm::ValidateNpmAnalysisConfig(config) == npm::NpmAnalysisConfigError::kNone);

    config = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline);
    config.max_pending_output_bytes = npm::kNpmMinPendingOutputBytes - 1;
    assert(npm::ValidateNpmAnalysisConfig(config) == npm::NpmAnalysisConfigError::kPendingOutputBytesOutOfRange);
    config.max_pending_output_bytes = npm::kNpmMaxPendingOutputBytes + 1;
    assert(npm::ValidateNpmAnalysisConfig(config) == npm::NpmAnalysisConfigError::kPendingOutputBytesOutOfRange);
    config.max_pending_output_bytes = npm::kNpmMinPendingOutputBytes;
    assert(npm::ValidateNpmAnalysisConfig(config) == npm::NpmAnalysisConfigError::kNone);
    config.max_pending_output_bytes = npm::kNpmMaxPendingOutputBytes;
    assert(npm::ValidateNpmAnalysisConfig(config) == npm::NpmAnalysisConfigError::kNone);
}

void TestObservationDomainMapping() {
    const npm::NpmObservationDomainMap domain_map{
        "pcapfile.capture", {{4, 7}, {9, 7}, {10, 8}}};
    assert(npm::ValidateNpmObservationDomainMap(domain_map) == npm::NpmObservationDomainError::kNone);

    uint64_t observation_domain_id = 99;
    assert(npm::ResolveNpmObservationDomain(domain_map, 4, &observation_domain_id) ==
           npm::NpmObservationDomainError::kNone);
    assert(observation_domain_id == 7);
    assert(npm::ResolveNpmObservationDomain(domain_map, 9, &observation_domain_id) ==
           npm::NpmObservationDomainError::kNone);
    assert(observation_domain_id == 7);
    assert(npm::ResolveNpmObservationDomain(domain_map, 10, &observation_domain_id) ==
           npm::NpmObservationDomainError::kNone);
    assert(observation_domain_id == 8);

    observation_domain_id = 99;
    assert(npm::ResolveNpmObservationDomain(domain_map, 11, &observation_domain_id) ==
           npm::NpmObservationDomainError::kUnknownSourceId);
    assert(observation_domain_id == 99);
    assert(npm::ResolveNpmObservationDomain(domain_map, 4, nullptr) ==
           npm::NpmObservationDomainError::kNullOutput);

    const npm::NpmObservationDomainMap empty_namespace{"", {{4, 7}}};
    assert(npm::ValidateNpmObservationDomainMap(empty_namespace) ==
           npm::NpmObservationDomainError::kEmptyInputNamespace);
    const npm::NpmObservationDomainMap empty_bindings{"pcapfile.capture", {}};
    assert(npm::ValidateNpmObservationDomainMap(empty_bindings) ==
           npm::NpmObservationDomainError::kEmptyBindings);
    const npm::NpmObservationDomainMap duplicate_source{
        "pcapfile.capture", {{4, 7}, {4, 8}}};
    assert(npm::ValidateNpmObservationDomainMap(duplicate_source) ==
           npm::NpmObservationDomainError::kDuplicateSourceId);
}

void TestBasicResultNullableContract() {
    static_assert(std::is_same_v<std::underlying_type_t<npm::NpmProtocolStatus>, uint8_t>);
    static_assert(std::is_same_v<std::underlying_type_t<npm::NpmSessionEndReason>, uint8_t>);

    npm::NpmBasicResult result;
    assert(npm::ValidateNpmBasicResult(result) == npm::NpmBasicResultError::kNone);

    result.protocol_status = npm::NpmProtocolStatus::kIdentified;
    assert(npm::ValidateNpmBasicResult(result) == npm::NpmBasicResultError::kProtocolFieldsMismatch);
    result.protocol_id = 80;
    assert(npm::ValidateNpmBasicResult(result) == npm::NpmBasicResultError::kProtocolFieldsMismatch);
    result.protocol_id.reset();
    result.protocol = "HTTP";
    assert(npm::ValidateNpmBasicResult(result) == npm::NpmBasicResultError::kProtocolFieldsMismatch);
    result.protocol_id = 80;
    assert(npm::ValidateNpmBasicResult(result) == npm::NpmBasicResultError::kNone);
    result.protocol_sub_id = 1;
    assert(npm::ValidateNpmBasicResult(result) == npm::NpmBasicResultError::kNone);

    result.protocol_status = npm::NpmProtocolStatus::kUnknown;
    assert(npm::ValidateNpmBasicResult(result) == npm::NpmBasicResultError::kProtocolFieldsMismatch);
    result.protocol_id.reset();
    result.protocol_sub_id.reset();
    result.protocol.reset();
    assert(npm::ValidateNpmBasicResult(result) == npm::NpmBasicResultError::kNone);

    result.protocol_status = npm::NpmProtocolStatus::kPending;
    result.protocol_id = 80;
    assert(npm::ValidateNpmBasicResult(result) == npm::NpmBasicResultError::kProtocolFieldsMismatch);
    result.protocol_id.reset();
    result.is_final = true;
    result.end_reason = npm::NpmSessionEndReason::kEof;
    assert(npm::ValidateNpmBasicResult(result) == npm::NpmBasicResultError::kPendingFinalResult);

    result.protocol_status = npm::NpmProtocolStatus::kUnknown;
    result.end_reason.reset();
    assert(npm::ValidateNpmBasicResult(result) == npm::NpmBasicResultError::kMissingFinalEndReason);
    result.end_reason = npm::NpmSessionEndReason::kTupleReuse;
    assert(npm::ValidateNpmBasicResult(result) == npm::NpmBasicResultError::kNone);
    result.is_final = false;
    assert(npm::ValidateNpmBasicResult(result) == npm::NpmBasicResultError::kUnexpectedActiveEndReason);

    result.end_reason = static_cast<npm::NpmSessionEndReason>(4);
    assert(npm::ValidateNpmBasicResult(result) == npm::NpmBasicResultError::kInvalidEndReason);
    result.end_reason.reset();
    result.protocol_status = static_cast<npm::NpmProtocolStatus>(3);
    assert(npm::ValidateNpmBasicResult(result) == npm::NpmBasicResultError::kInvalidProtocolStatus);

    assert(std::strcmp(npm::NpmProtocolStatusName(npm::NpmProtocolStatus::kPending), "pending") == 0);
    assert(std::strcmp(npm::NpmProtocolStatusName(npm::NpmProtocolStatus::kIdentified), "identified") == 0);
    assert(std::strcmp(npm::NpmProtocolStatusName(npm::NpmProtocolStatus::kUnknown), "unknown") == 0);
    assert(npm::NpmProtocolStatusName(static_cast<npm::NpmProtocolStatus>(3)) == nullptr);
    assert(std::strcmp(npm::NpmSessionEndReasonName(npm::NpmSessionEndReason::kClosed), "closed") == 0);
    assert(std::strcmp(npm::NpmSessionEndReasonName(npm::NpmSessionEndReason::kIdleTimeout), "idle_timeout") == 0);
    assert(std::strcmp(npm::NpmSessionEndReasonName(npm::NpmSessionEndReason::kTupleReuse), "tuple_reuse") == 0);
    assert(std::strcmp(npm::NpmSessionEndReasonName(npm::NpmSessionEndReason::kEof), "eof") == 0);
    assert(npm::NpmSessionEndReasonName(static_cast<npm::NpmSessionEndReason>(4)) == nullptr);
}

void TestBasicResultSchema() {
    struct ExpectedField {
        const char* name;
        arrow::Type::type type;
        bool nullable;
    };
    const std::vector<ExpectedField> expected{
        {"session_id", arrow::Type::UINT64, false},
        {"observation_domain_id", arrow::Type::UINT64, false},
        {"revision", arrow::Type::UINT64, false},
        {"observed_at", arrow::Type::INT64, false},
        {"is_final", arrow::Type::BOOL, false},
        {"ip_family", arrow::Type::UINT8, false},
        {"transport_protocol", arrow::Type::UINT8, false},
        {"a_ip", arrow::Type::STRING, false},
        {"b_ip", arrow::Type::STRING, false},
        {"a_port", arrow::Type::UINT16, false},
        {"b_port", arrow::Type::UINT16, false},
        {"first_ns", arrow::Type::INT64, false},
        {"last_ns", arrow::Type::INT64, false},
        {"packets_ab", arrow::Type::UINT64, false},
        {"packets_ba", arrow::Type::UINT64, false},
        {"wire_bytes_ab", arrow::Type::UINT64, false},
        {"wire_bytes_ba", arrow::Type::UINT64, false},
        {"protocol_status", arrow::Type::STRING, false},
        {"protocol_id", arrow::Type::UINT16, true},
        {"protocol_sub_id", arrow::Type::UINT16, true},
        {"protocol", arrow::Type::STRING, true},
        {"end_reason", arrow::Type::STRING, true},
    };

    const auto schema = npm::NpmBasicResultSchema();
    assert(schema != nullptr);
    assert(schema.get() == npm::NpmBasicResultSchema().get());
    assert(schema->num_fields() == static_cast<int>(expected.size()));
    for (std::size_t i = 0; i < expected.size(); ++i) {
        assert(schema->field(static_cast<int>(i))->name() == expected[i].name);
        assert(schema->field(static_cast<int>(i))->type()->id() == expected[i].type);
        assert(schema->field(static_cast<int>(i))->nullable() == expected[i].nullable);
    }
    assert(schema->GetFieldIndex("raw_data") == -1);
    assert(schema->metadata() != nullptr);
    assert(schema->metadata()->size() == 3);
    assert(schema->metadata()->Get("flowsql.entity").ValueOrDie() == "npm_basic_result");
    assert(schema->metadata()->Get("flowsql.schema_version").ValueOrDie() == "1");
    assert(schema->metadata()->Get("flowsql.timestamp_unit").ValueOrDie() == "ns");
}

void AssertBudgetUsage(const npm::NpmBudgetUsage& usage,
                       uint64_t session,
                       uint64_t module,
                       uint64_t input,
                       uint64_t output) {
    assert(usage.session_state_bytes == session);
    assert(usage.module_state_bytes == module);
    assert(usage.input_batch_bytes == input);
    assert(usage.pending_output_bytes == output);
    assert(npm::NpmTrackedBudgetBytes(usage) == session + module + input);
}

void TestBudgetAccounting() {
    auto config = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline);
    config.max_tracked_bytes = npm::kNpmMinTrackedBytes;
    config.max_pending_output_bytes = npm::kNpmMinPendingOutputBytes;
    npm::NpmBudgetUsage usage;

    assert(npm::ReserveNpmBudget(config, npm::NpmBudgetCategory::kSessionState, 100, &usage) ==
           npm::NpmBudgetError::kNone);
    assert(npm::ReserveNpmBudget(config,
                                npm::NpmBudgetCategory::kModuleState,
                                config.max_tracked_bytes - 100,
                                &usage) == npm::NpmBudgetError::kNone);
    AssertBudgetUsage(usage, 100, config.max_tracked_bytes - 100, 0, 0);
    assert(npm::ReserveNpmBudget(config, npm::NpmBudgetCategory::kInputBatch, 1, &usage) ==
           npm::NpmBudgetError::kTrackedLimitExceeded);
    AssertBudgetUsage(usage, 100, config.max_tracked_bytes - 100, 0, 0);

    assert(npm::ReserveNpmBudget(config,
                                npm::NpmBudgetCategory::kPendingOutput,
                                config.max_pending_output_bytes,
                                &usage) == npm::NpmBudgetError::kNone);
    assert(npm::ReserveNpmBudget(config, npm::NpmBudgetCategory::kPendingOutput, 1, &usage) ==
           npm::NpmBudgetError::kPendingOutputLimitExceeded);
    AssertBudgetUsage(usage, 100, config.max_tracked_bytes - 100, 0, config.max_pending_output_bytes);
    assert(npm::ReleaseNpmBudget(npm::NpmBudgetCategory::kPendingOutput, 1, &usage) ==
           npm::NpmBudgetError::kNone);
    assert(npm::ReserveNpmBudget(config, npm::NpmBudgetCategory::kPendingOutput, 1, &usage) ==
           npm::NpmBudgetError::kNone);
    AssertBudgetUsage(usage, 100, config.max_tracked_bytes - 100, 0, config.max_pending_output_bytes);

    assert(npm::ReleaseNpmBudget(npm::NpmBudgetCategory::kModuleState, 1, &usage) ==
           npm::NpmBudgetError::kNone);
    assert(npm::ReserveNpmBudget(config, npm::NpmBudgetCategory::kInputBatch, 1, &usage) ==
           npm::NpmBudgetError::kNone);
    AssertBudgetUsage(usage, 100, config.max_tracked_bytes - 101, 1, config.max_pending_output_bytes);

    assert(npm::ReleaseNpmBudget(npm::NpmBudgetCategory::kSessionState, 101, &usage) ==
           npm::NpmBudgetError::kReleaseUnderflow);
    AssertBudgetUsage(usage, 100, config.max_tracked_bytes - 101, 1, config.max_pending_output_bytes);
    assert(npm::ReserveNpmBudget(config, static_cast<npm::NpmBudgetCategory>(4), 1, &usage) ==
           npm::NpmBudgetError::kInvalidCategory);
    assert(npm::ReleaseNpmBudget(static_cast<npm::NpmBudgetCategory>(4), 1, &usage) ==
           npm::NpmBudgetError::kInvalidCategory);
    AssertBudgetUsage(usage, 100, config.max_tracked_bytes - 101, 1, config.max_pending_output_bytes);
    assert(npm::ReserveNpmBudget(config, npm::NpmBudgetCategory::kSessionState, 1, nullptr) ==
           npm::NpmBudgetError::kNullUsage);
    assert(npm::ReleaseNpmBudget(npm::NpmBudgetCategory::kSessionState, 1, nullptr) ==
           npm::NpmBudgetError::kNullUsage);
    AssertBudgetUsage(usage, 100, config.max_tracked_bytes - 101, 1, config.max_pending_output_bytes);
}

class FixtureBudget final : public npm::INpmTaskBudget {
 public:
    explicit FixtureBudget(npm::NpmAnalysisConfig config) : config_(config) {}

    npm::NpmBudgetError Reserve(npm::NpmBudgetCategory category, uint64_t bytes) override {
        return npm::ReserveNpmBudget(config_, category, bytes, &usage_);
    }

    npm::NpmBudgetError Release(npm::NpmBudgetCategory category, uint64_t bytes) override {
        return npm::ReleaseNpmBudget(category, bytes, &usage_);
    }

    npm::NpmBudgetUsage Usage() const override { return usage_; }

 private:
    npm::NpmAnalysisConfig config_;
    npm::NpmBudgetUsage usage_;
};

class FixtureWriter final : public npm::INpmResultWriter {
 public:
    int WriteBasic(const npm::NpmBasicResult& result) override {
        if (next_error != 0) return next_error;
        if (npm::ValidateNpmBasicResult(result) != npm::NpmBasicResultError::kNone) return EINVAL;
        last_result = result;
        ++accepted;
        return 0;
    }

    int next_error = 0;
    uint32_t accepted = 0;
    npm::NpmBasicResult last_result;
};

class FixtureModule final : public npm::INpmAnalysisModule {
 public:
    int OnPacket(const npm::NpmPacketView& packet,
                 const npm::NpmSessionView& session,
                 npm::INpmResultWriter& writer) override {
        assert(packet.layer != nullptr);
        assert(packet.payload.size == 2);
        assert(packet.payload[0] == 0x20);
        assert(packet.payload[1] == 0x30);
        assert(packet.packet.meta.timestamp_ns == 123);
        assert(packet.packet.bytes.size == 4);
        assert(packet.direction == npm::NpmPacketDirection::kBToA);
        assert(session.key != nullptr);
        assert(session.key->input_namespace == "pcapfile.capture");
        assert(session.key->observation_domain_id == 7);
        assert(session.key->a.port == 443);
        assert(session.key->b.port == 50000);
        assert(session.packets_ab == 3);
        assert(session.packets_ba == 5);

        npm::NpmBasicResult result;
        result.session_id = session.session_id;
        result.observation_domain_id = session.key->observation_domain_id;
        result.protocol_status = session.protocol_status;
        return writer.WriteBasic(result);
    }

    int OnSessionEnd(const npm::NpmSessionView& session,
                     npm::NpmSessionEndReason reason,
                     npm::INpmResultWriter& writer) override {
        npm::NpmBasicResult result;
        result.session_id = session.session_id;
        result.observation_domain_id = session.key->observation_domain_id;
        result.protocol_status = npm::NpmProtocolStatus::kUnknown;
        result.is_final = true;
        result.end_reason = reason;
        return writer.WriteBasic(result);
    }
};

void TestBorrowedViewsAndModuleInterfaces() {
    static_assert(std::is_abstract_v<npm::INpmTaskBudget>);
    static_assert(std::is_abstract_v<npm::INpmResultWriter>);
    static_assert(std::is_abstract_v<npm::INpmAnalysisModule>);
    static_assert(std::is_same_v<std::underlying_type_t<npm::NpmPacketDirection>, uint8_t>);

    auto bytes_owner = std::make_shared<std::array<uint8_t, 4>>(
        std::array<uint8_t, 4>{0x10, 0x20, 0x30, 0x40});
    const std::weak_ptr<std::array<uint8_t, 4>> weak_owner = bytes_owner;
    npm::NpmPacketView packet_view;
    packet_view.packet.meta.timestamp_ns = 123;
    packet_view.packet.meta.captured_len = bytes_owner->size();
    packet_view.packet.meta.wire_len = bytes_owner->size();
    packet_view.packet.bytes = flowsql::Span<const uint8_t>(bytes_owner->data(), bytes_owner->size());
    packet_view.payload = flowsql::Span<const uint8_t>(bytes_owner->data() + 1, 2);
    packet_view.direction = npm::NpmPacketDirection::kBToA;
    flowsql::packet::PacketLayerInfo layer;
    packet_view.layer = &layer;

    npm::NpmSessionKey key;
    key.input_namespace = "pcapfile.capture";
    key.observation_domain_id = 7;
    key.ip_family = flowsql::packet::AddressFamily::kIPv4;
    key.transport_protocol = 6;
    key.a.port = 443;
    key.b.port = 50000;
    npm::NpmSessionView session_view;
    session_view.session_id = 42;
    session_view.key = &key;
    session_view.packets_ab = 3;
    session_view.packets_ba = 5;
    session_view.protocol_status = npm::NpmProtocolStatus::kPending;

    FixtureModule module;
    FixtureWriter writer;
    assert(bytes_owner.use_count() == 1);
    assert(module.OnPacket(packet_view, session_view, writer) == 0);
    assert(bytes_owner.use_count() == 1);
    assert(writer.accepted == 1);
    assert(writer.last_result.session_id == 42);

    writer.next_error = EIO;
    assert(module.OnPacket(packet_view, session_view, writer) == EIO);
    assert(writer.accepted == 1);
    writer.next_error = 0;
    assert(module.OnSessionEnd(session_view, npm::NpmSessionEndReason::kEof, writer) == 0);
    assert(writer.accepted == 2);
    assert(writer.last_result.is_final);
    assert(writer.last_result.end_reason == npm::NpmSessionEndReason::kEof);
    assert(npm::ValidateNpmBasicResult(writer.last_result) == npm::NpmBasicResultError::kNone);

    FixtureBudget budget(npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline));
    assert(budget.Reserve(npm::NpmBudgetCategory::kModuleState, 16) == npm::NpmBudgetError::kNone);
    assert(budget.Usage().module_state_bytes == 16);
    assert(budget.Release(npm::NpmBudgetCategory::kModuleState, 16) == npm::NpmBudgetError::kNone);
    assert(budget.Usage().module_state_bytes == 0);

    bytes_owner.reset();
    assert(weak_owner.expired());
}

void TestTimeCapabilityRequirements() {
    npm::NpmTimeCapabilities capabilities;
    const auto offline = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline);
    assert(npm::ValidateNpmTimeCapabilities(offline, capabilities) == npm::NpmTimeCapabilityError::kNone);

    auto realtime = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kRealtime);
    assert(npm::ValidateNpmTimeCapabilities(realtime, capabilities) ==
           npm::NpmTimeCapabilityError::kMissingMonotonicTimeDrive);
    capabilities.monotonic_time_drive = true;
    assert(npm::ValidateNpmTimeCapabilities(realtime, capabilities) ==
           npm::NpmTimeCapabilityError::kMissingCaptureTimeProgress);
    capabilities.capture_time_progress = true;
    assert(npm::ValidateNpmTimeCapabilities(realtime, capabilities) ==
           npm::NpmTimeCapabilityError::kMissingSourceIdleConfirmation);
    capabilities.source_idle_confirmation = true;
    assert(npm::ValidateNpmTimeCapabilities(realtime, capabilities) ==
           npm::NpmTimeCapabilityError::kMissingSourceBacklogState);
    capabilities.source_backlog_state = true;
    assert(npm::ValidateNpmTimeCapabilities(realtime, capabilities) == npm::NpmTimeCapabilityError::kNone);

    realtime.run_mode = static_cast<npm::NpmRunMode>(2);
    assert(npm::ValidateNpmTimeCapabilities(realtime, capabilities) ==
           npm::NpmTimeCapabilityError::kInvalidRunMode);
}

void TestSessionPacketCanonicalizationAndObservationDomains() {
    const npm::NpmObservationDomainMap domain_map{"capture-a", {{1, 77}, {2, 77}, {3, 88}}};
    auto forward =
        MakeIpv4TcpPacket("192.0.2.200", 50000, "192.0.2.10", 443, {0x10, 0x20}, kTcpSyn, 0x01020304);
    auto reverse =
        MakeIpv4TcpPacket("192.0.2.10", 443, "192.0.2.200", 50000, {0x30}, kTcpSyn | kTcpAck, 0x50607080);
    npm::NpmSessionPacketBinding forward_binding;
    npm::NpmSessionPacketBinding reverse_binding;
    assert(npm::BuildNpmSessionPacketBinding(domain_map, forward.View(1), forward.layer, &forward_binding) ==
           npm::NpmSessionPacketError::kNone);
    assert(npm::BuildNpmSessionPacketBinding(domain_map, reverse.View(2), reverse.layer, &reverse_binding) ==
           npm::NpmSessionPacketError::kNone);
    assert(SameKey(forward_binding.key, reverse_binding.key));
    assert(forward_binding.direction == npm::NpmPacketDirection::kBToA);
    assert(reverse_binding.direction == npm::NpmPacketDirection::kAToB);
    assert(std::get<flowsql::packet::IPv4Address>(forward_binding.key.a.ip).bytes[3] == 10);
    assert(forward_binding.key.a.port == 443);
    assert(forward_binding.payload.size == 2 && forward_binding.payload[0] == 0x10);
    assert(forward_binding.tcp.valid && forward_binding.tcp.syn && !forward_binding.tcp.ack);
    assert(!forward_binding.tcp.fin && !forward_binding.tcp.rst);
    assert(forward_binding.tcp.sequence == 0x01020304);
    assert(reverse_binding.tcp.valid && reverse_binding.tcp.syn && reverse_binding.tcp.ack);
    assert(reverse_binding.tcp.sequence == 0x50607080);

    auto same_ip = MakeIpv4TcpPacket("198.51.100.4", 60000, "198.51.100.4", 53, {});
    npm::NpmSessionPacketBinding same_ip_binding;
    assert(npm::BuildNpmSessionPacketBinding(domain_map, same_ip.View(1), same_ip.layer, &same_ip_binding) ==
           npm::NpmSessionPacketError::kNone);
    assert(same_ip_binding.key.a.port == 53 && same_ip_binding.key.b.port == 60000);
    assert(same_ip_binding.direction == npm::NpmPacketDirection::kBToA);

    auto ipv6_forward = MakeIpv6UdpPacket("2001:db8::ff", 53000, "2001:db8::1", 53, {0xaa});
    auto ipv6_reverse = MakeIpv6UdpPacket("2001:db8::1", 53, "2001:db8::ff", 53000, {0xbb});
    npm::NpmSessionPacketBinding ipv6_forward_binding;
    npm::NpmSessionPacketBinding ipv6_reverse_binding;
    assert(npm::BuildNpmSessionPacketBinding(
               domain_map, ipv6_forward.View(1), ipv6_forward.layer, &ipv6_forward_binding) ==
           npm::NpmSessionPacketError::kNone);
    assert(npm::BuildNpmSessionPacketBinding(
               domain_map, ipv6_reverse.View(2), ipv6_reverse.layer, &ipv6_reverse_binding) ==
           npm::NpmSessionPacketError::kNone);
    assert(SameKey(ipv6_forward_binding.key, ipv6_reverse_binding.key));
    assert(ipv6_forward_binding.key.ip_family == flowsql::packet::AddressFamily::kIPv6);
    assert(ipv6_forward_binding.direction == npm::NpmPacketDirection::kBToA);
    assert(ipv6_reverse_binding.direction == npm::NpmPacketDirection::kAToB);
    assert(!ipv6_forward_binding.tcp.valid && !ipv6_reverse_binding.tcp.valid);

    npm::NpmSessionPacketBinding other_domain;
    assert(npm::BuildNpmSessionPacketBinding(domain_map, forward.View(3), forward.layer, &other_domain) ==
           npm::NpmSessionPacketError::kNone);
    assert(other_domain.key.observation_domain_id == 88);
    assert(!SameKey(forward_binding.key, other_domain.key));
    const npm::NpmObservationDomainMap other_namespace{"capture-b", {{1, 77}}};
    npm::NpmSessionPacketBinding namespace_binding;
    assert(npm::BuildNpmSessionPacketBinding(
               other_namespace, forward.View(1), forward.layer, &namespace_binding) ==
           npm::NpmSessionPacketError::kNone);
    assert(namespace_binding.key.input_namespace == "capture-b");
    assert(!SameKey(forward_binding.key, namespace_binding.key));

    npm::NpmSessionPacketBinding unchanged;
    unchanged.key.input_namespace = "unchanged";
    unchanged.tcp.sequence = 123;
    assert(npm::BuildNpmSessionPacketBinding(domain_map, forward.View(99), forward.layer, &unchanged) ==
           npm::NpmSessionPacketError::kUnknownSourceId);
    assert(unchanged.key.input_namespace == "unchanged");
    assert(unchanged.tcp.sequence == 123);
    const npm::NpmObservationDomainMap invalid_map{"", {{1, 77}}};
    assert(npm::BuildNpmSessionPacketBinding(invalid_map, forward.View(1), forward.layer, &unchanged) ==
           npm::NpmSessionPacketError::kInvalidObservationDomainMap);
    assert(npm::BuildNpmSessionPacketBinding(domain_map, forward.View(1), forward.layer, nullptr) ==
           npm::NpmSessionPacketError::kNullOutput);
}

void TestSessionPacketPayloadAndInvalidInputs() {
    const npm::NpmObservationDomainMap domain_map{"capture-a", {{1, 77}}};
    auto packet = MakeIpv4TcpPacket("192.0.2.1", 12000, "192.0.2.2", 443, {1, 2, 3, 4});
    npm::NpmSessionPacketBinding binding;
    assert(npm::BuildNpmSessionPacketBinding(domain_map, packet.View(1), packet.layer, &binding) ==
           npm::NpmSessionPacketError::kNone);
    assert(binding.payload.size == 4 && binding.payload[3] == 4);

    auto truncated_body = packet;
    truncated_body.bytes.resize(truncated_body.bytes.size() - 2);
    truncated_body.layer.status = flowsql::packet::LayerStatus::kTruncated;
    assert(npm::BuildNpmSessionPacketBinding(
               domain_map, truncated_body.View(1, packet.bytes.size()), truncated_body.layer, &binding) ==
           npm::NpmSessionPacketError::kNone);
    assert(binding.payload.size == 2 && binding.payload[1] == 2);

    auto truncated_header = packet;
    truncated_header.bytes.resize(sizeof(flowsql::Ipv4Header) + 10);
    truncated_header.layer.status = flowsql::packet::LayerStatus::kTruncated;
    assert(npm::BuildNpmSessionPacketBinding(
               domain_map, truncated_header.View(1, packet.bytes.size()), truncated_header.layer, &binding) ==
           npm::NpmSessionPacketError::kIncompleteTransportHeader);

    auto invalid_payload = packet;
    ++invalid_payload.layer.payload_offset;
    assert(npm::BuildNpmSessionPacketBinding(
               domain_map, invalid_payload.View(1), invalid_payload.layer, &binding) ==
           npm::NpmSessionPacketError::kInvalidPayloadBounds);

    auto invalid_udp_length = MakeIpv6UdpPacket("2001:db8::1", 1000, "2001:db8::2", 2000, {1, 2});
    auto* udp = reinterpret_cast<flowsql::UdpHeader*>(invalid_udp_length.bytes.data() + sizeof(flowsql::Ipv6Header));
    udp->length = htons(1000);
    assert(npm::BuildNpmSessionPacketBinding(
               domain_map, invalid_udp_length.View(1), invalid_udp_length.layer, &binding) ==
           npm::NpmSessionPacketError::kInvalidPayloadBounds);

    auto invalid = packet;
    invalid.layer.status = flowsql::packet::LayerStatus::kMalformed;
    assert(npm::BuildNpmSessionPacketBinding(domain_map, invalid.View(1), invalid.layer, &binding) ==
           npm::NpmSessionPacketError::kMalformedLayer);
    invalid.layer.status = flowsql::packet::LayerStatus::kNotDecoded;
    assert(npm::BuildNpmSessionPacketBinding(domain_map, invalid.View(1), invalid.layer, &binding) ==
           npm::NpmSessionPacketError::kLayerUnavailable);
    invalid = packet;
    invalid.layer.src_ip = std::monostate{};
    assert(npm::BuildNpmSessionPacketBinding(domain_map, invalid.View(1), invalid.layer, &binding) ==
           npm::NpmSessionPacketError::kIncompleteEndpoint);
    invalid = packet;
    invalid.layer.ports_valid = 0;
    assert(npm::BuildNpmSessionPacketBinding(domain_map, invalid.View(1), invalid.layer, &binding) ==
           npm::NpmSessionPacketError::kIncompleteEndpoint);
    invalid = packet;
    invalid.layer.transport_protocol = flowsql::ipv4::eNext::UDP;
    assert(npm::BuildNpmSessionPacketBinding(domain_map, invalid.View(1), invalid.layer, &binding) ==
           npm::NpmSessionPacketError::kUnsupportedTransportProtocol);
    auto invalid_view = packet.View(1);
    ++invalid_view.meta.captured_len;
    assert(npm::BuildNpmSessionPacketBinding(domain_map, invalid_view, packet.layer, &binding) ==
           npm::NpmSessionPacketError::kInvalidPacketBounds);
}

PacketFixture MakeTunnelPacket() {
    PacketFixture fixture;
    fixture.bytes.resize(78);
    auto* outer_ip = reinterpret_cast<flowsql::Ipv4Header*>(fixture.bytes.data());
    outer_ip->version = 4;
    outer_ip->ihl = 5;
    outer_ip->total_length = htons(fixture.bytes.size());
    outer_ip->protocol = flowsql::ipv4::eNext::UDP;
    outer_ip->src_addr = ParseIpv4("198.51.100.1");
    outer_ip->dst_addr = ParseIpv4("198.51.100.2");
    auto* outer_udp = reinterpret_cast<flowsql::UdpHeader*>(fixture.bytes.data() + 20);
    outer_udp->src_port = htons(40000);
    outer_udp->dst_port = htons(4789);
    outer_udp->length = htons(fixture.bytes.size() - 20);
    auto* inner_ip = reinterpret_cast<flowsql::Ipv4Header*>(fixture.bytes.data() + 36);
    inner_ip->version = 4;
    inner_ip->ihl = 5;
    inner_ip->total_length = htons(fixture.bytes.size() - 36);
    inner_ip->protocol = flowsql::ipv4::eNext::TCP;
    inner_ip->src_addr = ParseIpv4("192.0.2.1");
    inner_ip->dst_addr = ParseIpv4("192.0.2.2");
    auto* inner_tcp = reinterpret_cast<flowsql::TcpHeader*>(fixture.bytes.data() + 56);
    inner_tcp->src_port = htons(12000);
    inner_tcp->dst_port = htons(443);
    inner_tcp->offset = 5;

    fixture.layer.status = flowsql::packet::LayerStatus::kDecoded;
    fixture.layer.layer_count = 5;
    fixture.layer.layers[0] = {static_cast<uint16_t>(flowsql::eLayer::IPv4), 0};
    fixture.layer.layers[1] = {static_cast<uint16_t>(flowsql::eLayer::UDP), 20};
    fixture.layer.layers[2] = {static_cast<uint16_t>(flowsql::eLayer::VXLAN), 28};
    fixture.layer.layers[3] = {static_cast<uint16_t>(flowsql::eLayer::IPv4), 36};
    fixture.layer.layers[4] = {static_cast<uint16_t>(flowsql::eLayer::TCP), 56};
    fixture.layer.network_layer_index = 3;
    fixture.layer.transport_layer_index = 4;
    fixture.layer.payload_offset = 76;
    fixture.layer.src_ip = inner_ip->src_addr;
    fixture.layer.dst_ip = inner_ip->dst_addr;
    fixture.layer.transport_protocol = flowsql::ipv4::eNext::TCP;
    fixture.layer.src_port = 12000;
    fixture.layer.dst_port = 443;
    fixture.layer.ports_valid = 1;
    return fixture;
}

void TestSessionPacketFragmentsAndTunnelContext() {
    const npm::NpmObservationDomainMap domain_map{"capture-a", {{1, 77}}};
    npm::NpmSessionPacketBinding binding;

    auto ipv4_fragment = MakeIpv4TcpPacket("192.0.2.1", 12000, "192.0.2.2", 443, {1});
    auto* ipv4 = reinterpret_cast<flowsql::Ipv4Header*>(ipv4_fragment.bytes.data());
    ipv4->fragment_offset = htons(1);
    assert(npm::BuildNpmSessionPacketBinding(
               domain_map, ipv4_fragment.View(1), ipv4_fragment.layer, &binding) ==
           npm::NpmSessionPacketError::kNonInitialFragment);

    auto ipv6_fragment = MakeIpv6UdpPacket("2001:db8::1", 1000, "2001:db8::2", 2000, {1}, true, 2);
    ipv6_fragment.layer.layer_count = 2;
    ipv6_fragment.layer.transport_layer_index = flowsql::packet::kNoLayerIndex;
    ipv6_fragment.layer.ports_valid = 0;
    assert(npm::BuildNpmSessionPacketBinding(
               domain_map, ipv6_fragment.View(1), ipv6_fragment.layer, &binding) ==
           npm::NpmSessionPacketError::kNonInitialFragment);
    auto ipv6_first_fragment = MakeIpv6UdpPacket("2001:db8::1", 1000, "2001:db8::2", 2000, {1}, true, 0);
    assert(npm::BuildNpmSessionPacketBinding(
               domain_map, ipv6_first_fragment.View(1), ipv6_first_fragment.layer, &binding) ==
           npm::NpmSessionPacketError::kNone);
    assert(binding.payload.size == 1 && binding.payload[0] == 1);

    auto tunnel = MakeTunnelPacket();
    assert(npm::BuildNpmSessionPacketBinding(domain_map, tunnel.View(1), tunnel.layer, &binding) ==
           npm::NpmSessionPacketError::kAmbiguousTunnelContext);
    tunnel.layer.endpoint_scope = flowsql::packet::EndpointScope::kOutermost;
    tunnel.layer.network_layer_index = 0;
    tunnel.layer.transport_layer_index = 1;
    tunnel.layer.payload_offset = 28;
    tunnel.layer.src_ip = ParseIpv4("198.51.100.1");
    tunnel.layer.dst_ip = ParseIpv4("198.51.100.2");
    tunnel.layer.transport_protocol = flowsql::ipv4::eNext::UDP;
    tunnel.layer.src_port = 40000;
    tunnel.layer.dst_port = 4789;
    assert(npm::BuildNpmSessionPacketBinding(domain_map, tunnel.View(1), tunnel.layer, &binding) ==
           npm::NpmSessionPacketError::kNone);
    assert(binding.key.transport_protocol == flowsql::ipv4::eNext::UDP);
    assert(binding.payload.size == tunnel.bytes.size() - 28);
}

npm::NpmSessionPacketBinding BuildBinding(const npm::NpmObservationDomainMap& domain_map,
                                          const PacketFixture& packet,
                                          uint32_t source_id,
                                          int64_t timestamp_ns,
                                          uint32_t wire_len,
                                          flowsql::packet::PacketMeta* meta) {
    auto view = packet.View(source_id, wire_len);
    view.meta.timestamp_ns = timestamp_ns;
    npm::NpmSessionPacketBinding binding;
    assert(npm::BuildNpmSessionPacketBinding(domain_map, view, packet.layer, &binding) ==
           npm::NpmSessionPacketError::kNone);
    *meta = view.meta;
    return binding;
}

npm::NpmSessionTableError ObserveActive(npm::NpmSessionTable& table,
                                        const npm::NpmSessionPacketBinding& binding,
                                        const flowsql::packet::PacketMeta& meta,
                                        npm::NpmSessionView* output) {
    if (!output) return table.Observe(binding, meta, nullptr);
    npm::NpmSessionObserveResult result;
    const auto error = table.Observe(binding, meta, &result);
    if (error != npm::NpmSessionTableError::kNone) return error;
    assert(result.has_active_session);
    assert(result.ended_sessions.empty());
    *output = result.active_session;
    return npm::NpmSessionTableError::kNone;
}

void TestSessionTableBidirectionalCountersAndLookup() {
    const npm::NpmObservationDomainMap domain_map{"capture-a", {{1, 77}, {2, 77}}};
    const auto forward = MakeIpv4TcpPacket("192.0.2.200", 50000, "192.0.2.10", 443, {0x10});
    const auto reverse = MakeIpv4TcpPacket("192.0.2.10", 443, "192.0.2.200", 50000, {0x20});
    flowsql::packet::PacketMeta first_meta;
    flowsql::packet::PacketMeta reverse_meta;
    const auto first = BuildBinding(domain_map, forward, 1, 300, 100, &first_meta);
    const auto reverse_from_other_queue = BuildBinding(domain_map, reverse, 2, 100, 120, &reverse_meta);

    npm::NpmSessionTable table(4);
    npm::NpmSessionView view;
    assert(ObserveActive(table, first, first_meta, &view) == npm::NpmSessionTableError::kNone);
    assert(table.size() == 1);
    assert(view.session_id == 1);
    assert(view.key != nullptr && SameKey(*view.key, first.key));
    assert(view.key != &first.key);
    assert(view.first_ns == 300 && view.last_ns == 300);
    assert(view.packets_ab == 0 && view.packets_ba == 1);
    assert(view.wire_bytes_ab == 0 && view.wire_bytes_ba == 100);
    assert(view.protocol_status == npm::NpmProtocolStatus::kPending);
    assert(!view.protocol_id.has_value() && !view.protocol_sub_id.has_value());

    assert(ObserveActive(table, reverse_from_other_queue, reverse_meta, &view) ==
           npm::NpmSessionTableError::kNone);
    assert(table.size() == 1 && view.session_id == 1);
    assert(view.first_ns == 100 && view.last_ns == 300);
    assert(view.packets_ab == 1 && view.packets_ba == 1);
    assert(view.wire_bytes_ab == 120 && view.wire_bytes_ba == 100);

    auto later_meta = first_meta;
    later_meta.timestamp_ns = 500;
    later_meta.wire_len = 140;
    assert(ObserveActive(table, first, later_meta, &view) == npm::NpmSessionTableError::kNone);
    assert(view.first_ns == 100 && view.last_ns == 500);
    assert(view.packets_ab == 1 && view.packets_ba == 2);
    assert(view.wire_bytes_ab == 120 && view.wire_bytes_ba == 240);

    npm::NpmSessionView found;
    assert(table.Find(first.key, &found) == npm::NpmSessionTableError::kNone);
    assert(found.session_id == view.session_id && found.key == view.key);
    assert(found.first_ns == view.first_ns && found.last_ns == view.last_ns);
    assert(found.packets_ab == view.packets_ab && found.packets_ba == view.packets_ba);
    assert(found.wire_bytes_ab == view.wire_bytes_ab && found.wire_bytes_ba == view.wire_bytes_ba);
}

void TestSessionTableIsolationCapacityAndErrors() {
    const auto packet = MakeIpv6UdpPacket("2001:db8::1", 53000, "2001:db8::2", 53, {});
    const npm::NpmObservationDomainMap domain_seven{"capture-a", {{1, 7}}};
    const npm::NpmObservationDomainMap domain_eight{"capture-a", {{1, 8}}};
    const npm::NpmObservationDomainMap other_namespace{"capture-b", {{1, 7}}};
    flowsql::packet::PacketMeta meta;
    const auto first = BuildBinding(domain_seven, packet, 1, 10, 80, &meta);
    const auto different_domain = BuildBinding(domain_eight, packet, 1, 20, 80, &meta);
    const auto different_namespace = BuildBinding(other_namespace, packet, 1, 30, 80, &meta);

    npm::NpmSessionTable table(2);
    npm::NpmSessionView first_view;
    npm::NpmSessionView second_view;
    assert(ObserveActive(table, first, meta, &first_view) == npm::NpmSessionTableError::kNone);
    assert(ObserveActive(table, different_domain, meta, &second_view) == npm::NpmSessionTableError::kNone);
    assert(first_view.session_id == 1 && second_view.session_id == 2);
    assert(table.size() == 2);

    npm::NpmSessionView unchanged;
    unchanged.session_id = 99;
    assert(ObserveActive(table, different_namespace, meta, &unchanged) ==
           npm::NpmSessionTableError::kSessionLimitExceeded);
    assert(table.size() == 2 && unchanged.session_id == 99);

    auto existing_meta = meta;
    existing_meta.timestamp_ns = 40;
    existing_meta.wire_len = 90;
    assert(ObserveActive(table, first, existing_meta, &unchanged) == npm::NpmSessionTableError::kNone);
    assert(unchanged.session_id == 1 && unchanged.packets_ab == 2);
    assert(unchanged.wire_bytes_ab == 170);

    npm::NpmSessionView missing;
    missing.session_id = 77;
    assert(table.Find(different_namespace.key, &missing) == npm::NpmSessionTableError::kNotFound);
    assert(missing.session_id == 77 && table.size() == 2);
    assert(table.Find(first.key, nullptr) == npm::NpmSessionTableError::kNullOutput);
    assert(ObserveActive(table, first, existing_meta, nullptr) == npm::NpmSessionTableError::kNullOutput);
    assert(table.size() == 2);
    npm::NpmSessionView after_null_output;
    assert(table.Find(first.key, &after_null_output) == npm::NpmSessionTableError::kNone);
    assert(after_null_output.packets_ab == 2 && after_null_output.wire_bytes_ab == 170);

    auto invalid_direction = first;
    invalid_direction.direction = static_cast<npm::NpmPacketDirection>(2);
    unchanged.session_id = 66;
    assert(ObserveActive(table, invalid_direction, existing_meta, &unchanged) ==
           npm::NpmSessionTableError::kInvalidDirection);
    assert(unchanged.session_id == 66 && table.size() == 2);
    npm::NpmSessionView after_invalid_direction;
    assert(table.Find(first.key, &after_invalid_direction) == npm::NpmSessionTableError::kNone);
    assert(after_invalid_direction.packets_ab == 2 && after_invalid_direction.wire_bytes_ab == 170);

    npm::NpmSessionObserveResult unchanged_result;
    unchanged_result.has_active_session = true;
    unchanged_result.active_session.session_id = 88;
    unchanged_result.ended_sessions.emplace_back().session_id = 99;
    assert(table.Observe(invalid_direction, existing_meta, &unchanged_result) ==
           npm::NpmSessionTableError::kInvalidDirection);
    assert(unchanged_result.has_active_session && unchanged_result.active_session.session_id == 88);
    assert(unchanged_result.ended_sessions.size() == 1 && unchanged_result.ended_sessions[0].session_id == 99);
}

npm::NpmAnalysisConfig MakeIdleTestConfig(npm::NpmRunMode mode) {
    auto config = npm::DefaultNpmAnalysisConfig(mode);
    config.tcp_idle_timeout_ns = 30 * npm::kNpmNanosecondsPerSecond;
    config.udp_idle_timeout_ns = 20 * npm::kNpmNanosecondsPerSecond;
    config.out_of_order_tolerance_ns = 10 * npm::kNpmNanosecondsPerSecond;
    config.max_active_sessions = 4;
    return config;
}

npm::NpmCaptureProgressUpdate OfflineProgress(int64_t capture_time_ns) {
    npm::NpmCaptureProgressUpdate update;
    update.capture_time_ns = capture_time_ns;
    return update;
}

void TestOfflineEventWatermarkAndIdleRetirement() {
    constexpr int64_t second = npm::kNpmNanosecondsPerSecond;
    const npm::NpmObservationDomainMap domain_map{"capture-a", {{1, 77}}};
    const auto tcp_packet = MakeIpv4TcpPacket("192.0.2.1", 50000, "192.0.2.2", 443, {});
    const auto udp_packet = MakeIpv6UdpPacket("2001:db8::1", 53000, "2001:db8::2", 53, {});
    flowsql::packet::PacketMeta tcp_meta;
    flowsql::packet::PacketMeta udp_meta;
    const auto tcp = BuildBinding(domain_map, tcp_packet, 1, 50 * second, 100, &tcp_meta);
    const auto udp = BuildBinding(domain_map, udp_packet, 1, 70 * second, 80, &udp_meta);

    npm::NpmSessionTable table(MakeIdleTestConfig(npm::NpmRunMode::kOffline));
    npm::NpmSessionView observed;
    assert(ObserveActive(table, tcp, tcp_meta, &observed) == npm::NpmSessionTableError::kNone);
    const uint64_t tcp_session_id = observed.session_id;
    assert(ObserveActive(table, udp, udp_meta, &observed) == npm::NpmSessionTableError::kNone);
    assert(table.size() == 2);

    auto progress = table.AdvanceCaptureProgress(OfflineProgress(100 * second));
    assert(progress.disposition == npm::NpmCaptureProgressDisposition::kAdvanced);
    assert(progress.watermark_initialized && progress.watermark_ns == 90 * second);
    assert(progress.ended_sessions.size() == 2);
    assert(progress.ended_sessions[0].session_id == tcp_session_id);
    assert(progress.ended_sessions[0].end_reason == npm::NpmSessionEndReason::kIdleTimeout);
    const auto retired_view = progress.ended_sessions[0].View();
    assert(retired_view.key == &progress.ended_sessions[0].key);
    assert(retired_view.session_id == tcp_session_id && retired_view.first_ns == 50 * second);
    assert(retired_view.packets_ab + retired_view.packets_ba == 1);
    assert(table.size() == 0);
    assert(table.Find(tcp.key, &observed) == npm::NpmSessionTableError::kNotFound);
    assert(table.Find(udp.key, &observed) == npm::NpmSessionTableError::kNotFound);

    auto regressed = table.AdvanceCaptureProgress(OfflineProgress(95 * second));
    assert(regressed.disposition == npm::NpmCaptureProgressDisposition::kUnchanged);
    assert(regressed.watermark_ns == 90 * second && regressed.ended_sessions.empty());
    auto repeated = table.AdvanceCaptureProgress(OfflineProgress(100 * second));
    assert(repeated.disposition == npm::NpmCaptureProgressDisposition::kUnchanged);
    assert(repeated.ended_sessions.empty());
}

void TestIdleDeadlineReplacementLatePacketAndCapacityRelease() {
    constexpr int64_t second = npm::kNpmNanosecondsPerSecond;
    auto config = MakeIdleTestConfig(npm::NpmRunMode::kOffline);
    config.max_active_sessions = 1;
    const npm::NpmObservationDomainMap domain_map{"capture-a", {{1, 77}}};
    const auto packet = MakeIpv4TcpPacket("192.0.2.1", 50000, "192.0.2.2", 443, {});
    flowsql::packet::PacketMeta meta;
    const auto binding = BuildBinding(domain_map, packet, 1, 50 * second, 100, &meta);

    npm::NpmSessionTable table(config);
    npm::NpmSessionView view;
    assert(ObserveActive(table, binding, meta, &view) == npm::NpmSessionTableError::kNone);
    assert(view.session_id == 1);
    meta.timestamp_ns = 70 * second;
    meta.wire_len = 110;
    assert(ObserveActive(table, binding, meta, &view) == npm::NpmSessionTableError::kNone);
    assert(view.last_ns == 70 * second && view.packets_ab + view.packets_ba == 2);
    meta.timestamp_ns = 95 * second;
    meta.wire_len = 120;
    assert(ObserveActive(table, binding, meta, &view) == npm::NpmSessionTableError::kNone);
    assert(view.last_ns == 95 * second && view.packets_ab + view.packets_ba == 3);

    auto before_new_deadline = table.AdvanceCaptureProgress(OfflineProgress(100 * second));
    assert(before_new_deadline.watermark_ns == 90 * second);
    assert(before_new_deadline.ended_sessions.empty() && table.size() == 1);

    meta.timestamp_ns = 90 * second;
    meta.wire_len = 130;
    assert(ObserveActive(table, binding, meta, &view) == npm::NpmSessionTableError::kNone);
    assert(view.first_ns == 50 * second && view.last_ns == 95 * second);
    assert(view.packets_ab + view.packets_ba == 4);
    npm::NpmSessionView unchanged;
    unchanged.session_id = 99;
    meta.timestamp_ns = 89 * second;
    assert(ObserveActive(table, binding, meta, &unchanged) == npm::NpmSessionTableError::kLatePacket);
    assert(unchanged.session_id == 99 && table.size() == 1);

    auto at_replaced_deadline = table.AdvanceCaptureProgress(OfflineProgress(110 * second));
    assert(at_replaced_deadline.watermark_ns == 100 * second);
    assert(at_replaced_deadline.ended_sessions.empty() && table.size() == 1);
    auto at_new_deadline = table.AdvanceCaptureProgress(OfflineProgress(135 * second));
    assert(at_new_deadline.watermark_ns == 125 * second);
    assert(at_new_deadline.ended_sessions.size() == 1 && table.size() == 0);
    assert(at_new_deadline.ended_sessions[0].session_id == 1);
    assert(at_new_deadline.ended_sessions[0].packets_ab + at_new_deadline.ended_sessions[0].packets_ba == 4);

    meta.timestamp_ns = 124 * second;
    assert(ObserveActive(table, binding, meta, &unchanged) == npm::NpmSessionTableError::kLatePacket);
    assert(unchanged.session_id == 99 && table.size() == 0);

    meta.timestamp_ns = 125 * second;
    assert(ObserveActive(table, binding, meta, &view) == npm::NpmSessionTableError::kNone);
    assert(view.session_id == 2 && table.size() == 1);
    assert(view.first_ns == 125 * second && view.last_ns == 125 * second);
}

npm::NpmCaptureProgressUpdate RealtimeProgress(int64_t capture_time_ns,
                                               bool packet_observed,
                                               bool source_idle_confirmed,
                                               bool source_backlog_known,
                                               bool source_has_backlog) {
    npm::NpmCaptureProgressUpdate update;
    update.capture_time_ns = capture_time_ns;
    update.packet_observed = packet_observed;
    update.source_idle_confirmed = source_idle_confirmed;
    update.source_backlog_known = source_backlog_known;
    update.source_has_backlog = source_has_backlog;
    return update;
}

void TestRealtimeProgressRequiresIdleAndBacklogEvidence() {
    constexpr int64_t second = npm::kNpmNanosecondsPerSecond;
    auto config = MakeIdleTestConfig(npm::NpmRunMode::kRealtime);
    config.out_of_order_tolerance_ns = 0;
    const npm::NpmObservationDomainMap domain_map{"capture-a", {{1, 77}}};
    const auto packet = MakeIpv6UdpPacket("2001:db8::1", 53000, "2001:db8::2", 53, {});
    flowsql::packet::PacketMeta meta;
    const auto binding = BuildBinding(domain_map, packet, 1, 10 * second, 80, &meta);

    npm::NpmSessionTable packet_driven(config);
    npm::NpmSessionView view;
    assert(ObserveActive(packet_driven, binding, meta, &view) == npm::NpmSessionTableError::kNone);
    auto deferred = packet_driven.AdvanceCaptureProgress(RealtimeProgress(30 * second, true, false, false, false));
    assert(deferred.disposition == npm::NpmCaptureProgressDisposition::kDeferredBacklogUnknown);
    assert(!deferred.watermark_initialized && deferred.ended_sessions.empty() && packet_driven.size() == 1);
    deferred = packet_driven.AdvanceCaptureProgress(RealtimeProgress(30 * second, true, false, true, true));
    assert(deferred.disposition == npm::NpmCaptureProgressDisposition::kDeferredBacklogged);
    assert(!deferred.watermark_initialized && deferred.ended_sessions.empty() && packet_driven.size() == 1);
    deferred = packet_driven.AdvanceCaptureProgress(RealtimeProgress(30 * second, false, false, true, false));
    assert(deferred.disposition == npm::NpmCaptureProgressDisposition::kDeferredIdleUnconfirmed);
    assert(!deferred.watermark_initialized && deferred.ended_sessions.empty() && packet_driven.size() == 1);

    auto advanced = packet_driven.AdvanceCaptureProgress(RealtimeProgress(30 * second, true, false, true, false));
    assert(advanced.disposition == npm::NpmCaptureProgressDisposition::kAdvanced);
    assert(advanced.watermark_ns == 30 * second && advanced.ended_sessions.size() == 1);
    assert(packet_driven.size() == 0);

    npm::NpmSessionTable idle_driven(config);
    assert(ObserveActive(idle_driven, binding, meta, &view) == npm::NpmSessionTableError::kNone);
    advanced = idle_driven.AdvanceCaptureProgress(RealtimeProgress(30 * second, false, true, true, false));
    assert(advanced.disposition == npm::NpmCaptureProgressDisposition::kAdvanced);
    assert(advanced.ended_sessions.size() == 1 && idle_driven.size() == 0);
}

void TestTcpSynRetransmissionAndTupleReuse() {
    const npm::NpmObservationDomainMap domain_map{"capture-a", {{1, 77}}};
    const auto middle_packet =
        MakeIpv4TcpPacket("192.0.2.1", 50000, "192.0.2.2", 443, {}, kTcpAck, 1000);
    const auto initial_syn =
        MakeIpv4TcpPacket("192.0.2.1", 50000, "192.0.2.2", 443, {}, kTcpSyn, 100);
    const auto syn_ack =
        MakeIpv4TcpPacket("192.0.2.2", 443, "192.0.2.1", 50000, {}, kTcpSyn | kTcpAck, 500);
    const auto changed_syn =
        MakeIpv4TcpPacket("192.0.2.1", 50000, "192.0.2.2", 443, {}, kTcpSyn, 101);
    const auto reverse_syn =
        MakeIpv4TcpPacket("192.0.2.2", 443, "192.0.2.1", 50000, {}, kTcpSyn, 700);

    flowsql::packet::PacketMeta middle_meta;
    flowsql::packet::PacketMeta syn_meta;
    flowsql::packet::PacketMeta retransmit_meta;
    flowsql::packet::PacketMeta syn_ack_meta;
    flowsql::packet::PacketMeta changed_syn_meta;
    flowsql::packet::PacketMeta reverse_syn_meta;
    const auto middle = BuildBinding(domain_map, middle_packet, 1, 10, 100, &middle_meta);
    const auto syn = BuildBinding(domain_map, initial_syn, 1, 20, 110, &syn_meta);
    const auto retransmit = BuildBinding(domain_map, initial_syn, 1, 21, 111, &retransmit_meta);
    const auto syn_ack_binding = BuildBinding(domain_map, syn_ack, 1, 22, 112, &syn_ack_meta);
    const auto changed = BuildBinding(domain_map, changed_syn, 1, 30, 120, &changed_syn_meta);
    const auto reverse = BuildBinding(domain_map, reverse_syn, 1, 40, 130, &reverse_syn_meta);

    assert(middle.tcp.valid && !middle.tcp.syn && middle.tcp.ack && middle.tcp.sequence == 1000);
    assert(syn.tcp.valid && syn.tcp.syn && !syn.tcp.ack && syn.tcp.sequence == 100);
    assert(retransmit.direction == syn.direction && reverse.direction != syn.direction);

    npm::NpmSessionTable table(4);
    npm::NpmSessionObserveResult result;
    assert(table.Observe(middle, middle_meta, &result) == npm::NpmSessionTableError::kNone);
    assert(result.has_active_session && result.active_session.session_id == 1);
    assert(result.ended_sessions.empty());

    assert(table.Observe(syn, syn_meta, &result) == npm::NpmSessionTableError::kNone);
    assert(result.has_active_session && result.active_session.session_id == 2);
    assert(result.active_session.packets_ab + result.active_session.packets_ba == 1);
    assert(result.ended_sessions.size() == 1);
    assert(result.ended_sessions[0].session_id == 1);
    assert(result.ended_sessions[0].end_reason == npm::NpmSessionEndReason::kTupleReuse);
    assert(result.ended_sessions[0].packets_ab + result.ended_sessions[0].packets_ba == 1);

    assert(table.Observe(retransmit, retransmit_meta, &result) == npm::NpmSessionTableError::kNone);
    assert(result.has_active_session && result.active_session.session_id == 2);
    assert(result.active_session.packets_ab + result.active_session.packets_ba == 2);
    assert(result.ended_sessions.empty());

    assert(table.Observe(syn_ack_binding, syn_ack_meta, &result) == npm::NpmSessionTableError::kNone);
    assert(result.has_active_session && result.active_session.session_id == 2);
    assert(result.active_session.packets_ab + result.active_session.packets_ba == 3);
    assert(result.ended_sessions.empty());

    assert(table.Observe(changed, changed_syn_meta, &result) == npm::NpmSessionTableError::kNone);
    assert(result.has_active_session && result.active_session.session_id == 3);
    assert(result.active_session.packets_ab + result.active_session.packets_ba == 1);
    assert(result.ended_sessions.size() == 1 && result.ended_sessions[0].session_id == 2);
    assert(result.ended_sessions[0].packets_ab + result.ended_sessions[0].packets_ba == 3);
    assert(result.ended_sessions[0].end_reason == npm::NpmSessionEndReason::kTupleReuse);

    assert(table.Observe(reverse, reverse_syn_meta, &result) == npm::NpmSessionTableError::kNone);
    assert(result.has_active_session && result.active_session.session_id == 4);
    assert(result.active_session.packets_ab + result.active_session.packets_ba == 1);
    assert(result.ended_sessions.size() == 1 && result.ended_sessions[0].session_id == 3);
    assert(result.ended_sessions[0].packets_ab + result.ended_sessions[0].packets_ba == 1);
    assert(result.ended_sessions[0].end_reason == npm::NpmSessionEndReason::kTupleReuse);
    assert(table.size() == 1);
}

void TestTcpFinRstAndUdpLifecycle() {
    const npm::NpmObservationDomainMap domain_map{"capture-a", {{1, 77}}};
    const auto data_packet =
        MakeIpv4TcpPacket("192.0.2.1", 50000, "192.0.2.2", 443, {}, kTcpAck, 100);
    const auto forward_fin =
        MakeIpv4TcpPacket("192.0.2.1", 50000, "192.0.2.2", 443, {}, kTcpFin | kTcpAck, 200);
    const auto reverse_fin =
        MakeIpv4TcpPacket("192.0.2.2", 443, "192.0.2.1", 50000, {}, kTcpFin | kTcpAck, 300);
    const auto reverse_rst =
        MakeIpv4TcpPacket("192.0.2.2", 443, "192.0.2.1", 50000, {}, kTcpRst | kTcpAck, 400);

    flowsql::packet::PacketMeta data_meta;
    flowsql::packet::PacketMeta fin_meta;
    flowsql::packet::PacketMeta fin_retransmit_meta;
    flowsql::packet::PacketMeta reverse_fin_meta;
    flowsql::packet::PacketMeta rst_meta;
    const auto data = BuildBinding(domain_map, data_packet, 1, 1, 100, &data_meta);
    const auto fin = BuildBinding(domain_map, forward_fin, 1, 2, 110, &fin_meta);
    const auto fin_retransmit = BuildBinding(domain_map, forward_fin, 1, 3, 120, &fin_retransmit_meta);
    const auto opposite_fin = BuildBinding(domain_map, reverse_fin, 1, 4, 130, &reverse_fin_meta);
    const auto rst = BuildBinding(domain_map, reverse_rst, 1, 11, 95, &rst_meta);

    npm::NpmSessionTable fin_table(4);
    npm::NpmSessionObserveResult result;
    assert(fin_table.Observe(data, data_meta, &result) == npm::NpmSessionTableError::kNone);
    assert(fin_table.Observe(fin, fin_meta, &result) == npm::NpmSessionTableError::kNone);
    assert(result.has_active_session && result.active_session.session_id == 1);
    assert(result.active_session.packets_ab == 2 && result.active_session.packets_ba == 0);
    assert(result.ended_sessions.empty());
    assert(fin_table.Observe(fin_retransmit, fin_retransmit_meta, &result) ==
           npm::NpmSessionTableError::kNone);
    assert(result.has_active_session && result.active_session.session_id == 1);
    assert(result.active_session.packets_ab == 3 && result.active_session.packets_ba == 0);
    assert(result.ended_sessions.empty());
    assert(fin_table.Observe(opposite_fin, reverse_fin_meta, &result) == npm::NpmSessionTableError::kNone);
    assert(!result.has_active_session && fin_table.size() == 0);
    assert(result.ended_sessions.size() == 1);
    assert(result.ended_sessions[0].session_id == 1);
    assert(result.ended_sessions[0].end_reason == npm::NpmSessionEndReason::kClosed);
    assert(result.ended_sessions[0].packets_ab == 3 && result.ended_sessions[0].packets_ba == 1);
    assert(result.ended_sessions[0].wire_bytes_ab + result.ended_sessions[0].wire_bytes_ba == 460);

    npm::NpmSessionTable rst_table(4);
    assert(rst_table.Observe(data, data_meta, &result) == npm::NpmSessionTableError::kNone);
    assert(rst_table.Observe(rst, rst_meta, &result) == npm::NpmSessionTableError::kNone);
    assert(!result.has_active_session && rst_table.size() == 0);
    assert(result.ended_sessions.size() == 1);
    assert(result.ended_sessions[0].end_reason == npm::NpmSessionEndReason::kClosed);
    assert(result.ended_sessions[0].packets_ab + result.ended_sessions[0].packets_ba == 2);
    assert(result.ended_sessions[0].wire_bytes_ab + result.ended_sessions[0].wire_bytes_ba == 195);

    const auto udp_packet = MakeIpv6UdpPacket("2001:db8::1", 53000, "2001:db8::2", 53, {});
    flowsql::packet::PacketMeta udp_meta;
    const auto udp = BuildBinding(domain_map, udp_packet, 1, 20, 80, &udp_meta);
    assert(!udp.tcp.valid);
    npm::NpmSessionTable udp_table(4);
    assert(udp_table.Observe(udp, udp_meta, &result) == npm::NpmSessionTableError::kNone);
    assert(result.has_active_session && result.ended_sessions.empty());
    udp_meta.timestamp_ns = 21;
    assert(udp_table.Observe(udp, udp_meta, &result) == npm::NpmSessionTableError::kNone);
    assert(result.has_active_session && result.active_session.session_id == 1);
    assert(result.active_session.packets_ab + result.active_session.packets_ba == 2);
    assert(result.ended_sessions.empty() && udp_table.size() == 1);
}

class RecordingEndModule final : public npm::INpmAnalysisModule {
 public:
    RecordingEndModule(uint64_t marker,
                       std::vector<uint64_t>* calls,
                       std::vector<npm::NpmSessionEndReason>* reasons,
                       bool write_result,
                       int error)
        : marker_(marker), calls_(calls), reasons_(reasons), write_result_(write_result), error_(error) {}

    int OnPacket(const npm::NpmPacketView&,
                 const npm::NpmSessionView&,
                 npm::INpmResultWriter&) override {
        return 0;
    }

    int OnSessionEnd(const npm::NpmSessionView& session,
                     npm::NpmSessionEndReason reason,
                     npm::INpmResultWriter& writer) override {
        assert(session.key != nullptr);
        calls_->push_back(session.session_id * 10 + marker_);
        reasons_->push_back(reason);
        if (error_ != 0) return error_;
        if (!write_result_) return 0;

        npm::NpmBasicResult result;
        result.session_id = session.session_id;
        result.observation_domain_id = session.key->observation_domain_id;
        result.protocol_status = npm::NpmProtocolStatus::kUnknown;
        result.is_final = true;
        result.end_reason = reason;
        return writer.WriteBasic(result);
    }

 private:
    uint64_t marker_ = 0;
    std::vector<uint64_t>* calls_ = nullptr;
    std::vector<npm::NpmSessionEndReason>* reasons_ = nullptr;
    bool write_result_ = false;
    int error_ = 0;
};

void TestSessionEndModuleNotificationOrderAndErrors() {
    std::vector<npm::NpmSessionSnapshot> ended_sessions(2);
    ended_sessions[0].key.input_namespace = "capture-a";
    ended_sessions[0].key.observation_domain_id = 77;
    ended_sessions[0].session_id = 1;
    ended_sessions[0].end_reason = npm::NpmSessionEndReason::kClosed;
    ended_sessions[1].key.input_namespace = "capture-a";
    ended_sessions[1].key.observation_domain_id = 77;
    ended_sessions[1].session_id = 2;
    ended_sessions[1].end_reason = npm::NpmSessionEndReason::kTupleReuse;

    std::vector<uint64_t> calls;
    std::vector<npm::NpmSessionEndReason> reasons;
    RecordingEndModule first(1, &calls, &reasons, true, 0);
    RecordingEndModule second(2, &calls, &reasons, true, 0);
    std::vector<npm::INpmAnalysisModule*> modules{&first, &second};
    FixtureWriter writer;
    assert(npm::NotifyNpmSessionEnd(ended_sessions, modules, writer) == 0);
    assert((calls == std::vector<uint64_t>{11, 12, 21, 22}));
    assert((reasons == std::vector<npm::NpmSessionEndReason>{npm::NpmSessionEndReason::kClosed,
                                                            npm::NpmSessionEndReason::kClosed,
                                                            npm::NpmSessionEndReason::kTupleReuse,
                                                            npm::NpmSessionEndReason::kTupleReuse}));
    assert(writer.accepted == 4);

    calls.clear();
    reasons.clear();
    RecordingEndModule succeeds(1, &calls, &reasons, false, 0);
    RecordingEndModule fails(2, &calls, &reasons, false, EBUSY);
    RecordingEndModule skipped(3, &calls, &reasons, false, 0);
    modules = {&succeeds, &fails, &skipped};
    assert(npm::NotifyNpmSessionEnd({ended_sessions[0]}, modules, writer) == EBUSY);
    assert((calls == std::vector<uint64_t>{11, 12}));

    calls.clear();
    reasons.clear();
    RecordingEndModule writes(1, &calls, &reasons, true, 0);
    RecordingEndModule after_writer(2, &calls, &reasons, false, 0);
    modules = {&writes, &after_writer};
    writer.next_error = EIO;
    assert(npm::NotifyNpmSessionEnd({ended_sessions[0]}, modules, writer) == EIO);
    assert((calls == std::vector<uint64_t>{11}));
}

}  // namespace

int main() {
    TestConfigDefaultsAndEnumContract();
    TestConfigRangesAndUnsupportedValues();
    TestObservationDomainMapping();
    TestBasicResultNullableContract();
    TestBasicResultSchema();
    TestBudgetAccounting();
    TestBorrowedViewsAndModuleInterfaces();
    TestTimeCapabilityRequirements();
    TestSessionPacketCanonicalizationAndObservationDomains();
    TestSessionPacketPayloadAndInvalidInputs();
    TestSessionPacketFragmentsAndTunnelContext();
    TestSessionTableBidirectionalCountersAndLookup();
    TestSessionTableIsolationCapacityAndErrors();
    TestOfflineEventWatermarkAndIdleRetirement();
    TestIdleDeadlineReplacementLatePacketAndCapacityRelease();
    TestRealtimeProgressRequiresIdleAndBacklogEvidence();
    TestTcpSynRetransmissionAndTupleReuse();
    TestTcpFinRstAndUdpLifecycle();
    TestSessionEndModuleNotificationOrderAndErrors();
    return 0;
}
