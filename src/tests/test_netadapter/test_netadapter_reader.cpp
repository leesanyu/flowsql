// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <channels/netadapter/netadapter_plugin.h>
#include <channels/netadapter/netadapter_reader.h>
#include <channels/pcapfile/packet_filter_domain.h>
#include <rapidjson/document.h>
#include <unistd.h>
#include <cassert>
#include <chrono>
#include <cstring>
#include <deque>
#include <thread>
using namespace flowsql;
using namespace flowsql::channels::netadapter;
class Protocol final : public IProtocol {
 public:
    void Concurrency(int32_t) override {}
    protocol::Protocol Identify(int32_t, const uint8_t*, int32_t, const protocol::Layers*) override {
        assert(false);
        return {};
    }
    int32_t Layer(int32_t, const uint8_t*, int32_t, protocol::Layers* out) override {
        *out = {};
        return 0;
    }
    protocol::IDictionary* Dictionary() override { return nullptr; }
};
class Backend final : public ICaptureBackendSessionV1 {
 public:
    explicit Backend(size_t count = 2, size_t frame_bytes = 64)
        : queue(count), packet(frame_bytes), held(count), checks(count) {
        for (size_t index = 0; index < count; ++index) {
            CaptureBackendInputV1 i;
            i.interface_name = "test" + std::to_string(index + 1);
            i.buffer_bytes = 1024 * 1024;
            i.timestamp_source = "test";
            inputs.push_back(i);
        }
    }
    const std::vector<CaptureBackendInputV1>& Inputs() const override { return inputs; }
    int TryRead(uint32_t i, CapturePacketViewV1* out) override {
        ++checks[i];
        if (failure) return EIO;
        if (queue[i].empty()) return EAGAIN;
        assert(!held[i]);
        held[i] = true;
        work_time += work_per_packet;
        *out = {packet.data(), uint32_t(packet.size()), uint32_t(packet.size()), queue[i].front()};
        return 0;
    }
    void ReleasePacket(uint32_t i) override {
        assert(held[i]);
        held[i] = false;
        queue[i].pop_front();
        ++released;
    }
    CaptureBacklogV1 Backlog(uint32_t i) const override {
        return queue[i].empty() ? CaptureBacklogV1::kEmpty : CaptureBacklogV1::kPresent;
    }
    int64_t IdleTimeNs(uint32_t) const override { return idle_time; }
    int ReadCounters(uint32_t, CaptureCountersV1* out) override {
        *out = {};
        return 0;
    }
    void Cancel() override { ++cancelled; }
    std::vector<CaptureBackendInputV1> inputs;
    std::vector<std::deque<int64_t>> queue;
    std::vector<uint8_t> packet;
    std::vector<bool> held;
    std::vector<uint32_t> checks;
    uint32_t released = 0, cancelled = 0;
    bool failure = false;
    int64_t idle_time = 100000000000LL;
    std::chrono::steady_clock::duration work_time{}, work_per_packet{};
};
class Provider final : public ICaptureBackendProviderV1 {
 public:
    const char* Backend() const override { return "af_packet"; }
    int Open(const CaptureBackendConfigV1&, std::shared_ptr<ICaptureBackendSessionV1>* out, std::string*) override {
        *out = std::make_shared<::Backend>();
        return 0;
    }
};
class Querier final : public IQuerier {
 public:
    Protocol protocol;
    Provider provider;
    int Traverse(const Guid& iid, fntraverse call) override {
        if (std::memcmp(&iid, &IID_CAPTURE_BACKEND_PROVIDER_V1, sizeof(Guid)) == 0)
            call(static_cast<ICaptureBackendProviderV1*>(&provider));
        return 0;
    }
    void* First(const Guid& iid) override {
        return std::memcmp(&iid, &IID_PROTOCOL, sizeof(Guid)) == 0 ? static_cast<IProtocol*>(&protocol) : nullptr;
    }
};
void CheckPollBudgets(Protocol* protocol, const std::shared_ptr<const packet::PcapFilterPlan>& reject_all) {
    const auto check = [&](size_t count, size_t frame_bytes, uint32_t expected,
                           std::chrono::steady_clock::duration work_per_packet) {
        auto backend = std::make_shared<Backend>(count, frame_bytes);
        backend->work_per_packet = work_per_packet;
        CaptureBackendConfigV1 config;
        config.buffer_bytes = 128 * kMiB;
        for (const auto& input : backend->inputs) config.interfaces.push_back(input.interface_name);
        for (auto& queue : backend->queue)
            for (int i = 0; i < 100; ++i) queue.push_back(i);
        NetAdapterReader reader("budgets", config, 7, 1, backend, protocol, std::make_shared<int>(), reject_all,
                                [backend] { return std::chrono::steady_clock::time_point(backend->work_time); });
        assert(reader.Open() == 0);
        auto event = reader.PollCapture(0);
        assert(event.block.kind == BlockPollEvent::kTimeout && !event.block.batch);
        assert(backend->released == expected);
        for (size_t i = 0; i < count; ++i) {
            assert(!backend->held[i] && backend->checks[i] <= 16);
            const auto fact = std::find_if(event.progress.begin(), event.progress.end(),
                                           [&](const auto& value) { return value.source_id == i; });
            assert((fact != event.progress.end()) == (backend->checks[i] != 0));
            if (fact != event.progress.end()) {
                assert(!fact->source_idle_confirmed && !fact->packet_observed);
                assert(fact->backlog == CaptureBacklogV1::kPresent);
            }
        }
        if (count == 65 && frame_bytes == 64) {
            // The global cap stops before every input receives its full per-input share.
            assert(backend->checks[64] == 15);
        }
        if (work_per_packet != std::chrono::steady_clock::duration{}) {
            assert(event.progress.size() == 1 && backend->checks[1] == 0);
            event = reader.PollCapture(0);
            assert(event.progress.size() == 1 && event.progress[0].source_id == 1);
        }
        reader.Cancel();
        assert(reader.Close() == 0);
    };
    check(65, 64, 1024, {});
    // Byte accounting is checked after each bounded packet, including rejected packets.
    check(65, 65535, 65, {});
    check(2, 64, 1, std::chrono::milliseconds(2));

    for (size_t count : {size_t(1), size_t(32)}) {
        auto backend = std::make_shared<Backend>(count);
        CaptureBackendConfigV1 config;
        config.buffer_bytes = 64 * kMiB;
        for (const auto& input : backend->inputs) config.interfaces.push_back(input.interface_name);
        NetAdapterReader reader("idle-many", config, 7, 1, backend, protocol, std::make_shared<int>(), nullptr);
        assert(reader.Open() == 0);
        const auto start = std::chrono::steady_clock::now();
        const auto event = reader.PollCapture(1000);
        assert(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(100));
        assert(event.progress.size() == count);
        for (size_t i = 0; i < count; ++i) {
            assert(backend->checks[i] == 2 && event.progress[i].source_idle_confirmed);
        }
        reader.Cancel();
        assert(reader.Close() == 0);
    }
}
void CheckContinuingProgress(Protocol* protocol) {
    auto backend = std::make_shared<Backend>();
    CaptureBackendConfigV1 config;
    config.interfaces = {"test1", "test2"};
    NetAdapterReader reader("continuing", config, 7, 1, backend, protocol, std::make_shared<int>(), nullptr,
                            [] { return std::chrono::steady_clock::time_point{}; });
    assert(reader.Open() == 0);
    CaptureSourceSetV2 sources;
    assert(reader.DescribeSources(&sources) == 0);
    CaptureProgressTrackerV2 tracker(sources);
    int64_t previous = 0;
    for (int tick = 1; tick <= 32; ++tick) {
        backend->idle_time = tick * 10000 + 100;
        for (int packet = 0; packet < 16; ++packet) backend->queue[0].push_back(tick * 10000 + packet);
        if (tick % 2 == 0) backend->queue[1].push_back(tick * 10000 + 2);
        auto event = reader.PollCapture(1000);
        assert(event.block.kind == BlockPollEvent::kData && event.progress.size() == 2);
        assert(backend->queue[0].empty() && backend->queue[1].empty());
        assert(event.progress[1].source_idle_confirmed == (tick % 2 != 0));
        assert(reader.ReleaseBlock(event.block.batch) == 0);
        event.block.batch.reset();
        assert(tracker.Observe(event.progress) == CaptureProgressErrorV1::kNone);
        const auto common = tracker.CommonCandidateNs();
        assert(common && *common > previous && *common <= tick * 10000 + 15);
        previous = *common;
    }
    // A backlog suspends progress; draining it resumes the same common safety gate.
    for (int packet = 0; packet < 100; ++packet) backend->queue[0].push_back(330000 + packet);
    backend->idle_time = 340000;
    while (!backend->queue[0].empty()) {
        auto event = reader.PollCapture(0);
        assert(event.block.kind == BlockPollEvent::kData);
        assert(reader.ReleaseBlock(event.block.batch) == 0);
        event.block.batch.reset();
        assert(tracker.Observe(event.progress) == CaptureProgressErrorV1::kNone);
        if (!backend->queue[0].empty())
            assert(!tracker.CommonCandidateNs());
        else
            assert(tracker.CommonCandidateNs() && *tracker.CommonCandidateNs() > previous);
    }
    const auto safe = tracker.CommonCandidateNs();
    CaptureProgressV1 first, second;
    first.source_id = 0;
    second.source_id = 1;
    first.generation = second.generation = 1;
    first.fact_sequence = second.fact_sequence = 100;
    first.capture_time_ns = second.capture_time_ns = 350000;
    first.source_idle_confirmed = second.source_idle_confirmed = true;
    first.backlog = CaptureBacklogV1::kEmpty;
    second.backlog = CaptureBacklogV1::kUnknown;
    auto invalid = second;
    invalid.generation = 2;
    assert(tracker.Observe({first, invalid}) == CaptureProgressErrorV1::kWrongQueue);
    assert(tracker.CommonCandidateNs() == safe);
    assert(tracker.Observe({first, second}) == CaptureProgressErrorV1::kNone);
    assert(!tracker.CommonCandidateNs() && tracker.BlockedInputs() == std::vector<uint32_t>{1});
    assert(tracker.Observe({first}) == CaptureProgressErrorV1::kInvalidSequence);
    second.fact_sequence = 101;
    second.backlog = CaptureBacklogV1::kEmpty;
    assert(tracker.Observe({second}) == CaptureProgressErrorV1::kNone);
    assert(tracker.CommonCandidateNs() == 350000);
    second.fact_sequence = 102;
    second.capture_time_ns = 349999;
    assert(tracker.Observe({second}) == CaptureProgressErrorV1::kNone && !tracker.CommonCandidateNs());
    reader.Cancel();
    assert(reader.Close() == 0);
}
int main() {
    Protocol protocol;
    auto backend = std::make_shared<Backend>();
    CaptureBackendConfigV1 config;
    config.interfaces = {"test1", "test2"};
    auto lease = std::make_shared<int>(1);
    std::weak_ptr<int> retained = lease;
    auto reader = std::make_unique<NetAdapterReader>("test", config, 7, 1, backend, &protocol, lease, nullptr);
    lease.reset();
    assert(reader->Open() == 0);
    CaptureSourceSetV2 sources;
    assert(reader->DescribeSources(&sources) == 0 && sources.inputs.size() == 2);
    for (int i = 0; i < 100; ++i) backend->queue[0].push_back(i);
    backend->queue[1].push_back(50);
    auto event = reader->PollCapture(100);
    assert(event.block.kind == BlockPollEvent::kData);
    auto ids = std::static_pointer_cast<arrow::UInt32Array>(event.block.batch->column(4));
    assert(ids->Value(0) == 0 && event.block.batch->num_rows() <= 17);
    if (event.block.batch->num_rows() == 1) {
        assert(reader->ReleaseBlock(event.block.batch) == 0);
        event.block.batch.reset();
        event = reader->PollCapture(0);
        assert(event.block.kind == BlockPollEvent::kData);
        ids = std::static_pointer_cast<arrow::UInt32Array>(event.block.batch->column(4));
        assert(ids->Value(0) == 1);
    } else {
        assert(ids->Value(1) == 1);
    }
    assert(event.progress.size() == 2 && event.progress[0].backlog == CaptureBacklogV1::kPresent);
    assert(reader->PollCapture(0).block.kind == BlockPollEvent::kTimeout);
    auto slice = event.block.batch->column(6)->Slice(0, 1);
    assert(reader->ReleaseBlock(event.block.batch) == 0);
    assert(reader->ReleaseBlock(event.block.batch) == EINVAL);
    event.block.batch.reset();
    ids.reset();
    assert(!backend->held[0] && !backend->held[1]);
    backend->packet[0] = 99;
    assert(std::static_pointer_cast<arrow::BinaryArray>(slice)->GetView(0)[0] == 0);
    rapidjson::Document retained_memory;
    retained_memory.Parse(reader->Diagnostics().c_str());
    assert(retained_memory["arrow_bytes"].GetInt64() > 0);
    reader->Cancel();
    assert(reader->PollCapture(0).block.kind == BlockPollEvent::kCancelled);
    assert(reader->Close() == EBUSY);
    reader.reset();
    assert(!retained.expired());
    slice.reset();
    assert(retained.expired());
    auto idle = std::make_shared<Backend>();
    NetAdapterReader empty("idle", config, 7, 2, idle, &protocol, std::make_shared<int>(), nullptr);
    assert(empty.Open() == 0);
    auto start = std::chrono::steady_clock::now();
    auto idle_event = empty.PollCapture(1000);
    assert(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(100));
    assert(idle_event.progress.size() == 2 && idle_event.progress[0].source_idle_confirmed);
    assert(empty.Flush() == 0);
    empty.Cancel();
    assert(empty.PollCapture(0).block.kind == BlockPollEvent::kEof);
    assert(empty.PollCapture(0).block.kind == BlockPollEvent::kTimeout);
    assert(empty.Close() == 0);
    auto broken = std::make_shared<Backend>();
    broken->failure = true;
    NetAdapterReader bad("bad", config, 7, 3, broken, &protocol, std::make_shared<int>(), nullptr);
    assert(bad.Open() == 0);
    assert(bad.PollCapture(0).block.kind == BlockPollEvent::kError);
    assert(bad.Flush() == 0);
    bad.Cancel();
    assert(bad.PollCapture(0).block.kind == BlockPollEvent::kError);
    auto rejected_backend = std::make_shared<Backend>();
    for (auto& queue : rejected_backend->queue)
        for (int i = 0; i < 2000; ++i) queue.push_back(i);
    auto reject_all = std::make_shared<packet::PcapFilterPlan>();
    std::string filter_error;
    assert(flowsql::channels::pcapfile::CompilePcapFilterPlanJson(
               R"({"version":1,"root":{"node_id":1,"kind":"literal","type":"bool","nullable":false,"value":"false"}})",
               reject_all.get(), &filter_error) == 0);
    CheckPollBudgets(&protocol, reject_all);
    CheckContinuingProgress(&protocol);
    NetAdapterReader filtered("filtered", config, 7, 4, rejected_backend, &protocol, std::make_shared<int>(),
                              reject_all);
    assert(filtered.Open() == 0);
    auto rejected = filtered.PollCapture(0);
    assert(rejected.block.kind == BlockPollEvent::kTimeout && !rejected.block.batch);
    assert(rejected_backend->released > 0 && rejected_backend->released <= 2 * kInputPacketQuantum);
    assert(rejected.progress.size() == 2);
    for (const auto& fact : rejected.progress)
        assert(!fact.packet_observed && !fact.source_idle_confirmed && fact.backlog == CaptureBacklogV1::kPresent);
    filtered.Cancel();
    assert(filtered.Close() == 0);
    CaptureMemoryPool budget(128);
    uint8_t* bytes = nullptr;
    assert(budget.Allocate(64, 64, &bytes).ok());
    assert(!budget.Reallocate(64, 96, 64, &bytes).ok());
    budget.Free(bytes, 64, 64);
    assert(budget.bytes_allocated() == 0 && budget.max_memory() == 64);
    Querier querier;
    const std::string path = "/tmp/netadapter-manager-" + std::to_string(getpid()) + ".db";
    NetAdapterPlugin plugin;
    assert(plugin.Option(("{\"db_path\":\"" + path + "\"}").c_str()) == 0);
    assert(plugin.Load(&querier) == 0 && plugin.Start() == 0);
    const std::string options = R"({"backend":"af_packet","interfaces":["test1","test2"]})";
    assert(plugin.AddChannel("netadapter", "manager", options) == 0);
    uint64_t domain = 0;
    plugin.QueryChannels([&](const auto&, const auto&, const auto& json, const auto&) {
        rapidjson::Document doc;
        doc.Parse(json.c_str());
        assert(!doc.HasParseError());
        assert(doc["input_packet_quantum"].GetUint() == 16);
        assert(doc["input_byte_quantum"].GetUint64() == 65536);
        assert(doc["shared_envelope_min_bytes"].GetUint64() == 2097152);
        assert(doc["minimum_input_bytes"].GetUint64() == 1048576);
        assert(doc["readers"].Empty());
        domain = doc["observation_domain_id"].GetUint64();
    });
    IBlockStreamChannel* managed = nullptr;
    BlockStreamReaderConfigV1 request;
    request.task_id = "test";
    request.source_category = "netadapter";
    request.source_name = "manager";
    request.pushed_filter_plan_json = R"({"version":1,"root":null})";
    assert(plugin.CreateReader(request, &managed) == 0 && managed);
    plugin.QueryChannels([&](const auto&, const auto&, const auto& json, const auto& status) {
        rapidjson::Document doc;
        doc.Parse(json.c_str());
        assert(status == "busy" && doc["readers"].Size() == 1);
        const auto& active = doc["readers"][0];
        assert(active["input_packet_quantum"].GetUint() == 16);
        assert(active["input_byte_quantum"].GetUint64() == 65536);
        assert(active["shared_envelope_min_bytes"].GetUint64() == 2097152);
        assert(active["minimum_input_bytes"].GetUint64() == 1048576);
        assert(active["backend_bytes"].GetUint64() == 2097152);
        assert(active["arrow_budget_bytes"].GetUint64() == 62 * kMiB);
        assert(active["backend_bytes"].GetUint64() + active["arrow_budget_bytes"].GetUint64() ==
               doc["budget_bytes"].GetUint64());
        assert(active["inputs"].Size() == 2);
        for (uint32_t i = 0; i < 2; ++i) {
            const auto& input = active["inputs"][i];
            assert(input["source_id"].GetUint() == i);
            assert(input["buffer_bytes"].GetUint64() == 1048576);
            assert(input["queue_id"].GetUint() == 0 && !input["physical_queue"].GetBool());
        }
    });
    assert(plugin.ModifyChannel("netadapter", "manager", options) == EBUSY);
    assert(plugin.RemoveChannel("netadapter", "manager") == EBUSY && plugin.Stop() == EBUSY);
    plugin.ReleaseReader(managed);
    assert(plugin.Stop() == 0 && plugin.Unload() == 0);
    assert(plugin.Load(&querier) == 0 && plugin.Start() == 0);
    plugin.QueryChannels([&](const auto&, const auto&, const auto& json, const auto&) {
        rapidjson::Document doc;
        doc.Parse(json.c_str());
        assert(doc["observation_domain_id"].GetUint64() == domain);
    });
    assert(plugin.RemoveChannel("netadapter", "manager") == 0 && plugin.Unload() == 0);
    assert(unlink(path.c_str()) == 0);
}
