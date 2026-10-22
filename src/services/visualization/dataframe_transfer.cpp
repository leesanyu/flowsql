// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "dataframe_transfer.h"

#include <arrow/io/file.h>
#include <arrow/ipc/api.h>
#include <openssl/sha.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <iomanip>
#include <sstream>

namespace flowsql::visualization {
namespace {
std::string JsonString(const std::string& value) {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    writer.String(value.data(), static_cast<rapidjson::SizeType>(value.size()));
    return buffer.GetString();
}

std::string SchemaRevision(const arrow::Schema& schema) {
    auto serialized = arrow::ipc::SerializeSchema(schema);
    if (!serialized.ok()) return {};
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256((*serialized)->data(), static_cast<size_t>((*serialized)->size()), digest);
    std::ostringstream result;
    for (auto byte : digest) result << std::hex << std::setfill('0') << std::setw(2) << unsigned(byte);
    return result.str();
}

constexpr auto kSnapshotLifetime = std::chrono::minutes(15);

bool SafeId(const std::string& id) {
    return id.size() == 32 && std::all_of(id.begin(), id.end(), [](unsigned char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

std::string Text(const rapidjson::Value& value, const char* key) {
    if (!value.HasMember(key) || !value[key].IsString()) return {};
    return {value[key].GetString(), value[key].GetStringLength()};
}
}  // namespace

bool DataFrameTransferClient::Init(std::string upstream, std::string shared_dir, uint32_t timeout_ms) {
    if (upstream.empty() != shared_dir.empty()) return false;
    if (upstream.empty()) return true;
    httplib::Client client(upstream);
    if (timeout_ms == 0 || timeout_ms > 300000 || !client.is_valid()) return false;
    std::error_code ec;
    const auto canonical = std::filesystem::canonical(shared_dir, ec);
    struct stat status {};
    if (ec || lstat(shared_dir.c_str(), &status) != 0 || !S_ISDIR(status.st_mode) || S_ISLNK(status.st_mode) ||
        status.st_uid != geteuid() || (status.st_mode & 077) != 0)
        return false;
    upstream_ = std::move(upstream);
    shared_dir_ = canonical.string();
    timeout_ms_ = timeout_ms;
    client.set_connection_timeout(std::chrono::milliseconds(std::min(timeout_ms_, 30000U)));
    client.set_read_timeout(std::chrono::milliseconds(timeout_ms_));
    client.set_write_timeout(std::chrono::milliseconds(timeout_ms_));
    expiry_thread_ = std::thread(&DataFrameTransferClient::ExpireLoop, this);
    return true;
}

bool DataFrameTransferClient::Available() const { return !upstream_.empty() && !shared_dir_.empty(); }

std::shared_ptr<httplib::Client> DataFrameTransferClient::Client() const {
    if (!Available()) return {};
    auto client = std::make_shared<httplib::Client>(upstream_);
    client->set_connection_timeout(std::chrono::milliseconds(std::min(timeout_ms_, 30000U)));
    client->set_read_timeout(std::chrono::milliseconds(timeout_ms_));
    client->set_write_timeout(std::chrono::milliseconds(timeout_ms_));
    return client;
}

std::vector<DataFrameSourceInfoV1> DataFrameTransferClient::Sources(std::string* error) const {
    if (error) error->clear();
    if (!Available()) {
        if (error) *error = "DATA_PLANE_UNAVAILABLE";
        return {};
    }
    auto client = Client();
    if (!client) {
        if (error) *error = "DATA_PLANE_UNAVAILABLE";
        return {};
    }
    auto response = client->Get("/channels/dataframe/exploration/v1/sources");
    rapidjson::Document document;
    if (!response || response->status != 200) {
        if (error) *error = "DATA_PLANE_UNAVAILABLE";
        return {};
    }
    document.Parse(response->body.c_str(), response->body.size());
    if (document.HasParseError() || !document.IsObject() || !document.HasMember("sources") ||
        !document["sources"].IsArray()) {
        if (error) *error = "DATA_PLANE_UNAVAILABLE";
        return {};
    }
    std::vector<DataFrameSourceInfoV1> result;
    for (const auto& item : document["sources"].GetArray()) {
        auto source_id = Text(item, "source_id"), label = Text(item, "label");
        if (!source_id.empty() && !label.empty()) result.push_back({std::move(source_id), std::move(label)});
    }
    return result;
}

bool DataFrameTransferClient::Acquire(const std::string& request_id, const std::string& source_id, uint64_t max_bytes,
                                      const std::shared_ptr<std::atomic_bool>& cancelled, uint32_t timeout_ms,
                                      DataFrameSnapshotInfoV1* output, std::string* error) {
    if (!output || !error) return false;
    *output = {};
    error->clear();
    const auto request_budget_ms = std::min(std::min(timeout_ms_, 60000U), timeout_ms == 0 ? timeout_ms_ : timeout_ms);
    if (!Available() || request_id.empty() || source_id.empty() || max_bytes == 0 || request_budget_ms == 0) {
        *error = "DATA_PLANE_UNAVAILABLE";
        return false;
    }
    rapidjson::StringBuffer body;
    rapidjson::Writer<rapidjson::StringBuffer> writer(body);
    writer.StartObject();
    writer.Key("request_id");
    writer.String(request_id.c_str());
    writer.Key("source_id");
    writer.String(source_id.c_str());
    writer.Key("max_bytes");
    writer.Uint64(max_bytes);
    writer.Key("remaining_timeout_ms");
    writer.Uint(request_budget_ms);
    writer.EndObject();
    auto client = Client();
    if (!client) {
        *error = "DATA_PLANE_UNAVAILABLE";
        return false;
    }
    // Bounded wait: the source retires an abandoned request, and local cancellation reports the same code.
    auto response =
        client->Post("/channels/dataframe/exploration/v1/snapshots/acquire", body.GetString(), "application/json");
    if (cancelled && cancelled->load()) {
        *error = "CANCELLED";
        return false;
    }
    rapidjson::Document document;
    if (!response) {
        *error = "DATA_PLANE_UNAVAILABLE";
        return false;
    }
    document.Parse(response->body.c_str(), response->body.size());
    if (document.HasParseError() || !document.IsObject()) {
        *error = "DATA_PLANE_UNAVAILABLE";
        return false;
    }
    if (response->status != 200) {
        *error = Text(document, "code");
        if (error->empty()) *error = "DATA_PLANE_UNAVAILABLE";
        return false;
    }
    const auto transfer_id = Text(document, "transfer_id");
    const auto snapshot_id = Text(document, "snapshot_id");
    const auto returned_source = Text(document, "source_id");
    const auto schema_revision = Text(document, "schema_revision");
    const auto captured_at = Text(document, "captured_at");
    const auto rows = document.HasMember("row_count") && document["row_count"].IsUint64()
                          ? document["row_count"].GetUint64()
                          : UINT64_MAX;
    const auto bytes = document.HasMember("ipc_bytes") && document["ipc_bytes"].IsUint64()
                           ? document["ipc_bytes"].GetUint64()
                           : UINT64_MAX;
    const auto expires = document.HasMember("expires_at_unix") && document["expires_at_unix"].IsInt64()
                             ? document["expires_at_unix"].GetInt64()
                             : int64_t{0};
    if (!SafeId(transfer_id) || snapshot_id.empty() || returned_source != source_id || schema_revision.empty() ||
        !document.HasMember("fields") || !document["fields"].IsArray() || document["fields"].Empty() ||
        rows == UINT64_MAX || bytes == UINT64_MAX || !bytes || bytes > max_bytes || expires <= 0) {
        *error = "DATA_PLANE_UNAVAILABLE";
        return false;
    }
    const auto path = shared_dir_ + "/" + transfer_id + ".arrow";
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat opened {
    }, current{};
    if (fd < 0 || fstat(fd, &opened) != 0 || !S_ISREG(opened.st_mode) || opened.st_uid != geteuid() ||
        (opened.st_mode & 077) != 0 || static_cast<uint64_t>(opened.st_size) != bytes ||
        lstat(path.c_str(), &current) != 0 || !S_ISREG(current.st_mode) || opened.st_dev != current.st_dev ||
        opened.st_ino != current.st_ino) {
        if (fd >= 0) close(fd);
        *error = "DATA_PLANE_UNAVAILABLE";
        client->Post("/channels/dataframe/exploration/v1/snapshots/release",
                     "{\"transfer_id\":" + JsonString(transfer_id) + "}", "application/json");
        return false;
    }
    close(fd);
    auto mapped = arrow::io::MemoryMappedFile::Open(path, arrow::io::FileMode::READ);
    if (!mapped.ok()) {
        *error = "DATA_PLANE_UNAVAILABLE";
        client->Post("/channels/dataframe/exploration/v1/snapshots/release",
                     "{\"transfer_id\":" + JsonString(transfer_id) + "}", "application/json");
        return false;
    }
    struct stat mapped_status {
    }, path_status{};
    const auto map_fd = (*mapped)->file_descriptor();
    if (fstat(map_fd, &mapped_status) != 0 || lstat(path.c_str(), &path_status) != 0 ||
        mapped_status.st_dev != opened.st_dev || mapped_status.st_ino != opened.st_ino ||
        path_status.st_dev != opened.st_dev || path_status.st_ino != opened.st_ino) {
        (*mapped)->Close();
        *error = "DATA_PLANE_UNAVAILABLE";
        client->Post("/channels/dataframe/exploration/v1/snapshots/release",
                     "{\"transfer_id\":" + JsonString(transfer_id) + "}", "application/json");
        return false;
    }
    auto reader = arrow::ipc::RecordBatchFileReader::Open(*mapped);
    if (!reader.ok() || (*reader)->num_record_batches() != 1) {
        if (reader.ok()) (*mapped)->Close();
        *error = "DATA_PLANE_UNAVAILABLE";
        client->Post("/channels/dataframe/exploration/v1/snapshots/release",
                     "{\"transfer_id\":" + JsonString(transfer_id) + "}", "application/json");
        return false;
    }
    auto batch = (*reader)->ReadRecordBatch(0);
    const auto schema = (*reader)->schema();
    bool valid = batch.ok() && schema && schema->num_fields() > 0 && (*batch)->num_rows() == rows &&
                 SchemaRevision(*schema) == schema_revision && schema->num_fields() == document["fields"].Size();
    if (valid) {
        for (int i = 0; i < schema->num_fields(); ++i) {
            const auto& expected = document["fields"][i];
            if (!expected.IsObject() || Text(expected, "name") != schema->field(i)->name() ||
                Text(expected, "type") != schema->field(i)->type()->ToString() || !expected.HasMember("nullable") ||
                !expected["nullable"].IsBool() || expected["nullable"].GetBool() != schema->field(i)->nullable()) {
                valid = false;
                break;
            }
        }
    }
    if (!valid) {
        reader = arrow::Status::Invalid("invalid transfer");
        (*mapped)->Close();
        *error = "DATA_PLANE_UNAVAILABLE";
        client->Post("/channels/dataframe/exploration/v1/snapshots/release",
                     "{\"transfer_id\":" + JsonString(transfer_id) + "}", "application/json");
        return false;
    }
    output->source_id = source_id;
    output->snapshot_id = snapshot_id;
    output->schema_revision = schema_revision;
    output->captured_at = captured_at;
    output->row_count = rows;
    output->ipc_bytes = bytes;
    output->expires_at_unix = expires;
    output->mapping = std::move(*mapped);
    output->batch = std::move(*batch);
    const auto release_response = client->Post("/channels/dataframe/exploration/v1/snapshots/release",
                                               "{\"transfer_id\":" + JsonString(transfer_id) + "}", "application/json");
    if (!release_response || release_response->status != 200) {
        output->batch.reset();
        *error = "DATA_PLANE_UNAVAILABLE";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || (cancelled && cancelled->load())) {
            *output = {};
            *error = stopping_ ? "DATA_PLANE_UNAVAILABLE" : "CANCELLED";
            return false;
        }
        Snapshot snapshot;
        snapshot.info = *output;
        snapshot.transfer_id = transfer_id;
        snapshot.expires_at = std::chrono::steady_clock::now() + kSnapshotLifetime;
        snapshots_[snapshot_id] = std::move(snapshot);
    }
    return true;
}

bool DataFrameTransferClient::Lookup(const std::string& snapshot_id, std::shared_ptr<arrow::RecordBatch>* batch,
                                     DataFrameSnapshotInfoV1* meta) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = snapshots_.find(snapshot_id);
    if (found == snapshots_.end() || found->second.expires_at <= std::chrono::steady_clock::now()) return false;
    if (batch) *batch = found->second.info.batch;
    if (meta) *meta = found->second.info;
    return true;
}

bool DataFrameTransferClient::Release(const std::string& snapshot_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshots_.erase(snapshot_id) != 0;
}

bool DataFrameTransferClient::Renew(const std::string& snapshot_id, int64_t* expires_at_unix) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = snapshots_.find(snapshot_id);
    if (found == snapshots_.end() || found->second.expires_at <= std::chrono::steady_clock::now()) return false;
    found->second.expires_at = std::chrono::steady_clock::now() + kSnapshotLifetime;
    const auto now = std::chrono::system_clock::now() + kSnapshotLifetime;
    if (expires_at_unix)
        *expires_at_unix = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    return true;
}

void DataFrameTransferClient::ExpireLoop() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopping_) {
        wake_.wait_for(lock, std::chrono::seconds(1));
        if (stopping_) break;
        const auto now = std::chrono::steady_clock::now();
        for (auto it = snapshots_.begin(); it != snapshots_.end();) {
            if (it->second.expires_at <= now)
                it = snapshots_.erase(it);
            else
                ++it;
        }
    }
}

void DataFrameTransferClient::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return;
        stopping_ = true;
        snapshots_.clear();
    }
    wake_.notify_all();
    if (expiry_thread_.joinable()) expiry_thread_.join();
    upstream_.clear();
    shared_dir_.clear();
}

}  // namespace flowsql::visualization
