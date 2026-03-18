#ifndef _FLOWSQL_SERVICES_DATABASE_DATABASE_PLUGIN_H_
#define _FLOWSQL_SERVICES_DATABASE_DATABASE_PLUGIN_H_

#include <common/error_code.h>
#include <common/iplugin.h>
#include <framework/interfaces/idatabase_factory.h>
#include <framework/interfaces/irouter_handle.h>

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "database_channel.h"

namespace flowsql {
namespace database {

// DatabasePlugin — 数据库通道工厂插件
// 同时实现 IPlugin（生命周期）、IDatabaseFactory（通道工厂）、IRouterHandle（路由声明）
class __attribute__((visibility("default"))) DatabasePlugin
    : public IPlugin, public IDatabaseFactory, public IRouterHandle {
 public:
    DatabasePlugin() = default;
    ~DatabasePlugin() override = default;

    // IPlugin
    int Option(const char* arg) override;
    int Load(IQuerier* querier) override;
    int Unload() override;
    int Start() override;
    int Stop() override;

    // IDatabaseFactory
    IDatabaseChannel* Get(const char* type, const char* name) override;
    void List(std::function<void(const char* type, const char* name,
                                  const char* config_json)> callback) override;
    int Release(const char* type, const char* name) override;
    const char* LastError() override;

    // IDatabaseFactory 动态管理（Epic 6）
    int AddChannel(const char* config_str) override;
    int RemoveChannel(const char* type, const char* name) override;
    int UpdateChannel(const char* config_str) override;

    // IRouterHandle — 声明 /channels/database/* 路由
    void EnumRoutes(std::function<void(const RouteItem&)> callback) override;

 private:
    // 路由处理（fnRouterHandler 签名）
    int32_t HandleAdd(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleRemove(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleModify(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleQuery(const std::string& uri, const std::string& req, std::string& rsp);
    // 浏览器端点
    int32_t HandleTables(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandleDescribe(const std::string& uri, const std::string& req, std::string& rsp);
    int32_t HandlePreview(const std::string& uri, const std::string& req, std::string& rsp);

    // 内部辅助
    std::unique_ptr<IDbDriver> CreateDriver(const std::string& type);
    int ParseSingleConfig(const char* arg);
    int LoadFromYaml();
    int SaveToYaml();
    std::string EncryptPassword(const std::string& plain);
    std::string DecryptPassword(const std::string& cipher);

    // 通道池：key = "type.name"
    std::unordered_map<std::string, std::shared_ptr<DatabaseChannel>> channels_;
    // 驱动存储：key = "type.name"
    std::unordered_map<std::string, std::unique_ptr<IDbDriver>> driver_storage_;
    // 配置表：key = "type.name"
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> configs_;

    std::mutex mutex_;
    std::mutex save_mutex_;
    IQuerier* querier_ = nullptr;
    std::string config_file_;

    static thread_local std::string last_error_;
};

}  // namespace database
}  // namespace flowsql

#endif  // _FLOWSQL_SERVICES_DATABASE_DATABASE_PLUGIN_H_
