// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_PLUGINS_PROTOCOL_NPI_PACKET_DECODER_H_
#define _FLOWSQL_PLUGINS_PROTOCOL_NPI_PACKET_DECODER_H_

#include <framework/core/packet_family_codec.h>

#include "iprotocol.h"

namespace flowsql::protocol {

class NpiPacketLayerDecoder final : public packet::IPacketFamilyLayerDecoder {
 public:
    explicit NpiPacketLayerDecoder(IProtocol* protocol, int32_t pipeno = 0);

    int Decode(const packet::PacketView& packet, const packet::LayerDecodeOptions& options,
               packet::PacketLayerFacts* facts, packet::IPacketAddressSink& addresses) override;
    int Decode(const packet::PacketView& packet, const packet::LayerDecodeOptions& options,
               packet::PacketFamilyBatch& batch, packet::PacketFamilyRecord* record) {
        packet::PacketFamilyBatchAddressSink sink(batch);
        const int rc = Decode(packet, options, &record->layer, sink);
        if (rc == 0) record->address = sink.address;
        return rc;
    }

 private:
    IProtocol* protocol_ = nullptr;
    int32_t pipeno_ = 0;
};

class NpiPacketProtocolIdentifier final : public packet::IPacketFamilyProtocolIdentifier {
 public:
    explicit NpiPacketProtocolIdentifier(IProtocol* protocol, int32_t pipeno = 0);

    packet::PacketProtocolInfo Identify(const packet::PacketView& packet,
                                        const packet::PacketLayerFacts& layer) override;

 private:
    IProtocol* protocol_ = nullptr;
    int32_t pipeno_ = 0;
};

}  // namespace flowsql::protocol

#endif  // _FLOWSQL_PLUGINS_PROTOCOL_NPI_PACKET_DECODER_H_
