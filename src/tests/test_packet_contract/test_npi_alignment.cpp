// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <plugins/npi/packet_decoder.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <common/algo/objects_pool.hpp>
#include <common/loader.hpp>
#include <cstring>
#include <iostream>
#include <string>
#include <tuple>
#include <vector>

using namespace flowsql;
namespace {
using Bytes = std::vector<uint8_t>;
constexpr packet::LayerDecodeOptions kOptions{packet::kExtractMac | packet::kExtractIp | packet::kExtractPort};

struct Tracked {
    inline static int live = 0;
    inline static int destroyed = 0;
    int value = 0;
    Tracked() { ++live; }
    ~Tracked() {
        --live;
        ++destroyed;
    }
};

void PoolLifecycle() {
    for (int count : {0, 1, 4, 5, 13}) {
        Tracked::destroyed = 0;
        const int slots = ((count + 3) / 4) * 4;
        {
            ObjectsPool<Tracked, 4> pool;
            std::vector<Tracked*> values;
            for (int i = 0; i < count; ++i) {
                auto* value = pool.Alloc();
                value->value = i + 1;
                values.push_back(value);
            }
            assert(Tracked::live == slots);
            for (int i = 0; i < count; ++i) assert(values[i]->value == i + 1);
        }
        assert(Tracked::live == 0 && Tracked::destroyed == slots);
        {
            BufferPool<32> pool;
            std::vector<uint8_t*> values;
            for (int i = 0; i < count; ++i) {
                auto* value = pool.Alloc(8);
                for (int j = 0; j < 8; ++j) assert(value[j] == 0);
                std::memset(value, i + 1, 8);
                values.push_back(value);
            }
            for (int i = 0; i < count; ++i)
                for (int j = 0; j < 8; ++j) assert(values[i][j] == i + 1);
        }
    }
    {
        BufferPool<8> pool;
        const auto* first = pool.Copy("1234567", 7);
        const auto* second = pool.Copy("next", 4);
        assert(std::strcmp(first, "1234567") == 0 && std::strcmp(second, "next") == 0);
    }
    std::cout << "PASS pool destruction: empty, partial, full, multiple blocks and preserved contents\n";
}
void U16(Bytes& b, size_t p, uint16_t value) {
    b[p] = value >> 8;
    b[p + 1] = value;
}
void Ethernet(Bytes& b, size_t p, uint16_t type) {
    b[p] = 2;
    b[p + 6] = 1;
    U16(b, p + 12, type);
}
void Ip4(Bytes& b, size_t p, uint8_t next) {
    b[p] = 0x45;
    U16(b, p + 2, b.size() - p);
    b[p + 9] = next;
    b[p + 12] = 192;
    b[p + 15] = 1;
    b[p + 16] = 198;
    b[p + 19] = 2;
}
void Transport(Bytes& b, size_t p, uint8_t next, uint16_t port = 80) {
    U16(b, p, 1234);
    U16(b, p + 2, port);
    if (next == ipv4::eNext::TCP)
        b[p + 12] = 0x50;
    else if (next == ipv4::eNext::UDP)
        U16(b, p + 4, b.size() - p);
    else {
        b[p + 12] = 0;
        U16(b, p + 14, 16);
    }
}
Bytes Basic(uint8_t next, bool ip6 = false, bool vlan = false) {
    const size_t network = vlan ? 18 : 14, transport = network + (ip6 ? 40 : 20);
    Bytes b(transport + (next == ipv4::eNext::UDP ? 8 : next == ipv4::eNext::TCP ? 20 : 28));
    Ethernet(b, 0, vlan ? ethernet::eNext::VLAN : ip6 ? ethernet::eNext::IPv6 : ethernet::eNext::IPv4);
    if (vlan) U16(b, 16, ip6 ? ethernet::eNext::IPv6 : ethernet::eNext::IPv4);
    if (ip6) {
        b[network] = 0x60;
        U16(b, network + 4, b.size() - network - 40);
        b[network + 6] = next;
        b[network + 8] = 0x20;
        b[network + 24] = 0x30;
    } else
        Ip4(b, network, next);
    Transport(b, transport, next);
    return b;
}
Bytes Tunnel(eLayer kind) {
    const size_t width = kind == eLayer::GRE ? 4 : 8;
    const bool udp = kind != eLayer::GRE;
    const size_t tunnel = 34 + (udp ? 8 : 0), inner_eth = tunnel + width, inner_ip = inner_eth + (udp ? 14 : 0);
    Bytes b(inner_ip + 20 + 20);
    Ethernet(b, 0, ethernet::eNext::IPv4);
    Ip4(b, 14, udp ? ipv4::eNext::UDP : ipv4::eNext::GRE);
    if (udp) Transport(b, 34, ipv4::eNext::UDP, kind == eLayer::VXLAN ? udp::eNext::VXLAN : udp::eNext::GENEVE);
    if (kind == eLayer::GRE)
        U16(b, tunnel + 2, ethernet::eNext::IPv4);
    else {
        Ethernet(b, inner_eth, ethernet::eNext::IPv4);
        if (kind == eLayer::VXLAN)
            b[tunnel] = 0x08;
        else
            U16(b, tunnel + 2, geneve::eNext::ETHERNET);
    }
    Ip4(b, inner_ip, ipv4::eNext::TCP);
    Transport(b, inner_ip + 20, ipv4::eNext::TCP);
    return b;
}
struct Loaded {
    PluginLoader* owner = PluginLoader::Single();
    IProtocol* protocol = nullptr;
    explicit Loaded(const char* path, const char* rules = nullptr) {
        const char* libraries[] = {path};
        const std::string option = rules ? std::string("{\"ldfile\":\"") + rules + "\"}" : "{}";
        const char* options[] = {option.c_str()};
        assert(owner->Load("", libraries, options, 1) == 0 && owner->StartAll() == 0);
        protocol = static_cast<IProtocol*>(owner->First(IID_PROTOCOL));
        assert(protocol);
        if (rules) assert(protocol->Dictionary()->Query(2000)->number == 2000);
    }
    ~Loaded() {
        protocol = nullptr;
        owner->Unload();
    }
};
packet::PacketView View(const uint8_t* data, size_t size) {
    packet::PacketView v;
    v.bytes = {data, size};
    v.meta.captured_len = size;
    v.meta.wire_len = size;
    v.meta.link_type = 1;
    return v;
}
bool Same(const packet::PacketLayerInfo& a, const packet::PacketLayerInfo& b) {
    if (a.status != b.status || a.layer_count != b.layer_count || a.src_mac.valid != b.src_mac.valid ||
        a.dst_mac.valid != b.dst_mac.valid || !(a.src_mac.value == b.src_mac.value) ||
        !(a.dst_mac.value == b.dst_mac.value) || !(a.src_ip == b.src_ip) || !(a.dst_ip == b.dst_ip) ||
        a.src_port != b.src_port || a.dst_port != b.dst_port || a.ports_valid != b.ports_valid ||
        a.transport_protocol != b.transport_protocol || a.payload_offset != b.payload_offset ||
        a.network_layer_index != b.network_layer_index || a.transport_layer_index != b.transport_layer_index)
        return false;
    for (uint8_t i = 0; i < a.layer_count; ++i)
        if (a.layers[i].kind != b.layers[i].kind || a.layers[i].offset != b.layers[i].offset) return false;
    return true;
}
void Regression(IProtocol* p, const std::vector<Bytes>& inputs) {
    protocol::NpiPacketLayerDecoder decoder(p);
    protocol::NpiPacketProtocolIdentifier identifier(p);
    for (const auto& input : inputs) {
        alignas(16) std::array<uint8_t, 512> storage{};
        std::copy(input.begin(), input.end(), storage.begin());
        auto reference = decoder.Decode(View(storage.data(), input.size()), kOptions);
        assert(reference.status == packet::LayerStatus::kDecoded && reference.ports_valid);
        assert(reference.src_port == 1234 && reference.dst_port == 80);
        const auto identity = identifier.Identify(View(storage.data(), input.size()), reference);
        for (size_t shift = 0; shift < 16; ++shift) {
            std::copy(input.begin(), input.end(), storage.begin() + shift);
            auto v = View(storage.data() + shift, input.size());
            auto decoded = decoder.Decode(v, kOptions);
            assert(Same(reference, decoded));
            const auto result = identifier.Identify(v, decoded);
            assert(result.status == identity.status && result.id == identity.id && result.sub_id == identity.sub_id);
            for (size_t length = 0; length < input.size(); ++length) {
                auto truncated = View(storage.data() + shift, length);
                const auto result = decoder.Decode(truncated, kOptions);
                const auto expected = decoder.Decode(View(input.data(), length), kOptions);
                assert(Same(expected, result));
                if (length < 14) assert(result.status != packet::LayerStatus::kDecoded);
            }
        }
    }
    std::cout << "PASS real NPI layers/decoder/identifier: offsets 0..15, fields and all truncated prefixes\n";
}

void ExactPath(IProtocol* p, const Bytes& input, std::initializer_list<std::pair<eLayer, uint16_t>> path,
               uint16_t payload) {
    protocol::NpiPacketLayerDecoder decoder(p);
    for (size_t shift = 0; shift < 16; ++shift) {
        Bytes storage(input.size() + shift);
        std::copy(input.begin(), input.end(), storage.begin() + shift);
        const auto result = decoder.Decode(View(storage.data() + shift, input.size()), kOptions);
        assert(result.status == packet::LayerStatus::kDecoded);
        assert(result.layer_count == path.size() && result.payload_offset == payload);
        size_t index = 0;
        for (const auto& [kind, offset] : path) {
            assert(result.layers[index].kind == kind && result.layers[index].offset == offset);
            ++index;
        }
        assert(result.ports_valid && result.src_port == 1234 && result.dst_port == 80);
        const int network = result.network_layer_index;
        if (result.layers[network].kind == eLayer::IPv4) {
            assert(std::get<IPv4Address>(result.src_ip).addr == inet_addr("192.0.0.1"));
            assert(std::get<IPv4Address>(result.dst_ip).addr == inet_addr("198.0.0.2"));
        }
    }
    Regression(p, {input});
}

void ProtocolBoundaries(IProtocol* p) {
    // All offsets below come from the protocol wire format, independent of sizeof/offsetof.
    auto ordinary = Tunnel(eLayer::VXLAN);
    ExactPath(p, ordinary,
              {{eLayer::ETHERNET, 0},
               {eLayer::IPv4, 14},
               {eLayer::UDP, 34},
               {eLayer::VXLAN, 42},
               {eLayer::ETHERNET, 50},
               {eLayer::IPv4, 64},
               {eLayer::TCP, 84}},
              104);
    auto gpe = ordinary;
    U16(gpe, 36, 4790);
    gpe[42] = 0x0c;
    gpe[45] = 3;
    ExactPath(p, gpe,
              {{eLayer::ETHERNET, 0},
               {eLayer::IPv4, 14},
               {eLayer::UDP, 34},
               {eLayer::VXLAN_GPE, 42},
               {eLayer::ETHERNET, 50},
               {eLayer::IPv4, 64},
               {eLayer::TCP, 84}},
              104);
    for (const auto& [next, kind, width] :
         {std::tuple{uint8_t(0), eLayer::IPv6_EXT_HOPOPTS, 8}, std::tuple{uint8_t(43), eLayer::IPv6_EXT_ROUTING, 8},
          std::tuple{uint8_t(60), eLayer::IPv6_EXT_DSTOPTS, 8}, std::tuple{uint8_t(44), eLayer::IPv6_EXT_FRAGMENT, 8},
          std::tuple{uint8_t(51), eLayer::IPv6_EXT_AH, 12}}) {
        auto input = Basic(ipv4::eNext::UDP, true);
        input.insert(input.begin() + 54, width, 0);
        input[20] = next;
        input[54] = 17;
        if (next == 51) input[55] = 1;
        U16(input, 18, input.size() - 54);
        ExactPath(p, input,
                  {{eLayer::ETHERNET, 0}, {eLayer::IPv6, 14}, {kind, 54}, {eLayer::UDP, uint16_t(54 + width)}},
                  62 + width);
        if (next == 44) {
            input[57] = 1;  // First fragment with M=1 still has UDP.
            ExactPath(p, input, {{eLayer::ETHERNET, 0}, {eLayer::IPv6, 14}, {kind, 54}, {eLayer::UDP, 62}}, 70);
            input[56] = 1;  // Nonzero fragment offset in network byte order.
            protocol::Layers layers{};
            p->Layer(0, input.data(), input.size(), &layers);
            assert(layers.layercount == 3 && layers.Top() == eLayer::IPv6_EXT_FRAGMENT);
        }
    }
    auto gre = Tunnel(eLayer::GRE);
    gre.insert(gre.begin() + 38, 16, 0);  // checksum + key + sequence + terminal SRE
    gre[34] = 0xf0;
    ExactPath(p, gre,
              {{eLayer::ETHERNET, 0}, {eLayer::IPv4, 14}, {eLayer::GRE, 34}, {eLayer::IPv4, 54}, {eLayer::TCP, 74}},
              94);
    gre[50] = 1;
    gre[53] = 255;  // Captured route data is shorter than SRE.length; must terminate.
    protocol::Layers rejected{};
    p->Layer(0, gre.data(), gre.size(), &rejected);
    assert(rejected.layercount == 3);

    Bytes pppoe(14 + 6 + 2 + 20 + 8);
    Ethernet(pppoe, 0, 0x8864);
    pppoe[14] = 0x11;
    U16(pppoe, 18, pppoe.size() - 20);
    U16(pppoe, 20, 0x0021);
    Ip4(pppoe, 22, 17);
    Transport(pppoe, 42, 17);
    ExactPath(
        p, pppoe,
        {{eLayer::ETHERNET, 0}, {eLayer::PPPoE_Session, 14}, {eLayer::PPP, 20}, {eLayer::IPv4, 22}, {eLayer::UDP, 42}},
        50);
    auto mpls = Basic(17);
    mpls.insert(mpls.begin() + 14, {0, 0, 0, 64, 0, 0, 1, 64});
    U16(mpls, 12, 0x8847);
    ExactPath(p, mpls,
              {{eLayer::ETHERNET, 0}, {eLayer::MPLS, 14}, {eLayer::MPLS, 18}, {eLayer::IPv4, 22}, {eLayer::UDP, 42}},
              50);

    for (int flags : {0, 0x40, 0x08, 0x02, 0x4a}) {
        const int width = 6 + ((flags & 0x40) ? 2 : 0) + ((flags & 8) ? 4 : 0) + ((flags & 2) ? 5 : 0);
        Bytes input(42 + width + 4 + 20 + 8);
        Ethernet(input, 0, 0x0800);
        Ip4(input, 14, 17);
        Transport(input, 34, 17, 1701);
        input[42] = flags;
        input[43] = 2;
        if (flags & 0x40) U16(input, 44, input.size() - 42);
        if (flags & 2) U16(input, 42 + width - 5, 3);
        input[42 + width] = 0xff;
        input[43 + width] = 3;
        U16(input, 44 + width, 0x21);
        Ip4(input, 46 + width, 17);
        Transport(input, 66 + width, 17);
        ExactPath(p, input,
                  {{eLayer::ETHERNET, 0},
                   {eLayer::IPv4, 14},
                   {eLayer::UDP, 34},
                   {eLayer::L2TP, 42},
                   {eLayer::PPP, uint16_t(42 + width)},
                   {eLayer::IPv4, uint16_t(46 + width)},
                   {eLayer::UDP, uint16_t(66 + width)}},
                  74 + width);
    }
    for (int flags = 0; flags < 8; ++flags) {
        const int width = 8 + (flags ? 4 : 0) + ((flags & 4) ? 8 : 0);
        Bytes input(42 + width + 20 + 8);
        Ethernet(input, 0, 0x0800);
        Ip4(input, 14, 17);
        Transport(input, 34, 17, 2152);
        input[42] = 0x30 | flags;
        input[43] = 255;
        U16(input, 44, input.size() - 50);
        if (flags & 4) {
            input[53] = 0x85;
            input[54] = 2;
            input[61] = 0;
        }
        Ip4(input, 42 + width, 17);
        Transport(input, 62 + width, 17);
        ExactPath(p, input,
                  {{eLayer::ETHERNET, 0},
                   {eLayer::IPv4, 14},
                   {eLayer::UDP, 34},
                   {eLayer::GTP, 42},
                   {eLayer::IPv4, uint16_t(42 + width)},
                   {eLayer::UDP, uint16_t(62 + width)}},
                  70 + width);
    }
    std::cout << "PASS independent extension/tunnel paths and variable-field boundaries\n";
}

void RuleHit(IProtocol* p) {
    auto input = Basic(ipv4::eNext::TCP);
    const std::string payload = "GET / HTTP/1.1\r\nHost: example.test\r\n\r\n";
    input.insert(input.end(), payload.begin(), payload.end());
    U16(input, 16, input.size() - 14);
    protocol::NpiPacketLayerDecoder decoder(p);
    protocol::NpiPacketProtocolIdentifier identifier(p);
    alignas(16) std::array<uint8_t, 512> storage{};
    for (size_t shift = 0; shift < 16; ++shift) {
        std::copy(input.begin(), input.end(), storage.begin() + shift);
        const auto view = View(storage.data() + shift, input.size());
        const auto layers = decoder.Decode(view, kOptions);
        const auto result = identifier.Identify(view, layers);
        assert(result.id == 2000);
    }
    std::cout << "PASS configured HTTP rule hit after NPI reload: offsets 0..15\n";
}
void Cost(IProtocol* p, const std::vector<Bytes>& inputs) {
    protocol::NpiPacketLayerDecoder decoder(p);
    std::vector<protocol::Layers> layers(inputs.size());
    for (size_t i = 0; i < inputs.size(); ++i) p->Layer(0, inputs[i].data(), inputs[i].size(), &layers[i]);
    constexpr int count = 50000;
    for (int phase = 0; phase < 3; ++phase) {
        uint64_t checksum = 0;
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < count; ++i) {
            const size_t j = i % inputs.size();
            const auto& b = inputs[j];
            if (phase == 0) {
                protocol::Layers l{};
                checksum += p->Layer(0, b.data(), b.size(), &l) + l.payload;
            } else if (phase == 1) {
                const auto result = decoder.Decode(View(b.data(), b.size()), kOptions);
                checksum += result.src_port + result.dst_port + result.payload_offset;
            } else {
                const auto result = p->Identify(0, b.data(), b.size(), &layers[j]);
                checksum += result.id + result.subid;
            }
        }
        const auto ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
        std::cout << "NPI_COST {\"phase\":" << phase << ",\"packets\":" << count << ",\"ns_per_packet\":" << ns / count
                  << ",\"checksum\":" << checksum << "}\n";
    }
}
}  // namespace
int main(int argc, char** argv) {
    assert(argc >= 2);
    if (argc > 2 && std::string(argv[2]) == "--pool-lifecycle") {
        PoolLifecycle();
        return 0;
    }
    std::vector<Bytes> inputs{Basic(ipv4::eNext::UDP),
                              Basic(ipv4::eNext::TCP),
                              Basic(ipv4::eNext::SCTP),
                              Basic(ipv4::eNext::UDP, true),
                              Basic(ipv4::eNext::TCP, false, true),
                              Tunnel(eLayer::GRE),
                              Tunnel(eLayer::VXLAN),
                              Tunnel(eLayer::GENEVE)};
    if (argc > 2 && std::string(argv[2]) == "--cost") {
        Loaded library(argv[1]);
        Cost(library.protocol, inputs);
    } else {
        PoolLifecycle();
        for (int cycle = 0; cycle < 3; ++cycle) {
            {
                Loaded library(argv[1]);
                Regression(library.protocol, inputs);
                ProtocolBoundaries(library.protocol);
            }
            if (argc > 2) {
                Loaded library(argv[1], argv[2]);
                Regression(library.protocol, inputs);
                ProtocolBoundaries(library.protocol);
                RuleHit(library.protocol);
            }
        }
        std::cout << "PASS repeated NPI load/stop/unload\n";
    }
}
