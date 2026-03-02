/*
 * Copyright (C) 2020-06 - flowSQL
 *
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 *
 * Author       : LIHUO
 * Date         : 2020-10-07 16:42:22
 * LastEditors  : LIHUO
 * LastEditTime : 2026-02-25 12:00:00
 */
#ifndef _FLOWSQL_COMMON_LOADER_HPP_
#define _FLOWSQL_COMMON_LOADER_HPP_

#include <functional>
#include <map>
#include <vector>
#include "toolkit.hpp"
#include "guid.h"
#include "typedef.h"
#include "iquerier.hpp"

namespace flowsql {
interface IRegister {
    virtual void Regist(const Guid &iid, void *iface) = 0;  // interface
};

// 86dc3d8-e65f-9a83-1a39-66d26e95a9ca
const Guid IID_PLUGIN = {0x86dc3d8, 0xe65f, 0x9a83, {0x1a, 0x39, 0x66, 0xd2, 0x6e, 0x95, 0xa9, 0xca}};
interface IPlugin {
    virtual ~IPlugin(){};

    virtual int Option(const char * /* arg */) { return 0; }
    virtual int Load() = 0;    // do not call any interface in this func.
    virtual int Unload() = 0;  // do not call any interface in this func.

    // 模块启停（原 IModule 接口，合并后默认空实现，轻量插件无需覆写）
    virtual int Start() { return 0; }
    virtual int Stop() { return 0; }
};

class PluginLoader : public IRegister, public IQuerier {
 public:
    int Load(const char *fullpath[], int count);
    int Load(const char *path, const char *relapath[], const char *option[], int count);
    int Unload();

 public:
    // IRegister.
    virtual void Regist(const Guid &iid, void *iface);

    // IQuerier.
    virtual int Traverse(const Guid &iid, fntraverse proc);
    virtual void *First(const Guid &iid);

 public:
    static PluginLoader *Single() {
        static PluginLoader _loader;
        return &_loader;
    }

 public:
    // 直接访问接口列表，供 PluginRegistry::BuildIndex 使用（消除 fntraverse 限制）
    const std::vector<void *> *GetInterfaces(const Guid &iid) const {
        auto it = ifs_ref_.find(iid);
        return (it != ifs_ref_.end()) ? &it->second : nullptr;
    }

 public:
    // 插件启停 — 由 PluginRegistry 在锁外调用，避免 Start() 回调 Register() 死锁
    int StartAll();
    void StopAll();

 private:
    PluginLoader() {}

    std::map<Guid, std::vector<void *>> ifs_ref_;
    std::vector<thandle> plugins_ref_;
    size_t started_count_ = 0;  // 已启动的插件数量，避免重复启动
};

typedef flowsql::IPlugin *(*fnregister)(flowsql::IRegister *, const char *);
inline int PluginLoader::Load(const char *fullpath[], int count) {
    std::string app_path = get_absolute_process_path();
    app_path += "/";
    for (int pos = 0; pos < count; ++pos) {
        std::string lib_path = app_path + fullpath[pos];
        thandle h = loadlibrary(lib_path.c_str());
        if (!h) {
            printf("Load shared library '%s' faild, error : '%s'\n", lib_path.c_str(), getlasterror());
            return -1;
        }

        // iface regist.
        fnregister fregist = (fnregister)getprocaddress(h, "pluginregist");
        if (!fregist) {
            printf("'%s' getprocaddress of '%s' faild\n", lib_path.c_str(), "pluginregist");
            freelibrary(h);
            return -1;
        }

        // 记录注册前 IPlugin 数量，用于遍历本次 .so 注册的所有 IPlugin
        size_t prev_count = ifs_ref_[flowsql::IID_PLUGIN].size();
        fregist(this, nullptr);

        // 阶段 2: 遍历本次 .so 注册的所有 IPlugin 调用 Load()
        auto &plugins = ifs_ref_[flowsql::IID_PLUGIN];
        for (size_t i = prev_count; i < plugins.size(); ++i) {
            auto *plugin = reinterpret_cast<flowsql::IPlugin *>(plugins[i]);
            if (-1 == plugin->Load()) {
                printf("'%s' plugin load faild\n", lib_path.c_str());
                freelibrary(h);
                return -1;
            }
        }

        plugins_ref_.push_back(h);
    }

    // 注意：不在此处调用 StartAll()
    // 模块启动由 PluginRegistry::LoadPlugin() 在释放锁后单独调用，
    // 避免 Start() 回调 Register() 时与 LoadPlugin 持有的锁产生死锁
    return 0;
}

inline int PluginLoader::Load(const char *path, const char *relapath[], const char *option[], int count) {
    char realpath[PATH_MAX] = {0};
    size_t pathlen = strlen(path);
    if (pathlen + 1 >= PATH_MAX) {
        printf("library path '%s' too long.\n", path);
        return -1;
    }
    strncpy(realpath, path, pathlen);
    realpath[pathlen++] = '/';
    for (int pos = 0; pos < count; ++pos) {
        thandle h = 0;
        if (relapath[pos][0] == '/' || relapath[pos][0] == '.') {
            h = loadlibrary(relapath[pos]);
        } else {
            strncpy(realpath + pathlen, relapath[pos], strlen(relapath[pos]) + 1);
            h = loadlibrary(realpath);
        }
        if (!h) {
            printf("Load shared library '%s' faild, error : '%s'\n", relapath[pos], getlasterror());
            return -1;
        }

        // iface regist.
        fnregister fregist = (fnregister)getprocaddress(h, "pluginregist");
        if (!fregist) {
            printf("'%s' getprocaddress of '%s' faild\n", relapath[pos], "pluginregist");
            freelibrary(h);
            return -1;
        }

        // 记录注册前 IPlugin 数量
        size_t prev_count = ifs_ref_[flowsql::IID_PLUGIN].size();
        fregist(this, option[pos]);

        // 阶段 2: 遍历本次 .so 注册的所有 IPlugin 调用 Load()
        auto &plugins = ifs_ref_[flowsql::IID_PLUGIN];
        for (size_t i = prev_count; i < plugins.size(); ++i) {
            auto *plugin = reinterpret_cast<flowsql::IPlugin *>(plugins[i]);
            if (-1 == plugin->Load()) {
                printf("'%s' plugin load faild\n", relapath[pos]);
                freelibrary(h);
                return -1;
            }
        }

        plugins_ref_.push_back(h);
    }

    // 注意：不在此处调用 StartAll()（同上）
    return 0;
}

typedef void (*fnunregister)();
inline int PluginLoader::Unload() {
    // 注意：不在此处调用 StopAll()
    // 模块停止由 PluginRegistry::UnloadAll() 在释放锁后单独调用，
    // 避免 Stop() 回调 Unregister() 时与 UnloadAll 持有的锁产生死锁

    // 卸载所有 IPlugin
    this->Traverse(flowsql::IID_PLUGIN, [](void *imod) {
        flowsql::IPlugin *iplugin_ = reinterpret_cast<flowsql::IPlugin *>(imod);
        return iplugin_->Unload();
    });

    for (thandle h : plugins_ref_) {
        fnunregister funregist = (fnunregister)getprocaddress(h, "pluginunregist");
        if (funregist) {
            funregist();
        }
        freelibrary(h);
    }

    // 清理引用，避免悬空指针（问题 4）
    plugins_ref_.clear();
    ifs_ref_.clear();
    started_count_ = 0;  // 重置计数器，避免重新 Load 后跳过启动（问题 13）

    return 0;
}

inline void PluginLoader::Regist(const Guid &iid, void *iface) {
    // insert iface.
    ifs_ref_[iid].push_back(iface);
}

inline int PluginLoader::Traverse(const Guid &iid, fntraverse proc) {
    auto _i = ifs_ref_.find(iid);
    if (_i != ifs_ref_.end()) {
        for (auto &i : _i->second) {
            if (-1 == proc(i)) {
                break;
            }
        }
    }
    return 0;
}

inline void *PluginLoader::First(const Guid &iid) {
    auto _i = ifs_ref_.find(iid);
    if (_i != ifs_ref_.end() && !_i->second.empty()) {
        return _i->second[0];
    }
    return nullptr;
}

inline int PluginLoader::StartAll() {
    auto it = ifs_ref_.find(flowsql::IID_PLUGIN);
    if (it == ifs_ref_.end()) return 0;

    auto &plugins = it->second;
    // 只启动新增的插件（跳过已启动的）
    for (size_t i = started_count_; i < plugins.size(); ++i) {
        auto *plugin = reinterpret_cast<flowsql::IPlugin *>(plugins[i]);
        if (-1 == plugin->Start()) {
            printf("IPlugin::Start() failed at index %zu, rolling back\n", i);
            // 逆序 Stop 本次已成功启动的插件
            for (size_t j = i; j > started_count_; --j) {
                auto *started = reinterpret_cast<flowsql::IPlugin *>(plugins[j - 1]);
                started->Stop();
            }
            return -1;
        }
    }
    started_count_ = plugins.size();
    return 0;
}

inline void PluginLoader::StopAll() {
    auto it = ifs_ref_.find(flowsql::IID_PLUGIN);
    if (it == ifs_ref_.end()) return;

    auto &plugins = it->second;
    // 逆序停止已启动的插件
    for (size_t i = started_count_; i > 0; --i) {
        auto *plugin = reinterpret_cast<flowsql::IPlugin *>(plugins[i - 1]);
        plugin->Stop();
    }
    started_count_ = 0;
}

}  // namespace flowsql

EXPORT_API inline flowsql::IQuerier *getiquerier() {
    flowsql::PluginLoader *_loader = flowsql::PluginLoader::Single();
    return dynamic_cast<flowsql::IQuerier *>(_loader);
}

#define BEGIN_PLUGIN_REGIST(classname)                                                   \
    EXPORT_API void pluginunregist() {}                                                  \
                                                                                         \
    EXPORT_API flowsql::IPlugin *pluginregist(flowsql::IRegister *registry, const char *opt) { \
        static classname _plugin;
// const char *mname = #classname;

#define ____INTERFACE(iid, intername)                           \
    {                                                           \
        intername *iface = dynamic_cast<intername *>(&_plugin); \
        registry->Regist(iid, iface);                           \
    }

#define END_PLUGIN_REGIST() \
    _plugin.Option(opt);    \
    return &_plugin;        \
    }

#endif  // _FLOWSQL_COMMON_LOADER_HPP_
