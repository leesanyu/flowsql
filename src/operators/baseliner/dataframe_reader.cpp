// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include "dataframe_reader.h"
#include <arrow/compute/api.h>
#include <arrow/ipc/writer.h>
#include <arrow/util/byte_size.h>
#include <framework/interfaces/idataframe_channel.h>
#include <openssl/evp.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <stdexcept>
namespace flowsql::baseliner {
namespace {
void Check(arrow::Status status) {
    if (!status.ok()) throw std::runtime_error(status.ToString());
}
template <typename T>
T Value(arrow::Result<T> result) {
    Check(result.status());
    return std::move(*result);
}
class BudgetPool final : public arrow::ProxyMemoryPool {
 public:
    explicit BudgetPool(int64_t limit) : arrow::ProxyMemoryPool(arrow::default_memory_pool()), limit_(limit) {}
    arrow::Status Allocate(int64_t size, int64_t alignment, uint8_t** output) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (size > limit_ - bytes_allocated()) return arrow::Status::OutOfMemory("DataFrame reader budget exceeded");
        return arrow::ProxyMemoryPool::Allocate(size, alignment, output);
    }
    arrow::Status Reallocate(int64_t old_size, int64_t new_size, int64_t alignment, uint8_t** ptr) override {
        std::lock_guard<std::mutex> lock(mutex_);
        // A growing allocation may temporarily retain the old buffer while copying the new one.
        if (new_size > old_size && new_size > limit_ - bytes_allocated())
            return arrow::Status::OutOfMemory("DataFrame reader budget exceeded during resize");
        if (new_size > old_size) transient_peak_ = std::max(transient_peak_, bytes_allocated() + new_size);
        return arrow::ProxyMemoryPool::Reallocate(old_size, new_size, alignment, ptr);
    }
    void Free(uint8_t* ptr, int64_t size, int64_t alignment) override {
        std::lock_guard<std::mutex> lock(mutex_);
        arrow::ProxyMemoryPool::Free(ptr, size, alignment);
    }
    int64_t Peak() const { return std::max(max_memory(), transient_peak_); }

 private:
    int64_t limit_;
    int64_t transient_peak_ = 0;
    std::mutex mutex_;
};
// Attach the pool to every buffer, so even an extracted column outlives its reader safely.
std::shared_ptr<arrow::Buffer> KeepPool(std::shared_ptr<arrow::Buffer> buffer,
                                        const std::shared_ptr<BudgetPool>& pool) {
    if (!buffer) return nullptr;
    struct Owner {
        std::shared_ptr<BudgetPool> pool;
        std::shared_ptr<arrow::Buffer> buffer;
    };
    auto owner = std::make_shared<Owner>(Owner{pool, std::move(buffer)});
    auto* ptr = owner->buffer.get();
    return std::shared_ptr<arrow::Buffer>(std::move(owner), ptr);
}
std::shared_ptr<arrow::Array> KeepPool(const std::shared_ptr<arrow::Array>& array,
                                       const std::shared_ptr<BudgetPool>& pool) {
    auto data = array->data()->Copy();
    for (auto& buffer : data->buffers) buffer = KeepPool(std::move(buffer), pool);
    return arrow::MakeArray(std::move(data));
}
class Hash {
 public:
    Hash() : context_(EVP_MD_CTX_new(), EVP_MD_CTX_free) {
        if (!context_ || EVP_DigestInit_ex(context_.get(), EVP_sha256(), nullptr) != 1)
            throw std::runtime_error("snapshot hash initialization failed");
    }
    void Bytes(const void* data, size_t size) {
        if (EVP_DigestUpdate(context_.get(), data, size) != 1) throw std::runtime_error("snapshot hash update failed");
    }
    void Number(uint64_t value) {
        uint8_t bytes[8];
        for (size_t i = 0; i < 8; ++i) bytes[i] = value >> (8 * i);
        Bytes(bytes, sizeof(bytes));
    }
    void Text(std::string_view text) {
        Number(text.size());
        Bytes(text.data(), text.size());
    }
    std::string Finish() {
        unsigned char bytes[EVP_MAX_MD_SIZE];
        unsigned length = 0;
        if (EVP_DigestFinal_ex(context_.get(), bytes, &length) != 1 || length != 32)
            throw std::runtime_error("snapshot hash finalization failed");
        std::string output = "dataframe_snapshot_v1:";
        for (unsigned i = 0; i < length; ++i) {
            output += "0123456789abcdef"[bytes[i] >> 4];
            output += "0123456789abcdef"[bytes[i] & 15];
        }
        return output;
    }

 private:
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context_;
};
template <typename Array>
void HashNumber(Hash& hash, const arrow::Array& array, int64_t row) {
    auto value = static_cast<const Array&>(array).Value(row);
    if constexpr (std::is_floating_point_v<decltype(value)>) {
        using Bits = std::conditional_t<sizeof(value) == 4, uint32_t, uint64_t>;
        Bits bits;
        std::memcpy(&bits, &value, sizeof(value));
        hash.Number(bits);
    } else
        hash.Number(static_cast<uint64_t>(value));
}
void HashCell(Hash& hash, const arrow::Array& array, int64_t row) {
    hash.Number(!array.IsNull(row));
    if (array.IsNull(row)) return;
    switch (array.type_id()) {
        case arrow::Type::BOOL:
            return HashNumber<arrow::BooleanArray>(hash, array, row);
        case arrow::Type::INT8:
            return HashNumber<arrow::Int8Array>(hash, array, row);
        case arrow::Type::INT16:
            return HashNumber<arrow::Int16Array>(hash, array, row);
        case arrow::Type::INT32:
            return HashNumber<arrow::Int32Array>(hash, array, row);
        case arrow::Type::INT64:
            return HashNumber<arrow::Int64Array>(hash, array, row);
        case arrow::Type::UINT8:
            return HashNumber<arrow::UInt8Array>(hash, array, row);
        case arrow::Type::UINT16:
            return HashNumber<arrow::UInt16Array>(hash, array, row);
        case arrow::Type::UINT32:
            return HashNumber<arrow::UInt32Array>(hash, array, row);
        case arrow::Type::UINT64:
            return HashNumber<arrow::UInt64Array>(hash, array, row);
        case arrow::Type::FLOAT:
            return HashNumber<arrow::FloatArray>(hash, array, row);
        case arrow::Type::DOUBLE:
            return HashNumber<arrow::DoubleArray>(hash, array, row);
        case arrow::Type::DATE32:
            return HashNumber<arrow::Date32Array>(hash, array, row);
        case arrow::Type::DATE64:
            return HashNumber<arrow::Date64Array>(hash, array, row);
        case arrow::Type::TIME32:
            return HashNumber<arrow::Time32Array>(hash, array, row);
        case arrow::Type::TIME64:
            return HashNumber<arrow::Time64Array>(hash, array, row);
        case arrow::Type::TIMESTAMP:
            return HashNumber<arrow::TimestampArray>(hash, array, row);
        case arrow::Type::DURATION:
            return HashNumber<arrow::DurationArray>(hash, array, row);
        case arrow::Type::STRING:
            return hash.Text(static_cast<const arrow::StringArray&>(array).GetView(row));
        case arrow::Type::BINARY:
            return hash.Text(static_cast<const arrow::BinaryArray&>(array).GetView(row));
        case arrow::Type::LARGE_STRING:
            return hash.Text(static_cast<const arrow::LargeStringArray&>(array).GetView(row));
        case arrow::Type::LARGE_BINARY:
            return hash.Text(static_cast<const arrow::LargeBinaryArray&>(array).GetView(row));
        default:
            throw std::runtime_error("unsupported DataFrame snapshot column type: " + array.type()->ToString());
    }
}
bool Integer(arrow::Type::type type) { return type >= arrow::Type::UINT8 && type <= arrow::Type::INT64; }
std::shared_ptr<arrow::Array> Convert(const std::shared_ptr<arrow::Array>& array, LogicalType logical,
                                      arrow::compute::ExecContext* context) {
    auto target = ArrowType(logical);
    if (array->type()->Equals(target)) return array;
    if (array->length() == 0) return Value(arrow::MakeArrayOfNull(target, 0, context->memory_pool()));
    if (((logical == LogicalType::kInt64 || logical == LogicalType::kUInt64) && Integer(array->type_id())) ||
        (logical == LogicalType::kFloat64 && array->type_id() == arrow::Type::FLOAT))
        return Value(arrow::compute::Cast(*array, target, arrow::compute::CastOptions::Safe(), context));
    throw std::runtime_error("unsupported DataFrame conversion: " + array->type()->ToString() + " to " +
                             target->ToString());
}
template <typename T>
int CompareValues(const T& first, const T& second) {
    return first < second ? -1 : first > second ? 1 : 0;
}
int CompareRows(const arrow::Array& array, int64_t first, int64_t second) {
    switch (array.type_id()) {
        case arrow::Type::BOOL: {
            const auto& a = static_cast<const arrow::BooleanArray&>(array);
            return CompareValues(a.Value(first), a.Value(second));
        }
        case arrow::Type::INT64: {
            const auto& a = static_cast<const arrow::Int64Array&>(array);
            return CompareValues(a.Value(first), a.Value(second));
        }
        case arrow::Type::UINT64: {
            const auto& a = static_cast<const arrow::UInt64Array&>(array);
            return CompareValues(a.Value(first), a.Value(second));
        }
        case arrow::Type::DOUBLE: {
            const auto& a = static_cast<const arrow::DoubleArray&>(array);
            return CompareValues(a.Value(first), a.Value(second));
        }
        case arrow::Type::STRING: {
            const auto& a = static_cast<const arrow::StringArray&>(array);
            return CompareValues(a.GetView(first), a.GetView(second));
        }
        default:
            throw std::runtime_error("unsupported sort key");
    }
}
bool Matches(const arrow::Array& array, int64_t row, const Predicate& predicate) {
    if (array.IsNull(row)) return false;
    int compare = std::visit(
        [&](const auto& rhs) {
            using T = std::decay_t<decltype(rhs)>;
            if constexpr (std::is_same_v<T, bool>)
                return CompareValues(static_cast<const arrow::BooleanArray&>(array).Value(row), rhs);
            else if constexpr (std::is_same_v<T, int64_t>)
                return CompareValues(static_cast<const arrow::Int64Array&>(array).Value(row), rhs);
            else if constexpr (std::is_same_v<T, uint64_t>)
                return CompareValues(static_cast<const arrow::UInt64Array&>(array).Value(row), rhs);
            else if constexpr (std::is_same_v<T, double>) {
                double value = static_cast<const arrow::DoubleArray&>(array).Value(row);
                if (!std::isfinite(value)) throw std::runtime_error("nonfinite filter value");
                return CompareValues(value, rhs);
            } else
                return CompareValues(static_cast<const arrow::StringArray&>(array).GetView(row), std::string_view(rhs));
        },
        predicate.value);
    if (predicate.op == "eq") return compare == 0;
    if (predicate.op == "ne") return compare != 0;
    if (predicate.op == "lt") return compare < 0;
    if (predicate.op == "le") return compare <= 0;
    if (predicate.op == "gt") return compare > 0;
    if (predicate.op == "ge") return compare >= 0;
    throw std::runtime_error("unsupported filter operator");
}
}  // namespace
struct DataFrameReader::Impl {
    TaskConfig config;
    std::string error;
    std::string fingerprint;
    std::atomic<bool> cancelled{false};
    bool attempted = false;
    bool opened = false;
    bool failed = false;
    std::shared_ptr<BudgetPool> pool;
    std::shared_ptr<IDataFrameChannel> source;
    std::shared_ptr<arrow::RecordBatch> snapshot;
    std::shared_ptr<arrow::RecordBatch> normalized;
    std::shared_ptr<arrow::Buffer> index;
    uint64_t source_bytes = 0;
    int64_t selected = 0;
    int64_t position = 0;
    void CheckCancelled() const {
        if (cancelled) throw std::runtime_error("DataFrame reader cancelled");
    }
    int Fail(const std::exception& ex) {
        failed = true;
        error = ex.what();
        return -1;
    }
    std::string Fingerprint(const arrow::RecordBatch& batch) const {
        Hash hash;
        hash.Text("flowsql.dataframe_snapshot_v1");
        {
            auto schema = Value(arrow::ipc::SerializeSchema(*batch.schema(), pool.get()));
            hash.Number(schema->size());
            hash.Bytes(schema->data(), schema->size());
        }
        hash.Number(batch.num_rows());
        for (const auto& column : batch.columns())
            for (int64_t row = 0; row < batch.num_rows(); ++row) {
                CheckCancelled();
                HashCell(hash, *column, row);
            }
        return hash.Finish();
    }
    void SelectAndSort() {
        const auto& d = config.datasets[0];
        const int64_t width = BucketWidth(config, d.unit);
        std::vector<std::shared_ptr<arrow::Array>> keys{normalized->GetColumnByName(d.bucket_column)};
        for (const auto& key : d.deduplicate_keys) keys.push_back(normalized->GetColumnByName(key));
        if (!d.revision_column.empty()) keys.push_back(normalized->GetColumnByName(d.revision_column));
        int64_t* rows = reinterpret_cast<int64_t*>(index->mutable_data());
        for (int64_t row = 0; row < normalized->num_rows(); ++row) {
            CheckCancelled();
            bool matches = true;
            for (const auto& f : d.filter)
                if (!Matches(*normalized->GetColumnByName(f.column), row, f)) {
                    matches = false;
                    break;
                }
            if (!matches) continue;
            if (keys[0]->IsNull(row)) throw std::runtime_error("NULL time key");
            int64_t bucket = ToBucket(static_cast<const arrow::Int64Array&>(*keys[0]).Value(row), width);
            if (d.scope.begin_bucket && (bucket < *d.scope.begin_bucket || bucket >= *d.scope.end_bucket)) continue;
            for (const auto& key : keys) {
                if (key->IsNull(row)) throw std::runtime_error("NULL stable/revision key");
                if (key->type_id() == arrow::Type::DOUBLE &&
                    !std::isfinite(static_cast<const arrow::DoubleArray&>(*key).Value(row)))
                    throw std::runtime_error("nonfinite stable key");
            }
            rows[selected++] = row;
        }
        if (d.row_semantics == "latest_revision") {
            auto compare_keys = [&](int64_t a, int64_t b) {
                for (size_t i = 1; i <= d.deduplicate_keys.size(); ++i)
                    if (int compare = CompareRows(*keys[i], a, b)) return compare;
                return 0;
            };
            std::sort(rows, rows + selected, [&](int64_t a, int64_t b) {
                CheckCancelled();
                if (int compare = compare_keys(a, b)) return compare < 0;
                if (int compare = CompareRows(*keys.back(), a, b)) return compare > 0;
                return a < b;
            });
            int64_t retained = 0;
            for (int64_t begin = 0; begin < selected;) {
                CheckCancelled();
                int64_t end = begin + 1;
                while (end < selected && compare_keys(rows[begin], rows[end]) == 0) ++end;
                int64_t maximum = begin + 1;
                while (maximum < end && CompareRows(*keys.back(), rows[begin], rows[maximum]) == 0) ++maximum;
                for (int64_t i = begin; i < maximum; ++i) rows[retained++] = rows[i];
                begin = end;
            }
            selected = retained;
        }
        std::sort(rows, rows + selected, [&](int64_t a, int64_t b) {
            CheckCancelled();
            for (const auto& key : keys)
                if (int compare = CompareRows(*key, a, b)) return compare < 0;
            return a < b;  // Original order breaks equal-key ties without mutating the snapshot.
        });
    }
};
DataFrameReader::DataFrameReader() : impl_(std::make_unique<Impl>()) {}
DataFrameReader::~DataFrameReader() = default;
int DataFrameReader::Open(TaskConfig config, const BlockDataFrameInputBindingV1& binding,
                          std::shared_ptr<arrow::RecordBatch> snapshot) {
    auto& p = *impl_;
    try {
        p.CheckCancelled();
        if (p.attempted) throw std::runtime_error("DataFrame reader already initialized");
        p.attempted = true;
        if (!ValidBlockDataFrameInputBindingV1(binding) || !config.dataframe_source || config.database_source ||
            config.mode != Mode::kSnapshot || config.datasets.size() != 1 || config.source != binding.exact_source ||
            std::string(binding.source->Category()) != "dataframe" ||
            config.source != std::string(binding.source->Category()) + "." + binding.source->Name() ||
            std::string(binding.source->Type()) != ChannelType::kDataFrame || !snapshot)
            throw std::runtime_error("invalid DataFrame snapshot source");
        auto& d = config.datasets[0];
        if (!d.table.empty() || !d.schema.empty() || d.scope.consistency != "dataframe_snapshot" ||
            !d.scope.run_ids.empty() || !config.read.page_rows || !config.read.max_pending_bytes ||
            config.read.max_pending_bytes > kMaxBufferBytes)
            throw std::runtime_error("invalid DataFrame snapshot config");
        if (d.row_semantics == "npm_period_increment") {
            for (const auto& [name, type] :
                 std::map<std::string, LogicalType>{{"period_start_ns", LogicalType::kInt64},
                                                    {"period_end_ns", LogicalType::kInt64},
                                                    {"period_complete", LogicalType::kBoolean},
                                                    {"is_final", LogicalType::kBoolean},
                                                    {"interval_packets_ab", LogicalType::kUInt64},
                                                    {"interval_packets_ba", LogicalType::kUInt64}}) {
                auto [found, inserted] = d.fields.emplace(name, type);
                if (!inserted && found->second != type) throw std::runtime_error("NPM canonical field type: " + name);
            }
            if (d.bucket_column != "period_start_ns" || d.unit != TimeUnit::kNs)
                throw std::runtime_error("NPM increments require period_start_ns in ns");
        }
        Check(snapshot->ValidateFull());
        p.source_bytes = arrow::util::TotalBufferSize(*snapshot);
        if (p.source_bytes > config.read.max_pending_bytes ||
            uint64_t(snapshot->num_rows()) > (config.read.max_pending_bytes - p.source_bytes) / sizeof(int64_t))
            throw std::runtime_error("DataFrame snapshot/index budget exceeded");
        p.pool = std::make_shared<BudgetPool>(config.read.max_pending_bytes - p.source_bytes);
        p.index = KeepPool(Value(arrow::AllocateBuffer(snapshot->num_rows() * sizeof(int64_t), p.pool.get())), p.pool);
        arrow::compute::ExecContext context(p.pool.get());
        std::vector<std::shared_ptr<arrow::Field>> fields;
        std::vector<std::shared_ptr<arrow::Array>> arrays;
        for (const auto& [name, type] : d.fields) {
            p.CheckCancelled();
            const auto positions = snapshot->schema()->GetAllFieldIndices(name);
            if (positions.size() != 1) throw std::runtime_error("missing/ambiguous DataFrame field: " + name);
            arrays.push_back(KeepPool(Convert(snapshot->column(positions[0]), type, &context), p.pool));
            fields.push_back(arrow::field(name, ArrowType(type)));
        }
        p.normalized = arrow::RecordBatch::Make(arrow::schema(fields), snapshot->num_rows(), std::move(arrays));
        p.config = std::move(config);
        p.SelectAndSort();
        auto fingerprint = p.Fingerprint(*snapshot);
        p.CheckCancelled();
        p.source = binding.source;
        p.snapshot = std::move(snapshot);
        p.fingerprint = std::move(fingerprint);
        p.opened = true;
        return 0;
    } catch (const std::exception& ex) {
        p.normalized.reset();
        p.index.reset();
        return p.Fail(ex);
    }
}
int DataFrameReader::Next(SnapshotPage* output) {
    if (!output) return -1;
    *output = {};
    auto& p = *impl_;
    try {
        p.CheckCancelled();
        if (!p.opened || p.failed) throw std::runtime_error(p.error.empty() ? "DataFrame reader not open" : p.error);
        if (p.position == p.selected) {
            output->eof = true;
            return 0;
        }
        int64_t count = std::min<int64_t>(p.config.read.page_rows, p.selected - p.position);
        auto indices =
            arrow::MakeArray(arrow::ArrayData::Make(arrow::int64(), count, {nullptr, p.index}, 0, p.position));
        arrow::compute::ExecContext context(p.pool.get());
        std::vector<std::shared_ptr<arrow::Array>> columns;
        for (const auto& column : p.normalized->columns()) {
            p.CheckCancelled();
            auto taken =
                Value(arrow::compute::Take(*column, *indices, arrow::compute::TakeOptions::NoBoundsCheck(), &context));
            columns.push_back(KeepPool(taken, p.pool));
        }
        auto batch = arrow::RecordBatch::Make(p.normalized->schema(), count, std::move(columns));
        Check(batch->ValidateFull());
        p.CheckCancelled();
        output->batch = std::move(batch);
        p.position += count;
        return 0;
    } catch (const std::exception& ex) {
        return p.Fail(ex);
    }
}
void DataFrameReader::Cancel() { impl_->cancelled = true; }
const std::string& DataFrameReader::LastError() const { return impl_->error; }
const std::string& DataFrameReader::SourceFingerprint() const { return impl_->fingerprint; }
const TaskConfig& DataFrameReader::Config() const { return impl_->config; }
DataFrameReadStats DataFrameReader::Stats() const {
    return {impl_->source_bytes, impl_->source_bytes + uint64_t(impl_->pool ? impl_->pool->Peak() : 0),
            uint64_t(impl_->selected)};
}
}  // namespace flowsql::baseliner
