#ifndef _FLOWSQL_SERVICES_DATABASE_DRIVERS_MYSQL_DRIVER_H_
#define _FLOWSQL_SERVICES_DATABASE_DRIVERS_MYSQL_DRIVER_H_

#include <mysql/mysql.h>

#include <string>
#include <unordered_map>

#include "../row_based_db_driver_base.h"

namespace flowsql {
namespace database {

// MysqlDriver — MySQL 数据库驱动
// 基于 libmysqlclient，支持预编译语句和事务
class __attribute__((visibility("default"))) MysqlDriver : public RowBasedDbDriverBase {
 public:
    MysqlDriver() = default;
    ~MysqlDriver() override;

    // IDbDriver 实现
    int Connect(const std::unordered_map<std::string, std::string>& params) override;
    int Disconnect() override;
    bool IsConnected() override { return conn_ != nullptr; }
    const char* DriverName() override { return "mysql"; }

 protected:
    // 钩子方法实现
    void* ExecuteQueryImpl(const char* sql, std::string* error) override;
    std::shared_ptr<arrow::Schema> InferSchemaImpl(void* result, std::string* error) override;
    int FetchRowImpl(void* result,
                    const std::vector<std::unique_ptr<arrow::ArrayBuilder>>& builders,
                    std::string* error) override;
    void FreeResultImpl(void* result) override;
    int ExecuteSqlImpl(const char* sql, std::string* error) override;

 private:
    MYSQL* conn_ = nullptr;
    std::string host_;
    int port_ = 3306;
    std::string user_;
    std::string password_;
    std::string database_;
    std::string charset_ = "utf8mb4";
    int timeout_ = 10;
};

}  // namespace database
}  // namespace flowsql

#endif  // _FLOWSQL_SERVICES_DATABASE_DRIVERS_MYSQL_DRIVER_H_
