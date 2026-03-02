#ifndef _FLOWSQL_BRIDGE_BRIDGE_PLUGIN_H_
#define _FLOWSQL_BRIDGE_BRIDGE_PLUGIN_H_

#include <memory>
#include <string>
#include <vector>

#include <common/loader.hpp>

#include "python_operator_bridge.h"

namespace flowsql {

class PluginRegistry;

namespace bridge {

// BridgePlugin — 桥接插件生命周期管理器
// Gateway 架构下：不再管理 Python Worker 进程，通过 HTTP 发现 Worker 并注册算子
class BridgePlugin : public IPlugin {
 public:
    BridgePlugin() = default;
    ~BridgePlugin() override = default;

    // IPlugin
    int Option(const char* arg) override;
    int Load() override;
    int Unload() override;
    int Start() override;
    int Stop() override;

 private:
    // 从 Python Worker 获取算子列表并注册
    int DiscoverAndRegisterOperators();

    PluginRegistry* registry_ = nullptr;

    // 已动态注册的算子 key 列表（catelog.name）
    std::vector<std::string> registered_keys_;

    // 配置参数
    std::string host_ = "127.0.0.1";
    int port_ = 18900;
    std::string gateway_addr_;  // Gateway 地址（从环境变量获取）
};

}  // namespace bridge
}  // namespace flowsql

#endif  // _FLOWSQL_BRIDGE_BRIDGE_PLUGIN_H_
