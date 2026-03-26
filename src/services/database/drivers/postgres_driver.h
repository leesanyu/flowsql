#ifndef _FLOWSQL_SERVICES_DATABASE_DRIVERS_POSTGRES_DRIVER_H_
#define _FLOWSQL_SERVICES_DATABASE_DRIVERS_POSTGRES_DRIVER_H_

#include <postgresql/libpq-fe.h>

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../capability_interfaces.h"
#include "../connection_pool.h"
#include "../db_session.h"
#include "../relation_db_session.h"

namespace flowsql {
namespace database {

struct PostgresTraits {
    using ConnectionType = PGconn*;
    using StatementType = const char*;
    using ResultType = PGresult*;
    using BindType = void*;
};

class PostgresSession;

class __attribute__((visibility("default"))) PostgresDriver : public IDbDriver,
                                                               public IDbSessionFactoryProvider {
 public:
    PostgresDriver() = default;
    ~PostgresDriver() override;

    int Connect(const std::unordered_map<std::string, std::string>& params) override;
    int Disconnect() override;
    bool IsConnected() override { return pool_ != nullptr; }
    const char* DriverName() override { return "postgres"; }
    const char* LastError() override { return last_error_.c_str(); }
    bool Ping() override;

    std::shared_ptr<IDbSession> CreateSession() override;
    void ReturnToPool(PGconn* conn);

 private:
    std::string BuildConninfo() const;

 private:
    std::unique_ptr<ConnectionPool<PGconn*>> pool_;

    std::string host_ = "127.0.0.1";
    int port_ = 5432;
    std::string user_ = "postgres";
    std::string password_;
    std::string database_ = "postgres";
    int timeout_ = 10;
    std::string last_error_;
};

class PostgresResultSet : public IResultSet {
 public:
    PostgresResultSet(PGresult* result, std::function<void(PGresult*)> free_func);
    ~PostgresResultSet() override;

    int FieldCount() override;
    const char* FieldName(int index) const override;
    int FieldType(int index) const override;
    int FieldLength(int index) override;
    bool HasNext() override;
    bool Next() override;
    int GetInt(int index, int* value) override;
    int GetInt64(int index, int64_t* value) override;
    int GetDouble(int index, double* value) override;
    int GetString(int index, const char** value, size_t* len) override;
    bool IsNull(int index) override;

 private:
    int DecodeBytea(int index, const char** value, size_t* len);
    static int HexValue(char c);

 private:
    PGresult* result_;
    std::function<void(PGresult*)> free_func_;
    int current_row_ = -1;
    bool fetched_ = false;
    std::vector<std::string> bytea_cache_;
};

class PostgresSession : public RelationDbSessionBase<PostgresTraits> {
 public:
    PostgresSession(PostgresDriver* driver, PGconn* conn);
    ~PostgresSession() override;

    int ExecuteQuery(const char* sql, IResultSet** result) override;
    int ExecuteSql(const char* sql) override;

 protected:
    const char* PrepareStatement(PGconn* conn, const char* sql, std::string* error) override;
    int ExecuteStatement(const char* stmt_name, std::string* error) override;
    PGresult* GetResultMetadata(const char* stmt_name, std::string* error) override;
    int GetAffectedRows(const char* stmt_name) override;
    std::string GetDriverError(PGconn* conn) override;
    void FreeResult(PGresult* result) override;
    void FreeStatement(const char* stmt_name) override;
    bool PingImpl(PGconn* conn) override;
    void ReturnConnection(PGconn* conn) override;

    IResultSet* CreateResultSet(PGresult* result,
                                std::function<void(PGresult*)> free_func) override;
    IBatchReader* CreateBatchReader(IResultSet* result,
                                    std::shared_ptr<arrow::Schema> schema) override;
    IBatchWriter* CreateBatchWriter(const char* table) override;
    std::shared_ptr<arrow::Schema> InferSchema(IResultSet* result, std::string* error) override;
};

}  // namespace database
}  // namespace flowsql

#endif  // _FLOWSQL_SERVICES_DATABASE_DRIVERS_POSTGRES_DRIVER_H_
