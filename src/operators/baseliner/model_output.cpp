// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include "model_output.h"
#include <arrow/util/byte_size.h>
#include <openssl/sha.h>
#include <algorithm>
#include <cctype>
#include <filesystem>
namespace flowsql::baseliner {
namespace {
std::string Cell(const arrow::RecordBatch& batch, int col, int64_t row) {
    const auto value = batch.column(col)->GetScalar(row);
    if (!value.ok() || !(*value)->is_valid) return {};
    return (*value)->ToString();
}
std::string Lower(std::string value) {
    for (auto& c : value) c = std::tolower(static_cast<unsigned char>(c));
    return value;
}
std::string LogicalKey(const arrow::RecordBatch& batch, int64_t row, SchemaKind kind) {
    std::string identity;
    if (kind == SchemaKind::kResults) {
        identity = "baseline_results_v1";
        for (const auto& field : ResultKeyFields(true, true)) {
            const auto scalar = batch.GetColumnByName(field)->GetScalar(row).ValueOrDie();
            const auto value = scalar->ToString();
            identity += ":" + std::to_string(scalar->type->id()) + ":" + (scalar->is_valid ? "value:" : "null:") +
                        std::to_string(value.size()) + ":" + value;
        }
    } else {
        for (int col : {0, 1, 2, 3, 4, 5, 7}) {
            const auto value = Cell(batch, col, row);
            identity += std::to_string(value.size()) + ":" + value;
        }
    }
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(identity.data()), identity.size(), digest);
    constexpr char hex[] = "0123456789abcdef";
    std::string key;
    for (auto c : digest) {
        key += hex[c >> 4];
        key += hex[c & 15];
    }
    return key;
}
DatabaseAtomicStatusV1 DatabaseIdentity(IDatabaseAtomicSessionV1& session, const std::string& backend,
                                        std::string* identity, std::string* schema) {
    std::shared_ptr<arrow::RecordBatch> batch;
    const auto sql = backend == "sqlite"  ? "PRAGMA database_list"
                     : backend == "mysql" ? "SELECT @@hostname,@@port,DATABASE()"
                                          : "SELECT "
                                            "COALESCE(inet_server_addr()::text,'local'),COALESCE(inet_server_port(),0),"
                                            "current_database(),current_schema()";
    auto status = ReadAtomicBatch(session, sql, {}, &batch);
    if (!status.ok()) return status;
    if (!batch || !batch->num_rows()) return {DatabaseAtomicCodeV1::kError, "database identity unavailable"};
    if (backend == "sqlite") {
        for (int64_t row = 0; row < batch->num_rows(); ++row)
            if (Cell(*batch, 1, row) == "main") {
                const auto path = Cell(*batch, 2, row);
                if (path.empty()) return {DatabaseAtomicCodeV1::kUnsupported, "anonymous SQLite identity unavailable"};
                *identity = std::filesystem::weakly_canonical(path).string();
                *schema = "main";
                return {};
            }
        return {DatabaseAtomicCodeV1::kError, "SQLite main database missing"};
    }
    identity->clear();
    // MySQL schema names select databases independently of a connection's default database.
    // Compare its server first; ValidateRelations compares each actual source schema below.
    for (int col = 0; col < (backend == "mysql" ? 2 : 3); ++col) {
        const auto value = Cell(*batch, col, 0);
        *identity += std::to_string(value.size()) + ":" + value;
    }
    *schema = Cell(*batch, backend == "mysql" ? 2 : 3, 0);
    return {};
}
DatabaseParameterV1 Parameter(const arrow::Scalar& scalar) {
    if (!scalar.is_valid) return {};
    DatabaseParameterV1 p;
    if (scalar.type->id() == arrow::Type::STRING || scalar.type->id() == arrow::Type::BINARY) {
        const auto& value = static_cast<const arrow::BaseBinaryScalar&>(scalar);
        p.kind = scalar.type->id() == arrow::Type::STRING ? DatabaseParameterKindV1::kString
                                                          : DatabaseParameterKindV1::kBlob;
        p.data = value.value->data();
        p.size = value.value->size();
    } else if (scalar.type->id() == arrow::Type::INT64)
        p = IntParameter(static_cast<const arrow::Int64Scalar&>(scalar).value);
    else if (scalar.type->id() == arrow::Type::INT32)
        p = IntParameter(static_cast<const arrow::Int32Scalar&>(scalar).value);
    else if (scalar.type->id() == arrow::Type::UINT32)
        p = IntParameter(static_cast<const arrow::UInt32Scalar&>(scalar).value);
    else if (scalar.type->id() == arrow::Type::UINT64) {
        p.kind = DatabaseParameterKindV1::kUInt64;
        p.uint64_value = static_cast<const arrow::UInt64Scalar&>(scalar).value;
    } else if (scalar.type->id() == arrow::Type::DOUBLE) {
        p.kind = DatabaseParameterKindV1::kDouble;
        p.double_value = static_cast<const arrow::DoubleScalar&>(scalar).value;
    } else if (scalar.type->id() == arrow::Type::BOOL)
        p = IntParameter(static_cast<const arrow::BooleanScalar&>(scalar).value);
    else
        throw std::runtime_error("unsupported output scalar");
    return p;
}
}  // namespace
int ModelOutputStore::Fail(std::string error) {
    error_ = std::move(error);
    if (state_ != "committed" && state_ != "unknown") state_ = cancelled_ ? "cancelled" : "failed";
    return -1;
}
std::string ModelOutputStore::Quote(const std::string& id) const {
    return backend_ == "mysql" ? "`" + id + "`" : "\"" + id + "\"";
}
std::string ModelOutputStore::Type(const arrow::DataType& type) const {
    if (type.id() == arrow::Type::STRING) return backend_ == "mysql" ? "LONGTEXT" : "TEXT";
    if (type.id() == arrow::Type::BINARY)
        return backend_ == "mysql" ? "LONGBLOB" : backend_ == "postgres" ? "BYTEA" : "BLOB";
    if (type.id() == arrow::Type::DOUBLE)
        return backend_ == "sqlite" ? "REAL" : backend_ == "postgres" ? "DOUBLE PRECISION" : "DOUBLE";
    return "BIGINT";
}
int ModelOutputStore::Open(const ConfigSnapshot& config, std::shared_ptr<IDatabaseChannel> channel, std::string table,
                           SchemaKind kind) {
    if (session_ || cancelled_ || !channel || !channel->IsOpened() || !channel->IsConnected())
        return Fail("model target unavailable");
    if (kind != SchemaKind::kModelParameters && kind != SchemaKind::kResults) return Fail("invalid output kind");
    kind_ = kind;
    backend_ = channel->Category();
    if (backend_ != "sqlite" && backend_ != "mysql" && backend_ != "postgres")
        return Fail("unsupported model target backend");
    // The WITH contract restricts identifiers; keep direct internal callers equally strict.
    if (table.empty() || table.size() > 64 || !std::all_of(table.begin(), table.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '_' || c == '-';
        }))
        return Fail("invalid model target table");
    auto* target = dynamic_cast<IDatabaseAtomicTargetV1*>(channel.get());
    if (!target) return Fail("model target lacks atomic session capability");
    table_ = std::move(table);
    channel_ = std::move(channel);
    max_bytes_ = config.config.read.max_pending_bytes;
    DatabaseAtomicSessionOptionsV1 options;
    options.operation_timeout_ms = config.config.persistence.operation_timeout_ms;
    std::shared_ptr<IDatabaseAtomicSessionV1> session;
    auto status = target->AcquireAtomicSession(options, &session);
    if (!status.ok()) return Fail(status.message);
    std::atomic_store(&session_, session);
    if (cancelled_) {
        session->Cancel();
        return Fail("model output cancelled");
    }
    status = session->Begin();
    if (!status.ok()) return Fail(status.message);
    status = DatabaseIdentity(*session, backend_, &database_identity_, &default_schema_);
    if (!status.ok()) {
        session->Rollback();
        return Fail(status.message);
    }
    uint64_t affected = 0;
    std::string sql = "CREATE TABLE IF NOT EXISTS " + Quote(table_) + "(logical_key VARCHAR(64) NOT NULL PRIMARY KEY";
    const auto model_schema = MakeSchema(kind_);
    for (const auto& field : model_schema->fields())
        sql += "," + Quote(field->name()) + " " + Type(*field->type()) + (field->nullable() ? "" : " NOT NULL");
    sql += backend_ == "mysql" ? ") ENGINE=InnoDB" : ")";
    status = session->ExecutePrepared(sql.c_str(), nullptr, 0, &affected);
    if (!status.ok()) {
        session->Rollback();
        return Fail(status.message);
    }
    if (ValidateSchema() != 0) {
        session->Rollback();
        return -1;
    }
    const auto commit = session->Commit();
    if (!ValidDatabaseCommitResultV1(commit) || commit.outcome != DatabaseCommitOutcomeV1::kCommitted) {
        if (commit.outcome == DatabaseCommitOutcomeV1::kUnknown) state_ = "unknown";
        return Fail("model target schema commit: " + commit.status.message);
    }
    return 0;
}
int ModelOutputStore::ValidateSchema() {
    auto session = std::atomic_load(&session_);
    std::shared_ptr<arrow::RecordBatch> batch;
    DatabaseAtomicStatusV1 status;
    if (backend_ == "sqlite")
        status = ReadAtomicBatch(*session, "PRAGMA table_info(" + Quote(table_) + ")", {}, &batch);
    else
        status = ReadAtomicBatch(*session,
                                 "SELECT column_name,data_type,is_nullable" +
                                     std::string(backend_ == "mysql" ? ",column_key" : "") +
                                     " FROM information_schema.columns WHERE table_schema=" +
                                     std::string(backend_ == "mysql" ? "DATABASE()" : "current_schema()") +
                                     " AND table_name=? ORDER BY ordinal_position",
                                 {TextParameter(table_)}, &batch);
    if (!status.ok()) return Fail(status.message);
    const auto schema = MakeSchema(kind_);
    if (!batch || batch->num_rows() != schema->num_fields() + 1)
        return Fail("model target Schema column count mismatch");
    for (int64_t row = 0; row < batch->num_rows(); ++row) {
        const auto name = Cell(*batch, backend_ == "sqlite" ? 1 : 0, row);
        const auto type = Lower(Cell(*batch, backend_ == "sqlite" ? 2 : 1, row));
        const auto expected_name = row == 0 ? "logical_key" : schema->field(row - 1)->name();
        auto expected_type = row == 0 ? std::string("varchar(64)") : Lower(Type(*schema->field(row - 1)->type()));
        if (backend_ == "mysql" && row == 0) expected_type = "varchar";
        if (backend_ == "postgres" && row == 0) expected_type = "character varying";
        const bool required = backend_ == "sqlite" ? Cell(*batch, 3, row) == "1" : Cell(*batch, 2, row) == "NO";
        const bool expected_required = row == 0 || !schema->field(row - 1)->nullable();
        if (name != expected_name || type != expected_type || required != expected_required)
            return Fail("model target Schema mismatch: " + name);
        if (backend_ == "sqlite" && (Cell(*batch, 5, row) == "1") != (row == 0))
            return Fail("model target primary key mismatch");
        if (backend_ == "mysql" && (Cell(*batch, 3, row) == "PRI") != (row == 0))
            return Fail("model target primary key mismatch");
    }
    if (backend_ == "mysql") {
        status = ReadAtomicBatch(
            *session, "SELECT engine FROM information_schema.tables WHERE table_schema=DATABASE() AND table_name=?",
            {TextParameter(table_)}, &batch);
        if (!status.ok() || !batch || batch->num_rows() != 1 || Lower(Cell(*batch, 0, 0)) != "innodb")
            return Fail("output table requires transactional InnoDB engine");
    }
    if (backend_ == "postgres") {
        status = ReadAtomicBatch(*session,
                                 "SELECT k.column_name FROM information_schema.table_constraints t JOIN "
                                 "information_schema.key_column_usage k "
                                 "ON t.constraint_catalog=k.constraint_catalog AND "
                                 "t.constraint_schema=k.constraint_schema AND t.constraint_name=k.constraint_name "
                                 "WHERE t.table_schema=current_schema() AND t.table_name=? AND "
                                 "t.constraint_type='PRIMARY KEY' ORDER BY k.ordinal_position",
                                 {TextParameter(table_)}, &batch);
        if (!status.ok() || batch->num_rows() != 1 || Cell(*batch, 0, 0) != "logical_key")
            return Fail("model target primary key mismatch");
    }
    return 0;
}
int ModelOutputStore::ValidateRelations(IDatabaseChannel* source, const std::vector<Dataset>& datasets) {
    if (!source || backend_ != source->Category()) return 0;
    auto* target = dynamic_cast<IDatabaseAtomicTargetV1*>(source);
    if (!target) return Fail("source database identity capability unavailable");
    std::shared_ptr<IDatabaseAtomicSessionV1> session;
    auto status = target->AcquireAtomicSession({}, &session);
    if (!status.ok()) return Fail(status.message);
    status = session->Begin();
    if (!status.ok()) return Fail(status.message);
    std::string identity, schema;
    status = DatabaseIdentity(*session, backend_, &identity, &schema);
    session->Rollback();
    session->Close();
    if (!status.ok()) return Fail(status.message);
    if (identity != database_identity_) return 0;
    for (const auto& dataset : datasets) {
        const auto source_schema = dataset.schema.empty() ? schema : dataset.schema;
        const bool same_table = backend_ == "sqlite" ? Lower(dataset.table) == Lower(table_) : dataset.table == table_;
        if (source_schema == default_schema_ && same_table)
            return Fail("model output overlaps source/managed relation");
    }
    return 0;
}
int ModelOutputStore::Join(ModelOutputStore& model) {
    if (!session_ || !model.session_) return Fail("output session unavailable");
    if (backend_ != model.backend_ || database_identity_ != model.database_identity_ ||
        default_schema_ != model.default_schema_)
        return 0;
    if ((backend_ == "sqlite" ? Lower(table_) == Lower(model.table_) : table_ == model.table_))
        return Fail("model and results overlap actual target");
    model.Close();
    model.channel_ = channel_;  // Each session holder retains the actual session's channel lease.
    std::atomic_store(&model.session_, std::atomic_load(&session_));
    model.owns_session_ = false;
    return 0;
}
bool ModelOutputStore::SharesSession(const ModelOutputStore& model) const {
    const auto session = std::atomic_load(&session_);
    return session && session == std::atomic_load(&model.session_);
}
int ModelOutputStore::Insert(const std::shared_ptr<arrow::RecordBatch>& batch) {
    auto session = std::atomic_load(&session_);
    if (!batch || !batch->schema()->Equals(*MakeSchema(kind_), true) || !batch->ValidateFull().ok())
        return Fail("invalid output Schema");
    std::string sql = "INSERT INTO " + Quote(table_) + "(logical_key";
    for (const auto& field : batch->schema()->fields()) sql += "," + Quote(field->name());
    sql += ") VALUES(?";
    for (int i = 0; i < batch->num_columns(); ++i) sql += ",?";
    sql += ")";
    try {
        for (int64_t row = 0; row < batch->num_rows(); ++row) {
            if (cancelled_) return Fail("output cancelled");
            const auto key = LogicalKey(*batch, row, kind_);
            uint64_t affected = 0;
            auto parameter = TextParameter(key);
            const auto erase = "DELETE FROM " + Quote(table_) + " WHERE logical_key=?";
            auto status = session->ExecutePrepared(erase.c_str(), &parameter, 1, &affected);
            if (!status.ok()) return Fail(status.message);
            std::vector<std::shared_ptr<arrow::Scalar>> scalars;
            std::vector<DatabaseParameterV1> values{parameter};
            for (const auto& column : batch->columns()) {
                auto scalar = column->GetScalar(row);
                if (!scalar.ok()) throw std::runtime_error(scalar.status().ToString());
                scalars.push_back(*scalar);
                values.push_back(Parameter(**scalar));
            }
            status = session->ExecutePrepared(sql.c_str(), values.data(), values.size(), &affected);
            if (!status.ok() || affected != 1)
                return Fail(status.ok() ? "output insert count mismatch" : status.message);
        }
    } catch (const std::exception& e) {
        return Fail(e.what());
    }
    return 0;
}
int ModelOutputStore::Write(const std::shared_ptr<arrow::RecordBatch>& batch) {
    return Write({batch}, nullptr, nullptr);
}
int ModelOutputStore::Write(const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches, ModelOutputStore* model,
                            const std::shared_ptr<arrow::RecordBatch>& parameters) {
    auto session = std::atomic_load(&session_);
    if (!session || cancelled_ || state_ != "pending" ||
        (model && (!SharesSession(*model) || model->state_ != "pending" || model->cancelled_)))
        return Fail("output transaction unavailable");
    uint64_t bytes = 0;
    for (const auto& batch : batches) {
        if (!batch || uint64_t(arrow::util::TotalBufferSize(*batch)) > max_bytes_ - bytes)
            return Fail("output buffer budget exceeded");
        bytes += arrow::util::TotalBufferSize(*batch);
    }
    if (model && (!parameters || uint64_t(arrow::util::TotalBufferSize(*parameters)) > max_bytes_ - bytes))
        return Fail("combined output buffer budget exceeded");
    const auto rollback = [&] {
        session->Rollback();
        if (model) model->Fail(error_);
        return -1;
    };
    auto status = session->Begin();
    if (!status.ok()) {
        Fail(status.message);
        return rollback();
    }
    if (ValidateSchema() != 0) return rollback();
    if (model && model->ValidateSchema() != 0) {
        Fail(model->LastError());
        return rollback();
    }
    int64_t rows = 0;
    for (const auto& batch : batches) {
        if (Insert(batch) != 0) return rollback();
        rows += batch->num_rows();
    }
    if (model && model->Insert(parameters) != 0) {
        Fail(model->LastError());
        return rollback();
    }
    if (cancelled_ || (model && model->cancelled_)) {
        Fail("output cancelled");
        return rollback();
    }
    const auto commit = session->Commit();
    if (!ValidDatabaseCommitResultV1(commit) || commit.outcome != DatabaseCommitOutcomeV1::kCommitted) {
        state_ = commit.outcome == DatabaseCommitOutcomeV1::kUnknown ? "unknown" : "failed";
        if (model) model->state_ = state_;
        return Fail("output commit: " + commit.status.message);
    }
    state_ = "committed";
    rows_written_ = rows;
    if (model) {
        model->state_ = "committed";
        model->rows_written_ = parameters->num_rows();
    }
    return 0;
}
void ModelOutputStore::Cancel() {
    cancelled_ = true;
    auto session = std::atomic_load(&session_);
    if (session) session->Cancel();
}
void ModelOutputStore::Close() {
    auto session = std::atomic_exchange(&session_, std::shared_ptr<IDatabaseAtomicSessionV1>{});
    if (session && owns_session_) session->Close();
    session.reset();
    channel_.reset();
}
}  // namespace flowsql::baseliner
