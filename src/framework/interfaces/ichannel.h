#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_ICHANNEL_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_ICHANNEL_H_

#include <common/guid.h>
#include <common/typedef.h>

namespace flowsql {

// {0xc1d2e3f4-abcd-ef01-2345-6789abcdef01}
const Guid IID_CHANNEL = {0xc1d2e3f4, 0xabcd, 0xef01, {0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef, 0x01}};

// 通道类型常量
namespace ChannelType {
    constexpr const char* kDataFrame = "dataframe";
    constexpr const char* kDatabase  = "database";
}  // namespace ChannelType

// IChannel — 数据通道基类（纯接口，不继承 IPlugin）
// 只定义生命周期、身份和元数据，数据读写方法由子类定义（IDataFrameChannel、IDatabaseChannel 等）
interface IChannel {
    virtual ~IChannel() = default;

    // 身份标识
    virtual const char* Catelog() = 0;
    virtual const char* Name() = 0;

    // 通道类型（"dataframe"、"database" 等）
    virtual const char* Type() = 0;

    // 元数据描述（格式由实现决定，如 Arrow Schema JSON）
    virtual const char* Schema() = 0;

    // 生命周期
    virtual int Open() = 0;
    virtual int Close() = 0;
    virtual bool IsOpened() const = 0;

    // 批量刷新
    virtual int Flush() = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_ICHANNEL_H_
