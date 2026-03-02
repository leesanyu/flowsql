#include "python_process_manager.h"

#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

extern char** environ;

namespace flowsql {
namespace bridge {

PythonProcessManager::~PythonProcessManager() {
    Stop();
}

void PythonProcessManager::SetMessageHandler(IMessageHandler* handler) {
    message_handler_ = handler;
}

int PythonProcessManager::Start(const std::string& host, int port, const std::string& operators_dir,
                                const std::string& python_path) {
    if (IsAlive()) {
        printf("PythonProcessManager: Worker already running (pid=%d)\n", pid_);
        return 0;
    }

    host_ = host;
    port_ = port;

    // 1. 启动 ControlServer
    control_server_ = std::make_unique<ControlServer>();
    std::string socket_path = "/tmp/flowsql_control_" + std::to_string(getpid()) + ".sock";
    if (control_server_->Start(socket_path, message_handler_) != 0) {
        printf("PythonProcessManager: failed to start ControlServer\n");
        return -1;
    }

    // 2. 构建命令行参数: python3 -m flowsql.worker --host HOST --port PORT --operators-dir DIR
    std::string port_str = std::to_string(port);

    std::vector<const char*> argv;
    argv.push_back(python_path.c_str());
    argv.push_back("-m");
    argv.push_back("flowsql.worker");
    argv.push_back("--host");
    argv.push_back(host.c_str());
    argv.push_back("--port");
    argv.push_back(port_str.c_str());
    argv.push_back("--operators-dir");
    argv.push_back(operators_dir.c_str());
    argv.push_back(nullptr);

    // 3. 构建环境变量（添加 PYTHONPATH + FLOWSQL_CONTROL_SOCKET）
    // 获取可执行文件路径，推导 Python 模块路径
    char exe_path[1024];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    std::string python_module_path;
    if (len != -1) {
        exe_path[len] = '\0';
        // 可执行文件在 build/output/，Python 模块在 ../../python/
        std::string exe_dir(exe_path);
        size_t last_slash = exe_dir.find_last_of('/');
        if (last_slash != std::string::npos) {
            exe_dir = exe_dir.substr(0, last_slash);  // 去掉可执行文件名
            // 假设目录结构: build/output/xxx -> ../../src/python
            python_module_path = exe_dir + "/../../src/python";
        }
    }

    std::vector<std::string> env_strings;
    std::vector<const char*> envp;

    // 复制现有环境变量
    bool has_pythonpath = false;
    for (char** env = environ; *env != nullptr; ++env) {
        std::string env_str(*env);
        // 如果是 PYTHONPATH，追加我们的路径
        if (env_str.find("PYTHONPATH=") == 0) {
            has_pythonpath = true;
            if (!python_module_path.empty()) {
                env_str += ":" + python_module_path;
            }
        }
        env_strings.push_back(env_str);
    }

    // 如果没有 PYTHONPATH，添加一个
    if (!has_pythonpath && !python_module_path.empty()) {
        env_strings.push_back("PYTHONPATH=" + python_module_path);
    }

    // 添加 FLOWSQL_CONTROL_SOCKET 环境变量
    env_strings.push_back("FLOWSQL_CONTROL_SOCKET=" + socket_path);

    // 转换为 char* 数组
    for (const auto& env_str : env_strings) {
        envp.push_back(env_str.c_str());
    }
    envp.push_back(nullptr);

    // 4. 使用 posix_spawn 启动 Worker 进程
    pid_t child_pid;
    int ret = posix_spawn(&child_pid, python_path.c_str(), nullptr, nullptr,
                          const_cast<char* const*>(argv.data()),
                          const_cast<char* const*>(envp.data()));
    if (ret != 0) {
        // posix_spawn 可能找不到绝对路径，尝试 posix_spawnp（PATH 搜索）
        ret = posix_spawnp(&child_pid, python_path.c_str(), nullptr, nullptr,
                           const_cast<char* const*>(argv.data()),
                           const_cast<char* const*>(envp.data()));
        if (ret != 0) {
            printf("PythonProcessManager: posix_spawnp failed: %s\n", strerror(ret));
            // spawn 失败，清理 ControlServer 避免 socket 泄漏（问题 11）
            control_server_->Stop();
            control_server_.reset();
            return -1;
        }
    }

    pid_ = child_pid;
    printf("PythonProcessManager: Worker started (pid=%d, port=%d, control_socket=%s)\n",
           pid_, port_, socket_path.c_str());

    // 5. 等待 Worker 就绪（阻塞，超时 10 秒）
    if (control_server_->WaitWorkerReady(10000) != 0) {
        printf("PythonProcessManager: Worker not ready, killing process\n");
        Stop();
        return -1;
    }

    printf("PythonProcessManager: Worker ready\n");
    return 0;
}

int PythonProcessManager::SendCommand(const std::string& type, const std::string& payload_json) {
    if (!control_server_) {
        printf("PythonProcessManager: ControlServer not initialized\n");
        return -1;
    }
    return control_server_->SendCommand(type, payload_json);
}

void PythonProcessManager::Stop() {
    // 停止 ControlServer
    if (control_server_) {
        control_server_->Stop();
        control_server_.reset();
    }

    if (pid_ <= 0) return;

    if (IsAlive()) {
        // SIGTERM 优雅关闭
        kill(pid_, SIGTERM);

        // 等待 2 秒
        for (int i = 0; i < 20; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            int status;
            pid_t result = waitpid(pid_, &status, WNOHANG);
            if (result == pid_) {
                printf("PythonProcessManager: Worker stopped gracefully (pid=%d)\n", pid_);
                pid_ = -1;
                return;
            }
        }

        // SIGKILL 强制终止
        kill(pid_, SIGKILL);
        int status;
        waitpid(pid_, &status, 0);
        printf("PythonProcessManager: Worker killed (pid=%d)\n", pid_);
    } else {
        // 回收僵尸进程
        int status;
        waitpid(pid_, &status, WNOHANG);
    }

    pid_ = -1;
}

bool PythonProcessManager::IsAlive() const {
    if (pid_ <= 0) return false;
    return kill(pid_, 0) == 0;
}

}  // namespace bridge
}  // namespace flowsql
