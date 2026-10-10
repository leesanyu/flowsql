// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <operators/npm_basic/npm_protocol_contract.h>

namespace flowsql::npm {
struct NpmResultProgressV1 {
    uint32_t contract_version = 1;
    std::string entity_id = "basic";
    int64_t period_ns = 0;
    // Only period increments are immutable below this UTC epoch boundary.
    // Zero-increment terminal metadata may arrive later and is not a period fact.
    int64_t closed_before_ns = 0;
};
// Optional, separate consumer capability. Called serially after all entity writes/drains succeed.
// A failed/unknown publication is terminal; caller never retries it or advances its confirmed boundary.
interface INpmResultProgressConsumerV1 {
    virtual ~INpmResultProgressConsumerV1() = default;
    virtual int PublishProgress(const NpmResultContextV1& context, const NpmResultProgressV1& progress) = 0;
};
}  // namespace flowsql::npm
