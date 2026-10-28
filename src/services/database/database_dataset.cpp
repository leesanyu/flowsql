// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <sys/stat.h>
#include <common/channel_dataset.hpp>
#include <condition_variable>
#include <set>
#include <thread>
#include "database_plugin.h"
#include "drivers/clickhouse_driver.h"
#include "drivers/mysql_driver.h"
#include "drivers/postgres_driver.h"
#include "drivers/sqlite_driver.h"
#include "metadata_deadline.h"

namespace flowsql::database {
namespace {
using namespace dataset;
std::string Param(const std::unordered_map<std::string, std::string>& p, const std::string& key,
                  const std::string& fallback = "") {
    auto it = p.find(key);
    return it == p.end() ? fallback : it->second;
}
std::string Cell(const ExplorationValueV1& v) {
    if (const auto* s = std::get_if<std::string>(&v)) return *s;
    if (const auto* n = std::get_if<int64_t>(&v)) return std::to_string(*n);
    if (const auto* b = std::get_if<std::vector<uint8_t>>(&v)) return {b->begin(), b->end()};
    throw std::runtime_error("invalid catalog field");
}
class QueryBudget {
 public:
    QueryBudget(std::shared_ptr<IDatabaseExplorationSessionV1> query, int64_t deadline)
        : timer_([this, query, deadline] {
              std::unique_lock<std::mutex> lock(mutex_);
              if (!changed_.wait_until(lock, std::chrono::system_clock::time_point(std::chrono::milliseconds(deadline)),
                                       [this] { return done_; })) {
                  lock.unlock();
                  query->Cancel();
              }
          }) {}
    ~QueryBudget() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            done_ = true;
        }
        changed_.notify_all();
        timer_.join();
    }

 private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool done_ = false;
    std::thread timer_;
};
void Gap(Value& gaps, const std::string& path, Alloc& a) {
    Value gap(rapidjson::kObjectType);
    Set(gap, "path", path, a);
    Set(gap, "reason", "unknown", a);
    gaps.PushBack(gap, a);
}
}  // namespace
int DatabasePlugin::Describe(const ChannelDatasetRequestV1& request, ChannelDatasetSnapshotV1* out,
                             DataSemanticsErrorV1* error) {
    using namespace dataset;
    if (!out) return Fail(error, "INVALID_REQUEST", "output required");
    *out = {};
    if (error) *error = {};
    const auto deadline = MetadataIoNow() + request.timeout_ms;
    Json locator;
    if (JsonNestingWithin(request.locator_json, 64)) locator.Parse(request.locator_json.c_str());
    const auto type = dataset::Get(locator, "type"), name = dataset::Get(locator, "name"),
               table = dataset::Get(locator, "table");
    if (!locator.IsObject() || dataset::Get(locator, "kind") != "database_table" || name.empty() || table.empty() ||
        (type != "sqlite" && type != "mysql" && type != "postgres" && type != "clickhouse") || !request.timeout_ms ||
        request.timeout_ms > 30000)
        return Fail(error, "INVALID_REQUEST", "database_table locator and timeout_ms required");
    for (const auto& member : locator.GetObject()) {
        const auto key = Text(member.name);
        if ((key != "kind" && key != "type" && key != "name" && key != "database" && key != "schema" &&
             key != "table") ||
            !member.value.IsString() || Text(member.value).empty() ||
            Text(member.value).find('\0') != std::string::npos)
            return Fail(error, "INVALID_REQUEST", "unknown or invalid locator property", "/locator/" + key);
    }
    std::unordered_map<std::string, std::string> config;
    const auto key = type + "." + name;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = configs_.find(key);
        if (it == configs_.end()) return Fail(error, "NOT_FOUND", "database connection not configured");
        config = it->second;
    }
    try {
        auto target = config;
        target["_dataset_deadline_ms"] = std::to_string(deadline);
        target["timeout"] = std::to_string((request.timeout_ms + 999) / 1000);
        const auto database = dataset::Get(locator, "database", type == "sqlite" ? "main" : Param(config, "database"));
        const auto schema = dataset::Get(locator, "schema", type == "postgres" ? "public" : database);
        if (database.empty() || (type == "postgres" && database != Param(config, "database")) ||
            (type == "sqlite" && (database != "main" || schema != "main")) ||
            ((type == "mysql" || type == "clickhouse") && schema != database))
            return Fail(error, "INVALID_REQUEST", "database/schema does not match backend scope");
        Set(locator, "database", database, locator.GetAllocator());
        if (type == "postgres" || type == "sqlite") Set(locator, "schema", schema, locator.GetAllocator());
        if (type == "sqlite") target["readonly"] = "true";
        auto driver = CreateDriver(type);
        if (!driver || driver->Connect(target))
            return Fail(error, MetadataIoNow() >= deadline ? "SOURCE_TIMEOUT" : "SOURCE_UNAVAILABLE",
                        "metadata connection failed; verify authentication and catalog permissions");
        auto* factory = dynamic_cast<IDbSessionFactoryProvider*>(driver.get());
        auto session = factory ? factory->CreateSession() : nullptr;
        auto query = std::dynamic_pointer_cast<IDatabaseExplorationSessionV1>(session);
        auto* snapshot = dynamic_cast<IDbSnapshotSession*>(session.get());
        if (!query || !snapshot || MetadataIoNow() >= deadline)
            return Fail(error, MetadataIoNow() >= deadline ? "SOURCE_TIMEOUT" : "SOURCE_UNAVAILABLE",
                        "metadata session unavailable");
        DatabaseSnapshotOptionsV1 options;
        options.consistent_transaction = type != "clickhouse";
        options.operation_timeout_ms = static_cast<uint32_t>(deadline - MetadataIoNow());
        if (snapshot->BeginSnapshot(options))
            return Fail(error, MetadataIoNow() >= deadline ? "SOURCE_TIMEOUT" : "SOURCE_UNAVAILABLE",
                        "read-only metadata session failed");
        QueryBudget budget(query, deadline);
        auto run = [&](const std::string& sql, const std::vector<std::string>& values, ExplorationResultV1* result) {
            std::vector<DatabaseParameterV1> params;
            for (const auto& value : values) {
                DatabaseParameterV1 p;
                p.kind = DatabaseParameterKindV1::kString;
                p.data = value.data();
                p.size = value.size();
                params.push_back(p);
            }
            if (MetadataIoNow() >= deadline ||
                query->Query(sql.c_str(), params.data(), params.size(), 65536, 8 * 1024 * 1024, result)) {
                const auto e = query->QueryError();
                return Fail(error,
                            MetadataIoNow() >= deadline || e == "TIMEOUT" ? "SOURCE_TIMEOUT"
                            : e == "RESULT_LIMIT"                         ? "LIMIT_EXCEEDED"
                                                                          : "SOURCE_UNAVAILABLE",
                            "metadata catalog query failed");
            }
            return 0;
        };
        ExplorationResultV1 columns;
        std::string native_identity;
        bool identity_known = false;
        if (type == "sqlite") {
            ExplorationResultV1 object;
            if (run("SELECT rootpage,sql FROM main.sqlite_schema WHERE type='table' AND name=?", {table}, &object))
                return -1;
            if (object.rows.empty()) return Fail(error, "NOT_FOUND", "table not found");
            struct stat st {};
            if (stat(Param(config, "path").c_str(), &st) == 0)
                native_identity = std::to_string(st.st_dev) + ":" + std::to_string(st.st_ino) + ":";
            native_identity += Cell(object.rows[0][0]) + ":" + Cell(object.rows[0][1]);
            if (run("SELECT name,type,CASE WHEN \"notnull\"=1 THEN 'NO' ELSE 'YES' END,pk FROM pragma_table_xinfo(?, "
                    "'main') ORDER BY cid",
                    {table}, &columns))
                return -1;
            ExplorationResultV1 primary;
            if (run("SELECT wr,strict,(SELECT count(*) FROM pragma_index_list(?, 'main') WHERE origin='pk') FROM "
                    "pragma_table_list WHERE schema='main' AND name=?",
                    {table, table}, &primary))
                return -1;
            if (primary.rows.size() != 1) return Fail(error, "SOURCE_UNAVAILABLE", "table metadata unavailable");
            const bool strict_primary = Cell(primary.rows[0][0]) == "1" || Cell(primary.rows[0][1]) == "1";
            const bool rowid_alias = Cell(primary.rows[0][2]) == "0";
            for (auto& row : columns.rows)
                if (Cell(row[3]) != "0" && (strict_primary || rowid_alias)) row[2] = std::string("NO");
        } else if (type == "mysql") {
            if (run("SELECT COLUMN_NAME,COLUMN_TYPE,IS_NULLABLE FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=? "
                    "AND TABLE_NAME=? ORDER BY ORDINAL_POSITION",
                    {database, table}, &columns))
                return -1;
        } else if (type == "postgres") {
            if (run("SELECT a.attname,pg_catalog.format_type(a.atttypid,a.atttypmod),CASE WHEN a.attnotnull OR "
                    "t.typnotnull THEN 'NO' ELSE 'YES' END,d.oid::text||':'||c.oid::text "
                    "FROM pg_catalog.pg_attribute a JOIN pg_catalog.pg_class c ON a.attrelid=c.oid JOIN "
                    "pg_catalog.pg_namespace n ON c.relnamespace=n.oid JOIN pg_catalog.pg_type t ON t.oid=a.atttypid "
                    "JOIN pg_catalog.pg_database d ON d.datname=current_database() WHERE n.nspname=? AND c.relname=? "
                    "AND c.relkind IN ('r','p') AND a.attnum>0 AND NOT a.attisdropped ORDER BY a.attnum",
                    {schema, table}, &columns))
                return -1;
            if (!columns.rows.empty()) {
                native_identity = Cell(columns.rows[0][3]);
                identity_known = true;
            }
        } else {
            ExplorationResultV1 object;
            if (run("SELECT toString(uuid) FROM system.tables WHERE database=? AND name=?", {database, table}, &object))
                return -1;
            if (object.rows.empty()) return Fail(error, "NOT_FOUND", "table not found");
            native_identity = Cell(object.rows[0][0]);
            identity_known = native_identity != "00000000-0000-0000-0000-000000000000";
            if (run("SELECT name,type,if(startsWith(type,'Nullable(') OR "
                    "startsWith(type,'LowCardinality(Nullable('),'YES','NO') FROM system.columns WHERE database=? AND "
                    "table=? ORDER BY position",
                    {database, table}, &columns))
                return -1;
        }
        if (columns.rows.empty()) return Fail(error, "NOT_FOUND", "table not found or catalog access denied");
        Json fields(rapidjson::kArrayType), metadata(rapidjson::kObjectType), gaps(rapidjson::kArrayType);
        auto& fa = fields.GetAllocator();
        std::set<std::string> names;
        for (const auto& row : columns.rows) {
            const auto field_name = Cell(row.at(0)), native_type = Cell(row.at(1)), nullable = Cell(row.at(2));
            if (field_name.empty() || !names.insert(field_name).second)
                return Fail(error, "INCOMPLETE_STRUCTURE", "empty or duplicate catalog column");
            auto field = Field(field_name, Type("database", native_type, fa), nullable == "YES", fa);
            Set(field["type"]["parameters"], "dialect", type, fa);
            if (native_type.empty()) {
                field["type"].SetNull();
                Gap(gaps, "/fields/" + std::to_string(fields.Size()) + "/type", gaps.GetAllocator());
            }
            if (nullable != "YES" && nullable != "NO") {
                field["nullable"].SetNull();
                Gap(gaps, "/fields/" + std::to_string(fields.Size()) + "/nullable", gaps.GetAllocator());
            }
            fields.PushBack(field, fa);
        }
        Set(metadata, "dialect", type, metadata.GetAllocator());
        Set(metadata, "identity_observation", identity_known ? "native_object_id" : "external_recreation_unobservable",
            metadata.GetAllocator());
        if (type == "sqlite") Set(metadata, "type_scope", "column_declaration_only", metadata.GetAllocator());
        Json target_identity(rapidjson::kObjectType);
        for (const char* field : {"host", "port", "database", "path", "socket"})
            Set(target_identity, field, Param(config, field), target_identity.GetAllocator());
        Set(metadata, "connection_target_revision", Hash(Canonical(target_identity)), metadata.GetAllocator());
        Json table_key(rapidjson::kArrayType);
        for (const auto& part : {database, schema, table, identity_known ? native_identity : std::string()})
            table_key.PushBack(Value(part.c_str(), table_key.GetAllocator()), table_key.GetAllocator());
        auto result = Snapshot("database", Hash(Param(config, "dataset_instance_id") + Canonical(table_key)), locator,
                               fields, metadata, Canonical(target_identity) + native_identity,
                               identity_known ? "known" : "unknown", gaps.Empty() ? "known" : "incomplete");
        result["structure_gaps"].CopyFrom(gaps, result.GetAllocator());
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = configs_.find(key);
            if (it == configs_.end() || it->second != config)
                return Fail(error, "SOURCE_CHANGED", "connection configuration changed during observation");
        }
        if (MetadataIoNow() >= deadline) return Fail(error, "SOURCE_TIMEOUT", "metadata deadline exceeded");
        out->document_json = std::make_shared<const std::string>(Encode(result));
        return 0;
    } catch (const std::exception&) {
        return Fail(error, MetadataIoNow() >= deadline ? "SOURCE_TIMEOUT" : "SOURCE_UNAVAILABLE",
                    "metadata observation failed");
    }
}
}  // namespace flowsql::database
