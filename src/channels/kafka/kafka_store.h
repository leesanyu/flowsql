// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include <framework/interfaces/ikafka_connection_service.h>
#include <rapidjson/document.h>

#include <array>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>

struct sqlite3;

namespace flowsql::channels::kafka {

using Json = rapidjson::Document;
std::string Encode(const rapidjson::Value& value);
Json Parse(const std::string& text);
std::string Text(const rapidjson::Value& object, const char* key, const std::string& fallback = "");
void String(rapidjson::Value& object, const char* key, const std::string& value, Json::AllocatorType& allocator);

struct Binding {
    KafkaBindingSnapshotV1 snapshot;
    std::string source_options_json;
    Json connection;
    std::map<std::string, std::string> secrets;
    bool leased = false;
};

class Store {
 public:
    ~Store();
    void Open(const std::string& path, const std::array<unsigned char, 32>& key);
    void Close();
    Json Mutate(const std::string& kind, const std::string& operation, Json request);
    Json Query(const std::string& kind, const std::string& name = "", const std::string& after = "",
               size_t limit = 100);
    Binding Acquire(const std::string& name, const std::string& usage);
    void Release(const KafkaBindingSnapshotV1& snapshot);
    Binding Draft(Json connection);
    Binding AcquireConnection(const std::string& name);
    static void ValidateConnection(const rapidjson::Value& connection);
    static void ValidateChannel(const rapidjson::Value& channel);

 private:
    sqlite3* db_ = nullptr;
    std::array<unsigned char, 32> key_{};
    std::mutex mutex_;
    std::map<std::string, size_t> connections_in_use_, channels_in_use_;
    void Exec(const char* sql);
    Json Get(const std::string& kind, const std::string& name);
    std::string Encrypt(const std::string& plain);
    std::string Decrypt(const std::string& cipher);
    void Credentials(Json& request, const Json& previous);
    void ResolveSecrets(Binding& binding);
    Json Public(Json definition, const std::string& kind);
};

class Failure : public std::runtime_error {
 public:
    Failure(int status, std::string code, std::string stage = "configuration")
        : std::runtime_error(code), status(status), code(std::move(code)), stage(std::move(stage)) {}
    int status;
    std::string code, stage;
};

}  // namespace flowsql::channels::kafka
