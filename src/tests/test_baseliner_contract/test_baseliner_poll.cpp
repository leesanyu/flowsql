// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <operators/baseliner/poll_input.h>
#include <operators/npm_basic/npm_basic_result_consumer.h>
#include <operators/npm_basic/npm_result_progress.h>
#include <operators/npm_basic/output/npm_basic_result_encoder.h>
#include <services/database/database_plugin.h>
#include <unistd.h>
#include <cassert>
#include <fstream>
#include <sstream>
#include <thread>

using namespace flowsql;
using namespace flowsql::baseliner;
namespace {
constexpr int64_t second = 1000000000LL;
void CheckAt(bool ok, const std::string& error, int line) {
    if (!ok) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, error.c_str());
        std::abort();
    }
}
#define Check(ok, error) CheckAt(ok, error, __LINE__)
class Budget final : public npm::INpmTaskBudget {
 public:
    npm::NpmBudgetError Reserve(npm::NpmBudgetCategory, uint64_t) override { return npm::NpmBudgetError::kNone; }
    npm::NpmBudgetError Release(npm::NpmBudgetCategory, uint64_t) override { return npm::NpmBudgetError::kNone; }
    npm::NpmBudgetUsage Usage() const override { return {}; }
};
ConfigSnapshot Config(const std::string& source, const std::string& run, int64_t bucket = 60) {
    ConfigSnapshot c;
    c.config.source = source;
    c.config.task_key = "poll";
    c.config.mode = Mode::kPoll;
    c.config.bucket_seconds = bucket;
    c.config.read.page_rows = 1;
    c.config.read.poll_interval_ms = 1;
    Dataset d;
    d.id = "basic";
    d.table = "npm_basic_history_v1";
    d.scope.run_ids = {run};
    d.row_semantics = "npm_period_increment";
    d.bucket_column = "period_start_ns";
    d.unit = TimeUnit::kNs;
    d.fields = {{"__npm_run_id", LogicalType::kUtf8},     {"session_id", LogicalType::kUInt64},
                {"revision", LogicalType::kUInt64},       {"primary_label_id", LogicalType::kUInt64},
                {"period_start_ns", LogicalType::kInt64}, {"interval_wire_bytes_total", LogicalType::kUInt64}};
    d.series_keys = {"primary_label_id"};
    d.deduplicate_keys = {"__npm_run_id", "session_id", "revision"};
    Metric m;
    m.id = "bytes";
    m.value = {"interval_wire_bytes_total", Aggregate::kSum};
    d.metrics = {m};
    c.config.datasets = {d};
    return c;
}
BlockInputProgressV1 Boundary(PollInput& input, std::vector<double>* values = nullptr) {
    for (int i = 0; i < 100; ++i) {
        auto event = input.PollBlock(10);
        if (event.kind == BlockPollEvent::kTimeout) continue;
        Check(event.kind == BlockPollEvent::kData, input.LastError());
        BlockInputProgressV1 before;
        assert(input.ReadInputProgress(&before) != 0);
        if (event.batch->num_rows() && values) {
            auto array = std::static_pointer_cast<arrow::DoubleArray>(event.batch->GetColumnByName("value"));
            for (int64_t row = 0; row < array->length(); ++row) values->push_back(array->Value(row));
        }
        Check(input.ReleaseBlock(event.batch) == 0, "release");
        BlockInputProgressV1 after;
        Check(input.ReadInputProgress(&after) == 0, "released progress");
        if (!event.batch->num_rows()) return after;
    }
    std::abort();
}
struct GenericFacts {
    int generation = 1;
    int64_t closed = 2;
    int reads = 0;
    int fail_after = 0;
};
class GenericProgress final : public IDatabasePublishedProgressReaderV1 {
 public:
    explicit GenericProgress(std::shared_ptr<GenericFacts> facts) : facts_(std::move(facts)) {}
    int ReadProgress(const PublishedDatasetRequestV1& request, const std::string& position,
                     PublishedDatasetProgressV1* output) override {
        *output = {};
        if (cancelled_ || (facts_->fail_after && ++facts_->reads > facts_->fail_after)) return -1;
        if (!position.empty() && position != "opaque-" + std::to_string(facts_->generation)) return -1;
        output->progress = {request.dataset_id, "generic-epoch", "opaque-" + std::to_string(facts_->generation),
                            facts_->closed};
        return 0;
    }
    void Cancel() override { cancelled_ = true; }
    std::string LastError() const override { return "generic publication lost"; }

 private:
    std::shared_ptr<GenericFacts> facts_;
    std::atomic<bool> cancelled_{false};
};
// A normal business table is accepted only through its producer's explicit publication capability.
class GenericSource final : public IDatabaseChannel,
                            public IDatabaseSnapshotSourceV1,
                            public IDatabasePublishedSourceV1 {
 public:
    explicit GenericSource(std::shared_ptr<IDatabaseChannel> source) : source_(std::move(source)) {}
    std::shared_ptr<GenericFacts> facts = std::make_shared<GenericFacts>();
    const char* Category() override { return source_->Category(); }
    const char* Name() override { return source_->Name(); }
    const char* Type() override { return ChannelType::kDatabase; }
    const char* Schema() override { return ""; }
    int Open() override { return 0; }
    int Close() override { return 0; }
    int Flush() override { return 0; }
    bool IsOpened() const override { return source_->IsOpened(); }
    bool IsConnected() override { return source_->IsConnected(); }
    const char* GetLastError() override { return source_->GetLastError(); }
    int CreateReader(const char*, IBatchReader**) override { return -1; }
    int CreateWriter(const char*, IBatchWriter**) override { return -1; }
    int CreateArrowReader(const char*, IArrowReader**) override { return -1; }
    int CreateArrowWriter(const char*, IArrowWriter**) override { return -1; }
    int ExecuteQueryArrow(const char*, std::vector<std::shared_ptr<arrow::RecordBatch>>*) override { return -1; }
    int WriteArrowBatches(const char*, const std::vector<std::shared_ptr<arrow::RecordBatch>>&) override { return -1; }
    int ExecuteSql(const char*) override { return -1; }
    int CreateSnapshotSession(const DatabaseSnapshotOptionsV1& options,
                              std::shared_ptr<IDatabaseSnapshotSessionV1>* output) override {
        return dynamic_cast<IDatabaseSnapshotSourceV1*>(source_.get())->CreateSnapshotSession(options, output);
    }
    int CreatePublishedProgressReader(const DatabaseSnapshotOptionsV1&,
                                      std::shared_ptr<IDatabasePublishedProgressReaderV1>* output) override {
        *output = std::make_shared<GenericProgress>(facts);
        return 0;
    }

 private:
    std::shared_ptr<IDatabaseChannel> source_;
};
void TestGeneric(database::DatabasePlugin& plugin) {
    auto database = plugin.AcquireChannel("sqlite", "t2sqlite");
    auto source = std::make_shared<GenericSource>(database);
    const auto table =
        "baseliner_poll_generic_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    Check(database->ExecuteSql(("CREATE TABLE " + table + "(bucket INTEGER,id TEXT,value REAL)").c_str()) >= 0,
          "generic DDL");
    Check(database->ExecuteSql(("INSERT INTO " + table + " VALUES(0,'1',9),(0,'1',9),(0,'2',11),(1,'1',13)").c_str()) >=
              0,
          "generic data");
    ConfigSnapshot c;
    c.config.mode = Mode::kPoll;
    c.config.source = "sqlite.t2sqlite";
    c.config.read.page_rows = 1;
    Dataset d;
    d.id = "first";
    d.table = table;
    d.bucket_column = "bucket";
    d.unit = TimeUnit::kBucketId;
    d.fields = {{"bucket", LogicalType::kInt64}, {"id", LogicalType::kUInt64}, {"value", LogicalType::kFloat64}};
    d.series_keys = {"id"};
    d.deduplicate_keys = {"id"};
    Metric m;
    m.id = "value";
    m.value = {"value", Aggregate::kSum};
    d.metrics = {m};
    c.config.datasets = {d};
    d.id = "second";
    d.filter = {{"id", "eq", uint64_t{1}}};
    c.config.datasets.push_back(d);
    PollInput reader;
    Check(reader.Initialize(c, source, c.config.source) == 0, reader.LastError());
    std::vector<double> values;
    auto first = Boundary(reader, &values);
    assert(first.datasets.size() == 1 && values == std::vector<double>({9, 11, 13}));
    values.clear();
    auto both = Boundary(reader, &values);
    assert(both.datasets.size() == 2 && values == std::vector<double>({9, 13}));
    assert(reader.PollBlock(0).kind == BlockPollEvent::kTimeout);
    {
        PollInput lost;
        Check(lost.Initialize(c, source, c.config.source) == 0, lost.LastError());
        source->facts->reads = 0;
        source->facts->fail_after = 2;
        assert(lost.PollBlock(0).kind == BlockPollEvent::kError);  // fail within a bucket's pages before any output
        BlockInputProgressV1 p;
        assert(lost.ReadInputProgress(&p) != 0);
        source->facts->fail_after = 0;
    }
    // Identical rows deduplicate; conflicting facts at the same identity/bucket fail.
    Check(database->ExecuteSql(("INSERT INTO " + table + " VALUES(0,'1',99)").c_str()) >= 0, "conflict fixture");
    {
        PollInput conflict;
        Check(conflict.Initialize(c, source, c.config.source) == 0, conflict.LastError());
        assert(conflict.PollBlock(0).kind == BlockPollEvent::kError);
    }
    reader.Close();
    Check(database->ExecuteSql(("DROP TABLE " + table).c_str()) >= 0, "generic cleanup");
    std::puts("PASS generic published source, dataset isolation, duplicate/conflict and per-page failure");
}

void Test(database::DatabasePlugin& plugin, const std::string& category, const std::string& name) {
    auto source = plugin.AcquireChannel(category.c_str(), name.c_str());
    Check(source && source->IsConnected(), "required source unavailable: " + category);
    const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
    npm::NpmResultContextV1 context{"baseline-t3", "baseliner-t3-" + category + "-" + std::to_string(stamp)};
    auto entity = npm::NpmBasicEntityDescriptorV1();
    auto factory = npm::MakeNpmDatabaseResultConsumerFactory(source.get(), "netadapter.baseliner-t3");
    std::unique_ptr<npm::INpmManagedResultConsumerV1> consumer;
    const int created = factory->Create(context, {entity}, std::make_shared<Budget>(), &consumer);
    Check(created == 0, category + ": " + factory->LastError());
    auto* publisher = dynamic_cast<npm::INpmResultProgressConsumerV1*>(consumer.get());
    assert(publisher);
    auto publish = [&](int64_t closed) {
        npm::NpmResultProgressV1 p;
        p.period_ns = 30 * second;
        p.closed_before_ns = closed * second;
        Check(publisher->PublishProgress(context, p) == 0, "publish");
    };
    auto write = [&](uint64_t revision, int64_t start, uint64_t bytes, bool metadata = false) {
        npm::NpmBasicResult row;
        row.session_id = 1;
        row.revision = revision;
        row.primary_label_id = 17;
        row.protocol_status = npm::NpmProtocolStatus::kUnknown;
        row.is_final = metadata;
        if (metadata) row.end_reason = npm::NpmSessionEndReason::kEof;
        row.period = npm::NpmBasicPeriodStats{
            start * second, (start + 30) * second, true, metadata ? uint64_t{0} : uint64_t{1}, 0, bytes, 0, bytes};
        std::shared_ptr<arrow::RecordBatch> batch;
        Check(npm::EncodeNpmBasicResults({row}, &batch, nullptr, true) == npm::NpmBasicEncodeError::kNone, "encode");
        Check(consumer->Consume(context, entity, *batch) == 0, "consume");
    };
    auto c = Config(category + "." + name, context.run_id);
    // Old metadata alone is insufficient even though the run is writing and table exists.
    {
        PollInput old;
        assert(old.Initialize(c, source, c.config.source) != 0);
    }
    publish(0);
    write(1, 0, 10);
    publish(30);
    PollInput input;
    Check(input.Initialize(c, source, c.config.source) == 0, input.LastError());
    auto p2 = Boundary(input);
    assert(p2.datasets.size() == 1 && p2.datasets[0].closed_before_bucket == 0);
    assert(input.PollBlock(0).kind == BlockPollEvent::kTimeout && !input.IsFinished());
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    write(2, 30, 20);
    publish(60);
    auto batch = input.PollBlock(0);
    Check(batch.kind == BlockPollEvent::kData && batch.batch->num_rows() == 1, input.LastError());
    assert(std::static_pointer_cast<arrow::DoubleArray>(batch.batch->GetColumnByName("value"))->Value(0) == 30);
    assert(std::static_pointer_cast<arrow::StringArray>(batch.batch->GetColumnByName("source_epoch"))->GetString(0) ==
           context.run_id);
    // Advance the writer while a frozen range is held by the reader; only position 3 may be confirmed.
    write(3, 60, 30);
    publish(90);
    Check(input.ReleaseBlock(batch.batch) == 0, "release data");
    auto p3 = Boundary(input);
    assert(p3.datasets[0].committed_position == "3" && p3.datasets[0].closed_before_bucket == 1);
    PollInput resumed;
    Check(resumed.Initialize(c, source, c.config.source, p3) == 0, resumed.LastError());
    auto p4 = Boundary(resumed);
    assert(p4.datasets[0].committed_position == "4" && p4.datasets[0].closed_before_bucket == 1);
    write(4, 90, 40);
    publish(120);
    std::vector<double> values;
    auto p5 = Boundary(resumed, &values);
    assert(values == std::vector<double>{70} && p5.datasets[0].closed_before_bucket == 2);
    write(5, 0, 0, true);  // zero-increment terminal metadata never reopens bucket 0
    publish(180);
    values.clear();
    auto p6 = Boundary(resumed, &values);
    assert(values.empty() && p6.datasets[0].closed_before_bucket == 3);
    // Same source at m=1 emits the original four period buckets; pagination does not change observations.
    auto aligned = Config(c.config.source, context.run_id, 30);
    aligned.config.read.page_rows = 3;
    {
        PollInput one;
        Check(one.Initialize(aligned, source, c.config.source) == 0, one.LastError());
        Boundary(one, &values);
        assert(values == std::vector<double>({10, 20, 30, 40}));
    }
    {
        auto bad = c;
        bad.config.bucket_seconds = 45;
        PollInput p;
        assert(p.Initialize(bad, source, c.config.source) != 0);
    }
    {
        auto bad = p3;
        bad.datasets[0].epoch = "other";
        PollInput p;
        assert(p.Initialize(c, source, c.config.source, bad) != 0);
    }
    {
        auto bad = p3;
        bad.datasets[0].committed_position = "999999";
        PollInput p;
        assert(p.Initialize(c, source, c.config.source, bad) != 0);
    }
    {
        auto bad = p3;
        bad.datasets[0].closed_before_bucket = 2;
        PollInput p;
        assert(p.Initialize(c, source, c.config.source, bad) != 0);
    }
    {
        auto small = c;
        small.config.read.max_pending_bytes = 128;
        PollInput p;
        const int rc = p.Initialize(small, source, c.config.source);
        assert(rc != 0 || p.PollBlock(0).kind == BlockPollEvent::kError);
    }
    {
        auto slow = c;
        slow.config.read.poll_interval_ms = 60000;
        PollInput p;
        Check(p.Initialize(slow, source, c.config.source, p6) == 0, p.LastError());
        assert(p.PollBlock(0).kind == BlockPollEvent::kTimeout);
        auto begin = std::chrono::steady_clock::now();
        std::thread waiter([&] { assert(p.PollBlock(5000).kind == BlockPollEvent::kCancelled); });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        p.Cancel();
        waiter.join();
        assert(std::chrono::steady_clock::now() - begin < std::chrono::seconds(1));
    }
    Check(consumer->Finish() == 0, "finish");
    const std::string expire = category == "clickhouse"
                                   ? "ALTER TABLE npm_result_runs UPDATE expires_at_ns=1 WHERE run_id='"
                                   : "UPDATE npm_result_runs SET expires_at_ns=1 WHERE run_id='";
    Check(
        source->ExecuteSql(
            (expire + context.run_id + "'" + (category == "clickhouse" ? " SETTINGS mutations_sync=2" : "")).c_str()) >=
            0,
        source->GetLastError());
    {
        PollInput expired;
        assert(expired.Initialize(c, source, c.config.source, p6) != 0);
    }
    assert(input.PollBlock(0).kind == BlockPollEvent::kError);  // never acknowledge the frozen obsolete source
    std::printf("PASS published/poll source: %s\n", category.c_str());
}
}  // namespace
int main() {
    const char* path = std::getenv("BASELINER_TEST_DB_OPTIONS");
    Check(path, "four real source options required");
    std::ifstream file(path);
    std::ostringstream options;
    options << file.rdbuf();
    std::string isolated_options = options.str();
    const auto sqlite = isolated_options.find("type=sqlite");
    const auto path_field = isolated_options.find("path=", sqlite);
    Check(sqlite != std::string::npos && path_field != std::string::npos, "sqlite test path required");
    const auto path_end = isolated_options.find_first_of(";|\n", path_field);
    isolated_options.replace(path_field, path_end - path_field,
                             "path=/tmp/baseline-operator-t3/poll-" +
                                 std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + ".db");
    database::DatabasePlugin plugin;
    const int saved = dup(STDOUT_FILENO);
    FILE* quiet = std::fopen("/dev/null", "w");
    dup2(fileno(quiet), STDOUT_FILENO);
    int rc = plugin.Option(isolated_options.c_str());
    std::fflush(stdout);
    dup2(saved, STDOUT_FILENO);
    close(saved);
    std::fclose(quiet);
    Check(rc == 0 && plugin.Load(nullptr) == 0 && plugin.Start() == 0, "database initialization");
    TestGeneric(plugin);
    Test(plugin, "sqlite", "t2sqlite");
    Test(plugin, "mysql", "t2mysql");
    Test(plugin, "postgres", "t2postgres");
    Test(plugin, "clickhouse", "t2clickhouse");
    plugin.Stop();
    plugin.Unload();
}
