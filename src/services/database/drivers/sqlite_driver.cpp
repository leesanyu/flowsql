#include "sqlite_driver.h"

#include <arrow/api.h>

#include <cstdio>

namespace flowsql {
namespace database {

SqliteDriver::~SqliteDriver() {
    if (db_) Disconnect();
}

int SqliteDriver::Connect(const std::unordered_map<std::string, std::string>& params) {
    if (db_) return 0;  // 已连接

    auto it = params.find("path");
    db_path_ = (it != params.end()) ? it->second : ":memory:";

    // 使用 FULLMUTEX 模式，保证多线程安全
    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;

    // 支持只读模式
    auto ro_it = params.find("readonly");
    if (ro_it != params.end() && ro_it->second == "true") {
        flags = SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX;
        readonly_ = true;
    }

    int rc = sqlite3_open_v2(db_path_.c_str(), &db_, flags, nullptr);
    if (rc != SQLITE_OK) {
        last_error_ = db_ ? sqlite3_errmsg(db_) : "sqlite3_open_v2 failed";
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        return -1;
    }

    // 开启 WAL 模式，提升并发读写性能
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    printf("SqliteDriver: connected to %s\n", db_path_.c_str());
    return 0;
}

int SqliteDriver::Disconnect() {
    if (!db_) return 0;
    sqlite3_close(db_);
    db_ = nullptr;
    printf("SqliteDriver: disconnected from %s\n", db_path_.c_str());
    return 0;
}

// 钩子方法实现
void* SqliteDriver::ExecuteQueryImpl(const char* sql, std::string* error) {
    if (!db_) {
        *error = "database not connected";
        return nullptr;
    }

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        *error = sqlite3_errmsg(db_);
        return nullptr;
    }

    return stmt;
}

std::shared_ptr<arrow::Schema> SqliteDriver::InferSchemaImpl(void* result, std::string* error) {
    auto* stmt = static_cast<sqlite3_stmt*>(result);
    if (!stmt) {
        *error = "invalid statement";
        return nullptr;
    }

    int ncols = sqlite3_column_count(stmt);
    std::vector<std::shared_ptr<arrow::Field>> fields;

    for (int i = 0; i < ncols; ++i) {
        std::string col_name = sqlite3_column_name(stmt, i);
        std::shared_ptr<arrow::DataType> arrow_type;

        const char* decl = sqlite3_column_decltype(stmt, i);
        if (decl) {
            std::string dtype(decl);
            // 转大写比较
            for (auto& c : dtype) c = std::toupper(c);
            if (dtype.find("INT") != std::string::npos)
                arrow_type = arrow::int64();
            else if (dtype.find("REAL") != std::string::npos || dtype.find("FLOAT") != std::string::npos ||
                     dtype.find("DOUBLE") != std::string::npos)
                arrow_type = arrow::float64();
            else if (dtype.find("BLOB") != std::string::npos)
                arrow_type = arrow::binary();
            else
                arrow_type = arrow::utf8();
        } else {
            // decltype 为空（表达式列等），默认 utf8
            arrow_type = arrow::utf8();
        }
        fields.push_back(arrow::field(col_name, arrow_type));
    }

    return arrow::schema(fields);
}

int SqliteDriver::FetchRowImpl(void* result,
                               const std::vector<std::unique_ptr<arrow::ArrayBuilder>>& builders,
                               std::string* error) {
    auto* stmt = static_cast<sqlite3_stmt*>(result);
    if (!stmt) {
        *error = "invalid statement";
        return -1;
    }

    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
        return 0;  // 没有更多行
    }
    if (rc != SQLITE_ROW) {
        *error = sqlite3_errmsg(sqlite3_db_handle(stmt));
        return -1;
    }

    // 逐列读取值并追加到 builder
    int ncols = builders.size();
    for (int col = 0; col < ncols; ++col) {
        int col_type = sqlite3_column_type(stmt, col);
        auto* builder = builders[col].get();

        if (col_type == SQLITE_NULL) {
            if (!builder->AppendNull().ok()) {
                *error = "failed to append null";
                return -1;
            }
            continue;
        }

        // 根据 builder 类型写入
        auto arrow_type = builder->type();
        if (arrow_type->id() == arrow::Type::INT64) {
            auto status = static_cast<arrow::Int64Builder*>(builder)->Append(sqlite3_column_int64(stmt, col));
            if (!status.ok()) {
                *error = "failed to append int64: " + status.ToString();
                return -1;
            }
        } else if (arrow_type->id() == arrow::Type::DOUBLE) {
            auto status = static_cast<arrow::DoubleBuilder*>(builder)->Append(sqlite3_column_double(stmt, col));
            if (!status.ok()) {
                *error = "failed to append double: " + status.ToString();
                return -1;
            }
        } else if (arrow_type->id() == arrow::Type::STRING) {
            auto text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
            auto status = static_cast<arrow::StringBuilder*>(builder)->Append(text ? text : "");
            if (!status.ok()) {
                *error = "failed to append string: " + status.ToString();
                return -1;
            }
        } else if (arrow_type->id() == arrow::Type::BINARY) {
            auto blob = sqlite3_column_blob(stmt, col);
            int blob_len = sqlite3_column_bytes(stmt, col);
            auto status = static_cast<arrow::BinaryBuilder*>(builder)->Append(
                reinterpret_cast<const uint8_t*>(blob), blob_len);
            if (!status.ok()) {
                *error = "failed to append binary: " + status.ToString();
                return -1;
            }
        } else {
            // 默认当字符串处理
            auto text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
            auto status = static_cast<arrow::StringBuilder*>(builder)->Append(text ? text : "");
            if (!status.ok()) {
                *error = "failed to append string: " + status.ToString();
                return -1;
            }
        }
    }

    return 1;  // 成功读取一行
}

void SqliteDriver::FreeResultImpl(void* result) {
    auto* stmt = static_cast<sqlite3_stmt*>(result);
    if (stmt) {
        sqlite3_finalize(stmt);
    }
}

int SqliteDriver::ExecuteSqlImpl(const char* sql, std::string* error) {
    if (!db_) {
        *error = "database not connected";
        return -1;
    }

    char* errmsg = nullptr;
    int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        *error = errmsg ? errmsg : "sqlite3_exec failed";
        if (errmsg) sqlite3_free(errmsg);
        return -1;
    }
    return 0;
}

}  // namespace database
}  // namespace flowsql
