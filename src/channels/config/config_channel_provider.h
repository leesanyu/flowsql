// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_CHANNELS_CONFIG_CONFIG_CHANNEL_PROVIDER_H_
#define _FLOWSQL_CHANNELS_CONFIG_CONFIG_CHANNEL_PROVIDER_H_

#include <framework/interfaces/iconfig_channel_registry.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct sqlite3;

namespace flowsql::channels::config {

struct ConfigPublishRequest {
    std::string name;
    uint64_t expected_current_revision = 0;
    std::string format;
    std::string schema_id;
    std::string content;
    uint64_t base_revision = 0;  // 0 means no historical source.
    std::string original_filename;
    std::string change_note;
};

struct ConfigPublishResult {
    uint64_t revision = 0;
    bool created_revision = false;
    std::string sha256_hex;
};

struct ConfigChannelListItem {
    std::string name;
    uint64_t current_revision = 0;
    std::string format;
    std::string schema_id;
    std::string sha256_hex;
    uint64_t content_bytes = 0;
    int64_t updated_at_unix_ms = 0;
};

struct ConfigRevisionListItem {
    uint64_t revision = 0;
    std::string format;
    std::string schema_id;
    std::string sha256_hex;
    uint64_t content_bytes = 0;
    int64_t created_at_unix_ms = 0;
    uint64_t base_revision = 0;
    std::string original_filename;
    std::string change_note;
};

class ConfigChannelProvider : public IConfigChannelRegistryV1 {
 public:
    ~ConfigChannelProvider() override;
    ConfigChannelProvider(const ConfigChannelProvider&) = delete;
    ConfigChannelProvider& operator=(const ConfigChannelProvider&) = delete;
    ConfigChannelProvider() = default;

    int Open(const std::string& db_path, std::string* error);
    void Close();
    int Publish(const ConfigPublishRequest& request, ConfigPublishResult* result, std::string* error);
    int ListChannels(const std::string& cursor, uint32_t limit,
                     std::vector<ConfigChannelListItem>* items,
                     std::string* next_cursor, std::string* error);
    int ListHistory(const std::string& name, uint64_t before_revision, uint32_t limit,
                    std::vector<ConfigRevisionListItem>* items,
                    uint64_t* next_cursor, std::string* error);
    int Resolve(const char* exact_reference, ConfigChannelSnapshot* snapshot, std::string* error) override;

 private:
    std::mutex mutex_;
    sqlite3* db_ = nullptr;
};

}  // namespace flowsql::channels::config

#endif  // _FLOWSQL_CHANNELS_CONFIG_CONFIG_CHANNEL_PROVIDER_H_
