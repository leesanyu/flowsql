// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "baseline_task_base.h"

#include <common/error_code.h>

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "task_registry.h"

namespace flowsql {
namespace baseline {

BaselineTaskBase::BaselineTaskBase(TaskRegistry* registry,
                                   std::string task_id,
                                   BaselineTaskKind kind,
                                   std::string task_name,
                                   std::string config_json)
    : registry_(registry),
      task_id_(std::move(task_id)),
      kind_(kind),
      task_name_(std::move(task_name)),
      config_json_(std::move(config_json)) {}

const char* BaselineTaskBase::Id() const { return task_id_.c_str(); }
const char* BaselineTaskBase::Name() const { return task_name_.c_str(); }
BaselineTaskKind BaselineTaskBase::Kind() const { return kind_; }
const std::string& BaselineTaskBase::TaskId() const { return task_id_; }

BaselineSerializationResult BaselineTaskBase::ExportConfig(
    BaselineSerializationFormat format) const {
    if (format != BaselineSerializationFormat::kJson) {
        return UnsupportedFormatResult(format);
    }

    return {BaselineStatus::kOk, config_json_};
}

BaselineSerializationResult BaselineTaskBase::QueryTaskSnapshot(
    BaselineSerializationFormat format) const {
    if (format != BaselineSerializationFormat::kJson) {
        return UnsupportedFormatResult(format);
    }
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buf);
    writer.StartObject();
    writer.Key("task_id");
    writer.String(task_id_.c_str());
    writer.Key("name");
    writer.String(task_name_.c_str());
    writer.Key("kind");
    writer.String(KindName(kind_));
    writer.Key("closed");
    writer.Bool(closed_);
    writer.EndObject();
    return {BaselineStatus::kOk, buf.GetString()};
}

BaselineSerializationResult BaselineTaskBase::QuerySeriesSnapshot(
    std::string_view series_key,
    BaselineSerializationFormat format) const {
    if (format != BaselineSerializationFormat::kJson) {
        return UnsupportedFormatResult(format);
    }
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buf);
    writer.StartObject();
    writer.Key("task_id");
    writer.String(task_id_.c_str());
    writer.Key("key");
    const std::string key_copy(series_key);
    writer.String(key_copy.c_str());
    writer.Key("status");
    writer.String("not_ready");
    writer.EndObject();
    return {BaselineStatus::kOk, buf.GetString()};
}

BaselineStatus BaselineTaskBase::Close() {
    std::shared_ptr<BaselineTaskBase> self = shared_from_this();
    if (closed_) return BaselineStatus::kOk;
    closed_ = true;
    OnClosing();

    if (registry_) registry_->Unregister(task_id_, this);
    return BaselineStatus::kOk;
}

BaselineStatus BaselineTaskBase::EnsureOpen() const {
    return closed_ ? BaselineStatus::kInvalidArgument : BaselineStatus::kOk;
}

BaselineSerializationResult BaselineTaskBase::UnsupportedFormatResult(
    BaselineSerializationFormat) {
    return {BaselineStatus::kUnsupportedFormat, ""};
}

void BaselineTaskBase::OnClosing() {}

BaselineStatus BaselineTaskBase::BindStateLimits(const BaselineStateLimitsV1& limits) {
    if (closed_ || state_operations_started_ || state_limits_ || limits.max_runtime_identities == 0 ||
        limits.max_model_identities == 0 || limits.max_basis_versions_per_metric == 0 ||
        (kind_ == BaselineTaskKind::kRelation && limits.max_basis_versions_per_metric < 2)) {
        return BaselineStatus::kInvalidArgument;
    }
    state_limits_ = limits;
    return BaselineStatus::kOk;
}

BaselineStatus BaselineTaskBase::ReleaseIdentity(std::string_view key, BaselineStateReleaseScopeV1 scope) {
    if (closed_ || !state_limits_ || key.empty() ||
        (scope != BaselineStateReleaseScopeV1::kRuntimeOnly && scope != BaselineStateReleaseScopeV1::kAllState)) {
        return BaselineStatus::kInvalidArgument;
    }
    return DoReleaseIdentity(key, scope);
}

std::pair<BaselineStatus, BaselineStateUsageV1> BaselineTaskBase::QueryStateUsage() const {
    if (closed_ || !state_limits_) return {BaselineStatus::kInvalidArgument, {}};
    return {BaselineStatus::kOk, DoQueryStateUsage()};
}

const char* BaselineTaskBase::KindName(BaselineTaskKind kind) {
    switch (kind) {
        case BaselineTaskKind::kValue:
            return "value";
        case BaselineTaskKind::kRatio:
            return "ratio";
        case BaselineTaskKind::kRelation:
            return "relation";
    }
    return "unknown";
}

}  // namespace baseline
}  // namespace flowsql
