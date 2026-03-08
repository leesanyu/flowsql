#ifndef _FLOWSQL_SERVICES_DATABASE_DRIVERS_SQLITE_DRIVER_H_
#define _FLOWSQL_SERVICES_DATABASE_DRIVERS_SQLITE_DRIVER_H_

#include <sqlite3.h>

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

// SQLite Traits
struct SqliteTraits {
    using ConnectionType = sqlite3*;
    using StatementType = sqlite3_stmt*;
    using ResultType = sqlite3_stmt*;
    using BindType = void*;
};

// 前向声明
class SqliteSession;
class SqliteResultSet;

// SqliteDriver — SQLite 数据库驱动
// 使用 FULLMUTEX 模式保证多线程安全，WAL 模式提升并发读写性能
class __attribute__((visibility("default"))) SqliteDriver : public IDbDriver {
 public:
    SqliteDriver() = default;
    ~SqliteDriver() override;

    // IDbDriver 实现
    int Connect(const std::unordered_map<std::string, std::string>& params) override;
    int Disconnect() override;
    bool IsConnected() override { return pool_ != nullptr; }
    const char* DriverName() override { return "sqlite"; }
    const char* LastError() override { return last_error_.c_str(); }
    bool Ping() override;

    // 连接池相关
    std::shared_ptr<IDbSession> CreateSession();
    void ReturnToPool(sqlite3* db);

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
    std::unique_ptr<ConnectionPool<sqlite3*>> pool_;

    std::string db_path_;
    bool readonly_ = false;
    std::string last_error_;
};

// SQLite 结果集实现
class SqliteResultSet : public IResultSet {
public:
    SqliteResultSet(sqlite3_stmt* stmt, std::function<void(sqlite3_stmt*)> free_func);
    ~SqliteResultSet() override;

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
    sqlite3_stmt* stmt_;
    std::function<void(sqlite3_stmt*)> free_func_;
    bool has_next_;
    bool fetched_;
};

// SQLite Session 实现
class SqliteSession : public RelationDbSessionBase<SqliteTraits> {
public:
    SqliteSession(SqliteDriver* driver, sqlite3* db);
    ~SqliteSession() override;

protected:
    // 钩子方法实现
    sqlite3_stmt* PrepareStatement(sqlite3* db, const char* sql, std::string* error) override;
    int ExecuteStatement(sqlite3_stmt* stmt, std::string* error) override;
    sqlite3_stmt* GetResultMetadata(sqlite3_stmt* stmt, std::string* error) override;
    std::string GetDriverError(sqlite3* db) override;
    void FreeResult(sqlite3_stmt* result) override;
    void FreeStatement(sqlite3_stmt* stmt) override;
    bool PingImpl(sqlite3* db) override;
    void ReturnConnection(sqlite3* db) override;

    // 工厂方法
    IResultSet* CreateResultSet(sqlite3_stmt* stmt,
                                std::function<void(sqlite3_stmt*)> free_func) override;
    IBatchReader* CreateBatchReader(IResultSet* result,
                                    std::shared_ptr<arrow::Schema> schema) override;
    IBatchWriter* CreateBatchWriter(const char* table) override;
};

}  // namespace database
}  // namespace flowsql

#endif  // _FLOWSQL_SERVICES_DATABASE_DRIVERS_SQLITE_DRIVER_H_
