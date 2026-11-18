// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <common/guid.h>
#include <common/typedef.h>
#include <string>
namespace flowsql {
const Guid IID_BLOCK_TRANSFORM_SOURCE_CONFIG_PROVIDER = {
    0xb237c5aa, 0x85a7, 0x4b38, {0xa3, 0x5f, 0x91, 0x72, 0x47, 0x81, 0x2c, 0x60}};
// Optional capability for resolving source configuration.
// Declares source-aware configuration and database relation input support. Database tasks
// created from the normalized config must implement IBlockTransformDatabaseInputTask.
interface IBlockTransformSourceConfigProvider {
    virtual ~IBlockTransformSourceConfigProvider() = default;
    // Before CreateTask, including planning probes. No task creation or source/target I/O.
    // May resolve an exact Config Channel snapshot. Copies borrowed text; owned WITH output
    // binds the exact SQL source. Failure leaves output unchanged and returns owned error text.
    virtual int NormalizeSourceConfig(const char* with_json, const char* exact_source, std::string* output,
                                      std::string* error) const = 0;
};
}  // namespace flowsql
