// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include "data_semantics_registry.h"
#include <openssl/sha.h>
#include <rapidjson/pointer.h>
#include <sqlite3.h>
#include <chrono>
#include <filesystem>
#include <memory>
#include <unordered_set>

namespace flowsql::channels::config {
namespace {
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
Statement Prepare(sqlite3* db, const char* sql) {
    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &s, nullptr);
    return {s, sqlite3_finalize};
}
void Bind(sqlite3_stmt* s, int i, const std::string& text) {
    sqlite3_bind_text(s, i, text.data(), static_cast<int>(text.size()), SQLITE_TRANSIENT);
}
std::string Text(sqlite3_stmt* s, int i) {
    const auto* p = sqlite3_column_text(s, i);
    return p ? std::string(reinterpret_cast<const char*>(p), sqlite3_column_bytes(s, i)) : "";
}
std::string Digest(const std::string& text) {
    unsigned char bytes[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(text.data()), text.size(), bytes);
    std::string hex;
    for (unsigned char b : bytes) {
        hex += "0123456789abcdef"[b >> 4];
        hex += "0123456789abcdef"[b & 15];
    }
    return hex;
}
struct Rollback {
    sqlite3* db;
    ~Rollback() {
        if (db) sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
    }
};
std::string Exact(const std::string& kind, const std::string& name, uint64_t rev) {
    return kind + "." + name + "@" + std::to_string(rev);
}
}  // namespace
DataSemanticsRegistry::~DataSemanticsRegistry() { Close(); }
int DataSemanticsRegistry::StorageError(DataSemanticsErrorV1* e) {
    return SemanticsFail(e, "STORAGE_ERROR", db_ ? sqlite3_errmsg(db_) : "database closed");
}
int DataSemanticsRegistry::Open(const std::string& path, DataSemanticsErrorV1* e) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (e) *e = {};
    if (db_ || path.empty() || path == ":memory:")
        return SemanticsFail(e, "STORAGE_ERROR", "persistent unopened database required");
    std::error_code ec;
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);
    if (ec) return SemanticsFail(e, "STORAGE_ERROR", ec.message());
    if (sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
        StorageError(e);
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        return -1;
    }
    sqlite3_busy_timeout(db_, 5000);
    const char* sql =
        "BEGIN IMMEDIATE;"
        "CREATE TABLE IF NOT EXISTS semantics_object("
        "kind TEXT NOT NULL,name TEXT NOT NULL,current_revision INTEGER NOT NULL,"
        "object_provider TEXT,object_id TEXT,PRIMARY KEY(kind,name),UNIQUE(object_provider,object_id));"
        "CREATE TABLE IF NOT EXISTS semantics_revision("
        "kind TEXT NOT NULL,name TEXT NOT NULL,revision INTEGER NOT NULL,document TEXT NOT NULL,sha256 TEXT NOT NULL,"
        "created_at INTEGER NOT NULL,base_revision INTEGER,change_note TEXT NOT NULL,PRIMARY KEY(kind,name,revision));"
        "CREATE TABLE IF NOT EXISTS semantics_reference("
        "source TEXT NOT NULL,target TEXT NOT NULL,field_path TEXT NOT NULL,PRIMARY KEY(source,target,field_path));"
        "CREATE INDEX IF NOT EXISTS semantics_target ON semantics_reference(target);"
        "COMMIT;";
    if (sqlite3_exec(db_, sql, nullptr, nullptr, nullptr) != SQLITE_OK) {
        StorageError(e);
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        sqlite3_close(db_);
        db_ = nullptr;
        return -1;
    }
    auto check = Prepare(db_,
                         "SELECT 1 FROM semantics_object o LEFT JOIN semantics_revision r ON o.kind=r.kind "
                         "AND o.name=r.name AND o.current_revision=r.revision WHERE r.revision IS NULL LIMIT 1");
    if (!check || sqlite3_step(check.get()) != SQLITE_DONE) {
        SemanticsFail(e, "STORAGE_ERROR", "semantics current revision is missing or unreadable");
        check.reset();
        sqlite3_close(db_);
        db_ = nullptr;
        return -1;
    }
    return 0;
}
void DataSemanticsRegistry::Close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_) sqlite3_close(db_);
    db_ = nullptr;
}
int DataSemanticsRegistry::Read(const std::string& reference, DataSemanticsSnapshotV1* out, DataSemanticsErrorV1* e) {
    *out = {};
    SemanticsIdentity id;
    if (!ParseSemanticsReference(reference, &id))
        return SemanticsFail(e, "INVALID_REFERENCE", "exact reference required");
    if (!db_) return SemanticsFail(e, "UNAVAILABLE", "semantics registry is closed");
    auto s = Prepare(db_, "SELECT document,sha256 FROM semantics_revision WHERE kind=? AND name=? AND revision=?");
    if (!s) return StorageError(e);
    Bind(s.get(), 1, id.kind);
    Bind(s.get(), 2, id.name);
    sqlite3_bind_int64(s.get(), 3, id.revision);
    const int rc = sqlite3_step(s.get());
    if (rc == SQLITE_DONE) return SemanticsFail(e, "NOT_FOUND", "exact revision not found");
    if (rc != SQLITE_ROW) return StorageError(e);
    const auto content = Text(s.get(), 0), digest = Text(s.get(), 1);
    rapidjson::Document d;
    if (ParseSemanticsJson(content, &d, e) || !d.IsObject() || !d.HasMember("schema_id") ||
        !d["schema_id"].IsString() || Digest(content) != digest)
        return SemanticsFail(e, "STORAGE_ERROR", "revision content integrity failure");
    *out = {reference, d["schema_id"].GetString(), digest, std::make_shared<const std::string>(content)};
    return 0;
}
int DataSemanticsRegistry::Resolve(const char* reference, DataSemanticsSnapshotV1* out, DataSemanticsErrorV1* e) {
    if (e) *e = {};
    if (!out) return SemanticsFail(e, "INVALID_REFERENCE", "snapshot is required");
    std::lock_guard<std::mutex> lock(mutex_);
    return Read(reference ? reference : "", out, e);
}
int DataSemanticsRegistry::ValidateLinks(const std::string& kind, const rapidjson::Value& d,
                                         const std::vector<SemanticsReference>& refs, std::string* check,
                                         DataSemanticsErrorV1* e) {
    check->clear();
    for (const auto& ref : refs) {
        DataSemanticsSnapshotV1 target;
        if (Read(ref.target, &target, e)) {
            if (e) e->field_path = ref.path;
            return -1;
        }
        if (kind == "binding") {
            rapidjson::Document td;
            td.Parse(target.document_json->c_str());
            if (CheckSemanticsBinding(d, td, check, e)) return -1;
        } else if (ref.target.compare(0, 11, "dictionary.") == 0) {
            rapidjson::Document td;
            td.Parse(target.document_json->c_str());
            std::unordered_set<std::string> target_ids;
            for (const auto& f : td["fields"].GetArray()) target_ids.insert(f["field_id"].GetString());
            const auto slash = ref.path.rfind('/');
            const auto* mapping = rapidjson::Pointer((ref.path.substr(0, slash) + "/field_map").c_str()).Get(d);
            for (const auto& m : mapping->GetObject())
                if (!target_ids.count(m.value.GetString()))
                    return SemanticsFail(e, "INVALID_REFERENCE", "unknown relation target field", ref.path);
        }
    }
    return 0;
}
int DataSemanticsRegistry::Preview(const std::string& kind, const std::string& content, rapidjson::Document* out,
                                   std::string* check, DataSemanticsErrorV1* e) {
    if (e) *e = {};
    std::vector<SemanticsReference> refs;
    if (NormalizeSemantics(kind, content, out, &refs, e)) return -1;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return SemanticsFail(e, "UNAVAILABLE", "registry closed");
    return ValidateLinks(kind, *out, refs, check, e);
}
int DataSemanticsRegistry::Save(const SemanticsSaveRequest& r, SemanticsSaveResult* out, DataSemanticsErrorV1* e) {
    if (e) *e = {};
    if (!out) return SemanticsFail(e, "INVALID_DOCUMENT", "save output required");
    *out = {};
    if (!SemanticsName(r.name) || r.expected_current_revision > kSemanticsMaxRevision ||
        r.base_revision > kSemanticsMaxRevision || r.change_note.size() > 4096)
        return SemanticsFail(e, "INVALID_DOCUMENT", "invalid identity or revision");
    rapidjson::Document d;
    std::vector<SemanticsReference> refs;
    if (NormalizeSemantics(r.kind, r.document_json, &d, &refs, e)) return -1;
    const auto content = SemanticsJson(d), digest = Digest(content);
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return SemanticsFail(e, "UNAVAILABLE", "semantics registry is closed");
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK) return StorageError(e);
    Rollback rollback{db_};
    std::string binding_check;
    if (ValidateLinks(r.kind, d, refs, &binding_check, e)) return -1;
    uint64_t current = 0;
    {
        auto s = Prepare(db_, "SELECT current_revision FROM semantics_object WHERE kind=? AND name=?");
        if (!s) return StorageError(e);
        Bind(s.get(), 1, r.kind);
        Bind(s.get(), 2, r.name);
        const int rc = sqlite3_step(s.get());
        if (rc == SQLITE_ROW)
            current = sqlite3_column_int64(s.get(), 0);
        else if (rc != SQLITE_DONE)
            return StorageError(e);
    }
    if (r.base_revision) {
        DataSemanticsSnapshotV1 base;
        if (Read(Exact(r.kind, r.name, r.base_revision), &base, e)) return -1;
    }
    if (current) {
        DataSemanticsSnapshotV1 previous;
        if (Read(Exact(r.kind, r.name, current), &previous, e)) return -1;
        if (r.kind == "binding") {
            rapidjson::Document prior;
            prior.Parse(previous.document_json->c_str());
            if (prior["object_key"] != d["object_key"])
                return SemanticsFail(e, "BINDING_MISMATCH", "object_key is immutable", "/object_key");
        }
        if (*previous.document_json == content) {
            if (sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) return StorageError(e);
            rollback.db = nullptr;
            out->snapshot = previous;
            out->structure_check = binding_check;
            return 0;
        }
    }
    if (current != r.expected_current_revision)
        return SemanticsFail(e, "REVISION_CONFLICT", "current revision changed");
    if (current >= kSemanticsMaxRevision) return SemanticsFail(e, "LIMIT_EXCEEDED", "revision limit reached");
    if (r.kind == "binding") {
        auto s = Prepare(db_, "SELECT kind,name FROM semantics_object WHERE object_provider=? AND object_id=?");
        if (!s) return StorageError(e);
        Bind(s.get(), 1, d["object_key"]["provider_id"].GetString());
        Bind(s.get(), 2, d["object_key"]["object_id"].GetString());
        const auto rc = sqlite3_step(s.get());
        if (rc == SQLITE_ROW && (Text(s.get(), 0) != r.kind || Text(s.get(), 1) != r.name))
            return SemanticsFail(e, "BINDING_MISMATCH", "object_key already belongs to another binding", "/object_key");
        if (rc != SQLITE_ROW && rc != SQLITE_DONE) return StorageError(e);
    }
    const auto next = current + 1;
    const auto exact = Exact(r.kind, r.name, next);
    const auto now =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    {
        auto s = Prepare(db_, "INSERT INTO semantics_revision VALUES(?,?,?,?,?,?,?,?)");
        if (!s) return StorageError(e);
        Bind(s.get(), 1, r.kind);
        Bind(s.get(), 2, r.name);
        sqlite3_bind_int64(s.get(), 3, next);
        Bind(s.get(), 4, content);
        Bind(s.get(), 5, digest);
        sqlite3_bind_int64(s.get(), 6, now);
        if (r.base_revision)
            sqlite3_bind_int64(s.get(), 7, r.base_revision);
        else
            sqlite3_bind_null(s.get(), 7);
        Bind(s.get(), 8, r.change_note);
        if (sqlite3_step(s.get()) != SQLITE_DONE) return StorageError(e);
    }
    {
        auto s = Prepare(
            db_,
            "INSERT INTO semantics_object(kind,name,current_revision,object_provider,object_id) VALUES(?,?,?,?,?) "
            "ON CONFLICT(kind,name) DO UPDATE SET current_revision=excluded.current_revision");
        if (!s) return StorageError(e);
        Bind(s.get(), 1, r.kind);
        Bind(s.get(), 2, r.name);
        sqlite3_bind_int64(s.get(), 3, next);
        if (r.kind == "binding") {
            Bind(s.get(), 4, d["object_key"]["provider_id"].GetString());
            Bind(s.get(), 5, d["object_key"]["object_id"].GetString());
        } else {
            sqlite3_bind_null(s.get(), 4);
            sqlite3_bind_null(s.get(), 5);
        }
        if (sqlite3_step(s.get()) != SQLITE_DONE) return StorageError(e);
    }
    for (const auto& ref : refs) {
        auto s = Prepare(db_, "INSERT INTO semantics_reference VALUES(?,?,?)");
        if (!s) return StorageError(e);
        Bind(s.get(), 1, exact);
        Bind(s.get(), 2, ref.target);
        Bind(s.get(), 3, ref.path);
        if (sqlite3_step(s.get()) != SQLITE_DONE) return StorageError(e);
    }
    if (sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) return StorageError(e);
    rollback.db = nullptr;
    out->snapshot = {exact, d["schema_id"].GetString(), digest, std::make_shared<const std::string>(content)};
    out->created_revision = true;
    out->structure_check = binding_check;
    return 0;
}
int DataSemanticsRegistry::BindingLookup(const std::string& provider, const std::string& object,
                                         DataSemanticsSnapshotV1* out, DataSemanticsErrorV1* e) {
    if (e) *e = {};
    if (!out) return SemanticsFail(e, "INVALID_DOCUMENT", "snapshot required");
    *out = {};
    if (provider.empty() || object.empty())
        return SemanticsFail(e, "INVALID_DOCUMENT", "object_key required", "/object_key");
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return SemanticsFail(e, "UNAVAILABLE", "registry closed");
    auto s = Prepare(db_,
                     "SELECT name,current_revision FROM semantics_object WHERE kind='binding' AND object_provider=? "
                     "AND object_id=?");
    if (!s) return StorageError(e);
    Bind(s.get(), 1, provider);
    Bind(s.get(), 2, object);
    int rc = sqlite3_step(s.get());
    if (rc == SQLITE_DONE) return SemanticsFail(e, "NOT_FOUND", "binding not found");
    if (rc != SQLITE_ROW) return StorageError(e);
    return Read(Exact("binding", Text(s.get(), 0), sqlite3_column_int64(s.get(), 1)), out, e);
}
int DataSemanticsRegistry::References(const std::string& target, bool history, std::vector<SemanticsReferenceItem>* out,
                                      DataSemanticsErrorV1* e) {
    if (e) *e = {};
    out->clear();
    std::lock_guard<std::mutex> lock(mutex_);
    DataSemanticsSnapshotV1 snapshot;
    if (Read(target, &snapshot, e)) return -1;
    auto s = Prepare(db_,
                     "SELECT x.source,x.field_path,o.current_revision=r.revision FROM semantics_reference x "
                     "JOIN semantics_revision r ON x.source=r.kind||'.'||r.name||'@'||r.revision "
                     "JOIN semantics_object o ON o.kind=r.kind AND o.name=r.name "
                     "WHERE x.target=? AND (? OR o.current_revision=r.revision) ORDER BY x.source,x.field_path");
    if (!s) return StorageError(e);
    Bind(s.get(), 1, target);
    sqlite3_bind_int(s.get(), 2, history);
    int rc;
    while ((rc = sqlite3_step(s.get())) == SQLITE_ROW)
        out->push_back({Text(s.get(), 0), Text(s.get(), 1), sqlite3_column_int(s.get(), 2) != 0});
    return rc == SQLITE_DONE ? 0 : StorageError(e);
}
}  // namespace flowsql::channels::config
