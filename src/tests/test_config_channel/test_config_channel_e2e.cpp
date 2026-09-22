// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <common/loader.hpp>
#include <framework/interfaces/iconfig_channel_registry.h>

#ifdef FLOWSQL_CONFIG_LABELING_E2E
#include <arrow/api.h>
#include <common/network/netbase.h>
#include <framework/core/packet_codec.h>
#include <framework/interfaces/cpp_operator_plugin_abi.h>
#include <framework/interfaces/iblock_transform_operator.h>
#endif

#include <httplib.h>
#include <openssl/evp.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <arpa/inet.h>
#include <dlfcn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr size_t kMaxContentBytes = 8 * 1024 * 1024;
constexpr size_t kMaxControlRequestBytes = 12 * 1024 * 1024;

int ReservePort() {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
    socklen_t size = sizeof(address);
    assert(getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) == 0);
    const int port = ntohs(address.sin_port);
    close(fd);
    return port;
}

std::string Base64(const std::string& bytes) {
    std::string encoded(4 * ((bytes.size() + 2) / 3) + 1, '\0');
    const int size = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded.data()),
                                     reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
    assert(size >= 0);
    encoded.resize(static_cast<size_t>(size));
    return encoded;
}

std::string PublishRequest(const std::string& name, uint64_t expected, const std::string& content,
                           const std::string& format = "json", uint64_t base = 0,
                           const std::string& schema_id = "e2e-v1") {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    writer.StartObject();
    writer.Key("name");
    writer.String(name.c_str());
    writer.Key("expected_current_revision");
    writer.Uint64(expected);
    writer.Key("format");
    writer.String(format.c_str());
    writer.Key("schema_id");
    writer.String(schema_id.c_str());
    writer.Key("content_base64");
    writer.String(Base64(content).c_str());
    if (base) {
        writer.Key("base_revision");
        writer.Uint64(base);
    }
    writer.EndObject();
    return buffer.GetString();
}

rapidjson::Document Parse(const std::string& json) {
    rapidjson::Document document;
    document.Parse(json.c_str());
    assert(!document.HasParseError() && document.IsObject());
    return document;
}

class Runtime {
 public:
    explicit Runtime(const fs::path& root) {
        const int gateway_port = ReservePort();
        int router_port = ReservePort();
        while (router_port == gateway_port) router_port = ReservePort();
        web_port_ = ReservePort();
        while (web_port_ == gateway_port || web_port_ == router_port) web_port_ = ReservePort();
        const std::string gateway = "host=127.0.0.1;port=" + std::to_string(gateway_port);
        const std::string router = "host=127.0.0.1;port=" + std::to_string(router_port) +
                                   ";gateway=127.0.0.1:" + std::to_string(gateway_port);
        const std::string config = "db_path=" + (root / "config.db").string();
        const std::string web = "host=127.0.0.1;port=" + std::to_string(web_port_) +
                                ";gateway=127.0.0.1:" + std::to_string(gateway_port) +
                                ";db_path=:memory:;upload_dir=" + (root / "uploads").string();
        std::vector<std::string> libraries = {"libflowsql_gateway.so", "libflowsql_router.so",
                                              "libflowsql_config_channel.so", "libflowsql_web.so"};
        std::vector<std::string> options = {gateway, router, config, web};
#ifdef FLOWSQL_CONFIG_LABELING_E2E
        libraries.push_back("libflowsql_npi.so");
        options.push_back(std::string("{\"ldfile\":\"") + FLOWSQL_NPI_PROTOCOLS_PATH + "\",\"concurrency\":2}");
        libraries.push_back("libflowsql_flow_labeling.so");
        options.emplace_back();
#endif
        std::vector<const char*> library_pointers;
        std::vector<const char*> option_pointers;
        for (size_t index = 0; index < libraries.size(); ++index) {
            library_pointers.push_back(libraries[index].c_str());
            option_pointers.push_back(options[index].c_str());
        }
        assert(loader_->Load(flowsql::get_absolute_process_path(), library_pointers.data(), option_pointers.data(),
                             static_cast<int>(library_pointers.size())) == 0);
        assert(loader_->StartAll() == 0);
        bool ready = false;
        for (int attempt = 0; attempt < 200 && !ready; ++attempt) {
            httplib::Client client("127.0.0.1", web_port_);
            const auto result = client.Post("/api/channels/config/list", "{}", "application/json");
            ready = result && result->status == 200;
            if (!ready) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        assert(ready && "real Web/Gateway/Router/Config chain must become ready");
    }

    ~Runtime() {
        loader_->StopAll();
        assert(loader_->Unload() == 0);
    }

    std::string Post(const std::string& operation, const std::string& request, int status = 200) const {
        httplib::Client client("127.0.0.1", web_port_);
        client.set_connection_timeout(30);
        client.set_read_timeout(30);
        client.set_write_timeout(30);
        const auto result = client.Post("/api/channels/config/" + operation, request, "application/json");
        if (!result || result->status != status) {
            std::fprintf(stderr, "%s: expected HTTP %d, got %d: %s\n", operation.c_str(), status,
                         result ? result->status : 0, result ? result->body.c_str() : "transport failure");
        }
        assert(result && result->status == status);
        return result->body;
    }

    int PublishStatus(const std::string& request) const {
        httplib::Client client("127.0.0.1", web_port_);
        client.set_connection_timeout(30);
        client.set_read_timeout(30);
        client.set_write_timeout(30);
        const auto result = client.Post("/api/channels/config/publish", request, "application/json");
        assert(result);
        return result->status;
    }

    flowsql::IConfigChannelRegistryV1* Registry() const {
        auto* registry = static_cast<flowsql::IConfigChannelRegistryV1*>(
            loader_->First(flowsql::IID_CONFIG_CHANNEL_REGISTRY_V1));
        assert(registry);
        return registry;
    }

#ifdef FLOWSQL_CONFIG_LABELING_E2E
    flowsql::IQuerier* Querier() const { return loader_; }
#endif

 private:
    flowsql::PluginLoader* loader_ = flowsql::PluginLoader::Single();
    int web_port_ = 0;
};

// This test consumer resolves and compiles exactly once at task opening; events only read typed memory.
struct BoundTask {
    flowsql::ConfigChannelSnapshot snapshot;
    uint64_t value = 0;
    unsigned resolve_calls = 0;

    void Open(flowsql::IConfigChannelRegistryV1* registry) {
        std::string error;
        ++resolve_calls;
        assert(registry->Resolve("config.rules@1", &snapshot, &error) == 0);
        assert(snapshot.schema_id == "e2e-v1" && snapshot.format == "json");
        value = Parse(*snapshot.content)["value"].GetUint64();
    }

    uint64_t ProcessEvent() const { return value; }
};

std::string SnapshotRequest(uint64_t revision) {
    return "{\"exact_reference\":\"config.rules@" + std::to_string(revision) + "\"}";
}

void Save(const fs::path& path, const std::string& value) {
    std::ofstream output(path, std::ios::binary);
    output << value;
    assert(output.good());
}

std::string Read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    assert(input.is_open());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string TenThousandLabelConfig() {
    std::string yaml =
        "api_version: flowsql.io/flow-labeling/v1alpha1\n"
        "kind: FlowLabelingSet\n"
        "labels:\n";
    yaml.reserve(2 * 1024 * 1024);
    for (uint32_t id = 1; id <= 10000; ++id) {
        yaml += "  - id: " + std::to_string(id) + "\n";
        yaml += "    priority: " + std::to_string(10001 - id) + "\n";
        yaml += "    name: label-" + std::to_string(id) + "\n";
    }
    yaml +=
        "rules:\n"
        "  - id: representative-rule\n"
        "    label_id: 1\n"
        "    direction: bidirectional\n"
        "    matches:\n"
        "      - field: destination_port\n"
        "        min: 443\n"
        "        max: 443\n";
    return yaml;
}

#ifdef FLOWSQL_CONFIG_LABELING_E2E
std::string FlowLabelingConfig(uint32_t label_id) {
    return "api_version: flowsql.io/flow-labeling/v1alpha1\n"
           "kind: FlowLabelingSet\n"
           "spec:\n"
           "  engine:\n"
           "    type: dpdk-acl\n"
           "    categories: 1\n"
           "    algorithm: scalar\n"
           "    numa_socket_id: any\n"
           "    max_runtime_bytes: 8388608\n"
           "    limits:\n"
           "      max_labels: 10000\n"
           "      max_logical_rules: 50000\n"
           "      max_compiled_rules: 100000\n"
           "      max_expanded_fields: 64\n"
           "      reject_duplicate_label_priorities: true\n"
           "  labels:\n"
           "    - id: " +
           std::to_string(label_id) +
           "\n"
           "      priority: 1000\n"
           "      name: e2e-web\n"
           "  rules:\n"
           "    - id: e2e-web-rule\n"
           "      label_id: " +
           std::to_string(label_id) +
           "\n"
           "      direction: bidirectional\n"
           "      matches:\n"
           "        - field: destination_ipv4_prefix\n"
           "          value: 198.51.100.2\n"
           "          prefix_bits: 32\n"
           "        - field: destination_port\n"
           "          min: 443\n"
           "          max: 443\n";
}

flowsql::packet::PacketRecord MakeTcpPacket(const char* source, uint16_t source_port, const char* destination,
                                            uint16_t destination_port, int64_t timestamp_ns, uint64_t sequence) {
    auto bytes = std::make_shared<std::vector<uint8_t>>(sizeof(flowsql::Ipv4Header) + sizeof(flowsql::TcpHeader));
    auto* ip = reinterpret_cast<flowsql::Ipv4Header*>(bytes->data());
    ip->version = 4;
    ip->ihl = 5;
    ip->total_length = htons(static_cast<uint16_t>(bytes->size()));
    ip->protocol = flowsql::ipv4::eNext::TCP;
    assert(inet_pton(AF_INET, source, ip->src_addr.bytes) == 1);
    assert(inet_pton(AF_INET, destination, ip->dst_addr.bytes) == 1);
    auto* tcp = reinterpret_cast<flowsql::TcpHeader*>(bytes->data() + sizeof(flowsql::Ipv4Header));
    tcp->src_port = htons(source_port);
    tcp->dst_port = htons(destination_port);
    tcp->offset = 5;
    tcp->flags.flags_bit.ack = 1;

    flowsql::packet::PacketRecord record;
    record.meta.timestamp_ns = timestamp_ns;
    record.meta.captured_len = static_cast<uint32_t>(bytes->size());
    record.meta.wire_len = record.meta.captured_len;
    record.meta.link_type = 1;
    record.meta.source_id = 0;
    record.meta.sequence = sequence;
    record.raw_data.owner = bytes;
    record.raw_data.data = bytes->data();
    record.raw_data.size = static_cast<uint32_t>(bytes->size());
    record.layer.status = flowsql::packet::LayerStatus::kDecoded;
    record.layer.layer_count = 2;
    record.layer.layers[0] = {static_cast<uint16_t>(flowsql::eLayer::IPv4), 0};
    record.layer.layers[1] = {static_cast<uint16_t>(flowsql::eLayer::TCP), sizeof(flowsql::Ipv4Header)};
    record.layer.network_layer_index = 0;
    record.layer.transport_layer_index = 1;
    record.layer.payload_offset = static_cast<uint32_t>(bytes->size());
    record.layer.src_ip = ip->src_addr;
    record.layer.dst_ip = ip->dst_addr;
    record.layer.transport_protocol = flowsql::ipv4::eNext::TCP;
    record.layer.src_port = source_port;
    record.layer.dst_port = destination_port;
    record.layer.ports_valid = 1;
    return record;
}

std::shared_ptr<arrow::RecordBatch> LabelingInput() {
    const std::vector<flowsql::packet::PacketRecord> records = {
        MakeTcpPacket("192.0.2.1", 50000, "198.51.100.2", 443, 100, 1),
        MakeTcpPacket("198.51.100.2", 443, "192.0.2.1", 50000, 200, 2),
        MakeTcpPacket("192.0.2.9", 50001, "198.51.100.2", 80, 300, 3),
    };
    std::shared_ptr<arrow::RecordBatch> batch;
    std::string error;
    assert(flowsql::packet::EncodePacketBatch(records, &batch, &error) == flowsql::packet::PacketBatchError::kNone);
    assert(batch != nullptr && error.empty());
    return batch;
}

void RunPublishedLabelingTask(Runtime* runtime, const char* reference, uint32_t expected_label) {
    void* handle = dlopen(FLOWSQL_NPM_BASIC_PLUGIN_PATH, RTLD_NOW | RTLD_LOCAL);
    assert(handle != nullptr);
    auto create = reinterpret_cast<flowsql::CppOperatorPluginCreateCapabilityV2Fn>(
        dlsym(handle, flowsql::kCppOperatorPluginCreateCapabilityV2Symbol));
    auto destroy = reinterpret_cast<flowsql::CppOperatorPluginDestroyCapabilityV2Fn>(
        dlsym(handle, flowsql::kCppOperatorPluginDestroyCapabilityV2Symbol));
    assert(create != nullptr && destroy != nullptr);
    void* capability = create(0, runtime->Querier());
    assert(capability != nullptr);
    auto* provider = static_cast<flowsql::IBlockTransformOperatorV1*>(capability);

    const std::string task_id = std::string("config-labeling-") + reference;
    const std::string with_json =
        std::string(R"({"input_namespace":"pcapfile.capture","source_domains":"0:77","features":"basic,labeling",)") +
        R"("parameters":"{\"schema_version\":1,\"core\":{\"labeling\":\")" + reference + R"(\"}}"})";
    const std::string filter_plan = R"({"version":1,"root":null})";
    flowsql::BlockTransformTaskConfigV1 config;
    config.task_id = task_id.c_str();
    config.with_params_json = with_json.c_str();
    config.pushed_filter_plan_json = filter_plan.c_str();
    flowsql::IBlockTransformTaskV1* task = nullptr;
    assert(provider->CreateTask(config, &task) == 0 && task != nullptr);
    std::shared_ptr<arrow::Schema> output_schema;
    assert(task->Open(flowsql::packet::PacketSchema(), &output_schema) == 0);
    assert(output_schema != nullptr && output_schema->GetFieldIndex("primary_label_id") == 17);

    std::vector<flowsql::BlockTransformOutputV1> outputs;
    assert(task->ProcessBlock(LabelingInput(), 1, &outputs) ==
           static_cast<int>(flowsql::BlockTransformStatusV1::kContinue));
    outputs.clear();
    assert(task->Flush(&outputs) == 0 && outputs.size() == 1);
    const auto& output = outputs[0].batch;
    assert(output != nullptr && output->num_rows() == 2);
    auto labels = std::static_pointer_cast<arrow::UInt32Array>(output->column(17));
    assert(labels != nullptr && labels->null_count() == 0);
    assert(labels->Value(0) == expected_label && labels->Value(1) == 0);

    provider->ReleaseTask(task);
    labels.reset();
    outputs.clear();
    output_schema.reset();
    destroy(0, capability);
    dlclose(handle);
}
#endif

void Seed(const fs::path& root) {
    BoundTask task;
    {
        Runtime runtime(root);
        const std::string first = PublishRequest("rules", 0, R"({"value":1})");
        assert(Parse(runtime.Post("publish", first))["revision"].GetUint64() == 1);
        task.Open(runtime.Registry());
        assert(!Parse(runtime.Post("publish", first))["created_revision"].GetBool());

        std::atomic<int> ready{0};
        std::atomic<bool> go{false};
        int statuses[2] = {0, 0};
        // Query response status directly because either request may be the winner.
        auto race = [&](int index) {
            ready.fetch_add(1);
            while (!go.load()) std::this_thread::yield();
            const std::string content = "{\"value\":" + std::to_string(index + 2) + "}";
            statuses[index] = runtime.PublishStatus(PublishRequest("rules", 1, content));
        };
        std::thread a(race, 0);
        std::thread b(race, 1);
        while (ready.load() != 2) std::this_thread::yield();
        go.store(true);
        a.join();
        b.join();
        assert((statuses[0] == 200 && statuses[1] == 409) || (statuses[0] == 409 && statuses[1] == 200));
        runtime.Post("publish", PublishRequest("rules", 2, R"({"bad":1,"bad":2})"), 400);
        runtime.Post("publish", PublishRequest("rules", 2, std::string(kMaxContentBytes + 1, 'x')), 413);
        flowsql::ConfigChannelSnapshot current;
        std::string error;
        assert(runtime.Registry()->Resolve("config.rules@2", &current, &error) == 0);
        assert(runtime.Registry()->Resolve("config.rules@3", &current, &error) == ENOENT);
        assert(Parse(runtime.Post("publish", PublishRequest("rules", 2, R"({"value":1})", "json", 1)))
                   ["revision"].GetUint64() == 3);
        for (const auto& [name, format, content] : {
                 std::tuple{"yaml-rules", "yaml", "value: 1\n"},
                 std::tuple{"xml-rules", "xml", "<root><value>1</value></root>"}}) {
            assert(Parse(runtime.Post("publish", PublishRequest(name, 0, content, format)))
                       ["revision"].GetUint64() == 1);
        }
        const std::string max_content = "\"" + std::string(kMaxContentBytes - 2, 'x') + "\"";
        const auto maximum_result = Parse(runtime.Post("publish", PublishRequest("maximum", 0, max_content)));
        assert(maximum_result["revision"].GetUint64() == 1);
        flowsql::ConfigChannelSnapshot maximum;
        assert(runtime.Registry()->Resolve("config.maximum@1", &maximum, &error) == 0);
        assert(maximum.content_bytes == kMaxContentBytes && *maximum.content == max_content);
        assert(maximum.sha256_hex == maximum_result["sha256_hex"].GetString());
        const auto maximum_wire = Parse(runtime.Post("resolve", R"({"exact_reference":"config.maximum@1"})"));
        assert(maximum_wire["content_bytes"].GetUint64() == kMaxContentBytes);
        assert(maximum_wire["sha256_hex"].GetString() == maximum.sha256_hex);

        const std::string ten_k_labels = TenThousandLabelConfig();
        assert(ten_k_labels.size() > 512 * 1024 && ten_k_labels.size() < kMaxContentBytes);
        const std::string labeling_schema = "flowsql.io/flow-labeling/v1alpha1";
        const auto ten_k_result =
            Parse(runtime.Post("publish", PublishRequest("ten-k-labels", 0, ten_k_labels, "yaml", 0, labeling_schema)));
        flowsql::ConfigChannelSnapshot ten_k_snapshot;
        assert(runtime.Registry()->Resolve("config.ten-k-labels@1", &ten_k_snapshot, &error) == 0);
        assert(ten_k_snapshot.revision == 1 && ten_k_snapshot.schema_id == labeling_schema);
        assert(ten_k_snapshot.content_bytes == ten_k_labels.size() && *ten_k_snapshot.content == ten_k_labels);
        assert(ten_k_snapshot.sha256_hex == ten_k_result["sha256_hex"].GetString());
        assert(ten_k_snapshot.sha256_hex.size() == 64);

#ifdef FLOWSQL_CONFIG_LABELING_E2E
        const auto labeling_v1 = Parse(runtime.Post(
            "publish", PublishRequest("e2e-labels", 0, FlowLabelingConfig(1001), "yaml", 0, labeling_schema)));
        assert(labeling_v1["revision"].GetUint64() == 1);
        RunPublishedLabelingTask(&runtime, "config.e2e-labels@1", 1001);
        const auto labeling_v2 = Parse(runtime.Post(
            "publish", PublishRequest("e2e-labels", 1, FlowLabelingConfig(2002), "yaml", 1, labeling_schema)));
        assert(labeling_v2["revision"].GetUint64() == 2);
        RunPublishedLabelingTask(&runtime, "config.e2e-labels@1", 1001);
        RunPublishedLabelingTask(&runtime, "config.e2e-labels@2", 2002);
#endif

        runtime.Post("list", std::string(kMaxControlRequestBytes + 1, ' '), 413);
        runtime.Post("resolve", R"({"exact_reference":"config.rules@latest"})", 400);
        runtime.Post("resolve", SnapshotRequest(4), 404);
        const auto page = Parse(runtime.Post("list", R"({"limit":1})"));
        assert(page["items"].Size() == 1 && !page["items"][0].HasMember("content_base64"));
        assert(std::string(page["next_cursor"].GetString()).size() > 0);
        const auto history = Parse(runtime.Post("history", R"({"name":"rules"})"));
        assert(history["items"].Size() == 3 && history["items"][0]["base_revision"].GetUint64() == 1);
        for (uint64_t revision = 1; revision <= 3; ++revision) {
            Save(root / ("revision-" + std::to_string(revision)), runtime.Post("resolve", SnapshotRequest(revision)));
        }
        Save(root / "history", runtime.Post("history", R"({"name":"rules"})"));
        Save(root / "list", runtime.Post("list", "{}"));
        assert(task.resolve_calls == 1 && task.ProcessEvent() == 1);
    }
    assert(task.ProcessEvent() == 1 && task.resolve_calls == 1 && *task.snapshot.content == R"({"value":1})");
}

void Recover(const fs::path& root) {
    Runtime runtime(root);
    assert(runtime.Post("list", "{}") == Read(root / "list"));
    assert(runtime.Post("history", R"({"name":"rules"})") == Read(root / "history"));
    for (uint64_t revision = 1; revision <= 3; ++revision) {
        assert(runtime.Post("resolve", SnapshotRequest(revision)) ==
               Read(root / ("revision-" + std::to_string(revision))));
    }
    BoundTask task;
    task.Open(runtime.Registry());
    assert(task.ProcessEvent() == 1 && task.resolve_calls == 1);
    assert(Parse(runtime.Post("publish", PublishRequest("rules", 3, R"({"value":4})")))
               ["revision"].GetUint64() == 4);
    assert(runtime.Post("resolve", SnapshotRequest(1)) == Read(root / "revision-1"));
    for (int event = 0; event < 1000; ++event) assert(task.ProcessEvent() == 1);
    assert(task.resolve_calls == 1);
}

void RunChild(const fs::path& root, const char* phase) {
    const std::string executable = fs::canonical("/proc/self/exe").string();
    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        execl(executable.c_str(), executable.c_str(), phase, root.c_str(), nullptr);
        _exit(127);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3) {
        if (std::string(argv[1]) == "seed") Seed(argv[2]);
        else if (std::string(argv[1]) == "recover") Recover(argv[2]);
        else return 2;
        return 0;
    }
    char pattern[] = "/tmp/flowsql-config-e2e-XXXXXX";
    assert(mkdtemp(pattern));
    const fs::path root(pattern);
    RunChild(root, "seed");
    RunChild(root, "recover");
    fs::remove_all(root);
    std::puts("Config Channel real HTTP/process-restart/immutable-consumer E2E passed");
    return 0;
}
