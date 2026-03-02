#ifndef _FLOWSQL_BRIDGE_CONTROL_MESSAGE_H_
#define _FLOWSQL_BRIDGE_CONTROL_MESSAGE_H_

#include <string>
#include <vector>

#include "python_operator_bridge.h"

namespace flowsql {
namespace bridge {

class ControlMessage {
public:
    // 构建消息（C++ → Worker）
    static std::string BuildCommand(const std::string& type, const std::string& payload_json = "{}");
    static std::string BuildShutdown();
    static std::string BuildReloadOperators();

    // 解析消息（Worker → C++）
    // 返回 0 成功，-1 失败
    static int ParseMessage(const std::string& json, std::string* type, std::string* payload);

    // 解析 worker_ready 消息的 payload
    static int ParseOperatorList(const std::string& payload_json, std::vector<OperatorMeta>* operators);

    // 解析 operator_added 消息的 payload
    static int ParseOperatorMeta(const std::string& payload_json, OperatorMeta* meta);
};

}  // namespace bridge
}  // namespace flowsql

#endif  // _FLOWSQL_BRIDGE_CONTROL_MESSAGE_H_
