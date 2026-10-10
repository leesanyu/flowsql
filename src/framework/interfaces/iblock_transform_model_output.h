// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <framework/interfaces/idatabase_channel.h>
#include <framework/interfaces/idataframe_channel.h>
#include <memory>
#include <string>
namespace flowsql {
struct BlockModelOutputBindingV1 {
    uint32_t struct_size = sizeof(BlockModelOutputBindingV1);
    uint32_t contract_version = 1;
    const char* primary_target = nullptr;  // Borrowed during Bind only; may be empty for inline results.
    std::shared_ptr<IDatabaseChannel> database;
    std::shared_ptr<IDataFrameChannel> dataframe;  // Private staging channel, not yet registered.
};
// Independent optional side-output capability. Existing V1/V2 and managed-sink ABI stays unchanged.
// Bind before Open/input creation. Exactly one owning target lease must match ModelOutputTarget().
// Normal EOF stages a DataFrame or commits the database. Failures fail the task, cancellation does
// not publish a DataFrame. Scheduler registers staged data only on kCompleted, then confirms it.
interface IBlockTransformModelOutputTaskV1 {
    virtual ~IBlockTransformModelOutputTaskV1() = default;
    virtual std::string ModelOutputTarget() const = 0;  // Empty: no export or final model traversal.
    virtual int BindModelOutput(const BlockModelOutputBindingV1& binding) = 0;
    virtual int CompleteModelOutputPublication(bool published) = 0;
};
}  // namespace flowsql
