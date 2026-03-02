#include "config.h"

#include <cstdio>

#include <yaml-cpp/yaml.h>

namespace flowsql {
namespace gateway {

int LoadConfig(const std::string& path, GatewayConfig* config) {
    YAML::Node root;
    try {
        root = YAML::LoadFile(path);
    } catch (const YAML::Exception& e) {
        printf("LoadConfig: failed to parse %s: %s\n", path.c_str(), e.what());
        return -1;
    }

    // gateway 段
    if (auto gw = root["gateway"]) {
        if (gw["host"]) config->host = gw["host"].as<std::string>();
        if (gw["port"]) config->port = gw["port"].as<int>();
        if (gw["heartbeat_interval_s"]) config->heartbeat_interval_s = gw["heartbeat_interval_s"].as<int>();
        if (gw["heartbeat_timeout_count"]) config->heartbeat_timeout_count = gw["heartbeat_timeout_count"].as<int>();
    }

    // services 段
    if (auto svcs = root["services"]) {
        for (const auto& node : svcs) {
            ServiceConfig svc;
            svc.name = node["name"].as<std::string>();
            if (node["type"]) svc.type = node["type"].as<std::string>();
            if (node["command"]) svc.command = node["command"].as<std::string>();
            if (node["host"]) svc.host = node["host"].as<std::string>();
            if (node["port"]) svc.port = node["port"].as<int>();
            if (node["option"]) svc.option = node["option"].as<std::string>();
            if (node["plugins"]) {
                for (const auto& p : node["plugins"]) {
                    svc.plugins.push_back(p.as<std::string>());
                }
            }
            config->services.push_back(std::move(svc));
        }
    }

    printf("LoadConfig: gateway=%s:%d, %zu services\n", config->host.c_str(), config->port,
           config->services.size());
    return 0;
}

}  // namespace gateway
}  // namespace flowsql
