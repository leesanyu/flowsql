// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_session_key.h"

#include <common/network/netbase.h>

#include <arpa/inet.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>

namespace flowsql::npm {
namespace {

bool IsNetworkLayer(uint16_t kind) {
    return kind == static_cast<uint16_t>(eLayer::IPv4) || kind == static_cast<uint16_t>(eLayer::IPv6);
}

bool AddSize(size_t left, size_t right, size_t* result) {
    if (right > std::numeric_limits<size_t>::max() - left) return false;
    *result = left + right;
    return true;
}

template <typename Header>
bool ReadHeader(const packet::PacketView& packet, size_t offset, Header* header) {
    if (offset > packet.bytes.size || sizeof(Header) > packet.bytes.size - offset) return false;
    std::memcpy(header, packet.bytes.data + offset, sizeof(Header));
    return true;
}

NpmSessionPacketError ResolveDomain(const NpmObservationDomainMap& domain_map, uint32_t source_id,
                                    uint64_t* observation_domain_id) {
    const auto error = ResolveNpmObservationDomain(domain_map, source_id, observation_domain_id);
    if (error == NpmObservationDomainError::kNone) return NpmSessionPacketError::kNone;
    if (error == NpmObservationDomainError::kUnknownSourceId) return NpmSessionPacketError::kUnknownSourceId;
    return NpmSessionPacketError::kInvalidObservationDomainMap;
}

NpmSessionPacketError ValidateLayerStatus(packet::LayerStatus status) {
    switch (status) {
        case packet::LayerStatus::kDecoded:
        case packet::LayerStatus::kTruncated:
            return NpmSessionPacketError::kNone;
        case packet::LayerStatus::kMalformed:
            return NpmSessionPacketError::kMalformedLayer;
        case packet::LayerStatus::kNotDecoded:
        case packet::LayerStatus::kUnsupportedLinkType:
            return NpmSessionPacketError::kLayerUnavailable;
    }
    return NpmSessionPacketError::kLayerUnavailable;
}

NpmSessionPacketError ValidateIpv6Fragments(const packet::PacketView& packet, const packet::PacketLayerInfo& layer,
                                            uint8_t network_index) {
    for (uint8_t index = static_cast<uint8_t>(network_index + 1); index < layer.layer_count; ++index) {
        const uint16_t kind = layer.layers[index].kind;
        if (IsNetworkLayer(kind)) break;
        if (kind != static_cast<uint16_t>(eLayer::IPv6_EXT_FRAGMENT)) continue;
        const size_t offset = layer.layers[index].offset;
        if (offset > packet.bytes.size || 8 > packet.bytes.size - offset) {
            return NpmSessionPacketError::kIncompleteTransportHeader;
        }
        uint16_t fragment_field = 0;
        std::memcpy(&fragment_field, packet.bytes.data + offset + 2, sizeof(fragment_field));
        if ((ntohs(fragment_field) & 0xfff8u) != 0) return NpmSessionPacketError::kNonInitialFragment;
    }
    return NpmSessionPacketError::kNone;
}

int CompareEndpointAddresses(const NpmEndpoint& left, const NpmEndpoint& right, packet::AddressFamily family) {
    if (family == packet::AddressFamily::kIPv4) {
        const auto& left_address = std::get<packet::IPv4Address>(left.ip);
        const auto& right_address = std::get<packet::IPv4Address>(right.ip);
        return std::memcmp(left_address.bytes, right_address.bytes, sizeof(left_address.bytes));
    }
    const auto& left_address = std::get<packet::IPv6Address>(left.ip);
    const auto& right_address = std::get<packet::IPv6Address>(right.ip);
    return std::memcmp(left_address.bytes, right_address.bytes, sizeof(left_address.bytes));
}

bool EndpointComesFirst(const NpmEndpoint& left, const NpmEndpoint& right, packet::AddressFamily family) {
    const int address_order = CompareEndpointAddresses(left, right, family);
    return address_order < 0 || (address_order == 0 && left.port <= right.port);
}

void PopulateLabelEndpoint(const packet::MacAddress& mac, const packet::IpAddress& ip, uint16_t port,
                           FlowLabelEndpointFactsV1* output) {
    if (mac.valid) {
        std::memcpy(output->mac, mac.value.bytes, sizeof(output->mac));
        output->mac_valid = 1;
    }
    if (const auto* ipv4 = std::get_if<packet::IPv4Address>(&ip)) {
        std::memcpy(output->ip, ipv4->bytes, sizeof(ipv4->bytes));
        output->ip_valid = 1;
    } else if (const auto* ipv6 = std::get_if<packet::IPv6Address>(&ip)) {
        std::memcpy(output->ip, ipv6->bytes, sizeof(ipv6->bytes));
        output->ip_valid = 1;
    }
    output->port = port;
    output->port_valid = 1;
}

uint16_t VlanTpid(const packet::PacketView& packet, const packet::PacketLayerInfo& layer, uint8_t vlan_index) {
    if (vlan_index == 0) return 0;
    const auto& previous = layer.layers[vlan_index - 1];
    if (previous.kind == static_cast<uint16_t>(eLayer::ETHERNET)) {
        EthernetHeader ethernet;
        return ReadHeader(packet, previous.offset, &ethernet) ? ntohs(ethernet.ether_type) : 0;
    }
    if (previous.kind == static_cast<uint16_t>(eLayer::VLAN)) {
        VlanHeader vlan;
        return ReadHeader(packet, previous.offset, &vlan) ? ntohs(vlan.ether_type) : 0;
    }
    return 0;
}

void PopulateLabelVlans(const packet::PacketView& packet, const packet::PacketLayerInfo& layer, uint8_t network_index,
                        FlowLabelFactsV1* output) {
    size_t fact_index = 0;
    for (uint8_t index = 0; index < network_index && fact_index < 2; ++index) {
        if (layer.layers[index].kind != static_cast<uint16_t>(eLayer::VLAN)) continue;
        VlanHeader vlan;
        const uint16_t tpid = VlanTpid(packet, layer, index);
        if (tpid == 0 || !ReadHeader(packet, layer.layers[index].offset, &vlan)) continue;
        output->vlan[fact_index].tpid = tpid;
        output->vlan[fact_index].vid = ntohs(vlan.vlan_tci) & 0x0fffu;
        output->vlan[fact_index].valid = 1;
        ++fact_index;
    }
}

}  // namespace

NpmSessionPacketError BuildNpmSessionPacketBinding(const NpmObservationDomainMap& domain_map,
                                                   const packet::PacketView& packet,
                                                   const packet::PacketLayerInfo& layer,
                                                   NpmSessionPacketBinding* output) {
    if (!output) return NpmSessionPacketError::kNullOutput;

    uint64_t observation_domain_id = 0;
    const auto domain_error = ResolveDomain(domain_map, packet.meta.source_id, &observation_domain_id);
    if (domain_error != NpmSessionPacketError::kNone) return domain_error;
    if (packet.bytes.size != packet.meta.captured_len || packet.bytes.size == 0 || packet.bytes.data == nullptr ||
        (packet.meta.wire_len != 0 && packet.meta.wire_len < packet.meta.captured_len)) {
        return NpmSessionPacketError::kInvalidPacketBounds;
    }
    const auto status_error = ValidateLayerStatus(layer.status);
    if (status_error != NpmSessionPacketError::kNone) return status_error;
    if (layer.layer_count == 0 || layer.layer_count > packet::kMaxLayerDepth ||
        layer.network_layer_index >= layer.layer_count ||
        (layer.endpoint_scope != packet::EndpointScope::kInnermost &&
         layer.endpoint_scope != packet::EndpointScope::kOutermost)) {
        return NpmSessionPacketError::kInvalidLayerSelection;
    }

    const uint8_t network_index = layer.network_layer_index;
    const uint16_t network_kind = layer.layers[network_index].kind;
    if (!IsNetworkLayer(network_kind)) return NpmSessionPacketError::kInvalidLayerSelection;
    for (uint8_t index = 0; index < network_index; ++index) {
        if (IsNetworkLayer(layer.layers[index].kind)) return NpmSessionPacketError::kAmbiguousTunnelContext;
    }

    const size_t network_offset = layer.layers[network_index].offset;
    size_t network_end = 0;
    packet::AddressFamily family = packet::AddressFamily::kNone;
    if (network_kind == static_cast<uint16_t>(eLayer::IPv4)) {
        Ipv4Header header;
        if (!ReadHeader(packet, network_offset, &header)) {
            return NpmSessionPacketError::kInvalidPacketBounds;
        }
        if (header.version != 4 || header.ihl < 5) return NpmSessionPacketError::kMalformedLayer;
        const size_t header_size = static_cast<size_t>(header.ihl) * 4;
        if (header_size > packet.bytes.size - network_offset) {
            return NpmSessionPacketError::kInvalidPacketBounds;
        }
        const size_t total_length = ntohs(header.total_length);
        if (total_length < header_size || !AddSize(network_offset, total_length, &network_end)) {
            return NpmSessionPacketError::kInvalidPayloadBounds;
        }
        if ((ntohs(header.fragment_offset) & 0x1fffu) != 0) {
            return NpmSessionPacketError::kNonInitialFragment;
        }
        family = packet::AddressFamily::kIPv4;
    } else {
        Ipv6Header header;
        if (!ReadHeader(packet, network_offset, &header)) {
            return NpmSessionPacketError::kInvalidPacketBounds;
        }
        if (header.version != 6 ||
            !AddSize(network_offset, sizeof(Ipv6Header) + static_cast<size_t>(ntohs(header.payload)), &network_end)) {
            return NpmSessionPacketError::kInvalidPayloadBounds;
        }
        const auto fragment_error = ValidateIpv6Fragments(packet, layer, network_index);
        if (fragment_error != NpmSessionPacketError::kNone) return fragment_error;
        family = packet::AddressFamily::kIPv6;
    }
    if (packet.meta.wire_len != 0 && network_end > packet.meta.wire_len) {
        return NpmSessionPacketError::kInvalidPayloadBounds;
    }

    if (layer.transport_layer_index >= layer.layer_count || layer.transport_layer_index <= network_index) {
        return NpmSessionPacketError::kInvalidLayerSelection;
    }
    const uint8_t transport_index = layer.transport_layer_index;
    for (uint8_t index = static_cast<uint8_t>(network_index + 1); index < transport_index; ++index) {
        if (IsNetworkLayer(layer.layers[index].kind)) return NpmSessionPacketError::kInvalidLayerSelection;
    }
    const uint16_t transport_kind = layer.layers[transport_index].kind;
    const bool is_tcp = transport_kind == static_cast<uint16_t>(eLayer::TCP);
    const bool is_udp = transport_kind == static_cast<uint16_t>(eLayer::UDP);
    const uint8_t expected_protocol = is_tcp ? ipv4::eNext::TCP : is_udp ? ipv4::eNext::UDP : 0;
    if (expected_protocol == 0 || layer.transport_protocol != expected_protocol) {
        return NpmSessionPacketError::kUnsupportedTransportProtocol;
    }
    if (!layer.ports_valid) return NpmSessionPacketError::kIncompleteEndpoint;
    if ((family == packet::AddressFamily::kIPv4 && (!std::holds_alternative<packet::IPv4Address>(layer.src_ip) ||
                                                    !std::holds_alternative<packet::IPv4Address>(layer.dst_ip))) ||
        (family == packet::AddressFamily::kIPv6 && (!std::holds_alternative<packet::IPv6Address>(layer.src_ip) ||
                                                    !std::holds_alternative<packet::IPv6Address>(layer.dst_ip)))) {
        return NpmSessionPacketError::kIncompleteEndpoint;
    }

    const size_t transport_offset = layer.layers[transport_index].offset;
    if (transport_offset < network_offset || transport_offset >= network_end) {
        return NpmSessionPacketError::kInvalidPayloadBounds;
    }
    size_t payload_start = 0;
    size_t payload_end = 0;
    size_t payload_wire_bytes = 0;
    NpmTcpPacketFacts tcp_facts;
    if (is_tcp) {
        TcpHeader header;
        if (!ReadHeader(packet, transport_offset, &header)) {
            return NpmSessionPacketError::kIncompleteTransportHeader;
        }
        if (header.offset < 5) return NpmSessionPacketError::kInvalidPayloadBounds;
        const size_t header_size = static_cast<size_t>(header.offset) * 4;
        if (!AddSize(transport_offset, header_size, &payload_start)) {
            return NpmSessionPacketError::kInvalidPayloadBounds;
        }
        if (payload_start > packet.bytes.size) return NpmSessionPacketError::kIncompleteTransportHeader;
        if (payload_start > network_end) return NpmSessionPacketError::kInvalidPayloadBounds;
        payload_wire_bytes = network_end - payload_start;
        payload_end = std::min(network_end, packet.bytes.size);
        tcp_facts.valid = true;
        tcp_facts.syn = header.flags.flags_bit.syn != 0;
        tcp_facts.ack = header.flags.flags_bit.ack != 0;
        tcp_facts.fin = header.flags.flags_bit.fin != 0;
        tcp_facts.rst = header.flags.flags_bit.rst != 0;
        tcp_facts.sequence = ntohl(header.seq);
        tcp_facts.acknowledgment = ntohl(header.ack);
        tcp_facts.window = ntohs(header.window);
    } else {
        UdpHeader header;
        if (!ReadHeader(packet, transport_offset, &header)) {
            return NpmSessionPacketError::kIncompleteTransportHeader;
        }
        const size_t udp_length = ntohs(header.length);
        size_t udp_end = 0;
        if (udp_length < sizeof(UdpHeader) || !AddSize(transport_offset, udp_length, &udp_end) ||
            udp_end > network_end) {
            return NpmSessionPacketError::kInvalidPayloadBounds;
        }
        payload_start = transport_offset + sizeof(UdpHeader);
        payload_wire_bytes = udp_end - payload_start;
        payload_end = std::min(udp_end, packet.bytes.size);
    }
    if (layer.payload_offset != payload_start || payload_end < payload_start) {
        return NpmSessionPacketError::kInvalidPayloadBounds;
    }

    NpmSessionPacketBinding binding;
    binding.key.input_namespace = domain_map.input_namespace;
    binding.key.observation_domain_id = observation_domain_id;
    binding.key.ip_family = family;
    binding.key.transport_protocol = expected_protocol;
    const NpmEndpoint source{layer.src_ip, layer.src_port};
    const NpmEndpoint destination{layer.dst_ip, layer.dst_port};
    if (EndpointComesFirst(source, destination, family)) {
        binding.key.a = source;
        binding.key.b = destination;
        binding.direction = NpmPacketDirection::kAToB;
    } else {
        binding.key.a = destination;
        binding.key.b = source;
        binding.direction = NpmPacketDirection::kBToA;
    }
    binding.payload = Span<const uint8_t>(packet.bytes.data + payload_start, payload_end - payload_start);
    binding.transport.payload_wire_bytes = static_cast<uint32_t>(payload_wire_bytes);
    binding.transport.payload_captured_bytes = static_cast<uint32_t>(binding.payload.size);
    binding.transport.payload_complete = binding.payload.size == payload_wire_bytes;
    binding.transport.tcp = tcp_facts;
    binding.label_facts.observation_domain_id = observation_domain_id;
    binding.label_facts.ip_family = static_cast<uint8_t>(family);
    binding.label_facts.transport_protocol = expected_protocol;
    binding.label_facts.transport_valid = 1;
    PopulateLabelEndpoint(layer.src_mac, layer.src_ip, layer.src_port, &binding.label_facts.source);
    PopulateLabelEndpoint(layer.dst_mac, layer.dst_ip, layer.dst_port, &binding.label_facts.destination);
    PopulateLabelVlans(packet, layer, network_index, &binding.label_facts);
    *output = std::move(binding);
    return NpmSessionPacketError::kNone;
}

}  // namespace flowsql::npm
