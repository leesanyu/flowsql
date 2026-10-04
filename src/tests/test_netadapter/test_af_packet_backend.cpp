// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <arpa/inet.h>
#include <channels/netadapter/af_packet_backend.h>
#include <channels/netadapter/backend_plugin.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
using namespace flowsql;
using namespace flowsql::channels::netadapter;
int main(int argc, char** argv) {
    CaptureBackendConfigV1 config;
    config.backend = "af_packet";
    std::shared_ptr<ICaptureBackendSessionV1> session;
    std::string error;
    assert(OpenAfPacket(config, &session, &error) == EINVAL && !session);
    config.interfaces = {"absent-test"};
    config.buffer_bytes = 1;
    assert(OpenAfPacket(config, &session, &error) == ENOMEM && !session);
    if (argc == 1) return 0;
    if (argc < 3) {
        std::cerr << "real smoke requires at least two Ethernet interfaces\n";
        return 2;
    }
    config.buffer_bytes = 16 * 1024 * 1024;
    config.snaplen = 48;
    config.interfaces.assign(argv + 1, argv + argc);
    CaptureBackendPlugin plugin("af_packet", OpenAfPacket);
    assert(plugin.Load(nullptr) == 0 && plugin.Start() == 0);
    const int rc = plugin.Open(config, &session, &error);
    if (rc) {
        std::cerr << error << " (errno=" << rc << ")\n";
        return 1;
    }
    assert(session->Inputs().size() == config.interfaces.size());
    assert(plugin.Stop() == EBUSY);
    uint64_t capacity = 0;
    for (const auto& input : session->Inputs()) capacity += input.buffer_bytes;
    assert(capacity <= config.buffer_bytes - 2 * 1024 * 1024);
    for (size_t i = 0; i < config.interfaces.size(); ++i) {
        int fd = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, htons(0x88b5));
        assert(fd >= 0);
        sockaddr_ll address{};
        address.sll_family = AF_PACKET;
        address.sll_protocol = htons(0x88b5);
        address.sll_ifindex = if_nametoindex(config.interfaces[i].c_str());
        address.sll_halen = 6;
        std::memset(address.sll_addr, 0xff, 6);
        uint8_t frame[64]{};
        std::memset(frame, 0xff, 6);
        frame[6] = 2;
        frame[12] = 0x88;
        frame[13] = 0xb5;
        std::memcpy(frame + 14, "flowsql-smoke", 13);
        frame[27] = i;
        assert(sendto(fd, frame, sizeof(frame), 0, reinterpret_cast<sockaddr*>(&address), sizeof(address)) ==
               sizeof(frame));
        close(fd);
    }
    std::vector<bool> seen(config.interfaces.size());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        for (size_t i = 0; i < seen.size(); ++i) {
            for (int work = 0; work < 64; ++work) {
                CapturePacketViewV1 view;
                const int read = session->TryRead(i, &view);
                if (read == EAGAIN) break;
                assert(read == 0);
                if (view.captured_len >= 28 && std::memcmp(view.bytes + 14, "flowsql-smoke", 13) == 0 &&
                    view.bytes[27] == i) {
                    assert(view.captured_len == 48 && view.wire_len == 64 && view.timestamp_ns > 0);
                    seen[i] = true;
                }
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
        std::cout << config.interfaces[i]
                  << ": real L2/truncation/timestamp verified; received=" << counters.received_packets
                  << " dropped=" << counters.source_dropped_packets
                  << " ring_bytes=" << session->Inputs()[i].buffer_bytes << "\n";
    }
    session->Cancel();
    CapturePacketViewV1 view;
    assert(session->TryRead(0, &view) == ECANCELED);
    session.reset();
    assert(plugin.Stop() == 0 && plugin.Unload() == 0);
}
