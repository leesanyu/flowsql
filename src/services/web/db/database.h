#ifndef _FLOWSQL_WEB_DB_DATABASE_H_
#define _FLOWSQL_WEB_DB_DATABASE_H_

#include <sqlite3.h>

#include <functional>
#include <string>
#include <vector>

namespace flowsql {
namespace web {

// 查询结果行：列名→值 的有序对
using Row = std::vector<std::pair<std::string, std::string>>;

// SQLite 数据库封装
class Database {
 public:
    Database() = default;
    ~Database();

    // 禁止拷贝（持有 sqlite3* 裸指针，拷贝会 double-close，问题 9）
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // 打开数据库（文件路径，":memory:" 为内存库）
    int Open(const std::string& path);
    void Close();

    // 执行 DDL/DML（无返回值）
    int Execute(const std::string& sql);

    // 查询，返回结果行
    std::vector<Row> Query(const std::string& sql);

    // 参数化查询，返回结果行（防 SQL 注入）
    std::vector<Row> QueryParams(const std::string& sql, const std::vector<std::string>& params);

    // 插入并返回 last_insert_rowid
    int64_t Insert(const std::string& sql);

    // 参数化执行（防 SQL 注入）
    int ExecuteParams(const std::string& sql, const std::vector<std::string>& params);
    int64_t InsertParams(const std::string& sql, const std::vector<std::string>& params);

    // 初始化 schema（执行建表 SQL）
    int InitSchema(const std::string& schema_sql);

    const std::string& LastError() const { return last_error_; }

 private:
    sqlite3* db_ = nullptr;
    std::string last_error_;
};

}  // namespace web
}  // namespace flowsql

#endif  // _FLOWSQL_WEB_DB_DATABASE_H_
