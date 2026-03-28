#include "binaddon_host_plugin.h"

#include <common/error_code.h>
#include <common/log.h>

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
constexpr int kFlowSqlCppAbiVersion = 1;

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
}  // namespace

BinAddonHostPlugin::LoadedPlugin::~LoadedPlugin() {
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
    loaded_plugins_.clear();
    if (operator_db_ != nullptr) {
        sqlite3_close(operator_db_);
        operator_db_ = nullptr;
    }
    return 0;
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

    using FnAbiVersion = int (*)();
    using FnOperatorCount = int (*)();
    using FnCreateOperator = IOperator* (*)(int);
    using FnDestroyOperator = void (*)(IOperator*);

    void* handle = dlopen(row.file_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        const std::string err = std::string("dlopen failed: ") + (dlerror() ? dlerror() : "");
        std::lock_guard<std::mutex> lock(mu_);
        (void)UpdatePluginStatusLocked(plugin_id, "broken", err, -1, -1, "");
        rsp = std::string("{\"error\":\"") + err + "\"}";
        return error::BAD_REQUEST;
    }

    auto* abi_fn = reinterpret_cast<FnAbiVersion>(dlsym(handle, "flowsql_abi_version"));
    auto* count_fn = reinterpret_cast<FnOperatorCount>(dlsym(handle, "flowsql_operator_count"));
    auto* create_fn = reinterpret_cast<FnCreateOperator>(dlsym(handle, "flowsql_create_operator"));
    auto* destroy_fn = reinterpret_cast<FnDestroyOperator>(dlsym(handle, "flowsql_destroy_operator"));
    if (!abi_fn || !count_fn || !create_fn || !destroy_fn) {
        dlclose(handle);
        std::lock_guard<std::mutex> lock(mu_);
        (void)UpdatePluginStatusLocked(plugin_id, "broken", "missing required symbols", -1, -1, "");
        rsp = R"({"error":"missing required symbols"})";
        return error::BAD_REQUEST;
    }

    const int abi = abi_fn();
    if (abi != kFlowSqlCppAbiVersion) {
        dlclose(handle);
        std::lock_guard<std::mutex> lock(mu_);
        (void)UpdatePluginStatusLocked(plugin_id, "broken", "abi version mismatch", abi, -1, "");
        rsp = R"({"error":"abi version mismatch"})";
        return error::BAD_REQUEST;
    }

    const int count = count_fn();
    if (count <= 0) {
        dlclose(handle);
        std::lock_guard<std::mutex> lock(mu_);
        (void)UpdatePluginStatusLocked(plugin_id, "broken", "operator_count must > 0", abi, -1, "");
        rsp = R"({"error":"operator_count must > 0"})";
        return error::BAD_REQUEST;
    }

    std::vector<OperatorMeta> metas;
    std::vector<std::string> keys;
    std::vector<std::string> names;
    metas.reserve(static_cast<size_t>(count));
    std::unordered_set<std::string> local_keys;

    for (int i = 0; i < count; ++i) {
        IOperator* op = nullptr;
        try {
            op = create_fn(i);
        } catch (...) {
            op = nullptr;
        }
        if (!op) {
            dlclose(handle);
            std::lock_guard<std::mutex> lock(mu_);
            (void)UpdatePluginStatusLocked(plugin_id, "broken", "create operator failed", abi, -1, "");
            rsp = R"({"error":"create operator failed"})";
            return error::BAD_REQUEST;
        }
        OperatorMeta meta;
        meta.category = op->Category();
        meta.name = op->Name();
        meta.description = op->Description();
        meta.position = op->Position() == OperatorPosition::STORAGE ? "storage" : "data";
        meta.source = "cpp_plugin";
        destroy_fn(op);

        if (meta.category.empty() || meta.name.empty()) {
            dlclose(handle);
            std::lock_guard<std::mutex> lock(mu_);
            (void)UpdatePluginStatusLocked(plugin_id, "broken", "empty category/name in plugin operator", abi, -1, "");
            rsp = R"({"error":"empty category/name in plugin operator"})";
            return error::BAD_REQUEST;
        }

        const std::string key = meta.category + "." + meta.name;
        if (!local_keys.insert(key).second) {
            dlclose(handle);
            std::lock_guard<std::mutex> lock(mu_);
            (void)UpdatePluginStatusLocked(plugin_id, "broken", "duplicate operators inside plugin", abi, -1, "");
            rsp = R"({"error":"duplicate operators inside plugin"})";
            return error::CONFLICT;
        }
        keys.push_back(key);
        names.push_back(meta.name);
        metas.push_back(std::move(meta));
    }

    auto loaded = std::make_shared<LoadedPlugin>();
    loaded->plugin_id = plugin_id;
    loaded->file_path = row.file_path;
    loaded->so_file = row.so_file;
    loaded->sha256 = row.sha256;
    loaded->abi_version = abi;
    loaded->size_bytes = row.size_bytes;
    loaded->count_fn = count_fn;
    loaded->create_fn = create_fn;
    loaded->destroy_fn = destroy_fn;
    loaded->operator_keys = keys;
    loaded->operator_names = names;
    // Two-phase activate:
    // keep factory creation blocked until DB/catalog + status updates are fully committed.
    loaded->pending_unload.store(true, std::memory_order_release);

    {
        std::lock_guard<std::mutex> lock(mu_);
        auto rollback_registered = [this](const std::vector<std::string>& keys_to_remove) {
            for (const auto& key : keys_to_remove) {
                (void)registry_->RemoveFactory(key.c_str());
            }
        };
        if (EnsureOperatorDbLocked() != 0 || !operator_db_) {
            dlclose(handle);
            rsp = R"({"error":"operator catalog db is not initialized"})";
            return error::INTERNAL_ERROR;
        }
        std::vector<std::string> inserted_keys;
        for (size_t i = 0; i < keys.size(); ++i) {
            sqlite3_stmt* q = nullptr;
            const char* qsql = "SELECT plugin_id FROM operator_catalog WHERE category=?1 COLLATE NOCASE AND name=?2 COLLATE NOCASE LIMIT 1;";
            std::string existing_plugin_id;
            if (sqlite3_prepare_v2(operator_db_, qsql, -1, &q, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(q, 1, metas[i].category.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(q, 2, metas[i].name.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(q) == SQLITE_ROW) {
                    const unsigned char* v = sqlite3_column_text(q, 0);
                    existing_plugin_id = v ? reinterpret_cast<const char*>(v) : "";
                }
                sqlite3_finalize(q);
            }
            if (!existing_plugin_id.empty() && existing_plugin_id != plugin_id) {
                rollback_registered(inserted_keys);
                dlclose(handle);
                (void)UpdatePluginStatusLocked(plugin_id, "broken", "operator name conflict", abi, -1, "");
                rsp = R"({"error":"operator name conflict"})";
                return error::CONFLICT;
            }
            IOperator* existing = registry_->Create(keys[i].c_str());
            if (existing != nullptr) {
                delete existing;
                rollback_registered(inserted_keys);
                dlclose(handle);
                (void)UpdatePluginStatusLocked(plugin_id, "broken", "operator factory conflict", abi, -1, "");
                rsp = R"({"error":"operator factory conflict"})";
                return error::CONFLICT;
            }

            const int reg_rc = registry_->Register(
                keys[i].c_str(),
                [loaded, idx = static_cast<int>(i)]() -> IOperator* {
                    if (loaded->pending_unload.load(std::memory_order_acquire)) return nullptr;
                    IOperator* impl = nullptr;
                    try {
                        impl = loaded->create_fn(idx);
                    } catch (...) {
                        impl = nullptr;
                    }
                    if (!impl) return nullptr;
                    return new BinAddonOperatorProxy(impl, loaded);
                });
            if (reg_rc != 0) {
                rollback_registered(inserted_keys);
                dlclose(handle);
                (void)UpdatePluginStatusLocked(plugin_id, "broken", "operator factory conflict", abi, -1, "");
                rsp = R"({"error":"operator factory conflict"})";
                return error::CONFLICT;
            }
            inserted_keys.push_back(keys[i]);
        }

        if (UpsertCppOperatorsLocked(plugin_id, metas) != 0) {
            rollback_registered(inserted_keys);
            dlclose(handle);
            (void)UpdatePluginStatusLocked(plugin_id, "broken", "failed to upsert operator catalog", abi, -1, "");
            rsp = R"({"error":"failed to upsert operator catalog"})";
            return error::INTERNAL_ERROR;
        }

        const std::string operators_json = JsonArrayFromStrings(names);
        if (UpdatePluginStatusLocked(plugin_id, "activated", "", abi, count, operators_json) != 0) {
            rollback_registered(inserted_keys);
            dlclose(handle);
            (void)SetCppOperatorsActiveByPluginLocked(plugin_id, 0);
            rsp = R"({"error":"failed to update plugin status"})";
            return error::INTERNAL_ERROR;
        }
        loaded->handle = handle;
        loaded_plugins_[plugin_id] = loaded;
        // Activation commit finished, allow factory creation now.
        loaded->pending_unload.store(false, std::memory_order_release);
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
    w.Key("operators");
    w.StartArray();
    for (const auto& n : names) w.String(n.c_str());
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
        loaded->pending_unload.store(true, std::memory_order_release);
        if (loaded->active_count.load(std::memory_order_acquire) > 0) {
            loaded->pending_unload.store(false, std::memory_order_release);
            rsp = R"({"error":"plugin is in use"})";
            return error::CONFLICT;
        }
        for (const auto& key : loaded->operator_keys) (void)registry_->RemoveFactory(key.c_str());
        loaded_plugins_.erase(it);
    }
    (void)SetCppOperatorsActiveByPluginLocked(plugin_id, 0);
    if (UpdatePluginStatusLocked(plugin_id, "deactivated", "", row.abi_version, row.operator_count, row.operators_json) != 0) {
        rsp = R"({"error":"failed to update plugin status"})";
        return error::INTERNAL_ERROR;
    }
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
    w.Key("path");
    w.String(row.file_path.c_str());
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
