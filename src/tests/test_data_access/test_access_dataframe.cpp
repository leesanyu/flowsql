// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <framework/core/dataframe.h>
#include <framework/core/dataframe_channel.h>
#include <framework/interfaces/ichannel_registry.h>
#include <services/data_access/data_access_plugin.h>
#include <cassert>
#include <common/loader.hpp>
#include <filesystem>
#include <fstream>
using namespace flowsql;
using namespace flowsql::dataset;

int main() {
    char directory[] = "/tmp/flowsql-access-frame-XXXXXX";
    assert(mkdtemp(directory));
    const std::string root = directory;
    std::filesystem::create_directories(root + "/frames");
    auto* loader = PluginLoader::Single();
    struct Libraries {
        PluginLoader* loader;
        ~Libraries() {
            loader->StopAll();
            assert(loader->Unload() == 0);
        }
    } libraries{loader};
    const char* names[]{"libflowsql_config_channel.so", "libflowsql_builtin.so", "libflowsql_catalog.so"};
    const std::string config = "db_path=" + root + "/config.db";
    const std::string catalog = "data_dir=" + root + "/frames;operator_db_path=" + root + "/catalog.db";
    const char* options[]{config.c_str(), "", catalog.c_str()};
    assert(loader->Load(get_absolute_process_path(), names, options, 3) == 0 && loader->StartAll() == 0);
    auto* registry = static_cast<IChannelRegistry*>(loader->First(IID_CHANNEL_REGISTRY));
    assert(registry);
    auto frame = std::make_shared<DataFrameChannel>("dataframe", "precise");
    assert(frame->Open() == 0);
    arrow::Int64Builder ids;
    arrow::TimestampBuilder stamps(arrow::timestamp(arrow::TimeUnit::NANO, "Asia/Shanghai"),
                                   arrow::default_memory_pool());
    arrow::Decimal128Builder decimals(arrow::decimal128(38, 10));
    arrow::BinaryBuilder binary;
    assert(ids.AppendValues({9007199254740993LL, 9007199254740992LL}).ok());
    assert(stamps.AppendValues({1, 2}).ok());
    assert(decimals.Append(arrow::Decimal128("1230000000001")).ok() && decimals.AppendNull().ok());
    assert(binary.Append(std::string("\0\xff", 2)).ok() && binary.AppendNull().ok());
    DataFrame data;
    data.FromArrow(arrow::RecordBatch::Make(
        arrow::schema({arrow::field("id", arrow::int64()), arrow::field("stamp", stamps.type()),
                       arrow::field("price", decimals.type()), arrow::field("bytes", arrow::binary())}),
        2,
        {ids.Finish().ValueOrDie(), stamps.Finish().ValueOrDie(), decimals.Finish().ValueOrDie(),
         binary.Finish().ValueOrDie()}));
    assert(frame->Write(&data) == 0 && registry->Register("precise", frame) == 0);
    setenv("FLOWSQL_ACCESS_FRAME_TOKEN", "frame-test-credential-at-least-32-characters", 1);
    std::ofstream(root + "/policy.json")
        << R"({"subjects":[{"principal":"alice","token_env":"FLOWSQL_ACCESS_FRAME_TOKEN",
        "sources":[{"locator":{"kind":"dataframe","name":"precise"},"operations":["query"]}]}]})";
    httplib::Server reservation;
    const auto port = reservation.bind_to_any_port("127.0.0.1");
    assert(port > 0);
    std::thread listen([&] { reservation.listen_after_bind(); });
    reservation.wait_until_ready();
    reservation.stop();
    listen.join();
    Json config_json(rapidjson::kObjectType);
    auto& a = config_json.GetAllocator();
    config_json.AddMember("port", port, a);
    Set(config_json, "state_path", root + "/state.db", a);
    Set(config_json, "policy_file", root + "/policy.json", a);
    Set(config_json, "contract_file", std::string(FLOWSQL_SOURCE_DIR) + "/../config/agent-data-access/api.schema.json",
        a);
    access::DataAccessPlugin plugin;
    assert(plugin.Option(Encode(config_json).c_str()) == 0 && plugin.Load(loader) == 0 && plugin.Start() == 0);
    httplib::Client client("127.0.0.1", port);
    const httplib::Headers headers{{"Authorization", "Bearer frame-test-credential-at-least-32-characters"}};
    auto call = [&](const std::string& tool, const Value& args, int status = 200) {
        Json body(rapidjson::kObjectType);
        auto& b = body.GetAllocator();
        Set(body, "name", tool, b);
        body.AddMember("arguments", Copy(args, b), b);
        auto response = client.Post("/data-access/call", headers, Encode(body), "application/json");
        assert(response);
        if (response->status != status) fprintf(stderr, "%s\n", response->body.c_str());
        assert(response->status == status);
        return access::Parse(response->body);
    };
    auto sources = call("list_sources", access::Parse("{}"));
    assert(sources["sources"].Size() == 1);
    Json args(rapidjson::kObjectType);
    auto& b = args.GetAllocator();
    Set(args, "request_id", "precise-values", b);
    args.AddMember("source_ref", Copy(sources["sources"][0]["source_ref"], b), b);
    auto workflow = access::Parse(R"({"workflow":[{"type":"view","query":[{"op":"raw","fields":["f0","f1","f2","f3"]}]},
        {"type":"sort","by":["f0"],"sort":"ascending"}]})");
    args.AddMember("workflow", Copy(workflow["workflow"], b), b);
    assert(Get(call("query", args, 422)["error"], "code") == "SNAPSHOT_REQUIRED");
    args.AddMember("capture_snapshot", true, b);
    auto operation = call("query", args);
    Json status(rapidjson::kObjectType);
    Set(status, "operation_id", Get(operation, "operation_id"), status.GetAllocator());
    for (int i = 0; i < 500 && Get(operation, "status") != "completed"; ++i) {
        assert(Get(operation, "status") != "failed");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        operation = call("get_operation", status);
    }
    assert(Get(operation, "status") == "completed");
    Json read(rapidjson::kObjectType);
    Set(read, "result_ref", Get(operation, "result_ref"), read.GetAllocator());
    read.AddMember("limit", 1, read.GetAllocator());
    auto first = call("read_result", read);
    assert(Get(first["rows"][0][0], "value") == "9007199254740992");
    assert(Get(first["rows"][0][1], "value") == "1970-01-01 00:00:00.000000002Z");
    assert(Get(first["fields"][1], "native_type").find("Asia/Shanghai") != std::string::npos);
    assert(first["rows"][0][2].IsNull() && first["rows"][0][3].IsNull());
    read.AddMember("offset", 1, read.GetAllocator());
    auto second = call("read_result", read);
    assert(Get(second["rows"][0][2], "type") == "decimal" && Get(second["rows"][0][2], "value") == "123.0000000001");
    assert(Get(second["rows"][0][3], "type") == "binary_hex" && Get(second["rows"][0][3], "value") == "00ff");
    assert(Get(first, "content_sha256") == Get(second, "content_sha256"));
    assert(plugin.Stop() == 0);
    assert(registry->Unregister("precise") == 0);
}
