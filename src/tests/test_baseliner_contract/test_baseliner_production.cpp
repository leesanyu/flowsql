// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <arrow/io/api.h>
#include <arrow/ipc/api.h>
#include <dlfcn.h>
#include <framework/core/channel_adapter.h>
#include <framework/core/dataframe.h>
#include <framework/core/dataframe_channel.h>
#include <framework/core/packet_codec.h>
#include <framework/interfaces/cpp_operator_plugin_abi.h>
#include <framework/interfaces/iblock_stream_factory.h>
#include <framework/interfaces/iblock_stream_reader.h>
#include <framework/interfaces/iblock_transform_operator.h>
#include <framework/interfaces/icapture_block_stream_reader.h>
#include <framework/interfaces/ichannel_registry.h>
#include <framework/interfaces/idatabase_factory.h>
#include <framework/interfaces/idataframe_channel.h>
#include <framework/interfaces/irouter_handle.h>
#include <operators/baseliner/durable_store.h>
#include <operators/baseliner/model_output.h>
#include <operators/baseliner/poll_input.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <services/database/database_plugin.h>
#include <unistd.h>
#include <cassert>
#include <chrono>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wtype-limits"
#include <common/loader.hpp>
#pragma GCC diagnostic pop
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include <thread>
using namespace flowsql;
using namespace flowsql::baseliner;
namespace {
void Check(bool ok, const std::string& detail) {
    if (!ok) {
        std::cerr << detail << std::endl;
        std::abort();
    }
}
std::string Json(const std::vector<std::pair<std::string, std::string>>& fields) {
    rapidjson::StringBuffer b;
    rapidjson::Writer<rapidjson::StringBuffer> w(b);
    w.StartObject();
    for (const auto& p : fields) {
        w.Key(p.first.c_str());
        w.String(p.second.c_str());
    }
    w.EndObject();
    return {b.GetString(), b.GetSize()};
}
std::string Base64(const std::string& s) {
    const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < s.size(); i += 3) {
        uint32_t n = static_cast<unsigned char>(s[i]) << 16;
        if (i + 1 < s.size()) n |= static_cast<unsigned char>(s[i + 1]) << 8;
        if (i + 2 < s.size()) n |= static_cast<unsigned char>(s[i + 2]);
        out += alphabet[(n >> 18) & 63];
        out += alphabet[(n >> 12) & 63];
        out += i + 1 < s.size() ? alphabet[(n >> 6) & 63] : '=';
        out += i + 2 < s.size() ? alphabet[n & 63] : '=';
    }
    return out;
}
fnRouterHandler Route(const char* uri) {
    fnRouterHandler h;
    PluginLoader::Single()->Traverse(IID_ROUTER_HANDLE, [&](void* p) {
        static_cast<IRouterHandle*>(p)->EnumRoutes([&](const RouteItem& r) {
            if (r.method == "POST" && r.uri == uri) h = r.handler;
        });
        return h ? -1 : 0;
    });
    Check(static_cast<bool>(h), uri);
    return h;
}
rapidjson::Document Call(const char* uri, const std::string& body) {
    std::string response;
    int rc = Route(uri)(uri, body, response);
    Check(rc == 0, std::string(uri) + ": " + response);
    rapidjson::Document d;
    d.Parse(response.c_str());
    Check(!d.HasParseError(), response);
    if (d.HasMember("status") && d["status"].IsString() && std::string(d["status"].GetString()) == "failed")
        std::cerr << response << std::endl;
    return d;
}
rapidjson::Document Execute(const std::string& sql) { return Call("/scheduler/batch/execute", Json({{"sql", sql}})); }
std::shared_ptr<arrow::RecordBatch> Query(const std::string& sql) {
    // COUNT validates the empty managed result through native SQL; FlowSQL SELECT columns have no expressions.
    if (sql.find(" FROM clickhouse.") != std::string::npos ||
        sql.find("SELECT COUNT(*) AS empty_result_count FROM ") == 0) {
        const auto begin = sql.find(" FROM ") + 6, end = sql.find(' ', begin);
        const auto reference = sql.substr(begin, end - begin);
        const auto a = reference.find('.'), b = reference.find('.', a + 1);
        auto* factory = static_cast<IDatabaseChannelLeaseProviderV1*>(
            PluginLoader::Single()->First(IID_DATABASE_CHANNEL_LEASE_PROVIDER_V1));
        auto channel =
            factory->AcquireChannel(reference.substr(0, a).c_str(), reference.substr(a + 1, b - a - 1).c_str());
        assert(channel);
        auto native = sql;
        native.replace(begin, reference.size(), reference.substr(b + 1));
        if (reference.substr(0, a) != "clickhouse") {
            DataFrameChannel count_result("_temp", "empty_count");
            assert(count_result.Open() == 0);
            std::string error;
            Check(ChannelAdapter::ReadToDataFrame(channel.get(), native.c_str(), &count_result, &error) == 0, error);
            DataFrame frame;
            assert(count_result.Read(&frame) == 0);
            return frame.ToArrow();
        }
        std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
        auto rc = channel->ExecuteQueryArrow(native.c_str(), &batches);
        Check(rc == 0, channel->GetLastError());
        auto combined = arrow::ConcatenateRecordBatches(batches);
        Check(combined.ok(), combined.status().ToString());
        return combined.ValueOrDie();
    }
    Execute(sql + " INTO dataframe.t73_query");
    auto* registry = static_cast<IChannelRegistry*>(PluginLoader::Single()->First(IID_CHANNEL_REGISTRY));
    assert(registry);
    auto channel = std::dynamic_pointer_cast<IDataFrameChannel>(registry->Get("t73_query"));
    assert(channel);
    DataFrame frame;
    assert(channel->Read(&frame) == 0);
    auto batch = frame.ToArrow();
    assert(batch && batch->ValidateFull().ok());
    return batch;
}
std::string Fingerprint(const arrow::RecordBatch& b, bool include_native_schema = true) {
    std::string out = include_native_schema ? b.schema()->ToString(true) : std::to_string(b.num_rows()) + ":";
    if (!include_native_schema)
        for (const auto& field : b.schema()->fields())
            out += std::to_string(field->name().size()) + ":" + field->name();
    for (int64_t r = 0; r < b.num_rows(); ++r)
        for (int c = 0; c < b.num_columns(); ++c) {
            auto s = b.column(c)->GetScalar(r).ValueOrDie();
            std::string v = s->is_valid ? s->ToString() : "NULL";
            out += std::to_string(v.size()) + ":" + v;
        }
    return out;
}
void Pcap(const std::string& path) {
    std::ofstream f(path, std::ios::binary);
    auto le = [&](uint32_t n, int bytes) {
        for (int j = 0; j < bytes; ++j) f.put(static_cast<char>(n >> (8 * j)));
    };
    le(0xa1b2c3d4, 4);
    le(2, 2);
    le(4, 2);
    le(0, 4);
    le(0, 4);
    le(65535, 4);
    le(1, 4);
    for (int t = 0; t <= 40; ++t)
        for (int g = 1; g <= 3; ++g) {
            // Valid Ethernet/IPv4/UDP, different destination ports form the Relation groups.
            std::vector<uint8_t> p = {0,  17, 34,  51, 68,  85, 102, 119, 136, 153, 170, 187, 8,   0,
                                      69, 0,  0,   0,  0,   0,  0,   0,   64,  17,  0,   0,   192, 0,
                                      2,  1,  198, 51, 100, 2,  160, 40,  1,   0,   0,   0,   0,   0};
            p.resize(63 + g, 0);
            const auto ip = p.size() - 14, udp = p.size() - 34;
            p[16] = ip >> 8;
            p[17] = ip;
            p[36] = static_cast<uint8_t>(g >> 8);
            p[37] = g;
            p[38] = udp >> 8;
            p[39] = udp;
            uint32_t sum = 0;
            for (int j = 14; j < 34; j += 2) sum += (p[j] << 8) | p[j + 1];
            while (sum >> 16) sum = (sum & 65535) + (sum >> 16);
            p[24] = static_cast<uint8_t>((~sum) >> 8);
            p[25] = static_cast<uint8_t>(~sum);
            le(t * 60 + 1, 4);
            le(0, 4);
            le(p.size(), 4);
            le(p.size(), 4);
            f.write(reinterpret_cast<const char*>(p.data()), p.size());
        }
    assert(f.good());
}
std::string Isolate(const std::string& original, const std::string& root, const std::string& unique) {
    database::DatabasePlugin db;
    int saved = dup(STDOUT_FILENO);
    FILE* quiet = fopen("/dev/null", "w");
    dup2(fileno(quiet), STDOUT_FILENO);
    int rc = db.Option(original.c_str());
    fflush(stdout);
    dup2(saved, STDOUT_FILENO);
    close(saved);
    fclose(quiet);
    assert(rc == 0 && db.Load(nullptr) == 0 && db.Start() == 0);
    std::string result;
    for (auto type : {"sqlite", "mysql", "postgres", "clickhouse"}) {
        auto channel = db.AcquireChannel(type, ("t2" + std::string(type)).c_str());
        Check(bool(channel), std::string(type) + ": " + db.LastError());
        std::string config;
        std::istringstream parts(original);
        std::string part;
        while (std::getline(parts, part, '|'))
            if (part.find("type=" + std::string(type) + ";") != std::string::npos) config = part;
        Check(!config.empty(), "isolated backend configuration");
        while (!config.empty() && (config.back() == '\n' || config.back() == '\r')) config.pop_back();
        config += ";name=t73" + std::string(type);
        if (std::string(type) == "sqlite")
            config += ";path=" + root + "/source.db";
        else {
            auto name = "baseline_t73_" + unique;
            auto created = channel->ExecuteSql(("CREATE DATABASE " + name).c_str());
            Check(created >= 0, std::string(type) + " isolated database: " + channel->GetLastError());
            config += ";database=" + name;
        }
        if (!result.empty()) result += '|';
        result += config;
        if (std::string(type) != "clickhouse") result += '|' + config + ";name=t5alias" + type;
    }
    db.Stop();
    db.Unload();
    return result;
}
// Test-only capture adapter. It drains an actual finite PCAP reader, so its final packet fact
// is backed by consumed packets and confirmed EOF, with no unread packet backlog or idle extrapolation.
class CapturePcap final : public IBlockStreamFactory, public IBlockStreamReaderFactoryV1 {
 public:
    std::string pcap_name;
    class Source final : public IBlockStreamChannel {
     public:
        const char* Category() override { return "t73capture"; }
        const char* Name() override { return "live"; }
        const char* Type() override { return ChannelType::kBlockStream; }
        const char* Schema() override { return "packet"; }
        int Open() override { return 0; }
        int Close() override { return 0; }
        int Flush() override { return 0; }
        bool IsOpened() const override { return true; }
        bool IsFinished() const override { return false; }
        BlockPollEvent PollBlock(int) override { return {BlockPollEvent::kError, nullptr, EINVAL}; }
        int ReleaseBlock(const std::shared_ptr<arrow::RecordBatch>&) override { return EINVAL; }
        void Cancel() override {}
    } source;
    class Reader final : public ICaptureBlockStreamReaderV2 {
     public:
        explicit Reader(std::vector<std::shared_ptr<arrow::RecordBatch>> batches) : batches_(std::move(batches)) {}
        const char* Category() override { return "t73capture"; }
        const char* Name() override { return "live"; }
        const char* Type() override { return ChannelType::kBlockStream; }
        const char* Schema() override { return "packet"; }
        int Open() override { return 0; }
        int Close() override {
            batches_.clear();
            return 0;
        }
        int Flush() override { return 0; }
        bool IsOpened() const override { return true; }
        bool IsFinished() const override { return index_ >= batches_.size(); }
        BlockPollEvent PollBlock(int) override { return {BlockPollEvent::kError, nullptr, EINVAL}; }
        int ReleaseBlock(const std::shared_ptr<arrow::RecordBatch>& b) override {
            assert(outstanding_ == b);
            outstanding_.reset();
            return 0;
        }
        void Cancel() override { cancelled_ = true; }
        int DescribeSources(CaptureSourceSetV2* out) const override {
            *out = {};
            CaptureQueueIdentityV1 identity;
            identity.source_name = "t73capture.live";
            identity.source_id = 0;
            identity.observation_domain_id = 1;
            identity.generation = 1;
            identity.link_type = 1;
            out->inputs = {identity};
            out->limits.max_packets_per_batch = 128;
            out->limits.max_bytes_per_batch = 1 << 20;
            out->limits.max_wait_ms = 100;
            out->limits.max_outstanding_batches = 1;
            return 0;
        }
        CapturePollEventV2 PollCapture(int) override {
            CapturePollEventV2 e;
            if (cancelled_) {
                e.block.kind = BlockPollEvent::kCancelled;
                return e;
            }
            assert(!outstanding_);
            if (index_ == batches_.size()) {
                e.block.kind = BlockPollEvent::kEof;
                return e;
            }
            outstanding_ = batches_[index_++];
            e.block = {BlockPollEvent::kData, outstanding_, 0};
            // PCAP is sorted, and the adapter retains the entire remaining tail. Only the last
            // consumed batch can certify an empty backlog and close the common capture prefix.
            {
                auto time = std::static_pointer_cast<arrow::Int64Array>(outstanding_->GetColumnByName("timestamp_ns"));
                assert(time && time->length());
                CaptureProgressV1 fact;
                fact.source_id = 0;
                fact.generation = 1;
                fact.fact_sequence = index_;
                fact.capture_time_ns = time->Value(time->length() - 1);
                fact.packet_observed = true;
                fact.backlog = index_ == batches_.size() ? CaptureBacklogV1::kEmpty : CaptureBacklogV1::kPresent;
                e.progress = {fact};
            }
            return e;
        }
        int ReadInputCounters(uint32_t, CaptureCountersV1* out) const override {
            *out = {};
            out->generation = 1;
            return 0;
        }

     private:
        std::vector<std::shared_ptr<arrow::RecordBatch>> batches_;
        size_t index_ = 0;
        std::shared_ptr<arrow::RecordBatch> outstanding_;
        std::atomic<bool> cancelled_{false};
    };
    IBlockStreamChannel* Get(const char* type, const char* name) override {
        return type && name && std::string(type) == "t73capture" && std::string(name) == "live" ? &source : nullptr;
    }
    void List(std::function<void(const char*, const char*, IBlockStreamChannel*)> fn) override {
        fn("t73capture", "live", &source);
    }
    int CreateReader(const BlockStreamReaderConfigV1& config, IBlockStreamChannel** out) override {
        *out = nullptr;
        if (std::string(config.source_category) != "t73capture") return ENOTSUP;
        IBlockStreamChannel* input = nullptr;
        IBlockStreamReaderFactoryV1* owner = nullptr;
        auto actual = config;
        actual.source_category = "pcapfile";
        actual.source_name = pcap_name.c_str();
        PluginLoader::Single()->Traverse(IID_BLOCK_STREAM_READER_FACTORY_V1, [&](void* p) {
            auto* factory = static_cast<IBlockStreamReaderFactoryV1*>(p);
            if (factory == this) return 0;
            if (factory->CreateReader(actual, &input) == 0) {
                owner = factory;
                return -1;
            }
            return 0;
        });
        assert(input && owner);
        std::unique_ptr<IBlockStreamChannel, std::function<void(IBlockStreamChannel*)>> held(input, [owner](auto* p) {
            p->Close();
            owner->ReleaseReader(p);
        });
        assert(input->Open() == 0);
        std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
        for (;;) {
            auto e = input->PollBlock(100);
            if (e.kind == BlockPollEvent::kEof) break;
            assert(e.kind == BlockPollEvent::kData);
            Check(e.batch->schema()->Equals(*packet::PacketSchema(), true), e.batch->schema()->ToString(true));
            auto source_ids = std::static_pointer_cast<arrow::UInt32Array>(e.batch->column(4));
            for (int64_t r = 0; r < source_ids->length(); ++r)
                Check(source_ids->Value(r) == 0, "PCAP source ID must be zero");
            batches.push_back(e.batch);
            assert(input->ReleaseBlock(e.batch) == 0);
        }
        assert(!batches.empty());
        *out = new Reader(std::move(batches));
        return 0;
    }
    void ReleaseReader(IBlockStreamChannel* reader) override { delete reader; }
};
std::string CaptureRun(const std::string& exact, const std::string& id) {
    const std::string sql =
        "SELECT * FROM t73capture.live USING npm.basic WITH "
        "input_namespace='t73-capture',source_domains='0:1',features='basic',udp_idle_timeout_ns=120000000000,output_"
        "interval_ns=60000000000 INTO " +
        exact;
    Call("/scheduler/batch/submit", Json({{"runtime_task_id", id}, {"sql_text", sql}}));
    for (int i = 0; i < 1500; ++i) {
        auto d = Call("/scheduler/batch/status", Json({{"runtime_task_id", id}}));
        auto status = std::string(d["status"].GetString());
        Check(status != "failed", "NPM capture producer failed");
        if (status == "completed") return d["managed_result"]["run_id"].GetString();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    Check(false, "NPM capture producer deadline");
    return {};
}

void QueryOwnership() {
    class Reader final : public IBatchReader {
     public:
        Reader() {
            auto schema = arrow::schema({arrow::field("s", arrow::utf8()), arrow::field("v", arrow::int64())});
            for (int i = 0; i < 2; ++i) {
                arrow::StringBuilder s;
                arrow::Int64Builder v;
                assert(s.Append(std::string(8192, static_cast<char>('a' + i))).ok());
                assert(v.Append(i + 42).ok());
                auto batch = arrow::RecordBatch::Make(schema, 1, {s.Finish().ValueOrDie(), v.Finish().ValueOrDie()});
                auto out = arrow::io::BufferOutputStream::Create().ValueOrDie();
                auto writer = arrow::ipc::MakeStreamWriter(out, schema).ValueOrDie();
                assert(writer->WriteRecordBatch(*batch).ok() && writer->Close().ok());
                auto b = out->Finish().ValueOrDie();
                buffers.emplace_back(b->data(), b->data() + b->size());
            }
        }
        int GetSchema(const uint8_t**, size_t*) override { return -1; }
        int Next(const uint8_t** data, size_t* size) override {
            if (index) std::fill(buffers[index - 1].begin(), buffers[index - 1].end(), 0);
            if (index == buffers.size()) return 1;
            *data = buffers[index].data();
            *size = buffers[index++].size();
            return 0;
        }
        void Cancel() override {}
        void Close() override {
            for (auto& b : buffers) std::fill(b.begin(), b.end(), 0);
        }
        const char* GetLastError() override { return ""; }
        void Release() override { delete this; }
        std::vector<std::vector<uint8_t>> buffers;
        size_t index = 0;
    };
    class Channel final : public IDatabaseChannel {
     public:
        const char* Category() override { return "fixture"; }
        const char* Name() override { return "ipc"; }
        const char* Type() override { return ChannelType::kDatabase; }
        const char* Schema() override { return ""; }
        int Open() override { return 0; }
        int Close() override { return 0; }
        int Flush() override { return 0; }
        bool IsOpened() const override { return true; }
        bool IsConnected() override { return true; }
        const char* GetLastError() override { return ""; }
        int CreateReader(const char*, IBatchReader** out) override {
            *out = new Reader;
            return 0;
        }
        int CreateWriter(const char*, IBatchWriter**) override { return -1; }
        int CreateArrowReader(const char*, IArrowReader**) override { return -1; }
        int CreateArrowWriter(const char*, IArrowWriter**) override { return -1; }
        int ExecuteQueryArrow(const char*, std::vector<std::shared_ptr<arrow::RecordBatch>>*) override { return -1; }
        int WriteArrowBatches(const char*, const std::vector<std::shared_ptr<arrow::RecordBatch>>&) override {
            return -1;
        }
        int ExecuteSql(const char*) override { return -1; }
    } source;
    DataFrameChannel sink("dataframe", "ownership");
    assert(sink.Open() == 0);
    std::string error;
    Check(ChannelAdapter::ReadToDataFrame(&source, "SELECT s,v", &sink, &error) == 0, error);
    DataFrame frame;
    assert(sink.Read(&frame) == 0);
    auto b = frame.ToArrow();
    assert(b && b->num_rows() == 2);
    auto text = std::static_pointer_cast<arrow::StringArray>(b->column(0));
    auto number = std::static_pointer_cast<arrow::Int64Array>(b->column(1));
    for (int i = 0; i < 2; ++i)
        Check(text->GetString(i) == std::string(8192, static_cast<char>('a' + i)) && number->Value(i) == i + 42,
              "borrowed IPC overwritten after Next/Close");
    std::cout << "PASS Scheduler query owns IPC across reader reuse/close" << std::endl;
}
struct NpmOwner {
    void* handle = nullptr;
    void* provider = nullptr;
    CppOperatorPluginDestroyCapabilityV2Fn destroy = nullptr;
    explicit NpmOwner(PluginLoader* loader) {
        handle = dlopen("./libflowsql_npm_basic.so", RTLD_NOW | RTLD_LOCAL);
        Check(handle, dlerror() ? "NPM dlopen failed" : "NPM dlopen");
        auto count = reinterpret_cast<CppOperatorPluginCountFn>(dlsym(handle, kCppOperatorPluginCountSymbol));
        auto describe =
            reinterpret_cast<CppOperatorPluginDescribeV2Fn>(dlsym(handle, kCppOperatorPluginDescribeV2Symbol));
        auto create = reinterpret_cast<CppOperatorPluginCreateCapabilityV2Fn>(
            dlsym(handle, kCppOperatorPluginCreateCapabilityV2Symbol));
        destroy = reinterpret_cast<CppOperatorPluginDestroyCapabilityV2Fn>(
            dlsym(handle, kCppOperatorPluginDestroyCapabilityV2Symbol));
        assert(count && describe && create && destroy);
        for (int i = 0; i < count(); ++i) {
            CppOperatorDescriptorV2 d{};
            d.struct_size = kCppOperatorDescriptorV2Size;
            assert(describe(i, &d) == 0);
            if (std::memcmp(&d.contract_iid, &IID_BLOCK_TRANSFORM_OPERATOR_V2, sizeof(Guid)) == 0) {
                index = i;
                provider = create(i, loader);
                assert(provider);
                loader->Regist(IID_BLOCK_TRANSFORM_OPERATOR_V2, provider);
                break;
            }
        }
        assert(provider);
    }
    ~NpmOwner() {
        if (provider) destroy(index, provider);
        if (handle) dlclose(handle);
    }
    int index = -1;
};
struct Environment {
    PluginLoader* loader = PluginLoader::Single();
    std::unique_ptr<NpmOwner> npm;
    CapturePcap capture;
    Environment(const std::string& options, const std::string& root) {
        const char* libs[] = {"libflowsql_database.so",      "libflowsql_builtin.so",  "libflowsql_catalog.so",
                              "libflowsql_npi.so",           "libflowsql_pcapfile.so", "libflowsql_scheduler.so",
                              "libflowsql_stream.so",        "libflowsql_baseline.so", "libflowsql_baseliner.so",
                              "libflowsql_config_channel.so"};
        std::string catalog = "data_dir=" + root + ";operator_db_path=" + root + "/operators.db";
        std::string npi = std::string("{\"ldfile\":\"") + FLOWSQL_NPI_PROTOCOLS_PATH + "\",\"concurrency\":2}";
        std::string pcap = "db_path=" + root + "/pcap.db",
                    stream = "config_file=" + root + "/stream.yml;db_path=" + root + "/stream.db";
        std::string config = "db_path=" + root + "/config.db";
        const char* opts[] = {options.c_str(), nullptr,        catalog.c_str(), npi.c_str(), pcap.c_str(),
                              nullptr,         stream.c_str(), nullptr,         nullptr,     config.c_str()};
        // Database Option logs its options; suppress configuration values in test output.
        int saved = dup(STDOUT_FILENO);
        FILE* quiet = fopen("/dev/null", "w");
        dup2(fileno(quiet), STDOUT_FILENO);
        int rc = loader->Load(get_absolute_process_path(), libs, opts, 10);
        fflush(stdout);
        dup2(saved, STDOUT_FILENO);
        close(saved);
        fclose(quiet);
        Check(rc == 0, "load production plugins");
        loader->Regist(IID_BLOCK_STREAM_FACTORY, static_cast<IBlockStreamFactory*>(&capture));
        loader->Regist(IID_BLOCK_STREAM_READER_FACTORY_V1, static_cast<IBlockStreamReaderFactoryV1*>(&capture));
        npm = std::make_unique<NpmOwner>(loader);
        assert(loader->StartAll() == 0);
    }
    ~Environment() {
        loader->StopAll();
        npm.reset();
        loader->Unload();
    }
};
std::string Configuration(const std::string& source, const std::string& run, const std::string& key, bool poll,
                          int multiple = 1) {
    return R"({"schema_version":1,"task_key":")" + key + R"(","source":")" + source + R"(","mode":")" +
           (poll ? "poll" : "snapshot") + R"(",
"clock":{"bucket_seconds":)" +
           std::to_string(60 * multiple) +
           R"(,"timezone":"UTC"},"calendar":{"calendar_id":"cn-holiday","calendar_version":"2026.1"},
"forecast":{"horizon_buckets":2},"read_policy":{"page_rows":37,"poll_interval_ms":5},
"persistence":{"checkpoint_every_buckets":1000,"restore":"if_exists"},
"datasets":[{"id":"npm","table":"npm_basic_history_v1","fields":{"__npm_run_id":"utf8","session_id":"uint64","revision":"uint64",
"observation_domain_id":"uint64","b_port":"uint64","period_start_ns":"int64","period_end_ns":"int64","period_complete":"boolean",
"interval_wire_bytes_total":"uint64","interval_packets_ab":"uint64"},
"scope":{"run_ids":[")" +
           run + R"("],"begin_bucket":0,"end_bucket":)" + std::to_string(40 / multiple) +
           (poll ? "" : ",\"consistency\":\"immutable_range\"") + R"(},
"series_keys":["observation_domain_id"],"row_semantics":"npm_period_increment",
"deduplicate":{"keys":["__npm_run_id","session_id","revision"],"on_duplicate":"require_equal"},
"bucket":{"column":"period_start_ns","unit":"ns"},"filter":[{"column":"period_complete","op":"eq","value":true}],
"metrics":[{"id":"v","kind":"value","column":"interval_wire_bytes_total","aggregate":"sum","feature_type":"value_basic","profile":"default"},
{"id":"r","kind":"ratio","feature_type":"ratio","profile":"rate_core","numerator":{"column":"interval_packets_ab","aggregate":"sum"},"denominator":{"column":"interval_wire_bytes_total","aggregate":"sum"}},
{"id":"dist","kind":"relation","feature_type":"relation","profile":"default","group_space":{"id":"ports","version":"v1","column":"b_port","unknown":"reject"},
"metrics":[{"id":"m","column":"interval_wire_bytes_total","aggregate":"sum"}],"support_policy":{"k_support":3,"min_hist_share":0.01,"min_active_ratio":0.1},"summary_policy":{"k_head":2,"k_stable":1}}]}]})";
}
std::string Publish(const std::string& name, const std::string& content) {
    std::string request = Json(
        {{"name", name}, {"format", "json"}, {"schema_id", "baseliner.task.v1"}, {"content_base64", Base64(content)}});
    request.insert(1, "\"expected_current_revision\":0,");
    auto result = Call("/channels/config/publish", request);
    return result["exact_reference"].GetString();
}
struct Case {
    std::string source, target, key, config, ref, results, checkpoint, with;
    bool poll;
    int multiple = 1;
    bool empty = false;
};
std::string Results(const Case& c) {
    if (c.empty)
        return "SELECT COUNT(*) AS empty_result_count FROM " + c.target + ".baseline_results_v1 WHERE task_key='" +
               c.key + "'";
    return "SELECT "
           "metric_id,result_kind,target_bucket,issued_after_bucket,observed,expected,lower,upper,band_kind,model_"
           "basis_id,"
           "basis_id FROM " +
           c.target + ".baseline_results_v1 WHERE task_key='" + c.key +
           "' ORDER BY metric_id,result_kind,target_bucket,issued_after_bucket,summary_id";
}
void Run(const Case& c, bool restarted) {
    std::string sql = "SELECT * FROM " + c.source + " USING explore.baseliner WITH " +
                      (c.with.empty() ? "config='" + c.ref + "'" : c.with) + " INTO " + c.target;
    if (!c.poll) {
        auto d = Execute(sql);
        Check(std::string(d["status"].GetString()) == "completed", "snapshot completion");
        Check(d.HasMember("result") && d["result"].HasMember("rows_written"), "managed summary contract");
        Check((restarted || c.empty) ? d["result"]["rows_written"].GetInt64() == 0
                                     : d["result"]["rows_written"].GetInt64() > 0,
              "committed row count");
        return;
    }
    const auto id = c.key + (restarted ? "-restart" : "-first");
    Call("/scheduler/batch/submit", Json({{"runtime_task_id", id}, {"sql_text", sql}}));
    const auto body = Json({{"runtime_task_id", id}});
    bool ready = false;
    // Poll has no EOF; wait for the durable closed-prefix output before requesting graceful Stop.
    for (int i = 0; i < 1500; ++i) {
        auto d = Call("/scheduler/batch/status", body);
        Check(std::string(d["status"].GetString()) != "failed", Json({{"status", d["status"].GetString()}}));
        if (d.HasMember("managed_result") && d["managed_result"].HasMember("source_positions")) {
            const auto& p = d["managed_result"]["source_positions"];
            if (p.Size() && p[0]["closed_before_bucket"].GetInt64() >= 40 / c.multiple) {
                ready = true;
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    Check(ready, "poll durable prefix deadline: " + c.key);
    Call("/scheduler/batch/stop", Json({{"runtime_task_id", id}, {"mode", "stop"}}));
    for (int i = 0; i < 500; ++i) {
        auto d = Call("/scheduler/batch/status", body);
        auto s = std::string(d["status"].GetString());
        Check(s != "failed", "poll Stop failed");
        if (s == "stopped") {
            Check(d["managed_result"].HasMember("rows_written"), "poll managed summary");
            Check(restarted ? d["managed_result"]["rows_written"].GetInt64() == 0
                            : d["managed_result"]["rows_written"].GetInt64() > 0,
                  "poll committed row count");
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    Check(false, "Stop deadline");
}
std::string Checkpoint(const Case& c) {
    auto dot = c.target.find('.');
    auto* factory = static_cast<IDatabaseChannelLeaseProviderV1*>(
        PluginLoader::Single()->First(IID_DATABASE_CHANNEL_LEASE_PROVIDER_V1));
    auto target = factory->AcquireChannel(c.target.substr(0, dot).c_str(), c.target.substr(dot + 1).c_str());
    assert(target);
    ConfigSnapshot config;
    assert(ParseConfig(c.config, &config).ok());
    DurableStore store;
    Check(store.Open(config, target.get(), "inspection") == 0, store.LastError());
    StoredGeneration saved;
    Check(store.Load(&saved) == 0, store.LastError());
    Check(saved.generation > 0 && !saved.checkpoint.empty(), "durable model");
    store.Close();
    return saved.checkpoint;
}
void Inspect(const Case& c, const arrow::RecordBatch& b) {
    if (c.empty) {
        Check(
            b.num_rows() == 1 && b.GetColumnByName("empty_result_count")->GetScalar(0).ValueOrDie()->ToString() == "0",
            "empty snapshot produced evaluations");
        return;
    }
    int values = 0, ratios = 0, forecasts = 0, relations = 0, cold = 0;
    for (int64_t r = 0; r < b.num_rows(); ++r) {
        auto str = [&](const char* name) { return b.GetColumnByName(name)->GetScalar(r).ValueOrDie()->ToString(); };
        auto metric = str("metric_id"), kind = str("result_kind");
        auto actual = b.GetColumnByName("observed");
        auto expected = b.GetColumnByName("expected");
        Check(!str("model_basis_id").empty(), "model basis association");
        if (kind == "forecast") {
            ++forecasts;
            Check(actual->IsNull(r), "forecast actual must be NULL");
            continue;
        }
        if (metric == "v") {
            ++values;
            Check(std::stod(str("observed")) == 195 * c.multiple, "NPM interval byte actual");
        }
        if (metric == "r") {
            ++ratios;
            Check(std::abs(std::stod(str("observed")) - 3.0 / 195) < 1e-6, "ratio of sums actual");
        }
        if (metric == "m") {
            ++relations;
            Check(!str("basis_id").empty(), "relation basis association");
        }
        if (str("target_bucket") == "0" && metric == "v") {
            ++cold;
            Check(expected->IsNull(r), "cold NULL prediction");
        }
        if (!expected->IsNull(r))
            Check(!b.GetColumnByName("lower")->IsNull(r) && !b.GetColumnByName("upper")->IsNull(r), "band association");
    }
    Check(values == 40 / c.multiple && ratios == values && relations > 0 && forecasts > 0 && cold == 1,
          "three task result coverage: values=" + std::to_string(values) + " ratios=" + std::to_string(ratios) +
              " relation=" + std::to_string(relations) + " forecasts=" + std::to_string(forecasts) +
              " cold=" + std::to_string(cold));
}
std::string SqlLiteral(const std::string& text) {
    std::string literal = "'";
    for (char c : text) {
        literal += c;
        if (c == '\'') literal += c;
    }
    return literal + "'";
}
std::string ComparableResults(const Case& c) {
    return "SELECT metric_id,result_kind,target_bucket,issued_after_bucket,observed,expected,lower,upper,band_kind "
           "FROM " +
           c.target + ".baseline_results_v1 WHERE task_key='" + c.key +
           "' ORDER BY metric_id,result_kind,target_bucket,issued_after_bucket,observed,expected,lower,upper,band_kind";
}
void ThreePartCases(const std::string& source, const std::string& run, const std::string& poll_run,
                    const std::string& unique, std::vector<Case>* cases) {
    const auto target = "sqlite.t73sqlite";
    for (int variant = 0; variant < 3; ++variant) {
        Case c;
        c.source = source + ".npm_basic_history_v1";
        c.target = target;
        c.poll = variant == 1;
        c.key = "single-" + unique + "-" + source + "-" + std::to_string(variant);
        c.config = Configuration(c.source, c.poll ? poll_run : run, c.key, c.poll);
        if (variant == 2) {
            rapidjson::Document doc;
            doc.Parse(c.config.c_str());
            rapidjson::Value dataset(doc["datasets"][0], doc.GetAllocator());
            dataset.RemoveMember("table");
            doc.RemoveMember("datasets");
            doc.RemoveMember("source");
            doc.RemoveMember("schema_version");
            doc.RemoveMember("mode");
            doc.AddMember("dataset", dataset, doc.GetAllocator());
            rapidjson::StringBuffer buffer;
            rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
            doc.Accept(writer);
            const std::string shorthand(buffer.GetString(), buffer.GetSize());
            ConfigSnapshot normalized;
            Check(NormalizeInlineConfig(shorthand, c.source, &normalized).ok(), "single SQL config normalization");
            c.config = normalized.original_json;
            c.with = "parameters=" + SqlLiteral(shorthand);
        } else
            c.ref = Publish("single" + std::to_string(cases->size()), c.config);
        auto equivalent = std::find_if(cases->begin(), cases->end(), [&](const auto& previous) {
            return previous.source == source && previous.target == target && previous.poll == c.poll &&
                   previous.multiple == 1;
        });
        Check(equivalent != cases->end(), "two-part reference case missing");
        const auto expected = Fingerprint(*Query(ComparableResults(*equivalent)));
        Run(c, false);
        auto batch = Query(Results(c));
        Inspect(c, *batch);
        Check(Fingerprint(*Query(ComparableResults(c))) == expected, "two/three-part baseline results differ");
        c.results = Fingerprint(*batch);
        c.checkpoint = Checkpoint(c);
        cases->push_back(c);
        std::cout << "PASS three-part SQL " << source << " " << (c.poll ? "poll" : "snapshot")
                  << (variant == 2 ? " single WITH" : " exact config") << " matches two-part" << std::endl;
    }
    auto full = Configuration(source + ".npm_basic_history_v1", run, "missing-progress-" + unique, true);
    ConfigSnapshot finite;
    Check(ParseConfig(full, &finite).ok(), "finite producer poll config");
    const auto dot = source.find('.');
    auto* leases = static_cast<IDatabaseChannelLeaseProviderV1*>(
        PluginLoader::Single()->First(IID_DATABASE_CHANNEL_LEASE_PROVIDER_V1));
    auto channel = leases->AcquireChannel(source.substr(0, dot).c_str(), source.substr(dot + 1).c_str());
    PollInput missing;
    Check(missing.Initialize(finite, channel, finite.config.source) != 0, "finite PCAP falsely provided progress");
    Check(missing.LastError().find("publication") != std::string::npos, missing.LastError());
    auto bad = Configuration(source + ".npm_result_entities", run, "wrong-table-" + unique, false);
    const std::string sql = "SELECT * FROM " + source +
                            ".npm_result_entities USING explore.baseliner WITH parameters=" + SqlLiteral(bad) +
                            " INTO " + target;
    std::string response;
    Check(Route("/scheduler/batch/execute")("/scheduler/batch/execute", Json({{"sql", sql}}), response) != 0,
          "SQL accepted a configured table different from FROM");
    std::cout << "PASS three-part SQL " << source << " rejects cross-table config and missing publication" << std::endl;
}
std::string FrameConfiguration(const std::string& source, const std::string& key, bool database = false) {
    return R"({"schema_version":1,"task_key":")" + key + R"(","source":")" + source + R"(","mode":"snapshot",
"clock":{"bucket_seconds":60,"timezone":"UTC"},"calendar":{"calendar_id":"cn-holiday","calendar_version":"2026.1"},
"forecast":{"horizon_buckets":2},"read_policy":{"page_rows":7},
"persistence":{"checkpoint_every_buckets":2,"restore":"if_exists"},
"datasets":[{"id":"source",)" +
           (database ? R"("table":"csv_samples",)" : "") +
           R"("fields":{"bucket":"int64","domain":"uint64","port":"uint64","bytes":"uint64","packets":"uint64","keep":"uint64"},
"scope":{"consistency":")" +
           (database ? "consistent_snapshot" : "dataframe_snapshot") +
           (database ? R"(","begin_bucket":0,"end_bucket":40})" : R"("})") + R"(,
"series_keys":["domain"],"deduplicate":{"keys":["bucket","port"],"on_duplicate":"require_equal"},
"bucket":{"column":"bucket","unit":"bucket_id"},"filter":[{"column":"keep","op":"eq","value":1}],
"metrics":[{"id":"v","kind":"value","column":"bytes","aggregate":"sum","feature_type":"value_basic","profile":"default"},
{"id":"r","kind":"ratio","feature_type":"ratio","profile":"rate_core","numerator":{"column":"packets","aggregate":"sum"},"denominator":{"column":"bytes","aggregate":"sum"}},
{"id":"dist","kind":"relation","feature_type":"relation","profile":"default","group_space":{"id":"ports","version":"v1","column":"port","unknown":"reject"},
"metrics":[{"id":"m","column":"bytes","aggregate":"sum"}],"support_policy":{"k_support":3,"min_hist_share":0.01,"min_active_ratio":0.1},"summary_policy":{"k_head":2,"k_stable":1}}]}]})";
}
void DataFrameCases(const std::string& root, const std::string& unique, std::vector<Case>* cases) {
    // Catalog infers integer columns without guessing units. A numeric filter avoids bool CSV inference.
    const std::string upload = root + "/upload.csv";
    {
        std::ofstream csv(upload);
        csv << "bucket,domain,port,bytes,packets,keep\n";
        for (int bucket = 39; bucket >= 0; --bucket)
            for (int port = 3; port >= 1; --port) csv << bucket << ",1," << port << ',' << 63 + port << ",1,1\n";
        csv << "0,1,2,65,1,1\n100,1,1,999,1,0\n";
    }
    auto imported = Call("/channels/dataframe/import", Json({{"filename", "t3_samples.csv"}, {"tmp_path", upload}}));
    Check(imported["rows"].GetInt() == 122, "real CSV row count");
    const std::string source = "dataframe." + std::string(imported["name"].GetString());
    auto* registry = static_cast<IChannelRegistry*>(PluginLoader::Single()->First(IID_CHANNEL_REGISTRY));
    auto channel = std::dynamic_pointer_cast<IDataFrameChannel>(registry->Get(imported["name"].GetString()));
    Check(bool(channel), "imported CSV source lease");
    DataFrame frame;
    Check(channel->Read(&frame) == 0, "CSV source read");
    const auto original = frame.ToArrow();
    const auto before = Fingerprint(*original);
    Call("/operators/activate", Json({{"name", "builtin.passthrough"}}));
    Execute("SELECT * FROM " + source + " USING builtin.passthrough INTO dataframe.csv_passthrough");
    Check(Fingerprint(*Query("SELECT * FROM dataframe.csv_passthrough")) == before,
          "ordinary DataFrame operator routing changed");
    Execute("SELECT * FROM " + source + " INTO sqlite.t73sqlite.csv_samples");
    Case reference;
    reference.source = "sqlite.t73sqlite.csv_samples";
    reference.target = "sqlite.t73sqlite";
    reference.key = "csv-db-" + unique;
    reference.poll = false;
    reference.config = FrameConfiguration(reference.source, reference.key, true);
    reference.ref = Publish("csvdb", reference.config);
    Run(reference, false);
    Inspect(reference, *Query(Results(reference)));
    // Native SQL Schema differs across targets (MySQL binary/string and NOT NULL metadata).
    // Compare column names, row count, NULLs and every logical cell exactly.
    const auto expected = Fingerprint(*Query(ComparableResults(reference)), false);
    reference.results = Fingerprint(*Query(Results(reference)));
    reference.checkpoint = Checkpoint(reference);
    cases->push_back(reference);
    for (auto target : {"sqlite", "mysql", "postgres"}) {
        Case c;
        c.source = source;
        c.target = std::string(target) + ".t73" + target;
        c.key = "csv-" + unique + '-' + target;
        c.poll = false;
        c.config = FrameConfiguration(source, c.key);
        if (std::string(target) == "sqlite")
            c.ref = Publish("csvfull", c.config);
        else if (std::string(target) == "mysql") {
            rapidjson::Document range;
            range.Parse(c.config.c_str());
            range["datasets"][0]["scope"].AddMember("begin_bucket", 0, range.GetAllocator());
            range["datasets"][0]["scope"].AddMember("end_bucket", 40, range.GetAllocator());
            rapidjson::StringBuffer buffer;
            rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
            range.Accept(writer);
            c.config.assign(buffer.GetString(), buffer.GetSize());
            c.with = "parameters=" + SqlLiteral(c.config);
        } else {
            rapidjson::Document doc;
            doc.Parse(c.config.c_str());
            rapidjson::Value dataset(doc["datasets"][0], doc.GetAllocator());
            doc.RemoveMember("datasets");
            doc.RemoveMember("source");
            doc.RemoveMember("schema_version");
            doc.RemoveMember("mode");
            doc.AddMember("dataset", dataset, doc.GetAllocator());
            rapidjson::StringBuffer buffer;
            rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
            doc.Accept(writer);
            std::string shorthand(buffer.GetString(), buffer.GetSize());
            ConfigSnapshot normalized;
            auto status = NormalizeInlineConfig(shorthand, source, &normalized);
            Check(status.ok(), status.message);
            c.config = normalized.original_json;
            c.with = "parameters=" + SqlLiteral(shorthand);
        }
        Run(c, false);
        auto batch = Query(Results(c));
        Inspect(c, *batch);
        const auto comparable = Fingerprint(*Query(ComparableResults(c)), false);
        if (comparable != expected) {
            std::ofstream(root + "/csv-expected.txt") << expected;
            std::ofstream(root + "/csv-actual-" + target + ".txt") << comparable;
            std::cerr << "CSV comparison evidence: " << root << '\n';
        }
        Check(comparable == expected, "CSV/database numeric results differ");
        c.results = Fingerprint(*batch);
        c.checkpoint = Checkpoint(c);
        cases->push_back(c);
        if (std::string(target) == "sqlite") {
            for (bool schema_change : {false, true}) {
                auto changed = original->Slice(0, original->num_rows() - 1);
                if (schema_change)
                    changed = original->ReplaceSchemaMetadata(arrow::key_value_metadata({"t3"}, {"changed"}));
                DataFrame replacement;
                replacement.FromArrow(changed);
                Check(channel->Write(&replacement) == 0, "replace CSV snapshot");
                const auto sql =
                    "SELECT * FROM " + source + " USING explore.baseliner WITH config='" + c.ref + "' INTO " + c.target;
                std::string response;
                const int rc =
                    Route("/scheduler/batch/execute")("/scheduler/batch/execute", Json({{"sql", sql}}), response);
                Check(rc != 0 && response.find("fingerprint mismatch") != std::string::npos,
                      "changed CSV restored without fingerprint rejection: " + response);
                Check(Checkpoint(c) == c.checkpoint && Fingerprint(*Query(Results(c))) == c.results,
                      "rejected CSV changed model/results");
            }
            DataFrame restore;
            restore.FromArrow(original);
            Check(channel->Write(&restore) == 0, "restore CSV source");
            Run(c, true);
        }
        std::cout << "PASS real CSV -> " << target << " Value/Ratio/Relation+forecast matches database" << std::endl;
    }
    rapidjson::Document scalar;
    scalar.Parse(FrameConfiguration(source, "csv-scalar-" + unique).c_str());
    scalar["datasets"][0]["metrics"].PopBack();
    rapidjson::StringBuffer scalar_text;
    rapidjson::Writer<rapidjson::StringBuffer> scalar_writer(scalar_text);
    scalar.Accept(scalar_writer);
    const auto scalar_config = std::string(scalar_text.GetString(), scalar_text.GetSize());
    auto scalar_result =
        Execute("SELECT * FROM " + source + " USING explore.baseliner WITH parameters=" + SqlLiteral(scalar_config) +
                " INTO dataframe.csv_results");
    Check(std::string(scalar_result["status"].GetString()) == "completed" && scalar_result["rows"].GetInt64() == 240,
          "single Schema DataFrame baseline output");
    Check(Query("SELECT * FROM dataframe.csv_results")->num_rows() == 240, "DataFrame result registration");
    Check(channel->Read(&frame) == 0 && Fingerprint(*frame.ToArrow()) == before,
          "CSV source content/Schema/order changed");
    const auto empty_upload = root + "/empty_upload.csv";
    std::ofstream(empty_upload) << "bucket,domain,port,bytes,packets,keep\n";
    imported = Call("/channels/dataframe/import", Json({{"filename", "t3_empty.csv"}, {"tmp_path", empty_upload}}));
    Check(imported["rows"].GetInt() == 0 && imported["schema"].Size() == 6, "header-only CSV lost Schema");
    Case empty;
    empty.source = "dataframe." + std::string(imported["name"].GetString());
    empty.target = "sqlite.t73sqlite";
    empty.key = "csv-empty-" + unique;
    empty.poll = false;
    empty.empty = true;
    empty.config = FrameConfiguration(empty.source, empty.key);
    empty.ref = Publish("csvempty", empty.config);
    Run(empty, false);
    Inspect(empty, *Query(Results(empty)));
    empty.results = Fingerprint(*Query(Results(empty)));
    empty.checkpoint = Checkpoint(empty);
    cases->push_back(empty);
    std::cout << "PASS header-only CSV publishes recoverable empty snapshot" << std::endl;
}
void ModelOutputCases(const std::string& unique, const std::vector<Case>& cases) {
    auto* registry = static_cast<IChannelRegistry*>(PluginLoader::Single()->First(IID_CHANNEL_REGISTRY));
    auto frame = [&](const std::string& name) {
        auto channel = std::dynamic_pointer_cast<IDataFrameChannel>(registry->Get(name.c_str()));
        Check(bool(channel), "missing model DataFrame " + name);
        DataFrame data;
        Check(channel->Read(&data) == 0, "read model DataFrame");
        auto batch = data.ToArrow();
        Check(batch && batch->schema()->Equals(*MakeSchema(SchemaKind::kModelParameters), true), "model output Schema");
        return batch;
    };
    auto text = [](const rapidjson::Value& doc) {
        rapidjson::StringBuffer b;
        rapidjson::Writer<rapidjson::StringBuffer> w(b);
        doc.Accept(w);
        return std::string(b.GetString(), b.GetSize());
    };
    auto inspect = [&](const arrow::RecordBatch& batch, int64_t expected) {
        Check(batch.num_rows() == expected, "final model identity count");
        for (int64_t row = 0; row < batch.num_rows(); ++row) {
            auto p = batch.GetColumnByName("parameters_json")->GetScalar(row).ValueOrDie();
            Check(p->is_valid, "trained final model missing parameters");
            rapidjson::Document params;
            params.Parse(p->ToString().c_str());
            Check(!params.HasParseError() && params["parameters_version"].GetUint() == 1, "model parameters version");
            Check(params.HasMember("rolling") || params.HasMember("routed"), "actual parameters missing");
            Check(!params.HasMember("checkpoint"), "checkpoint exported as model");
        }
    };
    auto multi = std::find_if(cases.begin(), cases.end(), [](const auto& c) { return !c.poll && c.multiple == 2; });
    Check(multi != cases.end(), "multi dataset baseline missing");
    for (bool exact : {false, true}) {
        rapidjson::Document doc;
        doc.Parse(multi->config.c_str());
        rapidjson::Value second;
        second.CopyFrom(doc["datasets"][0], doc.GetAllocator());
        second["id"].SetString("npm-secondary", doc.GetAllocator());
        doc["datasets"].PushBack(second, doc.GetAllocator());
        const auto key = "t4-multi-" + unique + (exact ? "-exact" : "-inline");
        doc["task_key"].SetString(key.c_str(), doc.GetAllocator());
        const auto config = text(doc);
        const auto with =
            exact ? "config=" + SqlLiteral(Publish("modelmulti", config)) : "parameters=" + SqlLiteral(config);
        const auto dest = exact ? "multi_models_exact" : "multi_models_inline";
        auto result = Execute("SELECT * FROM " + multi->source + " USING explore.baseliner WITH " + with +
                              ",model_output='dataframe." + dest + "' INTO " + multi->target);
        Check(std::string(result["status"].GetString()) == "completed", "multi model completion");
        auto batch = frame(dest);
        Check(batch->num_rows() == 6, "multi dataset model count");
        std::set<std::string> datasets;
        auto ids = std::static_pointer_cast<arrow::StringArray>(batch->GetColumnByName("dataset_id"));
        for (int64_t row = 0; row < batch->num_rows(); ++row) datasets.insert(ids->GetString(row));
        Check(datasets.size() == 2, "two-part model export became single dataset");
        ConfigSnapshot original, resolved;
        Check(ParseConfig(config, &original).ok(), "parse model config");
        Check(original.sha256_hex == result["result"]["config_hash"].GetString(), "model target changed config hash");
        auto checkpoint = Query("SELECT checkpoint FROM " + multi->target +
                                ".baseline_model_versions WHERE task_key='" + key + "' ORDER BY generation");
        Check(checkpoint->num_rows() > 0, "old managed checkpoint missing");
    }
    auto csv = std::find_if(cases.begin(), cases.end(),
                            [](const auto& c) { return !c.empty && c.source.compare(0, 10, "dataframe.") == 0; });
    Check(csv != cases.end(), "CSV baseline missing");
    for (const char* backend : {"sqlite", "mysql", "postgres"}) {
        rapidjson::Document doc;
        doc.Parse(csv->config.c_str());
        const auto key = "t4-db-" + unique + "-" + backend;
        doc["task_key"].SetString(key.c_str(), doc.GetAllocator());
        const std::string target = std::string(backend) + ".t73" + backend + ".t4_models";
        const std::string sql = "SELECT * FROM " + csv->source +
                                " USING explore.baseliner WITH parameters=" + SqlLiteral(text(doc)) +
                                ",model_output=" + SqlLiteral(target) + " INTO sqlite.t73sqlite";
        auto result = Execute(sql);
        Check(std::string(result["status"].GetString()) == "completed" &&
                  std::string(result["result"]["model_output"]["status"].GetString()) == "committed",
              "model database completion");
        auto batch = Query("SELECT * FROM " + target + " WHERE task_key='" + key + "' ORDER BY metric_id");
        inspect(*batch, 3);
        Check(result["result"]["model_output"]["rows_written"].GetInt64() == 3, "model row count summary");
        // Restore old managed generation and upsert identical final logical model rows.
        auto before = Fingerprint(*batch, false);
        Execute(sql);
        Check(Fingerprint(*Query("SELECT * FROM " + target + " WHERE task_key='" + key + "' ORDER BY metric_id"),
                          false) == before,
              "model logical replay is not idempotent");
        std::cout << "PASS CSV final three-kind model export and replay to " << backend << std::endl;
    }
    // Same database channel, a different source relation is permitted.
    auto three = std::find_if(cases.begin(), cases.end(), [](const auto& c) {
        return !c.poll && c.source.compare(0, 7, "sqlite.") == 0 &&
               std::count(c.source.begin(), c.source.end(), '.') == 2;
    });
    Check(three != cases.end(), "three-part baseline missing");
    auto result =
        Execute("SELECT * FROM " + three->source + " USING explore.baseliner WITH config=" + SqlLiteral(three->ref) +
                ",model_output='sqlite.t73sqlite.t4_same_db_models' INTO " + three->target);
    Check(std::string(result["status"].GetString()) == "completed", "same database different model table rejected");
    auto scalar_config = FrameConfiguration(csv->source, "t4-df-" + unique);
    rapidjson::Document scalar;
    scalar.Parse(scalar_config.c_str());
    scalar["datasets"][0]["metrics"].Erase(scalar["datasets"][0]["metrics"].Begin() + 2);
    scalar_config = text(scalar);
    const std::string base =
        "SELECT * FROM " + csv->source + " USING explore.baseliner WITH parameters=" + SqlLiteral(scalar_config);
    result = Execute(base + ",model_output='dataframe.t4_csv_models' INTO dataframe.t4_csv_results");
    Check(result["rows"].GetInt64() == 240, "model export changed primary forecast/evaluation rows");
    inspect(*frame("t4_csv_models"), 2);
    auto sentinel = Fingerprint(*frame("t4_csv_models"));
    auto reject = [&](const std::string& sql) {
        std::string response;
        Check(Route("/scheduler/batch/execute")("/scheduler/batch/execute", Json({{"sql", sql}}), response) != 0,
              "invalid model output accepted");
        return response;
    };
    // A model-table failure follows a successful old managed publication. Report both truthfully.
    auto* leases = static_cast<IDatabaseChannelLeaseProviderV1*>(
        PluginLoader::Single()->First(IID_DATABASE_CHANNEL_LEASE_PROVIDER_V1));
    auto sqlite = leases->AcquireChannel("sqlite", "t73sqlite");
    Check(bool(sqlite), "model failure fixture channel");
    const auto failed_key = "t4-managed-fail-" + unique;
    const auto model_before =
        Fingerprint(*Query("SELECT * FROM sqlite.t73sqlite.t4_models ORDER BY logical_key"), false);
    Check(sqlite->ExecuteSql(
              ("CREATE UNIQUE INDEX t4_fail_bucket ON t4_models(as_of_bucket) WHERE task_key='" + failed_key + "'")
                  .c_str()) >= 0,
          "model failure constraint fixture");
    rapidjson::Document managed_config;
    managed_config.Parse(csv->config.c_str());
    managed_config["task_key"].SetString(failed_key.c_str(), managed_config.GetAllocator());
    const auto managed_json = text(managed_config);
    const auto managed_failure =
        reject("SELECT * FROM " + csv->source + " USING explore.baseliner WITH parameters=" + SqlLiteral(managed_json) +
               ",model_output='sqlite.t73sqlite.t4_models' INTO sqlite.t73sqlite");
    rapidjson::Document managed_summary;
    managed_summary.Parse(managed_failure.c_str());
    Check(managed_summary.HasMember("result") && managed_summary["result"]["generation"].GetInt64() > 0 &&
              managed_summary["result"]["rows_written"].GetInt64() > 0 &&
              std::string(managed_summary["result"]["model_output"]["status"].GetString()) == "failed" &&
              managed_summary["result"]["model_output"]["rows_written"].GetInt64() == 0,
          "model failure hid the committed old generation: " + managed_failure);
    auto committed = Query("SELECT checkpoint FROM sqlite.t73sqlite.baseline_model_versions WHERE task_key='" +
                           failed_key + "' ORDER BY generation");
    Check(committed->num_rows() > 0 &&
              !committed->column(0)->GetScalar(committed->num_rows() - 1).ValueOrDie()->ToString().empty(),
          "model failure lost the committed old checkpoint");
    Check(Fingerprint(*Query("SELECT * FROM sqlite.t73sqlite.t4_models ORDER BY logical_key"), false) == model_before,
          "model transaction failure changed existing parameters");
    std::cout << "PASS model output failure reports and preserves the committed managed generation" << std::endl;
    reject(base + ",model_output=" + SqlLiteral(csv->source) + " INTO dataframe.t4_csv_results");
    reject(base + ",model_output='dataframe.t4_csv_results' INTO dataframe.t4_csv_results");
    reject("SELECT * FROM " + three->source + " USING explore.baseliner WITH config=" + SqlLiteral(three->ref) +
           ",model_output=" + SqlLiteral(three->source) + " INTO " + three->target);
    scalar["persistence"].AddMember("max_checkpoint_bytes", 32, scalar.GetAllocator());
    const auto failed =
        reject("SELECT * FROM " + csv->source + " USING explore.baseliner WITH parameters=" + SqlLiteral(text(scalar)) +
               ",model_output='dataframe.t4_csv_models' INTO dataframe.t4_failed_results");
    Check(Fingerprint(*frame("t4_csv_models")) == sentinel && !registry->Get("t4_failed_results"),
          "failed model output replaced an existing DataFrame");
    rapidjson::Document failure;
    failure.Parse(failed.c_str());
    Check(failure.HasMember("result") &&
              std::string(failure["result"]["model_output"]["status"].GetString()) == "failed" &&
              failure["result"]["model_output"]["rows_written"].GetInt64() == 0,
          "model failure status missing");
    auto empty = std::find_if(cases.begin(), cases.end(),
                              [](const auto& c) { return c.empty && c.source.compare(0, 10, "dataframe.") == 0; });
    Check(empty != cases.end(), "empty CSV missing");
    Execute("SELECT * FROM " + empty->source + " USING explore.baseliner WITH config=" + SqlLiteral(empty->ref) +
            ",model_output='dataframe.t4_empty_models' INTO " + empty->target);
    Check(frame("t4_empty_models")->num_rows() == 0, "empty model lost Schema");
    std::cout
        << "PASS two-part multi dataset inline/exact, three-part, CSV/DataFrame model outputs and failure preservation"
        << std::endl;
}
void ResultOutputCases(const std::string& root, const std::string& unique, const std::vector<Case>& cases) {
    auto* registry = static_cast<IChannelRegistry*>(PluginLoader::Single()->First(IID_CHANNEL_REGISTRY));
    auto* leases = static_cast<IDatabaseChannelLeaseProviderV1*>(
        PluginLoader::Single()->First(IID_DATABASE_CHANNEL_LEASE_PROVIDER_V1));
    auto csv = std::find_if(cases.begin(), cases.end(),
                            [](const auto& c) { return !c.empty && c.source.compare(0, 10, "dataframe.") == 0; });
    auto two =
        std::find_if(cases.begin(), cases.end(), [](const auto& c) { return !c.poll && c.source == "mysql.t73mysql"; });
    auto three = std::find_if(cases.begin(), cases.end(), [](const auto& c) {
        return !c.poll && c.source == "mysql.t73mysql.npm_basic_history_v1";
    });
    Check(csv != cases.end() && two != cases.end() && three != cases.end(), "T5 input fixtures");
    auto comparable = [](const std::string& target, const std::string& key) {
        return "SELECT metric_id,result_kind,target_bucket,issued_after_bucket,observed,expected,lower,upper,band_kind "
               "FROM " +
               target + " WHERE task_key='" + key +
               "' ORDER BY "
               "metric_id,result_kind,target_bucket,issued_after_bucket,observed,expected,lower,upper,band_kind";
    };
    const auto numeric_rows = [](const arrow::RecordBatch& batch) {
        std::vector<std::string> rows;
        for (int64_t row = 0; row < batch.num_rows(); ++row) {
            std::string encoded;
            for (const auto& column : batch.columns()) {
                auto scalar = column->GetScalar(row).ValueOrDie();
                std::string value = scalar->is_valid ? scalar->ToString() : "NULL";
                if (scalar->is_valid && scalar->type->id() == arrow::Type::DOUBLE) {
                    std::ostringstream number;
                    number << std::setprecision(12) << static_cast<arrow::DoubleScalar&>(*scalar).value;
                    value = number.str();
                }
                encoded += std::to_string(value.size()) + ":" + value;
            }
            rows.push_back(encoded);
        }
        std::sort(rows.begin(), rows.end());
        return rows;
    };
    const auto result_batch = [&](const std::string& target, const std::string& key, bool comparable_columns) {
        if (target.compare(0, 10, "dataframe.") != 0)
            return Query(comparable_columns
                             ? comparable(target, key)
                             : "SELECT * FROM " + target +
                                   " ORDER BY metric_id,target_bucket,result_kind,issued_after_bucket,summary_id");
        auto channel = std::dynamic_pointer_cast<IDataFrameChannel>(registry->Get(target.substr(10).c_str()));
        DataFrame frame;
        Check(channel && channel->Read(&frame) == 0, "read specified result DataFrame");
        auto batch = frame.ToArrow();
        if (!comparable_columns) return batch;
        std::vector<int> columns;
        for (const char* field : {"metric_id", "result_kind", "target_bucket", "issued_after_bucket", "observed",
                                  "expected", "lower", "upper", "band_kind"})
            columns.push_back(batch->schema()->GetFieldIndex(field));
        return batch->SelectColumns(columns).ValueOrDie();
    };
    auto reject = [&](const std::string& sql) {
        std::string response;
        Check(Route("/scheduler/batch/execute")("/scheduler/batch/execute", Json({{"sql", sql}}), response) != 0,
              "invalid specified output accepted: " + sql);
        rapidjson::Document d;
        d.Parse(response.c_str());
        Check(!d.HasParseError(), response);
        return d;
    };
    auto state = [&](const rapidjson::Document& d, const char* output, const char* expected, int64_t rows) {
        Check(d.HasMember("result") && d["result"].HasMember(output) &&
                  std::string(d["result"][output]["status"].GetString()) == expected &&
                  (rows < 0 ? d["result"][output]["rows_written"].GetInt64() > 0
                            : d["result"][output]["rows_written"].GetInt64() == rows),
              "T5 output status/count");
    };
    for (const auto* c : {&*csv, &*two, &*three}) {
        const auto expected = numeric_rows(*Query(ComparableResults(*c)));
        const auto source_before = c == &*csv ? Fingerprint(*Query("SELECT * FROM " + c->source), true) : "";
        for (const char* backend : {"sqlite", "mysql", "postgres", "dataframe"}) {
            const std::string tag = c == &*csv ? "csv" : c == &*two ? "two" : "three";
            const std::string target = std::string(backend) == "dataframe"
                                           ? "dataframe.t5_" + tag
                                           : std::string(backend) + ".t73" + backend + ".t5_" + tag;
            const std::string model = std::string(backend) == "dataframe"
                                          ? "dataframe.t5_models_" + tag
                                          : std::string(backend) + ".t73" + backend + ".t5_models_" + tag;
            const auto sql = "SELECT * FROM " + c->source +
                             " USING explore.baseliner WITH config=" + SqlLiteral(c->ref) +
                             ",model_output=" + SqlLiteral(model) + " INTO " + target;
            auto completed = Execute(sql);
            Check(std::string(completed["status"].GetString()) == "completed", "specified output completed");
            state(completed, "results_output", "committed", -1);
            state(completed, "model_output", "committed", 3);
            Check(numeric_rows(*result_batch(target, c->key, true)) == expected, "specified output numeric parity");
            const auto before = numeric_rows(*result_batch(target, c->key, false));
            completed = Execute(sql);
            state(completed, "results_output", "committed", -1);
            Check(numeric_rows(*result_batch(target, c->key, false)) == before, "specified output replay changed rows");
            if (std::string(backend) == "dataframe") {
                auto channel = std::dynamic_pointer_cast<IDataFrameChannel>(registry->Get(("t5_" + tag).c_str()));
                DataFrame frame;
                Check(channel && channel->Read(&frame) == 0 &&
                          frame.ToArrow()->schema()->Equals(*MakeSchema(SchemaKind::kResults), true),
                      "Relation fusion/model Schema leaked to results");
            } else {
                // Another task shares this physical table; its rows must survive the original task's replay.
                auto other =
                    Execute("SELECT * FROM " + csv->source + " USING explore.baseliner WITH parameters=" +
                            SqlLiteral(FrameConfiguration(csv->source, "t5-other-" + unique)) + " INTO " + target);
                state(other, "results_output", "committed", -1);
                const auto unrelated = Fingerprint(*Query(comparable(target, "t5-other-" + unique)), false);
                Execute(sql);
                Check(Fingerprint(*Query(comparable(target, "t5-other-" + unique)), false) == unrelated,
                      "specified table cleared unrelated rows");
            }
            std::cout << "PASS T5 " << tag << " -> " << backend
                      << " results/models, parity, replay, preserve, Schema separation" << std::endl;
        }
        if (c == &*csv)
            Check(Fingerprint(*Query("SELECT * FROM " + c->source), true) == source_before,
                  "CSV source changed by outputs");
    }
    // Both outputs must roll back after the second table fails, on every supported backend.
    for (const char* backend : {"sqlite", "mysql", "postgres"}) {
        auto channel = leases->AcquireChannel(backend, ("t73" + std::string(backend)).c_str());
        const std::string prefix = std::string(backend) + ".t73" + backend;
        const auto results = prefix + ".t5_atomic_results", models = prefix + ".t5_atomic_models";
        const auto seed =
            "SELECT * FROM " + csv->source + " USING explore.baseliner WITH config=" + SqlLiteral(csv->ref);
        Execute(seed + " INTO " + results);
        Execute(seed + ",model_output=" + SqlLiteral(models) + " INTO dataframe.t5_seed");
        // Per-task partial index on SQLite; all rows have the same final bucket so a fresh model table is used
        // elsewhere.
        const std::string failing = "t5_fail_models";
        ModelOutputStore fixture;
        ConfigSnapshot config;
        Check(ParseConfig(csv->config, &config).ok() && fixture.Open(config, channel, failing) == 0,
              "joint failure model Schema fixture");
        fixture.Close();
        Check(channel->ExecuteSql(("CREATE UNIQUE INDEX t5_fail_unique ON " + failing + "(as_of_bucket)").c_str()) >= 0,
              "joint failure unique fixture");
        const auto before = Fingerprint(*Query("SELECT * FROM " + results + " ORDER BY logical_key"), false);
        const auto sql = seed +
                         ",model_output=" + SqlLiteral(std::string(backend) + ".t5alias" + backend + "." + failing) +
                         " INTO " + results;
        auto failed = reject(sql);
        state(failed, "results_output", "failed", 0);
        state(failed, "model_output", "failed", 0);
        Check(Fingerprint(*Query("SELECT * FROM " + results + " ORDER BY logical_key"), false) == before &&
                  Query("SELECT COUNT(*) AS empty_result_count FROM " + prefix + "." + failing)
                          ->column(0)
                          ->GetScalar(0)
                          .ValueOrDie()
                          ->ToString() == "0",
              "joint failure left partial data");
        std::cout << "PASS T5 production joint transaction rollback " << backend << std::endl;
    }
    Check(Query("SELECT * FROM " + csv->target + ".baseline_relation_fusion_v1 WHERE task_key='" + csv->key + "'")
                  ->num_rows() > 0,
          "old managed Relation fusion missing");
    const auto seed = "SELECT * FROM " + csv->source + " USING explore.baseliner WITH config=" + SqlLiteral(csv->ref);
    auto mixed =
        reject(seed + ",model_output='sqlite.t73sqlite.t5_fail_models' INTO postgres.t73postgres.t5_mixed_results");
    state(mixed, "results_output", "committed", -1);
    state(mixed, "model_output", "failed", 0);
    Check(Query("SELECT * FROM postgres.t73postgres.t5_mixed_results")->num_rows() > 0,
          "mixed failure lost committed results");
    std::filesystem::create_directory(root + "/t5_fail_df_models.csv");
    mixed = reject(seed + ",model_output='dataframe.t5_fail_df_models' INTO sqlite.t73sqlite.t5_mixed_df_results");
    state(mixed, "results_output", "committed", -1);
    state(mixed, "model_output", "failed", 0);
    Check(!registry->Get("t5_fail_df_models"), "failed model registration appeared committed");
    std::filesystem::create_directory(root + "/t5_fail_df_results.csv");
    mixed = reject(seed + ",model_output='mysql.t73mysql.t5_mixed_models' INTO dataframe.t5_fail_df_results");
    state(mixed, "results_output", "failed", 0);
    state(mixed, "model_output", "committed", 3);
    Check(
        !registry->Get("t5_fail_df_results") && Query("SELECT * FROM mysql.t73mysql.t5_mixed_models")->num_rows() == 3,
        "result registration failure misreported committed model");
    auto empty = std::find_if(cases.begin(), cases.end(),
                              [](const auto& c) { return c.empty && c.source.compare(0, 10, "dataframe.") == 0; });
    Check(empty != cases.end(), "empty fixture");
    Execute("SELECT * FROM " + empty->source + " USING explore.baseliner WITH config=" + SqlLiteral(empty->ref) +
            ",model_output='dataframe.t5_empty_models' INTO dataframe.t5_empty_results");
    auto channel = std::dynamic_pointer_cast<IDataFrameChannel>(registry->Get("t5_empty_results"));
    DataFrame frame;
    Check(channel && channel->Read(&frame) == 0 && frame.RowCount() == 0 &&
              frame.ToArrow()->schema()->Equals(*MakeSchema(SchemaKind::kResults), true),
          "empty results lost typed Schema");
    reject(seed + " INTO " + csv->source);
    reject(seed + ",model_output='sqlite.t73sqlite.t5_csv' INTO sqlite.t73sqlite.t5_csv");
    reject("SELECT * FROM " + three->source + " USING explore.baseliner WITH config=" + SqlLiteral(three->ref) +
           " INTO " + three->source);
    auto poll = std::find_if(cases.begin(), cases.end(), [](const auto& c) { return c.poll; });
    Check(poll != cases.end(), "poll fixture");
    reject("SELECT * FROM " + poll->source + " USING explore.baseliner WITH config=" + SqlLiteral(poll->ref) +
           " INTO sqlite.t73sqlite.t5_poll");
    std::cout << "PASS T5 mixed failures report committed rows; empty results; source/overlap/poll rejection"
              << std::endl;
}

}  // namespace
int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    QueryOwnership();
    if (std::getenv("BASELINER_QUERY_OWNERSHIP_ONLY")) return 0;
    const char* path = std::getenv("BASELINER_PRODUCTION_DB_OPTIONS");
    if (!path) path = std::getenv("BASELINER_TEST_DB_OPTIONS");
    assert(path);
    std::ifstream input(path);
    std::ostringstream o;
    o << input.rdbuf();
    assert(!o.str().empty());
    auto unique = std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    auto root = "/tmp/baseline-operator-t73/production-" + unique;
    std::filesystem::create_directories(root);
    std::ofstream(root + "/stream.yml") << "stream:\n  channels: []\n";
    Pcap(root + "/input.pcap");
    const auto isolated = Isolate(o.str(), root, unique);
    std::vector<Case> cases;
    std::map<std::string, std::string> sources, catalogs;
    {
        Environment environment(isolated, root);
        for (auto source : {"sqlite", "mysql", "postgres", "clickhouse"}) {
            auto name = "t73_" + std::string(source);
            std::string request = R"({"type":"pcapfile","name":")" + name + R"(","role":"source","options":{"path":")" +
                                  root + R"(/input.pcap","format":"pcap","batch_packets":32}})";
            Call("/channels/stream/add", request);
            auto exact = std::string(source) + ".t73" + source;
            auto result = Execute("SELECT * FROM pcapfile." + name +
                                  " USING npm.basic WITH "
                                  "input_namespace='t73-production',source_domains='0:1',features='basic',udp_idle_"
                                  "timeout_ns=120000000000,output_"
                                  "interval_ns=60000000000 INTO " +
                                  exact);
            const std::string run = result["result"]["run_id"].GetString();
            environment.capture.pcap_name = name;
            const auto poll_run = CaptureRun(exact, "producer-" + std::string(source) + unique);
            sources[exact + "|" + poll_run] =
                Fingerprint(*Query("SELECT * FROM " + exact + ".npm_basic_history_v1 WHERE __npm_run_id='" + poll_run +
                                   "' ORDER BY session_id,revision"));
            auto before = Query("SELECT * FROM " + exact + ".npm_basic_history_v1 WHERE __npm_run_id='" + run +
                                "' ORDER BY session_id,revision");
            Check(before->num_rows() >= 120, "real NPM SQL rows");
            sources[exact + "|" + run] = Fingerprint(*before);
            catalogs[exact] = Fingerprint(
                *Query("SELECT * FROM " + exact + ".npm_result_entities ORDER BY entity_id,schema_version"));
            for (auto target : {"sqlite", "mysql", "postgres"})
                for (bool poll : {false, true}) {
                    Case c;
                    c.source = exact;
                    c.target = std::string(target) + ".t73" + target;
                    c.poll = poll;
                    c.key = "sql-" + unique + "-" + source + "-" + target + (poll ? "-poll" : "-snapshot");
                    c.config = Configuration(exact, poll ? poll_run : run, c.key, poll);
                    c.ref = Publish("c" + std::to_string(cases.size()), c.config);
                    Run(c, false);
                    auto batch = Query(Results(c));
                    Inspect(c, *batch);
                    c.results = Fingerprint(*batch);
                    c.checkpoint = Checkpoint(c);
                    cases.push_back(c);
                    std::cout << "PASS production NPM " << source << " -> " << target << (poll ? " poll" : " snapshot")
                              << " Value/Ratio/Relation+forecast" << std::endl;
                }
            ThreePartCases(exact, run, poll_run, unique, &cases);
            if (std::string(source) == "sqlite")
                for (bool poll : {false, true}) {
                    Case c;
                    c.source = exact;
                    c.target = exact;
                    c.poll = poll;
                    c.multiple = 2;
                    c.key = "sql-" + unique + (poll ? "-m2-poll" : "-m2-snapshot");
                    c.config = Configuration(exact, poll ? poll_run : run, c.key, poll, 2);
                    c.ref = Publish("c" + std::to_string(cases.size()), c.config);
                    Run(c, false);
                    auto b = Query(Results(c));
                    Inspect(c, *b);
                    c.results = Fingerprint(*b);
                    c.checkpoint = Checkpoint(c);
                    cases.push_back(c);
                }
        }
        DataFrameCases(root, unique, &cases);
        ModelOutputCases(unique, cases);
        ResultOutputCases(root, unique, cases);
    }
    // Real StopAll/Unload/Load, no model service or Arrow products survive the library owner.
    {
        Environment restarted(isolated, root);
        for (auto& c : cases) {
            Run(c, true);
            auto b = Query(Results(c));
            Inspect(c, *b);
            auto actual = Fingerprint(*b);
            if (actual != c.results) {
                std::ofstream(root + "/before-results.txt") << c.results;
                std::ofstream(root + "/after-results.txt") << actual;
            }
            Check(actual == c.results, "restart logical results changed: " + c.key);
            Check(Checkpoint(c) == c.checkpoint, "restart model checkpoint changed: " + c.key);
        }
        for (const auto& catalog : catalogs)
            Check(Fingerprint(*Query("SELECT * FROM " + catalog.first +
                                     ".npm_result_entities ORDER BY entity_id,schema_version")) == catalog.second,
                  "NPM catalog schema fingerprint changed");
        for (const auto& source : sources) {
            auto p = source.first.find('|');
            auto exact = source.first.substr(0, p), run = source.first.substr(p + 1);
            auto b = Query("SELECT * FROM " + exact + ".npm_basic_history_v1 WHERE __npm_run_id='" + run +
                           "' ORDER BY session_id,revision");
            Check(Fingerprint(*b) == source.second, "source contents/schema changed");
        }
    }
    std::cout << "PASS production matrix " << cases.size() << " cases and " << cases.size()
              << " task recoveries after plugin reload; source schema/content preserved" << std::endl;
}
