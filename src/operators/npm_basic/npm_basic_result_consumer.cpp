// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "npm_basic_result_consumer.h"

#include <arrow/api.h>
#include <arrow/io/memory.h>
#include <arrow/ipc/reader.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <limits>
#include <mutex>
#include <new>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

namespace flowsql::npm {
namespace {

constexpr const char* kRunTable = "npm_result_runs";
constexpr const char* kEntityTable = "npm_result_entities";
constexpr int kPurgeBatchRows = 64;

std::mutex& ManagedResultOpenMutex() {
    static std::mutex mutex;
    return mutex;
}

enum class DatabaseBackend { kSqlite, kMysql, kPostgres, kClickHouse };

bool ParseBackend(std::string_view category, DatabaseBackend* backend) {
    if (category == "sqlite")
        *backend = DatabaseBackend::kSqlite;
    else if (category == "mysql")
        *backend = DatabaseBackend::kMysql;
    else if (category == "postgres" || category == "postgresql")
        *backend = DatabaseBackend::kPostgres;
    else if (category == "clickhouse")
        *backend = DatabaseBackend::kClickHouse;
    else
        return false;
    return true;
}

int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string QuoteIdentifier(std::string_view identifier) {
    std::string quoted = "\"";
    for (const char character : identifier) {
        quoted += character;
        if (character == '"') quoted += '"';
    }
    quoted += '"';
    return quoted;
}

std::string QuoteIdentifier(DatabaseBackend backend, std::string_view identifier) {
    if (backend != DatabaseBackend::kMysql && backend != DatabaseBackend::kClickHouse) {
        return QuoteIdentifier(identifier);
    }
    std::string quoted = "`";
    for (const char character : identifier) {
        quoted += character;
        if (character == '`') quoted += '`';
    }
    quoted += '`';
    return quoted;
}

bool IsEntityId(std::string_view value) {
    if (value.empty() || value.size() > 32 || value.front() < 'a' || value.front() > 'z') return false;
    return std::all_of(value.begin() + 1, value.end(), [](char character) {
        return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '_';
    });
}

bool IsManagedDataTable(std::string_view value) {
    if (value.size() < 11 || value.size() > 80 || value.substr(0, 4) != "npm_") return false;
    const size_t version = value.rfind("_v");
    if (version == std::string_view::npos || value.substr(value.size() - 5) != "_data" ||
        !IsEntityId(value.substr(4, version - 4))) {
        return false;
    }
    const auto digits = value.substr(version + 2, value.size() - version - 7);
    return !digits.empty() && std::all_of(digits.begin(), digits.end(),
                                          [](char character) { return character >= '0' && character <= '9'; });
}

std::string NowExpression(DatabaseBackend backend) {
    switch (backend) {
        case DatabaseBackend::kSqlite:
            return "(CAST(strftime('%s','now') AS INTEGER)*1000000000+"
                   "CAST(substr(strftime('%f','now'),4,3) AS INTEGER)*1000000)";
        case DatabaseBackend::kMysql:
            return "CAST(UNIX_TIMESTAMP(NOW(6))*1000000000 AS SIGNED)";
        case DatabaseBackend::kPostgres:
            return "(EXTRACT(EPOCH FROM clock_timestamp())*1000000000)::bigint";
        case DatabaseBackend::kClickHouse:
            return "toUnixTimestamp64Nano(now64(9))";
    }
    return {};
}

bool IsCatalogColumn(std::string_view value) {
    return !value.empty() && value.size() <= 128 && value.find('\0') == std::string_view::npos;
}

std::string QuoteLiteral(DatabaseBackend backend, std::string_view value) {
    std::string quoted = "'";
    for (const char character : value) {
        if (character == '\'' ||
            ((backend == DatabaseBackend::kMysql || backend == DatabaseBackend::kClickHouse) && character == '\\')) {
            quoted += character;
        }
        quoted += character;
    }
    quoted += '\'';
    return quoted;
}

std::string MetadataCanonical(const std::shared_ptr<const arrow::KeyValueMetadata>& metadata) {
    if (!metadata) return "0:";
    std::vector<std::pair<std::string, std::string>> entries;
    entries.reserve(metadata->size());
    for (int index = 0; index < metadata->size(); ++index) {
        entries.emplace_back(metadata->key(index), metadata->value(index));
    }
    std::sort(entries.begin(), entries.end());
    std::ostringstream canonical;
    canonical << entries.size() << ':';
    for (const auto& [key, value] : entries) {
        canonical << key.size() << ':' << key << value.size() << ':' << value;
    }
    return canonical.str();
}

std::string SchemaCanonical(const arrow::Schema& schema) {
    std::ostringstream canonical;
    canonical << "fields=" << schema.num_fields() << ';';
    for (const auto& field : schema.fields()) {
        canonical << field->name().size() << ':' << field->name() << ';' << field->type()->ToString().size() << ':'
                  << field->type()->ToString() << ';' << (field->nullable() ? '1' : '0') << ';'
                  << MetadataCanonical(field->metadata()) << ';';
    }
    canonical << "schema_metadata=" << MetadataCanonical(schema.metadata());
    return canonical.str();
}

std::string Fingerprint(std::string_view canonical) {
    uint64_t value = 1469598103934665603ULL;
    for (const unsigned char byte : canonical) {
        value ^= byte;
        value *= 1099511628211ULL;
    }
    std::ostringstream result;
    result << std::hex << std::setfill('0') << std::setw(16) << value;
    return result.str();
}

const char* SqliteType(const arrow::DataType& type) {
    switch (type.id()) {
        case arrow::Type::BOOL:
        case arrow::Type::INT8:
        case arrow::Type::INT16:
        case arrow::Type::INT32:
        case arrow::Type::INT64:
        case arrow::Type::UINT8:
        case arrow::Type::UINT16:
        case arrow::Type::UINT32:
            return "INTEGER";
        case arrow::Type::UINT64:
            return "TEXT";
        case arrow::Type::FLOAT:
        case arrow::Type::DOUBLE:
            return "REAL";
        case arrow::Type::STRING:
            return "TEXT";
        case arrow::Type::BINARY:
            return "BLOB";
        default:
            return nullptr;
    }
}

std::string DatabaseType(DatabaseBackend backend, const arrow::DataType& type, bool nullable) {
    if (backend == DatabaseBackend::kSqlite) {
        const char* mapped = SqliteType(type);
        return mapped ? mapped : "";
    }
    std::string mapped;
    switch (type.id()) {
        case arrow::Type::BOOL:
            mapped = backend == DatabaseBackend::kMysql      ? "TINYINT(1)"
                     : backend == DatabaseBackend::kPostgres ? "BOOLEAN"
                                                             : "Bool";
            break;
        case arrow::Type::INT8:
            mapped = backend == DatabaseBackend::kMysql      ? "TINYINT"
                     : backend == DatabaseBackend::kPostgres ? "SMALLINT"
                                                             : "Int8";
            break;
        case arrow::Type::INT16:
            mapped = backend == DatabaseBackend::kMysql      ? "SMALLINT"
                     : backend == DatabaseBackend::kPostgres ? "SMALLINT"
                                                             : "Int16";
            break;
        case arrow::Type::INT32:
            mapped = backend == DatabaseBackend::kMysql      ? "INT"
                     : backend == DatabaseBackend::kPostgres ? "INTEGER"
                                                             : "Int32";
            break;
        case arrow::Type::INT64:
            mapped = backend == DatabaseBackend::kClickHouse ? "Int64" : "BIGINT";
            break;
        case arrow::Type::UINT8:
            mapped = backend == DatabaseBackend::kMysql      ? "TINYINT UNSIGNED"
                     : backend == DatabaseBackend::kPostgres ? "SMALLINT"
                                                             : "UInt8";
            break;
        case arrow::Type::UINT16:
            mapped = backend == DatabaseBackend::kMysql      ? "SMALLINT UNSIGNED"
                     : backend == DatabaseBackend::kPostgres ? "INTEGER"
                                                             : "UInt16";
            break;
        case arrow::Type::UINT32:
            mapped = backend == DatabaseBackend::kMysql      ? "INT UNSIGNED"
                     : backend == DatabaseBackend::kPostgres ? "BIGINT"
                                                             : "UInt32";
            break;
        case arrow::Type::UINT64:
            mapped = backend == DatabaseBackend::kMysql      ? "BIGINT UNSIGNED"
                     : backend == DatabaseBackend::kPostgres ? "NUMERIC(20,0)"
                                                             : "UInt64";
            break;
        case arrow::Type::FLOAT:
            mapped = backend == DatabaseBackend::kMysql      ? "FLOAT"
                     : backend == DatabaseBackend::kPostgres ? "REAL"
                                                             : "Float32";
            break;
        case arrow::Type::DOUBLE:
            mapped = backend == DatabaseBackend::kMysql      ? "DOUBLE"
                     : backend == DatabaseBackend::kPostgres ? "DOUBLE PRECISION"
                                                             : "Float64";
            break;
        case arrow::Type::STRING:
            mapped = backend == DatabaseBackend::kClickHouse ? "String" : "TEXT";
            break;
        case arrow::Type::BINARY:
            mapped = backend == DatabaseBackend::kMysql      ? "BLOB"
                     : backend == DatabaseBackend::kPostgres ? "BYTEA"
                                                             : "String";
            break;
        default:
            return {};
    }
    if (backend == DatabaseBackend::kClickHouse && nullable) return "Nullable(" + mapped + ')';
    return mapped;
}

DatabaseParameterV1 StringParameter(std::string_view value) {
    DatabaseParameterV1 parameter;
    parameter.kind = DatabaseParameterKindV1::kString;
    parameter.data = value.data();
    parameter.size = value.size();
    return parameter;
}

DatabaseParameterV1 IntParameter(int64_t value) {
    DatabaseParameterV1 parameter;
    parameter.kind = DatabaseParameterKindV1::kInt64;
    parameter.int64_value = value;
    return parameter;
}

DatabaseParameterV1 UIntParameter(uint64_t value) {
    DatabaseParameterV1 parameter;
    parameter.kind = DatabaseParameterKindV1::kUInt64;
    parameter.uint64_value = value;
    return parameter;
}

DatabaseParameterV1 DoubleParameter(double value) {
    DatabaseParameterV1 parameter;
    parameter.kind = DatabaseParameterKindV1::kDouble;
    parameter.double_value = value;
    return parameter;
}

DatabaseParameterV1 NullParameter() { return {}; }

DatabaseParameterV1 BlobParameter(const void* data, size_t size) {
    DatabaseParameterV1 parameter;
    parameter.kind = DatabaseParameterKindV1::kBlob;
    parameter.data = data;
    parameter.size = size;
    return parameter;
}

bool AppendArrayValue(const arrow::Array& array, int64_t row, DatabaseParameterV1* parameter) {
    if (array.IsNull(row)) {
        *parameter = NullParameter();
        return true;
    }
    switch (array.type_id()) {
        case arrow::Type::BOOL:
            *parameter = IntParameter(static_cast<const arrow::BooleanArray&>(array).Value(row) ? 1 : 0);
            return true;
        case arrow::Type::INT8:
            *parameter = IntParameter(static_cast<const arrow::Int8Array&>(array).Value(row));
            return true;
        case arrow::Type::INT16:
            *parameter = IntParameter(static_cast<const arrow::Int16Array&>(array).Value(row));
            return true;
        case arrow::Type::INT32:
            *parameter = IntParameter(static_cast<const arrow::Int32Array&>(array).Value(row));
            return true;
        case arrow::Type::INT64:
            *parameter = IntParameter(static_cast<const arrow::Int64Array&>(array).Value(row));
            return true;
        case arrow::Type::UINT8:
            *parameter = IntParameter(static_cast<const arrow::UInt8Array&>(array).Value(row));
            return true;
        case arrow::Type::UINT16:
            *parameter = IntParameter(static_cast<const arrow::UInt16Array&>(array).Value(row));
            return true;
        case arrow::Type::UINT32:
            *parameter = IntParameter(static_cast<const arrow::UInt32Array&>(array).Value(row));
            return true;
        case arrow::Type::UINT64:
            *parameter = UIntParameter(static_cast<const arrow::UInt64Array&>(array).Value(row));
            return true;
        case arrow::Type::FLOAT:
            *parameter = DoubleParameter(static_cast<const arrow::FloatArray&>(array).Value(row));
            return true;
        case arrow::Type::DOUBLE:
            *parameter = DoubleParameter(static_cast<const arrow::DoubleArray&>(array).Value(row));
            return true;
        case arrow::Type::STRING: {
            const auto value = static_cast<const arrow::StringArray&>(array).GetView(row);
            *parameter = StringParameter(value);
            return true;
        }
        case arrow::Type::BINARY: {
            const auto value = static_cast<const arrow::BinaryArray&>(array).GetView(row);
            *parameter = BlobParameter(value.data(), value.size());
            return true;
        }
        default:
            return false;
    }
}

class PendingOutputLease final {
 public:
    PendingOutputLease(std::shared_ptr<INpmTaskBudget> budget, uint64_t bytes)
        : budget_(std::move(budget)), bytes_(bytes) {}
    ~PendingOutputLease() {
        if (reserved_) budget_->Release(NpmBudgetCategory::kPendingOutput, bytes_);
    }
    bool Reserve() {
        reserved_ = budget_ && budget_->Reserve(NpmBudgetCategory::kPendingOutput, bytes_) == NpmBudgetError::kNone;
        return reserved_;
    }

 private:
    std::shared_ptr<INpmTaskBudget> budget_;
    uint64_t bytes_ = 0;
    bool reserved_ = false;
};

struct EntityStorage {
    NpmEntityDescriptorV1 descriptor;
    std::string canonical;
    std::string fingerprint;
    std::string data_table;
    std::string history_relation;
    std::string latest_relation;
    std::string final_relation;
    std::string insert_sql;
    int64_t rows_written = 0;
};

struct SqliteTableColumn {
    std::string name;
    std::string type;
    bool not_null = false;
    int primary_key_position = 0;
};

struct SqliteObject {
    bool found = false;
    std::string type;
    std::string sql;
};

std::string NormalizeSql(std::string_view sql) {
    std::string normalized;
    normalized.reserve(sql.size());
    bool pending_space = false;
    for (const unsigned char character : sql) {
        if (std::isspace(character)) {
            pending_space = !normalized.empty();
            continue;
        }
        if (pending_space) normalized += ' ';
        normalized += static_cast<char>(character);
        pending_space = false;
    }
    return normalized;
}

int ReadSqliteObject(IDatabaseChannel* channel, std::string_view name, SqliteObject* object, std::string* error) {
    IBatchReader* raw_reader = nullptr;
    const std::string query = "SELECT type,sql FROM sqlite_master WHERE name='" + std::string(name) + "'";
    if (channel->CreateReader(query.c_str(), &raw_reader) != 0 || !raw_reader) {
        *error = channel->GetLastError();
        return EIO;
    }
    const auto release = [](IBatchReader* reader) {
        reader->Close();
        reader->Release();
    };
    std::unique_ptr<IBatchReader, decltype(release)> reader(raw_reader, release);
    const uint8_t* data = nullptr;
    size_t size = 0;
    while (true) {
        const int next = reader->Next(&data, &size);
        if (next == 1) break;
        if (next != 0 || !data || size == 0) {
            *error = reader->GetLastError();
            return EIO;
        }
        auto input = std::make_shared<arrow::io::BufferReader>(arrow::Buffer::Wrap(data, size));
        auto stream_result = arrow::ipc::RecordBatchStreamReader::Open(input);
        if (!stream_result.ok()) {
            *error = stream_result.status().ToString();
            return EIO;
        }
        auto stream = *stream_result;
        std::shared_ptr<arrow::RecordBatch> batch;
        while (true) {
            const auto read_status = stream->ReadNext(&batch);
            if (!read_status.ok()) {
                *error = read_status.ToString();
                return EIO;
            }
            if (!batch) break;
            const auto types = std::dynamic_pointer_cast<arrow::StringArray>(batch->GetColumnByName("type"));
            const auto definitions = std::dynamic_pointer_cast<arrow::StringArray>(batch->GetColumnByName("sql"));
            if (!types || !definitions || batch->num_rows() != 1 || types->IsNull(0) || definitions->IsNull(0) ||
                object->found) {
                *error = "unexpected SQLite object metadata";
                return EIO;
            }
            object->found = true;
            object->type = types->GetString(0);
            object->sql = definitions->GetString(0);
        }
    }
    return 0;
}

std::string BuildRelationSql(DatabaseBackend backend, const EntityStorage& entity, std::string_view relation,
                             bool latest, bool final) {
    const auto quote = [backend](std::string_view value) { return QuoteIdentifier(backend, value); };
    std::string sql = "CREATE VIEW " + quote(relation) + " AS SELECT d." + quote("__npm_run_id") + " AS " +
                      quote("__npm_run_id") + ",d." + quote("__npm_task_id") + " AS " + quote("__npm_task_id");
    for (const auto& field : entity.descriptor.schema->fields()) {
        sql += ",d." + quote(field->name()) + " AS " + quote(field->name());
    }
    sql += ",r." + quote("status") + " AS " + quote("__npm_run_status") + " FROM " + quote(entity.data_table) +
           " AS d JOIN " + quote(kRunTable) + " AS r ON r." + quote("run_id") + "=d." + quote("__npm_run_id");
    if (latest && backend != DatabaseBackend::kClickHouse) {
        const auto identity = quote(entity.descriptor.identity_column);
        const auto revision = quote(entity.descriptor.revision_column);
        sql += " LEFT JOIN " + quote(entity.data_table) + " AS newer ON newer." + quote("__npm_run_id") + "=d." +
               quote("__npm_run_id") + " AND newer." + identity + "=d." + identity + " AND ";
        if (backend == DatabaseBackend::kSqlite) {
            sql += "(length(newer." + revision + ")>length(d." + revision + ") OR (length(newer." + revision +
                   ")=length(d." + revision + ") AND newer." + revision + ">d." + revision + "))";
        } else {
            sql += "newer." + revision + ">d." + revision;
        }
    }
    sql += " WHERE r." + quote("status") + "<>'purging' AND (r." + quote("expires_at_ns") + " IS NULL OR r." +
           quote("expires_at_ns") + ">";
    switch (backend) {
        case DatabaseBackend::kSqlite:
            sql +=
                "(CAST(strftime('%s','now') AS INTEGER)*1000000000+"
                "CAST(substr(strftime('%f','now'),4,3) AS INTEGER)*1000000))";
            break;
        case DatabaseBackend::kMysql:
            sql += "CAST(UNIX_TIMESTAMP(NOW(6))*1000000000 AS SIGNED))";
            break;
        case DatabaseBackend::kPostgres:
            sql += "(EXTRACT(EPOCH FROM clock_timestamp())*1000000000)::bigint)";
            break;
        case DatabaseBackend::kClickHouse:
            sql += "toUnixTimestamp64Nano(now64(9)))";
            break;
    }
    if (latest) {
        const auto identity = quote(entity.descriptor.identity_column);
        const auto revision = quote(entity.descriptor.revision_column);
        if (backend == DatabaseBackend::kClickHouse) {
            sql += " AND (d." + quote("__npm_run_id") + ",d." + identity + ",d." + revision + ") IN (SELECT " +
                   quote("__npm_run_id") + ',' + identity + ",max(" + revision + ") FROM " + quote(entity.data_table) +
                   " GROUP BY " + quote("__npm_run_id") + ',' + identity + ')';
        } else {
            sql += " AND newer." + quote("__npm_run_id") + " IS NULL";
        }
    }
    if (final) {
        sql += " AND d." + quote(entity.descriptor.is_final_column) +
               (backend == DatabaseBackend::kSqlite ? "<>0" : "=TRUE");
    }
    return sql;
}

int ReadSqliteTableColumns(IDatabaseChannel* channel, std::string_view table, std::vector<SqliteTableColumn>* columns,
                           std::string* error) {
    IBatchReader* raw_reader = nullptr;
    const std::string query = "PRAGMA table_info(" + QuoteIdentifier(table) + ')';
    if (channel->CreateReader(query.c_str(), &raw_reader) != 0 || !raw_reader) {
        *error = channel->GetLastError();
        return EIO;
    }
    const auto release = [](IBatchReader* reader) {
        reader->Close();
        reader->Release();
    };
    std::unique_ptr<IBatchReader, decltype(release)> reader(raw_reader, release);
    const uint8_t* data = nullptr;
    size_t size = 0;
    while (true) {
        const int next = reader->Next(&data, &size);
        if (next == 1) break;
        if (next != 0 || !data || size == 0) {
            *error = reader->GetLastError();
            return EIO;
        }
        auto input = std::make_shared<arrow::io::BufferReader>(arrow::Buffer::Wrap(data, size));
        auto stream_result = arrow::ipc::RecordBatchStreamReader::Open(input);
        if (!stream_result.ok()) {
            *error = stream_result.status().ToString();
            return EIO;
        }
        auto stream = *stream_result;
        std::shared_ptr<arrow::RecordBatch> batch;
        while (true) {
            const auto read_status = stream->ReadNext(&batch);
            if (!read_status.ok()) {
                *error = read_status.ToString();
                return EIO;
            }
            if (!batch) break;
            const auto names = std::dynamic_pointer_cast<arrow::StringArray>(batch->GetColumnByName("name"));
            const auto types = std::dynamic_pointer_cast<arrow::StringArray>(batch->GetColumnByName("type"));
            const auto not_nulls = std::dynamic_pointer_cast<arrow::StringArray>(batch->GetColumnByName("notnull"));
            const auto primary_keys = std::dynamic_pointer_cast<arrow::StringArray>(batch->GetColumnByName("pk"));
            if (!names || !types || !not_nulls || !primary_keys) {
                *error = "unexpected SQLite table_info result schema";
                return EIO;
            }
            for (int64_t row = 0; row < batch->num_rows(); ++row) {
                if (names->IsNull(row) || types->IsNull(row) || not_nulls->IsNull(row) || primary_keys->IsNull(row)) {
                    *error = "null SQLite table_info value";
                    return EIO;
                }
                SqliteTableColumn column;
                column.name = names->GetString(row);
                column.type = types->GetString(row);
                std::transform(column.type.begin(), column.type.end(), column.type.begin(),
                               [](unsigned char value) { return static_cast<char>(std::toupper(value)); });
                try {
                    column.not_null = std::stoi(not_nulls->GetString(row)) != 0;
                    column.primary_key_position = std::stoi(primary_keys->GetString(row));
                } catch (const std::exception&) {
                    *error = "invalid SQLite table_info integer";
                    return EIO;
                }
                columns->push_back(std::move(column));
            }
        }
    }
    return 0;
}

int ValidateSqliteTableColumns(IDatabaseChannel* channel, std::string_view table,
                               const std::vector<SqliteTableColumn>& expected, std::string* error) {
    std::vector<SqliteTableColumn> actual;
    const int read_error = ReadSqliteTableColumns(channel, table, &actual, error);
    if (read_error != 0) return read_error;
    if (actual.size() != expected.size()) {
        *error = "SQLite table column count mismatch: " + std::string(table);
        return EINVAL;
    }
    for (size_t index = 0; index < expected.size(); ++index) {
        if (actual[index].name != expected[index].name || actual[index].type != expected[index].type ||
            actual[index].not_null != expected[index].not_null ||
            actual[index].primary_key_position != expected[index].primary_key_position) {
            *error = "SQLite table contract mismatch: " + std::string(table) + '.' + expected[index].name;
            return EINVAL;
        }
    }
    return 0;
}

int ValidateSqliteDataTable(IDatabaseChannel* channel, const EntityStorage& entity, std::string* error) {
    std::vector<SqliteTableColumn> expected;
    expected.reserve(static_cast<size_t>(entity.descriptor.schema->num_fields()) + 2);
    expected.push_back({"__npm_run_id", "TEXT", true, 1});
    expected.push_back({"__npm_task_id", "TEXT", true, 0});
    for (const auto& field : entity.descriptor.schema->fields()) {
        const int primary_key_position = field->name() == entity.descriptor.identity_column
                                             ? 2
                                             : (field->name() == entity.descriptor.revision_column ? 3 : 0);
        expected.push_back({field->name(), SqliteType(*field->type()), !field->nullable(), primary_key_position});
    }
    return ValidateSqliteTableColumns(channel, entity.data_table, expected, error);
}

using StringRows = std::vector<std::vector<std::string>>;

int AppendStringRows(const arrow::RecordBatch& batch, size_t columns, StringRows* rows, std::string* error) {
    if (batch.num_columns() != static_cast<int>(columns)) {
        *error = "unexpected database metadata column count";
        return EIO;
    }
    for (int64_t row = 0; row < batch.num_rows(); ++row) {
        std::vector<std::string> values;
        values.reserve(columns);
        for (size_t column = 0; column < columns; ++column) {
            const auto& array = batch.column(static_cast<int>(column));
            if (array->IsNull(row)) {
                values.emplace_back();
            } else if (array->type_id() == arrow::Type::STRING) {
                values.push_back(static_cast<const arrow::StringArray&>(*array).GetString(row));
            } else if (array->type_id() == arrow::Type::BINARY) {
                const auto value = static_cast<const arrow::BinaryArray&>(*array).GetView(row);
                values.emplace_back(value.data(), value.size());
            } else {
                *error = "unexpected database metadata value type: " + array->type()->ToString();
                return EIO;
            }
        }
        rows->push_back(std::move(values));
    }
    return 0;
}

int ReadStringRows(IDatabaseChannel* channel, DatabaseBackend backend, const std::string& query, size_t columns,
                   StringRows* rows, std::string* error) {
    if (backend == DatabaseBackend::kClickHouse) {
        std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
        if (channel->ExecuteQueryArrow(query.c_str(), &batches) != 0) {
            *error = channel->GetLastError();
            return EIO;
        }
        for (const auto& batch : batches) {
            if (!batch || AppendStringRows(*batch, columns, rows, error) != 0) return EIO;
        }
        return 0;
    }

    IBatchReader* raw_reader = nullptr;
    if (channel->CreateReader(query.c_str(), &raw_reader) != 0 || !raw_reader) {
        *error = channel->GetLastError();
        return EIO;
    }
    const auto release = [](IBatchReader* reader) {
        reader->Close();
        reader->Release();
    };
    std::unique_ptr<IBatchReader, decltype(release)> reader(raw_reader, release);
    const uint8_t* data = nullptr;
    size_t size = 0;
    while (true) {
        const int next = reader->Next(&data, &size);
        if (next == 1) break;
        if (next != 0 || !data || size == 0) {
            *error = reader->GetLastError();
            return EIO;
        }
        auto input = std::make_shared<arrow::io::BufferReader>(arrow::Buffer::Wrap(data, size));
        auto opened = arrow::ipc::RecordBatchStreamReader::Open(input);
        if (!opened.ok()) {
            *error = opened.status().ToString();
            return EIO;
        }
        std::shared_ptr<arrow::RecordBatch> batch;
        while (true) {
            const auto status = (*opened)->ReadNext(&batch);
            if (!status.ok()) {
                *error = status.ToString();
                return EIO;
            }
            if (!batch) break;
            if (AppendStringRows(*batch, columns, rows, error) != 0) return EIO;
        }
    }
    return 0;
}

std::string MetadataColumnsQuery(DatabaseBackend backend, std::string_view table) {
    if (backend == DatabaseBackend::kMysql) {
        return "SELECT CAST(column_name AS CHAR),CAST(column_type AS CHAR),CAST(is_nullable AS CHAR),"
               "CAST(column_key AS CHAR) FROM information_schema.columns WHERE table_schema=DATABASE() AND "
               "table_name='" +
               std::string(table) + "' ORDER BY ordinal_position";
    }
    if (backend == DatabaseBackend::kPostgres) {
        return "SELECT a.attname::text,pg_catalog.format_type(a.atttypid,a.atttypmod)::text,"
               "CASE WHEN a.attnotnull THEN 'NO' ELSE 'YES' END::text,CASE WHEN EXISTS (SELECT 1 FROM "
               "pg_catalog.pg_index i WHERE i.indrelid=c.oid AND i.indisprimary AND a.attnum=ANY(i.indkey)) "
               "THEN 'PRI' ELSE '' END::text FROM pg_catalog.pg_attribute a "
               "JOIN pg_catalog.pg_class c ON c.oid=a.attrelid JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace "
               "WHERE n.nspname=current_schema() AND c.relname='" +
               std::string(table) + "' AND a.attnum>0 AND NOT a.attisdropped ORDER BY a.attnum";
    }
    return "SELECT name,type,if(is_in_sorting_key,'YES','NO'),'' FROM system.columns WHERE database=currentDatabase() "
           "AND table='" +
           std::string(table) + "' ORDER BY position";
}

std::string NormalizeType(std::string value) {
    value.erase(
        std::remove_if(value.begin(), value.end(), [](unsigned char character) { return std::isspace(character); }),
        value.end());
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) { return static_cast<char>(std::toupper(character)); });
    return value;
}

struct ExpectedColumn {
    std::string name;
    std::string type;
    bool nullable = false;
    bool key = false;
};

int ValidateDatabaseColumns(DatabaseBackend backend, std::string_view object, const StringRows& actual,
                            const std::vector<ExpectedColumn>& expected, std::string* error) {
    if (actual.size() != expected.size()) {
        *error = "database object column count mismatch: " + std::string(object);
        return EINVAL;
    }
    for (size_t index = 0; index < expected.size(); ++index) {
        if (actual[index].size() != 4 || actual[index][0] != expected[index].name) {
            *error = "database object column mismatch: " + std::string(object) + '.' + expected[index].name;
            return EINVAL;
        }
        if (NormalizeType(actual[index][1]) != NormalizeType(expected[index].type)) {
            *error = "database object type mismatch: " + std::string(object) + '.' + expected[index].name;
            return EINVAL;
        }
        if (backend != DatabaseBackend::kClickHouse && (actual[index][2] == "YES") != expected[index].nullable) {
            *error = "database object nullability mismatch: " + std::string(object) + '.' + expected[index].name;
            return EINVAL;
        }
        const bool actual_key = actual[index][backend == DatabaseBackend::kClickHouse ? 2 : 3] ==
                                (backend == DatabaseBackend::kClickHouse ? "YES" : "PRI");
        if (actual_key != expected[index].key) {
            *error = "database object key mismatch: " + std::string(object) + '.' + expected[index].name;
            return EINVAL;
        }
    }
    return 0;
}

int ValidateDatabaseDataTable(IDatabaseChannel* channel, DatabaseBackend backend, const EntityStorage& entity,
                              std::string* error) {
    StringRows actual;
    if (ReadStringRows(channel, backend, MetadataColumnsQuery(backend, entity.data_table), 4, &actual, error) != 0) {
        return EIO;
    }
    const std::string internal_type = backend == DatabaseBackend::kMysql      ? "VARCHAR(128)"
                                      : backend == DatabaseBackend::kPostgres ? "TEXT"
                                                                              : "String";
    std::vector<ExpectedColumn> expected = {{"__npm_run_id", internal_type, false, true},
                                            {"__npm_task_id", internal_type, false, false}};
    for (const auto& field : entity.descriptor.schema->fields()) {
        const bool key =
            field->name() == entity.descriptor.identity_column || field->name() == entity.descriptor.revision_column;
        expected.push_back(
            {field->name(), DatabaseType(backend, *field->type(), field->nullable()), field->nullable(), key});
    }
    return ValidateDatabaseColumns(backend, entity.data_table, actual, expected, error);
}

std::string ObjectTypeQuery(DatabaseBackend backend, std::string_view name) {
    if (backend == DatabaseBackend::kMysql) {
        return "SELECT CAST(table_type AS CHAR) FROM information_schema.tables WHERE table_schema=DATABASE() AND "
               "table_name='" +
               std::string(name) + "'";
    }
    if (backend == DatabaseBackend::kPostgres) {
        return "SELECT CASE c.relkind WHEN 'v' THEN 'VIEW' WHEN 'm' THEN 'VIEW' ELSE 'TABLE' END::text FROM "
               "pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace WHERE "
               "n.nspname=current_schema() AND c.relname='" +
               std::string(name) + "'";
    }
    return "SELECT if(engine='View','VIEW','TABLE') FROM system.tables WHERE database=currentDatabase() AND name='" +
           std::string(name) + "'";
}

class NpmDatabaseResultConsumer final : public INpmManagedResultConsumerV1 {
 public:
    NpmDatabaseResultConsumer(IDatabaseChannel* channel, IDatabasePreparedCommandV1* commands, DatabaseBackend backend,
                              NpmResultContextV1 context, std::vector<EntityStorage> entities,
                              std::shared_ptr<INpmTaskBudget> budget)
        : channel_(channel),
          commands_(commands),
          backend_(backend),
          context_(std::move(context)),
          entities_(std::move(entities)),
          budget_(std::move(budget)) {}

    ~NpmDatabaseResultConsumer() override {
        Cancel();
        NpmResultFailureV1 failure;
        failure.code = ECANCELED;
        failure.stage = "open";
        failure.message = "managed result consumer destroyed before terminal state";
        (void)FailRun(failure);
    }

    int Consume(const NpmResultContextV1& context, const NpmEntityDescriptorV1& entity,
                const arrow::RecordBatch& rows) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (terminal_) return EPIPE;
        if (cancelled_.load(std::memory_order_acquire)) return ECANCELED;
        if (context.run_id != context_.run_id || context.task_id != context_.task_id) return EINVAL;
        const auto found = std::find_if(entities_.begin(), entities_.end(), [&](const auto& candidate) {
            return candidate.descriptor.entity_id == entity.entity_id &&
                   candidate.descriptor.schema_version == entity.schema_version;
        });
        if (found == entities_.end() || !rows.schema()->Equals(*found->descriptor.schema, true)) return EINVAL;
        if (rows.num_rows() == 0) return 0;

        const size_t parameters_per_row = static_cast<size_t>(rows.num_columns()) + 2;
        if (static_cast<uint64_t>(rows.num_rows()) > std::numeric_limits<size_t>::max() / parameters_per_row) {
            return EOVERFLOW;
        }
        const size_t parameter_count = static_cast<size_t>(rows.num_rows()) * parameters_per_row;
        if (parameter_count > std::numeric_limits<uint64_t>::max() / sizeof(DatabaseParameterV1)) return EOVERFLOW;
        PendingOutputLease lease(budget_, static_cast<uint64_t>(parameter_count * sizeof(DatabaseParameterV1)));
        if (!lease.Reserve()) return ENOSPC;

        try {
            std::vector<DatabaseParameterV1> parameters;
            parameters.reserve(parameter_count);
            for (int64_t row = 0; row < rows.num_rows(); ++row) {
                parameters.push_back(StringParameter(context_.run_id));
                parameters.push_back(StringParameter(context_.task_id));
                for (const auto& column : rows.columns()) {
                    DatabaseParameterV1 parameter;
                    if (!AppendArrayValue(*column, row, &parameter)) return ENOTSUP;
                    parameters.push_back(parameter);
                }
            }
            const int rc = commands_->ExecutePreparedBatch(found->insert_sql.c_str(), parameters.data(),
                                                           parameters_per_row, rows.num_rows());
            if (rc < 0) {
                error_ = channel_->GetLastError();
                return EIO;
            }
            found->rows_written += rows.num_rows();
            return 0;
        } catch (const std::bad_alloc&) {
            return ENOMEM;
        }
    }

    int Finish() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (terminal_) return terminal_status_ == "completed" ? 0 : EPIPE;
        if (cancelled_.load(std::memory_order_acquire)) return ECANCELED;
        const std::string status = "completed";
        const int64_t completed_at = NowNs();
        const DatabaseParameterV1 parameters[] = {StringParameter(status), IntParameter(completed_at),
                                                  StringParameter(context_.run_id)};
        const int rc =
            UpdateRun("status=?, completed_at_ns=?, metadata_status='known'", "status='writing'", parameters, 3);
        if (rc < 0 || (backend_ != DatabaseBackend::kClickHouse && rc != 1)) {
            metadata_status_ = "unknown";
            error_ = channel_->GetLastError();
            return EIO;
        }
        terminal_ = true;
        terminal_status_ = status;
        metadata_status_ = "known";
        return 0;
    }

    void Cancel() noexcept override { cancelled_.store(true, std::memory_order_release); }

    int FailRun(const NpmResultFailureV1& failure) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (terminal_) return terminal_status_ == "completed" ? EALREADY : 0;
        const std::string status = "incomplete";
        const std::string stage = failure.stage ? failure.stage : "unknown";
        const std::string message = failure.message ? failure.message : "managed result run failed";
        const int64_t completed_at = NowNs();
        const DatabaseParameterV1 parameters[] = {
            StringParameter(status), IntParameter(completed_at), IntParameter(failure.code),
            StringParameter(stage),  StringParameter(message),   StringParameter(context_.run_id),
        };
        const int rc = UpdateRun(
            "status=?, completed_at_ns=?, error_code=?, error_stage=?, error_message=?, metadata_status='known'",
            "status='writing'", parameters, 6);
        terminal_ = true;
        terminal_status_ = status;
        if (rc < 0 || (backend_ != DatabaseBackend::kClickHouse && rc != 1)) {
            metadata_status_ = "unknown";
            error_ = channel_->GetLastError();
            return EIO;
        }
        metadata_status_ = "known";
        return 0;
    }

    std::string ResultJson() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        writer.StartObject();
        writer.Key("run_id");
        writer.String(context_.run_id.c_str());
        writer.Key("run_status");
        writer.String(terminal_status_.c_str());
        writer.Key("metadata_status");
        writer.String(metadata_status_.c_str());
        int64_t total = 0;
        for (const auto& entity : entities_) total += entity.rows_written;
        writer.Key("rows_written");
        writer.Int64(total);
        writer.Key("entities");
        writer.StartArray();
        for (const auto& entity : entities_) {
            writer.StartObject();
            writer.Key("entity_id");
            writer.String(entity.descriptor.entity_id.c_str());
            writer.Key("schema_version");
            writer.Uint(entity.descriptor.schema_version);
            writer.Key("history_relation");
            writer.String(entity.history_relation.c_str());
            writer.Key("latest_relation");
            writer.String(entity.latest_relation.c_str());
            writer.Key("final_relation");
            writer.String(entity.final_relation.c_str());
            writer.Key("rows_written");
            writer.Int64(entity.rows_written);
            writer.EndObject();
        }
        writer.EndArray();
        writer.EndObject();
        return buffer.GetString();
    }

 private:
    int UpdateRun(std::string_view assignments, std::string_view predicate, const DatabaseParameterV1* parameters,
                  size_t parameter_count) {
        const std::string prefix = backend_ == DatabaseBackend::kClickHouse ? "ALTER TABLE npm_result_runs UPDATE "
                                                                            : "UPDATE npm_result_runs SET ";
        const std::string suffix = backend_ == DatabaseBackend::kClickHouse ? " SETTINGS mutations_sync=2" : "";
        const std::string sql =
            prefix + std::string(assignments) + " WHERE run_id=? AND " + std::string(predicate) + suffix;
        return commands_->ExecutePrepared(sql.c_str(), parameters, parameter_count);
    }

    IDatabaseChannel* channel_ = nullptr;
    IDatabasePreparedCommandV1* commands_ = nullptr;
    DatabaseBackend backend_ = DatabaseBackend::kSqlite;
    NpmResultContextV1 context_;
    std::vector<EntityStorage> entities_;
    std::shared_ptr<INpmTaskBudget> budget_;
    mutable std::mutex mutex_;
    std::atomic<bool> cancelled_{false};
    bool terminal_ = false;
    std::string terminal_status_ = "writing";
    std::string metadata_status_ = "known";
    std::string error_;
};

class NpmDatabaseResultConsumerFactory final : public INpmResultConsumerFactoryV1 {
 public:
    NpmDatabaseResultConsumerFactory(IDatabaseChannel* channel, std::string input_namespace,
                                     std::optional<uint32_t> retention_days)
        : channel_(channel), input_namespace_(std::move(input_namespace)), retention_days_(retention_days) {
        if (channel_) supported_backend_ = ParseBackend(channel_->Category(), &backend_);
    }

    int Create(const NpmResultContextV1& context, const std::vector<NpmEntityDescriptorV1>& entities,
               std::shared_ptr<INpmTaskBudget> budget,
               std::unique_ptr<INpmManagedResultConsumerV1>* consumer) override {
        std::lock_guard<std::mutex> open_lock(ManagedResultOpenMutex());
        if (!consumer || *consumer || !channel_ || !budget || context.run_id.empty() || context.task_id.empty() ||
            input_namespace_.empty() || !supported_backend_ || !channel_->IsOpened() || !channel_->IsConnected()) {
            return Fail(EINVAL, "invalid database managed result factory input");
        }
        if (retention_days_ && (*retention_days_ < 1 || *retention_days_ > 3650)) {
            return Fail(EINVAL, "managed result retention_days must be in 1..3650");
        }
        auto* commands = dynamic_cast<IDatabasePreparedCommandV1*>(channel_);
        if (!commands) return Fail(ENOTSUP, "database channel has no prepared command capability");
        try {
            std::vector<EntityStorage> storage;
            storage.reserve(entities.size());
            for (const auto& entity : entities) {
                const auto contract = ValidateNpmEntityDescriptorV1(entity);
                if (contract.error != NpmProtocolContractErrorV1::kNone || !IsEntityId(entity.entity_id)) {
                    return Fail(EINVAL, "invalid managed result entity: " + entity.entity_id);
                }
                for (const auto& field : entity.schema->fields()) {
                    if (field->name().rfind("__npm_", 0) == 0) {
                        return Fail(EINVAL, "reserved managed result column: " + field->name());
                    }
                    if (DatabaseType(backend_, *field->type(), field->nullable()).empty()) {
                        return Fail(ENOTSUP, "unsupported database result type: " + field->type()->ToString());
                    }
                }
                EntityStorage next;
                next.descriptor = entity;
                next.canonical = SchemaCanonical(*entity.schema);
                next.fingerprint = Fingerprint(next.canonical);
                const std::string suffix = entity.entity_id + "_v" + std::to_string(entity.schema_version);
                next.data_table = "npm_" + suffix + "_data";
                next.history_relation =
                    "npm_" + entity.entity_id + "_history_v" + std::to_string(entity.schema_version);
                next.latest_relation = "npm_" + entity.entity_id + "_latest_v" + std::to_string(entity.schema_version);
                next.final_relation = "npm_" + entity.entity_id + "_final_v" + std::to_string(entity.schema_version);
                storage.push_back(std::move(next));
            }
            if (CreateCatalog(commands) != 0) return last_code_;
            for (auto& entity : storage) {
                if (PrepareEntity(commands, &entity) != 0) return last_code_;
            }
            (void)PurgeOneBatch(commands);
            const std::string writing = "writing";
            const std::string known = "known";
            const int64_t started_at = NowNs();
            const DatabaseParameterV1 run_parameters[] = {
                StringParameter(context.run_id),
                StringParameter(context.task_id),
                StringParameter(input_namespace_),
                StringParameter(writing),
                IntParameter(started_at),
                retention_days_ ? IntParameter(started_at + static_cast<int64_t>(*retention_days_) * 86400000000000LL)
                                : NullParameter(),
                StringParameter(known),
            };
            if (commands->ExecutePrepared(
                    "INSERT INTO npm_result_runs(run_id,task_id,input_namespace,status,started_at_ns,expires_at_ns,"
                    "metadata_status) VALUES(?,?,?,?,?,?,?)",
                    run_parameters, 7) < 0) {
                return Fail(EIO, std::string("failed to create managed result run: ") + channel_->GetLastError());
            }
            *consumer = std::make_unique<NpmDatabaseResultConsumer>(channel_, commands, backend_, context,
                                                                    std::move(storage), std::move(budget));
            return 0;
        } catch (const std::bad_alloc&) {
            return Fail(ENOMEM, "managed result consumer allocation failed");
        }
    }

    std::string LastError() const override { return last_error_; }

 private:
    int Fail(int code, std::string error) {
        last_code_ = code;
        last_error_ = std::move(error);
        return code;
    }

    int PurgeOneBatch(IDatabasePreparedCommandV1* commands) {
        StringRows runs;
        std::string error;
        const std::string eligible =
            "SELECT run_id,status,coalesce(purge_cursor,'') FROM npm_result_runs WHERE status='purging' OR "
            "(status IN ('completed','incomplete') AND expires_at_ns IS NOT NULL AND expires_at_ns<=" +
            NowExpression(backend_) + ") ORDER BY started_at_ns,run_id LIMIT 1";
        if (ReadStringRows(channel_, backend_, eligible, 3, &runs, &error) != 0) return EIO;
        if (runs.empty()) return 0;
        const std::string& run_id = runs.front()[0];
        const std::string& status = runs.front()[1];
        const std::string& cursor = runs.front()[2];
        if (!cursor.empty() && !IsManagedDataTable(cursor)) return EINVAL;
        const auto run_parameter = StringParameter(run_id);
        if (status != "purging") {
            const std::string sql =
                (backend_ == DatabaseBackend::kClickHouse ? "ALTER TABLE npm_result_runs UPDATE "
                                                          : "UPDATE npm_result_runs SET ") +
                std::string("status='purging' WHERE run_id=? AND status IN ('completed','incomplete') AND ") +
                "expires_at_ns IS NOT NULL AND expires_at_ns<=" + NowExpression(backend_) +
                (backend_ == DatabaseBackend::kClickHouse ? " SETTINGS mutations_sync=2" : "");
            if (commands->ExecutePrepared(sql.c_str(), &run_parameter, 1) < 0) return EIO;
        }

        StringRows tables;
        const std::string catalog =
            "SELECT data_table,identity_column,revision_column FROM npm_result_entities WHERE data_table>" +
            QuoteLiteral(backend_, cursor) + " ORDER BY data_table LIMIT 1";
        if (ReadStringRows(channel_, backend_, catalog, 3, &tables, &error) != 0) return EIO;
        if (tables.empty()) {
            const std::string sql = backend_ == DatabaseBackend::kClickHouse
                                        ? "ALTER TABLE npm_result_runs DELETE WHERE run_id=? AND status='purging' "
                                          "SETTINGS mutations_sync=2"
                                        : "DELETE FROM npm_result_runs WHERE run_id=? AND status='purging'";
            return commands->ExecutePrepared(sql.c_str(), &run_parameter, 1) < 0 ? EIO : 0;
        }

        const auto& table = tables.front()[0];
        const auto& identity = tables.front()[1];
        const auto& revision = tables.front()[2];
        if (!IsManagedDataTable(table) || !IsCatalogColumn(identity) || !IsCatalogColumn(revision)) return EINVAL;
        const std::string quoted_table = QuoteIdentifier(backend_, table);
        std::string sql;
        switch (backend_) {
            case DatabaseBackend::kSqlite:
                sql = "DELETE FROM " + quoted_table + " WHERE rowid IN (SELECT rowid FROM " + quoted_table +
                      " WHERE __npm_run_id=? LIMIT " + std::to_string(kPurgeBatchRows) + ')';
                break;
            case DatabaseBackend::kMysql:
                sql = "DELETE FROM " + quoted_table + " WHERE __npm_run_id=? LIMIT " + std::to_string(kPurgeBatchRows);
                break;
            case DatabaseBackend::kPostgres:
                sql = "DELETE FROM " + quoted_table + " WHERE ctid IN (SELECT ctid FROM " + quoted_table +
                      " WHERE __npm_run_id=? LIMIT " + std::to_string(kPurgeBatchRows) + ')';
                break;
            case DatabaseBackend::kClickHouse: {
                const std::string key = QuoteIdentifier(backend_, identity) + ',' + QuoteIdentifier(backend_, revision);
                sql = "ALTER TABLE " + quoted_table + " DELETE WHERE (__npm_run_id," + key +
                      ") IN (SELECT __npm_run_id," + key + " FROM " + quoted_table + " WHERE __npm_run_id=? LIMIT " +
                      std::to_string(kPurgeBatchRows) + ") SETTINGS mutations_sync=2";
                break;
            }
        }
        if (commands->ExecutePrepared(sql.c_str(), &run_parameter, 1) < 0) return EIO;

        StringRows remaining;
        const std::string probe = "SELECT __npm_run_id FROM " + quoted_table +
                                  " WHERE __npm_run_id=" + QuoteLiteral(backend_, run_id) + " LIMIT 1";
        if (ReadStringRows(channel_, backend_, probe, 1, &remaining, &error) != 0) return EIO;
        if (remaining.empty()) {
            const auto table_parameter = StringParameter(table);
            const DatabaseParameterV1 parameters[] = {table_parameter, run_parameter};
            sql = backend_ == DatabaseBackend::kClickHouse
                      ? "ALTER TABLE npm_result_runs UPDATE purge_cursor=? WHERE run_id=? AND status='purging' "
                        "SETTINGS mutations_sync=2"
                      : "UPDATE npm_result_runs SET purge_cursor=? WHERE run_id=? AND status='purging'";
            if (commands->ExecutePrepared(sql.c_str(), parameters, 2) < 0) return EIO;
        }
        return 0;
    }

    int CreateCatalog(IDatabasePreparedCommandV1*) {
        std::string run_ddl =
            "CREATE TABLE IF NOT EXISTS npm_result_runs("
            "run_id String,task_id String,input_namespace String,status String,"
            "started_at_ns Int64,completed_at_ns Nullable(Int64),expires_at_ns Nullable(Int64),"
            "error_code Nullable(Int64),error_stage Nullable(String),error_message Nullable(String),"
            "metadata_status String,purge_cursor Nullable(String)) ENGINE=MergeTree ORDER BY run_id";
        std::string entity_ddl =
            "CREATE TABLE IF NOT EXISTS npm_result_entities("
            "entity_id String,schema_version UInt32,module_id String,revision_semantics String,identity_column String,"
            "revision_column String,observed_at_column String,is_final_column String,schema_canonical String,"
            "schema_fingerprint String,data_table String,history_relation String,latest_relation String,"
            "final_relation String) ENGINE=ReplacingMergeTree ORDER BY (entity_id,schema_version)";
        if (backend_ != DatabaseBackend::kClickHouse) {
            run_ddl =
                "CREATE TABLE IF NOT EXISTS npm_result_runs("
                "run_id TEXT PRIMARY KEY NOT NULL,task_id TEXT NOT NULL,input_namespace TEXT NOT NULL,"
                "status TEXT NOT NULL CHECK(status IN ('opening','writing','completed','incomplete','purging')),"
                "started_at_ns BIGINT NOT NULL,completed_at_ns BIGINT,expires_at_ns BIGINT,error_code BIGINT,"
                "error_stage TEXT,error_message TEXT,metadata_status TEXT NOT NULL CHECK(metadata_status IN "
                "('known','unknown')),purge_cursor TEXT)";
            entity_ddl =
                "CREATE TABLE IF NOT EXISTS npm_result_entities("
                "entity_id TEXT NOT NULL,schema_version INTEGER NOT NULL,module_id TEXT NOT NULL,"
                "revision_semantics TEXT NOT NULL,identity_column TEXT NOT NULL,revision_column TEXT NOT NULL,"
                "observed_at_column TEXT NOT NULL,is_final_column TEXT NOT NULL,schema_canonical TEXT NOT NULL,"
                "schema_fingerprint TEXT NOT NULL,data_table TEXT NOT NULL UNIQUE,history_relation TEXT NOT NULL "
                "UNIQUE,"
                "latest_relation TEXT NOT NULL UNIQUE,final_relation TEXT NOT NULL UNIQUE,"
                "PRIMARY KEY(entity_id,schema_version))";
            if (backend_ == DatabaseBackend::kSqlite) {
                run_ddl =
                    "CREATE TABLE IF NOT EXISTS npm_result_runs("
                    "run_id TEXT PRIMARY KEY NOT NULL,task_id TEXT NOT NULL,input_namespace TEXT NOT NULL,"
                    "status TEXT NOT NULL CHECK(status IN ('opening','writing','completed','incomplete','purging')),"
                    "started_at_ns INTEGER NOT NULL,completed_at_ns INTEGER,expires_at_ns INTEGER,error_code INTEGER,"
                    "error_stage TEXT,error_message TEXT,metadata_status TEXT NOT NULL CHECK(metadata_status IN "
                    "('known','unknown')),purge_cursor TEXT)";
            } else if (backend_ == DatabaseBackend::kMysql) {
                run_ddl =
                    "CREATE TABLE IF NOT EXISTS npm_result_runs("
                    "run_id VARCHAR(128) PRIMARY KEY NOT NULL,task_id VARCHAR(128) NOT NULL,"
                    "input_namespace VARCHAR(512) NOT NULL,status VARCHAR(16) NOT NULL,started_at_ns BIGINT NOT NULL,"
                    "completed_at_ns BIGINT,expires_at_ns BIGINT,error_code BIGINT,error_stage VARCHAR(64),"
                    "error_message TEXT,metadata_status VARCHAR(16) NOT NULL,purge_cursor VARCHAR(128)) ENGINE=InnoDB";
                entity_ddl =
                    "CREATE TABLE IF NOT EXISTS npm_result_entities("
                    "entity_id VARCHAR(32) NOT NULL,schema_version INTEGER NOT NULL,module_id VARCHAR(128) NOT NULL,"
                    "revision_semantics VARCHAR(16) NOT NULL,identity_column VARCHAR(128) NOT NULL,"
                    "revision_column VARCHAR(128) NOT NULL,observed_at_column VARCHAR(128) NOT NULL,"
                    "is_final_column VARCHAR(128) NOT NULL,schema_canonical TEXT NOT NULL,"
                    "schema_fingerprint VARCHAR(64) NOT NULL,data_table VARCHAR(128) NOT NULL UNIQUE,"
                    "history_relation VARCHAR(128) NOT NULL UNIQUE,latest_relation VARCHAR(128) NOT NULL UNIQUE,"
                    "final_relation VARCHAR(128) NOT NULL UNIQUE,PRIMARY KEY(entity_id,schema_version)) ENGINE=InnoDB";
            }
        }
        if (channel_->ExecuteSql(run_ddl.c_str()) < 0 || channel_->ExecuteSql(entity_ddl.c_str()) < 0) {
            return Fail(EIO, std::string("failed to create managed result catalog: ") + channel_->GetLastError());
        }
        if (backend_ != DatabaseBackend::kSqlite) return ValidateNonSqliteCatalog();
        const std::vector<SqliteTableColumn> run_columns = {
            {"run_id", "TEXT", true, 1},
            {"task_id", "TEXT", true, 0},
            {"input_namespace", "TEXT", true, 0},
            {"status", "TEXT", true, 0},
            {"started_at_ns", "INTEGER", true, 0},
            {"completed_at_ns", "INTEGER", false, 0},
            {"expires_at_ns", "INTEGER", false, 0},
            {"error_code", "INTEGER", false, 0},
            {"error_stage", "TEXT", false, 0},
            {"error_message", "TEXT", false, 0},
            {"metadata_status", "TEXT", true, 0},
            {"purge_cursor", "TEXT", false, 0},
        };
        const std::vector<SqliteTableColumn> entity_columns = {
            {"entity_id", "TEXT", true, 1},          {"schema_version", "INTEGER", true, 2},
            {"module_id", "TEXT", true, 0},          {"revision_semantics", "TEXT", true, 0},
            {"identity_column", "TEXT", true, 0},    {"revision_column", "TEXT", true, 0},
            {"observed_at_column", "TEXT", true, 0}, {"is_final_column", "TEXT", true, 0},
            {"schema_canonical", "TEXT", true, 0},   {"schema_fingerprint", "TEXT", true, 0},
            {"data_table", "TEXT", true, 0},         {"history_relation", "TEXT", true, 0},
            {"latest_relation", "TEXT", true, 0},    {"final_relation", "TEXT", true, 0},
        };
        std::string validation_error;
        std::vector<SqliteTableColumn> actual_runs;
        if (ReadSqliteTableColumns(channel_, kRunTable, &actual_runs, &validation_error) != 0) {
            return Fail(EIO, "failed to inspect managed result catalog: " + validation_error);
        }
        if (actual_runs.size() + 1 == run_columns.size()) {
            auto legacy_columns = run_columns;
            legacy_columns.pop_back();
            if (ValidateSqliteTableColumns(channel_, kRunTable, legacy_columns, &validation_error) != 0 ||
                channel_->ExecuteSql("ALTER TABLE npm_result_runs ADD COLUMN purge_cursor TEXT") < 0) {
                return Fail(EINVAL, "managed result catalog contract conflict: " + validation_error);
            }
        }
        if (ValidateSqliteTableColumns(channel_, kRunTable, run_columns, &validation_error) != 0 ||
            ValidateSqliteTableColumns(channel_, kEntityTable, entity_columns, &validation_error) != 0) {
            return Fail(EINVAL, "managed result catalog contract conflict: " + validation_error);
        }
        return 0;
    }

    int ValidateNonSqliteCatalog() {
        StringRows runs;
        StringRows entities;
        std::string error;
        if (ReadStringRows(channel_, backend_, MetadataColumnsQuery(backend_, kRunTable), 4, &runs, &error) != 0 ||
            ReadStringRows(channel_, backend_, MetadataColumnsQuery(backend_, kEntityTable), 4, &entities, &error) !=
                0) {
            return Fail(EIO, "failed to inspect managed result catalog: " + error);
        }
        const std::string text = backend_ == DatabaseBackend::kMysql ? "VARCHAR(128)" : "String";
        const std::string long_text = backend_ == DatabaseBackend::kMysql ? "TEXT" : "String";
        const std::string signed_integer = backend_ == DatabaseBackend::kClickHouse ? "Int64" : "BIGINT";
        const std::string nullable_integer = backend_ == DatabaseBackend::kClickHouse ? "Nullable(Int64)" : "BIGINT";
        const std::string nullable_text = backend_ == DatabaseBackend::kClickHouse ? "Nullable(String)" : "TEXT";
        std::vector<ExpectedColumn> expected_runs = {
            {"run_id", text, false, true},
            {"task_id", text},
            {"input_namespace", backend_ == DatabaseBackend::kMysql ? "VARCHAR(512)" : "String"},
            {"status", backend_ == DatabaseBackend::kMysql ? "VARCHAR(16)" : "String"},
            {"started_at_ns", signed_integer},
            {"completed_at_ns", nullable_integer, true},
            {"expires_at_ns", nullable_integer, true},
            {"error_code", nullable_integer, true},
            {"error_stage", backend_ == DatabaseBackend::kMysql ? "VARCHAR(64)" : nullable_text, true},
            {"error_message", nullable_text, true},
            {"metadata_status", backend_ == DatabaseBackend::kMysql ? "VARCHAR(16)" : "String"},
            {"purge_cursor", backend_ == DatabaseBackend::kMysql ? "VARCHAR(128)" : nullable_text, true},
        };
        const std::string schema_version = backend_ == DatabaseBackend::kMysql      ? "INT"
                                           : backend_ == DatabaseBackend::kPostgres ? "INTEGER"
                                                                                    : "UInt32";
        std::vector<ExpectedColumn> expected_entities = {
            {"entity_id", backend_ == DatabaseBackend::kMysql ? "VARCHAR(32)" : "String", false, true},
            {"schema_version", schema_version, false, true},
            {"module_id", text},
            {"revision_semantics", backend_ == DatabaseBackend::kMysql ? "VARCHAR(16)" : "String"},
            {"identity_column", text},
            {"revision_column", text},
            {"observed_at_column", text},
            {"is_final_column", text},
            {"schema_canonical", long_text},
            {"schema_fingerprint", backend_ == DatabaseBackend::kMysql ? "VARCHAR(64)" : "String"},
            {"data_table", text},
            {"history_relation", text},
            {"latest_relation", text},
            {"final_relation", text},
        };
        if (backend_ == DatabaseBackend::kPostgres) {
            for (auto& column : expected_runs) {
                if (column.type == "String") column.type = "TEXT";
            }
            for (auto& column : expected_entities) {
                if (column.type == "String") column.type = "TEXT";
            }
        }
        if (runs.size() + 1 == expected_runs.size()) {
            auto legacy_columns = expected_runs;
            legacy_columns.pop_back();
            if (ValidateDatabaseColumns(backend_, kRunTable, runs, legacy_columns, &error) != 0) {
                return Fail(EINVAL, "managed result catalog contract conflict: " + error);
            }
            const std::string type = backend_ == DatabaseBackend::kClickHouse ? "Nullable(String)"
                                     : backend_ == DatabaseBackend::kMysql    ? "VARCHAR(128)"
                                                                              : "TEXT";
            if (channel_->ExecuteSql(("ALTER TABLE npm_result_runs ADD COLUMN purge_cursor " + type).c_str()) < 0) {
                return Fail(EIO, std::string("failed to extend managed result catalog: ") + channel_->GetLastError());
            }
            runs.clear();
            if (ReadStringRows(channel_, backend_, MetadataColumnsQuery(backend_, kRunTable), 4, &runs, &error) != 0) {
                return Fail(EIO, "failed to recheck managed result catalog: " + error);
            }
        }
        if (ValidateDatabaseColumns(backend_, kRunTable, runs, expected_runs, &error) != 0 ||
            ValidateDatabaseColumns(backend_, kEntityTable, entities, expected_entities, &error) != 0) {
            return Fail(EINVAL, "managed result catalog contract conflict: " + error);
        }
        return 0;
    }

    int PrepareEntity(IDatabasePreparedCommandV1* commands, EntityStorage* entity) {
        const std::string semantics =
            entity->descriptor.revision_semantics == NpmRevisionSemanticsV1::kEvent ? "event" : "cumulative";
        const DatabaseParameterV1 parameters[] = {
            StringParameter(entity->descriptor.entity_id),
            IntParameter(entity->descriptor.schema_version),
            StringParameter(entity->descriptor.module_id),
            StringParameter(semantics),
            StringParameter(entity->descriptor.identity_column),
            StringParameter(entity->descriptor.revision_column),
            StringParameter(entity->descriptor.observed_at_column),
            StringParameter(entity->descriptor.is_final_column),
            StringParameter(entity->canonical),
            StringParameter(entity->fingerprint),
            StringParameter(entity->data_table),
            StringParameter(entity->history_relation),
            StringParameter(entity->latest_relation),
            StringParameter(entity->final_relation),
        };
        const char* upsert =
            "INSERT INTO npm_result_entities(entity_id,schema_version,module_id,revision_semantics,identity_column,"
            "revision_column,observed_at_column,is_final_column,schema_canonical,schema_fingerprint,data_table,"
            "history_relation,latest_relation,final_relation) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
            "ON CONFLICT(entity_id,schema_version) DO UPDATE SET schema_fingerprint=CASE WHEN "
            "module_id=excluded.module_id AND revision_semantics=excluded.revision_semantics AND "
            "identity_column=excluded.identity_column AND revision_column=excluded.revision_column AND "
            "observed_at_column=excluded.observed_at_column AND is_final_column=excluded.is_final_column AND "
            "schema_canonical=excluded.schema_canonical AND schema_fingerprint=excluded.schema_fingerprint AND "
            "data_table=excluded.data_table AND history_relation=excluded.history_relation AND "
            "latest_relation=excluded.latest_relation AND final_relation=excluded.final_relation "
            "THEN npm_result_entities.schema_fingerprint ELSE NULL END";
        int catalog_status = 0;
        if (backend_ == DatabaseBackend::kSqlite) {
            catalog_status = commands->ExecutePrepared(upsert, parameters, 14);
        } else if (backend_ == DatabaseBackend::kPostgres) {
            const char* postgres_upsert =
                "INSERT INTO npm_result_entities AS current(entity_id,schema_version,module_id,revision_semantics,"
                "identity_column,revision_column,observed_at_column,is_final_column,schema_canonical,"
                "schema_fingerprint,data_table,history_relation,latest_relation,final_relation) "
                "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(entity_id,schema_version) DO UPDATE SET "
                "schema_fingerprint=CASE WHEN current.module_id=excluded.module_id AND "
                "current.revision_semantics=excluded.revision_semantics AND "
                "current.identity_column=excluded.identity_column AND current.revision_column=excluded.revision_column "
                "AND current.observed_at_column=excluded.observed_at_column AND "
                "current.is_final_column=excluded.is_final_column AND "
                "current.schema_canonical=excluded.schema_canonical "
                "AND current.schema_fingerprint=excluded.schema_fingerprint AND current.data_table=excluded.data_table "
                "AND current.history_relation=excluded.history_relation AND "
                "current.latest_relation=excluded.latest_relation AND current.final_relation=excluded.final_relation "
                "THEN current.schema_fingerprint ELSE NULL END";
            catalog_status = commands->ExecutePrepared(postgres_upsert, parameters, 14);
        } else {
            catalog_status = PrepareNonSqliteEntityCatalog(commands, entity, parameters);
        }
        if (catalog_status < 0) {
            return Fail(EINVAL, std::string("managed result entity contract conflict: ") + channel_->GetLastError());
        }

        const auto quote = [this](std::string_view value) { return QuoteIdentifier(backend_, value); };
        const std::string internal_text =
            backend_ == DatabaseBackend::kClickHouse
                ? " String"
                : (backend_ == DatabaseBackend::kMysql ? " VARCHAR(128) NOT NULL" : " TEXT NOT NULL");
        std::string ddl = "CREATE TABLE IF NOT EXISTS " + quote(entity->data_table) + "(" + quote("__npm_run_id") +
                          internal_text + ',' + quote("__npm_task_id") + internal_text;
        for (const auto& field : entity->descriptor.schema->fields()) {
            ddl += ',' + quote(field->name()) + ' ' + DatabaseType(backend_, *field->type(), field->nullable());
            if (backend_ != DatabaseBackend::kClickHouse && !field->nullable()) ddl += " NOT NULL";
        }
        if (backend_ == DatabaseBackend::kClickHouse) {
            ddl += ") ENGINE=MergeTree ORDER BY (" + quote("__npm_run_id") + ',' +
                   quote(entity->descriptor.identity_column) + ',' + quote(entity->descriptor.revision_column) + ')';
        } else {
            ddl += ",PRIMARY KEY(" + quote("__npm_run_id") + ',' + quote(entity->descriptor.identity_column) + ',' +
                   quote(entity->descriptor.revision_column) + "))";
            if (backend_ == DatabaseBackend::kMysql) ddl += " ENGINE=InnoDB";
        }
        if (channel_->ExecuteSql(ddl.c_str()) < 0) {
            return Fail(EIO, std::string("failed to create managed result data table: ") + channel_->GetLastError());
        }
        std::string validation_error;
        const int validation_status = backend_ == DatabaseBackend::kSqlite
                                          ? ValidateSqliteDataTable(channel_, *entity, &validation_error)
                                          : ValidateDatabaseDataTable(channel_, backend_, *entity, &validation_error);
        if (validation_status != 0) {
            return Fail(validation_status, "managed result data table contract conflict: " + validation_error);
        }

        if (PrepareRelation(entity->history_relation, *entity,
                            BuildRelationSql(backend_, *entity, entity->history_relation, false, false)) != 0 ||
            PrepareRelation(entity->latest_relation, *entity,
                            BuildRelationSql(backend_, *entity, entity->latest_relation, true, false)) != 0 ||
            PrepareRelation(entity->final_relation, *entity,
                            BuildRelationSql(backend_, *entity, entity->final_relation, false, true)) != 0) {
            return last_code_;
        }

        entity->insert_sql =
            "INSERT INTO " + quote(entity->data_table) + "(" + quote("__npm_run_id") + ',' + quote("__npm_task_id");
        for (const auto& field : entity->descriptor.schema->fields()) {
            entity->insert_sql += ',' + quote(field->name());
        }
        entity->insert_sql += ") VALUES(";
        for (int index = 0; index < entity->descriptor.schema->num_fields() + 2; ++index) {
            if (index) entity->insert_sql += ',';
            entity->insert_sql += '?';
        }
        entity->insert_sql += ')';
        return 0;
    }

    int PrepareNonSqliteEntityCatalog(IDatabasePreparedCommandV1* commands, const EntityStorage* entity,
                                      const DatabaseParameterV1* parameters) {
        StringRows rows;
        std::string error;
        const std::string query = backend_ == DatabaseBackend::kClickHouse
                                      ? "SELECT module_id,revision_semantics,identity_column,revision_column,"
                                        "observed_at_column,is_final_column,schema_canonical,schema_fingerprint,"
                                        "data_table,history_relation,latest_relation,final_relation FROM "
                                        "npm_result_entities FINAL WHERE entity_id='" +
                                            entity->descriptor.entity_id +
                                            "' AND schema_version=" + std::to_string(entity->descriptor.schema_version)
                                      : "SELECT CAST(module_id AS CHAR),CAST(revision_semantics AS CHAR),"
                                        "CAST(identity_column AS CHAR),CAST(revision_column AS CHAR),"
                                        "CAST(observed_at_column AS CHAR),CAST(is_final_column AS CHAR),"
                                        "CAST(schema_canonical AS CHAR),CAST(schema_fingerprint AS CHAR),"
                                        "CAST(data_table AS CHAR),CAST(history_relation AS CHAR),"
                                        "CAST(latest_relation AS CHAR),CAST(final_relation AS CHAR) FROM "
                                        "npm_result_entities WHERE entity_id='" +
                                            entity->descriptor.entity_id +
                                            "' AND schema_version=" + std::to_string(entity->descriptor.schema_version);
        if (ReadStringRows(channel_, backend_, query, 12, &rows, &error) != 0) return -1;
        const std::vector<std::string> expected = {
            entity->descriptor.module_id,
            entity->descriptor.revision_semantics == NpmRevisionSemanticsV1::kEvent ? "event" : "cumulative",
            entity->descriptor.identity_column,
            entity->descriptor.revision_column,
            entity->descriptor.observed_at_column,
            entity->descriptor.is_final_column,
            entity->canonical,
            entity->fingerprint,
            entity->data_table,
            entity->history_relation,
            entity->latest_relation,
            entity->final_relation,
        };
        if (!rows.empty()) {
            if (rows.size() != 1 || rows.front() != expected) return -1;
            return 0;
        }
        const std::string insert =
            std::string(backend_ == DatabaseBackend::kMysql ? "INSERT IGNORE INTO " : "INSERT INTO ") +
            "npm_result_entities(entity_id,schema_version,module_id,revision_semantics,identity_column,"
            "revision_column,observed_at_column,is_final_column,schema_canonical,schema_fingerprint,data_table,"
            "history_relation,latest_relation,final_relation) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
        if (commands->ExecutePrepared(insert.c_str(), parameters, 14) < 0) return -1;
        rows.clear();
        return ReadStringRows(channel_, backend_, query, 12, &rows, &error) == 0 && rows.size() == 1 &&
                       rows.front() == expected
                   ? 0
                   : -1;
    }

    int PrepareRelation(std::string_view name, const EntityStorage& entity, const std::string& expected_sql) {
        if (backend_ != DatabaseBackend::kSqlite) {
            StringRows rows;
            std::string error;
            if (ReadStringRows(channel_, backend_, ObjectTypeQuery(backend_, name), 1, &rows, &error) != 0) {
                return Fail(EIO, "failed to inspect managed result relation: " + error);
            }
            if (rows.empty()) {
                const int create_status = channel_->ExecuteSql(expected_sql.c_str());
                const std::string create_error = channel_->GetLastError();
                rows.clear();
                if (ReadStringRows(channel_, backend_, ObjectTypeQuery(backend_, name), 1, &rows, &error) != 0 ||
                    rows.empty()) {
                    return Fail(EIO, create_status < 0 ? "failed to create managed result relation: " + create_error
                                                       : "failed to verify managed result relation: " + error);
                }
            }
            if (rows.size() != 1 || rows.front().size() != 1 ||
                (rows.front()[0] != "VIEW" && rows.front()[0] != "SYSTEM VIEW")) {
                return Fail(EINVAL, "managed result relation contract conflict: " + std::string(name));
            }
            rows.clear();
            const std::string columns_query = backend_ == DatabaseBackend::kClickHouse
                                                  ? "SELECT name,type,if(is_in_sorting_key,'YES','NO'),'' FROM "
                                                    "system.columns WHERE database=currentDatabase() AND table='" +
                                                        std::string(name) + "' ORDER BY position"
                                                  : MetadataColumnsQuery(backend_, name);
            if (ReadStringRows(channel_, backend_, columns_query, 4, &rows, &error) != 0) {
                return Fail(EIO, "failed to inspect managed result relation columns: " + error);
            }
            std::vector<std::string> expected_names = {"__npm_run_id", "__npm_task_id"};
            for (const auto& field : entity.descriptor.schema->fields()) expected_names.push_back(field->name());
            expected_names.emplace_back("__npm_run_status");
            if (rows.size() != expected_names.size()) {
                return Fail(EINVAL, "managed result relation column count conflict: " + std::string(name));
            }
            for (size_t index = 0; index < expected_names.size(); ++index) {
                if (rows[index].empty() || rows[index][0] != expected_names[index]) {
                    return Fail(EINVAL, "managed result relation column conflict: " + std::string(name));
                }
            }
            return 0;
        }
        SqliteObject object;
        std::string error;
        if (ReadSqliteObject(channel_, name, &object, &error) != 0) {
            return Fail(EIO, "failed to inspect managed result relation: " + error);
        }
        if (!object.found) {
            const int create_status = channel_->ExecuteSql(expected_sql.c_str());
            const std::string create_error = channel_->GetLastError();
            object = {};
            if (ReadSqliteObject(channel_, name, &object, &error) != 0 || !object.found) {
                return Fail(EIO, create_status < 0 ? "failed to create managed result relation: " + create_error
                                                   : "failed to verify managed result relation: " + error);
            }
        }
        if (object.type != "view" || NormalizeSql(object.sql) != NormalizeSql(expected_sql)) {
            return Fail(EINVAL, "managed result relation contract conflict: " + std::string(name));
        }
        return 0;
    }

    IDatabaseChannel* channel_ = nullptr;
    std::string input_namespace_;
    std::optional<uint32_t> retention_days_;
    DatabaseBackend backend_ = DatabaseBackend::kSqlite;
    bool supported_backend_ = false;
    int last_code_ = 0;
    std::string last_error_;
};

}  // namespace

std::unique_ptr<INpmResultConsumerFactoryV1> MakeNpmDatabaseResultConsumerFactory(
    IDatabaseChannel* channel, std::string input_namespace, std::optional<uint32_t> retention_days) {
    return std::make_unique<NpmDatabaseResultConsumerFactory>(channel, std::move(input_namespace), retention_days);
}

}  // namespace flowsql::npm
