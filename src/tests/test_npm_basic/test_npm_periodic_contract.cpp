// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <arrow/api.h>
#include <operators/npm_basic/npm_analysis_contract.h>
#include <operators/npm_basic/npm_protocol_contract.h>
#include <operators/npm_basic/output/npm_basic_result_encoder.h>
#include <cassert>
#include <limits>

using namespace flowsql::npm;

int main() {
    for (auto mode : {NpmRunMode::kOffline, NpmRunMode::kRealtime}) {
        const auto config = DefaultNpmAnalysisConfig(mode);
        assert(config.result_mode == NpmResultMode::kPeriodicSnapshot);
        assert(config.output_interval_ns == 30000000000LL);
        assert(ValidateNpmAnalysisConfig(config) == NpmAnalysisConfigError::kNone);
        for (int64_t period : {15000000000LL, 30000000000LL, 60000000000LL}) {
            auto custom = config;
            custom.output_interval_ns = period;
            assert(ValidateNpmAnalysisConfig(custom) == NpmAnalysisConfigError::kNone);
        }
    }
    const auto schema = NpmBasicResultSchema();
    assert(schema->Equals(*NpmBasicEntityDescriptorV1().schema, true));
    assert(schema->num_fields() == 32);
    assert(schema->GetFieldByName("primary_label_id")->nullable());
    assert(schema->metadata()->Get("flowsql.schema_version").ValueOrDie() == "1");
    assert(NpmBasicEntityDescriptorV1().schema_version == 1);
    NpmBasicResult row;
    row.session_id = 1;
    row.revision = 1;
    row.protocol_status = NpmProtocolStatus::kUnknown;
    row.wire_bytes_ab = 42;
    row.wire_bytes_ba = 58;
    row.wire_bytes_total = 100;
    std::shared_ptr<arrow::RecordBatch> batch;
    assert(EncodeNpmBasicResults({row}, &batch) == NpmBasicEncodeError::kNone);
    assert(batch->GetColumnByName("primary_label_id")->IsNull(0));
    assert(batch->GetColumnByName("period_start_ns")->IsNull(0));
    assert(EncodeNpmBasicResults({row}, &batch, nullptr, true) == NpmBasicEncodeError::kNone);
    const auto label = std::static_pointer_cast<arrow::UInt32Array>(batch->GetColumnByName("primary_label_id"));
    assert(!label->IsNull(0) && label->Value(0) == 0);
    row.primary_label_id = 7;
    row.period = NpmBasicPeriodStats{0, 30000000000LL, true, 1, 1, 42, 58, 100};
    assert(EncodeNpmBasicResults({row}, &batch, nullptr, true) == NpmBasicEncodeError::kNone);
    assert(std::static_pointer_cast<arrow::UInt32Array>(batch->GetColumnByName("primary_label_id"))->Value(0) == 7);
    assert(
        std::static_pointer_cast<arrow::UInt64Array>(batch->GetColumnByName("interval_wire_bytes_total"))->Value(0) ==
        100);
    const auto retained = batch;
    row.period->interval_wire_bytes_total = 99;
    assert(EncodeNpmBasicResults({row}, &batch) == NpmBasicEncodeError::kInvalidResult && batch == retained);
    row.period.reset();
    row.wire_bytes_ab = std::numeric_limits<uint64_t>::max();
    row.wire_bytes_ba = 1;
    assert(EncodeNpmBasicResults({row}, &batch) == NpmBasicEncodeError::kInvalidResult);
    row.wire_bytes_ba = 0;
    row.wire_bytes_total = row.wire_bytes_ab;
    row.is_final = true;
    row.end_reason = NpmSessionEndReason::kEof;
    assert(EncodeNpmBasicResults({row}, &batch) == NpmBasicEncodeError::kNone);
    assert(batch->GetColumnByName("interval_wire_bytes_total")->IsNull(0));
    assert(std::static_pointer_cast<arrow::UInt64Array>(batch->GetColumnByName("wire_bytes_total"))->Value(0) ==
           row.wire_bytes_total);
}
