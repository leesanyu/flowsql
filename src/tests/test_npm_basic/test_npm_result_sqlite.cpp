// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <arrow/api.h>
#include <arrow/array/concatenate.h>
#include <rapidjson/document.h>
#include <sqlite3.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <operators/npm_basic/npm_basic_result_consumer.h>
#include <services/database/database_plugin.h>

namespace {

using flowsql::IDatabaseChannel;
using flowsql::IDatabasePreparedCommandV1;
using flowsql::database::DatabasePlugin;
using flowsql::npm::INpmManagedResultConsumerV1;
using flowsql::npm::INpmResultConsumerFactoryV1;
using flowsql::npm::INpmTaskBudget;
using flowsql::npm::NpmBudgetCategory;
using flowsql::npm::NpmBudgetError;
using flowsql::npm::NpmBudgetUsage;
using flowsql::npm::NpmEntityDescriptorV1;
using flowsql::npm::NpmResultContextV1;

class BoundedBudget final : public INpmTaskBudget {
 public:
    explicit BoundedBudget(uint64_t pending_limit = std::numeric_limits<uint64_t>::max())
        : pending_limit_(pending_limit) {}

    NpmBudgetError Reserve(NpmBudgetCategory category, uint64_t bytes) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (category != NpmBudgetCategory::kPendingOutput) return NpmBudgetError::kInvalidCategory;
        if (usage_.pending_output_bytes > pending_limit_ || bytes > pending_limit_ - usage_.pending_output_bytes) {
            return NpmBudgetError::kPendingOutputLimitExceeded;
        }
        usage_.pending_output_bytes += bytes;
        peak_pending_ = std::max(peak_pending_, usage_.pending_output_bytes);
        return NpmBudgetError::kNone;
    }

    NpmBudgetError Release(NpmBudgetCategory category, uint64_t bytes) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (category != NpmBudgetCategory::kPendingOutput) return NpmBudgetError::kInvalidCategory;
        if (bytes > usage_.pending_output_bytes) return NpmBudgetError::kReleaseUnderflow;
        usage_.pending_output_bytes -= bytes;
        return NpmBudgetError::kNone;
    }

    NpmBudgetUsage Usage() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return usage_;
    }

    uint64_t PeakPending() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return peak_pending_;
    }

 private:
    uint64_t pending_limit_;
    mutable std::mutex mutex_;
    NpmBudgetUsage usage_;
    uint64_t peak_pending_ = 0;
};

class SqliteProbe final {
 public:
    explicit SqliteProbe(const std::string& path) {
        assert(sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, nullptr) ==
               SQLITE_OK);
    }
    ~SqliteProbe() { sqlite3_close(db_); }

    void Execute(const std::string& sql) {
        char* error = nullptr;
        const int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &error);
        if (rc != SQLITE_OK) std::fprintf(stderr, "SQLite probe failed: %s\n", error ? error : "unknown");
        sqlite3_free(error);
        assert(rc == SQLITE_OK);
    }

    int64_t Int64(const std::string& sql) {
        sqlite3_stmt* statement = Prepare(sql);
        assert(sqlite3_step(statement) == SQLITE_ROW);
        const int64_t value = sqlite3_column_int64(statement, 0);
        sqlite3_finalize(statement);
        return value;
    }

    std::string Text(const std::string& sql) {
        sqlite3_stmt* statement = Prepare(sql);
        assert(sqlite3_step(statement) == SQLITE_ROW);
        const auto* value = sqlite3_column_text(statement, 0);
        assert(value != nullptr);
        const std::string result(reinterpret_cast<const char*>(value), sqlite3_column_bytes(statement, 0));
        sqlite3_finalize(statement);
        return result;
    }

 private:
    sqlite3_stmt* Prepare(const std::string& sql) {
        sqlite3_stmt* statement = nullptr;
        const int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &statement, nullptr);
        if (rc != SQLITE_OK) std::fprintf(stderr, "SQLite probe prepare failed: %s\n", sqlite3_errmsg(db_));
        assert(rc == SQLITE_OK && statement != nullptr);
        return statement;
    }

    sqlite3* db_ = nullptr;
};

std::shared_ptr<arrow::RecordBatch> MakeRows(const NpmEntityDescriptorV1& entity, uint64_t identity, uint64_t revision,
                                             bool is_final = true) {
    std::vector<std::shared_ptr<arrow::Array>> columns;
    columns.reserve(entity.schema->num_fields());
    for (const auto& field : entity.schema->fields()) {
        std::unique_ptr<arrow::ArrayBuilder> builder;
        assert(arrow::MakeBuilder(arrow::default_memory_pool(), field->type(), &builder).ok());
        switch (field->type()->id()) {
            case arrow::Type::BOOL:
                assert(static_cast<arrow::BooleanBuilder*>(builder.get())
                           ->Append(field->name() == entity.is_final_column ? is_final : true)
                           .ok());
                break;
            case arrow::Type::INT8:
                assert(static_cast<arrow::Int8Builder*>(builder.get())->Append(1).ok());
                break;
            case arrow::Type::INT16:
                assert(static_cast<arrow::Int16Builder*>(builder.get())->Append(2).ok());
                break;
            case arrow::Type::INT32:
                assert(static_cast<arrow::Int32Builder*>(builder.get())->Append(3).ok());
                break;
            case arrow::Type::INT64:
                assert(static_cast<arrow::Int64Builder*>(builder.get())->Append(100).ok());
                break;
            case arrow::Type::UINT8:
                assert(static_cast<arrow::UInt8Builder*>(builder.get())->Append(4).ok());
                break;
            case arrow::Type::UINT16:
                assert(static_cast<arrow::UInt16Builder*>(builder.get())->Append(5).ok());
                break;
            case arrow::Type::UINT32:
                assert(static_cast<arrow::UInt32Builder*>(builder.get())->Append(6).ok());
                break;
            case arrow::Type::UINT64: {
                const uint64_t value = field->name() == entity.identity_column
                                           ? identity
                                           : (field->name() == entity.revision_column ? revision : 7);
                assert(static_cast<arrow::UInt64Builder*>(builder.get())->Append(value).ok());
                break;
            }
            case arrow::Type::FLOAT:
                assert(static_cast<arrow::FloatBuilder*>(builder.get())->Append(1.25F).ok());
                break;
            case arrow::Type::DOUBLE:
                assert(static_cast<arrow::DoubleBuilder*>(builder.get())->Append(2.5).ok());
                break;
            case arrow::Type::STRING:
                assert(static_cast<arrow::StringBuilder*>(builder.get())->Append("value").ok());
                break;
            case arrow::Type::BINARY:
                assert(static_cast<arrow::BinaryBuilder*>(builder.get())
                           ->Append(reinterpret_cast<const uint8_t*>("bytes"), 5)
                           .ok());
                break;
            default:
                assert(false && "test row builder does not support this Arrow type");
        }
        std::shared_ptr<arrow::Array> array;
        assert(builder->Finish(&array).ok());
        columns.push_back(std::move(array));
    }
    auto rows = arrow::RecordBatch::Make(entity.schema, 1, std::move(columns));
    assert(rows->ValidateFull().ok());
    return rows;
}

std::shared_ptr<arrow::RecordBatch> DuplicateRows(const std::shared_ptr<arrow::RecordBatch>& rows) {
    std::vector<std::shared_ptr<arrow::Array>> columns;
    columns.reserve(rows->num_columns());
    for (const auto& column : rows->columns()) {
        auto combined = arrow::Concatenate({column, column});
        assert(combined.ok());
        columns.push_back(*combined);
    }
    return arrow::RecordBatch::Make(rows->schema(), 2, std::move(columns));
}

std::unique_ptr<INpmManagedResultConsumerV1> CreateConsumer(INpmResultConsumerFactoryV1* factory,
                                                            NpmResultContextV1 context,
                                                            const std::vector<NpmEntityDescriptorV1>& entities,
                                                            const std::shared_ptr<INpmTaskBudget>& budget) {
    std::unique_ptr<INpmManagedResultConsumerV1> consumer;
    const int rc = factory->Create(context, entities, budget, &consumer);
    if (rc != 0) std::fprintf(stderr, "consumer create failed: %s\n", factory->LastError().c_str());
    assert(rc == 0 && consumer != nullptr);
    return consumer;
}

void AssertSummary(const std::string& json, const char* run_id, const char* status, int64_t rows,
                   rapidjson::SizeType entity_count) {
    rapidjson::Document document;
    document.Parse(json.c_str());
    assert(!document.HasParseError() && document.IsObject());
    assert(std::string(document["run_id"].GetString()) == run_id);
    assert(std::string(document["run_status"].GetString()) == status);
    assert(std::string(document["metadata_status"].GetString()) == "known");
    assert(document["rows_written"].GetInt64() == rows);
    assert(document["entities"].IsArray() && document["entities"].Size() == entity_count);
}

void RemoveSqliteFiles(const std::string& path) {
    std::remove(path.c_str());
    std::remove((path + "-wal").c_str());
    std::remove((path + "-shm").c_str());
}

}  // namespace

int main() {
    const std::string path = "/tmp/flowsql_npm_result_sqlite_" + std::to_string(getpid()) + ".db";
    RemoveSqliteFiles(path);

    DatabasePlugin plugin;
    assert(plugin.Option(("type=sqlite;name=result;path=" + path).c_str()) == 0);
    assert(plugin.Load(nullptr) == 0);
    assert(plugin.Start() == 0);
    auto channel = plugin.AcquireChannel("sqlite", "result");
    assert(channel && channel->IsOpened() && channel->IsConnected());
    auto* prepared_commands = dynamic_cast<IDatabasePreparedCommandV1*>(channel.get());
    assert(prepared_commands != nullptr);

    assert(channel->ExecuteSql("PRAGMA foreign_keys=ON") >= 0);
    assert(channel->ExecuteSql("CREATE TABLE parameter_probe(text_value TEXT,blob_value BLOB)") >= 0);
    flowsql::DatabaseParameterV1 empty_parameters[2];
    empty_parameters[0].kind = flowsql::DatabaseParameterKindV1::kString;
    empty_parameters[1].kind = flowsql::DatabaseParameterKindV1::kBlob;
    assert(prepared_commands->ExecutePrepared("INSERT INTO parameter_probe VALUES(?,?)", empty_parameters, 2) == 1);
    flowsql::DatabaseParameterV1 oversized_string;
    oversized_string.kind = flowsql::DatabaseParameterKindV1::kString;
    oversized_string.data = "x";
    oversized_string.size = static_cast<size_t>(std::numeric_limits<int>::max()) + 1;
    assert(prepared_commands->ExecutePrepared("INSERT INTO parameter_probe(text_value) VALUES(?)", &oversized_string,
                                              1) < 0);
    assert(channel->ExecuteSql("CREATE TABLE commit_parent(id INTEGER PRIMARY KEY)") >= 0);
    assert(channel->ExecuteSql("CREATE TABLE commit_child(parent_id INTEGER,FOREIGN KEY(parent_id) REFERENCES "
                               "commit_parent(id) DEFERRABLE INITIALLY DEFERRED)") >= 0);
    const flowsql::DatabaseParameterV1 invalid_parent = [] {
        flowsql::DatabaseParameterV1 parameter;
        parameter.kind = flowsql::DatabaseParameterKindV1::kInt64;
        parameter.int64_value = 999;
        return parameter;
    }();
    assert(prepared_commands->ExecutePreparedBatch("INSERT INTO commit_child(parent_id) VALUES(?)", &invalid_parent, 1,
                                                   1) < 0);
    const flowsql::DatabaseParameterV1 valid_parent = [] {
        flowsql::DatabaseParameterV1 parameter;
        parameter.kind = flowsql::DatabaseParameterKindV1::kInt64;
        parameter.int64_value = 1;
        return parameter;
    }();
    assert(prepared_commands->ExecutePrepared("INSERT INTO commit_parent(id) VALUES(?)", &valid_parent, 1) == 1);

    assert(channel->ExecuteSql(
               "CREATE TABLE npm_result_runs(run_id TEXT PRIMARY KEY NOT NULL,task_id TEXT NOT NULL,"
               "input_namespace TEXT NOT NULL,status TEXT NOT NULL CHECK(status IN "
               "('opening','writing','completed','incomplete','purging')),started_at_ns INTEGER NOT NULL,"
               "completed_at_ns INTEGER,expires_at_ns INTEGER,error_code INTEGER,error_stage TEXT,error_message TEXT,"
               "metadata_status TEXT NOT NULL CHECK(metadata_status IN ('known','unknown')))") >= 0);
    assert(channel->ExecuteSql(
               "INSERT INTO npm_result_runs(run_id,task_id,input_namespace,status,started_at_ns,metadata_status) "
               "VALUES('legacy-run','legacy-task','legacy-input','writing',1,'known')") >= 0);

    auto factory = flowsql::npm::MakeNpmDatabaseResultConsumerFactory(channel.get(), "pcapfile.capture");
    const auto basic = flowsql::npm::NpmBasicEntityDescriptorV1();
    const auto session = flowsql::npm::NpmSessionEntityDescriptorV1();
    const std::vector<NpmEntityDescriptorV1> entities = {basic, session};

    auto budget = std::make_shared<BoundedBudget>();
    auto first = CreateConsumer(factory.get(), {"task-repeat", "run-1"}, entities, budget);
    const uint64_t maximum = std::numeric_limits<uint64_t>::max();
    const auto basic_rows = MakeRows(basic, maximum, maximum - 1);
    const auto session_rows = MakeRows(session, maximum - 2, maximum - 3);
    assert(first->Consume({"task-repeat", "run-1"}, basic, *basic_rows) == 0);
    assert(budget->PeakPending() > 0 && budget->Usage().pending_output_bytes == 0);
    assert(first->Consume({"task-repeat", "run-1"}, session, *session_rows) == 0);
    assert(budget->Usage().pending_output_bytes == 0);
    AssertSummary(first->ResultJson(), "run-1", "writing", 2, 2);
    assert(first->Finish() == 0);
    AssertSummary(first->ResultJson(), "run-1", "completed", 2, 2);

    SqliteProbe probe(path);
    assert(probe.Int64("SELECT text_value IS NOT NULL AND length(text_value)=0 FROM parameter_probe") == 1);
    assert(probe.Int64("SELECT blob_value IS NOT NULL AND length(blob_value)=0 FROM parameter_probe") == 1);
    assert(probe.Int64("SELECT COUNT(*) FROM parameter_probe") == 1);
    assert(probe.Int64("SELECT COUNT(*) FROM commit_child") == 0);
    assert(probe.Int64("SELECT COUNT(*) FROM commit_parent") == 1);
    assert(probe.Int64("SELECT COUNT(*) FROM npm_result_runs") == 2);
    assert(probe.Int64("SELECT purge_cursor IS NULL FROM npm_result_runs WHERE run_id='legacy-run'") == 1);
    assert(probe.Int64("SELECT COUNT(*) FROM npm_result_entities") == 2);
    assert(probe.Int64("SELECT COUNT(*) FROM npm_basic_v1_data WHERE __npm_run_id='run-1'") == 1);
    assert(probe.Int64("SELECT COUNT(*) FROM npm_session_v1_data WHERE __npm_run_id='run-1'") == 1);
    assert(probe.Text("SELECT session_id FROM npm_basic_v1_data WHERE __npm_run_id='run-1'") ==
           std::to_string(maximum));
    assert(probe.Text("SELECT revision FROM npm_basic_v1_data WHERE __npm_run_id='run-1'") ==
           std::to_string(maximum - 1));
    assert(probe.Text("SELECT status FROM npm_result_runs WHERE run_id='run-1'") == "completed");
    assert(probe.Text("SELECT metadata_status FROM npm_result_runs WHERE run_id='run-1'") == "known");
    assert(probe.Int64("SELECT completed_at_ns IS NOT NULL FROM npm_result_runs WHERE run_id='run-1'") == 1);
    assert(probe.Text("SELECT type FROM sqlite_master WHERE name='npm_basic_history_v1'") == "view");
    assert(probe.Text("SELECT type FROM sqlite_master WHERE name='npm_basic_latest_v1'") == "view");
    assert(probe.Text("SELECT type FROM sqlite_master WHERE name='npm_basic_final_v1'") == "view");
    assert(probe.Int64("SELECT COUNT(*) FROM sqlite_master WHERE name IN "
                       "('npm_basic_history','npm_basic_latest','npm_basic_final')") == 0);

    auto event = basic;
    event.entity_id = "event_probe";
    event.module_id = "event_probe";
    event.schema_version = 7;
    event.revision_semantics = flowsql::npm::NpmRevisionSemanticsV1::kEvent;
    auto relation_run = CreateConsumer(factory.get(), {"task-relations", "run-relations"}, {basic, session, event},
                                       std::make_shared<BoundedBudget>());
    assert(relation_run->Consume({"task-relations", "run-relations"}, basic, *MakeRows(basic, 101, 2, false)) == 0);
    assert(relation_run->Consume({"task-relations", "run-relations"}, basic, *MakeRows(basic, 101, 10, true)) == 0);
    assert(relation_run->Consume({"task-relations", "run-relations"}, basic, *MakeRows(basic, 101, maximum, false)) ==
           0);
    assert(relation_run->Consume({"task-relations", "run-relations"}, session, *MakeRows(session, 101, 8, true)) == 0);
    assert(relation_run->Consume({"task-relations", "run-relations"}, event, *MakeRows(event, 101, 1, true)) == 0);
    assert(relation_run->Finish() == 0);
    AssertSummary(relation_run->ResultJson(), "run-relations", "completed", 5, 3);
    assert(probe.Int64("SELECT COUNT(*) FROM npm_basic_history_v1 WHERE __npm_run_id='run-relations'") == 3);
    assert(probe.Int64("SELECT COUNT(*) FROM npm_basic_latest_v1 WHERE __npm_run_id='run-relations'") == 1);
    assert(probe.Text("SELECT revision FROM npm_basic_latest_v1 WHERE __npm_run_id='run-relations'") ==
           std::to_string(maximum));
    assert(probe.Int64("SELECT COUNT(*) FROM npm_basic_final_v1 WHERE __npm_run_id='run-relations'") == 1);
    assert(probe.Text("SELECT revision FROM npm_basic_final_v1 WHERE __npm_run_id='run-relations'") == "10");
    assert(probe.Int64("SELECT COUNT(*) FROM npm_session_latest_v1 WHERE __npm_run_id='run-relations' "
                       "AND session_id='101' AND revision='8'") == 1);
    assert(probe.Int64("SELECT COUNT(*) FROM npm_event_probe_history_v7 WHERE __npm_run_id='run-relations'") == 1);
    assert(probe.Int64("SELECT COUNT(*) FROM npm_event_probe_latest_v7 WHERE __npm_run_id='run-relations'") == 1);
    assert(probe.Int64("SELECT COUNT(*) FROM npm_event_probe_final_v7 WHERE __npm_run_id='run-relations'") == 1);
    assert(probe.Text("SELECT __npm_run_status FROM npm_event_probe_history_v7 WHERE "
                      "__npm_run_id='run-relations'") == "completed");

    auto second_budget = std::make_shared<BoundedBudget>();
    auto second = CreateConsumer(factory.get(), {"task-repeat", "run-2"}, entities, second_budget);
    assert(second->Consume({"task-repeat", "run-2"}, basic, *MakeRows(basic, 101, 1)) == 0);
    assert(second->Finish() == 0);
    assert(probe.Int64("SELECT COUNT(*) FROM npm_result_runs WHERE task_id='task-repeat'") == 2);
    assert(probe.Int64("SELECT COUNT(*) FROM npm_basic_v1_data WHERE __npm_run_id IN ('run-1','run-2')") == 2);
    assert(probe.Int64("SELECT COUNT(*) FROM npm_basic_latest_v1 WHERE session_id='101' AND "
                       "__npm_run_id IN ('run-relations','run-2')") == 2);
    assert(probe.Text("SELECT revision FROM npm_basic_latest_v1 WHERE __npm_run_id='run-2'") == "1");

    auto failed =
        CreateConsumer(factory.get(), {"task-failed", "run-failed"}, entities, std::make_shared<BoundedBudget>());
    assert(failed->Consume({"task-failed", "run-failed"}, session, *MakeRows(session, 21, 1)) == 0);
    flowsql::npm::NpmResultFailureV1 failure;
    failure.code = EIO;
    failure.stage = "runtime";
    failure.message = "injected write suffix failure";
    assert(failed->FailRun(failure) == 0);
    AssertSummary(failed->ResultJson(), "run-failed", "incomplete", 1, 2);
    assert(probe.Text("SELECT status FROM npm_result_runs WHERE run_id='run-failed'") == "incomplete");
    assert(probe.Int64("SELECT error_code FROM npm_result_runs WHERE run_id='run-failed'") == EIO);
    assert(probe.Text("SELECT error_stage FROM npm_result_runs WHERE run_id='run-failed'") == "runtime");
    assert(probe.Text("SELECT error_message FROM npm_result_runs WHERE run_id='run-failed'") ==
           "injected write suffix failure");
    assert(probe.Int64("SELECT COUNT(*) FROM npm_session_v1_data WHERE __npm_run_id='run-failed'") == 1);
    assert(probe.Int64("SELECT COUNT(*) FROM npm_session_history_v1 WHERE __npm_run_id='run-failed'") == 1);
    assert(probe.Text("SELECT __npm_run_status FROM npm_session_history_v1 WHERE __npm_run_id='run-failed'") ==
           "incomplete");

    const auto labeled_basic = flowsql::npm::NpmBasicEntityDescriptorV1(true);
    auto labeled = CreateConsumer(factory.get(), {"task-labeled", "run-labeled"}, {labeled_basic},
                                  std::make_shared<BoundedBudget>());
    assert(labeled->Consume({"task-labeled", "run-labeled"}, labeled_basic, *MakeRows(labeled_basic, 101, 1)) == 0);
    assert(labeled->Finish() == 0);
    assert(probe.Int64("SELECT COUNT(*) FROM npm_basic_history_v1 WHERE __npm_run_id='run-labeled'") == 0);
    assert(probe.Int64("SELECT COUNT(*) FROM npm_basic_history_v2 WHERE __npm_run_id='run-labeled'") == 1);

    {
        auto cancelled = CreateConsumer(factory.get(), {"task-cancelled", "run-cancelled"}, entities,
                                        std::make_shared<BoundedBudget>());
        cancelled->Cancel();
    }
    assert(probe.Text("SELECT status FROM npm_result_runs WHERE run_id='run-cancelled'") == "incomplete");

    auto constrained_budget = std::make_shared<BoundedBudget>(1);
    auto constrained = CreateConsumer(factory.get(), {"task-budget", "run-budget"}, entities, constrained_budget);
    assert(constrained->Consume({"task-budget", "run-budget"}, basic, *MakeRows(basic, 31, 1)) == ENOSPC);
    assert(constrained_budget->Usage().pending_output_bytes == 0);
    failure.code = ENOSPC;
    failure.stage = "consumer";
    failure.message = "pending output budget exceeded";
    assert(constrained->FailRun(failure) == 0);
    assert(probe.Int64("SELECT COUNT(*) FROM npm_basic_v1_data WHERE __npm_run_id='run-budget'") == 0);

    auto atomic_batch =
        CreateConsumer(factory.get(), {"task-atomic", "run-atomic"}, entities, std::make_shared<BoundedBudget>());
    assert(atomic_batch->Consume({"task-atomic", "run-atomic"}, basic, *DuplicateRows(MakeRows(basic, 41, 1))) == EIO);
    assert(probe.Int64("SELECT COUNT(*) FROM npm_basic_v1_data WHERE __npm_run_id='run-atomic'") == 0);
    failure.code = EIO;
    failure.stage = "consumer";
    failure.message = "batch transaction rolled back";
    assert(atomic_batch->FailRun(failure) == 0);

    auto unknown_metadata =
        CreateConsumer(factory.get(), {"task-unknown", "run-unknown"}, {session}, std::make_shared<BoundedBudget>());
    probe.Execute("DELETE FROM npm_result_runs WHERE run_id='run-unknown'");
    failure.code = EIO;
    failure.stage = "metadata";
    failure.message = "injected run metadata loss";
    assert(unknown_metadata->FailRun(failure) == EIO);
    rapidjson::Document unknown_summary;
    unknown_summary.Parse(unknown_metadata->ResultJson().c_str());
    assert(!unknown_summary.HasParseError() && unknown_summary.IsObject());
    assert(std::string(unknown_summary["run_status"].GetString()) == "incomplete");
    assert(std::string(unknown_summary["metadata_status"].GetString()) == "unknown");

    auto retention_factory = flowsql::npm::MakeNpmDatabaseResultConsumerFactory(channel.get(), "pcapfile.retained", 1);
    auto retained = CreateConsumer(retention_factory.get(), {"task-retained", "run-retained"}, entities,
                                   std::make_shared<BoundedBudget>());
    for (uint64_t identity = 1000; identity < 1066; ++identity) {
        assert(retained->Consume({"task-retained", "run-retained"}, basic, *MakeRows(basic, identity, 1)) == 0);
    }
    assert(retained->Consume({"task-retained", "run-retained"}, session, *MakeRows(session, 2000, 1)) == 0);
    assert(retained->Finish() == 0);
    assert(probe.Int64("SELECT expires_at_ns IS NOT NULL FROM npm_result_runs WHERE run_id='run-retained'") == 1);
    assert(probe.Int64("SELECT expires_at_ns IS NULL FROM npm_result_runs WHERE run_id='run-1'") == 1);
    auto writing = CreateConsumer(retention_factory.get(), {"task-writing", "run-writing"}, {basic},
                                  std::make_shared<BoundedBudget>());
    assert(writing->Consume({"task-writing", "run-writing"}, basic, *MakeRows(basic, 3000, 1)) == 0);
    probe.Execute("UPDATE npm_result_runs SET expires_at_ns=1 WHERE run_id IN ('run-retained','run-writing')");
    assert(probe.Int64("SELECT COUNT(*) FROM npm_basic_history_v1 WHERE __npm_run_id='run-retained'") == 0);
    assert(probe.Int64("SELECT COUNT(*) FROM npm_basic_history_v1 WHERE __npm_run_id='run-writing'") == 0);
    probe.Execute(
        "CREATE TRIGGER npm_purge_fail BEFORE DELETE ON npm_basic_v1_data BEGIN SELECT RAISE(ABORT,'injected'); "
        "END");
    auto advance = [&](int step) {
        auto next = CreateConsumer(factory.get(), {"task-advance", "run-advance-" + std::to_string(step)}, {basic},
                                   std::make_shared<BoundedBudget>());
        assert(next->Finish() == 0);
    };
    advance(0);
    assert(probe.Text("SELECT status FROM npm_result_runs WHERE run_id='run-retained'") == "purging");
    assert(probe.Int64("SELECT COUNT(*) FROM npm_basic_v1_data WHERE __npm_run_id='run-retained'") == 66);
    probe.Execute("DROP TRIGGER npm_purge_fail");
    advance(1);
    assert(probe.Int64("SELECT COUNT(*) FROM npm_basic_v1_data WHERE __npm_run_id='run-retained'") == 2);
    advance(2);
    assert(probe.Int64("SELECT COUNT(*) FROM npm_basic_v1_data WHERE __npm_run_id='run-retained'") == 0);
    assert(probe.Text("SELECT purge_cursor FROM npm_result_runs WHERE run_id='run-retained'") == "npm_basic_v1_data");
    advance(3);
    advance(4);
    advance(5);
    advance(6);
    assert(probe.Int64("SELECT COUNT(*) FROM npm_result_runs WHERE run_id='run-retained'") == 0);
    assert(probe.Int64("SELECT COUNT(*) FROM npm_session_v1_data WHERE __npm_run_id='run-retained'") == 0);
    assert(probe.Text("SELECT status FROM npm_result_runs WHERE run_id='run-writing'") == "writing");
    assert(probe.Int64("SELECT COUNT(*) FROM npm_basic_v1_data WHERE __npm_run_id='run-writing'") == 1);
    assert(writing->Finish() == 0);

    auto invalid_id = basic;
    invalid_id.entity_id = "Basic";
    std::unique_ptr<INpmManagedResultConsumerV1> rejected;
    assert(factory->Create({"invalid", "run-invalid-id"}, {invalid_id}, std::make_shared<BoundedBudget>(), &rejected) ==
           EINVAL);
    assert(!rejected);

    auto reserved = basic;
    reserved.entity_id = "reserved";
    reserved.schema_version = 9;
    auto reserved_fields = reserved.schema->fields();
    reserved_fields.push_back(arrow::field("__npm_private", arrow::utf8(), true));
    reserved.schema = arrow::schema(std::move(reserved_fields), reserved.schema->metadata());
    assert(factory->Create({"invalid", "run-reserved"}, {reserved}, std::make_shared<BoundedBudget>(), &rejected) ==
           EINVAL);
    assert(!rejected);

    auto unsupported = basic;
    unsupported.entity_id = "unsupported";
    unsupported.schema_version = 9;
    auto unsupported_fields = unsupported.schema->fields();
    unsupported_fields.push_back(arrow::field("unsupported_date", arrow::date32(), true));
    unsupported.schema = arrow::schema(std::move(unsupported_fields), unsupported.schema->metadata());
    assert(factory->Create({"invalid", "run-unsupported"}, {unsupported}, std::make_shared<BoundedBudget>(),
                           &rejected) == ENOTSUP);
    assert(!rejected);

    auto conflict = basic;
    auto conflict_fields = conflict.schema->fields();
    conflict_fields.push_back(arrow::field("new_value", arrow::utf8(), true));
    conflict.schema = arrow::schema(std::move(conflict_fields), conflict.schema->metadata());
    assert(factory->Create({"invalid", "run-schema-conflict"}, {conflict}, std::make_shared<BoundedBudget>(),
                           &rejected) == EINVAL);
    assert(!rejected);
    assert(factory->LastError().find("contract conflict") != std::string::npos);
    assert(probe.Int64("SELECT COUNT(*) FROM npm_result_runs WHERE run_id LIKE 'run-%conflict'") == 0);

    auto relation_conflict = basic;
    relation_conflict.entity_id = "relation_conflict";
    relation_conflict.module_id = "relation_conflict";
    relation_conflict.schema_version = 9;
    probe.Execute("CREATE TABLE npm_relation_conflict_history_v9(value TEXT)");
    assert(factory->Create({"invalid", "run-relation-conflict"}, {relation_conflict}, std::make_shared<BoundedBudget>(),
                           &rejected) == EINVAL);
    assert(!rejected);
    assert(factory->LastError().find("relation contract conflict") != std::string::npos);
    assert(probe.Int64("SELECT COUNT(*) FROM npm_result_runs WHERE run_id='run-relation-conflict'") == 0);

    first.reset();
    second.reset();
    failed.reset();
    labeled.reset();
    relation_run.reset();
    constrained.reset();
    atomic_batch.reset();
    probe.Execute("DROP TABLE npm_basic_v1_data");
    probe.Execute("CREATE TABLE npm_basic_v1_data(__npm_run_id TEXT NOT NULL PRIMARY KEY)");
    assert(factory->Create({"invalid", "run-table-conflict"}, {basic}, std::make_shared<BoundedBudget>(), &rejected) ==
           EINVAL);
    assert(!rejected);
    assert(factory->LastError().find("data table contract conflict") != std::string::npos);
    assert(probe.Int64("SELECT COUNT(*) FROM npm_result_runs WHERE run_id='run-table-conflict'") == 0);

    probe.Execute("DROP TABLE npm_result_entities");
    probe.Execute("CREATE TABLE npm_result_entities(entity_id TEXT PRIMARY KEY NOT NULL)");
    assert(factory->Create({"invalid", "run-catalog-conflict"}, {session}, std::make_shared<BoundedBudget>(),
                           &rejected) == EINVAL);
    assert(!rejected);
    assert(factory->LastError().find("catalog contract conflict") != std::string::npos);
    assert(probe.Int64("SELECT COUNT(*) FROM npm_result_runs WHERE run_id='run-catalog-conflict'") == 0);

    factory.reset();
    channel.reset();
    assert(plugin.Stop() == 0);
    RemoveSqliteFiles(path);
    std::puts("[PASS] npm SQLite managed multi-entity result consumer");
    return 0;
}
