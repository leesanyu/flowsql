// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_PLUGINS_NPM_BASIC_NPM_SESSION_KEY_H_
#define _FLOWSQL_PLUGINS_NPM_BASIC_NPM_SESSION_KEY_H_

#include "npm_analysis_contract.h"

#include <cstdint>

namespace flowsql::npm {

enum class NpmSessionPacketError : uint8_t {
    kNone = 0,
    kNullOutput,
    kInvalidObservationDomainMap,
    kUnknownSourceId,
    kInvalidPacketBounds,
    kLayerUnavailable,
    kMalformedLayer,
    kInvalidLayerSelection,
    kAmbiguousTunnelContext,
    kIncompleteEndpoint,
    kUnsupportedTransportProtocol,
    kNonInitialFragment,
    kIncompleteTransportHeader,
    kInvalidPayloadBounds,
};

/** TCP control facts copied from a validated transport header. Sequence is in host byte order. */
struct NpmTcpControl {
    bool valid = false;
    bool syn = false;
    bool ack = false;
    bool fin = false;
    bool rst = false;
    uint32_t sequence = 0;
};

/** Owns the normalized key and borrows payload bytes from the current packet callback. */
struct NpmSessionPacketBinding {
    NpmSessionKey key;
    NpmPacketDirection direction = NpmPacketDirection::kAToB;
    Span<const uint8_t> payload;
    NpmTcpControl tcp;
};

/** Builds one packet-to-session binding. Output remains unchanged on every error. */
NpmSessionPacketError BuildNpmSessionPacketBinding(const NpmObservationDomainMap& domain_map,
                                                   const packet::PacketView& packet,
                                                   const packet::PacketLayerInfo& layer,
                                                   NpmSessionPacketBinding* output);

}  // namespace flowsql::npm

#endif  // _FLOWSQL_PLUGINS_NPM_BASIC_NPM_SESSION_KEY_H_
