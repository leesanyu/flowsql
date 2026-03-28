#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_IBRIDGE_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_IBRIDGE_H_

#include <common/iplugin.h>

#include <functional>
#include <memory>
#include <string>

namespace flowsql {

// {0xa1b2c3d4, 0xe5f6, 0x7890, {0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89}}
const Guid IID_BRIDGE = {0xa1b2c3d4, 0xe5f6, 0x7890, {0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89}};

interface IOperator;  // 前向声明

// IBridge — 纯接口，不继承 IPlugin
// 提供 Python 算子的查询、遍历和刷新能力
interface IBridge {
    virtual ~IBridge() {}
    // 按 category + name 查找算子，返回 shared_ptr 保证生命周期安全
    virtual std::shared_ptr<IOperator> FindOperator(const std::string& category, const std::string& name) = 0;
    // 遍历所有已发现的算子，回调返回 -1 时停止遍历
    virtual void TraverseOperators(std::function<int(IOperator*)> fn) = 0;
    // 重新从 Python Worker 发现算子
    virtual int Refresh() = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_IBRIDGE_H_
