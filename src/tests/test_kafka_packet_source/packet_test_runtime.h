// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <arrow/api.h>
#include <arrow/io/api.h>
#include <arrow/ipc/api.h>
#include <channels/kafka/kafka_packet.h>
#include <channels/kafka/kafka_store.h>
#include <framework/interfaces/ibinaddon_host.h>
#include <framework/interfaces/idatabase_factory.h>
#include <framework/interfaces/irouter_handle.h>
#include <tests/common/test_environment.h>
#include <chrono>
#include <common/loader.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

namespace packet_test {
using namespace flowsql;
using namespace flowsql::channels::kafka;
inline void Check(bool ok, const char* stage) {
    if (!ok) throw std::runtime_error(stage);
}
inline fnRouterHandler Route(PluginLoader* loader, const std::string& path) {
    fnRouterHandler found;
    loader->Traverse(IID_ROUTER_HANDLE, [&](void* p) {
        static_cast<IRouterHandle*>(p)->EnumRoutes([&](const RouteItem& item) {
            if (item.uri == path && item.method == "POST") found = item.handler;
        });
        return found ? -1 : 0;
    });
    Check(bool(found), "route_missing");
    return found;
}
// Declared before every plugin product; teardown waits for tasks and releases libraries last.
class Runtime {
 public:
    PluginLoader* loader = PluginLoader::Single();
    const std::filesystem::path root;
    explicit Runtime(std::filesystem::path workspace, const std::string& kafka_options = "", unsigned batch_workers = 1)
        : root(std::move(workspace)) {
        std::filesystem::create_directories(root);
        const auto database = "type=sqlite;name=results;path=" + (root / "results.db").string();
        const auto catalog =
            "data_dir=" + (root / "data").string() + ";operator_db_path=" + (root / "operators.db").string();
        const auto addon =
            "operator_db_path=" + (root / "operators.db").string() + ";upload_dir=" + (root / "uploads").string();
        const auto task = "db_path=" + (root / "tasks.db").string();
        const auto npi = std::string("{\"ldfile\":\"") + PACKET_NPI_CONFIG +
                         "\",\"concurrency\":" + std::to_string(batch_workers) + "}";
        const auto scheduler = "stream_workers=1;batch_workers=" + std::to_string(batch_workers);
        const char* libraries[] = {"libflowsql_database.so", "libflowsql_builtin.so",       "libflowsql_catalog.so",
                                   "libflowsql_npi.so",      "libflowsql_scheduler.so",     "libflowsql_binaddon.so",
                                   "libflowsql_task.so",     "libflowsql_flow_labeling.so", "libflowsql_kafka.so"};
        const char* options[] = {database.c_str(), nullptr,           catalog.c_str(),
                                 npi.c_str(),      scheduler.c_str(), addon.c_str(),
                                 task.c_str(),     nullptr,           kafka_options.c_str()};
        Check(loader->Load(get_absolute_process_path(), libraries, options, kafka_options.empty() ? 8 : 9) == 0,
              "plugins_load");
        libraries_.active = true;
        Check(loader->StartAll() == 0, "plugins_start");
        auto* host = static_cast<IBinAddonHost*>(loader->First(IID_BINADDON_HOST));
        Check(host, "binaddon_missing");
        const auto upload = root / "npm_basic.so";
        std::filesystem::copy_file(PACKET_NPM_LIBRARY, upload);
        std::string response;
        Check(host->UploadCppPlugin("npm_basic.so", upload.string(), response) == 0, "npm_upload");
        auto uploaded = Parse(response);
        Check(host->ActivateCppPlugin(Text(uploaded, "plugin_id"), response) == 0, "npm_activate");
    }
    Json Call(const std::string& path, const rapidjson::Value& request, int expected = 0) {
        std::string response;
        const auto rc = Route(loader, path)(path, Encode(request), response);
        if (rc != expected) std::cerr << "path=" << path << " rc=" << rc << " response=" << response << '\n';
        Check(rc == expected, "route_status");
        return Parse(response);
    }
    Json Submit(const std::string& id, const std::string& sql) {
        auto request = Parse("{}");
        auto& a = request.GetAllocator();
        String(request, "runtime_task_id", id, a);
        String(request, "sql_text", sql, a);
        return Call("/scheduler/batch/submit", request);
    }
    Json Status(const std::string& id, const std::string& action = "status") {
        auto request = Parse("{}");
        String(request, "runtime_task_id", id, request.GetAllocator());
        if (action == "cancel") String(request, "mode", "cancel", request.GetAllocator());
        return Call("/scheduler/batch/" + (action == "cancel" ? std::string("stop") : action), request);
    }
    Json Terminal(const std::string& id) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        do {
            auto result = Status(id);
            auto status = Text(result, "status");
            if (status == "stopped" || status == "cancelled" || status == "failed" || status == "completed")
                return result;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        } while (std::chrono::steady_clock::now() < deadline);
        throw std::runtime_error("task_terminal_timeout");
    }
    std::vector<std::shared_ptr<arrow::RecordBatch>> Query(const std::string& sql) {
        auto* factory = static_cast<IDatabaseFactory*>(loader->First(IID_DATABASE_FACTORY));
        auto* db = factory ? factory->Get("sqlite", "results") : nullptr;
        Check(db, "sqlite_missing");
        IBatchReader* raw = nullptr;
        const auto rc = db->CreateReader(sql.c_str(), &raw);
        if (rc) std::cerr << "query=" << sql << " error=" << db->GetLastError() << '\n';
        Check(!rc && raw, "sqlite_query");
        const auto release = [](IBatchReader* value) {
            value->Close();
            value->Release();
        };
        std::unique_ptr<IBatchReader, decltype(release)> reader(raw, release);
        std::vector<std::shared_ptr<arrow::RecordBatch>> result;
        for (;;) {
            const uint8_t* bytes = nullptr;
            size_t size = 0;
            const auto next = reader->Next(&bytes, &size);
            if (next == 1) break;
            Check(next == 0 && bytes && size, "sqlite_read");
            auto input = std::make_shared<arrow::io::BufferReader>(
                arrow::Buffer::FromString(std::string(reinterpret_cast<const char*>(bytes), size)));
            auto stream = arrow::ipc::RecordBatchStreamReader::Open(input).ValueOrDie();
            for (;;) {
                std::shared_ptr<arrow::RecordBatch> batch;
                Check(stream->ReadNext(&batch).ok(), "sqlite_ipc");
                if (!batch) break;
                result.push_back(std::move(batch));
            }
        }
        return result;
    }

 private:
    struct Libraries {
        bool active = false;
        ~Libraries() {
            if (active) {
                PluginLoader::Single()->StopAll();
                PluginLoader::Single()->Unload();
            }
        }
    } libraries_;
};
}  // namespace packet_test
