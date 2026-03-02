/*
 * Copyright (C) 2020-06 - flowSQL
 *
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 *
 * Author       : LIHUO
 * Date         : 2021-12-02 16:38:14
 * LastEditors  : LIHUO
 * LastEditTime : 2026-02-25 12:00:00
 */

#include "layer.h"
#include <common/network/netbase.h>

namespace flowsql {
namespace protocol {

namespace {
template <typename Integer>
inline Integer alignment(Integer number, int32_t align) {
    auto padding = number % align;
    if (padding) {
        return number + align - padding;
    }
    return number;
}
}  // namespace
namespace {
Delamination::Result ethernet_delamination(eLayer /* below */, const uint8_t* data, int32_t size,
                                           uint16_t base_length) {
    Delamination::Result res;
    res.dword = INVALID;
    if (size >= base_length) {
        res.offset = base_length;
        EthernetHeader* eth = (EthernetHeader*)data;
        switch (n2h16(eth->ether_type)) {
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
        VlanHeader* vlan = (VlanHeader*)data;
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
        Ipv4Header* ipv4header = (Ipv4Header*)data;
        res.offset = ipv4header->ihl << 2;
        switch (ipv4header->protocol) {
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
        Ipv6Header* ipv6header = (Ipv6Header*)data;
        res.offset = base_length;
        IPV6_NEXT_LAYER(res.next, ipv6header->protocol);
    }
    return res;
}

Delamination::Result ipv6_hopopts_delamination(eLayer /* below */, const uint8_t* data, int32_t size,
                                               uint16_t base_length) {
    Delamination::Result res;
    res.dword = INVALID;
    if (size >= base_length) {
        Ipv6ExtentHeader* exthdr = (Ipv6ExtentHeader*)data;
        res.offset = (exthdr->extlen + 1) * 8;
        IPV6_NEXT_LAYER(res.next, exthdr->protocol);
    }
    return res;
}

Delamination::Result ipv6_routing_delamination(eLayer /* below */, const uint8_t* data, int32_t size,
                                               uint16_t base_length) {
    Delamination::Result res;
    res.dword = INVALID;
    if (size >= base_length) {
        Ipv6ExtentHeader* exthdr = (Ipv6ExtentHeader*)data;
        res.offset = (exthdr->extlen + 1) * 8;
        IPV6_NEXT_LAYER(res.next, exthdr->protocol);
    }
    return res;
}

Delamination::Result ipv6_fragment_delamination(eLayer /* below */, const uint8_t* data, int32_t size,
                                                uint16_t base_length) {
    Delamination::Result res;
    res.dword = INVALID;
    if (size >= base_length) {
        Ipv6ExtentHeader* exthdr = (Ipv6ExtentHeader*)data;
        res.offset = base_length;
        if (0 == exthdr->offset) {
            // Only the first fragment has upper layer header
            IPV6_NEXT_LAYER(res.next, exthdr->protocol);
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
        Ipv6ExtentHeader* exthdr = (Ipv6ExtentHeader*)data;
        res.offset = (exthdr->extlen + 2) * 4;
        IPV6_NEXT_LAYER(res.next, exthdr->protocol);
    }
    return res;
}

Delamination::Result ipv6_dstopts_delamination(eLayer /* below */, const uint8_t* data, int32_t size,
                                               uint16_t base_length) {
    Delamination::Result res;
    res.dword = INVALID;
    if (size >= base_length) {
        Ipv6ExtentHeader* exthdr = (Ipv6ExtentHeader*)data;
        res.offset = (exthdr->extlen + 1) * 8;
        IPV6_NEXT_LAYER(res.next, exthdr->protocol);
    }
    return res;
}

Delamination::Result vxlan_delamination(eLayer /* below */, const uint8_t* data, int32_t size, uint16_t base_length) {
    Delamination::Result res;
    res.dword = INVALID;
    if (size >= base_length) {
        VxlanHeader* vxlan = (VxlanHeader*)data;
        res.offset = base_length;
        switch (vxlan->proto) {
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
        GreHeader* gre = (GreHeader*)data;
        // calc option fields length
        res.offset = base_length;
        if (gre->S) res.offset += 4;  // Sequence optional : 4 bytes
        if (gre->K) res.offset += 4;  // Key optional : 4 bytes
        if (gre->A) res.offset += 4;  // Acknowledgment optional : 4 bytes
        if (gre->R) {
            // Routing option : SourceRouteEntry
            res.offset += 4;  // Checksum2 + offset2
            while (res.offset + sizeof(SourceRouteEntry) <= size) {
                SourceRouteEntry* sre = (SourceRouteEntry*)(data + res.offset);
                if (res.offset + sizeof(SourceRouteEntry) + sre->length <= size) {
                    res.offset += sizeof(SourceRouteEntry) + sre->length;
                }

                if (0 == sre->addr_family && 0 == sre->length) {
                    break;
                }
            }
        } else if (gre->C)
            res.offset += 4;  // Checksum2 + offset2

        switch (n2h16(gre->proto)) {
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
        GeneveHeader* gnv = (GeneveHeader*)data;
        res.offset = base_length + gnv->opt_len * 4;
        switch (n2h16(gnv->proto)) {
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
        UdpHeader* udphdr = (UdpHeader*)data;
        res.offset = base_length;
        switch (n2h16(udphdr->dst_port)) {
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

        switch (n2h16(udphdr->src_port)) {
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
        const TcpHeader* tcphdr = reinterpret_cast<const TcpHeader*>(data);
        res.offset = tcphdr->offset * 4;
        res.next = eLayer::NONE;

        // switch (n2h16(tcphdr->dst_port)) {
        //     case tcp::eNext::TPKT:
        //         res.next = eLayer::TPKT;
        //         break;
        //     default:
        //         break;
        // }

        // switch (n2h16(tcphdr->src_port)) {
        //     case tcp::eNext::TPKT:
        //         res.next = eLayer::TPKT;
        //         break;
        //     default:
        //         break;
        // }
    }
    return res;
}

Delamination::Result sctp_delamination(eLayer /* below */, const uint8_t* data, int32_t size, uint16_t base_length) {
    Delamination::Result res;
    res.dword = INVALID;
    if (size >= base_length) {
        res.next = eLayer::NONE;
        const SctpHeader* sctp = reinterpret_cast<const SctpHeader*>(data);
        switch (sctp->chunk.type) {
            case sctp::eChunk::DATA:
                res.offset = 28;
                break;
            default:
                res.offset = n2h16(sctp->chunk.length) + 12;
                break;
        }
    }

    return res;
}

Delamination::Result l2tp_delamination(eLayer /* below */, const uint8_t* data, int32_t size, uint16_t base_length) {
    Delamination::Result res;
    res.dword = INVALID;
    if (size > base_length) {
        L2tpHeader* l2tp = (L2tpHeader*)data;
        if (l2tp->L) {
            res.offset = l2tp->tunnel_or_length;
        } else {
            res.offset = base_length;
            if (l2tp->S) res.offset += 4;
            if (l2tp->O) {
                uint16_t offset_length = *((uint16_t*)(data + res.offset));
                res.offset += 2 + offset_length;
            }
        }
        res.next = eLayer::PPP;
    } else {
        res.offset = base_length;
        res.next = eLayer::NONE;
    }
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
    if (size >= base_length) {
        PppHeader* ppp = (PppHeader*)data;
        if (eLayer::PPPoE_Session == below) {
            res.offset = 2;
            nextpro = n2h16(*(uint16_t*)data);
        } else {
            res.offset = base_length;
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
    MplsHeader* mpls = (MplsHeader*)(data);
    if (mpls->bos) {
        // detect forword one byte : ipver
        if (res.offset < size) {
            uint8_t byte = *(uint8_t*)(data + res.offset);
            switch ((byte & 0xf0) >> 4) {
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
    GprsTunnelHeader* gtphdr = (GprsTunnelHeader*)(data);
    if (gtphdr->sequence) res.offset += 2;
    if (gtphdr->npdu_number) res.offset += 1;
    if (gtphdr->next_extension_header) {
        res.offset += 1;  // exthdrtype byte
        res.offset = alignment(res.offset, 4);
        while (res.offset + 4 <= size) {
            uint8_t exthdrtype = *(data + res.offset + 3);
            res.offset += 4;
            if (0 == exthdrtype) {
                break;
            }
        }
        // while (res.offset + sizeof(GprsTunnelHeader::next_ext_hdr) <= size) {
        //     GprsTunnelHeader::next_ext_hdr* exthdr = (GprsTunnelHeader::next_ext_hdr*)(data + res.offset);
        //     res.offset += sizeof(GprsTunnelHeader::next_ext_hdr);
        //     if (0 == exthdr->type) {
        //         break;
        //     }
        // }
    } else {
        res.offset = alignment(res.offset, 4);
    }

    // TODO calc GTP layer length
    if (res.offset < size) {
        // detect forword one byte : ipver
        uint8_t byte = *(uint8_t*)(data + res.offset);
        switch ((byte & 0xf0) >> 4) {
            case gtp::eIpVer::IPv4:
                res.next = eLayer::IPv4;
                break;
            case gtp::eIpVer::IPv6:
                res.next = eLayer::IPv6;
                break;
            default:
                break;
        }
    } else {
        // packet error
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
    if (size) {
        res.offset = data[0] + 1;
    }
    return res;
}

/*
Delamination::Result mpls_delamination(eLayer below , const uint8_t* data, int32_t size, uint16_t base_length) {
    Delamination::Result res;
    res.dword = INVALID;
    for (size_t offs = 0; offs < size; offs += base_length) {
        MplsHeader* mpls = (MplsHeader*)(data + offs);
        if (mpls->bos) {
            offs += base_length;
            res.offset = offs;
            break;
        }
    }

    // detect forword one byte : ipver
    if (res.offset < size) {
        uint8_t byte = *(uint8_t*)(data + res.offset);
        switch (byte & 0xf0) {
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

    return res;
}
*/

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
    parsers_map_[eLayer::IPv6_EXT_AH] = new Delamination(2, ipv6_ah_delamination);
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

NetworkLayer::~NetworkLayer() {}

int32_t NetworkLayer::Layer(const uint8_t* packet, int32_t packet_size, protocol::Layers* layers) {
    flowsql::eLayer layer = flowsql::eLayer::ETHERNET;
    uint16_t offset = 0;
    int32_t degree = 0;
    eLayer below = eLayer::NONE;
    while (offset < packet_size && degree < MAX_LAYERS && layer != eLayer::NONE) {
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

    layers->payload = offset;
    layers->layercount = degree;
    return degree;
}
}  // namespace protocol
}  // namespace flowsql