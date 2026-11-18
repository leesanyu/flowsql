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

constexpr int64_t kNpmDnsDefaultResponseTimeoutNs = 5'000'000'000;
constexpr int64_t kNpmDnsMinResponseTimeoutNs = 1'000'000;
constexpr int64_t kNpmDnsMaxResponseTimeoutNs = 300'000'000'000;
constexpr uint32_t kNpmDnsDefaultMaxPendingPerSession = 256;
constexpr uint32_t kNpmDnsMaxPendingPerSession = 4096;
constexpr std::size_t kNpmDnsMaxPrimaryLabels = 256;

/** Task-owned values; no JSON, matcher, or stream view survives preparation. */
struct NpmDnsConfig {
    std::vector<uint32_t> primary_label_ids;
    int64_t response_timeout_ns = kNpmDnsDefaultResponseTimeoutNs;
    uint32_t max_pending_per_session = kNpmDnsDefaultMaxPendingPerSession;
};

/** Wire QNAME is a sequence of length-prefixed labels with ASCII case folded. */
struct NpmDnsQuestionKey {
    uint64_t session_id = 0;
    NpmPacketDirection query_direction = NpmPacketDirection::kAToB;
    uint16_t dns_id = 0;
    uint8_t opcode = 0;
    std::string wire_qname;
    uint16_t qtype = 0;
    uint16_t qclass = 0;
};

struct NpmDnsMessage {
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

struct NpmDnsTransaction {
    uint64_t entity_instance_id = 0;
    NpmDnsQuestionKey key;
    std::optional<int64_t> query_at_ns;
    std::optional<int64_t> deadline_ns;
    uint32_t query_retries = 0;
    std::optional<int64_t> response_at_ns;
    std::optional<uint16_t> response_rcode;
    std::optional<bool> response_tc;
};

enum class NpmDnsConfigError : uint8_t {
    kNone = 0,
    kMissing,
    kInvalidType,
    kInvalidRange,
    kDuplicateField,
    kUnknownField,
    kAllocationFailed,
};

struct NpmDnsConfigStatus {
    NpmDnsConfigError error = NpmDnsConfigError::kNone;
    std::string path;
};

/** Parses one borrowed parameters.dns object. Replaces output only on success. */
NpmDnsConfigStatus ParseNpmDnsConfig(std::string_view json, NpmDnsConfig* output);
NpmEntityDescriptor NpmDnsTransactionEntityDescriptor(packet::AddressFamily family);
NpmModulePlan NpmDnsModulePlan(const NpmDnsConfig& config);

}  // namespace flowsql::npm

#endif  // FLOWSQL_NPM_DNS_CONTRACT_H_
