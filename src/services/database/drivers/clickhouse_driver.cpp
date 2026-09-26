/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "clickhouse_driver.h"

#include <arrow/api.h>
#include <arrow/io/memory.h>
#include <arrow/ipc/reader.h>
#include <arrow/ipc/writer.h>
#include <httplib.h>

#include <common/log.h>

#include <iomanip>
#include <limits>
#include <sstream>

namespace flowsql {
namespace database {

// 表名转义：将反引号替换为双反引号，防止 SQL 注入（与 MySQL 驱动保持一致）
static std::string QuoteIdentifier(const std::string& name) {
    std::string result = "`";
    for (char c : name) {
        if (c == '`')
            result += "``";
        else
            result += c;
    }
    result += "`";
    return result;
}

// ==================== ClickHouseDriver 实现 ====================

int ClickHouseDriver::Connect(const std::unordered_map<std::string, std::string>& params) {
    auto get = [&](const std::string& k, const std::string& def) {
        auto it = params.find(k);
        return it != params.end() ? it->second : def;
    };
    host_ = get("host", "127.0.0.1");
    port_ = std::stoi(get("port", "8123"));
    user_ = get("user", "default");
    password_ = get("password", "");
    database_ = get("database", "default");

    // Ping 返回具体错误（区分网络不可达 vs 认证失败）
    if (!Ping()) {
        // last_error_ 已在 Ping() 内部根据 HTTP 状态码设置
        return -1;
    }
    connected_ = true;
    LOG_INFO("ClickHouseDriver: connected to %s:%d/%s", host_.c_str(), port_, database_.c_str());
    return 0;
}

bool ClickHouseDriver::Ping() {
    httplib::Client client(host_, port_);
    client.set_connection_timeout(5);
    httplib::Headers headers = {{"X-ClickHouse-User", user_}, {"X-ClickHouse-Key", password_}};
    auto res = client.Get("/?query=SELECT+1", headers);
    if (!res) {
        last_error_ = "ClickHouse unreachable at " + host_ + ":" + std::to_string(port_);
        return false;
    }
    if (res->status == 401 || res->status == 403) {
        last_error_ = "ClickHouse authentication failed (HTTP " + std::to_string(res->status) + "): " + res->body;
        return false;
    }
    if (res->status != 200) {
        last_error_ = "ClickHouse error (HTTP " + std::to_string(res->status) + "): " + res->body;
        return false;
    }
    return true;
}

std::shared_ptr<IDbSession> ClickHouseDriver::CreateSession() {
    if (!connected_) {
        last_error_ = "Driver not connected";
        return nullptr;
    }
    return std::make_shared<ClickHouseSession>(host_, port_, user_, password_, database_);
}

// ==================== ClickHouseSession 实现 ====================

ClickHouseSession::ClickHouseSession(const std::string& host, int port, const std::string& user,
                                     const std::string& password, const std::string& database)
    : host_(host), port_(port), user_(user), password_(password), database_(database) {}

bool ClickHouseSession::Ping() {
    httplib::Client client(host_, port_);
    client.set_connection_timeout(5);
    httplib::Headers headers = {{"X-ClickHouse-User", user_}, {"X-ClickHouse-Key", password_}};
    auto res = client.Get("/?query=SELECT+1", headers);
    return res && res->status == 200;
}

int ClickHouseSession::ExecuteSql(const char* sql) {
    last_error_.clear();
    httplib::Client client(host_, port_);
    client.set_connection_timeout(10);
    client.set_read_timeout(60);

    httplib::Headers headers = {{"X-ClickHouse-User", user_}, {"X-ClickHouse-Key", password_}};

    std::string path = "/?database=" + database_;
    auto res = client.Post(path, headers, std::string(sql), "text/plain");

    if (!res || res->status != 200) {
        last_error_ = res ? res->body : "Connection failed";
        return -1;
    }
    return 0;
}

namespace {

struct ClickHouseParameterizedSql {
    std::string sql;
    std::vector<DatabaseParameterV1> parameters;
};

const char* ClickHouseParameterType(DatabaseParameterKindV1 kind) {
    switch (kind) {
        case DatabaseParameterKindV1::kInt64:
            return "Int64";
        case DatabaseParameterKindV1::kUInt64:
            return "UInt64";
        case DatabaseParameterKindV1::kDouble:
            return "Float64";
        case DatabaseParameterKindV1::kString:
        case DatabaseParameterKindV1::kBlob:
            return "String";
        case DatabaseParameterKindV1::kNull:
            return nullptr;
    }
    return nullptr;
}

bool RewriteClickHouseParameters(std::string_view sql, const DatabaseParameterV1* parameters, size_t parameter_count,
                                 ClickHouseParameterizedSql* output) {
    bool single_quoted = false;
    bool double_quoted = false;
    bool backtick_quoted = false;
    size_t parameter = 0;
    for (size_t index = 0; index < sql.size(); ++index) {
        const char character = sql[index];
        if (character == '\'' && !double_quoted && !backtick_quoted) {
            if (single_quoted && index + 1 < sql.size() && sql[index + 1] == '\'') {
                output->sql += "''";
                ++index;
                continue;
            }
            single_quoted = !single_quoted;
        } else if (character == '"' && !single_quoted && !backtick_quoted) {
            double_quoted = !double_quoted;
        } else if (character == '`' && !single_quoted && !double_quoted) {
            backtick_quoted = !backtick_quoted;
        }
        if (character != '?' || single_quoted || double_quoted || backtick_quoted) {
            output->sql += character;
            continue;
        }
        if (parameter >= parameter_count) return false;
        const auto& value = parameters[parameter++];
        if (value.kind == DatabaseParameterKindV1::kNull) {
            output->sql += "NULL";
            continue;
        }
        const size_t name = output->parameters.size();
        output->sql += "{p" + std::to_string(name) + ':' + ClickHouseParameterType(value.kind) + '}';
        output->parameters.push_back(value);
    }
    return parameter == parameter_count && !single_quoted && !double_quoted && !backtick_quoted;
}

std::string ClickHouseParameterValue(const DatabaseParameterV1& parameter) {
    switch (parameter.kind) {
        case DatabaseParameterKindV1::kInt64:
            return std::to_string(parameter.int64_value);
        case DatabaseParameterKindV1::kUInt64:
            return std::to_string(parameter.uint64_value);
        case DatabaseParameterKindV1::kDouble: {
            std::ostringstream text;
            text << std::setprecision(std::numeric_limits<double>::max_digits10) << parameter.double_value;
            return text.str();
        }
        case DatabaseParameterKindV1::kString:
        case DatabaseParameterKindV1::kBlob:
            return std::string(parameter.data ? static_cast<const char*>(parameter.data) : "", parameter.size);
        case DatabaseParameterKindV1::kNull:
            return {};
    }
    return {};
}

}  // namespace

int ClickHouseSession::ExecuteParameterizedHttp(const std::string& sql, const DatabaseParameterV1* parameters,
                                                size_t parameter_count) {
    std::string path = "/?database=" + httplib::detail::encode_url(database_);
    for (size_t index = 0; index < parameter_count; ++index) {
        path += "&param_p" + std::to_string(index) + '=' +
                httplib::detail::encode_url(ClickHouseParameterValue(parameters[index]));
    }
    httplib::Client client(host_, port_);
    client.set_connection_timeout(10);
    client.set_read_timeout(60);
    const httplib::Headers headers = {{"X-ClickHouse-User", user_}, {"X-ClickHouse-Key", password_}};
    auto response = client.Post(path, headers, sql, "text/plain");
    if (!response || response->status != 200) {
        last_error_ = response ? response->body : "Connection failed";
        return -1;
    }
    return 0;
}

int ClickHouseSession::ExecutePrepared(const char* sql, const DatabaseParameterV1* parameters, size_t parameter_count) {
    last_error_.clear();
    if (!sql || (parameter_count != 0 && !parameters)) {
        last_error_ = "invalid prepared command";
        return -1;
    }
    ClickHouseParameterizedSql rewritten;
    if (!RewriteClickHouseParameters(sql, parameters, parameter_count, &rewritten)) {
        last_error_ = "prepared command parameter count mismatch";
        return -1;
    }
    return ExecuteParameterizedHttp(rewritten.sql, rewritten.parameters.data(), rewritten.parameters.size());
}

int ClickHouseSession::ExecutePreparedBatch(const char* sql, const DatabaseParameterV1* parameters,
                                            size_t parameters_per_execution, size_t execution_count) {
    last_error_.clear();
    if (!sql || (execution_count != 0 && (!parameters || parameters_per_execution == 0)) ||
        (execution_count != 0 && parameters_per_execution > std::numeric_limits<size_t>::max() / execution_count)) {
        last_error_ = "invalid prepared batch";
        return -1;
    }
    if (execution_count == 0) return 0;
    const std::string source(sql);
    const auto values = source.find("VALUES(");
    if (values == std::string::npos || source.back() != ')') {
        last_error_ = "ClickHouse prepared batch requires INSERT VALUES tuple";
        return -1;
    }
    const std::string prefix = source.substr(0, values + 6);
    const std::string tuple = source.substr(values + 6);
    ClickHouseParameterizedSql rewritten;
    rewritten.sql = prefix;
    for (size_t execution = 0; execution < execution_count; ++execution) {
        if (execution) rewritten.sql += ',';
        if (!RewriteClickHouseParameters(tuple, parameters + execution * parameters_per_execution,
                                         parameters_per_execution, &rewritten)) {
            last_error_ = "prepared batch parameter count mismatch";
            return -1;
        }
    }
    const int result =
        ExecuteParameterizedHttp(rewritten.sql, rewritten.parameters.data(), rewritten.parameters.size());
    return result < 0
               ? -1
               : static_cast<int>(std::min(execution_count, static_cast<size_t>(std::numeric_limits<int>::max())));
}

int ClickHouseSession::ExecuteQueryArrow(const char* sql, std::vector<std::shared_ptr<arrow::RecordBatch>>* batches) {
    last_error_.clear();
    std::string full_sql = std::string(sql) + " FORMAT ArrowStream";

    httplib::Client client(host_, port_);
    client.set_connection_timeout(10);
    client.set_read_timeout(60);

    httplib::Headers headers = {{"X-ClickHouse-User", user_}, {"X-ClickHouse-Key", password_}};

    std::string path = "/?database=" + database_;
    auto res = client.Post(path, headers, full_sql, "text/plain");

    if (!res || res->status != 200) {
        last_error_ = res ? res->body : "Connection failed";
        return -1;
    }

    return ParseArrowStream(res->body, batches);
}

int ClickHouseSession::WriteArrowBatches(const char* table,
                                         const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches) {
    last_error_.clear();
    if (batches.empty()) return 0;

    std::string body;
    if (SerializeArrowStream(batches, &body) != 0) return -1;

    std::string query = "INSERT INTO " + QuoteIdentifier(table) + " FORMAT ArrowStream";
    std::string path = "/?database=" + database_ + "&query=" + httplib::detail::encode_url(query);

    httplib::Client client(host_, port_);
    client.set_connection_timeout(10);
    client.set_read_timeout(60);

    httplib::Headers headers = {{"X-ClickHouse-User", user_}, {"X-ClickHouse-Key", password_}};

    auto res = client.Post(path, headers, body, "application/octet-stream");
    if (!res || res->status != 200) {
        last_error_ = res ? res->body : "Connection failed";
        return -1;
    }
    return 0;
}

int ClickHouseSession::ParseArrowStream(const std::string& body,
                                        std::vector<std::shared_ptr<arrow::RecordBatch>>* batches) {
    auto buffer = arrow::Buffer::FromString(body);
    auto buf_reader = std::make_shared<arrow::io::BufferReader>(buffer);

    auto reader_result = arrow::ipc::RecordBatchStreamReader::Open(buf_reader);
    if (!reader_result.ok()) {
        last_error_ = reader_result.status().ToString();
        return -1;
    }
    auto reader = *reader_result;

    while (true) {
        std::shared_ptr<arrow::RecordBatch> batch;
        auto status = reader->ReadNext(&batch);
        if (!status.ok()) {
            last_error_ = status.ToString();
            return -1;
        }
        if (!batch) break;
        batches->push_back(batch);
    }
    return 0;
}

int ClickHouseSession::SerializeArrowStream(const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches,
                                            std::string* body) {
    if (batches.empty()) {
        *body = "";
        return 0;
    }

    auto schema = batches[0]->schema();
    auto buffer_output_result = arrow::io::BufferOutputStream::Create();
    if (!buffer_output_result.ok()) {
        last_error_ = buffer_output_result.status().ToString();
        return -1;
    }
    auto buffer_output = *buffer_output_result;

    auto writer_result = arrow::ipc::MakeStreamWriter(buffer_output, schema);
    if (!writer_result.ok()) {
        last_error_ = writer_result.status().ToString();
        return -1;
    }
    auto writer = *writer_result;

    for (const auto& batch : batches) {
        auto status = writer->WriteRecordBatch(*batch);
        if (!status.ok()) {
            last_error_ = status.ToString();
            return -1;
        }
    }

    auto close_status = writer->Close();
    if (!close_status.ok()) {
        last_error_ = close_status.ToString();
        return -1;
    }

    auto buffer_result = buffer_output->Finish();
    if (!buffer_result.ok()) {
        last_error_ = buffer_result.status().ToString();
        return -1;
    }

    auto buf = *buffer_result;
    *body = std::string(reinterpret_cast<const char*>(buf->data()), buf->size());
    return 0;
}

}  // namespace database
}  // namespace flowsql
