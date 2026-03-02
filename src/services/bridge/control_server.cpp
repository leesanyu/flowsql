#include "control_server.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>

#include <rapidjson/document.h>

#include "control_message.h"
#include "control_protocol.h"

namespace flowsql {
namespace bridge {

ControlServer::ControlServer() {}

ControlServer::~ControlServer() {
    Stop();
}

int ControlServer::Start(const std::string& socket_path, IMessageHandler* handler) {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        printf("ControlServer: already running\n");
        return -1;
    }

    socket_path_ = socket_path;
    handler_ = handler;

    // 删除旧的 socket 文件
    unlink(socket_path_.c_str());

    // 创建 Unix Domain Socket
    server_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        printf("ControlServer: socket() failed: %s\n", strerror(errno));
        running_ = false;
        return -1;
    }

    // 绑定
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(server_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        printf("ControlServer: bind() failed: %s\n", strerror(errno));
        close(server_fd_);
        server_fd_ = -1;
        running_ = false;
        return -1;
    }

    // 监听
    if (listen(server_fd_, 1) < 0) {
        printf("ControlServer: listen() failed: %s\n", strerror(errno));
        close(server_fd_);
        server_fd_ = -1;
        unlink(socket_path_.c_str());
        running_ = false;
        return -1;
    }

    printf("ControlServer: listening on %s\n", socket_path_.c_str());

    // 启动消息处理线程
    message_thread_ = std::thread(&ControlServer::MessageLoop, this);

    return 0;
}

void ControlServer::Stop() {
    // 原子化 check-then-act，避免并发调用（问题 12）
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) return;

    // 关闭客户端连接（打断 recv 阻塞）
    if (client_fd_ >= 0) {
        close(client_fd_);
        client_fd_ = -1;
    }

    // 关闭服务器 socket
    if (server_fd_ >= 0) {
        close(server_fd_);
        server_fd_ = -1;
    }

    // 等待消息线程退出
    if (message_thread_.joinable()) {
        message_thread_.join();
    }

    // 删除 socket 文件
    if (!socket_path_.empty()) {
        unlink(socket_path_.c_str());
    }

    printf("ControlServer: stopped\n");
}

int ControlServer::WaitWorkerReady(int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    while (!worker_ready_ && running_) {
        if (cv_ready_.wait_until(lock, deadline) == std::cv_status::timeout) {
            printf("ControlServer: WaitWorkerReady timeout (%dms)\n", timeout_ms);
            return -1;
        }
    }

    if (!worker_ready_) {
        return -1;
    }

    return 0;
}

int ControlServer::SendCommand(const std::string& type, const std::string& payload_json) {
    std::string msg = ControlMessage::BuildCommand(type, payload_json);
    return SendMessage(msg);
}

int ControlServer::AcceptConnection() {
    struct sockaddr_un client_addr;
    socklen_t client_len = sizeof(client_addr);

    while (running_) {
        int fd = accept(server_fd_, (struct sockaddr*)&client_addr, &client_len);
        if (fd >= 0) {
            printf("ControlServer: accepted connection (fd=%d)\n", fd);
            return fd;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN) continue;
        printf("ControlServer: accept() failed: %s\n", strerror(errno));
        return -1;
    }
    return -1;
}

int ControlServer::ReceiveMessage(std::string* msg) {
    if (client_fd_ < 0) return -1;

    // 先检查缓冲区中是否已有完整消息（处理 TCP 粘包，问题 1）
    size_t pos = recv_buffer_.find('\n');
    if (pos != std::string::npos) {
        *msg = recv_buffer_.substr(0, pos);
        recv_buffer_.erase(0, pos + 1);
        return 0;
    }

    char buffer[4096];
    while (true) {
        ssize_t n = recv(client_fd_, buffer, sizeof(buffer), 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            printf("ControlServer: recv() failed: %s\n", strerror(errno));
            return -1;
        }
        if (n == 0) {
            printf("ControlServer: connection closed by peer\n");
            return -1;
        }

        recv_buffer_.append(buffer, n);

        pos = recv_buffer_.find('\n');
        if (pos != std::string::npos) {
            *msg = recv_buffer_.substr(0, pos);
            recv_buffer_.erase(0, pos + 1);
            return 0;
        }
    }
}

int ControlServer::SendMessage(const std::string& msg) {
    if (client_fd_ < 0) {
        printf("ControlServer: no client connected\n");
        return -1;
    }

    // 循环发送直到全部写完（问题 17）
    size_t total = msg.length();
    size_t sent = 0;
    while (sent < total) {
        ssize_t n = send(client_fd_, msg.c_str() + sent, total - sent, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            printf("ControlServer: send() failed: %s\n", strerror(errno));
            return -1;
        }
        sent += n;
    }

    return 0;
}

void ControlServer::MessageLoop() {
    // 外层循环：持续接受新连接（断线重连）
    while (running_) {
        client_fd_ = AcceptConnection();
        if (client_fd_ < 0) {
            if (!running_) break;  // 正常关闭
            printf("ControlServer: failed to accept connection, retrying...\n");
            continue;
        }

        // 内层循环：接收并处理当前连接的消息
        while (running_) {
            std::string msg;
            if (ReceiveMessage(&msg) != 0) {
                // 连接断开，清理后回到外层重新 accept
                printf("ControlServer: connection lost, waiting for reconnect...\n");
                recv_buffer_.clear();
                close(client_fd_);
                client_fd_ = -1;
                break;
            }

            DispatchMessage(msg);
        }
    }
}

void ControlServer::DispatchMessage(const std::string& msg_json) {
    printf("ControlServer::DispatchMessage: msg_json=[%s]\n", msg_json.c_str());
    std::string type, payload;
    if (ControlMessage::ParseMessage(msg_json, &type, &payload) != 0) {
        printf("ControlServer: failed to parse message\n");
        return;
    }

    printf("ControlServer::DispatchMessage: type=[%s], payload_len=%zu\n", type.c_str(), payload.size());

    if (!handler_) {
        printf("ControlServer: no message handler\n");
        return;
    }

    // 分发到处理器
    if (type == MessageType::WORKER_READY) {
        printf("ControlServer: handling WORKER_READY\n");
        std::vector<OperatorMeta> operators;
        if (ControlMessage::ParseOperatorList(payload, &operators) == 0) {
            printf("ControlServer: parsed %zu operators, calling OnWorkerReady\n", operators.size());
            handler_->OnWorkerReady(operators);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                worker_ready_ = true;
            }
            cv_ready_.notify_all();
            printf("ControlServer: worker_ready_ set to true, notified\n");
        } else {
            printf("ControlServer: ParseOperatorList FAILED\n");
        }
    } else if (type == MessageType::OPERATOR_ADDED) {
        OperatorMeta meta;
        if (ControlMessage::ParseOperatorMeta(payload, &meta) == 0) {
            handler_->OnOperatorAdded(meta);
        }
    } else if (type == MessageType::OPERATOR_REMOVED) {
        OperatorMeta meta;
        if (ControlMessage::ParseOperatorMeta(payload, &meta) == 0) {
            handler_->OnOperatorRemoved(meta.catelog, meta.name);
        }
    } else if (type == MessageType::HEARTBEAT) {
        handler_->OnHeartbeat(payload);
    } else if (type == MessageType::ERROR) {
        // 解析 code 和 message
        rapidjson::Document doc;
        doc.Parse(payload.c_str());
        int code = 0;
        std::string message = payload;
        if (!doc.HasParseError() && doc.IsObject()) {
            if (doc.HasMember("code") && doc["code"].IsInt())
                code = doc["code"].GetInt();
            if (doc.HasMember("message") && doc["message"].IsString())
                message = doc["message"].GetString();
        }
        handler_->OnError(code, message);
    } else {
        printf("ControlServer: unknown message type: %s\n", type.c_str());
    }
}

}  // namespace bridge
}  // namespace flowsql
