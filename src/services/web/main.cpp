// FlowSQL 通用入口
// Gateway 模式: flowsql --config gateway.yaml
// Service 模式: flowsql --role web --gateway 127.0.0.1:18800 --port 18802 --plugins libflowsql_web.so

#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "framework/core/plugin_registry.h"
#include "services/gateway/service_client.h"

using namespace flowsql;

// 解析命令行参数
struct Args {
    std::string config;       // Gateway 模式：配置文件路径
    std::string role;         // Service 模式：角色名
    std::string gateway_addr; // Service 模式：Gateway 地址
    int port = 0;             // Service 模式：监听端口
    std::string plugins;      // Service 模式：插件列表（逗号分隔）
    std::string option;       // 传给插件的 Option 参数
};

static Args ParseArgs(int argc, char* argv[]) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            args.config = argv[++i];
        } else if (strcmp(argv[i], "--role") == 0 && i + 1 < argc) {
            args.role = argv[++i];
        } else if (strcmp(argv[i], "--gateway") == 0 && i + 1 < argc) {
            args.gateway_addr = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            args.port = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "--plugins") == 0 && i + 1 < argc) {
            args.plugins = argv[++i];
        } else if (strcmp(argv[i], "--option") == 0 && i + 1 < argc) {
            args.option = argv[++i];
        }
    }
    return args;
}

static std::vector<std::string> Split(const std::string& s, char delim) {
    std::vector<std::string> result;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t next = s.find(delim, pos);
        if (next == std::string::npos) next = s.size();
        if (next > pos) result.push_back(s.substr(pos, next - pos));
        pos = next + 1;
    }
    return result;
}

// 阻塞等待 SIGINT/SIGTERM
static void WaitForSignal() {
    sigset_t waitset;
    sigemptyset(&waitset);
    sigaddset(&waitset, SIGINT);
    sigaddset(&waitset, SIGTERM);
    sigprocmask(SIG_BLOCK, &waitset, nullptr);
    int sig;
    sigwait(&waitset, &sig);
}

// --- Gateway 模式 ---
static int RunGateway(const std::string& config_path) {
    printf("========================================\n");
    printf("  FlowSQL Gateway\n");
    printf("========================================\n\n");

    auto* registry = PluginRegistry::Instance();

    if (registry->LoadPlugin("libflowsql_gateway.so", config_path.c_str()) != 0) {
        printf("Failed to load gateway plugin\n");
        return 1;
    }

    printf("Gateway started. Press Ctrl+C to stop.\n");
    WaitForSignal();

    printf("\nShutting down gateway...\n");
    registry->UnloadAll();
    return 0;
}

// --- Service 模式 ---
static int RunService(const Args& args) {
    printf("========================================\n");
    printf("  FlowSQL Service: %s\n", args.role.c_str());
    printf("========================================\n\n");

    auto* registry = PluginRegistry::Instance();

    // 加载指定的插件（自动将 port 注入 option）
    auto plugin_list = Split(args.plugins, ',');
    std::string auto_option;
    if (args.port > 0) {
        auto_option = "port=" + std::to_string(args.port);
        if (!args.option.empty()) auto_option += ";" + args.option;
    } else {
        auto_option = args.option;
    }
    for (const auto& plugin : plugin_list) {
        const char* opt = auto_option.empty() ? nullptr : auto_option.c_str();
        if (registry->LoadPlugin(plugin, opt) != 0) {
            printf("Failed to load plugin: %s\n", plugin.c_str());
        } else {
            printf("Loaded plugin: %s\n", plugin.c_str());
        }
    }

    // 向 Gateway 注册路由并启动心跳
    gateway::ServiceClient client;

    if (!args.gateway_addr.empty()) {
        std::string gw_host = "127.0.0.1";
        int gw_port = 18800;
        size_t colon = args.gateway_addr.find(':');
        if (colon != std::string::npos) {
            gw_host = args.gateway_addr.substr(0, colon);
            gw_port = std::stoi(args.gateway_addr.substr(colon + 1));
        }
        client.SetGateway(gw_host, gw_port);

        // 注册路由前缀：/<role>
        std::string local_addr = "127.0.0.1:" + std::to_string(args.port);
        std::string prefix = "/" + args.role;
        client.RegisterRoute(prefix, local_addr);

        client.StartHeartbeat(args.role, 5);
    }

    printf("Service %s running on port %d. Press Ctrl+C to stop.\n", args.role.c_str(), args.port);
    WaitForSignal();

    printf("\nShutting down service %s...\n", args.role.c_str());
    client.StopHeartbeat();
    registry->UnloadAll();
    return 0;
}

int main(int argc, char* argv[]) {
    Args args = ParseArgs(argc, argv);

    if (!args.config.empty()) {
        return RunGateway(args.config);
    } else if (!args.role.empty()) {
        return RunService(args);
    } else {
        printf("Usage:\n");
        printf("  Gateway mode: %s --config gateway.yaml\n", argv[0]);
        printf("  Service mode: %s --role <name> --gateway <host:port> --port <port> --plugins <plugin1,plugin2>\n",
               argv[0]);
        return 1;
    }
}
