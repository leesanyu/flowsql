// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#ifndef FLOWSQL_BASELINER_BUCKET_AGGREGATOR_H_
#define FLOWSQL_BASELINER_BUCKET_AGGREGATOR_H_
#include <arrow/api.h>
#include <memory>
#include "baseliner_contract.h"

namespace flowsql::baseliner {
struct RelationValues {
    std::string metric;
    double total = 0;
    uint32_t active_count = 0;
    std::vector<double> values_by_group;
};
struct Observation {
    std::string dataset_id;
    std::string metric_id;
    BaselineTaskKind kind = BaselineTaskKind::kValue;
    std::string identity;
    std::string source_epoch;
    int64_t bucket = 0;
    std::optional<double> value;
    std::optional<uint64_t> sample_count;
    std::optional<double> numerator;
    std::optional<double> denominator;
    std::vector<uint32_t> groups;
    std::vector<RelationValues> metrics;
};
struct AggregationStats {
    uint64_t duplicate_rows = 0;
    uint64_t terminal_rows = 0;
    std::map<std::string, uint64_t> skipped;
};
// One pending target bucket, bounded aggregation/dedup/identity/output storage. No algorithm state or clocks.
// Push/FinishDataset are serialized; failed instance is terminal and leaves output empty.
class BucketAggregator {
 public:
    BucketAggregator(TaskConfig config, std::string epoch);
    ~BucketAggregator();
    int Push(size_t dataset, const std::shared_ptr<arrow::RecordBatch>& batch, std::vector<Observation>* output);
    int FinishDataset(size_t dataset, std::vector<Observation>* output);
    const std::string& LastError() const;
    const AggregationStats& Stats() const;

 private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
int MakeObservationBatch(const std::vector<Observation>& rows, uint64_t max_bytes,
                         std::shared_ptr<arrow::RecordBatch>* output, std::string* error);
}  // namespace flowsql::baseliner
#endif
