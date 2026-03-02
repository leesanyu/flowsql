#ifndef _FLOWSQL_BRIDGE_PYTHON_OPERATOR_BRIDGE_H_
#define _FLOWSQL_BRIDGE_PYTHON_OPERATOR_BRIDGE_H_

#include <httplib.h>

#include <memory>
#include <string>

#include "framework/interfaces/ioperator.h"

namespace flowsql {
namespace bridge {

// Python 算子的元数据
struct OperatorMeta {
    std::string catelog;
    std::string name;
    std::string description;
    OperatorPosition position = OperatorPosition::DATA;
};

// PythonOperatorBridge — 实现 IOperator，将 Work() 转发给 Python Worker
// Work() 内部 dynamic_cast 到 IDataFrameChannel，完成 Read/Write + Arrow IPC 序列化
class PythonOperatorBridge : public IOperator {
 public:
    PythonOperatorBridge(const OperatorMeta& meta, const std::string& host, int port);
    ~PythonOperatorBridge() override = default;

    // IPlugin
    int Option(const char*) override { return 0; }
    int Load() override { return 0; }
    int Unload() override { return 0; }

    // IOperator 元数据
    std::string Catelog() override { return meta_.catelog; }
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
};

}  // namespace bridge
}  // namespace flowsql

#endif  // _FLOWSQL_BRIDGE_PYTHON_OPERATOR_BRIDGE_H_
