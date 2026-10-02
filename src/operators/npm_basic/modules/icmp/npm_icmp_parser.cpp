// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_icmp_parser.h"

#include <common/network/netbase.h>

#include <cstring>
#include <utility>

namespace flowsql::npm {
namespace {

uint16_t Read16(const uint8_t* bytes) {
    return static_cast<uint16_t>((static_cast<uint16_t>(bytes[0]) << 8) | bytes[1]);
}

uint32_t Read32(const uint8_t* bytes) {
    return (static_cast<uint32_t>(bytes[0]) << 24) | (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) | bytes[3];
}

packet::IpAddress ReadIpv4(const uint8_t* bytes) {
    packet::IPv4Address address;
    std::memcpy(address.bytes, bytes, 4);
    return address;
}

packet::IpAddress ReadIpv6(const uint8_t* bytes) {
    packet::IPv6Address address;
    std::memcpy(address.bytes, bytes, 16);
    return address;
}

bool FragmentedOuter(const NpmInputEventV1& input, uint8_t family) {
    const auto& layer = *input.layer;
    const auto bytes = input.packet.bytes;
    const size_t network = layer.layers[layer.network_layer_index].offset;
    if (family == 4) {
        if (network > bytes.size || bytes.size - network < 20) return true;
        return (Read16(bytes.data + network + 6) & 0x3fffu) != 0;
    }
    for (uint8_t i = static_cast<uint8_t>(layer.network_layer_index + 1); i < layer.layer_count; ++i) {
        if (layer.layers[i].kind != static_cast<uint16_t>(eLayer::IPv6_EXT_FRAGMENT)) continue;
        const size_t offset = layer.layers[i].offset;
        if (offset > bytes.size || bytes.size - offset < 8) return true;
        if ((Read16(bytes.data + offset + 2) & 0xfff9u) != 0) return true;
    }
    return false;
}

void ParseQuote(const NpmInputEventV1& input, uint8_t outer_family, NpmIcmpParsedV1* parsed) {
    const auto* quote = input.body.data + 8;
    const size_t size = input.body.size - 8;
    parsed->quote_status = "unavailable";
    if (size == 0) return;

    const uint8_t version = quote[0] >> 4;
    if (version != outer_family) {
        parsed->quote_status = "invalid";
        return;
    }
    size_t header_size = 0;
    size_t declared_size = 0;
    uint8_t protocol = 0;
    packet::IpAddress src, dst;
    bool first_fragment = true;
    if (version == 4) {
        if (size < 20) return;
        header_size = static_cast<size_t>(quote[0] & 0x0f) * 4;
        declared_size = Read16(quote + 2);
        if (header_size < 20 || declared_size < header_size) {
            parsed->quote_status = "invalid";
            return;
        }
        if (header_size > size) return;
        first_fragment = (Read16(quote + 6) & 0x1fffu) == 0;
        protocol = quote[9];
        src = ReadIpv4(quote + 12);
        dst = ReadIpv4(quote + 16);
    } else {
        if (size < 40) return;
        header_size = 40;
        declared_size = 40 + Read16(quote + 4);
        protocol = quote[6];
        src = ReadIpv6(quote + 8);
        dst = ReadIpv6(quote + 24);
    }
    parsed->quoted_ip_family = version;
    parsed->quoted_protocol = protocol;
    parsed->quoted_src_ip = src;
    parsed->quoted_dst_ip = dst;
    parsed->quote_status = "ip_only";
    if (!first_fragment || (protocol != 6 && protocol != 17) || declared_size < header_size + 4 ||
        size < header_size + 4) {
        return;
    }
    NpmIcmpQuotedFlowV1 flow;
    flow.observation_domain_id = input.observation_domain_id;
    flow.ip_family = version == 4 ? packet::AddressFamily::kIPv4 : packet::AddressFamily::kIPv6;
    flow.protocol = protocol;
    flow.src_ip = std::move(src);
    flow.dst_ip = std::move(dst);
    flow.src_port = Read16(quote + header_size);
    flow.dst_port = Read16(quote + header_size + 2);
    parsed->quoted_flow = std::move(flow);
    parsed->quote_status = "tuple";
}

}  // namespace

NpmIcmpParseErrorV1 ParseNpmIcmpControlV1(const NpmInputEventV1& input, NpmIcmpParsedV1* output) {
    if (!output || ValidateNpmInputEventV1(input).error != NpmProtocolContractErrorV1::kNone ||
        input.kind != NpmInputKindV1::kControlPacket || input.layer->network_layer_index >= input.layer->layer_count) {
        return NpmIcmpParseErrorV1::kInvalidInput;
    }
    NpmIcmpParsedV1 parsed;
    const uint8_t protocol = input.layer->transport_protocol;
    parsed.ip_family = protocol == 1 ? 4 : 6;
    if ((parsed.ip_family == 4 && (!std::holds_alternative<packet::IPv4Address>(input.layer->src_ip) ||
                                   !std::holds_alternative<packet::IPv4Address>(input.layer->dst_ip))) ||
        (parsed.ip_family == 6 && (!std::holds_alternative<packet::IPv6Address>(input.layer->src_ip) ||
                                   !std::holds_alternative<packet::IPv6Address>(input.layer->dst_ip)))) {
        return NpmIcmpParseErrorV1::kInvalidInput;
    }
    parsed.src_ip = input.layer->src_ip;
    parsed.dst_ip = input.layer->dst_ip;
    if (FragmentedOuter(input, parsed.ip_family) || input.body.size < 8) {
        *output = std::move(parsed);
        return NpmIcmpParseErrorV1::kNone;
    }
    const auto* body = input.body.data;
    parsed.icmp_type = body[0];
    parsed.icmp_code = body[1];
    parsed.outer_truncated = !input.body_complete;
    const bool request = (parsed.ip_family == 4 && body[0] == 8) || (parsed.ip_family == 6 && body[0] == 128);
    const bool reply = (parsed.ip_family == 4 && body[0] == 0) || (parsed.ip_family == 6 && body[0] == 129);
    if ((request || reply) && body[1] == 0) {
        parsed.kind = request ? NpmIcmpMessageKindV1::kEchoRequest : NpmIcmpMessageKindV1::kEchoReply;
        parsed.echo_id = Read16(body + 4);
        parsed.echo_sequence = Read16(body + 6);
    } else {
        const bool error =
            parsed.ip_family == 4 ? (body[0] == 3 || body[0] == 11 || body[0] == 12) : (body[0] >= 1 && body[0] <= 4);
        if (error) {
            parsed.kind = NpmIcmpMessageKindV1::kError;
            if (parsed.ip_family == 4 && body[0] == 3 && body[1] == 4) {
                parsed.next_hop_mtu = Read16(body + 6);
            } else if (parsed.ip_family == 6 && body[0] == 2) {
                parsed.next_hop_mtu = Read32(body + 4);
            }
            ParseQuote(input, parsed.ip_family, &parsed);
        }
    }
    *output = std::move(parsed);
    return NpmIcmpParseErrorV1::kNone;
}

}  // namespace flowsql::npm
