// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

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

/**
 * @brief 查询结果集接口，提供行迭代与字段访问能力。
 *
 * 所有权约定：ExecuteQuery 返回的 IResultSet 由调用方负责释放。
 */
interface IResultSet {
    virtual ~IResultSet() = default;

    /**
     * @brief 获取字段数量。
     * @return 字段数。
     */
    virtual int FieldCount() = 0;

    /**
     * @brief 获取字段名。
     * @param index 字段索引（从 0 开始）。
     * @return 字段名称字符串。
     */
    virtual const char* FieldName(int index) const = 0;

    /**
     * @brief 获取字段类型编码。
     * @param index 字段索引（从 0 开始）。
     * @return 字段类型编码（由具体驱动定义）。
     */
    virtual int FieldType(int index) const = 0;

    /**
     * @brief 获取字段长度。
     * @param index 字段索引（从 0 开始）。
     * @return 字段长度。
     */
    virtual int FieldLength(int index) = 0;

    /**
     * @brief 判断是否还有下一行。
     * @return true 表示仍有下一行。
     */
    virtual bool HasNext() = 0;

    /**
     * @brief 移动到下一行。
     * @return true 表示成功移动到下一行。
     */
    virtual bool Next() = 0;

    /**
     * @brief 获取当前行 int 值。
     * @param index 字段索引。
     * @param value 输出值指针。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int GetInt(int index, int* value) = 0;

    /**
     * @brief 获取当前行 int64 值。
     * @param index 字段索引。
     * @param value 输出值指针。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int GetInt64(int index, int64_t* value) = 0;

    /**
     * @brief 获取当前行 double 值。
     * @param index 字段索引。
     * @param value 输出值指针。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int GetDouble(int index, double* value) = 0;

    /**
     * @brief 获取当前行字符串值。
     * @param index 字段索引。
     * @param value 输出字符串地址。
     * @param len 输出字符串长度。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int GetString(int index, const char** value, size_t* len) = 0;

    /**
     * @brief 判断当前行字段是否为 NULL。
     * @param index 字段索引。
     * @return true 表示为 NULL。
     */
    virtual bool IsNull(int index) = 0;
};

// IDbSession — 数据库会话接口
// 封装单个数据库连接，提供查询执行、事务管理和健康检查
// 继承 enable_shared_from_this 以支持在成员方法中安全获取 shared_ptr<IDbSession>
class IDbSession : public std::enable_shared_from_this<IDbSession> {
public:
    virtual ~IDbSession() = default;

    /**
     * @brief 获取最近一次错误信息。
     * @return 错误字符串指针。
     */
    virtual const char* GetLastError() { return last_error_.c_str(); }

    // ==================== 行式数据库接口 ====================

    /**
     * @brief 执行查询 SQL 并返回结果集。
     * @param sql 查询 SQL。
     * @param result 输出结果集对象。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int ExecuteQuery(const char* sql, IResultSet** result) {
        last_error_ = "ExecuteQuery not supported";
        return -1;
    }

    /**
     * @brief 执行无结果集 SQL（INSERT/UPDATE/DELETE 等）。
     * @param sql 输入 SQL。
     * @return 受影响行数或 0 表示成功，<0 表示失败（由实现定义）。
     */
    virtual int ExecuteSql(const char* sql) {
        last_error_ = "ExecuteSql not supported";
        return -1;
    }

    virtual int ExecutePrepared(const char* sql, const DatabaseParameterV1* parameters, size_t parameter_count) {
        (void)sql;
        (void)parameters;
        (void)parameter_count;
        last_error_ = "ExecutePrepared not supported";
        return -1;
    }

    virtual int ExecutePreparedBatch(const char* sql, const DatabaseParameterV1* parameters,
                                     size_t parameters_per_execution, size_t execution_count) {
        (void)sql;
        (void)parameters;
        (void)parameters_per_execution;
        (void)execution_count;
        last_error_ = "ExecutePreparedBatch not supported";
        return -1;
    }

    // ==================== 列式数据库接口 ====================

    /**
     * @brief 执行 Arrow 查询。
     * @param sql 查询 SQL。
     * @param batches 输出 RecordBatch 数组。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int ExecuteQueryArrow(const char* sql,
                                  std::vector<std::shared_ptr<arrow::RecordBatch>>* batches) {
        last_error_ = "ExecuteQueryArrow not supported";
        return -1;
    }

    /**
     * @brief 写入 Arrow 批次数组。
     * @param table 目标表名。
     * @param batches 输入 RecordBatch 数组。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int WriteArrowBatches(const char* table,
                                  const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches) {
        last_error_ = "WriteArrowBatches not supported";
        return -1;
    }

    // ==================== 事务控制 ====================
    // 某些列式数据库（如 ClickHouse）不支持事务，默认实现为空操作

    /**
     * @brief 开启事务。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int BeginTransaction() { return 0; }
    /**
     * @brief 提交事务。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int CommitTransaction() { return 0; }
    /**
     * @brief 回滚事务。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int RollbackTransaction() { return 0; }

    // ==================== 健康检查 ====================

    /**
     * @brief 健康检查。
     * @return true 表示连接可用。
     */
    virtual bool Ping() = 0;

protected:
    std::string last_error_;  // 所有子类共享的错误存储
};

// 工厂函数类型：创建 Session
using SessionFactory = std::function<std::shared_ptr<IDbSession>()>;

}  // namespace database
}  // namespace flowsql

#endif  // _FLOWSQL_SERVICES_DATABASE_DB_SESSION_H_
