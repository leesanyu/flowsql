// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef FLOWSQL_FRAMEWORK_INTERFACES_IBLOCK_TRANSFORM_DATABASE_INPUT_H_
#define FLOWSQL_FRAMEWORK_INTERFACES_IBLOCK_TRANSFORM_DATABASE_INPUT_H_

#include <common/guid.h>
#include <common/typedef.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace arrow {
class Schema;
}
namespace flowsql {
interface IDatabaseChannel;
interface IBlockStreamChannel;

const Guid IID_BLOCK_TRANSFORM_DATABASE_INPUT_TASK_V1 = {
    0xd51b8f74, 0x4132, 0x4a4e, {0x99, 0x11, 0x74, 0x01, 0x0b, 0x8c, 0x71, 0x39}};
const Guid IID_BLOCK_STREAM_INPUT_PROGRESS_V1 = {
    0xd5218eb3, 0x9d15, 0x46a7, {0x87, 0x19, 0x32, 0x8a, 0x6e, 0x1b, 0x44, 0x76}};
const Guid IID_BLOCK_TRANSFORM_INPUT_PROGRESS_TASK_V1 = {
    0x5c8136d9, 0xe8be, 0x4c12, {0xae, 0x45, 0x2f, 0xf1, 0x89, 0xb4, 0xaa, 0x26}};
constexpr uint32_t kBlockDatabaseInputVersionV1 = 1;
constexpr uint32_t kBlockInputProgressVersionV1 = 1;

struct BlockDatasetProgressV1 {
    std::string dataset_id;
    std::string epoch;
    std::string committed_position;
    // All earlier buckets are complete and immutable. Position is an opaque committed prefix.
    int64_t closed_before_bucket = 0;
};
struct BlockInputProgressV1 {
    uint32_t contract_version = kBlockInputProgressVersionV1;
    std::vector<BlockDatasetProgressV1> datasets;
};

struct BlockDatabaseInputBindingV1 {
    uint32_t struct_size = sizeof(BlockDatabaseInputBindingV1);
    uint32_t contract_version = kBlockDatabaseInputVersionV1;
    std::shared_ptr<IDatabaseChannel> source;
    const char* exact_source = nullptr;  // Borrowed during CreateDatabaseInput; task copies text.
    uint32_t operation_timeout_ms = 5000;
    uint32_t max_cursor_bytes = 1024 * 1024;
};
struct BlockDatabaseInputV1 {
    uint32_t struct_size = sizeof(BlockDatabaseInputV1);
    uint32_t contract_version = kBlockDatabaseInputVersionV1;
    IBlockStreamChannel* input = nullptr;
    std::shared_ptr<arrow::Schema> schema;
};
inline bool ValidBlockDatabaseInputBindingV1(const BlockDatabaseInputBindingV1& value) {
    return value.struct_size == sizeof(value) && value.contract_version == kBlockDatabaseInputVersionV1 &&
           value.source && value.exact_source && value.exact_source[0] != '\0' && value.operation_timeout_ms > 0 &&
           value.operation_timeout_ms <= 3600000 && value.max_cursor_bytes > 0 && value.max_cursor_bytes <= 1024 * 1024;
}

// Optional task capability, discovered independently; existing transform virtual tables stay unchanged.
interface IBlockTransformDatabaseInputTaskV1 {
    virtual ~IBlockTransformDatabaseInputTaskV1() = default;
    // Once, before Open. Config must already be validated and the managed target bound.
    // Validate input/output size/version. Failure leaves output.input=nullptr and schema empty.
    // Creates an exclusive task reader and owns its allocation domain. Reader holds the source lease.
    // Metadata may be read here; data Poll begins only after successful task Open/restore.
    // Configured source must equal exact_source. Non-capability operators retain the old query path.
    virtual int CreateDatabaseInput(const BlockDatabaseInputBindingV1& binding, BlockDatabaseInputV1* output) = 0;
    // Once per successful creation, after Poll/ReleaseBlock/Cancel calls finish, before ReleaseTask.
    // Closes/cancels reader; outstanding owned Arrow products and Schema must die before plugin unload.
    virtual void ReleaseDatabaseInput(IBlockStreamChannel * input) = 0;
};

interface IBlockStreamInputProgressV1 {
    virtual ~IBlockStreamInputProgressV1() = default;
    // Side-effect-free owned snapshot. Read only after the corresponding batch was ReleaseBlock'ed.
    // Progress-only updates use an owned zero-row kData batch, never timeout-as-EOF.
    virtual int ReadInputProgress(BlockInputProgressV1 * output) const = 0;
};
interface IBlockTransformInputProgressTaskV1 {
    virtual ~IBlockTransformInputProgressTaskV1() = default;
    // Serialized with Open/ProcessBlock/OnTime/Flush/restore. No borrowed text is retained.
    // Accept only adapter-proven epochs/prefixes/closed buckets. Durable position advances on publish.
    virtual int AcceptInputProgress(const BlockInputProgressV1& progress) = 0;
};

}  // namespace flowsql
#endif
