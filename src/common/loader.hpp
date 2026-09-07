// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_COMMON_LOADER_HPP_
#define _FLOWSQL_COMMON_LOADER_HPP_

#include <algorithm>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "iplugin.h"
#include "toolkit.hpp"

namespace flowsql {

class PluginLoader : public IRegister, public IQuerier {
 public:
    int Load(const char* fullpath[], int count);
    int Load(const char* path, const char* relapath[], const char* option[], int count);
    int Unload();

 public:
    // IRegister
    virtual void Regist(const Guid& iid, void* iface);

    // IQuerier
    virtual int Traverse(const Guid& iid, fntraverse proc);
    virtual void* First(const Guid& iid);

 public:
    static PluginLoader* Single() {
        static PluginLoader _loader;
        return &_loader;
    }

 public:
    // 插件启停
    int StartAll();
    void StopAll();

 private:
    PluginLoader() {}

    std::map<Guid, std::vector<void*>> ifs_ref_;
    std::vector<thandle> plugins_ref_;
    std::vector<IPlugin*> started_plugins_;
};

typedef flowsql::IPlugin* (*fnregister)(flowsql::IRegister*, const char*);
typedef void (*fnunregister)();
inline int PluginLoader::Load(const char* fullpath[], int count) {
    if (count < 0 || (count > 0 && !fullpath)) return -1;

    std::vector<const char*> options(static_cast<size_t>(count), nullptr);
    return Load(get_absolute_process_path(), fullpath, options.data(), count);
}

inline int PluginLoader::Load(const char* path, const char* relapath[], const char* option[], int count) {
    if (count < 0 || (count > 0 && (!path || !relapath))) return -1;

    std::map<Guid, size_t> previous_interface_counts;
    for (const auto& entry : ifs_ref_) previous_interface_counts.emplace(entry.first, entry.second.size());

    std::vector<thandle> pending_handles;
    std::vector<IPlugin*> pending_plugins;

    auto restore_interfaces = [this, &previous_interface_counts]() {
        for (auto it = ifs_ref_.begin(); it != ifs_ref_.end();) {
            auto previous = previous_interface_counts.find(it->first);
            if (previous == previous_interface_counts.end()) {
                it = ifs_ref_.erase(it);
                continue;
            }
            it->second.resize(previous->second);
            ++it;
        }
    };

    auto release_handles = [&pending_handles]() {
        for (auto it = pending_handles.rbegin(); it != pending_handles.rend(); ++it) {
            fnunregister unregister = reinterpret_cast<fnunregister>(getprocaddress(*it, "pluginunregist"));
            if (unregister) unregister();
            freelibrary(*it);
        }
        pending_handles.clear();
    };

    auto rollback = [&pending_plugins, &restore_interfaces, &release_handles](size_t attempted_loads) {
        while (attempted_loads > 0) pending_plugins[--attempted_loads]->Unload();
        restore_interfaces();
        release_handles();
    };

    // Phase 1: load every library and finish every plugin registration/Option before any Load call.
    for (int pos = 0; pos < count; ++pos) {
        if (!relapath[pos]) {
            rollback(0);
            return -1;
        }

        std::string library_path = relapath[pos];
        if (!library_path.empty() && library_path[0] != '/' && library_path[0] != '.') {
            library_path = std::string(path) + "/" + library_path;
        }

        thandle h = loadlibrary(library_path.c_str());
        if (!h) {
            printf("Load shared library '%s' failed, error: '%s'\n", library_path.c_str(), getlasterror());
            rollback(0);
            return -1;
        }
        pending_handles.push_back(h);

        fnregister register_plugin = reinterpret_cast<fnregister>(getprocaddress(h, "pluginregist"));
        if (!register_plugin) {
            printf("'%s' getprocaddress of '%s' failed\n", library_path.c_str(), "pluginregist");
            rollback(0);
            return -1;
        }

        const size_t previous_plugin_count = ifs_ref_[flowsql::IID_PLUGIN].size();
        IPlugin* registered = register_plugin(this, option ? option[pos] : nullptr);
        auto& plugins = ifs_ref_[flowsql::IID_PLUGIN];
        if (!registered || plugins.size() == previous_plugin_count) {
            printf("'%s' plugin registration/Option failed\n", library_path.c_str());
            rollback(0);
            return -1;
        }
        for (size_t i = previous_plugin_count; i < plugins.size(); ++i) {
            pending_plugins.push_back(reinterpret_cast<IPlugin*>(plugins[i]));
        }
    }

    // Phase 2: only a fully registered/configured batch may enter Load.
    for (size_t i = 0; i < pending_plugins.size(); ++i) {
        if (pending_plugins[i]->Load(this) != 0) {
            printf("IPlugin::Load() failed at batch index %zu\n", i);
            rollback(i + 1);
            return -1;
        }
    }

    plugins_ref_.insert(plugins_ref_.end(), pending_handles.begin(), pending_handles.end());
    pending_handles.clear();
    return 0;
}

inline int PluginLoader::Unload() {
    this->Traverse(flowsql::IID_PLUGIN, [](void* imod) {
        flowsql::IPlugin* iplugin_ = reinterpret_cast<flowsql::IPlugin*>(imod);
        return iplugin_->Unload();
    });

    for (thandle h : plugins_ref_) {
        fnunregister funregist = (fnunregister)getprocaddress(h, "pluginunregist");
        if (funregist) {
            funregist();
        }
        freelibrary(h);
    }

    plugins_ref_.clear();
    ifs_ref_.clear();
    started_plugins_.clear();

    return 0;
}

inline void PluginLoader::Regist(const Guid& iid, void* iface) {
    ifs_ref_[iid].push_back(iface);
}

inline int PluginLoader::Traverse(const Guid& iid, fntraverse proc) {
    auto _i = ifs_ref_.find(iid);
    if (_i != ifs_ref_.end()) {
        for (auto& i : _i->second) {
            if (-1 == proc(i)) {
                break;
            }
        }
    }
    return 0;
}

inline void* PluginLoader::First(const Guid& iid) {
    auto _i = ifs_ref_.find(iid);
    if (_i != ifs_ref_.end() && !_i->second.empty()) {
        return _i->second[0];
    }
    return nullptr;
}

inline int PluginLoader::StartAll() {
    auto it = ifs_ref_.find(flowsql::IID_PLUGIN);
    if (it == ifs_ref_.end()) return 0;

    auto& plugins = it->second;
    std::vector<void*> external_entries;
    auto external_it = ifs_ref_.find(flowsql::IID_PLUGIN_EXTERNAL_ENTRY);
    if (external_it != ifs_ref_.end()) external_entries = external_it->second;

    const size_t previous_started_count = started_plugins_.size();
    auto rollback = [this, previous_started_count]() {
        while (started_plugins_.size() > previous_started_count) {
            started_plugins_.back()->Stop();
            started_plugins_.pop_back();
        }
    };

    auto start_phase = [&](bool external_entry) {
        for (size_t i = 0; i < plugins.size(); ++i) {
            auto* plugin = reinterpret_cast<IPlugin*>(plugins[i]);
            const bool marked_external =
                std::find(external_entries.begin(), external_entries.end(), plugin) != external_entries.end();
            if (marked_external != external_entry ||
                std::find(started_plugins_.begin(), started_plugins_.end(), plugin) != started_plugins_.end()) {
                continue;
            }
            if (plugin->Start() != 0) {
                printf("IPlugin::Start() failed at index %zu, rolling back\n", i);
                rollback();
                return -1;
            }
            started_plugins_.push_back(plugin);
        }
        return 0;
    };

    if (start_phase(false) != 0) return -1;
    return start_phase(true);
}

inline void PluginLoader::StopAll() {
    while (!started_plugins_.empty()) {
        started_plugins_.back()->Stop();
        started_plugins_.pop_back();
    }
}

}  // namespace flowsql

#endif  // _FLOWSQL_COMMON_LOADER_HPP_
