#include "service_manager.h"

#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

extern char** environ;

namespace flowsql {
namespace gateway {

// 当前时间戳（毫秒）
static int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

ServiceManager::~ServiceManager() {
    StopAll();
}

int ServiceManager::StartAll(const GatewayConfig& config, const std::string& gateway_addr) {
    std::lock_guard lock(mutex_);
    config_ = config;

    for (const auto& svc : config.services) {
        ServiceInfo info;
        info.config = svc;
        info.address = svc.host + ":" + std::to_string(svc.port);
        info.last_heartbeat_ms = NowMs();

        if (SpawnService(info, gateway_addr) != 0) {
            printf("ServiceManager: failed to start service: %s\n", svc.name.c_str());
            continue;
        }
        services_[svc.name] = std::move(info);
    }
    return 0;
}

void ServiceManager::StopAll() {
    std::lock_guard lock(mutex_);
    for (auto& [name, info] : services_) {
        KillProcess(info.pid, name);
        info.alive = false;
    }
    services_.clear();
}

void ServiceManager::StopService(const std::string& name) {
    std::lock_guard lock(mutex_);
    auto it = services_.find(name);
    if (it == services_.end()) return;
    KillProcess(it->second.pid, name);
    it->second.alive = false;
    it->second.pid = -1;
}

int ServiceManager::RestartService(const std::string& name, const std::string& gateway_addr) {
    std::lock_guard lock(mutex_);
    auto it = services_.find(name);
    if (it == services_.end()) return -1;

    KillProcess(it->second.pid, name);
    it->second.last_heartbeat_ms = NowMs();
    return SpawnService(it->second, gateway_addr);
}

void ServiceManager::UpdateHeartbeat(const std::string& name) {
    std::lock_guard lock(mutex_);
    auto it = services_.find(name);
    if (it != services_.end()) {
        it->second.last_heartbeat_ms = NowMs();
        it->second.alive = true;
    }
}

void ServiceManager::CheckAndRestart(int timeout_s, const std::string& gateway_addr) {
    std::lock_guard lock(mutex_);
    int64_t now = NowMs();
    int64_t timeout_ms = timeout_s * 1000LL;

    for (auto& [name, info] : services_) {
        if (info.pid <= 0) continue;

        // 检查进程是否还活着
        if (kill(info.pid, 0) != 0) {
            printf("ServiceManager: service %s (pid=%d) exited, restarting...\n", name.c_str(), info.pid);
            int status;
            waitpid(info.pid, &status, WNOHANG);
            info.last_heartbeat_ms = NowMs();
            SpawnService(info, gateway_addr);
            continue;
        }

        // 检查心跳超时
        if (now - info.last_heartbeat_ms > timeout_ms) {
            printf("ServiceManager: service %s heartbeat timeout (%llds), restarting...\n", name.c_str(),
                   (long long)(now - info.last_heartbeat_ms) / 1000);
            KillProcess(info.pid, name);
            info.last_heartbeat_ms = NowMs();
            SpawnService(info, gateway_addr);
        }
    }
}

const ServiceInfo* ServiceManager::GetService(const std::string& name) {
    std::lock_guard lock(mutex_);
    auto it = services_.find(name);
    return (it != services_.end()) ? &it->second : nullptr;
}

int ServiceManager::SpawnService(ServiceInfo& info, const std::string& gateway_addr) {
    const auto& svc = info.config;

    // 获取当前可执行文件路径
    char exe_path[1024];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len <= 0) {
        printf("ServiceManager: failed to get exe path\n");
        return -1;
    }
    exe_path[len] = '\0';

    std::vector<std::string> arg_strings;
    std::vector<const char*> argv;

    if (svc.type == "python") {
        // Python 服务：直接执行 command
        // command 格式: "python3 -m flowsql.worker --host 127.0.0.1 --port 18900"
        // 简单按空格分割
        std::string cmd = svc.command;
        size_t pos = 0;
        while (pos < cmd.size()) {
            size_t space = cmd.find(' ', pos);
            if (space == std::string::npos) space = cmd.size();
            if (space > pos) arg_strings.push_back(cmd.substr(pos, space - pos));
            pos = space + 1;
        }
        // 追加 --port 参数
        if (svc.port > 0) {
            arg_strings.push_back("--port");
            arg_strings.push_back(std::to_string(svc.port));
        }
    } else {
        // C++ 服务：使用同一个可执行文件，不同角色
        arg_strings.push_back(exe_path);
        arg_strings.push_back("--role");
        arg_strings.push_back(svc.name);
        arg_strings.push_back("--gateway");
        arg_strings.push_back(gateway_addr);
        arg_strings.push_back("--port");
        arg_strings.push_back(std::to_string(svc.port));
        // 插件列表
        if (!svc.plugins.empty()) {
            arg_strings.push_back("--plugins");
            std::string plugins_str;
            for (size_t i = 0; i < svc.plugins.size(); ++i) {
                if (i > 0) plugins_str += ",";
                plugins_str += svc.plugins[i];
            }
            arg_strings.push_back(plugins_str);
        }
        // 数据库配置（新增）
        if (!svc.databases.empty()) {
            arg_strings.push_back("--databases");
            std::string databases_str;
            for (size_t i = 0; i < svc.databases.size(); ++i) {
                if (i > 0) databases_str += "|";  // 使用 | 分隔多个数据库
                // 格式: type=sqlite;name=testdb;path=:memory:
                databases_str += "type=" + svc.databases[i].type + ";name=" + svc.databases[i].name;
                for (const auto& [k, v] : svc.databases[i].params) {
                    databases_str += ";" + k + "=" + v;
                }
            }
            arg_strings.push_back(databases_str);
        }
        // Option 参数
        if (!svc.option.empty()) {
            arg_strings.push_back("--option");
            arg_strings.push_back(svc.option);
        }
    }

    for (const auto& s : arg_strings) argv.push_back(s.c_str());
    argv.push_back(nullptr);

    // 构建环境变量（追加 FLOWSQL_GATEWAY_ADDR）
    std::vector<std::string> env_strings;
    for (char** env = environ; *env != nullptr; ++env) {
        env_strings.emplace_back(*env);
    }
    env_strings.push_back("FLOWSQL_GATEWAY_ADDR=" + gateway_addr);

    // Python 服务需要设置 PYTHONPATH
    if (svc.type == "python") {
        std::string exe_dir(exe_path);
        size_t last_slash = exe_dir.find_last_of('/');
        if (last_slash != std::string::npos) {
            std::string python_path = exe_dir.substr(0, last_slash) + "/../../src/python";
            bool found = false;
            for (auto& e : env_strings) {
                if (e.find("PYTHONPATH=") == 0) {
                    e += ":" + python_path;
                    found = true;
                    break;
                }
            }
            if (!found) env_strings.push_back("PYTHONPATH=" + python_path);
        }
    }

    std::vector<const char*> envp;
    for (const auto& e : env_strings) envp.push_back(e.c_str());
    envp.push_back(nullptr);

    // posix_spawn
    pid_t child_pid;
    const char* exec_path = (svc.type == "python") ? arg_strings[0].c_str() : exe_path;
    int ret = posix_spawnp(&child_pid, exec_path, nullptr, nullptr,
                           const_cast<char* const*>(argv.data()),
                           const_cast<char* const*>(envp.data()));
    if (ret != 0) {
        printf("ServiceManager: posix_spawnp failed for %s: %s\n", svc.name.c_str(), strerror(ret));
        return -1;
    }

    info.pid = child_pid;
    info.alive = true;
    printf("ServiceManager: started %s (pid=%d, port=%d)\n", svc.name.c_str(), child_pid, svc.port);
    return 0;
}

void ServiceManager::KillProcess(pid_t pid, const std::string& name) {
    if (pid <= 0) return;
    if (kill(pid, 0) != 0) {
        // 进程已退出
        int status;
        waitpid(pid, &status, WNOHANG);
        return;
    }

    // SIGTERM → 等待 2s → SIGKILL
    kill(pid, SIGTERM);
    for (int i = 0; i < 20; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        int status;
        if (waitpid(pid, &status, WNOHANG) == pid) {
            printf("ServiceManager: %s (pid=%d) stopped gracefully\n", name.c_str(), pid);
            return;
        }
    }

    kill(pid, SIGKILL);
    int status;
    waitpid(pid, &status, 0);
    printf("ServiceManager: %s (pid=%d) killed\n", name.c_str(), pid);
}

}  // namespace gateway
}  // namespace flowsql
