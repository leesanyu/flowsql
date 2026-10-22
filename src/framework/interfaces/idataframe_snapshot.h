// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include <arrow/api.h>
#include <common/guid.h>
#include <common/typedef.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace flowsql {
const Guid IID_DATAFRAME_SNAPSHOT_PROVIDER_V1 = {
    0x7cb940e1, 0xf280, 0x458e, {0xa8, 0xc9, 0x3b, 0x09, 0xd7, 0xa5, 0x70, 0x22}};

struct DataFrameSourceV1 {
    std::string source_id, label;
};
struct DataFrameSnapshotV1 {
    DataFrameSnapshotV1() = default;
    DataFrameSnapshotV1(const DataFrameSnapshotV1&) = default;
    DataFrameSnapshotV1(DataFrameSnapshotV1&&) noexcept = default;
    // Swap assignment also releases old data before its library lease, as destruction does.
    DataFrameSnapshotV1& operator=(DataFrameSnapshotV1 other) noexcept {
        source_id.swap(other.source_id);
        snapshot_id.swap(other.snapshot_id);
        schema_revision.swap(other.schema_revision);
        captured_at.swap(other.captured_at);
        std::swap(row_count, other.row_count);
        std::swap(retained_bytes, other.retained_bytes);
        lifetime.swap(other.lifetime);
        batch.swap(other.batch);
        return *this;
    }
    std::string source_id, snapshot_id, schema_revision, captured_at;
    uint64_t row_count = 0, retained_bytes = 0;
    // Retain lifetime with batch and derived results. Member order releases batch first.
    std::shared_ptr<void> lifetime;
    std::shared_ptr<arrow::RecordBatch> batch;
};
struct DataFrameSnapshotRequestV1 {
    std::string source_id;
    uint64_t max_bytes = 0;
    std::chrono::steady_clock::time_point deadline;
    std::shared_ptr<std::atomic_bool> cancelled;
};
// Published buffers must be immutable. Capture returns a stable, owned batch; failure clears output.
// Separate capability: existing IDataFrameChannel vtables remain unchanged.
interface IDataFrameSnapshotReaderV1 {
    virtual ~IDataFrameSnapshotReaderV1() = default;
    virtual int CaptureSnapshot(const DataFrameSnapshotRequestV1& request, std::shared_ptr<arrow::RecordBatch>* output,
                                uint64_t* retained_bytes, std::string* error) = 0;
};
interface IDataFrameSnapshotProviderV1 {
    virtual ~IDataFrameSnapshotProviderV1() = default;
    virtual int List(std::vector<DataFrameSourceV1> * output, std::string * error) = 0;
    virtual int Acquire(const DataFrameSnapshotRequestV1& request, DataFrameSnapshotV1* output, std::string* error) = 0;
};
}  // namespace flowsql
