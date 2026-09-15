/*
 * Copyright (C) 2026 LIHUO
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 */

#include "binaddon_host_plugin.h"

#include <common/error_code.h>
#include <common/log.h>
#include <framework/interfaces/iblock_transform_operator.h>
#include <framework/interfaces/istream_operator.h>

#include <openssl/evp.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cctype>
#include <cstring>
#include <cstdio>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_set>
#include <utility>
#include <vector>

#include "binaddon_operator_proxy.h"

namespace fs = std::filesystem;

namespace flowsql {
namespace binaddon {

namespace {
bool EqualsIgnoreCase(const std::string& a, const char* b) {
    if (!b) return false;
    const size_t n = a.size();
    if (n != std::strlen(b)) return false;
    for (size_t i = 0; i < n; ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

bool SameGuid(const Guid& left, const Guid& right) { return std::memcmp(&left, &right, sizeof(Guid)) == 0; }

const char* ContractName(const Guid& iid) {
    if (SameGuid(iid, IID_OPERATOR)) return "operator_v1";
    if (SameGuid(iid, IID_BLOCK_TRANSFORM_OPERATOR_V1)) return "block_transform_v1";
    return "unknown";
}

std::string ToLowerAscii(std::string value) {
    for (char& ch : value) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return value;
}

bool IsSafeUploadFilename(const std::string& filename) {
    if (filename.empty()) return false;
    if (filename.find("..") != std::string::npos) return false;
    if (filename.find('/') != std::string::npos || filename.find('\\') != std::string::npos) return false;
    return true;
}

int MoveUploadedFile(const fs::path& src, const fs::path& dst) {
    std::error_code ec;
    fs::rename(src, dst, ec);
    if (!ec) return 0;
    ec.clear();
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) return -1;
    fs::remove(src, ec);
    return 0;
}

std::string Sha256File(const fs::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) return "";

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return "";
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
        EVP_MD_CTX_free(ctx);
        return "";
    }

    char buf[8192];
    while (ifs.good()) {
        ifs.read(buf, sizeof(buf));
        const std::streamsize n = ifs.gcount();
        if (n > 0 && EVP_DigestUpdate(ctx, buf, static_cast<size_t>(n)) != 1) {
            EVP_MD_CTX_free(ctx);
            return "";
        }
    }

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    if (EVP_DigestFinal_ex(ctx, digest, &digest_len) != 1) {
        EVP_MD_CTX_free(ctx);
        return "";
    }
    EVP_MD_CTX_free(ctx);

    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < digest_len; ++i) oss << std::setw(2) << static_cast<int>(digest[i]);
    return oss.str();
}

std::string JsonArrayFromStrings(const std::vector<std::string>& values) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartArray();
    for (const auto& v : values) w.String(v.c_str());
    w.EndArray();
    return buf.GetString();
}

std::vector<std::string> ParseStringArrayJson(const std::string& json) {
    std::vector<std::string> values;
    if (json.empty()) return values;
    rapidjson::Document doc;
    doc.Parse(json.c_str());
    if (doc.HasParseError() || !doc.IsArray()) return values;
    values.reserve(doc.Size());
    for (auto& it : doc.GetArray()) {
        if (it.IsString()) values.push_back(it.GetString());
    }
    return values;
}

struct CapabilityLeaseLifetime {
    explicit CapabilityLeaseLifetime(std::shared_ptr<BinAddonHostPlugin::LoadedPlugin> owner)
        : owner(std::move(owner)) {}

    ~CapabilityLeaseLifetime() {
        if (owner) owner->active_count.fetch_sub(1, std::memory_order_acq_rel);
    }

    std::shared_ptr<BinAddonHostPlugin::LoadedPlugin> owner;
};
}  // namespace

BinAddonHostPlugin::LoadedPlugin::~LoadedPlugin() {
    if (destroy_capability_fn) {
        for (auto it = capabilities.rbegin(); it != capabilities.rend(); ++it) {
            if (!it->instance) continue;
            try {
                destroy_capability_fn(it->index, it->instance);
            } catch (...) {
            }
            it->instance = nullptr;
        }
    }
    capabilities.clear();
    if (handle) {
        dlclose(handle);
        handle = nullptr;
    }
}

int BinAddonHostPlugin::Option(const char* arg) {
    if (!arg || !*arg) return 0;
    std::string opts(arg);
    size_t pos = 0;
    while (pos < opts.size()) {
        size_t eq = opts.find('=', pos);
        if (eq == std::string::npos) break;
        size_t end = opts.find(';', eq);
        if (end == std::string::npos) end = opts.size();

        std::string key = opts.substr(pos, eq - pos);
        std::string val = opts.substr(eq + 1, end - eq - 1);
        if (key == "operator_db_dir" && !val.empty()) operator_db_dir_ = val;
        if (key == "operator_db_path" && !val.empty()) operator_db_path_ = val;
        if (key == "upload_dir" && !val.empty()) upload_dir_ = val;

        pos = (end < opts.size()) ? end + 1 : opts.size();
    }
    return 0;
}

int BinAddonHostPlugin::Load(IQuerier* querier) {
    querier_ = querier;
    registry_ = querier_ ? static_cast<IOperatorRegistry*>(querier_->First(IID_OPERATOR_REGISTRY)) : nullptr;
    if (!registry_) {
        LOG_ERROR("BinAddonHostPlugin::Load: missing IOperatorRegistry");
        return -1;
    }
    return 0;
}

int BinAddonHostPlugin::Unload() {
    Stop();
    registry_ = nullptr;
    querier_ = nullptr;
    return 0;
}

int BinAddonHostPlugin::Start() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (EnsureOperatorDbLocked() != 0) return -1;
    }
    return RecoverActivatedPlugins();
}

int BinAddonHostPlugin::Stop() {
    std::unordered_map<std::string, std::shared_ptr<LoadedPlugin>> retired_plugins;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (registry_) {
            for (auto& kv : loaded_plugins_) {
                auto& loaded = kv.second;
                if (!loaded) continue;
                loaded->pending_unload.store(true, std::memory_order_release);
                for (const auto& key : loaded->operator_keys) {
                    (void)registry_->RemoveFactory(key.c_str());
                }
            }
        }
        retired_plugins.swap(loaded_plugins_);
        if (operator_db_ != nullptr) {
            sqlite3_close(operator_db_);
            operator_db_ = nullptr;
        }
    }
    retired_plugins.clear();
    return 0;
}

int BinAddonHostPlugin::Acquire(const char* category, const char* name, const Guid& contract_iid,
                                CppOperatorCapabilityLeaseV1* lease) {
    if (!lease) return -1;
    *lease = {};
    if (!category || !*category || !name || !*name) return -1;

    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& item : loaded_plugins_) {
        const auto& loaded = item.second;
        if (!loaded || loaded->pending_unload.load(std::memory_order_acquire)) continue;
        for (const auto& capability : loaded->capabilities) {
            if (capability.category != category || capability.name != name ||
                !SameGuid(capability.contract_iid, contract_iid)) {
                continue;
            }

            loaded->active_count.fetch_add(1, std::memory_order_acq_rel);
            try {
                lease->lifetime = std::make_shared<CapabilityLeaseLifetime>(loaded);
            } catch (...) {
                loaded->active_count.fetch_sub(1, std::memory_order_acq_rel);
                return -1;
            }
            lease->capability = capability.instance;
            return lease->capability && lease->lifetime ? 0 : -1;
        }
    }
    return -1;
}

int BinAddonHostPlugin::EnsureOperatorDbDir() const {
    try {
        fs::path path = operator_db_path_.empty() ? fs::path(operator_db_dir_) : fs::path(operator_db_path_).parent_path();
        if (path.empty()) return 0;
        std::error_code ec;
        fs::create_directories(path, ec);
        return ec ? -1 : 0;
    } catch (...) {
        return -1;
    }
}

std::string BinAddonHostPlugin::OperatorDbPath() const {
    if (!operator_db_path_.empty()) return operator_db_path_;
    fs::path p(operator_db_dir_);
    p /= "operator_catalog.db";
    return p.string();
}

int BinAddonHostPlugin::EnsureOperatorDbLocked() {
    if (operator_db_ != nullptr) return 0;
    if (EnsureOperatorDbDir() != 0) return -1;
    const std::string path = OperatorDbPath();
    if (sqlite3_open(path.c_str(), &operator_db_) != SQLITE_OK) {
        if (operator_db_) {
            sqlite3_close(operator_db_);
            operator_db_ = nullptr;
        }
        return -1;
    }
    if (sqlite3_exec(operator_db_, "PRAGMA journal_mode=WAL", nullptr, nullptr, nullptr) != SQLITE_OK) {
        sqlite3_close(operator_db_);
        operator_db_ = nullptr;
        return -1;
    }
    return EnsureSchemaLocked();
}

int BinAddonHostPlugin::EnsureSchemaLocked() {
    if (!operator_db_) return -1;
    const char* ddl_catalog =
        "CREATE TABLE IF NOT EXISTS operator_catalog ("
        "category TEXT NOT NULL,"
        "name TEXT NOT NULL,"
        "type TEXT NOT NULL,"
        "description TEXT NOT NULL DEFAULT '',"
        "position TEXT NOT NULL DEFAULT '',"
        "source TEXT NOT NULL,"
        "active INTEGER NOT NULL DEFAULT 0,"
        "editable INTEGER NOT NULL DEFAULT 1,"
        "content_ref TEXT NOT NULL DEFAULT '',"
        "plugin_id TEXT,"
        "created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        "updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        "UNIQUE(category COLLATE NOCASE, name COLLATE NOCASE)"
        ");";
    if (sqlite3_exec(operator_db_, ddl_catalog, nullptr, nullptr, nullptr) != SQLITE_OK) return -1;

    bool has_category = false;
    bool has_plugin_id = false;
    sqlite3_stmt* info = nullptr;
    if (sqlite3_prepare_v2(operator_db_, "PRAGMA table_info(operator_catalog);", -1, &info, nullptr) == SQLITE_OK) {
        while (sqlite3_step(info) == SQLITE_ROW) {
            const unsigned char* name = sqlite3_column_text(info, 1);
            const std::string col = name ? reinterpret_cast<const char*>(name) : "";
            if (EqualsIgnoreCase(col, "category")) has_category = true;
            if (EqualsIgnoreCase(col, "plugin_id")) {
                has_plugin_id = true;
            }
        }
        sqlite3_finalize(info);
    }
    if (!has_category) {
        if (sqlite3_exec(operator_db_, "ALTER TABLE operator_catalog ADD COLUMN category TEXT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
            return -1;
        }
    }
    if (!has_plugin_id) {
        if (sqlite3_exec(operator_db_, "ALTER TABLE operator_catalog ADD COLUMN plugin_id TEXT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
            return -1;
        }
    }
    if (sqlite3_exec(operator_db_,
                     "CREATE UNIQUE INDEX IF NOT EXISTS ux_operator_catalog_category_name "
                     "ON operator_catalog(category COLLATE NOCASE, name COLLATE NOCASE);",
                     nullptr, nullptr, nullptr) != SQLITE_OK) {
        return -1;
    }

    const char* ddl_plugin_store =
        "CREATE TABLE IF NOT EXISTS operator_plugin_store ("
        "plugin_id TEXT PRIMARY KEY,"
        "so_file TEXT NOT NULL,"
        "file_path TEXT NOT NULL,"
        "size_bytes INTEGER NOT NULL,"
        "sha256 TEXT NOT NULL UNIQUE,"
        "status TEXT NOT NULL,"
        "last_error TEXT NOT NULL DEFAULT '',"
        "abi_version INTEGER,"
        "operator_count INTEGER,"
        "operators_json TEXT NOT NULL DEFAULT '',"
        "created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        "updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP"
        ");";
    return sqlite3_exec(operator_db_, ddl_plugin_store, nullptr, nullptr, nullptr) == SQLITE_OK ? 0 : -1;
}

bool BinAddonHostPlugin::QueryPluginByIdLocked(const std::string& plugin_id, PluginStoreRow* row) {
    if (!row || !operator_db_) return false;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT plugin_id, so_file, file_path, size_bytes, sha256, status, last_error, "
        "abi_version, operator_count, operators_json "
        "FROM operator_plugin_store WHERE plugin_id=?1 LIMIT 1;";
    if (sqlite3_prepare_v2(operator_db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, plugin_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return false;
    }
    auto txt = [stmt](int idx) -> std::string {
        const unsigned char* v = sqlite3_column_text(stmt, idx);
        return v ? reinterpret_cast<const char*>(v) : "";
    };
    row->plugin_id = txt(0);
    row->so_file = txt(1);
    row->file_path = txt(2);
    row->size_bytes = sqlite3_column_int64(stmt, 3);
    row->sha256 = txt(4);
    row->status = txt(5);
    row->last_error = txt(6);
    row->abi_version = (sqlite3_column_type(stmt, 7) == SQLITE_NULL) ? -1 : sqlite3_column_int(stmt, 7);
    row->operator_count = (sqlite3_column_type(stmt, 8) == SQLITE_NULL) ? -1 : sqlite3_column_int(stmt, 8);
    row->operators_json = txt(9);
    sqlite3_finalize(stmt);
    return true;
}

int BinAddonHostPlugin::UpdatePluginStatusLocked(const std::string& plugin_id,
                                                 const std::string& status,
                                                 const std::string& last_error,
                                                 int abi_version,
                                                 int operator_count,
                                                 const std::string& operators_json) {
    if (!operator_db_) return -1;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "UPDATE operator_plugin_store SET "
        "status=?1, last_error=?2, "
        "abi_version=CASE WHEN ?3 < 0 THEN NULL ELSE ?3 END, "
        "operator_count=CASE WHEN ?4 < 0 THEN NULL ELSE ?4 END, "
        "operators_json=?5, updated_at=CURRENT_TIMESTAMP "
        "WHERE plugin_id=?6;";
    if (sqlite3_prepare_v2(operator_db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, last_error.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, abi_version);
    sqlite3_bind_int(stmt, 4, operator_count);
    sqlite3_bind_text(stmt, 5, operators_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, plugin_id.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(stmt);
    const int changed = sqlite3_changes(operator_db_);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE && changed > 0) ? 0 : -1;
}

int BinAddonHostPlugin::MarkPluginBrokenLocked(const std::string& plugin_id, const std::string& last_error,
                                               int abi_version) {
    if (!operator_db_) return -1;
    if (sqlite3_exec(operator_db_, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK) return -1;
    const int active_rc = SetCppOperatorsActiveByPluginLocked(plugin_id, 0);
    const int status_rc = UpdatePluginStatusLocked(plugin_id, "broken", last_error, abi_version, -1, "");
    if (active_rc != 0 || status_rc != 0 ||
        sqlite3_exec(operator_db_, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
        (void)sqlite3_exec(operator_db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return -1;
    }
    return 0;
}

int BinAddonHostPlugin::UpsertCppOperatorsLocked(const std::string& plugin_id, const std::vector<OperatorMeta>& operators) {
    if (!operator_db_) return -1;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO operator_catalog("
        "category, name, type, source, description, position, active, editable, plugin_id, created_at, updated_at"
        ") VALUES (?1, ?2, 'cpp', ?3, ?4, ?5, 1, 0, ?6, CURRENT_TIMESTAMP, CURRENT_TIMESTAMP) "
        "ON CONFLICT(category, name) DO UPDATE SET "
        "type='cpp', source=excluded.source, description=excluded.description, position=excluded.position, "
        "active=1, editable=0, plugin_id=excluded.plugin_id, updated_at=CURRENT_TIMESTAMP;";
    if (sqlite3_prepare_v2(operator_db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return -1;
    for (const auto& op : operators) {
        sqlite3_clear_bindings(stmt);
        sqlite3_reset(stmt);
        sqlite3_bind_text(stmt, 1, op.category.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, op.name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, op.source.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, op.description.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, op.position.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 6, plugin_id.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            sqlite3_finalize(stmt);
            return -1;
        }
    }
    sqlite3_finalize(stmt);
    return 0;
}

int BinAddonHostPlugin::SetCppOperatorsActiveByPluginLocked(const std::string& plugin_id, int active) {
    if (!operator_db_) return -1;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE operator_catalog SET active=?1, updated_at=CURRENT_TIMESTAMP WHERE plugin_id=?2;";
    if (sqlite3_prepare_v2(operator_db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return -1;
    sqlite3_bind_int(stmt, 1, active);
    sqlite3_bind_text(stmt, 2, plugin_id.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

int BinAddonHostPlugin::DeleteCppOperatorsByPluginLocked(const std::string& plugin_id) {
    if (!operator_db_) return -1;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "DELETE FROM operator_catalog WHERE plugin_id=?1;";
    if (sqlite3_prepare_v2(operator_db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, plugin_id.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

int BinAddonHostPlugin::ListCppPlugins(std::string& rsp) {
    std::lock_guard<std::mutex> lock(mu_);
    if (EnsureOperatorDbLocked() != 0 || operator_db_ == nullptr) {
        rsp = R"({"error":"operator catalog db is not initialized"})";
        return error::INTERNAL_ERROR;
    }

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT plugin_id, so_file, file_path, size_bytes, sha256, status, last_error, "
        "abi_version, operator_count, operators_json "
        "FROM operator_plugin_store ORDER BY updated_at DESC, plugin_id ASC;";
    if (sqlite3_prepare_v2(operator_db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        rsp = R"({"error":"failed to query cpp plugin store"})";
        return error::INTERNAL_ERROR;
    }

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("operators");
    w.StartArray();
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        auto txt = [stmt](int idx) -> const char* {
            const unsigned char* v = sqlite3_column_text(stmt, idx);
            return v ? reinterpret_cast<const char*>(v) : "";
        };
        const std::string status = txt(5);
        const bool activated = EqualsIgnoreCase(status, "activated");
        const bool has_abi = sqlite3_column_type(stmt, 7) != SQLITE_NULL;
        const bool has_op_count = sqlite3_column_type(stmt, 8) != SQLITE_NULL;
        const std::string operators_json = txt(9);
        const auto operators = ParseStringArrayJson(operators_json);

        w.StartObject();
        w.Key("type");
        w.String("cpp");
        w.Key("active");
        w.Int(activated ? 1 : 0);
        w.Key("plugin_id");
        w.String(txt(0));
        w.Key("plugin");
        w.StartObject();
        w.Key("so_file");
        w.String(txt(1));
        w.Key("size_bytes");
        w.Int64(sqlite3_column_int64(stmt, 3));
        w.Key("sha256");
        w.String(txt(4));
        w.Key("status");
        w.String(status.c_str());
        w.Key("abi_version");
        if (has_abi) w.Int(sqlite3_column_int(stmt, 7));
        else w.Null();
        w.Key("operator_count");
        if (has_op_count) w.Int(sqlite3_column_int(stmt, 8));
        else w.Null();
        w.Key("operators");
        if (!operators.empty()) {
            w.StartArray();
            for (const auto& op : operators) w.String(op.c_str());
            w.EndArray();
        } else {
            w.Null();
        }
        w.Key("last_error");
        w.String(txt(6));
        w.EndObject();
        w.EndObject();
    }
    sqlite3_finalize(stmt);
    w.EndArray();
    w.EndObject();
    rsp = buf.GetString();
    return error::OK;
}

int BinAddonHostPlugin::UploadCppPlugin(const std::string& filename, const std::string& tmp_path, std::string& rsp) {
    // 逻辑链：
    // 1) 校验上传文件名与临时文件有效性，搬运到上传目录；
    // 2) 计算插件 SHA256 并以 hash 作为 plugin_id；
    // 3) 在 sqlite 元数据库去重并写入 uploaded 元数据；
    // 4) 返回上传结果与插件基础信息。
    if (!IsSafeUploadFilename(filename)) {
        rsp = R"({"error":"invalid filename"})";
        return error::BAD_REQUEST;
    }
    const fs::path src(tmp_path);
    if (!fs::exists(src) || !fs::is_regular_file(src)) {
        rsp = R"({"error":"tmp_path not found"})";
        return error::BAD_REQUEST;
    }

    fs::path target_dir(upload_dir_);
    std::error_code ec;
    fs::create_directories(target_dir, ec);
    if (ec) {
        rsp = R"({"error":"failed to create target directory"})";
        return error::INTERNAL_ERROR;
    }

    const fs::path dst = target_dir / filename;
    if (fs::exists(dst)) {
        rsp = R"({"error":"file already exists, delete old plugin first"})";
        return error::CONFLICT;
    }
    if (MoveUploadedFile(src, dst) != 0) {
        rsp = R"({"error":"failed to persist uploaded file"})";
        return error::INTERNAL_ERROR;
    }

    const int64_t size_bytes = static_cast<int64_t>(fs::file_size(dst, ec));
    const std::string sha256 = Sha256File(dst);
    if (ec || sha256.empty()) {
        fs::remove(dst, ec);
        rsp = R"({"error":"failed to compute sha256"})";
        return error::INTERNAL_ERROR;
    }
    const std::string plugin_id = sha256;

    {
        std::lock_guard<std::mutex> lock(mu_);
        if (EnsureOperatorDbLocked() != 0 || !operator_db_) {
            fs::remove(dst, ec);
            rsp = R"({"error":"operator catalog db is not initialized"})";
            return error::INTERNAL_ERROR;
        }

        sqlite3_stmt* q = nullptr;
        if (sqlite3_prepare_v2(operator_db_, "SELECT 1 FROM operator_plugin_store WHERE plugin_id=?1 LIMIT 1;",
                               -1, &q, nullptr) != SQLITE_OK) {
            fs::remove(dst, ec);
            rsp = R"({"error":"failed to query plugin store"})";
            return error::INTERNAL_ERROR;
        }
        sqlite3_bind_text(q, 1, plugin_id.c_str(), -1, SQLITE_TRANSIENT);
        const bool exists = (sqlite3_step(q) == SQLITE_ROW);
        sqlite3_finalize(q);
        if (exists) {
            fs::remove(dst, ec);
            rapidjson::StringBuffer buf;
            rapidjson::Writer<rapidjson::StringBuffer> w(buf);
            w.StartObject();
            w.Key("error");
            w.String("plugin already exists, delete old plugin before upload");
            w.Key("plugin_id");
            w.String(plugin_id.c_str());
            w.EndObject();
            rsp = buf.GetString();
            return error::CONFLICT;
        }

        sqlite3_stmt* ins = nullptr;
        const char* ins_sql =
            "INSERT INTO operator_plugin_store("
            "plugin_id, so_file, file_path, size_bytes, sha256, status, last_error, abi_version, operator_count, operators_json, created_at, updated_at"
            ") VALUES (?1, ?2, ?3, ?4, ?5, 'uploaded', '', NULL, NULL, '', CURRENT_TIMESTAMP, CURRENT_TIMESTAMP);";
        if (sqlite3_prepare_v2(operator_db_, ins_sql, -1, &ins, nullptr) != SQLITE_OK) {
            fs::remove(dst, ec);
            rsp = R"({"error":"failed to persist plugin metadata"})";
            return error::INTERNAL_ERROR;
        }
        sqlite3_bind_text(ins, 1, plugin_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 2, filename.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 3, dst.string().c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(ins, 4, size_bytes);
        sqlite3_bind_text(ins, 5, sha256.c_str(), -1, SQLITE_TRANSIENT);
        const int rc = sqlite3_step(ins);
        sqlite3_finalize(ins);
        if (rc != SQLITE_DONE) {
            fs::remove(dst, ec);
            rsp = R"({"error":"failed to persist plugin metadata"})";
            return error::INTERNAL_ERROR;
        }
    }

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("ok");
    w.Bool(true);
    w.Key("type");
    w.String("cpp");
    w.Key("plugin_id");
    w.String(plugin_id.c_str());
    w.Key("so_file");
    w.String(filename.c_str());
    w.Key("size_bytes");
    w.Int64(size_bytes);
    w.Key("sha256");
    w.String(sha256.c_str());
    w.Key("status");
    w.String("uploaded");
    w.EndObject();
    rsp = buf.GetString();
    return error::OK;
}

int BinAddonHostPlugin::ActivateCppPlugin(const std::string& plugin_id, std::string& rsp) {
    if (plugin_id.size() != 64) {
        rsp = R"({"error":"invalid plugin_id"})";
        return error::BAD_REQUEST;
    }
    if (!registry_) {
        rsp = R"({"error":"operator registry unavailable"})";
        return error::UNAVAILABLE;
    }

    PluginStoreRow row;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (EnsureOperatorDbLocked() != 0 || !operator_db_) {
            rsp = R"({"error":"operator catalog db is not initialized"})";
            return error::INTERNAL_ERROR;
        }
        if (!QueryPluginByIdLocked(plugin_id, &row)) {
            rsp = R"({"error":"cpp plugin not found"})";
            return error::NOT_FOUND;
        }
        if (loaded_plugins_.count(plugin_id) != 0) {
            rsp = R"({"error":"cpp plugin already activated"})";
            return error::CONFLICT;
        }
    }

    if (row.file_path.empty() || !fs::exists(row.file_path)) {
        std::lock_guard<std::mutex> lock(mu_);
        (void)UpdatePluginStatusLocked(plugin_id, "broken", "plugin file not found", -1, -1, "");
        rsp = R"({"error":"plugin file not found"})";
        return error::NOT_FOUND;
    }

    using FnStreamAbiVersion = int (*)();
    using FnStreamOperatorCount = int (*)();
    using FnCreateStreamOperator = IStreamOperator* (*)(int);
    using FnDestroyStreamOperator = void (*)(IStreamOperator*);

    const std::string current_sha256 = Sha256File(row.file_path);
    if (current_sha256.empty() || current_sha256 != row.sha256) {
        std::lock_guard<std::mutex> lock(mu_);
        (void)MarkPluginBrokenLocked(plugin_id, "plugin sha256 mismatch", -1);
        rsp = R"({"error":"plugin sha256 mismatch"})";
        return error::BAD_REQUEST;
    }

    void* handle = dlopen(row.file_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        const char* dlopen_error = dlerror();
        const std::string err = std::string("dlopen failed: ") + (dlopen_error ? dlopen_error : "unknown error");
        std::lock_guard<std::mutex> lock(mu_);
        (void)UpdatePluginStatusLocked(plugin_id, "broken", err, -1, -1, "");
        rsp = std::string("{\"error\":\"") + err + "\"}";
        return error::BAD_REQUEST;
    }

    std::shared_ptr<LoadedPlugin> loaded;
    try {
        loaded = std::make_shared<LoadedPlugin>();
    } catch (...) {
        dlclose(handle);
        rsp = R"({"error":"failed to allocate plugin state"})";
        return error::INTERNAL_ERROR;
    }
    loaded->handle = handle;
    loaded->plugin_id = plugin_id;
    loaded->file_path = row.file_path;
    loaded->so_file = row.so_file;
    loaded->sha256 = row.sha256;
    loaded->size_bytes = row.size_bytes;
    loaded->pending_unload.store(true, std::memory_order_release);

    auto mark_broken = [&](int code, const char* message, int abi_version) {
        std::lock_guard<std::mutex> lock(mu_);
        (void)MarkPluginBrokenLocked(plugin_id, message, abi_version);
        rsp = std::string("{\"error\":\"") + message + "\"}";
        return code;
    };

    auto* abi_fn = reinterpret_cast<CppOperatorPluginAbiVersionFn>(dlsym(handle, kCppOperatorPluginAbiVersionSymbol));
    auto* count_fn = reinterpret_cast<CppOperatorPluginCountFn>(dlsym(handle, kCppOperatorPluginCountSymbol));
    auto* create_fn = reinterpret_cast<CppOperatorPluginCreateV1Fn>(dlsym(handle, kCppOperatorPluginCreateV1Symbol));
    auto* destroy_fn = reinterpret_cast<CppOperatorPluginDestroyV1Fn>(dlsym(handle, kCppOperatorPluginDestroyV1Symbol));
    auto* describe_fn =
        reinterpret_cast<CppOperatorPluginDescribeV2Fn>(dlsym(handle, kCppOperatorPluginDescribeV2Symbol));
    auto* create_capability_fn = reinterpret_cast<CppOperatorPluginCreateCapabilityV2Fn>(
        dlsym(handle, kCppOperatorPluginCreateCapabilityV2Symbol));
    auto* destroy_capability_fn = reinterpret_cast<CppOperatorPluginDestroyCapabilityV2Fn>(
        dlsym(handle, kCppOperatorPluginDestroyCapabilityV2Symbol));
    auto* stream_abi_fn = reinterpret_cast<FnStreamAbiVersion>(dlsym(handle, "flowsql_stream_abi_version"));
    auto* stream_count_fn = reinterpret_cast<FnStreamOperatorCount>(dlsym(handle, "flowsql_stream_operator_count"));
    auto* stream_create_fn = reinterpret_cast<FnCreateStreamOperator>(dlsym(handle, "flowsql_create_stream_operator"));
    auto* stream_destroy_fn = reinterpret_cast<FnDestroyStreamOperator>(dlsym(handle, "flowsql_destroy_stream_operator"));
    if (!abi_fn || !count_fn) return mark_broken(error::BAD_REQUEST, "missing required symbols", -1);

    int abi = -1;
    int count = -1;
    try {
        abi = abi_fn();
        count = count_fn();
    } catch (...) {
        return mark_broken(error::BAD_REQUEST, "plugin export threw an exception", abi);
    }
    if (abi != kCppOperatorPluginAbiVersionV1 && abi != kCppOperatorPluginAbiVersionV2) {
        return mark_broken(error::BAD_REQUEST, "abi version mismatch", abi);
    }
    if (count <= 0) return mark_broken(error::BAD_REQUEST, "operator_count must > 0", abi);

    loaded->abi_version = abi;
    loaded->count_fn = count_fn;

    const bool has_any_stream_symbols =
        stream_abi_fn || stream_count_fn || stream_create_fn || stream_destroy_fn;

    int stream_count = 0;
    if (abi == kCppOperatorPluginAbiVersionV1) {
        if (!create_fn || !destroy_fn) {
            return mark_broken(error::BAD_REQUEST, "missing required symbols", abi);
        }
        loaded->create_fn = create_fn;
        loaded->destroy_fn = destroy_fn;
        if (has_any_stream_symbols && (!stream_abi_fn || !stream_count_fn || !stream_create_fn || !stream_destroy_fn)) {
            return mark_broken(error::BAD_REQUEST, "missing stream operator symbols", abi);
        }
    } else {
        if (!describe_fn || !create_capability_fn || !destroy_capability_fn) {
            return mark_broken(error::BAD_REQUEST, "missing required V2 symbols", abi);
        }
        if (has_any_stream_symbols) {
            return mark_broken(error::BAD_REQUEST, "contract-specific count is not allowed in ABI V2", abi);
        }
        loaded->describe_fn = describe_fn;
        loaded->create_capability_fn = create_capability_fn;
        loaded->destroy_capability_fn = destroy_capability_fn;
    }

    if (abi == kCppOperatorPluginAbiVersionV1 && has_any_stream_symbols) {
        constexpr int kFlowSqlStreamAbiVersion = 1;
        int stream_abi = -1;
        try {
            stream_abi = stream_abi_fn();
            stream_count = stream_count_fn();
        } catch (...) {
            return mark_broken(error::BAD_REQUEST, "stream plugin export threw an exception", abi);
        }
        if (stream_abi != kFlowSqlStreamAbiVersion) {
            return mark_broken(error::BAD_REQUEST, "stream abi version mismatch", abi);
        }
        if (stream_count < 0) {
            return mark_broken(error::BAD_REQUEST, "stream_operator_count must >= 0", abi);
        }
        for (int i = 0; i < stream_count; ++i) {
            IStreamOperator* op = nullptr;
            try {
                op = stream_create_fn(i);
            } catch (...) {
                op = nullptr;
            }
            if (!op) return mark_broken(error::BAD_REQUEST, "create stream operator failed", abi);
            std::string category;
            std::string name;
            try {
                category = op->Category();
                name = op->Name();
            } catch (...) {
                try {
                    stream_destroy_fn(op);
                } catch (...) {
                }
                return mark_broken(error::BAD_REQUEST, "stream operator metadata failed", abi);
            }
            try {
                stream_destroy_fn(op);
            } catch (...) {
                return mark_broken(error::BAD_REQUEST, "destroy stream operator failed", abi);
            }
            if (category.empty() || name.empty()) {
                return mark_broken(error::BAD_REQUEST, "empty category/name in stream operator", abi);
            }
        }
    }

    std::vector<OperatorMeta> metas;
    std::vector<std::string> keys;
    std::vector<std::string> names;
    metas.reserve(static_cast<size_t>(count));
    keys.reserve(static_cast<size_t>(count));
    names.reserve(static_cast<size_t>(count));
    std::unordered_set<std::string> local_keys;

    for (int i = 0; i < count; ++i) {
        OperatorMeta meta;
        meta.source = "cpp_plugin";

        if (abi == kCppOperatorPluginAbiVersionV1) {
            IOperator* op = nullptr;
            try {
                op = create_fn(i);
            } catch (...) {
                op = nullptr;
            }
            if (!op) return mark_broken(error::BAD_REQUEST, "create operator failed", abi);
            try {
                meta.category = op->Category();
                meta.name = op->Name();
                meta.description = op->Description();
                meta.position = op->Position() == OperatorPosition::STORAGE ? "storage" : "data";
            } catch (...) {
                try {
                    destroy_fn(op);
                } catch (...) {
                }
                return mark_broken(error::BAD_REQUEST, "operator metadata failed", abi);
            }
            try {
                destroy_fn(op);
            } catch (...) {
                return mark_broken(error::BAD_REQUEST, "destroy operator failed", abi);
            }
        } else {
            CppOperatorDescriptorV2 descriptor{};
            descriptor.struct_size = kCppOperatorDescriptorV2Size;
            int describe_rc = -1;
            try {
                describe_rc = describe_fn(i, &descriptor);
            } catch (...) {
                describe_rc = -1;
            }
            if (describe_rc != 0) {
                return mark_broken(error::BAD_REQUEST, "describe operator failed", abi);
            }
            if (!descriptor.category || !*descriptor.category || !descriptor.name || !*descriptor.name) {
                return mark_broken(error::BAD_REQUEST, "empty category/name in plugin operator", abi);
            }
            if (!SameGuid(descriptor.contract_iid, IID_OPERATOR) &&
                !SameGuid(descriptor.contract_iid, IID_BLOCK_TRANSFORM_OPERATOR_V1)) {
                return mark_broken(error::BAD_REQUEST, "unsupported operator contract", abi);
            }

            void* capability = nullptr;
            try {
                capability = create_capability_fn(i, querier_);
            } catch (...) {
                capability = nullptr;
            }
            if (!capability) {
                return mark_broken(error::BAD_REQUEST, "create operator capability failed", abi);
            }

            LoadedPlugin::Capability stored;
            stored.index = i;
            stored.category = descriptor.category;
            stored.name = descriptor.name;
            stored.description = descriptor.description ? descriptor.description : "";
            stored.contract_iid = descriptor.contract_iid;
            stored.instance = capability;
            loaded->capabilities.push_back(std::move(stored));

            try {
                if (SameGuid(descriptor.contract_iid, IID_OPERATOR)) {
                    auto* op = static_cast<IOperator*>(capability);
                    meta.category = op->Category();
                    meta.name = op->Name();
                    meta.description = descriptor.description ? descriptor.description : op->Description();
                    meta.position = op->Position() == OperatorPosition::STORAGE ? "storage" : "data";
                } else {
                    auto* op = static_cast<IBlockTransformOperatorV1*>(capability);
                    meta.category = op->Category();
                    meta.name = op->Name();
                    meta.description = descriptor.description ? descriptor.description : op->Description();
                    meta.position = "data";
                }
            } catch (...) {
                return mark_broken(error::BAD_REQUEST, "operator capability metadata failed", abi);
            }
            if (meta.category != descriptor.category || meta.name != descriptor.name) {
                return mark_broken(error::BAD_REQUEST, "operator descriptor mismatch", abi);
            }
        }

        if (meta.category.empty() || meta.name.empty()) {
            return mark_broken(error::BAD_REQUEST, "empty category/name in plugin operator", abi);
        }

        const std::string key = meta.category + "." + meta.name;
        if (!local_keys.insert(ToLowerAscii(key)).second) {
            return mark_broken(error::CONFLICT, "duplicate operators inside plugin", abi);
        }
        keys.push_back(key);
        names.push_back(meta.name);
        metas.push_back(std::move(meta));
    }

    loaded->operator_keys = keys;
    loaded->operator_names = names;

    {
        std::lock_guard<std::mutex> lock(mu_);
        auto rollback_registered = [this](const std::vector<std::string>& keys_to_remove) {
            for (const auto& key : keys_to_remove) {
                (void)registry_->RemoveFactory(key.c_str());
            }
        };
        if (EnsureOperatorDbLocked() != 0 || !operator_db_) {
            rsp = R"({"error":"operator catalog db is not initialized"})";
            return error::INTERNAL_ERROR;
        }
        if (loaded_plugins_.count(plugin_id) != 0) {
            rsp = R"({"error":"cpp plugin already activated"})";
            return error::CONFLICT;
        }

        std::vector<std::string> inserted_keys;
        for (size_t i = 0; i < keys.size(); ++i) {
            OperatorFactory factory;
            if (abi == kCppOperatorPluginAbiVersionV1) {
                factory = [loaded, idx = static_cast<int>(i)]() -> IOperator* {
                    loaded->active_count.fetch_add(1, std::memory_order_seq_cst);
                    if (loaded->pending_unload.load(std::memory_order_seq_cst)) {
                        loaded->active_count.fetch_sub(1, std::memory_order_seq_cst);
                        return nullptr;
                    }
                    IOperator* impl = nullptr;
                    try {
                        impl = loaded->create_fn(idx);
                    } catch (...) {
                        impl = nullptr;
                    }
                    if (!impl) {
                        loaded->active_count.fetch_sub(1, std::memory_order_seq_cst);
                        return nullptr;
                    }
                    try {
                        auto* proxy = new BinAddonOperatorProxy(impl, loaded);
                        loaded->active_count.fetch_sub(1, std::memory_order_seq_cst);
                        return proxy;
                    } catch (...) {
                        try {
                            loaded->destroy_fn(impl);
                        } catch (...) {
                        }
                        loaded->active_count.fetch_sub(1, std::memory_order_seq_cst);
                        return nullptr;
                    }
                };
            } else {
                factory = [loaded]() -> IOperator* { return nullptr; };
            }
            if (registry_->Register(keys[i].c_str(), std::move(factory)) != 0) {
                rollback_registered(inserted_keys);
                (void)MarkPluginBrokenLocked(plugin_id, "operator factory conflict", abi);
                rsp = R"({"error":"operator factory conflict"})";
                return error::CONFLICT;
            }
            inserted_keys.push_back(keys[i]);
        }

        try {
            if (!loaded_plugins_.emplace(plugin_id, loaded).second) {
                rollback_registered(inserted_keys);
                rsp = R"({"error":"cpp plugin already activated"})";
                return error::CONFLICT;
            }
        } catch (...) {
            rollback_registered(inserted_keys);
            (void)MarkPluginBrokenLocked(plugin_id, "failed to allocate active plugin entry", abi);
            rsp = R"({"error":"failed to allocate active plugin entry"})";
            return error::INTERNAL_ERROR;
        }
        auto rollback_runtime = [&]() {
            loaded_plugins_.erase(plugin_id);
            rollback_registered(inserted_keys);
        };

        if (sqlite3_exec(operator_db_, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK) {
            rollback_runtime();
            (void)MarkPluginBrokenLocked(plugin_id, "failed to begin activation transaction", abi);
            rsp = R"({"error":"failed to begin activation transaction"})";
            return error::INTERNAL_ERROR;
        }

        for (size_t i = 0; i < keys.size(); ++i) {
            sqlite3_stmt* query = nullptr;
            const char* query_sql =
                "SELECT plugin_id FROM operator_catalog "
                "WHERE category=?1 COLLATE NOCASE AND name=?2 COLLATE NOCASE LIMIT 1;";
            if (sqlite3_prepare_v2(operator_db_, query_sql, -1, &query, nullptr) != SQLITE_OK) {
                (void)sqlite3_exec(operator_db_, "ROLLBACK", nullptr, nullptr, nullptr);
                rollback_runtime();
                (void)MarkPluginBrokenLocked(plugin_id, "failed to query operator conflict", abi);
                rsp = R"({"error":"failed to query operator conflict"})";
                return error::INTERNAL_ERROR;
            }
            sqlite3_bind_text(query, 1, metas[i].category.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(query, 2, metas[i].name.c_str(), -1, SQLITE_TRANSIENT);
            const bool row_exists = sqlite3_step(query) == SQLITE_ROW;
            std::string existing_plugin_id;
            if (row_exists) {
                const unsigned char* value = sqlite3_column_text(query, 0);
                existing_plugin_id = value ? reinterpret_cast<const char*>(value) : "";
            }
            sqlite3_finalize(query);
            if (row_exists && existing_plugin_id != plugin_id) {
                (void)sqlite3_exec(operator_db_, "ROLLBACK", nullptr, nullptr, nullptr);
                rollback_runtime();
                (void)MarkPluginBrokenLocked(plugin_id, "operator name conflict", abi);
                rsp = R"({"error":"operator name conflict"})";
                return error::CONFLICT;
            }
        }

        if (UpsertCppOperatorsLocked(plugin_id, metas) != 0) {
            (void)sqlite3_exec(operator_db_, "ROLLBACK", nullptr, nullptr, nullptr);
            rollback_runtime();
            (void)MarkPluginBrokenLocked(plugin_id, "failed to upsert operator catalog", abi);
            rsp = R"({"error":"failed to upsert operator catalog"})";
            return error::INTERNAL_ERROR;
        }

        const std::string operators_json = JsonArrayFromStrings(keys);
        if (UpdatePluginStatusLocked(plugin_id, "activated", "", abi, count, operators_json) != 0) {
            (void)sqlite3_exec(operator_db_, "ROLLBACK", nullptr, nullptr, nullptr);
            rollback_runtime();
            (void)MarkPluginBrokenLocked(plugin_id, "failed to update plugin status", abi);
            rsp = R"({"error":"failed to update plugin status"})";
            return error::INTERNAL_ERROR;
        }
        if (sqlite3_exec(operator_db_, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
            (void)sqlite3_exec(operator_db_, "ROLLBACK", nullptr, nullptr, nullptr);
            rollback_runtime();
            (void)MarkPluginBrokenLocked(plugin_id, "failed to commit activation", abi);
            rsp = R"({"error":"failed to commit activation"})";
            return error::INTERNAL_ERROR;
        }

        loaded->pending_unload.store(false, std::memory_order_seq_cst);
    }

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("ok");
    w.Bool(true);
    w.Key("plugin_id");
    w.String(plugin_id.c_str());
    w.Key("abi_version");
    w.Int(abi);
    w.Key("operator_count");
    w.Int(count);
    if (has_any_stream_symbols) {
        w.Key("stream_operator_count");
        w.Int(stream_count);
    }
    w.Key("operators");
    w.StartArray();
    for (const auto& key : keys) w.String(key.c_str());
    w.EndArray();
    w.EndObject();
    rsp = buf.GetString();
    return error::OK;
}

int BinAddonHostPlugin::DeactivateCppPlugin(const std::string& plugin_id, std::string& rsp) {
    if (plugin_id.size() != 64) {
        rsp = R"({"error":"invalid plugin_id"})";
        return error::BAD_REQUEST;
    }

    std::shared_ptr<LoadedPlugin> retired_plugin;
    {
        std::lock_guard<std::mutex> lock(mu_);
        PluginStoreRow row;
        if (EnsureOperatorDbLocked() != 0 || !operator_db_) {
            rsp = R"({"error":"operator catalog db is not initialized"})";
            return error::INTERNAL_ERROR;
        }
        if (!QueryPluginByIdLocked(plugin_id, &row)) {
            rsp = R"({"error":"cpp plugin not found"})";
            return error::NOT_FOUND;
        }

        auto it = loaded_plugins_.find(plugin_id);
        if (it != loaded_plugins_.end()) {
            auto loaded = it->second;
            loaded->pending_unload.store(true, std::memory_order_seq_cst);
            if (loaded->active_count.load(std::memory_order_seq_cst) > 0) {
                loaded->pending_unload.store(false, std::memory_order_seq_cst);
                rsp = R"({"error":"plugin is in use"})";
                return error::CONFLICT;
            }

            if (sqlite3_exec(operator_db_, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK) {
                loaded->pending_unload.store(false, std::memory_order_seq_cst);
                rsp = R"({"error":"failed to begin deactivation transaction"})";
                return error::INTERNAL_ERROR;
            }
            if (SetCppOperatorsActiveByPluginLocked(plugin_id, 0) != 0 ||
                UpdatePluginStatusLocked(plugin_id, "deactivated", "", row.abi_version, row.operator_count,
                                         row.operators_json) != 0 ||
                sqlite3_exec(operator_db_, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
                (void)sqlite3_exec(operator_db_, "ROLLBACK", nullptr, nullptr, nullptr);
                loaded->pending_unload.store(false, std::memory_order_seq_cst);
                rsp = R"({"error":"failed to commit plugin deactivation"})";
                return error::INTERNAL_ERROR;
            }

            for (const auto& key : loaded->operator_keys) {
                (void)registry_->RemoveFactory(key.c_str());
            }
            retired_plugin = std::move(loaded);
            loaded_plugins_.erase(it);
        } else {
            if (!EqualsIgnoreCase(row.status, "deactivated")) {
                rsp = R"({"error":"activated plugin is not loaded"})";
                return error::CONFLICT;
            }
            if (SetCppOperatorsActiveByPluginLocked(plugin_id, 0) != 0) {
                rsp = R"({"error":"failed to update operator catalog"})";
                return error::INTERNAL_ERROR;
            }
        }
    }

    retired_plugin.reset();
    rsp = R"({"ok":true})";
    return error::OK;
}

int BinAddonHostPlugin::DeleteCppPlugin(const std::string& plugin_id, std::string& rsp) {
    if (plugin_id.size() != 64) {
        rsp = R"({"error":"invalid plugin_id"})";
        return error::BAD_REQUEST;
    }

    std::string file_path;
    {
        std::lock_guard<std::mutex> lock(mu_);
        PluginStoreRow row;
        if (EnsureOperatorDbLocked() != 0 || !operator_db_) {
            rsp = R"({"error":"operator catalog db is not initialized"})";
            return error::INTERNAL_ERROR;
        }
        if (!QueryPluginByIdLocked(plugin_id, &row)) {
            rsp = R"({"error":"cpp plugin not found"})";
            return error::NOT_FOUND;
        }
        if (EqualsIgnoreCase(row.status, "activated")) {
            rsp = R"({"error":"plugin is activated, deactivate first"})";
            return error::CONFLICT;
        }

        auto it = loaded_plugins_.find(plugin_id);
        if (it != loaded_plugins_.end()) {
            auto loaded = it->second;
            if (loaded->active_count.load(std::memory_order_acquire) > 0) {
                rsp = R"({"error":"plugin is in use"})";
                return error::CONFLICT;
            }
            for (const auto& key : loaded->operator_keys) (void)registry_->RemoveFactory(key.c_str());
            loaded_plugins_.erase(it);
        }

        if (DeleteCppOperatorsByPluginLocked(plugin_id) != 0) {
            rsp = R"({"error":"failed to delete cpp operators"})";
            return error::INTERNAL_ERROR;
        }
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(operator_db_, "DELETE FROM operator_plugin_store WHERE plugin_id=?1;", -1, &stmt, nullptr) != SQLITE_OK) {
            rsp = R"({"error":"failed to delete plugin metadata"})";
            return error::INTERNAL_ERROR;
        }
        sqlite3_bind_text(stmt, 1, plugin_id.c_str(), -1, SQLITE_TRANSIENT);
        const int rc = sqlite3_step(stmt);
        const int changed = sqlite3_changes(operator_db_);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE || changed <= 0) {
            rsp = R"({"error":"failed to delete plugin metadata"})";
            return error::INTERNAL_ERROR;
        }
        file_path = row.file_path;
    }

    if (!file_path.empty()) {
        std::error_code ec;
        fs::remove(file_path, ec);
    }
    rsp = R"({"ok":true})";
    return error::OK;
}

int BinAddonHostPlugin::GetCppPluginDetail(const std::string& plugin_id, std::string& rsp) {
    if (plugin_id.size() != 64) {
        rsp = R"({"error":"invalid plugin_id"})";
        return error::BAD_REQUEST;
    }

    PluginStoreRow row;
    std::vector<std::pair<std::string, std::string>> operator_details;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (EnsureOperatorDbLocked() != 0 || !operator_db_) {
            rsp = R"({"error":"operator catalog db is not initialized"})";
            return error::INTERNAL_ERROR;
        }
        if (!QueryPluginByIdLocked(plugin_id, &row)) {
            rsp = R"({"error":"cpp plugin not found"})";
            return error::NOT_FOUND;
        }
        const auto loaded = loaded_plugins_.find(plugin_id);
        if (loaded != loaded_plugins_.end() && loaded->second) {
            operator_details.reserve(loaded->second->capabilities.size());
            for (const auto& capability : loaded->second->capabilities) {
                operator_details.emplace_back(capability.category + "." + capability.name,
                                              ContractName(capability.contract_iid));
            }
        }
    }

    const auto operators = ParseStringArrayJson(row.operators_json);
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("type");
    w.String("cpp");
    w.Key("plugin_id");
    w.String(row.plugin_id.c_str());
    w.Key("active");
    w.Int(EqualsIgnoreCase(row.status, "activated") ? 1 : 0);
    w.Key("plugin");
    w.StartObject();
    w.Key("so_file");
    w.String(row.so_file.c_str());
    w.Key("size_bytes");
    w.Int64(row.size_bytes);
    w.Key("sha256");
    w.String(row.sha256.c_str());
    w.Key("status");
    w.String(row.status.c_str());
    w.Key("last_error");
    w.String(row.last_error.c_str());
    w.Key("abi_version");
    if (row.abi_version >= 0) w.Int(row.abi_version);
    else w.Null();
    w.Key("operator_count");
    if (row.operator_count >= 0) w.Int(row.operator_count);
    else w.Null();
    w.Key("operators");
    if (!operators.empty()) {
        w.StartArray();
        for (const auto& op : operators) w.String(op.c_str());
        w.EndArray();
    } else {
        w.Null();
    }
    w.Key("operator_details");
    w.StartArray();
    for (const auto& detail : operator_details) {
        w.StartObject();
        w.Key("name");
        w.String(detail.first.c_str());
        w.Key("contract");
        w.String(detail.second.c_str());
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    w.EndObject();
    rsp = buf.GetString();
    return error::OK;
}

int BinAddonHostPlugin::RecoverActivatedPlugins() {
    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (EnsureOperatorDbLocked() != 0 || !operator_db_) return -1;
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT plugin_id FROM operator_plugin_store WHERE status='activated' ORDER BY plugin_id ASC;";
        if (sqlite3_prepare_v2(operator_db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return -1;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char* v = sqlite3_column_text(stmt, 0);
            if (v) ids.emplace_back(reinterpret_cast<const char*>(v));
        }
        sqlite3_finalize(stmt);
    }

    for (const auto& id : ids) {
        std::string unused_rsp;
        (void)ActivateCppPlugin(id, unused_rsp);
    }
    return 0;
}

}  // namespace binaddon
}  // namespace flowsql
