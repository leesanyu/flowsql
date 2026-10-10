// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <arrow/api.h>
#include <operators/baseliner/snapshot_input.h>
#include <operators/baseliner/snapshot_reader.h>
#include <services/database/database_plugin.h>
#include <unistd.h>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>
#include <thread>

using namespace flowsql;
using namespace flowsql::baseliner;
namespace {
void Check(bool ok, const std::string& error) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", error.c_str());
        std::abort();
    }
}
TaskConfig Config(const std::string& category, const std::string& name, const std::string& table) {
    TaskConfig c;
    c.source = category + "." + name;
    c.task_key = "snapshot";
    c.read.page_rows = 1;
    Dataset d;
    d.id = "rows";
    d.table = table;
    d.fields = {{"bucket", LogicalType::kInt64}, {"id", LogicalType::kUInt64}, {"value", LogicalType::kFloat64}};
    d.scope.begin_bucket = -1;
    d.scope.end_bucket = 3;
    d.scope.consistency = category == "clickhouse" ? "immutable_range" : "consistent_snapshot";
    d.series_keys = {"id"};
    d.deduplicate_keys = {"id"};
    d.bucket_column = "bucket";
    c.datasets = {d};
    return c;
}
void Test(database::DatabasePlugin& plugin, const std::string& category, const std::string& name) {
    auto source = plugin.AcquireChannel(category.c_str(), name.c_str());
    Check(source && source->IsOpened() && source->IsConnected(), "required backend unavailable: " + category);
    const std::string table = "baseliner_t2_" + std::to_string(getpid());
    const std::string view = table + "_view";
    const std::string uinttype = category == "sqlite"     ? "TEXT"
                                 : category == "mysql"    ? "BIGINT UNSIGNED"
                                 : category == "postgres" ? "NUMERIC(20,0)"
                                                          : "UInt64";
    const std::string realtype = category == "clickhouse" ? "Float64" : "DOUBLE PRECISION";
    const std::string inttype = category == "clickhouse" ? "Int64" : "BIGINT";
    Check(
        source->ExecuteSql(("CREATE TABLE " + table + " (bucket " + inttype + ",id " + uinttype + ",value " + realtype +
                            ")" + (category == "clickhouse" ? " ENGINE=MergeTree ORDER BY (bucket,id)" : ""))
                               .c_str()) >= 0,
        source->GetLastError());
    Check(source->ExecuteSql(
              ("INSERT INTO " + table + " VALUES(-1,'18446744073709551615',2.5),(0,'1',3.5),(2,'2',4.5)").c_str()) >= 0,
          source->GetLastError());
    Check(source->ExecuteSql(("CREATE VIEW " + view + " AS SELECT * FROM " + table).c_str()) >= 0,
          source->GetLastError());
    auto c = Config(category, name, view);
    {
        SnapshotReader r;
        Check(r.Open(c, source, c.source) == 0, r.LastError());
        SnapshotPage page;
        Check(r.Next(&page) == 0 && page.batch->num_rows() == 1, r.LastError());
        assert(std::static_pointer_cast<arrow::UInt64Array>(page.batch->GetColumnByName("id"))->Value(0) ==
               std::numeric_limits<uint64_t>::max());
        if (category != "clickhouse")
            Check(source->ExecuteSql(("INSERT INTO " + table + " VALUES(1,'3',5.5)").c_str()) >= 0,
                  source->GetLastError());
        uint64_t count = 1;
        while (true) {
            Check(r.Next(&page) == 0, r.LastError());
            if (page.eof) break;
            count += page.batch->num_rows();
        }
        assert(count == 3);
        Check(r.Next(&page) == 0 && page.eof, r.LastError());
    }
    {
        auto read = [&](const TaskConfig& config) {
            SnapshotReader reader;
            Check(reader.Open(config, source, config.source) == 0, reader.LastError());
            std::string rows;
            SnapshotPage page;
            for (;;) {
                Check(reader.Next(&page) == 0, reader.LastError());
                if (page.eof) return rows;
                rows += page.batch->ToString();
            }
        };
        auto exact_table = c;
        exact_table.source += "." + view;
        exact_table.source_relation = view;
        Check(read(exact_table) == read(c), "two/three-part snapshot data differs");
        auto wrong_table = exact_table;
        wrong_table.datasets[0].table = table;
        SnapshotReader wrong;
        Check(wrong.Open(wrong_table, source, wrong_table.source) != 0, "three-part source read another relation");
        SnapshotReader mismatch;
        Check(mismatch.Open(exact_table, source, c.source) != 0, "exact SQL source mismatch accepted");
    }
    {
        // Equal pagination keys span pages; differing payloads must remain visible.
        Check(source->ExecuteSql(("INSERT INTO " + table + " VALUES(0,'1',7.5),(0,'1',3.5)").c_str()) >= 0,
              source->GetLastError());
        auto tied = c;
        tied.read.page_rows = 2;
        SnapshotReader r;
        Check(r.Open(tied, source, c.source) == 0, r.LastError());
        SnapshotPage page;
        uint64_t count = 0, ones = 0;
        while (true) {
            Check(r.Next(&page) == 0, r.LastError());
            if (page.eof) break;
            count += page.batch->num_rows();
            auto ids = std::static_pointer_cast<arrow::UInt64Array>(page.batch->GetColumnByName("id"));
            for (int64_t row = 0; row < ids->length(); ++row)
                if (ids->Value(row) == 1) ++ones;
        }
        assert(count == (category == "clickhouse" ? 5 : 6) && ones == 3);
    }
    {
        const std::string versions = table + "_versions";
        Check(source->ExecuteSql(("CREATE TABLE " + versions + "(bucket " + inttype + ",id " + uinttype + ",value " +
                                  realtype + ",rev " + uinttype + ")" +
                                  (category == "clickhouse" ? " ENGINE=MergeTree ORDER BY (bucket,id)" : ""))
                                     .c_str()) >= 0,
              source->GetLastError());
        Check(source->ExecuteSql(("INSERT INTO " + versions +
                                  " VALUES(0,'1',1,'9'),(1,'1',2,'10'),(1,'1',3,'10'),(2,'2',4,'18446744073709551615')")
                                     .c_str()) >= 0,
              source->GetLastError());
        auto latest = c;
        auto& d = latest.datasets[0];
        d.table = versions;
        d.fields["rev"] = LogicalType::kUInt64;
        d.row_semantics = "latest_revision";
        d.revision_column = "rev";
        {
            SnapshotReader r;
            Check(r.Open(latest, source, c.source) == 0, r.LastError());
            SnapshotPage page;
            uint64_t count = 0;
            while (true) {
                Check(r.Next(&page) == 0, r.LastError());
                if (page.eof) break;
                auto revision = std::static_pointer_cast<arrow::UInt64Array>(page.batch->GetColumnByName("rev"));
                assert(revision->Value(0) != 9);
                count += page.batch->num_rows();
            }
            assert(count == 3);
        }
        {
            ConfigSnapshot snapshot;
            snapshot.sha256_hex = "revision-conflict";
            snapshot.config = latest;
            Metric m;
            m.id = "value";
            m.value = {"value", Aggregate::kSum};
            snapshot.config.datasets[0].metrics = {m};
            SnapshotInput input;
            Check(input.Initialize(snapshot, source, c.source) == 0, input.LastError());
            auto event = input.PollBlock();
            assert(event.kind == BlockPollEvent::kError);
        }
        // Scope/filter apply before selecting the maximum revision.
        d.scope.end_bucket = 1;
        {
            SnapshotReader r;
            Check(r.Open(latest, source, c.source) == 0, r.LastError());
            SnapshotPage page;
            Check(r.Next(&page) == 0 && page.batch->num_rows() == 1, r.LastError());
            assert(std::static_pointer_cast<arrow::UInt64Array>(page.batch->GetColumnByName("rev"))->Value(0) == 9);
        }
        // Equal maximum revision with conflicting bucket must fail even after the earlier bucket closed.
        Check(source->ExecuteSql(("INSERT INTO " + versions + " VALUES(2,'1',2,'10')").c_str()) >= 0,
              source->GetLastError());
        d.scope.end_bucket = 3;
        {
            ConfigSnapshot snapshot;
            snapshot.sha256_hex = "revision-cross-bucket";
            snapshot.config = latest;
            snapshot.config.datasets[0].filter = {{"value", "eq", 2.0}};
            Metric m;
            m.id = "value";
            m.value = {"value", Aggregate::kSum};
            snapshot.config.datasets[0].metrics = {m};
            SnapshotInput input;
            Check(input.Initialize(snapshot, source, c.source) == 0, input.LastError());
            bool failed = false;
            for (;;) {
                auto event = input.PollBlock();
                if (event.kind == BlockPollEvent::kError) {
                    failed = true;
                    break;
                }
                if (event.kind == BlockPollEvent::kEof) break;
                Check(event.kind == BlockPollEvent::kData, input.LastError());
                assert(input.ReleaseBlock(event.batch) == 0);
            }
            assert(failed);
        }
        Check(source->ExecuteSql(("DROP TABLE " + versions).c_str()) >= 0, source->GetLastError());
    }
    {
        const std::string case_table = table + "_case";
        const std::string text = category == "mysql"    ? "VARCHAR(64) CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci"
                                 : category == "sqlite" ? "TEXT COLLATE NOCASE"
                                 : category == "clickhouse" ? "String"
                                                            : "TEXT";
        Check(source->ExecuteSql(("CREATE TABLE " + case_table + "(bucket " + inttype + ",id " + uinttype + ",value " +
                                  realtype + ",rev " + uinttype + ",label " + text + ")" +
                                  (category == "clickhouse" ? " ENGINE=MergeTree ORDER BY (bucket,label)" : ""))
                                     .c_str()) >= 0,
              source->GetLastError());
        Check(source->ExecuteSql(
                  ("INSERT INTO " + case_table + " VALUES(0,'1',1,'2','A'),(0,'2',2,'1','a')").c_str()) >= 0,
              source->GetLastError());
        auto config = c;
        auto& d = config.datasets[0];
        d.table = case_table;
        d.fields["label"] = LogicalType::kUtf8;
        d.fields["rev"] = LogicalType::kUInt64;
        d.deduplicate_keys = {"label"};
        d.series_keys = {"label"};
        d.row_semantics = "latest_revision";
        d.revision_column = "rev";
        {
            SnapshotReader r;
            Check(r.Open(config, source, c.source) == 0, r.LastError());
            SnapshotPage page;
            uint64_t count = 0;
            for (;;) {
                Check(r.Next(&page) == 0, r.LastError());
                if (page.eof) break;
                count += page.batch->num_rows();
            }
            assert(count == 2);
        }
        Check(source->ExecuteSql(("DROP TABLE " + case_table).c_str()) >= 0, source->GetLastError());
    }
    {
        auto empty = c;
        empty.datasets[0].scope.begin_bucket = 100;
        empty.datasets[0].scope.end_bucket = 101;
        SnapshotReader r;
        Check(r.Open(empty, source, c.source) == 0, r.LastError());
        SnapshotPage p;
        assert(r.Next(&p) == 0 && p.eof && p.batch->schema()->num_fields() == 3);
    }
    {
        auto filtered = c;
        filtered.datasets[0].filter = {{"id", "gt", uint64_t{3}}};
        SnapshotReader r;
        Check(r.Open(filtered, source, c.source) == 0, r.LastError());
        SnapshotPage page;
        assert(r.Next(&page) == 0 && page.batch->num_rows() == 1);
        assert(std::static_pointer_cast<arrow::UInt64Array>(page.batch->GetColumnByName("id"))->Value(0) == UINT64_MAX);
        assert(r.Next(&page) == 0 && page.eof);
    }
    {
        auto wrong = c;
        wrong.datasets[0].fields["value"] = LogicalType::kUtf8;
        SnapshotReader r;
        assert(r.Open(wrong, source, c.source) != 0);
    }
    {
        auto missing = c;
        missing.datasets[0].fields["absent"] = LogicalType::kInt64;
        SnapshotReader r;
        assert(r.Open(missing, source, c.source) != 0);
    }
    {
        auto missing = c;
        missing.datasets[0].table += "_absent";
        SnapshotReader r;
        assert(r.Open(missing, source, c.source) != 0);
    }
    {
        SnapshotReader r;
        Check(r.Open(c, source, c.source) == 0, r.LastError());
        r.Cancel();
        SnapshotPage p;
        assert(r.Next(&p) != 0 && !p.batch);
    }
    {
        auto bounded = c;
        bounded.read.max_pending_bytes = 32;
        SnapshotReader r;
        const auto rc = r.Open(bounded, source, c.source);
        SnapshotPage p;
        if (rc == 0)
            assert(r.Next(&p) != 0 && !p.batch);
        else
            assert(!r.LastError().empty());
    }
    {
        auto* cap = dynamic_cast<IDatabaseSnapshotSourceV1*>(source.get());
        assert(cap);
        DatabaseSnapshotOptionsV1 options;
        options.consistent_transaction = category != "clickhouse";
        std::shared_ptr<IDatabaseSnapshotSessionV1> session;
        assert(cap->CreateSnapshotSession(options, &session) == 0);
        std::shared_ptr<arrow::RecordBatch> p;
        auto schema = arrow::schema({arrow::field("id", arrow::uint64())});
        const auto sql = "SELECT id FROM " + table + " WHERE id=? LIMIT 1";
        DatabaseParameterV1 param;
        param.kind = DatabaseParameterKindV1::kUInt64;
        param.uint64_value = UINT64_MAX;
        Check(session->ReadPage(sql.c_str(), &param, 1, schema, 1, 1024, &p) == 0, session->LastError());
        assert(p->num_rows() == 1);
        std::thread cancel([&] { session->Cancel(); });
        cancel.join();
        assert(session->ReadPage(sql.c_str(), &param, 1, schema, 1, 1024, &p) != 0 && !p);
    }
    if (category == "clickhouse") {
        auto invalid = c;
        invalid.datasets[0].scope.consistency = "consistent_snapshot";
        SnapshotReader r;
        assert(r.Open(invalid, source, c.source) != 0);
    }

    {
        const std::string texttype = category == "clickhouse" ? "String"
                                     : category == "mysql"    ? "VARCHAR(128)"
                                                              : "TEXT";
        // Match the actual managed run catalog so finite and live producer tests can share a source.
        std::string runs_ddl =
            "CREATE TABLE IF NOT EXISTS npm_result_runs(run_id TEXT PRIMARY KEY NOT NULL,task_id TEXT NOT NULL,"
            "input_namespace TEXT NOT NULL,status TEXT NOT NULL,started_at_ns BIGINT NOT NULL,completed_at_ns BIGINT,"
            "expires_at_ns BIGINT,error_code BIGINT,error_stage TEXT,error_message TEXT,metadata_status TEXT NOT NULL,"
            "purge_cursor TEXT)";
        if (category == "sqlite")
            runs_ddl =
                "CREATE TABLE IF NOT EXISTS npm_result_runs(run_id TEXT PRIMARY KEY NOT NULL,task_id TEXT NOT NULL,"
                "input_namespace TEXT NOT NULL,status TEXT NOT NULL,started_at_ns INTEGER NOT NULL,completed_at_ns "
                "INTEGER,"
                "expires_at_ns INTEGER,error_code INTEGER,error_stage TEXT,error_message TEXT,metadata_status TEXT NOT "
                "NULL,"
                "purge_cursor TEXT)";
        else if (category == "mysql")
            runs_ddl =
                "CREATE TABLE IF NOT EXISTS npm_result_runs(run_id VARCHAR(128) PRIMARY KEY NOT NULL,"
                "task_id VARCHAR(128) NOT NULL,input_namespace VARCHAR(512) NOT NULL,status VARCHAR(16) NOT NULL,"
                "started_at_ns BIGINT NOT NULL,completed_at_ns BIGINT,expires_at_ns BIGINT,error_code BIGINT,"
                "error_stage VARCHAR(64),error_message TEXT,metadata_status VARCHAR(16) NOT NULL,purge_cursor "
                "VARCHAR(128)) ENGINE=InnoDB";
        else if (category == "clickhouse")
            runs_ddl =
                "CREATE TABLE IF NOT EXISTS npm_result_runs(run_id String,task_id String,input_namespace String,"
                "status String,started_at_ns Int64,completed_at_ns Nullable(Int64),expires_at_ns Nullable(Int64),"
                "error_code Nullable(Int64),error_stage Nullable(String),error_message Nullable(String),"
                "metadata_status String,purge_cursor Nullable(String)) ENGINE=MergeTree ORDER BY run_id";
        Check(source->ExecuteSql(runs_ddl.c_str()) >= 0, source->GetLastError());
        auto* commands = dynamic_cast<IDatabasePreparedCommandV1*>(source.get());
        assert(commands);
        auto stringparam = [](const std::string& v) {
            DatabaseParameterV1 p;
            p.kind = DatabaseParameterKindV1::kString;
            p.data = v.data();
            p.size = v.size();
            return p;
        };
        std::vector<std::string> runs;
        for (const auto& state : {"completed", "writing", "incomplete", "purging", "expired", "unknown"}) {
            const std::string run = table + "_" + state;
            runs.push_back(run);
            const std::string status =
                std::string(state) == "expired" || std::string(state) == "unknown" ? "completed" : state;
            const std::string metadata = std::string(state) == "unknown" ? "unknown" : "known";
            DatabaseParameterV1 started;
            started.kind = DatabaseParameterKindV1::kInt64;
            DatabaseParameterV1 expiry;
            if (std::string(state) == "expired") {
                expiry.kind = DatabaseParameterKindV1::kInt64;
                expiry.int64_value = 1;
            }
            DatabaseParameterV1 params[] = {
                stringparam(run), stringparam(table),   stringparam(table), stringparam(status), started,
                expiry,           stringparam(metadata)};
            Check(commands->ExecutePrepared("INSERT INTO "
                                            "npm_result_runs(run_id,task_id,input_namespace,status,started_at_ns,"
                                            "expires_at_ns,metadata_status) VALUES(?,?,?,?,?,?,?)",
                                            params, 7) >= 0,
                  source->GetLastError());
        }
        const std::string npm_table = table + "_npm";
        Check(source->ExecuteSql(("CREATE TABLE " + npm_table + "(bucket " + inttype + ",id " + uinttype + ",value " +
                                  realtype + ",__npm_run_id " + texttype + ")" +
                                  (category == "clickhouse" ? " ENGINE=MergeTree ORDER BY (bucket,id)" : ""))
                                     .c_str()) >= 0,
              source->GetLastError());
        Check(source->ExecuteSql(("INSERT INTO " + npm_table + " VALUES(0,'1',1.5,'" + runs[0] + "')").c_str()) >= 0,
              source->GetLastError());
        auto npm = c;
        npm.datasets[0].table = npm_table;
        npm.datasets[0].fields["__npm_run_id"] = LogicalType::kUtf8;
        npm.datasets[0].scope.consistency = "npm_completed";
        npm.state.idle_timeout_ms = 1;
        for (size_t i = 0; i < runs.size(); ++i) {
            npm.datasets[0].scope.run_ids = {runs[i]};
            SnapshotReader r;
            const int rc = r.Open(npm, source, c.source);
            if (i == 0) {
                Check(rc == 0, r.LastError());
                assert(r.Config().state.idle_timeout_ms == 0);
                SnapshotPage page;
                assert(r.Next(&page) == 0 && page.batch->num_rows() == 1);
            } else
                assert(rc != 0);
        }
        {
            const std::string periods = table + "_periods";
            const std::string boolean = category == "clickhouse" ? "Bool"
                                        : category == "postgres" ? "BOOLEAN"
                                                                 : "BOOLEAN";
            Check(
                source->ExecuteSql(
                    ("CREATE TABLE " + periods + "(__npm_run_id " + texttype + ",session_id " + uinttype +
                     ",revision " + uinttype + ",period_start_ns " + inttype + ",period_end_ns " + inttype +
                     ",is_final " + boolean + ",period_complete " + boolean + ",interval_packets_ab " + uinttype +
                     ",interval_packets_ba " + uinttype + ",interval_wire_bytes_ab " + uinttype + ")" +
                     (category == "clickhouse" ? " ENGINE=MergeTree ORDER BY (__npm_run_id,session_id,revision)" : ""))
                        .c_str()) >= 0,
                source->GetLastError());
            Check(source->ExecuteSql(("INSERT INTO " + periods + " VALUES('" + runs[0] +
                                      "','1','1',0,30000000000,false,true,'1','0','10'),('" + runs[0] +
                                      "','2','1',0,30000000000,false,true,'1','0','20'),('" + runs[0] +
                                      "','1','2',30000000000,60000000000,false,true,'1','0','30'),('" + runs[0] +
                                      "','1','3',60000000000,90000000000,false,true,'1','0','40'),('" + runs[0] +
                                      "','1','4',30000000000,60000000000,true,true,'0','0','0')")
                                         .c_str()) >= 0,
                  source->GetLastError());
            ConfigSnapshot snapshot;
            snapshot.sha256_hex = "finite-test-epoch";
            snapshot.config = c;
            auto& cfg = snapshot.config;
            auto& d = cfg.datasets[0];
            d.table = periods;
            d.fields = {{"__npm_run_id", LogicalType::kUtf8},
                        {"session_id", LogicalType::kUInt64},
                        {"revision", LogicalType::kUInt64},
                        {"period_start_ns", LogicalType::kInt64},
                        {"interval_wire_bytes_ab", LogicalType::kUInt64}};
            d.series_keys = {"__npm_run_id"};
            d.deduplicate_keys = {"__npm_run_id", "session_id", "revision"};
            d.row_semantics = "npm_period_increment";
            d.scope.consistency = "npm_completed";
            d.scope.run_ids = {runs[0]};
            d.scope.begin_bucket = 0;
            d.scope.end_bucket = 2;
            d.bucket_column = "period_start_ns";
            d.unit = TimeUnit::kNs;
            Metric metric;
            metric.id = "bytes";
            metric.value = {"interval_wire_bytes_ab", Aggregate::kSum};
            d.metrics = {metric};
            std::vector<std::shared_ptr<arrow::RecordBatch>> outputs;
            for (uint32_t size : {1, 4096}) {
                cfg.read.page_rows = size;
                SnapshotInput input;
                Check(input.Initialize(snapshot, source, c.source) == 0, input.LastError());
                uint64_t count = 0;
                std::vector<std::shared_ptr<arrow::RecordBatch>> pieces;
                for (;;) {
                    auto event = input.PollBlock();
                    if (event.kind == BlockPollEvent::kEof) break;
                    Check(event.kind == BlockPollEvent::kData, input.LastError());
                    assert(event.batch->schema()->Equals(*input.InputSchema(), true));
                    assert(input.PollBlock(0).kind == BlockPollEvent::kTimeout);
                    count += event.batch->num_rows();
                    pieces.push_back(event.batch);
                    assert(input.ReleaseBlock(event.batch) == 0 && input.ReleaseBlock(event.batch) != 0);
                }
                assert(count == 2 && input.IsFinished() && input.Stats().terminal_rows == 1);
                auto table_result = arrow::Table::FromRecordBatches(pieces);
                assert(table_result.ok());
                auto combined = (*table_result)->CombineChunksToBatch();
                assert(combined.ok());
                outputs.push_back(*combined);
                auto values = std::static_pointer_cast<arrow::DoubleArray>(outputs.back()->GetColumnByName("value"));
                assert(values->Value(0) == 60 && values->Value(1) == 40);
                assert(input.PollBlock().kind == BlockPollEvent::kEof);
            }
            assert(outputs[0]->Equals(*outputs[1]));
            {
                Metric relation;
                relation.id = "distribution";
                relation.kind = BaselineTaskKind::kRelation;
                GroupSpace groups;
                groups.id = "session-groups";
                groups.version = "1";
                groups.column = "session_id";
                relation.group_space = groups;
                relation.relation_metrics = {{"packets", {"interval_packets_ab", Aggregate::kSum}},
                                             {"bytes", {"interval_wire_bytes_ab", Aggregate::kSum}}};
                d.metrics = {relation};
                std::vector<std::shared_ptr<arrow::RecordBatch>> outputs;
                for (uint32_t page : {1, 4096}) {
                    cfg.read.page_rows = page;
                    SnapshotInput input;
                    Check(input.Initialize(snapshot, source, c.source) == 0, input.LastError());
                    std::vector<std::shared_ptr<arrow::RecordBatch>> parts;
                    for (;;) {
                        auto event = input.PollBlock();
                        if (event.kind == BlockPollEvent::kEof) break;
                        Check(event.kind == BlockPollEvent::kData, input.LastError());
                        parts.push_back(event.batch);
                        assert(input.ReleaseBlock(event.batch) == 0);
                    }
                    auto result = arrow::Table::FromRecordBatches(parts);
                    assert(result.ok());
                    auto combined = (*result)->CombineChunksToBatch();
                    assert(combined.ok());
                    outputs.push_back(*combined);
                    assert(outputs.back()->num_rows() == 2 && outputs.back()->ValidateFull().ok());
                    auto metrics =
                        std::static_pointer_cast<arrow::ListArray>(outputs.back()->GetColumnByName("metrics"));
                    auto values = std::static_pointer_cast<arrow::StructArray>(metrics->values());
                    auto names = std::static_pointer_cast<arrow::StringArray>(values->field(0));
                    auto totals = std::static_pointer_cast<arrow::DoubleArray>(values->field(1));
                    assert(names->GetString(0) == "packets" && names->GetString(1) == "bytes" &&
                           totals->Value(0) == 3 && totals->Value(1) == 60);
                }
                assert(outputs[0]->Equals(*outputs[1]));
                d.metrics = {metric};
            }
            cfg.bucket_seconds = 30;
            d.scope.end_bucket = 3;
            {
                SnapshotInput input;
                Check(input.Initialize(snapshot, source, c.source) == 0, input.LastError());
                uint64_t count = 0;
                for (;;) {
                    auto e = input.PollBlock();
                    if (e.kind == BlockPollEvent::kEof) break;
                    Check(e.kind == BlockPollEvent::kData, input.LastError());
                    count += e.batch->num_rows();
                    assert(input.ReleaseBlock(e.batch) == 0);
                }
                assert(count == 3);
            }
            {
                SnapshotInput input;
                Check(input.Initialize(snapshot, source, c.source) == 0, input.LastError());
                std::thread cancel([&] { input.Cancel(); });
                cancel.join();
                assert(input.PollBlock().kind == BlockPollEvent::kCancelled);
            }
            cfg.bucket_seconds = 45;
            {
                SnapshotInput input;
                Check(input.Initialize(snapshot, source, c.source) == 0, input.LastError());
                assert(input.PollBlock().kind == BlockPollEvent::kError);
            }
            Check(source->ExecuteSql(("DROP TABLE " + periods).c_str()) >= 0, source->GetLastError());
        }
        npm.datasets[0].scope.run_ids = {table + "_missing"};
        SnapshotReader missing;
        assert(missing.Open(npm, source, c.source) != 0);
        Check(source->ExecuteSql(("DROP TABLE " + npm_table).c_str()) >= 0, source->GetLastError());
        for (const auto& run : runs) {
            auto param = stringparam(run);
            const std::string sql = category == "clickhouse"
                                        ? "ALTER TABLE npm_result_runs DELETE WHERE run_id=? SETTINGS mutations_sync=2"
                                        : "DELETE FROM npm_result_runs WHERE run_id=?";
            Check(commands->ExecutePrepared(sql.c_str(), &param, 1) >= 0, source->GetLastError());
        }
    }
    if (category == "postgres" || category == "clickhouse") {
        const std::string special = category == "postgres" ? "CAST('Infinity' AS DOUBLE PRECISION)" : "inf";
        Check(source->ExecuteSql(("INSERT INTO " + table + " VALUES(2,'50'," + special + ")").c_str()) >= 0,
              source->GetLastError());
        ConfigSnapshot snapshot;
        snapshot.sha256_hex = "invalid-policy-test";
        snapshot.config = c;
        auto& d = snapshot.config.datasets[0];
        d.filter = {{"id", "eq", uint64_t{50}}};
        Metric m;
        m.id = "v";
        m.value = {"value", Aggregate::kSum};
        m.invalid_policy = InvalidPolicy::kSkip;
        d.metrics = {m};
        {
            SnapshotInput input;
            Check(input.Initialize(snapshot, source, c.source) == 0, input.LastError());
            Check(input.PollBlock().kind == BlockPollEvent::kEof, input.LastError());
            assert(input.Stats().skipped.at("rows.v:invalid") == 1);
        }
        d.metrics[0].invalid_policy = InvalidPolicy::kFail;
        {
            SnapshotInput input;
            Check(input.Initialize(snapshot, source, c.source) == 0, input.LastError());
            assert(input.PollBlock().kind == BlockPollEvent::kError);
        }
    }
    Check(source->ExecuteSql(("DROP VIEW " + view).c_str()) >= 0, source->GetLastError());
    Check(source->ExecuteSql(("DROP TABLE " + table).c_str()) >= 0, source->GetLastError());
    std::printf("PASS finite snapshot: %s\n", category.c_str());
}
}  // namespace
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    assert(ToBucket(-1, 60) == -1 && ToBucket(60, 60) == 1);
    const char* path = std::getenv("BASELINER_TEST_DB_OPTIONS");
    Check(path != nullptr, "BASELINER_TEST_DB_OPTIONS required (four real sources, no skip)");
    std::ifstream file(path);
    std::ostringstream options;
    options << file.rdbuf();
    database::DatabasePlugin plugin;
    const int saved = dup(STDOUT_FILENO);
    FILE* quiet = std::fopen("/dev/null", "w");
    dup2(fileno(quiet), STDOUT_FILENO);
    const int option_rc = plugin.Option(options.str().c_str());
    dup2(saved, STDOUT_FILENO);
    close(saved);
    std::fclose(quiet);
    Check(option_rc == 0, "database options failed");
    Check(plugin.Load(nullptr) == 0, "database load failed");
    Check(plugin.Start() == 0, "database start failed");
    Test(plugin, "sqlite", "t2sqlite");
    Test(plugin, "mysql", "t2mysql");
    Test(plugin, "postgres", "t2postgres");
    Test(plugin, "clickhouse", "t2clickhouse");
    plugin.Stop();
    plugin.Unload();
}
