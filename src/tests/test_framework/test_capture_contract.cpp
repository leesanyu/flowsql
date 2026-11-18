// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <tests/common/packet_family_test_support.h>

#include <framework/core/capture_progress_tracker.h>
#include <framework/core/capture_reader_state.h>
#include <framework/core/pipeline.h>
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
    flowsql::testing::PacketRecord record;
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
    assert(flowsql::testing::EncodePacketBatch({record}, &batch) == packet::PacketBatchError::kNone);
    return batch;
}

void TestCaptureReaderState() {
    CaptureReaderState state;
    CaptureQueueIdentity identity;
    identity.source_name = "eth0";
    identity.generation = 7;
    identity.link_type = 1;
    CaptureReaderLimits limits;
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
    assert(state.State() == CaptureReaderState::Terminal::kCancelled);
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
    CaptureQueueIdentity identity;
    identity.source_name = "eth0";
    identity.source_id = 9;
    identity.queue_id = 2;
    identity.generation = 7;
    SingleInputCaptureProgressTracker tracker(identity);
    CaptureProgress fact;
    fact.source_id = 9;
    fact.queue_id = 2;
    fact.generation = 7;
    fact.fact_sequence = 1;
    fact.capture_time_ns = 100;
    fact.packet_observed = true;
    fact.backlog = CaptureBacklog::kEmpty;
    assert(tracker.Observe(fact).error == CaptureProgressError::kUnprocessedPacket);
    const auto first = tracker.Observe(fact, 100);
    assert(first.error == CaptureProgressError::kNone);
    assert(first.disposition == CaptureProgressDisposition::kAdvanced);
    assert(first.capture_progress_ns == 100);
    assert(tracker.Observe(fact, 100).error == CaptureProgressError::kInvalidSequence);
    fact.fact_sequence = 2;
    fact.capture_time_ns = 200;
    fact.packet_observed = false;
    fact.backlog = CaptureBacklog::kUnknown;
    assert(tracker.Observe(fact).disposition == CaptureProgressDisposition::kBacklogUnknown);
    fact.fact_sequence = 3;
    fact.backlog = CaptureBacklog::kPresent;
    assert(tracker.Observe(fact).disposition == CaptureProgressDisposition::kBacklogged);
    fact.fact_sequence = 4;
    fact.backlog = CaptureBacklog::kEmpty;
    assert(tracker.Observe(fact).disposition == CaptureProgressDisposition::kIdleUnconfirmed);
    fact.fact_sequence = 5;
    fact.source_idle_confirmed = true;
    assert(tracker.Observe(fact).disposition == CaptureProgressDisposition::kAdvanced);
    assert(tracker.ProgressNs() == 200);
    fact.fact_sequence = 6;
    fact.capture_time_ns = 150;
    assert(tracker.Observe(fact).disposition == CaptureProgressDisposition::kTimeRegressed);
    assert(tracker.ProgressNs() == 200);
    fact.fact_sequence = 7;
    fact.capture_time_ns = 200;
    assert(tracker.Observe(fact).disposition == CaptureProgressDisposition::kUnchanged);
    fact.fact_sequence = 8;
    fact.queue_id = 3;
    assert(tracker.Observe(fact).error == CaptureProgressError::kWrongQueue);
    fact.queue_id = 2;
    fact.generation = 8;
    assert(tracker.Observe(fact).error == CaptureProgressError::kWrongQueue);
    fact.generation = 7;
    fact.packet_observed = true;
    fact.capture_time_ns = 210;
    fact.source_idle_confirmed = true;
    assert(tracker.Observe(fact, 210).error == CaptureProgressError::kInvalidFact);
    fact.source_idle_confirmed = false;
    assert(tracker.Observe(fact, 205).error == CaptureProgressError::kUnprocessedPacket);
    assert(tracker.Observe(fact, 210).disposition == CaptureProgressDisposition::kAdvanced);
}

class FakeCaptureReader final : public ICaptureBlockStreamReader {
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
    bool IsOpened() const override { return state_.State() == CaptureReaderState::Terminal::kOpen; }
    int Flush() override { return 0; }
    BlockPollEvent PollBlock(int) override { return {BlockPollEvent::kError, nullptr, ENOTSUP}; }
    int ReleaseBlock(const std::shared_ptr<arrow::RecordBatch>& batch) override {
        const int rc = state_.Release(batch);
        if (rc == 0) ++release_count_;
        wake_.notify_all();
        return rc;
    }
    void Cancel() override {
        state_.Cancel();
        wake_.notify_all();
    }
    bool IsFinished() const override { return state_.State() != CaptureReaderState::Terminal::kOpen; }
    int DescribeSources(CaptureSourceSet* sources) const override {
        if (!sources || sources->struct_size < sizeof(*sources) || sources->contract_version != 2) return EINVAL;
        sources->inputs = {identity_};
        sources->limits = limits_;
        return 0;
    }
    int ReadInputCounters(uint32_t source_id, CaptureCounters* counters) const override {
        return source_id == identity_.source_id ? state_.ReadCounters(counters) : EINVAL;
    }

    CapturePollEvent PollCapture(int timeout_ms) override {
        CapturePollEvent event;
        const int effective = state_.EffectiveWaitMs(timeout_ms);
        if (effective < 0) {
            event.block = {BlockPollEvent::kError, nullptr, EINVAL};
            return event;
        }
        std::unique_lock<std::mutex> lock(mutex_);
        if (state_.State() == CaptureReaderState::Terminal::kCancelled) {
            event.block.kind = BlockPollEvent::kCancelled;
            return event;
        }
        if (state_.State() == CaptureReaderState::Terminal::kError) {
            event.block = {BlockPollEvent::kError, nullptr, EIO};
            return event;
        }
        if (state_.State() == CaptureReaderState::Terminal::kEof) {
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
            event.progress.resize(1);
            event.progress[0] = MakeFact(next_packet_, next_packet_ * 100, true, false,
                                         next_packet_ == 2 ? CaptureBacklog::kEmpty : CaptureBacklog::kPresent);
            return event;
        }
        if (packets_ && next_packet_ == 2 && !idle_sent_) {
            idle_sent_ = true;
            event.block.kind = BlockPollEvent::kTimeout;
            event.progress.resize(1);
            event.progress[0] = MakeFact(3, 300, false, true, CaptureBacklog::kEmpty);
            return event;
        }
        if (state_.State() == CaptureReaderState::Terminal::kEof) {
            event.block.kind = BlockPollEvent::kEof;
            return event;
        }
        wake_.wait_for(lock, std::chrono::milliseconds(effective),
                       [&] { return state_.State() != CaptureReaderState::Terminal::kOpen; });
        event.block.kind = state_.State() == CaptureReaderState::Terminal::kCancelled ? BlockPollEvent::kCancelled
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
    int ReleaseCount() const { return release_count_; }

 private:
    CaptureProgress MakeFact(uint64_t sequence, int64_t time_ns, bool packet, bool idle, CaptureBacklog backlog) const {
        CaptureProgress fact;
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

    CaptureQueueIdentity identity_;
    CaptureReaderLimits limits_;
    CaptureReaderState state_;
    bool packets_ = true;
    uint64_t next_packet_ = 0;
    bool idle_sent_ = false;
    int release_count_ = 0;
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

/** Reusable two-packet contract probe for any deterministic ICaptureBlockStreamReaderCurrent fixture. */
void VerifyCaptureReaderContract(ICaptureBlockStreamReader& reader, uint32_t source_id, uint64_t generation) {
    CaptureSourceSet sources;
    assert(reader.DescribeSources(&sources) == 0);
    CaptureSourceSet wrong;
    wrong.contract_version = 1;
    assert(reader.DescribeSources(&wrong) == EINVAL);
    assert(ValidateCaptureSourceSet(sources) == CaptureDescriptionError::kNone);
    const auto& identity = sources.inputs.front();
    assert(identity.source_id == source_id && identity.generation == generation);
    SingleInputCaptureProgressTracker tracker(identity);
    auto first = reader.PollCapture(0);
    assert(first.block.kind == BlockPollEvent::kData && !first.progress.empty());
    const auto first_source = std::static_pointer_cast<arrow::UInt32Array>(first.block.batch->column(4));
    const auto first_sequence = std::static_pointer_cast<arrow::UInt64Array>(first.block.batch->column(5));
    const auto first_time = std::static_pointer_cast<arrow::Int64Array>(first.block.batch->column(0));
    assert(first_source->Value(0) == source_id && first_sequence->Value(0) == 1);
    assert(first_time->Value(0) == first.progress.front().capture_time_ns);
    assert(first.progress.front().backlog == CaptureBacklog::kPresent);
    assert(reader.PollCapture(0).block.kind == BlockPollEvent::kTimeout);
    assert(reader.ReleaseBlock(first.block.batch) == 0);
    assert(reader.ReleaseBlock(first.block.batch) == EINVAL);
    assert(tracker.Observe(first.progress.front(), first_time->Value(0)).disposition ==
           CaptureProgressDisposition::kBacklogged);
    first.block.batch.reset();
    auto second = reader.PollCapture(0);
    assert(second.block.kind == BlockPollEvent::kData && !second.progress.empty());
    assert(reader.ReleaseBlock(second.block.batch) == 0);
    const auto second_time = std::static_pointer_cast<arrow::Int64Array>(second.block.batch->column(0));
    assert(tracker.Observe(second.progress.front(), second_time->Value(0)).disposition ==
           CaptureProgressDisposition::kAdvanced);
    second.block.batch.reset();
    auto idle = reader.PollCapture(0);
    assert(idle.block.kind == BlockPollEvent::kTimeout && !idle.progress.empty());
    assert(tracker.Observe(idle.progress.front()).disposition == CaptureProgressDisposition::kAdvanced);
    assert(tracker.ProgressNs() == 300);
    CaptureCounters counters;
    assert(reader.ReadInputCounters(source_id, &counters) == 0);
    assert(counters.generation == generation && counters.received_packets == 2);
    assert(counters.delivered_packets == 2 && counters.delivered_bytes == 16);
    assert(counters.backpressure_events == 1);
    assert((counters.available_mask & kCaptureSourceDroppedPacketsAvailable) == 0);
}

void TestFakeCaptureReader() {
    LegacyBlockReader legacy;
    IBlockStreamChannel* legacy_base = &legacy;
    assert(dynamic_cast<ICaptureBlockStreamReader*>(legacy_base) == nullptr);
    FakeCaptureReader reader(17);
    VerifyCaptureReaderContract(reader, 9, 17);
    reader.SetSourceDropped(0);
    CaptureCounters counters;
    assert(reader.ReadInputCounters(9, &counters) == 0);
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

class CaptureFactProbeTask final : public IBlockTransformTask, public IBlockTransformCaptureFactTask {
 public:
    explicit CaptureFactProbeTask(FakeCaptureReader* reader, bool emit = false) : reader_(reader), emit_(emit) {}
    int Open(std::shared_ptr<arrow::Schema> schema, std::shared_ptr<arrow::Schema>* output) override {
        *output = std::move(schema);
        return 0;
    }
    int ProcessBlock(const std::shared_ptr<arrow::RecordBatch>& input, int64_t,
                     std::vector<BlockTransformOutput>* outputs) override {
        ++processed;
        if (emit_) outputs->push_back({input, 0});
        return 0;
    }
    int Flush(std::vector<BlockTransformOutput>*) override {
        ++flushed;
        return 0;
    }
    void Cancel() override { ++cancelled; }
    std::string LastError() const override { return {}; }
    int BindCaptureSources(const CaptureSourceSet&) override { return 0; }
    int AcceptCaptureFacts(const std::vector<CaptureProgress>& incoming) override {
        const auto& fact = incoming.front();
        if (fact.packet_observed) assert(reader_->ReleaseCount() + 1 == processed);
        facts.push_back(fact.fact_sequence);
        if (fact.fact_sequence == 3) reader_->Finish();
        return 0;
    }
    int processed = 0;
    int flushed = 0;
    int cancelled = 0;
    std::vector<uint64_t> facts;

 private:
    FakeCaptureReader* reader_;
    bool emit_;
};

void TestCapturePipelineFactOrder() {
    FakeCaptureReader reader(41);
    CaptureFactProbeTask task(&reader);
    BlockTransformPipelineConfig config;
    config.source = &reader;
    config.source_schema = packet::PacketSchema();
    config.transform = &task;
    config.capture_fact_task = &task;
    config.poll_timeout_ms = 0;
    config.output_consumer = [](const BlockTransformOutput&) { return 0; };
    BlockTransformPipelineRunner runner(std::move(config));
    BlockTransformPipelineResult result;
    std::string error;
    assert(runner.Run(&result, &error) == BlockTransformPipelineError::kNone);
    assert(error.empty() && result.terminal == BlockTransformPipelineTerminal::kCompleted);
    assert(task.processed == 2 && reader.ReleaseCount() == 2 && task.flushed == 1);
    assert(task.facts == std::vector<uint64_t>({1, 2, 3}));
    assert(reader.Close() == 0);

    FakeCaptureReader failed_reader(42);
    CaptureFactProbeTask failed_task(&failed_reader, true);
    BlockTransformPipelineConfig failed_config;
    failed_config.source = &failed_reader;
    failed_config.source_schema = packet::PacketSchema();
    failed_config.transform = &failed_task;
    failed_config.capture_fact_task = &failed_task;
    failed_config.output_consumer = [](const BlockTransformOutput&) { return EIO; };
    BlockTransformPipelineRunner failed_runner(std::move(failed_config));
    assert(failed_runner.Run(&result, &error) == BlockTransformPipelineError::kOutputConsumerFailed);
    assert(failed_reader.ReleaseCount() == 1 && failed_task.facts == std::vector<uint64_t>({1}));
    assert(failed_task.flushed == 0 && failed_task.cancelled == 1);
}

}  // namespace

void test_capture_contract_description() {
    static_assert(std::is_base_of_v<IBlockStreamChannel, ICaptureBlockStreamReader>);
    static_assert(std::is_abstract_v<ICaptureBlockStreamReader>);
    static_assert(std::is_abstract_v<IBlockStreamReaderFactory>);
    static_assert(kBlockStreamReaderContractVersion == 1);
    static_assert(kCaptureFactContractVersion == 1);

    CaptureQueueIdentity identity;
    CaptureReaderLimits limits;
    assert(identity.struct_size == sizeof(identity));
    assert(limits.struct_size == sizeof(limits));
    identity.source_name = "eth0";
    identity.generation = 1;
    limits.max_packets_per_batch = 16;
    limits.max_bytes_per_batch = 4096;
    limits.max_wait_ms = 20;
    limits.max_outstanding_batches = 2;
    assert(ValidateCaptureReaderDescription(identity, limits) == CaptureDescriptionError::kNone);

    identity.contract_version = 2;
    assert(ValidateCaptureReaderDescription(identity, limits) == CaptureDescriptionError::kInvalidVersion);
    identity.contract_version = kCaptureFactContractVersion;
    limits.struct_size = sizeof(limits) - 1;
    assert(ValidateCaptureReaderDescription(identity, limits) == CaptureDescriptionError::kInvalidVersion);
    limits.struct_size = sizeof(limits);
    identity.source_name = "";
    assert(ValidateCaptureReaderDescription(identity, limits) == CaptureDescriptionError::kInvalidIdentity);
    identity.source_name = "eth0";
    identity.generation = 0;
    assert(ValidateCaptureReaderDescription(identity, limits) == CaptureDescriptionError::kInvalidIdentity);
    identity.generation = 1;
    limits.max_wait_ms = 0;
    assert(ValidateCaptureReaderDescription(identity, limits) == CaptureDescriptionError::kInvalidLimits);

    CapturePollEvent event;
    CaptureProgress progress;
    CaptureCounters counters;
    assert(event.struct_size == sizeof(event));
    assert(progress.struct_size == sizeof(progress));
    assert(counters.struct_size == sizeof(counters));
    assert(event.block.kind == BlockPollEvent::kTimeout);
    assert(event.progress.empty());
    assert(progress.backlog == CaptureBacklog::kUnknown);
    assert(counters.available_mask == 0);
    assert((kCaptureSourceDroppedPacketsAvailable & kCaptureQueueDroppedPacketsAvailable) == 0);
    TestCaptureReaderState();
    TestCaptureProgressTracker();
    TestFakeCaptureReader();
    TestCapturePipelineFactOrder();
    std::printf("[PASS] capture reader public description contract\n");
}
