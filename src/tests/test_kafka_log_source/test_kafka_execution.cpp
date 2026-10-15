// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <arrow/api.h>
#include <framework/core/pipeline.h>
#include <framework/interfaces/iblock_stream_acknowledgement.h>
#include <cassert>
#include <iostream>
#include <stdexcept>
using namespace flowsql;
struct Source final : IBlockStreamChannel, IBlockStreamAcknowledgementV1 {
    std::shared_ptr<arrow::RecordBatch> batch;
    int polls = 0, acknowledges = 0, releases = 0, ack_error = 0;
    bool ack_throws = false;
    std::vector<std::string> operations;
    const char* Category() override { return "test"; }
    const char* Name() override { return "events"; }
    const char* Type() override { return ChannelType::kBlockStream; }
    const char* Schema() override { return ""; }
    int Open() override { return 0; }
    int Close() override { return 0; }
    int Flush() override { return 0; }
    bool IsOpened() const override { return true; }
    BlockPollEvent PollBlock(int) override {
        return polls++ ? BlockPollEvent{BlockPollEvent::kEof, nullptr, 0}
                       : BlockPollEvent{BlockPollEvent::kData, batch, 0};
    }
    int ReleaseBlock(const std::shared_ptr<arrow::RecordBatch>& value) override {
        assert(value == batch);
        ++releases;
        operations.push_back("release");
        return 0;
    }
    int AcknowledgeBlock(const std::shared_ptr<arrow::RecordBatch>& value) override {
        assert(value == batch);
        ++acknowledges;
        operations.push_back("ack");
        if (ack_throws) throw std::runtime_error("ack");
        return ack_error;
    }
    void Cancel() override {}
    bool IsFinished() const override { return polls > 1; }
};
struct Transform final : IBlockTransformTaskV1 {
    Source* source;
    int status = 0;
    bool fail = false;
    explicit Transform(Source* value) : source(value) {}
    int Open(std::shared_ptr<arrow::Schema> schema, std::shared_ptr<arrow::Schema>* output) override {
        *output = schema;
        return 0;
    }
    int ProcessBlock(const std::shared_ptr<arrow::RecordBatch>& input, int64_t,
                     std::vector<BlockTransformOutputV1>* outputs) override {
        source->operations.push_back("process");
        if (fail) return EIO;
        outputs->push_back({input, 0});
        return status;
    }
    int Flush(std::vector<BlockTransformOutputV1>*) override { return 0; }
    void Cancel() override {}
    std::string LastError() const override { return "failure"; }
};
int main() {
    auto schema = arrow::schema({arrow::field("value", arrow::int64())});
    arrow::Int64Builder builder;
    assert(builder.Append(42).ok());
    auto array = builder.Finish().ValueOrDie();
    for (int mode = 0; mode < 6; ++mode) {
        Source source;
        source.batch = arrow::RecordBatch::Make(schema, 1, {array});
        Transform transform(&source);
        transform.fail = mode == 1;
        transform.status = mode == 5 ? 1 : 0;
        source.ack_error = mode == 3 ? EIO : 0;
        source.ack_throws = mode == 4;
        BlockTransformPipelineConfig config;
        config.source = &source;
        config.source_schema = schema;
        config.transform = &transform;
        config.output_consumer = [&](const BlockTransformOutputV1&) {
            source.operations.push_back("deliver");
            assert(source.acknowledges == 0);
            return mode == 2 ? EIO : 0;
        };
        BlockTransformPipelineRunner runner(std::move(config));
        BlockTransformPipelineResult result;
        std::string error;
        auto rc = runner.Run(&result, &error);
        assert(source.releases == 1 && source.operations.back() == "release");
        assert(source.acknowledges == ((mode == 1 || mode == 2) ? 0 : 1));
        assert((rc == BlockTransformPipelineError::kNone) == (mode == 0 || mode == 5));
        if (mode == 0 || mode == 5)
            assert(source.operations == std::vector<std::string>({"process", "deliver", "ack", "release"}));
    }
    std::cout << "PASS: process/delivery precede acknowledgement; processing/delivery errors never acknowledge; commit "
                 "errors terminate and always release\n";
}
