// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <arrow/api.h>
#include <arrow/io/memory.h>
#include <arrow/ipc/reader.h>
#include <rapidjson/document.h>
#include <unistd.h>

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <operators/npm_basic/npm_basic_result_consumer.h>
#include <services/database/database_plugin.h>

namespace {

using flowsql::DatabaseParameterKindV1;
using flowsql::DatabaseParameterV1;
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

class TestBudget final : public INpmTaskBudget {
 public:
    NpmBudgetError Reserve(NpmBudgetCategory category, uint64_t bytes) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (category != NpmBudgetCategory::kPendingOutput) return NpmBudgetError::kInvalidCategory;
        usage_.pending_output_bytes += bytes;
        return NpmBudgetError::kNone;
    }

    NpmBudgetError Release(NpmBudgetCategory category, uint64_t bytes) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (category != NpmBudgetCategory::kPendingOutput || bytes > usage_.pending_output_bytes) {
            return NpmBudgetError::kReleaseUnderflow;
        }
        usage_.pending_output_bytes -= bytes;
        return NpmBudgetError::kNone;
    }

    NpmBudgetUsage Usage() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return usage_;
    }

 private:
    mutable std::mutex mutex_;
    NpmBudgetUsage usage_;
};

struct Backend {
    std::string category;
    std::string channel_name;
    std::shared_ptr<IDatabaseChannel> channel;
};

DatabaseParameterV1 UIntParameter(uint64_t value) {
    DatabaseParameterV1 parameter;
    parameter.kind = DatabaseParameterKindV1::kUInt64;
    parameter.uint64_value = value;
    return parameter;
}

DatabaseParameterV1 StringParameter(const std::string& value) {
    DatabaseParameterV1 parameter;
    parameter.kind = DatabaseParameterKindV1::kString;
    parameter.data = value.data();
    parameter.size = value.size();
    return parameter;
}

DatabaseParameterV1 BlobParameter(const std::string& value) {
    DatabaseParameterV1 parameter;
    parameter.kind = DatabaseParameterKindV1::kBlob;
    parameter.data = value.data();
    parameter.size = value.size();
    return parameter;
}

void AppendValue(arrow::ArrayBuilder* builder, const arrow::Field& field, uint64_t identity, uint64_t revision,
                 bool is_final, int row) {
    if (field.nullable() && row == 0) {
        assert(builder->AppendNull().ok());
        return;
    }
    const auto& name = field.name();
    switch (field.type()->id()) {
        case arrow::Type::BOOL:
            assert(static_cast<arrow::BooleanBuilder*>(builder)->Append(name == "is_final" ? is_final : true).ok());
            break;
        case arrow::Type::INT8:
            assert(static_cast<arrow::Int8Builder*>(builder)->Append(-8).ok());
            break;
        case arrow::Type::INT16:
            assert(static_cast<arrow::Int16Builder*>(builder)->Append(-16).ok());
            break;
        case arrow::Type::INT32:
            assert(static_cast<arrow::Int32Builder*>(builder)->Append(-32).ok());
            break;
        case arrow::Type::INT64:
            assert(static_cast<arrow::Int64Builder*>(builder)->Append(name == "observed_at" ? 1000 + row : -64).ok());
            break;
        case arrow::Type::UINT8:
            assert(static_cast<arrow::UInt8Builder*>(builder)->Append(8).ok());
            break;
        case arrow::Type::UINT16:
            assert(static_cast<arrow::UInt16Builder*>(builder)->Append(16).ok());
            break;
        case arrow::Type::UINT32:
            assert(static_cast<arrow::UInt32Builder*>(builder)->Append(32).ok());
            break;
        case arrow::Type::UINT64: {
            const uint64_t value = name == "session_id" ? identity : (name == "revision" ? revision : 64);
            assert(static_cast<arrow::UInt64Builder*>(builder)->Append(value).ok());
            break;
        }
        case arrow::Type::FLOAT:
            assert(static_cast<arrow::FloatBuilder*>(builder)->Append(1.25F).ok());
            break;
        case arrow::Type::DOUBLE:
            assert(static_cast<arrow::DoubleBuilder*>(builder)->Append(2.5).ok());
            break;
        case arrow::Type::STRING:
            assert(static_cast<arrow::StringBuilder*>(builder)->Append("quote' slash\\ unicode-结果").ok());
            break;
        case arrow::Type::BINARY:
            assert(
                static_cast<arrow::BinaryBuilder*>(builder)->Append(reinterpret_cast<const uint8_t*>("a\0b"), 3).ok());
            break;
        default:
            assert(false && "unsupported test type");
    }
}

std::shared_ptr<arrow::RecordBatch> MakeRows(const NpmEntityDescriptorV1& entity, uint64_t identity,
                                             const std::vector<uint64_t>& revisions, const std::vector<bool>& finals) {
    assert(revisions.size() == finals.size());
    std::vector<std::shared_ptr<arrow::Array>> columns;
    for (const auto& field : entity.schema->fields()) {
        std::unique_ptr<arrow::ArrayBuilder> builder;
        assert(arrow::MakeBuilder(arrow::default_memory_pool(), field->type(), &builder).ok());
        for (size_t row = 0; row < revisions.size(); ++row) {
            AppendValue(builder.get(), *field, identity, revisions[row], finals[row], static_cast<int>(row));
        }
        std::shared_ptr<arrow::Array> array;
        assert(builder->Finish(&array).ok());
        columns.push_back(std::move(array));
    }
    auto batch = arrow::RecordBatch::Make(entity.schema, static_cast<int64_t>(revisions.size()), std::move(columns));
    assert(batch->ValidateFull().ok());
    return batch;
}

std::vector<std::shared_ptr<arrow::RecordBatch>> Query(IDatabaseChannel* channel, const std::string& category,
                                                       const std::string& sql) {
    std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
    if (category == "clickhouse") {
        const int rc = channel->ExecuteQueryArrow(sql.c_str(), &batches);
        if (rc != 0)
            std::fprintf(stderr, "ClickHouse query failed: %s\nSQL: %s\n", channel->GetLastError(), sql.c_str());
        assert(rc == 0);
        return batches;
    }
    flowsql::IBatchReader* raw_reader = nullptr;
    const int create = channel->CreateReader(sql.c_str(), &raw_reader);
    if (create != 0)
        std::fprintf(stderr, "%s query failed: %s\nSQL: %s\n", category.c_str(), channel->GetLastError(), sql.c_str());
    assert(create == 0 && raw_reader != nullptr);
    const auto release = [](flowsql::IBatchReader* reader) {
        reader->Close();
        reader->Release();
    };
    std::unique_ptr<flowsql::IBatchReader, decltype(release)> reader(raw_reader, release);
    while (true) {
        const uint8_t* data = nullptr;
        size_t size = 0;
        const int next = reader->Next(&data, &size);
        if (next == 1) break;
        assert(next == 0 && data != nullptr && size != 0);
        auto input = std::make_shared<arrow::io::BufferReader>(arrow::Buffer::Wrap(data, size));
        auto opened = arrow::ipc::RecordBatchStreamReader::Open(input);
        assert(opened.ok());
        while (true) {
            std::shared_ptr<arrow::RecordBatch> batch;
            assert((*opened)->ReadNext(&batch).ok());
            if (!batch) break;
            batches.push_back(std::move(batch));
        }
    }
    return batches;
}

int64_t RowCount(const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches) {
    int64_t rows = 0;
    for (const auto& batch : batches) rows += batch->num_rows();
    return rows;
}

uint64_t UIntValue(const arrow::Array& array, int64_t row) {
    switch (array.type_id()) {
        case arrow::Type::UINT8:
            return static_cast<const arrow::UInt8Array&>(array).Value(row);
        case arrow::Type::UINT16:
            return static_cast<const arrow::UInt16Array&>(array).Value(row);
        case arrow::Type::UINT32:
            return static_cast<const arrow::UInt32Array&>(array).Value(row);
        case arrow::Type::UINT64:
            return static_cast<const arrow::UInt64Array&>(array).Value(row);
        case arrow::Type::INT32:
            return static_cast<const arrow::Int32Array&>(array).Value(row);
        case arrow::Type::INT64:
            return static_cast<const arrow::Int64Array&>(array).Value(row);
        case arrow::Type::STRING:
            return std::stoull(static_cast<const arrow::StringArray&>(array).GetString(row));
        default:
            assert(false && "not an unsigned-compatible array");
    }
    return 0;
}

std::string StringValue(const arrow::Array& array, int64_t row) {
    assert(!array.IsNull(row));
    if (array.type_id() == arrow::Type::STRING) return static_cast<const arrow::StringArray&>(array).GetString(row);
    if (array.type_id() == arrow::Type::BINARY) {
        const auto value = static_cast<const arrow::BinaryArray&>(array).GetView(row);
        return std::string(value.data(), value.size());
    }
    assert(false && "not a string-compatible array");
    return {};
}

std::unique_ptr<INpmManagedResultConsumerV1> CreateConsumer(
    IDatabaseChannel* channel, const std::string& input_namespace, const NpmResultContextV1& context,
    const std::vector<NpmEntityDescriptorV1>& entities,
    std::shared_ptr<TestBudget> budget = std::make_shared<TestBudget>()) {
    auto factory = flowsql::npm::MakeNpmDatabaseResultConsumerFactory(channel, input_namespace);
    std::unique_ptr<INpmManagedResultConsumerV1> consumer;
    const int rc = factory->Create(context, entities, std::move(budget), &consumer);
    if (rc != 0) std::fprintf(stderr, "consumer create failed: %s\n", factory->LastError().c_str());
    assert(rc == 0 && consumer != nullptr);
    return consumer;
}

void AssertSummary(const INpmManagedResultConsumerV1& consumer, const std::string& run_id, const char* status,
                   int64_t rows, rapidjson::SizeType entities) {
    rapidjson::Document document;
    const std::string json = consumer.ResultJson();
    document.Parse(json.c_str());
    assert(!document.HasParseError() && document.IsObject());
    assert(document["run_id"].GetString() == run_id);
    assert(std::string(document["run_status"].GetString()) == status);
    assert(std::string(document["metadata_status"].GetString()) == "known");
    assert(document["rows_written"].GetInt64() == rows);
    assert(document["entities"].IsArray() && document["entities"].Size() == entities);
}

void Execute(IDatabaseChannel* channel, const std::string& sql) {
    const int rc = channel->ExecuteSql(sql.c_str());
    if (rc < 0) std::fprintf(stderr, "SQL failed: %s\nSQL: %s\n", channel->GetLastError(), sql.c_str());
    assert(rc >= 0);
}

void Cleanup(const Backend& backend) {
    for (const char* relation :
         {"npm_basic_history_v1", "npm_basic_latest_v1", "npm_basic_final_v1", "npm_session_history_v1",
          "npm_session_latest_v1", "npm_session_final_v1", "npm_basic_history_v2", "npm_basic_latest_v2",
          "npm_basic_final_v2", "npm_conflict_history_v9", "npm_conflict_latest_v9", "npm_conflict_final_v9"}) {
        Execute(backend.channel.get(), "DROP VIEW IF EXISTS " + std::string(relation));
        Execute(backend.channel.get(), "DROP TABLE IF EXISTS " + std::string(relation));
    }
    for (const char* table :
         {"npm_basic_v1_data", "npm_session_v1_data", "npm_basic_v2_data", "npm_conflict_v9_data",
          "npm_t3_parameter_probe", "npm_t3_atomic_probe", "npm_result_entities", "npm_result_runs"}) {
        Execute(backend.channel.get(), "DROP TABLE IF EXISTS " + std::string(table));
    }
}

void TestPreparedCommands(const Backend& backend) {
    auto* commands = dynamic_cast<IDatabasePreparedCommandV1*>(backend.channel.get());
    assert(commands != nullptr);
    if (backend.category == "mysql") {
        Execute(backend.channel.get(),
                "CREATE TABLE npm_t3_parameter_probe(s TEXT,b BLOB,u BIGINT UNSIGNED,n BIGINT) ");
        Execute(backend.channel.get(), "CREATE TABLE npm_t3_atomic_probe(id BIGINT PRIMARY KEY) ENGINE=InnoDB");
    } else if (backend.category == "postgres") {
        Execute(backend.channel.get(), "CREATE TABLE npm_t3_parameter_probe(s TEXT,b BYTEA,u NUMERIC(20,0),n BIGINT)");
        Execute(backend.channel.get(), "CREATE TABLE npm_t3_atomic_probe(id BIGINT PRIMARY KEY)");
    } else {
        Execute(backend.channel.get(),
                "CREATE TABLE npm_t3_parameter_probe(s String,b String,u UInt64,n Nullable(Int64)) ENGINE=MergeTree "
                "ORDER BY tuple()");
        Execute(backend.channel.get(), "CREATE TABLE npm_t3_atomic_probe(id UInt8) ENGINE=MergeTree ORDER BY tuple()");
    }

    const std::string empty;
    const DatabaseParameterV1 probe[] = {
        StringParameter(empty), BlobParameter(empty), UIntParameter(std::numeric_limits<uint64_t>::max()), {}};
    assert(commands->ExecutePrepared("INSERT INTO npm_t3_parameter_probe VALUES(?,?,?,?)", probe, 4) >= 0);
    const auto values = Query(backend.channel.get(), backend.category, "SELECT s,b,u,n FROM npm_t3_parameter_probe");
    assert(RowCount(values) == 1);
    const auto& row = *values.front();
    assert(StringValue(*row.column(0), 0).empty());
    assert(row.column(1)->type_id() == arrow::Type::BINARY || row.column(1)->type_id() == arrow::Type::STRING);
    assert(UIntValue(*row.column(2), 0) == std::numeric_limits<uint64_t>::max());
    assert(row.column(3)->IsNull(0));

    DatabaseParameterV1 atomic[2];
    atomic[0] = UIntParameter(1);
    const std::string invalid_number = "not-a-number";
    atomic[1] = backend.category == "clickhouse" ? StringParameter(invalid_number) : UIntParameter(1);
    assert(commands->ExecutePreparedBatch("INSERT INTO npm_t3_atomic_probe VALUES(?)", atomic, 1, 2) < 0);
    const auto count = Query(backend.channel.get(), backend.category, "SELECT count(*) AS n FROM npm_t3_atomic_probe");
    assert(RowCount(count) == 1 && UIntValue(*count.front()->column(0), 0) == 0);
}

void TestConcurrentOpen(const Backend& backend, const std::vector<NpmEntityDescriptorV1>& entities,
                        const std::string& prefix) {
    std::vector<std::thread> threads;
    std::vector<int> results(4, -1);
    for (size_t index = 0; index < results.size(); ++index) {
        threads.emplace_back([&, index] {
            auto factory =
                flowsql::npm::MakeNpmDatabaseResultConsumerFactory(backend.channel.get(), "pcapfile.concurrent");
            std::unique_ptr<INpmManagedResultConsumerV1> consumer;
            results[index] = factory->Create({"task-concurrent", prefix + "-concurrent-" + std::to_string(index)},
                                             entities, std::make_shared<TestBudget>(), &consumer);
            if (results[index] != 0) {
                std::fprintf(stderr, "%s concurrent create %zu failed: %s\n", backend.category.c_str(), index,
                             factory->LastError().c_str());
            }
            if (results[index] == 0) results[index] = consumer->Finish();
        });
    }
    for (auto& thread : threads) thread.join();
    for (int result : results) assert(result == 0);
}

void TestRelations(const Backend& backend, const std::vector<NpmEntityDescriptorV1>& entities,
                   const std::string& prefix) {
    const auto& basic = entities[0];
    const auto& session = entities[1];
    const uint64_t maximum = std::numeric_limits<uint64_t>::max();
    const std::string run = prefix + "-completed";
    auto consumer =
        CreateConsumer(backend.channel.get(), "pcapfile.quote'\\capture", {"task'\\special", run}, entities);
    auto basic_rows = MakeRows(basic, maximum, {maximum - 1, maximum}, {false, true});
    auto session_rows = MakeRows(session, maximum, {maximum - 2}, {true});
    assert(consumer->Consume({"task'\\special", run}, basic, *basic_rows) == 0);
    assert(consumer->Consume({"task'\\special", run}, session, *session_rows) == 0);
    AssertSummary(*consumer, run, "writing", 3, 2);
    assert(consumer->Finish() == 0);
    AssertSummary(*consumer, run, "completed", 3, 2);

    const auto history = Query(backend.channel.get(), backend.category,
                               "SELECT * FROM npm_basic_history_v1 WHERE __npm_run_id='" + run + "' ORDER BY revision");
    assert(RowCount(history) == 2);
    const auto& history_batch = *history.front();
    const auto identity = history_batch.GetColumnByName("session_id");
    const auto revision = history_batch.GetColumnByName("revision");
    assert(identity && identity->type_id() == arrow::Type::UINT64 && UIntValue(*identity, 0) == maximum);
    assert(revision && UIntValue(*revision, 1) == maximum);
    const auto ip = history_batch.GetColumnByName("a_ip");
    assert(ip && StringValue(*ip, 1) == "quote' slash\\ unicode-结果");
    const auto nullable_protocol = history_batch.GetColumnByName("protocol");
    assert(nullable_protocol && nullable_protocol->IsNull(0) && !nullable_protocol->IsNull(1));
    const auto status = history_batch.GetColumnByName("__npm_run_status");
    assert(status && StringValue(*status, 0) == "completed");

    assert(RowCount(Query(backend.channel.get(), backend.category,
                          "SELECT * FROM npm_basic_latest_v1 WHERE __npm_run_id='" + run + "'")) == 1);
    assert(RowCount(Query(backend.channel.get(), backend.category,
                          "SELECT * FROM npm_basic_final_v1 WHERE __npm_run_id='" + run + "'")) == 1);
    assert(RowCount(Query(backend.channel.get(), backend.category,
                          "SELECT * FROM npm_session_history_v1 WHERE __npm_run_id='" + run + "'")) == 1);

    const std::string other_run = prefix + "-other";
    auto other = CreateConsumer(backend.channel.get(), "pcapfile.other", {"task-other", other_run}, entities);
    auto other_rows = MakeRows(basic, maximum, {1}, {true});
    assert(other->Consume({"task-other", other_run}, basic, *other_rows) == 0);
    assert(other->Finish() == 0);
    assert(RowCount(Query(backend.channel.get(), backend.category,
                          "SELECT * FROM npm_basic_latest_v1 WHERE session_id=" + std::to_string(maximum))) == 2);

    const std::string incomplete_run = prefix + "-incomplete";
    auto incomplete =
        CreateConsumer(backend.channel.get(), "pcapfile.incomplete", {"task-incomplete", incomplete_run}, entities);
    assert(incomplete->Consume({"task-incomplete", incomplete_run}, session, *session_rows) == 0);
    flowsql::npm::NpmResultFailureV1 failure{EIO, "write", "expected backend test failure"};
    assert(incomplete->FailRun(failure) == 0);
    AssertSummary(*incomplete, incomplete_run, "incomplete", 1, 2);
    const auto incomplete_rows =
        Query(backend.channel.get(), backend.category,
              "SELECT * FROM npm_session_history_v1 WHERE __npm_run_id='" + incomplete_run + "'");
    assert(RowCount(incomplete_rows) == 1);
    assert(StringValue(*incomplete_rows.front()->GetColumnByName("__npm_run_status"), 0) == "incomplete");

    const auto basic_v2 = flowsql::npm::NpmBasicEntityDescriptorV1(true);
    const std::string v2_run = prefix + "-v2";
    auto v2 = CreateConsumer(backend.channel.get(), "pcapfile.v2", {"task-v2", v2_run}, {basic_v2});
    auto v2_rows = MakeRows(basic_v2, maximum - 1, {maximum}, {true});
    assert(v2->Consume({"task-v2", v2_run}, basic_v2, *v2_rows) == 0);
    assert(v2->Finish() == 0);
    assert(RowCount(Query(backend.channel.get(), backend.category,
                          "SELECT * FROM npm_basic_history_v2 WHERE __npm_run_id='" + v2_run + "'")) == 1);
    assert(RowCount(Query(backend.channel.get(), backend.category,
                          "SELECT * FROM npm_basic_history_v1 WHERE __npm_run_id='" + v2_run + "'")) == 0);
}

void TestRetention(const Backend& backend, const std::vector<NpmEntityDescriptorV1>& entities,
                   const std::string& prefix) {
    const auto& basic = entities.front();
    const std::string expired_run = prefix + "-expired";
    auto factory = flowsql::npm::MakeNpmDatabaseResultConsumerFactory(backend.channel.get(), "pcapfile.retained", 1);
    std::unique_ptr<INpmManagedResultConsumerV1> retained;
    assert(factory->Create({"task-retained", expired_run}, entities, std::make_shared<TestBudget>(), &retained) == 0);
    assert(retained->Consume({"task-retained", expired_run}, basic, *MakeRows(basic, 77, {1}, {true})) == 0);
    assert(retained->Finish() == 0);
    const auto expiry = Query(backend.channel.get(), backend.category,
                              "SELECT expires_at_ns FROM npm_result_runs WHERE run_id='" + expired_run + "'");
    assert(RowCount(expiry) == 1 && !expiry.front()->column(0)->IsNull(0));
    const std::string writing_run = prefix + "-writing";
    auto writing = CreateConsumer(backend.channel.get(), "pcapfile.writing", {"task-writing", writing_run}, {basic});
    assert(writing->Consume({"task-writing", writing_run}, basic, *MakeRows(basic, 78, {1}, {true})) == 0);
    const std::string update = backend.category == "clickhouse"
                                   ? "ALTER TABLE npm_result_runs UPDATE expires_at_ns=1 WHERE run_id IN ('" +
                                         expired_run + "','" + writing_run + "') SETTINGS mutations_sync=2"
                                   : "UPDATE npm_result_runs SET expires_at_ns=1 WHERE run_id IN ('" + expired_run +
                                         "','" + writing_run + "')";
    Execute(backend.channel.get(), update);
    assert(RowCount(Query(backend.channel.get(), backend.category,
                          "SELECT * FROM npm_basic_history_v1 WHERE __npm_run_id='" + expired_run + "'")) == 0);
    for (int step = 0; step < 5; ++step) {
        auto next = CreateConsumer(backend.channel.get(), "pcapfile.advance",
                                   {"task-advance", prefix + "-advance-" + std::to_string(step)}, {basic});
        assert(next->Finish() == 0);
    }
    assert(RowCount(Query(backend.channel.get(), backend.category,
                          "SELECT * FROM npm_result_runs WHERE run_id='" + expired_run + "'")) == 0);
    assert(RowCount(Query(backend.channel.get(), backend.category,
                          "SELECT * FROM npm_basic_v1_data WHERE __npm_run_id='" + expired_run + "'")) == 0);
    assert(RowCount(Query(backend.channel.get(), backend.category,
                          "SELECT * FROM npm_result_runs WHERE run_id='" + writing_run + "'")) == 1);
    assert(writing->Finish() == 0);
}

void TestConflicts(const Backend& backend) {
    auto conflict = flowsql::npm::NpmBasicEntityDescriptorV1();
    conflict.entity_id = "conflict";
    conflict.schema_version = 9;
    Execute(backend.channel.get(),
            "CREATE TABLE npm_conflict_v9_data(x INT" +
                std::string(backend.category == "clickhouse" ? ") ENGINE=MergeTree ORDER BY tuple()" : ")"));
    auto factory = flowsql::npm::MakeNpmDatabaseResultConsumerFactory(backend.channel.get(), "pcapfile.conflict");
    std::unique_ptr<INpmManagedResultConsumerV1> consumer;
    assert(factory->Create({"task-data-conflict", "run-data-conflict"}, {conflict}, std::make_shared<TestBudget>(),
                           &consumer) != 0);
    assert(!consumer);
    Execute(backend.channel.get(), "DROP TABLE npm_conflict_v9_data");

    Execute(backend.channel.get(),
            "CREATE TABLE npm_conflict_history_v9(x INT" +
                std::string(backend.category == "clickhouse" ? ") ENGINE=MergeTree ORDER BY tuple()" : ")"));
    assert(factory->Create({"task-conflict", "run-conflict"}, {conflict}, std::make_shared<TestBudget>(), &consumer) !=
           0);
    assert(!consumer);

    Execute(backend.channel.get(), "DROP TABLE npm_conflict_history_v9");
    Cleanup(backend);
    Execute(backend.channel.get(),
            "CREATE TABLE npm_result_runs(x INT" +
                std::string(backend.category == "clickhouse" ? ") ENGINE=MergeTree ORDER BY tuple()" : ")"));
    factory = flowsql::npm::MakeNpmDatabaseResultConsumerFactory(backend.channel.get(), "pcapfile.catalog-conflict");
    assert(factory->Create({"task-catalog-conflict", "run-catalog-conflict"},
                           {flowsql::npm::NpmBasicEntityDescriptorV1()}, std::make_shared<TestBudget>(),
                           &consumer) != 0);
    assert(!consumer);
}

void TestBackend(const Backend& backend) {
    std::printf("[TEST] npm managed results on %s\n", backend.category.c_str());
    Cleanup(backend);
    TestPreparedCommands(backend);
    if (backend.category == "mysql") {
        Execute(backend.channel.get(),
                "CREATE TABLE npm_result_runs(run_id VARCHAR(128) PRIMARY KEY NOT NULL,task_id VARCHAR(128) "
                "NOT NULL,input_namespace VARCHAR(512) NOT NULL,status VARCHAR(16) NOT NULL,started_at_ns BIGINT "
                "NOT NULL,completed_at_ns BIGINT,expires_at_ns BIGINT,error_code BIGINT,error_stage VARCHAR(64),"
                "error_message TEXT,metadata_status VARCHAR(16) NOT NULL) ENGINE=InnoDB");
    } else if (backend.category == "postgres") {
        Execute(backend.channel.get(),
                "CREATE TABLE npm_result_runs(run_id TEXT PRIMARY KEY NOT NULL,task_id TEXT NOT NULL,"
                "input_namespace TEXT NOT NULL,status TEXT NOT NULL,started_at_ns BIGINT NOT NULL,"
                "completed_at_ns BIGINT,expires_at_ns BIGINT,error_code BIGINT,error_stage TEXT,error_message TEXT,"
                "metadata_status TEXT NOT NULL)");
    } else {
        Execute(backend.channel.get(),
                "CREATE TABLE npm_result_runs(run_id String,task_id String,input_namespace String,status String,"
                "started_at_ns Int64,completed_at_ns Nullable(Int64),expires_at_ns Nullable(Int64),"
                "error_code Nullable(Int64),error_stage Nullable(String),error_message Nullable(String),"
                "metadata_status String) ENGINE=MergeTree ORDER BY run_id");
    }
    const std::vector<NpmEntityDescriptorV1> entities = {flowsql::npm::NpmBasicEntityDescriptorV1(),
                                                         flowsql::npm::NpmSessionEntityDescriptorV1()};
    const std::string prefix = "t3-" + backend.category + '-' + std::to_string(getpid());
    TestConcurrentOpen(backend, entities, prefix);
    assert(RowCount(Query(backend.channel.get(), backend.category,
                          "SELECT purge_cursor FROM npm_result_runs WHERE run_id='missing'")) == 0);
    TestRelations(backend, entities, prefix);
    TestRetention(backend, entities, prefix);
    TestConflicts(backend);
    Cleanup(backend);
    std::printf("[PASS] npm managed results on %s\n", backend.category.c_str());
}

}  // namespace

int main() {
    DatabasePlugin plugin;
    const char* options =
        "type=mysql;name=t3mysql;host=127.0.0.1;port=3306;user=flowsql_user;password=flowSQL@user;"
        "database=flowsql_db|"
        "type=postgres;name=t3postgres;host=127.0.0.1;port=5432;user=flowsql_user;password=flowSQL@postgres;"
        "database=flowsql_db|"
        "type=clickhouse;name=t3clickhouse;host=127.0.0.1;port=8123;user=flowsql_user;password=flowSQL@user;"
        "database=flowsql_db";
    assert(plugin.Option(options) == 0);
    assert(plugin.Load(nullptr) == 0);
    assert(plugin.Start() == 0);

    std::vector<Backend> backends;
    for (const auto& [category, name] : {std::pair<const char*, const char*>("mysql", "t3mysql"),
                                         {"postgres", "t3postgres"},
                                         {"clickhouse", "t3clickhouse"}}) {
        auto channel = plugin.AcquireChannel(category, name);
        if (!channel || !channel->IsOpened() || !channel->IsConnected()) {
            std::fprintf(stderr, "required backend unavailable: %s.%s (%s)\n", category, name, plugin.LastError());
            assert(false && "T3 integration backend must not skip");
        }
        backends.push_back({category, name, std::move(channel)});
    }
    for (const auto& backend : backends) TestBackend(backend);
    backends.clear();
    assert(plugin.Stop() == 0);
    std::puts("[PASS] npm MySQL/PostgreSQL/ClickHouse managed-result integration");
    return 0;
}
