#ifndef _FLOWSQL_GATEWAY_SERVICE_MANAGER_H_
#define _FLOWSQL_GATEWAY_SERVICE_MANAGER_H_

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "config.h"

namespace flowsql {
namespace gateway {

// 子服务运行时信息
struct ServiceInfo {
    ServiceConfig config;
    pid_t pid = -1;
    std::string address;          // "host:port"
    int64_t last_heartbeat_ms = 0;
    bool alive = false;
};

// 子进程管理器 — 负责 spawn/stop/restart 所有子服务
class ServiceManager {
 public:
    ServiceManager() = default;
    ~ServiceManager();

    // 启动所有子服务
    int StartAll(const GatewayConfig& config, const std::string& gateway_addr);

    // 停止所有子服务
    void StopAll();

    // 停止指定服务
    void StopService(const std::string& name);

    // 重启指定服务
    int RestartService(const std::string& name, const std::string& gateway_addr);

    // 更新心跳时间戳
    void UpdateHeartbeat(const std::string& name);

    // 检查超时并重启（由心跳检测线程调用）
    void CheckAndRestart(int timeout_s, const std::string& gateway_addr);

    // 获取服务信息
    const ServiceInfo* GetService(const std::string& name);

 private:
    // 启动单个服务
    int SpawnService(ServiceInfo& info, const std::string& gateway_addr);

    // 停止单个进程
    void KillProcess(pid_t pid, const std::string& name);

    std::mutex mutex_;
    std::unordered_map<std::string, ServiceInfo> services_;
    GatewayConfig config_;
};

}  // namespace gateway
}  // namespace flowsql

#endif  // _FLOWSQL_GATEWAY_SERVICE_MANAGER_H_
