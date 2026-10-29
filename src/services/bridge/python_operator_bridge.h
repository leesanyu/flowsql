// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#ifndef _FLOWSQL_BRIDGE_PYTHON_OPERATOR_BRIDGE_H_
#define _FLOWSQL_BRIDGE_PYTHON_OPERATOR_BRIDGE_H_

#include <framework/interfaces/ioperator_contract_catalog.h>
#include <framework/interfaces/ioperator_data_contract.h>
#include <httplib.h>

#include <memory>
#include <string>
#include <unordered_map>

#include "framework/interfaces/ioperator.h"

namespace flowsql {
namespace bridge {

// Python 算子的元数据
struct OperatorMeta {
    std::string category;
    std::string name;
    std::string description;
    OperatorPosition position = OperatorPosition::DATA;
    OperatorComponentContractV1 contract;
};

// PythonOperatorBridge — 实现 IOperator，将 Work() 转发给 Python Worker
// Work() 内部 dynamic_cast 到 IDataFrameChannel，完成 Read/Write + Arrow IPC 序列化
class PythonOperatorBridge : public IOperator, public IOperatorContractIdentityV1, public IOperatorConfigAdapterV1 {
 public:
    PythonOperatorBridge(const OperatorMeta& meta, const std::string& host, int port);
    ~PythonOperatorBridge() override;

    std::shared_ptr<PythonOperatorBridge> NewInvocation() const {
        return std::make_shared<PythonOperatorBridge>(meta_, host_, port_);
    }
    int ResolveInvocation(const char*, std::string*, DataSemanticsErrorV1*) override;

    OperatorComponentContractV1 ContractIdentity() const override { return meta_.contract; }

    // IOperator 元数据
    std::string Category() override { return meta_.category; }
    std::string Name() override { return meta_.name; }
    std::string Description() override { return meta_.description; }
    OperatorPosition Position() override { return meta_.position; }

    // 核心：从 in 通道读取 DataFrame → Arrow IPC → Python Worker → 写入 out 通道
    int Work(IChannel* in, IChannel* out) override;

    // 配置转发
    int Configure(const char* key, const char* value) override;

    // 错误信息
    std::string LastError() override { return last_error_; }

 private:
    OperatorMeta meta_;
    std::unique_ptr<httplib::Client> client_;
    std::string host_;
    int port_;
    std::string last_error_;
    std::unordered_map<std::string, std::string> parameters_;
    std::string invocation_token_;
    void ReleaseInvocation();
};

}  // namespace bridge
}  // namespace flowsql

#endif  // _FLOWSQL_BRIDGE_PYTHON_OPERATOR_BRIDGE_H_
