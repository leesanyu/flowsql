// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <operators/baseliner/durable_store.h>
#include <operators/baseliner/evaluation.h>
#include <operators/baseliner/model_output.h>
#include <operators/baseliner/result_codec.h>
#include <plugins/baseline/baseline_plugin.h>
#include <services/database/database_plugin.h>
#include <unistd.h>
#include <cassert>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
using namespace flowsql;
using namespace flowsql::baseliner;
constexpr auto json = R"({"schema_version":1,"task_key":"evaluation","source":"sqlite.source","mode":"snapshot",
"clock":{"bucket_seconds":60,"timezone":"UTC"},"calendar":{"calendar_id":"cn-holiday","calendar_version":"2026.1"},
"datasets":[{"id":"d","table":"facts","fields":{"id":"uint64","t":"int64","v":"float64","n":"float64","den":"float64"},
"scope":{"begin_bucket":0,"end_bucket":10000,"consistency":"immutable_range"},"series_keys":["id"],
"deduplicate":{"keys":["id","t"],"on_duplicate":"require_equal"},"bucket":{"column":"t","unit":"bucket_id"},
"metrics":[{"id":"v","kind":"value","column":"v","feature_type":"value_basic","profile":"default"},
{"id":"r","kind":"ratio","feature_type":"ratio","profile":"rate_core","numerator":{"column":"n"},"denominator":{"column":"den"}}]}]})";
constexpr auto relation_json =
    R"({"schema_version":1,"task_key":"relation-evaluation","source":"sqlite.source","mode":"snapshot",
"clock":{"bucket_seconds":60,"timezone":"UTC"},"calendar":{"calendar_id":"cn-holiday","calendar_version":"2026.1"},
"datasets":[{"id":"d","table":"facts","fields":{"g":"uint64","t":"int64","x":"float64"},"scope":{"begin_bucket":0,"end_bucket":10000,"consistency":"immutable_range"},
"series_keys":["g"],"deduplicate":{"keys":["g","t"],"on_duplicate":"require_equal"},"bucket":{"column":"t","unit":"bucket_id"},
"metrics":[{"id":"dist","kind":"relation","feature_type":"relation","profile":"default","group_space":{"id":"groups","version":"v1","column":"g","unknown":"other","other_group_idx":99},
"metrics":[{"id":"m","column":"x","aggregate":"sum"}],"support_policy":{"k_support":3,"min_hist_share":0.01,"min_active_ratio":0.1},"summary_policy":{"k_head":2,"k_stable":1}}]}]})";
void ActualModels(IDatabaseChannel* channel) {
    baseline::BaselinePlugin plugin;
    assert(plugin.Option(nullptr) == 0 && plugin.Load(nullptr) == 0 && plugin.Start() == 0);
    {
        for (bool relation : {false, true}) {
            ConfigSnapshot c;
            std::string text = relation ? relation_json : json;
            text.replace(text.find("evaluation"), 10,
                         "store-actual-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
            assert(ParseConfig(text, &c).ok());
            c.config.horizon_buckets = 2;
            DurableStore store;
            assert(store.Open(c, channel, "actual") == 0);
            EvaluationEngine engine;
            assert(engine.Open(c, &plugin, &plugin, &plugin) == 0);
            std::vector<std::shared_ptr<arrow::RecordBatch>> results;
            for (int64_t bucket = 0; bucket < 40; ++bucket) {
                for (auto kind :
                     relation ? std::vector<BaselineTaskKind>{BaselineTaskKind::kRelation}
                              : std::vector<BaselineTaskKind>{BaselineTaskKind::kValue, BaselineTaskKind::kRatio}) {
                    Observation o;
                    o.dataset_id = "d";
                    o.kind = kind;
                    o.metric_id = kind == BaselineTaskKind::kRelation ? "dist"
                                  : kind == BaselineTaskKind::kValue  ? "v"
                                                                      : "r";
                    o.source_epoch = "source-1";
                    o.bucket = bucket;
                    assert(EncodeIdentity(c.config.source, "d", o.metric_id, kind, {uint64_t{1}}, &o.identity).ok());
                    if (kind == BaselineTaskKind::kValue) {
                        o.value = 100 + bucket % 3;
                        o.sample_count = 1;
                    } else if (kind == BaselineTaskKind::kRatio) {
                        o.numerator = 800 + bucket % 3;
                        o.denominator = 1000;
                    } else {
                        o.groups = {1, 2, 3};
                        o.metrics = {{"m", 1000. + bucket, 3, {500., 300., 200. + bucket}}};
                    }
                    EvaluationOutput output;
                    assert(engine.Submit(o, &output) == 0);
                    output.results.insert(output.results.end(), output.forecasts.begin(), output.forecasts.end());
                    std::shared_ptr<arrow::RecordBatch> batch;
                    std::string error;
                    if (!output.results.empty()) {
                        assert(EncodeResults(c, output.results, 1 << 24, &batch, &error) == 0);
                        results.push_back(batch);
                    }
                    if (output.relation) {
                        assert(EncodeFusion(c, o, *output.relation, 1 << 24, &batch, &error) == 0);
                        results.push_back(batch);
                    }
                }
            }
            std::string checkpoint;
            assert(engine.ExportCheckpoint(&checkpoint) == 0 && checkpoint.find("\"model\"") != std::string::npos);
            assert(store.Publish(checkpoint, {}, results) == 0);
            StoredGeneration restored;
            assert(store.Load(&restored) == 0 && restored.checkpoint == checkpoint && restored.generation == 1);
            std::shared_ptr<IDatabaseAtomicSessionV1> observer;
            assert(dynamic_cast<IDatabaseAtomicTargetV1*>(channel)->AcquireAtomicSession({}, &observer).ok());
            std::shared_ptr<arrow::RecordBatch> count;
            assert(ReadAtomicBatch(
                       *observer,
                       "SELECT COUNT(*) AS n FROM baseline_results_v1 WHERE task_key=? AND result_kind='forecast'",
                       {TextParameter(c.config.task_key)}, &count)
                       .ok());
            assert(std::static_pointer_cast<arrow::Int64Array>(count->column(0))->Value(0) > 0);
            if (relation) {
                assert(ReadAtomicBatch(*observer,
                                       "SELECT COUNT(*) AS n FROM baseline_relation_fusion_v1 WHERE task_key=?",
                                       {TextParameter(c.config.task_key)}, &count)
                           .ok());
                assert(std::static_pointer_cast<arrow::Int64Array>(count->column(0))->Value(0) == 40);
            }
            // A duplicate logical result causes complete rollback, including the staged current generation.
            assert(store.Publish(checkpoint, {}, results) != 0);
            assert(ReadAtomicBatch(*observer, "SELECT current_generation FROM baseline_tasks WHERE task_key=?",
                                   {TextParameter(c.config.task_key)}, &count)
                       .ok());
            assert(std::static_pointer_cast<arrow::Int64Array>(count->column(0))->Value(0) == 1);
            assert(ReadAtomicBatch(*observer, "SELECT COUNT(*) AS n FROM baseline_model_versions WHERE task_key=?",
                                   {TextParameter(c.config.task_key)}, &count)
                       .ok());
            assert(std::static_pointer_cast<arrow::Int64Array>(count->column(0))->Value(0) == 1);
            observer->Close();
            store.Close();
            engine.Close();
        }
    }
    plugin.Stop();
    plugin.Unload();
}
void ModelTable(const std::shared_ptr<IDatabaseChannel>& channel, IDatabaseChannel* cross_schema_source) {
    ConfigSnapshot config;
    assert(ParseConfig(json, &config).ok());
    const auto unique = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto table = "t4_models_" + unique;
    std::vector<ModelParametersRow> rows;
    for (int i = 0; i < 2; ++i) {
        ModelParametersRow row;
        row.identity.dataset_id = "d";
        row.identity.metric_id = "v";
        row.identity.identity = std::string("binary\0key", 10) + std::to_string(i);
        row.identity.source_epoch = "epoch";
        row.model_basis_id = "basis";
        row.as_of_bucket = 4;
        row.parameters.status = BaselineStatus::kOk;
        row.parameters.maturity = "level_ready";
        row.parameters.parameters_json = R"({"parameters_version":1,"theta":{"level":4.61512051684126}})";
        rows.push_back(std::move(row));
    }
    std::shared_ptr<arrow::RecordBatch> batch;
    std::string error;
    assert(EncodeModelParameters(config, rows, 1 << 20, &batch, &error) == 0);
    auto count = [&](const std::string& name) {
        std::shared_ptr<IDatabaseAtomicSessionV1> session;
        assert(dynamic_cast<IDatabaseAtomicTargetV1*>(channel.get())->AcquireAtomicSession({}, &session).ok());
        std::shared_ptr<arrow::RecordBatch> result;
        assert(ReadAtomicBatch(*session, "SELECT COUNT(*) FROM " + name, {}, &result).ok());
        auto n = std::stoll(result->column(0)->GetScalar(0).ValueOrDie()->ToString());
        session->Close();
        return n;
    };
    {
        ModelOutputStore store;
        const int opened = store.Open(config, channel, table);
        if (opened) std::cerr << "model target " << channel->Category() << ": " << store.LastError() << '\n';
        assert(opened == 0);
        assert(store.Write(batch) == 0 && store.RowsWritten() == 2 && store.State() == "committed");
    }
    {
        ModelOutputStore replay;
        assert(replay.Open(config, channel, table) == 0 && replay.Write(batch) == 0);
        assert(count(table) == 2);  // Same logical keys are idempotent.
    }
    {
        ModelOutputStore protected_source;
        assert(protected_source.Open(config, channel, table) == 0);
        Dataset input;
        input.table = table;
        if (std::string(channel->Category()) == "sqlite")
            for (auto& c : input.table) c = std::toupper(static_cast<unsigned char>(c));
        assert(protected_source.ValidateRelations(channel.get(), {input}) != 0 && count(table) == 2);
    }
    if (cross_schema_source) {
        ModelOutputStore protected_schema;
        assert(protected_schema.Open(config, channel, table) == 0);
        Dataset input;
        input.table = table;
        input.schema = protected_schema.DefaultSchema();
        assert(protected_schema.ValidateRelations(cross_schema_source, {input}) != 0 && count(table) == 2);
        ModelOutputStore different_schema;
        assert(different_schema.Open(config, channel, table) == 0);
        input.schema.clear();
        assert(different_schema.ValidateRelations(cross_schema_source, {input}) == 0);
        std::cout << "PASS MySQL explicit source schema across different connection databases\n";
    }
    config.config.task_key = "unrelated";
    std::shared_ptr<arrow::RecordBatch> unrelated;
    auto other = rows;
    other.resize(1);
    other[0].identity.dataset_id = "unrelated";
    other[0].as_of_bucket = 5;
    assert(EncodeModelParameters(config, other, 1 << 20, &unrelated, &error) == 0);
    {
        ModelOutputStore extra;
        assert(extra.Open(config, channel, table) == 0 && extra.Write(unrelated) == 0 && count(table) == 3);
    }
    {
        ModelOutputStore cancelled;
        assert(cancelled.Open(config, channel, table) == 0);
        cancelled.Cancel();
        assert(cancelled.Write(batch) != 0 && count(table) == 3);
    }
    const auto conflict = "t4_conflict_" + unique;
    assert(channel->ExecuteSql(("CREATE TABLE " + conflict + "(x BIGINT)").c_str()) >= 0);
    ModelOutputStore bad;
    assert(bad.Open(config, channel, conflict) != 0 && count(conflict) == 0);
    // A real constraint error after the first INSERT rolls back the whole model publication.
    const auto atomic_table = "t4_atomic_" + unique;
    {
        ModelOutputStore seed;
        assert(seed.Open(config, channel, atomic_table) == 0 && seed.Write(unrelated) == 0);
    }
    assert(channel->ExecuteSql(
               ("CREATE UNIQUE INDEX t4_unique_" + unique + " ON " + atomic_table + "(as_of_bucket)").c_str()) >= 0);
    ModelOutputStore failed;
    assert(failed.Open(config, channel, atomic_table) == 0);
    assert(failed.Write(batch) != 0 && failed.RowsWritten() == 0 && count(atomic_table) == 1);
    std::cout << "PASS model table " << channel->Category()
              << " schema, replay, preserve, cancel and transaction rollback\n";
}
void ResultTables(const std::shared_ptr<IDatabaseChannel>& channel) {
    ConfigSnapshot config;
    assert(ParseConfig(json, &config).ok());
    const auto unique = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto table = "t5_results_" + unique, models = "t5_models_" + unique;
    EvaluationRow row;
    row.input.dataset_id = "d";
    row.input.metric_id = "v";
    row.input.identity = std::string("key\0binary", 10);
    row.input.source_epoch = "epoch";
    row.values.target_bucket = row.values.issued_after_bucket = 4;
    row.values.observed = 3.14;
    row.values.status = BaselineStatus::kNotTrained;
    std::shared_ptr<arrow::RecordBatch> batch, parameters;
    std::string error;
    assert(EncodeResults(config, {row}, 1 << 20, &batch, &error) == 0);
    std::vector<ModelParametersRow> model_rows(2);
    for (int i = 0; i < 2; ++i) {
        model_rows[i].identity = row.input;
        model_rows[i].identity.metric_id = "m" + std::to_string(i);
        model_rows[i].model_basis_id = "basis";
        model_rows[i].as_of_bucket = 4;
        model_rows[i].parameters.status = BaselineStatus::kNotTrained;
    }
    assert(EncodeModelParameters(config, model_rows, 1 << 20, &parameters, &error) == 0);
    auto count = [&](const std::string& name) {
        std::shared_ptr<IDatabaseAtomicSessionV1> session;
        assert(dynamic_cast<IDatabaseAtomicTargetV1*>(channel.get())->AcquireAtomicSession({}, &session).ok());
        std::shared_ptr<arrow::RecordBatch> result;
        assert(ReadAtomicBatch(*session, "SELECT COUNT(*) FROM " + name, {}, &result).ok());
        const auto n = std::stoll(result->column(0)->GetScalar(0).ValueOrDie()->ToString());
        session->Close();
        return n;
    };
    for (int replay = 0; replay < 2; ++replay) {
        ModelOutputStore result, model;
        assert(result.Open(config, channel, table, SchemaKind::kResults) == 0);
        assert(model.Open(config, channel, models) == 0);
        assert(result.Join(model) == 0 && result.SharesSession(model));
        assert(result.Write({batch}, &model, parameters) == 0);
        assert(result.RowsWritten() == 1 && model.RowsWritten() == 2 && count(table) == 1 && count(models) == 2);
    }
    const auto unrelated_table = table;
    auto other = row;
    other.input.metric_id = "other";
    assert(EncodeResults(config, {other}, 1 << 20, &batch, &error) == 0);
    {
        ModelOutputStore result;
        assert(result.Open(config, channel, unrelated_table, SchemaKind::kResults) == 0 && result.Write(batch) == 0);
        assert(count(table) == 2);
    }
    // A real second-table constraint failure must undo first-table DELETE/INSERT as well.
    // Use a fresh empty table for failure injection.
    const auto failed_models = models + "_fail";
    {
        ModelOutputStore seed;
        assert(seed.Open(config, channel, failed_models) == 0);
    }
    assert(channel->ExecuteSql(
               ("CREATE UNIQUE INDEX t5_fail_" + unique + " ON " + failed_models + "(as_of_bucket)").c_str()) >= 0);
    other.values.observed = 99;
    assert(EncodeResults(config, {other}, 1 << 20, &batch, &error) == 0);
    ModelOutputStore result, model;
    assert(result.Open(config, channel, table, SchemaKind::kResults) == 0 &&
           model.Open(config, channel, failed_models) == 0);
    assert(result.Join(model) == 0 && result.Write({batch}, &model, parameters) != 0);
    assert(result.State() == "failed" && model.State() == "failed" && result.RowsWritten() == 0 &&
           model.RowsWritten() == 0);
    assert(count(table) == 2 && count(failed_models) == 0);
    std::shared_ptr<IDatabaseAtomicSessionV1> observer;
    assert(dynamic_cast<IDatabaseAtomicTargetV1*>(channel.get())->AcquireAtomicSession({}, &observer).ok());
    std::shared_ptr<arrow::RecordBatch> preserved;
    assert(
        ReadAtomicBatch(*observer, "SELECT observed FROM " + table + " WHERE metric_id='other'", {}, &preserved).ok());
    assert(preserved->column(0)->GetScalar(0).ValueOrDie()->ToString() == "3.14");
    observer->Close();
    ModelOutputStore alias;
    assert(alias.Open(config, channel, table, SchemaKind::kResults) == 0 && result.Join(alias) != 0);
    if (std::string(channel->Category()) == "mysql") {
        const auto nontransactional = table + "_myisam";
        {
            ModelOutputStore seed;
            assert(seed.Open(config, channel, nontransactional, SchemaKind::kResults) == 0);
        }
        assert(channel->ExecuteSql(("ALTER TABLE " + nontransactional + " ENGINE=MyISAM").c_str()) >= 0);
        ModelOutputStore reject;
        assert(reject.Open(config, channel, nontransactional, SchemaKind::kResults) != 0);
        assert(reject.LastError().find("InnoDB") != std::string::npos);
    }
    std::cout << "PASS specified result table " << channel->Category()
              << " replay, unrelated rows and joint rollback\n";
}
void Test(std::shared_ptr<IDatabaseChannel> channel, IDatabaseChannel* cross_schema_source = nullptr) {
    assert(channel && channel->IsOpened() && channel->IsConnected());
    ModelTable(channel, cross_schema_source);
    ResultTables(channel);
    const std::string backend = channel->Category();
    ConfigSnapshot c;
    c.config.task_key = "store-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    c.sha256_hex = "store-config";
    c.original_json = "{}";
    c.config.persistence.retain_generations = 2;
    Dataset dataset;
    dataset.id = "d";
    c.config.datasets = {dataset};
    DurableStore store;
    {
        int rc = store.Open(c, channel.get(), "owner");
        if (rc) std::cerr << backend << ": " << store.LastError() << std::endl;
        assert(rc == 0);
    }
    StoredGeneration restored;
    assert(store.Load(&restored) == 0 && restored.generation == 0);
    auto* target = dynamic_cast<IDatabaseAtomicTargetV1*>(channel.get());
    assert(target);
    std::shared_ptr<IDatabaseAtomicSessionV1> observer;
    assert(target->AcquireAtomicSession({}, &observer).ok());
    assert(channel->ExecuteSql("CREATE TABLE IF NOT EXISTS source_facts(id BIGINT PRIMARY KEY,v TEXT)") >= 0);
    assert(channel->ExecuteSql("DELETE FROM source_facts") >= 0);
    assert(channel->ExecuteSql("INSERT INTO source_facts VALUES(1,'unchanged')") >= 0);
    for (int64_t generation = 1; generation <= 4; ++generation) {
        EvaluationRow row;
        row.input.dataset_id = "d";
        row.input.metric_id = "m";
        row.input.identity = "identity";
        row.input.source_epoch = "source-1";
        row.values.target_bucket = row.values.issued_after_bucket = generation;
        row.values.observed = 100.;
        row.model_basis_id = "basis-before-" + std::to_string(generation);
        row.values.status = BaselineStatus::kNotTrained;
        auto forecast = row;
        forecast.values.forecast = true;
        forecast.values.observed.reset();
        forecast.values.target_bucket = 6;
        forecast.model_basis_id = "basis-after-" + std::to_string(generation);
        std::shared_ptr<arrow::RecordBatch> batch;
        std::string error;
        assert(EncodeResults(c, {row, forecast}, 1 << 20, &batch, &error) == 0);
        BlockInputProgressV1 p;
        p.datasets = {{"d", "source-1", std::to_string(generation), generation + 1}};
        assert(store.Publish("checkpoint-" + std::to_string(generation), p, {batch}) == 0);
        assert(store.Load(&restored) == 0 && restored.generation == generation &&
               restored.checkpoint == "checkpoint-" + std::to_string(generation));
        assert(restored.progress.datasets[0].committed_position == std::to_string(generation));
    }
    std::shared_ptr<arrow::RecordBatch> rows;
    assert(ReadAtomicBatch(*observer, "SELECT COUNT(*) AS n FROM baseline_results_v1 WHERE task_key=?",
                           {TextParameter(c.config.task_key)}, &rows)
               .ok());
    assert(std::static_pointer_cast<arrow::Int64Array>(rows->column(0))->Value(0) == 8);
    assert(ReadAtomicBatch(*observer, "SELECT COUNT(*) AS n FROM baseline_model_versions WHERE task_key=?",
                           {TextParameter(c.config.task_key)}, &rows)
               .ok());
    assert(std::static_pointer_cast<arrow::Int64Array>(rows->column(0))->Value(0) == 2);
    assert(ReadAtomicBatch(*observer, "SELECT v FROM source_facts", {}, &rows).ok());
    assert(std::static_pointer_cast<arrow::StringArray>(rows->column(0))->GetString(0) == "unchanged");
    assert(
        ReadAtomicBatch(*observer,
                        "SELECT issued_after_bucket,observed,model_basis_id,published_generation FROM "
                        "baseline_results_v1 WHERE task_key=? AND result_kind='forecast' ORDER BY issued_after_bucket",
                        {TextParameter(c.config.task_key)}, &rows)
            .ok());
    assert(rows->num_rows() == 4);
    for (int64_t i = 0; i < 4; ++i) {
        assert(std::static_pointer_cast<arrow::Int64Array>(rows->column(0))->Value(i) == i + 1);
        assert(rows->column(1)->IsNull(i));
        assert(std::static_pointer_cast<arrow::StringArray>(rows->column(2))->GetString(i) ==
               "basis-after-" + std::to_string(i + 1));
        assert(rows->column(3)->GetScalar(i).ValueOrDie()->ToString() == std::to_string(i + 1));
    }
    assert(ReadAtomicBatch(*observer,
                           "SELECT generation FROM baseline_model_versions WHERE task_key=? ORDER BY generation",
                           {TextParameter(c.config.task_key)}, &rows)
               .ok());
    assert(rows->num_rows() == 2 &&
           std::static_pointer_cast<arrow::Int64Array>(rows->column(0))->Value(1) == store.Generation());
    ActualModels(channel.get());
    store.Close();
    observer->Close();
    observer.reset();
    channel.reset();

    std::cout << "store SQLite passed\n";
}

int main() {
    const char* path = std::getenv("BASELINER_TEST_DB_OPTIONS");
    assert(path);
    std::ifstream input(path);
    std::ostringstream options;
    options << input.rdbuf();
    // A read-only alternate default database on the same server exercises explicit source schema.
    std::istringstream channels(options.str());
    std::string channel_options;
    while (std::getline(channels, channel_options, '|')) {
        if (channel_options.find("type=mysql;") == std::string::npos) continue;
        while (!channel_options.empty() && (channel_options.back() == '\n' || channel_options.back() == '\r'))
            channel_options.pop_back();
        options << '|' << channel_options << ";name=t4mysqlidentity;database=information_schema";
        break;
    }
    database::DatabasePlugin db;
    int saved = dup(STDOUT_FILENO);
    FILE* quiet = fopen("/dev/null", "w");
    dup2(fileno(quiet), STDOUT_FILENO);
    int rc = db.Option(options.str().c_str());
    fflush(stdout);
    dup2(saved, STDOUT_FILENO);
    close(saved);
    fclose(quiet);
    assert(rc == 0);
    assert(db.Load(nullptr) == 0 && db.Start() == 0);
    auto cross_schema_source = db.AcquireChannel("mysql", "t4mysqlidentity");
    assert(cross_schema_source);
    for (auto backend : {"sqlite", "mysql", "postgres"})
        Test(db.AcquireChannel(backend, ("t2" + std::string(backend)).c_str()),
             std::string(backend) == "mysql" ? cross_schema_source.get() : nullptr);
    cross_schema_source.reset();
    db.Stop();
    db.Unload();
}
