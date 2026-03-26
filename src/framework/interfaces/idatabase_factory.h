#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_IDATABASE_FACTORY_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_IDATABASE_FACTORY_H_

#include <common/guid.h>
#include <common/typedef.h>

#include <functional>

#include "idatabase_channel.h"

namespace flowsql {

// {0xa9b8c7d6, 0xe5f4, 0x3210, {0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10}}
const Guid IID_DATABASE_FACTORY = {0xa9b8c7d6, 0xe5f4, 0x3210,
                                   {0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10}};

// IDatabaseFactory — 数据库通道工厂接口
// 管理数据库通道的创建、缓存和释放
interface IDatabaseFactory {
    virtual ~IDatabaseFactory() = default;

    // 获取或创建数据库通道实例（懒加载）
    // type: "sqlite", "mysql", "postgres", "clickhouse"
    // name: 通道名称（如 "mydb"）
    // 返回: 通道指针（工厂持有所有权），失败返回 nullptr
    virtual IDatabaseChannel* Get(const char* type, const char* name) = 0;

    // [预留] 获取数据库通道（带用户上下文）
    virtual IDatabaseChannel* GetWithContext(const char* type, const char* name,
                                             const char* user_context) {
        return Get(type, name);
    }

    // 列出所有已配置的数据库连接
    // config_json：该通道的配置 JSON（密码字段脱敏为 "****"），可为 nullptr
    virtual void List(std::function<void(const char* type, const char* name,
                                         const char* config_json)> callback) = 0;

    // 释放指定通道（关闭连接，从池中移除）
    virtual int Release(const char* type, const char* name) = 0;

    // 获取最近一次操作的错误信息（线程安全：thread_local 存储）
    virtual const char* LastError() = 0;

    // ==================== 动态管理方法（Epic 6）====================

    // 运行时新增通道（写 YAML，加入 configs_，懒加载连接）
    // config_str 格式：type=xxx;name=xxx;host=xxx;...
    // type+name 已存在时返回 -1（请用 UpdateChannel）
    virtual int AddChannel(const char* config_str) { return -1; }

    // 运行时删除通道（关闭连接，从 YAML 删除，从 configs_ 移除）
    virtual int RemoveChannel(const char* type, const char* name) { return -1; }

    // 运行时更新通道配置（原子覆盖写 YAML，不走 Remove+Add）
    // config_str 格式同 AddChannel，type+name 不存在时返回 -1
    virtual int UpdateChannel(const char* config_str) { return -1; }
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_IDATABASE_FACTORY_H_
