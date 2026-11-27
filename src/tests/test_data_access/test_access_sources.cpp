// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <channels/kafka/kafka_plugin.h>
#include <framework/interfaces/iblock_stream_factory.h>
#include <framework/interfaces/iblock_stream_manager.h>
#include <framework/interfaces/istream_factory.h>
#include <framework/interfaces/istream_manager.h>
#include <services/data_access/data_access_plugin.h>
#include <unistd.h>
#include <cassert>
#include <common/loader.hpp>
#include <filesystem>
#include <fstream>

using namespace flowsql;
using namespace flowsql::dataset;
struct Libraries {
    PluginLoader* loader = PluginLoader::Single();
    ~Libraries() {
        loader->StopAll();
        assert(loader->Unload() == 0);
    }
};
class Providers : public IQuerier {
 public:
    PluginLoader* loader;
    channels::kafka::Plugin kafka;
    void* First(const Guid& iid) override {
        if (!std::memcmp(&iid, &IID_ROUTER_HANDLE, sizeof(Guid))) return static_cast<IRouterHandle*>(&kafka);
        return loader->First(iid);
    }
    int Traverse(const Guid& iid, fntraverse visit) override {
        if (!std::memcmp(&iid, &IID_ROUTER_HANDLE, sizeof(Guid)) && visit(static_cast<IRouterHandle*>(&kafka)))
            return 1;
        return loader->Traverse(iid, visit);
    }
};
int main() {
    char temporary[] = "/tmp/flowsql-access-sources-XXXXXX";
    assert(mkdtemp(temporary));
    const std::string root = temporary;
    std::filesystem::create_directories(root + "/frames");
    // One empty, valid PCAP. Discovery may stat it but must never open a reader.
    const uint32_t header[]{0xa1b2c3d4, 0x00040002, 0, 0, 65535, 1};
    std::ofstream(root + "/empty.pcap", std::ios::binary).write(reinterpret_cast<const char*>(header), sizeof(header));
    Libraries libraries;
    const char* names[]{"libflowsql_config_channel.so", "libflowsql_builtin.so",
                        "libflowsql_stream.so",         "libflowsql_npi.so",
                        "libflowsql_pcapfile.so",       "libflowsql_capture_af_packet.so",
                        "libflowsql_netadapter.so",     "libflowsql_catalog.so"};
    std::vector<std::string> options{
        "db_path=" + root + "/config.db",
        "",
        "db_path=" + root + "/stream.db",
        std::string("{\"ldfile\":\"") + FLOWSQL_SOURCE_DIR + "/plugins/npi/conf/protocols.yml\"}",
        "db_path=" + root + "/pcap.db",
        "",
        "{\"db_path\":\"" + root + "/netadapter.db\"}"};
    options.push_back("data_dir=" + root + "/frames;operator_db_path=" + root + "/catalog.db");
    std::vector<const char*> pointers;
    for (const auto& option : options) pointers.push_back(option.c_str());
    assert(libraries.loader->Load(get_absolute_process_path(), names, pointers.data(), 8) == 0);
    assert(libraries.loader->StartAll() == 0);
    Providers providers;
    providers.loader = libraries.loader;
    std::array<unsigned char, 32> key{};
    providers.kafka.Definitions().Open(root + "/kafka.db", key);
    auto& store = providers.kafka.Definitions();
    store.Mutate(
        "connections", "add",
        channels::kafka::Parse(
            R"({"name":"offline","bootstrap_servers":["metadata-only.invalid:1"],"security_protocol":"PLAINTEXT"})"));
    store.Mutate(
        "channels", "add",
        channels::kafka::Parse(
            R"({"name":"structured","connection_name":"offline","role":"both","topic":"events","consumer_group":"access","payload_kind":"structured","payload_options":{"source":{"encoding":"json","entity":"event","fields":[{"name":"value","path":"/v","type":"int64","nullable":true}]}}})"));
    store.Mutate(
        "channels", "add",
        channels::kafka::Parse(
            R"({"name":"packets","connection_name":"offline","role":"source","topic":"packets","consumer_group":"access","payload_kind":"packet"})"));
    store.Mutate(
        "channels", "add",
        channels::kafka::Parse(
            R"({"name":"sink","connection_name":"offline","role":"sink","topic":"output","payload_kind":"structured"})"));
    auto* streams = static_cast<IStreamManager*>(libraries.loader->First(IID_STREAM_MANAGER));
    assert(streams && streams->AddChannel("ring", "events", R"({"ring_size":64,"finite":false,"role":"both"})") == 0);
    libraries.loader->Traverse(IID_BLOCK_STREAM_MANAGER, [&](void* pointer) {
        auto* manager = static_cast<IBlockStreamManager*>(pointer);
        auto file = access::Parse("{}");
        Set(file, "path", root + "/empty.pcap", file.GetAllocator());
        const auto pcap = manager->AddChannel("pcapfile", "capture", Encode(file));
        assert(pcap == 0 || pcap == ENOTSUP);
        const auto net = manager->AddChannel(
            "netadapter", "live", R"({"backend":"af_packet","interfaces":["unopened0"],"promiscuous":false})");
        assert(net == 0 || net == ENOTSUP);
        return 0;
    });
    // Registration currently opens the legacy definition. Close it before the
    // observation so this test detects an implicit reopen during discovery.
    libraries.loader->Traverse(IID_BLOCK_STREAM_FACTORY, [&](void* pointer) {
        if (auto* source = static_cast<IBlockStreamFactory*>(pointer)->Get("pcapfile", "capture"))
            assert(source->Close() == 0);
        return 0;
    });
    auto control = [&](const std::string& path, const std::string& text) {
        fnRouterHandler handler;
        libraries.loader->Traverse(IID_ROUTER_HANDLE, [&](void* pointer) {
            static_cast<IRouterHandle*>(pointer)->EnumRoutes([&](const RouteItem& item) {
                if (item.uri == path) handler = item.handler;
            });
            return handler ? 1 : 0;
        });
        assert(handler);
        std::string response;
        assert(handler(path, text, response) == 0);
        return access::Parse(response);
    };
    control(
        "/channels/config/publish",
        R"({"name":"settings","expected_current_revision":0,"format":"json","schema_id":"example.settings","content_base64":"eyJzZWNyZXQiOiJjb25maWctYm9keS1tdXN0LW5vdC1sZWFrIn0="})");
    auto policy = access::Parse(
        R"({"subjects":[{"principal":"alice","token_env":"FLOWSQL_ACCESS_SOURCES_TOKEN","sources":[]}]})");
    auto& a = policy.GetAllocator();
    for (const auto& text : {R"({"kind":"block_stream","type":"kafka","name":"structured"})",
                             R"({"kind":"block_stream","type":"kafka","name":"packets"})",
                             R"({"kind":"block_stream","type":"kafka","name":"sink"})",
                             R"({"kind":"block_stream","type":"pcapfile","name":"capture"})",
                             R"({"kind":"block_stream","type":"netadapter","name":"live"})",
                             R"({"kind":"stream","type":"ring","name":"events"})",
                             R"({"kind":"config","reference":"config.settings@1"})"}) {
        Value grant(rapidjson::kObjectType), operations(rapidjson::kArrayType);
        grant.AddMember("locator", Copy(access::Parse(text), a), a);
        operations.PushBack("query", a);
        grant.AddMember("operations", operations, a);
        policy["subjects"][0]["sources"].PushBack(grant, a);
    }
    std::ofstream(root + "/policy.json") << Encode(policy);
    const std::string token = "isolated-sources-test-credential-0000000";
    setenv("FLOWSQL_ACCESS_SOURCES_TOKEN", token.c_str(), 1);
    httplib::Server reservation;
    const int port = reservation.bind_to_any_port("127.0.0.1");
    assert(port > 0);
    std::thread reserved([&] { reservation.listen_after_bind(); });
    reservation.wait_until_ready();
    reservation.stop();
    reserved.join();
    auto config = access::Parse("{}");
    auto& ca = config.GetAllocator();
    config.AddMember("port", port, ca);
    Set(config, "policy_file", root + "/policy.json", ca);
    Set(config, "state_path", root + "/state.db", ca);
    Set(config, "contract_file", std::string(FLOWSQL_SOURCE_DIR) + "/../config/agent-data-access/api.schema.json", ca);
    access::DataAccessPlugin plugin;
    assert(plugin.Option(Encode(config).c_str()) == 0 && plugin.Load(&providers) == 0 && plugin.Start() == 0);
    httplib::Client client("127.0.0.1", port);
    auto call = [&](const char* name, const Value& args, int status = 200) {
        auto body = access::Parse("{}");
        Set(body, "name", name, body.GetAllocator());
        body.AddMember("arguments", Copy(args, body.GetAllocator()), body.GetAllocator());
        auto response =
            client.Post("/data-access/call", {{"Authorization", "Bearer " + token}}, Encode(body), "application/json");
        if (!response || response->status != status)
            std::fprintf(stderr, "%s\n", response ? response->body.c_str() : "transport failed");
        assert(response && response->status == status);
        return access::Parse(response->body);
    };
    auto listing = call("list_sources", access::Parse("{}"));
    assert(listing["sources"].Size() == 7);
    for (const auto& source : listing["sources"].GetArray()) {
        assert(source["capabilities"]["describe"]["available"].GetBool());
        assert(!source["capabilities"]["query"]["supported"].GetBool());
        auto query = access::Parse(
            R"({"request_id":"denied","workflow":[{"type":"view","query":[{"op":"raw","fields":["f0"]}]}]})");
        query.AddMember("source_ref", Copy(source["source_ref"], query.GetAllocator()), query.GetAllocator());
        call("query", query, 422);
        auto description = access::Parse("{}");
        description.AddMember("source_ref", Copy(source["source_ref"], description.GetAllocator()),
                              description.GetAllocator());
        assert(call("describe_source", description)["source_ref"] == source["source_ref"]);
    }
    assert(Get(listing["sources"][0]["description"], "data_shape") == "structured");
    assert(Get(listing["sources"][0]["description"], "schema_state") == "known");
    assert(Get(listing["sources"][1]["description"], "data_shape") == "packet");
    assert(listing["sources"][2]["description"]["roles"][0] == "sink");
    assert(listing["sources"][3]["description"]["finite"].GetBool());
    assert(!listing["sources"][4]["description"]["finite"].GetBool());
    assert(listing["sources"][5]["description"]["roles"][0] == "both");
    assert(Get(listing["sources"][6]["description"], "reference") == "config.settings@1");
    const auto encoded = Encode(listing);
    assert(encoded.find(root) == std::string::npos && encoded.find("config-body-must-not-leak") == std::string::npos);
    libraries.loader->Traverse(IID_BLOCK_STREAM_FACTORY, [&](void* pointer) {
        auto* factory = static_cast<IBlockStreamFactory*>(pointer);
        if (auto* pcap = factory->Get("pcapfile", "capture")) assert(!pcap->IsOpened());
        if (auto* net = factory->Get("netadapter", "live")) assert(!net->IsOpened());
        return 0;
    });
    assert(!providers.kafka.Get("kafka", "sink"));
    auto args = access::Parse("{}");
    args.AddMember("source_ref", Copy(listing["sources"][0]["source_ref"], args.GetAllocator()), args.GetAllocator());
    auto changed = channels::kafka::Parse(
        R"({"name":"structured","expected_revision":1,"connection_name":"offline","role":"both","topic":"changed","consumer_group":"access","payload_kind":"structured","payload_options":{}})");
    store.Mutate("channels", "modify", std::move(changed));
    assert(Get(call("describe_source", args, 409)["error"], "code") == "SOURCE_CHANGED");
    plugin.Stop();
    std::puts("[PASS] real source catalogs, source/sink roles, revisions and discovery without opening data readers");
}
