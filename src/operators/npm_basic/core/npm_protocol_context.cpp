// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_protocol_context.h"

#include <new>
#include <utility>

namespace flowsql::npm {
namespace {

NpmProtocolContextError MapAcquireError(ProtocolPipelinePoolError error) {
    switch (error) {
        case ProtocolPipelinePoolError::kNone:
            return NpmProtocolContextError::kNone;
        case ProtocolPipelinePoolError::kUnavailable:
            return NpmProtocolContextError::kPipelineUnavailable;
        case ProtocolPipelinePoolError::kExhausted:
            return NpmProtocolContextError::kPipelineExhausted;
        default:
            return NpmProtocolContextError::kPipelineAcquireFailed;
    }
}

const char* LookupProtocolName(protocol::IDictionary* dictionary, uint16_t protocol_id) {
    if (dictionary == nullptr || protocol_id == 0) return nullptr;
    const protocol::Entry* entry = dictionary->Query(protocol_id);
    if (entry == nullptr || entry->number != protocol_id || entry->name == nullptr || entry->name[0] == '\0') {
        return nullptr;
    }
    return entry->name;
}

}  // namespace

NpmProtocolContextError NpmProtocolContext::Create(IQuerier* querier, std::unique_ptr<NpmProtocolContext>* output) {
    if (output == nullptr) return NpmProtocolContextError::kNullOutput;
    if (querier == nullptr) return NpmProtocolContextError::kNullQuerier;

    auto* pool = static_cast<IProtocolPipelinePoolV1*>(querier->First(IID_PROTOCOL_PIPELINE_POOL_V1));
    if (pool == nullptr) return NpmProtocolContextError::kProviderNotFound;

    IProtocol* protocol = pool->Protocol();
    if (protocol == nullptr) return NpmProtocolContextError::kProtocolUnavailable;
    protocol::IDictionary* dictionary = protocol->Dictionary();
    if (dictionary == nullptr) return NpmProtocolContextError::kDictionaryUnavailable;

    int32_t pipeno = -1;
    const ProtocolPipelinePoolError acquire_error = pool->Acquire(&pipeno);
    const NpmProtocolContextError context_error = MapAcquireError(acquire_error);
    if (context_error != NpmProtocolContextError::kNone) return context_error;

    std::unique_ptr<NpmProtocolContext> context(new (std::nothrow)
                                                    NpmProtocolContext(pool, protocol, dictionary, pipeno));
    if (!context) {
        pool->Release(pipeno);
        return NpmProtocolContextError::kAllocationFailed;
    }

    *output = std::move(context);
    return NpmProtocolContextError::kNone;
}

NpmProtocolContext::NpmProtocolContext(IProtocolPipelinePoolV1* pool, IProtocol* protocol,
                                       protocol::IDictionary* dictionary, int32_t pipeno)
    : pool_(pool), dictionary_(dictionary), pipeno_(pipeno), identifier_(protocol, pipeno) {}

NpmProtocolContext::~NpmProtocolContext() {
    if (pool_ != nullptr && pipeno_ >= 0) pool_->Release(pipeno_);
}

packet::IPacketProtocolIdentifier* NpmProtocolContext::Identifier() { return &identifier_; }

protocol::IDictionary* NpmProtocolContext::Dictionary() const { return dictionary_; }

int32_t NpmProtocolContext::Pipeno() const { return pipeno_; }

const char* NpmProtocolContext::ResolveProtocolName(uint16_t protocol_id, uint16_t protocol_sub_id) const {
    const char* name = LookupProtocolName(dictionary_, protocol_sub_id);
    return name != nullptr ? name : LookupProtocolName(dictionary_, protocol_id);
}

}  // namespace flowsql::npm
