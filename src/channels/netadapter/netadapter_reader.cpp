// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include "netadapter_reader.h"
#include <channels/pcapfile/packet_filter_domain.h>
#include <framework/core/packet_codec.h>
#include <plugins/npi/packet_decoder.h>
#include <poll.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <chrono>
#include <set>
namespace flowsql::channels::netadapter {
NetAdapterReader::NetAdapterReader(std::string name, CaptureBackendConfigV1 config, uint64_t domain,
                                   uint64_t generation, std::shared_ptr<ICaptureBackendSessionV1> backend,
                                   IProtocol* protocol, std::shared_ptr<void> lease,
                                   std::shared_ptr<const packet::PcapFilterPlan> filter, MonotonicNow monotonic_now)
    : name_(std::move(name)),
      config_(std::move(config)),
      sources_(DefaultSourceSet()),
      backend_(std::move(backend)),
      protocol_(protocol),
      channel_lease_(std::move(lease)),
      filter_(std::move(filter)),
      monotonic_now_(std::move(monotonic_now)) {
    for (const auto& input : backend_->Inputs()) {
        source_names_.push_back(input.interface_name + ":" + (input.physical_queue ? "rx" : "logical") +
                                std::to_string(input.queue_id));
        backend_bytes_ += input.buffer_bytes;
    }
    for (size_t i = 0; i < source_names_.size(); ++i) {
        CaptureQueueIdentityV1 identity;
        identity.source_name = source_names_[i].c_str();
        identity.source_id = i;
        identity.observation_domain_id = domain;
        identity.generation = generation;
        identity.queue_id = backend_->Inputs()[i].queue_id;
        identity.link_type = 1;
        sources_.inputs.push_back(identity);
    }
    counters_.resize(source_names_.size());
    fact_sequences_.resize(source_names_.size());
    backlogs_.resize(source_names_.size(), CaptureBacklogV1::kUnknown);
    candidates_.resize(source_names_.size());
    for (auto& counters : counters_) {
        counters.generation = generation;
        counters.available_mask = kCaptureDeliveredPacketsAvailable | kCaptureDeliveredBytesAvailable |
                                  kCaptureQueueDroppedPacketsAvailable | kCaptureBackpressureEventsAvailable;
    }
}
NetAdapterReader::~NetAdapterReader() {
    Cancel();
    if (cancel_fd_ >= 0) close(cancel_fd_);
}
int NetAdapterReader::Open() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (opened_) return EALREADY;
    if (ValidateCaptureSourceSetV2(sources_) != CaptureDescriptionErrorV1::kNone ||
        backend_bytes_ + kSharedEnvelopeBytes > config_.buffer_bytes)
        return ENOMEM;
    pool_ = std::make_shared<CaptureMemoryPool>(config_.buffer_bytes - backend_bytes_);
    cancel_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (cancel_fd_ < 0) return errno;
    opened_ = true;
    return 0;
}
int NetAdapterReader::Close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!outstanding_.empty() || (pool_ && pool_->bytes_allocated())) return EBUSY;
    if (!terminal_) return EBUSY;
    opened_ = false;
    return 0;
}
int NetAdapterReader::Flush() {
    int expected = 0;
    terminal_.compare_exchange_strong(expected, 1);
    if (cancel_fd_ >= 0) {
        uint64_t one = 1;
        const auto rc = write(cancel_fd_, &one, sizeof(one));
        (void)rc;
    }
    return 0;
}
void NetAdapterReader::Cancel() {
    int expected = 0;
    terminal_.compare_exchange_strong(expected, 2);
    if (cancel_fd_ >= 0) {
        uint64_t one = 1;
        const auto rc = write(cancel_fd_, &one, sizeof(one));
        (void)rc;
    }
    backend_->Cancel();
}
int NetAdapterReader::DescribeSources(CaptureSourceSetV2* output) const {
    if (!output || output->struct_size < sizeof(*output) || output->contract_version != 2) return EINVAL;
    *output = sources_;
    return 0;
}
int NetAdapterReader::ReleaseBlock(const std::shared_ptr<arrow::RecordBatch>& block) {
    std::lock_guard<std::mutex> lock(mutex_);
    return block && outstanding_.erase(block.get()) ? 0 : EINVAL;
}
int NetAdapterReader::ReadInputCounters(uint32_t source, CaptureCountersV1* output) const {
    if (!output || source >= counters_.size() || output->struct_size < sizeof(*output) || output->contract_version != 1)
        return EINVAL;
    std::lock_guard<std::mutex> lock(mutex_);
    *output = counters_[source];
    return 0;
}
CapturePollEventV2 NetAdapterReader::PollCapture(int timeout_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    CapturePollEventV2 event;
    auto fail = [&] {
        int expected = 0;
        terminal_.compare_exchange_strong(expected, 3);
    };
    auto terminal = [&] {
        event.progress.clear();
        if (terminal_ == 1) {
            event.block = {eof_reported_ ? BlockPollEvent::kTimeout : BlockPollEvent::kEof, nullptr, 0};
            eof_reported_ = true;
            return event;
        }
        event.block = {terminal_ == 2 ? BlockPollEvent::kCancelled : BlockPollEvent::kError, nullptr,
                       terminal_ == 2 ? 0 : EIO};
        return event;
    };
    if (terminal_) return terminal();
    if (!opened_ || timeout_ms < 0) {
        event.block = {BlockPollEvent::kError, nullptr, EINVAL};
        return event;
    }
    if (outstanding_.size() >= sources_.limits.max_outstanding_batches) {
        for (auto& c : counters_) ++c.backpressure_events;
        return event;
    }
    const size_t n = sources_.inputs.size();
    std::vector<bool> checked(n), empty(n);
    std::vector<std::optional<int64_t>> packet_times(n);
    std::vector<uint32_t> packets(n);
    std::vector<uint64_t> bytes(n);
    uint32_t inspected = 0;
    uint64_t inspected_bytes = 0, delivered_bytes = 0;
    bool waited = false;
    std::optional<uint32_t> borrowed;
    auto release = [&] {
        if (borrowed) {
            backend_->ReleasePacket(*borrowed);
            borrowed.reset();
        }
    };
    struct Guard {
        std::function<void()> release;
        ~Guard() { release(); }
    } guard{release};
    protocol::NpiPacketLayerDecoder decoder(protocol_);
    std::optional<std::chrono::steady_clock::time_point> started;
    std::chrono::steady_clock::duration wait_elapsed{};
    const auto expired = [&] {
        return monotonic_now_() - *started - wait_elapsed >= std::chrono::milliseconds(sources_.max_poll_work_ms);
    };
    const auto next = [&](packet::PacketRecord* record) {
        release();
        if (!started) started = monotonic_now_();
        while (!terminal_ && inspected < sources_.max_inspected_packets_per_poll &&
               inspected_bytes < sources_.max_inspected_bytes_per_poll && !expired()) {
            bool attempted = false;
            for (size_t step = 0; step < n; ++step) {
                if (inspected >= sources_.max_inspected_packets_per_poll ||
                    inspected_bytes >= sources_.max_inspected_bytes_per_poll || expired())
                    break;
                const size_t i = next_input_;
                next_input_ = (next_input_ + 1) % n;
                if (empty[i] || packets[i] >= kInputPacketQuantum || bytes[i] >= kInputByteQuantum) continue;
                attempted = true;
                checked[i] = true;
                CapturePacketViewV1 view;
                const int rc = backend_->TryRead(i, &view);
                if (rc == EAGAIN) {
                    empty[i] = true;
                    continue;
                }
                if (rc != 0) {
                    fail();
                    return rc;
                }
                borrowed = i;
                ++packets[i];
                ++inspected;
                bytes[i] += view.captured_len;
                inspected_bytes += view.captured_len;
                if (!view.bytes || view.captured_len > view.wire_len || view.captured_len > config_.snaplen ||
                    view.timestamp_ns < 0) {
                    fail();
                    return EINVAL;
                }
                const auto max_size = sources_.limits.max_bytes_per_batch;
                if (delivered_bytes + view.captured_len > max_size) {
                    ++counters_[i].queue_dropped_packets;
                    release();
                    return ENOENT;
                }
                packet::PacketView packet_view;
                packet_view.meta = {view.timestamp_ns,        view.captured_len, view.wire_len, 1,
                                    static_cast<uint32_t>(i), sequence_ + 1};
                packet_view.bytes = {view.bytes, view.captured_len};
                auto layer =
                    decoder.Decode(packet_view, {packet::kExtractMac | packet::kExtractIp | packet::kExtractPort,
                                                 packet::EndpointScope::kInnermost});
                if (filter_ && !pcapfile::EvaluatePcapDecodedFilter(*filter_, packet_view.meta, layer)) {
                    release();
                    continue;
                }
                if (sequence_ == UINT64_MAX) {
                    fail();
                    return EOVERFLOW;
                }
                ++sequence_;
                record->meta = packet_view.meta;
                record->layer = layer;
                record->raw_data = {backend_, view.bytes, view.captured_len};
                delivered_bytes += view.captured_len;
                ++counters_[i].delivered_packets;
                counters_[i].delivered_bytes += view.captured_len;
                packet_times[i] = std::max(packet_times[i].value_or(view.timestamp_ns), view.timestamp_ns);
                return 0;
            }
            if (!attempted || std::all_of(empty.begin(), empty.end(), [](bool v) { return v; })) {
                if (delivered_bytes || inspected || waited || timeout_ms == 0) break;
                std::vector<pollfd> fds{{cancel_fd_, POLLIN, 0}};
                for (const auto& input : backend_->Inputs())
                    if (input.readiness_fd >= 0) fds.push_back({input.readiness_fd, POLLIN, 0});
                const auto wait_start = monotonic_now_();
                const int poll_rc =
                    poll(fds.data(), fds.size(), std::min(timeout_ms, int(sources_.limits.max_wait_ms)));
                wait_elapsed += monotonic_now_() - wait_start;
                waited = true;
                if (poll_rc < 0 && errno != EINTR) {
                    fail();
                    return errno;
                }
                std::fill(empty.begin(), empty.end(), false);
            }
        }
        return ENOENT;
    };
    std::shared_ptr<arrow::RecordBatch> batch;
    std::string error;
    const auto encoded =
        packet::EncodePacketStream(next, sources_.limits.max_packets_per_batch, pool_.get(), &batch, &error);
    release();
    if (terminal_) return terminal();
    if (encoded != packet::PacketBatchError::kNone) {
        fail();
        return terminal();
    }
    for (size_t i = 0; i < n; ++i) {
        if (!checked[i]) continue;
        CaptureCountersV1 measured;
        if (backend_->ReadCounters(i, &measured) == 0) {
            auto& c = counters_[i];
            c.received_packets = measured.received_packets;
            c.source_dropped_packets = measured.source_dropped_packets;
            c.available_mask =
                (c.available_mask & ~(kCaptureReceivedPacketsAvailable | kCaptureSourceDroppedPacketsAvailable)) |
                measured.available_mask & (kCaptureReceivedPacketsAvailable | kCaptureSourceDroppedPacketsAvailable);
        }
        backlogs_[i] = backend_->Backlog(i);
        CaptureProgressV1 fact;
        fact.source_id = i;
        fact.queue_id = sources_.inputs[i].queue_id;
        fact.generation = sources_.inputs[i].generation;
        fact.fact_sequence = ++fact_sequences_[i];
        fact.backlog = backlogs_[i];
        fact.packet_observed = packet_times[i].has_value();
        fact.source_idle_confirmed = !fact.packet_observed && empty[i] && fact.backlog == CaptureBacklogV1::kEmpty;
        fact.capture_time_ns = packet_times[i].value_or(fact.source_idle_confirmed ? backend_->IdleTimeNs(i) : 0);
        candidates_[i] = fact.capture_time_ns;
        event.progress.push_back(fact);
    }
    if (batch->num_rows()) {
        std::vector<std::shared_ptr<arrow::Array>> arrays;
        for (const auto& column : batch->columns())
            arrays.push_back(arrow::MakeArray(RetainCaptureBuffers(column->data(), channel_lease_, pool_)));
        batch = arrow::RecordBatch::Make(batch->schema(), batch->num_rows(), std::move(arrays));
        outstanding_.emplace(batch.get(), batch);
        event.block = {BlockPollEvent::kData, batch, 0};
    }
    return event;
}
std::string NetAdapterReader::Diagnostics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> w(buffer);
    w.StartObject();
    w.Key("generation");
    w.Uint64(sources_.inputs.front().generation);
    w.Key("observation_domain_id");
    w.Uint64(sources_.inputs.front().observation_domain_id);
    w.Key("max_packets_per_batch");
    w.Uint(sources_.limits.max_packets_per_batch);
    w.Key("max_bytes_per_batch");
    w.Uint64(sources_.limits.max_bytes_per_batch);
    w.Key("max_wait_ms");
    w.Uint(sources_.limits.max_wait_ms);
    w.Key("max_outstanding_batches");
    w.Uint(sources_.limits.max_outstanding_batches);
    w.Key("max_inspected_packets_per_poll");
    w.Uint(sources_.max_inspected_packets_per_poll);
    w.Key("max_inspected_bytes_per_poll");
    w.Uint64(sources_.max_inspected_bytes_per_poll);
    w.Key("max_poll_work_ms");
    w.Uint(sources_.max_poll_work_ms);
    w.Key("input_packet_quantum");
    w.Uint(kInputPacketQuantum);
    w.Key("input_byte_quantum");
    w.Uint64(kInputByteQuantum);
    w.Key("shared_envelope_min_bytes");
    w.Uint64(kSharedEnvelopeBytes);
    w.Key("minimum_input_bytes");
    w.Uint64(kMinimumInputBytes);
    w.Key("backend_bytes");
    w.Uint64(backend_bytes_);
    w.Key("arrow_budget_bytes");
    w.Uint64(config_.buffer_bytes - backend_bytes_);
    w.Key("arrow_bytes");
    w.Int64(pool_ ? pool_->bytes_allocated() : 0);
    w.Key("arrow_peak_bytes");
    w.Int64(pool_ ? pool_->max_memory() : 0);
    w.Key("copy_path");
    w.String("backend borrowed view -> bounded Arrow builders; one payload copy");
    w.Key("totals");
    w.StartObject();
    for (const auto field :
         {kCaptureReceivedPacketsAvailable, kCaptureSourceDroppedPacketsAvailable, kCaptureDeliveredPacketsAvailable,
          kCaptureDeliveredBytesAvailable, kCaptureQueueDroppedPacketsAvailable}) {
        const char* key = field == kCaptureReceivedPacketsAvailable        ? "received_packets"
                          : field == kCaptureSourceDroppedPacketsAvailable ? "source_dropped_packets"
                          : field == kCaptureDeliveredPacketsAvailable     ? "delivered_packets"
                          : field == kCaptureDeliveredBytesAvailable       ? "delivered_bytes"
                                                                           : "reader_dropped_packets";
        bool available = true;
        uint64_t total = 0;
        for (const auto& c : counters_) {
            const uint64_t value = field == kCaptureReceivedPacketsAvailable        ? c.received_packets
                                   : field == kCaptureSourceDroppedPacketsAvailable ? c.source_dropped_packets
                                   : field == kCaptureDeliveredPacketsAvailable     ? c.delivered_packets
                                   : field == kCaptureDeliveredBytesAvailable       ? c.delivered_bytes
                                                                                    : c.queue_dropped_packets;
            if (!(c.available_mask & field) || value > UINT64_MAX - total)
                available = false;
            else
                total += value;
        }
        w.Key(key);
        if (available)
            w.Uint64(total);
        else
            w.Null();
    }
    w.EndObject();
    w.Key("inputs");
    w.StartArray();
    for (size_t i = 0; i < sources_.inputs.size(); ++i) {
        const auto& input = backend_->Inputs()[i];
        const auto& c = counters_[i];
        w.StartObject();
        w.Key("source_id");
        w.Uint(i);
        w.Key("interface");
        w.String(input.interface_name.c_str());
        w.Key("queue_id");
        w.Uint(input.queue_id);
        w.Key("physical_queue");
        w.Bool(input.physical_queue);
        w.Key("processed_candidate_ns");
        w.Int64(candidates_[i]);
        w.Key("buffer_bytes");
        w.Uint64(input.buffer_bytes);
        w.Key("timestamp_source");
        w.String(input.timestamp_source.c_str());
        w.Key("counter_source");
        w.String(input.counter_source.c_str());
        w.Key("backlog");
        w.String(backlogs_[i] == CaptureBacklogV1::kEmpty     ? "empty"
                 : backlogs_[i] == CaptureBacklogV1::kPresent ? "present"
                                                              : "unknown");
        w.Key("received_packets");
        if (c.available_mask & kCaptureReceivedPacketsAvailable)
            w.Uint64(c.received_packets);
        else
            w.Null();
        w.Key("source_dropped_packets");
        if (c.available_mask & kCaptureSourceDroppedPacketsAvailable)
            w.Uint64(c.source_dropped_packets);
        else
            w.Null();
        w.Key("delivered_packets");
        w.Uint64(c.delivered_packets);
        w.Key("delivered_bytes");
        w.Uint64(c.delivered_bytes);
        w.Key("reader_dropped_packets");
        w.Uint64(c.queue_dropped_packets);
        w.Key("backpressure_events");
        w.Uint64(c.backpressure_events);
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    return buffer.GetString();
}
}  // namespace flowsql::channels::netadapter
