// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include <common/guid.h>
#include <common/typedef.h>

#include <cstdint>
#include <string>

namespace flowsql {

const Guid IID_KAFKA_CONNECTION_SERVICE_V1 = {
    0x2e3562c1, 0x3412, 0x4a88, {0x97, 0x2c, 0xe1, 0x71, 0xcb, 0x32, 0x8f, 0x69}};

const Guid IID_KAFKA_CHANNEL_CATALOG_V1 = {
    0x9bf46062, 0x872b, 0x42f6, {0xa6, 0xed, 0x15, 0xe3, 0x5c, 0xd7, 0x9a, 0x42}};

struct KafkaChannelDescriptionV1 {
    std::string role;
    uint64_t revision = 0;
    bool source_adapter_available = false, sink_adapter_available = false;
};
interface IKafkaChannelCatalogV1 {
    virtual ~IKafkaChannelCatalogV1() = default;
    // Read-only discovery; never resolves credentials or opens a network client.
    virtual int DescribeChannel(const char* name, KafkaChannelDescriptionV1* description) = 0;
};

struct KafkaBindingSnapshotV1 {
    uint32_t contract_version = 1;
    std::string channel_name, connection_name, topic, consumer_group, payload_kind;
    uint64_t channel_revision = 0, connection_revision = 0;
    std::string usage;
};

interface IKafkaClientSessionV1 {
    virtual ~IKafkaClientSessionV1() = default;
    // Immutable borrowed snapshot; copy before outliving this session.
    virtual const KafkaBindingSnapshotV1& Snapshot() const = 0;
    // One operation at a time; Cancel may run concurrently and wakes blocking work.
    virtual int Probe(uint32_t timeout_ms, std::string * redacted_result_json) = 0;
    virtual void Cancel() = 0;
    // Capability pointers are borrowed until ReleaseSession; ENOTSUP returns nullptr.
    virtual int QueryInterface(const Guid& iid, void** capability) = 0;
};

interface IKafkaConnectionServiceV1 {
    virtual ~IKafkaConnectionServiceV1() = default;
    // Thread-safe open freezes configuration and owns credentials; never waits on network.
    // Failure writes nullptr. Usage must be source or sink.
    virtual int OpenSession(const char* channel_name, const char* usage, IKafkaClientSessionV1** session,
                            std::string* redacted_error) = 0;
    // Caller must finish method calls and callbacks before releasing exactly once.
    virtual void ReleaseSession(IKafkaClientSessionV1 * session) = 0;
};

}  // namespace flowsql
