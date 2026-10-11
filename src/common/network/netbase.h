// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_COMMON_NETWORK_NETBASE_H_
#define _FLOWSQL_COMMON_NETWORK_NETBASE_H_

#include <stdint.h>
#include "../typedef.h"
#include "wire.h"

namespace flowsql {
// 定义网络最基本的协议层次

namespace ethernet {
/**< https://blog.csdn.net/GarfieldGCat/article/details/81435742*/
enum eNext : uint16_t {
    IPv4 = 0x0800,            /**< IPv4 Protocol. */
    IPv6 = 0x86DD,            /**< IPv6 Protocol. */
    ARP = 0x0806,             /**< Arp Protocol. */
    RARP = 0x8035,            /**< Reverse Arp Protocol. */
    VLAN = 0x8100,            /**< IEEE 802.1Q VLAN tagging. */
    QINQ = 0x88A8,            /**< IEEE 802.1ad QinQ tagging. */
    PPPOE_DISCOVERY = 0x8863, /**< PPPoE Discovery Stage. */
    PPPOE_SESSION = 0x8864,   /**< PPPoE Session Stage. */
    ETAG = 0x893F,            /**< IEEE 802.1BR E-Tag. */
    PTP = 0x88F7,             /**< IEEE 802.1AS 1588 Precise Time Protocol. */
    SLOW = 0x8809,            /**< Slow protocols (LACP and Marker). */
    TEB = 0x6558,             /**< Transparent Ethernet Bridging. */
    LLDP = 0x88CC,            /**< LLDP Protocol. */
    MPLS = 0x8847,            /**< MPLS ethertype. */
    MPLSM = 0x8848,           /**< MPLS multicast ethertype. */
};
}  // namespace ethernet

/**
 * ipv4
 * https://ccie.lol/knowledge-base/ipv4-and-ipv6-packet-header/
 */
namespace ipv4 {
enum eNext : uint16_t {
    /**< #include <netinet/in.h> */
    ICMP = 1,   /* Internet Control Message Protocol */
    IGMP = 2,   /* Internet Control Message Protocol */
    IPv4 = 4,   /* RFC2003: IP Encapsulation within IP */
    TCP = 6,    /* Transmission Control Protocol */
    EGP = 8,    /* Exterior Gateway Protocol */
    PUP = 12,   /* PUP protocol */
    UDP = 17,   /* User Datagram Protocol */
    IDP = 22,   /* XNS IDP protocol */
    DCCP = 33,  /* RFC4340: Datagram Congestion Control Protocol */
    IPv6 = 41,  /* RFC2473: IPv6 encapsulation */
    GRE = 47,   /* General Routing Encapsulation */
    MTP = 92,   /* Multicast Transport Protocol */
    SCTP = 132, /* Stream Control Transmission Protocol */
    MPLS = 137, /* MPLS in IP */
};
}  // namespace ipv4

/**
 * ipv6
 * https://www.iana.org/assignments/protocol-numbers/protocol-numbers.xhtml
 * https://ccie.lol/knowledge-base/ipv4-and-ipv6-packet-header/
 */
namespace ipv6 {
enum eNext : uint16_t {
    /**< #include <netinet/in.h> */
    IPv6_EXT_HOPOPTS = 0,   /* RFC8200: Hop-by-hop Options Header for IPv6. */
    ICMP = 1,               /* Internet Control Message Protocol. */
    IGMP = 2,               /* Internet Control Message Protocol. */
    IPv4 = 4,               /* ip in ip */
    TCP = 6,                /* Transmission Control Protocol.  */
    EGP = 8,                /* Exterior Gateway Protocol.  */
    PUP = 12,               /* PUP protocol.  */
    UDP = 17,               /* User Datagram Protocol.  */
    IDP = 22,               /* XNS IDP protocol.  */
    DCCP = 33,              /* RFC4340: Datagram Congestion Control Protocol */
    IPv6 = 41,              /* RFC2473: IPv6 encapsulation */
    IPv6_EXT_ROUTING = 43,  /* RFC8200: Routing Header for IPv6 */
    IPv6_EXT_FRAGMENT = 44, /* RFC8200: Fragment Header for IPv6 */
    GRE = 47,               /* General Routing Encapsulation */
    IPv6_EXT_ESP = 50,      /* RFC4303: Encapsulating Security Payload */
    IPv6_EXT_AH = 51,       /* RFC4302: Authentication Header */
    IPv6_EXT_DSTOPTS = 60,  /* RFC8200: Destination Options Header for IPv6 */
    MTP = 92,               /* Multicast Transport Protocol */
    SCTP = 132,             /* Stream Control Transmission Protocol */
    IPv6_EXT_MIPV6 = 135,   /* RFC6275: Mobility Header */
    MPLS = 137,             /* MPLS in IP */
    IPv6_EXT_HIP = 139,     /* RFC7401: Host Identity Protocol */
    IPv6_EXT_SHIM6 = 140,   /* RFC5533: Shim6 Protocol */
};
}  // namespace ipv6

namespace udp {
enum eNext : uint16_t {
    L2TP = 1701,
    GTPC = 2123,  // GTP-C port
    GTPU = 2152,  // GTP-U port

    /**
     * teredo
     * https://www.rfc-editor.org/rfc/rfc4380.html
     * https://blog.csdn.net/zimingzim/article/details/51770575
     */
    IPv6 = 3544,
    GRE = 4754,
    VXLAN = 4789,
    VXLAN_GPE = 4790,
    GENEVE = 6081
};
}

namespace tcp {
enum eNext : uint16_t { TPKT = 102 };
}

namespace sctp {
// https://www.iana.org/assignments/sctp-parameters/sctp-parameters.xhtml
enum eChunk : uint8_t {
    DATA = 0,
    INIT = 1,
    INIT_ACK = 2,
    SACK = 3,
    HEARTBEAT = 4,
    HEARTBEAT_ACK = 5,
    ABORT = 6,
    SHUTDOWN = 7,
    SHUTDOWN_ACK = 8,
    ERROR = 9,
    COOKIE_ECHO = 10,
    COOKIE_ACK = 11,
    SHUTDOWN_COMPLETE = 14
};
}  // namespace sctp

namespace dccp {}

namespace gre {
enum eNext : uint16_t {
    ETHERNET = 0x6558, /**< Transparent Ethernet Bridging. */
    IPv4 = 0x0800,     /**< IPv4 Protocol. */
    PPP = 0x880B,      /**< PPP. */
    IPv6 = 0x86DD,     /**< IPv6 Protocol. */
    MPLS = 0x8847,     /**< MPLS ethertype. */
};
}

namespace vxlan {
enum eNext : uint16_t {
    /* VXLAN-GPE next protocol types */
    IPv4 = 1,     /**< IPv4 Protocol. */
    IPv6 = 2,     /**< IPv6 Protocol. */
    ETHERNET = 3, /**< Ethernet Protocol. */
    NSH = 4,      /**< NSH : Network Service Header. */
    MPLS = 5,     /**< MPLS Protocol. */
    // GBP = 6,      /**< GBP Protocol. */
    // VBNG = 7,     /**< vBNG Protocol. */
};
}

// https://www.rfc-editor.org/rfc/rfc8926.txt
namespace geneve {
enum eNext : uint16_t {
    // TODO
    ETHERNET = 0x6558, /**< Transparent Ethernet Bridging. */
    IPv4 = 0x0800,     /**< IPv4 Protocol. */
    PPP = 0x080B,      /**< PPP. */
    IPv6 = 0x86DD,     /**< IPv6 Protocol. */
    MPLS = 0x8847,     /**< MPLS ethertype. */
};
}

namespace ppp {
enum eNext : uint16_t {
    // TODO
    IPv4 = 0x0021,  /**< IPv4 Protocol. */
    IPv6 = 0x0057,  /**< IPv6 Protocol. */
    MPLS = 0x0281,  /**< MPLS Unicast. */
    MPLSM = 0x0283, /**< MPLS Multicast. */
};
}

namespace l2tp {
enum eNext : uint16_t {
    // TODO
    IPv4 = 0x0021,  /**< IPv4 Protocol. */
    IPv6 = 0x0057,  /**< IPv6 Protocol. */
    MPLS = 0x0281,  /**< MPLS Unicast. */
    MPLSM = 0x0283, /**< MPLS Multicast. */
};
}

namespace mpls {
enum eIpVer : uint16_t { IPv4 = 4, IPv6 = 6 };
}

namespace gtp {
enum eIpVer : uint16_t { IPv4 = 4, IPv6 = 6 };
}

/**
 * https://www.cnblogs.com/CasonChan/p/4739123.html
 * Layer protocol define
 */
enum eLayer : uint16_t {
    NONE = 0,
    TOP = NONE,

    // L2 [1-16)
    L2 = 1,
    ETHERNET = 2,
    VLAN = 3,
    PPPoE_Session = 4,  // PPP over ethernet session
    PPP = 5,            // point to point protocol
    MPLS = 6,           // MPLS-Multicast/MPLS-Unicast

    // L3 [16-32)
    L3 = 16,
    IPv4 = 17,
    IPv6 = 18,
    IPv6_EXT_HOPOPTS = 19,
    IPv6_EXT_ROUTING = 20,
    IPv6_EXT_FRAGMENT = 21,
    IPv6_EXT_ESP = 22,
    IPv6_EXT_AH = 23,
    IPv6_EXT_DSTOPTS = 24,
    GRE = 25,  // Tunnel: IP-GRE-Ethernet OR IP-NVGRE-Ethernet

    // L4 [32-48)
    L4 = 32,
    TCP = 33,
    UDP = 34,
    SCTP = 35,
    DCCP = 36,
    VXLAN = 37,      // Tunnel: UDP:4789-VXLAN-Ethernet
    VXLAN_GPE = 38,  // Tunnel: UDP:4790-VXLAN_GPE-Ethernet
    GENEVE = 39,     // Tunnel: UDP:6081-GENEVE-Ethernet
    L2TP = 40,       // Tunnel: UDP:1701-L2TP-PPP
    GTP = 41,        // Tunnel: UDP:2123-GPRS Tunneling Protocol:https://www.speedguide.net/port.php?port=2123
    RMCP = 42,       // Remote Management Control Protocol
    MAX = 64,

    // ISO Protocol Family : Do not support now.
    TPKT = 65,  // Top of TCP
    COTP = 66,  // Top of TPKT
    ISOMAX = 128
};

/**
 * Ethernet header: Contains the destination address, source address
 * and frame type.
 */
#pragma pack(push, 1)

struct EthernetHeader {
    enum : uint16_t { level = eLayer::ETHERNET };
    wire::EthernetAddress d_addr; /**< Destination address. */
    wire::EthernetAddress s_addr; /**< Source address. */
    uint16_t ether_type;          /**< Frame type. */
};

/**
 * Ethernet VLAN Header.
 * Contains the 16-bit VLAN Tag Control Identifier and the Ethernet type
 * of the encapsulated frame.
 */
struct VlanHeader {
    enum : uint16_t { level = eLayer::VLAN };
    uint16_t vlan_tci;   /**< Priority (3) + CFI (1) + Identifier Code (12) */
    uint16_t ether_type; /**< Ethernet type of encapsulated frame. */
};

/**
 * VXLAN-GPE protocol header (draft-ietf-nvo3-vxlan-gpe-05).
 * Contains the 8-bit flag, 8-bit next-protocol, 24-bit VXLAN Network
 * Identifier and Reserved fields (16 bits and 8 bits).
 */
struct VxlanHeader {
    enum : uint16_t { level = eLayer::VXLAN };
    union {
        uint8_t vx_flags;
        struct {
#if defined(__LITTLE_ENDIAN_BITFIELD)
            uint8_t O : 1;
            uint8_t reserved_flags1 : 1;
            uint8_t P : 1;
            uint8_t I : 1;
            uint8_t ver : 2;
            uint8_t reserved_flags2 : 2;
#else
            uint8_t reserved_flags2 : 2;
            uint8_t ver : 2;
            uint8_t I : 1;
            uint8_t P : 1;
            uint8_t reserved_flags1 : 1;
            uint8_t O : 1;
#endif
        };
    };
    uint8_t reserved1[2];
    uint8_t proto;
    uint8_t vni[3];
    uint8_t reserved2;
};

/**
 * GRE Header sizeof = 4 + K*4 + S*4 + R*
 * RFC 1701
 * https://wenku.baidu.com/view/e5c208f8aef8941ea76e05c0.html
 */
struct GreHeader {
    enum : uint16_t { level = eLayer::GRE };
#if defined(__LITTLE_ENDIAN_BITFIELD)
    uint8_t rc : 3;       /**< Recursion Control */
    uint8_t s : 1;        /**< Strict Source route */
    uint8_t S : 1;        /**< Sequence Number Present bit */
    uint8_t K : 1;        /**< Key Present bit */
    uint8_t R : 1;        /**< Routing Present bit */
    uint8_t C : 1;        /**< Checksum Present bit */
    uint8_t ver : 3;      /**< Version Number */
    uint8_t reserved : 4; /**< Reserved set to zero */
    uint8_t A : 1;        /**< Acknowledgment */
#elif defined(__BIG_ENDIAN_BITFIELD)
    uint8_t C : 1;        /**< Checksum Present bit */
    uint8_t R : 1;        /**< Routing Present bit */
    uint8_t K : 1;        /**< Key Present bit */
    uint8_t S : 1;        /**< Sequence Number Present bit */
    uint8_t s : 1;        /**< Strict Source route */
    uint8_t rc : 3;       /**< Recursion Control */
    uint8_t A : 1;        /**< Acknowledgment */
    uint8_t reserved : 4; /**< Reserved set to zero */
    uint8_t ver : 3;      /**< Version Number */
#endif
    uint16_t proto; /**< Protocol Type */
    // Variable fields below
    // uint16_t checksum;       /**< C=1 OR R=1 */
    // uint16_t offset;         /**< C=1 OR R=1 */
    // SourceRouteEntry sres;   /**< R=1 : list of Source Route Entries (SREs) */
};

struct SourceRouteEntry {
    uint16_t addr_family; /**< Address Family */
    uint8_t offset;       /**< SRE Offset */
    uint8_t length;       /**< SRE Length */
    // std::string routing; /**< Routing Information (variable) */
};

// GENEVE头部定义 sizeof = 8 + opt_len * 4
struct GeneveHeader {
    enum : uint16_t { level = eLayer::GENEVE };
#if defined(__LITTLE_ENDIAN_BITFIELD)
    uint8_t opt_len : 6;   /**< options length MAX 63*4=252. */
    uint8_t version : 2;   /**< version Default 0. */
    uint8_t reserved1 : 6; /**< reserved. */
    uint8_t c_flag : 1;    /**< Critical option flag. */
    uint8_t o_flag : 1;    /**< OAM flag. */
    uint16_t proto;        /**< Protocol Type */
    uint8_t vni[3];
    uint8_t reserved2;
#elif defined(__BIG_ENDIAN_BITFIELD)
    uint8_t version : 2;   /**< version Default 0. */
    uint8_t opt_len : 6;   /**< options length MAX 63*4=252. */
    uint8_t o_flag : 1;    /**< OAM flag. */
    uint8_t c_flag : 1;    /**< Critical option flag. */
    uint8_t reserved1 : 6; /**< reserved. */
    uint16_t proto;        /**< Protocol Type */
    uint8_t vni[3];
    uint8_t reserved2;
#endif
    // uint32_t options[0];    /**< VNI (24) + Reserved (8). */
};

/**
 * IPv4 Header
 */
struct Ipv4Header {
    enum : uint16_t { level = eLayer::IPv4 };
#if defined(__LITTLE_ENDIAN_BITFIELD)
    uint8_t ihl : 4;      // Internet header length (4 bits)
    uint8_t version : 4;  // Version (4 bits)
#elif defined(__BIG_ENDIAN_BITFIELD)
    uint8_t version : 4;  // Version (4 bits)
    uint8_t ihl : 4;      // Internet header length (4 bits)
#endif
    uint8_t type_of_service;     // Type of service
    uint16_t total_length;       // Total length
    uint16_t identify;           // Identification
    uint16_t fragment_offset;    // Flags (3 bits) + Fragment offset (13 bits)
    uint8_t ttl;                 // Time to live
    uint8_t protocol;            // Protocol
    uint16_t checksum;           // Header checksum
    wire::IPv4Address src_addr;  // Source address
    wire::IPv4Address dst_addr;  // Destination address
};

/* IPv6 header */
/* ************************** IPv6 header *******************************
                    IPv6 Header Format RFC 1883

|00             07|               15|               23|               31|(bit)
|-----------------------------------------------------------------------|
|version |priority|flow_label                                           |
|-----------------------------------------------------------------------|
|payload_length                     |nextheader       |hop_limit        |
|-----------------------------------------------------------------------|
|src_addr128bit                                                         |
|                                                                       |
|                                                                       |
|-----------------------------------------------------------------------|
|dst_addr128bit                                                         |
|                                                                       |
|                                                                       |
|-----------------------------------------------------------------------|
************************************************************************/
struct Ipv6Header {
    enum : uint16_t { level = eLayer::IPv6 };
#if defined(__LITTLE_ENDIAN_BITFIELD)
    uint8_t priority : 4;
    uint8_t version : 4;
    uint8_t flow_label_high : 4;
    uint8_t traffic_class_low : 4;
#else
    uint8_t version : 4;
    uint8_t priority : 4;
    uint8_t traffic_class_low : 4;
    uint8_t flow_label_high : 4;
#endif
    uint16_t flow_label_low;
    uint16_t payload;   /**< IP packet length - includes header size */
    uint8_t protocol;   /**< Protocol, next header. */
    uint8_t hop_limits; /**< Hop limits. */
    wire::IPv6Address src_addr;
    wire::IPv6Address dst_addr;
};

// Only this prefix is common to options/routing/AH headers. Actual lengths are protocol-specific.
struct Ipv6ExtensionPrefix {
    uint8_t protocol;
    uint8_t extlen;
};
struct Ipv6FragmentHeader {
    uint8_t protocol;
    uint8_t reserved;
    uint16_t fragment;  // Network order: offset in bits 15..3, M in bit 0.
    uint32_t identification;
};
struct Ipv6AuthenticationHeader {
    uint8_t protocol;
    uint8_t extlen;
    uint16_t reserved;
    uint32_t spi;
    uint32_t sequence;
};
struct Ipv6EspHeader {
    uint32_t spi;
    uint32_t sequence;
};
union Ipv6ExtentHeader {
    Ipv6ExtensionPrefix prefix;
    Ipv6FragmentHeader fragment;
    Ipv6AuthenticationHeader authentication;
    Ipv6EspHeader esp;
};

/**
 * UDP Header
 */
struct UdpHeader {
    enum : uint16_t { level = eLayer::UDP };
    uint16_t src_port;  // Source port
    uint16_t dst_port;  // Destination port
    uint16_t length;    // Datagram length
    uint16_t checksum;  // Checksum
};

/* tcp flags*/
struct TCPFlagsBit {
#if defined(__LITTLE_ENDIAN_BITFIELD)
    uint8_t fin : 1;
    uint8_t syn : 1;
    uint8_t rst : 1;
    uint8_t psh : 1;
    uint8_t ack : 1;
    uint8_t urg : 1;
    uint8_t ece : 1;
    uint8_t cwr : 1;
#elif defined(__BIG_ENDIAN_BITFIELD)
    uint8_t cwr : 1;
    uint8_t ece : 1;
    uint8_t urg : 1;
    uint8_t ack : 1;
    uint8_t psh : 1;
    uint8_t rst : 1;
    uint8_t syn : 1;
    uint8_t fin : 1;
#else
#error "Adjust your <asm/byteorder.h> defines"
#endif
};

union TCPFlags {
    TCPFlagsBit flags_bit;
    uint8_t flags_byte;
};

/**
 * TCP Header
 * https://www.rfc-editor.org/rfc/rfc793.html
 */
struct TcpHeader {
    enum : uint16_t { level = eLayer::TCP };
    uint16_t src_port;  // The source port number
    uint16_t dst_port;  // The destination port number
    uint32_t seq;
    uint32_t ack;

#if defined(__LITTLE_ENDIAN_BITFIELD)
    uint8_t reserved : 4;
    uint8_t offset : 4;
#elif defined(__BIG_ENDIAN_BITFIELD)
    uint8_t offset : 4;
    uint8_t reserved : 4;
#else
#error "Please fix <asm/byteorder.h>"
#endif
    TCPFlags flags;
    uint16_t window;
    uint16_t checkSum;
    uint16_t urgent;
};

/**
 * SCTP Header
 */
struct SctpHeader {
    enum : uint16_t { level = eLayer::SCTP };
    uint16_t src_port; /**< Source port. */
    uint16_t dst_port; /**< Destin port. */
    uint32_t tag;      /**< Validation tag. */
    uint32_t chksum;   /**< Checksum. */

    struct Chunk {
        uint8_t type;
        uint8_t flags;
        uint16_t length;
    } chunk;
};

/*
 * FF: private data passed from the MPLS dissector to subdissectors
 * (data parameter).
 */
struct MplsHeader {
    enum : uint16_t { level = eLayer::MPLS };
    uint16_t label_high;  // Network-order first 16 label bits.
#if defined(__LITTLE_ENDIAN_BITFIELD)
    uint8_t bos : 1;
    uint8_t exp : 3;
    uint8_t label_low : 4;
#else
    uint8_t label_low : 4;
    uint8_t exp : 3;
    uint8_t bos : 1;
#endif
    uint8_t ttl;
};

/**
 * PppoeHeader Header sizeof = 6
 * [rfc2516]
 * https://www.rfc-editor.org/rfc/rfc2516.txt
 * PPPoe-Session is layer protocol, PPPoe-Discovery is just protocol
 */
struct PppoeHeader {
    enum : uint16_t { level = eLayer::PPPoE_Session };
#if defined(__LITTLE_ENDIAN_BITFIELD)
    uint8_t type : 4;
    uint8_t ver : 4;
#elif defined(__BIG_ENDIAN_BITFIELD)
    uint8_t ver : 4;
    uint8_t type : 4;
#else
#error "Please fix <asm/byteorder.h>"
#endif
    uint8_t code;
    uint16_t session_id;
    uint16_t payload_length;
};

/**
 * PppHeader Header sizeof = 4
 * [RFC1661][RFC3818]
 * https://blog.csdn.net/bytxl/article/details/50111971
 * https://www.rfc-editor.org/rfc/rfc1661.html
 * https://www.iana.org/assignments/ppp-numbers/ppp-numbers.xhtml
 */
struct PppHeader {
    enum : uint16_t { level = eLayer::PPP };
    // address and control fields do not present if PPP over PPPoE
    uint8_t address;
    uint8_t control;
    uint16_t protocol;
};

/**
 * L2TP Header sizeof = 2
 * RFC 2661 for L2TPv2
 * https://tools.ietf.org/html/rfc2661
 *
 * RFC 3931 for L2TPv3
 * https://tools.ietf.org/html/rfc3931
 *
 * https://www.iana.org/assignments/l2tp-parameters/l2tp-parameters.xhtml
 */
struct L2tpHeader {
    enum : uint16_t { level = eLayer::L2TP };
#if defined(__LITTLE_ENDIAN_BITFIELD)
    uint8_t P : 1;     /**< the Priority bit */
    uint8_t O : 1;     /**< the Offset Size field. MUST be set to 0 (zero) for control messages. */
    uint8_t rsvd1 : 1; /**< reserved set to zero */
    uint8_t S : 1;     /**< the Ns and Nr fields. MUST be set to 1 for control messages. */
    uint8_t rsvd2 : 2; /**< All reserved bits MUST be set to 0 on outgoing messages and ignored on incoming messages */
    uint8_t L : 1;     /**< the Length field. MUST be set to 1 for control messages. */
    uint8_t T : 1;     /**< indicates the type of message. 0 for a data message and 1 for a control message. */
    uint8_t ver : 4;   /**< Version Number, Ver MUST be 2 or 3*/
    uint8_t rsvd4 : 4; /**< Reserved set to zero */
#elif defined(__BIG_ENDIAN_BITFIELD)
    uint8_t T : 1;     /**< indicates the type of message. 0 for a data message and 1 for a control message. */
    uint8_t L : 1;     /**< the Length field. MUST be set to 1 for control messages. */
    uint8_t rsvd2 : 2; /**< All reserved bits MUST be set to 0 on outgoing messages and ignored on incoming messages */
    uint8_t S : 1;     /**< the Ns and Nr fields. MUST be set to 1 for control messages. */
    uint8_t rsvd1 : 1; /**< reserved set to zero */
    uint8_t O : 1;     /**< the Offset Size field. MUST be set to 0 (zero) for control messages. */
    uint8_t P : 1;     /**< the Priority bit */
    uint8_t rsvd4 : 4; /**< Reserved set to zero */
    uint8_t ver : 4;   /**< Version Number, Ver MUST be 2*/
#endif
    uint16_t tunnel_or_length;
    uint16_t session_or_tunnel;

    struct avp {
#if defined(__LITTLE_ENDIAN_BITFIELD)
        uint16_t length : 10; /**< Encodes the number of octets (including the Overall Length and bitmask fields)
                                 contained in this AVP */
        uint16_t rsvd4 : 4;   /**< All reserved bits MUST be set to 0 */
        uint16_t H : 1;       /**< Hidden (H) bit. */
        uint16_t M : 1;       /**< Mandatory (M) bit. */

        uint16_t vendor;    /**<  Vendor ID */
        uint16_t attr_type; /**< Attribute Type. */
                            /**uint8_t attr_value[0]; // < Attribute Value length = avp.length - 6*/
#elif defined(__BIG_ENDIAN_BITFIELD)
        uint16_t M : 1;       /**< Mandatory (M) bit. */
        uint16_t H : 1;       /**< Hidden (H) bit. */
        uint16_t rsvd4 : 4;   /**< All reserved bits MUST be set to 0 */
        uint16_t length : 10; /**< Encodes the number of octets (including the Overall Length and bitmask fields)
                               contained in this AVP */
        uint16_t vendor;      /**<  Vendor ID */
        uint16_t attr_type;   /**< Attribute Type. */
                              /**uint8_t attr_value[0]; // < Attribute Value length = avp.length - 6*/
#endif
    };
};

// GTP-U
struct GprsTunnelHeader {
    enum : uint16_t { level = eLayer::GTP };
#if defined(__LITTLE_ENDIAN_BITFIELD)
    uint8_t npdu_number : 1;           /**< Is N-DPU Number Present? */
    uint8_t sequence : 1;              /**< Is Sequence Number Present? */
    uint8_t next_extension_header : 1; /**< Is Next Extension Header Present? */
    uint8_t rsvd : 1;                  /**< Reserved bit. */
    uint8_t proto_type : 1;            /**< Protocol Type. */
    uint8_t ver : 3;                   /**< Version. */
#elif defined(__BIG_ENDIAN_BITFIELD)
    uint8_t ver : 3;                   /**< Version. */
    uint8_t proto_type : 1;            /**< Protocol Type. */
    uint8_t rsvd : 1;                  /**< Reserved bit. */
    uint8_t next_extension_header : 1; /**< Is Next Extension Header Present? */
    uint8_t sequence : 1;              /**< Is Sequence Number Present? */
    uint8_t npdu_number : 1;           /**< Is N-DPU Number Present? */
#endif
    uint8_t msg_type;
    uint16_t length;
    uint32_t teid;

    // struct next_ext_hdr {
    //     uint8_t len;
    //     uint16_t word;
    //     uint8_t type;
    // };
};

// TPKT
// https://www.rfc-editor.org/rfc/rfc1006.html
struct TpktHeader {
    enum : uint16_t { level = eLayer::TPKT };
    uint8_t ver;
    uint8_t rsvd;
    uint16_t length;
};

// COTP
// https://www.rfc-editor.org/rfc/rfc983.html
struct CotpHeader {
    enum : uint16_t { level = eLayer::COTP };
    uint8_t length;
    // ...
};

// Fixed pieces of variable headers, viewed directly after their captured ranges are checked.
struct IpVersionHeader {
#if defined(__LITTLE_ENDIAN_BITFIELD)
    uint8_t low : 4;
    uint8_t version : 4;
#else
    uint8_t version : 4;
    uint8_t low : 4;
#endif
};
struct PortPairHeader {
    uint16_t src_port;
    uint16_t dst_port;
};
struct PppProtocolHeader {
    uint16_t protocol;
};
struct L2tpSessionHeader {
    uint16_t tunnel;
    uint16_t session;
};
struct L2tpSequenceHeader {
    uint16_t ns;
    uint16_t nr;
};
struct L2tpOffsetHeader {
    uint16_t size;
};
struct GtpOptionalHeader {
    uint16_t sequence;
    uint8_t npdu;
    uint8_t next_extension;
};
struct GtpExtensionPrefix {
    uint8_t length;
};
struct IcmpHeader {
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
    union {
        struct {
            uint16_t id;
            uint16_t sequence;
        } echo;
        struct {
            uint16_t unused;
            uint16_t mtu;
        } unreachable;
        uint32_t mtu;
    } body;
};

#pragma pack(pop)

}  // namespace flowsql

#endif  //_FLOWSQL_COMMON_NETWORK_NETBASE_H_
