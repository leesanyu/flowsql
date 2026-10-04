// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <arpa/inet.h>
#include <channels/netadapter/backend_plugin.h>
#include <channels/netadapter/pfring_backend.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <thread>
using namespace flowsql;
using namespace flowsql::channels::netadapter;
namespace {
size_t SocketEntries() {
    size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/net/pf_ring"))
        if (entry.is_regular_file() && entry.path().filename() != "info") ++count;
    return count;
}
bool Promiscuous(const std::string& name) {
    const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    assert(fd >= 0 && name.size() < IFNAMSIZ);
    ifreq query{};
    std::memcpy(query.ifr_name, name.c_str(), name.size() + 1);
    assert(ioctl(fd, SIOCGIFFLAGS, &query) == 0);
    close(fd);
    return query.ifr_flags & IFF_PROMISC;
}
}  // namespace
int main(int argc, char** argv) {
    std::cout << std::unitbuf;
    const bool abrupt_exit = argc == 6 && std::string(argv[1]) == "--abrupt-exit";
    if (abrupt_exit) {
        --argc;
        ++argv;
    }
    CaptureBackendConfigV1 config;
    config.backend = "pfring_classic";
    std::shared_ptr<ICaptureBackendSessionV1> session;
    std::string error;
    if (argc == 1) {
        config.interfaces = {"absent-test"};
        config.buffer_bytes = 1;
        const int rc = OpenPfringClassic(config, &session, &error);
        assert(rc != 0 && !session && !error.empty());
        std::cout << "explicit unavailable/budget failure verified: " << error << "\n";
        return 0;
    }
    if (argc != 5) {
        std::cerr << "real smoke requires rx_a rx_b tx_a tx_b for two Ethernet peer pairs\n";
        return 2;
    }
    config.interfaces = {argv[1], argv[2]};
    config.snaplen = 48;
    assert(SocketEntries() == 0);
    std::vector<bool> original_promiscuous;
    for (const auto& name : config.interfaces) original_promiscuous.push_back(Promiscuous(name));
    auto check_released = [&](const char* phase) {
        const auto remaining = SocketEntries();
        if (remaining) {
            std::cerr << phase << ": " << remaining << " PF_RING socket nodes remain\n";
            for (const auto& entry : std::filesystem::directory_iterator("/proc/net/pf_ring"))
                std::cerr << entry.path() << "\n";
        }
        assert(remaining == 0);
        for (size_t i = 0; i < config.interfaces.size(); ++i)
            assert(Promiscuous(config.interfaces[i]) == original_promiscuous[i]);
    };
    CaptureBackendPlugin plugin("pfring_classic", OpenPfringClassic);
    assert(plugin.Load(nullptr) == 0 && plugin.Start() == 0);
    const int rc = plugin.Open(config, &session, &error);
    if (rc) {
        std::cerr << error << " (errno=" << rc << ")\n";
        return 1;
    }
    assert(session && session->Inputs().size() == config.interfaces.size());
    assert(SocketEntries() == config.interfaces.size());
    if (abrupt_exit) {
        std::cout << "two Classic rings enabled; testing process exit without userspace close\n" << std::flush;
        _exit(0);
    }
    assert(plugin.Stop() == EBUSY);
    uint64_t capacity = 0;
    for (size_t i = 0; i < config.interfaces.size(); ++i) {
        const auto& input = session->Inputs()[i];
        assert(input.interface_name == config.interfaces[i] && !input.physical_queue);
        assert(!input.timestamp_source.empty() && !input.counter_source.empty());
        capacity += input.buffer_bytes;
        int fd = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, htons(0x88b5));
        assert(fd >= 0);
        sockaddr_ll address{};
        address.sll_family = AF_PACKET;
        address.sll_protocol = htons(0x88b5);
        address.sll_ifindex = if_nametoindex(argv[3 + i]);
        assert(address.sll_ifindex != 0);
        address.sll_halen = 6;
        std::memset(address.sll_addr, 0xff, 6);
        uint8_t frame[64]{};
        std::memset(frame, 0xff, 6);
        frame[6] = 2;
        frame[12] = 0x88;
        frame[13] = 0xb5;
        std::memcpy(frame + 14, "flowsql-classic", 15);
        frame[29] = i;
        assert(sendto(fd, frame, sizeof(frame), 0, reinterpret_cast<sockaddr*>(&address), sizeof(address)) ==
               sizeof(frame));
        close(fd);
    }
    assert(capacity <= config.buffer_bytes - 2 * 1024 * 1024);
    std::vector<bool> seen(config.interfaces.size());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        for (size_t i = 0; i < seen.size(); ++i) {
            for (int work = 0; work < 64; ++work) {
                CapturePacketViewV1 view;
                const int read = session->TryRead(i, &view);
                if (read == EAGAIN) break;
                assert(read == 0);
                if (view.captured_len >= 30 && std::memcmp(view.bytes + 14, "flowsql-classic", 15) == 0 &&
                    view.bytes[29] == i) {
                    assert(view.captured_len == 48 && view.wire_len == 64 && view.timestamp_ns > 0);
                    seen[i] = true;
                }
                CapturePacketViewV1 overlapping;
                assert(session->TryRead(i, &overlapping) == EBUSY);
                session->ReleasePacket(i);
            }
        }
        if (std::all_of(seen.begin(), seen.end(), [](bool value) { return value; })) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    for (size_t i = 0; i < seen.size(); ++i) {
        assert(seen[i]);
        CaptureCountersV1 counters;
        assert(session->ReadCounters(i, &counters) == 0);
        assert(counters.available_mask & kCaptureReceivedPacketsAvailable);
        assert(counters.available_mask & kCaptureSourceDroppedPacketsAvailable);
        assert(counters.received_packets > 0);
        std::cout << config.interfaces[i]
                  << ": real Classic L2/truncation/timestamp/borrow verified; received=" << counters.received_packets
                  << " dropped=" << counters.source_dropped_packets
                  << " ring_bytes=" << session->Inputs()[i].buffer_bytes << "\n";
    }
    session->Cancel();
    for (size_t i = 0; i < seen.size(); ++i) {
        CapturePacketViewV1 view;
        assert(session->TryRead(i, &view) == ECANCELED);
    }
    session.reset();
    check_released("Cancel/close");
    for (int repeat = 0; repeat < 3; ++repeat) {
        assert(plugin.Open(config, &session, &error) == 0 && session);
        assert(SocketEntries() == config.interfaces.size());
        assert(plugin.Stop() == EBUSY);
        session.reset();
        check_released("reopen/close");
    }
    auto rejected = config;
    rejected.snaplen = 1500;
    rejected.buffer_bytes = 16 * 1024 * 1024;
    assert(plugin.Open(rejected, &session, &error) == 0 && session);
    uint64_t full_frame_capacity = 0;
    for (const auto& input : session->Inputs()) full_frame_capacity += input.buffer_bytes;
    assert(full_frame_capacity <= rejected.buffer_bytes - 2 * 1024 * 1024);
    session.reset();
    check_released("1500-byte snaplen/16 MiB close");
    rejected = config;
    rejected.buffer_bytes = 1;
    assert(plugin.Open(rejected, &session, &error) == ENOMEM && !session);
    assert(error.find("budget") != std::string::npos);
    check_released("budget rejection");
    rejected = config;
    rejected.interfaces[1] = "absent-test";
    assert(plugin.Open(rejected, &session, &error) != 0 && !session);
    assert(error.find("interface unavailable") != std::string::npos);
    check_released("partial-open rollback");
    rejected.interfaces[1] = "lo";
    assert(plugin.Open(rejected, &session, &error) == EPROTONOSUPPORT && !session);
    assert(error.find("non-Ethernet") != std::string::npos);
    check_released("non-Ethernet rollback");
    assert(plugin.Stop() == 0 && plugin.Unload() == 0);
    std::cout << "same-netns proc/promiscuous cleanup, reopen, partial-open rollback and budget diagnostics verified\n";
}
