// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <tests/common/packet_family_test_support.h>

#include <sqlite3.h>
#include <unistd.h>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include <common/error_code.h>
#include <framework/builtin/dataframe/concat_operator.h>
#include <framework/builtin/dataframe/hstack_operator.h>
#include <framework/builtin/dataframe/passthrough_operator.h>
#include <framework/core/dataframe.h>
#include <framework/core/dataframe_channel.h>
#include <framework/core/packet_codec.h>
#include <framework/interfaces/cpp_operator_plugin_abi.h>
#include <framework/interfaces/ibinaddon_host.h>
#include <framework/interfaces/iblock_transform_operator.h>
#include <framework/interfaces/ibuiltin_registry.h>
#include <framework/interfaces/icpp_operator_plugin_registry.h>
#include <framework/interfaces/ioperator.h>
#include <framework/interfaces/ioperator_catalog.h>
#include <framework/interfaces/ioperator_registry.h>
#include <framework/interfaces/ipython_operator_publisher.h>
#include <framework/interfaces/irouter_handle.h>
#include <rapidjson/document.h>
#include <services/binaddon/binaddon_host_plugin.h>
#include <services/catalog/catalog_plugin.h>
#include <tests/support/scoped_shared_library.h>
#include <common/channel_dataset.hpp>

using namespace flowsql;
using namespace flowsql::binaddon;
using namespace flowsql::catalog;

#ifndef FIXTURE_CPP_SO_PATH
#define FIXTURE_CPP_SO_PATH ""
#endif

#ifndef FIXTURE_BAD_ABI_SO_PATH
#define FIXTURE_BAD_ABI_SO_PATH ""
#endif

#ifndef FIXTURE_CPP_CAPABILITIES_SO_PATH
#define FIXTURE_CPP_CAPABILITIES_SO_PATH ""
#endif

#define ASSERT_TRUE(expr)                                                \
    do {                                                                 \
        if (!(expr)) {                                                   \
            std::printf("[FAIL] %s:%d %s\n", __FILE__, __LINE__, #expr); \
            std::fflush(stdout);                                         \
            assert(false);                                               \
        }                                                                \
    } while (0)

#define ASSERT_EQ(a, b)                                                         \
    do {                                                                        \
        auto _a = (a);                                                          \
        auto _b = (b);                                                          \
        if (!(_a == _b)) {                                                      \
            std::printf("[FAIL] %s:%d %s != %s\n", __FILE__, __LINE__, #a, #b); \
            std::fflush(stdout);                                                \
            assert(false);                                                      \
        }                                                                       \
    } while (0)

static std::string MakeTempDir(const char* suffix) {
    std::filesystem::path p = std::filesystem::temp_directory_path() /
                              ("flowsql_catalog_test_" + std::string(suffix) + "_" + std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    std::filesystem::create_directories(p, ec);
    return p.string();
}

static std::string MakeCatalogOption(const std::string& data_dir) {
    return "data_dir=" + data_dir + ";operator_db_dir=" + data_dir + "/catalog";
}

static std::string MakeCatalogOptionWithDbPath(const std::string& data_dir, const std::string& db_path) {
    return "data_dir=" + data_dir + ";operator_db_path=" + db_path;
}

static std::shared_ptr<DataFrameChannel> MakeChannel(const std::string& name, int rows = 2) {
    auto ch = std::make_shared<DataFrameChannel>("dataframe", name);
    ch->Open();

    DataFrame df;
    df.SetSchema({
        {"id", DataType::INT64, 0, ""},
        {"score", DataType::DOUBLE, 0, ""},
        {"name", DataType::STRING, 0, ""},
    });
    for (int i = 0; i < rows; ++i) {
        df.AppendRow({int64_t(i + 1), double(i) + 0.5, std::string("u") + std::to_string(i + 1)});
    }
    ASSERT_EQ(ch->Write(&df), 0);
    return ch;
}

static std::shared_ptr<DataFrameChannel> MakePacketPreviewChannel(const std::string& name) {
    std::vector<flowsql::testing::PacketRecord> records;
    for (size_t row = 0; row < 3; ++row) {
        const size_t byte_count = row == 0 ? 0 : (row == 1 ? 64 : 65);
        auto bytes = std::make_shared<std::vector<uint8_t>>(byte_count);
        for (size_t index = 0; index < byte_count; ++index) {
            (*bytes)[index] = static_cast<uint8_t>(index);
        }

        flowsql::testing::PacketRecord record;
        record.meta.timestamp_ns = static_cast<int64_t>(row + 1) * 1000;
        record.meta.captured_len = static_cast<uint32_t>(byte_count);
        record.meta.wire_len = static_cast<uint32_t>(byte_count);
        record.meta.link_type = 1;
        record.meta.source_id = 7;
        record.meta.sequence = row;
        record.raw_data.owner = bytes;
        record.raw_data.data = bytes->empty() ? nullptr : bytes->data();
        record.raw_data.size = static_cast<uint32_t>(bytes->size());
        records.push_back(std::move(record));
    }

    records[1].layer.status = packet::LayerStatus::kDecoded;
    records[1].layer.layer_count = 2;
    records[1].layer.layers[0] = {1, 0};
    records[1].layer.layers[1] = {300, 14};
    records[1].layer.src_mac_valid = 1;
    const uint8_t mac[] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    std::memcpy(records[1].layer.src_mac.bytes, mac, sizeof(mac));

    packet::IPv6Address ipv6;
    for (size_t index = 0; index < sizeof(ipv6.bytes); ++index) {
        ipv6.bytes[index] = static_cast<uint8_t>(index);
    }
    records[2].layer.SetEndpoints(ipv6, ipv6);
    records[2].layer.transport_protocol = 6;
    records[2].layer.src_port = 3389;
    records[2].layer.dst_port = 49152;
    records[2].layer.ports_valid = 1;
    records[2].protocol.status = packet::ProtocolStatus::kIdentified;
    records[2].protocol.id = 400;
    records[2].protocol.sub_id = 1;

    std::shared_ptr<arrow::RecordBatch> batch;
    std::string error;
    ASSERT_EQ(flowsql::testing::EncodePacketBatch(records, &batch, &error), packet::PacketBatchError::kNone);
    ASSERT_TRUE(batch != nullptr);

    DataFrame frame;
    frame.FromArrow(batch);
    auto channel = std::make_shared<DataFrameChannel>("dataframe", name);
    ASSERT_EQ(channel->Open(), 0);
    ASSERT_EQ(channel->Write(&frame), 0);
    return channel;
}

static int JsonColumnIndex(const rapidjson::Value& columns, const char* name) {
    for (rapidjson::SizeType index = 0; index < columns.Size(); ++index) {
        if (columns[index].IsString() && std::strcmp(columns[index].GetString(), name) == 0) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

static std::string ByteSequenceHex(size_t byte_count) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string output;
    output.reserve(byte_count * 2);
    for (size_t index = 0; index < byte_count; ++index) {
        const uint8_t value = static_cast<uint8_t>(index);
        output.push_back(kHex[value >> 4]);
        output.push_back(kHex[value & 0x0f]);
    }
    return output;
}

static int ChannelRowCount(IDataFrameChannel* ch) {
    DataFrame out;
    if (!ch || ch->Read(&out) != 0) return -1;
    return out.RowCount();
}

static std::shared_ptr<IDataFrameChannel> GetDataFrame(CatalogPlugin& p, const char* name) {
    return std::dynamic_pointer_cast<IDataFrameChannel>(p.Get(name));
}

static std::string CopyToTmp(const std::string& src_path, const std::string& dir, const std::string& filename) {
    std::filesystem::path dst = std::filesystem::path(dir) / filename;
    std::error_code ec;
    std::filesystem::copy_file(src_path, dst, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) return "";
    return dst.string();
}

static std::string RuntimeOperatorsDir() {
    char exe_path[1024];
    ssize_t len = ::readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len > 0) {
        exe_path[len] = '\0';
        std::string exe_dir(exe_path);
        size_t pos = exe_dir.find_last_of('/');
        if (pos != std::string::npos) return exe_dir.substr(0, pos) + "/operators";
    }
    return "operators";
}

static std::string RuntimeOperatorPyPath(const std::string& category, const std::string& name) {
    std::filesystem::path p(RuntimeOperatorsDir());
    p /= (category + "_" + name + ".py");
    return p.string();
}

class TestQuerier : public IQuerier {
 public:
    IPythonOperatorPublisher* publisher = nullptr;
    IOperatorRegistry* op_registry = nullptr;
    IBinAddonHost* binaddon_host = nullptr;
    IBuiltinRegistry* builtin_registry = nullptr;
    ICppOperatorPluginRegistry* cpp_operator_plugin_registry = nullptr;
    std::atomic<int> cpp_operator_plugin_registry_queries{0};

    int Traverse(const Guid&, fntraverse) override { return 0; }

    void* First(const Guid& iid) override {
        if (std::memcmp(&iid, &IID_PYTHON_OPERATOR_PUBLISHER, sizeof(Guid)) == 0) return publisher;
        if (memcmp(&iid, &IID_OPERATOR_REGISTRY, sizeof(Guid)) == 0) {
            return op_registry;
        }
        if (memcmp(&iid, &IID_BINADDON_HOST, sizeof(Guid)) == 0) {
            return binaddon_host;
        }
        if (memcmp(&iid, &IID_BUILTIN_REGISTRY, sizeof(Guid)) == 0) {
            return builtin_registry;
        }
        if (memcmp(&iid, &IID_CPP_OPERATOR_PLUGIN_REGISTRY, sizeof(Guid)) == 0) {
            cpp_operator_plugin_registry_queries.fetch_add(1, std::memory_order_relaxed);
            return cpp_operator_plugin_registry;
        }
        return nullptr;
    }
};

class TestCppOperatorPluginRegistry final : public ICppOperatorPluginRegistry {
 public:
    int Acquire(const char* category, const char* name, const Guid& contract_iid,
                CppOperatorCapabilityLease* lease) override {
        if (!lease) return -1;
        *lease = {};
        if (!category || !name || std::strcmp(category, "fixture") != 0 || std::strcmp(name, "echo") != 0 ||
            std::memcmp(&contract_iid, &IID_OPERATOR, sizeof(Guid)) != 0) {
            return -1;
        }

        auto state = std::make_shared<State>();
        lease->capability = &state->marker;
        lease->lifetime = state;
        last_state_ = state;
        return 0;
    }

    bool LastLeaseExpired() const { return last_state_.expired(); }

 private:
    struct State {
        int marker = 42;
    };

    std::weak_ptr<State> last_state_;
};

class TestBuiltinRegistry : public IBuiltinRegistry {
 public:
    int FindStreamChannelType(const std::string&, StreamChannelTypeDescriptor*) override { return -1; }

    void ListStreamChannelTypes(std::function<void(const StreamChannelTypeDescriptor&)>) override {}

    void ListBuiltinOperators(std::function<void(const BuiltinOperatorDescriptor&)> callback) override {
        if (!callback) return;
        for (const auto& item : descriptors_) callback(item);
    }

    IOperator* CreateBuiltinOperator(const std::string& category, const std::string& name) override {
        const std::string full = category + "." + name;
        for (const auto& item : descriptors_) {
            if ((item.category + "." + item.name) == full) {
                return item.factory ? item.factory() : nullptr;
            }
        }
        return nullptr;
    }

 private:
    const std::vector<BuiltinOperatorDescriptor> descriptors_ = {
        {"builtin", "passthrough", []() -> IOperator* { return new PassthroughOperator(); }},
        {"builtin", "concat", []() -> IOperator* { return new ConcatOperator(); }},
        {"builtin", "hstack", []() -> IOperator* { return new HstackOperator(); }},
    };
};

static void TestRegistryBasic() {
    std::puts("[TEST] T04-T11 registry basic ...");
    const std::string dir = MakeTempDir("basic");

    CatalogPlugin p;
    TestBuiltinRegistry builtin_registry;
    TestQuerier querier;
    querier.builtin_registry = &builtin_registry;
    const std::string opt = MakeCatalogOption(dir);
    ASSERT_EQ(p.Option(opt.c_str()), 0);
    ASSERT_EQ(p.Load(&querier), 0);
    ASSERT_EQ(p.Start(), 0);

    auto ch1 = MakeChannel("result");

    // T04
    ASSERT_EQ(p.Register("result", std::static_pointer_cast<IChannel>(ch1)), 0);
    auto got = GetDataFrame(p, "result");
    ASSERT_TRUE(got != nullptr);
    ASSERT_EQ(ChannelRowCount(got.get()), 2);

    // T05
    ASSERT_EQ(p.Register("result", std::static_pointer_cast<IChannel>(MakeChannel("result_dup"))), -1);
    ASSERT_EQ(ChannelRowCount(GetDataFrame(p, "result").get()), 2);

    // T06
    ASSERT_TRUE(GetDataFrame(p, "missing") == nullptr);

    // T11
    std::set<std::string> names;
    p.List([&](const char* name, std::shared_ptr<IChannel>) { names.insert(name); });
    ASSERT_EQ(names.size(), size_t(1));
    ASSERT_TRUE(names.count("result") == 1);

    // T09
    ASSERT_EQ(p.Rename("result", "result_renamed"), 0);
    ASSERT_TRUE(GetDataFrame(p, "result") == nullptr);
    ASSERT_TRUE(GetDataFrame(p, "result_renamed") != nullptr);
    ASSERT_TRUE(std::filesystem::exists(std::filesystem::path(dir) / "result_renamed.csv"));

    // T10
    ASSERT_EQ(p.Register("another", std::static_pointer_cast<IChannel>(MakeChannel("another"))), 0);
    ASSERT_EQ(p.Rename("result_renamed", "another"), -1);
    ASSERT_TRUE(GetDataFrame(p, "result_renamed") != nullptr);

    // T07
    ASSERT_EQ(p.Unregister("result_renamed"), 0);
    ASSERT_TRUE(GetDataFrame(p, "result_renamed") == nullptr);
    ASSERT_TRUE(!std::filesystem::exists(std::filesystem::path(dir) / "result_renamed.csv"));

    // T08
    ASSERT_EQ(p.Unregister("not_exists"), -1);

    ASSERT_EQ(p.Stop(), 0);
    std::puts("[PASS] T04-T11 registry basic");
}

static void TestRestartRecover() {
    std::puts("[TEST] T12 restart recover ...");
    const std::string dir = MakeTempDir("recover");

    {
        CatalogPlugin p1;
        TestBuiltinRegistry builtin_registry;
        TestQuerier querier;
        querier.builtin_registry = &builtin_registry;
        const std::string opt = MakeCatalogOption(dir);
        ASSERT_EQ(p1.Option(opt.c_str()), 0);
        ASSERT_EQ(p1.Load(&querier), 0);
        ASSERT_EQ(p1.Start(), 0);
        ASSERT_EQ(p1.Register("daily", std::static_pointer_cast<IChannel>(MakeChannel("daily", 3))), 0);
    }

    CatalogPlugin p2;
    TestBuiltinRegistry builtin_registry;
    TestQuerier querier;
    querier.builtin_registry = &builtin_registry;
    const std::string opt = MakeCatalogOption(dir);
    ASSERT_EQ(p2.Option(opt.c_str()), 0);
    ASSERT_EQ(p2.Load(&querier), 0);
    ASSERT_EQ(p2.Start(), 0);

    auto got = GetDataFrame(p2, "daily");
    ASSERT_TRUE(got != nullptr);
    ASSERT_EQ(ChannelRowCount(got.get()), 3);
    std::puts("[PASS] T12 restart recover");
}

static void TestConcurrency() {
    std::puts("[TEST] T13 concurrent registry ...");
    const std::string dir = MakeTempDir("concurrency");

    CatalogPlugin p;
    TestBuiltinRegistry builtin_registry;
    TestQuerier querier;
    querier.builtin_registry = &builtin_registry;
    const std::string opt = MakeCatalogOption(dir);
    ASSERT_EQ(p.Option(opt.c_str()), 0);
    ASSERT_EQ(p.Load(&querier), 0);
    ASSERT_EQ(p.Start(), 0);

    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&p, t]() {
            const std::string name = "ch_" + std::to_string(t);
            auto ch = MakeChannel(name, 1);
            ASSERT_EQ(p.Register(name.c_str(), std::static_pointer_cast<IChannel>(ch)), 0);
            auto got = GetDataFrame(p, name.c_str());
            ASSERT_TRUE(got != nullptr);
            ASSERT_EQ(ChannelRowCount(got.get()), 1);
            ASSERT_EQ(p.Unregister(name.c_str()), 0);
        });
    }
    for (auto& th : threads) th.join();

    size_t cnt = 0;
    p.List([&](const char*, std::shared_ptr<IChannel>) { ++cnt; });
    ASSERT_EQ(cnt, size_t(0));
    std::puts("[PASS] T13 concurrent registry");
}

static void TestPersistFail() {
    std::puts("[TEST] T14 persist fail ...");
    CatalogPlugin p;
    TestBuiltinRegistry builtin_registry;
    TestQuerier querier;
    querier.builtin_registry = &builtin_registry;
    ASSERT_EQ(p.Option("data_dir=/proc/flowsql_catalog_test_no_permission"), 0);
    ASSERT_EQ(p.Load(&querier), 0);

    // Start 可能失败（目录不可创建），但 Register 也必须失败且不产生内存注册。
    (void)p.Start();
    ASSERT_EQ(p.Register("bad", std::static_pointer_cast<IChannel>(MakeChannel("bad"))), -1);
    ASSERT_TRUE(GetDataFrame(p, "bad") == nullptr);
    std::puts("[PASS] T14 persist fail");
}

static void TestOperatorRegistry() {
    std::puts("[TEST] T15-T17 operator registry ...");
    const std::string dir = MakeTempDir("operator");

    CatalogPlugin p;
    TestBuiltinRegistry builtin_registry;
    TestQuerier querier;
    querier.builtin_registry = &builtin_registry;
    const std::string opt = MakeCatalogOption(dir);
    ASSERT_EQ(p.Option(opt.c_str()), 0);
    ASSERT_EQ(p.Load(&querier), 0);  // Load 自动注册 passthrough
    ASSERT_EQ(p.Start(), 0);

    // T15
    IOperator* op = p.Create("builtin.passthrough");
    ASSERT_TRUE(op != nullptr);
    ASSERT_EQ(op->Name(), std::string("passthrough"));
    delete op;
    ASSERT_EQ(p.Register("builtin.passthrough", []() -> IOperator* { return new ConcatOperator(); }), -1);
    op = p.Create("builtin.passthrough");
    ASSERT_TRUE(op != nullptr);
    ASSERT_EQ(op->Name(), std::string("passthrough"));
    delete op;

    ASSERT_TRUE(p.Create("passthrough") == nullptr);
    ASSERT_EQ(p.Register("passthrough", []() -> IOperator* { return new PassthroughOperator(); }), -1);
    ASSERT_TRUE(p.Create("passthrough") == nullptr);

    // T16
    ASSERT_TRUE(p.Create("not_exists") == nullptr);

    // T17
    std::set<std::string> names;
    p.List([&](const char* name) { names.insert(name); });
    ASSERT_TRUE(names.count("builtin.passthrough") == 1);

    ASSERT_EQ(names.size(), size_t(3));
    ASSERT_EQ(names.count("passthrough"), size_t(0));
    std::puts("[PASS] T15-T17 operator registry");
}

class TestPythonPublisher : public IPythonOperatorPublisher {
 public:
    int publications = 0;
    int ReadPythonSource(const char*, const char*, std::string* response, DataSemanticsError*) override {
        std::ifstream input(RuntimeOperatorPyPath("ml", "clean"));
        ASSERT_TRUE(input.good());
        dataset::Json source(rapidjson::kObjectType);
        dataset::Set(source, "filename", "ml_clean.py", source.GetAllocator());
        dataset::Set(source, "source", std::string{std::istreambuf_iterator<char>(input), {}}, source.GetAllocator());
        *response = dataset::Encode(source);
        return 0;
    }
    int PublishPythonSource(const char* filename, const char* source, const char*, std::string* response,
                            DataSemanticsError*) override {
        ASSERT_EQ(std::string(filename), "ml_clean.py");
        std::filesystem::create_directories(RuntimeOperatorsDir());
        std::ofstream output(RuntimeOperatorPyPath("ml", "clean"));
        output << source;
        ASSERT_TRUE(output.good());
        ++publications;
        *response = R"({"ok":true})";
        return 0;
    }
};

static void TestOperatorCatalog() {
    std::puts("[TEST] T18-T23 operator catalog ...");
    const std::string dir = MakeTempDir("op_catalog");
    const std::string upload = MakeTempDir("op_catalog_upload");

    CatalogPlugin p;
    TestBuiltinRegistry builtin_registry;
    TestQuerier querier;
    querier.builtin_registry = &builtin_registry;
    const std::string opt = MakeCatalogOption(dir);
    ASSERT_EQ(p.Option(opt.c_str()), 0);
    ASSERT_EQ(p.Load(&querier), 0);
    ASSERT_EQ(p.Start(), 0);

    std::vector<OperatorMeta> batch;
    batch.push_back({"ml", "clean", "python", "bridge", "clean op", "data"});
    UpsertResult upsert = p.UpsertBatch(batch);
    ASSERT_EQ(upsert.success_count, 1);
    ASSERT_EQ(upsert.failed_count, 0);
    ASSERT_EQ(p.QueryStatus("ml", "clean"), OperatorStatus::kDeactivated);

    std::unordered_map<std::string, fnRouterHandler> routes;
    p.EnumRoutes([&](const RouteItem& item) { routes[item.method + ":" + item.uri] = item.handler; });
    ASSERT_TRUE(routes.count("POST:/operators/list") == 1);
    ASSERT_TRUE(routes.count("POST:/operators/upload") == 1);
    ASSERT_TRUE(routes.count("POST:/operators/delete") == 1);
    ASSERT_TRUE(routes.count("POST:/operators/detail") == 1);
    ASSERT_TRUE(routes.count("POST:/operators/activate") == 1);
    ASSERT_TRUE(routes.count("POST:/operators/deactivate") == 1);
    ASSERT_TRUE(routes.count("POST:/operators/update") == 1);
    ASSERT_TRUE(routes.count("POST:/operators/upsert_batch") == 1);
    std::string rsp;

    const std::string tmp_py = upload + "/ml_clean.py";
    {
        FILE* fp = fopen(tmp_py.c_str(), "wb");
        ASSERT_TRUE(fp != nullptr);
        const char* code = "def work(df):\n    return df\n";
        fwrite(code, 1, strlen(code), fp);
        fclose(fp);
    }
    const std::string upload_req = "{\"type\":\"python\",\"filename\":\"ml_clean.py\",\"tmp_path\":\"" + tmp_py + "\"}";
    const std::string py_path = RuntimeOperatorPyPath("ml", "clean");
    ASSERT_EQ(routes["POST:/operators/upload"]("/operators/upload", upload_req, rsp), error::UNAVAILABLE);
    ASSERT_TRUE(std::filesystem::exists(tmp_py));
    ASSERT_TRUE(!std::filesystem::exists(py_path));
    ASSERT_EQ(routes["POST:/operators/update"](
                  "/operators/update",
                  R"({"name":"ml.clean","description":"must not persist","content":"replacement"})", rsp),
              error::UNAVAILABLE);
    ASSERT_TRUE(!std::filesystem::exists(py_path));
    ASSERT_EQ(routes["POST:/operators/detail"]("/operators/detail", R"({"name":"ml.clean"})", rsp), error::OK);
    rapidjson::Document unchanged;
    unchanged.Parse(rsp.c_str());
    ASSERT_EQ(std::string(unchanged["description"].GetString()), "clean op");
    ASSERT_TRUE(!unchanged.HasMember("content"));
    TestPythonPublisher publisher;
    querier.publisher = &publisher;
    ASSERT_EQ(routes["POST:/operators/upload"]("/operators/upload", upload_req, rsp), error::OK);
    ASSERT_EQ(publisher.publications, 1);
    ASSERT_TRUE(std::filesystem::exists(tmp_py));
    ASSERT_EQ(routes["POST:/operators/update"]("/operators/update",
                                               R"({"name":"ml.clean","content":"def work(df): return df"})", rsp),
              error::OK);
    ASSERT_EQ(publisher.publications, 2);

    ASSERT_EQ(routes["POST:/operators/activate"]("/operators/activate", R"({"name":"ml.clean"})", rsp), error::OK);
    ASSERT_EQ(p.QueryStatus("ml", "clean"), OperatorStatus::kActive);

    // upsert 不覆盖 active
    std::vector<OperatorMeta> batch2;
    batch2.push_back({"ml", "clean", "python", "bridge", "clean op v2", "storage"});
    upsert = p.UpsertBatch(batch2);
    ASSERT_EQ(upsert.success_count, 1);
    ASSERT_EQ(upsert.failed_count, 0);
    ASSERT_EQ(p.QueryStatus("ml", "clean"), OperatorStatus::kActive);

    ASSERT_EQ(routes["POST:/operators/detail"]("/operators/detail", R"({"name":"ml.clean"})", rsp), error::OK);
    {
        rapidjson::Document d;
        d.Parse(rsp.c_str());
        ASSERT_TRUE(d.IsObject());
        ASSERT_EQ(std::string(d["description"].GetString()), "clean op v2");
        ASSERT_EQ(std::string(d["position"].GetString()), "storage");
        ASSERT_EQ(d["active"].GetInt(), 1);
    }

    ASSERT_EQ(routes["POST:/operators/update"](
                  "/operators/update", R"({"name":"ml.clean","description":"updated desc","position":"data"})", rsp),
              error::OK);
    ASSERT_EQ(routes["POST:/operators/detail"]("/operators/detail", R"({"name":"ml.clean"})", rsp), error::OK);
    {
        rapidjson::Document d;
        d.Parse(rsp.c_str());
        ASSERT_TRUE(d.IsObject());
        ASSERT_EQ(std::string(d["description"].GetString()), "updated desc");
        ASSERT_EQ(std::string(d["position"].GetString()), "data");
    }

    ASSERT_EQ(routes["POST:/operators/deactivate"]("/operators/deactivate", R"({"name":"ml.clean"})", rsp), error::OK);
    ASSERT_EQ(p.QueryStatus("ml", "clean"), OperatorStatus::kDeactivated);

    ASSERT_EQ(routes["POST:/operators/list"]("/operators/list", R"({"type":"python"})", rsp), error::OK);
    {
        rapidjson::Document d;
        d.Parse(rsp.c_str());
        ASSERT_TRUE(d.IsObject() && d.HasMember("operators") && d["operators"].IsArray());
        ASSERT_EQ(d["operators"].Size(), rapidjson::SizeType(1));
    }

    ASSERT_EQ(routes["POST:/operators/upsert_batch"]("/operators/upsert_batch",
                                                     R"({"operators":[{"category":"ml","name":"x"}]})", rsp),
              error::BAD_REQUEST);

    ASSERT_TRUE(std::filesystem::exists(py_path));

    ASSERT_EQ(routes["POST:/operators/activate"]("/operators/activate", R"({"name":"ml.clean"})", rsp), error::OK);
    ASSERT_EQ(routes["POST:/operators/delete"]("/operators/delete", R"({"type":"python","name":"ml.clean"})", rsp),
              error::CONFLICT);
    ASSERT_EQ(routes["POST:/operators/deactivate"]("/operators/deactivate", R"({"name":"ml.clean"})", rsp), error::OK);
    ASSERT_EQ(routes["POST:/operators/delete"]("/operators/delete", R"({"type":"python","name":"ml.clean"})", rsp),
              error::OK);
    ASSERT_EQ(p.QueryStatus("ml", "clean"), OperatorStatus::kNotFound);
    ASSERT_TRUE(!std::filesystem::exists(py_path));
    ASSERT_EQ(routes["POST:/operators/detail"]("/operators/detail", R"({"name":"ml.clean"})", rsp), error::NOT_FOUND);
    ASSERT_EQ(routes["POST:/operators/delete"]("/operators/delete", R"({"type":"python","name":"ml.clean"})", rsp),
              error::NOT_FOUND);
    ASSERT_EQ(routes["POST:/operators/delete"]("/operators/delete",
                                               R"({"type":"builtin","name":"builtin.passthrough"})", rsp),
              error::BAD_REQUEST);

    std::puts("[PASS] T18-T23 operator catalog");
}

static void TestCppOperatorPluginContracts() {
    std::puts("[TEST] T42 C++ operator plugin current contracts ...");
    ASSERT_EQ(::unsetenv("FLOWSQL_FIXTURE_CPP_OPERATOR_CASE_DUPLICATE"), 0);

    static_assert(std::is_standard_layout<CppOperatorDescriptor>::value,
                  "Current descriptor must have a stable field layout");
    static_assert(std::is_trivially_copyable<CppOperatorDescriptor>::value,
                  "Current descriptor must remain a plain ABI value");
    static_assert(std::is_same<decltype(&flowsql_describe_operator), CppOperatorPluginDescribeFn>::value,
                  "Current describe export and host function type must match");
    static_assert(
        std::is_same<decltype(&flowsql_create_operator_capability), CppOperatorPluginCreateCapabilityFn>::value,
        "Current create export and host function type must match");
    static_assert(
        std::is_same<decltype(&flowsql_destroy_operator_capability), CppOperatorPluginDestroyCapabilityFn>::value,
        "Current destroy export and host function type must match");
    ASSERT_EQ(kCppOperatorDescriptorSize, static_cast<uint32_t>(sizeof(CppOperatorDescriptor)));
    ASSERT_TRUE(std::memcmp(&IID_CPP_OPERATOR_PLUGIN_REGISTRY, &IID_OPERATOR, sizeof(Guid)) != 0);
    ASSERT_TRUE(std::memcmp(&IID_CPP_OPERATOR_PLUGIN_REGISTRY, &IID_BLOCK_TRANSFORM_OPERATOR, sizeof(Guid)) != 0);

    TestCppOperatorPluginRegistry registry;
    int stale_capability = 0;
    CppOperatorCapabilityLease lease;
    lease.capability = &stale_capability;
    lease.lifetime = std::make_shared<int>(1);
    ASSERT_EQ(registry.Acquire("fixture", "missing", IID_OPERATOR, &lease), -1);
    ASSERT_TRUE(lease.capability == nullptr);
    ASSERT_TRUE(!lease.lifetime);
    ASSERT_EQ(registry.Acquire("fixture", "echo", IID_OPERATOR, nullptr), -1);
    ASSERT_EQ(registry.Acquire(nullptr, "echo", IID_OPERATOR, &lease), -1);
    ASSERT_EQ(registry.Acquire("fixture", nullptr, IID_OPERATOR, &lease), -1);
    ASSERT_EQ(registry.Acquire("other", "echo", IID_OPERATOR, &lease), -1);
    ASSERT_EQ(registry.Acquire("fixture", "echo", IID_OPERATOR, &lease), 0);
    ASSERT_TRUE(lease.capability != nullptr);
    ASSERT_EQ(*static_cast<int*>(lease.capability), 42);
    ASSERT_TRUE(lease.lifetime != nullptr);
    ASSERT_TRUE(!registry.LastLeaseExpired());
    auto retained_lease = lease;
    ASSERT_EQ(registry.Acquire("fixture", "echo", IID_BLOCK_TRANSFORM_OPERATOR, &lease), -1);
    ASSERT_TRUE(lease.capability == nullptr);
    ASSERT_TRUE(!lease.lifetime);
    ASSERT_TRUE(!registry.LastLeaseExpired());
    retained_lease = {};
    ASSERT_TRUE(registry.LastLeaseExpired());

    const std::string capabilities_path = FIXTURE_CPP_CAPABILITIES_SO_PATH;
    ASSERT_TRUE(!capabilities_path.empty());
    flowsql::test::ScopedSharedLibrary capabilities_library(capabilities_path.c_str());
    ASSERT_TRUE(capabilities_library);
    auto* capabilities_abi = reinterpret_cast<CppOperatorPluginAbiVersionFn>(
        capabilities_library.Symbol(kCppOperatorPluginAbiVersionSymbol));
    auto* capabilities_count =
        reinterpret_cast<CppOperatorPluginCountFn>(capabilities_library.Symbol(kCppOperatorPluginCountSymbol));
    auto* capabilities_describe =
        reinterpret_cast<CppOperatorPluginDescribeFn>(capabilities_library.Symbol(kCppOperatorPluginDescribeSymbol));
    auto* capabilities_create = reinterpret_cast<CppOperatorPluginCreateCapabilityFn>(
        capabilities_library.Symbol(kCppOperatorPluginCreateCapabilitySymbol));
    auto* capabilities_destroy = reinterpret_cast<CppOperatorPluginDestroyCapabilityFn>(
        capabilities_library.Symbol(kCppOperatorPluginDestroyCapabilitySymbol));
    ASSERT_TRUE(capabilities_abi != nullptr);
    ASSERT_TRUE(capabilities_count != nullptr);
    ASSERT_TRUE(capabilities_describe != nullptr);
    ASSERT_TRUE(capabilities_create != nullptr);
    ASSERT_TRUE(capabilities_destroy != nullptr);
    ASSERT_EQ(capabilities_abi(), kCppOperatorPluginAbiVersion);
    ASSERT_EQ(capabilities_count(), 3);
    ASSERT_TRUE(capabilities_library.Symbol("flowsql_create_operator") == nullptr);
    ASSERT_TRUE(capabilities_library.Symbol("flowsql_stream_operator_count") == nullptr);
    ASSERT_TRUE(capabilities_library.Symbol("flowsql_block_transform_operator_count") == nullptr);

    CppOperatorDescriptor classic{};
    classic.struct_size = kCppOperatorDescriptorSize;
    ASSERT_EQ(capabilities_describe(0, &classic), 0);
    ASSERT_EQ(std::string(classic.category), std::string("fixture"));
    ASSERT_EQ(std::string(classic.name), std::string("echo"));
    ASSERT_EQ(std::string(classic.description), std::string("Current fixture classic operator"));
    ASSERT_TRUE(std::memcmp(&classic.contract_iid, &IID_OPERATOR, sizeof(Guid)) == 0);

    CppOperatorDescriptor transform{};
    transform.struct_size = kCppOperatorDescriptorSize;
    ASSERT_EQ(capabilities_describe(1, &transform), 0);
    ASSERT_EQ(std::string(transform.category), std::string("fixture"));
    ASSERT_EQ(std::string(transform.name), std::string("transform"));
    ASSERT_EQ(std::string(transform.description), std::string("Current fixture block transform operator"));
    ASSERT_TRUE(std::memcmp(&transform.contract_iid, &IID_BLOCK_TRANSFORM_OPERATOR, sizeof(Guid)) == 0);

    CppOperatorDescriptor time_transform{};
    time_transform.struct_size = kCppOperatorDescriptorSize;
    ASSERT_EQ(capabilities_describe(2, &time_transform), 0);
    ASSERT_EQ(std::string(time_transform.category), std::string("fixture"));
    ASSERT_EQ(std::string(time_transform.name), std::string("time_transform"));
    ASSERT_EQ(std::string(time_transform.description),
              std::string("Current fixture time-driven block transform operator"));
    ASSERT_TRUE(std::memcmp(&time_transform.contract_iid, &IID_BLOCK_TRANSFORM_OPERATOR, sizeof(Guid)) == 0);

    CppOperatorDescriptor undersized{};
    undersized.struct_size = kCppOperatorDescriptorSize - 1;
    ASSERT_EQ(capabilities_describe(0, &undersized), -1);
    ASSERT_TRUE(undersized.category == nullptr);
    ASSERT_EQ(capabilities_describe(0, nullptr), -1);
    transform = {};
    transform.struct_size = kCppOperatorDescriptorSize;
    ASSERT_EQ(capabilities_describe(-1, &transform), -1);
    ASSERT_EQ(capabilities_describe(capabilities_count(), &transform), -1);

    TestQuerier querier;
    ASSERT_TRUE(capabilities_create(0, nullptr) == nullptr);
    ASSERT_TRUE(capabilities_create(-1, &querier) == nullptr);
    ASSERT_TRUE(capabilities_create(capabilities_count(), &querier) == nullptr);
    void* classic_capability = capabilities_create(0, &querier);
    ASSERT_TRUE(classic_capability != nullptr);
    auto* classic_operator = static_cast<IOperator*>(classic_capability);
    ASSERT_EQ(classic_operator->Category(), std::string("fixture"));
    ASSERT_EQ(classic_operator->Name(), std::string("echo"));
    ASSERT_EQ(classic_operator->Work(nullptr, nullptr), 0);
    capabilities_destroy(0, classic_capability);

    void* transform_capability = capabilities_create(1, &querier);
    ASSERT_TRUE(transform_capability != nullptr);
    auto* transform_operator = static_cast<IBlockTransformOperator*>(transform_capability);
    ASSERT_EQ(transform_operator->Category(), std::string("fixture"));
    ASSERT_EQ(transform_operator->Name(), std::string("transform"));
    BlockTransformTaskConfig config;
    IBlockTransformTask* task = nullptr;
    ASSERT_EQ(transform_operator->CreateTask(config, &task), 0);
    ASSERT_TRUE(task != nullptr);
    transform_operator->ReleaseTask(task);
    capabilities_destroy(1, transform_capability);

    void* time_transform_capability = capabilities_create(2, &querier);
    ASSERT_TRUE(time_transform_capability != nullptr);
    auto* time_transform_operator = static_cast<IBlockTransformOperator*>(time_transform_capability);
    ASSERT_EQ(time_transform_operator->Category(), std::string("fixture"));
    ASSERT_EQ(time_transform_operator->Name(), std::string("time_transform"));
    BlockTransformTaskConfig time_config{};
    time_config.struct_size = kBlockTransformTaskConfigSize;
    time_config.contract_version = kBlockTransformContractVersion;
    time_config.task_id = "time-task";
    time_config.with_params_json = "{}";
    time_config.pushed_filter_plan_json = "{}";
    IBlockTransformTask* time_task = nullptr;
    ASSERT_EQ(time_transform_operator->CreateTask(time_config, &time_task), 0);
    ASSERT_TRUE(time_task != nullptr);
    time_transform_operator->ReleaseTask(time_task);
    capabilities_destroy(2, time_transform_capability);
    capabilities_destroy(0, nullptr);

    std::puts("[PASS] T42 C++ operator plugin current contracts");
}

static void TestCppPluginLifecycle() {
    std::puts("[TEST] T42-T48 cpp plugin lifecycle ...");
    const std::string fixture_so = FIXTURE_CPP_SO_PATH;
    const std::string fixture_capabilities_so = FIXTURE_CPP_CAPABILITIES_SO_PATH;
    const std::string bad_abi_so = FIXTURE_BAD_ABI_SO_PATH;
    ASSERT_TRUE(!fixture_so.empty());
    ASSERT_TRUE(!fixture_capabilities_so.empty());
    ASSERT_TRUE(!bad_abi_so.empty());
    ASSERT_TRUE(std::filesystem::exists(fixture_so));
    ASSERT_TRUE(std::filesystem::exists(fixture_capabilities_so));
    ASSERT_TRUE(std::filesystem::exists(bad_abi_so));

    const std::string dir = MakeTempDir("cpp_plugin");
    const std::string upload = MakeTempDir("cpp_upload");

    CatalogPlugin p;
    BinAddonHostPlugin binaddon;
    TestQuerier querier;
    querier.op_registry = static_cast<IOperatorRegistry*>(&p);
    querier.binaddon_host = static_cast<IBinAddonHost*>(&binaddon);
    querier.cpp_operator_plugin_registry = static_cast<ICppOperatorPluginRegistry*>(&binaddon);
    TestBuiltinRegistry builtin_registry;
    querier.builtin_registry = &builtin_registry;

    const std::string opt = MakeCatalogOption(dir);
    const std::string db_path = (std::filesystem::path(dir) / "catalog" / "operator_catalog.db").string();
    const std::string binaddon_opt =
        "operator_db_path=" + db_path + ";upload_dir=" + (std::filesystem::path(upload) / "binaddon").string();
    ASSERT_EQ(p.Option(opt.c_str()), 0);
    ASSERT_EQ(binaddon.Option(binaddon_opt.c_str()), 0);
    ASSERT_EQ(p.Load(&querier), 0);
    ASSERT_EQ(binaddon.Load(&querier), 0);
    ASSERT_EQ(p.Start(), 0);
    ASSERT_EQ(binaddon.Start(), 0);

    std::unordered_map<std::string, fnRouterHandler> routes;
    p.EnumRoutes([&](const RouteItem& item) { routes[item.method + ":" + item.uri] = item.handler; });
    ASSERT_TRUE(routes.count("POST:/operators/upload") == 1);
    ASSERT_TRUE(routes.count("POST:/operators/list") == 1);
    ASSERT_TRUE(routes.count("POST:/operators/activate") == 1);
    ASSERT_TRUE(routes.count("POST:/operators/detail") == 1);
    ASSERT_TRUE(routes.count("POST:/operators/deactivate") == 1);
    ASSERT_TRUE(routes.count("POST:/operators/delete") == 1);

    const std::string good_filename = "fixture_cpp_operator.so";
    const std::string good_tmp = CopyToTmp(fixture_so, upload, good_filename);
    ASSERT_TRUE(!good_tmp.empty());

    std::string rsp;
    std::string req = "{\"type\":\"cpp\",\"filename\":\"" + good_filename + "\",\"tmp_path\":\"" + good_tmp + "\"}";
    ASSERT_EQ(routes["POST:/operators/upload"]("/operators/upload", req, rsp), error::OK);

    std::string plugin_id;
    {
        rapidjson::Document d;
        d.Parse(rsp.c_str());
        ASSERT_TRUE(d.IsObject());
        ASSERT_TRUE(d.HasMember("plugin_id") && d["plugin_id"].IsString());
        ASSERT_EQ(std::string(d["status"].GetString()), "uploaded");
        plugin_id = d["plugin_id"].GetString();
        ASSERT_EQ(plugin_id.size(), size_t(64));
    }

    ASSERT_EQ(routes["POST:/operators/list"]("/operators/list", R"({"type":"cpp"})", rsp), error::OK);
    {
        rapidjson::Document d;
        d.Parse(rsp.c_str());
        ASSERT_TRUE(d.IsObject() && d["operators"].IsArray());
        ASSERT_EQ(d["operators"].Size(), rapidjson::SizeType(1));
        const auto& row = d["operators"][0];
        ASSERT_EQ(std::string(row["plugin_id"].GetString()), plugin_id);
        ASSERT_EQ(std::string(row["plugin"]["status"].GetString()), "uploaded");
        ASSERT_TRUE(row["plugin"]["abi_version"].IsNull());
        ASSERT_TRUE(row["plugin"]["operator_count"].IsNull());
        ASSERT_TRUE(row["plugin"]["operators"].IsNull());
    }

    req = "{\"type\":\"cpp\",\"plugin_id\":\"" + plugin_id + "\"}";
    ASSERT_EQ(routes["POST:/operators/activate"]("/operators/activate", req, rsp), error::OK);
    ASSERT_EQ(p.QueryStatus("cppdemo", "echo"), OperatorStatus::kActive);

    CppOperatorCapabilityLease classic_lease;
    ASSERT_EQ(binaddon.Acquire("cppdemo", "echo", IID_OPERATOR, &classic_lease), 0);
    auto* op = static_cast<IOperator*>(classic_lease.capability);
    ASSERT_TRUE(op != nullptr);
    ASSERT_EQ(op->Category(), std::string("cppdemo"));
    ASSERT_EQ(op->Name(), std::string("echo"));
    ASSERT_EQ(routes["POST:/operators/deactivate"]("/operators/deactivate", req, rsp), error::CONFLICT);
    ASSERT_TRUE(rsp.find("plugin is in use") != std::string::npos);
    CppOperatorCapabilityLease after_conflict_lease;
    ASSERT_EQ(binaddon.Acquire("cppdemo", "echo", IID_OPERATOR, &after_conflict_lease), 0);
    ASSERT_TRUE(after_conflict_lease.capability != nullptr);
    after_conflict_lease = {};
    classic_lease = {};
    ASSERT_EQ(p.Register("fixture.reentrant", [&p]() { return p.Create("cppdemo.echo"); }), 0);
    std::unique_ptr<IOperator> first(p.Create("fixture.reentrant"));
    std::unique_ptr<IOperator> second(p.Create("cppdemo.echo"));
    ASSERT_TRUE(first && second && first.get() != second.get());
    ASSERT_EQ(first->Category(), std::string("cppdemo"));
    ASSERT_EQ(second->Name(), std::string("echo"));
    ASSERT_EQ(first->Work(nullptr, nullptr), 0);
    ASSERT_EQ(routes["POST:/operators/deactivate"]("/operators/deactivate", req, rsp), error::CONFLICT);
    first.reset();
    ASSERT_EQ(routes["POST:/operators/deactivate"]("/operators/deactivate", req, rsp), error::CONFLICT);
    second.reset();
    ASSERT_EQ(p.RemoveFactory("fixture.reentrant"), 0);

    ASSERT_EQ(routes["POST:/operators/list"]("/operators/list", R"({"type":"cpp"})", rsp), error::OK);
    {
        rapidjson::Document d;
        d.Parse(rsp.c_str());
        ASSERT_TRUE(d.IsObject() && d["operators"].IsArray());
        const auto& row = d["operators"][0];
        ASSERT_EQ(std::string(row["plugin"]["status"].GetString()), "activated");
        ASSERT_EQ(row["plugin"]["operator_count"].GetInt(), 1);
        ASSERT_TRUE(row["plugin"]["operators"].IsArray());
        ASSERT_EQ(std::string(row["plugin"]["operators"][0].GetString()), "cppdemo.echo");
    }

    ASSERT_EQ(routes["POST:/operators/detail"]("/operators/detail", req, rsp), error::OK);
    {
        rapidjson::Document d;
        d.Parse(rsp.c_str());
        ASSERT_TRUE(d.IsObject());
        ASSERT_EQ(std::string(d["type"].GetString()), "cpp");
        ASSERT_EQ(std::string(d["plugin_id"].GetString()), plugin_id);
        ASSERT_TRUE(d["plugin"].IsObject());
        ASSERT_TRUE(!d["plugin"].HasMember("path"));
    }

    ASSERT_EQ(routes["POST:/operators/deactivate"]("/operators/deactivate", req, rsp), error::OK);
    ASSERT_EQ(p.QueryStatus("cppdemo", "echo"), OperatorStatus::kDeactivated);
    ASSERT_TRUE(p.Create("cppdemo.echo") == nullptr);

    ASSERT_EQ(routes["POST:/operators/delete"]("/operators/delete", req, rsp), error::OK);
    ASSERT_EQ(p.QueryStatus("cppdemo", "echo"), OperatorStatus::kNotFound);
    ASSERT_EQ(routes["POST:/operators/list"]("/operators/list", R"({"type":"cpp"})", rsp), error::OK);
    {
        rapidjson::Document d;
        d.Parse(rsp.c_str());
        ASSERT_TRUE(d.IsObject() && d["operators"].IsArray());
        ASSERT_EQ(d["operators"].Size(), rapidjson::SizeType(0));
    }

    auto* capability_registry = static_cast<ICppOperatorPluginRegistry*>(&binaddon);
    ASSERT_TRUE(capability_registry != nullptr);

    const std::string capabilities_filename = "fixture_cpp_operator_capabilities.so";
    std::string capabilities_tmp = CopyToTmp(fixture_capabilities_so, upload, capabilities_filename);
    ASSERT_TRUE(!capabilities_tmp.empty());
    req = "{\"type\":\"cpp\",\"filename\":\"" + capabilities_filename + "\",\"tmp_path\":\"" + capabilities_tmp + "\"}";
    ASSERT_EQ(routes["POST:/operators/upload"]("/operators/upload", req, rsp), error::OK);
    std::string capabilities_plugin_id;
    {
        rapidjson::Document d;
        d.Parse(rsp.c_str());
        ASSERT_TRUE(d.IsObject() && d.HasMember("plugin_id"));
        capabilities_plugin_id = d["plugin_id"].GetString();
    }
    std::string capabilities_req = "{\"type\":\"cpp\",\"plugin_id\":\"" + capabilities_plugin_id + "\"}";
    ASSERT_EQ(::setenv("FLOWSQL_FIXTURE_CPP_OPERATOR_CASE_DUPLICATE", "1", 1), 0);
    ASSERT_EQ(routes["POST:/operators/activate"]("/operators/activate", capabilities_req, rsp), error::CONFLICT);
    ASSERT_EQ(::unsetenv("FLOWSQL_FIXTURE_CPP_OPERATOR_CASE_DUPLICATE"), 0);
    ASSERT_EQ(p.QueryStatus("fixture", "echo"), OperatorStatus::kNotFound);
    ASSERT_EQ(p.QueryStatus("fixture", "transform"), OperatorStatus::kNotFound);
    CppOperatorCapabilityLease missing_lease;
    ASSERT_EQ(capability_registry->Acquire("fixture", "echo", IID_OPERATOR, &missing_lease), -1);
    ASSERT_TRUE(missing_lease.capability == nullptr);
    ASSERT_TRUE(!missing_lease.lifetime);
    ASSERT_EQ(routes["POST:/operators/detail"]("/operators/detail", capabilities_req, rsp), error::OK);
    {
        rapidjson::Document d;
        d.Parse(rsp.c_str());
        ASSERT_EQ(std::string(d["plugin"]["status"].GetString()), "broken");
        ASSERT_EQ(std::string(d["plugin"]["last_error"].GetString()), "duplicate operators inside plugin");
    }
    ASSERT_EQ(routes["POST:/operators/delete"]("/operators/delete", capabilities_req, rsp), error::OK);

    capabilities_tmp = CopyToTmp(fixture_capabilities_so, upload, capabilities_filename);
    ASSERT_TRUE(!capabilities_tmp.empty());
    req = "{\"type\":\"cpp\",\"filename\":\"" + capabilities_filename + "\",\"tmp_path\":\"" + capabilities_tmp + "\"}";
    ASSERT_EQ(routes["POST:/operators/upload"]("/operators/upload", req, rsp), error::OK);
    ASSERT_EQ(::setenv("FLOWSQL_FIXTURE_CPP_OPERATOR_DUPLICATE_TRANSFORM", "1", 1), 0);
    ASSERT_EQ(routes["POST:/operators/activate"]("/operators/activate", capabilities_req, rsp), error::CONFLICT);
    ASSERT_EQ(p.QueryStatus("fixture", "transform"), OperatorStatus::kNotFound);
    CppOperatorCapabilityLease duplicate_lease;
    ASSERT_EQ(capability_registry->Acquire("fixture", "transform", IID_BLOCK_TRANSFORM_OPERATOR, &duplicate_lease), -1);
    ASSERT_TRUE(!duplicate_lease.capability && !duplicate_lease.lifetime);
    ASSERT_EQ(routes["POST:/operators/delete"]("/operators/delete", capabilities_req, rsp), error::OK);
    ASSERT_EQ(::unsetenv("FLOWSQL_FIXTURE_CPP_OPERATOR_DUPLICATE_TRANSFORM"), 0);

    ASSERT_EQ(p.Register("fixture.transform", []() -> IOperator* { return new PassthroughOperator(); }), 0);
    capabilities_tmp = CopyToTmp(fixture_capabilities_so, upload, capabilities_filename);
    ASSERT_TRUE(!capabilities_tmp.empty());
    req = "{\"type\":\"cpp\",\"filename\":\"" + capabilities_filename + "\",\"tmp_path\":\"" + capabilities_tmp + "\"}";
    ASSERT_EQ(routes["POST:/operators/upload"]("/operators/upload", req, rsp), error::OK);
    ASSERT_EQ(routes["POST:/operators/activate"]("/operators/activate", capabilities_req, rsp), error::CONFLICT);
    ASSERT_EQ(p.QueryStatus("fixture", "echo"), OperatorStatus::kNotFound);
    ASSERT_EQ(p.QueryStatus("fixture", "transform"), OperatorStatus::kNotFound);
    ASSERT_EQ(capability_registry->Acquire("fixture", "echo", IID_OPERATOR, &missing_lease), -1);
    ASSERT_TRUE(missing_lease.capability == nullptr);
    ASSERT_TRUE(!missing_lease.lifetime);
    ASSERT_EQ(routes["POST:/operators/detail"]("/operators/detail", capabilities_req, rsp), error::OK);
    {
        rapidjson::Document d;
        d.Parse(rsp.c_str());
        ASSERT_EQ(std::string(d["plugin"]["status"].GetString()), "broken");
        ASSERT_EQ(std::string(d["plugin"]["last_error"].GetString()), "operator factory conflict");
    }
    ASSERT_EQ(routes["POST:/operators/delete"]("/operators/delete", capabilities_req, rsp), error::OK);
    ASSERT_EQ(p.RemoveFactory("fixture.transform"), 0);

    capabilities_tmp = CopyToTmp(fixture_capabilities_so, upload, capabilities_filename);
    ASSERT_TRUE(!capabilities_tmp.empty());
    req = "{\"type\":\"cpp\",\"filename\":\"" + capabilities_filename + "\",\"tmp_path\":\"" + capabilities_tmp + "\"}";
    ASSERT_EQ(routes["POST:/operators/upload"]("/operators/upload", req, rsp), error::OK);
    {
        rapidjson::Document d;
        d.Parse(rsp.c_str());
        ASSERT_EQ(std::string(d["plugin_id"].GetString()), capabilities_plugin_id);
    }
    ASSERT_EQ(routes["POST:/operators/activate"]("/operators/activate", capabilities_req, rsp), error::OK);
    {
        rapidjson::Document d;
        d.Parse(rsp.c_str());
        ASSERT_TRUE(d.IsObject());
        ASSERT_EQ(d["abi_version"].GetInt(), kCppOperatorPluginAbiVersion);
        ASSERT_EQ(d["operator_count"].GetInt(), 3);
        ASSERT_TRUE(d["operators"].IsArray());
        ASSERT_EQ(d["operators"].Size(), rapidjson::SizeType(3));
        ASSERT_EQ(std::string(d["operators"][0].GetString()), "fixture.echo");
        ASSERT_EQ(std::string(d["operators"][1].GetString()), "fixture.transform");
        ASSERT_EQ(std::string(d["operators"][2].GetString()), "fixture.time_transform");
    }
    ASSERT_EQ(p.QueryStatus("fixture", "echo"), OperatorStatus::kActive);
    ASSERT_EQ(p.QueryStatus("fixture", "transform"), OperatorStatus::kActive);
    ASSERT_EQ(p.QueryStatus("fixture", "time_transform"), OperatorStatus::kActive);
    ASSERT_EQ(routes["POST:/operators/detail"]("/operators/detail", capabilities_req, rsp), error::OK);
    {
        rapidjson::Document d;
        d.Parse(rsp.c_str());
        ASSERT_TRUE(d.IsObject() && d["plugin"].IsObject());
        const auto& details = d["plugin"]["operator_details"];
        ASSERT_TRUE(details.IsArray());
        ASSERT_EQ(details.Size(), rapidjson::SizeType(3));
        ASSERT_EQ(std::string(details[0]["name"].GetString()), "fixture.echo");
        ASSERT_EQ(std::string(details[0]["contract"].GetString()), "operator");
        ASSERT_EQ(std::string(details[1]["name"].GetString()), "fixture.transform");
        ASSERT_EQ(std::string(details[1]["contract"].GetString()), "block_transform");
        ASSERT_EQ(std::string(details[2]["name"].GetString()), "fixture.time_transform");
        ASSERT_EQ(std::string(details[2]["contract"].GetString()), "block_transform");
    }

    CppOperatorCapabilityLease echo_lease;
    CppOperatorCapabilityLease transform_lease;
    CppOperatorCapabilityLease time_transform_lease;
    ASSERT_EQ(capability_registry->Acquire("fixture", "echo", IID_OPERATOR, &echo_lease), 0);
    ASSERT_EQ(capability_registry->Acquire("fixture", "transform", IID_BLOCK_TRANSFORM_OPERATOR, &transform_lease), 0);
    ASSERT_EQ(
        capability_registry->Acquire("fixture", "time_transform", IID_BLOCK_TRANSFORM_OPERATOR, &time_transform_lease),
        0);
    ASSERT_TRUE(echo_lease.capability != nullptr && echo_lease.lifetime != nullptr);
    ASSERT_TRUE(transform_lease.capability != nullptr && transform_lease.lifetime != nullptr);
    ASSERT_TRUE(time_transform_lease.capability != nullptr && time_transform_lease.lifetime != nullptr);
    auto* echo = static_cast<IOperator*>(echo_lease.capability);
    auto* transform = static_cast<IBlockTransformOperator*>(transform_lease.capability);
    auto* time_transform = static_cast<IBlockTransformOperator*>(time_transform_lease.capability);
    ASSERT_EQ(echo->Category(), std::string("fixture"));
    ASSERT_EQ(echo->Name(), std::string("echo"));
    ASSERT_EQ(transform->Category(), std::string("fixture"));
    ASSERT_EQ(transform->Name(), std::string("transform"));
    ASSERT_EQ(time_transform->Category(), std::string("fixture"));
    ASSERT_EQ(time_transform->Name(), std::string("time_transform"));

    CppOperatorCapabilityLease wrong_contract = time_transform_lease;
    ASSERT_EQ(capability_registry->Acquire("fixture", "time_transform", IID_OPERATOR, &wrong_contract), -1);
    ASSERT_TRUE(wrong_contract.capability == nullptr);
    ASSERT_TRUE(!wrong_contract.lifetime);
    echo_lease = {};
    transform_lease = {};
    ASSERT_EQ(routes["POST:/operators/deactivate"]("/operators/deactivate", capabilities_req, rsp), error::CONFLICT);
    ASSERT_TRUE(rsp.find("plugin is in use") != std::string::npos);
    CppOperatorCapabilityLease after_conflict;
    ASSERT_EQ(capability_registry->Acquire("fixture", "echo", IID_OPERATOR, &after_conflict), 0);
    after_conflict = {};
    time_transform_lease = {};

    const int queries_before_deactivate = querier.cpp_operator_plugin_registry_queries.load(std::memory_order_relaxed);
    ASSERT_EQ(routes["POST:/operators/deactivate"]("/operators/deactivate", capabilities_req, rsp), error::OK);
    ASSERT_TRUE(querier.cpp_operator_plugin_registry_queries.load(std::memory_order_relaxed) >
                queries_before_deactivate);
    ASSERT_EQ(p.QueryStatus("fixture", "echo"), OperatorStatus::kDeactivated);
    ASSERT_EQ(p.QueryStatus("fixture", "transform"), OperatorStatus::kDeactivated);
    ASSERT_EQ(p.QueryStatus("fixture", "time_transform"), OperatorStatus::kDeactivated);
    ASSERT_EQ(capability_registry->Acquire("fixture", "echo", IID_OPERATOR, &missing_lease), -1);

    ASSERT_EQ(routes["POST:/operators/activate"]("/operators/activate", capabilities_req, rsp), error::OK);
    ASSERT_EQ(capability_registry->Acquire("fixture", "transform", IID_BLOCK_TRANSFORM_OPERATOR, &transform_lease), 0);
    ASSERT_EQ(
        capability_registry->Acquire("fixture", "time_transform", IID_BLOCK_TRANSFORM_OPERATOR, &time_transform_lease),
        0);
    transform_lease = {};
    time_transform_lease = {};
    const int queries_before_stop = querier.cpp_operator_plugin_registry_queries.load(std::memory_order_relaxed);
    ASSERT_EQ(binaddon.Stop(), 0);
    ASSERT_TRUE(querier.cpp_operator_plugin_registry_queries.load(std::memory_order_relaxed) > queries_before_stop);
    ASSERT_EQ(capability_registry->Acquire("fixture", "echo", IID_OPERATOR, &missing_lease), -1);
    ASSERT_EQ(binaddon.Start(), 0);
    ASSERT_EQ(capability_registry->Acquire("fixture", "echo", IID_OPERATOR, &echo_lease), 0);
    echo_lease = {};
    ASSERT_EQ(routes["POST:/operators/deactivate"]("/operators/deactivate", capabilities_req, rsp), error::OK);
    ASSERT_EQ(routes["POST:/operators/delete"]("/operators/delete", capabilities_req, rsp), error::OK);
    ASSERT_EQ(p.QueryStatus("fixture", "echo"), OperatorStatus::kNotFound);
    ASSERT_EQ(p.QueryStatus("fixture", "transform"), OperatorStatus::kNotFound);
    ASSERT_EQ(p.QueryStatus("fixture", "time_transform"), OperatorStatus::kNotFound);

    const std::string bad_filename = "fixture_bad_abi.so";
    const std::string bad_tmp = CopyToTmp(bad_abi_so, upload, bad_filename);
    ASSERT_TRUE(!bad_tmp.empty());
    req = "{\"type\":\"cpp\",\"filename\":\"" + bad_filename + "\",\"tmp_path\":\"" + bad_tmp + "\"}";
    ASSERT_EQ(routes["POST:/operators/upload"]("/operators/upload", req, rsp), error::OK);
    std::string bad_plugin_id;
    {
        rapidjson::Document d;
        d.Parse(rsp.c_str());
        bad_plugin_id = d["plugin_id"].GetString();
    }
    req = "{\"type\":\"cpp\",\"plugin_id\":\"" + bad_plugin_id + "\"}";
    ASSERT_EQ(routes["POST:/operators/activate"]("/operators/activate", req, rsp), error::BAD_REQUEST);
    ASSERT_EQ(routes["POST:/operators/list"]("/operators/list", R"({"type":"cpp"})", rsp), error::OK);
    {
        rapidjson::Document d;
        d.Parse(rsp.c_str());
        ASSERT_TRUE(d.IsObject() && d["operators"].IsArray());
        ASSERT_EQ(d["operators"].Size(), rapidjson::SizeType(1));
        ASSERT_EQ(std::string(d["operators"][0]["plugin"]["status"].GetString()), "broken");
        ASSERT_EQ(std::string(d["operators"][0]["plugin"]["last_error"].GetString()), "abi version mismatch");
        ASSERT_EQ(d["operators"][0]["plugin"]["abi_version"].GetInt(), 1);
    }
    ASSERT_EQ(routes["POST:/operators/delete"]("/operators/delete", req, rsp), error::OK);
    ASSERT_EQ(binaddon.Stop(), 0);
    ASSERT_EQ(p.Stop(), 0);
    std::puts("[PASS] T42-T48 cpp plugin lifecycle");
}

static void TestOperatorDbPathOption() {
    std::puts("[TEST] T41 operator_db_path option ...");
    const std::string dir = MakeTempDir("op_db_path");
    const std::string db_path = dir + "/meta/shared.db";

    CatalogPlugin p;
    TestBuiltinRegistry builtin_registry;
    TestQuerier querier;
    querier.builtin_registry = &builtin_registry;
    const std::string opt = MakeCatalogOptionWithDbPath(dir, db_path);
    ASSERT_EQ(p.Option(opt.c_str()), 0);
    ASSERT_EQ(p.Load(&querier), 0);
    ASSERT_EQ(p.Start(), 0);

    std::vector<OperatorMeta> batch;
    batch.push_back({"ml", "clean", "python", "bridge", "clean op", "data"});
    UpsertResult upsert = p.UpsertBatch(batch);
    ASSERT_EQ(upsert.success_count, 1);
    ASSERT_EQ(upsert.failed_count, 0);
    ASSERT_TRUE(std::filesystem::exists(db_path));
    ASSERT_TRUE(!std::filesystem::exists(std::filesystem::path(dir) / "catalog" / "operator_catalog.db"));
    ASSERT_EQ(p.Stop(), 0);
    std::puts("[PASS] T41 operator_db_path option");
}

static void TestHttpRoutes() {
    std::puts("[TEST] T24-T35 http routes ...");
    const std::string dir = MakeTempDir("http");
    const std::string upload = MakeTempDir("http_upload");

    CatalogPlugin p;
    TestBuiltinRegistry builtin_registry;
    TestQuerier querier;
    querier.builtin_registry = &builtin_registry;
    const std::string opt = MakeCatalogOption(dir);
    ASSERT_EQ(p.Option(opt.c_str()), 0);
    ASSERT_EQ(p.Load(&querier), 0);
    ASSERT_EQ(p.Start(), 0);

    std::unordered_map<std::string, fnRouterHandler> routes;
    p.EnumRoutes([&](const RouteItem& item) { routes[item.method + ":" + item.uri] = item.handler; });
    ASSERT_TRUE(routes.count("GET:/channels/dataframe") == 1);
    ASSERT_TRUE(routes.count("POST:/channels/dataframe/import") == 1);
    ASSERT_TRUE(routes.count("POST:/channels/dataframe/preview") == 1);
    ASSERT_TRUE(routes.count("POST:/channels/dataframe/rename") == 1);
    ASSERT_TRUE(routes.count("POST:/channels/dataframe/delete") == 1);

    std::string rsp;

    // T25: 无通道时列表为空
    ASSERT_EQ(routes["GET:/channels/dataframe"]("/channels/dataframe", "", rsp), error::OK);
    {
        rapidjson::Document d;
        d.Parse(rsp.c_str());
        ASSERT_TRUE(!d.HasParseError() && d.IsObject());
        ASSERT_TRUE(d.HasMember("channels") && d["channels"].IsArray());
        ASSERT_EQ(d["channels"].Size(), rapidjson::SizeType(0));
    }

    ASSERT_EQ(routes["GET:/channels/dataframe"]("/channels/dataframe", "", rsp), error::OK);
    ASSERT_TRUE(rsp.find("\"channels\"") != std::string::npos);

    // import
    const std::string tmp_csv = upload + "/users.csv";
    {
        FILE* fp = fopen(tmp_csv.c_str(), "wb");
        ASSERT_TRUE(fp != nullptr);
        const char* csv = "id,name,score\n1,Alice,1.5\n2,Bob,2.0\n";
        fwrite(csv, 1, strlen(csv), fp);
        fclose(fp);
    }
    std::string req = "{\"filename\":\"users.csv\",\"tmp_path\":\"" + tmp_csv + "\"}";
    ASSERT_EQ(routes["POST:/channels/dataframe/import"]("/channels/dataframe/import", req, rsp), error::OK);
    ASSERT_TRUE(rsp.find("\"name\":\"users\"") != std::string::npos);

    // T27: 冲突时自动追加时间戳后缀
    const std::string tmp_csv_2 = upload + "/users_dup.csv";
    {
        FILE* fp = fopen(tmp_csv_2.c_str(), "wb");
        ASSERT_TRUE(fp != nullptr);
        const char* csv = "id,name,score\n3,Coco,3.5\n";
        fwrite(csv, 1, strlen(csv), fp);
        fclose(fp);
    }
    req = "{\"filename\":\"users.csv\",\"tmp_path\":\"" + tmp_csv_2 + "\"}";
    ASSERT_EQ(routes["POST:/channels/dataframe/import"]("/channels/dataframe/import", req, rsp), error::OK);
    ASSERT_TRUE(rsp.find("\"name\":\"users_") != std::string::npos);

    // preview
    ASSERT_EQ(routes["POST:/channels/dataframe/preview"]("/channels/dataframe/preview", R"({"name":"users"})", rsp),
              error::OK);
    ASSERT_TRUE(rsp.find("\"columns\"") != std::string::npos);
    ASSERT_TRUE(rsp.find("\"rows\":2") != std::string::npos);
    {
        rapidjson::Document ordinary_page;
        ordinary_page.Parse(rsp.c_str());
        ASSERT_TRUE(!ordinary_page.HasParseError() && ordinary_page.IsObject());
        ASSERT_TRUE(!ordinary_page.HasMember("entity"));
        ASSERT_EQ(ordinary_page["page"].GetUint(), unsigned(1));
        ASSERT_EQ(ordinary_page["page_size"].GetUint(), unsigned(100));
    }

    // npm-packet-dataframe-view T2: packet Arrow values and bounded raw_data hex.
    {
        arrow::UInt32Builder v4;
        arrow::FixedSizeBinaryBuilder v6(arrow::fixed_size_binary(16));
        arrow::StringBuilder protocol;
        arrow::DoubleBuilder rate;
        ASSERT_TRUE(v4.Append(0x01020304).ok() && v4.AppendNull().ok());
        const uint8_t bytes[16] = {0x20, 1, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
        ASSERT_TRUE(v6.Append(bytes).ok() && v6.AppendNull().ok());
        const std::string protocol_name("HTTP\0状态", 11);
        ASSERT_TRUE(protocol.Append(protocol_name).ok() && protocol.AppendNull().ok());
        ASSERT_TRUE(rate.Append(12.5).ok() && rate.AppendNull().ok());
        auto schema = arrow::schema(
            {arrow::field("v4", arrow::uint32(), true,
                          arrow::key_value_metadata({"flowsql.semantic", "flowsql.address_encoding"},
                                                    {"ipv4_address", "unsigned_numeric"})),
             arrow::field("v6", arrow::fixed_size_binary(16), true,
                          arrow::key_value_metadata({"flowsql.semantic", "flowsql.address_encoding"},
                                                    {"ipv6_address", "network_octets"})),
             arrow::field("protocol_name", arrow::utf8(), true), arrow::field("rate", arrow::float64(), true)});
        DataFrame addresses;
        addresses.FromArrow(arrow::RecordBatch::Make(schema, 2,
                                                     {v4.Finish().ValueOrDie(), v6.Finish().ValueOrDie(),
                                                      protocol.Finish().ValueOrDie(), rate.Finish().ValueOrDie()}));
        auto channel = std::make_shared<DataFrameChannel>("dataframe", "address_preview");
        ASSERT_EQ(channel->Open(), 0);
        ASSERT_EQ(channel->Write(&addresses), 0);
        ASSERT_EQ(p.Register("address_preview", std::static_pointer_cast<IChannel>(channel)), 0);
        ASSERT_EQ(routes["POST:/channels/dataframe/preview"]("/channels/dataframe/preview",
                                                             R"({"name":"address_preview"})", rsp),
                  error::OK);
        rapidjson::Document page;
        page.Parse(rsp.c_str());
        ASSERT_TRUE(!page.HasParseError());
        ASSERT_EQ(std::string(page["data"][0][0].GetString()), "1.2.3.4");
        ASSERT_EQ(std::string(page["data"][0][1].GetString()), "2001:db8::1");
        ASSERT_TRUE(page["data"][1][0].IsNull() && page["data"][1][1].IsNull());
        ASSERT_EQ(std::string(page["data"][0][2].GetString(), page["data"][0][2].GetStringLength()), protocol_name);
        ASSERT_EQ(page["data"][0][3].GetDouble(), 12.5);
        ASSERT_TRUE(page["data"][1][2].IsNull() && page["data"][1][3].IsNull());
        ASSERT_EQ(std::string(page["types"][1].GetString()), "fixed_size_binary[16]");
    }
    ASSERT_EQ(
        p.Register("packet_preview", std::static_pointer_cast<IChannel>(MakePacketPreviewChannel("packet_preview"))),
        0);
    ASSERT_EQ(routes["POST:/channels/dataframe/preview"]("/channels/dataframe/preview",
                                                         R"({"name":"packet_preview","page":1,"page_size":2})", rsp),
              error::OK);
    {
        rapidjson::Document packet_page;
        packet_page.Parse(rsp.c_str());
        ASSERT_TRUE(!packet_page.HasParseError() && packet_page.IsObject());
        ASSERT_TRUE(packet_page.HasMember("entity") && packet_page["entity"].IsString());
        ASSERT_EQ(std::string(packet_page["entity"].GetString()), "packet");
        ASSERT_TRUE(packet_page.HasMember("rows") && packet_page["rows"].IsInt());
        ASSERT_EQ(packet_page["rows"].GetInt(), 3);
        ASSERT_TRUE(packet_page.HasMember("page") && packet_page["page"].IsInt());
        ASSERT_EQ(packet_page["page"].GetInt(), 1);
        ASSERT_TRUE(packet_page.HasMember("page_size") && packet_page["page_size"].IsInt());
        ASSERT_EQ(packet_page["page_size"].GetInt(), 2);
        ASSERT_TRUE(packet_page.HasMember("columns") && packet_page["columns"].IsArray());
        ASSERT_TRUE(packet_page.HasMember("data") && packet_page["data"].IsArray());
        ASSERT_EQ(packet_page["data"].Size(), rapidjson::SizeType(2));

        const int raw_index = JsonColumnIndex(packet_page["columns"], "raw_data");
        const int status_index = JsonColumnIndex(packet_page["columns"], "layer_status");
        const int layers_index = JsonColumnIndex(packet_page["columns"], "layer_ids");
        const int mac_index = JsonColumnIndex(packet_page["columns"], "src_mac");
        const int port_index = JsonColumnIndex(packet_page["columns"], "src_port");
        ASSERT_TRUE(raw_index >= 0 && status_index >= 0 && layers_index >= 0);
        ASSERT_TRUE(mac_index >= 0 && port_index >= 0);

        const auto& first = packet_page["data"][0];
        const auto& first_raw = first[raw_index];
        ASSERT_TRUE(first_raw.IsObject());
        ASSERT_EQ(std::string(first_raw["hex"].GetString()), "");
        ASSERT_EQ(first_raw["byte_length"].GetUint64(), uint64_t(0));
        ASSERT_TRUE(!first_raw["truncated"].GetBool());
        ASSERT_TRUE(first[mac_index].IsNull());
        ASSERT_TRUE(first[port_index].IsNull());

        const auto& second = packet_page["data"][1];
        const auto& second_raw = second[raw_index];
        ASSERT_TRUE(second_raw.IsObject());
        ASSERT_EQ(std::string(second_raw["hex"].GetString()), ByteSequenceHex(64));
        ASSERT_EQ(second_raw["byte_length"].GetUint64(), uint64_t(64));
        ASSERT_TRUE(!second_raw["truncated"].GetBool());
        ASSERT_TRUE(second[status_index].IsUint());
        ASSERT_EQ(second[status_index].GetUint(), static_cast<unsigned>(packet::LayerStatus::kDecoded));
        ASSERT_TRUE(second[layers_index].IsArray());
        ASSERT_EQ(second[layers_index].Size(), rapidjson::SizeType(packet::kMaxLayerDepth));
        ASSERT_EQ(second[layers_index][0].GetUint(), unsigned(1));
        ASSERT_EQ(second[layers_index][1].GetUint(), unsigned(300));
        ASSERT_TRUE(second[mac_index].IsString());
        ASSERT_EQ(std::string(second[mac_index].GetString()), "001122334455");
    }

    ASSERT_EQ(routes["POST:/channels/dataframe/preview"]("/channels/dataframe/preview",
                                                         R"({"name":"packet_preview","page":2,"page_size":2})", rsp),
              error::OK);
    {
        rapidjson::Document packet_page;
        packet_page.Parse(rsp.c_str());
        ASSERT_TRUE(!packet_page.HasParseError() && packet_page.IsObject());
        ASSERT_EQ(packet_page["page"].GetInt(), 2);
        ASSERT_EQ(packet_page["data"].Size(), rapidjson::SizeType(1));
        const int raw_index = JsonColumnIndex(packet_page["columns"], "raw_data");
        const int ipv6_index = JsonColumnIndex(packet_page["columns"], "ip_endpoints");
        const int port_index = JsonColumnIndex(packet_page["columns"], "src_port");
        const int protocol_index = JsonColumnIndex(packet_page["columns"], "protocol_id");
        ASSERT_TRUE(raw_index >= 0 && ipv6_index >= 0 && port_index >= 0 && protocol_index >= 0);
        const auto& row = packet_page["data"][0];
        const auto& raw = row[raw_index];
        ASSERT_EQ(std::string(raw["hex"].GetString()), ByteSequenceHex(64));
        ASSERT_EQ(raw["byte_length"].GetUint64(), uint64_t(65));
        ASSERT_TRUE(raw["truncated"].GetBool());
        ASSERT_TRUE(row[ipv6_index].IsObject());
        ASSERT_EQ(row[ipv6_index]["family"].GetInt(), 6);
        ASSERT_EQ(std::string(row[ipv6_index]["endpoints"]["src"].GetString()), ByteSequenceHex(16));
        ASSERT_EQ(row[port_index].GetUint(), unsigned(3389));
        ASSERT_EQ(row[protocol_index].GetUint(), unsigned(400));
    }

    ASSERT_EQ(routes["POST:/channels/dataframe/preview"]("/channels/dataframe/preview",
                                                         R"({"name":"packet_preview","page":1,"page_size":101})", rsp),
              error::BAD_REQUEST);
    ASSERT_EQ(routes["POST:/channels/dataframe/preview"]("/channels/dataframe/preview",
                                                         R"({"name":"packet_preview","page":0,"page_size":2})", rsp),
              error::BAD_REQUEST);
    // T30: 预览不存在通道 -> 404
    ASSERT_EQ(routes["POST:/channels/dataframe/preview"]("/channels/dataframe/preview", R"({"name":"not_found"})", rsp),
              error::NOT_FOUND);

    // rename
    ASSERT_EQ(routes["POST:/channels/dataframe/rename"]("/channels/dataframe/rename",
                                                        R"({"name":"users","new_name":"users_renamed"})", rsp),
              error::OK);
    ASSERT_TRUE(rsp.find("users_renamed") != std::string::npos);
    // T32: rename 源不存在 -> 404
    ASSERT_EQ(routes["POST:/channels/dataframe/rename"]("/channels/dataframe/rename",
                                                        R"({"name":"missing","new_name":"x"})", rsp),
              error::NOT_FOUND);
    // T33: rename 目标已存在 -> 409
    ASSERT_EQ(p.Register("exists_name", std::static_pointer_cast<IChannel>(MakeChannel("exists_name", 1))), 0);
    ASSERT_EQ(routes["POST:/channels/dataframe/rename"]("/channels/dataframe/rename",
                                                        R"({"name":"users_renamed","new_name":"exists_name"})", rsp),
              error::CONFLICT);

    // delete
    ASSERT_EQ(
        routes["POST:/channels/dataframe/delete"]("/channels/dataframe/delete", R"({"name":"users_renamed"})", rsp),
        error::OK);
    ASSERT_TRUE(rsp.find("\"ok\":true") != std::string::npos);
    ASSERT_TRUE(GetDataFrame(p, "users_renamed") == nullptr);
    // T35: 删除不存在 -> 404
    ASSERT_EQ(
        routes["POST:/channels/dataframe/delete"]("/channels/dataframe/delete", R"({"name":"users_renamed"})", rsp),
        error::NOT_FOUND);

    // T27a: import with bad tmp_path -> 400
    ASSERT_EQ(routes["POST:/channels/dataframe/import"](
                  "/channels/dataframe/import", R"({"filename":"bad.csv","tmp_path":"/tmp/not_exists.csv"})", rsp),
              error::BAD_REQUEST);

    // T36: 全整数列 -> INT64
    {
        const std::string f = upload + "/ints.csv";
        FILE* fp = fopen(f.c_str(), "wb");
        ASSERT_TRUE(fp != nullptr);
        const char* csv = "v\n1\n2\n3\n";
        fwrite(csv, 1, strlen(csv), fp);
        fclose(fp);
        req = "{\"filename\":\"ints.csv\",\"tmp_path\":\"" + f + "\"}";
        ASSERT_EQ(routes["POST:/channels/dataframe/import"]("/channels/dataframe/import", req, rsp), error::OK);
        rapidjson::Document d;
        d.Parse(rsp.c_str());
        ASSERT_TRUE(d.IsObject() && d.HasMember("schema"));
        ASSERT_EQ(d["schema"][0]["type"].GetInt(), static_cast<int>(DataType::INT64));
    }
    // T37: 含小数 -> DOUBLE
    {
        const std::string f = upload + "/double.csv";
        FILE* fp = fopen(f.c_str(), "wb");
        ASSERT_TRUE(fp != nullptr);
        const char* csv = "v\n1.5\n2.0\n";
        fwrite(csv, 1, strlen(csv), fp);
        fclose(fp);
        req = "{\"filename\":\"double.csv\",\"tmp_path\":\"" + f + "\"}";
        ASSERT_EQ(routes["POST:/channels/dataframe/import"]("/channels/dataframe/import", req, rsp), error::OK);
        rapidjson::Document d;
        d.Parse(rsp.c_str());
        ASSERT_EQ(d["schema"][0]["type"].GetInt(), static_cast<int>(DataType::DOUBLE));
    }
    // T38: 混合整数+字符串 -> STRING
    {
        const std::string f = upload + "/mixed.csv";
        FILE* fp = fopen(f.c_str(), "wb");
        ASSERT_TRUE(fp != nullptr);
        const char* csv = "v\n1\nabc\n";
        fwrite(csv, 1, strlen(csv), fp);
        fclose(fp);
        req = "{\"filename\":\"mixed.csv\",\"tmp_path\":\"" + f + "\"}";
        ASSERT_EQ(routes["POST:/channels/dataframe/import"]("/channels/dataframe/import", req, rsp), error::OK);
        rapidjson::Document d;
        d.Parse(rsp.c_str());
        ASSERT_EQ(d["schema"][0]["type"].GetInt(), static_cast<int>(DataType::STRING));
    }
    // T39: 含空值 -> STRING
    {
        const std::string f = upload + "/empty_cell.csv";
        FILE* fp = fopen(f.c_str(), "wb");
        ASSERT_TRUE(fp != nullptr);
        const char* csv = "v\n1\n\n3\n";
        fwrite(csv, 1, strlen(csv), fp);
        fclose(fp);
        req = "{\"filename\":\"empty_cell.csv\",\"tmp_path\":\"" + f + "\"}";
        ASSERT_EQ(routes["POST:/channels/dataframe/import"]("/channels/dataframe/import", req, rsp), error::OK);
        rapidjson::Document d;
        d.Parse(rsp.c_str());
        ASSERT_EQ(d["schema"][0]["type"].GetInt(), static_cast<int>(DataType::STRING));
    }
    // T40: 仅表头无数据 -> rows=0
    {
        const std::string f = upload + "/header_only.csv";
        FILE* fp = fopen(f.c_str(), "wb");
        ASSERT_TRUE(fp != nullptr);
        const char* csv = "a,b,c\n";
        fwrite(csv, 1, strlen(csv), fp);
        fclose(fp);
        req = "{\"filename\":\"header_only.csv\",\"tmp_path\":\"" + f + "\"}";
        ASSERT_EQ(routes["POST:/channels/dataframe/import"]("/channels/dataframe/import", req, rsp), error::OK);
        rapidjson::Document d;
        d.Parse(rsp.c_str());
        ASSERT_TRUE(d.IsObject() && d.HasMember("rows"));
        ASSERT_EQ(d["rows"].GetInt(), 0);
    }

    std::puts("[PASS] T24-T40 http routes");
}

static void TestDataFrameSnapshots() {
    const auto directory = MakeTempDir("snapshot");
    CatalogPlugin catalog;
    ASSERT_EQ(catalog.Option(MakeCatalogOption(directory).c_str()), 0);
    // Snapshot provider does not require the operator registry to expose registered channels.
    auto channel = MakeChannel("result");
    ASSERT_EQ(catalog.Register("result", channel), 0);
    std::vector<DataFrameSource> sources;
    std::string error;
    ASSERT_EQ(catalog.List(&sources, &error), 0);
    ASSERT_EQ(sources.size(), size_t(1));
    DataFrameSnapshotRequest request;
    request.source_id = sources[0].source_id;
    request.max_bytes = 1024 * 1024;
    request.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    request.cancelled = std::make_shared<std::atomic_bool>(false);
    DataFrameSnapshot first;
    ASSERT_EQ(catalog.Acquire(request, &first, &error), 0);
    ASSERT_EQ(first.row_count, uint64_t(2));
    ASSERT_TRUE(first.batch && first.lifetime && !first.schema_revision.empty());
    const auto original = first.batch->ToString();
    bool owner_alive = true;
    DataFrameSnapshot tracked;
    tracked.lifetime = std::shared_ptr<void>(new int(0), [&](void* p) {
        owner_alive = false;
        delete static_cast<int*>(p);
    });
    tracked.batch = std::shared_ptr<arrow::RecordBatch>(first.batch.get(), [&](auto*) { ASSERT_TRUE(owner_alive); });
    tracked = {};
    ASSERT_TRUE(!owner_alive);
    ASSERT_EQ(catalog.Rename("result", "renamed"), 0);
    ASSERT_EQ(catalog.List(&sources, &error), 0);
    ASSERT_EQ(sources[0].source_id, request.source_id);
    ASSERT_EQ(sources[0].label, std::string("dataframe.renamed"));
    DataFrame fixed;
    fixed.FromArrow(first.batch);
    std::thread writer([&] {
        for (int i = 0; i < 50; ++i) ASSERT_EQ(channel->Append(&fixed), 0);
    });
    for (int i = 0; i < 25; ++i) {
        DataFrameSnapshot concurrent;
        ASSERT_EQ(catalog.Acquire(request, &concurrent, &error), 0);
        ASSERT_EQ(concurrent.batch->num_rows(), int64_t(concurrent.row_count));
        ASSERT_TRUE(concurrent.batch->ValidateFull().ok());
    }
    writer.join();
    ASSERT_EQ(first.batch->ToString(), original);
    ASSERT_EQ(catalog.Unregister("renamed"), 0);
    ASSERT_EQ(channel->Close(), 0);
    channel.reset();
    ASSERT_EQ(first.batch->ToString(), original);
    ASSERT_EQ(catalog.Register("renamed", MakeChannel("other")), 0);
    ASSERT_EQ(catalog.List(&sources, &error), 0);
    ASSERT_TRUE(sources[0].source_id != request.source_id);
    DataFrameSnapshot output;
    ASSERT_TRUE(catalog.Acquire(request, &output, &error) != 0 && !output.batch);
    request.source_id = sources[0].source_id;
    request.max_bytes = 1;
    ASSERT_TRUE(catalog.Acquire(request, &output, &error) != 0 && !output.batch);
    request.max_bytes = 1024 * 1024;
    request.cancelled->store(true);
    ASSERT_TRUE(catalog.Acquire(request, &output, &error) != 0 && !output.batch);
    request.cancelled->store(false);
    request.deadline = std::chrono::steady_clock::now();
    ASSERT_TRUE(catalog.Acquire(request, &output, &error) != 0 && !output.batch);
    auto empty = std::make_shared<DataFrameChannel>("dataframe", "empty");
    ASSERT_EQ(empty->Open(), 0);
    DataFrameSnapshotRequest capture;
    capture.max_bytes = 1024 * 1024;
    capture.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    uint64_t bytes = 0;
    std::shared_ptr<arrow::RecordBatch> batch;
    ASSERT_TRUE(empty->CaptureSnapshot(capture, &batch, &bytes, &error) != 0 && !batch);
    DataFrame zero;
    zero.SetSchema({{"value", DataType::INT64}});
    ASSERT_EQ(empty->Write(&zero), 0);
    ASSERT_EQ(empty->CaptureSnapshot(capture, &batch, &bytes, &error), 0);
    ASSERT_EQ(batch->num_rows(), int64_t(0));
    catalog.Stop();
    ASSERT_EQ(first.batch->ToString(), original);
    std::puts("[PASS] dataframe snapshot ownership/identity/concurrency/budgets");
}

static void TestCatalogRequiresCurrentSchema() {
    for (bool missing_category : {false, true}) {
        const auto dir = MakeTempDir("catalog_schema");
        const auto path = dir + "/catalog.db";
        sqlite3* db = nullptr;
        ASSERT_EQ(sqlite3_open(path.c_str(), &db), SQLITE_OK);
        const auto category = missing_category ? "catelog" : "category";
        const auto ddl = std::string("CREATE TABLE operator_catalog(") + category +
                         " TEXT,name TEXT,type TEXT,description TEXT,position TEXT,source TEXT,active INTEGER,"
                         "editable INTEGER,content_ref TEXT,created_at TEXT,updated_at TEXT)";
        ASSERT_EQ(sqlite3_exec(db, ddl.c_str(), nullptr, nullptr, nullptr), SQLITE_OK);
        ASSERT_EQ(sqlite3_exec(db,
                               "INSERT INTO operator_catalog VALUES('fixture','saved','python','meaning',"
                               "'data','bridge',0,1,'source.py','created','updated')",
                               nullptr, nullptr, nullptr),
                  SQLITE_OK);
        sqlite3_close(db);
        const auto bytes = [&] {
            std::ifstream file(path, std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(file)), {});
        };
        const auto original = bytes();
        {
            TestBuiltinRegistry builtins;
            TestQuerier querier;
            querier.builtin_registry = &builtins;
            CatalogPlugin catalog;
            ASSERT_EQ(catalog.Option(MakeCatalogOptionWithDbPath(dir, path).c_str()), 0);
            ASSERT_EQ(catalog.Load(&querier), 0);
            ASSERT_TRUE(catalog.Start() != 0);
            ASSERT_TRUE(catalog.Start() != 0);
            ASSERT_EQ(catalog.Stop(), 0);
            ASSERT_EQ(bytes(), original);
            BinAddonHostPlugin addon;
            querier.op_registry = &catalog;
            ASSERT_EQ(addon.Option(("operator_db_path=" + path).c_str()), 0);
            ASSERT_EQ(addon.Load(&querier), 0);
            ASSERT_TRUE(addon.Start() != 0);
            ASSERT_TRUE(addon.Start() != 0);
            ASSERT_EQ(addon.Stop(), 0);
            ASSERT_EQ(bytes(), original);
        }
        // One-off fixture preparation is explicit and never called by product startup.
        ASSERT_EQ(sqlite3_open(path.c_str(), &db), SQLITE_OK);
        if (missing_category)
            ASSERT_EQ(sqlite3_exec(db, "ALTER TABLE operator_catalog RENAME COLUMN catelog TO category", nullptr,
                                   nullptr, nullptr),
                      SQLITE_OK);
        ASSERT_EQ(sqlite3_exec(db, "ALTER TABLE operator_catalog ADD COLUMN plugin_id TEXT", nullptr, nullptr, nullptr),
                  SQLITE_OK);
        sqlite3_close(db);
        TestBuiltinRegistry builtins;
        TestQuerier querier;
        querier.builtin_registry = &builtins;
        CatalogPlugin catalog;
        ASSERT_EQ(catalog.Option(MakeCatalogOptionWithDbPath(dir, path).c_str()), 0);
        ASSERT_EQ(catalog.Load(&querier), 0);
        ASSERT_EQ(catalog.Start(), 0);
        ASSERT_EQ(catalog.QueryStatus("fixture", "saved"), OperatorStatus::kDeactivated);
        ASSERT_EQ(sqlite3_open(path.c_str(), &db), SQLITE_OK);
        sqlite3_stmt* row = nullptr;
        ASSERT_EQ(sqlite3_prepare_v2(db,
                                     "SELECT description,created_at,updated_at,plugin_id FROM operator_catalog "
                                     "WHERE category='fixture' AND name='saved'",
                                     -1, &row, nullptr),
                  SQLITE_OK);
        ASSERT_EQ(sqlite3_step(row), SQLITE_ROW);
        ASSERT_EQ(std::string(reinterpret_cast<const char*>(sqlite3_column_text(row, 0))), "meaning");
        ASSERT_EQ(std::string(reinterpret_cast<const char*>(sqlite3_column_text(row, 1))), "created");
        ASSERT_EQ(std::string(reinterpret_cast<const char*>(sqlite3_column_text(row, 2))), "updated");
        ASSERT_EQ(sqlite3_column_type(row, 3), SQLITE_NULL);
        sqlite3_finalize(row);
        sqlite3_close(db);
        ASSERT_EQ(catalog.Stop(), 0);
    }
    std::puts("[PASS] current catalog required, rejected data unchanged, offline-prepared records retained");
}

int main() {
    std::puts("=== CatalogPlugin Tests ===");

    TestCatalogRequiresCurrentSchema();
    TestDataFrameSnapshots();
    TestRegistryBasic();
    TestRestartRecover();
    TestConcurrency();
    TestPersistFail();
    TestOperatorRegistry();
    TestOperatorCatalog();
    TestCppOperatorPluginContracts();
    TestCppPluginLifecycle();
    TestOperatorDbPathOption();
    TestHttpRoutes();

    std::puts("=== All CatalogPlugin tests passed ===");
    return 0;
}
