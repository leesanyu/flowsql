// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <services/web/pcap_upload_transaction.hpp>
#include <services/web/web_plugin.h>

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
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using flowsql::web::BuildPublicPcapUploadJson;
using flowsql::web::BuildSchedulerPcapAddJson;
using flowsql::web::CheckPcapUploadSize;
using flowsql::web::ManagedCaptureRef;
using flowsql::web::ManagedCaptureStore;
using flowsql::web::ManagedCaptureUpload;
using flowsql::web::MapPcapUploadError;
using flowsql::web::ParsePcapUploadFields;
using flowsql::web::PcapUploadError;
using flowsql::web::PcapUploadFields;
using flowsql::web::PcapUploadRequest;
using flowsql::web::PcapUploadTransaction;
using flowsql::web::WebPlugin;

namespace fs = std::filesystem;

class TempDirectory {
 public:
    TempDirectory() {
        static std::atomic<uint64_t> sequence{0};
        const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = fs::temp_directory_path() /
                ("flowsql_managed_capture_" + std::to_string(ticks) + "_" +
                 std::to_string(sequence.fetch_add(1)));
        std::error_code ec;
        assert(fs::create_directories(path_, ec));
        assert(!ec);
    }

    ~TempDirectory() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    const fs::path& path() const { return path_; }

 private:
    fs::path path_;
};

class CurrentPathGuard {
 public:
    explicit CurrentPathGuard(const fs::path& next) : previous_(fs::current_path()) {
        fs::current_path(next);
    }

    ~CurrentPathGuard() {
        std::error_code ec;
        fs::current_path(previous_, ec);
    }

 private:
    fs::path previous_;
};

std::string ReadFile(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

size_t EntryCount(const fs::path& path) {
    return static_cast<size_t>(std::distance(fs::directory_iterator(path), fs::directory_iterator()));
}

fs::path OnlyEntryPath(const fs::path& path) {
    fs::directory_iterator entry(path);
    assert(entry != fs::directory_iterator());
    const fs::path result = entry->path();
    ++entry;
    assert(entry == fs::directory_iterator());
    return result;
}

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

void WaitForHttpServer(int port) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        httplib::Client client("127.0.0.1", port);
        const auto result = client.Get("/api/health");
        if (result && result->status == 200) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(false && "Web HTTP server did not become ready");
}

httplib::Result PostCapture(httplib::Client* client,
                            httplib::MultipartFormDataItems fields,
                            const std::string& filename,
                            const std::string& content) {
    assert(client != nullptr);
    httplib::MultipartFormDataProviderItems files = {
        {"file",
         [&content](size_t offset, httplib::DataSink& sink) {
             if (offset == content.size()) {
                 sink.done();
                 return true;
             }
             const size_t chunk_size = std::min<size_t>(2, content.size() - offset);
             sink.os.write(content.data() + offset, static_cast<std::streamsize>(chunk_size));
             return true;
         },
         filename,
         "application/vnd.tcpdump.pcap"},
    };
    return client->Post("/api/channels/pcapfile/upload", {}, fields, files);
}

void ExpectHttpError(const httplib::Result& result,
                     int expected_status,
                     const std::string& expected_error) {
    assert(result);
    assert(result->status == expected_status);
    rapidjson::Document response;
    response.Parse(result->body.c_str());
    assert(!response.HasParseError() && response.IsObject());
    assert(response.HasMember("error") && response["error"].IsString());
    assert(std::string(response["error"].GetString()) == expected_error);
}

enum class QueryPathKind {
    kMissing,
    kString,
    kNumber,
};

struct QueryChannel {
    std::string type;
    std::string name;
    QueryPathKind path_kind = QueryPathKind::kMissing;
    std::string path;
};

std::string BuildStreamQueryResponse(const std::vector<QueryChannel>& channels) {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    writer.StartObject();
    writer.Key("channels");
    writer.StartArray();
    for (const auto& channel : channels) {
        writer.StartObject();
        writer.Key("type");
        writer.String(channel.type.c_str());
        writer.Key("name");
        writer.String(channel.name.c_str());
        writer.Key("option_json");
        writer.StartObject();
        if (channel.path_kind != QueryPathKind::kMissing) {
            writer.Key("path");
            if (channel.path_kind == QueryPathKind::kString) {
                writer.String(channel.path.c_str());
            } else {
                writer.Int(7);
            }
        }
        writer.EndObject();
        writer.EndObject();
    }
    writer.EndArray();
    writer.EndObject();
    return buffer.GetString();
}

httplib::Result PostRemove(httplib::Client* client,
                           const std::string& type,
                           const std::string& name) {
    assert(client != nullptr);
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    writer.StartObject();
    writer.Key("type");
    writer.String(type.c_str());
    writer.Key("name");
    writer.String(name.c_str());
    writer.EndObject();
    return client->Post("/api/channels/stream/remove", buffer.GetString(), "application/json");
}

std::string BuildStreamRedactionFixture(const std::string& pcap_path,
                                        const std::string& ordinary_path) {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    writer.StartObject();
    writer.Key("cursor");
    writer.String("next");
    writer.Key("channels");
    writer.StartArray();

    writer.StartObject();
    writer.Key("type");
    writer.String("pcapfile");
    writer.Key("name");
    writer.String("capture");
    writer.Key("role");
    writer.String("source");
    writer.Key("status");
    writer.String("running");
    writer.Key("in_use");
    writer.Bool(false);
    writer.Key("is_finite");
    writer.Bool(true);
    writer.Key("is_finished");
    writer.Bool(false);
    writer.Key("path");
    writer.String(pcap_path.c_str());
    writer.Key("option");
    const std::string pcap_option = "{\"path\":\"" + pcap_path + "\",\"format\":\"pcap\"}";
    writer.String(pcap_option.c_str());
    writer.Key("options");
    writer.StartObject();
    writer.Key("path");
    writer.String(pcap_path.c_str());
    writer.Key("batch_packets");
    writer.Uint(256);
    writer.EndObject();
    writer.Key("option_json");
    writer.StartObject();
    writer.Key("path");
    writer.String(pcap_path.c_str());
    writer.Key("replay_mode");
    writer.String("fast");
    writer.EndObject();
    writer.Key("visible_metadata");
    writer.String("keep");
    writer.EndObject();

    writer.StartObject();
    writer.Key("type");
    writer.String("memory");
    writer.Key("name");
    writer.String("ordinary");
    writer.Key("path");
    writer.String(ordinary_path.c_str());
    writer.Key("option");
    writer.String("ordinary-option");
    writer.Key("options");
    writer.StartObject();
    writer.Key("path");
    writer.String(ordinary_path.c_str());
    writer.EndObject();
    writer.Key("option_json");
    writer.StartObject();
    writer.Key("path");
    writer.String(ordinary_path.c_str());
    writer.EndObject();
    writer.EndObject();

    writer.EndArray();
    writer.EndObject();
    return buffer.GetString();
}

void AssertStreamQueryRedacted(const std::string& body,
                               const std::string& pcap_path,
                               const std::string& ordinary_path) {
    assert(body.find(pcap_path) == std::string::npos);
    assert(body.find(ordinary_path) != std::string::npos);

    rapidjson::Document response;
    response.Parse(body.c_str());
    assert(!response.HasParseError() && response.IsObject());
    assert(response.HasMember("cursor") && std::string(response["cursor"].GetString()) == "next");
    assert(response.HasMember("channels") && response["channels"].IsArray());
    assert(response["channels"].Size() == 2);

    const rapidjson::Value* pcap = nullptr;
    const rapidjson::Value* ordinary = nullptr;
    for (const auto& channel : response["channels"].GetArray()) {
        assert(channel.IsObject() && channel.HasMember("type") && channel["type"].IsString());
        const std::string type = channel["type"].GetString();
        if (type == "pcapfile") pcap = &channel;
        if (type == "memory") ordinary = &channel;
    }
    assert(pcap != nullptr && ordinary != nullptr);
    for (const char* field : {"path", "option", "options", "option_json"}) {
        assert(!pcap->HasMember(field));
        assert(ordinary->HasMember(field));
    }
    assert(std::string((*pcap)["name"].GetString()) == "capture");
    assert(std::string((*pcap)["role"].GetString()) == "source");
    assert(std::string((*pcap)["status"].GetString()) == "running");
    assert(!(*pcap)["in_use"].GetBool());
    assert((*pcap)["is_finite"].GetBool());
    assert(!(*pcap)["is_finished"].GetBool());
    assert(std::string((*pcap)["visible_metadata"].GetString()) == "keep");
    assert(std::string((*ordinary)["option"].GetString()) == "ordinary-option");
    assert(std::string((*ordinary)["option_json"]["path"].GetString()) == ordinary_path);
}

void ExpectInvalid(const PcapUploadFields& fields, const std::string& filename) {
    PcapUploadRequest request;
    std::string message;
    assert(ParsePcapUploadFields(fields, filename, &request, &message) == PcapUploadError::kInvalidRequest);
    assert(!message.empty());
}

void TestDefaultsAndBoundaries() {
    PcapUploadRequest request;
    std::string message;
    assert(ParsePcapUploadFields({{"name", "capture_01"}}, "capture.pcap", &request, &message) ==
           PcapUploadError::kOk);
    assert(message.empty());
    assert(request.channel_name == "capture_01");
    assert(request.original_filename == "capture.pcap");
    assert(request.format == "auto");
    assert(request.batch_packets == 256);
    assert(request.replay_mode == "fast");
    assert(request.replay_speed_milli == 1000);

    const PcapUploadFields boundaries = {
        {"name", "capture.02"},
        {"format", "pcapng"},
        {"batch_packets", "1"},
        {"replay_mode", "timestamp"},
        {"replay_speed_milli", "4294967295"},
    };
    assert(ParsePcapUploadFields(boundaries, "capture.pcapng", &request, &message) == PcapUploadError::kOk);
    assert(request.channel_name == "capture.02");
    assert(request.format == "pcapng");
    assert(request.batch_packets == 1);
    assert(request.replay_mode == "timestamp");
    assert(request.replay_speed_milli == std::numeric_limits<uint32_t>::max());
}

void TestInvalidFields() {
    ExpectInvalid({}, "capture.pcap");
    ExpectInvalid({{"name", ""}}, "capture.pcap");
    ExpectInvalid({{"name", "."}}, "capture.pcap");
    ExpectInvalid({{"name", ".."}}, "capture.pcap");
    ExpectInvalid({{"name", "dir/capture"}}, "capture.pcap");
    ExpectInvalid({{"name", "dir\\capture"}}, "capture.pcap");
    ExpectInvalid({{"name", "capture"}}, "");
    ExpectInvalid({{"name", "capture"}}, ".pcap");
    ExpectInvalid({{"name", "capture"}}, "../capture.pcap");
    ExpectInvalid({{"name", "capture"}}, "dir\\capture.pcap");
    ExpectInvalid({{"name", "capture"}}, "capture.txt");
    ExpectInvalid({{"name", "capture"}, {"unknown", "1"}}, "capture.pcap");
    ExpectInvalid({{"name", "capture"}, {"format", "raw"}}, "capture.pcap");
    ExpectInvalid({{"name", "capture"}, {"replay_mode", "slow"}}, "capture.pcap");

    for (const char* value : {"0", "+1", "-1", "4294967296", "12x", " 12"}) {
        ExpectInvalid({{"name", "capture"}, {"batch_packets", value}}, "capture.pcap");
        ExpectInvalid({{"name", "capture"}, {"replay_speed_milli", value}}, "capture.pcap");
    }

    std::string message;
    assert(ParsePcapUploadFields({{"name", "capture"}}, "capture.pcap", nullptr, &message) ==
           PcapUploadError::kInternal);
}

void TestSizeLimit() {
    uint64_t next = 0;
    assert(CheckPcapUploadSize(7, 3, 10, &next) == PcapUploadError::kOk);
    assert(next == 10);
    assert(CheckPcapUploadSize(8, 3, 10, &next) == PcapUploadError::kTooLarge);
    assert(CheckPcapUploadSize(std::numeric_limits<uint64_t>::max() - 1,
                               2,
                               std::numeric_limits<uint64_t>::max(),
                               &next) == PcapUploadError::kTooLarge);
    assert(CheckPcapUploadSize(0, 0, 10, nullptr) == PcapUploadError::kInternal);
}

void TestErrorMapping() {
    assert(MapPcapUploadError(PcapUploadError::kInvalidRequest).http_status == 400);
    assert(MapPcapUploadError(PcapUploadError::kConflict).http_status == 409);
    assert(MapPcapUploadError(PcapUploadError::kTooLarge).http_status == 413);
    assert(MapPcapUploadError(PcapUploadError::kUnavailable).http_status == 503);
    assert(MapPcapUploadError(PcapUploadError::kStorageFailure).http_status == 500);
    assert(MapPcapUploadError(PcapUploadError::kInternal).http_status == 500);
    assert(std::string(MapPcapUploadError(PcapUploadError::kInvalidRequest).error) == "invalid_request");
}

void TestInternalAndPublicJson() {
    PcapUploadRequest request;
    request.channel_name = "capture_01";
    request.original_filename = "capture.pcap";
    request.format = "pcap";
    request.batch_packets = 512;
    request.replay_mode = "timestamp";
    request.replay_speed_milli = 2000;

    ManagedCaptureRef capture;
    capture.channel_name = request.channel_name;
    capture.original_filename = request.original_filename;
    capture.canonical_path = "/srv/flowsql/uploads/pcapfile/managed-01.pcap";
    capture.size_bytes = 4096;

    const std::string internal_json = BuildSchedulerPcapAddJson(request, capture);
    rapidjson::Document internal;
    internal.Parse(internal_json.c_str());
    assert(!internal.HasParseError() && internal.IsObject());
    assert(std::string(internal["type"].GetString()) == "pcapfile");
    assert(std::string(internal["name"].GetString()) == "capture_01");
    assert(std::string(internal["role"].GetString()) == "source");
    assert(std::string(internal["options"]["path"].GetString()) == capture.canonical_path.string());
    assert(std::string(internal["options"]["format"].GetString()) == "pcap");
    assert(internal["options"]["batch_packets"].GetUint() == 512);
    assert(std::string(internal["options"]["replay_mode"].GetString()) == "timestamp");
    assert(internal["options"]["replay_speed_milli"].GetUint() == 2000);

    const std::string public_json = BuildPublicPcapUploadJson(capture, "running");
    rapidjson::Document external;
    external.Parse(public_json.c_str());
    assert(!external.HasParseError() && external.IsObject());
    assert(std::string(external["type"].GetString()) == "pcapfile");
    assert(std::string(external["name"].GetString()) == "capture_01");
    assert(std::string(external["role"].GetString()) == "source");
    assert(std::string(external["status"].GetString()) == "running");
    assert(std::string(external["filename"].GetString()) == "capture.pcap");
    assert(external["size_bytes"].GetUint64() == 4096);
    assert(!external.HasMember("path"));
    assert(!external.HasMember("option"));
    assert(!external.HasMember("options"));
    assert(!external.HasMember("option_json"));
    assert(public_json.find(capture.canonical_path.string()) == std::string::npos);
}

PcapUploadRequest MakeUploadRequest() {
    PcapUploadRequest request;
    request.channel_name = "capture_01";
    request.original_filename = "client-name.pcap";
    return request;
}

void TestManagedStoreInitialize() {
    TempDirectory temp;
    ManagedCaptureStore store;
    std::string message;
    {
        CurrentPathGuard current_path(temp.path());
        assert(store.Initialize("uploads", &message) == PcapUploadError::kOk);
    }
    assert(message.empty());
    assert(store.initialized());
    assert(store.root().is_absolute());
    assert(store.root() == fs::canonical(temp.path() / "uploads" / "pcapfile"));
    assert(fs::is_directory(store.root()));

    const fs::path invalid_base = temp.path() / "invalid";
    fs::create_directories(invalid_base);
    std::ofstream(invalid_base / "pcapfile") << "not a directory";
    ManagedCaptureStore invalid_store;
    assert(invalid_store.Initialize(invalid_base, &message) == PcapUploadError::kStorageFailure);
    assert(!message.empty());
}

void TestManagedStoreFinalizeRollback() {
    TempDirectory temp;
    ManagedCaptureStore store;
    std::string message;
    assert(store.Initialize(temp.path() / "uploads", &message) == PcapUploadError::kOk);

    fs::path final_path;
    {
        ManagedCaptureUpload upload;
        const PcapUploadRequest request = MakeUploadRequest();
        assert(store.Begin(request, &upload, &message) == PcapUploadError::kOk);
        assert(upload.active());
        assert(!upload.finalized());
        assert(upload.part_path().extension() == ".part");
        assert(upload.final_path().filename().string().find(request.original_filename) == std::string::npos);
        assert(fs::is_regular_file(upload.part_path()));
        assert(!fs::exists(upload.final_path()));
        assert(EntryCount(store.root()) == 1);

        assert(upload.Write("abc", 3, 6, &message) == PcapUploadError::kOk);
        assert(upload.Write("def", 3, 6, &message) == PcapUploadError::kOk);
        assert(upload.size_bytes() == 6);

        ManagedCaptureRef capture;
        assert(upload.Finalize(&capture, &message) == PcapUploadError::kOk);
        assert(upload.active());
        assert(upload.finalized());
        assert(!fs::exists(upload.part_path()));
        assert(fs::is_regular_file(upload.final_path()));
        assert(ReadFile(upload.final_path()) == "abcdef");
        assert(capture.channel_name == request.channel_name);
        assert(capture.original_filename == request.original_filename);
        assert(capture.canonical_path == fs::canonical(upload.final_path()));
        assert(capture.size_bytes == 6);
        assert(store.IsManagedPath(capture.canonical_path));
        final_path = capture.canonical_path;
    }
    assert(!fs::exists(final_path));
    assert(EntryCount(store.root()) == 0);
}

void TestManagedStoreCommitAndRemove() {
    TempDirectory temp;
    ManagedCaptureStore store;
    std::string message;
    assert(store.Initialize(temp.path() / "uploads", &message) == PcapUploadError::kOk);

    ManagedCaptureRef capture;
    {
        ManagedCaptureUpload upload;
        assert(store.Begin(MakeUploadRequest(), &upload, &message) == PcapUploadError::kOk);
        assert(upload.Write("packet", 6, 64, &message) == PcapUploadError::kOk);
        assert(upload.Finalize(&capture, &message) == PcapUploadError::kOk);
        assert(upload.Commit(&message) == PcapUploadError::kOk);
        assert(!upload.active());
    }
    assert(fs::is_regular_file(capture.canonical_path));
    assert(ReadFile(capture.canonical_path) == "packet");
    assert(store.RemoveManaged(capture.canonical_path, &message) == PcapUploadError::kOk);
    assert(!fs::exists(capture.canonical_path));
}

void TestManagedStoreFailureCleanup() {
    TempDirectory temp;
    ManagedCaptureStore store;
    std::string message;
    assert(store.Initialize(temp.path() / "uploads", &message) == PcapUploadError::kOk);

    ManagedCaptureUpload too_large;
    assert(store.Begin(MakeUploadRequest(), &too_large, &message) == PcapUploadError::kOk);
    assert(too_large.Write("four", 4, 3, &message) == PcapUploadError::kTooLarge);
    assert(!too_large.active());
    assert(EntryCount(store.root()) == 0);

    ManagedCaptureUpload cancelled;
    assert(store.Begin(MakeUploadRequest(), &cancelled, &message) == PcapUploadError::kOk);
    assert(cancelled.Write("partial", 7, 64, &message) == PcapUploadError::kOk);
    cancelled.Rollback();
    assert(!cancelled.active());
    assert(EntryCount(store.root()) == 0);
}

void TestManagedStoreRejectsUnmanagedDelete() {
    TempDirectory temp;
    ManagedCaptureStore store;
    std::string message;
    assert(store.Initialize(temp.path() / "uploads", &message) == PcapUploadError::kOk);

    const fs::path outside = temp.path() / "outside.pcap";
    std::ofstream(outside) << "outside";
    assert(store.RemoveManaged(outside, &message) == PcapUploadError::kInvalidRequest);
    assert(fs::is_regular_file(outside));
    assert(store.RemoveManaged("outside.pcap", &message) == PcapUploadError::kInvalidRequest);
    assert(store.RemoveManaged(store.root(), &message) == PcapUploadError::kInvalidRequest);

    const fs::path link = store.root() / "link.pcap";
    std::error_code ec;
    fs::create_symlink(outside, link, ec);
    assert(!ec);
    assert(store.RemoveManaged(link, &message) == PcapUploadError::kInvalidRequest);
    assert(fs::is_symlink(link));
    assert(fs::is_regular_file(outside));

    const fs::path escaped = store.root() / ".." / ".." / "outside.pcap";
    assert(store.RemoveManaged(escaped, &message) == PcapUploadError::kInvalidRequest);
    assert(fs::is_regular_file(outside));
}

void TestUploadTransactionSuccess() {
    TempDirectory temp;
    ManagedCaptureStore store;
    std::string message;
    assert(store.Initialize(temp.path() / "uploads", &message) == PcapUploadError::kOk);

    PcapUploadRequest request = MakeUploadRequest();
    request.format = "pcap";
    request.batch_packets = 512;
    request.replay_mode = "timestamp";
    request.replay_speed_milli = 2000;

    int scheduler_calls = 0;
    fs::path capture_path;
    std::string public_json;
    {
        PcapUploadTransaction transaction(
            &store,
            request,
            64,
            [&](const std::string& body, std::string*) {
                ++scheduler_calls;
                rapidjson::Document internal;
                internal.Parse(body.c_str());
                assert(!internal.HasParseError() && internal.IsObject());
                assert(std::string(internal["type"].GetString()) == "pcapfile");
                assert(std::string(internal["name"].GetString()) == request.channel_name);
                assert(std::string(internal["role"].GetString()) == "source");
                assert(internal.HasMember("options") && internal["options"].IsObject());
                capture_path = internal["options"]["path"].GetString();
                assert(capture_path.is_absolute());
                assert(store.IsManagedPath(capture_path));
                assert(fs::is_regular_file(capture_path));
                assert(ReadFile(capture_path) == "abcdef");
                assert(std::string(internal["options"]["format"].GetString()) == "pcap");
                assert(internal["options"]["batch_packets"].GetUint() == 512);
                assert(std::string(internal["options"]["replay_mode"].GetString()) == "timestamp");
                assert(internal["options"]["replay_speed_milli"].GetUint() == 2000);
                return PcapUploadError::kOk;
            });

        assert(transaction.Begin(&message) == PcapUploadError::kOk);
        assert(EntryCount(store.root()) == 1);
        assert(transaction.Write("abc", 3, &message) == PcapUploadError::kOk);
        assert(transaction.Write("def", 3, &message) == PcapUploadError::kOk);

        public_json = "stale";
        assert(transaction.Complete(&public_json, &message) == PcapUploadError::kOk);
        assert(message.empty());
        assert(scheduler_calls == 1);
        assert(fs::is_regular_file(capture_path));
        assert(EntryCount(store.root()) == 1);
    }
    assert(fs::is_regular_file(capture_path));
    assert(EntryCount(store.root()) == 1);

    rapidjson::Document external;
    external.Parse(public_json.c_str());
    assert(!external.HasParseError() && external.IsObject());
    assert(std::string(external["type"].GetString()) == "pcapfile");
    assert(std::string(external["name"].GetString()) == request.channel_name);
    assert(std::string(external["status"].GetString()) == "running");
    assert(external["size_bytes"].GetUint64() == 6);
    assert(!external.HasMember("path"));
    assert(!external.HasMember("option"));
    assert(!external.HasMember("options"));
    assert(!external.HasMember("option_json"));
    assert(public_json.find(capture_path.string()) == std::string::npos);

    assert(store.RemoveManaged(capture_path, &message) == PcapUploadError::kOk);
}

void TestUploadTransactionSchedulerRollback() {
    TempDirectory temp;
    ManagedCaptureStore store;
    std::string message;
    assert(store.Initialize(temp.path() / "uploads", &message) == PcapUploadError::kOk);

    const PcapUploadError scheduler_errors[] = {
        PcapUploadError::kConflict,
        PcapUploadError::kUnavailable,
        PcapUploadError::kInvalidRequest,
        PcapUploadError::kInternal,
    };
    for (PcapUploadError scheduler_error : scheduler_errors) {
        int scheduler_calls = 0;
        PcapUploadTransaction transaction(
            &store,
            MakeUploadRequest(),
            64,
            [&](const std::string&, std::string* callback_message) {
                ++scheduler_calls;
                if (callback_message) *callback_message = "scheduler rejected capture";
                return scheduler_error;
            });
        assert(transaction.Begin(&message) == PcapUploadError::kOk);
        assert(transaction.Write("packet", 6, &message) == PcapUploadError::kOk);

        std::string public_json = "stale";
        assert(transaction.Complete(&public_json, &message) == scheduler_error);
        assert(message == "scheduler rejected capture");
        assert(public_json.empty());
        assert(scheduler_calls == 1);
        assert(EntryCount(store.root()) == 0);
    }
}

void TestUploadTransactionLocalFailureAndCancellation() {
    TempDirectory temp;
    ManagedCaptureStore store;
    std::string message;
    assert(store.Initialize(temp.path() / "uploads", &message) == PcapUploadError::kOk);

    int scheduler_calls = 0;
    auto scheduler_add = [&](const std::string&, std::string*) {
        ++scheduler_calls;
        return PcapUploadError::kOk;
    };

    PcapUploadTransaction too_large(&store, MakeUploadRequest(), 3, scheduler_add);
    assert(too_large.Begin(&message) == PcapUploadError::kOk);
    assert(too_large.Write("four", 4, &message) == PcapUploadError::kTooLarge);
    assert(scheduler_calls == 0);
    assert(EntryCount(store.root()) == 0);

    PcapUploadTransaction finalize_failure(&store, MakeUploadRequest(), 64, scheduler_add);
    assert(finalize_failure.Begin(&message) == PcapUploadError::kOk);
    assert(finalize_failure.Write("packet", 6, &message) == PcapUploadError::kOk);
    const fs::path part_path = OnlyEntryPath(store.root());
    assert(part_path.extension() == ".part");
    assert(fs::remove(part_path));
    std::string public_json = "stale";
    assert(finalize_failure.Complete(&public_json, &message) == PcapUploadError::kStorageFailure);
    assert(public_json.empty());
    assert(scheduler_calls == 0);
    assert(EntryCount(store.root()) == 0);

    PcapUploadTransaction cancelled(&store, MakeUploadRequest(), 64, scheduler_add);
    assert(cancelled.Begin(&message) == PcapUploadError::kOk);
    assert(cancelled.Write("partial", 7, &message) == PcapUploadError::kOk);
    cancelled.Rollback();
    assert(scheduler_calls == 0);
    assert(EntryCount(store.root()) == 0);

    {
        PcapUploadTransaction abandoned(&store, MakeUploadRequest(), 64, scheduler_add);
        assert(abandoned.Begin(&message) == PcapUploadError::kOk);
        assert(abandoned.Write("partial", 7, &message) == PcapUploadError::kOk);
    }
    assert(scheduler_calls == 0);
    assert(EntryCount(store.root()) == 0);
}

void TestWebPcapMultipartUpload() {
    WebPlugin invalid_zero;
    assert(invalid_zero.Option("pcap_upload_max_bytes=0") != 0);
    WebPlugin invalid_text;
    assert(invalid_text.Option("pcap_upload_max_bytes=8x") != 0);

    std::atomic<int> scheduler_calls{0};
    std::atomic<int> query_calls{0};
    std::atomic<int> remove_calls{0};
    std::mutex scheduler_mutex;
    std::string scheduler_request;
    fs::path expected_remove_path;
    std::vector<std::string> delete_events;
    httplib::Server scheduler;
    scheduler.Post("/channels/stream/add", [&](const httplib::Request& req, httplib::Response& res) {
        ++scheduler_calls;
        rapidjson::Document request;
        request.Parse(req.body.c_str());
        assert(!request.HasParseError() && request.IsObject());
        assert(request.HasMember("name") && request["name"].IsString());
        const std::string name = request["name"].GetString();
        if (name == "conflict") {
            res.status = 409;
            res.set_content(R"({"error":"duplicate"})", "application/json");
            return;
        }
        if (name == "unavailable") {
            res.status = 503;
            res.set_content(R"({"error":"provider unavailable"})", "application/json");
            return;
        }
        {
            std::lock_guard<std::mutex> lock(scheduler_mutex);
            scheduler_request = req.body;
        }
        res.status = 200;
        res.set_content(R"({"ok":true})", "application/json");
    });
    scheduler.Post("/channels/stream/query", [&](const httplib::Request&, httplib::Response& res) {
        ++query_calls;
        std::string add_json;
        {
            std::lock_guard<std::mutex> lock(scheduler_mutex);
            delete_events.push_back("query");
            add_json = scheduler_request;
        }
        rapidjson::Document add_request;
        add_request.Parse(add_json.c_str());
        assert(!add_request.HasParseError() && add_request.IsObject());
        const std::string name = add_request["name"].GetString();
        const std::string path = add_request["options"]["path"].GetString();
        res.status = 200;
        res.set_content(BuildStreamQueryResponse({{"pcapfile", name, QueryPathKind::kString, path}}),
                        "application/json");
    });
    scheduler.Post("/channels/stream/remove", [&](const httplib::Request&, httplib::Response& res) {
        ++remove_calls;
        {
            std::lock_guard<std::mutex> lock(scheduler_mutex);
            delete_events.push_back("remove");
            assert(!expected_remove_path.empty());
            assert(fs::is_regular_file(expected_remove_path));
        }
        res.status = 200;
        res.set_content(R"({"ok":true})", "application/json");
    });
    const int scheduler_port = scheduler.bind_to_any_port("127.0.0.1");
    assert(scheduler_port > 0);
    std::thread scheduler_thread([&]() { assert(scheduler.listen_after_bind()); });
    scheduler.wait_until_ready();

    TempDirectory temp;
    const int web_port = ReserveLoopbackPort();
    WebPlugin web;
    const std::string options =
        "host=127.0.0.1;port=" + std::to_string(web_port) + ";db_path=:memory:;upload_dir=" +
        (temp.path() / "uploads").string() + ";gateway=127.0.0.1:" + std::to_string(scheduler_port) +
        ";pcap_upload_max_bytes=8";
    assert(web.Option(options.c_str()) == 0);

    bool enumerated_upload_route = false;
    web.EnumRoutes([&](const flowsql::RouteItem& route) {
        if (route.uri == "/api/channels/pcapfile/upload") enumerated_upload_route = true;
    });
    assert(!enumerated_upload_route);
    assert(web.Start() == 0);
    WaitForHttpServer(web_port);

    httplib::Client client("127.0.0.1", web_port);
    const httplib::MultipartFormDataItems valid_fields = {
        {"name", "capture_01", "", ""},
        {"format", "pcap", "", ""},
        {"batch_packets", "512", "", ""},
        {"replay_mode", "timestamp", "", ""},
        {"replay_speed_milli", "2000", "", ""},
    };
    const auto success = PostCapture(&client, valid_fields, "client.pcap", "abcdef");
    assert(success);
    assert(success->status == 200);
    rapidjson::Document public_response;
    public_response.Parse(success->body.c_str());
    assert(!public_response.HasParseError() && public_response.IsObject());
    assert(std::string(public_response["name"].GetString()) == "capture_01");
    assert(std::string(public_response["status"].GetString()) == "running");
    assert(public_response["size_bytes"].GetUint64() == 6);
    assert(!public_response.HasMember("path"));
    assert(!public_response.HasMember("option"));
    assert(!public_response.HasMember("options"));
    assert(!public_response.HasMember("option_json"));

    std::string add_json;
    {
        std::lock_guard<std::mutex> lock(scheduler_mutex);
        add_json = scheduler_request;
    }
    rapidjson::Document add_request;
    add_request.Parse(add_json.c_str());
    assert(!add_request.HasParseError() && add_request.IsObject());
    assert(std::string(add_request["type"].GetString()) == "pcapfile");
    assert(std::string(add_request["role"].GetString()) == "source");
    const fs::path managed_path = add_request["options"]["path"].GetString();
    const fs::path managed_root = fs::canonical(temp.path() / "uploads" / "pcapfile");
    assert(managed_path.is_absolute());
    assert(managed_path.parent_path() == managed_root);
    assert(ReadFile(managed_path) == "abcdef");
    assert(success->body.find(managed_path.string()) == std::string::npos);
    assert(EntryCount(managed_root) == 1);

    const int calls_after_success = scheduler_calls.load();
    ExpectHttpError(PostCapture(&client, valid_fields, "large.pcap", "123456789"),
                    413,
                    "file_too_large");
    assert(scheduler_calls.load() == calls_after_success);
    assert(EntryCount(managed_root) == 1);

    auto conflict_fields = valid_fields;
    conflict_fields[0].content = "conflict";
    ExpectHttpError(PostCapture(&client, conflict_fields, "conflict.pcap", "packet"),
                    409,
                    "channel_conflict");
    assert(EntryCount(managed_root) == 1);

    auto unavailable_fields = valid_fields;
    unavailable_fields[0].content = "unavailable";
    ExpectHttpError(PostCapture(&client, unavailable_fields, "unavailable.pcap", "packet"),
                    503,
                    "provider_unavailable");
    assert(EntryCount(managed_root) == 1);

    const httplib::MultipartFormDataItems missing_name = {
        {"format", "pcap", "", ""},
    };
    ExpectHttpError(PostCapture(&client, missing_name, "missing.pcap", "packet"),
                    400,
                    "invalid_request");
    const httplib::MultipartFormDataItems duplicate_name = {
        {"name", "one", "", ""},
        {"name", "two", "", ""},
    };
    ExpectHttpError(PostCapture(&client, duplicate_name, "duplicate.pcap", "packet"),
                    400,
                    "invalid_request");
    auto invalid_format = valid_fields;
    invalid_format[1].content = "invalid";
    ExpectHttpError(PostCapture(&client, invalid_format, "invalid.pcap", "packet"),
                    400,
                    "invalid_request");
    auto unknown_field = valid_fields;
    unknown_field.push_back({"unknown", "value", "", ""});
    ExpectHttpError(PostCapture(&client, unknown_field, "unknown.pcap", "packet"),
                    400,
                    "invalid_request");
    const httplib::MultipartFormDataItems missing_file = {
        {"name", "missing_file", "", ""},
    };
    ExpectHttpError(client.Post("/api/channels/pcapfile/upload", missing_file),
                    400,
                    "invalid_request");
    const httplib::MultipartFormDataItems duplicate_file = {
        {"name", "duplicate_file", "", ""},
        {"file", "one", "one.pcap", "application/vnd.tcpdump.pcap"},
        {"file", "two", "two.pcap", "application/vnd.tcpdump.pcap"},
    };
    ExpectHttpError(client.Post("/api/channels/pcapfile/upload", duplicate_file),
                    400,
                    "invalid_request");
    ExpectHttpError(client.Post("/api/channels/pcapfile/upload", "capture", "application/octet-stream"),
                    400,
                    "invalid_request");
    assert(EntryCount(managed_root) == 1);

    {
        std::lock_guard<std::mutex> lock(scheduler_mutex);
        expected_remove_path = managed_path;
        delete_events.clear();
    }
    const auto removed = PostRemove(&client, "pcapfile", "capture_01");
    assert(removed);
    assert(removed->status == 200);
    assert(query_calls.load() == 1);
    assert(remove_calls.load() == 1);
    {
        std::lock_guard<std::mutex> lock(scheduler_mutex);
        assert((delete_events == std::vector<std::string>{"query", "remove"}));
    }
    assert(!fs::exists(managed_path));
    assert(EntryCount(managed_root) == 0);

    scheduler.stop();
    scheduler_thread.join();
    const int calls_before_transport_failure = scheduler_calls.load();
    auto transport_failure_fields = valid_fields;
    transport_failure_fields[0].content = "transport_failure";
    ExpectHttpError(PostCapture(&client, transport_failure_fields, "transport.pcap", "packet"),
                    503,
                    "provider_unavailable");
    assert(scheduler_calls.load() == calls_before_transport_failure);
    assert(EntryCount(managed_root) == 0);

    assert(web.Stop() == 0);
}

struct DeleteSchedulerState {
    std::mutex mutex;
    int query_status = 200;
    int remove_status = 200;
    std::string query_body = R"({"channels":[]})";
    int query_calls = 0;
    int remove_calls = 0;
    fs::path expected_file_during_remove;
    bool saw_file_during_remove = false;
    bool replace_file_with_directory = false;
    std::vector<std::string> events;
};

struct DeleteSchedulerSnapshot {
    int query_calls = 0;
    int remove_calls = 0;
    bool saw_file_during_remove = false;
    std::vector<std::string> events;
};

void ConfigureDeleteScheduler(DeleteSchedulerState* state,
                              int query_status,
                              const std::string& query_body,
                              int remove_status,
                              const fs::path& expected_file = {},
                              bool replace_file_with_directory = false) {
    assert(state != nullptr);
    std::lock_guard<std::mutex> lock(state->mutex);
    state->query_status = query_status;
    state->query_body = query_body;
    state->remove_status = remove_status;
    state->expected_file_during_remove = expected_file;
    state->saw_file_during_remove = false;
    state->replace_file_with_directory = replace_file_with_directory;
    state->events.clear();
}

DeleteSchedulerSnapshot SnapshotDeleteScheduler(DeleteSchedulerState* state) {
    assert(state != nullptr);
    std::lock_guard<std::mutex> lock(state->mutex);
    return {state->query_calls, state->remove_calls, state->saw_file_during_remove, state->events};
}

void TestWebPcapManagedDeleteFailures() {
    DeleteSchedulerState state;
    httplib::Server scheduler;
    scheduler.Post("/channels/stream/query", [&](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(state.mutex);
        ++state.query_calls;
        state.events.push_back("query");
        res.status = state.query_status;
        res.set_content(state.query_body, "application/json");
    });
    scheduler.Post("/channels/stream/remove", [&](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(state.mutex);
        ++state.remove_calls;
        state.events.push_back("remove");
        if (!state.expected_file_during_remove.empty()) {
            state.saw_file_during_remove = fs::is_regular_file(state.expected_file_during_remove);
            if (state.replace_file_with_directory) {
                std::error_code error;
                assert(fs::remove(state.expected_file_during_remove, error));
                assert(!error);
                assert(fs::create_directory(state.expected_file_during_remove, error));
                assert(!error);
                std::ofstream(state.expected_file_during_remove / "keep") << "not empty";
            }
        }
        res.status = state.remove_status;
        res.set_content(state.remove_status == 200 ? R"({"ok":true})" : R"({"error":"scheduler"})",
                        "application/json");
    });
    const int scheduler_port = scheduler.bind_to_any_port("127.0.0.1");
    assert(scheduler_port > 0);
    std::thread scheduler_thread([&]() { assert(scheduler.listen_after_bind()); });
    scheduler.wait_until_ready();

    TempDirectory temp;
    const int web_port = ReserveLoopbackPort();
    WebPlugin web;
    const std::string options =
        "host=127.0.0.1;port=" + std::to_string(web_port) + ";db_path=:memory:;upload_dir=" +
        (temp.path() / "uploads").string() + ";gateway=127.0.0.1:" + std::to_string(scheduler_port);
    assert(web.Option(options.c_str()) == 0);
    assert(web.Start() == 0);
    WaitForHttpServer(web_port);

    httplib::Client client("127.0.0.1", web_port);
    const fs::path managed_root = fs::canonical(temp.path() / "uploads" / "pcapfile");
    const auto initial = SnapshotDeleteScheduler(&state);
    for (const char* invalid_request : {
             "",
             "not-json",
             "[]",
             "{}",
             R"({"type":"","name":"capture"})",
             R"({"type":"pcapfile","name":""})",
             R"({"type":7,"name":"capture"})",
             R"({"type":"pcapfile","name":7})",
         }) {
        const auto result = client.Post("/api/channels/stream/remove", invalid_request, "application/json");
        assert(result);
        assert(result->status == 400);
    }
    auto snapshot = SnapshotDeleteScheduler(&state);
    assert(snapshot.query_calls == initial.query_calls);
    assert(snapshot.remove_calls == initial.remove_calls);

    const fs::path ordinary_file = temp.path() / "ordinary.pcap";
    std::ofstream(ordinary_file) << "ordinary";
    ConfigureDeleteScheduler(&state, 500, "malformed", 200);
    const int ordinary_queries = snapshot.query_calls;
    const int ordinary_removes = snapshot.remove_calls;
    const auto ordinary = PostRemove(&client, "memory", "ordinary");
    assert(ordinary && ordinary->status == 200);
    snapshot = SnapshotDeleteScheduler(&state);
    assert(snapshot.query_calls == ordinary_queries);
    assert(snapshot.remove_calls == ordinary_removes + 1);
    assert((snapshot.events == std::vector<std::string>{"remove"}));
    assert(fs::is_regular_file(ordinary_file));

    const fs::path retained_file = managed_root / "retained.pcap";
    std::ofstream(retained_file) << "retained";
    const fs::path outside_file = temp.path() / "outside.pcap";
    std::ofstream(outside_file) << "outside";
    const std::string retained_query = BuildStreamQueryResponse({
        {"pcapfile", "other", QueryPathKind::kString, outside_file.string()},
        {"memory", "retained", QueryPathKind::kString, outside_file.string()},
        {"pcapfile", "retained", QueryPathKind::kString, retained_file.string()},
    });
    for (const int query_status : {400, 404, 409, 503, 500}) {
        ConfigureDeleteScheduler(&state, query_status, R"({"error":"query failed"})", 200, retained_file);
        const int query_failure_removes = SnapshotDeleteScheduler(&state).remove_calls;
        const auto query_failure = PostRemove(&client, "pcapfile", "retained");
        assert(query_failure && query_failure->status == query_status);
        snapshot = SnapshotDeleteScheduler(&state);
        assert(snapshot.remove_calls == query_failure_removes);
        assert((snapshot.events == std::vector<std::string>{"query"}));
        assert(fs::is_regular_file(retained_file));
    }

    for (const int remove_status : {409, 404, 503, 500}) {
        ConfigureDeleteScheduler(&state, 200, retained_query, remove_status, retained_file);
        const auto remove_failure = PostRemove(&client, "pcapfile", "retained");
        assert(remove_failure && remove_failure->status == remove_status);
        snapshot = SnapshotDeleteScheduler(&state);
        assert(snapshot.saw_file_during_remove);
        assert((snapshot.events == std::vector<std::string>{"query", "remove"}));
        assert(fs::is_regular_file(retained_file));
    }

    const std::vector<std::string> invalid_query_responses = {
        "not-json",
        R"({"channels":{}})",
        R"({"channels":[{"type":"pcapfile","name":"retained","option_json":7}]})",
        BuildStreamQueryResponse({}),
        BuildStreamQueryResponse({
            {"pcapfile", "retained", QueryPathKind::kString, retained_file.string()},
            {"pcapfile", "retained", QueryPathKind::kString, retained_file.string()},
        }),
        BuildStreamQueryResponse({{"pcapfile", "retained", QueryPathKind::kNumber, ""}}),
    };
    for (const auto& query_body : invalid_query_responses) {
        ConfigureDeleteScheduler(&state, 200, query_body, 200, retained_file);
        const int removes_before = SnapshotDeleteScheduler(&state).remove_calls;
        const auto invalid_query = PostRemove(&client, "pcapfile", "retained");
        assert(invalid_query && invalid_query->status == 500);
        snapshot = SnapshotDeleteScheduler(&state);
        assert(snapshot.remove_calls == removes_before);
        assert((snapshot.events == std::vector<std::string>{"query"}));
        assert(fs::is_regular_file(retained_file));
    }

    const fs::path symlink_path = managed_root / "outside-link.pcap";
    std::error_code error;
    fs::create_symlink(outside_file, symlink_path, error);
    assert(!error);
    const std::vector<QueryChannel> unmanaged_channels = {
        {"pcapfile", "outside", QueryPathKind::kString, outside_file.string()},
        {"pcapfile", "relative", QueryPathKind::kString, "relative.pcap"},
        {"pcapfile", "symlink", QueryPathKind::kString, symlink_path.string()},
        {"pcapfile", "missing-path", QueryPathKind::kMissing, ""},
    };
    for (const auto& channel : unmanaged_channels) {
        ConfigureDeleteScheduler(&state, 200, BuildStreamQueryResponse({channel}), 200);
        const auto removed = PostRemove(&client, "pcapfile", channel.name);
        assert(removed && removed->status == 200);
        snapshot = SnapshotDeleteScheduler(&state);
        assert((snapshot.events == std::vector<std::string>{"query", "remove"}));
    }
    assert(fs::is_regular_file(outside_file));
    assert(fs::is_symlink(symlink_path));

    const fs::path race_file = managed_root / "race.pcap";
    std::ofstream(race_file) << "race";
    ConfigureDeleteScheduler(
        &state,
        200,
        BuildStreamQueryResponse({{"pcapfile", "race", QueryPathKind::kString, race_file.string()}}),
        200,
        race_file,
        true);
    const auto storage_failure = PostRemove(&client, "pcapfile", "race");
    ExpectHttpError(storage_failure, 500, "storage_failure");
    snapshot = SnapshotDeleteScheduler(&state);
    assert(snapshot.saw_file_during_remove);
    assert((snapshot.events == std::vector<std::string>{"query", "remove"}));
    assert(fs::is_directory(race_file));

    assert(web.Stop() == 0);
    scheduler.stop();
    scheduler_thread.join();
}

void TestWebPcapStreamQueryRedaction() {
    std::mutex scheduler_mutex;
    int scheduler_status = 200;
    std::string scheduler_body;
    httplib::Server scheduler;
    scheduler.Post("/channels/stream/query", [&](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(scheduler_mutex);
        res.status = scheduler_status;
        res.set_content(scheduler_body, "application/json");
    });
    const int scheduler_port = scheduler.bind_to_any_port("127.0.0.1");
    assert(scheduler_port > 0);
    std::thread scheduler_thread([&]() { assert(scheduler.listen_after_bind()); });
    scheduler.wait_until_ready();

    TempDirectory temp;
    const std::string pcap_path = (temp.path() / "uploads" / "pcapfile" / "secret.pcap").string();
    const std::string ordinary_path = (temp.path() / "ordinary-buffer").string();
    {
        std::lock_guard<std::mutex> lock(scheduler_mutex);
        scheduler_body = BuildStreamRedactionFixture(pcap_path, ordinary_path);
    }

    const int web_port = ReserveLoopbackPort();
    WebPlugin web;
    const std::string options =
        "host=127.0.0.1;port=" + std::to_string(web_port) + ";db_path=:memory:;upload_dir=" +
        (temp.path() / "uploads").string() + ";gateway=127.0.0.1:" + std::to_string(scheduler_port);
    assert(web.Option(options.c_str()) == 0);
    assert(web.Start() == 0);
    WaitForHttpServer(web_port);

    flowsql::fnRouterHandler routed_query;
    web.EnumRoutes([&](const flowsql::RouteItem& route) {
        if (route.method == "POST" && route.uri == "/api/channels/stream/query") {
            routed_query = route.handler;
        }
    });
    assert(static_cast<bool>(routed_query));

    httplib::Client client("127.0.0.1", web_port);
    const auto http_query = client.Post("/api/channels/stream/query", "{}", "application/json");
    assert(http_query && http_query->status == 200);
    AssertStreamQueryRedacted(http_query->body, pcap_path, ordinary_path);

    std::string routed_response;
    assert(routed_query("/api/channels/stream/query", "{}", routed_response) == flowsql::error::OK);
    AssertStreamQueryRedacted(routed_response, pcap_path, ordinary_path);

    {
        std::lock_guard<std::mutex> lock(scheduler_mutex);
        scheduler_body = "{\"channels\":[{\"name\":\"missing-type\",\"path\":\"" + pcap_path + "\"}]}";
    }
    const auto malformed = client.Post("/api/channels/stream/query", "{}", "application/json");
    ExpectHttpError(malformed, 500, "invalid_scheduler_response");
    assert(malformed->body.find(pcap_path) == std::string::npos);

    routed_response = "stale";
    assert(routed_query("/api/channels/stream/query", "{}", routed_response) ==
           flowsql::error::INTERNAL_ERROR);
    assert(routed_response == R"({"error":"invalid_scheduler_response"})");
    assert(routed_response.find(pcap_path) == std::string::npos);

    {
        std::lock_guard<std::mutex> lock(scheduler_mutex);
        scheduler_body = "not-json";
    }
    ExpectHttpError(client.Post("/api/channels/stream/query", "{}", "application/json"),
                    500,
                    "invalid_scheduler_response");

    {
        std::lock_guard<std::mutex> lock(scheduler_mutex);
        scheduler_status = 503;
        scheduler_body = R"({"error":"scheduler unavailable"})";
    }
    const auto unavailable = client.Post("/api/channels/stream/query", "{}", "application/json");
    assert(unavailable && unavailable->status == 503);
    assert(unavailable->body == R"({"error":"scheduler unavailable"})");

    assert(web.Stop() == 0);
    scheduler.stop();
    scheduler_thread.join();
}

}  // namespace

int main() {
    TestDefaultsAndBoundaries();
    TestInvalidFields();
    TestSizeLimit();
    TestErrorMapping();
    TestInternalAndPublicJson();
    TestManagedStoreInitialize();
    TestManagedStoreFinalizeRollback();
    TestManagedStoreCommitAndRemove();
    TestManagedStoreFailureCleanup();
    TestManagedStoreRejectsUnmanagedDelete();
    TestUploadTransactionSuccess();
    TestUploadTransactionSchedulerRollback();
    TestUploadTransactionLocalFailureAndCancellation();
    TestWebPcapMultipartUpload();
    TestWebPcapManagedDeleteFailures();
    TestWebPcapStreamQueryRedaction();
    std::puts("[PASS] pcap upload contract");
    return 0;
}
