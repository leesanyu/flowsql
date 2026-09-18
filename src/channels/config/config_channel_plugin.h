// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_CHANNELS_CONFIG_CONFIG_CHANNEL_PLUGIN_H_
#define _FLOWSQL_CHANNELS_CONFIG_CONFIG_CHANNEL_PLUGIN_H_

#include <common/iplugin.h>
#include <framework/interfaces/irouter_handle.h>

#include <cerrno>
#include <cstdio>
#include <string>

#include "config_channel_provider.h"

namespace flowsql::channels::config {

class ConfigChannelPlugin final : public IPlugin,
                                  public ConfigChannelProvider,
                                  public IRouterHandle {
 public:
    int Option(const char* option) override {
        constexpr char prefix[] = "db_path=";
        if (!option || std::string(option).compare(0, sizeof(prefix) - 1, prefix) != 0) return EINVAL;
        db_path_ = std::string(option).substr(sizeof(prefix) - 1);
        return db_path_.empty() || db_path_.find(';') != std::string::npos ? EINVAL : 0;
    }

    int Load(IQuerier*) override {
        std::string error;
        const int rc = Open(db_path_, &error);
        if (rc != 0) std::fprintf(stderr, "ConfigChannelPlugin::Load: %s\n", error.c_str());
        return rc;
    }

    int Unload() override {
        Close();
        return 0;
    }

    void EnumRoutes(std::function<void(const RouteItem&)> callback) override;

 private:
    int32_t HandleList(const std::string& uri, const std::string& request, std::string& response);
    int32_t HandlePublish(const std::string& uri, const std::string& request, std::string& response);
    int32_t HandleHistory(const std::string& uri, const std::string& request, std::string& response);
    int32_t HandleResolve(const std::string& uri, const std::string& request, std::string& response);

    std::string db_path_;
};

}  // namespace flowsql::channels::config

#endif  // _FLOWSQL_CHANNELS_CONFIG_CONFIG_CHANNEL_PLUGIN_H_
