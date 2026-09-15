// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <common/iplugin.h>
#include <common/network/netbase.h>
#include <framework/core/packet_codec.h>
#include <framework/interfaces/cpp_operator_plugin_abi.h>
#include <operators/npm_basic/npm_analysis_contract.h>
#include <operators/npm_basic/npm_basic_operator.h>
#include <operators/npm_basic/npm_basic_result_collector.h>
#include <operators/npm_basic/npm_basic_result_encoder.h>
#include <operators/npm_basic/npm_basic_result_projector.h>
#include <operators/npm_basic/npm_basic_task_config.h>
#include <operators/npm_basic/npm_basic_task_runtime.h>
#include <operators/npm_basic/npm_eof_flusher.h>
#include <operators/npm_basic/npm_packet_batch_view.h>
#include <operators/npm_basic/npm_packet_processor.h>
#include <operators/npm_basic/npm_protocol_context.h>
#include <operators/npm_basic/npm_session_key.h>
#include <operators/npm_basic/npm_session_table.h>
#include <operators/npm_basic/npm_task_budget.h>
#include <plugins/npi/iprotocol.h>

#include <arrow/api.h>
#include <arrow/util/byte_size.h>

#include <arpa/inet.h>
#include <dlfcn.h>

#include <array>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace npm = flowsql::npm;

extern "C" flowsql::IPlugin* pluginregist(flowsql::IRegister* registry, const char* option);

namespace {

constexpr uint8_t kTcpFin = 0x01;
constexpr uint8_t kTcpSyn = 0x02;
constexpr uint8_t kTcpRst = 0x04;
constexpr uint8_t kTcpAck = 0x10;

bool SameGuid(const flowsql::Guid& left, const flowsql::Guid& right) {
    return !(left < right) && !(right < left);
}

class NpiInterfaceRegistry : public flowsql::IRegister {
 public:
    void Regist(const flowsql::Guid& iid, void* iface) override {
        if (SameGuid(iid, flowsql::IID_PROTOCOL)) {
            protocol = static_cast<flowsql::IProtocol*>(iface);
        } else if (SameGuid(iid, flowsql::IID_PROTOCOL_PIPELINE_POOL_V1)) {
            pipeline_pool = static_cast<flowsql::IProtocolPipelinePoolV1*>(iface);
        }
    }

    flowsql::IProtocol* protocol = nullptr;
    flowsql::IProtocolPipelinePoolV1* pipeline_pool = nullptr;
};

class SinglePoolQuerier final : public flowsql::IQuerier {
 public:
    explicit SinglePoolQuerier(flowsql::IProtocolPipelinePoolV1* pool) : pool_(pool) {}

    int Traverse(const flowsql::Guid& iid, fntraverse callback) override {
        if (!SameGuid(iid, flowsql::IID_PROTOCOL_PIPELINE_POOL_V1) || pool_ == nullptr) return 0;
        return callback ? callback(pool_) : 0;
    }

    void* First(const flowsql::Guid& iid) override {
        return SameGuid(iid, flowsql::IID_PROTOCOL_PIPELINE_POOL_V1) ? pool_ : nullptr;
    }

 private:
    flowsql::IProtocolPipelinePoolV1* pool_ = nullptr;
};

class ContextDictionary final : public flowsql::protocol::IDictionary {
 public:
    int32_t Count() const override { return 2; }

    const flowsql::protocol::Entry* Query(int32_t number) const override {
        if (number == main_.number) return &main_;
        if (number == sub_.number) return &sub_;
        return &unknown_;
    }

    int32_t Traverse(std::function<int32_t(const flowsql::protocol::Entry*)> traverser) const override {
        if (!traverser) return 0;
        if (traverser(&main_) != 0) return 1;
        traverser(&sub_);
        return 2;
    }

 private:
    const flowsql::protocol::Entry main_{7, 7, "MAIN", "MAIN", "MAIN"};
    const flowsql::protocol::Entry sub_{8, 7, "SUB", "SUB", "SUB"};
    const flowsql::protocol::Entry unknown_{0, 0, "UNKNOWN", "UNKNOWN", "UNKNOWN"};
};

class ContextProtocol final : public flowsql::IProtocol {
 public:
    explicit ContextProtocol(flowsql::protocol::IDictionary* dictionary) : dictionary_(dictionary) {}

    void Concurrency(int32_t) override {}

    flowsql::protocol::Protocol Identify(int32_t pipeno,
                                         const uint8_t*,
                                         int32_t,
                                         const flowsql::protocol::Layers*) override {
        if (identify_entered != nullptr) identify_entered->store(true, std::memory_order_release);
        while (release_identify != nullptr &&
               !release_identify->load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        identify_pipelines.push_back(pipeno);
        return {7, 8};
    }

    int32_t Layer(int32_t,
                  const uint8_t*,
                  int32_t,
                  flowsql::protocol::Layers*) override {
        ++layer_calls;
        return 0;
    }

    flowsql::protocol::IDictionary* Dictionary() override { return dictionary_; }

    int32_t layer_calls = 0;
    std::vector<int32_t> identify_pipelines;
    std::atomic<bool>* identify_entered = nullptr;
    std::atomic<bool>* release_identify = nullptr;

 private:
    flowsql::protocol::IDictionary* dictionary_ = nullptr;
};

class ContextPool final : public flowsql::IProtocolPipelinePoolV1 {
 public:
    explicit ContextPool(flowsql::IProtocol* protocol,
                         flowsql::ProtocolPipelinePoolError acquire_result =
                             flowsql::ProtocolPipelinePoolError::kNone)
        : protocol_(protocol), acquire_result_(acquire_result) {}

    flowsql::ProtocolPipelinePoolError Acquire(int32_t* pipeno) override {
        ++acquire_calls;
        if (pipeno == nullptr) return flowsql::ProtocolPipelinePoolError::kNullOutput;
        if (acquire_result_ != flowsql::ProtocolPipelinePoolError::kNone) return acquire_result_;
        if (leased_) return flowsql::ProtocolPipelinePoolError::kExhausted;
        leased_ = true;
        *pipeno = 3;
        return flowsql::ProtocolPipelinePoolError::kNone;
    }

    flowsql::ProtocolPipelinePoolError Release(int32_t pipeno) override {
        ++release_calls;
        if (pipeno != 3) return flowsql::ProtocolPipelinePoolError::kInvalidPipeline;
        if (!leased_) return flowsql::ProtocolPipelinePoolError::kNotLeased;
        leased_ = false;
        return flowsql::ProtocolPipelinePoolError::kNone;
    }

    int32_t Capacity() const override { return 4; }
    flowsql::IProtocol* Protocol() override { return protocol_; }

    int32_t acquire_calls = 0;
    int32_t release_calls = 0;

 private:
    flowsql::IProtocol* protocol_ = nullptr;
    flowsql::ProtocolPipelinePoolError acquire_result_;
    bool leased_ = false;
};

class DualContextPool final : public flowsql::IProtocolPipelinePoolV1 {
 public:
    explicit DualContextPool(flowsql::IProtocol* protocol) : protocol_(protocol) {}

    flowsql::ProtocolPipelinePoolError Acquire(int32_t* pipeno) override {
        ++acquire_calls;
        if (pipeno == nullptr) return flowsql::ProtocolPipelinePoolError::kNullOutput;
        for (size_t index = 0; index < leased_.size(); ++index) {
            if (leased_[index]) continue;
            leased_[index] = true;
            *pipeno = static_cast<int32_t>(index);
            return flowsql::ProtocolPipelinePoolError::kNone;
        }
        return flowsql::ProtocolPipelinePoolError::kExhausted;
    }

    flowsql::ProtocolPipelinePoolError Release(int32_t pipeno) override {
        ++release_calls;
        if (pipeno < 0 || static_cast<size_t>(pipeno) >= leased_.size()) {
            return flowsql::ProtocolPipelinePoolError::kInvalidPipeline;
        }
        if (!leased_[static_cast<size_t>(pipeno)]) {
            return flowsql::ProtocolPipelinePoolError::kNotLeased;
        }
        leased_[static_cast<size_t>(pipeno)] = false;
        return flowsql::ProtocolPipelinePoolError::kNone;
    }

    int32_t Capacity() const override { return static_cast<int32_t>(leased_.size()); }
    flowsql::IProtocol* Protocol() override { return protocol_; }

    int32_t acquire_calls = 0;
    int32_t release_calls = 0;

 private:
    flowsql::IProtocol* protocol_ = nullptr;
    std::array<bool, 2> leased_{};
};

class CountingProtocolProxy final : public flowsql::IProtocol {
 public:
    explicit CountingProtocolProxy(flowsql::IProtocol* target) : target_(target) {}

    void Concurrency(int32_t number) override { target_->Concurrency(number); }

    flowsql::protocol::Protocol Identify(int32_t pipeno,
                                         const uint8_t* packet,
                                         int32_t packet_size,
                                         const flowsql::protocol::Layers* layers) override {
        identify_pipelines.push_back(pipeno);
        return target_->Identify(pipeno, packet, packet_size, layers);
    }

    int32_t Layer(int32_t pipeno,
                  const uint8_t* packet,
                  int32_t packet_size,
                  flowsql::protocol::Layers* layers) override {
        ++layer_calls;
        return target_->Layer(pipeno, packet, packet_size, layers);
    }

    flowsql::protocol::IDictionary* Dictionary() override { return target_->Dictionary(); }

    int32_t layer_calls = 0;
    std::vector<int32_t> identify_pipelines;

 private:
    flowsql::IProtocol* target_ = nullptr;
};

class DelegatingPipelinePool final : public flowsql::IProtocolPipelinePoolV1 {
 public:
    DelegatingPipelinePool(flowsql::IProtocolPipelinePoolV1* target, flowsql::IProtocol* protocol)
        : target_(target), protocol_(protocol) {}

    flowsql::ProtocolPipelinePoolError Acquire(int32_t* pipeno) override { return target_->Acquire(pipeno); }
    flowsql::ProtocolPipelinePoolError Release(int32_t pipeno) override { return target_->Release(pipeno); }
    int32_t Capacity() const override { return target_->Capacity(); }
    flowsql::IProtocol* Protocol() override { return protocol_; }

 private:
    flowsql::IProtocolPipelinePoolV1* target_ = nullptr;
    flowsql::IProtocol* protocol_ = nullptr;
};

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

npm::NpmPacketView MakeNpmPacketView(const PacketFixture& packet,
                                     const npm::NpmSessionPacketBinding& binding,
                                     uint32_t source_id,
                                     int64_t timestamp_ns,
                                     uint32_t wire_len) {
    npm::NpmPacketView view;
    view.packet = packet.View(source_id, wire_len);
    view.packet.meta.timestamp_ns = timestamp_ns;
    view.layer = &packet.layer;
    view.payload = binding.payload;
    view.direction = binding.direction;
    return view;
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

void TestSessionTableActiveSnapshotIsOrderedAndNonMutating() {
    auto config = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kRealtime);
    config.max_active_sessions = 4;
    const npm::NpmObservationDomainMap domain_map{"capture-a", {{1, 77}}};
    const auto first_packet = MakeIpv4TcpPacket("192.0.2.20", 50020, "192.0.2.2", 443, {});
    const auto second_packet = MakeIpv6UdpPacket("2001:db8::20", 53020, "2001:db8::2", 53, {});
    flowsql::packet::PacketMeta first_meta;
    flowsql::packet::PacketMeta second_meta;
    const auto first = BuildBinding(domain_map, first_packet, 1, 20, 100, &first_meta);
    const auto second = BuildBinding(domain_map, second_packet, 1, 10, 80, &second_meta);

    auto budget = std::make_shared<npm::NpmTaskBudget>(config);
    npm::NpmSessionTable table(config, budget);
    std::vector<npm::NpmSessionView> views(1);
    views[0].session_id = 99;
    assert(table.SnapshotActive(nullptr) == npm::NpmSessionTableError::kNullOutput);
    assert(table.SnapshotActive(&views) == npm::NpmSessionTableError::kNone);
    assert(views.empty());

    npm::NpmSessionView observed;
    assert(ObserveActive(table, first, first_meta, &observed) == npm::NpmSessionTableError::kNone);
    assert(observed.session_id == 1);
    assert(ObserveActive(table, second, second_meta, &observed) == npm::NpmSessionTableError::kNone);
    assert(observed.session_id == 2);
    const auto usage_before = budget->Usage();
    const auto tracked_before = table.tracked_bytes();

    views.assign(1, npm::NpmSessionView{});
    views[0].session_id = 99;
    assert(table.SnapshotActive(&views) == npm::NpmSessionTableError::kNone);
    assert(views.size() == 2);
    assert(views[0].session_id == 1 && views[1].session_id == 2);
    assert(views[0].key != nullptr && SameKey(*views[0].key, first.key));
    assert(views[1].key != nullptr && SameKey(*views[1].key, second.key));
    assert(views[0].protocol_status == npm::NpmProtocolStatus::kPending);
    assert(views[1].protocol_status == npm::NpmProtocolStatus::kPending);
    assert(table.size() == 2 && table.tracked_bytes() == tracked_before);
    auto usage_after = budget->Usage();
    assert(usage_after.session_state_bytes == usage_before.session_state_bytes);
    assert(usage_after.module_state_bytes == usage_before.module_state_bytes);
    assert(usage_after.input_batch_bytes == usage_before.input_batch_bytes);
    assert(usage_after.pending_output_bytes == usage_before.pending_output_bytes);

    std::vector<npm::NpmSessionView> repeated;
    assert(table.SnapshotActive(&repeated) == npm::NpmSessionTableError::kNone);
    assert(repeated.size() == 2);
    assert(repeated[0].session_id == views[0].session_id && repeated[0].key == views[0].key);
    assert(repeated[1].session_id == views[1].session_id && repeated[1].key == views[1].key);
    assert(table.size() == 2 && table.tracked_bytes() == tracked_before);
    usage_after = budget->Usage();
    assert(usage_after.session_state_bytes == usage_before.session_state_bytes);
    assert(usage_after.module_state_bytes == usage_before.module_state_bytes);
    assert(usage_after.input_batch_bytes == usage_before.input_batch_bytes);
    assert(usage_after.pending_output_bytes == usage_before.pending_output_bytes);
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

npm::NpmCaptureProgressUpdate OfflineProgress(int64_t capture_time_ns);
void AssertTaskBudgetUsage(const npm::NpmBudgetUsage& usage,
                           uint64_t session,
                           uint64_t module,
                           uint64_t input,
                           uint64_t output);

void TestSessionTableBudgetReserveFailureAndStableCharge() {
    auto config = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline);
    config.max_active_sessions = 4;
    config.max_tracked_bytes = npm::kNpmMinTrackedBytes;
    const auto packet = MakeIpv6UdpPacket("2001:db8::1", 53000, "2001:db8::2", 53, {});
    const npm::NpmObservationDomainMap short_domain{"a", {{1, 7}}};
    const npm::NpmObservationDomainMap long_domain{std::string(128, 'n'), {{1, 7}}};
    flowsql::packet::PacketMeta short_meta;
    flowsql::packet::PacketMeta long_meta;
    const auto short_binding = BuildBinding(short_domain, packet, 1, 10, 80, &short_meta);
    const auto long_binding = BuildBinding(long_domain, packet, 1, 10, 80, &long_meta);

    auto short_budget = std::make_shared<npm::NpmTaskBudget>(config);
    npm::NpmSessionTable short_table(config, short_budget);
    npm::NpmSessionView observed;
    assert(ObserveActive(short_table, short_binding, short_meta, &observed) == npm::NpmSessionTableError::kNone);
    const uint64_t short_charge = short_table.tracked_bytes();
    assert(short_charge != 0);
    AssertTaskBudgetUsage(short_budget->Usage(), short_charge, 0, 0, 0);

    short_meta.timestamp_ns = 20;
    assert(ObserveActive(short_table, short_binding, short_meta, &observed) == npm::NpmSessionTableError::kNone);
    assert(short_table.tracked_bytes() == short_charge);
    AssertTaskBudgetUsage(short_budget->Usage(), short_charge, 0, 0, 0);

    auto long_budget = std::make_shared<npm::NpmTaskBudget>(config);
    npm::NpmSessionTable long_table(config, long_budget);
    assert(ObserveActive(long_table, long_binding, long_meta, &observed) == npm::NpmSessionTableError::kNone);
    assert(long_table.tracked_bytes() > short_charge);
    AssertTaskBudgetUsage(long_budget->Usage(), long_table.tracked_bytes(), 0, 0, 0);

    auto limited_budget = std::make_shared<npm::NpmTaskBudget>(config);
    assert(limited_budget->Reserve(
               npm::NpmBudgetCategory::kModuleState, config.max_tracked_bytes - short_charge + 1) ==
           npm::NpmBudgetError::kNone);
    npm::NpmSessionTable limited(config, limited_budget);
    npm::NpmSessionObserveResult unchanged;
    unchanged.has_active_session = true;
    unchanged.active_session.session_id = 99;
    unchanged.ended_sessions.emplace_back().session_id = 88;
    assert(limited.Observe(short_binding, short_meta, &unchanged) ==
           npm::NpmSessionTableError::kSessionBudgetExceeded);
    assert(limited.size() == 0 && limited.tracked_bytes() == 0);
    assert(unchanged.has_active_session && unchanged.active_session.session_id == 99);
    assert(unchanged.ended_sessions.size() == 1 && unchanged.ended_sessions[0].session_id == 88);
    AssertTaskBudgetUsage(
        limited_budget->Usage(), 0, config.max_tracked_bytes - short_charge + 1, 0, 0);

    assert(limited_budget->Release(
               npm::NpmBudgetCategory::kModuleState, config.max_tracked_bytes - short_charge + 1) ==
           npm::NpmBudgetError::kNone);
    assert(ObserveActive(limited, short_binding, short_meta, &observed) == npm::NpmSessionTableError::kNone);
    assert(observed.session_id == 1 && limited.tracked_bytes() == short_charge);
    AssertTaskBudgetUsage(limited_budget->Usage(), short_charge, 0, 0, 0);
}

void TestSessionTableBudgetReleasesAllTerminalPaths() {
    auto config = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline);
    config.max_active_sessions = 4;
    config.out_of_order_tolerance_ns = 0;
    config.udp_idle_timeout_ns = npm::kNpmNanosecondsPerSecond;
    const npm::NpmObservationDomainMap domain_map{"capture-a", {{1, 77}}};
    const auto udp_packet = MakeIpv6UdpPacket("2001:db8::1", 53000, "2001:db8::2", 53, {});
    flowsql::packet::PacketMeta udp_meta;
    const auto udp = BuildBinding(domain_map, udp_packet, 1, 0, 80, &udp_meta);

    auto idle_budget = std::make_shared<npm::NpmTaskBudget>(config);
    npm::NpmSessionTable idle_table(config, idle_budget);
    npm::NpmSessionView observed;
    assert(ObserveActive(idle_table, udp, udp_meta, &observed) == npm::NpmSessionTableError::kNone);
    assert(idle_table.tracked_bytes() != 0);
    const auto idle = idle_table.AdvanceCaptureProgress(OfflineProgress(npm::kNpmNanosecondsPerSecond));
    assert(idle.ended_sessions.size() == 1 && idle_table.tracked_bytes() == 0);
    AssertTaskBudgetUsage(idle_budget->Usage(), 0, 0, 0, 0);

    auto eof_budget = std::make_shared<npm::NpmTaskBudget>(config);
    npm::NpmSessionTable eof_table(config, eof_budget);
    assert(ObserveActive(eof_table, udp, udp_meta, &observed) == npm::NpmSessionTableError::kNone);
    std::vector<npm::NpmSessionSnapshot> eof_sessions;
    assert(eof_table.FinishAllAtEof(&eof_sessions) == npm::NpmSessionTableError::kNone);
    assert(eof_sessions.size() == 1 && eof_table.tracked_bytes() == 0);
    AssertTaskBudgetUsage(eof_budget->Usage(), 0, 0, 0, 0);

    const auto rst_packet =
        MakeIpv4TcpPacket("198.51.100.1", 51000, "198.51.100.2", 8443, {}, kTcpRst, 200);
    flowsql::packet::PacketMeta rst_meta;
    const auto rst = BuildBinding(domain_map, rst_packet, 1, 20, 90, &rst_meta);
    auto closed_budget = std::make_shared<npm::NpmTaskBudget>(config);
    npm::NpmSessionTable closed_table(config, closed_budget);
    npm::NpmSessionObserveResult closed;
    assert(closed_table.Observe(rst, rst_meta, &closed) == npm::NpmSessionTableError::kNone);
    assert(closed.ended_sessions.size() == 1 && closed_table.tracked_bytes() == 0);
    AssertTaskBudgetUsage(closed_budget->Usage(), 0, 0, 0, 0);

    auto destructor_budget = std::make_shared<npm::NpmTaskBudget>(config);
    {
        npm::NpmSessionTable table(config, destructor_budget);
        assert(ObserveActive(table, udp, udp_meta, &observed) == npm::NpmSessionTableError::kNone);
        assert(destructor_budget->Usage().session_state_bytes == table.tracked_bytes());
    }
    AssertTaskBudgetUsage(destructor_budget->Usage(), 0, 0, 0, 0);
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

class SequenceProtocolIdentifier final : public flowsql::packet::IPacketProtocolIdentifier {
 public:
    explicit SequenceProtocolIdentifier(std::vector<flowsql::packet::PacketProtocolInfo> results)
        : results_(std::move(results)) {}

    flowsql::packet::PacketProtocolInfo Identify(const flowsql::packet::PacketView& packet,
                                                  const flowsql::packet::PacketLayerInfo& layer) override {
        ++calls;
        last_packet_data = packet.bytes.data;
        last_layer = &layer;
        if (next_result_ < results_.size()) return results_[next_result_++];
        return {flowsql::packet::ProtocolStatus::kUnknown, 0, 0};
    }

    uint32_t calls = 0;
    const uint8_t* last_packet_data = nullptr;
    const flowsql::packet::PacketLayerInfo* last_layer = nullptr;

 private:
    std::vector<flowsql::packet::PacketProtocolInfo> results_;
    size_t next_result_ = 0;
};

void TestProtocolSamplingSkipsEmptyPayloadAndStopsAfterHit() {
    auto config = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline);
    config.payload_sample_packets = 3;
    const npm::NpmObservationDomainMap domain_map{"capture-a", {{1, 77}}};
    const auto empty_packet =
        MakeIpv4TcpPacket("192.0.2.1", 50000, "192.0.2.2", 443, {}, kTcpAck, 100);
    const auto payload_packet =
        MakeIpv4TcpPacket("192.0.2.1", 50000, "192.0.2.2", 443, {1, 2}, kTcpAck, 101);
    const auto rst_packet =
        MakeIpv4TcpPacket("192.0.2.1", 50000, "192.0.2.2", 443, {}, kTcpRst | kTcpAck, 102);

    flowsql::packet::PacketMeta empty_meta;
    flowsql::packet::PacketMeta payload_meta;
    flowsql::packet::PacketMeta rst_meta;
    const auto empty = BuildBinding(domain_map, empty_packet, 1, 10, 80, &empty_meta);
    const auto payload = BuildBinding(domain_map, payload_packet, 1, 20, 90, &payload_meta);
    const auto rst = BuildBinding(domain_map, rst_packet, 1, 30, 70, &rst_meta);
    const auto empty_view = MakeNpmPacketView(empty_packet, empty, 1, 10, 80);
    const auto payload_view = MakeNpmPacketView(payload_packet, payload, 1, 20, 90);

    npm::NpmSessionTable table(config);
    npm::NpmSessionObserveResult observed;
    assert(table.Observe(empty, empty_meta, &observed) == npm::NpmSessionTableError::kNone);
    const uint64_t session_id = observed.active_session.session_id;
    SequenceProtocolIdentifier identifier({{flowsql::packet::ProtocolStatus::kUnknown, 0, 0},
                                           {flowsql::packet::ProtocolStatus::kIdentified, 7, 8}});

    npm::NpmSessionView sampled;
    assert(table.SampleProtocol(empty.key, session_id, empty_view, identifier, &sampled) ==
           npm::NpmSessionTableError::kNone);
    assert(identifier.calls == 0 && sampled.protocol_status == npm::NpmProtocolStatus::kPending);
    assert(!sampled.protocol_id.has_value() && !sampled.protocol_sub_id.has_value());

    auto invalid_view = payload_view;
    invalid_view.layer = nullptr;
    sampled.session_id = 998;
    assert(table.SampleProtocol(payload.key, session_id, invalid_view, identifier, &sampled) ==
           npm::NpmSessionTableError::kInvalidPacketView);
    assert(identifier.calls == 0 && sampled.session_id == 998);
    assert(table.SampleProtocol(payload.key, session_id, payload_view, identifier, nullptr) ==
           npm::NpmSessionTableError::kNullOutput);
    assert(identifier.calls == 0);

    assert(table.SampleProtocol(payload.key, session_id, payload_view, identifier, &sampled) ==
           npm::NpmSessionTableError::kNone);
    assert(identifier.calls == 1 && sampled.protocol_status == npm::NpmProtocolStatus::kPending);
    assert(table.SampleProtocol(payload.key, session_id, payload_view, identifier, &sampled) ==
           npm::NpmSessionTableError::kNone);
    assert(identifier.calls == 2 && sampled.protocol_status == npm::NpmProtocolStatus::kIdentified);
    assert(sampled.protocol_id == 7 && sampled.protocol_sub_id == 8);
    assert(identifier.last_packet_data == payload_packet.bytes.data());
    assert(identifier.last_layer == &payload_packet.layer);

    assert(table.SampleProtocol(payload.key, session_id, payload_view, identifier, &sampled) ==
           npm::NpmSessionTableError::kNone);
    assert(identifier.calls == 2 && sampled.protocol_status == npm::NpmProtocolStatus::kIdentified);
    npm::NpmSessionView found;
    assert(table.Find(payload.key, &found) == npm::NpmSessionTableError::kNone);
    assert(found.protocol_status == npm::NpmProtocolStatus::kIdentified);
    assert(found.protocol_id == 7 && found.protocol_sub_id == 8);

    npm::NpmSessionView unchanged;
    unchanged.session_id = 999;
    assert(table.SampleProtocol(payload.key, session_id + 1, payload_view, identifier, &unchanged) ==
           npm::NpmSessionTableError::kSessionInstanceMismatch);
    assert(unchanged.session_id == 999 && identifier.calls == 2);

    assert(table.Observe(rst, rst_meta, &observed) == npm::NpmSessionTableError::kNone);
    assert(!observed.has_active_session && observed.ended_sessions.size() == 1);
    assert(observed.ended_sessions[0].protocol_status == npm::NpmProtocolStatus::kIdentified);
    assert(observed.ended_sessions[0].protocol_id == 7);
    assert(observed.ended_sessions[0].protocol_sub_id == 8);
    const auto ended_view = observed.ended_sessions[0].View();
    assert(ended_view.protocol_status == npm::NpmProtocolStatus::kIdentified);
    assert(ended_view.protocol_id == 7 && ended_view.protocol_sub_id == 8);

    assert(table.SampleProtocol(payload.key, session_id, payload_view, identifier, &unchanged) ==
           npm::NpmSessionTableError::kNotFound);
    assert(unchanged.session_id == 999 && identifier.calls == 2);
}

void TestProtocolSamplingExhaustionAndSessionInstanceIsolation() {
    auto config = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline);
    config.payload_sample_packets = 2;
    const npm::NpmObservationDomainMap domain_map{"capture-a", {{1, 77}}};
    const auto middle_packet =
        MakeIpv4TcpPacket("192.0.2.1", 50000, "192.0.2.2", 443, {1}, kTcpAck, 100);
    const auto syn_packet =
        MakeIpv4TcpPacket("192.0.2.1", 50000, "192.0.2.2", 443, {2}, kTcpSyn, 200);
    const auto rst_packet =
        MakeIpv4TcpPacket("192.0.2.2", 443, "192.0.2.1", 50000, {}, kTcpRst | kTcpAck, 300);

    flowsql::packet::PacketMeta middle_meta;
    flowsql::packet::PacketMeta syn_meta;
    flowsql::packet::PacketMeta rst_meta;
    const auto middle = BuildBinding(domain_map, middle_packet, 1, 10, 80, &middle_meta);
    const auto syn = BuildBinding(domain_map, syn_packet, 1, 20, 90, &syn_meta);
    const auto rst = BuildBinding(domain_map, rst_packet, 1, 30, 70, &rst_meta);
    const auto middle_view = MakeNpmPacketView(middle_packet, middle, 1, 10, 80);
    const auto syn_view = MakeNpmPacketView(syn_packet, syn, 1, 20, 90);

    npm::NpmSessionTable table(config);
    npm::NpmSessionObserveResult observed;
    assert(table.Observe(middle, middle_meta, &observed) == npm::NpmSessionTableError::kNone);
    const uint64_t old_session_id = observed.active_session.session_id;
    SequenceProtocolIdentifier identifier({});
    npm::NpmSessionView sampled;
    assert(table.SampleProtocol(middle.key, old_session_id, middle_view, identifier, &sampled) ==
           npm::NpmSessionTableError::kNone);
    assert(identifier.calls == 1 && sampled.protocol_status == npm::NpmProtocolStatus::kPending);

    assert(table.Observe(syn, syn_meta, &observed) == npm::NpmSessionTableError::kNone);
    assert(observed.ended_sessions.size() == 1);
    assert(observed.ended_sessions[0].session_id == old_session_id);
    assert(observed.ended_sessions[0].end_reason == npm::NpmSessionEndReason::kTupleReuse);
    assert(observed.ended_sessions[0].protocol_status == npm::NpmProtocolStatus::kUnknown);
    assert(!observed.ended_sessions[0].protocol_id.has_value());
    const uint64_t new_session_id = observed.active_session.session_id;
    assert(new_session_id != old_session_id);
    assert(observed.active_session.protocol_status == npm::NpmProtocolStatus::kPending);

    npm::NpmSessionView unchanged;
    unchanged.session_id = 999;
    assert(table.SampleProtocol(syn.key, old_session_id, syn_view, identifier, &unchanged) ==
           npm::NpmSessionTableError::kSessionInstanceMismatch);
    assert(unchanged.session_id == 999 && identifier.calls == 1);

    assert(table.SampleProtocol(syn.key, new_session_id, syn_view, identifier, &sampled) ==
           npm::NpmSessionTableError::kNone);
    assert(identifier.calls == 2 && sampled.protocol_status == npm::NpmProtocolStatus::kPending);
    assert(table.SampleProtocol(syn.key, new_session_id, syn_view, identifier, &sampled) ==
           npm::NpmSessionTableError::kNone);
    assert(identifier.calls == 3 && sampled.protocol_status == npm::NpmProtocolStatus::kUnknown);
    assert(!sampled.protocol_id.has_value() && !sampled.protocol_sub_id.has_value());
    assert(table.SampleProtocol(syn.key, new_session_id, syn_view, identifier, &sampled) ==
           npm::NpmSessionTableError::kNone);
    assert(identifier.calls == 3 && sampled.protocol_status == npm::NpmProtocolStatus::kUnknown);

    assert(table.Observe(rst, rst_meta, &observed) == npm::NpmSessionTableError::kNone);
    assert(observed.ended_sessions.size() == 1);
    assert(observed.ended_sessions[0].session_id == new_session_id);
    assert(observed.ended_sessions[0].protocol_status == npm::NpmProtocolStatus::kUnknown);
    assert(!observed.ended_sessions[0].protocol_id.has_value());
}

void TestProtocolPendingFinalizesUnknownOnIdle() {
    constexpr int64_t second = npm::kNpmNanosecondsPerSecond;
    auto config = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline);
    config.out_of_order_tolerance_ns = 0;
    const npm::NpmObservationDomainMap domain_map{"capture-a", {{1, 77}}};
    const auto udp_packet = MakeIpv6UdpPacket("2001:db8::1", 53000, "2001:db8::2", 53, {});
    flowsql::packet::PacketMeta meta;
    const auto udp = BuildBinding(domain_map, udp_packet, 1, 10 * second, 80, &meta);

    npm::NpmSessionTable table(config);
    npm::NpmSessionObserveResult observed;
    assert(table.Observe(udp, meta, &observed) == npm::NpmSessionTableError::kNone);
    assert(observed.active_session.protocol_status == npm::NpmProtocolStatus::kPending);
    const auto progress = table.AdvanceCaptureProgress(OfflineProgress(40 * second));
    assert(progress.ended_sessions.size() == 1);
    assert(progress.ended_sessions[0].end_reason == npm::NpmSessionEndReason::kIdleTimeout);
    assert(progress.ended_sessions[0].protocol_status == npm::NpmProtocolStatus::kUnknown);
    assert(!progress.ended_sessions[0].protocol_id.has_value());
    assert(!progress.ended_sessions[0].protocol_sub_id.has_value());

    const auto rst_packet =
        MakeIpv4TcpPacket("192.0.2.1", 50000, "192.0.2.2", 443, {}, kTcpRst | kTcpAck, 100);
    flowsql::packet::PacketMeta rst_meta;
    const auto rst = BuildBinding(domain_map, rst_packet, 1, 50 * second, 90, &rst_meta);
    npm::NpmSessionTable closed_table(config);
    assert(closed_table.Observe(rst, rst_meta, &observed) == npm::NpmSessionTableError::kNone);
    assert(observed.ended_sessions.size() == 1);
    assert(observed.ended_sessions[0].end_reason == npm::NpmSessionEndReason::kClosed);
    assert(observed.ended_sessions[0].protocol_status == npm::NpmProtocolStatus::kUnknown);
    assert(!observed.ended_sessions[0].protocol_id.has_value());
}

void TestSessionTableFinishAllAtEof() {
    auto config = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline);
    config.out_of_order_tolerance_ns = 0;
    const npm::NpmObservationDomainMap domain_map{"capture-a", {{1, 77}}};
    const auto pending_packet =
        MakeIpv4TcpPacket("192.0.2.1", 50000, "192.0.2.2", 443, {}, kTcpAck, 100);
    const auto identified_packet =
        MakeIpv6UdpPacket("2001:db8::1", 53000, "2001:db8::2", 53, {1, 2});
    const auto closed_packet =
        MakeIpv4TcpPacket("198.51.100.1", 51000, "198.51.100.2", 8443, {}, kTcpRst, 200);

    flowsql::packet::PacketMeta pending_meta;
    flowsql::packet::PacketMeta identified_meta;
    flowsql::packet::PacketMeta closed_meta;
    const auto pending = BuildBinding(domain_map, pending_packet, 1, 30, 100, &pending_meta);
    const auto identified = BuildBinding(domain_map, identified_packet, 1, 10, 80, &identified_meta);
    const auto closed = BuildBinding(domain_map, closed_packet, 1, 20, 90, &closed_meta);
    const auto identified_view = MakeNpmPacketView(identified_packet, identified, 1, 10, 80);

    npm::NpmSessionTable table(config);
    npm::NpmSessionObserveResult observed;
    assert(table.Observe(pending, pending_meta, &observed) == npm::NpmSessionTableError::kNone);
    assert(observed.active_session.session_id == 1);
    assert(table.Observe(identified, identified_meta, &observed) == npm::NpmSessionTableError::kNone);
    assert(observed.active_session.session_id == 2);
    SequenceProtocolIdentifier identifier(
        {{flowsql::packet::ProtocolStatus::kIdentified, 7, 8}});
    npm::NpmSessionView sampled;
    assert(table.SampleProtocol(identified.key,
                                observed.active_session.session_id,
                                identified_view,
                                identifier,
                                &sampled) == npm::NpmSessionTableError::kNone);
    assert(sampled.protocol_status == npm::NpmProtocolStatus::kIdentified);

    assert(table.Observe(closed, closed_meta, &observed) == npm::NpmSessionTableError::kNone);
    assert(!observed.has_active_session && observed.ended_sessions.size() == 1);
    assert(observed.ended_sessions[0].session_id == 3);
    assert(observed.ended_sessions[0].end_reason == npm::NpmSessionEndReason::kClosed);
    assert(table.size() == 2);

    assert(table.FinishAllAtEof(nullptr) == npm::NpmSessionTableError::kNullOutput);
    assert(table.size() == 2);

    std::vector<npm::NpmSessionSnapshot> snapshots(1);
    snapshots[0].session_id = 999;
    assert(table.FinishAllAtEof(&snapshots) == npm::NpmSessionTableError::kNone);
    assert(table.size() == 0 && snapshots.size() == 2);
    assert(snapshots[0].session_id == 1 && snapshots[1].session_id == 2);
    assert(snapshots[0].end_reason == npm::NpmSessionEndReason::kEof);
    assert(snapshots[1].end_reason == npm::NpmSessionEndReason::kEof);
    assert(snapshots[0].protocol_status == npm::NpmProtocolStatus::kUnknown);
    assert(!snapshots[0].protocol_id.has_value() && !snapshots[0].protocol_sub_id.has_value());
    assert(snapshots[1].protocol_status == npm::NpmProtocolStatus::kIdentified);
    assert(snapshots[1].protocol_id == 7 && snapshots[1].protocol_sub_id == 8);
    assert(snapshots[0].first_ns == 30 && snapshots[0].last_ns == 30);
    assert(snapshots[0].packets_ab + snapshots[0].packets_ba == 1);
    assert(snapshots[0].wire_bytes_ab + snapshots[0].wire_bytes_ba == 100);
    assert(snapshots[0].key.input_namespace == "capture-a");
    assert(snapshots[0].key.observation_domain_id == 77);
    const auto snapshot_view = snapshots[1].View();
    assert(snapshot_view.key == &snapshots[1].key);
    assert(snapshot_view.protocol_id == 7 && snapshot_view.protocol_sub_id == 8);

    const auto advanced = table.AdvanceCaptureProgress(OfflineProgress(100 * npm::kNpmNanosecondsPerSecond));
    assert(advanced.ended_sessions.empty());
    assert(table.FinishAllAtEof(&snapshots) == npm::NpmSessionTableError::kNone);
    assert(snapshots.empty());
}

class PacketProcessorRecordingModule final : public npm::INpmAnalysisModule {
 public:
    PacketProcessorRecordingModule(uint64_t marker, std::vector<uint64_t>* events)
        : marker_(marker), events_(events) {}

    int OnPacket(const npm::NpmPacketView& packet,
                 const npm::NpmSessionView& session,
                 npm::INpmResultWriter&) override {
        events_->push_back(2000 + session.session_id * 10 + marker_);
        packet_sequences.push_back(packet.packet.meta.sequence);
        packet_timestamps.push_back(packet.packet.meta.timestamp_ns);
        packet_matches_expectation =
            packet.packet.bytes.data == expected_packet_data && packet.layer == expected_layer &&
            packet.payload.size == expected_payload_size &&
            (expected_payload_size == 0 || packet.payload[0] == expected_payload_first) &&
            packet.direction == expected_direction;
        packet_protocol_statuses.push_back(session.protocol_status);
        return packet_error;
    }

    int OnSessionEnd(const npm::NpmSessionView& session,
                     npm::NpmSessionEndReason reason,
                     npm::INpmResultWriter&) override {
        events_->push_back(1000 + session.session_id * 10 + marker_);
        end_reasons.push_back(reason);
        return end_error;
    }

    int packet_error = 0;
    int end_error = 0;
    const uint8_t* expected_packet_data = nullptr;
    const flowsql::packet::PacketLayerInfo* expected_layer = nullptr;
    size_t expected_payload_size = 0;
    uint8_t expected_payload_first = 0;
    npm::NpmPacketDirection expected_direction = npm::NpmPacketDirection::kAToB;
    bool packet_matches_expectation = false;
    std::vector<uint64_t> packet_sequences;
    std::vector<int64_t> packet_timestamps;
    std::vector<npm::NpmProtocolStatus> packet_protocol_statuses;
    std::vector<npm::NpmSessionEndReason> end_reasons;

 private:
    uint64_t marker_ = 0;
    std::vector<uint64_t>* events_ = nullptr;
};

void TestProcessNpmPacketSamplingAndCallbackOrder() {
    auto config = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline);
    const npm::NpmObservationDomainMap domain_map{"capture-a", {{1, 77}}};
    const auto first_packet =
        MakeIpv4TcpPacket("192.0.2.1", 50000, "192.0.2.2", 443, {1}, kTcpAck, 100);
    const auto reuse_packet =
        MakeIpv4TcpPacket("192.0.2.1", 50000, "192.0.2.2", 443, {2}, kTcpSyn, 200);
    const auto close_packet =
        MakeIpv4TcpPacket("192.0.2.2", 443, "192.0.2.1", 50000, {}, kTcpRst | kTcpAck, 300);

    auto first_view = first_packet.View(1, 100);
    first_view.meta.timestamp_ns = 10;
    auto reuse_view = reuse_packet.View(1, 110);
    reuse_view.meta.timestamp_ns = 20;
    auto close_view = close_packet.View(1, 90);
    close_view.meta.timestamp_ns = 30;

    npm::NpmSessionTable table(config);
    SequenceProtocolIdentifier identifier({
        {flowsql::packet::ProtocolStatus::kIdentified, 7, 8},
        {flowsql::packet::ProtocolStatus::kUnknown, 0, 0},
    });
    std::vector<uint64_t> events;
    PacketProcessorRecordingModule first_module(1, &events);
    PacketProcessorRecordingModule second_module(2, &events);
    first_module.expected_packet_data = first_packet.bytes.data();
    first_module.expected_layer = &first_packet.layer;
    first_module.expected_payload_size = 1;
    first_module.expected_payload_first = 1;
    second_module.expected_packet_data = first_packet.bytes.data();
    second_module.expected_layer = &first_packet.layer;
    second_module.expected_payload_size = 1;
    second_module.expected_payload_first = 1;
    const std::vector<npm::INpmAnalysisModule*> modules{&first_module, &second_module};
    FixtureWriter writer;

    std::vector<npm::NpmSessionSnapshot> ended_sessions(1);
    ended_sessions[0].session_id = 999;
    auto status = npm::ProcessNpmPacket(domain_map,
                                        first_view,
                                        first_packet.layer,
                                        table,
                                        identifier,
                                        modules,
                                        writer,
                                        &ended_sessions);
    assert(status.error == npm::NpmPacketProcessError::kNone);
    assert(ended_sessions.empty() && table.size() == 1);
    assert((events == std::vector<uint64_t>{2011, 2012}));
    assert(identifier.calls == 1 && identifier.last_layer == &first_packet.layer);
    assert(first_module.packet_matches_expectation && second_module.packet_matches_expectation);
    assert(first_module.packet_protocol_statuses.back() == npm::NpmProtocolStatus::kIdentified);

    events.clear();
    first_module.expected_packet_data = reuse_packet.bytes.data();
    first_module.expected_layer = &reuse_packet.layer;
    first_module.expected_payload_first = 2;
    second_module.expected_packet_data = reuse_packet.bytes.data();
    second_module.expected_layer = &reuse_packet.layer;
    second_module.expected_payload_first = 2;
    status = npm::ProcessNpmPacket(domain_map,
                                   reuse_view,
                                   reuse_packet.layer,
                                   table,
                                   identifier,
                                   modules,
                                   writer,
                                   &ended_sessions);
    assert(status.error == npm::NpmPacketProcessError::kNone);
    assert(ended_sessions.size() == 1);
    assert(ended_sessions[0].session_id == 1);
    assert(ended_sessions[0].end_reason == npm::NpmSessionEndReason::kTupleReuse);
    assert(ended_sessions[0].protocol_status == npm::NpmProtocolStatus::kIdentified);
    assert((events == std::vector<uint64_t>{1011, 1012, 2021, 2022}));
    assert(identifier.calls == 2 && identifier.last_layer == &reuse_packet.layer);
    assert(first_module.packet_matches_expectation && second_module.packet_matches_expectation);
    assert(first_module.packet_protocol_statuses.back() == npm::NpmProtocolStatus::kPending);
    assert(table.size() == 1);

    events.clear();
    status = npm::ProcessNpmPacket(domain_map,
                                   close_view,
                                   close_packet.layer,
                                   table,
                                   identifier,
                                   modules,
                                   writer,
                                   &ended_sessions);
    assert(status.error == npm::NpmPacketProcessError::kNone);
    assert(ended_sessions.size() == 1);
    assert(ended_sessions[0].session_id == 2);
    assert(ended_sessions[0].end_reason == npm::NpmSessionEndReason::kClosed);
    assert((events == std::vector<uint64_t>{1021, 1022}));
    assert(identifier.calls == 2 && table.size() == 0);
}

void TestProcessNpmPacketErrorsAreStructuredAndAtomic() {
    auto config = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline);
    config.out_of_order_tolerance_ns = 0;
    const npm::NpmObservationDomainMap domain_map{"capture-a", {{1, 77}}};
    const auto packet =
        MakeIpv4TcpPacket("192.0.2.1", 50000, "192.0.2.2", 443, {1}, kTcpAck, 100);
    auto view = packet.View(1, 100);
    view.meta.timestamp_ns = 10;
    SequenceProtocolIdentifier identifier({});
    FixtureWriter writer;
    std::vector<uint64_t> events;
    PacketProcessorRecordingModule first_module(1, &events);
    PacketProcessorRecordingModule second_module(2, &events);
    std::vector<npm::INpmAnalysisModule*> modules{&first_module, &second_module};
    npm::NpmSessionTable table(config);

    auto status = npm::ProcessNpmPacket(
        domain_map, view, packet.layer, table, identifier, modules, writer, nullptr);
    assert(status.error == npm::NpmPacketProcessError::kNullOutput);
    assert(table.size() == 0 && identifier.calls == 0 && events.empty());

    std::vector<npm::NpmSessionSnapshot> ended_sessions(1);
    ended_sessions[0].session_id = 999;
    modules[1] = nullptr;
    status = npm::ProcessNpmPacket(
        domain_map, view, packet.layer, table, identifier, modules, writer, &ended_sessions);
    assert(status.error == npm::NpmPacketProcessError::kNullModule);
    assert(table.size() == 0 && identifier.calls == 0 && events.empty());
    assert(ended_sessions.size() == 1 && ended_sessions[0].session_id == 999);

    modules[1] = &second_module;
    auto unknown_source = view;
    unknown_source.meta.source_id = 2;
    status = npm::ProcessNpmPacket(domain_map,
                                   unknown_source,
                                   packet.layer,
                                   table,
                                   identifier,
                                   modules,
                                   writer,
                                   &ended_sessions);
    assert(status.error == npm::NpmPacketProcessError::kBindingError);
    assert(status.binding_error == npm::NpmSessionPacketError::kUnknownSourceId);
    assert(table.size() == 0 && events.empty());
    assert(ended_sessions.size() == 1 && ended_sessions[0].session_id == 999);

    table.AdvanceCaptureProgress(OfflineProgress(20));
    status = npm::ProcessNpmPacket(
        domain_map, view, packet.layer, table, identifier, modules, writer, &ended_sessions);
    assert(status.error == npm::NpmPacketProcessError::kSessionError);
    assert(status.session_error == npm::NpmSessionTableError::kLatePacket);
    assert(table.size() == 0 && events.empty());
    assert(ended_sessions.size() == 1 && ended_sessions[0].session_id == 999);

    npm::NpmSessionTable callback_table(config);
    first_module.packet_error = EIO;
    status = npm::ProcessNpmPacket(domain_map,
                                   view,
                                   packet.layer,
                                   callback_table,
                                   identifier,
                                   modules,
                                   writer,
                                   &ended_sessions);
    assert(status.error == npm::NpmPacketProcessError::kModuleError);
    assert(status.module_error == EIO);
    assert((events == std::vector<uint64_t>{2011}));
    assert(callback_table.size() == 1);
    assert(ended_sessions.size() == 1 && ended_sessions[0].session_id == 999);

    events.clear();
    first_module.packet_error = 0;
    first_module.end_error = EBUSY;
    const auto rst_packet =
        MakeIpv4TcpPacket("198.51.100.1", 51000, "198.51.100.2", 8443, {}, kTcpRst, 200);
    auto rst_view = rst_packet.View(1, 80);
    rst_view.meta.timestamp_ns = 30;
    npm::NpmSessionTable end_error_table(config);
    status = npm::ProcessNpmPacket(domain_map,
                                   rst_view,
                                   rst_packet.layer,
                                   end_error_table,
                                   identifier,
                                   modules,
                                   writer,
                                   &ended_sessions);
    assert(status.error == npm::NpmPacketProcessError::kModuleError);
    assert(status.module_error == EBUSY);
    assert((events == std::vector<uint64_t>{1011}));
    assert(end_error_table.size() == 0);
    assert(ended_sessions.size() == 1 && ended_sessions[0].session_id == 999);
}

flowsql::packet::PacketRecord MakeBatchPacketRecord(const PacketFixture& fixture,
                                                     uint32_t source_id,
                                                     int64_t timestamp_ns,
                                                     uint64_t sequence) {
    auto owner = std::make_shared<std::vector<uint8_t>>(fixture.bytes);
    flowsql::packet::PacketRecord record;
    record.meta.timestamp_ns = timestamp_ns;
    record.meta.captured_len = static_cast<uint32_t>(owner->size());
    record.meta.wire_len = record.meta.captured_len;
    record.meta.link_type = 1;
    record.meta.source_id = source_id;
    record.meta.sequence = sequence;
    record.raw_data.owner = owner;
    record.raw_data.data = owner->data();
    record.raw_data.size = static_cast<uint32_t>(owner->size());
    record.layer = fixture.layer;
    return record;
}

std::shared_ptr<arrow::RecordBatch> MakeEncodedPacketBatch(
    const std::vector<flowsql::packet::PacketRecord>& records) {
    std::shared_ptr<arrow::RecordBatch> batch;
    std::string error;
    assert(flowsql::packet::EncodePacketBatch(records, &batch, &error) ==
           flowsql::packet::PacketBatchError::kNone);
    assert(error.empty() && batch != nullptr);
    return batch;
}

std::unique_ptr<npm::NpmPacketBatchView> MakeValidatedPacketBatchView(
    const std::vector<flowsql::packet::PacketRecord>& records) {
    auto batch = MakeEncodedPacketBatch(records);

    std::unique_ptr<npm::NpmPacketBatchView> view;
    std::string error;
    assert(npm::NpmPacketBatchView::Create(batch, &view, &error) == npm::NpmPacketBatchError::kNone);
    assert(error.empty() && view != nullptr);
    return view;
}

void TestProcessNpmOfflinePacketBatchOrderAndWatermark() {
    constexpr int64_t second = npm::kNpmNanosecondsPerSecond;
    auto config = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline);
    config.tcp_idle_timeout_ns = second;
    config.udp_idle_timeout_ns = second;
    config.out_of_order_tolerance_ns = 0;
    const npm::NpmObservationDomainMap domain_map{"capture-a", {{1, 77}}};

    const auto idle_packet =
        MakeIpv4TcpPacket("203.0.113.1", 40000, "203.0.113.2", 80, {0x10}, kTcpAck, 100);
    const auto first_packet =
        MakeIpv4TcpPacket("192.0.2.1", 50000, "192.0.2.2", 443, {0x11}, kTcpAck, 200);
    const auto reuse_packet =
        MakeIpv4TcpPacket("192.0.2.1", 50000, "192.0.2.2", 443, {0x12}, kTcpSyn, 201);
    const auto close_packet =
        MakeIpv4TcpPacket("192.0.2.2", 443, "192.0.2.1", 50000, {}, kTcpRst | kTcpAck, 202);
    const auto later_packet =
        MakeIpv4TcpPacket("198.51.100.1", 41000, "198.51.100.2", 8080, {0x13}, kTcpAck, 300);
    auto batch = MakeValidatedPacketBatchView({
        MakeBatchPacketRecord(idle_packet, 1, second, 100),
        MakeBatchPacketRecord(first_packet, 1, 11 * second / 10, 101),
        MakeBatchPacketRecord(reuse_packet, 1, 12 * second / 10, 102),
        MakeBatchPacketRecord(close_packet, 1, 13 * second / 10, 103),
        MakeBatchPacketRecord(later_packet, 1, 3 * second, 104),
    });

    npm::NpmSessionTable table(config);
    SequenceProtocolIdentifier identifier({});
    std::vector<uint64_t> events;
    PacketProcessorRecordingModule first_module(1, &events);
    PacketProcessorRecordingModule second_module(2, &events);
    const std::vector<npm::INpmAnalysisModule*> modules{&first_module, &second_module};
    FixtureWriter writer;
    std::vector<npm::NpmSessionEndEvent> ended_events(1);
    ended_events[0].snapshot.session_id = 999;

    const auto status = npm::ProcessNpmOfflinePacketBatch(
        domain_map, *batch, table, identifier, modules, writer, &ended_events);
    assert(status.error == npm::NpmPacketBatchProcessError::kNone && status.row == -1);
    assert(status.progress_disposition == npm::NpmCaptureProgressDisposition::kAdvanced);
    assert((events == std::vector<uint64_t>{
                          2011, 2012, 2021, 2022, 1021, 1022, 2031,
                          2032, 1031, 1032, 2041, 2042, 1011, 1012}));
    assert((first_module.packet_sequences == std::vector<uint64_t>{100, 101, 102, 104}));
    assert((first_module.packet_timestamps ==
            std::vector<int64_t>{second, 11 * second / 10, 12 * second / 10, 3 * second}));
    assert(identifier.calls == 4 && table.size() == 1);

    assert(ended_events.size() == 3);
    assert(ended_events[0].snapshot.session_id == 2);
    assert(ended_events[0].snapshot.end_reason == npm::NpmSessionEndReason::kTupleReuse);
    assert(ended_events[0].observed_at == 12 * second / 10);
    assert(ended_events[1].snapshot.session_id == 3);
    assert(ended_events[1].snapshot.end_reason == npm::NpmSessionEndReason::kClosed);
    assert(ended_events[1].observed_at == 13 * second / 10);
    assert(ended_events[2].snapshot.session_id == 1);
    assert(ended_events[2].snapshot.end_reason == npm::NpmSessionEndReason::kIdleTimeout);
    assert(ended_events[2].observed_at == 3 * second);

    batch.reset();
    assert(ended_events[0].snapshot.key.input_namespace == "capture-a");
    assert(ended_events[2].snapshot.key.observation_domain_id == 77);
}

void TestProcessNpmOfflinePacketBatchErrorsAndAtomicOutput() {
    auto config = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline);
    config.out_of_order_tolerance_ns = 0;
    const npm::NpmObservationDomainMap domain_map{"capture-a", {{1, 77}}};
    auto empty_batch = MakeValidatedPacketBatchView({});
    npm::NpmSessionTable table(config);
    SequenceProtocolIdentifier identifier({});
    FixtureWriter writer;
    std::vector<uint64_t> events;
    PacketProcessorRecordingModule first_module(1, &events);
    PacketProcessorRecordingModule second_module(2, &events);
    std::vector<npm::INpmAnalysisModule*> modules{&first_module, &second_module};

    auto status = npm::ProcessNpmOfflinePacketBatch(
        domain_map, *empty_batch, table, identifier, modules, writer, nullptr);
    assert(status.error == npm::NpmPacketBatchProcessError::kNullOutput && status.row == -1);
    assert(table.size() == 0 && events.empty());

    std::vector<npm::NpmSessionEndEvent> ended_events(1);
    ended_events[0].snapshot.session_id = 999;
    modules[1] = nullptr;
    status = npm::ProcessNpmOfflinePacketBatch(
        domain_map, *empty_batch, table, identifier, modules, writer, &ended_events);
    assert(status.error == npm::NpmPacketBatchProcessError::kNullModule && status.row == -1);
    assert(ended_events.size() == 1 && ended_events[0].snapshot.session_id == 999);
    modules[1] = &second_module;

    status = npm::ProcessNpmOfflinePacketBatch(
        domain_map, *empty_batch, table, identifier, modules, writer, &ended_events);
    assert(status.error == npm::NpmPacketBatchProcessError::kNone && ended_events.empty());

    const auto packet =
        MakeIpv4TcpPacket("192.0.2.1", 50000, "192.0.2.2", 443, {0x20}, kTcpAck, 400);
    auto binding_error_batch = MakeValidatedPacketBatchView({
        MakeBatchPacketRecord(packet, 1, 10, 200),
        MakeBatchPacketRecord(packet, 2, 20, 201),
        MakeBatchPacketRecord(packet, 1, 30, 202),
    });
    ended_events.emplace_back().snapshot.session_id = 998;
    status = npm::ProcessNpmOfflinePacketBatch(
        domain_map, *binding_error_batch, table, identifier, modules, writer, &ended_events);
    assert(status.error == npm::NpmPacketBatchProcessError::kPacketError && status.row == 1);
    assert(status.packet_status.error == npm::NpmPacketProcessError::kBindingError);
    assert(status.packet_status.binding_error == npm::NpmSessionPacketError::kUnknownSourceId);
    assert(ended_events.size() == 1 && ended_events[0].snapshot.session_id == 998);
    assert((events == std::vector<uint64_t>{2011, 2012}));
    assert(table.size() == 1);

    events.clear();
    auto realtime_config = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kRealtime);
    npm::NpmSessionTable realtime_table(realtime_config);
    auto realtime_batch = MakeValidatedPacketBatchView({MakeBatchPacketRecord(packet, 1, 40, 300)});
    status = npm::ProcessNpmOfflinePacketBatch(
        domain_map, *realtime_batch, realtime_table, identifier, modules, writer, &ended_events);
    assert(status.error == npm::NpmPacketBatchProcessError::kProgressDeferred && status.row == 0);
    assert(status.progress_disposition == npm::NpmCaptureProgressDisposition::kDeferredBacklogUnknown);
    assert(ended_events.size() == 1 && ended_events[0].snapshot.session_id == 998);
    assert((events == std::vector<uint64_t>{2011, 2012}));

    constexpr int64_t second = npm::kNpmNanosecondsPerSecond;
    auto idle_config = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline);
    idle_config.tcp_idle_timeout_ns = second;
    idle_config.out_of_order_tolerance_ns = 0;
    const auto later_packet =
        MakeIpv4TcpPacket("198.51.100.1", 41000, "198.51.100.2", 8080, {0x21}, kTcpAck, 500);
    auto idle_error_batch = MakeValidatedPacketBatchView({
        MakeBatchPacketRecord(packet, 1, second, 400),
        MakeBatchPacketRecord(later_packet, 1, 3 * second, 401),
    });
    npm::NpmSessionTable idle_error_table(idle_config);
    events.clear();
    first_module.end_error = EBUSY;
    status = npm::ProcessNpmOfflinePacketBatch(
        domain_map, *idle_error_batch, idle_error_table, identifier, modules, writer, &ended_events);
    assert(status.error == npm::NpmPacketBatchProcessError::kModuleError && status.row == 1);
    assert(status.module_error == EBUSY);
    assert(ended_events.size() == 1 && ended_events[0].snapshot.session_id == 998);
    assert((events == std::vector<uint64_t>{2011, 2012, 2021, 2022, 1011}));
    assert(idle_error_table.size() == 1);
}

void TestNpmProtocolContextErrorsDictionaryAndRaii() {
    static_assert(!std::is_copy_constructible<npm::NpmProtocolContext>::value);
    static_assert(!std::is_copy_assignable<npm::NpmProtocolContext>::value);
    static_assert(!std::is_move_constructible<npm::NpmProtocolContext>::value);
    static_assert(!std::is_move_assignable<npm::NpmProtocolContext>::value);

    std::unique_ptr<npm::NpmProtocolContext> context;
    SinglePoolQuerier missing_querier(nullptr);
    assert(npm::NpmProtocolContext::Create(nullptr, &context) == npm::NpmProtocolContextError::kNullQuerier);
    assert(npm::NpmProtocolContext::Create(&missing_querier, nullptr) ==
           npm::NpmProtocolContextError::kNullOutput);
    assert(npm::NpmProtocolContext::Create(&missing_querier, &context) ==
           npm::NpmProtocolContextError::kProviderNotFound);
    assert(context == nullptr);

    ContextPool null_protocol_pool(nullptr);
    SinglePoolQuerier null_protocol_querier(&null_protocol_pool);
    assert(npm::NpmProtocolContext::Create(&null_protocol_querier, &context) ==
           npm::NpmProtocolContextError::kProtocolUnavailable);
    assert(null_protocol_pool.acquire_calls == 0 && context == nullptr);

    ContextProtocol null_dictionary_protocol(nullptr);
    ContextPool null_dictionary_pool(&null_dictionary_protocol);
    SinglePoolQuerier null_dictionary_querier(&null_dictionary_pool);
    assert(npm::NpmProtocolContext::Create(&null_dictionary_querier, &context) ==
           npm::NpmProtocolContextError::kDictionaryUnavailable);
    assert(null_dictionary_pool.acquire_calls == 0 && context == nullptr);

    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool unavailable_pool(&protocol, flowsql::ProtocolPipelinePoolError::kUnavailable);
    SinglePoolQuerier unavailable_querier(&unavailable_pool);
    assert(npm::NpmProtocolContext::Create(&unavailable_querier, &context) ==
           npm::NpmProtocolContextError::kPipelineUnavailable);
    assert(context == nullptr);

    ContextPool exhausted_pool(&protocol, flowsql::ProtocolPipelinePoolError::kExhausted);
    SinglePoolQuerier exhausted_querier(&exhausted_pool);
    assert(npm::NpmProtocolContext::Create(&exhausted_querier, &context) ==
           npm::NpmProtocolContextError::kPipelineExhausted);
    assert(context == nullptr);

    ContextPool failed_pool(&protocol, flowsql::ProtocolPipelinePoolError::kInvalidPipeline);
    SinglePoolQuerier failed_querier(&failed_pool);
    assert(npm::NpmProtocolContext::Create(&failed_querier, &context) ==
           npm::NpmProtocolContextError::kPipelineAcquireFailed);
    assert(context == nullptr);

    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    assert(npm::NpmProtocolContext::Create(&querier, &context) == npm::NpmProtocolContextError::kNone);
    assert(context != nullptr && context->Identifier() != nullptr);
    assert(context->Dictionary() == &dictionary && context->Pipeno() == 3);
    assert(std::string(context->ResolveProtocolName(7, 8)) == "SUB");
    assert(std::string(context->ResolveProtocolName(7, 9)) == "MAIN");
    assert(context->ResolveProtocolName(9, 0) == nullptr);
    assert(context->ResolveProtocolName(0, 0) == nullptr);

    npm::NpmProtocolContext* original = context.get();
    assert(npm::NpmProtocolContext::Create(&missing_querier, &context) ==
           npm::NpmProtocolContextError::kProviderNotFound);
    assert(context.get() == original && pool.release_calls == 0);
    context.reset();
    assert(pool.release_calls == 1);
    assert(npm::NpmProtocolContext::Create(&querier, &context) == npm::NpmProtocolContextError::kNone);
    assert(context->Pipeno() == 3);
    context.reset();
    assert(pool.release_calls == 2);
}

void TestNpmProtocolContextRealNpi(flowsql::IProtocolPipelinePoolV1* real_pool,
                                   flowsql::IProtocol* real_protocol) {
    CountingProtocolProxy protocol(real_protocol);
    DelegatingPipelinePool pool(real_pool, &protocol);
    SinglePoolQuerier querier(&pool);

    std::array<std::unique_ptr<npm::NpmProtocolContext>, 2> contexts;
    std::array<npm::NpmProtocolContextError, 2> results;
    std::array<std::thread, 2> workers;
    std::atomic<int32_t> ready{0};
    std::atomic<bool> start{false};
    for (size_t index = 0; index < workers.size(); ++index) {
        workers[index] = std::thread([&, index]() {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            results[index] = npm::NpmProtocolContext::Create(&querier, &contexts[index]);
        });
    }
    while (ready.load(std::memory_order_acquire) != static_cast<int32_t>(workers.size())) {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);
    for (auto& worker : workers) worker.join();

    assert(results[0] == npm::NpmProtocolContextError::kNone);
    assert(results[1] == npm::NpmProtocolContextError::kNone);
    assert(contexts[0] != nullptr && contexts[1] != nullptr);
    assert(contexts[0]->Pipeno() != contexts[1]->Pipeno());
    assert(contexts[0]->Dictionary() == real_protocol->Dictionary());
    assert(contexts[1]->Dictionary() == real_protocol->Dictionary());

    const std::string request = "GET / HTTP/1.1\r\n";
    const std::vector<uint8_t> payload(request.begin(), request.end());
    const auto packet = MakeIpv4TcpPacket("192.0.2.1", 50000, "192.0.2.2", 8080, payload, kTcpAck, 100);
    const auto packet_view = packet.View(1);

    auto config = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline);
    config.payload_sample_packets = 1;
    const npm::NpmObservationDomainMap domain_map{"capture-a", {{1, 77}}};
    flowsql::packet::PacketMeta meta;
    const auto binding = BuildBinding(domain_map, packet, 1, 10, packet.bytes.size(), &meta);
    const auto npm_packet = MakeNpmPacketView(packet, binding, 1, 10, packet.bytes.size());
    npm::NpmSessionTable table(config);
    npm::NpmSessionObserveResult observed;
    assert(table.Observe(binding, meta, &observed) == npm::NpmSessionTableError::kNone);
    npm::NpmSessionView sampled;
    assert(table.SampleProtocol(binding.key,
                                observed.active_session.session_id,
                                npm_packet,
                                *contexts[0]->Identifier(),
                                &sampled) == npm::NpmSessionTableError::kNone);

    const auto second = contexts[1]->Identifier()->Identify(packet_view, packet.layer);
    assert(sampled.protocol_status == npm::NpmProtocolStatus::kIdentified);
    assert(second.status == flowsql::packet::ProtocolStatus::kIdentified);
    assert(sampled.protocol_id == 2000 && sampled.protocol_sub_id == 2000);
    assert(second.id == 2000 && second.sub_id == 2000);
    assert(std::string(contexts[0]->ResolveProtocolName(*sampled.protocol_id, *sampled.protocol_sub_id)) == "HTTP");
    assert(std::string(contexts[1]->ResolveProtocolName(second.id, second.sub_id)) == "HTTP");
    assert(contexts[0]->ResolveProtocolName(0, 0) == nullptr);
    assert(protocol.layer_calls == 0);
    assert(protocol.identify_pipelines.size() == 2);
    assert(protocol.identify_pipelines[0] == contexts[0]->Pipeno());
    assert(protocol.identify_pipelines[1] == contexts[1]->Pipeno());

    std::unique_ptr<npm::NpmProtocolContext> exhausted;
    assert(npm::NpmProtocolContext::Create(&querier, &exhausted) ==
           npm::NpmProtocolContextError::kPipelineExhausted);
    assert(exhausted == nullptr);

    const int32_t released_pipeline = contexts[0]->Pipeno();
    contexts[0].reset();
    assert(npm::NpmProtocolContext::Create(&querier, &contexts[0]) == npm::NpmProtocolContextError::kNone);
    assert(contexts[0]->Pipeno() == released_pipeline);
    contexts[0].reset();
    contexts[1].reset();
}

void TestNpiPipelinePoolOptionAndLeaseContract() {
    const char* invalid_options[] = {
        "{\"concurrency\":0}",
        "{\"concurrency\":-1}",
        "{\"concurrency\":17}",
        "{\"concurrency\":\"2\"}",
        "{\"concurrency\":1.5}",
    };
    for (const char* option : invalid_options) {
        NpiInterfaceRegistry invalid_registry;
        assert(pluginregist(&invalid_registry, option) == nullptr);
    }

    NpiInterfaceRegistry default_registry;
    flowsql::IPlugin* plugin = pluginregist(&default_registry, "{}");
    assert(plugin != nullptr);
    assert(default_registry.protocol != nullptr && default_registry.pipeline_pool != nullptr);
    assert(default_registry.pipeline_pool->Capacity() == 1);
    int32_t unavailable_output = 73;
    assert(default_registry.pipeline_pool->Acquire(&unavailable_output) ==
           flowsql::ProtocolPipelinePoolError::kUnavailable);
    assert(unavailable_output == 73);

    NpiInterfaceRegistry max_registry;
    flowsql::IPlugin* max_plugin = pluginregist(&max_registry, "{\"concurrency\":16}");
    assert(max_plugin == plugin);
    assert(max_registry.pipeline_pool != nullptr);
    assert(max_registry.pipeline_pool->Capacity() == flowsql::kProtocolPipelineMaxCapacityV1);

    const std::string option = std::string("{\"ldfile\":\"") + FLOWSQL_NPI_PROTOCOLS_PATH +
                               "\",\"concurrency\":2}";
    NpiInterfaceRegistry registry;
    flowsql::IPlugin* configured_plugin = pluginregist(&registry, option.c_str());
    assert(configured_plugin == plugin);
    assert(registry.protocol != nullptr && registry.pipeline_pool != nullptr);
    assert(registry.pipeline_pool->Protocol() == registry.protocol);
    assert(registry.pipeline_pool->Capacity() == 2);

    unavailable_output = 74;
    assert(registry.pipeline_pool->Acquire(&unavailable_output) == flowsql::ProtocolPipelinePoolError::kUnavailable);
    assert(unavailable_output == 74);
    assert(plugin->Load(nullptr) == 0);
    assert(registry.pipeline_pool->Acquire(nullptr) == flowsql::ProtocolPipelinePoolError::kNullOutput);

    int32_t first = -1;
    int32_t second = -1;
    assert(registry.pipeline_pool->Acquire(&first) == flowsql::ProtocolPipelinePoolError::kNone);
    assert(registry.pipeline_pool->Acquire(&second) == flowsql::ProtocolPipelinePoolError::kNone);
    assert(first >= 0 && first < registry.pipeline_pool->Capacity());
    assert(second >= 0 && second < registry.pipeline_pool->Capacity());
    assert(first != second);

    int32_t exhausted_output = 75;
    assert(registry.pipeline_pool->Acquire(&exhausted_output) == flowsql::ProtocolPipelinePoolError::kExhausted);
    assert(exhausted_output == 75);
    assert(registry.pipeline_pool->Release(-1) == flowsql::ProtocolPipelinePoolError::kInvalidPipeline);
    assert(registry.pipeline_pool->Release(registry.pipeline_pool->Capacity()) ==
           flowsql::ProtocolPipelinePoolError::kInvalidPipeline);
    assert(registry.pipeline_pool->Release(first) == flowsql::ProtocolPipelinePoolError::kNone);
    assert(registry.pipeline_pool->Release(first) == flowsql::ProtocolPipelinePoolError::kNotLeased);
    int32_t reused = -1;
    assert(registry.pipeline_pool->Acquire(&reused) == flowsql::ProtocolPipelinePoolError::kNone);
    assert(reused == first);
    assert(registry.pipeline_pool->Release(reused) == flowsql::ProtocolPipelinePoolError::kNone);
    assert(registry.pipeline_pool->Release(second) == flowsql::ProtocolPipelinePoolError::kNone);

    registry.protocol->Concurrency(flowsql::kProtocolPipelineMaxCapacityV1);
    assert(registry.pipeline_pool->Capacity() == 2);

    std::atomic<int32_t> ready{0};
    std::atomic<bool> start{false};
    std::array<int32_t, 2> concurrent_pipelines{{-1, -1}};
    std::array<flowsql::ProtocolPipelinePoolError, 2> concurrent_results;
    std::array<std::thread, 2> workers;
    for (size_t index = 0; index < workers.size(); ++index) {
        workers[index] = std::thread([&, index]() {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            concurrent_results[index] = registry.pipeline_pool->Acquire(&concurrent_pipelines[index]);
        });
    }
    while (ready.load(std::memory_order_acquire) != static_cast<int32_t>(workers.size())) {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);
    for (auto& worker : workers) worker.join();
    assert(concurrent_results[0] == flowsql::ProtocolPipelinePoolError::kNone);
    assert(concurrent_results[1] == flowsql::ProtocolPipelinePoolError::kNone);
    assert(concurrent_pipelines[0] != concurrent_pipelines[1]);
    assert(registry.pipeline_pool->Release(concurrent_pipelines[0]) == flowsql::ProtocolPipelinePoolError::kNone);
    assert(registry.pipeline_pool->Release(concurrent_pipelines[1]) == flowsql::ProtocolPipelinePoolError::kNone);

    TestNpmProtocolContextRealNpi(registry.pipeline_pool, registry.protocol);

    assert(plugin->Unload() == 0);
    unavailable_output = 76;
    assert(registry.pipeline_pool->Acquire(&unavailable_output) == flowsql::ProtocolPipelinePoolError::kUnavailable);
    assert(unavailable_output == 76);
}

template <typename Builder, typename Value>
std::shared_ptr<arrow::Array> MakeOneValueArray(Value value) {
    Builder builder;
    assert(builder.Append(value).ok());
    std::shared_ptr<arrow::Array> output;
    assert(builder.Finish(&output).ok());
    return output;
}

template <typename Builder>
std::shared_ptr<arrow::Array> MakeOneNullArray() {
    Builder builder;
    assert(builder.AppendNull().ok());
    std::shared_ptr<arrow::Array> output;
    assert(builder.Finish(&output).ok());
    return output;
}

std::shared_ptr<arrow::Array> MakeOneBinaryArray(const std::vector<uint8_t>& bytes) {
    arrow::BinaryBuilder builder;
    assert(builder.Append(bytes.data(), static_cast<int32_t>(bytes.size())).ok());
    std::shared_ptr<arrow::Array> output;
    assert(builder.Finish(&output).ok());
    return output;
}

template <typename Builder, typename Value>
std::shared_ptr<arrow::Array> MakeFixedListArray(const std::array<Value, flowsql::packet::kMaxLayerDepth>& values,
                                                 bool null_first_value = false) {
    auto value_builder = std::make_shared<Builder>();
    arrow::FixedSizeListBuilder builder(
        arrow::default_memory_pool(), value_builder, flowsql::packet::kMaxLayerDepth);
    assert(builder.Append().ok());
    for (size_t index = 0; index < values.size(); ++index) {
        if (null_first_value && index == 0) {
            assert(value_builder->AppendNull().ok());
        } else {
            assert(value_builder->Append(values[index]).ok());
        }
    }
    std::shared_ptr<arrow::Array> output;
    assert(builder.Finish(&output).ok());
    return output;
}

std::shared_ptr<arrow::RecordBatch> ReplacePacketBatchColumns(
    const std::shared_ptr<arrow::RecordBatch>& batch,
    const std::vector<std::pair<int, std::shared_ptr<arrow::Array>>>& replacements) {
    std::vector<std::shared_ptr<arrow::Array>> columns;
    columns.reserve(batch->num_columns());
    for (int index = 0; index < batch->num_columns(); ++index) columns.push_back(batch->column(index));
    for (const auto& replacement : replacements) columns.at(replacement.first) = replacement.second;
    return arrow::RecordBatch::Make(batch->schema(), batch->num_rows(), std::move(columns));
}

std::shared_ptr<arrow::RecordBatch> MakeNpmPacketViewBatch() {
    const auto fixture = MakeIpv4TcpPacket(
        "192.0.2.1", 41000, "198.51.100.2", 443, std::vector<uint8_t>{0x11, 0x22, 0x33}, kTcpAck, 91);
    auto bytes = std::make_shared<std::vector<uint8_t>>(fixture.bytes);
    const auto ipv6_fixture = MakeIpv6UdpPacket(
        "2001:db8::1", 53000, "2001:db8::2", 53, std::vector<uint8_t>{0x44, 0x55});
    auto ipv6_bytes = std::make_shared<std::vector<uint8_t>>(ipv6_fixture.bytes);

    flowsql::packet::PacketRecord first;
    first.meta.timestamp_ns = 123456789;
    first.meta.captured_len = static_cast<uint32_t>(bytes->size());
    first.meta.wire_len = first.meta.captured_len + 8;
    first.meta.link_type = 1;
    first.meta.source_id = 7;
    first.meta.sequence = 19;
    first.raw_data.owner = bytes;
    first.raw_data.data = bytes->data();
    first.raw_data.size = static_cast<uint32_t>(bytes->size());
    first.layer = fixture.layer;
    first.layer.endpoint_scope = flowsql::packet::EndpointScope::kInnermost;
    first.layer.src_mac.valid = 1;
    first.layer.src_mac.value.bytes[0] = 0x10;
    first.layer.src_mac.value.bytes[5] = 0x15;
    first.protocol.status = flowsql::packet::ProtocolStatus::kIdentified;
    first.protocol.id = 2000;
    first.protocol.sub_id = 2001;

    flowsql::packet::PacketRecord second;
    second.meta.timestamp_ns = 123456790;
    second.meta.captured_len = static_cast<uint32_t>(ipv6_bytes->size());
    second.meta.wire_len = second.meta.captured_len;
    second.meta.link_type = 1;
    second.meta.source_id = 8;
    second.meta.sequence = 20;
    second.raw_data.owner = ipv6_bytes;
    second.raw_data.data = ipv6_bytes->data();
    second.raw_data.size = static_cast<uint32_t>(ipv6_bytes->size());
    second.layer = ipv6_fixture.layer;

    flowsql::packet::PacketRecord third;
    third.meta.timestamp_ns = 123456791;
    third.meta.link_type = 1;
    third.meta.source_id = 9;
    third.meta.sequence = 21;

    std::shared_ptr<arrow::RecordBatch> batch;
    std::string error;
    assert(flowsql::packet::EncodePacketBatch({first, second, third}, &batch, &error) ==
           flowsql::packet::PacketBatchError::kNone);
    assert(error.empty() && batch != nullptr);
    return batch;
}

void AssertPacketBatchCreateError(const std::shared_ptr<arrow::RecordBatch>& batch,
                                  npm::NpmPacketBatchError expected,
                                  const char* error_fragment,
                                  std::unique_ptr<npm::NpmPacketBatchView>* output) {
    auto* original = output->get();
    std::string error = "unchanged";
    assert(npm::NpmPacketBatchView::Create(batch, output, &error) == expected);
    assert(output->get() == original);
    assert(error.find(error_fragment) != std::string::npos);
}

void TestNpmPacketBatchViewBorrowedDecodeAndOwnership() {
    static_assert(!std::is_copy_constructible_v<npm::NpmPacketBatchView>);
    static_assert(!std::is_copy_assignable_v<npm::NpmPacketBatchView>);
    static_assert(!std::is_move_constructible_v<npm::NpmPacketBatchView>);
    static_assert(!std::is_move_assignable_v<npm::NpmPacketBatchView>);

    auto batch = MakeNpmPacketViewBatch();
    std::weak_ptr<arrow::RecordBatch> batch_owner = batch;
    auto raw = std::static_pointer_cast<arrow::BinaryArray>(batch->column(6));
    int32_t expected_size = 0;
    const uint8_t* expected_raw = raw->GetValue(0, &expected_size);
    assert(expected_raw != nullptr && expected_size > 0);

    std::unique_ptr<npm::NpmPacketBatchView> view;
    std::string error = "stale";
    assert(npm::NpmPacketBatchView::Create(batch, &view, &error) == npm::NpmPacketBatchError::kNone);
    assert(error.empty() && view != nullptr && view->num_rows() == 3);
    raw.reset();
    batch.reset();
    assert(!batch_owner.expired());

    flowsql::packet::PacketView packet_view;
    flowsql::packet::PacketLayerInfo layer_info;
    assert(view->Get(0, &packet_view, &layer_info) == npm::NpmPacketBatchError::kNone);
    assert(packet_view.meta.timestamp_ns == 123456789);
    assert(packet_view.meta.captured_len == static_cast<uint32_t>(expected_size));
    assert(packet_view.meta.wire_len == packet_view.meta.captured_len + 8);
    assert(packet_view.meta.link_type == 1 && packet_view.meta.source_id == 7);
    assert(packet_view.meta.sequence == 19);
    assert(packet_view.bytes.data == expected_raw);
    assert(packet_view.bytes.size == static_cast<size_t>(expected_size));
    assert(layer_info.status == flowsql::packet::LayerStatus::kDecoded);
    assert(layer_info.layer_count == 2);
    assert(layer_info.layers[0].kind == static_cast<uint16_t>(flowsql::eLayer::IPv4));
    assert(layer_info.layers[1].kind == static_cast<uint16_t>(flowsql::eLayer::TCP));
    assert(layer_info.network_layer_index == 0 && layer_info.transport_layer_index == 1);
    assert(layer_info.endpoint_scope == flowsql::packet::EndpointScope::kInnermost);
    assert(layer_info.payload_offset == sizeof(flowsql::Ipv4Header) + sizeof(flowsql::TcpHeader));
    assert(layer_info.src_mac.valid == 1 && layer_info.src_mac.value.bytes[0] == 0x10);
    assert(layer_info.dst_mac.valid == 0);
    assert(std::holds_alternative<flowsql::packet::IPv4Address>(layer_info.src_ip));
    assert(std::holds_alternative<flowsql::packet::IPv4Address>(layer_info.dst_ip));
    assert(layer_info.transport_protocol == flowsql::ipv4::eNext::TCP);
    assert(layer_info.ports_valid == 1 && layer_info.src_port == 41000 && layer_info.dst_port == 443);

    const uint8_t original_byte = packet_view.bytes.data[0];
    auto* mutable_raw = const_cast<uint8_t*>(expected_raw);
    mutable_raw[0] ^= 0xff;
    flowsql::packet::PacketView borrowed_again;
    flowsql::packet::PacketLayerInfo layer_again;
    assert(view->Get(0, &borrowed_again, &layer_again) == npm::NpmPacketBatchError::kNone);
    assert(borrowed_again.bytes.data == expected_raw && borrowed_again.bytes.data[0] == mutable_raw[0]);
    mutable_raw[0] = original_byte;

    assert(view->Get(1, &borrowed_again, &layer_again) == npm::NpmPacketBatchError::kNone);
    assert(borrowed_again.meta.source_id == 8 && borrowed_again.meta.sequence == 20);
    assert(std::holds_alternative<flowsql::packet::IPv6Address>(layer_again.src_ip));
    assert(std::holds_alternative<flowsql::packet::IPv6Address>(layer_again.dst_ip));
    assert(SameIp(layer_again.src_ip, flowsql::packet::IpAddress{ParseIpv6("2001:db8::1")}));
    assert(SameIp(layer_again.dst_ip, flowsql::packet::IpAddress{ParseIpv6("2001:db8::2")}));
    assert(layer_again.transport_protocol == flowsql::ipv6::eNext::UDP);
    assert(layer_again.ports_valid == 1 && layer_again.src_port == 53000 && layer_again.dst_port == 53);

    assert(view->Get(2, &borrowed_again, &layer_again) == npm::NpmPacketBatchError::kNone);
    assert(borrowed_again.meta.captured_len == 0 && borrowed_again.bytes.size == 0);
    assert(layer_again.status == flowsql::packet::LayerStatus::kNotDecoded);
    assert(layer_again.layer_count == 0 && layer_again.network_layer_index == flowsql::packet::kNoLayerIndex);
    assert(layer_again.transport_layer_index == flowsql::packet::kNoLayerIndex);
    assert(std::holds_alternative<std::monostate>(layer_again.src_ip));
    assert(std::holds_alternative<std::monostate>(layer_again.dst_ip));
    assert(layer_again.transport_protocol == 0 && layer_again.ports_valid == 0);

    flowsql::packet::PacketView unchanged_packet;
    unchanged_packet.meta.source_id = 99;
    flowsql::packet::PacketLayerInfo unchanged_layer;
    unchanged_layer.payload_offset = 99;
    assert(view->Get(-1, &unchanged_packet, &unchanged_layer) == npm::NpmPacketBatchError::kInvalidRow);
    assert(unchanged_packet.meta.source_id == 99 && unchanged_layer.payload_offset == 99);
    assert(view->Get(3, &unchanged_packet, &unchanged_layer) == npm::NpmPacketBatchError::kInvalidRow);
    assert(unchanged_packet.meta.source_id == 99 && unchanged_layer.payload_offset == 99);
    assert(view->Get(0, nullptr, &unchanged_layer) == npm::NpmPacketBatchError::kNullOutput);
    assert(view->Get(0, &unchanged_packet, nullptr) == npm::NpmPacketBatchError::kNullOutput);
    assert(unchanged_packet.meta.source_id == 99 && unchanged_layer.payload_offset == 99);

    view.reset();
    assert(batch_owner.expired());

    auto sliced_batch = MakeNpmPacketViewBatch()->Slice(1, 1);
    std::unique_ptr<npm::NpmPacketBatchView> sliced_view;
    assert(npm::NpmPacketBatchView::Create(sliced_batch, &sliced_view) == npm::NpmPacketBatchError::kNone);
    assert(sliced_view->Get(0, &borrowed_again, &layer_again) == npm::NpmPacketBatchError::kNone);
    assert(borrowed_again.meta.source_id == 8);
    assert(layer_again.layers[0].kind == static_cast<uint16_t>(flowsql::eLayer::IPv6));
    assert(layer_again.layers[1].kind == static_cast<uint16_t>(flowsql::eLayer::UDP));
}

void TestNpmPacketBatchViewRejectsSchemaAndColumnErrors() {
    auto batch = MakeNpmPacketViewBatch()->Slice(0, 1);
    std::unique_ptr<npm::NpmPacketBatchView> output;
    assert(npm::NpmPacketBatchView::Create(batch, &output) == npm::NpmPacketBatchError::kNone);

    std::string error;
    assert(npm::NpmPacketBatchView::Create(nullptr, &output, &error) == npm::NpmPacketBatchError::kNullInput);
    assert(!error.empty());
    assert(npm::NpmPacketBatchView::Create(batch, nullptr, &error) == npm::NpmPacketBatchError::kNullOutput);
    assert(!error.empty());

    std::vector<std::shared_ptr<arrow::Array>> columns;
    for (int index = 0; index < batch->num_columns(); ++index) columns.push_back(batch->column(index));
    auto no_metadata_schema = arrow::schema(flowsql::packet::PacketSchema()->fields());
    auto schema_mismatch = arrow::RecordBatch::Make(no_metadata_schema, batch->num_rows(), columns);
    AssertPacketBatchCreateError(
        schema_mismatch, npm::NpmPacketBatchError::kSchemaMismatch, "schema", &output);

    auto wrong_type = ReplacePacketBatchColumns(
        batch, {{0, MakeOneValueArray<arrow::UInt32Builder>(uint32_t{1})}});
    AssertPacketBatchCreateError(wrong_type, npm::NpmPacketBatchError::kInvalidColumn, "timestamp_ns", &output);

    arrow::Int64Builder empty_builder;
    std::shared_ptr<arrow::Array> empty_timestamp;
    assert(empty_builder.Finish(&empty_timestamp).ok());
    auto wrong_length = ReplacePacketBatchColumns(batch, {{0, empty_timestamp}});
    AssertPacketBatchCreateError(wrong_length, npm::NpmPacketBatchError::kInvalidColumn, "timestamp_ns", &output);

    auto required_null = ReplacePacketBatchColumns(batch, {{0, MakeOneNullArray<arrow::Int64Builder>()}});
    AssertPacketBatchCreateError(required_null, npm::NpmPacketBatchError::kInvalidColumn, "timestamp_ns", &output);

    std::array<uint16_t, flowsql::packet::kMaxLayerDepth> layer_ids{};
    auto null_list_value = ReplacePacketBatchColumns(
        batch, {{9, MakeFixedListArray<arrow::UInt16Builder>(layer_ids, true)}});
    AssertPacketBatchCreateError(
        null_list_value, npm::NpmPacketBatchError::kInvalidColumn, "layer_ids", &output);
}

void TestNpmPacketBatchViewRejectsInvalidRows() {
    auto batch = MakeNpmPacketViewBatch()->Slice(0, 1);
    std::unique_ptr<npm::NpmPacketBatchView> output;
    assert(npm::NpmPacketBatchView::Create(batch, &output) == npm::NpmPacketBatchError::kNone);

    const uint32_t captured_len =
        std::static_pointer_cast<arrow::UInt32Array>(batch->column(1))->Value(0);
    auto captured_mismatch = ReplacePacketBatchColumns(
        batch, {{1, MakeOneValueArray<arrow::UInt32Builder>(captured_len - 1)}});
    AssertPacketBatchCreateError(
        captured_mismatch, npm::NpmPacketBatchError::kInvalidRow, "captured_len", &output);

    auto wire_too_small = ReplacePacketBatchColumns(
        batch, {{2, MakeOneValueArray<arrow::UInt32Builder>(captured_len - 1)}});
    AssertPacketBatchCreateError(wire_too_small, npm::NpmPacketBatchError::kInvalidRow, "wire_len", &output);

    auto invalid_status = ReplacePacketBatchColumns(
        batch, {{7, MakeOneValueArray<arrow::UInt8Builder>(uint8_t{9})}});
    AssertPacketBatchCreateError(invalid_status, npm::NpmPacketBatchError::kInvalidRow, "layer_status", &output);

    auto invalid_count = ReplacePacketBatchColumns(
        batch, {{8, MakeOneValueArray<arrow::UInt8Builder>(uint8_t{flowsql::packet::kMaxLayerDepth + 1})}});
    AssertPacketBatchCreateError(invalid_count, npm::NpmPacketBatchError::kInvalidRow, "layer_count", &output);

    std::array<uint32_t, flowsql::packet::kMaxLayerDepth> layer_offsets{};
    layer_offsets[0] = captured_len + 1;
    auto invalid_offset = ReplacePacketBatchColumns(
        batch, {{10, MakeFixedListArray<arrow::UInt32Builder>(layer_offsets)}});
    AssertPacketBatchCreateError(invalid_offset, npm::NpmPacketBatchError::kInvalidRow, "layer_offsets", &output);

    auto invalid_scope = ReplacePacketBatchColumns(
        batch, {{11, MakeOneValueArray<arrow::UInt8Builder>(uint8_t{2})}});
    AssertPacketBatchCreateError(invalid_scope, npm::NpmPacketBatchError::kInvalidRow, "endpoint_scope", &output);

    auto invalid_network_index = ReplacePacketBatchColumns(
        batch, {{12, MakeOneValueArray<arrow::UInt8Builder>(uint8_t{2})}});
    AssertPacketBatchCreateError(
        invalid_network_index, npm::NpmPacketBatchError::kInvalidRow, "network_layer_index", &output);

    auto invalid_transport_index = ReplacePacketBatchColumns(
        batch, {{13, MakeOneValueArray<arrow::UInt8Builder>(uint8_t{2})}});
    AssertPacketBatchCreateError(
        invalid_transport_index, npm::NpmPacketBatchError::kInvalidRow, "transport_layer_index", &output);

    auto invalid_payload = ReplacePacketBatchColumns(
        batch, {{14, MakeOneValueArray<arrow::UInt32Builder>(captured_len + 1)}});
    AssertPacketBatchCreateError(invalid_payload, npm::NpmPacketBatchError::kInvalidRow, "payload_offset", &output);

    auto invalid_ip_family = ReplacePacketBatchColumns(
        batch, {{21, MakeOneValueArray<arrow::UInt8Builder>(
                         static_cast<uint8_t>(flowsql::packet::AddressFamily::kNone))}});
    AssertPacketBatchCreateError(invalid_ip_family, npm::NpmPacketBatchError::kInvalidRow, "src_ip", &output);

    std::vector<uint8_t> short_ipv6(15, 0x22);
    auto invalid_ipv6 = ReplacePacketBatchColumns(
        batch,
        {{17, MakeOneNullArray<arrow::UInt32Builder>()},
         {19, MakeOneBinaryArray(short_ipv6)},
         {21, MakeOneValueArray<arrow::UInt8Builder>(
                  static_cast<uint8_t>(flowsql::packet::AddressFamily::kIPv6))}});
    AssertPacketBatchCreateError(invalid_ipv6, npm::NpmPacketBatchError::kInvalidRow, "src_ip_v6", &output);

    auto invalid_ports = ReplacePacketBatchColumns(
        batch, {{26, MakeOneValueArray<arrow::BooleanBuilder>(false)}});
    AssertPacketBatchCreateError(invalid_ports, npm::NpmPacketBatchError::kInvalidRow, "ports_valid", &output);

    auto invalid_protocol = ReplacePacketBatchColumns(
        batch, {{27, MakeOneValueArray<arrow::UInt8Builder>(
                         static_cast<uint8_t>(flowsql::packet::ProtocolStatus::kUnknown))}});
    AssertPacketBatchCreateError(
        invalid_protocol, npm::NpmPacketBatchError::kInvalidRow, "protocol_status", &output);
}

npm::NpmSessionView MakeProjectionSession(const npm::NpmSessionKey* key, uint64_t session_id) {
    npm::NpmSessionView session;
    session.session_id = session_id;
    session.key = key;
    session.first_ns = 100;
    session.last_ns = 200;
    session.packets_ab = 3;
    session.packets_ba = 5;
    session.wire_bytes_ab = 300;
    session.wire_bytes_ba = 700;
    return session;
}

void TestNpmBasicResultProjectionRevisionsAndLabels() {
    static_assert(!std::is_copy_constructible_v<npm::NpmBasicResultProjector>);
    static_assert(!std::is_copy_assignable_v<npm::NpmBasicResultProjector>);
    static_assert(!std::is_move_constructible_v<npm::NpmBasicResultProjector>);
    static_assert(!std::is_move_assignable_v<npm::NpmBasicResultProjector>);

    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    std::unique_ptr<npm::NpmProtocolContext> context;
    assert(npm::NpmProtocolContext::Create(&querier, &context) == npm::NpmProtocolContextError::kNone);
    npm::NpmBasicResultProjector projector(*context);
    assert(projector.tracked_sessions() == 0);

    npm::NpmSessionKey ipv4_key;
    ipv4_key.input_namespace = "pcapfile.capture";
    ipv4_key.observation_domain_id = 77;
    ipv4_key.ip_family = flowsql::packet::AddressFamily::kIPv4;
    ipv4_key.transport_protocol = flowsql::ipv4::eNext::TCP;
    ipv4_key.a.ip = ParseIpv4("192.0.2.1");
    ipv4_key.a.port = 443;
    ipv4_key.b.ip = ParseIpv4("198.51.100.2");
    ipv4_key.b.port = 41000;
    auto session = MakeProjectionSession(&ipv4_key, 42);

    npm::NpmBasicResult result;
    assert(projector.ProjectActive(session, 1000, &result) == npm::NpmBasicProjectionError::kNone);
    assert(result.session_id == 42 && result.observation_domain_id == 77);
    assert(result.revision == 1 && result.observed_at == 1000 && !result.is_final);
    assert(result.ip_family == static_cast<uint8_t>(flowsql::packet::AddressFamily::kIPv4));
    assert(result.transport_protocol == flowsql::ipv4::eNext::TCP);
    assert(result.a_ip == "192.0.2.1" && result.b_ip == "198.51.100.2");
    assert(result.a_port == 443 && result.b_port == 41000);
    assert(result.first_ns == 100 && result.last_ns == 200);
    assert(result.packets_ab == 3 && result.packets_ba == 5);
    assert(result.wire_bytes_ab == 300 && result.wire_bytes_ba == 700);
    assert(result.protocol_status == npm::NpmProtocolStatus::kPending);
    assert(!result.protocol_id && !result.protocol_sub_id && !result.protocol && !result.end_reason);
    assert(npm::ValidateNpmBasicResult(result) == npm::NpmBasicResultError::kNone);
    assert(projector.tracked_sessions() == 1);

    session.protocol_status = npm::NpmProtocolStatus::kIdentified;
    session.protocol_id = 7;
    session.protocol_sub_id = 8;
    assert(projector.ProjectActive(session, 1100, &result) == npm::NpmBasicProjectionError::kNone);
    assert(result.revision == 2 && result.protocol_id == 7 && result.protocol_sub_id == 8);
    assert(result.protocol == "SUB");

    session.protocol_sub_id = 9;
    assert(projector.ProjectActive(session, 1200, &result) == npm::NpmBasicProjectionError::kNone);
    assert(result.revision == 3 && result.protocol_id == 7 && result.protocol_sub_id == 9);
    assert(result.protocol == "MAIN");

    npm::NpmSessionKey ipv6_key;
    ipv6_key.input_namespace = "pcapfile.capture";
    ipv6_key.observation_domain_id = 88;
    ipv6_key.ip_family = flowsql::packet::AddressFamily::kIPv6;
    ipv6_key.transport_protocol = flowsql::ipv6::eNext::UDP;
    ipv6_key.a.ip = ParseIpv6("2001:db8::1");
    ipv6_key.a.port = 53;
    ipv6_key.b.ip = ParseIpv6("2001:db8::2");
    ipv6_key.b.port = 53000;
    auto ipv6_session = MakeProjectionSession(&ipv6_key, 43);
    ipv6_session.protocol_status = npm::NpmProtocolStatus::kUnknown;
    assert(projector.ProjectActive(ipv6_session, 1300, &result) == npm::NpmBasicProjectionError::kNone);
    assert(result.revision == 1 && result.a_ip == "2001:db8::1" && result.b_ip == "2001:db8::2");
    assert(result.protocol_status == npm::NpmProtocolStatus::kUnknown);
    assert(!result.protocol_id && !result.protocol_sub_id && !result.protocol);
    assert(projector.tracked_sessions() == 2);

    assert(projector.ProjectFinal(session, npm::NpmSessionEndReason::kClosed, 1400, &result) ==
           npm::NpmBasicProjectionError::kNone);
    assert(result.revision == 4 && result.observed_at == 1400 && result.is_final);
    assert(result.end_reason == npm::NpmSessionEndReason::kClosed);
    assert(result.protocol == "MAIN");
    assert(npm::ValidateNpmBasicResult(result) == npm::NpmBasicResultError::kNone);
    assert(projector.tracked_sessions() == 1);

    assert(projector.ProjectFinal(ipv6_session, npm::NpmSessionEndReason::kIdleTimeout, 1500, &result) ==
           npm::NpmBasicProjectionError::kNone);
    assert(result.revision == 2 && result.is_final);
    assert(result.end_reason == npm::NpmSessionEndReason::kIdleTimeout);
    assert(projector.tracked_sessions() == 0);

    auto final_only = MakeProjectionSession(&ipv4_key, 44);
    final_only.protocol_status = npm::NpmProtocolStatus::kUnknown;
    assert(projector.ProjectFinal(final_only, npm::NpmSessionEndReason::kEof, 1600, &result) ==
           npm::NpmBasicProjectionError::kNone);
    assert(result.revision == 1 && result.end_reason == npm::NpmSessionEndReason::kEof);
    assert(projector.tracked_sessions() == 0);
}

void TestNpmBasicResultProjectionErrorsDoNotConsumeRevision() {
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    std::unique_ptr<npm::NpmProtocolContext> context;
    assert(npm::NpmProtocolContext::Create(&querier, &context) == npm::NpmProtocolContextError::kNone);
    npm::NpmBasicResultProjector projector(*context);

    npm::NpmSessionKey key;
    key.input_namespace = "pcapfile.capture";
    key.observation_domain_id = 77;
    key.ip_family = flowsql::packet::AddressFamily::kIPv4;
    key.transport_protocol = flowsql::ipv4::eNext::TCP;
    key.a.ip = ParseIpv4("192.0.2.1");
    key.a.port = 443;
    key.b.ip = ParseIpv4("198.51.100.2");
    key.b.port = 41000;
    auto session = MakeProjectionSession(&key, 50);

    npm::NpmBasicResult output;
    output.session_id = 999;
    output.revision = 88;
    output.a_ip = "sentinel";
    assert(projector.ProjectActive(session, 1000, nullptr) == npm::NpmBasicProjectionError::kNullOutput);

    auto invalid = session;
    invalid.key = nullptr;
    assert(projector.ProjectActive(invalid, 1000, &output) == npm::NpmBasicProjectionError::kInvalidSession);
    assert(output.session_id == 999 && output.revision == 88 && output.a_ip == "sentinel");
    invalid = session;
    invalid.session_id = 0;
    assert(projector.ProjectActive(invalid, 1000, &output) == npm::NpmBasicProjectionError::kInvalidSession);
    invalid = session;
    invalid.first_ns = 201;
    assert(projector.ProjectActive(invalid, 1000, &output) == npm::NpmBasicProjectionError::kInvalidSession);

    auto invalid_key = key;
    invalid_key.ip_family = flowsql::packet::AddressFamily::kIPv6;
    invalid = session;
    invalid.key = &invalid_key;
    assert(projector.ProjectActive(invalid, 1000, &output) == npm::NpmBasicProjectionError::kInvalidAddress);

    invalid = session;
    invalid.protocol_status = static_cast<npm::NpmProtocolStatus>(3);
    assert(projector.ProjectActive(invalid, 1000, &output) == npm::NpmBasicProjectionError::kInvalidSession);
    invalid = session;
    invalid.protocol_id = 7;
    assert(projector.ProjectActive(invalid, 1000, &output) == npm::NpmBasicProjectionError::kInvalidSession);
    invalid.protocol_status = npm::NpmProtocolStatus::kIdentified;
    invalid.protocol_id.reset();
    assert(projector.ProjectActive(invalid, 1000, &output) == npm::NpmBasicProjectionError::kInvalidSession);
    invalid.protocol_id = 0;
    assert(projector.ProjectActive(invalid, 1000, &output) == npm::NpmBasicProjectionError::kInvalidSession);

    invalid.protocol_id = 99;
    assert(projector.ProjectActive(invalid, 1000, &output) ==
           npm::NpmBasicProjectionError::kProtocolNameUnavailable);
    assert(output.session_id == 999 && output.revision == 88 && output.a_ip == "sentinel");
    assert(projector.tracked_sessions() == 0);

    session.protocol_status = npm::NpmProtocolStatus::kUnknown;
    assert(projector.ProjectActive(session, 1100, &output) == npm::NpmBasicProjectionError::kNone);
    assert(output.session_id == 50 && output.revision == 1);
    assert(projector.tracked_sessions() == 1);

    invalid = session;
    invalid.protocol_status = npm::NpmProtocolStatus::kPending;
    output.a_ip = "still-active";
    assert(projector.ProjectFinal(invalid, npm::NpmSessionEndReason::kEof, 1200, &output) ==
           npm::NpmBasicProjectionError::kInvalidResult);
    assert(output.revision == 1 && output.a_ip == "still-active");
    assert(projector.tracked_sessions() == 1);

    assert(projector.ProjectFinal(session, static_cast<npm::NpmSessionEndReason>(4), 1200, &output) ==
           npm::NpmBasicProjectionError::kInvalidResult);
    assert(output.revision == 1 && output.a_ip == "still-active");
    assert(projector.tracked_sessions() == 1);

    assert(projector.ProjectFinal(session, npm::NpmSessionEndReason::kTupleReuse, 1300, &output) ==
           npm::NpmBasicProjectionError::kNone);
    assert(output.revision == 2 && output.end_reason == npm::NpmSessionEndReason::kTupleReuse);
    assert(projector.tracked_sessions() == 0);
}

npm::NpmBasicResult MakeBasicEncodingResult(uint64_t index) {
    npm::NpmBasicResult result;
    result.session_id = index;
    result.observation_domain_id = 100 + index;
    result.revision = 10 + index;
    result.observed_at = 1000 + static_cast<int64_t>(index);
    result.ip_family = index % 2 == 0
                           ? static_cast<uint8_t>(flowsql::packet::AddressFamily::kIPv6)
                           : static_cast<uint8_t>(flowsql::packet::AddressFamily::kIPv4);
    result.transport_protocol =
        index % 2 == 0 ? static_cast<uint8_t>(flowsql::ipv6::eNext::UDP)
                       : static_cast<uint8_t>(flowsql::ipv4::eNext::TCP);
    result.a_ip = "a-" + std::to_string(index);
    result.b_ip = "b-" + std::to_string(index);
    result.a_port = static_cast<uint16_t>(1000 + index);
    result.b_port = static_cast<uint16_t>(2000 + index);
    result.first_ns = 2000 + static_cast<int64_t>(index);
    result.last_ns = 3000 + static_cast<int64_t>(index);
    result.packets_ab = 10 + index;
    result.packets_ba = 20 + index;
    result.wire_bytes_ab = 100 + index;
    result.wire_bytes_ba = 200 + index;
    return result;
}

template <typename ArrayType>
std::shared_ptr<ArrayType> BasicResultColumn(const std::shared_ptr<arrow::RecordBatch>& batch, int index) {
    return std::static_pointer_cast<ArrayType>(batch->column(index));
}

void TestNpmBasicResultArrowEncoding() {
    std::shared_ptr<arrow::RecordBatch> output = MakeNpmPacketViewBatch();
    const auto original = output;
    std::string error = "stale";
    assert(npm::EncodeNpmBasicResults({}, &output, &error) == npm::NpmBasicEncodeError::kNone);
    assert(error.empty() && output != nullptr && output != original);
    assert(output->schema().get() == npm::NpmBasicResultSchema().get());
    assert(output->num_rows() == 0 && output->num_columns() == 22);
    assert(output->ValidateFull().ok());

    std::vector<npm::NpmBasicResult> results;
    results.push_back(MakeBasicEncodingResult(1));

    auto active_identified = MakeBasicEncodingResult(2);
    active_identified.protocol_status = npm::NpmProtocolStatus::kIdentified;
    active_identified.protocol_id = 7;
    active_identified.protocol = "HTTP";
    results.push_back(active_identified);

    auto final_unknown = MakeBasicEncodingResult(3);
    final_unknown.is_final = true;
    final_unknown.protocol_status = npm::NpmProtocolStatus::kUnknown;
    final_unknown.end_reason = npm::NpmSessionEndReason::kClosed;
    results.push_back(final_unknown);

    auto final_identified = MakeBasicEncodingResult(4);
    final_identified.is_final = true;
    final_identified.protocol_status = npm::NpmProtocolStatus::kIdentified;
    final_identified.protocol_id = 8;
    final_identified.protocol_sub_id = 9;
    final_identified.protocol = "DNS";
    final_identified.end_reason = npm::NpmSessionEndReason::kIdleTimeout;
    results.push_back(final_identified);

    auto final_reuse = MakeBasicEncodingResult(5);
    final_reuse.is_final = true;
    final_reuse.protocol_status = npm::NpmProtocolStatus::kUnknown;
    final_reuse.end_reason = npm::NpmSessionEndReason::kTupleReuse;
    results.push_back(final_reuse);

    auto final_eof = MakeBasicEncodingResult(6);
    final_eof.is_final = true;
    final_eof.protocol_status = npm::NpmProtocolStatus::kUnknown;
    final_eof.end_reason = npm::NpmSessionEndReason::kEof;
    results.push_back(final_eof);

    error = "stale";
    assert(npm::EncodeNpmBasicResults(results, &output, &error) == npm::NpmBasicEncodeError::kNone);
    assert(error.empty() && output != nullptr);
    assert(output->schema().get() == npm::NpmBasicResultSchema().get());
    assert(output->num_rows() == 6 && output->num_columns() == 22);
    assert(output->ValidateFull().ok());

    const auto session_id = BasicResultColumn<arrow::UInt64Array>(output, 0);
    const auto observation_domain_id = BasicResultColumn<arrow::UInt64Array>(output, 1);
    const auto revision = BasicResultColumn<arrow::UInt64Array>(output, 2);
    const auto observed_at = BasicResultColumn<arrow::Int64Array>(output, 3);
    const auto is_final = BasicResultColumn<arrow::BooleanArray>(output, 4);
    const auto ip_family = BasicResultColumn<arrow::UInt8Array>(output, 5);
    const auto transport_protocol = BasicResultColumn<arrow::UInt8Array>(output, 6);
    const auto a_ip = BasicResultColumn<arrow::StringArray>(output, 7);
    const auto b_ip = BasicResultColumn<arrow::StringArray>(output, 8);
    const auto a_port = BasicResultColumn<arrow::UInt16Array>(output, 9);
    const auto b_port = BasicResultColumn<arrow::UInt16Array>(output, 10);
    const auto first_ns = BasicResultColumn<arrow::Int64Array>(output, 11);
    const auto last_ns = BasicResultColumn<arrow::Int64Array>(output, 12);
    const auto packets_ab = BasicResultColumn<arrow::UInt64Array>(output, 13);
    const auto packets_ba = BasicResultColumn<arrow::UInt64Array>(output, 14);
    const auto wire_bytes_ab = BasicResultColumn<arrow::UInt64Array>(output, 15);
    const auto wire_bytes_ba = BasicResultColumn<arrow::UInt64Array>(output, 16);
    const auto protocol_status = BasicResultColumn<arrow::StringArray>(output, 17);
    const auto protocol_id = BasicResultColumn<arrow::UInt16Array>(output, 18);
    const auto protocol_sub_id = BasicResultColumn<arrow::UInt16Array>(output, 19);
    const auto protocol = BasicResultColumn<arrow::StringArray>(output, 20);
    const auto end_reason = BasicResultColumn<arrow::StringArray>(output, 21);

    assert(session_id->Value(0) == 1 && session_id->Value(5) == 6);
    assert(observation_domain_id->Value(1) == 102);
    assert(revision->Value(2) == 13);
    assert(observed_at->Value(3) == 1004);
    assert(!is_final->Value(0) && !is_final->Value(1));
    assert(is_final->Value(2) && is_final->Value(5));
    assert(ip_family->Value(0) == static_cast<uint8_t>(flowsql::packet::AddressFamily::kIPv4));
    assert(ip_family->Value(1) == static_cast<uint8_t>(flowsql::packet::AddressFamily::kIPv6));
    assert(transport_protocol->Value(0) == flowsql::ipv4::eNext::TCP);
    assert(transport_protocol->Value(1) == flowsql::ipv6::eNext::UDP);
    assert(a_ip->GetString(4) == "a-5" && b_ip->GetString(4) == "b-5");
    assert(a_port->Value(0) == 1001 && b_port->Value(0) == 2001);
    assert(first_ns->Value(1) == 2002 && last_ns->Value(1) == 3002);
    assert(packets_ab->Value(2) == 13 && packets_ba->Value(2) == 23);
    assert(wire_bytes_ab->Value(3) == 104 && wire_bytes_ba->Value(3) == 204);
    assert(protocol_status->GetString(0) == "pending");
    assert(protocol_status->GetString(1) == "identified");
    assert(protocol_status->GetString(2) == "unknown");
    assert(protocol_id->IsNull(0) && protocol_sub_id->IsNull(0) && protocol->IsNull(0));
    assert(!protocol_id->IsNull(1) && protocol_id->Value(1) == 7);
    assert(protocol_sub_id->IsNull(1) && protocol->GetString(1) == "HTTP");
    assert(protocol_id->Value(3) == 8 && protocol_sub_id->Value(3) == 9);
    assert(protocol->GetString(3) == "DNS");
    assert(end_reason->IsNull(0) && end_reason->IsNull(1));
    assert(end_reason->GetString(2) == "closed");
    assert(end_reason->GetString(3) == "idle_timeout");
    assert(end_reason->GetString(4) == "tuple_reuse");
    assert(end_reason->GetString(5) == "eof");

    results[1].a_ip = "mutated";
    results[1].protocol = "mutated";
    assert(a_ip->GetString(1) == "a-2" && protocol->GetString(1) == "HTTP");
}

void TestNpmBasicResultArrowEncodingRejectsInvalidInput() {
    std::vector<npm::NpmBasicResult> results = {MakeBasicEncodingResult(1), MakeBasicEncodingResult(2)};
    std::shared_ptr<arrow::RecordBatch> output;
    std::string error;
    assert(npm::EncodeNpmBasicResults(results, &output, &error) == npm::NpmBasicEncodeError::kNone);
    const auto original = output;

    results[1].protocol_id = 80;
    error = "stale";
    assert(npm::EncodeNpmBasicResults(results, &output, &error) ==
           npm::NpmBasicEncodeError::kInvalidResult);
    assert(output == original);
    assert(error.find("result[1]") != std::string::npos);

    results[1].protocol_id.reset();
    results[1].a_ip = std::string(1, static_cast<char>(0xff));
    error = "stale";
    assert(npm::EncodeNpmBasicResults(results, &output, &error) ==
           npm::NpmBasicEncodeError::kInvalidResult);
    assert(output == original);
    assert(error.find("result[1].a_ip") != std::string::npos);

    error = "stale";
    assert(npm::EncodeNpmBasicResults(results, nullptr, &error) == npm::NpmBasicEncodeError::kNullOutput);
    assert(error.find("output") != std::string::npos);
    assert(output == original);
}

struct PendingOutputBudgetStats {
    uint32_t reserve_calls = 0;
    uint32_t release_calls = 0;
    npm::NpmBudgetCategory last_reserve_category = npm::NpmBudgetCategory::kSessionState;
    npm::NpmBudgetCategory last_release_category = npm::NpmBudgetCategory::kSessionState;
    uint64_t last_reserved_bytes = 0;
    uint64_t last_released_bytes = 0;
};

class PendingOutputBudget final : public npm::INpmTaskBudget {
 public:
    PendingOutputBudget(npm::NpmAnalysisConfig config,
                        std::shared_ptr<PendingOutputBudgetStats> stats)
        : config_(config), stats_(std::move(stats)) {}

    npm::NpmBudgetError Reserve(npm::NpmBudgetCategory category, uint64_t bytes) override {
        ++stats_->reserve_calls;
        stats_->last_reserve_category = category;
        stats_->last_reserved_bytes = bytes;
        return npm::ReserveNpmBudget(config_, category, bytes, &usage_);
    }

    npm::NpmBudgetError Release(npm::NpmBudgetCategory category, uint64_t bytes) override {
        ++stats_->release_calls;
        stats_->last_release_category = category;
        stats_->last_released_bytes = bytes;
        return npm::ReleaseNpmBudget(category, bytes, &usage_);
    }

    npm::NpmBudgetUsage Usage() const override { return usage_; }

 private:
    npm::NpmAnalysisConfig config_;
    std::shared_ptr<PendingOutputBudgetStats> stats_;
    npm::NpmBudgetUsage usage_;
};

uint64_t BasicResultBufferBytes(const std::shared_ptr<arrow::RecordBatch>& batch) {
    const int64_t bytes = arrow::util::TotalBufferSize(*batch);
    assert(bytes >= 0);
    return static_cast<uint64_t>(bytes);
}

npm::NpmSessionEndEvent MakeCollectorEndEvent(uint64_t session_id,
                                              npm::NpmSessionEndReason reason,
                                              int64_t observed_at) {
    npm::NpmSessionEndEvent event;
    event.snapshot.key.input_namespace = "pcapfile.capture";
    event.snapshot.key.observation_domain_id = 77 + session_id;
    event.snapshot.key.ip_family = flowsql::packet::AddressFamily::kIPv4;
    event.snapshot.key.transport_protocol = flowsql::ipv4::eNext::TCP;
    event.snapshot.key.a.ip = ParseIpv4("192.0.2.1");
    event.snapshot.key.a.port = 443;
    event.snapshot.key.b.ip = ParseIpv4("198.51.100.2");
    event.snapshot.key.b.port = 41000;
    event.snapshot.session_id = session_id;
    event.snapshot.first_ns = 100;
    event.snapshot.last_ns = 200;
    event.snapshot.packets_ab = 3;
    event.snapshot.packets_ba = 5;
    event.snapshot.wire_bytes_ab = 300;
    event.snapshot.wire_bytes_ba = 700;
    event.snapshot.protocol_status = npm::NpmProtocolStatus::kUnknown;
    event.snapshot.end_reason = reason;
    event.observed_at = observed_at;
    return event;
}

void TestNpmBasicResultCollectorCopiesAndDrainsUnifiedBatches() {
    static_assert(!std::is_copy_constructible_v<npm::NpmBasicResultCollector>);
    static_assert(!std::is_copy_assignable_v<npm::NpmBasicResultCollector>);
    static_assert(!std::is_move_constructible_v<npm::NpmBasicResultCollector>);
    static_assert(!std::is_move_assignable_v<npm::NpmBasicResultCollector>);

    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    std::unique_ptr<npm::NpmProtocolContext> context;
    assert(npm::NpmProtocolContext::Create(&querier, &context) == npm::NpmProtocolContextError::kNone);
    npm::NpmBasicResultProjector projector(*context);
    npm::NpmBasicResultCollector collector;

    auto module_result = MakeBasicEncodingResult(90);
    module_result.observed_at = 9000;
    assert(collector.WriteBasic(module_result) == 0);
    module_result.a_ip = "mutated";
    module_result.observed_at = -1;
    assert(collector.pending_results() == 1);

    auto invalid = MakeBasicEncodingResult(91);
    invalid.protocol_id = 7;
    assert(collector.WriteBasic(invalid) == EINVAL);
    assert(collector.pending_results() == 1);

    auto config = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline);
    auto stats = std::make_shared<PendingOutputBudgetStats>();
    auto budget = std::make_shared<PendingOutputBudget>(config, stats);
    const std::vector<npm::NpmSessionEndEvent> normal_events = {
        MakeCollectorEndEvent(91, npm::NpmSessionEndReason::kClosed, 9100),
        MakeCollectorEndEvent(92, npm::NpmSessionEndReason::kIdleTimeout, 9200),
    };
    std::shared_ptr<arrow::RecordBatch> normal_output;
    auto status = collector.Drain(normal_events, projector, budget, &normal_output);
    assert(status.error == npm::NpmBasicDrainError::kNone && status.event_index == -1);
    assert(collector.pending_results() == 0 && normal_output != nullptr);
    assert(normal_output->num_rows() == 3 && normal_output->schema()->Equals(*npm::NpmBasicResultSchema(), true));
    const auto normal_ids = BasicResultColumn<arrow::UInt64Array>(normal_output, 0);
    const auto normal_observed = BasicResultColumn<arrow::Int64Array>(normal_output, 3);
    const auto normal_final = BasicResultColumn<arrow::BooleanArray>(normal_output, 4);
    const auto normal_a_ip = BasicResultColumn<arrow::StringArray>(normal_output, 7);
    const auto normal_reason = BasicResultColumn<arrow::StringArray>(normal_output, 21);
    assert(normal_ids->Value(0) == 90 && normal_ids->Value(1) == 91 && normal_ids->Value(2) == 92);
    assert(normal_observed->Value(0) == 9000 && normal_observed->Value(1) == 9100);
    assert(normal_observed->Value(2) == 9200 && normal_a_ip->GetString(0) == "a-90");
    assert(!normal_final->Value(0) && normal_final->Value(1) && normal_final->Value(2));
    assert(normal_reason->IsNull(0) && normal_reason->GetString(1) == "closed");
    assert(normal_reason->GetString(2) == "idle_timeout");
    assert(stats->reserve_calls == 1 && stats->release_calls == 0);
    assert(budget->Usage().pending_output_bytes == BasicResultBufferBytes(normal_output));

    auto eof_module_result = MakeBasicEncodingResult(93);
    assert(collector.WriteBasic(eof_module_result) == 0);
    const std::vector<npm::NpmSessionEndEvent> eof_events = {
        MakeCollectorEndEvent(94, npm::NpmSessionEndReason::kEof, 9400),
    };
    std::shared_ptr<arrow::RecordBatch> eof_output;
    status = collector.Drain(eof_events, projector, budget, &eof_output);
    assert(status.error == npm::NpmBasicDrainError::kNone && eof_output->num_rows() == 2);
    const auto eof_ids = BasicResultColumn<arrow::UInt64Array>(eof_output, 0);
    const auto eof_reason = BasicResultColumn<arrow::StringArray>(eof_output, 21);
    assert(eof_ids->Value(0) == 93 && eof_ids->Value(1) == 94);
    assert(eof_reason->IsNull(0) && eof_reason->GetString(1) == "eof");
    assert(collector.pending_results() == 0 && stats->reserve_calls == 2);

    normal_output.reset();
    assert(stats->release_calls == 1 && budget->Usage().pending_output_bytes > 0);
    eof_output.reset();
    assert(stats->release_calls == 2 && budget->Usage().pending_output_bytes == 0);

    std::shared_ptr<arrow::RecordBatch> empty_output;
    status = collector.Drain({}, projector, budget, &empty_output);
    assert(status.error == npm::NpmBasicDrainError::kNone && empty_output->num_rows() == 0);
    assert(stats->reserve_calls == 3 && budget->Usage().pending_output_bytes > 0);
    empty_output.reset();
    assert(stats->release_calls == 3 && budget->Usage().pending_output_bytes == 0);
}

void TestNpmBasicResultCollectorFailuresKeepPendingAndOutput() {
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    std::unique_ptr<npm::NpmProtocolContext> context;
    assert(npm::NpmProtocolContext::Create(&querier, &context) == npm::NpmProtocolContextError::kNone);
    npm::NpmBasicResultProjector projector(*context);
    npm::NpmBasicResultCollector collector;
    assert(collector.WriteBasic(MakeBasicEncodingResult(100)) == 0);

    std::shared_ptr<arrow::RecordBatch> output = MakeNpmPacketViewBatch();
    const auto original = output;
    auto config = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline);
    auto stats = std::make_shared<PendingOutputBudgetStats>();
    auto budget = std::make_shared<PendingOutputBudget>(config, stats);

    auto status = collector.Drain({}, projector, budget, nullptr);
    assert(status.error == npm::NpmBasicDrainError::kNullOutput);
    assert(collector.pending_results() == 1 && output == original && stats->reserve_calls == 0);
    status = collector.Drain({}, projector, {}, &output);
    assert(status.error == npm::NpmBasicDrainError::kNullBudget);
    assert(collector.pending_results() == 1 && output == original && stats->reserve_calls == 0);

    auto invalid_event = MakeCollectorEndEvent(101, npm::NpmSessionEndReason::kEof, 10100);
    invalid_event.snapshot.key.input_namespace.clear();
    status = collector.Drain({invalid_event}, projector, budget, &output);
    assert(status.error == npm::NpmBasicDrainError::kProjectionError && status.event_index == 0);
    assert(status.projection_error == npm::NpmBasicProjectionError::kInvalidSession);
    assert(collector.pending_results() == 1 && output == original && stats->reserve_calls == 0);

    config.max_pending_output_bytes = 1;
    auto small_stats = std::make_shared<PendingOutputBudgetStats>();
    auto small_budget = std::make_shared<PendingOutputBudget>(config, small_stats);
    status = collector.Drain({}, projector, small_budget, &output);
    assert(status.error == npm::NpmBasicDrainError::kEncodeError);
    assert(status.encode_error == npm::NpmBasicEncodeError::kBudgetError);
    assert(collector.pending_results() == 1 && output == original);
    assert(small_stats->reserve_calls == 1 && small_stats->release_calls == 0);
}

void AddEofFlushSession(npm::NpmSessionTable& table,
                        const npm::NpmObservationDomainMap& domain_map,
                        const PacketFixture& fixture,
                        int64_t timestamp_ns) {
    flowsql::packet::PacketMeta meta;
    const auto binding = BuildBinding(domain_map, fixture, 1, timestamp_ns, 100, &meta);
    npm::NpmSessionView view;
    assert(ObserveActive(table, binding, meta, &view) == npm::NpmSessionTableError::kNone);
}

class EofWritingModule final : public npm::INpmAnalysisModule {
 public:
    EofWritingModule(uint64_t marker, std::vector<uint64_t>* events)
        : marker_(marker), events_(events) {}

    int OnPacket(const npm::NpmPacketView&,
                 const npm::NpmSessionView&,
                 npm::INpmResultWriter&) override {
        return 0;
    }

    int OnSessionEnd(const npm::NpmSessionView& session,
                     npm::NpmSessionEndReason reason,
                     npm::INpmResultWriter& writer) override {
        assert(reason == npm::NpmSessionEndReason::kEof);
        events_->push_back(1000 + session.session_id * 10 + marker_);
        auto result = MakeBasicEncodingResult(100 + session.session_id * 10 + marker_);
        result.observed_at = 4000 + static_cast<int64_t>(session.session_id * 10 + marker_);
        return writer.WriteBasic(result);
    }

 private:
    uint64_t marker_ = 0;
    std::vector<uint64_t>* events_ = nullptr;
};

void TestNpmEofFlusherSuccessEmptyAndRepeated() {
    static_assert(!std::is_copy_constructible_v<npm::NpmEofFlusher>);
    static_assert(!std::is_copy_assignable_v<npm::NpmEofFlusher>);
    static_assert(!std::is_move_constructible_v<npm::NpmEofFlusher>);
    static_assert(!std::is_move_assignable_v<npm::NpmEofFlusher>);

    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    std::unique_ptr<npm::NpmProtocolContext> context;
    assert(npm::NpmProtocolContext::Create(&querier, &context) == npm::NpmProtocolContextError::kNone);
    npm::NpmBasicResultProjector projector(*context);
    npm::NpmBasicResultCollector collector;

    const npm::NpmObservationDomainMap domain_map{"capture-a", {{1, 77}}};
    const auto first = MakeIpv4TcpPacket("192.0.2.1", 50000, "192.0.2.2", 443, {0x10});
    const auto second = MakeIpv4TcpPacket("198.51.100.1", 51000, "198.51.100.2", 8443, {0x20});
    auto config = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline);
    npm::NpmSessionTable table(config);
    AddEofFlushSession(table, domain_map, first, 100);
    AddEofFlushSession(table, domain_map, second, 200);

    std::vector<uint64_t> events;
    EofWritingModule first_module(1, &events);
    EofWritingModule second_module(2, &events);
    const std::vector<npm::INpmAnalysisModule*> modules{&first_module, &second_module};
    auto stats = std::make_shared<PendingOutputBudgetStats>();
    auto budget = std::make_shared<PendingOutputBudget>(config, stats);
    std::shared_ptr<arrow::RecordBatch> output;
    npm::NpmEofFlusher flusher;
    assert(flusher.state() == npm::NpmEofFlushState::kOpen);

    auto status = flusher.Flush(5000, table, modules, collector, projector, budget, &output);
    assert(status.error == npm::NpmEofFlushError::kNone);
    assert(flusher.state() == npm::NpmEofFlushState::kFlushed);
    assert(table.size() == 0 && collector.pending_results() == 0);
    assert((events == std::vector<uint64_t>{1011, 1012, 1021, 1022}));
    assert(output != nullptr && output->num_rows() == 6);
    const auto session_ids = BasicResultColumn<arrow::UInt64Array>(output, 0);
    const auto observed_at = BasicResultColumn<arrow::Int64Array>(output, 3);
    const auto is_final = BasicResultColumn<arrow::BooleanArray>(output, 4);
    const auto end_reason = BasicResultColumn<arrow::StringArray>(output, 21);
    assert(session_ids->Value(0) == 111 && session_ids->Value(1) == 112);
    assert(session_ids->Value(2) == 121 && session_ids->Value(3) == 122);
    assert(session_ids->Value(4) == 1 && session_ids->Value(5) == 2);
    assert(!is_final->Value(0) && !is_final->Value(1) && !is_final->Value(2));
    assert(!is_final->Value(3) && is_final->Value(4) && is_final->Value(5));
    assert(observed_at->Value(0) == 4011 && observed_at->Value(3) == 4022);
    assert(observed_at->Value(4) == 5000 && observed_at->Value(5) == 5000);
    assert(end_reason->IsNull(0) && end_reason->IsNull(3));
    assert(end_reason->GetString(4) == "eof" && end_reason->GetString(5) == "eof");
    assert(stats->reserve_calls == 1 && stats->release_calls == 0);
    assert(budget->Usage().pending_output_bytes == BasicResultBufferBytes(output));

    auto original = output;
    status = flusher.Flush(6000, table, modules, collector, projector, budget, &output);
    assert(status.error == npm::NpmEofFlushError::kAlreadyFlushed);
    assert(output == original && events.size() == 4 && stats->reserve_calls == 1);
    flusher.Cancel();
    flusher.MarkFailed();
    assert(flusher.state() == npm::NpmEofFlushState::kFlushed);
    output.reset();
    assert(stats->release_calls == 0 && budget->Usage().pending_output_bytes > 0);
    original.reset();
    assert(stats->release_calls == 1 && budget->Usage().pending_output_bytes == 0);

    npm::NpmSessionTable empty_table(config);
    npm::NpmBasicResultCollector empty_collector;
    npm::NpmBasicResultProjector empty_projector(*context);
    npm::NpmEofFlusher empty_flusher;
    std::shared_ptr<arrow::RecordBatch> empty_output;
    status = empty_flusher.Flush(
        7000, empty_table, {}, empty_collector, empty_projector, budget, &empty_output);
    assert(status.error == npm::NpmEofFlushError::kNone);
    assert(empty_flusher.state() == npm::NpmEofFlushState::kFlushed);
    assert(empty_output != nullptr && empty_output->num_rows() == 0);
    empty_output.reset();
    assert(stats->release_calls == 2 && budget->Usage().pending_output_bytes == 0);
}

void TestNpmEofFlusherTerminalAndErrorPaths() {
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    std::unique_ptr<npm::NpmProtocolContext> context;
    assert(npm::NpmProtocolContext::Create(&querier, &context) == npm::NpmProtocolContextError::kNone);
    const npm::NpmObservationDomainMap domain_map{"capture-a", {{1, 77}}};
    const auto packet = MakeIpv4TcpPacket("192.0.2.1", 50000, "192.0.2.2", 443, {0x10});
    auto config = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline);
    auto stats = std::make_shared<PendingOutputBudgetStats>();
    auto budget = std::make_shared<PendingOutputBudget>(config, stats);
    std::vector<uint64_t> events;
    PacketProcessorRecordingModule module(1, &events);
    std::vector<npm::INpmAnalysisModule*> modules{&module};

    npm::NpmSessionTable cancelled_table(config);
    AddEofFlushSession(cancelled_table, domain_map, packet, 100);
    npm::NpmBasicResultCollector cancelled_collector;
    assert(cancelled_collector.WriteBasic(MakeBasicEncodingResult(100)) == 0);
    npm::NpmBasicResultProjector cancelled_projector(*context);
    npm::NpmEofFlusher cancelled;
    cancelled.Cancel();
    assert(cancelled.state() == npm::NpmEofFlushState::kCancelled);
    std::shared_ptr<arrow::RecordBatch> output = MakeNpmPacketViewBatch();
    const auto original = output;
    auto status = cancelled.Flush(
        1000, cancelled_table, modules, cancelled_collector, cancelled_projector, budget, &output);
    assert(status.error == npm::NpmEofFlushError::kCancelled);
    assert(cancelled_table.size() == 1 && cancelled_collector.pending_results() == 1);
    assert(output == original && events.empty() && stats->reserve_calls == 0);
    cancelled.MarkFailed();
    assert(cancelled.state() == npm::NpmEofFlushState::kCancelled);

    npm::NpmSessionTable failed_table(config);
    AddEofFlushSession(failed_table, domain_map, packet, 100);
    npm::NpmBasicResultCollector failed_collector;
    npm::NpmBasicResultProjector failed_projector(*context);
    npm::NpmEofFlusher failed;
    failed.MarkFailed();
    status = failed.Flush(1000, failed_table, modules, failed_collector, failed_projector, budget, &output);
    assert(status.error == npm::NpmEofFlushError::kFailedState);
    assert(failed_table.size() == 1 && output == original && events.empty());

    npm::NpmSessionTable null_output_table(config);
    AddEofFlushSession(null_output_table, domain_map, packet, 100);
    npm::NpmBasicResultCollector null_output_collector;
    npm::NpmBasicResultProjector null_output_projector(*context);
    npm::NpmEofFlusher null_output;
    status = null_output.Flush(
        1000, null_output_table, modules, null_output_collector, null_output_projector, budget, nullptr);
    assert(status.error == npm::NpmEofFlushError::kNullOutput);
    assert(null_output.state() == npm::NpmEofFlushState::kFailed && null_output_table.size() == 1);

    npm::NpmSessionTable null_budget_table(config);
    AddEofFlushSession(null_budget_table, domain_map, packet, 100);
    npm::NpmBasicResultCollector null_budget_collector;
    npm::NpmBasicResultProjector null_budget_projector(*context);
    npm::NpmEofFlusher null_budget;
    status = null_budget.Flush(
        1000, null_budget_table, modules, null_budget_collector, null_budget_projector, {}, &output);
    assert(status.error == npm::NpmEofFlushError::kNullBudget);
    assert(null_budget.state() == npm::NpmEofFlushState::kFailed && null_budget_table.size() == 1);

    npm::NpmSessionTable null_module_table(config);
    AddEofFlushSession(null_module_table, domain_map, packet, 100);
    npm::NpmBasicResultCollector null_module_collector;
    npm::NpmBasicResultProjector null_module_projector(*context);
    npm::NpmEofFlusher null_module;
    modules.push_back(nullptr);
    status = null_module.Flush(
        1000, null_module_table, modules, null_module_collector, null_module_projector, budget, &output);
    assert(status.error == npm::NpmEofFlushError::kNullModule);
    assert(null_module.state() == npm::NpmEofFlushState::kFailed && null_module_table.size() == 1);
    modules.pop_back();

    npm::NpmSessionTable module_error_table(config);
    AddEofFlushSession(module_error_table, domain_map, packet, 100);
    npm::NpmBasicResultCollector module_error_collector;
    assert(module_error_collector.WriteBasic(MakeBasicEncodingResult(101)) == 0);
    npm::NpmBasicResultProjector module_error_projector(*context);
    npm::NpmEofFlusher module_error;
    module.end_error = EBUSY;
    status = module_error.Flush(1000,
                                module_error_table,
                                modules,
                                module_error_collector,
                                module_error_projector,
                                budget,
                                &output);
    assert(status.error == npm::NpmEofFlushError::kModuleError && status.module_error == EBUSY);
    assert(module_error.state() == npm::NpmEofFlushState::kFailed && module_error_table.size() == 0);
    assert(module_error_collector.pending_results() == 1 && output == original);
    assert((events == std::vector<uint64_t>{1011}));
    module.end_error = 0;
    status = module_error.Flush(2000,
                                module_error_table,
                                modules,
                                module_error_collector,
                                module_error_projector,
                                budget,
                                &output);
    assert(status.error == npm::NpmEofFlushError::kFailedState && events.size() == 1);

    npm::NpmSessionTable drain_error_table(config);
    AddEofFlushSession(drain_error_table, domain_map, packet, 100);
    npm::NpmBasicResultCollector drain_error_collector;
    assert(drain_error_collector.WriteBasic(MakeBasicEncodingResult(102)) == 0);
    npm::NpmBasicResultProjector drain_error_projector(*context);
    npm::NpmEofFlusher drain_error;
    config.max_pending_output_bytes = 1;
    auto small_stats = std::make_shared<PendingOutputBudgetStats>();
    auto small_budget = std::make_shared<PendingOutputBudget>(config, small_stats);
    events.clear();
    status = drain_error.Flush(3000,
                               drain_error_table,
                               modules,
                               drain_error_collector,
                               drain_error_projector,
                               small_budget,
                               &output);
    assert(status.error == npm::NpmEofFlushError::kDrainError);
    assert(status.drain_status.error == npm::NpmBasicDrainError::kEncodeError);
    assert(status.drain_status.encode_error == npm::NpmBasicEncodeError::kBudgetError);
    assert(drain_error.state() == npm::NpmEofFlushState::kFailed && drain_error_table.size() == 0);
    assert(drain_error_collector.pending_results() == 1 && output == original);
    assert((events == std::vector<uint64_t>{1011}));
    assert(small_stats->reserve_calls == 1 && small_stats->release_calls == 0);
}

void TestNpmBasicResultPendingOutputLease() {
    std::vector<npm::NpmBasicResult> results = {MakeBasicEncodingResult(1), MakeBasicEncodingResult(2)};
    std::shared_ptr<arrow::RecordBatch> reference;
    assert(npm::EncodeNpmBasicResults(results, &reference) == npm::NpmBasicEncodeError::kNone);
    const uint64_t expected_bytes = BasicResultBufferBytes(reference);
    assert(expected_bytes > 0);

    auto config = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline);
    config.max_pending_output_bytes = expected_bytes;
    auto stats = std::make_shared<PendingOutputBudgetStats>();
    auto budget = std::make_shared<PendingOutputBudget>(config, stats);
    std::shared_ptr<arrow::RecordBatch> output;
    std::string error = "stale";
    assert(npm::EncodeNpmBasicResultsWithBudget(results, budget, &output, &error) ==
           npm::NpmBasicEncodeError::kNone);
    assert(error.empty() && output != nullptr);
    assert(BasicResultBufferBytes(output) == expected_bytes);
    assert(stats->reserve_calls == 1 && stats->release_calls == 0);
    assert(stats->last_reserve_category == npm::NpmBudgetCategory::kPendingOutput);
    assert(stats->last_reserved_bytes == expected_bytes);
    assert(budget->Usage().pending_output_bytes == expected_bytes);

    auto output_copy = output;
    output.reset();
    assert(stats->release_calls == 0);
    assert(budget->Usage().pending_output_bytes == expected_bytes);
    output_copy.reset();
    assert(stats->release_calls == 1);
    assert(stats->last_release_category == npm::NpmBudgetCategory::kPendingOutput);
    assert(stats->last_released_bytes == expected_bytes);
    assert(budget->Usage().pending_output_bytes == 0);

    auto empty_stats = std::make_shared<PendingOutputBudgetStats>();
    auto empty_budget = std::make_shared<PendingOutputBudget>(config, empty_stats);
    std::shared_ptr<arrow::RecordBatch> empty_output;
    assert(npm::EncodeNpmBasicResultsWithBudget({}, empty_budget, &empty_output) ==
           npm::NpmBasicEncodeError::kNone);
    const uint64_t empty_bytes = BasicResultBufferBytes(empty_output);
    assert(empty_stats->last_reserved_bytes == empty_bytes);
    assert(empty_budget->Usage().pending_output_bytes == empty_bytes);
    empty_output.reset();
    assert(empty_stats->release_calls == 1 && empty_stats->last_released_bytes == empty_bytes);
    assert(empty_budget->Usage().pending_output_bytes == 0);
}

void TestNpmBasicResultPendingOutputBudgetAndLifetime() {
    const std::vector<npm::NpmBasicResult> results = {MakeBasicEncodingResult(3)};
    std::shared_ptr<arrow::RecordBatch> reference;
    assert(npm::EncodeNpmBasicResults(results, &reference) == npm::NpmBasicEncodeError::kNone);
    const uint64_t expected_bytes = BasicResultBufferBytes(reference);
    assert(expected_bytes > 0);

    auto config = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline);
    config.max_pending_output_bytes = expected_bytes * 2;
    auto stats = std::make_shared<PendingOutputBudgetStats>();
    auto budget = std::make_shared<PendingOutputBudget>(config, stats);
    std::shared_ptr<arrow::RecordBatch> normal_output;
    assert(npm::EncodeNpmBasicResultsWithBudget(results, budget, &normal_output) ==
           npm::NpmBasicEncodeError::kNone);
    assert(budget->Usage().pending_output_bytes == expected_bytes);

    std::shared_ptr<arrow::RecordBatch> flush_output;
    std::string error = "stale";
    assert(npm::EncodeNpmBasicResultsWithBudget(results, budget, &flush_output, &error) ==
           npm::NpmBasicEncodeError::kNone);
    assert(error.empty() && flush_output != nullptr);
    assert(stats->reserve_calls == 2 && stats->release_calls == 0);
    assert(budget->Usage().pending_output_bytes == expected_bytes * 2);

    std::shared_ptr<arrow::RecordBatch> blocked_output = reference;
    const auto original_blocked_output = blocked_output;
    error = "stale";
    assert(npm::EncodeNpmBasicResultsWithBudget(results, budget, &blocked_output, &error) ==
           npm::NpmBasicEncodeError::kBudgetError);
    assert(error.find("pending output budget") != std::string::npos);
    assert(blocked_output == original_blocked_output);
    assert(stats->reserve_calls == 3 && stats->release_calls == 0);
    assert(budget->Usage().pending_output_bytes == expected_bytes * 2);

    normal_output.reset();
    assert(stats->release_calls == 1 && budget->Usage().pending_output_bytes == expected_bytes);
    assert(npm::EncodeNpmBasicResultsWithBudget(results, budget, &blocked_output, &error) ==
           npm::NpmBasicEncodeError::kNone);
    assert(error.empty() && blocked_output != original_blocked_output);
    assert(stats->reserve_calls == 4);
    assert(budget->Usage().pending_output_bytes == expected_bytes * 2);
    flush_output.reset();
    assert(stats->release_calls == 2 && budget->Usage().pending_output_bytes == expected_bytes);
    blocked_output.reset();
    assert(stats->release_calls == 3 && budget->Usage().pending_output_bytes == 0);

    auto lifetime_stats = std::make_shared<PendingOutputBudgetStats>();
    auto lifetime_budget = std::make_shared<PendingOutputBudget>(config, lifetime_stats);
    std::weak_ptr<PendingOutputBudget> weak_budget = lifetime_budget;
    std::shared_ptr<arrow::RecordBatch> lifetime_output;
    assert(npm::EncodeNpmBasicResultsWithBudget(results, lifetime_budget, &lifetime_output) ==
           npm::NpmBasicEncodeError::kNone);
    lifetime_budget.reset();
    assert(!weak_budget.expired() && lifetime_stats->release_calls == 0);
    lifetime_output.reset();
    assert(weak_budget.expired() && lifetime_stats->release_calls == 1);
}

void TestNpmBasicResultPendingOutputFailuresAreAtomic() {
    std::vector<npm::NpmBasicResult> results = {MakeBasicEncodingResult(4)};
    std::shared_ptr<arrow::RecordBatch> output;
    assert(npm::EncodeNpmBasicResults(results, &output) == npm::NpmBasicEncodeError::kNone);
    const auto original = output;

    auto config = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline);
    auto stats = std::make_shared<PendingOutputBudgetStats>();
    auto budget = std::make_shared<PendingOutputBudget>(config, stats);
    std::string error = "stale";
    assert(npm::EncodeNpmBasicResultsWithBudget(results, budget, nullptr, &error) ==
           npm::NpmBasicEncodeError::kNullOutput);
    assert(error.find("output") != std::string::npos && stats->reserve_calls == 0);

    error = "stale";
    assert(npm::EncodeNpmBasicResultsWithBudget(results, {}, &output, &error) ==
           npm::NpmBasicEncodeError::kNullBudget);
    assert(error.find("budget") != std::string::npos && output == original);

    results[0].protocol_id = 80;
    error = "stale";
    assert(npm::EncodeNpmBasicResultsWithBudget(results, budget, &output, &error) ==
           npm::NpmBasicEncodeError::kInvalidResult);
    assert(error.find("result[0]") != std::string::npos);
    assert(output == original && stats->reserve_calls == 0 && stats->release_calls == 0);
}

npm::NpmBasicTaskConfigStatus ParseTaskConfigFailure(
    const char* json,
    npm::NpmBasicTaskConfigError expected_error,
    const char* expected_field = "") {
    npm::NpmBasicTaskConfig output;
    output.analysis = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kRealtime);
    output.analysis.max_active_sessions = 123;
    output.domains.input_namespace = "sentinel";
    output.domains.bindings = {{99, 88}};

    const auto status = npm::ParseNpmBasicTaskConfig(json, &output);
    assert(status.error == expected_error);
    assert(status.field == expected_field);
    assert(output.analysis.run_mode == npm::NpmRunMode::kRealtime);
    assert(output.analysis.result_mode == npm::NpmResultMode::kPeriodicSnapshot);
    assert(output.analysis.max_active_sessions == 123);
    assert(output.domains.input_namespace == "sentinel");
    assert(output.domains.bindings.size() == 1);
    assert(output.domains.bindings[0].source_id == 99);
    assert(output.domains.bindings[0].observation_domain_id == 88);
    return status;
}

void TestNpmBasicTaskConfigParsesOwnedValues() {
    static_assert(std::is_default_constructible_v<npm::NpmBasicTaskConfig>);
    static_assert(std::is_same_v<decltype(npm::NpmBasicTaskConfigStatus::field), std::string>);

    npm::NpmBasicTaskConfig minimal;
    const auto minimal_status = npm::ParseNpmBasicTaskConfig(
        R"JSON({"input_namespace":"minimal","source_domains":"0:0"})JSON", &minimal);
    assert(minimal_status.error == npm::NpmBasicTaskConfigError::kNone);
    assert(minimal.analysis.run_mode == npm::NpmRunMode::kOffline);
    assert(minimal.analysis.result_mode == npm::NpmResultMode::kFinal);
    assert(minimal.analysis.max_tracked_bytes == npm::kNpmDefaultTrackedBytes);
    assert(minimal.analysis.payload_sample_packets == npm::kNpmDefaultPayloadSamplePackets);
    assert(minimal.domains.bindings.size() == 1);
    assert(minimal.domains.bindings[0].source_id == 0);
    assert(minimal.domains.bindings[0].observation_domain_id == 0);

    std::string json = R"JSON({
        "result_mode":"periodic_snapshot",
        "source_domains":"0:7;1:7;2:8",
        "max_pending_output_bytes":"1099511627776",
        "max_tracked_bytes":"1099511627776",
        "max_active_sessions":"10000000",
        "out_of_order_tolerance_ns":"60000000000",
        "udp_idle_timeout_ns":"86400000000000",
        "tcp_idle_timeout_ns":"1000000000",
        "payload_sample_packets":"64",
        "output_interval_ns":"10000000",
        "overload_policy":"fail",
        "input_namespace":"pcapfile.capture",
        "run_mode":"offline"
    })JSON";
    npm::NpmBasicTaskConfig output;
    auto status = npm::ParseNpmBasicTaskConfig(json.c_str(), &output);
    assert(status.error == npm::NpmBasicTaskConfigError::kNone);
    assert(status.field.empty());
    assert(status.domain_error == npm::NpmObservationDomainError::kNone);
    assert(status.analysis_error == npm::NpmAnalysisConfigError::kNone);
    assert(output.analysis.run_mode == npm::NpmRunMode::kOffline);
    assert(output.analysis.result_mode == npm::NpmResultMode::kPeriodicSnapshot);
    assert(output.analysis.output_interval_ns == npm::kNpmMinOutputIntervalNs);
    assert(output.analysis.payload_sample_packets == npm::kNpmMaxPayloadSamplePackets);
    assert(output.analysis.tcp_idle_timeout_ns == npm::kNpmMinIdleTimeoutNs);
    assert(output.analysis.udp_idle_timeout_ns == npm::kNpmMaxIdleTimeoutNs);
    assert(output.analysis.out_of_order_tolerance_ns == npm::kNpmMaxOutOfOrderToleranceNs);
    assert(output.analysis.max_active_sessions == npm::kNpmMaxActiveSessions);
    assert(output.analysis.max_tracked_bytes == npm::kNpmMaxTrackedBytes);
    assert(output.analysis.max_pending_output_bytes == npm::kNpmMaxPendingOutputBytes);
    assert(output.analysis.overload_policy == npm::NpmOverloadPolicy::kFail);
    assert(output.domains.input_namespace == "pcapfile.capture");
    assert(output.domains.bindings.size() == 3);
    assert(output.domains.bindings[0].source_id == 0 &&
           output.domains.bindings[0].observation_domain_id == 7);
    assert(output.domains.bindings[1].source_id == 1 &&
           output.domains.bindings[1].observation_domain_id == 7);
    assert(output.domains.bindings[2].source_id == 2 &&
           output.domains.bindings[2].observation_domain_id == 8);

    json.assign(json.size(), 'x');
    assert(output.domains.input_namespace == "pcapfile.capture");
    assert(output.domains.bindings[2].observation_domain_id == 8);

    status = npm::ParseNpmBasicTaskConfig(
        R"JSON({"source_domains":"9:10","input_namespace":"live","run_mode":"realtime"})JSON",
        &output);
    assert(status.error == npm::NpmBasicTaskConfigError::kNone);
    assert(output.analysis.run_mode == npm::NpmRunMode::kRealtime);
    assert(output.analysis.result_mode == npm::NpmResultMode::kPeriodicSnapshot);
    assert(output.analysis.output_interval_ns == npm::kNpmDefaultOutputIntervalNs);
    assert(output.domains.input_namespace == "live");
    assert(output.domains.bindings.size() == 1);
    assert(output.domains.bindings[0].source_id == 9);
    assert(output.domains.bindings[0].observation_domain_id == 10);

    status = npm::ParseNpmBasicTaskConfig(
        R"JSON({"result_mode":"final","source_domains":"5:6","run_mode":"realtime","input_namespace":"ordered"})JSON",
        &output);
    assert(status.error == npm::NpmBasicTaskConfigError::kNone);
    assert(output.analysis.run_mode == npm::NpmRunMode::kRealtime);
    assert(output.analysis.result_mode == npm::NpmResultMode::kFinal);

    status = npm::ParseNpmBasicTaskConfig(
        R"JSON({"input_namespace":"limits","source_domains":"4294967295:18446744073709551615"})JSON",
        &output);
    assert(status.error == npm::NpmBasicTaskConfigError::kNone);
    assert(output.domains.bindings[0].source_id == UINT32_MAX);
    assert(output.domains.bindings[0].observation_domain_id == UINT64_MAX);
}

void TestNpmBasicTaskConfigRejectsMalformedFieldsAtomically() {
    ParseTaskConfigFailure(nullptr, npm::NpmBasicTaskConfigError::kNullInput);
    ParseTaskConfigFailure("", npm::NpmBasicTaskConfigError::kEmptyInput);
    npm::NpmBasicTaskConfigStatus status;
    status = npm::ParseNpmBasicTaskConfig("{}", nullptr);
    assert(status.error == npm::NpmBasicTaskConfigError::kNullOutput);
    ParseTaskConfigFailure("{", npm::NpmBasicTaskConfigError::kInvalidJson);
    ParseTaskConfigFailure("[]", npm::NpmBasicTaskConfigError::kInvalidJson);
    ParseTaskConfigFailure(
        R"JSON({"input_namespace":"a","input_namespace":"b","source_domains":"0:0"})JSON",
        npm::NpmBasicTaskConfigError::kDuplicateField,
        "input_namespace");
    ParseTaskConfigFailure(
        R"JSON({"input_namespace":"a","source_domains":"0:0","mystery":"x"})JSON",
        npm::NpmBasicTaskConfigError::kUnknownField,
        "mystery");
    ParseTaskConfigFailure(
        R"JSON({"input_namespace":7,"source_domains":"0:0"})JSON",
        npm::NpmBasicTaskConfigError::kNonStringValue,
        "input_namespace");
    ParseTaskConfigFailure(
        R"JSON({"source_domains":"0:0"})JSON",
        npm::NpmBasicTaskConfigError::kMissingRequiredField,
        "input_namespace");
    ParseTaskConfigFailure(
        R"JSON({"input_namespace":"a"})JSON",
        npm::NpmBasicTaskConfigError::kMissingRequiredField,
        "source_domains");

    ParseTaskConfigFailure(
        R"JSON({"input_namespace":"a","source_domains":"0:0","run_mode":"batch"})JSON",
        npm::NpmBasicTaskConfigError::kInvalidEnum,
        "run_mode");
    ParseTaskConfigFailure(
        R"JSON({"input_namespace":"a","source_domains":"0:0","result_mode":"latest"})JSON",
        npm::NpmBasicTaskConfigError::kInvalidEnum,
        "result_mode");
    ParseTaskConfigFailure(
        R"JSON({"input_namespace":"a","source_domains":"0:0","overload_policy":"drop"})JSON",
        npm::NpmBasicTaskConfigError::kInvalidEnum,
        "overload_policy");
    ParseTaskConfigFailure(
        R"JSON({"input_namespace":"a","source_domains":"0:0","output_interval_ns":"-1"})JSON",
        npm::NpmBasicTaskConfigError::kInvalidInteger,
        "output_interval_ns");
    ParseTaskConfigFailure(
        R"JSON({"input_namespace":"a","source_domains":"0:0","payload_sample_packets":"4294967296"})JSON",
        npm::NpmBasicTaskConfigError::kInvalidInteger,
        "payload_sample_packets");
    ParseTaskConfigFailure(
        R"JSON({"input_namespace":"a","source_domains":"0:0","max_tracked_bytes":"18446744073709551616"})JSON",
        npm::NpmBasicTaskConfigError::kInvalidInteger,
        "max_tracked_bytes");
}

void TestNpmBasicTaskConfigRejectsMappingsAndRanges() {
    const std::vector<std::string> invalid_domains = {
        "",          "0",     ":1",          "1:",
        "1:2;",      "1::2",  " 1:2",        "1:2 ",
        "+1:2",      "1:-2",  "4294967296:1", "1:18446744073709551616",
    };
    for (const auto& domains : invalid_domains) {
        const std::string json =
            R"JSON({"input_namespace":"a","source_domains":")JSON" + domains + R"JSON("})JSON";
        ParseTaskConfigFailure(
            json.c_str(), npm::NpmBasicTaskConfigError::kInvalidSourceDomains, "source_domains");
    }

    auto status = ParseTaskConfigFailure(
        R"JSON({"input_namespace":"a","source_domains":"1:2;1:3"})JSON",
        npm::NpmBasicTaskConfigError::kDomainValidationError,
        "source_domains");
    assert(status.domain_error == npm::NpmObservationDomainError::kDuplicateSourceId);

    status = ParseTaskConfigFailure(
        R"JSON({"input_namespace":"","source_domains":"1:2"})JSON",
        npm::NpmBasicTaskConfigError::kDomainValidationError,
        "input_namespace");
    assert(status.domain_error == npm::NpmObservationDomainError::kEmptyInputNamespace);

    status = ParseTaskConfigFailure(
        R"JSON({"input_namespace":"a","source_domains":"1:2","output_interval_ns":"1"})JSON",
        npm::NpmBasicTaskConfigError::kAnalysisValidationError,
        "output_interval_ns");
    assert(status.analysis_error == npm::NpmAnalysisConfigError::kOutputIntervalOutOfRange);
    status = ParseTaskConfigFailure(
        R"JSON({"input_namespace":"a","source_domains":"1:2","max_active_sessions":"0"})JSON",
        npm::NpmBasicTaskConfigError::kAnalysisValidationError,
        "max_active_sessions");
    assert(status.analysis_error == npm::NpmAnalysisConfigError::kActiveSessionsOutOfRange);
}

void TestNpmBasicTaskConfigNumericBoundaries() {
    struct NumericBoundary {
        const char* field;
        uint64_t minimum;
        uint64_t maximum;
        npm::NpmAnalysisConfigError range_error;
    };
    const NumericBoundary boundaries[] = {
        {"output_interval_ns", npm::kNpmMinOutputIntervalNs, npm::kNpmMaxOutputIntervalNs,
         npm::NpmAnalysisConfigError::kOutputIntervalOutOfRange},
        {"payload_sample_packets", npm::kNpmMinPayloadSamplePackets, npm::kNpmMaxPayloadSamplePackets,
         npm::NpmAnalysisConfigError::kPayloadSamplePacketsOutOfRange},
        {"tcp_idle_timeout_ns", npm::kNpmMinIdleTimeoutNs, npm::kNpmMaxIdleTimeoutNs,
         npm::NpmAnalysisConfigError::kTcpIdleTimeoutOutOfRange},
        {"udp_idle_timeout_ns", npm::kNpmMinIdleTimeoutNs, npm::kNpmMaxIdleTimeoutNs,
         npm::NpmAnalysisConfigError::kUdpIdleTimeoutOutOfRange},
        {"out_of_order_tolerance_ns", npm::kNpmMinOutOfOrderToleranceNs, npm::kNpmMaxOutOfOrderToleranceNs,
         npm::NpmAnalysisConfigError::kOutOfOrderToleranceOutOfRange},
        {"max_active_sessions", npm::kNpmMinActiveSessions, npm::kNpmMaxActiveSessions,
         npm::NpmAnalysisConfigError::kActiveSessionsOutOfRange},
        {"max_tracked_bytes", npm::kNpmMinTrackedBytes, npm::kNpmMaxTrackedBytes,
         npm::NpmAnalysisConfigError::kTrackedBytesOutOfRange},
        {"max_pending_output_bytes", npm::kNpmMinPendingOutputBytes, npm::kNpmMaxPendingOutputBytes,
         npm::NpmAnalysisConfigError::kPendingOutputBytesOutOfRange},
    };
    const auto make_json = [](const char* field, const std::string& value) {
        return std::string(R"JSON({"input_namespace":"a","source_domains":"0:0",")JSON") + field +
               R"JSON(":")JSON" + value + R"JSON("})JSON";
    };
    for (const auto& boundary : boundaries) {
        for (const uint64_t value : {boundary.minimum, boundary.maximum}) {
            npm::NpmBasicTaskConfig output;
            const auto json = make_json(boundary.field, std::to_string(value));
            const auto status = npm::ParseNpmBasicTaskConfig(json.c_str(), &output);
            assert(status.error == npm::NpmBasicTaskConfigError::kNone);
        }
        std::vector<uint64_t> invalid_values = {boundary.maximum + 1};
        if (boundary.minimum > 0) invalid_values.push_back(boundary.minimum - 1);
        for (const uint64_t value : invalid_values) {
            const auto json = make_json(boundary.field, std::to_string(value));
            const auto status = ParseTaskConfigFailure(
                json.c_str(), npm::NpmBasicTaskConfigError::kAnalysisValidationError, boundary.field);
            assert(status.analysis_error == boundary.range_error);
        }
        for (const char* text : {"", "+1", "-1", " 1", "1 ", "1.0", "1e3", "abc"}) {
            const auto json = make_json(boundary.field, text);
            ParseTaskConfigFailure(
                json.c_str(), npm::NpmBasicTaskConfigError::kInvalidInteger, boundary.field);
        }
    }
    const auto json = make_json("tcp_idle_timeout_ns", "9223372036854775808");
    ParseTaskConfigFailure(json.c_str(), npm::NpmBasicTaskConfigError::kInvalidInteger, "tcp_idle_timeout_ns");
}

void AssertTaskBudgetUsage(const npm::NpmBudgetUsage& usage,
                           uint64_t session,
                           uint64_t module,
                           uint64_t input,
                           uint64_t output) {
    assert(usage.session_state_bytes == session);
    assert(usage.module_state_bytes == module);
    assert(usage.input_batch_bytes == input);
    assert(usage.pending_output_bytes == output);
}

void TestNpmTaskBudgetLimitsAtomicityAndIsolation() {
    static_assert(std::is_final_v<npm::NpmTaskBudget>);
    static_assert(!std::is_copy_constructible_v<npm::NpmTaskBudget>);
    static_assert(!std::is_copy_assignable_v<npm::NpmTaskBudget>);
    static_assert(!std::is_move_constructible_v<npm::NpmTaskBudget>);
    static_assert(!std::is_move_assignable_v<npm::NpmTaskBudget>);

    auto config = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline);
    config.max_tracked_bytes = npm::kNpmMinTrackedBytes;
    config.max_pending_output_bytes = npm::kNpmMinPendingOutputBytes;
    npm::NpmTaskBudget budget(config);
    config.max_tracked_bytes = npm::kNpmMaxTrackedBytes;
    config.max_pending_output_bytes = npm::kNpmMaxPendingOutputBytes;
    AssertTaskBudgetUsage(budget.Usage(), 0, 0, 0, 0);

    assert(budget.Reserve(npm::NpmBudgetCategory::kSessionState, npm::kNpmMinTrackedBytes - 2) ==
           npm::NpmBudgetError::kNone);
    assert(budget.Reserve(npm::NpmBudgetCategory::kModuleState, 1) == npm::NpmBudgetError::kNone);
    assert(budget.Reserve(npm::NpmBudgetCategory::kInputBatch, 1) == npm::NpmBudgetError::kNone);
    AssertTaskBudgetUsage(budget.Usage(), npm::kNpmMinTrackedBytes - 2, 1, 1, 0);
    assert(budget.Reserve(npm::NpmBudgetCategory::kSessionState, 1) ==
           npm::NpmBudgetError::kTrackedLimitExceeded);
    AssertTaskBudgetUsage(budget.Usage(), npm::kNpmMinTrackedBytes - 2, 1, 1, 0);

    assert(budget.Reserve(npm::NpmBudgetCategory::kPendingOutput, npm::kNpmMinPendingOutputBytes) ==
           npm::NpmBudgetError::kNone);
    assert(budget.Reserve(npm::NpmBudgetCategory::kPendingOutput, 1) ==
           npm::NpmBudgetError::kPendingOutputLimitExceeded);
    AssertTaskBudgetUsage(
        budget.Usage(), npm::kNpmMinTrackedBytes - 2, 1, 1, npm::kNpmMinPendingOutputBytes);

    assert(budget.Release(npm::NpmBudgetCategory::kInputBatch, 2) ==
           npm::NpmBudgetError::kReleaseUnderflow);
    assert(budget.Reserve(static_cast<npm::NpmBudgetCategory>(99), 1) ==
           npm::NpmBudgetError::kInvalidCategory);
    assert(budget.Release(static_cast<npm::NpmBudgetCategory>(99), 1) ==
           npm::NpmBudgetError::kInvalidCategory);
    AssertTaskBudgetUsage(
        budget.Usage(), npm::kNpmMinTrackedBytes - 2, 1, 1, npm::kNpmMinPendingOutputBytes);

    assert(budget.Release(npm::NpmBudgetCategory::kSessionState, npm::kNpmMinTrackedBytes - 2) ==
           npm::NpmBudgetError::kNone);
    assert(budget.Release(npm::NpmBudgetCategory::kModuleState, 1) == npm::NpmBudgetError::kNone);
    assert(budget.Release(npm::NpmBudgetCategory::kInputBatch, 1) == npm::NpmBudgetError::kNone);
    assert(budget.Release(npm::NpmBudgetCategory::kPendingOutput, npm::kNpmMinPendingOutputBytes) ==
           npm::NpmBudgetError::kNone);
    AssertTaskBudgetUsage(budget.Usage(), 0, 0, 0, 0);

    npm::NpmTaskBudget independent(config);
    assert(independent.Reserve(npm::NpmBudgetCategory::kSessionState, npm::kNpmMaxTrackedBytes) ==
           npm::NpmBudgetError::kNone);
    AssertTaskBudgetUsage(budget.Usage(), 0, 0, 0, 0);
    AssertTaskBudgetUsage(independent.Usage(), npm::kNpmMaxTrackedBytes, 0, 0, 0);
}

void TestNpmTaskBudgetConcurrentAccountingAndSharedLifetime() {
    const auto config = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline);
    auto budget = std::make_shared<npm::NpmTaskBudget>(config);
    std::weak_ptr<npm::NpmTaskBudget> weak_budget = budget;
    std::atomic<bool> valid{true};
    std::vector<std::thread> threads;
    for (int index = 0; index < 8; ++index) {
        threads.emplace_back([budget, &valid]() {
            for (int iteration = 0; iteration < 2000; ++iteration) {
                if (budget->Reserve(npm::NpmBudgetCategory::kSessionState, 1) != npm::NpmBudgetError::kNone) {
                    valid.store(false);
                    return;
                }
                if (budget->Usage().session_state_bytes == 0 ||
                    budget->Release(npm::NpmBudgetCategory::kSessionState, 1) != npm::NpmBudgetError::kNone ||
                    budget->Reserve(npm::NpmBudgetCategory::kPendingOutput, 1) != npm::NpmBudgetError::kNone ||
                    budget->Usage().pending_output_bytes == 0 ||
                    budget->Release(npm::NpmBudgetCategory::kPendingOutput, 1) != npm::NpmBudgetError::kNone) {
                    valid.store(false);
                    return;
                }
            }
        });
    }
    budget.reset();
    assert(!weak_budget.expired());
    for (auto& thread : threads) thread.join();
    assert(valid.load());
    assert(weak_budget.expired());

    npm::NpmTaskBudget empty(config);
    AssertTaskBudgetUsage(empty.Usage(), 0, 0, 0, 0);
}

npm::NpmBasicTaskConfig MakeRuntimeTaskConfig(npm::NpmRunMode mode = npm::NpmRunMode::kOffline) {
    npm::NpmBasicTaskConfig config;
    config.analysis = npm::DefaultNpmAnalysisConfig(mode);
    config.domains.input_namespace = "pcapfile.capture";
    config.domains.bindings = {{0, 77}, {1, 77}};
    return config;
}

std::unique_ptr<npm::NpmBasicTaskRuntime> CreateRuntimeForTest(
    const npm::NpmBasicTaskConfig& config,
    flowsql::IQuerier* querier) {
    std::shared_ptr<arrow::Schema> output_schema;
    std::unique_ptr<npm::NpmBasicTaskRuntime> runtime;
    const auto status = npm::NpmBasicTaskRuntime::Create(
        config, querier, flowsql::packet::PacketSchema(), &output_schema, &runtime);
    assert(status.error == npm::NpmBasicTaskRuntimeError::kNone);
    assert(output_schema != nullptr && output_schema->Equals(*npm::NpmBasicResultSchema(), true));
    assert(runtime != nullptr);
    return runtime;
}

npm::NpmTimeCapabilities AllRealtimeTimeCapabilities() {
    npm::NpmTimeCapabilities capabilities;
    capabilities.monotonic_time_drive = true;
    capabilities.capture_time_progress = true;
    capabilities.source_idle_confirmation = true;
    capabilities.source_backlog_state = true;
    return capabilities;
}

std::unique_ptr<npm::NpmBasicTaskRuntime> CreateRealtimeRuntimeForTest(
    const npm::NpmBasicTaskConfig& config,
    flowsql::IQuerier* querier) {
    std::shared_ptr<arrow::Schema> output_schema;
    std::unique_ptr<npm::NpmBasicTaskRuntime> runtime;
    const auto status = npm::NpmBasicTaskRuntime::CreateWithTimeCapabilities(
        config,
        querier,
        flowsql::packet::PacketSchema(),
        AllRealtimeTimeCapabilities(),
        &output_schema,
        &runtime);
    assert(status.error == npm::NpmBasicTaskRuntimeError::kNone);
    assert(output_schema != nullptr && output_schema->Equals(*npm::NpmBasicResultSchema(), true));
    assert(runtime != nullptr);
    return runtime;
}

void TestNpmBasicTaskRuntimeCreatesExclusiveInitialState() {
    static_assert(std::is_final_v<npm::NpmBasicTaskRuntime>);
    static_assert(!std::is_copy_constructible_v<npm::NpmBasicTaskRuntime>);
    static_assert(!std::is_copy_assignable_v<npm::NpmBasicTaskRuntime>);
    static_assert(!std::is_move_constructible_v<npm::NpmBasicTaskRuntime>);
    static_assert(!std::is_move_assignable_v<npm::NpmBasicTaskRuntime>);

    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    auto config = MakeRuntimeTaskConfig();
    std::shared_ptr<arrow::Schema> output_schema;
    std::unique_ptr<npm::NpmBasicTaskRuntime> runtime;
    auto status = npm::NpmBasicTaskRuntime::Create(
        config, &querier, flowsql::packet::PacketSchema(), &output_schema, &runtime);
    assert(status.error == npm::NpmBasicTaskRuntimeError::kNone);
    assert(status.time_error == npm::NpmTimeCapabilityError::kNone);
    assert(status.protocol_error == npm::NpmProtocolContextError::kNone);
    assert(output_schema != nullptr && output_schema->Equals(*npm::NpmBasicResultSchema(), true));
    assert(runtime != nullptr && pool.acquire_calls == 1 && pool.release_calls == 0);
    assert(runtime->Config().analysis.run_mode == npm::NpmRunMode::kOffline);
    assert(runtime->Config().analysis.result_mode == npm::NpmResultMode::kFinal);
    assert(runtime->Config().domains.input_namespace == "pcapfile.capture");
    assert(runtime->Config().domains.bindings.size() == 2);
    assert(runtime->ProtocolContext().Pipeno() == 3);
    AssertTaskBudgetUsage(runtime->Budget()->Usage(), 0, 0, 0, 0);
    assert(runtime->Sessions().size() == 0);
    assert(runtime->Collector().pending_results() == 0);
    assert(runtime->Projector().tracked_sessions() == 0);
    assert(runtime->EofFlusher().state() == npm::NpmEofFlushState::kOpen);
    assert(runtime->Modules().empty());

    auto* original_runtime = runtime.get();
    auto sentinel_schema = arrow::schema({arrow::field("sentinel", arrow::int8())});
    const auto original_sentinel_schema = sentinel_schema;
    const auto wrong_schema = arrow::schema({arrow::field("wrong", arrow::int64(), false)});
    status = npm::NpmBasicTaskRuntime::Create(
        config, &querier, wrong_schema, &sentinel_schema, &runtime);
    assert(status.error == npm::NpmBasicTaskRuntimeError::kSchemaMismatch);
    assert(runtime.get() == original_runtime && sentinel_schema == original_sentinel_schema);
    assert(pool.acquire_calls == 1 && pool.release_calls == 0);

    config.analysis.result_mode = npm::NpmResultMode::kPeriodicSnapshot;
    config.domains.input_namespace = "mutated";
    config.domains.bindings.clear();
    assert(runtime->Config().analysis.result_mode == npm::NpmResultMode::kFinal);
    assert(runtime->Config().domains.input_namespace == "pcapfile.capture");
    assert(runtime->Config().domains.bindings.size() == 2);

    const auto packet = MakeIpv6UdpPacket("2001:db8::1", 53000, "2001:db8::2", 53, {});
    flowsql::packet::PacketMeta meta;
    const auto binding = BuildBinding(runtime->Config().domains, packet, 0, 10, 80, &meta);
    npm::NpmSessionView observed;
    assert(ObserveActive(runtime->Sessions(), binding, meta, &observed) == npm::NpmSessionTableError::kNone);
    const uint64_t runtime_session_bytes = runtime->Sessions().tracked_bytes();
    assert(runtime_session_bytes != 0);
    AssertTaskBudgetUsage(runtime->Budget()->Usage(), runtime_session_bytes, 0, 0, 0);

    std::unique_ptr<npm::NpmBasicTaskRuntime> blocked;
    std::shared_ptr<arrow::Schema> blocked_schema = arrow::schema({arrow::field("sentinel", arrow::int8())});
    const auto original_schema = blocked_schema;
    status = npm::NpmBasicTaskRuntime::Create(
        MakeRuntimeTaskConfig(), &querier, flowsql::packet::PacketSchema(), &blocked_schema, &blocked);
    assert(status.error == npm::NpmBasicTaskRuntimeError::kProtocolContextError);
    assert(status.protocol_error == npm::NpmProtocolContextError::kPipelineExhausted);
    assert(blocked == nullptr && blocked_schema == original_schema);
    assert(pool.acquire_calls == 2 && pool.release_calls == 0);

    auto retained_budget = runtime->Budget();
    std::weak_ptr<npm::INpmTaskBudget> weak_budget = retained_budget;
    runtime.reset();
    assert(pool.release_calls == 1 && !weak_budget.expired());
    AssertTaskBudgetUsage(retained_budget->Usage(), 0, 0, 0, 0);
    retained_budget.reset();
    assert(weak_budget.expired());

    status = npm::NpmBasicTaskRuntime::Create(
        MakeRuntimeTaskConfig(), &querier, flowsql::packet::PacketSchema(), &blocked_schema, &blocked);
    assert(status.error == npm::NpmBasicTaskRuntimeError::kNone);
    assert(blocked != nullptr && pool.acquire_calls == 3 && pool.release_calls == 1);
    blocked.reset();
    assert(pool.release_calls == 2);
}

void TestNpmBasicTaskRuntimeRejectsOpenFailuresAtomically() {
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    auto config = MakeRuntimeTaskConfig();

    std::shared_ptr<arrow::Schema> output_schema = arrow::schema({arrow::field("sentinel", arrow::int8())});
    const auto original_schema = output_schema;
    std::unique_ptr<npm::NpmBasicTaskRuntime> runtime;
    auto status = npm::NpmBasicTaskRuntime::Create(config, &querier, nullptr, &output_schema, &runtime);
    assert(status.error == npm::NpmBasicTaskRuntimeError::kNullInputSchema);
    assert(output_schema == original_schema && runtime == nullptr && pool.acquire_calls == 0);

    const auto wrong_schema = arrow::schema({arrow::field("wrong", arrow::int64(), false)});
    status = npm::NpmBasicTaskRuntime::Create(config, &querier, wrong_schema, &output_schema, &runtime);
    assert(status.error == npm::NpmBasicTaskRuntimeError::kSchemaMismatch);
    assert(output_schema == original_schema && runtime == nullptr && pool.acquire_calls == 0);

    const auto packet_without_metadata = arrow::schema(flowsql::packet::PacketSchema()->fields());
    status = npm::NpmBasicTaskRuntime::Create(
        config, &querier, packet_without_metadata, &output_schema, &runtime);
    assert(status.error == npm::NpmBasicTaskRuntimeError::kSchemaMismatch);
    assert(output_schema == original_schema && runtime == nullptr && pool.acquire_calls == 0);

    status = npm::NpmBasicTaskRuntime::Create(
        config, &querier, flowsql::packet::PacketSchema(), nullptr, &runtime);
    assert(status.error == npm::NpmBasicTaskRuntimeError::kNullOutputSchema);
    assert(runtime == nullptr && pool.acquire_calls == 0);
    status = npm::NpmBasicTaskRuntime::Create(
        config, &querier, flowsql::packet::PacketSchema(), &output_schema, nullptr);
    assert(status.error == npm::NpmBasicTaskRuntimeError::kNullRuntimeOutput);
    assert(output_schema == original_schema && pool.acquire_calls == 0);

    config = MakeRuntimeTaskConfig(npm::NpmRunMode::kRealtime);
    status = npm::NpmBasicTaskRuntime::Create(
        config, &querier, flowsql::packet::PacketSchema(), &output_schema, &runtime);
    assert(status.error == npm::NpmBasicTaskRuntimeError::kTimeCapabilityError);
    assert(status.time_error == npm::NpmTimeCapabilityError::kMissingMonotonicTimeDrive);
    assert(output_schema == original_schema && runtime == nullptr && pool.acquire_calls == 0);

    config = MakeRuntimeTaskConfig();
    status = npm::NpmBasicTaskRuntime::Create(
        config, nullptr, flowsql::packet::PacketSchema(), &output_schema, &runtime);
    assert(status.error == npm::NpmBasicTaskRuntimeError::kProtocolContextError);
    assert(status.protocol_error == npm::NpmProtocolContextError::kNullQuerier);
    assert(output_schema == original_schema && runtime == nullptr && pool.acquire_calls == 0);

    SinglePoolQuerier missing_querier(nullptr);
    status = npm::NpmBasicTaskRuntime::Create(
        config, &missing_querier, flowsql::packet::PacketSchema(), &output_schema, &runtime);
    assert(status.error == npm::NpmBasicTaskRuntimeError::kProtocolContextError);
    assert(status.protocol_error == npm::NpmProtocolContextError::kProviderNotFound);
    assert(output_schema == original_schema && runtime == nullptr);

    ContextPool exhausted_pool(&protocol, flowsql::ProtocolPipelinePoolError::kExhausted);
    SinglePoolQuerier exhausted_querier(&exhausted_pool);
    status = npm::NpmBasicTaskRuntime::Create(
        config, &exhausted_querier, flowsql::packet::PacketSchema(), &output_schema, &runtime);
    assert(status.error == npm::NpmBasicTaskRuntimeError::kProtocolContextError);
    assert(status.protocol_error == npm::NpmProtocolContextError::kPipelineExhausted);
    assert(output_schema == original_schema && runtime == nullptr);
    assert(exhausted_pool.acquire_calls == 1 && exhausted_pool.release_calls == 0);

    ContextPool unavailable_pool(&protocol, flowsql::ProtocolPipelinePoolError::kUnavailable);
    SinglePoolQuerier unavailable_querier(&unavailable_pool);
    status = npm::NpmBasicTaskRuntime::Create(
        config, &unavailable_querier, flowsql::packet::PacketSchema(), &output_schema, &runtime);
    assert(status.error == npm::NpmBasicTaskRuntimeError::kProtocolContextError);
    assert(status.protocol_error == npm::NpmProtocolContextError::kPipelineUnavailable);
    assert(output_schema == original_schema && runtime == nullptr);
    assert(unavailable_pool.acquire_calls == 1 && unavailable_pool.release_calls == 0);

    ContextPool no_protocol_pool(nullptr);
    SinglePoolQuerier no_protocol_querier(&no_protocol_pool);
    status = npm::NpmBasicTaskRuntime::Create(
        config, &no_protocol_querier, flowsql::packet::PacketSchema(), &output_schema, &runtime);
    assert(status.error == npm::NpmBasicTaskRuntimeError::kProtocolContextError);
    assert(status.protocol_error == npm::NpmProtocolContextError::kProtocolUnavailable);
    assert(output_schema == original_schema && runtime == nullptr);
    assert(no_protocol_pool.acquire_calls == 0 && no_protocol_pool.release_calls == 0);

    ContextProtocol no_dictionary_protocol(nullptr);
    ContextPool no_dictionary_pool(&no_dictionary_protocol);
    SinglePoolQuerier no_dictionary_querier(&no_dictionary_pool);
    status = npm::NpmBasicTaskRuntime::Create(
        config, &no_dictionary_querier, flowsql::packet::PacketSchema(), &output_schema, &runtime);
    assert(status.error == npm::NpmBasicTaskRuntimeError::kProtocolContextError);
    assert(status.protocol_error == npm::NpmProtocolContextError::kDictionaryUnavailable);
    assert(output_schema == original_schema && runtime == nullptr);
    assert(no_dictionary_pool.acquire_calls == 0 && no_dictionary_pool.release_calls == 0);
}

void TestNpmBasicTaskRuntimeRealtimeCapabilityInjection() {
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    const auto config = MakeRuntimeTaskConfig(npm::NpmRunMode::kRealtime);
    auto sentinel_schema = arrow::schema({arrow::field("sentinel", arrow::int8())});
    const auto original_schema = sentinel_schema;
    std::unique_ptr<npm::NpmBasicTaskRuntime> runtime;

    auto status = npm::NpmBasicTaskRuntime::CreateWithTimeCapabilities(
        config, &querier, flowsql::packet::PacketSchema(), {}, &sentinel_schema, &runtime);
    assert(status.error == npm::NpmBasicTaskRuntimeError::kTimeCapabilityError);
    assert(status.time_error == npm::NpmTimeCapabilityError::kMissingMonotonicTimeDrive);
    assert(sentinel_schema == original_schema && runtime == nullptr && pool.acquire_calls == 0);

    status = npm::NpmBasicTaskRuntime::CreateWithTimeCapabilities(
        config,
        &querier,
        flowsql::packet::PacketSchema(),
        AllRealtimeTimeCapabilities(),
        &sentinel_schema,
        &runtime);
    assert(status.error == npm::NpmBasicTaskRuntimeError::kNone);
    assert(status.time_error == npm::NpmTimeCapabilityError::kNone);
    assert(sentinel_schema != original_schema && sentinel_schema->Equals(*npm::NpmBasicResultSchema(), true));
    assert(runtime != nullptr && runtime->Config().analysis.run_mode == npm::NpmRunMode::kRealtime);
    assert(pool.acquire_calls == 1 && pool.release_calls == 0);
}

npm::NpmBasicRealtimeMaintenanceInput RealtimeMaintenanceInput(
    int64_t monotonic_now_ns,
    int64_t observed_at_ns,
    int64_t capture_time_ns,
    bool packet_observed,
    bool source_idle_confirmed,
    bool source_backlog_known,
    bool source_has_backlog) {
    npm::NpmBasicRealtimeMaintenanceInput input;
    input.monotonic_now_ns = monotonic_now_ns;
    input.observed_at_ns = observed_at_ns;
    input.capture_progress.capture_time_ns = capture_time_ns;
    input.capture_progress.packet_observed = packet_observed;
    input.capture_progress.source_idle_confirmed = source_idle_confirmed;
    input.capture_progress.source_backlog_known = source_backlog_known;
    input.capture_progress.source_has_backlog = source_has_backlog;
    return input;
}

void TestNpmBasicTaskRuntimeRealtimeMaintenanceAndRevisions() {
    constexpr int64_t millisecond = 1'000'000;
    constexpr int64_t second = npm::kNpmNanosecondsPerSecond;
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    auto config = MakeRuntimeTaskConfig(npm::NpmRunMode::kRealtime);
    config.analysis.output_interval_ns = 10 * millisecond;
    config.analysis.tcp_idle_timeout_ns = second;
    config.analysis.udp_idle_timeout_ns = second;
    config.analysis.out_of_order_tolerance_ns = 0;
    auto runtime = CreateRealtimeRuntimeForTest(config, &querier);
    auto budget = runtime->Budget();

    const auto tcp = MakeIpv4TcpPacket("192.0.2.20", 50020, "192.0.2.2", 443, {});
    const auto udp = MakeIpv6UdpPacket("2001:db8::20", 53020, "2001:db8::2", 53, {});
    flowsql::packet::PacketMeta tcp_meta;
    flowsql::packet::PacketMeta udp_meta;
    const auto tcp_binding = BuildBinding(config.domains, tcp, 0, 0, 100, &tcp_meta);
    const auto udp_binding = BuildBinding(config.domains, udp, 0, 0, 80, &udp_meta);
    npm::NpmSessionView observed;
    assert(ObserveActive(runtime->Sessions(), tcp_binding, tcp_meta, &observed) ==
           npm::NpmSessionTableError::kNone);
    assert(observed.session_id == 1);
    assert(ObserveActive(runtime->Sessions(), udp_binding, udp_meta, &observed) ==
           npm::NpmSessionTableError::kNone);
    assert(observed.session_id == 2);
    const uint64_t session_bytes = runtime->Sessions().tracked_bytes();

    std::shared_ptr<arrow::RecordBatch> output = MakeNpmPacketViewBatch();
    auto original_output = output;
    auto status = runtime->DriveRealtimeMaintenance(
        RealtimeMaintenanceInput(100, 1'700'000'000'000'000'000LL, 0, true, false, true, false),
        &output);
    assert(status.error == npm::NpmBasicRealtimeMaintenanceError::kNone);
    assert(status.progress_disposition == npm::NpmCaptureProgressDisposition::kAdvanced);
    assert(!status.snapshot_due && !status.emitted && output == original_output);
    assert(runtime->Sessions().size() == 2 && runtime->Projector().tracked_sessions() == 0);

    status = runtime->DriveRealtimeMaintenance(
        RealtimeMaintenanceInput(10 * millisecond,
                                 1'700'000'000'010'000'000LL,
                                 0,
                                 false,
                                 false,
                                 true,
                                 true),
        &output);
    assert(status.error == npm::NpmBasicRealtimeMaintenanceError::kNone);
    assert(status.progress_disposition == npm::NpmCaptureProgressDisposition::kDeferredBacklogged);
    assert(!status.snapshot_due && !status.emitted && output == original_output);

    const int64_t first_due = 100 + 10 * millisecond;
    status = runtime->DriveRealtimeMaintenance(
        RealtimeMaintenanceInput(first_due,
                                 1'700'000'000'020'000'000LL,
                                 0,
                                 false,
                                 false,
                                 true,
                                 true),
        &output);
    assert(status.error == npm::NpmBasicRealtimeMaintenanceError::kNone);
    assert(status.snapshot_due && status.emitted);
    assert(status.active_sessions == 2 && status.ended_sessions == 0);
    assert(output != nullptr && output != original_output && output->num_rows() == 2);
    const auto first_ids = BasicResultColumn<arrow::UInt64Array>(output, 0);
    const auto first_revisions = BasicResultColumn<arrow::UInt64Array>(output, 2);
    const auto first_observed = BasicResultColumn<arrow::Int64Array>(output, 3);
    const auto first_final = BasicResultColumn<arrow::BooleanArray>(output, 4);
    const auto first_reasons = BasicResultColumn<arrow::StringArray>(output, 21);
    assert(first_ids->Value(0) == 1 && first_ids->Value(1) == 2);
    assert(first_revisions->Value(0) == 1 && first_revisions->Value(1) == 1);
    assert(first_observed->Value(0) == 1'700'000'000'020'000'000LL);
    assert(first_observed->Value(1) == 1'700'000'000'020'000'000LL);
    assert(!first_final->Value(0) && !first_final->Value(1));
    assert(first_reasons->IsNull(0) && first_reasons->IsNull(1));
    assert(runtime->Projector().tracked_sessions() == 2);
    output.reset();
    AssertTaskBudgetUsage(budget->Usage(), session_bytes, 0, 0, 0);

    output = original_output;
    status = runtime->DriveRealtimeMaintenance(
        RealtimeMaintenanceInput(first_due,
                                 1'700'000'000'021'000'000LL,
                                 0,
                                 false,
                                 false,
                                 true,
                                 true),
        &output);
    assert(status.error == npm::NpmBasicRealtimeMaintenanceError::kNone);
    assert(!status.snapshot_due && !status.emitted && output == original_output);

    tcp_meta.timestamp_ns = 500 * millisecond;
    tcp_meta.wire_len = 110;
    assert(ObserveActive(runtime->Sessions(), tcp_binding, tcp_meta, &observed) ==
           npm::NpmSessionTableError::kNone);
    assert(observed.session_id == 1 && observed.packets_ba + observed.packets_ab == 2);
    const int64_t coalesced_due = first_due + 10 * config.analysis.output_interval_ns;
    status = runtime->DriveRealtimeMaintenance(
        RealtimeMaintenanceInput(coalesced_due,
                                 1'700'000'000'100'000'000LL,
                                 500 * millisecond,
                                 true,
                                 false,
                                 true,
                                 false),
        &output);
    assert(status.error == npm::NpmBasicRealtimeMaintenanceError::kNone);
    assert(status.snapshot_due && status.emitted && output->num_rows() == 2);
    const auto second_ids = BasicResultColumn<arrow::UInt64Array>(output, 0);
    const auto second_revisions = BasicResultColumn<arrow::UInt64Array>(output, 2);
    const auto second_packets_ab = BasicResultColumn<arrow::UInt64Array>(output, 13);
    const auto second_packets_ba = BasicResultColumn<arrow::UInt64Array>(output, 14);
    assert(second_ids->Value(0) == 1 && second_ids->Value(1) == 2);
    assert(second_revisions->Value(0) == 2 && second_revisions->Value(1) == 2);
    assert(second_packets_ab->Value(0) + second_packets_ba->Value(0) == 2);
    output.reset();
    AssertTaskBudgetUsage(budget->Usage(), runtime->Sessions().tracked_bytes(), 0, 0, 0);

    output = original_output;
    status = runtime->DriveRealtimeMaintenance(
        RealtimeMaintenanceInput(coalesced_due + 1,
                                 1'700'000'002'000'000'000LL,
                                 2 * second,
                                 false,
                                 true,
                                 true,
                                 false),
        &output);
    assert(status.error == npm::NpmBasicRealtimeMaintenanceError::kNone);
    assert(!status.snapshot_due && status.emitted);
    assert(status.active_sessions == 0 && status.ended_sessions == 2);
    assert(output != nullptr && output->num_rows() == 2);
    const auto final_revisions = BasicResultColumn<arrow::UInt64Array>(output, 2);
    const auto final_observed = BasicResultColumn<arrow::Int64Array>(output, 3);
    const auto final_flags = BasicResultColumn<arrow::BooleanArray>(output, 4);
    const auto final_protocol = BasicResultColumn<arrow::StringArray>(output, 17);
    const auto final_reasons = BasicResultColumn<arrow::StringArray>(output, 21);
    for (int64_t row = 0; row < output->num_rows(); ++row) {
        assert(final_revisions->Value(row) == 3);
        assert(final_observed->Value(row) == 1'700'000'002'000'000'000LL);
        assert(final_flags->Value(row));
        assert(final_protocol->GetString(row) == "unknown");
        assert(final_reasons->GetString(row) == "idle_timeout");
    }
    assert(runtime->Sessions().size() == 0 && runtime->Projector().tracked_sessions() == 0);
    output.reset();
    AssertTaskBudgetUsage(budget->Usage(), 0, 0, 0, 0);

    output = original_output;
    status = runtime->DriveRealtimeMaintenance(
        RealtimeMaintenanceInput(coalesced_due,
                                 1'700'000'002'001'000'000LL,
                                 2 * second,
                                 false,
                                 true,
                                 true,
                                 false),
        &output);
    assert(status.error == npm::NpmBasicRealtimeMaintenanceError::kMonotonicTimeRegression);
    assert(status.runtime_state == npm::NpmEofFlushState::kFailed);
    assert(output == original_output && runtime->State() == npm::NpmEofFlushState::kFailed);
    assert(!runtime->LastError().empty() && pool.release_calls == 1);
}

void TestNpmBasicRealtimeTaskIsolationAndSlowSinkBudget() {
    constexpr int64_t interval_ns = 10'000'000;
    constexpr int64_t monotonic_start_ns = 100;
    constexpr int64_t observed_start_ns = 1'700'000'000'000'000'000LL;
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    DualContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);

    auto config_a = MakeRuntimeTaskConfig(npm::NpmRunMode::kRealtime);
    config_a.analysis.output_interval_ns = interval_ns;
    config_a.analysis.max_pending_output_bytes = npm::kNpmMinPendingOutputBytes;
    config_a.domains.input_namespace = "pcapfile.live-a";
    config_a.domains.bindings = {{0, 701}};
    auto config_b = config_a;
    config_b.domains.input_namespace = "pcapfile.live-b";
    config_b.domains.bindings = {{0, 702}};

    auto runtime_a = CreateRealtimeRuntimeForTest(config_a, &querier);
    auto runtime_b = CreateRealtimeRuntimeForTest(config_b, &querier);
    auto budget_a = runtime_a->Budget();
    auto budget_b = runtime_b->Budget();
    assert(pool.acquire_calls == 2 && pool.release_calls == 0);
    assert(runtime_a->ProtocolContext().Pipeno() != runtime_b->ProtocolContext().Pipeno());
    assert(budget_a != budget_b);

    const auto packet =
        MakeIpv4TcpPacket("192.0.2.30", 50030, "198.51.100.30", 443, {});
    flowsql::packet::PacketMeta meta_a;
    flowsql::packet::PacketMeta meta_b;
    const auto binding_a = BuildBinding(config_a.domains, packet, 0, 100, 90, &meta_a);
    const auto binding_b = BuildBinding(config_b.domains, packet, 0, 100, 110, &meta_b);
    npm::NpmSessionView session_a;
    npm::NpmSessionView session_b;
    assert(ObserveActive(runtime_a->Sessions(), binding_a, meta_a, &session_a) ==
           npm::NpmSessionTableError::kNone);
    assert(ObserveActive(runtime_b->Sessions(), binding_b, meta_b, &session_b) ==
           npm::NpmSessionTableError::kNone);
    assert(session_a.session_id == 1 && session_b.session_id == 1);
    assert(session_a.key->input_namespace == "pcapfile.live-a");
    assert(session_b.key->input_namespace == "pcapfile.live-b");
    assert(session_a.key->observation_domain_id == 701);
    assert(session_b.key->observation_domain_id == 702);
    const uint64_t session_bytes_a = runtime_a->Sessions().tracked_bytes();
    const uint64_t session_bytes_b = runtime_b->Sessions().tracked_bytes();

    std::shared_ptr<arrow::RecordBatch> slow_output_a;
    std::shared_ptr<arrow::RecordBatch> slow_output_b;
    auto status_a = runtime_a->DriveRealtimeMaintenance(
        RealtimeMaintenanceInput(monotonic_start_ns,
                                 observed_start_ns,
                                 100,
                                 true,
                                 false,
                                 true,
                                 false),
        &slow_output_a);
    auto status_b = runtime_b->DriveRealtimeMaintenance(
        RealtimeMaintenanceInput(monotonic_start_ns,
                                 observed_start_ns,
                                 100,
                                 true,
                                 false,
                                 true,
                                 false),
        &slow_output_b);
    assert(status_a.error == npm::NpmBasicRealtimeMaintenanceError::kNone);
    assert(status_b.error == npm::NpmBasicRealtimeMaintenanceError::kNone);
    assert(!status_a.emitted && !status_b.emitted && !slow_output_a && !slow_output_b);

    const int64_t first_due_ns = monotonic_start_ns + interval_ns;
    status_a = runtime_a->DriveRealtimeMaintenance(
        RealtimeMaintenanceInput(first_due_ns,
                                 observed_start_ns + interval_ns,
                                 100,
                                 false,
                                 false,
                                 true,
                                 true),
        &slow_output_a);
    status_b = runtime_b->DriveRealtimeMaintenance(
        RealtimeMaintenanceInput(first_due_ns,
                                 observed_start_ns + interval_ns,
                                 100,
                                 false,
                                 false,
                                 true,
                                 true),
        &slow_output_b);
    assert(status_a.error == npm::NpmBasicRealtimeMaintenanceError::kNone && status_a.emitted);
    assert(status_b.error == npm::NpmBasicRealtimeMaintenanceError::kNone && status_b.emitted);
    assert(slow_output_a && slow_output_a->num_rows() == 1);
    assert(slow_output_b && slow_output_b->num_rows() == 1);
    assert(BasicResultColumn<arrow::UInt64Array>(slow_output_a, 0)->Value(0) == 1);
    assert(BasicResultColumn<arrow::UInt64Array>(slow_output_b, 0)->Value(0) == 1);
    assert(BasicResultColumn<arrow::UInt64Array>(slow_output_a, 1)->Value(0) == 701);
    assert(BasicResultColumn<arrow::UInt64Array>(slow_output_b, 1)->Value(0) == 702);
    assert(BasicResultColumn<arrow::UInt64Array>(slow_output_a, 2)->Value(0) == 1);
    assert(BasicResultColumn<arrow::UInt64Array>(slow_output_b, 2)->Value(0) == 1);
    assert(!BasicResultColumn<arrow::BooleanArray>(slow_output_a, 4)->Value(0));
    assert(!BasicResultColumn<arrow::BooleanArray>(slow_output_b, 4)->Value(0));

    const uint64_t output_bytes_a = BasicResultBufferBytes(slow_output_a);
    const uint64_t output_bytes_b = BasicResultBufferBytes(slow_output_b);
    assert(output_bytes_a > 0 && output_bytes_a < config_a.analysis.max_pending_output_bytes);
    assert(output_bytes_b > 0 && output_bytes_b < config_b.analysis.max_pending_output_bytes);
    AssertTaskBudgetUsage(budget_a->Usage(), session_bytes_a, 0, 0, output_bytes_a);
    AssertTaskBudgetUsage(budget_b->Usage(), session_bytes_b, 0, 0, output_bytes_b);

    const uint64_t backlog_bytes = config_a.analysis.max_pending_output_bytes - output_bytes_a;
    assert(budget_a->Reserve(npm::NpmBudgetCategory::kPendingOutput, backlog_bytes) ==
           npm::NpmBudgetError::kNone);
    assert(budget_a->Reserve(npm::NpmBudgetCategory::kPendingOutput, 1) ==
           npm::NpmBudgetError::kPendingOutputLimitExceeded);
    AssertTaskBudgetUsage(
        budget_a->Usage(), session_bytes_a, 0, 0, config_a.analysis.max_pending_output_bytes);

    auto blocked_output = MakeNpmPacketViewBatch();
    const auto original_blocked_output = blocked_output;
    status_a = runtime_a->DriveRealtimeMaintenance(
        RealtimeMaintenanceInput(first_due_ns + interval_ns,
                                 observed_start_ns + 2 * interval_ns,
                                 100,
                                 false,
                                 false,
                                 true,
                                 true),
        &blocked_output);
    assert(status_a.error == npm::NpmBasicRealtimeMaintenanceError::kDrainError);
    assert(status_a.runtime_state == npm::NpmEofFlushState::kFailed);
    assert(status_a.drain_status.error == npm::NpmBasicDrainError::kEncodeError);
    assert(status_a.drain_status.encode_error == npm::NpmBasicEncodeError::kBudgetError);
    assert(blocked_output == original_blocked_output);
    assert(runtime_a->State() == npm::NpmEofFlushState::kFailed);
    assert(!runtime_a->LastError().empty() && pool.release_calls == 1);
    AssertTaskBudgetUsage(
        budget_a->Usage(), 0, 0, 0, config_a.analysis.max_pending_output_bytes);

    std::shared_ptr<arrow::RecordBatch> second_output_b;
    status_b = runtime_b->DriveRealtimeMaintenance(
        RealtimeMaintenanceInput(first_due_ns + interval_ns,
                                 observed_start_ns + 2 * interval_ns,
                                 100,
                                 false,
                                 false,
                                 true,
                                 true),
        &second_output_b);
    assert(status_b.error == npm::NpmBasicRealtimeMaintenanceError::kNone && status_b.emitted);
    assert(status_b.runtime_state == npm::NpmEofFlushState::kOpen);
    assert(second_output_b && second_output_b->num_rows() == 1);
    assert(BasicResultColumn<arrow::UInt64Array>(second_output_b, 0)->Value(0) == 1);
    assert(BasicResultColumn<arrow::UInt64Array>(second_output_b, 1)->Value(0) == 702);
    assert(BasicResultColumn<arrow::UInt64Array>(second_output_b, 2)->Value(0) == 2);
    assert(pool.release_calls == 1);
    const uint64_t second_output_bytes_b = BasicResultBufferBytes(second_output_b);
    AssertTaskBudgetUsage(
        budget_b->Usage(), session_bytes_b, 0, 0, output_bytes_b + second_output_bytes_b);

    assert(budget_a->Release(npm::NpmBudgetCategory::kPendingOutput, backlog_bytes) ==
           npm::NpmBudgetError::kNone);
    AssertTaskBudgetUsage(budget_a->Usage(), 0, 0, 0, output_bytes_a);
    slow_output_a.reset();
    AssertTaskBudgetUsage(budget_a->Usage(), 0, 0, 0, 0);

    runtime_b->Cancel();
    assert(runtime_b->State() == npm::NpmEofFlushState::kCancelled);
    assert(pool.release_calls == 2);
    AssertTaskBudgetUsage(
        budget_b->Usage(), 0, 0, 0, output_bytes_b + second_output_bytes_b);
    slow_output_b.reset();
    second_output_b.reset();
    AssertTaskBudgetUsage(budget_b->Usage(), 0, 0, 0, 0);
}

void TestNpmBasicTaskRuntimeProcessesAndDrainsOfflineBatch() {
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    std::shared_ptr<arrow::Schema> output_schema;
    std::unique_ptr<npm::NpmBasicTaskRuntime> runtime;
    const auto create_status = npm::NpmBasicTaskRuntime::Create(
        MakeRuntimeTaskConfig(), &querier, flowsql::packet::PacketSchema(), &output_schema, &runtime);
    assert(create_status.error == npm::NpmBasicTaskRuntimeError::kNone && runtime != nullptr);

    const auto rst =
        MakeIpv4TcpPacket("192.0.2.1", 41000, "198.51.100.2", 443, {}, kTcpRst, 91);
    auto input = MakeEncodedPacketBatch({MakeBatchPacketRecord(rst, 0, 100, 1)});
    const uint64_t input_bytes = BasicResultBufferBytes(input);
    std::weak_ptr<arrow::RecordBatch> input_owner = input;
    std::shared_ptr<arrow::RecordBatch> output = MakeNpmPacketViewBatch();

    auto status = runtime->ProcessOfflineBatch(input, &output);
    assert(status.error == npm::NpmBasicOfflineBatchError::kNone);
    assert(status.runtime_state == npm::NpmEofFlushState::kOpen);
    assert(status.input_bytes == input_bytes);
    assert(status.budget_error == npm::NpmBudgetError::kNone);
    assert(status.batch_view_error == npm::NpmPacketBatchError::kNone);
    assert(status.process_status.error == npm::NpmPacketBatchProcessError::kNone);
    assert(status.drain_status.error == npm::NpmBasicDrainError::kNone);
    assert(output != nullptr && output->schema()->Equals(*npm::NpmBasicResultSchema(), true));
    assert(output->num_rows() == 1 && output->num_columns() == 22);
    assert(BasicResultColumn<arrow::BooleanArray>(output, 4)->Value(0));
    assert(BasicResultColumn<arrow::StringArray>(output, 17)->GetString(0) == "unknown");
    assert(BasicResultColumn<arrow::StringArray>(output, 21)->GetString(0) == "closed");
    assert(runtime->Sessions().size() == 0 && runtime->Collector().pending_results() == 0);
    assert(runtime->Projector().tracked_sessions() == 0);

    const uint64_t output_bytes = BasicResultBufferBytes(output);
    AssertTaskBudgetUsage(runtime->Budget()->Usage(), 0, 0, 0, output_bytes);
    input.reset();
    assert(input_owner.expired());
    assert(BasicResultColumn<arrow::StringArray>(output, 21)->GetString(0) == "closed");
    output.reset();
    AssertTaskBudgetUsage(runtime->Budget()->Usage(), 0, 0, 0, 0);

    auto empty_input = MakeEncodedPacketBatch({});
    status = runtime->ProcessOfflineBatch(empty_input, &output);
    assert(status.error == npm::NpmBasicOfflineBatchError::kNone);
    assert(status.runtime_state == npm::NpmEofFlushState::kOpen);
    assert(output != nullptr && output->schema()->Equals(*npm::NpmBasicResultSchema(), true));
    assert(output->num_rows() == 0 && output->num_columns() == 22);
    output.reset();
    AssertTaskBudgetUsage(runtime->Budget()->Usage(), 0, 0, 0, 0);
}

void TestNpmBasicTaskRuntimeUsesExactOfflineInputBudget() {
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    auto config = MakeRuntimeTaskConfig();
    config.analysis.max_tracked_bytes = npm::kNpmMinTrackedBytes;
    auto runtime = CreateRuntimeForTest(config, &querier);

    const auto packet =
        MakeIpv4TcpPacket("192.0.2.1", 41000, "198.51.100.2", 443, {0x11}, kTcpAck, 92);
    auto input = MakeEncodedPacketBatch({MakeBatchPacketRecord(packet, 99, 100, 2)});
    const uint64_t input_bytes = BasicResultBufferBytes(input);
    assert(input_bytes > 0 && input_bytes < config.analysis.max_tracked_bytes);
    auto budget = runtime->Budget();
    const uint64_t exact_existing_bytes = config.analysis.max_tracked_bytes - input_bytes;
    assert(budget->Reserve(npm::NpmBudgetCategory::kModuleState, exact_existing_bytes) ==
           npm::NpmBudgetError::kNone);
    std::shared_ptr<arrow::RecordBatch> output = MakeNpmPacketViewBatch();
    auto original_output = output;

    auto status = runtime->ProcessOfflineBatch(input, &output);
    assert(status.error == npm::NpmBasicOfflineBatchError::kBatchProcessError);
    assert(status.runtime_state == npm::NpmEofFlushState::kFailed);
    assert(status.input_bytes == input_bytes && status.budget_error == npm::NpmBudgetError::kNone);
    assert(status.process_status.error == npm::NpmPacketBatchProcessError::kPacketError);
    assert(status.process_status.row == 0);
    assert(status.process_status.packet_status.error == npm::NpmPacketProcessError::kBindingError);
    assert(status.process_status.packet_status.binding_error ==
           npm::NpmSessionPacketError::kUnknownSourceId);
    assert(output == original_output && runtime->State() == npm::NpmEofFlushState::kFailed);
    assert(!runtime->LastError().empty() && pool.release_calls == pool.acquire_calls);
    AssertTaskBudgetUsage(budget->Usage(), 0, exact_existing_bytes, 0, 0);
    assert(budget->Release(npm::NpmBudgetCategory::kModuleState, exact_existing_bytes) ==
           npm::NpmBudgetError::kNone);
    auto flush_status = runtime->FlushOffline(200, &output);
    assert(flush_status.error == npm::NpmEofFlushError::kFailedState);
    assert(output == original_output);

    runtime.reset();
    runtime = CreateRuntimeForTest(config, &querier);
    budget = runtime->Budget();
    const uint64_t overflowing_existing_bytes = exact_existing_bytes + 1;
    assert(budget->Reserve(npm::NpmBudgetCategory::kModuleState, overflowing_existing_bytes) ==
           npm::NpmBudgetError::kNone);
    status = runtime->ProcessOfflineBatch(input, &output);
    assert(status.error == npm::NpmBasicOfflineBatchError::kInputBudgetError);
    assert(status.runtime_state == npm::NpmEofFlushState::kFailed);
    assert(status.input_bytes == input_bytes);
    assert(status.budget_error == npm::NpmBudgetError::kTrackedLimitExceeded);
    assert(status.process_status.error == npm::NpmPacketBatchProcessError::kNone);
    assert(output == original_output && runtime->State() == npm::NpmEofFlushState::kFailed);
    assert(!runtime->LastError().empty() && pool.release_calls == pool.acquire_calls);
    AssertTaskBudgetUsage(budget->Usage(), 0, overflowing_existing_bytes, 0, 0);
    assert(budget->Release(npm::NpmBudgetCategory::kModuleState, overflowing_existing_bytes) ==
           npm::NpmBudgetError::kNone);
}

void TestNpmBasicTaskRuntimeRejectsOfflineBatchFailuresAtomically() {
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    const auto config = MakeRuntimeTaskConfig();
    auto runtime = CreateRuntimeForTest(config, &querier);

    const auto packet =
        MakeIpv4TcpPacket("192.0.2.1", 41000, "198.51.100.2", 443, {0x11}, kTcpAck, 93);
    auto input = MakeEncodedPacketBatch({MakeBatchPacketRecord(packet, 0, 100, 3)});
    std::shared_ptr<arrow::RecordBatch> output = MakeNpmPacketViewBatch();
    const auto original_output = output;
    const auto assert_failed_terminal = [&]() {
        assert(runtime->State() == npm::NpmEofFlushState::kFailed);
        const auto first_error = runtime->LastError();
        assert(!first_error.empty() && pool.release_calls == pool.acquire_calls);
        const auto retry = runtime->ProcessOfflineBatch(input, &output);
        assert(retry.error == npm::NpmBasicOfflineBatchError::kTerminalState);
        assert(retry.runtime_state == npm::NpmEofFlushState::kFailed);
        const auto flush = runtime->FlushOffline(1000, &output);
        assert(flush.error == npm::NpmEofFlushError::kFailedState);
        runtime->Cancel();
        assert(output == original_output && runtime->LastError() == first_error);
    };
    const auto reset_runtime = [&]() {
        runtime.reset();
        runtime = CreateRuntimeForTest(config, &querier);
    };

    auto status = runtime->ProcessOfflineBatch(nullptr, &output);
    assert(status.error == npm::NpmBasicOfflineBatchError::kNullInput && status.input_bytes == 0);
    assert(status.runtime_state == npm::NpmEofFlushState::kFailed && output == original_output);
    assert_failed_terminal();

    reset_runtime();
    status = runtime->ProcessOfflineBatch(input, nullptr);
    assert(status.error == npm::NpmBasicOfflineBatchError::kNullOutput && status.input_bytes == 0);
    assert(status.runtime_state == npm::NpmEofFlushState::kFailed);
    AssertTaskBudgetUsage(runtime->Budget()->Usage(), 0, 0, 0, 0);
    assert_failed_terminal();

    reset_runtime();
    auto schema_without_metadata = arrow::schema(input->schema()->fields());
    auto wrong_input =
        arrow::RecordBatch::Make(schema_without_metadata, input->num_rows(), input->columns());
    const uint64_t wrong_input_bytes = BasicResultBufferBytes(wrong_input);
    status = runtime->ProcessOfflineBatch(wrong_input, &output);
    assert(status.error == npm::NpmBasicOfflineBatchError::kBatchViewError);
    assert(status.runtime_state == npm::NpmEofFlushState::kFailed);
    assert(status.input_bytes == wrong_input_bytes);
    assert(status.batch_view_error == npm::NpmPacketBatchError::kSchemaMismatch);
    assert(output == original_output);
    AssertTaskBudgetUsage(runtime->Budget()->Usage(), 0, 0, 0, 0);
    assert_failed_terminal();

    reset_runtime();
    auto budget = runtime->Budget();
    const uint64_t output_limit = runtime->Config().analysis.max_pending_output_bytes;
    assert(budget->Reserve(npm::NpmBudgetCategory::kPendingOutput, output_limit) ==
           npm::NpmBudgetError::kNone);
    const auto rst =
        MakeIpv4TcpPacket("192.0.2.1", 41000, "198.51.100.2", 443, {}, kTcpRst, 94);
    auto drain_failure_input =
        MakeEncodedPacketBatch({MakeBatchPacketRecord(rst, 0, 200, 4)});
    status = runtime->ProcessOfflineBatch(drain_failure_input, &output);
    assert(status.error == npm::NpmBasicOfflineBatchError::kDrainError);
    assert(status.runtime_state == npm::NpmEofFlushState::kFailed);
    assert(status.drain_status.error == npm::NpmBasicDrainError::kEncodeError);
    assert(status.drain_status.encode_error == npm::NpmBasicEncodeError::kBudgetError);
    assert(output == original_output);
    AssertTaskBudgetUsage(budget->Usage(), 0, 0, 0, output_limit);
    assert(budget->Release(npm::NpmBudgetCategory::kPendingOutput, output_limit) ==
           npm::NpmBudgetError::kNone);
    assert_failed_terminal();
}

void TestNpmBasicTaskRuntimeFlushesOfflineExactlyOnce() {
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    const auto config = MakeRuntimeTaskConfig();
    auto runtime = CreateRuntimeForTest(config, &querier);
    auto budget = runtime->Budget();

    const auto packet =
        MakeIpv4TcpPacket("192.0.2.1", 41000, "198.51.100.2", 443, {0x11}, kTcpAck, 95);
    auto input = MakeEncodedPacketBatch({MakeBatchPacketRecord(packet, 0, 100, 5)});
    std::shared_ptr<arrow::RecordBatch> output;
    const auto process_status = runtime->ProcessOfflineBatch(input, &output);
    assert(process_status.error == npm::NpmBasicOfflineBatchError::kNone);
    assert(process_status.runtime_state == npm::NpmEofFlushState::kOpen);
    assert(output != nullptr && output->num_rows() == 0 && runtime->Sessions().size() == 1);
    const uint64_t session_bytes = runtime->Sessions().tracked_bytes();
    assert(session_bytes > 0);
    AssertTaskBudgetUsage(
        budget->Usage(), session_bytes, 0, 0, BasicResultBufferBytes(output));
    output.reset();
    AssertTaskBudgetUsage(budget->Usage(), session_bytes, 0, 0, 0);

    auto flush_status = runtime->FlushOffline(500, &output);
    assert(flush_status.error == npm::NpmEofFlushError::kNone);
    assert(runtime->State() == npm::NpmEofFlushState::kFlushed);
    assert(runtime->LastError().empty() && pool.release_calls == 1);
    assert(output != nullptr && output->schema()->Equals(*npm::NpmBasicResultSchema(), true));
    assert(output->num_rows() == 1 && output->num_columns() == 22);
    assert(BasicResultColumn<arrow::Int64Array>(output, 3)->Value(0) == 500);
    assert(BasicResultColumn<arrow::BooleanArray>(output, 4)->Value(0));
    assert(BasicResultColumn<arrow::StringArray>(output, 21)->GetString(0) == "eof");
    AssertTaskBudgetUsage(budget->Usage(), 0, 0, 0, BasicResultBufferBytes(output));

    auto original_output = output;
    flush_status = runtime->FlushOffline(600, &output);
    assert(flush_status.error == npm::NpmEofFlushError::kAlreadyFlushed);
    assert(output == original_output);
    const auto terminal_process = runtime->ProcessOfflineBatch(input, &output);
    assert(terminal_process.error == npm::NpmBasicOfflineBatchError::kTerminalState);
    assert(terminal_process.runtime_state == npm::NpmEofFlushState::kFlushed);
    assert(output == original_output);

    output.reset();
    assert(budget->Usage().pending_output_bytes > 0);
    runtime.reset();
    assert(pool.release_calls == 1);
    assert(budget->Usage().pending_output_bytes > 0);
    original_output.reset();
    AssertTaskBudgetUsage(budget->Usage(), 0, 0, 0, 0);

    runtime = CreateRuntimeForTest(config, &querier);
    flush_status = runtime->FlushOffline(700, &output);
    assert(flush_status.error == npm::NpmEofFlushError::kNone);
    assert(runtime->State() == npm::NpmEofFlushState::kFlushed);
    assert(runtime->LastError().empty() && pool.release_calls == 2);
    assert(output != nullptr && output->schema()->Equals(*npm::NpmBasicResultSchema(), true));
    assert(output->num_rows() == 0 && output->num_columns() == 22);
}

void TestNpmBasicTaskRuntimeEofFailureIsTerminal() {
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    const auto config = MakeRuntimeTaskConfig();
    auto runtime = CreateRuntimeForTest(config, &querier);

    const auto packet =
        MakeIpv4TcpPacket("192.0.2.1", 41000, "198.51.100.2", 443, {0x11}, kTcpAck, 96);
    auto input = MakeEncodedPacketBatch({MakeBatchPacketRecord(packet, 0, 100, 6)});
    std::shared_ptr<arrow::RecordBatch> batch_output;
    const auto process_status = runtime->ProcessOfflineBatch(input, &batch_output);
    assert(process_status.error == npm::NpmBasicOfflineBatchError::kNone);
    assert(runtime->Sessions().size() == 1);
    batch_output.reset();

    auto budget = runtime->Budget();
    const uint64_t output_limit = config.analysis.max_pending_output_bytes;
    assert(budget->Reserve(npm::NpmBudgetCategory::kPendingOutput, output_limit) ==
           npm::NpmBudgetError::kNone);
    std::shared_ptr<arrow::RecordBatch> output = MakeNpmPacketViewBatch();
    const auto original_output = output;
    auto flush_status = runtime->FlushOffline(500, &output);
    assert(flush_status.error == npm::NpmEofFlushError::kDrainError);
    assert(flush_status.drain_status.error == npm::NpmBasicDrainError::kEncodeError);
    assert(flush_status.drain_status.encode_error == npm::NpmBasicEncodeError::kBudgetError);
    assert(runtime->State() == npm::NpmEofFlushState::kFailed);
    const auto first_error = runtime->LastError();
    assert(!first_error.empty() && output == original_output);
    assert(pool.release_calls == 1);
    AssertTaskBudgetUsage(budget->Usage(), 0, 0, 0, output_limit);

    flush_status = runtime->FlushOffline(600, &output);
    assert(flush_status.error == npm::NpmEofFlushError::kFailedState);
    const auto terminal_process = runtime->ProcessOfflineBatch(input, &output);
    assert(terminal_process.error == npm::NpmBasicOfflineBatchError::kTerminalState);
    assert(terminal_process.runtime_state == npm::NpmEofFlushState::kFailed);
    runtime->Cancel();
    assert(output == original_output && runtime->LastError() == first_error);
    assert(budget->Release(npm::NpmBudgetCategory::kPendingOutput, output_limit) ==
           npm::NpmBudgetError::kNone);

    runtime = CreateRuntimeForTest(config, &querier);
    flush_status = runtime->FlushOffline(700, nullptr);
    assert(flush_status.error == npm::NpmEofFlushError::kNullOutput);
    assert(runtime->State() == npm::NpmEofFlushState::kFailed);
    assert(runtime->LastError() == "npm.basic EOF output is null");
    assert(pool.release_calls == 2);
}

void TestNpmBasicTaskRuntimeConcurrentCancelIsNonBlockingAndStable() {
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    std::atomic<bool> identify_entered{false};
    std::atomic<bool> release_identify{false};
    protocol.identify_entered = &identify_entered;
    protocol.release_identify = &release_identify;
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    auto runtime = CreateRuntimeForTest(MakeRuntimeTaskConfig(), &querier);
    auto budget = runtime->Budget();
    assert(runtime->State() == npm::NpmEofFlushState::kOpen && runtime->LastError().empty());

    const auto packet =
        MakeIpv4TcpPacket("192.0.2.1", 41000, "198.51.100.2", 443, {0x11}, kTcpAck, 97);
    auto input = MakeEncodedPacketBatch({MakeBatchPacketRecord(packet, 0, 100, 7)});
    std::shared_ptr<arrow::RecordBatch> output = MakeNpmPacketViewBatch();
    const auto original_output = output;
    npm::NpmBasicOfflineBatchStatus process_status;
    std::atomic<bool> process_finished{false};
    std::thread worker([&]() {
        process_status = runtime->ProcessOfflineBatch(input, &output);
        process_finished.store(true, std::memory_order_release);
    });
    while (!identify_entered.load(std::memory_order_acquire)) std::this_thread::yield();

    runtime->Cancel();
    assert(runtime->State() == npm::NpmEofFlushState::kCancelled);
    const auto cancel_error = runtime->LastError();
    assert(!cancel_error.empty() && !process_finished.load(std::memory_order_acquire));
    runtime->Cancel();
    assert(runtime->LastError() == cancel_error);

    release_identify.store(true, std::memory_order_release);
    worker.join();
    assert(process_status.error == npm::NpmBasicOfflineBatchError::kCancelled);
    assert(process_status.runtime_state == npm::NpmEofFlushState::kCancelled);
    assert(output == original_output && pool.release_calls == 1);
    AssertTaskBudgetUsage(budget->Usage(), 0, 0, 0, 0);
    const auto flush_status = runtime->FlushOffline(500, &output);
    assert(flush_status.error == npm::NpmEofFlushError::kCancelled);
    assert(output == original_output && runtime->LastError() == cancel_error);
}

void TestNpmBasicTaskRuntimeCancelKeepsDeliveredOutputAlive() {
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    auto runtime = CreateRuntimeForTest(MakeRuntimeTaskConfig(), &querier);
    auto budget = runtime->Budget();

    const auto rst =
        MakeIpv4TcpPacket("192.0.2.1", 41000, "198.51.100.2", 443, {}, kTcpRst, 98);
    auto input = MakeEncodedPacketBatch({MakeBatchPacketRecord(rst, 0, 100, 8)});
    std::shared_ptr<arrow::RecordBatch> output;
    const auto status = runtime->ProcessOfflineBatch(input, &output);
    assert(status.error == npm::NpmBasicOfflineBatchError::kNone && output != nullptr);
    const uint64_t output_bytes = BasicResultBufferBytes(output);
    AssertTaskBudgetUsage(budget->Usage(), 0, 0, 0, output_bytes);

    runtime->Cancel();
    assert(runtime->State() == npm::NpmEofFlushState::kCancelled);
    assert(!runtime->LastError().empty() && pool.release_calls == 1);
    AssertTaskBudgetUsage(budget->Usage(), 0, 0, 0, output_bytes);
    assert(BasicResultColumn<arrow::StringArray>(output, 21)->GetString(0) == "closed");
    runtime.reset();
    assert(pool.release_calls == 1);
    AssertTaskBudgetUsage(budget->Usage(), 0, 0, 0, output_bytes);
    output.reset();
    AssertTaskBudgetUsage(budget->Usage(), 0, 0, 0, 0);
}

flowsql::BlockTransformTaskConfigV1 MakeOperatorTaskConfig(
    const std::string& task_id,
    const std::string& with_params_json,
    const std::string& pushed_filter_plan_json) {
    flowsql::BlockTransformTaskConfigV1 config;
    config.task_id = task_id.c_str();
    config.with_params_json = with_params_json.c_str();
    config.pushed_filter_plan_json = pushed_filter_plan_json.c_str();
    return config;
}

void TestNpmBasicOperatorCopiesConfigAndOwnsTasks() {
    static_assert(std::is_final_v<npm::NpmBasicOperator>);
    static_assert(std::is_final_v<npm::NpmBasicTask>);

    npm::NpmBasicOperator provider(nullptr);
    npm::NpmBasicOperator other_provider(nullptr);
    assert(provider.Category() == "npm");
    assert(provider.Name() == "basic");
    assert(!provider.Description().empty());

    std::string task_id = "task-owned-one";
    std::string with_json =
        R"({"input_namespace":"pcapfile.capture","source_domains":"0:77"})";
    std::string filter_plan = R"({"version":1,"root":null})";
    auto config = MakeOperatorTaskConfig(task_id, with_json, filter_plan);
    flowsql::IBlockTransformTaskV1* first = nullptr;
    assert(provider.CreateTask(config, &first) == 0 && first != nullptr);

    auto* concrete = dynamic_cast<npm::NpmBasicTask*>(first);
    assert(concrete != nullptr);
    task_id.assign("mutated-task");
    with_json.assign("mutated-with");
    filter_plan.assign("mutated-plan");
    assert(concrete->TaskId() == "task-owned-one");
    assert(concrete->WithParamsJson() ==
           R"({"input_namespace":"pcapfile.capture","source_domains":"0:77"})");
    assert(concrete->PushedFilterPlanJson() == R"({"version":1,"root":null})");

    other_provider.ReleaseTask(first);
    assert(concrete->TaskId() == "task-owned-one");

    task_id = "task-owned-two";
    with_json = R"({"input_namespace":"pcapfile.other","source_domains":"1:88"})";
    filter_plan = R"({"version":1,"root":null})";
    config = MakeOperatorTaskConfig(task_id, with_json, filter_plan);
    flowsql::IBlockTransformTaskV1* second = nullptr;
    assert(provider.CreateTask(config, &second) == 0);
    assert(second != nullptr && second != first);
    assert(dynamic_cast<npm::NpmBasicTask*>(second)->TaskId() == "task-owned-two");

    provider.ReleaseTask(nullptr);
    provider.ReleaseTask(first);
    provider.ReleaseTask(second);

    auto* sentinel = reinterpret_cast<flowsql::IBlockTransformTaskV1*>(1);
    config.contract_version = flowsql::kBlockTransformContractVersionV1 + 1;
    assert(provider.CreateTask(config, &sentinel) == EINVAL &&
           sentinel == reinterpret_cast<flowsql::IBlockTransformTaskV1*>(1));
    config.contract_version = flowsql::kBlockTransformContractVersionV1;
    config.task_id = nullptr;
    assert(provider.CreateTask(config, &sentinel) == EINVAL &&
           sentinel == reinterpret_cast<flowsql::IBlockTransformTaskV1*>(1));
    config.task_id = "";
    assert(provider.CreateTask(config, &sentinel) == EINVAL &&
           sentinel == reinterpret_cast<flowsql::IBlockTransformTaskV1*>(1));
    config.task_id = "valid";
    config.with_params_json = nullptr;
    assert(provider.CreateTask(config, &sentinel) == EINVAL &&
           sentinel == reinterpret_cast<flowsql::IBlockTransformTaskV1*>(1));
    config.with_params_json = "{}";
    config.pushed_filter_plan_json = "";
    assert(provider.CreateTask(config, &sentinel) == EINVAL &&
           sentinel == reinterpret_cast<flowsql::IBlockTransformTaskV1*>(1));
    assert(provider.CreateTask(config, nullptr) == EINVAL);
}

void TestNpmBasicTaskOpenProcessAndFlush() {
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    npm::NpmBasicOperator provider(&querier);
    const std::string task_id = "task-happy";
    const std::string with_json =
        R"({"input_namespace":"pcapfile.capture","source_domains":"0:77"})";
    const std::string filter_plan = R"({"version":1,"root":null})";
    const auto config = MakeOperatorTaskConfig(task_id, with_json, filter_plan);

    flowsql::IBlockTransformTaskV1* task = nullptr;
    assert(provider.CreateTask(config, &task) == 0 && task != nullptr);
    auto sentinel_schema = arrow::schema({arrow::field("sentinel", arrow::int8())});
    std::shared_ptr<arrow::Schema> output_schema = sentinel_schema;
    assert(task->Open(flowsql::packet::PacketSchema(), &output_schema) == 0);
    assert(output_schema && output_schema != sentinel_schema);
    assert(output_schema->Equals(*npm::NpmBasicResultSchema(), true));
    assert(pool.acquire_calls == 1 && pool.release_calls == 0 && task->LastError().empty());

    const auto packet =
        MakeIpv4TcpPacket("192.0.2.1", 41000, "198.51.100.2", 443, {}, kTcpAck, 101);
    auto first_input = MakeEncodedPacketBatch({MakeBatchPacketRecord(packet, 0, 100, 1)});
    std::vector<flowsql::BlockTransformOutputV1> outputs;
    assert(task->ProcessBlock(first_input, 11, &outputs) ==
           static_cast<int>(flowsql::BlockTransformStatusV1::kContinue));
    assert(outputs.size() == 1 && outputs[0].batch != nullptr);
    assert(outputs[0].batch->num_rows() == 0 && outputs[0].ts_ms == 11);
    outputs.clear();

    auto out_of_order_input =
        MakeEncodedPacketBatch({MakeBatchPacketRecord(packet, 0, 50, 2)});
    assert(task->ProcessBlock(out_of_order_input, 22, &outputs) ==
           static_cast<int>(flowsql::BlockTransformStatusV1::kContinue));
    assert(outputs.size() == 1 && outputs[0].batch->num_rows() == 0 &&
           outputs[0].ts_ms == 22);
    outputs.clear();

    assert(task->Flush(&outputs) == 0);
    assert(outputs.size() == 1 && outputs[0].batch != nullptr && outputs[0].ts_ms == 22);
    assert(outputs[0].batch->num_rows() == 1);
    assert(BasicResultColumn<arrow::Int64Array>(outputs[0].batch, 3)->Value(0) == 100);
    assert(BasicResultColumn<arrow::Int64Array>(outputs[0].batch, 12)->Value(0) == 100);
    assert(BasicResultColumn<arrow::StringArray>(outputs[0].batch, 21)->GetString(0) == "eof");
    assert(pool.release_calls == 1 && task->LastError().empty());

    auto retained_output = outputs[0].batch;
    outputs.clear();
    assert(task->Flush(&outputs) != 0 && outputs.empty());
    const auto terminal_error = task->LastError();
    assert(!terminal_error.empty());
    task->Cancel();
    assert(task->LastError() == terminal_error);
    provider.ReleaseTask(task);
    assert(retained_output->num_rows() == 1);
    assert(BasicResultColumn<arrow::StringArray>(retained_output, 21)->GetString(0) == "eof");
}

void TestNpmBasicTaskRejectsInvalidCallsAtomically() {
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    npm::NpmBasicOperator provider(&querier);
    const std::string task_id = "task-invalid";
    const std::string filter_plan = R"({"version":1,"root":null})";

    std::string with_json = "{";
    auto config = MakeOperatorTaskConfig(task_id, with_json, filter_plan);
    flowsql::IBlockTransformTaskV1* task = nullptr;
    assert(provider.CreateTask(config, &task) == 0);
    auto sentinel_schema = arrow::schema({arrow::field("sentinel", arrow::int8())});
    std::shared_ptr<arrow::Schema> output_schema = sentinel_schema;
    assert(task->Open(flowsql::packet::PacketSchema(), &output_schema) != 0);
    assert(output_schema == sentinel_schema);
    const auto config_error = task->LastError();
    assert(!config_error.empty());
    assert(task->Open(flowsql::packet::PacketSchema(), &output_schema) != 0);
    assert(output_schema == sentinel_schema && task->LastError() == config_error);
    provider.ReleaseTask(task);

    with_json = R"({"input_namespace":"pcapfile.capture","source_domains":"0:77"})";
    config = MakeOperatorTaskConfig(task_id, with_json, filter_plan);
    task = nullptr;
    assert(provider.CreateTask(config, &task) == 0);
    const auto wrong_schema = arrow::schema({arrow::field("wrong", arrow::int64())});
    output_schema = sentinel_schema;
    assert(task->Open(wrong_schema, &output_schema) != 0);
    assert(output_schema == sentinel_schema && !task->LastError().empty());
    provider.ReleaseTask(task);

    task = nullptr;
    assert(provider.CreateTask(config, &task) == 0);
    output_schema = sentinel_schema;
    assert(task->Open(flowsql::packet::PacketSchema(), &output_schema) == 0);
    std::shared_ptr<arrow::Schema> repeated_schema = sentinel_schema;
    assert(task->Open(nullptr, &repeated_schema) != 0);
    assert(repeated_schema == sentinel_schema);
    const auto repeated_error = task->LastError();
    assert(!repeated_error.empty() && pool.release_calls == 1);
    std::vector<flowsql::BlockTransformOutputV1> outputs;
    assert(task->ProcessBlock(nullptr, 0, &outputs) < 0 && outputs.empty());
    assert(task->LastError() == repeated_error);
    provider.ReleaseTask(task);

    task = nullptr;
    assert(provider.CreateTask(config, &task) == 0);
    output_schema = sentinel_schema;
    assert(task->Open(flowsql::packet::PacketSchema(), &output_schema) == 0);
    assert(task->ProcessBlock(nullptr, 0, &outputs) < 0 && outputs.empty());
    const auto process_error = task->LastError();
    assert(!process_error.empty() && pool.release_calls == 2);
    assert(task->Flush(&outputs) != 0 && outputs.empty());
    assert(task->LastError() == process_error);
    provider.ReleaseTask(task);
}

void TestNpmBasicTaskMethodPreconditions() {
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    npm::NpmBasicOperator provider(&querier);
    const std::string task_id = "task-preconditions";
    const std::string with_json =
        R"({"input_namespace":"pcapfile.capture","source_domains":"0:77"})";
    const std::string filter_plan = R"({"version":1,"root":null})";
    const auto config = MakeOperatorTaskConfig(task_id, with_json, filter_plan);
    const auto packet =
        MakeIpv4TcpPacket("192.0.2.1", 41000, "198.51.100.2", 443, {}, kTcpAck, 103);
    auto input = MakeEncodedPacketBatch({MakeBatchPacketRecord(packet, 0, 100, 4)});
    auto sentinel_schema = arrow::schema({arrow::field("sentinel", arrow::int8())});
    std::vector<flowsql::BlockTransformOutputV1> outputs;
    flowsql::IBlockTransformTaskV1* task = nullptr;

    assert(provider.CreateTask(config, &task) == 0);
    assert(task->ProcessBlock(input, 1, &outputs) < 0 && outputs.empty());
    const auto before_open_error = task->LastError();
    std::shared_ptr<arrow::Schema> output_schema = sentinel_schema;
    assert(task->Open(flowsql::packet::PacketSchema(), &output_schema) != 0);
    assert(output_schema == sentinel_schema && task->LastError() == before_open_error);
    provider.ReleaseTask(task);

    task = nullptr;
    assert(provider.CreateTask(config, &task) == 0);
    assert(task->Flush(&outputs) != 0 && outputs.empty());
    const auto before_open_flush_error = task->LastError();
    output_schema = sentinel_schema;
    assert(task->Open(flowsql::packet::PacketSchema(), &output_schema) != 0);
    assert(output_schema == sentinel_schema && task->LastError() == before_open_flush_error);
    provider.ReleaseTask(task);

    task = nullptr;
    assert(provider.CreateTask(config, &task) == 0);
    output_schema = sentinel_schema;
    assert(task->Open(nullptr, &output_schema) != 0);
    assert(output_schema == sentinel_schema && !task->LastError().empty());
    provider.ReleaseTask(task);

    task = nullptr;
    assert(provider.CreateTask(config, &task) == 0);
    assert(task->Open(flowsql::packet::PacketSchema(), nullptr) != 0);
    assert(!task->LastError().empty());
    provider.ReleaseTask(task);

    task = nullptr;
    assert(provider.CreateTask(config, &task) == 0);
    output_schema = sentinel_schema;
    assert(task->Open(flowsql::packet::PacketSchema(), &output_schema) == 0);
    outputs.push_back({MakeNpmPacketViewBatch(), 99});
    assert(task->ProcessBlock(input, 2, &outputs) < 0 && outputs.empty());
    assert(!task->LastError().empty() && pool.release_calls == 1);
    provider.ReleaseTask(task);

    task = nullptr;
    assert(provider.CreateTask(config, &task) == 0);
    assert(task->Open(flowsql::packet::PacketSchema(), &output_schema) == 0);
    outputs.push_back({MakeNpmPacketViewBatch(), 99});
    assert(task->Flush(&outputs) != 0 && outputs.empty());
    assert(!task->LastError().empty() && pool.release_calls == 2);
    provider.ReleaseTask(task);

    task = nullptr;
    assert(provider.CreateTask(config, &task) == 0);
    assert(task->Open(flowsql::packet::PacketSchema(), &output_schema) == 0);
    auto unknown_source = MakeEncodedPacketBatch({MakeBatchPacketRecord(packet, 1, 100, 5)});
    assert(task->ProcessBlock(unknown_source, 3, &outputs) < 0 && outputs.empty());
    const auto runtime_error = task->LastError();
    assert(!runtime_error.empty() && pool.release_calls == 3);
    assert(task->ProcessBlock(input, 4, &outputs) < 0 && outputs.empty());
    assert(task->LastError() == runtime_error);
    provider.ReleaseTask(task);

    task = nullptr;
    assert(provider.CreateTask(config, &task) == 0);
    assert(task->Open(flowsql::packet::PacketSchema(), &output_schema) == 0);
    assert(task->ProcessBlock(input, 5, nullptr) < 0);
    assert(!task->LastError().empty() && pool.release_calls == 4);
    provider.ReleaseTask(task);

    task = nullptr;
    assert(provider.CreateTask(config, &task) == 0);
    assert(task->Open(flowsql::packet::PacketSchema(), &output_schema) == 0);
    assert(task->Flush(nullptr) != 0);
    assert(!task->LastError().empty() && pool.release_calls == 5);
    provider.ReleaseTask(task);

    npm::NpmBasicOperator missing_dependency(nullptr);
    task = nullptr;
    assert(missing_dependency.CreateTask(config, &task) == 0);
    output_schema = sentinel_schema;
    assert(task->Open(flowsql::packet::PacketSchema(), &output_schema) != 0);
    assert(output_schema == sentinel_schema && !task->LastError().empty());
    missing_dependency.ReleaseTask(task);
}

void TestNpmBasicTaskCancelBeforeOpenAndDuringProcess() {
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    npm::NpmBasicOperator provider(&querier);
    const std::string task_id = "task-cancel";
    const std::string with_json =
        R"({"input_namespace":"pcapfile.capture","source_domains":"0:77"})";
    const std::string filter_plan = R"({"version":1,"root":null})";
    const auto config = MakeOperatorTaskConfig(task_id, with_json, filter_plan);

    flowsql::IBlockTransformTaskV1* task = nullptr;
    assert(provider.CreateTask(config, &task) == 0);
    task->Cancel();
    const auto preopen_error = task->LastError();
    assert(!preopen_error.empty());
    task->Cancel();
    assert(task->LastError() == preopen_error);
    auto sentinel_schema = arrow::schema({arrow::field("sentinel", arrow::int8())});
    std::shared_ptr<arrow::Schema> output_schema = sentinel_schema;
    assert(task->Open(flowsql::packet::PacketSchema(), &output_schema) != 0);
    assert(output_schema == sentinel_schema && task->LastError() == preopen_error);
    provider.ReleaseTask(task);

    std::atomic<bool> identify_entered{false};
    std::atomic<bool> release_identify{false};
    protocol.identify_entered = &identify_entered;
    protocol.release_identify = &release_identify;
    task = nullptr;
    assert(provider.CreateTask(config, &task) == 0);
    assert(task->Open(flowsql::packet::PacketSchema(), &output_schema) == 0);
    const auto packet =
        MakeIpv4TcpPacket("192.0.2.1", 41000, "198.51.100.2", 443, {0x11}, kTcpAck, 102);
    auto input = MakeEncodedPacketBatch({MakeBatchPacketRecord(packet, 0, 100, 3)});
    std::vector<flowsql::BlockTransformOutputV1> outputs;
    std::atomic<bool> process_finished{false};
    int process_rc = 0;
    std::thread worker([&]() {
        process_rc = task->ProcessBlock(input, 33, &outputs);
        process_finished.store(true, std::memory_order_release);
    });
    while (!identify_entered.load(std::memory_order_acquire)) std::this_thread::yield();

    task->Cancel();
    const auto concurrent_error = task->LastError();
    assert(!concurrent_error.empty() && !process_finished.load(std::memory_order_acquire));
    task->Cancel();
    assert(task->LastError() == concurrent_error);
    release_identify.store(true, std::memory_order_release);
    worker.join();
    assert(process_rc < 0 && outputs.empty());
    assert(task->LastError() == concurrent_error && pool.release_calls == 1);
    assert(task->Flush(&outputs) != 0 && outputs.empty());
    assert(task->LastError() == concurrent_error);
    provider.ReleaseTask(task);
}

void TestNpmBasicV2PluginExports() {
    void* handle = dlopen(FLOWSQL_NPM_BASIC_PLUGIN_PATH, RTLD_NOW | RTLD_LOCAL);
    assert(handle != nullptr);

    auto abi_version = reinterpret_cast<flowsql::CppOperatorPluginAbiVersionFn>(
        dlsym(handle, flowsql::kCppOperatorPluginAbiVersionSymbol));
    auto operator_count =
        reinterpret_cast<flowsql::CppOperatorPluginCountFn>(dlsym(handle, flowsql::kCppOperatorPluginCountSymbol));
    auto describe = reinterpret_cast<flowsql::CppOperatorPluginDescribeV2Fn>(
        dlsym(handle, flowsql::kCppOperatorPluginDescribeV2Symbol));
    auto create = reinterpret_cast<flowsql::CppOperatorPluginCreateCapabilityV2Fn>(
        dlsym(handle, flowsql::kCppOperatorPluginCreateCapabilityV2Symbol));
    auto destroy = reinterpret_cast<flowsql::CppOperatorPluginDestroyCapabilityV2Fn>(
        dlsym(handle, flowsql::kCppOperatorPluginDestroyCapabilityV2Symbol));
    assert(abi_version != nullptr && operator_count != nullptr && describe != nullptr);
    assert(create != nullptr && destroy != nullptr);
    assert(dlsym(handle, "pluginregist") == nullptr);
    assert(dlsym(handle, flowsql::kCppOperatorPluginCreateV1Symbol) == nullptr);
    assert(dlsym(handle, flowsql::kCppOperatorPluginDestroyV1Symbol) == nullptr);
    assert(abi_version() == flowsql::kCppOperatorPluginAbiVersionV2);
    assert(operator_count() == 1);

    flowsql::CppOperatorDescriptorV2 descriptor{};
    descriptor.struct_size = flowsql::kCppOperatorDescriptorV2Size;
    assert(describe(0, &descriptor) == 0);
    assert(descriptor.struct_size == flowsql::kCppOperatorDescriptorV2Size);
    assert(std::string(descriptor.category) == "npm");
    assert(std::string(descriptor.name) == "basic");
    assert(descriptor.description != nullptr && descriptor.description[0] != '\0');
    assert(SameGuid(descriptor.contract_iid, flowsql::IID_BLOCK_TRANSFORM_OPERATOR_V1));

    flowsql::CppOperatorDescriptorV2 rejected{};
    rejected.struct_size = flowsql::kCppOperatorDescriptorV2Size - 1;
    rejected.category = "sentinel";
    assert(describe(0, &rejected) == EINVAL);
    assert(rejected.struct_size == flowsql::kCppOperatorDescriptorV2Size - 1);
    assert(std::string(rejected.category) == "sentinel");
    assert(describe(-1, &descriptor) == EINVAL);
    assert(describe(1, &descriptor) == EINVAL);
    assert(describe(0, nullptr) == EINVAL);
    assert(create(-1, nullptr) == nullptr);
    assert(create(1, nullptr) == nullptr);

    SinglePoolQuerier missing_dependency(nullptr);
    assert(create(0, nullptr) == nullptr);
    assert(create(0, &missing_dependency) == nullptr);

    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    void* capability = create(0, &querier);
    assert(capability != nullptr);
    auto* provider = static_cast<flowsql::IBlockTransformOperatorV1*>(capability);
    assert(provider->Category() == "npm" && provider->Name() == "basic");

    const std::string task_id = "v2-capability";
    const std::string with_json =
        R"({"input_namespace":"pcapfile.capture","source_domains":"0:77"})";
    const std::string filter_plan = R"({"version":1,"root":null})";
    const auto config = MakeOperatorTaskConfig(task_id, with_json, filter_plan);
    flowsql::IBlockTransformTaskV1* task = nullptr;
    assert(provider->CreateTask(config, &task) == 0 && task != nullptr);
    std::shared_ptr<arrow::Schema> output_schema;
    assert(task->Open(flowsql::packet::PacketSchema(), &output_schema) == 0);
    assert(output_schema != nullptr && output_schema->Equals(*npm::NpmBasicResultSchema(), true));

    const auto rst =
        MakeIpv4TcpPacket("192.0.2.1", 41000, "198.51.100.2", 443, {}, kTcpRst, 104);
    auto input = MakeEncodedPacketBatch({MakeBatchPacketRecord(rst, 0, 100, 6)});
    std::vector<flowsql::BlockTransformOutputV1> outputs;
    assert(task->ProcessBlock(input, 44, &outputs) ==
           static_cast<int>(flowsql::BlockTransformStatusV1::kContinue));
    assert(outputs.size() == 1 && outputs[0].batch != nullptr && outputs[0].ts_ms == 44);
    assert(outputs[0].batch->num_rows() == 1);
    assert(BasicResultColumn<arrow::StringArray>(outputs[0].batch, 21)->GetString(0) == "closed");
    outputs.clear();
    assert(task->Flush(&outputs) == 0);
    assert(outputs.size() == 1 && outputs[0].batch != nullptr && outputs[0].batch->num_rows() == 0);
    provider->ReleaseTask(task);
    outputs.clear();
    output_schema.reset();

    destroy(0, capability);
    destroy(0, nullptr);
    assert(dlclose(handle) == 0);
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
    TestSessionTableActiveSnapshotIsOrderedAndNonMutating();
    TestSessionTableIsolationCapacityAndErrors();
    TestSessionTableBudgetReserveFailureAndStableCharge();
    TestSessionTableBudgetReleasesAllTerminalPaths();
    TestOfflineEventWatermarkAndIdleRetirement();
    TestIdleDeadlineReplacementLatePacketAndCapacityRelease();
    TestRealtimeProgressRequiresIdleAndBacklogEvidence();
    TestTcpSynRetransmissionAndTupleReuse();
    TestTcpFinRstAndUdpLifecycle();
    TestSessionEndModuleNotificationOrderAndErrors();
    TestProtocolSamplingSkipsEmptyPayloadAndStopsAfterHit();
    TestProtocolSamplingExhaustionAndSessionInstanceIsolation();
    TestProtocolPendingFinalizesUnknownOnIdle();
    TestSessionTableFinishAllAtEof();
    TestProcessNpmPacketSamplingAndCallbackOrder();
    TestProcessNpmPacketErrorsAreStructuredAndAtomic();
    TestProcessNpmOfflinePacketBatchOrderAndWatermark();
    TestProcessNpmOfflinePacketBatchErrorsAndAtomicOutput();
    TestNpmProtocolContextErrorsDictionaryAndRaii();
    TestNpiPipelinePoolOptionAndLeaseContract();
    TestNpmPacketBatchViewBorrowedDecodeAndOwnership();
    TestNpmPacketBatchViewRejectsSchemaAndColumnErrors();
    TestNpmPacketBatchViewRejectsInvalidRows();
    TestNpmBasicResultProjectionRevisionsAndLabels();
    TestNpmBasicResultProjectionErrorsDoNotConsumeRevision();
    TestNpmBasicResultArrowEncoding();
    TestNpmBasicResultArrowEncodingRejectsInvalidInput();
    TestNpmBasicResultCollectorCopiesAndDrainsUnifiedBatches();
    TestNpmBasicResultCollectorFailuresKeepPendingAndOutput();
    TestNpmEofFlusherSuccessEmptyAndRepeated();
    TestNpmEofFlusherTerminalAndErrorPaths();
    TestNpmBasicResultPendingOutputLease();
    TestNpmBasicResultPendingOutputBudgetAndLifetime();
    TestNpmBasicResultPendingOutputFailuresAreAtomic();
    TestNpmBasicTaskConfigParsesOwnedValues();
    TestNpmBasicTaskConfigRejectsMalformedFieldsAtomically();
    TestNpmBasicTaskConfigRejectsMappingsAndRanges();
    TestNpmBasicTaskConfigNumericBoundaries();
    TestNpmTaskBudgetLimitsAtomicityAndIsolation();
    TestNpmTaskBudgetConcurrentAccountingAndSharedLifetime();
    TestNpmBasicTaskRuntimeCreatesExclusiveInitialState();
    TestNpmBasicTaskRuntimeRejectsOpenFailuresAtomically();
    TestNpmBasicTaskRuntimeRealtimeCapabilityInjection();
    TestNpmBasicTaskRuntimeRealtimeMaintenanceAndRevisions();
    TestNpmBasicRealtimeTaskIsolationAndSlowSinkBudget();
    TestNpmBasicTaskRuntimeProcessesAndDrainsOfflineBatch();
    TestNpmBasicTaskRuntimeUsesExactOfflineInputBudget();
    TestNpmBasicTaskRuntimeRejectsOfflineBatchFailuresAtomically();
    TestNpmBasicTaskRuntimeFlushesOfflineExactlyOnce();
    TestNpmBasicTaskRuntimeEofFailureIsTerminal();
    TestNpmBasicTaskRuntimeConcurrentCancelIsNonBlockingAndStable();
    TestNpmBasicTaskRuntimeCancelKeepsDeliveredOutputAlive();
    TestNpmBasicOperatorCopiesConfigAndOwnsTasks();
    TestNpmBasicTaskOpenProcessAndFlush();
    TestNpmBasicTaskRejectsInvalidCallsAtomically();
    TestNpmBasicTaskMethodPreconditions();
    TestNpmBasicTaskCancelBeforeOpenAndDuringProcess();
    TestNpmBasicV2PluginExports();
    return 0;
}
