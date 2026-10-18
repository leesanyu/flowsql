// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include "result_codec.h"
#include <arrow/util/byte_size.h>
#include <algorithm>
#include <limits>
#include <mutex>
#include <stdexcept>
#include "result_batch.h"

namespace flowsql::baseliner {
namespace {
struct EncodingError : std::runtime_error {
    arrow::Status status;
    explicit EncodingError(arrow::Status value) : std::runtime_error(value.ToString()), status(std::move(value)) {}
};
void Check(arrow::Status status) {
    if (!status.ok()) throw EncodingError(std::move(status));
}
class ResultPool final : public arrow::ProxyMemoryPool {
 public:
    explicit ResultPool(uint64_t limit)
        : arrow::ProxyMemoryPool(arrow::default_memory_pool()),
          limit_(std::min<uint64_t>(limit, std::numeric_limits<int64_t>::max())) {}
    arrow::Status Allocate(int64_t size, int64_t alignment, uint8_t** out) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (size > limit_ - bytes_allocated()) return arrow::Status::OutOfMemory("baseliner output budget exceeded");
        return arrow::ProxyMemoryPool::Allocate(size, alignment, out);
    }
    arrow::Status Reallocate(int64_t old_size, int64_t new_size, int64_t alignment, uint8_t** ptr) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (new_size > old_size) {
            if (new_size > limit_ - bytes_allocated())
                return arrow::Status::OutOfMemory("baseliner output budget exceeded during resize");
            transient_peak_ = std::max(transient_peak_, bytes_allocated() + new_size);
        }
        return arrow::ProxyMemoryPool::Reallocate(old_size, new_size, alignment, ptr);
    }
    void Free(uint8_t* ptr, int64_t size, int64_t alignment) override {
        std::lock_guard<std::mutex> lock(mutex_);
        arrow::ProxyMemoryPool::Free(ptr, size, alignment);
    }
    uint64_t Peak() const { return std::max(max_memory(), transient_peak_); }

 private:
    int64_t limit_, transient_peak_ = 0;
    std::mutex mutex_;
};
std::shared_ptr<arrow::RecordBatch> KeepPool(const std::shared_ptr<arrow::RecordBatch>& batch,
                                             const std::shared_ptr<ResultPool>& pool) {
    std::vector<std::shared_ptr<arrow::Array>> arrays;
    for (const auto& array : batch->columns()) {
        auto data = array->data()->Copy();
        for (auto& buffer : data->buffers) {
            if (!buffer) continue;
            struct Owner {
                std::shared_ptr<ResultPool> pool;
                std::shared_ptr<arrow::Buffer> buffer;
            };
            auto owner = std::make_shared<Owner>(Owner{pool, std::move(buffer)});
            auto* pointer = owner->buffer.get();
            buffer = std::shared_ptr<arrow::Buffer>(std::move(owner), pointer);
        }
        arrays.push_back(arrow::MakeArray(std::move(data)));
    }
    return arrow::RecordBatch::Make(batch->schema(), batch->num_rows(), std::move(arrays));
}
template <class T>
std::shared_ptr<T> Column(const arrow::RecordBatch& batch, const char* name) {
    return std::static_pointer_cast<T>(batch.GetColumnByName(name));
}
class Encoder {
 public:
    explicit Encoder(SchemaKind kind, arrow::MemoryPool* pool = arrow::default_memory_pool())
        : schema_(MakeSchema(kind)) {
        for (const auto& field : schema_->fields()) {
            auto made = arrow::MakeBuilder(field->type(), pool);
            if (!made.ok()) throw std::runtime_error(made.status().ToString());
            builders_.push_back(std::move(*made));
        }
    }
    void Put(const char* name, const arrow::Scalar& value) {
        auto& builder = builders_.at(schema_->GetFieldIndex(name));
        Check(reserving_ ? builder->Reserve(1) : builder->AppendScalar(value));
    }
    void Binary(const char* name, const std::string& value) {
        auto* builder = static_cast<arrow::BinaryBuilder*>(builders_.at(schema_->GetFieldIndex(name)).get());
        if (reserving_) {
            Check(builder->Reserve(1));
            Check(builder->ReserveData(value.size()));
        } else {
            Check(builder->Append(value));
        }
    }
    void Text(const char* name, const std::string& value) { Binary(name, value); }
    void Int(const char* name, int64_t value) { Put(name, arrow::Int64Scalar(value)); }
    void Null(const char* name) {
        auto& builder = builders_.at(schema_->GetFieldIndex(name));
        Check(reserving_ ? builder->Reserve(1) : builder->AppendNull());
    }
    void Number(const char* name, const std::optional<double>& value) {
        if (value)
            Put(name, arrow::DoubleScalar(*value));
        else
            Null(name);
    }
    void Boolean(const char* name, const std::optional<bool>& value) {
        if (value)
            Put(name, arrow::BooleanScalar(*value));
        else
            Null(name);
    }
    void Common(const ConfigSnapshot& config, const Observation& input, int64_t target) {
        Text("task_key", config.config.task_key);
        Text("dataset_id", input.dataset_id);
        Text("metric_id", input.metric_id);
        Binary("series_key", input.identity);
        Text("source_epoch", input.source_epoch);
        Int("target_bucket", target);
        Text("config_hash", config.sha256_hex);
        Put("published_generation", arrow::UInt64Scalar(0));
    }
    void Prepare(const ConfigSnapshot& config, const EvaluationRow& row) {
        reserving_ = true;
        try {
            Result(config, row);
        } catch (...) {
            reserving_ = false;
            throw;
        }
        reserving_ = false;
    }
    void Result(const ConfigSnapshot& config, const EvaluationRow& row) {
        auto status = ValidateResult(row.values);
        if (!status.ok()) throw std::runtime_error(status.message);
        Common(config, row.input, row.values.target_bucket);
        Text("result_kind", row.values.forecast ? "forecast" : "evaluation");
        if (row.summary_id.empty())
            Null("summary_id");
        else
            Text("summary_id", row.summary_id);
        if (row.basis_id.empty())
            Null("basis_id");
        else
            Text("basis_id", row.basis_id);
        Int("issued_after_bucket", row.values.issued_after_bucket);
        Text("model_basis_id", row.model_basis_id);
        Int("bucket_seconds", config.config.bucket_seconds);
        Text("timezone", config.config.timezone);
        Text("unit", row.unit);
        Number("observed", row.values.observed);
        Number("expected", row.values.expected);
        Number("lower", row.values.lower);
        Number("upper", row.values.upper);
        Put("status", arrow::Int32Scalar(static_cast<int32_t>(row.values.status)));
        Text("band_kind", row.band_kind);
        if (row.values.forecast) {
            for (const char* field : {"confidence", "maturity", "score", "is_outside_band", "can_score", "can_update",
                                      "update_weight", "can_alert"})
                Null(field);
        } else {
            Number("confidence", row.rolling.confidence);
            Text("maturity", row.rolling.maturity_status);
            Number("score", row.rolling.can_score ? std::optional<double>{row.rolling.z_score} : std::nullopt);
            Boolean("is_outside_band", row.rolling.is_outside_band);
            Boolean("can_score", row.rolling.can_score);
            Boolean("can_update", row.rolling.can_update);
            Number("update_weight", row.rolling.update_weight);
            Boolean("can_alert", row.values.can_alert);
        }
    }
    std::shared_ptr<arrow::RecordBatch> Finish(int64_t rows, uint64_t max_bytes) {
        std::vector<std::shared_ptr<arrow::Array>> arrays;
        for (auto& builder : builders_) {
            auto finished = builder->Finish();
            if (!finished.ok()) throw std::runtime_error(finished.status().ToString());
            arrays.push_back(std::move(*finished));
        }
        auto batch = arrow::RecordBatch::Make(schema_, rows, std::move(arrays));
        Check(batch->ValidateFull());
        if (uint64_t(arrow::util::TotalBufferSize(*batch)) > max_bytes)
            throw std::runtime_error("result batch budget exceeded");
        return batch;
    }

 private:
    bool reserving_ = false;
    std::shared_ptr<arrow::Schema> schema_;
    std::vector<std::unique_ptr<arrow::ArrayBuilder>> builders_;
};
std::shared_ptr<arrow::RecordBatch> Fusion(const ConfigSnapshot& config, const Observation& input,
                                           const RelationRollingResult& relation, uint64_t max_bytes,
                                           arrow::MemoryPool* pool) {
    Encoder encoder(SchemaKind::kRelationFusion, pool);
    encoder.Common(config, input, input.bucket);
    encoder.Put("status",
                arrow::Int32Scalar(static_cast<int32_t>(
                    relation.has_fusion_result
                        ? relation.fusion_result.status
                        : (relation.status == BaselineStatus::kOk ? BaselineStatus::kNotTrained : relation.status))));
    encoder.Number("fusion_score", relation.has_fusion_result
                                       ? std::optional<double>{relation.fusion_result.relation_risk}
                                       : std::nullopt);
    encoder.Text("basis_id", std::to_string(relation.basis_version));
    encoder.Null("can_alert");
    return encoder.Finish(1, max_bytes);
}
}  // namespace
struct ResultBatchEncoder::Impl {
    const ConfigSnapshot& config;
    uint64_t budget;
    ResultBatchLimits limits;
    std::shared_ptr<ResultPool> pool;
    Encoder encoder;
    int64_t rows = 0;
    ResultBatchStats stats;
    Impl(const ConfigSnapshot& cfg, uint64_t max_bytes, ResultBatchLimits lim)
        : config(cfg),
          budget(max_bytes),
          limits(lim),
          pool(std::make_shared<ResultPool>(max_bytes)),
          encoder(SchemaKind::kResults, pool.get()) {}
    void Flush(std::vector<std::shared_ptr<arrow::RecordBatch>>* batches) {
        if (!rows) return;
        auto batch = KeepPool(encoder.Finish(rows, budget), pool);
        stats.encoded_bytes += arrow::util::TotalBufferSize(*batch);
        ++stats.result_batches;
        batches->push_back(std::move(batch));
        rows = 0;
    }
};
ResultBatchEncoder::ResultBatchEncoder(const ConfigSnapshot& config, uint64_t max_bytes, ResultBatchLimits limits)
    : impl_(std::make_unique<Impl>(config, max_bytes, limits)) {}
ResultBatchEncoder::~ResultBatchEncoder() = default;
int ResultBatchEncoder::Append(const EvaluationRow& row, std::vector<std::shared_ptr<arrow::RecordBatch>>* batches,
                               std::string* error) {
    try {
        try {
            impl_->encoder.Prepare(impl_->config, row);
        } catch (const EncodingError& failure) {
            if (!failure.status.IsOutOfMemory() || !impl_->rows) throw;
            impl_->Flush(batches);
            impl_->encoder.Prepare(impl_->config, row);
        }
        impl_->encoder.Result(impl_->config, row);
        ++impl_->rows;
        ++impl_->stats.result_rows;
        if (impl_->rows == impl_->limits.max_rows) impl_->Flush(batches);
        return 0;
    } catch (const std::exception& failure) {
        *error = failure.what();
        return -1;
    }
}
int ResultBatchEncoder::Flush(std::vector<std::shared_ptr<arrow::RecordBatch>>* batches, std::string* error) {
    try {
        impl_->Flush(batches);
        return 0;
    } catch (const std::exception& failure) {
        *error = failure.what();
        return -1;
    }
}
int ResultBatchEncoder::AppendFusion(const Observation& input, const RelationRollingResult& relation,
                                     std::vector<std::shared_ptr<arrow::RecordBatch>>* batches, std::string* error) {
    try {
        impl_->Flush(batches);
        batches->push_back(
            KeepPool(Fusion(impl_->config, input, relation, impl_->budget, impl_->pool.get()), impl_->pool));
        return 0;
    } catch (const std::exception& failure) {
        *error = failure.what();
        return -1;
    }
}
ResultBatchStats ResultBatchEncoder::Stats() const {
    auto stats = impl_->stats;
    stats.peak_staging_bytes = impl_->pool->Peak();
    return stats;
}
int EncodeModelParameters(const ConfigSnapshot& config, const std::vector<ModelParametersRow>& rows, uint64_t max_bytes,
                          std::shared_ptr<arrow::RecordBatch>* output, std::string* error) {
    if (!output || !error) return -1;
    output->reset();
    try {
        Encoder encoder(SchemaKind::kModelParameters);
        uint64_t bytes = 0;
        for (const auto& row : rows) {
            bytes += row.parameters.parameters_json.size() + row.identity.identity.size() + row.model_basis_id.size() +
                     row.identity.source_epoch.size() + row.identity.dataset_id.size() + row.identity.metric_id.size() +
                     config.config.task_key.size() + config.sha256_hex.size() + row.parameters.maturity.size() + 128;
            if (bytes > max_bytes) throw std::runtime_error("model output budget exceeded");
            encoder.Text("task_key", config.config.task_key);
            encoder.Text("dataset_id", row.identity.dataset_id);
            encoder.Text("metric_id", row.identity.metric_id);
            encoder.Put("series_key", arrow::BinaryScalar(row.identity.identity));
            encoder.Text("source_epoch", row.identity.source_epoch);
            encoder.Text("config_hash", config.sha256_hex);
            encoder.Text("model_kind", row.identity.kind == BaselineTaskKind::kValue   ? "value"
                                       : row.identity.kind == BaselineTaskKind::kRatio ? "ratio"
                                                                                       : "relation");
            encoder.Text("model_basis_id", row.model_basis_id);
            if (row.as_of_bucket)
                encoder.Int("as_of_bucket", *row.as_of_bucket);
            else
                encoder.Null("as_of_bucket");
            encoder.Put("status", arrow::Int32Scalar(static_cast<int32_t>(row.parameters.status)));
            if (!row.parameters.maturity.empty())
                encoder.Text("maturity", row.parameters.maturity);
            else
                encoder.Null("maturity");
            encoder.Put("parameters_version", arrow::UInt32Scalar(row.parameters.parameters_version));
            if (!row.parameters.parameters_json.empty())
                encoder.Text("parameters_json", row.parameters.parameters_json);
            else
                encoder.Null("parameters_json");
        }
        *output = encoder.Finish(rows.size(), max_bytes);
        error->clear();
        return 0;
    } catch (const std::exception& e) {
        *error = e.what();
        return -1;
    }
}
int DecodeObservations(const std::shared_ptr<arrow::RecordBatch>& batch, uint64_t max_bytes,
                       std::vector<Observation>* output, std::string* error) {
    if (!output || !error) return -1;
    output->clear();
    try {
        if (!batch || !batch->schema()->Equals(*MakeSchema(SchemaKind::kObservation), true))
            throw std::runtime_error("standard observation schema required");
        Check(batch->ValidateFull());
        if (uint64_t(arrow::util::TotalBufferSize(*batch)) > max_bytes)
            throw std::runtime_error("observation input budget exceeded");
        auto dataset = Column<arrow::StringArray>(*batch, "dataset_id");
        auto metric = Column<arrow::StringArray>(*batch, "metric_id");
        auto identity = Column<arrow::BinaryArray>(*batch, "identity");
        auto epoch = Column<arrow::StringArray>(*batch, "source_epoch");
        auto kind = Column<arrow::UInt8Array>(*batch, "kind");
        auto buckets = Column<arrow::Int64Array>(*batch, "bucket_id");
        std::vector<Observation> rows;
        for (int64_t r = 0; r < batch->num_rows(); ++r) {
            for (int i = 0; i < 6; ++i)
                if (batch->column(i)->IsNull(r)) throw std::runtime_error("NULL observation identity");
            if (kind->Value(r) > uint8_t(BaselineTaskKind::kRelation))
                throw std::runtime_error("invalid observation kind");
            Observation row;
            row.dataset_id = dataset->GetString(r);
            row.metric_id = metric->GetString(r);
            row.identity = identity->GetString(r);
            row.source_epoch = epoch->GetString(r);
            row.kind = static_cast<BaselineTaskKind>(kind->Value(r));
            row.bucket = buckets->Value(r);
            auto number = [&](const char* name) -> std::optional<double> {
                auto a = Column<arrow::DoubleArray>(*batch, name);
                return a->IsNull(r) ? std::nullopt : std::optional<double>{a->Value(r)};
            };
            row.value = number("value");
            row.numerator = number("numerator");
            row.denominator = number("denominator");
            auto samples = Column<arrow::UInt64Array>(*batch, "sample_count");
            if (!samples->IsNull(r)) row.sample_count = samples->Value(r);
            auto groups = Column<arrow::ListArray>(*batch, "group_idx");
            auto metrics = Column<arrow::ListArray>(*batch, "metrics");
            if (row.kind == BaselineTaskKind::kRelation) {
                if (groups->IsNull(r) || metrics->IsNull(r)) throw std::runtime_error("NULL relation lists");
                auto values = std::static_pointer_cast<arrow::UInt32Array>(groups->values());
                for (int64_t i = groups->value_offset(r); i < groups->value_offset(r) + groups->value_length(r); ++i) {
                    if (values->IsNull(i)) throw std::runtime_error("NULL group index");
                    row.groups.push_back(values->Value(i));
                }
                auto items = std::static_pointer_cast<arrow::StructArray>(metrics->values());
                auto names = std::static_pointer_cast<arrow::StringArray>(items->field(0));
                auto totals = std::static_pointer_cast<arrow::DoubleArray>(items->field(1));
                auto counts = std::static_pointer_cast<arrow::UInt32Array>(items->field(2));
                auto list = std::static_pointer_cast<arrow::ListArray>(items->field(3));
                auto numbers = std::static_pointer_cast<arrow::DoubleArray>(list->values());
                for (int64_t i = metrics->value_offset(r); i < metrics->value_offset(r) + metrics->value_length(r);
                     ++i) {
                    if (items->IsNull(i) || names->IsNull(i) || totals->IsNull(i) || counts->IsNull(i) ||
                        list->IsNull(i))
                        throw std::runtime_error("NULL relation metric");
                    RelationValues value{names->GetString(i), totals->Value(i), counts->Value(i), {}};
                    for (int64_t j = list->value_offset(i); j < list->value_offset(i) + list->value_length(i); ++j) {
                        if (numbers->IsNull(j)) throw std::runtime_error("NULL relation mass");
                        value.values_by_group.push_back(numbers->Value(j));
                    }
                    row.metrics.push_back(std::move(value));
                }
            } else if (!groups->IsNull(r) || !metrics->IsNull(r))
                throw std::runtime_error("scalar contains relation lists");
            rows.push_back(std::move(row));
        }
        *output = std::move(rows);
        return 0;
    } catch (const std::exception& ex) {
        *error = ex.what();
        return -1;
    }
}
int EncodeResults(const ConfigSnapshot& config, const std::vector<EvaluationRow>& rows, uint64_t max_bytes,
                  std::shared_ptr<arrow::RecordBatch>* output, std::string* error) {
    if (!output || !error) return -1;
    output->reset();
    try {
        Encoder encoder(SchemaKind::kResults);
        for (const auto& row : rows) encoder.Result(config, row);
        *output = encoder.Finish(rows.size(), max_bytes);
        return 0;
    } catch (const std::exception& ex) {
        *error = ex.what();
        return -1;
    }
}
int EncodeFusion(const ConfigSnapshot& config, const Observation& input, const RelationRollingResult& relation,
                 uint64_t max_bytes, std::shared_ptr<arrow::RecordBatch>* output, std::string* error) {
    if (!output || !error) return -1;
    output->reset();
    try {
        *output = Fusion(config, input, relation, max_bytes, arrow::default_memory_pool());
        return 0;
    } catch (const std::exception& ex) {
        *error = ex.what();
        return -1;
    }
}
int EncodeMaintenance(const ConfigSnapshot& config, const std::vector<MaintenanceRow>& rows, uint64_t max_bytes,
                      std::shared_ptr<arrow::RecordBatch>* output, std::string* error) {
    if (!output || !error) return -1;
    output->reset();
    try {
        Encoder encoder(SchemaKind::kMaintenance);
        for (const auto& row : rows) {
            encoder.Common(config, row.identity, row.identity.bucket);
            encoder.Text("event_kind", row.event_kind);
            encoder.Text("reason", row.reason);
            encoder.Put("runtime_identities", arrow::UInt64Scalar(row.usage.runtime_identities));
            encoder.Put("model_identities", arrow::UInt64Scalar(row.usage.model_identities));
            encoder.Put("routed_states", arrow::UInt64Scalar(row.usage.routed_states));
            encoder.Put("retained_basis_versions", arrow::UInt64Scalar(row.usage.retained_basis_versions));
        }
        *output = encoder.Finish(rows.size(), max_bytes);
        return 0;
    } catch (const std::exception& e) {
        *error = e.what();
        return -1;
    }
}
}  // namespace flowsql::baseliner
