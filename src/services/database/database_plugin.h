#ifndef _FLOWSQL_SERVICES_DATABASE_DATABASE_PLUGIN_H_
#define _FLOWSQL_SERVICES_DATABASE_DATABASE_PLUGIN_H_

#include <common/iplugin.h>
#include <framework/interfaces/idatabase_factory.h>

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "database_channel.h"

namespace flowsql {
namespace database {

// DatabasePlugin — 数据库通道工厂插件
// 同时实现 IPlugin（生命周期）和 IDatabaseFactory（通道工厂）
class DatabasePlugin : public IPlugin, public IDatabaseFactory {
 public:
    DatabasePlugin() = default;
    ~DatabasePlugin() override = default;

    // IPlugin
    int Option(const char* arg) override;
    int Load(IQuerier* querier) override;
    int Unload() override;
    int Start() override { return 0; }
    int Stop() override;

    // IDatabaseFactory
    IDatabaseChannel* Get(const char* type, const char* name) override;
    void List(std::function<void(const char* type, const char* name)> callback) override;
    int Release(const char* type, const char* name) override;
    const char* LastError() override;

 private:
    // 创建指定类型的数据库驱动
    std::unique_ptr<IDbDriver> CreateDriver(const std::string& type);

    // 解析单个数据库配置
    int ParseSingleConfig(const char* arg);

    // 通道池：key = "type.name"
    std::unordered_map<std::string, std::shared_ptr<DatabaseChannel>> channels_;

    // 配置表：key = "type.name", value = 连接参数
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> configs_;

    std::mutex mutex_;
    IQuerier* querier_ = nullptr;

    // 线程安全的错误信息存储
    static thread_local std::string last_error_;
};

}  // namespace database
}  // namespace flowsql

#endif  // _FLOWSQL_SERVICES_DATABASE_DATABASE_PLUGIN_H_
