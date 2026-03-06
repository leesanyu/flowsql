#ifndef _FLOWSQL_SERVICES_DATABASE_DRIVERS_SQLITE_DRIVER_H_
#define _FLOWSQL_SERVICES_DATABASE_DRIVERS_SQLITE_DRIVER_H_

#include <sqlite3.h>

#include <string>
#include <unordered_map>

#include "../row_based_db_driver_base.h"

namespace flowsql {
namespace database {

// SqliteDriver — SQLite 数据库驱动
// 使用 FULLMUTEX 模式保证多线程安全，WAL 模式提升并发读写性能
class __attribute__((visibility("default"))) SqliteDriver : public RowBasedDbDriverBase {
 public:
    SqliteDriver() = default;
    ~SqliteDriver() override;

    // IDbDriver 实现
    int Connect(const std::unordered_map<std::string, std::string>& params) override;
    int Disconnect() override;
    bool IsConnected() override { return db_ != nullptr; }
    const char* DriverName() override { return "sqlite"; }

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
    sqlite3* db_ = nullptr;
    std::string db_path_;
    bool readonly_ = false;
};

}  // namespace database
}  // namespace flowsql

#endif  // _FLOWSQL_SERVICES_DATABASE_DRIVERS_SQLITE_DRIVER_H_
