// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_icmp_parser.h"

#include <common/network/netbase.h>

#include <utility>

namespace flowsql::npm {
namespace {

bool FragmentedOuter(const NpmInputEventV1& input, uint8_t family) {
    const auto& layer = *input.layer;
    const auto bytes = input.packet.bytes;
    const size_t network = layer.layers[layer.network_layer_index].offset;
    if (family == 4) {
        const auto* header = wire::View<Ipv4Header>(bytes.data, bytes.size, network);
        if (!header) return true;
        return (ntohs(header->fragment_offset) & 0x3fffu) != 0;
    }
    for (uint8_t i = static_cast<uint8_t>(layer.network_layer_index + 1); i < layer.layer_count; ++i) {
        if (layer.layers[i].kind != static_cast<uint16_t>(eLayer::IPv6_EXT_FRAGMENT)) continue;
        const size_t offset = layer.layers[i].offset;
        const auto* fragment = wire::View<Ipv6FragmentHeader>(bytes.data, bytes.size, offset);
        if (!fragment) return true;
        if ((ntohs(fragment->fragment) & 0xfff9u) != 0) return true;
    }
    return false;
}

template <typename Address>
void ParseQuote(const NpmInputEventV1& input, uint8_t outer_family, NpmIcmpFamilyParsed<Address>* parsed) {
    const auto* quote = input.body.data + sizeof(IcmpHeader);
    const size_t size = input.body.size - sizeof(IcmpHeader);
    parsed->quote_status = "unavailable";
    if (size == 0) return;

    const auto* ip = reinterpret_cast<const IpVersionHeader*>(quote);
    const uint8_t version = ip->version;
    if (version != outer_family) {
        parsed->quote_status = "invalid";
        return;
    }
    size_t header_size = 0;
    size_t declared_size = 0;
    uint8_t protocol = 0;
    Address src, dst;
    bool first_fragment = true;
    if constexpr (sizeof(Address) == 4) {
        if (size < sizeof(Ipv4Header)) return;
        const auto* header = reinterpret_cast<const Ipv4Header*>(quote);
        header_size = static_cast<size_t>(header->ihl) * 4;
        declared_size = ntohs(header->total_length);
        if (header_size < 20 || declared_size < header_size) {
            parsed->quote_status = "invalid";
            return;
        }
        if (header_size > size) return;
        first_fragment = (ntohs(header->fragment_offset) & 0x1fffu) == 0;
        protocol = header->protocol;
        std::memcpy(src.bytes, header->src_addr.bytes, sizeof(Address));
        std::memcpy(dst.bytes, header->dst_addr.bytes, sizeof(Address));
    } else {
        if (size < sizeof(Ipv6Header)) return;
        const auto* header = reinterpret_cast<const Ipv6Header*>(quote);
        header_size = sizeof(Ipv6Header);
        declared_size = sizeof(Ipv6Header) + ntohs(header->payload);
        protocol = header->protocol;
        std::memcpy(src.bytes, header->src_addr.bytes, sizeof(Address));
        std::memcpy(dst.bytes, header->dst_addr.bytes, sizeof(Address));
    }
    parsed->quoted_ip_family = version;
    parsed->quoted_protocol = protocol;
    parsed->quoted_src_ip = src;
    parsed->quoted_dst_ip = dst;
    parsed->quote_status = "ip_only";
    if (!first_fragment || (protocol != 6 && protocol != 17) || declared_size < header_size + sizeof(PortPairHeader) ||
        size < header_size + sizeof(PortPairHeader)) {
        return;
    }
    NpmIcmpFamilyQuotedFlow<Address> flow;
    flow.observation_domain_id = input.observation_domain_id;
    flow.protocol = protocol;
    flow.src_ip = std::move(src);
    flow.dst_ip = std::move(dst);
    const auto* ports = reinterpret_cast<const PortPairHeader*>(quote + header_size);
    flow.src_port = ntohs(ports->src_port);
    flow.dst_port = ntohs(ports->dst_port);
    parsed->quoted_flow = std::move(flow);
    parsed->quote_status = "tuple";
}

}  // namespace

template <typename Address>
NpmIcmpParseErrorV1 ParseNpmIcmpControlV1(const NpmInputEventV1& input, NpmIcmpFamilyParsed<Address>* output) {
    if (!output || ValidateNpmInputEventV1(input).error != NpmProtocolContractErrorV1::kNone ||
        input.kind != NpmInputKindV1::kControlPacket || input.layer->network_layer_index >= input.layer->layer_count) {
        return NpmIcmpParseErrorV1::kInvalidInput;
    }
    NpmIcmpFamilyParsed<Address> parsed;
    constexpr uint8_t family = sizeof(Address) == 4 ? 4 : 6;
    if (input.layer->transport_protocol != (family == 4 ? 1 : 58) ||
        input.layer->address_state != packet::PacketAddressState::kAvailable)
        return NpmIcmpParseErrorV1::kInvalidInput;
    if constexpr (sizeof(Address) == 4) {
        if (!input.layer->Get4()) return NpmIcmpParseErrorV1::kInvalidInput;
        parsed.src_ip = input.layer->Get4()->src;
        parsed.dst_ip = input.layer->Get4()->dst;
    } else {
        if (!input.layer->Get6()) return NpmIcmpParseErrorV1::kInvalidInput;
        parsed.src_ip = input.layer->Get6()->src;
        parsed.dst_ip = input.layer->Get6()->dst;
    }
    if (FragmentedOuter(input, family) || input.body.size < sizeof(IcmpHeader)) {
        *output = std::move(parsed);
        return NpmIcmpParseErrorV1::kNone;
    }
    const auto* header = reinterpret_cast<const IcmpHeader*>(input.body.data);
    parsed.icmp_type = header->type;
    parsed.icmp_code = header->code;
    parsed.outer_truncated = !input.body_complete;
    const bool request = (family == 4 && header->type == 8) || (family == 6 && header->type == 128);
    const bool reply = (family == 4 && header->type == 0) || (family == 6 && header->type == 129);
    if ((request || reply) && header->code == 0) {
        parsed.kind = request ? NpmIcmpMessageKindV1::kEchoRequest : NpmIcmpMessageKindV1::kEchoReply;
        parsed.echo_id = ntohs(header->body.echo.id);
        parsed.echo_sequence = ntohs(header->body.echo.sequence);
    } else {
        const bool error = family == 4 ? (header->type == 3 || header->type == 11 || header->type == 12)
                                       : (header->type >= 1 && header->type <= 4);
        if (error) {
            parsed.kind = NpmIcmpMessageKindV1::kError;
            if (family == 4 && header->type == 3 && header->code == 4) {
                parsed.next_hop_mtu = ntohs(header->body.unreachable.mtu);
            } else if (family == 6 && header->type == 2) {
                parsed.next_hop_mtu = ntohl(header->body.mtu);
            }
            ParseQuote(input, family, &parsed);
        }
    }
    *output = std::move(parsed);
    return NpmIcmpParseErrorV1::kNone;
}

template NpmIcmpParseErrorV1 ParseNpmIcmpControlV1(const NpmInputEventV1&, NpmIcmpParsed4*);
template NpmIcmpParseErrorV1 ParseNpmIcmpControlV1(const NpmInputEventV1&, NpmIcmpParsed6*);

}  // namespace flowsql::npm
