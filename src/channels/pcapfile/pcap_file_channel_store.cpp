// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "pcap_file_channel_store.h"

#include <sqlite3.h>

#include <cerrno>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace flowsql::channels::pcapfile {
namespace {

constexpr int kBusyTimeoutMs = 5000;

constexpr char kSchemaSql[] =
    "CREATE TABLE IF NOT EXISTS pcapfile_channel_store ("
    "type TEXT NOT NULL CHECK(type = 'pcapfile'),"
    "name TEXT NOT NULL,"
    "option TEXT NOT NULL,"
    "updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"
    "PRIMARY KEY(type, name)"
    ");";

void ClearError(std::string* error) {
    if (error) error->clear();
}

int ReturnError(std::string* error, int code, std::string message) {
    if (error) *error = std::move(message);
    return code;
}

int MapSqliteError(int sqlite_code) {
    const int primary_code = sqlite_code & 0xff;
    if (primary_code == SQLITE_BUSY || primary_code == SQLITE_LOCKED) return EBUSY;
    return EIO;
}

std::string SqliteError(sqlite3* db, int sqlite_code) {
    const char* message = db ? sqlite3_errmsg(db) : sqlite3_errstr(sqlite_code);
    return message ? message : "unknown SQLite error";
}

int Prepare(sqlite3* db, const char* sql, sqlite3_stmt** statement,
            const char* operation, std::string* error) {
    const int rc = sqlite3_prepare_v2(db, sql, -1, statement, nullptr);
    if (rc == SQLITE_OK) return 0;
    return ReturnError(error, MapSqliteError(rc),
                       std::string(operation) + " failed: " + SqliteError(db, rc));
}

int BindText(sqlite3* db, sqlite3_stmt* statement, int index,
             const std::string& value, const char* operation,
             std::string* error) {
    const int rc = sqlite3_bind_text(statement, index, value.c_str(), -1,
                                     SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) return 0;
    return ReturnError(error, MapSqliteError(rc),
                       std::string(operation) + " failed to bind parameters: " +
                           SqliteError(db, rc));
}

int ConfigureWal(sqlite3* db, std::string* error) {
    sqlite3_stmt* statement = nullptr;
    int result = Prepare(db, "PRAGMA journal_mode=WAL;", &statement,
                         "enable pcapfile channel store WAL", error);
    if (result != 0) return result;

    const int step_rc = sqlite3_step(statement);
    if (step_rc != SQLITE_ROW) {
        const std::string detail = SqliteError(db, step_rc);
        sqlite3_finalize(statement);
        return ReturnError(error, MapSqliteError(step_rc),
                           "enable pcapfile channel store WAL failed: " + detail);
    }

    const auto* mode = sqlite3_column_text(statement, 0);
    const bool enabled = mode && sqlite3_stricmp(
                                      reinterpret_cast<const char*>(mode),
                                      "wal") == 0;
    const int finalize_rc = sqlite3_finalize(statement);
    if (!enabled) {
        return ReturnError(error, EIO,
                           "enable pcapfile channel store WAL failed: journal mode is not wal");
    }
    if (finalize_rc != SQLITE_OK) {
        return ReturnError(error, MapSqliteError(finalize_rc),
                           "enable pcapfile channel store WAL failed: " +
                               SqliteError(db, finalize_rc));
    }
    return 0;
}

int EnsureOpen(sqlite3* db, std::string* error) {
    if (db) return 0;
    return ReturnError(error, ENODEV, "pcapfile channel store is not open");
}

}  // namespace

PcapFileChannelStore::~PcapFileChannelStore() { Close(); }

int PcapFileChannelStore::Open(const std::string& db_path, std::string* error) {
    ClearError(error);
    if (db_path.empty()) {
        return ReturnError(error, EINVAL,
                           "pcapfile channel store db_path is empty");
    }
    if (db_) {
        return ReturnError(error, EALREADY,
                           "pcapfile channel store is already open");
    }

    const std::filesystem::path path(db_path);
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) {
        std::error_code filesystem_error;
        std::filesystem::create_directories(parent, filesystem_error);
        if (filesystem_error) {
            return ReturnError(error, EIO,
                               "create pcapfile channel store parent directory failed: " +
                                   filesystem_error.message());
        }
    }

    sqlite3* db = nullptr;
    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                      SQLITE_OPEN_FULLMUTEX;
    int rc = sqlite3_open_v2(db_path.c_str(), &db, flags, nullptr);
    if (rc != SQLITE_OK) {
        const std::string detail = SqliteError(db, rc);
        if (db) sqlite3_close(db);
        return ReturnError(error, MapSqliteError(rc),
                           "open pcapfile channel store failed: " + detail);
    }

    rc = sqlite3_busy_timeout(db, kBusyTimeoutMs);
    if (rc != SQLITE_OK) {
        const std::string detail = SqliteError(db, rc);
        sqlite3_close(db);
        return ReturnError(error, MapSqliteError(rc),
                           "set pcapfile channel store busy timeout failed: " +
                               detail);
    }

    int result = ConfigureWal(db, error);
    if (result != 0) {
        sqlite3_close(db);
        return result;
    }

    char* sqlite_error = nullptr;
    rc = sqlite3_exec(db, kSchemaSql, nullptr, nullptr, &sqlite_error);
    if (rc != SQLITE_OK) {
        const std::string detail = sqlite_error ? sqlite_error : SqliteError(db, rc);
        if (sqlite_error) sqlite3_free(sqlite_error);
        sqlite3_close(db);
        return ReturnError(error, MapSqliteError(rc),
                           "create pcapfile channel store schema failed: " +
                               detail);
    }

    db_ = db;
    return 0;
}

int PcapFileChannelStore::LoadAll(
    std::vector<PcapFileChannelRecord>* records, std::string* error) {
    ClearError(error);
    if (!records) {
        return ReturnError(error, EINVAL,
                           "load pcapfile channel records requires output");
    }
    int result = EnsureOpen(db_, error);
    if (result != 0) return result;

    constexpr char kSql[] =
        "SELECT type, name, option FROM pcapfile_channel_store "
        "ORDER BY type ASC, name ASC;";
    sqlite3_stmt* statement = nullptr;
    result = Prepare(db_, kSql, &statement, "load pcapfile channel records",
                     error);
    if (result != 0) return result;

    std::vector<PcapFileChannelRecord> loaded;
    int step_rc = SQLITE_OK;
    while ((step_rc = sqlite3_step(statement)) == SQLITE_ROW) {
        if (sqlite3_column_type(statement, 0) == SQLITE_NULL ||
            sqlite3_column_type(statement, 1) == SQLITE_NULL ||
            sqlite3_column_type(statement, 2) == SQLITE_NULL) {
            sqlite3_finalize(statement);
            return ReturnError(error, EIO,
                               "load pcapfile channel records failed: record contains null field");
        }

        const auto* type = sqlite3_column_text(statement, 0);
        const auto* name = sqlite3_column_text(statement, 1);
        const auto* option = sqlite3_column_text(statement, 2);
        if (!type || !name || !option) {
            sqlite3_finalize(statement);
            return ReturnError(error, EIO,
                               "load pcapfile channel records failed: cannot read text field");
        }
        loaded.push_back({
            std::string(reinterpret_cast<const char*>(type),
                        sqlite3_column_bytes(statement, 0)),
            std::string(reinterpret_cast<const char*>(name),
                        sqlite3_column_bytes(statement, 1)),
            std::string(reinterpret_cast<const char*>(option),
                        sqlite3_column_bytes(statement, 2)),
        });
    }

    if (step_rc != SQLITE_DONE) {
        const std::string detail = SqliteError(db_, step_rc);
        sqlite3_finalize(statement);
        return ReturnError(error, MapSqliteError(step_rc),
                           "load pcapfile channel records failed: " + detail);
    }
    const int finalize_rc = sqlite3_finalize(statement);
    if (finalize_rc != SQLITE_OK) {
        return ReturnError(error, MapSqliteError(finalize_rc),
                           "load pcapfile channel records failed: " +
                               SqliteError(db_, finalize_rc));
    }

    *records = std::move(loaded);
    return 0;
}

int PcapFileChannelStore::Insert(const PcapFileChannelRecord& record,
                                 std::string* error) {
    ClearError(error);
    int result = EnsureOpen(db_, error);
    if (result != 0) return result;

    constexpr char kSql[] =
        "INSERT INTO pcapfile_channel_store(type, name, option) "
        "VALUES(?1, ?2, ?3);";
    sqlite3_stmt* statement = nullptr;
    result = Prepare(db_, kSql, &statement, "insert pcapfile channel record",
                     error);
    if (result != 0) return result;

    result = BindText(db_, statement, 1, record.type,
                      "insert pcapfile channel record", error);
    if (result == 0) {
        result = BindText(db_, statement, 2, record.name,
                          "insert pcapfile channel record", error);
    }
    if (result == 0) {
        result = BindText(db_, statement, 3, record.option,
                          "insert pcapfile channel record", error);
    }
    if (result != 0) {
        sqlite3_finalize(statement);
        return result;
    }

    const int step_rc = sqlite3_step(statement);
    const int extended_rc = sqlite3_extended_errcode(db_);
    const std::string detail = SqliteError(db_, step_rc);
    sqlite3_finalize(statement);
    if (step_rc == SQLITE_DONE) return 0;
    if (extended_rc == SQLITE_CONSTRAINT_PRIMARYKEY ||
        extended_rc == SQLITE_CONSTRAINT_UNIQUE) {
        return ReturnError(error, EEXIST,
                           "insert pcapfile channel record failed: record already exists");
    }
    return ReturnError(error, MapSqliteError(step_rc),
                       "insert pcapfile channel record failed: " + detail);
}

int PcapFileChannelStore::Update(const PcapFileChannelRecord& record,
                                 std::string* error) {
    ClearError(error);
    int result = EnsureOpen(db_, error);
    if (result != 0) return result;

    constexpr char kSql[] =
        "UPDATE pcapfile_channel_store "
        "SET option=?1, updated_at=CURRENT_TIMESTAMP "
        "WHERE type=?2 AND name=?3;";
    sqlite3_stmt* statement = nullptr;
    result = Prepare(db_, kSql, &statement, "update pcapfile channel record",
                     error);
    if (result != 0) return result;

    result = BindText(db_, statement, 1, record.option,
                      "update pcapfile channel record", error);
    if (result == 0) {
        result = BindText(db_, statement, 2, record.type,
                          "update pcapfile channel record", error);
    }
    if (result == 0) {
        result = BindText(db_, statement, 3, record.name,
                          "update pcapfile channel record", error);
    }
    if (result != 0) {
        sqlite3_finalize(statement);
        return result;
    }

    const int step_rc = sqlite3_step(statement);
    const std::string detail = SqliteError(db_, step_rc);
    const int changed = sqlite3_changes(db_);
    sqlite3_finalize(statement);
    if (step_rc != SQLITE_DONE) {
        return ReturnError(error, MapSqliteError(step_rc),
                           "update pcapfile channel record failed: " + detail);
    }
    if (changed == 0) {
        return ReturnError(error, ENOENT,
                           "update pcapfile channel record failed: record does not exist");
    }
    return 0;
}

int PcapFileChannelStore::Erase(const std::string& type,
                                const std::string& name,
                                std::string* error) {
    ClearError(error);
    int result = EnsureOpen(db_, error);
    if (result != 0) return result;

    constexpr char kSql[] =
        "DELETE FROM pcapfile_channel_store WHERE type=?1 AND name=?2;";
    sqlite3_stmt* statement = nullptr;
    result = Prepare(db_, kSql, &statement, "erase pcapfile channel record",
                     error);
    if (result != 0) return result;

    result = BindText(db_, statement, 1, type,
                      "erase pcapfile channel record", error);
    if (result == 0) {
        result = BindText(db_, statement, 2, name,
                          "erase pcapfile channel record", error);
    }
    if (result != 0) {
        sqlite3_finalize(statement);
        return result;
    }

    const int step_rc = sqlite3_step(statement);
    const std::string detail = SqliteError(db_, step_rc);
    const int changed = sqlite3_changes(db_);
    sqlite3_finalize(statement);
    if (step_rc != SQLITE_DONE) {
        return ReturnError(error, MapSqliteError(step_rc),
                           "erase pcapfile channel record failed: " + detail);
    }
    if (changed == 0) {
        return ReturnError(error, ENOENT,
                           "erase pcapfile channel record failed: record does not exist");
    }
    return 0;
}

void PcapFileChannelStore::Close() {
    if (!db_) return;
    if (sqlite3_close(db_) == SQLITE_OK) db_ = nullptr;
}

}  // namespace flowsql::channels::pcapfile
