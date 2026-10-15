// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <arpa/inet.h>
#include <framework/interfaces/iblock_stream_manager.h>
#include <framework/interfaces/irouter_handle.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>
#include <common/loader.hpp>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include "packet_sink_test_support.h"
using namespace packet_sink_test;
using Clock = std::chrono::steady_clock;
namespace {
void Check(bool ok, const char* stage) {
    if (!ok) throw std::runtime_error(stage);
}
Json Call(PluginLoader* loader, const std::string& path, const rapidjson::Value& request) {
    fnRouterHandler f;
    loader->Traverse(IID_ROUTER_HANDLE, [&](void* p) {
        static_cast<IRouterHandle*>(p)->EnumRoutes([&](const RouteItem& r) {
            if (r.uri == path && r.method == "POST") f = r.handler;
        });
        return f ? -1 : 0;
    });
    Check(bool(f), "route");
    std::string out;
    int rc = f(path, Encode(request), out);
    if (rc) std::cerr << out << '\n';
    Check(!rc, "route_status");
    return Parse(out);
}
Json Id(const std::string& id) {
    auto j = Parse("{}");
    String(j, "runtime_task_id", id, j.GetAllocator());
    return j;
}
struct SlowSession final : IKafkaClientSessionV1, IKafkaProducerSessionV1 {
    KafkaBindingSnapshotV1 snapshot;
    std::atomic<bool> cancelled{false}, entered{false};
    uint32_t delay = 0;
    uint64_t confirmed_packets = 0, messages = 0, bytes = 0;
    std::vector<uint64_t> latencies;
    uint64_t last_sequence = 0;
    const KafkaBindingSnapshotV1& Snapshot() const override { return snapshot; }
    int Probe(uint32_t, std::string*) override { return 0; }
    int QueryInterface(const Guid&, void** out) override {
        *out = static_cast<IKafkaProducerSessionV1*>(this);
        return 0;
    }
    void Cancel() override { cancelled = true; }
    int Produce(const std::vector<KafkaProduceMessageV1>& m, uint32_t timeout, KafkaDeliveryV1* out) override {
        Check(m.size() == 1, "single_poll");
        entered = true;
        auto start = Clock::now();
        while (!cancelled && Clock::now() - start < std::chrono::milliseconds(delay))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (cancelled) {
            *out = {0, 0, 1};
            return ECANCELED;
        }
        Check(Clock::now() - start < std::chrono::milliseconds(timeout), "budget");
        const auto& b = m[0].value;
        auto get = [&](size_t at, size_t n) {
            uint64_t v = 0;
            for (size_t i = 0; i < n; ++i) v = (v << 8) | uint8_t(b.at(at + i));
            return v;
        };
        Check(b.compare(0, 4, "FSPK") == 0 && get(40, 8) == messages + 1, "wire_poll_order");
        const auto packets = get(48, 4), facts = get(52, 4);
        size_t at = 56;
        for (uint64_t i = 0; i < packets; ++i) {
            const auto captured = get(at + 12, 4), sequence = get(at + 24, 8);
            Check(sequence > last_sequence, "packet_order");
            last_sequence = sequence;
            Check(captured == 64 || captured == 1500, "raw_length");
            Check(b.compare(at + 32 + 14, 12, "packet-sink!") == 0, "raw_bytes");
            at += 32 + captured;
        }
        Check(at + facts * 28 == b.size(), "fact_shape");
        confirmed_packets += packets;
        ++messages;
        bytes += b.size() + m[0].key.size();
        latencies.push_back(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count());
        *out = {1, 0, 0};
        return 0;
    }
    int Drain(uint32_t) override { return 0; }
};
struct Factory final : IPacketSinkFactoryV1, IKafkaChannelCatalogV1 {
    std::shared_ptr<SlowSession> session;
    int DescribeChannel(const char*, KafkaChannelDescriptionV1* d) override {
        d->role = "sink";
        d->sink_adapter_available = true;
        return 0;
    }
    int CreateWriter(const PacketSinkContextV1& c, std::unique_ptr<IPacketSinkWriterV1>* out) override {
        *out = std::make_unique<PacketWriter>(Config(), c, session);
        return 0;
    }
};
uint64_t Send(const char* interface_name, int count, int size, int interval_us) {
    int fd = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, htons(0x88b5));
    Check(fd >= 0, "capture_permission");
    sockaddr_ll address{};
    address.sll_family = AF_PACKET;
    address.sll_protocol = htons(0x88b5);
    address.sll_ifindex = if_nametoindex(interface_name);
    address.sll_halen = 6;
    std::memset(address.sll_addr, 0xff, 6);
    std::string frame(size, 'x');
    std::memset(frame.data(), 0xff, 6);
    frame[6] = 2;
    frame[12] = '\x88';
    frame[13] = '\xb5';
    frame.replace(14, 12, "packet-sink!");
    for (int n = 0; n < count; ++n) {
        Check(sendto(fd, frame.data(), frame.size(), 0, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == size,
              "send");
        if (interval_us) std::this_thread::sleep_for(std::chrono::microseconds(interval_us));
    }
    close(fd);
    return count;
}
}  // namespace
int main(int argc, char** argv) {
    try {
        Check(argc == 3, "usage: packet_sink_capture receive_interface send_interface");
        const auto root =
            std::filesystem::temp_directory_path() / ("flowsql-packet-capture-" + std::to_string(getpid()));
        std::filesystem::create_directories(root);
        Factory factory;
        auto* loader = PluginLoader::Single();
        loader->Regist(IID_PACKET_SINK_FACTORY_V1, &factory);
        loader->Regist(IID_KAFKA_CHANNEL_CATALOG_V1, static_cast<IKafkaChannelCatalogV1*>(&factory));
        struct Libraries {
            PluginLoader* l;
            ~Libraries() {
                l->StopAll();
                l->Unload();
            }
        } libraries{loader};
        const auto db = "type=sqlite;name=local;path=" + (root / "results.db").string();
        const auto catalog =
            "data_dir=" + (root / "data").string() + ";operator_db_path=" + (root / "operators.db").string();
        const auto npi = std::string("{\"ldfile\":\"") + PACKET_NPI_CONFIG + "\",\"concurrency\":1}";
        const auto adapter = std::string("{\"db_path\":\"") + (root / "adapter.db").string() + "\"}";
        const char* names[] = {"libflowsql_database.so",  "libflowsql_builtin.so", "libflowsql_catalog.so",
                               "libflowsql_scheduler.so", "libflowsql_npi.so",     "libflowsql_capture_af_packet.so",
                               "libflowsql_netadapter.so"};
        const char* options[] = {db.c_str(),  nullptr, catalog.c_str(), "batch_workers=1",
                                 npi.c_str(), nullptr, adapter.c_str()};
        Check(!loader->Load(get_absolute_process_path(), names, options, 7) && !loader->StartAll(), "plugins");
        auto* manager = static_cast<IBlockStreamManager*>(loader->First(IID_BLOCK_STREAM_MANAGER));
        Check(manager, "manager");
        const auto config = std::string("{\"backend\":\"af_packet\",\"interfaces\":[\"") + argv[1] +
                            "\"],\"buffer_mib\":4,\"promiscuous\":false}";
        Check(!manager->AddChannel("netadapter", "edge", config), "channel");
        for (const std::string mode : {"normal", "overload", "cancel"}) {
            factory.session = std::make_shared<SlowSession>();
            factory.session->delay = mode == "normal" ? 0 : 150;
            auto request = Id(mode);
            String(request, "sql_text", "SELECT * FROM netadapter.edge INTO kafka.packets", request.GetAllocator());
            rusage before{}, after{};
            getrusage(RUSAGE_SELF, &before);
            const auto start = Clock::now();
            Call(loader, "/scheduler/batch/submit", request);
            for (int i = 0; i < 300 && !factory.session->entered; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (!factory.session->entered)
                std::cerr << Encode(Call(loader, "/scheduler/batch/status", Id(mode))) << '\n';
            Check(factory.session->entered, "sink_ready");
            const auto sent = Send(argv[2], mode == "overload" ? 60000 : 100, mode == "overload" ? 1500 : 64,
                                   mode == "normal" ? 5000 : 0);
            if (mode != "cancel") std::this_thread::sleep_for(std::chrono::milliseconds(mode == "normal" ? 100 : 800));
            auto stop = Id(mode);
            String(stop, "mode", mode == "cancel" ? "cancel" : "stop", stop.GetAllocator());
            const auto stop_at = Clock::now();
            Call(loader, "/scheduler/batch/stop", stop);
            Json terminal;
            for (int i = 0; i < 2000; ++i) {
                terminal = Call(loader, "/scheduler/batch/status", Id(mode));
                auto state = Text(terminal, "status");
                if (state == "stopped" || state == "cancelled" || state == "failed") break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            if (Text(terminal, "status") != (mode == "cancel" ? "cancelled" : "stopped"))
                std::cerr << Encode(terminal) << '\n';
            Check(Text(terminal, "status") == (mode == "cancel" ? "cancelled" : "stopped"), "terminal");
            const auto stop_us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - stop_at).count();
            getrusage(RUSAGE_SELF, &after);
            const auto& publication = terminal["managed_result"]["publication"];
            const auto& counters = publication["capture_counters"][0];
            Check((counters["available_mask"].GetUint() & kCaptureSourceDroppedPacketsAvailable) != 0,
                  "drop_counter_available");
            const auto dropped = counters["source_dropped_packets"].GetUint64();
            if (mode == "normal") Check(factory.session->confirmed_packets == sent && dropped == 0, "no_loss_load");
            if (mode == "overload")
                Check(dropped > 0 && factory.session->confirmed_packets < sent, "measured_overload_drop");
            if (mode == "cancel") Check(stop_us < 500000, "cancel_budget");
            auto metric = Parse("{}");
            auto& a = metric.GetAllocator();
            String(metric, "case", mode, a);
            metric.AddMember("sent_packets", sent, a);
            metric.AddMember("confirmed_packets", factory.session->confirmed_packets, a);
            metric.AddMember("messages", factory.session->messages, a);
            metric.AddMember("bytes", factory.session->bytes, a);
            metric.AddMember("source_dropped_packets", dropped, a);
            metric.AddMember("stop_us", stop_us, a);
            metric.AddMember("elapsed_us",
                             std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count(), a);
            metric.AddMember("rss_kib", uint64_t(after.ru_maxrss), a);
            auto cpu = [](const timeval& t) { return int64_t(t.tv_sec) * 1000000 + t.tv_usec; };
            metric.AddMember(
                "cpu_us", cpu(after.ru_utime) + cpu(after.ru_stime) - cpu(before.ru_utime) - cpu(before.ru_stime), a);
            auto latency = factory.session->latencies;
            std::sort(latency.begin(), latency.end());
            metric.AddMember("confirm_p95_us", latency.empty() ? 0 : latency[latency.size() * 95 / 100], a);
            metric.AddMember("publication", rapidjson::Value(publication, a), a);
            std::cout << "METRIC " << Encode(metric) << '\n';
        }
        std::cout
            << "PASS isolated AF_PACKET production scheduler/writer slow delivery, actual drop counters, stop/cancel\n";
    } catch (const std::exception& e) {
        std::cerr << "capture_failed: " << e.what() << '\n';
        return 1;
    }
}
