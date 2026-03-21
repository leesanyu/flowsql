#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_IOPERATOR_REGISTRY_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_IOPERATOR_REGISTRY_H_

#include <common/guid.h>
#include <common/typedef.h>

#include <functional>

#include "ioperator.h"

namespace flowsql {

// {0x9e2f6a11-8f47-4a72-a466-c52b8e4f7643}
const Guid IID_OPERATOR_REGISTRY = {
    0x9e2f6a11, 0x8f47, 0x4a72, {0xa4, 0x66, 0xc5, 0x2b, 0x8e, 0x4f, 0x76, 0x43}};

// 算子工厂函数类型：无参构造，调用方负责 delete
using OperatorFactory = std::function<IOperator*()>;

// IOperatorRegistry — 内置算子类型注册中心
interface IOperatorRegistry {
    virtual ~IOperatorRegistry() = default;

    // 注册算子类型；同名时覆盖
    virtual int Register(const char* name, OperatorFactory factory) = 0;

    // 按名创建算子实例（未注册返回 nullptr）
    virtual IOperator* Create(const char* name) = 0;

    // 枚举所有已注册算子类型
    virtual void List(std::function<void(const char* name)> callback) = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_IOPERATOR_REGISTRY_H_
