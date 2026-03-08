#ifndef _FLOWSQL_SERVICES_DATABASE_DRIVERS_MYSQL_DRIVER_H_
#define _FLOWSQL_SERVICES_DATABASE_DRIVERS_MYSQL_DRIVER_H_

#include <mysql/mysql.h>

#include <functional>
#include <string>
#include <unordered_map>
#include <memory>

#include "../row_based_db_driver_base.h"
#include "../connection_pool.h"
#include "../db_session.h"
#include "../relation_db_session.h"

namespace flowsql {
namespace database {

// MySQL Traits
struct MysqlTraits {
    using ConnectionType = MYSQL*;
    using StatementType = MYSQL_STMT*;
    using ResultType = MYSQL_RES*;
    using BindType = MYSQL_BIND*;
};

// 前向声明
class MysqlSession;

// MysqlDriver — MySQL 数据库驱动
// 基于 libmysqlclient，支持预编译语句和事务
class __attribute__((visibility("default"))) MysqlDriver : public IDbDriver {
 public:
    MysqlDriver() = default;
    ~MysqlDriver() override;

    // IDbDriver 实现
    int Connect(const std::unordered_map<std::string, std::string>& params) override;
    int Disconnect() override;
    bool IsConnected() override { return pool_ != nullptr; }
    const char* DriverName() override { return "mysql"; }
    const char* LastError() override { return last_error_.c_str(); }
    bool Ping() override;

    // 连接池相关
    std::shared_ptr<IDbSession> CreateSession();
    void ReturnToPool(MYSQL* conn);

    // IBatchReadable 实现（委托给 Session）
    int CreateReader(const char* query, IBatchReader** reader);

    // IBatchWritable 实现（委托给 Session）
    int CreateWriter(const char* table, IBatchWriter** writer);

    // ITransactional 实现
    int BeginTransaction(std::string* error);
    int CommitTransaction(std::string* error);
    int RollbackTransaction(std::string* error);

 private:
    // 连接池
    std::unique_ptr<ConnectionPool<MYSQL*>> pool_;

    // 连接参数
    std::string host_;
    int port_ = 3306;
    std::string user_;
    std::string password_;
    std::string database_;
    std::string charset_ = "utf8mb4";
    int timeout_ = 10;

    std::string last_error_;
};

// MySQL 结果集实现
class MysqlResultSet : public IResultSet {
public:
    MysqlResultSet(MYSQL_RES* result, std::function<void(MYSQL_RES*)> free_func);
    ~MysqlResultSet() override;

    int FieldCount() override;
    const char* FieldName(int index) override;
    int FieldType(int index) override;
    int FieldLength(int index) override;
    bool HasNext() override;
    bool Next() override;
    int GetInt(int index, int* value) override;
    int GetInt64(int index, int64_t* value) override;
    int GetDouble(int index, double* value) override;
    int GetString(int index, const char** value, size_t* len) override;
    bool IsNull(int index) override;

private:
    MYSQL_RES* result_;
    std::function<void(MYSQL_RES*)> free_func_;
    MYSQL_ROW current_row_;
    bool has_next_;
    bool fetched_;
};

// MySQL Session 实现
class MysqlSession : public RelationDbSessionBase<MysqlTraits> {
public:
    MysqlSession(MysqlDriver* driver, MYSQL* conn);
    ~MysqlSession() override;

protected:
    // 钩子方法实现
    MYSQL_STMT* PrepareStatement(MYSQL* conn, const char* sql, std::string* error) override;
    int ExecuteStatement(MYSQL_STMT* stmt, std::string* error) override;
    MYSQL_RES* GetResultMetadata(MYSQL_STMT* stmt, std::string* error) override;
    int GetAffectedRows(MYSQL_STMT* stmt) override;
    std::string GetDriverError(MYSQL* conn) override;
    void FreeResult(MYSQL_RES* result) override;
    void FreeStatement(MYSQL_STMT* stmt) override;
    bool PingImpl(MYSQL* conn) override;
    void ReturnConnection(MYSQL* conn) override;

    // 工厂方法
    IResultSet* CreateResultSet(MYSQL_RES* result,
                                std::function<void(MYSQL_RES*)> free_func) override;
    IBatchReader* CreateBatchReader(IResultSet* result,
                                    std::shared_ptr<arrow::Schema> schema) override;
    IBatchWriter* CreateBatchWriter(const char* table) override;
};

}  // namespace database
}  // namespace flowsql

#endif  // _FLOWSQL_SERVICES_DATABASE_DRIVERS_MYSQL_DRIVER_H_
