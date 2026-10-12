// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <operators/npm_basic/npm_ip_family_contract.h>

#include <arpa/inet.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

namespace {
namespace network = flowsql::network;
namespace npm = flowsql::npm;
namespace packet = flowsql::packet;
namespace wire = flowsql::wire;

static_assert(sizeof(network::IPv4Address) == 4 && alignof(network::IPv4Address) == 4);
static_assert(sizeof(network::IPv6Address) == 16 && alignof(network::IPv6Address) == 16);
static_assert(sizeof(network::EthernetAddress) == 6 && alignof(network::EthernetAddress) == 2);
static_assert(std::is_standard_layout_v<network::IPv4Address> && std::is_trivially_copyable_v<network::IPv4Address>);
static_assert(std::is_standard_layout_v<network::IPv6Address> && std::is_trivially_copyable_v<network::IPv6Address>);
static_assert(std::is_standard_layout_v<network::EthernetAddress> &&
              std::is_trivially_copyable_v<network::EthernetAddress>);
static_assert(offsetof(network::IPv4Address, bytes) == 0);
static_assert(offsetof(network::IPv6Address, bytes) == 0);
static_assert(offsetof(network::EthernetAddress, bytes) == 0);

// Freeze the supported 64-bit toolchain layout, including string ownership and scalar tail padding.
static_assert(sizeof(std::string) == 32);
static_assert(sizeof(npm::NpmSessionKey4) == 56 && alignof(npm::NpmSessionKey4) == 8);
static_assert(offsetof(npm::NpmSessionKey4, a_ip) == 0);
static_assert(offsetof(npm::NpmSessionKey4, b_ip) == 4);
static_assert(offsetof(npm::NpmSessionKey4, observation_domain_id) == 8);
static_assert(offsetof(npm::NpmSessionKey4, input_namespace) == 16);
static_assert(offsetof(npm::NpmSessionKey4, a_port) == 48);
static_assert(offsetof(npm::NpmSessionKey4, b_port) == 50);
static_assert(offsetof(npm::NpmSessionKey4, transport_protocol) == 52);
static_assert(sizeof(npm::NpmSessionKey6) == 80 && alignof(npm::NpmSessionKey6) == 16);
static_assert(offsetof(npm::NpmSessionKey6, a_ip) == 0);
static_assert(offsetof(npm::NpmSessionKey6, b_ip) == 16);
static_assert(offsetof(npm::NpmSessionKey6, input_namespace) == 32);
static_assert(offsetof(npm::NpmSessionKey6, observation_domain_id) == 64);
static_assert(offsetof(npm::NpmSessionKey6, a_port) == 72);
static_assert(offsetof(npm::NpmSessionKey6, b_port) == 74);
static_assert(offsetof(npm::NpmSessionKey6, transport_protocol) == 76);
static_assert(sizeof(npm::NpmSessionKeyView) == 16);
static_assert(!std::is_default_constructible_v<npm::NpmSessionKeyView>);
static_assert(!std::is_constructible_v<npm::NpmSessionKeyView, npm::NpmSessionKey4&&>);
static_assert(!std::is_constructible_v<npm::NpmSessionKeyView, npm::NpmSessionKey6&&>);
static_assert(!std::is_constructible_v<npm::NpmSessionKeyView, const npm::NpmSessionKey4&&>);
static_assert(!std::is_constructible_v<npm::NpmSessionKeyView, const npm::NpmSessionKey6&&>);
static_assert(network::IPv4Address{{1, 2, 3, 4}}.ToUInt32() == 0x01020304u);

void TestIpv4NumericSemantics() {
    const struct {
        const char* text;
        uint32_t value;
    } cases[] = {{"0.0.0.0", 0u},
                 {"0.0.0.1", 1u},
                 {"1.2.3.4", 0x01020304u},
                 {"128.0.0.1", 0x80000001u},
                 {"192.0.2.128", 0xc0000280u},
                 {"255.255.255.255", 0xffffffffu}};
    for (const auto& sample : cases) {
        wire::IPv4Address input{};
        assert(inet_pton(AF_INET, sample.text, input.bytes) == 1);
        const auto address = network::FromWire(input);
        assert(address.ToUInt32() == sample.value);
        const auto reconstructed = network::IPv4Address::FromUInt32(sample.value);
        assert(reconstructed == address);
        char text[INET_ADDRSTRLEN]{};
        assert(inet_ntop(AF_INET, reconstructed.bytes, text, sizeof(text)) != nullptr);
        assert(std::string(text) == sample.text);
    }
}

template <typename Wire, typename Address, size_t Size>
void TestWireCopies(const std::array<uint8_t, Size>& expected) {
    alignas(16) std::array<uint8_t, Size + 15> packet_bytes{};
    for (size_t offset = 0; offset != 16; ++offset) {
        std::copy(expected.begin(), expected.end(), packet_bytes.begin() + offset);
        const auto* input = wire::View<Wire>(packet_bytes.data(), packet_bytes.size(), offset);
        assert(input != nullptr);
        const Address owned = network::FromWire(*input);
        packet_bytes.fill(0);  // The input can be reused immediately after conversion.
        assert(std::memcmp(owned.bytes, expected.data(), Size) == 0);
    }
}

void TestNetworkOctetsAndAlignment() {
    TestWireCopies<wire::IPv4Address, network::IPv4Address>(std::array<uint8_t, 4>{192, 0, 2, 128});
    TestWireCopies<wire::IPv6Address, network::IPv6Address>(
        std::array<uint8_t, 16>{0x20, 1, 0x0d, 0xb8, 0, 0xff, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 1});
    TestWireCopies<wire::EthernetAddress, network::EthernetAddress>(std::array<uint8_t, 6>{0x02, 0, 0x80, 0xff, 0, 1});

    std::vector<network::IPv6Address> addresses(9);
    std::vector<npm::NpmSessionKey6> keys(9);
    for (size_t index = 0; index != keys.size(); ++index) {
        assert(reinterpret_cast<uintptr_t>(&addresses[index]) % 16 == 0);
        assert(reinterpret_cast<uintptr_t>(&keys[index].a_ip) % 16 == 0);
        assert(reinterpret_cast<uintptr_t>(&keys[index].b_ip) % 16 == 0);
        assert(reinterpret_cast<const uint8_t*>(&keys[index].b_ip) -
                   reinterpret_cast<const uint8_t*>(&keys[index].a_ip) ==
               16);
    }

    const network::IPv4Address zero4{};
    const network::IPv6Address zero6{};
    assert(zero4.ToUInt32() == 0);  // A zero address is a value, not the absence of an address.
    for (uint8_t byte : zero6.bytes) assert(byte == 0);
    auto other6 = zero6;
    other6.bytes[15] = 1;
    assert(!(other6 == zero6));
    const network::EthernetAddress zero_mac{};
    auto other_mac = zero_mac;
    other_mac.bytes[5] = 1;
    assert(!(other_mac == zero_mac));
}

void TestBorrowedFamilyDispatch() {
    npm::NpmSessionKey4 key4;
    npm::NpmSessionKey6 key6;
    key4.a_ip = network::IPv4Address::FromUInt32(0xc0000201u);
    key4.a_port = 12345;
    std::copy(std::begin(key4.a_ip.bytes), std::end(key4.a_ip.bytes), key6.a_ip.bytes);
    key6.a_port = key4.a_port;
    const npm::NpmSessionKeyView view4(key4);
    const npm::NpmSessionKeyView view6(key6);
    assert(view4.Family() == packet::AddressFamily::kIPv4);
    assert(view6.Family() == packet::AddressFamily::kIPv6);
    assert(view4.Get4() == &key4 && view4.Get6() == nullptr);
    assert(view6.Get6() == &key6 && view6.Get4() == nullptr);
    key4.a_port = 54321;
    assert(view4.Get4()->a_port == 54321);  // The view borrows; it does not copy a key.
    auto copied_view = view6;
    assert(copied_view.Get6() == &key6);
    copied_view = view4;
    assert(copied_view.Get4() == &key4 && copied_view.Get6() == nullptr);
}

void TestOwnedKeyCopies() {
    npm::NpmSessionKey4 retained4;
    npm::NpmSessionKey6 retained6;
    const std::string expected_namespace(128, 'n');  // Force actual string ownership beyond SSO.
    {
        npm::NpmSessionKey4 key4;
        key4.a_ip = network::IPv4Address::FromUInt32(0xc0000201u);
        key4.b_ip = network::IPv4Address::FromUInt32(0xc6336402u);
        key4.input_namespace = expected_namespace;
        key4.observation_domain_id = 77;
        key4.a_port = 12345;
        key4.b_port = 443;
        key4.transport_protocol = 6;
        npm::NpmSessionKey6 key6;
        assert(inet_pton(AF_INET6, "2001:db8::1", key6.a_ip.bytes) == 1);
        assert(inet_pton(AF_INET6, "2001:db8::2", key6.b_ip.bytes) == 1);
        key6.input_namespace = expected_namespace;
        key6.observation_domain_id = 88;
        key6.a_port = 5353;
        key6.b_port = 53;
        key6.transport_protocol = 17;
        retained4 = *npm::NpmSessionKeyView(key4).Get4();
        retained6 = *npm::NpmSessionKeyView(key6).Get6();
        key4 = {};
        key6 = {};
    }
    assert(retained4.a_ip.ToUInt32() == 0xc0000201u && retained4.b_ip.ToUInt32() == 0xc6336402u);
    assert(retained4.input_namespace == expected_namespace && retained4.observation_domain_id == 77);
    assert(retained4.a_port == 12345 && retained4.b_port == 443 && retained4.transport_protocol == 6);
    char text[INET6_ADDRSTRLEN]{};
    assert(inet_ntop(AF_INET6, retained6.a_ip.bytes, text, sizeof(text)) != nullptr);
    assert(std::string(text) == "2001:db8::1");
    assert(inet_ntop(AF_INET6, retained6.b_ip.bytes, text, sizeof(text)) != nullptr);
    assert(std::string(text) == "2001:db8::2");
    assert(retained6.input_namespace == expected_namespace && retained6.observation_domain_id == 88);
    assert(retained6.a_port == 5353 && retained6.b_port == 53 && retained6.transport_protocol == 17);
}
}  // namespace

int main() {
    TestIpv4NumericSemantics();
    TestNetworkOctetsAndAlignment();
    TestBorrowedFamilyDispatch();
    TestOwnedKeyCopies();
    std::puts("IP family contract: address layouts, numeric values, wire ownership and typed key views passed");
    return 0;
}
