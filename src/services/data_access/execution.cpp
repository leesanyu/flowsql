// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include "access_store.h"
#include "data_access_plugin.h"

#include <framework/interfaces/ischeduler_control_service.h>
#include <services/visualization/dataframe_workflow.h>
#include <services/visualization/workflow.h>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace flowsql::access {
using namespace dataset;
namespace {
using visualization::Field;
std::string Classify(std::string native) {
    std::transform(native.begin(), native.end(), native.begin(), [](unsigned char c) { return std::toupper(c); });
    if (native.find("BOOL") != std::string::npos || native == "TINYINT(1)") return "boolean";
    for (const char* marker : {"INT", "REAL", "FLOAT", "DOUBLE", "DECIMAL", "NUMERIC"})
        if (native.find(marker) != std::string::npos) return "number";
    return "string";
}
std::vector<Field> Fields(const Value& description) {
    std::vector<Field> fields;
    if (!description.HasMember("fields") || !description["fields"].IsArray())
        throw AccessError("SCHEMA_UNAVAILABLE", "source structure is not known", 422);
    for (const auto& field : description["fields"].GetArray()) {
        const auto native = Get(field["type"], "name");
        fields.push_back({"f" + std::to_string(fields.size()), Get(field, "name"), Classify(native), native,
                          field["nullable"].GetBool()});
    }
    return fields;
}
Value Typed(const ExplorationValue& input, Alloc& a) {
    Value output(rapidjson::kObjectType);
    std::string type, value;
    if (std::holds_alternative<std::monostate>(input)) return Value();
    if (const auto* item = std::get_if<int64_t>(&input)) {
        type = "int64";
        value = std::to_string(*item);
    } else if (const auto* item = std::get_if<double>(&input)) {
        type = "float64";
        std::ostringstream text;
        text << std::setprecision(17) << *item;
        value = text.str();
    } else if (const auto* item = std::get_if<std::string>(&input)) {
        type = "string";
        value = *item;
    } else if (const auto* item = std::get_if<ExplorationDecimal>(&input)) {
        type = "decimal";
        value = item->text;
    } else {
        const auto& bytes = std::get<std::vector<uint8_t>>(input);
        type = "binary_hex";
        value = Hex(bytes.data(), bytes.size());
    }
    Set(output, "type", type, a);
    Set(output, "value", value, a);
    return output;
}
Json Workflow(const Source& source, const Value& args, const std::vector<Field>& fields) {
    Json payload(rapidjson::kObjectType);
    auto& a = payload.GetAllocator();
    Value workflow(rapidjson::kArrayType);
    if (source.grant->HasMember("filters") && !(*source.grant)["filters"].Empty()) {
        Value filters = Copy((*source.grant)["filters"], a);
        for (auto& filter : filters.GetArray()) {
            Keys(filter, {"fid", "rule"});
            const auto name = Required(filter, "fid");
            const auto field =
                std::find_if(fields.begin(), fields.end(), [&](const auto& field) { return field.name == name; });
            if (field == fields.end()) throw AccessError("POLICY_INVALID", "policy filter field is absent", 403);
            Set(filter, "fid", field->id, a);
        }
        Value step(rapidjson::kObjectType);
        Set(step, "type", "filter", a);
        step.AddMember("filters", filters, a);
        workflow.PushBack(step, a);
    }
    for (const auto& step : args["workflow"].GetArray()) workflow.PushBack(Copy(step, a), a);
    payload.AddMember("workflow", workflow, a);
    for (const char* key : {"limit", "offset"})
        if (args.HasMember(key)) payload.AddMember(Value(key, a), Copy(args[key], a), a);
    return payload;
}
// Registration is removed before the source lease leaves scope, including on failure.
class SessionBinding {
 public:
    SessionBinding(std::shared_ptr<Running> task, std::shared_ptr<IDatabaseExplorationSession> session) : task_(task) {
        std::lock_guard<std::mutex> lock(task_->mutex);
        task_->session = std::move(session);
        if (*task_->cancelled || task_->timed_out) task_->session->Cancel();
    }
    ~SessionBinding() {
        std::lock_guard<std::mutex> lock(task_->mutex);
        task_->session.reset();
    }

 private:
    std::shared_ptr<Running> task_;
};
void Interrupted(const std::shared_ptr<Running>& task) {
    if (task->timed_out) throw AccessError("TIMEOUT", "execution budget exceeded", 408);
    if (*task->cancelled) throw AccessError("CANCELLED", "execution cancelled", 409);
}
}  // namespace

Json DataAccessPlugin::Compute(const Subject& subject, const Source& source, const Value& args,
                               const std::shared_ptr<Running>& task) {
    const auto maximum = Bound(args, "max_rows", Bound(*subject.policy, "max_rows", 1000, 1, 65536), 1,
                               Bound(*subject.policy, "max_rows", 1000, 1, 65536));
    const auto bytes = Bound(args, "max_bytes", Bound(*subject.policy, "max_bytes", 8388608, 1024, 67108864), 1024,
                             Bound(*subject.policy, "max_bytes", 8388608, 1024, 67108864));
    const auto timeout = Bound(args, "timeout_ms", Bound(*subject.policy, "timeout_ms", 5000, 1, 30000), 1,
                               Bound(*subject.policy, "timeout_ms", 5000, 1, 30000));
    const auto started = Now();
    auto description = Parse(source.description);
    auto locator = Parse(source.locator);
    Json payload, basis(rapidjson::kObjectType);
    auto& ba = basis.GetAllocator();
    // Result provenance retains exact identities rather than copying the entire
    // description/editor document twice into every page and its integrity header.
    const auto provenance = [&](const Source& input) {
        auto value = SourceJson(input);
        const auto& detail = value["description"];
        if (detail.HasMember("semantics") && detail["semantics"].HasMember("basis"))
            value.AddMember("metadata_basis", Copy(detail["semantics"]["basis"], value.GetAllocator()),
                            value.GetAllocator());
        value.RemoveMember("description");
        value.RemoveMember("capabilities");
        return value;
    };
    auto actual = provenance(source);
    basis.AddMember("source", Copy(actual, ba), ba);
    std::vector<Field> fields;
    std::vector<std::vector<ExplorationValue>> rows;
    std::string scope;
    Interrupted(task);
    if (source.kind == "database_table") {
        fields = Fields(description);
        payload = Workflow(source, args, fields);
        const auto& normalized = description["locator"];
        visualization::SourceSchema schema{Get(normalized, "table"), fields, Get(normalized, "type"),
                                           Get(normalized, "schema", Get(normalized, "database"))};
        auto plan = visualization::CompileWorkflow(schema, payload, "complete", maximum);
        auto* factory =
            static_cast<IDatabaseChannelLeaseProvider*>(querier_->First(IID_DATABASE_CHANNEL_LEASE_PROVIDER));
        if (!factory) throw AccessError("SOURCE_UNAVAILABLE", "database provider is unavailable", 503);
        auto channel = factory->AcquireChannel(Get(locator, "type").c_str(), Get(locator, "name").c_str());
        if (!channel) throw AccessError("SOURCE_UNAVAILABLE", "database channel is no longer available", 503);
        auto* source_api = dynamic_cast<IDatabaseExplorationSource*>(channel.get());
        if (!source_api) throw AccessError("UNSUPPORTED_OPERATION", "source has no read-only query capability", 422);
        DatabaseSnapshotOptions options;
        options.consistent_transaction = source.provider != "clickhouse";
        options.operation_timeout_ms = timeout;
        std::shared_ptr<IDatabaseExplorationSession> session;
        if (source_api->CreateExplorationSession(options, &session))
            throw AccessError("SOURCE_UNAVAILABLE", "read-only session could not be created", 503);
        SessionBinding binding(task, session);
        Interrupted(task);
        const auto parameters = visualization::BindValues(plan.parameters);
        ExplorationResult result;
        const auto rc =
            session->Query(plan.sql.c_str(), parameters.data(), parameters.size(), maximum + 1, bytes, &result);
        Interrupted(task);
        if (rc)
            throw AccessError(session->QueryError() == "TIMEOUT"        ? "TIMEOUT"
                              : session->QueryError() == "RESULT_LIMIT" ? "RESULT_LIMIT"
                                                                        : "QUERY_FAILED",
                              "bounded database execution failed", 422);
        fields = plan.fields;
        rows = std::move(result.rows);
        scope = plan.scope;
        Set(basis, "sql", plan.sql, ba);
        Value parameters_json(rapidjson::kArrayType);
        for (const auto& parameter : plan.parameters) parameters_json.PushBack(Typed(parameter, ba), ba);
        basis.AddMember("parameters", parameters_json, ba);
        basis.AddMember("consistent_transaction", source.provider != "clickhouse", ba);
    } else {
        auto* provider = static_cast<IDataFrameSnapshotProvider*>(querier_->First(IID_DATAFRAME_SNAPSHOT_PROVIDER));
        if (!provider) throw AccessError("SOURCE_UNAVAILABLE", "snapshot provider is unavailable", 503);
        std::vector<DataFrameSource> candidates;
        std::string error;
        if (provider->List(&candidates, &error))
            throw AccessError("SOURCE_UNAVAILABLE", "snapshot discovery failed", 503);
        const auto candidate = std::find_if(candidates.begin(), candidates.end(),
                                            [&](const auto& item) { return item.label == "dataframe." + source.name; });
        if (candidate == candidates.end())
            throw AccessError("SOURCE_UNAVAILABLE", "dataframe is no longer available", 503);
        DataFrameSnapshotRequest request;
        request.source_id = candidate->source_id;
        request.max_bytes = bytes;
        request.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
        request.cancelled = task->cancelled;
        DataFrameSnapshot snapshot;
        if (provider->Acquire(request, &snapshot, &error)) {
            Interrupted(task);
            throw AccessError("SNAPSHOT_FAILED", "snapshot capture exceeded its budget or source is unavailable", 422);
        }
        fields = visualization::DescribeArrowSchema(*snapshot.batch->schema());
        payload = Workflow(source, args, fields);
        auto computed = visualization::ExecuteDataFrameWorkflow(snapshot.batch, fields, payload, "complete", maximum,
                                                                bytes, request.deadline, task->cancelled.get());
        rows = std::move(computed.rows);
        fields = std::move(computed.fields);
        scope = computed.scope;
        Set(basis, "snapshot_id", snapshot.snapshot_id, ba);
        Set(basis, "snapshot_schema_revision", snapshot.schema_revision, ba);
        Set(basis, "captured_at", snapshot.captured_at, ba);
        Value displays(rapidjson::kArrayType);
        for (const auto& display : computed.displays) {
            Value item(rapidjson::kObjectType);
            Set(item, "field_id", display.field_id, ba);
            Set(item, "mode", display.mode, ba);
            Set(item, "origin", display.origin, ba);
            displays.PushBack(item, ba);
        }
        basis.AddMember("numeric_display", displays, ba);
    }
    Interrupted(task);
    if (rows.size() > maximum)
        throw AccessError("RESULT_LIMIT", "complete result exceeds row budget; request an explicit limit", 413);
    const auto observed = Describe(locator, *source.grant);
    if (observed.id != source.id ||
        (observed.revision != source.revision && Get(description, "schema_state") != "pending"))
        throw AccessError("SOURCE_CHANGED", "source changed during execution; result was not published", 409);
    basis.AddMember("observed_source", Copy(provenance(observed), ba), ba);
    basis.AddMember("effective_workflow", Copy(payload, ba), ba);
    Set(basis, "scope", scope, ba);
    Set(basis, "operation_id", task->id, ba);
    basis.AddMember("started_at_ms", started, ba);
    basis.AddMember("finished_at_ms", Now(), ba);
    Json output(rapidjson::kObjectType);
    auto& a = output.GetAllocator();
    Value columns(rapidjson::kArrayType), values(rapidjson::kArrayType);
    for (const auto& field : fields) {
        Value column(rapidjson::kObjectType);
        Set(column, "field_id", field.id, a);
        Set(column, "name", field.name, a);
        Set(column, "native_type", field.native_type, a);
        column.AddMember("nullable", field.nullable, a);
        columns.PushBack(column, a);
    }
    for (const auto& row : rows) {
        Value value(rapidjson::kArrayType);
        for (const auto& cell : row) value.PushBack(Typed(cell, a), a);
        values.PushBack(value, a);
    }
    output.AddMember("fields", columns, a);
    output.AddMember("rows", values, a);
    output.AddMember("basis", Copy(basis, a), a);
    if (Encode(output).size() > bytes) throw AccessError("RESULT_LIMIT", "encoded result exceeds byte budget", 413);
    return output;
}
void DataAccessPlugin::Run(const Subject& subject, Source source, std::string input,
                           const std::shared_ptr<Running>& task) {
    auto args = Parse(input);
    const auto timeout = Bound(args, "timeout_ms", Bound(*subject.policy, "timeout_ms", 5000, 1, 30000), 1,
                               Bound(*subject.policy, "timeout_ms", 5000, 1, 30000));
    std::thread timer([task, timeout] {
        std::unique_lock<std::mutex> lock(task->mutex);
        if (!task->changed.wait_for(lock, std::chrono::milliseconds(timeout), [&] { return task->finished.load(); })) {
            task->timed_out = true;
            if (task->session) task->session->Cancel();
        }
    });
    const auto failure = [&](const std::string& code) {
        std::lock_guard<std::mutex> lock(task->mutex);
        auto record = store_->Find(subject.principal, task->id);
        std::string status = !task->timed_out && *task->cancelled ? "cancelled" : "failed";
        std::string error = task->timed_out ? "TIMEOUT" : *task->cancelled ? "CANCELLED" : code;
        if (task->kind == "analysis") {
            const bool submitted = record.HasMember("backend_submitted") && record["backend_submitted"].GetBool();
            const bool confirmed = !Get(record, "underlying_status").empty();
            if (code == "RECONCILE_REQUIRED" || (submitted && !confirmed)) {
                status = "reconcile_required";
                error = code;
            } else if (code == "CANCELLED_CONFIRMED") {
                status = task->timed_out && !*task->cancelled ? "failed" : "cancelled";
                error = task->timed_out && !*task->cancelled ? "TIMEOUT" : "CANCELLED";
            } else if (code == "TIMEOUT_CONFIRMED") {
                status = "failed";
                error = "TIMEOUT";
            } else if (confirmed) {
                status = "failed";
                error = code;
            }
        }
        Set(record, "status", status, record.GetAllocator());
        Set(record, "error_code", error, record.GetAllocator());
        Set(record, "error_stage", "execute", record.GetAllocator());
        record.AddMember("finished_at_ms", Now(), record.GetAllocator());
        store_->Update(subject.principal, record);
    };
    try {
        {
            std::lock_guard<std::mutex> lock(task->mutex);
            auto record = store_->Find(subject.principal, task->id);
            Set(record, "status", *task->cancelled ? "cancelling" : "running", record.GetAllocator());
            store_->Update(subject.principal, record);
        }
        auto result =
            task->kind == "analysis" ? ComputeAnalysis(subject, args, task) : Compute(subject, source, args, task);
        std::lock_guard<std::mutex> lock(task->mutex);
        if (task->kind == "query") Interrupted(task);
        store_->Complete(subject.principal, task->id, result,
                         Bound(*subject.policy, "result_ttl_ms", 3600000, 1, 86400000));
    } catch (const visualization::QueryFailure& error) {
        failure(error.code);
    } catch (const AccessError& error) {
        failure(error.code);
    }
    {
        std::lock_guard<std::mutex> lock(task->mutex);
        task->finished = true;
    }
    task->changed.notify_all();
    timer.join();
}
Json DataAccessPlugin::SubmitOperation(const Subject& subject, const std::string& tool, const Value& args) {
    const std::string kind = tool == "query" ? "query" : "analysis";
    const auto request = Required(args, "request_id"), digest = Hash(kind + "\n" + Canonical(args));
    std::lock_guard<std::mutex> lock(tasks_mutex_);
    try {
        auto previous = store_->Find(subject.principal, "", request);
        if (Get(previous, "arguments_sha256") != digest)
            throw AccessError("REQUEST_CONFLICT", "request identity has different arguments", 409);
        if (kind == "analysis" && Get(previous, "status") == "reconcile_required")
            return ReconcileAnalysis(subject, std::move(previous));
        return previous;
    } catch (const AccessError& error) {
        if (error.code != "NOT_FOUND") throw;
    }
    auto source = Resolve(subject, args["source_ref"]);
    Json plan;
    if (kind == "analysis")
        plan = PrepareAnalysis(subject, args);
    else {
        if (!Permitted(source, "query")) throw AccessError("FORBIDDEN", "query is not granted for this source", 403);
        if (source.kind != "database_table" && source.kind != "dataframe")
            throw AccessError("UNSUPPORTED_OPERATION", "this source cannot be queried as a table", 422);
        if (!source.available) throw AccessError("SOURCE_UNAVAILABLE", "source is currently unavailable", 503);
        if (source.kind == "dataframe" && (!args.HasMember("capture_snapshot") || !args["capture_snapshot"].GetBool()))
            throw AccessError("SNAPSHOT_REQUIRED", "dataframe query requires explicit capture_snapshot=true", 422);
    }
    for (const auto* key : {"max_rows", "max_bytes", "timeout_ms"}) {
        const auto high = std::string(key) == "max_rows" ? 65536 : std::string(key) == "max_bytes" ? 67108864 : 30000;
        const auto low = std::string(key) == "max_bytes" ? 1024 : 1;
        const auto fallback = std::string(key) == "max_rows" ? 1000 : std::string(key) == "max_bytes" ? 8388608 : 5000;
        const auto allowed = Bound(*subject.policy, key, fallback, low, high);
        Bound(args, key, allowed, low, allowed);
    }
    for (auto it = tasks_.begin(); it != tasks_.end();) {
        if ((*it)->finished) {
            (*it)->thread.join();
            it = tasks_.erase(it);
        } else
            ++it;
    }
    if (tasks_.size() >= 4) throw AccessError("BUSY", "execution concurrency budget is full", 429);
    bool created;
    auto record = store_->Register(subject.principal, request, digest, kind, &created);
    if (kind == "analysis") {
        auto& a = record.GetAllocator();
        Set(record, "analysis_plan", Copy(plan, a), a);
        Set(record, "runtime_task_id", "access_" + Get(record, "operation_id"), a);
        const auto allowed = Bound(*subject.policy, "max_bytes", 8388608, 1024, 67108864);
        record.AddMember("max_bytes", Bound(args, "max_bytes", allowed, 1024, allowed), a);
        store_->Update(subject.principal, record);
    }
    auto task = std::make_shared<Running>();
    task->id = Get(record, "operation_id");
    task->owner = subject.principal;
    task->kind = kind;
    tasks_.push_back(task);
    task->thread =
        std::thread([this, &subject, source, input = Encode(args), task] { Run(subject, source, input, task); });
    return record;
}
Json DataAccessPlugin::Cancel(const Subject& subject, const std::string& id) {
    std::lock_guard<std::mutex> tasks_lock(tasks_mutex_);
    auto record = store_->Find(subject.principal, id);
    for (const auto& task : tasks_) {
        if (task->id != id || task->owner != subject.principal || task->finished) continue;
        std::lock_guard<std::mutex> lock(task->mutex);
        record = store_->Find(subject.principal, id);
        if (Get(record, "status") == "completed" || Get(record, "status") == "failed" ||
            Get(record, "status") == "cancelled")
            return record;
        *task->cancelled = true;
        if (task->session) task->session->Cancel();
        Set(record, "status", "cancelling", record.GetAllocator());
        store_->Update(subject.principal, record);
        return record;
    }
    if (Get(record, "kind") == "analysis" && Get(record, "status") == "reconcile_required") {
        auto* scheduler = static_cast<ISchedulerControlService*>(querier_->First(IID_SCHEDULER_CONTROL_SERVICE));
        Set(record, "cancel_requested", Value(true), record.GetAllocator());
        store_->Update(subject.principal, record);
        if (scheduler) {
            Json request(rapidjson::kObjectType);
            Set(request, "runtime_task_id", Get(record, "runtime_task_id"), request.GetAllocator());
            Set(request, "mode", "cancel", request.GetAllocator());
            std::string response;
            scheduler->StopBatch(Encode(request), &response);
            return ReconcileAnalysis(subject, std::move(record));
        }
    }
    return record;
}
Json DataAccessPlugin::ExecuteCall(const Subject& subject, const std::string& tool, const Value& args) {
    if (tool == "list_operations") return Operations(subject);
    if (tool == "describe_operation") {
        return DescribeOperation(subject, args["operation_ref"]);
    }
    if (tool == "preflight_analysis") return PrepareAnalysis(subject, args);
    if (tool == "query" || tool == "submit_analysis") return SubmitOperation(subject, tool, args);
    if (tool == "get_operation") {
        const auto id = Get(args, "operation_id"), request = Get(args, "request_id");
        if (id.empty() == request.empty())
            throw AccessError("INVALID_REQUEST", "provide exactly one operation_id or request_id");
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        auto record = store_->Find(subject.principal, id, request);
        if (Get(record, "kind") == "analysis" && Get(record, "status") == "reconcile_required")
            return ReconcileAnalysis(subject, std::move(record));
        return record;
    }
    if (tool == "cancel_operation") return Cancel(subject, Required(args, "operation_id"));
    if (tool == "read_result")
        return store_->Read(subject.principal, Required(args, "result_ref"), Bound(args, "offset", 0, 0, INT64_MAX),
                            Bound(args, "limit", 100, 1, 1000), Get(args, "lease_id"));
    if (tool == "acquire_result_lease")
        return store_->AcquireLease(subject.principal, Required(args, "result_ref"),
                                    Bound(args, "duration_ms", 60000, 1, 300000));
    if (tool == "release_result_lease") return store_->ReleaseLease(subject.principal, Required(args, "lease_id"));
    throw AccessError("UNSUPPORTED_OPERATION", "unknown operation", 422);
}
}  // namespace flowsql::access
