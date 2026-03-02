#ifndef _FLOWSQL_BRIDGE_PYTHON_PROCESS_MANAGER_H_
#define _FLOWSQL_BRIDGE_PYTHON_PROCESS_MANAGER_H_

#include <memory>
#include <string>

#include "control_server.h"

namespace flowsql {
namespace bridge {

class PythonProcessManager {
 public:
    PythonProcessManager() = default;
    ~PythonProcessManager();

    // 设置消息处理器
    void SetMessageHandler(IMessageHandler* handler);

    // 启动 Python Worker 进程（阻塞等待就绪）
    // 返回 0 表示成功，Worker 已就绪且算子已加载
    int Start(const std::string& host, int port, const std::string& operators_dir,
              const std::string& python_path = "python3");

    // 停止 Worker 进程（SIGTERM → 等待 → SIGKILL）
    void Stop();

    // 检查进程是否存活
    bool IsAlive() const;

    // 发送命令到 Worker
    int SendCommand(const std::string& type, const std::string& payload_json = "{}");

    int Port() const { return port_; }
    const std::string& Host() const { return host_; }

 private:
    pid_t pid_ = -1;
    std::string host_;
    int port_ = 0;
    std::unique_ptr<ControlServer> control_server_;
    IMessageHandler* message_handler_ = nullptr;
};

}  // namespace bridge
}  // namespace flowsql

#endif  // _FLOWSQL_BRIDGE_PYTHON_PROCESS_MANAGER_H_
