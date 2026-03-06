#include "mysql_driver.h"

#include <arrow/api.h>

#include <cstdio>
#include <cstring>

namespace flowsql {
namespace database {

// MySQL 结果集包装（用于 void* 类型擦除）
struct MysqlResult {
    MYSQL_STMT* stmt = nullptr;
    MYSQL_RES* metadata = nullptr;
    MYSQL_BIND* binds = nullptr;
    unsigned long* lengths = nullptr;
    bool* is_nulls = nullptr;  // MySQL 8.0+ 使用 bool 替代 my_bool
    bool* errors = nullptr;
    int num_fields = 0;
};

MysqlDriver::~MysqlDriver() {
    if (conn_) Disconnect();
}

int MysqlDriver::Connect(const std::unordered_map<std::string, std::string>& params) {
    if (conn_) return 0;  // 已连接

    // 解析连接参数
    auto it = params.find("host");
    host_ = (it != params.end()) ? it->second : "localhost";

    it = params.find("port");
    if (it != params.end()) {
        port_ = std::stoi(it->second);
    }

    it = params.find("user");
    user_ = (it != params.end()) ? it->second : "root";

    it = params.find("password");
    password_ = (it != params.end()) ? it->second : "";

    it = params.find("database");
    database_ = (it != params.end()) ? it->second : "";

    it = params.find("charset");
    if (it != params.end()) {
        charset_ = it->second;
    }

    it = params.find("timeout");
    if (it != params.end()) {
        timeout_ = std::stoi(it->second);
    }

    // 初始化 MySQL 连接
    conn_ = mysql_init(nullptr);
    if (!conn_) {
        last_error_ = "mysql_init failed";
        return -1;
    }

    // 设置连接超时
    mysql_options(conn_, MYSQL_OPT_CONNECT_TIMEOUT, &timeout_);

    // 设置字符集
    mysql_options(conn_, MYSQL_SET_CHARSET_NAME, charset_.c_str());

    // 连接到 MySQL 服务器
    if (!mysql_real_connect(conn_, host_.c_str(), user_.c_str(), password_.c_str(), database_.c_str(), port_,
                            nullptr, 0)) {
        last_error_ = mysql_error(conn_);
        mysql_close(conn_);
        conn_ = nullptr;
        return -1;
    }

    printf("MysqlDriver: connected to %s:%d/%s\n", host_.c_str(), port_, database_.c_str());
    return 0;
}

int MysqlDriver::Disconnect() {
    if (!conn_) return 0;
    mysql_close(conn_);
    conn_ = nullptr;
    printf("MysqlDriver: disconnected from %s:%d\n", host_.c_str(), port_);
    return 0;
}

// 钩子方法实现
void* MysqlDriver::ExecuteQueryImpl(const char* sql, std::string* error) {
    if (!conn_) {
        *error = "database not connected";
        return nullptr;
    }

    // 创建预编译语句
    MYSQL_STMT* stmt = mysql_stmt_init(conn_);
    if (!stmt) {
        *error = "mysql_stmt_init failed: " + std::string(mysql_error(conn_));
        return nullptr;
    }

    // 预编译 SQL
    if (mysql_stmt_prepare(stmt, sql, strlen(sql)) != 0) {
        *error = "mysql_stmt_prepare failed: " + std::string(mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return nullptr;
    }

    // 执行查询
    if (mysql_stmt_execute(stmt) != 0) {
        *error = "mysql_stmt_execute failed: " + std::string(mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return nullptr;
    }

    // 获取元数据
    MYSQL_RES* metadata = mysql_stmt_result_metadata(stmt);
    if (!metadata) {
        *error = "mysql_stmt_result_metadata failed: " + std::string(mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return nullptr;
    }

    // 创建结果集包装
    auto* result = new MysqlResult();
    result->stmt = stmt;
    result->metadata = metadata;
    result->num_fields = mysql_num_fields(metadata);

    // 分配 MYSQL_BIND 数组
    result->binds = new MYSQL_BIND[result->num_fields];
    result->lengths = new unsigned long[result->num_fields];
    result->is_nulls = new bool[result->num_fields];
    result->errors = new bool[result->num_fields];
    memset(result->binds, 0, sizeof(MYSQL_BIND) * result->num_fields);

    // 绑定结果集列（使用动态缓冲区）
    MYSQL_FIELD* fields = mysql_fetch_fields(metadata);
    for (int i = 0; i < result->num_fields; ++i) {
        result->binds[i].is_null = &result->is_nulls[i];
        result->binds[i].length = &result->lengths[i];
        result->binds[i].error = &result->errors[i];

        // 根据类型分配缓冲区
        switch (fields[i].type) {
            case MYSQL_TYPE_TINY:
            case MYSQL_TYPE_SHORT:
            case MYSQL_TYPE_LONG:
            case MYSQL_TYPE_LONGLONG:
            case MYSQL_TYPE_INT24:
                result->binds[i].buffer = new int64_t;
                result->binds[i].buffer_length = sizeof(int64_t);
                result->binds[i].buffer_type = MYSQL_TYPE_LONGLONG;  // 统一使用 LONGLONG
                break;
            case MYSQL_TYPE_FLOAT:
            case MYSQL_TYPE_DOUBLE:
                result->binds[i].buffer = new double;
                result->binds[i].buffer_length = sizeof(double);
                result->binds[i].buffer_type = MYSQL_TYPE_DOUBLE;  // 统一使用 DOUBLE
                break;
            default:
                // 字符串/BLOB 类型使用动态缓冲区
                result->binds[i].buffer = new char[65536];  // 64KB 缓冲区
                result->binds[i].buffer_length = 65536;
                result->binds[i].buffer_type = fields[i].type;  // 保持原始类型
                break;
        }
    }

    // 绑定结果集
    if (mysql_stmt_bind_result(stmt, result->binds) != 0) {
        *error = "mysql_stmt_bind_result failed: " + std::string(mysql_stmt_error(stmt));
        // 清理资源
        for (int i = 0; i < result->num_fields; ++i) {
            delete[] static_cast<char*>(result->binds[i].buffer);
        }
        delete[] result->binds;
        delete[] result->lengths;
        delete[] result->is_nulls;
        delete[] result->errors;
        mysql_free_result(metadata);
        mysql_stmt_close(stmt);
        delete result;
        return nullptr;
    }

    return result;
}

std::shared_ptr<arrow::Schema> MysqlDriver::InferSchemaImpl(void* result, std::string* error) {
    auto* mysql_result = static_cast<MysqlResult*>(result);
    if (!mysql_result || !mysql_result->metadata) {
        *error = "invalid result";
        return nullptr;
    }

    MYSQL_FIELD* fields = mysql_fetch_fields(mysql_result->metadata);
    std::vector<std::shared_ptr<arrow::Field>> arrow_fields;

    for (int i = 0; i < mysql_result->num_fields; ++i) {
        std::string col_name = fields[i].name;
        std::shared_ptr<arrow::DataType> arrow_type;

        // MySQL 类型 → Arrow 类型映射
        switch (fields[i].type) {
            case MYSQL_TYPE_TINY:
            case MYSQL_TYPE_SHORT:
            case MYSQL_TYPE_LONG:
            case MYSQL_TYPE_LONGLONG:
            case MYSQL_TYPE_INT24:
                arrow_type = arrow::int64();
                break;
            case MYSQL_TYPE_FLOAT:
            case MYSQL_TYPE_DOUBLE:
            case MYSQL_TYPE_DECIMAL:
            case MYSQL_TYPE_NEWDECIMAL:
                arrow_type = arrow::float64();
                break;
            case MYSQL_TYPE_BLOB:
            case MYSQL_TYPE_TINY_BLOB:
            case MYSQL_TYPE_MEDIUM_BLOB:
            case MYSQL_TYPE_LONG_BLOB:
                arrow_type = arrow::binary();
                break;
            default:
                arrow_type = arrow::utf8();
                break;
        }

        arrow_fields.push_back(arrow::field(col_name, arrow_type));
    }

    return arrow::schema(arrow_fields);
}

int MysqlDriver::FetchRowImpl(void* result, const std::vector<std::unique_ptr<arrow::ArrayBuilder>>& builders,
                              std::string* error) {
    auto* mysql_result = static_cast<MysqlResult*>(result);
    if (!mysql_result || !mysql_result->stmt) {
        *error = "invalid result";
        return -1;
    }

    // 获取下一行
    int ret = mysql_stmt_fetch(mysql_result->stmt);
    if (ret == MYSQL_NO_DATA) {
        return 0;  // 没有更多行
    }
    if (ret != 0 && ret != MYSQL_DATA_TRUNCATED) {
        *error = "mysql_stmt_fetch failed: " + std::string(mysql_stmt_error(mysql_result->stmt));
        return -1;
    }

    // 逐列读取值并追加到 builder
    MYSQL_FIELD* fields = mysql_fetch_fields(mysql_result->metadata);
    for (int col = 0; col < mysql_result->num_fields; ++col) {
        auto* builder = builders[col].get();

        // 处理 NULL 值
        if (mysql_result->is_nulls[col]) {
            if (!builder->AppendNull().ok()) {
                *error = "failed to append null";
                return -1;
            }
            continue;
        }

        // 根据类型读取值
        auto arrow_type = builder->type();
        if (arrow_type->id() == arrow::Type::INT64) {
            int64_t value = *static_cast<int64_t*>(mysql_result->binds[col].buffer);
            auto status = static_cast<arrow::Int64Builder*>(builder)->Append(value);
            if (!status.ok()) {
                *error = "failed to append int64: " + status.ToString();
                return -1;
            }
        } else if (arrow_type->id() == arrow::Type::DOUBLE) {
            double value = *static_cast<double*>(mysql_result->binds[col].buffer);
            auto status = static_cast<arrow::DoubleBuilder*>(builder)->Append(value);
            if (!status.ok()) {
                *error = "failed to append double: " + status.ToString();
                return -1;
            }
        } else if (arrow_type->id() == arrow::Type::STRING) {
            char* str = static_cast<char*>(mysql_result->binds[col].buffer);
            auto status = static_cast<arrow::StringBuilder*>(builder)->Append(str, mysql_result->lengths[col]);
            if (!status.ok()) {
                *error = "failed to append string: " + status.ToString();
                return -1;
            }
        } else if (arrow_type->id() == arrow::Type::BINARY) {
            char* data = static_cast<char*>(mysql_result->binds[col].buffer);
            auto status = static_cast<arrow::BinaryBuilder*>(builder)->Append(
                reinterpret_cast<const uint8_t*>(data), mysql_result->lengths[col]);
            if (!status.ok()) {
                *error = "failed to append binary: " + status.ToString();
                return -1;
            }
        } else {
            // 默认当字符串处理
            char* str = static_cast<char*>(mysql_result->binds[col].buffer);
            auto status = static_cast<arrow::StringBuilder*>(builder)->Append(str, mysql_result->lengths[col]);
            if (!status.ok()) {
                *error = "failed to append string: " + status.ToString();
                return -1;
            }
        }
    }

    return 1;  // 成功读取一行
}

void MysqlDriver::FreeResultImpl(void* result) {
    auto* mysql_result = static_cast<MysqlResult*>(result);
    if (!mysql_result) return;

    // 释放缓冲区
    if (mysql_result->binds) {
        for (int i = 0; i < mysql_result->num_fields; ++i) {
            if (mysql_result->binds[i].buffer) {
                delete[] static_cast<char*>(mysql_result->binds[i].buffer);
            }
        }
        delete[] mysql_result->binds;
    }

    delete[] mysql_result->lengths;
    delete[] mysql_result->is_nulls;
    delete[] mysql_result->errors;

    if (mysql_result->metadata) {
        mysql_free_result(mysql_result->metadata);
    }

    if (mysql_result->stmt) {
        mysql_stmt_close(mysql_result->stmt);
    }

    delete mysql_result;
}

int MysqlDriver::ExecuteSqlImpl(const char* sql, std::string* error) {
    if (!conn_) {
        *error = "database not connected";
        return -1;
    }

    if (mysql_query(conn_, sql) != 0) {
        *error = "mysql_query failed: " + std::string(mysql_error(conn_));
        return -1;
    }

    // 清理结果集（如果有）
    MYSQL_RES* res = mysql_store_result(conn_);
    if (res) {
        mysql_free_result(res);
    }

    return 0;
}

}  // namespace database
}  // namespace flowsql
