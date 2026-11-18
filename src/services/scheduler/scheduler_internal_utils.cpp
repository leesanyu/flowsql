// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "scheduler_internal_utils.h"

#include <common/error_code.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <regex>

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace flowsql {
namespace scheduler {

std::string ToLowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

bool IEquals(const std::string& a, const std::string& b) {
    return ToLowerAscii(a) == ToLowerAscii(b);
}

bool StartsWithIgnoreCase(const std::string& s, const std::string& prefix) {
    if (s.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(s[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

bool IsDataframeRefName(const std::string& name) {
    return StartsWithIgnoreCase(name, "dataframe.") && name.size() > std::strlen("dataframe.");
}

std::string DataframeNamePart(const std::string& name) {
    if (!IsDataframeRefName(name)) return "";
    return name.substr(std::strlen("dataframe."));
}

bool IsStreamRefName(const std::string& name) {
    return StartsWithIgnoreCase(name, "stream.") && name.size() > std::strlen("stream.");
}

std::string StreamNamePart(const std::string& name) {
    if (!IsStreamRefName(name)) return "";
    return name.substr(std::strlen("stream."));
}

bool ParseChannelRef(const std::string& raw, ParsedChannelRef* out, std::string* err) {
    if (!out) return false;
    if (err) err->clear();
    out->raw = raw;
    out->base.clear();
    out->has_selector = false;
    out->wildcard_selector = false;
    out->selector_index = -1;

    if (raw.empty()) {
        if (err) *err = "empty channel reference";
        return false;
    }

    const auto lb = raw.find('[');
    if (lb == std::string::npos) {
        out->base = raw;
        return true;
    }

    const auto rb = raw.find(']', lb + 1);
    if (rb == std::string::npos) {
        if (err) *err = "invalid channel selector: missing ']'";
        return false;
    }
    if (raw.find('[', rb + 1) != std::string::npos || rb + 1 != raw.size()) {
        if (err) *err = "invalid channel selector: duplicate selector is not allowed";
        return false;
    }

    out->base = raw.substr(0, lb);
    if (out->base.empty()) {
        if (err) *err = "invalid channel selector: empty base channel";
        return false;
    }
    const std::string selector = raw.substr(lb + 1, rb - lb - 1);
    out->has_selector = true;
    if (selector == "*") {
        out->wildcard_selector = true;
        return true;
    }
    if (selector.empty()) {
        if (err) *err = "invalid channel selector: expected '*' or index";
        return false;
    }
    for (char c : selector) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            if (err) *err = "invalid channel selector: expected '*' or index";
            return false;
        }
    }
    out->selector_index = std::atoi(selector.c_str());
    return true;
}

bool ParseDatabaseDestination(const std::string& dest,
                              std::string* db_type,
                              std::string* db_name,
                              std::string* table_name) {
    if (!db_type || !db_name || !table_name) return false;
    db_type->clear();
    db_name->clear();
    table_name->clear();

    const auto first = dest.find('.');
    if (first == std::string::npos || first == 0 || first >= dest.size() - 1) {
        return false;
    }
    *db_type = ToLowerAscii(dest.substr(0, first));

    const auto second = dest.find('.', first + 1);
    if (second == std::string::npos) {
        *db_name = dest.substr(first + 1);
        return !db_name->empty();
    }

    if (second == first + 1 || second >= dest.size() - 1) {
        return false;
    }
    *db_name = dest.substr(first + 1, second - first - 1);
    *table_name = dest.substr(second + 1);
    return !db_name->empty() && !table_name->empty();
}

bool IsQualifiedDestination(const std::string& dest) {
    // 合法目标：
    // 1) dataframe.<name>
    // 2) type.name 或 type.name.table
    if (dest.empty()) return false;
    if (IsDataframeRefName(dest)) return true;
    const auto first = dest.find('.');
    if (first == std::string::npos || first == 0 || first == dest.size() - 1) return false;
    const auto second = dest.find('.', first + 1);
    if (second == first + 1) return false;
    if (second != std::string::npos && second == dest.size() - 1) return false;
    return true;
}

int32_t MapStreamManagerErrorToStatus(int rc) {
    if (rc == 0) return error::OK;
    if (rc == EEXIST || rc == EBUSY) return error::CONFLICT;
    if (rc == ENOENT) return error::NOT_FOUND;
    if (rc == EINVAL) return error::BAD_REQUEST;
    if (rc == ENOTSUP) return error::BAD_REQUEST;
    return error::INTERNAL_ERROR;
}

std::string MakeStreamChannelKey(const std::string& type, const std::string& name) {
    return ToLowerAscii(type) + "." + name;
}

std::string NormalizeStreamRole(const std::string& role) {
    const std::string lower = ToLowerAscii(role);
    if (lower == "source" || lower == "sink" || lower == "both") return lower;
    return "";
}

bool IsSourceRoleAllowed(const std::string& role) {
    const std::string normalized = NormalizeStreamRole(role);
    return normalized == "source" || normalized == "both";
}

bool IsSinkRoleAllowed(const std::string& role) {
    const std::string normalized = NormalizeStreamRole(role);
    return normalized == "sink" || normalized == "both";
}

int ParseOptionObject(const std::string& option, rapidjson::Document* out, std::string* err) {
    out->Parse(option.c_str());
    if (out->HasParseError() || !out->IsObject()) {
        if (err) *err = "option must be a JSON object";
        return EINVAL;
    }
    return 0;
}

std::string OptionObjectToJson(const rapidjson::Value& option_obj) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    option_obj.Accept(w);
    return buf.GetString();
}

std::string BuildOptionWithRoleJson(const rapidjson::Value* options, const std::string& role) {
    rapidjson::Document d;
    d.SetObject();
    auto& alloc = d.GetAllocator();
    if (options && options->IsObject()) {
        for (auto it = options->MemberBegin(); it != options->MemberEnd(); ++it) {
            rapidjson::Value key;
            key.SetString(it->name.GetString(), alloc);
            rapidjson::Value value;
            value.CopyFrom(it->value, alloc);
            d.AddMember(key, value, alloc);
        }
    }
    rapidjson::Value role_key("role", alloc);
    rapidjson::Value role_value(role.c_str(), alloc);
    if (d.HasMember("role")) {
        d["role"] = role_value;
    } else {
        d.AddMember(role_key, role_value, alloc);
    }
    return OptionObjectToJson(d);
}

std::string ReadRoleFromOption(const std::string& option) {
    rapidjson::Document d;
    std::string parse_err;
    if (ParseOptionObject(option, &d, &parse_err) != 0 || !d.IsObject()) {
        return "both";
    }
    if (!d.HasMember("role") || !d["role"].IsString()) return "both";
    const std::string role = NormalizeStreamRole(d["role"].GetString());
    return role.empty() ? "both" : role;
}

std::string ExtractStageFromExecutionError(const std::string& error) {
    // Pipeline::Run 失败消息：operator <category>.<name> execution failed
    static const std::regex kPattern(R"(^operator\s+([^.]+)\.([^\s]+)\s+execution failed$)",
                                     std::regex_constants::icase);
    std::smatch m;
    if (!std::regex_match(error, m, kPattern)) return "";
    if (m.size() < 3) return "";
    return m[2].str();
}

int64_t CurrentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

size_t NextPowerOfTwo(size_t value) {
    if (value <= 1) return 1;
    size_t v = value - 1;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    if (sizeof(size_t) >= 8) {
        v |= v >> 32;
    }
    return v + 1;
}

std::string TrimAsciiSpace(const std::string& input) {
    size_t first = 0;
    while (first < input.size() &&
           (input[first] == ' ' || input[first] == '\t' ||
            input[first] == '\r' || input[first] == '\n')) {
        ++first;
    }
    if (first >= input.size()) return "";
    size_t last = input.size();
    while (last > first &&
           (input[last - 1] == ' ' || input[last - 1] == '\t' ||
            input[last - 1] == '\r' || input[last - 1] == '\n')) {
        --last;
    }
    return input.substr(first, last - first);
}

std::string ExtractErrorMessage(const std::string& json) {
    rapidjson::Document d;
    d.Parse(json.c_str());
    if (d.HasParseError() || !d.IsObject() || !d.HasMember("error") || !d["error"].IsString()) {
        return "";
    }
    return d["error"].GetString();
}

}  // namespace scheduler
}  // namespace flowsql
