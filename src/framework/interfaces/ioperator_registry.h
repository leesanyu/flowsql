// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

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

/**
 * @brief 内置算子类型注册中心接口。
 */
interface IOperatorRegistry {
    virtual ~IOperatorRegistry() = default;

    /**
     * @brief 注册算子工厂。
     * @param name 算子类型全名 category.name（分类和名称均非空）。
     * @param factory 算子工厂函数。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int Register(const char* name, OperatorFactory factory) = 0;

    /**
     * @brief 按名称创建算子实例。
     * @param name 算子类型全名 category.name（分类和名称均非空）。
     * @return 新创建的算子实例（调用方负责释放），未注册返回 nullptr。
     */
    virtual IOperator* Create(const char* name) = 0;

    /**
     * @brief 移除算子工厂。
     * @param name 算子类型全名 category.name（分类和名称均非空）。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int RemoveFactory(const char* name) = 0;

    /**
     * @brief 枚举所有已注册算子名称。
     * @param callback 枚举回调，参数为算子名。
     */
    virtual void List(std::function<void(const char* name)> callback) = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_IOPERATOR_REGISTRY_H_
