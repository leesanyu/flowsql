#ifndef _FLOWSQL_BRIDGE_BRIDGE_PLUGIN_H_
#define _FLOWSQL_BRIDGE_BRIDGE_PLUGIN_H_

#include <memory>
#include <string>
#include <vector>

#include <common/iplugin.h>

#include "framework/interfaces/ibridge.h"
#include "python_operator_bridge.h"

namespace flowsql {
namespace bridge {

// BridgePlugin — 桥接插件生命周期管理器
// Gateway 架构下：不再管理 Python Worker 进程，通过 HTTP 发现 Worker 并注册算子
// 实现 IBridge 接口，提供 Python 算子的查询和刷新能力
class BridgePlugin : public IPlugin, public IBridge {
 public:
    BridgePlugin() = default;
    ~BridgePlugin() override = default;

    // IPlugin
    int Option(const char* arg) override;
    int Load(IQuerier* querier) override;
    int Unload() override;
    int Start() override;
    int Stop() override;

    // IBridge
    std::shared_ptr<IOperator> FindOperator(const std::string& catelog, const std::string& name) override;
    void TraverseOperators(std::function<int(IOperator*)> fn) override;
    int Refresh() override;

 private:
    // 从 Python Worker 获取算子列表（只存内部，不注册到 PluginLoader）
    int DiscoverOperators();
    int SyncOperatorsToCatalog();

    IQuerier* querier_ = nullptr;

    // 已发现的 Python 算子（持有 shared_ptr 保证生命周期安全）
    std::vector<std::shared_ptr<PythonOperatorBridge>> registered_operators_;

    // 配置参数
    std::string host_ = "127.0.0.1";
    int port_ = 18900;
    std::string gateway_addr_;  // Gateway 地址（从环境变量获取）
};

}  // namespace bridge
}  // namespace flowsql

#endif  // _FLOWSQL_BRIDGE_BRIDGE_PLUGIN_H_
