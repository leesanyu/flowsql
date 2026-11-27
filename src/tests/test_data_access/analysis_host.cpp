// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <framework/core/dataframe.h>
#include <framework/core/dataframe_channel.h>
#include <framework/interfaces/ibinaddon_host.h>
#include <framework/interfaces/iblock_stream_manager.h>
#include <framework/interfaces/ichannel_registry.h>
#include <framework/interfaces/idatabase_factory.h>
#include <framework/interfaces/ioperator_catalog.h>
#include <framework/interfaces/irouter_handle.h>
#include <services/data_access/data_access_plugin.h>
#include <cassert>
#include <common/loader.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace flowsql;
using namespace flowsql::dataset;
int main(int argc, char** argv) {
    assert(argc == 3);
    const std::string root = argv[1];
    auto* loader = PluginLoader::Single();
    struct Libraries {
        PluginLoader* loader;
        ~Libraries() {
            auto* registry = static_cast<IChannelRegistry*>(loader->First(IID_CHANNEL_REGISTRY));
            std::vector<std::string> names;
            registry->List([&](const char* name, std::shared_ptr<IChannel>) { names.emplace_back(name); });
            for (const auto& name : names) registry->Unregister(name.c_str());
            loader->StopAll();
            assert(loader->Unload() == 0);
        }
    } libraries{loader};
    const char* names[]{
        "libflowsql_config_channel.so", "libflowsql_builtin.so",  "libflowsql_catalog.so", "libflowsql_binaddon.so",
        "libflowsql_baseline.so",       "libflowsql_database.so", "libflowsql_npi.so",     "libflowsql_pcapfile.so",
        "libflowsql_flow_labeling.so",  "libflowsql_scheduler.so"};
    const std::vector<std::string> options{
        "db_path=" + root + "/config.db",
        "",
        "data_dir=" + root + "/frames;operator_db_path=" + root + "/catalog.db",
        "operator_db_path=" + root + "/catalog.db;upload_dir=" + root + "/uploads",
        "",
        "config_file=" + root + "/database.yml",
        std::string("{\"ldfile\":\"") + FLOWSQL_SOURCE_DIR + "/plugins/npi/conf/protocols.yml\"}",
        "db_path=" + root + "/pcap.db",
        "",
        ""};
    std::vector<const char*> pointers;
    for (const auto& option : options) pointers.push_back(option.c_str());
    assert(loader->Load(get_absolute_process_path(), names, pointers.data(), 10) == 0 && loader->StartAll() == 0);
    auto* databases = static_cast<IDatabaseFactory*>(loader->First(IID_DATABASE_FACTORY));
    assert(databases);
    for (const char* name : {"business", "results"}) {
        bool registered = false;
        databases->List([&](const char* type, const char* channel, const char*) {
            if (std::string(type) == "sqlite" && std::string(channel) == name) registered = true;
        });
        if (!registered)
            assert(databases->AddChannel(
                       ("type=sqlite;name=" + std::string(name) + ";path=" + root + "/" + name + ".db").c_str()) == 0);
    }
    auto* business = databases->Get("sqlite", "business");
    assert(
        business->ExecuteSql(
            "CREATE TABLE IF NOT EXISTS one(bucket INTEGER,series TEXT,value REAL,total INTEGER,\"group\" INTEGER)") >=
        0);
    if (!std::filesystem::exists(root + "/initialized")) {
        assert(business->ExecuteSql("INSERT INTO one VALUES(1,'a',2.5,4,1),(2,'a',3.5,4,1),(3,'a',4.5,4,1)") >= 0);
        std::ofstream(root + "/initialized").put('1');
    }
    assert(business->ExecuteSql("CREATE TABLE IF NOT EXISTS two AS SELECT * FROM one") >= 0);
    auto* registry = static_cast<IChannelRegistry*>(loader->First(IID_CHANNEL_REGISTRY));
    if (!registry->Get("input")) {
        auto input = std::make_shared<DataFrameChannel>("dataframe", "input");
        assert(input->Open() == 0);
        arrow::Int64Builder bucket;
        arrow::StringBuilder series;
        arrow::DoubleBuilder value;
        arrow::UInt64Builder total, group;
        assert(bucket.AppendValues({1, 2, 3}).ok() && series.AppendValues({"a", "a", "a"}).ok() &&
               value.AppendValues({2.5, 3.5, 4.5}).ok() && total.AppendValues({4, 4, 4}).ok() &&
               group.AppendValues({1, 1, 1}).ok());
        DataFrame data;
        data.FromArrow(arrow::RecordBatch::Make(
            arrow::schema({arrow::field("bucket", arrow::int64()), arrow::field("series", arrow::utf8()),
                           arrow::field("value", arrow::float64()), arrow::field("total", arrow::uint64()),
                           arrow::field("group", arrow::uint64())}),
            3,
            {bucket.Finish().ValueOrDie(), series.Finish().ValueOrDie(), value.Finish().ValueOrDie(),
             total.Finish().ValueOrDie(), group.Finish().ValueOrDie()}));
        assert(input->Write(&data) == 0 && registry->Register("input", input) == 0);
    }
    auto* host = static_cast<IBinAddonHost*>(loader->First(IID_BINADDON_HOST));
    auto* catalog = static_cast<IOperatorCatalog*>(loader->First(IID_OPERATOR_CATALOG));
    assert(host && catalog);
    for (const auto& component :
         {std::make_pair(NPM_PLUGIN_PATH, "npm.so"), std::make_pair(BASELINER_PLUGIN_PATH, "baseliner.so")}) {
        std::string response, id;
        bool active = false;
        assert(host->ListCppPlugins(response) == 0);
        auto installed = access::Parse(response);
        for (const auto& item : installed["operators"].GetArray())
            if (Get(item["plugin"], "so_file") == component.second) {
                id = Get(item, "plugin_id");
                active = Get(item["plugin"], "status") == "activated";
            }
        if (id.empty()) {
            const auto upload = root + "/" + component.second;
            std::filesystem::copy_file(component.first, upload, std::filesystem::copy_options::overwrite_existing);
            assert(host->UploadCppPlugin(component.second, upload, response) == 0);
            id = Get(access::Parse(response), "plugin_id");
        }
        if (!active) {
            const auto rc = host->ActivateCppPlugin(id, response);
            if (rc) std::cerr << response << '\n';
            assert(rc == 0);
        }
    }
    assert(catalog->SetActive("npm", "basic", true) == 0 && catalog->SetActive("explore", "baseliner", true) == 0);
    for (const char* name : {"pcap", "pcapng", "slow"}) {
        loader->Traverse(IID_BLOCK_STREAM_MANAGER, [&](void* pointer) {
            auto* manager = static_cast<IBlockStreamManager*>(pointer);
            bool found = false;
            manager->QueryChannels([&](const auto& type, const auto& channel, const auto&, const auto&) {
                if (type == "pcapfile" && channel == name) found = true;
            });
            if (found) return 0;
            Json option(rapidjson::kObjectType);
            Set(option, "path", root + "/" + name, option.GetAllocator());
            option.AddMember("batch_packets", 4, option.GetAllocator());
            const auto rc = manager->AddChannel("pcapfile", name, Encode(option));
            assert(rc == 0 || rc == ENOTSUP);
            return 0;
        });
    }
    Json option(rapidjson::kObjectType);
    auto& a = option.GetAllocator();
    option.AddMember("port", std::stoi(argv[2]), a);
    Set(option, "policy_file", root + "/policy.json", a);
    Set(option, "state_path", root + "/access.db", a);
    Set(option, "contract_file", std::string(FLOWSQL_SOURCE_DIR) + "/../config/agent-data-access/api.schema.json", a);
    access::DataAccessPlugin access;
    assert(access.Option(Encode(option).c_str()) == 0 && access.Load(loader) == 0 && access.Start() == 0);
    std::cout << "ACCESS_READY" << std::endl;
    std::string line;
    while (std::getline(std::cin, line) && line != "quit") {
        auto request = access::Parse(line);
        const auto path = Get(request, "path");
        std::string response;
        int rc = ENOENT;
        loader->Traverse(IID_ROUTER_HANDLE, [&](void* pointer) {
            static_cast<IRouterHandle*>(pointer)->EnumRoutes([&](const RouteItem& route) {
                if (route.method == "POST" && route.uri == path)
                    rc = route.handler(path, Encode(request["payload"]), response);
            });
            return rc == ENOENT ? 0 : 1;
        });
        Json result(rapidjson::kObjectType);
        result.AddMember("rc", rc, result.GetAllocator());
        if (!response.empty())
            result.AddMember("result", Copy(access::Parse(response), result.GetAllocator()), result.GetAllocator());
        std::cout << "ACCESS_REPLY " << Encode(result) << std::endl;
    }
    assert(access.Stop() == 0);
}
