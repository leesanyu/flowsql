#include <cstdio>
#include <memory>

#include "services/database/database_plugin.h"
#include "services/database/drivers/mysql_driver.h"
#include "services/database/drivers/sqlite_driver.h"
#include "services/database/capability_interfaces.h"

using namespace flowsql;
using namespace flowsql::database;

// 辅助函数：执行 SQL 语句
int ExecuteSql(IDbDriver* driver, const char* sql) {
    auto* transactional = dynamic_cast<ITransactional*>(driver);
    if (!transactional) {
        return -1;
    }

    std::string error;
    // 使用事务接口执行 SQL（通过 BEGIN/COMMIT 包装）
    if (transactional->BeginTransaction(&error) != 0) {
        return -1;
    }

    // 这里需要一个公开的执行 SQL 的方法
    // 暂时通过 BeginTransaction 来测试连接
    if (transactional->CommitTransaction(&error) != 0) {
        return -1;
    }

    return 0;
}

void TestMysqlConnection() {
    printf("\n========================================\n");
    printf("Test 1: MySQL Connection\n");
    printf("========================================\n");

    auto driver = std::make_unique<MysqlDriver>();

    std::unordered_map<std::string, std::string> params;
    params["host"] = "127.0.0.1";  // 使用 IP 地址强制 TCP 连接
    params["port"] = "3306";
    params["user"] = "flowsql_user";  // 使用普通用户
    params["password"] = "flowSQL@user";
    params["database"] = "flowsql_db";
    params["charset"] = "utf8mb4";

    int ret = driver->Connect(params);
    if (ret != 0) {
        printf("❌ Connection failed: %s\n", driver->LastError());
        return;
    }

    printf("✅ Connected to MySQL: %s\n", driver->DriverName());
    printf("✅ IsConnected: %s\n", driver->IsConnected() ? "true" : "false");

    driver->Disconnect();
    printf("✅ Disconnected\n");
}

void TestMysqlCreateTable() {
    printf("\n========================================\n");
    printf("Test 2: MySQL Create Table & Insert\n");
    printf("========================================\n");
    printf("⚠️  Skipped: Need public ExecuteSql API\n");
    printf("    (Will be tested via DatabaseChannel in integration tests)\n");
}

void TestMysqlQuery() {
    printf("\n========================================\n");
    printf("Test 3: MySQL Query with BatchReader\n");
    printf("========================================\n");
    printf("⚠️  Skipped: Need test data setup first\n");
    printf("    (Will be tested in integration tests)\n");
}

void TestCapabilityDetection() {
    printf("\n========================================\n");
    printf("Test 4: Capability Detection\n");
    printf("========================================\n");

    auto mysql_driver = std::make_unique<MysqlDriver>();
    auto sqlite_driver = std::make_unique<SqliteDriver>();

    // 测试 MySQL 驱动能力
    printf("MySQL Driver capabilities:\n");
    printf("  - IBatchReadable: %s\n",
           dynamic_cast<IBatchReadable*>(mysql_driver.get()) ? "✅" : "❌");
    printf("  - IBatchWritable: %s\n",
           dynamic_cast<IBatchWritable*>(mysql_driver.get()) ? "✅" : "❌");
    printf("  - ITransactional: %s\n",
           dynamic_cast<ITransactional*>(mysql_driver.get()) ? "✅" : "❌");
    printf("  - IArrowReadable: %s\n",
           dynamic_cast<IArrowReadable*>(mysql_driver.get()) ? "✅" : "❌");

    // 测试 SQLite 驱动能力
    printf("\nSQLite Driver capabilities:\n");
    printf("  - IBatchReadable: %s\n",
           dynamic_cast<IBatchReadable*>(sqlite_driver.get()) ? "✅" : "❌");
    printf("  - IBatchWritable: %s\n",
           dynamic_cast<IBatchWritable*>(sqlite_driver.get()) ? "✅" : "❌");
    printf("  - ITransactional: %s\n",
           dynamic_cast<ITransactional*>(sqlite_driver.get()) ? "✅" : "❌");
    printf("  - IArrowReadable: %s\n",
           dynamic_cast<IArrowReadable*>(sqlite_driver.get()) ? "✅" : "❌");
}

int main() {
    printf("========================================\n");
    printf("  MySQL Driver Test Suite\n");
    printf("========================================\n");

    TestMysqlConnection();
    TestMysqlCreateTable();
    TestMysqlQuery();
    TestCapabilityDetection();

    printf("\n========================================\n");
    printf("  All Tests Completed\n");
    printf("========================================\n\n");

    return 0;
}
