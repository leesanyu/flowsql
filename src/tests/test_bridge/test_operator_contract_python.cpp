// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <common/error_code.h>
#include <framework/core/dataframe.h>
#include <framework/core/dataframe_channel.h>
#include <framework/interfaces/ibridge.h>
#include <framework/interfaces/ichannel_registry.h>
#include <framework/interfaces/ioperator.h>
#include <framework/interfaces/ioperator_catalog.h>
#include <framework/interfaces/ioperator_contract_catalog.h>
#include <framework/interfaces/ioperator_data_contract.h>
#include <framework/interfaces/ipython_operator_publisher.h>
#include <framework/interfaces/irouter_handle.h>
#include <framework/interfaces/ischeduler_control_service.h>
#include <sqlite3.h>
#include <cassert>
#include <common/channel_dataset.hpp>
#include <common/loader.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
using namespace flowsql;
using namespace flowsql::dataset;
std::string Read(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    assert(in);
    return {std::istreambuf_iterator<char>(in), {}};
}
int main(int argc, char** argv) {
    assert(argc == 3);
    const std::string port = argv[1], root = argv[2];
    auto* loader = PluginLoader::Single();
    struct Owner {
        PluginLoader* loader;
        ~Owner() { loader->Unload(); }
    } owner{loader};
    const char* libs[]{CONFIG_PLUGIN_PATH, BUILTIN_PLUGIN_PATH, CATALOG_PLUGIN_PATH, BRIDGE_PLUGIN_PATH,
                       SCHEDULER_PLUGIN_PATH};
    const auto config = "db_path=" + root + "/config.db";
    const auto catalog_options = "data_dir=" + root + "/frames;operator_db_path=" + root + "/catalog.db";
    const auto bridge_options = "worker_host=127.0.0.1;worker_port=" + port;
    const char* options[]{config.c_str(), nullptr, catalog_options.c_str(), bridge_options.c_str(), nullptr};
    assert(loader->Load(".", libs, options, 5) == 0 && loader->StartAll() == 0);
    auto* contracts = static_cast<IOperatorContractCatalogV1*>(loader->First(IID_OPERATOR_CONTRACT_CATALOG_V1));
    auto* publisher = static_cast<IPythonOperatorPublisherV1*>(loader->First(IID_PYTHON_OPERATOR_PUBLISHER_V1));
    auto* bridge = static_cast<IBridge*>(loader->First(IID_BRIDGE));
    assert(contracts && publisher && bridge);
    auto invoke = [&](const std::string& uri, const Value& request, std::string* response) {
        fnRouterHandler handler;
        loader->Traverse(IID_ROUTER_HANDLE, [&](void* p) {
            static_cast<IRouterHandle*>(p)->EnumRoutes([&](const RouteItem& r) {
                if (r.uri == uri && r.method == "POST") handler = r.handler;
            });
            return handler ? -1 : 0;
        });
        assert(handler);
        return handler(uri, Encode(request), *response);
    };
    auto upload = [&](const std::string& fixture, const std::string& filename, int expected) {
        Json request(rapidjson::kObjectType);
        auto& a = request.GetAllocator();
        Set(request, "type", "python", a);
        Set(request, "filename", filename, a);
        Set(request, "tmp_path", root + "/" + fixture, a);
        std::string response;
        const auto rc = invoke("/operators/upload", request, &response);
        if (rc != expected) std::cerr << response << '\n';
        assert(rc == expected);
    };
    upload("good.py", "py_demo.py", error::OK);
    DataSemanticsErrorV1 issue;
    OperatorComponentContractV1 first, current;
    assert(contracts->Lookup("py", "demo", &first, &issue) == 0);
    assert(first.contract_state == "declared" && first.contract_ref == "operator_contract.py.demo@1");
    assert(first.component_revision == Hash(Read(root + "/good.py")));
    auto held = bridge->FindOperator("py", "demo");
    auto* identity = dynamic_cast<IOperatorContractIdentityV1*>(held.get());
    assert(identity && identity->ContractIdentity().component_revision == first.component_revision);
    auto parallel = bridge->FindOperator("py", "demo");
    assert(parallel != held);
    assert(held->Configure("factor", "3") == 0 && parallel->Configure("factor", "7") == 0);
    std::string prepared;
    auto* adapter = dynamic_cast<IOperatorConfigAdapterV1*>(held.get());
    assert(adapter && adapter->ResolveInvocation(R"({"parameters":{"factor":"3"},"inputs":[],"execution_context":{}})",
                                                 &prepared, &issue) == 0);
    Json effective;
    effective.Parse(prepared.c_str());
    assert(effective["effective_parameters"]["factor"] == "3");
    const auto old_source = Read(root + "/operators/py_demo.py");
    for (const auto& fixture : {"invalid.py", "missing.py", "mixed.py"}) {
        upload(fixture, "py_demo.py", std::string(fixture) == "missing.py" ? error::NOT_FOUND : error::BAD_REQUEST);
        assert(Read(root + "/operators/py_demo.py") == old_source);
        assert(contracts->Lookup("py", "demo", &current, &issue) == 0);
        assert(current.component_revision == first.component_revision && current.contract_ref == first.contract_ref);
        auto unchanged = bridge->FindOperator("py", "demo");
        assert(dynamic_cast<IOperatorContractIdentityV1*>(unchanged.get())->ContractIdentity().component_revision ==
               first.component_revision);
    }
    assert(contracts->Lookup("py", "other", &current, &issue) == 0 && current.component_revision.empty());
    upload("good.py", "py_demo.py", error::OK);
    assert(contracts->Lookup("py", "demo", &current, &issue) == 0 && current.contract_ref == first.contract_ref);
    sqlite3* database = nullptr;
    assert(sqlite3_open((root + "/catalog.db").c_str(), &database) == SQLITE_OK);
    assert(sqlite3_exec(database,
                        "CREATE TRIGGER reject_contract BEFORE UPDATE ON operator_contract_association "
                        "BEGIN SELECT RAISE(ABORT,'fixture association failure'); END",
                        nullptr, nullptr, nullptr) == SQLITE_OK);
    upload("changed.py", "py_demo.py", error::INTERNAL_ERROR);
    assert(Read(root + "/operators/py_demo.py") == old_source);
    assert(contracts->Lookup("py", "demo", &current, &issue) == 0 &&
           current.component_revision == first.component_revision);
    assert(sqlite3_exec(database, "DROP TRIGGER reject_contract", nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(database);
    // The existing content editor follows the same candidate/association protocol.
    Json update(rapidjson::kObjectType);
    auto& a = update.GetAllocator();
    Set(update, "name", "py.demo", a);
    Set(update, "content", Read(root + "/reference.py"), a);
    Set(update, "expected_component_revision", first.component_revision, a);
    std::string response;
    assert(invoke("/operators/update", update, &response) == error::OK);
    assert(contracts->Lookup("py", "demo", &current, &issue) == 0);
    assert(current.contract_ref == first.contract_ref && current.component_revision != first.component_revision);
    assert(current.component_revision == Hash(Read(root + "/operators/py_demo.py")));
    assert(identity->ContractIdentity().component_revision == first.component_revision);
    // Work uses the exact old prepared instance after source replacement.
    DataFrame input;
    input.SetSchema({{"x", DataType::INT64, 0, ""}});
    input.AppendRow({int64_t(2)});
    input.AppendRow({int64_t(4)});
    DataFrameChannel input_channel("dataframe", "input"), output_a("dataframe", "a"), output_b("dataframe", "b");
    assert(input_channel.Open() == 0 && output_a.Open() == 0 && output_b.Open() == 0);
    assert(input_channel.Write(&input) == 0);
    int work_a = -1, work_b = -1;
    std::thread first_work([&] { work_a = held->Work(&input_channel, &output_a); });
    std::thread second_work([&] { work_b = parallel->Work(&input_channel, &output_b); });
    first_work.join();
    second_work.join();
    assert(work_a == 0 && work_b == 0);
    DataFrame result_a, result_b;
    assert(output_a.Read(&result_a) == 0 && output_b.Read(&result_b) == 0);
    auto values_a = std::static_pointer_cast<arrow::Int64Array>(result_a.ToArrow()->column(0));
    auto values_b = std::static_pointer_cast<arrow::Int64Array>(result_b.ToArrow()->column(0));
    assert(values_a->length() == 2 && values_a->Value(0) == 6 && values_a->Value(1) == 12);
    assert(values_b->length() == 2 && values_b->Value(0) == 14 && values_b->Value(1) == 28);
    auto updated = bridge->FindOperator("py", "demo");
    assert(updated->Work(&input_channel, &output_a) == 0);
    assert(output_a.Read(&result_a) == 0);
    values_a = std::static_pointer_cast<arrow::Int64Array>(result_a.ToArrow()->column(0));
    assert(values_a->Value(0) == 1002 && values_a->Value(1) == 1004);
    assert(invoke("/operators/update", update, &response) == error::CONFLICT);
    upload("legacy.py", "py_legacy.py", error::OK);
    assert(contracts->Lookup("py", "legacy", &current, &issue) == 0 && current.contract_state == "missing");
    Json detail(rapidjson::kObjectType);
    Set(detail, "name", "py.demo", detail.GetAllocator());
    assert(invoke("/operators/detail", detail, &response) == error::OK);
    Json decoded;
    decoded.Parse(response.c_str());
    assert(Get(decoded, "contract_ref") == first.contract_ref);
    assert(Get(decoded, "content") == Read(root + "/reference.py"));
    assert(bridge->Refresh() == 0);
    auto* channels = static_cast<IChannelRegistry*>(loader->First(IID_CHANNEL_REGISTRY));
    auto* scheduler = static_cast<ISchedulerControlService*>(loader->First(IID_SCHEDULER_CONTROL_SERVICE));
    auto* directory = static_cast<IOperatorCatalog*>(loader->First(IID_OPERATOR_CATALOG));
    assert(channels && scheduler);
    assert(directory && directory->SetActive("py", "demo", true) == 0);
    auto registered_input = std::make_shared<DataFrameChannel>("dataframe", "input");
    assert(registered_input->Open() == 0 && registered_input->Write(&input) == 0 &&
           channels->Register("input", registered_input) == 0);
    const auto sql =
        R"({"sql":"SELECT * FROM dataframe.input USING py.demo WITH factor='2' THEN py.demo WITH factor='3' INTO dataframe.then_result"})";
    const auto execute_rc = scheduler->ExecuteBatch(sql, &response);
    if (execute_rc) std::cerr << response << '\n';
    assert(execute_rc == 0);
    decoded.Parse(response.c_str());
    assert(decoded["contract_checks"].Size() == 2 && decoded["contract_checks"][0]["state"] == "compatible" &&
           decoded["contract_checks"][1]["state"] == "compatible");
    auto result_channel = channels->Get("then_result");
    assert(result_channel && dynamic_cast<IDataFrameChannel*>(result_channel.get())->Read(&result_a) == 0);
    values_a = std::static_pointer_cast<arrow::Int64Array>(result_a.ToArrow()->column(0));
    assert(values_a->Value(0) == 4012 && values_a->Value(1) == 4024);
    auto wrong_input = std::make_shared<DataFrameChannel>("dataframe", "wrong");
    DataFrame wrong;
    wrong.SetSchema({{"y", DataType::INT64, 0, ""}});
    wrong.AppendRow({int64_t(2)});
    assert(wrong_input->Open() == 0 && wrong_input->Write(&wrong) == 0 &&
           channels->Register("wrong", wrong_input) == 0);
    assert(scheduler->ExecuteBatch(R"({"sql":"SELECT * FROM dataframe.wrong USING py.demo INTO dataframe.rejected"})",
                                   &response) != 0);
    decoded.Parse(response.c_str());
    assert(decoded["error"]["code"] == "CONTRACT_INCOMPATIBLE" && !channels->Get("rejected"));
    std::cout << "PASS: real Worker/Catalog/Bridge, upload/editor, exact refs, digests, no-op, invalid batch, "
                 "conflict, old identity\n";
}
