// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef FLOWSQL_FRAMEWORK_INTERFACES_IBLOCK_STREAM_CHANNEL_DESCRIPTOR_H_
#define FLOWSQL_FRAMEWORK_INTERFACES_IBLOCK_STREAM_CHANNEL_DESCRIPTOR_H_

#include <framework/interfaces/ibuiltin_registry.h>

namespace flowsql {

// {cadbd2e8-a751-465f-9ae4-1c3de850beb4}
const Guid IID_BLOCK_STREAM_CHANNEL_DESCRIPTOR = {
    0xcadbd2e8, 0xa751, 0x465f, {0x9a, 0xe4, 0x1c, 0x3d, 0xe8, 0x50, 0xbe, 0xb4}};

struct BlockStreamChannelTypeDescriptor {
    std::string channel_type;
    std::string display_name;
    std::vector<std::string> allowed_roles;
    std::vector<StreamOptionField> option_schema;
    bool is_finite = false;
    bool supports_reset = false;
};

/** Optional management metadata; channel creation remains with IBlockStreamManager. */
interface IBlockStreamChannelDescriptor {
    virtual ~IBlockStreamChannelDescriptor() = default;
    virtual void DescribeChannelTypes(std::function<void(const BlockStreamChannelTypeDescriptor&)> callback) const = 0;
};

}  // namespace flowsql

#endif
