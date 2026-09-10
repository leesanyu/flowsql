// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "scheduler_plugin.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cstdio>
#include <chrono>
#include <common/error_code.h>
#include <common/log.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <regex>
#include <sstream>
#include <thread>
#include <unordered_set>

#include "framework/core/channel_adapter.h"
#include "framework/core/dataframe.h"
#include "framework/core/dataframe_channel.h"
#include "framework/core/fan_in_stream_channel.h"
#include "framework/core/fan_out_stream_channel.h"
#include "framework/core/filter_planner.h"
#include "framework/core/json_error_builder.h"
#include "framework/core/pipeline.h"
#include "framework/core/ring_stream_channel.h"
#include "framework/core/sql_parser.h"
#include "framework/core/sql_text_splitter.h"
#include "framework/core/packet_codec.h"
#include "framework/interfaces/ichannel.h"
#include "framework/interfaces/ichannel_registry.h"
#include "framework/interfaces/idatabase_channel.h"
#include "framework/interfaces/idatabase_factory.h"
#include "framework/interfaces/idataframe_channel.h"
#include "framework/interfaces/ibuiltin_registry.h"
#include "framework/interfaces/iblock_stream_operator.h"
#include "framework/interfaces/iblock_stream_factory.h"
#include "framework/interfaces/iblock_stream_manager.h"
#include "framework/interfaces/iblock_stream_reader.h"
#include "framework/interfaces/ibridge.h"
#include "framework/interfaces/ioperator.h"
#include "framework/interfaces/ioperator_catalog.h"
#include "framework/interfaces/ioperator_registry.h"
#include "framework/interfaces/istream_channel.h"
#include "framework/interfaces/istream_factory.h"
#include "framework/interfaces/istream_manager.h"
#include "scheduler_json_codec.h"
#include "scheduler_internal_utils.h"

namespace flowsql {
namespace scheduler {

static std::shared_ptr<IChannel> MakeNonOwningChannelHolder(IChannel* ch) {
    if (!ch) return nullptr;
    return std::shared_ptr<IChannel>(ch, [](IChannel*) {});
}

static std::shared_ptr<IBlockStreamChannel> HoldExclusiveBlockReader(
    IBlockStreamReaderFactoryV1* provider,
    IBlockStreamChannel* reader) {
    return std::shared_ptr<IBlockStreamChannel>(
        reader, [provider](IBlockStreamChannel* value) {
            try {
                provider->ReleaseReader(value);
            } catch (...) {
                LOG_ERROR("IBlockStreamReaderFactoryV1::ReleaseReader threw");
            }
        });
}

static int CreateExclusiveBlockReader(IQuerier* querier,
                                      const std::string& task_id,
                                      IBlockStreamChannel* legacy_source,
                                      const std::string& pushed_filter_plan_json,
                                      std::shared_ptr<IBlockStreamChannel>* reader_out,
                                      std::string* error) {
    if (!querier || !legacy_source || pushed_filter_plan_json.empty() ||
        !reader_out || !error) {
        return EINVAL;
    }
    reader_out->reset();
    error->clear();

    const std::string source_category =
        legacy_source->Category() ? legacy_source->Category() : "";
    const std::string source_name = legacy_source->Name() ? legacy_source->Name() : "";
    BlockStreamReaderConfigV1 config;
    config.task_id = task_id.c_str();
    config.source_category = source_category.c_str();
    config.source_name = source_name.c_str();
    config.pushed_filter_plan_json = pushed_filter_plan_json.c_str();

    std::vector<std::shared_ptr<IBlockStreamChannel>> matches;
    int route_error = 0;
    try {
        const int traverse_rc = querier->Traverse(
            IID_BLOCK_STREAM_READER_FACTORY_V1, [&](void* value) -> int {
                auto* provider = static_cast<IBlockStreamReaderFactoryV1*>(value);
                if (!provider) return 0;

                IBlockStreamChannel* reader = nullptr;
                int create_rc = 0;
                try {
                    create_rc = provider->CreateReader(config, &reader);
                } catch (const std::exception& ex) {
                    if (reader) {
                        HoldExclusiveBlockReader(provider, reader).reset();
                    }
                    *error = std::string("block stream reader factory threw: ") + ex.what();
                    route_error = EFAULT;
                    return -1;
                } catch (...) {
                    if (reader) {
                        HoldExclusiveBlockReader(provider, reader).reset();
                    }
                    *error = "block stream reader factory threw an unknown exception";
                    route_error = EFAULT;
                    return -1;
                }

                if (create_rc == ENOTSUP && !reader) return 0;
                if (create_rc != 0 || !reader) {
                    if (reader) {
                        HoldExclusiveBlockReader(provider, reader).reset();
                    }
                    if (create_rc == 0) {
                        *error = "block stream reader factory returned a null reader";
                        route_error = EPROTO;
                    } else if (create_rc == ENOTSUP) {
                        *error = "block stream reader factory returned a reader with ENOTSUP";
                        route_error = EPROTO;
                    } else {
                        *error = "block stream reader factory failed with code " +
                                 std::to_string(create_rc);
                        route_error = create_rc;
                    }
                    return -1;
                }

                matches.push_back(HoldExclusiveBlockReader(provider, reader));
                if (matches.size() > 1) {
                    *error = "multiple block stream reader factories matched: " +
                             source_category + "." + source_name;
                    route_error = EEXIST;
                    return -1;
                }
                return 0;
            });
        if (route_error != 0) return route_error;
        if (traverse_rc != 0) {
            *error = "block stream reader factory traversal failed with code " +
                     std::to_string(traverse_rc);
            return traverse_rc;
        }
    } catch (const std::exception& ex) {
        *error = std::string("block stream reader factory traversal threw: ") + ex.what();
        return EFAULT;
    } catch (...) {
        *error = "block stream reader factory traversal threw an unknown exception";
        return EFAULT;
    }

    if (matches.empty()) return ENOTSUP;
    *reader_out = std::move(matches.front());
    return 0;
}

size_t SchedulerPlugin::TraverseBlockFactories(
    const std::function<int(IBlockStreamFactory*)>& visitor) const {
    if (!querier_ || !visitor) return 0;
    size_t provider_count = 0;
    querier_->Traverse(IID_BLOCK_STREAM_FACTORY, [&](void* value) -> int {
        auto* provider = static_cast<IBlockStreamFactory*>(value);
        if (!provider) return 0;
        ++provider_count;
        return visitor(provider);
    });
    return provider_count;
}

size_t SchedulerPlugin::TraverseBlockManagers(
    const std::function<int(IBlockStreamManager*)>& visitor) const {
    if (!querier_ || !visitor) return 0;
    size_t provider_count = 0;
    querier_->Traverse(IID_BLOCK_STREAM_MANAGER, [&](void* value) -> int {
        auto* provider = static_cast<IBlockStreamManager*>(value);
        if (!provider) return 0;
        ++provider_count;
        return visitor(provider);
    });
    return provider_count;
}

SchedulerPlugin::BlockManagerRouteResult SchedulerPlugin::RouteBlockManagers(
    const std::function<int(IBlockStreamManager*)>& operation,
    bool ignore_not_found) const {
    BlockManagerRouteResult result;
    if (!operation) return result;
    result.provider_count = TraverseBlockManagers([&](IBlockStreamManager* manager) -> int {
        const int rc = operation(manager);
        if (rc == ENOTSUP) return 0;
        if (ignore_not_found && rc == ENOENT) {
            ++result.not_found_count;
            return 0;
        }
        ++result.accepted_count;
        if (result.accepted_count == 1) {
            result.accepted_rc = rc;
        } else {
            result.conflict = true;
        }
        return result.conflict ? -1 : 0;
    });
    return result;
}

std::vector<IBlockStreamManager*> SchedulerPlugin::FindBlockManagerOwners(
    const std::string& type,
    const std::string& name) const {
    std::vector<IBlockStreamManager*> owners;
    TraverseBlockManagers([&](IBlockStreamManager* manager) -> int {
        bool owns_channel = false;
        manager->QueryChannels([&](const std::string& item_type,
                                   const std::string& item_name,
                                   const std::string&,
                                   const std::string&) {
            if (ToLowerAscii(item_type) == ToLowerAscii(type) && item_name == name) owns_channel = true;
        });
        if (owns_channel) owners.push_back(manager);
        return 0;
    });
    return owners;
}

static std::shared_ptr<IStreamChannel> MakeStreamOwner(IStreamChannel* stream_ch,
                                                       const std::shared_ptr<IChannel>& owner) {
    if (!stream_ch) return nullptr;
    if (owner) {
        auto stream_owner = std::dynamic_pointer_cast<IStreamChannel>(owner);
        if (stream_owner) return stream_owner;
    }
    return std::shared_ptr<IStreamChannel>(stream_ch, [](IStreamChannel*) {});
}

void SchedulerPlugin::EnumRoutes(std::function<void(const RouteItem&)> cb) {
    // 任务执行
    cb({"POST", "/scheduler/batch/execute",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleExecute(u, req, rsp);
        }});
    cb({"POST", "/scheduler/batch/submit",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleBatchSubmit(u, req, rsp);
        }});
    cb({"POST", "/scheduler/batch/status",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleBatchStatus(u, req, rsp);
        }});
    cb({"POST", "/scheduler/batch/stop",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleBatchStop(u, req, rsp);
        }});
    cb({"POST", "/scheduler/sql/classify",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleSqlClassify(u, req, rsp);
        }});
    cb({"POST", "/scheduler/stream/execute",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleStreamExecute(u, req, rsp);
        }});
    cb({"POST", "/scheduler/stream/stop",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleStreamStop(u, req, rsp);
        }});
    cb({"POST", "/scheduler/stream/status",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleStreamStatus(u, req, rsp);
        }});
    cb({"POST", "/scheduler/stream/list",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleStreamList(u, req, rsp);
        }});
    cb({"POST", "/scheduler/runtime/graph/query",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleRuntimeGraphQuery(u, req, rsp);
        }});
    // 流式通道查询（管理面最小字段）
    cb({"POST", "/channels/stream/query",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleQueryStreamChannels(u, req, rsp);
        }});
    cb({"POST", "/channels/stream/definitions/query",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleQueryStreamChannelDefinitions(u, req, rsp);
        }});
    cb({"POST", "/channels/stream/add",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleAddStreamChannel(u, req, rsp);
        }});
    cb({"POST", "/channels/stream/modify",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleModifyStreamChannel(u, req, rsp);
        }});
    cb({"POST", "/channels/stream/reset",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleResetStreamChannel(u, req, rsp);
        }});
    cb({"POST", "/channels/stream/remove",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleRemoveStreamChannel(u, req, rsp);
        }});
    // 内存通道查询
    cb({"POST", "/channels/dataframe/query",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleGetChannels(u, req, rsp);
        }});
    // Python 算子刷新
    cb({"POST", "/operators/python/refresh",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleRefreshOperators(u, req, rsp);
        }});
}

int32_t SchedulerPlugin::HandleSqlClassify(const std::string&, const std::string& req_body, std::string& rsp) {
    rapidjson::Document doc;
    doc.Parse(req_body.c_str());
    if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("sql") || !doc["sql"].IsString()) {
        rsp = BuildErrorJson("invalid request, expected {\"sql\":\"...\"}");
        return error::BAD_REQUEST;
    }

    std::string task_kind;
    std::string err_rsp;
    const int32_t rc = ClassifySqlTaskKind(doc["sql"].GetString(), &task_kind, &err_rsp);
    if (rc != error::OK) {
        rsp = err_rsp.empty() ? BuildErrorJson("sql classify failed") : err_rsp;
        return rc;
    }

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("task_kind");
    w.String(task_kind.c_str());
    w.EndObject();
    rsp = buf.GetString();
    return error::OK;
}

int32_t SchedulerPlugin::HandleBatchSubmit(const std::string&, const std::string& req_body, std::string& rsp) {
    rapidjson::Document doc;
    doc.Parse(req_body.c_str());
    if (doc.HasParseError() || !doc.IsObject()) {
        rsp = BuildErrorJson("invalid request body");
        return error::BAD_REQUEST;
    }

    std::string runtime_task_id;
    if (doc.HasMember("runtime_task_id")) {
        if (!doc["runtime_task_id"].IsString()) {
            rsp = BuildErrorJson("runtime_task_id must be string");
            return error::BAD_REQUEST;
        }
        runtime_task_id = doc["runtime_task_id"].GetString();
    }
    if (runtime_task_id.empty()) {
        runtime_task_id = "b_" + NextStreamTaskId();
    }

    int timeout_s = 0;
    if (doc.HasMember("timeout_s")) {
        if (!doc["timeout_s"].IsInt()) {
            rsp = BuildErrorJson("timeout_s must be integer");
            return error::BAD_REQUEST;
        }
        timeout_s = doc["timeout_s"].GetInt();
        if (timeout_s < 0) {
            rsp = BuildErrorJson("timeout_s must be >= 0");
            return error::BAD_REQUEST;
        }
    }

    std::vector<std::string> sqls;
    if (doc.HasMember("sqls")) {
        if (!doc["sqls"].IsArray() || doc["sqls"].Empty()) {
            rsp = BuildErrorJson("sqls must be non-empty string array");
            return error::BAD_REQUEST;
        }
        for (const auto& it : doc["sqls"].GetArray()) {
            if (!it.IsString()) {
                rsp = BuildErrorJson("sqls must be non-empty string array");
                return error::BAD_REQUEST;
            }
            std::string sql = it.GetString();
            if (sql.empty()) {
                rsp = BuildErrorJson("sqls must not contain empty SQL");
                return error::BAD_REQUEST;
            }
            sqls.push_back(sql);
        }
    } else if (doc.HasMember("sql_text") && doc["sql_text"].IsString()) {
        SqlTextSplitError split_err;
        if (SplitSqlText(doc["sql_text"].GetString(), &sqls, &split_err) != 0) {
            std::string err = "invalid sql_text";
            if (!split_err.message.empty()) err += ": " + split_err.message;
            rsp = BuildErrorJson(err);
            return error::BAD_REQUEST;
        }
    } else {
        rsp = BuildErrorJson("request must contain sqls or sql_text");
        return error::BAD_REQUEST;
    }

    for (size_t i = 0; i < sqls.size(); ++i) {
        std::string task_kind;
        std::string classify_err_rsp;
        const int32_t classify_rc = ClassifySqlTaskKind(sqls[i], &task_kind, &classify_err_rsp);
        if (classify_rc != error::OK) {
            rsp = classify_err_rsp.empty() ? BuildErrorJson("sql classify failed") : classify_err_rsp;
            return classify_rc;
        }
        if (task_kind != "batch") {
            rsp = BuildErrorJson("batch submit only accepts batch SQL");
            return error::BAD_REQUEST;
        }
    }

    std::string submit_err;
    const int submit_rc = batch_runtime_.Submit(runtime_task_id, std::move(sqls), timeout_s, &submit_err);
    if (submit_rc != 0) {
        if (submit_rc == EEXIST) {
            rsp = BuildErrorJson("runtime_task_id already exists: " + runtime_task_id);
            return error::CONFLICT;
        }
        rsp = BuildErrorJson("batch submit failed: " + submit_err);
        return error::INTERNAL_ERROR;
    }
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("status");
    w.String("submitted");
    w.Key("runtime_task_id");
    w.String(runtime_task_id.c_str());
    w.Key("runtime_kind");
    w.String("batch");
    w.EndObject();
    rsp = buf.GetString();
    return error::OK;
}

int32_t SchedulerPlugin::HandleBatchStatus(const std::string&, const std::string& req_body, std::string& rsp) {
    rapidjson::Document doc;
    doc.Parse(req_body.c_str());
    if (doc.HasParseError() || !doc.IsObject()) {
        rsp = BuildErrorJson("invalid request body");
        return error::BAD_REQUEST;
    }
    std::string runtime_task_id;
    if (doc.HasMember("runtime_task_id") && doc["runtime_task_id"].IsString()) {
        runtime_task_id = doc["runtime_task_id"].GetString();
    } else if (doc.HasMember("task_id") && doc["task_id"].IsString()) {
        runtime_task_id = doc["task_id"].GetString();
    }
    if (runtime_task_id.empty()) {
        rsp = BuildErrorJson("runtime_task_id is required");
        return error::BAD_REQUEST;
    }

    BatchRuntimeSnapshot snapshot;
    const int query_rc = batch_runtime_.Query(runtime_task_id, &snapshot);
    if (query_rc != 0) {
        rsp = BuildErrorJson("batch runtime task not found: " + runtime_task_id);
        return error::NOT_FOUND;
    }
    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    batch_runtime_.SweepFinished(now_ms, stream_runtime_retention_s_, stream_runtime_max_count_);

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("runtime_task_id");
    w.String(snapshot.runtime_task_id.c_str());
    w.Key("runtime_kind");
    w.String("batch");
    w.Key("status");
    w.String(BatchRuntimeStatusName(snapshot.status));
    w.Key("error_code");
    w.String(snapshot.error_code.c_str());
    w.Key("error_message");
    w.String(snapshot.error_message.c_str());
    w.Key("error_stage");
    w.String(snapshot.error_stage.c_str());
    w.Key("current_sql_index");
    w.Int(snapshot.current_sql_index);
    w.Key("sql_count");
    w.Int(snapshot.sql_count);
    w.Key("timeout_s");
    w.Int(snapshot.timeout_s);
    w.Key("result_row_count");
    w.Int64(snapshot.result_row_count);
    w.Key("result_col_count");
    w.Int64(snapshot.result_col_count);
    w.Key("result_target");
    w.String(snapshot.result_target.c_str());
    w.Key("created_ms");
    w.Int64(snapshot.created_ms);
    w.Key("started_ms");
    w.Int64(snapshot.started_ms);
    w.Key("last_active_ms");
    w.Int64(snapshot.last_active_ms);
    w.Key("finished_ms");
    w.Int64(snapshot.finished_ms);
    w.EndObject();
    rsp = buf.GetString();
    return error::OK;
}

int32_t SchedulerPlugin::HandleBatchStop(const std::string&, const std::string& req_body, std::string& rsp) {
    rapidjson::Document doc;
    doc.Parse(req_body.c_str());
    if (doc.HasParseError() || !doc.IsObject()) {
        rsp = BuildErrorJson("invalid request body");
        return error::BAD_REQUEST;
    }
    std::string runtime_task_id;
    if (doc.HasMember("runtime_task_id") && doc["runtime_task_id"].IsString()) {
        runtime_task_id = doc["runtime_task_id"].GetString();
    } else if (doc.HasMember("task_id") && doc["task_id"].IsString()) {
        runtime_task_id = doc["task_id"].GetString();
    }
    if (runtime_task_id.empty()) {
        rsp = BuildErrorJson("runtime_task_id is required");
        return error::BAD_REQUEST;
    }

    std::string stop_err;
    const int stop_rc = batch_runtime_.RequestStop(runtime_task_id, &stop_err);
    if (stop_rc != 0) {
        rsp = BuildErrorJson("batch stop failed: " + stop_err);
        return stop_rc == ENOENT ? error::NOT_FOUND : error::BAD_REQUEST;
    }
    return HandleBatchStatus("", std::string("{\"runtime_task_id\":\"") + runtime_task_id + "\"}", rsp);
}

int32_t SchedulerPlugin::HandleStreamExecuteSingle(const rapidjson::Document& doc, std::string& rsp) {
    if (doc.HasMember("group_mode") ||
        doc.HasMember("dag") ||
        doc.HasMember("sql") ||
        doc.HasMember("sqls") ||
        doc.HasMember("share_set_ready_timeout_s")) {
        rsp = BuildExecutionErrorJson(
            "single execution accepts only sql_text and timeout_s",
            ErrorCodeId::kStreamGroupSqlTextInvalid,
            ErrorStageId::kRequest);
        return error::BAD_REQUEST;
    }
    if (!doc.HasMember("sql_text") || !doc["sql_text"].IsString()) {
        rsp = BuildExecutionErrorJson(
            "invalid request, expected {\"sql_text\":\"...\"}",
            ErrorCodeId::kStreamGroupSqlTextInvalid,
            ErrorStageId::kRequest);
        return error::BAD_REQUEST;
    }
    std::vector<std::string> sqls;
    SqlTextSplitError split_err;
    if (SplitSqlText(doc["sql_text"].GetString(), &sqls, &split_err) != 0) {
        std::string err = "invalid sql_text";
        if (!split_err.message.empty()) {
            err += ": " + split_err.message;
        }
        rsp = BuildExecutionErrorWithSqlIndexJson(
            err,
            ErrorCodeId::kStreamGroupSqlTextInvalid,
            ErrorStageId::kRequest,
            split_err.statement_index);
        return error::BAD_REQUEST;
    }
    if (sqls.size() != 1) {
        rsp = BuildExecutionErrorJson(
            "single execution requires exactly one SQL statement",
            ErrorCodeId::kStreamGroupSqlTextInvalid,
            ErrorStageId::kRequest);
        return error::BAD_REQUEST;
    }

    SqlParser parser;
    SqlStatement stmt = parser.Parse(sqls.front());
    if (!stmt.error.empty()) {
        rsp = BuildExecutionErrorJson(
            stmt.error,
            ErrorCodeId::kStreamGroupSqlTextInvalid,
            ErrorStageId::kParse);
        return error::BAD_REQUEST;
    }
    if (stmt.sources.empty() && !stmt.source.empty()) {
        stmt.sources.push_back(stmt.source);
    }
    if (stmt.sources.empty()) {
        rsp = BuildExecutionErrorJson(
            "source channel not found",
            ErrorCodeId::kStreamGroupSqlTextInvalid,
            ErrorStageId::kParse);
        return error::BAD_REQUEST;
    }
    return ExecuteStreamTask(stmt, rsp);
}

int32_t SchedulerPlugin::HandleStreamExecute(const std::string&, const std::string& req_body, std::string& rsp) {
    rapidjson::Document doc;
    doc.Parse(req_body.c_str());
    if (doc.HasParseError() || !doc.IsObject()) {
        rsp = BuildExecutionErrorJson(
            "invalid request body",
            ErrorCodeId::kStreamGroupSqlTextInvalid,
            ErrorStageId::kRequest);
        return error::BAD_REQUEST;
    }
    if (doc.HasMember("task_id")) {
        rsp = BuildExecutionErrorJson(
            "external task_id is not allowed",
            ErrorCodeId::kStreamGroupSqlTextInvalid,
            ErrorStageId::kRequest);
        return error::BAD_REQUEST;
    }

    std::string execution_kind = "single";
    if (doc.HasMember("execution_kind")) {
        if (!doc["execution_kind"].IsString()) {
            rsp = BuildExecutionErrorJson(
                "execution_kind must be string",
                ErrorCodeId::kStreamGroupSqlTextInvalid,
                ErrorStageId::kRequest);
            return error::BAD_REQUEST;
        }
        execution_kind = ToLowerAscii(doc["execution_kind"].GetString());
    }

    if (execution_kind == "single") {
        return HandleStreamExecuteSingle(doc, rsp);
    }
    if (execution_kind == "group") {
        return HandleStreamExecuteGroup(doc, rsp);
    }

    rsp = BuildExecutionErrorJson(
        "unsupported execution_kind: " + execution_kind,
        ErrorCodeId::kStreamGroupSqlTextInvalid,
        ErrorStageId::kRequest);
    return error::BAD_REQUEST;
}

int32_t SchedulerPlugin::HandleStreamStop(const std::string&, const std::string& req, std::string& rsp) {
    SweepFinishedTaskLeases();
    SweepRuntimeRetainedObjects();
    rapidjson::Document doc;
    doc.Parse(req.c_str());
    if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("task_id") || !doc["task_id"].IsString()) {
        rsp = BuildErrorJson("invalid request, expected {\"task_id\":\"...\"}");
        return error::BAD_REQUEST;
    }
    const std::string task_id = doc["task_id"].GetString();
    {
        std::lock_guard<std::mutex> lock(stream_group_nodes_mu_);
        if (stream_group_node_owners_.find(task_id) != stream_group_node_owners_.end()) {
            rsp = BuildErrorJson("group node runtime_task_id is internal; use group task_id");
            return error::BAD_REQUEST;
        }
    }

    std::shared_ptr<StreamTaskGroup> group;
    {
        std::lock_guard<std::mutex> lock(stream_task_groups_mu_);
        auto it = stream_task_groups_.find(task_id);
        if (it != stream_task_groups_.end()) {
            group = it->second;
        }
    }
    if (group) {
        group->RequestStop();
        group->Join();
        StreamGroupSnapshot snapshot = group->Snapshot();
        TouchRuntimeAccess(task_id);
        if (IsTerminalStreamGroupStatus(snapshot.status)) {
            MarkRuntimeTerminal(task_id, "group");
        }
        const auto node_sources = QueryGroupNodeResolvedSources(task_id);
        const auto share_sets = QueryGroupShareSetSnapshots(task_id);
        CleanupGroupRuntimeResources(task_id, &snapshot);
        rapidjson::StringBuffer buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(buf);
        WriteGroupSnapshotJson(&w, snapshot, &share_sets,
                               node_sources.empty() ? nullptr : &node_sources);
        rsp = buf.GetString();
        SweepRuntimeRetainedObjects();
        return error::OK;
    }

    std::shared_ptr<StreamTask> task;
    {
        std::lock_guard<std::mutex> lock(stream_tasks_mu_);
        auto it = stream_tasks_.find(task_id);
        if (it == stream_tasks_.end()) {
            rsp = BuildErrorJson("stream task not found: " + task_id);
            return error::NOT_FOUND;
        }
        task = it->second;
    }

    task->RequestStop();
    task->Join();
    ReleaseStreamTaskLeases(task_id);
    TouchRuntimeAccess(task_id);
    MarkRuntimeTerminal(task_id, "single");

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    WriteTaskSnapshotJson(&w, task->Snapshot(), nullptr);
    rsp = buf.GetString();
    SweepRuntimeRetainedObjects();
    return error::OK;
}

int32_t SchedulerPlugin::HandleStreamStatus(const std::string&, const std::string& req, std::string& rsp) {
    SweepFinishedTaskLeases();
    SweepRuntimeRetainedObjects();
    rapidjson::Document doc;
    doc.Parse(req.c_str());
    if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("task_id") || !doc["task_id"].IsString()) {
        rsp = BuildErrorJson("invalid request, expected {\"task_id\":\"...\"}");
        return error::BAD_REQUEST;
    }
    const std::string task_id = doc["task_id"].GetString();
    {
        std::lock_guard<std::mutex> lock(stream_group_nodes_mu_);
        if (stream_group_node_owners_.find(task_id) != stream_group_node_owners_.end()) {
            rsp = BuildErrorJson("group node runtime_task_id is internal; use group task_id");
            return error::BAD_REQUEST;
        }
    }

    std::shared_ptr<StreamTaskGroup> group;
    {
        std::lock_guard<std::mutex> lock(stream_task_groups_mu_);
        auto it = stream_task_groups_.find(task_id);
        if (it != stream_task_groups_.end()) {
            group = it->second;
        }
    }
    if (group) {
        StreamGroupSnapshot snapshot = group->Snapshot();
        TouchRuntimeAccess(task_id);
        if (IsTerminalStreamGroupStatus(snapshot.status)) {
            MarkRuntimeTerminal(task_id, "group");
        }
        const auto node_sources = QueryGroupNodeResolvedSources(task_id);
        const auto share_sets = QueryGroupShareSetSnapshots(task_id);
        if (IsTerminalStreamGroupStatus(snapshot.status)) {
            CleanupGroupRuntimeResources(task_id, &snapshot);
        }
        rapidjson::StringBuffer buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(buf);
        WriteGroupSnapshotJson(&w, snapshot, &share_sets,
                               node_sources.empty() ? nullptr : &node_sources);
        rsp = buf.GetString();
        SweepRuntimeRetainedObjects();
        return error::OK;
    }

    std::shared_ptr<StreamTask> task;
    {
        std::lock_guard<std::mutex> lock(stream_tasks_mu_);
        auto it = stream_tasks_.find(task_id);
        if (it == stream_tasks_.end()) {
            rsp = BuildErrorJson("stream task not found: " + task_id);
            return error::NOT_FOUND;
        }
        task = it->second;
    }

    TaskSnapshot snapshot = task->Snapshot();
    TouchRuntimeAccess(task_id);
    if (IsTerminalStreamTaskStatus(snapshot.status)) {
        MarkRuntimeTerminal(task_id, "single");
        ReleaseStreamTaskLeases(task_id);
    }

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    SharedHubSnapshot shared_hub_snapshot;
    SharedHubSnapshot* shared_hub_ptr = nullptr;
    if (QueryRuntimeSharedHubSnapshot(task_id, &shared_hub_snapshot) == 0) {
        shared_hub_ptr = &shared_hub_snapshot;
    }
    WriteTaskSnapshotJson(&w, snapshot, shared_hub_ptr);
    rsp = buf.GetString();
    SweepRuntimeRetainedObjects();
    return error::OK;
}

int32_t SchedulerPlugin::HandleStreamList(const std::string&, const std::string&, std::string& rsp) {
    SweepFinishedTaskLeases();
    SweepRuntimeRetainedObjects();
    std::vector<TaskSnapshot> snapshots;
    std::vector<StreamGroupSnapshot> group_snapshots;
    std::vector<std::string> terminal_tasks;
    std::unordered_set<std::string> internal_node_ids;
    {
        std::lock_guard<std::mutex> lock(stream_group_nodes_mu_);
        internal_node_ids.reserve(stream_group_node_owners_.size());
        for (const auto& kv : stream_group_node_owners_) {
            internal_node_ids.insert(kv.first);
        }
    }
    {
        std::lock_guard<std::mutex> lock(stream_tasks_mu_);
        snapshots.reserve(stream_tasks_.size());
        for (const auto& kv : stream_tasks_) {
            if (internal_node_ids.count(kv.first) > 0) continue;
            if (!kv.second) continue;
            TaskSnapshot s = kv.second->Snapshot();
            TouchRuntimeAccess(kv.first);
            if (IsTerminalStreamTaskStatus(s.status)) {
                terminal_tasks.push_back(kv.first);
                MarkRuntimeTerminal(kv.first, "single");
            }
            snapshots.push_back(std::move(s));
        }
    }
    {
        std::lock_guard<std::mutex> lock(stream_task_groups_mu_);
        group_snapshots.reserve(stream_task_groups_.size());
        for (const auto& kv : stream_task_groups_) {
            if (!kv.second) continue;
            StreamGroupSnapshot s = kv.second->Snapshot();
            TouchRuntimeAccess(kv.first);
            if (IsTerminalStreamGroupStatus(s.status)) {
                MarkRuntimeTerminal(kv.first, "group");
                for (const auto& node : s.nodes) {
                    terminal_tasks.push_back(node.runtime_task_id);
                }
            }
            group_snapshots.push_back(std::move(s));
        }
    }
    for (const auto& task_id : terminal_tasks) {
        ReleaseStreamTaskLeases(task_id);
    }

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("tasks");
    w.StartArray();
    for (const auto& s : snapshots) {
        SharedHubSnapshot shared_hub_snapshot;
        SharedHubSnapshot* shared_hub_ptr = nullptr;
        if (QueryRuntimeSharedHubSnapshot(s.task_id, &shared_hub_snapshot) == 0) {
            shared_hub_ptr = &shared_hub_snapshot;
        }
        WriteTaskSnapshotJson(&w, s, shared_hub_ptr);
    }
    for (const auto& s : group_snapshots) {
        const auto node_sources = QueryGroupNodeResolvedSources(s.task_id);
        const auto share_sets = QueryGroupShareSetSnapshots(s.task_id);
        if (IsTerminalStreamGroupStatus(s.status)) {
            CleanupGroupRuntimeResources(s.task_id, &s);
        }
        WriteGroupSnapshotJson(&w, s, &share_sets, node_sources.empty() ? nullptr : &node_sources);
    }
    w.EndArray();
    w.EndObject();
    rsp = buf.GetString();
    SweepRuntimeRetainedObjects();
    return error::OK;
}

// 逻辑链：
// 1) 解析并校验 SQL，请求路由先区分 stream/batch 源类型；
// 2) batch 路径解析算子链并绑定 source/sink；
// 3) 执行纯传输或算子链执行，统一抽取错误阶段并返回标准错误结构；
// 4) INTO dataframe.<name> 场景写回注册中心并组织最终响应。
int32_t SchedulerPlugin::HandleExecute(const std::string&, const std::string& req_body, std::string& rsp) {
    auto* ch_registry = querier_ ? static_cast<IChannelRegistry*>(querier_->First(IID_CHANNEL_REGISTRY)) : nullptr;
    rapidjson::Document doc;
    doc.Parse(req_body.c_str());
    if (doc.HasParseError() || !doc.HasMember("sql") || !doc["sql"].IsString()) {
        rsp = BuildErrorJson("invalid request, expected {\"sql\":\"...\"}");
        return error::BAD_REQUEST;
    }
    std::string sql_text = doc["sql"].GetString();

    static constexpr size_t kMaxSqlLength = 64 * 1024;
    if (sql_text.size() > kMaxSqlLength) {
        rsp = BuildErrorJson("SQL too long (max 64KB)");
        return error::BAD_REQUEST;
    }

    SqlParser parser;
    auto stmt = parser.Parse(sql_text);
    if (!stmt.error.empty()) {
        rsp = BuildErrorJson(stmt.error);
        return error::BAD_REQUEST;
    }

    if (stmt.sources.empty() && !stmt.source.empty()) {
        stmt.sources.push_back(stmt.source);
    }
    if (stmt.sources.empty()) {
        rsp = BuildErrorJson("source channel not found");
        return error::BAD_REQUEST;
    }

    SourceResolveResult source_resolved;
    std::string source_err_rsp;
    const int32_t source_rc = ResolveSourceBindings(stmt, &source_resolved, &source_err_rsp);
    if (source_rc != error::OK) {
        rsp = source_err_rsp.empty() ? BuildErrorJson("source resolve failed") : source_err_rsp;
        return source_rc;
    }
    if (source_resolved.has_stream_source && source_resolved.has_non_stream_source) {
        rsp = BuildErrorJson("mixed stream and non-stream sources are not supported");
        return error::BAD_REQUEST;
    }
    if (source_resolved.has_block_source && source_resolved.has_stream_source) {
        rsp = BuildErrorJson("mixed block and stream sources are not supported");
        return error::BAD_REQUEST;
    }
    if (source_resolved.has_block_source && source_resolved.has_non_stream_source) {
        rsp = BuildErrorJson("mixed block and batch sources are not supported");
        return error::BAD_REQUEST;
    }
    if (source_resolved.has_stream_source) {
        return ExecuteStreamTask(stmt, rsp);
    }

    std::vector<IChannel*> input_channels = source_resolved.channels;

    std::vector<std::shared_ptr<IOperator>> op_holders;
    std::vector<IOperator*> op_chain;
    std::vector<OperatorRef> parsed_ops = stmt.operators;
    if (parsed_ops.empty() && !stmt.op_category.empty() && !stmt.op_name.empty()) {
        parsed_ops.push_back({stmt.op_category, stmt.op_name});
    }
    std::unique_ptr<void, std::function<void(void*)>> block_lease_guard(nullptr, [](void*) {});
    std::shared_ptr<IBlockStreamChannel> exclusive_block_reader;
    std::shared_ptr<const BoundFilterExpr> block_source_residual;
    if (source_resolved.has_block_source) {
        if (source_resolved.block_channels.size() != 1) {
            rsp = BuildErrorJson("block stream source currently supports one source");
            return error::BAD_REQUEST;
        }
        const std::string block_runtime_id = NextStreamTaskId();
        BlockSourceFilterPlan source_filter_plan;
        std::string source_plan_error;
        const int source_plan_rc = BuildBlockSourceFilterPlan(
            source_resolved.block_channels.front().get(), stmt,
            &source_filter_plan, &source_plan_error);
        if (source_plan_rc != 0) {
            rsp = BuildExecutionErrorJson(
                source_plan_error.empty() ? "block source filter planning failed"
                                          : source_plan_error,
                source_plan_rc == EINVAL ? ErrorCodeId::kSqlTextInvalid
                                         : ErrorCodeId::kOpExecFail,
                ErrorStageId::kCapabilityCheck);
            return source_plan_rc == EINVAL ? error::BAD_REQUEST
                                            : error::INTERNAL_ERROR;
        }
        std::string reader_error;
        const int reader_rc = CreateExclusiveBlockReader(
            querier_, block_runtime_id, source_resolved.block_channels.front().get(),
            source_filter_plan.pushed_filter_plan_json,
            &exclusive_block_reader, &reader_error);
        if (reader_rc == 0) {
            block_source_residual = std::move(source_filter_plan.exclusive_residual);
            source_resolved.block_channels.front() = exclusive_block_reader;
            input_channels.front() = exclusive_block_reader.get();
        } else if (reader_rc == ENOTSUP) {
            block_source_residual = std::move(source_filter_plan.shared_residual);
            std::string conflict_key;
            bool blocked_by_mutation = false;
            const int lease_rc = TryAcquireStreamTaskLeases(block_runtime_id,
                                                            source_resolved.source_keys,
                                                            {},
                                                            &conflict_key,
                                                            &blocked_by_mutation);
            if (lease_rc != 0) {
                rsp = BuildExecutionErrorJson(
                    blocked_by_mutation ? "block source is being modified: " + conflict_key
                                        : "block source is in use: " + conflict_key,
                    blocked_by_mutation ? ErrorCodeId::kStreamChannelMutating
                                        : ErrorCodeId::kStreamSourceInUse,
                    ErrorStageId::kLease);
                return error::CONFLICT;
            }
            block_lease_guard = std::unique_ptr<void, std::function<void(void*)>>(
                reinterpret_cast<void*>(1),
                [this, block_runtime_id](void*) {
                    ReleaseStreamTaskLeases(block_runtime_id);
                });
        } else {
            rsp = BuildExecutionErrorJson(
                reader_error.empty() ? "block stream reader factory failed"
                                     : reader_error,
                ErrorCodeId::kOpExecFail,
                ErrorStageId::kCapabilityCheck);
            return reader_rc == EEXIST ? error::CONFLICT : error::INTERNAL_ERROR;
        }
    }
    if (source_resolved.has_block_source && !parsed_ops.empty()) {
        std::vector<IBlockTransformOperatorV1*> transform_providers;
        transform_providers.reserve(parsed_ops.size());
        size_t transform_matches = 0;
        for (const auto& op_ref : parsed_ops) {
            bool transform_ambiguous = false;
            int transform_traverse_error = 0;
            IBlockTransformOperatorV1* transform_provider = FindBlockTransformOperator(
                op_ref.category,
                op_ref.name,
                &transform_ambiguous,
                &transform_traverse_error);
            if (transform_traverse_error != 0) {
                rsp = BuildExecutionErrorJson(
                    "block transform operator discovery failed with code " +
                        std::to_string(transform_traverse_error),
                    ErrorCodeId::kOpExecFail,
                    ErrorStageId::kCapabilityCheck);
                return error::INTERNAL_ERROR;
            }
            if (transform_ambiguous) {
                rsp = BuildErrorJson("multiple block transform operators matched: " +
                                     op_ref.category + "." + op_ref.name);
                return error::CONFLICT;
            }
            transform_providers.push_back(transform_provider);
            if (transform_provider) ++transform_matches;
        }
        if (transform_matches != parsed_ops.size() &&
            (transform_matches != 0 || parsed_ops.size() > 1)) {
            for (size_t i = 0; i < transform_providers.size(); ++i) {
                if (transform_providers[i]) continue;
                rsp = BuildErrorJson("block transform operator not found: " +
                                     parsed_ops[i].category + "." + parsed_ops[i].name);
                return error::NOT_FOUND;
            }
        }
        if (transform_matches == parsed_ops.size()) {
            if (!stmt.dest.empty() && !IsDataframeRefName(stmt.dest)) {
                rsp = BuildErrorJson("block transform currently requires a DataFrame destination");
                return error::BAD_REQUEST;
            }
            if (!stmt.dest.empty() && !ch_registry) {
                rsp = BuildErrorJson("channel registry unavailable");
                return error::INTERNAL_ERROR;
            }

            const bool named_result = !stmt.dest.empty();
            const std::string dataframe_name = named_result
                                                   ? DataframeNamePart(stmt.dest)
                                                   : std::string("sink");
            auto dataframe_sink = std::make_shared<DataFrameChannel>(
                named_result ? "dataframe" : "_temp", dataframe_name);
            if (dataframe_sink->Open() != 0) {
                rsp = BuildErrorJson("failed to open block transform DataFrame sink");
                return error::INTERNAL_ERROR;
            }

            int64_t rows = 0;
            std::string transform_error;
            BlockExecutionTerminal transform_terminal = BlockExecutionTerminal::kFailed;
            int transform_rc = 0;
            try {
                transform_rc = ExecuteBlockTransformPipeline(
                    source_resolved.block_channels.front().get(),
                    transform_providers,
                    dataframe_sink.get(),
                    stmt,
                    block_source_residual,
                    &transform_terminal,
                    &rows,
                    &transform_error);
            } catch (const std::exception& ex) {
                transform_error = std::string("block transform execution threw: ") + ex.what();
                transform_rc = EFAULT;
            } catch (...) {
                transform_error = "block transform execution threw an unknown exception";
                transform_rc = EFAULT;
            }
            if (transform_terminal == BlockExecutionTerminal::kFailed ||
                (transform_terminal != BlockExecutionTerminal::kCancelled &&
                 transform_rc != 0)) {
                rsp = BuildExecutionErrorJson(
                    transform_error.empty()
                        ? "block transform execution failed"
                        : transform_error,
                    transform_rc == EINVAL ? ErrorCodeId::kSqlTextInvalid
                                           : ErrorCodeId::kOpExecFail,
                    transform_rc == EINVAL ? ErrorStageId::kCapabilityCheck
                                           : ErrorStageId::kExecute);
                return transform_rc == EINVAL ? error::BAD_REQUEST
                                              : error::INTERNAL_ERROR;
            }

            if (named_result) {
                if (ch_registry->Get(dataframe_name.c_str())) {
                    (void)ch_registry->Unregister(dataframe_name.c_str());
                }
                if (ch_registry->Register(
                        dataframe_name.c_str(),
                        std::static_pointer_cast<IChannel>(dataframe_sink)) != 0) {
                    rsp = BuildErrorJson("failed to register dataframe channel: " +
                                         dataframe_name);
                    return error::INTERNAL_ERROR;
                }
            }

            const char* status = "completed";
            if (transform_terminal == BlockExecutionTerminal::kStopped) {
                status = "stopped";
            } else if (transform_terminal == BlockExecutionTerminal::kCancelled) {
                status = "cancelled";
            }
            DataFrame result;
            std::string result_json = "[]";
            if (!named_result && dataframe_sink->Read(&result) == 0 &&
                result.RowCount() > 0) {
                result_json = result.ToJson();
            }
            rapidjson::StringBuffer transform_buf;
            rapidjson::Writer<rapidjson::StringBuffer> transform_writer(transform_buf);
            transform_writer.StartObject();
            transform_writer.Key("status");
            transform_writer.String(status);
            transform_writer.Key("rows");
            transform_writer.Int64(rows);
            transform_writer.Key("result_row_count");
            transform_writer.Int64(rows);
            transform_writer.Key("result_target");
            transform_writer.String(stmt.dest.c_str());
            if (!named_result) {
                transform_writer.Key("data");
                transform_writer.RawValue(
                    result_json.c_str(), result_json.size(), rapidjson::kArrayType);
            }
            transform_writer.EndObject();
            rsp = transform_buf.GetString();
            return error::OK;
        }

        if (parsed_ops.size() != 1) {
            rsp = BuildErrorJson(
                "block stream terminal operator path currently supports one operator");
            return error::BAD_REQUEST;
        }

        IBlockStreamOperator* block_op = FindBlockOperator(parsed_ops[0].category, parsed_ops[0].name);
        if (!block_op) {
            rsp = BuildErrorJson("block stream operator not found: " + parsed_ops[0].category + "." + parsed_ops[0].name);
            return error::NOT_FOUND;
        }
        try {
            const auto& params = !stmt.operator_with_params.empty() ? stmt.operator_with_params[0] : stmt.with_params;
            for (const auto& kv : params) {
                if (block_op->Configure(kv.first.c_str(), kv.second.c_str()) != 0) {
                    rsp = BuildErrorJson("block stream operator configuration failed");
                    return error::BAD_REQUEST;
                }
            }
            if (block_op->Init("{}") != 0) {
                rsp = BuildErrorJson("block stream operator initialization failed");
                return error::INTERNAL_ERROR;
            }
            int64_t rows = 0;
            std::string block_error;
            BlockExecutionTerminal block_terminal = BlockExecutionTerminal::kFailed;
            const int block_rc = ExecuteBlockOperator(
                source_resolved.block_channels.front().get(), block_op,
                block_source_residual, &block_terminal, &rows, &block_error);
            if (block_terminal == BlockExecutionTerminal::kFailed ||
                (block_terminal != BlockExecutionTerminal::kCancelled && block_rc != 0)) {
                rsp = BuildExecutionErrorJson(block_error.empty() ? "block stream execution failed" : block_error,
                                              ErrorCodeId::kOpExecFail, ErrorStageId::kExecute);
                return error::INTERNAL_ERROR;
            }
            const char* block_status = "completed";
            if (block_terminal == BlockExecutionTerminal::kStopped) {
                block_status = "stopped";
            } else if (block_terminal == BlockExecutionTerminal::kCancelled) {
                block_status = "cancelled";
            }
            rapidjson::StringBuffer block_buf;
            rapidjson::Writer<rapidjson::StringBuffer> block_writer(block_buf);
            block_writer.StartObject();
            block_writer.Key("status"); block_writer.String(block_status);
            block_writer.Key("rows"); block_writer.Int64(rows);
            block_writer.Key("result_row_count"); block_writer.Int64(rows);
            block_writer.EndObject();
            rsp = block_buf.GetString();
            return error::OK;
        } catch (const std::exception& e) {
            rsp = BuildExecutionErrorJson(e.what(), ErrorCodeId::kOpExecFail, ErrorStageId::kExecute);
            return error::INTERNAL_ERROR;
        } catch (...) {
            rsp = BuildExecutionErrorJson("block stream execution exception", ErrorCodeId::kOpExecFail, ErrorStageId::kExecute);
            return error::INTERNAL_ERROR;
        }
    }
    if (!parsed_ops.empty()) {
        auto* catalog = querier_ ? static_cast<IOperatorCatalog*>(querier_->First(IID_OPERATOR_CATALOG)) : nullptr;
        if (!catalog) {
            rsp = BuildErrorJson("operator catalog unavailable");
            return error::UNAVAILABLE;
        }
        for (const auto& op_ref : parsed_ops) {
            OperatorStatus status = catalog->QueryStatus(op_ref.category, op_ref.name);
            if (status == OperatorStatus::kNotFound) {
                rsp = BuildErrorJson("operator not found: " + op_ref.category + "." + op_ref.name);
                return error::NOT_FOUND;
            }
            if (status == OperatorStatus::kDeactivated) {
                rsp = BuildErrorJson("operator is deactivated: " + op_ref.category + "." + op_ref.name);
                return error::CONFLICT;
            }
            auto holder = FindOperator(op_ref.category, op_ref.name);
            if (!holder) {
                rsp = BuildErrorJson("operator not found: " + op_ref.category + "." + op_ref.name);
                return error::NOT_FOUND;
            }
            op_chain.push_back(holder.get());
            op_holders.push_back(std::move(holder));
        }
    }

    try {
        if (!op_chain.empty()) {
            for (size_t i = 0; i < op_chain.size(); ++i) {
                const auto& params = (i < stmt.operator_with_params.size())
                    ? stmt.operator_with_params[i]
                    : (i == 0 ? stmt.with_params : std::unordered_map<std::string, std::string>{});
                for (const auto& kv : params) {
                    op_chain[i]->Configure(kv.first.c_str(), kv.second.c_str());
                }
            }
        }

        std::shared_ptr<DataFrameChannel> temp_sink;
        std::shared_ptr<IDataFrameChannel> named_df_sink;
        std::shared_ptr<IChannel> named_sink_holder;
        IChannel* sink = nullptr;

        if (!stmt.dest.empty()) {
            if (!IsQualifiedDestination(stmt.dest)) {
                rsp = BuildErrorJson("invalid INTO destination: " + stmt.dest +
                                    ", expected dataframe.<name> or <type>.<name>[.<table>]");
                return error::BAD_REQUEST;
            }
            if (IsDataframeRefName(stmt.dest)) {
                if (!ch_registry) {
                    rsp = BuildErrorJson("channel registry unavailable");
                    return error::INTERNAL_ERROR;
                }
                std::string df_name = DataframeNamePart(stmt.dest);
                auto ch = std::make_shared<DataFrameChannel>("dataframe", df_name);
                ch->Open();
                named_df_sink = ch;
                sink = ch.get();
            } else {
                sink = FindChannel(stmt.dest, &named_sink_holder);
                if (!sink) {
                    rsp = BuildErrorJson("destination channel not found: " + stmt.dest);
                    return error::NOT_FOUND;
                }
            }
        } else {
            temp_sink = std::make_shared<DataFrameChannel>("_temp", "sink");
            temp_sink->Open();
            sink = temp_sink.get();
        }

        int rc = 0;
        int64_t affected_rows = 0;
        std::string exec_error;
        std::string sink_type(sink->Type());

        if (input_channels.size() > 1 && op_chain.empty()) {
            rsp = BuildErrorJson("multi-source FROM requires USING operator");
            return error::BAD_REQUEST;
        }
        if (input_channels.size() > 1) {
            for (const auto& source_name : stmt.sources) {
                if (!IsDataframeRefName(source_name)) {
                    rsp = BuildErrorJson("multi-source FROM only supports dataframe.* in Sprint 10");
                    return error::BAD_REQUEST;
                }
            }
            if (!stmt.where_clause.empty()) {
                rsp = BuildErrorJson("multi-source FROM does not support WHERE in Sprint 10");
                return error::BAD_REQUEST;
            }
        }

        if (op_chain.empty()) {
            if (input_channels.size() != 1) {
                rsp = BuildErrorJson("invalid source count");
                return error::BAD_REQUEST;
            }
            IChannel* source = input_channels[0];
            std::string source_type(source->Type());
            rc = ExecuteTransfer(source, sink, source_type, sink_type, stmt,
                                 block_source_residual, &affected_rows, &exec_error);
        } else {
            rc = ExecuteWithOperatorChain(Span<IChannel*>(input_channels), sink, op_chain, sink_type, stmt,
                                          &affected_rows, &exec_error);
        }

        if (rc != 0) {
            std::string err = exec_error;
            if (err.empty() && !op_chain.empty()) err = op_chain.back()->LastError();
            if (err.empty()) err = "execution failed";
            std::string stage = ExtractStageFromExecutionError(err);
            if (stage.empty()) stage = "execute";
            rsp = BuildExecutionErrorJson(err, ErrorCodeId::kOpExecFail, stage);
            return error::INTERNAL_ERROR;
        }

        // INTO dataframe.<name>：覆盖语义（已存在则先注销，再注册新结果）
        if (!stmt.dest.empty() && IsDataframeRefName(stmt.dest) && named_df_sink) {
            std::string df_name = DataframeNamePart(stmt.dest);
            if (ch_registry->Get(df_name.c_str())) {
                (void)ch_registry->Unregister(df_name.c_str());
            }
            if (ch_registry->Register(df_name.c_str(), std::static_pointer_cast<IChannel>(named_df_sink)) != 0) {
                rsp = BuildErrorJson("failed to register dataframe channel: " + df_name);
                return error::INTERNAL_ERROR;
            }
            auto registered = ch_registry->Get(df_name.c_str());
            if (!registered) {
                rsp = BuildErrorJson("failed to fetch registered dataframe channel: " + df_name);
                return error::INTERNAL_ERROR;
            }
            auto* registered_df = dynamic_cast<IDataFrameChannel*>(registered.get());
            if (!registered_df) {
                rsp = BuildErrorJson("registered channel is not dataframe: " + df_name);
                return error::INTERNAL_ERROR;
            }
            sink = registered_df;
            sink_type = sink->Type();
        }

        auto* df_sink = dynamic_cast<IDataFrameChannel*>(sink);
        DataFrame result;
        std::string result_json = "[]";
        int64_t row_count = 0;
        const bool named_dataframe_result = !stmt.dest.empty() && IsDataframeRefName(stmt.dest);
        if (df_sink && df_sink->Read(&result) == 0 && result.RowCount() > 0) {
            row_count = result.RowCount();
            if (!named_dataframe_result) result_json = result.ToJson();
        } else if (sink_type == ChannelType::kDatabase) {
            row_count = affected_rows;
        }

        rapidjson::StringBuffer buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(buf);
        w.StartObject();
        w.Key("status"); w.String("completed");
        w.Key("rows"); w.Int64(row_count);
        w.Key("result_row_count"); w.Int64(row_count);
        w.Key("result_target"); w.String(stmt.dest.c_str());
        if (!named_dataframe_result) {
            w.Key("data"); w.RawValue(result_json.c_str(), result_json.size(), rapidjson::kArrayType);
        }
        w.EndObject();
        rsp = buf.GetString();
        return error::OK;

    } catch (const std::exception& e) {
        std::string err = std::string("internal error: ") + e.what();
        LOG_ERROR("SchedulerPlugin::HandleExecute: exception: %s", err.c_str());
        rsp = BuildErrorJson(err);
        return error::INTERNAL_ERROR;
    } catch (...) {
        LOG_ERROR("SchedulerPlugin::HandleExecute: unknown exception");
        rsp = BuildErrorJson("internal error: unknown exception");
        return error::INTERNAL_ERROR;
    }
}

// --- HandleGetChannels ---
int32_t SchedulerPlugin::HandleGetChannels(const std::string&, const std::string&, std::string& rsp) {
    auto* ch_registry = querier_ ? static_cast<IChannelRegistry*>(querier_->First(IID_CHANNEL_REGISTRY)) : nullptr;
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartArray();

    // 内部通道表
    auto managed_snapshot = SnapshotManagedChannels();
    for (auto& [key, ch_ptr] : managed_snapshot) {
        auto* ch = ch_ptr.get();
        w.StartObject();
        w.Key("category"); w.String(ch->Category());
        w.Key("name"); w.String(ch->Name());
        w.Key("type"); w.String(ch->Type());
        w.Key("schema"); w.String(ch->Schema());
        w.EndObject();
    }

    // 具名 DataFrame 通道（CatalogPlugin 注册中心）
    if (ch_registry) {
        ch_registry->List([&w](const char* name, std::shared_ptr<IChannel> ch) {
            if (!name || !ch) return;
            w.StartObject();
            w.Key("category"); w.String(ch->Category());
            w.Key("name"); w.String(name);
            w.Key("type"); w.String(ch->Type());
            w.Key("schema"); w.String(ch->Schema());
            w.EndObject();
        });
    }

    // 静态注册的通道（通过 IQuerier）
    if (querier_) {
        querier_->Traverse(IID_CHANNEL, [&w](void* p) -> int {
            auto* ch = static_cast<IChannel*>(p);
            w.StartObject();
            w.Key("category"); w.String(ch->Category());
            w.Key("name"); w.String(ch->Name());
            w.Key("type"); w.String(ch->Type());
            w.Key("schema"); w.String(ch->Schema());
            w.EndObject();
            return 0;
        });

        // 数据库通道（通过 IDatabaseFactory）
        auto* factory = static_cast<IDatabaseFactory*>(querier_->First(IID_DATABASE_FACTORY));
        if (factory) {
            factory->List([&w](const char* type, const char* name, const char* config_json) {
                w.StartObject();
                w.Key("category"); w.String(type);
                w.Key("name"); w.String(name);
                w.Key("type"); w.String(ChannelType::kDatabase);
                // 从 config_json 提取 database 字段作为 schema 展示
                std::string db_label;
                if (config_json) {
                    rapidjson::Document cfg;
                    cfg.Parse(config_json);
                    if (!cfg.HasParseError() && cfg.IsObject()) {
                        if (cfg.HasMember("database") && cfg["database"].IsString()) {
                            db_label = cfg["database"].GetString();
                        } else if (cfg.HasMember("path") && cfg["path"].IsString()) {
                            db_label = cfg["path"].GetString();
                        }
                    }
                }
                w.Key("schema"); w.String(db_label.c_str());
                w.EndObject();
            });
        }

        // 流式通道（通过 IStreamFactory）
        auto* stream_factory = static_cast<IStreamFactory*>(querier_->First(IID_STREAM_FACTORY));
        if (stream_factory) {
            stream_factory->List([&w](const char* type, const char* name, IStreamChannel* stream_ch) {
                if (!stream_ch || !type || !name) return;
                w.StartObject();
                w.Key("category"); w.String(type);
                w.Key("name"); w.String(name);
                w.Key("type"); w.String(stream_ch->Type());
                w.Key("schema"); w.String(stream_ch->Schema());
                w.EndObject();
            });
        }

        TraverseBlockFactories([&w](IBlockStreamFactory* factory) -> int {
            factory->List([&w](const char* type, const char* name, IBlockStreamChannel* channel) {
                if (!type || !name || !channel) return;
                w.StartObject();
                w.Key("category"); w.String(channel->Category());
                w.Key("name"); w.String(channel->Name());
                w.Key("type"); w.String(channel->Type());
                w.Key("schema"); w.String(channel->Schema());
                w.EndObject();
            });
            return 0;
        });
    }

    w.EndArray();
    rsp = buf.GetString();
    return error::OK;
}

// --- HandleQueryStreamChannelDefinitions ---
int32_t SchedulerPlugin::HandleQueryStreamChannelDefinitions(const std::string&,
                                                             const std::string&,
                                                             std::string& rsp) {
    auto* builtin_registry = querier_ ? static_cast<IBuiltinRegistry*>(querier_->First(IID_BUILTIN_REGISTRY)) : nullptr;
    if (!builtin_registry) {
        rsp = BuildErrorJson("builtin registry unavailable");
        return error::UNAVAILABLE;
    }

    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("definitions");
    w.StartArray();

    builtin_registry->ListStreamChannelTypes([&w](const StreamChannelTypeDescriptor& def) {
        w.StartObject();
        w.Key("channel_type");
        w.String(def.type.c_str());
        w.Key("display_name");
        w.String(def.display_name.c_str());

        w.Key("allowed_roles");
        w.StartArray();
        for (const auto& role : def.allowed_roles) {
            w.String(role.c_str());
        }
        w.EndArray();

        w.Key("option_schema");
        w.StartArray();
        for (const auto& field : def.option_schema) {
            w.StartObject();
            w.Key("key");
            w.String(field.key.c_str());
            w.Key("type");
            w.String(field.type.c_str());
            w.Key("required");
            w.Bool(field.required);
            w.Key("default_value");
            w.String(field.default_value.c_str());
            w.Key("enum_values");
            w.StartArray();
            for (const auto& value : field.enum_values) {
                w.String(value.c_str());
            }
            w.EndArray();
            w.Key("min_value");
            w.Int64(field.min_value);
            w.Key("max_value");
            w.Int64(field.max_value);
            w.Key("has_range");
            w.Bool(field.has_range);
            w.Key("power_of_two");
            w.Bool(field.power_of_two);
            w.Key("desc");
            w.String(field.desc.c_str());
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

// --- HandleQueryStreamChannels ---
int32_t SchedulerPlugin::HandleQueryStreamChannels(const std::string&,
                                                   const std::string&,
                                                   std::string& rsp) {
    SweepFinishedTaskLeases();
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("channels");
    w.StartArray();

    auto* stream_manager = querier_ ? static_cast<IStreamManager*>(querier_->First(IID_STREAM_MANAGER)) : nullptr;
    auto* stream_factory = querier_ ? static_cast<IStreamFactory*>(querier_->First(IID_STREAM_FACTORY)) : nullptr;
    if (stream_manager) {
        stream_manager->QueryChannels([this, stream_factory, &w](const std::string& type,
                                                                 const std::string& name,
                                                                 const std::string& option,
                                                                 const std::string& status) {
            const std::string key = MakeStreamChannelKey(type, name);
            uint32_t in_use_count = 0;
            {
                std::lock_guard<std::mutex> lock(stream_channel_refs_mu_);
                auto it = stream_channel_ref_counts_.find(key);
                if (it != stream_channel_ref_counts_.end()) in_use_count = it->second;
            }
            IStreamChannel* stream_ch = stream_factory ? stream_factory->Get(type.c_str(), name.c_str()) : nullptr;
            const std::string role = ReadRoleFromOption(option);

            rapidjson::Document option_doc;
            std::string option_parse_err;
            const bool option_ok = (ParseOptionObject(option, &option_doc, &option_parse_err) == 0 && option_doc.IsObject());

            w.StartObject();
            w.Key("type");
            w.String(type.c_str());
            w.Key("name");
            w.String(name.c_str());
            w.Key("role");
            w.String(role.c_str());
            w.Key("option");
            w.String(option.c_str());
            w.Key("option_json");
            if (option_ok) {
                rapidjson::Document option_only;
                option_only.SetObject();
                auto& alloc = option_only.GetAllocator();
                for (auto it = option_doc.MemberBegin(); it != option_doc.MemberEnd(); ++it) {
                    if (std::string(it->name.GetString()) == "role") continue;
                    rapidjson::Value key_json;
                    key_json.SetString(it->name.GetString(), alloc);
                    rapidjson::Value val_json;
                    val_json.CopyFrom(it->value, alloc);
                    option_only.AddMember(key_json, val_json, alloc);
                }
                const std::string option_json = OptionObjectToJson(option_only);
                w.RawValue(option_json.c_str(), option_json.size(), rapidjson::kObjectType);
            } else {
                w.StartObject();
                w.EndObject();
            }
            w.Key("status");
            w.String(status.c_str());
            w.Key("in_use");
            w.Bool(in_use_count > 0);
            w.Key("capacity");
            w.Uint64(stream_ch ? stream_ch->Capacity() : 0);
            w.Key("size");
            w.Uint64(stream_ch ? stream_ch->Size() : 0);
            w.Key("is_finite");
            w.Bool(stream_ch ? stream_ch->IsFinite() : false);
            w.Key("is_finished");
            w.Bool(stream_ch ? stream_ch->IsFinished() : true);
            w.Key("derived_channels");
            w.StartArray();
            if (stream_ch && stream_ch->IsHubChannel() &&
                IEquals(stream_ch->HubModeHint() ? stream_ch->HubModeHint() : "", "split")) {
                for (size_t i = 0; i < stream_ch->HubPartitionCount(); ++i) {
                    auto partition = stream_ch->HubPartition(i);
                    if (!partition) continue;
                    std::string part_status = "running";
                    if (partition->IsFinished() && partition->IsEmpty()) {
                        part_status = "stopped";
                    } else if (partition->IsFinished()) {
                        part_status = "draining";
                    }
                    w.StartObject();
                    w.Key("index");
                    w.Uint(static_cast<unsigned>(i));
                    w.Key("name");
                    w.String(partition->Name());
                    w.Key("status");
                    w.String(part_status.c_str());
                    w.Key("capacity");
                    w.Uint64(partition->Capacity());
                    w.Key("size");
                    w.Uint64(partition->Size());
                    w.Key("is_finite");
                    w.Bool(partition->IsFinite());
                    w.Key("is_finished");
                    w.Bool(partition->IsFinished());
                    w.EndObject();
                }
            }
            w.EndArray();
            w.EndObject();
        });
    }

    TraverseBlockManagers([&w](IBlockStreamManager* manager) -> int {
        manager->QueryChannels([&w](const std::string& type,
                                    const std::string& name,
                                    const std::string& option,
                                    const std::string& status) {
            w.StartObject();
            w.Key("type"); w.String(type.c_str());
            w.Key("name"); w.String(name.c_str());
            w.Key("role"); w.String("source");
            w.Key("option"); w.String(option.c_str());
            w.Key("option_json");
            rapidjson::Document option_doc;
            option_doc.Parse(option.c_str());
            if (!option_doc.HasParseError() && option_doc.IsObject()) {
                w.RawValue(option.c_str(), option.size(), rapidjson::kObjectType);
            } else {
                w.StartObject(); w.EndObject();
            }
            w.Key("status"); w.String(status.c_str());
            w.Key("in_use"); w.Bool(false);
            w.Key("is_finite"); w.Bool(true);
            w.Key("is_finished"); w.Bool(status != "running");
            w.EndObject();
        });
        return 0;
    });

    w.EndArray();
    w.EndObject();
    rsp = buf.GetString();
    return error::OK;
}

int32_t SchedulerPlugin::HandleAddStreamChannel(const std::string&,
                                                const std::string& req,
                                                std::string& rsp) {
    auto* stream_manager = querier_ ? static_cast<IStreamManager*>(querier_->First(IID_STREAM_MANAGER)) : nullptr;

    rapidjson::Document doc;
    doc.Parse(req.c_str());
    if (doc.HasParseError() || !doc.IsObject()) {
        rsp = BuildErrorJson("invalid request body");
        return error::BAD_REQUEST;
    }

    std::string type;
    std::string name;
    std::string role_raw;
    std::string option_legacy;
    const rapidjson::Value* options_obj = nullptr;
    const rapidjson::Value* cfg = (doc.HasMember("config") && doc["config"].IsObject()) ? &doc["config"] : nullptr;
    auto read_string = [&](const char* key, std::string* out) {
        if (!out) return;
        out->clear();
        if (doc.HasMember(key) && doc[key].IsString()) {
            *out = doc[key].GetString();
            return;
        }
        if (cfg && cfg->HasMember(key) && (*cfg)[key].IsString()) {
            *out = (*cfg)[key].GetString();
        }
    };
    read_string("type", &type);
    read_string("name", &name);
    read_string("role", &role_raw);
    read_string("option", &option_legacy);
    if (doc.HasMember("options") && doc["options"].IsObject()) {
        options_obj = &doc["options"];
    } else if (cfg && cfg->HasMember("options") && (*cfg)["options"].IsObject()) {
        options_obj = &(*cfg)["options"];
    }
    if (type.empty() || name.empty()) {
        rsp = BuildErrorJson("invalid request, expected {\"type\":\"...\",\"name\":\"...\",\"role\":\"...\",\"options\":{...}}");
        return error::BAD_REQUEST;
    }
    std::string role = role_raw.empty() ? "both" : NormalizeStreamRole(role_raw);
    if (role.empty()) {
        rsp = BuildErrorJson("invalid role, expected source|sink|both");
        return error::BAD_REQUEST;
    }
    if (!option_legacy.empty() && role_raw.empty()) {
        role = ReadRoleFromOption(option_legacy);
    }

    if (role == "source") {
        std::string option;
        if (options_obj) {
            option = OptionObjectToJson(*options_obj);
        } else if (!option_legacy.empty()) {
            rapidjson::Document option_doc;
            std::string parse_err;
            if (ParseOptionObject(option_legacy, &option_doc, &parse_err) != 0 || !option_doc.IsObject()) {
                rsp = BuildErrorJson("invalid option: " + parse_err);
                return error::BAD_REQUEST;
            }
            option = OptionObjectToJson(option_doc);
        } else {
            option = "{}";
        }
        const auto route = RouteBlockManagers(
            [&](IBlockStreamManager* manager) {
                return manager->AddChannel(ToLowerAscii(type), name, option);
            },
            false);
        if (route.conflict) {
            rsp = BuildErrorJson("multiple block stream managers accepted request: " + type + "." + name);
            return error::CONFLICT;
        }
        if (route.accepted_count == 1) {
            if (route.accepted_rc != 0) {
                rsp = BuildErrorJson("add block stream channel failed: " + type + "." + name);
                return MapStreamManagerErrorToStatus(route.accepted_rc);
            }
            rsp = R"({"ok":true})";
            return error::OK;
        }
    }
    if (!stream_manager) {
        rsp = BuildErrorJson("stream manager unavailable");
        return error::UNAVAILABLE;
    }

    auto* builtin_registry = querier_ ? static_cast<IBuiltinRegistry*>(querier_->First(IID_BUILTIN_REGISTRY)) : nullptr;
    if (builtin_registry) {
        StreamChannelTypeDescriptor def;
        if (builtin_registry->FindStreamChannelType(type, &def) != 0) {
            rsp = BuildErrorJson("unsupported stream channel type: " + type);
            return error::BAD_REQUEST;
        }
        bool allowed = def.allowed_roles.empty();
        for (const auto& item : def.allowed_roles) {
            if (NormalizeStreamRole(item) == role) {
                allowed = true;
                break;
            }
        }
        if (!allowed) {
            rsp = BuildErrorJson("role is not allowed for stream type: " + type);
            return error::BAD_REQUEST;
        }
    }

    std::string option;
    if (options_obj) {
        option = BuildOptionWithRoleJson(options_obj, role);
    } else if (!option_legacy.empty()) {
        rapidjson::Document option_doc;
        std::string parse_err;
        if (ParseOptionObject(option_legacy, &option_doc, &parse_err) != 0 || !option_doc.IsObject()) {
            rsp = BuildErrorJson("invalid option: " + parse_err);
            return error::BAD_REQUEST;
        }
        option = BuildOptionWithRoleJson(&option_doc, role);
    } else {
        option = BuildOptionWithRoleJson(nullptr, role);
    }

    const int rc = stream_manager->AddChannel(ToLowerAscii(type), name, option);
    if (rc != 0) {
        rsp = BuildErrorJson("add stream channel failed: " + type + "." + name);
        return MapStreamManagerErrorToStatus(rc);
    }
    {
        std::lock_guard<std::mutex> lock(stream_channel_refs_mu_);
        stream_channel_versions_[MakeStreamChannelKey(type, name)] += 1;
    }
    rsp = R"({"ok":true})";
    return error::OK;
}

int32_t SchedulerPlugin::HandleModifyStreamChannel(const std::string&,
                                                   const std::string& req,
                                                   std::string& rsp) {
    auto* stream_manager = querier_ ? static_cast<IStreamManager*>(querier_->First(IID_STREAM_MANAGER)) : nullptr;

    rapidjson::Document doc;
    doc.Parse(req.c_str());
    if (doc.HasParseError() || !doc.IsObject()) {
        rsp = BuildErrorJson("invalid request body");
        return error::BAD_REQUEST;
    }

    std::string type;
    std::string name;
    std::string role_raw;
    std::string option_legacy;
    const rapidjson::Value* options_obj = nullptr;
    const rapidjson::Value* cfg = (doc.HasMember("config") && doc["config"].IsObject()) ? &doc["config"] : nullptr;
    auto read_string = [&](const char* key, std::string* out) {
        if (!out) return;
        out->clear();
        if (doc.HasMember(key) && doc[key].IsString()) {
            *out = doc[key].GetString();
            return;
        }
        if (cfg && cfg->HasMember(key) && (*cfg)[key].IsString()) {
            *out = (*cfg)[key].GetString();
        }
    };
    read_string("type", &type);
    read_string("name", &name);
    read_string("role", &role_raw);
    read_string("option", &option_legacy);
    if (doc.HasMember("options") && doc["options"].IsObject()) {
        options_obj = &doc["options"];
    } else if (cfg && cfg->HasMember("options") && (*cfg)["options"].IsObject()) {
        options_obj = &(*cfg)["options"];
    }
    if (type.empty() || name.empty()) {
        rsp = BuildErrorJson("invalid request, expected {\"type\":\"...\",\"name\":\"...\",\"role\":\"...\",\"options\":{...}}");
        return error::BAD_REQUEST;
    }
    std::string role = role_raw.empty() ? "both" : NormalizeStreamRole(role_raw);
    if (role.empty()) {
        rsp = BuildErrorJson("invalid role, expected source|sink|both");
        return error::BAD_REQUEST;
    }
    if (!option_legacy.empty() && role_raw.empty()) {
        role = ReadRoleFromOption(option_legacy);
    }

    {
        std::string option;
        if (options_obj) {
            option = OptionObjectToJson(*options_obj);
        } else if (!option_legacy.empty()) {
            rapidjson::Document option_doc;
            std::string parse_err;
            if (ParseOptionObject(option_legacy, &option_doc, &parse_err) != 0 || !option_doc.IsObject()) {
                rsp = BuildErrorJson("invalid option: " + parse_err);
                return error::BAD_REQUEST;
            }
            option = OptionObjectToJson(option_doc);
        } else {
            option = "{}";
        }
        const auto owners = FindBlockManagerOwners(type, name);
        if (owners.size() > 1) {
            rsp = BuildErrorJson("multiple block stream managers own channel: " + type + "." + name);
            return error::CONFLICT;
        }
        if (!owners.empty() && role != "source") {
            rsp = BuildErrorJson("block stream channel role must be source: " + type + "." + name);
            return error::BAD_REQUEST;
        }
        std::unique_ptr<void, std::function<void(void*)>> mutation_guard;
        if (!owners.empty()) {
            SweepFinishedTaskLeases();
            const std::string key = MakeStreamChannelKey(type, name);
            std::string mutation_reason;
            const int mutation_rc = TryBeginStreamChannelMutation(key, &mutation_reason);
            if (mutation_rc != 0) {
                rsp = BuildExecutionErrorJson(
                    "block stream channel is in use: " + type + "." + name,
                    ErrorCodeId::kStreamChannelInUse,
                    ErrorStageId::kModify);
                return error::CONFLICT;
            }
            mutation_guard = std::unique_ptr<void, std::function<void(void*)>>(
                reinterpret_cast<void*>(1),
                [this, key](void*) { EndStreamChannelMutation(key); });
        }
        if (!owners.empty()) {
            const int rc = owners.front()->ModifyChannel(ToLowerAscii(type), name, option);
            if (rc != 0) {
                rsp = BuildErrorJson("modify block stream channel failed: " + type + "." + name);
                return MapStreamManagerErrorToStatus(rc);
            }
            rsp = R"({"ok":true})";
            return error::OK;
        }
        if (role == "source") {
            const auto route = RouteBlockManagers(
                [&](IBlockStreamManager* manager) {
                    return manager->ModifyChannel(ToLowerAscii(type), name, option);
                },
                true);
            if (route.conflict) {
                rsp = BuildErrorJson("multiple block stream managers accepted request: " + type + "." + name);
                return error::CONFLICT;
            }
            if (route.accepted_count == 1) {
                if (route.accepted_rc != 0) {
                    rsp = BuildErrorJson("modify block stream channel failed: " + type + "." + name);
                    return MapStreamManagerErrorToStatus(route.accepted_rc);
                }
                rsp = R"({"ok":true})";
                return error::OK;
            }
            if (route.not_found_count != 0 && !stream_manager) {
                rsp = BuildErrorJson("block stream channel not found: " + type + "." + name);
                return error::NOT_FOUND;
            }
        }
    }
    if (!stream_manager) {
        rsp = BuildErrorJson("stream manager unavailable");
        return error::UNAVAILABLE;
    }
    auto* builtin_registry = querier_ ? static_cast<IBuiltinRegistry*>(querier_->First(IID_BUILTIN_REGISTRY)) : nullptr;
    if (builtin_registry) {
        StreamChannelTypeDescriptor def;
        if (builtin_registry->FindStreamChannelType(type, &def) != 0) {
            rsp = BuildErrorJson("unsupported stream channel type: " + type);
            return error::BAD_REQUEST;
        }
        bool allowed = def.allowed_roles.empty();
        for (const auto& item : def.allowed_roles) {
            if (NormalizeStreamRole(item) == role) {
                allowed = true;
                break;
            }
        }
        if (!allowed) {
            rsp = BuildErrorJson("role is not allowed for stream type: " + type);
            return error::BAD_REQUEST;
        }
    }
    std::string option;
    if (options_obj) {
        option = BuildOptionWithRoleJson(options_obj, role);
    } else if (!option_legacy.empty()) {
        rapidjson::Document option_doc;
        std::string parse_err;
        if (ParseOptionObject(option_legacy, &option_doc, &parse_err) != 0 || !option_doc.IsObject()) {
            rsp = BuildErrorJson("invalid option: " + parse_err);
            return error::BAD_REQUEST;
        }
        option = BuildOptionWithRoleJson(&option_doc, role);
    } else {
        option = BuildOptionWithRoleJson(nullptr, role);
    }

    SweepFinishedTaskLeases();
    const std::string key = MakeStreamChannelKey(type, name);
    std::string mutation_reason;
    const int mutation_rc = TryBeginStreamChannelMutation(key, &mutation_reason);
    if (mutation_rc != 0) {
        if (mutation_reason == "source_in_use") {
            rsp = BuildExecutionErrorJson("stream source is in use", ErrorCodeId::kStreamSourceInUse, ErrorStageId::kModify);
            return error::CONFLICT;
        }
        if (mutation_reason == "mutating") {
            rsp = BuildExecutionErrorJson("stream channel is mutating", ErrorCodeId::kStreamChannelMutating, ErrorStageId::kModify);
            return error::CONFLICT;
        }
        rsp = BuildExecutionErrorJson("stream channel is in use", ErrorCodeId::kStreamChannelInUse, ErrorStageId::kModify);
        return error::CONFLICT;
    }
    auto mutation_guard = std::unique_ptr<void, std::function<void(void*)>>(
        reinterpret_cast<void*>(1),
        [this, key](void*) { EndStreamChannelMutation(key); });

    const int rc = stream_manager->ModifyChannel(ToLowerAscii(type), name, option);
    if (rc != 0) {
        rsp = BuildErrorJson("modify stream channel failed: " + type + "." + name);
        return MapStreamManagerErrorToStatus(rc);
    }
    mutation_guard.reset();
    rsp = R"({"ok":true})";
    return error::OK;
}

int32_t SchedulerPlugin::HandleResetStreamChannel(const std::string&,
                                                  const std::string& req,
                                                  std::string& rsp) {
    auto* stream_manager = querier_ ? static_cast<IStreamManager*>(querier_->First(IID_STREAM_MANAGER)) : nullptr;
    if (!stream_manager) {
        rsp = BuildErrorJson("stream manager unavailable");
        return error::UNAVAILABLE;
    }

    rapidjson::Document doc;
    doc.Parse(req.c_str());
    if (doc.HasParseError() || !doc.IsObject() ||
        !doc.HasMember("type") || !doc["type"].IsString() ||
        !doc.HasMember("name") || !doc["name"].IsString()) {
        rsp = BuildErrorJson("invalid request, expected {\"type\":\"...\",\"name\":\"...\"}");
        return error::BAD_REQUEST;
    }

    const std::string type = doc["type"].GetString();
    const std::string name = doc["name"].GetString();
    const std::string type_l = ToLowerAscii(type);

    SweepFinishedTaskLeases();
    const std::string key = MakeStreamChannelKey(type, name);
    std::string mutation_reason;
    const int mutation_rc = TryBeginStreamChannelMutation(key, &mutation_reason);
    if (mutation_rc != 0) {
        if (mutation_reason == "source_in_use") {
            rsp = BuildExecutionErrorJson("stream source is in use", ErrorCodeId::kStreamSourceInUse, ErrorStageId::kModify);
            return error::CONFLICT;
        }
        if (mutation_reason == "mutating") {
            rsp = BuildExecutionErrorJson("stream channel is mutating", ErrorCodeId::kStreamChannelMutating, ErrorStageId::kModify);
            return error::CONFLICT;
        }
        rsp = BuildExecutionErrorJson("stream channel is in use", ErrorCodeId::kStreamChannelInUse, ErrorStageId::kModify);
        return error::CONFLICT;
    }
    auto mutation_guard = std::unique_ptr<void, std::function<void(void*)>>(
        reinterpret_cast<void*>(1),
        [this, key](void*) { EndStreamChannelMutation(key); });

    bool found = false;
    std::string current_option;
    stream_manager->QueryChannels(
        [&](const std::string& item_type,
            const std::string& item_name,
            const std::string& option,
            const std::string&) {
            if (found) return;
            if (ToLowerAscii(item_type) == type_l && item_name == name) {
                found = true;
                current_option = option;
            }
        });
    if (!found) {
        rsp = BuildErrorJson("stream channel not found: " + type + "." + name);
        return error::NOT_FOUND;
    }

    const int rc = stream_manager->ModifyChannel(type_l, name, current_option);
    if (rc != 0) {
        rsp = BuildErrorJson("reset stream channel failed: " + type + "." + name);
        return MapStreamManagerErrorToStatus(rc);
    }

    mutation_guard.reset();
    rsp = R"({"ok":true})";
    return error::OK;
}

int32_t SchedulerPlugin::HandleRemoveStreamChannel(const std::string&,
                                                   const std::string& req,
                                                   std::string& rsp) {
    auto* stream_manager = querier_ ? static_cast<IStreamManager*>(querier_->First(IID_STREAM_MANAGER)) : nullptr;

    rapidjson::Document doc;
    doc.Parse(req.c_str());
    if (doc.HasParseError() || !doc.IsObject() ||
        !doc.HasMember("type") || !doc["type"].IsString() ||
        !doc.HasMember("name") || !doc["name"].IsString()) {
        rsp = BuildErrorJson("invalid request, expected {\"type\":\"...\",\"name\":\"...\"}");
        return error::BAD_REQUEST;
    }
    const std::string type = doc["type"].GetString();
    const std::string name = doc["name"].GetString();

    {
        const auto owners = FindBlockManagerOwners(type, name);
        if (owners.size() > 1) {
            rsp = BuildErrorJson("multiple block stream managers own channel: " + type + "." + name);
            return error::CONFLICT;
        }
        std::unique_ptr<void, std::function<void(void*)>> mutation_guard;
        std::string mutation_key;
        if (!owners.empty()) {
            SweepFinishedTaskLeases();
            mutation_key = MakeStreamChannelKey(type, name);
            std::string mutation_reason;
            const int mutation_rc = TryBeginStreamChannelMutation(mutation_key, &mutation_reason);
            if (mutation_rc != 0) {
                rsp = BuildExecutionErrorJson(
                    "block stream channel is in use: " + type + "." + name,
                    ErrorCodeId::kStreamChannelInUse,
                    ErrorStageId::kRemove);
                return error::CONFLICT;
            }
            mutation_guard = std::unique_ptr<void, std::function<void(void*)>>(
                reinterpret_cast<void*>(1),
                [this, mutation_key](void*) { EndStreamChannelMutation(mutation_key); });
        }
        if (!owners.empty()) {
            const int rc = owners.front()->RemoveChannel(ToLowerAscii(type), name);
            if (rc != 0) {
                rsp = BuildErrorJson("remove block stream channel failed: " + type + "." + name);
                return MapStreamManagerErrorToStatus(rc);
            }
            rsp = R"({"ok":true})";
            return error::OK;
        }
        const auto route = RouteBlockManagers(
            [&](IBlockStreamManager* manager) {
                return manager->RemoveChannel(ToLowerAscii(type), name);
            },
            true);
        if (route.conflict) {
            rsp = BuildErrorJson("multiple block stream managers accepted request: " + type + "." + name);
            return error::CONFLICT;
        }
        if (route.accepted_count == 1) {
            if (route.accepted_rc != 0) {
                rsp = BuildErrorJson("remove block stream channel failed: " + type + "." + name);
                return MapStreamManagerErrorToStatus(route.accepted_rc);
            }
            rsp = R"({"ok":true})";
            return error::OK;
        }
        if (route.not_found_count != 0 && !stream_manager) {
            rsp = BuildErrorJson("block stream channel not found: " + type + "." + name);
            return error::NOT_FOUND;
        }
    }
    if (!stream_manager) {
        rsp = BuildErrorJson("stream manager unavailable");
        return error::UNAVAILABLE;
    }

    SweepFinishedTaskLeases();
    const std::string key = MakeStreamChannelKey(type, name);
    std::string mutation_reason;
    const int mutation_rc = TryBeginStreamChannelMutation(key, &mutation_reason);
    if (mutation_rc != 0) {
        if (mutation_reason == "source_in_use") {
            rsp = BuildExecutionErrorJson("stream source is in use", ErrorCodeId::kStreamSourceInUse, ErrorStageId::kRemove);
            return error::CONFLICT;
        }
        if (mutation_reason == "mutating") {
            rsp = BuildExecutionErrorJson("stream channel is mutating", ErrorCodeId::kStreamChannelMutating, ErrorStageId::kRemove);
            return error::CONFLICT;
        }
        rsp = BuildExecutionErrorJson("stream channel is in use", ErrorCodeId::kStreamChannelInUse, ErrorStageId::kRemove);
        return error::CONFLICT;
    }
    auto mutation_guard = std::unique_ptr<void, std::function<void(void*)>>(
        reinterpret_cast<void*>(1),
        [this, key](void*) { EndStreamChannelMutation(key); });

    const int rc = stream_manager->RemoveChannel(ToLowerAscii(type), name);
    if (rc != 0) {
        rsp = BuildErrorJson("remove stream channel failed: " + type + "." + name);
        return MapStreamManagerErrorToStatus(rc);
    }
    mutation_guard.reset();
    rsp = R"({"ok":true})";
    return error::OK;
}

// --- HandleRefreshOperators ---
int32_t SchedulerPlugin::HandleRefreshOperators(const std::string&, const std::string&, std::string& rsp) {
    if (!querier_) {
        rsp = R"({"error":"querier not initialized"})";
        return error::INTERNAL_ERROR;
    }
    auto* bridge = static_cast<IBridge*>(querier_->First(IID_BRIDGE));
    if (bridge) {
        int rc = bridge->Refresh();
        if (rc == 0) {
            rsp = R"({"status":"refreshed"})";
            return error::OK;
        } else {
            rsp = R"({"error":"refresh failed"})";
            return error::INTERNAL_ERROR;
        }
    } else {
        rsp = R"({"error":"bridge not available"})";
        return error::NOT_FOUND;
    }
}

}  // namespace scheduler
}  // namespace flowsql
