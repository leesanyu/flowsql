// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#ifndef FLOWSQL_FRAMEWORK_INTERFACES_IBLOCK_TRANSFORM_DATAFRAME_INPUT_H_
#define FLOWSQL_FRAMEWORK_INTERFACES_IBLOCK_TRANSFORM_DATAFRAME_INPUT_H_
#include <common/guid.h>
#include <common/typedef.h>
#include <cstdint>
#include <memory>
#include <string>
namespace arrow {
class Schema;
}
namespace flowsql {
interface IDataFrameChannel;
interface IBlockStreamChannel;
const Guid IID_BLOCK_TRANSFORM_DATAFRAME_INPUT_TASK_V1 = {
    0x5c7e0f81, 0x0c6d, 0x4d92, {0xb8, 0xf1, 0x39, 0x67, 0x8e, 0x12, 0xa4, 0x65}};
const Guid IID_BLOCK_TRANSFORM_DATAFRAME_INPUT_PROVIDER_V1 = {
    0x7b45a310, 0x1ee6, 0x4a07, {0xb1, 0x92, 0x5f, 0x18, 0x90, 0x67, 0xcc, 0x24}};
// Provider opt-in allows Scheduler to keep ordinary DataFrame operators on their existing path.
interface IBlockTransformDataFrameInputProviderV1 {
    virtual ~IBlockTransformDataFrameInputProviderV1() = default;
    virtual bool SupportsDataFrameInput() const = 0;
};
constexpr uint32_t kBlockDataFrameInputVersionV1 = 1;
struct BlockDataFrameInputBindingV1 {
    uint32_t struct_size = sizeof(BlockDataFrameInputBindingV1);
    uint32_t contract_version = kBlockDataFrameInputVersionV1;
    std::shared_ptr<IDataFrameChannel> source;
    const char* exact_source = nullptr;  // Borrowed during creation; task copies text.
};
struct BlockDataFrameInputV1 {
    uint32_t struct_size = sizeof(BlockDataFrameInputV1);
    uint32_t contract_version = kBlockDataFrameInputVersionV1;
    IBlockStreamChannel* input = nullptr;
    std::shared_ptr<arrow::Schema> schema;
    std::string source_fingerprint;  // Owned versioned full snapshot content/Schema evidence.
};
inline bool ValidBlockDataFrameInputBindingV1(const BlockDataFrameInputBindingV1& value) {
    return value.struct_size == sizeof(value) && value.contract_version == kBlockDataFrameInputVersionV1 &&
           value.source && value.exact_source && value.exact_source[0] != '\0';
}
// Independent optional task capability; existing V1/V2 and database virtual tables stay unchanged.
interface IBlockTransformDataFrameInputTaskV1 {
    virtual ~IBlockTransformDataFrameInputTaskV1() = default;
    // Once before Open, after validated config and managed target binding. Validate both size/version pairs.
    // Capture one non-destructive Arrow snapshot and retain source lease. Data Poll starts after Open/restore.
    // Failure clears input/schema/fingerprint. Reader and Schema belong to the task allocation domain.
    virtual int CreateDataFrameInput(const BlockDataFrameInputBindingV1& binding, BlockDataFrameInputV1* output) = 0;
    // After all Poll/ReleaseBlock/Cancel calls finish, before ReleaseTask. Closes the task-owned reader.
    // Outstanding Arrow products and Schema must be destroyed before plugin unload.
    virtual void ReleaseDataFrameInput(IBlockStreamChannel * input) = 0;
};
}  // namespace flowsql
#endif
