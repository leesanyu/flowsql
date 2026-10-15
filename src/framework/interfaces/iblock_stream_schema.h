// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <memory>
namespace arrow {
class Schema;
}
namespace flowsql {
// Optional channel capability. The returned immutable Schema has shared lifetime ownership.
struct IBlockStreamSchemaV1 {
    virtual ~IBlockStreamSchemaV1() = default;
    virtual std::shared_ptr<arrow::Schema> ArrowSchema() const = 0;
};
}  // namespace flowsql
