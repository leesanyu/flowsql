// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <common/guid.h>
#include <common/typedef.h>
namespace flowsql {
const Guid IID_BLOCK_TRANSFORM_EXECUTION_POLICY_PROVIDER = {
    0xe81a9fa4, 0xf063, 0x4a61, {0x92, 0x47, 0x03, 0x85, 0xa1, 0x59, 0x67, 0x3f}};
// Optional provider capability. Resolves config only, without creating a task or opening I/O.
// Both inline parameters and exact Config Channel references use the provider's normal configuration validator.
interface IBlockTransformExecutionPolicyProvider {
    virtual ~IBlockTransformExecutionPolicyProvider() = default;
    virtual int RequiresAsyncExecution(const char* with_params_json, const char* exact_source, bool* output) const = 0;
};
}  // namespace flowsql
