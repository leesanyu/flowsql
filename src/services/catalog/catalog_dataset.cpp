// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <framework/interfaces/idataframe_metadata.h>
#include <sys/stat.h>
#include <common/channel_dataset_arrow.hpp>
#include "catalog_plugin.h"

namespace flowsql::catalog {
namespace {
struct Statement {
    sqlite3_stmt* p = nullptr;
    Statement(sqlite3* db, const char* sql) {
        if (!db || sqlite3_prepare_v2(db, sql, -1, &p, nullptr) != SQLITE_OK) throw std::runtime_error("STORAGE_ERROR");
    }
    ~Statement() { sqlite3_finalize(p); }
    void Bind(int n, const std::string& v) { sqlite3_bind_text(p, n, v.c_str(), v.size(), SQLITE_TRANSIENT); }
    std::string Text(int n) { return reinterpret_cast<const char*>(sqlite3_column_text(p, n)); }
    int Step() {
        int rc = sqlite3_step(p);
        if (rc != SQLITE_DONE && rc != SQLITE_ROW) throw std::runtime_error("STORAGE_ERROR");
        return rc;
    }
};
std::string Stamp(const std::string& path) {
    struct stat st {};
    if (stat(path.c_str(), &st) != 0) return {};
    return std::to_string(st.st_dev) + ":" + std::to_string(st.st_ino) + ":" + std::to_string(st.st_size) + ":" +
           std::to_string(st.st_mtim.tv_sec) + ":" + std::to_string(st.st_mtim.tv_nsec);
}
}  // namespace
// Caller holds mu_. This is metadata in the existing Catalog DB, never a second dictionary registry.
std::string CatalogPlugin::DatasetIdentity(const std::string& name, uint64_t generation, bool replace) {
    try {
        Statement old(operator_db_, "SELECT object_id,file_stamp FROM dataset_identities WHERE name=?");
        old.Bind(1, name);
        const auto stamp = Stamp(CsvPath(name));
        const auto previous = dataset_generations_.find(name);
        replace = replace || (previous != dataset_generations_.end() && previous->second != generation);
        std::string id;
        if (old.Step() == SQLITE_ROW && !replace && old.Text(1) == stamp) id = old.Text(0);
        if (id.empty()) id = dataset::Token();
        Statement put(operator_db_,
                      "INSERT INTO dataset_identities VALUES(?,?,?) ON CONFLICT(name) DO UPDATE SET "
                      "object_id=excluded.object_id,file_stamp=excluded.file_stamp");
        put.Bind(1, name);
        put.Bind(2, id);
        put.Bind(3, stamp);
        put.Step();
        dataset_generations_[name] = generation;
        return id;
    } catch (const std::exception&) {
        return {};
    }
}
int CatalogPlugin::RemoveDatasetIdentity(const std::string& name) {
    if (!operator_db_) return 0;
    try {
        Statement s(operator_db_, "DELETE FROM dataset_identities WHERE name=?");
        s.Bind(1, name);
        s.Step();
        dataset_generations_.erase(name);
        return 0;
    } catch (const std::exception&) {
        return -1;
    }
}
int CatalogPlugin::RenameDatasetIdentity(const std::string& old_name, const std::string& new_name) {
    try {
        Statement s(operator_db_, "UPDATE dataset_identities SET name=? WHERE name=?");
        s.Bind(1, new_name);
        s.Bind(2, old_name);
        s.Step();
        dataset_generations_[new_name] = dataset_generations_[old_name];
        dataset_generations_.erase(old_name);
        return 0;
    } catch (const std::exception&) {
        return -1;
    }
}
int CatalogPlugin::Describe(const ChannelDatasetRequestV1& request, ChannelDatasetSnapshotV1* out,
                            DataSemanticsErrorV1* error) {
    using namespace dataset;
    if (!out) return Fail(error, "INVALID_REQUEST", "output is required");
    *out = {};
    if (error) *error = {};
    Json locator;
    locator.Parse(request.locator_json.c_str());
    if (!locator.IsObject() || dataset::Get(locator, "kind") != "dataframe" || dataset::Get(locator, "name").empty() ||
        !request.timeout_ms || request.timeout_ms > 30000)
        return Fail(error, "INVALID_REQUEST", "a named dataframe locator is required");
    std::lock_guard<std::mutex> lock(mu_);
    const auto name = dataset::Get(locator, "name");
    auto it = channels_.find(name);
    if (it == channels_.end()) return Fail(error, "NOT_FOUND", "dataframe does not exist");
    auto* reader = dynamic_cast<IDataFrameMetadataV1*>(it->second.get());
    if (!reader) return Fail(error, "UNSUPPORTED_SOURCE", "channel has no metadata capability");
    auto meta = reader->DescribeMetadata();
    if (!meta.error.empty()) return Fail(error, "SOURCE_UNAVAILABLE", meta.error);
    auto id = DatasetIdentity(name, meta.replacement_generation, false);
    if (id.empty()) return Fail(error, "STORAGE_ERROR", "dataset identity cannot be persisted");
    Json owner;
    auto& a = owner.GetAllocator();
    Value fields, metadata(rapidjson::kObjectType);
    try {
        if (!meta.pending) {
            if (!meta.schema || meta.schema->num_fields() == 0)
                return Fail(error, "INCOMPLETE_STRUCTURE", "Arrow schema is unavailable");
            fields = ArrowFields(*meta.schema, a);
        }
        auto snapshot =
            Snapshot(ProviderId(), id, locator, fields, metadata, "", "known", meta.pending ? "pending" : "known");
        out->document_json = std::make_shared<const std::string>(Encode(snapshot));
        return 0;
    } catch (const std::exception& e) {
        return Fail(error, "INCOMPLETE_STRUCTURE", e.what());
    }
}
}  // namespace flowsql::catalog
