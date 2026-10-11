// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <common/network/netbase.h>

#include <array>
#include <cassert>
#include <iostream>
#include <type_traits>

using namespace flowsql;

#define LAYOUT(Type, Size)               \
    static_assert(sizeof(Type) == Size); \
    static_assert(alignof(Type) == 1);   \
    static_assert(std::is_trivially_copyable_v<Type>);
LAYOUT(wire::IPv4Address, 4)
LAYOUT(wire::IPv6Address, 16)
LAYOUT(wire::EthernetAddress, 6)
LAYOUT(EthernetHeader, 14)
LAYOUT(VlanHeader, 4)
LAYOUT(VxlanHeader, 8)
LAYOUT(GreHeader, 4)
LAYOUT(SourceRouteEntry, 4)
LAYOUT(GeneveHeader, 8)
LAYOUT(Ipv4Header, 20)
LAYOUT(Ipv6Header, 40)
LAYOUT(Ipv6ExtensionPrefix, 2)
LAYOUT(Ipv6FragmentHeader, 8)
LAYOUT(Ipv6AuthenticationHeader, 12)
LAYOUT(Ipv6EspHeader, 8)
LAYOUT(Ipv6ExtentHeader, 12)
LAYOUT(UdpHeader, 8)
LAYOUT(TCPFlagsBit, 1)
LAYOUT(TCPFlags, 1)
LAYOUT(TcpHeader, 20)
LAYOUT(SctpHeader, 16)
LAYOUT(SctpHeader::Chunk, 4)
LAYOUT(MplsHeader, 4)
LAYOUT(PppoeHeader, 6)
LAYOUT(PppHeader, 4)
LAYOUT(L2tpHeader, 6)
LAYOUT(L2tpHeader::avp, 6)
LAYOUT(GprsTunnelHeader, 8)
LAYOUT(TpktHeader, 4)
LAYOUT(CotpHeader, 1)
LAYOUT(IpVersionHeader, 1)
LAYOUT(PortPairHeader, 4)
LAYOUT(PppProtocolHeader, 2)
LAYOUT(L2tpSessionHeader, 4)
LAYOUT(L2tpSequenceHeader, 4)
LAYOUT(L2tpOffsetHeader, 2)
LAYOUT(GtpOptionalHeader, 4)
LAYOUT(GtpExtensionPrefix, 1)
LAYOUT(IcmpHeader, 8)
#undef LAYOUT
static_assert(offsetof(EthernetHeader, ether_type) == 12);
static_assert(offsetof(VxlanHeader, proto) == 3 && offsetof(VxlanHeader, vni) == 4);
static_assert(offsetof(GeneveHeader, proto) == 2 && offsetof(GeneveHeader, vni) == 4);
static_assert(offsetof(Ipv4Header, src_addr) == 12 && offsetof(Ipv4Header, dst_addr) == 16);
static_assert(offsetof(Ipv6Header, payload) == 4 && offsetof(Ipv6Header, protocol) == 6);
static_assert(offsetof(Ipv6Header, src_addr) == 8 && offsetof(Ipv6Header, dst_addr) == 24);
static_assert(offsetof(TcpHeader, flags) == 13 && offsetof(TcpHeader, window) == 14);
static_assert(offsetof(Ipv6FragmentHeader, fragment) == 2);
static_assert(offsetof(Ipv6AuthenticationHeader, spi) == 4);
static_assert(offsetof(L2tpHeader::avp, vendor) == 2);
// pack must not leak into ordinary runtime objects.
struct PackScope {
    uint8_t byte;
    uint64_t integer;
};
static_assert(alignof(PackScope) == alignof(uint64_t));

template <typename Header, size_t N, typename Check>
void Fixture(const std::array<uint8_t, N>& bytes, Check check) {
    static_assert(N == sizeof(Header));
    std::array<uint8_t, N + 16> storage{};
    for (size_t shift = 0; shift < 16; ++shift) {
        std::memcpy(storage.data() + shift, bytes.data(), N);
        const auto* header = wire::View<Header>(storage.data(), shift + N, shift);
        assert(header && reinterpret_cast<const uint8_t*>(header) == storage.data() + shift);
        check(*header);
        for (size_t length = 0; length < N; ++length)
            assert(!wire::View<Header>(storage.data(), shift + length, shift));
    }
}
int main() {
    Fixture<GreHeader>(std::array<uint8_t, 4>{0xf8, 0x80, 0x08, 0x00}, [](const auto& h) {
        assert(h.C && h.R && h.K && h.S && h.s && h.A && h.ver == 0 && ntohs(h.proto) == 0x0800);
    });
    Fixture<Ipv4Header>(
        std::array<uint8_t, 20>{0x45, 0, 0, 40, 0, 0, 0x20, 0, 64, 6, 0, 0, 192, 0, 2, 1, 198, 51, 100, 2},
        [](const auto& h) {
            assert(h.version == 4 && h.ihl == 5 && h.protocol == 6 && ntohs(h.total_length) == 40);
            assert(wire::ToAnalysis(h.src_addr).addr == inet_addr("192.0.2.1"));
            assert(wire::ToAnalysis(h.dst_addr).addr == inet_addr("198.51.100.2"));
        });
    std::array<uint8_t, 40> v6{0x6a, 0xb1, 0x23, 0x45, 0, 8, 17, 64, 0x20, 1, 0x0d, 0xb8};
    v6[23] = 1;
    v6[24] = 0x20;
    v6[25] = 1;
    v6[26] = 0x0d;
    v6[27] = 0xb8;
    v6[39] = 2;
    Fixture<Ipv6Header>(v6, [](const auto& h) {
        assert(h.version == 6 && h.priority == 10 && h.traffic_class_low == 11);
        assert(h.flow_label_high == 1 && ntohs(h.flow_label_low) == 0x2345 && h.protocol == 17);
        const auto src = wire::ToAnalysis(h.src_addr), dst = wire::ToAnalysis(h.dst_addr);
        assert(src.bytes[15] == 1 && dst.bytes[15] == 2 && src.bytes[0] == 0x20);
    });
    Fixture<EthernetHeader>(
        std::array<uint8_t, 14>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 0x86, 0xdd}, [](const auto& h) {
            assert(wire::ToAnalysis(h.s_addr).bytes[0] == 6 && wire::ToAnalysis(h.d_addr).bytes[5] == 5);
            assert(ntohs(h.ether_type) == 0x86dd);
        });
    Fixture<VxlanHeader>(std::array<uint8_t, 8>{0x1c, 0, 0, 3, 1, 2, 3, 0},
                         [](const auto& h) { assert(h.I && h.P && h.ver == 1 && h.proto == 3 && h.vni[2] == 3); });
    Fixture<GeneveHeader>(std::array<uint8_t, 8>{2, 0xc0, 0x65, 0x58, 1, 2, 3, 0}, [](const auto& h) {
        assert(h.opt_len == 2 && h.c_flag && h.o_flag && ntohs(h.proto) == 0x6558 && h.vni[0] == 1);
    });
    Fixture<TcpHeader>(
        std::array<uint8_t, 20>{4, 0xd2, 0, 80, 0, 0, 0, 1, 0, 0, 0, 2, 0x60, 0xd3, 0x12, 0x34}, [](const auto& h) {
            assert(ntohs(h.src_port) == 1234 && h.offset == 6 && h.flags.flags_bit.syn && h.flags.flags_bit.ack);
            assert(h.flags.flags_bit.fin && h.flags.flags_bit.ece && h.flags.flags_bit.cwr &&
                   ntohs(h.window) == 0x1234);
        });
    Fixture<MplsHeader>(std::array<uint8_t, 4>{0, 1, 1, 64}, [](const auto& h) { assert(h.bos && h.ttl == 64); });
    Fixture<L2tpHeader>(std::array<uint8_t, 6>{0xcb, 2, 0, 20, 0, 1}, [](const auto& h) {
        assert(h.T && h.L && h.S && h.O && h.P && h.ver == 2 && ntohs(h.tunnel_or_length) == 20);
    });
    Fixture<PppoeHeader>(std::array<uint8_t, 6>{0x11, 0, 0, 1, 0, 2},
                         [](const auto& h) { assert(h.ver == 1 && h.type == 1); });
    Fixture<GprsTunnelHeader>(std::array<uint8_t, 8>{0x37, 0xff, 0, 12, 0, 0, 0, 1}, [](const auto& h) {
        assert(h.ver == 1 && h.proto_type && h.sequence && h.npdu_number && h.next_extension_header);
    });
    std::cout << "PASS wire layouts, independent protocol bytes, direct packet views, offsets 0..15, truncated "
                 "prefixes, address conversions\n";
}
