// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_OPERATORS_NPM_BASIC_NPM_SESSION_KEY_H_
#define _FLOWSQL_OPERATORS_NPM_BASIC_NPM_SESSION_KEY_H_

#include <framework/interfaces/iflow_labeling.h>
#include <operators/npm_basic/npm_protocol_contract.h>

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

/** Owns the normalized key and borrows payload bytes from the current packet callback. */
struct NpmSessionPacketBinding {
    NpmSessionKey key;
    NpmPacketDirection direction = NpmPacketDirection::kAToB;
    Span<const uint8_t> payload;
    NpmTransportPacketFacts transport;
    FlowLabelFactsV1 label_facts;
};

/** Validates network bounds and normalizes control protocol metadata and exposes a body without a port session. */
NpmSessionPacketError BuildNpmControlInput(const NpmObservationDomainMap&, const packet::PacketView&,
                                           packet::PacketLayerInfo&, NpmInputEventV1*);

/** Builds one packet-to-session binding. Output remains unchanged on every error. */
NpmSessionPacketError BuildNpmSessionPacketBinding(const NpmObservationDomainMap& domain_map,
                                                   const packet::PacketView& packet,
                                                   const packet::PacketLayerInfo& layer,
                                                   NpmSessionPacketBinding* output);

}  // namespace flowsql::npm

#endif  // _FLOWSQL_OPERATORS_NPM_BASIC_NPM_SESSION_KEY_H_
