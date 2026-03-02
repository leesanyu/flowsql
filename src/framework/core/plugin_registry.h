#ifndef _FLOWSQL_FRAMEWORK_CORE_PLUGIN_REGISTRY_H_
#define _FLOWSQL_FRAMEWORK_CORE_PLUGIN_REGISTRY_H_

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>

#include <common/loader.hpp>

namespace flowsql {

class PluginLoader;

class PluginRegistry {
 public:
    static PluginRegistry* Instance();

    // 静态插件加载（委托 PluginLoader）
    int LoadPlugin(const std::string& path);
    int LoadPlugin(const std::string& path, const char* option);
    void UnloadAll();

    // 动态注册/注销 — 通用接口，按 IID + key 管理
    void Register(const Guid& iid, const std::string& key, std::shared_ptr<void> instance);
    void Unregister(const Guid& iid, const std::string& key);

    // 插件启停 — 委托 PluginLoader，不持有 mutex_（避免 Start 回调 Register 死锁）
    int StartAll();
    void StopAll();

    // 统一查询 — 合并静态 + 动态，动态优先
    void* Get(const Guid& iid, const std::string& key);

    // 统一遍历 — 合并静态 + 动态，直接遍历自己的 map
    void Traverse(const Guid& iid, std::function<int(void*)> callback);

    // 类型安全的便捷模板
    template <typename T>
    T* Get(const Guid& iid, const std::string& key) {
        return static_cast<T*>(Get(iid, key));
    }

    template <typename T>
    void Traverse(const Guid& iid, std::function<void(T*)> callback) {
        Traverse(iid, [&callback](void* p) -> int {
            callback(static_cast<T*>(p));
            return 0;
        });
    }

 private:
    PluginRegistry();
    ~PluginRegistry();
    PluginRegistry(const PluginRegistry&) = delete;
    PluginRegistry& operator=(const PluginRegistry&) = delete;

    void BuildIndex();

    PluginLoader* loader_;
    mutable std::shared_mutex mutex_;

    // 静态索引（LoadPlugin 成功后通过 BuildIndex 构建）
    std::map<Guid, std::unordered_map<std::string, void*>> static_index_;
    // 动态索引（shared_ptr<void> 管理生命周期）
    std::map<Guid, std::unordered_map<std::string, std::shared_ptr<void>>> dynamic_index_;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_CORE_PLUGIN_REGISTRY_H_
