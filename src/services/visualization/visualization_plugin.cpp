// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "visualization_plugin.h"

#include <openssl/sha.h>
#include <rapidjson/document.h>

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace flowsql::visualization {
namespace {
std::string Read(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

bool ValidPackage(const std::string& directory) {
    rapidjson::Document manifest;
    manifest.Parse(Read(std::filesystem::path(directory) / "manifest.json").c_str());
    if (!manifest.IsObject() || !manifest.HasMember("api_version") || manifest["api_version"] != 1 ||
        !manifest.HasMember("bridge_version") || manifest["bridge_version"] != 1 ||
        !manifest.HasMember("workflow_version") || !manifest["workflow_version"].IsString() ||
        std::string(manifest["workflow_version"].GetString()) != "graphic-walker-0.5.3" ||
        !manifest.HasMember("build") || !manifest["build"].IsObject() || !manifest.HasMember("files") ||
        !manifest["files"].IsObject() || !manifest["files"].HasMember("index.html"))
        return false;
    const auto& build = manifest["build"];
    if (!build.HasMember("version") || !build["version"].IsString() || !build.HasMember("revision") ||
        (!build["revision"].IsNull() && !build["revision"].IsString()) || !build.HasMember("dirty") ||
        (!build["dirty"].IsNull() && !build["dirty"].IsBool()))
        return false;
    for (auto it = manifest["files"].MemberBegin(); it != manifest["files"].MemberEnd(); ++it) {
        const std::filesystem::path file(it->name.GetString());
        if (file.is_absolute() || !it->value.IsString()) return false;
        for (const auto& part : file)
            if (part == "..") return false;
        const auto path = std::filesystem::path(directory) / file;
        if (!std::filesystem::is_regular_file(path)) return false;
        const auto content = Read(path);
        unsigned char digest[SHA256_DIGEST_LENGTH];
        SHA256(reinterpret_cast<const unsigned char*>(content.data()), content.size(), digest);
        std::ostringstream hex;
        for (auto byte : digest) hex << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(byte);
        if (hex.str() != it->value.GetString()) return false;
    }
    return true;
}
}  // namespace

int VisualizationPlugin::Option(const char* arg) {
    if (!arg || !*arg) return 0;
    rapidjson::Document options;
    options.Parse(arg);
    if (!options.IsObject()) return -1;
    for (auto it = options.MemberBegin(); it != options.MemberEnd(); ++it) {
        const std::string key = it->name.GetString();
        if (key == "host" && it->value.IsString())
            options_.host = it->value.GetString();
        else if (key == "port" && it->value.IsInt() && it->value.GetInt() > 0 && it->value.GetInt() < 65536)
            options_.port = it->value.GetInt();
        else if (key == "static_dir" && it->value.IsString())
            options_.static_dir = it->value.GetString();
        else if (key == "dataframe_upstream" && it->value.IsString())
            options_.dataframe_upstream = it->value.GetString();
        else if (key == "dataframe_shared_dir" && it->value.IsString())
            options_.dataframe_shared_dir = it->value.GetString();
        else if (key == "limits" && it->value.IsObject()) {
            for (auto limit = it->value.MemberBegin(); limit != it->value.MemberEnd(); ++limit) {
                if (!limit->value.IsUint() || !limit->value.GetUint()) return -1;
                const std::string name = limit->name.GetString();
                const auto value = limit->value.GetUint();
                if (name == "max_rows" && value < 1000000)
                    limits_.max_rows = value;
                else if (name == "max_bytes")
                    limits_.max_bytes = value;
                else if (name == "timeout_ms" && value <= 300000)
                    limits_.timeout_ms = value;
                else if (name == "max_active" && value <= 8)
                    limits_.max_active = value;
                else
                    return -1;
            }
        } else
            return -1;
    }
    return 0;
}

int VisualizationPlugin::Load(IQuerier* querier) {
    querier_ = querier;
    return 0;
}

int VisualizationPlugin::Start() {
    if (!ValidPackage(options_.static_dir)) return -1;
    server_.new_task_queue = [this] { return new httplib::ThreadPool(limits_.max_active + 4); };
    if (!service_.Init(querier_, limits_, options_.dataframe_upstream, options_.dataframe_shared_dir)) return -1;
    service_.Routes(server_);
    server_.Get("/api/visualization/v1/capabilities", [](const auto&, auto& res) {
        res.set_header("Cache-Control", "no-store");
        res.set_content(
            R"({"api_version":1,"workflow_version":"graphic-walker-0.5.3","dialects":["sqlite","mysql","postgres","clickhouse"]})",
            "application/json");
    });
    server_.set_pre_routing_handler([this](const auto& req, auto& res) {
        res.set_header("Content-Security-Policy", "frame-ancestors 'self'");
        res.set_header("X-Frame-Options", "SAMEORIGIN");
        if (req.path == "/exploration/" || req.path == "/exploration/index.html" ||
            req.path == "/exploration/manifest.json") {
            res.set_header("Cache-Control", "no-store");
            if (!ValidPackage(options_.static_dir)) {
                res.status = 409;
                res.set_content("数据探索静态包损坏或版本不兼容", "text/plain; charset=utf-8");
                return httplib::Server::HandlerResponse::Handled;
            }
        } else if (req.path.rfind("/exploration/assets/", 0) == 0) {
            res.set_header("Cache-Control", "public, max-age=31536000, immutable");
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });
    if (!server_.set_mount_point("/exploration", options_.static_dir)) return -1;
    if (!server_.bind_to_port(options_.host, options_.port)) return -1;
    service_.Start();
    thread_ = std::thread([this] { server_.listen_after_bind(); });
    return 0;
}

int VisualizationPlugin::Stop() {
    server_.stop();
    service_.Stop();
    if (thread_.joinable()) thread_.join();
    return 0;
}
int VisualizationPlugin::Unload() { return 0; }
}  // namespace flowsql::visualization

BEGIN_PLUGIN_REGIST(flowsql::visualization::VisualizationPlugin)
____INTERFACE(flowsql::IID_PLUGIN, flowsql::IPlugin)
____INTERFACE(flowsql::IID_PLUGIN_EXTERNAL_ENTRY, flowsql::IPlugin)
END_PLUGIN_REGIST()
