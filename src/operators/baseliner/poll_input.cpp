// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include "poll_input.h"
#include <algorithm>
#include <cerrno>

namespace flowsql::baseliner {
int PollInput::Initialize(ConfigSnapshot config, std::shared_ptr<IDatabaseChannel> source,
                          std::string_view exact_source, const BlockInputProgressV1& restore,
                          const std::map<std::string, int64_t>& pending_starts,
                          const std::map<std::string, std::string>& source_epochs) {
    if (opened_ || source_ || cancelled_ || !source || config.config.mode != Mode::kPoll ||
        !config.config.database_source || config.config.source != exact_source ||
        !MatchesDatabaseSource(config.config, source.get())) {
        error_ = "invalid poll source";
        return -1;
    }
    std::vector<std::string> ids;
    for (const auto& d : config.config.datasets) ids.push_back(d.id);
    if (ids.empty() || !ValidateProgress(restore, ids).ok()) {
        error_ = "invalid poll restore envelope";
        return -1;
    }
    auto* capability = dynamic_cast<IDatabasePublishedSourceV1*>(source.get());
    if (!capability) {
        error_ = "source lacks published progress capability";
        return -1;
    }
    DatabaseSnapshotOptionsV1 options;
    options.operation_timeout_ms = config.config.persistence.operation_timeout_ms;
    std::shared_ptr<IDatabasePublishedProgressReaderV1> reader;
    if (capability->CreatePublishedProgressReader(options, &reader) != 0 || !reader) {
        error_ = "cannot create published progress reader";
        return -1;
    }
    config_ = std::move(config);
    source_ = std::move(source);
    std::atomic_store(&progress_, reader);
    if (cancelled_) {
        reader->Cancel();
        error_ = "poll input cancelled during initialization";
        return -1;
    }
    try {
        const int64_t width = BucketWidth(config_.config, TimeUnit::kNs);
        for (size_t i = 0; i < config_.config.datasets.size(); ++i) {
            const auto& d = config_.config.datasets[i];
            if (d.scope.run_ids.size() > 1) {
                error_ = "poll dataset requires one source epoch";
                return -1;
            }
            DatasetState state;
            state.request = {d.id,    d.schema,
                             d.table, d.scope.run_ids.empty() ? "" : d.scope.run_ids[0],
                             width,   d.row_semantics == "npm_period_increment"};
            PublishedDatasetProgressV1 latest;
            if (progress_->ReadProgress(state.request, "", &latest) != 0) {
                error_ = progress_->LastError();
                return -1;
            }
            if (!ValidateProgress({1, {latest.progress}}, {d.id}).ok() ||
                latest.progress.closed_before_bucket < latest.first_bucket ||
                (state.request.npm_period_increment &&
                 (latest.source_period_ns <= 0 || width % latest.source_period_ns))) {
                error_ = "invalid source publication contract";
                return -1;
            }
            auto pending = pending_starts.find(d.id);
            if (pending != pending_starts.end() && pending->second < latest.first_bucket) {
                error_ = "poll pending retention gap";
                return -1;
            }
            auto epoch = source_epochs.find(d.id);
            if (epoch != source_epochs.end() && epoch->second != latest.progress.epoch) {
                error_ = "poll pending source epoch mismatch";
                return -1;
            }
            state.epoch = latest.progress.epoch;
            state.source_period_ns = latest.source_period_ns;
            state.next_bucket = std::max(latest.first_bucket, d.scope.begin_bucket.value_or(latest.first_bucket));
            auto old = std::find_if(restore.datasets.begin(), restore.datasets.end(),
                                    [&](const auto& p) { return p.dataset_id == d.id; });
            if (old != restore.datasets.end()) {
                PublishedDatasetProgressV1 saved;
                if (progress_->ReadProgress(state.request, old->committed_position, &saved) != 0) {
                    error_ = progress_->LastError();
                    return -1;
                }
                if (old->epoch != state.epoch || old->epoch != saved.progress.epoch ||
                    old->closed_before_bucket != saved.progress.closed_before_bucket ||
                    saved.source_period_ns != state.source_period_ns ||
                    old->closed_before_bucket < latest.first_bucket ||
                    latest.progress.closed_before_bucket < old->closed_before_bucket) {
                    error_ = "poll restore epoch/position/retention mismatch";
                    return -1;
                }
                state.confirmed = *old;
                state.next_bucket = std::max(state.next_bucket, old->closed_before_bucket);
            }
            if (pending != pending_starts.end() && old == restore.datasets.end()) state.next_bucket = pending->second;
            states_.push_back(std::move(state));
            // Explicit selections are validated even when no target bucket is closed yet.
            auto probe = config_.config;
            probe.mode = Mode::kSnapshot;
            probe.datasets = {d};
            probe.datasets[0].scope.consistency = "immutable_range";
            probe.datasets[0].scope.begin_bucket = 0;
            probe.datasets[0].scope.end_bucket = 1;
            auto selection = std::make_shared<SnapshotReader>();
            std::atomic_store(&selection_, selection);
            if (cancelled_) selection->Cancel();
            const int selection_rc = selection->Open(std::move(probe), source_, exact_source);
            std::atomic_store(&selection_, std::shared_ptr<SnapshotReader>{});
            if (selection_rc != 0 || cancelled_) {
                error_ = selection->LastError();
                return -1;
            }
        }
    } catch (const std::exception& ex) {
        error_ = ex.what();
        return -1;
    }
    opened_ = true;
    return 0;
}
std::map<std::string, int64_t> PollInput::PendingStarts() const {
    std::map<std::string, int64_t> output;
    for (const auto& state : states_) output.emplace(state.request.dataset_id, state.next_bucket);
    return output;
}
std::map<std::string, std::string> PollInput::SourceEpochs() const {
    std::map<std::string, std::string> result;
    for (const auto& state : states_) result.emplace(state.request.dataset_id, state.epoch);
    return result;
}
int PollInput::Validate(size_t dataset, const std::string& position, PublishedDatasetProgressV1* output) {
    if (progress_->ReadProgress(states_[dataset].request, position, output) != 0) {
        error_ = progress_->LastError();
        return -1;
    }
    const auto& state = states_[dataset];
    if (output->progress.dataset_id != state.request.dataset_id || output->progress.epoch != state.epoch ||
        output->progress.committed_position.empty() || output->source_period_ns != state.source_period_ns ||
        state.next_bucket < output->first_bucket ||
        (!state.confirmed.committed_position.empty() &&
         output->progress.closed_before_bucket < state.confirmed.closed_before_bucket)) {
        error_ = "poll epoch/period/retention/regression mismatch";
        return -1;
    }
    return 0;
}
BlockPollEvent PollInput::Fail(std::string error) {
    error_ = std::move(error);
    finished_ = true;
    return cancelled_ ? BlockPollEvent{BlockPollEvent::kCancelled, nullptr, ECANCELED}
                      : BlockPollEvent{BlockPollEvent::kError, nullptr, EIO};
}
BlockPollEvent PollInput::PollBlock(int timeout_ms) {
    if (cancelled_) return {BlockPollEvent::kCancelled, nullptr, ECANCELED};
    if (!opened_ || !error_.empty()) return Fail(error_.empty() ? "poll input closed" : error_);
    if (outstanding_) return {BlockPollEvent::kTimeout, nullptr, 0};
    progress_readable_ = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(0, timeout_ms));
    if (!cycle_ && std::chrono::steady_clock::now() < next_poll_) {
        std::unique_lock<std::mutex> lock(wait_mutex_);
        wake_.wait_until(lock, std::min(deadline, next_poll_), [&] { return cancelled_.load(); });
        if (cancelled_) return {BlockPollEvent::kCancelled, nullptr, ECANCELED};
        if (std::chrono::steady_clock::now() < next_poll_) return {BlockPollEvent::kTimeout, nullptr, 0};
    }
    try {
        if (!cycle_) {
            bool changed = false;
            for (size_t count = 0; count < states_.size(); ++count) {
                auto& state = states_[dataset_];
                if (Validate(dataset_, "", &frozen_) != 0) return Fail(error_);
                if (frozen_.progress.committed_position != state.confirmed.committed_position) {
                    changed = true;
                    break;
                }
                dataset_ = (dataset_ + 1) % states_.size();
            }
            if (!changed) {
                next_poll_ =
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(config_.config.read.poll_interval_ms);
                std::unique_lock<std::mutex> lock(wait_mutex_);
                wake_.wait_until(lock, std::min(deadline, next_poll_), [&] { return cancelled_.load(); });
                return cancelled_ ? BlockPollEvent{BlockPollEvent::kCancelled, nullptr, ECANCELED}
                                  : BlockPollEvent{BlockPollEvent::kTimeout, nullptr, 0};
            }
            const auto& d = config_.config.datasets[dataset_];
            const auto end = std::min(frozen_.progress.closed_before_bucket,
                                      d.scope.end_bucket.value_or(frozen_.progress.closed_before_bucket));
            if (states_[dataset_].next_bucket < end) {
                auto window = config_;
                window.config.mode = Mode::kSnapshot;
                window.config.datasets = {d};
                window.config.datasets[0].scope.consistency = "immutable_range";
                window.config.datasets[0].scope.begin_bucket = states_[dataset_].next_bucket;
                window.config.datasets[0].scope.end_bucket = end;
                window.sha256_hex = states_[dataset_].epoch;
                auto created = std::make_shared<SnapshotInput>();
                auto guard = [this](const SnapshotPage& page) {
                    PublishedDatasetProgressV1 valid;
                    if (Validate(dataset_, frozen_.progress.committed_position, &valid) != 0 ||
                        valid.progress.closed_before_bucket != frozen_.progress.closed_before_bucket)
                        return -1;
                    if (!states_[dataset_].request.npm_period_increment || page.eof) return 0;
                    auto starts =
                        std::static_pointer_cast<arrow::Int64Array>(page.batch->GetColumnByName("period_start_ns"));
                    auto ends =
                        std::static_pointer_cast<arrow::Int64Array>(page.batch->GetColumnByName("period_end_ns"));
                    for (int64_t row = 0; row < page.batch->num_rows(); ++row) {
                        if (starts->IsNull(row) || ends->IsNull(row) ||
                            static_cast<__int128>(ends->Value(row)) - starts->Value(row) != valid.source_period_ns) {
                            error_ = "source period differs from frozen publication";
                            return -1;
                        }
                    }
                    return 0;
                };
                std::atomic_store(&cycle_, created);
                if (cancelled_) created->Cancel();
                if (created->Initialize(std::move(window), source_, config_.config.source, std::move(guard)) != 0)
                    return Fail(created->LastError());
                if (cancelled_) {
                    cycle_->Cancel();
                    return {BlockPollEvent::kCancelled, nullptr, ECANCELED};
                }
            }
        }
        if (cycle_) {
            auto event = cycle_->PollBlock(timeout_ms);
            if (event.kind == BlockPollEvent::kData) {
                outstanding_ = event.batch;
                return event;
            }
            if (event.kind != BlockPollEvent::kEof)
                return event.kind == BlockPollEvent::kError ? Fail(error_.empty() ? cycle_->LastError() : error_)
                                                            : event;
            std::atomic_store(&cycle_, std::shared_ptr<SnapshotInput>{});
        }
        PublishedDatasetProgressV1 boundary;
        if (Validate(dataset_, frozen_.progress.committed_position, &boundary) != 0 ||
            boundary.progress.closed_before_bucket != frozen_.progress.closed_before_bucket)
            return Fail(error_.empty() ? "frozen publication changed before confirmation" : error_);
        auto empty = arrow::RecordBatch::MakeEmpty(InputSchema());
        if (!empty.ok()) return Fail(empty.status().ToString());
        outstanding_ = *empty;
        outstanding_boundary_ = true;
        return {BlockPollEvent::kData, outstanding_, 0};
    } catch (const std::exception& ex) {
        return Fail(ex.what());
    }
}
int PollInput::ReleaseBlock(const std::shared_ptr<arrow::RecordBatch>& block) {
    // Stop wakes PollBlock through Cancel; an already leased batch must still be returned.
    if (!outstanding_ || block != outstanding_) return -1;
    if (outstanding_boundary_) {
        auto& state = states_[dataset_];
        state.confirmed = frozen_.progress;
        state.next_bucket = std::max(state.next_bucket, frozen_.progress.closed_before_bucket);
        dataset_ = (dataset_ + 1) % states_.size();
        outstanding_boundary_ = false;
    } else if (!cycle_ || cycle_->ReleaseBlock(block) != 0)
        return -1;
    outstanding_.reset();
    progress_readable_ = true;
    return 0;
}
int PollInput::ReadInputProgress(BlockInputProgressV1* output) const {
    // This reads the released batch's confirmed snapshot without starting another database call.
    if (!output || !progress_readable_ || outstanding_ || !error_.empty()) return -1;
    *output = {};
    for (const auto& state : states_)
        if (!state.confirmed.committed_position.empty()) output->datasets.push_back(state.confirmed);
    return 0;
}
void PollInput::Cancel() {
    {
        std::lock_guard<std::mutex> lock(wait_mutex_);
        cancelled_ = true;
    }
    wake_.notify_all();
    auto progress = std::atomic_load(&progress_);
    if (progress) progress->Cancel();
    auto cycle = std::atomic_load(&cycle_);
    if (cycle) cycle->Cancel();
    auto selection = std::atomic_load(&selection_);
    if (selection) selection->Cancel();
}
int PollInput::Close() {
    Cancel();
    outstanding_.reset();
    std::atomic_store(&cycle_, std::shared_ptr<SnapshotInput>{});
    std::atomic_store(&progress_, std::shared_ptr<IDatabasePublishedProgressReaderV1>{});
    std::atomic_store(&selection_, std::shared_ptr<SnapshotReader>{});
    source_.reset();
    states_.clear();
    frozen_ = {};
    outstanding_boundary_ = progress_readable_ = false;
    dataset_ = 0;
    opened_ = false;
    finished_ = true;
    return 0;
}
}  // namespace flowsql::baseliner
