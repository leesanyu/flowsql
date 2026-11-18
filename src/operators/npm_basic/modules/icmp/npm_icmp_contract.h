// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef FLOWSQL_NPM_ICMP_CONTRACT_H_
#define FLOWSQL_NPM_ICMP_CONTRACT_H_

#include <operators/npm_basic/npm_family_result_contract.h>
#include <operators/npm_basic/npm_protocol_contract.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace flowsql::npm {

constexpr int64_t kNpmIcmpDefaultEchoTimeoutNs = 5'000'000'000;
constexpr int64_t kNpmIcmpMinEchoTimeoutNs = 1'000'000;
constexpr int64_t kNpmIcmpMaxEchoTimeoutNs = 300'000'000'000;
constexpr uint32_t kNpmIcmpDefaultMaxPendingEcho = 4096;

struct NpmIcmpConfig {
    int64_t echo_timeout_ns = kNpmIcmpDefaultEchoTimeoutNs;
    uint32_t max_pending_echo = kNpmIcmpDefaultMaxPendingEcho;
};

struct NpmIcmpPendingEcho {
    uint64_t entity_instance_id = 0;
    int64_t request_at_ns = 0;
    int64_t deadline_ns = 0;
    uint32_t retries = 0;
};

enum class NpmIcmpConfigError : uint8_t {
    kNone = 0,
    kInvalidType,
    kInvalidRange,
    kDuplicateField,
    kUnknownField,
    kAllocationFailed,
};

struct NpmIcmpConfigStatus {
    NpmIcmpConfigError error = NpmIcmpConfigError::kNone;
    std::string path;
};

NpmIcmpConfigStatus ParseNpmIcmpConfig(std::string_view json, NpmIcmpConfig* output);
NpmEntityDescriptor NpmIcmpEventEntityDescriptor(packet::AddressFamily family);
NpmModulePlan NpmIcmpModulePlan();

}  // namespace flowsql::npm

#endif  // FLOWSQL_NPM_ICMP_CONTRACT_H_
