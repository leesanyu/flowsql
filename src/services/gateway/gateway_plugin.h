#ifndef _FLOWSQL_GATEWAY_GATEWAY_PLUGIN_H_
#define _FLOWSQL_GATEWAY_GATEWAY_PLUGIN_H_

#include <atomic>
#include <string>
#include <thread>

#include <httplib.h>

#include <common/iplugin.h>

#include "config.h"
#include "route_table.h"

namespace flowsql {
namespace gateway {

// GatewayPlugin — 路由转发 + 路由注册管理 + 过期清理
// 删除了 ServiceManager（子进程管理）和 HeartbeatThread（心跳检测）
// 服务自治：各服务通过 RouterAgencyPlugin 的 KeepAlive 线程定期重注册路由
class GatewayPlugin : public IPlugin {
 public:
    GatewayPlugin() = default;
    ~GatewayPlugin() override = default;

    // IPlugin
    int Option(const char* arg) override;
    int Load(IQuerier* querier) override;
    int Unload() override;
    int Start() override;
    int Stop() override;

 private:
    // HTTP 服务线程
    void HttpThread();

    // 过期路由清理线程（定期移除超时未续期的路由）
    void CleanupThread();

    // 路由转发（转发完整 URI，不剥离前缀）
    void HandleForward(const httplib::Request& req, httplib::Response& res);

    // Gateway 管理 API
    void HandleRegister(const httplib::Request& req, httplib::Response& res);
    void HandleUnregister(const httplib::Request& req, httplib::Response& res);
    void HandleRoutes(const httplib::Request& req, httplib::Response& res);

    GatewayConfig config_;
    RouteTable route_table_;
    httplib::Server server_;

    std::thread http_thread_;
    std::thread cleanup_thread_;
    std::atomic<bool> running_{false};

    // 路由过期时间（秒），默认 keepalive_interval_s * 3
    int route_ttl_s_ = 30;
};

}  // namespace gateway
}  // namespace flowsql

#endif  // _FLOWSQL_GATEWAY_GATEWAY_PLUGIN_H_
