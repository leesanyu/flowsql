// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#ifndef FLOWSQL_BASELINER_SNAPSHOT_INPUT_H_
#define FLOWSQL_BASELINER_SNAPSHOT_INPUT_H_
#include <framework/interfaces/iblock_stream_channel.h>
#include <functional>
#include <limits>
#include "bucket_aggregator.h"
#include "dataframe_reader.h"
#include "snapshot_reader.h"

namespace flowsql::baseliner {
class SnapshotInput : public IBlockStreamChannel {
 public:
    ~SnapshotInput() override { Close(); }
    int Initialize(ConfigSnapshot config, std::shared_ptr<IDatabaseChannel> source, std::string_view exact_source,
                   std::function<int(const SnapshotPage&)> page_guard = {});
    int InitializeDataFrame(ConfigSnapshot config, const BlockDataFrameInputBindingV1& binding);
    const std::string& SourceFingerprint() const { return dataframe_reader_->SourceFingerprint(); }
    int ReadInputProgress(BlockInputProgressV1* output) const;
    const char* Category() override { return "baseliner"; }
    const char* Name() override { return "snapshot"; }
    const char* Type() override { return ChannelType::kBlockStream; }
    const char* Schema() override { return "baseline_observation_v1"; }
    int Open() override { return opened_ ? 0 : -1; }
    int Close() override;
    bool IsOpened() const override { return opened_; }
    int Flush() override { return 0; }
    BlockPollEvent PollBlock(int timeout_ms = 100) override;
    int ReleaseBlock(const std::shared_ptr<arrow::RecordBatch>& block) override;
    void Cancel() override;
    bool IsFinished() const override { return finished_; }
    std::shared_ptr<arrow::Schema> InputSchema() const { return MakeSchema(SchemaKind::kObservation); }
    const std::string& LastError() const { return error_; }
    const AggregationStats& Stats() const { return aggregator_->Stats(); }

 private:
    std::shared_ptr<SnapshotReader> reader_;
    std::shared_ptr<DataFrameReader> dataframe_reader_;
    BlockInputProgressV1 dataframe_progress_;
    bool boundary_emitted_ = false;
    bool progress_readable_ = false;
    std::unique_ptr<BucketAggregator> aggregator_;
    std::shared_ptr<arrow::RecordBatch> outstanding_;
    std::atomic<bool> cancelled_{false};
    std::atomic<bool> finished_{false};
    bool opened_ = false;
    size_t dataset_count_ = 0;
    size_t completed_datasets_ = 0;
    uint64_t max_bytes_ = 0;
    std::string error_;
    std::function<int(const SnapshotPage&)> page_guard_;
};
// Only DataFrame snapshots advertise progress; legacy database snapshot runners need no progress consumer.
class DataFrameSnapshotInput final : public SnapshotInput, public IBlockStreamInputProgressV1 {
 public:
    int ReadInputProgress(BlockInputProgressV1* output) const override {
        return SnapshotInput::ReadInputProgress(output);
    }
};
}  // namespace flowsql::baseliner
#endif
