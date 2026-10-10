// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <framework/interfaces/iblock_transform_database_input.h>
#include <framework/interfaces/idatabase_snapshot_source.h>

namespace flowsql {
const Guid IID_DATABASE_PUBLISHED_SOURCE_V1 = {
    0xa932e75b, 0x3762, 0x4059, {0xa6, 0x44, 0xef, 0x71, 0xa9, 0x29, 0x4d, 0x62}};
struct PublishedDatasetRequestV1 {
    std::string dataset_id;
    std::string schema;
    std::string relation;
    std::string run_id;
    int64_t bucket_ns = 0;
    bool npm_period_increment = false;
};
struct PublishedDatasetProgressV1 {
    BlockDatasetProgressV1 progress;
    int64_t source_period_ns = 0;  // zero for generic sources without a fixed period
    int64_t first_bucket = 0;      // earliest retained complete bucket; recovery before it fails
};
// Adapter proves that all selected facts in buckets below closed_before_bucket are published and immutable.
// Reading that closed time range freezes the data prefix even if the writer publishes a newer position.
// No assumptions about MAX(timestamp), auto IDs, polling silence, or processing time are permitted.
interface IDatabasePublishedProgressReaderV1 {
    virtual ~IDatabasePublishedProgressReaderV1() = default;
    // Empty position selects latest, otherwise recover exactly that durable record. Missing/expired is an error.
    // Fresh source validation on every call. Failure leaves output empty. Serialized except Cancel.
    virtual int ReadProgress(const PublishedDatasetRequestV1& request, const std::string& position,
                             PublishedDatasetProgressV1* output) = 0;
    virtual void Cancel() = 0;
    virtual std::string LastError() const = 0;
};
interface IDatabasePublishedSourceV1 {
    virtual ~IDatabasePublishedSourceV1() = default;
    // Task-private cancellable reader; caller retains the channel lease for its entire lifetime.
    virtual int CreatePublishedProgressReader(const DatabaseSnapshotOptionsV1& options,
                                              std::shared_ptr<IDatabasePublishedProgressReaderV1>* output) = 0;
};
}  // namespace flowsql
