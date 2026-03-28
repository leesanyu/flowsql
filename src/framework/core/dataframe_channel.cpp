#include "dataframe_channel.h"

namespace flowsql {

DataFrameChannel::DataFrameChannel(const std::string& category, const std::string& name)
    : category_(category), name_(name) {}

const char* DataFrameChannel::Schema() {
    std::lock_guard<std::mutex> lock(mutex_);
    return schema_cache_.c_str();
}

int DataFrameChannel::Open() {
    opened_ = true;
    return 0;
}

int DataFrameChannel::Close() {
    std::lock_guard<std::mutex> lock(mutex_);
    opened_ = false;
    data_.Clear();
    return 0;
}

int DataFrameChannel::Write(IDataFrame* df) {
    if (!opened_ || !df) return -1;

    std::lock_guard<std::mutex> lock(mutex_);
    // 替换语义：通过 Arrow RecordBatch 零拷贝传递
    auto batch = df->ToArrow();
    data_.Clear();
    if (batch) {
        data_.FromArrow(batch);
    }
    // 更新 schema 缓存
    auto fields = data_.GetSchema();
    if (fields.empty()) {
        schema_cache_ = "[]";
    } else {
        schema_cache_ = "[";
        for (size_t i = 0; i < fields.size(); ++i) {
            if (i > 0) schema_cache_ += ",";
            schema_cache_ += "{\"name\":\"" + fields[i].name + "\",\"type\":" +
                             std::to_string(static_cast<int>(fields[i].type)) + "}";
        }
        schema_cache_ += "]";
    }
    return 0;
}

int DataFrameChannel::Read(IDataFrame* df) {
    if (!opened_ || !df) return -1;

    std::lock_guard<std::mutex> lock(mutex_);
    // 快照语义：通过 Arrow RecordBatch 零拷贝传递
    auto batch = data_.ToArrow();
    if (batch) {
        df->FromArrow(batch);
    }
    return 0;
}

}  // namespace flowsql
