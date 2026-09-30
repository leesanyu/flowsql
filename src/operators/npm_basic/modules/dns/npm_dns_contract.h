// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef FLOWSQL_NPM_DNS_CONTRACT_H_
#define FLOWSQL_NPM_DNS_CONTRACT_H_

#include <operators/npm_basic/npm_protocol_contract.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace flowsql::npm {

constexpr int64_t kNpmDnsDefaultResponseTimeoutNsV1 = 5'000'000'000;
constexpr int64_t kNpmDnsMinResponseTimeoutNsV1 = 1'000'000;
constexpr int64_t kNpmDnsMaxResponseTimeoutNsV1 = 300'000'000'000;
constexpr uint32_t kNpmDnsDefaultMaxPendingPerSessionV1 = 256;
constexpr uint32_t kNpmDnsMaxPendingPerSessionV1 = 4096;
constexpr std::size_t kNpmDnsMaxPrimaryLabelsV1 = 256;

/** Task-owned values; no JSON, matcher, or stream view survives preparation. */
struct NpmDnsConfigV1 {
    std::vector<uint32_t> primary_label_ids;
    int64_t response_timeout_ns = kNpmDnsDefaultResponseTimeoutNsV1;
    uint32_t max_pending_per_session = kNpmDnsDefaultMaxPendingPerSessionV1;
};

/** Wire QNAME is a sequence of length-prefixed labels with ASCII case folded. */
struct NpmDnsQuestionKeyV1 {
    uint64_t session_id = 0;
    NpmPacketDirection query_direction = NpmPacketDirection::kAToB;
    uint16_t dns_id = 0;
    uint8_t opcode = 0;
    std::string wire_qname;
    uint16_t qtype = 0;
    uint16_t qclass = 0;
};

struct NpmDnsMessageV1 {
    uint16_t dns_id = 0;
    uint8_t opcode = 0;
    bool is_response = false;
    bool truncated = false;
    uint16_t response_rcode = 0;
    std::optional<std::string> wire_qname;
    std::optional<uint16_t> qtype;
    std::optional<uint16_t> qclass;
    int64_t complete_at_ns = 0;
};

struct NpmDnsTransactionV1 {
    uint64_t entity_instance_id = 0;
    NpmDnsQuestionKeyV1 key;
    std::optional<int64_t> query_at_ns;
    std::optional<int64_t> deadline_ns;
    uint32_t query_retries = 0;
    std::optional<int64_t> response_at_ns;
    std::optional<uint16_t> response_rcode;
    std::optional<bool> response_tc;
};

enum class NpmDnsConfigErrorV1 : uint8_t {
    kNone = 0,
    kMissing,
    kInvalidType,
    kInvalidRange,
    kDuplicateField,
    kUnknownField,
    kAllocationFailed,
};

struct NpmDnsConfigStatusV1 {
    NpmDnsConfigErrorV1 error = NpmDnsConfigErrorV1::kNone;
    std::string path;
};

/** Parses one borrowed parameters.dns object. Replaces output only on success. */
NpmDnsConfigStatusV1 ParseNpmDnsConfigV1(std::string_view json, NpmDnsConfigV1* output);
NpmEntityDescriptorV1 NpmDnsTransactionEntityDescriptorV1();
NpmModulePlanV1 NpmDnsModulePlanV1(const NpmDnsConfigV1& config);

}  // namespace flowsql::npm

#endif  // FLOWSQL_NPM_DNS_CONTRACT_H_
