// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <arrow/api.h>
#include <arrow/util/byte_size.h>
#include <framework/core/dataframe.h>
#include <framework/core/dataframe_channel.h>
#include <operators/baseliner/bucket_aggregator.h>
#include <operators/baseliner/dataframe_reader.h>
#include <operators/baseliner/snapshot_input.h>
#include <cassert>
#include <iostream>
#include <limits>
#include <thread>
using namespace flowsql;
using namespace flowsql::baseliner;
namespace {
void Check(bool pass, const std::string& error) {
    if (!pass) {
        std::cerr << error << '\n';
        std::abort();
    }
}
TaskConfig Config() {
    TaskConfig c;
    c.source = "dataframe.samples";
    c.database_source = false;
    c.dataframe_source = true;
    c.task_key = "frame";
    c.read.page_rows = 1;
    Dataset d;
    d.id = "source";
    d.fields = {{"bucket", LogicalType::kInt64}, {"id", LogicalType::kUInt64},  {"n", LogicalType::kFloat64},
                {"d", LogicalType::kFloat64},    {"rev", LogicalType::kUInt64}, {"series", LogicalType::kUtf8}};
    d.scope.consistency = "dataframe_snapshot";
    d.bucket_column = "bucket";
    d.series_keys = {"series"};
    d.deduplicate_keys = {"id"};
    Metric value;
    value.id = "value";
    value.value = {"n", Aggregate::kSum};
    Metric ratio;
    ratio.id = "ratio";
    ratio.kind = BaselineTaskKind::kRatio;
    ratio.numerator = {"n", Aggregate::kSum};
    ratio.denominator = {"d", Aggregate::kSum};
    Metric relation;
    relation.id = "relation";
    relation.kind = BaselineTaskKind::kRelation;
    GroupSpace groups;
    groups.id = "groups";
    groups.version = "1";
    groups.column = "id";
    relation.group_space = groups;
    relation.relation_metrics = {{"n", {"n", Aggregate::kSum}}, {"d", {"d", Aggregate::kSum}}};
    d.metrics = {value, ratio, relation};
    c.datasets = {d};
    return c;
}
struct Row {
    int32_t bucket;
    uint32_t id;
    std::optional<float> n;
    float d;
    uint32_t rev = 1;
    std::string series = "A";
};
std::shared_ptr<arrow::RecordBatch> Batch(const std::vector<Row>& rows, bool wide = false) {
    DataFrame df;
    df.SetSchema({{"bucket", wide ? DataType::INT64 : DataType::INT32, 0, ""},
                  {"id", wide ? DataType::UINT64 : DataType::UINT32, 0, ""},
                  {"n", wide ? DataType::DOUBLE : DataType::FLOAT, 0, ""},
                  {"d", wide ? DataType::DOUBLE : DataType::FLOAT, 0, ""},
                  {"rev", wide ? DataType::UINT64 : DataType::UINT32, 0, ""},
                  {"series", DataType::STRING, 0, ""}});
    for (const auto& r : rows) {
        Check(r.n.has_value(), "Batch uses explicit nullable column replacement");
        if (wide)
            Check(df.AppendRow(
                      {int64_t(r.bucket), uint64_t(r.id), double(*r.n), double(r.d), uint64_t(r.rev), r.series}) == 0,
                  "append wide");
        else
            Check(df.AppendRow({r.bucket, r.id, *r.n, r.d, r.rev, r.series}) == 0, "append narrow");
    }
    return df.ToArrow();
}
BlockDataFrameInputBindingV1 Binding(const std::shared_ptr<arrow::RecordBatch>& batch) {
    auto source = std::make_shared<DataFrameChannel>("dataframe", "samples");
    Check(source->Open() == 0, "channel Open");
    DataFrame df;
    df.FromArrow(batch);
    Check(source->Write(&df) == 0, "channel Write");
    BlockDataFrameInputBindingV1 binding;
    binding.source = source;
    binding.exact_source = "dataframe.samples";
    return binding;
}
std::vector<Observation> Collect(DataFrameReader& reader) {
    BucketAggregator aggregate(reader.Config(), "same-epoch");
    std::vector<Observation> result;
    for (;;) {
        SnapshotPage page;
        Check(reader.Next(&page) == 0, reader.LastError());
        std::vector<Observation> observations;
        int rc = page.eof ? aggregate.FinishDataset(page.dataset_index, &observations)
                          : aggregate.Push(page.dataset_index, page.batch, &observations);
        Check(rc == 0, aggregate.LastError());
        result.insert(result.end(), observations.begin(), observations.end());
        if (page.eof) break;
        Check(page.batch->num_rows() <= reader.Config().read.page_rows, "page_rows exceeded");
    }
    return result;
}
std::shared_ptr<arrow::RecordBatch> Encode(const std::vector<Observation>& observations) {
    std::shared_ptr<arrow::RecordBatch> output;
    std::string error;
    Check(MakeObservationBatch(observations, 1024 * 1024, &output, &error) == 0, error);
    return output;
}
void OrderedEquivalence() {
    auto raw = Batch({{2, 4, 40, 5}, {0, 2, 20, 3}, {1, 3, 30, 4}, {0, 1, 10, 2}, {0, 2, 20, 3}});
    auto original = Batch({{2, 4, 40, 5}, {0, 2, 20, 3}, {1, 3, 30, 4}, {0, 1, 10, 2}, {0, 2, 20, 3}});
    auto binding = Binding(raw);
    auto c = Config();
    DataFrameReader reader;
    Check(reader.Open(c, binding, raw) == 0, reader.LastError());
    auto result = Collect(reader);
    Check(result.size() == 9, "three metrics and tail bucket");
    auto expected_raw = Batch({{0, 1, 10, 2}, {0, 2, 20, 3}, {1, 3, 30, 4}, {2, 4, 40, 5}}, true);
    BucketAggregator expected(c, "same-epoch");
    std::vector<Observation> first, tail;
    Check(expected.Push(0, expected_raw, &first) == 0, expected.LastError());
    Check(expected.FinishDataset(0, &tail) == 0, expected.LastError());
    first.insert(first.end(), tail.begin(), tail.end());
    Check(Encode(first)->Equals(*Encode(result)), "typed raw-page aggregation differs");
    DataFrame after;
    Check(binding.source->Read(&after) == 0 && after.ToArrow()->Equals(*original) && raw->Equals(*original),
          "source changed");
    Check(reader.Stats().selected_rows == 5 && reader.Stats().peak_buffer_bytes <= c.read.max_pending_bytes,
          "reader budget");
    auto hash = reader.SourceFingerprint();
    Check(hash.size() == std::string("dataframe_snapshot_v1:").size() + 64, "versioned fingerprint");
    c.read.page_rows = 99;
    DataFrameReader many;
    Check(many.Open(c, binding, raw) == 0 && many.SourceFingerprint() == hash, many.LastError());
    Check(Encode(Collect(many))->Equals(*Encode(result)), "page size changed result");
    std::cout << "PASS unordered narrow snapshot, three metric aggregation, duplicate and tail EOF\n";
}
template <typename Builder, typename T>
std::shared_ptr<arrow::Array> Array(std::initializer_list<T> values) {
    Builder builder;
    for (auto value : values) Check(builder.Append(value).ok(), "array append");
    auto array = builder.Finish();
    Check(array.ok(), array.status().ToString());
    return *array;
}
std::shared_ptr<arrow::RecordBatch> Replace(const std::shared_ptr<arrow::RecordBatch>& batch, const std::string& name,
                                            std::shared_ptr<arrow::Array> array) {
    auto result = batch->SetColumn(batch->schema()->GetFieldIndex(name), arrow::field(name, array->type()), array);
    Check(result.ok(), result.status().ToString());
    return *result;
}
std::vector<std::pair<int64_t, uint64_t>> Rows(DataFrameReader& reader) {
    std::vector<std::pair<int64_t, uint64_t>> result;
    for (;;) {
        SnapshotPage page;
        Check(reader.Next(&page) == 0, reader.LastError());
        if (page.eof) {
            Check(!page.batch, "EOF carries no raw data");
            break;
        }
        Check(page.batch && page.batch->num_rows() <= reader.Config().read.page_rows, "invalid page");
        for (int64_t i = 0; i < page.batch->num_rows(); ++i)
            result.emplace_back(
                std::static_pointer_cast<arrow::Int64Array>(page.batch->GetColumnByName("bucket"))->Value(i),
                std::static_pointer_cast<arrow::UInt64Array>(page.batch->GetColumnByName("id"))->Value(i));
    }
    return result;
}
void ScopeAndRevision() {
    auto raw = Batch({{2, 4, 40, 5}, {0, 2, 20, 3}, {1, 3, 30, 4}, {0, 1, 10, 2}});
    auto binding = Binding(raw);
    auto c = Config();
    c.datasets[0].scope.begin_bucket = 0;
    c.datasets[0].scope.end_bucket = 2;
    c.datasets[0].filter = {{"n", "gt", 10.0}, {"series", "eq", std::string("A")}};
    DataFrameReader ranged;
    Check(ranged.Open(c, binding, raw) == 0, ranged.LastError());
    Check(Rows(ranged) == std::vector<std::pair<int64_t, uint64_t>>{{0, 2}, {1, 3}}, "half-open scope/filter");
    raw = Batch({{60000, 5, 5, 1}, {-1, 2, 2, 1}, {-60001, 4, 4, 1}, {0, 3, 3, 1}, {-60000, 1, 1, 1}});
    binding = Binding(raw);
    c = Config();
    c.datasets[0].scope.begin_bucket = -1;
    c.datasets[0].scope.end_bucket = 1;
    c.datasets[0].unit = TimeUnit::kMs;
    DataFrameReader negative;
    Check(negative.Open(c, binding, raw) == 0, negative.LastError());
    Check(Rows(negative) == std::vector<std::pair<int64_t, uint64_t>>{{-60000, 1}, {-1, 2}, {0, 3}},
          "negative time floor/range");
    raw = Batch({{0, 1, 100, 1, 1}, {2, 1, 2, 1, 9}, {0, 2, 3, 1, 1}, {2, 1, 2, 1, 9}});
    binding = Binding(raw);
    c = Config();
    c.datasets[0].row_semantics = "latest_revision";
    c.datasets[0].revision_column = "rev";
    DataFrameReader latest;
    Check(latest.Open(c, binding, raw) == 0, latest.LastError());
    Check(latest.Stats().selected_rows == 3, "keep equal maximum revisions for conflict validation");
    auto observations = Collect(latest);
    Check(observations.size() == 6 && observations[0].value == 3 && observations[3].value == 2,
          "maximum revision selection");
    c.datasets[0].scope.begin_bucket = 0;
    c.datasets[0].scope.end_bucket = 1;
    DataFrameReader scoped;
    Check(scoped.Open(c, binding, raw) == 0, scoped.LastError());
    auto scoped_observations = Collect(scoped);
    Check(scoped_observations.size() == 3 && scoped_observations[0].value == 103, "scope before maximum revision");
    c.datasets[0].scope = {};
    c.datasets[0].scope.consistency = "dataframe_snapshot";
    c.datasets[0].filter = {{"n", "gt", 10.0}};
    DataFrameReader filtered;
    Check(filtered.Open(c, binding, raw) == 0 && Rows(filtered) == std::vector<std::pair<int64_t, uint64_t>>{{0, 1}},
          "filter before maximum revision");
    std::cout << "PASS half-open range, negative time, filters and maximum revision\n";
}
void Conflict(const std::shared_ptr<arrow::RecordBatch>& raw, bool latest) {
    auto c = Config();
    if (latest) {
        c.datasets[0].row_semantics = "latest_revision";
        c.datasets[0].revision_column = "rev";
    }
    DataFrameReader reader;
    Check(reader.Open(c, Binding(raw), raw) == 0, reader.LastError());
    BucketAggregator aggregate(c, "e");
    bool rejected = false;
    for (;;) {
        SnapshotPage page;
        Check(reader.Next(&page) == 0, reader.LastError());
        std::vector<Observation> output;
        if (page.eof) break;
        if (aggregate.Push(0, page.batch, &output) != 0) {
            Check(output.empty(), "failed aggregate emits no observations");
            rejected = true;
            break;
        }
    }
    Check(rejected, "conflicting duplicates accepted");
}
void Reject(TaskConfig c, BlockDataFrameInputBindingV1 binding, const std::shared_ptr<arrow::RecordBatch>& raw,
            const std::string& fragment) {
    DataFrameReader reader;
    Check(reader.Open(std::move(c), binding, raw) != 0, "invalid snapshot accepted: " + fragment);
    Check(reader.LastError().find(fragment) != std::string::npos, "wrong rejection: " + reader.LastError());
    Check(reader.SourceFingerprint().empty(), "failed Open publishes fingerprint");
    SnapshotPage page{99, raw, true};
    Check(reader.Next(&page) != 0 && !page.batch && !page.eof, "failed reader reports data/EOF");
}
void FailureBoundaries() {
    auto raw = Batch({{0, 1, 1, 1}, {1, 2, 2, 1}});
    auto binding = Binding(raw);
    auto invalid = binding;
    invalid.contract_version = 2;
    Reject(Config(), invalid, raw, "source");
    invalid = binding;
    --invalid.struct_size;
    Reject(Config(), invalid, raw, "source");
    invalid = binding;
    invalid.exact_source = "dataframe.other";
    Reject(Config(), invalid, raw, "source");
    invalid = binding;
    invalid.source.reset();
    Reject(Config(), invalid, raw, "source");
    Reject(Config(), binding, nullptr, "source");
    auto c = Config();
    c.mode = Mode::kPoll;
    Reject(c, binding, raw, "source");
    c = Config();
    c.datasets.push_back(c.datasets[0]);
    Reject(c, binding, raw, "source");
    c = Config();
    c.datasets[0].table = "other";
    Reject(c, binding, raw, "config");
    c = Config();
    c.read.max_pending_bytes = 1;
    Reject(c, binding, raw, "budget");
    c.read.max_pending_bytes = arrow::util::TotalBufferSize(*raw) + raw->num_rows() * sizeof(int64_t) + 64;
    Reject(c, binding, raw, "budget");
    auto missing = raw->RemoveColumn(0);
    Check(missing.ok(), "remove column");
    Reject(Config(), binding, *missing, "missing/ambiguous");
    auto duplicate = raw->AddColumn(0, raw->schema()->field(0), raw->column(0));
    Check(duplicate.ok(), "duplicate field");
    Reject(Config(), binding, *duplicate, "missing/ambiguous");
    auto numeric_string = Replace(raw, "bucket", Array<arrow::StringBuilder, const char*>({"0", "1"}));
    Reject(Config(), binding, numeric_string, "unsupported DataFrame conversion");
    auto unsigned_time = Replace(raw, "bucket", Array<arrow::UInt64Builder, uint64_t>({0, UINT64_MAX}));
    Reject(Config(), binding, unsigned_time, "range");
    auto negative_id = Replace(raw, "id", Array<arrow::Int64Builder, int64_t>({0, -1}));
    Reject(Config(), binding, negative_id, "range");
    auto integer_value = Replace(raw, "n", Array<arrow::Int64Builder, int64_t>({0, INT64_MAX}));
    Reject(Config(), binding, integer_value, "unsupported DataFrame conversion");
    auto timestamp = Replace(
        raw, "bucket",
        std::make_shared<arrow::TimestampArray>(arrow::timestamp(arrow::TimeUnit::SECOND), 2,
                                                Array<arrow::Int64Builder, int64_t>({0, 1})->data()->buffers[1]));
    Reject(Config(), binding, timestamp, "unsupported DataFrame conversion");
    arrow::UInt32Builder null_key;
    Check(null_key.AppendNull().ok() && null_key.Append(2).ok(), "null key");
    auto nullable_key = null_key.Finish();
    Check(nullable_key.ok(), "null key finish");
    Reject(Config(), binding, Replace(raw, "id", *nullable_key), "NULL stable");
    c = Config();
    c.datasets[0].fields["id"] = LogicalType::kFloat64;
    Reject(c, binding,
           Replace(raw, "id", Array<arrow::DoubleBuilder, double>({1, std::numeric_limits<double>::infinity()})),
           "nonfinite stable");
    auto malformed = arrow::RecordBatch::Make(raw->schema(), 3, raw->columns());
    Reject(Config(), binding, malformed, "match");
    Conflict(Batch({{0, 1, 1, 1}, {0, 1, 2, 1}}), false);
    Conflict(Batch({{0, 1, 1, 1, 9}, {1, 1, 1, 1, 9}}), true);
    std::cout << "PASS source/version/type/overflow/key/budget failures and duplicate conflicts\n";
}
void FingerprintsAndOwnership() {
    auto raw = Batch({{0, 1, 1, 1}, {1, 2, 2, 1}});
    auto binding = Binding(raw);
    auto fingerprint = [&](const std::shared_ptr<arrow::RecordBatch>& data) {
        DataFrameReader reader;
        Check(reader.Open(Config(), binding, data) == 0, reader.LastError());
        return reader.SourceFingerprint();
    };
    auto hash = fingerprint(raw);
    auto sliced = Batch({{-9, 9, 9, 9}, {0, 1, 1, 1}, {1, 2, 2, 1}, {9, 9, 9, 9}})->Slice(1, 2);
    Check(fingerprint(sliced) == hash, "fingerprint depends on buffer offset");
    Check(fingerprint(Batch({{0, 1, 7, 1}, {1, 2, 2, 1}})) != hash, "content change invisible");
    auto metadata = raw->ReplaceSchemaMetadata(arrow::key_value_metadata({"unit"}, {"custom"}));
    Check(fingerprint(metadata) != hash, "Schema metadata change invisible");
    Check(fingerprint(Batch({{0, 1, 1, 1}, {1, 2, 2, 1}}, true)) != hash, "physical Schema change invisible");
    auto extra =
        raw->AddColumn(0, arrow::field("ignored", arrow::utf8()), Array<arrow::StringBuilder, const char*>({"x", "y"}));
    Check(extra.ok(), "extra column");
    auto extra_changed = Replace(*extra, "ignored", Array<arrow::StringBuilder, const char*>({"x", "z"}));
    Check(fingerprint(*extra) != fingerprint(extra_changed), "ignored source content change invisible");
    std::weak_ptr<IDataFrameChannel> lease = binding.source;
    std::shared_ptr<arrow::Array> owned_column;
    {
        auto reader = std::make_unique<DataFrameReader>();
        Check(reader->Open(Config(), binding, raw) == 0, reader->LastError());
        binding.source.reset();
        Check(!lease.expired(), "reader dropped source lease");
        SnapshotPage page;
        Check(reader->Next(&page) == 0 && page.batch, reader->LastError());
        owned_column = page.batch->GetColumnByName("bucket");
        raw.reset();
    }
    Check(lease.expired(), "reader retained source lease after destruction");
    Check(std::static_pointer_cast<arrow::Int64Array>(owned_column)->Value(0) == 0, "column lifetime lost");
    owned_column.reset();
    raw = Batch({{0, 1, 1, 1}});
    binding = Binding(raw);
    DataFrameReader frozen;
    Check(frozen.Open(Config(), binding, raw) == 0, frozen.LastError());
    DataFrame replacement;
    replacement.FromArrow(Batch({{99, 99, 99, 99}}));
    Check(binding.source->Write(&replacement) == 0 && Rows(frozen) == std::vector<std::pair<int64_t, uint64_t>>{{0, 1}},
          "reader followed replaced source");
    std::cout << "PASS full content/Schema fingerprint, source replacement, lease and extracted column lifetime\n";
}
void NullEmptyAndCancel() {
    auto empty = Batch({{0, 0, 0, 0}})->Slice(0, 0);
    auto binding = Binding(empty);
    DataFrameReader reader;
    Check(reader.Open(Config(), binding, empty) == 0, reader.LastError());
    Check(Collect(reader).empty(), "empty snapshot emits observations");
    SnapshotPage page;
    Check(reader.Next(&page) == 0 && page.eof, "empty/repeated EOF");
    auto raw = Batch({{0, 1, 1, 1}, {0, 2, 10, 2}});
    arrow::FloatBuilder nullable;
    Check(nullable.AppendNull().ok() && nullable.Append(10).ok(), "nullable value");
    auto array = nullable.Finish();
    Check(array.ok(), "nullable finish");
    raw = Replace(raw, "n", *array);
    auto c = Config();
    c.datasets[0].metrics.resize(2);
    for (auto& metric : c.datasets[0].metrics) metric.null_policy = InvalidPolicy::kSkip;
    DataFrameReader nulls;
    Check(nulls.Open(c, Binding(raw), raw) == 0, nulls.LastError());
    auto result = Collect(nulls);
    Check(result.size() == 2 && result[0].value == 10 && result[1].numerator == 10 && result[1].denominator == 2,
          "null policy changed");
    raw = Batch({{0, 1, 1, 1}});
    binding = Binding(raw);
    DataFrameReader before;
    before.Cancel();
    Check(before.Open(Config(), binding, raw) != 0, "cancel before Open");
    DataFrameReader during;
    Check(during.Open(Config(), binding, raw) == 0, during.LastError());
    std::thread cancel([&] { during.Cancel(); });
    cancel.join();
    page = {9, raw, true};
    Check(during.Next(&page) != 0 && !page.batch && !page.eof, "cancel returned success/data/EOF");
    DataFrameReader uninitialized;
    Check(uninitialized.Next(&page) != 0 && !page.eof, "unopened reader reports EOF");
    std::cout << "PASS typed empty EOF, NULL policy and cancellation without success EOF\n";
}
void AdapterCompletion() {
    auto raw = Batch({{2, 1, 2, 1}, {0, 1, 1, 1}});
    auto binding = Binding(raw);
    ConfigSnapshot config;
    config.config = Config();
    DataFrameSnapshotInput input;
    Check(input.InitializeDataFrame(config, binding) == 0, input.LastError());
    auto fingerprint = input.SourceFingerprint();
    std::weak_ptr<IDataFrameChannel> lease = binding.source;
    binding.source.reset();
    Check(!lease.expired(), "adapter dropped source lease");
    BlockInputProgressV1 progress;
    int observations = 0, boundaries = 0;
    for (;;) {
        auto event = input.PollBlock();
        if (event.kind == BlockPollEvent::kEof) break;
        Check(event.kind == BlockPollEvent::kData && event.batch, input.LastError());
        if (!event.batch->num_rows())
            ++boundaries;
        else
            observations += event.batch->num_rows();
        Check(input.ReadInputProgress(&progress) != 0, "unreleased block exposed progress");
        Check(input.ReleaseBlock(event.batch) == 0, "adapter ReleaseBlock");
        Check(input.ReadInputProgress(&progress) == 0, "adapter progress read");
        if (event.batch->num_rows()) Check(progress.datasets.empty(), "tail EOF exposed before boundary");
    }
    Check(observations == 6 && boundaries == 1 && progress.datasets.size() == 1 &&
              progress.datasets[0].epoch == fingerprint && progress.datasets[0].closed_before_bucket == 3 &&
              progress.datasets[0].committed_position == "eof:" + fingerprint,
          "adapter real EOF progress");
    input.Close();
    Check(lease.expired(), "adapter source lease survived Close");
    DataFrameSnapshotInput cancelled;
    binding = Binding(raw);
    Check(cancelled.InitializeDataFrame(config, binding) == 0, cancelled.LastError());
    auto event = cancelled.PollBlock();
    Check(event.kind == BlockPollEvent::kData, "cancel fixture data");
    cancelled.Cancel();
    Check(cancelled.ReleaseBlock(event.batch) == 0 && cancelled.ReadInputProgress(&progress) != 0 &&
              cancelled.PollBlock().kind == BlockPollEvent::kCancelled,
          "cancel adapter emitted success progress");
    auto empty = raw->Slice(0, 0);
    std::vector<std::shared_ptr<arrow::Array>> columns;
    std::vector<std::shared_ptr<arrow::Field>> fields;
    for (const auto& field : empty->schema()->fields()) {
        fields.push_back(arrow::field(field->name(), arrow::utf8()));
        columns.push_back(arrow::MakeArrayOfNull(arrow::utf8(), 0).ValueOrDie());
    }
    empty = arrow::RecordBatch::Make(arrow::schema(fields), 0, columns);
    DataFrameSnapshotInput header;
    Check(header.InitializeDataFrame(config, Binding(empty)) == 0, header.LastError());
    event = header.PollBlock();
    Check(event.kind == BlockPollEvent::kData && event.batch->num_rows() == 0, "empty EOF boundary missing");
    Check(header.ReleaseBlock(event.batch) == 0 && header.ReadInputProgress(&progress) == 0 &&
              progress.datasets.size() == 1 && header.PollBlock().kind == BlockPollEvent::kEof,
          "empty Schema snapshot completion");
    std::cout << "PASS snapshot adapter tail/empty EOF, progress release, cancel and source lease\n";
}
}  // namespace
int main() {
    DataFrame empty_frame;
    empty_frame.SetSchema({{"bucket", DataType::STRING, 0, ""}, {"group", DataType::STRING, 0, ""}});
    auto empty_arrow = empty_frame.ToArrow();
    Check(empty_arrow && empty_arrow->num_rows() == 0 && empty_arrow->num_columns() == 2,
          "explicit empty DataFrame Schema lost before CSV analysis");
    OrderedEquivalence();
    ScopeAndRevision();
    FailureBoundaries();
    FingerprintsAndOwnership();
    NullEmptyAndCancel();
    AdapterCompletion();
    return 0;
}
