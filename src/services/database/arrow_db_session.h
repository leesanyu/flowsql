#ifndef _FLOWSQL_SERVICES_DATABASE_ARROW_DB_SESSION_H_
#define _FLOWSQL_SERVICES_DATABASE_ARROW_DB_SESSION_H_

#include "db_session.h"
#include "capability_interfaces.h"
#include "idb_driver.h"

#include <arrow/api.h>

#include <memory>
#include <string>
#include <vector>

namespace flowsql {
namespace database {

// ==================== Arrow Traits 定义示例 ====================

// ClickHouse Traits（未来扩展）
struct ClickHouseTraits {
    using ConnectionType = void*;  // clickhouse_cpp 的连接类型
    using BatchType = std::shared_ptr<arrow::RecordBatch>;
};

// ==================== 列式数据库模板基类 ====================

// ArrowDbSessionBase - 列式数据库通用模板基类
// 使用 traits 模式封装数据库特定的类型和操作
template<typename Traits>
class ArrowDbSessionBase : public IDbSession,
                            public IArrowReadable,
                            public IArrowWritable {
public:
    ArrowDbSessionBase(IDbDriver* driver, typename Traits::ConnectionType conn)
        : driver_(driver), conn_(conn) {}

    virtual ~ArrowDbSessionBase() = default;

    // IDbSession 实现 - ExecuteQueryArrow
    int ExecuteQueryArrow(const char* sql,
                          std::vector<std::shared_ptr<arrow::RecordBatch>>* batches,
                          std::string* error) override {
        return FetchArrowBatches(conn_, sql, batches, error);
    }

    // IDbSession 实现 - WriteArrowBatches
    int WriteArrowBatches(const char* table,
                          const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches,
                          std::string* error) override {
        return WriteBatches(conn_, table, batches, error);
    }

    // IArrowReadable 实现
    int ExecuteQueryArrow(const char* sql,
                          std::vector<std::shared_ptr<arrow::RecordBatch>>* batches,
                          std::string* error) override {
        return FetchArrowBatches(conn_, sql, batches, error);
    }

    // IArrowWritable 实现
    int WriteArrowBatches(const char* table,
                          const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches,
                          std::string* error) override {
        return WriteBatches(conn_, table, batches, error);
    }

    bool Ping() override {
        return PingImpl(conn_);
    }

protected:
    // ==================== 钩子方法（子类必须实现） ====================

    // 获取 Arrow RecordBatches
    virtual int FetchArrowBatches(
        typename Traits::ConnectionType conn,
        const char* sql,
        std::vector<std::shared_ptr<arrow::RecordBatch>>* batches,
        std::string* error) = 0;

    // 写入 Arrow RecordBatches
    virtual int WriteBatches(
        typename Traits::ConnectionType conn,
        const char* table,
        const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches,
        std::string* error) = 0;

    // 执行 DDL 语句
    virtual int ExecuteDDL(
        typename Traits::ConnectionType conn,
        const char* sql) = 0;

    // 健康检查实现
    virtual bool PingImpl(typename Traits::ConnectionType conn) = 0;

    // 归还连接到连接池
    virtual void ReturnConnection(typename Traits::ConnectionType conn) = 0;

protected:
    IDbDriver* driver_;
    typename Traits::ConnectionType conn_;
};

}  // namespace database
}  // namespace flowsql

#endif  // _FLOWSQL_SERVICES_DATABASE_ARROW_DB_SESSION_H_
