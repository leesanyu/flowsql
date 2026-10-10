// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <framework/interfaces/idatabase_channel.h>
#include <memory>
namespace flowsql {
// Independent optional capability; existing transform and managed-sink virtual tables are unchanged.
interface IBlockTransformResultOutputProviderV1 {
    virtual ~IBlockTransformResultOutputProviderV1() = default;
    virtual bool SupportsResultOutput() const = 0;
};
struct BlockResultOutputBindingV1 {
    uint32_t struct_size = sizeof(BlockResultOutputBindingV1);
    uint32_t contract_version = 1;
    const char* target = nullptr;                // Borrowed during Bind; empty for inline results.
    std::shared_ptr<IDatabaseChannel> database;  // Required only for a specified database table.
};
// Bind before input/Open. Only the primary results Schema enters a specified table/DataFrame.
// Database publication belongs to the task. Scheduler confirms DataFrame registration after EOF;
// failure leaves each already committed output visible in the run summary.
interface IBlockTransformResultOutputTaskV1 {
    virtual ~IBlockTransformResultOutputTaskV1() = default;
    virtual int BindResultOutput(const BlockResultOutputBindingV1& binding) = 0;
    virtual int CompleteResultOutputPublication(bool published) = 0;
};
}  // namespace flowsql
