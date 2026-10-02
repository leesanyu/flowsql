// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <framework/core/capture_progress_tracker.h>
#include <framework/core/capture_reader_state.h>
#include <framework/interfaces/iblock_stream_reader.h>
#include <framework/interfaces/icapture_block_stream_reader.h>

#include <arrow/api.h>

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <future>
#include <mutex>
#include <type_traits>
#include <vector>

using namespace flowsql;

namespace {

std::shared_ptr<arrow::RecordBatch> MakePacketBatch(uint32_t captured_bytes, uint32_t source_id = 0,
                                                    uint64_t sequence = 1, int64_t timestamp_ns = 10) {
    packet::PacketRecord record;
    record.meta.timestamp_ns = timestamp_ns;
    record.meta.captured_len = captured_bytes;
    record.meta.wire_len = captured_bytes;
    record.meta.link_type = 1;
    record.meta.source_id = source_id;
    record.meta.sequence = sequence;
    auto bytes = std::make_shared<std::vector<uint8_t>>(captured_bytes, 0);
    record.raw_data.owner = bytes;
    record.raw_data.data = bytes->data();
    record.raw_data.size = captured_bytes;
    record.layer.status = packet::LayerStatus::kTruncated;
    std::shared_ptr<arrow::RecordBatch> batch;
    assert(packet::EncodePacketBatch({record}, &batch) == packet::PacketBatchError::kNone);
    return batch;
}

void TestCaptureReaderState() {
    CaptureReaderStateV1 state;
    CaptureQueueIdentityV1 identity;
    identity.source_name = "eth0";
    identity.generation = 7;
    identity.link_type = 1;
    CaptureReaderLimitsV1 limits;
    limits.max_packets_per_batch = 1;
    limits.max_bytes_per_batch = 64;
    limits.max_wait_ms = 10;
    limits.max_outstanding_batches = 1;
    assert(state.Open(identity, limits) == 0);
    assert(state.Open(identity, limits) == EALREADY);
    assert(state.EffectiveWaitMs(50) == 10);
    assert(state.EffectiveWaitMs(0) == 0);
    assert(state.EffectiveWaitMs(-1) == -EINVAL);

    auto batch = MakePacketBatch(8);
    auto second = MakePacketBatch(8, 0, 2);
    auto large = MakePacketBatch(65);
    auto owner = std::make_shared<int>(1);
    assert(state.Borrow(1, batch, 65, owner) == EINVAL);
    assert(state.Borrow(1, large, 65, owner) == EOVERFLOW);
    assert(state.Borrow(1, batch, 8, owner) == 0);
    assert(state.OutstandingCount() == 1);
    assert(!state.CanReuse(1));
    assert(state.Borrow(2, second, 8, owner) == EAGAIN);
    assert(state.Release(second) == EINVAL);
    assert(state.Release(batch) == 0);
    assert(state.Release(batch) == EINVAL);
    assert(!state.CanReuse(1));
    assert(state.Borrow(1, second, 8, owner) == EBUSY);
    owner.reset();
    assert(state.CanReuse(1));
    auto next_owner = std::make_shared<int>(2);
    assert(state.Borrow(1, second, 8, next_owner) == 0);
    state.Cancel();
    state.Finish();
    assert(state.State() == CaptureReaderStateV1::Terminal::kCancelled);
    assert(state.Borrow(2, batch, 8, next_owner) == EPIPE);
    assert(state.Close() == EBUSY);
    assert(state.Release(second) == 0);
    assert(state.Close() == EBUSY);
    next_owner.reset();
    assert(state.Close() == 0);
    assert(state.Open(identity, limits) == EINVAL);
    identity.generation = 8;
    assert(state.Open(identity, limits) == 0);
    state.Finish();
    assert(state.Close() == 0);
}

void TestCaptureProgressTracker() {
    CaptureQueueIdentityV1 identity;
    identity.source_name = "eth0";
    identity.source_id = 9;
    identity.queue_id = 2;
    identity.generation = 7;
    CaptureProgressTrackerV1 tracker(identity);
    CaptureProgressV1 fact;
    fact.source_id = 9;
    fact.queue_id = 2;
    fact.generation = 7;
    fact.fact_sequence = 1;
    fact.capture_time_ns = 100;
    fact.packet_observed = true;
    fact.backlog = CaptureBacklogV1::kEmpty;
    assert(tracker.Observe(fact).error == CaptureProgressErrorV1::kUnprocessedPacket);
    const auto first = tracker.Observe(fact, 100);
    assert(first.error == CaptureProgressErrorV1::kNone);
    assert(first.disposition == CaptureProgressDispositionV1::kAdvanced);
    assert(first.capture_progress_ns == 100);
    assert(tracker.Observe(fact, 100).error == CaptureProgressErrorV1::kInvalidSequence);
    fact.fact_sequence = 2;
    fact.capture_time_ns = 200;
    fact.packet_observed = false;
    fact.backlog = CaptureBacklogV1::kUnknown;
    assert(tracker.Observe(fact).disposition == CaptureProgressDispositionV1::kBacklogUnknown);
    fact.fact_sequence = 3;
    fact.backlog = CaptureBacklogV1::kPresent;
    assert(tracker.Observe(fact).disposition == CaptureProgressDispositionV1::kBacklogged);
    fact.fact_sequence = 4;
    fact.backlog = CaptureBacklogV1::kEmpty;
    assert(tracker.Observe(fact).disposition == CaptureProgressDispositionV1::kIdleUnconfirmed);
    fact.fact_sequence = 5;
    fact.source_idle_confirmed = true;
    assert(tracker.Observe(fact).disposition == CaptureProgressDispositionV1::kAdvanced);
    assert(tracker.ProgressNs() == 200);
    fact.fact_sequence = 6;
    fact.capture_time_ns = 150;
    assert(tracker.Observe(fact).disposition == CaptureProgressDispositionV1::kTimeRegressed);
    assert(tracker.ProgressNs() == 200);
    fact.fact_sequence = 7;
    fact.capture_time_ns = 200;
    assert(tracker.Observe(fact).disposition == CaptureProgressDispositionV1::kUnchanged);
    fact.fact_sequence = 8;
    fact.queue_id = 3;
    assert(tracker.Observe(fact).error == CaptureProgressErrorV1::kWrongQueue);
    fact.queue_id = 2;
    fact.generation = 8;
    assert(tracker.Observe(fact).error == CaptureProgressErrorV1::kWrongQueue);
    fact.generation = 7;
    fact.packet_observed = true;
    fact.capture_time_ns = 210;
    fact.source_idle_confirmed = true;
    assert(tracker.Observe(fact, 210).error == CaptureProgressErrorV1::kInvalidFact);
    fact.source_idle_confirmed = false;
    assert(tracker.Observe(fact, 205).error == CaptureProgressErrorV1::kUnprocessedPacket);
    assert(tracker.Observe(fact, 210).disposition == CaptureProgressDispositionV1::kAdvanced);
}

class FakeCaptureReader final : public ICaptureBlockStreamReaderV1 {
 public:
    explicit FakeCaptureReader(uint64_t generation, bool packets = true) : packets_(packets) {
        identity_.source_name = "fake0";
        identity_.source_id = 9;
        identity_.queue_id = 2;
        identity_.generation = generation;
        identity_.link_type = 1;
        limits_.max_packets_per_batch = 1;
        limits_.max_bytes_per_batch = 64;
        limits_.max_wait_ms = 100;
        limits_.max_outstanding_batches = 1;
        assert(state_.Open(identity_, limits_) == 0);
    }

    const char* Category() override { return "fakecapture"; }
    const char* Name() override { return "fake0"; }
    const char* Type() override { return ChannelType::kBlockStream; }
    const char* Schema() override { return "packet"; }
    int Open() override { return 0; }
    int Close() override { return state_.Close(); }
    bool IsOpened() const override { return state_.State() == CaptureReaderStateV1::Terminal::kOpen; }
    int Flush() override { return 0; }
    BlockPollEvent PollBlock(int) override { return {BlockPollEvent::kError, nullptr, ENOTSUP}; }
    int ReleaseBlock(const std::shared_ptr<arrow::RecordBatch>& batch) override {
        const int rc = state_.Release(batch);
        wake_.notify_all();
        return rc;
    }
    void Cancel() override {
        state_.Cancel();
        wake_.notify_all();
    }
    bool IsFinished() const override { return state_.State() != CaptureReaderStateV1::Terminal::kOpen; }
    int Describe(CaptureQueueIdentityV1* identity, CaptureReaderLimitsV1* limits) const override {
        if (!identity || !limits || identity->struct_size < sizeof(*identity) ||
            limits->struct_size < sizeof(*limits) ||
            identity->contract_version != kCaptureBlockStreamContractVersionV1 ||
            limits->contract_version != kCaptureBlockStreamContractVersionV1) {
            return EINVAL;
        }
        *identity = identity_;
        *limits = limits_;
        return 0;
    }
    int ReadCounters(CaptureCountersV1* counters) const override { return state_.ReadCounters(counters); }

    CapturePollEventV1 PollCapture(int timeout_ms) override {
        CapturePollEventV1 event;
        const int effective = state_.EffectiveWaitMs(timeout_ms);
        if (effective < 0) {
            event.block = {BlockPollEvent::kError, nullptr, EINVAL};
            return event;
        }
        std::unique_lock<std::mutex> lock(mutex_);
        if (state_.State() == CaptureReaderStateV1::Terminal::kCancelled) {
            event.block.kind = BlockPollEvent::kCancelled;
            return event;
        }
        if (state_.State() == CaptureReaderStateV1::Terminal::kError) {
            event.block = {BlockPollEvent::kError, nullptr, EIO};
            return event;
        }
        if (state_.State() == CaptureReaderStateV1::Terminal::kEof) {
            event.block.kind = BlockPollEvent::kEof;
            return event;
        }
        if (packets_ && next_packet_ < 2) {
            if (state_.OutstandingCount() != 0 || !state_.CanReuse(1)) {
                state_.CountBackpressure();
                event.block.kind = BlockPollEvent::kTimeout;
                return event;
            }
            auto batch = MakePacketBatch(8, identity_.source_id, next_packet_ + 1, (next_packet_ + 1) * 100);
            const std::shared_ptr<const void> owner = batch;
            assert(state_.Borrow(1, batch, 8, owner) == 0);
            state_.CountReceived(1);
            ++next_packet_;
            event.block = {BlockPollEvent::kData, batch, 0};
            event.has_progress = true;
            event.progress = MakeFact(next_packet_, next_packet_ * 100, true, false,
                                      next_packet_ == 2 ? CaptureBacklogV1::kEmpty : CaptureBacklogV1::kPresent);
            return event;
        }
        if (packets_ && next_packet_ == 2 && !idle_sent_) {
            idle_sent_ = true;
            event.block.kind = BlockPollEvent::kTimeout;
            event.has_progress = true;
            event.progress = MakeFact(3, 300, false, true, CaptureBacklogV1::kEmpty);
            return event;
        }
        if (state_.State() == CaptureReaderStateV1::Terminal::kEof) {
            event.block.kind = BlockPollEvent::kEof;
            return event;
        }
        wake_.wait_for(lock, std::chrono::milliseconds(effective),
                       [&] { return state_.State() != CaptureReaderStateV1::Terminal::kOpen; });
        event.block.kind = state_.State() == CaptureReaderStateV1::Terminal::kCancelled ? BlockPollEvent::kCancelled
                                                                                        : BlockPollEvent::kTimeout;
        return event;
    }

    void Finish() {
        state_.Finish();
        wake_.notify_all();
    }
    void Fail() {
        state_.Fail();
        wake_.notify_all();
    }
    void SetSourceDropped(uint64_t packets) { state_.SetSourceDropped(packets); }

 private:
    CaptureProgressV1 MakeFact(uint64_t sequence, int64_t time_ns, bool packet, bool idle,
                               CaptureBacklogV1 backlog) const {
        CaptureProgressV1 fact;
        fact.source_id = identity_.source_id;
        fact.queue_id = identity_.queue_id;
        fact.generation = identity_.generation;
        fact.fact_sequence = sequence;
        fact.capture_time_ns = time_ns;
        fact.packet_observed = packet;
        fact.source_idle_confirmed = idle;
        fact.backlog = backlog;
        return fact;
    }

    CaptureQueueIdentityV1 identity_;
    CaptureReaderLimitsV1 limits_;
    CaptureReaderStateV1 state_;
    bool packets_ = true;
    uint64_t next_packet_ = 0;
    bool idle_sent_ = false;
    std::mutex mutex_;
    std::condition_variable wake_;
};

class LegacyBlockReader final : public IBlockStreamChannel {
 public:
    const char* Category() override { return "legacy"; }
    const char* Name() override { return "old"; }
    const char* Type() override { return ChannelType::kBlockStream; }
    const char* Schema() override { return "packet"; }
    int Open() override { return 0; }
    int Close() override { return 0; }
    bool IsOpened() const override { return true; }
    int Flush() override { return 0; }
    BlockPollEvent PollBlock(int) override { return {BlockPollEvent::kTimeout, nullptr, 0}; }
    int ReleaseBlock(const std::shared_ptr<arrow::RecordBatch>&) override { return 0; }
    void Cancel() override {}
    bool IsFinished() const override { return false; }
};

/** Reusable two-packet contract probe for any deterministic ICaptureBlockStreamReaderV1 fixture. */
void VerifyCaptureReaderContractV1(ICaptureBlockStreamReaderV1& reader, uint32_t source_id, uint64_t generation) {
    CaptureQueueIdentityV1 identity;
    CaptureReaderLimitsV1 limits;
    assert(reader.Describe(&identity, &limits) == 0);
    CaptureQueueIdentityV1 wrong_version;
    wrong_version.contract_version = 2;
    CaptureReaderLimitsV1 requested_limits;
    assert(reader.Describe(&wrong_version, &requested_limits) == EINVAL);
    assert(ValidateCaptureReaderDescriptionV1(identity, limits) == CaptureDescriptionErrorV1::kNone);
    assert(identity.source_id == source_id && identity.generation == generation);
    CaptureProgressTrackerV1 tracker(identity);
    auto first = reader.PollCapture(0);
    assert(first.block.kind == BlockPollEvent::kData && first.has_progress);
    const auto first_source = std::static_pointer_cast<arrow::UInt32Array>(first.block.batch->column(4));
    const auto first_sequence = std::static_pointer_cast<arrow::UInt64Array>(first.block.batch->column(5));
    const auto first_time = std::static_pointer_cast<arrow::Int64Array>(first.block.batch->column(0));
    assert(first_source->Value(0) == source_id && first_sequence->Value(0) == 1);
    assert(first_time->Value(0) == first.progress.capture_time_ns);
    assert(first.progress.backlog == CaptureBacklogV1::kPresent);
    assert(reader.PollCapture(0).block.kind == BlockPollEvent::kTimeout);
    assert(reader.ReleaseBlock(first.block.batch) == 0);
    assert(reader.ReleaseBlock(first.block.batch) == EINVAL);
    assert(tracker.Observe(first.progress, first_time->Value(0)).disposition ==
           CaptureProgressDispositionV1::kBacklogged);
    first.block.batch.reset();
    auto second = reader.PollCapture(0);
    assert(second.block.kind == BlockPollEvent::kData && second.has_progress);
    assert(reader.ReleaseBlock(second.block.batch) == 0);
    const auto second_time = std::static_pointer_cast<arrow::Int64Array>(second.block.batch->column(0));
    assert(tracker.Observe(second.progress, second_time->Value(0)).disposition ==
           CaptureProgressDispositionV1::kAdvanced);
    second.block.batch.reset();
    auto idle = reader.PollCapture(0);
    assert(idle.block.kind == BlockPollEvent::kTimeout && idle.has_progress);
    assert(tracker.Observe(idle.progress).disposition == CaptureProgressDispositionV1::kAdvanced);
    assert(tracker.ProgressNs() == 300);
    CaptureCountersV1 counters;
    assert(reader.ReadCounters(&counters) == 0);
    assert(counters.generation == generation && counters.received_packets == 2);
    assert(counters.delivered_packets == 2 && counters.delivered_bytes == 16);
    assert(counters.backpressure_events == 1);
    assert((counters.available_mask & kCaptureSourceDroppedPacketsAvailable) == 0);
}

void TestFakeCaptureReader() {
    LegacyBlockReader legacy;
    IBlockStreamChannel* legacy_base = &legacy;
    assert(dynamic_cast<ICaptureBlockStreamReaderV1*>(legacy_base) == nullptr);
    FakeCaptureReader reader(17);
    VerifyCaptureReaderContractV1(reader, 9, 17);
    reader.SetSourceDropped(0);
    CaptureCountersV1 counters;
    assert(reader.ReadCounters(&counters) == 0);
    assert((counters.available_mask & kCaptureSourceDroppedPacketsAvailable) != 0);
    reader.Finish();
    assert(reader.PollCapture(0).block.kind == BlockPollEvent::kEof);
    assert(reader.Close() == 0);

    FakeCaptureReader cancelled(18, false);
    auto waiting = std::async(std::launch::async, [&] { return cancelled.PollCapture(5000); });
    cancelled.Cancel();
    assert(waiting.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    assert(waiting.get().block.kind == BlockPollEvent::kCancelled);
    cancelled.Finish();
    assert(cancelled.Close() == 0);
    FakeCaptureReader failed(19, false);
    failed.Fail();
    assert(failed.PollCapture(0).block.kind == BlockPollEvent::kError);
    assert(failed.Close() == 0);
}

}  // namespace

void test_capture_contract_description() {
    static_assert(std::is_base_of_v<IBlockStreamChannel, ICaptureBlockStreamReaderV1>);
    static_assert(std::is_abstract_v<ICaptureBlockStreamReaderV1>);
    static_assert(std::is_abstract_v<IBlockStreamReaderFactoryV1>);
    static_assert(kBlockStreamReaderContractVersionV1 == 1);
    static_assert(kCaptureBlockStreamContractVersionV1 == 1);

    CaptureQueueIdentityV1 identity;
    CaptureReaderLimitsV1 limits;
    assert(identity.struct_size == sizeof(identity));
    assert(limits.struct_size == sizeof(limits));
    identity.source_name = "eth0";
    identity.generation = 1;
    limits.max_packets_per_batch = 16;
    limits.max_bytes_per_batch = 4096;
    limits.max_wait_ms = 20;
    limits.max_outstanding_batches = 2;
    assert(ValidateCaptureReaderDescriptionV1(identity, limits) == CaptureDescriptionErrorV1::kNone);

    identity.contract_version = 2;
    assert(ValidateCaptureReaderDescriptionV1(identity, limits) == CaptureDescriptionErrorV1::kInvalidVersion);
    identity.contract_version = kCaptureBlockStreamContractVersionV1;
    limits.struct_size = sizeof(limits) - 1;
    assert(ValidateCaptureReaderDescriptionV1(identity, limits) == CaptureDescriptionErrorV1::kInvalidVersion);
    limits.struct_size = sizeof(limits);
    identity.source_name = "";
    assert(ValidateCaptureReaderDescriptionV1(identity, limits) == CaptureDescriptionErrorV1::kInvalidIdentity);
    identity.source_name = "eth0";
    identity.generation = 0;
    assert(ValidateCaptureReaderDescriptionV1(identity, limits) == CaptureDescriptionErrorV1::kInvalidIdentity);
    identity.generation = 1;
    limits.max_wait_ms = 0;
    assert(ValidateCaptureReaderDescriptionV1(identity, limits) == CaptureDescriptionErrorV1::kInvalidLimits);

    CapturePollEventV1 event;
    CaptureProgressV1 progress;
    CaptureCountersV1 counters;
    assert(event.struct_size == sizeof(event));
    assert(progress.struct_size == sizeof(progress));
    assert(counters.struct_size == sizeof(counters));
    assert(event.block.kind == BlockPollEvent::kTimeout);
    assert(!event.has_progress);
    assert(progress.backlog == CaptureBacklogV1::kUnknown);
    assert(counters.available_mask == 0);
    assert((kCaptureSourceDroppedPacketsAvailable & kCaptureQueueDroppedPacketsAvailable) == 0);
    TestCaptureReaderState();
    TestCaptureProgressTracker();
    TestFakeCaptureReader();
    std::printf("[PASS] capture reader public description contract\n");
}
