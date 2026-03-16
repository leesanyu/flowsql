#ifndef _FLOWSQL_SERVICES_DATABASE_ARROW_ADAPTERS_H_
#define _FLOWSQL_SERVICES_DATABASE_ARROW_ADAPTERS_H_

#include <framework/interfaces/idatabase_channel.h>
#include "db_session.h"

#include <memory>
#include <string>
#include <vector>

namespace flowsql {
namespace database {

// ArrowReaderAdapter — 将 IArrowReader 接口委托给 IDbSession::ExecuteQueryArrow
// 用于 DatabaseChannel::CreateArrowReader()，支持列式数据库（如 ClickHouse）
class ArrowReaderAdapter : public IArrowReader {
public:
    ArrowReaderAdapter(std::shared_ptr<IDbSession> session, const char* query)
        : session_(std::move(session)), query_(query) {}

    int ExecuteQueryArrow(const char* query,
                          std::vector<std::shared_ptr<arrow::RecordBatch>>* batches) override {
        int ret = session_->ExecuteQueryArrow(query ? query : query_.c_str(), batches);
        if (ret != 0) last_error_ = session_->GetLastError();
        return ret;
    }

    // Schema 懒加载，暂不实现
    std::shared_ptr<arrow::Schema> GetSchema() override { return nullptr; }

    const char* GetLastError() override { return last_error_.c_str(); }

    void Release() override { delete this; }

private:
    std::shared_ptr<IDbSession> session_;
    std::string query_;
    std::string last_error_;
};

// ArrowWriterAdapter — 将 IArrowWriter 接口委托给 IDbSession::WriteArrowBatches
// 用于 DatabaseChannel::CreateArrowWriter()，支持列式数据库（如 ClickHouse）
class ArrowWriterAdapter : public IArrowWriter {
public:
    ArrowWriterAdapter(std::shared_ptr<IDbSession> session, const char* table)
        : session_(std::move(session)), table_(table) {}

    int WriteBatches(const char* table,
                     const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches) override {
        int ret = session_->WriteArrowBatches(table ? table : table_.c_str(), batches);
        if (ret != 0) last_error_ = session_->GetLastError();
        return ret;
    }

    const char* GetLastError() override { return last_error_.c_str(); }

    void Release() override { delete this; }

private:
    std::shared_ptr<IDbSession> session_;
    std::string table_;
    std::string last_error_;
};

}  // namespace database
}  // namespace flowsql

#endif  // _FLOWSQL_SERVICES_DATABASE_ARROW_ADAPTERS_H_
