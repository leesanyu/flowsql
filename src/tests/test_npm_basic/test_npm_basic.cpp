// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <common/iplugin.h>
#include <common/network/netbase.h>
#include <framework/core/packet_codec.h>
#include <framework/interfaces/cpp_operator_plugin_abi.h>
#include <framework/interfaces/iflow_labeling.h>
#include <operators/npm_basic/config/npm_basic_task_config.h>
#include <operators/npm_basic/config/npm_parameters.h>
#include <operators/npm_basic/core/npm_basic_task_runtime.h>
#include <operators/npm_basic/core/npm_eof_flusher.h>
#include <operators/npm_basic/core/npm_packet_batch_view.h>
#include <operators/npm_basic/core/npm_packet_processor.h>
#include <operators/npm_basic/core/npm_protocol_context.h>
#include <operators/npm_basic/core/npm_session_key.h>
#include <operators/npm_basic/core/npm_session_table.h>
#include <operators/npm_basic/core/npm_task_budget.h>
#include <operators/npm_basic/modules/basic/npm_basic_result_projector.h>
#include <operators/npm_basic/modules/dns/npm_dns_contract.h>
#include <operators/npm_basic/modules/dns/npm_dns_module.h>
#include <operators/npm_basic/modules/http1/npm_http1_contract.h>
#include <operators/npm_basic/modules/http1/npm_http1_module.h>
#include <operators/npm_basic/modules/icmp/npm_icmp_contract.h>
#include <operators/npm_basic/modules/session/npm_session_analysis_module.h>
#include <operators/npm_basic/modules/session/npm_tcp_performance_tracker.h>
#include <operators/npm_basic/modules/tls/npm_tls_contract.h>
#include <operators/npm_basic/modules/tls/npm_tls_framer.h>
#include <operators/npm_basic/modules/tls/npm_tls_handshake.h>
#include <operators/npm_basic/modules/tls/npm_tls_module.h>
#include <operators/npm_basic/npm_analysis_contract.h>
#include <operators/npm_basic/npm_basic_operator.h>
#include <operators/npm_basic/output/npm_basic_result_collector.h>
#include <operators/npm_basic/output/npm_basic_result_encoder.h>
#include <operators/npm_basic/output/npm_session_result_encoder.h>
#include <plugins/npi/iprotocol.h>
#include <tests/support/scoped_shared_library.h>
#include <common/loader.hpp>

#include <arrow/api.h>
#include <arrow/util/byte_size.h>

#include <arpa/inet.h>
#include <dlfcn.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace npm = flowsql::npm;

void TestIcmpModuleAndBudget();

extern "C" flowsql::IPlugin* pluginregist(flowsql::IRegister* registry, const char* option);

namespace {

constexpr uint8_t kTcpFin = 0x01;
constexpr uint8_t kTcpSyn = 0x02;
constexpr uint8_t kTcpRst = 0x04;
constexpr uint8_t kTcpAck = 0x10;

int64_t ResultClockNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool SameGuid(const flowsql::Guid& left, const flowsql::Guid& right) { return !(left < right) && !(right < left); }

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
        if (SameGuid(iid, flowsql::IID_PROTOCOL_PIPELINE_POOL_V1)) return pool_;
        if (SameGuid(iid, flowsql::IID_FLOW_LABELING_PROVIDER_V1)) {
            ++labeling_queries;
            return labeling_provider;
        }
        if (SameGuid(iid, flowsql::IID_CONFIG_CHANNEL_REGISTRY_V1)) {
            ++config_registry_queries;
            return config_registry;
        }
        return nullptr;
    }

    flowsql::IFlowLabelingProviderV1* labeling_provider = nullptr;
    flowsql::IConfigChannelRegistryV1* config_registry = nullptr;
    uint32_t labeling_queries = 0;
    uint32_t config_registry_queries = 0;

 private:
    flowsql::IProtocolPipelinePoolV1* pool_ = nullptr;
};

class LabelingConfigRegistry final : public flowsql::IConfigChannelRegistryV1 {
 public:
    int Resolve(const char* exact_reference, flowsql::ConfigChannelSnapshot* snapshot, std::string* error) override {
        ++resolve_calls;
        last_reference = exact_reference == nullptr ? std::string() : exact_reference;
        if (!succeed || snapshot == nullptr || last_reference != "config.corp-labels@7") {
            if (error != nullptr) *error = "mock exact snapshot unavailable";
            return ENOENT;
        }
        flowsql::ConfigChannelSnapshot next;
        next.channel_name = "corp-labels";
        next.revision = 7;
        next.format = "yaml";
        next.schema_id = "flowsql.io/flow-labeling/v1alpha1";
        next.sha256_hex = "mock";
        next.content = std::make_shared<const std::string>("labels: []");
        next.content_bytes = next.content->size();
        *snapshot = std::move(next);
        if (error != nullptr) error->clear();
        return 0;
    }

    bool succeed = true;
    uint32_t resolve_calls = 0;
    std::string last_reference;
};

#ifdef FLOWSQL_FLOW_LABELING_PLUGIN_PATH
class RealLabelingSnapshotRegistry final : public flowsql::IConfigChannelRegistryV1 {
 public:
    int Resolve(const char* exact_reference, flowsql::ConfigChannelSnapshot* snapshot, std::string* error) override {
        ++resolve_calls;
        if (exact_reference == nullptr || std::strcmp(exact_reference, "config.corp-labels@7") != 0 ||
            snapshot == nullptr) {
            if (error != nullptr) *error = "unknown exact revision";
            return ENOENT;
        }
        flowsql::ConfigChannelSnapshot next;
        next.channel_name = "corp-labels";
        next.revision = 7;
        next.format = "yaml";
        next.schema_id = "flowsql.io/flow-labeling/v1alpha1";
        next.sha256_hex = "test";
        next.content = std::make_shared<const std::string>(R"(api_version: flowsql.io/flow-labeling/v1alpha1
kind: FlowLabelingSet
spec:
  engine:
    type: dpdk-acl
    categories: 1
    algorithm: scalar
    numa_socket_id: any
    max_runtime_bytes: 8388608
    limits:
      max_labels: 10000
      max_logical_rules: 50000
      max_compiled_rules: 100000
      max_expanded_fields: 64
      reject_duplicate_label_priorities: true
  labels:
    - id: 1001
      priority: 3000
      name: corp-web
  rules:
    - id: corp-web-port
      label_id: 1001
      direction: bidirectional
      matches:
        - field: destination_ipv4_prefix
          value: 198.51.100.2
          prefix_bits: 32
        - field: destination_port
          min: 443
          max: 443
)");
        next.content_bytes = next.content->size();
        *snapshot = std::move(next);
        if (error != nullptr) error->clear();
        return 0;
    }

    uint32_t resolve_calls = 0;
};
#endif

struct LabelingMatcherStats {
    uint32_t classify_calls = 0;
    uint32_t release_calls = 0;
    int classify_error = 0;
    int fail_call = -1;
    std::vector<uint32_t> batch_counts;
    std::vector<flowsql::FlowLabelFactsV1> facts;
    std::vector<std::vector<uint32_t>> labels_by_call;
};

class RecordingLabelMatcher final : public flowsql::IFlowLabelMatcherV1 {
 public:
    explicit RecordingLabelMatcher(LabelingMatcherStats* stats) : stats_(stats) {}

    int ClassifyBatch(const flowsql::FlowLabelFactsV1* facts, uint32_t count,
                      uint32_t* primary_label_ids) const override {
        assert(stats_ != nullptr && facts != nullptr && count != 0 && primary_label_ids != nullptr);
        const size_t call_index = stats_->classify_calls++;
        stats_->batch_counts.push_back(count);
        stats_->facts.insert(stats_->facts.end(), facts, facts + count);
        if (stats_->classify_error != 0 &&
            (stats_->fail_call < 0 || static_cast<size_t>(stats_->fail_call) == call_index)) {
            return stats_->classify_error;
        }
        for (uint32_t index = 0; index < count; ++index) {
            if (call_index < stats_->labels_by_call.size() && index < stats_->labels_by_call[call_index].size()) {
                primary_label_ids[index] = stats_->labels_by_call[call_index][index];
            } else {
                primary_label_ids[index] = facts[index].destination.port == 443 ? 1001 : 0;
            }
        }
        return 0;
    }

    bool FindLabel(uint32_t, flowsql::FlowPrimaryLabelViewV1*) const override { return false; }

    void Release() noexcept override {
        ++stats_->release_calls;
        delete this;
    }

 private:
    LabelingMatcherStats* stats_ = nullptr;
};

class LabelingStatusProvider final : public flowsql::IFlowLabelingProviderV1 {
 public:
    flowsql::FlowLabelingErrorV1 RuntimeStatus(flowsql::FlowLabelingDiagnosticV1* diagnostic) const override {
        ++runtime_status_calls;
        if (ready) {
            if (diagnostic != nullptr) *diagnostic = {};
            return flowsql::FlowLabelingErrorV1::kNone;
        }
        if (diagnostic != nullptr) {
            diagnostic->error = flowsql::FlowLabelingErrorV1::kUnavailable;
            diagnostic->path = "/runtime";
            diagnostic->detail = "mock labeling runtime unavailable";
        }
        return flowsql::FlowLabelingErrorV1::kUnavailable;
    }

    flowsql::FlowLabelingErrorV1 CreateMatcher(const flowsql::FlowLabelingCompileRequestV1& request,
                                               flowsql::IFlowLabelMatcherV1** output,
                                               flowsql::FlowLabelingDiagnosticV1* diagnostic) override {
        ++create_matcher_calls;
        if (create_succeeds && request.snapshot != nullptr && output != nullptr) {
            captured_reference = request.snapshot->channel_name + "@" + std::to_string(request.snapshot->revision);
            captured_reserved_bytes = request.reserved_module_state_bytes;
            captured_max_labels = request.max_labels;
            captured_max_logical_rules = request.max_logical_rules;
            captured_max_compiled_rules = request.max_compiled_rules;
            *output = new RecordingLabelMatcher(&matcher_stats);
            if (diagnostic != nullptr) *diagnostic = {};
            return flowsql::FlowLabelingErrorV1::kNone;
        }
        if (diagnostic != nullptr) {
            diagnostic->error = flowsql::FlowLabelingErrorV1::kInvalidConfig;
            diagnostic->path = "/mock";
            diagnostic->detail = "not used by T0.1";
        }
        return flowsql::FlowLabelingErrorV1::kInvalidConfig;
    }

    bool ready = true;
    bool create_succeeds = false;
    mutable uint32_t runtime_status_calls = 0;
    uint32_t create_matcher_calls = 0;
    uint64_t captured_reserved_bytes = 0;
    uint32_t captured_max_labels = 0;
    uint32_t captured_max_logical_rules = 0;
    uint32_t captured_max_compiled_rules = 0;
    std::string captured_reference;
    LabelingMatcherStats matcher_stats;
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

    flowsql::protocol::Protocol Identify(int32_t pipeno, const uint8_t*, int32_t,
                                         const flowsql::protocol::Layers*) override {
        if (identify_entered != nullptr) identify_entered->store(true, std::memory_order_release);
        while (release_identify != nullptr && !release_identify->load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        identify_pipelines.push_back(pipeno);
        return {7, 8};
    }

    int32_t Layer(int32_t, const uint8_t*, int32_t, flowsql::protocol::Layers*) override {
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
                         flowsql::ProtocolPipelinePoolError acquire_result = flowsql::ProtocolPipelinePoolError::kNone)
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

    flowsql::protocol::Protocol Identify(int32_t pipeno, const uint8_t* packet, int32_t packet_size,
                                         const flowsql::protocol::Layers* layers) override {
        identify_pipelines.push_back(pipeno);
        return target_->Identify(pipeno, packet, packet_size, layers);
    }

    int32_t Layer(int32_t pipeno, const uint8_t* packet, int32_t packet_size,
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

PacketFixture MakeIpv4TcpPacket(const char* src, uint16_t src_port, const char* dst, uint16_t dst_port,
                                const std::vector<uint8_t>& payload, uint8_t flags = 0, uint32_t sequence = 0,
                                uint32_t acknowledgment = 0, uint16_t window = 0) {
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
    tcp->ack = htonl(acknowledgment);
    tcp->offset = 5;
    tcp->flags.flags_byte = flags;
    tcp->window = htons(window);
    if (!payload.empty()) {
        std::memcpy(fixture.bytes.data() + sizeof(flowsql::Ipv4Header) + sizeof(flowsql::TcpHeader), payload.data(),
                    payload.size());
    }

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

PacketFixture MakeIpv6UdpPacket(const char* src, uint16_t src_port, const char* dst, uint16_t dst_port,
                                const std::vector<uint8_t>& payload, bool fragmented = false,
                                uint16_t fragment_offset = 0) {
    const size_t fragment_header_size = fragmented ? 8 : 0;
    PacketFixture fixture;
    fixture.bytes.resize(sizeof(flowsql::Ipv6Header) + fragment_header_size + sizeof(flowsql::UdpHeader) +
                         payload.size());
    auto* ip = reinterpret_cast<flowsql::Ipv6Header*>(fixture.bytes.data());
    fixture.bytes[0] = 0x60;
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
    if (!payload.empty()) {
        std::memcpy(fixture.bytes.data() + udp_offset + sizeof(flowsql::UdpHeader), payload.data(), payload.size());
    }

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
    return left.input_namespace == right.input_namespace && left.observation_domain_id == right.observation_domain_id &&
           left.ip_family == right.ip_family && left.transport_protocol == right.transport_protocol &&
           SameIp(left.a.ip, right.a.ip) && left.a.port == right.a.port && SameIp(left.b.ip, right.b.ip) &&
           left.b.port == right.b.port;
}

void TestConfigDefaultsAndEnumContract() {
    static_assert(std::is_same_v<std::underlying_type_t<npm::NpmRunMode>, uint8_t>);
    static_assert(std::is_same_v<std::underlying_type_t<npm::NpmResultMode>, uint8_t>);
    static_assert(std::is_same_v<std::underlying_type_t<npm::NpmOverloadPolicy>, uint8_t>);
    static_assert(npm::kNpmMinOutputIntervalNs == 10'000'000LL);
    static_assert(npm::kNpmDefaultOutputIntervalNs == 30'000'000'000LL);
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
    assert(offline.result_mode == npm::NpmResultMode::kPeriodicSnapshot);
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
    const npm::NpmObservationDomainMap domain_map{"pcapfile.capture", {{4, 7}, {9, 7}, {10, 8}}};
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
    assert(npm::ResolveNpmObservationDomain(domain_map, 4, nullptr) == npm::NpmObservationDomainError::kNullOutput);

    const npm::NpmObservationDomainMap empty_namespace{"", {{4, 7}}};
    assert(npm::ValidateNpmObservationDomainMap(empty_namespace) ==
           npm::NpmObservationDomainError::kEmptyInputNamespace);
    const npm::NpmObservationDomainMap empty_bindings{"pcapfile.capture", {}};
    assert(npm::ValidateNpmObservationDomainMap(empty_bindings) == npm::NpmObservationDomainError::kEmptyBindings);
    const npm::NpmObservationDomainMap duplicate_source{"pcapfile.capture", {{4, 7}, {4, 8}}};
    assert(npm::ValidateNpmObservationDomainMap(duplicate_source) ==
           npm::NpmObservationDomainError::kDuplicateSourceId);

    const npm::NpmObservationDomainMap all_sources{"pcapfile.capture", {}, true};
    assert(npm::ValidateNpmObservationDomainMap(all_sources) == npm::NpmObservationDomainError::kNone);
    for (const uint32_t source : {0u, 11u, UINT32_MAX}) {
        assert(npm::ResolveNpmObservationDomain(all_sources, source, &observation_domain_id) ==
               npm::NpmObservationDomainError::kNone);
        assert(observation_domain_id == source);
    }
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
        {"primary_label_id", arrow::Type::UINT32, true},
        {"protocol_status", arrow::Type::STRING, false},
        {"protocol_id", arrow::Type::UINT16, true},
        {"protocol_sub_id", arrow::Type::UINT16, true},
        {"protocol", arrow::Type::STRING, true},
        {"end_reason", arrow::Type::STRING, true},
        {"wire_bytes_total", arrow::Type::UINT64, false},
        {"period_start_ns", arrow::Type::INT64, true},
        {"period_end_ns", arrow::Type::INT64, true},
        {"period_complete", arrow::Type::BOOL, true},
        {"interval_packets_ab", arrow::Type::UINT64, true},
        {"interval_packets_ba", arrow::Type::UINT64, true},
        {"interval_wire_bytes_ab", arrow::Type::UINT64, true},
        {"interval_wire_bytes_ba", arrow::Type::UINT64, true},
        {"interval_wire_bytes_total", arrow::Type::UINT64, true},
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
    assert(schema->metadata()->size() == 5);
    assert(schema->metadata()->Get("flowsql.entity").ValueOrDie() == "npm_basic_result");
    assert(schema->metadata()->Get("flowsql.schema_version").ValueOrDie() == "1");
    assert(schema->metadata()->Get("flowsql.timestamp_unit").ValueOrDie() == "ns");
}

npm::NpmSessionResult MakeValidUdpSessionResult() {
    npm::NpmSessionResult result;
    result.session_id = 1;
    result.observation_domain_id = 7;
    result.revision = 1;
    result.observed_at = 100;
    result.ip_family = 4;
    result.transport_protocol = 17;
    result.a_ip = "192.0.2.1";
    result.b_ip = "192.0.2.2";
    result.a_port = 1000;
    result.b_port = 2000;
    result.first_ns = 10;
    result.last_ns = 10;
    result.duration_ns = 0;
    result.packets_ab = 1;
    result.wire_bytes_ab = 100;
    result.payload_bytes_ab = 20;
    return result;
}

npm::NpmSessionResult MakeValidTcpSessionResult() {
    auto result = MakeValidUdpSessionResult();
    result.transport_protocol = 6;
    result.last_ns = 20;
    result.duration_ns = 10;
    result.protocol_status = npm::NpmProtocolStatus::kIdentified;
    result.protocol_id = 80;
    result.protocol = "HTTP";
    result.rate_status = npm::NpmRateStatus::kValid;
    result.wire_bps_ab = 80.0;
    result.wire_bps_ba = 0.0;
    result.payload_bps_ab = 16.0;
    result.payload_bps_ba = 0.0;
    result.tcp_unique_payload_bytes_ab = 20;
    result.tcp_unique_payload_bytes_ba = 0;
    result.tcp_unique_payload_bps_ab = 16.0;
    result.tcp_unique_payload_bps_ba = 0.0;
    result.tcp_handshake_status = npm::NpmTcpHandshakeStatus::kComplete;
    result.tcp_initiator = npm::NpmTcpInitiator::kA;
    result.tcp_handshake_duration_ns = 5;
    result.tcp_synack_rtt_ns = 2;
    result.tcp_rtt_status = npm::NpmTcpRttStatus::kValid;
    result.tcp_rtt_samples = 1;
    result.tcp_rtt_min_ns = 3;
    result.tcp_rtt_mean_ns = 3;
    result.tcp_rtt_max_ns = 3;
    result.tcp_retransmission_status = npm::NpmTcpRetransmissionStatus::kValid;
    result.tcp_retrans_packets_ab = 0;
    result.tcp_retrans_packets_ba = 0;
    result.tcp_retrans_payload_bytes_ab = 0;
    result.tcp_retrans_payload_bytes_ba = 0;
    return result;
}

void TestSessionResultStatusAndNullableContract() {
    static_assert(std::is_same_v<std::underlying_type_t<npm::NpmRateStatus>, uint8_t>);
    static_assert(std::is_same_v<std::underlying_type_t<npm::NpmTcpHandshakeStatus>, uint8_t>);
    static_assert(std::is_same_v<std::underlying_type_t<npm::NpmTcpRttStatus>, uint8_t>);
    static_assert(std::is_same_v<std::underlying_type_t<npm::NpmTcpRetransmissionStatus>, uint8_t>);
    static_assert(std::is_same_v<std::underlying_type_t<npm::NpmTcpInitiator>, uint8_t>);
    static_assert(npm::kNpmMeasurementKnownFlags == 31);

    assert(std::strcmp(npm::NpmRateStatusName(npm::NpmRateStatus::kValid), "valid") == 0);
    assert(std::strcmp(npm::NpmRateStatusName(npm::NpmRateStatus::kInsufficientSpan), "insufficient_span") == 0);
    assert(std::strcmp(npm::NpmTcpHandshakeStatusName(npm::NpmTcpHandshakeStatus::kComplete), "complete") == 0);
    assert(std::strcmp(npm::NpmTcpHandshakeStatusName(npm::NpmTcpHandshakeStatus::kPartial), "partial") == 0);
    assert(std::strcmp(npm::NpmTcpHandshakeStatusName(npm::NpmTcpHandshakeStatus::kNotObserved), "not_observed") == 0);
    assert(std::strcmp(npm::NpmTcpHandshakeStatusName(npm::NpmTcpHandshakeStatus::kAmbiguous), "ambiguous") == 0);
    assert(std::strcmp(npm::NpmTcpHandshakeStatusName(npm::NpmTcpHandshakeStatus::kNotApplicable), "not_applicable") ==
           0);
    assert(std::strcmp(npm::NpmTcpRttStatusName(npm::NpmTcpRttStatus::kNoSample), "no_sample") == 0);
    assert(std::strcmp(npm::NpmTcpRetransmissionStatusName(npm::NpmTcpRetransmissionStatus::kNotApplicable),
                       "not_applicable") == 0);
    assert(std::strcmp(npm::NpmTcpInitiatorName(npm::NpmTcpInitiator::kA), "a") == 0);
    assert(std::strcmp(npm::NpmTcpInitiatorName(npm::NpmTcpInitiator::kB), "b") == 0);
    assert(npm::NpmRateStatusName(static_cast<npm::NpmRateStatus>(2)) == nullptr);
    assert(npm::NpmTcpHandshakeStatusName(static_cast<npm::NpmTcpHandshakeStatus>(5)) == nullptr);
    assert(npm::NpmTcpRttStatusName(static_cast<npm::NpmTcpRttStatus>(4)) == nullptr);
    assert(npm::NpmTcpRetransmissionStatusName(static_cast<npm::NpmTcpRetransmissionStatus>(3)) == nullptr);
    assert(npm::NpmTcpInitiatorName(static_cast<npm::NpmTcpInitiator>(2)) == nullptr);

    auto udp = MakeValidUdpSessionResult();
    assert(npm::ValidateNpmSessionResult(udp) == npm::NpmSessionResultError::kNone);
    udp.measurement_flags = npm::kNpmMeasurementTruncatedPayload | npm::kNpmMeasurementTimestampRegression;
    assert(npm::ValidateNpmSessionResult(udp) == npm::NpmSessionResultError::kNone);
    udp.measurement_flags = npm::kNpmMeasurementMidstreamStart;
    assert(npm::ValidateNpmSessionResult(udp) == npm::NpmSessionResultError::kTcpFieldsMismatch);
    udp = MakeValidUdpSessionResult();
    udp.tcp_rtt_samples = 0;
    assert(npm::ValidateNpmSessionResult(udp) == npm::NpmSessionResultError::kTcpFieldsMismatch);

    auto tcp = MakeValidTcpSessionResult();
    assert(npm::ValidateNpmSessionResult(tcp) == npm::NpmSessionResultError::kNone);

    auto invalid = tcp;
    invalid.revision = 0;
    assert(npm::ValidateNpmSessionResult(invalid) == npm::NpmSessionResultError::kInvalidIdentity);
    invalid = tcp;
    invalid.duration_ns = 9;
    assert(npm::ValidateNpmSessionResult(invalid) == npm::NpmSessionResultError::kInvalidTimeRange);
    invalid = tcp;
    invalid.wire_bps_ab = std::numeric_limits<double>::infinity();
    assert(npm::ValidateNpmSessionResult(invalid) == npm::NpmSessionResultError::kInvalidRateValue);
    invalid = tcp;
    invalid.payload_bytes_ab = invalid.wire_bytes_ab + 1;
    assert(npm::ValidateNpmSessionResult(invalid) == npm::NpmSessionResultError::kTrafficTotalsMismatch);
    invalid = tcp;
    invalid.measurement_flags = 1u << 5;
    assert(npm::ValidateNpmSessionResult(invalid) == npm::NpmSessionResultError::kInvalidMeasurementFlags);

    invalid = tcp;
    invalid.tcp_rtt_status = static_cast<npm::NpmTcpRttStatus>(4);
    assert(npm::ValidateNpmSessionResult(invalid) == npm::NpmSessionResultError::kInvalidTcpRttStatus);
    invalid = tcp;
    invalid.tcp_handshake_duration_ns.reset();
    assert(npm::ValidateNpmSessionResult(invalid) == npm::NpmSessionResultError::kHandshakeFieldsMismatch);
    invalid = tcp;
    invalid.tcp_rtt_samples = 0;
    assert(npm::ValidateNpmSessionResult(invalid) == npm::NpmSessionResultError::kRttFieldsMismatch);
    invalid = tcp;
    invalid.tcp_retrans_packets_ab.reset();
    assert(npm::ValidateNpmSessionResult(invalid) == npm::NpmSessionResultError::kRetransmissionFieldsMismatch);

    tcp.tcp_rtt_status = npm::NpmTcpRttStatus::kNoSample;
    tcp.tcp_rtt_samples = 0;
    tcp.tcp_rtt_min_ns.reset();
    tcp.tcp_rtt_mean_ns.reset();
    tcp.tcp_rtt_max_ns.reset();
    assert(npm::ValidateNpmSessionResult(tcp) == npm::NpmSessionResultError::kNone);

    tcp.measurement_flags = npm::kNpmMeasurementSequenceAmbiguous;
    tcp.tcp_unique_payload_bytes_ab.reset();
    tcp.tcp_unique_payload_bytes_ba.reset();
    tcp.tcp_unique_payload_bps_ab.reset();
    tcp.tcp_unique_payload_bps_ba.reset();
    tcp.tcp_rtt_status = npm::NpmTcpRttStatus::kAmbiguous;
    tcp.tcp_rtt_samples.reset();
    tcp.tcp_retransmission_status = npm::NpmTcpRetransmissionStatus::kAmbiguous;
    tcp.tcp_retrans_packets_ab.reset();
    tcp.tcp_retrans_packets_ba.reset();
    tcp.tcp_retrans_payload_bytes_ab.reset();
    tcp.tcp_retrans_payload_bytes_ba.reset();
    assert(npm::ValidateNpmSessionResult(tcp) == npm::NpmSessionResultError::kNone);

    tcp.measurement_flags |= npm::kNpmMeasurementSynRetransmitted;
    assert(npm::ValidateNpmSessionResult(tcp) == npm::NpmSessionResultError::kHandshakeFieldsMismatch);
    tcp.tcp_handshake_status = npm::NpmTcpHandshakeStatus::kAmbiguous;
    tcp.tcp_initiator.reset();
    tcp.tcp_handshake_duration_ns.reset();
    tcp.tcp_synack_rtt_ns.reset();
    assert(npm::ValidateNpmSessionResult(tcp) == npm::NpmSessionResultError::kNone);

    invalid = MakeValidTcpSessionResult();
    invalid.is_final = true;
    invalid.protocol_status = npm::NpmProtocolStatus::kPending;
    invalid.protocol_id.reset();
    invalid.protocol.reset();
    invalid.end_reason = npm::NpmSessionEndReason::kEof;
    assert(npm::ValidateNpmSessionResult(invalid) == npm::NpmSessionResultError::kPendingFinalResult);
    invalid.protocol_status = npm::NpmProtocolStatus::kUnknown;
    assert(npm::ValidateNpmSessionResult(invalid) == npm::NpmSessionResultError::kNone);
    invalid.end_reason.reset();
    assert(npm::ValidateNpmSessionResult(invalid) == npm::NpmSessionResultError::kMissingFinalEndReason);
}

void TestSessionResultSchema() {
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
        {"duration_ns", arrow::Type::INT64, false},
        {"protocol_status", arrow::Type::STRING, false},
        {"protocol_id", arrow::Type::UINT16, true},
        {"protocol_sub_id", arrow::Type::UINT16, true},
        {"protocol", arrow::Type::STRING, true},
        {"end_reason", arrow::Type::STRING, true},
        {"packets_ab", arrow::Type::UINT64, false},
        {"packets_ba", arrow::Type::UINT64, false},
        {"wire_bytes_ab", arrow::Type::UINT64, false},
        {"wire_bytes_ba", arrow::Type::UINT64, false},
        {"payload_bytes_ab", arrow::Type::UINT64, false},
        {"payload_bytes_ba", arrow::Type::UINT64, false},
        {"rate_status", arrow::Type::STRING, false},
        {"wire_bps_ab", arrow::Type::DOUBLE, true},
        {"wire_bps_ba", arrow::Type::DOUBLE, true},
        {"payload_bps_ab", arrow::Type::DOUBLE, true},
        {"payload_bps_ba", arrow::Type::DOUBLE, true},
        {"tcp_unique_payload_bytes_ab", arrow::Type::UINT64, true},
        {"tcp_unique_payload_bytes_ba", arrow::Type::UINT64, true},
        {"tcp_unique_payload_bps_ab", arrow::Type::DOUBLE, true},
        {"tcp_unique_payload_bps_ba", arrow::Type::DOUBLE, true},
        {"tcp_handshake_status", arrow::Type::STRING, false},
        {"tcp_initiator", arrow::Type::STRING, true},
        {"tcp_handshake_duration_ns", arrow::Type::INT64, true},
        {"tcp_synack_rtt_ns", arrow::Type::INT64, true},
        {"tcp_rtt_status", arrow::Type::STRING, false},
        {"tcp_rtt_samples", arrow::Type::UINT64, true},
        {"tcp_rtt_min_ns", arrow::Type::INT64, true},
        {"tcp_rtt_mean_ns", arrow::Type::INT64, true},
        {"tcp_rtt_max_ns", arrow::Type::INT64, true},
        {"tcp_retransmission_status", arrow::Type::STRING, false},
        {"tcp_retrans_packets_ab", arrow::Type::UINT64, true},
        {"tcp_retrans_packets_ba", arrow::Type::UINT64, true},
        {"tcp_retrans_payload_bytes_ab", arrow::Type::UINT64, true},
        {"tcp_retrans_payload_bytes_ba", arrow::Type::UINT64, true},
        {"measurement_flags", arrow::Type::UINT32, false},
    };

    const auto schema = npm::NpmSessionResultSchema();
    assert(schema != nullptr && schema.get() == npm::NpmSessionResultSchema().get());
    assert(schema->num_fields() == 49);
    assert(schema->num_fields() == static_cast<int>(expected.size()));
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const auto& field = schema->field(static_cast<int>(i));
        assert(field->name() == expected[i].name);
        assert(field->type()->id() == expected[i].type);
        assert(field->nullable() == expected[i].nullable);
    }
    assert(schema->GetFieldIndex("raw_data") == -1);
    assert(schema->metadata() != nullptr && schema->metadata()->size() == 5);
    assert(schema->metadata()->Get("flowsql.entity").ValueOrDie() == "npm_session_result");
    assert(schema->metadata()->Get("flowsql.schema_version").ValueOrDie() == "1");
    assert(schema->metadata()->Get("flowsql.timestamp_unit").ValueOrDie() == "ns");
    assert(schema->metadata()->Get("flowsql.revision_semantics").ValueOrDie() == "cumulative");
    assert(schema->metadata()->Get("flowsql.measurement_scope").ValueOrDie() == "single_capture_observed_packets");
}

void AssertBudgetUsage(const npm::NpmBudgetUsage& usage, uint64_t session, uint64_t module, uint64_t input,
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
    assert(npm::ReserveNpmBudget(config, npm::NpmBudgetCategory::kModuleState, config.max_tracked_bytes - 100,
                                 &usage) == npm::NpmBudgetError::kNone);
    AssertBudgetUsage(usage, 100, config.max_tracked_bytes - 100, 0, 0);
    assert(npm::ReserveNpmBudget(config, npm::NpmBudgetCategory::kInputBatch, 1, &usage) ==
           npm::NpmBudgetError::kTrackedLimitExceeded);
    AssertBudgetUsage(usage, 100, config.max_tracked_bytes - 100, 0, 0);

    assert(npm::ReserveNpmBudget(config, npm::NpmBudgetCategory::kPendingOutput, config.max_pending_output_bytes,
                                 &usage) == npm::NpmBudgetError::kNone);
    assert(npm::ReserveNpmBudget(config, npm::NpmBudgetCategory::kPendingOutput, 1, &usage) ==
           npm::NpmBudgetError::kPendingOutputLimitExceeded);
    AssertBudgetUsage(usage, 100, config.max_tracked_bytes - 100, 0, config.max_pending_output_bytes);
    assert(npm::ReleaseNpmBudget(npm::NpmBudgetCategory::kPendingOutput, 1, &usage) == npm::NpmBudgetError::kNone);
    assert(npm::ReserveNpmBudget(config, npm::NpmBudgetCategory::kPendingOutput, 1, &usage) ==
           npm::NpmBudgetError::kNone);
    AssertBudgetUsage(usage, 100, config.max_tracked_bytes - 100, 0, config.max_pending_output_bytes);

    assert(npm::ReleaseNpmBudget(npm::NpmBudgetCategory::kModuleState, 1, &usage) == npm::NpmBudgetError::kNone);
    assert(npm::ReserveNpmBudget(config, npm::NpmBudgetCategory::kInputBatch, 1, &usage) == npm::NpmBudgetError::kNone);
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
    assert(npm::ReleaseNpmBudget(npm::NpmBudgetCategory::kSessionState, 1, nullptr) == npm::NpmBudgetError::kNullUsage);
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

    int WriteSession(const npm::NpmSessionResult& result) override {
        if (next_error != 0) return next_error;
        if (npm::ValidateNpmSessionResult(result) != npm::NpmSessionResultError::kNone) return EINVAL;
        last_session_result = result;
        ++session_accepted;
        return 0;
    }

    int next_error = 0;
    uint32_t accepted = 0;
    uint32_t session_accepted = 0;
    npm::NpmBasicResult last_result;
    npm::NpmSessionResult last_session_result;
};

class CapturingForwardingWriter final : public npm::INpmResultWriter {
 public:
    explicit CapturingForwardingWriter(npm::INpmResultWriter& downstream) : downstream_(downstream) {}

    int WriteBasic(const npm::NpmBasicResult& result) override {
        const int error = downstream_.WriteBasic(result);
        if (error != 0) return error;
        last_basic_result = result;
        ++basic_writes;
        return 0;
    }

    int WriteSession(const npm::NpmSessionResult& result) override {
        const int error = downstream_.WriteSession(result);
        if (error != 0) return error;
        last_session_result = result;
        ++session_writes;
        return 0;
    }

    uint32_t basic_writes = 0;
    uint32_t session_writes = 0;
    npm::NpmBasicResult last_basic_result;
    npm::NpmSessionResult last_session_result;

 private:
    npm::INpmResultWriter& downstream_;
};

class FixtureModule final : public npm::INpmAnalysisModule {
 public:
    int OnPacket(const npm::NpmPacketView& packet, const npm::NpmSessionView& session,
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

    int OnSessionSnapshot(const npm::NpmSessionView& session, int64_t observed_at_ns,
                          npm::INpmResultWriter& writer) override {
        auto result = MakeValidUdpSessionResult();
        result.session_id = session.session_id;
        result.observation_domain_id = session.key->observation_domain_id;
        result.observed_at = observed_at_ns;
        return writer.WriteSession(result);
    }

    int OnSessionEnd(const npm::NpmSessionView& session, npm::NpmSessionEndReason reason, int64_t observed_at_ns,
                     npm::INpmResultWriter& writer) override {
        auto result = MakeValidUdpSessionResult();
        result.session_id = session.session_id;
        result.observation_domain_id = session.key->observation_domain_id;
        result.observed_at = observed_at_ns;
        result.protocol_status = npm::NpmProtocolStatus::kUnknown;
        result.is_final = true;
        result.end_reason = reason;
        return writer.WriteSession(result);
    }
};

void TestBorrowedViewsAndModuleInterfaces() {
    using WriteSessionMethod = int (npm::INpmResultWriter::*)(const npm::NpmSessionResult&);
    using SessionSnapshotMethod =
        int (npm::INpmAnalysisModule::*)(const npm::NpmSessionView&, int64_t, npm::INpmResultWriter&);
    using SessionEndMethod = int (npm::INpmAnalysisModule::*)(const npm::NpmSessionView&, npm::NpmSessionEndReason,
                                                              int64_t, npm::INpmResultWriter&);
    static_assert(std::is_abstract_v<npm::INpmTaskBudget>);
    static_assert(std::is_abstract_v<npm::INpmResultWriter>);
    static_assert(std::is_abstract_v<npm::INpmAnalysisModule>);
    static_assert(std::is_same_v<decltype(&npm::INpmResultWriter::WriteSession), WriteSessionMethod>);
    static_assert(std::is_same_v<decltype(&npm::INpmAnalysisModule::OnSessionSnapshot), SessionSnapshotMethod>);
    static_assert(std::is_same_v<decltype(&npm::INpmAnalysisModule::OnSessionEnd), SessionEndMethod>);
    static_assert(std::is_same_v<std::underlying_type_t<npm::NpmPacketDirection>, uint8_t>);

    auto bytes_owner = std::make_shared<std::array<uint8_t, 4>>(std::array<uint8_t, 4>{0x10, 0x20, 0x30, 0x40});
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
    assert(module.OnSessionSnapshot(session_view, 456, writer) == 0);
    assert(writer.session_accepted == 1);
    assert(writer.last_session_result.session_id == 42);
    assert(writer.last_session_result.observed_at == 456 && !writer.last_session_result.is_final);
    writer.next_error = EIO;
    assert(module.OnSessionSnapshot(session_view, 457, writer) == EIO);
    assert(writer.session_accepted == 1);
    writer.next_error = 0;
    assert(module.OnSessionEnd(session_view, npm::NpmSessionEndReason::kEof, 789, writer) == 0);
    assert(writer.session_accepted == 2);
    assert(writer.last_session_result.observed_at == 789);
    assert(writer.last_session_result.is_final);
    assert(writer.last_session_result.end_reason == npm::NpmSessionEndReason::kEof);
    assert(npm::ValidateNpmSessionResult(writer.last_session_result) == npm::NpmSessionResultError::kNone);

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
    assert(npm::ValidateNpmTimeCapabilities(realtime, capabilities) == npm::NpmTimeCapabilityError::kInvalidRunMode);
}

void TestSessionPacketCanonicalizationAndObservationDomains() {
    const npm::NpmObservationDomainMap domain_map{"capture-a", {{1, 77}, {2, 77}, {3, 88}}};
    auto forward =
        MakeIpv4TcpPacket("192.0.2.200", 50000, "192.0.2.10", 443, {0x10, 0x20}, kTcpSyn, 0x01020304, 0x11121314, 4096);
    auto reverse = MakeIpv4TcpPacket("192.0.2.10", 443, "192.0.2.200", 50000, {0x30}, kTcpSyn | kTcpAck, 0x50607080,
                                     0xa0b0c0d0, 8192);
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
    assert(forward_binding.transport.tcp.valid && forward_binding.transport.tcp.syn &&
           !forward_binding.transport.tcp.ack);
    assert(!forward_binding.transport.tcp.fin && !forward_binding.transport.tcp.rst);
    assert(forward_binding.transport.tcp.sequence == 0x01020304);
    assert(forward_binding.transport.tcp.acknowledgment == 0x11121314);
    assert(forward_binding.transport.tcp.window == 4096);
    assert(forward_binding.transport.payload_wire_bytes == 2);
    assert(forward_binding.transport.payload_captured_bytes == 2);
    assert(forward_binding.transport.payload_complete);
    assert(reverse_binding.transport.tcp.valid && reverse_binding.transport.tcp.syn &&
           reverse_binding.transport.tcp.ack);
    assert(reverse_binding.transport.tcp.sequence == 0x50607080);
    assert(reverse_binding.transport.tcp.acknowledgment == 0xa0b0c0d0);
    assert(reverse_binding.transport.tcp.window == 8192);

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
    assert(npm::BuildNpmSessionPacketBinding(domain_map, ipv6_forward.View(1), ipv6_forward.layer,
                                             &ipv6_forward_binding) == npm::NpmSessionPacketError::kNone);
    assert(npm::BuildNpmSessionPacketBinding(domain_map, ipv6_reverse.View(2), ipv6_reverse.layer,
                                             &ipv6_reverse_binding) == npm::NpmSessionPacketError::kNone);
    assert(SameKey(ipv6_forward_binding.key, ipv6_reverse_binding.key));
    assert(ipv6_forward_binding.key.ip_family == flowsql::packet::AddressFamily::kIPv6);
    assert(ipv6_forward_binding.direction == npm::NpmPacketDirection::kBToA);
    assert(ipv6_reverse_binding.direction == npm::NpmPacketDirection::kAToB);
    assert(!ipv6_forward_binding.transport.tcp.valid && !ipv6_reverse_binding.transport.tcp.valid);
    assert(ipv6_forward_binding.transport.payload_wire_bytes == 1);
    assert(ipv6_forward_binding.transport.payload_captured_bytes == 1);
    assert(ipv6_forward_binding.transport.payload_complete);

    npm::NpmSessionPacketBinding other_domain;
    assert(npm::BuildNpmSessionPacketBinding(domain_map, forward.View(3), forward.layer, &other_domain) ==
           npm::NpmSessionPacketError::kNone);
    assert(other_domain.key.observation_domain_id == 88);
    assert(!SameKey(forward_binding.key, other_domain.key));
    const npm::NpmObservationDomainMap other_namespace{"capture-b", {{1, 77}}};
    npm::NpmSessionPacketBinding namespace_binding;
    assert(npm::BuildNpmSessionPacketBinding(other_namespace, forward.View(1), forward.layer, &namespace_binding) ==
           npm::NpmSessionPacketError::kNone);
    assert(namespace_binding.key.input_namespace == "capture-b");
    assert(!SameKey(forward_binding.key, namespace_binding.key));

    npm::NpmSessionPacketBinding unchanged;
    unchanged.key.input_namespace = "unchanged";
    unchanged.transport.tcp.sequence = 123;
    assert(npm::BuildNpmSessionPacketBinding(domain_map, forward.View(99), forward.layer, &unchanged) ==
           npm::NpmSessionPacketError::kUnknownSourceId);
    assert(unchanged.key.input_namespace == "unchanged");
    assert(unchanged.transport.tcp.sequence == 123);
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
    assert(binding.transport.payload_wire_bytes == 4);
    assert(binding.transport.payload_captured_bytes == 4);
    assert(binding.transport.payload_complete);

    auto truncated_body = packet;
    truncated_body.bytes.resize(truncated_body.bytes.size() - 2);
    truncated_body.layer.status = flowsql::packet::LayerStatus::kTruncated;
    assert(npm::BuildNpmSessionPacketBinding(domain_map, truncated_body.View(1, packet.bytes.size()),
                                             truncated_body.layer, &binding) == npm::NpmSessionPacketError::kNone);
    assert(binding.payload.size == 2 && binding.payload[1] == 2);
    assert(binding.transport.payload_wire_bytes == 4);
    assert(binding.transport.payload_captured_bytes == 2);
    assert(!binding.transport.payload_complete);

    auto udp_packet = MakeIpv6UdpPacket("2001:db8::1", 1000, "2001:db8::2", 2000, {5, 6, 7});
    assert(npm::BuildNpmSessionPacketBinding(domain_map, udp_packet.View(1), udp_packet.layer, &binding) ==
           npm::NpmSessionPacketError::kNone);
    assert(binding.transport.payload_wire_bytes == 3);
    assert(binding.transport.payload_captured_bytes == 3);
    assert(binding.transport.payload_complete && !binding.transport.tcp.valid);
    auto truncated_udp = udp_packet;
    truncated_udp.bytes.resize(truncated_udp.bytes.size() - 1);
    truncated_udp.layer.status = flowsql::packet::LayerStatus::kTruncated;
    assert(npm::BuildNpmSessionPacketBinding(domain_map, truncated_udp.View(1, udp_packet.bytes.size()),
                                             truncated_udp.layer, &binding) == npm::NpmSessionPacketError::kNone);
    assert(binding.transport.payload_wire_bytes == 3);
    assert(binding.transport.payload_captured_bytes == 2);
    assert(!binding.transport.payload_complete && !binding.transport.tcp.valid);

    auto truncated_header = packet;
    truncated_header.bytes.resize(sizeof(flowsql::Ipv4Header) + 10);
    truncated_header.layer.status = flowsql::packet::LayerStatus::kTruncated;
    binding.transport.payload_wire_bytes = 99;
    binding.transport.tcp.sequence = 88;
    assert(npm::BuildNpmSessionPacketBinding(domain_map, truncated_header.View(1, packet.bytes.size()),
                                             truncated_header.layer,
                                             &binding) == npm::NpmSessionPacketError::kIncompleteTransportHeader);
    assert(binding.transport.payload_wire_bytes == 99);
    assert(binding.transport.tcp.sequence == 88);

    auto invalid_payload = packet;
    ++invalid_payload.layer.payload_offset;
    assert(npm::BuildNpmSessionPacketBinding(domain_map, invalid_payload.View(1), invalid_payload.layer, &binding) ==
           npm::NpmSessionPacketError::kInvalidPayloadBounds);

    auto invalid_udp_length = MakeIpv6UdpPacket("2001:db8::1", 1000, "2001:db8::2", 2000, {1, 2});
    auto* udp = reinterpret_cast<flowsql::UdpHeader*>(invalid_udp_length.bytes.data() + sizeof(flowsql::Ipv6Header));
    udp->length = htons(1000);
    assert(npm::BuildNpmSessionPacketBinding(domain_map, invalid_udp_length.View(1), invalid_udp_length.layer,
                                             &binding) == npm::NpmSessionPacketError::kInvalidPayloadBounds);

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
    assert(npm::BuildNpmSessionPacketBinding(domain_map, ipv4_fragment.View(1), ipv4_fragment.layer, &binding) ==
           npm::NpmSessionPacketError::kNonInitialFragment);

    auto ipv6_fragment = MakeIpv6UdpPacket("2001:db8::1", 1000, "2001:db8::2", 2000, {1}, true, 2);
    ipv6_fragment.layer.layer_count = 2;
    ipv6_fragment.layer.transport_layer_index = flowsql::packet::kNoLayerIndex;
    ipv6_fragment.layer.ports_valid = 0;
    assert(npm::BuildNpmSessionPacketBinding(domain_map, ipv6_fragment.View(1), ipv6_fragment.layer, &binding) ==
           npm::NpmSessionPacketError::kNonInitialFragment);
    auto ipv6_first_fragment = MakeIpv6UdpPacket("2001:db8::1", 1000, "2001:db8::2", 2000, {1}, true, 0);
    assert(npm::BuildNpmSessionPacketBinding(domain_map, ipv6_first_fragment.View(1), ipv6_first_fragment.layer,
                                             &binding) == npm::NpmSessionPacketError::kNone);
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

npm::NpmSessionPacketBinding BuildBinding(const npm::NpmObservationDomainMap& domain_map, const PacketFixture& packet,
                                          uint32_t source_id, int64_t timestamp_ns, uint32_t wire_len,
                                          flowsql::packet::PacketMeta* meta) {
    auto view = packet.View(source_id, wire_len);
    view.meta.timestamp_ns = timestamp_ns;
    npm::NpmSessionPacketBinding binding;
    assert(npm::BuildNpmSessionPacketBinding(domain_map, view, packet.layer, &binding) ==
           npm::NpmSessionPacketError::kNone);
    *meta = view.meta;
    return binding;
}

npm::NpmPacketView MakeNpmPacketView(const PacketFixture& packet, const npm::NpmSessionPacketBinding& binding,
                                     uint32_t source_id, int64_t timestamp_ns, uint32_t wire_len) {
    npm::NpmPacketView view;
    view.packet = packet.View(source_id, wire_len);
    view.packet.meta.timestamp_ns = timestamp_ns;
    view.layer = &packet.layer;
    view.payload = binding.payload;
    view.direction = binding.direction;
    view.transport = binding.transport;
    return view;
}

npm::NpmPacketView MakeTcpPerformancePacket(const npm::NpmObservationDomainMap& domain_map, const PacketFixture& packet,
                                            int64_t timestamp_ns) {
    flowsql::packet::PacketMeta meta;
    const auto binding = BuildBinding(domain_map, packet, 0, timestamp_ns, 0, &meta);
    return MakeNpmPacketView(packet, binding, 0, timestamp_ns, 0);
}

npm::NpmSessionView MakePerformanceSession(uint64_t session_id) {
    npm::NpmSessionView session;
    session.session_id = session_id;
    return session;
}

void TestTcpPerformanceHandshakeStates() {
    static_assert(std::is_final_v<npm::NpmTcpPerformanceTracker>);
    static_assert(!std::is_copy_constructible_v<npm::NpmTcpPerformanceTracker>);
    static_assert(!std::is_move_constructible_v<npm::NpmTcpPerformanceTracker>);

    const npm::NpmObservationDomainMap domain_map{"capture", {{0, 1}}};
    const auto syn = MakeIpv4TcpPacket("192.0.2.1", 50000, "198.51.100.2", 443, {}, kTcpSyn, 100);
    const auto synack = MakeIpv4TcpPacket("198.51.100.2", 443, "192.0.2.1", 50000, {}, kTcpSyn | kTcpAck, 500, 101);
    const auto ack = MakeIpv4TcpPacket("192.0.2.1", 50000, "198.51.100.2", 443, {}, kTcpAck, 101, 501);
    const auto ordinary = MakeIpv4TcpPacket("192.0.2.1", 50000, "198.51.100.2", 443, {1}, kTcpAck, 1000, 700);
    auto budget = std::make_shared<FixtureBudget>(npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline));
    npm::NpmTcpPerformanceTracker tracker(budget);
    npm::NpmTcpPerformanceSnapshot snapshot;
    assert(tracker.Snapshot(1, nullptr) == npm::NpmTcpPerformanceError::kNullOutput);
    assert(tracker.Snapshot(1, &snapshot) == npm::NpmTcpPerformanceError::kNotFound);

    const auto session = MakePerformanceSession(1);
    assert(tracker.Observe(MakeTcpPerformancePacket(domain_map, syn, 10), session) ==
           npm::NpmTcpPerformanceError::kNone);
    assert(tracker.Snapshot(1, &snapshot) == npm::NpmTcpPerformanceError::kNone);
    assert(snapshot.handshake_status == npm::NpmTcpHandshakeStatus::kPartial);
    assert(snapshot.initiator == npm::NpmTcpInitiator::kA);
    assert(!snapshot.handshake_duration_ns.has_value() && !snapshot.synack_rtt_ns.has_value());
    assert(snapshot.rtt_status == npm::NpmTcpRttStatus::kNoSample);
    assert(snapshot.rtt_samples == 0 && snapshot.measurement_flags == 0);

    assert(tracker.Observe(MakeTcpPerformancePacket(domain_map, synack, 20), session) ==
           npm::NpmTcpPerformanceError::kNone);
    assert(tracker.Snapshot(1, &snapshot) == npm::NpmTcpPerformanceError::kNone);
    assert(snapshot.handshake_status == npm::NpmTcpHandshakeStatus::kPartial);
    assert(snapshot.synack_rtt_ns == 10 && !snapshot.handshake_duration_ns.has_value());

    assert(tracker.Observe(MakeTcpPerformancePacket(domain_map, ack, 30), session) ==
           npm::NpmTcpPerformanceError::kNone);
    assert(tracker.Snapshot(1, &snapshot) == npm::NpmTcpPerformanceError::kNone);
    assert(snapshot.handshake_status == npm::NpmTcpHandshakeStatus::kComplete);
    assert(snapshot.initiator == npm::NpmTcpInitiator::kA);
    assert(snapshot.handshake_duration_ns == 20 && snapshot.synack_rtt_ns == 10);

    const auto retransmitted_session = MakePerformanceSession(2);
    assert(tracker.Observe(MakeTcpPerformancePacket(domain_map, syn, 40), retransmitted_session) ==
           npm::NpmTcpPerformanceError::kNone);
    assert(tracker.Observe(MakeTcpPerformancePacket(domain_map, syn, 45), retransmitted_session) ==
           npm::NpmTcpPerformanceError::kNone);
    assert(tracker.Observe(MakeTcpPerformancePacket(domain_map, synack, 50), retransmitted_session) ==
           npm::NpmTcpPerformanceError::kNone);
    assert(tracker.Observe(MakeTcpPerformancePacket(domain_map, ack, 60), retransmitted_session) ==
           npm::NpmTcpPerformanceError::kNone);
    assert(tracker.Snapshot(2, &snapshot) == npm::NpmTcpPerformanceError::kNone);
    assert(snapshot.handshake_status == npm::NpmTcpHandshakeStatus::kAmbiguous);
    assert(!snapshot.initiator.has_value() && !snapshot.handshake_duration_ns.has_value());
    assert(!snapshot.synack_rtt_ns.has_value());
    assert((snapshot.measurement_flags & npm::kNpmMeasurementSynRetransmitted) != 0);

    const auto synack_first_session = MakePerformanceSession(3);
    assert(tracker.Observe(MakeTcpPerformancePacket(domain_map, synack, 70), synack_first_session) ==
           npm::NpmTcpPerformanceError::kNone);
    assert(tracker.Snapshot(3, &snapshot) == npm::NpmTcpPerformanceError::kNone);
    assert(snapshot.handshake_status == npm::NpmTcpHandshakeStatus::kPartial);
    assert(snapshot.initiator == npm::NpmTcpInitiator::kA);
    assert(!snapshot.handshake_duration_ns.has_value() && !snapshot.synack_rtt_ns.has_value());
    assert((snapshot.measurement_flags & npm::kNpmMeasurementMidstreamStart) != 0);

    const auto midstream_session = MakePerformanceSession(4);
    assert(tracker.Observe(MakeTcpPerformancePacket(domain_map, ordinary, 80), midstream_session) ==
           npm::NpmTcpPerformanceError::kNone);
    assert(tracker.Snapshot(4, &snapshot) == npm::NpmTcpPerformanceError::kNone);
    assert(snapshot.handshake_status == npm::NpmTcpHandshakeStatus::kNotObserved);
    assert(!snapshot.initiator.has_value() && !snapshot.handshake_duration_ns.has_value());
    assert((snapshot.measurement_flags & npm::kNpmMeasurementMidstreamStart) != 0);
}

void TestTcpPerformanceRttKarnAndTimestampRegression() {
    const npm::NpmObservationDomainMap domain_map{"capture", {{0, 1}}};
    auto budget = std::make_shared<FixtureBudget>(npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline));
    npm::NpmTcpPerformanceTracker tracker(budget);
    const auto session = MakePerformanceSession(10);
    const auto data_ab =
        MakeIpv4TcpPacket("192.0.2.1", 50000, "198.51.100.2", 443, std::vector<uint8_t>(10, 1), kTcpAck, 100);
    const auto ack_ab = MakeIpv4TcpPacket("198.51.100.2", 443, "192.0.2.1", 50000, {}, kTcpAck, 500, 110);
    const auto data_ba =
        MakeIpv4TcpPacket("198.51.100.2", 443, "192.0.2.1", 50000, std::vector<uint8_t>(5, 2), kTcpAck, 500, 110);
    const auto ack_ba = MakeIpv4TcpPacket("192.0.2.1", 50000, "198.51.100.2", 443, {}, kTcpAck, 110, 505);

    assert(tracker.Observe(MakeTcpPerformancePacket(domain_map, data_ab, 100), session) ==
           npm::NpmTcpPerformanceError::kNone);
    assert(tracker.Observe(MakeTcpPerformancePacket(domain_map, ack_ab, 130), session) ==
           npm::NpmTcpPerformanceError::kNone);
    assert(tracker.Observe(MakeTcpPerformancePacket(domain_map, data_ba, 140), session) ==
           npm::NpmTcpPerformanceError::kNone);
    assert(tracker.Observe(MakeTcpPerformancePacket(domain_map, ack_ba, 170), session) ==
           npm::NpmTcpPerformanceError::kNone);
    assert(tracker.Observe(MakeTcpPerformancePacket(domain_map, ack_ba, 180), session) ==
           npm::NpmTcpPerformanceError::kNone);

    npm::NpmTcpPerformanceSnapshot snapshot;
    assert(tracker.Snapshot(10, &snapshot) == npm::NpmTcpPerformanceError::kNone);
    assert(snapshot.rtt_status == npm::NpmTcpRttStatus::kValid);
    assert(snapshot.rtt_samples == 2);
    assert(snapshot.rtt_min_ns == 30 && snapshot.rtt_mean_ns == 30 && snapshot.rtt_max_ns == 30);
    assert(tracker.outstanding_segments() == 0);

    const auto retransmit =
        MakeIpv4TcpPacket("192.0.2.1", 50000, "198.51.100.2", 443, std::vector<uint8_t>(10, 3), kTcpAck, 200);
    const auto retransmit_ack = MakeIpv4TcpPacket("198.51.100.2", 443, "192.0.2.1", 50000, {}, kTcpAck, 600, 210);
    assert(tracker.Observe(MakeTcpPerformancePacket(domain_map, retransmit, 200), session) ==
           npm::NpmTcpPerformanceError::kNone);
    assert(tracker.Observe(MakeTcpPerformancePacket(domain_map, retransmit, 210), session) ==
           npm::NpmTcpPerformanceError::kNone);
    assert(tracker.Observe(MakeTcpPerformancePacket(domain_map, retransmit_ack, 240), session) ==
           npm::NpmTcpPerformanceError::kNone);
    assert(tracker.Snapshot(10, &snapshot) == npm::NpmTcpPerformanceError::kNone);
    assert(snapshot.rtt_status == npm::NpmTcpRttStatus::kValid && snapshot.rtt_samples == 2);
    assert(tracker.outstanding_segments() == 0);

    const auto regressed_session = MakePerformanceSession(11);
    assert(tracker.Observe(MakeTcpPerformancePacket(domain_map, data_ab, 300), regressed_session) ==
           npm::NpmTcpPerformanceError::kNone);
    const uint64_t bytes_with_segment = tracker.tracked_bytes();
    assert(tracker.outstanding_segments() == 1);
    assert(tracker.Observe(MakeTcpPerformancePacket(domain_map, ack_ab, 290), regressed_session) ==
           npm::NpmTcpPerformanceError::kNone);
    assert(tracker.Snapshot(11, &snapshot) == npm::NpmTcpPerformanceError::kNone);
    assert(snapshot.handshake_status == npm::NpmTcpHandshakeStatus::kAmbiguous);
    assert(snapshot.rtt_status == npm::NpmTcpRttStatus::kAmbiguous);
    assert(!snapshot.initiator.has_value() && !snapshot.handshake_duration_ns.has_value());
    assert(!snapshot.synack_rtt_ns.has_value() && !snapshot.rtt_samples.has_value());
    assert(!snapshot.rtt_min_ns.has_value() && !snapshot.rtt_mean_ns.has_value());
    assert(!snapshot.rtt_max_ns.has_value());
    assert((snapshot.measurement_flags & npm::kNpmMeasurementTimestampRegression) != 0);
    assert(tracker.outstanding_segments() == 0 && tracker.tracked_bytes() < bytes_with_segment);

    assert(tracker.Observe(MakeTcpPerformancePacket(domain_map, data_ab, 310), regressed_session) ==
           npm::NpmTcpPerformanceError::kNone);
    assert(tracker.Snapshot(11, &snapshot) == npm::NpmTcpPerformanceError::kNone);
    assert(snapshot.handshake_status == npm::NpmTcpHandshakeStatus::kAmbiguous);
    assert(snapshot.rtt_status == npm::NpmTcpRttStatus::kAmbiguous);
    assert(tracker.outstanding_segments() == 0);
    assert(tracker.tracked_bytes() == budget->Usage().module_state_bytes);
}

void TestTcpPerformanceBudgetCleanupAndIsolation() {
    const npm::NpmObservationDomainMap domain_map{"capture", {{0, 1}}};
    const auto syn = MakeIpv4TcpPacket("192.0.2.1", 50000, "198.51.100.2", 443, {}, kTcpSyn, 100);
    const auto synack = MakeIpv4TcpPacket("198.51.100.2", 443, "192.0.2.1", 50000, {}, kTcpSyn | kTcpAck, 500, 101);
    const auto ack = MakeIpv4TcpPacket("192.0.2.1", 50000, "198.51.100.2", 443, {}, kTcpAck, 101, 501);
    const auto data =
        MakeIpv4TcpPacket("192.0.2.1", 50000, "198.51.100.2", 443, std::vector<uint8_t>(10, 1), kTcpAck, 200);
    const auto packet = MakeTcpPerformancePacket(domain_map, syn, 10);

    npm::NpmAnalysisConfig tiny = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline);
    tiny.max_tracked_bytes = 1;
    auto tiny_budget = std::make_shared<FixtureBudget>(tiny);
    npm::NpmTcpPerformanceTracker constrained(tiny_budget);
    assert(constrained.Observe(packet, MakePerformanceSession(1)) == npm::NpmTcpPerformanceError::kBudgetExceeded);
    assert(constrained.size() == 0 && constrained.tracked_bytes() == 0);
    assert(tiny_budget->Usage().module_state_bytes == 0);

    uint64_t state_charge = 0;
    auto probe_budget = std::make_shared<FixtureBudget>(npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline));
    {
        npm::NpmTcpPerformanceTracker probe(probe_budget);
        assert(probe.Observe(packet, MakePerformanceSession(1)) == npm::NpmTcpPerformanceError::kNone);
        state_charge = probe.tracked_bytes();
        assert(state_charge > 1 && state_charge == probe_budget->Usage().module_state_bytes);
    }
    assert(probe_budget->Usage().module_state_bytes == 0);

    npm::NpmAnalysisConfig state_only = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline);
    state_only.max_tracked_bytes = state_charge;
    auto rollback_budget = std::make_shared<FixtureBudget>(state_only);
    npm::NpmTcpPerformanceTracker rollback_tracker(rollback_budget);
    assert(rollback_tracker.Observe(MakeTcpPerformancePacket(domain_map, data, 100), MakePerformanceSession(1)) ==
           npm::NpmTcpPerformanceError::kBudgetExceeded);
    assert(rollback_tracker.size() == 0 && rollback_tracker.tracked_bytes() == 0);
    assert(rollback_budget->Usage().module_state_bytes == 0);

    assert(rollback_tracker.Observe(packet, MakePerformanceSession(2)) == npm::NpmTcpPerformanceError::kNone);
    assert(rollback_tracker.Observe(MakeTcpPerformancePacket(domain_map, data, 100), MakePerformanceSession(2)) ==
           npm::NpmTcpPerformanceError::kBudgetExceeded);
    assert(rollback_tracker.outstanding_segments() == 0);
    assert(rollback_tracker.tracked_bytes() == state_charge);
    assert(rollback_tracker.Observe(MakeTcpPerformancePacket(domain_map, synack, 20), MakePerformanceSession(2)) ==
           npm::NpmTcpPerformanceError::kNone);
    assert(rollback_tracker.Observe(MakeTcpPerformancePacket(domain_map, ack, 30), MakePerformanceSession(2)) ==
           npm::NpmTcpPerformanceError::kNone);
    npm::NpmTcpPerformanceSnapshot rollback_snapshot;
    assert(rollback_tracker.Snapshot(2, &rollback_snapshot) == npm::NpmTcpPerformanceError::kNone);
    assert(rollback_snapshot.handshake_status == npm::NpmTcpHandshakeStatus::kComplete);
    assert(rollback_snapshot.handshake_duration_ns == 20);
    assert((rollback_snapshot.measurement_flags & npm::kNpmMeasurementTimestampRegression) == 0);
    rollback_tracker.Clear();
    assert(rollback_budget->Usage().module_state_bytes == 0);

    npm::NpmTcpPerformanceTracker missing_budget(nullptr);
    assert(missing_budget.Observe(packet, MakePerformanceSession(1)) == npm::NpmTcpPerformanceError::kNullBudget);
    assert(missing_budget.size() == 0);

    auto budget = std::make_shared<FixtureBudget>(npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline));
    {
        npm::NpmTcpPerformanceTracker tracker(budget);
        auto invalid_packet = packet;
        invalid_packet.transport.tcp.valid = false;
        assert(tracker.Observe(packet, MakePerformanceSession(0)) == npm::NpmTcpPerformanceError::kInvalidSession);
        assert(tracker.Observe(invalid_packet, MakePerformanceSession(1)) ==
               npm::NpmTcpPerformanceError::kInvalidPacket);
        assert(tracker.size() == 0 && tracker.tracked_bytes() == 0);

        assert(tracker.Observe(packet, MakePerformanceSession(1)) == npm::NpmTcpPerformanceError::kNone);
        assert(tracker.Observe(MakeTcpPerformancePacket(domain_map, data, 20), MakePerformanceSession(2)) ==
               npm::NpmTcpPerformanceError::kNone);
        assert(tracker.size() == 2 && tracker.outstanding_segments() == 1);
        assert(tracker.tracked_bytes() == budget->Usage().module_state_bytes);
        const uint64_t before_remove = tracker.tracked_bytes();
        tracker.Remove(1);
        assert(tracker.size() == 1 && tracker.outstanding_segments() == 1);
        assert(tracker.tracked_bytes() < before_remove);
        assert(tracker.tracked_bytes() == budget->Usage().module_state_bytes);
        tracker.Remove(999);
        assert(tracker.size() == 1);
        tracker.Remove(2);
        assert(tracker.size() == 0 && tracker.outstanding_segments() == 0);
        assert(tracker.tracked_bytes() == 0 && budget->Usage().module_state_bytes == 0);

        assert(tracker.Observe(MakeTcpPerformancePacket(domain_map, data, 30), MakePerformanceSession(2)) ==
               npm::NpmTcpPerformanceError::kNone);
        assert(tracker.size() == 1 && tracker.outstanding_segments() == 1);
        tracker.Clear();
        assert(tracker.size() == 0 && tracker.outstanding_segments() == 0);
        assert(tracker.tracked_bytes() == 0 && budget->Usage().module_state_bytes == 0);

        assert(tracker.Observe(MakeTcpPerformancePacket(domain_map, data, 40), MakePerformanceSession(3)) ==
               npm::NpmTcpPerformanceError::kNone);
        assert(tracker.outstanding_segments() == 1 && budget->Usage().module_state_bytes > 0);
    }
    assert(budget->Usage().module_state_bytes == 0);
}

npm::NpmPacketView MakeLedgerPacket(int64_t timestamp_ns, npm::NpmPacketDirection direction, uint32_t sequence,
                                    uint32_t payload_bytes, std::optional<uint32_t> acknowledgment = std::nullopt,
                                    bool syn = false) {
    npm::NpmPacketView packet;
    packet.packet.meta.timestamp_ns = timestamp_ns;
    packet.direction = direction;
    packet.transport.tcp.valid = true;
    packet.transport.tcp.sequence = sequence;
    packet.transport.tcp.syn = syn;
    packet.transport.tcp.ack = acknowledgment.has_value();
    packet.transport.tcp.acknowledgment = acknowledgment.value_or(0);
    packet.transport.payload_wire_bytes = payload_bytes;
    return packet;
}

void TestTcpLedgerWrapOverlapAndKarn() {
    const auto ab = npm::NpmPacketDirection::kAToB;
    const auto ba = npm::NpmPacketDirection::kBToA;
    auto budget = std::make_shared<FixtureBudget>(npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline));
    npm::NpmTcpPerformanceTracker tracker(budget);
    const auto session = MakePerformanceSession(1);
    assert(tracker.Observe(MakeLedgerPacket(10, ab, 0xfffffff8u, 16), session) == npm::NpmTcpPerformanceError::kNone);
    assert(tracker.Observe(MakeLedgerPacket(40, ba, 500, 0, 8), session) == npm::NpmTcpPerformanceError::kNone);
    npm::NpmTcpPerformanceSnapshot snapshot;
    assert(tracker.Snapshot(1, &snapshot) == npm::NpmTcpPerformanceError::kNone);
    assert(snapshot.unique_payload_bytes_ab == 16 && snapshot.unique_payload_bytes_ba == 0);
    assert(snapshot.rtt_samples == 1 && snapshot.rtt_mean_ns == 30);
    assert(snapshot.retransmission_status == npm::NpmTcpRetransmissionStatus::kValid);
    assert(snapshot.retrans_packets_ab == 0 && snapshot.retrans_payload_bytes_ab == 0);
    assert(tracker.outstanding_segments() == 0);

    // Overlap with already ACKed history still counts as an observed retransmission.
    assert(tracker.Observe(MakeLedgerPacket(50, ab, 0, 16), session) == npm::NpmTcpPerformanceError::kNone);
    assert(tracker.Observe(MakeLedgerPacket(70, ba, 500, 0, 16), session) == npm::NpmTcpPerformanceError::kNone);
    assert(tracker.Snapshot(1, &snapshot) == npm::NpmTcpPerformanceError::kNone);
    assert(snapshot.unique_payload_bytes_ab == 24);
    assert(snapshot.retrans_packets_ab == 1 && snapshot.retrans_payload_bytes_ab == 8);
    assert(snapshot.rtt_samples == 1);  // Mixed original/retransmitted payload is excluded by Karn.

    const auto partial_session = MakePerformanceSession(2);
    assert(tracker.Observe(MakeLedgerPacket(10, ab, 100, 20), partial_session) == npm::NpmTcpPerformanceError::kNone);
    assert(tracker.Observe(MakeLedgerPacket(20, ab, 110, 20), partial_session) == npm::NpmTcpPerformanceError::kNone);
    assert(tracker.Observe(MakeLedgerPacket(30, ba, 500, 0, 130), partial_session) ==
           npm::NpmTcpPerformanceError::kNone);
    assert(tracker.Snapshot(2, &snapshot) == npm::NpmTcpPerformanceError::kNone);
    assert(snapshot.unique_payload_bytes_ab == 30);
    assert(snapshot.retrans_packets_ab == 1 && snapshot.retrans_payload_bytes_ab == 10);
    assert(snapshot.rtt_status == npm::NpmTcpRttStatus::kNoSample && snapshot.rtt_samples == 0);
    assert(tracker.Observe(MakeLedgerPacket(40, ab, 130, 10), partial_session) == npm::NpmTcpPerformanceError::kNone);
    assert(tracker.Observe(MakeLedgerPacket(80, ba, 500, 0, 140), partial_session) ==
           npm::NpmTcpPerformanceError::kNone);
    assert(tracker.Snapshot(2, &snapshot) == npm::NpmTcpPerformanceError::kNone);
    assert(snapshot.rtt_samples == 1 && snapshot.rtt_mean_ns == 40);

    // SYN consumes sequence space, but only its payload contributes to unique bytes and data RTT.
    const auto syn_session = MakePerformanceSession(3);
    assert(tracker.Observe(MakeLedgerPacket(10, ab, 0xffffffffu, 4, std::nullopt, true), syn_session) ==
           npm::NpmTcpPerformanceError::kNone);
    assert(tracker.Observe(MakeLedgerPacket(30, ba, 500, 0, 4), syn_session) == npm::NpmTcpPerformanceError::kNone);
    assert(tracker.Snapshot(3, &snapshot) == npm::NpmTcpPerformanceError::kNone);
    assert(snapshot.unique_payload_bytes_ab == 4 && snapshot.rtt_mean_ns == 20);
    tracker.Clear();
    assert(budget->Usage().module_state_bytes == 0);
}

void TestTcpLedgerOutOfOrderAckAndAmbiguity() {
    const auto ab = npm::NpmPacketDirection::kAToB;
    const auto ba = npm::NpmPacketDirection::kBToA;
    auto budget = std::make_shared<FixtureBudget>(npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline));
    npm::NpmTcpPerformanceTracker tracker(budget);
    const auto session = MakePerformanceSession(10);
    assert(tracker.Observe(MakeLedgerPacket(10, ab, 120, 10), session) == npm::NpmTcpPerformanceError::kNone);
    assert(tracker.Observe(MakeLedgerPacket(20, ab, 100, 10), session) == npm::NpmTcpPerformanceError::kNone);
    assert(tracker.Observe(MakeLedgerPacket(30, ab, 110, 10), session) == npm::NpmTcpPerformanceError::kNone);
    assert(tracker.Observe(MakeLedgerPacket(60, ba, 500, 0, 130), session) == npm::NpmTcpPerformanceError::kNone);
    npm::NpmTcpPerformanceSnapshot snapshot;
    assert(tracker.Snapshot(10, &snapshot) == npm::NpmTcpPerformanceError::kNone);
    assert(snapshot.unique_payload_bytes_ab == 30 && snapshot.retrans_packets_ab == 0);
    assert(snapshot.rtt_samples == 1 && snapshot.rtt_mean_ns == 30);
    assert(tracker.outstanding_segments() == 0);
    assert(tracker.Observe(MakeLedgerPacket(70, ab, 100, 10), session) == npm::NpmTcpPerformanceError::kNone);
    assert(tracker.Observe(MakeLedgerPacket(80, ba, 500, 0, 130), session) == npm::NpmTcpPerformanceError::kNone);
    assert(tracker.outstanding_segments() == 0);
    assert(tracker.Snapshot(10, &snapshot) == npm::NpmTcpPerformanceError::kNone);
    assert(snapshot.retrans_packets_ab == 1 && snapshot.rtt_samples == 1);

    // Direction and session ID are independent; RTT samples may come from either direction.
    assert(tracker.Observe(MakeLedgerPacket(90, ba, 500, 5), session) == npm::NpmTcpPerformanceError::kNone);
    assert(tracker.Observe(MakeLedgerPacket(120, ab, 130, 0, 505), session) == npm::NpmTcpPerformanceError::kNone);
    assert(tracker.Snapshot(10, &snapshot) == npm::NpmTcpPerformanceError::kNone);
    assert(snapshot.unique_payload_bytes_ba == 5 && snapshot.retrans_packets_ba == 0);
    assert(snapshot.rtt_samples == 2);

    const auto ambiguous = MakePerformanceSession(11);
    assert(tracker.Observe(MakeLedgerPacket(10, ab, 0, 10), ambiguous) == npm::NpmTcpPerformanceError::kNone);
    const uint64_t before = tracker.tracked_bytes();
    assert(tracker.Observe(MakeLedgerPacket(20, ab, 0x8000000au, 1), ambiguous) == npm::NpmTcpPerformanceError::kNone);
    assert(tracker.Snapshot(11, &snapshot) == npm::NpmTcpPerformanceError::kNone);
    assert((snapshot.measurement_flags & npm::kNpmMeasurementSequenceAmbiguous) != 0);
    assert(snapshot.rtt_status == npm::NpmTcpRttStatus::kAmbiguous);
    assert(snapshot.retransmission_status == npm::NpmTcpRetransmissionStatus::kAmbiguous);
    assert(!snapshot.unique_payload_bytes_ab && !snapshot.unique_payload_bytes_ba);
    assert(!snapshot.retrans_packets_ab && !snapshot.retrans_payload_bytes_ab && !snapshot.rtt_samples);
    assert(tracker.tracked_bytes() < before && tracker.outstanding_segments() == 0);
    assert(tracker.Observe(MakeLedgerPacket(30, ab, 10, 10), ambiguous) == npm::NpmTcpPerformanceError::kNone);
    assert(tracker.Snapshot(11, &snapshot) == npm::NpmTcpPerformanceError::kNone);
    assert(!snapshot.unique_payload_bytes_ab && snapshot.rtt_status == npm::NpmTcpRttStatus::kAmbiguous);
    assert(tracker.Snapshot(10, &snapshot) == npm::NpmTcpPerformanceError::kNone);
    assert(snapshot.unique_payload_bytes_ab == 30 && snapshot.rtt_samples == 2);

    const auto ambiguous_ack = MakePerformanceSession(12);
    assert(tracker.Observe(MakeLedgerPacket(10, ab, 0, 10), ambiguous_ack) == npm::NpmTcpPerformanceError::kNone);
    assert(tracker.Observe(MakeLedgerPacket(20, ba, 500, 0, 0x8000000au), ambiguous_ack) ==
           npm::NpmTcpPerformanceError::kNone);
    assert(tracker.Snapshot(12, &snapshot) == npm::NpmTcpPerformanceError::kNone);
    assert((snapshot.measurement_flags & npm::kNpmMeasurementSequenceAmbiguous) != 0);
    assert(snapshot.retransmission_status == npm::NpmTcpRetransmissionStatus::kAmbiguous);
    assert(!snapshot.unique_payload_bytes_ab && !snapshot.rtt_samples);
    tracker.Remove(11);
    tracker.Clear();
    assert(budget->Usage().module_state_bytes == 0);
}

void TestTcpLedgerLimitsAndBudgetAtomicity() {
    const auto ab = npm::NpmPacketDirection::kAToB;
    auto config = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline);
    auto budget = std::make_shared<FixtureBudget>(config);
    npm::NpmTcpPerformanceTracker limited(budget, 8);
    const auto session = MakePerformanceSession(1);
    for (uint32_t index = 0; index < 4; ++index) {
        assert(limited.Observe(MakeLedgerPacket(10 + index, ab, 100 + index * 20, 10), session) ==
               npm::NpmTcpPerformanceError::kNone);
    }
    const uint64_t before = limited.tracked_bytes();
    assert(limited.Observe(MakeLedgerPacket(100, ab, 180, 10), session) ==
           npm::NpmTcpPerformanceError::kRangeLimitExceeded);
    assert(limited.tracked_bytes() == before && limited.outstanding_segments() == 4);
    npm::NpmTcpPerformanceSnapshot snapshot;
    assert(limited.Snapshot(1, &snapshot) == npm::NpmTcpPerformanceError::kNone);
    assert(snapshot.unique_payload_bytes_ab == 40 && snapshot.retrans_packets_ab == 0);
    assert(limited.Observe(MakeLedgerPacket(50, ab, 100, 70), session) == npm::NpmTcpPerformanceError::kNone);
    assert(limited.Snapshot(1, &snapshot) == npm::NpmTcpPerformanceError::kNone);
    assert(snapshot.unique_payload_bytes_ab == 70 && snapshot.retrans_payload_bytes_ab == 40);
    assert((snapshot.measurement_flags & npm::kNpmMeasurementTimestampRegression) == 0);
    limited.Clear();
    assert(budget->Usage().module_state_bytes == 0);

    npm::NpmTcpPerformanceTracker invalid(budget, 0);
    assert(invalid.Observe(MakeLedgerPacket(10, ab, 100, 10), session) ==
           npm::NpmTcpPerformanceError::kInvalidRangeLimit);
    npm::NpmTcpPerformanceTracker no_room(budget, 1);
    assert(no_room.Observe(MakeLedgerPacket(10, ab, 100, 10), session) ==
           npm::NpmTcpPerformanceError::kInvalidRangeLimit);
    assert(no_room.size() == 0 && no_room.tracked_bytes() == 0);

    // Allow state + one node: the second node reservation must fail without publishing history/state.
    uint64_t state_charge = 0;
    uint64_t node_charge = 0;
    {
        npm::NpmTcpPerformanceTracker probe(budget);
        assert(probe.Observe(MakeLedgerPacket(10, ab, 100, 0, std::nullopt, true), session) ==
               npm::NpmTcpPerformanceError::kNone);
        state_charge = probe.tracked_bytes();
        assert(probe.Observe(MakeLedgerPacket(20, ab, 101, 10), session) == npm::NpmTcpPerformanceError::kNone);
        node_charge = (probe.tracked_bytes() - state_charge) / 2;
        assert(node_charge > 0);
    }
    assert(budget->Usage().module_state_bytes == 0);
    config.max_tracked_bytes = state_charge + node_charge;
    auto tight_budget = std::make_shared<FixtureBudget>(config);
    npm::NpmTcpPerformanceTracker tight(tight_budget);
    assert(tight.Observe(MakeLedgerPacket(100, ab, 101, 10), session) == npm::NpmTcpPerformanceError::kBudgetExceeded);
    assert(tight.size() == 0 && tight.tracked_bytes() == 0);
    assert(tight_budget->Usage().module_state_bytes == 0);
    assert(tight.Observe(MakeLedgerPacket(10, ab, 100, 0, std::nullopt, true), session) ==
           npm::NpmTcpPerformanceError::kNone);
    assert(tight.Observe(MakeLedgerPacket(100, ab, 101, 10), session) == npm::NpmTcpPerformanceError::kBudgetExceeded);
    assert(tight.tracked_bytes() == state_charge && tight.outstanding_segments() == 0);
    assert(tight.Snapshot(1, &snapshot) == npm::NpmTcpPerformanceError::kNone);
    assert(snapshot.unique_payload_bytes_ab == 0 && snapshot.retrans_packets_ab == 0);
    tight.Clear();
    assert(tight_budget->Usage().module_state_bytes == 0);
}

void TestSessionTrackerPeakStateAndRangeProfile() {
    constexpr uint64_t session_count = 128;
    constexpr uint32_t ranges_per_session = 4;
    auto budget = std::make_shared<FixtureBudget>(npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline));
    npm::NpmTcpPerformanceTracker tracker(budget, 1024);
    uint64_t peak_module_bytes = 0;
    size_t peak_sessions = 0;
    size_t peak_outstanding = 0;

    for (uint64_t session_id = 1; session_id <= session_count; ++session_id) {
        const auto session = MakePerformanceSession(session_id);
        for (uint32_t range = 0; range < ranges_per_session; ++range) {
            const int64_t timestamp = static_cast<int64_t>((session_id - 1) * ranges_per_session + range + 1);
            assert(tracker.Observe(MakeLedgerPacket(timestamp, npm::NpmPacketDirection::kAToB, 100 + range * 20, 10),
                                   session) == npm::NpmTcpPerformanceError::kNone);
            const auto usage = budget->Usage();
            assert(usage.module_state_bytes == tracker.tracked_bytes());
            assert(usage.session_state_bytes == 0 && usage.input_batch_bytes == 0 && usage.pending_output_bytes == 0);
            peak_module_bytes = std::max(peak_module_bytes, usage.module_state_bytes);
            peak_sessions = std::max(peak_sessions, tracker.size());
            peak_outstanding = std::max(peak_outstanding, tracker.outstanding_segments());
        }
    }
    assert(peak_sessions == session_count);
    assert(peak_outstanding == session_count * ranges_per_session);
    assert(peak_module_bytes > 0 && peak_module_bytes == tracker.tracked_bytes());
    std::fprintf(stderr, "session_tracker_profile,sessions=%zu,outstanding_ranges=%zu,module_state_bytes=%llu\n",
                 peak_sessions, peak_outstanding, static_cast<unsigned long long>(peak_module_bytes));
    tracker.Clear();
    assert(tracker.size() == 0 && tracker.outstanding_segments() == 0 && tracker.tracked_bytes() == 0);
    assert(budget->Usage().module_state_bytes == 0);
}

npm::NpmPacketView MakeUdpPerformancePacket(int64_t timestamp_ns, npm::NpmPacketDirection direction,
                                            uint32_t payload_wire_bytes, uint32_t wire_bytes,
                                            bool payload_complete = true) {
    npm::NpmPacketView packet;
    packet.packet.meta.timestamp_ns = timestamp_ns;
    packet.packet.meta.wire_len = wire_bytes;
    packet.direction = direction;
    packet.transport.payload_wire_bytes = payload_wire_bytes;
    packet.transport.payload_captured_bytes = payload_complete ? payload_wire_bytes : payload_wire_bytes / 2;
    packet.transport.payload_complete = payload_complete;
    return packet;
}

npm::NpmSessionView MakeTransportSnapshotView(npm::NpmSessionKey* key, uint64_t session_id, int64_t first_ns,
                                              int64_t last_ns, uint64_t packets_ab, uint64_t packets_ba,
                                              uint64_t wire_bytes_ab, uint64_t wire_bytes_ba) {
    npm::NpmSessionView session;
    session.session_id = session_id;
    session.key = key;
    session.first_ns = first_ns;
    session.last_ns = last_ns;
    session.packets_ab = packets_ab;
    session.packets_ba = packets_ba;
    session.wire_bytes_ab = wire_bytes_ab;
    session.wire_bytes_ba = wire_bytes_ba;
    return session;
}

void TestSessionPerformanceUdpProjectionAndRates() {
    npm::NpmSessionKey udp_key;
    udp_key.transport_protocol = flowsql::ipv4::eNext::UDP;
    auto budget = std::make_shared<FixtureBudget>(npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline));
    npm::NpmTcpPerformanceTracker tracker(budget);
    auto session = MakeTransportSnapshotView(&udp_key, 1, 100, 100, 0, 0, 0, 0);
    assert(tracker.Observe(MakeUdpPerformancePacket(100, npm::NpmPacketDirection::kAToB, 20, 120, false), session) ==
           npm::NpmTcpPerformanceError::kNone);
    assert(tracker.Observe(MakeUdpPerformancePacket(200, npm::NpmPacketDirection::kBToA, 10, 80), session) ==
           npm::NpmTcpPerformanceError::kNone);

    session = MakeTransportSnapshotView(&udp_key, 1, 100, 200, 1, 1, 120, 80);
    npm::NpmTcpPerformanceSnapshot snapshot;
    assert(tracker.Snapshot(session, &snapshot) == npm::NpmTcpPerformanceError::kNone);
    assert(snapshot.payload_bytes_ab == 20 && snapshot.payload_bytes_ba == 10);
    assert(snapshot.rate_status == npm::NpmRateStatus::kValid);
    assert(snapshot.wire_bps_ab == 9'600'000'000.0 && snapshot.wire_bps_ba == 6'400'000'000.0);
    assert(snapshot.payload_bps_ab == 1'600'000'000.0 && snapshot.payload_bps_ba == 800'000'000.0);
    assert(snapshot.handshake_status == npm::NpmTcpHandshakeStatus::kNotApplicable);
    assert(snapshot.rtt_status == npm::NpmTcpRttStatus::kNotApplicable);
    assert(snapshot.retransmission_status == npm::NpmTcpRetransmissionStatus::kNotApplicable);
    assert(!snapshot.initiator && !snapshot.handshake_duration_ns && !snapshot.synack_rtt_ns);
    assert(!snapshot.rtt_samples && !snapshot.unique_payload_bytes_ab && !snapshot.unique_payload_bytes_ba);
    assert(!snapshot.unique_payload_bps_ab && !snapshot.unique_payload_bps_ba);
    assert(!snapshot.retrans_packets_ab && !snapshot.retrans_packets_ba);
    assert((snapshot.measurement_flags & npm::kNpmMeasurementTruncatedPayload) != 0);
    assert((snapshot.measurement_flags & (npm::kNpmMeasurementMidstreamStart | npm::kNpmMeasurementSequenceAmbiguous |
                                          npm::kNpmMeasurementSynRetransmitted)) == 0);

    auto zero_span = MakeTransportSnapshotView(&udp_key, 2, 300, 300, 0, 0, 0, 0);
    assert(tracker.Observe(MakeUdpPerformancePacket(300, npm::NpmPacketDirection::kAToB, 5, 30), zero_span) ==
           npm::NpmTcpPerformanceError::kNone);
    zero_span = MakeTransportSnapshotView(&udp_key, 2, 300, 300, 1, 0, 30, 0);
    assert(tracker.Snapshot(zero_span, &snapshot) == npm::NpmTcpPerformanceError::kNone);
    assert(snapshot.rate_status == npm::NpmRateStatus::kInsufficientSpan);
    assert(!snapshot.wire_bps_ab && !snapshot.wire_bps_ba);
    assert(!snapshot.payload_bps_ab && !snapshot.payload_bps_ba);
    tracker.Clear();
    assert(budget->Usage().module_state_bytes == 0);
}

void TestSessionPerformanceTcpRatesAndSequenceAmbiguity() {
    npm::NpmSessionKey tcp_key;
    tcp_key.transport_protocol = flowsql::ipv4::eNext::TCP;
    auto budget = std::make_shared<FixtureBudget>(npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline));
    npm::NpmTcpPerformanceTracker tracker(budget);
    auto session = MakeTransportSnapshotView(&tcp_key, 10, 0, 0, 0, 0, 0, 0);
    auto first = MakeLedgerPacket(0, npm::NpmPacketDirection::kAToB, 100, 20);
    first.packet.meta.wire_len = 100;
    auto overlap = MakeLedgerPacket(1'000'000'000, npm::NpmPacketDirection::kAToB, 110, 20);
    overlap.packet.meta.wire_len = 100;
    assert(tracker.Observe(first, session) == npm::NpmTcpPerformanceError::kNone);
    assert(tracker.Observe(overlap, session) == npm::NpmTcpPerformanceError::kNone);
    session = MakeTransportSnapshotView(&tcp_key, 10, 0, 1'000'000'000, 2, 0, 200, 0);
    npm::NpmTcpPerformanceSnapshot snapshot;
    assert(tracker.Snapshot(session, &snapshot) == npm::NpmTcpPerformanceError::kNone);
    assert(snapshot.payload_bytes_ab == 40 && snapshot.payload_bytes_ba == 0);
    assert(snapshot.unique_payload_bytes_ab == 30 && snapshot.unique_payload_bytes_ba == 0);
    assert(snapshot.wire_bps_ab == 1600.0 && snapshot.payload_bps_ab == 320.0);
    assert(snapshot.unique_payload_bps_ab == 240.0 && snapshot.unique_payload_bps_ba == 0.0);
    assert(snapshot.retrans_packets_ab == 1 && snapshot.retrans_payload_bytes_ab == 10);

    const auto ambiguous_session = MakePerformanceSession(11);
    assert(tracker.Observe(MakeLedgerPacket(0, npm::NpmPacketDirection::kAToB, 0, 10), ambiguous_session) ==
           npm::NpmTcpPerformanceError::kNone);
    assert(tracker.Observe(MakeLedgerPacket(1, npm::NpmPacketDirection::kAToB, 0x8000000au, 1), ambiguous_session) ==
           npm::NpmTcpPerformanceError::kNone);
    auto ambiguous_view = MakeTransportSnapshotView(&tcp_key, 11, 0, 1'000'000'000, 2, 0, 80, 0);
    assert(tracker.Snapshot(ambiguous_view, &snapshot) == npm::NpmTcpPerformanceError::kNone);
    assert(snapshot.rate_status == npm::NpmRateStatus::kValid && snapshot.payload_bps_ab == 88.0);
    assert(!snapshot.unique_payload_bytes_ab && !snapshot.unique_payload_bps_ab);
    assert(snapshot.retransmission_status == npm::NpmTcpRetransmissionStatus::kAmbiguous);
    tracker.Clear();
    assert(budget->Usage().module_state_bytes == 0);
}

void TestSessionPerformanceProtocolAndViewErrorsAreAtomic() {
    npm::NpmSessionKey udp_key;
    udp_key.transport_protocol = flowsql::ipv4::eNext::UDP;
    npm::NpmSessionKey tcp_key;
    tcp_key.transport_protocol = flowsql::ipv4::eNext::TCP;
    auto budget = std::make_shared<FixtureBudget>(npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline));
    npm::NpmTcpPerformanceTracker tracker(budget);
    auto udp = MakeTransportSnapshotView(&udp_key, 1, 10, 10, 0, 0, 0, 0);
    assert(tracker.Observe(MakeUdpPerformancePacket(10, npm::NpmPacketDirection::kAToB, 5, 20), udp) ==
           npm::NpmTcpPerformanceError::kNone);
    const uint64_t before = tracker.tracked_bytes();
    auto tcp_packet = MakeLedgerPacket(20, npm::NpmPacketDirection::kAToB, 100, 10);
    assert(tracker.Observe(tcp_packet, udp) == npm::NpmTcpPerformanceError::kProtocolMismatch);
    assert(tracker.tracked_bytes() == before && tracker.size() == 1);
    npm::NpmTcpPerformanceSnapshot snapshot;
    assert(tracker.Snapshot(1, &snapshot) == npm::NpmTcpPerformanceError::kNone);
    assert(snapshot.payload_bytes_ab == 5);

    auto wrong_protocol = MakeTransportSnapshotView(&tcp_key, 1, 10, 20, 1, 0, 20, 0);
    assert(tracker.Snapshot(wrong_protocol, &snapshot) == npm::NpmTcpPerformanceError::kProtocolMismatch);
    assert(snapshot.payload_bytes_ab == 5 && snapshot.handshake_status == npm::NpmTcpHandshakeStatus::kNotApplicable);
    auto invalid_time = MakeTransportSnapshotView(&udp_key, 1, 20, 10, 1, 0, 20, 0);
    assert(tracker.Snapshot(invalid_time, &snapshot) == npm::NpmTcpPerformanceError::kSessionViewMismatch);
    auto missing_packet = MakeTransportSnapshotView(&udp_key, 1, 10, 20, 0, 0, 20, 0);
    assert(tracker.Snapshot(missing_packet, &snapshot) == npm::NpmTcpPerformanceError::kSessionViewMismatch);
    auto missing_wire = MakeTransportSnapshotView(&udp_key, 1, 10, 20, 1, 0, 19, 0);
    assert(tracker.Snapshot(missing_wire, &snapshot) == npm::NpmTcpPerformanceError::kSessionViewMismatch);
    assert(tracker.Snapshot(MakePerformanceSession(1), &snapshot) == npm::NpmTcpPerformanceError::kSessionViewMismatch);
    assert(snapshot.payload_bytes_ab == 5 && snapshot.handshake_status == npm::NpmTcpHandshakeStatus::kNotApplicable);
    assert(tracker.tracked_bytes() == before && budget->Usage().module_state_bytes == before);
    tracker.Clear();
}

npm::NpmSessionKey MakeSessionModuleKey(uint8_t transport_protocol) {
    npm::NpmSessionKey key;
    key.input_namespace = "pcapfile.capture";
    key.observation_domain_id = 77;
    key.ip_family = flowsql::packet::AddressFamily::kIPv4;
    key.transport_protocol = transport_protocol;
    key.a.ip = ParseIpv4("192.0.2.1");
    key.a.port = 50000;
    key.b.ip = ParseIpv4("198.51.100.2");
    key.b.port = 443;
    return key;
}

void TestSessionAnalysisModuleTcpRevisionsAndWriterAtomicity() {
    static_assert(std::is_final_v<npm::NpmSessionAnalysisModule>);
    static_assert(!std::is_copy_constructible_v<npm::NpmSessionAnalysisModule>);
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    std::unique_ptr<npm::NpmProtocolContext> context;
    assert(npm::NpmProtocolContext::Create(&querier, &context) == npm::NpmProtocolContextError::kNone);
    auto budget = std::make_shared<FixtureBudget>(npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline));
    npm::NpmSessionAnalysisModule module(*context, budget, 8);
    auto key = MakeSessionModuleKey(flowsql::ipv4::eNext::TCP);
    auto session = MakeTransportSnapshotView(&key, 1, 100, 200, 1, 0, 100, 0);
    session.protocol_status = npm::NpmProtocolStatus::kIdentified;
    session.protocol_id = 7;
    session.protocol_sub_id = 8;
    auto packet = MakeLedgerPacket(100, npm::NpmPacketDirection::kAToB, 100, 20);
    packet.packet.meta.wire_len = 100;
    FixtureWriter writer;
    assert(module.OnPacket(packet, session, writer) == 0);
    assert(writer.session_accepted == 0 && module.tracked_sessions() == 1);
    assert(module.tracked_bytes() == budget->Usage().module_state_bytes);

    assert(module.OnSessionSnapshot(session, 1000, writer) == 0);
    assert(writer.session_accepted == 1 && writer.last_session_result.revision == 1);
    assert(!writer.last_session_result.is_final && !writer.last_session_result.end_reason);
    assert(writer.last_session_result.protocol == "SUB");
    assert(writer.last_session_result.payload_bytes_ab == 20);
    assert(writer.last_session_result.tcp_unique_payload_bytes_ab == 20);
    assert(writer.last_session_result.rate_status == npm::NpmRateStatus::kValid);

    assert(module.OnSessionSnapshot(session, 1100, writer) == 0);
    assert(writer.last_session_result.revision == 2);
    writer.next_error = EIO;
    assert(module.OnSessionSnapshot(session, 1200, writer) == EIO);
    assert(module.tracked_sessions() == 1);
    writer.next_error = 0;
    assert(module.OnSessionSnapshot(session, 1300, writer) == 0);
    assert(writer.last_session_result.revision == 3);

    auto second_session = session;
    second_session.session_id = 5;
    assert(module.OnPacket(packet, second_session, writer) == 0);
    assert(module.OnSessionSnapshot(second_session, 1350, writer) == 0);
    assert(writer.last_session_result.revision == 1);

    assert(module.OnSessionEnd(session, npm::NpmSessionEndReason::kClosed, 1400, writer) == 0);
    assert(writer.last_session_result.revision == 4 && writer.last_session_result.is_final);
    assert(writer.last_session_result.end_reason == npm::NpmSessionEndReason::kClosed);
    assert(module.tracked_sessions() == 1 && module.tracked_bytes() > 0);
    assert(module.OnSessionEnd(second_session, npm::NpmSessionEndReason::kClosed, 1450, writer) == 0);
    assert(writer.last_session_result.revision == 2 && writer.last_session_result.is_final);
    assert(module.tracked_sessions() == 0 && module.tracked_bytes() == 0);
    assert(budget->Usage().module_state_bytes == 0);
    assert(module.OnSessionSnapshot(session, 1500, writer) == ENOENT);
}

void TestSessionAnalysisModuleUdpFinalOnlyAndInvalidFinal() {
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    std::unique_ptr<npm::NpmProtocolContext> context;
    assert(npm::NpmProtocolContext::Create(&querier, &context) == npm::NpmProtocolContextError::kNone);
    auto budget = std::make_shared<FixtureBudget>(npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline));
    npm::NpmSessionAnalysisModule module(*context, budget, 8);
    auto key = MakeSessionModuleKey(flowsql::ipv4::eNext::UDP);
    key.b.port = 53;
    auto session = MakeTransportSnapshotView(&key, 2, 10, 10, 0, 1, 0, 80);
    session.protocol_status = npm::NpmProtocolStatus::kUnknown;
    const auto packet = MakeUdpPerformancePacket(10, npm::NpmPacketDirection::kBToA, 12, 80, false);
    FixtureWriter writer;
    assert(module.OnPacket(packet, session, writer) == 0);
    assert(module.OnSessionEnd(session, npm::NpmSessionEndReason::kEof, 20, writer) == 0);
    assert(writer.session_accepted == 1 && writer.last_session_result.revision == 1);
    assert(writer.last_session_result.tcp_handshake_status == npm::NpmTcpHandshakeStatus::kNotApplicable);
    assert(writer.last_session_result.tcp_rtt_status == npm::NpmTcpRttStatus::kNotApplicable);
    assert(writer.last_session_result.tcp_retransmission_status == npm::NpmTcpRetransmissionStatus::kNotApplicable);
    assert(writer.last_session_result.payload_bytes_ba == 12);
    assert((writer.last_session_result.measurement_flags & npm::kNpmMeasurementTruncatedPayload) != 0);
    assert(module.tracked_sessions() == 0 && budget->Usage().module_state_bytes == 0);

    session.session_id = 3;
    session.protocol_status = npm::NpmProtocolStatus::kPending;
    assert(module.OnPacket(packet, session, writer) == 0);
    const uint64_t before = module.tracked_bytes();
    assert(module.OnSessionEnd(session, npm::NpmSessionEndReason::kEof, 30, writer) == EINVAL);
    assert(module.tracked_sessions() == 1 && module.tracked_bytes() >= before);
    assert(writer.session_accepted == 1);
    module.Clear();
    assert(module.tracked_sessions() == 0 && budget->Usage().module_state_bytes == 0);
}

void TestSessionAnalysisModuleRevisionBudgetAndDestructorCleanup() {
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    std::unique_ptr<npm::NpmProtocolContext> context;
    assert(npm::NpmProtocolContext::Create(&querier, &context) == npm::NpmProtocolContextError::kNone);
    auto key = MakeSessionModuleKey(flowsql::ipv4::eNext::UDP);
    auto session = MakeTransportSnapshotView(&key, 4, 10, 10, 1, 0, 30, 0);
    session.protocol_status = npm::NpmProtocolStatus::kUnknown;
    const auto packet = MakeUdpPerformancePacket(10, npm::NpmPacketDirection::kAToB, 5, 30);
    FixtureWriter writer;

    uint64_t tracker_bytes = 0;
    auto probe_budget = std::make_shared<FixtureBudget>(npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline));
    {
        npm::NpmSessionAnalysisModule probe(*context, probe_budget, 8);
        assert(probe.OnPacket(packet, session, writer) == 0);
        tracker_bytes = probe.tracked_bytes();
        assert(tracker_bytes > 0);
    }
    assert(probe_budget->Usage().module_state_bytes == 0);

    auto config = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline);
    config.max_tracked_bytes = tracker_bytes;
    auto tight_budget = std::make_shared<FixtureBudget>(config);
    {
        npm::NpmSessionAnalysisModule tight(*context, tight_budget, 8);
        assert(tight.OnPacket(packet, session, writer) == 0);
        assert(tight.OnSessionSnapshot(session, 100, writer) == ENOSPC);
        assert(tight.tracked_sessions() == 1 && tight.tracked_bytes() == tracker_bytes);
        assert(writer.session_accepted == 0);
    }
    assert(tight_budget->Usage().module_state_bytes == 0);

    auto cleanup_budget = std::make_shared<FixtureBudget>(npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline));
    {
        npm::NpmSessionAnalysisModule cleanup(*context, cleanup_budget, 8);
        session.session_id = 5;
        assert(cleanup.OnPacket(packet, session, writer) == 0);
        const uint64_t tracker_only_bytes = cleanup.tracked_bytes();
        assert(cleanup.OnSessionSnapshot(session, 200, writer) == 0);
        assert(cleanup.tracked_bytes() > tracker_only_bytes);
        assert(cleanup.tracked_bytes() == cleanup_budget->Usage().module_state_bytes);
    }
    assert(cleanup_budget->Usage().module_state_bytes == 0);
}

npm::NpmSessionTableError ObserveActive(npm::NpmSessionTable& table, const npm::NpmSessionPacketBinding& binding,
                                        const flowsql::packet::PacketMeta& meta, npm::NpmSessionView* output) {
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

    assert(ObserveActive(table, reverse_from_other_queue, reverse_meta, &view) == npm::NpmSessionTableError::kNone);
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
uint64_t ResultBufferCapacity(const std::shared_ptr<arrow::RecordBatch>& batch) {
    uint64_t bytes = 0;
    for (const auto& column : batch->columns())
        for (const auto& buffer : column->data()->buffers)
            if (buffer) bytes += buffer->capacity();
    return bytes;
}

void AssertTaskBudgetUsage(const npm::NpmBudgetUsage& usage, uint64_t session, uint64_t module, uint64_t input,
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
    assert(limited_budget->Reserve(npm::NpmBudgetCategory::kModuleState, config.max_tracked_bytes - short_charge + 1) ==
           npm::NpmBudgetError::kNone);
    npm::NpmSessionTable limited(config, limited_budget);
    npm::NpmSessionObserveResult unchanged;
    unchanged.has_active_session = true;
    unchanged.active_session.session_id = 99;
    unchanged.ended_sessions.emplace_back().session_id = 88;
    assert(limited.Observe(short_binding, short_meta, &unchanged) == npm::NpmSessionTableError::kSessionBudgetExceeded);
    assert(limited.size() == 0 && limited.tracked_bytes() == 0);
    assert(unchanged.has_active_session && unchanged.active_session.session_id == 99);
    assert(unchanged.ended_sessions.size() == 1 && unchanged.ended_sessions[0].session_id == 88);
    AssertTaskBudgetUsage(limited_budget->Usage(), 0, config.max_tracked_bytes - short_charge + 1, 0, 0);

    assert(limited_budget->Release(npm::NpmBudgetCategory::kModuleState, config.max_tracked_bytes - short_charge + 1) ==
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

    const auto rst_packet = MakeIpv4TcpPacket("198.51.100.1", 51000, "198.51.100.2", 8443, {}, kTcpRst, 200);
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

npm::NpmCaptureProgressUpdate RealtimeProgress(int64_t capture_time_ns, bool packet_observed,
                                               bool source_idle_confirmed, bool source_backlog_known,
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
    const auto middle_packet = MakeIpv4TcpPacket("192.0.2.1", 50000, "192.0.2.2", 443, {}, kTcpAck, 1000);
    const auto initial_syn = MakeIpv4TcpPacket("192.0.2.1", 50000, "192.0.2.2", 443, {}, kTcpSyn, 100);
    const auto syn_ack = MakeIpv4TcpPacket("192.0.2.2", 443, "192.0.2.1", 50000, {}, kTcpSyn | kTcpAck, 500);
    const auto changed_syn = MakeIpv4TcpPacket("192.0.2.1", 50000, "192.0.2.2", 443, {}, kTcpSyn, 101);
    const auto reverse_syn = MakeIpv4TcpPacket("192.0.2.2", 443, "192.0.2.1", 50000, {}, kTcpSyn, 700);

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

    assert(middle.transport.tcp.valid && !middle.transport.tcp.syn && middle.transport.tcp.ack &&
           middle.transport.tcp.sequence == 1000);
    assert(syn.transport.tcp.valid && syn.transport.tcp.syn && !syn.transport.tcp.ack &&
           syn.transport.tcp.sequence == 100);
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
    const auto data_packet = MakeIpv4TcpPacket("192.0.2.1", 50000, "192.0.2.2", 443, {}, kTcpAck, 100);
    const auto forward_fin = MakeIpv4TcpPacket("192.0.2.1", 50000, "192.0.2.2", 443, {}, kTcpFin | kTcpAck, 200);
    const auto reverse_fin = MakeIpv4TcpPacket("192.0.2.2", 443, "192.0.2.1", 50000, {}, kTcpFin | kTcpAck, 300);
    const auto reverse_rst = MakeIpv4TcpPacket("192.0.2.2", 443, "192.0.2.1", 50000, {}, kTcpRst | kTcpAck, 400);

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
    assert(fin_table.Observe(fin_retransmit, fin_retransmit_meta, &result) == npm::NpmSessionTableError::kNone);
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
    assert(!udp.transport.tcp.valid);
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
    RecordingEndModule(uint64_t marker, std::vector<uint64_t>* calls, std::vector<npm::NpmSessionEndReason>* reasons,
                       std::vector<int64_t>* observed_ats, bool write_result, int error)
        : marker_(marker),
          calls_(calls),
          reasons_(reasons),
          observed_ats_(observed_ats),
          write_result_(write_result),
          error_(error) {}

    int OnPacket(const npm::NpmPacketView&, const npm::NpmSessionView&, npm::INpmResultWriter&) override { return 0; }

    int OnSessionSnapshot(const npm::NpmSessionView&, int64_t, npm::INpmResultWriter&) override { return 0; }

    int OnSessionEnd(const npm::NpmSessionView& session, npm::NpmSessionEndReason reason, int64_t observed_at_ns,
                     npm::INpmResultWriter& writer) override {
        assert(session.key != nullptr);
        calls_->push_back(session.session_id * 10 + marker_);
        reasons_->push_back(reason);
        observed_ats_->push_back(observed_at_ns);
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
    std::vector<int64_t>* observed_ats_ = nullptr;
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
    std::vector<int64_t> observed_ats;
    RecordingEndModule first(1, &calls, &reasons, &observed_ats, true, 0);
    RecordingEndModule second(2, &calls, &reasons, &observed_ats, true, 0);
    std::vector<npm::INpmAnalysisModule*> modules{&first, &second};
    FixtureWriter writer;
    assert(npm::NotifyNpmSessionEnd(ended_sessions, modules, 5000, writer) == 0);
    assert((calls == std::vector<uint64_t>{11, 12, 21, 22}));
    assert((reasons == std::vector<npm::NpmSessionEndReason>{
                           npm::NpmSessionEndReason::kClosed, npm::NpmSessionEndReason::kClosed,
                           npm::NpmSessionEndReason::kTupleReuse, npm::NpmSessionEndReason::kTupleReuse}));
    assert((observed_ats == std::vector<int64_t>{5000, 5000, 5000, 5000}));
    assert(writer.accepted == 4);

    calls.clear();
    reasons.clear();
    observed_ats.clear();
    RecordingEndModule succeeds(1, &calls, &reasons, &observed_ats, false, 0);
    RecordingEndModule fails(2, &calls, &reasons, &observed_ats, false, EBUSY);
    RecordingEndModule skipped(3, &calls, &reasons, &observed_ats, false, 0);
    modules = {&succeeds, &fails, &skipped};
    assert(npm::NotifyNpmSessionEnd({ended_sessions[0]}, modules, 6000, writer) == EBUSY);
    assert((calls == std::vector<uint64_t>{11, 12}));
    assert((observed_ats == std::vector<int64_t>{6000, 6000}));

    calls.clear();
    reasons.clear();
    observed_ats.clear();
    RecordingEndModule writes(1, &calls, &reasons, &observed_ats, true, 0);
    RecordingEndModule after_writer(2, &calls, &reasons, &observed_ats, false, 0);
    modules = {&writes, &after_writer};
    writer.next_error = EIO;
    assert(npm::NotifyNpmSessionEnd({ended_sessions[0]}, modules, 7000, writer) == EIO);
    assert((calls == std::vector<uint64_t>{11}));
    assert((observed_ats == std::vector<int64_t>{7000}));
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
    const auto empty_packet = MakeIpv4TcpPacket("192.0.2.1", 50000, "192.0.2.2", 443, {}, kTcpAck, 100);
    const auto payload_packet = MakeIpv4TcpPacket("192.0.2.1", 50000, "192.0.2.2", 443, {1, 2}, kTcpAck, 101);
    const auto rst_packet = MakeIpv4TcpPacket("192.0.2.1", 50000, "192.0.2.2", 443, {}, kTcpRst | kTcpAck, 102);

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
    SequenceProtocolIdentifier identifier(
        {{flowsql::packet::ProtocolStatus::kUnknown, 0, 0}, {flowsql::packet::ProtocolStatus::kIdentified, 7, 8}});

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
    const auto middle_packet = MakeIpv4TcpPacket("192.0.2.1", 50000, "192.0.2.2", 443, {1}, kTcpAck, 100);
    const auto syn_packet = MakeIpv4TcpPacket("192.0.2.1", 50000, "192.0.2.2", 443, {2}, kTcpSyn, 200);
    const auto rst_packet = MakeIpv4TcpPacket("192.0.2.2", 443, "192.0.2.1", 50000, {}, kTcpRst | kTcpAck, 300);

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

    const auto rst_packet = MakeIpv4TcpPacket("192.0.2.1", 50000, "192.0.2.2", 443, {}, kTcpRst | kTcpAck, 100);
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
    const auto pending_packet = MakeIpv4TcpPacket("192.0.2.1", 50000, "192.0.2.2", 443, {}, kTcpAck, 100);
    const auto identified_packet = MakeIpv6UdpPacket("2001:db8::1", 53000, "2001:db8::2", 53, {1, 2});
    const auto closed_packet = MakeIpv4TcpPacket("198.51.100.1", 51000, "198.51.100.2", 8443, {}, kTcpRst, 200);

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
    SequenceProtocolIdentifier identifier({{flowsql::packet::ProtocolStatus::kIdentified, 7, 8}});
    npm::NpmSessionView sampled;
    assert(table.SampleProtocol(identified.key, observed.active_session.session_id, identified_view, identifier,
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
    PacketProcessorRecordingModule(uint64_t marker, std::vector<uint64_t>* events) : marker_(marker), events_(events) {}

    int OnPacket(const npm::NpmPacketView& packet, const npm::NpmSessionView& session,
                 npm::INpmResultWriter&) override {
        events_->push_back(2000 + session.session_id * 10 + marker_);
        packet_sequences.push_back(packet.packet.meta.sequence);
        packet_timestamps.push_back(packet.packet.meta.timestamp_ns);
        transport_facts.push_back(packet.transport);
        packet_matches_expectation = packet.packet.bytes.data == expected_packet_data &&
                                     packet.layer == expected_layer && packet.payload.size == expected_payload_size &&
                                     (expected_payload_size == 0 || packet.payload[0] == expected_payload_first) &&
                                     packet.direction == expected_direction;
        packet_protocol_statuses.push_back(session.protocol_status);
        packet_label_ids.push_back(session.primary_label_id);
        return packet_error;
    }

    int OnSessionSnapshot(const npm::NpmSessionView&, int64_t, npm::INpmResultWriter&) override { return 0; }

    int OnSessionEnd(const npm::NpmSessionView& session, npm::NpmSessionEndReason reason, int64_t observed_at_ns,
                     npm::INpmResultWriter&) override {
        events_->push_back(1000 + session.session_id * 10 + marker_);
        end_reasons.push_back(reason);
        end_observed_ats.push_back(observed_at_ns);
        end_protocol_statuses.push_back(session.protocol_status);
        end_protocol_ids.push_back(session.protocol_id);
        end_protocol_sub_ids.push_back(session.protocol_sub_id);
        end_label_ids.push_back(session.primary_label_id);
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
    std::vector<npm::NpmTransportPacketFacts> transport_facts;
    std::vector<npm::NpmProtocolStatus> packet_protocol_statuses;
    std::vector<uint32_t> packet_label_ids;
    std::vector<npm::NpmSessionEndReason> end_reasons;
    std::vector<int64_t> end_observed_ats;
    std::vector<npm::NpmProtocolStatus> end_protocol_statuses;
    std::vector<std::optional<uint16_t>> end_protocol_ids;
    std::vector<std::optional<uint16_t>> end_protocol_sub_ids;
    std::vector<uint32_t> end_label_ids;

 private:
    uint64_t marker_ = 0;
    std::vector<uint64_t>* events_ = nullptr;
};

void TestProcessNpmPacketSamplingAndCallbackOrder() {
    auto config = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline);
    const npm::NpmObservationDomainMap domain_map{"capture-a", {{1, 77}}};
    const auto first_packet = MakeIpv4TcpPacket("192.0.2.1", 50000, "192.0.2.2", 443, {1}, kTcpAck, 100, 90, 2048);
    const auto reuse_packet = MakeIpv4TcpPacket("192.0.2.1", 50000, "192.0.2.2", 443, {2}, kTcpSyn, 200);
    const auto close_packet = MakeIpv4TcpPacket("192.0.2.2", 443, "192.0.2.1", 50000, {}, kTcpRst | kTcpAck, 300);

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
    auto status = npm::ProcessNpmPacket(domain_map, first_view, first_packet.layer, table, identifier, modules, writer,
                                        &ended_sessions);
    assert(status.error == npm::NpmPacketProcessError::kNone);
    assert(ended_sessions.empty() && table.size() == 1);
    assert((events == std::vector<uint64_t>{2011, 2012}));
    assert(identifier.calls == 1 && identifier.last_layer == &first_packet.layer);
    assert(first_module.packet_matches_expectation && second_module.packet_matches_expectation);
    assert(first_module.packet_protocol_statuses.back() == npm::NpmProtocolStatus::kIdentified);
    assert(first_module.transport_facts.size() == 1 && second_module.transport_facts.size() == 1);
    assert(first_module.transport_facts[0].payload_wire_bytes == 1);
    assert(first_module.transport_facts[0].payload_captured_bytes == 1);
    assert(first_module.transport_facts[0].payload_complete);
    assert(first_module.transport_facts[0].tcp.valid);
    assert(first_module.transport_facts[0].tcp.sequence == 100);
    assert(first_module.transport_facts[0].tcp.acknowledgment == 90);
    assert(first_module.transport_facts[0].tcp.window == 2048);

    events.clear();
    first_module.expected_packet_data = reuse_packet.bytes.data();
    first_module.expected_layer = &reuse_packet.layer;
    first_module.expected_payload_first = 2;
    second_module.expected_packet_data = reuse_packet.bytes.data();
    second_module.expected_layer = &reuse_packet.layer;
    second_module.expected_payload_first = 2;
    status = npm::ProcessNpmPacket(domain_map, reuse_view, reuse_packet.layer, table, identifier, modules, writer,
                                   &ended_sessions);
    assert(status.error == npm::NpmPacketProcessError::kNone);
    assert(ended_sessions.size() == 1);
    assert(ended_sessions[0].session_id == 1);
    assert(ended_sessions[0].end_reason == npm::NpmSessionEndReason::kTupleReuse);
    assert(ended_sessions[0].protocol_status == npm::NpmProtocolStatus::kIdentified);
    assert((events == std::vector<uint64_t>{1011, 1012, 2021, 2022}));
    assert(first_module.end_observed_ats.back() == 20);
    assert(second_module.end_observed_ats.back() == 20);
    assert(identifier.calls == 2 && identifier.last_layer == &reuse_packet.layer);
    assert(first_module.packet_matches_expectation && second_module.packet_matches_expectation);
    assert(first_module.packet_protocol_statuses.back() == npm::NpmProtocolStatus::kPending);
    assert(table.size() == 1);

    events.clear();
    first_module.expected_packet_data = close_packet.bytes.data();
    first_module.expected_layer = &close_packet.layer;
    first_module.expected_payload_size = 0;
    first_module.expected_direction = npm::NpmPacketDirection::kBToA;
    second_module.expected_packet_data = close_packet.bytes.data();
    second_module.expected_layer = &close_packet.layer;
    second_module.expected_payload_size = 0;
    second_module.expected_direction = npm::NpmPacketDirection::kBToA;
    status = npm::ProcessNpmPacket(domain_map, close_view, close_packet.layer, table, identifier, modules, writer,
                                   &ended_sessions);
    assert(status.error == npm::NpmPacketProcessError::kNone);
    assert(ended_sessions.size() == 1);
    assert(ended_sessions[0].session_id == 2);
    assert(ended_sessions[0].end_reason == npm::NpmSessionEndReason::kClosed);
    assert((events == std::vector<uint64_t>{2021, 2022, 1021, 1022}));
    assert(first_module.packet_matches_expectation && second_module.packet_matches_expectation);
    assert(first_module.packet_sequences.back() == 0 && second_module.packet_sequences.back() == 0);
    assert(first_module.packet_timestamps.back() == 30 && second_module.packet_timestamps.back() == 30);
    assert(first_module.transport_facts.back().tcp.rst && second_module.transport_facts.back().tcp.rst);
    assert(first_module.end_observed_ats.back() == 30);
    assert(second_module.end_observed_ats.back() == 30);
    assert(identifier.calls == 2 && table.size() == 0);

    const auto old_packet = MakeIpv4TcpPacket("203.0.113.1", 52000, "203.0.113.2", 443, {}, kTcpAck, 10);
    const auto reuse_and_close_packet =
        MakeIpv4TcpPacket("203.0.113.1", 52000, "203.0.113.2", 443, {}, kTcpSyn | kTcpRst, 20);
    auto old_view = old_packet.View(1, 80);
    old_view.meta.timestamp_ns = 40;
    auto reuse_and_close_view = reuse_and_close_packet.View(1, 80);
    reuse_and_close_view.meta.timestamp_ns = 50;
    npm::NpmSessionTable reuse_and_close_table(config);
    events.clear();
    assert(npm::ProcessNpmPacket(domain_map, old_view, old_packet.layer, reuse_and_close_table, identifier, modules,
                                 writer, &ended_sessions)
               .error == npm::NpmPacketProcessError::kNone);
    events.clear();
    status = npm::ProcessNpmPacket(domain_map, reuse_and_close_view, reuse_and_close_packet.layer,
                                   reuse_and_close_table, identifier, modules, writer, &ended_sessions);
    assert(status.error == npm::NpmPacketProcessError::kNone);
    assert(ended_sessions.size() == 2);
    assert(ended_sessions[0].session_id == 1);
    assert(ended_sessions[0].end_reason == npm::NpmSessionEndReason::kTupleReuse);
    assert(ended_sessions[1].session_id == 2);
    assert(ended_sessions[1].end_reason == npm::NpmSessionEndReason::kClosed);
    assert((events == std::vector<uint64_t>{1011, 1012, 2021, 2022, 1021, 1022}));
    assert(reuse_and_close_table.size() == 0);
}

void TestProcessNpmPacketSamplesTerminalPayloadBeforeFinalization() {
    const npm::NpmObservationDomainMap domain_map{"capture-a", {{1, 77}}};
    FixtureWriter writer;

    {
        auto config = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline);
        const auto packet = MakeIpv4TcpPacket("192.0.2.1", 50000, "192.0.2.2", 443, {0x31}, kTcpRst | kTcpAck, 100);
        auto view = packet.View(1, 100);
        view.meta.timestamp_ns = 10;
        npm::NpmSessionTable table(config);
        SequenceProtocolIdentifier identifier({{flowsql::packet::ProtocolStatus::kIdentified, 7, 8}});
        std::vector<uint64_t> events;
        PacketProcessorRecordingModule module(1, &events);
        const std::vector<npm::INpmAnalysisModule*> modules{&module};
        std::vector<npm::NpmSessionSnapshot> ended_sessions;

        const auto status =
            npm::ProcessNpmPacket(domain_map, view, packet.layer, table, identifier, modules, writer, &ended_sessions);
        assert(status.error == npm::NpmPacketProcessError::kNone);
        assert(identifier.calls == 1 && identifier.last_packet_data == packet.bytes.data());
        assert(ended_sessions.size() == 1 && table.size() == 0);
        assert(ended_sessions[0].protocol_status == npm::NpmProtocolStatus::kIdentified);
        assert(ended_sessions[0].protocol_id == 7 && ended_sessions[0].protocol_sub_id == 8);
        assert((events == std::vector<uint64_t>{2011, 1011}));
        assert(module.packet_protocol_statuses.back() == npm::NpmProtocolStatus::kIdentified);
        assert(module.end_protocol_statuses.back() == npm::NpmProtocolStatus::kIdentified);
        assert(module.end_protocol_ids.back() == 7 && module.end_protocol_sub_ids.back() == 8);
    }

    {
        auto config = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline);
        config.payload_sample_packets = 2;
        const auto first =
            MakeIpv4TcpPacket("198.51.100.1", 51000, "198.51.100.2", 8443, {0x40}, kTcpFin | kTcpAck, 200);
        const auto close =
            MakeIpv4TcpPacket("198.51.100.2", 8443, "198.51.100.1", 51000, {0x41}, kTcpFin | kTcpAck, 300);
        auto first_view = first.View(1, 100);
        first_view.meta.timestamp_ns = 20;
        auto close_view = close.View(1, 110);
        close_view.meta.timestamp_ns = 30;
        npm::NpmSessionTable table(config);
        SequenceProtocolIdentifier identifier({
            {flowsql::packet::ProtocolStatus::kUnknown, 0, 0},
            {flowsql::packet::ProtocolStatus::kIdentified, 7, 8},
        });
        std::vector<uint64_t> events;
        PacketProcessorRecordingModule module(1, &events);
        const std::vector<npm::INpmAnalysisModule*> modules{&module};
        std::vector<npm::NpmSessionSnapshot> ended_sessions;

        assert(npm::ProcessNpmPacket(domain_map, first_view, first.layer, table, identifier, modules, writer,
                                     &ended_sessions)
                   .error == npm::NpmPacketProcessError::kNone);
        assert(identifier.calls == 1 && ended_sessions.empty() && table.size() == 1);
        assert(module.packet_protocol_statuses.back() == npm::NpmProtocolStatus::kPending);
        events.clear();
        const auto status = npm::ProcessNpmPacket(domain_map, close_view, close.layer, table, identifier, modules,
                                                  writer, &ended_sessions);
        assert(status.error == npm::NpmPacketProcessError::kNone);
        assert(identifier.calls == 2 && ended_sessions.size() == 1 && table.size() == 0);
        assert(ended_sessions[0].protocol_status == npm::NpmProtocolStatus::kIdentified);
        assert(ended_sessions[0].protocol_id == 7 && ended_sessions[0].protocol_sub_id == 8);
        assert((events == std::vector<uint64_t>{2011, 1011}));
        assert(module.packet_protocol_statuses.back() == npm::NpmProtocolStatus::kIdentified);
        assert(module.end_protocol_statuses.back() == npm::NpmProtocolStatus::kIdentified);
    }

    {
        auto config = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline);
        const auto first = MakeIpv4TcpPacket("203.0.113.10", 52100, "203.0.113.20", 9443, {0x48}, kTcpAck, 350);
        const auto close =
            MakeIpv4TcpPacket("203.0.113.20", 9443, "203.0.113.10", 52100, {0x49}, kTcpRst | kTcpAck, 360);
        auto first_view = first.View(1, 100);
        first_view.meta.timestamp_ns = 35;
        auto close_view = close.View(1, 110);
        close_view.meta.timestamp_ns = 36;
        npm::NpmSessionTable table(config);
        SequenceProtocolIdentifier identifier({
            {flowsql::packet::ProtocolStatus::kIdentified, 7, 8},
            {flowsql::packet::ProtocolStatus::kIdentified, 9, 10},
        });
        std::vector<uint64_t> events;
        PacketProcessorRecordingModule module(1, &events);
        const std::vector<npm::INpmAnalysisModule*> modules{&module};
        std::vector<npm::NpmSessionSnapshot> ended_sessions;

        assert(npm::ProcessNpmPacket(domain_map, first_view, first.layer, table, identifier, modules, writer,
                                     &ended_sessions)
                   .error == npm::NpmPacketProcessError::kNone);
        assert(identifier.calls == 1);
        events.clear();
        const auto status = npm::ProcessNpmPacket(domain_map, close_view, close.layer, table, identifier, modules,
                                                  writer, &ended_sessions);
        assert(status.error == npm::NpmPacketProcessError::kNone);
        assert(identifier.calls == 1 && ended_sessions.size() == 1 && table.size() == 0);
        assert(ended_sessions[0].protocol_status == npm::NpmProtocolStatus::kIdentified);
        assert(ended_sessions[0].protocol_id == 7 && ended_sessions[0].protocol_sub_id == 8);
        assert(module.packet_protocol_statuses.back() == npm::NpmProtocolStatus::kIdentified);
        assert(module.end_protocol_statuses.back() == npm::NpmProtocolStatus::kIdentified);
    }

    {
        auto config = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline);
        config.payload_sample_packets = 1;
        const auto first = MakeIpv4TcpPacket("203.0.113.1", 52000, "203.0.113.2", 9443, {0x50}, kTcpAck, 400);
        const auto close = MakeIpv4TcpPacket("203.0.113.2", 9443, "203.0.113.1", 52000, {0x51}, kTcpRst | kTcpAck, 500);
        auto first_view = first.View(1, 100);
        first_view.meta.timestamp_ns = 40;
        auto close_view = close.View(1, 110);
        close_view.meta.timestamp_ns = 50;
        npm::NpmSessionTable table(config);
        SequenceProtocolIdentifier identifier({
            {flowsql::packet::ProtocolStatus::kUnknown, 0, 0},
            {flowsql::packet::ProtocolStatus::kIdentified, 7, 8},
        });
        std::vector<uint64_t> events;
        PacketProcessorRecordingModule module(1, &events);
        const std::vector<npm::INpmAnalysisModule*> modules{&module};
        std::vector<npm::NpmSessionSnapshot> ended_sessions;

        assert(npm::ProcessNpmPacket(domain_map, first_view, first.layer, table, identifier, modules, writer,
                                     &ended_sessions)
                   .error == npm::NpmPacketProcessError::kNone);
        assert(identifier.calls == 1);
        assert(module.packet_protocol_statuses.back() == npm::NpmProtocolStatus::kUnknown);
        events.clear();
        const auto status = npm::ProcessNpmPacket(domain_map, close_view, close.layer, table, identifier, modules,
                                                  writer, &ended_sessions);
        assert(status.error == npm::NpmPacketProcessError::kNone);
        assert(identifier.calls == 1 && ended_sessions.size() == 1 && table.size() == 0);
        assert(ended_sessions[0].protocol_status == npm::NpmProtocolStatus::kUnknown);
        assert(!ended_sessions[0].protocol_id && !ended_sessions[0].protocol_sub_id);
        assert(module.packet_protocol_statuses.back() == npm::NpmProtocolStatus::kUnknown);
        assert(module.end_protocol_statuses.back() == npm::NpmProtocolStatus::kUnknown);
    }

    {
        auto config = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline);
        const auto old_packet = MakeIpv4TcpPacket("10.0.0.1", 53000, "10.0.0.2", 443, {}, kTcpAck, 600);
        const auto replacement = MakeIpv4TcpPacket("10.0.0.1", 53000, "10.0.0.2", 443, {0x61}, kTcpSyn | kTcpRst, 700);
        auto old_view = old_packet.View(1, 80);
        old_view.meta.timestamp_ns = 60;
        auto replacement_view = replacement.View(1, 100);
        replacement_view.meta.timestamp_ns = 70;
        npm::NpmSessionTable table(config);
        SequenceProtocolIdentifier identifier({{flowsql::packet::ProtocolStatus::kIdentified, 7, 8}});
        std::vector<uint64_t> events;
        PacketProcessorRecordingModule module(1, &events);
        const std::vector<npm::INpmAnalysisModule*> modules{&module};
        std::vector<npm::NpmSessionSnapshot> ended_sessions;

        assert(npm::ProcessNpmPacket(domain_map, old_view, old_packet.layer, table, identifier, modules, writer,
                                     &ended_sessions)
                   .error == npm::NpmPacketProcessError::kNone);
        assert(identifier.calls == 0 && table.size() == 1);
        events.clear();
        const auto status = npm::ProcessNpmPacket(domain_map, replacement_view, replacement.layer, table, identifier,
                                                  modules, writer, &ended_sessions);
        assert(status.error == npm::NpmPacketProcessError::kNone);
        assert(identifier.calls == 1 && ended_sessions.size() == 2 && table.size() == 0);
        assert(ended_sessions[0].session_id == 1);
        assert(ended_sessions[0].end_reason == npm::NpmSessionEndReason::kTupleReuse);
        assert(ended_sessions[0].protocol_status == npm::NpmProtocolStatus::kUnknown);
        assert(ended_sessions[1].session_id == 2);
        assert(ended_sessions[1].end_reason == npm::NpmSessionEndReason::kClosed);
        assert(ended_sessions[1].protocol_status == npm::NpmProtocolStatus::kIdentified);
        assert(ended_sessions[1].protocol_id == 7 && ended_sessions[1].protocol_sub_id == 8);
        assert((events == std::vector<uint64_t>{1011, 2021, 1021}));
        assert(module.end_protocol_statuses[module.end_protocol_statuses.size() - 2] ==
               npm::NpmProtocolStatus::kUnknown);
        assert(module.packet_protocol_statuses.back() == npm::NpmProtocolStatus::kIdentified);
        assert(module.end_protocol_statuses.back() == npm::NpmProtocolStatus::kIdentified);
    }

    {
        auto config = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline);
        const auto packet = MakeIpv4TcpPacket("172.16.0.1", 54000, "172.16.0.2", 443, {0x71}, kTcpRst | kTcpAck, 800);
        auto view = packet.View(1, 100);
        view.meta.timestamp_ns = 80;
        npm::NpmSessionTable table(config);
        SequenceProtocolIdentifier identifier({{flowsql::packet::ProtocolStatus::kUnknown, 0, 0}});
        std::vector<uint64_t> events;
        PacketProcessorRecordingModule module(1, &events);
        const std::vector<npm::INpmAnalysisModule*> modules{&module};
        std::vector<npm::NpmSessionSnapshot> ended_sessions;

        const auto status =
            npm::ProcessNpmPacket(domain_map, view, packet.layer, table, identifier, modules, writer, &ended_sessions);
        assert(status.error == npm::NpmPacketProcessError::kNone);
        assert(identifier.calls == 1 && ended_sessions.size() == 1);
        assert(ended_sessions[0].protocol_status == npm::NpmProtocolStatus::kUnknown);
        assert(!ended_sessions[0].protocol_id && !ended_sessions[0].protocol_sub_id);
        assert(module.packet_protocol_statuses.back() == npm::NpmProtocolStatus::kUnknown);
        assert(module.end_protocol_statuses.back() == npm::NpmProtocolStatus::kUnknown);
    }
}

void TestProcessNpmPacketErrorsAreStructuredAndAtomic() {
    auto config = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline);
    config.out_of_order_tolerance_ns = 0;
    const npm::NpmObservationDomainMap domain_map{"capture-a", {{1, 77}}};
    const auto packet = MakeIpv4TcpPacket("192.0.2.1", 50000, "192.0.2.2", 443, {1}, kTcpAck, 100);
    auto view = packet.View(1, 100);
    view.meta.timestamp_ns = 10;
    SequenceProtocolIdentifier identifier({});
    FixtureWriter writer;
    std::vector<uint64_t> events;
    PacketProcessorRecordingModule first_module(1, &events);
    PacketProcessorRecordingModule second_module(2, &events);
    std::vector<npm::INpmAnalysisModule*> modules{&first_module, &second_module};
    npm::NpmSessionTable table(config);

    auto status = npm::ProcessNpmPacket(domain_map, view, packet.layer, table, identifier, modules, writer, nullptr);
    assert(status.error == npm::NpmPacketProcessError::kNullOutput);
    assert(table.size() == 0 && identifier.calls == 0 && events.empty());

    std::vector<npm::NpmSessionSnapshot> ended_sessions(1);
    ended_sessions[0].session_id = 999;
    modules[1] = nullptr;
    status = npm::ProcessNpmPacket(domain_map, view, packet.layer, table, identifier, modules, writer, &ended_sessions);
    assert(status.error == npm::NpmPacketProcessError::kNullModule);
    assert(table.size() == 0 && identifier.calls == 0 && events.empty());
    assert(ended_sessions.size() == 1 && ended_sessions[0].session_id == 999);

    modules[1] = &second_module;
    auto unknown_source = view;
    unknown_source.meta.source_id = 2;
    status = npm::ProcessNpmPacket(domain_map, unknown_source, packet.layer, table, identifier, modules, writer,
                                   &ended_sessions);
    assert(status.error == npm::NpmPacketProcessError::kBindingError);
    assert(status.binding_error == npm::NpmSessionPacketError::kUnknownSourceId);
    assert(table.size() == 0 && events.empty());
    assert(ended_sessions.size() == 1 && ended_sessions[0].session_id == 999);

    table.AdvanceCaptureProgress(OfflineProgress(20));
    status = npm::ProcessNpmPacket(domain_map, view, packet.layer, table, identifier, modules, writer, &ended_sessions);
    assert(status.error == npm::NpmPacketProcessError::kSessionError);
    assert(status.session_error == npm::NpmSessionTableError::kLatePacket);
    assert(table.size() == 0 && events.empty());
    assert(ended_sessions.size() == 1 && ended_sessions[0].session_id == 999);

    npm::NpmSessionTable callback_table(config);
    first_module.packet_error = EIO;
    status = npm::ProcessNpmPacket(domain_map, view, packet.layer, callback_table, identifier, modules, writer,
                                   &ended_sessions);
    assert(status.error == npm::NpmPacketProcessError::kModuleError);
    assert(status.module_error == EIO);
    assert((events == std::vector<uint64_t>{2011}));
    assert(callback_table.size() == 1);
    assert(ended_sessions.size() == 1 && ended_sessions[0].session_id == 999);

    events.clear();
    first_module.end_error = EBUSY;
    const auto rst_packet = MakeIpv4TcpPacket("198.51.100.1", 51000, "198.51.100.2", 8443, {}, kTcpRst, 200);
    auto rst_view = rst_packet.View(1, 80);
    rst_view.meta.timestamp_ns = 30;
    npm::NpmSessionTable terminal_packet_error_table(config);
    status = npm::ProcessNpmPacket(domain_map, rst_view, rst_packet.layer, terminal_packet_error_table, identifier,
                                   modules, writer, &ended_sessions);
    assert(status.error == npm::NpmPacketProcessError::kModuleError);
    assert(status.module_error == EIO);
    assert((events == std::vector<uint64_t>{2011}));
    assert(terminal_packet_error_table.size() == 0);
    assert(ended_sessions.size() == 1 && ended_sessions[0].session_id == 999);

    events.clear();
    first_module.packet_error = 0;
    npm::NpmSessionTable end_error_table(config);
    status = npm::ProcessNpmPacket(domain_map, rst_view, rst_packet.layer, end_error_table, identifier, modules, writer,
                                   &ended_sessions);
    assert(status.error == npm::NpmPacketProcessError::kModuleError);
    assert(status.module_error == EBUSY);
    assert((events == std::vector<uint64_t>{2011, 2012, 1011}));
    assert(end_error_table.size() == 0);
    assert(ended_sessions.size() == 1 && ended_sessions[0].session_id == 999);
}

flowsql::packet::PacketRecord MakeBatchPacketRecord(const PacketFixture& fixture, uint32_t source_id,
                                                    int64_t timestamp_ns, uint64_t sequence) {
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

std::shared_ptr<arrow::RecordBatch> MakeEncodedPacketBatch(const std::vector<flowsql::packet::PacketRecord>& records) {
    std::shared_ptr<arrow::RecordBatch> batch;
    std::string error;
    assert(flowsql::packet::EncodePacketBatch(records, &batch, &error) == flowsql::packet::PacketBatchError::kNone);
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

    const auto idle_packet = MakeIpv4TcpPacket("203.0.113.1", 40000, "203.0.113.2", 80, {0x10}, kTcpAck, 100);
    const auto first_packet = MakeIpv4TcpPacket("192.0.2.1", 50000, "192.0.2.2", 443, {0x11}, kTcpAck, 200);
    const auto reuse_packet = MakeIpv4TcpPacket("192.0.2.1", 50000, "192.0.2.2", 443, {0x12}, kTcpSyn, 201);
    const auto close_packet = MakeIpv4TcpPacket("192.0.2.2", 443, "192.0.2.1", 50000, {}, kTcpRst | kTcpAck, 202);
    const auto later_packet = MakeIpv4TcpPacket("198.51.100.1", 41000, "198.51.100.2", 8080, {0x13}, kTcpAck, 300);
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

    const auto status =
        npm::ProcessNpmOfflinePacketBatch(domain_map, *batch, table, identifier, modules, writer, &ended_events);
    assert(status.error == npm::NpmPacketBatchProcessError::kNone && status.row == -1);
    assert(status.progress_disposition == npm::NpmCaptureProgressDisposition::kAdvanced);
    assert((events == std::vector<uint64_t>{2011, 2012, 2021, 2022, 1021, 1022, 2031, 2032, 2031, 2032, 1031, 1032,
                                            1011, 1012, 2041, 2042}));
    assert((first_module.packet_sequences == std::vector<uint64_t>{100, 101, 102, 103, 104}));
    assert((first_module.packet_timestamps ==
            std::vector<int64_t>{second, 11 * second / 10, 12 * second / 10, 13 * second / 10, 3 * second}));
    assert((first_module.end_observed_ats == std::vector<int64_t>{12 * second / 10, 13 * second / 10, 3 * second}));
    assert(first_module.end_observed_ats == second_module.end_observed_ats);
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

    auto status =
        npm::ProcessNpmOfflinePacketBatch(domain_map, *empty_batch, table, identifier, modules, writer, nullptr);
    assert(status.error == npm::NpmPacketBatchProcessError::kNullOutput && status.row == -1);
    assert(table.size() == 0 && events.empty());

    std::vector<npm::NpmSessionEndEvent> ended_events(1);
    ended_events[0].snapshot.session_id = 999;
    modules[1] = nullptr;
    status =
        npm::ProcessNpmOfflinePacketBatch(domain_map, *empty_batch, table, identifier, modules, writer, &ended_events);
    assert(status.error == npm::NpmPacketBatchProcessError::kNullModule && status.row == -1);
    assert(ended_events.size() == 1 && ended_events[0].snapshot.session_id == 999);
    modules[1] = &second_module;

    status =
        npm::ProcessNpmOfflinePacketBatch(domain_map, *empty_batch, table, identifier, modules, writer, &ended_events);
    assert(status.error == npm::NpmPacketBatchProcessError::kNone && ended_events.empty());

    const auto packet = MakeIpv4TcpPacket("192.0.2.1", 50000, "192.0.2.2", 443, {0x20}, kTcpAck, 400);
    auto binding_error_batch = MakeValidatedPacketBatchView({
        MakeBatchPacketRecord(packet, 1, 10, 200),
        MakeBatchPacketRecord(packet, 2, 20, 201),
        MakeBatchPacketRecord(packet, 1, 30, 202),
    });
    ended_events.emplace_back().snapshot.session_id = 998;
    status = npm::ProcessNpmOfflinePacketBatch(domain_map, *binding_error_batch, table, identifier, modules, writer,
                                               &ended_events);
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
    status = npm::ProcessNpmOfflinePacketBatch(domain_map, *realtime_batch, realtime_table, identifier, modules, writer,
                                               &ended_events);
    assert(status.error == npm::NpmPacketBatchProcessError::kProgressDeferred && status.row == 0);
    assert(status.progress_disposition == npm::NpmCaptureProgressDisposition::kDeferredBacklogUnknown);
    assert(ended_events.size() == 1 && ended_events[0].snapshot.session_id == 998);
    assert(events.empty() && realtime_table.size() == 0);

    npm::NpmSessionTable live_table(realtime_config);
    std::vector<npm::NpmSessionEndEvent> live_ended;
    status = npm::ProcessNpmOfflinePacketBatch(domain_map, *realtime_batch, live_table, identifier, {}, writer,
                                               &live_ended, nullptr, {}, nullptr, false);
    assert(status.error == npm::NpmPacketBatchProcessError::kNone);
    assert(status.progress_disposition == npm::NpmCaptureProgressDisposition::kUnchanged);
    assert(live_table.size() == 1 && live_ended.empty());
    npm::NpmCaptureProgressUpdate released_fact;
    released_fact.capture_time_ns = 40;
    released_fact.packet_observed = true;
    released_fact.source_backlog_known = true;
    assert(live_table.AdvanceCaptureProgress(released_fact).disposition ==
           npm::NpmCaptureProgressDisposition::kAdvanced);

    constexpr int64_t second = npm::kNpmNanosecondsPerSecond;
    auto idle_config = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline);
    idle_config.tcp_idle_timeout_ns = second;
    idle_config.out_of_order_tolerance_ns = 0;
    const auto later_packet = MakeIpv4TcpPacket("198.51.100.1", 41000, "198.51.100.2", 8080, {0x21}, kTcpAck, 500);
    auto idle_error_batch = MakeValidatedPacketBatchView({
        MakeBatchPacketRecord(packet, 1, second, 400),
        MakeBatchPacketRecord(later_packet, 1, 3 * second, 401),
    });
    npm::NpmSessionTable idle_error_table(idle_config);
    events.clear();
    first_module.end_error = EBUSY;
    status = npm::ProcessNpmOfflinePacketBatch(domain_map, *idle_error_batch, idle_error_table, identifier, modules,
                                               writer, &ended_events);
    assert(status.error == npm::NpmPacketBatchProcessError::kModuleError && status.row == 1);
    assert(status.module_error == EBUSY);
    assert(ended_events.size() == 1 && ended_events[0].snapshot.session_id == 998);
    // The failed timeout callback prevents delivery of the current packet.
    assert((events == std::vector<uint64_t>{2011, 2012, 1011}));
    assert(idle_error_table.size() == 0);
}

void TestFlowLabelingAdmissionBatchSessionReuseAndFailureAtomicity() {
    auto config = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline);
    config.out_of_order_tolerance_ns = 0;
    config.payload_sample_packets = 2;
    const npm::NpmObservationDomainMap domain_map{"capture-a", {{1, 77}}};
    const auto first = MakeIpv4TcpPacket("192.0.2.1", 50000, "198.51.100.2", 443, {0x10}, kTcpAck, 100);
    const auto first_reverse = MakeIpv4TcpPacket("198.51.100.2", 443, "192.0.2.1", 50000, {0x11}, kTcpAck, 200);
    const auto unmatched = MakeIpv4TcpPacket("203.0.113.1", 51000, "203.0.113.2", 53, {0x12}, kTcpAck, 300);

    auto initial_batch = MakeValidatedPacketBatchView({
        MakeBatchPacketRecord(first, 1, 10, 1),
        MakeBatchPacketRecord(first_reverse, 1, 20, 2),
        MakeBatchPacketRecord(unmatched, 1, 30, 3),
        MakeBatchPacketRecord(unmatched, 1, 35, 4),
    });
    npm::NpmSessionTable table(config);
    SequenceProtocolIdentifier identifier({
        {flowsql::packet::ProtocolStatus::kUnknown, 0, 0},
        {flowsql::packet::ProtocolStatus::kIdentified, 7, 8},
        {flowsql::packet::ProtocolStatus::kUnknown, 0, 0},
        {flowsql::packet::ProtocolStatus::kUnknown, 0, 0},
    });
    std::vector<uint64_t> events;
    PacketProcessorRecordingModule module(1, &events);
    const std::vector<npm::INpmAnalysisModule*> modules{&module};
    FixtureWriter writer;
    std::vector<npm::NpmSessionEndEvent> ended_events;
    LabelingMatcherStats stats;
    stats.labels_by_call = {{1001, 0}, {2002}};
    auto* matcher = new RecordingLabelMatcher(&stats);

    auto status = npm::ProcessNpmOfflinePacketBatch(domain_map, *initial_batch, table, identifier, modules, writer,
                                                    &ended_events, matcher);
    assert(status.error == npm::NpmPacketBatchProcessError::kNone);
    assert(stats.classify_calls == 1 && (stats.batch_counts == std::vector<uint32_t>{2}));
    assert(stats.facts.size() == 2);
    assert(stats.facts[0].observation_domain_id == 77 && stats.facts[0].ip_family == 4);
    assert(stats.facts[0].transport_valid == 1 && stats.facts[0].transport_protocol == flowsql::ipv4::eNext::TCP);
    assert(stats.facts[0].source.port_valid == 1 && stats.facts[0].source.port == 50000);
    assert(stats.facts[0].destination.port == 443 && stats.facts[1].destination.port == 53);
    assert(std::memcmp(stats.facts[0].source.ip, ParseIpv4("192.0.2.1").bytes, 4) == 0);
    assert(table.size() == 2 && ended_events.empty());
    assert((module.packet_label_ids == std::vector<uint32_t>{1001, 1001, 0, 0}));
    assert((module.packet_protocol_statuses ==
            std::vector<npm::NpmProtocolStatus>{npm::NpmProtocolStatus::kPending, npm::NpmProtocolStatus::kIdentified,
                                                npm::NpmProtocolStatus::kPending, npm::NpmProtocolStatus::kUnknown}));

    flowsql::packet::PacketMeta first_meta;
    flowsql::packet::PacketMeta unmatched_meta;
    auto first_binding = BuildBinding(domain_map, first, 1, 10, first.bytes.size(), &first_meta);
    auto unmatched_binding = BuildBinding(domain_map, unmatched, 1, 30, unmatched.bytes.size(), &unmatched_meta);
    npm::NpmSessionView first_view;
    npm::NpmSessionView unmatched_view;
    assert(table.Find(first_binding.key, &first_view) == npm::NpmSessionTableError::kNone);
    assert(table.Find(unmatched_binding.key, &unmatched_view) == npm::NpmSessionTableError::kNone);
    assert(first_view.primary_label_id == 1001 && first_view.protocol_status == npm::NpmProtocolStatus::kIdentified);
    assert(unmatched_view.primary_label_id == 0 && unmatched_view.protocol_status == npm::NpmProtocolStatus::kUnknown);

    auto hits = MakeValidatedPacketBatchView({
        MakeBatchPacketRecord(first, 1, 40, 4),
        MakeBatchPacketRecord(unmatched, 1, 50, 5),
    });
    status = npm::ProcessNpmOfflinePacketBatch(domain_map, *hits, table, identifier, modules, writer, &ended_events,
                                               matcher);
    assert(status.error == npm::NpmPacketBatchProcessError::kNone);
    assert(stats.classify_calls == 1);
    assert(module.packet_label_ids[module.packet_label_ids.size() - 2] == 1001);
    assert(module.packet_label_ids.back() == 0);

    const auto reuse = MakeIpv4TcpPacket("192.0.2.1", 50000, "198.51.100.2", 443, {0x13}, kTcpSyn, 900);
    auto reuse_batch = MakeValidatedPacketBatchView({MakeBatchPacketRecord(reuse, 1, 60, 6)});
    status = npm::ProcessNpmOfflinePacketBatch(domain_map, *reuse_batch, table, identifier, modules, writer,
                                               &ended_events, matcher);
    assert(status.error == npm::NpmPacketBatchProcessError::kNone);
    assert(stats.classify_calls == 2 && (stats.batch_counts == std::vector<uint32_t>{2, 1}));
    assert(ended_events.size() == 1);
    assert(ended_events[0].snapshot.end_reason == npm::NpmSessionEndReason::kTupleReuse);
    assert(ended_events[0].snapshot.primary_label_id == 1001);
    assert(table.Find(first_binding.key, &first_view) == npm::NpmSessionTableError::kNone);
    assert(first_view.primary_label_id == 2002 && first_view.session_id != ended_events[0].snapshot.session_id);
    assert(module.end_label_ids.back() == 1001 && module.packet_label_ids.back() == 2002);
    matcher->Release();
    assert(stats.release_calls == 1);

    npm::NpmSessionTable failed_table(config);
    SequenceProtocolIdentifier failed_identifier({});
    std::vector<uint64_t> failed_events;
    PacketProcessorRecordingModule failed_module(1, &failed_events);
    const std::vector<npm::INpmAnalysisModule*> failed_modules{&failed_module};
    std::vector<npm::NpmSessionEndEvent> unchanged_events(1);
    unchanged_events[0].snapshot.session_id = 999;
    LabelingMatcherStats failed_stats;
    failed_stats.classify_error = EIO;
    auto* failed_matcher = new RecordingLabelMatcher(&failed_stats);
    status = npm::ProcessNpmOfflinePacketBatch(domain_map, *initial_batch, failed_table, failed_identifier,
                                               failed_modules, writer, &unchanged_events, failed_matcher);
    assert(status.error == npm::NpmPacketBatchProcessError::kLabelingError);
    assert(status.labeling_error == EIO && failed_stats.classify_calls == 1);
    assert(failed_table.size() == 0 && failed_identifier.calls == 0 && failed_events.empty());
    assert(unchanged_events.size() == 1 && unchanged_events[0].snapshot.session_id == 999);
    failed_matcher->Release();
    assert(failed_stats.release_calls == 1);

    std::vector<flowsql::packet::PacketRecord> bounded_records;
    bounded_records.reserve(257);
    for (uint32_t index = 0; index < 257; ++index) {
        const auto candidate =
            MakeIpv4TcpPacket("10.0.0.1", static_cast<uint16_t>(10'000 + index), "10.0.0.2", 443, {}, kTcpAck, index);
        bounded_records.push_back(MakeBatchPacketRecord(candidate, 1, 1000 + index, 1000 + index));
    }
    auto bounded_batch = MakeValidatedPacketBatchView(bounded_records);
    npm::NpmSessionTable bounded_table(config);
    SequenceProtocolIdentifier bounded_identifier({});
    std::vector<uint64_t> bounded_events;
    PacketProcessorRecordingModule bounded_module(1, &bounded_events);
    const std::vector<npm::INpmAnalysisModule*> bounded_modules{&bounded_module};
    std::vector<npm::NpmSessionEndEvent> bounded_ended_events;
    LabelingMatcherStats bounded_stats;
    bounded_stats.classify_error = EIO;
    bounded_stats.fail_call = 1;
    auto* bounded_matcher = new RecordingLabelMatcher(&bounded_stats);
    status = npm::ProcessNpmOfflinePacketBatch(domain_map, *bounded_batch, bounded_table, bounded_identifier,
                                               bounded_modules, writer, &bounded_ended_events, bounded_matcher);
    assert(status.error == npm::NpmPacketBatchProcessError::kLabelingError && status.labeling_error == EIO);
    assert(bounded_stats.classify_calls == 2 && (bounded_stats.batch_counts == std::vector<uint32_t>{256, 1}));
    assert(bounded_table.size() == 0 && bounded_identifier.calls == 0 && bounded_events.empty() &&
           bounded_ended_events.empty());
    bounded_matcher->Release();
    assert(bounded_stats.release_calls == 1);

    std::vector<flowsql::packet::PacketRecord> boundary_hit_records;
    boundary_hit_records.reserve(257);
    for (uint32_t index = 0; index < 256; ++index) {
        const auto candidate =
            MakeIpv4TcpPacket("10.1.0.1", static_cast<uint16_t>(20'000 + index), "10.1.0.2", 443, {}, kTcpAck, index);
        boundary_hit_records.push_back(MakeBatchPacketRecord(candidate, 1, 2000 + index, 2000 + index));
    }
    const auto first_candidate_hit = MakeIpv4TcpPacket("10.1.0.1", 20'000, "10.1.0.2", 443, {}, kTcpAck, 999);
    boundary_hit_records.push_back(MakeBatchPacketRecord(first_candidate_hit, 1, 2256, 2256));
    auto boundary_hit_batch = MakeValidatedPacketBatchView(boundary_hit_records);
    npm::NpmSessionTable boundary_hit_table(config);
    SequenceProtocolIdentifier boundary_hit_identifier({});
    std::vector<uint64_t> boundary_hit_events;
    PacketProcessorRecordingModule boundary_hit_module(1, &boundary_hit_events);
    const std::vector<npm::INpmAnalysisModule*> boundary_hit_modules{&boundary_hit_module};
    std::vector<npm::NpmSessionEndEvent> boundary_hit_ended_events;
    LabelingMatcherStats boundary_hit_stats;
    auto* boundary_hit_matcher = new RecordingLabelMatcher(&boundary_hit_stats);
    status = npm::ProcessNpmOfflinePacketBatch(domain_map, *boundary_hit_batch, boundary_hit_table,
                                               boundary_hit_identifier, boundary_hit_modules, writer,
                                               &boundary_hit_ended_events, boundary_hit_matcher);
    assert(status.error == npm::NpmPacketBatchProcessError::kNone);
    assert(boundary_hit_stats.classify_calls == 1 && (boundary_hit_stats.batch_counts == std::vector<uint32_t>{256}));
    assert(boundary_hit_module.packet_label_ids.size() == 257);
    assert(boundary_hit_module.packet_label_ids.front() == 1001 && boundary_hit_module.packet_label_ids.back() == 1001);
    boundary_hit_matcher->Release();
    assert(boundary_hit_stats.release_calls == 1);
}

void TestFlowLabelingAdmissionTracksIntraBlockSessionLifecycles() {
    constexpr int64_t second = npm::kNpmNanosecondsPerSecond;
    auto config = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline);
    config.tcp_idle_timeout_ns = second;
    config.out_of_order_tolerance_ns = 0;
    const npm::NpmObservationDomainMap domain_map{"capture-a", {{1, 77}}};
    SequenceProtocolIdentifier identifier({});
    FixtureWriter writer;

    const auto first = MakeIpv4TcpPacket("192.0.2.1", 50000, "198.51.100.2", 443, {0x10}, kTcpAck, 100);
    flowsql::packet::PacketMeta first_meta;
    const auto first_binding = BuildBinding(domain_map, first, 1, 1, first.bytes.size(), &first_meta);

    {
        npm::NpmSessionTable table(config);
        std::vector<uint64_t> events;
        PacketProcessorRecordingModule module(1, &events);
        const std::vector<npm::INpmAnalysisModule*> modules{&module};
        std::vector<npm::NpmSessionEndEvent> ended_events;
        LabelingMatcherStats stats;
        stats.labels_by_call = {{1001}, {2002, 3003}};
        auto* matcher = new RecordingLabelMatcher(&stats);

        auto initial_batch = MakeValidatedPacketBatchView({MakeBatchPacketRecord(first, 1, 1, 1)});
        auto status = npm::ProcessNpmOfflinePacketBatch(domain_map, *initial_batch, table, identifier, modules, writer,
                                                        &ended_events, matcher);
        assert(status.error == npm::NpmPacketBatchProcessError::kNone);

        const auto rst = MakeIpv4TcpPacket("192.0.2.1", 50000, "198.51.100.2", 443, {}, kTcpRst, 101);
        const auto replacement = MakeIpv4TcpPacket("192.0.2.1", 50000, "198.51.100.2", 443, {0x11}, kTcpAck, 102);
        const auto fin = MakeIpv4TcpPacket("192.0.2.1", 50000, "198.51.100.2", 443, {}, kTcpFin | kTcpAck, 103);
        const auto reverse_fin = MakeIpv4TcpPacket("198.51.100.2", 443, "192.0.2.1", 50000, {}, kTcpFin | kTcpAck, 104);
        const auto second_replacement =
            MakeIpv4TcpPacket("192.0.2.1", 50000, "198.51.100.2", 443, {0x12}, kTcpAck, 105);
        auto lifecycle_batch = MakeValidatedPacketBatchView({
            MakeBatchPacketRecord(rst, 1, 2, 2),
            MakeBatchPacketRecord(replacement, 1, 3, 3),
            MakeBatchPacketRecord(fin, 1, 4, 4),
            MakeBatchPacketRecord(reverse_fin, 1, 5, 5),
            MakeBatchPacketRecord(second_replacement, 1, 6, 6),
        });
        status = npm::ProcessNpmOfflinePacketBatch(domain_map, *lifecycle_batch, table, identifier, modules, writer,
                                                   &ended_events, matcher);
        assert(status.error == npm::NpmPacketBatchProcessError::kNone);
        assert(stats.classify_calls == 2 && (stats.batch_counts == std::vector<uint32_t>{1, 2}));
        assert(ended_events.size() == 2 && ended_events[0].snapshot.primary_label_id == 1001);
        assert(ended_events[1].snapshot.primary_label_id == 2002);
        npm::NpmSessionView replacement_view;
        assert(table.Find(first_binding.key, &replacement_view) == npm::NpmSessionTableError::kNone);
        assert(replacement_view.primary_label_id == 3003);
        assert((module.packet_label_ids == std::vector<uint32_t>{1001, 1001, 2002, 2002, 2002, 3003}));
        matcher->Release();
    }

    {
        npm::NpmSessionTable table(config);
        std::vector<uint64_t> events;
        PacketProcessorRecordingModule module(1, &events);
        const std::vector<npm::INpmAnalysisModule*> modules{&module};
        std::vector<npm::NpmSessionEndEvent> ended_events;
        LabelingMatcherStats stats;
        stats.labels_by_call = {{1001}, {3003, 2002}};
        auto* matcher = new RecordingLabelMatcher(&stats);

        auto initial_batch = MakeValidatedPacketBatchView({MakeBatchPacketRecord(first, 1, 1, 1)});
        auto status = npm::ProcessNpmOfflinePacketBatch(domain_map, *initial_batch, table, identifier, modules, writer,
                                                        &ended_events, matcher);
        assert(status.error == npm::NpmPacketBatchProcessError::kNone);

        const auto watermark_driver =
            MakeIpv4TcpPacket("203.0.113.1", 51000, "203.0.113.2", 8443, {0x20}, kTcpAck, 200);
        const auto replacement = MakeIpv4TcpPacket("192.0.2.1", 50000, "198.51.100.2", 443, {0x21}, kTcpAck, 201);
        auto lifecycle_batch = MakeValidatedPacketBatchView({
            MakeBatchPacketRecord(watermark_driver, 1, 2 * second, 2),
            MakeBatchPacketRecord(replacement, 1, 2 * second + 1, 3),
        });
        status = npm::ProcessNpmOfflinePacketBatch(domain_map, *lifecycle_batch, table, identifier, modules, writer,
                                                   &ended_events, matcher);
        assert(status.error == npm::NpmPacketBatchProcessError::kNone);
        assert(stats.classify_calls == 2 && (stats.batch_counts == std::vector<uint32_t>{1, 2}));
        assert(ended_events.size() == 1);
        assert(ended_events[0].snapshot.end_reason == npm::NpmSessionEndReason::kIdleTimeout);
        assert(ended_events[0].snapshot.primary_label_id == 1001);
        npm::NpmSessionView replacement_view;
        assert(table.Find(first_binding.key, &replacement_view) == npm::NpmSessionTableError::kNone);
        assert(replacement_view.primary_label_id == 2002);
        matcher->Release();
    }

    {
        npm::NpmSessionTable table(config);
        std::vector<uint64_t> events;
        PacketProcessorRecordingModule module(1, &events);
        const std::vector<npm::INpmAnalysisModule*> modules{&module};
        std::vector<npm::NpmSessionEndEvent> ended_events;
        LabelingMatcherStats stats;
        stats.labels_by_call = {{1001, 2002}};
        auto* matcher = new RecordingLabelMatcher(&stats);

        const auto initial_syn = MakeIpv4TcpPacket("192.0.2.1", 50000, "198.51.100.2", 443, {}, kTcpSyn, 300);
        const auto reuse_syn = MakeIpv4TcpPacket("192.0.2.1", 50000, "198.51.100.2", 443, {}, kTcpSyn, 301);
        auto lifecycle_batch = MakeValidatedPacketBatchView({
            MakeBatchPacketRecord(initial_syn, 1, 10, 1),
            MakeBatchPacketRecord(reuse_syn, 1, 20, 2),
        });
        const auto status = npm::ProcessNpmOfflinePacketBatch(domain_map, *lifecycle_batch, table, identifier, modules,
                                                              writer, &ended_events, matcher);
        assert(status.error == npm::NpmPacketBatchProcessError::kNone);
        assert(stats.classify_calls == 1 && (stats.batch_counts == std::vector<uint32_t>{2}));
        assert(ended_events.size() == 1);
        assert(ended_events[0].snapshot.end_reason == npm::NpmSessionEndReason::kTupleReuse);
        assert(ended_events[0].snapshot.primary_label_id == 1001);
        npm::NpmSessionView replacement_view;
        assert(table.Find(first_binding.key, &replacement_view) == npm::NpmSessionTableError::kNone);
        assert(replacement_view.primary_label_id == 2002);
        assert((module.packet_label_ids == std::vector<uint32_t>{1001, 2002}));
        matcher->Release();
    }
}

void TestNpmProtocolContextErrorsDictionaryAndRaii() {
    static_assert(!std::is_copy_constructible<npm::NpmProtocolContext>::value);
    static_assert(!std::is_copy_assignable<npm::NpmProtocolContext>::value);
    static_assert(!std::is_move_constructible<npm::NpmProtocolContext>::value);
    static_assert(!std::is_move_assignable<npm::NpmProtocolContext>::value);

    std::unique_ptr<npm::NpmProtocolContext> context;
    SinglePoolQuerier missing_querier(nullptr);
    assert(npm::NpmProtocolContext::Create(nullptr, &context) == npm::NpmProtocolContextError::kNullQuerier);
    assert(npm::NpmProtocolContext::Create(&missing_querier, nullptr) == npm::NpmProtocolContextError::kNullOutput);
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

void TestNpmProtocolContextRealNpi(flowsql::IProtocolPipelinePoolV1* real_pool, flowsql::IProtocol* real_protocol) {
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
    assert(table.SampleProtocol(binding.key, observed.active_session.session_id, npm_packet, *contexts[0]->Identifier(),
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
    assert(npm::NpmProtocolContext::Create(&querier, &exhausted) == npm::NpmProtocolContextError::kPipelineExhausted);
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
        "{\"concurrency\":0}",     "{\"concurrency\":-1}",  "{\"concurrency\":17}",
        "{\"concurrency\":\"2\"}", "{\"concurrency\":1.5}",
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

    const std::string option = std::string("{\"ldfile\":\"") + FLOWSQL_NPI_PROTOCOLS_PATH + "\",\"concurrency\":2}";
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
    arrow::FixedSizeListBuilder builder(arrow::default_memory_pool(), value_builder, flowsql::packet::kMaxLayerDepth);
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
    const auto fixture =
        MakeIpv4TcpPacket("192.0.2.1", 41000, "198.51.100.2", 443, std::vector<uint8_t>{0x11, 0x22, 0x33}, kTcpAck, 91);
    auto bytes = std::make_shared<std::vector<uint8_t>>(fixture.bytes);
    const auto ipv6_fixture =
        MakeIpv6UdpPacket("2001:db8::1", 53000, "2001:db8::2", 53, std::vector<uint8_t>{0x44, 0x55});
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

void AssertPacketBatchCreateError(const std::shared_ptr<arrow::RecordBatch>& batch, npm::NpmPacketBatchError expected,
                                  const char* error_fragment, std::unique_ptr<npm::NpmPacketBatchView>* output) {
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
    AssertPacketBatchCreateError(schema_mismatch, npm::NpmPacketBatchError::kSchemaMismatch, "schema", &output);

    auto wrong_type = ReplacePacketBatchColumns(batch, {{0, MakeOneValueArray<arrow::UInt32Builder>(uint32_t{1})}});
    AssertPacketBatchCreateError(wrong_type, npm::NpmPacketBatchError::kInvalidColumn, "timestamp_ns", &output);

    arrow::Int64Builder empty_builder;
    std::shared_ptr<arrow::Array> empty_timestamp;
    assert(empty_builder.Finish(&empty_timestamp).ok());
    auto wrong_length = ReplacePacketBatchColumns(batch, {{0, empty_timestamp}});
    AssertPacketBatchCreateError(wrong_length, npm::NpmPacketBatchError::kInvalidColumn, "timestamp_ns", &output);

    auto required_null = ReplacePacketBatchColumns(batch, {{0, MakeOneNullArray<arrow::Int64Builder>()}});
    AssertPacketBatchCreateError(required_null, npm::NpmPacketBatchError::kInvalidColumn, "timestamp_ns", &output);

    std::array<uint16_t, flowsql::packet::kMaxLayerDepth> layer_ids{};
    auto null_list_value =
        ReplacePacketBatchColumns(batch, {{9, MakeFixedListArray<arrow::UInt16Builder>(layer_ids, true)}});
    AssertPacketBatchCreateError(null_list_value, npm::NpmPacketBatchError::kInvalidColumn, "layer_ids", &output);
}

void TestNpmPacketBatchViewRejectsInvalidRows() {
    auto batch = MakeNpmPacketViewBatch()->Slice(0, 1);
    std::unique_ptr<npm::NpmPacketBatchView> output;
    assert(npm::NpmPacketBatchView::Create(batch, &output) == npm::NpmPacketBatchError::kNone);

    const uint32_t captured_len = std::static_pointer_cast<arrow::UInt32Array>(batch->column(1))->Value(0);
    auto captured_mismatch =
        ReplacePacketBatchColumns(batch, {{1, MakeOneValueArray<arrow::UInt32Builder>(captured_len - 1)}});
    AssertPacketBatchCreateError(captured_mismatch, npm::NpmPacketBatchError::kInvalidRow, "captured_len", &output);

    auto wire_too_small =
        ReplacePacketBatchColumns(batch, {{2, MakeOneValueArray<arrow::UInt32Builder>(captured_len - 1)}});
    AssertPacketBatchCreateError(wire_too_small, npm::NpmPacketBatchError::kInvalidRow, "wire_len", &output);

    auto invalid_status = ReplacePacketBatchColumns(batch, {{7, MakeOneValueArray<arrow::UInt8Builder>(uint8_t{9})}});
    AssertPacketBatchCreateError(invalid_status, npm::NpmPacketBatchError::kInvalidRow, "layer_status", &output);

    auto invalid_count = ReplacePacketBatchColumns(
        batch, {{8, MakeOneValueArray<arrow::UInt8Builder>(uint8_t{flowsql::packet::kMaxLayerDepth + 1})}});
    AssertPacketBatchCreateError(invalid_count, npm::NpmPacketBatchError::kInvalidRow, "layer_count", &output);

    std::array<uint32_t, flowsql::packet::kMaxLayerDepth> layer_offsets{};
    layer_offsets[0] = captured_len + 1;
    auto invalid_offset =
        ReplacePacketBatchColumns(batch, {{10, MakeFixedListArray<arrow::UInt32Builder>(layer_offsets)}});
    AssertPacketBatchCreateError(invalid_offset, npm::NpmPacketBatchError::kInvalidRow, "layer_offsets", &output);

    auto invalid_scope = ReplacePacketBatchColumns(batch, {{11, MakeOneValueArray<arrow::UInt8Builder>(uint8_t{2})}});
    AssertPacketBatchCreateError(invalid_scope, npm::NpmPacketBatchError::kInvalidRow, "endpoint_scope", &output);

    auto invalid_network_index =
        ReplacePacketBatchColumns(batch, {{12, MakeOneValueArray<arrow::UInt8Builder>(uint8_t{2})}});
    AssertPacketBatchCreateError(invalid_network_index, npm::NpmPacketBatchError::kInvalidRow, "network_layer_index",
                                 &output);

    auto invalid_transport_index =
        ReplacePacketBatchColumns(batch, {{13, MakeOneValueArray<arrow::UInt8Builder>(uint8_t{2})}});
    AssertPacketBatchCreateError(invalid_transport_index, npm::NpmPacketBatchError::kInvalidRow,
                                 "transport_layer_index", &output);

    auto invalid_payload =
        ReplacePacketBatchColumns(batch, {{14, MakeOneValueArray<arrow::UInt32Builder>(captured_len + 1)}});
    AssertPacketBatchCreateError(invalid_payload, npm::NpmPacketBatchError::kInvalidRow, "payload_offset", &output);

    auto invalid_ip_family = ReplacePacketBatchColumns(
        batch,
        {{21, MakeOneValueArray<arrow::UInt8Builder>(static_cast<uint8_t>(flowsql::packet::AddressFamily::kNone))}});
    AssertPacketBatchCreateError(invalid_ip_family, npm::NpmPacketBatchError::kInvalidRow, "src_ip", &output);

    std::vector<uint8_t> short_ipv6(15, 0x22);
    auto invalid_ipv6 = ReplacePacketBatchColumns(
        batch,
        {{17, MakeOneNullArray<arrow::UInt32Builder>()},
         {19, MakeOneBinaryArray(short_ipv6)},
         {21, MakeOneValueArray<arrow::UInt8Builder>(static_cast<uint8_t>(flowsql::packet::AddressFamily::kIPv6))}});
    AssertPacketBatchCreateError(invalid_ipv6, npm::NpmPacketBatchError::kInvalidRow, "src_ip_v6", &output);

    auto invalid_ports = ReplacePacketBatchColumns(batch, {{26, MakeOneValueArray<arrow::BooleanBuilder>(false)}});
    AssertPacketBatchCreateError(invalid_ports, npm::NpmPacketBatchError::kInvalidRow, "ports_valid", &output);

    auto invalid_protocol =
        ReplacePacketBatchColumns(batch, {{27, MakeOneValueArray<arrow::UInt8Builder>(
                                                   static_cast<uint8_t>(flowsql::packet::ProtocolStatus::kUnknown))}});
    AssertPacketBatchCreateError(invalid_protocol, npm::NpmPacketBatchError::kInvalidRow, "protocol_status", &output);
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
    assert(projector.ProjectActive(invalid, 1000, &output) == npm::NpmBasicProjectionError::kProtocolNameUnavailable);
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
    result.ip_family = index % 2 == 0 ? static_cast<uint8_t>(flowsql::packet::AddressFamily::kIPv6)
                                      : static_cast<uint8_t>(flowsql::packet::AddressFamily::kIPv4);
    result.transport_protocol = index % 2 == 0 ? static_cast<uint8_t>(flowsql::ipv6::eNext::UDP)
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
    result.wire_bytes_total = result.wire_bytes_ab + result.wire_bytes_ba;
    return result;
}

template <typename ArrayType>
std::shared_ptr<ArrayType> BasicResultColumn(const std::shared_ptr<arrow::RecordBatch>& batch, int index) {
    return std::static_pointer_cast<ArrayType>(batch->column(index));
}

template <typename ArrayType>
std::shared_ptr<ArrayType> BasicResultColumn(const std::shared_ptr<arrow::RecordBatch>& batch, const char* name) {
    const auto column = batch->GetColumnByName(name);
    assert(column != nullptr);
    return std::static_pointer_cast<ArrayType>(column);
}

void TestNpmBasicResultArrowEncoding() {
    std::shared_ptr<arrow::RecordBatch> output = MakeNpmPacketViewBatch();
    const auto original = output;
    std::string error = "stale";
    assert(npm::EncodeNpmBasicResults({}, &output, &error) == npm::NpmBasicEncodeError::kNone);
    assert(error.empty() && output != nullptr && output != original);
    assert(output->schema().get() == npm::NpmBasicResultSchema().get());
    assert(output->num_rows() == 0 && output->num_columns() == 32);
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
    assert(output->num_rows() == 6 && output->num_columns() == 32);
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
    const auto protocol_status = BasicResultColumn<arrow::StringArray>(output, 18);
    const auto protocol_id = BasicResultColumn<arrow::UInt16Array>(output, 19);
    const auto protocol_sub_id = BasicResultColumn<arrow::UInt16Array>(output, 20);
    const auto protocol = BasicResultColumn<arrow::StringArray>(output, 21);
    const auto end_reason = BasicResultColumn<arrow::StringArray>(output, 22);

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
    assert(npm::EncodeNpmBasicResults(results, &output, &error) == npm::NpmBasicEncodeError::kInvalidResult);
    assert(output == original);
    assert(error.find("result[1]") != std::string::npos);

    results[1].protocol_id.reset();
    results[1].a_ip = std::string(1, static_cast<char>(0xff));
    error = "stale";
    assert(npm::EncodeNpmBasicResults(results, &output, &error) == npm::NpmBasicEncodeError::kInvalidResult);
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
    PendingOutputBudget(npm::NpmAnalysisConfig config, std::shared_ptr<PendingOutputBudgetStats> stats)
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

std::vector<npm::NpmSessionResult> MakeSessionEncodingResults() {
    auto complete = MakeValidTcpSessionResult();
    complete.session_id = 101;
    complete.observation_domain_id = 201;
    complete.revision = 3;
    complete.observed_at = 1000;
    complete.a_ip = "192.0.2.10";
    complete.b_ip = "198.51.100.20";
    complete.a_port = 12345;
    complete.b_port = 443;
    complete.protocol_sub_id = 81;
    complete.packets_ab = 4;
    complete.packets_ba = 2;
    complete.wire_bytes_ba = 80;
    complete.payload_bytes_ba = 10;
    complete.wire_bps_ba = 64.0;
    complete.payload_bps_ba = 8.0;
    complete.tcp_unique_payload_bytes_ab = 18;
    complete.tcp_unique_payload_bytes_ba = 8;
    complete.tcp_unique_payload_bps_ab = 14.4;
    complete.tcp_unique_payload_bps_ba = 6.4;
    complete.tcp_rtt_samples = 2;
    complete.tcp_rtt_min_ns = 3;
    complete.tcp_rtt_mean_ns = 4;
    complete.tcp_rtt_max_ns = 5;
    complete.tcp_retrans_packets_ab = 1;
    complete.tcp_retrans_payload_bytes_ab = 2;

    auto partial = MakeValidTcpSessionResult();
    partial.session_id = 102;
    partial.is_final = true;
    partial.protocol_status = npm::NpmProtocolStatus::kUnknown;
    partial.protocol_id.reset();
    partial.protocol.reset();
    partial.end_reason = npm::NpmSessionEndReason::kClosed;
    partial.tcp_handshake_status = npm::NpmTcpHandshakeStatus::kPartial;
    partial.tcp_initiator = npm::NpmTcpInitiator::kB;
    partial.tcp_handshake_duration_ns.reset();
    partial.tcp_synack_rtt_ns = 4;
    partial.tcp_rtt_status = npm::NpmTcpRttStatus::kNoSample;
    partial.tcp_rtt_samples = 0;
    partial.tcp_rtt_min_ns.reset();
    partial.tcp_rtt_mean_ns.reset();
    partial.tcp_rtt_max_ns.reset();

    auto not_observed = MakeValidTcpSessionResult();
    not_observed.session_id = 103;
    not_observed.is_final = true;
    not_observed.protocol_status = npm::NpmProtocolStatus::kUnknown;
    not_observed.protocol_id.reset();
    not_observed.protocol.reset();
    not_observed.end_reason = npm::NpmSessionEndReason::kEof;
    not_observed.last_ns = not_observed.first_ns;
    not_observed.duration_ns = 0;
    not_observed.rate_status = npm::NpmRateStatus::kInsufficientSpan;
    not_observed.wire_bps_ab.reset();
    not_observed.wire_bps_ba.reset();
    not_observed.payload_bps_ab.reset();
    not_observed.payload_bps_ba.reset();
    not_observed.tcp_unique_payload_bps_ab.reset();
    not_observed.tcp_unique_payload_bps_ba.reset();
    not_observed.tcp_handshake_status = npm::NpmTcpHandshakeStatus::kNotObserved;
    not_observed.tcp_initiator.reset();
    not_observed.tcp_handshake_duration_ns.reset();
    not_observed.tcp_synack_rtt_ns.reset();
    not_observed.tcp_rtt_status = npm::NpmTcpRttStatus::kNoSample;
    not_observed.tcp_rtt_samples = 0;
    not_observed.tcp_rtt_min_ns.reset();
    not_observed.tcp_rtt_mean_ns.reset();
    not_observed.tcp_rtt_max_ns.reset();

    auto ambiguous = MakeValidTcpSessionResult();
    ambiguous.session_id = 104;
    ambiguous.protocol_sub_id = 81;
    ambiguous.tcp_handshake_status = npm::NpmTcpHandshakeStatus::kAmbiguous;
    ambiguous.tcp_initiator.reset();
    ambiguous.tcp_handshake_duration_ns.reset();
    ambiguous.tcp_synack_rtt_ns.reset();
    ambiguous.tcp_rtt_status = npm::NpmTcpRttStatus::kAmbiguous;
    ambiguous.tcp_rtt_samples.reset();
    ambiguous.tcp_rtt_min_ns.reset();
    ambiguous.tcp_rtt_mean_ns.reset();
    ambiguous.tcp_rtt_max_ns.reset();
    ambiguous.tcp_retransmission_status = npm::NpmTcpRetransmissionStatus::kAmbiguous;
    ambiguous.tcp_retrans_packets_ab.reset();
    ambiguous.tcp_retrans_packets_ba.reset();
    ambiguous.tcp_retrans_payload_bytes_ab.reset();
    ambiguous.tcp_retrans_payload_bytes_ba.reset();
    ambiguous.tcp_unique_payload_bytes_ab.reset();
    ambiguous.tcp_unique_payload_bytes_ba.reset();
    ambiguous.tcp_unique_payload_bps_ab.reset();
    ambiguous.tcp_unique_payload_bps_ba.reset();
    ambiguous.measurement_flags = npm::kNpmMeasurementSequenceAmbiguous;

    auto udp = MakeValidUdpSessionResult();
    udp.session_id = 105;
    udp.is_final = true;
    udp.protocol_status = npm::NpmProtocolStatus::kUnknown;
    udp.end_reason = npm::NpmSessionEndReason::kIdleTimeout;
    udp.measurement_flags = npm::kNpmMeasurementTruncatedPayload | npm::kNpmMeasurementTimestampRegression;

    std::vector<npm::NpmSessionResult> results{complete, partial, not_observed, ambiguous, udp};
    for (const auto& result : results) {
        assert(npm::ValidateNpmSessionResult(result) == npm::NpmSessionResultError::kNone);
    }
    return results;
}

template <typename ArrayType>
std::shared_ptr<ArrayType> SessionResultColumn(const std::shared_ptr<arrow::RecordBatch>& batch, int index) {
    return std::static_pointer_cast<ArrayType>(batch->column(index));
}

void TestNpmSessionResultArrowEncoding() {
    std::shared_ptr<arrow::RecordBatch> output = MakeNpmPacketViewBatch();
    const auto original = output;
    std::string error = "stale";
    assert(npm::EncodeNpmSessionResults({}, &output, &error) == npm::NpmSessionEncodeError::kNone);
    assert(error.empty() && output != original);
    assert(output->schema().get() == npm::NpmSessionResultSchema().get());
    assert(output->num_rows() == 0 && output->num_columns() == 49);
    assert(output->ValidateFull().ok());

    auto results = MakeSessionEncodingResults();
    assert(npm::EncodeNpmSessionResults(results, &output, &error) == npm::NpmSessionEncodeError::kNone);
    assert(error.empty() && output->schema().get() == npm::NpmSessionResultSchema().get());
    assert(output->num_rows() == 5 && output->num_columns() == 49);
    assert(output->ValidateFull().ok());

    const auto session_id = SessionResultColumn<arrow::UInt64Array>(output, 0);
    const auto observation_domain_id = SessionResultColumn<arrow::UInt64Array>(output, 1);
    const auto revision = SessionResultColumn<arrow::UInt64Array>(output, 2);
    const auto observed_at = SessionResultColumn<arrow::Int64Array>(output, 3);
    const auto is_final = SessionResultColumn<arrow::BooleanArray>(output, 4);
    const auto ip_family = SessionResultColumn<arrow::UInt8Array>(output, 5);
    const auto transport_protocol = SessionResultColumn<arrow::UInt8Array>(output, 6);
    const auto a_ip = SessionResultColumn<arrow::StringArray>(output, 7);
    const auto b_ip = SessionResultColumn<arrow::StringArray>(output, 8);
    const auto a_port = SessionResultColumn<arrow::UInt16Array>(output, 9);
    const auto b_port = SessionResultColumn<arrow::UInt16Array>(output, 10);
    const auto first_ns = SessionResultColumn<arrow::Int64Array>(output, 11);
    const auto last_ns = SessionResultColumn<arrow::Int64Array>(output, 12);
    const auto duration_ns = SessionResultColumn<arrow::Int64Array>(output, 13);
    const auto protocol_status = SessionResultColumn<arrow::StringArray>(output, 14);
    const auto protocol_id = SessionResultColumn<arrow::UInt16Array>(output, 15);
    const auto protocol_sub_id = SessionResultColumn<arrow::UInt16Array>(output, 16);
    const auto protocol = SessionResultColumn<arrow::StringArray>(output, 17);
    const auto end_reason = SessionResultColumn<arrow::StringArray>(output, 18);
    const auto packets_ab = SessionResultColumn<arrow::UInt64Array>(output, 19);
    const auto packets_ba = SessionResultColumn<arrow::UInt64Array>(output, 20);
    const auto wire_bytes_ab = SessionResultColumn<arrow::UInt64Array>(output, 21);
    const auto wire_bytes_ba = SessionResultColumn<arrow::UInt64Array>(output, 22);
    const auto payload_bytes_ab = SessionResultColumn<arrow::UInt64Array>(output, 23);
    const auto payload_bytes_ba = SessionResultColumn<arrow::UInt64Array>(output, 24);
    const auto rate_status = SessionResultColumn<arrow::StringArray>(output, 25);
    const auto wire_bps_ab = SessionResultColumn<arrow::DoubleArray>(output, 26);
    const auto wire_bps_ba = SessionResultColumn<arrow::DoubleArray>(output, 27);
    const auto payload_bps_ab = SessionResultColumn<arrow::DoubleArray>(output, 28);
    const auto payload_bps_ba = SessionResultColumn<arrow::DoubleArray>(output, 29);
    const auto unique_bytes_ab = SessionResultColumn<arrow::UInt64Array>(output, 30);
    const auto unique_bytes_ba = SessionResultColumn<arrow::UInt64Array>(output, 31);
    const auto unique_bps_ab = SessionResultColumn<arrow::DoubleArray>(output, 32);
    const auto unique_bps_ba = SessionResultColumn<arrow::DoubleArray>(output, 33);
    const auto handshake_status = SessionResultColumn<arrow::StringArray>(output, 34);
    const auto initiator = SessionResultColumn<arrow::StringArray>(output, 35);
    const auto handshake_duration = SessionResultColumn<arrow::Int64Array>(output, 36);
    const auto synack_rtt = SessionResultColumn<arrow::Int64Array>(output, 37);
    const auto rtt_status = SessionResultColumn<arrow::StringArray>(output, 38);
    const auto rtt_samples = SessionResultColumn<arrow::UInt64Array>(output, 39);
    const auto rtt_min = SessionResultColumn<arrow::Int64Array>(output, 40);
    const auto rtt_mean = SessionResultColumn<arrow::Int64Array>(output, 41);
    const auto rtt_max = SessionResultColumn<arrow::Int64Array>(output, 42);
    const auto retrans_status = SessionResultColumn<arrow::StringArray>(output, 43);
    const auto retrans_packets_ab = SessionResultColumn<arrow::UInt64Array>(output, 44);
    const auto retrans_packets_ba = SessionResultColumn<arrow::UInt64Array>(output, 45);
    const auto retrans_bytes_ab = SessionResultColumn<arrow::UInt64Array>(output, 46);
    const auto retrans_bytes_ba = SessionResultColumn<arrow::UInt64Array>(output, 47);
    const auto measurement_flags = SessionResultColumn<arrow::UInt32Array>(output, 48);

    assert(session_id->Value(0) == 101 && session_id->Value(4) == 105);
    assert(observation_domain_id->Value(0) == 201 && revision->Value(0) == 3);
    assert(observed_at->Value(0) == 1000 && !is_final->Value(0) && is_final->Value(4));
    assert(ip_family->Value(0) == 4 && transport_protocol->Value(0) == 6);
    assert(transport_protocol->Value(4) == 17);
    assert(a_ip->GetString(0) == "192.0.2.10" && b_ip->GetString(0) == "198.51.100.20");
    assert(a_port->Value(0) == 12345 && b_port->Value(0) == 443);
    assert(first_ns->Value(0) == 10 && last_ns->Value(0) == 20 && duration_ns->Value(0) == 10);
    assert(protocol_status->GetString(0) == "identified");
    assert(protocol_status->GetString(1) == "unknown");
    assert(protocol_id->Value(0) == 80 && protocol_sub_id->Value(0) == 81);
    assert(protocol->GetString(0) == "HTTP" && protocol_id->IsNull(1));
    assert(end_reason->IsNull(0) && end_reason->GetString(1) == "closed");
    assert(end_reason->GetString(2) == "eof" && end_reason->GetString(4) == "idle_timeout");
    assert(packets_ab->Value(0) == 4 && packets_ba->Value(0) == 2);
    assert(wire_bytes_ab->Value(0) == 100 && wire_bytes_ba->Value(0) == 80);
    assert(payload_bytes_ab->Value(0) == 20 && payload_bytes_ba->Value(0) == 10);
    assert(rate_status->GetString(0) == "valid");
    assert(rate_status->GetString(2) == "insufficient_span");
    assert(wire_bps_ab->Value(0) == 80.0 && wire_bps_ba->Value(0) == 64.0);
    assert(payload_bps_ab->Value(0) == 16.0 && payload_bps_ba->Value(0) == 8.0);
    assert(wire_bps_ab->IsNull(2) && payload_bps_ba->IsNull(2));
    assert(unique_bytes_ab->Value(0) == 18 && unique_bytes_ba->Value(0) == 8);
    assert(unique_bps_ab->Value(0) == 14.4 && unique_bps_ba->Value(0) == 6.4);
    assert(unique_bytes_ab->IsNull(3) && unique_bytes_ab->IsNull(4));
    assert(handshake_status->GetString(0) == "complete");
    assert(handshake_status->GetString(1) == "partial");
    assert(handshake_status->GetString(2) == "not_observed");
    assert(handshake_status->GetString(3) == "ambiguous");
    assert(handshake_status->GetString(4) == "not_applicable");
    assert(initiator->GetString(0) == "a" && initiator->GetString(1) == "b");
    assert(initiator->IsNull(2) && handshake_duration->Value(0) == 5);
    assert(handshake_duration->IsNull(1) && synack_rtt->Value(1) == 4);
    assert(rtt_status->GetString(0) == "valid");
    assert(rtt_status->GetString(1) == "no_sample");
    assert(rtt_status->GetString(3) == "ambiguous");
    assert(rtt_status->GetString(4) == "not_applicable");
    assert(rtt_samples->Value(0) == 2 && rtt_samples->Value(1) == 0);
    assert(rtt_min->Value(0) == 3 && rtt_mean->Value(0) == 4 && rtt_max->Value(0) == 5);
    assert(rtt_samples->IsNull(3) && rtt_min->IsNull(3));
    assert(retrans_status->GetString(0) == "valid");
    assert(retrans_status->GetString(3) == "ambiguous");
    assert(retrans_status->GetString(4) == "not_applicable");
    assert(retrans_packets_ab->Value(0) == 1 && retrans_packets_ba->Value(0) == 0);
    assert(retrans_bytes_ab->Value(0) == 2 && retrans_bytes_ba->Value(0) == 0);
    assert(retrans_packets_ab->IsNull(3) && retrans_packets_ab->IsNull(4));
    assert(measurement_flags->Value(3) == npm::kNpmMeasurementSequenceAmbiguous);
    assert(measurement_flags->Value(4) ==
           (npm::kNpmMeasurementTruncatedPayload | npm::kNpmMeasurementTimestampRegression));

    results[0].a_ip = "mutated";
    results[0].protocol = "mutated";
    assert(a_ip->GetString(0) == "192.0.2.10" && protocol->GetString(0) == "HTTP");
}

void TestNpmSessionResultArrowEncodingRejectsInvalidInput() {
    auto results = MakeSessionEncodingResults();
    std::shared_ptr<arrow::RecordBatch> output;
    std::string error;
    assert(npm::EncodeNpmSessionResults(results, &output, &error) == npm::NpmSessionEncodeError::kNone);
    const auto original = output;

    results[1].revision = 0;
    error = "stale";
    assert(npm::EncodeNpmSessionResults(results, &output, &error) == npm::NpmSessionEncodeError::kInvalidResult);
    assert(output == original && error.find("result[1]") != std::string::npos);

    const std::string invalid_utf8(1, static_cast<char>(0xff));
    results = MakeSessionEncodingResults();
    results[0].a_ip = invalid_utf8;
    assert(npm::EncodeNpmSessionResults(results, &output, &error) == npm::NpmSessionEncodeError::kInvalidResult);
    assert(output == original && error.find("result[0].a_ip") != std::string::npos);

    results = MakeSessionEncodingResults();
    results[0].b_ip = invalid_utf8;
    assert(npm::EncodeNpmSessionResults(results, &output, &error) == npm::NpmSessionEncodeError::kInvalidResult);
    assert(output == original && error.find("result[0].b_ip") != std::string::npos);

    results = MakeSessionEncodingResults();
    results[0].protocol = invalid_utf8;
    assert(npm::EncodeNpmSessionResults(results, &output, &error) == npm::NpmSessionEncodeError::kInvalidResult);
    assert(output == original && error.find("result[0].protocol") != std::string::npos);

    error = "stale";
    assert(npm::EncodeNpmSessionResults(results, nullptr, &error) == npm::NpmSessionEncodeError::kNullOutput);
    assert(output == original && error.find("output") != std::string::npos);
}

void TestNpmSessionResultPendingOutputLeaseAndErrors() {
    const auto results = MakeSessionEncodingResults();
    std::shared_ptr<arrow::RecordBatch> reference;
    assert(npm::EncodeNpmSessionResults(results, &reference) == npm::NpmSessionEncodeError::kNone);
    const uint64_t expected_bytes = BasicResultBufferBytes(reference);
    assert(expected_bytes > 0);

    auto config = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline);
    config.max_pending_output_bytes = expected_bytes;
    auto stats = std::make_shared<PendingOutputBudgetStats>();
    auto budget = std::make_shared<PendingOutputBudget>(config, stats);
    std::shared_ptr<arrow::RecordBatch> output;
    std::string error = "stale";
    assert(npm::EncodeNpmSessionResultsWithBudget(results, budget, &output, &error) ==
           npm::NpmSessionEncodeError::kNone);
    assert(error.empty() && BasicResultBufferBytes(output) == expected_bytes);
    assert(stats->reserve_calls == 1 && stats->release_calls == 0);
    assert(stats->last_reserve_category == npm::NpmBudgetCategory::kPendingOutput);
    assert(stats->last_reserved_bytes == expected_bytes);
    assert(budget->Usage().pending_output_bytes == expected_bytes);

    auto output_copy = output;
    output.reset();
    assert(stats->release_calls == 0 && budget->Usage().pending_output_bytes == expected_bytes);
    output_copy.reset();
    assert(stats->release_calls == 1 && stats->last_released_bytes == expected_bytes);
    assert(budget->Usage().pending_output_bytes == 0);

    config.max_pending_output_bytes = expected_bytes - 1;
    auto blocked_stats = std::make_shared<PendingOutputBudgetStats>();
    auto blocked_budget = std::make_shared<PendingOutputBudget>(config, blocked_stats);
    std::shared_ptr<arrow::RecordBatch> blocked_output = reference;
    const auto original = blocked_output;
    error = "stale";
    assert(npm::EncodeNpmSessionResultsWithBudget(results, blocked_budget, &blocked_output, &error) ==
           npm::NpmSessionEncodeError::kBudgetError);
    assert(blocked_output == original && error.find("pending output budget") != std::string::npos);
    assert(blocked_stats->reserve_calls == 1 && blocked_stats->release_calls == 0);
    assert(blocked_budget->Usage().pending_output_bytes == 0);

    error = "stale";
    assert(npm::EncodeNpmSessionResultsWithBudget(results, budget, nullptr, &error) ==
           npm::NpmSessionEncodeError::kNullOutput);
    assert(error.find("output") != std::string::npos);
    error = "stale";
    assert(npm::EncodeNpmSessionResultsWithBudget(results, {}, &blocked_output, &error) ==
           npm::NpmSessionEncodeError::kNullBudget);
    assert(blocked_output == original && error.find("budget") != std::string::npos);

    config.max_pending_output_bytes = expected_bytes;
    auto lifetime_stats = std::make_shared<PendingOutputBudgetStats>();
    auto lifetime_budget = std::make_shared<PendingOutputBudget>(config, lifetime_stats);
    std::weak_ptr<PendingOutputBudget> weak_budget = lifetime_budget;
    std::shared_ptr<arrow::RecordBatch> lifetime_output;
    assert(npm::EncodeNpmSessionResultsWithBudget(results, lifetime_budget, &lifetime_output) ==
           npm::NpmSessionEncodeError::kNone);
    lifetime_budget.reset();
    assert(!weak_budget.expired() && lifetime_stats->release_calls == 0);
    lifetime_output.reset();
    assert(weak_budget.expired() && lifetime_stats->release_calls == 1);
}

npm::NpmSessionEndEvent MakeCollectorEndEvent(uint64_t session_id, npm::NpmSessionEndReason reason,
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

    const auto session_result = MakeValidUdpSessionResult();
    assert(collector.WriteSession(session_result) == ENOTSUP);
    assert(collector.pending_results() == 0);

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
    const auto generated_start_ns = ResultClockNowNs();
    auto status = collector.Drain(normal_events, projector, budget, &normal_output);
    const auto generated_end_ns = ResultClockNowNs();
    assert(status.error == npm::NpmBasicDrainError::kNone && status.event_index == -1);
    assert(collector.pending_results() == 0 && normal_output != nullptr);
    assert(normal_output->num_rows() == 3 && normal_output->schema()->Equals(*npm::NpmBasicResultSchema(), true));
    const auto normal_ids = BasicResultColumn<arrow::UInt64Array>(normal_output, 0);
    const auto normal_observed = BasicResultColumn<arrow::Int64Array>(normal_output, 3);
    const auto normal_final = BasicResultColumn<arrow::BooleanArray>(normal_output, 4);
    const auto normal_a_ip = BasicResultColumn<arrow::StringArray>(normal_output, 7);
    const auto normal_reason = BasicResultColumn<arrow::StringArray>(normal_output, "end_reason");
    assert(normal_ids->Value(0) == 90 && normal_ids->Value(1) == 91 && normal_ids->Value(2) == 92);
    assert(normal_observed->Value(0) == 9000 && normal_a_ip->GetString(0) == "a-90");
    for (int64_t row : {1, 2})
        assert(normal_observed->Value(row) >= generated_start_ns && normal_observed->Value(row) <= generated_end_ns);
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
    const auto eof_start_ns = ResultClockNowNs();
    status = collector.Drain(eof_events, projector, budget, &eof_output);
    const auto eof_end_ns = ResultClockNowNs();
    assert(status.error == npm::NpmBasicDrainError::kNone && eof_output->num_rows() == 2);
    const auto eof_ids = BasicResultColumn<arrow::UInt64Array>(eof_output, 0);
    const auto eof_reason = BasicResultColumn<arrow::StringArray>(eof_output, "end_reason");
    assert(eof_ids->Value(0) == 93 && eof_ids->Value(1) == 94);
    const auto eof_observed = BasicResultColumn<arrow::Int64Array>(eof_output, 3)->Value(1);
    assert(eof_observed >= eof_start_ns && eof_observed <= eof_end_ns);
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

void TestNpmBasicResultCollectorRoutesEnabledEntities() {
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    std::unique_ptr<npm::NpmProtocolContext> context;
    assert(npm::NpmProtocolContext::Create(&querier, &context) == npm::NpmProtocolContextError::kNone);
    npm::NpmBasicResultProjector projector(*context);

    auto analysis = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline);
    auto budget_stats = std::make_shared<PendingOutputBudgetStats>();
    auto budget = std::make_shared<PendingOutputBudget>(analysis, budget_stats);

    npm::NpmBasicFeatureConfig session_only;
    session_only.basic_enabled = false;
    session_only.session_enabled = true;
    session_only.observing = npm::NpmResultEntity::kSession;
    npm::NpmBasicResultCollector session_collector(session_only);

    const auto basic_result = MakeBasicEncodingResult(110);
    assert(session_collector.WriteBasic(basic_result) == ENOTSUP);
    auto invalid_session = MakeValidUdpSessionResult();
    invalid_session.revision = 0;
    assert(session_collector.WriteSession(invalid_session) == EINVAL);

    auto session_result = MakeValidUdpSessionResult();
    session_result.session_id = 111;
    session_result.a_ip = "session-original";
    assert(session_collector.WriteSession(session_result) == 0);
    session_result.a_ip = "mutated";
    assert(session_collector.pending_results() == 1);

    auto ignored_event = MakeCollectorEndEvent(112, npm::NpmSessionEndReason::kClosed, 11200);
    ignored_event.snapshot.key.input_namespace.clear();
    std::shared_ptr<arrow::RecordBatch> output = MakeNpmPacketViewBatch();
    auto status = session_collector.Drain({ignored_event}, projector, budget, &output);
    assert(status.error == npm::NpmBasicDrainError::kNone);
    assert(status.encode_error == npm::NpmBasicEncodeError::kNone);
    assert(status.session_encode_error == npm::NpmSessionEncodeError::kNone);
    assert(session_collector.pending_results() == 0 && output->num_rows() == 1);
    assert(output->schema()->Equals(*npm::NpmSessionResultSchema(), true));
    assert(SessionResultColumn<arrow::StringArray>(output, 7)->GetString(0) == "session-original");

    output.reset();
    status = session_collector.Drain({}, projector, budget, &output);
    assert(status.error == npm::NpmBasicDrainError::kNone && output->num_rows() == 0);
    assert(output->num_columns() == 49);
    assert(output->schema()->Equals(*npm::NpmSessionResultSchema(), true));
    output.reset();

    npm::NpmBasicFeatureConfig both_basic;
    both_basic.basic_enabled = true;
    both_basic.session_enabled = true;
    both_basic.observing = npm::NpmResultEntity::kBasic;
    npm::NpmBasicResultCollector basic_collector(both_basic);
    invalid_session = MakeValidUdpSessionResult();
    invalid_session.revision = 0;
    assert(basic_collector.WriteSession(invalid_session) == EINVAL);
    assert(basic_collector.WriteSession(MakeValidUdpSessionResult()) == 0);
    assert(basic_collector.pending_results() == 0);
    assert(basic_collector.WriteBasic(basic_result) == 0);
    assert(basic_collector.pending_results() == 1);
    status = basic_collector.Drain({}, projector, budget, &output);
    assert(status.error == npm::NpmBasicDrainError::kNone);
    assert(basic_collector.pending_results() == 0 && output->num_rows() == 1);
    assert(output->schema()->Equals(*npm::NpmBasicResultSchema(), true));
    assert(BasicResultColumn<arrow::UInt64Array>(output, 0)->Value(0) == basic_result.session_id);
    output.reset();

    npm::NpmBasicFeatureConfig both_session = both_basic;
    both_session.observing = npm::NpmResultEntity::kSession;
    npm::NpmBasicResultCollector both_session_collector(both_session);
    auto invalid_basic = MakeBasicEncodingResult(113);
    invalid_basic.protocol_id = 7;
    assert(both_session_collector.WriteBasic(invalid_basic) == EINVAL);
    assert(both_session_collector.WriteBasic(MakeBasicEncodingResult(114)) == 0);
    assert(both_session_collector.pending_results() == 0);

    auto observed_session = MakeValidUdpSessionResult();
    observed_session.session_id = 115;
    assert(both_session_collector.WriteSession(observed_session) == 0);
    auto projected_event = MakeCollectorEndEvent(116, npm::NpmSessionEndReason::kIdleTimeout, 11600);
    npm::NpmBasicResult projected_active;
    assert(projector.ProjectActive(projected_event.snapshot.View(), 11500, &projected_active) ==
           npm::NpmBasicProjectionError::kNone);
    assert(projector.tracked_sessions() == 1);
    status = both_session_collector.Drain({projected_event}, projector, budget, &output);
    assert(status.error == npm::NpmBasicDrainError::kNone);
    assert(projector.tracked_sessions() == 0);
    assert(both_session_collector.pending_results() == 0 && output->num_rows() == 1);
    assert(output->schema()->Equals(*npm::NpmSessionResultSchema(), true));
    assert(SessionResultColumn<arrow::UInt64Array>(output, 0)->Value(0) == observed_session.session_id);
    output.reset();

    npm::NpmBasicResultCollector blocked_collector(session_only);
    assert(blocked_collector.WriteSession(MakeValidUdpSessionResult()) == 0);
    analysis.max_pending_output_bytes = 1;
    auto small_stats = std::make_shared<PendingOutputBudgetStats>();
    auto small_budget = std::make_shared<PendingOutputBudget>(analysis, small_stats);
    output = MakeNpmPacketViewBatch();
    const auto original = output;
    status = blocked_collector.Drain({}, projector, small_budget, &output);
    assert(status.error == npm::NpmBasicDrainError::kEncodeError);
    assert(status.encode_error == npm::NpmBasicEncodeError::kNone);
    assert(status.session_encode_error == npm::NpmSessionEncodeError::kBudgetError);
    assert(blocked_collector.pending_results() == 1 && output == original);
    assert(small_stats->reserve_calls == 1 && small_stats->release_calls == 0);
}

void AddEofFlushSession(npm::NpmSessionTable& table, const npm::NpmObservationDomainMap& domain_map,
                        const PacketFixture& fixture, int64_t timestamp_ns) {
    flowsql::packet::PacketMeta meta;
    const auto binding = BuildBinding(domain_map, fixture, 1, timestamp_ns, 100, &meta);
    npm::NpmSessionView view;
    assert(ObserveActive(table, binding, meta, &view) == npm::NpmSessionTableError::kNone);
}

class EofWritingModule final : public npm::INpmAnalysisModule {
 public:
    EofWritingModule(uint64_t marker, std::vector<uint64_t>* events) : marker_(marker), events_(events) {}

    int OnPacket(const npm::NpmPacketView&, const npm::NpmSessionView&, npm::INpmResultWriter&) override { return 0; }

    int OnSessionSnapshot(const npm::NpmSessionView&, int64_t, npm::INpmResultWriter&) override { return 0; }

    int OnSessionEnd(const npm::NpmSessionView& session, npm::NpmSessionEndReason reason, int64_t observed_at_ns,
                     npm::INpmResultWriter& writer) override {
        assert(reason == npm::NpmSessionEndReason::kEof);
        events_->push_back(1000 + session.session_id * 10 + marker_);
        end_observed_ats.push_back(observed_at_ns);
        auto result = MakeBasicEncodingResult(100 + session.session_id * 10 + marker_);
        result.observed_at = 4000 + static_cast<int64_t>(session.session_id * 10 + marker_);
        return writer.WriteBasic(result);
    }

    std::vector<int64_t> end_observed_ats;

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

    const auto generated_start_ns = ResultClockNowNs();
    auto status = flusher.Flush(5000, table, modules, collector, projector, budget, &output);
    const auto generated_end_ns = ResultClockNowNs();
    assert(status.error == npm::NpmEofFlushError::kNone);
    assert(flusher.state() == npm::NpmEofFlushState::kFlushed);
    assert(table.size() == 0 && collector.pending_results() == 0);
    assert((events == std::vector<uint64_t>{1011, 1012, 1021, 1022}));
    assert((first_module.end_observed_ats == std::vector<int64_t>{5000, 5000}));
    assert(first_module.end_observed_ats == second_module.end_observed_ats);
    assert(output != nullptr && output->num_rows() == 6);
    const auto session_ids = BasicResultColumn<arrow::UInt64Array>(output, 0);
    const auto observed_at = BasicResultColumn<arrow::Int64Array>(output, 3);
    const auto is_final = BasicResultColumn<arrow::BooleanArray>(output, 4);
    const auto end_reason = BasicResultColumn<arrow::StringArray>(output, "end_reason");
    assert(session_ids->Value(0) == 111 && session_ids->Value(1) == 112);
    assert(session_ids->Value(2) == 121 && session_ids->Value(3) == 122);
    assert(session_ids->Value(4) == 1 && session_ids->Value(5) == 2);
    assert(!is_final->Value(0) && !is_final->Value(1) && !is_final->Value(2));
    assert(!is_final->Value(3) && is_final->Value(4) && is_final->Value(5));
    assert(observed_at->Value(0) == 4011 && observed_at->Value(3) == 4022);
    for (int64_t row : {4, 5})
        assert(observed_at->Value(row) >= generated_start_ns && observed_at->Value(row) <= generated_end_ns);
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
    status = empty_flusher.Flush(7000, empty_table, {}, empty_collector, empty_projector, budget, &empty_output);
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
    auto status =
        cancelled.Flush(1000, cancelled_table, modules, cancelled_collector, cancelled_projector, budget, &output);
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
    status = null_output.Flush(1000, null_output_table, modules, null_output_collector, null_output_projector, budget,
                               nullptr);
    assert(status.error == npm::NpmEofFlushError::kNullOutput);
    assert(null_output.state() == npm::NpmEofFlushState::kFailed && null_output_table.size() == 1);

    npm::NpmSessionTable null_budget_table(config);
    AddEofFlushSession(null_budget_table, domain_map, packet, 100);
    npm::NpmBasicResultCollector null_budget_collector;
    npm::NpmBasicResultProjector null_budget_projector(*context);
    npm::NpmEofFlusher null_budget;
    status =
        null_budget.Flush(1000, null_budget_table, modules, null_budget_collector, null_budget_projector, {}, &output);
    assert(status.error == npm::NpmEofFlushError::kNullBudget);
    assert(null_budget.state() == npm::NpmEofFlushState::kFailed && null_budget_table.size() == 1);

    npm::NpmSessionTable null_module_table(config);
    AddEofFlushSession(null_module_table, domain_map, packet, 100);
    npm::NpmBasicResultCollector null_module_collector;
    npm::NpmBasicResultProjector null_module_projector(*context);
    npm::NpmEofFlusher null_module;
    modules.push_back(nullptr);
    status = null_module.Flush(1000, null_module_table, modules, null_module_collector, null_module_projector, budget,
                               &output);
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
    status = module_error.Flush(1000, module_error_table, modules, module_error_collector, module_error_projector,
                                budget, &output);
    assert(status.error == npm::NpmEofFlushError::kModuleError && status.module_error == EBUSY);
    assert(module_error.state() == npm::NpmEofFlushState::kFailed && module_error_table.size() == 0);
    assert(module_error_collector.pending_results() == 1 && output == original);
    assert((events == std::vector<uint64_t>{1011}));
    module.end_error = 0;
    status = module_error.Flush(2000, module_error_table, modules, module_error_collector, module_error_projector,
                                budget, &output);
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
    status = drain_error.Flush(3000, drain_error_table, modules, drain_error_collector, drain_error_projector,
                               small_budget, &output);
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
    assert(npm::EncodeNpmBasicResultsWithBudget(results, budget, &output, &error) == npm::NpmBasicEncodeError::kNone);
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
    assert(npm::EncodeNpmBasicResultsWithBudget({}, empty_budget, &empty_output) == npm::NpmBasicEncodeError::kNone);
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
    assert(npm::EncodeNpmBasicResultsWithBudget(results, budget, &normal_output) == npm::NpmBasicEncodeError::kNone);
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
    assert(npm::EncodeNpmBasicResultsWithBudget(results, {}, &output, &error) == npm::NpmBasicEncodeError::kNullBudget);
    assert(error.find("budget") != std::string::npos && output == original);

    results[0].protocol_id = 80;
    error = "stale";
    assert(npm::EncodeNpmBasicResultsWithBudget(results, budget, &output, &error) ==
           npm::NpmBasicEncodeError::kInvalidResult);
    assert(error.find("result[0]") != std::string::npos);
    assert(output == original && stats->reserve_calls == 0 && stats->release_calls == 0);
}

npm::NpmBasicTaskConfigStatus ParseTaskConfigFailure(
    const char* json, npm::NpmBasicTaskConfigError expected_error, const char* expected_field = "",
    npm::NpmParameterErrorV1 expected_parameter_error = npm::NpmParameterErrorV1::kNone,
    const char* expected_parameter_path = "") {
    npm::NpmBasicTaskConfig output;
    output.analysis = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kRealtime);
    output.analysis.max_active_sessions = 123;
    output.domains.input_namespace = "sentinel";
    output.domains.bindings = {{99, 88}};
    output.features.basic_enabled = false;
    output.features.session_enabled = true;
    output.features.observing = npm::NpmResultEntity::kSession;
    output.features.session_max_tcp_ranges_per_direction = 321;

    const auto status = npm::ParseNpmBasicTaskConfig(json, &output);
    assert(status.error == expected_error);
    assert(status.field == expected_field);
    assert(status.parameter_status.error == expected_parameter_error);
    assert(status.parameter_status.path == expected_parameter_path);
    assert(output.analysis.run_mode == npm::NpmRunMode::kRealtime);
    assert(output.analysis.result_mode == npm::NpmResultMode::kPeriodicSnapshot);
    assert(output.analysis.max_active_sessions == 123);
    assert(output.domains.input_namespace == "sentinel");
    assert(output.domains.bindings.size() == 1);
    assert(output.domains.bindings[0].source_id == 99);
    assert(output.domains.bindings[0].observation_domain_id == 88);
    assert(!output.features.basic_enabled && output.features.session_enabled);
    assert(output.features.observing == npm::NpmResultEntity::kSession);
    assert(output.features.session_max_tcp_ranges_per_direction == 321);
    return status;
}

void TestNpmBasicTaskConfigParsesOwnedValues() {
    static_assert(std::is_default_constructible_v<npm::NpmBasicTaskConfig>);
    static_assert(std::is_same_v<decltype(npm::NpmBasicTaskConfigStatus::field), std::string>);

    npm::NpmBasicTaskConfig minimal;
    const auto minimal_status =
        npm::ParseNpmBasicTaskConfig(R"JSON({"input_namespace":"minimal","source_domains":"0:0"})JSON", &minimal);
    assert(minimal_status.error == npm::NpmBasicTaskConfigError::kNone);
    assert(minimal.analysis.run_mode == npm::NpmRunMode::kOffline);
    assert(minimal.analysis.result_mode == npm::NpmResultMode::kPeriodicSnapshot);
    assert(minimal.analysis.max_tracked_bytes == npm::kNpmDefaultTrackedBytes);
    assert(minimal.analysis.payload_sample_packets == npm::kNpmDefaultPayloadSamplePackets);
    assert(minimal.features.basic_enabled && !minimal.features.session_enabled);
    assert(minimal.features.observing == npm::NpmResultEntity::kBasic);
    assert(minimal.features.session_max_tcp_ranges_per_direction == npm::kNpmDefaultSessionTcpRangesPerDirection);
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
    assert(output.domains.bindings[0].source_id == 0 && output.domains.bindings[0].observation_domain_id == 7);
    assert(output.domains.bindings[1].source_id == 1 && output.domains.bindings[1].observation_domain_id == 7);
    assert(output.domains.bindings[2].source_id == 2 && output.domains.bindings[2].observation_domain_id == 8);

    json.assign(json.size(), 'x');
    assert(output.domains.input_namespace == "pcapfile.capture");
    assert(output.domains.bindings[2].observation_domain_id == 8);

    status = npm::ParseNpmBasicTaskConfig(
        R"JSON({"source_domains":"9:10","input_namespace":"live","run_mode":"realtime"})JSON", &output);
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
        R"JSON({"input_namespace":"limits","source_domains":"4294967295:18446744073709551615"})JSON", &output);
    assert(status.error == npm::NpmBasicTaskConfigError::kNone);
    assert(output.domains.bindings[0].source_id == UINT32_MAX);
    assert(output.domains.bindings[0].observation_domain_id == UINT64_MAX);
}

void TestNpmBasicTaskConfigInputDefaults() {
    npm::NpmBasicTaskConfig config;
    for (const char* json : {"{}", R"({"source_domains":"all"})", R"({"parameters":"{\"schema_version\":1}"})"}) {
        assert(npm::ParseNpmBasicTaskConfig(json, &config).error == npm::NpmBasicTaskConfigError::kNone);
        assert(config.domains.input_namespace == "default");
        assert(config.domains.bindings.empty() && config.domains.source_id_as_domain);
        uint64_t domain = 99;
        assert(npm::ResolveNpmObservationDomain(config.domains, UINT32_MAX, &domain) ==
               npm::NpmObservationDomainError::kNone);
        assert(domain == UINT32_MAX);
    }
    assert(npm::ParseNpmBasicTaskConfig("{}", &config, false, npm::ProductionNpmModuleCatalogV1(), "pcapfile.http")
               .error == npm::NpmBasicTaskConfigError::kNone);
    assert(config.domains.input_namespace == "pcapfile.http");
    for (const char* json :
         {R"({"features":"basic,session,dns,http1,tls,icmp"})",
          R"({"features":"basic,session,dns,http1,tls,icmp","parameters":"{\"schema_version\":1}"})"}) {
        assert(
            npm::ParseNpmBasicTaskConfig(json, &config, false, npm::ProductionNpmModuleCatalogV1(), "netadapter.eth0")
                .error == npm::NpmBasicTaskConfigError::kNone);
        assert(config.domains.input_namespace == "netadapter.eth0");
        assert(config.domains.source_id_as_domain && config.domains.bindings.empty());
    }
    assert(npm::ParseNpmBasicTaskConfig(R"({"input_namespace":"custom"})", &config, false,
                                        npm::ProductionNpmModuleCatalogV1(), "pcapfile.http")
               .error == npm::NpmBasicTaskConfigError::kNone);
    assert(config.domains.input_namespace == "custom" && config.domains.source_id_as_domain);
    assert(npm::ParseNpmBasicTaskConfig(R"({"source_domains":"0:7;1:7"})", &config).error ==
           npm::NpmBasicTaskConfigError::kNone);
    assert(config.domains.input_namespace == "default" && !config.domains.source_id_as_domain);
    uint64_t domain = 99;
    assert(npm::ResolveNpmObservationDomain(config.domains, 1, &domain) == npm::NpmObservationDomainError::kNone);
    assert(domain == 7);
    assert(npm::ResolveNpmObservationDomain(config.domains, 2, &domain) ==
           npm::NpmObservationDomainError::kUnknownSourceId);

    // Identical endpoints from separate sources stay separate; an explicit mapping may merge them.
    const auto packet = MakeIpv6UdpPacket("2001:db8::1", 53000, "2001:db8::2", 53, {});
    for (const bool merge : {false, true}) {
        assert(npm::ParseNpmBasicTaskConfig(merge ? R"({"source_domains":"0:7;1:7"})" : "{}", &config).error ==
               npm::NpmBasicTaskConfigError::kNone);
        npm::NpmSessionTable table(2);
        flowsql::packet::PacketMeta meta;
        npm::NpmSessionView first;
        npm::NpmSessionView second;
        const auto binding0 = BuildBinding(config.domains, packet, 0, 10, 80, &meta);
        assert(ObserveActive(table, binding0, meta, &first) == npm::NpmSessionTableError::kNone);
        const auto binding1 = BuildBinding(config.domains, packet, 1, 20, 80, &meta);
        assert(ObserveActive(table, binding1, meta, &second) == npm::NpmSessionTableError::kNone);
        assert((first.session_id == second.session_id) == merge);
        assert(table.size() == (merge ? 1u : 2u));
    }
}

void TestNpmBasicTaskConfigRejectsMalformedFieldsAtomically() {
    ParseTaskConfigFailure(nullptr, npm::NpmBasicTaskConfigError::kNullInput);
    ParseTaskConfigFailure("", npm::NpmBasicTaskConfigError::kEmptyInput);
    npm::NpmBasicTaskConfigStatus status;
    status = npm::ParseNpmBasicTaskConfig("{}", nullptr);
    assert(status.error == npm::NpmBasicTaskConfigError::kNullOutput);
    ParseTaskConfigFailure("{", npm::NpmBasicTaskConfigError::kInvalidJson);
    ParseTaskConfigFailure("[]", npm::NpmBasicTaskConfigError::kInvalidJson);
    ParseTaskConfigFailure(R"JSON({"input_namespace":"a","input_namespace":"b","source_domains":"0:0"})JSON",
                           npm::NpmBasicTaskConfigError::kDuplicateField, "input_namespace");
    ParseTaskConfigFailure(R"JSON({"input_namespace":"a","source_domains":"0:0","mystery":"x"})JSON",
                           npm::NpmBasicTaskConfigError::kUnknownField, "mystery");
    ParseTaskConfigFailure(R"JSON({"input_namespace":7,"source_domains":"0:0"})JSON",
                           npm::NpmBasicTaskConfigError::kNonStringValue, "input_namespace");
    ParseTaskConfigFailure(R"JSON({"source_domains":7})JSON", npm::NpmBasicTaskConfigError::kNonStringValue,
                           "source_domains");

    ParseTaskConfigFailure(R"JSON({"input_namespace":"a","source_domains":"0:0","run_mode":"batch"})JSON",
                           npm::NpmBasicTaskConfigError::kInvalidEnum, "run_mode");
    ParseTaskConfigFailure(R"JSON({"input_namespace":"a","source_domains":"0:0","result_mode":"latest"})JSON",
                           npm::NpmBasicTaskConfigError::kInvalidEnum, "result_mode");
    ParseTaskConfigFailure(R"JSON({"input_namespace":"a","source_domains":"0:0","overload_policy":"drop"})JSON",
                           npm::NpmBasicTaskConfigError::kInvalidEnum, "overload_policy");
    ParseTaskConfigFailure(R"JSON({"input_namespace":"a","source_domains":"0:0","output_interval_ns":"-1"})JSON",
                           npm::NpmBasicTaskConfigError::kInvalidInteger, "output_interval_ns");
    ParseTaskConfigFailure(
        R"JSON({"input_namespace":"a","source_domains":"0:0","payload_sample_packets":"4294967296"})JSON",
        npm::NpmBasicTaskConfigError::kInvalidInteger, "payload_sample_packets");
    ParseTaskConfigFailure(
        R"JSON({"input_namespace":"a","source_domains":"0:0","max_tracked_bytes":"18446744073709551616"})JSON",
        npm::NpmBasicTaskConfigError::kInvalidInteger, "max_tracked_bytes");
}

void TestNpmBasicTaskConfigRejectsMappingsAndRanges() {
    const std::vector<std::string> invalid_domains = {
        "", "0", ":1", "1:", "1:2;", "1::2", " 1:2", "1:2 ", "+1:2", "1:-2", "4294967296:1", "1:18446744073709551616",
    };
    for (const auto& domains : invalid_domains) {
        const std::string json = R"JSON({"input_namespace":"a","source_domains":")JSON" + domains + R"JSON("})JSON";
        ParseTaskConfigFailure(json.c_str(), npm::NpmBasicTaskConfigError::kInvalidSourceDomains, "source_domains");
    }

    auto status = ParseTaskConfigFailure(R"JSON({"input_namespace":"a","source_domains":"1:2;1:3"})JSON",
                                         npm::NpmBasicTaskConfigError::kDomainValidationError, "source_domains");
    assert(status.domain_error == npm::NpmObservationDomainError::kDuplicateSourceId);

    status = ParseTaskConfigFailure(R"JSON({"input_namespace":"","source_domains":"1:2"})JSON",
                                    npm::NpmBasicTaskConfigError::kDomainValidationError, "input_namespace");
    assert(status.domain_error == npm::NpmObservationDomainError::kEmptyInputNamespace);

    status =
        ParseTaskConfigFailure(R"JSON({"input_namespace":"a","source_domains":"1:2","output_interval_ns":"1"})JSON",
                               npm::NpmBasicTaskConfigError::kAnalysisValidationError, "output_interval_ns");
    assert(status.analysis_error == npm::NpmAnalysisConfigError::kOutputIntervalOutOfRange);
    status =
        ParseTaskConfigFailure(R"JSON({"input_namespace":"a","source_domains":"1:2","max_active_sessions":"0"})JSON",
                               npm::NpmBasicTaskConfigError::kAnalysisValidationError, "max_active_sessions");
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
        return std::string(R"JSON({"input_namespace":"a","source_domains":"0:0",")JSON") + field + R"JSON(":")JSON" +
               value + R"JSON("})JSON";
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
            ParseTaskConfigFailure(json.c_str(), npm::NpmBasicTaskConfigError::kInvalidInteger, boundary.field);
        }
    }
    const auto json = make_json("tcp_idle_timeout_ns", "9223372036854775808");
    ParseTaskConfigFailure(json.c_str(), npm::NpmBasicTaskConfigError::kInvalidInteger, "tcp_idle_timeout_ns");
}

void TestNpmBasicTaskConfigFeatureSelection() {
    static_assert(npm::kNpmMinSessionTcpRangesPerDirection == 8);
    static_assert(npm::kNpmDefaultSessionTcpRangesPerDirection == 1024);
    static_assert(npm::kNpmMaxSessionTcpRangesPerDirection == 65536);
    static_assert(npm::kNpmMinLabelingMemoryMiB == 8);
    static_assert(npm::kNpmDefaultLabelingMemoryMiB == 64);
    static_assert(npm::kNpmMaxLabelingMemoryMiB == 256);

    npm::NpmBasicTaskConfig output;
    auto status =
        npm::ParseNpmBasicTaskConfig(R"JSON({"input_namespace":"a","source_domains":"0:0","features":"session",)JSON"
                                     R"JSON("observing":"session","session_max_tcp_ranges_per_direction":"8"})JSON",
                                     &output);
    assert(status.error == npm::NpmBasicTaskConfigError::kNone);
    assert(!output.features.basic_enabled && output.features.session_enabled);
    assert(output.features.observing == npm::NpmResultEntity::kSession);
    assert(output.features.session_max_tcp_ranges_per_direction == npm::kNpmMinSessionTcpRangesPerDirection);

    status = npm::ParseNpmBasicTaskConfig(
        R"JSON({"input_namespace":"a","source_domains":"0:0","features":"basic,session",)JSON"
        R"JSON("observing":"basic","session_max_tcp_ranges_per_direction":"65536"})JSON",
        &output);
    assert(status.error == npm::NpmBasicTaskConfigError::kNone);
    assert(output.features.basic_enabled && output.features.session_enabled);
    assert(output.features.observing == npm::NpmResultEntity::kBasic);
    assert(output.features.session_max_tcp_ranges_per_direction == npm::kNpmMaxSessionTcpRangesPerDirection);

    status = npm::ParseNpmBasicTaskConfig(
        R"JSON({"input_namespace":"a","source_domains":"0:0","features":" session , basic ",)JSON"
        R"JSON("observing":" session "})JSON",
        &output);
    assert(status.error == npm::NpmBasicTaskConfigError::kNone);
    assert(output.features.basic_enabled && output.features.session_enabled);
    assert(output.features.observing == npm::NpmResultEntity::kSession);
    assert(output.features.session_max_tcp_ranges_per_direction == npm::kNpmDefaultSessionTcpRangesPerDirection);

    const char* labeling_json = R"JSON({"input_namespace":"a","source_domains":"0:0","features":"basic,labeling",)JSON"
                                R"JSON("parameters":"{\"schema_version\":1,\"core\":{)JSON"
                                R"JSON(\"labeling\":\"config.corp-labels@7\"}}"})JSON";
    status = npm::ParseNpmBasicTaskConfig(labeling_json, &output, true);
    assert(status.error == npm::NpmBasicTaskConfigError::kNone);
    assert(output.features.basic_enabled && !output.features.session_enabled && output.features.labeling_enabled);
    assert(output.labeling_reference == "config.corp-labels@7");
    assert(output.labeling_memory_mib == npm::kNpmDefaultLabelingMemoryMiB);

    const char* sized_labeling_json =
        R"JSON({"input_namespace":"a","source_domains":"0:0","features":"basic,labeling",)JSON"
        R"JSON("parameters":"{\"schema_version\":1,\"core\":{)JSON"
        R"JSON(\"labeling\":\"config.corp-labels@7\",\"labeling_memory_mib\":96}}"})JSON";
    status = npm::ParseNpmBasicTaskConfig(sized_labeling_json, &output, true);
    assert(status.error == npm::NpmBasicTaskConfigError::kNone);
    assert(output.labeling_reference == "config.corp-labels@7");
    assert(output.labeling_memory_mib == 96);

    status = npm::ParseNpmBasicTaskConfig(labeling_json, &output, true);
    assert(status.error == npm::NpmBasicTaskConfigError::kNone);
    assert(output.labeling_reference == "config.corp-labels@7");
    assert(output.labeling_memory_mib == npm::kNpmDefaultLabelingMemoryMiB);

    status = npm::ParseNpmBasicTaskConfig(labeling_json, &output);
    assert(status.error == npm::NpmBasicTaskConfigError::kInvalidParameters);
    assert(status.parameter_status.error == npm::NpmParameterErrorV1::kUnavailableFeature);
    assert(status.parameter_status.path == "/labeling");

    const char* invalid_features[] = {
        R"JSON({"input_namespace":"a","source_domains":"0:0","features":""})JSON",
        R"JSON({"input_namespace":"a","source_domains":"0:0","features":"basic,,session"})JSON",
        R"JSON({"input_namespace":"a","source_domains":"0:0","features":"basic,basic"})JSON",
        R"JSON({"input_namespace":"a","source_domains":"0:0","features":"basic,labeling,labeling"})JSON",
        R"JSON({"input_namespace":"a","source_domains":"0:0","features":"labeling"})JSON",
    };
    for (const char* json : invalid_features) {
        ParseTaskConfigFailure(json, npm::NpmBasicTaskConfigError::kInvalidFeatures, "features");
    }

    ParseTaskConfigFailure(R"JSON({"input_namespace":"a","source_domains":"0:0","observing":"session"})JSON",
                           npm::NpmBasicTaskConfigError::kObservingFeatureDisabled, "observing");
    ParseTaskConfigFailure(R"JSON({"input_namespace":"a","source_domains":"0:0","features":"session"})JSON",
                           npm::NpmBasicTaskConfigError::kObservingFeatureDisabled, "observing");
    ParseTaskConfigFailure(
        R"JSON({"input_namespace":"a","source_domains":"0:0","features":"basic","observing":"session"})JSON",
        npm::NpmBasicTaskConfigError::kObservingFeatureDisabled, "observing");
    ParseTaskConfigFailure(R"JSON({"input_namespace":"a","source_domains":"0:0","observing":"basic,session"})JSON",
                           npm::NpmBasicTaskConfigError::kInvalidObserving, "observing");
    ParseTaskConfigFailure(R"JSON({"input_namespace":"a","source_domains":"0:0","observing":"dns"})JSON",
                           npm::NpmBasicTaskConfigError::kInvalidObserving, "observing");
    ParseTaskConfigFailure(
        R"JSON({"input_namespace":"a","source_domains":"0:0","session_max_tcp_ranges_per_direction":"1024"})JSON",
        npm::NpmBasicTaskConfigError::kSessionConfigWithoutFeature, "session_max_tcp_ranges_per_direction");
    ParseTaskConfigFailure(R"JSON({"input_namespace":"a","source_domains":"0:0","features":"session",)JSON"
                           R"JSON("observing":"session","session_max_tcp_ranges_per_direction":"x"})JSON",
                           npm::NpmBasicTaskConfigError::kInvalidInteger, "session_max_tcp_ranges_per_direction");
    ParseTaskConfigFailure(R"JSON({"input_namespace":"a","source_domains":"0:0","features":"session",)JSON"
                           R"JSON("observing":"session","session_max_tcp_ranges_per_direction":"7"})JSON",
                           npm::NpmBasicTaskConfigError::kSessionTcpRangesOutOfRange,
                           "session_max_tcp_ranges_per_direction");
    ParseTaskConfigFailure(R"JSON({"input_namespace":"a","source_domains":"0:0","features":"session",)JSON"
                           R"JSON("observing":"session","session_max_tcp_ranges_per_direction":"65537"})JSON",
                           npm::NpmBasicTaskConfigError::kSessionTcpRangesOutOfRange,
                           "session_max_tcp_ranges_per_direction");
}

void AssertEquivalentTaskConfig(const npm::NpmBasicTaskConfig& left, const npm::NpmBasicTaskConfig& right) {
    assert(left.analysis.run_mode == right.analysis.run_mode);
    assert(left.analysis.result_mode == right.analysis.result_mode);
    assert(left.analysis.overload_policy == right.analysis.overload_policy);
    assert(left.analysis.output_interval_ns == right.analysis.output_interval_ns);
    assert(left.analysis.payload_sample_packets == right.analysis.payload_sample_packets);
    assert(left.analysis.tcp_idle_timeout_ns == right.analysis.tcp_idle_timeout_ns);
    assert(left.analysis.udp_idle_timeout_ns == right.analysis.udp_idle_timeout_ns);
    assert(left.analysis.out_of_order_tolerance_ns == right.analysis.out_of_order_tolerance_ns);
    assert(left.analysis.max_active_sessions == right.analysis.max_active_sessions);
    assert(left.analysis.max_tracked_bytes == right.analysis.max_tracked_bytes);
    assert(left.analysis.max_pending_output_bytes == right.analysis.max_pending_output_bytes);
    assert(left.features.basic_enabled == right.features.basic_enabled);
    assert(left.features.session_enabled == right.features.session_enabled);
    assert(left.features.labeling_enabled == right.features.labeling_enabled);
    assert(left.labeling_reference == right.labeling_reference);
    assert(left.labeling_memory_mib == right.labeling_memory_mib);
    assert(left.features.observing == right.features.observing);
    assert(left.features.session_max_tcp_ranges_per_direction == right.features.session_max_tcp_ranges_per_direction);
    assert(left.domains.input_namespace == right.domains.input_namespace);
    assert(left.domains.bindings.size() == right.domains.bindings.size());
    for (std::size_t index = 0; index < left.domains.bindings.size(); ++index) {
        assert(left.domains.bindings[index].source_id == right.domains.bindings[index].source_id);
        assert(left.domains.bindings[index].observation_domain_id ==
               right.domains.bindings[index].observation_domain_id);
    }
}

struct EquivalentRuntimeTaskConfigs {
    std::string legacy_json;
    std::string parameters_v1_json;
    npm::NpmBasicTaskConfig legacy;
    npm::NpmBasicTaskConfig parameters_v1;
};

EquivalentRuntimeTaskConfigs MakeEquivalentRuntimeTaskConfigs(npm::NpmRunMode run_mode, bool observing_session) {
    const char* feature_fields = observing_session ? R"JSON(,"features":"session","observing":"session")JSON" : "";
    const char* legacy_framework = run_mode == npm::NpmRunMode::kRealtime
                                       ? R"JSON(,"run_mode":"realtime",)JSON"
                                         R"JSON("result_mode":"periodic_snapshot",)JSON"
                                         R"JSON("output_interval_ns":"10000000",)JSON"
                                         R"JSON("payload_sample_packets":"4",)JSON"
                                         R"JSON("tcp_idle_timeout_ns":"1000000000",)JSON"
                                         R"JSON("udp_idle_timeout_ns":"1000000000",)JSON"
                                         R"JSON("out_of_order_tolerance_ns":"0",)JSON"
                                         R"JSON("max_active_sessions":"32",)JSON"
                                         R"JSON("max_tracked_bytes":"1048576",)JSON"
                                         R"JSON("max_pending_output_bytes":"1048576")JSON"
                                       : R"JSON(,"run_mode":"offline",)JSON"
                                         R"JSON("result_mode":"final",)JSON"
                                         R"JSON("output_interval_ns":"10000000",)JSON"
                                         R"JSON("payload_sample_packets":"4",)JSON"
                                         R"JSON("tcp_idle_timeout_ns":"1000000000",)JSON"
                                         R"JSON("udp_idle_timeout_ns":"1000000000",)JSON"
                                         R"JSON("out_of_order_tolerance_ns":"0",)JSON"
                                         R"JSON("max_active_sessions":"32",)JSON"
                                         R"JSON("max_tracked_bytes":"1048576",)JSON"
                                         R"JSON("max_pending_output_bytes":"1048576")JSON";
    const char* parameters_core =
        run_mode == npm::NpmRunMode::kRealtime
            ? R"JSON(,"parameters":"{\"schema_version\":1,\"core\":{)JSON"
              R"JSON(\"run_mode\":\"realtime\",\"result_mode\":\"periodic_snapshot\",)JSON"
              R"JSON(\"output_interval_ns\":10000000,\"payload_sample_packets\":4,)JSON"
              R"JSON(\"tcp_idle_timeout_ns\":1000000000,\"udp_idle_timeout_ns\":1000000000,)JSON"
              R"JSON(\"out_of_order_tolerance_ns\":0,\"max_active_sessions\":32,)JSON"
              R"JSON(\"max_tracked_bytes\":1048576,\"max_pending_output_bytes\":1048576})JSON"
            : R"JSON(,"parameters":"{\"schema_version\":1,\"core\":{)JSON"
              R"JSON(\"run_mode\":\"offline\",\"result_mode\":\"final\",)JSON"
              R"JSON(\"output_interval_ns\":10000000,\"payload_sample_packets\":4,)JSON"
              R"JSON(\"tcp_idle_timeout_ns\":1000000000,\"udp_idle_timeout_ns\":1000000000,)JSON"
              R"JSON(\"out_of_order_tolerance_ns\":0,\"max_active_sessions\":32,)JSON"
              R"JSON(\"max_tracked_bytes\":1048576,\"max_pending_output_bytes\":1048576})JSON";

    EquivalentRuntimeTaskConfigs configs;
    configs.legacy_json = std::string(R"JSON({"input_namespace":"pcapfile.capture","source_domains":"0:77")JSON") +
                          feature_fields + legacy_framework;
    configs.parameters_v1_json =
        std::string(R"JSON({"input_namespace":"pcapfile.capture","source_domains":"0:77")JSON") + feature_fields +
        parameters_core;
    if (observing_session) {
        configs.legacy_json += R"JSON(,"session_max_tcp_ranges_per_direction":"8")JSON";
        configs.parameters_v1_json += R"JSON(,\"session\":{\"max_tcp_ranges_per_direction\":8})JSON";
    }
    configs.legacy_json += "}";
    configs.parameters_v1_json += R"JSON(}"})JSON";

    const auto legacy_status = npm::ParseNpmBasicTaskConfig(configs.legacy_json.c_str(), &configs.legacy);
    const auto parameters_status =
        npm::ParseNpmBasicTaskConfig(configs.parameters_v1_json.c_str(), &configs.parameters_v1);
    assert(legacy_status.error == npm::NpmBasicTaskConfigError::kNone);
    assert(parameters_status.error == npm::NpmBasicTaskConfigError::kNone);
    AssertEquivalentTaskConfig(configs.legacy, configs.parameters_v1);
    return configs;
}

void TestNpmBasicTaskConfigNormalizesParametersV1() {
    static_assert(std::is_same_v<decltype(npm::NpmBasicTaskConfigStatus::parameter_status), npm::NpmParameterStatusV1>);

    std::string v1_json = R"JSON({
        "input_namespace":"pcapfile.capture",
        "source_domains":"0:7;1:8",
        "features":"basic,session",
        "observing":"session",
        "parameters":"{\"schema_version\":1,\"core\":{\"run_mode\":\"realtime\",)JSON"
                          R"JSON(\"result_mode\":\"final\",\"overload_policy\":\"fail\",)JSON"
                          R"JSON(\"output_interval_ns\":10000000,\"payload_sample_packets\":64,)JSON"
                          R"JSON(\"tcp_idle_timeout_ns\":1000000000,)JSON"
                          R"JSON(\"udp_idle_timeout_ns\":86400000000000,)JSON"
                          R"JSON(\"out_of_order_tolerance_ns\":60000000000,)JSON"
                          R"JSON(\"max_active_sessions\":10000000,)JSON"
                          R"JSON(\"max_tracked_bytes\":1099511627776,)JSON"
                          R"JSON(\"max_pending_output_bytes\":1099511627776},)JSON"
                          R"JSON(\"basic\":{},\"session\":{\"max_tcp_ranges_per_direction\":8},)JSON"
                          R"JSON(\"future\":{\"opaque\":true}}"
    })JSON";
    npm::NpmBasicTaskConfig v1;
    auto status = npm::ParseNpmBasicTaskConfig(v1_json.c_str(), &v1);
    assert(status.error == npm::NpmBasicTaskConfigError::kNone);
    assert(status.parameter_status.error == npm::NpmParameterErrorV1::kNone);
    assert(v1.analysis.run_mode == npm::NpmRunMode::kRealtime);
    assert(v1.analysis.result_mode == npm::NpmResultMode::kFinal);
    assert(v1.analysis.output_interval_ns == npm::kNpmMinOutputIntervalNs);
    assert(v1.analysis.payload_sample_packets == npm::kNpmMaxPayloadSamplePackets);
    assert(v1.analysis.tcp_idle_timeout_ns == npm::kNpmMinIdleTimeoutNs);
    assert(v1.analysis.udp_idle_timeout_ns == npm::kNpmMaxIdleTimeoutNs);
    assert(v1.analysis.out_of_order_tolerance_ns == npm::kNpmMaxOutOfOrderToleranceNs);
    assert(v1.analysis.max_active_sessions == npm::kNpmMaxActiveSessions);
    assert(v1.analysis.max_tracked_bytes == npm::kNpmMaxTrackedBytes);
    assert(v1.analysis.max_pending_output_bytes == npm::kNpmMaxPendingOutputBytes);
    assert(v1.features.basic_enabled && v1.features.session_enabled);
    assert(v1.features.observing == npm::NpmResultEntity::kSession);
    assert(v1.features.session_max_tcp_ranges_per_direction == npm::kNpmMinSessionTcpRangesPerDirection);
    assert(v1.domains.input_namespace == "pcapfile.capture");
    assert(v1.domains.bindings.size() == 2);

    const char* legacy_json = R"JSON({
        "input_namespace":"pcapfile.capture",
        "source_domains":"0:7;1:8",
        "features":"basic,session",
        "observing":"session",
        "run_mode":"realtime",
        "result_mode":"final",
        "overload_policy":"fail",
        "output_interval_ns":"10000000",
        "payload_sample_packets":"64",
        "tcp_idle_timeout_ns":"1000000000",
        "udp_idle_timeout_ns":"86400000000000",
        "out_of_order_tolerance_ns":"60000000000",
        "max_active_sessions":"10000000",
        "max_tracked_bytes":"1099511627776",
        "max_pending_output_bytes":"1099511627776",
        "session_max_tcp_ranges_per_direction":"8"
    })JSON";
    npm::NpmBasicTaskConfig legacy;
    status = npm::ParseNpmBasicTaskConfig(legacy_json, &legacy);
    assert(status.error == npm::NpmBasicTaskConfigError::kNone);
    AssertEquivalentTaskConfig(v1, legacy);

    v1_json.assign(v1_json.size(), 'x');
    assert(v1.domains.input_namespace == "pcapfile.capture");
    assert(v1.domains.bindings[1].observation_domain_id == 8);
    assert(v1.features.session_max_tcp_ranges_per_direction == npm::kNpmMinSessionTcpRangesPerDirection);

    npm::NpmBasicTaskConfig basic_only;
    status =
        npm::ParseNpmBasicTaskConfig(R"JSON({"input_namespace":"a","source_domains":"0:0",)JSON"
                                     R"JSON("parameters":"{\"schema_version\":1,\"session\":{\"unknown\":null},)JSON"
                                     R"JSON(\"future\":{\"anything\":[1,2]}}"})JSON",
                                     &basic_only);
    assert(status.error == npm::NpmBasicTaskConfigError::kNone);
    assert(basic_only.features.basic_enabled && !basic_only.features.session_enabled);
    assert(basic_only.features.session_max_tcp_ranges_per_direction == npm::kNpmDefaultSessionTcpRangesPerDirection);

    npm::NpmBasicTaskConfig session_only;
    status =
        npm::ParseNpmBasicTaskConfig(R"JSON({"input_namespace":"a","source_domains":"0:0","features":"session",)JSON"
                                     R"JSON("observing":"session",)JSON"
                                     R"JSON("parameters":"{\"schema_version\":1,\"basic\":{\"unknown\":1}}"})JSON",
                                     &session_only);
    assert(status.error == npm::NpmBasicTaskConfigError::kNone);
    assert(!session_only.features.basic_enabled && session_only.features.session_enabled);
    assert(session_only.features.session_max_tcp_ranges_per_direction == npm::kNpmDefaultSessionTcpRangesPerDirection);
}

void TestNpmPeriodicConfigurationParsing() {
    for (const auto period : {15000000000LL, 30000000000LL, 60000000000LL}) {
        for (const bool nested : {false, true}) {
            npm::NpmBasicTaskConfig config;
            const std::string json =
                nested ? "{\"parameters\":\"{\\\"schema_version\\\":1,\\\"core\\\":{\\\"output_interval_ns\\\":" +
                             std::to_string(period) + "}}\"}"
                       : "{\"output_interval_ns\":\"" + std::to_string(period) + "\"}";
            assert(npm::ParseNpmBasicTaskConfig(json.c_str(), &config).error == npm::NpmBasicTaskConfigError::kNone);
            assert(config.analysis.output_interval_ns == period);
            assert(config.analysis.result_mode == npm::NpmResultMode::kPeriodicSnapshot && !config.run_mode_explicit);
        }
    }
    for (const auto mode : {"offline", "realtime"}) {
        for (const bool nested : {false, true}) {
            npm::NpmBasicTaskConfig config;
            const std::string json =
                nested ? "{\"parameters\":\"{\\\"schema_version\\\":1,\\\"core\\\":{\\\"run_mode\\\":\\\"" +
                             std::string(mode) + "\\\"}}\"}"
                       : "{\"run_mode\":\"" + std::string(mode) + "\"}";
            assert(npm::ParseNpmBasicTaskConfig(json.c_str(), &config).error == npm::NpmBasicTaskConfigError::kNone);
            assert(config.run_mode_explicit && config.analysis.output_interval_ns == 30000000000LL);
            assert(config.analysis.result_mode == npm::NpmResultMode::kPeriodicSnapshot);
            assert((config.analysis.run_mode == npm::NpmRunMode::kRealtime) == (std::string_view(mode) == "realtime"));
        }
    }
    npm::NpmBasicTaskConfig config;
    assert(npm::ParseNpmBasicTaskConfig("{}", &config).error == npm::NpmBasicTaskConfigError::kNone);
    assert(config.analysis.output_interval_ns == 30000000000LL && !config.run_mode_explicit);
}

void TestNpmBasicTaskConfigRejectsParameterSourceConflicts() {
    static constexpr const char* kLegacyTuningFields[] = {
        "run_mode",
        "result_mode",
        "overload_policy",
        "output_interval_ns",
        "payload_sample_packets",
        "tcp_idle_timeout_ns",
        "udp_idle_timeout_ns",
        "out_of_order_tolerance_ns",
        "max_active_sessions",
        "max_tracked_bytes",
        "max_pending_output_bytes",
        "session_max_tcp_ranges_per_direction",
    };
    for (const char* field : kLegacyTuningFields) {
        const std::string json = std::string(R"JSON({"input_namespace":"a","source_domains":"0:0",)JSON") +
                                 R"JSON("parameters":"{\"schema_version\":1}",")JSON" + field +
                                 R"JSON(":"ignored"})JSON";
        const std::string path = std::string("/") + field;
        ParseTaskConfigFailure(json.c_str(), npm::NpmBasicTaskConfigError::kParameterSourceConflict, field,
                               npm::NpmParameterErrorV1::kLegacyConflict, path.c_str());
    }

    npm::NpmBasicTaskConfig output;
    const auto status =
        npm::ParseNpmBasicTaskConfig(R"JSON({"input_namespace":"a","source_domains":"0:0","features":"session",)JSON"
                                     R"JSON("observing":"session","parameters":"{\"schema_version\":1}"})JSON",
                                     &output);
    assert(status.error == npm::NpmBasicTaskConfigError::kNone);
    assert(!output.features.basic_enabled && output.features.session_enabled);
}

void TestNpmBasicTaskConfigPreservesParameterFailuresAtomically() {
    ParseTaskConfigFailure(R"JSON({"input_namespace":"a","source_domains":"0:0","parameters":""})JSON",
                           npm::NpmBasicTaskConfigError::kInvalidParameters, "parameters",
                           npm::NpmParameterErrorV1::kEmptyInput);
    ParseTaskConfigFailure(R"JSON({"input_namespace":"a","source_domains":"0:0",)JSON"
                           R"JSON("parameters":"{\"schema_version\":1,\"core\":{)JSON"
                           R"JSON(\"max_active_sessions\":\"1\"}}"})JSON",
                           npm::NpmBasicTaskConfigError::kInvalidParameters, "parameters",
                           npm::NpmParameterErrorV1::kInvalidType, "/core/max_active_sessions");
    ParseTaskConfigFailure(R"JSON({"input_namespace":"a","source_domains":"0:0","features":"session",)JSON"
                           R"JSON("observing":"session",)JSON"
                           R"JSON("parameters":"{\"schema_version\":1,\"session\":{)JSON"
                           R"JSON(\"max_tcp_ranges_per_direction\":7}}"})JSON",
                           npm::NpmBasicTaskConfigError::kInvalidParameters, "parameters",
                           npm::NpmParameterErrorV1::kInvalidRange, "/session/max_tcp_ranges_per_direction");
    ParseTaskConfigFailure(R"JSON({"input_namespace":"a","source_domains":"0:0","parameters":{}})JSON",
                           npm::NpmBasicTaskConfigError::kNonStringValue, "parameters");
    ParseTaskConfigFailure(R"JSON({"input_namespace":"a","source_domains":"0:0",)JSON"
                           R"JSON("parameters":"{\"schema_version\":1}\u0000trailing"})JSON",
                           npm::NpmBasicTaskConfigError::kInvalidParameters, "parameters",
                           npm::NpmParameterErrorV1::kInvalidJson);

    std::string oversized = R"JSON({"input_namespace":"a","source_domains":"0:0","parameters":")JSON";
    oversized.append(npm::kNpmParametersMaxJsonBytesV1 + 1, 'x');
    oversized += R"JSON("})JSON";
    ParseTaskConfigFailure(oversized.c_str(), npm::NpmBasicTaskConfigError::kInvalidParameters, "parameters",
                           npm::NpmParameterErrorV1::kJsonTooLarge);
}

npm::NpmParameterStatusV1 ParseParametersFailure(const char* json, const npm::NpmParameterConsumersV1& consumers,
                                                 npm::NpmParameterErrorV1 expected_error,
                                                 const char* expected_path = "") {
    npm::NpmTaskParametersV1 output;
    output.schema_version = 99;
    output.source = npm::NpmParameterSourceV1::kLegacyWith;
    output.core.analysis = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kRealtime);
    output.core.analysis.max_active_sessions = 123;
    output.core.labeling_reference = "sentinel";
    output.core.labeling_memory_mib = 8;
    output.core.tcp_stream = {131072, 12345};
    output.basic.reset();
    output.session = npm::NpmSessionModuleParametersV1{321};

    const auto status = npm::ParseNpmParametersV1(json, consumers, &output);
    assert(status.error == expected_error);
    assert(status.path == expected_path);
    assert(output.schema_version == 99);
    assert(output.source == npm::NpmParameterSourceV1::kLegacyWith);
    assert(output.core.analysis.run_mode == npm::NpmRunMode::kRealtime);
    assert(output.core.analysis.max_active_sessions == 123);
    assert(output.core.labeling_reference == "sentinel");
    assert(output.core.labeling_memory_mib == 8);
    assert(output.core.tcp_stream.max_buffered_bytes_per_direction == 131072);
    assert(output.core.tcp_stream.gap_timeout_ns == 12345);
    assert(!output.basic.has_value());
    assert(output.session.has_value());
    assert(output.session->max_tcp_ranges_per_direction == 321);
    return status;
}

void AssertEquivalentParameters(const npm::NpmTaskParametersV1& left, const npm::NpmTaskParametersV1& right) {
    assert(left.schema_version == right.schema_version);
    assert(left.source == right.source);
    assert(left.core.analysis.run_mode == right.core.analysis.run_mode);
    assert(left.core.analysis.result_mode == right.core.analysis.result_mode);
    assert(left.core.analysis.output_interval_ns == right.core.analysis.output_interval_ns);
    assert(left.core.analysis.payload_sample_packets == right.core.analysis.payload_sample_packets);
    assert(left.core.analysis.tcp_idle_timeout_ns == right.core.analysis.tcp_idle_timeout_ns);
    assert(left.core.analysis.udp_idle_timeout_ns == right.core.analysis.udp_idle_timeout_ns);
    assert(left.core.analysis.out_of_order_tolerance_ns == right.core.analysis.out_of_order_tolerance_ns);
    assert(left.core.analysis.max_active_sessions == right.core.analysis.max_active_sessions);
    assert(left.core.analysis.max_tracked_bytes == right.core.analysis.max_tracked_bytes);
    assert(left.core.analysis.max_pending_output_bytes == right.core.analysis.max_pending_output_bytes);
    assert(left.core.analysis.overload_policy == right.core.analysis.overload_policy);
    assert(left.core.labeling_reference == right.core.labeling_reference);
    assert(left.core.labeling_memory_mib == right.core.labeling_memory_mib);
    assert(left.core.tcp_stream.max_buffered_bytes_per_direction ==
           right.core.tcp_stream.max_buffered_bytes_per_direction);
    assert(left.core.tcp_stream.gap_timeout_ns == right.core.tcp_stream.gap_timeout_ns);
    assert(left.basic.has_value() == right.basic.has_value());
    assert(left.session.has_value() == right.session.has_value());
    if (left.session) {
        assert(left.session->max_tcp_ranges_per_direction == right.session->max_tcp_ranges_per_direction);
    }
}

void TestNpmTcpStreamParameters() {
    for (const char* json : {R"({"schema_version":1})", R"({"schema_version":1,"core":{}})",
                             R"({"schema_version":1,"core":{"tcp_stream":{}}})"}) {
        npm::NpmTaskParametersV1 output;
        output.core.tcp_stream = {65536, 0};
        assert(npm::ParseNpmParametersV1(json, {}, &output).error == npm::NpmParameterErrorV1::kNone);
        assert(output.core.tcp_stream.max_buffered_bytes_per_direction == 1048576);
        assert(output.core.tcp_stream.gap_timeout_ns == 1000000000);
    }
    for (const auto bytes : {65536ULL, 131072ULL, 67108864ULL}) {
        for (const auto timeout : {0LL, 12345LL, 60000000000LL}) {
            std::string json = R"({"schema_version":1,"core":{"tcp_stream":{"max_buffered_bytes_per_direction":)" +
                               std::to_string(bytes) + R"(,"gap_timeout_ns":)" + std::to_string(timeout) + "}}}";
            npm::NpmTaskParametersV1 output;
            assert(npm::ParseNpmParametersV1(json.c_str(), {}, &output).error == npm::NpmParameterErrorV1::kNone);
            json.assign(json.size(), 'x');
            assert(output.core.tcp_stream.max_buffered_bytes_per_direction == bytes);
            assert(output.core.tcp_stream.gap_timeout_ns == timeout);
            assert(!output.session && !output.core.labeling_reference);
        }
    }
    using Error = npm::NpmParameterErrorV1;
    struct InvalidCase {
        const char* node;
        Error error;
        const char* suffix;
    };
    const InvalidCase invalid[] = {
        {"null", Error::kInvalidType, ""},
        {"[]", Error::kInvalidType, ""},
        {"1", Error::kInvalidType, ""},
        {R"({"unknown":1})", Error::kUnknownConsumedField, "/unknown"},
        {R"({"bad/~":1})", Error::kUnknownConsumedField, "/bad~1~0"},
        {R"({"gap_timeout_ns":0,"gap_timeout_ns":1})", Error::kDuplicateField, "/gap_timeout_ns"},
        {R"({"max_buffered_bytes_per_direction":65535})", Error::kInvalidRange, "/max_buffered_bytes_per_direction"},
        {R"({"max_buffered_bytes_per_direction":67108865})", Error::kInvalidRange, "/max_buffered_bytes_per_direction"},
        {R"({"max_buffered_bytes_per_direction":18446744073709551615})", Error::kInvalidRange,
         "/max_buffered_bytes_per_direction"},
        {R"({"max_buffered_bytes_per_direction":-1})", Error::kInvalidType, "/max_buffered_bytes_per_direction"},
        {R"({"max_buffered_bytes_per_direction":"65536"})", Error::kInvalidType, "/max_buffered_bytes_per_direction"},
        {R"({"max_buffered_bytes_per_direction":null})", Error::kInvalidType, "/max_buffered_bytes_per_direction"},
        {R"({"max_buffered_bytes_per_direction":65536.0})", Error::kInvalidType, "/max_buffered_bytes_per_direction"},
        {R"({"max_buffered_bytes_per_direction":true})", Error::kInvalidType, "/max_buffered_bytes_per_direction"},
        {R"({"gap_timeout_ns":-1})", Error::kInvalidRange, "/gap_timeout_ns"},
        {R"({"gap_timeout_ns":60000000001})", Error::kInvalidRange, "/gap_timeout_ns"},
        {R"({"gap_timeout_ns":18446744073709551615})", Error::kInvalidType, "/gap_timeout_ns"},
        {R"({"gap_timeout_ns":"0"})", Error::kInvalidType, "/gap_timeout_ns"},
        {R"({"gap_timeout_ns":null})", Error::kInvalidType, "/gap_timeout_ns"},
        {R"({"gap_timeout_ns":0.5})", Error::kInvalidType, "/gap_timeout_ns"},
        {R"({"gap_timeout_ns":false})", Error::kInvalidType, "/gap_timeout_ns"},
    };
    for (const auto& test : invalid) {
        const std::string json = std::string(R"({"schema_version":1,"core":{"tcp_stream":)") + test.node + "}}";
        const std::string path = std::string("/core/tcp_stream") + test.suffix;
        ParseParametersFailure(json.c_str(), {}, test.error, path.c_str());
    }
    ParseParametersFailure(R"({"schema_version":1,"core":{"tcp_stream":{},"tcp_stream":{}}})", {},
                           Error::kDuplicateField, "/core/tcp_stream");

    npm::NpmBasicTaskConfig config;
    config.tcp_stream = {131072, 12345};
    const auto status = npm::ParseNpmBasicTaskConfig(
        R"({"parameters":"{\"schema_version\":1,\"core\":{\"tcp_stream\":{\"gap_timeout_ns\":-1}}}"})", &config);
    assert(status.error == npm::NpmBasicTaskConfigError::kInvalidParameters);
    assert(status.parameter_status.error == Error::kInvalidRange);
    assert(status.parameter_status.path == "/core/tcp_stream/gap_timeout_ns");
    assert(config.tcp_stream.max_buffered_bytes_per_direction == 131072 && config.tcp_stream.gap_timeout_ns == 12345);
    assert(npm::ParseNpmBasicTaskConfig("{}", &config).error == npm::NpmBasicTaskConfigError::kNone);
    assert(config.tcp_stream.max_buffered_bytes_per_direction == 1048576 &&
           config.tcp_stream.gap_timeout_ns == 1000000000);
}

void TestNpmParametersV1OwnsCanonicalConfig() {
    static_assert(std::is_default_constructible_v<npm::NpmTaskParametersV1>);
    static_assert(std::is_same_v<decltype(npm::NpmParameterStatusV1::path), std::string>);
    static_assert(npm::kNpmParametersMaxJsonBytesV1 == 64 * 1024);
    static_assert(npm::kNpmParametersMaxDepthV1 == 64);

    npm::NpmTaskParametersV1 minimal;
    auto status = npm::ParseNpmParametersV1(R"JSON({"schema_version":1})JSON", {}, &minimal);
    assert(status.error == npm::NpmParameterErrorV1::kNone && status.path.empty());
    assert(minimal.schema_version == 1);
    assert(minimal.source == npm::NpmParameterSourceV1::kParametersV1);
    assert(minimal.core.analysis.run_mode == npm::NpmRunMode::kOffline);
    assert(minimal.core.analysis.result_mode == npm::NpmResultMode::kPeriodicSnapshot);
    assert(!minimal.core.labeling_reference.has_value());
    assert(!minimal.core.labeling_memory_mib.has_value());
    assert(minimal.basic.has_value() && !minimal.session.has_value());

    npm::NpmParameterConsumersV1 consumers;
    consumers.session_enabled = true;
    std::string json = R"JSON({
        "future":{"opaque":null,"array":[1,{"nested":true}]},
        "session":{"max_tcp_ranges_per_direction":65536},
        "basic":{},
        "core":{
            "max_pending_output_bytes":1099511627776,
            "max_tracked_bytes":1099511627776,
            "max_active_sessions":10000000,
            "out_of_order_tolerance_ns":60000000000,
            "udp_idle_timeout_ns":86400000000000,
            "tcp_idle_timeout_ns":1000000000,
            "payload_sample_packets":64,
            "output_interval_ns":10000000,
            "overload_policy":"fail",
            "result_mode":"final",
            "run_mode":"realtime",
            "labeling":"not-an-exact-reference"
        },
        "schema_version":1
    })JSON";
    npm::NpmTaskParametersV1 first;
    status = npm::ParseNpmParametersV1(json.c_str(), consumers, &first);
    assert(status.error == npm::NpmParameterErrorV1::kNone);
    assert(first.core.analysis.run_mode == npm::NpmRunMode::kRealtime);
    assert(first.core.analysis.result_mode == npm::NpmResultMode::kFinal);
    assert(first.core.analysis.output_interval_ns == npm::kNpmMinOutputIntervalNs);
    assert(first.core.analysis.payload_sample_packets == npm::kNpmMaxPayloadSamplePackets);
    assert(first.core.analysis.tcp_idle_timeout_ns == npm::kNpmMinIdleTimeoutNs);
    assert(first.core.analysis.udp_idle_timeout_ns == npm::kNpmMaxIdleTimeoutNs);
    assert(first.core.analysis.out_of_order_tolerance_ns == npm::kNpmMaxOutOfOrderToleranceNs);
    assert(first.core.analysis.max_active_sessions == npm::kNpmMaxActiveSessions);
    assert(first.core.analysis.max_tracked_bytes == npm::kNpmMaxTrackedBytes);
    assert(first.core.analysis.max_pending_output_bytes == npm::kNpmMaxPendingOutputBytes);
    assert(!first.core.labeling_reference.has_value());
    assert(!first.core.labeling_memory_mib.has_value());
    assert(first.basic.has_value() && first.session.has_value());
    assert(first.session->max_tcp_ranges_per_direction == npm::kNpmMaxSessionTcpRangesPerDirection);

    const char* reordered = R"JSON({
        "schema_version":1,
        "core":{"labeling":"ignored","run_mode":"realtime","result_mode":"final",
            "overload_policy":"fail","output_interval_ns":10000000,"payload_sample_packets":64,
            "tcp_idle_timeout_ns":1000000000,"udp_idle_timeout_ns":86400000000000,
            "out_of_order_tolerance_ns":60000000000,"max_active_sessions":10000000,
            "max_tracked_bytes":1099511627776,"max_pending_output_bytes":1099511627776},
        "basic":{},"session":{"max_tcp_ranges_per_direction":65536},
        "future":{"array":[1,{"nested":true}],"opaque":null}
    })JSON";
    npm::NpmTaskParametersV1 second;
    status = npm::ParseNpmParametersV1(reordered, consumers, &second);
    assert(status.error == npm::NpmParameterErrorV1::kNone);
    AssertEquivalentParameters(first, second);

    json.assign(json.size(), 'x');
    assert(first.session->max_tcp_ranges_per_direction == npm::kNpmMaxSessionTcpRangesPerDirection);
}

void TestNpmParametersV1RejectsEnvelopeAndDuplicatesAtomically() {
    const npm::NpmParameterConsumersV1 consumers;
    ParseParametersFailure(nullptr, consumers, npm::NpmParameterErrorV1::kNullInput);
    ParseParametersFailure("", consumers, npm::NpmParameterErrorV1::kEmptyInput);
    auto status = npm::ParseNpmParametersV1("{}", consumers, nullptr);
    assert(status.error == npm::NpmParameterErrorV1::kNullOutput && status.path.empty());
    ParseParametersFailure("{", consumers, npm::NpmParameterErrorV1::kInvalidJson);
    ParseParametersFailure("[]", consumers, npm::NpmParameterErrorV1::kInvalidEnvelope);
    ParseParametersFailure("{}", consumers, npm::NpmParameterErrorV1::kMissingSchemaVersion, "/schema_version");
    ParseParametersFailure(R"JSON({"schema_version":"1"})JSON", consumers, npm::NpmParameterErrorV1::kInvalidType,
                           "/schema_version");
    ParseParametersFailure(R"JSON({"schema_version":2})JSON", consumers, npm::NpmParameterErrorV1::kUnsupportedVersion,
                           "/schema_version");
    ParseParametersFailure(R"JSON({"schema_version":1,"schema_version":1})JSON", consumers,
                           npm::NpmParameterErrorV1::kDuplicateField, "/schema_version");
    ParseParametersFailure(R"JSON({"schema_version":1,"core":{"run_mode":"offline","run_mode":"realtime"}})JSON",
                           consumers, npm::NpmParameterErrorV1::kDuplicateField, "/core/run_mode");
    ParseParametersFailure(R"JSON({"schema_version":1,"future":{"nested":{"value":1,"value":2}}})JSON", consumers,
                           npm::NpmParameterErrorV1::kDuplicateField, "/future/nested/value");
    ParseParametersFailure(R"JSON({"schema_version":1,"":7})JSON", consumers, npm::NpmParameterErrorV1::kInvalidType,
                           "/");

    const std::string size_prefix = R"JSON({"schema_version":1,"future":{"padding":")JSON";
    const std::string size_suffix = R"JSON("}})JSON";
    std::string maximum_size = size_prefix;
    maximum_size.append(npm::kNpmParametersMaxJsonBytesV1 - size_prefix.size() - size_suffix.size(), 'x');
    maximum_size += size_suffix;
    npm::NpmTaskParametersV1 maximum_size_output;
    status = npm::ParseNpmParametersV1(maximum_size.c_str(), consumers, &maximum_size_output);
    assert(status.error == npm::NpmParameterErrorV1::kNone);
    std::string oversized = maximum_size + " ";
    ParseParametersFailure(oversized.c_str(), consumers, npm::NpmParameterErrorV1::kJsonTooLarge);

    const auto nested_json = [](std::size_t depth) {
        std::string json = R"JSON({"schema_version":1,"future":{"nested":)JSON";
        json.append(depth, '[');
        json += "0";
        json.append(depth, ']');
        json += "}}";
        return json;
    };
    const std::string maximum_depth = nested_json(npm::kNpmParametersMaxDepthV1 - 2);
    npm::NpmTaskParametersV1 maximum_depth_output;
    status = npm::ParseNpmParametersV1(maximum_depth.c_str(), consumers, &maximum_depth_output);
    assert(status.error == npm::NpmParameterErrorV1::kNone);

    const std::string deep = nested_json(npm::kNpmParametersMaxDepthV1 - 1);
    std::string deep_path = "/future/nested";
    for (std::size_t depth = 0; depth < npm::kNpmParametersMaxDepthV1 - 1; ++depth) {
        deep_path += "/0";
    }
    ParseParametersFailure(deep.c_str(), consumers, npm::NpmParameterErrorV1::kNestingTooDeep, deep_path.c_str());
}

void TestNpmParametersV1ConsumesOnlyEnabledAvailableModules() {
    npm::NpmParameterConsumersV1 basic_only;
    npm::NpmTaskParametersV1 output;
    auto status =
        npm::ParseNpmParametersV1(R"JSON({"schema_version":1,"session":{"max_tcp_ranges_per_direction":null},)JSON"
                                  R"JSON("future":{"anything":false},"basic":{}})JSON",
                                  basic_only, &output);
    assert(status.error == npm::NpmParameterErrorV1::kNone);
    assert(output.basic.has_value() && !output.session.has_value());

    status = npm::ParseNpmParametersV1(R"JSON({"schema_version":1,"session":{"unknown":"ignored"}})JSON", basic_only,
                                       &output);
    assert(status.error == npm::NpmParameterErrorV1::kNone && !output.session.has_value());
    ParseParametersFailure(R"JSON({"schema_version":1,"future":7})JSON", basic_only,
                           npm::NpmParameterErrorV1::kInvalidType, "/future");
    ParseParametersFailure(R"JSON({"schema_version":1,"basic":{"unknown":1}})JSON", basic_only,
                           npm::NpmParameterErrorV1::kUnknownConsumedField, "/basic/unknown");
    ParseParametersFailure(R"JSON({"schema_version":1,"basic":{"":1}})JSON", basic_only,
                           npm::NpmParameterErrorV1::kUnknownConsumedField, "/basic/");

    npm::NpmParameterConsumersV1 session = basic_only;
    session.basic_enabled = false;
    session.session_enabled = true;
    status = npm::ParseNpmParametersV1(R"JSON({"schema_version":1})JSON", session, &output);
    assert(status.error == npm::NpmParameterErrorV1::kNone);
    assert(!output.basic.has_value() && output.session.has_value());
    assert(output.session->max_tcp_ranges_per_direction == npm::kNpmDefaultSessionTcpRangesPerDirection);
    ParseParametersFailure(R"JSON({"schema_version":1,"session":{"max_tcp_ranges_per_direction":null}})JSON", session,
                           npm::NpmParameterErrorV1::kInvalidType, "/session/max_tcp_ranges_per_direction");
    ParseParametersFailure(R"JSON({"schema_version":1,"session":{"max_tcp_ranges_per_direction":"1024"}})JSON", session,
                           npm::NpmParameterErrorV1::kInvalidType, "/session/max_tcp_ranges_per_direction");
    ParseParametersFailure(R"JSON({"schema_version":1,"session":{"max_tcp_ranges_per_direction":7}})JSON", session,
                           npm::NpmParameterErrorV1::kInvalidRange, "/session/max_tcp_ranges_per_direction");
    ParseParametersFailure(R"JSON({"schema_version":1,"session":{"max_tcp_ranges_per_direction":65537}})JSON", session,
                           npm::NpmParameterErrorV1::kInvalidRange, "/session/max_tcp_ranges_per_direction");
    ParseParametersFailure(R"JSON({"schema_version":1,"session":{"max_tcp_ranges_per_direction":4294967296}})JSON",
                           session, npm::NpmParameterErrorV1::kInvalidRange, "/session/max_tcp_ranges_per_direction");
    ParseParametersFailure(R"JSON({"schema_version":1,"session":{"max_tcp_ranges_per_direction":-1}})JSON", session,
                           npm::NpmParameterErrorV1::kInvalidType, "/session/max_tcp_ranges_per_direction");
    ParseParametersFailure(R"JSON({"schema_version":1,"session":{"unknown":1}})JSON", session,
                           npm::NpmParameterErrorV1::kUnknownConsumedField, "/session/unknown");

    session.session_available = false;
    ParseParametersFailure(R"JSON({"schema_version":1,"session":{}})JSON", session,
                           npm::NpmParameterErrorV1::kUnavailableFeature, "/session");
    session.session_enabled = false;
    status = npm::ParseNpmParametersV1(R"JSON({"schema_version":1,"session":{"unknown":null}})JSON", session, &output);
    assert(status.error == npm::NpmParameterErrorV1::kNone && !output.session.has_value());
}

void TestNpmParametersV1UsesCoreNamespaceExclusively() {
    const npm::NpmParameterConsumersV1 consumers;
    npm::NpmTaskParametersV1 output;
    auto status = npm::ParseNpmParametersV1(R"JSON({"schema_version":1,"core":{"max_active_sessions":123}})JSON",
                                            consumers, &output);
    assert(status.error == npm::NpmParameterErrorV1::kNone);
    assert(output.core.analysis.max_active_sessions == 123);

    ParseParametersFailure(R"JSON({"schema_version":1,"framework":{}})JSON", consumers,
                           npm::NpmParameterErrorV1::kUnknownConsumedField, "/framework");
    ParseParametersFailure(R"JSON({"schema_version":1,"core":{},"framework":{"max_active_sessions":123}})JSON",
                           consumers, npm::NpmParameterErrorV1::kUnknownConsumedField, "/framework");
}

void TestNpmParametersV1ValidatesCoreAndConditionalLabeling() {
    npm::NpmParameterConsumersV1 consumers;
    npm::NpmTaskParametersV1 retention;
    for (uint32_t days : {1U, 3650U}) {
        const std::string json =
            R"JSON({"schema_version":1,"core":{"result":{"retention_days":)JSON" + std::to_string(days) + "}}}";
        assert(npm::ParseNpmParametersV1(json.c_str(), consumers, &retention).error == npm::NpmParameterErrorV1::kNone);
        assert(retention.core.result_retention_days == days);
    }
    for (const char* invalid : {"0", "3651", "null", "\"1\""}) {
        const std::string json =
            std::string(R"JSON({"schema_version":1,"core":{"result":{"retention_days":)JSON") + invalid + "}}}";
        ParseParametersFailure(json.c_str(), consumers,
                               invalid[0] == 'n' || invalid[0] == '"' ? npm::NpmParameterErrorV1::kInvalidType
                                                                      : npm::NpmParameterErrorV1::kInvalidRange,
                               "/core/result/retention_days");
    }
    ParseParametersFailure(R"JSON({"schema_version":1,"core":{"result":{"unknown":1}}})JSON", consumers,
                           npm::NpmParameterErrorV1::kUnknownConsumedField, "/core/result/unknown");
    ParseParametersFailure(R"JSON({"schema_version":1,"core":{"unknown":1}})JSON", consumers,
                           npm::NpmParameterErrorV1::kUnknownConsumedField, "/core/unknown");
    ParseParametersFailure(R"JSON({"schema_version":1,"core":{"":1}})JSON", consumers,
                           npm::NpmParameterErrorV1::kUnknownConsumedField, "/core/");
    ParseParametersFailure(R"JSON({"schema_version":1,"core":{"run_mode":null}})JSON", consumers,
                           npm::NpmParameterErrorV1::kInvalidType, "/core/run_mode");
    ParseParametersFailure(R"JSON({"schema_version":1,"core":{"run_mode":"batch"}})JSON", consumers,
                           npm::NpmParameterErrorV1::kInvalidValue, "/core/run_mode");
    ParseParametersFailure(R"JSON({"schema_version":1,"core":{"max_active_sessions":"1"}})JSON", consumers,
                           npm::NpmParameterErrorV1::kInvalidType, "/core/max_active_sessions");
    ParseParametersFailure(R"JSON({"schema_version":1,"core":{"max_active_sessions":0}})JSON", consumers,
                           npm::NpmParameterErrorV1::kInvalidRange, "/core/max_active_sessions");
    ParseParametersFailure(R"JSON({"schema_version":1,"core":null})JSON", consumers,
                           npm::NpmParameterErrorV1::kInvalidType, "/core");

    npm::NpmTaskParametersV1 output;
    auto status = npm::ParseNpmParametersV1(
        R"JSON({"schema_version":1,"core":{"labeling":null,"labeling_memory_mib":"ignored"}})JSON", consumers, &output);
    assert(status.error == npm::NpmParameterErrorV1::kNone);
    assert(!output.core.labeling_reference.has_value());
    assert(!output.core.labeling_memory_mib.has_value());

    consumers.labeling_enabled = true;
    consumers.labeling_available = true;
    for (const char* reference : {"", "rules@1", "config.rules", "config.rules@latest", "config.rules@0",
                                  "config.Rules@1", "config.rules@01"}) {
        const std::string json =
            std::string(R"JSON({"schema_version":1,"core":{"labeling":")JSON") + reference + R"JSON("}})JSON";
        ParseParametersFailure(json.c_str(), consumers, npm::NpmParameterErrorV1::kInvalidExactReference,
                               "/core/labeling");
    }
    status = npm::ParseNpmParametersV1(R"JSON({"schema_version":1,"core":{"labeling":"config.corp-labels@7"}})JSON",
                                       consumers, &output);
    assert(status.error == npm::NpmParameterErrorV1::kNone);
    assert(output.core.labeling_reference == "config.corp-labels@7");
    assert(output.core.labeling_memory_mib == npm::kNpmDefaultLabelingMemoryMiB);

    for (uint32_t memory_mib : {8U, 12U, 16U, 24U, 32U, 64U, 96U, 127U, 128U}) {
        const std::string json =
            R"JSON({"schema_version":1,"core":{"labeling":"config.corp-labels@7","labeling_memory_mib":)JSON" +
            std::to_string(memory_mib) + "}}";
        status = npm::ParseNpmParametersV1(json.c_str(), consumers, &output);
        assert(status.error == npm::NpmParameterErrorV1::kNone);
        assert(output.core.labeling_memory_mib == memory_mib);
    }
    status = npm::ParseNpmParametersV1(
        R"JSON({"schema_version":1,"core":{"labeling":"config.corp-labels@7","max_tracked_bytes":536870912,"labeling_memory_mib":256}})JSON",
        consumers, &output);
    assert(status.error == npm::NpmParameterErrorV1::kNone);
    assert(output.core.labeling_memory_mib == 256);

    for (const char* invalid : {"null", "\"64\"", "0", "7", "512"}) {
        const std::string json =
            std::string(
                R"JSON({"schema_version":1,"core":{"labeling":"config.corp-labels@7","labeling_memory_mib":)JSON") +
            invalid + "}}";
        ParseParametersFailure(json.c_str(), consumers,
                               invalid[0] == '"' || invalid[0] == 'n' ? npm::NpmParameterErrorV1::kInvalidType
                                                                      : npm::NpmParameterErrorV1::kInvalidRange,
                               "/core/labeling_memory_mib");
    }
    ParseParametersFailure(
        R"JSON({"schema_version":1,"core":{"labeling":"config.corp-labels@7","max_tracked_bytes":538968064,"labeling_memory_mib":257}})JSON",
        consumers, npm::NpmParameterErrorV1::kInvalidRange, "/core/labeling_memory_mib");
    ParseParametersFailure(
        R"JSON({"schema_version":1,"core":{"labeling":"config.corp-labels@7","labeling_memory_mib":256}})JSON",
        consumers, npm::NpmParameterErrorV1::kInvalidRange, "/core/labeling_memory_mib");
    ParseParametersFailure(
        R"JSON({"schema_version":1,"core":{"labeling":"config.corp-labels@7","max_tracked_bytes":268435455,"labeling_memory_mib":128}})JSON",
        consumers, npm::NpmParameterErrorV1::kInvalidRange, "/core/labeling_memory_mib");

    consumers.labeling_available = false;
    ParseParametersFailure(R"JSON({"schema_version":1})JSON", consumers, npm::NpmParameterErrorV1::kUnavailableFeature,
                           "/labeling");
}

void AssertTaskBudgetUsage(const npm::NpmBudgetUsage& usage, uint64_t session, uint64_t module, uint64_t input,
                           uint64_t output) {
    assert(usage.session_state_bytes == session);
    assert(usage.module_state_bytes == module);
    assert(usage.input_batch_bytes == input);
    assert(usage.pending_output_bytes == output);
}

void AssertEquivalentBudgetUsage(const npm::NpmBudgetUsage& left, const npm::NpmBudgetUsage& right) {
    assert(left.session_state_bytes == right.session_state_bytes);
    assert(left.module_state_bytes == right.module_state_bytes);
    assert(left.input_batch_bytes == right.input_batch_bytes);
    assert(left.pending_output_bytes == right.pending_output_bytes);
}

void AssertEquivalentRecordBatch(const std::shared_ptr<arrow::RecordBatch>& left,
                                 const std::shared_ptr<arrow::RecordBatch>& right) {
    assert(left != nullptr && right != nullptr);
    assert(left->schema()->Equals(*right->schema(), true) && left->num_rows() == right->num_rows());
    for (int column = 0; column < left->num_columns(); ++column) {
        if (left->GetColumnByName("period_start_ns") && left->schema()->field(column)->name() == "observed_at")
            continue;
        assert(left->column(column)->Equals(*right->column(column)));
    }
}

void AssertEquivalentOfflineBatchStatus(const npm::NpmBasicOfflineBatchStatus& left,
                                        const npm::NpmBasicOfflineBatchStatus& right) {
    assert(left.error == right.error);
    assert(left.runtime_state == right.runtime_state);
    assert(left.input_bytes == right.input_bytes);
    assert(left.budget_error == right.budget_error);
    assert(left.batch_view_error == right.batch_view_error);
    assert(left.process_status.error == right.process_status.error);
    assert(left.process_status.row == right.process_status.row);
    assert(left.process_status.batch_error == right.process_status.batch_error);
    assert(left.process_status.packet_status.error == right.process_status.packet_status.error);
    assert(left.process_status.packet_status.binding_error == right.process_status.packet_status.binding_error);
    assert(left.process_status.packet_status.session_error == right.process_status.packet_status.session_error);
    assert(left.process_status.packet_status.module_error == right.process_status.packet_status.module_error);
    assert(left.process_status.progress_disposition == right.process_status.progress_disposition);
    assert(left.process_status.module_error == right.process_status.module_error);
    assert(left.drain_status.error == right.drain_status.error);
    assert(left.drain_status.event_index == right.drain_status.event_index);
    assert(left.drain_status.projection_error == right.drain_status.projection_error);
    assert(left.drain_status.encode_error == right.drain_status.encode_error);
    assert(left.drain_status.session_encode_error == right.drain_status.session_encode_error);
}

void AssertEquivalentRealtimeStatus(const npm::NpmBasicRealtimeMaintenanceStatus& left,
                                    const npm::NpmBasicRealtimeMaintenanceStatus& right) {
    assert(left.error == right.error);
    assert(left.runtime_state == right.runtime_state);
    assert(left.progress_disposition == right.progress_disposition);
    assert(left.snapshot_due == right.snapshot_due);
    assert(left.emitted == right.emitted);
    assert(left.active_sessions == right.active_sessions);
    assert(left.ended_sessions == right.ended_sessions);
    assert(left.active_session_index == right.active_session_index);
    assert(left.session_error == right.session_error);
    assert(left.module_error == right.module_error);
    assert(left.projection_error == right.projection_error);
    assert(left.writer_error == right.writer_error);
    assert(left.drain_status.error == right.drain_status.error);
    assert(left.drain_status.event_index == right.drain_status.event_index);
    assert(left.drain_status.projection_error == right.drain_status.projection_error);
    assert(left.drain_status.encode_error == right.drain_status.encode_error);
    assert(left.drain_status.session_encode_error == right.drain_status.session_encode_error);
}

void AssertEquivalentEofStatus(const npm::NpmEofFlushStatus& left, const npm::NpmEofFlushStatus& right) {
    assert(left.error == right.error);
    assert(left.session_error == right.session_error);
    assert(left.module_error == right.module_error);
    assert(left.drain_status.error == right.drain_status.error);
    assert(left.drain_status.event_index == right.drain_status.event_index);
    assert(left.drain_status.projection_error == right.drain_status.projection_error);
    assert(left.drain_status.encode_error == right.drain_status.encode_error);
    assert(left.drain_status.session_encode_error == right.drain_status.session_encode_error);
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
    assert(budget.Reserve(npm::NpmBudgetCategory::kSessionState, 1) == npm::NpmBudgetError::kTrackedLimitExceeded);
    AssertTaskBudgetUsage(budget.Usage(), npm::kNpmMinTrackedBytes - 2, 1, 1, 0);

    assert(budget.Reserve(npm::NpmBudgetCategory::kPendingOutput, npm::kNpmMinPendingOutputBytes) ==
           npm::NpmBudgetError::kNone);
    assert(budget.Reserve(npm::NpmBudgetCategory::kPendingOutput, 1) ==
           npm::NpmBudgetError::kPendingOutputLimitExceeded);
    AssertTaskBudgetUsage(budget.Usage(), npm::kNpmMinTrackedBytes - 2, 1, 1, npm::kNpmMinPendingOutputBytes);

    assert(budget.Release(npm::NpmBudgetCategory::kInputBatch, 2) == npm::NpmBudgetError::kReleaseUnderflow);
    assert(budget.Reserve(static_cast<npm::NpmBudgetCategory>(99), 1) == npm::NpmBudgetError::kInvalidCategory);
    assert(budget.Release(static_cast<npm::NpmBudgetCategory>(99), 1) == npm::NpmBudgetError::kInvalidCategory);
    AssertTaskBudgetUsage(budget.Usage(), npm::kNpmMinTrackedBytes - 2, 1, 1, npm::kNpmMinPendingOutputBytes);

    assert(budget.Release(npm::NpmBudgetCategory::kSessionState, npm::kNpmMinTrackedBytes - 2) ==
           npm::NpmBudgetError::kNone);
    assert(budget.Release(npm::NpmBudgetCategory::kModuleState, 1) == npm::NpmBudgetError::kNone);
    assert(budget.Release(npm::NpmBudgetCategory::kInputBatch, 1) == npm::NpmBudgetError::kNone);
    assert(budget.Release(npm::NpmBudgetCategory::kPendingOutput, npm::kNpmMinPendingOutputBytes) ==
           npm::NpmBudgetError::kNone);
    AssertTaskBudgetUsage(budget.Usage(), 0, 0, 0, 0);

    npm::NpmTaskBudget independent(config);
    const auto peak = budget.HighWaterMarks();
    assert(peak.tracked_bytes == npm::kNpmMinTrackedBytes);
    assert(peak.input_batch_bytes == 1 && peak.pending_output_bytes == npm::kNpmMinPendingOutputBytes);
    // Releases and rejected reservations preserve the actual high-water marks.
    assert(independent.HighWaterMarks().tracked_bytes == 0);
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
    if (mode == npm::NpmRunMode::kOffline) config.analysis.result_mode = npm::NpmResultMode::kFinal;
    config.domains.input_namespace = "pcapfile.capture";
    config.domains.bindings = {{0, 77}, {1, 77}};
    return config;
}

std::unique_ptr<npm::NpmBasicTaskRuntime> CreateRuntimeForTest(const npm::NpmBasicTaskConfig& config,
                                                               flowsql::IQuerier* querier) {
    std::shared_ptr<arrow::Schema> output_schema;
    std::unique_ptr<npm::NpmBasicTaskRuntime> runtime;
    const auto status =
        npm::NpmBasicTaskRuntime::Create(config, querier, flowsql::packet::PacketSchema(), &output_schema, &runtime);
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

std::unique_ptr<npm::NpmBasicTaskRuntime> CreateRealtimeRuntimeForTest(const npm::NpmBasicTaskConfig& config,
                                                                       flowsql::IQuerier* querier) {
    std::shared_ptr<arrow::Schema> output_schema;
    std::unique_ptr<npm::NpmBasicTaskRuntime> runtime;
    const auto status = npm::NpmBasicTaskRuntime::CreateWithTimeCapabilities(
        config, querier, flowsql::packet::PacketSchema(), AllRealtimeTimeCapabilities(), &output_schema, &runtime);
    assert(status.error == npm::NpmBasicTaskRuntimeError::kNone);
    const auto expected_schema = config.features.observing == npm::NpmResultEntity::kSession
                                     ? npm::NpmSessionResultSchema()
                                     : npm::NpmBasicResultSchema();
    assert(output_schema != nullptr && output_schema->Equals(*expected_schema, true));
    assert(runtime != nullptr);
    return runtime;
}

void TestNpmBasicTaskRuntimeMatchesLegacyAndParametersV1Offline() {
    for (const bool observing_session : {false, true}) {
        auto configs = MakeEquivalentRuntimeTaskConfigs(npm::NpmRunMode::kOffline, observing_session);
        configs.legacy_json.assign(configs.legacy_json.size(), 'x');
        configs.parameters_v1_json.assign(configs.parameters_v1_json.size(), 'y');
        assert(configs.legacy.domains.input_namespace == "pcapfile.capture");
        assert(configs.parameters_v1.domains.input_namespace == "pcapfile.capture");

        ContextDictionary dictionary;
        ContextProtocol protocol(&dictionary);
        DualContextPool pool(&protocol);
        SinglePoolQuerier querier(&pool);
        std::shared_ptr<arrow::Schema> legacy_schema;
        std::shared_ptr<arrow::Schema> parameters_schema;
        std::unique_ptr<npm::NpmBasicTaskRuntime> legacy_runtime;
        std::unique_ptr<npm::NpmBasicTaskRuntime> parameters_runtime;
        auto legacy_create = npm::NpmBasicTaskRuntime::Create(configs.legacy, &querier, flowsql::packet::PacketSchema(),
                                                              &legacy_schema, &legacy_runtime);
        auto parameters_create = npm::NpmBasicTaskRuntime::Create(
            configs.parameters_v1, &querier, flowsql::packet::PacketSchema(), &parameters_schema, &parameters_runtime);
        assert(legacy_create.error == npm::NpmBasicTaskRuntimeError::kNone);
        assert(parameters_create.error == npm::NpmBasicTaskRuntimeError::kNone);
        assert(legacy_schema != nullptr && parameters_schema != nullptr);
        assert(legacy_schema->Equals(*parameters_schema, true));
        assert(legacy_runtime != nullptr && parameters_runtime != nullptr);
        AssertEquivalentTaskConfig(legacy_runtime->Config(), parameters_runtime->Config());
        assert(legacy_runtime->State() == npm::NpmEofFlushState::kOpen);
        assert(parameters_runtime->State() == npm::NpmEofFlushState::kOpen);
        assert(pool.acquire_calls == 2 && pool.release_calls == 0);

        const auto packet = MakeIpv4TcpPacket("192.0.2.10", 41000, "198.51.100.20", 443, {0x16, 0x03}, kTcpAck, 101);
        auto input = MakeEncodedPacketBatch({MakeBatchPacketRecord(packet, 0, 100, 1)});
        std::shared_ptr<arrow::RecordBatch> legacy_output;
        std::shared_ptr<arrow::RecordBatch> parameters_output;
        const auto legacy_status = legacy_runtime->ProcessOfflineBatch(input, &legacy_output);
        const auto parameters_status = parameters_runtime->ProcessOfflineBatch(input, &parameters_output);
        AssertEquivalentOfflineBatchStatus(legacy_status, parameters_status);
        assert(legacy_status.error == npm::NpmBasicOfflineBatchError::kNone);
        AssertEquivalentRecordBatch(legacy_output, parameters_output);
        assert(legacy_output->num_rows() == 0);
        assert(legacy_runtime->Sessions().size() == 1);
        assert(parameters_runtime->Sessions().size() == 1);
        AssertEquivalentBudgetUsage(legacy_runtime->Budget()->Usage(), parameters_runtime->Budget()->Usage());

        legacy_output.reset();
        parameters_output.reset();
        AssertEquivalentBudgetUsage(legacy_runtime->Budget()->Usage(), parameters_runtime->Budget()->Usage());
        std::shared_ptr<arrow::RecordBatch> legacy_final;
        std::shared_ptr<arrow::RecordBatch> parameters_final;
        const auto legacy_flush = legacy_runtime->FlushOffline(100, &legacy_final);
        const auto parameters_flush = parameters_runtime->FlushOffline(100, &parameters_final);
        AssertEquivalentEofStatus(legacy_flush, parameters_flush);
        assert(legacy_flush.error == npm::NpmEofFlushError::kNone);
        AssertEquivalentRecordBatch(legacy_final, parameters_final);
        assert(legacy_final->num_rows() == 1);
        assert(legacy_runtime->State() == npm::NpmEofFlushState::kFlushed);
        assert(parameters_runtime->State() == npm::NpmEofFlushState::kFlushed);
        assert(pool.release_calls == 2);
        AssertEquivalentBudgetUsage(legacy_runtime->Budget()->Usage(), parameters_runtime->Budget()->Usage());
        assert(legacy_runtime->Budget()->Usage().pending_output_bytes > 0);

        legacy_final.reset();
        parameters_final.reset();
        AssertTaskBudgetUsage(legacy_runtime->Budget()->Usage(), 0, 0, 0, 0);
        AssertTaskBudgetUsage(parameters_runtime->Budget()->Usage(), 0, 0, 0, 0);
    }
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
    auto status =
        npm::NpmBasicTaskRuntime::Create(config, &querier, flowsql::packet::PacketSchema(), &output_schema, &runtime);
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
    status = npm::NpmBasicTaskRuntime::Create(config, &querier, wrong_schema, &sentinel_schema, &runtime);
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
    status = npm::NpmBasicTaskRuntime::Create(MakeRuntimeTaskConfig(), &querier, flowsql::packet::PacketSchema(),
                                              &blocked_schema, &blocked);
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

    status = npm::NpmBasicTaskRuntime::Create(MakeRuntimeTaskConfig(), &querier, flowsql::packet::PacketSchema(),
                                              &blocked_schema, &blocked);
    assert(status.error == npm::NpmBasicTaskRuntimeError::kNone);
    assert(blocked != nullptr && pool.acquire_calls == 3 && pool.release_calls == 1);
    blocked.reset();
    assert(pool.release_calls == 2);
}

void TestNpmBasicTaskRuntimeSelectsObservedSchema() {
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    size_t completed_cases = 0;
    const auto packet = MakeIpv4TcpPacket("192.0.2.1", 50000, "198.51.100.2", 443, {1}, kTcpAck, 100, 90, 2048);

    const auto assert_schema = [&](const npm::NpmBasicTaskConfig& config,
                                   const std::shared_ptr<arrow::Schema>& expected_schema, bool expect_session_module) {
        auto sentinel_schema = arrow::schema({arrow::field("sentinel", arrow::int8())});
        std::shared_ptr<arrow::Schema> output_schema = sentinel_schema;
        std::unique_ptr<npm::NpmBasicTaskRuntime> runtime;
        const auto status = npm::NpmBasicTaskRuntime::Create(config, &querier, flowsql::packet::PacketSchema(),
                                                             &output_schema, &runtime);

        assert(status.error == npm::NpmBasicTaskRuntimeError::kNone);
        assert(output_schema.get() == expected_schema.get());
        assert(runtime != nullptr);
        assert(runtime->Config().features.basic_enabled == config.features.basic_enabled);
        assert(runtime->Config().features.session_enabled == config.features.session_enabled);
        assert(runtime->Config().features.observing == config.features.observing);
        assert(pool.acquire_calls == static_cast<int>(completed_cases + 1));
        assert(pool.release_calls == static_cast<int>(completed_cases));
        assert(runtime->Modules().size() == (expect_session_module ? 1 : 0));

        npm::NpmSessionAnalysisModule* session_module = nullptr;
        if (expect_session_module) {
            session_module = dynamic_cast<npm::NpmSessionAnalysisModule*>(runtime->Modules()[0]);
            assert(session_module != nullptr && session_module->tracked_sessions() == 0);
        }

        auto view = packet.View(0, 100);
        view.meta.timestamp_ns = static_cast<int64_t>(completed_cases + 1) * 10;
        std::vector<npm::NpmSessionSnapshot> ended_sessions;
        const auto process_status = npm::ProcessNpmPacket(runtime->Config().domains, view, packet.layer,
                                                          runtime->Sessions(), *runtime->ProtocolContext().Identifier(),
                                                          runtime->Modules(), runtime->Collector(), &ended_sessions);
        assert(process_status.error == npm::NpmPacketProcessError::kNone);
        assert(ended_sessions.empty() && runtime->Sessions().size() == 1);
        std::vector<npm::NpmSessionView> active_sessions;
        assert(runtime->Sessions().SnapshotActive(&active_sessions) == npm::NpmSessionTableError::kNone);
        assert(active_sessions.size() == 1);
        assert(active_sessions[0].packets_ab + active_sessions[0].packets_ba == 1);
        assert(protocol.identify_pipelines.size() == completed_cases + 1);
        assert(protocol.identify_pipelines.back() == 3);
        const auto usage = runtime->Budget()->Usage();
        assert(usage.session_state_bytes > 0);
        if (expect_session_module) {
            assert(session_module->tracked_sessions() == 1);
            assert(session_module->tracked_bytes() == usage.module_state_bytes);
            assert(usage.module_state_bytes > 0);
        } else {
            assert(usage.module_state_bytes == 0);
        }

        auto retained_budget = runtime->Budget();
        if (completed_cases == 2) {
            std::shared_ptr<arrow::RecordBatch> ignored_output;
            const auto failure = runtime->ProcessOfflineBatch(nullptr, &ignored_output);
            assert(failure.error == npm::NpmBasicOfflineBatchError::kNullInput);
            assert(runtime->State() == npm::NpmEofFlushState::kFailed);
        } else {
            runtime->Cancel();
            assert(runtime->State() == npm::NpmEofFlushState::kCancelled);
        }
        AssertTaskBudgetUsage(retained_budget->Usage(), 0, 0, 0, 0);
        runtime.reset();
        ++completed_cases;
        assert(pool.release_calls == static_cast<int>(completed_cases));
    };

    auto config = MakeRuntimeTaskConfig();
    assert_schema(config, npm::NpmBasicResultSchema(), false);

    config.features.basic_enabled = true;
    config.features.session_enabled = true;
    config.features.observing = npm::NpmResultEntity::kBasic;
    assert_schema(config, npm::NpmBasicResultSchema(), true);

    config.features.basic_enabled = false;
    config.features.session_enabled = true;
    config.features.observing = npm::NpmResultEntity::kSession;
    assert_schema(config, npm::NpmSessionResultSchema(), true);

    config.features.basic_enabled = true;
    assert_schema(config, npm::NpmSessionResultSchema(), true);
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
    status = npm::NpmBasicTaskRuntime::Create(config, &querier, packet_without_metadata, &output_schema, &runtime);
    assert(status.error == npm::NpmBasicTaskRuntimeError::kSchemaMismatch);
    assert(output_schema == original_schema && runtime == nullptr && pool.acquire_calls == 0);

    status = npm::NpmBasicTaskRuntime::Create(config, &querier, flowsql::packet::PacketSchema(), nullptr, &runtime);
    assert(status.error == npm::NpmBasicTaskRuntimeError::kNullOutputSchema);
    assert(runtime == nullptr && pool.acquire_calls == 0);
    status =
        npm::NpmBasicTaskRuntime::Create(config, &querier, flowsql::packet::PacketSchema(), &output_schema, nullptr);
    assert(status.error == npm::NpmBasicTaskRuntimeError::kNullRuntimeOutput);
    assert(output_schema == original_schema && pool.acquire_calls == 0);

    config = MakeRuntimeTaskConfig(npm::NpmRunMode::kRealtime);
    status =
        npm::NpmBasicTaskRuntime::Create(config, &querier, flowsql::packet::PacketSchema(), &output_schema, &runtime);
    assert(status.error == npm::NpmBasicTaskRuntimeError::kTimeCapabilityError);
    assert(status.time_error == npm::NpmTimeCapabilityError::kMissingMonotonicTimeDrive);
    assert(output_schema == original_schema && runtime == nullptr && pool.acquire_calls == 0);

    config = MakeRuntimeTaskConfig();
    status =
        npm::NpmBasicTaskRuntime::Create(config, nullptr, flowsql::packet::PacketSchema(), &output_schema, &runtime);
    assert(status.error == npm::NpmBasicTaskRuntimeError::kProtocolContextError);
    assert(status.protocol_error == npm::NpmProtocolContextError::kNullQuerier);
    assert(output_schema == original_schema && runtime == nullptr && pool.acquire_calls == 0);

    SinglePoolQuerier missing_querier(nullptr);
    status = npm::NpmBasicTaskRuntime::Create(config, &missing_querier, flowsql::packet::PacketSchema(), &output_schema,
                                              &runtime);
    assert(status.error == npm::NpmBasicTaskRuntimeError::kProtocolContextError);
    assert(status.protocol_error == npm::NpmProtocolContextError::kProviderNotFound);
    assert(output_schema == original_schema && runtime == nullptr);

    ContextPool exhausted_pool(&protocol, flowsql::ProtocolPipelinePoolError::kExhausted);
    SinglePoolQuerier exhausted_querier(&exhausted_pool);
    status = npm::NpmBasicTaskRuntime::Create(config, &exhausted_querier, flowsql::packet::PacketSchema(),
                                              &output_schema, &runtime);
    assert(status.error == npm::NpmBasicTaskRuntimeError::kProtocolContextError);
    assert(status.protocol_error == npm::NpmProtocolContextError::kPipelineExhausted);
    assert(output_schema == original_schema && runtime == nullptr);
    assert(exhausted_pool.acquire_calls == 1 && exhausted_pool.release_calls == 0);

    ContextPool unavailable_pool(&protocol, flowsql::ProtocolPipelinePoolError::kUnavailable);
    SinglePoolQuerier unavailable_querier(&unavailable_pool);
    status = npm::NpmBasicTaskRuntime::Create(config, &unavailable_querier, flowsql::packet::PacketSchema(),
                                              &output_schema, &runtime);
    assert(status.error == npm::NpmBasicTaskRuntimeError::kProtocolContextError);
    assert(status.protocol_error == npm::NpmProtocolContextError::kPipelineUnavailable);
    assert(output_schema == original_schema && runtime == nullptr);
    assert(unavailable_pool.acquire_calls == 1 && unavailable_pool.release_calls == 0);

    ContextPool no_protocol_pool(nullptr);
    SinglePoolQuerier no_protocol_querier(&no_protocol_pool);
    status = npm::NpmBasicTaskRuntime::Create(config, &no_protocol_querier, flowsql::packet::PacketSchema(),
                                              &output_schema, &runtime);
    assert(status.error == npm::NpmBasicTaskRuntimeError::kProtocolContextError);
    assert(status.protocol_error == npm::NpmProtocolContextError::kProtocolUnavailable);
    assert(output_schema == original_schema && runtime == nullptr);
    assert(no_protocol_pool.acquire_calls == 0 && no_protocol_pool.release_calls == 0);

    ContextProtocol no_dictionary_protocol(nullptr);
    ContextPool no_dictionary_pool(&no_dictionary_protocol);
    SinglePoolQuerier no_dictionary_querier(&no_dictionary_pool);
    status = npm::NpmBasicTaskRuntime::Create(config, &no_dictionary_querier, flowsql::packet::PacketSchema(),
                                              &output_schema, &runtime);
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
        config, &querier, flowsql::packet::PacketSchema(), AllRealtimeTimeCapabilities(), &sentinel_schema, &runtime);
    assert(status.error == npm::NpmBasicTaskRuntimeError::kNone);
    assert(status.time_error == npm::NpmTimeCapabilityError::kNone);
    assert(sentinel_schema != original_schema && sentinel_schema->Equals(*npm::NpmBasicResultSchema(), true));
    assert(runtime != nullptr && runtime->Config().analysis.run_mode == npm::NpmRunMode::kRealtime);
    assert(pool.acquire_calls == 1 && pool.release_calls == 0);
}

npm::NpmBasicRealtimeMaintenanceInput RealtimeMaintenanceInput(int64_t monotonic_now_ns, int64_t observed_at_ns,
                                                               int64_t capture_time_ns, bool packet_observed,
                                                               bool source_idle_confirmed, bool source_backlog_known,
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

constexpr int64_t kPeriodicTestInterval = 10'000'000;

struct PeriodicRuntimeFixture {
    ContextDictionary dictionary;
    ContextProtocol protocol{&dictionary};
    ContextPool pool{&protocol};
    SinglePoolQuerier querier{&pool};
    npm::NpmBasicTaskConfig config;
    std::unique_ptr<npm::NpmBasicTaskRuntime> runtime;
    std::shared_ptr<npm::INpmTaskBudget> budget;
    std::shared_ptr<arrow::RecordBatch> output;
    int64_t monotonic_ns = 0;
    uint64_t sequence = 0;

    explicit PeriodicRuntimeFixture(bool session = false, bool observe_session = false, bool basic = true,
                                    npm::NpmRunMode mode = npm::NpmRunMode::kRealtime) {
        config = MakeRuntimeTaskConfig(mode);
        config.analysis.result_mode = npm::NpmResultMode::kPeriodicSnapshot;
        config.analysis.output_interval_ns = kPeriodicTestInterval;
        config.analysis.out_of_order_tolerance_ns = 0;
        config.analysis.tcp_idle_timeout_ns = 100 * kPeriodicTestInterval;
        config.analysis.udp_idle_timeout_ns = 100 * kPeriodicTestInterval;
        config.features.basic_enabled = basic;
        config.features.session_enabled = session;
        config.features.observing = observe_session ? npm::NpmResultEntity::kSession : npm::NpmResultEntity::kBasic;
        runtime = mode == npm::NpmRunMode::kRealtime ? CreateRealtimeRuntimeForTest(config, &querier)
                                                     : CreateRuntimeForTest(config, &querier);
        budget = runtime->Budget();
        assert(runtime->Modules().size() == static_cast<size_t>(basic) + static_cast<size_t>(session));
    }
    npm::NpmBasicOfflineBatchStatus Packet(const PacketFixture& packet, int64_t timestamp, uint32_t wire_len = 0) {
        auto record = MakeBatchPacketRecord(packet, 0, timestamp, ++sequence);
        if (wire_len) record.meta.wire_len = wire_len;
        output.reset();
        return runtime->ProcessOfflineBatch(MakeEncodedPacketBatch({record}), &output);
    }
    npm::NpmBasicRealtimeMaintenanceStatus Drive(int64_t capture_time, bool idle = true, bool backlog_known = true,
                                                 bool backlog = false) {
        output.reset();
        monotonic_ns += kPeriodicTestInterval;
        return runtime->DriveRealtimeMaintenance(
            RealtimeMaintenanceInput(monotonic_ns, capture_time + 100, capture_time, false, idle, backlog_known,
                                     backlog),
            &output);
    }
    void Finish(int64_t observed_at) {
        output.reset();
        assert(runtime->FlushOffline(observed_at, &output).error == npm::NpmEofFlushError::kNone);
        assert(runtime->State() == npm::NpmEofFlushState::kFlushed && pool.release_calls == 1);
        assert(budget->Usage().session_state_bytes == 0 && budget->Usage().module_state_bytes == 0);
    }
    void CancelAndCheck() {
        runtime->Cancel();
        output.reset();
        assert(runtime->State() == npm::NpmEofFlushState::kCancelled);
        assert(npm::NpmTrackedBudgetBytes(budget->Usage()) == 0 && pool.release_calls == 1);
    }
};

uint64_t PeriodicUInt(const std::shared_ptr<arrow::RecordBatch>& batch, const char* field, int64_t row = 0) {
    return BasicResultColumn<arrow::UInt64Array>(batch, field)->Value(row);
}
int64_t PeriodicInt(const std::shared_ptr<arrow::RecordBatch>& batch, const char* field, int64_t row = 0) {
    return BasicResultColumn<arrow::Int64Array>(batch, field)->Value(row);
}
bool PeriodicFlag(const std::shared_ptr<arrow::RecordBatch>& batch, const char* field, int64_t row = 0) {
    return BasicResultColumn<arrow::BooleanArray>(batch, field)->Value(row);
}

void TestNpmBasicTaskRuntimeRealtimeMaintenanceAndRevisions() {
    PeriodicRuntimeFixture fixture;
    const auto packet = MakeIpv4TcpPacket("192.0.2.1", 50000, "192.0.2.2", 443, {1});
    assert(fixture.Packet(packet, 1).error == npm::NpmBasicOfflineBatchError::kNone);
    const auto identity = fixture.runtime->Sessions().size();
    assert(identity == 1);
    for (const auto flags : {std::array<bool, 3>{false, true, false}, {true, false, false}, {true, true, true}}) {
        auto status = fixture.Drive(kPeriodicTestInterval, flags[0], flags[1], flags[2]);
        assert(status.error == npm::NpmBasicRealtimeMaintenanceError::kNone && !status.emitted && !fixture.output);
    }
    auto status = fixture.Drive(kPeriodicTestInterval);
    assert(status.error == npm::NpmBasicRealtimeMaintenanceError::kNone && status.emitted);
    assert(fixture.output->num_rows() == 1);
    assert(PeriodicUInt(fixture.output, "revision") == 1);
    assert(PeriodicUInt(fixture.output, "interval_packets_ab") == 1);
    assert(PeriodicFlag(fixture.output, "period_complete") && !PeriodicFlag(fixture.output, "is_final"));
    status = fixture.Drive(kPeriodicTestInterval - 1);
    assert(status.error == npm::NpmBasicRealtimeMaintenanceError::kNone && !status.emitted && !fixture.output);
    status = fixture.Drive(2 * kPeriodicTestInterval);
    assert(status.error == npm::NpmBasicRealtimeMaintenanceError::kNone && status.emitted);
    assert(PeriodicUInt(fixture.output, "revision") == 2 && PeriodicUInt(fixture.output, "packets_ab") == 1);
    assert(PeriodicUInt(fixture.output, "interval_wire_bytes_total") == 0);
    assert(fixture.runtime->Sessions().size() == identity);
    fixture.Finish(2 * kPeriodicTestInterval + 100);
    assert(fixture.output->num_rows() == 1 && PeriodicUInt(fixture.output, "revision") == 3);
    assert(PeriodicFlag(fixture.output, "is_final") && PeriodicFlag(fixture.output, "period_complete"));
    assert(PeriodicUInt(fixture.output, "interval_wire_bytes_total") == 0);
    assert(PeriodicInt(fixture.output, "period_start_ns") == kPeriodicTestInterval);
    assert(fixture.runtime->FlushOffline(2 * kPeriodicTestInterval + 101, &fixture.output).error ==
           npm::NpmEofFlushError::kAlreadyFlushed);
    fixture.output.reset();
    assert(npm::NpmTrackedBudgetBytes(fixture.budget->Usage()) == 0);
}

void TestNpmBasicTaskRuntimeRoutesRealtimePeriodicSnapshots() {
    const auto packet = MakeIpv6UdpPacket("2001:db8::1", 53000, "2001:db8::2", 53, {1});
    for (const auto selection :
         {std::array<bool, 3>{false, false, true}, {true, false, true}, {true, true, false}, {true, true, true}}) {
        PeriodicRuntimeFixture fixture(selection[0], selection[1], selection[2]);
        assert(fixture.Packet(packet, 1).error == npm::NpmBasicOfflineBatchError::kNone);
        assert(fixture.Drive(1).error == npm::NpmBasicRealtimeMaintenanceError::kNone);
        auto status = fixture.Drive(kPeriodicTestInterval);
        assert(status.error == npm::NpmBasicRealtimeMaintenanceError::kNone && status.emitted);
        assert(fixture.output->num_rows() == 1);
        assert(PeriodicUInt(fixture.output, "revision") == 1);
        assert(!PeriodicFlag(fixture.output, "is_final"));
        const auto expected = selection[1] ? npm::NpmSessionResultSchema() : npm::NpmBasicResultSchema();
        assert(fixture.output->schema()->Equals(*expected, true));
        if (!selection[1]) {
            assert(PeriodicUInt(fixture.output, "interval_packets_ab") == 1);
            assert(PeriodicUInt(fixture.output, "wire_bytes_total") == packet.bytes.size());
            assert(PeriodicInt(fixture.output, "period_end_ns") == kPeriodicTestInterval);
        }
        assert(fixture.runtime->Sessions().size() == 1);
        fixture.CancelAndCheck();
    }
}

void TestNpmBasicTaskRuntimeClosesRealtimeSessionsAfterPeriodicSnapshots() {
    for (const bool session : {false, true}) {
        PeriodicRuntimeFixture fixture(session);
        const auto first = MakeIpv4TcpPacket("192.0.2.1", 50000, "192.0.2.2", 443, {1}, kTcpAck, 100);
        const auto close = MakeIpv4TcpPacket("192.0.2.2", 443, "192.0.2.1", 50000, {}, kTcpRst, 200);
        assert(fixture.Packet(first, 1, 100).error == npm::NpmBasicOfflineBatchError::kNone);
        assert(fixture.Drive(kPeriodicTestInterval).error == npm::NpmBasicRealtimeMaintenanceError::kNone);
        const auto session_id = PeriodicUInt(fixture.output, "session_id");
        assert(fixture.Packet(close, kPeriodicTestInterval + 1, 80).error == npm::NpmBasicOfflineBatchError::kNone);
        assert(fixture.runtime->Sessions().size() == 0);
        assert(fixture.Drive(kPeriodicTestInterval + 1).error == npm::NpmBasicRealtimeMaintenanceError::kNone);
        assert(fixture.output->num_rows() == 1 && PeriodicUInt(fixture.output, "session_id") == session_id);
        assert(PeriodicUInt(fixture.output, "revision") == 2 && PeriodicFlag(fixture.output, "is_final"));
        assert(!PeriodicFlag(fixture.output, "period_complete"));
        assert(PeriodicUInt(fixture.output, "interval_wire_bytes_total") == 80);
        assert(PeriodicUInt(fixture.output, "wire_bytes_total") == 180);
        assert(BasicResultColumn<arrow::StringArray>(fixture.output, "end_reason")->GetString(0) == "closed");
        fixture.Finish(kPeriodicTestInterval + 200);
        assert(fixture.output->num_rows() == 0);
        fixture.output.reset();
        assert(npm::NpmTrackedBudgetBytes(fixture.budget->Usage()) == 0);
    }
}

void TestNpmBasicTaskRuntimeIsolatesRealtimeTupleReuseAfterPeriodicSnapshots() {
    PeriodicRuntimeFixture fixture(true);
    const auto first = MakeIpv4TcpPacket("192.0.2.1", 50000, "192.0.2.2", 443, {1}, kTcpAck, 100);
    const auto replacement = MakeIpv4TcpPacket("192.0.2.1", 50000, "192.0.2.2", 443, {}, kTcpSyn, 300);
    assert(fixture.Packet(first, 1).error == npm::NpmBasicOfflineBatchError::kNone);
    assert(fixture.Drive(kPeriodicTestInterval).error == npm::NpmBasicRealtimeMaintenanceError::kNone);
    const auto old_id = PeriodicUInt(fixture.output, "session_id");
    assert(fixture.Packet(replacement, kPeriodicTestInterval + 1).error == npm::NpmBasicOfflineBatchError::kNone);
    assert(fixture.runtime->Sessions().size() == 1);
    assert(fixture.Drive(kPeriodicTestInterval + 1).error == npm::NpmBasicRealtimeMaintenanceError::kNone);
    assert(fixture.output->num_rows() == 1 && PeriodicUInt(fixture.output, "session_id") == old_id);
    assert(PeriodicFlag(fixture.output, "is_final"));
    assert(PeriodicUInt(fixture.output, "interval_packets_ab") == 0);
    assert(BasicResultColumn<arrow::StringArray>(fixture.output, "end_reason")->GetString(0) == "tuple_reuse");
    assert(fixture.Drive(2 * kPeriodicTestInterval).error == npm::NpmBasicRealtimeMaintenanceError::kNone);
    assert(PeriodicUInt(fixture.output, "session_id") != old_id);
    assert(PeriodicUInt(fixture.output, "revision") == 1 && PeriodicUInt(fixture.output, "packets_ab") == 1);
    fixture.Finish(2 * kPeriodicTestInterval + 100);
    assert(fixture.output->num_rows() == 1 && PeriodicFlag(fixture.output, "is_final"));
    assert(PeriodicUInt(fixture.output, "session_id") != old_id);
}

void TestNpmBasicTaskRuntimeFlushesRealtimeSessionsAtEofAfterPeriodicSnapshots() {
    for (const auto mode : {npm::NpmRunMode::kOffline, npm::NpmRunMode::kRealtime}) {
        PeriodicRuntimeFixture fixture(true, false, true, mode);
        const auto tcp = MakeIpv4TcpPacket("192.0.2.1", 50000, "192.0.2.2", 443, {1});
        const auto udp = MakeIpv6UdpPacket("2001:db8::1", 53000, "2001:db8::2", 53, {2});
        uint64_t totals[2] = {};
        std::map<uint64_t, uint64_t> interval_sums;
        const auto collect = [&] {
            for (int64_t row = 0; fixture.output && row < fixture.output->num_rows(); ++row)
                interval_sums[PeriodicUInt(fixture.output, "session_id", row)] +=
                    PeriodicUInt(fixture.output, "interval_wire_bytes_total", row);
        };
        for (int period = 0; period < 3; ++period) {
            for (int transport = 0; transport < 2; ++transport) {
                const auto& packet = transport ? udp : tcp;
                assert(fixture.Packet(packet, period * kPeriodicTestInterval + 1, 100 + transport).error ==
                       npm::NpmBasicOfflineBatchError::kNone);
                collect();
                totals[transport] += 100 + transport;
            }
            if (mode == npm::NpmRunMode::kRealtime) {
                assert(fixture.Drive((period + 1) * kPeriodicTestInterval).error ==
                       npm::NpmBasicRealtimeMaintenanceError::kNone);
                collect();
                assert(fixture.output->num_rows() == 2);
                for (int64_t row = 0; row < 2; ++row) {
                    assert(PeriodicUInt(fixture.output, "revision", row) == static_cast<uint64_t>(period + 1));
                    assert(PeriodicUInt(fixture.output, "packets_ab", row) == static_cast<uint64_t>(period + 1));
                }
            }
        }
        fixture.Finish(3 * kPeriodicTestInterval + 100);
        collect();
        assert(fixture.output->num_rows() == 2);
        for (int64_t row = 0; row < 2; ++row) {
            const uint64_t id = PeriodicUInt(fixture.output, "session_id", row);
            assert(PeriodicFlag(fixture.output, "is_final", row));
            assert(PeriodicUInt(fixture.output, "wire_bytes_total", row) == totals[id - 1]);
            assert(interval_sums[id] == totals[id - 1]);
            assert(PeriodicUInt(fixture.output, "packets_ab", row) == 3);
        }
        fixture.output.reset();
        assert(npm::NpmTrackedBudgetBytes(fixture.budget->Usage()) == 0);
    }
}

void TestNpmBasicTaskRuntimeCleansRealtimeBackpressureFailures() {
    PeriodicRuntimeFixture fixture(true);
    const auto packet = MakeIpv6UdpPacket("2001:db8::1", 53000, "2001:db8::2", 53, {1});
    assert(fixture.Packet(packet, 1).error == npm::NpmBasicOfflineBatchError::kNone);
    fixture.output.reset();
    const auto limit = fixture.config.analysis.max_pending_output_bytes;
    assert(fixture.budget->Reserve(npm::NpmBudgetCategory::kPendingOutput, limit) == npm::NpmBudgetError::kNone);
    const auto status = fixture.Drive(kPeriodicTestInterval);
    assert(status.error != npm::NpmBasicRealtimeMaintenanceError::kNone && !fixture.output);
    assert(fixture.runtime->State() == npm::NpmEofFlushState::kFailed && fixture.pool.release_calls == 1);
    assert(fixture.budget->Usage().session_state_bytes == 0 && fixture.budget->Usage().module_state_bytes == 0);
    assert(fixture.budget->Usage().pending_output_bytes == limit);
    assert(fixture.budget->Release(npm::NpmBudgetCategory::kPendingOutput, limit) == npm::NpmBudgetError::kNone);
    assert(npm::NpmTrackedBudgetBytes(fixture.budget->Usage()) == 0);
    assert(fixture.runtime->FlushOffline(100, &fixture.output).error == npm::NpmEofFlushError::kFailedState);
}

void TestNpmBasicTaskRuntimeCancelsSessionStateWithoutTerminalResults() {
    PeriodicRuntimeFixture fixture(true);
    const auto packet = MakeIpv4TcpPacket("192.0.2.1", 50000, "192.0.2.2", 443, {1});
    assert(fixture.Packet(packet, 1).error == npm::NpmBasicOfflineBatchError::kNone);
    assert(fixture.Drive(kPeriodicTestInterval).error == npm::NpmBasicRealtimeMaintenanceError::kNone);
    const auto delivered = fixture.output;
    assert(!PeriodicFlag(delivered, "is_final"));
    fixture.runtime->Cancel();
    fixture.output.reset();
    assert(fixture.pool.release_calls == 1 && fixture.budget->Usage().module_state_bytes == 0);
    assert(fixture.budget->Usage().pending_output_bytes > 0);
    assert(fixture.runtime->FlushOffline(100, &fixture.output).error == npm::NpmEofFlushError::kCancelled);
    assert(!fixture.output && !PeriodicFlag(delivered, "is_final"));
}

void TestNpmBasicTaskRuntimeRetiresRealtimeIdleSessionsOnce() {
    PeriodicRuntimeFixture fixture(true);
    const auto packet = MakeIpv6UdpPacket("2001:db8::1", 53000, "2001:db8::2", 53, {});
    assert(fixture.Packet(packet, 1).error == npm::NpmBasicOfflineBatchError::kNone);
    const int64_t end = fixture.config.analysis.udp_idle_timeout_ns + 1;
    const auto status = fixture.Drive(end);
    assert(status.error == npm::NpmBasicRealtimeMaintenanceError::kNone && status.ended_sessions == 1);
    assert(fixture.runtime->Sessions().size() == 0 && fixture.output->num_rows() == 101);
    uint64_t sum = 0;
    for (int64_t row = 0; row < fixture.output->num_rows(); ++row) {
        sum += PeriodicUInt(fixture.output, "interval_wire_bytes_total", row);
        assert(PeriodicFlag(fixture.output, "is_final", row) == (row == 100));
    }
    assert(sum == packet.bytes.size() && PeriodicUInt(fixture.output, "wire_bytes_total", 100) == sum);
    assert(!PeriodicFlag(fixture.output, "period_complete", 100));
    assert(BasicResultColumn<arrow::StringArray>(fixture.output, "end_reason")->GetString(100) == "idle_timeout");
    assert(fixture.Drive(end + kPeriodicTestInterval).error == npm::NpmBasicRealtimeMaintenanceError::kNone);
    assert(!fixture.output || fixture.output->num_rows() == 0);
    fixture.Finish(end + 100);
    assert(fixture.output->num_rows() == 0);
}

void TestNpmBasicRealtimeTaskIsolationAndSlowSinkBudget() {
    PeriodicRuntimeFixture first, second;
    const auto packet = MakeIpv6UdpPacket("2001:db8::1", 53000, "2001:db8::2", 53, {1});
    assert(first.Packet(packet, 1).error == npm::NpmBasicOfflineBatchError::kNone);
    assert(second.Packet(packet, 1).error == npm::NpmBasicOfflineBatchError::kNone);
    assert(first.Drive(kPeriodicTestInterval).error == npm::NpmBasicRealtimeMaintenanceError::kNone);
    const auto held = first.output;
    assert(second.Drive(kPeriodicTestInterval).error == npm::NpmBasicRealtimeMaintenanceError::kNone);
    assert(PeriodicUInt(first.output, "session_id") == 1 && PeriodicUInt(second.output, "session_id") == 1);
    first.output.reset();
    assert(first.budget->Usage().pending_output_bytes > 0);
    assert(second.Drive(2 * kPeriodicTestInterval).error == npm::NpmBasicRealtimeMaintenanceError::kNone);
    assert(PeriodicUInt(second.output, "revision") == 2 && PeriodicUInt(held, "revision") == 1);
    second.CancelAndCheck();
    first.runtime->Cancel();
    assert(first.budget->Usage().module_state_bytes == 0 && first.budget->Usage().pending_output_bytes > 0);
}

void TestNpmBasicPeriodicOrderDiagnosticsAndStalledBudget() {
    const auto packet = MakeIpv4TcpPacket("192.0.2.1", 50000, "192.0.2.2", 443, {1}, kTcpAck, 100);
    std::vector<std::array<uint64_t, 6>> reference;
    for (const bool disorder : {false, true}) {
        for (const bool split : {false, true}) {
            ContextDictionary dictionary;
            ContextProtocol protocol(&dictionary);
            ContextPool pool(&protocol);
            SinglePoolQuerier querier(&pool);
            auto config = MakeRuntimeTaskConfig();
            config.analysis.result_mode = npm::NpmResultMode::kPeriodicSnapshot;
            config.analysis.output_interval_ns = kPeriodicTestInterval;
            config.analysis.out_of_order_tolerance_ns = kPeriodicTestInterval;
            auto runtime = CreateRuntimeForTest(config, &querier);
            auto budget = runtime->Budget();
            std::vector<flowsql::packet::PacketRecord> records;
            for (const int64_t time :
                 {int64_t{1}, kPeriodicTestInterval, 2 * kPeriodicTestInterval + 1, 3 * kPeriodicTestInterval + 1}) {
                auto record = MakeBatchPacketRecord(packet, 0, time, records.size() + 1);
                record.meta.wire_len = 100;  // capture truncation and observed retransmission use the wire length.
                records.push_back(record);
            }
            if (disorder) std::swap(records[0], records[1]);
            std::vector<std::array<uint64_t, 6>> rows;
            std::shared_ptr<arrow::RecordBatch> output;
            const auto collect = [&] {
                for (int64_t row = 0; row < output->num_rows(); ++row) {
                    rows.push_back({PeriodicUInt(output, "session_id", row), PeriodicUInt(output, "revision", row),
                                    static_cast<uint64_t>(PeriodicInt(output, "period_start_ns", row)),
                                    PeriodicUInt(output, "interval_wire_bytes_total", row),
                                    PeriodicUInt(output, "wire_bytes_total", row),
                                    static_cast<uint64_t>(PeriodicFlag(output, "is_final", row))});
                }
                output.reset();
            };
            if (split) {
                for (const auto& record : records) {
                    assert(runtime->ProcessOfflineBatch(MakeEncodedPacketBatch({record}), &output).error ==
                           npm::NpmBasicOfflineBatchError::kNone);
                    collect();
                }
            } else {
                assert(runtime->ProcessOfflineBatch(MakeEncodedPacketBatch(records), &output).error ==
                       npm::NpmBasicOfflineBatchError::kNone);
                collect();
            }
            assert(runtime->FlushOffline(3 * kPeriodicTestInterval + 100, &output).error ==
                   npm::NpmEofFlushError::kNone);
            collect();
            assert(rows.size() == 4 && rows.back()[4] == 400);
            if (reference.empty())
                reference = rows;
            else
                assert(rows == reference);
            assert(npm::NpmTrackedBudgetBytes(budget->Usage()) == 0);
        }
    }
    {
        PeriodicRuntimeFixture fixture;
        assert(fixture.Packet(packet, 1).error == npm::NpmBasicOfflineBatchError::kNone);
        assert(fixture.Drive(kPeriodicTestInterval).error == npm::NpmBasicRealtimeMaintenanceError::kNone);
        const auto delivered = fixture.output;
        const auto late = fixture.Packet(packet, 1);
        assert(late.error == npm::NpmBasicOfflineBatchError::kBatchProcessError);
        assert(fixture.runtime->State() == npm::NpmEofFlushState::kFailed);
        const auto diagnostic = fixture.runtime->LastError();
        assert(diagnostic.find("source=0") != std::string::npos);
        assert(diagnostic.find("timestamp_ns=1") != std::string::npos);
        assert(diagnostic.find("closed_boundary_ns=10000000") != std::string::npos);
        assert(!PeriodicFlag(delivered, "is_final") && !fixture.output);
        assert(fixture.budget->Usage().session_state_bytes == 0 && fixture.budget->Usage().module_state_bytes == 0);
    }
    {
        PeriodicRuntimeFixture fixture;
        assert(fixture.Packet(packet, 1).error == npm::NpmBasicOfflineBatchError::kNone);
        fixture.output.reset();
        const auto next_input =
            MakeEncodedPacketBatch({MakeBatchPacketRecord(packet, 0, 2 * kPeriodicTestInterval, 2)});
        const auto used = fixture.budget->Usage();
        const uint64_t retained = fixture.config.analysis.max_tracked_bytes - used.session_state_bytes -
                                  used.module_state_bytes -
                                  static_cast<uint64_t>(arrow::util::TotalBufferSize(*next_input));
        assert(fixture.budget->Reserve(npm::NpmBudgetCategory::kModuleState, retained) == npm::NpmBudgetError::kNone);
        assert(fixture.runtime->ProcessOfflineBatch(next_input, &fixture.output).error ==
               npm::NpmBasicOfflineBatchError::kBatchProcessError);
        assert(fixture.runtime->State() == npm::NpmEofFlushState::kFailed && !fixture.output);
        assert(fixture.runtime->LastError().find("budget") != std::string::npos);
        assert(fixture.budget->Usage().module_state_bytes == retained);
        assert(fixture.budget->Release(npm::NpmBudgetCategory::kModuleState, retained) == npm::NpmBudgetError::kNone);
        assert(npm::NpmTrackedBudgetBytes(fixture.budget->Usage()) == 0);
    }
}

void TestNpmBasicTaskRuntimeMatchesLegacyAndParametersV1Realtime() {
    constexpr int64_t interval_ns = 10'000'000;
    constexpr int64_t observed_start_ns = 1'700'000'000'000'000'000LL;
    for (const bool observing_session : {false, true}) {
        auto configs = MakeEquivalentRuntimeTaskConfigs(npm::NpmRunMode::kRealtime, observing_session);
        configs.legacy_json.assign(configs.legacy_json.size(), 'x');
        configs.parameters_v1_json.assign(configs.parameters_v1_json.size(), 'y');

        ContextDictionary dictionary;
        ContextProtocol protocol(&dictionary);
        DualContextPool pool(&protocol);
        SinglePoolQuerier querier(&pool);
        std::shared_ptr<arrow::Schema> legacy_schema;
        std::shared_ptr<arrow::Schema> parameters_schema;
        std::unique_ptr<npm::NpmBasicTaskRuntime> legacy_runtime;
        std::unique_ptr<npm::NpmBasicTaskRuntime> parameters_runtime;
        const auto legacy_create = npm::NpmBasicTaskRuntime::CreateWithTimeCapabilities(
            configs.legacy, &querier, flowsql::packet::PacketSchema(), AllRealtimeTimeCapabilities(), &legacy_schema,
            &legacy_runtime);
        const auto parameters_create = npm::NpmBasicTaskRuntime::CreateWithTimeCapabilities(
            configs.parameters_v1, &querier, flowsql::packet::PacketSchema(), AllRealtimeTimeCapabilities(),
            &parameters_schema, &parameters_runtime);
        assert(legacy_create.error == npm::NpmBasicTaskRuntimeError::kNone);
        assert(parameters_create.error == npm::NpmBasicTaskRuntimeError::kNone);
        assert(legacy_schema != nullptr && parameters_schema != nullptr);
        assert(legacy_schema->Equals(*parameters_schema, true));
        AssertEquivalentTaskConfig(legacy_runtime->Config(), parameters_runtime->Config());
        assert(pool.acquire_calls == 2 && pool.release_calls == 0);

        const auto packet = MakeIpv6UdpPacket("2001:db8::10", 53010, "2001:db8::20", 53, {0x12, 0x34});
        auto packet_view = packet.View(0, 100);
        packet_view.meta.timestamp_ns = 1'000;
        std::vector<npm::NpmSessionSnapshot> legacy_ended;
        std::vector<npm::NpmSessionSnapshot> parameters_ended;
        const auto legacy_process =
            npm::ProcessNpmPacket(legacy_runtime->Config().domains, packet_view, packet.layer,
                                  legacy_runtime->Sessions(), *legacy_runtime->ProtocolContext().Identifier(),
                                  legacy_runtime->Modules(), legacy_runtime->Collector(), &legacy_ended);
        const auto parameters_process =
            npm::ProcessNpmPacket(parameters_runtime->Config().domains, packet_view, packet.layer,
                                  parameters_runtime->Sessions(), *parameters_runtime->ProtocolContext().Identifier(),
                                  parameters_runtime->Modules(), parameters_runtime->Collector(), &parameters_ended);
        assert(legacy_process.error == npm::NpmPacketProcessError::kNone);
        assert(parameters_process.error == legacy_process.error);
        assert(parameters_process.binding_error == legacy_process.binding_error);
        assert(parameters_process.session_error == legacy_process.session_error);
        assert(parameters_process.module_error == legacy_process.module_error);
        assert(legacy_ended.empty() && parameters_ended.empty());
        assert(legacy_runtime->Sessions().size() == 1);
        assert(parameters_runtime->Sessions().size() == 1);
        AssertEquivalentBudgetUsage(legacy_runtime->Budget()->Usage(), parameters_runtime->Budget()->Usage());

        auto legacy_output = MakeNpmPacketViewBatch();
        auto parameters_output = MakeNpmPacketViewBatch();
        const auto legacy_sentinel = legacy_output;
        const auto parameters_sentinel = parameters_output;
        auto legacy_status = legacy_runtime->DriveRealtimeMaintenance(
            RealtimeMaintenanceInput(100, observed_start_ns, packet_view.meta.timestamp_ns, true, false, true, false),
            &legacy_output);
        auto parameters_status = parameters_runtime->DriveRealtimeMaintenance(
            RealtimeMaintenanceInput(100, observed_start_ns, packet_view.meta.timestamp_ns, true, false, true, false),
            &parameters_output);
        AssertEquivalentRealtimeStatus(legacy_status, parameters_status);
        assert(legacy_status.error == npm::NpmBasicRealtimeMaintenanceError::kNone);
        assert(!legacy_status.snapshot_due && !legacy_status.emitted);
        assert(legacy_output == legacy_sentinel && parameters_output == parameters_sentinel);

        legacy_status = legacy_runtime->DriveRealtimeMaintenance(
            RealtimeMaintenanceInput(100 + interval_ns, observed_start_ns + interval_ns,
                                     interval_ns + configs.legacy.analysis.out_of_order_tolerance_ns, false, true, true,
                                     false),
            &legacy_output);
        parameters_status = parameters_runtime->DriveRealtimeMaintenance(
            RealtimeMaintenanceInput(100 + interval_ns, observed_start_ns + interval_ns,
                                     interval_ns + configs.legacy.analysis.out_of_order_tolerance_ns, false, true, true,
                                     false),
            &parameters_output);
        AssertEquivalentRealtimeStatus(legacy_status, parameters_status);
        assert(legacy_status.error == npm::NpmBasicRealtimeMaintenanceError::kNone);
        assert(legacy_status.snapshot_due && legacy_status.emitted);
        assert(legacy_status.active_sessions == 1 && legacy_status.ended_sessions == 0);
        AssertEquivalentRecordBatch(legacy_output, parameters_output);
        assert(legacy_output->num_rows() == 1);
        if (observing_session) {
            assert(SessionResultColumn<arrow::UInt64Array>(legacy_output, 2)->Value(0) == 1);
        } else {
            assert(BasicResultColumn<arrow::UInt64Array>(legacy_output, 2)->Value(0) == 1);
        }
        AssertEquivalentBudgetUsage(legacy_runtime->Budget()->Usage(), parameters_runtime->Budget()->Usage());
        assert(legacy_runtime->Budget()->Usage().pending_output_bytes > 0);

        legacy_runtime->Cancel();
        parameters_runtime->Cancel();
        assert(legacy_runtime->State() == npm::NpmEofFlushState::kCancelled);
        assert(parameters_runtime->State() == npm::NpmEofFlushState::kCancelled);
        assert(pool.release_calls == 2);
        AssertEquivalentBudgetUsage(legacy_runtime->Budget()->Usage(), parameters_runtime->Budget()->Usage());
        legacy_output.reset();
        parameters_output.reset();
        AssertTaskBudgetUsage(legacy_runtime->Budget()->Usage(), 0, 0, 0, 0);
        AssertTaskBudgetUsage(parameters_runtime->Budget()->Usage(), 0, 0, 0, 0);
    }
}

void TestNpmBasicTaskRuntimeCleansSessionModuleFailures() {
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);

    auto both_basic = MakeRuntimeTaskConfig();
    both_basic.features.session_enabled = true;
    both_basic.features.session_max_tcp_ranges_per_direction = 8;
    auto session_only = both_basic;
    session_only.features.basic_enabled = false;
    session_only.features.observing = npm::NpmResultEntity::kSession;
    auto both_session = both_basic;
    both_session.features.observing = npm::NpmResultEntity::kSession;
    const std::array<npm::NpmBasicTaskConfig, 3> configs{both_basic, session_only, both_session};

    for (size_t index = 0; index < configs.size(); ++index) {
        const auto& config = configs[index];
        const bool observing_session = config.features.observing == npm::NpmResultEntity::kSession;
        const auto expected_schema = observing_session ? npm::NpmSessionResultSchema() : npm::NpmBasicResultSchema();
        std::shared_ptr<arrow::Schema> output_schema;
        std::unique_ptr<npm::NpmBasicTaskRuntime> runtime;
        const auto create_status = npm::NpmBasicTaskRuntime::Create(config, &querier, flowsql::packet::PacketSchema(),
                                                                    &output_schema, &runtime);
        assert(create_status.error == npm::NpmBasicTaskRuntimeError::kNone);
        assert(runtime != nullptr && output_schema.get() == expected_schema.get());
        assert(runtime->Modules().size() == 1);
        assert(pool.acquire_calls == static_cast<int>(index + 1));
        assert(pool.release_calls == static_cast<int>(index));

        std::vector<flowsql::packet::PacketRecord> records;
        for (uint32_t packet_index = 0; packet_index < 5; ++packet_index) {
            const auto packet =
                MakeIpv4TcpPacket("192.0.2.1", 41000, "198.51.100.2", 443, std::vector<uint8_t>(10, 0x11), kTcpAck,
                                  100 + packet_index * 20, 1, 1024);
            records.push_back(
                MakeBatchPacketRecord(packet, 0, 100 + static_cast<int64_t>(packet_index), packet_index + 1));
        }
        auto input = MakeEncodedPacketBatch(records);
        std::shared_ptr<arrow::RecordBatch> output = MakeNpmPacketViewBatch();
        const auto original_output = output;
        auto budget = runtime->Budget();

        auto status = runtime->ProcessOfflineBatch(input, &output);
        assert(status.error == npm::NpmBasicOfflineBatchError::kBatchProcessError);
        assert(status.runtime_state == npm::NpmEofFlushState::kFailed);
        assert(status.process_status.error == npm::NpmPacketBatchProcessError::kPacketError);
        assert(status.process_status.row == 4);
        assert(status.process_status.packet_status.error == npm::NpmPacketProcessError::kModuleError);
        assert(status.process_status.packet_status.module_error == ENOSPC);
        assert(output == original_output && runtime->State() == npm::NpmEofFlushState::kFailed);
        const auto first_error = runtime->LastError();
        assert(!first_error.empty() && pool.release_calls == static_cast<int>(index + 1));
        AssertTaskBudgetUsage(budget->Usage(), 0, 0, 0, 0);

        status = runtime->ProcessOfflineBatch(input, &output);
        assert(status.error == npm::NpmBasicOfflineBatchError::kTerminalState);
        assert(status.runtime_state == npm::NpmEofFlushState::kFailed);
        const auto flush_status = runtime->FlushOffline(1'000, &output);
        assert(flush_status.error == npm::NpmEofFlushError::kFailedState);
        runtime->Cancel();
        assert(runtime->State() == npm::NpmEofFlushState::kFailed);
        assert(runtime->LastError() == first_error && output == original_output);
        AssertTaskBudgetUsage(budget->Usage(), 0, 0, 0, 0);
    }
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

    const auto rst = MakeIpv4TcpPacket("192.0.2.1", 41000, "198.51.100.2", 443, {}, kTcpRst, 91);
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
    assert(output->num_rows() == 1 && output->num_columns() == 32);
    assert(BasicResultColumn<arrow::BooleanArray>(output, 4)->Value(0));
    assert(BasicResultColumn<arrow::StringArray>(output, "protocol_status")->GetString(0) == "unknown");
    assert(BasicResultColumn<arrow::StringArray>(output, "end_reason")->GetString(0) == "closed");
    assert(runtime->Sessions().size() == 0 && runtime->Collector().pending_results() == 0);
    assert(runtime->Projector().tracked_sessions() == 0);

    const uint64_t output_bytes = ResultBufferCapacity(output);
    AssertTaskBudgetUsage(runtime->Budget()->Usage(), 0, 0, 0, output_bytes);
    input.reset();
    assert(input_owner.expired());
    assert(BasicResultColumn<arrow::StringArray>(output, "end_reason")->GetString(0) == "closed");
    output.reset();
    AssertTaskBudgetUsage(runtime->Budget()->Usage(), 0, 0, 0, 0);

    auto empty_input = MakeEncodedPacketBatch({});
    status = runtime->ProcessOfflineBatch(empty_input, &output);
    assert(status.error == npm::NpmBasicOfflineBatchError::kNone);
    assert(status.runtime_state == npm::NpmEofFlushState::kOpen);
    assert(output != nullptr && output->schema()->Equals(*npm::NpmBasicResultSchema(), true));
    assert(output->num_rows() == 0 && output->num_columns() == 32);
    output.reset();
    AssertTaskBudgetUsage(runtime->Budget()->Usage(), 0, 0, 0, 0);
}

void TestNpmBasicTaskRuntimeProcessesImmediateClosedSession() {
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    auto config = MakeRuntimeTaskConfig();
    config.features.basic_enabled = false;
    config.features.session_enabled = true;
    config.features.observing = npm::NpmResultEntity::kSession;

    std::shared_ptr<arrow::Schema> output_schema;
    std::unique_ptr<npm::NpmBasicTaskRuntime> runtime;
    const auto create_status =
        npm::NpmBasicTaskRuntime::Create(config, &querier, flowsql::packet::PacketSchema(), &output_schema, &runtime);
    assert(create_status.error == npm::NpmBasicTaskRuntimeError::kNone);
    assert(runtime != nullptr && output_schema.get() == npm::NpmSessionResultSchema().get());
    assert(runtime->Modules().size() == 1);

    const auto rst = MakeIpv4TcpPacket("192.0.2.1", 41000, "198.51.100.2", 443, {}, kTcpRst, 91);
    auto input = MakeEncodedPacketBatch({MakeBatchPacketRecord(rst, 0, 100, 1)});
    std::shared_ptr<arrow::RecordBatch> output = MakeNpmPacketViewBatch();
    const auto status = runtime->ProcessOfflineBatch(input, &output);
    assert(status.error == npm::NpmBasicOfflineBatchError::kNone);
    assert(status.runtime_state == npm::NpmEofFlushState::kOpen);
    assert(status.process_status.error == npm::NpmPacketBatchProcessError::kNone);
    assert(status.drain_status.error == npm::NpmBasicDrainError::kNone);
    assert(output != nullptr && output->schema().get() == npm::NpmSessionResultSchema().get());
    assert(output->num_rows() == 1 && output->num_columns() == 49);
    assert(SessionResultColumn<arrow::BooleanArray>(output, 4)->Value(0));
    assert(SessionResultColumn<arrow::UInt8Array>(output, 6)->Value(0) == 6);
    assert(SessionResultColumn<arrow::StringArray>(output, 18)->GetString(0) == "closed");
    const auto packets_ab = SessionResultColumn<arrow::UInt64Array>(output, 19)->Value(0);
    const auto packets_ba = SessionResultColumn<arrow::UInt64Array>(output, 20)->Value(0);
    const auto wire_bytes_ab = SessionResultColumn<arrow::UInt64Array>(output, 21)->Value(0);
    const auto wire_bytes_ba = SessionResultColumn<arrow::UInt64Array>(output, 22)->Value(0);
    assert(packets_ab + packets_ba == 1);
    assert(wire_bytes_ab + wire_bytes_ba == rst.bytes.size());
    assert(runtime->Sessions().size() == 0 && runtime->Collector().pending_results() == 0);
    auto* session_module = dynamic_cast<npm::NpmSessionAnalysisModule*>(runtime->Modules()[0]);
    assert(session_module != nullptr && session_module->tracked_sessions() == 0);
    auto budget = runtime->Budget();
    AssertTaskBudgetUsage(budget->Usage(), 0, 0, 0, ResultBufferCapacity(output));

    output.reset();
    AssertTaskBudgetUsage(budget->Usage(), 0, 0, 0, 0);
    runtime->Cancel();
    assert(runtime->State() == npm::NpmEofFlushState::kCancelled && pool.release_calls == 1);
}

void TestNpmBasicTaskRuntimeClosesActiveSessionsAcrossFeatureSelections() {
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);

    auto basic_only = MakeRuntimeTaskConfig();
    auto both_basic = MakeRuntimeTaskConfig();
    both_basic.features.session_enabled = true;
    auto session_only = MakeRuntimeTaskConfig();
    session_only.features.basic_enabled = false;
    session_only.features.session_enabled = true;
    session_only.features.observing = npm::NpmResultEntity::kSession;
    auto both_session = MakeRuntimeTaskConfig();
    both_session.features.session_enabled = true;
    both_session.features.observing = npm::NpmResultEntity::kSession;
    const std::array<npm::NpmBasicTaskConfig, 4> configs{basic_only, both_basic, session_only, both_session};

    for (size_t index = 0; index < configs.size(); ++index) {
        const auto& config = configs[index];
        const bool observing_session = config.features.observing == npm::NpmResultEntity::kSession;
        const auto expected_schema = observing_session ? npm::NpmSessionResultSchema() : npm::NpmBasicResultSchema();
        const int expected_columns = observing_session ? 49 : 32;
        const int64_t active_timestamp_ns = 100 + static_cast<int64_t>(index);
        const int64_t closed_timestamp_ns = 200 + static_cast<int64_t>(index);

        std::shared_ptr<arrow::Schema> output_schema;
        std::unique_ptr<npm::NpmBasicTaskRuntime> runtime;
        const auto create_status = npm::NpmBasicTaskRuntime::Create(config, &querier, flowsql::packet::PacketSchema(),
                                                                    &output_schema, &runtime);
        assert(create_status.error == npm::NpmBasicTaskRuntimeError::kNone);
        assert(runtime != nullptr && output_schema.get() == expected_schema.get());
        assert(pool.acquire_calls == static_cast<int>(index + 1));
        assert(pool.release_calls == static_cast<int>(index));

        npm::NpmSessionAnalysisModule* session_module = nullptr;
        if (config.features.session_enabled) {
            assert(runtime->Modules().size() == 1);
            session_module = dynamic_cast<npm::NpmSessionAnalysisModule*>(runtime->Modules()[0]);
            assert(session_module != nullptr);
        } else {
            assert(runtime->Modules().empty());
        }

        const std::vector<uint8_t> active_payload{0x10, 0x11, 0x12};
        const auto active_packet =
            MakeIpv4TcpPacket("192.0.2.1", 41000, "198.51.100.2", 443, active_payload, kTcpAck, 100, 90, 2048);
        auto active_input =
            MakeEncodedPacketBatch({MakeBatchPacketRecord(active_packet, 0, active_timestamp_ns, 2 * index + 1)});
        std::weak_ptr<arrow::RecordBatch> active_input_owner = active_input;
        std::shared_ptr<arrow::RecordBatch> output = MakeNpmPacketViewBatch();
        auto status = runtime->ProcessOfflineBatch(active_input, &output);
        assert(status.error == npm::NpmBasicOfflineBatchError::kNone);
        assert(status.runtime_state == npm::NpmEofFlushState::kOpen);
        assert(status.process_status.error == npm::NpmPacketBatchProcessError::kNone);
        assert(status.drain_status.error == npm::NpmBasicDrainError::kNone);
        assert(output != nullptr && output->schema().get() == expected_schema.get());
        assert(output->num_rows() == 0 && output->num_columns() == expected_columns);
        assert(runtime->Sessions().size() == 1 && runtime->Collector().pending_results() == 0);
        assert(runtime->Projector().tracked_sessions() == 0);

        const uint64_t session_bytes = runtime->Sessions().tracked_bytes();
        const uint64_t module_bytes = session_module == nullptr ? 0 : session_module->tracked_bytes();
        assert(session_bytes > 0);
        if (session_module != nullptr) {
            assert(session_module->tracked_sessions() == 1 && module_bytes > 0);
        }
        auto budget = runtime->Budget();
        AssertTaskBudgetUsage(budget->Usage(), session_bytes, module_bytes, 0, ResultBufferCapacity(output));
        active_input.reset();
        assert(active_input_owner.expired());
        output.reset();
        AssertTaskBudgetUsage(budget->Usage(), session_bytes, module_bytes, 0, 0);

        const std::vector<uint8_t> closed_payload{0x20, 0x21};
        const auto closed_packet = MakeIpv4TcpPacket("198.51.100.2", 443, "192.0.2.1", 41000, closed_payload,
                                                     kTcpRst | kTcpAck, 500, 103, 4096);
        auto closed_input =
            MakeEncodedPacketBatch({MakeBatchPacketRecord(closed_packet, 0, closed_timestamp_ns, 2 * index + 2)});
        std::weak_ptr<arrow::RecordBatch> closed_input_owner = closed_input;
        const auto closed_start_ns = ResultClockNowNs();
        status = runtime->ProcessOfflineBatch(closed_input, &output);
        const auto closed_end_ns = ResultClockNowNs();
        assert(status.error == npm::NpmBasicOfflineBatchError::kNone);
        assert(status.runtime_state == npm::NpmEofFlushState::kOpen);
        assert(status.process_status.error == npm::NpmPacketBatchProcessError::kNone);
        assert(status.drain_status.error == npm::NpmBasicDrainError::kNone);
        assert(output != nullptr && output->schema().get() == expected_schema.get());
        assert(output->num_rows() == 1 && output->num_columns() == expected_columns);

        if (observing_session) {
            assert(SessionResultColumn<arrow::UInt64Array>(output, 2)->Value(0) == 1);
            assert(SessionResultColumn<arrow::Int64Array>(output, 3)->Value(0) == closed_timestamp_ns);
            assert(SessionResultColumn<arrow::BooleanArray>(output, 4)->Value(0));
            assert(SessionResultColumn<arrow::Int64Array>(output, 11)->Value(0) == active_timestamp_ns);
            assert(SessionResultColumn<arrow::Int64Array>(output, 12)->Value(0) == closed_timestamp_ns);
            assert(SessionResultColumn<arrow::StringArray>(output, 18)->GetString(0) == "closed");
            assert(SessionResultColumn<arrow::UInt64Array>(output, 19)->Value(0) == 1);
            assert(SessionResultColumn<arrow::UInt64Array>(output, 20)->Value(0) == 1);
            assert(SessionResultColumn<arrow::UInt64Array>(output, 21)->Value(0) == active_packet.bytes.size());
            assert(SessionResultColumn<arrow::UInt64Array>(output, 22)->Value(0) == closed_packet.bytes.size());
            assert(SessionResultColumn<arrow::UInt64Array>(output, 23)->Value(0) == active_payload.size());
            assert(SessionResultColumn<arrow::UInt64Array>(output, 24)->Value(0) == closed_payload.size());
        } else {
            assert(BasicResultColumn<arrow::UInt64Array>(output, 2)->Value(0) == 1);
            const auto generated_at = BasicResultColumn<arrow::Int64Array>(output, 3)->Value(0);
            assert(generated_at >= closed_start_ns && generated_at <= closed_end_ns);
            assert(BasicResultColumn<arrow::BooleanArray>(output, 4)->Value(0));
            assert(BasicResultColumn<arrow::Int64Array>(output, 11)->Value(0) == active_timestamp_ns);
            assert(BasicResultColumn<arrow::Int64Array>(output, 12)->Value(0) == closed_timestamp_ns);
            assert(BasicResultColumn<arrow::UInt64Array>(output, 13)->Value(0) == 1);
            assert(BasicResultColumn<arrow::UInt64Array>(output, 14)->Value(0) == 1);
            assert(BasicResultColumn<arrow::UInt64Array>(output, 15)->Value(0) == active_packet.bytes.size());
            assert(BasicResultColumn<arrow::UInt64Array>(output, 16)->Value(0) == closed_packet.bytes.size());
            assert(BasicResultColumn<arrow::StringArray>(output, "end_reason")->GetString(0) == "closed");
        }

        assert(runtime->Sessions().size() == 0 && runtime->Collector().pending_results() == 0);
        assert(runtime->Projector().tracked_sessions() == 0);
        if (session_module != nullptr) {
            assert(session_module->tracked_sessions() == 0 && session_module->tracked_bytes() == 0);
        }
        AssertTaskBudgetUsage(budget->Usage(), 0, 0, 0, ResultBufferCapacity(output));
        closed_input.reset();
        assert(closed_input_owner.expired());
        output.reset();
        AssertTaskBudgetUsage(budget->Usage(), 0, 0, 0, 0);

        auto empty_input = MakeEncodedPacketBatch({});
        status = runtime->ProcessOfflineBatch(empty_input, &output);
        assert(status.error == npm::NpmBasicOfflineBatchError::kNone);
        assert(status.runtime_state == npm::NpmEofFlushState::kOpen);
        assert(status.process_status.error == npm::NpmPacketBatchProcessError::kNone);
        assert(status.drain_status.error == npm::NpmBasicDrainError::kNone);
        assert(output != nullptr && output->schema().get() == expected_schema.get());
        assert(output->num_rows() == 0 && output->num_columns() == expected_columns);
        assert(runtime->Sessions().size() == 0 && runtime->Collector().pending_results() == 0);
        assert(runtime->Projector().tracked_sessions() == 0);
        if (session_module != nullptr) {
            assert(session_module->tracked_sessions() == 0 && session_module->tracked_bytes() == 0);
        }
        AssertTaskBudgetUsage(budget->Usage(), 0, 0, 0, ResultBufferCapacity(output));
        empty_input.reset();
        output.reset();
        AssertTaskBudgetUsage(budget->Usage(), 0, 0, 0, 0);

        runtime->Cancel();
        assert(runtime->State() == npm::NpmEofFlushState::kCancelled);
        assert(pool.release_calls == static_cast<int>(index + 1));
    }
}

void TestNpmBasicTaskRuntimeIsolatesTupleReuseAcrossFeatureSelections() {
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);

    auto basic_only = MakeRuntimeTaskConfig();
    auto both_basic = MakeRuntimeTaskConfig();
    both_basic.features.session_enabled = true;
    auto session_only = MakeRuntimeTaskConfig();
    session_only.features.basic_enabled = false;
    session_only.features.session_enabled = true;
    session_only.features.observing = npm::NpmResultEntity::kSession;
    auto both_session = MakeRuntimeTaskConfig();
    both_session.features.session_enabled = true;
    both_session.features.observing = npm::NpmResultEntity::kSession;
    const std::array<npm::NpmBasicTaskConfig, 4> configs{basic_only, both_basic, session_only, both_session};

    for (size_t index = 0; index < configs.size(); ++index) {
        const auto& config = configs[index];
        const bool observing_session = config.features.observing == npm::NpmResultEntity::kSession;
        const auto expected_schema = observing_session ? npm::NpmSessionResultSchema() : npm::NpmBasicResultSchema();
        const int expected_columns = observing_session ? 49 : 32;
        const int64_t old_timestamp_ns = 100 + static_cast<int64_t>(index);
        const int64_t reuse_timestamp_ns = 200 + static_cast<int64_t>(index);
        const int64_t closed_timestamp_ns = 300 + static_cast<int64_t>(index);

        std::shared_ptr<arrow::Schema> output_schema;
        std::unique_ptr<npm::NpmBasicTaskRuntime> runtime;
        const auto create_status = npm::NpmBasicTaskRuntime::Create(config, &querier, flowsql::packet::PacketSchema(),
                                                                    &output_schema, &runtime);
        assert(create_status.error == npm::NpmBasicTaskRuntimeError::kNone);
        assert(runtime != nullptr && output_schema.get() == expected_schema.get());
        assert(pool.acquire_calls == static_cast<int>(index + 1));
        assert(pool.release_calls == static_cast<int>(index));

        npm::NpmSessionAnalysisModule* session_module = nullptr;
        if (config.features.session_enabled) {
            assert(runtime->Modules().size() == 1);
            session_module = dynamic_cast<npm::NpmSessionAnalysisModule*>(runtime->Modules()[0]);
            assert(session_module != nullptr);
        } else {
            assert(runtime->Modules().empty());
        }

        const std::vector<uint8_t> old_payload{0x10, 0x11, 0x12};
        const auto old_packet =
            MakeIpv4TcpPacket("192.0.2.1", 41000, "198.51.100.2", 443, old_payload, kTcpAck, 100, 90, 2048);
        auto old_input =
            MakeEncodedPacketBatch({MakeBatchPacketRecord(old_packet, 0, old_timestamp_ns, 3 * index + 1)});
        std::weak_ptr<arrow::RecordBatch> old_input_owner = old_input;
        std::shared_ptr<arrow::RecordBatch> output = MakeNpmPacketViewBatch();
        auto status = runtime->ProcessOfflineBatch(old_input, &output);
        assert(status.error == npm::NpmBasicOfflineBatchError::kNone);
        assert(status.runtime_state == npm::NpmEofFlushState::kOpen);
        assert(status.process_status.error == npm::NpmPacketBatchProcessError::kNone);
        assert(status.drain_status.error == npm::NpmBasicDrainError::kNone);
        assert(output != nullptr && output->schema().get() == expected_schema.get());
        assert(output->num_rows() == 0 && output->num_columns() == expected_columns);
        assert(runtime->Sessions().size() == 1 && runtime->Collector().pending_results() == 0);
        assert(runtime->Projector().tracked_sessions() == 0);
        if (session_module != nullptr) {
            assert(session_module->tracked_sessions() == 1 && session_module->tracked_bytes() > 0);
        }

        auto budget = runtime->Budget();
        AssertTaskBudgetUsage(budget->Usage(), runtime->Sessions().tracked_bytes(),
                              session_module == nullptr ? 0 : session_module->tracked_bytes(), 0,
                              ResultBufferCapacity(output));
        old_input.reset();
        assert(old_input_owner.expired());
        output.reset();
        AssertTaskBudgetUsage(budget->Usage(), runtime->Sessions().tracked_bytes(),
                              session_module == nullptr ? 0 : session_module->tracked_bytes(), 0, 0);

        const std::vector<uint8_t> replacement_payload{0x20, 0x21};
        const auto replacement_packet =
            MakeIpv4TcpPacket("192.0.2.1", 41000, "198.51.100.2", 443, replacement_payload, kTcpSyn, 200, 0, 4096);
        auto replacement_input =
            MakeEncodedPacketBatch({MakeBatchPacketRecord(replacement_packet, 0, reuse_timestamp_ns, 3 * index + 2)});
        std::weak_ptr<arrow::RecordBatch> replacement_input_owner = replacement_input;
        const auto reuse_start_ns = ResultClockNowNs();
        status = runtime->ProcessOfflineBatch(replacement_input, &output);
        const auto reuse_end_ns = ResultClockNowNs();
        assert(status.error == npm::NpmBasicOfflineBatchError::kNone);
        assert(status.runtime_state == npm::NpmEofFlushState::kOpen);
        assert(status.process_status.error == npm::NpmPacketBatchProcessError::kNone);
        assert(status.drain_status.error == npm::NpmBasicDrainError::kNone);
        assert(output != nullptr && output->schema().get() == expected_schema.get());
        assert(output->num_rows() == 1 && output->num_columns() == expected_columns);

        if (observing_session) {
            assert(SessionResultColumn<arrow::UInt64Array>(output, 0)->Value(0) == 1);
            assert(SessionResultColumn<arrow::UInt64Array>(output, 2)->Value(0) == 1);
            assert(SessionResultColumn<arrow::Int64Array>(output, 3)->Value(0) == reuse_timestamp_ns);
            assert(SessionResultColumn<arrow::BooleanArray>(output, 4)->Value(0));
            assert(SessionResultColumn<arrow::Int64Array>(output, 11)->Value(0) == old_timestamp_ns);
            assert(SessionResultColumn<arrow::Int64Array>(output, 12)->Value(0) == old_timestamp_ns);
            assert(SessionResultColumn<arrow::StringArray>(output, 14)->GetString(0) == "identified");
            assert(SessionResultColumn<arrow::UInt16Array>(output, 15)->Value(0) == 7);
            assert(SessionResultColumn<arrow::UInt16Array>(output, 16)->Value(0) == 8);
            assert(SessionResultColumn<arrow::StringArray>(output, 17)->GetString(0) == "SUB");
            assert(SessionResultColumn<arrow::StringArray>(output, 18)->GetString(0) == "tuple_reuse");
            assert(SessionResultColumn<arrow::UInt64Array>(output, 19)->Value(0) == 1);
            assert(SessionResultColumn<arrow::UInt64Array>(output, 20)->Value(0) == 0);
            assert(SessionResultColumn<arrow::UInt64Array>(output, 21)->Value(0) == old_packet.bytes.size());
            assert(SessionResultColumn<arrow::UInt64Array>(output, 22)->Value(0) == 0);
            assert(SessionResultColumn<arrow::UInt64Array>(output, 23)->Value(0) == old_payload.size());
            assert(SessionResultColumn<arrow::UInt64Array>(output, 24)->Value(0) == 0);
        } else {
            assert(BasicResultColumn<arrow::UInt64Array>(output, 0)->Value(0) == 1);
            assert(BasicResultColumn<arrow::UInt64Array>(output, 2)->Value(0) == 1);
            const auto generated_at = BasicResultColumn<arrow::Int64Array>(output, 3)->Value(0);
            assert(generated_at >= reuse_start_ns && generated_at <= reuse_end_ns);
            assert(BasicResultColumn<arrow::BooleanArray>(output, 4)->Value(0));
            assert(BasicResultColumn<arrow::Int64Array>(output, 11)->Value(0) == old_timestamp_ns);
            assert(BasicResultColumn<arrow::Int64Array>(output, 12)->Value(0) == old_timestamp_ns);
            assert(BasicResultColumn<arrow::UInt64Array>(output, 13)->Value(0) == 1);
            assert(BasicResultColumn<arrow::UInt64Array>(output, 14)->Value(0) == 0);
            assert(BasicResultColumn<arrow::UInt64Array>(output, 15)->Value(0) == old_packet.bytes.size());
            assert(BasicResultColumn<arrow::UInt64Array>(output, 16)->Value(0) == 0);
            assert(BasicResultColumn<arrow::StringArray>(output, "protocol_status")->GetString(0) == "identified");
            assert(BasicResultColumn<arrow::UInt16Array>(output, "protocol_id")->Value(0) == 7);
            assert(BasicResultColumn<arrow::UInt16Array>(output, "protocol_sub_id")->Value(0) == 8);
            assert(BasicResultColumn<arrow::StringArray>(output, "protocol")->GetString(0) == "SUB");
            assert(BasicResultColumn<arrow::StringArray>(output, "end_reason")->GetString(0) == "tuple_reuse");
        }

        replacement_input.reset();
        assert(replacement_input_owner.expired());
        assert(runtime->Sessions().size() == 1 && runtime->Collector().pending_results() == 0);
        assert(runtime->Projector().tracked_sessions() == 0);
        std::vector<npm::NpmSessionView> active;
        assert(runtime->Sessions().SnapshotActive(&active) == npm::NpmSessionTableError::kNone);
        assert(active.size() == 1 && active[0].session_id == 2);
        assert(active[0].first_ns == reuse_timestamp_ns && active[0].last_ns == reuse_timestamp_ns);
        assert(active[0].packets_ab == 1 && active[0].packets_ba == 0);
        assert(active[0].wire_bytes_ab == replacement_packet.bytes.size());
        assert(active[0].wire_bytes_ba == 0);
        assert(active[0].protocol_status == npm::NpmProtocolStatus::kIdentified);
        assert(active[0].protocol_id == 7 && active[0].protocol_sub_id == 8);
        if (session_module != nullptr) {
            assert(session_module->tracked_sessions() == 1 && session_module->tracked_bytes() > 0);
        }
        const uint64_t old_output_bytes = ResultBufferCapacity(output);
        AssertTaskBudgetUsage(budget->Usage(), runtime->Sessions().tracked_bytes(),
                              session_module == nullptr ? 0 : session_module->tracked_bytes(), 0, old_output_bytes);
        auto old_output = output;

        const std::vector<uint8_t> closed_payload{0x30};
        const auto closed_packet = MakeIpv4TcpPacket("198.51.100.2", 443, "192.0.2.1", 41000, closed_payload,
                                                     kTcpRst | kTcpAck, 500, 203, 1024);
        auto closed_input =
            MakeEncodedPacketBatch({MakeBatchPacketRecord(closed_packet, 0, closed_timestamp_ns, 3 * index + 3)});
        std::weak_ptr<arrow::RecordBatch> closed_input_owner = closed_input;
        const auto closed_start_ns = ResultClockNowNs();
        status = runtime->ProcessOfflineBatch(closed_input, &output);
        const auto closed_end_ns = ResultClockNowNs();
        assert(status.error == npm::NpmBasicOfflineBatchError::kNone);
        assert(status.runtime_state == npm::NpmEofFlushState::kOpen);
        assert(status.process_status.error == npm::NpmPacketBatchProcessError::kNone);
        assert(status.drain_status.error == npm::NpmBasicDrainError::kNone);
        assert(output != nullptr && output->schema().get() == expected_schema.get());
        assert(output->num_rows() == 1 && output->num_columns() == expected_columns);

        if (observing_session) {
            assert(SessionResultColumn<arrow::UInt64Array>(output, 0)->Value(0) == 2);
            assert(SessionResultColumn<arrow::UInt64Array>(output, 2)->Value(0) == 1);
            assert(SessionResultColumn<arrow::Int64Array>(output, 3)->Value(0) == closed_timestamp_ns);
            assert(SessionResultColumn<arrow::BooleanArray>(output, 4)->Value(0));
            assert(SessionResultColumn<arrow::Int64Array>(output, 11)->Value(0) == reuse_timestamp_ns);
            assert(SessionResultColumn<arrow::Int64Array>(output, 12)->Value(0) == closed_timestamp_ns);
            assert(SessionResultColumn<arrow::StringArray>(output, 14)->GetString(0) == "identified");
            assert(SessionResultColumn<arrow::UInt16Array>(output, 15)->Value(0) == 7);
            assert(SessionResultColumn<arrow::UInt16Array>(output, 16)->Value(0) == 8);
            assert(SessionResultColumn<arrow::StringArray>(output, 17)->GetString(0) == "SUB");
            assert(SessionResultColumn<arrow::StringArray>(output, 18)->GetString(0) == "closed");
            assert(SessionResultColumn<arrow::UInt64Array>(output, 19)->Value(0) == 1);
            assert(SessionResultColumn<arrow::UInt64Array>(output, 20)->Value(0) == 1);
            assert(SessionResultColumn<arrow::UInt64Array>(output, 21)->Value(0) == replacement_packet.bytes.size());
            assert(SessionResultColumn<arrow::UInt64Array>(output, 22)->Value(0) == closed_packet.bytes.size());
            assert(SessionResultColumn<arrow::UInt64Array>(output, 23)->Value(0) == replacement_payload.size());
            assert(SessionResultColumn<arrow::UInt64Array>(output, 24)->Value(0) == closed_payload.size());
        } else {
            assert(BasicResultColumn<arrow::UInt64Array>(output, 0)->Value(0) == 2);
            assert(BasicResultColumn<arrow::UInt64Array>(output, 2)->Value(0) == 1);
            const auto generated_at = BasicResultColumn<arrow::Int64Array>(output, 3)->Value(0);
            assert(generated_at >= closed_start_ns && generated_at <= closed_end_ns);
            assert(BasicResultColumn<arrow::BooleanArray>(output, 4)->Value(0));
            assert(BasicResultColumn<arrow::Int64Array>(output, 11)->Value(0) == reuse_timestamp_ns);
            assert(BasicResultColumn<arrow::Int64Array>(output, 12)->Value(0) == closed_timestamp_ns);
            assert(BasicResultColumn<arrow::UInt64Array>(output, 13)->Value(0) == 1);
            assert(BasicResultColumn<arrow::UInt64Array>(output, 14)->Value(0) == 1);
            assert(BasicResultColumn<arrow::UInt64Array>(output, 15)->Value(0) == replacement_packet.bytes.size());
            assert(BasicResultColumn<arrow::UInt64Array>(output, 16)->Value(0) == closed_packet.bytes.size());
            assert(BasicResultColumn<arrow::StringArray>(output, "protocol_status")->GetString(0) == "identified");
            assert(BasicResultColumn<arrow::UInt16Array>(output, "protocol_id")->Value(0) == 7);
            assert(BasicResultColumn<arrow::UInt16Array>(output, "protocol_sub_id")->Value(0) == 8);
            assert(BasicResultColumn<arrow::StringArray>(output, "protocol")->GetString(0) == "SUB");
            assert(BasicResultColumn<arrow::StringArray>(output, "end_reason")->GetString(0) == "closed");
        }

        closed_input.reset();
        assert(closed_input_owner.expired());
        assert(runtime->Sessions().size() == 0 && runtime->Collector().pending_results() == 0);
        assert(runtime->Projector().tracked_sessions() == 0);
        if (session_module != nullptr) {
            assert(session_module->tracked_sessions() == 0 && session_module->tracked_bytes() == 0);
        }
        const uint64_t new_output_bytes = ResultBufferCapacity(output);
        AssertTaskBudgetUsage(budget->Usage(), 0, 0, 0, old_output_bytes + new_output_bytes);
        output.reset();
        AssertTaskBudgetUsage(budget->Usage(), 0, 0, 0, old_output_bytes);
        old_output.reset();
        AssertTaskBudgetUsage(budget->Usage(), 0, 0, 0, 0);

        auto empty_input = MakeEncodedPacketBatch({});
        status = runtime->ProcessOfflineBatch(empty_input, &output);
        assert(status.error == npm::NpmBasicOfflineBatchError::kNone);
        assert(output != nullptr && output->schema().get() == expected_schema.get());
        assert(output->num_rows() == 0 && output->num_columns() == expected_columns);
        assert(runtime->Sessions().size() == 0 && runtime->Collector().pending_results() == 0);
        assert(runtime->Projector().tracked_sessions() == 0);
        if (session_module != nullptr) {
            assert(session_module->tracked_sessions() == 0 && session_module->tracked_bytes() == 0);
        }
        empty_input.reset();
        output.reset();
        AssertTaskBudgetUsage(budget->Usage(), 0, 0, 0, 0);

        runtime->Cancel();
        assert(runtime->State() == npm::NpmEofFlushState::kCancelled);
        assert(pool.release_calls == static_cast<int>(index + 1));
    }
}

void TestNpmBasicTaskRuntimeClosesTupleReuseReplacementImmediately() {
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);

    auto basic_only = MakeRuntimeTaskConfig();
    auto both_basic = MakeRuntimeTaskConfig();
    both_basic.features.session_enabled = true;
    auto session_only = MakeRuntimeTaskConfig();
    session_only.features.basic_enabled = false;
    session_only.features.session_enabled = true;
    session_only.features.observing = npm::NpmResultEntity::kSession;
    auto both_session = MakeRuntimeTaskConfig();
    both_session.features.session_enabled = true;
    both_session.features.observing = npm::NpmResultEntity::kSession;
    const std::array<npm::NpmBasicTaskConfig, 4> configs{basic_only, both_basic, session_only, both_session};

    for (size_t index = 0; index < configs.size(); ++index) {
        const auto& config = configs[index];
        const bool observing_session = config.features.observing == npm::NpmResultEntity::kSession;
        const auto expected_schema = observing_session ? npm::NpmSessionResultSchema() : npm::NpmBasicResultSchema();
        const int expected_columns = observing_session ? 49 : 32;
        const int64_t old_timestamp_ns = 100 + static_cast<int64_t>(index);
        const int64_t replacement_timestamp_ns = 200 + static_cast<int64_t>(index);

        std::shared_ptr<arrow::Schema> output_schema;
        std::unique_ptr<npm::NpmBasicTaskRuntime> runtime;
        const auto create_status = npm::NpmBasicTaskRuntime::Create(config, &querier, flowsql::packet::PacketSchema(),
                                                                    &output_schema, &runtime);
        assert(create_status.error == npm::NpmBasicTaskRuntimeError::kNone);
        assert(runtime != nullptr && output_schema.get() == expected_schema.get());
        assert(pool.acquire_calls == static_cast<int>(index + 1));
        assert(pool.release_calls == static_cast<int>(index));

        npm::NpmSessionAnalysisModule* session_module = nullptr;
        if (config.features.session_enabled) {
            assert(runtime->Modules().size() == 1);
            session_module = dynamic_cast<npm::NpmSessionAnalysisModule*>(runtime->Modules()[0]);
            assert(session_module != nullptr);
        } else {
            assert(runtime->Modules().empty());
        }

        const std::vector<uint8_t> old_payload{0x10, 0x11};
        const auto old_packet =
            MakeIpv4TcpPacket("192.0.2.1", 41000, "198.51.100.2", 443, old_payload, kTcpAck, 100, 90, 2048);
        auto old_input =
            MakeEncodedPacketBatch({MakeBatchPacketRecord(old_packet, 0, old_timestamp_ns, 2 * index + 1)});
        std::weak_ptr<arrow::RecordBatch> old_input_owner = old_input;
        std::shared_ptr<arrow::RecordBatch> output = MakeNpmPacketViewBatch();
        auto status = runtime->ProcessOfflineBatch(old_input, &output);
        assert(status.error == npm::NpmBasicOfflineBatchError::kNone);
        assert(status.runtime_state == npm::NpmEofFlushState::kOpen);
        assert(status.process_status.error == npm::NpmPacketBatchProcessError::kNone);
        assert(status.drain_status.error == npm::NpmBasicDrainError::kNone);
        assert(output != nullptr && output->schema().get() == expected_schema.get());
        assert(output->num_rows() == 0 && output->num_columns() == expected_columns);
        assert(runtime->Sessions().size() == 1 && runtime->Collector().pending_results() == 0);
        assert(runtime->Projector().tracked_sessions() == 0);
        assert(protocol.identify_pipelines.size() == 2 * index + 1);
        if (session_module != nullptr) {
            assert(session_module->tracked_sessions() == 1 && session_module->tracked_bytes() > 0);
        }

        auto budget = runtime->Budget();
        AssertTaskBudgetUsage(budget->Usage(), runtime->Sessions().tracked_bytes(),
                              session_module == nullptr ? 0 : session_module->tracked_bytes(), 0,
                              ResultBufferCapacity(output));
        old_input.reset();
        assert(old_input_owner.expired());
        output.reset();
        AssertTaskBudgetUsage(budget->Usage(), runtime->Sessions().tracked_bytes(),
                              session_module == nullptr ? 0 : session_module->tracked_bytes(), 0, 0);

        const std::vector<uint8_t> replacement_payload{0x20, 0x21, 0x22};
        const auto replacement_packet = MakeIpv4TcpPacket("192.0.2.1", 41000, "198.51.100.2", 443, replacement_payload,
                                                          kTcpSyn | kTcpRst, 200, 0, 4096);
        auto replacement_input = MakeEncodedPacketBatch(
            {MakeBatchPacketRecord(replacement_packet, 0, replacement_timestamp_ns, 2 * index + 2)});
        std::weak_ptr<arrow::RecordBatch> replacement_input_owner = replacement_input;
        const auto generated_start_ns = ResultClockNowNs();
        status = runtime->ProcessOfflineBatch(replacement_input, &output);
        const auto generated_end_ns = ResultClockNowNs();
        assert(status.error == npm::NpmBasicOfflineBatchError::kNone);
        assert(status.runtime_state == npm::NpmEofFlushState::kOpen);
        assert(status.process_status.error == npm::NpmPacketBatchProcessError::kNone);
        assert(status.drain_status.error == npm::NpmBasicDrainError::kNone);
        assert(output != nullptr && output->schema().get() == expected_schema.get());
        assert(output->num_rows() == 2 && output->num_columns() == expected_columns);
        assert(protocol.identify_pipelines.size() == 2 * index + 2);

        if (observing_session) {
            const auto* session_ids = SessionResultColumn<arrow::UInt64Array>(output, 0).get();
            const auto* revisions = SessionResultColumn<arrow::UInt64Array>(output, 2).get();
            const auto* observed_at = SessionResultColumn<arrow::Int64Array>(output, 3).get();
            const auto* final_flags = SessionResultColumn<arrow::BooleanArray>(output, 4).get();
            const auto* first_ns = SessionResultColumn<arrow::Int64Array>(output, 11).get();
            const auto* last_ns = SessionResultColumn<arrow::Int64Array>(output, 12).get();
            const auto* protocol_status = SessionResultColumn<arrow::StringArray>(output, 14).get();
            const auto* protocol_ids = SessionResultColumn<arrow::UInt16Array>(output, 15).get();
            const auto* protocol_sub_ids = SessionResultColumn<arrow::UInt16Array>(output, 16).get();
            const auto* protocol_names = SessionResultColumn<arrow::StringArray>(output, 17).get();
            const auto* reasons = SessionResultColumn<arrow::StringArray>(output, 18).get();
            const auto* packets_ab = SessionResultColumn<arrow::UInt64Array>(output, 19).get();
            const auto* packets_ba = SessionResultColumn<arrow::UInt64Array>(output, 20).get();
            const auto* wire_bytes_ab = SessionResultColumn<arrow::UInt64Array>(output, 21).get();
            const auto* wire_bytes_ba = SessionResultColumn<arrow::UInt64Array>(output, 22).get();
            const auto* payload_bytes_ab = SessionResultColumn<arrow::UInt64Array>(output, 23).get();
            const auto* payload_bytes_ba = SessionResultColumn<arrow::UInt64Array>(output, 24).get();

            assert(session_ids->Value(0) == 1 && session_ids->Value(1) == 2);
            assert(revisions->Value(0) == 1 && revisions->Value(1) == 1);
            assert(observed_at->Value(0) == replacement_timestamp_ns);
            assert(observed_at->Value(1) == replacement_timestamp_ns);
            assert(final_flags->Value(0) && final_flags->Value(1));
            assert(first_ns->Value(0) == old_timestamp_ns && last_ns->Value(0) == old_timestamp_ns);
            assert(first_ns->Value(1) == replacement_timestamp_ns);
            assert(last_ns->Value(1) == replacement_timestamp_ns);
            assert(protocol_status->GetString(0) == "identified");
            assert(protocol_status->GetString(1) == "identified");
            assert(protocol_ids->Value(0) == 7 && protocol_ids->Value(1) == 7);
            assert(protocol_sub_ids->Value(0) == 8 && protocol_sub_ids->Value(1) == 8);
            assert(protocol_names->GetString(0) == "SUB" && protocol_names->GetString(1) == "SUB");
            assert(reasons->GetString(0) == "tuple_reuse");
            assert(reasons->GetString(1) == "closed");
            assert(packets_ab->Value(0) == 1 && packets_ab->Value(1) == 1);
            assert(packets_ba->Value(0) == 0 && packets_ba->Value(1) == 0);
            assert(wire_bytes_ab->Value(0) == old_packet.bytes.size());
            assert(wire_bytes_ab->Value(1) == replacement_packet.bytes.size());
            assert(wire_bytes_ba->Value(0) == 0 && wire_bytes_ba->Value(1) == 0);
            assert(payload_bytes_ab->Value(0) == old_payload.size());
            assert(payload_bytes_ab->Value(1) == replacement_payload.size());
            assert(payload_bytes_ba->Value(0) == 0 && payload_bytes_ba->Value(1) == 0);
        } else {
            const auto* session_ids = BasicResultColumn<arrow::UInt64Array>(output, 0).get();
            const auto* revisions = BasicResultColumn<arrow::UInt64Array>(output, 2).get();
            const auto* observed_at = BasicResultColumn<arrow::Int64Array>(output, 3).get();
            const auto* final_flags = BasicResultColumn<arrow::BooleanArray>(output, 4).get();
            const auto* first_ns = BasicResultColumn<arrow::Int64Array>(output, 11).get();
            const auto* last_ns = BasicResultColumn<arrow::Int64Array>(output, 12).get();
            const auto* packets_ab = BasicResultColumn<arrow::UInt64Array>(output, 13).get();
            const auto* packets_ba = BasicResultColumn<arrow::UInt64Array>(output, 14).get();
            const auto* wire_bytes_ab = BasicResultColumn<arrow::UInt64Array>(output, 15).get();
            const auto* wire_bytes_ba = BasicResultColumn<arrow::UInt64Array>(output, 16).get();
            const auto* protocol_status = BasicResultColumn<arrow::StringArray>(output, "protocol_status").get();
            const auto* protocol_ids = BasicResultColumn<arrow::UInt16Array>(output, "protocol_id").get();
            const auto* protocol_sub_ids = BasicResultColumn<arrow::UInt16Array>(output, "protocol_sub_id").get();
            const auto* protocol_names = BasicResultColumn<arrow::StringArray>(output, "protocol").get();
            const auto* reasons = BasicResultColumn<arrow::StringArray>(output, "end_reason").get();

            assert(session_ids->Value(0) == 1 && session_ids->Value(1) == 2);
            assert(revisions->Value(0) == 1 && revisions->Value(1) == 1);
            for (int64_t row : {0, 1})
                assert(observed_at->Value(row) >= generated_start_ns && observed_at->Value(row) <= generated_end_ns);
            assert(final_flags->Value(0) && final_flags->Value(1));
            assert(first_ns->Value(0) == old_timestamp_ns && last_ns->Value(0) == old_timestamp_ns);
            assert(first_ns->Value(1) == replacement_timestamp_ns);
            assert(last_ns->Value(1) == replacement_timestamp_ns);
            assert(packets_ab->Value(0) == 1 && packets_ab->Value(1) == 1);
            assert(packets_ba->Value(0) == 0 && packets_ba->Value(1) == 0);
            assert(wire_bytes_ab->Value(0) == old_packet.bytes.size());
            assert(wire_bytes_ab->Value(1) == replacement_packet.bytes.size());
            assert(wire_bytes_ba->Value(0) == 0 && wire_bytes_ba->Value(1) == 0);
            assert(protocol_status->GetString(0) == "identified");
            assert(protocol_status->GetString(1) == "identified");
            assert(protocol_ids->Value(0) == 7 && protocol_ids->Value(1) == 7);
            assert(protocol_sub_ids->Value(0) == 8 && protocol_sub_ids->Value(1) == 8);
            assert(protocol_names->GetString(0) == "SUB" && protocol_names->GetString(1) == "SUB");
            assert(reasons->GetString(0) == "tuple_reuse");
            assert(reasons->GetString(1) == "closed");
        }

        replacement_input.reset();
        assert(replacement_input_owner.expired());
        assert(runtime->Sessions().size() == 0 && runtime->Collector().pending_results() == 0);
        assert(runtime->Projector().tracked_sessions() == 0);
        if (session_module != nullptr) {
            assert(session_module->tracked_sessions() == 0 && session_module->tracked_bytes() == 0);
        }
        AssertTaskBudgetUsage(budget->Usage(), 0, 0, 0, ResultBufferCapacity(output));
        output.reset();
        AssertTaskBudgetUsage(budget->Usage(), 0, 0, 0, 0);

        auto empty_input = MakeEncodedPacketBatch({});
        status = runtime->ProcessOfflineBatch(empty_input, &output);
        assert(status.error == npm::NpmBasicOfflineBatchError::kNone);
        assert(output != nullptr && output->schema().get() == expected_schema.get());
        assert(output->num_rows() == 0 && output->num_columns() == expected_columns);
        assert(runtime->Sessions().size() == 0 && runtime->Collector().pending_results() == 0);
        assert(runtime->Projector().tracked_sessions() == 0);
        if (session_module != nullptr) {
            assert(session_module->tracked_sessions() == 0 && session_module->tracked_bytes() == 0);
        }
        empty_input.reset();
        output.reset();
        AssertTaskBudgetUsage(budget->Usage(), 0, 0, 0, 0);

        runtime->Cancel();
        assert(runtime->State() == npm::NpmEofFlushState::kCancelled);
        assert(pool.release_calls == static_cast<int>(index + 1));
    }
}

void TestNpmBasicTaskRuntimeRoutesObservedEntity() {
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);

    auto basic_only = MakeRuntimeTaskConfig();
    auto both_basic = MakeRuntimeTaskConfig();
    both_basic.features.session_enabled = true;
    auto session_only = MakeRuntimeTaskConfig();
    session_only.features.basic_enabled = false;
    session_only.features.session_enabled = true;
    session_only.features.observing = npm::NpmResultEntity::kSession;
    auto both_session = MakeRuntimeTaskConfig();
    both_session.features.session_enabled = true;
    both_session.features.observing = npm::NpmResultEntity::kSession;
    const std::array<npm::NpmBasicTaskConfig, 4> configs{basic_only, both_basic, session_only, both_session};

    for (size_t index = 0; index < configs.size(); ++index) {
        const auto& config = configs[index];
        const bool observing_session = config.features.observing == npm::NpmResultEntity::kSession;
        const auto expected_schema = observing_session ? npm::NpmSessionResultSchema() : npm::NpmBasicResultSchema();
        const int expected_columns = observing_session ? 49 : 32;

        std::shared_ptr<arrow::Schema> output_schema;
        std::unique_ptr<npm::NpmBasicTaskRuntime> runtime;
        const auto create_status = npm::NpmBasicTaskRuntime::Create(config, &querier, flowsql::packet::PacketSchema(),
                                                                    &output_schema, &runtime);
        assert(create_status.error == npm::NpmBasicTaskRuntimeError::kNone);
        assert(runtime != nullptr && output_schema.get() == expected_schema.get());
        assert(runtime->Modules().size() == (config.features.session_enabled ? 1 : 0));
        assert(pool.acquire_calls == static_cast<int>(index + 1));
        assert(pool.release_calls == static_cast<int>(index));

        const auto packet =
            MakeIpv6UdpPacket("2001:db8::10", 53000, "2001:db8::20", 53, {static_cast<uint8_t>(index + 1)});
        auto input =
            MakeEncodedPacketBatch({MakeBatchPacketRecord(packet, 0, 100 + static_cast<int64_t>(index), index + 1)});
        std::shared_ptr<arrow::RecordBatch> output = MakeNpmPacketViewBatch();
        const auto process_status = runtime->ProcessOfflineBatch(input, &output);
        assert(process_status.error == npm::NpmBasicOfflineBatchError::kNone);
        assert(process_status.runtime_state == npm::NpmEofFlushState::kOpen);
        assert(process_status.drain_status.error == npm::NpmBasicDrainError::kNone);
        assert(output != nullptr && output->schema().get() == expected_schema.get());
        assert(output->num_rows() == 0 && output->num_columns() == expected_columns);
        assert(runtime->Sessions().size() == 1 && runtime->Collector().pending_results() == 0);
        assert(runtime->Projector().tracked_sessions() == 0);

        if (config.features.session_enabled) {
            auto* session_module = dynamic_cast<npm::NpmSessionAnalysisModule*>(runtime->Modules()[0]);
            assert(session_module != nullptr && session_module->tracked_sessions() == 1);
        }

        auto budget = runtime->Budget();
        output.reset();
        assert(budget->Usage().pending_output_bytes == 0);

        const int64_t observed_at = 500 + static_cast<int64_t>(index);
        const auto generated_start_ns = ResultClockNowNs();
        const auto flush_status = runtime->FlushOffline(observed_at, &output);
        const auto generated_end_ns = ResultClockNowNs();
        assert(flush_status.error == npm::NpmEofFlushError::kNone);
        assert(flush_status.drain_status.error == npm::NpmBasicDrainError::kNone);
        assert(runtime->State() == npm::NpmEofFlushState::kFlushed);
        assert(runtime->LastError().empty());
        assert(pool.release_calls == static_cast<int>(index + 1));
        assert(output != nullptr && output->schema().get() == expected_schema.get());
        assert(output->num_rows() == 1 && output->num_columns() == expected_columns);

        if (observing_session) {
            assert(SessionResultColumn<arrow::Int64Array>(output, 3)->Value(0) == observed_at);
            assert(SessionResultColumn<arrow::BooleanArray>(output, 4)->Value(0));
            assert(SessionResultColumn<arrow::UInt8Array>(output, 6)->Value(0) == 17);
            assert(SessionResultColumn<arrow::StringArray>(output, 18)->GetString(0) == "eof");
        } else {
            const auto generated_at = BasicResultColumn<arrow::Int64Array>(output, 3)->Value(0);
            assert(generated_at >= generated_start_ns && generated_at <= generated_end_ns);
            assert(BasicResultColumn<arrow::BooleanArray>(output, 4)->Value(0));
            assert(BasicResultColumn<arrow::StringArray>(output, "end_reason")->GetString(0) == "eof");
        }

        AssertTaskBudgetUsage(budget->Usage(), 0, 0, 0, ResultBufferCapacity(output));
        output.reset();
        AssertTaskBudgetUsage(budget->Usage(), 0, 0, 0, 0);
        runtime.reset();
    }
}

void TestNpmBasicTaskRuntimeUsesExactOfflineInputBudget() {
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    auto config = MakeRuntimeTaskConfig();
    config.analysis.max_tracked_bytes = npm::kNpmMinTrackedBytes;
    auto runtime = CreateRuntimeForTest(config, &querier);

    const auto packet = MakeIpv4TcpPacket("192.0.2.1", 41000, "198.51.100.2", 443, {0x11}, kTcpAck, 92);
    auto input = MakeEncodedPacketBatch({MakeBatchPacketRecord(packet, 99, 100, 2)});
    const uint64_t input_bytes = BasicResultBufferBytes(input);
    assert(input_bytes > 0 && input_bytes < config.analysis.max_tracked_bytes);
    auto budget = runtime->Budget();
    const uint64_t exact_existing_bytes = config.analysis.max_tracked_bytes - input_bytes;
    assert(budget->Reserve(npm::NpmBudgetCategory::kModuleState, exact_existing_bytes) == npm::NpmBudgetError::kNone);
    std::shared_ptr<arrow::RecordBatch> output = MakeNpmPacketViewBatch();
    auto original_output = output;

    auto status = runtime->ProcessOfflineBatch(input, &output);
    assert(status.error == npm::NpmBasicOfflineBatchError::kBatchProcessError);
    assert(status.runtime_state == npm::NpmEofFlushState::kFailed);
    assert(status.input_bytes == input_bytes && status.budget_error == npm::NpmBudgetError::kNone);
    assert(status.process_status.error == npm::NpmPacketBatchProcessError::kPacketError);
    assert(status.process_status.row == 0);
    assert(status.process_status.packet_status.error == npm::NpmPacketProcessError::kBindingError);
    assert(status.process_status.packet_status.binding_error == npm::NpmSessionPacketError::kUnknownSourceId);
    assert(output == original_output && runtime->State() == npm::NpmEofFlushState::kFailed);
    assert(!runtime->LastError().empty() && pool.release_calls == pool.acquire_calls);
    AssertTaskBudgetUsage(budget->Usage(), 0, exact_existing_bytes, 0, 0);
    assert(budget->Release(npm::NpmBudgetCategory::kModuleState, exact_existing_bytes) == npm::NpmBudgetError::kNone);
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

    const auto packet = MakeIpv4TcpPacket("192.0.2.1", 41000, "198.51.100.2", 443, {0x11}, kTcpAck, 93);
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
    auto wrong_input = arrow::RecordBatch::Make(schema_without_metadata, input->num_rows(), input->columns());
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
    assert(budget->Reserve(npm::NpmBudgetCategory::kPendingOutput, output_limit) == npm::NpmBudgetError::kNone);
    const auto rst = MakeIpv4TcpPacket("192.0.2.1", 41000, "198.51.100.2", 443, {}, kTcpRst, 94);
    auto drain_failure_input = MakeEncodedPacketBatch({MakeBatchPacketRecord(rst, 0, 200, 4)});
    status = runtime->ProcessOfflineBatch(drain_failure_input, &output);
    assert(status.error == npm::NpmBasicOfflineBatchError::kDrainError);
    assert(status.runtime_state == npm::NpmEofFlushState::kFailed);
    assert(status.drain_status.error == npm::NpmBasicDrainError::kRouterError);
    assert(status.drain_status.router_error == ENOMEM);
    assert(output == original_output);
    AssertTaskBudgetUsage(budget->Usage(), 0, 0, 0, output_limit);
    assert(budget->Release(npm::NpmBudgetCategory::kPendingOutput, output_limit) == npm::NpmBudgetError::kNone);
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

    const auto packet = MakeIpv4TcpPacket("192.0.2.1", 41000, "198.51.100.2", 443, {0x11}, kTcpAck, 95);
    auto input = MakeEncodedPacketBatch({MakeBatchPacketRecord(packet, 0, 100, 5)});
    std::shared_ptr<arrow::RecordBatch> output;
    const auto process_status = runtime->ProcessOfflineBatch(input, &output);
    assert(process_status.error == npm::NpmBasicOfflineBatchError::kNone);
    assert(process_status.runtime_state == npm::NpmEofFlushState::kOpen);
    assert(output != nullptr && output->num_rows() == 0 && runtime->Sessions().size() == 1);
    const uint64_t session_bytes = runtime->Sessions().tracked_bytes();
    assert(session_bytes > 0);
    AssertTaskBudgetUsage(budget->Usage(), session_bytes, 0, 0, ResultBufferCapacity(output));
    output.reset();
    AssertTaskBudgetUsage(budget->Usage(), session_bytes, 0, 0, 0);

    const auto generated_start_ns = ResultClockNowNs();
    auto flush_status = runtime->FlushOffline(500, &output);
    const auto generated_end_ns = ResultClockNowNs();
    assert(flush_status.error == npm::NpmEofFlushError::kNone);
    assert(runtime->State() == npm::NpmEofFlushState::kFlushed);
    assert(runtime->LastError().empty() && pool.release_calls == 1);
    assert(output != nullptr && output->schema()->Equals(*npm::NpmBasicResultSchema(), true));
    assert(output->num_rows() == 1 && output->num_columns() == 32);
    const auto generated_at = BasicResultColumn<arrow::Int64Array>(output, 3)->Value(0);
    assert(generated_at >= generated_start_ns && generated_at <= generated_end_ns);
    assert(BasicResultColumn<arrow::BooleanArray>(output, 4)->Value(0));
    assert(BasicResultColumn<arrow::StringArray>(output, "end_reason")->GetString(0) == "eof");
    AssertTaskBudgetUsage(budget->Usage(), 0, 0, 0, ResultBufferCapacity(output));

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
    assert(output->num_rows() == 0 && output->num_columns() == 32);
}

void TestNpmBasicTaskRuntimeEofFailureIsTerminal() {
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    const auto config = MakeRuntimeTaskConfig();
    auto runtime = CreateRuntimeForTest(config, &querier);

    const auto packet = MakeIpv4TcpPacket("192.0.2.1", 41000, "198.51.100.2", 443, {0x11}, kTcpAck, 96);
    auto input = MakeEncodedPacketBatch({MakeBatchPacketRecord(packet, 0, 100, 6)});
    std::shared_ptr<arrow::RecordBatch> batch_output;
    const auto process_status = runtime->ProcessOfflineBatch(input, &batch_output);
    assert(process_status.error == npm::NpmBasicOfflineBatchError::kNone);
    assert(runtime->Sessions().size() == 1);
    batch_output.reset();

    auto budget = runtime->Budget();
    const uint64_t output_limit = config.analysis.max_pending_output_bytes;
    assert(budget->Reserve(npm::NpmBudgetCategory::kPendingOutput, output_limit) == npm::NpmBudgetError::kNone);
    std::shared_ptr<arrow::RecordBatch> output = MakeNpmPacketViewBatch();
    const auto original_output = output;
    auto flush_status = runtime->FlushOffline(500, &output);
    assert(flush_status.error == npm::NpmEofFlushError::kDrainError);
    assert(flush_status.drain_status.error == npm::NpmBasicDrainError::kRouterError);
    assert(flush_status.drain_status.router_error == ENOMEM);
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
    assert(budget->Release(npm::NpmBudgetCategory::kPendingOutput, output_limit) == npm::NpmBudgetError::kNone);

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

    const auto packet = MakeIpv4TcpPacket("192.0.2.1", 41000, "198.51.100.2", 443, {0x11}, kTcpAck, 97);
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

    const auto rst = MakeIpv4TcpPacket("192.0.2.1", 41000, "198.51.100.2", 443, {}, kTcpRst, 98);
    auto input = MakeEncodedPacketBatch({MakeBatchPacketRecord(rst, 0, 100, 8)});
    std::shared_ptr<arrow::RecordBatch> output;
    const auto status = runtime->ProcessOfflineBatch(input, &output);
    assert(status.error == npm::NpmBasicOfflineBatchError::kNone && output != nullptr);
    const uint64_t output_bytes = ResultBufferCapacity(output);
    AssertTaskBudgetUsage(budget->Usage(), 0, 0, 0, output_bytes);

    runtime->Cancel();
    assert(runtime->State() == npm::NpmEofFlushState::kCancelled);
    assert(!runtime->LastError().empty() && pool.release_calls == 1);
    AssertTaskBudgetUsage(budget->Usage(), 0, 0, 0, output_bytes);
    assert(BasicResultColumn<arrow::StringArray>(output, "end_reason")->GetString(0) == "closed");
    runtime.reset();
    assert(pool.release_calls == 1);
    AssertTaskBudgetUsage(budget->Usage(), 0, 0, 0, output_bytes);
    output.reset();
    AssertTaskBudgetUsage(budget->Usage(), 0, 0, 0, 0);
}

flowsql::BlockTransformTaskConfigV1 MakeOperatorTaskConfig(const std::string& task_id,
                                                           const std::string& with_params_json,
                                                           const std::string& pushed_filter_plan_json) {
    flowsql::BlockTransformTaskConfigV1 config;
    config.task_id = task_id.c_str();
    config.with_params_json = with_params_json.c_str();
    config.pushed_filter_plan_json = pushed_filter_plan_json.c_str();
    return config;
}

void TestNpmBasicTaskPreservesLatePacketDiagnostic() {
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    npm::NpmBasicOperator provider(&querier);
    const std::string task_id = "public-late-packet";
    const std::string json =
        R"({"source_domains":"7:77","output_interval_ns":"10000000","out_of_order_tolerance_ns":"0"})";
    const std::string filter = R"({"version":1,"root":null})";
    flowsql::IBlockTransformTaskV1* task = nullptr;
    assert(provider.CreateTask(MakeOperatorTaskConfig(task_id, json, filter), &task) == 0);
    std::shared_ptr<arrow::Schema> schema;
    assert(task->Open(flowsql::packet::PacketSchema(), &schema) == 0);
    const auto packet = MakeIpv4TcpPacket("192.0.2.1", 50000, "192.0.2.2", 443, {1});
    std::vector<flowsql::BlockTransformOutputV1> outputs;
    assert(task->ProcessBlock(MakeEncodedPacketBatch({MakeBatchPacketRecord(packet, 7, 1, 1)}), 0, &outputs) == 0);
    outputs.clear();
    assert(task->ProcessBlock(MakeEncodedPacketBatch({MakeBatchPacketRecord(packet, 7, 20'000'000, 2)}), 0, &outputs) ==
           0);
    assert(outputs.size() == 1 && outputs.front().batch->num_rows() == 2);
    auto prefix = outputs.front().batch;
    outputs.clear();
    const auto late = MakeEncodedPacketBatch({MakeBatchPacketRecord(packet, 7, 1, 3)});
    assert(task->ProcessBlock(late, 0, &outputs) == -EIO && outputs.empty());
    const auto diagnostic = task->LastError();
    assert(diagnostic.find("late npm.basic packet") != std::string::npos);
    assert(diagnostic.find("source=7") != std::string::npos);
    assert(diagnostic.find("timestamp_ns=1") != std::string::npos);
    assert(diagnostic.find("closed_boundary_ns=20000000") != std::string::npos);
    assert(task->ProcessBlock(late, 0, &outputs) == -EPIPE && outputs.empty());
    assert(task->Flush(&outputs) == -EPIPE && outputs.empty());
    task->Cancel();
    assert(task->LastError() == diagnostic && pool.release_calls == 1);
    assert(PeriodicUInt(prefix, "interval_packets_ab", 0) == 1);
    assert(PeriodicUInt(prefix, "interval_packets_ab", 1) == 0);
    assert(!PeriodicFlag(prefix, "is_final", 0) && !PeriodicFlag(prefix, "is_final", 1));
    prefix.reset();
    schema.reset();
    provider.ReleaseTask(task);
}

void TestNpmBasicFinalUsesGenerationTime() {
    for (const bool rst_close : {true, false}) {
        for (const bool observing_session : {false, true}) {
            ContextDictionary dictionary;
            ContextProtocol protocol(&dictionary);
            ContextPool pool(&protocol);
            SinglePoolQuerier querier(&pool);
            npm::NpmBasicOperator provider(&querier);
            const std::string task_id = "final-generation-time";
            const std::string json =
                std::string(R"({"source_domains":"0:77","result_mode":"final","features":"basic,session",)"
                            R"("observing":")") +
                (observing_session ? "session" : "basic") + "\"}";
            const std::string filter = R"({"version":1,"root":null})";
            flowsql::IBlockTransformTaskV1* task = nullptr;
            assert(provider.CreateTask(MakeOperatorTaskConfig(task_id, json, filter), &task) == 0);
            std::shared_ptr<arrow::Schema> schema;
            assert(task->Open(flowsql::packet::PacketSchema(), &schema) == 0);
            const auto packet =
                MakeIpv4TcpPacket("192.0.2.1", 50000, "192.0.2.2", 443, {1}, rst_close ? kTcpRst : kTcpAck);
            const auto input = MakeEncodedPacketBatch({MakeBatchPacketRecord(packet, 0, 1'000'000'000, 1)});
            std::vector<flowsql::BlockTransformOutputV1> outputs;
            int64_t started_ns = ResultClockNowNs();
            assert(task->ProcessBlock(input, 0, &outputs) == 0);
            if (!rst_close) {
                for (const auto& output : outputs) assert(output.batch->num_rows() == 0);
                outputs.clear();
                started_ns = ResultClockNowNs();
                assert(task->Flush(&outputs) == 0);
            }
            const int64_t finished_ns = ResultClockNowNs();
            assert(outputs.size() == 1 && outputs.front().batch->num_rows() == 1);
            const auto& result = outputs.front().batch;
            const auto observed_at = PeriodicInt(result, "observed_at");
            if (observing_session) {
                assert(observed_at == 1'000'000'000);
            } else {
                assert(observed_at >= started_ns && observed_at <= finished_ns);
                for (const char* column : {"period_start_ns", "period_end_ns", "period_complete", "interval_packets_ab",
                                           "interval_packets_ba", "interval_wire_bytes_ab", "interval_wire_bytes_ba",
                                           "interval_wire_bytes_total"})
                    assert(result->GetColumnByName(column)->IsNull(0));
                assert(PeriodicUInt(result, "wire_bytes_total") == packet.bytes.size());
            }
            assert(PeriodicInt(result, "first_ns") == 1'000'000'000);
            assert(PeriodicInt(result, "last_ns") == 1'000'000'000);
            assert(PeriodicFlag(result, "is_final"));
            assert(PeriodicUInt(result, "packets_ab") == 1 && PeriodicUInt(result, "packets_ba") == 0);
            outputs.clear();
            schema.reset();
            provider.ReleaseTask(task);
        }
    }
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
    std::string with_json = R"({"input_namespace":"pcapfile.capture","source_domains":"0:77"})";
    std::string filter_plan = R"({"version":1,"root":null})";
    auto config = MakeOperatorTaskConfig(task_id, with_json, filter_plan);
    flowsql::IBlockTransformTaskV1* first = nullptr;
    assert(provider.CreateTask(config, &first) == 0 && first != nullptr);

    auto* concrete = dynamic_cast<npm::NpmBasicTask*>(first);
    assert(concrete != nullptr);
    auto* input_task = dynamic_cast<flowsql::IBlockTransformInputSourceTaskV1*>(first);
    assert(input_task != nullptr);
    assert(input_task->BindInputSource(nullptr) == EINVAL);
    assert(input_task->BindInputSource("") == EINVAL);
    std::string source = "pcapfile.http";
    assert(input_task->BindInputSource(source.c_str()) == 0);
    source.assign("changed");
    assert(input_task->BindInputSource("pcapfile.other") == EALREADY);
    task_id.assign("mutated-task");
    with_json.assign("mutated-with");
    filter_plan.assign("mutated-plan");
    assert(concrete->TaskId() == "task-owned-one");
    assert(concrete->WithParamsJson() == R"({"input_namespace":"pcapfile.capture","source_domains":"0:77"})");
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

    provider.ReleaseTask(static_cast<flowsql::IBlockTransformTaskV1*>(nullptr));
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

void TestNpmBasicTaskObservingSchemaProbe() {
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    npm::NpmBasicOperator provider(&querier);
    const std::string filter_plan = R"({"version":1,"root":null})";
    size_t completed_probes = 0;

    const auto assert_probe = [&](const std::string& task_id, const std::string& with_json,
                                  const std::shared_ptr<arrow::Schema>& expected_schema) {
        const auto config = MakeOperatorTaskConfig(task_id, with_json, filter_plan);
        flowsql::IBlockTransformTaskV1* task = nullptr;
        assert(provider.CreateTask(config, &task) == 0 && task != nullptr);

        auto sentinel_schema = arrow::schema({arrow::field("sentinel", arrow::int8())});
        std::shared_ptr<arrow::Schema> output_schema = sentinel_schema;
        assert(task->Open(flowsql::packet::PacketSchema(), &output_schema) == 0);
        assert(output_schema.get() == expected_schema.get());
        assert(task->LastError().empty());
        assert(pool.acquire_calls == static_cast<int>(completed_probes + 1));
        assert(pool.release_calls == static_cast<int>(completed_probes));

        task->Cancel();
        provider.ReleaseTask(task);
        ++completed_probes;
        assert(pool.release_calls == static_cast<int>(completed_probes));
    };

    assert_probe("probe-default", R"({"input_namespace":"pcapfile.capture","source_domains":"0:77"})",
                 npm::NpmBasicResultSchema());
    assert_probe("probe-both-basic",
                 R"({"input_namespace":"pcapfile.capture","source_domains":"0:77","features":"basic,session",)"
                 R"("observing":"basic"})",
                 npm::NpmBasicResultSchema());
    assert_probe(
        "probe-session-only",
        R"({"input_namespace":"pcapfile.capture","source_domains":"0:77","features":"session","observing":"session"})",
        npm::NpmSessionResultSchema());
    assert_probe("probe-both-session",
                 R"({"input_namespace":"pcapfile.capture","source_domains":"0:77","features":"basic,session",)"
                 R"("observing":"session"})",
                 npm::NpmSessionResultSchema());
}

void TestNpmBasicTaskProbeMatchesLegacyAndParametersV1() {
    for (const bool observing_session : {false, true}) {
        auto configs = MakeEquivalentRuntimeTaskConfigs(npm::NpmRunMode::kOffline, observing_session);
        const std::string expected_legacy_json = configs.legacy_json;
        const std::string expected_parameters_json = configs.parameters_v1_json;
        ContextDictionary dictionary;
        ContextProtocol protocol(&dictionary);
        DualContextPool pool(&protocol);
        SinglePoolQuerier querier(&pool);
        npm::NpmBasicOperator provider(&querier);
        const std::string filter_plan = R"({"version":1,"root":null})";
        const std::string legacy_task_id = observing_session ? "probe-legacy-session" : "probe-legacy-basic";
        const std::string parameters_task_id =
            observing_session ? "probe-parameters-session" : "probe-parameters-basic";
        const auto legacy_config = MakeOperatorTaskConfig(legacy_task_id, configs.legacy_json, filter_plan);
        const auto parameters_config =
            MakeOperatorTaskConfig(parameters_task_id, configs.parameters_v1_json, filter_plan);
        flowsql::IBlockTransformTaskV1* legacy_task = nullptr;
        flowsql::IBlockTransformTaskV1* parameters_task = nullptr;
        assert(provider.CreateTask(legacy_config, &legacy_task) == 0 && legacy_task != nullptr);
        assert(provider.CreateTask(parameters_config, &parameters_task) == 0 && parameters_task != nullptr);

        configs.legacy_json.assign(configs.legacy_json.size(), 'x');
        configs.parameters_v1_json.assign(configs.parameters_v1_json.size(), 'y');
        auto* legacy_concrete = dynamic_cast<npm::NpmBasicTask*>(legacy_task);
        auto* parameters_concrete = dynamic_cast<npm::NpmBasicTask*>(parameters_task);
        assert(legacy_concrete != nullptr && parameters_concrete != nullptr);
        assert(legacy_concrete->WithParamsJson() == expected_legacy_json);
        assert(parameters_concrete->WithParamsJson() == expected_parameters_json);

        std::shared_ptr<arrow::Schema> legacy_schema;
        std::shared_ptr<arrow::Schema> parameters_schema;
        assert(legacy_task->Open(flowsql::packet::PacketSchema(), &legacy_schema) == 0);
        assert(parameters_task->Open(flowsql::packet::PacketSchema(), &parameters_schema) == 0);
        assert(legacy_schema != nullptr && parameters_schema != nullptr);
        assert(legacy_schema->Equals(*parameters_schema, true));
        const auto expected_schema = observing_session ? npm::NpmSessionResultSchema() : npm::NpmBasicResultSchema();
        assert(legacy_schema->Equals(*expected_schema, true));
        assert(legacy_task->LastError().empty() && parameters_task->LastError().empty());
        assert(pool.acquire_calls == 2 && pool.release_calls == 0);

        legacy_task->Cancel();
        assert(pool.release_calls == 1);
        parameters_task->Cancel();
        assert(pool.release_calls == 2);
        provider.ReleaseTask(legacy_task);
        provider.ReleaseTask(parameters_task);
        assert(pool.release_calls == 2);
    }
}

void TestNpmBasicTaskOpenProcessAndFlush() {
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    npm::NpmBasicOperator provider(&querier);
    const std::string task_id = "task-happy";
    const std::string with_json = R"({"input_namespace":"pcapfile.capture","source_domains":"0:77"})";
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

    const auto packet = MakeIpv4TcpPacket("192.0.2.1", 41000, "198.51.100.2", 443, {}, kTcpAck, 101);
    auto first_input = MakeEncodedPacketBatch({MakeBatchPacketRecord(packet, 0, 100, 1)});
    std::vector<flowsql::BlockTransformOutputV1> outputs;
    assert(task->ProcessBlock(first_input, 11, &outputs) ==
           static_cast<int>(flowsql::BlockTransformStatusV1::kContinue));
    assert(outputs.size() == 1 && outputs[0].batch != nullptr);
    assert(outputs[0].batch->num_rows() == 0 && outputs[0].ts_ms == 11);
    outputs.clear();

    auto out_of_order_input = MakeEncodedPacketBatch({MakeBatchPacketRecord(packet, 0, 50, 2)});
    assert(task->ProcessBlock(out_of_order_input, 22, &outputs) ==
           static_cast<int>(flowsql::BlockTransformStatusV1::kContinue));
    assert(outputs.size() == 1 && outputs[0].batch->num_rows() == 0 && outputs[0].ts_ms == 22);
    outputs.clear();

    const auto before_flush_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    assert(task->Flush(&outputs) == 0);
    assert(outputs.size() == 1 && outputs[0].batch != nullptr && outputs[0].ts_ms == 22);
    assert(outputs[0].batch->num_rows() == 1);
    assert(BasicResultColumn<arrow::Int64Array>(outputs[0].batch, 3)->Value(0) >= before_flush_ns);
    assert(BasicResultColumn<arrow::Int64Array>(outputs[0].batch, 12)->Value(0) == 100);
    assert(BasicResultColumn<arrow::StringArray>(outputs[0].batch, "end_reason")->GetString(0) == "eof");
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
    assert(BasicResultColumn<arrow::StringArray>(retained_output, "end_reason")->GetString(0) == "eof");
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

void TestNpmBasicTaskReportsConfigurationFailurePaths() {
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    npm::NpmBasicOperator provider(&querier);
    const std::string filter_plan = R"({"version":1,"root":null})";
    const auto assert_failure = [&](const std::string& task_id, const std::string& with_json,
                                    const std::vector<std::string>& expected_fragments) {
        const auto config = MakeOperatorTaskConfig(task_id, with_json, filter_plan);
        flowsql::IBlockTransformTaskV1* task = nullptr;
        assert(provider.CreateTask(config, &task) == 0 && task != nullptr);
        auto sentinel_schema = arrow::schema({arrow::field("sentinel", arrow::int8())});
        std::shared_ptr<arrow::Schema> output_schema = sentinel_schema;
        assert(task->Open(flowsql::packet::PacketSchema(), &output_schema) != 0);
        assert(output_schema == sentinel_schema);
        const auto error = task->LastError();
        for (const auto& fragment : expected_fragments) {
            assert(error.find(fragment) != std::string::npos);
        }
        provider.ReleaseTask(task);
    };

    assert_failure("task-invalid-core-parameter",
                   R"JSON({"input_namespace":"pcapfile.capture","source_domains":"0:77",)JSON"
                   R"JSON("parameters":"{\"schema_version\":1,\"core\":{\"max_active_sessions\":\"1\"}}"})JSON",
                   {"invalid parameters", "/core/max_active_sessions"});
    assert_failure("task-invalid-session-parameter",
                   R"JSON({"input_namespace":"pcapfile.capture","source_domains":"0:77",)JSON"
                   R"JSON("features":"basic,session","observing":"session",)JSON"
                   R"JSON("parameters":"{\"schema_version\":1,\"session\":{\"max_tcp_ranges_per_direction\":7}}"})JSON",
                   {"invalid parameters", "/session/max_tcp_ranges_per_direction"});
    assert_failure("task-parameter-source-conflict",
                   R"JSON({"input_namespace":"pcapfile.capture","source_domains":"0:77",)JSON"
                   R"JSON("run_mode":"offline","parameters":"{\"schema_version\":1}"})JSON",
                   {"configuration source conflict", "/run_mode"});
    assert_failure("task-six-modules-missing-dns-config", R"({"features":"basic,session,dns,http1,tls,icmp"})",
                   {"runtime creation failed", "invalid module configuration", "/dns/primary_label_ids"});
    assert_failure("task-missing-http1-config", R"({"features":"basic,http1"})",
                   {"invalid module configuration", "/http1/primary_label_ids"});
    assert_failure("task-missing-tls-config", R"({"features":"basic,tls"})",
                   {"invalid module configuration", "/tls/primary_label_ids"});
    assert_failure(
        "task-dns-requires-labeling",
        R"({"features":"basic,dns","parameters":"{\"schema_version\":1,\"dns\":{\"primary_label_ids\":[1001]}}"})",
        {"required module capability is unavailable", "/dns/labeling"});
}

void TestNpmBasicTaskQueriesLabelingProviderConditionally() {
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    LabelingStatusProvider labeling_provider;
    querier.labeling_provider = &labeling_provider;

    npm::NpmBasicOperator lifecycle_provider;
    assert(lifecycle_provider.Option(nullptr) == 0);
    assert(lifecycle_provider.Load(&querier) == 0);
    assert(lifecycle_provider.Start() == 0);
    assert(querier.labeling_queries == 0);
    assert(lifecycle_provider.Stop() == 0);
    assert(lifecycle_provider.Unload() == 0);

    npm::NpmBasicOperator provider(&querier);
    const std::string filter_plan = R"({"version":1,"root":null})";
    auto sentinel_schema = arrow::schema({arrow::field("sentinel", arrow::int8())});

    const std::string ordinary_json = R"({"input_namespace":"pcapfile.capture","source_domains":"0:77",)"
                                      R"("features":"basic,session","parameters":"{\"schema_version\":1}"})";
    const std::string ordinary_task_id = "ordinary-with-labeling-provider";
    auto config = MakeOperatorTaskConfig(ordinary_task_id, ordinary_json, filter_plan);
    flowsql::IBlockTransformTaskV1* task = nullptr;
    assert(provider.CreateTask(config, &task) == 0 && task != nullptr);
    std::shared_ptr<arrow::Schema> output_schema = sentinel_schema;
    assert(task->Open(flowsql::packet::PacketSchema(), &output_schema) == 0);
    assert(output_schema != sentinel_schema);
    assert(querier.labeling_queries == 0);
    assert(querier.config_registry_queries == 0);
    assert(labeling_provider.runtime_status_calls == 0 && labeling_provider.create_matcher_calls == 0);
    provider.ReleaseTask(task);

    const std::string ignored_labeling_json = R"({"input_namespace":"pcapfile.capture","source_domains":"0:77",)"
                                              R"("parameters":"{\"schema_version\":1,\"core\":{\"labeling\":null}}"})";
    querier.labeling_provider = nullptr;
    const std::string ignored_task_id = "ordinary-ignores-labeling-field";
    config = MakeOperatorTaskConfig(ignored_task_id, ignored_labeling_json, filter_plan);
    task = nullptr;
    assert(provider.CreateTask(config, &task) == 0 && task != nullptr);
    output_schema = sentinel_schema;
    assert(task->Open(flowsql::packet::PacketSchema(), &output_schema) == 0);
    assert(output_schema != sentinel_schema && querier.labeling_queries == 0);
    provider.ReleaseTask(task);

    const std::string labeling_json = R"({"input_namespace":"pcapfile.capture","source_domains":"0:77",)"
                                      R"("features":"basic,labeling",)"
                                      R"("parameters":"{\"schema_version\":1,\"core\":{)"
                                      R"(\"labeling\":\"config.corp-labels@7\",)"
                                      R"(\"labeling_memory_mib\":127}}"})";

    querier.labeling_provider = nullptr;
    const std::string missing_task_id = "missing-labeling-provider";
    config = MakeOperatorTaskConfig(missing_task_id, labeling_json, filter_plan);
    task = nullptr;
    assert(provider.CreateTask(config, &task) == 0 && task != nullptr);
    output_schema = sentinel_schema;
    assert(task->Open(flowsql::packet::PacketSchema(), &output_schema) == ENODEV);
    assert(output_schema == sentinel_schema);
    assert(task->LastError() == "npm.basic labeling provider is unavailable");
    assert(querier.labeling_queries == 1);
    assert(labeling_provider.runtime_status_calls == 0 && labeling_provider.create_matcher_calls == 0);
    provider.ReleaseTask(task);

    querier.labeling_provider = &labeling_provider;
    labeling_provider.ready = false;
    const std::string unready_task_id = "unready-labeling-provider";
    config = MakeOperatorTaskConfig(unready_task_id, labeling_json, filter_plan);
    task = nullptr;
    assert(provider.CreateTask(config, &task) == 0 && task != nullptr);
    output_schema = sentinel_schema;
    assert(task->Open(flowsql::packet::PacketSchema(), &output_schema) == ENODEV);
    assert(output_schema == sentinel_schema);
    assert(task->LastError() ==
           "npm.basic labeling provider is unavailable at /runtime: mock labeling runtime unavailable");
    assert(querier.labeling_queries == 2);
    assert(labeling_provider.runtime_status_calls == 1 && labeling_provider.create_matcher_calls == 0);
    provider.ReleaseTask(task);

    labeling_provider.ready = true;
    const std::string ready_task_id = "ready-labeling-provider-without-config-registry";
    config = MakeOperatorTaskConfig(ready_task_id, labeling_json, filter_plan);
    task = nullptr;
    assert(provider.CreateTask(config, &task) == 0 && task != nullptr);
    output_schema = sentinel_schema;
    assert(task->Open(flowsql::packet::PacketSchema(), &output_schema) == ENODEV);
    assert(output_schema == sentinel_schema);
    assert(task->LastError() == "npm.basic labeling config registry is unavailable");
    assert(querier.labeling_queries == 3);
    assert(querier.config_registry_queries == 1);
    assert(labeling_provider.runtime_status_calls == 2 && labeling_provider.create_matcher_calls == 0);
    provider.ReleaseTask(task);

    LabelingConfigRegistry config_registry;
    querier.config_registry = &config_registry;
    labeling_provider.create_succeeds = true;
    const std::string connected_task_id = "connected-labeling-matcher";
    config = MakeOperatorTaskConfig(connected_task_id, labeling_json, filter_plan);
    task = nullptr;
    assert(provider.CreateTask(config, &task) == 0 && task != nullptr);
    output_schema = sentinel_schema;
    assert(task->Open(flowsql::packet::PacketSchema(), &output_schema) == 0);
    assert(output_schema != sentinel_schema && output_schema->GetFieldIndex("primary_label_id") == 17);
    assert(querier.labeling_queries == 4 && querier.config_registry_queries == 2);
    assert(config_registry.resolve_calls == 1 && config_registry.last_reference == "config.corp-labels@7");
    assert(labeling_provider.runtime_status_calls == 3 && labeling_provider.create_matcher_calls == 1);
    assert(labeling_provider.captured_reference == "corp-labels@7");
    assert(labeling_provider.captured_reserved_bytes == 127ULL * 1024ULL * 1024ULL);
    assert(labeling_provider.captured_max_labels == 10'000);
    assert(labeling_provider.captured_max_logical_rules == 50'000);
    assert(labeling_provider.captured_max_compiled_rules == 100'000);
    assert(labeling_provider.matcher_stats.release_calls == 0);

    const auto labeled_packet =
        MakeIpv4TcpPacket("192.0.2.10", 51000, "198.51.100.20", 443, {0x16, 0x03}, kTcpAck, 700);
    auto labeled_input = MakeEncodedPacketBatch({MakeBatchPacketRecord(labeled_packet, 0, 100, 1)});
    std::vector<flowsql::BlockTransformOutputV1> labeled_outputs;
    assert(task->ProcessBlock(labeled_input, 17, &labeled_outputs) ==
           static_cast<int>(flowsql::BlockTransformStatusV1::kContinue));
    assert(labeled_outputs.size() == 1 && labeled_outputs[0].batch->num_rows() == 0);
    labeled_outputs.clear();
    assert(task->Flush(&labeled_outputs) == 0);
    assert(labeled_outputs.size() == 1 && labeled_outputs[0].batch->num_rows() == 1);
    const int label_column = labeled_outputs[0].batch->schema()->GetFieldIndex("primary_label_id");
    const int protocol_column = labeled_outputs[0].batch->schema()->GetFieldIndex("protocol");
    assert(label_column == 17 && protocol_column == 21);
    assert(BasicResultColumn<arrow::UInt32Array>(labeled_outputs[0].batch, label_column)->Value(0) == 1001);
    assert(BasicResultColumn<arrow::StringArray>(labeled_outputs[0].batch, protocol_column)->GetString(0) == "SUB");
    assert(labeling_provider.matcher_stats.classify_calls == 1);
    assert((labeling_provider.matcher_stats.batch_counts == std::vector<uint32_t>{1}));
    assert(labeling_provider.matcher_stats.release_calls == 1);
    provider.ReleaseTask(task);
    assert(labeling_provider.matcher_stats.release_calls == 1);

    const std::string session_labeling_json = R"({"input_namespace":"pcapfile.capture","source_domains":"0:77",)"
                                              R"("features":"basic,session,labeling","observing":"session",)"
                                              R"("parameters":"{\"schema_version\":1,\"core\":{)"
                                              R"(\"labeling\":\"config.corp-labels@7\"}}"})";
    const std::string session_task_id = "connected-labeling-session-result";
    config = MakeOperatorTaskConfig(session_task_id, session_labeling_json, filter_plan);
    task = nullptr;
    assert(provider.CreateTask(config, &task) == 0 && task != nullptr);
    output_schema = sentinel_schema;
    assert(task->Open(flowsql::packet::PacketSchema(), &output_schema) == 0);
    assert(output_schema->GetFieldIndex("primary_label_id") == 14);
    assert(labeling_provider.captured_reserved_bytes == 64ULL * 1024ULL * 1024ULL);
    labeled_outputs.clear();
    assert(task->ProcessBlock(labeled_input, 18, &labeled_outputs) ==
           static_cast<int>(flowsql::BlockTransformStatusV1::kContinue));
    labeled_outputs.clear();
    assert(task->Flush(&labeled_outputs) == 0);
    assert(labeled_outputs.size() == 1 && labeled_outputs[0].batch->num_rows() == 1);
    const int session_label_column = labeled_outputs[0].batch->schema()->GetFieldIndex("primary_label_id");
    const int session_protocol_column = labeled_outputs[0].batch->schema()->GetFieldIndex("protocol");
    assert(session_label_column == 14 && session_protocol_column == 18);
    assert(BasicResultColumn<arrow::UInt32Array>(labeled_outputs[0].batch, session_label_column)->Value(0) == 1001);
    assert(BasicResultColumn<arrow::StringArray>(labeled_outputs[0].batch, session_protocol_column)->GetString(0) ==
           "SUB");
    assert(labeling_provider.matcher_stats.classify_calls == 2);
    assert(labeling_provider.matcher_stats.release_calls == 2);
    provider.ReleaseTask(task);
    assert(labeling_provider.create_matcher_calls == 2 && config_registry.resolve_calls == 2);

    const std::string invalid_labeling_json =
        R"({"input_namespace":"pcapfile.capture","source_domains":"0:77",)"
        R"("features":"basic,labeling",)"
        R"("parameters":"{\"schema_version\":1,\"core\":{\"labeling\":\"@latest\"}}"})";
    const std::string invalid_task_id = "invalid-labeling-reference";
    config = MakeOperatorTaskConfig(invalid_task_id, invalid_labeling_json, filter_plan);
    task = nullptr;
    assert(provider.CreateTask(config, &task) == 0 && task != nullptr);
    output_schema = sentinel_schema;
    assert(task->Open(flowsql::packet::PacketSchema(), &output_schema) == EINVAL);
    assert(output_schema == sentinel_schema);
    assert(task->LastError().find("/core/labeling") != std::string::npos);
    assert(querier.labeling_queries == 6);
    assert(labeling_provider.runtime_status_calls == 5 && labeling_provider.create_matcher_calls == 2);
    provider.ReleaseTask(task);
}

#ifdef FLOWSQL_FLOW_LABELING_PLUGIN_PATH
void TestRealFlowLabelingMatcherWithNpmBasicTask() {
    flowsql::PluginLoader* loader = flowsql::PluginLoader::Single();
    const char* libraries[] = {FLOWSQL_FLOW_LABELING_PLUGIN_PATH};
    assert(loader->Load(libraries, 1) == 0);
    assert(loader->StartAll() == 0);
    auto* labeling_provider =
        static_cast<flowsql::IFlowLabelingProviderV1*>(loader->First(flowsql::IID_FLOW_LABELING_PROVIDER_V1));
    auto* plugin = static_cast<flowsql::IPlugin*>(loader->First(flowsql::IID_PLUGIN));
    assert(labeling_provider != nullptr && plugin != nullptr);

    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    RealLabelingSnapshotRegistry registry;
    querier.labeling_provider = labeling_provider;
    querier.config_registry = &registry;
    npm::NpmBasicOperator provider(&querier);

    const auto first = MakeIpv4TcpPacket("192.0.2.1", 50000, "198.51.100.2", 443, {}, kTcpAck);
    const auto reply = MakeIpv4TcpPacket("198.51.100.2", 443, "192.0.2.1", 50000, {}, kTcpAck);
    const auto reverse_first = MakeIpv4TcpPacket("198.51.100.2", 443, "192.0.2.9", 50001, {}, kTcpAck);
    const auto unmatched = MakeIpv4TcpPacket("192.0.2.20", 50002, "198.51.100.2", 80, {}, kTcpAck);
    const auto input = MakeEncodedPacketBatch(
        {MakeBatchPacketRecord(first, 0, 100, 1), MakeBatchPacketRecord(reply, 0, 200, 2),
         MakeBatchPacketRecord(reverse_first, 0, 300, 3), MakeBatchPacketRecord(unmatched, 0, 400, 4)});
    const std::string filter_plan = R"({"version":1,"root":null})";
    const std::string params =
        R"("parameters":"{\"schema_version\":1,\"core\":{\"labeling\":\"config.corp-labels@7\"}}")";

    for (bool session_result : {false, true}) {
        const std::string task_id = session_result ? "real-labeling-session" : "real-labeling-basic";
        const std::string with_json =
            R"({"input_namespace":"pcapfile.capture","source_domains":"0:77",)" +
            std::string(session_result ? R"("features":"basic,session,labeling","observing":"session",)"
                                       : R"("features":"basic,labeling",)") +
            params + "}";
        const auto config = MakeOperatorTaskConfig(task_id, with_json, filter_plan);
        flowsql::IBlockTransformTaskV1* task = nullptr;
        assert(provider.CreateTask(config, &task) == 0 && task != nullptr);
        std::shared_ptr<arrow::Schema> output_schema;
        assert(task->Open(flowsql::packet::PacketSchema(), &output_schema) == 0);
        assert(output_schema != nullptr && output_schema->GetFieldIndex("primary_label_id") >= 0);
        if (!session_result) assert(plugin->Stop() != 0 && "live task matcher prevents plugin Stop");

        std::vector<flowsql::BlockTransformOutputV1> outputs;
        assert(task->ProcessBlock(input, 1, &outputs) == static_cast<int>(flowsql::BlockTransformStatusV1::kContinue));
        outputs.clear();
        assert(task->Flush(&outputs) == 0 && outputs.size() == 1);
        const auto& batch = outputs[0].batch;
        assert(batch != nullptr && batch->num_rows() == 3);
        const int label_column = batch->schema()->GetFieldIndex("primary_label_id");
        const int a_port_column = batch->schema()->GetFieldIndex("a_port");
        assert(label_column >= 0 && a_port_column >= 0);
        auto labels = BasicResultColumn<arrow::UInt32Array>(batch, label_column);
        auto ports = BasicResultColumn<arrow::UInt16Array>(batch, a_port_column);
        assert(labels->null_count() == 0);
        for (int64_t row = 0; row < batch->num_rows(); ++row) {
            const uint32_t expected = ports->Value(row) == 50002 ? 0 : 1001;
            assert(labels->Value(row) == expected);
        }
        assert(querier.labeling_queries == registry.resolve_calls);
        provider.ReleaseTask(task);
    }

    assert(registry.resolve_calls == 2);
    loader->StopAll();
    flowsql::FlowLabelingDiagnosticV1 diagnostic;
    assert(labeling_provider->RuntimeStatus(&diagnostic) == flowsql::FlowLabelingErrorV1::kUnavailable);
    assert(loader->Unload() == 0);
}
#endif

void TestNpmBasicTaskMethodPreconditions() {
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    npm::NpmBasicOperator provider(&querier);
    const std::string task_id = "task-preconditions";
    const std::string with_json = R"({"input_namespace":"pcapfile.capture","source_domains":"0:77"})";
    const std::string filter_plan = R"({"version":1,"root":null})";
    const auto config = MakeOperatorTaskConfig(task_id, with_json, filter_plan);
    const auto packet = MakeIpv4TcpPacket("192.0.2.1", 41000, "198.51.100.2", 443, {}, kTcpAck, 103);
    auto input = MakeEncodedPacketBatch({MakeBatchPacketRecord(packet, 0, 100, 4)});
    auto sentinel_schema = arrow::schema({arrow::field("sentinel", arrow::int8())});
    std::vector<flowsql::BlockTransformOutputV1> outputs;
    flowsql::IBlockTransformTaskV1* task = nullptr;

    const std::string retained_with =
        R"JSON({"input_namespace":"pcapfile.capture","source_domains":"0:77","parameters":"{\"schema_version\":1,\"core\":{\"result\":{\"retention_days\":1}}}"})JSON";
    const auto retained_config = MakeOperatorTaskConfig(task_id, retained_with, filter_plan);
    assert(provider.CreateTask(retained_config, &task) == 0);
    std::shared_ptr<arrow::Schema> retained_schema = sentinel_schema;
    assert(task->Open(flowsql::packet::PacketSchema(), &retained_schema) == EINVAL);
    assert(retained_schema == sentinel_schema);
    assert(task->LastError().find("retention requires a managed database sink") != std::string::npos);
    provider.ReleaseTask(task);
    task = nullptr;

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
    const std::string with_json = R"({"input_namespace":"pcapfile.capture","source_domains":"0:77"})";
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
    const auto packet = MakeIpv4TcpPacket("192.0.2.1", 41000, "198.51.100.2", 443, {0x11}, kTcpAck, 102);
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

flowsql::CaptureSourceSetV2 MakeCaptureSourceSet(flowsql::CaptureQueueIdentityV1 identity) {
    identity.link_type = 1;
    flowsql::CaptureSourceSetV2 sources;
    sources.inputs = {identity};
    sources.limits.max_packets_per_batch = 256;
    sources.limits.max_bytes_per_batch = 1048576;
    sources.limits.max_wait_ms = 10;
    sources.limits.max_outstanding_batches = 1;
    return sources;
}

void TestNpmBasicV2PluginExports() {
    flowsql::test::ScopedSharedLibrary library(FLOWSQL_NPM_BASIC_PLUGIN_PATH);
    assert(library);

    auto abi_version = reinterpret_cast<flowsql::CppOperatorPluginAbiVersionFn>(
        library.Symbol(flowsql::kCppOperatorPluginAbiVersionSymbol));
    auto operator_count =
        reinterpret_cast<flowsql::CppOperatorPluginCountFn>(library.Symbol(flowsql::kCppOperatorPluginCountSymbol));
    auto describe = reinterpret_cast<flowsql::CppOperatorPluginDescribeV2Fn>(
        library.Symbol(flowsql::kCppOperatorPluginDescribeV2Symbol));
    auto create = reinterpret_cast<flowsql::CppOperatorPluginCreateCapabilityV2Fn>(
        library.Symbol(flowsql::kCppOperatorPluginCreateCapabilityV2Symbol));
    auto destroy = reinterpret_cast<flowsql::CppOperatorPluginDestroyCapabilityV2Fn>(
        library.Symbol(flowsql::kCppOperatorPluginDestroyCapabilityV2Symbol));
    assert(abi_version != nullptr && operator_count != nullptr && describe != nullptr);
    assert(create != nullptr && destroy != nullptr);
    assert(library.Symbol("pluginregist") == nullptr);
    assert(library.Symbol(flowsql::kCppOperatorPluginCreateV1Symbol) == nullptr);
    assert(library.Symbol(flowsql::kCppOperatorPluginDestroyV1Symbol) == nullptr);
    assert(abi_version() == flowsql::kCppOperatorPluginAbiVersionV2);
    assert(operator_count() == 2);

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
    assert(describe(1, &descriptor) == 0);
    assert(std::string(descriptor.category) == "npm" && std::string(descriptor.name) == "basic");
    assert(SameGuid(descriptor.contract_iid, flowsql::IID_BLOCK_TRANSFORM_OPERATOR_V2));
    assert(describe(2, &descriptor) == EINVAL);
    assert(describe(0, nullptr) == EINVAL);
    assert(create(-1, nullptr) == nullptr);
    assert(create(2, nullptr) == nullptr);

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
    const std::string with_json = R"({"input_namespace":"pcapfile.capture","source_domains":"0:77"})";
    const std::string filter_plan = R"({"version":1,"root":null})";
    const auto config = MakeOperatorTaskConfig(task_id, with_json, filter_plan);
    flowsql::IBlockTransformTaskV1* task = nullptr;
    assert(provider->CreateTask(config, &task) == 0 && task != nullptr);
    std::shared_ptr<arrow::Schema> output_schema;
    assert(task->Open(flowsql::packet::PacketSchema(), &output_schema) == 0);
    assert(output_schema != nullptr && output_schema->Equals(*npm::NpmBasicResultSchema(), true));

    const auto rst = MakeIpv4TcpPacket("192.0.2.1", 41000, "198.51.100.2", 443, {}, kTcpRst, 104);
    auto input = MakeEncodedPacketBatch({MakeBatchPacketRecord(rst, 0, 100, 6)});
    std::vector<flowsql::BlockTransformOutputV1> outputs;
    assert(task->ProcessBlock(input, 44, &outputs) == static_cast<int>(flowsql::BlockTransformStatusV1::kContinue));
    assert(outputs.size() == 1 && outputs[0].batch != nullptr && outputs[0].ts_ms == 44);
    assert(outputs[0].batch->num_rows() == 0);
    outputs.clear();
    assert(task->Flush(&outputs) == 0);
    assert(outputs.size() == 1 && outputs[0].batch != nullptr && outputs[0].batch->num_rows() == 1);
    assert(BasicResultColumn<arrow::StringArray>(outputs[0].batch, "end_reason")->GetString(0) == "closed");
    assert(BasicResultColumn<arrow::BooleanArray>(outputs[0].batch, "is_final")->Value(0));
    assert(!BasicResultColumn<arrow::BooleanArray>(outputs[0].batch, "period_complete")->Value(0));
    provider->ReleaseTask(task);
    outputs.clear();
    output_schema.reset();

    void* v2_capability = create(1, &querier);
    assert(v2_capability != nullptr);
    auto* v2_provider = static_cast<flowsql::IBlockTransformOperatorV2*>(v2_capability);
    flowsql::BlockTransformTaskConfigV2 v2_config{};
    v2_config.struct_size = flowsql::kBlockTransformTaskConfigV2Size;
    v2_config.contract_version = flowsql::kBlockTransformContractVersionV2;
    v2_config.task_id = "v2-realtime";
    v2_config.with_params_json = R"({"input_namespace":"live","source_domains":"7:77","run_mode":"realtime"})";
    v2_config.pushed_filter_plan_json = filter_plan.c_str();
    flowsql::IBlockTransformTaskV2* v2_task = nullptr;
    assert(v2_provider->CreateTask(v2_config, &v2_task) == 0 && v2_task != nullptr);
    auto* capture_task = dynamic_cast<flowsql::IBlockTransformCaptureFactTaskV2*>(v2_task);
    assert(capture_task != nullptr);
    std::shared_ptr<arrow::Schema> v2_schema;
    assert(v2_task->Open(flowsql::packet::PacketSchema(), &v2_schema) != 0);
    v2_provider->ReleaseTask(v2_task);

    assert(v2_provider->CreateTask(v2_config, &v2_task) == 0 && v2_task != nullptr);
    capture_task = dynamic_cast<flowsql::IBlockTransformCaptureFactTaskV2*>(v2_task);
    flowsql::CaptureQueueIdentityV1 identity;
    identity.source_name = "live.capture";
    identity.source_id = 7;
    identity.observation_domain_id = 77;
    identity.generation = 1;
    assert(capture_task->BindCaptureSources(MakeCaptureSourceSet(identity)) == 0);
    assert(capture_task->BindCaptureSources(MakeCaptureSourceSet(identity)) == EALREADY);
    assert(v2_task->Open(flowsql::packet::PacketSchema(), &v2_schema) == 0);
    assert(v2_schema != nullptr && v2_schema->Equals(*npm::NpmBasicResultSchema(), true));
    flowsql::BlockTransformTimeDriveStateV1 time_state{};
    time_state.struct_size = flowsql::kBlockTransformTimeDriveStateV1Size;
    time_state.contract_version = flowsql::kBlockTransformTimeDriveVersionV1;
    assert(v2_task->GetTimeDriveState(&time_state) == 0);
    assert(time_state.armed == 1 && time_state.deadline_ns == 0);
    flowsql::BlockTransformTimeEventV1 time_event{};
    time_event.struct_size = flowsql::kBlockTransformTimeEventV1Size;
    time_event.contract_version = flowsql::kBlockTransformTimeDriveVersionV1;
    time_event.monotonic_now_ns = 100;
    time_event.wall_now_ns = 1'000'000'000;
    std::vector<flowsql::BlockTransformOutputV1> time_outputs;
    assert(v2_task->OnTime(time_event, &time_outputs) == 0 && time_outputs.empty());
    assert(v2_task->GetTimeDriveState(&time_state) == 0);
    assert(time_state.armed == 1 && time_state.deadline_ns > 100);
    const int64_t first_snapshot_deadline = time_state.deadline_ns;
    const auto live_packet = MakeIpv4TcpPacket("192.0.2.10", 44000, "198.51.100.10", 443, {0x10}, kTcpAck, 100);
    auto live_batch = MakeEncodedPacketBatch({MakeBatchPacketRecord(live_packet, 7, 100, 1)});
    std::vector<flowsql::BlockTransformOutputV1> packet_outputs;
    assert(v2_task->ProcessBlock(live_batch, 1, &packet_outputs) == 0);
    assert(packet_outputs.size() == 1 && packet_outputs[0].batch != nullptr);
    packet_outputs.clear();
    flowsql::CaptureProgressV1 fact;
    fact.source_id = 7;
    fact.generation = 1;
    fact.fact_sequence = 1;
    fact.capture_time_ns = 100;
    fact.packet_observed = true;
    fact.backlog = flowsql::CaptureBacklogV1::kEmpty;
    assert(capture_task->AcceptCaptureFacts({fact}) == 0);
    assert(v2_task->GetTimeDriveState(&time_state) == 0 && time_state.deadline_ns == 0);
    time_event.monotonic_now_ns = 101;
    time_event.wall_now_ns = 500'000'000;
    assert(v2_task->OnTime(time_event, &time_outputs) == 0 && time_outputs.empty());
    assert(v2_task->GetTimeDriveState(&time_state) == 0);
    assert(time_state.deadline_ns == first_snapshot_deadline);
    assert(capture_task->AcceptCaptureFacts({fact}) == EINVAL);
    fact.fact_sequence = 2;
    fact.generation = 2;
    assert(capture_task->AcceptCaptureFacts({fact}) == EINVAL);
    fact.generation = 1;
    fact.queue_id = 1;
    assert(capture_task->AcceptCaptureFacts({fact}) == EINVAL);
    fact.queue_id = 0;
    fact.capture_time_ns = 200;
    fact.packet_observed = false;
    fact.backlog = flowsql::CaptureBacklogV1::kPresent;
    assert(capture_task->AcceptCaptureFacts({fact}) == 0);
    time_event.monotonic_now_ns = 102;
    assert(v2_task->OnTime(time_event, &time_outputs) == 0 && time_outputs.empty());
    fact.fact_sequence = 3;
    fact.capture_time_ns = 300;
    fact.backlog = flowsql::CaptureBacklogV1::kUnknown;
    assert(capture_task->AcceptCaptureFacts({fact}) == 0);
    time_event.monotonic_now_ns = 103;
    assert(v2_task->OnTime(time_event, &time_outputs) == 0 && time_outputs.empty());
    time_event.monotonic_now_ns = first_snapshot_deadline;
    time_event.wall_now_ns = 100'000'000;
    assert(v2_task->OnTime(time_event, &time_outputs) == 0);
    assert(time_outputs.empty());
    fact.fact_sequence = 4;
    fact.capture_time_ns = 31'000'000'000LL;
    fact.backlog = flowsql::CaptureBacklogV1::kEmpty;
    fact.source_idle_confirmed = true;
    assert(capture_task->AcceptCaptureFacts({fact}) == 0);
    time_event.monotonic_now_ns = first_snapshot_deadline + 1;
    assert(v2_task->OnTime(time_event, &time_outputs) == 0);
    assert(time_outputs.size() == 1 && time_outputs[0].batch->num_rows() == 1);
    assert(time_outputs[0].batch->schema()->Equals(*npm::NpmBasicResultSchema(), true));
    assert(BasicResultColumn<arrow::BooleanArray>(time_outputs[0].batch, "period_complete")->Value(0));
    assert(v2_task->GetTimeDriveState(&time_state) == 0);
    assert(time_state.deadline_ns > first_snapshot_deadline);
    time_outputs.clear();
    v2_schema.reset();
    v2_task->Cancel();
    v2_provider->ReleaseTask(v2_task);

    // Omitted input parameters inherit the named source and reader domain, not domain=source_id.
    v2_config.task_id = "v2-input-defaults";
    v2_config.with_params_json = "{}";
    assert(v2_provider->CreateTask(v2_config, &v2_task) == 0 && v2_task != nullptr);
    auto* input_task = dynamic_cast<flowsql::IBlockTransformInputSourceTaskV1*>(v2_task);
    assert(input_task != nullptr && input_task->BindInputSource("netadapter.eth0") == 0);
    capture_task = dynamic_cast<flowsql::IBlockTransformCaptureFactTaskV2*>(v2_task);
    assert(capture_task != nullptr && capture_task->BindCaptureSources(MakeCaptureSourceSet(identity)) == 0);
    assert(v2_task->Open(flowsql::packet::PacketSchema(), &v2_schema) == 0);
    assert(v2_task->ProcessBlock(live_batch, 1, &packet_outputs) == 0);
    packet_outputs.clear();
    assert(v2_task->Flush(&packet_outputs) == 0);
    assert(packet_outputs.size() == 1 && packet_outputs[0].batch->num_rows() == 1);
    assert(BasicResultColumn<arrow::UInt64Array>(packet_outputs[0].batch, "observation_domain_id")->Value(0) == 77);
    packet_outputs.clear();
    v2_schema.reset();
    v2_provider->ReleaseTask(v2_task);
    destroy(1, v2_capability);

    destroy(0, capability);
    destroy(0, nullptr);
}

struct ProtocolInputTrace {
    int created = 0;
    int aborted = 0;
    int prepared = 0;
    int snapshots = 0;
    std::vector<npm::NpmInputKindV1> kinds;
    std::vector<size_t> sizes;
    std::vector<bool> complete;
    std::vector<uint64_t> sessions;
    std::vector<std::string> order;
    std::string json;
};

class CountingProtocolModule final : public npm::INpmProtocolModuleV1 {
 public:
    explicit CountingProtocolModule(ProtocolInputTrace* trace) : trace_(trace) { ++trace_->created; }
    int OnInput(const npm::NpmInputEventV1& event, npm::INpmResultEmitterV1&) override {
        assert(npm::ValidateNpmInputEventV1(event).error == npm::NpmProtocolContractErrorV1::kNone);
        trace_->kinds.push_back(event.kind);
        trace_->sizes.push_back(event.body.size);
        trace_->complete.push_back(event.body_complete);
        trace_->sessions.push_back(event.session ? event.session->session_id : 0);
        trace_->order.push_back("packet");
        return 0;
    }
    int OnSessionSnapshot(const npm::NpmSessionView&, int64_t, npm::INpmResultEmitterV1&) override {
        ++trace_->snapshots;
        return 0;
    }
    int OnSessionEnd(const npm::NpmSessionView&, npm::NpmSessionEndReason, int64_t,
                     npm::INpmResultEmitterV1&) override {
        trace_->order.push_back("end");
        return 0;
    }
    std::optional<int64_t> NextEventDeadlineNs() const override { return {}; }
    int OnTime(const npm::NpmModuleTimeV1&, npm::INpmResultEmitterV1&) override { return 0; }
    int Finish(int64_t, npm::INpmResultEmitterV1&) override { return 0; }
    void Abort() noexcept override { ++trace_->aborted; }

 private:
    ProtocolInputTrace* trace_;
};

npm::NpmModuleRegistrationV1 CountingRegistration(std::string id, uint8_t mask, ProtocolInputTrace* trace,
                                                  bool stream = false,
                                                  std::optional<std::vector<uint32_t>> labels = {}) {
    npm::NpmModuleRegistrationV1 entry;
    entry.module_id = id;
    entry.entity_ids = {id + "_event"};
    entry.prepare = [id, mask, trace, stream, labels](const npm::NpmBasicTaskConfig&, std::string_view json,
                                                      npm::NpmPreparedModuleV1* output) {
        ++trace->prepared;
        trace->json = json;
        output->plan.module_id = id;
        output->plan.input_mask = mask;
        output->plan.requires_tcp_stream = stream;
        output->plan.requires_labeling = labels.has_value();
        output->plan.primary_label_ids = labels;
        auto entity = npm::NpmBasicEntityDescriptorV1();
        entity.entity_id = id + "_event";
        entity.module_id = id;
        output->plan.entities = {entity};
        output->create = [trace](npm::NpmProtocolContext&, std::shared_ptr<npm::INpmTaskBudget>) {
            npm::NpmModuleInstanceV1 instance;
            instance.protocol = std::make_unique<CountingProtocolModule>(trace);
            return instance;
        };
        return npm::NpmProtocolContractStatusV1{};
    };
    return entry;
}

PacketFixture MakeControlPacket(bool ipv6 = false) {
    auto fixture = ipv6 ? MakeIpv6UdpPacket("2001:db8::1", 1, "2001:db8::2", 2, {9, 8, 7, 6})
                        : MakeIpv4TcpPacket("192.0.2.1", 1, "192.0.2.2", 2, {});
    const size_t header = ipv6 ? sizeof(flowsql::Ipv6Header) : sizeof(flowsql::Ipv4Header);
    fixture.bytes.resize(header + 12);
    if (ipv6) {
        auto* ip = reinterpret_cast<flowsql::Ipv6Header*>(fixture.bytes.data());
        ip->protocol = 58;
        ip->payload = htons(12);
    } else {
        auto* ip = reinterpret_cast<flowsql::Ipv4Header*>(fixture.bytes.data());
        ip->protocol = 1;
        ip->total_length = htons(header + 12);
    }
    fixture.layer.transport_protocol = ipv6 ? 58 : 1;
    fixture.layer.transport_layer_index = flowsql::packet::kNoLayerIndex;
    fixture.layer.layer_count = 1;
    fixture.layer.ports_valid = false;
    fixture.layer.src_port = fixture.layer.dst_port = 0;
    fixture.layer.payload_offset = header;
    return fixture;
}

void TestIpv6WireVersionAndEthernetControlBounds() {
    const npm::NpmObservationDomainMap domains{"wire-ipv6", {{1, 77}}};
    auto udp = MakeIpv6UdpPacket("2001:db8::1", 41000, "2001:db8::2", 41001, {1, 2, 3});
    npm::NpmSessionPacketBinding binding;
    assert(udp.bytes[0] == 0x60);
    assert(npm::BuildNpmSessionPacketBinding(domains, udp.View(1), udp.layer, &binding) ==
           npm::NpmSessionPacketError::kNone);
    // Traffic class and flow label do not move the wire version nibble.
    udp.bytes[0] = 0x6f;
    udp.bytes[1] = 0xab;
    udp.bytes[2] = 0xcd;
    udp.bytes[3] = 0xef;
    assert(npm::BuildNpmSessionPacketBinding(domains, udp.View(1), udp.layer, &binding) ==
           npm::NpmSessionPacketError::kNone);
    udp.bytes[0] = 0x40;
    assert(npm::BuildNpmSessionPacketBinding(domains, udp.View(1), udp.layer, &binding) ==
           npm::NpmSessionPacketError::kInvalidPayloadBounds);

    // Real Ethernet Router Solicitation: 14-byte L2, 40-byte IPv6, 16-byte ICMPv6 body.
    auto control = MakeControlPacket(true);
    control.bytes.resize(56);
    reinterpret_cast<flowsql::Ipv6Header*>(control.bytes.data())->payload = htons(16);
    control.bytes[40] = 133;
    control.bytes.insert(control.bytes.begin(), 14, 0);
    control.bytes[12] = 0x86;
    control.bytes[13] = 0xdd;
    control.layer.layer_count = 2;
    control.layer.network_layer_index = 1;
    control.layer.layers[0] = {static_cast<uint16_t>(flowsql::eLayer::ETHERNET), 0};
    control.layer.layers[1] = {static_cast<uint16_t>(flowsql::eLayer::IPv6), 14};
    control.layer.payload_offset = 54;
    npm::NpmInputEventV1 event;
    assert(npm::BuildNpmControlInput(domains, control.View(1), control.layer, &event) ==
           npm::NpmSessionPacketError::kNone);
    assert(event.kind == npm::NpmInputKindV1::kControlPacket && event.body.size == 16 && event.body[0] == 133);
    assert(event.body_complete && !event.layer->ports_valid && event.observation_domain_id == 77);
}

void TestNpmBasicTaskFlushAppliesPendingCaptureFacts() {
    const auto tcp = MakeIpv4TcpPacket("192.0.2.1", 50000, "192.0.2.2", 443, {1});
    auto icmp = MakeControlPacket();
    icmp.bytes[sizeof(flowsql::Ipv4Header)] = 8;
    icmp.bytes[sizeof(flowsql::Ipv4Header) + 1] = 0;
    using Row = std::array<uint64_t, 12>;
    std::vector<Row> reference;
    for (const bool drive_before_flush : {true, false}) {
        for (const bool cancel : {false, true}) {
            ContextDictionary dictionary;
            ContextProtocol protocol(&dictionary);
            ContextPool pool(&protocol);
            SinglePoolQuerier querier(&pool);
            npm::NpmBasicOperator provider(&querier);
            flowsql::BlockTransformTaskConfigV2 config{};
            config.struct_size = flowsql::kBlockTransformTaskConfigV2Size;
            config.contract_version = flowsql::kBlockTransformContractVersionV2;
            config.task_id = "pending-capture-stop";
            config.with_params_json = R"({"input_namespace":"live","source_domains":"0:77","features":"basic,icmp",)"
                                      R"("output_interval_ns":"10000000","out_of_order_tolerance_ns":"0"})";
            config.pushed_filter_plan_json = R"({"version":1,"root":null})";
            flowsql::IBlockTransformTaskV2* task = nullptr;
            assert(provider.CreateTask(config, &task) == 0);
            auto* capture_task = dynamic_cast<flowsql::IBlockTransformCaptureFactTaskV2*>(task);
            assert(capture_task != nullptr);
            flowsql::CaptureQueueIdentityV1 identity;
            identity.source_name = "review.live";
            identity.observation_domain_id = 77;
            identity.generation = 1;
            assert(capture_task->BindCaptureSources(MakeCaptureSourceSet(identity)) == 0);
            std::shared_ptr<arrow::Schema> schema;
            assert(task->Open(flowsql::packet::PacketSchema(), &schema) == 0);
            std::vector<flowsql::BlockTransformOutputV1> outputs;
            flowsql::CaptureProgressV1 fact;
            fact.generation = 1;
            fact.packet_observed = true;
            fact.backlog = flowsql::CaptureBacklogV1::kEmpty;
            for (const auto& record :
                 {MakeBatchPacketRecord(tcp, 0, 10'000'001, 1), MakeBatchPacketRecord(icmp, 0, 40'000'001, 2)}) {
                assert(task->ProcessBlock(MakeEncodedPacketBatch({record}), 0, &outputs) == 0);
                for (const auto& output : outputs) assert(output.batch->num_rows() == 0);
                outputs.clear();
                fact.fact_sequence = record.meta.sequence;
                fact.capture_time_ns = record.meta.timestamp_ns;
                assert(capture_task->AcceptCaptureFacts({fact}) == 0);
            }
            std::vector<Row> rows;
            const auto collect = [&] {
                for (const auto& output : outputs) {
                    for (int64_t row = 0; row < output.batch->num_rows(); ++row) {
                        const auto& batch = output.batch;
                        rows.push_back({PeriodicUInt(batch, "session_id", row), PeriodicUInt(batch, "revision", row),
                                        static_cast<uint64_t>(PeriodicInt(batch, "period_start_ns", row)),
                                        static_cast<uint64_t>(PeriodicInt(batch, "period_end_ns", row)),
                                        PeriodicFlag(batch, "period_complete", row),
                                        PeriodicFlag(batch, "is_final", row), PeriodicUInt(batch, "packets_ab", row),
                                        PeriodicUInt(batch, "packets_ba", row),
                                        PeriodicUInt(batch, "wire_bytes_total", row),
                                        PeriodicUInt(batch, "interval_packets_ab", row),
                                        PeriodicUInt(batch, "interval_packets_ba", row),
                                        PeriodicUInt(batch, "interval_wire_bytes_total", row)});
                    }
                }
                outputs.clear();
            };
            if (drive_before_flush) {
                flowsql::BlockTransformTimeEventV1 time{};
                time.struct_size = flowsql::kBlockTransformTimeEventV1Size;
                time.contract_version = flowsql::kBlockTransformTimeDriveVersionV1;
                time.monotonic_now_ns = 100;
                time.wall_now_ns = 1'700'000'000'000'000'000;
                assert(task->OnTime(time, &outputs) == 0);
                collect();
            }
            if (cancel) {
                task->Cancel();
                assert(task->Flush(&outputs) == -ECANCELED && outputs.empty());
                for (const auto& row : rows) assert(row[5] == 0);
            } else {
                assert(task->Flush(&outputs) == 0);
                collect();
                assert(rows.size() == 4);
                for (size_t i = 0; i < rows.size(); ++i) {
                    assert(rows[i][0] == rows.front()[0] && rows[i][1] == i + 1);
                    assert(rows[i][2] == (i + 1) * 10'000'000 && rows[i][3] == (i + 2) * 10'000'000);
                    assert(rows[i][4] == (i != 3) && rows[i][5] == (i == 3));
                    assert(rows[i][6] == 1 && rows[i][7] == 0 && rows[i][8] == tcp.bytes.size());
                    assert(rows[i][9] == (i == 0) && rows[i][10] == 0);
                    assert(rows[i][11] == (i == 0 ? tcp.bytes.size() : 0));
                }
                if (reference.empty())
                    reference = rows;
                else
                    assert(rows == reference);
                assert(task->Flush(&outputs) != 0 && outputs.empty());
            }
            schema.reset();
            provider.ReleaseTask(task);
        }
    }
}

void TestProtocolCatalogOpenAndDispatch() {
    ProtocolInputTrace first, second, control;
    auto catalog = npm::ProductionNpmModuleCatalogV1();
    assert(catalog.size() == 6);
    catalog.push_back(CountingRegistration("probe", 3, &first));
    catalog.push_back(CountingRegistration("mirror", 3, &second));
    catalog.push_back(CountingRegistration("control", 4, &control));
    npm::NpmBasicTaskConfig config;
    const char* json =
        R"({"input_namespace":"capture","source_domains":"1:77","features":"control,mirror,session,probe,basic","parameters":"{\"schema_version\":1,\"probe\":{\"marker\":7},\"disabled\":{\"bad\":true}}"})";
    assert(npm::ParseNpmBasicTaskConfig(json, &config, false, catalog).error == npm::NpmBasicTaskConfigError::kNone);
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    std::shared_ptr<arrow::Schema> schema;
    std::unique_ptr<npm::NpmBasicTaskRuntime> runtime;
    assert(npm::NpmBasicTaskRuntime::Create(config, &querier, flowsql::packet::PacketSchema(), &schema, &runtime, {},
                                            nullptr, catalog)
               .error == npm::NpmBasicTaskRuntimeError::kNone);
    assert(schema->Equals(*npm::NpmBasicResultSchema(), true));
    assert(first.json == R"({"marker":7})");
    assert(first.created == 1 && second.created == 1 && control.created == 1);
    // The task owns its plans and factories, independent of the injected directory and configuration lifetimes.
    catalog.clear();
    config.parameters_json.clear();
    auto tcp = MakeIpv4TcpPacket("192.0.2.1", 123, "192.0.2.2", 443, {1, 2});
    auto udp = MakeIpv6UdpPacket("2001:db8::1", 123, "2001:db8::2", 53, {3, 4, 5, 6});
    auto truncated = udp;
    truncated.bytes.resize(truncated.bytes.size() - 2);
    truncated.layer.status = flowsql::packet::LayerStatus::kTruncated;
    auto short_record = MakeBatchPacketRecord(truncated, 1, 40, 4);
    short_record.meta.wire_len = udp.bytes.size();
    auto input = MakeEncodedPacketBatch(
        {MakeBatchPacketRecord(tcp, 1, 10, 1), MakeBatchPacketRecord(MakeControlPacket(), 1, 20, 2),
         MakeBatchPacketRecord(udp, 1, 30, 3), short_record, MakeBatchPacketRecord(MakeControlPacket(true), 1, 50, 5)});
    std::shared_ptr<arrow::RecordBatch> output;
    assert(runtime->ProcessOfflineBatch(input, &output).error == npm::NpmBasicOfflineBatchError::kNone);
    assert(first.kinds.size() == 3 && first.kinds == second.kinds);
    assert(first.sizes == std::vector<size_t>({2, 4, 2}));
    assert(first.complete == std::vector<bool>({true, true, false}));
    assert(first.sessions == second.sessions && first.sessions[1] == first.sessions[2]);
    assert(control.kinds.size() == 2 && control.sessions == std::vector<uint64_t>({0, 0}));
    assert(control.sizes == std::vector<size_t>({12, 12}));
    assert(runtime->Sessions().size() == 2 && protocol.identify_pipelines.size() == 2);
    std::vector<npm::NpmSessionView> active;
    assert(runtime->Sessions().SnapshotActive(&active) == npm::NpmSessionTableError::kNone);
    assert(active[0].packets_ab + active[0].packets_ba == 1);
    assert(active[1].packets_ab + active[1].packets_ba == 2);
    assert(runtime->FlushOffline(60, &output).error == npm::NpmEofFlushError::kNone);
    assert(output->num_rows() == 2);
    assert(control.order == std::vector<std::string>({"packet", "packet"}));
    assert(first.order == std::vector<std::string>({"packet", "packet", "packet", "end", "end"}));
    assert(pool.release_calls == 1);
}

void TestProtocolOpenRejectionsAndControlCompatibility() {
    ProtocolInputTrace trace;
    auto catalog = npm::ProductionNpmModuleCatalogV1();
    catalog.push_back(CountingRegistration("probe", 3, &trace, true, std::vector<uint32_t>{1001}));
    npm::NpmBasicTaskConfig config;
    assert(npm::ParseNpmBasicTaskConfig(
               R"({"input_namespace":"capture","source_domains":"1:77","features":"basic,probe"})", &config, false,
               catalog)
               .error == npm::NpmBasicTaskConfigError::kNone);
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    auto schema = arrow::schema({});
    const auto original_schema = schema;
    std::unique_ptr<npm::NpmBasicTaskRuntime> runtime;
    auto status = npm::NpmBasicTaskRuntime::Create(config, &querier, flowsql::packet::PacketSchema(), &schema, &runtime,
                                                   {}, nullptr, catalog);
    assert(status.error == npm::NpmBasicTaskRuntimeError::kModulePlanError);
    assert(!runtime && schema == original_schema && trace.created == 0);
    assert(status.module_status.field.find("probe") != std::string::npos);
    assert(pool.acquire_calls == 0);
    for (const char* features : {"basic,basic", "basic,", "labeling", "basic,probe,probe"}) {
        const std::string text =
            std::string(R"({"input_namespace":"capture","source_domains":"1:77","features":")") + features + "\"}";
        assert(npm::ParseNpmBasicTaskConfig(text.c_str(), &config, false, catalog).error !=
               npm::NpmBasicTaskConfigError::kNone);
    }
    catalog.back().available = false;
    assert(npm::ParseNpmBasicTaskConfig(
               R"({"input_namespace":"capture","source_domains":"1:77","features":"basic,probe"})", &config, false,
               catalog)
               .error != npm::NpmBasicTaskConfigError::kNone);
    assert(
        npm::ParseNpmBasicTaskConfig(
            R"({"input_namespace":"capture","source_domains":"1:77","parameters":"{\"schema_version\":1,\"probe\":{\"bad\":true}}"})",
            &config, false, catalog)
            .error == npm::NpmBasicTaskConfigError::kNone);
    const int prepared_before = trace.prepared;
    assert(npm::NpmBasicTaskRuntime::Create(config, &querier, flowsql::packet::PacketSchema(), &schema, &runtime, {},
                                            nullptr, catalog)
               .error == npm::NpmBasicTaskRuntimeError::kNone);
    assert(trace.prepared == prepared_before);
    auto input = MakeEncodedPacketBatch({MakeBatchPacketRecord(MakeControlPacket(), 1, 10, 1)});
    std::shared_ptr<arrow::RecordBatch> output;
    assert(runtime->ProcessOfflineBatch(input, &output).error == npm::NpmBasicOfflineBatchError::kBatchProcessError);
    assert(protocol.identify_pipelines.empty());
    runtime.reset();
    catalog.back() = CountingRegistration("probe", 4, &trace);
    assert(npm::ParseNpmBasicTaskConfig(
               R"({"input_namespace":"capture","source_domains":"1:77","features":"probe","observing":"probe_event"})",
               &config, false, catalog)
               .error == npm::NpmBasicTaskConfigError::kNone);
    assert(npm::NpmBasicTaskRuntime::Create(config, &querier, flowsql::packet::PacketSchema(), &schema, &runtime, {},
                                            nullptr, catalog)
               .error == npm::NpmBasicTaskRuntimeError::kNone);
    assert(runtime->ProcessOfflineBatch(input, &output).error == npm::NpmBasicOfflineBatchError::kNone);
    assert(output->num_rows() == 0 && output->schema()->Equals(*schema, true));
    assert(runtime->Sessions().size() == 0 && protocol.identify_pipelines.empty());
    auto extended = MakeControlPacket(true);
    extended.bytes.insert(extended.bytes.begin() + sizeof(flowsql::Ipv6Header), 8, 0);
    auto* ip6 = reinterpret_cast<flowsql::Ipv6Header*>(extended.bytes.data());
    ip6->protocol = 44;
    ip6->payload = htons(20);
    extended.bytes[sizeof(flowsql::Ipv6Header)] = 58;
    extended.layer.transport_protocol = 44;
    extended.layer.layer_count = 2;
    extended.layer.layers[1] = {static_cast<uint16_t>(flowsql::eLayer::IPv6_EXT_FRAGMENT), sizeof(flowsql::Ipv6Header)};
    input = MakeEncodedPacketBatch({MakeBatchPacketRecord(extended, 1, 20, 2)});
    assert(runtime->ProcessOfflineBatch(input, &output).error == npm::NpmBasicOfflineBatchError::kNone);
    assert(trace.sizes.back() == 12);
    extended.bytes.resize(extended.bytes.size() - 3);
    extended.layer.status = flowsql::packet::LayerStatus::kTruncated;
    auto record = MakeBatchPacketRecord(extended, 1, 30, 3);
    record.meta.wire_len += 3;
    input = MakeEncodedPacketBatch({record});
    assert(runtime->ProcessOfflineBatch(input, &output).error == npm::NpmBasicOfflineBatchError::kNone);
    assert(trace.sizes.back() == 9 && !trace.complete.back());
    npm::NpmInputEventV1 event;
    const npm::NpmObservationDomainMap domains{"capture", {{1, 77}}};
    auto malformed = MakeControlPacket();
    malformed.layer.layers[0].offset = 999;
    assert(npm::BuildNpmControlInput(domains, malformed.View(1), malformed.layer, &event) !=
           npm::NpmSessionPacketError::kNone);
    malformed = MakeControlPacket();
    reinterpret_cast<flowsql::Ipv4Header*>(malformed.bytes.data())->fragment_offset = htons(1);
    assert(npm::BuildNpmControlInput(domains, malformed.View(1), malformed.layer, &event) ==
           npm::NpmSessionPacketError::kNonInitialFragment);
}

class ProtocolLabelMatcher final : public flowsql::IFlowLabelMatcherV1 {
 public:
    explicit ProtocolLabelMatcher(LabelingMatcherStats* stats) : stats_(stats) {}
    int ClassifyBatch(const flowsql::FlowLabelFactsV1* facts, uint32_t count, uint32_t* labels) const override {
        ++stats_->classify_calls;
        for (uint32_t i = 0; i < count; ++i) {
            stats_->facts.push_back(facts[i]);
            labels[i] = facts[i].destination.port == 443 ? 1001 : 0;
        }
        return 0;
    }
    bool FindLabel(uint32_t id, flowsql::FlowPrimaryLabelViewV1*) const override { return id == 1001; }
    void Release() noexcept override {
        ++stats_->release_calls;
        delete this;
    }

 private:
    LabelingMatcherStats* stats_;
};

class StreamContractModule final : public npm::INpmProtocolModuleV1, public npm::INpmTcpStreamConsumerV1 {
 public:
    explicit StreamContractModule(int* aborts) : aborts_(aborts) {}
    int OnInput(const npm::NpmInputEventV1&, npm::INpmResultEmitterV1&) override { return 0; }
    int OnSessionSnapshot(const npm::NpmSessionView&, int64_t, npm::INpmResultEmitterV1&) override { return 0; }
    int OnSessionEnd(const npm::NpmSessionView&, npm::NpmSessionEndReason, int64_t,
                     npm::INpmResultEmitterV1&) override {
        return 0;
    }
    std::optional<int64_t> NextEventDeadlineNs() const override { return {}; }
    int OnTime(const npm::NpmModuleTimeV1&, npm::INpmResultEmitterV1&) override { return 0; }
    int Finish(int64_t, npm::INpmResultEmitterV1&) override { return 0; }
    void Abort() noexcept override { ++*aborts_; }
    int OnTcpStreamReadable(const npm::NpmTcpStreamContextV1&, npm::INpmTcpStreamCursorV1&,
                            npm::INpmResultEmitterV1&) override {
        assert(false && "T0 must not dispatch TCP stream events");
        return EINVAL;
    }

 private:
    int* aborts_;
};

void TestNpmTcpStreamModuleInstanceContract() {
    static_assert(std::is_abstract_v<npm::INpmTcpStreamCursorV1>);
    static_assert(std::is_abstract_v<npm::INpmTcpStreamConsumerV1>);
    static_assert(!std::is_abstract_v<CountingProtocolModule>);  // Existing V1 needs no new overrides.
    using Error = npm::NpmProtocolContractErrorV1;
    npm::NpmModulePlanV1 plan;
    plan.module_id = "probe";
    plan.requires_tcp_stream = true;
    int aborts = 0;
    npm::NpmModuleInstanceV1 instance;
    auto module = std::make_unique<StreamContractModule>(&aborts);
    instance.tcp_stream_consumer = module.get();
    instance.protocol = std::move(module);
    assert(npm::ValidateNpmModuleInstanceV1(plan, instance).error == Error::kNone);
    auto* consumer = instance.tcp_stream_consumer;
    instance.tcp_stream_consumer = nullptr;
    auto status = npm::ValidateNpmModuleInstanceV1(plan, instance);
    assert(status.error == Error::kInvalidPlan && status.field == "probe/tcp_stream_consumer");
    StreamContractModule unrelated(&aborts);
    instance.tcp_stream_consumer = &unrelated;
    assert(npm::ValidateNpmModuleInstanceV1(plan, instance).error == Error::kInvalidPlan);
    instance.tcp_stream_consumer = consumer;
    auto moved = std::move(instance);
    assert(npm::ValidateNpmModuleInstanceV1(plan, moved).error == Error::kNone);
    plan.requires_tcp_stream = false;
    assert(npm::ValidateNpmModuleInstanceV1(plan, moved).error == Error::kInvalidPlan);
    moved.tcp_stream_consumer = nullptr;
    assert(npm::ValidateNpmModuleInstanceV1(plan, moved).error == Error::kNone);
    ProtocolInputTrace trace;
    moved.protocol = std::make_unique<CountingProtocolModule>(&trace);
    assert(npm::ValidateNpmModuleInstanceV1(plan, moved).error == Error::kNone);
    plan.requires_tcp_stream = true;
    moved.tcp_stream_consumer = &unrelated;
    assert(npm::ValidateNpmModuleInstanceV1(plan, moved).error == Error::kInvalidPlan);
    moved.protocol.reset();
    assert(npm::ValidateNpmModuleInstanceV1(plan, moved).error == Error::kInvalidPlan);
    moved.tcp_stream_consumer = nullptr;
    plan.requires_tcp_stream = false;
    assert(npm::ValidateNpmModuleInstanceV1(plan, moved).error == Error::kInvalidPlan);
    plan.module_id = "basic";
    assert(npm::ValidateNpmModuleInstanceV1(plan, moved).error == Error::kNone);
    assert(aborts == 0);  // Validation has no lifecycle side effects.
}

void TestNpmTcpStreamOpenGatesAndFrozenConfig() {
    using Error = npm::NpmProtocolContractErrorV1;
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    auto schema = arrow::schema({});
    const auto original_schema = schema;
    std::unique_ptr<npm::NpmBasicTaskRuntime> runtime;
    npm::NpmBasicTaskConfig config;
    std::string json =
        R"({"parameters":"{\"schema_version\":1,\"core\":{\"tcp_stream\":{\"max_buffered_bytes_per_direction\":131072,\"gap_timeout_ns\":12345}}}"})";
    assert(npm::ParseNpmBasicTaskConfig(json.c_str(), &config).error == npm::NpmBasicTaskConfigError::kNone);
    const auto expected_json = config.parameters_json;
    assert(config.features.module_ids == std::vector<std::string>{"basic"});
    assert(!config.features.session_enabled && !config.features.labeling_enabled);
    assert(
        npm::NpmBasicTaskRuntime::Create(config, &querier, flowsql::packet::PacketSchema(), &schema, &runtime).error ==
        npm::NpmBasicTaskRuntimeError::kNone);
    json.assign(json.size(), 'x');
    config.parameters_json.clear();
    config.tcp_stream = {};
    assert(runtime->Config().tcp_stream.max_buffered_bytes_per_direction == 131072);
    assert(runtime->Config().tcp_stream.gap_timeout_ns == 12345);
    assert(runtime->Config().parameters_json == expected_json);
    assert(runtime->Modules().size() == 1);
    assert(runtime->Sessions().size() == 0);
    assert(runtime->Budget()->Usage().module_state_bytes == 0);
    runtime.reset();
    schema = original_schema;

    ProtocolInputTrace trace;
    LabelingMatcherStats matcher_stats;
    for (const auto& labels : std::vector<std::vector<uint32_t>>{{1001}, {}, {0}, {1001, 1001}, {999}}) {
        const int created_before = trace.created;
        const int aborted_before = trace.aborted;
        auto catalog = npm::ProductionNpmModuleCatalogV1();
        catalog.push_back(CountingRegistration("probe", 1, &trace, true, labels));
        assert(npm::ParseNpmBasicTaskConfig(R"({"features":"basic,labeling,probe"})", &config, true, catalog).error ==
               npm::NpmBasicTaskConfigError::kNone);
        const auto status =
            npm::NpmBasicTaskRuntime::Create(config, &querier, flowsql::packet::PacketSchema(), &schema, &runtime, {},
                                             new ProtocolLabelMatcher(&matcher_stats), catalog);
        const bool valid_labels = labels == std::vector<uint32_t>{1001};
        assert(status.error == (valid_labels ? npm::NpmBasicTaskRuntimeError::kModuleCreateError
                                             : npm::NpmBasicTaskRuntimeError::kModulePlanError));
        assert(status.module_status.error == (valid_labels ? Error::kInvalidPlan : Error::kInvalidLabelSelection));
        assert(status.module_status.field == (valid_labels ? "probe/tcp_stream_consumer" : "probe/primary_label_ids"));
        assert(!runtime && schema == original_schema);
        assert(trace.created == created_before + static_cast<int>(valid_labels));
        assert(trace.aborted == aborted_before + static_cast<int>(valid_labels));
    }
    assert(matcher_stats.release_calls == 5 && matcher_stats.classify_calls == 0);

    // An undeclared consumer must fail before publishing the runtime, aborting already-created modules as well.
    int aborts = 0;
    auto catalog = npm::ProductionNpmModuleCatalogV1();
    catalog.push_back(CountingRegistration("prior", 1, &trace));
    auto invalid = CountingRegistration("probe", 1, &trace);
    const auto prepare = invalid.prepare;
    invalid.prepare = [prepare, &aborts](const npm::NpmBasicTaskConfig& cfg, std::string_view text,
                                         npm::NpmPreparedModuleV1* out) {
        auto status = prepare(cfg, text, out);
        out->create = [&aborts](npm::NpmProtocolContext&, std::shared_ptr<npm::INpmTaskBudget>) {
            npm::NpmModuleInstanceV1 instance;
            auto module = std::make_unique<StreamContractModule>(&aborts);
            instance.tcp_stream_consumer = module.get();
            instance.protocol = std::move(module);
            return instance;
        };
        return status;
    };
    catalog.push_back(std::move(invalid));
    assert(npm::ParseNpmBasicTaskConfig(R"({"features":"basic,prior,probe"})", &config, false, catalog).error ==
           npm::NpmBasicTaskConfigError::kNone);
    auto budget = std::make_shared<npm::NpmTaskBudget>(config.analysis);
    const int prior_created = trace.created;
    const int prior_aborted = trace.aborted;
    const auto status = npm::NpmBasicTaskRuntime::Create(config, &querier, flowsql::packet::PacketSchema(), &schema,
                                                         &runtime, budget, nullptr, catalog);
    assert(status.error == npm::NpmBasicTaskRuntimeError::kModuleCreateError);
    assert(status.module_status.error == Error::kInvalidPlan &&
           status.module_status.field == "probe/tcp_stream_consumer");
    assert(!runtime && schema == original_schema);
    assert(aborts == 1 && trace.created == prior_created + 1 && trace.aborted == prior_aborted + 1);
    assert(npm::NpmTrackedBudgetBytes(budget->Usage()) == 0);
    assert(pool.acquire_calls == pool.release_calls);
}

void TestDnsT0OpenContract() {
    using Error = npm::NpmBasicTaskRuntimeError;
    using ContractError = npm::NpmProtocolContractErrorV1;
    const auto& production = npm::ProductionNpmModuleCatalogV1();
    assert(production.size() == 6 && production[0].module_id == "basic" && production[1].module_id == "session" &&
           production[2].module_id == "dns" && production[3].module_id == "http1" && production[4].module_id == "tls");
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    LabelingMatcherStats matcher_stats;
    int aborts = 0;
    StreamContractModule unrelated(&aborts);
    int consumer_mode = 0;  // 0: valid; 1: missing; 2: wrong object.
    auto catalog = production;
    catalog.erase(catalog.begin() + 2);  // Replace production DNS with the controllable test module.
    npm::NpmModuleRegistrationV1 registration;
    registration.module_id = "dns";
    registration.entity_ids = {"dns_transaction"};
    registration.prepare = [&aborts, &consumer_mode, &unrelated](const npm::NpmBasicTaskConfig&, std::string_view json,
                                                                 npm::NpmPreparedModuleV1* output) {
        npm::NpmDnsConfigV1 dns;
        const auto parsed = npm::ParseNpmDnsConfigV1(json, &dns);
        if (parsed.error != npm::NpmDnsConfigErrorV1::kNone) {
            const std::string prefix = "/dns/";
            return npm::NpmProtocolContractStatusV1{
                ContractError::kInvalidPlan, parsed.path == "/dns" ? "config" : parsed.path.substr(prefix.size())};
        }
        output->plan = npm::NpmDnsModulePlanV1(dns);
        output->create = [&aborts, &consumer_mode, &unrelated](npm::NpmProtocolContext&,
                                                               std::shared_ptr<npm::INpmTaskBudget>) {
            npm::NpmModuleInstanceV1 instance;
            auto module = std::make_unique<StreamContractModule>(&aborts);
            instance.tcp_stream_consumer = consumer_mode == 1   ? nullptr
                                           : consumer_mode == 2 ? &unrelated
                                                                : module.get();
            instance.protocol = std::move(module);
            return instance;
        };
        return npm::NpmProtocolContractStatusV1{};
    };
    catalog[2] = std::move(registration);

    const auto parse = [&](const char* json, bool labeling = true) {
        npm::NpmBasicTaskConfig config;
        assert(npm::ParseNpmBasicTaskConfig(json, &config, labeling, catalog).error ==
               npm::NpmBasicTaskConfigError::kNone);
        return config;
    };
    const auto open = [&](const npm::NpmBasicTaskConfig& config, bool matcher_available,
                          std::shared_ptr<arrow::Schema>* schema, std::unique_ptr<npm::NpmBasicTaskRuntime>* runtime) {
        return npm::NpmBasicTaskRuntime::Create(
            config, &querier, flowsql::packet::PacketSchema(), schema, runtime, {},
            matcher_available ? static_cast<flowsql::IFlowLabelMatcherV1*>(new ProtocolLabelMatcher(&matcher_stats))
                              : nullptr,
            catalog);
    };
    auto sentinel = arrow::schema({arrow::field("sentinel", arrow::int8())});
    auto schema = sentinel;
    std::unique_ptr<npm::NpmBasicTaskRuntime> runtime;
    const char* valid =
        R"({"features":"dns,labeling","observing":"dns_transaction","parameters":"{\"schema_version\":1,\"dns\":{\"primary_label_ids\":[1001]}}"})";
    auto config = parse(valid);
    auto status = open(config, false, &schema, &runtime);
    assert(status.error == Error::kLabelingMatcherMissing);
    assert(schema == sentinel && !runtime);
    config = parse(
        R"({"features":"dns","observing":"dns_transaction","parameters":"{\"schema_version\":1,\"dns\":{\"primary_label_ids\":[1001]}}"})");
    status = open(config, false, &schema, &runtime);
    assert(status.error == Error::kModulePlanError &&
           status.module_status.error == ContractError::kUnavailableCapability);
    assert(schema == sentinel && !runtime);
    config = parse(
        R"({"features":"dns,labeling","observing":"dns_transaction","parameters":"{\"schema_version\":1,\"dns\":{\"primary_label_ids\":[999]}}"})");
    status = open(config, true, &schema, &runtime);
    assert(status.error == Error::kModulePlanError &&
           status.module_status.error == ContractError::kInvalidLabelSelection);
    assert(schema == sentinel && !runtime);
    for (const char* node : {"{}", R"({"primary_label_ids":[]})", R"({"primary_label_ids":[1001],"bad":1})"}) {
        npm::NpmBasicTaskConfig invalid = parse(valid);
        invalid.parameters_json = std::string(R"({"schema_version":1,"dns":)") + node + "}";
        status = open(invalid, true, &schema, &runtime);
        assert(status.error == Error::kModulePlanError && status.module_status.error == ContractError::kInvalidPlan);
        assert(schema == sentinel && !runtime);
    }
    config =
        parse(R"({"features":"dns,labeling","observing":"dns_transaction","parameters":"{\"schema_version\":1}"})");
    status = open(config, true, &schema, &runtime);
    assert(status.error == Error::kModulePlanError && status.module_status.field == "dns/primary_label_ids");
    assert(schema == sentinel && !runtime);
    config = parse(valid);
    for (consumer_mode = 1; consumer_mode <= 2; ++consumer_mode) {
        const int previous_aborts = aborts;
        status = open(config, true, &schema, &runtime);
        assert(status.error == Error::kModuleCreateError && status.module_status.field == "dns/tcp_stream_consumer");
        assert(aborts == previous_aborts + 1 && schema == sentinel && !runtime);
    }
    consumer_mode = 0;
    status = open(config, true, &schema, &runtime);
    assert(status.error == Error::kNone && runtime);
    assert(schema->Equals(*npm::NpmDnsTransactionEntityDescriptorV1().schema, true));
    runtime->Cancel();
    runtime.reset();
    config = parse(
        R"({"features":"basic","observing":"basic","parameters":"{\"schema_version\":1,\"dns\":{\"unknown\":null}}"})");
    status = open(config, false, &schema, &runtime);
    assert(status.error == Error::kNone && runtime);
    assert(schema->Equals(*npm::NpmBasicResultSchema(), true) && runtime->Modules().size() == 1);
    runtime->Cancel();
    runtime.reset();
    assert(pool.acquire_calls == pool.release_calls);
    assert(npm::ParseNpmBasicTaskConfig(R"({"features":"basic","observing":"dns_transaction"})", &config, true, catalog)
               .error == npm::NpmBasicTaskConfigError::kObservingFeatureDisabled);
    assert(
        npm::ParseNpmBasicTaskConfig(R"({"features":"dns,labeling","observing":"dns_transaction"})", &config).error ==
        npm::NpmBasicTaskConfigError::kNone);
    schema = sentinel;
    status = npm::NpmBasicTaskRuntime::Create(config, &querier, flowsql::packet::PacketSchema(), &schema, &runtime, {},
                                              new ProtocolLabelMatcher(&matcher_stats));
    assert(status.error == Error::kModulePlanError && status.module_status.field == "dns/primary_label_ids");
    assert(schema == sentinel && !runtime);
}

void TestHttp1T0OpenContract() {
    using Error = npm::NpmBasicTaskRuntimeError;
    using ContractError = npm::NpmProtocolContractErrorV1;
    const auto& production = npm::ProductionNpmModuleCatalogV1();
    assert(production.size() == 6 && production[0].module_id == "basic" && production[1].module_id == "session" &&
           production[2].module_id == "dns" && production[3].module_id == "http1" && production[4].module_id == "tls");
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    LabelingMatcherStats matcher_stats;
    int aborts = 0;
    StreamContractModule unrelated(&aborts);
    int consumer_mode = 0;  // 0: valid; 1: missing; 2: wrong object.
    auto catalog = production;
    npm::NpmModuleRegistrationV1 registration;
    registration.module_id = "http1";
    registration.entity_ids = {"http1_transaction"};
    registration.prepare = [&aborts, &consumer_mode, &unrelated](const npm::NpmBasicTaskConfig&, std::string_view json,
                                                                 npm::NpmPreparedModuleV1* output) {
        npm::NpmHttp1ConfigV1 http1;
        const auto parsed = npm::ParseNpmHttp1ConfigV1(json, &http1);
        if (parsed.error != npm::NpmHttp1ConfigErrorV1::kNone) {
            const std::string prefix = "/http1/";
            return npm::NpmProtocolContractStatusV1{
                ContractError::kInvalidPlan, parsed.path == "/http1" ? "config" : parsed.path.substr(prefix.size())};
        }
        output->plan = npm::NpmHttp1ModulePlanV1(http1);
        output->create = [&aborts, &consumer_mode, &unrelated](npm::NpmProtocolContext&,
                                                               std::shared_ptr<npm::INpmTaskBudget>) {
            npm::NpmModuleInstanceV1 instance;
            auto module = std::make_unique<StreamContractModule>(&aborts);
            instance.tcp_stream_consumer = consumer_mode == 1   ? nullptr
                                           : consumer_mode == 2 ? &unrelated
                                                                : module.get();
            instance.protocol = std::move(module);
            return instance;
        };
        return npm::NpmProtocolContractStatusV1{};
    };
    catalog[3] = std::move(registration);

    const auto parse = [&](const char* json, bool labeling = true) {
        npm::NpmBasicTaskConfig config;
        assert(npm::ParseNpmBasicTaskConfig(json, &config, labeling, catalog).error ==
               npm::NpmBasicTaskConfigError::kNone);
        return config;
    };
    const auto open = [&](const npm::NpmBasicTaskConfig& config, bool matcher_available,
                          std::shared_ptr<arrow::Schema>* schema, std::unique_ptr<npm::NpmBasicTaskRuntime>* runtime) {
        return npm::NpmBasicTaskRuntime::Create(
            config, &querier, flowsql::packet::PacketSchema(), schema, runtime, {},
            matcher_available ? static_cast<flowsql::IFlowLabelMatcherV1*>(new ProtocolLabelMatcher(&matcher_stats))
                              : nullptr,
            catalog);
    };
    auto sentinel = arrow::schema({arrow::field("sentinel", arrow::int8())});
    auto schema = sentinel;
    std::unique_ptr<npm::NpmBasicTaskRuntime> runtime;
    const char* valid =
        R"({"features":"http1,labeling","observing":"http1_transaction","parameters":"{\"schema_version\":1,\"http1\":{\"primary_label_ids\":[1001]}}"})";
    auto config = parse(valid);
    auto status = open(config, false, &schema, &runtime);
    assert(status.error == Error::kLabelingMatcherMissing && schema == sentinel && !runtime);
    config = parse(
        R"({"features":"http1","observing":"http1_transaction","parameters":"{\"schema_version\":1,\"http1\":{\"primary_label_ids\":[1001]}}"})");
    status = open(config, false, &schema, &runtime);
    assert(status.error == Error::kModulePlanError &&
           status.module_status.error == ContractError::kUnavailableCapability && schema == sentinel && !runtime);
    config = parse(
        R"({"features":"http1,labeling","observing":"http1_transaction","parameters":"{\"schema_version\":1,\"http1\":{\"primary_label_ids\":[999]}}"})");
    status = open(config, true, &schema, &runtime);
    assert(status.error == Error::kModulePlanError &&
           status.module_status.error == ContractError::kInvalidLabelSelection && schema == sentinel && !runtime);
    for (const char* node :
         {"{}", "null", R"({"primary_label_ids":[]})", R"({"primary_label_ids":[1001,1001]})",
          R"({"primary_label_ids":[1001],"bad":1})", R"({"primary_label_ids":[1001],"max_header_bytes":1023})",
          R"({"primary_label_ids":[1001],"primary_label_ids":[1001]})"}) {
        npm::NpmBasicTaskConfig invalid = parse(valid);
        invalid.parameters_json = std::string(R"({"schema_version":1,"http1":)") + node + "}";
        status = open(invalid, true, &schema, &runtime);
        assert(status.error == Error::kModulePlanError && status.module_status.error == ContractError::kInvalidPlan &&
               schema == sentinel && !runtime);
    }
    config =
        parse(R"({"features":"http1,labeling","observing":"http1_transaction","parameters":"{\"schema_version\":1}"})");
    status = open(config, true, &schema, &runtime);
    assert(status.error == Error::kModulePlanError && status.module_status.field == "http1/primary_label_ids" &&
           schema == sentinel && !runtime);
    config = parse(valid);
    for (consumer_mode = 1; consumer_mode <= 2; ++consumer_mode) {
        const int previous_aborts = aborts;
        status = open(config, true, &schema, &runtime);
        assert(status.error == Error::kModuleCreateError && status.module_status.field == "http1/tcp_stream_consumer");
        assert(aborts == previous_aborts + 1 && schema == sentinel && !runtime);
    }
    consumer_mode = 0;
    status = open(config, true, &schema, &runtime);
    assert(status.error == Error::kNone && runtime);
    assert(schema->Equals(*npm::NpmHttp1TransactionEntityDescriptorV1().schema, true));
    runtime->Cancel();
    runtime.reset();
    config = parse(
        R"({"features":"basic","observing":"basic","parameters":"{\"schema_version\":1,\"http1\":{\"unknown\":null}}"})");
    status = open(config, false, &schema, &runtime);
    assert(status.error == Error::kNone && runtime);
    assert(schema->Equals(*npm::NpmBasicResultSchema(), true) && runtime->Modules().size() == 1);
    runtime->Cancel();
    runtime.reset();
    assert(pool.acquire_calls == pool.release_calls);
    assert(
        npm::ParseNpmBasicTaskConfig(R"({"features":"basic","observing":"http1_transaction"})", &config, true, catalog)
            .error == npm::NpmBasicTaskConfigError::kObservingFeatureDisabled);
    config = parse(valid);
    schema = sentinel;
    status = npm::NpmBasicTaskRuntime::Create(config, &querier, flowsql::packet::PacketSchema(), &schema, &runtime, {},
                                              new ProtocolLabelMatcher(&matcher_stats));
    assert(status.error == Error::kNone && runtime);
    assert(schema->Equals(*npm::NpmHttp1TransactionEntityDescriptorV1().schema, true));
    runtime->Cancel();
    runtime.reset();
}

class IcmpContractModule final : public npm::INpmProtocolModuleV1 {
 public:
    int OnInput(const npm::NpmInputEventV1&, npm::INpmResultEmitterV1&) override { return 0; }
    int OnSessionSnapshot(const npm::NpmSessionView&, int64_t, npm::INpmResultEmitterV1&) override { return EINVAL; }
    int OnSessionEnd(const npm::NpmSessionView&, npm::NpmSessionEndReason, int64_t,
                     npm::INpmResultEmitterV1&) override {
        return EINVAL;
    }
    std::optional<int64_t> NextEventDeadlineNs() const override { return {}; }
    int OnTime(const npm::NpmModuleTimeV1&, npm::INpmResultEmitterV1&) override { return 0; }
    int Finish(int64_t, npm::INpmResultEmitterV1&) override { return 0; }
    void Abort() noexcept override {}
};

void TestIcmpT0OpenContract() {
    using Error = npm::NpmBasicTaskRuntimeError;
    using ContractError = npm::NpmProtocolContractErrorV1;
    auto catalog = npm::ProductionNpmModuleCatalogV1();
    catalog.pop_back();  // Replace production ICMP with the controllable Open contract module.
    npm::NpmModuleRegistrationV1 registration;
    registration.module_id = "icmp";
    registration.entity_ids = {"icmp_event"};
    registration.prepare = [](const npm::NpmBasicTaskConfig&, std::string_view json, npm::NpmPreparedModuleV1* out) {
        npm::NpmIcmpConfigV1 config;
        const auto parsed = npm::ParseNpmIcmpConfigV1(json, &config);
        if (parsed.error != npm::NpmIcmpConfigErrorV1::kNone) {
            return npm::NpmProtocolContractStatusV1{ContractError::kInvalidPlan,
                                                    parsed.path == "/icmp" ? "config" : parsed.path.substr(6)};
        }
        out->plan = npm::NpmIcmpModulePlanV1();
        out->create = [](npm::NpmProtocolContext&, std::shared_ptr<npm::INpmTaskBudget>) {
            npm::NpmModuleInstanceV1 instance;
            instance.protocol = std::make_unique<IcmpContractModule>();
            return instance;
        };
        return npm::NpmProtocolContractStatusV1{};
    };
    catalog.push_back(std::move(registration));

    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    const auto parse = [&](const char* json) {
        npm::NpmBasicTaskConfig config;
        assert(npm::ParseNpmBasicTaskConfig(json, &config, false, catalog).error ==
               npm::NpmBasicTaskConfigError::kNone);
        return config;
    };
    auto config = parse(R"({"features":"icmp","observing":"icmp_event"})");
    auto sentinel = arrow::schema({arrow::field("sentinel", arrow::int8())});
    auto schema = sentinel;
    std::unique_ptr<npm::NpmBasicTaskRuntime> runtime;
    auto status = npm::NpmBasicTaskRuntime::Create(config, &querier, flowsql::packet::PacketSchema(), &schema, &runtime,
                                                   {}, nullptr, npm::ProductionNpmModuleCatalogV1());
    assert(status.error == Error::kNone && runtime);
    assert(schema->Equals(*npm::NpmIcmpEventEntityDescriptorV1().schema, true));
    runtime->Cancel();
    runtime.reset();
    schema = sentinel;

    for (const char* node : {"null", R"({"unknown":1})", R"({"echo_timeout_ns":999999})", R"({"max_pending_echo":0})",
                             R"({"max_pending_echo":1,"max_pending_echo":2})"}) {
        auto invalid = config;
        invalid.parameters_json = std::string(R"({"schema_version":1,"icmp":)") + node + "}";
        status = npm::NpmBasicTaskRuntime::Create(invalid, &querier, flowsql::packet::PacketSchema(), &schema, &runtime,
                                                  {}, nullptr, catalog);
        assert(status.error == Error::kModulePlanError && status.module_status.error == ContractError::kInvalidPlan &&
               !runtime && schema == sentinel);
    }
    status = npm::NpmBasicTaskRuntime::Create(config, &querier, flowsql::packet::PacketSchema(), &schema, &runtime, {},
                                              nullptr, catalog);
    assert(status.error == Error::kNone && runtime);
    assert(schema->Equals(*npm::NpmIcmpEventEntityDescriptorV1().schema, true));
    runtime->Cancel();
    runtime.reset();

    config = parse(
        R"({"features":"basic","observing":"basic","parameters":"{\"schema_version\":1,\"icmp\":{\"unknown\":null}}"})");
    status = npm::NpmBasicTaskRuntime::Create(config, &querier, flowsql::packet::PacketSchema(), &schema, &runtime, {},
                                              nullptr, catalog);
    assert(status.error == Error::kNone && runtime && schema->Equals(*npm::NpmBasicResultSchema(), true));
    runtime->Cancel();
    runtime.reset();
    assert(npm::ParseNpmBasicTaskConfig(R"({"features":"basic","observing":"icmp_event"})", &config, false, catalog)
               .error == npm::NpmBasicTaskConfigError::kObservingFeatureDisabled);
    assert(pool.acquire_calls == pool.release_calls);
}

void TestIcmpMixedBatchAndSplitEquivalence() {
    auto request = MakeControlPacket();
    request.bytes[20] = 8;
    request.bytes[21] = 0;
    request.bytes[24] = 0x12;
    request.bytes[25] = 0x34;
    request.bytes[26] = 0;
    request.bytes[27] = 9;
    auto reply = request;
    reply.bytes[20] = 0;
    auto* reply_ip = reinterpret_cast<flowsql::Ipv4Header*>(reply.bytes.data());
    std::swap(reply_ip->src_addr, reply_ip->dst_addr);
    std::swap(reply.layer.src_ip, reply.layer.dst_ip);
    const auto tcp = MakeIpv4TcpPacket("192.0.2.10", 40000, "198.51.100.10", 443, {});
    const auto udp = MakeIpv6UdpPacket("2001:db8::1", 53000, "2001:db8::2", 53, {1, 2});
    const std::vector<flowsql::packet::PacketRecord> records = {
        MakeBatchPacketRecord(tcp, 1, 10, 1), MakeBatchPacketRecord(request, 1, 20, 2),
        MakeBatchPacketRecord(udp, 1, 30, 3), MakeBatchPacketRecord(reply, 1, 50, 4)};

    const auto run = [&](bool split) {
        npm::NpmBasicTaskConfig config;
        assert(
            npm::ParseNpmBasicTaskConfig(
                R"({"input_namespace":"capture","source_domains":"1:77","features":"icmp","observing":"icmp_event"})",
                &config, false, npm::ProductionNpmModuleCatalogV1())
                .error == npm::NpmBasicTaskConfigError::kNone);
        ContextDictionary dictionary;
        ContextProtocol protocol(&dictionary);
        ContextPool pool(&protocol);
        SinglePoolQuerier querier(&pool);
        std::shared_ptr<arrow::Schema> schema;
        std::unique_ptr<npm::NpmBasicTaskRuntime> runtime;
        assert(npm::NpmBasicTaskRuntime::Create(config, &querier, flowsql::packet::PacketSchema(), &schema, &runtime)
                   .error == npm::NpmBasicTaskRuntimeError::kNone);
        std::shared_ptr<arrow::RecordBatch> output;
        if (split) {
            auto first = MakeEncodedPacketBatch(
                std::vector<flowsql::packet::PacketRecord>(records.begin(), records.begin() + 3));
            assert(runtime->ProcessOfflineBatch(first, &output).error == npm::NpmBasicOfflineBatchError::kNone);
            assert(output && output->num_rows() == 0);
            first.reset();
            auto second = MakeEncodedPacketBatch({records.back()});
            assert(runtime->ProcessOfflineBatch(second, &output).error == npm::NpmBasicOfflineBatchError::kNone);
            second.reset();
        } else {
            auto input = MakeEncodedPacketBatch(records);
            assert(runtime->ProcessOfflineBatch(input, &output).error == npm::NpmBasicOfflineBatchError::kNone);
            input.reset();
        }
        assert(output && output->num_rows() == 1 && output->schema()->Equals(*schema, true));
        assert(BasicResultColumn<arrow::StringArray>(output, 8)->GetString(0) == "echo_matched");
        assert(BasicResultColumn<arrow::UInt64Array>(output, 4)->Value(0) == 77);
        assert(BasicResultColumn<arrow::Int64Array>(output, 18)->Value(0) == 30);
        assert(runtime->Sessions().size() == 2);
        std::shared_ptr<arrow::RecordBatch> tail;
        assert(runtime->FlushOffline(60, &tail).error == npm::NpmEofFlushError::kNone);
        assert(tail && tail->num_rows() == 0);
        return output;
    };
    assert(run(false)->Equals(*run(true), true));
}

void TestTlsT0OpenContract() {
    using Error = npm::NpmBasicTaskRuntimeError;
    using ContractError = npm::NpmProtocolContractErrorV1;
    const auto& production = npm::ProductionNpmModuleCatalogV1();
    assert(production.size() == 6 && production[4].module_id == "tls");

    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    LabelingMatcherStats matcher_stats;
    int aborts = 0;
    StreamContractModule unrelated(&aborts);
    int consumer_mode = 0;  // 0: same object; 1: absent; 2: unrelated object.
    auto catalog = production;
    catalog.erase(catalog.begin() + 4);  // Replace production TLS with the controllable Open contract module.
    npm::NpmModuleRegistrationV1 registration;
    registration.module_id = "tls";
    registration.entity_ids = {"tls_handshake"};
    registration.prepare = [&aborts, &consumer_mode, &unrelated](const npm::NpmBasicTaskConfig&, std::string_view json,
                                                                 npm::NpmPreparedModuleV1* output) {
        npm::NpmTlsConfigV1 tls;
        const auto parsed = npm::ParseNpmTlsConfigV1(json, &tls);
        if (parsed.error != npm::NpmTlsConfigErrorV1::kNone) {
            const std::string prefix = "/tls/";
            return npm::NpmProtocolContractStatusV1{
                ContractError::kInvalidPlan, parsed.path == "/tls" ? "config" : parsed.path.substr(prefix.size())};
        }
        output->plan = npm::NpmTlsModulePlanV1(tls);
        output->create = [&aborts, &consumer_mode, &unrelated](npm::NpmProtocolContext&,
                                                               std::shared_ptr<npm::INpmTaskBudget>) {
            npm::NpmModuleInstanceV1 instance;
            auto module = std::make_unique<StreamContractModule>(&aborts);
            instance.tcp_stream_consumer = consumer_mode == 1   ? nullptr
                                           : consumer_mode == 2 ? &unrelated
                                                                : module.get();
            instance.protocol = std::move(module);
            return instance;
        };
        return npm::NpmProtocolContractStatusV1{};
    };
    catalog.push_back(std::move(registration));

    const auto parse = [&](const char* json, bool labeling = true) {
        npm::NpmBasicTaskConfig config;
        assert(npm::ParseNpmBasicTaskConfig(json, &config, labeling, catalog).error ==
               npm::NpmBasicTaskConfigError::kNone);
        return config;
    };
    const auto open = [&](const npm::NpmBasicTaskConfig& config, bool matcher_available,
                          std::shared_ptr<arrow::Schema>* schema, std::unique_ptr<npm::NpmBasicTaskRuntime>* runtime) {
        return npm::NpmBasicTaskRuntime::Create(
            config, &querier, flowsql::packet::PacketSchema(), schema, runtime, {},
            matcher_available ? static_cast<flowsql::IFlowLabelMatcherV1*>(new ProtocolLabelMatcher(&matcher_stats))
                              : nullptr,
            catalog);
    };
    const char* valid =
        R"({"features":"tls,labeling","observing":"tls_handshake","parameters":"{\"schema_version\":1,\"tls\":{\"primary_label_ids\":[1001]}}"})";
    npm::NpmBasicTaskConfig production_rejected;
    assert(npm::ParseNpmBasicTaskConfig(valid, &production_rejected, true, production).error ==
           npm::NpmBasicTaskConfigError::kNone);

    auto sentinel = arrow::schema({arrow::field("sentinel", arrow::int8())});
    auto schema = sentinel;
    std::unique_ptr<npm::NpmBasicTaskRuntime> runtime;
    auto config = parse(valid);
    auto status = open(config, false, &schema, &runtime);
    assert(status.error == Error::kLabelingMatcherMissing && schema == sentinel && !runtime);
    config = parse(
        R"({"features":"tls","observing":"tls_handshake","parameters":"{\"schema_version\":1,\"tls\":{\"primary_label_ids\":[1001]}}"})");
    status = open(config, false, &schema, &runtime);
    assert(status.error == Error::kModulePlanError &&
           status.module_status.error == ContractError::kUnavailableCapability && schema == sentinel && !runtime);
    config = parse(
        R"({"features":"tls,labeling","observing":"tls_handshake","parameters":"{\"schema_version\":1,\"tls\":{\"primary_label_ids\":[999]}}"})");
    status = open(config, true, &schema, &runtime);
    assert(status.error == Error::kModulePlanError &&
           status.module_status.error == ContractError::kInvalidLabelSelection && schema == sentinel && !runtime);

    for (const char* node :
         {"{}", "null", R"({"primary_label_ids":[]})", R"({"primary_label_ids":[1001,1001]})",
          R"({"primary_label_ids":[1001],"bad":1})", R"({"primary_label_ids":[1001],"max_hello_bytes":4095})",
          R"({"primary_label_ids":[1001],"primary_label_ids":[1001]})"}) {
        npm::NpmBasicTaskConfig invalid = parse(valid);
        invalid.parameters_json = std::string(R"({"schema_version":1,"tls":)") + node + "}";
        status = open(invalid, true, &schema, &runtime);
        assert(status.error == Error::kModulePlanError && status.module_status.error == ContractError::kInvalidPlan &&
               schema == sentinel && !runtime);
    }
    config = parse(R"({"features":"tls,labeling","observing":"tls_handshake","parameters":"{\"schema_version\":1}"})");
    status = open(config, true, &schema, &runtime);
    assert(status.error == Error::kModulePlanError && status.module_status.field == "tls/primary_label_ids" &&
           schema == sentinel && !runtime);

    config = parse(valid);
    for (consumer_mode = 1; consumer_mode <= 2; ++consumer_mode) {
        const int previous_aborts = aborts;
        status = open(config, true, &schema, &runtime);
        assert(status.error == Error::kModuleCreateError && status.module_status.field == "tls/tcp_stream_consumer");
        assert(aborts == previous_aborts + 1 && schema == sentinel && !runtime);
    }
    consumer_mode = 0;
    status = open(config, true, &schema, &runtime);
    assert(status.error == Error::kNone && runtime);
    assert(schema->Equals(*npm::NpmTlsHandshakeEntityDescriptorV1().schema, true));
    runtime->Cancel();
    runtime.reset();

    config = parse(
        R"({"features":"basic","observing":"basic","parameters":"{\"schema_version\":1,\"tls\":{\"unknown\":null}}"})");
    status = open(config, false, &schema, &runtime);
    assert(status.error == Error::kNone && runtime);
    assert(schema->Equals(*npm::NpmBasicResultSchema(), true) && runtime->Modules().size() == 1);
    runtime->Cancel();
    runtime.reset();
    assert(pool.acquire_calls == pool.release_calls);
    assert(npm::ParseNpmBasicTaskConfig(R"({"features":"basic","observing":"tls_handshake"})", &config, true, catalog)
               .error == npm::NpmBasicTaskConfigError::kObservingFeatureDisabled);
}

struct ProtocolLifecycleTrace {
    int created = 0;
    int finish_calls = 0;
    int abort_calls = 0;
    int finish_error = 0;
    size_t control_entity_count = 1;
    uint64_t entity_bytes = 32;
    uint64_t second_entity_bytes = 32;
    uint64_t next_entity_id = 1;
    size_t live_entities = 0;
    size_t peak_entities = 0;
    std::vector<int64_t> watermarks;
    std::vector<uint64_t> expired_entities;
    std::vector<std::string> order;
    std::function<void()> on_time;
};

class LifecycleProtocolModule final : public npm::INpmProtocolModuleV1 {
 public:
    LifecycleProtocolModule(ProtocolLifecycleTrace* trace, std::shared_ptr<npm::INpmTaskBudget> budget)
        : trace_(trace), budget_(std::move(budget)) {
        ++trace_->created;
    }

    int OnInput(const npm::NpmInputEventV1& event, npm::INpmResultEmitterV1&) override {
        const uint64_t session_id = event.session == nullptr ? 0 : event.session->session_id;
        trace_->order.push_back("input:" + std::to_string(session_id));
        const size_t count = session_id == 0 ? trace_->control_entity_count : 2;
        std::vector<Entity> added;
        for (size_t index = 0; index < count; ++index) {
            const uint64_t bytes = index == 0 ? trace_->entity_bytes : trace_->second_entity_bytes;
            if (budget_->Reserve(npm::NpmBudgetCategory::kModuleState, bytes) != npm::NpmBudgetError::kNone) {
                for (const auto& entity : added) budget_->Release(npm::NpmBudgetCategory::kModuleState, entity.bytes);
                return ENOSPC;
            }
            added.push_back({trace_->next_entity_id++, session_id, event.packet.meta.timestamp_ns + 10, bytes});
        }
        entities_.insert(entities_.end(), added.begin(), added.end());
        trace_->live_entities = entities_.size();
        trace_->peak_entities = std::max(trace_->peak_entities, trace_->live_entities);
        return 0;
    }

    int OnSessionSnapshot(const npm::NpmSessionView&, int64_t, npm::INpmResultEmitterV1&) override { return 0; }

    int OnSessionEnd(const npm::NpmSessionView& session, npm::NpmSessionEndReason, int64_t,
                     npm::INpmResultEmitterV1&) override {
        trace_->order.push_back("end:" + std::to_string(session.session_id));
        ReleaseIf([&](const Entity& entity) { return entity.session_id == session.session_id; });
        return 0;
    }

    std::optional<int64_t> NextEventDeadlineNs() const override {
        std::optional<int64_t> result;
        for (const auto& entity : entities_) {
            if (!result || entity.deadline_ns < *result) result = entity.deadline_ns;
        }
        return result;
    }

    int OnTime(const npm::NpmModuleTimeV1& time, npm::INpmResultEmitterV1&) override {
        assert(time.watermark_ns.has_value());
        trace_->watermarks.push_back(*time.watermark_ns);
        trace_->order.push_back("time:" + std::to_string(*time.watermark_ns));
        ReleaseIf([&](const Entity& entity) {
            if (entity.deadline_ns > *time.watermark_ns) return false;
            trace_->expired_entities.push_back(entity.id);
            return true;
        });
        if (trace_->on_time) trace_->on_time();
        return 0;
    }

    int Finish(int64_t, npm::INpmResultEmitterV1&) override {
        ++trace_->finish_calls;
        trace_->order.push_back("finish");
        if (trace_->finish_error != 0) return trace_->finish_error;
        ReleaseIf([](const Entity&) { return true; });
        return 0;
    }

    void Abort() noexcept override {
        ++trace_->abort_calls;
        trace_->order.push_back("abort");
        ReleaseIf([](const Entity&) { return true; });
    }

 private:
    struct Entity {
        uint64_t id = 0;
        uint64_t session_id = 0;
        int64_t deadline_ns = 0;
        uint64_t bytes = 0;
    };

    template <typename Predicate>
    void ReleaseIf(Predicate predicate) noexcept {
        auto entity = entities_.begin();
        while (entity != entities_.end()) {
            if (!predicate(*entity)) {
                ++entity;
                continue;
            }
            budget_->Release(npm::NpmBudgetCategory::kModuleState, entity->bytes);
            entity = entities_.erase(entity);
        }
        trace_->live_entities = entities_.size();
    }

    ProtocolLifecycleTrace* trace_;
    std::shared_ptr<npm::INpmTaskBudget> budget_;
    std::vector<Entity> entities_;
};

npm::NpmModuleRegistrationV1 LifecycleRegistration(ProtocolLifecycleTrace* trace) {
    npm::NpmModuleRegistrationV1 entry;
    entry.module_id = "lifecycle";
    entry.entity_ids = {"lifecycle_event"};
    entry.prepare = [trace](const npm::NpmBasicTaskConfig&, std::string_view, npm::NpmPreparedModuleV1* output) {
        output->plan.module_id = "lifecycle";
        output->plan.input_mask = npm::kNpmInputMaskV1;
        auto entity = npm::NpmBasicEntityDescriptorV1();
        entity.module_id = "lifecycle";
        entity.entity_id = "lifecycle_event";
        output->plan.entities = {std::move(entity)};
        output->create = [trace](npm::NpmProtocolContext&, std::shared_ptr<npm::INpmTaskBudget> budget) {
            npm::NpmModuleInstanceV1 result;
            result.protocol = std::make_unique<LifecycleProtocolModule>(trace, std::move(budget));
            return result;
        };
        return npm::NpmProtocolContractStatusV1{};
    };
    return entry;
}

std::unique_ptr<npm::NpmBasicTaskRuntime> CreateLifecycleRuntime(ProtocolLifecycleTrace* trace,
                                                                 SinglePoolQuerier* querier,
                                                                 npm::NpmBasicTaskConfig config,
                                                                 std::shared_ptr<npm::NpmTaskBudget> budget = {}) {
    config.features.module_ids = {"basic", "lifecycle"};
    config.features.basic_enabled = true;
    config.features.observing = npm::NpmResultEntity::kBasic;
    config.features.observing_entity = "basic";
    auto catalog = npm::ProductionNpmModuleCatalogV1();
    catalog.push_back(LifecycleRegistration(trace));
    std::shared_ptr<arrow::Schema> schema;
    std::unique_ptr<npm::NpmBasicTaskRuntime> runtime;
    const auto status = npm::NpmBasicTaskRuntime::Create(config, querier, flowsql::packet::PacketSchema(), &schema,
                                                         &runtime, std::move(budget), nullptr, catalog);
    assert(status.error == npm::NpmBasicTaskRuntimeError::kNone);
    return runtime;
}

void TestProtocolLifecycleDeadlinesSessionsAndEof() {
    ProtocolLifecycleTrace trace;
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    auto config = MakeRuntimeTaskConfig();
    config.analysis.out_of_order_tolerance_ns = 0;
    auto runtime = CreateLifecycleRuntime(&trace, &querier, config);
    const auto tcp = MakeIpv4TcpPacket("192.0.2.1", 123, "192.0.2.2", 443, {1});
    auto input = MakeEncodedPacketBatch(
        {MakeBatchPacketRecord(tcp, 0, 100, 1), MakeBatchPacketRecord(MakeControlPacket(), 0, 105, 2)});
    std::shared_ptr<arrow::RecordBatch> output;
    assert(runtime->ProcessOfflineBatch(input, &output).error == npm::NpmBasicOfflineBatchError::kNone);
    assert(trace.peak_entities == 3 && trace.live_entities == 3);
    assert(runtime->Budget()->Usage().module_state_bytes == 96);
    const auto plan = runtime->MaintenancePlan();
    assert(plan.event_deadline_ns == 110 && !plan.snapshot_deadline_monotonic_ns);
    assert(trace.watermarks == std::vector<int64_t>({100, 105}));

    input = MakeEncodedPacketBatch({MakeBatchPacketRecord(MakeControlPacket(), 0, 110, 3)});
    assert(runtime->ProcessOfflineBatch(input, &output).error == npm::NpmBasicOfflineBatchError::kNone);
    assert(trace.expired_entities == std::vector<uint64_t>({1, 2}));
    assert(trace.live_entities == 2 && runtime->Sessions().size() == 1);
    assert(runtime->Budget()->Usage().module_state_bytes == 64);
    assert(runtime->MaintenancePlan().event_deadline_ns == 115);

    const auto rst = MakeIpv4TcpPacket("192.0.2.1", 123, "192.0.2.2", 443, {2}, kTcpRst);
    input = MakeEncodedPacketBatch({MakeBatchPacketRecord(rst, 0, 112, 4)});
    assert(runtime->ProcessOfflineBatch(input, &output).error == npm::NpmBasicOfflineBatchError::kNone);
    assert(trace.live_entities == 2 && runtime->Sessions().size() == 0);
    const auto terminal_input = std::find(trace.order.begin(), trace.order.end(), "input:1");
    const auto terminal_end = std::find(trace.order.begin(), trace.order.end(), "end:1");
    assert(terminal_input != trace.order.end() && terminal_end != trace.order.end() && terminal_input < terminal_end);
    const auto flush = runtime->FlushOffline(120, &output);
    assert(flush.error == npm::NpmEofFlushError::kNone);
    assert(trace.finish_calls == 1 && trace.abort_calls == 0 && trace.live_entities == 0);
    assert(runtime->Budget()->Usage().module_state_bytes == 0);
    assert(runtime->FlushOffline(121, &output).error == npm::NpmEofFlushError::kAlreadyFlushed);
    runtime->Cancel();
    assert(trace.finish_calls == 1 && trace.abort_calls == 0);
}

void TestProtocolLifecycleEofOrderAndObservingIndependence() {
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    std::vector<std::vector<int64_t>> trajectories;
    for (const bool observe_lifecycle : {false, true}) {
        ProtocolLifecycleTrace trace;
        auto config = MakeRuntimeTaskConfig();
        config.analysis.out_of_order_tolerance_ns = 0;
        config.features.module_ids = {"basic", "lifecycle"};
        if (observe_lifecycle) {
            config.features.observing = npm::NpmResultEntity::kProtocol;
            config.features.observing_entity = "lifecycle_event";
        }
        auto catalog = npm::ProductionNpmModuleCatalogV1();
        catalog.push_back(LifecycleRegistration(&trace));
        std::shared_ptr<arrow::Schema> schema;
        std::unique_ptr<npm::NpmBasicTaskRuntime> runtime;
        assert(npm::NpmBasicTaskRuntime::Create(config, &querier, flowsql::packet::PacketSchema(), &schema, &runtime,
                                                {}, nullptr, catalog)
                   .error == npm::NpmBasicTaskRuntimeError::kNone);
        const auto tcp = MakeIpv4TcpPacket("192.0.2.1", 123, "192.0.2.2", 443, {1});
        auto input = MakeEncodedPacketBatch(
            {MakeBatchPacketRecord(tcp, 0, 100, 1), MakeBatchPacketRecord(MakeControlPacket(), 0, 105, 2)});
        std::shared_ptr<arrow::RecordBatch> output;
        assert(runtime->ProcessOfflineBatch(input, &output).error == npm::NpmBasicOfflineBatchError::kNone);
        assert(runtime->FlushOffline(106, &output).error == npm::NpmEofFlushError::kNone);
        const auto session_end = std::find(trace.order.begin(), trace.order.end(), "end:1");
        const auto finish = std::find(trace.order.begin(), trace.order.end(), "finish");
        assert(session_end != trace.order.end() && finish != trace.order.end() && session_end < finish);
        assert(trace.finish_calls == 1 && trace.abort_calls == 0 && trace.live_entities == 0);
        trajectories.push_back(trace.watermarks);
    }
    assert(trajectories[0] == trajectories[1]);
    assert(trajectories[0] == std::vector<int64_t>({100, 105}));
}

void TestProtocolLifecycleTupleReuseAndBudgetFailureCleanup() {
    ProtocolLifecycleTrace trace;
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    auto config = MakeRuntimeTaskConfig();
    config.analysis.out_of_order_tolerance_ns = 0;
    auto runtime = CreateLifecycleRuntime(&trace, &querier, config);
    const auto first = MakeIpv4TcpPacket("192.0.2.1", 123, "192.0.2.2", 443, {1}, kTcpSyn, 1);
    const auto reuse = MakeIpv4TcpPacket("192.0.2.1", 123, "192.0.2.2", 443, {2}, kTcpSyn, 2);
    auto input =
        MakeEncodedPacketBatch({MakeBatchPacketRecord(first, 0, 100, 1), MakeBatchPacketRecord(reuse, 0, 101, 2)});
    std::shared_ptr<arrow::RecordBatch> output;
    assert(runtime->ProcessOfflineBatch(input, &output).error == npm::NpmBasicOfflineBatchError::kNone);
    assert(trace.peak_entities == 2 && trace.live_entities == 2);
    assert(trace.order[1] == "input:1" && trace.order[3] == "end:1" && trace.order[4] == "input:2");
    runtime->Cancel();
    runtime->Cancel();
    assert(trace.finish_calls == 0 && trace.abort_calls == 1 && trace.live_entities == 0);
    assert(runtime->Budget()->Usage().module_state_bytes == 0);

    ProtocolLifecycleTrace constrained;
    constrained.control_entity_count = 2;
    constrained.entity_bytes = 32;
    constrained.second_entity_bytes = npm::kNpmMinTrackedBytes;
    auto tight_config = config;
    tight_config.analysis.max_tracked_bytes = npm::kNpmMinTrackedBytes;
    auto budget = std::make_shared<npm::NpmTaskBudget>(tight_config.analysis);
    auto failed = CreateLifecycleRuntime(&constrained, &querier, tight_config, budget);
    input = MakeEncodedPacketBatch({MakeBatchPacketRecord(MakeControlPacket(), 0, 200, 3)});
    assert(failed->ProcessOfflineBatch(input, &output).error == npm::NpmBasicOfflineBatchError::kBatchProcessError);
    assert(failed->State() == npm::NpmEofFlushState::kFailed);
    assert(constrained.finish_calls == 0 && constrained.abort_calls == 1 && constrained.live_entities == 0);
    assert(budget->Usage().module_state_bytes == 0);
}

void TestProtocolLifecycleFinishFailureAbortsAndDoesNotReplay() {
    ProtocolLifecycleTrace trace;
    trace.finish_error = EIO;
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    auto config = MakeRuntimeTaskConfig();
    config.analysis.out_of_order_tolerance_ns = 0;
    auto runtime = CreateLifecycleRuntime(&trace, &querier, config);
    auto input = MakeEncodedPacketBatch({MakeBatchPacketRecord(MakeControlPacket(), 0, 100, 1)});
    std::shared_ptr<arrow::RecordBatch> output;
    assert(runtime->ProcessOfflineBatch(input, &output).error == npm::NpmBasicOfflineBatchError::kNone);
    assert(trace.live_entities == 1 && runtime->Budget()->Usage().module_state_bytes == 32);
    assert(runtime->FlushOffline(101, &output).error == npm::NpmEofFlushError::kModuleError);
    assert(runtime->State() == npm::NpmEofFlushState::kFailed);
    assert(trace.finish_calls == 1 && trace.abort_calls == 1 && trace.live_entities == 0);
    assert(runtime->Budget()->Usage().module_state_bytes == 0);
    assert(runtime->FlushOffline(102, &output).error == npm::NpmEofFlushError::kFailedState);
    runtime->Cancel();
    assert(trace.finish_calls == 1 && trace.abort_calls == 1);
}

void TestProtocolLifecycleRealtimeDeferralAndCallbackCancel() {
    ProtocolLifecycleTrace trace;
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    auto config = MakeRuntimeTaskConfig(npm::NpmRunMode::kRealtime);
    config.analysis.out_of_order_tolerance_ns = 0;
    auto catalog = npm::ProductionNpmModuleCatalogV1();
    catalog.push_back(LifecycleRegistration(&trace));
    config.features.module_ids = {"basic", "lifecycle"};
    std::shared_ptr<arrow::Schema> schema;
    std::unique_ptr<npm::NpmBasicTaskRuntime> runtime;
    npm::NpmTimeCapabilities capabilities{true, true, true, true};
    assert(npm::NpmBasicTaskRuntime::CreateWithTimeCapabilities(config, &querier, flowsql::packet::PacketSchema(),
                                                                capabilities, &schema, &runtime, {}, nullptr, catalog)
               .error == npm::NpmBasicTaskRuntimeError::kNone);
    std::shared_ptr<arrow::RecordBatch> output;
    auto maintenance = RealtimeMaintenanceInput(10, 1000, 100, false, false, false, false);
    assert(runtime->DriveRealtimeMaintenance(maintenance, &output).error ==
           npm::NpmBasicRealtimeMaintenanceError::kNone);
    assert(trace.watermarks.empty());
    maintenance = RealtimeMaintenanceInput(20, 1001, 200, false, false, true, true);
    assert(runtime->DriveRealtimeMaintenance(maintenance, &output).error ==
           npm::NpmBasicRealtimeMaintenanceError::kNone);
    assert(trace.watermarks.empty());
    maintenance = RealtimeMaintenanceInput(30, 1002, 300, false, true, true, false);
    assert(runtime->DriveRealtimeMaintenance(maintenance, &output).error ==
           npm::NpmBasicRealtimeMaintenanceError::kNone);
    assert(trace.watermarks == std::vector<int64_t>({300}));
    maintenance = RealtimeMaintenanceInput(40, 1003, 250, false, true, true, false);
    assert(runtime->DriveRealtimeMaintenance(maintenance, &output).error ==
           npm::NpmBasicRealtimeMaintenanceError::kNone);
    assert(trace.watermarks == std::vector<int64_t>({300}));
    const auto plan = runtime->MaintenancePlan();
    assert(!plan.event_deadline_ns && plan.snapshot_deadline_monotonic_ns.has_value());

    trace.on_time = [&]() { runtime->Cancel(); };
    maintenance = RealtimeMaintenanceInput(50, 1004, 400, false, true, true, false);
    const auto cancelled = runtime->DriveRealtimeMaintenance(maintenance, &output);
    assert(cancelled.error == npm::NpmBasicRealtimeMaintenanceError::kCancelled);
    assert(runtime->State() == npm::NpmEofFlushState::kCancelled);
    assert(trace.finish_calls == 0 && trace.abort_calls == 1);
    runtime->Cancel();
    assert(trace.abort_calls == 1);
}

class UnexpectedManagedFactory final : public npm::INpmResultConsumerFactoryV1 {
 public:
    int Create(const npm::NpmResultContextV1&, const std::vector<npm::NpmEntityDescriptorV1>&,
               std::shared_ptr<npm::INpmTaskBudget>, std::unique_ptr<npm::INpmManagedResultConsumerV1>*) override {
        ++create_calls;
        return EFAULT;
    }
    std::string LastError() const override { return "managed factory must not run before module validation"; }

    int create_calls = 0;
};

void TestProtocolLabelIsolationAndAtomicFactoryFailure() {
    ProtocolInputTrace selected, all, control;
    auto catalog = npm::ProductionNpmModuleCatalogV1();
    catalog.push_back(CountingRegistration("selected", 1, &selected, false, std::vector<uint32_t>{1001}));
    catalog.push_back(CountingRegistration("all", 3, &all));
    catalog.push_back(CountingRegistration("control", 4, &control));
    npm::NpmBasicTaskConfig config;
    assert(
        npm::ParseNpmBasicTaskConfig(
            R"({"input_namespace":"capture","source_domains":"1:77","features":"basic,labeling,selected,all,control","out_of_order_tolerance_ns":"0","tcp_idle_timeout_ns":"1000000000"})",
            &config, true, catalog)
            .error == npm::NpmBasicTaskConfigError::kNone);
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    LabelingMatcherStats labels;
    std::shared_ptr<arrow::Schema> schema;
    std::unique_ptr<npm::NpmBasicTaskRuntime> runtime;
    assert(npm::NpmBasicTaskRuntime::Create(config, &querier, flowsql::packet::PacketSchema(), &schema, &runtime, {},
                                            new ProtocolLabelMatcher(&labels), catalog)
               .error == npm::NpmBasicTaskRuntimeError::kNone);
    const auto tcp = MakeIpv4TcpPacket("192.0.2.1", 123, "192.0.2.2", 443, {1});
    const auto other = MakeIpv4TcpPacket("192.0.2.1", 456, "192.0.2.2", 80, {2});
    auto input = MakeEncodedPacketBatch({MakeBatchPacketRecord(tcp, 1, 1, 1), MakeBatchPacketRecord(other, 1, 2, 2),
                                         MakeBatchPacketRecord(MakeControlPacket(), 1, 2000000000, 3),
                                         MakeBatchPacketRecord(tcp, 1, 2000000001, 4)});
    std::shared_ptr<arrow::RecordBatch> output;
    assert(runtime->ProcessOfflineBatch(input, &output).error == npm::NpmBasicOfflineBatchError::kNone);
    assert(selected.kinds.size() == 2 && all.kinds.size() == 3 && control.kinds.size() == 1);
    assert(selected.sessions[0] != selected.sessions[1]);
    assert(labels.facts.size() == 3 && labels.classify_calls == 1);
    assert(protocol.identify_pipelines.size() == 3 && output->num_rows() == 2);
    assert(selected.order == std::vector<std::string>({"packet", "end", "packet"}));
    // Control input must observe the same late-packet rule without creating or labeling a session.
    input = MakeEncodedPacketBatch({MakeBatchPacketRecord(MakeControlPacket(), 1, 0, 5)});
    assert(runtime->ProcessOfflineBatch(input, &output).error == npm::NpmBasicOfflineBatchError::kBatchProcessError);
    assert(control.kinds.size() == 1 && labels.release_calls == 1);
    runtime.reset();

    auto bad_catalog = catalog;
    bad_catalog[catalog.size() - 3] = CountingRegistration("selected", 1, &selected, false, std::vector<uint32_t>{999});
    const auto previous_schema = schema;
    assert(npm::NpmBasicTaskRuntime::Create(config, &querier, flowsql::packet::PacketSchema(), &schema, &runtime, {},
                                            new ProtocolLabelMatcher(&labels), bad_catalog)
               .error == npm::NpmBasicTaskRuntimeError::kModulePlanError);
    assert(!runtime && schema == previous_schema);
    bad_catalog[catalog.size() - 3] = CountingRegistration("selected", 1, &selected, true, std::vector<uint32_t>{1001});
    const auto stream_error =
        npm::NpmBasicTaskRuntime::Create(config, &querier, flowsql::packet::PacketSchema(), &schema, &runtime, {},
                                         new ProtocolLabelMatcher(&labels), bad_catalog);
    assert(stream_error.error == npm::NpmBasicTaskRuntimeError::kModuleCreateError);
    assert(stream_error.module_status.field == "selected/tcp_stream_consumer");
    // Fail after an earlier module was instantiated; neither runtime nor schema is published.
    auto prepare = catalog.back().prepare;
    catalog.back().prepare = [prepare](const npm::NpmBasicTaskConfig& cfg, std::string_view json,
                                       npm::NpmPreparedModuleV1* out) {
        auto status = prepare(cfg, json, out);
        out->create = [](npm::NpmProtocolContext&, std::shared_ptr<npm::INpmTaskBudget>) {
            return npm::NpmModuleInstanceV1{};
        };
        return status;
    };
    const int aborted = selected.aborted;
    UnexpectedManagedFactory managed_factory;
    assert(npm::NpmBasicTaskRuntime::Create(config, &querier, flowsql::packet::PacketSchema(), &schema, &runtime, {},
                                            new ProtocolLabelMatcher(&labels), catalog, {}, "atomic-factory-task",
                                            &managed_factory)
               .error == npm::NpmBasicTaskRuntimeError::kModuleCreateError);
    assert(!runtime && schema == previous_schema && selected.aborted == aborted + 1 &&
           managed_factory.create_calls == 0);
    assert(pool.acquire_calls == pool.release_calls);
}

struct ResultConsumerTrace {
    std::vector<npm::NpmResultProgressV1> progress;
    std::vector<size_t> rows_at_progress;
    int progress_error = 0;
    std::vector<std::string> entities;
    std::vector<npm::NpmResultContextV1> contexts;
    std::vector<uint64_t> revisions;
    std::vector<bool> finals;
    std::vector<uint64_t> dns_domains;
    std::vector<uint16_t> dns_ids;
    std::vector<std::string> dns_outcomes;
    std::vector<uint64_t> http_domains;
    std::vector<std::string> http_outcomes;
    std::vector<uint16_t> http_statuses;
    std::vector<uint64_t> tls_domains;
    std::vector<uint64_t> tls_ids;
    std::vector<std::string> tls_outcomes;
    int attempts = 0;
    int fail_at = 0;
    int finishes = 0;
    int finish_error = 0;
    std::atomic<int> cancels{0};
    std::function<void()> on_consume;
    std::function<void()> on_finish;
    std::function<void()> on_cancel;
};
class RecordingResultConsumer final : public npm::INpmResultConsumerV1, public npm::INpmResultProgressConsumerV1 {
 public:
    explicit RecordingResultConsumer(ResultConsumerTrace* trace) : trace_(trace) {}
    int PublishProgress(const npm::NpmResultContextV1&, const npm::NpmResultProgressV1& progress) override {
        if (trace_->progress_error) return trace_->progress_error;
        trace_->progress.push_back(progress);
        trace_->rows_at_progress.push_back(trace_->entities.size());
        return 0;
    }
    int Consume(const npm::NpmResultContextV1& context, const npm::NpmEntityDescriptorV1& entity,
                const arrow::RecordBatch& rows) override {
        ++trace_->attempts;
        if (trace_->on_consume) trace_->on_consume();
        if (trace_->attempts == trace_->fail_at) return EIO;
        // Copy only the values needed by this bounded test consumer, never retain borrowed Arrow owners.
        for (int64_t i = 0; i < rows.num_rows(); ++i) {
            trace_->entities.push_back(entity.entity_id);
            trace_->contexts.push_back(context);
            trace_->revisions.push_back(
                std::static_pointer_cast<arrow::UInt64Array>(rows.GetColumnByName(entity.revision_column))->Value(i));
            trace_->finals.push_back(
                std::static_pointer_cast<arrow::BooleanArray>(rows.GetColumnByName(entity.is_final_column))->Value(i));
            if (entity.entity_id == "dns_transaction") {
                trace_->dns_domains.push_back(
                    std::static_pointer_cast<arrow::UInt64Array>(rows.GetColumnByName("observation_domain_id"))
                        ->Value(i));
                trace_->dns_ids.push_back(
                    std::static_pointer_cast<arrow::UInt16Array>(rows.GetColumnByName("dns_id"))->Value(i));
                trace_->dns_outcomes.push_back(
                    std::static_pointer_cast<arrow::StringArray>(rows.GetColumnByName("outcome"))->GetString(i));
            }
            if (entity.entity_id == "http1_transaction") {
                trace_->http_domains.push_back(
                    std::static_pointer_cast<arrow::UInt64Array>(rows.GetColumnByName("observation_domain_id"))
                        ->Value(i));
                trace_->http_outcomes.push_back(
                    std::static_pointer_cast<arrow::StringArray>(rows.GetColumnByName("outcome"))->GetString(i));
                auto status = std::static_pointer_cast<arrow::UInt16Array>(rows.GetColumnByName("status_code"));
                trace_->http_statuses.push_back(status->IsNull(i) ? 0 : status->Value(i));
            }
            if (entity.entity_id == "tls_handshake") {
                trace_->tls_domains.push_back(
                    std::static_pointer_cast<arrow::UInt64Array>(rows.GetColumnByName("observation_domain_id"))
                        ->Value(i));
                trace_->tls_ids.push_back(
                    std::static_pointer_cast<arrow::UInt64Array>(rows.GetColumnByName("entity_instance_id"))->Value(i));
                trace_->tls_outcomes.push_back(
                    std::static_pointer_cast<arrow::StringArray>(rows.GetColumnByName("outcome"))->GetString(i));
            }
        }
        return 0;
    }
    int Finish() override {
        ++trace_->finishes;
        if (trace_->on_finish) trace_->on_finish();
        return trace_->finish_error;
    }
    void Cancel() noexcept override {
        ++trace_->cancels;
        if (trace_->on_cancel) trace_->on_cancel();
    }

 private:
    ResultConsumerTrace* trace_;
};

class EmittingProtocolModule final : public npm::INpmProtocolModuleV1 {
 public:
    explicit EmittingProtocolModule(ProtocolInputTrace* trace) : trace_(trace) {}
    int OnInput(const npm::NpmInputEventV1&, npm::INpmResultEmitterV1& emitter) override {
        auto result = MakeBasicEncodingResult(++next_id_);
        result.protocol_status = npm::NpmProtocolStatus::kUnknown;
        result.revision = 1;
        result.is_final = true;
        result.end_reason = npm::NpmSessionEndReason::kClosed;
        std::shared_ptr<arrow::RecordBatch> batch;
        assert(npm::EncodeNpmBasicResults({result}, &batch) == npm::NpmBasicEncodeError::kNone);
        const int error = emitter.Emit("emitting_event", *batch);
        // Borrowed storage can be immediately overwritten; foreground must already own a copy.
        auto values = batch->column(0)->data()->buffers[1];
        std::memset(values->mutable_data(), 0, values->size());
        return error;
    }
    int OnSessionSnapshot(const npm::NpmSessionView&, int64_t, npm::INpmResultEmitterV1&) override { return 0; }
    int OnSessionEnd(const npm::NpmSessionView&, npm::NpmSessionEndReason, int64_t,
                     npm::INpmResultEmitterV1&) override {
        return 0;
    }
    std::optional<int64_t> NextEventDeadlineNs() const override { return {}; }
    int OnTime(const npm::NpmModuleTimeV1&, npm::INpmResultEmitterV1&) override { return 0; }
    int Finish(int64_t, npm::INpmResultEmitterV1&) override {
        trace_->order.push_back("finish");
        return 0;
    }
    void Abort() noexcept override { ++trace_->aborted; }

 private:
    ProtocolInputTrace* trace_;
    uint64_t next_id_ = 0;
};
npm::NpmModuleCatalogV1 EmittingCatalog(ProtocolInputTrace* trace) {
    auto catalog = npm::ProductionNpmModuleCatalogV1();
    auto entry = CountingRegistration("emitting", npm::kNpmInputMaskV1, trace);
    auto prepare = entry.prepare;
    entry.prepare = [prepare, trace](const npm::NpmBasicTaskConfig& config, std::string_view json,
                                     npm::NpmPreparedModuleV1* out) {
        const auto status = prepare(config, json, out);
        out->plan.entities[0].revision_semantics = npm::NpmRevisionSemanticsV1::kEvent;
        out->create = [trace](npm::NpmProtocolContext&, std::shared_ptr<npm::INpmTaskBudget>) {
            npm::NpmModuleInstanceV1 instance;
            instance.protocol = std::make_unique<EmittingProtocolModule>(trace);
            return instance;
        };
        return status;
    };
    catalog.push_back(entry);
    return catalog;
}
std::unique_ptr<npm::NpmBasicTaskRuntime> CreateConsumingRuntime(
    npm::NpmBasicTaskConfig config, SinglePoolQuerier* querier, ResultConsumerTrace* trace,
    const npm::NpmModuleCatalogV1& catalog = npm::ProductionNpmModuleCatalogV1()) {
    std::unique_ptr<npm::NpmBasicTaskRuntime> runtime;
    std::shared_ptr<arrow::Schema> schema;
    auto consumer = trace ? std::make_unique<RecordingResultConsumer>(trace) : nullptr;
    const auto status = npm::NpmBasicTaskRuntime::CreateWithTimeCapabilities(
        config, querier, flowsql::packet::PacketSchema(), AllRealtimeTimeCapabilities(), &schema, &runtime, {}, nullptr,
        catalog, std::move(consumer), "repeatable-task");
    assert(status.error == npm::NpmBasicTaskRuntimeError::kNone);
    assert(runtime->ResultContext().task_id == "repeatable-task" && !runtime->ResultContext().run_id.empty());
    return runtime;
}
void TestUnifiedConsumerBasicSessionSnapshotsFinalsAndRunIdentity() {
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    std::string previous_run;
    struct Scenario {
        bool basic_enabled;
        bool session_enabled;
        npm::NpmResultEntity observing;
        std::vector<std::string> snapshot_entities;
        std::vector<std::string> all_entities;
    };
    const std::vector<Scenario> scenarios = {
        {true, false, npm::NpmResultEntity::kBasic, {"basic"}, {"basic", "basic"}},
        {true, true, npm::NpmResultEntity::kBasic, {"basic", "session"}, {"basic", "session", "session", "basic"}},
        {false, true, npm::NpmResultEntity::kSession, {"session"}, {"session", "session"}},
        {true, true, npm::NpmResultEntity::kSession, {"basic", "session"}, {"basic", "session", "session", "basic"}},
    };
    for (const auto& scenario : scenarios) {
        ResultConsumerTrace trace;
        auto config = MakeRuntimeTaskConfig(npm::NpmRunMode::kRealtime);
        config.features.basic_enabled = scenario.basic_enabled;
        config.features.session_enabled = scenario.session_enabled;
        config.features.observing = scenario.observing;
        config.analysis.output_interval_ns = 100;
        config.analysis.out_of_order_tolerance_ns = 0;
        auto runtime = CreateConsumingRuntime(config, &querier, &trace);
        assert(previous_run != runtime->ResultContext().run_id);
        previous_run = runtime->ResultContext().run_id;
        // These calls acquire the lifecycle mutex, proving consumer callbacks execute outside that lock.
        trace.on_consume = [&] { runtime->MaintenancePlan(); };
        trace.on_finish = [&] { runtime->MaintenancePlan(); };
        const auto packet = MakeIpv6UdpPacket("2001:db8::1", 4000, "2001:db8::2", 53, {1});
        auto packet_view = packet.View(0);
        packet_view.meta.timestamp_ns = 20;
        std::vector<npm::NpmSessionSnapshot> ended;
        assert(npm::ProcessNpmPacket(config.domains, packet_view, packet.layer, runtime->Sessions(),
                                     *runtime->ProtocolContext().Identifier(), runtime->Modules(), runtime->Collector(),
                                     &ended)
                   .error == npm::NpmPacketProcessError::kNone);
        std::shared_ptr<arrow::RecordBatch> output;
        auto drive = RealtimeMaintenanceInput(100, 1000, 20, true, false, true, false);
        assert(runtime->DriveRealtimeMaintenance(drive, &output).error == npm::NpmBasicRealtimeMaintenanceError::kNone);
        drive.monotonic_now_ns = 200;
        drive.observed_at_ns = 1100;
        drive.capture_progress.capture_time_ns = 100;
        drive.capture_progress.packet_observed = false;
        drive.capture_progress.source_idle_confirmed = true;
        assert(runtime->DriveRealtimeMaintenance(drive, &output).error == npm::NpmBasicRealtimeMaintenanceError::kNone);
        assert(output->num_rows() == 1);
        const auto observed_schema = scenario.observing == npm::NpmResultEntity::kSession
                                         ? npm::NpmSessionResultSchema()
                                         : npm::NpmBasicResultSchema();
        assert(output->schema()->Equals(*observed_schema, true));
        assert(trace.entities == scenario.snapshot_entities);
        assert(std::all_of(trace.revisions.begin(), trace.revisions.end(), [](uint64_t value) { return value == 1; }));
        assert(std::none_of(trace.finals.begin(), trace.finals.end(), [](bool value) { return value; }));
        const size_t snapshot_count = trace.entities.size();
        output.reset();
        assert(runtime->FlushOffline(1200, &output).error == npm::NpmEofFlushError::kNone);
        assert(trace.entities == scenario.all_entities);
        assert(std::all_of(trace.revisions.begin() + snapshot_count, trace.revisions.end(),
                           [](uint64_t value) { return value == 2; }));
        assert(
            std::all_of(trace.finals.begin() + snapshot_count, trace.finals.end(), [](bool value) { return value; }));
        assert(trace.finishes == 1 && trace.cancels == 0);
        assert(runtime->FlushOffline(1300, &output).error == npm::NpmEofFlushError::kAlreadyFlushed);
        runtime->Cancel();
        runtime.reset();
        assert(trace.finishes == 1 && trace.cancels == 0);
    }
}
void TestUnifiedConsumerProtocolCopyFailurePrefixAndOwnerBudget() {
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    for (int fail_at : {0, 1, 2}) {
        ProtocolInputTrace module;
        ResultConsumerTrace trace;
        trace.fail_at = fail_at;
        auto config = MakeRuntimeTaskConfig();
        config.features.basic_enabled = false;
        config.features.module_ids = {"emitting"};
        config.features.observing = npm::NpmResultEntity::kProtocol;
        config.features.observing_entity = "emitting_event";
        auto runtime = CreateConsumingRuntime(config, &querier, &trace, EmittingCatalog(&module));
        auto budget = runtime->Budget();
        auto input = MakeEncodedPacketBatch({MakeBatchPacketRecord(MakeControlPacket(), 0, 100, 1),
                                             MakeBatchPacketRecord(MakeControlPacket(true), 0, 101, 2)});
        std::weak_ptr<arrow::RecordBatch> input_owner = input;
        std::shared_ptr<arrow::RecordBatch> output;
        const auto status = runtime->ProcessOfflineBatch(input, &output);
        if (fail_at) {
            assert(status.error == npm::NpmBasicOfflineBatchError::kBatchProcessError);
            assert(!output && trace.attempts == fail_at);
            assert(trace.entities.size() == static_cast<size_t>(fail_at - 1));
            assert(trace.finishes == 0 && trace.cancels == 1 && module.aborted == 1);
            assert(runtime->LastError().find("emitting_event: consumer Consume failed: 5") != std::string::npos);
            assert(runtime->ProcessOfflineBatch(input, &output).error ==
                   npm::NpmBasicOfflineBatchError::kTerminalState);
            assert(runtime->FlushOffline(200, &output).error == npm::NpmEofFlushError::kFailedState);
            assert(trace.attempts == fail_at && npm::NpmTrackedBudgetBytes(budget->Usage()) == 0);
        } else {
            assert(status.error == npm::NpmBasicOfflineBatchError::kNone && output->num_rows() == 2);
            assert(BasicResultColumn<arrow::UInt64Array>(output, 0)->Value(0) == 1);
            assert(BasicResultColumn<arrow::UInt64Array>(output, 0)->Value(1) == 2);
            input.reset();
            assert(input_owner.expired());
            auto retained = output->column(0)->Slice(1, 1);
            output.reset();
            assert(budget->Usage().pending_output_bytes > 0);
            runtime.reset();
            assert(budget->Usage().pending_output_bytes > 0);
            assert(std::static_pointer_cast<arrow::UInt64Array>(retained)->Value(0) == 2);
            retained.reset();
            assert(npm::NpmTrackedBudgetBytes(budget->Usage()) == 0);
        }
    }
}
void TestUnifiedConsumerFinishFailureAndCancellation() {
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    for (bool during_finish : {false, true}) {
        ResultConsumerTrace trace;
        auto config = MakeRuntimeTaskConfig();
        auto runtime = CreateConsumingRuntime(config, &querier, &trace);
        auto budget = runtime->Budget();
        auto packet = MakeIpv4TcpPacket("192.0.2.1", 4000, "192.0.2.2", 443, {1});
        auto input = MakeEncodedPacketBatch({MakeBatchPacketRecord(packet, 0, 100, 1)});
        std::shared_ptr<arrow::RecordBatch> output;
        assert(runtime->ProcessOfflineBatch(input, &output).error == npm::NpmBasicOfflineBatchError::kNone);
        output.reset();
        std::mutex gate;
        std::condition_variable changed;
        bool entered = false;
        auto block = [&] {
            std::unique_lock<std::mutex> lock(gate);
            entered = true;
            changed.notify_all();
            changed.wait(lock, [&] { return trace.cancels.load() != 0; });
        };
        if (during_finish)
            trace.on_finish = block;
        else
            trace.on_consume = block;
        trace.on_cancel = [&] {
            std::lock_guard<std::mutex> lock(gate);
            changed.notify_all();
        };
        npm::NpmEofFlushStatus status;
        std::thread worker([&] { status = runtime->FlushOffline(200, &output); });
        {
            std::unique_lock<std::mutex> lock(gate);
            assert(changed.wait_for(lock, std::chrono::seconds(5), [&] { return entered; }));
        }
        runtime->Cancel();
        worker.join();
        assert(status.error == npm::NpmEofFlushError::kCancelled && !output);
        assert(trace.cancels == 1 && trace.finishes == (during_finish ? 1 : 0));
        runtime->Cancel();
        runtime.reset();
        assert(trace.cancels == 1 && npm::NpmTrackedBudgetBytes(budget->Usage()) == 0);
    }
    ResultConsumerTrace trace;
    trace.finish_error = EBUSY;
    auto runtime = CreateConsumingRuntime(MakeRuntimeTaskConfig(), &querier, &trace);
    auto packet = MakeIpv6UdpPacket("2001:db8::1", 4000, "2001:db8::2", 53, {1});
    auto input = MakeEncodedPacketBatch({MakeBatchPacketRecord(packet, 0, 100, 1)});
    std::shared_ptr<arrow::RecordBatch> output;
    assert(runtime->ProcessOfflineBatch(input, &output).error == npm::NpmBasicOfflineBatchError::kNone);
    output.reset();
    assert(runtime->FlushOffline(200, &output).error == npm::NpmEofFlushError::kDrainError);
    assert(!output && trace.entities.size() == 1 && trace.finishes == 1 && trace.cancels == 1);
    assert(runtime->LastError().find("consumer Finish failed") != std::string::npos);
    assert(runtime->FlushOffline(200, &output).error == npm::NpmEofFlushError::kFailedState);
    assert(trace.finishes == 1 && npm::NpmTrackedBudgetBytes(runtime->Budget()->Usage()) == 0);
}
void TestResultRouterValidationBudgetAndUnobservedRelease() {
    auto config = npm::DefaultNpmAnalysisConfig(npm::NpmRunMode::kOffline);
    auto budget = std::make_shared<npm::NpmTaskBudget>(config);
    auto invoke = [](const std::function<int()>& fn) { return fn(); };
    auto basic = npm::NpmBasicEntityDescriptorV1();
    auto another = basic;
    another.entity_id = "other";
    another.module_id = "other";
    std::shared_ptr<arrow::RecordBatch> rows;
    assert(npm::EncodeNpmBasicResults({MakeBasicEncodingResult(1)}, &rows) == npm::NpmBasicEncodeError::kNone);
    {
        npm::NpmResultRouter router({basic, another}, "basic", npm::MakeNpmResultContext("task"), budget, {}, invoke);
        for (int i = 0; i < 1000; ++i) assert(router.Emit("other", "other", *rows) == 0);
        assert(router.pending_results() == 0 && npm::NpmTrackedBudgetBytes(budget->Usage()) == 0);
        assert(router.Emit("other", "basic", *rows) == EINVAL);
        assert(router.LastError().find("module_id") != std::string::npos);
    }
    for (int invalid : {0, 1, 2}) {
        ResultConsumerTrace trace;
        npm::NpmResultRouter router({basic}, "basic", npm::MakeNpmResultContext("task"), budget,
                                    std::make_unique<RecordingResultConsumer>(&trace), invoke);
        if (invalid == 0) assert(router.Emit("basic", "undeclared", *rows) == EINVAL);
        if (invalid == 1) assert(router.Emit("basic", "basic", *MakeNpmPacketViewBatch()) == EINVAL);
        if (invalid == 2) {
            auto invalid_rows = rows->ReplaceSchemaMetadata(arrow::key_value_metadata({"bad"}, {"metadata"}));
            assert(router.Emit("basic", "basic", *invalid_rows) == EINVAL);
        }
        assert(trace.attempts == 0 && npm::NpmTrackedBudgetBytes(budget->Usage()) == 0);
    }
    config.max_pending_output_bytes = 1;
    budget = std::make_shared<npm::NpmTaskBudget>(config);
    ResultConsumerTrace trace;
    npm::NpmResultRouter router({basic}, "basic", npm::MakeNpmResultContext("task"), budget,
                                std::make_unique<RecordingResultConsumer>(&trace), invoke);
    assert(router.Emit("basic", "basic", *rows) == ENOMEM);
    assert(trace.attempts == 0 && npm::NpmTrackedBudgetBytes(budget->Usage()) == 0);
}

struct DualEntityProtocolTrace {
    std::atomic<int> inputs{0};
    std::atomic<int> finishes{0};
    std::atomic<int> aborts{0};
};

class DualEntityProtocolModule final : public npm::INpmProtocolModuleV1 {
 public:
    DualEntityProtocolModule(DualEntityProtocolTrace* trace, uint64_t first_id) : trace_(trace), next_id_(first_id) {}

    int OnInput(const npm::NpmInputEventV1& input, npm::INpmResultEmitterV1& emitter) override {
        assert(input.kind == npm::NpmInputKindV1::kControlPacket);
        assert(input.transport == nullptr && input.session == nullptr);
        ++trace_->inputs;
        const int left_error = EmitEvent("dual_left", emitter);
        return left_error == 0 ? EmitEvent("dual_right", emitter) : left_error;
    }
    int OnSessionSnapshot(const npm::NpmSessionView&, int64_t, npm::INpmResultEmitterV1&) override {
        assert(false);
        return EINVAL;
    }
    int OnSessionEnd(const npm::NpmSessionView&, npm::NpmSessionEndReason, int64_t,
                     npm::INpmResultEmitterV1&) override {
        assert(false);
        return EINVAL;
    }
    std::optional<int64_t> NextEventDeadlineNs() const override { return {}; }
    int OnTime(const npm::NpmModuleTimeV1&, npm::INpmResultEmitterV1&) override { return 0; }
    int Finish(int64_t, npm::INpmResultEmitterV1&) override {
        ++trace_->finishes;
        return 0;
    }
    void Abort() noexcept override { ++trace_->aborts; }

 private:
    int EmitEvent(std::string_view entity, npm::INpmResultEmitterV1& emitter) {
        auto result = MakeBasicEncodingResult(next_id_++);
        result.protocol_status = npm::NpmProtocolStatus::kUnknown;
        result.revision = 1;
        result.is_final = true;
        result.end_reason = npm::NpmSessionEndReason::kClosed;
        std::shared_ptr<arrow::RecordBatch> rows;
        if (npm::EncodeNpmBasicResults({result}, &rows) != npm::NpmBasicEncodeError::kNone) return ENOMEM;
        return emitter.Emit(entity, *rows);
    }

    DualEntityProtocolTrace* trace_ = nullptr;
    uint64_t next_id_ = 0;
};

npm::NpmModuleCatalogV1 DualEntityCatalog(DualEntityProtocolTrace* trace, uint64_t first_id) {
    auto catalog = npm::ProductionNpmModuleCatalogV1();
    npm::NpmModuleRegistrationV1 registration;
    registration.module_id = "dual";
    registration.entity_ids = {"dual_left", "dual_right"};
    registration.prepare = [trace, first_id](const npm::NpmBasicTaskConfig&, std::string_view,
                                             npm::NpmPreparedModuleV1* out) {
        out->plan.module_id = "dual";
        out->plan.input_mask = static_cast<uint8_t>(npm::NpmInputKindV1::kControlPacket);
        for (const char* entity_id : {"dual_left", "dual_right"}) {
            auto entity = npm::NpmBasicEntityDescriptorV1();
            entity.entity_id = entity_id;
            entity.module_id = "dual";
            entity.revision_semantics = npm::NpmRevisionSemanticsV1::kEvent;
            out->plan.entities.push_back(std::move(entity));
        }
        out->create = [trace, first_id](npm::NpmProtocolContext&, std::shared_ptr<npm::INpmTaskBudget>) {
            npm::NpmModuleInstanceV1 result;
            result.protocol = std::make_unique<DualEntityProtocolModule>(trace, first_id);
            return result;
        };
        return npm::NpmProtocolContractStatusV1{};
    };
    catalog.push_back(std::move(registration));
    return catalog;
}

void TestProtocolProductionCatalogAndConcurrentDualEntityControlRuntime() {
    const auto& production = npm::ProductionNpmModuleCatalogV1();
    assert(production.size() == 6);
    assert(production[0].module_id == "basic" && production[0].entity_ids == std::vector<std::string>{"basic"});
    assert(production[1].module_id == "session" && production[1].entity_ids == std::vector<std::string>{"session"});
    assert(production[2].module_id == "dns" && production[2].entity_ids == std::vector<std::string>{"dns_transaction"});
    assert(production[3].module_id == "http1" &&
           production[3].entity_ids == std::vector<std::string>{"http1_transaction"});
    assert(production[4].module_id == "tls" && production[4].entity_ids == std::vector<std::string>{"tls_handshake"});
    assert(production[5].module_id == "icmp" && production[5].entity_ids == std::vector<std::string>{"icmp_event"});

    ContextDictionary first_dictionary, second_dictionary;
    ContextProtocol first_protocol(&first_dictionary), second_protocol(&second_dictionary);
    ContextPool first_pool(&first_protocol), second_pool(&second_protocol);
    SinglePoolQuerier first_querier(&first_pool), second_querier(&second_pool);

    for (const char* unavailable : {"unavailable_protocol"}) {
        auto config = MakeRuntimeTaskConfig();
        config.features.module_ids = {unavailable};
        config.features.basic_enabled = false;
        config.features.observing = npm::NpmResultEntity::kProtocol;
        config.features.observing_entity = std::string(unavailable) + "_event";
        std::shared_ptr<arrow::Schema> schema;
        std::unique_ptr<npm::NpmBasicTaskRuntime> runtime;
        const auto status = npm::NpmBasicTaskRuntime::Create(config, &first_querier, flowsql::packet::PacketSchema(),
                                                             &schema, &runtime);
        assert(status.error == npm::NpmBasicTaskRuntimeError::kModulePlanError);
        assert(status.module_status.field == unavailable && !runtime && !schema);
    }

    auto config = MakeRuntimeTaskConfig();
    config.features.module_ids = {"dual"};
    config.features.basic_enabled = false;
    config.features.observing = npm::NpmResultEntity::kProtocol;
    config.features.observing_entity = "dual_left";
    DualEntityProtocolTrace first_module, second_module;
    ResultConsumerTrace first_consumer, second_consumer;
    auto first_runtime =
        CreateConsumingRuntime(config, &first_querier, &first_consumer, DualEntityCatalog(&first_module, 100));
    auto second_runtime =
        CreateConsumingRuntime(config, &second_querier, &second_consumer, DualEntityCatalog(&second_module, 1000));
    assert(first_runtime->ResultContext().run_id != second_runtime->ResultContext().run_id);
    auto first_budget = first_runtime->Budget();
    auto second_budget = second_runtime->Budget();
    auto input = MakeEncodedPacketBatch({MakeBatchPacketRecord(MakeControlPacket(), 0, 100, 1),
                                         MakeBatchPacketRecord(MakeControlPacket(true), 0, 101, 2)});
    std::shared_ptr<arrow::RecordBatch> first_output, second_output;
    npm::NpmBasicOfflineBatchStatus first_status, second_status;
    std::thread first_worker([&] { first_status = first_runtime->ProcessOfflineBatch(input, &first_output); });
    std::thread second_worker([&] { second_status = second_runtime->ProcessOfflineBatch(input, &second_output); });
    first_worker.join();
    second_worker.join();

    assert(first_status.error == npm::NpmBasicOfflineBatchError::kNone);
    assert(second_status.error == npm::NpmBasicOfflineBatchError::kNone);
    assert(first_module.inputs == 2 && second_module.inputs == 2);
    assert(first_output->num_rows() == 2 && second_output->num_rows() == 2);
    assert(BasicResultColumn<arrow::UInt64Array>(first_output, 0)->Value(0) == 100);
    assert(BasicResultColumn<arrow::UInt64Array>(first_output, 0)->Value(1) == 102);
    assert(BasicResultColumn<arrow::UInt64Array>(second_output, 0)->Value(0) == 1000);
    assert(BasicResultColumn<arrow::UInt64Array>(second_output, 0)->Value(1) == 1002);
    const std::vector<std::string> expected_entities{"dual_left", "dual_right", "dual_left", "dual_right"};
    assert(first_consumer.entities == expected_entities && second_consumer.entities == expected_entities);
    for (const auto& context : first_consumer.contexts) assert(context.run_id == first_runtime->ResultContext().run_id);
    for (const auto& context : second_consumer.contexts)
        assert(context.run_id == second_runtime->ResultContext().run_id);

    npm::NpmEofFlushStatus first_flush, second_flush;
    std::shared_ptr<arrow::RecordBatch> first_eof, second_eof;
    std::thread first_finish([&] { first_flush = first_runtime->FlushOffline(200, &first_eof); });
    std::thread second_finish([&] { second_flush = second_runtime->FlushOffline(200, &second_eof); });
    first_finish.join();
    second_finish.join();
    assert(first_flush.error == npm::NpmEofFlushError::kNone);
    assert(second_flush.error == npm::NpmEofFlushError::kNone);
    assert(first_module.finishes == 1 && first_module.aborts == 0);
    assert(second_module.finishes == 1 && second_module.aborts == 0);
    assert(first_consumer.finishes == 1 && first_consumer.cancels == 0);
    assert(second_consumer.finishes == 1 && second_consumer.cancels == 0);
    assert(first_eof->num_rows() == 0 && second_eof->num_rows() == 0);

    auto retained = first_output->column(0)->Slice(1, 1);
    first_output.reset();
    first_eof.reset();
    first_runtime.reset();
    assert(first_budget->Usage().pending_output_bytes > 0);
    assert(std::static_pointer_cast<arrow::UInt64Array>(retained)->Value(0) == 102);
    assert(second_runtime->State() == npm::NpmEofFlushState::kFlushed);
    second_output.reset();
    second_eof.reset();
    second_runtime.reset();
    assert(npm::NpmTrackedBudgetBytes(second_budget->Usage()) == 0);
    retained.reset();
    assert(npm::NpmTrackedBudgetBytes(first_budget->Usage()) == 0);
}

}  // namespace

struct StreamRuntimeTrace {
    std::string name;
    std::vector<std::string> calls;
    std::vector<std::tuple<uint64_t, uint64_t, npm::NpmPacketDirection, std::string>> data;
    int stream_ends = 0;
    int stream_gaps = 0;
    int module_ends = 0;
    int finishes = 0;
    int aborts = 0;
    bool hold_stream = false;
    npm::NpmBasicTaskRuntime* cancel_during_stream = nullptr;
};

class StreamRuntimeModule final : public npm::INpmProtocolModuleV1, public npm::INpmTcpStreamConsumerV1 {
 public:
    explicit StreamRuntimeModule(StreamRuntimeTrace* trace) : trace_(trace) {}
    int OnInput(const npm::NpmInputEventV1& event, npm::INpmResultEmitterV1&) override {
        assert(event.kind == npm::NpmInputKindV1::kTcpPacket);
        trace_->calls.push_back("packet");
        return 0;
    }
    int OnSessionSnapshot(const npm::NpmSessionView&, int64_t, npm::INpmResultEmitterV1&) override { return 0; }
    int OnSessionEnd(const npm::NpmSessionView&, npm::NpmSessionEndReason, int64_t,
                     npm::INpmResultEmitterV1&) override {
        trace_->calls.push_back("module_end");
        ++trace_->module_ends;
        return 0;
    }
    std::optional<int64_t> NextEventDeadlineNs() const override { return {}; }
    int OnTime(const npm::NpmModuleTimeV1&, npm::INpmResultEmitterV1&) override {
        trace_->calls.push_back("time");
        return 0;
    }
    int Finish(int64_t, npm::INpmResultEmitterV1&) override {
        trace_->calls.push_back("finish");
        ++trace_->finishes;
        return 0;
    }
    void Abort() noexcept override { ++trace_->aborts; }
    int OnTcpStreamReadable(const npm::NpmTcpStreamContextV1& context, npm::INpmTcpStreamCursorV1& cursor,
                            npm::INpmResultEmitterV1&) override {
        assert(context.session && context.session->key);
        if (trace_->cancel_during_stream) {
            trace_->calls.push_back("cancel_stream");
            trace_->cancel_during_stream->Cancel();
            return 0;
        }
        if (trace_->hold_stream) return 0;
        npm::NpmTcpStreamEventV1 event;
        while (cursor.Peek(&event)) {
            if (event.kind == npm::NpmTcpStreamEventKindV1::kData) {
                trace_->calls.push_back("data");
                trace_->data.emplace_back(
                    context.session->key->observation_domain_id, context.session->session_id, context.direction,
                    std::string(reinterpret_cast<const char*>(event.bytes.data), event.bytes.size));
                assert(cursor.Consume(event.bytes.size) == 0);
            } else if (event.kind == npm::NpmTcpStreamEventKindV1::kGap) {
                trace_->calls.push_back("gap");
                ++trace_->stream_gaps;
                assert(cursor.Consume(0) == 0);
            } else {
                assert(context.final_drain);
                trace_->calls.push_back("stream_end");
                ++trace_->stream_ends;
                assert(cursor.Consume(0) == 0);
            }
        }
        return 0;
    }

 private:
    StreamRuntimeTrace* trace_;
};

npm::NpmModuleRegistrationV1 StreamRuntimeRegistration(const char* name, StreamRuntimeTrace* trace) {
    npm::NpmModuleRegistrationV1 registration;
    registration.module_id = name;
    registration.entity_ids = {std::string(name) + "_event"};
    registration.prepare = [name = std::string(name), trace](const npm::NpmBasicTaskConfig&, std::string_view,
                                                             npm::NpmPreparedModuleV1* output) {
        output->plan.module_id = name;
        output->plan.input_mask = static_cast<uint8_t>(npm::NpmInputKindV1::kTcpPacket);
        output->plan.requires_labeling = true;
        output->plan.requires_tcp_stream = true;
        output->plan.primary_label_ids = std::vector<uint32_t>{1001};
        auto entity = npm::NpmBasicEntityDescriptorV1();
        entity.entity_id = name + "_event";
        entity.module_id = name;
        output->plan.entities = {entity};
        output->create = [trace](npm::NpmProtocolContext&, std::shared_ptr<npm::INpmTaskBudget>) {
            npm::NpmModuleInstanceV1 instance;
            auto module = std::make_unique<StreamRuntimeModule>(trace);
            instance.tcp_stream_consumer = module.get();
            instance.protocol = std::move(module);
            return instance;
        };
        return npm::NpmProtocolContractStatusV1{};
    };
    return registration;
}

void TestTcpStreamRuntimeAdmissionAndLifecycle() {
    {
        auto budget = std::make_shared<npm::NpmTaskBudget>(npm::NpmAnalysisConfig{});
        std::unique_ptr<npm::NpmTcpStreamProvider> empty;
        assert(npm::NpmTcpStreamProvider::Create({}, budget, {}, &empty) == npm::NpmTcpStreamError::kNone);
        assert(!empty && budget->Usage().module_state_bytes == 0);
    }
    for (const char* features : {"basic,labeling,alpha,beta", "beta,alpha,labeling,basic"}) {
        StreamRuntimeTrace alpha{"alpha"}, beta{"beta"};
        auto catalog = npm::ProductionNpmModuleCatalogV1();
        catalog.push_back(StreamRuntimeRegistration("alpha", &alpha));
        catalog.push_back(StreamRuntimeRegistration("beta", &beta));
        npm::NpmBasicTaskConfig config;
        const std::string json =
            std::string(R"({"input_namespace":"capture","source_domains":"1:77;2:78","features":")") + features + "\"}";
        assert(npm::ParseNpmBasicTaskConfig(json.c_str(), &config, true, catalog).error ==
               npm::NpmBasicTaskConfigError::kNone);
        config.analysis.result_mode = npm::NpmResultMode::kFinal;
        assert(!config.features.session_enabled);
        ContextDictionary dictionary;
        ContextProtocol protocol(&dictionary);
        ContextPool pool(&protocol);
        SinglePoolQuerier querier(&pool);
        LabelingMatcherStats matcher_stats;
        auto budget = std::make_shared<npm::NpmTaskBudget>(config.analysis);
        std::shared_ptr<arrow::Schema> schema;
        std::unique_ptr<npm::NpmBasicTaskRuntime> runtime;
        assert(npm::NpmBasicTaskRuntime::Create(config, &querier, flowsql::packet::PacketSchema(), &schema, &runtime,
                                                budget, new ProtocolLabelMatcher(&matcher_stats), catalog)
                   .error == npm::NpmBasicTaskRuntimeError::kNone);
        const auto baseline_module_bytes = budget->Usage().module_state_bytes;
        auto a = MakeIpv4TcpPacket("192.0.2.1", 50000, "198.51.100.1", 443, {}, kTcpSyn, 100);
        auto b = MakeIpv4TcpPacket("192.0.2.1", 50000, "198.51.100.1", 443, {'A', 'B', 'C'}, kTcpAck, 101);
        auto unmatched = MakeIpv4TcpPacket("192.0.2.1", 50001, "198.51.100.1", 80, {'n'}, kTcpAck, 10);
        auto other_domain = MakeIpv4TcpPacket("192.0.2.1", 50000, "198.51.100.1", 443, {'Q'}, kTcpAck, 501);
        auto reverse = MakeIpv4TcpPacket("198.51.100.1", 443, "192.0.2.1", 50000, {'Z'}, kTcpAck, 800);
        auto rst = MakeIpv4TcpPacket("192.0.2.1", 50000, "198.51.100.1", 443, {}, kTcpRst, 104);
        auto replacement = MakeIpv4TcpPacket("192.0.2.1", 50000, "198.51.100.1", 443, {}, kTcpSyn, 200);
        auto new_data = MakeIpv4TcpPacket("192.0.2.1", 50000, "198.51.100.1", 443, {'X', 'Y'}, kTcpAck, 201);
        auto half_close = MakeIpv4TcpPacket("192.0.2.1", 50000, "198.51.100.1", 443, {}, kTcpFin, 203);
        std::shared_ptr<arrow::RecordBatch> output;
        auto input = MakeEncodedPacketBatch({MakeBatchPacketRecord(unmatched, 1, 9, 1)});
        assert(runtime->ProcessOfflineBatch(input, &output).error == npm::NpmBasicOfflineBatchError::kNone);
        assert(budget->Usage().module_state_bytes == baseline_module_bytes);
        assert(std::find(alpha.calls.begin(), alpha.calls.end(), "packet") == alpha.calls.end());
        assert(std::find(alpha.calls.begin(), alpha.calls.end(), "data") == alpha.calls.end());
        input = MakeEncodedPacketBatch({MakeBatchPacketRecord(a, 1, 10, 1), MakeBatchPacketRecord(b, 1, 11, 2),
                                        MakeBatchPacketRecord(other_domain, 2, 13, 4),
                                        MakeBatchPacketRecord(reverse, 1, 14, 5), MakeBatchPacketRecord(rst, 1, 15, 6),
                                        MakeBatchPacketRecord(replacement, 1, 16, 7),
                                        MakeBatchPacketRecord(new_data, 1, 17, 8)});
        assert(runtime->ProcessOfflineBatch(input, &output).error == npm::NpmBasicOfflineBatchError::kNone);
        assert(alpha.data == beta.data && alpha.data.size() == 4);
        assert(std::get<0>(alpha.data[0]) == 77 && std::get<3>(alpha.data[0]) == "ABC");
        assert(std::get<0>(alpha.data[1]) == 78 && std::get<3>(alpha.data[1]) == "Q");
        assert(std::get<0>(alpha.data[2]) == 77 && std::get<3>(alpha.data[2]) == "Z");
        assert(std::get<3>(alpha.data[3]) == "XY");
        assert(std::get<1>(alpha.data[0]) != std::get<1>(alpha.data[1]));
        assert(std::get<1>(alpha.data[0]) != std::get<1>(alpha.data[3]));
        assert(alpha.stream_ends == 2 && beta.stream_ends == 2);
        assert(alpha.module_ends == 1 && beta.module_ends == 1);
        const auto first_end = std::find(alpha.calls.begin(), alpha.calls.end(), "stream_end");
        assert(first_end != alpha.calls.end());
        assert(std::find(first_end, alpha.calls.end(), "module_end") != alpha.calls.end());
        const auto before_fin = alpha.calls.size();
        input = MakeEncodedPacketBatch({MakeBatchPacketRecord(half_close, 1, 18, 9)});
        assert(runtime->ProcessOfflineBatch(input, &output).error == npm::NpmBasicOfflineBatchError::kNone);
        assert(alpha.stream_ends == 3 && beta.stream_ends == 3);
        assert(alpha.module_ends == 1 && beta.module_ends == 1);
        const auto fin_packet = std::find(alpha.calls.begin() + before_fin, alpha.calls.end(), "packet");
        assert(fin_packet != alpha.calls.end() &&
               std::find(fin_packet, alpha.calls.end(), "stream_end") != alpha.calls.end());
        assert(std::find(alpha.calls.begin() + before_fin, alpha.calls.end(), "module_end") == alpha.calls.end());
        assert(runtime->FlushOffline(19, &output).error == npm::NpmEofFlushError::kNone);
        assert(alpha.stream_ends == 4 && beta.stream_ends == 4);
        assert(alpha.module_ends == 3 && beta.module_ends == 3);
        assert(alpha.finishes == 1 && beta.finishes == 1);
        assert(alpha.calls.back() == "finish" && beta.calls.back() == "finish");
        assert(alpha.aborts == 0 && beta.aborts == 0);
        assert(budget->Usage().module_state_bytes == 0);
        assert(matcher_stats.classify_calls >= 1 && matcher_stats.facts.size() == 4 &&
               matcher_stats.release_calls == 1);
    }
}

void TestTcpStreamRuntimeDeadlineAndCancel() {
    StreamRuntimeTrace trace{"alpha"};
    auto catalog = npm::ProductionNpmModuleCatalogV1();
    catalog.push_back(StreamRuntimeRegistration("alpha", &trace));
    npm::NpmBasicTaskConfig config;
    assert(
        npm::ParseNpmBasicTaskConfig(
            R"({"input_namespace":"capture","source_domains":"1:77","features":"basic,labeling,alpha","parameters":"{\"schema_version\":1,\"core\":{\"tcp_stream\":{\"gap_timeout_ns\":5}}}"})",
            &config, true, catalog)
            .error == npm::NpmBasicTaskConfigError::kNone);
    config.analysis.out_of_order_tolerance_ns = 0;
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    LabelingMatcherStats matcher_stats;
    auto budget = std::make_shared<npm::NpmTaskBudget>(config.analysis);
    std::shared_ptr<arrow::Schema> schema;
    std::unique_ptr<npm::NpmBasicTaskRuntime> runtime;
    assert(npm::NpmBasicTaskRuntime::Create(config, &querier, flowsql::packet::PacketSchema(), &schema, &runtime,
                                            budget, new ProtocolLabelMatcher(&matcher_stats), catalog)
               .error == npm::NpmBasicTaskRuntimeError::kNone);
    auto a = MakeIpv4TcpPacket("192.0.2.1", 50000, "198.51.100.1", 443, {'A', 'B', 'C'}, kTcpAck, 100);
    auto b = MakeIpv4TcpPacket("192.0.2.1", 50000, "198.51.100.1", 443, {'F', 'G'}, kTcpAck, 105);
    auto input = MakeEncodedPacketBatch({MakeBatchPacketRecord(a, 1, 1, 1), MakeBatchPacketRecord(b, 1, 2, 2)});
    std::shared_ptr<arrow::RecordBatch> output;
    assert(runtime->ProcessOfflineBatch(input, &output).error == npm::NpmBasicOfflineBatchError::kNone);
    assert(trace.stream_gaps == 0 && runtime->MaintenancePlan().event_deadline_ns == 7);
    auto tick = MakeIpv4TcpPacket("192.0.2.1", 50001, "198.51.100.1", 80, {}, kTcpAck, 1);
    input = MakeEncodedPacketBatch({MakeBatchPacketRecord(tick, 1, 7, 3)});
    assert(runtime->ProcessOfflineBatch(input, &output).error == npm::NpmBasicOfflineBatchError::kNone);
    assert(trace.stream_gaps == 1);
    const auto gap = std::find(trace.calls.begin(), trace.calls.end(), "gap");
    assert(gap != trace.calls.end() && std::find(gap, trace.calls.end(), "time") != trace.calls.end());
    runtime->Cancel();
    assert(trace.stream_ends == 0 && trace.finishes == 0 && trace.aborts == 1);
    assert(budget->Usage().module_state_bytes == 0);
    assert(runtime->State() == npm::NpmEofFlushState::kCancelled);
}

void TestTcpStreamRuntimeFailureDiagnostics() {
    StreamRuntimeTrace trace{"alpha"};
    trace.hold_stream = true;
    auto catalog = npm::ProductionNpmModuleCatalogV1();
    catalog.push_back(StreamRuntimeRegistration("alpha", &trace));
    npm::NpmBasicTaskConfig config;
    assert(npm::ParseNpmBasicTaskConfig(
               R"({"input_namespace":"capture","source_domains":"1:77","features":"basic,labeling,alpha"})", &config,
               true, catalog)
               .error == npm::NpmBasicTaskConfigError::kNone);
    config.tcp_stream.max_buffered_bytes_per_direction = 65536;
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    LabelingMatcherStats matcher_stats;
    auto budget = std::make_shared<npm::NpmTaskBudget>(config.analysis);
    std::shared_ptr<arrow::Schema> schema;
    std::unique_ptr<npm::NpmBasicTaskRuntime> runtime;
    assert(npm::NpmBasicTaskRuntime::Create(config, &querier, flowsql::packet::PacketSchema(), &schema, &runtime,
                                            budget, new ProtocolLabelMatcher(&matcher_stats), catalog)
               .error == npm::NpmBasicTaskRuntimeError::kNone);
    auto first =
        MakeIpv4TcpPacket("192.0.2.1", 50000, "198.51.100.1", 443, std::vector<uint8_t>(40000, 'a'), kTcpAck, 100);
    auto second =
        MakeIpv4TcpPacket("192.0.2.1", 50000, "198.51.100.1", 443, std::vector<uint8_t>(30000, 'b'), kTcpAck, 40100);
    std::shared_ptr<arrow::RecordBatch> output;
    assert(
        runtime->ProcessOfflineBatch(MakeEncodedPacketBatch({MakeBatchPacketRecord(first, 1, 1, 1)}), &output).error ==
        npm::NpmBasicOfflineBatchError::kNone);
    const auto status =
        runtime->ProcessOfflineBatch(MakeEncodedPacketBatch({MakeBatchPacketRecord(second, 1, 2, 2)}), &output);
    assert(status.error == npm::NpmBasicOfflineBatchError::kBatchProcessError);
    assert(runtime->State() == npm::NpmEofFlushState::kFailed);
    assert(runtime->LastError().find("direction limit exceeded") != std::string::npos);
    assert(runtime->LastError().find("session_id=") != std::string::npos);
    assert(runtime->LastError().find("retaining=alpha") != std::string::npos);
    assert(trace.stream_ends == 0 && trace.finishes == 0 && trace.aborts == 1);
    assert(budget->Usage().module_state_bytes == 0);
}

void TestTcpStreamRuntimeCancelInsideCallback() {
    StreamRuntimeTrace alpha{"alpha"}, beta{"beta"};
    auto catalog = npm::ProductionNpmModuleCatalogV1();
    catalog.push_back(StreamRuntimeRegistration("alpha", &alpha));
    catalog.push_back(StreamRuntimeRegistration("beta", &beta));
    npm::NpmBasicTaskConfig config;
    assert(npm::ParseNpmBasicTaskConfig(
               R"({"input_namespace":"capture","source_domains":"1:77","features":"basic,labeling,alpha,beta"})",
               &config, true, catalog)
               .error == npm::NpmBasicTaskConfigError::kNone);
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    LabelingMatcherStats matcher_stats;
    auto budget = std::make_shared<npm::NpmTaskBudget>(config.analysis);
    std::shared_ptr<arrow::Schema> schema;
    std::unique_ptr<npm::NpmBasicTaskRuntime> runtime;
    assert(npm::NpmBasicTaskRuntime::Create(config, &querier, flowsql::packet::PacketSchema(), &schema, &runtime,
                                            budget, new ProtocolLabelMatcher(&matcher_stats), catalog)
               .error == npm::NpmBasicTaskRuntimeError::kNone);
    alpha.cancel_during_stream = runtime.get();
    auto packet = MakeIpv4TcpPacket("192.0.2.1", 50000, "198.51.100.1", 443, {'A'}, kTcpAck, 100);
    std::shared_ptr<arrow::RecordBatch> output;
    const auto result =
        runtime->ProcessOfflineBatch(MakeEncodedPacketBatch({MakeBatchPacketRecord(packet, 1, 1, 1)}), &output);
    assert(result.error == npm::NpmBasicOfflineBatchError::kCancelled);
    assert(runtime->State() == npm::NpmEofFlushState::kCancelled);
    assert(std::find(alpha.calls.begin(), alpha.calls.end(), "cancel_stream") != alpha.calls.end());
    assert(std::find(beta.calls.begin(), beta.calls.end(), "cancel_stream") == beta.calls.end());
    assert(alpha.stream_ends == 0 && beta.stream_ends == 0);
    assert(alpha.aborts == 1 && beta.aborts == 1);
    assert(budget->Usage().module_state_bytes == 0);
}

using StreamEventRow = std::tuple<uint8_t, uint8_t, uint64_t, uint64_t, std::string, std::optional<int64_t>>;

std::shared_ptr<arrow::Schema> StreamEventSchema() {
    static auto schema = arrow::schema({
        arrow::field("entity_instance_id", arrow::uint64(), false),
        arrow::field("revision", arrow::uint64(), false),
        arrow::field("observed_at", arrow::int64(), false),
        arrow::field("is_final", arrow::boolean(), false),
        arrow::field("session_id", arrow::uint64(), false),
        arrow::field("direction", arrow::uint8(), false),
        arrow::field("kind", arrow::uint8(), false),
        arrow::field("begin", arrow::uint64(), false),
        arrow::field("end", arrow::uint64(), false),
        arrow::field("bytes", arrow::binary(), false),
        arrow::field("captured_at", arrow::int64()),
    });
    return schema;
}

void AppendStreamEventRows(const arrow::RecordBatch& batch, std::vector<StreamEventRow>* output) {
    assert(batch.schema()->Equals(*StreamEventSchema(), true));
    const auto& ids = static_cast<const arrow::UInt64Array&>(*batch.GetColumnByName("entity_instance_id"));
    const auto& revisions = static_cast<const arrow::UInt64Array&>(*batch.GetColumnByName("revision"));
    const auto& finals = static_cast<const arrow::BooleanArray&>(*batch.GetColumnByName("is_final"));
    const auto& sessions = static_cast<const arrow::UInt64Array&>(*batch.GetColumnByName("session_id"));
    const auto& directions = static_cast<const arrow::UInt8Array&>(*batch.GetColumnByName("direction"));
    const auto& kinds = static_cast<const arrow::UInt8Array&>(*batch.GetColumnByName("kind"));
    const auto& begins = static_cast<const arrow::UInt64Array&>(*batch.GetColumnByName("begin"));
    const auto& ends = static_cast<const arrow::UInt64Array&>(*batch.GetColumnByName("end"));
    const auto& bytes = static_cast<const arrow::BinaryArray&>(*batch.GetColumnByName("bytes"));
    const auto& captured = static_cast<const arrow::Int64Array&>(*batch.GetColumnByName("captured_at"));
    for (int64_t row = 0; row < batch.num_rows(); ++row) {
        assert(ids.Value(row) > 0 && revisions.Value(row) == 1 && finals.Value(row) && sessions.Value(row) > 0);
        output->emplace_back(directions.Value(row), kinds.Value(row), begins.Value(row), ends.Value(row),
                             bytes.GetString(row),
                             captured.IsNull(row) ? std::nullopt : std::optional<int64_t>(captured.Value(row)));
    }
}

struct StreamResultTrace {
    std::vector<StreamEventRow> rows;
    std::vector<std::string> entities;
    std::string run_id;
    int basic_rows = 0;
    int session_rows = 0;
    int finishes = 0;
    int cancels = 0;
    int module_finishes = 0;
    int module_aborts = 0;
};

class StreamResultConsumer final : public npm::INpmResultConsumerV1 {
 public:
    explicit StreamResultConsumer(StreamResultTrace* trace) : trace_(trace) {}
    int Consume(const npm::NpmResultContextV1& context, const npm::NpmEntityDescriptorV1& entity,
                const arrow::RecordBatch& rows) override {
        assert(!context.run_id.empty());
        if (trace_->run_id.empty()) trace_->run_id = context.run_id;
        assert(trace_->run_id == context.run_id);
        trace_->entities.push_back(entity.entity_id);
        if (entity.entity_id == "stream_event") AppendStreamEventRows(rows, &trace_->rows);
        if (entity.entity_id == "basic") trace_->basic_rows += rows.num_rows();
        if (entity.entity_id == "session") trace_->session_rows += rows.num_rows();
        return 0;
    }
    int Finish() override {
        ++trace_->finishes;
        return 0;
    }
    void Cancel() noexcept override { ++trace_->cancels; }

 private:
    StreamResultTrace* trace_;
};

class StreamResultModule final : public npm::INpmProtocolModuleV1, public npm::INpmTcpStreamConsumerV1 {
 public:
    explicit StreamResultModule(StreamResultTrace* trace) : trace_(trace) {}
    int OnInput(const npm::NpmInputEventV1&, npm::INpmResultEmitterV1&) override { return 0; }
    int OnSessionSnapshot(const npm::NpmSessionView&, int64_t, npm::INpmResultEmitterV1&) override { return 0; }
    int OnSessionEnd(const npm::NpmSessionView&, npm::NpmSessionEndReason, int64_t,
                     npm::INpmResultEmitterV1&) override {
        return 0;
    }
    std::optional<int64_t> NextEventDeadlineNs() const override { return {}; }
    int OnTime(const npm::NpmModuleTimeV1&, npm::INpmResultEmitterV1&) override { return 0; }
    int Finish(int64_t, npm::INpmResultEmitterV1&) override {
        ++trace_->module_finishes;
        return 0;
    }
    void Abort() noexcept override { ++trace_->module_aborts; }
    int OnTcpStreamReadable(const npm::NpmTcpStreamContextV1& context, npm::INpmTcpStreamCursorV1& cursor,
                            npm::INpmResultEmitterV1& emitter) override {
        npm::NpmTcpStreamEventV1 event;
        while (cursor.Peek(&event)) {
            assert(context.session && context.session->session_id > 0);
            std::vector<uint8_t> payload;
            if (event.kind == npm::NpmTcpStreamEventKindV1::kData)
                payload.assign(event.bytes.data, event.bytes.data + event.bytes.size);
            auto batch = arrow::RecordBatch::Make(
                StreamEventSchema(), 1,
                {MakeOneValueArray<arrow::UInt64Builder>(next_id_++),
                 MakeOneValueArray<arrow::UInt64Builder>(uint64_t{1}),
                 MakeOneValueArray<arrow::Int64Builder>(context.observed_at_ns),
                 MakeOneValueArray<arrow::BooleanBuilder>(true),
                 MakeOneValueArray<arrow::UInt64Builder>(context.session->session_id),
                 MakeOneValueArray<arrow::UInt8Builder>(static_cast<uint8_t>(context.direction)),
                 MakeOneValueArray<arrow::UInt8Builder>(static_cast<uint8_t>(event.kind)),
                 MakeOneValueArray<arrow::UInt64Builder>(event.begin),
                 MakeOneValueArray<arrow::UInt64Builder>(event.end), MakeOneBinaryArray(payload),
                 event.captured_at_ns ? MakeOneValueArray<arrow::Int64Builder>(*event.captured_at_ns)
                                      : MakeOneNullArray<arrow::Int64Builder>()});
            const int error = emitter.Emit("stream_event", *batch);
            if (error) return error;
            if (cursor.Consume(event.kind == npm::NpmTcpStreamEventKindV1::kData ? event.bytes.size : 0)) return EINVAL;
        }
        return 0;
    }

 private:
    StreamResultTrace* trace_;
    uint64_t next_id_ = 1;
};

npm::NpmModuleCatalogV1 StreamResultCatalog(StreamResultTrace* trace) {
    auto catalog = npm::ProductionNpmModuleCatalogV1();
    npm::NpmModuleRegistrationV1 registration;
    registration.module_id = "stream_probe";
    registration.entity_ids = {"stream_event"};
    registration.prepare = [trace](const npm::NpmBasicTaskConfig&, std::string_view, npm::NpmPreparedModuleV1* out) {
        out->plan.module_id = "stream_probe";
        out->plan.input_mask = static_cast<uint8_t>(npm::NpmInputKindV1::kTcpPacket);
        out->plan.requires_labeling = true;
        out->plan.requires_tcp_stream = true;
        out->plan.primary_label_ids = std::vector<uint32_t>{1001};
        npm::NpmEntityDescriptorV1 entity;
        entity.entity_id = "stream_event";
        entity.module_id = "stream_probe";
        entity.schema = StreamEventSchema();
        entity.revision_semantics = npm::NpmRevisionSemanticsV1::kEvent;
        out->plan.entities = {entity};
        out->create = [trace](npm::NpmProtocolContext&, std::shared_ptr<npm::INpmTaskBudget>) {
            npm::NpmModuleInstanceV1 instance;
            auto module = std::make_unique<StreamResultModule>(trace);
            instance.tcp_stream_consumer = module.get();
            instance.protocol = std::move(module);
            return instance;
        };
        return npm::NpmProtocolContractStatusV1{};
    };
    catalog.push_back(std::move(registration));
    return catalog;
}

struct StreamResultOutcome {
    std::vector<StreamEventRow> consumed;
    std::vector<StreamEventRow> foreground;
    int basic_rows = 0;
    int session_rows = 0;
};

StreamResultOutcome RunStreamResultBatches(bool split_batches, bool observe_stream) {
    StreamResultTrace trace;
    auto catalog = StreamResultCatalog(&trace);
    npm::NpmBasicTaskConfig config;
    const std::string json = std::string(R"({"input_namespace":"capture","source_domains":"1:77",)") +
                             R"("features":"basic,session,labeling,stream_probe","observing":")" +
                             (observe_stream ? "stream_event" : "session") + R"("})";
    assert(npm::ParseNpmBasicTaskConfig(json.c_str(), &config, true, catalog).error ==
           npm::NpmBasicTaskConfigError::kNone);
    config.analysis.out_of_order_tolerance_ns = 0;
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    LabelingMatcherStats labels;
    auto budget = std::make_shared<npm::NpmTaskBudget>(config.analysis);
    std::shared_ptr<arrow::Schema> schema;
    std::unique_ptr<npm::NpmBasicTaskRuntime> runtime;
    auto consumer = std::make_unique<StreamResultConsumer>(&trace);
    assert(npm::NpmBasicTaskRuntime::Create(config, &querier, flowsql::packet::PacketSchema(), &schema, &runtime,
                                            budget, new ProtocolLabelMatcher(&labels), catalog, std::move(consumer))
               .error == npm::NpmBasicTaskRuntimeError::kNone);
    assert(schema->Equals(*(observe_stream ? StreamEventSchema() : npm::NpmSessionResultSchema(true)), true));
    auto first = MakeIpv4TcpPacket("192.0.2.1", 50000, "198.51.100.1", 443, {'A', 'B', 'C'}, kTcpAck, 100);
    auto late = MakeIpv4TcpPacket("192.0.2.1", 50000, "198.51.100.1", 443, {'F', 'G'}, kTcpAck, 105);
    auto reset = MakeIpv4TcpPacket("192.0.2.1", 50000, "198.51.100.1", 443, {}, kTcpRst, 107);
    StreamResultOutcome outcome;
    auto process = [&](std::shared_ptr<arrow::RecordBatch> input, bool live_direction) {
        std::weak_ptr<arrow::RecordBatch> owner = input;
        std::shared_ptr<arrow::RecordBatch> output;
        assert(runtime->ProcessOfflineBatch(input, &output).error == npm::NpmBasicOfflineBatchError::kNone);
        assert(output && output->schema()->Equals(*schema, true));
        if (observe_stream) AppendStreamEventRows(*output, &outcome.foreground);
        output.reset();
        input.reset();
        assert(owner.expired());
        if (live_direction) assert(budget->Usage().module_state_bytes > 0);
    };
    if (split_batches) {
        process(MakeEncodedPacketBatch({MakeBatchPacketRecord(first, 1, 10, 1)}), true);
        process(MakeEncodedPacketBatch({MakeBatchPacketRecord(late, 1, 11, 2)}), true);
        process(MakeEncodedPacketBatch({MakeBatchPacketRecord(reset, 1, 12, 3)}), false);
    } else {
        process(MakeEncodedPacketBatch({MakeBatchPacketRecord(first, 1, 10, 1), MakeBatchPacketRecord(late, 1, 11, 2),
                                        MakeBatchPacketRecord(reset, 1, 12, 3)}),
                false);
    }
    std::shared_ptr<arrow::RecordBatch> eof;
    assert(runtime->FlushOffline(20, &eof).error == npm::NpmEofFlushError::kNone);
    assert(eof && eof->schema()->Equals(*schema, true));
    if (observe_stream) AppendStreamEventRows(*eof, &outcome.foreground);
    eof.reset();
    assert(trace.finishes == 1 && trace.cancels == 0 && trace.module_finishes == 1 && trace.module_aborts == 0);
    assert(trace.basic_rows > 0 && trace.session_rows > 0 && !trace.run_id.empty());
    assert(labels.release_calls == 1);
    outcome.consumed = trace.rows;
    outcome.basic_rows = trace.basic_rows;
    outcome.session_rows = trace.session_rows;
    runtime.reset();
    assert(labels.release_calls == 1 && npm::NpmTrackedBudgetBytes(budget->Usage()) == 0);
    return outcome;
}

void TestTcpStreamRecordBatchTypedResultRouting() {
    const auto one_batch = RunStreamResultBatches(false, true);
    const auto split = RunStreamResultBatches(true, true);
    const auto session_observed = RunStreamResultBatches(true, false);
    assert(one_batch.consumed == split.consumed && split.consumed == session_observed.consumed);
    assert(one_batch.foreground == one_batch.consumed && split.foreground == split.consumed);
    assert(session_observed.foreground.empty());
    assert(one_batch.basic_rows == split.basic_rows && split.basic_rows == session_observed.basic_rows);
    assert(one_batch.session_rows == split.session_rows && split.session_rows == session_observed.session_rows);
    const auto& events = split.consumed;
    assert(events.size() == 4);
    assert(std::get<1>(events[0]) == static_cast<uint8_t>(npm::NpmTcpStreamEventKindV1::kData));
    assert(std::get<2>(events[0]) == 0 && std::get<3>(events[0]) == 3 && std::get<4>(events[0]) == "ABC");
    assert(std::get<5>(events[0]) == 10);
    assert(std::get<1>(events[1]) == static_cast<uint8_t>(npm::NpmTcpStreamEventKindV1::kGap));
    assert(std::get<2>(events[1]) == 3 && std::get<3>(events[1]) == 5 && std::get<4>(events[1]).empty());
    assert(std::get<1>(events[2]) == static_cast<uint8_t>(npm::NpmTcpStreamEventKindV1::kData));
    assert(std::get<2>(events[2]) == 5 && std::get<3>(events[2]) == 7 && std::get<4>(events[2]) == "FG");
    assert(std::get<5>(events[2]) == 11);
    assert(std::get<1>(events[3]) == static_cast<uint8_t>(npm::NpmTcpStreamEventKindV1::kEnd));
    assert(std::get<2>(events[3]) == 7 && std::get<3>(events[3]) == 7);
    assert(std::get<0>(events[0]) == static_cast<uint8_t>(npm::NpmPacketDirection::kAToB));
}

struct TlsStreamTrace {
    std::vector<npm::NpmTlsFramedEventV1> hellos;
    int packets = 0;
    int finishes = 0;
    int aborts = 0;
};

class TlsStreamProbe final : public npm::INpmProtocolModuleV1, public npm::INpmTcpStreamConsumerV1 {
 public:
    explicit TlsStreamProbe(TlsStreamTrace* trace) : trace_(trace) {}
    int OnInput(const npm::NpmInputEventV1& input, npm::INpmResultEmitterV1&) override {
        assert(input.kind == npm::NpmInputKindV1::kTcpPacket);
        ++trace_->packets;
        return 0;
    }
    int OnSessionSnapshot(const npm::NpmSessionView&, int64_t, npm::INpmResultEmitterV1&) override { return 0; }
    int OnSessionEnd(const npm::NpmSessionView& session, npm::NpmSessionEndReason, int64_t,
                     npm::INpmResultEmitterV1&) override {
        framers_.erase(session.session_id);
        return 0;
    }
    std::optional<int64_t> NextEventDeadlineNs() const override { return {}; }
    int OnTime(const npm::NpmModuleTimeV1&, npm::INpmResultEmitterV1&) override { return 0; }
    int Finish(int64_t, npm::INpmResultEmitterV1&) override {
        assert(framers_.empty());
        ++trace_->finishes;
        return 0;
    }
    void Abort() noexcept override { ++trace_->aborts; }
    int OnTcpStreamReadable(const npm::NpmTcpStreamContextV1& context, npm::INpmTcpStreamCursorV1& cursor,
                            npm::INpmResultEmitterV1&) override {
        assert(context.session);
        auto [entry, inserted] = framers_.try_emplace(context.session->session_id, 4096);
        (void)inserted;
        npm::NpmTcpStreamEventV1 event;
        while (cursor.Peek(&event)) {
            std::vector<npm::NpmTlsFramedEventV1> parsed;
            const auto error = entry->second.Consume(context, event, &parsed);
            if (error != npm::NpmTlsFramerErrorV1::kNone) return EINVAL;
            if (cursor.Consume(event.kind == npm::NpmTcpStreamEventKindV1::kData ? event.bytes.size : 0)) return EINVAL;
            for (auto& fact : parsed) {
                if (fact.hello) trace_->hellos.push_back(std::move(fact));
            }
        }
        return 0;
    }

 private:
    TlsStreamTrace* trace_;
    std::map<uint64_t, npm::NpmTlsFramerV1> framers_;
};

struct TlsTrackerTrace {
    std::vector<npm::NpmTlsHandshakeResultV1> results;
    std::vector<std::string> calls;
    int aborts = 0;
};

class TlsTrackerProbe final : public npm::INpmProtocolModuleV1, public npm::INpmTcpStreamConsumerV1 {
 public:
    explicit TlsTrackerProbe(TlsTrackerTrace* trace) : trace_(trace) {}
    int OnInput(const npm::NpmInputEventV1& input, npm::INpmResultEmitterV1&) override {
        assert(input.kind == npm::NpmInputKindV1::kTcpPacket);
        return 0;
    }
    int OnSessionSnapshot(const npm::NpmSessionView&, int64_t, npm::INpmResultEmitterV1&) override { return 0; }
    int OnSessionEnd(const npm::NpmSessionView& session, npm::NpmSessionEndReason reason, int64_t observed,
                     npm::INpmResultEmitterV1&) override {
        trace_->calls.push_back("session_end");
        auto found = trackers_.find(session.session_id);
        if (found == trackers_.end()) return 0;
        std::optional<npm::NpmTlsHandshakeResultV1> result;
        if (found->second.OnSessionEnd(reason, observed, &result) != npm::NpmTlsTrackerErrorV1::kNone) return EINVAL;
        if (result) trace_->results.push_back(std::move(*result));
        trackers_.erase(found);
        return 0;
    }
    std::optional<int64_t> NextEventDeadlineNs() const override {
        std::optional<int64_t> minimum;
        for (const auto& item : trackers_) {
            const auto deadline = item.second.NextEventDeadlineNs();
            if (deadline && (!minimum || *deadline < *minimum)) minimum = *deadline;
        }
        return minimum;
    }
    int OnTime(const npm::NpmModuleTimeV1& time, npm::INpmResultEmitterV1&) override {
        trace_->calls.push_back("time");
        if (!time.watermark_ns) return 0;
        for (auto& item : trackers_) {
            std::optional<npm::NpmTlsHandshakeResultV1> result;
            if (item.second.OnCaptureWatermark(*time.watermark_ns, &result) != npm::NpmTlsTrackerErrorV1::kNone)
                return EINVAL;
            if (result) trace_->results.push_back(std::move(*result));
        }
        return 0;
    }
    int Finish(int64_t, npm::INpmResultEmitterV1&) override {
        assert(trackers_.empty());
        trace_->calls.push_back("finish");
        return 0;
    }
    void Abort() noexcept override {
        for (auto& item : trackers_) item.second.Abort();
        trackers_.clear();
        ++trace_->aborts;
    }
    int OnTcpStreamReadable(const npm::NpmTcpStreamContextV1& context, npm::INpmTcpStreamCursorV1& cursor,
                            npm::INpmResultEmitterV1&) override {
        assert(context.session);
        auto [found, inserted] =
            trackers_.try_emplace(context.session->session_id, context.session->session_id, npm::NpmTlsConfigV1{});
        (void)inserted;
        npm::NpmTcpStreamEventV1 event;
        while (cursor.Peek(&event)) {
            trace_->calls.push_back(event.kind == npm::NpmTcpStreamEventKindV1::kEnd ? "stream_end" : "data");
            std::optional<npm::NpmTlsHandshakeResultV1> result;
            if (found->second.Consume(context, event, &result) != npm::NpmTlsTrackerErrorV1::kNone) return EINVAL;
            if (cursor.Consume(event.kind == npm::NpmTcpStreamEventKindV1::kData ? event.bytes.size : 0)) return EINVAL;
            if (result) trace_->results.push_back(std::move(*result));
        }
        return 0;
    }

 private:
    TlsTrackerTrace* trace_;
    std::map<uint64_t, npm::NpmTlsHandshakeTrackerV1> trackers_;
};

npm::NpmModuleCatalogV1 TlsTrackerCatalog(TlsTrackerTrace* trace) {
    auto catalog = npm::ProductionNpmModuleCatalogV1();
    catalog.erase(catalog.begin() + 4);
    npm::NpmModuleRegistrationV1 registration;
    registration.module_id = "tls";
    registration.entity_ids = {"tls_handshake"};
    registration.prepare = [trace](const npm::NpmBasicTaskConfig&, std::string_view json,
                                   npm::NpmPreparedModuleV1* output) {
        npm::NpmTlsConfigV1 config;
        if (npm::ParseNpmTlsConfigV1(json, &config).error != npm::NpmTlsConfigErrorV1::kNone)
            return npm::NpmProtocolContractStatusV1{npm::NpmProtocolContractErrorV1::kInvalidPlan, "tls"};
        output->plan = npm::NpmTlsModulePlanV1(config);
        output->create = [trace](npm::NpmProtocolContext&, std::shared_ptr<npm::INpmTaskBudget>) {
            npm::NpmModuleInstanceV1 instance;
            auto module = std::make_unique<TlsTrackerProbe>(trace);
            instance.tcp_stream_consumer = module.get();
            instance.protocol = std::move(module);
            return instance;
        };
        return npm::NpmProtocolContractStatusV1{};
    };
    catalog.push_back(std::move(registration));
    return catalog;
}

npm::NpmModuleCatalogV1 TlsStreamCatalog(TlsStreamTrace* trace) {
    auto catalog = npm::ProductionNpmModuleCatalogV1();
    catalog.erase(catalog.begin() + 4);
    npm::NpmModuleRegistrationV1 registration;
    registration.module_id = "tls";
    registration.entity_ids = {"tls_handshake"};
    registration.prepare = [trace](const npm::NpmBasicTaskConfig&, std::string_view json,
                                   npm::NpmPreparedModuleV1* output) {
        npm::NpmTlsConfigV1 tls;
        if (npm::ParseNpmTlsConfigV1(json, &tls).error != npm::NpmTlsConfigErrorV1::kNone) {
            return npm::NpmProtocolContractStatusV1{npm::NpmProtocolContractErrorV1::kInvalidPlan, "tls"};
        }
        output->plan = npm::NpmTlsModulePlanV1(tls);
        output->create = [trace](npm::NpmProtocolContext&, std::shared_ptr<npm::INpmTaskBudget>) {
            npm::NpmModuleInstanceV1 instance;
            auto module = std::make_unique<TlsStreamProbe>(trace);
            instance.tcp_stream_consumer = module.get();
            instance.protocol = std::move(module);
            return instance;
        };
        return npm::NpmProtocolContractStatusV1{};
    };
    catalog.push_back(std::move(registration));
    return catalog;
}

npm::NpmModuleCatalogV1 TlsResultCatalog() { return npm::ProductionNpmModuleCatalogV1(); }

void TlsAppendU16(std::string* bytes, uint16_t value) {
    bytes->push_back(static_cast<char>(value >> 8));
    bytes->push_back(static_cast<char>(value));
}

std::string TlsTestMessage(uint8_t type, std::string body) {
    std::string message(1, static_cast<char>(type));
    message.push_back(0);
    TlsAppendU16(&message, static_cast<uint16_t>(body.size()));
    return message + body;
}

std::string TlsTestRecord(std::string message) {
    std::string record("\x16\x03\x03", 3);
    TlsAppendU16(&record, static_cast<uint16_t>(message.size()));
    return record + message;
}

std::string TlsTestHello(bool server) {
    std::string body("\x03\x03", 2);
    body += std::string(32, server ? 's' : 'c');
    body.push_back(0);
    if (server) {
        TlsAppendU16(&body, 0x1301);
        body.push_back(0);
        body += std::string("\x00\x06\x00\x2b\x00\x02\x03\x04", 8);
        return TlsTestRecord(TlsTestMessage(2, body));
    }
    TlsAppendU16(&body, 2);
    TlsAppendU16(&body, 0x1301);
    body += std::string("\x01\x00", 2);
    std::string extensions("\x00\x00\x00\x10\x00\x0e\x00\x00\x0b", 9);
    extensions += "example.com";
    extensions += std::string("\x00\x10\x00\x05\x00\x03\x02h2", 9);
    extensions += std::string("\x00\x2b\x00\x03\x02\x03\x04", 7);
    TlsAppendU16(&body, static_cast<uint16_t>(extensions.size()));
    return TlsTestRecord(TlsTestMessage(1, body + extensions));
}

std::vector<npm::NpmTlsFramedEventV1> RunTlsStreamBatches(bool split_batches) {
    TlsStreamTrace trace;
    auto catalog = TlsStreamCatalog(&trace);
    npm::NpmBasicTaskConfig config;
    assert(
        npm::ParseNpmBasicTaskConfig(
            R"({"input_namespace":"capture","source_domains":"1:77","features":"basic,labeling,tls","observing":"basic","parameters":"{\"schema_version\":1,\"tls\":{\"primary_label_ids\":[1001]}}"})",
            &config, true, catalog)
            .error == npm::NpmBasicTaskConfigError::kNone);
    config.analysis.out_of_order_tolerance_ns = 0;
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    LabelingMatcherStats labels;
    auto budget = std::make_shared<npm::NpmTaskBudget>(config.analysis);
    std::shared_ptr<arrow::Schema> schema;
    std::unique_ptr<npm::NpmBasicTaskRuntime> runtime;
    assert(npm::NpmBasicTaskRuntime::Create(config, &querier, flowsql::packet::PacketSchema(), &schema, &runtime,
                                            budget, new ProtocolLabelMatcher(&labels), catalog)
               .error == npm::NpmBasicTaskRuntimeError::kNone);
    const auto client = TlsTestHello(false);
    const auto server = TlsTestHello(true);
    const size_t client_split = 29;
    const size_t server_split = 21;
    auto packet = [&](bool reverse, const std::string& bytes, uint8_t flags, uint32_t sequence) {
        return reverse ? MakeIpv4TcpPacket("198.51.100.1", 443, "192.0.2.1", 50000,
                                           std::vector<uint8_t>(bytes.begin(), bytes.end()), flags, sequence)
                       : MakeIpv4TcpPacket("192.0.2.1", 50000, "198.51.100.1", 443,
                                           std::vector<uint8_t>(bytes.begin(), bytes.end()), flags, sequence);
    };
    std::vector<flowsql::packet::PacketRecord> records = {
        MakeBatchPacketRecord(packet(false, "", kTcpSyn, 100), 1, 10, 1),
        MakeBatchPacketRecord(packet(true, "", kTcpSyn | kTcpAck, 500), 1, 11, 2),
        MakeBatchPacketRecord(packet(false, client.substr(0, client_split), kTcpAck, 101), 1, 12, 3),
        MakeBatchPacketRecord(packet(false, client.substr(client_split), kTcpAck, 101 + client_split), 1, 13, 4),
        MakeBatchPacketRecord(packet(true, server.substr(0, server_split), kTcpAck, 501), 1, 14, 5),
        MakeBatchPacketRecord(packet(true, server.substr(server_split), kTcpAck, 501 + server_split), 1, 15, 6)};
    auto process = [&](std::shared_ptr<arrow::RecordBatch> input) {
        std::weak_ptr<arrow::RecordBatch> owner = input;
        std::shared_ptr<arrow::RecordBatch> output;
        assert(runtime->ProcessOfflineBatch(input, &output).error == npm::NpmBasicOfflineBatchError::kNone);
        assert(output && output->schema()->Equals(*schema, true));
        output.reset();
        input.reset();
        assert(owner.expired());
    };
    if (split_batches) {
        for (const auto& record : records) process(MakeEncodedPacketBatch({record}));
    } else {
        process(MakeEncodedPacketBatch(records));
    }
    assert(trace.hellos.size() == 2 && trace.packets == 6);
    assert(trace.hellos[0].hello->sni == "example.com" && trace.hellos[0].hello->complete_at_ns == 13);
    assert(trace.hellos[0].hello->offered_alpn == std::vector<std::string>{"h2"});
    assert(trace.hellos[0].hello->offered_versions == std::vector<uint16_t>{0x0304});
    assert(trace.hellos[1].hello->selected_version == 0x0304 && trace.hellos[1].hello->cipher_suite == 0x1301);
    assert(trace.hellos[1].hello->complete_at_ns == 15);
    std::shared_ptr<arrow::RecordBatch> eof;
    assert(runtime->FlushOffline(20, &eof).error == npm::NpmEofFlushError::kNone);
    eof.reset();
    assert(trace.finishes == 1 && trace.aborts == 0 && labels.release_calls == 1);
    auto facts = std::move(trace.hellos);
    runtime.reset();
    assert(npm::NpmTrackedBudgetBytes(budget->Usage()) == 0);
    return facts;
}

void TestTlsT1RecordBatchIntegration() {
    const auto one_batch = RunTlsStreamBatches(false);
    const auto split = RunTlsStreamBatches(true);
    assert(one_batch.size() == 2 && split.size() == 2);
    for (size_t index = 0; index < one_batch.size(); ++index) {
        assert(one_batch[index].direction == split[index].direction);
        assert(one_batch[index].kind == split[index].kind);
        assert(one_batch[index].hello->legacy_version == split[index].hello->legacy_version);
        assert(one_batch[index].hello->sni == split[index].hello->sni);
        assert(one_batch[index].hello->offered_alpn == split[index].hello->offered_alpn);
        assert(one_batch[index].hello->selected_version == split[index].hello->selected_version);
        assert(one_batch[index].hello->cipher_suite == split[index].hello->cipher_suite);
        assert(one_batch[index].hello->complete_at_ns == split[index].hello->complete_at_ns);
    }
}

void TestTlsT2RuntimeFinalDrain() {
    TlsTrackerTrace trace;
    auto catalog = TlsTrackerCatalog(&trace);
    npm::NpmBasicTaskConfig config;
    assert(
        npm::ParseNpmBasicTaskConfig(
            R"({"input_namespace":"capture","source_domains":"1:77","features":"basic,labeling,tls","observing":"basic","parameters":"{\"schema_version\":1,\"tls\":{\"primary_label_ids\":[1001]}}"})",
            &config, true, catalog)
            .error == npm::NpmBasicTaskConfigError::kNone);
    config.analysis.out_of_order_tolerance_ns = 0;
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    LabelingMatcherStats labels;
    auto budget = std::make_shared<npm::NpmTaskBudget>(config.analysis);
    std::shared_ptr<arrow::Schema> schema;
    std::unique_ptr<npm::NpmBasicTaskRuntime> runtime;
    assert(npm::NpmBasicTaskRuntime::Create(config, &querier, flowsql::packet::PacketSchema(), &schema, &runtime,
                                            budget, new ProtocolLabelMatcher(&labels), catalog)
               .error == npm::NpmBasicTaskRuntimeError::kNone);
    const auto client = TlsTestHello(false);
    const auto syn = MakeIpv4TcpPacket("192.0.2.1", 50000, "198.51.100.1", 443, {}, kTcpSyn, 100);
    const auto hello = MakeIpv4TcpPacket("192.0.2.1", 50000, "198.51.100.1", 443,
                                         std::vector<uint8_t>(client.begin(), client.end()), kTcpAck, 101);
    const auto reset = MakeIpv4TcpPacket("192.0.2.1", 50000, "198.51.100.1", 443, {}, kTcpRst, 101 + client.size());
    auto input = MakeEncodedPacketBatch({MakeBatchPacketRecord(syn, 1, 10, 1), MakeBatchPacketRecord(hello, 1, 11, 2),
                                         MakeBatchPacketRecord(reset, 1, 12, 3)});
    std::shared_ptr<arrow::RecordBatch> output;
    assert(runtime->ProcessOfflineBatch(input, &output).error == npm::NpmBasicOfflineBatchError::kNone);
    input.reset();
    output.reset();
    assert(trace.results.size() == 1 && trace.results[0].outcome == npm::NpmTlsOutcomeV1::kIncomplete);
    assert(trace.results[0].incomplete_reason == npm::NpmTlsIncompleteReasonV1::kSessionEnd);
    const auto stream_end = std::find(trace.calls.begin(), trace.calls.end(), "stream_end");
    const auto session_end = std::find(trace.calls.begin(), trace.calls.end(), "session_end");
    assert(stream_end != trace.calls.end() && session_end != trace.calls.end() && stream_end < session_end);
    std::shared_ptr<arrow::RecordBatch> eof;
    assert(runtime->FlushOffline(20, &eof).error == npm::NpmEofFlushError::kNone);
    eof.reset();
    assert(trace.results.size() == 1 && trace.calls.back() == "finish" && trace.aborts == 0);
    runtime.reset();
    assert(labels.release_calls == 1 && npm::NpmTrackedBudgetBytes(budget->Usage()) == 0);
}

void TestDnsT3RuntimeRouting() {
    const auto& catalog = npm::ProductionNpmModuleCatalogV1();

    const std::vector<uint8_t> query = {0, 9, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 'a', 0, 0, 1, 0, 1};
    auto answer = query;
    answer[2] = 0x80;
    const auto q = MakeIpv6UdpPacket("2001:db8::1", 53000, "2001:db8::2", 443, query);
    const auto a = MakeIpv6UdpPacket("2001:db8::2", 443, "2001:db8::1", 53000, answer);
    const auto batch =
        MakeEncodedPacketBatch({MakeBatchPacketRecord(q, 1, 100, 1), MakeBatchPacketRecord(a, 1, 200, 2),
                                MakeBatchPacketRecord(q, 2, 300, 3), MakeBatchPacketRecord(a, 2, 400, 4)});
    const auto first_query = MakeEncodedPacketBatch({MakeBatchPacketRecord(q, 1, 100, 1)});
    const auto remaining =
        MakeEncodedPacketBatch({MakeBatchPacketRecord(a, 1, 200, 2), MakeBatchPacketRecord(q, 2, 300, 3),
                                MakeBatchPacketRecord(a, 2, 400, 4)});
    struct Scenario {
        const char* features;
        const char* observing;
    };
    for (const auto scenario :
         {Scenario{"dns,labeling", "dns_transaction"}, Scenario{"labeling,dns,basic", "dns_transaction"},
          Scenario{"basic,dns,labeling", "basic"}, Scenario{"basic,session,dns,labeling", "session"}}) {
        ContextDictionary dictionary;
        ContextProtocol protocol(&dictionary);
        ContextPool pool(&protocol);
        SinglePoolQuerier querier(&pool);
        LabelingMatcherStats labels;
        ResultConsumerTrace trace;
        npm::NpmBasicTaskConfig config;
        const std::string json =
            std::string(R"({"input_namespace":"capture","source_domains":"1:77;2:78","features":")") +
            scenario.features + R"(","observing":")" + scenario.observing + R"("})";
        assert(npm::ParseNpmBasicTaskConfig(json.c_str(), &config, true, catalog).error ==
               npm::NpmBasicTaskConfigError::kNone);
        config.parameters_json = R"({"schema_version":1,"dns":{"primary_label_ids":[1001]}})";
        auto budget = std::make_shared<npm::NpmTaskBudget>(config.analysis);
        std::shared_ptr<arrow::Schema> schema;
        std::unique_ptr<npm::NpmBasicTaskRuntime> runtime;
        assert(npm::NpmBasicTaskRuntime::Create(config, &querier, flowsql::packet::PacketSchema(), &schema, &runtime,
                                                budget, new ProtocolLabelMatcher(&labels), catalog,
                                                std::make_unique<RecordingResultConsumer>(&trace))
                   .error == npm::NpmBasicTaskRuntimeError::kNone);
        std::shared_ptr<arrow::RecordBatch> output;
        assert(runtime->ProcessOfflineBatch(first_query, &output).error == npm::NpmBasicOfflineBatchError::kNone);
        assert(runtime->MaintenancePlan().event_deadline_ns == 5'000'000'100);
        output.reset();
        assert(runtime->ProcessOfflineBatch(remaining, &output).error == npm::NpmBasicOfflineBatchError::kNone);
        assert(output && output->schema()->Equals(*schema, true));
        const int64_t foreground_dns =
            std::string_view(scenario.observing) == "dns_transaction" ? output->num_rows() : 0;
        output.reset();
        assert(runtime->FlushOffline(500, &output).error == npm::NpmEofFlushError::kNone);
        assert(output && output->schema()->Equals(*schema, true));
        assert(trace.dns_domains == (std::vector<uint64_t>{77, 78}));
        assert(trace.dns_ids == (std::vector<uint16_t>{9, 9}));
        assert(trace.dns_outcomes == (std::vector<std::string>{"matched", "matched"}));
        if (std::string_view(scenario.features).find("basic") != std::string_view::npos)
            assert(std::find(trace.entities.begin(), trace.entities.end(), "basic") != trace.entities.end());
        if (std::string_view(scenario.features).find("session") != std::string_view::npos)
            assert(std::find(trace.entities.begin(), trace.entities.end(), "session") != trace.entities.end());
        assert(foreground_dns == (std::string_view(scenario.observing) == "dns_transaction" ? 2 : 0));
        assert(trace.finishes == 1);
        output.reset();
        runtime.reset();
        assert(npm::NpmTrackedBudgetBytes(budget->Usage()) == 0 && labels.release_calls == 1);
    }
    {
        ContextDictionary dictionary;
        ContextProtocol protocol(&dictionary);
        ContextPool pool(&protocol);
        SinglePoolQuerier querier(&pool);
        LabelingMatcherStats labels;
        ResultConsumerTrace trace;
        trace.fail_at = 2;
        npm::NpmBasicTaskConfig config;
        assert(
            npm::ParseNpmBasicTaskConfig(
                R"({"input_namespace":"capture","source_domains":"1:77;2:78","features":"dns,labeling","observing":"dns_transaction"})",
                &config, true, catalog)
                .error == npm::NpmBasicTaskConfigError::kNone);
        config.parameters_json = R"({"schema_version":1,"dns":{"primary_label_ids":[1001]}})";
        auto budget = std::make_shared<npm::NpmTaskBudget>(config.analysis);
        std::shared_ptr<arrow::Schema> schema;
        std::unique_ptr<npm::NpmBasicTaskRuntime> runtime;
        assert(npm::NpmBasicTaskRuntime::Create(config, &querier, flowsql::packet::PacketSchema(), &schema, &runtime,
                                                budget, new ProtocolLabelMatcher(&labels), catalog,
                                                std::make_unique<RecordingResultConsumer>(&trace))
                   .error == npm::NpmBasicTaskRuntimeError::kNone);
        std::shared_ptr<arrow::RecordBatch> output;
        assert(runtime->ProcessOfflineBatch(batch, &output).error ==
               npm::NpmBasicOfflineBatchError::kBatchProcessError);
        assert(!output && trace.attempts == 2 && trace.dns_outcomes == (std::vector<std::string>{"matched"}));
        assert(trace.cancels == 1 && trace.finishes == 0);
        runtime.reset();
        assert(npm::NpmTrackedBudgetBytes(budget->Usage()) == 0);
    }
}

void TestHttp1T3RuntimeRouting() {
    const auto& catalog = npm::ProductionNpmModuleCatalogV1();
    const std::string request = "GET /a HTTP/1.1\r\nHost: example.test\r\n\r\n";
    const std::string response = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
    std::vector<flowsql::packet::PacketRecord> packets;
    uint64_t packet_id = 1;
    for (uint32_t source : {1U, 2U}) {
        const auto syn = MakeIpv4TcpPacket("192.0.2.1", 50000, "198.51.100.2", 443, {}, kTcpSyn, 100);
        const auto synack = MakeIpv4TcpPacket("198.51.100.2", 443, "192.0.2.1", 50000, {}, kTcpSyn | kTcpAck, 500, 101);
        const auto req = MakeIpv4TcpPacket("192.0.2.1", 50000, "198.51.100.2", 443,
                                           std::vector<uint8_t>(request.begin(), request.end()), kTcpAck, 101, 501);
        const auto resp = MakeIpv4TcpPacket("198.51.100.2", 443, "192.0.2.1", 50000,
                                            std::vector<uint8_t>(response.begin(), response.end()), kTcpAck, 501,
                                            101 + request.size());
        const int64_t start = source == 1 ? 100 : 300;
        packets.push_back(MakeBatchPacketRecord(syn, source, start, packet_id++));
        packets.push_back(MakeBatchPacketRecord(synack, source, start + 1, packet_id++));
        packets.push_back(MakeBatchPacketRecord(req, source, start + 2, packet_id++));
        packets.push_back(MakeBatchPacketRecord(resp, source, start + 3, packet_id++));
    }
    const std::vector<uint8_t> dns_query = {0, 9, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 'a', 0, 0, 1, 0, 1};
    auto dns_answer = dns_query;
    dns_answer[2] = 0x80;
    packets.push_back(MakeBatchPacketRecord(MakeIpv6UdpPacket("2001:db8::1", 53000, "2001:db8::2", 443, dns_query), 1,
                                            600, packet_id++));
    packets.push_back(MakeBatchPacketRecord(MakeIpv6UdpPacket("2001:db8::2", 443, "2001:db8::1", 53000, dns_answer), 1,
                                            700, packet_id++));
    const auto batch = MakeEncodedPacketBatch(packets);
    struct Scenario {
        const char* features;
        const char* observing;
    };
    for (const auto scenario :
         {Scenario{"http1,labeling", "http1_transaction"}, Scenario{"labeling,http1,basic", "basic"},
          Scenario{"basic,session,http1,labeling", "session"},
          Scenario{"dns,basic,labeling,http1", "dns_transaction"}}) {
        ContextDictionary dictionary;
        ContextProtocol protocol(&dictionary);
        ContextPool pool(&protocol);
        SinglePoolQuerier querier(&pool);
        LabelingMatcherStats labels;
        ResultConsumerTrace trace;
        npm::NpmBasicTaskConfig config;
        const std::string json =
            std::string(R"({"input_namespace":"capture","source_domains":"1:77;2:78","features":")") +
            scenario.features + R"(","observing":")" + scenario.observing + R"("})";
        assert(npm::ParseNpmBasicTaskConfig(json.c_str(), &config, true, catalog).error ==
               npm::NpmBasicTaskConfigError::kNone);
        config.parameters_json =
            R"({"schema_version":1,"http1":{"primary_label_ids":[1001]},"dns":{"primary_label_ids":[1001]}})";
        auto budget = std::make_shared<npm::NpmTaskBudget>(config.analysis);
        std::shared_ptr<arrow::Schema> schema;
        std::unique_ptr<npm::NpmBasicTaskRuntime> runtime;
        assert(npm::NpmBasicTaskRuntime::Create(config, &querier, flowsql::packet::PacketSchema(), &schema, &runtime,
                                                budget, new ProtocolLabelMatcher(&labels), catalog,
                                                std::make_unique<RecordingResultConsumer>(&trace))
                   .error == npm::NpmBasicTaskRuntimeError::kNone);
        std::shared_ptr<arrow::RecordBatch> output;
        assert(runtime->ProcessOfflineBatch(batch, &output).error == npm::NpmBasicOfflineBatchError::kNone);
        assert(output && output->schema()->Equals(*schema, true));
        int64_t foreground_http = std::string_view(scenario.observing) == "http1_transaction" ? output->num_rows() : 0;
        output.reset();
        assert(runtime->FlushOffline(800, &output).error == npm::NpmEofFlushError::kNone);
        if (std::string_view(scenario.observing) == "http1_transaction") foreground_http += output->num_rows();
        assert(trace.http_domains == (std::vector<uint64_t>{77, 78}));
        assert(trace.http_outcomes == (std::vector<std::string>{"matched", "matched"}));
        assert(trace.http_statuses == (std::vector<uint16_t>{200, 200}));
        assert(foreground_http == (std::string_view(scenario.observing) == "http1_transaction" ? 2 : 0));
        if (std::string_view(scenario.features).find("dns") != std::string_view::npos) {
            assert(trace.dns_domains == (std::vector<uint64_t>{77}));
            assert(trace.dns_outcomes == (std::vector<std::string>{"matched"}));
        }
        if (std::string_view(scenario.features).find("basic") != std::string_view::npos)
            assert(std::find(trace.entities.begin(), trace.entities.end(), "basic") != trace.entities.end());
        if (std::string_view(scenario.features).find("session") != std::string_view::npos)
            assert(std::find(trace.entities.begin(), trace.entities.end(), "session") != trace.entities.end());
        assert(trace.finishes == 1);
        output.reset();
        runtime.reset();
        assert(npm::NpmTrackedBudgetBytes(budget->Usage()) == 0 && labels.release_calls == 1);
    }
    {
        ContextDictionary dictionary;
        ContextProtocol protocol(&dictionary);
        ContextPool pool(&protocol);
        SinglePoolQuerier querier(&pool);
        LabelingMatcherStats labels;
        ResultConsumerTrace trace;
        trace.fail_at = 2;
        npm::NpmBasicTaskConfig config;
        assert(
            npm::ParseNpmBasicTaskConfig(
                R"({"input_namespace":"capture","source_domains":"1:77;2:78","features":"http1,labeling","observing":"http1_transaction"})",
                &config, true, catalog)
                .error == npm::NpmBasicTaskConfigError::kNone);
        config.parameters_json = R"({"schema_version":1,"http1":{"primary_label_ids":[1001]}})";
        auto budget = std::make_shared<npm::NpmTaskBudget>(config.analysis);
        std::shared_ptr<arrow::Schema> schema;
        std::unique_ptr<npm::NpmBasicTaskRuntime> runtime;
        assert(npm::NpmBasicTaskRuntime::Create(config, &querier, flowsql::packet::PacketSchema(), &schema, &runtime,
                                                budget, new ProtocolLabelMatcher(&labels), catalog,
                                                std::make_unique<RecordingResultConsumer>(&trace))
                   .error == npm::NpmBasicTaskRuntimeError::kNone);
        std::shared_ptr<arrow::RecordBatch> output;
        assert(runtime->ProcessOfflineBatch(batch, &output).error ==
               npm::NpmBasicOfflineBatchError::kBatchProcessError);
        assert(!output && trace.attempts == 2 && trace.http_domains == (std::vector<uint64_t>{77}));
        assert(trace.cancels == 1 && trace.finishes == 0);
        runtime.reset();
        assert(npm::NpmTrackedBudgetBytes(budget->Usage()) == 0);
    }
}

void TestTlsT3RuntimeRouting() {
    const auto catalog = TlsResultCatalog();
    std::vector<flowsql::packet::PacketRecord> packets;
    uint64_t packet_id = 1;
    const auto client = TlsTestHello(false);
    const auto server = TlsTestHello(true);
    for (uint32_t source : {1U, 2U}) {
        const int64_t start = source == 1 ? 100 : 300;
        const auto syn = MakeIpv4TcpPacket("192.0.2.1", 50000, "198.51.100.2", 443, {}, kTcpSyn, 100);
        const auto synack = MakeIpv4TcpPacket("198.51.100.2", 443, "192.0.2.1", 50000, {}, kTcpSyn | kTcpAck, 500, 101);
        const auto request = MakeIpv4TcpPacket("192.0.2.1", 50000, "198.51.100.2", 443,
                                               std::vector<uint8_t>(client.begin(), client.end()), kTcpAck, 101, 501);
        const auto response =
            MakeIpv4TcpPacket("198.51.100.2", 443, "192.0.2.1", 50000,
                              std::vector<uint8_t>(server.begin(), server.end()), kTcpAck, 501, 101 + client.size());
        packets.push_back(MakeBatchPacketRecord(syn, source, start, packet_id++));
        packets.push_back(MakeBatchPacketRecord(synack, source, start + 1, packet_id++));
        packets.push_back(MakeBatchPacketRecord(request, source, start + 2, packet_id++));
        packets.push_back(MakeBatchPacketRecord(response, source, start + 3, packet_id++));
    }
    const auto http_request = std::string("GET /a HTTP/1.1\r\nHost: example.test\r\n\r\n");
    const auto http_response = std::string("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    packets.push_back(MakeBatchPacketRecord(
        MakeIpv4TcpPacket("192.0.2.1", 51000, "198.51.100.2", 443, {}, kTcpSyn, 1000), 1, 500, packet_id++));
    packets.push_back(MakeBatchPacketRecord(
        MakeIpv4TcpPacket("198.51.100.2", 443, "192.0.2.1", 51000, {}, kTcpSyn | kTcpAck, 2000, 1001), 1, 501,
        packet_id++));
    packets.push_back(MakeBatchPacketRecord(
        MakeIpv4TcpPacket("192.0.2.1", 51000, "198.51.100.2", 443,
                          std::vector<uint8_t>(http_request.begin(), http_request.end()), kTcpAck, 1001, 2001),
        1, 502, packet_id++));
    packets.push_back(
        MakeBatchPacketRecord(MakeIpv4TcpPacket("198.51.100.2", 443, "192.0.2.1", 51000,
                                                std::vector<uint8_t>(http_response.begin(), http_response.end()),
                                                kTcpAck, 2001, 1001 + http_request.size()),
                              1, 503, packet_id++));
    const std::vector<uint8_t> dns_query = {0, 9, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 'a', 0, 0, 1, 0, 1};
    auto dns_answer = dns_query;
    dns_answer[2] = 0x80;
    packets.push_back(MakeBatchPacketRecord(MakeIpv6UdpPacket("2001:db8::1", 53000, "2001:db8::2", 443, dns_query), 1,
                                            600, packet_id++));
    packets.push_back(MakeBatchPacketRecord(MakeIpv6UdpPacket("2001:db8::2", 443, "2001:db8::1", 53000, dns_answer), 1,
                                            700, packet_id++));
    const auto batch = MakeEncodedPacketBatch(packets);
    struct Scenario {
        const char* features;
        const char* observing;
    };
    for (const auto scenario : {Scenario{"tls,labeling", "tls_handshake"}, Scenario{"labeling,tls,basic", "basic"},
                                Scenario{"basic,session,dns,http1,tls,labeling", "session"},
                                Scenario{"labeling,http1,tls,dns,basic", "dns_transaction"},
                                Scenario{"http1,labeling,tls", "http1_transaction"}}) {
        ContextDictionary dictionary;
        ContextProtocol protocol(&dictionary);
        ContextPool pool(&protocol);
        SinglePoolQuerier querier(&pool);
        LabelingMatcherStats labels;
        ResultConsumerTrace trace;
        npm::NpmBasicTaskConfig config;
        const std::string json =
            std::string(R"({"input_namespace":"capture","source_domains":"1:77;2:78","features":")") +
            scenario.features + R"(","observing":")" + scenario.observing + R"("})";
        assert(npm::ParseNpmBasicTaskConfig(json.c_str(), &config, true, catalog).error ==
               npm::NpmBasicTaskConfigError::kNone);
        config.parameters_json =
            R"({"schema_version":1,"tls":{"primary_label_ids":[1001]},"http1":{"primary_label_ids":[1001]},"dns":{"primary_label_ids":[1001]}})";
        auto budget = std::make_shared<npm::NpmTaskBudget>(config.analysis);
        std::shared_ptr<arrow::Schema> schema;
        std::unique_ptr<npm::NpmBasicTaskRuntime> runtime;
        assert(npm::NpmBasicTaskRuntime::Create(config, &querier, flowsql::packet::PacketSchema(), &schema, &runtime,
                                                budget, new ProtocolLabelMatcher(&labels), catalog,
                                                std::make_unique<RecordingResultConsumer>(&trace))
                   .error == npm::NpmBasicTaskRuntimeError::kNone);
        std::shared_ptr<arrow::RecordBatch> output;
        assert(runtime->ProcessOfflineBatch(batch, &output).error == npm::NpmBasicOfflineBatchError::kNone);
        assert(output && output->schema()->Equals(*schema, true));
        int64_t foreground_tls = std::string_view(scenario.observing) == "tls_handshake" ? output->num_rows() : 0;
        output.reset();
        assert(runtime->FlushOffline(800, &output).error == npm::NpmEofFlushError::kNone);
        if (std::string_view(scenario.observing) == "tls_handshake") foreground_tls += output->num_rows();
        assert(trace.tls_domains == (std::vector<uint64_t>{77, 78}));
        assert(trace.tls_outcomes == (std::vector<std::string>{"server_hello_observed", "server_hello_observed"}));
        assert(trace.tls_ids.size() == 2 && trace.tls_ids[0] != 0 && trace.tls_ids[0] != trace.tls_ids[1]);
        assert(foreground_tls == (std::string_view(scenario.observing) == "tls_handshake" ? 2 : 0));
        if (std::string_view(scenario.features).find("basic") != std::string_view::npos)
            assert(std::find(trace.entities.begin(), trace.entities.end(), "basic") != trace.entities.end());
        if (std::string_view(scenario.features).find("session") != std::string_view::npos)
            assert(std::find(trace.entities.begin(), trace.entities.end(), "session") != trace.entities.end());
        if (std::string_view(scenario.features).find("dns") != std::string_view::npos)
            assert(trace.dns_domains == (std::vector<uint64_t>{77}));
        if (std::string_view(scenario.features).find("http1") != std::string_view::npos)
            assert(trace.http_domains == (std::vector<uint64_t>{77}));
        assert(trace.finishes == 1);
        output.reset();
        runtime.reset();
        assert(npm::NpmTrackedBudgetBytes(budget->Usage()) == 0 && labels.release_calls == 1);
    }
    {
        ContextDictionary dictionary;
        ContextProtocol protocol(&dictionary);
        ContextPool pool(&protocol);
        SinglePoolQuerier querier(&pool);
        LabelingMatcherStats labels;
        ResultConsumerTrace trace;
        trace.fail_at = 2;
        npm::NpmBasicTaskConfig config;
        assert(
            npm::ParseNpmBasicTaskConfig(
                R"({"input_namespace":"capture","source_domains":"1:77;2:78","features":"tls,labeling","observing":"tls_handshake"})",
                &config, true, catalog)
                .error == npm::NpmBasicTaskConfigError::kNone);
        config.parameters_json = R"({"schema_version":1,"tls":{"primary_label_ids":[1001]}})";
        auto budget = std::make_shared<npm::NpmTaskBudget>(config.analysis);
        std::shared_ptr<arrow::Schema> schema;
        std::unique_ptr<npm::NpmBasicTaskRuntime> runtime;
        assert(npm::NpmBasicTaskRuntime::Create(config, &querier, flowsql::packet::PacketSchema(), &schema, &runtime,
                                                budget, new ProtocolLabelMatcher(&labels), catalog,
                                                std::make_unique<RecordingResultConsumer>(&trace))
                   .error == npm::NpmBasicTaskRuntimeError::kNone);
        std::shared_ptr<arrow::RecordBatch> output;
        assert(runtime->ProcessOfflineBatch(batch, &output).error ==
               npm::NpmBasicOfflineBatchError::kBatchProcessError);
        assert(!output && trace.attempts == 2 && trace.tls_domains == (std::vector<uint64_t>{77}));
        assert(trace.cancels == 1 && trace.finishes == 0);
        runtime.reset();
        assert(npm::NpmTrackedBudgetBytes(budget->Usage()) == 0);
    }
}

// The same target packets must have the same timeout semantics with/without unrelated traffic.
void TestOfflineDeadlinesBeforePacketAdmission() {
    constexpr int64_t second = npm::kNpmNanosecondsPerSecond;
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    for (bool udp : {false, true})
        for (bool labeling : {false, true})
            for (bool split : {false, true})
                for (bool background : {false, true})
                    for (int64_t tolerance : {int64_t{0}, second / 2})
                        for (int64_t delta : {-1, 0, 1}) {
                            auto config = MakeRuntimeTaskConfig();
                            config.analysis.tcp_idle_timeout_ns = second;
                            config.analysis.udp_idle_timeout_ns = second;
                            config.analysis.out_of_order_tolerance_ns = tolerance;
                            config.features.labeling_enabled = labeling;
                            LabelingMatcherStats labels;
                            std::shared_ptr<arrow::Schema> schema;
                            std::unique_ptr<npm::NpmBasicTaskRuntime> runtime;
                            assert(npm::NpmBasicTaskRuntime::Create(
                                       config, &querier, flowsql::packet::PacketSchema(), &schema, &runtime, {},
                                       labeling ? new RecordingLabelMatcher(&labels) : nullptr)
                                       .error == npm::NpmBasicTaskRuntimeError::kNone);
                            auto target =
                                udp ? MakeIpv6UdpPacket("2001:db8::1", 40000, "2001:db8::2", 443, {})
                                    : MakeIpv4TcpPacket("192.0.2.1", 40000, "192.0.2.2", 443, {}, kTcpAck, 100);
                            auto other = MakeIpv4TcpPacket("192.0.2.3", 40001, "192.0.2.4", 80, {}, kTcpAck, 100);
                            const int64_t next = 2 * second + tolerance + delta;
                            std::vector<flowsql::packet::PacketRecord> records{
                                MakeBatchPacketRecord(target, 0, second, 1)};
                            if (background) records.push_back(MakeBatchPacketRecord(other, 0, next, 2));
                            records.push_back(MakeBatchPacketRecord(target, 0, next, 3));
                            std::vector<uint64_t> counts;
                            std::vector<std::string> reasons;
                            const auto collect = [&](const std::shared_ptr<arrow::RecordBatch>& rows) {
                                auto ports =
                                    std::static_pointer_cast<arrow::UInt16Array>(rows->GetColumnByName("a_port"));
                                auto packets =
                                    std::static_pointer_cast<arrow::UInt64Array>(rows->GetColumnByName("packets_ab"));
                                auto ends =
                                    std::static_pointer_cast<arrow::StringArray>(rows->GetColumnByName("end_reason"));
                                for (int64_t i = 0; i < rows->num_rows(); ++i) {
                                    if (ports->Value(i) != 40000) continue;
                                    counts.push_back(packets->Value(i));
                                    reasons.push_back(ends->GetString(i));
                                    if (labeling)
                                        assert(std::static_pointer_cast<arrow::UInt32Array>(
                                                   rows->GetColumnByName("primary_label_id"))
                                                   ->Value(i) == 1001);
                                }
                            };
                            std::shared_ptr<arrow::RecordBatch> output;
                            if (split) {
                                for (const auto& record : records) {
                                    output.reset();
                                    assert(
                                        runtime->ProcessOfflineBatch(MakeEncodedPacketBatch({record}), &output).error ==
                                        npm::NpmBasicOfflineBatchError::kNone);
                                    collect(output);
                                }
                            } else {
                                assert(runtime->ProcessOfflineBatch(MakeEncodedPacketBatch(records), &output).error ==
                                       npm::NpmBasicOfflineBatchError::kNone);
                                collect(output);
                            }
                            output.reset();
                            assert(runtime->FlushOffline(next, &output).error == npm::NpmEofFlushError::kNone);
                            collect(output);
                            assert(counts == (delta < 0 ? std::vector<uint64_t>{2} : std::vector<uint64_t>{1, 1}));
                            assert(reasons == (delta < 0 ? std::vector<std::string>{"eof"}
                                                         : std::vector<std::string>{"idle_timeout", "eof"}));
                            if (labeling) {
                                assert(labels.facts.size() == (delta < 0 ? 1U : 2U) + background);
                                assert(labels.release_calls == 1);
                            }
                        }
}

void TestOfflineIcmpDeadlinesBeforeReply() {
    constexpr int64_t second = npm::kNpmNanosecondsPerSecond;
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    for (bool labeling : {false, true})
        for (bool split : {false, true})
            for (bool background : {false, true})
                for (int64_t tolerance : {int64_t{0}, second / 2})
                    for (int64_t delta : {-1, 0, 1}) {
                        npm::NpmBasicTaskConfig config;
                        assert(
                            npm::ParseNpmBasicTaskConfig(
                                R"({"input_namespace":"capture","source_domains":"1:77","features":"icmp","observing":"icmp_event","out_of_order_tolerance_ns":"0"})",
                                &config)
                                .error == npm::NpmBasicTaskConfigError::kNone);
                        config.analysis.out_of_order_tolerance_ns = tolerance;
                        config.features.labeling_enabled = labeling;
                        LabelingMatcherStats labels;
                        std::shared_ptr<arrow::Schema> schema;
                        std::unique_ptr<npm::NpmBasicTaskRuntime> runtime;
                        assert(npm::NpmBasicTaskRuntime::Create(config, &querier, flowsql::packet::PacketSchema(),
                                                                &schema, &runtime, {},
                                                                labeling ? new RecordingLabelMatcher(&labels) : nullptr)
                                   .error == npm::NpmBasicTaskRuntimeError::kNone);
                        auto request = MakeControlPacket();
                        request.bytes[20] = 8;
                        request.bytes[21] = 0;
                        request.bytes[24] = 0x12;
                        request.bytes[25] = 0x34;
                        request.bytes[26] = 0;
                        request.bytes[27] = 9;
                        auto reply = request;
                        reply.bytes[20] = 0;
                        auto* ip = reinterpret_cast<flowsql::Ipv4Header*>(reply.bytes.data());
                        std::swap(ip->src_addr, ip->dst_addr);
                        std::swap(reply.layer.src_ip, reply.layer.dst_ip);
                        auto other = MakeIpv4TcpPacket("192.0.2.3", 40000, "192.0.2.4", 80, {}, kTcpAck, 100);
                        const int64_t next = 6 * second + tolerance + delta;
                        std::vector<flowsql::packet::PacketRecord> records{
                            MakeBatchPacketRecord(request, 1, second, 1)};
                        if (background) records.push_back(MakeBatchPacketRecord(other, 1, next, 2));
                        records.push_back(MakeBatchPacketRecord(reply, 1, next, 3));
                        std::vector<std::string> outcomes;
                        const auto collect = [&](const std::shared_ptr<arrow::RecordBatch>& rows) {
                            auto values =
                                std::static_pointer_cast<arrow::StringArray>(rows->GetColumnByName("outcome"));
                            for (int64_t i = 0; i < rows->num_rows(); ++i) outcomes.push_back(values->GetString(i));
                        };
                        std::shared_ptr<arrow::RecordBatch> output;
                        if (split) {
                            for (const auto& record : records) {
                                output.reset();
                                assert(runtime->ProcessOfflineBatch(MakeEncodedPacketBatch({record}), &output).error ==
                                       npm::NpmBasicOfflineBatchError::kNone);
                                collect(output);
                            }
                        } else {
                            assert(runtime->ProcessOfflineBatch(MakeEncodedPacketBatch(records), &output).error ==
                                   npm::NpmBasicOfflineBatchError::kNone);
                            collect(output);
                        }
                        output.reset();
                        assert(runtime->FlushOffline(next, &output).error == npm::NpmEofFlushError::kNone);
                        collect(output);
                        assert(outcomes == (delta < 0
                                                ? std::vector<std::string>{"echo_matched"}
                                                : std::vector<std::string>{"echo_request_only", "echo_reply_only"}));
                    }
}

void TestResultRouterPreservesSlicedNullableColumns() {
    auto config = MakeRuntimeTaskConfig();
    auto budget = std::make_shared<npm::NpmTaskBudget>(config.analysis);
    auto invoke = [](const std::function<int()>& fn) { return fn(); };
    npm::NpmResultRouter router({npm::NpmBasicEntityDescriptorV1()}, "basic", npm::MakeNpmResultContext("slice"),
                                budget, {}, invoke);
    auto first = MakeBasicEncodingResult(1);
    auto second = MakeBasicEncodingResult(2);
    second.protocol_status = npm::NpmProtocolStatus::kUnknown;
    second.protocol_id.reset();
    second.protocol_sub_id.reset();
    std::shared_ptr<arrow::RecordBatch> rows, expected, output;
    assert(npm::EncodeNpmBasicResults({first, second}, &rows) == npm::NpmBasicEncodeError::kNone);
    assert(npm::EncodeNpmBasicResults({second, first}, &expected) == npm::NpmBasicEncodeError::kNone);
    assert(router.Emit("basic", "basic", *rows->Slice(1, 1)) == 0);
    assert(router.Emit("basic", "basic", *rows->Slice(0, 1)) == 0);
    rows.reset();
    assert(router.Drain(&output) == 0 && output->Equals(*expected));
    output.reset();
    assert(budget->Usage().pending_output_bytes == 0);
}

void TestPeriodicDeadlineTracksReorderedAndClosedSessions() {
    constexpr int64_t second = npm::kNpmNanosecondsPerSecond;
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    auto config = MakeRuntimeTaskConfig();
    config.analysis.result_mode = npm::NpmResultMode::kPeriodicSnapshot;
    config.analysis.output_interval_ns = 15 * second;
    config.analysis.out_of_order_tolerance_ns = 40 * second;
    auto runtime = CreateRuntimeForTest(config, &querier);
    auto target = MakeIpv4TcpPacket("192.0.2.1", 40000, "192.0.2.2", 80, {}, kTcpAck, 100);
    auto close = MakeIpv4TcpPacket("192.0.2.1", 40000, "192.0.2.2", 80, {}, kTcpRst | kTcpAck, 100);
    auto other = MakeIpv4TcpPacket("192.0.2.3", 40001, "192.0.2.4", 80, {}, kTcpAck, 100);
    std::shared_ptr<arrow::RecordBatch> output;
    const auto send = [&](const PacketFixture& packet, int64_t timestamp) {
        output.reset();
        assert(
            runtime
                ->ProcessOfflineBatch(MakeEncodedPacketBatch({MakeBatchPacketRecord(packet, 0, timestamp, 1)}), &output)
                .error == npm::NpmBasicOfflineBatchError::kNone);
    };
    send(target, 31 * second);
    assert(output->num_rows() == 0);
    // A valid out-of-order packet moves the first pending window from [30,45) to [15,30).
    send(target, 16 * second);
    assert(output->num_rows() == 0);
    send(other, 70 * second);
    assert(output->num_rows() == 1 && PeriodicUInt(output, "revision") == 1);
    assert(PeriodicInt(output, "period_start_ns") == 15 * second);
    assert(PeriodicInt(output, "period_end_ns") == 30 * second);
    assert(PeriodicUInt(output, "interval_packets_ab") == 1 && PeriodicFlag(output, "period_complete"));
    // RST ends the remaining window before its grid boundary; watermark 32 must deliver it.
    send(close, 32 * second);
    assert(output->num_rows() == 0);
    send(other, 72 * second);
    assert(output->num_rows() == 1 && PeriodicUInt(output, "revision") == 2);
    assert(PeriodicInt(output, "period_start_ns") == 30 * second);
    assert(PeriodicUInt(output, "interval_packets_ab") == 2 && PeriodicUInt(output, "packets_ab") == 3);
    assert(PeriodicFlag(output, "is_final") && !PeriodicFlag(output, "period_complete"));
    auto budget = runtime->Budget();
    output.reset();
    runtime->Cancel();
    assert(npm::NpmTrackedBudgetBytes(budget->Usage()) == 0);
}

void TestMatcherReservationFollowsOwnership() {
    for (uint64_t reserved : {uint64_t{0}, uint64_t{3} * npm::kNpmMebibyte, uint64_t{64} * npm::kNpmMebibyte}) {
        for (int scenario = 0; scenario < 6; ++scenario) {
            auto config = MakeRuntimeTaskConfig();
            config.features.labeling_enabled = true;
            auto budget = std::make_shared<npm::NpmTaskBudget>(config.analysis);
            constexpr uint64_t unrelated = 128;
            assert(budget->Reserve(npm::NpmBudgetCategory::kModuleState, unrelated + reserved) ==
                   npm::NpmBudgetError::kNone);
            ContextDictionary dictionary;
            ContextProtocol protocol(&dictionary);
            ContextPool pool(&protocol, scenario == 3 ? flowsql::ProtocolPipelinePoolError::kExhausted
                                                      : flowsql::ProtocolPipelinePoolError::kNone);
            SinglePoolQuerier querier(&pool);
            LabelingMatcherStats labels;
            UnexpectedManagedFactory failing_factory;
            std::shared_ptr<arrow::Schema> schema;
            std::unique_ptr<npm::NpmBasicTaskRuntime> runtime;
            const auto created = npm::NpmBasicTaskRuntime::Create(
                config, &querier, scenario == 2 ? nullptr : flowsql::packet::PacketSchema(), &schema, &runtime, budget,
                new RecordingLabelMatcher(&labels), npm::ProductionNpmModuleCatalogV1(), {}, {},
                scenario == 4 ? &failing_factory : nullptr, reserved);
            std::shared_ptr<arrow::RecordBatch> output;
            if (scenario >= 2 && scenario <= 4) {
                assert(created.error != npm::NpmBasicTaskRuntimeError::kNone && !runtime);
                if (scenario == 4) assert(failing_factory.create_calls == 1);
            } else {
                assert(created.error == npm::NpmBasicTaskRuntimeError::kNone);
                assert(budget->Usage().module_state_bytes == unrelated + reserved);
                const auto packet = MakeIpv4TcpPacket("192.0.2.1", 40000, "192.0.2.2", 443, {}, kTcpAck, 100);
                const auto processed = runtime->ProcessOfflineBatch(
                    MakeEncodedPacketBatch({MakeBatchPacketRecord(packet, scenario == 5 ? 99 : 0, 1, 1)}), &output);
                if (scenario == 5) {
                    assert(processed.error == npm::NpmBasicOfflineBatchError::kBatchProcessError);
                } else {
                    assert(processed.error == npm::NpmBasicOfflineBatchError::kNone);
                    output.reset();
                    if (scenario == 0) {
                        assert(runtime->FlushOffline(1, &output).error == npm::NpmEofFlushError::kNone);
                        assert(output && output->num_rows() == 1 && budget->Usage().pending_output_bytes > 0);
                    } else {
                        runtime->Cancel();
                        runtime->Cancel();
                    }
                }
            }
            assert(labels.release_calls == 1 && budget->Usage().module_state_bytes == unrelated);
            runtime.reset();
            assert(labels.release_calls == 1);
            output.reset();
            assert(budget->Release(npm::NpmBudgetCategory::kModuleState, unrelated) == npm::NpmBudgetError::kNone);
            assert(npm::NpmTrackedBudgetBytes(budget->Usage()) == 0);
        }
    }
}

void TestLargeResultDeliveryWithinDefaultBudget() {
    constexpr int count = 30000;
    constexpr int64_t second = npm::kNpmNanosecondsPerSecond;
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    // EOF final, EOF periodic, normal realtime terminal Flush, and a common periodic deadline.
    for (int scenario = 0; scenario < 4; ++scenario) {
        const bool realtime = scenario == 2;
        auto config = MakeRuntimeTaskConfig(realtime ? npm::NpmRunMode::kRealtime : npm::NpmRunMode::kOffline);
        if (scenario != 0) config.analysis.result_mode = npm::NpmResultMode::kPeriodicSnapshot;
        config.analysis.out_of_order_tolerance_ns = 0;
        auto runtime =
            realtime ? CreateRealtimeRuntimeForTest(config, &querier) : CreateRuntimeForTest(config, &querier);
        auto budget = std::static_pointer_cast<npm::NpmTaskBudget>(runtime->Budget());
        std::shared_ptr<arrow::RecordBatch> output;
        for (int begin = 0; begin < count; begin += 512) {
            std::vector<flowsql::packet::PacketRecord> records;
            for (int index = begin; index < std::min(begin + 512, count); ++index) {
                auto packet = MakeIpv4TcpPacket("192.0.2.1", index + 1, "192.0.2.2", 80, {}, kTcpAck, 100);
                records.push_back(MakeBatchPacketRecord(packet, 0, second, index + 1));
            }
            output.reset();
            assert(runtime->ProcessOfflineBatch(MakeEncodedPacketBatch(records), &output).error ==
                   npm::NpmBasicOfflineBatchError::kNone);
            assert(output && output->num_rows() == 0);
        }
        output.reset();
        if (scenario == 3) {
            auto other = MakeIpv4TcpPacket("192.0.2.3", 40000, "192.0.2.4", 80, {}, kTcpAck, 100);
            assert(runtime
                       ->ProcessOfflineBatch(MakeEncodedPacketBatch({MakeBatchPacketRecord(
                                                 other, 0, config.analysis.output_interval_ns, count + 1)}),
                                             &output)
                       .error == npm::NpmBasicOfflineBatchError::kNone);
        } else {
            assert(runtime->FlushOffline(second, &output).error == npm::NpmEofFlushError::kNone);
        }
        assert(output && output->num_rows() == count && output->ValidateFull().ok());
        for (int row = 0; row < count; ++row) {
            assert(PeriodicUInt(output, "session_id", row) == static_cast<uint64_t>(row + 1));
            assert(PeriodicUInt(output, "revision", row) == 1);
            assert(PeriodicUInt(output, "packets_ab", row) == 1);
            assert(PeriodicUInt(output, "wire_bytes_total", row) == 40);
            assert(PeriodicFlag(output, "is_final", row) == (scenario != 3));
        }
        assert(budget->HighWaterMarks().pending_output_bytes < config.analysis.max_pending_output_bytes);
        printf("large result scenario=%d rows=%d pending_peak=%lu\n", scenario, count,
               budget->HighWaterMarks().pending_output_bytes);
        auto retained = output->GetColumnByName("session_id")->Slice(count - 1, 1);
        output.reset();
        if (scenario == 3) runtime->Cancel();
        runtime.reset();
        assert(budget->Usage().session_state_bytes == 0 && budget->Usage().module_state_bytes == 0);
        assert(budget->Usage().pending_output_bytes > 0);
        assert(std::static_pointer_cast<arrow::UInt64Array>(retained)->Value(0) == count);
        retained.reset();
        assert(npm::NpmTrackedBudgetBytes(budget->Usage()) == 0);
    }
}

void TestSafeResultProgress() {
    flowsql::CaptureSourceSetV2 sources;
    flowsql::CaptureQueueIdentityV1 first, second_input;
    first.source_id = 1;
    second_input.source_id = 2;
    sources.inputs = {first, second_input};
    flowsql::CaptureProgressTrackerV2 tracker(sources);
    flowsql::CaptureProgressV1 a, b;
    a.source_id = 1;
    b.source_id = 2;
    a.fact_sequence = b.fact_sequence = 1;
    a.capture_time_ns = 100;
    b.capture_time_ns = 80;
    a.source_idle_confirmed = b.source_idle_confirmed = true;
    a.backlog = b.backlog = flowsql::CaptureBacklogV1::kEmpty;
    assert(tracker.Observe({a}) == flowsql::CaptureProgressErrorV1::kNone && !tracker.CommonCandidateNs());
    assert(tracker.Observe({b}) == flowsql::CaptureProgressErrorV1::kNone && tracker.CommonCandidateNs() == 80);
    b.fact_sequence = 2;
    b.backlog = flowsql::CaptureBacklogV1::kPresent;
    assert(tracker.Observe({b}) == flowsql::CaptureProgressErrorV1::kNone && !tracker.CommonCandidateNs());
    b.fact_sequence = 3;
    b.backlog = flowsql::CaptureBacklogV1::kUnknown;
    assert(tracker.Observe({b}) == flowsql::CaptureProgressErrorV1::kNone && !tracker.CommonCandidateNs());
    b.fact_sequence = 4;
    b.backlog = flowsql::CaptureBacklogV1::kEmpty;
    b.capture_time_ns = 150;
    assert(tracker.Observe({b}) == flowsql::CaptureProgressErrorV1::kNone && tracker.CommonCandidateNs() == 100);
    constexpr int64_t second = npm::kNpmNanosecondsPerSecond;
    ContextDictionary dictionary;
    ContextProtocol protocol(&dictionary);
    ContextPool pool(&protocol);
    SinglePoolQuerier querier(&pool);
    for (int scenario = 0; scenario < 4; ++scenario) {
        ResultConsumerTrace trace;
        auto config = MakeRuntimeTaskConfig(npm::NpmRunMode::kRealtime);
        config.analysis.result_mode = npm::NpmResultMode::kPeriodicSnapshot;
        config.analysis.output_interval_ns = 30 * second;
        config.analysis.out_of_order_tolerance_ns = 2 * second;
        auto runtime = CreateConsumingRuntime(config, &querier, &trace);
        std::shared_ptr<arrow::RecordBatch> output;
        if (scenario != 1) {
            auto packet = MakeIpv4TcpPacket("192.0.2.1", 40000, "192.0.2.2", 80, {}, kTcpAck, 100);
            assert(runtime
                       ->ProcessOfflineBatch(MakeEncodedPacketBatch({MakeBatchPacketRecord(packet, 0, second, 1)}),
                                             &output)
                       .error == npm::NpmBasicOfflineBatchError::kNone);
            assert(trace.progress.empty());  // ProcessBlock cannot publish before ReleaseBlock/facts.
        }
        int64_t tick = 1;
        auto drive = [&](bool known, bool backlog, bool idle) {
            output.reset();
            return runtime->DriveRealtimeMaintenance(
                RealtimeMaintenanceInput(tick++, 1000, 32 * second, false, idle, known, backlog), &output);
        };
        assert(drive(false, false, true).error == npm::NpmBasicRealtimeMaintenanceError::kNone);
        assert(drive(true, true, true).error == npm::NpmBasicRealtimeMaintenanceError::kNone);
        assert(drive(true, false, false).error == npm::NpmBasicRealtimeMaintenanceError::kNone);
        assert(trace.progress.empty());
        if (scenario == 2) trace.fail_at = trace.attempts + 1;
        if (scenario == 3) trace.progress_error = EIO;
        auto status = drive(true, false, true);
        if (scenario >= 2) {
            assert(status.error != npm::NpmBasicRealtimeMaintenanceError::kNone && trace.progress.empty());
            assert(runtime->State() == npm::NpmEofFlushState::kFailed);
        } else {
            assert(status.error == npm::NpmBasicRealtimeMaintenanceError::kNone);
            assert(trace.progress.size() == 1 && trace.progress[0].closed_before_ns == 30 * second);
            assert(trace.rows_at_progress[0] == (scenario == 1 ? 0 : 1));
            assert(drive(true, false, true).error == npm::NpmBasicRealtimeMaintenanceError::kNone);
            assert(trace.progress.size() == 1);
            output.reset();
            assert(runtime->FlushOffline(1000, &output).error == npm::NpmEofFlushError::kNone);
            assert(trace.progress.size() == 1);  // EOF/partial tail does not invent a closed period.
        }
        output.reset();
        runtime->Cancel();
        assert(trace.progress.size() == (scenario < 2 ? 1 : 0));
    }
}

int main(int argc, char** argv) {
    if (argc == 1 || (argc == 2 && std::string_view(argv[1]) == "--progress-only")) {
        TestSafeResultProgress();
        if (argc == 2) return 0;
    }
    if (argc == 1 || (argc == 2 && std::string_view(argv[1]) == "--review-fixes-only")) {
        TestOfflineDeadlinesBeforePacketAdmission();
        TestOfflineIcmpDeadlinesBeforeReply();
        TestResultRouterPreservesSlicedNullableColumns();
        TestPeriodicDeadlineTracksReorderedAndClosedSessions();
        TestMatcherReservationFollowsOwnership();
        TestLargeResultDeliveryWithinDefaultBudget();
        if (argc == 2) return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--periodic-only") {
        TestNpmBasicTaskPreservesLatePacketDiagnostic();
        TestNpmBasicFinalUsesGenerationTime();
        TestNpmBasicTaskFlushAppliesPendingCaptureFacts();
        TestNpmPeriodicConfigurationParsing();
        TestNpmBasicPeriodicOrderDiagnosticsAndStalledBudget();
        TestNpmBasicTaskRuntimeRealtimeMaintenanceAndRevisions();
        TestNpmBasicTaskRuntimeRoutesRealtimePeriodicSnapshots();
        TestNpmBasicTaskRuntimeClosesRealtimeSessionsAfterPeriodicSnapshots();
        TestNpmBasicTaskRuntimeIsolatesRealtimeTupleReuseAfterPeriodicSnapshots();
        TestNpmBasicTaskRuntimeFlushesRealtimeSessionsAtEofAfterPeriodicSnapshots();
        TestNpmBasicTaskRuntimeCleansRealtimeBackpressureFailures();
        TestNpmBasicTaskRuntimeCancelsSessionStateWithoutTerminalResults();
        TestNpmBasicTaskRuntimeRetiresRealtimeIdleSessionsOnce();
        TestNpmBasicRealtimeTaskIsolationAndSlowSinkBudget();
        TestProtocolLifecycleRealtimeDeferralAndCallbackCancel();
        TestProtocolLifecycleFinishFailureAbortsAndDoesNotReplay();
        TestProtocolLifecycleTupleReuseAndBudgetFailureCleanup();
        TestUnifiedConsumerBasicSessionSnapshotsFinalsAndRunIdentity();
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--tcp-stream-only") {
        TestTcpStreamRecordBatchTypedResultRouting();
        TestTcpStreamRuntimeAdmissionAndLifecycle();
        TestTcpStreamRuntimeDeadlineAndCancel();
        TestTcpStreamRuntimeFailureDiagnostics();
        TestTcpStreamRuntimeCancelInsideCallback();
        TestNpmTcpStreamOpenGatesAndFrozenConfig();
        return 0;
    }
    assert(argc == 1);
    TestTcpStreamRecordBatchTypedResultRouting();
    TestTcpStreamRuntimeAdmissionAndLifecycle();
    TestTcpStreamRuntimeDeadlineAndCancel();
    TestTcpStreamRuntimeFailureDiagnostics();
    TestTcpStreamRuntimeCancelInsideCallback();
    TestNpmTcpStreamParameters();
    TestNpmTcpStreamModuleInstanceContract();
    TestNpmTcpStreamOpenGatesAndFrozenConfig();
    TestDnsT0OpenContract();
    TestHttp1T0OpenContract();
    TestTlsT0OpenContract();
    TestIcmpT0OpenContract();
    TestIcmpModuleAndBudget();
    TestIcmpMixedBatchAndSplitEquivalence();
    TestTlsT1RecordBatchIntegration();
    TestTlsT2RuntimeFinalDrain();
    TestDnsT3RuntimeRouting();
    TestHttp1T3RuntimeRouting();
    TestTlsT3RuntimeRouting();
    TestProtocolProductionCatalogAndConcurrentDualEntityControlRuntime();
    TestResultRouterValidationBudgetAndUnobservedRelease();
    TestUnifiedConsumerBasicSessionSnapshotsFinalsAndRunIdentity();
    TestUnifiedConsumerProtocolCopyFailurePrefixAndOwnerBudget();
    TestUnifiedConsumerFinishFailureAndCancellation();
    TestProtocolLifecycleRealtimeDeferralAndCallbackCancel();
    TestProtocolLifecycleFinishFailureAbortsAndDoesNotReplay();
    TestProtocolLifecycleTupleReuseAndBudgetFailureCleanup();
    TestProtocolLifecycleEofOrderAndObservingIndependence();
    TestProtocolLifecycleDeadlinesSessionsAndEof();
    TestProtocolLabelIsolationAndAtomicFactoryFailure();
    TestProtocolCatalogOpenAndDispatch();
    TestIpv6WireVersionAndEthernetControlBounds();
    TestProtocolOpenRejectionsAndControlCompatibility();
    TestConfigDefaultsAndEnumContract();
    TestConfigRangesAndUnsupportedValues();
    TestObservationDomainMapping();
    TestBasicResultNullableContract();
    TestBasicResultSchema();
    TestSessionResultStatusAndNullableContract();
    TestSessionResultSchema();
    TestBudgetAccounting();
    TestBorrowedViewsAndModuleInterfaces();
    TestTimeCapabilityRequirements();
    TestSessionPacketCanonicalizationAndObservationDomains();
    TestSessionPacketPayloadAndInvalidInputs();
    TestSessionPacketFragmentsAndTunnelContext();
    TestTcpPerformanceHandshakeStates();
    TestTcpPerformanceRttKarnAndTimestampRegression();
    TestTcpPerformanceBudgetCleanupAndIsolation();
    TestTcpLedgerWrapOverlapAndKarn();
    TestTcpLedgerOutOfOrderAckAndAmbiguity();
    TestTcpLedgerLimitsAndBudgetAtomicity();
    TestSessionTrackerPeakStateAndRangeProfile();
    TestSessionPerformanceUdpProjectionAndRates();
    TestSessionPerformanceTcpRatesAndSequenceAmbiguity();
    TestSessionPerformanceProtocolAndViewErrorsAreAtomic();
    TestSessionAnalysisModuleTcpRevisionsAndWriterAtomicity();
    TestSessionAnalysisModuleUdpFinalOnlyAndInvalidFinal();
    TestSessionAnalysisModuleRevisionBudgetAndDestructorCleanup();
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
    TestProcessNpmPacketSamplesTerminalPayloadBeforeFinalization();
    TestProcessNpmPacketErrorsAreStructuredAndAtomic();
    TestProcessNpmOfflinePacketBatchOrderAndWatermark();
    TestProcessNpmOfflinePacketBatchErrorsAndAtomicOutput();
    TestFlowLabelingAdmissionBatchSessionReuseAndFailureAtomicity();
    TestFlowLabelingAdmissionTracksIntraBlockSessionLifecycles();
    TestNpmProtocolContextErrorsDictionaryAndRaii();
    TestNpiPipelinePoolOptionAndLeaseContract();
    TestNpmPacketBatchViewBorrowedDecodeAndOwnership();
    TestNpmPacketBatchViewRejectsSchemaAndColumnErrors();
    TestNpmPacketBatchViewRejectsInvalidRows();
    TestNpmBasicResultProjectionRevisionsAndLabels();
    TestNpmBasicResultProjectionErrorsDoNotConsumeRevision();
    TestNpmBasicResultArrowEncoding();
    TestNpmBasicResultArrowEncodingRejectsInvalidInput();
    TestNpmSessionResultArrowEncoding();
    TestNpmSessionResultArrowEncodingRejectsInvalidInput();
    TestNpmSessionResultPendingOutputLeaseAndErrors();
    TestNpmBasicResultCollectorCopiesAndDrainsUnifiedBatches();
    TestNpmBasicResultCollectorFailuresKeepPendingAndOutput();
    TestNpmBasicResultCollectorRoutesEnabledEntities();
    TestNpmEofFlusherSuccessEmptyAndRepeated();
    TestNpmEofFlusherTerminalAndErrorPaths();
    TestNpmBasicResultPendingOutputLease();
    TestNpmBasicResultPendingOutputBudgetAndLifetime();
    TestNpmBasicResultPendingOutputFailuresAreAtomic();
    TestNpmBasicTaskConfigParsesOwnedValues();
    TestNpmBasicTaskConfigInputDefaults();
    TestNpmBasicTaskConfigRejectsMalformedFieldsAtomically();
    TestNpmBasicTaskConfigRejectsMappingsAndRanges();
    TestNpmBasicTaskConfigNumericBoundaries();
    TestNpmBasicTaskConfigFeatureSelection();
    TestNpmBasicTaskConfigNormalizesParametersV1();
    TestNpmPeriodicConfigurationParsing();
    TestNpmBasicTaskConfigRejectsParameterSourceConflicts();
    TestNpmBasicTaskConfigPreservesParameterFailuresAtomically();
    TestNpmParametersV1OwnsCanonicalConfig();
    TestNpmParametersV1RejectsEnvelopeAndDuplicatesAtomically();
    TestNpmParametersV1ConsumesOnlyEnabledAvailableModules();
    TestNpmParametersV1UsesCoreNamespaceExclusively();
    TestNpmParametersV1ValidatesCoreAndConditionalLabeling();
    TestNpmTaskBudgetLimitsAtomicityAndIsolation();
    TestNpmTaskBudgetConcurrentAccountingAndSharedLifetime();
    TestNpmBasicTaskRuntimeMatchesLegacyAndParametersV1Offline();
    TestNpmBasicTaskRuntimeCreatesExclusiveInitialState();
    TestNpmBasicTaskRuntimeSelectsObservedSchema();
    TestNpmBasicTaskRuntimeRejectsOpenFailuresAtomically();
    TestNpmBasicTaskRuntimeRealtimeCapabilityInjection();
    TestNpmBasicTaskRuntimeMatchesLegacyAndParametersV1Realtime();
    TestNpmBasicPeriodicOrderDiagnosticsAndStalledBudget();
    TestNpmBasicTaskRuntimeRealtimeMaintenanceAndRevisions();
    TestNpmBasicTaskRuntimeRoutesRealtimePeriodicSnapshots();
    TestNpmBasicTaskRuntimeClosesRealtimeSessionsAfterPeriodicSnapshots();
    TestNpmBasicTaskRuntimeIsolatesRealtimeTupleReuseAfterPeriodicSnapshots();
    TestNpmBasicTaskRuntimeFlushesRealtimeSessionsAtEofAfterPeriodicSnapshots();
    TestNpmBasicTaskRuntimeCleansSessionModuleFailures();
    TestNpmBasicTaskRuntimeCleansRealtimeBackpressureFailures();
    TestNpmBasicTaskRuntimeCancelsSessionStateWithoutTerminalResults();
    TestNpmBasicTaskRuntimeRetiresRealtimeIdleSessionsOnce();
    TestNpmBasicRealtimeTaskIsolationAndSlowSinkBudget();
    TestNpmBasicTaskRuntimeProcessesAndDrainsOfflineBatch();
    TestNpmBasicTaskRuntimeProcessesImmediateClosedSession();
    TestNpmBasicTaskRuntimeClosesActiveSessionsAcrossFeatureSelections();
    TestNpmBasicTaskRuntimeIsolatesTupleReuseAcrossFeatureSelections();
    TestNpmBasicTaskRuntimeClosesTupleReuseReplacementImmediately();
    TestNpmBasicTaskRuntimeRoutesObservedEntity();
    TestNpmBasicTaskRuntimeUsesExactOfflineInputBudget();
    TestNpmBasicTaskRuntimeRejectsOfflineBatchFailuresAtomically();
    TestNpmBasicTaskRuntimeFlushesOfflineExactlyOnce();
    TestNpmBasicTaskRuntimeEofFailureIsTerminal();
    TestNpmBasicTaskRuntimeConcurrentCancelIsNonBlockingAndStable();
    TestNpmBasicTaskRuntimeCancelKeepsDeliveredOutputAlive();
    TestNpmBasicTaskPreservesLatePacketDiagnostic();
    TestNpmBasicFinalUsesGenerationTime();
    TestNpmBasicOperatorCopiesConfigAndOwnsTasks();
    TestNpmBasicTaskObservingSchemaProbe();
    TestNpmBasicTaskProbeMatchesLegacyAndParametersV1();
    TestNpmBasicTaskOpenProcessAndFlush();
    TestNpmBasicTaskRejectsInvalidCallsAtomically();
    TestNpmBasicTaskReportsConfigurationFailurePaths();
    TestNpmBasicTaskQueriesLabelingProviderConditionally();
#ifdef FLOWSQL_FLOW_LABELING_PLUGIN_PATH
    TestRealFlowLabelingMatcherWithNpmBasicTask();
#endif
    TestNpmBasicTaskMethodPreconditions();
    TestNpmBasicTaskCancelBeforeOpenAndDuringProcess();
    TestNpmBasicTaskFlushAppliesPendingCaptureFacts();
    TestNpmBasicV2PluginExports();
    return 0;
}
