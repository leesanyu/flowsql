// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <channels/netadapter/af_xdp_backend.h>
#include <channels/netadapter/backend_plugin.h>
#include <fcntl.h>
#include <linux/if_tun.h>
#include <linux/if_xdp.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>
#ifdef FLOWSQL_HAVE_AF_XDP
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <linux/if_link.h>
#endif
using namespace flowsql;
using namespace flowsql::channels::netadapter;
namespace {
#ifdef FLOWSQL_HAVE_AF_XDP
uint32_t XdpId(const std::string& name) {
    bpf_xdp_query_opts query{};
    query.sz = sizeof(query);
    assert(bpf_xdp_query(if_nametoindex(name.c_str()), XDP_FLAGS_SKB_MODE, &query) == 0);
    assert(query.drv_prog_id == 0 && query.hw_prog_id == 0);
    return query.skb_prog_id;
}
uint32_t Promiscuity(const std::string& name) {
    const int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    assert(fd >= 0);
    struct {
        nlmsghdr header;
        ifinfomsg link;
    } request{};
    request.header.nlmsg_len = NLMSG_LENGTH(sizeof(ifinfomsg));
    request.header.nlmsg_type = RTM_GETLINK;
    request.header.nlmsg_flags = NLM_F_REQUEST;
    request.link.ifi_index = if_nametoindex(name.c_str());
    sockaddr_nl kernel{};
    kernel.nl_family = AF_NETLINK;
    assert(sendto(fd, &request, request.header.nlmsg_len, 0, reinterpret_cast<sockaddr*>(&kernel), sizeof(kernel)) ==
           request.header.nlmsg_len);
    alignas(nlmsghdr) char response[4096];
    int size = recv(fd, response, sizeof(response), 0);
    close(fd);
    auto* header = reinterpret_cast<nlmsghdr*>(response);
    assert(NLMSG_OK(header, size) && header->nlmsg_type == RTM_NEWLINK);
    auto* link = static_cast<ifinfomsg*>(NLMSG_DATA(header));
    int remaining = IFLA_PAYLOAD(header);
    for (auto* attribute = IFLA_RTA(link); RTA_OK(attribute, remaining); attribute = RTA_NEXT(attribute, remaining)) {
        if (attribute->rta_type == IFLA_PROMISCUITY) {
            uint32_t count = 0;
            assert(RTA_PAYLOAD(attribute) == sizeof(count));
            std::memcpy(&count, RTA_DATA(attribute), sizeof(count));
            return count;
        }
    }
    assert(false);
    return 0;
}
struct PassProgram {
    int fd = -1;
    PassProgram() {
        bpf_insn code[2]{};
        code[0].code = BPF_ALU64 | BPF_MOV | BPF_K;
        code[0].dst_reg = BPF_REG_0;
        code[0].imm = XDP_PASS;
        code[1].code = BPF_JMP | BPF_EXIT;
        bpf_prog_load_opts options{};
        options.sz = sizeof(options);
        fd = bpf_prog_load(BPF_PROG_TYPE_XDP, "flowsql_test", "GPL", code, 2, &options);
        assert(fd >= 0);
    }
    ~PassProgram() { close(fd); }
    void Attach(const std::string& name, int previous = -1) const {
        bpf_xdp_attach_opts options{};
        options.sz = sizeof(options);
        options.old_prog_fd = previous;
        assert(bpf_xdp_attach(if_nametoindex(name.c_str()), fd,
                              XDP_FLAGS_SKB_MODE | (previous < 0 ? XDP_FLAGS_UPDATE_IF_NOEXIST : XDP_FLAGS_REPLACE),
                              &options) == 0);
    }
    void Detach(const std::string& name) const {
        bpf_xdp_attach_opts options{};
        options.sz = sizeof(options);
        options.old_prog_fd = fd;
        assert(bpf_xdp_detach(if_nametoindex(name.c_str()), XDP_FLAGS_SKB_MODE | XDP_FLAGS_REPLACE, &options) == 0);
    }
};
struct TapQueues {
    std::vector<int> fds;
    TapQueues(const std::vector<std::string>& names, const char* node) {
        for (const auto& name : names) {
            for (int queue = 0; queue < 2; ++queue) {
                const int fd = open(node, O_RDWR | O_CLOEXEC);
                assert(fd >= 0);
                ifreq request{};
                assert(name.size() < IFNAMSIZ);
                std::memcpy(request.ifr_name, name.c_str(), name.size() + 1);
                request.ifr_flags = IFF_TAP | IFF_NO_PI | IFF_MULTI_QUEUE;
                assert(ioctl(fd, TUNSETIFF, &request) == 0);
                fds.push_back(fd);
            }
            const int control = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
            assert(control >= 0);
            ifreq request{};
            std::memcpy(request.ifr_name, name.c_str(), name.size() + 1);
            assert(ioctl(control, SIOCGIFFLAGS, &request) == 0);
            request.ifr_flags |= IFF_UP;
            assert(ioctl(control, SIOCSIFFLAGS, &request) == 0);
            close(control);
        }
    }
    ~TapQueues() { Close(); }
    void Close() {
        for (const int fd : fds) close(fd);
        fds.clear();
    }
};
void CheckQueues(const std::shared_ptr<ICaptureBackendSessionV1>& session, const CaptureBackendConfigV1& config,
                 const TapQueues& taps) {
    const auto& inputs = session->Inputs();
    assert(inputs.size() == 4);
    for (const auto& name : config.interfaces)
        assert(std::count_if(inputs.begin(), inputs.end(),
                             [&](const auto& input) { return input.interface_name == name; }) == 2);
    std::vector<uint32_t> counts(inputs.size());
    uint64_t capacity = 0;
    for (size_t source = 0; source < inputs.size(); ++source) {
        const auto& input = inputs[source];
        assert(input.physical_queue && input.queue_id < 2);
        assert(std::none_of(inputs.begin(), inputs.begin() + source, [&](const auto& previous) {
            return previous.interface_name == input.interface_name && previous.queue_id == input.queue_id;
        }));
        assert(!input.timestamp_source.empty() && !input.counter_source.empty());
        xdp_options mode{};
        socklen_t mode_size = sizeof(mode);
        assert(getsockopt(input.readiness_fd, SOL_XDP, XDP_OPTIONS, &mode, &mode_size) == 0);
        assert(!(mode.flags & XDP_OPTIONS_ZEROCOPY));
        capacity += input.buffer_bytes;
    }
    assert(capacity <= config.buffer_bytes - 2 * 1024 * 1024);
    // Each queue's 256 UMEM frames must be recycled more than twice, with every round checked.
    for (uint32_t round = 0; round < 600; ++round) {
        for (const auto& input : inputs) {
            const auto selected = std::find(config.interfaces.begin(), config.interfaces.end(), input.interface_name);
            assert(selected != config.interfaces.end());
            const size_t i = std::distance(config.interfaces.begin(), selected);
            uint8_t frame[64]{};
            frame[0] = 2;
            frame[5] = input.queue_id;
            frame[6] = 2;
            frame[11] = i + 10;
            frame[12] = 0x88;
            frame[13] = 0xb5;
            std::memcpy(frame + 14, "flowsql-smoke", 13);
            frame[27] = i;
            frame[28] = input.queue_id;
            frame[29] = round >> 8;
            frame[30] = round;
            assert(write(taps.fds[i * 2 + input.queue_id], frame, sizeof(frame)) == sizeof(frame));
        }
        std::vector<bool> seen(inputs.size());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            for (uint32_t source = 0; source < inputs.size(); ++source) {
                for (int work = 0; work < 64; ++work) {
                    CapturePacketViewV1 view;
                    const int read = session->TryRead(source, &view);
                    if (read == EAGAIN) break;
                    assert(read == 0);
                    if (view.captured_len >= 31 && std::memcmp(view.bytes + 14, "flowsql-smoke", 13) == 0) {
                        const auto& input = inputs[source];
                        assert(view.bytes[27] < config.interfaces.size());
                        assert(config.interfaces[view.bytes[27]] == input.interface_name);
                        if (view.bytes[28] != input.queue_id) {
                            std::cerr << "queue steering mismatch: interface=" << input.interface_name
                                      << " marker_queue=" << unsigned(view.bytes[28])
                                      << " actual_rx_queue=" << input.queue_id << "\n";
                            std::abort();
                        }
                        assert((uint32_t(view.bytes[29]) << 8 | view.bytes[30]) == round);
                        assert(view.captured_len == 48 && view.wire_len == 64 && view.timestamp_ns > 0);
                        assert(!seen[source]);
                        seen[source] = true;
                        ++counts[source];
                    }
                    CapturePacketViewV1 overlapping;
                    assert(session->TryRead(source, &overlapping) == EBUSY);
                    session->ReleasePacket(source);
                }
            }
            if (std::all_of(seen.begin(), seen.end(), [](bool value) { return value; })) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        for (size_t i = 0; i < seen.size(); ++i) {
            if (!seen[i]) {
                std::cerr << "missing known frame: interface=" << inputs[i].interface_name
                          << " queue=" << inputs[i].queue_id << " round=" << round << "\n";
                std::abort();
            }
        }
    }
    for (size_t source = 0; source < inputs.size(); ++source) {
        CaptureCountersV1 counters;
        assert(session->ReadCounters(source, &counters) == 0);
        assert(!(counters.available_mask & kCaptureReceivedPacketsAvailable));
        assert(counters.available_mask & kCaptureSourceDroppedPacketsAvailable);
        assert(counters.source_dropped_packets == 0 && counts[source] == 600);
        std::cout << inputs[source].interface_name << ":rx" << inputs[source].queue_id
                  << ": known_packets=" << counts[source] << " SKB/COPY L2/truncation/timestamp/UMEM recycle verified"
                  << "; received=unavailable dropped=" << counters.source_dropped_packets
                  << " buffer_bytes=" << inputs[source].buffer_bytes << "\n";
    }
    std::cout.flush();
}
#endif
}  // namespace
int main(int argc, char** argv) {
    CaptureBackendConfigV1 config;
    config.backend = "af_xdp_copy_skb";
    std::shared_ptr<ICaptureBackendSessionV1> session;
    std::string error;
#ifdef FLOWSQL_HAVE_AF_XDP
    assert(OpenAfXdpCopySkb(config, &session, &error) == EINVAL && !session);
#else
    assert(OpenAfXdpCopySkb(config, &session, &error) == ENODEV && !session && !error.empty());
#endif
    config.interfaces = {"absent-test"};
    config.buffer_bytes = 1;
    assert(OpenAfXdpCopySkb(config, &session, &error) != 0 && !session);
    if (argc == 1) return 0;
    if (argc != 4) {
        std::cerr << "real smoke requires tap_a tap_b tun_device_node in a private network namespace\n";
        return 2;
    }
#ifndef FLOWSQL_HAVE_AF_XDP
    std::cerr << error << "\n";
    return 1;
#else
    config.buffer_bytes = 16 * 1024 * 1024;
    config.snaplen = 48;
    config.interfaces.assign(argv + 1, argv + 3);
    TapQueues taps(config.interfaces, argv[3]);
    std::vector<uint32_t> original_promiscuity;
    for (const auto& name : config.interfaces) {
        assert(XdpId(name) == 0);
        original_promiscuity.push_back(Promiscuity(name));
    }
    CaptureBackendPlugin plugin("af_xdp_copy_skb", OpenAfXdpCopySkb);
    assert(plugin.Load(nullptr) == 0 && plugin.Start() == 0);
    auto invalid = config;
    invalid.interfaces[1] = "lo";
    const int invalid_rc = plugin.Open(invalid, &session, &error);
    if (invalid_rc != EPROTONOSUPPORT) std::cerr << "invalid input check: " << error << " errno=" << invalid_rc << "\n";
    assert(invalid_rc == EPROTONOSUPPORT && !session);
    invalid = config;
    invalid.buffer_bytes = 4 * 1024 * 1024;
    assert(plugin.Open(invalid, &session, &error) == ENOMEM && !session);
    for (size_t i = 0; i < config.interfaces.size(); ++i)
        assert(XdpId(config.interfaces[i]) == 0 && Promiscuity(config.interfaces[i]) == original_promiscuity[i]);
    PassProgram external;
    external.Attach(config.interfaces[1]);
    const auto external_id = XdpId(config.interfaces[1]);
    assert(external_id != 0);
    assert(plugin.Open(config, &session, &error) == EBUSY && !session);
    assert(XdpId(config.interfaces[0]) == 0 && XdpId(config.interfaces[1]) == external_id);
    external.Detach(config.interfaces[1]);
    const int rc = plugin.Open(config, &session, &error);
    if (rc) {
        std::cerr << error << " (errno=" << rc << ")\n";
        return 1;
    }
    assert(plugin.Stop() == EBUSY);
    for (size_t i = 0; i < config.interfaces.size(); ++i) {
        assert(XdpId(config.interfaces[i]) != 0);
        assert(Promiscuity(config.interfaces[i]) == original_promiscuity[i] + 1);
    }
    CheckQueues(session, config, taps);
    session->Cancel();
    for (uint32_t i = 0; i < session->Inputs().size(); ++i) {
        CapturePacketViewV1 view;
        assert(session->TryRead(i, &view) == ECANCELED);
    }
    session.reset();
    for (size_t i = 0; i < config.interfaces.size(); ++i)
        assert(XdpId(config.interfaces[i]) == 0 && Promiscuity(config.interfaces[i]) == original_promiscuity[i]);
    // Reopening the same queues proves the previous XSK/UMEM binding and provider lease were released.
    assert(plugin.Open(config, &session, &error) == 0);
    const int previous = bpf_prog_get_fd_by_id(XdpId(config.interfaces[0]));
    assert(previous >= 0);
    external.Attach(config.interfaces[0], previous);
    close(previous);
    const auto replacement_id = XdpId(config.interfaces[0]);
    assert(replacement_id == external_id);
    session->Cancel();
    session.reset();
    assert(XdpId(config.interfaces[0]) == replacement_id && XdpId(config.interfaces[1]) == 0);
    external.Detach(config.interfaces[0]);
    for (size_t i = 0; i < config.interfaces.size(); ++i)
        assert(XdpId(config.interfaces[i]) == 0 && Promiscuity(config.interfaces[i]) == original_promiscuity[i]);
    assert(plugin.Stop() == 0 && plugin.Unload() == 0);
    taps.Close();
    for (const auto& name : config.interfaces) assert(if_nametoindex(name.c_str()) == 0);
    std::cout << "all four RX queues, repeated FILL recycling, Cancel, reopen, owner rejection/preservation, and "
                 "TAP cleanup verified\n";
#endif
}
