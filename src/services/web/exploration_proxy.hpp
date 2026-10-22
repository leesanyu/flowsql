// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#pragma once
#include <httplib.h>

#include <regex>
#include <string>

namespace flowsql::web {
inline bool ValidExplorationUpstream(const std::string& upstream) {
    if (upstream.empty()) return true;
    if (!std::regex_match(upstream, std::regex(R"(http://[a-zA-Z0-9_.-]+:[0-9]{1,5})"))) return false;
    const int port = std::stoi(upstream.substr(upstream.rfind(':') + 1));
    return port > 0 && port < 65536;
}

inline bool IsExplorationPath(const std::string& path) {
    return path == "/exploration" || path.rfind("/exploration/", 0) == 0 ||
           path.rfind("/api/visualization/v1/", 0) == 0;
}

inline void ProxyExploration(const std::string& upstream, const httplib::Request& req, httplib::Response& res) {
    if (upstream.empty()) {
        res.status = 503;
        res.set_content("未配置数据探索服务", "text/plain; charset=utf-8");
        return;
    }
    const bool asset = req.path == "/exploration" || req.path.rfind("/exploration/", 0) == 0;
    const bool api_get = req.path == "/api/visualization/v1/capabilities" ||
                         req.path == "/api/visualization/v1/sources" ||
                         req.path.rfind("/api/visualization/v1/sources/", 0) == 0;
    const bool api_post = req.path == "/api/visualization/v1/compute" || req.path == "/api/visualization/v1/cancel";
    if (!(((asset || api_get) && (req.method == "GET" || req.method == "HEAD")) ||
          (api_post && req.method == "POST"))) {
        res.status = 405;
        return;
    }
    httplib::Client client(upstream);
    client.set_connection_timeout(2);
    client.set_read_timeout(310);
    client.set_write_timeout(5);
    client.set_decompress(false);
    httplib::Request forward;
    forward.method = req.method;
    forward.path = req.target;
    forward.body = req.body;
    for (const auto* name :
         {"Content-Type", "Accept", "Accept-Encoding", "If-None-Match", "If-Modified-Since", "Range"})
        if (req.has_header(name)) forward.set_header(name, req.get_header_value(name));
    auto result = client.send(forward);
    if (!result) {
        res.status = 503;
        res.set_content("数据探索服务暂不可用", "text/plain; charset=utf-8");
        return;
    }
    res.status = result->status;
    res.body = result->body;
    if (req.method == "HEAD" && result->has_header("Content-Length"))
        res.set_header("Content-Length", result->get_header_value("Content-Length"));
    for (const auto* name : {"Content-Type", "Content-Encoding", "Cache-Control", "ETag", "Last-Modified", "Expires",
                             "Vary", "Content-Range", "Accept-Ranges", "Content-Security-Policy", "X-Frame-Options"})
        if (result->has_header(name)) res.set_header(name, result->get_header_value(name));
}
}  // namespace flowsql::web
