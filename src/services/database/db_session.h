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
//
// 所有权说明：IResultSet 由 ExecuteQuery 的调用方负责释放（delete）。
// 在 RelationDbSessionBase 中，IResultSet 被传入 IBatchReader，由 IBatchReader 持有并在
// Release() 时释放。直接调用 ExecuteQuery 的代码需手动 delete 返回的 IResultSet。
interface IResultSet {
    virtual ~IResultSet() = default;

    // 获取字段数
    virtual int FieldCount() = 0;

    // 获取字段名
    virtual const char* FieldName(int index) const = 0;

    // 获取字段类型
    virtual int FieldType(int index) const = 0;

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
// 继承 enable_shared_from_this 以支持在成员方法中安全获取 shared_ptr<IDbSession>
class IDbSession : public std::enable_shared_from_this<IDbSession> {
public:
    virtual ~IDbSession() = default;

    // 获取最近一次操作的错误信息
    virtual const char* GetLastError() { return last_error_.c_str(); }

    // ==================== 行式数据库接口 ====================

    // 执行查询，返回结果集（返回 0 成功，-1 失败）
    virtual int ExecuteQuery(const char* sql, IResultSet** result) {
        last_error_ = "ExecuteQuery not supported";
        return -1;
    }

    // 执行 SQL（INSERT/UPDATE/DELETE 等），返回受影响行数，-1 失败
    virtual int ExecuteSql(const char* sql) {
        last_error_ = "ExecuteSql not supported";
        return -1;
    }

    // ==================== 列式数据库接口 ====================

    // 执行 Arrow 查询，返回 RecordBatch 列表
    virtual int ExecuteQueryArrow(const char* sql,
                                  std::vector<std::shared_ptr<arrow::RecordBatch>>* batches) {
        last_error_ = "ExecuteQueryArrow not supported";
        return -1;
    }

    // 写入 Arrow RecordBatches
    virtual int WriteArrowBatches(const char* table,
                                  const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches) {
        last_error_ = "WriteArrowBatches not supported";
        return -1;
    }

    // ==================== 事务控制 ====================
    // 某些列式数据库（如 ClickHouse）不支持事务，默认实现为空操作

    virtual int BeginTransaction() { return 0; }
    virtual int CommitTransaction() { return 0; }
    virtual int RollbackTransaction() { return 0; }

    // ==================== 健康检查 ====================

    virtual bool Ping() = 0;

protected:
    std::string last_error_;  // 所有子类共享的错误存储
};

// 工厂函数类型：创建 Session
using SessionFactory = std::function<std::shared_ptr<IDbSession>()>;

}  // namespace database
}  // namespace flowsql

#endif  // _FLOWSQL_SERVICES_DATABASE_DB_SESSION_H_
