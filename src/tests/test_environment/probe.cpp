// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <services/database/database_plugin.h>
#include <services/database/drivers/clickhouse_driver.h>
#include <services/database/drivers/mysql_driver.h>
#include <services/database/drivers/postgres_driver.h>
#include <tests/common/test_environment.h>

#include <arrow/api.h>
#include <unistd.h>
#include <cstdio>
#include <memory>
#include <string>

namespace {
std::string Category(std::string error) {
    for (char& c : error) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (error.find("password") != std::string::npos || error.find("authentication") != std::string::npos ||
        error.find("access denied for user") != std::string::npos)
        return "authentication_failed";
    if (error.find("unknown database") != std::string::npos || error.find("does not exist") != std::string::npos ||
        error.find("unknown_database") != std::string::npos)
        return "database_missing";
    if (error.find("permission") != std::string::npos || error.find("denied") != std::string::npos ||
        error.find("not enough privileges") != std::string::npos)
        return "permission_denied";
    return "network_unreachable";
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    const std::string backend = argv[1], role = argv[2];
    const bool write = argc == 4 && std::string(argv[3]) == "write";
    const int saved = dup(STDOUT_FILENO);
    FILE* quiet = std::fopen("/dev/null", "w");
    dup2(fileno(quiet), STDOUT_FILENO);
    std::string category, stage = "authentication";
    try {
        std::unique_ptr<flowsql::database::IDbDriver> driver;
        if (backend == "mysql") driver = std::make_unique<flowsql::database::MysqlDriver>();
        if (backend == "postgres") driver = std::make_unique<flowsql::database::PostgresDriver>();
        if (backend == "clickhouse") driver = std::make_unique<flowsql::database::ClickHouseDriver>();
        if (!driver) throw std::runtime_error("config_invalid: backend");
        auto p = flowsql::test::DatabaseParameters(backend, role);
        std::unordered_map<std::string, std::string> parameters(p.begin(), p.end());
        parameters["connect_timeout"] = "3";
        if (driver->Connect(parameters) != 0) {
            std::string diagnostic = driver->LastError();
            size_t found;
            while ((found = diagnostic.find(parameters["password"])) != std::string::npos)
                diagnostic.replace(found, parameters["password"].size(), "[REDACTED]");
            std::fprintf(stderr, "%s\n", diagnostic.c_str());
            category = Category(diagnostic);
        }
        if (!category.empty()) throw std::runtime_error(category);
        flowsql::database::DatabasePlugin plugin;
        const auto options = flowsql::test::DatabaseOptions(backend, "probe", role) + ";timeout=3;connect_timeout=3";
        if (plugin.Option(options.c_str()) != 0 || plugin.Load(nullptr) != 0 || plugin.Start() != 0) {
            category = "config_invalid";
        } else {
            auto channel = plugin.AcquireChannel(backend.c_str(), "probe");
            if (!channel) {
                std::fprintf(stderr, "%s\n", plugin.LastError());
                stage = "channel";
                category = Category(plugin.LastError());
            } else {
                stage = "query";
                if (backend == "clickhouse") {
                    std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
                    if (channel->ExecuteQueryArrow("SELECT CAST(1 AS Int32) AS value", &batches) != 0 ||
                        batches.empty())
                        category = Category(channel->GetLastError());
                } else {
                    flowsql::IBatchReader* raw = nullptr;
                    if (channel->CreateReader("SELECT 1", &raw) != 0 || !raw) {
                        category = Category(channel->GetLastError());
                    } else {
                        const uint8_t* data = nullptr;
                        size_t size = 0;
                        if (raw->Next(&data, &size) != 0 || !size) category = "permission_denied";
                        raw->Close();
                        raw->Release();
                    }
                }
                if (category.empty() && write) {
                    stage = "permissions";
                    const std::string table = "__environment_probe_" + std::to_string(getpid());
                    const std::string create =
                        "CREATE TABLE " + table + " (value INT)" + (backend == "clickhouse" ? " ENGINE=Memory" : "");
                    if (channel->ExecuteSql(create.c_str()) < 0) {
                        category = "permission_denied";
                    } else {
                        if (channel->ExecuteSql(("INSERT INTO " + table + " VALUES (1)").c_str()) < 0)
                            category = "permission_denied";
                        if (channel->ExecuteSql(("DROP TABLE " + table).c_str()) < 0) category = "permission_denied";
                    }
                }
            }
        }
    } catch (const std::exception& error) {
        if (category.empty()) {
            const std::string message = error.what();
            category = message.substr(0, message.find(':'));
        }
    }
    fflush(stdout);
    dup2(saved, STDOUT_FILENO);
    close(saved);
    fclose(quiet);
    std::printf("{\"category\":\"%s\",\"stage\":\"%s\",\"backend\":\"%s\",\"permissions\":\"%s\"}\n",
                category.empty() ? "ready" : category.c_str(), stage.c_str(), backend.c_str(),
                write && category.empty() ? "verified" : "unverified");
    return category.empty() ? 0 : 1;
}
