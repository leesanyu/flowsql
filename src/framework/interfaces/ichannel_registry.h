#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_ICHANNEL_REGISTRY_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_ICHANNEL_REGISTRY_H_

#include <common/guid.h>
#include <common/typedef.h>

#include <functional>
#include <memory>

#include "ichannel.h"

namespace flowsql {

// {0xb6f1c3d2-8a31-4e63-bb65-11a17f0d9e41}
const Guid IID_CHANNEL_REGISTRY = {
    0xb6f1c3d2, 0x8a31, 0x4e63, {0xbb, 0x65, 0x11, 0xa1, 0x7f, 0x0d, 0x9e, 0x41}};

// IChannelRegistry — 具名通道注册中心
// name 不含 catalog 前缀（例如 "result"，而不是 "dataframe.result"）
interface IChannelRegistry {
    virtual ~IChannelRegistry() = default;

    // 注册具名通道，同名已存在返回 -1
    virtual int Register(const char* name, std::shared_ptr<IChannel> channel) = 0;

    // 按名查找（未注册返回 nullptr）
    virtual std::shared_ptr<IChannel> Get(const char* name) = 0;

    // 注销通道（未注册返回 -1）
    virtual int Unregister(const char* name) = 0;

    // 原子重命名（更新注册表，底层是否持久化由实现决定）
    virtual int Rename(const char* old_name, const char* new_name) = 0;

    // 枚举所有已注册通道
    virtual void List(std::function<void(const char* name, std::shared_ptr<IChannel>)> callback) = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_ICHANNEL_REGISTRY_H_
