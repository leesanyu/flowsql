// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_IBLOCK_STREAM_READER_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_IBLOCK_STREAM_READER_H_

#include <common/guid.h>
#include <common/typedef.h>

#include <cstdint>

#include "iblock_stream_channel.h"

namespace flowsql {

// {e7a13c9d-5b42-4df8-86a1-c37e2094f65b}
const Guid IID_BLOCK_STREAM_READER_FACTORY_V1 = {
    0xe7a13c9d, 0x5b42, 0x4df8, {0x86, 0xa1, 0xc3, 0x7e, 0x20, 0x94, 0xf6, 0x5b}};

constexpr uint32_t kBlockStreamReaderContractVersionV1 = 1;

/** Temporary planner-owned input. A successful provider copies every string before returning. */
struct BlockStreamReaderConfigV1 {
    uint32_t contract_version = kBlockStreamReaderContractVersionV1;
    const char* task_id = nullptr;
    const char* source_category = nullptr;
    const char* source_name = nullptr;
    // Exact pushed expression only; use the versioned canonical empty plan for zero pushdown.
    const char* pushed_filter_plan_json = nullptr;
};

/** Stateless provider that creates one task-exclusive block-stream reader per successful call. */
interface IBlockStreamReaderFactoryV1 {
    virtual ~IBlockStreamReaderFactoryV1() = default;

    /**
     * @return 0 with a non-null exclusive reader, ENOTSUP when this provider does not own the named
     *         source, or another nonzero error. Nonzero returns must leave reader null.
     */
    virtual int CreateReader(const BlockStreamReaderConfigV1& config,
                             IBlockStreamChannel** reader) = 0;

    /** Called only after every method invocation on reader has completed. */
    virtual void ReleaseReader(IBlockStreamChannel* reader) = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_IBLOCK_STREAM_READER_H_
