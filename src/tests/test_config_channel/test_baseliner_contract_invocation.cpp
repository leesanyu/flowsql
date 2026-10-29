// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <arrow/api.h>
#include <framework/core/dataframe.h>
#include <framework/core/dataframe_channel.h>
#include <framework/interfaces/iblock_transform_operator.h>
#include <framework/interfaces/ichannel_registry.h>
#include <framework/interfaces/idatabase_factory.h>
#include <openssl/evp.h>
#include <operators/baseliner/snapshot_reader.h>
#include <unistd.h>
#include <cassert>
#include <common/loader.hpp>
#include <common/operator_contract_execution.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace flowsql;
using namespace flowsql::dataset;
int main() {
    char temporary[] = "/tmp/flowsql_baseliner_contract_input_XXXXXX";
    assert(mkdtemp(temporary));
    const std::string root = temporary;
    std::filesystem::create_directories(root + "/frames");
    std::ofstream(root + "/frames/lazy.csv") << "bucket,series,value,total,group\n1,a,2.5,4,1\n";
    auto* loader = PluginLoader::Single();
    struct Owner {
        PluginLoader* loader;
        ~Owner() { loader->Unload(); }
    } owner{loader};
    const char* libraries[]{CONFIG_PLUGIN_PATH, BUILTIN_PLUGIN_PATH, CATALOG_PLUGIN_PATH, DATABASE_PLUGIN_PATH,
                            BASELINER_PLUGIN_PATH};
    const auto config = "db_path=" + root + "/config.db";
    const auto catalog = "data_dir=" + root + "/frames;operator_db_path=" + root + "/catalog.db";
    const auto db = "config_file=" + root + "/database.yml";
    const char* options[]{config.c_str(), nullptr, catalog.c_str(), db.c_str(), nullptr};
    assert(loader->Load(".", libraries, options, 5) == 0 && loader->StartAll() == 0);
    auto* provider = static_cast<IBlockTransformOperatorV2*>(loader->First(IID_BLOCK_TRANSFORM_OPERATOR_V2));
    auto* adapter = dynamic_cast<IOperatorConfigAdapterV1*>(provider);
    auto* channels = static_cast<IChannelRegistry*>(loader->First(IID_CHANNEL_REGISTRY));
    auto* databases = static_cast<IDatabaseFactory*>(loader->First(IID_DATABASE_FACTORY));
    assert(provider && adapter && channels && databases);
    assert(databases->AddChannel(("type=sqlite;name=example;path=" + root + "/input.db").c_str()) == 0);
    auto* database = databases->Get("sqlite", "example");
    assert(database);
    for (const char* table : {"one", "two"}) {
        const auto sql = std::string("CREATE TABLE ") + table +
                         "(bucket INTEGER,series TEXT,value REAL,total INTEGER,\"group\" INTEGER)";
        assert(database->ExecuteSql(sql.c_str()) == 0);
        const auto insert = std::string("INSERT INTO ") + table + " VALUES(1,'series-a',2.5,4,1)";
        assert(database->ExecuteSql(insert.c_str()) >= 0);
    }
    auto input = std::make_shared<DataFrameChannel>("dataframe", "input");
    assert(input->Open() == 0);
    arrow::Int32Builder time;
    arrow::StringBuilder series;
    arrow::FloatBuilder value;
    arrow::UInt32Builder total, group;
    assert(time.Append(1).ok() && series.Append("a").ok() && value.Append(2.5f).ok() && total.Append(4).ok() &&
           group.Append(1).ok());
    auto batch = arrow::RecordBatch::Make(
        arrow::schema({arrow::field("bucket", arrow::int32()), arrow::field("series", arrow::utf8()),
                       arrow::field("value", arrow::float32()), arrow::field("total", arrow::uint32()),
                       arrow::field("group", arrow::uint32())}),
        1,
        {time.Finish().ValueOrDie(), series.Finish().ValueOrDie(), value.Finish().ValueOrDie(),
         total.Finish().ValueOrDie(), group.Finish().ValueOrDie()});
    DataFrame frame;
    frame.FromArrow(batch);
    assert(input->Write(&frame) == 0 && channels->Register("input", input) == 0);
    std::ifstream fixture(std::string(FLOWSQL_SOURCE_ROOT) +
                          "/src/tests/test_config_channel/fixtures/operator-baseliner-configs.json");
    assert(fixture);
    const std::string fixture_text{std::istreambuf_iterator<char>(fixture), {}};
    Json fixtures;
    fixtures.Parse(fixture_text.c_str());
    DataSemanticsErrorV1 error;
    auto invoke = [&](const Value& parameters, const std::string& source,
                      IOperatorConfigAdapterV1* selected = nullptr) {
        contracts::Invocation invocation;
        assert(contracts::Component(loader, dynamic_cast<IOperatorContractIdentityV1*>(provider), "explore",
                                    "baseliner", &invocation.component, &error) == 0);
        Json inputs(rapidjson::kArrayType), context(rapidjson::kObjectType);
        Set(context, "source", source, context.GetAllocator());
        int rc = contracts::CheckInvocation(loader, &invocation, selected ? selected : adapter, parameters, inputs,
                                            context, &error);
        if (rc) std::cerr << error.code << " " << error.field_path << " " << error.message << '\n';
        assert(rc == 0);
        if (Get(invocation.check, "state") == "incompatible") std::cerr << Encode(invocation.check) << '\n';
        assert(Get(invocation.check, "state") != "incompatible");
        assert(invocation.check["data_validation"] == "not_run");
        return invocation;
    };
    Json saved_parameters;
    for (const auto& fixture : fixtures.GetObject()) {
        Json parameters(rapidjson::kObjectType);
        Set(parameters, "parameters", Encode(fixture.value["config"]), parameters.GetAllocator());
        const auto source = Get(fixture.value, "source");
        auto invocation = invoke(parameters, source);
        assert(invocation.resolved["normalized_config"]["read_policy"]["page_rows"].GetUint() == 4096);
        const auto& inputs = invocation.resolved["inputs"];
        assert(inputs[0]["port_id"] == "datasets" && inputs[0]["adaptations"].Size() == 5);
        if (Text(fixture.name) == "multi") {
            assert(inputs.Size() == 2 && inputs[0]["item_key"] == "source" && inputs[1]["item_key"] == "ratio");
            assert(inputs[0]["snapshot"]["locator"]["table"] == "one" &&
                   inputs[1]["snapshot"]["locator"]["table"] == "two");
            assert(invocation.check["role_map"].MemberCount() == 2);
            baseliner::ConfigSnapshot parsed;
            assert(baseliner::ParseConfig(Encode(fixture.value["config"]), &parsed).ok());
            auto* leases =
                static_cast<IDatabaseChannelLeaseProviderV1*>(loader->First(IID_DATABASE_CHANNEL_LEASE_PROVIDER_V1));
            baseliner::SnapshotReader reader;
            assert(leases && reader.Open(parsed.config, leases->AcquireChannel("sqlite", "example"), source) == 0);
            size_t rows = 0;
            for (size_t dataset = 0; dataset < 2;) {
                baseliner::SnapshotPage page;
                assert(reader.Next(&page) == 0);
                if (page.eof) {
                    ++dataset;
                    continue;
                }
                assert(page.batch && page.batch->GetColumnByName("bucket")->type_id() == arrow::Type::INT64);
                assert(page.batch->GetColumnByName("total")->type_id() == arrow::Type::UINT64);
                assert(page.batch->GetColumnByName("value")->type_id() == arrow::Type::DOUBLE);
                rows += page.batch->num_rows();
            }
            assert(rows == 2);
        }
        if (Text(fixture.name) == "relation") {
            const auto roles = Encode(invocation.check["role_map"]);
            assert(roles.find("distribution/bytes") != std::string::npos);
        }
        if (Text(fixture.name) == "value") {
            saved_parameters.CopyFrom(invocation.resolved["effective_parameters"], saved_parameters.GetAllocator());
            assert(inputs[0]["snapshot"]["fields"][0]["type"]["name"] == "int32");
            bool widened = false;
            for (const auto& adaptation : inputs[0]["adaptations"].GetArray())
                if (adaptation["column"] == "bucket") widened = adaptation["target_type"]["name"] == "int64";
            assert(widened);
        }
    }
    // Real config publication and exact lookup; task retains its parsed revision.
    const auto content = Encode(fixtures["value"]["config"]);
    std::string encoded(4 * ((content.size() + 2) / 3), '\0');
    EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded.data()),
                    reinterpret_cast<const unsigned char*>(content.data()), content.size());
    Json publish(rapidjson::kObjectType);
    auto& pa = publish.GetAllocator();
    Set(publish, "name", "contract_input", pa);
    Set(publish, "format", "json", pa);
    Set(publish, "schema_id", "flowsql.baseliner.task.v1", pa);
    Set(publish, "content_base64", encoded, pa);
    publish.AddMember("expected_current_revision", 0, pa);
    Json published;
    bool did_publish = false;
    loader->Traverse(IID_ROUTER_HANDLE, [&](void* ptr) {
        static_cast<IRouterHandle*>(ptr)->EnumRoutes([&](const RouteItem& route) {
            if (route.uri != "/channels/config/publish") return;
            std::string result;
            assert(route.handler(route.uri, Encode(publish), result) == 0);
            published.Parse(result.c_str());
            did_publish = true;
        });
        return 0;
    });
    assert(did_publish);
    Json exact(rapidjson::kObjectType);
    Set(exact, "config", "config.contract_input@1", exact.GetAllocator());
    auto resolved = invoke(exact, "dataframe.input");
    assert(resolved.resolved["config_refs"][0] == "config.contract_input@1");
    bool checked_config = false;
    for (const auto& ref : resolved.check["checked_refs"].GetArray())
        checked_config |= ref == "config.contract_input@1";
    assert(checked_config);
    const auto with = Encode(exact);
    BlockTransformTaskConfigV2 creation{kBlockTransformTaskConfigV2Size, kBlockTransformContractVersionV2, "frozen",
                                        with.c_str(), ""};
    IBlockTransformTaskV2* raw = nullptr;
    assert(provider->CreateTask(creation, &raw) == 0 && raw);
    std::unique_ptr<IBlockTransformTaskV2, std::function<void(IBlockTransformTaskV2*)>> task(
        raw, [&](auto* t) { provider->ReleaseTask(t); });
    auto* task_adapter = dynamic_cast<IOperatorConfigAdapterV1*>(task.get());
    assert(task_adapter);
    Json hostile;
    hostile.CopyFrom(saved_parameters, hostile.GetAllocator());
    Set(hostile, "parameters", "{}", hostile.GetAllocator());
    auto frozen = invoke(hostile, "dataframe.input", task_adapter);
    assert(frozen.resolved["config_refs"][0] == "config.contract_input@1" &&
           frozen.resolved["normalized_config"] == resolved.resolved["normalized_config"]);
    auto reject = [&](Json& request, const char* expected) {
        std::string output = "dirty";
        assert(adapter->ResolveInvocation(Encode(request).c_str(), &output, &error) != 0 && error.code == expected &&
               output.empty());
    };
    Json request;
    request.Parse(R"({"parameters":{"parameters":"{}"},"inputs":[],"execution_context":{"source":"dataframe.input"}})");
    reject(request, "CONTRACT_INCOMPATIBLE");
    request["parameters"].SetObject();
    Set(request["parameters"], "config", "config.contract_input@999", request.GetAllocator());
    reject(request, "NOT_FOUND");
    request["parameters"].CopyFrom(saved_parameters, request.GetAllocator());
    Json missing;
    missing.CopyFrom(fixtures["value"]["config"], missing.GetAllocator());
    missing["datasets"][0]["fields"].AddMember("absent", "float64", missing.GetAllocator());
    Set(request["parameters"], "parameters", Encode(missing), request.GetAllocator());
    reject(request, "CONTRACT_INCOMPATIBLE");
    assert(error.field_path == "/datasets/0/fields/absent");
    // Lazy metadata remains pending and the adapter does not trigger a load.
    Json pending_config;
    pending_config.CopyFrom(fixtures["value"]["config"], pending_config.GetAllocator());
    Set(pending_config, "source", "dataframe.lazy", pending_config.GetAllocator());
    Json pending_parameters(rapidjson::kObjectType);
    Set(pending_parameters, "parameters", Encode(pending_config), pending_parameters.GetAllocator());
    auto pending = invoke(pending_parameters, "dataframe.lazy");
    assert(pending.resolved["inputs"][0]["snapshot"]["schema_state"] == "pending" &&
           pending.check["state"] == "undetermined");
    std::cout << "PASS: real Baseliner adapter, production defaults/config revision/task snapshot, raw DataFrame "
                 "widening, SQLite dataset isolation, nested Relation roles and failures\n";
}
