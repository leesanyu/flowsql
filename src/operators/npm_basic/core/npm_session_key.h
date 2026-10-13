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

struct NpmPacketBindingView {
    NpmSessionKeyView key;
    NpmPacketDirection direction = NpmPacketDirection::kAToB;
    Span<const uint8_t> payload;
    NpmTransportPacketFacts transport;
    FlowLabelFactsV1 label_facts;
};

/** Validates network bounds and normalizes control protocol metadata and exposes a body without a port session. */
NpmSessionPacketError BuildNpmControlInput(const NpmObservationDomainLookup&, const packet::PacketView&,
                                           packet::PacketFamilyLayerView&, NpmInputEventV1*);

/** Builds one packet-to-session binding. Output remains unchanged on every error. */
template <typename Key>
NpmSessionPacketError BuildNpmSessionPacketBinding(const NpmObservationDomainLookup& domain_map,
                                                   const packet::PacketView& packet,
                                                   const packet::PacketFamilyLayerView& layer, Key* key,
                                                   NpmPacketBindingView* output);

}  // namespace flowsql::npm

#endif  // _FLOWSQL_OPERATORS_NPM_BASIC_NPM_SESSION_KEY_H_
