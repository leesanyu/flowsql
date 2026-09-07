// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "dataframe_channel.h"

namespace flowsql {

DataFrameChannel::DataFrameChannel(const std::string& category, const std::string& name)
    : category_(category), name_(name) {}

bool DataFrameChannel::SchemaCompatibleLocked(const std::vector<Field>& lhs,
                                              const std::vector<Field>& rhs) const {
    if (lhs.size() != rhs.size()) return false;
    for (size_t i = 0; i < lhs.size(); ++i) {
        if (lhs[i].name != rhs[i].name || lhs[i].type != rhs[i].type) {
            return false;
        }
    }
    return true;
}

void DataFrameChannel::RefreshSchemaCacheLocked() {
    auto fields = data_.GetSchema();
    if (fields.empty()) {
        schema_cache_ = "[]";
        return;
    }
    schema_cache_ = "[";
    for (size_t i = 0; i < fields.size(); ++i) {
        if (i > 0) schema_cache_ += ",";
        schema_cache_ += "{\"name\":\"" + fields[i].name + "\",\"type\":" +
                         std::to_string(static_cast<int>(fields[i].type)) + "}";
    }
    schema_cache_ += "]";
}

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
    RefreshSchemaCacheLocked();
    return 0;
}

int DataFrameChannel::Append(IDataFrame* df) {
    if (!opened_ || !df) return -1;

    std::lock_guard<std::mutex> lock(mutex_);
    const std::vector<Field> incoming_schema = df->GetSchema();
    if (incoming_schema.empty()) {
        return 0;
    }

    const std::vector<Field> current_schema = data_.GetSchema();
    if (current_schema.empty()) {
        const auto incoming_batch = df->ToArrow();
        if (incoming_batch) {
            data_.FromArrow(incoming_batch);
        } else {
            data_.SetSchema(incoming_schema);
        }
        RefreshSchemaCacheLocked();
        return 0;
    } else if (!SchemaCompatibleLocked(current_schema, incoming_schema)) {
        return -1;
    }

    const auto current_batch = data_.ToArrow();
    const auto incoming_batch = df->ToArrow();
    if (current_batch && incoming_batch &&
        current_batch->schema()->Equals(incoming_batch->schema())) {
        auto concatenated = arrow::ConcatenateRecordBatches({current_batch, incoming_batch});
        if (!concatenated.ok()) {
            return -1;
        }
        data_.FromArrow(*concatenated);
        RefreshSchemaCacheLocked();
        return 0;
    }

    DataFrame merged;
    merged.SetSchema(current_schema);

    const int32_t current_rows = data_.RowCount();
    for (int32_t i = 0; i < current_rows; ++i) {
        if (merged.AppendRow(data_.GetRow(i)) != 0) {
            return -1;
        }
    }

    const int32_t incoming_rows = df->RowCount();
    for (int32_t i = 0; i < incoming_rows; ++i) {
        if (merged.AppendRow(df->GetRow(i)) != 0) {
            return -1;
        }
    }

    const auto merged_batch = merged.ToArrow();
    if (merged_batch) {
        data_.FromArrow(merged_batch);
    } else {
        data_.Clear();
        data_.SetSchema(current_schema);
    }
    RefreshSchemaCacheLocked();
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
