// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "baseline_plugin.h"

#include <common/iplugin.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <utility>

#include "config/runtime_config.h"
#include "config_parser.h"
#include "model/task_spec.h"
#include "solver/solver_backend.h"
#include "task/relation_task.h"
#include "task/ratio_task.h"
#include "task/task_registry.h"
#include "task/value_task.h"

namespace flowsql {
namespace baseline {

namespace {

class BaselineTaskStateControl final : public IBaselineTaskStateControlV1 {
 public:
    explicit BaselineTaskStateControl(std::shared_ptr<BaselineTaskBase> task) : task_(std::move(task)) {}
    BaselineStatus ReleaseIdentity(std::string_view key, BaselineStateReleaseScopeV1 scope) override {
        return task_->ReleaseIdentity(key, scope);
    }
    std::pair<BaselineStatus, BaselineStateUsageV1> QueryUsage() const override { return task_->QueryStateUsage(); }

 private:
    std::shared_ptr<BaselineTaskBase> task_;
};

BaselineStatus ValidateCreateFormat(BaselineSerializationFormat format) {
    return format == BaselineSerializationFormat::kJson
               ? BaselineStatus::kOk
               : BaselineStatus::kUnsupportedFormat;
}

std::string MakeConfigCopy(std::string_view config_content) {
    return std::string(config_content.data(), config_content.size());
}

BaselineStatus ParseFailureStatus(int rc) {
    return rc == error::OK ? BaselineStatus::kOk : BaselineStatus::kParseFailed;
}

BaselineStatus BindTaskCalendar(int64_t delta, const std::string& timezone,
                                std::shared_ptr<const CompiledEventCalendar>* calendar) {
    if (!*calendar) return BaselineStatus::kOk;
    BaselineTaskSpec clock;
    clock.delta = delta;
    clock.tz = timezone;
    auto bound = std::make_shared<CompiledEventCalendar>();
    std::string diagnostics;
    if (BindEventCalendar(**calendar, clock, bound.get(), &diagnostics) != error::OK) {
        return BaselineStatus::kInvalidArgument;
    }
    *calendar = std::move(bound);
    return BaselineStatus::kOk;
}

}  // namespace

BaselinePlugin::BaselinePlugin()
    : task_registry_(std::make_unique<TaskRegistry>()) {}

BaselinePlugin::~BaselinePlugin() = default;

int BaselinePlugin::Option(const char* arg) {
    option_ = arg ? arg : "";
    config_file_.clear();
    config_strict_ = true;

    if (option_.empty()) return error::OK;
    std::size_t pos = 0;
    while (pos < option_.size()) {
        const std::size_t eq = option_.find('=', pos);
        if (eq == std::string::npos) break;
        std::size_t end = option_.find(';', eq);
        if (end == std::string::npos) end = option_.size();

        const std::string key = option_.substr(pos, eq - pos);
        const std::string value = option_.substr(eq + 1, end - eq - 1);
        if (key == "config_file") {
            config_file_ = value;
        } else if (key == "strict") {
            std::string lowered = value;
            std::transform(lowered.begin(),
                           lowered.end(),
                           lowered.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (lowered == "1" || lowered == "true" || lowered == "yes") {
                config_strict_ = true;
            } else if (lowered == "0" || lowered == "false" || lowered == "no") {
                config_strict_ = false;
            } else {
                return error::BAD_REQUEST;
            }
        }
        pos = end < option_.size() ? end + 1 : option_.size();
    }
    return error::OK;
}

int BaselinePlugin::Load(IQuerier* querier) {
    std::string err;
    const int rc =
        LoadBaselineRuntimeConfigFromYaml(config_file_, config_strict_, &err);
    if (rc != error::OK) {
        if (!err.empty()) {
            std::fprintf(stderr, "[baseline] load runtime config failed: %s\n", err.c_str());
        }
        return rc;
    }

    querier_ = querier;
    (void)SolverBackend::IsAvailable();
    return error::OK;
}

int BaselinePlugin::Unload() {
    Stop();
    task_registry_ = std::make_unique<TaskRegistry>();
    querier_ = nullptr;
    ResetBaselineRuntimeConfig();
    return error::OK;
}

int BaselinePlugin::Start() { return error::OK; }

std::pair<BaselineStatus, std::shared_ptr<IBaselineTaskStateControlV1>> BaselinePlugin::Bind(
    std::shared_ptr<IBaselineTask> task, const BaselineStateLimitsV1& limits) {
    auto implementation = std::dynamic_pointer_cast<BaselineTaskBase>(task);
    if (!task_registry_ || !implementation || !task_registry_->Owns(implementation.get())) {
        return {BaselineStatus::kInvalidArgument, nullptr};
    }
    auto control = std::make_shared<BaselineTaskStateControl>(implementation);
    const auto status = implementation->BindStateLimits(limits);
    if (status != BaselineStatus::kOk) return {status, nullptr};
    return {BaselineStatus::kOk, std::move(control)};
}

int BaselinePlugin::Stop() {
    if (task_registry_) {
        const auto tasks = task_registry_->Snapshot();
        for (const auto& task : tasks) {
            if (task) (void)task->Close();
        }
    }
    return error::OK;
}

std::pair<BaselineStatus, std::shared_ptr<IBaselineValueTask>>
BaselinePlugin::CreateValueTask(std::string_view config_content,
                                BaselineSerializationFormat format) {
    const BaselineStatus format_status = ValidateCreateFormat(format);
    if (format_status != BaselineStatus::kOk) return {format_status, nullptr};
    if (!task_registry_) return {BaselineStatus::kInvalidArgument, nullptr};

    const std::string config = MakeConfigCopy(config_content);
    BaselineTaskSpec spec;
    std::string err;
    const BaselineStatus parse_status =
        ParseFailureStatus(ConfigParser::ParseValueTask(config.c_str(), &spec, &err));
    if (parse_status != BaselineStatus::kOk) return {parse_status, nullptr};

    auto calendar = FindBaselineEventCalendar(spec.calendar_ref);
    const BaselineStatus calendar_status = BindTaskCalendar(spec.delta, spec.tz, &calendar);
    if (calendar_status != BaselineStatus::kOk) return {calendar_status, nullptr};
    auto task = std::make_shared<BaselineValueTask>(
        task_registry_.get(), spec.task_id, spec.name, config, spec, std::move(calendar));
    if (task_registry_->Register(task) != error::OK) {
        return {BaselineStatus::kInvalidArgument, nullptr};
    }
    return {BaselineStatus::kOk, task};
}

std::pair<BaselineStatus, std::shared_ptr<IBaselineRatioTask>>
BaselinePlugin::CreateRatioTask(std::string_view config_content,
                                BaselineSerializationFormat format) {
    const BaselineStatus format_status = ValidateCreateFormat(format);
    if (format_status != BaselineStatus::kOk) return {format_status, nullptr};
    if (!task_registry_) return {BaselineStatus::kInvalidArgument, nullptr};

    const std::string config = MakeConfigCopy(config_content);
    BaselineTaskSpec spec;
    std::string err;
    const BaselineStatus parse_status =
        ParseFailureStatus(ConfigParser::ParseRatioTask(config.c_str(), &spec, &err));
    if (parse_status != BaselineStatus::kOk) return {parse_status, nullptr};

    auto calendar = FindBaselineEventCalendar(spec.calendar_ref);
    const BaselineStatus calendar_status = BindTaskCalendar(spec.delta, spec.tz, &calendar);
    if (calendar_status != BaselineStatus::kOk) return {calendar_status, nullptr};
    auto task = std::make_shared<BaselineRatioTask>(
        task_registry_.get(), spec.task_id, spec.name, config, spec, std::move(calendar));
    if (task_registry_->Register(task) != error::OK) {
        return {BaselineStatus::kInvalidArgument, nullptr};
    }
    return {BaselineStatus::kOk, task};
}

std::pair<BaselineStatus, std::shared_ptr<IBaselineRelationTask>>
BaselinePlugin::CreateRelationTask(std::string_view config_content,
                                   BaselineSerializationFormat format) {
    const BaselineStatus format_status = ValidateCreateFormat(format);
    if (format_status != BaselineStatus::kOk) return {format_status, nullptr};
    if (!task_registry_) return {BaselineStatus::kInvalidArgument, nullptr};

    const std::string config = MakeConfigCopy(config_content);
    RelationTaskCreateSpec create_spec;
    std::string err;
    const BaselineStatus parse_status =
        ParseFailureStatus(ConfigParser::ParseRelationTask(config.c_str(), &create_spec, &err));
    if (parse_status != BaselineStatus::kOk) return {parse_status, nullptr};

    auto calendar = FindBaselineEventCalendar(create_spec.task_spec.calendar_ref);
    const BaselineStatus calendar_status =
        BindTaskCalendar(create_spec.clock_spec.delta, create_spec.clock_spec.tz, &calendar);
    if (calendar_status != BaselineStatus::kOk) return {calendar_status, nullptr};
    auto task = std::make_shared<BaselineRelationTask>(
        task_registry_.get(),
        create_spec.task_spec.task_id,
        create_spec.task_spec.name,
        config,
        create_spec,
        std::move(calendar));
    if (task_registry_->Register(task) != error::OK) {
        return {BaselineStatus::kInvalidArgument, nullptr};
    }
    return {BaselineStatus::kOk, task};
}

BaselineSerializationResult BaselinePlugin::QueryServiceSnapshot(
    BaselineSerializationFormat format) const {
    if (format != BaselineSerializationFormat::kJson) {
        return {BaselineStatus::kUnsupportedFormat, ""};
    }

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buf);
    writer.StartObject();
    writer.Key("task_count");
    writer.Uint64(task_registry_ ? task_registry_->Size() : 0);
    writer.Key("tasks");
    writer.StartArray();
    if (task_registry_) {
        const auto tasks = task_registry_->Snapshot();
        for (const auto& task : tasks) {
            if (!task) continue;
            writer.StartObject();
            writer.Key("task_id");
            writer.String(task->Id());
            writer.Key("task_name");
            writer.String(task->Name());
            writer.Key("kind");
            switch (task->Kind()) {
                case BaselineTaskKind::kValue:
                    writer.String("value");
                    break;
                case BaselineTaskKind::kRatio:
                    writer.String("ratio");
                    break;
                case BaselineTaskKind::kRelation:
                    writer.String("relation");
                    break;
            }
            writer.EndObject();
        }
    }
    writer.EndArray();
    writer.EndObject();
    return {BaselineStatus::kOk, buf.GetString()};
}

}  // namespace baseline
}  // namespace flowsql

BEGIN_PLUGIN_REGIST(flowsql::baseline::BaselinePlugin)
    ____INTERFACE(flowsql::IID_PLUGIN, flowsql::IPlugin)
    ____INTERFACE(flowsql::IID_BASELINE_SERVICE, flowsql::IBaselineService)
    ____INTERFACE(flowsql::IID_BASELINE_STATE_CONTROL_SERVICE_V1, flowsql::IBaselineStateControlServiceV1)
    END_PLUGIN_REGIST()
