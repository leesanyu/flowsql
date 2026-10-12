// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "packet_decoder.h"

#include <common/network/netbase.h>

#include <arpa/inet.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace flowsql::protocol {
namespace {

constexpr uint32_t kDltEthernet = 1;

bool IsNetworkLayer(eLayer layer) { return layer == eLayer::IPv4 || layer == eLayer::IPv6; }

bool IsTransportLayer(eLayer layer) {
    switch (layer) {
        case eLayer::TCP:
        case eLayer::UDP:
        case eLayer::SCTP:
        case eLayer::DCCP:
        case eLayer::GRE:
        case eLayer::VXLAN:
        case eLayer::VXLAN_GPE:
        case eLayer::GENEVE:
        case eLayer::L2TP:
        case eLayer::GTP:
            return true;
        default:
            return false;
    }
}

uint8_t LayerProtocolNumber(eLayer layer) {
    switch (layer) {
        case eLayer::TCP:
            return ipv4::eNext::TCP;
        case eLayer::UDP:
            return ipv4::eNext::UDP;
        case eLayer::SCTP:
            return ipv4::eNext::SCTP;
        case eLayer::DCCP:
            return ipv4::eNext::DCCP;
        case eLayer::GRE:
            return ipv4::eNext::GRE;
        default:
            return 0;
    }
}

template <typename Header>
const Header* HeaderAt(const protocol::Layers& layers, uint16_t index, const uint8_t* data, size_t size) {
    if (index >= layers.layercount) return nullptr;
    const size_t offset = layers.layers[index].offset;
    return wire::View<Header>(data, size, offset);
}

int FindNetworkLayer(const protocol::Layers& layers, packet::EndpointScope scope) {
    if (scope == packet::EndpointScope::kOutermost) {
        for (uint16_t index = 0; index < layers.layercount; ++index) {
            if (IsNetworkLayer(layers.layers[index].layer)) return index;
        }
    } else {
        for (int index = static_cast<int>(layers.layercount) - 1; index >= 0; --index) {
            if (IsNetworkLayer(layers.layers[index].layer)) return index;
        }
    }
    return -1;
}

int FindTransportLayer(const protocol::Layers& layers, int network_index) {
    if (network_index < 0) return -1;
    for (uint16_t index = static_cast<uint16_t>(network_index + 1); index < layers.layercount; ++index) {
        if (IsNetworkLayer(layers.layers[index].layer)) break;
        if (IsTransportLayer(layers.layers[index].layer)) return index;
    }
    return -1;
}

int FindEthernetLayer(const protocol::Layers& layers, int network_index, packet::EndpointScope scope) {
    if (network_index < 0) {
        if (scope == packet::EndpointScope::kOutermost) {
            for (uint16_t index = 0; index < layers.layercount; ++index) {
                if (layers.layers[index].layer == eLayer::ETHERNET) return index;
            }
        } else {
            for (int index = static_cast<int>(layers.layercount) - 1; index >= 0; --index) {
                if (layers.layers[index].layer == eLayer::ETHERNET) return index;
            }
        }
        return -1;
    }

    int candidate = -1;
    for (int index = 0; index <= network_index; ++index) {
        const eLayer layer = layers.layers[index].layer;
        if (layer == eLayer::ETHERNET) {
            candidate = index;
        } else if (IsNetworkLayer(layer) && index != network_index) {
            // A second IP layer starts a new encapsulation segment. An inner Ethernet
            // can establish a new candidate after this boundary.
            candidate = -1;
        }
    }
    return candidate;
}

void MarkTruncated(packet::PacketLayerFacts* result) {
    if (result->status != packet::LayerStatus::kMalformed &&
        result->status != packet::LayerStatus::kUnsupportedLinkType) {
        result->status = packet::LayerStatus::kTruncated;
    }
}

void MarkMalformed(packet::PacketLayerFacts* result) {
    if (result->status != packet::LayerStatus::kUnsupportedLinkType) result->status = packet::LayerStatus::kMalformed;
}

packet::PacketProtocolInfo UnknownProtocol() { return {packet::ProtocolStatus::kUnknown, 0, 0}; }

bool HasBytes(const packet::PacketView& packet_view, uint32_t offset, size_t length) {
    return offset <= packet_view.bytes.size && length <= packet_view.bytes.size - offset;
}

size_t MinimumHeaderSize(eLayer layer) {
    switch (layer) {
        case eLayer::ETHERNET:
            return sizeof(EthernetHeader);
        case eLayer::VLAN:
            return sizeof(VlanHeader);
        case eLayer::IPv4:
            return sizeof(Ipv4Header);
        case eLayer::IPv6:
            return sizeof(Ipv6Header);
        case eLayer::IPv6_EXT_HOPOPTS:
        case eLayer::IPv6_EXT_ROUTING:
        case eLayer::IPv6_EXT_DSTOPTS:
            return sizeof(Ipv6ExtensionPrefix);
        case eLayer::IPv6_EXT_FRAGMENT:
            return sizeof(Ipv6FragmentHeader);
        case eLayer::IPv6_EXT_ESP:
            return sizeof(Ipv6EspHeader);
        case eLayer::IPv6_EXT_AH:
            return sizeof(Ipv6AuthenticationHeader);
        case eLayer::PPPoE_Session:
            return sizeof(PppoeHeader);
        case eLayer::PPP:
            return sizeof(PppHeader);
        case eLayer::MPLS:
            return sizeof(MplsHeader);
        case eLayer::TCP:
            return sizeof(TcpHeader);
        case eLayer::UDP:
            return sizeof(UdpHeader);
        case eLayer::SCTP:
            return sizeof(SctpHeader);
        case eLayer::GRE:
            return sizeof(GreHeader);
        case eLayer::VXLAN:
        case eLayer::VXLAN_GPE:
            return sizeof(VxlanHeader);
        case eLayer::GENEVE:
            return sizeof(GeneveHeader);
        case eLayer::L2TP:
            return sizeof(L2tpHeader);
        case eLayer::GTP:
            return sizeof(GprsTunnelHeader);
        default:
            return 0;
    }
}

bool BuildProtocolLayers(const packet::PacketView& packet_view, const packet::PacketLayerFacts& source,
                         protocol::Layers* output) {
    if (output == nullptr || source.layer_count == 0 || source.layer_count > protocol::MAX_LAYERS) return false;
    if (source.endpoint_scope != packet::EndpointScope::kInnermost &&
        source.endpoint_scope != packet::EndpointScope::kOutermost) {
        return false;
    }
    if (source.status == packet::LayerStatus::kNotDecoded || source.status == packet::LayerStatus::kMalformed ||
        source.status == packet::LayerStatus::kUnsupportedLinkType) {
        return false;
    }
    if (packet_view.bytes.size != packet_view.meta.captured_len || packet_view.bytes.size == 0 ||
        packet_view.bytes.data == nullptr ||
        packet_view.bytes.size > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
        return false;
    }
    if (packet_view.meta.wire_len != 0 && packet_view.meta.wire_len < packet_view.meta.captured_len) return false;
    if (source.payload_offset > packet_view.meta.captured_len ||
        source.payload_offset > std::numeric_limits<uint16_t>::max()) {
        return false;
    }
    if (source.network_layer_index != packet::kNoLayerIndex && source.network_layer_index >= source.layer_count) {
        return false;
    }
    if (source.transport_layer_index != packet::kNoLayerIndex && source.transport_layer_index >= source.layer_count) {
        return false;
    }
    if (source.network_layer_index != packet::kNoLayerIndex &&
        !IsNetworkLayer(static_cast<eLayer>(source.layers[source.network_layer_index].kind))) {
        return false;
    }
    if (source.transport_layer_index != packet::kNoLayerIndex &&
        !IsTransportLayer(static_cast<eLayer>(source.layers[source.transport_layer_index].kind))) {
        return false;
    }

    uint16_t top_index = source.transport_layer_index;
    if (top_index == packet::kNoLayerIndex) {
        if (source.network_layer_index == packet::kNoLayerIndex) {
            top_index = static_cast<uint16_t>(source.layer_count - 1);
        } else {
            top_index = source.network_layer_index;
            for (uint16_t index = static_cast<uint16_t>(source.network_layer_index + 1); index < source.layer_count;
                 ++index) {
                if (IsNetworkLayer(static_cast<eLayer>(source.layers[index].kind))) break;
                top_index = index;
            }
        }
    }
    for (uint16_t index = 0; index < source.layer_count; ++index) {
        const auto& layer = source.layers[index];
        if (layer.offset > packet_view.meta.captured_len || layer.offset > std::numeric_limits<uint16_t>::max()) {
            return false;
        }
    }

    *output = protocol::Layers{};
    // The complete path remains in PacketLayerFacts. NPI receives the prefix ending at
    // the selected transport so its legacy Top() API observes the selected context.
    output->layercount = static_cast<uint16_t>(top_index + 1);
    output->payload = static_cast<uint16_t>(source.payload_offset);
    for (uint16_t index = 0; index <= top_index; ++index) {
        output->layers[index].offset = static_cast<uint16_t>(source.layers[index].offset);
        output->layers[index].layer = static_cast<eLayer>(source.layers[index].kind);
    }

    const eLayer top_layer = output->Top();
    if (top_layer == eLayer::NONE) return false;
    const uint32_t top_offset = output->layers[top_index].offset;
    const size_t minimum_header = MinimumHeaderSize(top_layer);
    if (minimum_header != 0 && !HasBytes(packet_view, top_offset, minimum_header)) return false;
    if (source.payload_offset > packet_view.bytes.size) return false;
    return true;
}

}  // namespace

NpiPacketLayerDecoder::NpiPacketLayerDecoder(IProtocol* protocol, int32_t pipeno)
    : protocol_(protocol), pipeno_(pipeno) {}

int NpiPacketLayerDecoder::Decode(const packet::PacketView& packet_view, const packet::LayerDecodeOptions& options,
                                  packet::PacketLayerFacts* output, packet::IPacketAddressSink& addresses) {
    if (!output) return EINVAL;
    packet::PacketLayerFacts result;
    result.endpoint_scope = options.endpoint_scope;
    result.address_state = (options.field_mask & packet::kExtractIp) ? packet::PacketAddressState::kIncomplete
                                                                     : packet::PacketAddressState::kNotRequested;

    if (options.endpoint_scope != packet::EndpointScope::kInnermost &&
        options.endpoint_scope != packet::EndpointScope::kOutermost) {
        result.status = packet::LayerStatus::kMalformed;
        *output = result;
        return 0;
    }

    if (packet_view.bytes.size != packet_view.meta.captured_len ||
        (packet_view.bytes.size != 0 && packet_view.bytes.data == nullptr)) {
        result.status = packet::LayerStatus::kMalformed;
        *output = result;
        return 0;
    }
    if (packet_view.meta.wire_len != 0 && packet_view.meta.wire_len < packet_view.meta.captured_len) {
        result.status = packet::LayerStatus::kMalformed;
        *output = result;
        return 0;
    }
    if (packet_view.bytes.size > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
        result.status = packet::LayerStatus::kMalformed;
        *output = result;
        return 0;
    }
    if (packet_view.meta.link_type != kDltEthernet) {
        result.status = packet::LayerStatus::kUnsupportedLinkType;
        *output = result;
        return 0;
    }
    if (protocol_ == nullptr || packet_view.bytes.size == 0) {
        result.status = packet::LayerStatus::kTruncated;
        *output = result;
        return 0;
    }

    protocol::Layers parsed{};
    const int32_t decoded =
        protocol_->Layer(pipeno_, packet_view.bytes.data, static_cast<int32_t>(packet_view.bytes.size), &parsed);
    if (decoded < 0 || decoded > MAX_LAYERS || parsed.layercount > MAX_LAYERS) {
        result.status = packet::LayerStatus::kMalformed;
        *output = result;
        return 0;
    }

    result.layer_count = static_cast<uint8_t>(std::min<uint16_t>(parsed.layercount, packet::kMaxLayerDepth));
    for (uint8_t index = 0; index < result.layer_count; ++index) {
        result.layers[index].kind = static_cast<uint16_t>(parsed.layers[index].layer);
        result.layers[index].offset = parsed.layers[index].offset;
        if (result.layers[index].offset > packet_view.meta.captured_len) MarkTruncated(&result);
        const size_t minimum_header = MinimumHeaderSize(parsed.layers[index].layer);
        if (minimum_header != 0 && !HasBytes(packet_view, result.layers[index].offset, minimum_header)) {
            MarkTruncated(&result);
        }
    }
    result.payload_offset = parsed.payload;
    if (result.payload_offset > packet_view.meta.captured_len) MarkTruncated(&result);
    if (packet_view.meta.wire_len > packet_view.meta.captured_len) MarkTruncated(&result);
    if (result.status == packet::LayerStatus::kNotDecoded) result.status = packet::LayerStatus::kDecoded;

    const int network_index = FindNetworkLayer(parsed, options.endpoint_scope);
    const int transport_index = FindTransportLayer(parsed, network_index);
    const int ethernet_index = FindEthernetLayer(parsed, network_index, options.endpoint_scope);
    result.network_layer_index = network_index < 0 ? packet::kNoLayerIndex : static_cast<uint8_t>(network_index);
    result.transport_layer_index = transport_index < 0 ? packet::kNoLayerIndex : static_cast<uint8_t>(transport_index);

    if (options.field_mask & packet::kExtractMac) {
        if (ethernet_index >= 0) {
            const auto* ethernet = HeaderAt<EthernetHeader>(parsed, static_cast<uint16_t>(ethernet_index),
                                                            packet_view.bytes.data, packet_view.bytes.size);
            if (ethernet) {
                result.src_mac = network::FromWire(ethernet->s_addr);
                result.dst_mac = network::FromWire(ethernet->d_addr);
                result.src_mac_valid = true;
                result.dst_mac_valid = true;
            } else {
                MarkTruncated(&result);
            }
        }
    }

    const eLayer network_layer = network_index < 0 ? eLayer::NONE : parsed.layers[network_index].layer;
    if ((options.field_mask & packet::kExtractIp) && network_index < 0 &&
        result.status == packet::LayerStatus::kDecoded)
        result.address_state = packet::PacketAddressState::kNotPresent;
    if (network_layer == eLayer::IPv4) {
        const auto* header = HeaderAt<Ipv4Header>(parsed, static_cast<uint16_t>(network_index), packet_view.bytes.data,
                                                  packet_view.bytes.size);
        if (!header) {
            MarkTruncated(&result);
        } else if (header->version != 4 || header->ihl < 5) {
            MarkMalformed(&result);
        } else if (parsed.layers[network_index].offset + (static_cast<size_t>(header->ihl) << 2) >
                   packet_view.bytes.size) {
            MarkTruncated(&result);
        } else {
            result.transport_protocol = header->protocol;
            if (options.field_mask & packet::kExtractIp) {
                const packet::PacketIpEndpoints4 endpoints{network::FromWire(header->src_addr),
                                                           network::FromWire(header->dst_addr)};
                const int rc = addresses.Set(endpoints);
                if (rc != 0) return rc;
                result.address_state = packet::PacketAddressState::kAvailable;
            }
        }
    } else if (network_layer == eLayer::IPv6) {
        const auto* header = HeaderAt<Ipv6Header>(parsed, static_cast<uint16_t>(network_index), packet_view.bytes.data,
                                                  packet_view.bytes.size);
        if (!header) {
            MarkTruncated(&result);
        } else if (header->version != 6) {
            MarkMalformed(&result);
        } else {
            result.transport_protocol = header->protocol;
            if (transport_index >= 0) {
                const uint8_t mapped = LayerProtocolNumber(parsed.layers[transport_index].layer);
                if (mapped != 0) result.transport_protocol = mapped;
            }
            if (options.field_mask & packet::kExtractIp) {
                const packet::PacketIpEndpoints6 endpoints{network::FromWire(header->src_addr),
                                                           network::FromWire(header->dst_addr)};
                const int rc = addresses.Set(endpoints);
                if (rc != 0) return rc;
                result.address_state = packet::PacketAddressState::kAvailable;
            }
        }
    }

    if (transport_index >= 0) {
        const eLayer transport_layer = parsed.layers[transport_index].layer;
        if (result.transport_protocol == 0) result.transport_protocol = LayerProtocolNumber(transport_layer);
        if (options.field_mask & packet::kExtractPort) {
            if (transport_layer == eLayer::TCP) {
                const auto* header = HeaderAt<TcpHeader>(parsed, static_cast<uint16_t>(transport_index),
                                                         packet_view.bytes.data, packet_view.bytes.size);
                if (!header) {
                    MarkTruncated(&result);
                } else if (header->offset < 5) {
                    MarkMalformed(&result);
                } else if (parsed.layers[transport_index].offset + (static_cast<size_t>(header->offset) << 2) >
                           packet_view.bytes.size) {
                    MarkTruncated(&result);
                } else {
                    result.src_port = n2h16(header->src_port);
                    result.dst_port = n2h16(header->dst_port);
                    result.ports_valid = 1;
                }
            } else if (transport_layer == eLayer::UDP) {
                const auto* header = HeaderAt<UdpHeader>(parsed, static_cast<uint16_t>(transport_index),
                                                         packet_view.bytes.data, packet_view.bytes.size);
                if (!header) {
                    MarkTruncated(&result);
                } else if (n2h16(header->length) < sizeof(UdpHeader)) {
                    MarkMalformed(&result);
                } else {
                    result.src_port = n2h16(header->src_port);
                    result.dst_port = n2h16(header->dst_port);
                    result.ports_valid = 1;
                }
            } else if (transport_layer == eLayer::SCTP) {
                const auto* header = HeaderAt<SctpHeader>(parsed, static_cast<uint16_t>(transport_index),
                                                          packet_view.bytes.data, packet_view.bytes.size);
                if (!header) {
                    MarkTruncated(&result);
                } else {
                    result.src_port = n2h16(header->src_port);
                    result.dst_port = n2h16(header->dst_port);
                    result.ports_valid = 1;
                }
            }
        }
        if (transport_index + 1 < parsed.layercount) {
            result.payload_offset = parsed.layers[transport_index + 1].offset;
        } else {
            result.payload_offset = parsed.payload;
        }
        if (result.payload_offset > packet_view.meta.captured_len) MarkTruncated(&result);
    } else if (network_index >= 0 && network_index + 1 < parsed.layercount) {
        result.payload_offset = parsed.layers[network_index + 1].offset;
        if (result.payload_offset > packet_view.meta.captured_len) MarkTruncated(&result);
    }
    for (uint8_t index = 0; index < result.layer_count; ++index) {
        if (result.layers[index].offset > packet_view.meta.captured_len) {
            result.layers[index].offset = packet_view.meta.captured_len;
        }
    }
    if (result.payload_offset > packet_view.meta.captured_len) {
        result.payload_offset = packet_view.meta.captured_len;
    }
    *output = result;
    return 0;
}

NpiPacketProtocolIdentifier::NpiPacketProtocolIdentifier(IProtocol* protocol, int32_t pipeno)
    : protocol_(protocol), pipeno_(pipeno) {}

packet::PacketProtocolInfo NpiPacketProtocolIdentifier::Identify(const packet::PacketView& packet_view,
                                                                 const packet::PacketLayerFacts& layer) {
    if (protocol_ == nullptr) return UnknownProtocol();

    protocol::Layers npi_layers{};
    if (!BuildProtocolLayers(packet_view, layer, &npi_layers)) return UnknownProtocol();

    const auto identified =
        protocol_->Identify(pipeno_, packet_view.bytes.data, static_cast<int32_t>(packet_view.bytes.size), &npi_layers);
    if (identified.id == 0) return UnknownProtocol();
    return {packet::ProtocolStatus::kIdentified, identified.id, identified.subid};
}

}  // namespace flowsql::protocol
