// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_OPERATORS_NPM_BASIC_NPM_PROTOCOL_CONTEXT_H_
#define _FLOWSQL_OPERATORS_NPM_BASIC_NPM_PROTOCOL_CONTEXT_H_

#include <common/iquerier.hpp>
#include <plugins/npi/packet_decoder.h>

#include <cstdint>
#include <memory>

namespace flowsql::npm {

enum class NpmProtocolContextError : uint8_t {
    kNone = 0,
    kNullQuerier,
    kNullOutput,
    kProviderNotFound,
    kProtocolUnavailable,
    kDictionaryUnavailable,
    kPipelineUnavailable,
    kPipelineExhausted,
    kPipelineAcquireFailed,
    kAllocationFailed,
};

/**
 * Task-owned NPI protocol context. Its lifetime must not cross the provider's Stop/Unload boundary.
 */
class NpmProtocolContext final {
 public:
    static NpmProtocolContextError Create(IQuerier* querier, std::unique_ptr<NpmProtocolContext>* output);

    ~NpmProtocolContext();

    NpmProtocolContext(const NpmProtocolContext&) = delete;
    NpmProtocolContext& operator=(const NpmProtocolContext&) = delete;
    NpmProtocolContext(NpmProtocolContext&&) = delete;
    NpmProtocolContext& operator=(NpmProtocolContext&&) = delete;

    packet::IPacketProtocolIdentifier* Identifier();
    protocol::IDictionary* Dictionary() const;
    int32_t Pipeno() const;

    /** Returns a dictionary-owned name, preferring a valid nonzero sub ID, or nullptr when unresolved. */
    const char* ResolveProtocolName(uint16_t protocol_id, uint16_t protocol_sub_id) const;

 private:
    NpmProtocolContext(IProtocolPipelinePoolV1* pool,
                       IProtocol* protocol,
                       protocol::IDictionary* dictionary,
                       int32_t pipeno);

    IProtocolPipelinePoolV1* pool_ = nullptr;
    protocol::IDictionary* dictionary_ = nullptr;
    int32_t pipeno_ = -1;
    protocol::NpiPacketProtocolIdentifier identifier_;
};

}  // namespace flowsql::npm

#endif  // _FLOWSQL_OPERATORS_NPM_BASIC_NPM_PROTOCOL_CONTEXT_H_
