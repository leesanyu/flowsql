// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <channels/kafka/kafka_store.h>
#include <channels/kafka/kafka_writer.h>
#include <common/error_code.h>
#include <framework/core/dataframe.h>
#include <framework/core/dataframe_channel.h>
#include <framework/interfaces/iblock_stream_acknowledgement.h>
#include <framework/interfaces/iblock_stream_factory.h>
#include <framework/interfaces/iblock_stream_schema.h>
#include <framework/interfaces/iblock_transform_operator.h>
#include <framework/interfaces/ichannel_registry.h>
#include <framework/interfaces/idatabase_factory.h>
#include <framework/interfaces/irouter_handle.h>
#include <services/task/task_store_sqlite.h>
#include <services/web/web_server.h>
#include <tests/common/test_environment.h>
#include <atomic>
#include <cassert>
#include <common/loader.hpp>
#include <iostream>
#include <thread>
using namespace flowsql;
using namespace flowsql::channels::kafka;
namespace {
using Clock = std::chrono::steady_clock;
std::shared_ptr<arrow::RecordBatch> Batch() {
    arrow::Int64Builder id, revision, value;
    assert(id.AppendValues({1, 2, 3}).ok());
    assert(revision.AppendValues({1, 2, 3}).ok());
    assert(value.AppendValues({5, 15, 25}).ok());
    return arrow::RecordBatch::Make(
        arrow::schema({arrow::field("id", arrow::int64()), arrow::field("revision", arrow::int64()),
                       arrow::field("value", arrow::int64())}),
        3, {id.Finish().ValueOrDie(), revision.Finish().ValueOrDie(), value.Finish().ValueOrDie()});
}
struct Session final : IKafkaClientSessionV1, IKafkaProducerSessionV1 {
    KafkaBindingSnapshotV1 snapshot;
    std::vector<KafkaProduceMessageV1> messages;
    std::atomic<bool> cancelled{false}, entered{false};
    bool fail = false, block = false;
    int drains = 0;
    const KafkaBindingSnapshotV1& Snapshot() const override { return snapshot; }
    int QueryInterface(const Guid&, void** out) override {
        *out = static_cast<IKafkaProducerSessionV1*>(this);
        return 0;
    }
    void Cancel() override { cancelled = true; }
    int Probe(uint32_t, std::string*) override { return cancelled ? ECANCELED : 0; }
    int Produce(const std::vector<KafkaProduceMessageV1>& m, uint32_t, KafkaDeliveryV1* out) override {
        entered = true;
        while (block && !cancelled) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (cancelled) {
            *out = {0, 0, m.size()};
            return ECANCELED;
        }
        if (fail) {
            messages.push_back(m.front());
            *out = {1, 0, m.size() - 1};
            return EIO;
        }
        messages.insert(messages.end(), m.begin(), m.end());
        *out = {m.size(), 0, 0};
        return 0;
    }
    int Drain(uint32_t) override {
        ++drains;
        return 0;
    }
};
struct Factory final : IResultSinkFactoryV1, IKafkaChannelCatalogV1 {
    std::shared_ptr<Session> session = std::make_shared<Session>();
    int created = 0;
    int DescribeChannel(const char* name, KafkaChannelDescriptionV1* d) override {
        d->role = std::string(name) == "source" ? "source" : "sink";
        d->sink_adapter_available = true;
        return 0;
    }
    int CreateWriter(const ResultSinkContextV1& context, std::unique_ptr<IResultSinkWriterV1>* out) override {
        ++created;
        auto c = ParseSinkConfig(Parse(
            R"({"encoding":"json","entity":"alert","schema_version":1,"record_kind":"upsert","key_fields":["id"],"revision_field":"revision"})"));
        *out = std::make_unique<ResultWriter>(c, context, session);
        return 0;
    }
    void Reset() { session = std::make_shared<Session>(); }
};
struct Source final : IBlockStreamChannel, IBlockStreamSchemaV1, IBlockStreamAcknowledgementV1, IBlockStreamFactory {
    std::shared_ptr<arrow::RecordBatch> batch = Batch();
    int polls = 0, acks = 0, releases = 0;
    std::atomic<bool> cancelled{false};
    bool continuous = false;
    std::shared_ptr<Session> session;
    const char* Category() override { return "fixture"; }
    const char* Name() override { return "input"; }
    const char* Type() override { return ChannelType::kBlockStream; }
    const char* Schema() override { return ""; }
    std::shared_ptr<arrow::Schema> ArrowSchema() const override { return batch->schema(); }
    int Open() override { return 0; }
    int Close() override { return 0; }
    int Flush() override { return 0; }
    bool IsOpened() const override { return true; }
    BlockPollEvent PollBlock(int) override {
        if (cancelled) return {BlockPollEvent::kCancelled, nullptr, ECANCELED};
        if (polls++ == 0) return {BlockPollEvent::kData, batch, 0};
        if (continuous) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            return {BlockPollEvent::kTimeout, nullptr, 0};
        }
        return {BlockPollEvent::kEof, nullptr, 0};
    }
    int AcknowledgeBlock(const std::shared_ptr<arrow::RecordBatch>&) override {
        assert(session->messages.size() == 3);
        ++acks;
        return 0;
    }
    int ReleaseBlock(const std::shared_ptr<arrow::RecordBatch>&) override {
        ++releases;
        return 0;
    }
    void Cancel() override { cancelled = true; }
    bool IsFinished() const override { return cancelled; }
    IBlockStreamChannel* Get(const char* type, const char* name) override {
        return std::string(type) == "fixture" && std::string(name) == "input" ? this : nullptr;
    }
    void List(std::function<void(const char*, const char*, IBlockStreamChannel*)> cb) override {
        cb("fixture", "input", this);
    }
    void Reset(std::shared_ptr<Session> s) {
        polls = acks = releases = 0;
        cancelled = false;
        continuous = false;
        session = std::move(s);
    }
};
struct Transform final : IBlockTransformOperatorV1 {
    bool flush_fail = false;
    class Task final : public IBlockTransformTaskV1 {
        bool fail_;

     public:
        explicit Task(bool fail) : fail_(fail) {}
        int Open(std::shared_ptr<arrow::Schema> s, std::shared_ptr<arrow::Schema>* out) override {
            *out = s;
            return 0;
        }
        int ProcessBlock(const std::shared_ptr<arrow::RecordBatch>& b, int64_t ts,
                         std::vector<BlockTransformOutputV1>* out) override {
            out->push_back({b, ts});
            return 0;
        }
        int Flush(std::vector<BlockTransformOutputV1>*) override { return fail_ ? EIO : 0; }
        void Cancel() override {}
        std::string LastError() const override { return "fixture_flush_failed"; }
    };
    std::string Category() const override { return "fixture"; }
    std::string Name() const override { return "echo"; }
    std::string Description() const override { return "single schema output fixture"; }
    int CreateTask(const BlockTransformTaskConfigV1&, IBlockTransformTaskV1** out) override {
        *out = new Task(flush_fail);
        return 0;
    }
    void ReleaseTask(IBlockTransformTaskV1* p) override { delete p; }
};
Json Call(PluginLoader* loader, const std::string& path, const std::string& request, int expected = error::OK) {
    fnRouterHandler handler;
    loader->Traverse(IID_ROUTER_HANDLE, [&](void* p) {
        static_cast<IRouterHandle*>(p)->EnumRoutes([&](const RouteItem& item) {
            if (item.uri == path && item.method == "POST") handler = item.handler;
        });
        return handler ? -1 : 0;
    });
    assert(handler);
    std::string response;
    auto rc = handler(path, request, response);
    if (rc != expected) std::cerr << path << " expected=" << expected << " got=" << rc << " " << response << '\n';
    assert(rc == expected);
    return Parse(response);
}
std::string Sql(const std::string& sql) {
    auto j = Parse("{}");
    String(j, "sql", sql, j.GetAllocator());
    return Encode(j);
}
}  // namespace
int main() {
    Factory factory;
    Source source;
    Transform transform;
    auto* loader = PluginLoader::Single();
    loader->Regist(IID_RESULT_SINK_FACTORY_V1, &factory);
    loader->Regist(IID_KAFKA_CHANNEL_CATALOG_V1, static_cast<IKafkaChannelCatalogV1*>(&factory));
    loader->Regist(IID_BLOCK_STREAM_FACTORY, static_cast<IBlockStreamFactory*>(&source));
    loader->Regist(IID_BLOCK_TRANSFORM_OPERATOR_V1, &transform);
    const auto root = flowsql::test::WorkspacePath("runtime");
    std::filesystem::create_directories(root);
    const std::string db = "type=sqlite;name=local;path=" + root + "/results.db";
    const std::string catalog = "data_dir=" + root + "/frames;operator_db_path=" + root + "/operators.db";
    const char* libs[] = {"libflowsql_database.so", "libflowsql_builtin.so", "libflowsql_catalog.so",
                          "libflowsql_scheduler.so", "libflowsql_task.so"};
    const std::string task_db = "db_path=" + root + "/tasks.db";
    const char* options[] = {db.c_str(), nullptr, catalog.c_str(), "batch_workers=1", task_db.c_str()};
    std::string persisted_task, persisted_publication;
    assert(loader->Load(get_absolute_process_path(), libs, options, 5) == 0 && loader->StartAll() == 0);
    {
        auto* registry = static_cast<IChannelRegistry*>(loader->First(IID_CHANNEL_REGISTRY));
        assert(registry);
        auto channel = std::make_shared<DataFrameChannel>("dataframe", "input");
        assert(channel->Open() == 0);
        DataFrame original;
        original.FromArrow(Batch());
        assert(channel->Write(&original) == 0);
        assert(registry->Register("input", channel) == 0);
        auto execute = [&](const std::string& sql, int expected = error::OK) {
            return Call(loader, "/scheduler/batch/execute", Sql(sql), expected);
        };
        auto result = execute("SELECT id, revision FROM dataframe.input WHERE value >= 15 INTO kafka.alerts");
        assert(result["rows_affected"].GetUint64() == 2 && factory.session->drains == 1);
        assert(factory.session->messages.size() == 2);
        auto message = Parse(factory.session->messages[0].value);
        assert(message["data"].MemberCount() == 2 && message["data"]["id"] == "2");
        DataFrame unchanged;
        assert(channel->Read(&unchanged) == 0 && unchanged.ToArrow()->Equals(*original.ToArrow()));
        factory.Reset();
        result = execute("SELECT id, revision FROM dataframe.input WHERE value > 100 INTO kafka.alerts");
        assert(result["rows"].GetUint64() == 0 && factory.session->messages.empty() && factory.session->drains == 1);
        factory.Reset();
        result = execute("SELECT value FROM dataframe.input INTO kafka.alerts", error::INTERNAL_ERROR);
        assert(result["result"]["publication"]["stage"] == "sink_open" && factory.session->messages.empty());
        factory.Reset();
        factory.session->fail = true;
        result = execute("SELECT * FROM dataframe.input INTO kafka.alerts", error::INTERNAL_ERROR);
        assert(result["rows"].GetUint64() == 1 && result["result"]["publication"]["unknown_rows"].GetUint64() == 2 &&
               factory.session->drains == 0);
        factory.Reset();
        result = execute("SELECT * FROM dataframe.input USING builtin.passthrough INTO kafka.alerts");
        assert(result["rows"].GetUint64() == 3 &&
               factory.session->messages.size() == 3);  // Actual shipped DataFrame operator.
        auto* databases = static_cast<IDatabaseFactory*>(loader->First(IID_DATABASE_FACTORY));
        auto* database = databases->Get("sqlite", "local");
        assert(database);
        assert(database->ExecuteSql("CREATE TABLE results(id INTEGER, revision INTEGER, value INTEGER)") >= 0);
        assert(database->ExecuteSql("INSERT INTO results VALUES(1,1,5),(2,2,15),(3,3,25)") >= 0);
        factory.Reset();
        result = execute("SELECT id, revision FROM sqlite.local.results WHERE value >= 15 INTO kafka.alerts");
        assert(result["rows"].GetUint64() == 2 && factory.session->messages.size() == 2);
        factory.Reset();
        result = execute("SELECT id, revision FROM sqlite.local.results WHERE value > 100 INTO kafka.alerts");
        assert(result["rows"].GetUint64() == 0 && factory.session->messages.empty() && factory.session->drains == 1);
        factory.Reset();
        source.Reset(factory.session);
        result = execute("SELECT * FROM fixture.input USING fixture.echo INTO kafka.alerts");
        assert(result["rows"].GetUint64() == 3 && source.acks == 1 && source.releases == 1 &&
               factory.session->drains == 1);
        factory.Reset();
        source.Reset(factory.session);
        factory.session->fail = true;
        execute("SELECT * FROM fixture.input USING fixture.echo INTO kafka.alerts", error::INTERNAL_ERROR);
        assert(source.acks == 0 && source.releases == 1 && factory.session->drains == 0);
        factory.Reset();
        source.Reset(factory.session);
        execute("SELECT * FROM fixture.input USING fixture.echo INTO kafka.alerts");
        assert(source.acks == 1 && factory.session->messages.size() == 3);  // Failed source batch can be replayed.
        factory.Reset();
        source.Reset(factory.session);
        transform.flush_fail = true;
        execute("SELECT * FROM fixture.input USING fixture.echo INTO kafka.alerts", error::INTERNAL_ERROR);
        assert(source.acks == 1 && factory.session->drains == 0);
        transform.flush_fail = false;
        factory.Reset();
        source.Reset(factory.session);
        const int creates = factory.created;
        execute("SELECT * FROM fixture.input USING fixture.missing INTO kafka.alerts", error::INTERNAL_ERROR);
        assert(source.polls == 0 && factory.created == creates);
        for (bool cancel : {false, true}) {
            factory.Reset();
            source.Reset(factory.session);
            source.continuous = true;
            factory.session->block = cancel;
            auto request = Parse("{}");
            String(request, "sql_text", "SELECT * FROM fixture.input USING fixture.echo INTO kafka.alerts",
                   request.GetAllocator());
            String(request, "runtime_task_id", cancel ? "cancel" : "stop", request.GetAllocator());
            auto submitted = Call(loader, "/scheduler/batch/submit", Encode(request), error::OK);
            (void)submitted;
            auto deadline = Clock::now() + std::chrono::seconds(5);
            while (!factory.session->entered && Clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            assert(factory.session->entered);
            auto stopped = Parse("{}");
            String(stopped, "runtime_task_id", cancel ? "cancel" : "stop", stopped.GetAllocator());
            String(stopped, "mode", cancel ? "cancel" : "stop", stopped.GetAllocator());
            Call(loader, "/scheduler/batch/stop", Encode(stopped));
            Json state;
            do {
                state = Call(loader, "/scheduler/batch/status", Encode(stopped));
                if (state["status"] == "stopped" || state["status"] == "cancelled" || state["status"] == "failed")
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            } while (Clock::now() < deadline);
            assert(state["status"] == (cancel ? "cancelled" : "stopped"));
            assert(factory.session->drains == (cancel ? 0 : 1));
            assert(source.acks == (cancel ? 0 : 1));
        }
        // Bridge real Web routes to the production Task routes over HTTP.
        httplib::Server task_http;
        task_http.Post("/tasks/batch/execute", [&](const httplib::Request& req, httplib::Response& rsp) {
            rsp.set_content(Encode(Call(loader, "/tasks/batch/execute", req.body)), "application/json");
        });
        task_http.Post("/tasks/detail", [&](const httplib::Request& req, httplib::Response& rsp) {
            rsp.set_content(Encode(Call(loader, "/tasks/detail", req.body)), "application/json");
        });
        const int port = task_http.bind_to_any_port("127.0.0.1");
        assert(port > 0);
        std::thread http_thread([&] { task_http.listen_after_bind(); });
        flowsql::web::WebServer web;
        web.SetSchedulerAddress("127.0.0.1", port);
        auto web_call = [&](const std::string& path, const std::string& req) {
            fnRouterHandler handler;
            web.EnumApiRoutes([&](const RouteItem& route) {
                if (route.uri == path) handler = route.handler;
            });
            assert(handler);
            std::string response;
            assert(handler(path, req, response) == error::OK);
            return Parse(response);
        };
        for (bool fail : {false, true}) {
            factory.Reset();
            factory.session->fail = fail;
            auto response = web_call("/api/tasks/batch/execute",
                                     R"({"sql_text":"SELECT * FROM dataframe.input INTO kafka.alerts","mode":"sync"})");
            assert(response["status"] == (fail ? "failed" : "completed"));
            assert(response["rows_affected"].GetInt64() == (fail ? 1 : 3));
            assert(response["result"]["publication"]["confirmed_rows"].GetUint64() == (fail ? 1 : 3));
            auto query = Parse("{}");
            String(query, "task_id", response["task_id"].GetString(), query.GetAllocator());
            auto detail = web_call("/api/tasks/result", Encode(query));
            assert(detail["rows_affected"].GetInt64() == (fail ? 1 : 3));
            assert(detail["result_target"] == "kafka.alerts");
            assert(Encode(detail["result"]) == Encode(response["result"]));
            persisted_task = response["task_id"].GetString();
            persisted_publication = Encode(response["result"]);
        }
        for (bool cancel : {false, true}) {
            factory.Reset();
            source.Reset(factory.session);
            source.continuous = true;
            factory.session->block = cancel;
            auto response = web_call(
                "/api/tasks/batch/execute",
                R"({"sql_text":"SELECT * FROM fixture.input USING fixture.echo INTO kafka.alerts","mode":"async"})");
            auto query = Parse("{}");
            String(query, "task_id", response["task_id"].GetString(), query.GetAllocator());
            auto deadline = Clock::now() + std::chrono::seconds(5);
            while (!factory.session->entered && Clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            assert(factory.session->entered);
            auto detail = Call(loader, "/tasks/detail", Encode(query));
            auto stop = Parse("{}");
            String(stop, "runtime_task_id", detail["runtime_task_id"].GetString(), stop.GetAllocator());
            String(stop, "mode", cancel ? "cancel" : "stop", stop.GetAllocator());
            Call(loader, "/scheduler/batch/stop", Encode(stop));
            do {
                detail = web_call("/api/tasks/result", Encode(query));
                if (detail["status"] == "stopped" || detail["status"] == "cancelled") break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            } while (Clock::now() < deadline);
            assert(detail["status"] == (cancel ? "cancelled" : "stopped"));
            auto& publication = detail["result"]["publication"];
            assert(publication["state"] == (cancel ? "cancelled" : "finished"));
            assert(detail["rows_affected"].GetInt64() == (cancel ? 0 : 3));
            assert(publication["input_rows"].GetUint64() == publication["confirmed_rows"].GetUint64() +
                                                                publication["not_sent_rows"].GetUint64() +
                                                                publication["unknown_rows"].GetUint64());
            auto terminal = Call(loader, "/tasks/detail", Encode(query));
            assert(Encode(terminal["result"]) == Encode(detail["result"]));
        }
        task_http.stop();
        http_thread.join();
        assert(registry->Unregister("input") == 0);
        channel.reset();
    }
    loader->StopAll();
    loader->Unload();
    {
        flowsql::task::TaskStoreSqlite store;
        flowsql::task::TaskStoreSqlite::OpenOptions options;
        options.db_path = root + "/tasks.db";
        assert(store.Open(options) == 0);
        std::string summary;
        assert(store.GetResultSummary(persisted_task, &summary) == 0 && summary == persisted_publication);
        assert(store.DeleteTask(persisted_task) == 0);
        assert(store.GetResultSummary(persisted_task, &summary) == 0 && summary.empty());
        store.Close();
    }
    std::cout << "production scheduler query/snapshot/operator/stop/cancel contracts passed\n";
}
