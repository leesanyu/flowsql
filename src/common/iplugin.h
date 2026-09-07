// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_COMMON_IPLUGIN_H_
#define _FLOWSQL_COMMON_IPLUGIN_H_

#include "guid.h"
#include "typedef.h"
#include "iquerier.hpp"

namespace flowsql {

/**
 * @brief 插件注册接口，由加载器在插件注册阶段调用。
 */
interface IRegister {
    /**
     * @brief 注册插件暴露的接口实例。
     * @param iid 接口唯一标识。
     * @param iface 接口实例指针（非拥有语义）。
     */
    virtual void Regist(const Guid& iid, void* iface) = 0;
};

// {86dc3d8-e65f-9a83-1a39-66d26e95a9ca}
const Guid IID_PLUGIN = {0x86dc3d8, 0xe65f, 0x9a83, {0x1a, 0x39, 0x66, 0xd2, 0x6e, 0x95, 0xa9, 0xca}};

// Marker IID: the registered value is the same IPlugin* and its Start() activates an externally reachable entry.
// {f37d063e-799f-4c61-9174-5bef3f12d140}
const Guid IID_PLUGIN_EXTERNAL_ENTRY = {
    0xf37d063e, 0x799f, 0x4c61, {0x91, 0x74, 0x5b, 0xef, 0x3f, 0x12, 0xd1, 0x40}};

/**
 * @brief 插件生命周期接口，定义插件装载与运行阶段回调。
 *
 * 生命周期并发契约：
 * - 宿主框架必须按批次串行执行：所有插件 Option 成功后再执行所有 Load，
 *   所有 Load 成功后才可执行任何 Start。
 * - 宿主框架必须串行调用同一插件实例的 Option/Load/Start/Stop/Unload。
 * - Stop/Unload 不得与该插件已暴露接口的业务调用并发执行。
 * - 若宿主框架允许生命周期回调与业务接口并发，必须由宿主框架在调用
 *   插件前提供外部同步；插件实现不应假设自身会被并发卸载。
 * - 插件自身暴露的业务接口是否支持并发调用，由对应接口文档单独声明。
 */
interface IPlugin {
    virtual ~IPlugin(){};

    /**
     * @brief 解析插件配置参数。
     * @param arg 插件启动参数字符串，可为空。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int Option(const char* /* arg */) { return 0; }
    /**
     * @brief 执行插件加载逻辑并初始化本插件暴露的能力。
     * @param querier 插件查询器；依赖其他插件 Load 结果时，可保存该指针并在 Start 阶段查询接口。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int Load(IQuerier* querier) = 0;
    /**
     * @brief 执行插件卸载逻辑并释放资源。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int Unload() = 0;

    /**
     * @brief 启动插件运行逻辑。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int Start() { return 0; }
    /**
     * @brief 停止插件运行逻辑。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int Stop() { return 0; }
};

}  // namespace flowsql

// --- 插件注册宏 ---

#define BEGIN_PLUGIN_REGIST(classname)                                                          \
    EXPORT_API void pluginunregist() {}                                                         \
                                                                                                \
    EXPORT_API flowsql::IPlugin* pluginregist(flowsql::IRegister* registry, const char* opt) {  \
        static classname _plugin;

#define ____INTERFACE(iid, intername)                           \
    {                                                           \
        intername* iface = dynamic_cast<intername*>(&_plugin);  \
        registry->Regist(iid, iface);                           \
    }

#define END_PLUGIN_REGIST()         \
    if (_plugin.Option(opt) != 0) { \
        return nullptr;             \
    }                               \
    return &_plugin;                \
    }

#endif  // _FLOWSQL_COMMON_IPLUGIN_H_
