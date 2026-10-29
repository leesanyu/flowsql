// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <framework/core/dataframe.h>
#include <framework/core/dataframe_channel.h>
#include <framework/interfaces/ibinaddon_host.h>
#include <framework/interfaces/iblock_stream_factory.h>
#include <framework/interfaces/iblock_stream_reader.h>
#include <framework/interfaces/iblock_stream_schema.h>
#include <framework/interfaces/ichannel_registry.h>
#include <framework/interfaces/idatabase_factory.h>
#include <framework/interfaces/ioperator_catalog.h>
#include <framework/interfaces/ischeduler_control_service.h>
#include <openssl/evp.h>
#include <unistd.h>
#include <cassert>
#include <common/channel_dataset.hpp>
#include <common/loader.hpp>
#include <common/operator_block_contract.hpp>
#include <common/operator_contract_control.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace flowsql;
using namespace flowsql::dataset;
class Blocks final : public IBlockStreamFactory, public IBlockStreamReaderFactoryV1 {
 public:
    bool empty = false;
    class Reader final : public IBlockStreamChannel, public IBlockStreamSchemaV1 {
     public:
        bool empty = false, done = false;
        const char* Category() override { return "fixture"; }
        const char* Name() override { return "source"; }
        const char* Type() override { return ChannelType::kBlockStream; }
        const char* Schema() override { return ""; }
        int Open() override { return 0; }
        int Close() override { return 0; }
        bool IsOpened() const override { return true; }
        int Flush() override { return 0; }
        std::shared_ptr<arrow::Schema> ArrowSchema() const override {
            return arrow::schema({arrow::field("x", arrow::int64())});
        }
        BlockPollEvent PollBlock(int) override {
            if (done) return {BlockPollEvent::kEof};
            done = true;
            arrow::Int64Builder b;
            if (!empty) assert(b.AppendValues({2, 4}).ok());
            auto array = b.Finish().ValueOrDie();
            return {BlockPollEvent::kData, arrow::RecordBatch::Make(ArrowSchema(), empty ? 0 : 2, {array}), 0};
        }
        int ReleaseBlock(const std::shared_ptr<arrow::RecordBatch>&) override { return 0; }
        void Cancel() override { done = true; }
        bool IsFinished() const override { return done; }
    } source;
    IBlockStreamChannel* Get(const char* category, const char* name) override {
        return std::string(category) == "fixture" && std::string(name) == "source" ? &source : nullptr;
    }
    void List(std::function<void(const char*, const char*, IBlockStreamChannel*)> f) override {
        f("fixture", "source", &source);
    }
    int CreateReader(const BlockStreamReaderConfigV1& c, IBlockStreamChannel** out) override {
        *out = nullptr;
        if (!Get(c.source_category, c.source_name)) return ENOTSUP;
        auto* reader = new Reader;
        reader->empty = empty;
        *out = reader;
        return 0;
    }
    void ReleaseReader(IBlockStreamChannel* reader) override { delete reader; }
};
int main() {
    char temporary[] = "/tmp/flowsql_contract_execution_XXXXXX";
    assert(mkdtemp(temporary));
    const std::string root = temporary;
    std::filesystem::create_directories(root + "/frames");
    std::ofstream(root + "/frames/lazy.csv") << "bucket,series,value,total,group\n1,a,2.5,4,1\n";
    auto* loader = PluginLoader::Single();
    Blocks blocks;
    loader->Regist(IID_BLOCK_STREAM_FACTORY, static_cast<IBlockStreamFactory*>(&blocks));
    loader->Regist(IID_BLOCK_STREAM_READER_FACTORY_V1, static_cast<IBlockStreamReaderFactoryV1*>(&blocks));
    struct Owner {
        PluginLoader* loader;
        ~Owner() { loader->Unload(); }
    } owner{loader};
    const char* libraries[]{CONFIG_PLUGIN_PATH,    BUILTIN_PLUGIN_PATH,  CATALOG_PLUGIN_PATH,  BINADDON_PLUGIN_PATH,
                            SCHEDULER_PLUGIN_PATH, DATABASE_PLUGIN_PATH, BASELINE_PLUGIN_PATH, BASELINER_PLUGIN_PATH};
    const auto config = "db_path=" + root + "/config.db";
    const auto catalog = "data_dir=" + root + "/frames;operator_db_path=" + root + "/catalog.db";
    const auto host_config = "operator_db_path=" + root + "/catalog.db;upload_dir=" + root + "/uploads";
    const auto db_options = "config_file=" + root + "/database.yml";
    const char* options[]{config.c_str(),     nullptr, catalog.c_str(), host_config.c_str(), nullptr,
                          db_options.c_str(), nullptr, nullptr};
    assert(loader->Load(".", libraries, options, 8) == 0 && loader->StartAll() == 0);
    auto* channels = static_cast<IChannelRegistry*>(loader->First(IID_CHANNEL_REGISTRY));
    auto* host = static_cast<IBinAddonHost*>(loader->First(IID_BINADDON_HOST));
    auto* scheduler = static_cast<ISchedulerControlService*>(loader->First(IID_SCHEDULER_CONTROL_SERVICE));
    assert(channels && host && scheduler);
    std::string response;
    const auto fixture = root + "/fixture.so";
    std::filesystem::copy_file(FIXTURE_BLOCK_PATH, fixture);
    assert(host->UploadCppPlugin("fixture.so", fixture, response) == 0);
    Json uploaded;
    uploaded.Parse(response.c_str());
    assert(host->ActivateCppPlugin(Get(uploaded, "plugin_id"), response) == 0);
    auto* directory = static_cast<IOperatorCatalog*>(loader->First(IID_OPERATOR_CATALOG));
    assert(directory && directory->SetActive("builtin", "passthrough", true) == 0 &&
           directory->SetActive("builtin", "concat", true) == 0 &&
           directory->SetActive("contractdemo", "block", true) == 0);
    auto sql = [&](const std::string& statement, bool success) {
        Json request(rapidjson::kObjectType);
        Set(request, "sql", statement, request.GetAllocator());
        const auto rc = scheduler->ExecuteBatch(Encode(request), &response);
        if ((rc == 0) != success) std::cerr << response << '\n';
        assert((rc == 0) == success);
        Json result;
        result.Parse(response.c_str());
        assert(result.IsObject());
        return result;
    };
    auto result = sql("SELECT * FROM fixture.source USING contractdemo.block WITH column='x'", true);
    assert(result["rows"].GetInt64() == 2 && result["contract_checks"][0]["state"] == "compatible");
    result = sql(
        "SELECT * FROM fixture.source USING contractdemo.block WITH column='x' THEN contractdemo.block WITH column='x'",
        true);
    assert(result["contract_checks"].Size() == 2);
    const auto marker = root + "/must-not-open";
    auto invalid = sql("SELECT * FROM fixture.source USING contractdemo.block WITH column='missing',marker='" + marker +
                           "' INTO dataframe.bad",
                       false);
    assert(invalid["error"]["code"] == "CONTRACT_INCOMPATIBLE" && !std::filesystem::exists(marker) &&
           !channels->Get("bad"));
    invalid =
        sql("SELECT * FROM fixture.source USING contractdemo.block WITH column='x' THEN contractdemo.block WITH "
            "column='missing',marker='" +
                marker + "'",
            false);
    assert(invalid["error"]["code"] == "CONTRACT_INCOMPATIBLE" && invalid["error"]["details"]["checks"].Size() == 2 &&
           !std::filesystem::exists(marker));
    for (const char* phase : {"process", "flush"}) {
        auto drifted =
            sql(std::string("SELECT * FROM fixture.source USING contractdemo.block WITH column='x',drift='") + phase +
                    "' INTO dataframe.drift_" + phase,
                false);
        assert(drifted["error"]["code"] == "OUTPUT_SCHEMA_MISMATCH");
        assert(!channels->Get((std::string("drift_") + phase).c_str()));
    }
    // Time callbacks are checked at the same boundary, including THEN wrappers.
    class TimeDrift final : public IBlockTransformTaskV2 {
     public:
        int Open(std::shared_ptr<arrow::Schema> in, std::shared_ptr<arrow::Schema>* out) override {
            *out = in;
            return 0;
        }
        int ProcessBlock(const std::shared_ptr<arrow::RecordBatch>&, int64_t,
                         std::vector<BlockTransformOutputV1>*) override {
            return 0;
        }
        int Flush(std::vector<BlockTransformOutputV1>*) override { return 0; }
        void Cancel() override {}
        std::string LastError() const override { return {}; }
        int GetTimeDriveState(BlockTransformTimeDriveStateV1*) override { return 0; }
        int OnTime(const BlockTransformTimeEventV1&, std::vector<BlockTransformOutputV1>* out) override {
            arrow::StringBuilder builder;
            out->push_back({arrow::RecordBatch::Make(arrow::schema({arrow::field("x", arrow::utf8())}), 0,
                                                     {builder.Finish().ValueOrDie()}),
                            0});
            return 0;
        }
    } time_drift;
    auto state = std::make_shared<contracts::BlockInvocation>();
    state->querier = loader;
    DataSemanticsErrorV1 time_error;
    auto* contract_catalog = static_cast<IOperatorContractCatalogV1*>(loader->First(IID_OPERATOR_CONTRACT_CATALOG_V1));
    assert(contract_catalog &&
           contract_catalog->Lookup("contractdemo", "block", &state->invocation.component, &time_error) == 0);
    Set(state->parameters, "column", "x", state->parameters.GetAllocator());
    contracts::CheckedBlockTask guarded(&time_drift, nullptr, state);
    std::shared_ptr<arrow::Schema> time_schema;
    assert(guarded.Open(arrow::schema({arrow::field("x", arrow::int64())}), &time_schema) == 0);
    std::vector<BlockTransformOutputV1> time_outputs;
    BlockTransformTimeEventV1 event{};
    assert(guarded.OnTime(event, &time_outputs) != 0 && state->error.code == "OUTPUT_SCHEMA_MISMATCH" &&
           time_outputs.empty());
    blocks.empty = true;
    result = sql("SELECT * FROM fixture.source USING contractdemo.block WITH column='x'", true);
    assert(result["rows"].GetInt64() == 0 && result["contract_checks"][0]["state"] == "compatible");
    // Real Scheduler -> production Baseliner -> actual DataFrame/SQLite readers.
    assert(directory->SetActive("explore", "baseliner", true) == 0);
    auto input = std::make_shared<DataFrameChannel>("dataframe", "input");
    arrow::Int32Builder bucket;
    arrow::StringBuilder series;
    arrow::FloatBuilder value;
    arrow::UInt32Builder total, group;
    assert(bucket.Append(1).ok() && series.Append("a").ok() && value.Append(2.5f).ok() && total.Append(4).ok() &&
           group.Append(1).ok());
    DataFrame frame;
    frame.FromArrow(arrow::RecordBatch::Make(
        arrow::schema({arrow::field("bucket", arrow::int32()), arrow::field("series", arrow::utf8()),
                       arrow::field("value", arrow::float32()), arrow::field("total", arrow::uint32()),
                       arrow::field("group", arrow::uint32())}),
        1,
        {bucket.Finish().ValueOrDie(), series.Finish().ValueOrDie(), value.Finish().ValueOrDie(),
         total.Finish().ValueOrDie(), group.Finish().ValueOrDie()}));
    assert(input->Open() == 0 && input->Write(&frame) == 0 && channels->Register("input", input) == 0);
    auto* databases = static_cast<IDatabaseFactory*>(loader->First(IID_DATABASE_FACTORY));
    assert(databases && databases->AddChannel(("type=sqlite;name=example;path=" + root + "/input.db").c_str()) == 0);
    auto* db = databases->Get("sqlite", "example");
    assert(db);
    for (const char* table : {"one", "two"}) {
        assert(db->ExecuteSql((std::string("CREATE TABLE ") + table +
                               "(bucket INTEGER,series TEXT,value REAL,total INTEGER,\"group\" INTEGER)")
                                  .c_str()) == 0);
        assert(db->ExecuteSql((std::string("INSERT INTO ") + table + " VALUES(1,'a',2.5,4,1)").c_str()) >= 0);
    }
    // Bind two real SQLite objects through F2; their physical identity stays unknown,
    // while exact dictionary context remains available for each metric.
    for (const char* table : {"one", "two"}) {
        Json locator(rapidjson::kObjectType), request(rapidjson::kObjectType), preview, saved, applied;
        auto& a = request.GetAllocator();
        Set(locator, "kind", "database_table", locator.GetAllocator());
        Set(locator, "type", "sqlite", locator.GetAllocator());
        Set(locator, "name", "example", locator.GetAllocator());
        Set(locator, "table", table, locator.GetAllocator());
        Set(request, "locator", Copy(locator, a), a);
        DataSemanticsErrorV1 error;
        assert(contracts::Control(loader, "channels/preview", request, &preview, &error) == 0);
        auto& dictionary = preview["data"]["dictionary"];
        for (auto& field : dictionary["fields"].GetArray()) {
            const auto name = Get(field, "name");
            if (name != "value" && name != "total") continue;
            Json unit(rapidjson::kObjectType);
            Set(unit, "state", "declared", unit.GetAllocator());
            Value value(rapidjson::kObjectType);
            const auto code = name == "total" ? "1" : std::string(table) == "one" ? "ms" : "B";
            Set(value, "code", code, unit.GetAllocator());
            Set(value, "label", code, unit.GetAllocator());
            Set(unit, "value", std::move(value), unit.GetAllocator());
            Set(field, "unit", Copy(unit, preview.GetAllocator()), preview.GetAllocator());
        }
        Json save(rapidjson::kObjectType);
        auto& sa = save.GetAllocator();
        Set(save, "kind", "dictionary", sa);
        Set(save, "name", std::string("input_") + table, sa);
        save.AddMember("expected_current_revision", 0, sa);
        Set(save, "document", Copy(dictionary, sa), sa);
        assert(contracts::Control(loader, "save", save, &saved, &error) == 0);
        Set(request, "name", std::string("input_") + table, a);
        Set(request, "dictionary_ref", Get(saved["data"], "exact_reference"), a);
        Set(request, "field_map", Copy(preview["data"]["field_map"], a), a);
        Set(request, "source_basis", Copy(preview["data"]["source_snapshot"], a), a);
        // The API accepts only the declared source-basis fields.
        auto& basis = request["source_basis"];
        for (auto it = basis.MemberBegin(); it != basis.MemberEnd();) {
            const auto key = Text(it->name);
            if (key != "object_key" && key != "source_revision" && key != "schema_fingerprint")
                it = basis.EraseMember(it);
            else
                ++it;
        }
        Set(request, "confirmations", Copy(preview["data"]["required_confirmations"], a), a);
        request.AddMember("expected_current_revision", 0, a);
        assert(contracts::Control(loader, "channels/apply", request, &applied, &error) == 0);
    }
    std::ifstream fixture_configs(std::string(FLOWSQL_SOURCE_ROOT) +
                                  "/src/tests/test_config_channel/fixtures/operator-baseliner-configs.json");
    const std::string text{std::istreambuf_iterator<char>(fixture_configs), {}};
    Json configs;
    configs.Parse(text.c_str());
    assert(configs.IsObject());
    auto baseline_sql = [&](const Value& cfg, const std::string& source, const std::string& name, bool success = true) {
        const auto statement = "SELECT * FROM " + source + " USING explore.baseliner WITH parameters='" + Encode(cfg) +
                               "' INTO dataframe." + name;
        auto response = sql(statement, success);
        if (success) {
            assert(response["contract_checks"].Size() == 1);
            const auto& check = response["contract_checks"][0];
            // Ordinary SQLite tables do not promise persistent physical identity
            // across recreation. Keep F2's explicit unknown identity visible.
            const bool database_source = source.rfind("sqlite.", 0) == 0;
            const std::string expected = database_source ? "undetermined" : "compatible";
            if (check["state"] != expected.c_str()) std::cerr << name << ": " << Encode(response) << '\n';
            assert(check["state"] == expected.c_str());
            if (database_source) {
                bool unknown_identity = false;
                for (const auto& reason : check["reasons"].GetArray())
                    unknown_identity |= reason["code"] == "IDENTITY_UNKNOWN";
                assert(unknown_identity);
            }
            assert(check["output_check"] == "matched" && check.HasMember("use_ref"));
            const auto& outputs = check["outputs"];
            assert(outputs.Size() == 4 && outputs[0]["state"] == "observed");
            assert(outputs[0]["row_cases"].Size() == 2 &&
                   outputs[0]["context_state"] == (database_source ? "complete" : "incomplete"));
            assert(!outputs[0]["semantic_contexts"].Empty());
            assert(outputs[1]["state"] == "absent" && outputs[2]["state"] == "absent" &&
                   outputs[3]["state"] == "absent");
            for (const auto& context : outputs[0]["semantic_contexts"].GetArray()) {
                assert(context["state"] == (database_source ? "complete" : "incomplete") &&
                       context["unit"]["state"] == "unknown");
                if (database_source) {
                    const auto dataset_id = Get(context["when"], "dataset_id");
                    const auto expected_ref =
                        dataset_id == "ratio" ? "dictionary.input_two@1" : "dictionary.input_one@1";
                    assert(context["required_context_refs"].Size() == 1 &&
                           context["required_context_refs"][0] == expected_ref);
                    bool measure = false;
                    for (const auto& role : context["roles"].GetArray())
                        if (Get(role, "column") == "value") {
                            measure = true;
                            assert(role["source_semantics"]["unit"]["value"]["code"] ==
                                   (dataset_id == "ratio" ? "B" : "ms"));
                        }
                    assert(measure);
                    if (context.HasMember("parent_metric_id")) {
                        assert(context["parent_metric_id"] == "distribution" &&
                               context["when"]["metric_id"] == "bytes");
                    }
                }
                assert(!context["roles"].Empty());
            }
            for (const auto& roles : check["role_map"].GetObject()) {
                assert(!roles.value.Empty());
                for (const auto& role : roles.value.GetArray()) assert(role.HasMember("field"));
            }
            auto out = std::dynamic_pointer_cast<IDataFrameChannel>(channels->Get(name.c_str()));
            DataFrame result;
            assert(out && out->Read(&result) == 0 && result.ToArrow());
            assert(result.ToArrow()->schema()->GetFieldIndex("observed") >= 0);
        } else
            assert(!channels->Get(name.c_str()));
        return response;
    };
    for (const auto& item : configs.GetObject())
        baseline_sql(item.value["config"], Get(item.value, "source"), "result_" + Text(item.name));
    Json cold;
    cold.CopyFrom(configs["value"]["config"], cold.GetAllocator());
    Value forecast(rapidjson::kObjectType);
    forecast.AddMember("horizon_buckets", 2, cold.GetAllocator());
    Set(cold, "forecast", std::move(forecast), cold.GetAllocator());
    baseline_sql(cold, "dataframe.input", "cold_forecast");
    DataFrame forecast_frame;
    assert(std::dynamic_pointer_cast<IDataFrameChannel>(channels->Get("cold_forecast"))->Read(&forecast_frame) == 0);
    auto forecast_batch = forecast_frame.ToArrow();
    auto kinds = std::static_pointer_cast<arrow::StringArray>(forecast_batch->GetColumnByName("result_kind"));
    auto targets = std::static_pointer_cast<arrow::Int64Array>(forecast_batch->GetColumnByName("target_bucket"));
    auto issued = std::static_pointer_cast<arrow::Int64Array>(forecast_batch->GetColumnByName("issued_after_bucket"));
    int forecasts = 0, evaluations = 0;
    for (int64_t i = 0; i < forecast_batch->num_rows(); ++i) {
        if (kinds->GetString(i) == "forecast") {
            ++forecasts;
            assert(targets->Value(i) > issued->Value(i));
            assert(forecast_batch->GetColumnByName("observed")->IsNull(i));
            assert(forecast_batch->GetColumnByName("can_alert")->IsNull(i));
            assert(std::static_pointer_cast<arrow::Int32Array>(forecast_batch->GetColumnByName("status"))->Value(i) ==
                   0);
            for (const char* field : {"expected", "lower", "upper"})
                assert(!forecast_batch->GetColumnByName(field)->IsNull(i));
        } else {
            ++evaluations;
            assert(targets->Value(i) == issued->Value(i));
            assert(
                std::static_pointer_cast<arrow::DoubleArray>(forecast_batch->GetColumnByName("observed"))->Value(i) ==
                2.5);
        }
        if (kinds->GetString(i) == "evaluation") {
            assert(std::static_pointer_cast<arrow::Int32Array>(forecast_batch->GetColumnByName("status"))->Value(i) ==
                   1);
            for (const char* field : {"expected", "lower", "upper"})
                assert(forecast_batch->GetColumnByName(field)->IsNull(i));
        }
    }
    assert(forecasts == 2 && evaluations == 1);
    const auto model_sql = "SELECT * FROM dataframe.input USING explore.baseliner WITH parameters='" +
                           Encode(configs["value"]["config"]) +
                           "',model_output='dataframe.model_contract' INTO dataframe.model_results";
    auto model_response = sql(model_sql, true);
    const auto& model = model_response["contract_checks"][0]["outputs"][1];
    assert(model["state"] == "observed" && model.HasMember("binding_ref"));
    assert(model["snapshot"]["locator"]["name"] == "model_contract");
    DataFrame model_frame;
    auto model_channel = std::dynamic_pointer_cast<IDataFrameChannel>(channels->Get("model_contract"));
    assert(model_channel && model_channel->Read(&model_frame) == 0 && model_frame.RowCount() == 1);
    assert(model_frame.ToArrow()->GetColumnByName("parameters_json")->IsNull(0));
    Json lazy;
    lazy.CopyFrom(configs["value"]["config"], lazy.GetAllocator());
    Set(lazy, "source", "dataframe.lazy", lazy.GetAllocator());
    baseline_sql(lazy, "dataframe.lazy", "lazy_result");
    Json missing;
    missing.CopyFrom(configs["value"]["config"], missing.GetAllocator());
    missing["datasets"][0]["metrics"][0]["column"].SetString("absent", missing.GetAllocator());
    auto rejected = baseline_sql(missing, "dataframe.input", "missing_result", false);
    assert(rejected["error"]["code"] == "CONTRACT_INCOMPATIBLE");
    // Exact Config publication uses the ordinary channel API, with unchanged SQL.
    const auto content = Encode(configs["value"]["config"]);
    std::string encoded(4 * ((content.size() + 2) / 3), '\0');
    EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded.data()),
                    reinterpret_cast<const unsigned char*>(content.data()), content.size());
    Json publish(rapidjson::kObjectType);
    auto& pa = publish.GetAllocator();
    Set(publish, "name", "block_input", pa);
    Set(publish, "format", "json", pa);
    Set(publish, "schema_id", "flowsql.baseliner.task.v1", pa);
    Set(publish, "content_base64", encoded, pa);
    publish.AddMember("expected_current_revision", 0, pa);
    bool published = false;
    loader->Traverse(IID_ROUTER_HANDLE, [&](void* ptr) {
        static_cast<IRouterHandle*>(ptr)->EnumRoutes([&](const RouteItem& route) {
            if (route.uri == "/channels/config/publish") {
                std::string out;
                assert(route.handler(route.uri, Encode(publish), out) == 0);
                published = true;
            }
        });
        return 0;
    });
    assert(published);
    result =
        sql("SELECT * FROM dataframe.input USING explore.baseliner WITH config='config.block_input@1' INTO "
            "dataframe.exact_result",
            true);
    bool exact_checked = false;
    for (const auto& ref : result["contract_checks"][0]["checked_refs"].GetArray())
        exact_checked |= ref == "config.block_input@1";
    assert(exact_checked);
    std::cout << "PASS: actual exclusive Arrow block, frozen component, SQL unchanged, THEN parameters, incompatible "
                 "before Open, zero rows\n";
}
