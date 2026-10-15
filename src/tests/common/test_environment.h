// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <rapidjson/document.h>

#include <sys/stat.h>
#include <unistd.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace flowsql::test {
inline std::string ReadPrivateSecret(const std::filesystem::path& base, const std::string& reference) {
    std::string value;
    if (reference.rfind("env:", 0) == 0) {
        const char* variable = std::getenv(reference.substr(4).c_str());
        if (!variable) throw std::runtime_error("config_missing: credential");
        value = variable;
    } else if (reference.rfind("file:", 0) == 0) {
        auto path = base / reference.substr(5);
        if (reference.rfind("file:@state/", 0) == 0) {
            const char* configured = std::getenv("FLOWSQL_TEST_STATE");
            const auto state = configured ? std::string(configured) : "/tmp/flowsql-test-" + std::to_string(getuid());
            path = std::filesystem::path(state) / reference.substr(12);
        }
        struct stat status {};
        if (stat(path.c_str(), &status) != 0) throw std::runtime_error("config_missing: credential");
        if (status.st_mode & 0077) throw std::runtime_error("permission_denied: private credential file");
        if (status.st_size > 65536) throw std::runtime_error("config_invalid: credential size");
        std::ifstream file(path);
        if (!file) throw std::runtime_error("config_missing: credential");
        std::getline(file, value);
    } else {
        throw std::runtime_error("config_invalid: credential reference");
    }
    if (value.empty() || value.find_first_of(";|\r\n") != std::string::npos)
        throw std::runtime_error("config_invalid: credential encoding");
    return value;
}

inline std::filesystem::path TestConfigPath() {
    if (const char* path = std::getenv("FLOWSQL_TEST_CONFIG")) return std::filesystem::absolute(path);
    return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path() /
           "config/testing/environment.json";
}

inline std::map<std::string, std::string> DatabaseParameters(const std::string& backend,
                                                             const std::string& role = "driver") {
    const auto path = TestConfigPath();
    std::ifstream input(path);
    if (!input) throw std::runtime_error("config_missing: test environment; run tools/test.sh integration");
    std::ostringstream text;
    text << input.rdbuf();
    rapidjson::Document config;
    config.Parse(text.str().c_str());
    auto member = [](const rapidjson::Value& value, const char* name) -> const rapidjson::Value& {
        if (!value.IsObject() || !value.HasMember(name)) throw std::runtime_error("config_invalid: database field");
        return value[name];
    };
    auto string = [](const rapidjson::Value& value) {
        if (!value.IsString()) throw std::runtime_error("config_invalid: string field");
        return std::string(value.GetString(), value.GetStringLength());
    };
    const auto& version = member(config, "schema_version");
    if (!version.IsInt() || version.GetInt() != 1) throw std::runtime_error("config_invalid: version");
    const auto& db = member(member(config, "databases"), backend.c_str());
    const char* location = std::getenv("FLOWSQL_TEST_LOCATION");
    const auto& endpoint = member(member(db, "endpoints"), location ? location : "native");
    const auto& port = member(endpoint, "port");
    if (!port.IsUint() || port.GetUint() == 0 || port.GetUint() > 65535)
        throw std::runtime_error("config_invalid: port");
    return {{"host", string(member(endpoint, "host"))},
            {"port", std::to_string(port.GetUint())},
            {"user", string(member(db, "user"))},
            {"password", ReadPrivateSecret(path.parent_path(), string(member(db, "password_ref")))},
            {"database", string(member(member(db, "roles"), role.c_str()))}};
}

inline std::unordered_map<std::string, std::string> DriverParameters(const std::string& backend) {
    const auto parameters = DatabaseParameters(backend);
    std::unordered_map<std::string, std::string> result(parameters.begin(), parameters.end());
    if (backend == "mysql") result["charset"] = "utf8mb4";
    return result;
}

inline std::string WorkspacePath(const std::string& relative) {
    static const std::filesystem::path root = [] {
        if (const char* configured = std::getenv("FLOWSQL_TEST_WORKSPACE")) return std::filesystem::path(configured);
        auto path = std::filesystem::temp_directory_path() / ("flowsql-test-workspace-" + std::to_string(getpid()));
        std::filesystem::create_directories(path);
        return path;
    }();
    const auto path = root / relative;
    std::filesystem::create_directories(path.parent_path());
    return path.string();
}

inline std::string DatabaseOptions(const std::string& backend, const std::string& name,
                                   const std::string& role = "driver") {
    auto parameters = DatabaseParameters(backend, role);
    std::string options = "type=" + backend + ";name=" + name;
    for (const auto& [key, value] : parameters) options += ";" + key + "=" + value;
    return options;
}

inline std::string BaselinerOptions(bool production = false) {
    const auto prefix = production ? "t73" : "t2";
    std::string options =
        std::string("type=sqlite;name=") + prefix + "sqlite;path=" + WorkspacePath("baseliner-source.db");
    for (const auto* backend : {"mysql", "postgres", "clickhouse"}) {
        options += '|' + DatabaseOptions(backend, std::string(prefix) + backend, production ? "production" : "source");
        if (!production) options += '|' + DatabaseOptions(backend, std::string("t3") + backend, "target");
    }
    return options;
}
}  // namespace flowsql::test
