// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <framework/interfaces/icapture_block_stream_reader.h>
#include <memory>
#include <string>
namespace arrow {
class Schema;
}
namespace flowsql {
const Guid IID_PACKET_SINK_FACTORY = {0x3f072e61, 0xb167, 0x4974, {0xb2, 0x10, 0x86, 0x9d, 0x4b, 0x70, 0x96, 0x38}};
struct PacketSinkContext {
    uint32_t contract_version = 1;
    std::string target, task_id;
};
struct PacketSinkCounts {
    uint64_t input = 0, confirmed = 0, not_sent = 0, unknown = 0;
};
struct PacketSinkReport {
    std::string state, code, stage, binding_json;
    PacketSinkCounts messages, packets;
};
// Open/Write/Finish are serialized. Cancel and Report may run concurrently.
// Write borrows the complete event until it returns; success means delivery acknowledgement.
interface IPacketSinkWriter {
    virtual ~IPacketSinkWriter() = default;
    virtual int Open(const CaptureSourceSet&, std::shared_ptr<arrow::Schema>) = 0;
    virtual int Write(const CapturePollEvent&) = 0;
    virtual int Finish() = 0;
    virtual void Cancel() = 0;
    virtual PacketSinkReport Report() const = 0;
};
interface IPacketSinkFactory {
    virtual ~IPacketSinkFactory() = default;
    virtual int CreateWriter(const PacketSinkContext&, std::unique_ptr<IPacketSinkWriter>*) = 0;
};
}  // namespace flowsql
