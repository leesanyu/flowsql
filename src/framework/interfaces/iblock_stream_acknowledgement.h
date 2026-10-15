// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <memory>
namespace arrow {
class RecordBatch;
}
namespace flowsql {
// Optional cross-cast capability. ReleaseBlock always remains a resource operation.
// Executors acknowledge only after successful processing and current output delivery.
struct IBlockStreamAcknowledgementV1 {
    virtual ~IBlockStreamAcknowledgementV1() = default;
    virtual int AcknowledgeBlock(const std::shared_ptr<arrow::RecordBatch>& block) = 0;
};
}  // namespace flowsql
