// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_FRAMEWORK_CORE_PACKET_CODEC_H_
#define _FLOWSQL_FRAMEWORK_CORE_PACKET_CODEC_H_

#include <framework/interfaces/ipacket.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace arrow {
class RecordBatch;
class Schema;
class MemoryPool;
}  // namespace arrow

namespace flowsql::packet {

enum class PacketEnvelopeError : uint8_t {
    kNone = 0,
    kNullOutput,
    kLengthMismatch,
    kNullData,
    kWireLengthTooSmall,
    kAllocationFailed,
};

enum class PacketBatchError : uint8_t {
    kNone = 0,
    kNullOutput,
    kInvalidRecord,
    kArrowError,
    kAllocationFailed,
};

PacketEnvelopeError ValidatePacketView(const PacketView& packet);

PacketEnvelopeError CopyPacketBytes(const PacketView& packet, PacketBytes* output);

std::shared_ptr<arrow::Schema> PacketSchema();

PacketBatchError EncodePacketBatch(const std::vector<PacketRecord>& records,
                                   std::shared_ptr<arrow::RecordBatch>* output, std::string* error = nullptr);

/** next returns 0 for a borrowed record or ENOENT at end; bytes are copied before the next call. */
PacketBatchError EncodePacketStream(const std::function<int(PacketRecord*)>& next, uint32_t max_records,
                                    arrow::MemoryPool* pool, std::shared_ptr<arrow::RecordBatch>* output,
                                    std::string* error = nullptr);

}  // namespace flowsql::packet

#endif  // _FLOWSQL_FRAMEWORK_CORE_PACKET_CODEC_H_
