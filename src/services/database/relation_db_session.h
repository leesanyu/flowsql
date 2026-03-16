#ifndef _FLOWSQL_SERVICES_DATABASE_RELATION_DB_SESSION_H_
#define _FLOWSQL_SERVICES_DATABASE_RELATION_DB_SESSION_H_

#include "db_session.h"
#include "capability_interfaces.h"
#include "idb_driver.h"

#include <arrow/api.h>

#include <memory>
#include <string>
#include <vector>

namespace flowsql {
namespace database {

// ==================== 行式数据库模板基类 ====================

// RelationDbSessionBase - 行式数据库通用模板基类
// 使用 traits 模式封装数据库特定的类型和操作
template<typename Traits>
class RelationDbSessionBase : public IDbSession,
                               public IBatchReadable,
                               public IBatchWritable {
public:
    RelationDbSessionBase(IDbDriver* driver, typename Traits::ConnectionType conn)
        : driver_(driver), conn_(conn), in_transaction_(false) {}

    virtual ~RelationDbSessionBase() = default;

    // 单一 override 同时满足 IDbSession + IBatchReadable + IBatchWritable 的 GetLastError()
    const char* GetLastError() override { return last_error_.c_str(); }

    // IDbSession 实现
    int ExecuteQuery(const char* sql, IResultSet** result) override {
        last_error_.clear();
        auto stmt = PrepareStatement(conn_, sql, &last_error_);
        if (!stmt) return -1;

        if (ExecuteStatement(stmt, &last_error_) != 0) {
            FreeStatement(stmt);
            return -1;
        }

        auto res = GetResultMetadata(stmt, &last_error_);
        if (!res) {
            FreeStatement(stmt);
            return -1;
        }

        *result = CreateResultSet(res, [this](typename Traits::ResultType r) {
            FreeResult(r);
        });
        if (!*result) {
            FreeResult(res);
            FreeStatement(stmt);
            return -1;
        }
        return 0;
    }

    int ExecuteSql(const char* sql) override {
        last_error_.clear();
        auto stmt = PrepareStatement(conn_, sql, &last_error_);
        if (!stmt) return -1;

        int ret = ExecuteStatement(stmt, &last_error_);
        int affected_rows = GetAffectedRows(stmt);
        FreeStatement(stmt);
        return (ret == 0) ? affected_rows : -1;
    }

    int BeginTransaction() override {
        last_error_.clear();
        if (in_transaction_) { last_error_ = "Transaction already in progress"; return -1; }
        int ret = ExecuteSql("BEGIN");
        if (ret != -1) in_transaction_ = true;
        return (ret != -1) ? 0 : -1;
    }

    int CommitTransaction() override {
        last_error_.clear();
        if (!in_transaction_) { last_error_ = "No transaction in progress"; return -1; }
        int ret = ExecuteSql("COMMIT");
        if (ret != -1) in_transaction_ = false;
        return (ret != -1) ? 0 : -1;
    }

    int RollbackTransaction() override {
        last_error_.clear();
        if (!in_transaction_) { last_error_ = "No transaction in progress"; return -1; }
        int ret = ExecuteSql("ROLLBACK");
        if (ret != -1) in_transaction_ = false;
        return (ret != -1) ? 0 : -1;
    }

    bool Ping() override {
        return PingImpl(conn_);
    }

    // IBatchReadable 实现
    int CreateReader(const char* query, IBatchReader** reader) override {
        last_error_.clear();
        IResultSet* raw_result = nullptr;
        if (ExecuteQuery(query, &raw_result) != 0) return -1;
        std::unique_ptr<IResultSet> result(raw_result);

        auto schema = InferSchema(result.get(), &last_error_);
        if (!schema) return -1;

        *reader = CreateBatchReader(result.release(), schema);
        return (*reader) ? 0 : -1;
    }

    // IBatchWritable 实现
    int CreateWriter(const char* table, IBatchWriter** writer) override {
        last_error_.clear();
        if (!table || table[0] == '\0') { *writer = nullptr; return -1; }
        *writer = CreateBatchWriter(table);
        return (*writer) ? 0 : -1;
    }

protected:
    // ==================== 钩子方法（子类必须实现） ====================

    // 准备 SQL 语句
    virtual typename Traits::StatementType PrepareStatement(
        typename Traits::ConnectionType conn, const char* sql, std::string* error) = 0;

    // 执行语句
    virtual int ExecuteStatement(
        typename Traits::StatementType stmt, std::string* error) = 0;

    // 获取结果元数据
    virtual typename Traits::ResultType GetResultMetadata(
        typename Traits::StatementType stmt, std::string* error) = 0;

    // 获取受影响行数
    virtual int GetAffectedRows(typename Traits::StatementType stmt) {
        return 0;  // 默认返回 0
    }

    // 获取驱动错误信息
    virtual std::string GetDriverError(typename Traits::ConnectionType conn) = 0;

    // 释放结果集
    virtual void FreeResult(typename Traits::ResultType result) = 0;

    // 释放语句
    virtual void FreeStatement(typename Traits::StatementType stmt) = 0;

    // 健康检查实现
    virtual bool PingImpl(typename Traits::ConnectionType conn) = 0;

    // 归还连接到连接池
    virtual void ReturnConnection(typename Traits::ConnectionType conn) = 0;

    // ==================== 工厂方法（子类实现） ====================

    // 创建结果集
    virtual IResultSet* CreateResultSet(
        typename Traits::ResultType result,
        std::function<void(typename Traits::ResultType)> free_func) = 0;

    // 批量读取器工厂
    virtual IBatchReader* CreateBatchReader(IResultSet* result,
                                            std::shared_ptr<arrow::Schema> schema) = 0;

    // 批量写入器工厂
    virtual IBatchWriter* CreateBatchWriter(const char* table) = 0;

    // 推断 Schema（各驱动类型映射不同，子类必须实现）
    virtual std::shared_ptr<arrow::Schema> InferSchema(IResultSet* result,
                                                       std::string* error) = 0;

protected:
    IDbDriver* driver_;
    typename Traits::ConnectionType conn_;
    bool in_transaction_;
};

}  // namespace database
}  // namespace flowsql

#endif  // _FLOWSQL_SERVICES_DATABASE_RELATION_DB_SESSION_H_
