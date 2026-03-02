#ifndef _FLOWSQL_SCHEDULER_SCHEDULER_PLUGIN_H_
#define _FLOWSQL_SCHEDULER_SCHEDULER_PLUGIN_H_

#include <httplib.h>

#include <string>
#include <thread>

#include <common/loader.hpp>

namespace flowsql {

class PluginRegistry;

namespace scheduler {

// SchedulerPlugin — SQL 执行调度插件
// 提供 HTTP 端点接收 SQL 执行请求，在本进程内完成 Pipeline 执行
// Gateway 架构下：算子（含 Python 桥接）注册在 Scheduler 进程，Web 进程通过 Gateway 转发到此
class SchedulerPlugin : public IPlugin {
 public:
    SchedulerPlugin() = default;
    ~SchedulerPlugin() override = default;

    // IPlugin
    int Option(const char* arg) override;
    int Load() override;
    int Unload() override;
    int Start() override;
    int Stop() override;

 private:
    // 注册 HTTP 路由
    void RegisterRoutes();

    // HTTP 端点处理
    void HandleExecute(const httplib::Request& req, httplib::Response& res);
    void HandleGetChannels(const httplib::Request& req, httplib::Response& res);
    void HandleGetOperators(const httplib::Request& req, httplib::Response& res);

    PluginRegistry* registry_ = nullptr;
    httplib::Server server_;
    std::thread server_thread_;

    std::string host_ = "127.0.0.1";
    int port_ = 18803;
};

}  // namespace scheduler
}  // namespace flowsql

#endif  // _FLOWSQL_SCHEDULER_SCHEDULER_PLUGIN_H_
