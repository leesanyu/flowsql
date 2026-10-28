// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <httplib.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <common/json_depth.hpp>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace flowsql {
inline constexpr const char* kMetadataDeadlineHeader = "X-FlowSQL-Metadata-Deadline-Ms";
inline bool MetadataPath(const std::string& path) {
    return path.find("/semantics/v1/channels/") != std::string::npos ||
           path.find("/dataset/describe") != std::string::npos;
}
inline int64_t MetadataNow() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}
// Only valid metadata requests acquire a budget; malformed requests keep their ordinary schema error.
inline int64_t MetadataDeadline(const std::string& body, const std::string& inherited = "") {
    if (!JsonNestingWithin(body, 64)) return 0;
    rapidjson::Document d;
    d.Parse(body.c_str());
    if (!d.IsObject()) return 0;
    uint32_t budget = 5000;
    if (d.HasMember("timeout_ms")) {
        if (!d["timeout_ms"].IsUint() || !d["timeout_ms"].GetUint() || d["timeout_ms"].GetUint() > 30000) return 0;
        budget = d["timeout_ms"].GetUint();
    }
    const auto deadline = MetadataNow() + budget;
    int64_t upstream = 0;
    const auto parsed = std::from_chars(inherited.data(), inherited.data() + inherited.size(), upstream);
    return parsed.ec == std::errc{} && parsed.ptr == inherited.data() + inherited.size() && upstream > 0
               ? std::min(deadline, upstream)
               : deadline;
}
inline bool MetadataRemaining(std::string* body, int64_t deadline) {
    if (!deadline) return true;
    const auto remaining = deadline - MetadataNow();
    if (remaining <= 0) return false;
    rapidjson::Document d;
    d.Parse(body->c_str());
    const auto ms = static_cast<unsigned>(std::min<int64_t>(remaining, 30000));
    if (d.HasMember("timeout_ms"))
        d["timeout_ms"].SetUint(ms);
    else
        d.AddMember("timeout_ms", ms, d.GetAllocator());
    rapidjson::StringBuffer b;
    rapidjson::Writer<rapidjson::StringBuffer> w(b);
    d.Accept(w);
    *body = b.GetString();
    return true;
}
inline const char* MetadataTimeoutJson() {
    return R"({"error":{"code":"SOURCE_TIMEOUT","message":"metadata request deadline exceeded","field_path":"","details":{}}})";
}
// Control requests are infrequent. The timer bounds connect + write + read together, including silent peers.
class MetadataHttpBudget {
 public:
    MetadataHttpBudget(httplib::Client& client, int64_t deadline) {
        if (!deadline) return;
        timer_ = std::thread([this, &client, deadline] {
            std::unique_lock<std::mutex> lock(mutex_);
            if (!changed_.wait_until(lock, std::chrono::system_clock::time_point(std::chrono::milliseconds(deadline)),
                                     [this] { return done_; })) {
                lock.unlock();
                client.stop();
            }
        });
    }
    ~MetadataHttpBudget() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            done_ = true;
        }
        changed_.notify_all();
        if (timer_.joinable()) timer_.join();
    }

 private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool done_ = false;
    std::thread timer_;
};
}  // namespace flowsql
