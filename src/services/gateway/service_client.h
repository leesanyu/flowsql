#ifndef _FLOWSQL_GATEWAY_SERVICE_CLIENT_H_
#define _FLOWSQL_GATEWAY_SERVICE_CLIENT_H_

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace flowsql {
namespace gateway {

// 服务注册客户端 — 各子服务进程内使用，向 Gateway 注册路由和发送心跳
class ServiceClient {
 public:
    ServiceClient() = default;
    ~ServiceClient();

    // 设置 Gateway 地址
    void SetGateway(const std::string& host, int port);

    // 注册单个路由前缀
    int RegisterRoute(const std::string& prefix, const std::string& local_address);

    // 批量注册路由
    int RegisterRoutes(const std::vector<std::string>& prefixes, const std::string& local_address);

    // 注销路由
    int UnregisterRoute(const std::string& prefix);

    // 启动心跳线程
    void StartHeartbeat(const std::string& service_name, int interval_s);

    // 停止心跳线程
    void StopHeartbeat();

    // 查询路由表（服务发现）
    std::string QueryRoutes();

 private:
    std::string gateway_host_ = "127.0.0.1";
    int gateway_port_ = 18800;
    std::string service_name_;
    std::thread heartbeat_thread_;
    std::atomic<bool> heartbeat_running_{false};
};

}  // namespace gateway
}  // namespace flowsql

#endif  // _FLOWSQL_GATEWAY_SERVICE_CLIENT_H_
