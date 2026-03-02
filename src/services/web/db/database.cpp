#include "database.h"

#include <cstdio>

namespace flowsql {
namespace web {

Database::~Database() {
    Close();
}

int Database::Open(const std::string& path) {
    if (db_) Close();
    int rc = sqlite3_open(path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        last_error_ = sqlite3_errmsg(db_);
        printf("Database::Open failed: %s\n", last_error_.c_str());
        sqlite3_close(db_);
        db_ = nullptr;
        return -1;
    }
    // 使用 DELETE 日志模式（兼容性最好）
    sqlite3_exec(db_, "PRAGMA journal_mode=DELETE", nullptr, nullptr, nullptr);
    return 0;
}

void Database::Close() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

int Database::Execute(const std::string& sql) {
    if (!db_) return -1;
    char* err = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        last_error_ = err ? err : "unknown error";
        printf("Database::Execute failed: %s\n", last_error_.c_str());
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

std::vector<Row> Database::Query(const std::string& sql) {
    std::vector<Row> results;
    if (!db_) return results;

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        last_error_ = sqlite3_errmsg(db_);
        return results;
    }

    int col_count = sqlite3_column_count(stmt);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Row row;
        for (int i = 0; i < col_count; ++i) {
            const char* name = sqlite3_column_name(stmt, i);
            const char* val = reinterpret_cast<const char*>(sqlite3_column_text(stmt, i));
            row.emplace_back(name ? name : "", val ? val : "");
        }
        results.push_back(std::move(row));
    }

    sqlite3_finalize(stmt);
    return results;
}

int64_t Database::Insert(const std::string& sql) {
    if (Execute(sql) != 0) return -1;
    return sqlite3_last_insert_rowid(db_);
}

std::vector<Row> Database::QueryParams(const std::string& sql, const std::vector<std::string>& params) {
    std::vector<Row> results;
    if (!db_) return results;

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        last_error_ = sqlite3_errmsg(db_);
        return results;
    }
    for (size_t i = 0; i < params.size(); ++i) {
        sqlite3_bind_text(stmt, static_cast<int>(i + 1), params[i].c_str(), -1, SQLITE_TRANSIENT);
    }

    int col_count = sqlite3_column_count(stmt);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Row row;
        for (int i = 0; i < col_count; ++i) {
            const char* name = sqlite3_column_name(stmt, i);
            const char* val = reinterpret_cast<const char*>(sqlite3_column_text(stmt, i));
            row.emplace_back(name ? name : "", val ? val : "");
        }
        results.push_back(std::move(row));
    }

    sqlite3_finalize(stmt);
    return results;
}

int Database::InitSchema(const std::string& schema_sql) {
    return Execute(schema_sql);
}

int Database::ExecuteParams(const std::string& sql, const std::vector<std::string>& params) {
    if (!db_) return -1;
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        last_error_ = sqlite3_errmsg(db_);
        printf("Database::ExecuteParams prepare failed: %s\n", last_error_.c_str());
        return -1;
    }
    for (size_t i = 0; i < params.size(); ++i) {
        sqlite3_bind_text(stmt, static_cast<int>(i + 1), params[i].c_str(), -1, SQLITE_TRANSIENT);
    }
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        last_error_ = sqlite3_errmsg(db_);
        printf("Database::ExecuteParams step failed: %s\n", last_error_.c_str());
        return -1;
    }
    return 0;
}

int64_t Database::InsertParams(const std::string& sql, const std::vector<std::string>& params) {
    if (ExecuteParams(sql, params) != 0) return -1;
    return sqlite3_last_insert_rowid(db_);
}

}  // namespace web
}  // namespace flowsql
