// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef FLOWSQL_NPM_HTTP1_CONTRACT_H_
#define FLOWSQL_NPM_HTTP1_CONTRACT_H_

#include <operators/npm_basic/npm_protocol_contract.h>
#include <operators/npm_basic/npm_tcp_stream_contract.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace flowsql::npm {

constexpr int64_t kNpmHttp1DefaultResponseTimeoutNsV1 = 5'000'000'000;
constexpr int64_t kNpmHttp1MinResponseTimeoutNsV1 = 1'000'000;
constexpr int64_t kNpmHttp1MaxResponseTimeoutNsV1 = 300'000'000'000;
constexpr uint32_t kNpmHttp1DefaultMaxPendingPerSessionV1 = 128;
constexpr uint32_t kNpmHttp1MaxPendingPerSessionV1 = 4096;
constexpr uint32_t kNpmHttp1DefaultMaxHeaderBytesV1 = 65536;
constexpr uint32_t kNpmHttp1MinHeaderBytesV1 = 1024;
constexpr uint32_t kNpmHttp1MaxHeaderBytesV1 = 1048576;
constexpr std::size_t kNpmHttp1MaxPrimaryLabelsV1 = 256;

/** Task-owned configuration; no JSON, matcher, or borrowed stream views survive preparation. */
struct NpmHttp1ConfigV1 {
    std::vector<uint32_t> primary_label_ids;
    int64_t response_timeout_ns = kNpmHttp1DefaultResponseTimeoutNsV1;
    uint32_t max_pending_per_session = kNpmHttp1DefaultMaxPendingPerSessionV1;
    uint32_t max_header_bytes = kNpmHttp1DefaultMaxHeaderBytesV1;
};

enum class NpmHttp1VersionV1 : uint8_t { k10, k11 };
enum class NpmHttp1FramingV1 : uint8_t { kNoBody, kContentLength, kChunked, kCloseDelimited };

/** Owned, safely parsed header facts. A missing capture time remains null. */
struct NpmHttp1MessageHeadV1 {
    bool is_response = false;
    NpmHttp1VersionV1 version = NpmHttp1VersionV1::k11;
    std::optional<std::string> method;
    std::optional<std::string> target;
    std::optional<std::string> host;
    std::optional<uint16_t> status_code;
    NpmHttp1FramingV1 framing = NpmHttp1FramingV1::kNoBody;
    std::optional<int64_t> complete_at_ns;
};

/** Per-direction private state; retained bytes must be budgeted before allocation. */
struct NpmHttp1DirectionStateV1 {
    NpmTcpStreamOriginV1 origin = NpmTcpStreamOriginV1::kUnknown;
    std::string header_bytes;
    uint32_t header_bytes_seen = 0;
    uint64_t body_remaining = 0;
    uint64_t chunk_remaining = 0;
    uint64_t chunk_line_value = 0;
    uint32_t chunk_line_bytes = 0;
    uint32_t trailer_bytes_seen = 0;
    bool gap_seen = false;
    bool ended = false;
    bool alignment_lost = false;
};

struct NpmHttp1TransactionV1 {
    uint64_t entity_instance_id = 0;
    uint64_t session_id = 0;
    NpmPacketDirection request_direction = NpmPacketDirection::kAToB;
    NpmHttp1MessageHeadV1 request;
    uint64_t fifo_position = 0;
    std::optional<int64_t> deadline_ns;
    uint32_t informational_count = 0;
};

enum class NpmHttp1ConfigErrorV1 : uint8_t {
    kNone = 0,
    kMissing,
    kInvalidType,
    kInvalidRange,
    kDuplicateField,
    kUnknownField,
    kAllocationFailed,
};

struct NpmHttp1ConfigStatusV1 {
    NpmHttp1ConfigErrorV1 error = NpmHttp1ConfigErrorV1::kNone;
    std::string path;
};

/** Parses one borrowed parameters.http1 object. Replaces output only on success. */
NpmHttp1ConfigStatusV1 ParseNpmHttp1ConfigV1(std::string_view json, NpmHttp1ConfigV1* output);
NpmEntityDescriptorV1 NpmHttp1TransactionEntityDescriptorV1();
NpmModulePlanV1 NpmHttp1ModulePlanV1(const NpmHttp1ConfigV1& config);

}  // namespace flowsql::npm

#endif  // FLOWSQL_NPM_HTTP1_CONTRACT_H_
