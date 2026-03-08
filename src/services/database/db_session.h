#ifndef _FLOWSQL_SERVICES_DATABASE_DB_SESSION_H_
#define _FLOWSQL_SERVICES_DATABASE_DB_SESSION_H_

#include "idb_driver.h"
#include "capability_interfaces.h"

#include <arrow/api.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace arrow {
class RecordBatch;
class Schema;
}

namespace flowsql {
namespace database {

// IResultSet — 查询结果集接口
// 封装数据库查询结果，提供行迭代和元数据访问
interface IResultSet {
    virtual ~IResultSet() = default;

    // 获取字段数
    virtual int FieldCount() = 0;

    // 获取字段名
    virtual const char* FieldName(int index) = 0;

    // 获取字段类型
    virtual int FieldType(int index) = 0;

    // 获取字段长度
    virtual int FieldLength(int index) = 0;

    // 判断是否还有下一行
    virtual bool HasNext() = 0;

    // 移动到下一行，返回 true 表示成功
    virtual bool Next() = 0;

    // 获取当前行的整数值
    virtual int GetInt(int index, int* value) = 0;

    // 获取当前行的 64 位整数值
    virtual int GetInt64(int index, int64_t* value) = 0;

    // 获取当前行的双精度浮点数值
    virtual int GetDouble(int index, double* value) = 0;

    // 获取当前行的字符串值
    virtual int GetString(int index, const char** value, size_t* len) = 0;

    // 判断当前行的指定列是否为 NULL
    virtual bool IsNull(int index) = 0;
};

// IDbSession — 数据库会话接口
// 封装单个数据库连接，提供查询执行、事务管理和健康检查
interface IDbSession {
    virtual ~IDbSession() = default;

    // ==================== 行式数据库接口 ====================

    // 执行查询，返回结果集
    // 返回 0 表示成功，-1 表示失败（error 会包含错误信息）
    virtual int ExecuteQuery(const char* sql, IResultSet** result, std::string* error) {
        // 默认返回 -1，表示不支持
        if (error) *error = "ExecuteQuery not supported";
        return -1;
    }

    // 执行 SQL（INSERT/UPDATE/DELETE 等），返回受影响的行数
    // 返回 -1 表示失败
    virtual int ExecuteSql(const char* sql, std::string* error) {
        // 默认返回 -1，表示不支持
        if (error) *error = "ExecuteSql not supported";
        return -1;
    }

    // ==================== 列式数据库接口（未来扩展） ====================

    // 执行 Arrow 查询，返回 RecordBatch 列表
    virtual int ExecuteQueryArrow(const char* sql,
                                  std::vector<std::shared_ptr<arrow::RecordBatch>>* batches,
                                  std::string* error) {
        // 默认返回 -1，表示不支持
        if (error) *error = "ExecuteQueryArrow not supported";
        return -1;
    }

    // 写入 Arrow RecordBatches
    virtual int WriteArrowBatches(const char* table,
                                  const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches,
                                  std::string* error) {
        // 默认返回 -1，表示不支持
        if (error) *error = "WriteArrowBatches not supported";
        return -1;
    }

    // ==================== 事务控制 ====================
    // 注意：某些列式数据库（如 ClickHouse）不支持事务，可返回 0（空实现）

    // 开始事务
    virtual int BeginTransaction(std::string* error) {
        if (error) *error = "BeginTransaction not supported";
        return 0;  // 默认空实现
    }

    // 提交事务
    virtual int CommitTransaction(std::string* error) {
        if (error) *error = "CommitTransaction not supported";
        return 0;  // 默认空实现
    }

    // 回滚事务
    virtual int RollbackTransaction(std::string* error) {
        if (error) *error = "RollbackTransaction not supported";
        return 0;  // 默认空实现
    }

    // ==================== 健康检查 ====================

    // Ping 数据库连接，检查连接是否有效
    virtual bool Ping() = 0;
};

// 工厂函数类型：创建 Session
using SessionFactory = std::function<std::shared_ptr<IDbSession>()>;

}  // namespace database
}  // namespace flowsql

#endif  // _FLOWSQL_SERVICES_DATABASE_DB_SESSION_H_
