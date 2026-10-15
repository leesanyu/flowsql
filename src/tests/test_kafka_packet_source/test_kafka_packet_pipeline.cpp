// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <arrow/api.h>
#include <framework/core/packet_family_codec.h>
#include <framework/core/pipeline.h>
#include <framework/interfaces/iblock_stream_acknowledgement.h>
#include <cassert>
#include <iostream>
using namespace flowsql;
namespace {
struct Source final : ICaptureBlockStreamReaderV2, IBlockStreamAcknowledgementV1 {
    std::shared_ptr<arrow::RecordBatch> batch;
    std::vector<std::string> operations;
    int polls = 0, acknowledgements = 0, releases = 0;
    bool bad_fact = false, no_fact = false;
    const char* Category() override { return "kafka"; }
    const char* Name() override { return "packets"; }
    const char* Type() override { return ChannelType::kBlockStream; }
    const char* Schema() override { return "packet"; }
    int Open() override { return 0; }
    int Close() override { return 0; }
    int Flush() override { return 0; }
    bool IsOpened() const override { return true; }
    bool IsFinished() const override { return polls > 1; }
    void Cancel() override {}
    int DescribeSources(CaptureSourceSetV2*) const override { return ENOTSUP; }
    int ReadInputCounters(uint32_t, CaptureCountersV1*) const override { return ENOTSUP; }
    BlockPollEvent PollBlock(int ms) override { return PollCapture(ms).block; }
    CapturePollEventV2 PollCapture(int) override {
        CapturePollEventV2 event;
        if (polls++)
            event.block.kind = BlockPollEvent::kEof;
        else {
            event.block = {BlockPollEvent::kData, batch, 0};
            CaptureProgressV1 fact;
            fact.source_id = 0;
            fact.capture_time_ns = bad_fact ? 200 : 100;
            fact.packet_observed = batch->num_rows() > 0;
            fact.source_idle_confirmed = !fact.packet_observed;
            if (!no_fact) event.progress.push_back(fact);
        }
        return event;
    }
    int ReleaseBlock(const std::shared_ptr<arrow::RecordBatch>& value) override {
        assert(value == batch);
        operations.push_back("release");
        ++releases;
        return 0;
    }
    int AcknowledgeBlock(const std::shared_ptr<arrow::RecordBatch>& value) override {
        assert(value == batch);
        operations.push_back("ack");
        ++acknowledgements;
        return 0;
    }
};
struct Task final : IBlockTransformTaskV2, IBlockTransformCaptureFactTaskV2 {
    Source& source;
    int mode;
    bool pending = false;
    Task(Source& s, int m) : source(s), mode(m) {}
    int BindCaptureSources(const CaptureSourceSetV2&) override { return 0; }
    int AcceptCaptureFacts(const std::vector<CaptureProgressV1>& facts) override {
        assert(facts.size() == 1 && source.acknowledgements == 0 && source.releases == 0);
        source.operations.push_back("facts");
        pending = true;
        return mode == 2 ? EIO : 0;
    }
    int Open(std::shared_ptr<arrow::Schema> schema, std::shared_ptr<arrow::Schema>* output) override {
        *output = schema;
        return 0;
    }
    int ProcessBlock(const std::shared_ptr<arrow::RecordBatch>& input, int64_t,
                     std::vector<BlockTransformOutputV1>* outputs) override {
        source.operations.push_back("process");
        if (mode == 1) return EIO;
        outputs->push_back({input, 0});
        return 1;
    }
    int Flush(std::vector<BlockTransformOutputV1>*) override {
        source.operations.push_back("flush");
        return 0;
    }
    void Cancel() override {}
    std::string LastError() const override { return "fixture failure"; }
    int GetTimeDriveState(BlockTransformTimeDriveStateV1* state) override {
        state->armed = pending;
        state->deadline_ns = 0;
        return 0;
    }
    int OnTime(const BlockTransformTimeEventV1&, std::vector<BlockTransformOutputV1>* outputs) override {
        source.operations.push_back("time");
        pending = false;
        if (mode == 4) return EIO;
        outputs->push_back({source.batch, 0});
        return 0;
    }
};
}  // namespace
int main() {
    for (int mode = 0; mode < 8; ++mode) {
        Source source;
        source.bad_fact = mode == 6;
        source.no_fact = mode == 7;
        packet::PacketFamilyBatch records;
        auto owner = std::make_shared<std::string>(3, 'x');
        packet::PacketFamilyRecord row;
        row.meta = {100, 3, 64, 1, 0, 1};
        row.raw_data = {owner, reinterpret_cast<const uint8_t*>(owner->data()), 3};
        if (mode != 5) records.rows.push_back(row);
        assert(packet::EncodePacketFamilyBatch(records, &source.batch) == packet::PacketBatchError::kNone);
        Task task(source, mode);
        BlockTransformPipelineConfig config;
        config.source = &source;
        config.source_schema = packet::PacketFamilySchema();
        config.transform = &task;
        config.time_transform = &task;
        config.capture_fact_task = &task;
        config.output_consumer = [&](const BlockTransformOutputV1&) {
            assert(source.acknowledgements == 0 && source.releases == 0);
            source.operations.push_back("deliver");
            return mode == 3 ? EIO : 0;
        };
        BlockTransformPipelineRunner runner(config);
        BlockTransformPipelineResult result;
        std::string error;
        const auto rc = runner.Run(&result, &error);
        assert(source.releases == 1);
        const bool success = mode == 0 || mode == 5 || mode == 7;
        if ((rc == BlockTransformPipelineError::kNone) != success)
            std::cerr << "mode=" << mode << " error=" << error << '\n';
        assert((rc == BlockTransformPipelineError::kNone) == success);
        assert(source.acknowledgements == (success ? 1 : 0));
        if (mode == 0 || mode == 5)
            assert(source.operations == std::vector<std::string>({"process", "facts", "deliver", "time", "deliver",
                                                                  "ack", "release", "flush"}));
        if (mode == 6) assert(source.operations == std::vector<std::string>({"release"}));
    }
    std::cout
        << "PASS process/facts/current-output/ack/release ordering, zero-row facts and every failure blocks commit\n";
}
