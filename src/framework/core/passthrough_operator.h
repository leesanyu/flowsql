#ifndef _FLOWSQL_FRAMEWORK_CORE_PASSTHROUGH_OPERATOR_H_
#define _FLOWSQL_FRAMEWORK_CORE_PASSTHROUGH_OPERATOR_H_

#include <framework/interfaces/ioperator.h>

#include <string>

namespace flowsql {

// PassthroughOperator — 无状态直通算子
// 从 in 通道读取 DataFrame，原样写入 out 通道
// 不继承 IPlugin，作为公共类直接构造使用
class PassthroughOperator : public IOperator {
 public:
    PassthroughOperator() = default;
    ~PassthroughOperator() override = default;

    // IOperator 元数据
    std::string Category() override { return "builtin"; }
    std::string Name() override { return "passthrough"; }
    std::string Description() override { return "Passthrough operator, copies data as-is"; }
    OperatorPosition Position() override { return OperatorPosition::DATA; }

    // 核心处理：从 in 通道读取 DataFrame，原样写入 out 通道
    int Work(IChannel* in, IChannel* out) override;

    // 可选配置：WITH delay_ms=<N>，用于测试长时运行场景
    int Configure(const char* key, const char* value) override;

 private:
    int delay_ms_ = 0;
    bool force_fail_ = false;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_CORE_PASSTHROUGH_OPERATOR_H_
