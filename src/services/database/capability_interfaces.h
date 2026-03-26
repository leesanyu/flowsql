#ifndef _FLOWSQL_SERVICES_DATABASE_CAPABILITY_INTERFACES_H_
#define _FLOWSQL_SERVICES_DATABASE_CAPABILITY_INTERFACES_H_

#include <framework/interfaces/idatabase_channel.h>

#include <memory>
#include <string>
#include <vector>

namespace arrow {
class RecordBatch;
}

namespace flowsql {
namespace database {

class IDbSession;

// 批量读取能力（行式数据库）
interface IBatchReadable {
    virtual ~IBatchReadable() = default;
    virtual int CreateReader(const char* query, IBatchReader** reader) = 0;
    virtual const char* GetLastError() = 0;
};

// 批量写入能力（行式数据库）
interface IBatchWritable {
    virtual ~IBatchWritable() = default;
    virtual int CreateWriter(const char* table, IBatchWriter** writer) = 0;
    virtual const char* GetLastError() = 0;
};

// Arrow 原生读取能力（列式数据库）
interface IArrowReadable {
    virtual ~IArrowReadable() = default;
    virtual int ExecuteQueryArrow(const char* sql,
                                  std::vector<std::shared_ptr<arrow::RecordBatch>>* batches) = 0;
    virtual const char* GetLastError() = 0;
};

// Arrow 原生写入能力（列式数据库）
interface IArrowWritable {
    virtual ~IArrowWritable() = default;
    virtual int WriteArrowBatches(const char* table,
                                  const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches) = 0;
    virtual const char* GetLastError() = 0;
};

// 事务支持能力
interface ITransactional {
    virtual ~ITransactional() = default;
    virtual int BeginTransaction() = 0;
    virtual int CommitTransaction() = 0;
    virtual int RollbackTransaction() = 0;
    virtual const char* GetLastError() = 0;
};

// 会话工厂能力（由 Driver 提供）
interface IDbSessionFactoryProvider {
    virtual ~IDbSessionFactoryProvider() = default;
    virtual std::shared_ptr<IDbSession> CreateSession() = 0;
};

}  // namespace database
}  // namespace flowsql

#endif  // _FLOWSQL_SERVICES_DATABASE_CAPABILITY_INTERFACES_H_
