// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <arrow/api.h>
#include <httplib.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include <common/error_code.h>
#include <common/loader.hpp>
#include <framework/interfaces/iblock_stream_operator.h>
#include <framework/interfaces/ipacket.h>
#include <framework/interfaces/irouter_handle.h>

namespace {

namespace fs = std::filesystem;

class TempDirectory {
 public:
    TempDirectory() {
        static std::atomic<uint64_t> sequence{0};
        const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = fs::temp_directory_path() /
                ("flowsql-pcap-web-e2e-" + std::to_string(ticks) + "-" +
                 std::to_string(sequence.fetch_add(1)));
        std::error_code error;
        assert(fs::create_directories(path_, error));
        assert(!error);
    }

    ~TempDirectory() {
        std::error_code error;
        fs::remove_all(path_, error);
    }

    const fs::path& path() const { return path_; }

 private:
    fs::path path_;
};

class PacketCounter final : public flowsql::IBlockStreamOperator {
 public:
    std::string Category() override { return "test"; }
    std::string Name() override { return "packet_counter"; }
    std::string Description() override { return "Web upload E2E packet counter"; }
    int Configure(const char*, const char*) override { return 0; }

    int Init(const char*) override {
        rows_seen = 0;
        process_calls = 0;
        flush_calls = 0;
        schema_calls = 0;
        return 0;
    }

    int OnSchemaReady(std::shared_ptr<arrow::Schema> schema) override {
        ++schema_calls;
        return schema ? 0 : EINVAL;
    }

    int ProcessBlock(const std::shared_ptr<arrow::RecordBatch>& batch, int64_t) override {
        if (!batch) return EINVAL;
        ++process_calls;
        rows_seen += batch->num_rows();
        return 0;
    }

    int Flush() override {
        ++flush_calls;
        return 0;
    }

    std::string LastError() override { return "packet counter failed"; }

    int64_t rows_seen = 0;
    int process_calls = 0;
    int flush_calls = 0;
    int schema_calls = 0;
};

int ReserveLoopbackPort() {
    const int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(socket_fd >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    assert(bind(socket_fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);

    socklen_t address_size = sizeof(address);
    assert(getsockname(socket_fd, reinterpret_cast<sockaddr*>(&address), &address_size) == 0);
    const int port = ntohs(address.sin_port);
    close(socket_fd);
    return port;
}

void WaitForWeb(int port) {
    for (int attempt = 0; attempt < 200; ++attempt) {
        httplib::Client client("127.0.0.1", port);
        const auto response = client.Get("/api/health");
        if (response && response->status == 200) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(false && "Web HTTP server did not become ready");
}

std::string ReadBinary(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    assert(input.is_open());
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

size_t EntryCount(const fs::path& directory) {
    if (!fs::exists(directory)) return 0;
    return static_cast<size_t>(std::distance(fs::directory_iterator(directory), fs::directory_iterator()));
}

fs::path OnlyEntry(const fs::path& directory) {
    fs::directory_iterator entry(directory);
    assert(entry != fs::directory_iterator());
    const fs::path result = entry->path();
    ++entry;
    assert(entry == fs::directory_iterator());
    return result;
}

flowsql::fnRouterHandler FindRoute(flowsql::PluginLoader* loader,
                                   const std::string& method,
                                   const std::string& uri) {
    flowsql::fnRouterHandler handler;
    loader->Traverse(flowsql::IID_ROUTER_HANDLE, [&](void* value) {
        auto* router = static_cast<flowsql::IRouterHandle*>(value);
        router->EnumRoutes([&](const flowsql::RouteItem& route) {
            if (route.method == method && route.uri == uri) handler = route.handler;
        });
        return handler ? -1 : 0;
    });
    return handler;
}

size_t CountRoutes(flowsql::PluginLoader* loader,
                   const std::string& method,
                   const std::string& uri) {
    size_t count = 0;
    loader->Traverse(flowsql::IID_ROUTER_HANDLE, [&](void* value) {
        auto* router = static_cast<flowsql::IRouterHandle*>(value);
        router->EnumRoutes([&](const flowsql::RouteItem& route) {
            if (route.method == method && route.uri == uri) ++count;
        });
        return 0;
    });
    return count;
}

int HttpStatus(int32_t error_code) {
    switch (error_code) {
        case flowsql::error::OK:
            return 200;
        case flowsql::error::BAD_REQUEST:
            return 400;
        case flowsql::error::NOT_FOUND:
            return 404;
        case flowsql::error::CONFLICT:
            return 409;
        case flowsql::error::UNAVAILABLE:
            return 503;
        default:
            return 500;
    }
}

void BindSchedulerRoute(httplib::Server* server,
                        const std::string& http_path,
                        const std::string& scheduler_uri,
                        flowsql::fnRouterHandler handler) {
    assert(server != nullptr);
    assert(handler);
    server->Post(http_path, [scheduler_uri, handler = std::move(handler)](const httplib::Request& request,
                                                                         httplib::Response& response) {
        std::string body;
        const int32_t error_code = handler(scheduler_uri, request.body, body);
        response.status = HttpStatus(error_code);
        response.set_content(body, "application/json");
    });
}

httplib::Result UploadCapture(httplib::Client* client,
                              const std::string& channel_name,
                              const std::string& format,
                              const std::string& filename,
                              const std::string& content) {
    assert(client != nullptr);
    const httplib::MultipartFormDataItems fields = {
        {"name", channel_name, "", ""},
        {"format", format, "", ""},
        {"batch_packets", "2", "", ""},
        {"replay_mode", "fast", "", ""},
        {"replay_speed_milli", "1000", "", ""},
    };
    const httplib::MultipartFormDataProviderItems files = {
        {"file",
         [&content](size_t offset, httplib::DataSink& sink) {
             if (offset == content.size()) {
                 sink.done();
                 return true;
             }
             const size_t chunk = std::min<size_t>(64, content.size() - offset);
             sink.os.write(content.data() + offset, static_cast<std::streamsize>(chunk));
             return true;
         },
         filename,
         "application/vnd.tcpdump.pcap"},
    };
    return client->Post("/api/channels/pcapfile/upload", {}, fields, files);
}

std::string ChannelRequest(const std::string& name) {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    writer.StartObject();
    writer.Key("type");
    writer.String("pcapfile");
    writer.Key("name");
    writer.String(name.c_str());
    writer.EndObject();
    return buffer.GetString();
}

std::string ExecuteRequest(const std::string& name) {
    const std::string sql = "SELECT * FROM pcapfile." + name + " USING test.packet_counter";
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    writer.StartObject();
    writer.Key("sql");
    writer.String(sql.c_str());
    writer.EndObject();
    return buffer.GetString();
}

std::string ExecuteDataFrameRequest(const std::string& source_name,
                                    const std::string& dataframe_name) {
    const std::string sql = "SELECT * FROM pcapfile." + source_name +
                            " INTO dataframe." + dataframe_name;
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    writer.StartObject();
    writer.Key("sql");
    writer.String(sql.c_str());
    writer.EndObject();
    return buffer.GetString();
}

std::string PreviewRequest(const std::string& name, int page, int page_size) {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    writer.StartObject();
    writer.Key("category");
    writer.String("dataframe");
    writer.Key("name");
    writer.String(name.c_str());
    writer.Key("page");
    writer.Int(page);
    writer.Key("page_size");
    writer.Int(page_size);
    writer.EndObject();
    return buffer.GetString();
}

bool QueryContainsChannel(httplib::Client* client,
                          const std::string& name,
                          const std::string& forbidden_path = {}) {
    const auto response = client->Post("/api/channels/stream/query", "{}", "application/json");
    assert(response && response->status == 200);
    if (!forbidden_path.empty()) assert(response->body.find(forbidden_path) == std::string::npos);

    rapidjson::Document document;
    document.Parse(response->body.c_str());
    assert(!document.HasParseError() && document.IsObject());
    assert(document.HasMember("channels") && document["channels"].IsArray());
    for (const auto& channel : document["channels"].GetArray()) {
        assert(channel.IsObject());
        if (!channel.HasMember("name") || !channel["name"].IsString()) continue;
        if (name != channel["name"].GetString()) continue;
        assert(channel.HasMember("type") && channel["type"].IsString());
        assert(std::string(channel["type"].GetString()) == "pcapfile");
        assert(!channel.HasMember("path"));
        assert(!channel.HasMember("option"));
        assert(!channel.HasMember("options"));
        assert(!channel.HasMember("option_json"));
        return true;
    }
    return false;
}

void AssertPacketPreview(httplib::Client* client,
                         const std::string& dataframe_name,
                         int page,
                         int page_size,
                         int64_t total_rows,
                         rapidjson::SizeType expected_page_rows) {
    const auto preview = client->Post("/api/channels/dataframe/preview",
                                      PreviewRequest(dataframe_name, page, page_size),
                                      "application/json");
    assert(preview && preview->status == 200);

    rapidjson::Document document;
    document.Parse(preview->body.c_str());
    assert(!document.HasParseError() && document.IsObject());
    assert(document.HasMember("entity") && document["entity"].IsString());
    assert(std::string(document["entity"].GetString()) == "packet");
    assert(document.HasMember("rows") && document["rows"].IsInt64());
    assert(document["rows"].GetInt64() == total_rows);
    assert(document.HasMember("page") && document["page"].IsInt());
    assert(document["page"].GetInt() == page);
    assert(document.HasMember("page_size") && document["page_size"].IsInt());
    assert(document["page_size"].GetInt() == page_size);
    assert(document.HasMember("columns") && document["columns"].IsArray());
    assert(document.HasMember("data") && document["data"].IsArray());
    assert(document["data"].Size() == expected_page_rows);

    rapidjson::SizeType raw_index = document["columns"].Size();
    rapidjson::SizeType layer_status_index = document["columns"].Size();
    rapidjson::SizeType layer_count_index = document["columns"].Size();
    rapidjson::SizeType layer_ids_index = document["columns"].Size();
    rapidjson::SizeType layer_offsets_index = document["columns"].Size();
    for (rapidjson::SizeType index = 0; index < document["columns"].Size(); ++index) {
        const auto& column = document["columns"][index];
        if (!column.IsString()) continue;
        const std::string name = column.GetString();
        if (name == "raw_data") raw_index = index;
        if (name == "layer_status") layer_status_index = index;
        if (name == "layer_count") layer_count_index = index;
        if (name == "layer_ids") layer_ids_index = index;
        if (name == "layer_offsets") layer_offsets_index = index;
    }
    assert(raw_index < document["columns"].Size());
    assert(layer_status_index < document["columns"].Size());
    assert(layer_count_index < document["columns"].Size());
    assert(layer_ids_index < document["columns"].Size());
    assert(layer_offsets_index < document["columns"].Size());

    bool has_decoded_layers = false;
    for (const auto& row : document["data"].GetArray()) {
        assert(row.IsArray() && row.Size() == document["columns"].Size());
        const auto& raw = row[raw_index];
        assert(raw.IsObject());
        assert(raw.HasMember("hex") && raw["hex"].IsString());
        assert(raw.HasMember("byte_length") && raw["byte_length"].IsUint64());
        assert(raw.HasMember("truncated") && raw["truncated"].IsBool());
        const std::string hex = raw["hex"].GetString();
        const uint64_t byte_length = raw["byte_length"].GetUint64();
        assert(hex.size() <= 128 && hex.size() % 2 == 0);
        for (const char value : hex) {
            assert((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'));
        }
        if (raw["truncated"].GetBool()) {
            assert(byte_length > 64 && hex.size() == 128);
        } else {
            assert(byte_length <= 64 && hex.size() == byte_length * 2);
        }

        assert(row[layer_status_index].IsUint());
        assert(row[layer_count_index].IsUint());
        has_decoded_layers = has_decoded_layers || row[layer_count_index].GetUint() > 0;

        const auto& layer_ids = row[layer_ids_index];
        const auto& layer_offsets = row[layer_offsets_index];
        assert(layer_ids.IsArray());
        assert(layer_offsets.IsArray());
        assert(layer_ids.Size() == rapidjson::SizeType(flowsql::packet::kMaxLayerDepth));
        assert(layer_offsets.Size() == rapidjson::SizeType(flowsql::packet::kMaxLayerDepth));
        for (rapidjson::SizeType index = 0; index < layer_ids.Size(); ++index) {
            assert(layer_ids[index].IsUint());
            assert(layer_offsets[index].IsUint());
        }
    }
    assert(has_decoded_layers);
}

void RunSuccessfulCapture(httplib::Client* client,
                          PacketCounter* counter,
                          const fs::path& managed_root,
                          const fs::path& fixture,
                          const std::string& format,
                          const std::string& channel_name) {
    const std::string content = ReadBinary(fixture);
    assert(!content.empty());
    const auto upload = UploadCapture(client, channel_name, format, fixture.filename().string(), content);
    assert(upload && upload->status == 200);

    rapidjson::Document upload_response;
    upload_response.Parse(upload->body.c_str());
    assert(!upload_response.HasParseError() && upload_response.IsObject());
    assert(upload_response.HasMember("status") && upload_response["status"].IsString());
    assert(std::string(upload_response["status"].GetString()) == "running");
    assert(upload_response.HasMember("size_bytes") && upload_response["size_bytes"].IsUint64());
    assert(upload_response["size_bytes"].GetUint64() == content.size());
    assert(!upload_response.HasMember("path"));
    assert(!upload_response.HasMember("option"));
    assert(!upload_response.HasMember("options"));
    assert(!upload_response.HasMember("option_json"));

    assert(EntryCount(managed_root) == 1);
    const fs::path managed_file = OnlyEntry(managed_root);
    assert(managed_file.is_absolute());
    assert(ReadBinary(managed_file) == content);
    assert(upload->body.find(managed_file.string()) == std::string::npos);
    assert(QueryContainsChannel(client, channel_name, managed_file.string()));

    const auto execute = client->Post("/api/tasks/batch/execute",
                                      ExecuteRequest(channel_name),
                                      "application/json");
    assert(execute && execute->status == 200);
    rapidjson::Document execute_response;
    execute_response.Parse(execute->body.c_str());
    assert(!execute_response.HasParseError() && execute_response.IsObject());
    assert(execute_response.HasMember("status") && execute_response["status"].IsString());
    assert(std::string(execute_response["status"].GetString()) == "completed");
    assert(execute_response.HasMember("rows") && execute_response["rows"].IsInt64());
    assert(execute_response["rows"].GetInt64() > 0);
    assert(execute_response["rows"].GetInt64() == counter->rows_seen);
    assert(counter->schema_calls == 1);
    assert(counter->process_calls > 0);
    assert(counter->flush_calls == 1);

    assert(QueryContainsChannel(client, channel_name, managed_file.string()));
    const auto removed = client->Post("/api/channels/stream/remove",
                                      ChannelRequest(channel_name),
                                      "application/json");
    assert(removed && removed->status == 200);
    assert(!fs::exists(managed_file));
    assert(EntryCount(managed_root) == 0);
    assert(!QueryContainsChannel(client, channel_name));
}

void RunInvalidCapture(httplib::Client* client, const fs::path& managed_root) {
    const std::string channel_name = "invalid_capture";
    const auto upload = UploadCapture(client,
                                      channel_name,
                                      "pcap",
                                      "invalid.pcap",
                                      "this is not a valid pcap header");
    assert(upload && upload->status == 400);
    rapidjson::Document response;
    response.Parse(upload->body.c_str());
    assert(!response.HasParseError() && response.IsObject());
    assert(response.HasMember("error") && response["error"].IsString());
    assert(std::string(response["error"].GetString()) == "invalid_request");
    assert(EntryCount(managed_root) == 0);
    assert(!QueryContainsChannel(client, channel_name));
}

void RunDataFrameCapture(httplib::Client* client,
                         const fs::path& managed_root,
                         const fs::path& fixture) {
    const std::string name = "web_packet_dataframe";
    const std::string content = ReadBinary(fixture);
    const auto upload = UploadCapture(client, name, "pcapng", fixture.filename().string(), content);
    assert(upload && upload->status == 200);
    assert(EntryCount(managed_root) == 1);
    const fs::path managed_file = OnlyEntry(managed_root);

    const auto execute = client->Post("/api/tasks/batch/execute",
                                      ExecuteDataFrameRequest(name, name),
                                      "application/json");
    assert(execute && execute->status == 200);
    rapidjson::Document execute_response;
    execute_response.Parse(execute->body.c_str());
    assert(!execute_response.HasParseError() && execute_response.IsObject());
    assert(execute_response.MemberCount() == 4);
    assert(!execute_response.HasMember("data"));
    assert(execute_response.HasMember("status") && execute_response["status"].IsString());
    assert(std::string(execute_response["status"].GetString()) == "completed");
    assert(execute_response.HasMember("rows") && execute_response["rows"].IsInt64());
    assert(execute_response.HasMember("result_target") && execute_response["result_target"].IsString());
    assert(std::string(execute_response["result_target"].GetString()) == "dataframe." + name);
    assert(execute_response.HasMember("result_row_count") && execute_response["result_row_count"].IsInt64());
    const int64_t total_rows = execute_response["result_row_count"].GetInt64();
    assert(total_rows > 2);
    assert(execute_response["rows"].GetInt64() == total_rows);

    AssertPacketPreview(client, name, 1, 2, total_rows, 2);

    const auto removed = client->Post("/api/channels/stream/remove",
                                      ChannelRequest(name),
                                      "application/json");
    assert(removed && removed->status == 200);
    assert(!fs::exists(managed_file));
    assert(EntryCount(managed_root) == 0);
    assert(!QueryContainsChannel(client, name));

    const int last_page = static_cast<int>((total_rows + 1) / 2);
    const auto last_page_rows = static_cast<rapidjson::SizeType>(total_rows - (last_page - 1) * 2);
    AssertPacketPreview(client, name, last_page, 2, total_rows, last_page_rows);
}

}  // namespace

int main() {
    TempDirectory temp;
    const fs::path upload_dir = temp.path() / "uploads";
    const fs::path managed_root = upload_dir / "pcapfile";
    const fs::path dataframe_dir = temp.path() / "dataframes";
    const fs::path operator_db_dir = temp.path() / "operator_catalog";
    const int web_port = ReserveLoopbackPort();

    httplib::Server scheduler_http;
    const int scheduler_port = scheduler_http.bind_to_any_port("127.0.0.1");
    assert(scheduler_port > 0);

    PacketCounter counter;
    flowsql::PluginLoader* loader = flowsql::PluginLoader::Single();
    loader->Regist(flowsql::IID_BLOCK_STREAM_OPERATOR, &counter);

    const char* libraries[] = {
        "libflowsql_npi.so",
        "libflowsql_pcapfile.so",
        "libflowsql_scheduler.so",
        "libflowsql_builtin.so",
        "libflowsql_catalog.so",
        "libflowsql_web.so",
    };
    const std::string npi_option =
        std::string("{\"ldfile\":\"") + FLOWSQL_NPI_PROTOCOLS_PATH + "\"}";
    const std::string web_option =
        "host=127.0.0.1;port=" + std::to_string(web_port) +
        ";db_path=:memory:;upload_dir=" + upload_dir.string() +
        ";gateway=127.0.0.1:" + std::to_string(scheduler_port);
    const std::string catalog_option =
        "data_dir=" + dataframe_dir.string() + ";operator_db_dir=" + operator_db_dir.string();
    const char* options[] = {
        npi_option.c_str(), nullptr, nullptr, nullptr, catalog_option.c_str(), web_option.c_str()};
    assert(loader->Load(flowsql::get_absolute_process_path(), libraries, options, 6) == 0);
    assert(CountRoutes(loader, "POST", "/channels/dataframe/preview") == 1);

    BindSchedulerRoute(&scheduler_http,
                       "/channels/stream/add",
                       "/channels/stream/add",
                       FindRoute(loader, "POST", "/channels/stream/add"));
    BindSchedulerRoute(&scheduler_http,
                       "/channels/stream/query",
                       "/channels/stream/query",
                       FindRoute(loader, "POST", "/channels/stream/query"));
    BindSchedulerRoute(&scheduler_http,
                       "/channels/stream/remove",
                       "/channels/stream/remove",
                       FindRoute(loader, "POST", "/channels/stream/remove"));
    BindSchedulerRoute(&scheduler_http,
                       "/tasks/batch/execute",
                       "/scheduler/batch/execute",
                       FindRoute(loader, "POST", "/scheduler/batch/execute"));
    BindSchedulerRoute(&scheduler_http,
                       "/channels/dataframe/preview",
                       "/channels/dataframe/preview",
                       FindRoute(loader, "POST", "/channels/dataframe/preview"));
    std::thread scheduler_thread([&]() { assert(scheduler_http.listen_after_bind()); });
    scheduler_http.wait_until_ready();

    assert(loader->StartAll() == 0);
    WaitForWeb(web_port);
    assert(fs::is_directory(managed_root));

    httplib::Client web("127.0.0.1", web_port);
    web.set_read_timeout(20);
    RunSuccessfulCapture(&web,
                         &counter,
                         managed_root,
                         FLOWSQL_PCAP_FIXTURE_PATH,
                         "pcap",
                         "web_pcap");
    RunSuccessfulCapture(&web,
                         &counter,
                         managed_root,
                         FLOWSQL_PCAPNG_FIXTURE_PATH,
                         "pcapng",
                         "web_pcapng");
    RunDataFrameCapture(&web, managed_root, FLOWSQL_PCAPNG_FIXTURE_PATH);
    RunInvalidCapture(&web, managed_root);

    loader->StopAll();
    scheduler_http.stop();
    scheduler_thread.join();
    loader->Unload();
    assert(EntryCount(managed_root) == 0);

    std::cout << "Web upload, Scheduler consumption, DataFrame preview, and managed delete E2E passed\n";
    return 0;
}
