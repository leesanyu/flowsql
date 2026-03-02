#ifndef _FLOWSQL_BRIDGE_CONTROL_SERVER_H_
#define _FLOWSQL_BRIDGE_CONTROL_SERVER_H_

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "python_operator_bridge.h"

namespace flowsql {
namespace bridge {

// 消息处理器接口
class IMessageHandler {
public:
    virtual ~IMessageHandler() = default;
    virtual void OnWorkerReady(const std::vector<OperatorMeta>& operators) = 0;
    virtual void OnOperatorAdded(const OperatorMeta& meta) = 0;
    virtual void OnOperatorRemoved(const std::string& catelog, const std::string& name) = 0;
    virtual void OnHeartbeat(const std::string& stats_json) = 0;
    virtual void OnError(int code, const std::string& message) = 0;
};

class ControlServer {
public:
    ControlServer();
    ~ControlServer();

    // 启动服务器（创建 Unix Socket，绑定，监听）
    int Start(const std::string& socket_path, IMessageHandler* handler);

    // 停止服务器
    void Stop();

    // 等待 Worker 就绪（阻塞，超时返回 -1）
    int WaitWorkerReady(int timeout_ms);

    // 发送命令到 Worker
    int SendCommand(const std::string& type, const std::string& payload_json = "{}");

    // 获取 socket 路径
    const std::string& SocketPath() const { return socket_path_; }

private:
    int server_fd_ = -1;
    int client_fd_ = -1;
    std::string socket_path_;
    IMessageHandler* handler_ = nullptr;
    std::thread message_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> worker_ready_{false};
    std::mutex mutex_;
    std::condition_variable cv_ready_;
    std::string recv_buffer_;  // TCP 粘包缓冲区，跨 ReceiveMessage 调用保留未处理数据

    // 接受连接
    int AcceptConnection();

    // 接收消息（阻塞读取一行，以 \n 结尾）
    int ReceiveMessage(std::string* msg);

    // 发送消息
    int SendMessage(const std::string& msg);

    // 消息处理循环（在独立线程中运行）
    void MessageLoop();

    // 分发消息到处理器
    void DispatchMessage(const std::string& msg_json);
};

}  // namespace bridge
}  // namespace flowsql

#endif  // _FLOWSQL_BRIDGE_CONTROL_SERVER_H_
