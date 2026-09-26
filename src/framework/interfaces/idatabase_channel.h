// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_IDATABASE_CHANNEL_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_IDATABASE_CHANNEL_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ichannel.h"

// Arrow 前向声明
namespace arrow {
class RecordBatch;
class Schema;
}

namespace flowsql {

// {0xf3a4b5c6-def0-1234-5678-9abcdef01234}
const Guid IID_DATABASE_CHANNEL = {0xf3a4b5c6, 0xdef0, 0x1234, {0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0, 0x12, 0x34}};

// 写入统计信息
struct BatchWriteStats {
    int64_t rows_written = 0;
    int64_t bytes_written = 0;
    int64_t elapsed_ms = 0;
};

enum class DatabaseParameterKindV1 : uint8_t {
    kNull = 0,
    kInt64,
    kUInt64,
    kDouble,
    kString,
    kBlob,
};

/** Borrowed value for one ExecutePrepared call. */
struct DatabaseParameterV1 {
    DatabaseParameterKindV1 kind = DatabaseParameterKindV1::kNull;
    int64_t int64_value = 0;
    uint64_t uint64_value = 0;
    double double_value = 0.0;
    const void* data = nullptr;
    size_t size = 0;
};

/** Optional parameterized-command capability; separate from the existing IDatabaseChannel ABI. */
interface IDatabasePreparedCommandV1 {
    virtual ~IDatabasePreparedCommandV1() = default;
    virtual int ExecutePrepared(const char* sql, const DatabaseParameterV1* parameters, size_t parameter_count) = 0;
    virtual int ExecutePreparedBatch(const char* sql, const DatabaseParameterV1* parameters,
                                     size_t parameters_per_execution, size_t execution_count) = 0;
};

/**
 * @brief 行式批量读取器接口。
 *
 * 生命周期：CreateReader() -> GetSchema() -> Next()... -> Close() -> Release()。
 */
interface IBatchReader {
    virtual ~IBatchReader() = default;

    /**
     * @brief 获取结果集 schema（Arrow Schema IPC 序列化）。
     * @param buf 输出 buffer 地址。
     * @param len 输出 buffer 长度。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int GetSchema(const uint8_t** buf, size_t* len) = 0;

    /**
     * @brief 读取下一批数据（Arrow RecordBatch IPC 序列化）。
     * @param buf 输出 buffer 地址。
     * @param len 输出 buffer 长度。
     * @return 0 表示有数据，1 表示已读完，<0 表示错误。
     */
    virtual int Next(const uint8_t** buf, size_t* len) = 0;

    /**
     * @brief 取消进行中的读取操作。
     */
    virtual void Cancel() = 0;

    /**
     * @brief 关闭读取器，释放底层游标资源。
     */
    virtual void Close() = 0;

    /**
     * @brief 获取最近一次错误信息。
     * @return 错误字符串指针。
     */
    virtual const char* GetLastError() = 0;

    /**
     * @brief 释放读取器对象自身。
     */
    virtual void Release() = 0;
};

/**
 * @brief 行式批量写入器接口。
 *
 * 生命周期：CreateWriter() -> Write()... -> Flush() -> Close() -> Release()。
 */
interface IBatchWriter {
    virtual ~IBatchWriter() = default;

    /**
     * @brief 写入一批数据（Arrow RecordBatch IPC 序列化）。
     * @param buf 输入 buffer 地址。
     * @param len 输入 buffer 长度。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int Write(const uint8_t* buf, size_t len) = 0;

    /**
     * @brief 强制刷新缓冲区到数据库。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int Flush() = 0;

    /**
     * @brief 关闭写入器并返回统计信息。
     * @param stats 输出统计信息，可为 nullptr。
     */
    virtual void Close(BatchWriteStats* stats) = 0;

    /**
     * @brief 获取最近一次错误信息。
     * @return 错误字符串指针。
     */
    virtual const char* GetLastError() = 0;

    /**
     * @brief 释放写入器对象自身。
     */
    virtual void Release() = 0;
};

/**
 * @brief Arrow 原生读取器接口。
 */
interface IArrowReader {
    virtual ~IArrowReader() = default;

    /**
     * @brief 执行查询并读取 Arrow RecordBatch 列表。
     * @param query 查询 SQL。
     * @param batches 输出批次数组。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int ExecuteQueryArrow(const char* query,
                                  std::vector<std::shared_ptr<arrow::RecordBatch>>* batches) = 0;

    /**
     * @brief 获取结果 schema。
     * @return Arrow Schema 指针。
     */
    virtual std::shared_ptr<arrow::Schema> GetSchema() = 0;

    /**
     * @brief 获取最近一次错误信息。
     * @return 错误字符串指针。
     */
    virtual const char* GetLastError() = 0;

    /**
     * @brief 释放读取器对象自身。
     */
    virtual void Release() = 0;
};

/**
 * @brief Arrow 原生写入器接口。
 */
interface IArrowWriter {
    virtual ~IArrowWriter() = default;

    /**
     * @brief 直接写入 Arrow RecordBatch 列表。
     * @param table 目标表名。
     * @param batches 输入批次数组。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int WriteBatches(const char* table,
                            const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches) = 0;

    /**
     * @brief 获取最近一次错误信息。
     * @return 错误字符串指针。
     */
    virtual const char* GetLastError() = 0;

    /**
     * @brief 释放写入器对象自身。
     */
    virtual void Release() = 0;
};

/**
 * @brief 数据库通道接口，封装连接、读写器创建与 SQL 执行能力。
 */
interface IDatabaseChannel : public IChannel {
    // ==================== 行式数据库接口 ====================

    /**
     * @brief 创建行式读取器并执行查询。
     * @param query 查询 SQL。
     * @param reader 输出读取器指针。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int CreateReader(const char* query, IBatchReader** reader) = 0;

    /**
     * @brief 创建行式写入器。
     * @param table 目标表名。
     * @param writer 输出写入器指针。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int CreateWriter(const char* table, IBatchWriter** writer) = 0;

    // ==================== 列式数据库接口（Arrow 原生） ====================

    /**
     * @brief 创建 Arrow 读取器并执行查询。
     * @param query 查询 SQL。
     * @param reader 输出 Arrow 读取器指针。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int CreateArrowReader(const char* query, IArrowReader** reader) = 0;

    /**
     * @brief 创建 Arrow 写入器。
     * @param table 目标表名。
     * @param writer 输出 Arrow 写入器指针。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int CreateArrowWriter(const char* table, IArrowWriter** writer) = 0;

    /**
     * @brief 便捷执行 Arrow 查询。
     * @param query 查询 SQL。
     * @param batches 输出批次数组。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int ExecuteQueryArrow(const char* query,
                                  std::vector<std::shared_ptr<arrow::RecordBatch>>* batches) = 0;

    /**
     * @brief 便捷写入 Arrow 批次数组。
     * @param table 目标表名。
     * @param batches 输入批次数组。
     * @return 0 表示成功，非 0 表示失败。
     */
    virtual int WriteArrowBatches(const char* table,
                                  const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches) = 0;

    // ==================== 通用接口 ====================

    /**
     * @brief 执行无结果集 SQL（DDL/DML）。
     * @param sql 输入 SQL。
     * @return 0 或受影响行数表示成功，<0 表示失败（由实现定义）。
     */
    virtual int ExecuteSql(const char* sql) = 0;

    /**
     * @brief 获取最近一次错误信息。
     * @return 错误字符串指针。
     */
    virtual const char* GetLastError() = 0;

    /**
     * @brief 测试连接是否可用。
     * @return true 表示可用，false 表示不可用。
     */
    virtual bool IsConnected() = 0;
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_IDATABASE_CHANNEL_H_
