// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef FLOWSQL_NPM_ICMP_CONTRACT_H_
#define FLOWSQL_NPM_ICMP_CONTRACT_H_

#include <operators/npm_basic/npm_protocol_contract.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace flowsql::npm {

constexpr int64_t kNpmIcmpDefaultEchoTimeoutNsV1 = 5'000'000'000;
constexpr int64_t kNpmIcmpMinEchoTimeoutNsV1 = 1'000'000;
constexpr int64_t kNpmIcmpMaxEchoTimeoutNsV1 = 300'000'000'000;
constexpr uint32_t kNpmIcmpDefaultMaxPendingEchoV1 = 4096;

struct NpmIcmpConfigV1 {
    int64_t echo_timeout_ns = kNpmIcmpDefaultEchoTimeoutNsV1;
    uint32_t max_pending_echo = kNpmIcmpDefaultMaxPendingEchoV1;
};

struct NpmIcmpEchoKeyV1 {
    uint64_t observation_domain_id = 0;
    packet::AddressFamily ip_family = packet::AddressFamily::kNone;
    packet::IpAddress src_ip;
    packet::IpAddress dst_ip;
    uint16_t identifier = 0;
    uint16_t sequence = 0;
};

struct NpmIcmpPendingEchoV1 {
    uint64_t entity_instance_id = 0;
    int64_t request_at_ns = 0;
    int64_t deadline_ns = 0;
    uint32_t retries = 0;
};

struct NpmIcmpQuotedFlowV1 {
    uint64_t observation_domain_id = 0;
    packet::AddressFamily ip_family = packet::AddressFamily::kNone;
    uint8_t protocol = 0;
    packet::IpAddress src_ip;
    packet::IpAddress dst_ip;
    uint16_t src_port = 0;
    uint16_t dst_port = 0;
};

/** Task-private synchronous lookup. Returned ID is only a current tuple candidate. */
interface INpmIcmpActiveSessionLookupV1 {
    virtual ~INpmIcmpActiveSessionLookupV1() = default;
    virtual std::optional<uint64_t> FindActive(const NpmIcmpQuotedFlowV1&) const = 0;
};

struct NpmIcmpEventV1 {
    uint64_t entity_instance_id = 0;
    int64_t observed_at = 0;
    uint64_t observation_domain_id = 0;
    uint8_t ip_family = 0;
    std::string src_ip;
    std::string dst_ip;
    std::string outcome;
    uint8_t icmp_type = 0;
    uint8_t icmp_code = 0;
    bool outer_truncated = false;
    std::optional<std::string> incomplete_reason;
    std::optional<uint16_t> echo_id;
    std::optional<uint16_t> echo_sequence;
    std::optional<uint32_t> echo_retries;
    std::optional<int64_t> request_at_ns;
    std::optional<int64_t> reply_at_ns;
    std::optional<int64_t> latency_ns;
    std::optional<std::string> quote_status;
    std::optional<uint8_t> quoted_ip_family;
    std::optional<uint8_t> quoted_protocol;
    std::optional<std::string> quoted_src_ip;
    std::optional<std::string> quoted_dst_ip;
    std::optional<uint16_t> quoted_src_port;
    std::optional<uint16_t> quoted_dst_port;
    std::optional<uint64_t> active_quoted_session_id;
    std::optional<uint32_t> next_hop_mtu;
};

enum class NpmIcmpConfigErrorV1 : uint8_t {
    kNone = 0,
    kInvalidType,
    kInvalidRange,
    kDuplicateField,
    kUnknownField,
    kAllocationFailed,
};

struct NpmIcmpConfigStatusV1 {
    NpmIcmpConfigErrorV1 error = NpmIcmpConfigErrorV1::kNone;
    std::string path;
};

NpmIcmpConfigStatusV1 ParseNpmIcmpConfigV1(std::string_view json, NpmIcmpConfigV1* output);
NpmEntityDescriptorV1 NpmIcmpEventEntityDescriptorV1();
NpmModulePlanV1 NpmIcmpModulePlanV1();

}  // namespace flowsql::npm

#endif  // FLOWSQL_NPM_ICMP_CONTRACT_H_
