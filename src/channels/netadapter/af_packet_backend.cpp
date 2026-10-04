// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include "af_packet_backend.h"
#include <arpa/inet.h>
#include <linux/filter.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include "netadapter_config.h"
namespace flowsql::channels::netadapter {
namespace {
constexpr uint32_t kBlockBytes = 1024 * 1024;
int64_t RealtimeNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}
struct Ring {
    int fd = -1;
    void* mapping = MAP_FAILED;
    size_t capacity = 0;
    uint32_t blocks = 0, block = 0, remaining = 0, offset = 0;
    tpacket3_hdr* borrowed = nullptr;
    uint64_t received = 0, dropped = 0, consumed = 0;
    bool counters_valid = false;
    int64_t idle_time = 0;
    ~Ring() {
        if (mapping != MAP_FAILED) munmap(mapping, capacity);
        if (fd >= 0) close(fd);
    }
    tpacket_block_desc* Current() const {
        return reinterpret_cast<tpacket_block_desc*>(static_cast<uint8_t*>(mapping) + size_t(block) * kBlockBytes);
    }
    bool UserBlock() const {
        return __atomic_load_n(&Current()->hdr.bh1.block_status, __ATOMIC_ACQUIRE) & TP_STATUS_USER;
    }
    int Statistics() {
        tpacket_stats_v3 value{};
        socklen_t size = sizeof(value);
        if (getsockopt(fd, SOL_PACKET, PACKET_STATISTICS, &value, &size) != 0) {
            counters_valid = false;
            return errno;
        }
        received += value.tp_packets;
        dropped += value.tp_drops;
        counters_valid = true;
        return 0;
    }
};
class AfPacketSession final : public ICaptureBackendSessionV1 {
 public:
    const std::vector<CaptureBackendInputV1>& Inputs() const override { return inputs; }
    int TryRead(uint32_t index, CapturePacketViewV1* view) override {
        if (cancelled) return ECANCELED;
        if (!view || index >= rings.size()) return EINVAL;
        auto& ring = *rings[index];
        if (ring.borrowed) return EBUSY;
        if (!ring.remaining) {
            if (!ring.UserBlock()) {
                // The sampled time precedes the counter snapshot: later arrivals cannot invalidate this earlier
                // boundary.
                const auto now = RealtimeNs();
                const int rc = ring.Statistics();
                if (rc == 0 && ring.consumed == ring.received - ring.dropped && !ring.UserBlock())
                    ring.idle_time = now;
                else
                    ring.idle_time = 0;
                return EAGAIN;
            }
            ring.remaining = ring.Current()->hdr.bh1.num_pkts;
            ring.offset = ring.Current()->hdr.bh1.offset_to_first_pkt;
            if (!ring.remaining) {
                ReturnBlock(ring);
                return EAGAIN;
            }
        }
        if (ring.offset > kBlockBytes - sizeof(tpacket3_hdr)) return EPROTO;
        auto* packet = reinterpret_cast<tpacket3_hdr*>(reinterpret_cast<uint8_t*>(ring.Current()) + ring.offset);
        if (packet->tp_snaplen > packet->tp_len || packet->tp_mac > kBlockBytes - ring.offset ||
            packet->tp_snaplen > kBlockBytes - ring.offset - packet->tp_mac || packet->tp_nsec >= 1000000000)
            return EPROTO;
        ring.borrowed = packet;
        *view = {reinterpret_cast<uint8_t*>(packet) + packet->tp_mac, packet->tp_snaplen, packet->tp_len,
                 int64_t(packet->tp_sec) * 1000000000 + packet->tp_nsec};
        return 0;
    }
    void ReleasePacket(uint32_t index) override {
        auto& ring = *rings[index];
        if (!ring.borrowed) return;
        const auto offset = ring.borrowed->tp_next_offset;
        ring.borrowed = nullptr;
        ++ring.consumed;
        if (--ring.remaining == 0)
            ReturnBlock(ring);
        else
            ring.offset += offset;
    }
    CaptureBacklogV1 Backlog(uint32_t index) const override {
        const auto& ring = *rings[index];
        if (ring.remaining || ring.UserBlock()) return CaptureBacklogV1::kPresent;
        // Kernel-owned partially filled V3 blocks are invisible until retirement. Counters must prove them empty.
        if (!ring.counters_valid) return CaptureBacklogV1::kUnknown;
        return ring.consumed == ring.received - ring.dropped ? CaptureBacklogV1::kEmpty : CaptureBacklogV1::kPresent;
    }
    int64_t IdleTimeNs(uint32_t index) const override { return rings[index]->idle_time; }
    int ReadCounters(uint32_t index, CaptureCountersV1* output) override {
        if (!output || index >= rings.size()) return EINVAL;
        auto& ring = *rings[index];
        const int rc = ring.Statistics();
        if (rc) return rc;
        *output = {};
        output->received_packets = ring.received;
        output->source_dropped_packets = ring.dropped;
        output->available_mask = kCaptureReceivedPacketsAvailable | kCaptureSourceDroppedPacketsAvailable;
        return 0;
    }
    void Cancel() override { cancelled = true; }
    std::vector<CaptureBackendInputV1> inputs;
    std::vector<std::unique_ptr<Ring>> rings;

 private:
    static void ReturnBlock(Ring& ring) {
        __atomic_store_n(&ring.Current()->hdr.bh1.block_status, TP_STATUS_KERNEL, __ATOMIC_RELEASE);
        ring.block = (ring.block + 1) % ring.blocks;
        ring.remaining = 0;
    }
    std::atomic<bool> cancelled{false};
};
}  // namespace
int OpenAfPacket(const CaptureBackendConfigV1& config, std::shared_ptr<ICaptureBackendSessionV1>* output,
                 std::string* error) {
    if (!output) return EINVAL;
    output->reset();
    auto fail = [&](int code, const std::string& message) {
        if (error) *error = message;
        return code;
    };
    if (config.backend != "af_packet" || config.interfaces.empty() || !config.snaplen || config.snaplen > 65535)
        return EINVAL;
    if (config.buffer_bytes < kSharedEnvelopeBytes + config.interfaces.size() * kMinimumInputBytes)
        return fail(ENOMEM, "AF_PACKET budget cannot cover all interfaces");
    const auto ring_budget = std::max(config.interfaces.size() * kMinimumInputBytes, config.buffer_bytes / 2);
    const auto count =
        std::min<uint64_t>(UINT32_MAX / kBlockBytes, ring_budget / config.interfaces.size() / kBlockBytes);
    auto session = std::make_shared<AfPacketSession>();
    for (const auto& name : config.interfaces) {
        auto ring = std::make_unique<Ring>();
        ring->fd = socket(AF_PACKET, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (ring->fd < 0) return fail(errno, "AF_PACKET socket unavailable for " + name + ": " + strerror(errno));
        ifreq request{};
        if (name.size() >= IFNAMSIZ) return EINVAL;
        std::memcpy(request.ifr_name, name.c_str(), name.size() + 1);
        if (ioctl(ring->fd, SIOCGIFHWADDR, &request) != 0) return fail(errno, "interface unavailable: " + name);
        if (request.ifr_hwaddr.sa_family != ARPHRD_ETHER)
            return fail(EPROTONOSUPPORT, "non-Ethernet interface: " + name);
        const auto ifindex = if_nametoindex(name.c_str());
        if (!ifindex) return fail(ENODEV, "interface index unavailable: " + name);
        int version = TPACKET_V3;
        if (setsockopt(ring->fd, SOL_PACKET, PACKET_VERSION, &version, sizeof(version)) != 0)
            return fail(errno, "TPACKET_V3 unavailable");
        sock_filter instruction{BPF_RET | BPF_K, 0, 0, config.snaplen};
        sock_fprog filter{1, &instruction};
        if (setsockopt(ring->fd, SOL_SOCKET, SO_ATTACH_FILTER, &filter, sizeof(filter)) != 0)
            return fail(errno, "snaplen filter unavailable");
        tpacket_req3 setup{};
        setup.tp_block_size = kBlockBytes;
        setup.tp_block_nr = count;
        setup.tp_frame_size = 2048;
        setup.tp_frame_nr = count * (kBlockBytes / setup.tp_frame_size);
        setup.tp_retire_blk_tov = 1;
        if (setsockopt(ring->fd, SOL_PACKET, PACKET_RX_RING, &setup, sizeof(setup)) != 0)
            return fail(errno, "RX_RING allocation failed: " + name);
        ring->blocks = count;
        ring->capacity = size_t(count) * kBlockBytes;
        ring->mapping = mmap(nullptr, ring->capacity, PROT_READ | PROT_WRITE, MAP_SHARED, ring->fd, 0);
        if (ring->mapping == MAP_FAILED) return fail(errno, "RX_RING mmap failed: " + name);
        sockaddr_ll address{};
        address.sll_family = AF_PACKET;
        address.sll_protocol = htons(ETH_P_ALL);
        address.sll_ifindex = ifindex;
        if (bind(ring->fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
            return fail(errno, "interface bind failed: " + name);
        if (config.promiscuous) {
            packet_mreq membership{};
            membership.mr_ifindex = ifindex;
            membership.mr_type = PACKET_MR_PROMISC;
            if (setsockopt(ring->fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &membership, sizeof(membership)) != 0)
                return fail(errno, "promiscuous membership failed: " + name);
        }
        CaptureBackendInputV1 input;
        input.interface_name = name;
        input.ifindex = ifindex;
        input.readiness_fd = ring->fd;
        input.buffer_bytes = ring->capacity;
        input.timestamp_source = "kernel software RX timestamp (Unix ns)";
        input.counter_source = "PACKET_STATISTICS accumulated";
        session->inputs.push_back(std::move(input));
        session->rings.push_back(std::move(ring));
    }
    *output = std::move(session);
    return 0;
}
}  // namespace flowsql::channels::netadapter
