// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef FLOWSQL_NPM_TLS_CONTRACT_H_
#define FLOWSQL_NPM_TLS_CONTRACT_H_

#include <operators/npm_basic/npm_protocol_contract.h>
#include <operators/npm_basic/npm_tcp_stream_contract.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace flowsql::npm {

constexpr int64_t kNpmTlsDefaultHandshakeTimeoutNsV1 = 5'000'000'000;
constexpr int64_t kNpmTlsMinHandshakeTimeoutNsV1 = 1'000'000;
constexpr int64_t kNpmTlsMaxHandshakeTimeoutNsV1 = 300'000'000'000;
constexpr uint32_t kNpmTlsDefaultMaxHelloBytesV1 = 65'536;
constexpr uint32_t kNpmTlsMinHelloBytesV1 = 4'096;
constexpr uint32_t kNpmTlsMaxHelloBytesV1 = 1'048'576;
constexpr std::size_t kNpmTlsMaxPrimaryLabelsV1 = 256;

/** Task-owned configuration; no JSON or provider view survives preparation. */
struct NpmTlsConfigV1 {
    std::vector<uint32_t> primary_label_ids;
    int64_t handshake_timeout_ns = kNpmTlsDefaultHandshakeTimeoutNsV1;
    uint32_t max_hello_bytes = kNpmTlsDefaultMaxHelloBytesV1;
};

/** Complete, owned initial Hello facts; opaque ALPN IDs remain byte strings until output encoding. */
struct NpmTlsHelloFactsV1 {
    uint16_t legacy_version = 0;
    std::vector<uint16_t> offered_versions;
    std::optional<uint16_t> selected_version;
    std::optional<uint16_t> cipher_suite;
    std::optional<std::string> sni;
    std::vector<std::string> offered_alpn;
    std::optional<std::string> selected_alpn;
    bool hello_retry_request = false;
    std::optional<int64_t> complete_at_ns;
};

/** Per-direction scalar framing state and one bounded, owned Hello copy. */
struct NpmTlsDirectionStateV1 {
    NpmTcpStreamOriginV1 origin = NpmTcpStreamOriginV1::kUnknown;
    std::array<uint8_t, 5> record_header{};
    uint8_t record_header_bytes = 0;
    uint32_t record_remaining = 0;
    std::array<uint8_t, 4> handshake_header{};
    uint8_t handshake_header_bytes = 0;
    uint32_t handshake_remaining = 0;
    std::string hello_bytes;
    bool first_record_checked = false;
    bool gap_seen = false;
    bool ended = false;
    bool encrypted_boundary = false;
};

enum class NpmTlsHandshakePhaseV1 : uint8_t {
    kCandidate,
    kAwaitingServerHello,
    kAwaitingRetryClientHello,
    kAwaitingFinalServerHello,
    kAwaitingTls12Boundary,
    kClosed,
};

/** One candidate per base session; first ClientHello remains the output/time reference after a retry. */
struct NpmTlsHandshakeV1 {
    uint64_t entity_instance_id = 0;
    uint64_t session_id = 0;
    NpmPacketDirection client_direction = NpmPacketDirection::kAToB;
    NpmTlsHandshakePhaseV1 phase = NpmTlsHandshakePhaseV1::kCandidate;
    std::optional<NpmTlsHelloFactsV1> first_client_hello;
    std::optional<NpmTlsHelloFactsV1> retry_client_hello;
    std::optional<NpmTlsHelloFactsV1> final_server_hello;
    uint8_t hello_retry_count = 0;
    std::optional<int64_t> first_client_byte_at_ns;
    std::optional<int64_t> deadline_ns;
};

enum class NpmTlsConfigErrorV1 : uint8_t {
    kNone = 0,
    kMissing,
    kInvalidType,
    kInvalidRange,
    kDuplicateField,
    kUnknownField,
    kAllocationFailed,
};

struct NpmTlsConfigStatusV1 {
    NpmTlsConfigErrorV1 error = NpmTlsConfigErrorV1::kNone;
    std::string path;
};

/** Parses one borrowed parameters.tls object and replaces output only on success. */
NpmTlsConfigStatusV1 ParseNpmTlsConfigV1(std::string_view json, NpmTlsConfigV1* output);
NpmEntityDescriptorV1 NpmTlsHandshakeEntityDescriptorV1();
NpmModulePlanV1 NpmTlsModulePlanV1(const NpmTlsConfigV1& config);

}  // namespace flowsql::npm

#endif  // FLOWSQL_NPM_TLS_CONTRACT_H_
