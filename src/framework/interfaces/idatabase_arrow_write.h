// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <framework/interfaces/idatabase_channel.h>

namespace arrow {
class MemoryPool;
}

namespace flowsql {

// Optional synchronous columnar write capability. Existing channel vtables are unchanged.
interface IDatabaseArrowWriteV1 {
    virtual ~IDatabaseArrowWriteV1() = default;

    // Borrow batches and pool only until return. Allocate IPC buffers through the supplied non-null pool.
    // Return 0 only after the INSERT response succeeds; never retry an unknown write automatically.
    virtual int WriteArrowBatchesWithPool(const char* table,
                                          const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches,
                                          arrow::MemoryPool* pool) = 0;
};

}  // namespace flowsql
