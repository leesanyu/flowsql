// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "builtin_registry.h"

#include <framework/builtin/dataframe/concat_operator.h>
#include <framework/builtin/dataframe/dataframe_dispatch_stream_operator.h>
#include <framework/builtin/dataframe/hstack_operator.h>
#include <framework/builtin/dataframe/passthrough_operator.h>
#include <framework/builtin/stream/count_window_stream_operator.h>
#include <framework/builtin/stream/passthrough_stream_operator.h>
#include <framework/builtin/stream/tcp_service_merge_stream_operator.h>
#include <framework/builtin/stream/tcp_session_mock_stream_channel.h>
#include <framework/core/ring_stream_channel.h>
#include <framework/core/stream_hub_channel.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace flowsql {
namespace {

std::string ToLowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

bool IsPowerOfTwo(uint64_t value) {
    return value > 0 && ((value & (value - 1)) == 0);
}

std::string BuildNormalizedOptionsJson(const std::vector<std::pair<std::string, rapidjson::Type>>& keys,
                                       const std::unordered_map<std::string, std::string>& values) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    for (const auto& kv : keys) {
        const auto it = values.find(kv.first);
        if (it == values.end()) continue;
        w.Key(kv.first.c_str());
        if (kv.second == rapidjson::kTrueType || kv.second == rapidjson::kFalseType) {
            w.Bool(it->second == "true");
        } else if (kv.second == rapidjson::kNumberType) {
            const int64_t num = std::strtoll(it->second.c_str(), nullptr, 10);
            w.Int64(num);
        } else {
            w.String(it->second.c_str());
        }
    }
    w.EndObject();
    return buf.GetString();
}

int ParseIntOption(const rapidjson::Value& options,
                   const char* key,
                   int64_t min_value,
                   int64_t max_value,
                   int64_t default_value,
                   int64_t* out,
                   std::string* err) {
    if (!out) return EINVAL;
    *out = default_value;
    if (!options.IsObject() || !options.HasMember(key)) return 0;
    const auto& v = options[key];
    if (!v.IsInt64()) {
        if (err) *err = std::string("option '") + key + "' must be int";
        return EINVAL;
    }
    const int64_t value = v.GetInt64();
    if (value < min_value || value > max_value) {
        if (err) {
            std::ostringstream oss;
            oss << "option '" << key << "' out of range [" << min_value << "," << max_value << "]";
            *err = oss.str();
        }
        return EINVAL;
    }
    *out = value;
    return 0;
}

int ParseBoolOption(const rapidjson::Value& options,
                    const char* key,
                    bool default_value,
                    bool* out,
                    std::string* err) {
    if (!out) return EINVAL;
    *out = default_value;
    if (!options.IsObject() || !options.HasMember(key)) return 0;
    const auto& v = options[key];
    if (!v.IsBool()) {
        if (err) *err = std::string("option '") + key + "' must be bool";
        return EINVAL;
    }
    *out = v.GetBool();
    return 0;
}

int ParseStringEnumOption(const rapidjson::Value& options,
                          const char* key,
                          const std::vector<std::string>& candidates,
                          const std::string& default_value,
                          std::string* out,
                          std::string* err) {
    if (!out) return EINVAL;
    *out = default_value;
    if (!options.IsObject() || !options.HasMember(key)) return 0;
    const auto& v = options[key];
    if (!v.IsString()) {
        if (err) *err = std::string("option '") + key + "' must be string";
        return EINVAL;
    }
    const std::string value = ToLowerAscii(v.GetString());
    if (std::find(candidates.begin(), candidates.end(), value) == candidates.end()) {
        if (err) {
            std::ostringstream oss;
            oss << "option '" << key << "' must be one of [";
            for (size_t i = 0; i < candidates.size(); ++i) {
                if (i != 0) oss << ",";
                oss << candidates[i];
            }
            oss << "]";
            *err = oss.str();
        }
        return EINVAL;
    }
    *out = value;
    return 0;
}

int ValidateRingOptions(const rapidjson::Value& options,
                        std::string* normalized_json,
                        std::string* err) {
    const rapidjson::Value* obj = &options;
    rapidjson::Document default_obj;
    if (options.IsNull()) {
        default_obj.SetObject();
        obj = &default_obj;
    }
    if (!obj->IsObject()) {
        if (err) *err = "options must be object";
        return EINVAL;
    }

    std::string ring_mode;
    int64_t ring_size = 256;
    std::string overflow;
    bool finite = false;
    int64_t batch_rows = 1024;

    int rc = ParseStringEnumOption(*obj, "ring_mode",
                                   {"spsc", "spmc", "mpsc", "mpmc"},
                                   "spsc", &ring_mode, err);
    if (rc != 0) return rc;
    rc = ParseIntOption(*obj, "ring_size", 2, (1LL << 30), 256, &ring_size, err);
    if (rc != 0) return rc;
    if (!IsPowerOfTwo(static_cast<uint64_t>(ring_size))) {
        if (err) *err = "option 'ring_size' must be power of two";
        return EINVAL;
    }
    rc = ParseStringEnumOption(*obj, "overflow", {"drop", "block"},
                               "drop", &overflow, err);
    if (rc != 0) return rc;
    rc = ParseBoolOption(*obj, "finite", false, &finite, err);
    if (rc != 0) return rc;
    rc = ParseIntOption(*obj, "batch_rows", 1, (1LL << 30), 1024, &batch_rows, err);
    if (rc != 0) return rc;

    if (normalized_json) {
        *normalized_json = BuildNormalizedOptionsJson(
            {{"ring_mode", rapidjson::kStringType},
             {"ring_size", rapidjson::kNumberType},
             {"overflow", rapidjson::kStringType},
             {"finite", rapidjson::kTrueType},
             {"batch_rows", rapidjson::kNumberType}},
            {{"ring_mode", ring_mode},
             {"ring_size", std::to_string(ring_size)},
             {"overflow", overflow},
             {"finite", finite ? "true" : "false"},
             {"batch_rows", std::to_string(batch_rows)}});
    }
    return 0;
}

int BuildRingChannel(const std::string& category,
                     const std::string& name,
                     const std::string& normalized_json,
                     std::shared_ptr<IStreamChannel>* out,
                     std::string* err) {
    if (!out) return EINVAL;
    out->reset();

    rapidjson::Document d;
    d.Parse(normalized_json.c_str());
    if (d.HasParseError() || !d.IsObject()) {
        if (err) *err = "ring options json parse failed";
        return EINVAL;
    }

    RingStreamChannelOptions opts;
    opts.ring_mode = ParseRingMode(d["ring_mode"].GetString());
    opts.ring_size = static_cast<size_t>(d["ring_size"].GetInt64());
    opts.overflow = ParseOverflowPolicy(d["overflow"].GetString());
    opts.finite = d["finite"].GetBool();
    opts.batch_rows = static_cast<int>(d["batch_rows"].GetInt64());

    auto ch = std::make_shared<RingStreamChannel>(category, name, opts);
    const int rc = ch->Open();
    if (rc != 0) {
        if (err) *err = "open ring channel failed";
        return rc;
    }
    *out = ch;
    return 0;
}

int ValidateTcpSessionMockOptions(const rapidjson::Value& options,
                                  std::string* normalized_json,
                                  std::string* err) {
    const rapidjson::Value* obj = &options;
    rapidjson::Document default_obj;
    if (options.IsNull()) {
        default_obj.SetObject();
        obj = &default_obj;
    }
    if (!obj->IsObject()) {
        if (err) *err = "options must be object";
        return EINVAL;
    }

    std::string mode;
    int64_t total_records = 1024;
    int64_t batch_rows = 64;
    int64_t emit_interval_ms = 0;
    int64_t partition_count = 4;
    int64_t ring_size = 256;
    std::string overflow;
    std::string ring_mode;

    int rc = ParseStringEnumOption(*obj, "mode", {"none", "stateless", "keyed"}, "none", &mode, err);
    if (rc != 0) return rc;
    rc = ParseIntOption(*obj, "total_records", 0, (1LL << 32), 1024, &total_records, err);
    if (rc != 0) return rc;
    rc = ParseIntOption(*obj, "batch_rows", 1, (1LL << 30), 64, &batch_rows, err);
    if (rc != 0) return rc;
    rc = ParseIntOption(*obj, "emit_interval_ms", 0, (1LL << 30), 0, &emit_interval_ms, err);
    if (rc != 0) return rc;
    rc = ParseIntOption(*obj, "partition_count", 1, 4096, 4, &partition_count, err);
    if (rc != 0) return rc;
    rc = ParseIntOption(*obj, "ring_size", 2, (1LL << 30), 256, &ring_size, err);
    if (rc != 0) return rc;
    if (!IsPowerOfTwo(static_cast<uint64_t>(ring_size))) {
        if (err) *err = "option 'ring_size' must be power of two";
        return EINVAL;
    }
    rc = ParseStringEnumOption(*obj, "overflow", {"drop", "block"}, "drop", &overflow, err);
    if (rc != 0) return rc;
    rc = ParseStringEnumOption(*obj, "ring_mode",
                               {"spsc", "spmc", "mpsc", "mpmc"},
                               "spsc", &ring_mode, err);
    if (rc != 0) return rc;

    if (normalized_json) {
        *normalized_json = BuildNormalizedOptionsJson(
            {{"mode", rapidjson::kStringType},
             {"total_records", rapidjson::kNumberType},
             {"batch_rows", rapidjson::kNumberType},
             {"emit_interval_ms", rapidjson::kNumberType},
             {"partition_count", rapidjson::kNumberType},
             {"ring_size", rapidjson::kNumberType},
             {"overflow", rapidjson::kStringType},
             {"ring_mode", rapidjson::kStringType}},
            {{"mode", mode},
             {"total_records", std::to_string(total_records)},
             {"batch_rows", std::to_string(batch_rows)},
             {"emit_interval_ms", std::to_string(emit_interval_ms)},
             {"partition_count", std::to_string(partition_count)},
             {"ring_size", std::to_string(ring_size)},
             {"overflow", overflow},
             {"ring_mode", ring_mode}});
    }
    return 0;
}

int BuildTcpSessionMockChannel(const std::string& category,
                               const std::string& name,
                               const std::string& normalized_json,
                               std::shared_ptr<IStreamChannel>* out,
                               std::string* err) {
    if (!out) return EINVAL;
    out->reset();

    rapidjson::Document d;
    d.Parse(normalized_json.c_str());
    if (d.HasParseError() || !d.IsObject()) {
        if (err) *err = "tcp_session_mock options json parse failed";
        return EINVAL;
    }

    TcpSessionMockOptions opts;
    opts.mode = ParseTcpSessionMockMode(d["mode"].GetString());
    opts.total_records = d["total_records"].GetInt64();
    opts.batch_rows = static_cast<int32_t>(d["batch_rows"].GetInt64());
    opts.emit_interval_ms = static_cast<int32_t>(d["emit_interval_ms"].GetInt64());
    opts.partition_count = static_cast<int32_t>(d["partition_count"].GetInt64());
    opts.queue_options.ring_size = static_cast<size_t>(d["ring_size"].GetInt64());
    opts.queue_options.overflow = ParseOverflowPolicy(d["overflow"].GetString());
    opts.queue_options.ring_mode = ParseRingMode(d["ring_mode"].GetString());
    opts.queue_options.finite = true;

    auto ch = std::make_shared<TcpSessionMockStreamChannel>(category, name, opts);
    const int rc = ch->Open();
    if (rc != 0) {
        if (err) *err = "open tcp_session_mock channel failed";
        return rc;
    }
    *out = ch;
    return 0;
}

int ValidateStreamHubOptions(const rapidjson::Value& options,
                             std::string* normalized_json,
                             std::string* err) {
    const rapidjson::Value* obj = &options;
    rapidjson::Document default_obj;
    if (options.IsNull()) {
        default_obj.SetObject();
        obj = &default_obj;
    }
    if (!obj->IsObject()) {
        if (err) *err = "options must be object";
        return EINVAL;
    }

    std::string mode;
    int64_t partition_count = 4;
    std::string partition_ring_mode;
    int64_t partition_ring_size = 256;

    int rc = ParseStringEnumOption(*obj, "mode", {"split", "merge"},
                                   "split", &mode, err);
    if (rc != 0) return rc;
    rc = ParseIntOption(*obj, "partition_count", 1, 1024, 4, &partition_count, err);
    if (rc != 0) return rc;
    rc = ParseStringEnumOption(*obj, "partition_ring_mode",
                               {"spsc", "spmc", "mpsc", "mpmc"},
                               "spsc", &partition_ring_mode, err);
    if (rc != 0) return rc;
    rc = ParseIntOption(*obj, "partition_ring_size", 2, (1LL << 30), 256, &partition_ring_size, err);
    if (rc != 0) return rc;
    if (!IsPowerOfTwo(static_cast<uint64_t>(partition_ring_size))) {
        if (err) *err = "option 'partition_ring_size' must be power of two";
        return EINVAL;
    }

    if (normalized_json) {
        *normalized_json = BuildNormalizedOptionsJson(
            {{"mode", rapidjson::kStringType},
             {"partition_count", rapidjson::kNumberType},
             {"partition_ring_mode", rapidjson::kStringType},
             {"partition_ring_size", rapidjson::kNumberType}},
            {{"mode", mode},
             {"partition_count", std::to_string(partition_count)},
             {"partition_ring_mode", partition_ring_mode},
             {"partition_ring_size", std::to_string(partition_ring_size)}});
    }
    return 0;
}

int BuildStreamHubChannel(const std::string& category,
                          const std::string& name,
                          const std::string& normalized_json,
                          std::shared_ptr<IStreamChannel>* out,
                          std::string* err) {
    if (!out) return EINVAL;
    out->reset();

    rapidjson::Document d;
    d.Parse(normalized_json.c_str());
    if (d.HasParseError() || !d.IsObject()) {
        if (err) *err = "stream_hub options json parse failed";
        return EINVAL;
    }

    StreamHubOptions opts;
    opts.mode = ParseStreamHubMode(d["mode"].GetString());
    opts.partition_count = static_cast<int32_t>(d["partition_count"].GetInt64());
    opts.partition_ring_mode = ParseRingMode(d["partition_ring_mode"].GetString());
    opts.partition_ring_size = static_cast<size_t>(d["partition_ring_size"].GetInt64());

    auto ch = std::make_shared<StreamHubChannel>(category, name, opts);
    const int rc = ch->Open();
    if (rc != 0) {
        if (err) *err = "open stream_hub channel failed";
        return rc;
    }
    *out = ch;
    return 0;
}

void RegisterDefaultStreamTypes() {
    auto& registry = BuiltinRegistry::Instance();
    StreamChannelTypeDescriptor ring_desc;
    ring_desc.type = "ring";
    ring_desc.display_name = "Ring Stream";
    ring_desc.allowed_roles = {"source", "sink", "both"};
    ring_desc.option_schema = {
        {"ring_mode", "enum", true, "spsc", {"spsc", "spmc", "mpsc", "mpmc"}, 0, 0, false, false, ""},
        {"ring_size", "int", true, "256", {}, 2, 0, true, true, ""},
        {"overflow", "enum", true, "drop", {"drop", "block"}, 0, 0, false, false, ""},
        {"finite", "bool", false, "false", {}, 0, 0, false, false, ""},
    };
    ring_desc.validate_and_normalize = ValidateRingOptions;
    ring_desc.build = BuildRingChannel;
    const int ring_rc = registry.RegisterStreamChannelType(ring_desc);
    if (ring_rc != 0 && ring_rc != EEXIST) {
        throw std::runtime_error("register ring stream type failed");
    }

    StreamChannelTypeDescriptor mock_desc;
    mock_desc.type = "tcp_session_mock";
    mock_desc.display_name = "TCP Session Mock";
    mock_desc.allowed_roles = {"source", "sink", "both"};
    mock_desc.option_schema = {
        {"mode", "enum", true, "none", {"none", "stateless", "keyed"}, 0, 0, false, false, ""},
        {"total_records", "int", true, "1024", {}, 0, 0, true, false, ""},
        {"batch_rows", "int", true, "64", {}, 1, 0, true, false, ""},
        {"emit_interval_ms", "int", true, "0", {}, 0, 0, true, false, ""},
        {"partition_count", "int", true, "4", {}, 1, 0, true, false, ""},
        {"ring_size", "int", true, "256", {}, 2, 0, true, true, ""},
        {"overflow", "enum", true, "drop", {"drop", "block"}, 0, 0, false, false, ""},
        {"ring_mode", "enum", true, "spsc", {"spsc", "spmc", "mpsc", "mpmc"}, 0, 0, false, false, ""},
    };
    mock_desc.validate_and_normalize = ValidateTcpSessionMockOptions;
    mock_desc.build = BuildTcpSessionMockChannel;
    const int mock_rc = registry.RegisterStreamChannelType(mock_desc);
    if (mock_rc != 0 && mock_rc != EEXIST) {
        throw std::runtime_error("register tcp_session_mock stream type failed");
    }

    StreamChannelTypeDescriptor hub_desc;
    hub_desc.type = "stream_hub";
    hub_desc.display_name = "Stream Hub";
    hub_desc.allowed_roles = {"source", "sink", "both"};
    hub_desc.option_schema = {
        {"mode", "enum", true, "split", {"split", "merge"}, 0, 0, false, false, ""},
        {"partition_count", "int", true, "4", {}, 1, 0, true, false, ""},
        {"partition_ring_mode", "enum", true, "spsc", {"spsc", "spmc", "mpsc", "mpmc"}, 0, 0, false, false, ""},
        {"partition_ring_size", "int", true, "256", {}, 2, 0, true, true, ""},
    };
    hub_desc.validate_and_normalize = ValidateStreamHubOptions;
    hub_desc.build = BuildStreamHubChannel;
    const int hub_rc = registry.RegisterStreamChannelType(hub_desc);
    if (hub_rc != 0 && hub_rc != EEXIST) {
        throw std::runtime_error("register stream_hub stream type failed");
    }
}

void RegisterDefaultBuiltinOperators() {
    auto& registry = BuiltinRegistry::Instance();
    const std::vector<BuiltinOperatorDescriptor> defaults = {
        {"builtin", "passthrough", []() -> IOperator* { return new PassthroughOperator(); }},
        {"builtin", "dataframe_dispatch_stream", []() -> IOperator* { return new DataframeDispatchStreamOperator(); }},
        {"builtin", "concat", []() -> IOperator* { return new ConcatOperator(); }},
        {"builtin", "hstack", []() -> IOperator* { return new HstackOperator(); }},
        {"builtin", "passthrough_stream", []() -> IOperator* { return new PassthroughStreamOperator(); }},
        {"builtin", "count_window_stream", []() -> IOperator* { return new CountWindowStreamOperator(); }},
        {"builtin", "tcp_service_merge_stream", []() -> IOperator* { return new TcpServiceMergeStreamOperator(); }},
    };

    for (const auto& desc : defaults) {
        const int rc = registry.RegisterBuiltinOperator(desc);
        if (rc != 0 && rc != EEXIST) {
            throw std::runtime_error("register builtin operator failed");
        }
    }
}

}  // namespace

BuiltinRegistry& BuiltinRegistry::Instance() {
    static BuiltinRegistry instance;
    return instance;
}

int BuiltinRegistry::RegisterStreamChannelType(const StreamChannelTypeDescriptor& desc) {
    if (desc.type.empty() || !desc.validate_and_normalize || !desc.build) return EINVAL;
    const std::string key = ToLowerAscii(desc.type);
    std::lock_guard<std::mutex> lock(mu_);
    if (stream_types_.find(key) != stream_types_.end()) return EEXIST;
    stream_types_[key] = desc;
    stream_types_[key].type = key;
    return 0;
}

int BuiltinRegistry::RegisterBuiltinOperator(const BuiltinOperatorDescriptor& desc) {
    if (desc.category.empty() || desc.name.empty() || !desc.factory) return EINVAL;
    const std::string key = ToLowerAscii(desc.category) + "." + ToLowerAscii(desc.name);
    std::lock_guard<std::mutex> lock(mu_);
    if (operator_index_.find(key) != operator_index_.end()) return EEXIST;
    operator_index_[key] = operators_.size();
    operators_.push_back(desc);
    return 0;
}

bool BuiltinRegistry::FindStreamChannelType(const std::string& type,
                                            StreamChannelTypeDescriptor* out) const {
    if (!out) return false;
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = stream_types_.find(ToLowerAscii(type));
    if (it == stream_types_.end()) return false;
    *out = it->second;
    return true;
}

std::vector<StreamChannelTypeDescriptor> BuiltinRegistry::ListStreamChannelTypes() const {
    std::vector<StreamChannelTypeDescriptor> ret;
    std::lock_guard<std::mutex> lock(mu_);
    ret.reserve(stream_types_.size());
    for (const auto& kv : stream_types_) {
        ret.push_back(kv.second);
    }
    std::sort(ret.begin(), ret.end(), [](const auto& a, const auto& b) {
        return a.type < b.type;
    });
    return ret;
}

std::vector<BuiltinOperatorDescriptor> BuiltinRegistry::ListBuiltinOperators() const {
    std::lock_guard<std::mutex> lock(mu_);
    return operators_;
}

void EnsureBuiltinRegistryInitialized() {
    static std::once_flag once;
    std::call_once(once, []() {
        RegisterDefaultStreamTypes();
        RegisterDefaultBuiltinOperators();
    });
}

std::string NormalizeRole(const std::string& role) {
    const std::string lower = ToLowerAscii(role);
    if (lower.empty()) return "both";
    if (lower == "source" || lower == "sink" || lower == "both") return lower;
    return "";
}

bool IsRoleAllowed(const std::string& role,
                   const std::vector<std::string>& allowed_roles) {
    const std::string normalized = NormalizeRole(role);
    if (normalized.empty()) return false;
    if (allowed_roles.empty()) return true;
    const std::string both = "both";
    if (normalized == both) return true;
    for (const auto& item : allowed_roles) {
        if (ToLowerAscii(item) == normalized || ToLowerAscii(item) == both) return true;
    }
    return false;
}

}  // namespace flowsql
