// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include "durable_store.h"
#include <arrow/util/byte_size.h>
#include <openssl/sha.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <array>
#include <cmath>
#include <cstring>
#include <set>
namespace flowsql::baseliner {
namespace {
std::string Hash(std::string_view value) {
    std::array<unsigned char, SHA256_DIGEST_LENGTH> hash{};
    SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), hash.data());
    std::string result;
    const char* digits = "0123456789abcdef";
    for (auto c : hash) {
        result += digits[c >> 4];
        result += digits[c & 15];
    }
    return result;
}
std::string Text(const arrow::RecordBatch& b, int column, int64_t row) {
    return std::static_pointer_cast<arrow::StringArray>(b.column(column))->GetString(row);
}
DatabaseParameterV1 ScalarParameter(const arrow::Scalar& scalar) {
    DatabaseParameterV1 p;
    if (!scalar.is_valid) return p;
    switch (scalar.type->id()) {
        case arrow::Type::STRING:
        case arrow::Type::BINARY: {
            auto& s = static_cast<const arrow::BaseBinaryScalar&>(scalar);
            p.kind = scalar.type->id() == arrow::Type::STRING ? DatabaseParameterKindV1::kString
                                                              : DatabaseParameterKindV1::kBlob;
            p.data = s.value->data();
            p.size = s.value->size();
            break;
        }
        case arrow::Type::INT64:
            p = IntParameter(static_cast<const arrow::Int64Scalar&>(scalar).value);
            break;
        case arrow::Type::INT32:
            p = IntParameter(static_cast<const arrow::Int32Scalar&>(scalar).value);
            break;
        case arrow::Type::UINT64:
            p.kind = DatabaseParameterKindV1::kUInt64;
            p.uint64_value = static_cast<const arrow::UInt64Scalar&>(scalar).value;
            break;
        case arrow::Type::DOUBLE:
            p.kind = DatabaseParameterKindV1::kDouble;
            p.double_value = static_cast<const arrow::DoubleScalar&>(scalar).value;
            break;
        case arrow::Type::BOOL:
            p = IntParameter(static_cast<const arrow::BooleanScalar&>(scalar).value);
            break;
        default:
            throw std::runtime_error("unsupported result scalar");
    }
    return p;
}
}  // namespace
std::string RestoreCompatibilityHash(const ConfigSnapshot& config) {
    rapidjson::Document doc;
    doc.Parse<rapidjson::kParseFullPrecisionFlag>(config.original_json.data(), config.original_json.size());
    if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("schema_version")) return config.sha256_hex;
    if (doc.HasMember("persistence") && doc["persistence"].IsObject()) {
        auto& p = doc["persistence"];
        auto restore = p.FindMember("restore");
        if (restore != p.MemberEnd()) p.EraseMember(restore);
        if (p.ObjectEmpty()) doc.EraseMember(doc.FindMember("persistence"));
    }
    rapidjson::StringBuffer b;
    rapidjson::Writer<rapidjson::StringBuffer> w(b);
    doc.Accept(w);
    return Hash({b.GetString(), b.GetSize()});
}
int DurableStore::Fail(std::string error) {
    failed_ = true;
    error_ = std::move(error);
    return -1;
}
std::string DurableStore::Quote(const std::string& id) const {
    return backend_ == "mysql" ? "`" + id + "`" : "\"" + id + "\"";
}
int DurableStore::Open(const ConfigSnapshot& config, IDatabaseChannel* channel, const std::string& owner) {
    if (session_ || cancelled_ || !channel || !channel->IsOpened()) return Fail("invalid managed target");
    backend_ = channel->Category();
    if (backend_ != "sqlite" && backend_ != "mysql" && backend_ != "postgres")
        return Fail("target does not support atomic publication");
    auto* target = dynamic_cast<IDatabaseAtomicTargetV1*>(channel);
    if (!target) return Fail("missing atomic target capability");
    config_ = config;
    DatabaseAtomicSessionOptionsV1 options;
    options.operation_timeout_ms = config.config.persistence.operation_timeout_ms;
    std::shared_ptr<IDatabaseAtomicSessionV1> session;
    auto status = target->AcquireAtomicSession(options, &session);
    if (!status.ok()) return Fail(status.message);
    std::atomic_store(&session_, session);
    if (cancelled_) {
        session->Cancel();
        return Fail("managed target cancelled during open");
    }
    if (fence_.Acquire(session, backend_, config.config.task_key, owner, RestoreCompatibilityHash(config),
                       config.config.persistence.lease_ms) != 0)
        return Fail(fence_.LastError());
    if (Initialize() != 0) return -1;
    if (config.config.persistence.restore == "fresh" && fence_.Generation() != 0)
        return Fail("fresh requires a new task_key without a model");
    if (config.config.persistence.restore == "require" && fence_.Generation() == 0)
        return Fail("required model missing");
    return 0;
}
int DurableStore::Initialize() {
    auto& session = fence_.Session();
    auto status = session.Begin();
    if (!status.ok()) return Fail(status.message);
    const std::string text = backend_ == "mysql" ? "LONGTEXT" : "TEXT";
    const std::string task_type = backend_ == "mysql" ? "VARBINARY(1024)" : "VARCHAR(256)";
    const std::vector<std::string> statements = {
        "CREATE TABLE IF NOT EXISTS baseline_model_versions(task_key " + task_type +
            " NOT NULL,generation BIGINT NOT "
            "NULL,payload_version BIGINT NOT NULL,config_hash VARCHAR(128) NOT NULL,config_json " +
            text + " NOT NULL,checksum VARCHAR(64) NOT NULL,checkpoint " + text +
            " NOT NULL,PRIMARY KEY(task_key,generation))",
        "CREATE TABLE IF NOT EXISTS baseline_source_positions(task_key " + task_type +
            " NOT NULL,generation BIGINT NOT "
            "NULL,dataset_id VARCHAR(128) NOT NULL,source_epoch " +
            text + " NOT NULL,committed_position " + text +
            " NOT NULL,closed_before_bucket BIGINT NOT NULL,PRIMARY KEY(task_key,generation,dataset_id))"};
    uint64_t n = 0;
    for (const auto& sql : statements) {
        status = session.ExecutePrepared(sql.c_str(), nullptr, 0, &n);
        if (!status.ok()) {
            session.Rollback();
            return Fail(status.message);
        }
    }
    for (auto kind : {SchemaKind::kResults, SchemaKind::kRelationFusion, SchemaKind::kMaintenance}) {
        auto schema = MakeSchema(kind);
        std::string table = kind == SchemaKind::kResults          ? "baseline_results_v1"
                            : kind == SchemaKind::kRelationFusion ? "baseline_relation_fusion_v1"
                                                                  : "baseline_maintenance_v1";
        std::string sql = "CREATE TABLE IF NOT EXISTS " + table + "(logical_key VARCHAR(64) PRIMARY KEY";
        for (const auto& f : schema->fields()) {
            std::string type;
            switch (f->type()->id()) {
                case arrow::Type::STRING:
                    type = text;
                    break;
                case arrow::Type::BINARY:
                    type = backend_ == "postgres" ? "BYTEA" : backend_ == "mysql" ? "LONGBLOB" : "BLOB";
                    break;
                case arrow::Type::DOUBLE:
                    type = "DOUBLE PRECISION";
                    break;
                case arrow::Type::UINT64:
                    type = backend_ == "sqlite" ? "TEXT" : backend_ == "mysql" ? "BIGINT UNSIGNED" : "NUMERIC(20,0)";
                    break;
                default:
                    type = "BIGINT";
            }
            // task_key is indexed for query/pruning; all other fields retain their frozen logical values.
            if (f->name() == "task_key") type = task_type;
            sql += "," + Quote(f->name()) + " " + type + (f->nullable() ? "" : " NOT NULL");
        }
        sql += ")";
        status = session.ExecutePrepared(sql.c_str(), nullptr, 0, &n);
        if (!status.ok()) {
            session.Rollback();
            return Fail(status.message);
        }
        auto index = "baseline_" + std::to_string(static_cast<int>(kind)) + "_task_generation";
        if (backend_ != "mysql") {
            sql = "CREATE INDEX IF NOT EXISTS " + index + " ON " + table + "(task_key,published_generation)";
            status = session.ExecutePrepared(sql.c_str(), nullptr, 0, &n);
            if (!status.ok()) {
                session.Rollback();
                return Fail(status.message);
            }
        }
    }
    auto result = session.Commit();
    return result.status.ok() ? 0 : Fail(result.status.message);
}
int DurableStore::Load(StoredGeneration* output) {
    if (!output || failed_) return Fail("store unavailable");
    *output = {};
    output->generation = fence_.Generation();
    if (!output->generation) return 0;
    auto& session = fence_.Session();
    auto status = session.Begin();
    if (!status.ok()) return Fail(status.message);
    std::shared_ptr<arrow::RecordBatch> batch;
    auto p = std::vector<DatabaseParameterV1>{TextParameter(config_.config.task_key), IntParameter(output->generation)};
    status = ReadAtomicBatch(session,
                             "SELECT config_hash,checksum,checkpoint,config_json FROM baseline_model_versions WHERE "
                             "task_key=? AND generation=?",
                             p, &batch);
    if (!status.ok() || batch->num_rows() != 1) {
        session.Rollback();
        return Fail("current model missing/corrupt");
    }
    output->checkpoint = Text(*batch, 2, 0);
    ConfigSnapshot saved_config;
    saved_config.original_json = Text(*batch, 3, 0);
    saved_config.sha256_hex = Text(*batch, 0, 0);
    if (RestoreCompatibilityHash(saved_config) != RestoreCompatibilityHash(config_) ||
        Text(*batch, 1, 0) != Hash(output->checkpoint) ||
        output->checkpoint.size() > config_.config.persistence.max_checkpoint_bytes) {
        session.Rollback();
        return Fail("model config/checksum/size mismatch");
    }
    status = ReadAtomicBatch(session,
                             "SELECT dataset_id,source_epoch,committed_position,closed_before_bucket FROM "
                             "baseline_source_positions WHERE task_key=? AND generation=? ORDER BY dataset_id",
                             p, &batch);
    if (!status.ok()) {
        session.Rollback();
        return Fail(status.message);
    }
    for (int64_t i = 0; i < batch->num_rows(); ++i)
        output->progress.datasets.push_back({Text(*batch, 0, i), Text(*batch, 1, i), Text(*batch, 2, i),
                                             std::static_pointer_cast<arrow::Int64Array>(batch->column(3))->Value(i)});
    auto commit = session.Commit();
    return commit.status.ok() ? 0 : Fail(commit.status.message);
}
int DurableStore::InsertResults(const arrow::RecordBatch& batch, int64_t generation) {
    SchemaKind kind;
    if (batch.schema()->Equals(*MakeSchema(SchemaKind::kResults), true))
        kind = SchemaKind::kResults;
    else if (batch.schema()->Equals(*MakeSchema(SchemaKind::kRelationFusion), true))
        kind = SchemaKind::kRelationFusion;
    else if (batch.schema()->Equals(*MakeSchema(SchemaKind::kMaintenance), true))
        kind = SchemaKind::kMaintenance;
    else
        return Fail("result schema mismatch");
    std::string table = kind == SchemaKind::kResults          ? "baseline_results_v1"
                        : kind == SchemaKind::kRelationFusion ? "baseline_relation_fusion_v1"
                                                              : "baseline_maintenance_v1";
    std::string sql = "INSERT INTO " + table + "(logical_key";
    std::string marks = "?";
    for (const auto& f : batch.schema()->fields()) {
        sql += "," + Quote(f->name());
        marks += ",?";
    }
    sql += ") VALUES(" + marks + ")";
    for (int64_t row = 0; row < batch.num_rows(); ++row) {
        std::vector<std::shared_ptr<arrow::Scalar>> scalars;
        std::vector<DatabaseParameterV1> parameters(1);
        for (int i = 0; i < batch.num_columns(); ++i) {
            auto scalar = batch.column(i)->GetScalar(row);
            if (!scalar.ok()) return Fail(scalar.status().ToString());
            scalars.push_back(*scalar);
            parameters.push_back(ScalarParameter(**scalar));
        }
        if (Text(batch, batch.schema()->GetFieldIndex("task_key"), row) != config_.config.task_key ||
            Text(batch, batch.schema()->GetFieldIndex("config_hash"), row) != config_.sha256_hex)
            return Fail("result task/config mismatch");
        parameters[1 + batch.schema()->GetFieldIndex("published_generation")] = IntParameter(generation);
        std::vector<std::string> keys = kind == SchemaKind::kResults
                                            ? ResultKeyFields(true, true)
                                            : std::vector<std::string>{"task_key",   "dataset_id",   "metric_id",
                                                                       "series_key", "source_epoch", "target_bucket"};
        if (kind == SchemaKind::kMaintenance) keys.push_back("event_kind");
        std::string key = table;
        for (const auto& name : keys) {
            auto i = batch.schema()->GetFieldIndex(name);
            const auto& scalar = *scalars[i];
            auto value = scalar.ToString();
            key += ":" + std::to_string(scalar.type->id()) + ":" + (scalar.is_valid ? "value:" : "null:") +
                   std::to_string(value.size()) + ":" + value;
        }
        if (kind == SchemaKind::kMaintenance) key += ":generation:" + std::to_string(generation);
        auto digest = Hash(key);
        parameters[0] = TextParameter(digest);
        uint64_t n = 0;
        auto status = fence_.Session().ExecutePrepared(sql.c_str(), parameters.data(), parameters.size(), &n);
        if (!status.ok() || n != 1) return Fail(status.ok() ? "result insert count mismatch" : status.message);
    }
    return 0;
}
int DurableStore::Publish(const std::string& checkpoint, const BlockInputProgressV1& progress,
                          const std::vector<std::shared_ptr<arrow::RecordBatch>>& results) {
    if (failed_ || checkpoint.empty() || checkpoint.size() > config_.config.persistence.max_checkpoint_bytes)
        return Fail("checkpoint size/store unavailable");
    std::vector<std::string> ids;
    for (const auto& d : config_.config.datasets) ids.push_back(d.id);
    if (!ValidateProgress(progress, ids).ok()) return Fail("invalid persisted source positions");
    uint64_t bytes = 0;
    for (const auto& b : results) {
        if (!b) return Fail("null result batch");
        bytes += arrow::util::TotalBufferSize(*b);
        if (bytes > config_.config.read.max_pending_bytes) return Fail("unpublished result byte limit");
    }
    if (fence_.Stage() != 0) return Fail(fence_.LastError());
    auto& session = fence_.Session();
    const auto generation = fence_.Generation() + 1;
    uint64_t n = 0;
    auto checksum = Hash(checkpoint);
    auto p = std::vector<DatabaseParameterV1>{TextParameter(config_.config.task_key),
                                              IntParameter(generation),
                                              IntParameter(1),
                                              TextParameter(config_.sha256_hex),
                                              TextParameter(config_.original_json),
                                              TextParameter(checksum),
                                              TextParameter(checkpoint)};
    auto status =
        session.ExecutePrepared("INSERT INTO baseline_model_versions VALUES(?,?,?,?,?,?,?)", p.data(), p.size(), &n);
    if (!status.ok()) {
        session.Rollback();
        return Fail(status.message);
    }
    for (const auto& position : progress.datasets) {
        p = {TextParameter(config_.config.task_key),     IntParameter(generation),
             TextParameter(position.dataset_id),         TextParameter(position.epoch),
             TextParameter(position.committed_position), IntParameter(position.closed_before_bucket)};
        status = session.ExecutePrepared("INSERT INTO baseline_source_positions VALUES(?,?,?,?,?,?)", p.data(),
                                         p.size(), &n);
        if (!status.ok()) {
            session.Rollback();
            return Fail(status.message);
        }
    }
    for (const auto& b : results)
        if (InsertResults(*b, generation) != 0) {
            session.Rollback();
            return -1;
        }
    const auto oldest = generation - config_.config.persistence.retain_generations + 1;
    for (const auto* table : {"baseline_source_positions", "baseline_model_versions"}) {
        p = {TextParameter(config_.config.task_key), IntParameter(oldest)};
        const auto sql = std::string("DELETE FROM ") + table + " WHERE task_key=? AND generation<?";
        status = session.ExecutePrepared(sql.c_str(), p.data(), p.size(), &n);
        if (!status.ok()) {
            session.Rollback();
            return Fail(status.message);
        }
    }
    const auto committed = session.Commit();
    if (fence_.Confirm(committed) != 0) return Fail(fence_.LastError());
    return 0;
}
}  // namespace flowsql::baseliner
