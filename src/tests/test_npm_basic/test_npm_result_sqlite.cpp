// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <arrow/api.h>
#include <arrow/array/concatenate.h>
#include <arrow/io/memory.h>
#include <arrow/ipc/reader.h>
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

#include <operators/npm_basic/modules/dns/npm_dns_contract.h>
#include <operators/npm_basic/modules/http1/npm_http1_contract.h>
#include <operators/npm_basic/npm_basic_result_consumer.h>
#include <operators/npm_basic/npm_family_result_contract.h>
#include <operators/npm_basic/npm_result_progress.h>
#include <operators/npm_basic/output/npm_basic_result_encoder.h>
#include <operators/npm_basic/output/npm_result_router.h>
#include <services/database/database_plugin.h>

namespace {
int PublishForRun(flowsql::npm::INpmFamilyProgressConsumer* publisher, const flowsql::npm::NpmResultContextV1& context,
                  flowsql::npm::NpmFamilyPublicationRequest request) {
    request.key = {"basic_ipv4", "npm_basic_ipv4_history_v1", context.run_id};
    flowsql::npm::NpmFamilyPublishedProgress confirmed;
    return publisher->PublishProgress(request, &confirmed);
}

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

// Simulate a lost acknowledgement after the real commit. The writer must not retry or publish position 2.
class LostPublicationAck final : public IDatabaseChannel, public IDatabasePreparedCommandV1 {
 public:
    explicit LostPublicationAck(IDatabaseChannel* channel)
        : channel_(channel), commands_(dynamic_cast<IDatabasePreparedCommandV1*>(channel)) {}
    const char* Category() override { return channel_->Category(); }
    const char* Name() override { return channel_->Name(); }
    const char* Type() override { return channel_->Type(); }
    const char* Schema() override { return channel_->Schema(); }
    int Open() override { return 0; }
    int Close() override { return 0; }
    int Flush() override { return channel_->Flush(); }
    int CreateWriter(const char* table, flowsql::IBatchWriter** out) override {
        return channel_->CreateWriter(table, out);
    }
    int CreateArrowWriter(const char* table, flowsql::IArrowWriter** out) override {
        return channel_->CreateArrowWriter(table, out);
    }
    int ExecuteQueryArrow(const char* sql, std::vector<std::shared_ptr<arrow::RecordBatch>>* out) override {
        return channel_->ExecuteQueryArrow(sql, out);
    }
    int WriteArrowBatches(const char* table, const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches) override {
        return channel_->WriteArrowBatches(table, batches);
    }
    bool IsOpened() const override { return channel_->IsOpened(); }
    bool IsConnected() override { return channel_->IsConnected(); }
    const char* GetLastError() override { return "injected unknown publication commit"; }
    int ExecuteSql(const char* sql) override { return channel_->ExecuteSql(sql); }
    int CreateReader(const char* sql, flowsql::IBatchReader** out) override { return channel_->CreateReader(sql, out); }
    int CreateArrowReader(const char* sql, flowsql::IArrowReader** out) override {
        return channel_->CreateArrowReader(sql, out);
    }
    int ExecutePrepared(const char* sql, const flowsql::DatabaseParameterV1* parameters, size_t count) override {
        const int rc = commands_->ExecutePrepared(sql, parameters, count);
        return std::string(sql).find("INSERT INTO npm_result_progress_v1") == 0 && rc >= 0 ? -1 : rc;
    }
    int ExecutePreparedBatch(const char* sql, const flowsql::DatabaseParameterV1* parameters, size_t width,
                             size_t count) override {
        return commands_->ExecutePreparedBatch(sql, parameters, width, count);
    }

 private:
    IDatabaseChannel* channel_;
    IDatabasePreparedCommandV1* commands_;
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
                                             bool is_final = true, bool null_optionals = false) {
    std::vector<std::shared_ptr<arrow::Array>> columns;
    columns.reserve(entity.schema->num_fields());
    for (const auto& field : entity.schema->fields()) {
        std::unique_ptr<arrow::ArrayBuilder> builder;
        assert(arrow::MakeBuilder(arrow::default_memory_pool(), field->type(), &builder).ok());
        if (null_optionals && field->nullable()) {
            assert(builder->AppendNull().ok());
        } else
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
                    assert(static_cast<arrow::UInt32Builder*>(builder.get())
                               ->Append(field->name() == "a_ip" || field->name() == "src_ip" ? UINT32_MAX : 0)
                               .ok());
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
                case arrow::Type::FIXED_SIZE_BINARY: {
                    const uint8_t address[16] = {0, 0x80, 0xff, 0, 1, 2, 3, 4, 5, 6, 7, 8, 0, 10, 11, 12};
                    assert(static_cast<arrow::FixedSizeBinaryBuilder*>(builder.get())->Append(address).ok());
                    break;
                }
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

int main(int argc, char** argv) {
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
    {
        std::vector<NpmEntityDescriptorV1> families;
        for (const auto kind : {flowsql::npm::NpmFamilyEntityKind::kBasic, flowsql::npm::NpmFamilyEntityKind::kSession,
                                flowsql::npm::NpmFamilyEntityKind::kDns, flowsql::npm::NpmFamilyEntityKind::kHttp1,
                                flowsql::npm::NpmFamilyEntityKind::kTls, flowsql::npm::NpmFamilyEntityKind::kIcmp})
            for (const auto family : {flowsql::packet::AddressFamily::kIPv4, flowsql::packet::AddressFamily::kIPv6})
                families.push_back(flowsql::npm::NpmFamilyEntityDescriptor(kind, family));
        auto budget = std::make_shared<BoundedBudget>();
        auto storage = CreateConsumer(factory.get(), {"task-family", "run-family"}, families, budget);
        for (const auto& entity : families) {
            auto expected = MakeRows(entity, UINT64_MAX, 1, true, true);
            assert(storage->Consume({"task-family", "run-family"}, entity, *expected) == 0);
            assert(budget->Usage().pending_output_bytes == 0);
            const auto query = "SELECT * FROM npm_" + entity.entity_id + "_history_v1 WHERE __npm_run_id='run-family'";
            flowsql::IBatchReader* raw = nullptr;
            const int opened = channel->CreateReader(query.c_str(), &raw);
            if (opened) std::fprintf(stderr, "family reader failed: %s\n", channel->GetLastError());
            assert(opened == 0 && raw);
            const auto close = [](flowsql::IBatchReader* reader) {
                reader->Close();
                reader->Release();
            };
            std::unique_ptr<flowsql::IBatchReader, decltype(close)> reader(raw, close);
            const uint8_t* data = nullptr;
            size_t size = 0;
            const int next = reader->Next(&data, &size);
            if (next) std::fprintf(stderr, "family read failed: %s\n", reader->GetLastError());
            assert(next == 0);
            auto input = std::make_shared<arrow::io::BufferReader>(
                arrow::Buffer::FromString(std::string(reinterpret_cast<const char*>(data), size)));
            auto stream = arrow::ipc::RecordBatchStreamReader::Open(input).ValueOrDie();
            auto actual = stream->Next().ValueOrDie();
            assert(actual && actual->num_rows() == 1);
            for (int i = 0; i < expected->num_columns(); ++i) {
                const auto index = actual->schema()->GetFieldIndex(entity.schema->field(i)->name());
                assert(index >= 0 && actual->schema()->field(index)->Equals(*entity.schema->field(i), true));
                assert(actual->column(index)->Equals(expected->column(i)));
            }
            assert(actual->schema()->metadata()->Equals(*entity.schema->metadata()));
            const auto address = entity.schema->GetFieldByName("a_ip") ? "a_ip" : "src_ip";
            const auto type = entity.schema->GetFieldByName(address)->type()->id();
            const auto table = "npm_" + entity.entity_id + "_v1_data";
            if (type == arrow::Type::UINT32) {
                assert(channel->ExecuteSql(
                           ("UPDATE " + table + " SET " + address + "=-1 WHERE __npm_run_id='run-family'").c_str()) <
                       0);
                assert(channel->ExecuteSql(
                           ("UPDATE " + table + " SET " + address + "=4294967296 WHERE __npm_run_id='run-family'")
                               .c_str()) < 0);
            } else {
                assert(channel->ExecuteSql(
                           ("UPDATE " + table + " SET " + address + "=X'0080FF' WHERE __npm_run_id='run-family'")
                               .c_str()) < 0);
            }
        }
        assert(storage->Finish() == 0);
    }
    if (argc == 2 && std::string(argv[1]) == "--storage-only") {
        channel.reset();
        assert(plugin.Stop() == 0);
        RemoveSqliteFiles(path);
        std::puts("[PASS] twelve family SQLite storage and exact Schema readback");
        return 0;
    }
    const auto basic = flowsql::npm::NpmBasicEntityDescriptorV1(flowsql::packet::AddressFamily::kIPv4);
    const auto session = flowsql::npm::NpmSessionEntityDescriptorV1(flowsql::packet::AddressFamily::kIPv4);
    const std::vector<NpmEntityDescriptorV1> entities = {basic, session};

    {
        const auto basic6 = flowsql::npm::NpmBasicEntityDescriptorV1(flowsql::packet::AddressFamily::kIPv6);
        const NpmResultContextV1 context{"router-families", "run-router-families"};
        auto budget = std::make_shared<BoundedBudget>();
        auto storage = CreateConsumer(factory.get(), context, {basic, basic6}, budget);
        flowsql::npm::NpmResultRouter router({basic, basic6}, basic.entity_id, context, budget, std::move(storage),
                                             [](const std::function<int()>& call) { return call(); });
        assert(router.Emit("basic", basic.entity_id, *MakeRows(basic, 42, 1)) == 0);
        assert(router.Emit("basic", basic6.entity_id, *MakeRows(basic6, 42, 1)) == 0);
        std::shared_ptr<arrow::RecordBatch> foreground;
        assert(router.Drain(&foreground) == 0 && foreground->num_rows() == 1);
        assert(foreground->schema()->Equals(*basic.schema, true));
        assert(router.Finish() == 0);
        foreground.reset();
        assert(budget->Usage().pending_output_bytes == 0);
        for (const char* family : {"ipv4", "ipv6"}) {
            flowsql::IBatchReader* raw = nullptr;
            const std::string query = "SELECT a_ip AS endpoint FROM npm_basic_" + std::string(family) +
                                      "_history_v1 WHERE __npm_run_id='run-router-families'";
            assert(channel->CreateReader(query.c_str(), &raw) == 0 && raw);
            const uint8_t* data = nullptr;
            size_t size = 0;
            assert(raw->Next(&data, &size) == 0);
            auto input = std::make_shared<arrow::io::BufferReader>(
                arrow::Buffer::FromString(std::string(reinterpret_cast<const char*>(data), size)));
            auto actual = arrow::ipc::RecordBatchStreamReader::Open(input).ValueOrDie()->Next().ValueOrDie();
            assert(actual && actual->num_rows() == 1 && actual->num_columns() == 1);
            const auto entity = std::string(family) == "ipv4" ? basic : basic6;
            assert(
                actual->schema()->field(0)->Equals(*entity.schema->GetFieldByName("a_ip")->WithName("endpoint"), true));
            raw->Close();
            raw->Release();
        }
    }

    {
        const auto basic6 = flowsql::npm::NpmBasicEntityDescriptorV1(flowsql::packet::AddressFamily::kIPv6);
        const NpmResultContextV1 context{"dual-publication", "run-dual-publication"};
        auto dual = CreateConsumer(factory.get(), context, {basic, basic6}, std::make_shared<BoundedBudget>());
        auto* publisher = dynamic_cast<flowsql::npm::INpmFamilyProgressConsumer*>(dual.get());
        assert(publisher);
        auto publish = [&](const NpmEntityDescriptorV1& entity, int64_t closed, int64_t position) {
            flowsql::npm::NpmFamilyPublicationRequest request{
                {entity.entity_id, "npm_" + entity.entity_id + "_history_v1", context.run_id}, 30, closed};
            flowsql::npm::NpmFamilyPublishedProgress fact;
            const int result = publisher->PublishProgress(request, &fact);
            if (!result) {
                assert(fact.committed_position == position && fact.epoch == context.run_id &&
                       fact.publication.key.entity_id == entity.entity_id &&
                       fact.publication.closed_before_ns == closed);
            } else
                assert(fact.committed_position == 0);
            return result;
        };
        assert(publish(basic, 30, 1) == 0 && publish(basic6, 30, 1) == 0);
        assert(publish(basic, 60, 2) == 0);  // Independent relation-local position.
        assert(publish(basic6, 30, 1) == 0);
        assert(channel->ExecuteSql("CREATE TRIGGER fail_second_family BEFORE INSERT ON npm_result_progress_v1 "
                                   "WHEN NEW.run_id='run-dual-publication' AND NEW.entity_id='basic_ipv6' "
                                   "BEGIN SELECT RAISE(ABORT,'second family failed'); END") >= 0);
        assert(publish(basic6, 60, 2) == EIO);
        assert(publish(basic, 90, 3) == EPIPE && dual->Finish() == EPIPE);
        flowsql::npm::NpmResultFailureV1 failure{EIO, "publish", "second family failed"};
        assert(dual->FailRun(failure) == 0);
        assert(channel->ExecuteSql("DROP TRIGGER fail_second_family") >= 0);
    }

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
    assert(probe.Int64("SELECT COUNT(*) FROM npm_result_runs WHERE run_id IN ('legacy-run','run-1')") == 2);
    assert(probe.Int64("SELECT purge_cursor IS NULL FROM npm_result_runs WHERE run_id='legacy-run'") == 1);
    assert(probe.Int64("SELECT COUNT(*) FROM npm_result_entities WHERE entity_id IN ('basic_ipv4','session_ipv4')") ==
           2);
    assert(probe.Int64("SELECT COUNT(*) FROM npm_basic_ipv4_v1_data WHERE __npm_run_id='run-1'") == 1);
    assert(probe.Int64("SELECT COUNT(*) FROM npm_session_ipv4_v1_data WHERE __npm_run_id='run-1'") == 1);
    assert(probe.Text("SELECT session_id FROM npm_basic_ipv4_v1_data WHERE __npm_run_id='run-1'") ==
           std::to_string(maximum));
    assert(probe.Text("SELECT revision FROM npm_basic_ipv4_v1_data WHERE __npm_run_id='run-1'") ==
           std::to_string(maximum - 1));
    assert(probe.Text("SELECT status FROM npm_result_runs WHERE run_id='run-1'") == "completed");
    assert(probe.Text("SELECT metadata_status FROM npm_result_runs WHERE run_id='run-1'") == "known");
    assert(probe.Int64("SELECT completed_at_ns IS NOT NULL FROM npm_result_runs WHERE run_id='run-1'") == 1);
    assert(probe.Text("SELECT type FROM sqlite_master WHERE name='npm_basic_ipv4_history_v1'") == "view");
    assert(probe.Text("SELECT type FROM sqlite_master WHERE name='npm_basic_ipv4_latest_v1'") == "view");
    assert(probe.Text("SELECT type FROM sqlite_master WHERE name='npm_basic_ipv4_final_v1'") == "view");
    assert(probe.Int64("SELECT COUNT(*) FROM sqlite_master WHERE name IN "
                       "('npm_basic_ipv4_history','npm_basic_ipv4_latest','npm_basic_ipv4_final')") == 0);

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
    assert(probe.Int64("SELECT COUNT(*) FROM npm_basic_ipv4_history_v1 WHERE __npm_run_id='run-relations'") == 3);
    assert(probe.Int64("SELECT COUNT(*) FROM npm_basic_ipv4_latest_v1 WHERE __npm_run_id='run-relations'") == 1);
    assert(probe.Text("SELECT revision FROM npm_basic_ipv4_latest_v1 WHERE __npm_run_id='run-relations'") ==
           std::to_string(maximum));
    assert(probe.Int64("SELECT COUNT(*) FROM npm_basic_ipv4_final_v1 WHERE __npm_run_id='run-relations'") == 1);
    assert(probe.Text("SELECT revision FROM npm_basic_ipv4_final_v1 WHERE __npm_run_id='run-relations'") == "10");
    assert(probe.Int64("SELECT COUNT(*) FROM npm_session_ipv4_latest_v1 WHERE __npm_run_id='run-relations' "
                       "AND session_id='101' AND revision='8'") == 1);
    assert(probe.Int64("SELECT COUNT(*) FROM npm_event_probe_history_v7 WHERE __npm_run_id='run-relations'") == 1);
    assert(probe.Int64("SELECT COUNT(*) FROM npm_event_probe_latest_v7 WHERE __npm_run_id='run-relations'") == 1);
    assert(probe.Int64("SELECT COUNT(*) FROM npm_event_probe_final_v7 WHERE __npm_run_id='run-relations'") == 1);
    assert(probe.Text("SELECT __npm_run_status FROM npm_event_probe_history_v7 WHERE "
                      "__npm_run_id='run-relations'") == "completed");

    const auto dns = flowsql::npm::NpmDnsTransactionEntityDescriptorV1(flowsql::packet::AddressFamily::kIPv4);
    auto dns_first = CreateConsumer(factory.get(), {"task-dns", "run-dns-1"}, {dns}, std::make_shared<BoundedBudget>());
    assert(dns_first->Consume({"task-dns", "run-dns-1"}, dns, *MakeRows(dns, 9001, 1)) == 0);
    assert(dns_first->Finish() == 0);
    AssertSummary(dns_first->ResultJson(), "run-dns-1", "completed", 1, 1);
    auto dns_second =
        CreateConsumer(factory.get(), {"task-dns", "run-dns-2"}, {dns}, std::make_shared<BoundedBudget>());
    assert(dns_second->Consume({"task-dns", "run-dns-2"}, dns, *MakeRows(dns, 9001, 1)) == 0);
    assert(dns_second->Finish() == 0);
    for (const char* relation : {"history", "latest", "final"}) {
        const std::string name = std::string("npm_dns_transaction_ipv4_") + relation + "_v1";
        assert(probe.Text("SELECT type FROM sqlite_master WHERE name='" + name + "'") == "view");
        assert(probe.Int64("SELECT COUNT(*) FROM " + name + " WHERE __npm_run_id='run-dns-1'") == 1);
        assert(probe.Int64("SELECT COUNT(*) FROM " + name + " WHERE __npm_run_id='run-dns-2'") == 1);
        assert(probe.Int64("SELECT COUNT(*) FROM " + name + " WHERE __npm_run_id='run-1'") == 0);
    }
    assert(probe.Text("SELECT entity_instance_id FROM npm_dns_transaction_ipv4_final_v1 WHERE "
                      "__npm_run_id='run-dns-1'") == "9001");
    assert(probe.Int64("SELECT schema_version FROM npm_result_entities WHERE entity_id='dns_transaction_ipv4'") == 1);

    const auto http1 = flowsql::npm::NpmHttp1TransactionEntityDescriptorV1(flowsql::packet::AddressFamily::kIPv4);
    auto http_first =
        CreateConsumer(factory.get(), {"task-http1", "run-http1-1"}, {http1}, std::make_shared<BoundedBudget>());
    assert(http_first->Consume({"task-http1", "run-http1-1"}, http1, *MakeRows(http1, 7001, 1)) == 0);
    assert(http_first->Finish() == 0);
    AssertSummary(http_first->ResultJson(), "run-http1-1", "completed", 1, 1);
    auto http_second =
        CreateConsumer(factory.get(), {"task-http1", "run-http1-2"}, {http1}, std::make_shared<BoundedBudget>());
    assert(http_second->Consume({"task-http1", "run-http1-2"}, http1, *MakeRows(http1, 7001, 1)) == 0);
    assert(http_second->Finish() == 0);
    for (const char* relation : {"history", "latest", "final"}) {
        const std::string name = std::string("npm_http1_transaction_ipv4_") + relation + "_v1";
        assert(probe.Text("SELECT type FROM sqlite_master WHERE name='" + name + "'") == "view");
        assert(probe.Int64("SELECT COUNT(*) FROM " + name + " WHERE __npm_run_id='run-http1-1'") == 1);
        assert(probe.Int64("SELECT COUNT(*) FROM " + name + " WHERE __npm_run_id='run-http1-2'") == 1);
        assert(probe.Int64("SELECT COUNT(*) FROM " + name + " WHERE __npm_run_id='run-1'") == 0);
    }
    assert(probe.Text("SELECT entity_instance_id FROM npm_http1_transaction_ipv4_final_v1 WHERE "
                      "__npm_run_id='run-http1-1'") == "7001");
    assert(probe.Int64("SELECT schema_version FROM npm_result_entities WHERE entity_id='http1_transaction_ipv4'") == 1);

    auto second_budget = std::make_shared<BoundedBudget>();
    auto second = CreateConsumer(factory.get(), {"task-repeat", "run-2"}, entities, second_budget);
    assert(second->Consume({"task-repeat", "run-2"}, basic, *MakeRows(basic, 101, 1)) == 0);
    assert(second->Finish() == 0);
    assert(probe.Int64("SELECT COUNT(*) FROM npm_result_runs WHERE task_id='task-repeat'") == 2);
    assert(probe.Int64("SELECT COUNT(*) FROM npm_basic_ipv4_v1_data WHERE __npm_run_id IN ('run-1','run-2')") == 2);
    assert(probe.Int64("SELECT COUNT(*) FROM npm_basic_ipv4_latest_v1 WHERE session_id='101' AND "
                       "__npm_run_id IN ('run-relations','run-2')") == 2);
    assert(probe.Text("SELECT revision FROM npm_basic_ipv4_latest_v1 WHERE __npm_run_id='run-2'") == "1");

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
    assert(probe.Int64("SELECT COUNT(*) FROM npm_session_ipv4_v1_data WHERE __npm_run_id='run-failed'") == 1);
    assert(probe.Int64("SELECT COUNT(*) FROM npm_session_ipv4_history_v1 WHERE __npm_run_id='run-failed'") == 1);
    assert(probe.Text("SELECT __npm_run_status FROM npm_session_ipv4_history_v1 WHERE __npm_run_id='run-failed'") ==
           "incomplete");

    const auto labeled_basic = flowsql::npm::NpmBasicEntityDescriptorV1(flowsql::packet::AddressFamily::kIPv4);
    auto labeled = CreateConsumer(factory.get(), {"task-labeled", "run-labeled"}, {labeled_basic},
                                  std::make_shared<BoundedBudget>());
    assert(labeled->Consume({"task-labeled", "run-labeled"}, labeled_basic, *MakeRows(labeled_basic, 101, 1)) == 0);
    assert(labeled->Finish() == 0);
    assert(probe.Int64("SELECT COUNT(*) FROM npm_basic_ipv4_history_v1 WHERE __npm_run_id='run-labeled'") == 1);
    assert(probe.Int64("SELECT COUNT(*) FROM sqlite_master WHERE name='npm_basic_ipv4_history_v2'") == 0);

    {
        auto late = CreateConsumer(factory.get(), {"task-late", "run-late"}, {labeled_basic},
                                   std::make_shared<BoundedBudget>());
        auto* publisher = dynamic_cast<flowsql::npm::INpmFamilyProgressConsumer*>(late.get());
        flowsql::npm::NpmBasicResult4 row;
        row.session_id = 10001;
        row.revision = 1;
        row.protocol_status = flowsql::npm::NpmProtocolStatus::kUnknown;
        row.period = flowsql::npm::NpmBasicPeriodStats{0, 30000000000LL, true, 1, 0, 1, 0, 1};
        std::shared_ptr<arrow::RecordBatch> encoded;
        assert(flowsql::npm::EncodeNpmBasicResults({row}, &encoded) == flowsql::npm::NpmBasicEncodeError::kNone);
        assert(late->Consume({"task-late", "run-late"}, labeled_basic, *encoded) == 0);
        flowsql::npm::NpmFamilyPublicationRequest progress;
        progress.period_ns = progress.closed_before_ns = 30000000000LL;
        assert(PublishForRun(publisher, {"task-late", "run-late"}, progress) == 0);
        row.revision = 2;
        assert(flowsql::npm::EncodeNpmBasicResults({row}, &encoded) == flowsql::npm::NpmBasicEncodeError::kNone);
        assert(late->Consume({"task-late", "run-late"}, labeled_basic, *encoded) == EINVAL);
        progress.closed_before_ns *= 2;
        assert(PublishForRun(publisher, {"task-late", "run-late"}, progress) == EPIPE);
        assert(probe.Int64("SELECT COUNT(*) FROM npm_basic_ipv4_v1_data WHERE __npm_run_id='run-late'") == 1);
        assert(probe.Int64("SELECT COUNT(*) FROM npm_result_progress_v1 WHERE run_id='run-late'") == 1);
    }
    {
        auto failing = CreateConsumer(factory.get(), {"task-publish-fail", "run-publish-fail"}, {labeled_basic},
                                      std::make_shared<BoundedBudget>());
        probe.Execute(
            "CREATE TRIGGER fail_progress BEFORE INSERT ON npm_result_progress_v1 "
            "WHEN NEW.run_id='run-publish-fail' BEGIN SELECT RAISE(ABORT,'publication failed'); END");
        auto* publisher = dynamic_cast<flowsql::npm::INpmFamilyProgressConsumer*>(failing.get());
        flowsql::npm::NpmFamilyPublicationRequest progress;
        progress.period_ns = progress.closed_before_ns = 30000000000LL;
        assert(PublishForRun(publisher, {"task-publish-fail", "run-publish-fail"}, progress) == EIO);
        assert(PublishForRun(publisher, {"task-publish-fail", "run-publish-fail"}, progress) == EPIPE);
        assert(probe.Int64("SELECT COUNT(*) FROM npm_result_progress_v1 WHERE run_id='run-publish-fail'") == 0);
        probe.Execute("DROP TRIGGER fail_progress");
    }

    {
        LostPublicationAck lost_ack(channel.get());
        auto lost_factory = flowsql::npm::MakeNpmDatabaseResultConsumerFactory(&lost_ack, "netadapter.ack");
        auto unknown = CreateConsumer(lost_factory.get(), {"task-publication-unknown", "run-publication-unknown"},
                                      {labeled_basic}, std::make_shared<BoundedBudget>());
        auto* publisher = dynamic_cast<flowsql::npm::INpmFamilyProgressConsumer*>(unknown.get());
        flowsql::npm::NpmFamilyPublicationRequest progress;
        progress.period_ns = progress.closed_before_ns = 30000000000LL;
        assert(PublishForRun(publisher, {"task-publication-unknown", "run-publication-unknown"}, progress) == EIO);
        assert(probe.Int64("SELECT COUNT(*) FROM npm_result_progress_v1 WHERE run_id='run-publication-unknown'") == 1);
        progress.closed_before_ns *= 2;
        assert(PublishForRun(publisher, {"task-publication-unknown", "run-publication-unknown"}, progress) == EPIPE);
        assert(unknown->Finish() == EPIPE);
        assert(probe.Int64("SELECT MAX(position) FROM npm_result_progress_v1 WHERE run_id='run-publication-unknown'") ==
               1);
    }

    for (int labels : {0, 1, 2}) {
        const std::string run = "run-periodic-" + std::to_string(labels);
        auto periodic =
            CreateConsumer(factory.get(), {"task-periodic", run}, {labeled_basic}, std::make_shared<BoundedBudget>());
        flowsql::npm::NpmBasicResult4 row;
        row.session_id = 101;
        row.revision = 1;
        row.protocol_status = flowsql::npm::NpmProtocolStatus::kUnknown;
        row.primary_label_id = labels == 2 ? 17 : 0;
        row.wire_bytes_ab = maximum - 10;
        row.wire_bytes_total = maximum - 10;
        row.period = flowsql::npm::NpmBasicPeriodStats{0, 30000000000LL, true, 1, 0, maximum - 10, 0, maximum - 10};
        std::vector<flowsql::npm::NpmBasicResult4> records{row};
        row.revision = 2;
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
        assert(periodic->Consume({"task-periodic", run}, labeled_basic, *encoded) == 0);
        auto* publisher = dynamic_cast<flowsql::npm::INpmFamilyProgressConsumer*>(periodic.get());
        assert(publisher);
        flowsql::npm::NpmFamilyPublicationRequest progress;
        progress.period_ns = 30000000000LL;
        progress.closed_before_ns = 60000000000LL;
        assert(PublishForRun(publisher, {"task-periodic", run}, progress) == 0);
        assert(PublishForRun(publisher, {"task-periodic", run}, progress) == 0);  // idempotent boundary
        assert(PublishForRun(publisher, {"task-periodic", "wrong-run"}, progress) == EINVAL);
        progress.closed_before_ns = 90000000000LL;
        assert(PublishForRun(publisher, {"task-periodic", run}, progress) == 0);  // no data progression
        assert(probe.Int64("SELECT COUNT(*) FROM npm_result_progress_v1 WHERE run_id='" + run + "'") == 2);
        assert(probe.Int64("SELECT MAX(position) FROM npm_result_progress_v1 WHERE run_id='" + run + "'") == 2);
        assert(probe.Text("SELECT epoch FROM npm_result_progress_v1 WHERE run_id='" + run + "' AND position=2") == run);
        assert(probe.Int64("SELECT closed_before_ns FROM npm_result_progress_v1 WHERE run_id='" + run +
                           "' AND position=2") == 90000000000LL);
        assert(periodic->Finish() == 0);
        const std::string where = " WHERE __npm_run_id='" + run + "'";
        assert(probe.Int64("SELECT COUNT(*) FROM npm_basic_ipv4_history_v1" + where) == 3);
        assert(probe.Text("SELECT interval_wire_bytes_total FROM npm_basic_ipv4_history_v1" + where +
                          " AND revision='1'") == std::to_string(maximum - 10));
        assert(probe.Text("SELECT interval_wire_bytes_total FROM npm_basic_ipv4_history_v1" + where +
                          " AND revision='2'") == "10");
        assert(probe.Text("SELECT interval_wire_bytes_total FROM npm_basic_ipv4_final_v1" + where) == "0");
        assert(probe.Text("SELECT wire_bytes_total FROM npm_basic_ipv4_final_v1" + where) == std::to_string(maximum));
        assert(probe.Text("SELECT revision FROM npm_basic_ipv4_latest_v1" + where) == "3");
        assert(probe.Int64("SELECT COUNT(*) FROM npm_basic_ipv4_history_v1" + where +
                           " AND primary_label_id IS NULL") == (labels == 0 ? 3 : 0));
        if (labels)
            assert(probe.Int64("SELECT primary_label_id FROM npm_basic_ipv4_final_v1" + where) ==
                   (labels == 2 ? 17 : 0));
    }

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
    assert(probe.Int64("SELECT COUNT(*) FROM npm_basic_ipv4_v1_data WHERE __npm_run_id='run-budget'") == 0);

    auto atomic_batch =
        CreateConsumer(factory.get(), {"task-atomic", "run-atomic"}, entities, std::make_shared<BoundedBudget>());
    assert(atomic_batch->Consume({"task-atomic", "run-atomic"}, basic, *DuplicateRows(MakeRows(basic, 41, 1))) == EIO);
    assert(probe.Int64("SELECT COUNT(*) FROM npm_basic_ipv4_v1_data WHERE __npm_run_id='run-atomic'") == 0);
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
    assert(probe.Int64("SELECT COUNT(*) FROM npm_basic_ipv4_history_v1 WHERE __npm_run_id='run-retained'") == 0);
    assert(probe.Int64("SELECT COUNT(*) FROM npm_basic_ipv4_history_v1 WHERE __npm_run_id='run-writing'") == 0);
    probe.Execute(
        "CREATE TRIGGER npm_purge_fail BEFORE DELETE ON npm_basic_ipv4_v1_data BEGIN SELECT RAISE(ABORT,'injected'); "
        "END");
    auto advance = [&](int step) {
        auto next = CreateConsumer(factory.get(), {"task-advance", "run-advance-" + std::to_string(step)}, {basic},
                                   std::make_shared<BoundedBudget>());
        assert(next->Finish() == 0);
    };
    advance(0);
    assert(probe.Text("SELECT status FROM npm_result_runs WHERE run_id='run-retained'") == "purging");
    assert(probe.Int64("SELECT COUNT(*) FROM npm_basic_ipv4_v1_data WHERE __npm_run_id='run-retained'") == 66);
    probe.Execute("DROP TRIGGER npm_purge_fail");
    advance(1);
    assert(probe.Int64("SELECT COUNT(*) FROM npm_basic_ipv4_v1_data WHERE __npm_run_id='run-retained'") == 2);
    advance(2);
    assert(probe.Int64("SELECT COUNT(*) FROM npm_basic_ipv4_v1_data WHERE __npm_run_id='run-retained'") == 0);
    assert(probe.Text("SELECT purge_cursor FROM npm_result_runs WHERE run_id='run-retained'") ==
           "npm_basic_ipv4_v1_data");
    advance(3);
    advance(4);
    advance(5);
    advance(6);
    advance(7);
    advance(8);
    for (int step = 9; step < 18; ++step) advance(step);
    assert(probe.Int64("SELECT COUNT(*) FROM npm_result_runs WHERE run_id='run-retained'") == 0);
    assert(probe.Int64("SELECT COUNT(*) FROM npm_session_ipv4_v1_data WHERE __npm_run_id='run-retained'") == 0);
    assert(probe.Text("SELECT status FROM npm_result_runs WHERE run_id='run-writing'") == "writing");
    assert(probe.Int64("SELECT COUNT(*) FROM npm_basic_ipv4_v1_data WHERE __npm_run_id='run-writing'") == 1);
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
    probe.Execute("DROP TABLE npm_basic_ipv4_v1_data");
    probe.Execute("CREATE TABLE npm_basic_ipv4_v1_data(__npm_run_id TEXT NOT NULL PRIMARY KEY)");
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
