#include "catalog_plugin.h"

#include <framework/core/dataframe.h>
#include <framework/core/dataframe_channel.h>
#include <framework/core/passthrough_operator.h>

#include <common/error_code.h>

#include <cerrno>
#include <charconv>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <type_traits>
#include <utility>
#include <vector>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace fs = std::filesystem;

namespace flowsql {
namespace catalog {

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
        pos = (end < opts.size()) ? end + 1 : opts.size();
    }
    return 0;
}

int CatalogPlugin::Load(IQuerier* querier) {
    querier_ = querier;
    return Register("passthrough", []() -> IOperator* { return new PassthroughOperator(); });
}

int CatalogPlugin::Unload() {
    Stop();
    return 0;
}

int CatalogPlugin::Start() {
    if (EnsureDataDir() != 0) return -1;

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
    channels_.clear();
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
