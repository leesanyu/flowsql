// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include "access_store.h"
#include "data_access_plugin.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace flowsql::access {
using namespace dataset;
namespace {
void Check(int code) {
    if (code != SQLITE_OK && code != SQLITE_DONE && code != SQLITE_ROW)
        throw AccessError("STATE_ERROR", "operation/result storage failed", 500);
}
void Exec(sqlite3* db, const char* sql) { Check(sqlite3_exec(db, sql, nullptr, nullptr, nullptr)); }
class Statement {
 public:
    Statement(sqlite3* db, const char* sql) { Check(sqlite3_prepare_v2(db, sql, -1, &statement_, nullptr)); }
    ~Statement() { sqlite3_finalize(statement_); }
    Statement& Bind(int position, const std::string& value) {
        Check(sqlite3_bind_text(statement_, position, value.data(), value.size(), SQLITE_TRANSIENT));
        return *this;
    }
    Statement& Bind(int position, int64_t value) {
        Check(sqlite3_bind_int64(statement_, position, value));
        return *this;
    }
    bool Step() {
        const int code = sqlite3_step(statement_);
        Check(code);
        return code == SQLITE_ROW;
    }
    std::string Text(int column) const {
        const auto* text = sqlite3_column_text(statement_, column);
        return text ? std::string(reinterpret_cast<const char*>(text), sqlite3_column_bytes(statement_, column)) : "";
    }
    int64_t Integer(int column) const { return sqlite3_column_int64(statement_, column); }

 private:
    sqlite3_stmt* statement_ = nullptr;
};
Json Stored(const std::string& text) {
    Json result;
    result.Parse(text.data(), text.size());
    if (result.HasParseError() || !result.IsObject())
        throw AccessError("STATE_CORRUPT", "stored operation/result is invalid", 500);
    return result;
}
std::string IntegrityHeader(const Value& payload) {
    Json header;
    header.CopyFrom(payload, header.GetAllocator());
    header.RemoveMember("rows");
    return Canonical(header);
}
std::string ContentDigest(const Value& payload) {
    std::string content = IntegrityHeader(payload) + "\n";
    for (const auto& row : payload["rows"].GetArray()) content += Canonical(row) + "\n";
    return Hash(content);
}
class Transaction {
 public:
    explicit Transaction(sqlite3* db) : db_(db) { Exec(db_, "BEGIN IMMEDIATE"); }
    ~Transaction() {
        if (!committed_) sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    }
    void Commit() {
        Exec(db_, "COMMIT");
        committed_ = true;
    }

 private:
    sqlite3* db_;
    bool committed_ = false;
};
}  // namespace
AccessStore::AccessStore(const std::string& path) {
    lock_ = open((path + ".lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock_ < 0 || flock(lock_, LOCK_EX | LOCK_NB)) {
        if (lock_ >= 0) close(lock_);
        throw AccessError("STATE_LOCKED", "state path is unavailable or used by another service", 500);
    }
    const int file = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (file < 0) {
        close(lock_);
        throw AccessError("STATE_ERROR", "cannot open state file", 500);
    }
    fchmod(file, 0600);
    close(file);
    try {
        Check(sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, nullptr));
        sqlite3_busy_timeout(db_, 5000);
        Exec(db_,
             "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA foreign_keys=ON;"
             "CREATE TABLE IF NOT EXISTS operations(id TEXT PRIMARY KEY,owner TEXT NOT NULL,request TEXT NOT NULL,"
             "digest TEXT NOT NULL,document TEXT NOT NULL,UNIQUE(owner,request));"
             "CREATE TABLE IF NOT EXISTS results(id TEXT PRIMARY KEY,owner TEXT NOT NULL,expires INTEGER NOT NULL,"
             "digest TEXT NOT NULL,schema_digest TEXT NOT NULL,rows INTEGER NOT NULL,bytes INTEGER NOT NULL,content "
             "TEXT);"
             "CREATE TABLE IF NOT EXISTS leases(id TEXT PRIMARY KEY,owner TEXT NOT NULL,result TEXT NOT NULL,"
             "expires INTEGER NOT NULL,FOREIGN KEY(result) REFERENCES results(id));");
        Statement rows(db_, "SELECT owner,document FROM operations");
        while (rows.Step()) {
            auto record = Stored(rows.Text(1));
            const auto state = Get(record, "status");
            if (state == "queued" || state == "running" || state == "cancelling") {
                Set(record, "status", Get(record, "kind") == "query" ? "interrupted" : "reconcile_required",
                    record.GetAllocator());
                Set(record, "error_stage", "service_restart", record.GetAllocator());
                UpdateLocked(rows.Text(0), record);
            }
        }
        Cleanup();
    } catch (...) {
        if (db_) sqlite3_close(db_);
        close(lock_);
        throw;
    }
}
AccessStore::~AccessStore() {
    sqlite3_close(db_);
    close(lock_);
}
void AccessStore::Cleanup() {
    Statement leases(db_, "DELETE FROM leases WHERE expires<=?");
    leases.Bind(1, Now()).Step();
    Statement results(db_,
                      "UPDATE results SET content=NULL WHERE content IS NOT NULL AND expires<=? "
                      "AND NOT EXISTS(SELECT 1 FROM leases WHERE leases.result=results.id)");
    results.Bind(1, Now()).Step();
}
Json AccessStore::FindLocked(const std::string& owner, const std::string& id, const std::string& request) {
    Statement row(db_, id.empty() ? "SELECT document FROM operations WHERE owner=? AND request=?"
                                  : "SELECT document FROM operations WHERE owner=? AND id=?");
    row.Bind(1, owner).Bind(2, id.empty() ? request : id);
    if (!row.Step()) throw AccessError("NOT_FOUND", "operation is not available to this subject", 404);
    return Stored(row.Text(0));
}
Json AccessStore::Find(const std::string& owner, const std::string& id, const std::string& request) {
    std::lock_guard<std::mutex> lock(mutex_);
    return FindLocked(owner, id, request);
}
Json AccessStore::Register(const std::string& owner, const std::string& request, const std::string& digest,
                           const std::string& kind, bool* created) {
    std::lock_guard<std::mutex> lock(mutex_);
    Statement existing(db_, "SELECT digest,document FROM operations WHERE owner=? AND request=?");
    existing.Bind(1, owner).Bind(2, request);
    if (existing.Step()) {
        if (existing.Text(0) != digest)
            throw AccessError("REQUEST_CONFLICT", "request identity has different arguments", 409);
        *created = false;
        return Stored(existing.Text(1));
    }
    Statement count(db_, "SELECT count(*) FROM operations");
    count.Step();
    if (count.Integer(0) >= 10000) throw AccessError("STATE_LIMIT", "operation ledger capacity reached", 429);
    Json record(rapidjson::kObjectType);
    auto& a = record.GetAllocator();
    const auto id = Token();
    Set(record, "operation_id", id, a);
    Set(record, "request_id", request, a);
    Set(record, "arguments_sha256", digest, a);
    Set(record, "kind", kind, a);
    Set(record, "status", "queued", a);
    record.AddMember("created_at_ms", Now(), a);
    Statement insert(db_, "INSERT INTO operations VALUES(?,?,?,?,?)");
    insert.Bind(1, id).Bind(2, owner).Bind(3, request).Bind(4, digest).Bind(5, Encode(record)).Step();
    *created = true;
    return record;
}
void AccessStore::UpdateLocked(const std::string& owner, const Value& record) {
    Statement update(db_, "UPDATE operations SET document=? WHERE owner=? AND id=?");
    update.Bind(1, Encode(record)).Bind(2, owner).Bind(3, Get(record, "operation_id")).Step();
    if (sqlite3_changes(db_) != 1) throw AccessError("NOT_FOUND", "operation is not available to this subject", 404);
}
void AccessStore::Update(const std::string& owner, const Value& record) {
    std::lock_guard<std::mutex> lock(mutex_);
    UpdateLocked(owner, record);
}
Json AccessStore::Outputs(const std::string& owner) {
    std::lock_guard<std::mutex> lock(mutex_);
    Json output(rapidjson::kArrayType);
    Statement rows(db_, "SELECT document FROM operations WHERE owner=?");
    rows.Bind(1, owner);
    while (rows.Step()) {
        const auto record = Stored(rows.Text(0));
        if (Get(record, "status") == "completed" && record.HasMember("derived_sources"))
            for (const auto& item : record["derived_sources"].GetArray())
                output.PushBack(Copy(item, output.GetAllocator()), output.GetAllocator());
    }
    return output;
}
Json AccessStore::Complete(const std::string& owner, const std::string& id, const Value& payload, uint64_t ttl_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    Cleanup();
    auto record = FindLocked(owner, id, "");
    if (!Get(record, "result_ref").empty()) return record;
    const auto content = Canonical(payload), result = Token();
    Statement size(db_, "SELECT coalesce(sum(length(content)),0) FROM results WHERE owner=?");
    size.Bind(1, owner).Step();
    if (content.size() + size.Integer(0) > 128 * 1024 * 1024)
        throw AccessError("RESULT_LIMIT", "subject result storage budget exceeded", 429);
    Transaction transaction(db_);
    Statement insert(db_, "INSERT INTO results VALUES(?,?,?,?,?,?,?,?)");
    insert.Bind(1, result)
        .Bind(2, owner)
        .Bind(3, Now() + ttl_ms)
        .Bind(4, ContentDigest(payload))
        .Bind(5, Hash(Canonical(payload["fields"])))
        .Bind(6, payload["rows"].Size())
        .Bind(7, content.size())
        .Bind(8, content)
        .Step();
    Set(record, "result_ref", result, record.GetAllocator());
    Set(record, "status", "completed", record.GetAllocator());
    record.AddMember("finished_at_ms", Now(), record.GetAllocator());
    UpdateLocked(owner, record);
    transaction.Commit();
    return record;
}
Json AccessStore::Read(const std::string& owner, const std::string& result, uint64_t offset, uint32_t limit,
                       const std::string& lease) {
    std::lock_guard<std::mutex> lock(mutex_);
    Cleanup();
    Statement row(db_, "SELECT expires,digest,schema_digest,rows,bytes,content FROM results WHERE owner=? AND id=?");
    row.Bind(1, owner).Bind(2, result);
    if (!row.Step()) throw AccessError("NOT_FOUND", "result is not available to this subject", 404);
    bool held = false;
    if (!lease.empty()) {
        Statement claim(db_, "SELECT 1 FROM leases WHERE id=? AND owner=? AND result=? AND expires>?");
        held = claim.Bind(1, lease).Bind(2, owner).Bind(3, result).Bind(4, Now()).Step();
    }
    if (!lease.empty() && !held)
        throw AccessError("LEASE_UNAVAILABLE", "read lease is expired or unavailable to this subject", 410);
    if ((row.Integer(0) <= Now() && !held) || row.Text(5).empty())
        throw AccessError("RESULT_EXPIRED", "result or supplied lease has expired", 410);
    auto output = Stored(row.Text(5));
    auto& a = output.GetAllocator();
    Value page(rapidjson::kArrayType);
    Value row_json(rapidjson::kArrayType);
    const auto total = output["rows"].Size();
    for (uint64_t i = std::min<uint64_t>(offset, total); i < total && i - offset < limit; ++i) {
        page.PushBack(Copy(output["rows"][i], a), a);
        row_json.PushBack(Value(Canonical(output["rows"][i]).c_str(), a), a);
    }
    const auto header = IntegrityHeader(output);
    output["rows"] = std::move(page);
    output.AddMember("row_json", row_json, a);
    Set(output, "integrity_header", header, a);
    Set(output, "digest_encoding", "UTF-8(header + LF + each row_json + LF)", a);
    Set(output, "result_ref", result, a);
    Set(output, "content_sha256", row.Text(1), a);
    Set(output, "schema_sha256", row.Text(2), a);
    output.AddMember("expires_at_ms", row.Integer(0), a);
    output.AddMember("total_rows", uint64_t(row.Integer(3)), a);
    output.AddMember("byte_count", uint64_t(row.Integer(4)), a);
    output.AddMember("offset", offset, a);
    output.AddMember("has_more", offset < total && limit < total - offset, a);
    if (Encode(output).size() > 1024 * 1024)
        throw AccessError("RESULT_PAGE_LIMIT", "result page exceeds 1 MiB; request fewer rows", 413);
    return output;
}
Json AccessStore::AcquireLease(const std::string& owner, const std::string& result, uint64_t duration_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    Cleanup();
    Statement row(db_, "SELECT expires,content IS NOT NULL FROM results WHERE owner=? AND id=?");
    row.Bind(1, owner).Bind(2, result);
    if (!row.Step()) throw AccessError("NOT_FOUND", "result is not available to this subject", 404);
    if (row.Integer(0) <= Now() || !row.Integer(1)) throw AccessError("RESULT_EXPIRED", "result has expired", 410);
    Statement count(db_, "SELECT count(*) FROM leases WHERE owner=?");
    count.Bind(1, owner).Step();
    if (count.Integer(0) >= 64) throw AccessError("LEASE_LIMIT", "too many active read leases", 429);
    const auto id = Token();
    const auto expires = Now() + std::min<uint64_t>(duration_ms, 300000);
    Statement insert(db_, "INSERT INTO leases VALUES(?,?,?,?)");
    insert.Bind(1, id).Bind(2, owner).Bind(3, result).Bind(4, expires).Step();
    Json output(rapidjson::kObjectType);
    Set(output, "lease_id", id, output.GetAllocator());
    Set(output, "result_ref", result, output.GetAllocator());
    output.AddMember("expires_at_ms", expires, output.GetAllocator());
    return output;
}
Json AccessStore::ReleaseLease(const std::string& owner, const std::string& lease) {
    std::lock_guard<std::mutex> lock(mutex_);
    Statement claim(db_, "SELECT 1 FROM leases WHERE owner=? AND id=?");
    if (!claim.Bind(1, owner).Bind(2, lease).Step())
        throw AccessError("NOT_FOUND", "lease is unavailable to this subject", 404);
    Statement remove(db_, "DELETE FROM leases WHERE owner=? AND id=?");
    remove.Bind(1, owner).Bind(2, lease).Step();
    Cleanup();
    Json output(rapidjson::kObjectType);
    output.AddMember("released", true, output.GetAllocator());
    return output;
}
}  // namespace flowsql::access
