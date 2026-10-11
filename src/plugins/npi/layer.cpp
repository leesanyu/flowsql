// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "layer.h"
#include <common/network/netbase.h>
#include <algorithm>

namespace flowsql {
namespace protocol {

namespace {
Delamination::Result ethernet_delamination(eLayer /* below */, const uint8_t* data, int32_t size,
                                           uint16_t base_length) {
    Delamination::Result res;
    res.dword = INVALID;
    if (size >= base_length) {
        res.offset = base_length;

        const auto* ethernet = reinterpret_cast<const EthernetHeader*>(data);
        switch (n2h16(ethernet->ether_type)) {
            case ethernet::eNext::PPPOE_SESSION:
                res.next = eLayer::PPPoE_Session;
                break;
            case ethernet::eNext::IPv4:
                res.next = eLayer::IPv4;
                break;
            case ethernet::eNext::VLAN:
            case ethernet::eNext::QINQ:
                res.next = eLayer::VLAN;
                break;
            case ethernet::eNext::IPv6:
                res.next = eLayer::IPv6;
                break;
            case ethernet::eNext::MPLS:
                res.next = eLayer::MPLS;
                break;
            default:
                res.next = eLayer::NONE;
                break;
        }
    }
    return res;
}

Delamination::Result vlan_delamination(eLayer /* below */, const uint8_t* data, int32_t size, uint16_t base_length) {
    Delamination::Result res;
    res.dword = INVALID;
    if (size >= base_length) {
        res.offset = base_length;

        const auto* vlan = reinterpret_cast<const VlanHeader*>(data);
        switch (n2h16(vlan->ether_type)) {
            case ethernet::eNext::IPv4:
                res.next = eLayer::IPv4;
                break;
            case ethernet::eNext::VLAN:
            case ethernet::eNext::QINQ:
                res.next = eLayer::VLAN;
                break;
            case ethernet::eNext::IPv6:
                res.next = eLayer::IPv6;
                break;
            case ethernet::eNext::MPLS:
                res.next = eLayer::MPLS;
                break;
            default:
                res.next = eLayer::NONE;
                break;
        }
    }
    return res;
}

Delamination::Result ipv4_delamination(eLayer /* below */, const uint8_t* data, int32_t size, uint16_t base_length) {
    Delamination::Result res;
    res.dword = INVALID;
    if (size >= base_length) {
        const auto* header = reinterpret_cast<const Ipv4Header*>(data);
        if (header->version != 4 || header->ihl < 5) return res;
        res.offset = header->ihl * 4;
        if (res.offset > size) return res;
        if ((n2h16(header->fragment_offset) & 0x1fff) != 0) return res;
        switch (header->protocol) {
            case ipv4::eNext::TCP:
                res.next = eLayer::TCP;
                break;
            case ipv4::eNext::UDP:
                res.next = eLayer::UDP;
                break;
            case ipv4::eNext::SCTP:
                res.next = eLayer::SCTP;
                break;
            case ipv4::eNext::GRE:
                res.next = eLayer::GRE;
                break;
            case ipv4::eNext::IPv4:
                res.next = eLayer::IPv4;
                break;
            case ipv4::eNext::IPv6:
                res.next = eLayer::IPv6;
                break;
            case ipv4::eNext::MPLS:
                res.next = eLayer::MPLS;
                break;
            default:
                res.next = eLayer::NONE;
                break;
        }
    }
    return res;
}

#define IPV6_NEXT_LAYER(level, next)           \
    switch (next) {                            \
        case ipv6::eNext::TCP:                 \
            level = eLayer::TCP;               \
            break;                             \
        case ipv6::eNext::UDP:                 \
            level = eLayer::UDP;               \
            break;                             \
        case ipv6::eNext::SCTP:                \
            level = eLayer::SCTP;              \
            break;                             \
        case ipv6::eNext::GRE:                 \
            level = eLayer::GRE;               \
            break;                             \
        case ipv6::eNext::MPLS:                \
            level = eLayer::MPLS;              \
            break;                             \
        case ipv6::eNext::IPv4:                \
            level = eLayer::IPv4;              \
            break;                             \
        case ipv6::eNext::IPv6:                \
            level = eLayer::IPv6;              \
            break;                             \
        case ipv6::eNext::IPv6_EXT_HOPOPTS:    \
            level = eLayer::IPv6_EXT_HOPOPTS;  \
            break;                             \
        case ipv6::eNext::IPv6_EXT_ROUTING:    \
            level = eLayer::IPv6_EXT_ROUTING;  \
            break;                             \
        case ipv6::eNext::IPv6_EXT_FRAGMENT:   \
            level = eLayer::IPv6_EXT_FRAGMENT; \
            break;                             \
        case ipv6::eNext::IPv6_EXT_ESP:        \
            level = eLayer::IPv6_EXT_ESP;      \
            break;                             \
        case ipv6::eNext::IPv6_EXT_AH:         \
            level = eLayer::IPv6_EXT_AH;       \
            break;                             \
        case ipv6::eNext::IPv6_EXT_DSTOPTS:    \
            level = eLayer::IPv6_EXT_DSTOPTS;  \
            break;                             \
        default:                               \
            level = eLayer::NONE;              \
            break;                             \
    }

Delamination::Result ipv6_delamination(eLayer /* below */, const uint8_t* data, int32_t size, uint16_t base_length) {
    Delamination::Result res;
    res.dword = INVALID;
    if (size >= base_length) {
        const auto* header = reinterpret_cast<const Ipv6Header*>(data);
        if (header->version != 6) return res;
        res.offset = base_length;
        IPV6_NEXT_LAYER(res.next, header->protocol);
    }
    return res;
}

Delamination::Result ipv6_hopopts_delamination(eLayer /* below */, const uint8_t* data, int32_t size,
                                               uint16_t base_length) {
    Delamination::Result res;
    res.dword = INVALID;
    if (size >= base_length) {
        const auto* header = reinterpret_cast<const Ipv6ExtensionPrefix*>(data);
        res.offset = (header->extlen + 1) * 8;
        if (res.offset > size) return res;
        IPV6_NEXT_LAYER(res.next, header->protocol);
    }
    return res;
}

Delamination::Result ipv6_routing_delamination(eLayer /* below */, const uint8_t* data, int32_t size,
                                               uint16_t base_length) {
    Delamination::Result res;
    res.dword = INVALID;
    if (size >= base_length) {
        const auto* header = reinterpret_cast<const Ipv6ExtensionPrefix*>(data);
        res.offset = (header->extlen + 1) * 8;
        if (res.offset > size) return res;
        IPV6_NEXT_LAYER(res.next, header->protocol);
    }
    return res;
}

Delamination::Result ipv6_fragment_delamination(eLayer /* below */, const uint8_t* data, int32_t size,
                                                uint16_t base_length) {
    Delamination::Result res;
    res.dword = INVALID;
    if (size >= base_length) {
        res.offset = base_length;
        const auto* header = reinterpret_cast<const Ipv6FragmentHeader*>(data);
        if ((n2h16(header->fragment) & 0xfff8) == 0) {
            // Only the first fragment has upper layer header
            IPV6_NEXT_LAYER(res.next, header->protocol);
        } else {
            // TODO
            res.next = eLayer::NONE;
        }
    }
    return res;
}

Delamination::Result ipv6_esp_delamination(eLayer /* below */, const uint8_t* data, int32_t size,
                                           uint16_t base_length) {
    Delamination::Result res;
    res.dword = INVALID;
    // TODO
    return res;
}

Delamination::Result ipv6_ah_delamination(eLayer /* below */, const uint8_t* data, int32_t size, uint16_t base_length) {
    Delamination::Result res;
    res.dword = INVALID;
    if (size >= base_length) {
        const auto* header = reinterpret_cast<const Ipv6AuthenticationHeader*>(data);
        res.offset = (header->extlen + 2) * 4;
        if (res.offset < sizeof(Ipv6AuthenticationHeader)) {
            res.dword = INVALID;
            return res;
        }
        if (res.offset > size) return res;
        IPV6_NEXT_LAYER(res.next, header->protocol);
    }
    return res;
}

Delamination::Result ipv6_dstopts_delamination(eLayer /* below */, const uint8_t* data, int32_t size,
                                               uint16_t base_length) {
    Delamination::Result res;
    res.dword = INVALID;
    if (size >= base_length) {
        const auto* header = reinterpret_cast<const Ipv6ExtensionPrefix*>(data);
        res.offset = (header->extlen + 1) * 8;
        if (res.offset > size) return res;
        IPV6_NEXT_LAYER(res.next, header->protocol);
    }
    return res;
}

Delamination::Result vxlan_delamination(eLayer /* below */, const uint8_t* data, int32_t size, uint16_t base_length) {
    Delamination::Result res;
    res.dword = INVALID;
    if (size >= base_length) {
        res.offset = base_length;
        const auto* header = reinterpret_cast<const VxlanHeader*>(data);
        if (!header->I || header->ver != 0) {
            res.dword = INVALID;
            return res;
        }
        if (!header->P) {
            res.next = eLayer::ETHERNET;
            return res;
        }
        switch (header->proto) {
            case vxlan::eNext::IPv4:
                res.next = eLayer::IPv4;
                break;
            case vxlan::eNext::ETHERNET:
                res.next = eLayer::ETHERNET;
                break;
            case vxlan::eNext::IPv6:
                res.next = eLayer::IPv6;
                break;
            case vxlan::eNext::MPLS:
                res.next = eLayer::MPLS;
                break;
            default:
                res.next = eLayer::NONE;
                break;
        }
    }
    return res;
}

Delamination::Result gre_delamination(eLayer /* below */, const uint8_t* data, int32_t size, uint16_t base_length) {
    Delamination::Result res;
    res.dword = INVALID;
    if (size >= base_length) {
        const auto* header = reinterpret_cast<const GreHeader*>(data);
        res.offset = base_length;
        if (header->C || header->R) res.offset += 4;
        if (header->K) res.offset += 4;
        if (header->S) res.offset += 4;
        if (header->A) res.offset += 4;
        if (res.offset > size) return res;
        if (header->R) {
            for (;;) {
                const auto* route = wire::View<SourceRouteEntry>(data, size, res.offset);
                if (!route) {
                    res.offset = std::min<size_t>(UINT16_MAX, res.offset + sizeof(SourceRouteEntry));
                    return res;
                }
                if (res.offset + sizeof(SourceRouteEntry) + route->length > UINT16_MAX) {
                    res.dword = INVALID;
                    return res;
                }
                res.offset += sizeof(SourceRouteEntry);
                if (route->length > size - res.offset) {
                    res.offset += route->length;
                    return res;
                }
                res.offset += route->length;
                if (route->addr_family == 0 && route->length == 0) break;
            }
        }

        switch (n2h16(header->proto)) {
            case gre::eNext::IPv4:
                res.next = eLayer::IPv4;
                break;
            case gre::eNext::ETHERNET:
                res.next = eLayer::ETHERNET;
                break;
            case gre::eNext::IPv6:
                res.next = eLayer::IPv6;
                break;
            case gre::eNext::MPLS:
                res.next = eLayer::MPLS;
                break;
            case gre::eNext::PPP:
                res.next = eLayer::PPP;
                break;
            default:
                res.next = eLayer::NONE;
                break;
        }
    }
    return res;
}

Delamination::Result geneve_delamination(eLayer /* below */, const uint8_t* data, int32_t size, uint16_t base_length) {
    Delamination::Result res;
    res.dword = INVALID;
    if (size >= base_length) {
        const auto* header = reinterpret_cast<const GeneveHeader*>(data);
        if (header->version != 0) return res;
        res.offset = base_length + header->opt_len * 4;
        if (res.offset > size) return res;
        switch (n2h16(header->proto)) {
            case geneve::eNext::IPv4:
                res.next = eLayer::IPv4;
                break;
            case geneve::eNext::ETHERNET:
                res.next = eLayer::ETHERNET;
                break;
            case geneve::eNext::IPv6:
                res.next = eLayer::IPv6;
                break;
            case geneve::eNext::MPLS:
                res.next = eLayer::MPLS;
                break;
            case geneve::eNext::PPP:
                res.next = eLayer::PPP;
                break;
            default:
                res.next = eLayer::NONE;
                break;
        }
    }
    return res;
}

Delamination::Result udp_delamination(eLayer /* below */, const uint8_t* data, int32_t size, uint16_t base_length) {
    Delamination::Result res;
    res.dword = INVALID;
    if (size >= base_length) {
        res.offset = base_length;
        const auto* udp = reinterpret_cast<const UdpHeader*>(data);
        switch (n2h16(udp->dst_port)) {
            case udp::eNext::L2TP:
                res.next = eLayer::L2TP;
                break;
            case udp::eNext::GRE:
                res.next = eLayer::GRE;
                break;
            case udp::eNext::VXLAN:
                res.next = eLayer::VXLAN;
                break;
            case udp::eNext::VXLAN_GPE:
                res.next = eLayer::VXLAN_GPE;
                break;
            case udp::eNext::GENEVE:
                res.next = eLayer::GENEVE;
                break;
            case udp::eNext::IPv6:
                res.next = eLayer::IPv6;
                break;
            case udp::GTPU:
                res.next = eLayer::GTP;
                break;
            default:
                break;
        }

        switch (n2h16(udp->src_port)) {
            case udp::eNext::L2TP:
                res.next = eLayer::L2TP;
                break;
            case udp::eNext::GRE:
                res.next = eLayer::GRE;
                break;
            case udp::eNext::VXLAN:
                res.next = eLayer::VXLAN;
                break;
            case udp::eNext::VXLAN_GPE:
                res.next = eLayer::VXLAN_GPE;
                break;
            case udp::eNext::GENEVE:
                res.next = eLayer::GENEVE;
                break;
            case udp::eNext::IPv6:
                res.next = eLayer::IPv6;
                break;
            case udp::GTPU:
                res.next = eLayer::GTP;
                break;
            default:
                break;
        }
    }
    return res;
}

Delamination::Result tcp_delamination(eLayer /* below */, const uint8_t* data, int32_t size, uint16_t base_length) {
    Delamination::Result res;
    res.dword = INVALID;
    if (size >= base_length) {
        const auto* header = reinterpret_cast<const TcpHeader*>(data);
        if (header->offset < 5) return res;
        res.offset = header->offset * 4;
        res.next = eLayer::NONE;
    }
    return res;
}

Delamination::Result sctp_delamination(eLayer /* below */, const uint8_t* data, int32_t size, uint16_t base_length) {
    Delamination::Result res;
    res.dword = INVALID;
    if (size >= base_length) {
        res.next = eLayer::NONE;

        const auto* header = reinterpret_cast<const SctpHeader*>(data);
        switch (header->chunk.type) {
            case sctp::eChunk::DATA:
                res.offset = 28;
                break;
            default:
                res.offset = 12 + n2h16(header->chunk.length);
                break;
        }
    }

    return res;
}

Delamination::Result l2tp_delamination(eLayer /* below */, const uint8_t* data, int32_t size, uint16_t base_length) {
    Delamination::Result res;
    res.dword = INVALID;
    if (size < base_length) return res;
    const auto* header = reinterpret_cast<const L2tpHeader*>(data);
    if (header->ver != 2 || header->T) return res;
    res.offset = base_length + (header->L ? 2 : 0);
    if (header->L && n2h16(header->tunnel_or_length) < res.offset) return res;
    if (header->S) res.offset += sizeof(L2tpSequenceHeader);
    if (header->O) {
        const auto* padding = wire::View<L2tpOffsetHeader>(data, size, res.offset);
        if (!padding) {
            res.offset += sizeof(L2tpOffsetHeader);
            return res;
        }
        const size_t next = res.offset + sizeof(L2tpOffsetHeader) + n2h16(padding->size);
        if (next > UINT16_MAX) {
            res.dword = INVALID;
            return res;
        }
        res.offset = next;
    }
    if (res.offset <= size) res.next = eLayer::PPP;
    return res;
}

Delamination::Result pppoe_session_delamination(eLayer /* below */, const uint8_t* data, int32_t size,
                                                uint16_t base_length) {
    Delamination::Result res;
    res.next = eLayer::PPP;
    res.offset = base_length;
    return res;
}

Delamination::Result ppp_delamination(eLayer below, const uint8_t* data, int32_t size, uint16_t base_length) {
    Delamination::Result res;
    res.dword = INVALID;
    uint16_t nextpro = 0;
    if (below == eLayer::PPPoE_Session) base_length = sizeof(PppProtocolHeader);
    if (size >= base_length) {
        if (eLayer::PPPoE_Session == below) {
            res.offset = 2;
            const auto* ppp = reinterpret_cast<const PppProtocolHeader*>(data);
            nextpro = n2h16(ppp->protocol);
        } else {
            res.offset = base_length;
            const auto* ppp = reinterpret_cast<const PppHeader*>(data);
            nextpro = n2h16(ppp->protocol);
        }

        switch (nextpro) {
            case ppp::eNext::IPv4:
                res.next = eLayer::IPv4;
                break;
            case ppp::eNext::IPv6:
                res.next = eLayer::IPv6;
                break;
            case ppp::eNext::MPLS:
                res.next = eLayer::MPLS;
                break;
            case ppp::eNext::MPLSM:
                res.next = eLayer::MPLS;
                break;
            default:
                res.next = eLayer::NONE;
                break;
        }
    }
    return res;
}

Delamination::Result mpls_delamination(eLayer /* below */, const uint8_t* data, int32_t size, uint16_t base_length) {
    Delamination::Result res;
    res.dword = INVALID;
    res.offset = base_length;
    if (size < base_length) return res;
    const auto* mpls = reinterpret_cast<const MplsHeader*>(data);
    if (mpls->bos) {
        // detect forword one byte : ipver
        if (res.offset < size) {
            const auto* header = reinterpret_cast<const IpVersionHeader*>(data + res.offset);
            switch (header->version) {
                case mpls::eIpVer::IPv4:
                    res.next = eLayer::IPv4;
                    break;
                case mpls::eIpVer::IPv6:
                    res.next = eLayer::IPv6;
                    break;
                default:
                    break;
            }
        } else {
            // packet error
        }
    } else {
        res.next = eLayer::MPLS;
    }

    return res;
}

Delamination::Result gtp_delamination(eLayer /* below */, const uint8_t* data, int32_t size, uint16_t base_length) {
    Delamination::Result res;
    res.dword = INVALID;
    res.offset = base_length;
    if (size < base_length) return res;
    const auto* header = reinterpret_cast<const GprsTunnelHeader*>(data);
    if (header->ver != 1 || !header->proto_type || header->msg_type != 255) return res;
    if (header->sequence || header->npdu_number || header->next_extension_header) {
        const auto* optional = wire::View<GtpOptionalHeader>(data, size, res.offset);
        if (!optional) {
            res.offset += sizeof(GtpOptionalHeader);
            return res;
        }
        res.offset += sizeof(GtpOptionalHeader);
        uint8_t next_extension = header->next_extension_header ? optional->next_extension : 0;
        while (next_extension != 0) {
            const auto* extension = wire::View<GtpExtensionPrefix>(data, size, res.offset);
            if (!extension) {
                ++res.offset;
                return res;
            }
            const size_t length = extension->length * 4;
            if (length == 0) {
                res.dword = INVALID;
                return res;
            }
            const size_t next = res.offset + length;
            if (next > UINT16_MAX) {
                res.dword = INVALID;
                return res;
            }
            if (next > static_cast<size_t>(size)) {
                res.offset = next;
                return res;
            }
            // The final octet of each variable-length extension selects its successor.
            next_extension = data[next - 1];
            res.offset = next;
        }
    }
    if (res.offset < size) {
        const auto* inner = reinterpret_cast<const IpVersionHeader*>(data + res.offset);
        if (inner->version == 4) res.next = eLayer::IPv4;
        if (inner->version == 6) res.next = eLayer::IPv6;
    }

    return res;
}

Delamination::Result tpkt_delamination(eLayer /* below */, const uint8_t* data, int32_t size, uint16_t base_length) {
    Delamination::Result res;
    res.offset = base_length;
    res.next = eLayer::NONE;
    if (res.offset < size) {
        res.next = eLayer::COTP;
    }

    return res;
}

Delamination::Result cotp_delamination(eLayer /* below */, const uint8_t* data, int32_t size, uint16_t base_length) {
    Delamination::Result res;
    res.offset = 0;
    res.next = eLayer::NONE;
    if (size >= base_length) {
        const auto* cotp = reinterpret_cast<const CotpHeader*>(data);
        res.offset = cotp->length + 1;
    }
    return res;
}

}  // namespace

NetworkLayer::NetworkLayer() {
    parsers_map_[eLayer::ETHERNET] = new Delamination(sizeof(EthernetHeader), ethernet_delamination);
    parsers_map_[eLayer::VLAN] = new Delamination(sizeof(VlanHeader), vlan_delamination);
    parsers_map_[eLayer::PPPoE_Session] = new Delamination(sizeof(PppoeHeader), pppoe_session_delamination);
    parsers_map_[eLayer::PPP] = new Delamination(sizeof(PppHeader), ppp_delamination);
    parsers_map_[eLayer::MPLS] = new Delamination(sizeof(MplsHeader), mpls_delamination);
    parsers_map_[eLayer::IPv4] = new Delamination(sizeof(Ipv4Header), ipv4_delamination);
    parsers_map_[eLayer::IPv6] = new Delamination(sizeof(Ipv6Header), ipv6_delamination);
    parsers_map_[eLayer::IPv6_EXT_HOPOPTS] = new Delamination(2, ipv6_hopopts_delamination);
    parsers_map_[eLayer::IPv6_EXT_ROUTING] = new Delamination(2, ipv6_routing_delamination);
    parsers_map_[eLayer::IPv6_EXT_FRAGMENT] = new Delamination(8, ipv6_fragment_delamination);
    parsers_map_[eLayer::IPv6_EXT_ESP] = new Delamination(2, ipv6_esp_delamination);
    parsers_map_[eLayer::IPv6_EXT_AH] = new Delamination(sizeof(Ipv6AuthenticationHeader), ipv6_ah_delamination);
    parsers_map_[eLayer::IPv6_EXT_DSTOPTS] = new Delamination(2, ipv6_dstopts_delamination);
    parsers_map_[eLayer::UDP] = new Delamination(sizeof(UdpHeader), udp_delamination);
    parsers_map_[eLayer::TCP] = new Delamination(sizeof(TcpHeader), tcp_delamination);
    parsers_map_[eLayer::SCTP] = new Delamination(sizeof(SctpHeader), sctp_delamination);
    parsers_map_[eLayer::GRE] = new Delamination(sizeof(GreHeader), gre_delamination);
    parsers_map_[eLayer::VXLAN] = new Delamination(sizeof(VxlanHeader), vxlan_delamination);
    parsers_map_[eLayer::VXLAN_GPE] = new Delamination(sizeof(VxlanHeader), vxlan_delamination);
    parsers_map_[eLayer::GENEVE] = new Delamination(sizeof(GeneveHeader), geneve_delamination);
    parsers_map_[eLayer::L2TP] = new Delamination(sizeof(L2tpHeader), l2tp_delamination);
    parsers_map_[eLayer::GTP] = new Delamination(sizeof(GprsTunnelHeader), gtp_delamination);

    // ISO Protocol Family
    // parsers_map_[eLayer::TPKT] = new Delamination(sizeof(TpktHeader), tpkt_delamination);
    // parsers_map_[eLayer::COTP] = new Delamination(sizeof(CotpHeader), cotp_delamination);
}

NetworkLayer::~NetworkLayer() {
    for (auto* parser : parsers_map_) delete parser;
}

int32_t NetworkLayer::Layer(const uint8_t* packet, int32_t packet_size, protocol::Layers* layers) {
    flowsql::eLayer layer = flowsql::eLayer::ETHERNET;
    uint32_t offset = 0;
    int32_t degree = 0;
    eLayer below = eLayer::NONE;
    if (!layers || !packet || packet_size <= 0) return 0;
    while (offset <= UINT16_MAX && offset < static_cast<uint32_t>(packet_size) && degree < MAX_LAYERS &&
           layer != eLayer::NONE) {
        layers->layers[degree].offset = offset;
        layers->layers[degree].layer = layer;
        Delamination* parser = parsers_map_[layer];
        if (parser) {
            auto res = (*parser)(below, packet + offset, packet_size - offset);
            offset += res.offset;
            below = layer;
            layer = res.next;
            ++degree;
        } else {
            break;
        }
    }

    layers->payload = static_cast<uint16_t>(std::min<uint32_t>(offset, UINT16_MAX));
    layers->layercount = degree;
    return degree;
}
}  // namespace protocol
}  // namespace flowsql
