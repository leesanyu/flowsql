#ifndef _FLOWSQL_BRIDGE_CONTROL_PROTOCOL_H_
#define _FLOWSQL_BRIDGE_CONTROL_PROTOCOL_H_

namespace flowsql {
namespace bridge {

// 协议版本
constexpr const char* PROTOCOL_VERSION = "1.0";

// 消息类型定义
namespace MessageType {
    // Worker → C++ (通知类)
    constexpr const char* WORKER_READY = "worker_ready";
    constexpr const char* OPERATOR_ADDED = "operator_added";
    constexpr const char* OPERATOR_REMOVED = "operator_removed";
    constexpr const char* HEARTBEAT = "heartbeat";
    constexpr const char* ERROR = "error";

    // C++ → Worker (命令类)
    constexpr const char* RELOAD_OPERATORS = "reload_operators";
    constexpr const char* UNLOAD_OPERATOR = "unload_operator";
    constexpr const char* UPDATE_CONFIG = "update_config";
    constexpr const char* SHUTDOWN = "shutdown";

    // 双向
    constexpr const char* PING = "ping";
    constexpr const char* PONG = "pong";
    constexpr const char* ACK = "ack";
}

}  // namespace bridge
}  // namespace flowsql

#endif  // _FLOWSQL_BRIDGE_CONTROL_PROTOCOL_H_
