#ifndef _FLOWSQL_SERVICES_BINADDON_BINADDON_HOST_PLUGIN_H_
#define _FLOWSQL_SERVICES_BINADDON_BINADDON_HOST_PLUGIN_H_

#include <common/iplugin.h>
#include <framework/interfaces/ibinaddon_host.h>
#include <framework/interfaces/ioperator.h>
#include <framework/interfaces/ioperator_catalog.h>
#include <framework/interfaces/ioperator_registry.h>

#include <sqlite3.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace flowsql {
namespace binaddon {

class __attribute__((visibility("default"))) BinAddonHostPlugin : public IPlugin, public IBinAddonHost {
 public:
    struct PluginStoreRow {
        std::string plugin_id;
        std::string so_file;
        std::string file_path;
        int64_t size_bytes = 0;
        std::string sha256;
        std::string status;
        std::string last_error;
        int abi_version = -1;
        int operator_count = -1;
        std::string operators_json;
    };

    struct LoadedPlugin {
        void* handle = nullptr;
        std::string plugin_id;
        std::string file_path;
        std::string so_file;
        std::string sha256;
        int abi_version = 0;
        int64_t size_bytes = 0;
        std::atomic<int> active_count{0};
        std::atomic<bool> pending_unload{false};
        int (*count_fn)() = nullptr;
        IOperator* (*create_fn)(int) = nullptr;
        void (*destroy_fn)(IOperator*) = nullptr;
        std::vector<std::string> operator_keys;
        std::vector<std::string> operator_names;
        ~LoadedPlugin();
    };

    BinAddonHostPlugin() = default;
    ~BinAddonHostPlugin() override = default;

    // IPlugin
    int Option(const char* arg) override;
    int Load(IQuerier* querier) override;
    int Unload() override;
    int Start() override;
    int Stop() override;

    // IBinAddonHost
    int ListCppPlugins(std::string& rsp) override;
    int UploadCppPlugin(const std::string& filename, const std::string& tmp_path, std::string& rsp) override;
    int ActivateCppPlugin(const std::string& plugin_id, std::string& rsp) override;
    int DeactivateCppPlugin(const std::string& plugin_id, std::string& rsp) override;
    int DeleteCppPlugin(const std::string& plugin_id, std::string& rsp) override;
    int GetCppPluginDetail(const std::string& plugin_id, std::string& rsp) override;

 private:
    int EnsureOperatorDbDir() const;
    int EnsureOperatorDbLocked();
    int EnsureSchemaLocked();
    std::string OperatorDbPath() const;
    bool QueryPluginByIdLocked(const std::string& plugin_id, PluginStoreRow* row);
    int UpdatePluginStatusLocked(const std::string& plugin_id,
                                 const std::string& status,
                                 const std::string& last_error,
                                 int abi_version,
                                 int operator_count,
                                 const std::string& operators_json);
    int UpsertCppOperatorsLocked(const std::string& plugin_id, const std::vector<OperatorMeta>& operators);
    int SetCppOperatorsActiveByPluginLocked(const std::string& plugin_id, int active);
    int DeleteCppOperatorsByPluginLocked(const std::string& plugin_id);
    int RecoverActivatedPlugins();

 private:
    IQuerier* querier_ = nullptr;
    IOperatorRegistry* registry_ = nullptr;
    std::unordered_map<std::string, std::shared_ptr<LoadedPlugin>> loaded_plugins_;
    std::string operator_db_dir_ = "./catalog";
    std::string operator_db_path_;
    std::string upload_dir_ = "./uploads/binaddon";
    sqlite3* operator_db_ = nullptr;
    mutable std::mutex mu_;
};

}  // namespace binaddon
}  // namespace flowsql

#endif  // _FLOWSQL_SERVICES_BINADDON_BINADDON_HOST_PLUGIN_H_
