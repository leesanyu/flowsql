#ifndef _FLOWSQL_GATEWAY_CONFIG_H_
#define _FLOWSQL_GATEWAY_CONFIG_H_

#include <string>
#include <unordered_map>
#include <vector>
#include <chrono>

namespace flowsql {
namespace gateway {

// 连接池配置
struct ConnectionPoolConfig {
    int max_connections = 10;                    // 最大连接数
    int min_connections = 0;                     // 最小连接数
    int idle_timeout_seconds = 300;              // 空闲超时（秒）
    int health_check_interval_seconds = 60;      // 健康检查间隔（秒）
};

// 数据库配置
struct DatabaseConfig {
    std::string type;                                      // 数据库类型（sqlite/mysql/postgresql）
    std::string name;                                      // 实例名称
    std::unordered_map<std::string, std::string> params;  // 其他参数（path/host/port/user/password 等）
    ConnectionPoolConfig pool;                             // 连接池配置
};

// 子服务配置
struct ServiceConfig {
    std::string name;            // "web" | "scheduler" | "pyworker"
    std::string type = "cpp";    // "cpp" | "python"
    std::string command;         // Python 服务启动命令
    std::vector<std::string> plugins;
    std::vector<DatabaseConfig> databases;  // 数据库配置列表
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
    std::string mode = "guardian";   // "guardian"（多进程）| "single"（单进程）
    std::vector<ServiceConfig> services;
};

// 从 YAML 文件加载配置
int LoadConfig(const std::string& path, GatewayConfig* config);

}  // namespace gateway
}  // namespace flowsql

#endif  // _FLOWSQL_GATEWAY_CONFIG_H_
