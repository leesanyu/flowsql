#ifndef _FLOWSQL_SERVICES_DATABASE_ROW_BASED_DB_DRIVER_BASE_H_
#define _FLOWSQL_SERVICES_DATABASE_ROW_BASED_DB_DRIVER_BASE_H_

#include "capability_interfaces.h"
#include "idb_driver.h"
#include "db_session.h"

#include <arrow/api.h>

#include <memory>
#include <string>
#include <vector>

namespace flowsql {
namespace database {

// RowBasedBatchReader — 通用批量读取器
// 持有 shared_ptr<IDbSession>，析构时自动释放结果集资源
class RowBasedBatchReader : public IBatchReader {
public:
    RowBasedBatchReader(std::shared_ptr<IDbSession> session,
                       IResultSet* result,
                       std::shared_ptr<arrow::Schema> schema);

    ~RowBasedBatchReader() override;

    int GetSchema(const uint8_t** data, size_t* size) override;
    int Next(const uint8_t** data, size_t* size) override;
    void Cancel() override;
    void Close() override;
    const char* GetLastError() override;
    void Release() override;

protected:
    // 释放结果集资源（由具体数据库驱动实现）
    virtual void FreeResultImpl(IResultSet* result) = 0;

protected:
    std::shared_ptr<IDbSession> session_;  // 持有 Session
    IResultSet* result_;
    std::shared_ptr<arrow::Schema> schema_;
    std::shared_ptr<arrow::Buffer> schema_buffer_;
    std::shared_ptr<arrow::Buffer> batch_buffer_;
    std::string last_error_;
    bool done_ = false;
    bool cancelled_ = false;
};

// RowBasedBatchWriter — 通用批量写入器
// 持有 shared_ptr<IDbSession>，事务管理由 Session 负责
class RowBasedBatchWriter : public IBatchWriter {
public:
    RowBasedBatchWriter(std::shared_ptr<IDbSession> session, const char* table);

    ~RowBasedBatchWriter() override;

    int Write(const uint8_t* data, size_t size) override;
    int Flush() override;
    void Close(BatchWriteStats* stats) override;
    const char* GetLastError() override;
    void Release() override;

protected:
    // 创建表（由具体数据库驱动实现）
    virtual int CreateTableImpl(std::shared_ptr<arrow::Schema> schema) = 0;

    // 插入一行（由具体数据库驱动实现）
    virtual int InsertRowImpl(const std::vector<std::string>& values) = 0;

protected:
    std::shared_ptr<IDbSession> session_;  // 持有 Session
    std::string table_;
    std::string last_error_;
    int64_t rows_written_ = 0;
    int64_t bytes_written_ = 0;
    bool transaction_started_ = false;
    bool committed_ = false;
};

}  // namespace database
}  // namespace flowsql

#endif  // _FLOWSQL_SERVICES_DATABASE_ROW_BASED_DB_DRIVER_BASE_H_
