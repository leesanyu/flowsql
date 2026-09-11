// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <channels/pcapfile/pcap_file_channel.h>
#include <channels/pcapfile/pcap_file_channel_store.h>
#include <channels/pcapfile/packet_filter_domain.h>
#include <common/loader.hpp>
#include <framework/core/filter_binding.h>
#include <framework/core/filter_executor.h>
#include <framework/core/filter_expression.h>
#include <framework/core/filter_planner.h>
#include <framework/core/packet_codec.h>
#include <framework/interfaces/iblock_stream_reader.h>

#include <arrow/api.h>
#include <sqlite3.h>

#include <arpa/inet.h>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace pcapfile = flowsql::channels::pcapfile;
namespace packet = flowsql::packet;

namespace {

class MockProtocol final : public flowsql::IProtocol {
 public:
    void Concurrency(int32_t) override {}
    flowsql::protocol::Protocol Identify(int32_t, const uint8_t*, int32_t, const flowsql::protocol::Layers*) override {
        ++identify_calls;
        return {};
    }
    int32_t Layer(int32_t, const uint8_t* data, int32_t size, flowsql::protocol::Layers* layers) override {
        ++layer_calls;
        layer_sizes.push_back(size);
        if (data && size > 0) {
            layer_packets.emplace_back(data, data + size);
        } else {
            layer_packets.emplace_back();
        }
        if (size == 0 || !layers) return -1;
        if (layer_result < 0) return layer_result;
        *layers = {};
        layers->layercount = reported_layer_count;
        if (reported_layer_count != 0) {
            layers->layers[0].layer = layer_kind;
            layers->layers[0].offset = layer_offset;
        }
        layers->payload = payload_offset;
        return layer_result;
    }
    flowsql::protocol::IDictionary* Dictionary() override { return nullptr; }
    int layer_calls = 0;
    int identify_calls = 0;
    int layer_result = 1;
    uint16_t reported_layer_count = 1;
    flowsql::eLayer layer_kind = flowsql::eLayer::ETHERNET;
    uint16_t layer_offset = 0;
    uint16_t payload_offset = 0;
    std::vector<int32_t> layer_sizes;
    std::vector<std::vector<uint8_t>> layer_packets;
};

class DeferredProtocolQuerier final : public flowsql::IQuerier {
 public:
    void SetProtocol(flowsql::IProtocol* protocol) { protocol_ = protocol; }

    int Traverse(const flowsql::Guid& iid, fntraverse callback) override {
        if (iid < flowsql::IID_PROTOCOL || flowsql::IID_PROTOCOL < iid || !protocol_) return 0;
        return callback ? callback(protocol_) : 0;
    }

    void* First(const flowsql::Guid& iid) override {
        if (!(iid < flowsql::IID_PROTOCOL) && !(flowsql::IID_PROTOCOL < iid)) return protocol_;
        return nullptr;
    }

 private:
    flowsql::IProtocol* protocol_ = nullptr;
};

void Put16(std::vector<uint8_t>* out, uint16_t value, bool little) {
    if (little) { out->push_back(value & 0xff); out->push_back(value >> 8); }
    else { out->push_back(value >> 8); out->push_back(value & 0xff); }
}
void Put32(std::vector<uint8_t>* out, uint32_t value, bool little) {
    if (little) { for (int i = 0; i < 4; ++i) out->push_back(static_cast<uint8_t>(value >> (8 * i))); }
    else { for (int i = 3; i >= 0; --i) out->push_back(static_cast<uint8_t>(value >> (8 * i))); }
}
void Put64(std::vector<uint8_t>* out, uint64_t value, bool little) {
    if (little) { for (int i = 0; i < 8; ++i) out->push_back(static_cast<uint8_t>(value >> (8 * i))); }
    else { for (int i = 7; i >= 0; --i) out->push_back(static_cast<uint8_t>(value >> (8 * i))); }
}
void WriteFile(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::vector<uint8_t> MakeClassicHeader(bool little, bool nanosecond,
                                       uint32_t snaplen = 65535,
                                       uint32_t link_type = 1) {
    std::vector<uint8_t> bytes;
    if (little && nanosecond) bytes.insert(bytes.end(), {0x4d, 0x3c, 0xb2, 0xa1});
    else if (!little && nanosecond) bytes.insert(bytes.end(), {0xa1, 0xb2, 0x3c, 0x4d});
    else if (little) bytes.insert(bytes.end(), {0xd4, 0xc3, 0xb2, 0xa1});
    else bytes.insert(bytes.end(), {0xa1, 0xb2, 0xc3, 0xd4});
    Put16(&bytes, 2, little);
    Put16(&bytes, 4, little);
    Put32(&bytes, 0, little);
    Put32(&bytes, 0, little);
    Put32(&bytes, snaplen, little);
    Put32(&bytes, link_type, little);
    return bytes;
}

void AppendClassicRecord(std::vector<uint8_t>* bytes, bool little,
                         uint32_t seconds, uint32_t fraction,
                         const std::vector<uint8_t>& packet,
                         uint32_t wire_len) {
    Put32(bytes, seconds, little);
    Put32(bytes, fraction, little);
    Put32(bytes, static_cast<uint32_t>(packet.size()), little);
    Put32(bytes, wire_len, little);
    bytes->insert(bytes->end(), packet.begin(), packet.end());
}

void Store32(std::vector<uint8_t>* bytes, size_t offset, uint32_t value, bool little) {
    assert(bytes && offset + 4 <= bytes->size());
    for (size_t i = 0; i < 4; ++i) {
        const size_t shift_index = little ? i : 3 - i;
        (*bytes)[offset + i] = static_cast<uint8_t>(value >> (8 * shift_index));
    }
}

std::vector<uint8_t> MakeClassicPcap(bool little, bool nanosecond, uint32_t fraction,
                                     uint32_t link_type = 1, uint32_t captured = 4,
                                     uint32_t wire = 4) {
    std::vector<uint8_t> bytes = MakeClassicHeader(little, nanosecond, 65535, link_type);
    Put32(&bytes, 2, little);
    Put32(&bytes, fraction, little);
    Put32(&bytes, captured, little);
    Put32(&bytes, wire, little);
    bytes.insert(bytes.end(), {1, 2, 3, 4});
    return bytes;
}

std::vector<uint8_t> MakeNanosecondReplayPcap(const std::vector<uint64_t>& timestamps_ns) {
    std::vector<uint8_t> bytes = {0x4d, 0x3c, 0xb2, 0xa1};
    Put16(&bytes, 2, true);
    Put16(&bytes, 4, true);
    Put32(&bytes, 0, true);
    Put32(&bytes, 0, true);
    Put32(&bytes, 65535, true);
    Put32(&bytes, 1, true);
    for (size_t index = 0; index < timestamps_ns.size(); ++index) {
        Put32(&bytes, static_cast<uint32_t>(timestamps_ns[index] / 1000000000ULL), true);
        Put32(&bytes, static_cast<uint32_t>(timestamps_ns[index] % 1000000000ULL), true);
        Put32(&bytes, 1, true);
        Put32(&bytes, 1, true);
        bytes.push_back(static_cast<uint8_t>(index + 1));
    }
    return bytes;
}

std::vector<uint8_t> MakeEthernetPacket(bool target_mac) {
    std::vector<uint8_t> packet(14, 0);
    const std::array<uint8_t, 6> mac = target_mac
                                           ? std::array<uint8_t, 6>{0xaa, 0xbb, 0xcc,
                                                                    0xdd, 0xee, 0xff}
                                           : std::array<uint8_t, 6>{0x10, 0x20, 0x30,
                                                                    0x40, 0x50, 0x60};
    std::copy(mac.begin(), mac.end(), packet.begin());
    packet[12] = 0x08;
    packet[13] = 0x00;
    return packet;
}

void AppendPcapngBlock(std::vector<uint8_t>* out, uint32_t type,
                       const std::vector<uint8_t>& body, bool little) {
    const uint32_t total = static_cast<uint32_t>(body.size() + 12);
    Put32(out, type, little);
    Put32(out, total, little);
    out->insert(out->end(), body.begin(), body.end());
    Put32(out, total, little);
}

void AppendPcapngSectionHeaderWithOptions(std::vector<uint8_t>* out, bool little,
                                          const std::vector<uint8_t>& options) {
    std::vector<uint8_t> body;
    Put32(&body, 0x1a2b3c4d, little);
    Put16(&body, 1, little);
    Put16(&body, 0, little);
    Put64(&body, 0xffffffffffffffffULL, little);
    body.insert(body.end(), options.begin(), options.end());
    AppendPcapngBlock(out, 0x0a0d0d0a, body, little);
}

void AppendPcapngSectionHeader(std::vector<uint8_t>* out, bool little) {
    AppendPcapngSectionHeaderWithOptions(out, little, {});
}

void AppendPcapngInterface(std::vector<uint8_t>* out, bool little, uint16_t link_type,
                           bool binary_resolution = false, uint8_t resolution = 6,
                           int64_t tsoffset = 0, uint32_t snaplen = 65535) {
    std::vector<uint8_t> body;
    Put16(&body, link_type, little);
    Put16(&body, 0, little);
    Put32(&body, snaplen, little);
    if (binary_resolution || resolution != 6) {
        Put16(&body, 9, little); Put16(&body, 1, little);
        body.push_back(static_cast<uint8_t>(resolution | (binary_resolution ? 0x80 : 0)));
        body.insert(body.end(), 3, 0);
    }
    if (tsoffset != 0) {
        Put16(&body, 14, little); Put16(&body, 8, little);
        Put64(&body, static_cast<uint64_t>(tsoffset), little);
    }
    if (body.size() > 8) {
        Put16(&body, 0, little); Put16(&body, 0, little);
    }
    AppendPcapngBlock(out, 1, body, little);
}

void AppendPcapngEnhancedPacket(std::vector<uint8_t>* out, bool little, uint32_t interface_id,
                                uint64_t timestamp, const std::vector<uint8_t>& packet,
                                uint32_t wire_len) {
    std::vector<uint8_t> body;
    Put32(&body, interface_id, little);
    Put32(&body, static_cast<uint32_t>(timestamp >> 32), little);
    Put32(&body, static_cast<uint32_t>(timestamp), little);
    Put32(&body, static_cast<uint32_t>(packet.size()), little);
    Put32(&body, wire_len, little);
    body.insert(body.end(), packet.begin(), packet.end());
    while ((body.size() & 3u) != 0) body.push_back(0);
    AppendPcapngBlock(out, 6, body, little);
}

void AppendPcapngEnhancedPacket(std::vector<uint8_t>* out, bool little, uint32_t interface_id,
                                uint64_t timestamp, const std::vector<uint8_t>& packet) {
    AppendPcapngEnhancedPacket(out, little, interface_id, timestamp, packet,
                               static_cast<uint32_t>(packet.size()));
}

std::string Temp(const char* suffix) { return std::string("/tmp/flowsql_pcapfile_") + suffix; }

void AssertNoBatchEvent(const flowsql::BlockPollEvent& event, flowsql::BlockPollEvent::Kind kind) {
    assert(event.kind == kind);
    assert(!event.batch);
}

void AssertCaptureSourceError(const char* suffix, const char* format,
                              const std::vector<uint8_t>& bytes) {
    const std::string path = Temp(suffix);
    WriteFile(path, bytes);
    MockProtocol protocol;
    pcapfile::PcapFileSourceConfig config;
    config.path = path;
    config.format = format;
    pcapfile::PcapFileChannel channel("capture_error", config, &protocol);
    const int open_rc = channel.Open();
    const auto event = channel.PollBlock(0);
    AssertNoBatchEvent(event, flowsql::BlockPollEvent::kError);
    assert(event.err != 0);
    const auto after_error = channel.PollBlock(0);
    AssertNoBatchEvent(after_error, flowsql::BlockPollEvent::kCancelled);
    assert(after_error.err == ECANCELED);
    if (open_rc == 0) assert(channel.IsOpened());
}

void AssertPcapngSourceError(const char* suffix, const std::vector<uint8_t>& bytes) {
    const std::string path = Temp(suffix);
    WriteFile(path, bytes);
    MockProtocol protocol;
    pcapfile::PcapFileSourceConfig config;
    config.path = path;
    config.format = "pcapng";
    pcapfile::PcapFileChannel channel("pcapng_error", config, &protocol);
    assert(channel.Open() == 0);
    const auto event = channel.PollBlock(0);
    AssertNoBatchEvent(event, flowsql::BlockPollEvent::kError);
    assert(event.err != 0);
    const auto after_error = channel.PollBlock(0);
    AssertNoBatchEvent(after_error, flowsql::BlockPollEvent::kCancelled);
    assert(after_error.err == ECANCELED);
}

void TestRfc3339TimestampNs() {
    struct ValidCase {
        const char* text;
        int64_t expected;
    };
    const ValidCase valid[] = {
        {"1970-01-01T00:00:00Z", 0},
        {"1970-01-01t00:00:00z", 0},
        {"1970-01-01T00:00:00.123Z", 123000000},
        {"1970-01-01T00:00:00.123456Z", 123456000},
        {"1970-01-01T00:00:00.123456789Z", 123456789},
        {"2026-09-08T09:30:00.123456789+08:00", 1788831000123456789LL},
        {"2026-09-07T20:00:00.123-05:30", 1788831000123000000LL},
        {"2024-02-29T00:00:00+00:00", 1709164800000000000LL},
        {"1969-12-31T23:59:59.5Z", -500000000},
        {"2262-04-11T23:47:16.854775807Z", std::numeric_limits<int64_t>::max()},
        {"1677-09-21T00:12:43.145224192Z", std::numeric_limits<int64_t>::min()},
    };
    for (const auto& test : valid) {
        int64_t output = 1;
        std::string error = "stale";
        assert(pcapfile::ParseRfc3339TimestampNs(test.text, &output, &error) == 0);
        assert(output == test.expected);
        assert(error.empty());
    }

    const char* invalid[] = {
        "",
        "2026-09-08T01:30:00",
        "2026-09-08T01:30:00.",
        "2026-09-08T01:30:00.1234567890Z",
        "2026-09-08T01:30:60Z",
        "2023-02-29T00:00:00Z",
        "0000-01-01T00:00:00Z",
        "2026-13-01T00:00:00Z",
        "2026-09-08T24:00:00Z",
        "2026-09-08T01:60:00Z",
        "2026-09-08T01:30:00+24:00",
        "2026-09-08T01:30:00+00:60",
        "2026-09-08 01:30:00Z",
        "2026-09-08T01:30:00Europe/Shanghai",
        "2026-09-08T01:30:00Ztrailing",
    };
    for (const char* text : invalid) {
        int64_t output = 99;
        std::string error;
        assert(pcapfile::ParseRfc3339TimestampNs(text, &output, &error) == EINVAL);
        assert(output == 0);
        assert(!error.empty());
    }

    for (const char* text : {"2262-04-11T23:47:16.854775808Z",
                             "1677-09-21T00:12:43.145224191Z"}) {
        int64_t output = 99;
        std::string error;
        assert(pcapfile::ParseRfc3339TimestampNs(text, &output, &error) == EOVERFLOW);
        assert(output == 0);
        assert(!error.empty());
    }

    std::string error;
    assert(pcapfile::ParseRfc3339TimestampNs("1970-01-01T00:00:00Z", nullptr, &error) == EINVAL);
    assert(!error.empty());
}

void TestPacketDomainKeyCompilation() {
    auto same_ip = [](const packet::PacketIpKey& lhs, const packet::PacketIpKey& rhs) {
        return lhs.family == rhs.family && lhs.ipv4_network_order == rhs.ipv4_network_order &&
               lhs.ipv6 == rhs.ipv6;
    };
    auto same_endpoint = [&](const packet::PacketEndpointKey& lhs,
                             const packet::PacketEndpointKey& rhs) {
        return same_ip(lhs.address, rhs.address) && lhs.port == rhs.port;
    };
    auto same_pair = [&](const packet::TransportPairKey& lhs,
                         const packet::TransportPairKey& rhs) {
        return lhs.transport_protocol == rhs.transport_protocol && same_endpoint(lhs.first, rhs.first) &&
               same_endpoint(lhs.second, rhs.second);
    };

    std::string error = "stale";
    packet::PacketMacKey mac;
    assert(pcapfile::CompilePacketMacKey("00:11:22:aB:CD:ef", &mac, &error) == 0);
    assert(error.empty());
    const std::array<uint8_t, 6> expected_mac = {0x00, 0x11, 0x22, 0xab, 0xcd, 0xef};
    assert(mac.bytes == expected_mac);
    packet::PacketMacKey same_mac;
    assert(pcapfile::CompilePacketMacKey("00:11:22:AB:cd:EF", &same_mac, &error) == 0);
    assert(same_mac.bytes == mac.bytes);
    for (const char* invalid : {"00:11:22:33:44", "00:11:22:33:44:5g", "0:11:22:33:44:55",
                                "00-11-22-33-44-55"}) {
        mac.bytes.fill(0xff);
        assert(pcapfile::CompilePacketMacKey(invalid, &mac, &error) == EINVAL);
        assert(mac.bytes == packet::PacketMacKey{}.bytes && !error.empty());
    }

    packet::PacketIpKey ipv4;
    assert(pcapfile::CompilePacketIpKey("192.0.2.10", &ipv4, &error) == 0);
    assert(ipv4.family == packet::AddressFamily::kIPv4);
    assert(ipv4.ipv4_network_order == inet_addr("192.0.2.10"));
    assert(ipv4.ipv6 == packet::PacketIpKey{}.ipv6);

    packet::PacketIpKey ipv6;
    packet::PacketIpKey expanded_ipv6;
    assert(pcapfile::CompilePacketIpKey("2001:db8::20", &ipv6, &error) == 0);
    assert(pcapfile::CompilePacketIpKey("2001:0DB8:0:0:0:0:0:20", &expanded_ipv6, &error) == 0);
    assert(ipv6.family == packet::AddressFamily::kIPv6 && same_ip(ipv6, expanded_ipv6));
    assert(ipv6.ipv6[0] == 0x20 && ipv6.ipv6[1] == 0x01 && ipv6.ipv6[2] == 0x0d &&
           ipv6.ipv6[3] == 0xb8 && ipv6.ipv6[15] == 0x20);
    assert(ipv6.ipv4_network_order == 0);
    for (const char* invalid : {"", "192.0.2.999", "192.0.2.1/24", "2001:db8::1%eth0"}) {
        ipv4.family = packet::AddressFamily::kIPv6;
        ipv4.ipv4_network_order = 1;
        ipv4.ipv6.fill(1);
        assert(pcapfile::CompilePacketIpKey(invalid, &ipv4, &error) == EINVAL);
        assert(same_ip(ipv4, packet::PacketIpKey{}) && !error.empty());
    }

    uint16_t port = 1;
    assert(pcapfile::CompilePacketPort("0", &port, &error) == 0 && port == 0);
    assert(pcapfile::CompilePacketPort("65535", &port, &error) == 0 && port == 65535);
    for (const char* invalid : {"", "-1", "+1", "01x", "65536", "18446744073709551616"}) {
        port = 99;
        assert(pcapfile::CompilePacketPort(invalid, &port, &error) == EINVAL);
        assert(port == 0 && !error.empty());
    }

    packet::TransportPairKey tcp;
    packet::TransportPairKey reverse_tcp;
    packet::TransportPairKey crossed_ports;
    packet::TransportPairKey udp;
    assert(pcapfile::CompileTransportPairKey(
               "TcP", "192.0.2.10", "52314", "198.51.100.20", "3389", &tcp, &error) == 0);
    assert(pcapfile::CompileTransportPairKey(
               "tcp", "198.51.100.20", "3389", "192.0.2.10", "52314", &reverse_tcp, &error) == 0);
    assert(same_pair(tcp, reverse_tcp));
    assert(tcp.transport_protocol == 6 && tcp.first.address.ipv4_network_order == inet_addr("192.0.2.10") &&
           tcp.first.port == 52314 && tcp.second.address.ipv4_network_order == inet_addr("198.51.100.20") &&
           tcp.second.port == 3389);
    assert(pcapfile::CompileTransportPairKey(
               "tcp", "192.0.2.10", "3389", "198.51.100.20", "52314", &crossed_ports, &error) == 0);
    assert(!same_pair(tcp, crossed_ports));
    packet::TransportPairKey same_address;
    packet::TransportPairKey reverse_same_address;
    assert(pcapfile::CompileTransportPairKey(
               "tcp", "192.0.2.10", "443", "192.0.2.10", "80", &same_address, &error) == 0);
    assert(pcapfile::CompileTransportPairKey(
               "tcp", "192.0.2.10", "80", "192.0.2.10", "443", &reverse_same_address, &error) == 0);
    assert(same_pair(same_address, reverse_same_address) && same_address.first.port == 80 &&
           same_address.second.port == 443);
    assert(pcapfile::CompileTransportPairKey(
               "UDP", "2001:db8::20", "0", "2001:db8::10", "65535", &udp, &error) == 0);
    assert(udp.transport_protocol == 17 && udp.first.address.ipv6[15] == 0x10 &&
           udp.first.port == 65535 && udp.second.address.ipv6[15] == 0x20 && udp.second.port == 0);

    for (const auto& invalid : std::vector<std::array<std::string, 5>>{
             {"sctp", "192.0.2.1", "1", "192.0.2.2", "2"},
             {"tcp", "192.0.2.1", "1", "2001:db8::2", "2"},
             {"udp", "invalid", "1", "192.0.2.2", "2"},
             {"tcp", "192.0.2.1", "65536", "192.0.2.2", "2"}}) {
        tcp.transport_protocol = 99;
        assert(pcapfile::CompileTransportPairKey(invalid[0], invalid[1], invalid[2], invalid[3], invalid[4],
                                                 &tcp, &error) == EINVAL);
        assert(same_pair(tcp, packet::TransportPairKey{}) && !error.empty());
    }

    packet::PacketMacKey high_mac;
    assert(pcapfile::CompilePacketMacKey("ff:00:00:00:00:00", &high_mac, &error) == 0);
    packet::PacketIpKey high_ipv4;
    assert(pcapfile::CompilePacketIpKey("198.51.100.20", &high_ipv4, &error) == 0);
    packet::TransportPairKey noncanonical_tcp = reverse_tcp;
    std::swap(noncanonical_tcp.first, noncanonical_tcp.second);

    packet::PacketFilterRule root;
    root.kind = packet::PacketFilterRuleKind::kAnd;
    root.operands.resize(4);
    root.operands[0].kind = packet::PacketFilterRuleKind::kMacAnyOf;
    root.operands[0].mac_keys = {high_mac, same_mac, same_mac};
    root.operands[1].kind = packet::PacketFilterRuleKind::kIpAnyOf;
    root.operands[1].ip_keys = {ipv6, high_ipv4, ipv4, ipv4};
    root.operands[2].kind = packet::PacketFilterRuleKind::kPortAnyOf;
    root.operands[2].ports = {65535, 443, 0, 443};
    root.operands[3].kind = packet::PacketFilterRuleKind::kTransportPairAnyOf;
    root.operands[3].transport_pairs = {udp, noncanonical_tcp, reverse_tcp};
    pcapfile::CanonicalizePacketFilterRuleKeys(&root);
    assert(root.operands[0].mac_keys.size() == 2 && root.operands[0].mac_keys[0].bytes == same_mac.bytes &&
           root.operands[0].mac_keys[1].bytes == high_mac.bytes);
    assert(root.operands[1].ip_keys.size() == 3 && same_ip(root.operands[1].ip_keys[0], ipv4) &&
           same_ip(root.operands[1].ip_keys[1], high_ipv4) && same_ip(root.operands[1].ip_keys[2], ipv6));
    assert(root.operands[2].ports == std::vector<uint16_t>({0, 443, 65535}));
    assert(root.operands[3].transport_pairs.size() == 2 &&
           same_pair(root.operands[3].transport_pairs[0], reverse_tcp) &&
           same_pair(root.operands[3].transport_pairs[1], udp));
    pcapfile::CanonicalizePacketFilterRuleKeys(nullptr);
}

struct PcapCanonicalFixture {
    std::string canonical_json;
    std::shared_ptr<const flowsql::BoundFilterExpr> bound;
    flowsql::FilterPushdownSplit split;
};

PcapCanonicalFixture BuildPcapCanonicalFixture(const std::string& filter) {
    std::shared_ptr<flowsql::FilterExpr> parsed;
    std::string error;
    assert(flowsql::ParseFilterExpression(filter, &parsed, &error));
    assert(parsed && error.empty());

    pcapfile::PcapFilterDomainResolver resolver;
    flowsql::FilterDomainResolveRequestV1 request;
    request.target_kind = flowsql::FilterDomainTargetKindV1::kSource;
    request.target_category = "pcapfile";
    request.target_name = "capture";
    request.output_schema = packet::PacketSchema();
    request.expression = parsed;
    flowsql::FilterDomainResolveResultV1 resolved;
    assert(resolver.Resolve(request, &resolved) == 0);
    assert(resolved.lowered_expression && !resolved.diagnostic.empty());

    PcapCanonicalFixture fixture;
    assert(flowsql::BindFilterExpression(packet::PacketSchema(), resolved.lowered_expression,
                                         &fixture.bound, &error) ==
           flowsql::FilterBindError::kNone);
    assert(fixture.bound && error.empty());
    assert(flowsql::SplitFilterForPushdown(fixture.bound, {}, &fixture.split, &error) ==
           flowsql::FilterPlanError::kNone);
    assert(flowsql::BuildCanonicalFilterPlan(packet::PacketSchema(), fixture.bound,
                                             &fixture.canonical_json, &error) ==
           flowsql::FilterPlanError::kNone);
    assert(!fixture.canonical_json.empty() && error.empty());
    return fixture;
}

std::string BuildPcapCanonicalPlan(const std::string& filter) {
    return BuildPcapCanonicalFixture(filter).canonical_json;
}

std::shared_ptr<const packet::PcapFilterPlan> CompilePcapFilterPlan(
    const std::string& filter) {
    auto plan = std::make_shared<packet::PcapFilterPlan>();
    std::string error;
    assert(pcapfile::CompilePcapFilterPlanJson(
               BuildPcapCanonicalPlan(filter), plan.get(), &error) == 0);
    assert(error.empty());
    return plan;
}

void TestCanonicalPacketPlanCompilerAndPredicates() {
    auto compile = [](const std::string& filter) {
        packet::PcapFilterPlan plan;
        std::string error = "stale";
        assert(pcapfile::CompilePcapFilterPlanJson(BuildPcapCanonicalPlan(filter), &plan, &error) == 0);
        assert(error.empty() && plan.version == packet::kPcapFilterPlanVersion);
        return plan;
    };
    auto assign_ip = [](const std::string& text, packet::IpAddress* output) {
        packet::PacketIpKey key;
        std::string error;
        assert(pcapfile::CompilePacketIpKey(text, &key, &error) == 0);
        if (key.family == packet::AddressFamily::kIPv4) {
            *output = packet::IPv4Address(key.ipv4_network_order);
        } else {
            packet::IPv6Address value;
            std::copy(key.ipv6.begin(), key.ipv6.end(), value.bytes);
            *output = value;
        }
    };
    auto assign_mac = [](const std::string& text, packet::MacAddress* output) {
        packet::PacketMacKey key;
        std::string error;
        assert(pcapfile::CompilePacketMacKey(text, &key, &error) == 0);
        std::copy(key.bytes.begin(), key.bytes.end(), output->value.bytes);
        output->valid = 1;
    };

    packet::PacketMeta meta;
    packet::PacketLayerInfo layer;
    layer.status = packet::LayerStatus::kDecoded;

    packet::PcapFilterPlan empty;
    std::string error = "stale";
    assert(pcapfile::CompilePcapFilterPlanJson(
               flowsql::kEmptyCanonicalFilterPlanV1, &empty, &error) == 0);
    assert(error.empty());
    assert(pcapfile::EvaluatePcapHeaderFilter(empty, meta) ==
           pcapfile::PacketHeaderFilterResult::kMatch);
    assert(pcapfile::EvaluatePcapDecodedFilter(empty, meta, layer));
    packet::PcapFilterPlan match_all;
    packet::PcapFilterPlan match_none;
    assert(pcapfile::CompilePcapFilterPlanJson(
               R"({"version":1,"root":{"node_id":1,"kind":"literal","type":"bool","nullable":false,"value":"true"}})",
               &match_all, &error) == 0);
    assert(pcapfile::CompilePcapFilterPlanJson(
               R"({"version":1,"root":{"node_id":1,"kind":"literal","type":"bool","nullable":false,"value":"false"}})",
               &match_none, &error) == 0);
    assert(pcapfile::EvaluatePcapHeaderFilter(match_all, meta) ==
           pcapfile::PacketHeaderFilterResult::kMatch);
    assert(pcapfile::EvaluatePcapHeaderFilter(match_none, meta) ==
           pcapfile::PacketHeaderFilterResult::kReject);

    const auto time = compile("timestamp_ns >= 10 AND timestamp_ns < 20");
    meta.timestamp_ns = 9;
    assert(pcapfile::EvaluatePcapHeaderFilter(time, meta) ==
           pcapfile::PacketHeaderFilterResult::kReject);
    meta.timestamp_ns = 10;
    assert(pcapfile::EvaluatePcapHeaderFilter(time, meta) ==
           pcapfile::PacketHeaderFilterResult::kMatch);
    meta.timestamp_ns = 19;
    assert(pcapfile::EvaluatePcapDecodedFilter(time, meta, layer));
    meta.timestamp_ns = 20;
    assert(!pcapfile::EvaluatePcapDecodedFilter(time, meta, layer));
    meta.timestamp_ns = 5;
    assert(pcapfile::EvaluatePcapHeaderFilter(time, meta) ==
           pcapfile::PacketHeaderFilterResult::kReject);
    const auto time_between = compile("timestamp_ns BETWEEN 10 AND 20");
    meta.timestamp_ns = 20;
    assert(pcapfile::EvaluatePcapHeaderFilter(time_between, meta) ==
           pcapfile::PacketHeaderFilterResult::kMatch);
    meta.timestamp_ns = 21;
    assert(pcapfile::EvaluatePcapHeaderFilter(time_between, meta) ==
           pcapfile::PacketHeaderFilterResult::kReject);

    const auto unsigned_fields = compile(
        "captured_len >= 4 AND wire_len <= 10 AND source_id = 7 AND sequence != 9");
    meta.captured_len = 4;
    meta.wire_len = 10;
    meta.source_id = 7;
    meta.sequence = 8;
    assert(pcapfile::EvaluatePcapHeaderFilter(unsigned_fields, meta) ==
           pcapfile::PacketHeaderFilterResult::kMatch);
    meta.sequence = 9;
    assert(pcapfile::EvaluatePcapHeaderFilter(unsigned_fields, meta) ==
           pcapfile::PacketHeaderFilterResult::kReject);
    const auto captured_between = compile("captured_len BETWEEN 4 AND 10");
    meta.captured_len = 10;
    assert(pcapfile::EvaluatePcapHeaderFilter(captured_between, meta) ==
           pcapfile::PacketHeaderFilterResult::kMatch);
    meta.captured_len = 11;
    assert(pcapfile::EvaluatePcapHeaderFilter(captured_between, meta) ==
           pcapfile::PacketHeaderFilterResult::kReject);

    const auto mac = compile("mac('00:11:22:33:44:55')");
    meta = {};
    assert(pcapfile::EvaluatePcapHeaderFilter(mac, meta) ==
           pcapfile::PacketHeaderFilterResult::kNeedDecoded);
    assign_mac("00:11:22:33:44:55", &layer.src_mac);
    assert(pcapfile::EvaluatePcapDecodedFilter(mac, meta, layer));
    layer.src_mac.valid = 0;
    assert(!pcapfile::EvaluatePcapDecodedFilter(mac, meta, layer));
    assign_mac("00:11:22:33:44:55", &layer.dst_mac);
    assert(pcapfile::EvaluatePcapDecodedFilter(mac, meta, layer));

    const auto ipv4 = compile("ip('192.0.2.10')");
    layer = {};
    layer.status = packet::LayerStatus::kDecoded;
    assign_ip("192.0.2.10", &layer.src_ip);
    assert(pcapfile::EvaluatePcapDecodedFilter(ipv4, meta, layer));
    assign_ip("2001:db8::10", &layer.src_ip);
    assert(!pcapfile::EvaluatePcapDecodedFilter(ipv4, meta, layer));
    const auto ipv6 = compile("ip('2001:db8::10')");
    assert(pcapfile::EvaluatePcapDecodedFilter(ipv6, meta, layer));

    const auto port = compile("port(443)");
    layer.src_port = 443;
    layer.ports_valid = 0;
    assert(!pcapfile::EvaluatePcapDecodedFilter(port, meta, layer));
    layer.ports_valid = 1;
    assert(pcapfile::EvaluatePcapDecodedFilter(port, meta, layer));

    const auto tcp = compile("tcp('192.0.2.10', 52314, '198.51.100.20', 3389)");
    layer = {};
    layer.status = packet::LayerStatus::kDecoded;
    layer.transport_protocol = 6;
    layer.ports_valid = 1;
    layer.src_port = 52314;
    layer.dst_port = 3389;
    assign_ip("192.0.2.10", &layer.src_ip);
    assign_ip("198.51.100.20", &layer.dst_ip);
    assert(pcapfile::EvaluatePcapDecodedFilter(tcp, meta, layer));
    std::swap(layer.src_ip, layer.dst_ip);
    std::swap(layer.src_port, layer.dst_port);
    assert(pcapfile::EvaluatePcapDecodedFilter(tcp, meta, layer));
    layer.src_port = 52314;
    layer.dst_port = 3389;
    assert(!pcapfile::EvaluatePcapDecodedFilter(tcp, meta, layer));
    layer.ports_valid = 0;
    assert(!pcapfile::EvaluatePcapDecodedFilter(tcp, meta, layer));
    layer.ports_valid = 1;
    layer.transport_protocol = 17;
    assert(!pcapfile::EvaluatePcapDecodedFilter(tcp, meta, layer));
    layer.transport_protocol = 6;
    layer.src_ip = std::monostate{};
    assert(!pcapfile::EvaluatePcapDecodedFilter(tcp, meta, layer));
    layer.status = packet::LayerStatus::kTruncated;
    assign_ip("198.51.100.20", &layer.src_ip);
    assert(!pcapfile::EvaluatePcapDecodedFilter(tcp, meta, layer));

    const auto udp = compile("udp('2001:db8::10', 53, '2001:db8::20', 5353)");
    layer = {};
    layer.status = packet::LayerStatus::kDecoded;
    layer.transport_protocol = 17;
    layer.ports_valid = 1;
    layer.src_port = 5353;
    layer.dst_port = 53;
    assign_ip("2001:db8::20", &layer.src_ip);
    assign_ip("2001:db8::10", &layer.dst_ip);
    assert(pcapfile::EvaluatePcapDecodedFilter(udp, meta, layer));

    const auto and_plan = compile("timestamp_ns >= 10 AND mac('00:11:22:33:44:55')");
    meta.timestamp_ns = 9;
    assert(pcapfile::EvaluatePcapHeaderFilter(and_plan, meta) ==
           pcapfile::PacketHeaderFilterResult::kReject);
    meta.timestamp_ns = 10;
    assert(pcapfile::EvaluatePcapHeaderFilter(and_plan, meta) ==
           pcapfile::PacketHeaderFilterResult::kNeedDecoded);
    layer = {};
    layer.status = packet::LayerStatus::kDecoded;
    assign_mac("00:11:22:33:44:55", &layer.src_mac);
    assert(pcapfile::EvaluatePcapDecodedFilter(and_plan, meta, layer));
    meta.timestamp_ns = 9;
    assert(!pcapfile::EvaluatePcapDecodedFilter(and_plan, meta, layer));
    const auto or_plan = compile("timestamp_ns >= 10 OR mac('00:11:22:33:44:55')");
    meta.timestamp_ns = 10;
    assert(pcapfile::EvaluatePcapHeaderFilter(or_plan, meta) ==
           pcapfile::PacketHeaderFilterResult::kMatch);
    meta.timestamp_ns = 9;
    assert(pcapfile::EvaluatePcapHeaderFilter(or_plan, meta) ==
           pcapfile::PacketHeaderFilterResult::kNeedDecoded);
    assert(pcapfile::EvaluatePcapDecodedFilter(or_plan, meta, layer));
    layer.src_mac.valid = 0;
    assert(!pcapfile::EvaluatePcapDecodedFilter(or_plan, meta, layer));
    const auto not_plan = compile("NOT mac('00:11:22:33:44:55')");
    assert(pcapfile::EvaluatePcapHeaderFilter(not_plan, meta) ==
           pcapfile::PacketHeaderFilterResult::kNeedDecoded);
    assign_mac("00:11:22:33:44:55", &layer.dst_mac);
    assert(!pcapfile::EvaluatePcapDecodedFilter(not_plan, meta, layer));
    assign_mac("aa:bb:cc:dd:ee:ff", &layer.dst_mac);
    assert(pcapfile::EvaluatePcapDecodedFilter(not_plan, meta, layer));

    packet::PcapFilterPlan rejected;
    for (const std::string& invalid : {
             std::string("{"), std::string("{\"version\":2,\"root\":null}"),
             std::string("{\"version\":1,\"root\":{}}"),
             BuildPcapCanonicalPlan("src_port = 443"),
             BuildPcapCanonicalPlan("transport_protocol = 6"),
             BuildPcapCanonicalPlan("src_ip_v4 = 1")}) {
        error.clear();
        assert(pcapfile::CompilePcapFilterPlanJson(invalid, &rejected, &error) == EINVAL);
        assert(!error.empty());
    }
    assert(pcapfile::CompilePcapFilterPlanJson(
               flowsql::kEmptyCanonicalFilterPlanV1, nullptr, &error) == EINVAL);
}

void TestPcapFilterCandidateSelection() {
    auto select = [](const PcapCanonicalFixture& fixture,
                     const std::vector<uint32_t>& candidates) {
        std::vector<uint32_t> accepted = {std::numeric_limits<uint32_t>::max()};
        std::string error = "stale";
        assert(pcapfile::SelectCompilablePcapFilterNodes(
                   fixture.canonical_json, candidates, &accepted, &error) == 0);
        assert(error.empty());
        return accepted;
    };

    const auto mixed = BuildPcapCanonicalFixture(
        "timestamp_ns >= 10 AND src_port = 443 AND captured_len <= 64");
    assert(mixed.split.candidate_node_ids.size() == 3);
    const std::vector<uint32_t> requested = {
        mixed.split.candidate_node_ids[2], mixed.split.candidate_node_ids[1],
        mixed.split.candidate_node_ids[0]};
    const auto requested_copy = requested;
    const auto accepted = select(mixed, requested);
    assert(requested == requested_copy);
    assert((accepted == std::vector<uint32_t>{mixed.split.candidate_node_ids[2],
                                              mixed.split.candidate_node_ids[0]}));

    const auto decoded = BuildPcapCanonicalFixture("mac('00:11:22:33:44:55')");
    assert(decoded.split.candidate_node_ids.size() == 1);
    assert(select(decoded, decoded.split.candidate_node_ids) == decoded.split.candidate_node_ids);

    const auto exact_or = BuildPcapCanonicalFixture(
        "timestamp_ns >= 10 OR captured_len <= 64");
    assert(exact_or.split.candidate_node_ids.size() == 1);
    assert(select(exact_or, exact_or.split.candidate_node_ids) ==
           exact_or.split.candidate_node_ids);
    const auto exact_not = BuildPcapCanonicalFixture(
        "NOT mac('00:11:22:33:44:55')");
    assert(exact_not.split.candidate_node_ids.size() == 1);
    assert(select(exact_not, exact_not.split.candidate_node_ids) ==
           exact_not.split.candidate_node_ids);

    const auto inexact_or = BuildPcapCanonicalFixture(
        "timestamp_ns >= 10 OR src_port = 443");
    assert(inexact_or.split.candidate_node_ids.size() == 1);
    assert(select(inexact_or, inexact_or.split.candidate_node_ids).empty());
    assert(select(inexact_or, {}).empty());

    std::vector<uint32_t> rejected = {123};
    std::string error = "stale";
    assert(pcapfile::SelectCompilablePcapFilterNodes(
               flowsql::kEmptyCanonicalFilterPlanV1, {}, &rejected, &error) == 0);
    assert(rejected.empty() && error.empty());
    assert(pcapfile::SelectCompilablePcapFilterNodes(
               flowsql::kEmptyCanonicalFilterPlanV1, {1}, &rejected, &error) == EINVAL);
    assert(rejected.empty() && !error.empty());

    error = "stale";
    assert(pcapfile::SelectCompilablePcapFilterNodes(
               mixed.canonical_json,
               {mixed.split.candidate_node_ids[0], 0}, &rejected, &error) == EINVAL);
    assert(rejected.empty() && !error.empty());
    error = "stale";
    assert(pcapfile::SelectCompilablePcapFilterNodes(
               mixed.canonical_json,
               {mixed.split.candidate_node_ids[0], mixed.split.candidate_node_ids[0]},
               &rejected, &error) == EINVAL);
    assert(rejected.empty() && !error.empty());

    const std::string duplicate_node_ids =
        R"({"version":1,"root":{"node_id":1,"kind":"and","type":"bool","nullable":false,)"
        R"("operands":[{"node_id":2,"kind":"literal","type":"bool","nullable":false,)"
        R"("value":"true"},{"node_id":2,"kind":"literal","type":"bool","nullable":false,)"
        R"("value":"false"}]}})";
    for (const std::string& malformed : {std::string("{"), duplicate_node_ids}) {
        rejected = {123};
        error = "stale";
        assert(pcapfile::SelectCompilablePcapFilterNodes(
                   malformed, {}, &rejected, &error) == EINVAL);
        assert(rejected.empty() && !error.empty());
    }
    assert(pcapfile::SelectCompilablePcapFilterNodes(
               mixed.canonical_json, mixed.split.candidate_node_ids, nullptr, &error) == EINVAL);
    assert(!error.empty());
}

void TestPcapFilterPushdownCapability() {
    const std::string path = Temp("filter_pushdown_capability.pcap");
    WriteFile(path, MakeClassicPcap(true, false, 0));

    MockProtocol protocol;
    DeferredProtocolQuerier querier;
    querier.SetProtocol(&protocol);
    pcapfile::PcapFilePlugin plugin;
    assert(plugin.Load(&querier) == 0);
    assert(plugin.Start() == 0);
    assert(plugin.AddChannel(
               "pcapfile", "pushdown",
               "{\"path\":\"" + path + "\"}") == 0);

    const auto mixed = BuildPcapCanonicalFixture(
        "timestamp_ns >= 10 AND src_port = 443 AND captured_len <= 64");
    assert(mixed.split.candidate_node_ids.size() == 3);
    flowsql::FilterPushdownRequestV1 request;
    request.target_kind = flowsql::FilterPushdownTargetKindV1::kSource;
    request.target_category = "PCAPFILE";
    request.target_name = "pushdown";
    request.output_schema = packet::PacketSchema();
    request.canonical_plan_json = mixed.canonical_json.c_str();
    request.candidate_node_ids = {mixed.split.candidate_node_ids[2],
                                  mixed.split.candidate_node_ids[1],
                                  mixed.split.candidate_node_ids[0]};
    const auto candidates_copy = request.candidate_node_ids;
    flowsql::FilterPushdownResultV1 result;
    result.accepted_node_ids = {std::numeric_limits<uint32_t>::max()};
    result.diagnostic = "stale";
    assert(plugin.EvaluatePushdown(request, &result) == 0);
    assert((result.accepted_node_ids ==
            std::vector<uint32_t>{mixed.split.candidate_node_ids[2],
                                  mixed.split.candidate_node_ids[0]}));
    assert(!result.diagnostic.empty());
    assert(request.candidate_node_ids == candidates_copy);
    assert(request.canonical_plan_json == mixed.canonical_json.c_str());
    assert(request.output_schema == packet::PacketSchema());

    const auto unsupported = BuildPcapCanonicalFixture("src_port = 443");
    request.canonical_plan_json = unsupported.canonical_json.c_str();
    request.candidate_node_ids = unsupported.split.candidate_node_ids;
    assert(plugin.EvaluatePushdown(request, &result) == 0);
    assert(result.accepted_node_ids.empty() && !result.diagnostic.empty());

    request.target_kind = flowsql::FilterPushdownTargetKindV1::kTransform;
    result.accepted_node_ids = {1};
    result.diagnostic = "stale";
    assert(plugin.EvaluatePushdown(request, &result) == ENOTSUP);
    assert(result.accepted_node_ids.empty() && result.diagnostic.empty());
    request.target_kind = flowsql::FilterPushdownTargetKindV1::kSource;
    request.target_category = "other";
    result.accepted_node_ids = {1};
    assert(plugin.EvaluatePushdown(request, &result) == ENOTSUP);
    assert(result.accepted_node_ids.empty());
    request.target_category = "pcapfile";
    request.target_name = "not-owned";
    result.accepted_node_ids = {1};
    assert(plugin.EvaluatePushdown(request, &result) == ENOTSUP);
    assert(result.accepted_node_ids.empty());

    request.target_name = "pushdown";
    request.canonical_plan_json = mixed.canonical_json.c_str();
    request.candidate_node_ids = mixed.split.candidate_node_ids;
    request.contract_version = flowsql::kFilterPushdownContractVersionV1 + 1;
    assert(plugin.EvaluatePushdown(request, &result) == EINVAL);
    assert(result.accepted_node_ids.empty() && !result.diagnostic.empty());
    request.contract_version = flowsql::kFilterPushdownContractVersionV1;
    request.output_schema = arrow::schema({arrow::field("value", arrow::int64())});
    assert(plugin.EvaluatePushdown(request, &result) == EINVAL);
    assert(result.accepted_node_ids.empty() && !result.diagnostic.empty());
    request.output_schema = packet::PacketSchema();
    request.canonical_plan_json = "{";
    assert(plugin.EvaluatePushdown(request, &result) == EINVAL);
    assert(result.accepted_node_ids.empty() && !result.diagnostic.empty());
    request.canonical_plan_json = mixed.canonical_json.c_str();
    request.candidate_node_ids = {0};
    assert(plugin.EvaluatePushdown(request, &result) == EINVAL);
    assert(result.accepted_node_ids.empty() && !result.diagnostic.empty());
    request.candidate_node_ids = {mixed.split.candidate_node_ids[0],
                                  mixed.split.candidate_node_ids[0]};
    assert(plugin.EvaluatePushdown(request, &result) == EINVAL);
    assert(result.accepted_node_ids.empty() && !result.diagnostic.empty());
    assert(plugin.EvaluatePushdown(request, nullptr) == EINVAL);

    request.target_category = nullptr;
    assert(plugin.EvaluatePushdown(request, &result) == EINVAL);
    assert(result.accepted_node_ids.empty() && !result.diagnostic.empty());
    request.target_category = "pcapfile";
    request.target_name = "";
    assert(plugin.EvaluatePushdown(request, &result) == EINVAL);
    assert(result.accepted_node_ids.empty() && !result.diagnostic.empty());
    request.target_name = "pushdown";
    request.canonical_plan_json = nullptr;
    assert(plugin.EvaluatePushdown(request, &result) == EINVAL);
    assert(result.accepted_node_ids.empty() && !result.diagnostic.empty());

    assert(plugin.RemoveChannel("pcapfile", "pushdown") == 0);
    assert(plugin.Stop() == 0);
    assert(plugin.Unload() == 0);
}

void TestClassicPcapTwoStageFilterDataPath() {
    const auto target = MakeEthernetPacket(true);
    const auto other = MakeEthernetPacket(false);
    std::vector<uint8_t> bytes = MakeClassicHeader(true, false, 65535, 1);
    AppendClassicRecord(&bytes, true, 3, 0, target, target.size());
    AppendClassicRecord(&bytes, true, 1, 0, other, other.size());
    AppendClassicRecord(&bytes, true, 2, 0, target, target.size());
    AppendClassicRecord(&bytes, true, 2, 1, target, target.size());
    const std::string path = Temp("classic_two_stage_filter.pcap");
    WriteFile(path, bytes);

    MockProtocol protocol;
    pcapfile::PcapFileSourceConfig config;
    config.path = path;
    config.format = "pcap";
    config.batch_packets = 2;
    pcapfile::PcapFileChannel channel(
        "classic_two_stage_filter", config, &protocol, {},
        CompilePcapFilterPlan(
            "timestamp_ns < 2500000000 AND mac('aa:bb:cc:dd:ee:ff')"));
    assert(channel.Open() == 0);
    const auto data = channel.PollBlock(0);
    assert(data.kind == flowsql::BlockPollEvent::kData && data.batch);
    assert(data.batch->num_rows() == 2);
    auto timestamp = std::static_pointer_cast<arrow::Int64Array>(data.batch->column(0));
    auto sequence = std::static_pointer_cast<arrow::UInt64Array>(data.batch->column(5));
    assert(timestamp->Value(0) == 2000000000LL);
    assert(timestamp->Value(1) == 2000001000LL);
    assert(sequence->Value(0) == 2 && sequence->Value(1) == 3);
    assert(protocol.layer_calls == 3);
    assert(protocol.identify_calls == 0);
    assert(channel.OutstandingBatchCount() == 1);
    AssertNoBatchEvent(channel.PollBlock(0), flowsql::BlockPollEvent::kTimeout);
    assert(channel.ReleaseBlock(data.batch) == 0);
    AssertNoBatchEvent(channel.PollBlock(0), flowsql::BlockPollEvent::kEof);
    AssertNoBatchEvent(channel.PollBlock(0), flowsql::BlockPollEvent::kCancelled);
}

void TestPcapngHeaderFilterDataPath() {
    std::vector<uint8_t> bytes;
    AppendPcapngSectionHeader(&bytes, true);
    AppendPcapngInterface(&bytes, true, 1);
    AppendPcapngEnhancedPacket(&bytes, true, 0, 1, MakeEthernetPacket(false));
    AppendPcapngEnhancedPacket(&bytes, true, 0, 2, MakeEthernetPacket(true));
    const std::string path = Temp("pcapng_header_filter.pcapng");
    WriteFile(path, bytes);

    MockProtocol protocol;
    pcapfile::PcapFileSourceConfig config;
    config.path = path;
    config.format = "pcapng";
    config.batch_packets = 8;
    pcapfile::PcapFileChannel channel(
        "pcapng_header_filter", config, &protocol, {},
        CompilePcapFilterPlan("timestamp_ns >= 2000"));
    assert(channel.Open() == 0);
    const auto data = channel.PollBlock(0);
    assert(data.kind == flowsql::BlockPollEvent::kData && data.batch);
    assert(data.batch->num_rows() == 1);
    auto timestamp = std::static_pointer_cast<arrow::Int64Array>(data.batch->column(0));
    auto sequence = std::static_pointer_cast<arrow::UInt64Array>(data.batch->column(5));
    assert(timestamp->Value(0) == 2000 && sequence->Value(0) == 1);
    assert(protocol.layer_calls == 1 && protocol.identify_calls == 0);
    assert(channel.ReleaseBlock(data.batch) == 0);
    AssertNoBatchEvent(channel.PollBlock(0), flowsql::BlockPollEvent::kEof);
}

void TestFilteredEmptyAndSkippedErrorTerminals() {
    std::vector<uint8_t> classic = MakeClassicHeader(true, false, 65535, 1);
    const auto other = MakeEthernetPacket(false);
    AppendClassicRecord(&classic, true, 1, 0, other, other.size());
    AppendClassicRecord(&classic, true, 2, 0, other, other.size());
    const std::string classic_path = Temp("decoded_filter_empty.pcap");
    WriteFile(classic_path, classic);

    MockProtocol decoded_protocol;
    pcapfile::PcapFileSourceConfig classic_config;
    classic_config.path = classic_path;
    classic_config.format = "pcap";
    pcapfile::PcapFileChannel decoded_channel(
        "decoded_filter_empty", classic_config, &decoded_protocol, {},
        CompilePcapFilterPlan("mac('aa:bb:cc:dd:ee:ff')"));
    assert(decoded_channel.Open() == 0);
    AssertNoBatchEvent(decoded_channel.PollBlock(0), flowsql::BlockPollEvent::kEof);
    assert(decoded_channel.OutstandingBatchCount() == 0);
    assert(decoded_protocol.layer_calls == 2 && decoded_protocol.identify_calls == 0);

    std::vector<uint8_t> pcapng;
    AppendPcapngSectionHeader(&pcapng, true);
    AppendPcapngInterface(&pcapng, true, 1);
    AppendPcapngEnhancedPacket(&pcapng, true, 0, 1, MakeEthernetPacket(true));
    Store32(&pcapng, pcapng.size() - 4, 0, true);
    const std::string pcapng_path = Temp("skipped_bad_trailer.pcapng");
    WriteFile(pcapng_path, pcapng);

    MockProtocol skipped_protocol;
    pcapfile::PcapFileSourceConfig pcapng_config;
    pcapng_config.path = pcapng_path;
    pcapng_config.format = "pcapng";
    pcapfile::PcapFileChannel skipped_channel(
        "skipped_bad_trailer", pcapng_config, &skipped_protocol, {},
        CompilePcapFilterPlan("timestamp_ns >= 2000"));
    assert(skipped_channel.Open() == 0);
    const auto error = skipped_channel.PollBlock(0);
    AssertNoBatchEvent(error, flowsql::BlockPollEvent::kError);
    assert(error.err != 0);
    assert(skipped_protocol.layer_calls == 0 && skipped_protocol.identify_calls == 0);
    AssertNoBatchEvent(skipped_channel.PollBlock(0),
                       flowsql::BlockPollEvent::kCancelled);
}

void TestFilteredTimestampReplay() {
    const std::vector<uint64_t> timestamps_ns = {
        1000000000ULL, 1006001001ULL, 1006001002ULL};
    const std::string path = Temp("filtered_timestamp_replay.pcap");
    WriteFile(path, MakeNanosecondReplayPcap(timestamps_ns));

    MockProtocol protocol;
    pcapfile::PcapFileSourceConfig config;
    config.path = path;
    config.format = "pcap";
    config.replay_mode = pcapfile::PcapReplayMode::kTimestamp;
    std::vector<int64_t> waits_ns;
    pcapfile::PcapFileChannel channel(
        "filtered_timestamp_replay", config, &protocol,
        [&](std::chrono::nanoseconds delay) { waits_ns.push_back(delay.count()); },
        CompilePcapFilterPlan("sequence >= 2"));
    assert(channel.Open() == 0);
    const auto data = channel.PollBlock(0);
    assert(data.kind == flowsql::BlockPollEvent::kData && data.batch);
    assert(data.batch->num_rows() == 1);
    auto sequence = std::static_pointer_cast<arrow::UInt64Array>(data.batch->column(5));
    assert(sequence->Value(0) == 2);
    assert(waits_ns == std::vector<int64_t>{6001002});
    assert(protocol.layer_calls == 1 && protocol.identify_calls == 0);
    assert(channel.ReleaseBlock(data.batch) == 0);
    AssertNoBatchEvent(channel.PollBlock(0), flowsql::BlockPollEvent::kEof);
}

void TestPcapFilterDomainResolverResidual() {
    pcapfile::PcapFilterDomainResolver resolver;
    const auto schema = packet::PacketSchema();

    auto parse = [](const std::string& text) {
        std::shared_ptr<flowsql::FilterExpr> expression;
        std::string error;
        assert(flowsql::ParseFilterExpression(text, &expression, &error));
        assert(expression && error.empty());
        return expression;
    };
    auto resolve = [&](const std::shared_ptr<flowsql::FilterExpr>& expression) {
        flowsql::FilterDomainResolveRequestV1 request;
        request.target_kind = flowsql::FilterDomainTargetKindV1::kSource;
        request.target_category = "pcapfile";
        request.target_name = "capture";
        request.output_schema = schema;
        request.expression = expression;
        flowsql::FilterDomainResolveResultV1 result;
        assert(resolver.Resolve(request, &result) == 0);
        assert(result.lowered_expression && !result.diagnostic.empty());
        return result.lowered_expression;
    };
    auto bind = [&](const std::string& text) {
        auto lowered = resolve(parse(text));
        std::shared_ptr<const flowsql::BoundFilterExpr> bound;
        std::string error;
        assert(flowsql::BindFilterExpression(schema, lowered, &bound, &error) ==
               flowsql::FilterBindError::kNone);
        assert(bound && error.empty());
        return bound;
    };

    const std::string main_filter =
        "timestamp_ns >= TIMESTAMP '2026-09-08T09:30:00.123456789+08:00' AND "
        "timestamp_ns < TIMESTAMP '2026-09-08T01:30:00.123456793Z' AND "
        "tcp('192.0.2.10', 52314, '198.51.100.20', 3389)";
    auto original = parse(main_filter);
    const uint32_t original_root_id = original->node_id;
    bool original_has_call = false;
    bool original_has_typed = false;
    std::set<uint32_t> replaced_root_ids;
    std::function<void(const std::shared_ptr<const flowsql::FilterExpr>&)> inspect_original;
    inspect_original = [&](const std::shared_ptr<const flowsql::FilterExpr>& node) {
        assert(node);
        original_has_call = original_has_call || node->kind == flowsql::FilterExprKind::kCall;
        original_has_typed = original_has_typed ||
                             (node->kind == flowsql::FilterExprKind::kLiteral &&
                              node->literal.kind == flowsql::FilterLiteralKind::kTyped);
        if (node->kind == flowsql::FilterExprKind::kCall ||
            (node->kind == flowsql::FilterExprKind::kLiteral &&
             node->literal.kind == flowsql::FilterLiteralKind::kTyped)) {
            replaced_root_ids.insert(node->node_id);
        }
        for (const auto& operand : node->operands) inspect_original(operand);
    };
    inspect_original(original);
    assert(original_has_call && original_has_typed);

    auto lowered = resolve(original);
    assert(lowered.get() != original.get() && lowered->node_id == original_root_id);
    original_has_call = false;
    original_has_typed = false;
    inspect_original(original);
    assert(original_has_call && original_has_typed);

    std::set<uint32_t> lowered_ids;
    size_t synthetic_count = 0;
    std::function<void(const std::shared_ptr<const flowsql::FilterExpr>&)> inspect_lowered;
    inspect_lowered = [&](const std::shared_ptr<const flowsql::FilterExpr>& node) {
        assert(node && node->kind != flowsql::FilterExprKind::kCall);
        assert(node->kind != flowsql::FilterExprKind::kLiteral ||
               node->literal.kind != flowsql::FilterLiteralKind::kTyped);
        assert(node->node_id != 0 && lowered_ids.insert(node->node_id).second);
        if (node->node_id >= flowsql::kFilterDomainSyntheticNodeIdBaseV1) ++synthetic_count;
        for (const auto& operand : node->operands) inspect_lowered(operand);
    };
    inspect_lowered(lowered);
    assert(synthetic_count != 0);
    for (uint32_t node_id : replaced_root_ids) assert(lowered_ids.count(node_id) == 1);

    std::vector<std::shared_ptr<std::vector<uint8_t>>> byte_owners;
    std::vector<packet::PacketRecord> records;
    auto add_record = [&](int64_t timestamp_ns, uint64_t sequence, const std::string& src_ip,
                          uint16_t src_port, const std::string& dst_ip, uint16_t dst_port,
                          uint8_t protocol, bool ports_valid, const std::string& src_mac,
                          const std::string& dst_mac) {
        auto bytes = std::make_shared<std::vector<uint8_t>>(1, static_cast<uint8_t>(sequence));
        byte_owners.push_back(bytes);

        packet::PacketRecord record;
        record.meta.timestamp_ns = timestamp_ns;
        record.meta.captured_len = 1;
        record.meta.wire_len = 1;
        record.meta.link_type = 1;
        record.meta.sequence = sequence;
        record.raw_data.owner = bytes;
        record.raw_data.data = bytes->data();
        record.raw_data.size = 1;
        record.layer.status = packet::LayerStatus::kDecoded;
        record.layer.endpoint_scope = packet::EndpointScope::kInnermost;
        record.layer.transport_protocol = protocol;
        record.layer.src_port = src_port;
        record.layer.dst_port = dst_port;
        record.layer.ports_valid = ports_valid;

        auto assign_ip = [](const std::string& text, packet::IpAddress* output) {
            packet::PacketIpKey key;
            std::string error;
            assert(pcapfile::CompilePacketIpKey(text, &key, &error) == 0);
            if (key.family == packet::AddressFamily::kIPv4) {
                *output = packet::IPv4Address(key.ipv4_network_order);
            } else {
                packet::IPv6Address value;
                std::copy(key.ipv6.begin(), key.ipv6.end(), value.bytes);
                *output = value;
            }
        };
        assign_ip(src_ip, &record.layer.src_ip);
        assign_ip(dst_ip, &record.layer.dst_ip);

        auto assign_mac = [](const std::string& text, packet::MacAddress* output) {
            packet::PacketMacKey key;
            std::string error;
            assert(pcapfile::CompilePacketMacKey(text, &key, &error) == 0);
            std::copy(key.bytes.begin(), key.bytes.end(), output->value.bytes);
            output->valid = 1;
        };
        assign_mac(src_mac, &record.layer.src_mac);
        assign_mac(dst_mac, &record.layer.dst_mac);
        records.push_back(std::move(record));
    };

    constexpr int64_t kStart = 1788831000123456789LL;
    const char* target_mac = "00:11:22:33:44:55";
    const char* other_mac = "aa:bb:cc:dd:ee:ff";
    add_record(kStart, 1, "192.0.2.10", 52314, "198.51.100.20", 3389, 6, true,
               target_mac, other_mac);
    add_record(kStart + 1, 2, "198.51.100.20", 3389, "192.0.2.10", 52314, 6, true,
               other_mac, target_mac);
    add_record(kStart + 2, 3, "192.0.2.10", 3389, "198.51.100.20", 52314, 6, true,
               other_mac, other_mac);
    add_record(kStart + 3, 4, "192.0.2.10", 52314, "198.51.100.20", 3389, 6, false,
               other_mac, other_mac);
    add_record(kStart + 4, 5, "2001:db8::20", 0, "2001:db8::10", 65535, 17, true,
               other_mac, other_mac);

    std::shared_ptr<arrow::RecordBatch> batch;
    std::string error;
    assert(packet::EncodePacketBatch(records, &batch, &error) == packet::PacketBatchError::kNone);
    assert(batch && batch->num_rows() == 5 && error.empty());

    auto expect_sequences = [&](const std::string& text, const std::vector<uint64_t>& expected) {
        std::shared_ptr<arrow::RecordBatch> filtered;
        assert(flowsql::FilterRecordBatch(batch, bind(text), &filtered, &error) ==
               flowsql::FilterEvalError::kNone);
        assert(filtered && error.empty() && filtered->num_rows() == static_cast<int64_t>(expected.size()));
        auto sequence = std::static_pointer_cast<arrow::UInt64Array>(
            filtered->GetColumnByName("sequence"));
        for (size_t index = 0; index < expected.size(); ++index) {
            assert(sequence->Value(static_cast<int64_t>(index)) == expected[index]);
        }
    };
    expect_sequences(main_filter, {1, 2});
    expect_sequences("mac('00:11:22:33:44:55')", {1, 2});
    expect_sequences("ip('198.51.100.20')", {1, 2, 3, 4});
    expect_sequences("ip('2001:db8::10')", {5});
    expect_sequences("port(3389)", {1, 2, 3});
    expect_sequences("tcp('192.0.2.10', 52314, '198.51.100.20', 3389)", {1, 2});
    expect_sequences("udp('2001:db8::10', 65535, '2001:db8::20', 0)", {5});

    flowsql::FilterDomainResolveRequestV1 request;
    request.target_kind = flowsql::FilterDomainTargetKindV1::kSource;
    request.target_category = "other";
    request.target_name = "capture";
    request.output_schema = schema;
    request.expression = original;
    flowsql::FilterDomainResolveResultV1 result;
    result.lowered_expression = original;
    result.diagnostic = "stale";
    assert(resolver.Resolve(request, &result) == ENOTSUP);
    assert(!result.lowered_expression && result.diagnostic.empty());
    request.target_category = "pcapfile";
    request.target_kind = flowsql::FilterDomainTargetKindV1::kTransform;
    assert(resolver.Resolve(request, &result) == ENOTSUP);
    request.target_kind = flowsql::FilterDomainTargetKindV1::kSource;
    request.contract_version = 99;
    assert(resolver.Resolve(request, &result) == EINVAL);
    assert(!result.lowered_expression && !result.diagnostic.empty());

    request.contract_version = flowsql::kFilterDomainResolverContractVersionV1;
    request.target_name = "";
    assert(resolver.Resolve(request, &result) == EINVAL);
    assert(!result.lowered_expression && !result.diagnostic.empty());
    request.target_name = "capture";
    request.output_schema = arrow::schema({arrow::field("timestamp_ns", arrow::int64())});
    assert(resolver.Resolve(request, &result) == EINVAL);
    assert(!result.lowered_expression && !result.diagnostic.empty());
    request.output_schema = schema;
    for (const char* invalid : {"timestamp_ns >= TIMESTAMP '2026-09-08T01:30:00'",
                                "captured_len = TIMESTAMP '1970-01-01T00:00:00Z'",
                                "mac('bad')", "ip('bad')", "port(65536)", "port('80')",
                                "tcp('192.0.2.1', 1, '2001:db8::2', 2)",
                                "tcp('192.0.2.1', 1)", "unknown_packet_function(1)"}) {
        request.expression = parse(invalid);
        assert(resolver.Resolve(request, &result) != 0);
        assert(!result.lowered_expression && !result.diagnostic.empty());
    }
}

void TestClassicPcap() {
    std::vector<uint8_t> bytes;
    Put32(&bytes, 0xa1b2c3d4, true); Put16(&bytes, 2, true); Put16(&bytes, 4, true);
    Put32(&bytes, 0, true); Put32(&bytes, 0, true); Put32(&bytes, 65535, true); Put32(&bytes, 1, true);
    Put32(&bytes, 2, true); Put32(&bytes, 123456, true); Put32(&bytes, 4, true); Put32(&bytes, 6, true);
    bytes.insert(bytes.end(), {1, 2, 3, 4});
    const std::string path = Temp("classic.pcap"); WriteFile(path, bytes);

    MockProtocol protocol;
    pcapfile::PcapFileSourceConfig config; config.path = path; config.format = "pcap"; config.batch_packets = 1;
    pcapfile::PcapFileChannel channel("classic", config, &protocol);
    assert(channel.Open() == 0);
    assert(channel.OutstandingBatchCount() == 0);
    auto event = channel.PollBlock(0);
    assert(event.kind == flowsql::BlockPollEvent::kData && event.batch);
    assert(channel.OutstandingBatchCount() == 1);
    assert(event.batch->num_rows() == 1);
    auto timestamp = std::static_pointer_cast<arrow::Int64Array>(event.batch->column(0));
    auto captured = std::static_pointer_cast<arrow::UInt32Array>(event.batch->column(1));
    auto raw = std::static_pointer_cast<arrow::BinaryArray>(event.batch->column(6));
    assert(timestamp->Value(0) == 2123456000LL);
    assert(captured->Value(0) == 4);
    assert(raw->GetView(0) == "\x01\x02\x03\x04");
    assert(protocol.layer_calls == 1 && protocol.identify_calls == 0);
    assert(channel.ReleaseBlock(event.batch) == 0);
    assert(channel.OutstandingBatchCount() == 0);
    assert(channel.ReleaseBlock(event.batch) != 0);
    const auto eof = channel.PollBlock(0);
    AssertNoBatchEvent(eof, flowsql::BlockPollEvent::kEof);
    assert(eof.err == 0);
    const auto after_eof = channel.PollBlock(0);
    AssertNoBatchEvent(after_eof, flowsql::BlockPollEvent::kCancelled);
    assert(after_eof.err == ECANCELED);
}

void TestClassicMagicAndEndian() {
    const struct {
        bool little;
        bool nanosecond;
        uint32_t fraction;
        int64_t expected;
    } cases[] = {
        {true, false, 123, 2000123000LL},
        {false, false, 123, 2000123000LL},
        {true, true, 123, 2000000123LL},
        {false, true, 123, 2000000123LL},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const std::string path = Temp((std::string("magic_") + std::to_string(i) + ".pcap").c_str());
        WriteFile(path, MakeClassicPcap(cases[i].little, cases[i].nanosecond, cases[i].fraction));
        MockProtocol protocol;
        pcapfile::PcapFileSourceConfig config;
        config.path = path;
        config.format = "auto";
        config.batch_packets = 1;
        pcapfile::PcapFileChannel channel("magic", config, &protocol);
        assert(channel.Open() == 0);
        auto event = channel.PollBlock(0);
        assert(event.kind == flowsql::BlockPollEvent::kData);
        auto timestamp = std::static_pointer_cast<arrow::Int64Array>(event.batch->column(0));
        assert(timestamp->Value(0) == cases[i].expected);
        assert(channel.ReleaseBlock(event.batch) == 0);
    }
}

void TestClassicFieldsAndFileOrder() {
    std::vector<uint8_t> bytes = MakeClassicHeader(false, false, 8, 1);
    AppendClassicRecord(&bytes, false, 4, 7, {0x01, 0x02, 0x03}, 5);
    AppendClassicRecord(&bytes, false, 2, 9, {0xa0}, 0);
    const std::string path = Temp("classic_fields_order.pcap");
    WriteFile(path, bytes);

    MockProtocol protocol;
    pcapfile::PcapFileSourceConfig config;
    config.path = path;
    config.format = "pcap";
    config.batch_packets = 8;
    pcapfile::PcapFileChannel channel("classic_fields_order", config, &protocol);
    assert(channel.Open() == 0);
    const auto event = channel.PollBlock(0);
    assert(event.kind == flowsql::BlockPollEvent::kData && event.batch);
    assert(event.batch->num_rows() == 2);

    auto timestamp = std::static_pointer_cast<arrow::Int64Array>(event.batch->column(0));
    auto captured = std::static_pointer_cast<arrow::UInt32Array>(event.batch->column(1));
    auto wire = std::static_pointer_cast<arrow::UInt32Array>(event.batch->column(2));
    auto link_type = std::static_pointer_cast<arrow::UInt32Array>(event.batch->column(3));
    auto source = std::static_pointer_cast<arrow::UInt32Array>(event.batch->column(4));
    auto sequence = std::static_pointer_cast<arrow::UInt64Array>(event.batch->column(5));
    auto raw = std::static_pointer_cast<arrow::BinaryArray>(event.batch->column(6));
    assert(timestamp->Value(0) == 4000007000LL && timestamp->Value(1) == 2000009000LL);
    assert(captured->Value(0) == 3 && captured->Value(1) == 1);
    assert(wire->Value(0) == 5 && wire->Value(1) == 0);
    assert(link_type->Value(0) == 1 && link_type->Value(1) == 1);
    assert(source->Value(0) == 0 && source->Value(1) == 0);
    assert(sequence->Value(0) == 0 && sequence->Value(1) == 1);
    assert(raw->GetView(0) == std::string_view("\x01\x02\x03", 3));
    assert(raw->GetView(1) == std::string_view("\xa0", 1));
    assert(channel.ReleaseBlock(event.batch) == 0);
    assert(channel.PollBlock(0).kind == flowsql::BlockPollEvent::kEof);
}

void TestClassicStructureErrors() {
    AssertCaptureSourceError(
        "unsupported_capture_magic.bin", "auto", std::vector<uint8_t>(24, 0));
    {
        auto bytes = MakeClassicPcap(true, false, 0);
        bytes[4] = 3;
        AssertCaptureSourceError("classic_bad_version.pcap", "pcap", bytes);
    }
    {
        auto bytes = MakeClassicPcap(true, false, 0);
        Store32(&bytes, 16, 0, true);
        AssertCaptureSourceError("classic_zero_snaplen.pcap", "pcap", bytes);
    }
    AssertCaptureSourceError(
        "classic_bad_microseconds.pcap", "pcap",
        MakeClassicPcap(true, false, 1000000));
    AssertCaptureSourceError(
        "classic_bad_nanoseconds.pcap", "pcap",
        MakeClassicPcap(false, true, 1000000000));
    AssertCaptureSourceError(
        "classic_wire_shorter_than_capture.pcap", "pcap",
        MakeClassicPcap(true, false, 0, 1, 4, 3));
    {
        auto bytes = MakeClassicPcap(true, false, 0);
        Store32(&bytes, 16, 3, true);
        AssertCaptureSourceError("classic_capture_exceeds_snaplen.pcap", "pcap", bytes);
    }
    {
        auto bytes = MakeClassicHeader(true, false);
        bytes.insert(bytes.end(), 15, 0);
        AssertCaptureSourceError("classic_truncated_record_header.pcap", "pcap", bytes);
    }
    {
        auto bytes = MakeClassicPcap(true, false, 0);
        bytes.pop_back();
        AssertCaptureSourceError("classic_truncated_packet.pcap", "pcap", bytes);
    }
}

void TestPendingErrorAfterData() {
    auto bytes = MakeClassicPcap(true, false, 0);
    Put32(&bytes, 3, true);
    Put32(&bytes, 0, true);
    Put32(&bytes, 4, true);
    Put32(&bytes, 4, true);
    bytes.push_back(9);
    const std::string path = Temp("pending_error.pcap");
    WriteFile(path, bytes);
    MockProtocol protocol;
    pcapfile::PcapFileSourceConfig config;
    config.path = path;
    config.batch_packets = 8;
    pcapfile::PcapFileChannel channel("pending", config, &protocol);
    assert(channel.Open() == 0);
    auto data = channel.PollBlock(0);
    assert(data.kind == flowsql::BlockPollEvent::kData && data.batch);
    assert(data.batch->num_rows() == 1);
    assert(channel.OutstandingBatchCount() == 1);
    assert(channel.ReleaseBlock(data.batch) == 0);
    assert(channel.OutstandingBatchCount() == 0);
    const auto error = channel.PollBlock(0);
    AssertNoBatchEvent(error, flowsql::BlockPollEvent::kError);
    assert(error.err != 0);
    const auto after_error = channel.PollBlock(0);
    AssertNoBatchEvent(after_error, flowsql::BlockPollEvent::kCancelled);
    assert(after_error.err == ECANCELED);
}

void TestSupportedLayerDecodeContract() {
    const std::vector<std::vector<uint8_t>> packets = {
        std::vector<uint8_t>(14, 0x11),
        std::vector<uint8_t>(15, 0x22),
        std::vector<uint8_t>(16, 0x33),
    };
    std::vector<uint8_t> bytes = MakeClassicHeader(true, true, 65535, 1);
    for (size_t index = 0; index < packets.size(); ++index) {
        AppendClassicRecord(&bytes, true, 1, static_cast<uint32_t>(index), packets[index],
                            static_cast<uint32_t>(packets[index].size()));
    }
    const std::string path = Temp("supported_layer_contract.pcap");
    WriteFile(path, bytes);

    MockProtocol protocol;
    protocol.payload_offset = 14;
    pcapfile::PcapFileSourceConfig config;
    config.path = path;
    config.format = "pcap";
    config.batch_packets = 8;
    pcapfile::PcapFileChannel channel("supported_layer_contract", config, &protocol);
    assert(channel.Open() == 0);
    const auto event = channel.PollBlock(0);
    assert(event.kind == flowsql::BlockPollEvent::kData && event.batch);
    assert(event.batch->num_rows() == static_cast<int64_t>(packets.size()));
    assert(protocol.layer_calls == static_cast<int>(packets.size()));
    assert(protocol.identify_calls == 0);
    assert(protocol.layer_sizes == std::vector<int32_t>({14, 15, 16}));
    assert(protocol.layer_packets == packets);

    auto status = std::static_pointer_cast<arrow::UInt8Array>(event.batch->GetColumnByName("layer_status"));
    auto count = std::static_pointer_cast<arrow::UInt8Array>(event.batch->GetColumnByName("layer_count"));
    auto ids_list =
        std::static_pointer_cast<arrow::FixedSizeListArray>(event.batch->GetColumnByName("layer_ids"));
    auto ids = std::static_pointer_cast<arrow::UInt16Array>(ids_list->values());
    auto offsets_list =
        std::static_pointer_cast<arrow::FixedSizeListArray>(event.batch->GetColumnByName("layer_offsets"));
    auto offsets = std::static_pointer_cast<arrow::UInt32Array>(offsets_list->values());
    auto payload = std::static_pointer_cast<arrow::UInt32Array>(event.batch->GetColumnByName("payload_offset"));
    auto raw = std::static_pointer_cast<arrow::BinaryArray>(event.batch->GetColumnByName("raw_data"));
    for (size_t index = 0; index < packets.size(); ++index) {
        assert(status->Value(index) == static_cast<uint8_t>(flowsql::packet::LayerStatus::kDecoded));
        assert(count->Value(index) == 1);
        const size_t layer_base = index * flowsql::packet::kMaxLayerDepth;
        assert(ids->Value(layer_base) == static_cast<uint16_t>(flowsql::eLayer::ETHERNET));
        assert(offsets->Value(layer_base) == 0);
        assert(payload->Value(index) == 14);
        const std::string_view expected(
            reinterpret_cast<const char*>(packets[index].data()), packets[index].size());
        assert(raw->GetView(index) == expected);
    }
    assert(channel.ReleaseBlock(event.batch) == 0);
}

void TestUnsupportedTruncatedAndMalformedLayer() {
    const std::vector<uint8_t> packet(14, 0x5a);
    const std::string_view expected_raw(
        reinterpret_cast<const char*>(packet.data()), packet.size());
    {
        std::vector<uint8_t> bytes = MakeClassicHeader(true, false, 65535, 147);
        AppendClassicRecord(&bytes, true, 1, 0, packet, packet.size());
        const std::string path = Temp("unsupported_link.pcap");
        WriteFile(path, bytes);
        MockProtocol protocol;
        pcapfile::PcapFileSourceConfig config;
        config.path = path;
        pcapfile::PcapFileChannel channel("unsupported", config, &protocol);
        assert(channel.Open() == 0);
        const auto event = channel.PollBlock(0);
        assert(event.kind == flowsql::BlockPollEvent::kData && event.batch);
        auto status = std::static_pointer_cast<arrow::UInt8Array>(event.batch->column(7));
        auto raw = std::static_pointer_cast<arrow::BinaryArray>(event.batch->column(6));
        assert(status->Value(0) ==
               static_cast<uint8_t>(flowsql::packet::LayerStatus::kUnsupportedLinkType));
        assert(raw->GetView(0) == expected_raw);
        assert(protocol.layer_calls == 0 && protocol.identify_calls == 0);
        assert(protocol.layer_packets.empty());
        assert(channel.ReleaseBlock(event.batch) == 0);
    }
    {
        std::vector<uint8_t> bytes = MakeClassicHeader(true, false, 65535, 1);
        AppendClassicRecord(&bytes, true, 1, 0, packet, packet.size() + 4);
        const std::string path = Temp("truncated_wire.pcap");
        WriteFile(path, bytes);
        MockProtocol protocol;
        pcapfile::PcapFileSourceConfig config;
        config.path = path;
        pcapfile::PcapFileChannel channel("truncated_wire", config, &protocol);
        assert(channel.Open() == 0);
        const auto event = channel.PollBlock(0);
        assert(event.kind == flowsql::BlockPollEvent::kData && event.batch);
        auto status = std::static_pointer_cast<arrow::UInt8Array>(event.batch->column(7));
        auto raw = std::static_pointer_cast<arrow::BinaryArray>(event.batch->column(6));
        assert(status->Value(0) == static_cast<uint8_t>(flowsql::packet::LayerStatus::kTruncated));
        assert(raw->GetView(0) == expected_raw);
        assert(protocol.layer_calls == 1 && protocol.identify_calls == 0);
        assert(protocol.layer_packets == std::vector<std::vector<uint8_t>>({packet}));
        assert(channel.ReleaseBlock(event.batch) == 0);
    }
    {
        std::vector<uint8_t> bytes = MakeClassicHeader(true, false, 65535, 1);
        AppendClassicRecord(&bytes, true, 1, 0, packet, packet.size());
        const std::string path = Temp("truncated_payload.pcap");
        WriteFile(path, bytes);
        MockProtocol protocol;
        protocol.payload_offset = static_cast<uint16_t>(packet.size() + 1);
        pcapfile::PcapFileSourceConfig config;
        config.path = path;
        pcapfile::PcapFileChannel channel("truncated_payload", config, &protocol);
        assert(channel.Open() == 0);
        const auto event = channel.PollBlock(0);
        assert(event.kind == flowsql::BlockPollEvent::kData && event.batch);
        auto status = std::static_pointer_cast<arrow::UInt8Array>(event.batch->column(7));
        auto payload = std::static_pointer_cast<arrow::UInt32Array>(event.batch->column(14));
        auto raw = std::static_pointer_cast<arrow::BinaryArray>(event.batch->column(6));
        assert(status->Value(0) == static_cast<uint8_t>(flowsql::packet::LayerStatus::kTruncated));
        assert(payload->Value(0) == packet.size());
        assert(raw->GetView(0) == expected_raw);
        assert(protocol.layer_calls == 1 && protocol.identify_calls == 0);
        assert(channel.ReleaseBlock(event.batch) == 0);
    }
    {
        std::vector<uint8_t> bytes = MakeClassicHeader(true, false, 65535, 1);
        AppendClassicRecord(&bytes, true, 1, 0, packet, packet.size());
        const std::string path = Temp("truncated_layer_offset.pcap");
        WriteFile(path, bytes);
        MockProtocol protocol;
        protocol.layer_offset = static_cast<uint16_t>(packet.size() + 1);
        pcapfile::PcapFileSourceConfig config;
        config.path = path;
        pcapfile::PcapFileChannel channel("truncated_layer_offset", config, &protocol);
        assert(channel.Open() == 0);
        const auto event = channel.PollBlock(0);
        assert(event.kind == flowsql::BlockPollEvent::kData && event.batch);
        auto status = std::static_pointer_cast<arrow::UInt8Array>(event.batch->column(7));
        auto offsets_list =
            std::static_pointer_cast<arrow::FixedSizeListArray>(event.batch->column(10));
        auto offsets = std::static_pointer_cast<arrow::UInt32Array>(offsets_list->values());
        auto raw = std::static_pointer_cast<arrow::BinaryArray>(event.batch->column(6));
        assert(status->Value(0) == static_cast<uint8_t>(flowsql::packet::LayerStatus::kTruncated));
        assert(offsets->Value(0) == packet.size());
        assert(raw->GetView(0) == expected_raw);
        assert(protocol.layer_calls == 1 && protocol.identify_calls == 0);
        assert(protocol.layer_packets == std::vector<std::vector<uint8_t>>({packet}));
        assert(channel.ReleaseBlock(event.batch) == 0);
    }
    for (const bool invalid_layer_count : {false, true}) {
        std::vector<uint8_t> bytes = MakeClassicHeader(true, false, 65535, 1);
        AppendClassicRecord(&bytes, true, 1, 0, packet, packet.size());
        const std::string suffix = invalid_layer_count
                                       ? "malformed_layer_count.pcap"
                                       : "malformed_layer_result.pcap";
        const std::string path = Temp(suffix.c_str());
        WriteFile(path, bytes);
        MockProtocol protocol;
        if (invalid_layer_count) {
            protocol.reported_layer_count =
                static_cast<uint16_t>(flowsql::protocol::MAX_LAYERS + 1);
        } else {
            protocol.layer_result = -1;
        }
        pcapfile::PcapFileSourceConfig config;
        config.path = path;
        pcapfile::PcapFileChannel channel("malformed", config, &protocol);
        assert(channel.Open() == 0);
        const auto event = channel.PollBlock(0);
        assert(event.kind == flowsql::BlockPollEvent::kData && event.batch);
        auto status = std::static_pointer_cast<arrow::UInt8Array>(event.batch->column(7));
        auto raw = std::static_pointer_cast<arrow::BinaryArray>(event.batch->column(6));
        assert(status->Value(0) == static_cast<uint8_t>(flowsql::packet::LayerStatus::kMalformed));
        assert(raw->GetView(0) == expected_raw);
        assert(protocol.layer_calls == 1 && protocol.identify_calls == 0);
        assert(protocol.layer_packets == std::vector<std::vector<uint8_t>>({packet}));
        assert(channel.ReleaseBlock(event.batch) == 0);
    }
}

void TestEmptyFileEof() {
    const std::string path = Temp("empty.pcap");
    WriteFile(path, {});
    MockProtocol protocol;
    pcapfile::PcapFileSourceConfig config;
    config.path = path;
    pcapfile::PcapFileChannel channel("empty", config, &protocol);
    assert(channel.Open() == 0);
    const auto eof = channel.PollBlock(0);
    AssertNoBatchEvent(eof, flowsql::BlockPollEvent::kEof);
    assert(eof.err == 0);
    const auto after_eof = channel.PollBlock(0);
    AssertNoBatchEvent(after_eof, flowsql::BlockPollEvent::kCancelled);
    assert(after_eof.err == ECANCELED);
}

void TestPcapngSectionAndResolution() {
    std::vector<uint8_t> bytes;
    auto block = [&](uint32_t type, const std::vector<uint8_t>& body) {
        const uint32_t total = static_cast<uint32_t>(body.size() + 12);
        Put32(&bytes, type, true); Put32(&bytes, total, true);
        bytes.insert(bytes.end(), body.begin(), body.end()); Put32(&bytes, total, true);
    };
    std::vector<uint8_t> shb; Put32(&shb, 0x1a2b3c4d, true); Put16(&shb, 1, true); Put16(&shb, 0, true); Put64(&shb, 0xffffffffffffffffULL, true); block(0x0a0d0d0a, shb);
    std::vector<uint8_t> idb; Put16(&idb, 1, true); Put16(&idb, 0, true); Put32(&idb, 65535, true);
    Put16(&idb, 9, true); Put16(&idb, 1, true); idb.push_back(9); idb.push_back(0); idb.push_back(0); idb.push_back(0); Put16(&idb, 14, true); Put16(&idb, 8, true); Put64(&idb, 1, true); Put16(&idb, 0, true); Put16(&idb, 0, true); block(1, idb);
    std::vector<uint8_t> epb; Put32(&epb, 0, true); Put32(&epb, 0, true); Put32(&epb, 1, true); Put32(&epb, 3, true); Put32(&epb, 5, true); epb.insert(epb.end(), {9, 8, 7}); epb.push_back(0); block(6, epb);
    const std::string path = Temp("section.pcapng"); WriteFile(path, bytes);

    MockProtocol protocol;
    pcapfile::PcapFileSourceConfig config; config.path = path; config.format = "pcapng";
    pcapfile::PcapFileChannel channel("ng", config, &protocol);
    assert(channel.Open() == 0);
    auto event = channel.PollBlock(0);
    assert(event.kind == flowsql::BlockPollEvent::kData);
    auto timestamp = std::static_pointer_cast<arrow::Int64Array>(event.batch->column(0));
    auto source = std::static_pointer_cast<arrow::UInt32Array>(event.batch->column(4));
    auto sequence = std::static_pointer_cast<arrow::UInt64Array>(event.batch->column(5));
    assert(timestamp->Value(0) == 1000000001LL);
    assert(source->Value(0) == 0 && sequence->Value(0) == 0);
    assert(channel.ReleaseBlock(event.batch) == 0);
}

void TestPcapngSectionsAndRounding() {
    std::vector<uint8_t> bytes;
    AppendPcapngSectionHeader(&bytes, true);
    AppendPcapngInterface(&bytes, true, 1, false, 10);
    AppendPcapngEnhancedPacket(&bytes, true, 0, 5, {1});
    AppendPcapngEnhancedPacket(&bytes, true, 0, 15, {2});
    AppendPcapngSectionHeader(&bytes, false);
    AppendPcapngInterface(&bytes, false, 1, true, 3, -1);
    AppendPcapngEnhancedPacket(&bytes, false, 0, 8, {3});
    const std::string path = Temp("sections_rounding.pcapng");
    WriteFile(path, bytes);

    MockProtocol protocol;
    pcapfile::PcapFileSourceConfig config;
    config.path = path;
    config.format = "auto";
    config.batch_packets = 8;
    pcapfile::PcapFileChannel channel("sections", config, &protocol);
    assert(channel.Open() == 0);
    auto event = channel.PollBlock(0);
    assert(event.kind == flowsql::BlockPollEvent::kData);
    assert(event.batch->num_rows() == 3);
    auto timestamp = std::static_pointer_cast<arrow::Int64Array>(event.batch->column(0));
    auto source = std::static_pointer_cast<arrow::UInt32Array>(event.batch->column(4));
    auto sequence = std::static_pointer_cast<arrow::UInt64Array>(event.batch->column(5));
    assert(timestamp->Value(0) == 0);
    assert(timestamp->Value(1) == 2);
    assert(timestamp->Value(2) == 0);
    assert(source->Value(0) == 0 && source->Value(1) == 0 && source->Value(2) == 1);
    assert(sequence->Value(0) == 0 && sequence->Value(1) == 1 && sequence->Value(2) == 0);
    assert(protocol.layer_calls == 3);
    assert(channel.ReleaseBlock(event.batch) == 0);
    assert(channel.PollBlock(0).kind == flowsql::BlockPollEvent::kEof);
}

void TestPcapngInterfaceFieldsAndFileOrder() {
    std::vector<uint8_t> bytes;
    AppendPcapngSectionHeader(&bytes, true);
    AppendPcapngInterface(&bytes, true, 1, false, 6);
    AppendPcapngInterface(&bytes, true, 147, false, 9);
    AppendPcapngEnhancedPacket(&bytes, true, 1, 1000000001ULL, {0xb1, 0xb2}, 4);
    AppendPcapngEnhancedPacket(&bytes, true, 0, 2, {0xa0}, 1);
    AppendPcapngEnhancedPacket(&bytes, true, 1, 3, {0xc0}, 0);
    const std::string path = Temp("pcapng_interface_order.pcapng");
    WriteFile(path, bytes);

    MockProtocol protocol;
    pcapfile::PcapFileSourceConfig config;
    config.path = path;
    config.format = "pcapng";
    config.batch_packets = 8;
    pcapfile::PcapFileChannel channel("pcapng_interface_order", config, &protocol);
    assert(channel.Open() == 0);
    const auto event = channel.PollBlock(0);
    assert(event.kind == flowsql::BlockPollEvent::kData && event.batch);
    assert(event.batch->num_rows() == 3);

    auto timestamp = std::static_pointer_cast<arrow::Int64Array>(event.batch->column(0));
    auto captured = std::static_pointer_cast<arrow::UInt32Array>(event.batch->column(1));
    auto wire = std::static_pointer_cast<arrow::UInt32Array>(event.batch->column(2));
    auto link_type = std::static_pointer_cast<arrow::UInt32Array>(event.batch->column(3));
    auto source = std::static_pointer_cast<arrow::UInt32Array>(event.batch->column(4));
    auto sequence = std::static_pointer_cast<arrow::UInt64Array>(event.batch->column(5));
    auto raw = std::static_pointer_cast<arrow::BinaryArray>(event.batch->column(6));
    assert(timestamp->Value(0) == 1000000001LL);
    assert(timestamp->Value(1) == 2000LL);
    assert(timestamp->Value(2) == 3LL);
    assert(captured->Value(0) == 2 && captured->Value(1) == 1 && captured->Value(2) == 1);
    assert(wire->Value(0) == 4 && wire->Value(1) == 1 && wire->Value(2) == 0);
    assert(link_type->Value(0) == 147 && link_type->Value(1) == 1 && link_type->Value(2) == 147);
    assert(source->Value(0) == 1 && source->Value(1) == 0 && source->Value(2) == 1);
    assert(sequence->Value(0) == 0 && sequence->Value(1) == 0 && sequence->Value(2) == 1);
    assert(raw->GetView(0) == std::string_view("\xb1\xb2", 2));
    assert(raw->GetView(1) == std::string_view("\xa0", 1));
    assert(raw->GetView(2) == std::string_view("\xc0", 1));
    assert(channel.ReleaseBlock(event.batch) == 0);
    assert(channel.PollBlock(0).kind == flowsql::BlockPollEvent::kEof);
}

void TestPcapngTimestampQuantizationAndOverflow() {
    {
        std::vector<uint8_t> bytes;
        AppendPcapngSectionHeader(&bytes, true);
        AppendPcapngInterface(&bytes, true, 1, true, 10);
        AppendPcapngEnhancedPacket(&bytes, true, 0, 1, {1});
        AppendPcapngEnhancedPacket(&bytes, true, 0, 3, {2});
        const std::string path = Temp("pcapng_binary_ties_to_even.pcapng");
        WriteFile(path, bytes);

        MockProtocol protocol;
        pcapfile::PcapFileSourceConfig config;
        config.path = path;
        config.format = "pcapng";
        config.batch_packets = 8;
        pcapfile::PcapFileChannel channel("pcapng_binary_ties_to_even", config, &protocol);
        assert(channel.Open() == 0);
        const auto event = channel.PollBlock(0);
        assert(event.kind == flowsql::BlockPollEvent::kData && event.batch);
        assert(event.batch->num_rows() == 2);
        auto timestamp = std::static_pointer_cast<arrow::Int64Array>(event.batch->column(0));
        assert(timestamp->Value(0) == 976562LL);
        assert(timestamp->Value(1) == 2929688LL);
        assert(channel.ReleaseBlock(event.batch) == 0);
        assert(channel.PollBlock(0).kind == flowsql::BlockPollEvent::kEof);
    }
    {
        std::vector<uint8_t> bytes;
        AppendPcapngSectionHeader(&bytes, true);
        AppendPcapngInterface(&bytes, true, 1, false, 0);
        AppendPcapngEnhancedPacket(&bytes, true, 0, UINT64_MAX, {1});
        AssertPcapngSourceError("pcapng_timestamp_overflow.pcapng", bytes);
    }
}

void TestPcapngEnhancedPacketMapping() {
    std::vector<uint8_t> bytes;
    AppendPcapngSectionHeader(&bytes, true);
    AppendPcapngInterface(&bytes, true, 1, false, 6, 0, 4);
    AppendPcapngEnhancedPacket(&bytes, true, 0, 1234567, {0xde, 0xad, 0xbe}, 9);
    const std::string path = Temp("epb_mapping.pcapng");
    WriteFile(path, bytes);

    MockProtocol protocol;
    pcapfile::PcapFileSourceConfig config;
    config.path = path;
    config.format = "pcapng";
    pcapfile::PcapFileChannel channel("epb_mapping", config, &protocol);
    assert(channel.Open() == 0);
    const auto event = channel.PollBlock(0);
    assert(event.kind == flowsql::BlockPollEvent::kData && event.batch);
    assert(event.batch->num_rows() == 1);
    auto timestamp = std::static_pointer_cast<arrow::Int64Array>(event.batch->column(0));
    auto captured = std::static_pointer_cast<arrow::UInt32Array>(event.batch->column(1));
    auto wire = std::static_pointer_cast<arrow::UInt32Array>(event.batch->column(2));
    auto link_type = std::static_pointer_cast<arrow::UInt32Array>(event.batch->column(3));
    auto source = std::static_pointer_cast<arrow::UInt32Array>(event.batch->column(4));
    auto sequence = std::static_pointer_cast<arrow::UInt64Array>(event.batch->column(5));
    auto raw = std::static_pointer_cast<arrow::BinaryArray>(event.batch->column(6));
    assert(timestamp->Value(0) == 1234567000LL);
    assert(captured->Value(0) == 3 && wire->Value(0) == 9);
    assert(link_type->Value(0) == 1 && source->Value(0) == 0 && sequence->Value(0) == 0);
    assert(raw->GetView(0) == std::string_view("\xde\xad\xbe", 3));
    assert(channel.ReleaseBlock(event.batch) == 0);
    assert(channel.PollBlock(0).kind == flowsql::BlockPollEvent::kEof);
}

void TestPcapngEnhancedPacketErrors() {
    {
        std::vector<uint8_t> bytes;
        AppendPcapngSectionHeader(&bytes, true);
        AppendPcapngInterface(&bytes, true, 1);
        AppendPcapngEnhancedPacket(&bytes, true, 1, 0, {1});
        AssertPcapngSourceError("epb_unknown_interface.pcapng", bytes);
    }
    for (uint32_t type : {2u, 3u, 0x12345678u}) {
        std::vector<uint8_t> bytes;
        AppendPcapngSectionHeader(&bytes, true);
        AppendPcapngInterface(&bytes, true, 1);
        AppendPcapngBlock(&bytes, type, std::vector<uint8_t>(20, 0), true);
        const std::string suffix = "epb_unsupported_" + std::to_string(type) + ".pcapng";
        AssertPcapngSourceError(suffix.c_str(), bytes);
    }
    {
        std::vector<uint8_t> bytes;
        AppendPcapngSectionHeader(&bytes, true);
        AppendPcapngInterface(&bytes, true, 1, false, 6, 0, 2);
        AppendPcapngEnhancedPacket(&bytes, true, 0, 0, {1, 2, 3});
        AssertPcapngSourceError("epb_exceeds_snaplen.pcapng", bytes);
    }
    {
        std::vector<uint8_t> bytes;
        AppendPcapngSectionHeader(&bytes, true);
        AppendPcapngInterface(&bytes, true, 1);
        std::vector<uint8_t> body;
        Put32(&body, 0, true);
        Put32(&body, 0, true);
        Put32(&body, 0, true);
        Put32(&body, 5, true);
        Put32(&body, 5, true);
        body.insert(body.end(), {1, 2, 3, 4});
        AppendPcapngBlock(&bytes, 6, body, true);
        AssertPcapngSourceError("epb_truncated_bytes.pcapng", bytes);
    }
    {
        std::vector<uint8_t> bytes;
        AppendPcapngSectionHeader(&bytes, true);
        AppendPcapngInterface(&bytes, true, 1);
        std::vector<uint8_t> body;
        Put32(&body, 0, true);
        Put32(&body, 0, true);
        Put32(&body, 0, true);
        Put32(&body, 4, true);
        Put32(&body, 4, true);
        body.insert(body.end(), {1, 2, 3, 4});
        Put16(&body, 1, true);
        Put16(&body, 4, true);
        AppendPcapngBlock(&bytes, 6, body, true);
        AssertPcapngSourceError("epb_truncated_option.pcapng", bytes);
    }
}

void TestPcapngSectionValidation() {
    {
        std::vector<uint8_t> bytes;
        AppendPcapngSectionHeader(&bytes, true);
        bytes[12] = 2;
        AssertCaptureSourceError("pcapng_bad_version.pcapng", "pcapng", bytes);
    }
    {
        std::vector<uint8_t> bytes;
        AppendPcapngSectionHeader(&bytes, true);
        bytes[8] = 0;
        AssertCaptureSourceError("pcapng_bad_byte_order_magic.pcapng", "pcapng", bytes);
    }
    {
        std::vector<uint8_t> bytes;
        AppendPcapngSectionHeader(&bytes, true);
        AssertPcapngSourceError("section_without_idb.pcapng", bytes);
    }
    {
        std::vector<uint8_t> bytes;
        AppendPcapngSectionHeader(&bytes, true);
        AppendPcapngInterface(&bytes, true, 1);
        const std::string path = Temp("empty_section.pcapng");
        WriteFile(path, bytes);
        MockProtocol protocol;
        pcapfile::PcapFileSourceConfig config;
        config.path = path;
        config.format = "pcapng";
        pcapfile::PcapFileChannel channel("empty_section", config, &protocol);
        assert(channel.Open() == 0);
        assert(channel.PollBlock(0).kind == flowsql::BlockPollEvent::kEof);
    }
    {
        std::vector<uint8_t> bytes;
        AppendPcapngSectionHeader(&bytes, true);
        AppendPcapngSectionHeader(&bytes, false);
        AppendPcapngInterface(&bytes, false, 1);
        AssertPcapngSourceError("section_before_idb.pcapng", bytes);
    }
    {
        std::vector<uint8_t> bytes;
        AppendPcapngSectionHeader(&bytes, true);
        AppendPcapngInterface(&bytes, true, 1);
        AppendPcapngEnhancedPacket(&bytes, true, 0, 0, {1});
        AppendPcapngSectionHeader(&bytes, false);
        const std::string path = Temp("last_section_without_idb.pcapng");
        WriteFile(path, bytes);
        MockProtocol protocol;
        pcapfile::PcapFileSourceConfig config;
        config.path = path;
        config.format = "pcapng";
        pcapfile::PcapFileChannel channel("last_section_without_idb", config, &protocol);
        assert(channel.Open() == 0);
        const auto data = channel.PollBlock(0);
        assert(data.kind == flowsql::BlockPollEvent::kData && data.batch);
        assert(data.batch->num_rows() == 1);
        assert(channel.ReleaseBlock(data.batch) == 0);
        const auto error = channel.PollBlock(0);
        assert(error.kind == flowsql::BlockPollEvent::kError && !error.batch && error.err != 0);
        assert(channel.PollBlock(0).kind == flowsql::BlockPollEvent::kCancelled);
    }
    {
        std::vector<uint8_t> bytes;
        std::vector<uint8_t> options;
        Put16(&options, 1, true);
        Put16(&options, 4, true);
        AppendPcapngSectionHeaderWithOptions(&bytes, true, options);
        const std::string path = Temp("first_shb_truncated_option.pcapng");
        WriteFile(path, bytes);
        MockProtocol protocol;
        pcapfile::PcapFileSourceConfig config;
        config.path = path;
        config.format = "pcapng";
        pcapfile::PcapFileChannel channel("first_shb_truncated_option", config, &protocol);
        assert(channel.Open() != 0);
        const auto error = channel.PollBlock(0);
        assert(error.kind == flowsql::BlockPollEvent::kError && !error.batch && error.err != 0);
        assert(channel.PollBlock(0).kind == flowsql::BlockPollEvent::kCancelled);
    }
    {
        std::vector<uint8_t> bytes;
        AppendPcapngSectionHeader(&bytes, true);
        AppendPcapngInterface(&bytes, true, 1);
        std::vector<uint8_t> options;
        Put16(&options, 0, false);
        Put16(&options, 1, false);
        AppendPcapngSectionHeaderWithOptions(&bytes, false, options);
        AssertPcapngSourceError("later_shb_invalid_terminator.pcapng", bytes);
    }
}

void TestPcapngInterfaceOptionErrors() {
    {
        std::vector<uint8_t> bytes;
        AppendPcapngSectionHeader(&bytes, true);
        std::vector<uint8_t> interface_body;
        Put16(&interface_body, 1, true);
        Put16(&interface_body, 0, true);
        Put32(&interface_body, 65535, true);
        Put16(&interface_body, 9, true);
        Put16(&interface_body, 2, true);
        interface_body.insert(interface_body.end(), {6, 0, 0, 0});
        Put16(&interface_body, 0, true);
        Put16(&interface_body, 0, true);
        AppendPcapngBlock(&bytes, 1, interface_body, true);
        AssertPcapngSourceError("pcapng_bad_tsresol_length.pcapng", bytes);
    }
    {
        std::vector<uint8_t> bytes;
        AppendPcapngSectionHeader(&bytes, true);
        std::vector<uint8_t> interface_body;
        Put16(&interface_body, 1, true);
        Put16(&interface_body, 0, true);
        Put32(&interface_body, 65535, true);
        Put16(&interface_body, 14, true);
        Put16(&interface_body, 4, true);
        Put32(&interface_body, 1, true);
        Put16(&interface_body, 0, true);
        Put16(&interface_body, 0, true);
        AppendPcapngBlock(&bytes, 1, interface_body, true);
        AssertPcapngSourceError("pcapng_bad_tsoffset_length.pcapng", bytes);
    }
    {
        std::vector<uint8_t> bytes;
        AppendPcapngSectionHeader(&bytes, true);
        std::vector<uint8_t> interface_body;
        Put16(&interface_body, 1, true);
        Put16(&interface_body, 0, true);
        Put32(&interface_body, 65535, true);
        Put16(&interface_body, 2, true);
        Put16(&interface_body, 4, true);
        Put32(&interface_body, 0, true);
        AppendPcapngBlock(&bytes, 1, interface_body, true);
        AssertPcapngSourceError("pcapng_missing_option_terminator.pcapng", bytes);
    }
}

void TestManyPcapngSectionsIterative() {
    constexpr uint32_t kSectionCount = 32768;
    std::vector<uint8_t> bytes;
    bytes.reserve(static_cast<size_t>(kSectionCount) * 48);
    for (uint32_t index = 0; index < kSectionCount; ++index) {
        const bool little = (index & 1u) == 0;
        AppendPcapngSectionHeader(&bytes, little);
        AppendPcapngInterface(&bytes, little, 1);
        if (index + 1 == kSectionCount) {
            AppendPcapngEnhancedPacket(&bytes, little, 0, 0, {1});
        }
    }
    const std::string path = Temp("many_sections.pcapng");
    WriteFile(path, bytes);
    MockProtocol protocol;
    pcapfile::PcapFileSourceConfig config;
    config.path = path;
    config.format = "pcapng";
    pcapfile::PcapFileChannel channel("many_sections", config, &protocol);
    assert(channel.Open() == 0);
    const auto event = channel.PollBlock(0);
    assert(event.kind == flowsql::BlockPollEvent::kData && event.batch);
    assert(event.batch->num_rows() == 1);
    auto source = std::static_pointer_cast<arrow::UInt32Array>(event.batch->column(4));
    auto sequence = std::static_pointer_cast<arrow::UInt64Array>(event.batch->column(5));
    assert(source->Value(0) == kSectionCount - 1);
    assert(sequence->Value(0) == 0);
    assert(channel.ReleaseBlock(event.batch) == 0);
    assert(channel.PollBlock(0).kind == flowsql::BlockPollEvent::kEof);
}

void TestReplayModes() {
    const std::vector<uint64_t> timestamps_ns = {
        1000000000ULL,
        1006001001ULL,
        1006001002ULL,
        1003001002ULL,
        1012002003ULL,
    };
    const std::string path = Temp("replay.pcap");
    WriteFile(path, MakeNanosecondReplayPcap(timestamps_ns));

    {
        MockProtocol protocol;
        pcapfile::PcapFileSourceConfig config;
        config.path = path;
        config.format = "pcap";
        config.batch_packets = 8;
        config.replay_mode = pcapfile::PcapReplayMode::kFast;
        std::vector<int64_t> waits_ns;
        pcapfile::PcapFileChannel channel(
            "fast", config, &protocol,
            [&](std::chrono::nanoseconds delay) { waits_ns.push_back(delay.count()); });
        assert(channel.Open() == 0);
        const auto event = channel.PollBlock(0);
        assert(event.kind == flowsql::BlockPollEvent::kData && event.batch);
        assert(event.batch->num_rows() == static_cast<int64_t>(timestamps_ns.size()));
        auto timestamp = std::static_pointer_cast<arrow::Int64Array>(event.batch->column(0));
        auto sequence = std::static_pointer_cast<arrow::UInt64Array>(event.batch->column(5));
        auto raw = std::static_pointer_cast<arrow::BinaryArray>(event.batch->column(6));
        for (size_t index = 0; index < timestamps_ns.size(); ++index) {
            assert(timestamp->Value(index) == static_cast<int64_t>(timestamps_ns[index]));
            assert(sequence->Value(index) == index);
            assert(raw->GetView(index).size() == 1);
            assert(static_cast<uint8_t>(raw->GetView(index)[0]) == index + 1);
        }
        assert(waits_ns.empty());
        assert(channel.ReleaseBlock(event.batch) == 0);
        const auto eof = channel.PollBlock(0);
        AssertNoBatchEvent(eof, flowsql::BlockPollEvent::kEof);
        AssertNoBatchEvent(channel.PollBlock(0), flowsql::BlockPollEvent::kCancelled);
    }

    {
        MockProtocol protocol;
        pcapfile::PcapFileSourceConfig config;
        config.path = path;
        config.format = "pcap";
        config.batch_packets = 8;
        config.replay_mode = pcapfile::PcapReplayMode::kTimestamp;
        config.replay_speed_milli = 1000;
        std::vector<int64_t> waits_ns;
        pcapfile::PcapFileChannel channel(
            "timestamp_original_speed", config, &protocol,
            [&](std::chrono::nanoseconds delay) { waits_ns.push_back(delay.count()); });
        assert(channel.Open() == 0);
        auto event = channel.PollBlock(0);
        assert(event.kind == flowsql::BlockPollEvent::kData && waits_ns.empty());
        assert(channel.ReleaseBlock(event.batch) == 0);
        event = channel.PollBlock(0);
        assert(event.kind == flowsql::BlockPollEvent::kData);
        assert(waits_ns == std::vector<int64_t>{6001001});
        assert(channel.ReleaseBlock(event.batch) == 0);
    }

    {
        MockProtocol protocol;
        pcapfile::PcapFileSourceConfig config;
        config.path = path;
        config.format = "pcap";
        config.batch_packets = 8;
        config.replay_mode = pcapfile::PcapReplayMode::kTimestamp;
        config.replay_speed_milli = 3000;
        std::vector<int64_t> waits_ns;
        pcapfile::PcapFileChannel channel(
            "timestamp", config, &protocol,
            [&](std::chrono::nanoseconds delay) { waits_ns.push_back(delay.count()); });
        assert(channel.Open() == 0);
        for (size_t index = 0; index < timestamps_ns.size(); ++index) {
            const auto event = channel.PollBlock(0);
            assert(event.kind == flowsql::BlockPollEvent::kData && event.batch);
            assert(event.batch->num_rows() == 1);
            auto timestamp = std::static_pointer_cast<arrow::Int64Array>(event.batch->column(0));
            auto sequence = std::static_pointer_cast<arrow::UInt64Array>(event.batch->column(5));
            auto raw = std::static_pointer_cast<arrow::BinaryArray>(event.batch->column(6));
            assert(timestamp->Value(0) == static_cast<int64_t>(timestamps_ns[index]));
            assert(sequence->Value(0) == index);
            assert(raw->GetView(0).size() == 1);
            assert(static_cast<uint8_t>(raw->GetView(0)[0]) == index + 1);
            assert(channel.ReleaseBlock(event.batch) == 0);
            if (index == 0) assert(waits_ns.empty());
            if (index == 1) assert(waits_ns == std::vector<int64_t>{2000333});
            if (index == 2) assert(waits_ns == std::vector<int64_t>({2000333, 1}));
            if (index == 3) assert(waits_ns == std::vector<int64_t>({2000333, 1}));
        }
        assert(waits_ns == std::vector<int64_t>({2000333, 1, 3000333}));
        const auto eof = channel.PollBlock(0);
        AssertNoBatchEvent(eof, flowsql::BlockPollEvent::kEof);
        AssertNoBatchEvent(channel.PollBlock(0), flowsql::BlockPollEvent::kCancelled);
    }
}

void TestBackpressureAndCancelWakeup() {
    const std::string path = Temp("backpressure.pcap");
    WriteFile(path, MakeNanosecondReplayPcap({1000000000ULL, 1000000001ULL}));

    MockProtocol protocol;
    pcapfile::PcapFileSourceConfig config;
    config.path = path;
    config.format = "pcap";
    config.batch_packets = 1;
    pcapfile::PcapFileChannel channel("backpressure", config, &protocol);
    assert(channel.Open() == 0);
    const auto first = channel.PollBlock(0);
    assert(first.kind == flowsql::BlockPollEvent::kData && first.batch);
    assert(channel.OutstandingBatchCount() == 1);
    const auto timeout = channel.PollBlock(0);
    AssertNoBatchEvent(timeout, flowsql::BlockPollEvent::kTimeout);
    assert(timeout.err == 0);
    assert(channel.OutstandingBatchCount() == 1);

    auto next_poll = std::async(std::launch::async, [&]() { return channel.PollBlock(5000); });
    assert(next_poll.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
    assert(channel.OutstandingBatchCount() == 1);
    assert(channel.ReleaseBlock(first.batch) == 0);
    assert(next_poll.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    const auto second = next_poll.get();
    assert(second.kind == flowsql::BlockPollEvent::kData && second.batch);
    assert(channel.OutstandingBatchCount() == 1);
    auto sequence = std::static_pointer_cast<arrow::UInt64Array>(second.batch->column(5));
    assert(sequence->Value(0) == 1);
    assert(channel.ReleaseBlock(second.batch) == 0);
    assert(channel.ReleaseBlock(second.batch) == EINVAL);
    assert(channel.OutstandingBatchCount() == 0);

    assert(channel.Close() == 0);
    assert(channel.Open() == 0);
    const auto outstanding = channel.PollBlock(0);
    assert(outstanding.kind == flowsql::BlockPollEvent::kData && outstanding.batch);
    assert(channel.OutstandingBatchCount() == 1);
    auto cancelled_poll = std::async(std::launch::async, [&]() { return channel.PollBlock(5000); });
    assert(cancelled_poll.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
    channel.Cancel();
    assert(cancelled_poll.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    AssertNoBatchEvent(cancelled_poll.get(), flowsql::BlockPollEvent::kCancelled);
    assert(channel.OutstandingBatchCount() == 1);
    assert(channel.ReleaseBlock(outstanding.batch) == 0);
    assert(channel.ReleaseBlock(outstanding.batch) == EINVAL);
    assert(channel.OutstandingBatchCount() == 0);
    AssertNoBatchEvent(channel.PollBlock(0), flowsql::BlockPollEvent::kCancelled);
}

void TestReleaseBlockOwnership() {
    const std::string path = Temp("release_ownership.pcap");
    WriteFile(path, MakeClassicPcap(true, false, 0));

    MockProtocol protocol;
    pcapfile::PcapFileSourceConfig config;
    config.path = path;
    pcapfile::PcapFileChannel first_channel("release_first", config, &protocol);
    pcapfile::PcapFileChannel second_channel("release_second", config, &protocol);
    assert(first_channel.Open() == 0);
    assert(second_channel.Open() == 0);
    assert(first_channel.ReleaseBlock(std::shared_ptr<arrow::RecordBatch>{}) == EINVAL);

    const auto first = first_channel.PollBlock(0);
    const auto second = second_channel.PollBlock(0);
    assert(first.kind == flowsql::BlockPollEvent::kData && first.batch);
    assert(second.kind == flowsql::BlockPollEvent::kData && second.batch);
    assert(first_channel.OutstandingBatchCount() == 1);
    assert(second_channel.OutstandingBatchCount() == 1);

    assert(first_channel.ReleaseBlock(second.batch) == EINVAL);
    assert(second_channel.ReleaseBlock(first.batch) == EINVAL);
    const auto sliced = first.batch->Slice(0, first.batch->num_rows());
    assert(sliced.get() != first.batch.get());
    assert(first_channel.ReleaseBlock(sliced) == EINVAL);
    assert(first_channel.OutstandingBatchCount() == 1);
    assert(second_channel.OutstandingBatchCount() == 1);

    assert(first_channel.ReleaseBlock(first.batch) == 0);
    assert(first_channel.ReleaseBlock(first.batch) == EINVAL);
    assert(second_channel.ReleaseBlock(second.batch) == 0);
    assert(first_channel.OutstandingBatchCount() == 0);
    assert(second_channel.OutstandingBatchCount() == 0);
}

void TestCancelInterruptsReplayWait() {
    const std::string path = Temp("cancel_replay_wait.pcap");
    WriteFile(path, MakeNanosecondReplayPcap({1000000000ULL, 11000000000ULL}));

    MockProtocol protocol;
    pcapfile::PcapFileSourceConfig config;
    config.path = path;
    config.format = "pcap";
    config.replay_mode = pcapfile::PcapReplayMode::kTimestamp;
    pcapfile::PcapFileChannel channel("cancel_replay", config, &protocol);
    assert(channel.Open() == 0);
    const auto first = channel.PollBlock(0);
    assert(first.kind == flowsql::BlockPollEvent::kData && first.batch);
    assert(channel.ReleaseBlock(first.batch) == 0);

    auto replay_poll = std::async(std::launch::async, [&]() { return channel.PollBlock(0); });
    assert(replay_poll.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
    assert(channel.Close() == EBUSY);
    channel.Cancel();
    assert(replay_poll.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    AssertNoBatchEvent(replay_poll.get(), flowsql::BlockPollEvent::kCancelled);
    assert(channel.Close() == 0);
}

void TestBatchOwnerAndPluginLifecycle() {
    const std::string path = Temp("owner_lifecycle.pcap");
    WriteFile(path, MakeClassicPcap(true, false, 0));

    std::shared_ptr<arrow::RecordBatch> retained;
    {
        MockProtocol protocol;
        pcapfile::PcapFileSourceConfig config;
        config.path = path;
        auto channel = std::make_unique<pcapfile::PcapFileChannel>("owner", config, &protocol);
        assert(channel->Open() == 0);
        const auto event = channel->PollBlock(0);
        assert(event.kind == flowsql::BlockPollEvent::kData && event.batch);
        retained = event.batch;
        assert(channel->Close() == EBUSY);
    }
    assert(retained);
    assert(retained->num_rows() == 1);
    assert(retained->schema()->Equals(*flowsql::packet::PacketSchema()));
    auto retained_timestamp = std::static_pointer_cast<arrow::Int64Array>(retained->column(0));
    auto retained_raw = std::static_pointer_cast<arrow::BinaryArray>(retained->column(6));
    assert(retained_timestamp->Value(0) == 2000000000LL);
    assert(retained_raw->GetView(0) == "\x01\x02\x03\x04");

    MockProtocol protocol;
    pcapfile::PcapFilePlugin plugin;
    class Querier final : public flowsql::IQuerier {
     public:
        explicit Querier(flowsql::IProtocol* protocol) : protocol_(protocol) {}
        int Traverse(const flowsql::Guid& iid, fntraverse callback) override {
            if (iid < flowsql::IID_PROTOCOL || flowsql::IID_PROTOCOL < iid) return 0;
            return callback ? callback(protocol_) : 0;
        }
        void* First(const flowsql::Guid& iid) override {
            if (!(iid < flowsql::IID_PROTOCOL) && !(flowsql::IID_PROTOCOL < iid)) return protocol_;
            return nullptr;
        }

     private:
        flowsql::IProtocol* protocol_;
    } querier(&protocol);
    assert(plugin.Load(&querier) == 0);
    assert(plugin.Start() == 0);
    assert(plugin.AddChannel("pcapfile", "owner", "{\"path\":\"" + path + "\"}") == 0);
    auto* channel = plugin.Get("pcapfile", "owner");
    assert(channel != nullptr);
    const auto event = channel->PollBlock(0);
    assert(event.kind == flowsql::BlockPollEvent::kData && event.batch);
    assert(plugin.Stop() == EBUSY);
    assert(plugin.Unload() == EBUSY);
    assert(plugin.Get("pcapfile", "owner") == channel);
    assert(channel->ReleaseBlock(event.batch) == 0);
    assert(plugin.Stop() == 0);
    assert(plugin.Unload() == 0);
    auto released_raw = std::static_pointer_cast<arrow::BinaryArray>(event.batch->column(6));
    assert(released_raw->GetView(0) == "\x01\x02\x03\x04");
}

void TestMalformedPcapngAndFormatMismatch() {
    std::vector<uint8_t> bytes;
    AppendPcapngSectionHeader(&bytes, true);
    AppendPcapngInterface(&bytes, true, 1);
    AppendPcapngEnhancedPacket(&bytes, true, 0, 0, {1, 2, 3});
    AssertCaptureSourceError("pcapng_declared_as_pcap.pcapng", "pcap", bytes);
    bytes[bytes.size() - 1] = 0;
    bytes.back() = 0xff;
    const std::string path = Temp("bad_pcapng.pcapng");
    WriteFile(path, bytes);
    MockProtocol protocol;
    pcapfile::PcapFileSourceConfig config;
    config.path = path;
    config.format = "pcapng";
    pcapfile::PcapFileChannel channel("bad", config, &protocol);
    assert(channel.Open() == 0);
    const auto error = channel.PollBlock(0);
    AssertNoBatchEvent(error, flowsql::BlockPollEvent::kError);
    assert(error.err != 0);
    AssertNoBatchEvent(channel.PollBlock(0), flowsql::BlockPollEvent::kCancelled);

    const std::string pcap_path = Temp("format_mismatch.pcap");
    WriteFile(pcap_path, MakeClassicPcap(true, false, 0));
    config.path = pcap_path;
    config.format = "pcapng";
    pcapfile::PcapFileChannel mismatch("mismatch", config, &protocol);
    assert(mismatch.Open() != 0);
    const auto mismatch_error = mismatch.PollBlock(0);
    AssertNoBatchEvent(mismatch_error, flowsql::BlockPollEvent::kError);
    assert(mismatch_error.err != 0);
    AssertNoBatchEvent(mismatch.PollBlock(0), flowsql::BlockPollEvent::kCancelled);
}

void TestIncrementalReadAfterOpen() {
    {
        const std::string path = Temp("classic_truncated_after_open.pcap");
        auto bytes = MakeClassicPcap(true, false, 0);
        WriteFile(path, bytes);
        MockProtocol protocol;
        pcapfile::PcapFileSourceConfig config;
        config.path = path;
        config.format = "pcap";
        pcapfile::PcapFileChannel channel("classic_truncated_after_open", config, &protocol);
        assert(channel.Open() == 0);
        bytes.resize(24);
        WriteFile(path, bytes);
        const auto error = channel.PollBlock(0);
        assert(error.kind == flowsql::BlockPollEvent::kError && !error.batch && error.err != 0);
        assert(channel.PollBlock(0).kind == flowsql::BlockPollEvent::kCancelled);
    }
    {
        const std::string path = Temp("pcapng_truncated_after_open.pcapng");
        std::vector<uint8_t> bytes;
        AppendPcapngSectionHeader(&bytes, true);
        const size_t section_header_size = bytes.size();
        AppendPcapngInterface(&bytes, true, 1);
        AppendPcapngEnhancedPacket(&bytes, true, 0, 0, {1, 2, 3, 4});
        WriteFile(path, bytes);
        MockProtocol protocol;
        pcapfile::PcapFileSourceConfig config;
        config.path = path;
        config.format = "pcapng";
        pcapfile::PcapFileChannel channel("pcapng_truncated_after_open", config, &protocol);
        assert(channel.Open() == 0);
        bytes.resize(section_header_size);
        WriteFile(path, bytes);
        const auto error = channel.PollBlock(0);
        assert(error.kind == flowsql::BlockPollEvent::kError && !error.batch && error.err != 0);
        assert(channel.PollBlock(0).kind == flowsql::BlockPollEvent::kCancelled);
    }
    {
        std::vector<uint8_t> bytes;
        AppendPcapngSectionHeader(&bytes, true);
        AppendPcapngInterface(&bytes, true, 1);
        Put32(&bytes, 6, true);
        Put32(&bytes, 0xfffffffcU, true);
        Put32(&bytes, 0, true);
        AssertPcapngSourceError("oversized_declared_block.pcapng", bytes);
    }
    {
        std::vector<uint8_t> bytes = {0xd4, 0xc3, 0xb2, 0xa1};
        Put16(&bytes, 2, true);
        Put16(&bytes, 4, true);
        Put32(&bytes, 0, true);
        Put32(&bytes, 0, true);
        Put32(&bytes, 0xffffffffU, true);
        Put32(&bytes, 1, true);
        Put32(&bytes, 0, true);
        Put32(&bytes, 0, true);
        Put32(&bytes, 0xffffffffU, true);
        Put32(&bytes, 0xffffffffU, true);
        const std::string path = Temp("oversized_declared_packet.pcap");
        WriteFile(path, bytes);
        MockProtocol protocol;
        pcapfile::PcapFileSourceConfig config;
        config.path = path;
        config.format = "pcap";
        pcapfile::PcapFileChannel channel("oversized_declared_packet", config, &protocol);
        assert(channel.Open() == 0);
        const auto error = channel.PollBlock(0);
        assert(error.kind == flowsql::BlockPollEvent::kError && !error.batch && error.err != 0);
        assert(channel.PollBlock(0).kind == flowsql::BlockPollEvent::kCancelled);
    }
}

void TestCancelAndManagerBusy() {
    const std::string path = Temp("manager_busy.pcap");
    auto bytes = MakeClassicPcap(true, false, 0);
    WriteFile(path, bytes);
    MockProtocol protocol;
    pcapfile::PcapFilePlugin plugin;
    class Querier final : public flowsql::IQuerier {
     public:
        explicit Querier(flowsql::IProtocol* protocol) : protocol_(protocol) {}
        int Traverse(const flowsql::Guid& iid, fntraverse callback) override {
            if (iid < flowsql::IID_PROTOCOL || flowsql::IID_PROTOCOL < iid) return 0;
            return callback ? callback(protocol_) : 0;
        }
        void* First(const flowsql::Guid& iid) override {
            if (!(iid < flowsql::IID_PROTOCOL) && !(flowsql::IID_PROTOCOL < iid)) return protocol_;
            return nullptr;
        }
     private:
        flowsql::IProtocol* protocol_;
    } querier(&protocol);
    assert(plugin.Load(&querier) == 0);
    assert(plugin.Start() == 0);
    const std::string option = "{\"path\":\"" + path + "\",\"batch_packets\":1}";
    assert(plugin.AddChannel("pcapfile", "busy", option) == 0);
    auto* channel = dynamic_cast<pcapfile::PcapFileChannel*>(plugin.Get("pcapfile", "busy"));
    assert(channel != nullptr);
    auto event = channel->PollBlock(0);
    assert(event.kind == flowsql::BlockPollEvent::kData && event.batch);
    assert(channel->OutstandingBatchCount() == 1);
    assert(channel->Close() == EBUSY);
    assert(plugin.ModifyChannel("pcapfile", "busy", option) == EBUSY);
    assert(plugin.Get("pcapfile", "busy") == channel);
    assert(plugin.RemoveChannel("pcapfile", "busy") == EBUSY);
    assert(plugin.Get("pcapfile", "busy") == channel);
    assert(channel->OutstandingBatchCount() == 1);
    assert(channel->ReleaseBlock(event.batch) == 0);
    assert(channel->OutstandingBatchCount() == 0);
    channel->Cancel();
    AssertNoBatchEvent(channel->PollBlock(0), flowsql::BlockPollEvent::kCancelled);
    assert(plugin.RemoveChannel("pcapfile", "busy") == 0);
    assert(plugin.Get("pcapfile", "busy") == nullptr);
    plugin.Unload();
}

void TestOptions() {
    pcapfile::PcapFileSourceConfig config;
    std::string normalized;
    std::string error;
    assert(pcapfile::ParsePcapFileSourceConfig("{\"path\":\"/tmp/none\",\"batch_packets\":0}", &config, &normalized, &error) != 0);
    assert(pcapfile::ParsePcapFileSourceConfig("{\"path\":\"/tmp/none\",\"unknown\":1}", &config, &normalized, &error) != 0);

    const std::string path = Temp("options_valid.pcap");
    WriteFile(path, MakeClassicPcap(true, false, 0));
    const std::string option = "{\"path\":\"" + path +
                               "\",\"format\":\"PCAP\",\"batch_packets\":4,"
                               "\"replay_mode\":\"TIMESTAMP\",\"replay_speed_milli\":2000}";
    assert(pcapfile::ParsePcapFileSourceConfig(option, &config, &normalized, &error) == 0);
    assert(config.path == path);
    assert(config.format == "pcap");
    assert(config.batch_packets == 4);
    assert(config.replay_mode == pcapfile::PcapReplayMode::kTimestamp);
    assert(config.replay_speed_milli == 2000);
    assert(normalized == "{\"path\":\"" + path +
                             "\",\"format\":\"pcap\",\"batch_packets\":4,"
                             "\"replay_mode\":\"timestamp\",\"replay_speed_milli\":2000}");
}

void TestPluginDependencyLifecycle() {
    MockProtocol protocol;
    DeferredProtocolQuerier querier;

    pcapfile::PcapFilePlugin delayed;
    assert(delayed.Load(&querier) == 0);
    assert(delayed.Start() == ENODEV);
    querier.SetProtocol(&protocol);
    assert(delayed.Start() == 0);
    assert(delayed.Unload() == 0);

    pcapfile::PcapFilePlugin available;
    assert(available.Load(&querier) == 0);
    assert(available.Start() == 0);
    assert(available.Unload() == 0);

    pcapfile::PcapFilePlugin missing;
    assert(missing.Load(nullptr) == 0);
    assert(missing.Start() == ENODEV);
    assert(missing.Unload() == 0);
}

void TestExclusiveReaderFactory() {
    const std::string path = Temp("exclusive_reader.pcap");
    WriteFile(path, MakeClassicPcap(true, false, 0));

    MockProtocol protocol;
    DeferredProtocolQuerier querier;
    querier.SetProtocol(&protocol);
    pcapfile::PcapFilePlugin plugin;
    assert(plugin.Load(&querier) == 0);
    assert(plugin.Start() == 0);
    assert(plugin.AddChannel(
               "pcapfile", "exclusive",
               "{\"path\":\"" + path + "\",\"batch_packets\":1}") == 0);
    auto* managed = plugin.Get("pcapfile", "exclusive");
    assert(managed != nullptr);

    std::string task_one = "reader-task-one";
    std::string category_one = "pcapfile";
    std::string source_one = "exclusive";
    std::string plan_one = BuildPcapCanonicalPlan("timestamp_ns >= 10");
    flowsql::BlockStreamReaderConfigV1 config_one;
    config_one.task_id = task_one.c_str();
    config_one.source_category = category_one.c_str();
    config_one.source_name = source_one.c_str();
    config_one.pushed_filter_plan_json = plan_one.c_str();
    flowsql::IBlockStreamChannel* reader_one = nullptr;
    assert(plugin.CreateReader(config_one, &reader_one) == 0);
    assert(reader_one != nullptr && reader_one != managed && reader_one->IsOpened());

    std::string plan_two = BuildPcapCanonicalPlan("timestamp_ns < 10");
    flowsql::BlockStreamReaderConfigV1 config_two;
    config_two.task_id = "reader-task-two";
    config_two.source_category = "PCAPFILE";
    config_two.source_name = "exclusive";
    config_two.pushed_filter_plan_json = plan_two.c_str();
    flowsql::IBlockStreamChannel* reader_two = nullptr;
    assert(plugin.CreateReader(config_two, &reader_two) == 0);
    assert(reader_two != nullptr && reader_two != managed && reader_two != reader_one);
    assert(reader_two->IsOpened());

    auto* typed_reader_one = dynamic_cast<pcapfile::PcapFileChannel*>(reader_one);
    auto* typed_reader_two = dynamic_cast<pcapfile::PcapFileChannel*>(reader_two);
    assert(typed_reader_one && typed_reader_two);
    packet::PacketMeta plan_meta;
    plan_meta.timestamp_ns = 10;
    assert(typed_reader_one->EvaluateHeaderFilter(plan_meta) ==
           pcapfile::PacketHeaderFilterResult::kMatch);
    assert(typed_reader_two->EvaluateHeaderFilter(plan_meta) ==
           pcapfile::PacketHeaderFilterResult::kReject);

    task_one = "caller-task-mutated";
    category_one = "caller-category-mutated";
    source_one = "caller-source-mutated";
    plan_one = "caller-plan-mutated";
    plan_two = "caller-plan-two-mutated";
    assert(typed_reader_one->EvaluateHeaderFilter(plan_meta) ==
           pcapfile::PacketHeaderFilterResult::kMatch);
    assert(typed_reader_two->EvaluateHeaderFilter(plan_meta) ==
           pcapfile::PacketHeaderFilterResult::kReject);
    assert(std::string(reader_one->Category()) == "pcapfile");
    assert(std::string(reader_one->Name()) == "exclusive");
    assert(plugin.RemoveChannel("pcapfile", "exclusive") == EBUSY);
    assert(plugin.Get("pcapfile", "exclusive") == managed);
    assert(plugin.Stop() == EBUSY);
    assert(plugin.Unload() == EBUSY);

    const auto first = reader_one->PollBlock(0);
    const auto second = reader_two->PollBlock(0);
    assert(first.kind == flowsql::BlockPollEvent::kData && first.batch);
    AssertNoBatchEvent(second, flowsql::BlockPollEvent::kEof);
    auto first_sequence =
        std::static_pointer_cast<arrow::UInt64Array>(first.batch->column(5));
    assert(first_sequence->Value(0) == 0);
    assert(reader_one->ReleaseBlock(first.batch) == 0);
    reader_one->Cancel();
    AssertNoBatchEvent(
        reader_one->PollBlock(0), flowsql::BlockPollEvent::kCancelled);
    AssertNoBatchEvent(
        reader_two->PollBlock(0), flowsql::BlockPollEvent::kCancelled);

    plugin.ReleaseReader(reader_one);
    assert(plugin.Stop() == EBUSY);
    assert(plugin.RemoveChannel("pcapfile", "exclusive") == EBUSY);
    plugin.ReleaseReader(reader_two);
    assert(plugin.Get("pcapfile", "exclusive") == managed);
    assert(plugin.RemoveChannel("pcapfile", "exclusive") == 0);
    assert(plugin.Get("pcapfile", "exclusive") == nullptr);
    assert(plugin.Stop() == 0);
    assert(plugin.Unload() == 0);

    pcapfile::PcapFilePlugin validation_plugin;
    assert(validation_plugin.Load(&querier) == 0);
    assert(validation_plugin.Start() == 0);
    assert(validation_plugin.AddChannel(
               "pcapfile", "validation",
               "{\"path\":\"" + path + "\"}") == 0);
    flowsql::BlockStreamReaderConfigV1 invalid;
    invalid.task_id = "invalid-task";
    invalid.source_category = "pcapfile";
    invalid.source_name = "validation";
    invalid.pushed_filter_plan_json = flowsql::kEmptyCanonicalFilterPlanV1;
    auto* rejected = reinterpret_cast<flowsql::IBlockStreamChannel*>(1);
    invalid.contract_version = flowsql::kBlockStreamReaderContractVersionV1 + 1;
    assert(validation_plugin.CreateReader(invalid, &rejected) == EINVAL);
    assert(rejected == nullptr);
    invalid.contract_version = flowsql::kBlockStreamReaderContractVersionV1;
    invalid.task_id = nullptr;
    rejected = reinterpret_cast<flowsql::IBlockStreamChannel*>(1);
    assert(validation_plugin.CreateReader(invalid, &rejected) == EINVAL);
    assert(rejected == nullptr);
    invalid.task_id = "invalid-task";
    invalid.source_category = "other";
    rejected = reinterpret_cast<flowsql::IBlockStreamChannel*>(1);
    assert(validation_plugin.CreateReader(invalid, &rejected) == ENOTSUP);
    assert(rejected == nullptr);
    invalid.source_category = "pcapfile";
    invalid.source_name = "missing";
    rejected = reinterpret_cast<flowsql::IBlockStreamChannel*>(1);
    assert(validation_plugin.CreateReader(invalid, &rejected) == ENOTSUP);
    assert(rejected == nullptr);
    invalid.source_name = "validation";
    invalid.pushed_filter_plan_json = "{\"version\":1,\"root\":{}}";
    rejected = reinterpret_cast<flowsql::IBlockStreamChannel*>(1);
    assert(validation_plugin.CreateReader(invalid, &rejected) == EINVAL);
    assert(rejected == nullptr);
    assert(validation_plugin.CreateReader(invalid, nullptr) == EINVAL);
    assert(validation_plugin.Unload() == 0);
}

void AssertDynamicPluginOrder(bool pcapfile_first) {
    flowsql::PluginLoader* loader = flowsql::PluginLoader::Single();
    loader->StopAll();
    loader->Unload();

    const std::string npi_option = std::string("{\"ldfile\":\"") + FLOWSQL_NPI_PROTOCOLS_PATH + "\"}";
    const char* libraries[] = {
        pcapfile_first ? FLOWSQL_PCAPFILE_PLUGIN_PATH : FLOWSQL_NPI_PLUGIN_PATH,
        pcapfile_first ? FLOWSQL_NPI_PLUGIN_PATH : FLOWSQL_PCAPFILE_PLUGIN_PATH,
    };
    const char* options[] = {
        pcapfile_first ? nullptr : npi_option.c_str(),
        pcapfile_first ? npi_option.c_str() : nullptr,
    };

    assert(loader->Load(".", libraries, options, 2) == 0);
    assert(loader->StartAll() == 0);
    assert(loader->First(flowsql::IID_PROTOCOL) != nullptr);
    assert(loader->First(flowsql::IID_BLOCK_STREAM_FACTORY) != nullptr);
    assert(loader->First(flowsql::IID_BLOCK_STREAM_READER_FACTORY_V1) != nullptr);
    assert(loader->First(flowsql::IID_FILTER_PUSHDOWN_V1) != nullptr);
    loader->StopAll();
    assert(loader->Unload() == 0);
}

void TestDynamicPluginDependencyOrders() {
    AssertDynamicPluginOrder(true);
    AssertDynamicPluginOrder(false);

    flowsql::PluginLoader* loader = flowsql::PluginLoader::Single();
    const char* libraries[] = {FLOWSQL_PCAPFILE_PLUGIN_PATH};
    const char* options[] = {nullptr};
    assert(loader->Load(".", libraries, options, 1) == 0);
    assert(loader->StartAll() != 0);
    loader->StopAll();
    assert(loader->Unload() == 0);
}

void TestPluginManager() {
    MockProtocol protocol;
    pcapfile::PcapFilePlugin plugin;
    class Querier final : public flowsql::IQuerier {
     public:
        explicit Querier(flowsql::IProtocol* protocol) : protocol_(protocol) {}
        int Traverse(const flowsql::Guid& iid, fntraverse callback) override {
            if (iid < flowsql::IID_PROTOCOL || flowsql::IID_PROTOCOL < iid) return 0;
            return callback ? callback(protocol_) : 0;
        }
        void* First(const flowsql::Guid& iid) override {
            if (!(iid < flowsql::IID_PROTOCOL) && !(flowsql::IID_PROTOCOL < iid)) return protocol_;
            return nullptr;
        }
     private:
        flowsql::IProtocol* protocol_;
    } querier(&protocol);
    assert(plugin.Load(&querier) == 0);
    assert(plugin.Start() == 0);
    const std::string path = Temp("manager.pcap");
    WriteFile(path, {});
    assert(plugin.AddChannel("pcapfile", "one", "{\"path\":\"" + path + "\"}") == 0);
    auto* channel = plugin.Get("PCAPFILE", "one");
    assert(channel != nullptr);
    assert(std::string(channel->Category()) == "pcapfile");
    assert(std::string(channel->Name()) == "one");
    assert(std::string(channel->Type()) == flowsql::ChannelType::kBlockStream);
    assert(std::string(channel->Schema()) == "packet");
    assert(channel->Flush() == 0);
    int listed = 0;
    plugin.List([&](const char* type, const char* name, flowsql::IBlockStreamChannel* listed_channel) {
        assert(std::string(type) == "pcapfile");
        assert(std::string(name) == "one");
        assert(listed_channel == channel);
        ++listed;
    });
    assert(listed == 1);
    assert(plugin.AddChannel("other", "x", "{}") == ENOTSUP);
    assert(plugin.ModifyChannel("pcapfile", "missing", "{}") == ENOENT);
    assert(plugin.ModifyChannel("pcapfile", "one", "{\"path\":\"" + path + "\"}") == 0);
    assert(plugin.RemoveChannel("pcapfile", "one") == 0);
    assert(plugin.RemoveChannel("pcapfile", "one") == ENOENT);
    plugin.Unload();
}

void AssertPcapFileChannelStoreSchema(const std::string& db_path) {
    sqlite3* db = nullptr;
    assert(sqlite3_open_v2(db_path.c_str(), &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr) == SQLITE_OK);

    sqlite3_stmt* statement = nullptr;
    assert(sqlite3_prepare_v2(db,
                              "SELECT sql FROM sqlite_master "
                              "WHERE type='table' AND name='pcapfile_channel_store';",
                              -1, &statement, nullptr) == SQLITE_OK);
    assert(sqlite3_step(statement) == SQLITE_ROW);
    const auto* schema_text = sqlite3_column_text(statement, 0);
    assert(schema_text != nullptr);
    const std::string schema(reinterpret_cast<const char*>(schema_text));
    assert(schema.find("pcapfile_channel_store") != std::string::npos);
    assert(schema.find("CHECK(type = 'pcapfile')") != std::string::npos);
    sqlite3_finalize(statement);

    statement = nullptr;
    assert(sqlite3_prepare_v2(db, "PRAGMA journal_mode;", -1, &statement, nullptr) == SQLITE_OK);
    assert(sqlite3_step(statement) == SQLITE_ROW);
    const auto* journal_mode = sqlite3_column_text(statement, 0);
    assert(journal_mode != nullptr);
    assert(std::string(reinterpret_cast<const char*>(journal_mode)) == "wal");
    sqlite3_finalize(statement);
    assert(sqlite3_close(db) == SQLITE_OK);
}

void TestPcapFileChannelStore() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path root =
        std::filesystem::path("/tmp") / ("flowsql_pcapfile_channel_store_" + std::to_string(unique));
    const std::filesystem::path directory_db_path = root / "directory.sqlite";
    const std::filesystem::path db_path = root / "nested" / "pcapfile.sqlite";

    pcapfile::PcapFileChannelStore store;
    std::string error;
    std::vector<pcapfile::PcapFileChannelRecord> records = {{"sentinel", "sentinel", "sentinel"}};
    assert(store.LoadAll(&records, &error) == ENODEV);
    assert(!error.empty());
    assert(records.size() == 1 && records[0].type == "sentinel");

    const pcapfile::PcapFileChannelRecord closed_record = {"pcapfile", "closed", "{}"};
    error.clear();
    assert(store.Insert(closed_record, &error) == ENODEV);
    assert(!error.empty());
    error.clear();
    assert(store.Update(closed_record, &error) == ENODEV);
    assert(!error.empty());
    error.clear();
    assert(store.Erase(closed_record.type, closed_record.name, &error) == ENODEV);
    assert(!error.empty());

    error.clear();
    assert(store.Open("", &error) == EINVAL);
    assert(!error.empty());

    assert(std::filesystem::create_directories(directory_db_path));
    error.clear();
    assert(store.Open(directory_db_path.string(), &error) != 0);
    assert(!error.empty());

    error = "stale error";
    assert(store.Open(db_path.string(), &error) == 0);
    assert(error.empty());
    assert(std::filesystem::is_regular_file(db_path));
    AssertPcapFileChannelStoreSchema(db_path.string());

    records.clear();
    error = "stale error";
    assert(store.LoadAll(&records, &error) == 0);
    assert(error.empty());
    assert(records.empty());

    const pcapfile::PcapFileChannelRecord zeta = {
        "pcapfile", "zeta", "{\"path\":\"/tmp/zeta.pcap\",\"format\":\"pcap\"}"};
    const pcapfile::PcapFileChannelRecord quoted = {
        "pcapfile", "capture'); DROP TABLE pcapfile_channel_store; --",
        "{\"path\":\"/tmp/it's-a-capture.pcap\",\"format\":\"pcap\",\"batch_packets\":2}"};
    assert(store.Insert(zeta, &error) == 0);
    assert(store.Insert(quoted, &error) == 0);
    error.clear();
    assert(store.Insert(quoted, &error) == EEXIST);
    assert(!error.empty());

    error.clear();
    assert(store.Insert({"other", "invalid-type", "{}"}, &error) != 0);
    assert(!error.empty());

    records.clear();
    assert(store.LoadAll(&records, &error) == 0);
    assert(records.size() == 2);
    assert(records[0].type == quoted.type);
    assert(records[0].name == quoted.name);
    assert(records[0].option == quoted.option);
    assert(records[1].type == zeta.type);
    assert(records[1].name == zeta.name);
    assert(records[1].option == zeta.option);

    pcapfile::PcapFileChannelRecord updated = quoted;
    updated.option =
        "{\"path\":\"/tmp/it's-updated.pcapng\",\"format\":\"pcapng\",\"batch_packets\":4}";
    assert(store.Update(updated, &error) == 0);
    error.clear();
    assert(store.Update({"pcapfile", "missing", "{}"}, &error) == ENOENT);
    assert(!error.empty());

    assert(store.Erase(zeta.type, zeta.name, &error) == 0);
    error.clear();
    assert(store.Erase(zeta.type, zeta.name, &error) == ENOENT);
    assert(!error.empty());

    store.Close();
    store.Close();
    error.clear();
    assert(store.LoadAll(&records, &error) == ENODEV);
    assert(!error.empty());

    error = "stale error";
    assert(store.Open(db_path.string(), &error) == 0);
    assert(error.empty());
    records.clear();
    assert(store.LoadAll(&records, &error) == 0);
    assert(records.size() == 1);
    assert(records[0].type == updated.type);
    assert(records[0].name == updated.name);
    assert(records[0].option == updated.option);
    store.Close();

    assert(std::filesystem::remove(db_path));
    std::filesystem::remove(db_path.string() + "-wal");
    std::filesystem::remove(db_path.string() + "-shm");
    assert(std::filesystem::remove(db_path.parent_path()));
    assert(std::filesystem::remove(directory_db_path));
    assert(std::filesystem::remove(root));
}

void TestPcapFilePluginPersistentStartupRecovery() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path root =
        std::filesystem::path("/tmp") / ("flowsql_pcapfile_recovery_" + std::to_string(unique));
    const std::filesystem::path db_path = root / "meta" / "pcapfile.sqlite";
    const std::filesystem::path first_path = root / "first.pcap";
    const std::filesystem::path second_path = root / "second.pcap";
    assert(std::filesystem::create_directories(root));
    WriteFile(first_path.string(), MakeClassicPcap(true, false, 0));
    WriteFile(second_path.string(), MakeClassicPcap(true, false, 0));

    const pcapfile::PcapFileChannelRecord first = {
        "pcapfile", "first",
        "{\"path\":\"" + first_path.string() +
            "\",\"format\":\"pcap\",\"batch_packets\":1,\"replay_mode\":\"fast\","
            "\"replay_speed_milli\":1000}"};
    const pcapfile::PcapFileChannelRecord second = {
        "pcapfile", "second",
        "{\"path\":\"" + second_path.string() +
            "\",\"format\":\"pcap\",\"batch_packets\":2,\"replay_mode\":\"fast\","
            "\"replay_speed_milli\":1000}"};
    pcapfile::PcapFileChannelStore seed;
    std::string error;
    assert(seed.Open(db_path.string(), &error) == 0);
    assert(seed.Insert(second, &error) == 0);
    assert(seed.Insert(first, &error) == 0);
    seed.Close();

    MockProtocol protocol;
    DeferredProtocolQuerier querier;
    querier.SetProtocol(&protocol);
    pcapfile::PcapFilePlugin plugin;
    const std::string plugin_option = "ignored=value;db_path=" + db_path.string() + ";unused=value";
    assert(plugin.Option(plugin_option.c_str()) == 0);
    assert(plugin.Load(&querier) == 0);
    assert(plugin.Start() == 0);
    auto* first_channel = dynamic_cast<pcapfile::PcapFileChannel*>(plugin.Get("pcapfile", "first"));
    assert(first_channel != nullptr && first_channel->IsOpened());
    assert(plugin.Get("pcapfile", "second") != nullptr);
    std::map<std::string, std::string> recovered_options;
    plugin.QueryChannels([&](const std::string& type, const std::string& name, const std::string& option,
                             const std::string&) {
        assert(type == "pcapfile");
        recovered_options[name] = option;
    });
    assert(recovered_options.size() == 2);
    assert(recovered_options["first"] == first.option);
    assert(recovered_options["second"] == second.option);

    const auto first_batch = first_channel->PollBlock(0);
    assert(first_batch.kind == flowsql::BlockPollEvent::kData && first_batch.batch);
    assert(first_channel->ReleaseBlock(first_batch.batch) == 0);
    assert(plugin.Stop() == 0);
    assert(!first_channel->IsOpened());
    assert(plugin.Get("pcapfile", "first") == first_channel);
    assert(plugin.Start() == 0);
    auto* same_instance_restart = plugin.Get("pcapfile", "first");
    assert(same_instance_restart != nullptr && same_instance_restart->IsOpened());
    assert(plugin.Unload() == 0);
    assert(plugin.Get("pcapfile", "first") == nullptr);

    pcapfile::PcapFilePlugin restarted;
    assert(restarted.Option(("db_path=" + db_path.string()).c_str()) == 0);
    assert(restarted.Load(&querier) == 0);
    assert(restarted.Start() == 0);
    auto* restarted_channel = restarted.Get("pcapfile", "first");
    assert(restarted_channel != nullptr && restarted_channel->IsOpened());
    const auto restarted_batch = restarted_channel->PollBlock(0);
    assert(restarted_batch.kind == flowsql::BlockPollEvent::kData && restarted_batch.batch);
    assert(restarted_channel->ReleaseBlock(restarted_batch.batch) == 0);
    assert(restarted.Unload() == 0);

    WriteFile(second_path.string(), {0x00, 0x01, 0x02});
    pcapfile::PcapFilePlugin corrupt_capture;
    assert(corrupt_capture.Option(("db_path=" + db_path.string()).c_str()) == 0);
    assert(corrupt_capture.Load(&querier) == 0);
    assert(corrupt_capture.Start() != 0);
    assert(corrupt_capture.Get("pcapfile", "first") == nullptr);
    assert(corrupt_capture.Unload() == 0);
    WriteFile(second_path.string(), MakeClassicPcap(true, false, 0));

    pcapfile::PcapFileChannelStore verify;
    assert(verify.Open(db_path.string(), &error) == 0);
    std::vector<pcapfile::PcapFileChannelRecord> records;
    assert(verify.LoadAll(&records, &error) == 0);
    assert(records.size() == 2);
    verify.Close();

    pcapfile::PcapFilePlugin explicit_volatile;
    assert(explicit_volatile.Option(("db_path=" + (root / "not-a-db").string()).c_str()) == 0);
    assert(explicit_volatile.Option("db_path=") == 0);
    assert(explicit_volatile.Load(&querier) == 0);
    assert(explicit_volatile.Start() == 0);
    assert(explicit_volatile.Unload() == 0);

    const std::filesystem::path directory_db_path = root / "directory.sqlite";
    assert(std::filesystem::create_directory(directory_db_path));
    pcapfile::PcapFilePlugin invalid_store;
    assert(invalid_store.Option(("db_path=" + directory_db_path.string()).c_str()) == 0);
    assert(invalid_store.Load(&querier) == 0);
    assert(invalid_store.Start() != 0);
    assert(invalid_store.Get("pcapfile", "first") == nullptr);
    assert(invalid_store.Unload() == 0);

    assert(std::filesystem::remove(db_path));
    std::filesystem::remove(db_path.string() + "-wal");
    std::filesystem::remove(db_path.string() + "-shm");
    assert(std::filesystem::remove(db_path.parent_path()));
    assert(std::filesystem::remove(directory_db_path));
    assert(std::filesystem::remove(first_path));
    assert(std::filesystem::remove(second_path));
    assert(std::filesystem::remove(root));
}

void TestPcapFilePluginStrictStartupRecovery() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path root =
        std::filesystem::path("/tmp") / ("flowsql_pcapfile_strict_recovery_" + std::to_string(unique));
    const std::filesystem::path db_path = root / "pcapfile.sqlite";
    const std::filesystem::path valid_path = root / "valid.pcap";
    const std::filesystem::path missing_path = root / "missing.pcap";
    assert(std::filesystem::create_directories(root));
    WriteFile(valid_path.string(), MakeClassicPcap(true, false, 0));

    const auto option_for = [](const std::filesystem::path& path) {
        return "{\"path\":\"" + path.string() +
               "\",\"format\":\"pcap\",\"batch_packets\":1,\"replay_mode\":\"fast\","
               "\"replay_speed_milli\":1000}";
    };
    pcapfile::PcapFileChannelStore seed;
    std::string error;
    assert(seed.Open(db_path.string(), &error) == 0);
    assert(seed.Insert({"pcapfile", "a-valid", option_for(valid_path)}, &error) == 0);
    assert(seed.Insert({"pcapfile", "z-missing", option_for(missing_path)}, &error) == 0);
    seed.Close();

    MockProtocol protocol;
    DeferredProtocolQuerier querier;
    querier.SetProtocol(&protocol);
    pcapfile::PcapFilePlugin plugin;
    assert(plugin.Option(("db_path=" + db_path.string()).c_str()) == 0);
    assert(plugin.Load(&querier) == 0);
    assert(plugin.Start() != 0);
    assert(plugin.Get("pcapfile", "a-valid") == nullptr);
    int visible = 0;
    plugin.List([&](const char*, const char*, flowsql::IBlockStreamChannel*) { ++visible; });
    assert(visible == 0);

    WriteFile(missing_path.string(), MakeClassicPcap(true, false, 0));
    assert(plugin.Start() == 0);
    assert(plugin.Get("pcapfile", "a-valid") != nullptr);
    assert(plugin.Get("pcapfile", "z-missing") != nullptr);
    assert(plugin.Unload() == 0);

    assert(seed.Open(db_path.string(), &error) == 0);
    assert(seed.Update({"pcapfile", "z-missing", "{}"}, &error) == 0);
    seed.Close();
    pcapfile::PcapFilePlugin malformed;
    assert(malformed.Option(("db_path=" + db_path.string()).c_str()) == 0);
    assert(malformed.Load(&querier) == 0);
    assert(malformed.Start() != 0);
    assert(malformed.Get("pcapfile", "a-valid") == nullptr);
    assert(malformed.Unload() == 0);

    assert(seed.Open(db_path.string(), &error) == 0);
    assert(seed.Erase("pcapfile", "z-missing", &error) == 0);
    assert(seed.Insert({"pcapfile", "", option_for(valid_path)}, &error) == 0);
    seed.Close();
    pcapfile::PcapFilePlugin unnamed;
    assert(unnamed.Option(("db_path=" + db_path.string()).c_str()) == 0);
    assert(unnamed.Load(&querier) == 0);
    assert(unnamed.Start() == EINVAL);
    assert(unnamed.Get("pcapfile", "a-valid") == nullptr);
    assert(unnamed.Unload() == 0);

    assert(std::filesystem::remove(db_path));
    std::filesystem::remove(db_path.string() + "-wal");
    std::filesystem::remove(db_path.string() + "-shm");
    assert(std::filesystem::remove(valid_path));
    assert(std::filesystem::remove(missing_path));
    assert(std::filesystem::remove(root));
}

std::vector<pcapfile::PcapFileChannelRecord> LoadPcapFileChannelRecords(
    const std::filesystem::path& db_path) {
    pcapfile::PcapFileChannelStore store;
    std::string error;
    assert(store.Open(db_path.string(), &error) == 0);
    std::vector<pcapfile::PcapFileChannelRecord> records;
    assert(store.LoadAll(&records, &error) == 0);
    store.Close();
    return records;
}

void ExecutePcapFileChannelStoreSql(const std::filesystem::path& db_path,
                                    const char* sql) {
    sqlite3* db = nullptr;
    assert(sqlite3_open_v2(db_path.string().c_str(), &db,
                           SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX,
                           nullptr) == SQLITE_OK);
    char* error = nullptr;
    assert(sqlite3_exec(db, sql, nullptr, nullptr, &error) == SQLITE_OK);
    assert(error == nullptr);
    assert(sqlite3_close(db) == SQLITE_OK);
}

void RemovePcapFilePersistenceFixture(const std::filesystem::path& root,
                                      const std::filesystem::path& db_path,
                                      const std::vector<std::filesystem::path>& captures) {
    assert(std::filesystem::remove(db_path));
    std::filesystem::remove(db_path.string() + "-wal");
    std::filesystem::remove(db_path.string() + "-shm");
    for (const auto& capture : captures) assert(std::filesystem::remove(capture));
    assert(std::filesystem::remove(root));
}

void TestPcapFilePluginPersistentAddModify() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path root =
        std::filesystem::path("/tmp") / ("flowsql_pcapfile_add_modify_" + std::to_string(unique));
    const std::filesystem::path db_path = root / "pcapfile.sqlite";
    const std::filesystem::path first_path = root / "first.pcap";
    const std::filesystem::path second_path = root / "second.pcap";
    assert(std::filesystem::create_directories(root));
    WriteFile(first_path.string(), MakeClassicPcap(true, false, 0));
    WriteFile(second_path.string(), MakeClassicPcap(true, false, 0));

    MockProtocol protocol;
    DeferredProtocolQuerier querier;
    querier.SetProtocol(&protocol);
    pcapfile::PcapFilePlugin plugin;
    assert(plugin.Option(("db_path=" + db_path.string()).c_str()) == 0);
    assert(plugin.Load(&querier) == 0);
    assert(plugin.Start() == 0);

    const std::string add_option =
        "{\"batch_packets\":3,\"format\":\"PCAP\",\"path\":\"" +
        first_path.string() + "\"}";
    const std::string normalized_add =
        "{\"path\":\"" + first_path.string() +
        "\",\"format\":\"pcap\",\"batch_packets\":3,\"replay_mode\":\"fast\","
        "\"replay_speed_milli\":1000}";
    assert(plugin.AddChannel("PCAPFILE", "managed", add_option) == 0);
    auto* before_modify = plugin.Get("pcapfile", "managed");
    assert(before_modify != nullptr && before_modify->IsOpened());
    auto records = LoadPcapFileChannelRecords(db_path);
    assert(records.size() == 1);
    assert(records[0].type == "pcapfile");
    assert(records[0].name == "managed");
    assert(records[0].option == normalized_add);

    const std::string modify_option =
        "{\"path\":\"" + second_path.string() +
        "\",\"format\":\"PCAP\",\"batch_packets\":1,\"replay_mode\":\"TIMESTAMP\","
        "\"replay_speed_milli\":2000}";
    const std::string normalized_modify =
        "{\"path\":\"" + second_path.string() +
        "\",\"format\":\"pcap\",\"batch_packets\":1,\"replay_mode\":\"timestamp\","
        "\"replay_speed_milli\":2000}";
    assert(plugin.ModifyChannel("pcapfile", "managed", modify_option) == 0);
    auto* after_modify = plugin.Get("pcapfile", "managed");
    assert(after_modify != nullptr && after_modify != before_modify);
    records = LoadPcapFileChannelRecords(db_path);
    assert(records.size() == 1 && records[0].option == normalized_modify);
    std::string visible_option;
    plugin.QueryChannels([&](const std::string&, const std::string& name,
                             const std::string& option, const std::string&) {
        if (name == "managed") visible_option = option;
    });
    assert(visible_option == normalized_modify);
    const auto modified_batch = after_modify->PollBlock(0);
    assert(modified_batch.kind == flowsql::BlockPollEvent::kData && modified_batch.batch);
    assert(after_modify->ReleaseBlock(modified_batch.batch) == 0);
    assert(plugin.Unload() == 0);

    pcapfile::PcapFilePlugin restarted;
    assert(restarted.Option(("db_path=" + db_path.string()).c_str()) == 0);
    assert(restarted.Load(&querier) == 0);
    assert(restarted.Start() == 0);
    auto* recovered = restarted.Get("pcapfile", "managed");
    assert(recovered != nullptr && recovered->IsOpened());
    const auto recovered_batch = recovered->PollBlock(0);
    assert(recovered_batch.kind == flowsql::BlockPollEvent::kData && recovered_batch.batch);
    assert(recovered->ReleaseBlock(recovered_batch.batch) == 0);
    assert(restarted.Unload() == 0);

    RemovePcapFilePersistenceFixture(root, db_path, {first_path, second_path});
}

void TestPcapFilePluginPersistentAddFailureDoesNotPublish() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path root =
        std::filesystem::path("/tmp") / ("flowsql_pcapfile_add_failure_" + std::to_string(unique));
    const std::filesystem::path db_path = root / "pcapfile.sqlite";
    const std::filesystem::path capture_path = root / "capture.pcap";
    assert(std::filesystem::create_directories(root));
    WriteFile(capture_path.string(), MakeClassicPcap(true, false, 0));

    MockProtocol protocol;
    DeferredProtocolQuerier querier;
    querier.SetProtocol(&protocol);
    pcapfile::PcapFilePlugin plugin;
    assert(plugin.Option(("db_path=" + db_path.string()).c_str()) == 0);
    assert(plugin.Load(&querier) == 0);
    assert(plugin.Start() == 0);
    ExecutePcapFileChannelStoreSql(
        db_path,
        "CREATE TRIGGER fail_pcapfile_insert BEFORE INSERT ON pcapfile_channel_store "
        "BEGIN SELECT RAISE(ABORT, 'forced insert failure'); END;");

    const std::string option = "{\"path\":\"" + capture_path.string() + "\"}";
    assert(plugin.AddChannel("pcapfile", "rejected", option) != 0);
    assert(plugin.Get("pcapfile", "rejected") == nullptr);
    int visible = 0;
    plugin.List([&](const char*, const char*, flowsql::IBlockStreamChannel*) { ++visible; });
    assert(visible == 0);
    assert(LoadPcapFileChannelRecords(db_path).empty());
    assert(plugin.Unload() == 0);

    RemovePcapFilePersistenceFixture(root, db_path, {capture_path});
}

void TestPcapFilePluginPersistentModifyFailureKeepsOldChannel() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path root =
        std::filesystem::path("/tmp") / ("flowsql_pcapfile_modify_failure_" + std::to_string(unique));
    const std::filesystem::path db_path = root / "pcapfile.sqlite";
    const std::filesystem::path old_path = root / "old.pcap";
    const std::filesystem::path next_path = root / "next.pcap";
    assert(std::filesystem::create_directories(root));
    WriteFile(old_path.string(), MakeClassicPcap(true, false, 0));
    WriteFile(next_path.string(), MakeClassicPcap(true, false, 0));

    MockProtocol protocol;
    DeferredProtocolQuerier querier;
    querier.SetProtocol(&protocol);
    pcapfile::PcapFilePlugin plugin;
    assert(plugin.Option(("db_path=" + db_path.string()).c_str()) == 0);
    assert(plugin.Load(&querier) == 0);
    assert(plugin.Start() == 0);
    const std::string old_option = "{\"path\":\"" + old_path.string() + "\",\"batch_packets\":1}";
    assert(plugin.AddChannel("pcapfile", "stable", old_option) == 0);
    auto* old_channel = plugin.Get("pcapfile", "stable");
    assert(old_channel != nullptr && old_channel->IsOpened());
    std::string normalized_old;
    plugin.QueryChannels([&](const std::string&, const std::string& name,
                             const std::string& option, const std::string&) {
        if (name == "stable") normalized_old = option;
    });
    ExecutePcapFileChannelStoreSql(
        db_path,
        "CREATE TRIGGER fail_pcapfile_update BEFORE UPDATE ON pcapfile_channel_store "
        "BEGIN SELECT RAISE(ABORT, 'forced update failure'); END;");

    const std::string next_option = "{\"path\":\"" + next_path.string() + "\",\"batch_packets\":2}";
    assert(plugin.ModifyChannel("pcapfile", "stable", next_option) != 0);
    assert(plugin.Get("pcapfile", "stable") == old_channel);
    assert(old_channel->IsOpened());
    std::string visible_option;
    plugin.QueryChannels([&](const std::string&, const std::string& name,
                             const std::string& option, const std::string&) {
        if (name == "stable") visible_option = option;
    });
    assert(visible_option == normalized_old);
    const auto records = LoadPcapFileChannelRecords(db_path);
    assert(records.size() == 1);
    assert(records[0].name == "stable");
    assert(records[0].option == normalized_old);
    const auto old_batch = old_channel->PollBlock(0);
    assert(old_batch.kind == flowsql::BlockPollEvent::kData && old_batch.batch);
    assert(old_channel->ReleaseBlock(old_batch.batch) == 0);
    assert(plugin.Unload() == 0);

    RemovePcapFilePersistenceFixture(root, db_path, {old_path, next_path});
}

void TestPcapFilePluginPersistentRemoveAndReaderLifecycle() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path root =
        std::filesystem::path("/tmp") / ("flowsql_pcapfile_remove_" + std::to_string(unique));
    const std::filesystem::path db_path = root / "pcapfile.sqlite";
    const std::filesystem::path capture_path = root / "capture.pcap";
    assert(std::filesystem::create_directories(root));
    WriteFile(capture_path.string(), MakeClassicPcap(true, false, 0));

    MockProtocol protocol;
    DeferredProtocolQuerier querier;
    querier.SetProtocol(&protocol);
    pcapfile::PcapFilePlugin plugin;
    assert(plugin.Option(("db_path=" + db_path.string()).c_str()) == 0);
    assert(plugin.Load(&querier) == 0);
    assert(plugin.Start() == 0);
    const std::string option =
        "{\"path\":\"" + capture_path.string() + "\",\"batch_packets\":1}";
    assert(plugin.AddChannel("pcapfile", "removable", option) == 0);
    auto* base = plugin.Get("pcapfile", "removable");
    assert(base != nullptr && base->IsOpened());
    assert(LoadPcapFileChannelRecords(db_path).size() == 1);

    const auto base_batch = base->PollBlock(0);
    assert(base_batch.kind == flowsql::BlockPollEvent::kData && base_batch.batch);
    assert(plugin.RemoveChannel("pcapfile", "removable") == EBUSY);
    assert(plugin.Get("pcapfile", "removable") == base);
    assert(LoadPcapFileChannelRecords(db_path).size() == 1);
    assert(base->ReleaseBlock(base_batch.batch) == 0);

    flowsql::BlockStreamReaderConfigV1 reader_config;
    reader_config.task_id = "persistent-remove-reader";
    reader_config.source_category = "pcapfile";
    reader_config.source_name = "removable";
    reader_config.pushed_filter_plan_json = flowsql::kEmptyCanonicalFilterPlanV1;
    flowsql::IBlockStreamChannel* reader = nullptr;
    assert(plugin.CreateReader(reader_config, &reader) == 0);
    assert(reader != nullptr);
    assert(plugin.RemoveChannel("pcapfile", "removable") == EBUSY);
    assert(plugin.Get("pcapfile", "removable") == base);
    assert(LoadPcapFileChannelRecords(db_path).size() == 1);
    const auto reader_batch = reader->PollBlock(0);
    assert(reader_batch.kind == flowsql::BlockPollEvent::kData && reader_batch.batch);
    assert(reader->ReleaseBlock(reader_batch.batch) == 0);
    plugin.ReleaseReader(reader);

    assert(plugin.Get("pcapfile", "removable") == base);
    assert(LoadPcapFileChannelRecords(db_path).size() == 1);
    flowsql::IBlockStreamChannel* second_reader = nullptr;
    reader_config.task_id = "persistent-remove-reader-two";
    assert(plugin.CreateReader(reader_config, &second_reader) == 0);
    assert(second_reader != nullptr);
    const auto second_reader_batch = second_reader->PollBlock(0);
    assert(second_reader_batch.kind == flowsql::BlockPollEvent::kData &&
           second_reader_batch.batch);
    assert(second_reader->ReleaseBlock(second_reader_batch.batch) == 0);
    plugin.ReleaseReader(second_reader);
    assert(LoadPcapFileChannelRecords(db_path).size() == 1);

    assert(plugin.RemoveChannel("PCAPFILE", "removable") == 0);
    assert(plugin.Get("pcapfile", "removable") == nullptr);
    assert(LoadPcapFileChannelRecords(db_path).empty());
    assert(std::filesystem::is_regular_file(capture_path));
    assert(plugin.Unload() == 0);

    pcapfile::PcapFilePlugin restarted;
    assert(restarted.Option(("db_path=" + db_path.string()).c_str()) == 0);
    assert(restarted.Load(&querier) == 0);
    assert(restarted.Start() == 0);
    assert(restarted.Get("pcapfile", "removable") == nullptr);
    int visible = 0;
    restarted.List([&](const char*, const char*, flowsql::IBlockStreamChannel*) { ++visible; });
    assert(visible == 0);
    assert(restarted.Unload() == 0);

    RemovePcapFilePersistenceFixture(root, db_path, {capture_path});
}

void TestPcapFilePluginPersistentRemoveFailureKeepsOldChannel() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path root =
        std::filesystem::path("/tmp") / ("flowsql_pcapfile_remove_failure_" + std::to_string(unique));
    const std::filesystem::path db_path = root / "pcapfile.sqlite";
    const std::filesystem::path capture_path = root / "capture.pcap";
    assert(std::filesystem::create_directories(root));
    WriteFile(capture_path.string(), MakeClassicPcap(true, false, 0));

    MockProtocol protocol;
    DeferredProtocolQuerier querier;
    querier.SetProtocol(&protocol);
    pcapfile::PcapFilePlugin plugin;
    assert(plugin.Option(("db_path=" + db_path.string()).c_str()) == 0);
    assert(plugin.Load(&querier) == 0);
    assert(plugin.Start() == 0);
    const std::string option = "{\"path\":\"" + capture_path.string() + "\"}";
    assert(plugin.AddChannel("pcapfile", "stable", option) == 0);
    auto* old_channel = plugin.Get("pcapfile", "stable");
    assert(old_channel != nullptr && old_channel->IsOpened());
    const auto before = LoadPcapFileChannelRecords(db_path);
    assert(before.size() == 1);
    ExecutePcapFileChannelStoreSql(
        db_path,
        "CREATE TRIGGER fail_pcapfile_delete BEFORE DELETE ON pcapfile_channel_store "
        "BEGIN SELECT RAISE(ABORT, 'forced delete failure'); END;");

    assert(plugin.RemoveChannel("pcapfile", "stable") != 0);
    assert(plugin.Get("pcapfile", "stable") == old_channel);
    assert(old_channel->IsOpened());
    const auto after = LoadPcapFileChannelRecords(db_path);
    assert(after.size() == 1);
    assert(after[0].type == before[0].type);
    assert(after[0].name == before[0].name);
    assert(after[0].option == before[0].option);
    const auto event = old_channel->PollBlock(0);
    assert(event.kind == flowsql::BlockPollEvent::kData && event.batch);
    assert(old_channel->ReleaseBlock(event.batch) == 0);
    assert(std::filesystem::is_regular_file(capture_path));
    assert(plugin.Unload() == 0);

    RemovePcapFilePersistenceFixture(root, db_path, {capture_path});
}

}  // namespace

int main() {
    TestRfc3339TimestampNs();
    TestPacketDomainKeyCompilation();
    TestCanonicalPacketPlanCompilerAndPredicates();
    TestPcapFilterCandidateSelection();
    TestPcapFilterPushdownCapability();
    TestClassicPcapTwoStageFilterDataPath();
    TestPcapngHeaderFilterDataPath();
    TestFilteredEmptyAndSkippedErrorTerminals();
    TestFilteredTimestampReplay();
    TestPcapFilterDomainResolverResidual();
    TestClassicPcap();
    TestClassicMagicAndEndian();
    TestClassicFieldsAndFileOrder();
    TestClassicStructureErrors();
    TestPendingErrorAfterData();
    TestSupportedLayerDecodeContract();
    TestUnsupportedTruncatedAndMalformedLayer();
    TestEmptyFileEof();
    TestPcapngSectionAndResolution();
    TestPcapngSectionsAndRounding();
    TestPcapngInterfaceFieldsAndFileOrder();
    TestPcapngTimestampQuantizationAndOverflow();
    TestPcapngEnhancedPacketMapping();
    TestPcapngEnhancedPacketErrors();
    TestPcapngSectionValidation();
    TestPcapngInterfaceOptionErrors();
    TestManyPcapngSectionsIterative();
    TestReplayModes();
    TestBackpressureAndCancelWakeup();
    TestReleaseBlockOwnership();
    TestCancelInterruptsReplayWait();
    TestBatchOwnerAndPluginLifecycle();
    TestMalformedPcapngAndFormatMismatch();
    TestIncrementalReadAfterOpen();
    TestOptions();
    TestPluginDependencyLifecycle();
    TestExclusiveReaderFactory();
    TestDynamicPluginDependencyOrders();
    TestPluginManager();
    TestCancelAndManagerBusy();
    TestPcapFileChannelStore();
    TestPcapFilePluginPersistentStartupRecovery();
    TestPcapFilePluginStrictStartupRecovery();
    TestPcapFilePluginPersistentAddModify();
    TestPcapFilePluginPersistentAddFailureDoesNotPublish();
    TestPcapFilePluginPersistentModifyFailureKeepsOldChannel();
    TestPcapFilePluginPersistentRemoveAndReaderLifecycle();
    TestPcapFilePluginPersistentRemoveFailureKeepsOldChannel();
    std::puts("[PASS] pcapfile import");
    return 0;
}
