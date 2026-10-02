// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef FLOWSQL_NPM_ICMP_PARSER_H_
#define FLOWSQL_NPM_ICMP_PARSER_H_

#include "npm_icmp_contract.h"

#include <cstdint>
#include <optional>
#include <string>

namespace flowsql::npm {

enum class NpmIcmpMessageKindV1 : uint8_t { kIgnored = 0, kEchoRequest, kEchoReply, kError };
enum class NpmIcmpParseErrorV1 : uint8_t { kNone = 0, kInvalidInput };

struct NpmIcmpParsedV1 {
    NpmIcmpMessageKindV1 kind = NpmIcmpMessageKindV1::kIgnored;
    uint8_t ip_family = 0;
    packet::IpAddress src_ip;
    packet::IpAddress dst_ip;
    uint8_t icmp_type = 0;
    uint8_t icmp_code = 0;
    bool outer_truncated = false;
    uint16_t echo_id = 0;
    uint16_t echo_sequence = 0;
    std::optional<uint32_t> next_hop_mtu;
    std::string quote_status;
    std::optional<uint8_t> quoted_ip_family;
    std::optional<uint8_t> quoted_protocol;
    std::optional<packet::IpAddress> quoted_src_ip;
    std::optional<packet::IpAddress> quoted_dst_ip;
    std::optional<NpmIcmpQuotedFlowV1> quoted_flow;
};

/** Parses only captured bytes; ignored control messages and incomplete headers are not task errors. */
NpmIcmpParseErrorV1 ParseNpmIcmpControlV1(const NpmInputEventV1& input, NpmIcmpParsedV1* output);

}  // namespace flowsql::npm

#endif  // FLOWSQL_NPM_ICMP_PARSER_H_
