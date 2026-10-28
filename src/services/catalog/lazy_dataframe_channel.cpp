// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "lazy_dataframe_channel.h"
#include <framework/core/dataframe_channel.h>

#include <filesystem>
#include <fstream>

namespace flowsql::catalog {
namespace {
bool Interrupted(const DataFrameSnapshotRequestV1* request, std::string* error) {
    if (!request) return false;
    if (request->cancelled && request->cancelled->load()) {
        *error = "CANCELLED";
        return true;
    }
    if (std::chrono::steady_clock::now() >= request->deadline) {
        *error = "TIMEOUT";
        return true;
    }
    return false;
}
}  // namespace

LazyDataFrameChannel::LazyDataFrameChannel(std::string path, std::string name, Loader loader)
    : path_(std::move(path)), name_(std::move(name)), loader_(std::move(loader)) {}

const char* LazyDataFrameChannel::Name() {
    static thread_local std::string name;
    std::lock_guard<std::mutex> lock(mutex_);
    name = name_;
    return name.c_str();
}
const char* LazyDataFrameChannel::Schema() {
    static thread_local std::string schema;
    std::lock_guard<std::mutex> lock(mutex_);
    schema = data_ ? data_->Schema() : "null";
    return schema.c_str();
}
bool LazyDataFrameChannel::IsOpened() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return opened_ && !retired_;
}
int LazyDataFrameChannel::Open() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (retired_) return -1;
    opened_ = true;
    return 0;
}
void LazyDataFrameChannel::ClearLocked() {
    if (load_cancelled_) load_cancelled_->store(true);
    data_.reset();
    state_ = "unloaded";
    error_.clear();
    changed_.notify_all();
}
int LazyDataFrameChannel::Close() {
    std::lock_guard<std::mutex> lock(mutex_);
    opened_ = false;
    ClearLocked();
    return 0;
}
void LazyDataFrameChannel::Retire() {
    std::lock_guard<std::mutex> lock(mutex_);
    retired_ = true;
    opened_ = false;
    ClearLocked();
}
int LazyDataFrameChannel::RenameFile(const std::string& path, const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (retired_) return -1;
    std::error_code error;
    std::filesystem::rename(path_, path, error);
    if (error) return -1;
    path_ = path;
    name_ = name;
    return 0;
}
std::shared_ptr<IDataFrameChannel> LazyDataFrameChannel::EnsureLoaded(const DataFrameSnapshotRequestV1* request,
                                                                      std::string* error) {
    error->clear();
    std::unique_lock<std::mutex> lock(mutex_);
    if (Interrupted(request, error)) return {};
    if (!opened_ || retired_) {
        *error = "SOURCE_UNAVAILABLE";
        return {};
    }
    if (data_) return data_;
    if (loading_) {
        // An interrupted waiter does not cancel the shared parser.
        const auto attempt = attempt_;
        while (!attempt->done && opened_ && !retired_ && !data_) {
            if (Interrupted(request, error)) return {};
            changed_.wait_for(lock, std::chrono::milliseconds(5));
        }
        if (Interrupted(request, error)) return {};
        if (attempt->done && !attempt->error.empty()) {
            *error = attempt->error;
            return {};
        }
        if (opened_ && !retired_ && data_) return data_;
        *error = attempt->error.empty() ? "SOURCE_UNAVAILABLE" : attempt->error;
        return {};
    }
    loading_ = true;
    const auto attempt = std::make_shared<Attempt>();
    attempt_ = attempt;
    state_ = "loading";
    error_.clear();
    const auto cancelled = std::make_shared<std::atomic_bool>(false);
    load_cancelled_ = cancelled;
    // Opening under the state lock lets rename coordinate the path without holding it during parsing.
    std::ifstream input(path_, std::ios::binary);
    const auto name = name_;
    lock.unlock();
    std::string load_error;
    const auto interrupted = [&] {
        if (cancelled->load()) {
            load_error = "SOURCE_UNAVAILABLE";
            return true;
        }
        return Interrupted(request, &load_error);
    };
    std::shared_ptr<IDataFrameChannel> loaded;
    try {
        if (input && !interrupted()) loaded = loader_(input, name, interrupted);
    } catch (const std::exception&) {
        load_error = "CSV_LOAD_FAILED";
    }
    lock.lock();
    const bool aborted = interrupted();
    if (loaded && !aborted && opened_ && !retired_) {
        data_ = std::move(loaded);
        state_ = "loaded";
        error_.clear();
    } else if (!data_) {
        error_ = load_error.empty() ? "CSV_LOAD_FAILED" : load_error;
        state_ = aborted ? "unloaded" : "failed";
    }
    loading_ = false;
    attempt->done = true;
    attempt->error = load_error.empty() ? error_ : load_error;
    changed_.notify_all();
    if (aborted || !opened_ || retired_ || !data_) {
        *error = load_error.empty() ? (error_.empty() ? "SOURCE_UNAVAILABLE" : error_) : load_error;
        return {};
    }
    return data_;
}
int LazyDataFrameChannel::Read(IDataFrame* frame) {
    if (!frame) return -1;
    std::string error;
    const auto data = EnsureLoaded(nullptr, &error);
    return data ? data->Read(frame) : -1;
}
int LazyDataFrameChannel::Append(IDataFrame* frame) {
    if (!frame) return -1;
    std::string error;
    const auto data = EnsureLoaded(nullptr, &error);
    std::lock_guard<std::mutex> lock(mutex_);
    // A concurrent replacement/close must never receive an append intended for the old version.
    if (!opened_ || retired_ || !data || data != data_) return -1;
    return dynamic_cast<IAppendableDataFrameChannel*>(data.get())->Append(frame);
}
int LazyDataFrameChannel::Write(IDataFrame* frame) {
    if (!frame) return -1;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!opened_ || retired_) return -1;
    auto replacement = std::make_shared<DataFrameChannel>("dataframe", name_);
    replacement->Open();
    if (replacement->Write(frame) != 0) return -1;
    if (load_cancelled_) load_cancelled_->store(true);
    data_ = std::move(replacement);
    ++replacement_generation_;
    state_ = "loaded";
    error_.clear();
    changed_.notify_all();
    return 0;
}
int LazyDataFrameChannel::CaptureSnapshot(const DataFrameSnapshotRequestV1& request,
                                          std::shared_ptr<arrow::RecordBatch>* output, uint64_t* retained_bytes,
                                          std::string* error) {
    if (!output || !retained_bytes || !error) return -1;
    output->reset();
    *retained_bytes = 0;
    const auto data = EnsureLoaded(&request, error);
    if (!data) return -1;
    return dynamic_cast<IDataFrameSnapshotReaderV1*>(data.get())
        ->CaptureSnapshot(request, output, retained_bytes, error);
}
DataFrameMetadataV1 LazyDataFrameChannel::DescribeMetadata() {
    std::lock_guard<std::mutex> lock(mutex_);
    DataFrameMetadataV1 result;
    result.replacement_generation = replacement_generation_;
    if (!opened_ || retired_)
        result.error = "SOURCE_UNAVAILABLE";
    else if (state_ == "failed")
        result.error = error_;
    else if (!data_)
        result.pending = true;
    else {
        auto* reader = dynamic_cast<IDataFrameMetadataV1*>(data_.get());
        if (reader)
            result.schema = reader->DescribeMetadata().schema;
        else
            result.error = "METADATA_UNSUPPORTED";
    }
    return result;
}

LazyDataFrameChannel::Metadata LazyDataFrameChannel::Describe() {
    std::lock_guard<std::mutex> lock(mutex_);
    Metadata result{state_, error_, -1, {}};
    if (data_) {
        DataFrame frame;
        if (data_->Read(&frame) == 0) {
            result.rows = frame.RowCount();
            result.schema = frame.GetSchema();
        }
    }
    return result;
}
}  // namespace flowsql::catalog
