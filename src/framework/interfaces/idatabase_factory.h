// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_IDATABASE_FACTORY_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_IDATABASE_FACTORY_H_

#include <common/guid.h>
#include <common/typedef.h>

#include <functional>
#include <memory>

#include "idatabase_channel.h"

namespace flowsql {

// {0xa9b8c7d6, 0xe5f4, 0x3210, {0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10}}
const Guid IID_DATABASE_FACTORY = {0xa9b8c7d6, 0xe5f4, 0x3210,
                                   {0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10}};

// {0x7b56c4e1, 0x5db8, 0x4af6, {0xb7, 0x1d, 0x7e, 0xe1, 0x9d, 0x32, 0x7a, 0x44}}
const Guid IID_DATABASE_CHANNEL_LEASE_PROVIDER_V1 = {
    0x7b56c4e1, 0x5db8, 0x4af6, {0xb7, 0x1d, 0x7e, 0xe1, 0x9d, 0x32, 0x7a, 0x44}};

/** Optional owning lease capability while the provider plugin is running. */
interface IDatabaseChannelLeaseProviderV1 {
    virtual ~IDatabaseChannelLeaseProviderV1() = default;
    virtual std::shared_ptr<IDatabaseChannel> AcquireChannel(const char* type, const char* name) = 0;
};

/**
 * @brief 数据库通道工厂接口，负责数据库通道生命周期管理。
 */
interface IDatabaseFactory {
    virtual ~IDatabaseFactory() = default;

    /**
     * @brief 获取或创建数据库通道（懒加载）。
     * @param type 数据库类型，例如 "sqlite"、"mysql"、"postgres"、"clickhouse"。
     * @param name 通道名称。
     * @return 通道指针（工厂持有所有权），失败返回 nullptr。
     */
    virtual IDatabaseChannel* Get(const char* type, const char* name) = 0;

    /**
     * @brief 带用户上下文获取数据库通道（扩展入口）。
     * @param type 数据库类型。
     * @param name 通道名称。
     * @param user_context 用户上下文字符串。
     * @return 通道指针（工厂持有所有权），失败返回 nullptr。
     */
    virtual IDatabaseChannel* GetWithContext(const char* type, const char* name,
                                             const char* user_context) {
        return Get(type, name);
    }

    /**
     * @brief 枚举已配置数据库通道。
     * @param callback 枚举回调：
     * type 为数据库类型，name 为通道名，config_json 为脱敏后的配置 JSON（可为空）。
     */
    virtual void List(std::function<void(const char* type, const char* name,
                                         const char* config_json)> callback) = 0;

    /**
     * @brief 释放指定通道并从缓存中移除。
     * @param type 数据库类型。
     * @param name 通道名称。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int Release(const char* type, const char* name) = 0;

    /**
     * @brief 获取最近一次错误信息。
     * @return 错误字符串指针（线程局部存储）。
     */
    virtual const char* LastError() = 0;

    // ==================== 动态管理方法（Epic 6）====================

    /**
     * @brief 运行时新增数据库通道配置。
     * @param config_str 通道配置字符串（key=value;key=value...）。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int AddChannel(const char* config_str) { return -1; }

    /**
     * @brief 运行时删除数据库通道配置。
     * @param type 数据库类型。
     * @param name 通道名称。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int RemoveChannel(const char* type, const char* name) { return -1; }

    /**
     * @brief 运行时更新数据库通道配置。
     * @param config_str 通道配置字符串（key=value;key=value...）。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int UpdateChannel(const char* config_str) { return -1; }
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_IDATABASE_FACTORY_H_
