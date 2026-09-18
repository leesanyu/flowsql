// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "config_channel_provider.h"

#include "config_content_validator.h"

#include <openssl/sha.h>
#include <sqlite3.h>

#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>

namespace flowsql::channels::config {
namespace {

constexpr size_t kMaxContentBytes = 512 * 1024;
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

int Fail(std::string* error, int code, const std::string& message) {
    if (error) *error = message;
    return code;
}

int SqlFail(sqlite3* db, std::string* error, const char* operation) {
    return Fail(error, EIO, std::string(operation) + ": " + sqlite3_errmsg(db));
}

bool ValidName(const std::string& name) {
    if (name.empty() || name.size() > 64 || name[0] < 'a' || name[0] > 'z') return false;
    for (char ch : name) {
        if ((ch < 'a' || ch > 'z') && (ch < '0' || ch > '9') && ch != '_' && ch != '-') return false;
    }
    return true;
}

bool ValidUtf8(const std::string& value) {
    for (size_t i = 0; i < value.size();) {
        const auto first = static_cast<unsigned char>(value[i]);
        if (first < 0x80) {
            ++i;
            continue;
        }
        const size_t width = first >= 0xc2 && first <= 0xdf ? 2 :
                             first >= 0xe0 && first <= 0xef ? 3 :
                             first >= 0xf0 && first <= 0xf4 ? 4 : 0;
        if (width == 0 || width > value.size() - i) return false;
        const auto second = static_cast<unsigned char>(value[i + 1]);
        if (second < 0x80 || second > 0xbf ||
            (first == 0xe0 && second < 0xa0) || (first == 0xed && second > 0x9f) ||
            (first == 0xf0 && second < 0x90) || (first == 0xf4 && second > 0x8f)) return false;
        for (size_t j = 2; j < width; ++j) {
            const auto next = static_cast<unsigned char>(value[i + j]);
            if (next < 0x80 || next > 0xbf) return false;
        }
        i += width;
    }
    return true;
}

int Validate(const ConfigPublishRequest& request, std::string* error) {
    if (!ValidName(request.name) ||
        (request.format != "json" && request.format != "yaml" && request.format != "xml") ||
        request.schema_id.empty() || request.schema_id.size() > 255 ||
        !ValidUtf8(request.schema_id) || request.original_filename.size() > 255 ||
        !ValidUtf8(request.original_filename) || request.change_note.size() > 1024 ||
        !ValidUtf8(request.change_note) || request.content.empty() || !ValidUtf8(request.content)) {
        return Fail(error, EINVAL, "invalid configuration metadata or UTF-8 content");
    }
    if (request.content.size() > kMaxContentBytes) return Fail(error, EFBIG, "configuration content exceeds 512 KiB");
    if (request.expected_current_revision > INT64_MAX || request.base_revision > INT64_MAX) {
        return Fail(error, EINVAL, "revision exceeds SQLite integer range");
    }
    return 0;
}

std::string Hex(const unsigned char* data, size_t size) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        result.push_back(digits[data[i] >> 4]);
        result.push_back(digits[data[i] & 15]);
    }
    return result;
}

Statement Prepare(sqlite3* db, const char* sql) {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &raw, nullptr) != SQLITE_OK) return Statement(nullptr, sqlite3_finalize);
    return Statement(raw, sqlite3_finalize);
}

int Exec(sqlite3* db, const char* sql, std::string* error) {
    if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK) return 0;
    return SqlFail(db, error, sql);
}

void BindText(sqlite3_stmt* statement, int index, const std::string& value) {
    sqlite3_bind_text(statement, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
}

std::string Text(sqlite3_stmt* statement, int index) {
    const auto* data = sqlite3_column_text(statement, index);
    return data ? std::string(reinterpret_cast<const char*>(data), sqlite3_column_bytes(statement, index)) : "";
}

struct Rollback {
    sqlite3* db;
    ~Rollback() { if (db) sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr); }
};

int64_t UnixMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

}  // namespace

ConfigChannelProvider::~ConfigChannelProvider() { Close(); }

int ConfigChannelProvider::Open(const std::string& db_path, std::string* error) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (error) error->clear();
    if (db_) return Fail(error, EALREADY, "configuration database is already open");
    if (db_path.empty() || db_path == ":memory:") return Fail(error, EINVAL, "persistent db_path required");
    std::error_code fs_error;
    const auto parent = std::filesystem::path(db_path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, fs_error);
    if (fs_error) return Fail(error, EIO, "create configuration database directory failed: " + fs_error.message());
    sqlite3* db = nullptr;
    const int rc = sqlite3_open_v2(db_path.c_str(), &db,
                                    SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (rc != SQLITE_OK) {
        const std::string message = db ? sqlite3_errmsg(db) : sqlite3_errstr(rc);
        if (db) sqlite3_close(db);
        return Fail(error, EIO, "open configuration database failed: " + message);
    }
    const char* schema =
        "CREATE TABLE IF NOT EXISTS config_channel ("
        "name TEXT PRIMARY KEY, current_revision INTEGER NOT NULL,"
        "created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS config_channel_revision ("
        "name TEXT NOT NULL, revision INTEGER NOT NULL, format TEXT NOT NULL,"
        "schema_id TEXT NOT NULL, content BLOB NOT NULL, content_bytes INTEGER NOT NULL,"
        "content_sha256 BLOB NOT NULL, base_revision INTEGER, original_filename TEXT,"
        "change_note TEXT, created_at INTEGER NOT NULL, PRIMARY KEY(name, revision));";
    if (sqlite3_busy_timeout(db, 5000) != SQLITE_OK || Exec(db, schema, error) != 0) {
        if (error && error->empty()) *error = sqlite3_errmsg(db);
        sqlite3_close(db);
        return EIO;
    }
    auto check = Prepare(db, "SELECT c.name FROM config_channel c LEFT JOIN config_channel_revision r "
                              "ON c.name = r.name AND c.current_revision = r.revision "
                              "WHERE r.revision IS NULL LIMIT 1");
    if (!check) {
        const int result = SqlFail(db, error, "read configuration database");
        sqlite3_close(db);
        return result;
    }
    const int step = sqlite3_step(check.get());
    if (step != SQLITE_DONE) {
        const int result = step == SQLITE_ROW ? Fail(error, EIO, "current revision is missing") :
                                                SqlFail(db, error, "read configuration database");
        check.reset();
        sqlite3_close(db);
        return result;
    }
    check.reset();
    db_ = db;
    return 0;
}

void ConfigChannelProvider::Close() {
    std::lock_guard<std::mutex> guard(mutex_);
    if (db_) sqlite3_close(db_);
    db_ = nullptr;
}

int ConfigChannelProvider::Publish(const ConfigPublishRequest& request, ConfigPublishResult* result,
                                   std::string* error) {
    if (error) error->clear();
    if (!result) return Fail(error, EINVAL, "publish result is required");
    *result = {};
    int rc = Validate(request, error);
    if (rc != 0) return rc;
    rc = ValidateConfigContent(request.format, request.content, error);
    if (rc != 0) return rc;
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    SHA256(reinterpret_cast<const unsigned char*>(request.content.data()), request.content.size(), digest.data());
    const std::string sha_hex = Hex(digest.data(), digest.size());

    std::lock_guard<std::mutex> guard(mutex_);
    if (!db_) return Fail(error, ENODEV, "configuration database is not open");
    if ((rc = Exec(db_, "BEGIN IMMEDIATE", error)) != 0) return rc;
    Rollback rollback{db_};
    uint64_t current = 0;
    {
        auto query = Prepare(db_, "SELECT current_revision FROM config_channel WHERE name = ?");
        if (!query) return SqlFail(db_, error, "prepare current revision");
        BindText(query.get(), 1, request.name);
        const int step = sqlite3_step(query.get());
        if (step == SQLITE_ROW) {
            const int64_t value = sqlite3_column_int64(query.get(), 0);
            if (value < 1) return Fail(error, EIO, "invalid current revision");
            current = static_cast<uint64_t>(value);
        } else if (step != SQLITE_DONE) return SqlFail(db_, error, "read current revision");
    }
    if (current) {
        auto query = Prepare(db_, "SELECT format, schema_id, content_sha256, content FROM "
                                  "config_channel_revision WHERE name = ? AND revision = ?");
        if (!query) return SqlFail(db_, error, "prepare current snapshot");
        BindText(query.get(), 1, request.name);
        sqlite3_bind_int64(query.get(), 2, static_cast<sqlite3_int64>(current));
        const int step = sqlite3_step(query.get());
        if (step != SQLITE_ROW) return Fail(error, EIO, "current snapshot is missing");
        const bool same = Text(query.get(), 0) == request.format && Text(query.get(), 1) == request.schema_id &&
            sqlite3_column_bytes(query.get(), 2) == static_cast<int>(digest.size()) &&
            std::memcmp(sqlite3_column_blob(query.get(), 2), digest.data(), digest.size()) == 0 &&
            sqlite3_column_bytes(query.get(), 3) == static_cast<int>(request.content.size()) &&
            std::memcmp(sqlite3_column_blob(query.get(), 3), request.content.data(), request.content.size()) == 0;
        if (same) {
            query.reset();
            if ((rc = Exec(db_, "COMMIT", error)) != 0) return rc;
            rollback.db = nullptr;
            *result = {current, false, sha_hex};
            return 0;
        }
    }
    if (current != request.expected_current_revision) return Fail(error, EAGAIN, "current revision conflict");
    if (current >= static_cast<uint64_t>(INT64_MAX)) return Fail(error, EIO, "revision overflow");
    if (request.base_revision) {
        auto base = Prepare(db_, "SELECT 1 FROM config_channel_revision WHERE name = ? AND revision = ?");
        if (!base) return SqlFail(db_, error, "prepare base revision");
        BindText(base.get(), 1, request.name);
        sqlite3_bind_int64(base.get(), 2, static_cast<sqlite3_int64>(request.base_revision));
        const int step = sqlite3_step(base.get());
        if (step != SQLITE_ROW) return step == SQLITE_DONE ? Fail(error, ENOENT, "base revision not found") :
                                                    SqlFail(db_, error, "read base revision");
    }
    const int64_t now = UnixMillis();
    if (!current) {
        auto create = Prepare(db_, "INSERT INTO config_channel(name,current_revision,created_at,updated_at) "
                                   "VALUES(?,1,?,?)");
        if (!create) return SqlFail(db_, error, "prepare channel insert");
        BindText(create.get(), 1, request.name);
        sqlite3_bind_int64(create.get(), 2, now);
        sqlite3_bind_int64(create.get(), 3, now);
        if (sqlite3_step(create.get()) != SQLITE_DONE) return SqlFail(db_, error, "insert channel");
    }
    const uint64_t next = current + 1;
    {
        auto insert = Prepare(db_, "INSERT INTO config_channel_revision (name,revision,format,schema_id,content,"
                                   "content_bytes,content_sha256,base_revision,"
                                   "original_filename,change_note,created_at) "
                                   "VALUES(?,?,?,?,?,?,?,?,?,?,?)");
        if (!insert) return SqlFail(db_, error, "prepare snapshot insert");
        BindText(insert.get(), 1, request.name);
        sqlite3_bind_int64(insert.get(), 2, static_cast<sqlite3_int64>(next));
        BindText(insert.get(), 3, request.format);
        BindText(insert.get(), 4, request.schema_id);
        sqlite3_bind_blob(insert.get(), 5, request.content.data(),
                          static_cast<int>(request.content.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int64(insert.get(), 6, static_cast<sqlite3_int64>(request.content.size()));
        sqlite3_bind_blob(insert.get(), 7, digest.data(), static_cast<int>(digest.size()), SQLITE_TRANSIENT);
        if (request.base_revision) {
            sqlite3_bind_int64(insert.get(), 8, static_cast<sqlite3_int64>(request.base_revision));
        }
        if (!request.original_filename.empty()) BindText(insert.get(), 9, request.original_filename);
        if (!request.change_note.empty()) BindText(insert.get(), 10, request.change_note);
        sqlite3_bind_int64(insert.get(), 11, now);
        if (sqlite3_step(insert.get()) != SQLITE_DONE) return SqlFail(db_, error, "insert snapshot");
    }
    if (current) {
        auto update = Prepare(db_, "UPDATE config_channel SET current_revision = ?, updated_at = ? WHERE name = ?");
        if (!update) return SqlFail(db_, error, "prepare current update");
        sqlite3_bind_int64(update.get(), 1, static_cast<sqlite3_int64>(next));
        sqlite3_bind_int64(update.get(), 2, now);
        BindText(update.get(), 3, request.name);
        if (sqlite3_step(update.get()) != SQLITE_DONE) return SqlFail(db_, error, "update current revision");
    }
    if ((rc = Exec(db_, "COMMIT", error)) != 0) return rc;
    rollback.db = nullptr;
    *result = {next, true, sha_hex};
    return 0;
}

int ConfigChannelProvider::ListChannels(const std::string& cursor, uint32_t limit,
                                        std::vector<ConfigChannelListItem>* items,
                                        std::string* next_cursor, std::string* error) {
    if (error) error->clear();
    if (!items || !next_cursor || limit == 0 || limit > 100 ||
        (!cursor.empty() && !ValidName(cursor))) {
        return Fail(error, EINVAL, "invalid configuration channel list request");
    }
    const std::string after = cursor;
    items->clear();
    next_cursor->clear();
    std::lock_guard<std::mutex> guard(mutex_);
    if (!db_) return Fail(error, ENODEV, "configuration database is not open");
    auto query = Prepare(db_,
        "SELECT c.name,c.current_revision,r.format,r.schema_id,r.content_sha256,"
        "r.content_bytes,c.updated_at FROM config_channel c "
        "JOIN config_channel_revision r ON r.name=c.name AND r.revision=c.current_revision "
        "WHERE c.name>? ORDER BY c.name ASC LIMIT ?");
    if (!query) return SqlFail(db_, error, "prepare configuration channel list");
    BindText(query.get(), 1, after);
    sqlite3_bind_int(query.get(), 2, static_cast<int>(limit + 1));
    std::vector<ConfigChannelListItem> loaded;
    int step = SQLITE_OK;
    while ((step = sqlite3_step(query.get())) == SQLITE_ROW) {
        const int digest_bytes = sqlite3_column_bytes(query.get(), 4);
        const int64_t revision = sqlite3_column_int64(query.get(), 1);
        const int64_t content_bytes = sqlite3_column_int64(query.get(), 5);
        if (digest_bytes != SHA256_DIGEST_LENGTH || revision < 1 || content_bytes < 1 ||
            content_bytes > static_cast<int64_t>(kMaxContentBytes)) {
            return Fail(error, EIO, "invalid persisted configuration channel metadata");
        }
        const auto* digest = static_cast<const unsigned char*>(sqlite3_column_blob(query.get(), 4));
        ConfigChannelListItem item;
        item.name = Text(query.get(), 0);
        item.current_revision = static_cast<uint64_t>(revision);
        item.format = Text(query.get(), 2);
        item.schema_id = Text(query.get(), 3);
        item.sha256_hex = Hex(digest, digest_bytes);
        item.content_bytes = static_cast<uint64_t>(content_bytes);
        item.updated_at_unix_ms = sqlite3_column_int64(query.get(), 6);
        loaded.push_back(std::move(item));
    }
    if (step != SQLITE_DONE) return SqlFail(db_, error, "list configuration channels");
    if (loaded.size() > limit) {
        loaded.resize(limit);
        *next_cursor = loaded.back().name;
    }
    *items = std::move(loaded);
    return 0;
}

int ConfigChannelProvider::ListHistory(const std::string& name, uint64_t before_revision,
                                       uint32_t limit,
                                       std::vector<ConfigRevisionListItem>* items,
                                       uint64_t* next_cursor, std::string* error) {
    if (error) error->clear();
    if (!items || !next_cursor || !ValidName(name) || limit == 0 || limit > 100 ||
        before_revision > INT64_MAX) {
        return Fail(error, EINVAL, "invalid configuration history request");
    }
    items->clear();
    *next_cursor = 0;
    std::lock_guard<std::mutex> guard(mutex_);
    if (!db_) return Fail(error, ENODEV, "configuration database is not open");
    {
        auto exists = Prepare(db_, "SELECT 1 FROM config_channel WHERE name=?");
        if (!exists) return SqlFail(db_, error, "prepare configuration channel lookup");
        BindText(exists.get(), 1, name);
        const int step = sqlite3_step(exists.get());
        if (step == SQLITE_DONE) return Fail(error, ENOENT, "configuration channel not found");
        if (step != SQLITE_ROW) return SqlFail(db_, error, "read configuration channel");
    }
    auto query = Prepare(db_,
        "SELECT revision,format,schema_id,content_sha256,content_bytes,created_at,"
        "base_revision,original_filename,change_note FROM config_channel_revision "
        "WHERE name=? AND (?=0 OR revision<?) ORDER BY revision DESC LIMIT ?");
    if (!query) return SqlFail(db_, error, "prepare configuration history");
    BindText(query.get(), 1, name);
    sqlite3_bind_int64(query.get(), 2, static_cast<sqlite3_int64>(before_revision));
    sqlite3_bind_int64(query.get(), 3, static_cast<sqlite3_int64>(before_revision));
    sqlite3_bind_int(query.get(), 4, static_cast<int>(limit + 1));
    std::vector<ConfigRevisionListItem> loaded;
    int step = SQLITE_OK;
    while ((step = sqlite3_step(query.get())) == SQLITE_ROW) {
        const int64_t revision = sqlite3_column_int64(query.get(), 0);
        const int digest_bytes = sqlite3_column_bytes(query.get(), 3);
        const int64_t content_bytes = sqlite3_column_int64(query.get(), 4);
        const int64_t base_revision = sqlite3_column_type(query.get(), 6) == SQLITE_NULL
                                          ? 0 : sqlite3_column_int64(query.get(), 6);
        if (revision < 1 || digest_bytes != SHA256_DIGEST_LENGTH || content_bytes < 1 ||
            content_bytes > static_cast<int64_t>(kMaxContentBytes) || base_revision < 0) {
            return Fail(error, EIO, "invalid persisted configuration revision metadata");
        }
        const auto* digest = static_cast<const unsigned char*>(sqlite3_column_blob(query.get(), 3));
        ConfigRevisionListItem item;
        item.revision = static_cast<uint64_t>(revision);
        item.format = Text(query.get(), 1);
        item.schema_id = Text(query.get(), 2);
        item.sha256_hex = Hex(digest, digest_bytes);
        item.content_bytes = static_cast<uint64_t>(content_bytes);
        item.created_at_unix_ms = sqlite3_column_int64(query.get(), 5);
        item.base_revision = static_cast<uint64_t>(base_revision);
        item.original_filename = Text(query.get(), 7);
        item.change_note = Text(query.get(), 8);
        loaded.push_back(std::move(item));
    }
    if (step != SQLITE_DONE) return SqlFail(db_, error, "list configuration history");
    if (loaded.size() > limit) {
        loaded.resize(limit);
        *next_cursor = loaded.back().revision;
    }
    *items = std::move(loaded);
    return 0;
}

int ConfigChannelProvider::Resolve(const char* exact_reference, ConfigChannelSnapshot* snapshot, std::string* error) {
    if (error) error->clear();
    if (!snapshot) return Fail(error, EINVAL, "snapshot output is required");
    *snapshot = {};
    if (!exact_reference) return Fail(error, EINVAL, "exact reference is required");
    const std::string reference(exact_reference);
    if (reference.compare(0, 7, "config.") != 0) return Fail(error, EINVAL, "expected config reference");
    const auto at = reference.find('@', 7);
    if (at == std::string::npos) return Fail(error, EINVAL, "exact revision required");
    const std::string name = reference.substr(7, at - 7);
    const std::string revision_text = reference.substr(at + 1);
    if (!ValidName(name) || revision_text.empty() || revision_text[0] == '0') {
        return Fail(error, EINVAL, "invalid exact reference");
    }
    uint64_t revision = 0;
    const auto parsed = std::from_chars(revision_text.data(), revision_text.data() + revision_text.size(), revision);
    if (parsed.ec != std::errc{} || parsed.ptr != revision_text.data() + revision_text.size() ||
        revision > INT64_MAX) return Fail(error, EINVAL, "invalid revision");

    std::lock_guard<std::mutex> guard(mutex_);
    if (!db_) return Fail(error, ENODEV, "configuration database is not open");
    auto query = Prepare(db_, "SELECT format,schema_id,content,content_bytes,content_sha256,created_at "
                              "FROM config_channel_revision WHERE name = ? AND revision = ?");
    if (!query) return SqlFail(db_, error, "prepare snapshot lookup");
    BindText(query.get(), 1, name);
    sqlite3_bind_int64(query.get(), 2, static_cast<sqlite3_int64>(revision));
    const int step = sqlite3_step(query.get());
    if (step == SQLITE_DONE) return Fail(error, ENOENT, "configuration snapshot not found");
    if (step != SQLITE_ROW) return SqlFail(db_, error, "read configuration snapshot");
    const int bytes = sqlite3_column_bytes(query.get(), 2);
    const int digest_bytes = sqlite3_column_bytes(query.get(), 4);
    if (bytes < 1 || bytes > static_cast<int>(kMaxContentBytes) ||
        digest_bytes != SHA256_DIGEST_LENGTH || sqlite3_column_int64(query.get(), 3) != bytes) {
        return Fail(error, EIO, "invalid persisted snapshot");
    }
    const auto* digest = static_cast<const unsigned char*>(sqlite3_column_blob(query.get(), 4));
    const auto* content = static_cast<const char*>(sqlite3_column_blob(query.get(), 2));
    ConfigChannelSnapshot resolved;
    resolved.channel_name = name;
    resolved.revision = revision;
    resolved.format = Text(query.get(), 0);
    resolved.schema_id = Text(query.get(), 1);
    resolved.sha256_hex = Hex(digest, digest_bytes);
    resolved.content_bytes = bytes;
    resolved.created_at_unix_ms = sqlite3_column_int64(query.get(), 5);
    resolved.content = std::make_shared<const std::string>(content, bytes);
    *snapshot = std::move(resolved);
    return 0;
}

}  // namespace flowsql::channels::config
