#include "clickhouse_driver.h"

#include <arrow/api.h>
#include <arrow/io/memory.h>
#include <arrow/ipc/reader.h>
#include <arrow/ipc/writer.h>
#include <httplib.h>

#include <common/log.h>

namespace flowsql {
namespace database {

// 表名转义：将反引号替换为双反引号，防止 SQL 注入（与 MySQL 驱动保持一致）
static std::string QuoteIdentifier(const std::string& name) {
    std::string result = "`";
    for (char c : name) {
        if (c == '`') result += "``";
        else result += c;
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
    host_     = get("host", "127.0.0.1");
    port_     = std::stoi(get("port", "8123"));
    user_     = get("user", "default");
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
    httplib::Headers headers = {
        {"X-ClickHouse-User", user_},
        {"X-ClickHouse-Key", password_}
    };
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

ClickHouseSession::ClickHouseSession(const std::string& host, int port,
                                     const std::string& user, const std::string& password,
                                     const std::string& database)
    : host_(host), port_(port), user_(user), password_(password), database_(database) {}

bool ClickHouseSession::Ping() {
    httplib::Client client(host_, port_);
    client.set_connection_timeout(5);
    httplib::Headers headers = {
        {"X-ClickHouse-User", user_},
        {"X-ClickHouse-Key", password_}
    };
    auto res = client.Get("/?query=SELECT+1", headers);
    return res && res->status == 200;
}

int ClickHouseSession::ExecuteSql(const char* sql, std::string* error) {
    httplib::Client client(host_, port_);
    client.set_connection_timeout(10);
    client.set_read_timeout(60);

    httplib::Headers headers = {
        {"X-ClickHouse-User", user_},
        {"X-ClickHouse-Key", password_}
    };

    std::string path = "/?database=" + database_;
    auto res = client.Post(path, headers, std::string(sql), "text/plain");

    if (!res || res->status != 200) {
        if (error) *error = res ? res->body : "Connection failed";
        return -1;
    }
    return 0;
}

int ClickHouseSession::ExecuteQueryArrow(const char* sql,
                                         std::vector<std::shared_ptr<arrow::RecordBatch>>* batches,
                                         std::string* error) {
    std::string full_sql = std::string(sql) + " FORMAT ArrowStream";

    httplib::Client client(host_, port_);
    client.set_connection_timeout(10);
    client.set_read_timeout(60);

    httplib::Headers headers = {
        {"X-ClickHouse-User", user_},
        {"X-ClickHouse-Key", password_}
    };

    std::string path = "/?database=" + database_;
    auto res = client.Post(path, headers, full_sql, "text/plain");

    if (!res || res->status != 200) {
        if (error) *error = res ? res->body : "Connection failed";
        return -1;
    }

    return ParseArrowStream(res->body, batches, error);
}

int ClickHouseSession::WriteArrowBatches(const char* table,
                                         const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches,
                                         std::string* error) {
    // 空 batches 提前返回，不发 HTTP 请求
    // （ClickHouse 收到只有 Schema 的 IPC Stream 行为未定义）
    if (batches.empty()) return 0;

    std::string body;
    if (SerializeArrowStream(batches, &body, error) != 0) return -1;

    std::string query = "INSERT INTO " + QuoteIdentifier(table) + " FORMAT ArrowStream";
    std::string path = "/?database=" + database_ + "&query=" + httplib::detail::encode_url(query);

    httplib::Client client(host_, port_);
    client.set_connection_timeout(10);
    client.set_read_timeout(60);

    httplib::Headers headers = {
        {"X-ClickHouse-User", user_},
        {"X-ClickHouse-Key", password_}
    };

    auto res = client.Post(path, headers, body, "application/octet-stream");
    if (!res || res->status != 200) {
        if (error) *error = res ? res->body : "Connection failed";
        return -1;
    }
    return 0;
}

int ClickHouseSession::ParseArrowStream(const std::string& body,
                                        std::vector<std::shared_ptr<arrow::RecordBatch>>* batches,
                                        std::string* error) {
    auto buffer = arrow::Buffer::FromString(body);
    auto buf_reader = std::make_shared<arrow::io::BufferReader>(buffer);

    auto reader_result = arrow::ipc::RecordBatchStreamReader::Open(buf_reader);
    if (!reader_result.ok()) {
        if (error) *error = reader_result.status().ToString();
        return -1;
    }
    auto reader = *reader_result;

    while (true) {
        std::shared_ptr<arrow::RecordBatch> batch;
        auto status = reader->ReadNext(&batch);
        if (!status.ok()) {
            if (error) *error = status.ToString();
            return -1;
        }
        if (!batch) break;  // EOS
        batches->push_back(batch);
    }
    return 0;
}

int ClickHouseSession::SerializeArrowStream(
    const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches,
    std::string* body, std::string* error) {
    if (batches.empty()) {
        *body = "";
        return 0;
    }

    auto schema = batches[0]->schema();
    auto buffer_output_result = arrow::io::BufferOutputStream::Create();
    if (!buffer_output_result.ok()) {
        if (error) *error = buffer_output_result.status().ToString();
        return -1;
    }
    auto buffer_output = *buffer_output_result;

    auto writer_result = arrow::ipc::MakeStreamWriter(buffer_output, schema);
    if (!writer_result.ok()) {
        if (error) *error = writer_result.status().ToString();
        return -1;
    }
    auto writer = *writer_result;

    for (const auto& batch : batches) {
        auto status = writer->WriteRecordBatch(*batch);
        if (!status.ok()) {
            if (error) *error = status.ToString();
            return -1;
        }
    }

    auto close_status = writer->Close();
    if (!close_status.ok()) {
        if (error) *error = close_status.ToString();
        return -1;
    }

    auto buffer_result = buffer_output->Finish();
    if (!buffer_result.ok()) {
        if (error) *error = buffer_result.status().ToString();
        return -1;
    }

    auto buf = *buffer_result;
    *body = std::string(reinterpret_cast<const char*>(buf->data()), buf->size());
    return 0;
}

}  // namespace database
}  // namespace flowsql
