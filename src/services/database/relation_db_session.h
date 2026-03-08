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

// ==================== 行式数据库结果集实现 ====================

// 通用的基于行的结果集实现
template<typename Traits>
class RelationDbResultSet : public IResultSet {
public:
    RelationDbResultSet(typename Traits::ResultType result,
                       std::function<void(typename Traits::ResultType)> free_func)
        : result_(result), free_func_(free_func), has_next_(true) {}

    ~RelationDbResultSet() override {
        if (result_ && free_func_) {
            free_func_(result_);
        }
    }

    int FieldCount() override = 0;
    const char* FieldName(int index) override = 0;
    int FieldType(int index) override = 0;
    int FieldLength(int index) override = 0;
    bool HasNext() override { return has_next_; }
    bool Next() override = 0;
    int GetInt(int index, int* value) override = 0;
    int GetInt64(int index, int64_t* value) override = 0;
    int GetDouble(int index, double* value) override = 0;
    int GetString(int index, const char** value, size_t* len) override = 0;
    bool IsNull(int index) override = 0;

protected:
    typename Traits::ResultType result_;
    std::function<void(typename Traits::ResultType)> free_func_;
    bool has_next_;
};

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

    // IDbSession 实现
    int ExecuteQuery(const char* sql, IResultSet** result, std::string* error) override {
        auto stmt = PrepareStatement(conn_, sql, error);
        if (!stmt) {
            return -1;
        }

        if (ExecuteStatement(stmt, error) != 0) {
            FreeStatement(stmt);
            return -1;
        }

        auto res = GetResultMetadata(stmt, error);
        if (!res) {
            FreeStatement(stmt);
            return -1;
        }

        // 创建结果集（由子类实现具体的 CreateResultSet）
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

    int ExecuteSql(const char* sql, std::string* error) override {
        auto stmt = PrepareStatement(conn_, sql, error);
        if (!stmt) {
            return -1;
        }

        int ret = ExecuteStatement(stmt, error);
        int affected_rows = GetAffectedRows(stmt);

        FreeStatement(stmt);
        return (ret == 0) ? affected_rows : -1;
    }

    int BeginTransaction(std::string* error) override {
        if (in_transaction_) {
            if (error) *error = "Transaction already in progress";
            return -1;
        }
        int ret = ExecuteSql("BEGIN", error);
        if (ret == 0) {
            in_transaction_ = true;
        }
        return ret;
    }

    int CommitTransaction(std::string* error) override {
        if (!in_transaction_) {
            if (error) *error = "No transaction in progress";
            return -1;
        }
        int ret = ExecuteSql("COMMIT", error);
        if (ret == 0) {
            in_transaction_ = false;
        }
        return ret;
    }

    int RollbackTransaction(std::string* error) override {
        if (!in_transaction_) {
            if (error) *error = "No transaction in progress";
            return -1;
        }
        int ret = ExecuteSql("ROLLBACK", error);
        if (ret == 0) {
            in_transaction_ = false;
        }
        return ret;
    }

    bool Ping() override {
        return PingImpl(conn_);
    }

    // IBatchReadable 实现
    int CreateReader(const char* query, IBatchReader** reader) override {
        std::string error;
        IResultSet* result = nullptr;
        if (ExecuteQuery(query, &result, &error) != 0) {
            return -1;
        }

        // 推断 Schema
        auto schema = InferSchema(result, &error);
        if (!schema) {
            delete result;
            return -1;
        }

        // 创建批量读取器
        *reader = CreateBatchReader(result, schema);
        return (*reader) ? 0 : -1;
    }

    // IBatchWritable 实现
    int CreateWriter(const char* table, IBatchWriter** writer) override {
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

    // 推断 Schema
    virtual std::shared_ptr<arrow::Schema> InferSchema(IResultSet* result,
                                                       std::string* error) {
        // 使用 Arrow 的类型推断
        std::vector<std::shared_ptr<arrow::Field>> fields;
        int field_count = result->FieldCount();

        for (int i = 0; i < field_count; ++i) {
            const char* name = result->FieldName(i);
            int type = result->FieldType(i);

            // 根据数据库类型映射到 Arrow 类型
            std::shared_ptr<arrow::DataType> arrow_type;
            switch (type) {
                case 1:  // 假设 1 = INT
                    arrow_type = arrow::int32();
                    break;
                case 2:  // 假设 2 = BIGINT
                    arrow_type = arrow::int64();
                    break;
                case 3:  // 假设 3 = DOUBLE
                    arrow_type = arrow::float64();
                    break;
                case 4:  // 假设 4 = STRING
                    arrow_type = arrow::utf8();
                    break;
                default:
                    arrow_type = arrow::utf8();  // 默认为字符串
            }

            fields.push_back(arrow::field(name, arrow_type));
        }

        return arrow::schema(fields);
    }

protected:
    IDbDriver* driver_;
    typename Traits::ConnectionType conn_;
    bool in_transaction_;
};

}  // namespace database
}  // namespace flowsql

#endif  // _FLOWSQL_SERVICES_DATABASE_RELATION_DB_SESSION_H_
