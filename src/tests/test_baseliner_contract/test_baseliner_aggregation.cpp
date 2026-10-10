// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <arrow/api.h>
#include <operators/baseliner/bucket_aggregator.h>
#include <operators/baseliner/snapshot_reader.h>
#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
using namespace flowsql;
using namespace flowsql::baseliner;
namespace {
TaskConfig Config() {
    TaskConfig c;
    c.source = "sqlite.source";
    c.task_key = "aggregate";
    Dataset d;
    d.id = "one";
    d.table = "facts";
    d.fields = {{"bucket", LogicalType::kInt64}, {"id", LogicalType::kUInt64},  {"n", LogicalType::kFloat64},
                {"d", LogicalType::kFloat64},    {"rev", LogicalType::kUInt64}, {"series", LogicalType::kUtf8}};
    d.scope.begin_bucket = 0;
    d.scope.end_bucket = 4;
    d.scope.consistency = "consistent_snapshot";
    d.bucket_column = "bucket";
    d.deduplicate_keys = {"id"};
    d.series_keys = {"series"};
    Metric value;
    value.id = "value";
    value.value = {"n", Aggregate::kSum};
    Metric ratio;
    ratio.id = "ratio";
    ratio.kind = BaselineTaskKind::kRatio;
    ratio.numerator = {"n", Aggregate::kSum};
    ratio.denominator = {"d", Aggregate::kSum};
    d.metrics = {value, ratio};
    c.datasets = {d};
    return c;
}
struct Row {
    int64_t bucket;
    uint64_t id;
    std::optional<double> n;
    double d;
    uint64_t rev = 1;
    std::string series = "A";
};
std::shared_ptr<arrow::RecordBatch> Batch(const std::vector<Row>& rows) {
    arrow::Int64Builder bucket;
    arrow::UInt64Builder id, rev;
    arrow::DoubleBuilder n, d;
    arrow::StringBuilder series;
    for (const auto& r : rows) {
        assert(bucket.Append(r.bucket).ok());
        assert(id.Append(r.id).ok());
        assert(rev.Append(r.rev).ok());
        assert((r.n ? n.Append(*r.n) : n.AppendNull()).ok());
        assert(d.Append(r.d).ok());
        assert(series.Append(r.series).ok());
    }
    std::vector<std::shared_ptr<arrow::Array>> arrays;
    for (arrow::ArrayBuilder* b : std::vector<arrow::ArrayBuilder*>{&bucket, &id, &n, &d, &rev, &series}) {
        auto a = b->Finish();
        assert(a.ok());
        arrays.push_back(*a);
    }
    auto schema = arrow::schema({arrow::field("bucket", arrow::int64()), arrow::field("id", arrow::uint64()),
                                 arrow::field("n", arrow::float64()), arrow::field("d", arrow::float64()),
                                 arrow::field("rev", arrow::uint64()), arrow::field("series", arrow::utf8())});
    return arrow::RecordBatch::Make(schema, rows.size(), arrays);
}
void Ok(int rc, const BucketAggregator& a) {
    if (rc) std::cerr << a.LastError() << '\n';
    assert(rc == 0);
}
void Core() {
    auto c = Config();
    BucketAggregator a(c, "epoch");
    std::vector<Observation> out;
    Ok(a.Push(0, Batch({{0, 1, 10, 1}}), &out), a);
    assert(out.empty());
    Ok(a.Push(0, Batch({{0, 2, 20, 9}, {0, 2, 20, 9}, {2, 3, 5, 2}}), &out), a);
    assert(out.size() == 2 && out[0].bucket == 0 && out[0].value == 30 && out[0].sample_count == 2);
    assert(out[1].numerator == 30 && out[1].denominator == 10 && !out[1].value);
    assert(a.Stats().duplicate_rows == 1);
    out.clear();
    Ok(a.FinishDataset(0, &out), a);
    assert(out.size() == 2 && out[0].bucket == 2);
    std::shared_ptr<arrow::RecordBatch> encoded;
    std::string error;
    assert(MakeObservationBatch(out, 16384, &encoded, &error) == 0);
    assert(encoded->schema()->Equals(*MakeSchema(SchemaKind::kObservation), true));
    assert(encoded->ValidateFull().ok());
}
void Failures() {
    auto c = Config();
    std::vector<Observation> out;
    BucketAggregator duplicate(c, "e");
    assert(duplicate.Push(0, Batch({{0, 1, 1, 1}, {0, 1, 2, 1}}), &out) != 0 && out.empty());
    BucketAggregator late(c, "e");
    assert(late.Push(0, Batch({{1, 1, 1, 1}, {0, 2, 1, 1}}), &out) != 0 && out.empty());
    BucketAggregator null(c, "e");
    assert(null.Push(0, Batch({{0, 1, std::nullopt, 1}}), &out) != 0 && out.empty());
    c.read.max_pending_bytes = 32;
    BucketAggregator budget(c, "e");
    assert(budget.Push(0, Batch({{0, 1, 1, 1}}), &out) != 0 && out.empty());
}

std::vector<Observation> Collect(TaskConfig c, const std::vector<Row>& rows, size_t page) {
    BucketAggregator a(c, "e");
    std::vector<Observation> result, out;
    for (size_t i = 0; i < rows.size(); i += page) {
        auto end = std::min(i + page, rows.size());
        Ok(a.Push(0, Batch(std::vector<Row>(rows.begin() + i, rows.begin() + end)), &out), a);
        result.insert(result.end(), out.begin(), out.end());
    }
    Ok(a.FinishDataset(0, &out), a);
    result.insert(result.end(), out.begin(), out.end());
    return result;
}
void EquivalenceAndPolicies() {
    auto c = Config();
    std::vector<Row> rows{{0, 1, 10, 2}, {0, 2, 20, 3}, {1, 3, 30, 4}, {3, 4, 40, 5}};
    auto one = Collect(c, rows, 1), many = Collect(c, rows, 99);
    std::shared_ptr<arrow::RecordBatch> x, y;
    std::string error;
    assert(MakeObservationBatch(one, 65536, &x, &error) == 0 && MakeObservationBatch(many, 65536, &y, &error) == 0 &&
           x->Equals(*y));
    c.datasets[0].metrics[0].null_policy = InvalidPolicy::kSkip;
    c.datasets[0].metrics[1].null_policy = InvalidPolicy::kSkip;
    c.datasets[0].metrics[0].invalid_policy = InvalidPolicy::kSkip;
    c.datasets[0].metrics[1].invalid_policy = InvalidPolicy::kSkip;
    auto valid =
        Collect(c, {{0, 1, std::nullopt, 100}, {0, 2, std::numeric_limits<double>::infinity(), 100}, {0, 3, 5, 2}}, 1);
    assert(valid.size() == 2 && valid[0].value == 5 && valid[0].sample_count == 1 && valid[1].numerator == 5 &&
           valid[1].denominator == 2);
    c.datasets[0].metrics.resize(1);
    for (auto op : {Aggregate::kSum, Aggregate::kMean, Aggregate::kMin, Aggregate::kMax, Aggregate::kCount}) {
        c.datasets[0].metrics[0].value.aggregate = op;
        auto r = Collect(c, {{0, 1, 10, 1}, {0, 2, 20, 1}}, 2);
        double expected = op == Aggregate::kSum    ? 30
                          : op == Aggregate::kMean ? 15
                          : op == Aggregate::kMin  ? 10
                          : op == Aggregate::kMax  ? 20
                                                   : 2;
        assert(r.size() == 1 && r[0].value == expected);
    }
    c.datasets[0].metrics[0].value.aggregate = Aggregate::kColumn;
    BucketAggregator multiple(c, "e");
    std::vector<Observation> out;
    // invalid_policy=skip means invalid column cardinality produces no observation.
    Ok(multiple.Push(0, Batch({{0, 1, 1, 1}, {0, 2, 2, 1}}), &out), multiple);
    Ok(multiple.FinishDataset(0, &out), multiple);
    assert(out.empty());
    c.datasets[0].metrics[0].value = {"n", Aggregate::kSum, 2, 10};
    auto scaled = Collect(c, {{0, 1, 10, 1}}, 1);
    assert(scaled[0].value == 2);
    c.datasets[0].fill_missing_zero = true;
    auto filled = Collect(c, {{2, 1, 10, 1}}, 1);
    assert(filled.size() == 4 && filled[0].bucket == 0 && filled[0].value == 0 && filled[2].value == 2 &&
           filled[3].bucket == 3);
    assert(Collect(c, {}, 1).empty());
    c.datasets[0].fill_missing_zero = false;
    c.state.limits.max_runtime_identities = 1;
    BucketAggregator capacity(c, "e");
    assert(capacity.Push(0, Batch({{0, 1, 1, 1, 1, "A"}, {0, 2, 1, 1, 1, "B"}}), &out) != 0);
    c = Config();
    c.datasets.push_back(c.datasets[0]);
    c.datasets[1].id = "two";
    BucketAggregator datasets(c, "e");
    Ok(datasets.Push(0, Batch({{0, 1, 1, 1}}), &out), datasets);
    Ok(datasets.FinishDataset(0, &out), datasets);
    auto identity = out[0].identity;
    Ok(datasets.Push(1, Batch({{0, 1, 1, 1}}), &out), datasets);
    Ok(datasets.FinishDataset(1, &out), datasets);
    assert(out[0].identity != identity);
}
std::shared_ptr<arrow::RecordBatch> UnsignedBatch(uint64_t value) {
    auto batch = Batch({{0, 1, 1, 1}});
    arrow::UInt64Builder b;
    assert(b.Append(value).ok());
    auto a = b.Finish();
    assert(a.ok());
    auto result = batch->SetColumn(2, arrow::field("n", arrow::uint64()), *a);
    assert(result.ok());
    return *result;
}
void Precision() {
    auto c = Config();
    c.datasets[0].metrics.resize(1);
    c.datasets[0].fields["n"] = LogicalType::kUInt64;
    std::vector<Observation> out;
    BucketAggregator exact(c, "e");
    Ok(exact.Push(0, UnsignedBatch(uint64_t{1} << 63), &out), exact);
    Ok(exact.FinishDataset(0, &out), exact);
    assert(out[0].value == std::ldexp(1.0, 63));
    BucketAggregator loss(c, "e");
    Ok(loss.Push(0, UnsignedBatch((uint64_t{1} << 53) + 1), &out), loss);
    assert(loss.FinishDataset(0, &out) != 0 && out.empty());
    c = Config();
    c.datasets[0].metrics.resize(1);
    c.datasets[0].metrics[0].sample_count = Expression{"id", Aggregate::kSum};
    BucketAggregator sample(c, "e");
    Ok(sample.Push(0, Batch({{0, UINT64_MAX, 1, 1}}), &out), sample);
    Ok(sample.FinishDataset(0, &out), sample);
    assert(out[0].sample_count == UINT64_MAX);
    BucketAggregator overflow(c, "e");
    assert(overflow.Push(0, Batch({{0, UINT64_MAX, 1, 1}, {0, 1, 1, 1}}), &out) != 0);
}
TaskConfig NpmConfig() {
    auto c = Config();
    auto& d = c.datasets[0];
    d.fields = {{"period_start_ns", LogicalType::kInt64},
                {"period_end_ns", LogicalType::kInt64},
                {"period_complete", LogicalType::kBoolean},
                {"is_final", LogicalType::kBoolean},
                {"interval_packets_ab", LogicalType::kUInt64},
                {"interval_packets_ba", LogicalType::kUInt64},
                {"interval_wire_bytes_ab", LogicalType::kUInt64},
                {"__npm_run_id", LogicalType::kUtf8},
                {"session_id", LogicalType::kUInt64},
                {"revision", LogicalType::kUInt64}};
    d.bucket_column = "period_start_ns";
    d.unit = TimeUnit::kNs;
    d.series_keys = {"__npm_run_id"};
    d.deduplicate_keys = {"__npm_run_id", "session_id", "revision"};
    d.row_semantics = "npm_period_increment";
    d.metrics.resize(1);
    d.metrics[0].value = {"interval_wire_bytes_ab", Aggregate::kSum};
    return c;
}
struct Period {
    int64_t start;
    uint64_t session, revision, bytes;
    int64_t width = 30000000000;
    bool final = false, complete = true;
};
std::shared_ptr<arrow::RecordBatch> Periods(const std::vector<Period>& rows) {
    auto d = NpmConfig().datasets[0];
    std::vector<std::shared_ptr<arrow::Field>> fields;
    std::vector<std::shared_ptr<arrow::Array>> arrays;
    for (const auto& [name, type] : d.fields) {
        auto made = arrow::MakeBuilder(ArrowType(type));
        assert(made.ok());
        auto builder = std::move(*made);
        for (const auto& r : rows) {
            std::shared_ptr<arrow::Scalar> scalar;
            if (type == LogicalType::kUtf8)
                scalar = std::make_shared<arrow::StringScalar>("run");
            else if (type == LogicalType::kInt64)
                scalar = std::make_shared<arrow::Int64Scalar>(name == "period_start_ns" ? r.start : r.start + r.width);
            else if (type == LogicalType::kBoolean)
                scalar = std::make_shared<arrow::BooleanScalar>(name == "is_final" ? r.final : r.complete);
            else
                scalar = std::make_shared<arrow::UInt64Scalar>(name == "session_id"               ? r.session
                                                               : name == "revision"               ? r.revision
                                                               : name == "interval_wire_bytes_ab" ? r.bytes
                                                               : r.final                          ? 0
                                                                                                  : 1);
            assert(builder->AppendScalar(*scalar).ok());
        }
        auto array = builder->Finish();
        assert(array.ok());
        arrays.push_back(*array);
        fields.push_back(arrow::field(name, ArrowType(type)));
    }
    return arrow::RecordBatch::Make(arrow::schema(fields), rows.size(), arrays);
}
void NpmBuckets() {
    auto c = NpmConfig();
    std::vector<Observation> out;
    BucketAggregator a(c, "e");
    Ok(a.Push(0, Periods({{0, 1, 1, 10}}), &out), a);
    assert(out.empty());
    Ok(a.Push(0, Periods({{30000000000, 1, 2, 20}, {60000000000, 1, 3, 30}}), &out), a);
    assert(out.size() == 1 && out[0].bucket == 0 && out[0].value == 30);
    Ok(a.Push(0, Periods({{30000000000, 1, 4, 0, 30000000000, true}}), &out), a);
    assert(out.empty() && a.Stats().terminal_rows == 1);
    Ok(a.FinishDataset(0, &out), a);
    assert(out[0].bucket == 1 && out[0].value == 30);
    c.bucket_seconds = 30;
    BucketAggregator same(c, "e");
    Ok(same.Push(0, Periods({{0, 1, 1, 10}, {30000000000, 1, 2, 20}}), &out), same);
    assert(out.size() == 1 && out[0].value == 10);
    Ok(same.FinishDataset(0, &out), same);
    assert(out[0].bucket == 1 && out[0].value == 20);
    c = NpmConfig();
    BucketAggregator wrong(c, "e");
    assert(wrong.Push(0, Periods({{0, 1, 1, 1, 45000000000}}), &out) != 0);
    BucketAggregator changed(c, "e");
    assert(changed.Push(0, Periods({{0, 1, 1, 1}, {0, 2, 1, 1, 60000000000}}), &out) != 0);
    c.datasets[0].filter = {{"period_complete", "eq", true}};
    BucketAggregator partial(c, "e");
    Ok(partial.Push(0, Periods({{0, 1, 1, 5, 30000000000, false, false}}), &out), partial);
    Ok(partial.FinishDataset(0, &out), partial);
    assert(out.empty());
    c.datasets[0].metrics[0].value.column = "session_id";
    BucketAggregator cumulative(c, "e");
    assert(cumulative.Push(0, Periods({{0, 1, 1, 1}}), &out) != 0);
}

void Relations() {
    auto c = Config();
    auto& d = c.datasets[0];
    Metric relation;
    relation.id = "mix";
    relation.kind = BaselineTaskKind::kRelation;
    GroupSpace groups;
    groups.id = "groups";
    groups.version = "1";
    groups.column = "id";
    relation.group_space = groups;
    relation.relation_metrics = {{"second", {"d", Aggregate::kSum}}, {"first", {"n", Aggregate::kSum}}};
    d.metrics = {relation};
    BucketAggregator a(c, "e");
    std::vector<Observation> out;
    Ok(a.Push(0, Batch({{0, 2, 10, 2}}), &out), a);
    assert(out.empty());
    // Dedup identity is revision-aware so repeated group 2 is a distinct contribution.
    c.datasets[0].deduplicate_keys = {"id", "rev"};
    BucketAggregator b(c, "e");
    Ok(b.Push(0, Batch({{0, 2, 10, 2, 1}, {0, 1, 20, 4, 1}}), &out), b);
    assert(out.empty());
    Ok(b.Push(0, Batch({{0, 2, 5, 1, 2}, {1, 3, 7, 2, 1}}), &out), b);
    assert(out.size() == 1 && out[0].groups == std::vector<uint32_t>({1, 2}));
    assert(out[0].metrics[0].metric == "second" && out[0].metrics[0].values_by_group == std::vector<double>({4, 3}));
    assert(out[0].metrics[1].metric == "first" && out[0].metrics[1].values_by_group == std::vector<double>({20, 15}));
    assert(out[0].metrics[1].total == 35 && out[0].metrics[1].active_count == 2);
    std::shared_ptr<arrow::RecordBatch> batch;
    std::string error;
    assert(MakeObservationBatch(out, 65536, &batch, &error) == 0 && batch->ValidateFull().ok());
    auto list = std::static_pointer_cast<arrow::ListArray>(batch->GetColumnByName("metrics"));
    assert(list->value_length(0) == 2);
    Ok(b.FinishDataset(0, &out), b);
    assert(out[0].groups == std::vector<uint32_t>({3}));
    // Dictionary order does not renumber stable group_idx; unknown uses explicit other.
    auto& m = c.datasets[0].metrics[0];
    c.datasets[0].series_keys = {"rev"};  // One source, distinct dictionary groups.
    m.group_space->column = "series";
    m.group_space->dictionary = {{std::string("B"), 7}, {std::string("A"), 3}};
    m.group_space->other = 9;
    auto r = Collect(c, {{0, 1, 1, 2, 1, "A"}, {0, 2, 2, 3, 1, "B"}, {0, 3, 3, 4, 1, "unknown"}}, 1);
    assert(r[0].groups == std::vector<uint32_t>({3, 7, 9}) && r[0].metrics[1].total == 6);
    auto together = Collect(c, {{0, 1, 1, 2, 1, "A"}, {0, 2, 2, 3, 1, "B"}, {0, 3, 3, 4, 1, "unknown"}}, 99);
    std::shared_ptr<arrow::RecordBatch> x, y;
    assert(MakeObservationBatch(r, 65536, &x, &error) == 0 && MakeObservationBatch(together, 65536, &y, &error) == 0 &&
           x->Equals(*y));
    m.group_space->other.reset();
    BucketAggregator unknown(c, "e");
    assert(unknown.Push(0, Batch({{0, 1, 1, 1, 1, "unknown"}}), &out) != 0);
    m.group_space->column = "id";
    m.group_space->dictionary.clear();
    BucketAggregator large(c, "e");
    assert(large.Push(0, Batch({{0, UINT64_MAX, 1, 1}}), &out) != 0);
    BucketAggregator negative(c, "e");
    assert(negative.Push(0, Batch({{0, 1, -1, 2}}), &out) != 0);
    m.null_policy = InvalidPolicy::kSkip;
    auto skipped = Collect(c, {{0, 1, std::nullopt, 99}, {0, 2, 5, 2}}, 1);
    assert(skipped[0].groups == std::vector<uint32_t>({2}) && skipped[0].metrics[0].total == 2);
    BucketAggregator missing(c, "e");
    auto without = Batch({{0, 1, 1, 1}})->RemoveColumn(3);
    assert(without.ok());
    assert(missing.Push(0, *without, &out) != 0);
    c.read.max_pending_bytes = 1000;
    BucketAggregator bounded(c, "e");
    assert(bounded.Push(0, Batch({{0, 1, 1, 1}, {0, 2, 2, 2}}), &out) != 0);
    // Relation uses the same integer-multiple NPM bucket mapping.
    c = NpmConfig();
    auto& npm = c.datasets[0];
    relation.group_space->column = "session_id";
    relation.relation_metrics = {{"bytes", {"interval_wire_bytes_ab", Aggregate::kSum}},
                                 {"packets", {"interval_packets_ab", Aggregate::kSum}}};
    npm.metrics = {relation};
    BucketAggregator merged(c, "e");
    Ok(merged.Push(0, Periods({{0, 1, 1, 10}, {30000000000, 2, 1, 20}, {60000000000, 1, 2, 30}}), &out), merged);
    assert(out.size() == 1 && out[0].groups == std::vector<uint32_t>({1, 2}) && out[0].metrics[0].total == 30);
    Ok(merged.FinishDataset(0, &out), merged);
    assert(out[0].bucket == 1);
}
}  // namespace
int main() {
    Relations();
    Core();
    EquivalenceAndPolicies();
    Precision();
    NpmBuckets();
    Failures();
    std::cout << "PASS Value/Ratio aggregation\n";
}
