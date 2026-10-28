// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <framework/interfaces/irouter_handle.h>
#include <sqlite3.h>
#include <cassert>
#include <common/channel_dataset.hpp>
#include <common/loader.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace flowsql;
using namespace flowsql::dataset;
struct Runtime {
    PluginLoader* loader = PluginLoader::Single();
    ~Runtime() {
        loader->StopAll();
        assert(loader->Unload() == 0);
    }
};
int main(int argc, char** argv) {
    assert(argc == 6);
    const std::string root = argv[1], gateway = argv[2], router = argv[3], web = argv[4], mode = argv[5];
    std::filesystem::create_directories(root + "/frames");
    if (!std::filesystem::exists(root + "/frames/sample.csv"))
        std::ofstream(root + "/frames/sample.csv") << "id,value\n1,2.5\n";
    if (!std::filesystem::exists(root + "/key"))
        std::ofstream(root + "/key", std::ios::binary).write("12345678901234567890123456789012", 32);
    Json kafka(rapidjson::kObjectType);
    auto& a = kafka.GetAllocator();
    Set(kafka, "db_path", root + "/kafka.db", a);
    Set(kafka, "master_key_file", root + "/key", a);
    Set(kafka, "runtime_directory", root + "/runtime", a);
    Runtime runtime;
    std::vector<std::string> libraries, options;
    auto add = [&](std::string lib, std::string option) {
        libraries.push_back(std::move(lib));
        options.push_back(std::move(option));
    };
    if (mode != "sources" && mode != "iid") add("libflowsql_gateway.so", "host=127.0.0.1;port=" + gateway);
    if (mode != "iid") add("libflowsql_router.so", "host=127.0.0.1;port=" + router + ";gateway=127.0.0.1:" + gateway);
    if (mode != "sources") {
        add("libflowsql_config_channel.so", "db_path=" + root + "/config.db;gateway=127.0.0.1:" + gateway);
        if (mode != "iid")
            add("libflowsql_web.so", "host=127.0.0.1;port=" + web + ";gateway=127.0.0.1:" + gateway +
                                         ";db_path=" + root + "/web.db;upload_dir=" + root + "/uploads");
    }
    if (mode != "control") {
        add("libflowsql_builtin.so", "");
        add("libflowsql_catalog.so", "data_dir=" + root + "/frames;operator_db_path=" + root + "/catalog.db");
        add("libflowsql_kafka.so", Encode(kafka));
        add("libflowsql_database.so", "config_file=" + root + "/database.yml");
    }
    std::vector<const char*> libs, opts;
    for (size_t i = 0; i < libraries.size(); ++i) {
        libs.push_back(libraries[i].c_str());
        opts.push_back(options[i].c_str());
    }
    assert(runtime.loader->Load(get_absolute_process_path(), libs.data(), opts.data(), libs.size()) == 0);
    auto route = [&](const std::string& path, const std::string& req, std::string& rsp) {
        bool found = false;
        int rc = -1;
        runtime.loader->Traverse(IID_ROUTER_HANDLE, [&](void* p) {
            static_cast<IRouterHandle*>(p)->EnumRoutes([&](const RouteItem& r) {
                if (r.uri == path && r.method == "POST") {
                    found = true;
                    rc = r.handler(path, req, rsp);
                }
            });
            return 0;
        });
        assert(found);
        return rc;
    };
    if (mode != "control") {
        std::string response;
        assert(route("/channels/kafka/connections/query", R"({"name":"cluster"})", response) == 0);
        Json d;
        d.Parse(response.c_str());
        // Seed through the actual provider control route.
        if (d["items"].Empty()) {
            assert(
                route(
                    "/channels/kafka/connections/add",
                    R"({"name":"cluster","bootstrap_servers":["unused.invalid:9092"],"security_protocol":"PLAINTEXT"})",
                    response) == 0);
            assert(
                route(
                    "/channels/kafka/channels/add",
                    R"({"name":"events","connection_name":"cluster","role":"source","topic":"events","consumer_group":"test","payload_kind":"structured","payload_options":{"source":{"encoding":"json","entity":"events","fields":[{"name":"value","path":"/v","type":"float64","nullable":true,"unit":"ms"}]}}})",
                    response) == 0);
        }
    }
    assert(runtime.loader->StartAll() == 0);
    if (mode != "control") {
        sqlite3* db = nullptr;
        assert(sqlite3_open((root + "/source.db").c_str(), &db) == SQLITE_OK);
        assert(sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS empty_table(id INTEGER NOT NULL,amount DECIMAL(21,7))",
                            nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(db);
        std::string response;
        if (!std::filesystem::exists(root + "/database.yml")) {
            Json request(rapidjson::kObjectType);
            Set(request, "config", "type=sqlite;name=sample;path=" + root + "/source.db", request.GetAllocator());
            assert(route("/channels/database/add", Encode(request), response) == 0);
        }
    }
    std::cout << "DATASET_READY" << std::endl;
    std::string line;
    while (std::getline(std::cin, line) && line != "quit") {
        Json command;
        command.Parse(line.c_str());
        std::string response;
        if (command.HasMember("route")) {
            const auto rc = route(Get(command, "route"), Encode(command["request"]), response);
            std::cout << "DATASET_ROUTE " << rc << " " << response << std::endl;
        } else {
            const auto kind = Get(command, "kind");
            const std::string provider = kind == "kafka_source" ? "kafka"
                                         : kind == "dataframe"  ? "dataframe"
                                                                : "database";
            bool found = false;
            runtime.loader->Traverse(IID_CHANNEL_DATASET_DESCRIPTOR_V1, [&](void* p) {
                auto* descriptor = static_cast<IChannelDatasetDescriptorV1*>(p);
                if (provider == descriptor->ProviderId()) {
                    found = true;
                    assert(DescriptorControl(*descriptor, "{\"locator\":" + line + "}", response) == 0);
                    return 1;
                }
                return 0;
            });
            assert(found);
            std::cout << "DATASET_IID " << response << std::endl;
        }
    }
}
