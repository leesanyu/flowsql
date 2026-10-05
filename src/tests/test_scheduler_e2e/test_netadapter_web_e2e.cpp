// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <common/error_code.h>
#include <framework/interfaces/iblock_stream_channel_descriptor.h>
#include <framework/interfaces/iblock_stream_factory.h>
#include <framework/interfaces/iblock_stream_reader.h>
#include <framework/interfaces/icapture_backend.h>
#include <framework/interfaces/irouter_handle.h>
#include <httplib.h>
#include <plugins/npi/iprotocol.h>
#include <rapidjson/document.h>
#include <unistd.h>
#include <common/loader.hpp>

#include <cassert>
#include <filesystem>
#include <thread>

using namespace flowsql;

namespace {
class TestBlockSource final : public IBlockStreamChannel {
 public:
    explicit TestBlockSource(const char* category) : category_(category) {}
    const char* Category() override { return category_; }
    const char* Name() override { return "input"; }
    const char* Type() override { return ChannelType::kBlockStream; }
    const char* Schema() override { return ""; }
    int Open() override { return 0; }
    int Close() override { return 0; }
    bool IsOpened() const override { return false; }
    int Flush() override { return 0; }
    BlockPollEvent PollBlock(int) override {
        assert(false);
        return {};
    }
    int ReleaseBlock(const std::shared_ptr<arrow::RecordBatch>&) override {
        assert(false);
        return 0;
    }
    void Cancel() override {}
    bool IsFinished() const override { return false; }

 private:
    const char* category_;
};

class TestBlockSources final : public IBlockStreamFactory, public IBlockStreamChannelDescriptorV1 {
 public:
    IBlockStreamChannel* Get(const char* type, const char* name) override {
        if (std::string(name) != "input") return nullptr;
        for (auto* source : {&finite_, &live_, &legacy_})
            if (std::string(type) == source->Category()) return source;
        return nullptr;
    }
    void List(std::function<void(const char*, const char*, IBlockStreamChannel*)> callback) override {
        for (auto* source : {&finite_, &live_, &legacy_}) callback(source->Category(), source->Name(), source);
    }
    void DescribeChannelTypes(std::function<void(const BlockStreamChannelTypeDescriptorV1&)> callback) const override {
        for (const auto* category : {"finite_capture", "custom_live"}) {
            BlockStreamChannelTypeDescriptorV1 descriptor;
            descriptor.channel_type = category;
            descriptor.is_finite = std::string(category) == "finite_capture";
            callback(descriptor);
        }
    }

 private:
    TestBlockSource finite_{"finite_capture"}, live_{"custom_live"}, legacy_{"legacy_capture"};
};

class Protocol final : public IProtocol {
 public:
    void Concurrency(int32_t) override {}
    protocol::Protocol Identify(int32_t, const uint8_t*, int32_t, const protocol::Layers*) override { return {}; }
    int32_t Layer(int32_t, const uint8_t*, int32_t, protocol::Layers* out) override {
        *out = {};
        return 0;
    }
    protocol::IDictionary* Dictionary() override { return nullptr; }
};

class Session final : public ICaptureBackendSessionV1 {
 public:
    explicit Session(const CaptureBackendConfigV1& config) {
        for (const auto& name : config.interfaces) {
            CaptureBackendInputV1 input;
            input.interface_name = name;
            input.buffer_bytes = 1024 * 1024;
            inputs_.push_back(input);
        }
    }
    const std::vector<CaptureBackendInputV1>& Inputs() const override { return inputs_; }
    int TryRead(uint32_t, CapturePacketViewV1*) override { return EAGAIN; }
    void ReleasePacket(uint32_t) override { assert(false); }
    CaptureBacklogV1 Backlog(uint32_t) const override { return CaptureBacklogV1::kEmpty; }
    int64_t IdleTimeNs(uint32_t) const override { return 0; }
    int ReadCounters(uint32_t, CaptureCountersV1* out) override {
        *out = {};
        return 0;
    }
    void Cancel() override {}

 private:
    std::vector<CaptureBackendInputV1> inputs_;
};

class Provider final : public ICaptureBackendProviderV1 {
 public:
    explicit Provider(const char* name) : name_(name) {}
    const char* Backend() const override { return name_; }
    int Open(const CaptureBackendConfigV1& config, std::shared_ptr<ICaptureBackendSessionV1>* out,
             std::string*) override {
        *out = std::make_shared<Session>(config);
        return 0;
    }

 private:
    const char* name_;
};

fnRouterHandler Route(PluginLoader* loader, const std::string& uri) {
    fnRouterHandler result;
    loader->Traverse(IID_ROUTER_HANDLE, [&](void* value) {
        static_cast<IRouterHandle*>(value)->EnumRoutes([&](const RouteItem& item) {
            if (item.method == "POST" && item.uri == uri) result = item.handler;
        });
        return result ? -1 : 0;
    });
    assert(result);
    return result;
}

// All plugin callbacks and products are destroyed before the loader unloads its libraries.
class Runtime {
 public:
    explicit Runtime(const std::string& db) : loader_(PluginLoader::Single()) {
        loader_->Regist(IID_PROTOCOL, &protocol_);
        loader_->Regist(IID_CAPTURE_BACKEND_PROVIDER_V1, &af_packet_);
        loader_->Regist(IID_CAPTURE_BACKEND_PROVIDER_V1, &pfring_);
        loader_->Regist(IID_BLOCK_STREAM_FACTORY, static_cast<IBlockStreamFactory*>(&block_sources_));
        loader_->Regist(IID_BLOCK_STREAM_CHANNEL_DESCRIPTOR_V1,
                        static_cast<IBlockStreamChannelDescriptorV1*>(&block_sources_));
        const int port = server_->bind_to_any_port("127.0.0.1");
        assert(port > 0);
        const char* libraries[] = {"libflowsql_netadapter.so", "libflowsql_builtin.so", "libflowsql_scheduler.so",
                                   "libflowsql_web.so"};
        const std::string netadapter = "{\"db_path\":\"" + db + "\"}";
        const std::string web = "db_path=:memory:;gateway=127.0.0.1:" + std::to_string(port);
        const char* options[] = {netadapter.c_str(), nullptr, nullptr, web.c_str()};
        assert(loader_->Load(get_absolute_process_path(), libraries, options, 4) == 0);
        for (const auto* operation : {"definitions/query", "query", "add", "modify", "remove"}) {
            const std::string uri = std::string("/channels/stream/") + operation;
            const auto handler = Route(loader_, uri);
            server_->Post(uri, [uri, handler](const httplib::Request& req, httplib::Response& rsp) {
                std::string body;
                const auto rc = handler(uri, req.body, body);
                rsp.status = rc == error::OK ? 200 : rc == error::CONFLICT ? 409 : rc == error::UNAVAILABLE ? 503 : 400;
                rsp.set_content(body, "application/json");
            });
        }
        thread_ = std::thread([this] { assert(server_->listen_after_bind()); });
        server_->wait_until_ready();
        assert(loader_->StartAll() == 0);
    }
    ~Runtime() {
        assert(reader_ == nullptr);
        loader_->StopAll();
        server_->stop();
        thread_.join();
        server_.reset();
        assert(loader_->Unload() == 0);
    }
    rapidjson::Document Request(const std::string& operation, const std::string& body, int32_t expected = error::OK) {
        std::string response;
        const std::string uri = "/api/channels/stream/" + operation;
        const auto handler = Route(loader_, uri);
        assert(handler(uri, body, response) == expected);
        rapidjson::Document doc;
        doc.Parse(response.c_str());
        assert(!doc.HasParseError());
        return doc;
    }
    void Acquire() {
        auto* factory = static_cast<IBlockStreamReaderFactoryV1*>(loader_->First(IID_BLOCK_STREAM_READER_FACTORY_V1));
        assert(factory);
        BlockStreamReaderConfigV1 config;
        config.task_id = "web-test";
        config.source_category = "netadapter";
        config.source_name = "edge";
        config.pushed_filter_plan_json = R"({"version":1,"root":null})";
        assert(factory->CreateReader(config, &reader_) == 0 && reader_);
    }
    void CheckExecutionPolicy(const char* source, bool requires_async) {
        const auto handler = Route(loader_, "/scheduler/sql/classify");
        std::string response;
        const std::string request = std::string("{\"sql\":\"SELECT * FROM ") + source + "\"}";
        assert(handler("/scheduler/sql/classify", request, response) == error::OK);
        rapidjson::Document doc;
        doc.Parse(response.c_str());
        assert(std::string(doc["task_kind"].GetString()) == "batch");
        assert(doc.HasMember("requires_async") && doc["requires_async"].IsBool());
        assert(doc["requires_async"].GetBool() == requires_async);
    }
    void Release() {
        auto* factory = static_cast<IBlockStreamReaderFactoryV1*>(loader_->First(IID_BLOCK_STREAM_READER_FACTORY_V1));
        factory->ReleaseReader(reader_);
        reader_ = nullptr;
    }

 private:
    Protocol protocol_;
    TestBlockSources block_sources_;
    Provider af_packet_{"af_packet"}, pfring_{"pfring_classic"};
    PluginLoader* loader_;
    std::unique_ptr<httplib::Server> server_ = std::make_unique<httplib::Server>();
    std::thread thread_;
    IBlockStreamChannel* reader_ = nullptr;
};
}  // namespace

int main() {
    const std::string db = "/tmp/flowsql-netadapter-web-" + std::to_string(getpid()) + ".db";
    const std::string add =
        R"({"type":"netadapter","name":"edge","role":"source","options":{"backend":"af_packet","interfaces":["test1","test2"]}})";
    const std::string modify =
        R"({"type":"netadapter","name":"edge","role":"source","options":{"backend":"pfring_classic","interfaces":["test2"],"snaplen":128,"buffer_mib":16,"promiscuous":false}})";
    const std::string remove = R"({"type":"netadapter","name":"edge"})";
    uint64_t domain;
    {
        Runtime runtime(db);
        const auto definitions = runtime.Request("definitions/query", "{}");
        bool found = false;
        for (const auto& def : definitions["definitions"].GetArray()) {
            if (std::string(def["channel_type"].GetString()) != "netadapter") continue;
            found = true;
            assert(def["allowed_roles"].Size() == 1 && std::string(def["allowed_roles"][0].GetString()) == "source");
            assert(def["option_schema"].Size() == 5 && !def["is_finite"].GetBool() && !def["supports_reset"].GetBool());
            assert(def["option_schema"][0]["enum_values"].Size() == 3);
        }
        assert(found);
        runtime.Request("add", add);
        runtime.CheckExecutionPolicy("netadapter.edge", true);
        runtime.CheckExecutionPolicy("custom_live.input", true);
        runtime.CheckExecutionPolicy("finite_capture.input", false);
        runtime.CheckExecutionPolicy("legacy_capture.input", false);
        runtime.Request("add", add, error::CONFLICT);
        auto query = runtime.Request("query", "{}");
        assert(query["channels"].Size() == 1);
        const auto& channel = query["channels"][0];
        assert(!channel["is_finite"].GetBool() && !channel["is_finished"].GetBool());
        assert(!channel["supports_reset"].GetBool() && !channel["in_use"].GetBool());
        const auto& options = channel["option_json"];
        domain = options["observation_domain_id"].GetUint64();
        assert(options["snaplen"].GetUint() == 65535 && options["buffer_mib"].GetUint() == 64);
        runtime.Request(
            "modify",
            R"({"type":"netadapter","name":"edge","role":"source","options":{"backend":"af_packet","interfaces":["test1","test1"]}})",
            error::BAD_REQUEST);
        runtime.Request(
            "modify",
            R"({"type":"netadapter","name":"edge","role":"source","options":{"backend":"af_xdp_copy_skb","interfaces":["test1"]}})",
            error::UNAVAILABLE);
        runtime.Acquire();
        query = runtime.Request("query", "{}");
        assert(query["channels"][0]["in_use"].GetBool());
        runtime.Request("modify", modify, error::CONFLICT);
        runtime.Request("remove", remove, error::CONFLICT);
        query = runtime.Request("query", "{}");
        assert(query["channels"][0]["option_json"]["interfaces"].Size() == 2);
        runtime.Release();
        runtime.Request("modify", modify);
    }
    {
        Runtime runtime(db);
        const auto query = runtime.Request("query", "{}");
        const auto& options = query["channels"][0]["option_json"];
        assert(options["observation_domain_id"].GetUint64() == domain);
        assert(std::string(options["backend"].GetString()) == "pfring_classic");
        assert(options["snaplen"].GetUint() == 128 && options["buffer_mib"].GetUint() == 16);
        assert(!options["promiscuous"].GetBool() && options["interfaces"].Size() == 1);
        runtime.Request("remove", remove);
        assert(runtime.Request("query", "{}")["channels"].Empty());
    }
    assert(unlink(db.c_str()) == 0);
}
