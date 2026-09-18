// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_ICONFIG_CHANNEL_REGISTRY_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_ICONFIG_CHANNEL_REGISTRY_H_

#include <common/guid.h>
#include <common/typedef.h>

#include <cstdint>
#include <memory>
#include <string>

namespace flowsql {

// {7a25ee39-9f46-4ad7-b924-d84dd45efa42}
const Guid IID_CONFIG_CHANNEL_REGISTRY_V1 = {
    0x7a25ee39, 0x9f46, 0x4ad7, {0xb9, 0x24, 0xd8, 0x4d, 0xd4, 0x5e, 0xfa, 0x42}};

struct ConfigChannelSnapshot {
    std::string channel_name;
    uint64_t revision;
    std::string format;
    std::string schema_id;
    std::string sha256_hex;
    uint64_t content_bytes;
    int64_t created_at_unix_ms;
    std::shared_ptr<const std::string> content;
};

/** Read-only, thread-safe snapshot lookup; independent of the SQL channel registry. */
interface IConfigChannelRegistryV1 {
    virtual ~IConfigChannelRegistryV1() = default;

    /**
     * Resolve an exact config.<name>@<revision> reference into an owned snapshot.
     * Returns 0 on success, or a nonzero error on failure.
     * A successful snapshot owns its content independently of the provider's lifetime.
     */
    virtual int Resolve(const char* exact_reference,
                        ConfigChannelSnapshot* snapshot,
                        std::string* error) = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_ICONFIG_CHANNEL_REGISTRY_H_
