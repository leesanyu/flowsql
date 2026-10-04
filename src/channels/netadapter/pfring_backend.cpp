// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include "pfring_backend.h"
#include <cerrno>
#include "netadapter_config.h"
#ifdef FLOWSQL_HAVE_PFRING
#include <net/if.h>
#include <net/if_arp.h>
#pragma push_macro("interface")
#undef interface
#include <pfring.h>
#pragma pop_macro("interface")
#include <sys/ioctl.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <fstream>
namespace flowsql::channels::netadapter {
namespace {
struct ClassicRing {
    pfring* ring = nullptr;
    std::vector<uint8_t> scratch;
    bool borrowed = false;
    int64_t idle_time = 0;
    ~ClassicRing() {
        if (ring) pfring_close(ring);
    }
};
class ClassicSession final : public ICaptureBackendSessionV1 {
 public:
    const std::vector<CaptureBackendInputV1>& Inputs() const override { return inputs; }
    int TryRead(uint32_t i, CapturePacketViewV1* out) override {
        if (cancelled) return ECANCELED;
        if (!out || i >= rings.size()) return EINVAL;
        auto& input = *rings[i];
        if (input.borrowed) return EBUSY;
        const auto now =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();
        pfring_pkthdr header{};
        auto* bytes = input.scratch.data();
        // Classic recv publishes its remove index before returning. Request its bounded copy, never borrow the recycled
        // slot.
        const int rc = pfring_recv(input.ring, &bytes, input.scratch.size(), &header, 0);
        if (rc < 0) return EIO;
        if (rc == 0) {
            input.idle_time = pfring_get_num_queued_pkts(input.ring) == 0 ? now : 0;
            return EAGAIN;
        }
        if (header.caplen > input.scratch.size() || header.caplen > header.len) return EPROTO;
        input.borrowed = true;
        *out = {bytes, header.caplen, header.len, int64_t(header.ts.tv_sec) * 1000000000 + header.ts.tv_usec * 1000};
        return 0;
    }
    void ReleasePacket(uint32_t i) override { rings[i]->borrowed = false; }
    CaptureBacklogV1 Backlog(uint32_t i) const override {
        return rings[i]->borrowed || pfring_get_num_queued_pkts(rings[i]->ring) > 0 ? CaptureBacklogV1::kPresent
                                                                                    : CaptureBacklogV1::kEmpty;
    }
    int64_t IdleTimeNs(uint32_t i) const override { return rings[i]->idle_time; }
    int ReadCounters(uint32_t i, CaptureCountersV1* out) override {
        if (!out || i >= rings.size()) return EINVAL;
        pfring_stat counters{};
        if (pfring_stats(rings[i]->ring, &counters) != 0) return ENOTSUP;
        *out = {};
        out->received_packets = counters.recv;
        out->source_dropped_packets = counters.drop;
        out->available_mask = kCaptureReceivedPacketsAvailable | kCaptureSourceDroppedPacketsAvailable;
        return 0;
    }
    void Cancel() override { cancelled = true; }
    std::vector<CaptureBackendInputV1> inputs;
    std::vector<std::unique_ptr<ClassicRing>> rings;

 private:
    std::atomic<bool> cancelled{false};
};
}  // namespace
int OpenPfringClassic(const CaptureBackendConfigV1& config, std::shared_ptr<ICaptureBackendSessionV1>* out,
                      std::string* error) {
    if (!out) return EINVAL;
    out->reset();
    auto fail = [&](int rc, const std::string& message) {
        if (error) *error = message;
        return rc;
    };
    if (config.backend != "pfring_classic" || config.interfaces.empty() || !config.snaplen || config.snaplen > 65535)
        return EINVAL;
    uint64_t slots = 0;
    std::ifstream parameter("/sys/module/pf_ring/parameters/min_num_slots");
    if (!(parameter >> slots) || slots < MIN_NUM_SLOTS)
        return fail(ENODEV, "PF_RING Classic kernel module unavailable or ring capacity cannot be established");
    // Account the kernel ring before pfring_open allocates it, including alignment/page slack and the safe recv
    // scratch.
    // pfring_open without PF_RING_LONG_HEADER selects this short header before mmap. Include magic/alignment slack.
    const uint64_t header_bound = offsetof(pfring_pkthdr, extended_hdr.tx) + 16;
    const uint64_t slot_memory =
        config.snaplen > 1600
            ? std::max(slots * (1600 + header_bound), uint64_t(MIN_NUM_SLOTS) * (config.snaplen + header_bound))
            : slots * (config.snaplen + header_bound);
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) return fail(ENOTSUP, "PF_RING Classic page alignment cannot be established");
    const uint64_t per_ring = slot_memory + sizeof(FlowSlotInfo) + page_size + SHMLBA + config.snaplen;
    if (config.buffer_bytes < kSharedEnvelopeBytes ||
        per_ring > (config.buffer_bytes - kSharedEnvelopeBytes) / config.interfaces.size())
        return fail(ENOMEM, "PF_RING Classic min_num_slots/snaplen cannot fit the whole-channel budget");
    auto session = std::make_shared<ClassicSession>();
    uint64_t capacity = 0;
    for (const auto& name : config.interfaces) {
        int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (fd < 0) return fail(errno, "Ethernet validation socket unavailable");
        ifreq query{};
        if (name.size() >= IFNAMSIZ) {
            close(fd);
            return EINVAL;
        }
        std::memcpy(query.ifr_name, name.c_str(), name.size() + 1);
        const int checked = ioctl(fd, SIOCGIFHWADDR, &query);
        const int saved = errno;
        close(fd);
        if (checked != 0) return fail(saved, "interface unavailable: " + name);
        if (query.ifr_hwaddr.sa_family != ARPHRD_ETHER) return fail(EPROTONOSUPPORT, "non-Ethernet interface: " + name);
        auto input = std::make_unique<ClassicRing>();
        input->scratch.resize(config.snaplen);
        input->ring = pfring_open(name.c_str(), config.snaplen, config.promiscuous ? PF_RING_PROMISC : 0);
        if (!input->ring) return fail(errno ? errno : ENODEV, "PF_RING Classic open failed: " + name);
        if (!input->ring->slots_info || !input->ring->buffer)
            return fail(EPROTONOSUPPORT, "PF_RING backend is not a Classic mapped ring");
        const auto actual = input->ring->slots_info->tot_mem + input->scratch.capacity();
        if (actual > per_ring || actual > config.buffer_bytes - kSharedEnvelopeBytes - capacity)
            return fail(ENOMEM, "actual PF_RING Classic capacity exceeds budget");
        capacity += actual;
        if (pfring_set_poll_watermark(input->ring, 1) != 0 ||
            pfring_set_direction(input->ring, rx_and_tx_direction) != 0 ||
            pfring_set_socket_mode(input->ring, recv_only_mode) != 0)
            return fail(EIO, "PF_RING Classic receive scope/setup failed: " + name);
        CaptureBackendInputV1 description;
        description.interface_name = name;
        description.ifindex = if_nametoindex(name.c_str());
        description.buffer_bytes = actual;
        description.readiness_fd = pfring_get_selectable_fd(input->ring);
        description.timestamp_source = "PF_RING Classic software timeval (Unix ns conversion)";
        description.counter_source = "pfring_stats; Classic safe scratch copy then Arrow payload copy";
        session->inputs.push_back(std::move(description));
        session->rings.push_back(std::move(input));
    }
    for (const auto& input : session->rings)
        if (pfring_enable_ring(input->ring) != 0) return fail(EIO, "PF_RING Classic enable failed");
    *out = std::move(session);
    return 0;
}
}  // namespace flowsql::channels::netadapter
#else
namespace flowsql::channels::netadapter {
int OpenPfringClassic(const CaptureBackendConfigV1&, std::shared_ptr<ICaptureBackendSessionV1>* out,
                      std::string* error) {
    if (!out) return EINVAL;
    out->reset();
    if (error) *error = "PF_RING Classic unavailable: build with PF_RING headers/library (FLOWSQL_PFRING_ROOT)";
    return ENODEV;
}
}  // namespace flowsql::channels::netadapter
#endif
