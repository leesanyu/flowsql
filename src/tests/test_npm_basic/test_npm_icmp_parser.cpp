// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <operators/npm_basic/modules/icmp/npm_icmp_parser.h>

#include <common/network/netbase.h>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace npm = flowsql::npm;

namespace {

struct Fixture {
    std::vector<uint8_t> bytes;
    flowsql::packet::PacketLayerInfo layer;
    npm::NpmInputEventV1 event;

    Fixture(bool ipv6, std::vector<uint8_t> body, bool complete = true) {
        const size_t header = ipv6 ? 40 : 20;
        bytes.resize(header + body.size());
        bytes[0] = ipv6 ? 0x60 : 0x45;
        if (ipv6) {
            const auto payload = static_cast<uint16_t>(body.size());
            bytes[4] = static_cast<uint8_t>(payload >> 8);
            bytes[5] = static_cast<uint8_t>(payload);
            bytes[6] = 58;
        } else {
            const auto total = static_cast<uint16_t>(bytes.size());
            bytes[2] = static_cast<uint8_t>(total >> 8);
            bytes[3] = static_cast<uint8_t>(total);
            bytes[9] = 1;
        }
        std::copy(body.begin(), body.end(), bytes.begin() + header);
        layer.status = flowsql::packet::LayerStatus::kDecoded;
        layer.layer_count = 1;
        layer.layers[0] = {static_cast<uint16_t>(ipv6 ? flowsql::eLayer::IPv6 : flowsql::eLayer::IPv4), 0};
        layer.network_layer_index = 0;
        layer.transport_protocol = ipv6 ? 58 : 1;
        if (ipv6) {
            flowsql::packet::IPv6Address src, dst;
            src.bytes[15] = 1;
            dst.bytes[15] = 2;
            layer.src_ip = src;
            layer.dst_ip = dst;
        } else {
            flowsql::packet::IPv4Address src, dst;
            src.bytes[0] = dst.bytes[0] = 10;
            src.bytes[3] = 1;
            dst.bytes[3] = 2;
            layer.src_ip = src;
            layer.dst_ip = dst;
        }
        event.kind = npm::NpmInputKindV1::kControlPacket;
        event.observation_domain_id = 77;
        event.packet.bytes = {bytes.data(), bytes.size()};
        event.packet.meta.captured_len = static_cast<uint32_t>(bytes.size());
        event.layer = &layer;
        event.body = {bytes.data() + header, body.size()};
        event.body_complete = complete;
    }
};

std::vector<uint8_t> Quote4() {
    std::vector<uint8_t> quote(24);
    quote[0] = 0x45;
    quote[2] = 0;
    quote[3] = 40;
    quote[9] = 6;
    quote[12] = 192;
    quote[15] = 1;
    quote[16] = 192;
    quote[19] = 2;
    quote[20] = 0x12;
    quote[21] = 0x34;
    quote[22] = 0x00;
    quote[23] = 0x50;
    return quote;
}

std::vector<uint8_t> Quote6() {
    std::vector<uint8_t> quote(44);
    quote[0] = 0x60;
    quote[5] = 20;
    quote[6] = 17;
    quote[23] = 1;
    quote[39] = 2;
    quote[40] = 0x12;
    quote[41] = 0x34;
    quote[42] = 0;
    quote[43] = 53;
    return quote;
}

void TestEcho() {
    for (bool ipv6 : {false, true}) {
        Fixture request(ipv6, {static_cast<uint8_t>(ipv6 ? 128 : 8), 0, 0, 0, 0x12, 0x34, 0, 9});
        npm::NpmIcmpParsedV1 parsed;
        assert(npm::ParseNpmIcmpControlV1(request.event, &parsed) == npm::NpmIcmpParseErrorV1::kNone);
        assert(parsed.kind == npm::NpmIcmpMessageKindV1::kEchoRequest);
        assert(parsed.echo_id == 0x1234 && parsed.echo_sequence == 9 && parsed.ip_family == (ipv6 ? 6 : 4));
        request.bytes[ipv6 ? 40 : 20] = static_cast<uint8_t>(ipv6 ? 129 : 0);
        assert(npm::ParseNpmIcmpControlV1(request.event, &parsed) == npm::NpmIcmpParseErrorV1::kNone);
        assert(parsed.kind == npm::NpmIcmpMessageKindV1::kEchoReply);
        request.bytes[(ipv6 ? 40 : 20) + 1] = 1;
        assert(npm::ParseNpmIcmpControlV1(request.event, &parsed) == npm::NpmIcmpParseErrorV1::kNone);
        assert(parsed.kind == npm::NpmIcmpMessageKindV1::kIgnored);
    }
    Fixture truncated(false, {8, 0, 0, 0, 0, 1, 0, 2}, false);
    npm::NpmIcmpParsedV1 parsed;
    assert(npm::ParseNpmIcmpControlV1(truncated.event, &parsed) == npm::NpmIcmpParseErrorV1::kNone);
    assert(parsed.kind == npm::NpmIcmpMessageKindV1::kEchoRequest && parsed.outer_truncated);
    Fixture short_header(false, {8, 0, 0, 0, 0, 1, 0});
    assert(npm::ParseNpmIcmpControlV1(short_header.event, &parsed) == npm::NpmIcmpParseErrorV1::kNone);
    assert(parsed.kind == npm::NpmIcmpMessageKindV1::kIgnored);
    Fixture fragment(false, {8, 0, 0, 0, 0, 1, 0, 2});
    fragment.bytes[6] = 0x20;
    assert(npm::ParseNpmIcmpControlV1(fragment.event, &parsed) == npm::NpmIcmpParseErrorV1::kNone);
    assert(parsed.kind == npm::NpmIcmpMessageKindV1::kIgnored);
    Fixture ipv6_fragment(true, {128, 0, 0, 0, 0, 1, 0, 2});
    ipv6_fragment.bytes.insert(ipv6_fragment.bytes.begin() + 40, 8, 0);
    ipv6_fragment.bytes[6] = 44;
    ipv6_fragment.bytes[40] = 58;
    ipv6_fragment.bytes[43] = 1;
    ipv6_fragment.layer.layer_count = 2;
    ipv6_fragment.layer.layers[1] = {static_cast<uint16_t>(flowsql::eLayer::IPv6_EXT_FRAGMENT), 40};
    ipv6_fragment.event.packet.bytes = {ipv6_fragment.bytes.data(), ipv6_fragment.bytes.size()};
    ipv6_fragment.event.packet.meta.captured_len = static_cast<uint32_t>(ipv6_fragment.bytes.size());
    ipv6_fragment.event.body = {ipv6_fragment.bytes.data() + 48, 8};
    assert(npm::ParseNpmIcmpControlV1(ipv6_fragment.event, &parsed) == npm::NpmIcmpParseErrorV1::kNone);
    assert(parsed.kind == npm::NpmIcmpMessageKindV1::kIgnored);
}

void TestErrorsAndQuotes() {
    auto quote = Quote4();
    std::vector<uint8_t> body{3, 4, 0, 0, 0, 0, 0x05, 0xdc};
    body.insert(body.end(), quote.begin(), quote.end());
    Fixture error(false, body);
    npm::NpmIcmpParsedV1 parsed;
    assert(npm::ParseNpmIcmpControlV1(error.event, &parsed) == npm::NpmIcmpParseErrorV1::kNone);
    assert(parsed.kind == npm::NpmIcmpMessageKindV1::kError && parsed.next_hop_mtu == 1500);
    assert(parsed.quote_status == "tuple" && parsed.quoted_flow);
    assert(parsed.quoted_flow->observation_domain_id == 77 && parsed.quoted_flow->protocol == 6);
    assert(parsed.quoted_flow->src_port == 0x1234 && parsed.quoted_flow->dst_port == 80);
    error.bytes.resize(28);
    error.event.packet.bytes = {error.bytes.data(), error.bytes.size()};
    error.event.packet.meta.captured_len = static_cast<uint32_t>(error.bytes.size());
    error.event.body = {error.bytes.data() + 20, error.bytes.size() - 20};
    assert(npm::ParseNpmIcmpControlV1(error.event, &parsed) == npm::NpmIcmpParseErrorV1::kNone);
    assert(parsed.quote_status == "unavailable" && !parsed.quoted_flow);

    quote = Quote4();
    quote.resize(20);
    body.assign({11, 0, 0, 0, 0, 0, 0, 0});
    body.insert(body.end(), quote.begin(), quote.end());
    Fixture ip_only(false, body);
    assert(npm::ParseNpmIcmpControlV1(ip_only.event, &parsed) == npm::NpmIcmpParseErrorV1::kNone);
    assert(parsed.quote_status == "ip_only" && parsed.quoted_protocol == 6 && !parsed.quoted_flow);
    ip_only.bytes[28] = 0x46;
    assert(npm::ParseNpmIcmpControlV1(ip_only.event, &parsed) == npm::NpmIcmpParseErrorV1::kNone);
    assert(parsed.quote_status == "unavailable");
    ip_only.bytes[28] = 0x65;
    assert(npm::ParseNpmIcmpControlV1(ip_only.event, &parsed) == npm::NpmIcmpParseErrorV1::kNone);
    assert(parsed.quote_status == "invalid");

    auto quote6 = Quote6();
    body.assign({2, 0, 0, 0, 0, 0, 0x05, 0xdc});
    body.insert(body.end(), quote6.begin(), quote6.end());
    Fixture ipv6(true, body);
    assert(npm::ParseNpmIcmpControlV1(ipv6.event, &parsed) == npm::NpmIcmpParseErrorV1::kNone);
    assert(parsed.kind == npm::NpmIcmpMessageKindV1::kError && parsed.next_hop_mtu == 1500);
    assert(parsed.quote_status == "tuple" && parsed.quoted_flow->protocol == 17);
    assert(parsed.quoted_flow->src_port == 0x1234 && parsed.quoted_flow->dst_port == 53);
    ipv6.bytes[48 + 6] = 0;
    assert(npm::ParseNpmIcmpControlV1(ipv6.event, &parsed) == npm::NpmIcmpParseErrorV1::kNone);
    assert(parsed.quote_status == "ip_only" && !parsed.quoted_flow);
}

void TestErrorTypesAndQuoteBounds() {
    npm::NpmIcmpParsedV1 parsed;
    for (const uint8_t type : {uint8_t{3}, uint8_t{11}, uint8_t{12}}) {
        auto body = std::vector<uint8_t>{type, 7, 0, 0, 0, 0, 0, 0};
        const auto quote = Quote4();
        body.insert(body.end(), quote.begin(), quote.end());
        Fixture input(false, body);
        assert(npm::ParseNpmIcmpControlV1(input.event, &parsed) == npm::NpmIcmpParseErrorV1::kNone);
        assert(parsed.kind == npm::NpmIcmpMessageKindV1::kError && parsed.icmp_type == type && parsed.icmp_code == 7);
        assert(parsed.quote_status == "tuple" && parsed.quoted_flow && !parsed.next_hop_mtu);
    }
    for (const uint8_t type : {uint8_t{1}, uint8_t{3}, uint8_t{4}}) {
        auto body = std::vector<uint8_t>{type, 9, 0, 0, 0, 0, 0, 0};
        const auto quote = Quote6();
        body.insert(body.end(), quote.begin(), quote.end());
        Fixture input(true, body);
        assert(npm::ParseNpmIcmpControlV1(input.event, &parsed) == npm::NpmIcmpParseErrorV1::kNone);
        assert(parsed.kind == npm::NpmIcmpMessageKindV1::kError && parsed.icmp_type == type && parsed.icmp_code == 9);
        assert(parsed.quote_status == "tuple" && parsed.quoted_flow && !parsed.next_hop_mtu);
    }
    auto quote = Quote4();
    auto body = std::vector<uint8_t>{3, 0, 0, 0, 0, 0, 0, 0};
    body.insert(body.end(), quote.begin(), quote.end());
    Fixture bounded(false, body);
    bounded.bytes[28 + 3] = 19;
    assert(npm::ParseNpmIcmpControlV1(bounded.event, &parsed) == npm::NpmIcmpParseErrorV1::kNone);
    assert(parsed.quote_status == "invalid" && !parsed.quoted_flow);
    bounded.bytes[28 + 3] = 20;
    assert(npm::ParseNpmIcmpControlV1(bounded.event, &parsed) == npm::NpmIcmpParseErrorV1::kNone);
    assert(parsed.quote_status == "ip_only" && !parsed.quoted_flow);
    bounded.bytes[28 + 3] = 40;
    bounded.bytes[28 + 6] = 1;
    assert(npm::ParseNpmIcmpControlV1(bounded.event, &parsed) == npm::NpmIcmpParseErrorV1::kNone);
    assert(parsed.quote_status == "ip_only" && !parsed.quoted_flow);
    bounded.bytes[28 + 6] = 0;
    bounded.bytes[28 + 9] = 1;
    assert(npm::ParseNpmIcmpControlV1(bounded.event, &parsed) == npm::NpmIcmpParseErrorV1::kNone);
    assert(parsed.quote_status == "ip_only" && !parsed.quoted_flow);
}

}  // namespace

int main() {
    TestEcho();
    TestErrorsAndQuotes();
    TestErrorTypesAndQuoteBounds();
}
