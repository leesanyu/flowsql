// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_FRAMEWORK_CORE_DATAFRAME_CHANNEL_H_
#define _FLOWSQL_FRAMEWORK_CORE_DATAFRAME_CHANNEL_H_

#include <mutex>
#include <string>

#include "dataframe.h"
#include "framework/interfaces/ichannel.h"
#include "framework/interfaces/idataframe_channel.h"
#include "framework/interfaces/idataframe_snapshot.h"

namespace flowsql {

// DataFrameChannel — IDataFrameChannel 的内存实现
// Read() 快照语义（非破坏性），Write() 替换语义
class DataFrameChannel : public IAppendableDataFrameChannel, public IDataFrameSnapshotReaderV1 {
 public:
    DataFrameChannel(const std::string& category, const std::string& name);
    ~DataFrameChannel() override = default;

    // IChannel — 身份
    const char* Category() override { return category_.c_str(); }
    const char* Name() override { return name_.c_str(); }
    const char* Type() override { return ChannelType::kDataFrame; }
    const char* Schema() override;

    // IChannel — 生命周期
    int Open() override;
    int Close() override;
    bool IsOpened() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return opened_;
    }
    int Flush() override { return 0; }

    // IDataFrameChannel — 数据读写
    int Write(IDataFrame* df) override;
    int Append(IDataFrame* df) override;
    int Read(IDataFrame* df) override;
    int CaptureSnapshot(const DataFrameSnapshotRequestV1& request, std::shared_ptr<arrow::RecordBatch>* output,
                        uint64_t* retained_bytes, std::string* error) override;

 private:
    bool SchemaCompatibleLocked(const std::vector<Field>& lhs, const std::vector<Field>& rhs) const;
    void RefreshSchemaCacheLocked();

    std::string category_;
    std::string name_;
    bool opened_ = false;
    DataFrame data_;            // 内部存储
    std::string schema_cache_;  // Schema() 返回值缓存
    mutable std::mutex mutex_;  // 保护并发读写
};

}  // namespace flowsql

#endif  // _FLOWSQL_FRAMEWORK_CORE_DATAFRAME_CHANNEL_H_
