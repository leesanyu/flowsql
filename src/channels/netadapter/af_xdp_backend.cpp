// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include "af_xdp_backend.h"
#include <cerrno>
#include "netadapter_config.h"
#ifdef FLOWSQL_HAVE_AF_XDP
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <linux/bpf.h>
#include <linux/ethtool.h>
#include <linux/if_link.h>
#include <linux/if_packet.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include <xdp/xsk.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
namespace flowsql::channels::netadapter {
namespace {
constexpr uint32_t kFrameBytes = 4096, kRingEntries = 256;
constexpr uint64_t kDescriptorEnvelope = 64 * 1024;
int64_t RealtimeNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}
struct XdpDevice {
    std::string name;
    uint32_t ifindex = 0, queues = 0;
    int map = -1, program = -1, promiscuous = -1;
    bool attached = false;
    ~XdpDevice() {
        if (attached) {
            bpf_xdp_attach_opts options{};
            options.sz = sizeof(options);
            options.old_prog_fd = program;
            // Compare-and-detach: a replacement owned by someone else must survive.
            bpf_xdp_detach(ifindex, XDP_FLAGS_SKB_MODE | XDP_FLAGS_REPLACE, &options);
        }
        if (program >= 0) close(program);
        if (map >= 0) close(map);
        if (promiscuous >= 0) close(promiscuous);
    }
};
struct XdpQueue {
    xsk_socket* socket = nullptr;
    xsk_umem* umem = nullptr;
    xsk_ring_prod fill{};
    xsk_ring_cons completion{}, rx{};
    void* memory = MAP_FAILED;
    uint64_t capacity = 0, address = 0, received = 0;
    bool borrowed = false;
    int failure = 0;
    int64_t idle_time = 0;
    ~XdpQueue() {
        if (socket) xsk_socket__delete(socket);
        if (umem) xsk_umem__delete(umem);
        if (memory != MAP_FAILED) munmap(memory, capacity);
    }
};
bpf_insn Instruction(uint8_t code, uint8_t dst, uint8_t src, int16_t offset, int32_t immediate) {
    bpf_insn result{};
    result.code = code;
    result.dst_reg = dst;
    result.src_reg = src;
    result.off = offset;
    result.imm = immediate;
    return result;
}
int CreateRedirect(XdpDevice& device) {
    bpf_map_create_opts map_options{};
    map_options.sz = sizeof(map_options);
    device.map = bpf_map_create(BPF_MAP_TYPE_XSKMAP, "flowsql_xsks", sizeof(uint32_t), sizeof(uint32_t), device.queues,
                                &map_options);
    if (device.map < 0) return errno;
    const bpf_insn code[] = {
        Instruction(BPF_LDX | BPF_MEM | BPF_W, BPF_REG_2, BPF_REG_1, offsetof(xdp_md, rx_queue_index), 0),
        Instruction(BPF_LD | BPF_DW | BPF_IMM, BPF_REG_1, BPF_PSEUDO_MAP_FD, 0, device.map),
        Instruction(0, 0, 0, 0, 0),
        Instruction(BPF_ALU64 | BPF_MOV | BPF_K, BPF_REG_3, 0, 0, XDP_PASS),
        Instruction(BPF_JMP | BPF_CALL, 0, 0, 0, BPF_FUNC_redirect_map),
        Instruction(BPF_JMP | BPF_EXIT, 0, 0, 0, 0)};
    bpf_prog_load_opts options{};
    options.sz = sizeof(options);
    device.program =
        bpf_prog_load(BPF_PROG_TYPE_XDP, "flowsql_capture", "GPL", code, sizeof(code) / sizeof(code[0]), &options);
    return device.program < 0 ? errno : 0;
}
int DescribeDevice(const std::string& name, XdpDevice& device, std::string* error) {
    int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return errno;
    struct Fd {
        int fd;
        ~Fd() { close(fd); }
    } owner{fd};
    ifreq request{};
    if (name.size() >= IFNAMSIZ) return EINVAL;
    std::memcpy(request.ifr_name, name.c_str(), name.size() + 1);
    if (ioctl(fd, SIOCGIFHWADDR, &request) != 0) return errno;
    if (request.ifr_hwaddr.sa_family != ARPHRD_ETHER) return EPROTONOSUPPORT;
    if (ioctl(fd, SIOCGIFMTU, &request) != 0) return errno;
    // Single-frame COPY mode needs a complete Ethernet frame before applying the user's snaplen.
    if (request.ifr_mtu + 22 + 256 > int(kFrameBytes)) {
        if (error) *error = "AF_XDP COPY frame capacity cannot cover interface MTU: " + name;
        return EMSGSIZE;
    }
    device.name = name;
    device.ifindex = if_nametoindex(name.c_str());
    if (!device.ifindex) return ENODEV;
    ethtool_channels channels{};
    channels.cmd = ETHTOOL_GCHANNELS;
    request.ifr_data = reinterpret_cast<char*>(&channels);
    if (ioctl(fd, SIOCETHTOOL, &request) == 0) device.queues = channels.rx_count + channels.combined_count;
    if (!device.queues) {
        ethtool_value rings{};
        rings.cmd = ETHTOOL_GRXRINGS;
        request.ifr_data = reinterpret_cast<char*>(&rings);
        if (ioctl(fd, SIOCETHTOOL, &request) == 0) device.queues = rings.data;
    }
    if (!device.queues) {
        const auto root = std::filesystem::path("/sys/class/net") / name;
        uint32_t index = 0;
        std::ifstream(root / "ifindex") >> index;
        if (index == device.ifindex) {
            std::error_code ec;
            for (const auto& entry : std::filesystem::directory_iterator(root / "queues", ec)) {
                const auto queue = entry.path().filename().string();
                if (queue.rfind("rx-", 0) == 0)
                    device.queues = std::max(device.queues, uint32_t(std::stoul(queue.substr(3)) + 1));
            }
            if (ec) return EIO;
        }
    }
    if (!device.queues) return ENOTSUP;
    bpf_xdp_query_opts query{};
    query.sz = sizeof(query);
    const int rc = bpf_xdp_query(device.ifindex, 0, &query);
    if (rc) return -rc;
    if (query.prog_id || query.drv_prog_id || query.hw_prog_id || query.skb_prog_id) {
        if (error) *error = "AF_XDP refuses interface with an existing XDP owner: " + name;
        return EBUSY;
    }
    return 0;
}
class XdpSession final : public ICaptureBackendSessionV1 {
 public:
    const std::vector<CaptureBackendInputV1>& Inputs() const override { return inputs; }
    int TryRead(uint32_t i, CapturePacketViewV1* output) override {
        if (cancelled) return ECANCELED;
        if (!output || i >= queues.size()) return EINVAL;
        auto& queue = *queues[i];
        if (queue.failure) return queue.failure;
        if (queue.borrowed) return EBUSY;
        const auto now = RealtimeNs();
        uint32_t index = 0;
        if (!xsk_ring_cons__peek(&queue.rx, 1, &index)) {
            if (xsk_ring_prod__needs_wakeup(&queue.fill)) {
                pollfd fd{xsk_socket__fd(queue.socket), POLLIN, 0};
                poll(&fd, 1, 0);
            }
            queue.idle_time = now;
            return EAGAIN;
        }
        const auto* descriptor = xsk_ring_cons__rx_desc(&queue.rx, index);
        if (descriptor->options || descriptor->len > kFrameBytes || descriptor->addr > queue.capacity - descriptor->len)
            return EPROTO;
        queue.address = descriptor->addr;
        queue.borrowed = true;
        ++queue.received;
        *output = {static_cast<uint8_t*>(queue.memory) + descriptor->addr, std::min(snaplen, descriptor->len),
                   descriptor->len, now};
        return 0;
    }
    void ReleasePacket(uint32_t i) override {
        auto& queue = *queues[i];
        if (!queue.borrowed) return;
        xsk_ring_cons__release(&queue.rx, 1);
        uint32_t index = 0;
        if (xsk_ring_prod__reserve(&queue.fill, 1, &index) != 1)
            queue.failure = ENOBUFS;
        else {
            *xsk_ring_prod__fill_addr(&queue.fill, index) = queue.address & ~uint64_t(kFrameBytes - 1);
            xsk_ring_prod__submit(&queue.fill, 1);
        }
        queue.borrowed = false;
    }
    CaptureBacklogV1 Backlog(uint32_t i) const override {
        const auto& queue = *queues[i];
        if (queue.failure) return CaptureBacklogV1::kUnknown;
        const auto produced = __atomic_load_n(queue.rx.producer, __ATOMIC_ACQUIRE);
        const auto consumed = __atomic_load_n(queue.rx.consumer, __ATOMIC_ACQUIRE);
        return produced != consumed || queue.borrowed ? CaptureBacklogV1::kPresent : CaptureBacklogV1::kEmpty;
    }
    int64_t IdleTimeNs(uint32_t i) const override { return queues[i]->idle_time; }
    int ReadCounters(uint32_t i, CaptureCountersV1* output) override {
        if (!output || i >= queues.size()) return EINVAL;
        xdp_statistics statistics{};
        socklen_t size = sizeof(statistics);
        const int rc = getsockopt(xsk_socket__fd(queues[i]->socket), SOL_XDP, XDP_STATISTICS, &statistics, &size);
        *output = {};
        // No per-XSK kernel received counter exists; never present user-dequeued count as kernel received.
        if (rc == 0) {
            output->source_dropped_packets = statistics.rx_dropped;
            output->available_mask = kCaptureSourceDroppedPacketsAvailable;
        }
        return 0;
    }
    void Cancel() override { cancelled = true; }
    uint32_t snaplen = 65535;
    std::vector<CaptureBackendInputV1> inputs;
    std::vector<std::unique_ptr<XdpDevice>> devices;
    std::vector<std::unique_ptr<XdpQueue>> queues;

 private:
    std::atomic<bool> cancelled{false};
};
}  // namespace
int OpenAfXdpCopySkb(const CaptureBackendConfigV1& config, std::shared_ptr<ICaptureBackendSessionV1>* output,
                     std::string* error) {
    if (!output) return EINVAL;
    output->reset();
    auto fail = [&](int rc, const std::string& message) {
        if (error && error->empty()) *error = message;
        return rc;
    };
    if (error) error->clear();
    if (config.backend != "af_xdp_copy_skb" || config.interfaces.empty() || !config.snaplen || config.snaplen > 65535)
        return EINVAL;
    auto session = std::make_shared<XdpSession>();
    session->snaplen = config.snaplen;
    uint64_t inputs = 0;
    for (const auto& name : config.interfaces) {
        auto device = std::make_unique<XdpDevice>();
        const int rc = DescribeDevice(name, *device, error);
        if (rc)
            return fail(rc, "AF_XDP interface/queue/XDP ownership validation failed: " + name + ": " + strerror(rc));
        inputs += device->queues;
        session->devices.push_back(std::move(device));
    }
    if (config.buffer_bytes < kSharedEnvelopeBytes ||
        inputs > (config.buffer_bytes - kSharedEnvelopeBytes) / (kMinimumInputBytes + kDescriptorEnvelope))
        return fail(ENOMEM, "AF_XDP budget cannot cover every RX queue and shared Arrow capacity");
    const uint64_t per_input =
        std::max(inputs * (kMinimumInputBytes + kDescriptorEnvelope), config.buffer_bytes / 2) / inputs;
    uint32_t frames = 256;
    while (uint64_t(frames) * 2 * kFrameBytes + kDescriptorEnvelope <= per_input && frames < 1024) frames *= 2;
    for (const auto& device : session->devices) {
        const int created = CreateRedirect(*device);
        if (created) return fail(created, "AF_XDP BPF map/program unavailable");
        if (config.promiscuous) {
            device->promiscuous = ::socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, 0);
            if (device->promiscuous < 0) return fail(errno, "AF_XDP promiscuous membership socket unavailable");
            packet_mreq membership{};
            membership.mr_ifindex = device->ifindex;
            membership.mr_type = PACKET_MR_PROMISC;
            if (setsockopt(device->promiscuous, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &membership, sizeof(membership)) !=
                0)
                return fail(errno, "AF_XDP promiscuous membership failed");
        }
        for (uint32_t id = 0; id < device->queues; ++id) {
            auto queue = std::make_unique<XdpQueue>();
            queue->capacity = uint64_t(frames) * kFrameBytes;
            queue->memory = mmap(nullptr, queue->capacity, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (queue->memory == MAP_FAILED) return fail(errno, "AF_XDP UMEM allocation failed");
            xsk_umem_config umem{};
            umem.fill_size = frames;
            umem.comp_size = frames;
            umem.frame_size = kFrameBytes;
            int rc =
                xsk_umem__create(&queue->umem, queue->memory, queue->capacity, &queue->fill, &queue->completion, &umem);
            if (rc) return fail(-rc, "AF_XDP UMEM registration failed");
            xsk_socket_config socket{};
            socket.rx_size = frames;
            socket.libxdp_flags = XSK_LIBXDP_FLAGS__INHIBIT_PROG_LOAD;
            socket.xdp_flags = XDP_FLAGS_SKB_MODE;
            socket.bind_flags = XDP_COPY | XDP_USE_NEED_WAKEUP;
            rc =
                xsk_socket__create(&queue->socket, device->name.c_str(), id, queue->umem, &queue->rx, nullptr, &socket);
            if (rc) return fail(-rc, "AF_XDP SKB/COPY socket failed: " + device->name + " queue=" + std::to_string(id));
            xdp_options mode{};
            socklen_t mode_size = sizeof(mode);
            if (getsockopt(xsk_socket__fd(queue->socket), SOL_XDP, XDP_OPTIONS, &mode, &mode_size) != 0 ||
                mode.flags & XDP_OPTIONS_ZEROCOPY)
                return fail(EPROTONOSUPPORT, "AF_XDP actual mode is not COPY");
            uint32_t fill = 0;
            if (xsk_ring_prod__reserve(&queue->fill, frames, &fill) != frames)
                return fail(ENOBUFS, "AF_XDP initial FILL capacity failed");
            for (uint32_t n = 0; n < frames; ++n)
                *xsk_ring_prod__fill_addr(&queue->fill, fill + n) = uint64_t(n) * kFrameBytes;
            xsk_ring_prod__submit(&queue->fill, frames);
            const int socket_fd = xsk_socket__fd(queue->socket);
            if (bpf_map_update_elem(device->map, &id, &socket_fd, BPF_ANY) != 0)
                return fail(errno, "AF_XDP queue map registration failed");
            xdp_mmap_offsets offsets{};
            socklen_t length = sizeof(offsets);
            if (getsockopt(socket_fd, SOL_XDP, XDP_MMAP_OFFSETS, &offsets, &length) != 0)
                return fail(errno, "AF_XDP mmap capacity discovery failed");
            const uint64_t page = sysconf(_SC_PAGESIZE);
            auto aligned = [&](uint64_t bytes) { return (bytes + page - 1) / page * page; };
            const uint64_t descriptors = aligned(offsets.rx.desc + frames * sizeof(xdp_desc)) +
                                         aligned(offsets.fr.desc + frames * sizeof(uint64_t)) +
                                         aligned(offsets.cr.desc + frames * sizeof(uint64_t));
            if (descriptors > kDescriptorEnvelope)
                return fail(ENOMEM, "AF_XDP descriptor mappings exceed reserved capacity");
            CaptureBackendInputV1 input;
            input.interface_name = device->name;
            input.ifindex = device->ifindex;
            input.queue_id = id;
            input.physical_queue = true;
            input.readiness_fd = socket_fd;
            input.buffer_bytes = queue->capacity + descriptors;
            input.timestamp_source = "user RX-dequeue Unix clock";
            input.counter_source =
                "XDP_STATISTICS rx_dropped; received unavailable; SKB/COPY redirect consumes host RX";
            session->inputs.push_back(std::move(input));
            session->queues.push_back(std::move(queue));
        }
        bpf_xdp_attach_opts options{};
        options.sz = sizeof(options);
        const int rc = bpf_xdp_attach(device->ifindex, device->program,
                                      XDP_FLAGS_SKB_MODE | XDP_FLAGS_UPDATE_IF_NOEXIST, &options);
        if (rc) return fail(-rc, "AF_XDP SKB attach failed; existing owners preserved");
        device->attached = true;
    }
    *output = std::move(session);
    return 0;
}
}  // namespace flowsql::channels::netadapter
#else
namespace flowsql::channels::netadapter {
int OpenAfXdpCopySkb(const CaptureBackendConfigV1&, std::shared_ptr<ICaptureBackendSessionV1>* output,
                     std::string* error) {
    if (!output) return EINVAL;
    output->reset();
    if (error) *error = "AF_XDP SKB/COPY unavailable: libxdp/libbpf build dependencies missing";
    return ENODEV;
}
}  // namespace flowsql::channels::netadapter
#endif
