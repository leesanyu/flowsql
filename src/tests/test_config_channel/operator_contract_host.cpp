// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <framework/core/dataframe.h>
#include <framework/core/dataframe_channel.h>
#include <framework/interfaces/ibinaddon_host.h>
#include <framework/interfaces/iblock_stream_manager.h>
#include <framework/interfaces/ichannel_registry.h>
#include <framework/interfaces/idatabase_exploration.h>
#include <framework/interfaces/idatabase_factory.h>
#include <framework/interfaces/ioperator_catalog.h>
#include <framework/interfaces/irouter_handle.h>
#include <tests/common/test_environment.h>
#include <cassert>
#include <common/channel_dataset.hpp>
#include <common/loader.hpp>
#include <filesystem>
#include <iostream>
#include <type_traits>
using namespace flowsql;

// Test-only native value oracle; it uses the same read-only interface on every
// backend and preserves narrow integers/NULL without the DataFrame JSON view.
dataset::Json NativeQuery(IDatabaseChannel* database, const std::string& sql) {
    auto* source = dynamic_cast<IDatabaseExplorationSourceV1*>(database);
    assert(source);
    DatabaseSnapshotOptionsV1 options;
    options.consistent_transaction = false;
    std::shared_ptr<IDatabaseExplorationSessionV1> session;
    assert(source->CreateExplorationSession(options, &session) == 0 && session);
    ExplorationResultV1 result;
    const auto rc = session->Query(sql.c_str(), nullptr, 0, 10000, 16 * 1024 * 1024, &result);
    if (rc) std::cerr << session->QueryError() << '\n';
    assert(rc == 0);
    dataset::Json response(rapidjson::kObjectType);
    auto& a = response.GetAllocator();
    dataset::Set(response, "status", "completed", a);
    response.AddMember("rows", static_cast<uint64_t>(result.rows.size()), a);
    dataset::Value data(rapidjson::kObjectType), columns(rapidjson::kArrayType), rows(rapidjson::kArrayType);
    for (const auto& column : result.columns) columns.PushBack(dataset::Value(column.name.c_str(), a), a);
    for (const auto& values : result.rows) {
        dataset::Value row(rapidjson::kArrayType);
        for (const auto& cell : values) {
            row.PushBack(std::visit(
                             [&](const auto& value) -> dataset::Value {
                                 using T = std::decay_t<decltype(value)>;
                                 if constexpr (std::is_same_v<T, std::monostate>)
                                     return {};
                                 else if constexpr (std::is_same_v<T, std::string>)
                                     return dataset::Value(value.c_str(), value.size(), a);
                                 else if constexpr (std::is_same_v<T, ExplorationDecimalV1>)
                                     return dataset::Value(value.text.c_str(), a);
                                 else if constexpr (std::is_same_v<T, std::vector<uint8_t>>) {
                                     dataset::Value bytes(rapidjson::kArrayType);
                                     for (auto byte : value) bytes.PushBack(static_cast<unsigned>(byte), a);
                                     return bytes;
                                 } else
                                     return dataset::Value(value);
                             },
                             cell),
                         a);
        }
        rows.PushBack(row, a);
    }
    data.AddMember("columns", columns, a).AddMember("data", rows, a);
    response.AddMember("data", data, a);
    return response;
}

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
    const bool npm = argc == 7 && (std::string(argv[6]) == "--npm" || std::string(argv[6]) == "--npm-http");
    const bool local = argc == 7 && (std::string(argv[6]) == "--local" || std::string(argv[6]) == "--npm");
    const bool provider = argc == 6 || local || npm;
    const char* configured_backend = npm ? std::getenv("FLOWSQL_TEST_NPM_BACKEND") : nullptr;
    const std::string backend = configured_backend ? configured_backend : "sqlite";
    std::vector<std::string> options{"host=127.0.0.1;port=" + std::string(argv[2]),
                                     "host=127.0.0.1;port=" + std::string(argv[3]) + ";gateway=" + gateway,
                                     "db_path=" + root + "/config.db;gateway=" + gateway,
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
    if (local) options[2] = "db_path=" + root + "/config.db";
    options[11] += ";worker_host=127.0.0.1;worker_port=" + std::string(argv[5]);
    dataset::Json npi(rapidjson::kObjectType);
    dataset::Set(npi, "ldfile", NPI_PROTOCOLS_PATH, npi.GetAllocator());
    options.push_back(dataset::Encode(npi));
    options.push_back("db_path=" + root + "/pcapfiles.db");
    options.push_back("");
    const char* libraries[]{"libflowsql_gateway.so",   "libflowsql_router.so",   "libflowsql_config_channel.so",
                            "libflowsql_builtin.so",   "libflowsql_catalog.so",  "libflowsql_binaddon.so",
                            "libflowsql_bridge.so",    "libflowsql_database.so", "libflowsql_baseline.so",
                            "libflowsql_scheduler.so", "libflowsql_task.so",     "libflowsql_web.so",
                            "libflowsql_npi.so",       "libflowsql_pcapfile.so", "libflowsql_flow_labeling.so"};
    std::vector<const char*> selected, pointers;
    for (size_t i = 0; i < options.size(); ++i) {
        if ((!provider && (i == 2 || i == 12)) || (local && (i < 2 || i == 6 || i == 10 || i == 11)) ||
            (!npm && i >= 13))
            continue;
        selected.push_back(libraries[i]);
        pointers.push_back(options[i].c_str());
    }
    assert(loader->Load(get_absolute_process_path(), selected.data(), pointers.data(), selected.size()) == 0);
    assert(loader->StartAll() == 0);
    if (npm) {
        auto* streams = static_cast<IBlockStreamManager*>(loader->First(IID_BLOCK_STREAM_MANAGER));
        assert(streams);
        bool exists = false;
        streams->QueryChannels([&](const auto& type, const auto& name, const auto&, const auto&) {
            if (type == "pcapfile" && name == "fixture") exists = true;
        });
        dataset::Json source(rapidjson::kObjectType);
        dataset::Set(source, "path", root + "/npm.pcap", source.GetAllocator());
        dataset::Set(source, "format", "auto", source.GetAllocator());
        source.AddMember("batch_packets", 4, source.GetAllocator());
        if (!exists) assert(streams->AddChannel("pcapfile", "fixture", dataset::Encode(source)) == 0);
    }
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
             {std::make_pair(FIXTURE_OUTPUT_PATH, "output.so"), std::make_pair(BASELINER_PATH, "baseliner.so"),
              std::make_pair(NPM_PATH, "npm.so")}) {
            std::string response, plugin_id;
            bool activated = false;
            assert(host->ListCppPlugins(response) == 0);
            dataset::Json installed;
            installed.Parse(response.c_str());
            for (const auto& item : installed["operators"].GetArray()) {
                if (dataset::Get(item["plugin"], "so_file") == fixture.second) {
                    plugin_id = dataset::Get(item, "plugin_id");
                    activated = dataset::Get(item["plugin"], "status") == "activated";
                }
            }
            if (plugin_id.empty()) {
                std::filesystem::copy_file(fixture.first, root + "/" + fixture.second,
                                           std::filesystem::copy_options::overwrite_existing);
                assert(host->UploadCppPlugin(fixture.second, root + "/" + fixture.second, response) == 0);
                dataset::Json uploaded;
                uploaded.Parse(response.c_str());
                plugin_id = dataset::Get(uploaded, "plugin_id");
            }
            if (!activated) {
                const int rc = host->ActivateCppPlugin(plugin_id, response);
                if (rc) std::cerr << fixture.second << ": " << response << '\n';
                assert(rc == 0);
            }
        }
        assert(catalog->SetActive("contractdemo", "output", true) == 0);
        assert(catalog->SetActive("explore", "baseliner", true) == 0);
        assert(catalog->SetActive("npm", "basic", true) == 0);
    }
    const bool fresh_database = !std::filesystem::exists(root + "/input.db");
    auto* databases = static_cast<IDatabaseFactory*>(loader->First(IID_DATABASE_FACTORY));
    assert(databases);
    if (!databases->Get(backend.c_str(), "example")) {
        const auto connection = backend == "sqlite" ? "type=sqlite;name=example;path=" + root + "/input.db"
                                                    : test::DatabaseOptions(backend, "example", "npm");
        assert(databases->AddChannel(connection.c_str()) == 0);
    }
    auto* database = databases->Get(backend.c_str(), "example");
    assert(database);
    for (const char* table : npm ? std::vector<const char*>{} : std::vector<const char*>{"one", "two"}) {
        assert(database->ExecuteSql((std::string("CREATE TABLE IF NOT EXISTS ") + table +
                                     "(bucket INTEGER,series TEXT,value REAL,total INTEGER,\"group\" INTEGER)")
                                        .c_str()) == 0);
        if (fresh_database)
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
        if (npm && path == "/test/database/query") {
            response = dataset::Encode(NativeQuery(database, dataset::Get(command["payload"], "sql")));
            rc = 0;
        } else {
            loader->Traverse(IID_ROUTER_HANDLE, [&](void* value) {
                static_cast<IRouterHandle*>(value)->EnumRoutes([&](const RouteItem& route) {
                    if (route.uri == path && route.method == "POST")
                        rc = route.handler(path, dataset::Encode(command["payload"]), response);
                });
                return 0;
            });
        }
        dataset::Json result(rapidjson::kObjectType), body;
        body.Parse(response.c_str());
        result.AddMember("rc", rc, result.GetAllocator())
            .AddMember("result", dataset::Copy(body, result.GetAllocator()), result.GetAllocator());
        std::cout << "CONTRACT_LOCAL " << dataset::Encode(result) << std::endl;
    }
}
