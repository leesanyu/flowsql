// FlowSQL 通用入口
// Guardian 模式: flowsql --config gateway.yaml          （fork 守护，拉起所有服务）
// Single   模式: flowsql --config single.yaml           （单进程，所有插件同进程加载）
// Gateway 模式:  flowsql --role gateway --port 18800 --plugins libflowsql_gateway.so:...
// Service 模式:  flowsql --role web --port 18802 --plugins libflowsql_web.so,libflowsql_router.so

#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <chrono>
#include <thread>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <common/loader.hpp>

// 仅 Guardian 模式需要解析 YAML 配置
#include "services/gateway/config.h"

using namespace flowsql;

// 解析命令行参数
struct Args {
    std::string config;       // Guardian 模式：配置文件路径
    std::string role;         // Service/Gateway 模式：角色名（仅用于日志）
    int port = 0;             // 监听端口（注入插件 option）
    std::string plugins;      // 插件列表（逗号分隔）
    std::string option;       // 传给插件的 Option 参数
};

static Args ParseArgs(int argc, char* argv[]) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            args.config = argv[++i];
        } else if (strcmp(argv[i], "--role") == 0 && i + 1 < argc) {
            args.role = argv[++i];
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

// 仅加载单个插件（pluginregist → Option → Load），不调用 Start
static int LoadPluginOnly(PluginLoader* loader, const std::string& path, const char* option) {
    std::string app_path = std::string(get_absolute_process_path());

    std::string lib_path = path;
    std::string plugin_option;
    auto colon = path.find(':');
    if (colon != std::string::npos) {
        lib_path = path.substr(0, colon);
        plugin_option = path.substr(colon + 1);
    }
    if (plugin_option.empty() && option) plugin_option = option;

    const char* relapath[] = {lib_path.c_str()};
    const char* opt_ptr = plugin_option.empty() ? nullptr : plugin_option.c_str();
    const char* options[] = {opt_ptr};
    return loader->Load(app_path.c_str(), relapath, options, 1);
}

// ============================================================
// --- Service / Gateway 模式（子进程执行）---
// ============================================================

static int RunService(const Args& args) {
    printf("========================================\n");
    printf("  FlowSQL Service: %s\n", args.role.c_str());
    printf("========================================\n\n");

    auto* loader = PluginLoader::Single();

    auto plugin_list = Split(args.plugins, ',');
    std::string auto_option;
    if (args.port > 0) {
        auto_option = "port=" + std::to_string(args.port);
        if (!args.option.empty()) auto_option += ";" + args.option;
    } else {
        auto_option = args.option;
    }

    // Phase 1：所有插件 Load()
    for (const auto& plugin : plugin_list) {
        const char* opt = auto_option.empty() ? nullptr : auto_option.c_str();
        if (LoadPluginOnly(loader, plugin, opt) != 0) {
            printf("Failed to load plugin: %s\n", plugin.c_str());
        } else {
            printf("Loaded plugin: %s\n", plugin.c_str());
        }
    }

    // Phase 2：统一 StartAll()
    if (loader->StartAll() != 0) {
        printf("Failed to start plugins\n");
        loader->StopAll();
        loader->Unload();
        return 1;
    }

    printf("Service %s running on port %d. Press Ctrl+C to stop.\n", args.role.c_str(), args.port);
    WaitForSignal();

    printf("\nShutting down service %s...\n", args.role.c_str());
    loader->StopAll();
    loader->Unload();
    return 0;
}

// ============================================================
// --- Single 模式（单进程，所有插件同进程加载）---
// ============================================================

static int RunSingle(const std::string& config_path) {
    printf("========================================\n");
    printf("  FlowSQL Single Process\n");
    printf("========================================\n\n");

    gateway::GatewayConfig config;
    if (gateway::LoadConfig(config_path, &config) != 0) {
        printf("RunSingle: failed to load config: %s\n", config_path.c_str());
        return 1;
    }

    auto* loader = PluginLoader::Single();

    // Phase 1：按 services 顺序 Load 所有插件（跳过 python 类型服务）
    for (const auto& svc : config.services) {
        if (svc.type == "python") continue;
        for (const auto& plugin : svc.plugins) {
            if (LoadPluginOnly(loader, plugin, nullptr) != 0) {
                printf("RunSingle: failed to load plugin: %s\n", plugin.c_str());
            }
        }
    }

    // Phase 2：统一 StartAll
    if (loader->StartAll() != 0) {
        printf("RunSingle: failed to start plugins\n");
        loader->StopAll();
        loader->Unload();
        return 1;
    }

    printf("RunSingle: all plugins started. Press Ctrl+C to stop.\n");
    WaitForSignal();

    printf("\nRunSingle: shutting down...\n");
    loader->StopAll();
    loader->Unload();
    return 0;
}

// ============================================================
// --- Guardian 模式（守护进程，fork 所有服务）---
// ============================================================

// 全局 running 标志，信号处理器设置
static volatile sig_atomic_t g_running = 1;

static void OnSignal(int) {
    g_running = 0;
}

// 将 ServiceConfig 转换为 argv 数组，fork 后 execv 执行
// 返回子进程 pid，失败返回 -1
static pid_t SpawnService(const std::string& exe_path,
                           const gateway::ServiceConfig& svc,
                           const std::string& config_path) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }
    if (pid > 0) {
        // 父进程：返回子进程 pid
        return pid;
    }

    // 子进程：构造 argv 并 execv
    // 对于 python 类型服务，直接执行 command
    if (svc.type == "python") {
        if (svc.command.empty()) {
            fprintf(stderr, "Guardian: python service '%s' has no command\n", svc.name.c_str());
            _exit(1);
        }
        // 简单 shell 执行
        execl("/bin/sh", "sh", "-c", svc.command.c_str(), nullptr);
        perror("execl");
        _exit(1);
    }

    // C++ 服务：构造 flowsql --role <name> --port <port> --plugins <...> [--option <...>]
    std::vector<std::string> argv_strs;
    argv_strs.push_back(exe_path);
    argv_strs.push_back("--role");
    argv_strs.push_back(svc.name);

    if (svc.port > 0) {
        argv_strs.push_back("--port");
        argv_strs.push_back(std::to_string(svc.port));
    }

    if (!svc.plugins.empty()) {
        // 拼接插件列表（逗号分隔）
        std::string plugins_str;
        for (size_t i = 0; i < svc.plugins.size(); ++i) {
            if (i > 0) plugins_str += ",";
            plugins_str += svc.plugins[i];
        }
        argv_strs.push_back("--plugins");
        argv_strs.push_back(plugins_str);
    }

    if (!svc.option.empty()) {
        argv_strs.push_back("--option");
        argv_strs.push_back(svc.option);
    }

    // 构造 C 风格 argv
    std::vector<char*> argv_ptrs;
    for (auto& s : argv_strs) argv_ptrs.push_back(const_cast<char*>(s.c_str()));
    argv_ptrs.push_back(nullptr);

    execv(exe_path.c_str(), argv_ptrs.data());
    perror("execv");
    _exit(1);
}

static int RunGuardian(const std::string& config_path) {
    // 先解析配置，根据 mode 决定走单进程还是多进程
    gateway::GatewayConfig config;
    if (gateway::LoadConfig(config_path, &config) != 0) {
        printf("Guardian: failed to load config: %s\n", config_path.c_str());
        return 1;
    }
    if (config.mode == "single") {
        return RunSingle(config_path);
    }

    printf("========================================\n");
    printf("  FlowSQL Guardian\n");
    printf("========================================\n\n");

    // 注册信号处理（不使用 sigwait，因为需要 waitpid 响应子进程退出）
    signal(SIGTERM, OnSignal);
    signal(SIGINT,  OnSignal);
    // 忽略 SIGPIPE，防止子进程管道断开导致守护进程退出
    signal(SIGPIPE, SIG_IGN);

    // 获取当前可执行文件路径
    std::string exe_path = std::string(get_absolute_process_path()) + "/flowsql";

    // fork 所有服务（按 services 顺序，第一个服务启动后等待就绪）
    std::map<pid_t, gateway::ServiceConfig> children;

    auto spawn_all = [&]() {
        bool first = true;
        for (const auto& svc : config.services) {
            pid_t pid = SpawnService(exe_path, svc, config_path);
            if (pid > 0) {
                children[pid] = svc;
                printf("Guardian: spawned %s (pid=%d)\n", svc.name.c_str(), pid);
                if (first) {
                    // 等待第一个服务（Gateway）就绪后再启动其他服务
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    first = false;
                }
            }
        }
    };

    spawn_all();

    printf("Guardian: all services started. Watching...\n");

    // 守护循环：子进程退出就重启
    while (g_running) {
        int status;
        pid_t died = waitpid(-1, &status, WNOHANG);
        if (died > 0) {
            auto it = children.find(died);
            if (it != children.end()) {
                gateway::ServiceConfig dead_svc = it->second;
                children.erase(it);

                if (WIFEXITED(status)) {
                    printf("Guardian: %s (pid=%d) exited with code %d\n",
                           dead_svc.name.c_str(), died, WEXITSTATUS(status));
                } else if (WIFSIGNALED(status)) {
                    printf("Guardian: %s (pid=%d) killed by signal %d\n",
                           dead_svc.name.c_str(), died, WTERMSIG(status));
                }

                if (g_running) {
                    // 短暂延迟后重启，避免崩溃循环
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    pid_t new_pid = SpawnService(exe_path, dead_svc, config_path);
                    if (new_pid > 0) {
                        children[new_pid] = dead_svc;
                        printf("Guardian: restarted %s (new pid=%d)\n",
                               dead_svc.name.c_str(), new_pid);
                    }
                }
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    printf("\nGuardian: shutting down all services...\n");

    // 优雅关闭：SIGTERM 所有子进程
    for (auto& [pid, svc] : children) {
        printf("Guardian: sending SIGTERM to %s (pid=%d)\n", svc.name.c_str(), pid);
        kill(pid, SIGTERM);
    }

    // 等待所有子进程退出（最多 5 秒）
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (auto& [pid, svc] : children) {
        while (std::chrono::steady_clock::now() < deadline) {
            int st;
            if (waitpid(pid, &st, WNOHANG) > 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    printf("Guardian: done\n");
    return 0;
}

// ============================================================
// --- main ---
// ============================================================

int main(int argc, char* argv[]) {
    Args args = ParseArgs(argc, argv);

    if (!args.config.empty()) {
        // Guardian 模式：fork 守护，拉起所有服务
        return RunGuardian(args.config);
    } else if (!args.role.empty()) {
        // Service/Gateway 模式：加载插件并运行
        return RunService(args);
    } else {
        printf("Usage:\n");
        printf("  Guardian mode: %s --config gateway.yaml\n", argv[0]);
        printf("  Service mode:  %s --role <name> --port <port> --plugins <plugin1,plugin2> [--option <opts>]\n",
               argv[0]);
        return 1;
    }
}
