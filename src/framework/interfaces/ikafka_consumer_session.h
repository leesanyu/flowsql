// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <common/guid.h>
#include <common/typedef.h>
#include <cstdint>
#include <string>
#include <vector>

namespace flowsql {
const Guid IID_KAFKA_CONSUMER_SESSION_V1 = {
    0x3d962b1a, 0x2d16, 0x46e3, {0xa5, 0xe6, 0x82, 0x40, 0xbe, 0x49, 0x26, 0x17}};
struct KafkaMessageV1 {
    std::string topic, value, key;
    int32_t partition = 0;
    int64_t offset = 0, timestamp_ms = -1;
    bool value_is_null = false, key_is_null = true;
};
struct KafkaOffsetV1 {
    int32_t partition = 0;
    int64_t next_offset = 0;
};
interface IKafkaConsumerSessionV1 {
    virtual ~IKafkaConsumerSessionV1() = default;
    // Once before polling; reset is earliest/latest/error. No implicit offset store or commit.
    virtual int StartConsumer(const char* reset, uint32_t max_message_bytes) = 0;
    // 0 = owned message, EAGAIN = timeout, ECANCELED = cancelled; other errors are terminal.
    virtual int PollMessage(uint32_t timeout_ms, KafkaMessageV1 * message) = 0;
    // Explicit synchronous confirmation, bounded by timeout_ms; only this session's Topic.
    virtual int CommitOffsets(const std::vector<KafkaOffsetV1>& offsets, uint32_t timeout_ms) = 0;
};
}  // namespace flowsql
