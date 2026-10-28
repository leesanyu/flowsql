// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include <framework/core/dataframe.h>
#include <framework/interfaces/idataframe_channel.h>
#include <framework/interfaces/idataframe_metadata.h>
#include <framework/interfaces/idataframe_snapshot.h>

#include <condition_variable>
#include <functional>
#include <istream>
#include <mutex>

namespace flowsql::catalog {

// Catalog-private implementation; public channel and snapshot vtables remain unchanged.
class LazyDataFrameChannel : public IAppendableDataFrameChannel,
                             public IDataFrameSnapshotReaderV1,
                             public IDataFrameMetadataV1 {
 public:
    using Check = std::function<bool()>;
    using Loader = std::function<std::shared_ptr<IDataFrameChannel>(std::istream&, const std::string&, const Check&)>;
    struct Metadata {
        std::string state, error;
        int64_t rows = -1;
        std::vector<Field> schema;
    };

    LazyDataFrameChannel(std::string path, std::string name, Loader loader);
    const char* Category() override { return "dataframe"; }
    const char* Name() override;
    const char* Type() override { return ChannelType::kDataFrame; }
    const char* Schema() override;
    int Open() override;
    int Close() override;
    bool IsOpened() const override;
    int Flush() override { return 0; }
    int Write(IDataFrame* frame) override;
    int Append(IDataFrame* frame) override;
    int Read(IDataFrame* frame) override;
    int CaptureSnapshot(const DataFrameSnapshotRequestV1& request, std::shared_ptr<arrow::RecordBatch>* output,
                        uint64_t* retained_bytes, std::string* error) override;
    Metadata Describe();
    DataFrameMetadataV1 DescribeMetadata() override;
    void Retire();
    int RenameFile(const std::string& path, const std::string& name);

 private:
    std::shared_ptr<IDataFrameChannel> EnsureLoaded(const DataFrameSnapshotRequestV1* request, std::string* error);
    void ClearLocked();
    std::string path_, name_, state_ = "unloaded", error_;
    Loader loader_;
    std::shared_ptr<IDataFrameChannel> data_;
    bool opened_ = true;
    bool retired_ = false;
    bool loading_ = false;
    uint64_t replacement_generation_ = 0;
    struct Attempt {
        bool done = false;
        std::string error;
    };
    std::shared_ptr<Attempt> attempt_;
    std::shared_ptr<std::atomic_bool> load_cancelled_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
};
}  // namespace flowsql::catalog
