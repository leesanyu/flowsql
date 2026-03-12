#ifndef _FLOWSQL_SERVICES_DATABASE_DRIVERS_CLICKHOUSE_DRIVER_H_
#define _FLOWSQL_SERVICES_DATABASE_DRIVERS_CLICKHOUSE_DRIVER_H_

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../db_session.h"
#include "../idb_driver.h"
#include "../capability_interfaces.h"

namespace flowsql {
namespace database {

// ClickHouseDriver — ClickHouse 数据库驱动
// 基于 HTTP 接口（8123 端口），使用 cpp-httplib，零新依赖
// HTTP 无状态，不需要连接池，每次 CreateSession() 创建新 Session
class __attribute__((visibility("default"))) ClickHouseDriver : public IDbDriver {
public:
    ClickHouseDriver() = default;
    ~ClickHouseDriver() override = default;

    // IDbDriver 实现
    int Connect(const std::unordered_map<std::string, std::string>& params) override;
    int Disconnect() override { connected_ = false; return 0; }
    bool IsConnected() override { return connected_; }
    const char* DriverName() override { return "clickhouse"; }
    const char* LastError() override { return last_error_.c_str(); }
    bool Ping() override;

    // 创建 Session（每次返回新实例，HTTP 无状态）
    std::shared_ptr<IDbSession> CreateSession();

private:
    std::string host_;
    int port_ = 8123;
    std::string user_;
    std::string password_;
    std::string database_;
    bool connected_ = false;
    std::string last_error_;
};

// ClickHouseSession — ClickHouse 会话实现
// 直接继承 IDbSession，覆盖 Arrow 方法
// 同时继承 IArrowReadable + IArrowWritable，供 DatabaseChannel::CreateArrowReader/Writer 的 dynamic_cast 检查
// 不继承 RelationDbSessionBase（ClickHouse 是列式数据库，不走行式路径）
class ClickHouseSession : public IDbSession,
                          public IArrowReadable,
                          public IArrowWritable {
public:
    ClickHouseSession(const std::string& host, int port,
                      const std::string& user, const std::string& password,
                      const std::string& database);
    ~ClickHouseSession() override = default;

    // ==================== 列式接口（核心实现）====================

    // 执行 Arrow 查询：构造 "{sql} FORMAT ArrowStream"，POST，解析响应体
    int ExecuteQueryArrow(const char* sql,
                          std::vector<std::shared_ptr<arrow::RecordBatch>>* batches,
                          std::string* error) override;

    // 写入 Arrow batches：序列化为 Arrow IPC Stream，POST INSERT
    int WriteArrowBatches(const char* table,
                          const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches,
                          std::string* error) override;

    // ==================== 行式接口（DDL 等）====================

    // 执行 SQL（DDL/DML），走普通 HTTP 文本响应
    int ExecuteSql(const char* sql, std::string* error) override;

    // 健康检查：GET /?query=SELECT+1
    bool Ping() override;

    // 事务：ClickHouse 不支持，显式覆盖返回 -1
    // 基类默认实现返回 0（空操作），会误导调用方认为事务已成功开启
    int BeginTransaction(std::string* error) override {
        if (error) *error = "ClickHouse does not support transactions";
        return -1;
    }
    int CommitTransaction(std::string* error) override {
        if (error) *error = "ClickHouse does not support transactions";
        return -1;
    }
    int RollbackTransaction(std::string* error) override {
        if (error) *error = "ClickHouse does not support transactions";
        return -1;
    }

private:
    // 解析 Arrow IPC Stream 响应体
    int ParseArrowStream(const std::string& body,
                         std::vector<std::shared_ptr<arrow::RecordBatch>>* batches,
                         std::string* error);

    // 序列化 batches 为 Arrow IPC Stream
    int SerializeArrowStream(const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches,
                             std::string* body, std::string* error);

    std::string host_;
    int port_;
    std::string user_;
    std::string password_;
    std::string database_;
};

}  // namespace database
}  // namespace flowsql

#endif  // _FLOWSQL_SERVICES_DATABASE_DRIVERS_CLICKHOUSE_DRIVER_H_
