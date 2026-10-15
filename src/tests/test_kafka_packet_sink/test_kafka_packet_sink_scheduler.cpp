// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <common/error_code.h>
#include <framework/interfaces/iblock_stream_factory.h>
#include <framework/interfaces/iblock_stream_reader.h>
#include <framework/interfaces/iblock_stream_schema.h>
#include <framework/interfaces/irouter_handle.h>
#include <tests/common/test_environment.h>
#include <common/loader.hpp>
#include <filesystem>
#include <iostream>
#include "packet_sink_test_support.h"
using namespace packet_sink_test;
namespace {
struct State {
    std::atomic<int> polls{0}, releases{0}, cancels{0}, flushes{0};
    bool schema_bad = false, oversize = false, missing = false, error = false, continuous = false,
         stale_counters = false;
};
template <class Base>
struct Reader final : Base, IBlockStreamSchemaV1 {
    std::shared_ptr<State> state;
    bool finished = false;
    explicit Reader(std::shared_ptr<State> s) : state(std::move(s)) {}
    const char* Category() override { return "netadapter"; }
    const char* Name() override { return "edge"; }
    const char* Type() override { return ChannelType::kBlockStream; }
    const char* Schema() override { return "packet"; }
    int Open() override { return 0; }
    int Close() override { return 0; }
    int Flush() override {
        ++state->flushes;
        finished = true;
        return 0;
    }
    bool IsOpened() const override { return true; }
    bool IsFinished() const override { return finished; }
    void Cancel() override {
        ++state->cancels;
        finished = true;
    }
    std::shared_ptr<arrow::Schema> ArrowSchema() const override {
        return state->schema_bad ? arrow::schema({}) : packet::PacketFamilySchema();
    }
    int DescribeSources(CaptureSourceSetV2* s) const {
        *s = Sources();
        if (state->oversize) s->limits.max_bytes_per_batch = 2097152;
        return 0;
    }
    int ReadInputCounters(uint32_t id, CaptureCountersV1* c) const {
        c->generation = state->stale_counters ? 10 : 9;
        c->available_mask = kCaptureDeliveredPacketsAvailable | kCaptureQueueDroppedPacketsAvailable;
        c->delivered_packets = id == 0 && state->polls ? 3 : 0;
        c->queue_dropped_packets = 2;
        return 0;
    }
    BlockPollEvent PollBlock(int ms) override { return PollCapture(ms).block; }
    CapturePollEventV2 PollCapture(int) {
        if (finished) {
            CapturePollEventV2 e;
            e.block.kind = BlockPollEvent::kEof;
            return e;
        }
        const int n = state->polls++;
        if (n == 0) return Event();
        if (state->error) {
            CapturePollEventV2 e;
            e.block.kind = BlockPollEvent::kError;
            e.block.err = EIO;
            return e;
        }
        if (n == 1) return Event(2, 0);
        CapturePollEventV2 e;
        if (!state->continuous)
            e.block.kind = BlockPollEvent::kEof;
        else
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return e;
    }
    int ReleaseBlock(const std::shared_ptr<arrow::RecordBatch>& b) override {
        assert(b && b->num_rows() == 3);
        ++state->releases;
        return 0;
    }
};
struct Source final : IBlockStreamFactory, IBlockStreamReaderFactoryV1 {
    std::shared_ptr<State> state = std::make_shared<State>();
    Reader<IBlockStreamChannel> definition{state};
    int creates = 0;
    IBlockStreamChannel* Get(const char* type, const char* name) override {
        return std::string(type) == "netadapter" && std::string(name) == "edge" ? &definition : nullptr;
    }
    void List(std::function<void(const char*, const char*, IBlockStreamChannel*)>) override {}
    int CreateReader(const BlockStreamReaderConfigV1& c, IBlockStreamChannel** out) override {
        *out = nullptr;
        if (std::string(c.source_category) != "netadapter") return ENOTSUP;
        ++creates;
        if (state->missing)
            *out = new Reader<IBlockStreamChannel>(state);
        else
            *out = new Reader<ICaptureBlockStreamReaderV2>(state);
        return 0;
    }
    void ReleaseReader(IBlockStreamChannel* r) override {
        r->Cancel();
        delete r;
    }
    void Reset() {
        state = std::make_shared<State>();
        creates = 0;
    }
};
struct Factory final : IPacketSinkFactoryV1, IKafkaChannelCatalogV1 {
    std::shared_ptr<FakeSession> session = std::make_shared<FakeSession>();
    bool available = true;
    int DescribeChannel(const char*, KafkaChannelDescriptionV1* d) override {
        d->role = "sink";
        d->sink_adapter_available = true;
        return 0;
    }
    int CreateWriter(const PacketSinkContextV1& c, std::unique_ptr<IPacketSinkWriterV1>* out) override {
        out->reset();
        if (!available) return ENOTSUP;
        *out = std::make_unique<PacketWriter>(Config(), c, session);
        return 0;
    }
    void Reset() {
        session = std::make_shared<FakeSession>();
        available = true;
    }
};
Json Call(PluginLoader* loader, const std::string& path, const rapidjson::Value& request, int expected = 0) {
    fnRouterHandler f;
    loader->Traverse(IID_ROUTER_HANDLE, [&](void* p) {
        static_cast<IRouterHandle*>(p)->EnumRoutes([&](const RouteItem& r) {
            if (r.uri == path && r.method == "POST") f = r.handler;
        });
        return f ? -1 : 0;
    });
    assert(f);
    std::string response;
    int rc = f(path, Encode(request), response);
    if (rc != expected) std::cerr << path << " rc=" << rc << " response=" << response << '\n';
    assert(rc == expected);
    return Parse(response);
}
Json Id(const std::string& id) {
    auto j = Parse("{}");
    String(j, "runtime_task_id", id, j.GetAllocator());
    return j;
}
Json Terminal(PluginLoader* loader, const std::string& id) {
    for (int i = 0; i < 2000; ++i) {
        auto j = Call(loader, "/scheduler/batch/status", Id(id));
        auto s = Text(j, "status");
        if (s == "failed" || s == "completed" || s == "cancelled" || s == "stopped") return j;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    assert(false);
    return Parse("{}");
}
void Submit(PluginLoader* loader, const std::string& id,
            const std::string& sql = "SELECT * FROM netadapter.edge INTO kafka.packets") {
    auto j = Id(id);
    String(j, "sql_text", sql, j.GetAllocator());
    Call(loader, "/scheduler/batch/submit", j);
}
}  // namespace
int main() {
    Factory factory;
    Source source;
    auto* loader = PluginLoader::Single();
    loader->Regist(IID_PACKET_SINK_FACTORY_V1, &factory);
    loader->Regist(IID_KAFKA_CHANNEL_CATALOG_V1, static_cast<IKafkaChannelCatalogV1*>(&factory));
    loader->Regist(IID_BLOCK_STREAM_FACTORY, static_cast<IBlockStreamFactory*>(&source));
    loader->Regist(IID_BLOCK_STREAM_READER_FACTORY_V1, static_cast<IBlockStreamReaderFactoryV1*>(&source));
    struct Libraries {
        PluginLoader* l;
        ~Libraries() {
            l->StopAll();
            l->Unload();
        }
    } libraries{loader};
    const std::string root = flowsql::test::WorkspacePath("packet-scheduler");
    std::filesystem::create_directories(root);
    const std::string db = "type=sqlite;name=local;path=" + root + "/results.db",
                      catalog = "data_dir=" + root + "/data;operator_db_path=" + root + "/operators.db";
    const char* names[] = {"libflowsql_database.so", "libflowsql_builtin.so", "libflowsql_catalog.so",
                           "libflowsql_scheduler.so"};
    const char* options[] = {db.c_str(), nullptr, catalog.c_str(), "batch_workers=1"};
    assert(!loader->Load(get_absolute_process_path(), names, options, 4) && !loader->StartAll());
    int id = 0;
    for (const char* sql : {"SELECT captured_len FROM netadapter.edge INTO kafka.packets",
                            "SELECT * FROM netadapter.edge WHERE source_id = 0 INTO kafka.packets",
                            "SELECT * FROM netadapter.edge USING npm.basic INTO kafka.packets"}) {
        source.Reset();
        factory.Reset();
        const auto name = "bad" + std::to_string(id++);
        Submit(loader, name, sql);
        auto t = Terminal(loader, name);
        assert(Text(t, "status") == "failed" && source.creates == 0 && factory.session->calls == 0);
        assert(t["managed_result"]["publication"]["code"] == "PACKET_SINK_SQL_UNSUPPORTED");
    }
    for (int mode = 0; mode < 8; ++mode) {
        source.Reset();
        factory.Reset();
        source.state->schema_bad = mode == 1;
        source.state->oversize = mode == 2;
        source.state->missing = mode == 3;
        source.state->error = mode == 4;
        source.state->stale_counters = mode == 7;
        factory.available = mode != 5;
        if (mode == 6) factory.session->fail_call = 0;
        auto name = "mode" + std::to_string(mode);
        Submit(loader, name);
        auto t = Terminal(loader, name);
        const auto& p = t["managed_result"]["publication"];
        if (mode == 0 || mode == 7) {
            assert(Text(t, "status") == "completed" && t["result_row_count"].GetInt64() == 3);
            assert(p["messages"]["confirmed"].GetUint64() == 2 && p["packets"]["confirmed"].GetUint64() == 3);
            assert(source.state->releases == 1);
            assert(p["capture_counters"][0]["source_dropped_packets"].IsNull());
            if (mode == 7) {
                assert(p["capture_counters"][0]["available_mask"].GetUint() == 0);
                assert(p["capture_counters"][0]["queue_dropped_packets"].IsNull());
                assert(p["capture_counters"][0]["delivered_packets"].IsNull());
            } else {
                assert(p["capture_counters"][0]["queue_dropped_packets"].GetUint64() == 2);
            }
        } else {
            assert(Text(t, "status") == "failed");
            if (mode < 4 || mode == 5) assert(source.state->polls == 0);
            if (mode == 4) assert(p["stage"] == "source_capture");
            if (mode == 6) assert(p["packets"]["unknown"].GetUint64() == 3 && source.state->releases == 1);
        }
    }
    for (bool cancel : {false, true}) {
        source.Reset();
        factory.Reset();
        source.state->continuous = true;
        factory.session->wait = true;
        const std::string name = cancel ? "cancel" : "stop";
        Submit(loader, name);
        for (int i = 0; i < 2000 && !factory.session->entered; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        assert(factory.session->entered);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        assert(source.state->polls == 1 && source.state->releases == 0);
        auto request = Id(name);
        String(request, "mode", name, request.GetAllocator());
        Call(loader, "/scheduler/batch/stop", request);
        if (!cancel) factory.session->wait = false;
        auto t = Terminal(loader, name);
        assert(Text(t, "status") == name + "led" || (!cancel && Text(t, "status") == "stopped"));
        assert(factory.session->drains == (cancel ? 0 : 1));
        assert(source.state->releases == 1);
        assert(t["managed_result"]["publication"]["packets"][cancel ? "unknown" : "confirmed"].GetUint64() == 3);
    }
    std::cout << "PASS production packet scheduler SQL/schema/budget/capture gates, same-poll facts, no read-ahead, "
                 "stop/cancel and counters\n";
}
