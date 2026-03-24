#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_IOPERATOR_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_IOPERATOR_H_

#include <common/guid.h>
#include <common/span.h>
#include <common/typedef.h>

#include <cstdint>
#include <string>

namespace flowsql {

// 前向声明
interface IChannel;

// {0xd4e5f6a7-bcde-f012-3456-789abcdef012}
const Guid IID_OPERATOR = {0xd4e5f6a7, 0xbcde, 0xf012, {0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0, 0x12}};

enum class OperatorPosition : int32_t {
    STORAGE = 0,
    DATA = 1
};

// IOperator — 算子纯接口，不继承 IPlugin
interface IOperator {
    virtual ~IOperator() = default;

    // 元数据
    virtual std::string Catelog() = 0;
    virtual std::string Name() = 0;
    virtual std::string Description() = 0;
    virtual OperatorPosition Position() = 0;

    // 核心处理：算子自行从 in 通道读取、向 out 通道写入
    virtual int Work(IChannel* in, IChannel* out) = 0;

    // 多输入处理：默认退化到单输入 inputs[0]。
    virtual int Work(Span<IChannel*> inputs, IChannel* out) {
        if (inputs.empty()) return -1;
        return Work(inputs[0], out);
    }

    // 配置
    virtual int Configure(const char* key, const char* value) = 0;

    // 最近一次执行错误信息（默认空）
    virtual std::string LastError() { return ""; }
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_IOPERATOR_H_
