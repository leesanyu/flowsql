#ifndef _FLOWSQL_GATEWAY_GATEWAY_PLUGIN_H_
#define _FLOWSQL_GATEWAY_GATEWAY_PLUGIN_H_

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include <httplib.h>

#include <common/loader.hpp>

#include "config.h"
#include "route_table.h"
#include "service_manager.h"

namespace flowsql {
namespace gateway {

// Gateway 核心插件 — 路由转发 + 子服务管理 + 心跳检测
class GatewayPlugin : public IPlugin {
 public:
    GatewayPlugin() = default;
    ~GatewayPlugin() override = default;

    // IPlugin
    int Option(const char* arg) override;
    int Load() override;
    int Unload() override;
    int Start() override;
    int Stop() override;

 private:
    // HTTP 服务线程
    void HttpThread();

    // 心跳检测线程
    void HeartbeatThread();

    // 路由转发
    void HandleForward(const httplib::Request& req, httplib::Response& res);

    // Gateway API 处理
    void HandleRegister(const httplib::Request& req, httplib::Response& res);
    void HandleUnregister(const httplib::Request& req, httplib::Response& res);
    void HandleRoutes(const httplib::Request& req, httplib::Response& res);
    void HandleHeartbeat(const httplib::Request& req, httplib::Response& res);

    GatewayConfig config_;
    RouteTable route_table_;
    ServiceManager service_manager_;
    httplib::Server server_;

    std::thread http_thread_;
    std::thread heartbeat_thread_;
    std::atomic<bool> running_{false};
};

}  // namespace gateway
}  // namespace flowsql

#endif  // _FLOWSQL_GATEWAY_GATEWAY_PLUGIN_H_
