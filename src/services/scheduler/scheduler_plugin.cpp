#include "scheduler_plugin.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cstdio>
#include <common/error_code.h>
#include <common/log.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <regex>
#include <type_traits>
#include <unordered_set>

#include "framework/core/channel_adapter.h"
#include "framework/core/dataframe.h"
#include "framework/core/dataframe_channel.h"
#include "framework/core/pipeline.h"
#include "framework/core/sql_parser.h"
#include "framework/interfaces/ichannel.h"
#include "framework/interfaces/ichannel_registry.h"
#include "framework/interfaces/idatabase_channel.h"
#include "framework/interfaces/idatabase_factory.h"
#include "framework/interfaces/idataframe_channel.h"
#include "framework/interfaces/ioperator.h"
#include "framework/interfaces/ioperator_catalog.h"
#include "framework/interfaces/ioperator_registry.h"

namespace flowsql {
namespace scheduler {

static std::string ToLowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

static bool IEquals(const std::string& a, const std::string& b) {
    return ToLowerAscii(a) == ToLowerAscii(b);
}

static bool StartsWithIgnoreCase(const std::string& s, const std::string& prefix) {
    if (s.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(s[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

static bool IsDataFrameRef(const std::string& name) {
    return StartsWithIgnoreCase(name, "dataframe.") && name.size() > strlen("dataframe.");
}

static std::string DataFrameNamePart(const std::string& name) {
    if (!IsDataFrameRef(name)) return "";
    return name.substr(strlen("dataframe."));
}

static bool IsQualifiedDestination(const std::string& dest) {
    // 合法目标：
    // 1) dataframe.<name>
    // 2) type.name 或 type.name.table
    if (dest.empty()) return false;
    if (IsDataFrameRef(dest)) return true;
    const auto first = dest.find('.');
    if (first == std::string::npos || first == 0 || first == dest.size() - 1) return false;
    const auto second = dest.find('.', first + 1);
    if (second == first + 1) return false;
    if (second != std::string::npos && second == dest.size() - 1) return false;
    return true;
}

// --- JSON 辅助 ---
static std::string MakeErrorJson(const std::string& error) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("error");
    w.String(error.c_str());
    w.EndObject();
    return buf.GetString();
}

static std::string MakeExecutionErrorJson(const std::string& error,
                                          const std::string& error_code,
                                          const std::string& error_stage) {
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartObject();
    w.Key("error");
    w.String(error.c_str());
    w.Key("error_code");
    w.String(error_code.c_str());
    w.Key("error_stage");
    w.String(error_stage.c_str());
    w.EndObject();
    return buf.GetString();
}

static std::string ExtractStageFromExecutionError(const std::string& error) {
    // Pipeline::Run 失败消息：operator <catelog>.<name> execution failed
    static const std::regex kPattern(R"(^operator\s+([^.]+)\.([^\s]+)\s+execution failed$)",
                                     std::regex_constants::icase);
    std::smatch m;
    if (!std::regex_match(error, m, kPattern)) return "";
    if (m.size() < 3) return "";
    return m[2].str();
}

static const char* DataTypeName(DataType t) {
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

// --- IPlugin ---
int SchedulerPlugin::Option(const char* arg) {
    if (!arg) return 0;

    std::string opts(arg);
    size_t pos = 0;
    while (pos < opts.size()) {
        size_t eq = opts.find('=', pos);
        if (eq == std::string::npos) break;
        size_t end = opts.find(';', eq);
        if (end == std::string::npos) end = opts.size();

        std::string key = opts.substr(pos, eq - pos);
        std::string val = opts.substr(eq + 1, end - eq - 1);

        if (key == "host") host_ = val;
        else if (key == "port") port_ = std::stoi(val);

        pos = (end < opts.size()) ? end + 1 : opts.size();
    }
    return 0;
}

int SchedulerPlugin::Load(IQuerier* querier) {
    querier_ = querier;
    LOG_INFO("SchedulerPlugin::Load: host=%s, port=%d", host_.c_str(), port_);
    return 0;
}

int SchedulerPlugin::Unload() {
    return 0;
}

// --- 通道管理 ---
void SchedulerPlugin::RegisterChannel(const std::string& key, std::shared_ptr<IChannel> ch) {
    channels_[key] = std::move(ch);
}

// --- IPlugin::Start ---
int SchedulerPlugin::Start() {
    auto* catalog = querier_ ? static_cast<IOperatorCatalog*>(querier_->First(IID_OPERATOR_CATALOG)) : nullptr;
    if (catalog) {
        std::vector<OperatorMeta> ops;
        std::unordered_set<std::string> seen;
        auto append_unique = [&](OperatorMeta meta) {
            const std::string key = ToLowerAscii(meta.catelog) + "." + ToLowerAscii(meta.name);
            if (seen.insert(key).second) {
                ops.push_back(std::move(meta));
            }
        };
        querier_->Traverse(IID_OPERATOR, [&](void* p) -> int {
            auto* op = static_cast<IOperator*>(p);
            if (!op || op->Catelog().empty() || op->Name().empty()) return 0;
            OperatorMeta meta;
            meta.catelog = op->Catelog();
            meta.name = op->Name();
            meta.type = "cpp";
            meta.source = "scheduler";
            meta.description = op->Description();
            meta.position = op->Position() == OperatorPosition::STORAGE ? "storage" : "data";
            append_unique(std::move(meta));
            return 0;
        });
        auto* op_registry = static_cast<IOperatorRegistry*>(querier_->First(IID_OPERATOR_REGISTRY));
        if (op_registry) {
            op_registry->List([&](const char* name) {
                if (!name || name[0] == '\0') return;
                OperatorMeta meta;
                meta.catelog = "builtin";
                meta.name = name;
                meta.type = "cpp";
                meta.source = "scheduler";
                meta.position = "data";
                IOperator* op = op_registry->Create(name);
                if (op) {
                    meta.description = op->Description();
                    meta.position = op->Position() == OperatorPosition::STORAGE ? "storage" : "data";
                    delete op;
                }
                append_unique(std::move(meta));
            });
        }
        UpsertResult upsert = catalog->UpsertBatch(ops);
        if (upsert.failed_count > 0) {
            LOG_ERROR("SchedulerPlugin::Start: catalog upsert failed, success=%d failed=%d err=%s",
                      upsert.success_count, upsert.failed_count, upsert.error_message.c_str());
        } else {
            LOG_INFO("SchedulerPlugin::Start: synced %d C++ operators to Catalog", upsert.success_count);
        }
    } else {
        LOG_ERROR("SchedulerPlugin::Start: IOperatorCatalog not found");
    }
    LOG_INFO("SchedulerPlugin::Start: ready");
    return 0;
}

int SchedulerPlugin::Stop() {
    channels_.clear();
    LOG_INFO("SchedulerPlugin::Stop: done");
    return 0;
}

// --- IRouterHandle ---
void SchedulerPlugin::EnumRoutes(std::function<void(const RouteItem&)> cb) {
    // 任务执行
    cb({"POST", "/tasks/instant/execute",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleExecute(u, req, rsp);
        }});
    // 内存通道查询
    cb({"POST", "/channels/dataframe/query",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleGetChannels(u, req, rsp);
        }});
    // 内存通道数据预览
    cb({"POST", "/channels/dataframe/preview",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandlePreviewDataframe(u, req, rsp);
        }});
    // Python 算子刷新
    cb({"POST", "/operators/python/refresh",
        [this](const std::string& u, const std::string& req, std::string& rsp) {
            return HandleRefreshOperators(u, req, rsp);
        }});
}

// --- 通道查找辅助 ---
IChannel* SchedulerPlugin::FindChannel(const std::string& name) {
    auto* ch_registry = querier_ ? static_cast<IChannelRegistry*>(querier_->First(IID_CHANNEL_REGISTRY)) : nullptr;
    if (IsDataFrameRef(name) && ch_registry) {
        auto ch = ch_registry->Get(DataFrameNamePart(name).c_str());
        auto* df = dynamic_cast<IDataFrameChannel*>(ch.get());
        if (df) return df;
    }

    // 先在内部通道表中查找
    auto it = channels_.find(name);
    if (it != channels_.end()) return it->second.get();

    // 通过 IQuerier 遍历静态注册的通道
    IChannel* found = nullptr;
    if (querier_) {
        querier_->Traverse(IID_CHANNEL, [&](void* p) -> int {
            auto* c = static_cast<IChannel*>(p);
            auto dot = name.find('.');
            bool catelog_and_name_match = false;
            if (dot != std::string::npos) {
                const std::string req_catelog = name.substr(0, dot);
                const std::string req_name = name.substr(dot + 1);
                catelog_and_name_match = IEquals(c->Catelog(), req_catelog) && std::string(c->Name()) == req_name;
            }
            if (catelog_and_name_match || std::string(c->Name()) == name) {
                found = c;
                return -1;  // 找到了，停止遍历
            }
            return 0;
        });
    }

    // 模糊匹配内部通道表
    if (!found) {
        // 【第四层】尝试通过 IDatabaseFactory 获取数据库通道
        // 支持三段式（type.name.table）和两段式（type.name）
        if (querier_) {
            auto* factory = static_cast<IDatabaseFactory*>(
                querier_->First(IID_DATABASE_FACTORY));
            if (factory) {
                // 尝试解析 type.name 格式
                auto pos = name.find('.');
                if (pos != std::string::npos) {
                    std::string type = ToLowerAscii(name.substr(0, pos));
                    std::string rest = name.substr(pos + 1);
                    // 对于三段式 type.name.table，取前两段作为 type.name
                    auto pos2 = rest.find('.');
                    std::string db_name = (pos2 != std::string::npos) ? rest.substr(0, pos2) : rest;

                    auto* db_ch = factory->Get(type.c_str(), db_name.c_str());
                    if (db_ch) found = db_ch;
                }
            }
        }
    }

    return found;
}

// --- 算子查找 ---
// 先查 C++ 静态算子（IQuerier），再查 Python 算子（IBridge）
std::shared_ptr<IOperator> SchedulerPlugin::FindOperator(const std::string& catelog, const std::string& name) {
    if (!querier_) return nullptr;
    auto* op_registry = static_cast<IOperatorRegistry*>(querier_->First(IID_OPERATOR_REGISTRY));

    // 1. 先查 C++ 静态算子
    IOperator* found = nullptr;
    querier_->Traverse(IID_OPERATOR, [&](void* p) -> int {
        auto* op = static_cast<IOperator*>(p);
        if (IEquals(op->Catelog(), catelog) && op->Name() == name) {
            found = op;
            return -1;
        }
        return 0;
    });
    // C++ 算子由 PluginLoader 管理生命周期，用空 deleter 包装
    if (found) return std::shared_ptr<IOperator>(found, [](IOperator*) {});

    // 2. 再查 Python 算子（通过 IBridge）
    auto* bridge = static_cast<IBridge*>(querier_->First(IID_BRIDGE));
    if (bridge) {
        auto py_op = bridge->FindOperator(catelog, name);
        if (py_op) return py_op;
    }

    // 3. 再查内置算子注册表（CatalogPlugin）
    if (IEquals(catelog, "builtin") && op_registry) {
        IOperator* op = op_registry->Create(name.c_str());
        if (op) return std::shared_ptr<IOperator>(op, [](IOperator* p) { delete p; });
    }
    return nullptr;
}

// --- Build Database Query ---

// 从目标名称中提取表名（支持三段式 type.name.table）
static std::string ExtractTableName(const std::string& dest_name) {
    auto pos1 = dest_name.find('.');
    if (pos1 != std::string::npos) {
        auto pos2 = dest_name.find('.', pos1 + 1);
        if (pos2 != std::string::npos) {
            return dest_name.substr(pos2 + 1);  // 三段式
        }
        return dest_name.substr(pos1 + 1);  // 两段式
    }
    return dest_name;
}

// BuildQuery: 构建数据库查询语句
// 将 sql_part 中的第一个 FROM 子句替换为实际表名（支持多段式表名规范化）
// 子查询中的 FROM 保持原样，不做替换
static std::string BuildQuery(const std::string& source_name, const SqlStatement& stmt) {
    std::string sql = stmt.sql_part;
    std::string table = ExtractTableName(source_name);

    // 匹配第一个 FROM 子句（支持多段式表名如 catalog.db.table）
    // 使用不区分大小写，兼容 "from" / "From" 等写法。
    std::regex FROM_PATTERN(R"((\bFROM\s+)((?:[\w-]+\.)*[\w-]+))", std::regex_constants::icase);
    std::smatch m;
    if (std::regex_search(sql, m, FROM_PATTERN)) {
        // 只替换第一个匹配（主查询的 FROM），子查询不受影响
        sql = sql.substr(0, m.position()) +
              m[1].str() + table +
              sql.substr(m.position() + m.length());
    }

    return sql;
}

// --- 辅助：对 DataFrame 通道应用 WHERE 过滤 ---
static std::shared_ptr<DataFrameChannel> ApplyDataFrameFilter(
    IDataFrameChannel* src, const std::string& where_clause, uint64_t seq) {
    DataFrame data;
    if (src->Read(&data) != 0 || data.RowCount() == 0) return nullptr;

    if (data.Filter(where_clause.c_str()) != 0) return nullptr;

    auto filtered = std::make_shared<DataFrameChannel>("_filter", std::to_string(seq));
    filtered->Open();
    filtered->Write(&data);
    return filtered;
}

// --- 无算子：纯数据搬运 ---
int SchedulerPlugin::ExecuteTransfer(IChannel* source, IChannel* sink,
                                      const std::string& source_type,
                                      const std::string& sink_type,
                                      const SqlStatement& stmt, int64_t* rows_affected,
                                      std::string* error) {
    if (source_type == ChannelType::kDataFrame && sink_type == ChannelType::kDataFrame) {
        auto* src = dynamic_cast<IDataFrameChannel*>(source);
        auto* dst = dynamic_cast<IDataFrameChannel*>(sink);
        if (!src || !dst) return -1;

        // DataFrame + WHERE → 先过滤再复制
        if (!stmt.where_clause.empty()) {
            auto filtered = ApplyDataFrameFilter(src, stmt.where_clause, ++tmp_channel_seq_);
            if (!filtered) return -1;
            return ChannelAdapter::CopyDataFrame(filtered.get(), dst);
        }
        return ChannelAdapter::CopyDataFrame(src, dst);
    }

    if (source_type == ChannelType::kDataFrame && sink_type == ChannelType::kDatabase) {
        auto* src = dynamic_cast<IDataFrameChannel*>(source);
        auto* dst = dynamic_cast<IDatabaseChannel*>(sink);
        if (!src || !dst) return -1;
        std::string table = ExtractTableName(stmt.dest);

        // DataFrame + WHERE → 先过滤再写入
        if (!stmt.where_clause.empty()) {
            auto filtered = ApplyDataFrameFilter(src, stmt.where_clause, ++tmp_channel_seq_);
            if (!filtered) return -1;
            int64_t rows = ChannelAdapter::WriteFromDataFrame(filtered.get(), dst, table.c_str(), error);
            if (rows_affected) *rows_affected = rows;
            return (rows < 0) ? -1 : 0;
        }
        int64_t rows = ChannelAdapter::WriteFromDataFrame(src, dst, table.c_str(), error);
        if (rows_affected) *rows_affected = rows;
        return (rows < 0) ? -1 : 0;
    }

    if (source_type == ChannelType::kDatabase && sink_type == ChannelType::kDataFrame) {
        auto* src = dynamic_cast<IDatabaseChannel*>(source);
        auto* dst = dynamic_cast<IDataFrameChannel*>(sink);
        if (!src || !dst) return -1;
        std::string query = BuildQuery(stmt.source, stmt);
        return ChannelAdapter::ReadToDataFrame(src, query.c_str(), dst, error);
    }

    if (source_type == ChannelType::kDatabase && sink_type == ChannelType::kDatabase) {
        auto* src = dynamic_cast<IDatabaseChannel*>(source);
        auto* dst = dynamic_cast<IDatabaseChannel*>(sink);
        if (!src || !dst) return -1;

        auto tmp = std::make_shared<DataFrameChannel>("_adapter", std::to_string(++tmp_channel_seq_));
        tmp->Open();

        std::string query = BuildQuery(stmt.source, stmt);
        int rc = ChannelAdapter::ReadToDataFrame(src, query.c_str(), tmp.get(), error);
        if (rc != 0) return rc;

        std::string table = ExtractTableName(stmt.dest);
        int64_t rows = ChannelAdapter::WriteFromDataFrame(tmp.get(), dst, table.c_str(), error);
        if (rows_affected) *rows_affected = rows;
        return (rows < 0) ? -1 : 0;
    }

    if (error) *error = "unsupported transfer: " + source_type + " → " + sink_type;
    return -1;
}

// --- 有算子：自动适配通道类型 ---
int SchedulerPlugin::ExecuteWithOperator(IChannel* source, IChannel* sink,
                                          IOperator* op,
                                          const std::string& source_type,
                                          const std::string& sink_type,
                                          const SqlStatement& stmt, int64_t* rows_affected,
                                          std::string* error) {
    std::vector<IOperator*> ops;
    ops.push_back(op);
    return ExecuteWithOperatorChain(source, sink, ops, source_type, sink_type, stmt, rows_affected, error);
}

int SchedulerPlugin::ExecuteWithOperatorChain(IChannel* source, IChannel* sink,
                                              const std::vector<IOperator*>& ops,
                                              const std::string& source_type,
                                              const std::string& sink_type,
                                              const SqlStatement& stmt, int64_t* rows_affected,
                                              std::string* error) {
    if (ops.empty()) return -1;
    IChannel* actual_source = source;
    IChannel* actual_sink = sink;
    std::shared_ptr<DataFrameChannel> tmp_in;
    std::vector<std::shared_ptr<DataFrameChannel>> stage_buffers;

    if (source_type == ChannelType::kDatabase) {
        auto* db_src = dynamic_cast<IDatabaseChannel*>(source);
        if (!db_src) return -1;

        tmp_in = std::make_shared<DataFrameChannel>("_adapter", std::to_string(++tmp_channel_seq_));
        tmp_in->Open();

        std::string query = BuildQuery(stmt.source, stmt);
        int rc = ChannelAdapter::ReadToDataFrame(db_src, query.c_str(), tmp_in.get(), error);
        if (rc != 0) return rc;

        actual_source = tmp_in.get();
    } else if (source_type == ChannelType::kDataFrame && !stmt.where_clause.empty()) {
        // DataFrame + WHERE → 先过滤
        auto* df_src = dynamic_cast<IDataFrameChannel*>(source);
        if (!df_src) return -1;

        tmp_in = ApplyDataFrameFilter(df_src, stmt.where_clause, ++tmp_channel_seq_);
        if (!tmp_in) return -1;
        actual_source = tmp_in.get();
    }

    for (size_t i = 0; i < ops.size(); ++i) {
        std::shared_ptr<DataFrameChannel> stage_out;
        if (i + 1 == ops.size()) {
            if (sink_type == ChannelType::kDatabase) {
                stage_out = std::make_shared<DataFrameChannel>("_adapter", std::to_string(++tmp_channel_seq_));
                stage_out->Open();
                actual_sink = stage_out.get();
            } else {
                actual_sink = sink;
            }
        } else {
            stage_out = std::make_shared<DataFrameChannel>("_pipe", std::to_string(++tmp_channel_seq_));
            stage_out->Open();
            actual_sink = stage_out.get();
        }

        auto pipeline = PipelineBuilder()
                            .SetSource(actual_source)
                            .SetOperator(ops[i])
                            .SetSink(actual_sink)
                            .Build();
        pipeline->Run();

        if (pipeline->State() == PipelineState::FAILED) {
            if (error) *error = pipeline->ErrorMessage();
            return -1;
        }

        if (stage_out) {
            stage_buffers.push_back(stage_out);
            actual_source = stage_out.get();
        }
    }

    if (sink_type == ChannelType::kDatabase) {
        auto* db_sink = dynamic_cast<IDatabaseChannel*>(sink);
        if (!db_sink) return -1;
        if (stage_buffers.empty()) return -1;

        std::string table = ExtractTableName(stmt.dest);
        int64_t written_rows = ChannelAdapter::WriteFromDataFrame(stage_buffers.back().get(), db_sink, table.c_str(), error);
        if (written_rows < 0) return -1;

        if (rows_affected) *rows_affected = written_rows;
    }

    return 0;
}

// --- HandleExecute ---
int32_t SchedulerPlugin::HandleExecute(const std::string&, const std::string& req_body, std::string& rsp) {
    auto* ch_registry = querier_ ? static_cast<IChannelRegistry*>(querier_->First(IID_CHANNEL_REGISTRY)) : nullptr;
    rapidjson::Document doc;
    doc.Parse(req_body.c_str());
    if (doc.HasParseError() || !doc.HasMember("sql") || !doc["sql"].IsString()) {
        rsp = MakeErrorJson("invalid request, expected {\"sql\":\"...\"}");
        return error::BAD_REQUEST;
    }
    std::string sql_text = doc["sql"].GetString();

    static constexpr size_t kMaxSqlLength = 64 * 1024;
    if (sql_text.size() > kMaxSqlLength) {
        rsp = MakeErrorJson("SQL too long (max 64KB)");
        return error::BAD_REQUEST;
    }

    SqlParser parser;
    auto stmt = parser.Parse(sql_text);
    if (!stmt.error.empty()) {
        rsp = MakeErrorJson(stmt.error);
        return error::BAD_REQUEST;
    }

    IChannel* source = FindChannel(stmt.source);
    if (!source) {
        rsp = MakeErrorJson("source channel not found: " + stmt.source);
        return IsDataFrameRef(stmt.source) ? error::NOT_FOUND : error::BAD_REQUEST;
    }

    std::vector<std::shared_ptr<IOperator>> op_holders;
    std::vector<IOperator*> op_chain;
    std::vector<OperatorRef> parsed_ops = stmt.operators;
    if (parsed_ops.empty() && !stmt.op_catelog.empty() && !stmt.op_name.empty()) {
        parsed_ops.push_back({stmt.op_catelog, stmt.op_name});
    }
    if (!parsed_ops.empty()) {
        auto* catalog = querier_ ? static_cast<IOperatorCatalog*>(querier_->First(IID_OPERATOR_CATALOG)) : nullptr;
        if (!catalog) {
            rsp = MakeErrorJson("operator catalog unavailable");
            return error::UNAVAILABLE;
        }
        for (const auto& op_ref : parsed_ops) {
            OperatorStatus status = catalog->QueryStatus(op_ref.catelog, op_ref.name);
            if (status == OperatorStatus::kNotFound) {
                rsp = MakeErrorJson("operator not found: " + op_ref.catelog + "." + op_ref.name);
                return error::NOT_FOUND;
            }
            if (status == OperatorStatus::kDeactivated) {
                rsp = MakeErrorJson("operator is deactivated: " + op_ref.catelog + "." + op_ref.name);
                return error::CONFLICT;
            }
            auto holder = FindOperator(op_ref.catelog, op_ref.name);
            if (!holder) {
                rsp = MakeErrorJson("operator not found: " + op_ref.catelog + "." + op_ref.name);
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
        IChannel* sink = nullptr;

        if (!stmt.dest.empty()) {
            if (!IsQualifiedDestination(stmt.dest)) {
                rsp = MakeErrorJson("invalid INTO destination: " + stmt.dest +
                                    ", expected dataframe.<name> or <type>.<name>[.<table>]");
                return error::BAD_REQUEST;
            }
            if (IsDataFrameRef(stmt.dest)) {
                if (!ch_registry) {
                    rsp = MakeErrorJson("channel registry unavailable");
                    return error::INTERNAL_ERROR;
                }
                std::string df_name = DataFrameNamePart(stmt.dest);
                auto ch = std::make_shared<DataFrameChannel>("dataframe", df_name);
                ch->Open();
                named_df_sink = ch;
                sink = ch.get();
            } else {
                sink = FindChannel(stmt.dest);
                if (!sink) {
                    rsp = MakeErrorJson("destination channel not found: " + stmt.dest);
                    return error::NOT_FOUND;
                }
            }
        } else {
            temp_sink = std::make_shared<DataFrameChannel>("_temp", "sink");
            temp_sink->Open();
            sink = temp_sink.get();
        }

        std::string source_type(source->Type());
        std::string sink_type(sink->Type());

        int rc = 0;
        int64_t affected_rows = 0;
        std::string exec_error;

        if (op_chain.empty()) {
            rc = ExecuteTransfer(source, sink, source_type, sink_type, stmt, &affected_rows, &exec_error);
        } else {
            rc = ExecuteWithOperatorChain(source, sink, op_chain, source_type, sink_type, stmt, &affected_rows, &exec_error);
        }

        if (rc != 0) {
            std::string err = exec_error;
            if (err.empty() && !op_chain.empty()) err = op_chain.back()->LastError();
            if (err.empty()) err = "execution failed";
            std::string stage = ExtractStageFromExecutionError(err);
            if (stage.empty()) stage = "execute";
            rsp = MakeExecutionErrorJson(err, "OP_EXEC_FAIL", stage);
            return error::INTERNAL_ERROR;
        }

        // INTO dataframe.<name>：覆盖语义（已存在则先注销，再注册新结果）
        if (!stmt.dest.empty() && IsDataFrameRef(stmt.dest) && named_df_sink) {
            std::string df_name = DataFrameNamePart(stmt.dest);
            if (ch_registry->Get(df_name.c_str())) {
                (void)ch_registry->Unregister(df_name.c_str());
            }
            if (ch_registry->Register(df_name.c_str(), std::static_pointer_cast<IChannel>(named_df_sink)) != 0) {
                rsp = MakeErrorJson("failed to register dataframe channel: " + df_name);
                return error::INTERNAL_ERROR;
            }
            auto registered = ch_registry->Get(df_name.c_str());
            if (!registered) {
                rsp = MakeErrorJson("failed to fetch registered dataframe channel: " + df_name);
                return error::INTERNAL_ERROR;
            }
            auto* registered_df = dynamic_cast<IDataFrameChannel*>(registered.get());
            if (!registered_df) {
                rsp = MakeErrorJson("registered channel is not dataframe: " + df_name);
                return error::INTERNAL_ERROR;
            }
            sink = registered_df;
            sink_type = sink->Type();
        }

        auto* df_sink = dynamic_cast<IDataFrameChannel*>(sink);
        DataFrame result;
        std::string result_json = "[]";
        int64_t row_count = 0;
        if (df_sink && df_sink->Read(&result) == 0 && result.RowCount() > 0) {
            result_json = result.ToJson();
            row_count = result.RowCount();
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
        w.Key("data"); w.RawValue(result_json.c_str(), result_json.size(), rapidjson::kArrayType);
        w.EndObject();
        rsp = buf.GetString();
        return error::OK;

    } catch (const std::exception& e) {
        std::string err = std::string("internal error: ") + e.what();
        LOG_ERROR("SchedulerPlugin::HandleExecute: exception: %s", err.c_str());
        rsp = MakeErrorJson(err);
        return error::INTERNAL_ERROR;
    } catch (...) {
        LOG_ERROR("SchedulerPlugin::HandleExecute: unknown exception");
        rsp = MakeErrorJson("internal error: unknown exception");
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
    for (auto& [key, ch_ptr] : channels_) {
        auto* ch = ch_ptr.get();
        w.StartObject();
        w.Key("catelog"); w.String(ch->Catelog());
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
            w.Key("catelog"); w.String(ch->Catelog());
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
            w.Key("catelog"); w.String(ch->Catelog());
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
                w.Key("catelog"); w.String(type);
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
    }

    w.EndArray();
    rsp = buf.GetString();
    return error::OK;
}

// --- HandlePreviewDataframe ---
// POST /channels/dataframe/preview — Body: {"catelog":"...","name":"..."} 或 {"name":"..."}
int32_t SchedulerPlugin::HandlePreviewDataframe(const std::string&, const std::string& req, std::string& rsp) {
    auto* ch_registry = querier_ ? static_cast<IChannelRegistry*>(querier_->First(IID_CHANNEL_REGISTRY)) : nullptr;
    rapidjson::Document doc;
    doc.Parse(req.c_str());
    if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("name") || !doc["name"].IsString()) {
        rsp = R"({"error":"missing 'name'"})";
        return error::BAD_REQUEST;
    }
    std::string catelog = "dataframe";
    if (doc.HasMember("catelog") && doc["catelog"].IsString()) {
        catelog = doc["catelog"].GetString();
    }
    std::string name = doc["name"].GetString();
    int page = 1;
    int page_size = 20;
    if (doc.HasMember("page") && doc["page"].IsInt()) page = doc["page"].GetInt();
    if (doc.HasMember("page_size") && doc["page_size"].IsInt()) page_size = doc["page_size"].GetInt();
    if (page < 1) page = 1;
    if (page_size < 1) page_size = 20;
    if (page_size > 100) page_size = 100;
    std::string key     = catelog + "." + name;

    // 先在内部通道表查找
    IChannel* raw_ch = nullptr;
    auto it = channels_.find(key);
    if (it != channels_.end()) {
        raw_ch = it->second.get();
    }

    // 再去 IQuerier 静态注册通道查找
    if (!raw_ch && querier_) {
        querier_->Traverse(IID_CHANNEL, [&](void* p) -> int {
            auto* ch = static_cast<IChannel*>(p);
            if (IEquals(ch->Catelog(), catelog) && std::string(ch->Name()) == name) {
                raw_ch = ch;
                return 1;  // 找到，停止遍历
            }
            return 0;
        });
    }

    if (!raw_ch) {
        if (IEquals(catelog, "dataframe") && ch_registry) {
            auto named = ch_registry->Get(name.c_str());
            auto* named_df = dynamic_cast<IDataFrameChannel*>(named.get());
            if (named_df) raw_ch = named_df;
        }
    }

    if (!raw_ch) {
        rsp = "{\"error\":\"channel not found: " + key + "\"}";
        return error::NOT_FOUND;
    }

    auto* df_ch = dynamic_cast<IDataFrameChannel*>(raw_ch);
    if (!df_ch) {
        rsp = R"({"error":"not a dataframe channel"})";
        return error::BAD_REQUEST;
    }

    DataFrame data;
    if (df_ch->Read(&data) != 0 || data.RowCount() == 0) {
        rsp = R"({"columns":[],"types":[],"data":[],"rows":0})";
        return error::OK;
    }
    const auto schema = data.GetSchema();
    const int rows = data.RowCount();
    const int start = (page - 1) * page_size;
    const int end = std::min(rows, start + page_size);

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
    for (int r = start; r < end; ++r) {
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
    w.Key("page");
    w.Int(page);
    w.Key("page_size");
    w.Int(page_size);
    w.EndObject();
    rsp = buf.GetString();
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

// ==================== 数据库通道动态管理端点（Epic 6）====================
// 已移交 DatabasePlugin 处理，此处删除

}  // namespace scheduler
}  // namespace flowsql
