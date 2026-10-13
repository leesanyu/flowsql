// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include "npm_session_table.h"

namespace flowsql::npm {

constexpr size_t kNpmTypedEntityRowLimit = 512;
constexpr uint64_t kNpmTypedStorageSoftLimit = kNpmMebibyte;

struct NpmResultDeliveryEntity {
    NpmEntityDescriptorV1 descriptor;
    bool observing = false;
    bool consuming = false;
    bool RequiresEncoding() const noexcept { return observing || consuming; }
};

struct NpmResultDeliveryPlan {
    // Created at Open and passed as const thereafter; contains every enabled entity.
    std::vector<NpmResultDeliveryEntity> entities;
    size_t entity_row_limit = kNpmTypedEntityRowLimit;
    uint64_t typed_storage_soft_limit = kNpmTypedStorageSoftLimit;
};

// T4 collector implements this together with INpmResultWriter. Write accepts borrowed rows
// synchronously; FlushAccepted delivers ALL entities before foreground Drain/publication/Finish.
// Any delivery error is terminal, keeps the original error/committed prefix, and forbids retry.
interface INpmAcceptedResultDelivery {
    virtual ~INpmAcceptedResultDelivery() = default;
    virtual int FlushAccepted() = 0;
    virtual size_t pending_typed_rows() const noexcept = 0;
    virtual void DiscardAccepted() noexcept = 0;
};

}  // namespace flowsql::npm
