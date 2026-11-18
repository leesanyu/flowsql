// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef FLOWSQL_NPM_TLS_HELLO_H_
#define FLOWSQL_NPM_TLS_HELLO_H_

#include "npm_tls_contract.h"

namespace flowsql::npm {

enum class NpmTlsHelloError : uint8_t { kNone, kMalformed, kLimit, kInvalidOutput, kAllocationFailed };

/** Parses one complete Hello body. Input is borrowed; output is replaced only on success. */
NpmTlsHelloError ParseNpmTlsHello(uint8_t message_type, std::string_view body, std::optional<int64_t> complete_at_ns,
                                  NpmTlsHelloFacts* output);

/** Encodes opaque ALPN IDs as a reversible, size-limited JSON string array. */
NpmTlsHelloError EncodeNpmTlsAlpnJson(const std::vector<std::string>& protocols, std::string* output);

}  // namespace flowsql::npm

#endif  // FLOWSQL_NPM_TLS_HELLO_H_
