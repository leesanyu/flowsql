// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <framework/core/dataframe.h>
#include <framework/core/dataframe_channel.h>
#include <framework/interfaces/ibinaddon_host.h>
#include <framework/interfaces/ichannel_registry.h>
#include <framework/interfaces/idatabase_factory.h>
#include <framework/interfaces/ioperator_catalog.h>
#include <framework/interfaces/irouter_handle.h>
#include <cassert>
#include <common/channel_dataset.hpp>
#include <common/loader.hpp>
#include <filesystem>
#include <iostream>
using namespace flowsql;
int main(int argc, char** argv) {
    assert(argc == 6 || argc == 7);
    auto* loader = PluginLoader::Single();
    struct Runtime {
        PluginLoader* loader;
        ~Runtime() {
            // Plugin-produced Arrow schemas and buffers must die before the
            // dynamic operator host releases its libraries during StopAll.
            auto* channels = static_cast<IChannelRegistry*>(loader->First(IID_CHANNEL_REGISTRY));
            if (channels) {
                std::vector<std::string> names;
                channels->List([&](const char* name, std::shared_ptr<IChannel>) { names.emplace_back(name); });
                for (const auto& name : names) channels->Unregister(name.c_str());
            }
            loader->StopAll();
            assert(loader->Unload() == 0);
        }
    } runtime{loader};
    const std::string root = argv[1], gateway = "127.0.0.1:" + std::string(argv[2]);
    const bool local = argc == 7 && std::string(argv[6]) == "--local";
    const bool provider = argc == 6 || local;
    std::vector<std::string> options{"host=127.0.0.1;port=" + std::string(argv[2]),
                                     "host=127.0.0.1;port=" + std::string(argv[3]) + ";gateway=" + gateway,
                                     "db_path=" + root + "/config.db",
                                     "",
                                     "data_dir=" + root + "/frames;operator_db_path=" + root + "/catalog.db",
                                     "operator_db_path=" + root + "/catalog.db;upload_dir=" + root + "/uploads",
                                     "worker_host=127.0.0.1;worker_port=" + std::string(argv[5]),
                                     "",
                                     "",
                                     "",
                                     "db_path=" + root + "/tasks.db",
                                     "host=127.0.0.1;port=" + std::string(argv[4]) + ";gateway=" + gateway +
                                         ";db_path=" + root + "/web.db;upload_dir=" + root + "/uploads"};
    options[7] = "config_file=" + root + "/database.yml";
    options[11] += ";worker_host=127.0.0.1;worker_port=" + std::string(argv[5]);
    const char* libraries[]{"libflowsql_gateway.so",   "libflowsql_router.so",   "libflowsql_config_channel.so",
                            "libflowsql_builtin.so",   "libflowsql_catalog.so",  "libflowsql_binaddon.so",
                            "libflowsql_bridge.so",    "libflowsql_database.so", "libflowsql_baseline.so",
                            "libflowsql_scheduler.so", "libflowsql_task.so",     "libflowsql_web.so"};
    std::vector<const char*> selected, pointers;
    for (size_t i = 0; i < options.size(); ++i) {
        if ((!provider && i == 2) || (local && (i < 2 || i == 6 || i == 10 || i == 11))) continue;
        selected.push_back(libraries[i]);
        pointers.push_back(options[i].c_str());
    }
    assert(loader->Load(get_absolute_process_path(), selected.data(), pointers.data(), selected.size()) == 0);
    assert(loader->StartAll() == 0);
    auto* channels = static_cast<IChannelRegistry*>(loader->First(IID_CHANNEL_REGISTRY));
    auto* catalog = static_cast<IOperatorCatalog*>(loader->First(IID_OPERATOR_CATALOG));
    assert(channels && catalog);
    auto frame = std::make_shared<DataFrameChannel>("dataframe", "input");
    assert(frame->Open() == 0);
    arrow::DoubleBuilder values;
    assert(values.AppendValues({1000., 2000., 3000.}).ok());
    DataFrame data;
    data.FromArrow(arrow::RecordBatch::Make(arrow::schema({arrow::field("latency", arrow::float64(), false)}), 3,
                                            {values.Finish().ValueOrDie()}));
    assert(frame->Write(&data) == 0 && channels->Register("input", frame) == 0);
    assert(catalog->SetActive("builtin", "passthrough", true) == 0);
    if (provider) {
        auto* host = static_cast<IBinAddonHost*>(loader->First(IID_BINADDON_HOST));
        assert(host);
        for (const auto& fixture :
             {std::make_pair(FIXTURE_OUTPUT_PATH, "output.so"), std::make_pair(BASELINER_PATH, "baseliner.so")}) {
            std::filesystem::copy_file(fixture.first, root + "/" + fixture.second);
            std::string response;
            assert(host->UploadCppPlugin(fixture.second, root + "/" + fixture.second, response) == 0);
            dataset::Json uploaded;
            uploaded.Parse(response.c_str());
            assert(host->ActivateCppPlugin(dataset::Get(uploaded, "plugin_id"), response) == 0);
        }
        assert(catalog->SetActive("contractdemo", "output", true) == 0);
        assert(catalog->SetActive("explore", "baseliner", true) == 0);
    }
    auto* databases = static_cast<IDatabaseFactory*>(loader->First(IID_DATABASE_FACTORY));
    assert(databases && databases->AddChannel(("type=sqlite;name=example;path=" + root + "/input.db").c_str()) == 0);
    auto* database = databases->Get("sqlite", "example");
    assert(database);
    for (const char* table : {"one", "two"}) {
        assert(database->ExecuteSql((std::string("CREATE TABLE ") + table +
                                     "(bucket INTEGER,series TEXT,value REAL,total INTEGER,\"group\" INTEGER)")
                                        .c_str()) == 0);
        assert(database->ExecuteSql((std::string("INSERT INTO ") + table + " VALUES(1,'a',2.5,4,1)").c_str()) >= 0);
    }
    std::cout << "CONTRACT_HOST_READY" << std::endl;
    std::string line;
    while (std::getline(std::cin, line) && line != "quit") {
        if (!local) continue;
        dataset::Json command;
        command.Parse(line.c_str());
        const auto path = dataset::Get(command, "path");
        std::string response;
        int rc = -2;
        loader->Traverse(IID_ROUTER_HANDLE, [&](void* value) {
            static_cast<IRouterHandle*>(value)->EnumRoutes([&](const RouteItem& route) {
                if (route.uri == path && route.method == "POST")
                    rc = route.handler(path, dataset::Encode(command["payload"]), response);
            });
            return 0;
        });
        dataset::Json result(rapidjson::kObjectType), body;
        body.Parse(response.c_str());
        result.AddMember("rc", rc, result.GetAllocator())
            .AddMember("result", dataset::Copy(body, result.GetAllocator()), result.GetAllocator());
        std::cout << "CONTRACT_LOCAL " << dataset::Encode(result) << std::endl;
    }
}
