// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "exploration_service.h"

#include "dataframe_workflow.h"
#include "numeric_display.h"

#include <openssl/sha.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace flowsql::visualization {
namespace {
std::string JsonString(const std::string& value) {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    writer.String(value.data(), value.size());
    return buffer.GetString();
}
std::string Hash(const std::string& value) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), digest);
    std::ostringstream output;
    for (auto byte : digest) output << std::hex << std::setfill('0') << std::setw(2) << int(byte);
    return output.str();
}
std::string FieldJson(const FieldV1& field) {
    return "{\"field_id\":" + JsonString(field.id) + ",\"label\":" + JsonString(field.name) +
           ",\"type\":" + JsonString(field.type) + ",\"native_type\":" + JsonString(field.native_type) +
           ",\"nullable\":" + (field.nullable ? "true}" : "false}");
}
std::string FieldsJson(const std::vector<FieldV1>& fields) {
    std::string json = "[";
    for (const auto& field : fields) {
        if (json.size() > 1) json += ',';
        json += FieldJson(field);
    }
    return json + ']';
}
std::string Revision(const SourceSchemaV1& source) { return Hash(FieldsJson(source.fields)); }
// Browser-facing type classes; nested and binary fields stay unsupported instead of being coerced.
std::string ArrowFieldType(const std::shared_ptr<arrow::DataType>& type) {
    switch (type->id()) {
        case arrow::Type::INT8:
        case arrow::Type::INT16:
        case arrow::Type::INT32:
        case arrow::Type::INT64:
        case arrow::Type::UINT8:
        case arrow::Type::UINT16:
        case arrow::Type::UINT32:
        case arrow::Type::UINT64:
        case arrow::Type::HALF_FLOAT:
        case arrow::Type::FLOAT:
        case arrow::Type::DOUBLE:
        case arrow::Type::DECIMAL:
            return "number";
        case arrow::Type::BOOL:
            return "boolean";
        case arrow::Type::STRING:
        case arrow::Type::LARGE_STRING:
        case arrow::Type::DATE32:
        case arrow::Type::DATE64:
        case arrow::Type::TIMESTAMP:
            return "string";
        default:
            return "unsupported";
    }
}
std::string StringCell(const ExplorationValueV1& cell) {
    if (auto value = std::get_if<std::string>(&cell)) return *value;
    throw QueryFailure("DATABASE_ERROR", "数据表元信息无效");
}
int64_t IntCell(const ExplorationValueV1& cell) {
    if (auto value = std::get_if<int64_t>(&cell)) return *value;
    throw QueryFailure("DATABASE_ERROR", "数据表元信息无效");
}
void Error(httplib::Response& response, const QueryFailure& error, const std::string& id = "") {
    response.status = error.code == "SOURCE_UNAVAILABLE" || error.code == "DATA_PLANE_UNAVAILABLE" ? 503
                      : error.code == "BUSY"                                                       ? 429
                      : error.code == "TIMEOUT"                                                    ? 408
                      : (error.code == "SCHEMA_CHANGED" || error.code == "DUPLICATE_REQUEST" ||
                         error.code == "SNAPSHOT_MISMATCH" || error.code == "SNAPSHOT_EXPIRED")
                          ? 409
                      : error.code == "RESULT_LIMIT" || error.code == "SNAPSHOT_LIMIT" ? 413
                                                                                       : 400;
    response.set_content("{\"request_id\":" + JsonString(id) + ",\"code\":" + JsonString(error.code) +
                             ",\"message\":" + JsonString(error.what()) + "}",
                         "application/json");
}
rapidjson::Document Parse(const std::string& text) {
    if (text.size() > 1024 * 1024) throw QueryFailure("INVALID_WORKFLOW", "请求过大");
    int depth = 0;
    bool string = false, escaped = false;
    for (char c : text) {
        if (string) {
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                string = false;
        } else if (c == '"')
            string = true;
        else if (c == '{' || c == '[') {
            if (++depth > 32) throw QueryFailure("INVALID_WORKFLOW", "请求嵌套过深");
        } else if (c == '}' || c == ']')
            --depth;
    }
    rapidjson::Document document;
    document.Parse(text.data(), text.size());
    if (document.HasParseError() || !document.IsObject())
        throw QueryFailure("INVALID_WORKFLOW", "请求不是有效 JSON 对象");
    return document;
}
std::string Text(const rapidjson::Value& value, const char* key) {
    if (!value.HasMember(key) || !value[key].IsString() || value[key].GetStringLength() > 256)
        throw QueryFailure("INVALID_WORKFLOW", "缺少必要的请求字段");
    return {value[key].GetString(), value[key].GetStringLength()};
}
}  // namespace

bool ExplorationService::Init(IQuerier* querier, QueryLimitsV1 limits, std::string dataframe_upstream,
                              std::string dataframe_shared_dir) {
    limits_ = limits;
    if (querier) {
        factory_ = static_cast<IDatabaseFactory*>(querier->First(IID_DATABASE_FACTORY));
        leases_ = static_cast<IDatabaseChannelLeaseProviderV1*>(querier->First(IID_DATABASE_CHANNEL_LEASE_PROVIDER_V1));
    }
    // A configured but unusable data plane keeps the database path available and reports the reason.
    if (!dataframe_upstream.empty() || !dataframe_shared_dir.empty())
        dataframes_.Init(std::move(dataframe_upstream), std::move(dataframe_shared_dir), limits_.timeout_ms);
    return !factory_ || leases_;
}
void ExplorationService::Start() {
    monitor_ = std::thread([this] {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!stopping_) {
            wake_.wait_for(lock, std::chrono::milliseconds(10));
            const auto now = std::chrono::steady_clock::now();
            for (auto& [id, request] : active_) {
                if (now >= request.deadline) request.timeout = true;
                if ((request.timeout || request.cancelled) && request.session) request.session->Cancel();
            }
        }
    });
}
std::shared_ptr<IDatabaseChannel> ExplorationService::Lease(const std::string& dialect, const std::string& channel) {
    auto lease = leases_ ? leases_->AcquireChannel(dialect.c_str(), channel.c_str()) : nullptr;
    if (!lease) throw QueryFailure("SOURCE_UNAVAILABLE", "数据库通道不可用");
    return lease;
}
std::shared_ptr<IDatabaseExplorationSessionV1> ExplorationService::Session(
    const std::shared_ptr<IDatabaseChannel>& lease) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || sessions_count_ >= limits_.max_active) throw QueryFailure("BUSY", "数据库会话繁忙");
    auto* source = dynamic_cast<IDatabaseExplorationSourceV1*>(lease.get());
    std::shared_ptr<IDatabaseExplorationSessionV1> session;
    DatabaseSnapshotOptionsV1 options;
    options.operation_timeout_ms = limits_.timeout_ms;
    if (!source || source->CreateExplorationSession(options, &session))
        throw QueryFailure("SOURCE_UNAVAILABLE", "无法创建数据库只读会话");
    ++sessions_count_;
    auto* pointer = session.get();
    std::shared_ptr<IDatabaseExplorationSessionV1> owned(
        pointer, [this, session = std::move(session), lease](IDatabaseExplorationSessionV1*) mutable {
            session.reset();
            std::lock_guard<std::mutex> guard(mutex_);
            --sessions_count_;
        });
    sessions_.erase(std::remove_if(sessions_.begin(), sessions_.end(), [](const auto& item) { return item.expired(); }),
                    sessions_.end());
    sessions_.push_back(owned);
    return owned;
}
ExplorationResultV1 ExplorationService::Query(IDatabaseExplorationSessionV1& session, const std::string& sql,
                                              const std::vector<ExplorationValueV1>& values) {
    auto parameters = BindValues(values);
    ExplorationResultV1 result;
    if (session.Query(sql.c_str(), parameters.data(), parameters.size(), limits_.max_rows + 1, limits_.max_bytes,
                      &result)) {
        const auto code = session.QueryError();
        throw QueryFailure(
            code == "CANCELLED" || code == "TIMEOUT" || code == "RESULT_LIMIT" || code == "UNSUPPORTED_TYPE"
                ? code
                : "DATABASE_ERROR",
            code == "CANCELLED"      ? "查询已取消"
            : code == "TIMEOUT"      ? "查询超过计算期限"
            : code == "RESULT_LIMIT" ? "结果超过服务上限，请增加过滤或限制行数"
                                     : "数据库查询失败，请刷新数据源");
    }
    return result;
}
std::vector<ExplorationService::Source> ExplorationService::Sources() {
    std::vector<Source> result;
    std::vector<std::pair<std::string, std::string>> channels;
    if (factory_)
        factory_->List([&](const char* type, const char* name, const char*) {
            if (std::string(type) == "sqlite" || std::string(type) == "mysql" || std::string(type) == "postgres" ||
                std::string(type) == "clickhouse")
                channels.emplace_back(type, name);
        });
    for (const auto& [dialect, name] : channels) {
        try {
            auto lease = Lease(dialect, name);
            auto session = Session(lease);
            auto tables = Query(
                *session,
                dialect == "sqlite"
                    ? "SELECT name,'' FROM sqlite_schema WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name"
                : dialect == "postgres"
                    ? "SELECT tablename,schemaname FROM pg_catalog.pg_tables WHERE schemaname NOT IN "
                      "('pg_catalog','information_schema') AND schemaname NOT LIKE 'pg_toast%' AND "
                      "has_table_privilege(quote_ident(schemaname)||'.'||quote_ident(tablename),'SELECT') ORDER BY "
                      "schemaname,tablename"
                : dialect == "clickhouse"
                    ? "SELECT name,database FROM system.tables WHERE database=currentDatabase() AND is_temporary=0 AND "
                      "engine NOT IN ('View','MaterializedView','LiveView','WindowView') ORDER BY name"
                    : "SELECT table_name,table_schema FROM information_schema.tables WHERE table_schema=DATABASE() AND "
                      "table_type='BASE TABLE' ORDER BY table_name");
            for (const auto& row : tables.rows) {
                const auto table = StringCell(row[0]), schema = StringCell(row[1]);
                const auto identity = dialect + ":" + std::to_string(name.size()) + ":" + name + ":" +
                                      std::to_string(schema.size()) + ":" + schema + ":" + table;
                result.push_back({Hash(identity), name, table, dialect, schema});
            }
        } catch (const QueryFailure& error) {
            if (error.code == "BUSY") throw;
            throw QueryFailure("SOURCE_UNAVAILABLE", "数据库通道“" + name + "”不可用");
        }
    }
    auto frames = DataFrameSources(nullptr);
    result.insert(result.end(), frames.begin(), frames.end());
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& source : result) sources_[source.id] = source;
    }
    return result;
}

std::vector<ExplorationService::Source> ExplorationService::DataFrameSources(std::string* error) const {
    std::vector<Source> result;
    if (!dataframes_.Available()) {
        if (error) *error = "DATA_PLANE_UNAVAILABLE";
        return result;
    }
    for (const auto& source : dataframes_.Sources(error))
        result.push_back({source.source_id, {}, {}, "dataframe", {}, true, source.label});
    return result;
}
ExplorationService::Source ExplorationService::FindSource(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = sources_.find(id);
    if (found != sources_.end()) return found->second;
    throw QueryFailure("SOURCE_UNAVAILABLE", "数据源不存在，请刷新列表");
}
SourceSchemaV1 ExplorationService::Schema(IDatabaseExplorationSessionV1& session, const Source& source) {
    auto rows =
        source.dialect == "sqlite"
            ? Query(session, "SELECT cid,name,type,\"notnull\",pk FROM pragma_table_info(?) ORDER BY cid",
                    {source.table})
        : source.dialect == "postgres"
            ? Query(session,
                    "SELECT a.attnum-1,a.attname,pg_catalog.format_type(a.atttypid,a.atttypmod),CASE WHEN a.attnotnull "
                    "THEN 1 ELSE 0 END,0 FROM pg_catalog.pg_attribute a JOIN pg_catalog.pg_class c ON a.attrelid=c.oid "
                    "JOIN pg_catalog.pg_namespace n ON c.relnamespace=n.oid WHERE n.nspname=? AND c.relname=? AND "
                    "a.attnum>0 AND NOT a.attisdropped ORDER BY a.attnum",
                    {source.schema, source.table})
        : source.dialect == "clickhouse"
            ? Query(session,
                    "SELECT position-1,name,type,if(startsWith(type,'Nullable('),0,1),0 FROM system.columns WHERE "
                    "database=? AND table=? ORDER BY position",
                    {source.schema, source.table})
            : Query(session,
                    "SELECT ordinal_position-1,column_name,column_type,CASE WHEN is_nullable='YES' THEN 0 ELSE 1 END,0 "
                    "FROM information_schema.columns WHERE table_schema=? AND table_name=? ORDER BY ordinal_position",
                    {source.schema, source.table});
    SourceSchemaV1 schema{source.table, {}, source.dialect, source.schema};
    for (const auto& row : rows.rows) {
        auto native = StringCell(row[2]);
        std::string upper = native;
        std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return std::toupper(c); });
        std::string type = "string";
        if (upper.find("INT") != std::string::npos || upper.find("REAL") != std::string::npos ||
            upper.find("FLOA") != std::string::npos || upper.find("DOUB") != std::string::npos ||
            (source.dialect != "sqlite" &&
             (upper.find("DEC") != std::string::npos || upper.find("NUM") != std::string::npos)))
            type = "number";
        if (upper == "BOOLEAN" || upper == "BOOL" || (source.dialect == "mysql" && upper == "TINYINT(1)"))
            type = "boolean";
        else if (type != "number" &&
                 (upper.find("BLOB") != std::string::npos || upper.find("DEC") != std::string::npos ||
                  upper.find("NUM") != std::string::npos || upper.find("BINARY") != std::string::npos ||
                  upper == "JSON" || upper == "BIT"))
            type = "unsupported";
        if (source.dialect == "postgres") {
            if (upper == "BOOLEAN")
                type = "boolean";
            else if (upper == "SMALLINT" || upper == "INTEGER" || upper == "BIGINT" || upper == "REAL" ||
                     upper == "DOUBLE PRECISION" || upper.rfind("NUMERIC", 0) == 0)
                type = "number";
            else if (upper == "TEXT" || upper.rfind("CHARACTER", 0) == 0 || upper == "DATE" ||
                     upper.rfind("TIMESTAMP", 0) == 0 || upper.rfind("TIME ", 0) == 0 || upper == "UUID")
                type = "string";
            else
                type = "unsupported";
            if (upper.find("[]") != std::string::npos) type = "unsupported";
        }
        if (source.dialect == "clickhouse") {
            auto type_name = native;
            while ((type_name.rfind("Nullable(", 0) == 0 || type_name.rfind("LowCardinality(", 0) == 0) &&
                   type_name.back() == ')')
                type_name = type_name.substr(type_name.find('(') + 1, type_name.size() - type_name.find('(') - 2);
            if (type_name.rfind("Int", 0) == 0 || type_name.rfind("UInt", 0) == 0 || type_name.rfind("Float", 0) == 0 ||
                type_name.rfind("Decimal", 0) == 0)
                type = "number";
            else if (type_name == "Bool")
                type = "boolean";
            else if (type_name == "String" || type_name.rfind("FixedString(", 0) == 0 ||
                     type_name.rfind("Date", 0) == 0 || type_name == "UUID" || type_name == "IPv4" ||
                     type_name == "IPv6")
                type = "string";
            else
                type = "unsupported";
        }
        schema.fields.push_back(
            {"f" + std::to_string(IntCell(row[0])), StringCell(row[1]), type, native, IntCell(row[3]) == 0});
    }
    if (schema.fields.empty()) throw QueryFailure("SCHEMA_CHANGED", "表结构已变化，请刷新数据源");
    return schema;
}

void ExplorationService::Routes(httplib::Server& server) {
    server.set_payload_max_length(1024 * 1024);
    server.Get("/api/visualization/v1/sources", [this](const auto&, auto& response) {
        try {
            std::string body = "{\"sources\":[";
            for (const auto& source : Sources()) {
                if (body.back() != '[') body += ',';
                body += "{\"source_id\":" + JsonString(source.id) + ",\"dialect\":" + JsonString(source.dialect) +
                        ",\"kind\":" + JsonString(source.dataframe ? "dataframe" : "database") + ",\"label\":" +
                        JsonString(source.dataframe
                                       ? source.label
                                       : source.channel + "." + (source.schema.empty() ? "" : source.schema + ".") +
                                             source.table) +
                        "}";
            }
            response.set_content(body + "]}", "application/json");
        } catch (const QueryFailure& error) {
            Error(response, error);
        }
    });
    server.Get(
        R"(/api/visualization/v1/sources/([a-f0-9]{32}|[a-f0-9]{64}))", [this](const auto& request, auto& response) {
            try {
                const auto source = FindSource(request.matches[1]);
                if (source.dataframe) {
                    // Discovery only: never create a snapshot as a side effect of describing a source.
                    response.set_content("{\"source_id\":" + JsonString(source.id) +
                                             ",\"kind\":\"dataframe\",\"label\":" + JsonString(source.label) +
                                             ",\"fields\":[],\"capabilities\":[\"projection\",\"filter\",\"group\","
                                             "\"count\",\"sum\",\"mean\",\"min\",\"max\",\"distinctCount\"],"
                                             "\"time_bucket_fields\":[],\"status\":\"available\"}",
                                         "application/json");
                    return;
                }
                auto lease = Lease(source.dialect, source.channel);
                auto session = Session(lease);
                const auto schema = Schema(*session, source);
                std::string bucket_fields = "[";
                for (const auto& field : schema.fields) {
                    if (!SupportsTimeBucket(field, source.dialect)) continue;
                    if (bucket_fields.size() > 1) bucket_fields += ',';
                    bucket_fields += JsonString(field.id);
                }
                bucket_fields += ']';
                response.set_content(
                    "{\"source_id\":" + JsonString(source.id) +
                        ",\"kind\":\"database\",\"dialect\":" + JsonString(source.dialect) + ",\"label\":" +
                        JsonString(source.channel + "." + (source.schema.empty() ? "" : source.schema + ".") +
                                   source.table) +
                        ",\"schema_revision\":" + JsonString(Revision(schema)) +
                        ",\"fields\":" + FieldsJson(schema.fields) +
                        ",\"capabilities\":[\"projection\",\"filter\",\"group\",\"count\",\"sum\","
                        "\"mean\",\"min\",\"max\",\"distinctCount\",\"day\",\"month\"],\"time_bucket_fields\":" +
                        bucket_fields +
                        ","
                        "\"workflow_version\":\"graphic-walker-0.5.3\",\"semantics\":{\"timezone\":"
                        "\"source_calendar\",\"collation\":\"source column collation; temporal values retain source "
                        "timezone\"}}",
                    "application/json");
            } catch (const QueryFailure& error) {
                Error(response, error);
            }
        });
    server.Post("/api/visualization/v1/snapshots/acquire",
                [this](const auto& request, auto& response) { AcquireSnapshot(request, response); });
    server.Post("/api/visualization/v1/snapshots/renew", [this](const auto& request, auto& response) {
        try {
            const auto document = Parse(request.body);
            const auto snapshot_id = Text(document, "snapshot_id");
            int64_t expires_at = 0;
            if (!dataframes_.Renew(snapshot_id, &expires_at))
                throw QueryFailure("SNAPSHOT_EXPIRED", "快照已过期，请重新获取");
            response.set_content("{\"snapshot_id\":" + JsonString(snapshot_id) +
                                     ",\"expires_at_unix\":" + std::to_string(expires_at) + "}",
                                 "application/json");
        } catch (const QueryFailure& error) {
            Error(response, error);
        }
    });
    server.Post("/api/visualization/v1/snapshots/release", [this](const auto& request, auto& response) {
        try {
            const auto document = Parse(request.body);
            const auto snapshot_id = Text(document, "snapshot_id");
            dataframes_.Release(snapshot_id);
            response.set_content("{\"snapshot_id\":" + JsonString(snapshot_id) + ",\"status\":\"released\"}",
                                 "application/json");
        } catch (const QueryFailure& error) {
            Error(response, error);
        }
    });
    server.Post("/api/visualization/v1/compute",
                [this](const auto& request, auto& response) { Compute(request, response); });
    server.Post("/api/visualization/v1/cancel", [this](const auto& request, auto& response) {
        try {
            const auto document = Parse(request.body);
            const auto id = Text(document, "request_id");
            std::lock_guard<std::mutex> lock(mutex_);
            const auto found = active_.find(id);
            if (found != active_.end()) found->second.cancelled = true;
            if (found != active_.end() && found->second.session) found->second.session->Cancel();
            if (found != active_.end() && found->second.acquire_cancel) found->second.acquire_cancel->store(true);
            response.set_content(
                found == active_.end() ? R"({"status":"not_running"})" : R"({"status":"cancel_requested"})",
                "application/json");
        } catch (const QueryFailure& error) {
            Error(response, error);
        }
    });
}

void ExplorationService::Compute(const httplib::Request& request, httplib::Response& response) {
    std::string id;
    bool registered = false;
    try {
        const auto document = Parse(request.body);
        id = Text(document, "request_id");
        if (!std::regex_match(id, std::regex("[a-fA-F0-9]{8}(-[a-fA-F0-9]{4}){3}-[a-fA-F0-9]{12}")))
            throw QueryFailure("INVALID_WORKFLOW", "request_id 必须是 UUID");
        if (Text(document, "workflow_version") != "graphic-walker-0.5.3" || !document.HasMember("workflow"))
            throw QueryFailure("INVALID_WORKFLOW", "workflow 版本不兼容");
        if (FindSource(Text(document, "source_id")).dataframe) {
            // DataFrame compute is bound to one snapshot version and never touches SQL channels.
            ComputeDataFrame(request, document, response);
            return;
        }
        const auto started = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (active_.count(id)) throw QueryFailure("DUPLICATE_REQUEST", "请求身份正在使用");
            if (stopping_ || active_.size() >= limits_.max_active)
                throw QueryFailure("BUSY", "当前查询繁忙，请稍后重试");
            active_.emplace(id, Active{});
            active_.at(id).deadline = started + std::chrono::milliseconds(limits_.timeout_ms);
            registered = true;
        }
        const auto source = FindSource(Text(document, "source_id"));
        auto lease = Lease(source.dialect, source.channel);
        auto session = Session(lease);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto& active = active_.at(id);
            active.lease = lease;
            active.session = session;
            if (active.cancelled || active.timeout || stopping_) session->Cancel();
        }
        const auto schema = Schema(*session, source);
        if (Text(document, "schema_revision") != Revision(schema))
            throw QueryFailure("SCHEMA_CHANGED", "字段结构已变化，请刷新数据源");
        auto plan = CompileWorkflow(schema, document["workflow"], Text(document, "detail_mode"), limits_.max_rows);
        // Guard actual numeric inputs before native AVG can round a wide integer.
        // Use compiler-owned field provenance and the exact bound filter scope.
        for (const auto& input : plan.numeric_inputs) {
            auto native = input.field.native_type;
            std::transform(native.begin(), native.end(), native.begin(),
                           [](unsigned char c) { return std::toupper(c); });
            const bool decimal_mean = source.dialect == "clickhouse" && input.aggregate == "mean" &&
                                      native.find("DECIMAL") != std::string::npos;
            const bool histogram = input.aggregate == "histogram";
            if (source.dialect != "sqlite" && native.find("INT") == std::string::npos && !decimal_mean && !histogram)
                continue;
            const auto column = QuoteIdentifier(input.field.name, source.dialect);
            auto invalid = "(" + column + ">9007199254740991 OR " + column + "< -9007199254740991)";
            if (histogram && source.dialect == "clickhouse")
                invalid = "(" + invalid + " OR NOT isFinite(toFloat64(" + column + ")))";
            if (source.dialect == "sqlite")
                invalid = "(" + column + " IS NOT NULL AND (typeof(" + column + ") NOT IN ('integer','real') OR (" +
                          (histogram ? "1=1" : "typeof(" + column + ")='integer'") + " AND " + invalid + ")))";
            const auto result =
                Query(*session,
                      "SELECT 1" + plan.filtered_from + (plan.filtered ? " AND " : " WHERE ") + invalid + " LIMIT 1",
                      plan.parameters);
            if (!result.rows.empty())
                throw QueryFailure("UNSUPPORTED_TYPE", "整数输入超出安全范围或含混合存储类型，不能定量聚合");
        }
        auto result = Query(*session, plan.sql, plan.parameters);
        if (result.rows.size() > limits_.max_rows)
            throw QueryFailure("RESULT_LIMIT", "结果超限，请增加过滤或明确选择受限明细");
        std::vector<NumericDisplayV1> displays;
        std::vector<int> display_index(result.columns.size(), -1);
        for (size_t i = 0; i < result.columns.size(); ++i) {
            NumericDisplayV1 display;
            display.field_id = result.columns[i].name;
            auto native = plan.fields[i].native_type;
            bool decimal = false, native_mean = false;
            for (const auto& input : plan.numeric_inputs)
                if (input.output_id == display.field_id) {
                    native = input.field.native_type;
                    native_mean = source.dialect == "clickhouse" && input.aggregate == "mean" &&
                                  native.find("Decimal") != std::string::npos;
                }
            std::transform(native.begin(), native.end(), native.begin(),
                           [](unsigned char c) { return std::toupper(c); });
            decimal = native.find("DECIMAL") != std::string::npos || native.find("NUMERIC") != std::string::npos;
            for (const auto& row : result.rows)
                decimal = decimal || std::holds_alternative<ExplorationDecimalV1>(row[i]);
            if (!decimal) continue;
            if (native_mean) {
                display.origin = "native_decimal_mean";
                display.mode = "approximate";
            } else {
                display.exact_values.reserve(result.rows.size());
                for (const auto& row : result.rows) {
                    if (std::holds_alternative<std::monostate>(row[i]))
                        display.exact_values.emplace_back(std::monostate{});
                    else if (auto value = std::get_if<ExplorationDecimalV1>(&row[i]))
                        display.exact_values.emplace_back(value->text);
                    else if (auto value = std::get_if<int64_t>(&row[i]))
                        display.exact_values.emplace_back(std::to_string(*value));
                    else
                        throw QueryFailure("UNSUPPORTED_TYPE", "源库未返回精确 Decimal 文本");
                }
            }
            display_index[i] = displays.size();
            displays.push_back(std::move(display));
        }
        std::string body =
            "{\"request_id\":" + JsonString(id) + ",\"fields\":" + FieldsJson(plan.fields) + ",\"rows\":[";
        for (const auto& row : result.rows) {
            if (body.back() != '[') body += ',';
            body += '{';
            for (size_t i = 0; i < row.size(); ++i) {
                if (i) body += ',';
                body += JsonString(result.columns[i].name) + ':';
                const auto& cell = row[i];
                if (std::holds_alternative<std::monostate>(cell))
                    body += "null";
                else if (auto value = std::get_if<int64_t>(&cell)) {
                    if (plan.fields[i].type == "boolean") {
                        if (*value != 0 && *value != 1) throw QueryFailure("UNSUPPORTED_TYPE", "布尔字段含非布尔值");
                        body += *value ? "true" : "false";
                        continue;
                    }
                    if (*value > 9007199254740991LL || *value < -9007199254740991LL)
                        throw QueryFailure("UNSUPPORTED_TYPE", "整数超出浏览器安全数值范围，不能定量绘图");
                    body += std::to_string(*value);
                } else if (auto value = std::get_if<double>(&cell)) {
                    if (!std::isfinite(*value)) throw QueryFailure("UNSUPPORTED_TYPE", "非有限数值不能绘图");
                    if (display_index[i] >= 0 && std::fabs(*value) > 9007199254740991.0)
                        throw QueryFailure("UNSUPPORTED_TYPE", "Decimal 均值超出浏览器安全数值范围");
                    std::ostringstream number;
                    number << std::setprecision(17) << *value;
                    body += number.str();
                } else if (auto value = std::get_if<std::string>(&cell)) {
                    if (plan.fields[i].type == "number" || plan.fields[i].type == "boolean")
                        throw QueryFailure("UNSUPPORTED_TYPE", "SQLite 混合存储类型不能定量绘图");
                    body += JsonString(*value);
                } else if (auto value = std::get_if<ExplorationDecimalV1>(&cell)) {
                    const auto number = DecimalForDisplay(value->text);
                    if (!number.exact) displays[display_index[i]].mode = "approximate";
                    std::ostringstream text;
                    text << std::setprecision(17) << number.value;
                    body += text.str();
                } else
                    throw QueryFailure("UNSUPPORTED_TYPE", "不支持二进制绘图");
            }
            body += '}';
            if (body.size() > limits_.max_bytes) throw QueryFailure("RESULT_LIMIT", "序列化结果超过字节上限");
        }
        body += "]";
        if (!displays.empty()) {
            body += ",\"numeric_display\":[";
            for (size_t i = 0; i < displays.size(); ++i) {
                if (i) body += ',';
                const auto& display = displays[i];
                body += "{\"field_id\":" + JsonString(display.field_id) + ",\"mode\":" + JsonString(display.mode) +
                        ",\"origin\":" + JsonString(display.origin);
                if (display.origin == "decimal") {
                    body += ",\"exact_values\":[";
                    for (size_t row = 0; row < display.exact_values.size(); ++row) {
                        if (row) body += ',';
                        const auto& value = display.exact_values[row];
                        body += std::holds_alternative<std::monostate>(value)
                                    ? "null"
                                    : JsonString(std::get<std::string>(value));
                        if (body.size() > limits_.max_bytes)
                            throw QueryFailure("RESULT_LIMIT", "精度元信息超过结果字节上限");
                    }
                    body += ']';
                }
                body += '}';
            }
            body += ']';
        }
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        body += ",\"execution\":\"database\",\"scope\":" + JsonString(plan.scope) +
                ",\"returned_rows\":" + std::to_string(result.rows.size()) +
                ",\"elapsed_ms\":" + std::to_string(elapsed) + "}";
        if (body.size() > limits_.max_bytes)
            throw QueryFailure("RESULT_LIMIT",
                               displays.empty() ? "序列化结果超过字节上限" : "包含精度元信息的序列化结果超过字节上限");
        if (elapsed >= limits_.timeout_ms) throw QueryFailure("TIMEOUT", "查询超过计算期限");
        response.set_content(body, "application/json");
    } catch (const QueryFailure& error) {
        Error(response, error, id);
    } catch (const std::exception&) {
        Error(response, QueryFailure("DATABASE_ERROR", "数据库计算失败"), id);
    }
    if (registered) {
        Active released;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto found = active_.find(id);
            if (found->second.timeout)
                Error(response, QueryFailure("TIMEOUT", "查询超过计算期限"), id);
            else if (found->second.cancelled)
                Error(response, QueryFailure("CANCELLED", "查询已取消"), id);
            released = std::move(found->second);
            active_.erase(found);
        }
    }
}
void ExplorationService::AcquireSnapshot(const httplib::Request& request, httplib::Response& response) {
    std::string id;
    bool registered = false;
    try {
        const auto document = Parse(request.body);
        id = Text(document, "request_id");
        if (!std::regex_match(id, std::regex("[a-fA-F0-9]{8}(-[a-fA-F0-9]{4}){3}-[a-fA-F0-9]{12}")))
            throw QueryFailure("INVALID_WORKFLOW", "request_id 必须是 UUID");
        if (!dataframes_.Available()) throw QueryFailure("DATA_PLANE_UNAVAILABLE", "DataFrame 数据面未配置");
        const auto source_id = Text(document, "source_id");
        const auto source = FindSource(source_id);
        if (!source.dataframe) throw QueryFailure("INVALID_WORKFLOW", "只有 DataFrame 来源可以获取快照");
        auto cancellation = std::make_shared<std::atomic_bool>(false);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (active_.count(id)) throw QueryFailure("DUPLICATE_REQUEST", "请求身份正在使用");
            if (stopping_ || active_.size() >= limits_.max_active)
                throw QueryFailure("BUSY", "当前查询繁忙，请稍后重试");
            active_.emplace(id, Active{});
            active_.at(id).deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(limits_.timeout_ms);
            active_.at(id).acquire_cancel = cancellation;
            registered = true;
        }
        DataFrameSnapshotInfoV1 snapshot;
        std::string failure;
        if (!dataframes_.Acquire(id, source_id, 256ULL * 1024 * 1024, cancellation, limits_.timeout_ms, &snapshot,
                                 &failure)) {
            if (failure == "CANCELLED") throw QueryFailure("CANCELLED", "获取快照已取消");
            if (failure == "SNAPSHOT_LIMIT") throw QueryFailure("SNAPSHOT_LIMIT", "数据超过快照字节预算");
            if (failure == "BUSY") throw QueryFailure("BUSY", "当前采集繁忙，请稍后重试");
            throw QueryFailure("DATA_PLANE_UNAVAILABLE", "DataFrame 数据面不可用或来源已失效");
        }
        std::string fields = "[";
        for (int i = 0; i < snapshot.batch->schema()->num_fields(); ++i) {
            const auto& field = snapshot.batch->schema()->field(i);
            if (fields.size() > 1) fields += ',';
            fields += "{\"field_id\":" + JsonString("f" + std::to_string(i)) +
                      ",\"label\":" + JsonString(field->name()) +
                      ",\"type\":" + JsonString(ArrowFieldType(field->type())) +
                      ",\"native_type\":" + JsonString(field->type()->ToString()) +
                      ",\"nullable\":" + (field->nullable() ? "true}" : "false}");
        }
        fields += ']';
        response.set_content("{\"request_id\":" + JsonString(id) + ",\"source_id\":" + JsonString(snapshot.source_id) +
                                 ",\"snapshot_id\":" + JsonString(snapshot.snapshot_id) +
                                 ",\"schema_revision\":" + JsonString(snapshot.schema_revision) +
                                 ",\"captured_at\":" + JsonString(snapshot.captured_at) +
                                 ",\"row_count\":" + std::to_string(snapshot.row_count) +
                                 ",\"ipc_bytes\":" + std::to_string(snapshot.ipc_bytes) + ",\"expires_at_unix\":" +
                                 std::to_string(snapshot.expires_at_unix) + ",\"fields\":" + fields +
                                 ",\"capabilities\":[\"projection\",\"filter\",\"group\",\"count\",\"sum\","
                                 "\"mean\",\"min\",\"max\",\"distinctCount\",\"day\",\"month\"],"
                                 "\"time_bucket_fields\":[]}",
                             "application/json");
    } catch (const QueryFailure& error) {
        Error(response, error, id);
    }
    if (registered) {
        std::lock_guard<std::mutex> lock(mutex_);
        active_.erase(id);
    }
}

void ExplorationService::ComputeDataFrame(const httplib::Request& request, const rapidjson::Document& document,
                                          httplib::Response& response) {
    (void)request;
    std::string id;
    bool registered = false;
    const auto started = std::chrono::steady_clock::now();
    try {
        id = Text(document, "request_id");
        const auto source_id = Text(document, "source_id");
        const auto snapshot_id = Text(document, "snapshot_id");
        if (snapshot_id.empty()) throw QueryFailure("INVALID_WORKFLOW", "DataFrame 计算必须绑定快照");
        std::shared_ptr<arrow::RecordBatch> batch;
        DataFrameSnapshotInfoV1 meta;
        if (!dataframes_.Lookup(snapshot_id, &batch, &meta))
            throw QueryFailure("SNAPSHOT_EXPIRED", "快照已过期，请重新获取");
        if (meta.source_id != source_id || meta.schema_revision != Text(document, "schema_revision"))
            throw QueryFailure("SNAPSHOT_MISMATCH", "快照与请求的来源或字段版本不一致");
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (active_.count(id)) throw QueryFailure("DUPLICATE_REQUEST", "请求身份正在使用");
            if (stopping_ || active_.size() >= limits_.max_active)
                throw QueryFailure("BUSY", "当前查询繁忙，请稍后重试");
            active_.emplace(id, Active{});
            active_.at(id).deadline = started + std::chrono::milliseconds(limits_.timeout_ms);
            registered = true;
        }
        const auto fields = DescribeArrowSchema(*batch->schema());
        // A bounded detail request stays limited; explicit limit/offset marks the Top-N scope.
        const auto detail = document.HasMember("detail_mode") && document["detail_mode"].IsString()
                                ? std::string(document["detail_mode"].GetString())
                                : std::string("complete");
        // The workflow field keeps the database-path shape ({workflow, limit, offset}); request identity
        // stays outside the plan.
        const auto& plan = document["workflow"];
        auto result = ExecuteDataFrameWorkflow(batch, fields, plan, detail, limits_.max_rows, limits_.max_bytes,
                                               started + std::chrono::milliseconds(limits_.timeout_ms));
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto found = active_.find(id);
            if (found != active_.end() && found->second.timeout) throw QueryFailure("TIMEOUT", "查询超过计算期限");
            if (found != active_.end() && found->second.cancelled) throw QueryFailure("CANCELLED", "查询已取消");
        }
        std::string body = "{\"request_id\":" + JsonString(id) + ",\"source_id\":" + JsonString(source_id) +
                           ",\"snapshot_id\":" + JsonString(snapshot_id) +
                           ",\"schema_revision\":" + JsonString(meta.schema_revision) +
                           ",\"fields\":" + FieldsJson(result.fields) + ",\"rows\":[";
        for (const auto& row : result.rows) {
            if (body.back() != '[') body += ',';
            body += '{';
            for (size_t i = 0; i < row.size(); ++i) {
                if (i) body += ',';
                body += JsonString(result.fields[i].id) + ':';
                const auto& cell = row[i];
                if (std::holds_alternative<std::monostate>(cell)) {
                    body += "null";
                } else if (auto value = std::get_if<int64_t>(&cell)) {
                    if (result.fields[i].type == "boolean") {
                        if (*value != 0 && *value != 1) throw QueryFailure("UNSUPPORTED_TYPE", "布尔字段含非布尔值");
                        body += *value ? "true" : "false";
                        continue;
                    }
                    if (*value > 9007199254740991LL || *value < -9007199254740991LL)
                        throw QueryFailure("UNSUPPORTED_TYPE", "整数超出浏览器安全数值范围，不能定量绘图");
                    body += std::to_string(*value);
                } else if (auto value = std::get_if<double>(&cell)) {
                    if (!std::isfinite(*value)) throw QueryFailure("UNSUPPORTED_TYPE", "非有限数值不能绘图");
                    std::ostringstream number;
                    number << std::setprecision(17) << *value;
                    body += number.str();
                } else if (auto value = std::get_if<std::string>(&cell)) {
                    body += JsonString(*value);
                } else if (auto value = std::get_if<ExplorationDecimalV1>(&cell)) {
                    // Exact decimal text remains available through numeric_display below.
                    const auto number = DecimalForDisplay(value->text);
                    std::ostringstream text;
                    text << std::setprecision(17) << number.value;
                    body += text.str();
                } else {
                    throw QueryFailure("UNSUPPORTED_TYPE", "不支持二进制绘图");
                }
            }
            body += '}';
            if (body.size() > limits_.max_bytes) throw QueryFailure("RESULT_LIMIT", "序列化结果超过字节上限");
        }
        body += "]";
        if (!result.displays.empty()) {
            body += ",\"numeric_display\":[";
            for (size_t i = 0; i < result.displays.size(); ++i) {
                if (i) body += ',';
                body += "{\"field_id\":" + JsonString(result.displays[i].field_id) +
                        ",\"mode\":" + JsonString(result.displays[i].mode) +
                        ",\"origin\":" + JsonString(result.displays[i].origin) + "}";
            }
            body += ']';
        }
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        body += ",\"execution\":\"dataframe\",\"scope\":" + JsonString(result.scope) +
                ",\"returned_rows\":" + std::to_string(result.rows.size()) +
                ",\"elapsed_ms\":" + std::to_string(elapsed) + "}";
        if (body.size() > limits_.max_bytes) throw QueryFailure("RESULT_LIMIT", "序列化结果超过字节上限");
        if (elapsed >= limits_.timeout_ms) throw QueryFailure("TIMEOUT", "查询超过计算期限");
        response.set_content(body, "application/json");
    } catch (const QueryFailure& error) {
        Error(response, error, id);
    } catch (const std::exception&) {
        Error(response, QueryFailure("DATAFRAME_ERROR", "DataFrame 计算失败"), id);
    }
    if (registered) {
        std::lock_guard<std::mutex> lock(mutex_);
        active_.erase(id);
    }
}

void ExplorationService::Stop() {
    std::vector<std::shared_ptr<IDatabaseExplorationSessionV1>> sessions;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        for (auto& [id, request] : active_) request.cancelled = true;
        for (const auto& weak : sessions_)
            if (auto session = weak.lock()) sessions.push_back(std::move(session));
        wake_.notify_all();
    }
    for (const auto& session : sessions) session->Cancel();
    if (monitor_.joinable()) monitor_.join();
    dataframes_.Stop();
}
}  // namespace flowsql::visualization
