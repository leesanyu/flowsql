#include "catalog_plugin.h"
#include "builtin/concat_operator.h"
#include "builtin/hstack_operator.h"

#include <framework/core/dataframe.h>
#include <framework/core/dataframe_channel.h>
#include <framework/core/passthrough_operator.h>

#include <common/error_code.h>
#include <common/log.h>

#include <cerrno>
#include <charconv>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <type_traits>
#include <utility>
#include <vector>
#include <unistd.h>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace fs = std::filesystem;

namespace flowsql {
namespace catalog {

namespace {
bool EqualsIgnoreCase(const std::string& a, const char* b) {
    if (!b) return false;
    size_t n = a.size();
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
}  // namespace

int CatalogPlugin::Option(const char* arg) {
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
        if (key == "data_dir" && !val.empty()) data_dir_ = val;
        if (key == "operator_db_dir" && !val.empty()) operator_db_dir_ = val;
        if (key == "operator_db_path" && !val.empty()) operator_db_path_ = val;
        pos = (end < opts.size()) ? end + 1 : opts.size();
    }
    return 0;
}

int CatalogPlugin::Load(IQuerier* querier) {
    querier_ = querier;
    binaddon_host_ = nullptr;
    // 在 Load 阶段尽力初始化 operator_catalog DB，避免 Start 顺序导致
    // Scheduler/Bridge 提前调用 UpsertBatch 时出现未初始化错误。
    // 这里不把初始化失败作为 Load 失败返回，保留“Start 阶段报错”的既有语义。
    if (EnsureDataDir() == 0 && EnsureOperatorDbDir() == 0) {
        if (EnsureOperatorCatalogDb() != 0) {
            LOG_WARN("CatalogPlugin::Load: operator catalog db init failed, will retry in Start()");
        }
    }
    if (Register("passthrough", []() -> IOperator* { return new PassthroughOperator(); }) != 0) return -1;
    if (Register("concat", []() -> IOperator* { return new ConcatOperator(); }) != 0) return -1;
    if (Register("hstack", []() -> IOperator* { return new HstackOperator(); }) != 0) return -1;
    if (Register("builtin.passthrough", []() -> IOperator* { return new PassthroughOperator(); }) != 0) return -1;
    if (Register("builtin.concat", []() -> IOperator* { return new ConcatOperator(); }) != 0) return -1;
    if (Register("builtin.hstack", []() -> IOperator* { return new HstackOperator(); }) != 0) return -1;
    return 0;
}

int CatalogPlugin::Unload() {
    Stop();
    return 0;
}

int CatalogPlugin::Start() {
    if (EnsureDataDir() != 0) return -1;
    if (EnsureOperatorDbDir() != 0) return -1;
    if (EnsureOperatorCatalogDb() != 0) return -1;

    for (const auto& entry : fs::directory_iterator(data_dir_)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".csv") continue;
        const std::string name = entry.path().stem().string();
        auto channel = LoadCsvFile(entry.path().string(), name);
        if (!channel) continue;

        std::lock_guard<std::mutex> lock(mu_);
        channels_[name] = std::move(channel);
    }
    return 0;
}

int CatalogPlugin::Stop() {
    std::lock_guard<std::mutex> lock(mu_);
    binaddon_host_ = nullptr;
    channels_.clear();
    if (operator_db_ != nullptr) {
        sqlite3_close(operator_db_);
        operator_db_ = nullptr;
    }
    return 0;
}

int CatalogPlugin::Register(const char* name, std::shared_ptr<IChannel> channel) {
    if (!name || !*name || !channel) return -1;
    auto* df_channel = dynamic_cast<IDataFrameChannel*>(channel.get());

    {
        std::lock_guard<std::mutex> lock(mu_);
        if (channels_.find(name) != channels_.end()) return -1;
    }

    if (df_channel && PersistChannelToCsv(name, df_channel) != 0) return -1;

    std::lock_guard<std::mutex> lock(mu_);
    if (channels_.find(name) != channels_.end()) return -1;
    channels_[name] = std::move(channel);
    return 0;
}

std::shared_ptr<IChannel> CatalogPlugin::Get(const char* name) {
    if (!name || !*name) return nullptr;
    std::lock_guard<std::mutex> lock(mu_);
    auto it = channels_.find(name);
    if (it == channels_.end()) return nullptr;
    return it->second;
}

int CatalogPlugin::Unregister(const char* name) {
    if (!name || !*name) return -1;
    bool remove_csv = false;

    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = channels_.find(name);
        if (it == channels_.end()) return -1;
        remove_csv = dynamic_cast<IDataFrameChannel*>(it->second.get()) != nullptr;
        channels_.erase(it);
    }

    if (remove_csv) {
        std::error_code ec;
        fs::remove(CsvPath(name), ec);
    }
    return 0;
}

int CatalogPlugin::Rename(const char* old_name, const char* new_name) {
    if (!old_name || !*old_name || !new_name || !*new_name) return -1;

    std::lock_guard<std::mutex> lock(mu_);
    auto old_it = channels_.find(old_name);
    if (old_it == channels_.end()) return -1;
    if (channels_.find(new_name) != channels_.end()) return -1;

    auto* df_channel = dynamic_cast<IDataFrameChannel*>(old_it->second.get());
    if (df_channel) {
        std::error_code ec;
        fs::rename(CsvPath(old_name), CsvPath(new_name), ec);
        if (ec) return -1;
    }

    auto channel = old_it->second;
    channels_.erase(old_it);
    channels_[new_name] = std::move(channel);
    return 0;
}

void CatalogPlugin::List(std::function<void(const char* name, std::shared_ptr<IChannel>)> callback) {
    if (!callback) return;

    std::vector<std::pair<std::string, std::shared_ptr<IChannel>>> snapshot;
    {
        std::lock_guard<std::mutex> lock(mu_);
        snapshot.reserve(channels_.size());
        for (const auto& [name, ch] : channels_) {
            snapshot.push_back({name, ch});
        }
    }
    for (const auto& [name, ch] : snapshot) callback(name.c_str(), ch);
}

int CatalogPlugin::Register(const char* name, OperatorFactory factory) {
    if (!name || !*name || !factory) return -1;
    std::lock_guard<std::mutex> lock(mu_);
    op_factories_[name] = std::move(factory);
    return 0;
}

IOperator* CatalogPlugin::Create(const char* name) {
    if (!name || !*name) return nullptr;
    std::lock_guard<std::mutex> lock(mu_);
    auto it = op_factories_.find(name);
    if (it == op_factories_.end()) return nullptr;
    return it->second();
}

int CatalogPlugin::RemoveFactory(const char* name) {
    if (!name || !*name) return -1;
    std::lock_guard<std::mutex> lock(mu_);
    auto it = op_factories_.find(name);
    if (it == op_factories_.end()) return -1;
    op_factories_.erase(it);
    return 0;
}

void CatalogPlugin::List(std::function<void(const char* name)> callback) {
    if (!callback) return;

    std::vector<std::string> names;
    {
        std::lock_guard<std::mutex> lock(mu_);
        names.reserve(op_factories_.size());
        for (const auto& [name, _] : op_factories_) names.push_back(name);
    }
    for (const auto& name : names) callback(name.c_str());
}

IBinAddonHost* CatalogPlugin::ResolveBinAddonHost() {
    if (binaddon_host_) return binaddon_host_;
    if (!querier_) return nullptr;
    binaddon_host_ = static_cast<IBinAddonHost*>(querier_->First(IID_BINADDON_HOST));
    return binaddon_host_;
}

OperatorStatus CatalogPlugin::QueryStatus(const std::string& category, const std::string& name) {
    if (category.empty() || name.empty()) return OperatorStatus::kNotFound;

    std::lock_guard<std::mutex> lock(mu_);
    if (operator_db_ == nullptr) return OperatorStatus::kNotFound;

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT active FROM operator_catalog WHERE category = ?1 COLLATE NOCASE AND name = ?2 COLLATE NOCASE LIMIT 1;";
    if (sqlite3_prepare_v2(operator_db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return OperatorStatus::kNotFound;
    }
    sqlite3_bind_text(stmt, 1, category.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return OperatorStatus::kNotFound;
    }

    const int active = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return active == 1 ? OperatorStatus::kActive : OperatorStatus::kDeactivated;
}

UpsertResult CatalogPlugin::UpsertBatch(const std::vector<OperatorMeta>& operators) {
    UpsertResult result;
    if (operators.empty()) return result;

    std::lock_guard<std::mutex> lock(mu_);
    if (operator_db_ == nullptr) {
        result.failed_count = static_cast<int32_t>(operators.size());
        result.error_message = "operator catalog db is not initialized";
        return result;
    }

    const char* sql =
        "INSERT INTO operator_catalog("
        "category, name, type, source, description, position, active, editable, created_at, updated_at"
        ") VALUES (?1, ?2, ?3, ?4, ?5, ?6, 0, ?7, CURRENT_TIMESTAMP, CURRENT_TIMESTAMP) "
        "ON CONFLICT(category, name) DO UPDATE SET "
        "type=excluded.type, source=excluded.source, description=excluded.description, "
        "position=excluded.position, editable=excluded.editable, updated_at=CURRENT_TIMESTAMP;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(operator_db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        result.failed_count = static_cast<int32_t>(operators.size());
        result.error_message = sqlite3_errmsg(operator_db_);
        return result;
    }

    for (const auto& op : operators) {
        if (op.category.empty() || op.name.empty()) {
            ++result.failed_count;
            if (result.error_message.empty()) result.error_message = "category/name must not be empty";
            continue;
        }

        sqlite3_clear_bindings(stmt);
        sqlite3_reset(stmt);
        sqlite3_bind_text(stmt, 1, op.category.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, op.name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, op.type.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, op.source.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, op.description.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 6, op.position.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 7, EqualsIgnoreCase(op.type, "python") ? 1 : 0);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            ++result.failed_count;
            if (result.error_message.empty()) result.error_message = sqlite3_errmsg(operator_db_);
            continue;
        }
        ++result.success_count;
    }
    sqlite3_finalize(stmt);
    return result;
}

int CatalogPlugin::SetActive(const std::string& category, const std::string& name, bool active) {
    return SetOperatorActive(category, name, active ? 1 : 0);
}

void CatalogPlugin::EnumRoutes(std::function<void(const RouteItem&)> cb) {
    cb({"GET", "/channels/dataframe",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleListChannels(u, req, rsp);
        }});
    cb({"POST", "/channels/dataframe/import",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleImportCsv(u, req, rsp);
        }});
    cb({"POST", "/channels/dataframe/preview",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandlePreview(u, req, rsp);
        }});
    cb({"POST", "/channels/dataframe/rename",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleRename(u, req, rsp);
        }});
    cb({"POST", "/channels/dataframe/delete",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleDelete(u, req, rsp);
        }});
    cb({"POST", "/operators/list",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleListOperators(u, req, rsp);
        }});
    cb({"POST", "/operators/upload",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleOperatorUpload(u, req, rsp);
        }});
    cb({"POST", "/operators/delete",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleOperatorDelete(u, req, rsp);
        }});
    cb({"POST", "/operators/detail",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleOperatorDetail(u, req, rsp);
        }});
    cb({"POST", "/operators/activate",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleOperatorActivate(u, req, rsp);
        }});
    cb({"POST", "/operators/deactivate",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleOperatorDeactivate(u, req, rsp);
        }});
    cb({"POST", "/operators/update",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleOperatorUpdate(u, req, rsp);
        }});
    cb({"POST", "/operators/upsert_batch",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleOperatorUpsertBatch(u, req, rsp);
        }});
}

int32_t CatalogPlugin::HandleListChannels(const std::string&, const std::string&, std::string& rsp) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("channels");
    w.StartArray();

    List([&](const char* name, std::shared_ptr<IChannel> ch) {
        auto* df_ch = dynamic_cast<IDataFrameChannel*>(ch.get());
        if (!name || !df_ch) return;
        DataFrame data;
        int rows = 0;
        std::vector<Field> schema;
        if (df_ch->Read(&data) == 0) {
            rows = data.RowCount();
            schema = data.GetSchema();
        }
        w.StartObject();
        w.Key("name");
        w.String(name);
        w.Key("rows");
        w.Int(rows);
        w.Key("schema");
        w.StartArray();
        for (const auto& f : schema) {
            w.StartObject();
            w.Key("name");
            w.String(f.name.c_str());
            w.Key("type");
            w.Int(static_cast<int>(f.type));
            w.EndObject();
        }
        w.EndArray();
        w.EndObject();
    });

    w.EndArray();
    w.EndObject();
    rsp = buf.GetString();
    return error::OK;
}

int32_t CatalogPlugin::HandleImportCsv(const std::string&, const std::string& req, std::string& rsp) {
    rapidjson::Document doc;
    doc.Parse(req.c_str());
    if (doc.HasParseError() || !doc.IsObject() ||
        !doc.HasMember("filename") || !doc["filename"].IsString() ||
        !doc.HasMember("tmp_path") || !doc["tmp_path"].IsString()) {
        rsp = R"({"error":"invalid request, expected {\"filename\":\"...\",\"tmp_path\":\"...\"}"})";
        return error::BAD_REQUEST;
    }

    const std::string filename = doc["filename"].GetString();
    const std::string tmp_path = doc["tmp_path"].GetString();
    if (filename.empty() || tmp_path.empty()) {
        rsp = R"({"error":"filename/tmp_path must not be empty"})";
        return error::BAD_REQUEST;
    }

    fs::path tmp(tmp_path);
    if (!fs::exists(tmp) || !fs::is_regular_file(tmp)) {
        rsp = R"({"error":"tmp_path not found"})";
        return error::BAD_REQUEST;
    }

    if (EnsureDataDir() != 0) {
        rsp = R"({"error":"failed to create data_dir"})";
        return error::INTERNAL_ERROR;
    }

    const std::string name = GenerateImportName(filename);
    const fs::path final_path = CsvPath(name);

    std::error_code ec;
    fs::rename(tmp, final_path, ec);
    if (ec) {
        ec.clear();
        fs::copy_file(tmp, final_path, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            rsp = R"({"error":"failed to move uploaded file"})";
            return error::BAD_REQUEST;
        }
        fs::remove(tmp, ec);
    }

    auto channel = LoadCsvFile(final_path.string(), name);
    if (!channel) {
        fs::remove(final_path, ec);
        rsp = R"({"error":"failed to parse csv"})";
        return error::BAD_REQUEST;
    }

    if (Register(name.c_str(), std::static_pointer_cast<IChannel>(channel)) != 0) {
        fs::remove(final_path, ec);
        rsp = R"({"error":"failed to register channel"})";
        return error::INTERNAL_ERROR;
    }

    DataFrame data;
    channel->Read(&data);
    const auto schema = data.GetSchema();

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("name");
    w.String(name.c_str());
    w.Key("rows");
    w.Int(data.RowCount());
    w.Key("schema");
    w.StartArray();
    for (const auto& f : schema) {
        w.StartObject();
        w.Key("name");
        w.String(f.name.c_str());
        w.Key("type");
        w.Int(static_cast<int>(f.type));
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    rsp = buf.GetString();
    return error::OK;
}

int32_t CatalogPlugin::HandlePreview(const std::string&, const std::string& req, std::string& rsp) {
    rapidjson::Document doc;
    doc.Parse(req.c_str());
    if (doc.HasParseError() || !doc.IsObject() ||
        !doc.HasMember("name") || !doc["name"].IsString()) {
        rsp = R"({"error":"missing 'name'"})";
        return error::BAD_REQUEST;
    }

    auto raw_ch = Get(doc["name"].GetString());
    auto ch = std::dynamic_pointer_cast<IDataFrameChannel>(raw_ch);
    if (!ch) {
        rsp = R"({"error":"channel not found"})";
        return error::NOT_FOUND;
    }

    DataFrame data;
    if (ch->Read(&data) != 0) {
        rsp = R"({"error":"failed to read channel"})";
        return error::INTERNAL_ERROR;
    }

    const auto schema = data.GetSchema();
    const int rows = data.RowCount();
    const int cap = rows > 100 ? 100 : rows;

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("columns");
    w.StartArray();
    for (const auto& f : schema) w.String(f.name.c_str());
    w.EndArray();
    w.Key("types");
    w.StartArray();
    for (const auto& f : schema) w.String(DataTypeName(f.type));
    w.EndArray();
    w.Key("data");
    w.StartArray();
    for (int r = 0; r < cap; ++r) {
        const auto row = data.GetRow(r);
        w.StartArray();
        for (const auto& v : row) {
            std::visit(
                [&](auto&& val) {
                    using T = std::decay_t<decltype(val)>;
                    if constexpr (std::is_same_v<T, int32_t>) w.Int(val);
                    else if constexpr (std::is_same_v<T, int64_t>) w.Int64(val);
                    else if constexpr (std::is_same_v<T, uint32_t>) w.Uint(val);
                    else if constexpr (std::is_same_v<T, uint64_t>) w.Uint64(val);
                    else if constexpr (std::is_same_v<T, float>) w.Double(val);
                    else if constexpr (std::is_same_v<T, double>) w.Double(val);
                    else if constexpr (std::is_same_v<T, std::string>) w.String(val.c_str());
                    else if constexpr (std::is_same_v<T, std::vector<uint8_t>>)
                        w.String(reinterpret_cast<const char*>(val.data()), val.size());
                    else if constexpr (std::is_same_v<T, bool>) w.Bool(val);
                },
                v);
        }
        w.EndArray();
    }
    w.EndArray();
    w.Key("rows");
    w.Int(rows);
    w.EndObject();
    rsp = buf.GetString();
    return error::OK;
}

int32_t CatalogPlugin::HandleRename(const std::string&, const std::string& req, std::string& rsp) {
    rapidjson::Document doc;
    doc.Parse(req.c_str());
    if (doc.HasParseError() || !doc.IsObject() ||
        !doc.HasMember("name") || !doc["name"].IsString() ||
        !doc.HasMember("new_name") || !doc["new_name"].IsString()) {
        rsp = R"({"error":"invalid request, expected {\"name\":\"x\",\"new_name\":\"y\"}"})";
        return error::BAD_REQUEST;
    }

    const std::string name = doc["name"].GetString();
    const std::string new_name = doc["new_name"].GetString();
    if (Get(name.c_str()) == nullptr) {
        rsp = R"({"error":"channel not found"})";
        return error::NOT_FOUND;
    }
    if (Get(new_name.c_str()) != nullptr) {
        rsp = R"({"error":"target already exists"})";
        return error::CONFLICT;
    }

    if (Rename(name.c_str(), new_name.c_str()) != 0) {
        rsp = R"({"error":"rename failed"})";
        return error::INTERNAL_ERROR;
    }
    rsp = std::string("{\"name\":\"") + new_name + "\"}";
    return error::OK;
}

int32_t CatalogPlugin::HandleDelete(const std::string&, const std::string& req, std::string& rsp) {
    rapidjson::Document doc;
    doc.Parse(req.c_str());
    if (doc.HasParseError() || !doc.IsObject() ||
        !doc.HasMember("name") || !doc["name"].IsString()) {
        rsp = R"({"error":"missing 'name'"})";
        return error::BAD_REQUEST;
    }
    const std::string name = doc["name"].GetString();
    if (Get(name.c_str()) == nullptr) {
        rsp = R"({"error":"channel not found"})";
        return error::NOT_FOUND;
    }
    if (Unregister(name.c_str()) != 0) {
        rsp = R"({"error":"delete failed"})";
        return error::INTERNAL_ERROR;
    }
    rsp = R"({"ok":true})";
    return error::OK;
}

bool CatalogPlugin::ParseOperatorRefFromName(const std::string& full_name, std::string* category, std::string* name) {
    if (!category || !name || full_name.empty()) return false;
    const size_t dot = full_name.find('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= full_name.size()) return false;
    *category = full_name.substr(0, dot);
    *name = full_name.substr(dot + 1);
    return true;
}

bool CatalogPlugin::ParseOperatorRef(const rapidjson::Value& doc, std::string* category, std::string* name) {
    if (!category || !name || !doc.IsObject()) return false;
    if (doc.HasMember("category") && doc["category"].IsString() &&
        doc.HasMember("name") && doc["name"].IsString()) {
        *category = doc["category"].GetString();
        *name = doc["name"].GetString();
        return !category->empty() && !name->empty();
    }
    if (doc.HasMember("name") && doc["name"].IsString()) {
        return ParseOperatorRefFromName(doc["name"].GetString(), category, name);
    }
    return false;
}

int32_t CatalogPlugin::HandleListOperators(const std::string&, const std::string& req, std::string& rsp) {
    rapidjson::Document doc;
    doc.Parse(req.c_str());
    if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("type") || !doc["type"].IsString()) {
        rsp = R"({"error":"invalid request, expected {\"type\":\"builtin|python|cpp\"}"})";
        return error::BAD_REQUEST;
    }
    const std::string type = doc["type"].GetString();
    if (!EqualsIgnoreCase(type, "builtin") && !EqualsIgnoreCase(type, "python") && !EqualsIgnoreCase(type, "cpp")) {
        rsp = R"({"error":"invalid type, expected builtin|python|cpp"})";
        return error::BAD_REQUEST;
    }

    if (EqualsIgnoreCase(type, "cpp")) {
        auto* host = ResolveBinAddonHost();
        if (!host) {
            rsp = R"({"error":"binaddon host is unavailable"})";
            return error::UNAVAILABLE;
        }
        return host->ListCppPlugins(rsp);
    }

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("operators");
    w.StartArray();

    std::lock_guard<std::mutex> lock(mu_);
    if (operator_db_ == nullptr) {
        rsp = R"({"error":"operator catalog db is not initialized"})";
        return error::INTERNAL_ERROR;
    }

    {
        sqlite3_stmt* stmt = nullptr;
        const char* sql =
            "SELECT category, name, type, source, description, position, active, editable, "
            "created_at, updated_at FROM operator_catalog "
            "WHERE type = ?1 COLLATE NOCASE "
            "ORDER BY category ASC, name ASC;";
        if (sqlite3_prepare_v2(operator_db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            rsp = R"({"error":"failed to query operator catalog"})";
            return error::INTERNAL_ERROR;
        }
        sqlite3_bind_text(stmt, 1, type.c_str(), -1, SQLITE_TRANSIENT);

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            auto txt = [stmt](int idx) -> const char* {
                const unsigned char* v = sqlite3_column_text(stmt, idx);
                return v ? reinterpret_cast<const char*>(v) : "";
            };
            w.StartObject();
            w.Key("category");
            w.String(txt(0));
            w.Key("name");
            w.String(txt(1));
            w.Key("full_name");
            std::string full_name = std::string(txt(0)) + "." + txt(1);
            w.String(full_name.c_str());
            w.Key("type");
            w.String(txt(2));
            w.Key("source");
            w.String(txt(3));
            w.Key("description");
            w.String(txt(4));
            w.Key("position");
            w.String(txt(5));
            w.Key("active");
            w.Int(sqlite3_column_int(stmt, 6));
            w.Key("editable");
            int editable = sqlite3_column_int(stmt, 7);
            const std::string row_type = txt(2);
            if (!EqualsIgnoreCase(row_type, "python")) editable = 0;
            w.Int(editable);
            w.Key("created_at");
            w.String(txt(8));
            w.Key("updated_at");
            w.String(txt(9));
            w.EndObject();
        }
        sqlite3_finalize(stmt);
    }

    w.EndArray();
    w.EndObject();
    rsp = buf.GetString();
    return error::OK;
}

int32_t CatalogPlugin::HandleOperatorUpload(const std::string&, const std::string& req, std::string& rsp) {
    rapidjson::Document doc;
    doc.Parse(req.c_str());
    if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("type") || !doc["type"].IsString() ||
        !doc.HasMember("filename") || !doc["filename"].IsString() ||
        !doc.HasMember("tmp_path") || !doc["tmp_path"].IsString()) {
        rsp = R"({"error":"invalid request, expected {\"type\":\"python|cpp\",\"filename\":\"...\",\"tmp_path\":\"...\"}"})";
        return error::BAD_REQUEST;
    }

    const std::string type = doc["type"].GetString();
    const std::string filename = doc["filename"].GetString();
    const std::string tmp_path = doc["tmp_path"].GetString();
    if (!EqualsIgnoreCase(type, "python") && !EqualsIgnoreCase(type, "cpp")) {
        rsp = R"({"error":"type must be python or cpp"})";
        return error::BAD_REQUEST;
    }
    if (!IsSafeUploadFilename(filename)) {
        rsp = R"({"error":"invalid filename"})";
        return error::BAD_REQUEST;
    }
    const fs::path src(tmp_path);
    if (!fs::exists(src) || !fs::is_regular_file(src)) {
        rsp = R"({"error":"tmp_path not found"})";
        return error::BAD_REQUEST;
    }

    if (EqualsIgnoreCase(type, "cpp")) {
        auto* host = ResolveBinAddonHost();
        if (!host) {
            rsp = R"({"error":"binaddon host is unavailable"})";
            return error::UNAVAILABLE;
        }
        return host->UploadCppPlugin(filename, tmp_path, rsp);
    }

    fs::path target_dir = OperatorsDirPath();

    std::error_code ec;
    fs::create_directories(target_dir, ec);
    if (ec) {
        rsp = R"({"error":"failed to create target directory"})";
        return error::INTERNAL_ERROR;
    }

    const fs::path dst = target_dir / filename;
    if (EqualsIgnoreCase(type, "python")) {
        if (fs::exists(dst)) fs::remove(dst, ec);
        if (MoveUploadedFile(src, dst) != 0) {
            rsp = R"({"error":"failed to persist uploaded file"})";
            return error::INTERNAL_ERROR;
        }
        rapidjson::StringBuffer buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(buf);
        w.StartObject();
        w.Key("ok");
        w.Bool(true);
        w.Key("type");
        w.String("python");
        w.Key("filename");
        w.String(filename.c_str());
        w.Key("path");
        w.String(dst.string().c_str());
        w.EndObject();
        rsp = buf.GetString();
        return error::OK;
    }

    rsp = R"({"error":"unexpected type"})";
    return error::BAD_REQUEST;
}

int32_t CatalogPlugin::HandleOperatorDelete(const std::string&, const std::string& req, std::string& rsp) {
    rapidjson::Document doc;
    doc.Parse(req.c_str());
    if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("type") || !doc["type"].IsString()) {
        rsp = R"({"error":"invalid request, expected {\"type\":\"python|cpp\",...}"})";
        return error::BAD_REQUEST;
    }
    const std::string type = doc["type"].GetString();
    if (EqualsIgnoreCase(type, "python")) {
        std::string category;
        std::string name;
        if (!ParseOperatorRef(doc, &category, &name)) {
            rsp = R"({"error":"invalid request, expected {\"type\":\"python\",\"name\":\"category.op\"}"})";
            return error::BAD_REQUEST;
        }

        OperatorMeta meta;
        int active = 0;
        int editable = 0;
        std::string created_at;
        std::string updated_at;
        if (QueryOperatorDetail(category, name, &meta, &active, &editable, &created_at, &updated_at) != 0) {
            rsp = R"({"error":"operator not found"})";
            return error::NOT_FOUND;
        }
        if (!EqualsIgnoreCase(meta.type, "python")) {
            rsp = R"({"error":"delete only supports python operators or cpp plugins"})";
            return error::BAD_REQUEST;
        }
        if (active != 0) {
            rsp = R"({"error":"operator is active, deactivate first"})";
            return error::CONFLICT;
        }

        const std::string content_path = OperatorContentPath(category, name);
        std::error_code ec;
        if (fs::exists(content_path, ec) && !ec) {
            if (!fs::remove(content_path, ec) || ec) {
                rsp = R"({"error":"failed to remove operator file"})";
                return error::INTERNAL_ERROR;
            }
        }

        {
            std::lock_guard<std::mutex> lock(mu_);
            if (operator_db_ == nullptr) {
                rsp = R"({"error":"operator catalog db unavailable"})";
                return error::INTERNAL_ERROR;
            }
            sqlite3_stmt* stmt = nullptr;
            const char* sql =
                "DELETE FROM operator_catalog "
                "WHERE category = ?1 COLLATE NOCASE AND name = ?2 COLLATE NOCASE AND type = 'python';";
            if (sqlite3_prepare_v2(operator_db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
                rsp = R"({"error":"failed to prepare delete"})";
                return error::INTERNAL_ERROR;
            }
            sqlite3_bind_text(stmt, 1, category.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_TRANSIENT);
            const int rc = sqlite3_step(stmt);
            const int changed = sqlite3_changes(operator_db_);
            sqlite3_finalize(stmt);
            if (rc != SQLITE_DONE) {
                rsp = R"({"error":"failed to delete operator"})";
                return error::INTERNAL_ERROR;
            }
            if (changed == 0) {
                rsp = R"({"error":"operator not found"})";
                return error::NOT_FOUND;
            }
        }

        rsp = R"({"ok":true})";
        return error::OK;
    }

    if (!EqualsIgnoreCase(type, "cpp")) {
        rsp = R"({"error":"delete only supports python operators or cpp plugins"})";
        return error::BAD_REQUEST;
    }
    if (!doc.HasMember("plugin_id") || !doc["plugin_id"].IsString()) {
        rsp = R"({"error":"missing plugin_id for cpp delete"})";
        return error::BAD_REQUEST;
    }
    const std::string plugin_id = doc["plugin_id"].GetString();
    if (plugin_id.size() != 64) {
        rsp = R"({"error":"invalid plugin_id"})";
        return error::BAD_REQUEST;
    }
    auto* host = ResolveBinAddonHost();
    if (!host) {
        rsp = R"({"error":"binaddon host is unavailable"})";
        return error::UNAVAILABLE;
    }
    return host->DeleteCppPlugin(plugin_id, rsp);
}

int32_t CatalogPlugin::HandleOperatorDetail(const std::string&, const std::string& req, std::string& rsp) {
    rapidjson::Document doc;
    doc.Parse(req.c_str());
    const std::string type =
        (doc.IsObject() && doc.HasMember("type") && doc["type"].IsString()) ? doc["type"].GetString() : "";
    if (doc.HasParseError() || !doc.IsObject()) {
        rsp = R"({"error":"invalid request"})";
        return error::BAD_REQUEST;
    }
    if (EqualsIgnoreCase(type, "cpp")) {
        if (!doc.HasMember("plugin_id") || !doc["plugin_id"].IsString()) {
            rsp = R"({"error":"invalid request, expected {\"type\":\"cpp\",\"plugin_id\":\"...\"}"})";
            return error::BAD_REQUEST;
        }
        auto* host = ResolveBinAddonHost();
        if (!host) {
            rsp = R"({"error":"binaddon host is unavailable"})";
            return error::UNAVAILABLE;
        }
        return host->GetCppPluginDetail(doc["plugin_id"].GetString(), rsp);
    }

    std::string category;
    std::string name;
    if (!ParseOperatorRef(doc, &category, &name)) {
        rsp = R"({"error":"invalid request, expected {\"name\":\"category.op\"}"})";
        return error::BAD_REQUEST;
    }

    OperatorMeta meta;
    int active = 0;
    int editable = 0;
    std::string created_at;
    std::string updated_at;
    if (QueryOperatorDetail(category, name, &meta, &active, &editable, &created_at, &updated_at) != 0) {
        rsp = R"({"error":"operator not found"})";
        return error::NOT_FOUND;
    }

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("category");
    w.String(meta.category.c_str());
    w.Key("name");
    w.String(meta.name.c_str());
    w.Key("full_name");
    std::string full_name = meta.category + "." + meta.name;
    w.String(full_name.c_str());
    w.Key("type");
    w.String(meta.type.c_str());
    w.Key("source");
    w.String(meta.source.c_str());
    w.Key("description");
    w.String(meta.description.c_str());
    w.Key("position");
    w.String(meta.position.c_str());
    w.Key("active");
    w.Int(active);
    w.Key("editable");
    if (!EqualsIgnoreCase(meta.type, "python")) editable = 0;
    w.Int(editable);
    if (EqualsIgnoreCase(meta.type, "python")) {
        std::string content;
        if (LoadOperatorContent(meta.category, meta.name, &content) == 0) {
            w.Key("content");
            w.String(content.c_str());
        }
    }
    w.Key("created_at");
    w.String(created_at.c_str());
    w.Key("updated_at");
    w.String(updated_at.c_str());
    w.EndObject();
    rsp = buf.GetString();
    return error::OK;
}

int32_t CatalogPlugin::HandleOperatorActivate(const std::string&, const std::string& req, std::string& rsp) {
    rapidjson::Document doc;
    doc.Parse(req.c_str());
    if (doc.HasParseError() || !doc.IsObject()) {
        rsp = R"({"error":"invalid request"})";
        return error::BAD_REQUEST;
    }
    const std::string type =
        (doc.HasMember("type") && doc["type"].IsString()) ? doc["type"].GetString() : "";
    if (EqualsIgnoreCase(type, "cpp")) {
        if (!doc.HasMember("plugin_id") || !doc["plugin_id"].IsString()) {
            rsp = R"({"error":"invalid request, expected {\"type\":\"cpp\",\"plugin_id\":\"...\"}"})";
            return error::BAD_REQUEST;
        }
        auto* host = ResolveBinAddonHost();
        if (!host) {
            rsp = R"({"error":"binaddon host is unavailable"})";
            return error::UNAVAILABLE;
        }
        return host->ActivateCppPlugin(doc["plugin_id"].GetString(), rsp);
    }

    std::string category;
    std::string name;
    if (!ParseOperatorRef(doc, &category, &name)) {
        rsp = R"({"error":"invalid request, expected {\"name\":\"category.op\"}"})";
        return error::BAD_REQUEST;
    }
    if (SetOperatorActive(category, name, 1) != 0) {
        rsp = R"({"error":"operator not found"})";
        return error::NOT_FOUND;
    }
    rsp = R"({"ok":true})";
    return error::OK;
}

int32_t CatalogPlugin::HandleOperatorDeactivate(const std::string&, const std::string& req, std::string& rsp) {
    rapidjson::Document doc;
    doc.Parse(req.c_str());
    if (doc.HasParseError() || !doc.IsObject()) {
        rsp = R"({"error":"invalid request"})";
        return error::BAD_REQUEST;
    }
    const std::string type =
        (doc.HasMember("type") && doc["type"].IsString()) ? doc["type"].GetString() : "";
    if (EqualsIgnoreCase(type, "cpp")) {
        if (!doc.HasMember("plugin_id") || !doc["plugin_id"].IsString()) {
            rsp = R"({"error":"invalid request, expected {\"type\":\"cpp\",\"plugin_id\":\"...\"}"})";
            return error::BAD_REQUEST;
        }
        auto* host = ResolveBinAddonHost();
        if (!host) {
            rsp = R"({"error":"binaddon host is unavailable"})";
            return error::UNAVAILABLE;
        }
        return host->DeactivateCppPlugin(doc["plugin_id"].GetString(), rsp);
    }

    std::string category;
    std::string name;
    if (!ParseOperatorRef(doc, &category, &name)) {
        rsp = R"({"error":"invalid request, expected {\"name\":\"category.op\"}"})";
        return error::BAD_REQUEST;
    }
    if (SetOperatorActive(category, name, 0) != 0) {
        rsp = R"({"error":"operator not found"})";
        return error::NOT_FOUND;
    }
    rsp = R"({"ok":true})";
    return error::OK;
}

int32_t CatalogPlugin::HandleOperatorUpdate(const std::string&, const std::string& req, std::string& rsp) {
    rapidjson::Document doc;
    doc.Parse(req.c_str());
    std::string category;
    std::string name;
    if (doc.HasParseError() || !ParseOperatorRef(doc, &category, &name)) {
        rsp = R"({"error":"invalid request, expected {\"name\":\"category.op\"}"})";
        return error::BAD_REQUEST;
    }

    std::string description;
    std::string position;
    std::string content;
    const std::string* description_ptr = nullptr;
    const std::string* position_ptr = nullptr;
    const std::string* content_ptr = nullptr;
    if (doc.HasMember("description") && doc["description"].IsString()) {
        description = doc["description"].GetString();
        description_ptr = &description;
    }
    if (doc.HasMember("position") && doc["position"].IsString()) {
        position = doc["position"].GetString();
        position_ptr = &position;
    }
    if (doc.HasMember("content") && doc["content"].IsString()) {
        content = doc["content"].GetString();
        content_ptr = &content;
    }
    if (description_ptr == nullptr && position_ptr == nullptr && content_ptr == nullptr) {
        rsp = R"({"error":"at least one of description/position/content is required"})";
        return error::BAD_REQUEST;
    }

    OperatorMeta meta;
    int active = 0;
    int editable = 0;
    std::string created_at;
    std::string updated_at;
    if (QueryOperatorDetail(category, name, &meta, &active, &editable, &created_at, &updated_at) != 0) {
        rsp = R"({"error":"operator not found"})";
        return error::NOT_FOUND;
    }
    if (content_ptr != nullptr && !EqualsIgnoreCase(meta.type, "python")) {
        rsp = R"({"error":"content update only supports python operators"})";
        return error::BAD_REQUEST;
    }

    const int rc = UpdateOperatorFields(category, name, description_ptr, position_ptr);
    if (rc == 1) return error::NOT_FOUND;
    if (rc != 0) {
        rsp = R"({"error":"failed to update operator"})";
        return error::INTERNAL_ERROR;
    }
    if (content_ptr != nullptr && SaveOperatorContent(category, name, *content_ptr) != 0) {
        rsp = R"({"error":"failed to update operator content"})";
        return error::INTERNAL_ERROR;
    }
    rsp = R"({"ok":true})";
    return error::OK;
}

int32_t CatalogPlugin::HandleOperatorUpsertBatch(const std::string&, const std::string& req, std::string& rsp) {
    rapidjson::Document doc;
    doc.Parse(req.c_str());
    if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("operators") || !doc["operators"].IsArray()) {
        rsp = R"({"error":"invalid request, expected {\"operators\":[...]}"})";
        return error::BAD_REQUEST;
    }

    std::vector<OperatorMeta> ops;
    const auto& arr = doc["operators"];
    ops.reserve(arr.Size());
    for (rapidjson::SizeType i = 0; i < arr.Size(); ++i) {
        const auto& it = arr[i];
        const bool has_category = it.IsObject() && it.HasMember("category") && it["category"].IsString();
        if (!has_category || !it.HasMember("name") ||
            !it["name"].IsString() || !it.HasMember("type") || !it["type"].IsString() || !it.HasMember("source") ||
            !it["source"].IsString()) {
            rsp = R"({"error":"operator item must include category/name/type/source"})";
            return error::BAD_REQUEST;
        }
        OperatorMeta meta;
        meta.category = it["category"].GetString();
        meta.name = it["name"].GetString();
        meta.type = it["type"].GetString();
        meta.source = it["source"].GetString();
        if (it.HasMember("description") && it["description"].IsString()) meta.description = it["description"].GetString();
        if (it.HasMember("position") && it["position"].IsString()) meta.position = it["position"].GetString();
        ops.push_back(std::move(meta));
    }

    UpsertResult result = UpsertBatch(ops);
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("success_count");
    w.Int(result.success_count);
    w.Key("failed_count");
    w.Int(result.failed_count);
    w.Key("error_message");
    w.String(result.error_message.c_str());
    w.EndObject();
    rsp = buf.GetString();
    return result.failed_count > 0 ? error::INTERNAL_ERROR : error::OK;
}

int CatalogPlugin::EnsureOperatorCatalogDb() {
    std::lock_guard<std::mutex> lock(mu_);
    if (operator_db_ != nullptr) return 0;

    if (EnsureOperatorDbDir() != 0) return -1;
    const std::string db_path = OperatorCatalogDbPath();
    if (sqlite3_open(db_path.c_str(), &operator_db_) != SQLITE_OK) {
        if (operator_db_ != nullptr) {
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
    return EnsureOperatorCatalogSchema();
}

int CatalogPlugin::EnsureOperatorCatalogSchema() {
    if (operator_db_ == nullptr) return -1;
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

    // 补 plugin_id 列（仅支持 category 字段，不再保留旧别名）
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

    return 0;
}

std::string CatalogPlugin::OperatorCatalogDbPath() const {
    if (!operator_db_path_.empty()) return operator_db_path_;
    fs::path p(operator_db_dir_);
    p /= "operator_catalog.db";
    return p.string();
}

int CatalogPlugin::QueryOperatorDetail(const std::string& category,
                                       const std::string& name,
                                       OperatorMeta* meta,
                                       int* active,
                                       int* editable,
                                       std::string* created_at,
                                       std::string* updated_at) {
    if (!meta || !active || !editable || !created_at || !updated_at) return -1;

    std::lock_guard<std::mutex> lock(mu_);
    if (operator_db_ == nullptr) return -1;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT category, name, type, source, description, position, active, editable, created_at, updated_at "
        "FROM operator_catalog WHERE category = ?1 COLLATE NOCASE AND name = ?2 COLLATE NOCASE LIMIT 1;";
    if (sqlite3_prepare_v2(operator_db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, category.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return -1;
    }

    auto txt = [stmt](int idx) -> std::string {
        const unsigned char* v = sqlite3_column_text(stmt, idx);
        return v ? reinterpret_cast<const char*>(v) : "";
    };
    meta->category = txt(0);
    meta->name = txt(1);
    meta->type = txt(2);
    meta->source = txt(3);
    meta->description = txt(4);
    meta->position = txt(5);
    *active = sqlite3_column_int(stmt, 6);
    *editable = sqlite3_column_int(stmt, 7);
    *created_at = txt(8);
    *updated_at = txt(9);
    sqlite3_finalize(stmt);
    return 0;
}

int CatalogPlugin::SetOperatorActive(const std::string& category, const std::string& name, int active) {
    std::lock_guard<std::mutex> lock(mu_);
    if (operator_db_ == nullptr) return -1;

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "UPDATE operator_catalog SET active = ?1, updated_at = CURRENT_TIMESTAMP "
        "WHERE category = ?2 COLLATE NOCASE AND name = ?3 COLLATE NOCASE;";
    if (sqlite3_prepare_v2(operator_db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return -1;
    sqlite3_bind_int(stmt, 1, active);
    sqlite3_bind_text(stmt, 2, category.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, name.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(stmt);
    const int changed = sqlite3_changes(operator_db_);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -1;
    return changed > 0 ? 0 : -1;
}

int CatalogPlugin::UpdateOperatorFields(const std::string& category,
                                        const std::string& name,
                                        const std::string* description,
                                        const std::string* position) {
    std::lock_guard<std::mutex> lock(mu_);
    if (operator_db_ == nullptr) return -1;

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "UPDATE operator_catalog SET "
        "description = CASE WHEN ?1 IS NULL THEN description ELSE ?1 END, "
        "position = CASE WHEN ?2 IS NULL THEN position ELSE ?2 END, "
        "updated_at = CURRENT_TIMESTAMP "
        "WHERE category = ?3 COLLATE NOCASE AND name = ?4 COLLATE NOCASE;";
    if (sqlite3_prepare_v2(operator_db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return -1;

    if (description != nullptr) {
        sqlite3_bind_text(stmt, 1, description->c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, 1);
    }
    if (position != nullptr) {
        sqlite3_bind_text(stmt, 2, position->c_str(), -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, 2);
    }
    sqlite3_bind_text(stmt, 3, category.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, name.c_str(), -1, SQLITE_TRANSIENT);

    const int rc = sqlite3_step(stmt);
    const int changed = sqlite3_changes(operator_db_);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -1;
    return changed > 0 ? 0 : 1;
}

std::string CatalogPlugin::OperatorsDirPath() const {
    char exe_path[1024];
    ssize_t len = ::readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len > 0) {
        exe_path[len] = '\0';
        std::string exe_dir(exe_path);
        size_t pos = exe_dir.find_last_of('/');
        if (pos != std::string::npos) return exe_dir.substr(0, pos) + "/operators";
    }
    return "operators";
}

std::string CatalogPlugin::OperatorContentPath(const std::string& category, const std::string& name) const {
    fs::path p(OperatorsDirPath());
    p /= (category + "_" + name + ".py");
    return p.string();
}

int CatalogPlugin::LoadOperatorContent(const std::string& category, const std::string& name, std::string* content) const {
    if (!content) return -1;
    const std::string path = OperatorContentPath(category, name);
    std::ifstream ifs(path, std::ios::in | std::ios::binary);
    if (!ifs.is_open()) {
        LOG_ERROR("CatalogPlugin::LoadOperatorContent: open failed path=%s errno=%d", path.c_str(), errno);
        return -1;
    }
    std::ostringstream ss;
    ss << ifs.rdbuf();
    *content = ss.str();
    return 0;
}

int CatalogPlugin::SaveOperatorContent(const std::string& category, const std::string& name, const std::string& content) const {
    std::error_code ec;
    fs::create_directories(OperatorsDirPath(), ec);
    std::ofstream ofs(OperatorContentPath(category, name), std::ios::out | std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) return -1;
    ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
    return ofs.good() ? 0 : -1;
}

std::string CatalogPlugin::GenerateImportName(const std::string& filename) {
    std::string base = filename;
    auto slash = base.find_last_of("/\\");
    if (slash != std::string::npos) base = base.substr(slash + 1);
    if (base.size() >= 4 && base.substr(base.size() - 4) == ".csv") base.resize(base.size() - 4);
    if (base.empty()) base = "imported";

    std::string name = base;
    if (Get(name.c_str()) == nullptr) return name;

    char ts[32] = {0};
    std::time_t now = std::time(nullptr);
    std::tm tm_now;
#ifdef _WIN32
    localtime_s(&tm_now, &now);
#else
    localtime_r(&now, &tm_now);
#endif
    std::strftime(ts, sizeof(ts), "%Y%m%d%H%M%S", &tm_now);
    return base + "_" + ts;
}

const char* CatalogPlugin::DataTypeName(DataType t) {
    switch (t) {
        case DataType::INT32: return "INT32";
        case DataType::INT64: return "INT64";
        case DataType::UINT32: return "UINT32";
        case DataType::UINT64: return "UINT64";
        case DataType::FLOAT: return "FLOAT";
        case DataType::DOUBLE: return "DOUBLE";
        case DataType::STRING: return "STRING";
        case DataType::BYTES: return "BYTES";
        case DataType::TIMESTAMP: return "TIMESTAMP";
        case DataType::BOOLEAN: return "BOOLEAN";
        default: return "UNKNOWN";
    }
}

int CatalogPlugin::EnsureDataDir() {
    std::error_code ec;
    fs::create_directories(data_dir_, ec);
    return ec ? -1 : 0;
}

int CatalogPlugin::EnsureOperatorDbDir() const {
    if (!operator_db_path_.empty()) {
        fs::path p(operator_db_path_);
        fs::path parent = p.parent_path();
        if (parent.empty()) return 0;
        std::error_code ec;
        fs::create_directories(parent, ec);
        return ec ? -1 : 0;
    }
    std::error_code ec;
    fs::create_directories(operator_db_dir_, ec);
    return ec ? -1 : 0;
}

std::string CatalogPlugin::CsvPath(const std::string& name) const {
    fs::path p(data_dir_);
    p /= name + ".csv";
    return p.string();
}

int CatalogPlugin::PersistChannelToCsv(const std::string& name, IDataFrameChannel* channel) {
    if (!channel) return -1;
    if (EnsureDataDir() != 0) return -1;

    DataFrame data;
    if (channel->Read(&data) != 0) return -1;

    std::ofstream out(CsvPath(name), std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return -1;

    const auto schema = data.GetSchema();
    for (size_t i = 0; i < schema.size(); ++i) {
        if (i > 0) out << ",";
        out << EscapeCsvField(schema[i].name);
    }
    out << "\n";

    for (int32_t r = 0; r < data.RowCount(); ++r) {
        const auto row = data.GetRow(r);
        for (size_t c = 0; c < row.size(); ++c) {
            if (c > 0) out << ",";
            out << EscapeCsvField(FieldValueToString(row[c]));
        }
        out << "\n";
    }
    return out.good() ? 0 : -1;
}

std::shared_ptr<IDataFrameChannel> CatalogPlugin::LoadCsvFile(const std::string& path, const std::string& name) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return nullptr;

    std::string line;
    if (!std::getline(in, line)) return nullptr;
    if (!line.empty() && line.back() == '\r') line.pop_back();

    auto headers = ParseCsvLine(line);
    if (headers.empty()) return nullptr;
    // 去掉 UTF-8 BOM，避免首列表头显示为空/异常字符
    if (!headers[0].empty() && headers[0].size() >= 3 &&
        static_cast<unsigned char>(headers[0][0]) == 0xEF &&
        static_cast<unsigned char>(headers[0][1]) == 0xBB &&
        static_cast<unsigned char>(headers[0][2]) == 0xBF) {
        headers[0] = headers[0].substr(3);
    }
    // 空表头兜底命名，避免前端出现空列名
    for (size_t i = 0; i < headers.size(); ++i) {
        if (headers[i].empty()) headers[i] = "col_" + std::to_string(i + 1);
    }

    std::vector<std::vector<std::string>> rows;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto row = ParseCsvLine(line);
        if (row.size() < headers.size()) row.resize(headers.size());
        if (row.size() > headers.size()) row.resize(headers.size());
        rows.push_back(std::move(row));
    }

    std::vector<Field> schema;
    schema.reserve(headers.size());
    for (size_t c = 0; c < headers.size(); ++c) {
        bool all_int64 = !rows.empty();
        bool all_double = !rows.empty();

        for (const auto& row : rows) {
            const auto& cell = row[c];
            if (cell.empty()) {
                all_int64 = false;
                all_double = false;
                break;
            }
            int64_t i64 = 0;
            double d = 0.0;
            if (!TryParseInt64(cell, &i64)) all_int64 = false;
            if (!TryParseDouble(cell, &d)) all_double = false;
        }

        DataType type = DataType::STRING;
        if (all_int64) type = DataType::INT64;
        else if (all_double) type = DataType::DOUBLE;
        schema.push_back({headers[c], type, 0, ""});
    }

    DataFrame df;
    df.SetSchema(schema);
    for (const auto& row : rows) {
        std::vector<FieldValue> values;
        values.reserve(schema.size());
        for (size_t c = 0; c < schema.size(); ++c) {
            const auto& cell = row[c];
            if (schema[c].type == DataType::INT64) {
                int64_t v = 0;
                if (!TryParseInt64(cell, &v)) return nullptr;
                values.push_back(v);
            } else if (schema[c].type == DataType::DOUBLE) {
                double v = 0.0;
                if (!TryParseDouble(cell, &v)) return nullptr;
                values.push_back(v);
            } else {
                values.push_back(cell);
            }
        }
        if (df.AppendRow(values) != 0) return nullptr;
    }

    auto ch = std::make_shared<DataFrameChannel>("dataframe", name);
    ch->Open();
    if (ch->Write(&df) != 0) return nullptr;
    return ch;
}

std::vector<std::string> CatalogPlugin::ParseCsvLine(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool in_quotes = false;

    for (size_t i = 0; i < line.size(); ++i) {
        const char ch = line[i];
        if (in_quotes) {
            if (ch == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') {
                    cur.push_back('"');
                    ++i;
                } else {
                    in_quotes = false;
                }
            } else {
                cur.push_back(ch);
            }
        } else {
            if (ch == ',') {
                out.push_back(cur);
                cur.clear();
            } else if (ch == '"') {
                in_quotes = true;
            } else {
                cur.push_back(ch);
            }
        }
    }
    out.push_back(cur);
    return out;
}

std::string CatalogPlugin::EscapeCsvField(const std::string& field) {
    bool need_quotes = false;
    for (char ch : field) {
        if (ch == ',' || ch == '"' || ch == '\n' || ch == '\r') {
            need_quotes = true;
            break;
        }
    }
    if (!need_quotes) return field;

    std::string out;
    out.reserve(field.size() + 2);
    out.push_back('"');
    for (char ch : field) {
        if (ch == '"') out.push_back('"');
        out.push_back(ch);
    }
    out.push_back('"');
    return out;
}

bool CatalogPlugin::TryParseInt64(const std::string& s, int64_t* out) {
    if (s.empty() || !out) return false;
    const char* begin = s.data();
    const char* end = s.data() + s.size();
    auto [ptr, ec] = std::from_chars(begin, end, *out);
    return ec == std::errc() && ptr == end;
}

bool CatalogPlugin::TryParseDouble(const std::string& s, double* out) {
    if (s.empty() || !out) return false;
    errno = 0;
    char* endptr = nullptr;
    *out = std::strtod(s.c_str(), &endptr);
    return errno == 0 && endptr == s.c_str() + s.size();
}

std::string CatalogPlugin::FieldValueToString(const FieldValue& v) {
    return std::visit(
        [](auto&& val) -> std::string {
            using T = std::decay_t<decltype(val)>;
            if constexpr (std::is_same_v<T, int32_t>) return std::to_string(val);
            if constexpr (std::is_same_v<T, int64_t>) return std::to_string(val);
            if constexpr (std::is_same_v<T, uint32_t>) return std::to_string(val);
            if constexpr (std::is_same_v<T, uint64_t>) return std::to_string(val);
            if constexpr (std::is_same_v<T, float>) return std::to_string(val);
            if constexpr (std::is_same_v<T, double>) return std::to_string(val);
            if constexpr (std::is_same_v<T, std::string>) return val;
            if constexpr (std::is_same_v<T, std::vector<uint8_t>>) {
                return std::string(val.begin(), val.end());
            }
            if constexpr (std::is_same_v<T, bool>) return val ? "true" : "false";
            return std::string();
        },
        v);
}

}  // namespace catalog
}  // namespace flowsql
