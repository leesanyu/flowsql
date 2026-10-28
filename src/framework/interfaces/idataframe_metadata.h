// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <arrow/type.h>
#include <cstdint>
#include <memory>
#include <string>

namespace flowsql {
struct DataFrameMetadataV1 {
    std::shared_ptr<arrow::Schema> schema;
    uint64_t replacement_generation = 0;
    bool pending = false;
    std::string error;
};
// Optional in-process channel capability. Never loads data or copies record buffers.
struct IDataFrameMetadataV1 {
    virtual ~IDataFrameMetadataV1() = default;
    virtual DataFrameMetadataV1 DescribeMetadata() = 0;
};
}  // namespace flowsql
