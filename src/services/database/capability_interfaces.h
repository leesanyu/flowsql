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

// 批量读取能力（行式数据库）
interface IBatchReadable {
    virtual ~IBatchReadable() = default;
    virtual int CreateReader(const char* query, IBatchReader** reader) = 0;
};

// 批量写入能力（行式数据库）
interface IBatchWritable {
    virtual ~IBatchWritable() = default;
    virtual int CreateWriter(const char* table, IBatchWriter** writer) = 0;
};

// Arrow 原生读取能力（列式数据库 - 未来扩展）
interface IArrowReadable {
    virtual ~IArrowReadable() = default;
    virtual int ExecuteQueryArrow(const char* sql,
                                  std::vector<std::shared_ptr<arrow::RecordBatch>>* batches,
                                  std::string* error) = 0;
};

// Arrow 原生写入能力（列式数据库 - 未来扩展）
interface IArrowWritable {
    virtual ~IArrowWritable() = default;
    virtual int WriteArrowBatches(const char* table,
                                  const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches,
                                  std::string* error) = 0;
};

// 事务支持能力
interface ITransactional {
    virtual ~ITransactional() = default;
    virtual int BeginTransaction(std::string* error) = 0;
    virtual int CommitTransaction(std::string* error) = 0;
    virtual int RollbackTransaction(std::string* error) = 0;
};

}  // namespace database
}  // namespace flowsql

#endif  // _FLOWSQL_SERVICES_DATABASE_CAPABILITY_INTERFACES_H_
