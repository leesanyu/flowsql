// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include "bucket_aggregator.h"

#include <arrow/util/byte_size.h>
#include <openssl/sha.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>
#include "snapshot_reader.h"

namespace flowsql::baseliner {
namespace {
using Row = std::map<std::string, std::optional<KeyValue>>;
struct NumericError : std::runtime_error {
    bool null;
    NumericError(bool n, const std::string& message) : std::runtime_error(message), null(n) {}
};
const KeyValue& Required(const Row& row, const std::string& name) {
    auto it = row.find(name);
    if (it == row.end()) throw std::runtime_error("missing field: " + name);
    if (!it->second) throw NumericError(true, "NULL field: " + name);
    return *it->second;
}
Row ReadRow(const std::shared_ptr<arrow::RecordBatch>& batch, const Dataset& d, int64_t index) {
    Row row;
    for (const auto& [name, type] : d.fields) {
        auto a = batch->GetColumnByName(name);
        if (!a || !a->type()->Equals(ArrowType(type))) throw std::runtime_error("field type mismatch: " + name);
        if (a->IsNull(index)) {
            row[name] = std::nullopt;
            continue;
        }
        switch (type) {
            case LogicalType::kBoolean:
                row[name] = std::static_pointer_cast<arrow::BooleanArray>(a)->Value(index);
                break;
            case LogicalType::kInt64:
                row[name] = std::static_pointer_cast<arrow::Int64Array>(a)->Value(index);
                break;
            case LogicalType::kUInt64:
                row[name] = std::static_pointer_cast<arrow::UInt64Array>(a)->Value(index);
                break;
            case LogicalType::kFloat64:
                row[name] = std::static_pointer_cast<arrow::DoubleArray>(a)->Value(index);
                break;
            case LogicalType::kUtf8:
                row[name] = std::static_pointer_cast<arrow::StringArray>(a)->GetString(index);
                break;
        }
    }
    return row;
}
std::vector<KeyValue> Keys(const Row& row, const std::vector<std::string>& names) {
    std::vector<KeyValue> out;
    for (const auto& name : names) out.push_back(Required(row, name));
    return out;
}
std::string Identity(const TaskConfig& c, const Dataset& d, const std::string& metric, BaselineTaskKind kind,
                     const std::vector<KeyValue>& keys) {
    std::string result;
    auto status = EncodeIdentity(c.source, d.id, metric, kind, keys, &result);
    if (!status.ok()) throw std::runtime_error("identity: " + status.message);
    return result;
}
std::string Digest(const Row& row) {
    std::string bytes;
    for (const auto& [name, cell] : row) {
        bytes.push_back(cell ? 1 : 0);
        if (!cell) continue;
        bytes.push_back(static_cast<char>(cell->index()));
        std::visit(
            [&](auto v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, std::string>) {
                    uint64_t length = v.size();
                    bytes.append(reinterpret_cast<const char*>(&length), sizeof(length));
                    bytes += v;
                } else {
                    if constexpr (std::is_same_v<T, double>)
                        if (v == 0) v = 0;
                    bytes.append(reinterpret_cast<const char*>(&v), sizeof(v));
                }
            },
            *cell);
    }
    std::string result(SHA256_DIGEST_LENGTH, '\0');
    SHA256(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(),
           reinterpret_cast<unsigned char*>(result.data()));
    return result;
}
struct Number {
    long double value = 0;
    bool integer = true;
    bool unsigned_value = true;
};
Number Input(const Row& row, const Expression& e) {
    if (e.aggregate == Aggregate::kCount) {
        if (e.column.empty()) return {1, true, true};
        return {row.at(e.column).has_value() ? 1.0L : 0.0L, true, true};
    }
    const auto& v = Required(row, e.column);
    if (auto p = std::get_if<uint64_t>(&v)) return {static_cast<long double>(*p), true, true};
    if (auto p = std::get_if<int64_t>(&v)) return {static_cast<long double>(*p), true, false};
    if (auto p = std::get_if<double>(&v)) {
        if (!std::isfinite(*p)) throw NumericError(false, "nonfinite metric: " + e.column);
        return {*p, false, false};
    }
    throw NumericError(false, "nonnumeric metric: " + e.column);
}
struct Accumulator {
    long double total = 0, minimum = 0, maximum = 0;
    uint64_t rows = 0;
    bool integer = true, unsigned_value = true;
    void Add(Number n, const Expression& e) {
        if (rows == UINT64_MAX) throw NumericError(false, "sample counter overflow");
        if (!rows) {
            minimum = maximum = n.value;
            integer = n.integer;
            unsigned_value = n.unsigned_value;
        }
        minimum = std::min(minimum, n.value);
        maximum = std::max(maximum, n.value);
        if (e.aggregate != Aggregate::kMin && e.aggregate != Aggregate::kMax) total += n.value;
        ++rows;
        if (!std::isfinite(total) ||
            (integer &&
             (total > (unsigned_value ? static_cast<long double>(UINT64_MAX) : static_cast<long double>(INT64_MAX)) ||
              total < (unsigned_value ? 0.0L : static_cast<long double>(INT64_MIN)))))
            throw NumericError(false, "aggregate overflow");
    }
    long double Value(const Expression& e) const {
        if (!rows) throw NumericError(true, "empty aggregate");
        switch (e.aggregate) {
            case Aggregate::kColumn:
                if (rows != 1) throw NumericError(false, "column requires exactly one retained row");
                return total;
            case Aggregate::kMean:
                return total / rows;
            case Aggregate::kMin:
                return minimum;
            case Aggregate::kMax:
                return maximum;
            default:
                return total;
        }
    }
    double Finish(const Expression& e) const {
        long double base = Value(e);
        if (integer && e.aggregate != Aggregate::kMean && static_cast<long double>(static_cast<double>(base)) != base)
            throw NumericError(false, "inexact integer narrowing");
        long double v = base * static_cast<long double>(e.scale) / static_cast<long double>(e.divisor);
        double result = static_cast<double>(v);
        if (!std::isfinite(result)) throw NumericError(false, "inexact or overflowing numeric narrowing");
        return result;
    }
    uint64_t Count(const Expression& e) const {
        long double v = Value(e);
        if (v < 0 || v > static_cast<long double>(UINT64_MAX) || std::floor(v) != v)
            throw NumericError(false, "sample_count overflow");
        return static_cast<uint64_t>(v);
    }
};
struct RelationState {
    std::map<uint32_t, std::vector<Accumulator>> groups;
};
struct ScalarState {
    Accumulator first, second, samples;
};
uint64_t ObservationBytes(const Observation& o) {
    uint64_t n = 256 + 2 * (o.dataset_id.size() + o.metric_id.size() + o.identity.size() + o.source_epoch.size()) +
                 8 * o.groups.size();
    for (const auto& m : o.metrics) n += 128 + 2 * m.metric.size() + 16 * m.values_by_group.size();
    return n;
}
}  // namespace
struct BucketAggregator::Impl {
    TaskConfig config;
    std::string epoch, error;
    AggregationStats stats;
    size_t dataset = 0;
    bool failed = false, done = false;
    std::optional<int64_t> bucket, source_period;
    std::map<std::string, std::string> dedup;
    std::vector<std::map<std::string, ScalarState>> states;
    std::vector<std::map<std::string, RelationState>> relations;
    std::vector<std::map<std::string, int64_t>> emitted;
    uint64_t retained = 0, input_bytes = 0, output_bytes = 0;
    Impl(TaskConfig c, std::string e) : config(std::move(c)), epoch(std::move(e)) { Reset(); }
    void Reset() {
        bucket.reset();
        source_period.reset();
        dedup.clear();
        states.clear();
        relations.clear();
        emitted.clear();
        retained = 0;
        if (dataset < config.datasets.size()) {
            states.resize(config.datasets[dataset].metrics.size());
            emitted.resize(states.size());
            relations.resize(states.size());
        }
    }
    void Budget(uint64_t extra = 0) const {
        if (extra > config.read.max_pending_bytes || retained > config.read.max_pending_bytes - extra ||
            input_bytes > config.read.max_pending_bytes - extra - retained ||
            output_bytes > config.read.max_pending_bytes - extra - retained - input_bytes)
            throw std::runtime_error("aggregation pending/output budget exceeded");
    }
    bool Skip(const Dataset& d, const Metric& m, const NumericError& ex) {
        if ((ex.null ? m.null_policy : m.invalid_policy) == InvalidPolicy::kFail) throw ex;
        ++stats.skipped[d.id + "." + m.id + (ex.null ? ":null" : ":invalid")];
        return true;
    }
    void Append(Observation o, std::vector<Observation>* out) {
        auto bytes = ObservationBytes(o);
        Budget(bytes);
        output_bytes += bytes;
        out->push_back(std::move(o));
    }
    Observation Base(const Dataset& d, const Metric& m, const std::string& key, int64_t time) const {
        Observation o;
        o.dataset_id = d.id;
        o.metric_id = m.id;
        o.kind = m.kind;
        o.identity = key;
        o.source_epoch = epoch;
        o.bucket = time;
        return o;
    }
    void Zero(const Dataset& d, const Metric& m, const std::string& key, int64_t begin, int64_t end,
              std::vector<Observation>* out) {
        for (int64_t time = begin; time < end; ++time) {
            auto o = Base(d, m, key, time);
            if (m.kind == BaselineTaskKind::kValue) {
                o.value = 0;
                o.sample_count = 0;
            } else if (m.kind == BaselineTaskKind::kRatio) {
                o.numerator = 0;
                o.denominator = 0;
            } else {
                std::set<uint32_t> groups;
                for (const auto& [value, index] : m.group_space->dictionary) groups.insert(index);
                if (m.group_space->other) groups.insert(*m.group_space->other);
                o.groups.assign(groups.begin(), groups.end());
                for (const auto& child : m.relation_metrics)
                    o.metrics.push_back({child.id, 0, 0, std::vector<double>(o.groups.size(), 0)});
            }
            Append(std::move(o), out);
        }
    }
    void Publish(Observation o, size_t i, std::vector<Observation>* out) {
        const auto& d = config.datasets[dataset];
        const auto& m = d.metrics[i];
        if (d.fill_missing_zero) {
            auto found = emitted[i].find(o.identity);
            int64_t begin = found == emitted[i].end() ? *d.scope.begin_bucket : found->second + 1;
            Zero(d, m, o.identity, begin, o.bucket, out);
            if (found == emitted[i].end()) {
                if (emitted[i].size() >= config.state.limits.max_runtime_identities)
                    throw std::runtime_error("zero-fill identity capacity exceeded");
                Budget(128 + 2 * o.identity.size());
                retained += 128 + 2 * o.identity.size();
            }
            emitted[i][o.identity] = o.bucket;
        }
        Append(std::move(o), out);
    }
    uint64_t GroupBytes(size_t children) const { return 128 + 2 * sizeof(Accumulator) * children; }
    uint32_t Group(const Row& row, const GroupSpace& space) const {
        const auto& value = Required(row, space.column);
        if (space.dictionary.empty()) {
            uint64_t index = std::get<uint64_t>(value);
            if (index > UINT32_MAX) throw std::runtime_error("group_idx narrowing overflow");
            return static_cast<uint32_t>(index);
        }
        for (const auto& [known, index] : space.dictionary)
            if (value == known) return index;
        if (space.other) return *space.other;
        throw std::runtime_error("unknown group in " + space.id + "@" + space.version);
    }
    void AddRelation(size_t i, const Row& row, const std::vector<KeyValue>& keys, bool npm) {
        const auto& d = config.datasets[dataset];
        const auto& m = d.metrics[i];
        std::string key = Identity(config, d, m.id, m.kind, keys);
        auto found = relations[i].find(key);
        uint32_t group;
        std::vector<Accumulator> next(m.relation_metrics.size());
        try {
            group = Group(row, m.group_space.value());
            if (found != relations[i].end()) {
                auto g = found->second.groups.find(group);
                if (g != found->second.groups.end()) next = g->second;
            }
            for (size_t child = 0; child < m.relation_metrics.size(); ++child) {
                const auto& e = m.relation_metrics[child].value;
                if (npm && e.aggregate != Aggregate::kCount && e.column.rfind("interval_", 0) != 0)
                    throw std::runtime_error("NPM cumulative/non-interval Relation metric rejected");
                auto number = Input(row, e);
                if (number.value < 0) throw NumericError(false, "negative Relation mass");
                next[child].Add(number, e);
            }
        } catch (const NumericError& ex) {
            Skip(d, m, ex);
            return;
        }
        if (found == relations[i].end()) {
            if (relations[i].size() >= config.state.limits.max_runtime_identities)
                throw std::runtime_error("Relation identity capacity exceeded");
            Budget(256 + 2 * key.size());
            retained += 256 + 2 * key.size();
            found = relations[i].emplace(key, RelationState{}).first;
        }
        if (!found->second.groups.count(group)) {
            if (found->second.groups.size() >= kMaxGroupDictionary)
                throw std::runtime_error("Relation group capacity exceeded");
            Budget(GroupBytes(next.size()));
            retained += GroupBytes(next.size());
        }
        found->second.groups[group] = std::move(next);
    }
    void CloseRelations(size_t i, std::vector<Observation>* out) {
        const auto& d = config.datasets[dataset];
        const auto& m = d.metrics[i];
        for (const auto& [key, state] : relations[i]) {
            auto o = Base(d, m, key, *bucket);
            for (const auto& [group, values] : state.groups) o.groups.push_back(group);
            try {
                for (size_t child = 0; child < m.relation_metrics.size(); ++child) {
                    RelationValues values;
                    values.metric = m.relation_metrics[child].id;
                    long double total = 0;
                    for (const auto& [group, acc] : state.groups) {
                        double value = acc[child].Finish(m.relation_metrics[child].value);
                        if (value < 0) throw NumericError(false, "negative scaled Relation mass");
                        values.values_by_group.push_back(value);
                        total += value;
                        if (value > 0) ++values.active_count;
                    }
                    values.total = static_cast<double>(total);
                    if (!std::isfinite(values.total)) throw NumericError(false, "Relation total overflow");
                    o.metrics.push_back(std::move(values));
                }
            } catch (const NumericError& ex) {
                Skip(d, m, ex);
                continue;
            }
            Publish(std::move(o), i, out);
        }
        for (const auto& [key, state] : relations[i])
            retained -= 256 + 2 * key.size() + state.groups.size() * GroupBytes(m.relation_metrics.size());
        relations[i].clear();
    }
    void Close(std::vector<Observation>* out) {
        if (!bucket) return;
        const auto& d = config.datasets[dataset];
        // Drop dedup summaries before building output; payloads were never retained.
        // latest_revision identity spans the finite dataset, not only the current bucket.
        if (d.row_semantics != "latest_revision") {
            for (const auto& [key, digest] : dedup) retained -= 128 + 2 * key.size();
            dedup.clear();
        }
        for (size_t i = 0; i < d.metrics.size(); ++i) {
            const auto& m = d.metrics[i];
            if (m.kind == BaselineTaskKind::kRelation) {
                CloseRelations(i, out);
                continue;
            }
            for (const auto& [key, state] : states[i]) {
                auto o = Base(d, m, key, *bucket);
                try {
                    if (m.kind == BaselineTaskKind::kValue) {
                        o.value = state.first.Finish(m.value);
                        o.sample_count = m.sample_count ? state.samples.Count(*m.sample_count) : state.first.rows;
                    } else {
                        o.numerator = state.first.Finish(m.numerator);
                        o.denominator = state.second.Finish(m.denominator);
                    }
                } catch (const NumericError& ex) {
                    Skip(d, m, ex);
                    continue;
                }
                Publish(std::move(o), i, out);
            }
            for (const auto& [key, state] : states[i]) retained -= 256 + 2 * key.size();
            states[i].clear();
        }
    }
    void Add(const Row& row, std::vector<Observation>* out) {
        const auto& d = config.datasets[dataset];
        const bool npm = d.row_semantics == "npm_period_increment";
        if (npm) {
            const auto final = std::get<bool>(Required(row, "is_final"));
            if (final && std::get<uint64_t>(Required(row, "interval_packets_ab")) == 0 &&
                std::get<uint64_t>(Required(row, "interval_packets_ba")) == 0) {
                ++stats.terminal_rows;
                return;
            }
            int64_t start = std::get<int64_t>(Required(row, "period_start_ns")),
                    end = std::get<int64_t>(Required(row, "period_end_ns"));
            __int128 delta = static_cast<__int128>(end) - start;
            int64_t width = BucketWidth(config, TimeUnit::kNs);
            if (delta <= 0 || delta > width || width % delta || start < 0 || start % delta)
                throw std::runtime_error("NPM period incompatible with integer-multiple target bucket");
            if (source_period && *source_period != delta) throw std::runtime_error("NPM source period changed");
            source_period = static_cast<int64_t>(delta);
            (void)std::get<bool>(Required(row, "period_complete"));
        }
        for (const auto& predicate : d.filter) {
            const auto& cell = row.at(predicate.column);
            if (!cell) return;
            const auto& rhs = predicate.value;
            bool equal = *cell == rhs, less = *cell < rhs;
            if ((predicate.op == "eq" && !equal) || (predicate.op == "ne" && equal) ||
                (predicate.op == "lt" && !less) || (predicate.op == "le" && !less && !equal) ||
                (predicate.op == "gt" && (less || equal)) || (predicate.op == "ge" && less))
                return;
        }
        int64_t time = std::get<int64_t>(Required(row, d.bucket_column));
        int64_t target = ToBucket(time, BucketWidth(config, d.unit));
        if (d.scope.begin_bucket && (target < *d.scope.begin_bucket || target >= *d.scope.end_bucket))
            throw std::runtime_error("row outside frozen bucket range");
        if (bucket && target < *bucket) throw std::runtime_error("late/descending target bucket");
        if (bucket && target > *bucket) Close(out);
        bucket = target;
        auto fact = Identity(config, d, "__dedup", BaselineTaskKind::kValue, Keys(row, d.deduplicate_keys));
        auto digest = Digest(row);
        auto found = dedup.find(fact);
        if (found != dedup.end()) {
            if (found->second != digest) throw std::runtime_error("conflicting duplicate/max revision");
            ++stats.duplicate_rows;
            return;
        }
        Budget(128 + 2 * fact.size());
        retained += 128 + 2 * fact.size();
        dedup.emplace(std::move(fact), std::move(digest));
        auto keys = Keys(row, d.series_keys);
        for (size_t i = 0; i < d.metrics.size(); ++i) {
            const auto& m = d.metrics[i];
            if (m.kind == BaselineTaskKind::kRelation) {
                AddRelation(i, row, keys, npm);
                continue;
            }
            auto key = Identity(config, d, m.id, m.kind, keys);
            ScalarState next;
            auto state = states[i].find(key);
            if (state != states[i].end()) next = state->second;
            auto validate_npm = [&](const Expression& e) {
                if (npm && e.aggregate != Aggregate::kCount && e.column.rfind("interval_", 0) != 0)
                    throw std::runtime_error("NPM cumulative/non-interval metric rejected");
            };
            try {
                if (m.kind == BaselineTaskKind::kValue) {
                    validate_npm(m.value);
                    auto n = Input(row, m.value);
                    std::optional<Number> sample;
                    if (m.sample_count) {
                        validate_npm(*m.sample_count);
                        sample = Input(row, *m.sample_count);
                    }
                    next.first.Add(n, m.value);
                    if (sample) next.samples.Add(*sample, *m.sample_count);
                } else {
                    validate_npm(m.numerator);
                    validate_npm(m.denominator);
                    auto n = Input(row, m.numerator), v = Input(row, m.denominator);
                    next.first.Add(n, m.numerator);
                    next.second.Add(v, m.denominator);
                }
            } catch (const NumericError& ex) {
                Skip(d, m, ex);
                continue;
            }
            if (state == states[i].end()) {
                if (states[i].size() >= config.state.limits.max_runtime_identities)
                    throw std::runtime_error("identity capacity exceeded");
                Budget(256 + 2 * key.size());
                retained += 256 + 2 * key.size();
            }
            states[i][std::move(key)] = next;
        }
    }
    int Run(size_t index, const std::shared_ptr<arrow::RecordBatch>& batch, bool finish,
            std::vector<Observation>* out) {
        if (!out) return -1;
        out->clear();
        output_bytes = 0;
        input_bytes = 0;
        if (failed || done || index != dataset) {
            error = "invalid/finished aggregation dataset";
            failed = true;
            return -1;
        }
        try {
            const auto& d = config.datasets[dataset];
            if (d.fill_missing_zero &&
                (config.mode != Mode::kSnapshot || !d.scope.begin_bucket || d.scope.consistency.empty()))
                throw std::runtime_error("zero fill requires finite complete snapshot range");
            if (finish) {
                Close(out);
                if (d.fill_missing_zero)
                    for (size_t i = 0; i < d.metrics.size(); ++i)
                        for (const auto& [key, last] : emitted[i])
                            Zero(d, d.metrics[i], key, last + 1, *d.scope.end_bucket, out);
                ++dataset;
                done = dataset == config.datasets.size();
                Reset();
            } else {
                if (!batch || !batch->ValidateFull().ok()) throw std::runtime_error("invalid raw batch");
                input_bytes = arrow::util::TotalBufferSize(*batch);
                Budget();
                for (int64_t row = 0; row < batch->num_rows(); ++row) Add(ReadRow(batch, d, row), out);
            }
            input_bytes = 0;
            return 0;
        } catch (const std::exception& ex) {
            error = ex.what();
            failed = true;
            out->clear();
            input_bytes = 0;
            return -1;
        }
    }
};
BucketAggregator::BucketAggregator(TaskConfig c, std::string epoch)
    : impl_(std::make_unique<Impl>(std::move(c), std::move(epoch))) {}
BucketAggregator::~BucketAggregator() = default;
int BucketAggregator::Push(size_t d, const std::shared_ptr<arrow::RecordBatch>& batch, std::vector<Observation>* out) {
    return impl_->Run(d, batch, false, out);
}
int BucketAggregator::FinishDataset(size_t d, std::vector<Observation>* out) {
    return impl_->Run(d, nullptr, true, out);
}
const std::string& BucketAggregator::LastError() const { return impl_->error; }
const AggregationStats& BucketAggregator::Stats() const { return impl_->stats; }

int MakeObservationBatch(const std::vector<Observation>& rows, uint64_t max_bytes,
                         std::shared_ptr<arrow::RecordBatch>* output, std::string* error) {
    if (!output || !error) return -1;
    output->reset();
    try {
        auto schema = MakeSchema(SchemaKind::kObservation);
        std::vector<std::unique_ptr<arrow::ArrayBuilder>> builders;
        for (const auto& field : schema->fields()) {
            auto b = arrow::MakeBuilder(field->type());
            if (!b.ok()) throw std::runtime_error(b.status().ToString());
            builders.push_back(std::move(*b));
        }
        auto check = [](arrow::Status s) {
            if (!s.ok()) throw std::runtime_error(s.ToString());
        };
        uint64_t charged = 0;
        for (const auto& row : rows) {
            auto bytes = ObservationBytes(row);
            if (bytes > max_bytes || charged > max_bytes - bytes)
                throw std::runtime_error("observation batch budget exceeded");
            charged += bytes;
            check(static_cast<arrow::StringBuilder*>(builders[0].get())->Append(row.dataset_id));
            check(static_cast<arrow::StringBuilder*>(builders[1].get())->Append(row.metric_id));
            check(static_cast<arrow::UInt8Builder*>(builders[2].get())->Append(static_cast<uint8_t>(row.kind)));
            check(static_cast<arrow::BinaryBuilder*>(builders[3].get())->Append(row.identity));
            check(static_cast<arrow::StringBuilder*>(builders[4].get())->Append(row.source_epoch));
            check(static_cast<arrow::Int64Builder*>(builders[5].get())->Append(row.bucket));
            auto number = [&](size_t index, std::optional<double> v) {
                auto* b = static_cast<arrow::DoubleBuilder*>(builders[index].get());
                check(v ? b->Append(*v) : b->AppendNull());
            };
            number(6, row.value);
            auto* samples = static_cast<arrow::UInt64Builder*>(builders[7].get());
            check(row.sample_count ? samples->Append(*row.sample_count) : samples->AppendNull());
            number(8, row.numerator);
            number(9, row.denominator);
            auto* groups = static_cast<arrow::ListBuilder*>(builders[10].get());
            auto* metrics = static_cast<arrow::ListBuilder*>(builders[11].get());
            if (row.kind != BaselineTaskKind::kRelation) {
                check(groups->AppendNull());
                check(metrics->AppendNull());
                continue;
            }
            check(groups->Append());
            auto* indices = static_cast<arrow::UInt32Builder*>(groups->value_builder());
            for (auto g : row.groups) check(indices->Append(g));
            check(metrics->Append());
            auto* items = static_cast<arrow::StructBuilder*>(metrics->value_builder());
            for (const auto& m : row.metrics) {
                check(items->Append());
                check(static_cast<arrow::StringBuilder*>(items->field_builder(0))->Append(m.metric));
                check(static_cast<arrow::DoubleBuilder*>(items->field_builder(1))->Append(m.total));
                check(static_cast<arrow::UInt32Builder*>(items->field_builder(2))->Append(m.active_count));
                auto* values = static_cast<arrow::ListBuilder*>(items->field_builder(3));
                check(values->Append());
                auto* numbers = static_cast<arrow::DoubleBuilder*>(values->value_builder());
                for (double v : m.values_by_group) check(numbers->Append(v));
            }
        }
        std::vector<std::shared_ptr<arrow::Array>> arrays;
        for (auto& b : builders) {
            auto a = b->Finish();
            if (!a.ok()) throw std::runtime_error(a.status().ToString());
            arrays.push_back(*a);
        }
        auto batch = arrow::RecordBatch::Make(schema, rows.size(), arrays);
        check(batch->ValidateFull());
        if (static_cast<uint64_t>(arrow::util::TotalBufferSize(*batch)) > max_bytes)
            throw std::runtime_error("encoded batch budget exceeded");
        *output = std::move(batch);
        return 0;
    } catch (const std::exception& ex) {
        *error = ex.what();
        return -1;
    }
}
}  // namespace flowsql::baseliner
