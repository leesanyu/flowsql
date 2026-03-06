#ifndef _FLOWSQL_SERVICES_DATABASE_ROW_BASED_DB_DRIVER_BASE_H_
#define _FLOWSQL_SERVICES_DATABASE_ROW_BASED_DB_DRIVER_BASE_H_

#include "capability_interfaces.h"
#include "idb_driver.h"

#include <arrow/api.h>

#include <memory>
#include <string>
#include <vector>

namespace flowsql {
namespace database {

// RowBasedDbDriverBase — 行式数据库辅助基类
// 实现 IDbDriver + IBatchReadable + IBatchWritable + ITransactional
// 提供模板方法实现，子类只需实现 5 个钩子方法
class __attribute__((visibility("default"))) RowBasedDbDriverBase : public IDbDriver,
                              public IBatchReadable,
                              public IBatchWritable,
                              public ITransactional {
public:
    // IDbDriver 实现
    const char* LastError() override { return last_error_.c_str(); }

    // IBatchReadable 实现（模板方法）
    int CreateReader(const char* query, IBatchReader** reader) override;

    // IBatchWritable 实现（模板方法）
    int CreateWriter(const char* table, IBatchWriter** writer) override;

    // ITransactional 实现
    int BeginTransaction(std::string* error) override {
        return ExecuteSqlImpl("BEGIN", error);
    }
    int CommitTransaction(std::string* error) override {
        return ExecuteSqlImpl("COMMIT", error);
    }
    int RollbackTransaction(std::string* error) override {
        return ExecuteSqlImpl("ROLLBACK", error);
    }

protected:
    // 子类实现的钩子方法
    // void* 是数据库特定的结果集类型（如 sqlite3_stmt*、MYSQL_RES*）
    virtual void* ExecuteQueryImpl(const char* sql, std::string* error) = 0;
    virtual std::shared_ptr<arrow::Schema> InferSchemaImpl(void* result, std::string* error) = 0;
    virtual int FetchRowImpl(void* result,
                            const std::vector<std::unique_ptr<arrow::ArrayBuilder>>& builders,
                            std::string* error) = 0;
    virtual void FreeResultImpl(void* result) = 0;
    virtual int ExecuteSqlImpl(const char* sql, std::string* error) = 0;

    std::string last_error_;

    // 友元类，允许访问钩子方法
    friend class RowBasedBatchReader;
    friend class RowBasedBatchWriter;
};

}  // namespace database
}  // namespace flowsql

#endif  // _FLOWSQL_SERVICES_DATABASE_ROW_BASED_DB_DRIVER_BASE_H_
