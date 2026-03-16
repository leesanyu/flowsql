#ifndef _FLOWSQL_ROUTER_ROUTER_AGENCY_PLUGIN_H_
#define _FLOWSQL_ROUTER_ROUTER_AGENCY_PLUGIN_H_

#include <atomic>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>

#include <httplib.h>

#include <common/error_code.h>
#include <common/iplugin.h>
#include <framework/interfaces/irouter_handle.h>

namespace flowsql {
namespace router {

// RouterAgencyPlugin — 进程内路由代理
//
// 职责：
//   1. Start() 时通过 Traverse(IID_ROUTER_HANDLE) 收集所有业务插件声明的路由
//   2. 启动 httplib::Server，catch-all 分发到对应 fnRouterHandler
//   3. 向 Gateway 注册路由前缀，并通过 KeepAlive 线程定期续期
//
// 业务插件只需实现 IRouterHandle::EnumRoutes()，对 HTTP 完全无感知
class RouterAgencyPlugin : public IPlugin {
 public:
    RouterAgencyPlugin() = default;
    ~RouterAgencyPlugin() override = default;

    // IPlugin
    int Option(const char* arg) override;
    int Load(IQuerier* querier) override;
    int Unload() override;
    int Start() override;
    int Stop() override;

    // 路由收集（独立方法，便于单元测试传入 mock IQuerier）
    int CollectRoutes(IQuerier* querier);

 private:
    // HTTP 服务线程
    void HttpThread();

    // 请求分发（catch-all handler）
    void Dispatch(const httplib::Request& req, httplib::Response& res);

    // 向 Gateway 注册所有路由前缀（幂等）
    void RegisterOnce();

    // 向 Gateway 注销所有路由前缀（优雅关闭时调用）
    void UnregisterOnce();

    // KeepAlive 线程：定期向 Gateway 重注册路由前缀，兼做故障恢复
    void KeepAliveThread();

    // 业务错误码 → HTTP 状态码
    static int HttpStatus(int32_t rc);

    // 配置参数
    std::string host_ = "127.0.0.1";
    int port_ = 18803;
    std::string gateway_host_;
    int gateway_port_ = 18800;
    int keepalive_interval_s_ = 10;

    // 路由表：key = "METHOD:URI"，value = handler
    std::unordered_map<std::string, fnRouterHandler> route_table_;

    // 从路由表提取的一级前缀集合（用于 Gateway 注册）
    std::set<std::string> prefixes_;

    IQuerier* querier_ = nullptr;
    httplib::Server server_;
    std::thread http_thread_;
    std::thread keepalive_thread_;
    std::atomic<bool> running_{false};
};

}  // namespace router
}  // namespace flowsql

#endif  // _FLOWSQL_ROUTER_ROUTER_AGENCY_PLUGIN_H_
