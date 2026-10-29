// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#ifndef _FLOWSQL_BRIDGE_BRIDGE_PLUGIN_H_
#define _FLOWSQL_BRIDGE_BRIDGE_PLUGIN_H_

#include <framework/interfaces/ipython_operator_publisher.h>
#include <rapidjson/document.h>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <common/iplugin.h>

#include "framework/interfaces/ibridge.h"
#include "framework/interfaces/ioperator_catalog.h"
#include "python_operator_bridge.h"

namespace flowsql {
namespace bridge {

// BridgePlugin — 桥接插件生命周期管理器
// Gateway 架构下：不再管理 Python Worker 进程，通过 HTTP 发现 Worker 并注册算子
// 实现 IBridge 接口，提供 Python 算子的查询和刷新能力
class BridgePlugin : public IPlugin, public IBridge, public IPythonOperatorPublisherV1 {
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
    std::shared_ptr<IOperator> FindOperator(const std::string& category, const std::string& name) override;
    void TraverseOperators(std::function<int(IOperator*)> fn) override;
    int Refresh() override;
    int ReadPythonSource(const char*, const char*, std::string*, DataSemanticsErrorV1*) override;
    int PublishPythonSource(const char*, const char*, const char*, std::string*, DataSemanticsErrorV1*) override;

 private:
    using BridgeOperatorMap = std::unordered_map<std::string, std::shared_ptr<PythonOperatorBridge>>;

    // 从 Python Worker 获取算子列表并原子替换本地快照（不注册到 PluginLoader）
    int DiscoverOperators();
    int ApplyOperators(const rapidjson::Value&, const std::string& token, DataSemanticsErrorV1*);
    int WorkerControl(const std::string&, const rapidjson::Value&, rapidjson::Document*, DataSemanticsErrorV1*);
    std::mutex discovery_mu_;
    int SyncOperatorsToCatalog(const std::vector<flowsql::OperatorMeta>& ops);
    size_t OperatorCount() const;

    IQuerier* querier_ = nullptr;

    // 已发现的 Python 算子快照（持有 shared_ptr 保证生命周期安全）
    mutable std::shared_mutex operators_mu_;
    BridgeOperatorMap registered_operators_;

    // 配置参数
    std::string host_ = "127.0.0.1";
    int port_ = 18900;
    std::string gateway_addr_;  // Gateway 地址（从环境变量获取）
};

}  // namespace bridge
}  // namespace flowsql

#endif  // _FLOWSQL_BRIDGE_BRIDGE_PLUGIN_H_
