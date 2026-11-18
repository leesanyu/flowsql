// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <common/guid.h>
#include <common/typedef.h>
#include <cstdint>
#include <string>
#include <vector>
namespace flowsql {
const Guid IID_KAFKA_PRODUCER_SESSION = {0x6a193e84, 0xecb4, 0x47ca, {0x86, 0xdd, 0xa0, 0x19, 0xcc, 0xd4, 0x7f, 0x93}};
struct KafkaProduceMessage {
    std::string key, value;
};
struct KafkaDelivery {
    uint64_t confirmed = 0, not_sent = 0, unknown = 0;
};
interface IKafkaProducerSession {
    virtual ~IKafkaProducerSession() = default;
    // Borrowed messages; success requires delivery callbacks. All return paths account for every message.
    virtual int Produce(const std::vector<KafkaProduceMessage>&, uint32_t timeout_ms, KafkaDelivery*) = 0;
    virtual int Drain(uint32_t timeout_ms) = 0;
};
}  // namespace flowsql
