#ifndef _FLOWSQL_GATEWAY_CONFIG_H_
#define _FLOWSQL_GATEWAY_CONFIG_H_

#include <string>
#include <vector>

namespace flowsql {
namespace gateway {

// 子服务配置
struct ServiceConfig {
    std::string name;            // "web" | "scheduler" | "pyworker"
    std::string type = "cpp";    // "cpp" | "python"
    std::string command;         // Python 服务启动命令
    std::vector<std::string> plugins;
    std::string host = "127.0.0.1";
    int port = 0;
    std::string option;          // 传给插件的 Option 参数
};

// Gateway 全局配置
struct GatewayConfig {
    std::string host = "127.0.0.1";
    int port = 18800;
    int heartbeat_interval_s = 5;
    int heartbeat_timeout_count = 3;
    std::vector<ServiceConfig> services;
};

// 从 YAML 文件加载配置
int LoadConfig(const std::string& path, GatewayConfig* config);

}  // namespace gateway
}  // namespace flowsql

#endif  // _FLOWSQL_GATEWAY_CONFIG_H_
