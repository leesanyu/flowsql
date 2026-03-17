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
        if (gw["mode"]) config->mode = gw["mode"].as<std::string>();
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
                    if (p.IsScalar()) {
                        // 旧格式：字符串 "libflowsql_database.so:type=sqlite;name=testdb;path=:memory:"
                        svc.plugins.push_back(p.as<std::string>());
                    } else if (p.IsMap()) {
                        // 新格式：对象 {name: "libflowsql_database.so", option: "...", databases: [...]}
                        if (!p["name"]) {
                            printf("LoadConfig: plugin map missing 'name' field\n");
                            continue;
                        }
                        std::string plugin_name = p["name"].as<std::string>();
                        // 插件级 option 内嵌到插件名（"libxxx.so:key=val" 格式，LoadPlugin 会解析）
                        if (p["option"]) {
                            plugin_name += ":" + p["option"].as<std::string>();
                        }
                        svc.plugins.push_back(plugin_name);

                        // 解析 databases 数组
                        if (p["databases"]) {
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
                    } else {
                        printf("LoadConfig: unknown plugin format (not scalar or map)\n");
                    }
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
