#ifndef _FLOWSQL_SERVICES_DATABASE_IDB_DRIVER_H_
#define _FLOWSQL_SERVICES_DATABASE_IDB_DRIVER_H_

#include <common/typedef.h>

#include <string>
#include <unordered_map>

namespace flowsql {
namespace database {

// IDbDriver — 数据库驱动基础接口（所有驱动必须实现）
// 只包含连接管理和元数据，不包含数据读写方法
// 数据读写能力通过能力接口（IBatchReadable/IBatchWritable等）按需组合
interface IDbDriver {
    virtual ~IDbDriver() = default;

    // 连接管理
    virtual int Connect(const std::unordered_map<std::string, std::string>& params) = 0;
    virtual int Disconnect() = 0;
    virtual bool IsConnected() = 0;

    // 驱动元数据
    virtual const char* DriverName() = 0;
    virtual const char* LastError() = 0;

    // 健康检查
    virtual bool Ping() = 0;
};

}  // namespace database
}  // namespace flowsql

#endif  // _FLOWSQL_SERVICES_DATABASE_IDB_DRIVER_H_
