// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <framework/interfaces/idatabase_published_source.h>
#include <condition_variable>
#include <mutex>
#include "snapshot_input.h"

namespace flowsql::baseliner {
// Serialized task-private reader. Unclosed target buckets remain at source; no premature observations.
// Only a released progress-only window boundary advances input confirmation, independently of target durability.
class PollInput final : public IBlockStreamChannel, public IBlockStreamInputProgressV1 {
 public:
    ~PollInput() override { Close(); }
    int Initialize(ConfigSnapshot config, std::shared_ptr<IDatabaseChannel> source, std::string_view exact_source,
                   const BlockInputProgressV1& restore = {}, const std::map<std::string, int64_t>& pending_starts = {},
                   const std::map<std::string, std::string>& source_epochs = {});
    std::map<std::string, int64_t> PendingStarts() const;
    std::map<std::string, std::string> SourceEpochs() const;
    const char* Category() override { return "baseliner"; }
    const char* Name() override { return "poll"; }
    const char* Type() override { return ChannelType::kBlockStream; }
    const char* Schema() override { return "baseline_observation_v1"; }
    int Open() override { return opened_ ? 0 : -1; }
    int Close() override;
    bool IsOpened() const override { return opened_; }
    int Flush() override { return 0; }
    BlockPollEvent PollBlock(int timeout_ms = 100) override;
    int ReleaseBlock(const std::shared_ptr<arrow::RecordBatch>& block) override;
    int ReadInputProgress(BlockInputProgressV1* output) const override;
    void Cancel() override;
    bool IsFinished() const override { return cancelled_ || finished_; }
    const std::string& LastError() const { return error_; }
    std::shared_ptr<arrow::Schema> InputSchema() const { return MakeSchema(SchemaKind::kObservation); }

 private:
    struct DatasetState {
        PublishedDatasetRequestV1 request;
        std::string epoch;
        int64_t next_bucket = 0;
        int64_t source_period_ns = 0;
        BlockDatasetProgressV1 confirmed;
    };
    BlockPollEvent Fail(std::string error);
    int Validate(size_t dataset, const std::string& position, PublishedDatasetProgressV1* output);
    ConfigSnapshot config_;
    std::shared_ptr<IDatabaseChannel> source_;
    std::shared_ptr<IDatabasePublishedProgressReaderV1> progress_;
    std::shared_ptr<SnapshotInput> cycle_;
    std::shared_ptr<SnapshotReader> selection_;
    std::vector<DatasetState> states_;
    PublishedDatasetProgressV1 frozen_;
    std::shared_ptr<arrow::RecordBatch> outstanding_;
    bool outstanding_boundary_ = false;
    bool progress_readable_ = false;
    bool opened_ = false;
    size_t dataset_ = 0;
    std::atomic<bool> cancelled_{false};
    std::atomic<bool> finished_{false};
    std::mutex wait_mutex_;
    std::condition_variable wake_;
    std::chrono::steady_clock::time_point next_poll_{};
    std::string error_;
};
}  // namespace flowsql::baseliner
