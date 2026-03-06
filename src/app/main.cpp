// FlowSQL 通用入口
// Gateway 模式: flowsql --config gateway.yaml
// Service 模式: flowsql --role web --gateway 127.0.0.1:18800 --port 18802 --plugins libflowsql_web.so

#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <common/loader.hpp>
#include "services/gateway/service_client.h"

using namespace flowsql;

// 解析命令行参数
struct Args {
    std::string config;       // Gateway 模式：配置文件路径
    std::string role;         // Service 模式：角色名
    std::string gateway_addr; // Service 模式：Gateway 地址
    int port = 0;             // Service 模式：监听端口
    std::string plugins;      // Service 模式：插件列表（逗号分隔）
    std::string databases;    // Service 模式：数据库配置列表（| 分隔）
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
        } else if (strcmp(argv[i], "--databases") == 0 && i + 1 < argc) {
            args.databases = argv[++i];
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

// 加载单个插件（封装 PluginLoader 调用）
static int LoadPlugin(PluginLoader* loader, const std::string& path, const char* option) {
    std::string app_path = get_absolute_process_path();

    // 支持 "libxxx.so:key=val;..." 格式：冒号前为文件名，冒号后为插件 option
    std::string lib_path = path;
    std::string plugin_option;
    auto colon = path.find(':');
    if (colon != std::string::npos) {
        lib_path = path.substr(0, colon);
        plugin_option = path.substr(colon + 1);
    }
    // 命令行 option 与内嵌 option 合并（内嵌优先）
    if (plugin_option.empty() && option) plugin_option = option;

    const char* relapath[] = {lib_path.c_str()};
    const char* opt_ptr = plugin_option.empty() ? nullptr : plugin_option.c_str();
    const char* options[] = {opt_ptr};
    int ret = loader->Load(app_path.c_str(), relapath, options, 1);
    if (ret != 0) return ret;
    return loader->StartAll();
}

// --- Gateway 模式 ---
static int RunGateway(const std::string& config_path) {
    printf("========================================\n");
    printf("  FlowSQL Gateway\n");
    printf("========================================\n\n");

    auto* loader = PluginLoader::Single();

    if (LoadPlugin(loader, "libflowsql_gateway.so", config_path.c_str()) != 0) {
        printf("Failed to load gateway plugin\n");
        return 1;
    }

    printf("Gateway started. Press Ctrl+C to stop.\n");
    WaitForSignal();

    printf("\nShutting down gateway...\n");
    loader->StopAll();
    loader->Unload();
    return 0;
}

// --- Service 模式 ---
static int RunService(const Args& args) {
    printf("========================================\n");
    printf("  FlowSQL Service: %s\n", args.role.c_str());
    printf("========================================\n\n");

    auto* loader = PluginLoader::Single();

    // 加载指定的插件（自动将 port 注入 option）
    auto plugin_list = Split(args.plugins, ',');
    std::string auto_option;
    if (args.port > 0) {
        auto_option = "port=" + std::to_string(args.port);
        if (!args.option.empty()) auto_option += ";" + args.option;
    } else {
        auto_option = args.option;
    }

    // 处理数据库配置（新增）
    std::vector<std::string> database_configs;
    if (!args.databases.empty()) {
        database_configs = Split(args.databases, '|');
    }

    for (const auto& plugin : plugin_list) {
        // 特殊处理 database 插件
        if (plugin.find("libflowsql_database.so") != std::string::npos && !database_configs.empty()) {
            // 只加载一次插件，但为每个数据库配置调用一次 Option
            // 将所有配置合并为一个字符串，用 | 分隔
            std::string merged_config;
            for (size_t i = 0; i < database_configs.size(); ++i) {
                if (i > 0) merged_config += "|";
                merged_config += database_configs[i];
            }
            if (LoadPlugin(loader, "libflowsql_database.so:" + merged_config, nullptr) != 0) {
                printf("Failed to load database plugin\n");
            } else {
                printf("Loaded database plugin with %zu database(s)\n", database_configs.size());
            }
        } else {
            // 普通插件或旧格式
            const char* opt = auto_option.empty() ? nullptr : auto_option.c_str();
            if (LoadPlugin(loader, plugin, opt) != 0) {
                printf("Failed to load plugin: %s\n", plugin.c_str());
            } else {
                printf("Loaded plugin: %s\n", plugin.c_str());
            }
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

        std::string local_addr = "127.0.0.1:" + std::to_string(args.port);
        std::string prefix = "/" + args.role;
        client.RegisterRoute(prefix, local_addr);

        client.StartHeartbeat(args.role, 5);
    }

    printf("Service %s running on port %d. Press Ctrl+C to stop.\n", args.role.c_str(), args.port);
    WaitForSignal();

    printf("\nShutting down service %s...\n", args.role.c_str());
    client.StopHeartbeat();
    loader->StopAll();
    loader->Unload();
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
