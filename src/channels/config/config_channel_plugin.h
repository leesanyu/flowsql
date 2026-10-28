// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_CHANNELS_CONFIG_CONFIG_CHANNEL_PLUGIN_H_
#define _FLOWSQL_CHANNELS_CONFIG_CONFIG_CHANNEL_PLUGIN_H_

#include <common/iplugin.h>
#include <framework/interfaces/irouter_handle.h>

#include <cerrno>
#include <cstdio>
#include <string>

#include "channel_dictionary.h"
#include "config_channel_provider.h"
#include "data_semantics_registry.h"

namespace flowsql::channels::config {

class ConfigChannelPlugin final : public IPlugin,
                                  public ConfigChannelProvider,
                                  public DataSemanticsRegistry,
                                  public IRouterHandle {
 public:
    using ConfigChannelProvider::Close;
    using ConfigChannelProvider::Open;
    using ConfigChannelProvider::Resolve;
    int Option(const char* option) override {
        if (!option) return EINVAL;
        std::string options(option);
        size_t pos = 0;
        bool has_path = false;
        while (pos < options.size()) {
            const auto end = options.find(';', pos);
            const auto item = options.substr(pos, end == std::string::npos ? end : end - pos);
            const auto eq = item.find('=');
            if (eq == std::string::npos) return EINVAL;
            const auto key = item.substr(0, eq), value = item.substr(eq + 1);
            if (value.empty()) return EINVAL;
            if (key == "db_path" && !has_path) {
                db_path_ = value;
                has_path = true;
            } else if (key == "gateway") {
                const auto colon = value.rfind(':');
                if (colon == std::string::npos || colon == 0) return EINVAL;
                try {
                    gateway_port_ = std::stoi(value.substr(colon + 1));
                } catch (...) {
                    return EINVAL;
                }
                if (gateway_port_ < 1 || gateway_port_ > 65535) return EINVAL;
                gateway_host_ = value.substr(0, colon);
            } else
                return EINVAL;
            if (end == std::string::npos) break;
            pos = end + 1;
        }
        return has_path ? 0 : EINVAL;
    }

    int Load(IQuerier* querier) override {
        querier_ = querier;
        std::string error;
        int rc = ConfigChannelProvider::Open(db_path_, &error);
        if (rc == 0) {
            DataSemanticsErrorV1 semantics_error;
            rc = DataSemanticsRegistry::Open(db_path_, &semantics_error);
            if (rc != 0) {
                error = semantics_error.message;
                ConfigChannelProvider::Close();
            }
        }
        if (rc != 0) std::fprintf(stderr, "ConfigChannelPlugin::Load: %s\n", error.c_str());
        return rc;
    }

    int Unload() override {
        DataSemanticsRegistry::Close();
        ConfigChannelProvider::Close();
        return 0;
    }

    void EnumRoutes(std::function<void(const RouteItem&)> callback) override;

 private:
    int32_t HandleList(const std::string& uri, const std::string& request, std::string& response);
    int32_t HandlePublish(const std::string& uri, const std::string& request, std::string& response);
    int32_t HandleHistory(const std::string& uri, const std::string& request, std::string& response);
    int32_t HandleResolve(const std::string& uri, const std::string& request, std::string& response);

    int DescribeDataset(const ChannelDatasetRequestV1&, ChannelDatasetSnapshotV1*, DataSemanticsErrorV1*);
    IQuerier* querier_ = nullptr;
    std::string gateway_host_ = "127.0.0.1";
    int gateway_port_ = 18800;
    std::string db_path_;
};

}  // namespace flowsql::channels::config

#endif  // _FLOWSQL_CHANNELS_CONFIG_CONFIG_CHANNEL_PLUGIN_H_
