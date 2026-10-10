// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include "snapshot_input.h"
#include <framework/core/dataframe.h>
#include <framework/interfaces/idataframe_channel.h>
#include <cerrno>

namespace flowsql::baseliner {
int SnapshotInput::Initialize(ConfigSnapshot config, std::shared_ptr<IDatabaseChannel> source,
                              std::string_view exact_source, std::function<int(const SnapshotPage&)> page_guard) {
    if (opened_ || reader_ || cancelled_) {
        error_ = "input already initialized/cancelled";
        return -1;
    }
    auto reader = std::make_shared<SnapshotReader>();
    std::atomic_store(&reader_, reader);
    if (cancelled_) reader->Cancel();
    if (reader->Open(std::move(config.config), std::move(source), exact_source) != 0) {
        error_ = reader->LastError();
        std::atomic_store(&reader_, std::shared_ptr<SnapshotReader>{});
        return -1;
    }
    if (cancelled_) {
        reader->Cancel();
        error_ = "snapshot input cancelled during initialization";
        return -1;
    }
    dataset_count_ = reader->Config().datasets.size();
    page_guard_ = std::move(page_guard);
    max_bytes_ = reader_->Config().read.max_pending_bytes;
    aggregator_ = std::make_unique<BucketAggregator>(reader_->Config(), config.sha256_hex);
    opened_ = true;
    return 0;
}
int SnapshotInput::InitializeDataFrame(ConfigSnapshot config, const BlockDataFrameInputBindingV1& binding) {
    if (opened_ || reader_ || dataframe_reader_ || cancelled_ || !ValidBlockDataFrameInputBindingV1(binding)) {
        error_ = "invalid DataFrame input initialization";
        return -1;
    }
    auto reader = std::make_shared<DataFrameReader>();
    std::atomic_store(&dataframe_reader_, reader);
    if (cancelled_) reader->Cancel();
    DataFrame snapshot;
    if (binding.source->Read(&snapshot) != 0 || reader->Open(config.config, binding, snapshot.ToArrow()) != 0) {
        error_ = reader->LastError().empty() ? "DataFrame snapshot read failed" : reader->LastError();
        return -1;
    }
    if (cancelled_) {
        reader->Cancel();
        error_ = "DataFrame input cancelled during initialization";
        return -1;
    }
    dataset_count_ = 1;
    max_bytes_ = config.config.read.max_pending_bytes;
    const auto& dataset = config.config.datasets.front();
    dataframe_progress_.datasets.push_back({dataset.id, reader->SourceFingerprint(),
                                            "eof:" + reader->SourceFingerprint(),
                                            dataset.scope.end_bucket.value_or(std::numeric_limits<int64_t>::min())});
    aggregator_ = std::make_unique<BucketAggregator>(config.config, reader->SourceFingerprint());
    opened_ = true;
    return 0;
}
int SnapshotInput::Close() {
    Cancel();
    outstanding_.reset();
    aggregator_.reset();
    std::atomic_store(&reader_, std::shared_ptr<SnapshotReader>{});
    std::atomic_store(&dataframe_reader_, std::shared_ptr<DataFrameReader>{});
    opened_ = false;
    finished_ = true;
    return 0;
}
BlockPollEvent SnapshotInput::PollBlock(int) {
    if (cancelled_) {
        finished_ = true;
        return {BlockPollEvent::kCancelled, nullptr, ECANCELED};
    }
    if (!error_.empty()) {
        finished_ = true;
        return {BlockPollEvent::kError, nullptr, EIO};
    }
    if (!opened_) {
        error_ = "snapshot input closed";
        finished_ = true;
        return {BlockPollEvent::kError, nullptr, EINVAL};
    }
    if (outstanding_) return {BlockPollEvent::kTimeout, nullptr, 0};
    if (completed_datasets_ == dataset_count_) {
        if (dataframe_reader_ && !boundary_emitted_) {
            if (MakeObservationBatch({}, max_bytes_, &outstanding_, &error_) != 0)
                return {BlockPollEvent::kError, nullptr, EIO};
            if (cancelled_) {
                outstanding_.reset();
                return {BlockPollEvent::kCancelled, nullptr, ECANCELED};
            }
            boundary_emitted_ = true;
            return {BlockPollEvent::kData, outstanding_, 0};
        }
        finished_ = true;
        return {BlockPollEvent::kEof, nullptr, 0};
    }
    while (!cancelled_) {
        SnapshotPage page;
        if ((dataframe_reader_ ? dataframe_reader_->Next(&page) : reader_->Next(&page)) != 0) {
            error_ = dataframe_reader_ ? dataframe_reader_->LastError() : reader_->LastError();
            break;
        }
        if (page_guard_ && page_guard_(page) != 0) {
            error_ = "source publication/page validation failed";
            break;
        }
        if (dataframe_reader_ && page.batch && page.batch->num_rows()) {
            const auto& config = dataframe_reader_->Config();
            const auto& dataset = config.datasets.front();
            auto times =
                std::static_pointer_cast<arrow::Int64Array>(page.batch->GetColumnByName(dataset.bucket_column));
            const auto bucket = ToBucket(times->Value(times->length() - 1), BucketWidth(config, dataset.unit));
            if (bucket == std::numeric_limits<int64_t>::max()) {
                error_ = "DataFrame completed bucket overflow";
                break;
            }
            auto& closed = dataframe_progress_.datasets.front().closed_before_bucket;
            closed = std::max(closed, bucket + 1);
        }
        std::vector<Observation> observations;
        int rc;
        if (page.eof) {
            rc = aggregator_->FinishDataset(page.dataset_index, &observations);
            ++completed_datasets_;
        } else
            rc = aggregator_->Push(page.dataset_index, page.batch, &observations);
        page.batch.reset();
        if (rc != 0) {
            error_ = aggregator_->LastError();
            break;
        }
        if (!observations.empty()) {
            if (MakeObservationBatch(observations, max_bytes_, &outstanding_, &error_) != 0) break;
            if (cancelled_) {
                outstanding_.reset();
                break;
            }
            return {BlockPollEvent::kData, outstanding_, 0};
        }
        if (completed_datasets_ == dataset_count_) {
            if (dataframe_reader_) return PollBlock();
            finished_ = true;
            return {BlockPollEvent::kEof, nullptr, 0};
        }
    }
    finished_ = true;
    return cancelled_ ? BlockPollEvent{BlockPollEvent::kCancelled, nullptr, ECANCELED}
                      : BlockPollEvent{BlockPollEvent::kError, nullptr, EIO};
}
int SnapshotInput::ReleaseBlock(const std::shared_ptr<arrow::RecordBatch>& block) {
    if (!outstanding_ || block != outstanding_) return -1;
    outstanding_.reset();
    if (boundary_emitted_ && !cancelled_) progress_readable_ = true;
    return 0;
}
int SnapshotInput::ReadInputProgress(BlockInputProgressV1* output) const {
    if (!output || outstanding_ || cancelled_) return -1;
    *output = progress_readable_ ? dataframe_progress_ : BlockInputProgressV1{};
    return 0;
}
void SnapshotInput::Cancel() {
    cancelled_ = true;
    auto reader = std::atomic_load(&reader_);
    if (reader) reader->Cancel();
    auto dataframe = std::atomic_load(&dataframe_reader_);
    if (dataframe) dataframe->Cancel();
}
}  // namespace flowsql::baseliner
