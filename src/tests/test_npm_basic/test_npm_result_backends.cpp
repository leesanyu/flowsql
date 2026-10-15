// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <arrow/api.h>
#include <arrow/io/memory.h>
#include <arrow/ipc/reader.h>
#include <rapidjson/document.h>
#include <tests/common/test_environment.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <framework/interfaces/idatabase_arrow_write.h>
#include <operators/npm_basic/npm_basic_result_consumer.h>
#include <operators/npm_basic/npm_family_result_contract.h>
#include <operators/npm_basic/npm_result_progress.h>
#include <operators/npm_basic/output/npm_basic_result_collector.h>
#include <operators/npm_basic/output/npm_basic_result_encoder.h>
#include <services/database/database_plugin.h>

namespace {
int PublishForRun(flowsql::npm::INpmFamilyProgressConsumer* publisher, const flowsql::npm::NpmResultContextV1& context,
                  flowsql::npm::NpmFamilyPublicationRequest request) {
    request.key = {"basic_ipv4", "npm_basic_ipv4_history_v1", context.run_id};
    flowsql::npm::NpmFamilyPublishedProgress confirmed;
    return publisher->PublishProgress(request, &confirmed);
}

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
    explicit TestBudget(uint64_t limit = std::numeric_limits<uint64_t>::max()) : limit_(limit) {}
    NpmBudgetError Reserve(NpmBudgetCategory category, uint64_t bytes) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (category != NpmBudgetCategory::kPendingOutput) return NpmBudgetError::kInvalidCategory;
        if (bytes > limit_ - usage_.pending_output_bytes) return NpmBudgetError::kPendingOutputLimitExceeded;
        usage_.pending_output_bytes += bytes;
        peak_ = std::max(peak_, usage_.pending_output_bytes);
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
    uint64_t Peak() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return peak_;
    }

 private:
    mutable std::mutex mutex_;
    NpmBudgetUsage usage_;
    uint64_t limit_;
    uint64_t peak_ = 0;
};

class ColumnarProbe final : public IDatabaseChannel,
                            public IDatabasePreparedCommandV1,
                            public flowsql::IDatabaseArrowWriteV1 {
 public:
    explicit ColumnarProbe(IDatabaseChannel* channel) : channel_(channel) {}
    const char* Category() override { return channel_->Category(); }
    const char* Name() override { return channel_->Name(); }
    const char* Type() override { return channel_->Type(); }
    const char* Schema() override { return channel_->Schema(); }
    int Open() override { return channel_->Open(); }
    int Close() override { return channel_->Close(); }
    int Flush() override { return channel_->Flush(); }
    bool IsOpened() const override { return channel_->IsOpened(); }
    bool IsConnected() override { return channel_->IsConnected(); }
    const char* GetLastError() override { return channel_->GetLastError(); }
    int CreateReader(const char* sql, flowsql::IBatchReader** out) override { return channel_->CreateReader(sql, out); }
    int CreateWriter(const char* table, flowsql::IBatchWriter** out) override {
        return channel_->CreateWriter(table, out);
    }
    int CreateArrowReader(const char* sql, flowsql::IArrowReader** out) override {
        return channel_->CreateArrowReader(sql, out);
    }
    int CreateArrowWriter(const char* table, flowsql::IArrowWriter** out) override {
        return channel_->CreateArrowWriter(table, out);
    }
    int ExecuteQueryArrow(const char* sql, std::vector<std::shared_ptr<arrow::RecordBatch>>* out) override {
        return channel_->ExecuteQueryArrow(sql, out);
    }
    int WriteArrowBatches(const char* table, const std::vector<std::shared_ptr<arrow::RecordBatch>>& rows) override {
        return channel_->WriteArrowBatches(table, rows);
    }
    int ExecuteSql(const char* sql) override { return channel_->ExecuteSql(sql); }
    int ExecutePrepared(const char* sql, const DatabaseParameterV1* parameters, size_t count) override {
        return dynamic_cast<IDatabasePreparedCommandV1*>(channel_)->ExecutePrepared(sql, parameters, count);
    }
    int ExecutePreparedBatch(const char* sql, const DatabaseParameterV1* parameters, size_t width,
                             size_t count) override {
        ++parameter_batches;
        return dynamic_cast<IDatabasePreparedCommandV1*>(channel_)->ExecutePreparedBatch(sql, parameters, width, count);
    }
    int WriteArrowBatchesWithPool(const char* table, const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches,
                                  arrow::MemoryPool* pool) override {
        ++arrow_calls;
        assert(pool && expected && batches.size() == 1);
        const auto& rows = batches.front();
        assert(rows->num_rows() == expected->num_rows() && rows->num_columns() == expected->num_columns() + 2);
        for (int column = 0; column < expected->num_columns(); ++column) {
            assert(rows->column(column + 2).get() == expected->column(column).get());
        }
        const auto run = std::static_pointer_cast<arrow::StringArray>(rows->column(0));
        const auto task = std::static_pointer_cast<arrow::StringArray>(rows->column(1));
        for (int64_t row = 0; row < rows->num_rows(); ++row) {
            assert(run->GetString(row) == context.run_id && task->GetString(row) == context.task_id);
        }
        const int result =
            dynamic_cast<flowsql::IDatabaseArrowWriteV1*>(channel_)->WriteArrowBatchesWithPool(table, batches, pool);
        return lose_ack && result == 0 ? EIO : result;
    }
    std::shared_ptr<arrow::RecordBatch> expected;
    NpmResultContextV1 context;
    int arrow_calls = 0;
    int parameter_batches = 0;
    bool lose_ack = false;

 private:
    IDatabaseChannel* channel_;
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
            assert(static_cast<arrow::UInt32Builder*>(builder)
                       ->Append(name == "a_ip" || name == "src_ip" ? UINT32_MAX : 0)
                       .ok());
            break;
        case arrow::Type::UINT64: {
            const uint64_t value = (name == "session_id" || name == "transaction_id" || name == "handshake_id" ||
                                    name == "event_id" || name == "entity_instance_id")
                                       ? identity
                                       : (name == "revision" ? revision : 64);
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
        case arrow::Type::FIXED_SIZE_BINARY: {
            const uint8_t address[16] = {0, 0x80, 0xff, 0, 1, 2, 3, 4, 5, 6, 7, 8, 0, 10, 11, 12};
            assert(static_cast<arrow::FixedSizeBinaryBuilder*>(builder)->Append(address).ok());
            break;
        }
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
        auto input = std::make_shared<arrow::io::BufferReader>(
            arrow::Buffer::FromString(std::string(reinterpret_cast<const char*>(data), size)));
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
    for (const auto kind : {flowsql::npm::NpmFamilyEntityKind::kBasic, flowsql::npm::NpmFamilyEntityKind::kSession,
                            flowsql::npm::NpmFamilyEntityKind::kDns, flowsql::npm::NpmFamilyEntityKind::kHttp1,
                            flowsql::npm::NpmFamilyEntityKind::kTls, flowsql::npm::NpmFamilyEntityKind::kIcmp})
        for (const auto family : {flowsql::packet::AddressFamily::kIPv4, flowsql::packet::AddressFamily::kIPv6}) {
            const auto entity = flowsql::npm::NpmFamilyEntityDescriptor(kind, family);
            for (const char* relation : {"history", "latest", "final"})
                Execute(backend.channel.get(), "DROP VIEW IF EXISTS npm_" + entity.entity_id + '_' + relation + "_v1");
            Execute(backend.channel.get(), "DROP TABLE IF EXISTS npm_" + entity.entity_id + "_v1_data");
        }
    for (const char* relation :
         {"npm_basic_ipv4_history_v1", "npm_basic_ipv4_latest_v1", "npm_basic_ipv4_final_v1",
          "npm_session_ipv4_history_v1", "npm_session_ipv4_latest_v1", "npm_session_ipv4_final_v1",
          "npm_basic_ipv4_history_v2", "npm_basic_ipv4_latest_v2", "npm_basic_ipv4_final_v2", "npm_conflict_history_v9",
          "npm_conflict_latest_v9", "npm_conflict_final_v9"}) {
        Execute(backend.channel.get(), "DROP VIEW IF EXISTS " + std::string(relation));
        Execute(backend.channel.get(), "DROP TABLE IF EXISTS " + std::string(relation));
    }
    for (const char* table : {"npm_basic_ipv4_v1_data", "npm_session_ipv4_v1_data", "npm_basic_ipv4_v2_data",
                              "npm_conflict_v9_data", "npm_t3_parameter_probe", "npm_t3_atomic_probe",
                              "npm_result_progress_v1", "npm_result_entities", "npm_result_runs"}) {
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

    const auto history =
        Query(backend.channel.get(), backend.category,
              "SELECT * FROM npm_basic_ipv4_history_v1 WHERE __npm_run_id='" + run + "' ORDER BY revision");
    assert(RowCount(history) == 2);
    const auto& history_batch = *history.front();
    const auto identity = history_batch.GetColumnByName("session_id");
    const auto revision = history_batch.GetColumnByName("revision");
    assert(identity && identity->type_id() == arrow::Type::UINT64 && UIntValue(*identity, 0) == maximum);
    assert(revision && UIntValue(*revision, 1) == maximum);
    const auto ip = history_batch.GetColumnByName("a_ip");
    assert(ip && ip->type_id() == arrow::Type::UINT32 && UIntValue(*ip, 1) == UINT32_MAX);
    const auto nullable_protocol = history_batch.GetColumnByName("protocol");
    assert(nullable_protocol && nullable_protocol->IsNull(0) && !nullable_protocol->IsNull(1));
    const auto status = history_batch.GetColumnByName("__npm_run_status");
    assert(status && StringValue(*status, 0) == "completed");

    assert(RowCount(Query(backend.channel.get(), backend.category,
                          "SELECT * FROM npm_basic_ipv4_latest_v1 WHERE __npm_run_id='" + run + "'")) == 1);
    assert(RowCount(Query(backend.channel.get(), backend.category,
                          "SELECT * FROM npm_basic_ipv4_final_v1 WHERE __npm_run_id='" + run + "'")) == 1);
    assert(RowCount(Query(backend.channel.get(), backend.category,
                          "SELECT * FROM npm_session_ipv4_history_v1 WHERE __npm_run_id='" + run + "'")) == 1);

    const std::string other_run = prefix + "-other";
    auto other = CreateConsumer(backend.channel.get(), "pcapfile.other", {"task-other", other_run}, entities);
    auto other_rows = MakeRows(basic, maximum, {1}, {true});
    assert(other->Consume({"task-other", other_run}, basic, *other_rows) == 0);
    assert(other->Finish() == 0);
    assert(RowCount(Query(backend.channel.get(), backend.category,
                          "SELECT * FROM npm_basic_ipv4_latest_v1 WHERE session_id=" + std::to_string(maximum) +
                              " AND __npm_run_id IN ('" + run + "','" + other_run + "')")) == 2);

    const std::string incomplete_run = prefix + "-incomplete";
    auto incomplete =
        CreateConsumer(backend.channel.get(), "pcapfile.incomplete", {"task-incomplete", incomplete_run}, entities);
    assert(incomplete->Consume({"task-incomplete", incomplete_run}, session, *session_rows) == 0);
    flowsql::npm::NpmResultFailureV1 failure{EIO, "write", "expected backend test failure"};
    assert(incomplete->FailRun(failure) == 0);
    AssertSummary(*incomplete, incomplete_run, "incomplete", 1, 2);
    const auto incomplete_rows =
        Query(backend.channel.get(), backend.category,
              "SELECT * FROM npm_session_ipv4_history_v1 WHERE __npm_run_id='" + incomplete_run + "'");
    assert(RowCount(incomplete_rows) == 1);
    assert(StringValue(*incomplete_rows.front()->GetColumnByName("__npm_run_status"), 0) == "incomplete");

    const auto labeled_basic = flowsql::npm::NpmBasicEntityDescriptorV1(flowsql::packet::AddressFamily::kIPv4);
    assert(labeled_basic.schema->Equals(*basic.schema, true) && labeled_basic.schema_version == 1);
    for (int labels : {0, 1, 2}) {
        const std::string periodic_run = prefix + "-periodic-" + std::to_string(labels);
        auto periodic = CreateConsumer(backend.channel.get(), "pcapfile.periodic", {"task-periodic", periodic_run},
                                       {labeled_basic});
        {
            auto duplicate_factory =
                flowsql::npm::MakeNpmDatabaseResultConsumerFactory(backend.channel.get(), "pcapfile.periodic");
            std::unique_ptr<INpmManagedResultConsumerV1> duplicate;
            assert(duplicate_factory->Create({"task-periodic", periodic_run}, {labeled_basic},
                                             std::make_shared<TestBudget>(), &duplicate) == EEXIST);
            assert(!duplicate);
        }
        flowsql::npm::NpmBasicResult4 row;
        row.session_id = 101;
        row.revision = 1;
        row.protocol_status = flowsql::npm::NpmProtocolStatus::kUnknown;
        row.primary_label_id = labels == 2 ? 17 : 0;
        row.wire_bytes_ab = maximum - 10;
        row.wire_bytes_total = maximum - 10;
        row.packets_ab = 1;
        row.period = flowsql::npm::NpmBasicPeriodStats{0, 30000000000LL, true, 1, 0, maximum - 10, 0, maximum - 10};
        std::vector<flowsql::npm::NpmBasicResult4> records{row};
        row.revision = 2;
        row.packets_ba = 1;
        row.wire_bytes_ba = 10;
        row.wire_bytes_total = maximum;
        row.period = flowsql::npm::NpmBasicPeriodStats{30000000000LL, 60000000000LL, false, 0, 1, 0, 10, 10};
        records.push_back(row);
        row.revision = 3;
        row.is_final = true;
        row.end_reason = flowsql::npm::NpmSessionEndReason::kEof;
        row.period->interval_packets_ba = 0;
        row.period->interval_wire_bytes_ba = row.period->interval_wire_bytes_total = 0;
        records.push_back(row);
        std::shared_ptr<arrow::RecordBatch> encoded;
        assert(flowsql::npm::EncodeNpmBasicResults(records, &encoded, nullptr, labels != 0) ==
               flowsql::npm::NpmBasicEncodeError::kNone);
        assert(periodic->Consume({"task-periodic", periodic_run}, labeled_basic, *encoded) == 0);
        auto* publisher = dynamic_cast<flowsql::npm::INpmFamilyProgressConsumer*>(periodic.get());
        assert(publisher);
        flowsql::npm::NpmFamilyPublicationRequest progress;
        progress.period_ns = 30000000000LL;
        progress.closed_before_ns = 60000000000LL;
        assert(PublishForRun(publisher, {"task-periodic", periodic_run}, progress) == 0);
        assert(PublishForRun(publisher, {"task-periodic", periodic_run}, progress) == 0);  // idempotent boundary
        assert(PublishForRun(publisher, {"task-periodic", "wrong-run"}, progress) == EINVAL);
        progress.closed_before_ns = 90000000000LL;
        assert(PublishForRun(publisher, {"task-periodic", periodic_run}, progress) == 0);  // no data progression
        const auto published = Query(
            backend.channel.get(), backend.category,
            "SELECT epoch,position,closed_before_ns FROM npm_result_progress_v1 WHERE run_id='" + periodic_run + "'");
        assert(RowCount(published) == 2);
        assert(periodic->Finish() == 0);
        const auto stored = Query(
            backend.channel.get(), backend.category,
            "SELECT * FROM npm_basic_ipv4_history_v1 WHERE __npm_run_id='" + periodic_run + "' ORDER BY revision");
        assert(RowCount(stored) == 3);
        uint64_t summed = 0;
        for (const auto& batch : stored) {
            const auto delta = batch->GetColumnByName("interval_wire_bytes_total");
            const auto label = batch->GetColumnByName("primary_label_id");
            for (int64_t index = 0; index < batch->num_rows(); ++index) {
                assert(!delta->IsNull(index));
                summed += UIntValue(*delta, index);
                assert(label->IsNull(index) == (labels == 0));
                if (labels != 0) assert(UIntValue(*label, index) == static_cast<uint64_t>(labels == 2 ? 17 : 0));
            }
        }
        assert(summed == maximum);
        const auto latest = Query(backend.channel.get(), backend.category,
                                  "SELECT * FROM npm_basic_ipv4_latest_v1 WHERE __npm_run_id='" + periodic_run + "'");
        const auto terminal = Query(backend.channel.get(), backend.category,
                                    "SELECT * FROM npm_basic_ipv4_final_v1 WHERE __npm_run_id='" + periodic_run + "'");
        assert(RowCount(latest) == 1 && RowCount(terminal) == 1);
        assert(UIntValue(*terminal.front()->GetColumnByName("wire_bytes_total"), 0) == maximum);
        assert(UIntValue(*terminal.front()->GetColumnByName("interval_wire_bytes_total"), 0) == 0);
        assert(UIntValue(*latest.front()->GetColumnByName("revision"), 0) == 3);
    }
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
                          "SELECT * FROM npm_basic_ipv4_history_v1 WHERE __npm_run_id='" + expired_run + "'")) == 0);
    for (int step = 0; step < 16; ++step) {
        auto next = CreateConsumer(backend.channel.get(), "pcapfile.advance",
                                   {"task-advance", prefix + "-advance-" + std::to_string(step)}, {basic});
        assert(next->Finish() == 0);
    }
    assert(RowCount(Query(backend.channel.get(), backend.category,
                          "SELECT * FROM npm_result_runs WHERE run_id='" + expired_run + "'")) == 0);
    assert(RowCount(Query(backend.channel.get(), backend.category,
                          "SELECT * FROM npm_basic_ipv4_v1_data WHERE __npm_run_id='" + expired_run + "'")) == 0);
    assert(RowCount(Query(backend.channel.get(), backend.category,
                          "SELECT * FROM npm_result_runs WHERE run_id='" + writing_run + "'")) == 1);
    assert(writing->Finish() == 0);
}

void TestConflicts(const Backend& backend) {
    auto conflict = flowsql::npm::NpmBasicEntityDescriptorV1(flowsql::packet::AddressFamily::kIPv4);
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
                           {flowsql::npm::NpmBasicEntityDescriptorV1(flowsql::packet::AddressFamily::kIPv4)},
                           std::make_shared<TestBudget>(), &consumer) != 0);
    assert(!consumer);
}

std::vector<std::pair<NpmEntityDescriptorV1, std::shared_ptr<arrow::RecordBatch>>> RunTypedDelivery(
    std::shared_ptr<INpmTaskBudget> budget, std::unique_ptr<INpmManagedResultConsumerV1> consumer,
    const NpmResultContextV1& context) {
    using namespace flowsql::npm;
    using flowsql::packet::AddressFamily;
    std::vector<NpmEntityDescriptorV1> entities;
    for (auto family : {AddressFamily::kIPv4, AddressFamily::kIPv6}) {
        entities.push_back(NpmBasicEntityDescriptorV1(family));
        entities.push_back(NpmSessionEntityDescriptorV1(family));
    }
    auto router = std::make_shared<NpmResultRouter>(entities, "basic_ipv6", context, budget, std::move(consumer),
                                                    [](const std::function<int()>& call) { return call(); });
    NpmBasicFeatureConfig features;
    features.session_enabled = true;
    features.observing_entity = "basic_ipv6";
    NpmBasicResultCollector collector(features, budget, router);
    std::vector<std::pair<NpmEntityDescriptorV1, std::shared_ptr<arrow::RecordBatch>>> expected;
    const auto append = [&](auto sample, const NpmEntityDescriptorV1& entity, auto encode) {
        using Result = decltype(sample);
        std::vector<Result> values;
        for (uint64_t id = 1; id <= 513; ++id) {
            Result row;
            row.session_id = id;
            row.observation_domain_id = UINT64_MAX;
            row.revision = 1;
            row.observed_at = 1000 + id;
            row.transport_protocol = 17;
            row.is_final = true;
            row.end_reason = NpmSessionEndReason::kClosed;
            row.protocol_status = NpmProtocolStatus::kIdentified;
            row.protocol_id = 7;
            row.protocol = std::string(64, 'p') + "结果";
            if constexpr (std::is_same_v<decltype(row.a_ip), flowsql::network::IPv4Address>) {
                row.a_ip = flowsql::network::IPv4Address::FromUInt32(UINT32_MAX);
                row.b_ip = flowsql::network::IPv4Address::FromUInt32(0x0080ff00);
            } else {
                const uint8_t bytes[16] = {0, 0x80, 0xff, 0, 1, 2, 3, 4, 5, 6, 7, 8, 0, 10, 11, 12};
                std::memcpy(row.a_ip.bytes, bytes, 16);
                std::memcpy(row.b_ip.bytes, bytes, 16);
            }
            if constexpr (std::is_same_v<Result, NpmBasicResult4> || std::is_same_v<Result, NpmBasicResult6>)
                assert(collector.WriteBasic(row) == 0);
            else
                assert(collector.WriteSession(row) == 0);
            values.push_back(std::move(row));
        }
        std::shared_ptr<arrow::RecordBatch> rows;
        const auto encoded = encode(values, &rows, nullptr, false, nullptr);
        assert(encoded == decltype(encoded)::kNone);
        expected.emplace_back(entity, std::move(rows));
    };
    append(NpmBasicResult4{}, NpmBasicEntityDescriptorV1(AddressFamily::kIPv4),
           EncodeNpmBasicResults<flowsql::network::IPv4Address>);
    append(NpmBasicResult6{}, NpmBasicEntityDescriptorV1(AddressFamily::kIPv6),
           EncodeNpmBasicResults<flowsql::network::IPv6Address>);
    append(NpmSessionResult4{}, NpmSessionEntityDescriptorV1(AddressFamily::kIPv4),
           EncodeNpmSessionResults<flowsql::network::IPv4Address>);
    append(NpmSessionResult6{}, NpmSessionEntityDescriptorV1(AddressFamily::kIPv6),
           EncodeNpmSessionResults<flowsql::network::IPv6Address>);
    assert(collector.pending_typed_rows() > 0);
    assert(collector.FlushAccepted() == 0 && collector.pending_typed_rows() == 0);
    std::shared_ptr<arrow::RecordBatch> foreground;
    assert(router->Drain(&foreground) == 0 && foreground->num_rows() == 513);
    assert(foreground->Equals(*expected[1].second, true));
    assert(collector.Finish() == 0 && collector.Finish() == 0);
    assert(budget->Usage().pending_output_bytes > 0);
    foreground.reset();
    assert(budget->Usage().pending_output_bytes == 0);
    return expected;
}

void AssertTypedReadback(const std::vector<std::shared_ptr<arrow::RecordBatch>>& actual,
                         const arrow::RecordBatch& expected) {
    int64_t offset = 0;
    for (const auto& batch : actual) {
        assert(batch->schema()->metadata()->Equals(*expected.schema()->metadata()));
        for (int column = 0; column < expected.num_columns(); ++column) {
            const auto& field = expected.schema()->field(column);
            const int index = batch->schema()->GetFieldIndex(field->name());
            assert(index >= 0 && batch->schema()->field(index)->Equals(*field, true));
            if (!batch->column(index)->Equals(expected.column(column)->Slice(offset, batch->num_rows())))
                std::fprintf(stderr, "typed readback mismatch: %s at %lld actual=%s expected=%s\n",
                             field->name().c_str(), static_cast<long long>(offset),
                             batch->column(index)->Slice(0, 4)->ToString().c_str(),
                             expected.column(column)->Slice(offset, 4)->ToString().c_str());
            assert(batch->column(index)->Equals(expected.column(column)->Slice(offset, batch->num_rows())));
        }
        offset += batch->num_rows();
    }
    assert(offset == expected.num_rows());
}

void TestColumnarConsumer(const Backend& backend, const std::string& prefix) {
    if (backend.category != "clickhouse") return;
    const auto basic = flowsql::npm::NpmBasicEntityDescriptorV1(flowsql::packet::AddressFamily::kIPv4);
    auto rows = MakeRows(basic, std::numeric_limits<uint64_t>::max(), {1, 2}, {false, true});
    ColumnarProbe probe(backend.channel.get());
    probe.expected = rows;
    probe.context = {"task'\\&%=中文", prefix + "-columnar"};
    auto budget = std::make_shared<TestBudget>();
    auto consumer = CreateConsumer(&probe, "pcapfile.columnar", probe.context, {basic}, budget);
    assert(consumer->Consume(probe.context, basic, *rows) == 0);
    assert(probe.arrow_calls == 1 && probe.parameter_batches == 0);
    assert(budget->Peak() > 0 && budget->Usage().pending_output_bytes == 0);
    assert(consumer->Finish() == 0);
    AssertSummary(*consumer, probe.context.run_id, "completed", 2, 1);
    AssertTypedReadback(Query(backend.channel.get(), backend.category,
                              "SELECT * FROM npm_basic_ipv4_history_v1 WHERE __npm_run_id='" + probe.context.run_id +
                                  "' ORDER BY revision"),
                        *rows);

    for (uint64_t limit : {uint64_t{0}, uint64_t{512}, uint64_t{4096}}) {
        probe.context = {"task-budget", prefix + "-columnar-budget-" + std::to_string(limit)};
        auto limited = std::make_shared<TestBudget>(limit);
        auto denied = CreateConsumer(&probe, "pcapfile.columnar", probe.context, {basic}, limited);
        const int calls = probe.arrow_calls;
        assert(denied->Consume(probe.context, basic, *rows) == ENOSPC);
        assert(limited->Peak() <= limit && limited->Usage().pending_output_bytes == 0);
        assert(probe.arrow_calls <= calls + 1 && probe.parameter_batches == 0);
        assert(RowCount(Query(
                   backend.channel.get(), backend.category,
                   "SELECT * FROM npm_basic_ipv4_history_v1 WHERE __npm_run_id='" + probe.context.run_id + "'")) == 0);
        assert(denied->FailRun({ENOSPC, "write", "expected pool refusal"}) == 0);
    }

    probe.context = {"task-unknown", prefix + "-columnar-unknown"};
    probe.lose_ack = true;
    auto unknown = CreateConsumer(&probe, "pcapfile.columnar", probe.context, {basic}, std::make_shared<TestBudget>());
    const int calls = probe.arrow_calls;
    assert(unknown->Consume(probe.context, basic, *rows) == EIO);
    assert(probe.arrow_calls == calls + 1 && probe.parameter_batches == 0);
    assert(unknown->Finish() != 0);
    assert(unknown->FailRun({EIO, "write", "expected unknown acknowledgement"}) == 0);
    AssertSummary(*unknown, probe.context.run_id, "incomplete", 0, 1);
    assert(RowCount(Query(
               backend.channel.get(), backend.category,
               "SELECT * FROM npm_basic_ipv4_history_v1 WHERE __npm_run_id='" + probe.context.run_id + "'")) == 2);
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
    const std::vector<NpmEntityDescriptorV1> entities = {
        flowsql::npm::NpmBasicEntityDescriptorV1(flowsql::packet::AddressFamily::kIPv4),
        flowsql::npm::NpmSessionEntityDescriptorV1(flowsql::packet::AddressFamily::kIPv4)};
    const std::string prefix = "t3-" + backend.category + '-' + std::to_string(getpid());
    TestConcurrentOpen(backend, entities, prefix);
    {
        std::vector<NpmEntityDescriptorV1> families;
        for (const auto kind : {flowsql::npm::NpmFamilyEntityKind::kBasic, flowsql::npm::NpmFamilyEntityKind::kSession,
                                flowsql::npm::NpmFamilyEntityKind::kDns, flowsql::npm::NpmFamilyEntityKind::kHttp1,
                                flowsql::npm::NpmFamilyEntityKind::kTls, flowsql::npm::NpmFamilyEntityKind::kIcmp})
            for (const auto family : {flowsql::packet::AddressFamily::kIPv4, flowsql::packet::AddressFamily::kIPv6})
                families.push_back(flowsql::npm::NpmFamilyEntityDescriptor(kind, family));
        const NpmResultContextV1 context{"task-family", prefix + "-family"};
        auto budget = std::make_shared<TestBudget>();
        auto storage = CreateConsumer(backend.channel.get(), "pcapfile.family", context, families, budget);
        flowsql::npm::NpmResultRouter router(families, "", context, budget, std::move(storage),
                                             [](const std::function<int()>& call) { return call(); });
        for (const auto& entity : families) {
            auto expected = MakeRows(entity, UINT64_MAX, {1, 2}, {false, true});
            if (entity.revision_semantics == flowsql::npm::NpmRevisionSemanticsV1::kEvent) {
                arrow::UInt64Builder identities, revisions;
                arrow::BooleanBuilder finals;
                assert(identities.AppendValues({UINT64_MAX, UINT64_MAX - 1}).ok());
                assert(revisions.AppendValues({1, 1}).ok());
                assert(finals.AppendValues(std::vector<bool>{true, true}).ok());
                expected = expected
                               ->SetColumn(expected->schema()->GetFieldIndex(entity.identity_column),
                                           expected->schema()->GetFieldByName(entity.identity_column),
                                           identities.Finish().ValueOrDie())
                               .ValueOrDie();
                expected = expected
                               ->SetColumn(expected->schema()->GetFieldIndex(entity.revision_column),
                                           expected->schema()->GetFieldByName(entity.revision_column),
                                           revisions.Finish().ValueOrDie())
                               .ValueOrDie();
                expected = expected
                               ->SetColumn(expected->schema()->GetFieldIndex(entity.is_final_column),
                                           expected->schema()->GetFieldByName(entity.is_final_column),
                                           finals.Finish().ValueOrDie())
                               .ValueOrDie();
            }
            const int consumed = router.Emit(entity.module_id, entity.entity_id, *expected);
            if (consumed)
                std::fprintf(stderr, "%s %s Consume=%d error=%s\n", backend.category.c_str(), entity.entity_id.c_str(),
                             consumed, backend.channel->GetLastError());
            assert(consumed == 0);
            assert(budget->Usage().pending_output_bytes == 0);
            auto batches = Query(backend.channel.get(), backend.category,
                                 "SELECT * FROM npm_" + entity.entity_id + "_history_v1 WHERE __npm_run_id='" +
                                     context.run_id + "' ORDER BY observed_at");
            assert(RowCount(batches) == 2 && batches.size() == 1);
            const auto& actual = batches.front();
            for (int i = 0; i < expected->num_columns(); ++i) {
                const auto index = actual->schema()->GetFieldIndex(entity.schema->field(i)->name());
                assert(index >= 0 && actual->schema()->field(index)->Equals(*entity.schema->field(i), true));
                if (!actual->column(index)->Equals(expected->column(i)))
                    std::fprintf(stderr, "%s %s.%s actual=%s expected=%s\n", backend.category.c_str(),
                                 entity.entity_id.c_str(), entity.schema->field(i)->name().c_str(),
                                 actual->column(index)->ToString().c_str(), expected->column(i)->ToString().c_str());
                assert(actual->column(index)->Equals(expected->column(i)));
            }
            assert(actual->schema()->metadata()->Equals(*entity.schema->metadata()));
            if (backend.category == "postgres") {
                const auto address = entity.schema->GetFieldByName("a_ip") ? "a_ip" : "src_ip";
                const auto invalid = entity.schema->GetFieldByName(address)->type()->id() == arrow::Type::UINT32
                                         ? "4294967296"
                                         : "decode('0080ff','hex')";
                assert(backend.channel->ExecuteSql(("UPDATE npm_" + entity.entity_id + "_v1_data SET " + address + '=' +
                                                    invalid + " WHERE __npm_run_id='" + context.run_id + "'")
                                                       .c_str()) < 0);
            }
        }
        assert(router.Finish() == 0);
        const NpmResultContextV1 typed_context{"task-typed", prefix + "-typed"};
        const auto expected = RunTypedDelivery(
            budget, CreateConsumer(backend.channel.get(), "pcapfile.typed", typed_context, families, budget),
            typed_context);
        for (const auto& [entity, rows] : expected) {
            auto actual = Query(backend.channel.get(), backend.category,
                                "SELECT * FROM npm_" + entity.entity_id + "_history_v1 WHERE __npm_run_id='" +
                                    typed_context.run_id + "' ORDER BY session_id");
            AssertTypedReadback(actual, *rows);
        }
        assert(budget->Usage().pending_output_bytes == 0);
    }
    assert(RowCount(Query(backend.channel.get(), backend.category,
                          "SELECT purge_cursor FROM npm_result_runs WHERE run_id='missing'")) == 0);
    TestRelations(backend, entities, prefix);
    TestColumnarConsumer(backend, prefix);
    TestRetention(backend, entities, prefix);
    TestConflicts(backend);
    Cleanup(backend);
    std::printf("[PASS] npm managed results on %s\n", backend.category.c_str());
}

}  // namespace

int main() {
    DatabasePlugin plugin;
    std::string options;
    for (const char* backend : {"mysql", "postgres", "clickhouse"}) {
        if (!options.empty()) options += '|';
        options += flowsql::test::DatabaseOptions(backend, std::string("t3") + backend, "npm");
    }
    assert(plugin.Option(options.c_str()) == 0);
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
