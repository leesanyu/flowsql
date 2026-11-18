// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "config.h"

#include <cstdio>

#include <yaml-cpp/yaml.h>

namespace flowsql {
namespace gateway {

int LoadConfig(const std::string& path, GatewayConfig* output) {
    GatewayConfig next;
    auto* config = &next;
    YAML::Node root;
    try {
        root = YAML::LoadFile(path);

        // gateway 段
        if (auto gw = root["gateway"]) {
            if (gw["host"]) config->host = gw["host"].as<std::string>();
            if (gw["port"]) config->port = gw["port"].as<int>();
            if (gw["heartbeat_interval_s"]) config->heartbeat_interval_s = gw["heartbeat_interval_s"].as<int>();
            if (gw["heartbeat_timeout_count"])
                config->heartbeat_timeout_count = gw["heartbeat_timeout_count"].as<int>();
        }

        // services 段
        if (auto svcs = root["services"]) {
            if (!svcs.IsSequence()) {
                printf("LoadConfig: services must be a sequence\n");
                return -1;
            }
            for (const auto& node : svcs) {
                ServiceConfig svc;
                svc.name = node["name"].as<std::string>();
                if (node["type"]) svc.type = node["type"].as<std::string>();
                if (node["command"]) svc.command = node["command"].as<std::string>();
                if (node["host"]) svc.host = node["host"].as<std::string>();
                if (node["port"]) svc.port = node["port"].as<int>();
                if (node["option"]) svc.option = node["option"].as<std::string>();
                if (node["plugins"]) {
                    if (!node["plugins"].IsSequence()) {
                        printf("LoadConfig: service %s plugins must be a sequence\n", svc.name.c_str());
                        return -1;
                    }
                    for (const auto& p : node["plugins"]) {
                        if (!p.IsMap() || !p["name"] || !p["name"].IsScalar() || p["name"].as<std::string>().empty()) {
                            printf("LoadConfig: service %s plugin must be an object with a nonempty name\n",
                                   svc.name.c_str());
                            return -1;
                        }
                        std::string plugin_name = p["name"].as<std::string>();
                        // 插件级 option 内嵌到插件名（"libxxx.so:key=val" 格式，LoadPlugin 会解析）
                        if (p["option"]) {
                            plugin_name += ":" + p["option"].as<std::string>();
                        }
                        svc.plugins.push_back(plugin_name);

                        // 解析 databases 数组
                        if (p["databases"]) {
                            if (!p["databases"].IsSequence()) {
                                printf("LoadConfig: plugin databases must be a sequence\n");
                                return -1;
                            }
                            for (const auto& db : p["databases"]) {
                                DatabaseConfig dbcfg;
                                dbcfg.type = db["type"].as<std::string>();
                                dbcfg.name = db["name"].as<std::string>();

                                // 解析所有其他字段到 params
                                for (const auto& kv : db) {
                                    std::string key = kv.first.as<std::string>();
                                    if (key != "type" && key != "name") {
                                        dbcfg.params[key] = kv.second.as<std::string>();
                                    }
                                }
                                svc.databases.push_back(std::move(dbcfg));
                            }
                        }
                    }
                }
                config->services.push_back(std::move(svc));
            }
        }

    } catch (const YAML::Exception& e) {
        printf("LoadConfig: invalid deployment %s: %s\n", path.c_str(), e.what());
        return -1;
    }
    printf("LoadConfig: gateway=%s:%d, %zu services\n", config->host.c_str(), config->port, config->services.size());
    *output = std::move(next);
    return 0;
}

}  // namespace gateway
}  // namespace flowsql
