// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <arrow/type.h>
#include <cstdint>
#include <memory>
#include <string>

namespace flowsql {
struct DataFrameMetadata {
    std::shared_ptr<arrow::Schema> schema;
    uint64_t replacement_generation = 0;
    bool pending = false;
    std::string error;
};
// Optional in-process channel capability. Never loads data or copies record buffers.
struct IDataFrameMetadata {
    virtual ~IDataFrameMetadata() = default;
    virtual DataFrameMetadata DescribeMetadata() = 0;
};
}  // namespace flowsql
