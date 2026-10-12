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

constexpr int64_t kNpmIcmpDefaultEchoTimeoutNsV1 = 5'000'000'000;
constexpr int64_t kNpmIcmpMinEchoTimeoutNsV1 = 1'000'000;
constexpr int64_t kNpmIcmpMaxEchoTimeoutNsV1 = 300'000'000'000;
constexpr uint32_t kNpmIcmpDefaultMaxPendingEchoV1 = 4096;

struct NpmIcmpConfigV1 {
    int64_t echo_timeout_ns = kNpmIcmpDefaultEchoTimeoutNsV1;
    uint32_t max_pending_echo = kNpmIcmpDefaultMaxPendingEchoV1;
};

struct NpmIcmpPendingEchoV1 {
    uint64_t entity_instance_id = 0;
    int64_t request_at_ns = 0;
    int64_t deadline_ns = 0;
    uint32_t retries = 0;
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
NpmEntityDescriptorV1 NpmIcmpEventEntityDescriptorV1(packet::AddressFamily family);
NpmModulePlanV1 NpmIcmpModulePlanV1();

}  // namespace flowsql::npm

#endif  // FLOWSQL_NPM_ICMP_CONTRACT_H_
