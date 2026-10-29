// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <framework/core/dataframe.h>
#include <framework/core/dataframe_channel.h>
#include <framework/interfaces/ibinaddon_host.h>
#include <framework/interfaces/ichannel_registry.h>
#include <framework/interfaces/ioperator_catalog.h>
#include <framework/interfaces/ischeduler_control_service.h>
#include <sqlite3.h>
#include <unistd.h>
#include <cassert>
#include <common/loader.hpp>
#include <common/operator_contract_control.hpp>
#include <filesystem>
#include <iostream>
using namespace flowsql;
using namespace flowsql::dataset;
int main() {
    char temporary[] = "/tmp/flowsql_output_execution_XXXXXX";
    assert(mkdtemp(temporary));
    const std::string root = temporary;
    auto* loader = PluginLoader::Single();
    struct Owner {
        PluginLoader* loader;
        ~Owner() { loader->Unload(); }
    } owner{loader};
    const char* libraries[]{CONFIG_PLUGIN_PATH, BUILTIN_PLUGIN_PATH, CATALOG_PLUGIN_PATH, BINADDON_PLUGIN_PATH,
                            SCHEDULER_PLUGIN_PATH};
    const auto config = "db_path=" + root + "/config.db",
               catalog = "data_dir=" + root + "/frames;operator_db_path=" + root + "/catalog.db",
               host_config = "operator_db_path=" + root + "/catalog.db;upload_dir=" + root + "/uploads";
    const char* options[]{config.c_str(), nullptr, catalog.c_str(), host_config.c_str(), nullptr};
    assert(loader->Load(".", libraries, options, 5) == 0 && loader->StartAll() == 0);
    auto* channels = static_cast<IChannelRegistry*>(loader->First(IID_CHANNEL_REGISTRY));
    auto* host = static_cast<IBinAddonHost*>(loader->First(IID_BINADDON_HOST));
    auto* scheduler = static_cast<ISchedulerControlService*>(loader->First(IID_SCHEDULER_CONTROL_SERVICE));
    auto* registry = static_cast<IDataSemanticsRegistryV1*>(loader->First(IID_DATA_SEMANTICS_REGISTRY_V1));
    auto* directory = static_cast<IOperatorCatalog*>(loader->First(IID_OPERATOR_CATALOG));
    assert(channels && host && scheduler && registry && directory);
    std::filesystem::copy_file(FIXTURE_OUTPUT_PATH, root + "/output.so");
    std::string response;
    assert(host->UploadCppPlugin("output.so", root + "/output.so", response) == 0);
    Json uploaded;
    uploaded.Parse(response.c_str());
    assert(host->ActivateCppPlugin(Get(uploaded, "plugin_id"), response) == 0);
    assert(directory->SetActive("contractdemo", "output", true) == 0 &&
           directory->SetActive("builtin", "passthrough", true) == 0);
    auto add = [&](const char* name, bool empty) {
        auto channel = std::make_shared<DataFrameChannel>("dataframe", name);
        assert(channel->Open() == 0);
        arrow::DoubleBuilder b;
        if (!empty) assert(b.AppendValues({1000., 2000., 3000.}).ok());
        DataFrame frame;
        frame.FromArrow(arrow::RecordBatch::Make(arrow::schema({arrow::field("latency", arrow::float64(), false)}),
                                                 empty ? 0 : 3, {b.Finish().ValueOrDie()}));
        assert(channel->Write(&frame) == 0 && channels->Register(name, channel) == 0);
    };
    add("input", false);
    add("empty", true);
    DataSemanticsErrorV1 error;
    auto control = [&](const std::string& op, const Value& request) {
        Json result;
        const int rc = contracts::Control(loader, op, request, &result, &error);
        if (rc) std::cerr << error.code << " " << error.message << " " << error.field_path << '\n';
        assert(rc == 0);
        return result;
    };
    Json request;
    request.Parse(R"({"locator":{"kind":"dataframe","name":"input"}})");
    auto preview = control("channels/preview", request);
    auto& pa = preview.GetAllocator();
    Json unit;
    unit.Parse(R"({"state":"declared","value":{"code":"ms","label":"milliseconds"}})");
    Set(preview["data"]["dictionary"]["fields"][0], "unit", Copy(unit, pa), pa);
    const auto original_id = Get(preview["data"]["dictionary"]["fields"][0], "field_id");
    Json save;
    save.Parse(R"({"kind":"dictionary","name":"milliseconds","expected_current_revision":0})");
    save.AddMember("document", Copy(preview["data"]["dictionary"], save.GetAllocator()), save.GetAllocator());
    auto saved = control("save", save);
    Json apply;
    apply.Parse(
        R"({"locator":{"kind":"dataframe","name":"input"},"name":"input","dictionary_ref":"","field_map":{},"expected_current_revision":0,"source_basis":{},"confirmations":[]})");
    auto& aa = apply.GetAllocator();
    Set(apply, "dictionary_ref", Get(saved["data"], "exact_reference"), aa);
    Set(apply, "field_map", Copy(preview["data"]["field_map"], aa), aa);
    for (const char* key : {"object_key", "source_revision", "schema_fingerprint"})
        apply["source_basis"].AddMember(Value(key, aa), Copy(preview["data"]["source_snapshot"][key], aa), aa);
    control("channels/apply", apply);
    auto sql = [&](const std::string& statement, bool success = true) {
        Json request(rapidjson::kObjectType);
        Set(request, "sql", statement, request.GetAllocator());
        int rc = scheduler->ExecuteBatch(Encode(request), &response);
        if ((rc == 0) != success) std::cerr << response << '\n';
        assert((rc == 0) == success);
        Json result;
        result.Parse(response.c_str());
        assert(result.IsObject());
        return result;
    };
    auto resolve = [&](const std::string& exact) {
        DataSemanticsSnapshotV1 snapshot;
        assert(registry->Resolve(exact.c_str(), &snapshot, &error) == 0);
        Json result;
        result.Parse(snapshot.document_json->c_str());
        return result;
    };
    auto output = [&](const char* name) {
        auto ch = std::dynamic_pointer_cast<IDataFrameChannel>(channels->Get(name));
        DataFrame frame;
        assert(ch && ch->Read(&frame) == 0);
        return frame.ToArrow();
    };
    std::string prior_use, static_contract;
    Json prior_key;
    for (const char* op : {"copy", "rename", "scale", "sum", "count", "mean", "min", "max", "unknown", "dual"}) {
        const std::string operation = op, name = "out_" + operation;
        auto result = sql("SELECT * FROM dataframe.input USING contractdemo.output WITH operation='" + operation +
                          "' INTO dataframe." + name);
        const auto& check = result["contract_checks"][0];
        assert(check["state"] == "compatible" && check["output_check"] == "matched" &&
               check["metadata_state"] == "persisted");
        auto used = resolve(Get(check, "use_ref"));
        if (static_contract.empty()) static_contract = Get(used, "contract_ref");
        assert(Get(used, "contract_ref") == static_contract);
        assert(used["outputs"][0]["snapshot"]["object_key"] == check["outputs"][0]["snapshot"]["object_key"]);
        assert(Get(used["outputs"][0]["snapshot"]["object_key"], "provider_id") != "operator_output");
        auto dictionary = resolve(Get(used["outputs"][0], "dictionary_ref"));
        const auto& field = dictionary["fields"][0];
        if (operation == "unknown")
            assert(field["unit"]["state"] == "unknown");
        else
            assert(field["unit"]["value"]["code"] == (operation == "scale" ? "s" : operation == "count" ? "1" : "ms"));
        if (operation == "rename") assert(field["field_id"] == original_id.c_str());
        auto batch = output(name.c_str());
        auto numbers = std::static_pointer_cast<arrow::DoubleArray>(batch->column(0));
        if (operation == "scale")
            assert(numbers->length() == 3 && numbers->Value(0) == 1 && numbers->Value(1) == 2 &&
                   numbers->Value(2) == 3);
        else if (operation == "sum")
            assert(numbers->length() == 1 && numbers->Value(0) == 6000);
        else if (operation == "count")
            assert(numbers->length() == 1 && numbers->Value(0) == 3);
        else if (operation == "mean")
            assert(numbers->length() == 1 && numbers->Value(0) == 2000);
        else if (operation == "min")
            assert(numbers->length() == 1 && numbers->Value(0) == 1000);
        else if (operation == "max")
            assert(numbers->length() == 1 && numbers->Value(0) == 3000);
        else
            assert(numbers->length() == 3 && numbers->Value(0) == 1000 && numbers->Value(2) == 3000);
        if (operation == "dual") {
            assert(used["outputs"][1]["state"] == "observed" && used["outputs"][1]["row_count"].GetInt64() == 1);
            assert(used["outputs"][0]["binding_ref"] != used["outputs"][1]["binding_ref"]);
            auto second = resolve(Get(used["outputs"][1], "dictionary_ref"));
            assert(second["fields"][0]["name"] == "count");
        } else
            assert(used["outputs"][1]["state"] == "absent");
        if (operation == "copy") {
            prior_use = Get(check, "use_ref");
            prior_key.CopyFrom(used["outputs"][0]["snapshot"]["object_key"], prior_key.GetAllocator());
        }
    }
    auto replaced = sql("SELECT * FROM dataframe.input USING contractdemo.output INTO dataframe.out_copy");
    assert(replaced["contract_checks"][0]["outputs"][0]["snapshot"]["object_key"] != prior_key);
    assert(resolve(prior_use)["outputs"][0]["snapshot"]["object_key"] == prior_key);
    auto chain =
        sql("SELECT * FROM dataframe.input USING contractdemo.output WITH operation='scale' THEN builtin.passthrough "
            "INTO dataframe.seconds");
    assert(chain["contract_checks"].Size() == 2);
    auto last = resolve(Get(chain["contract_checks"][1], "use_ref"));
    auto seconds = resolve(Get(last["outputs"][0], "dictionary_ref"));
    assert(seconds["fields"][0]["unit"]["value"]["code"] == "s");
    auto empty = sql("SELECT * FROM dataframe.empty USING contractdemo.output INTO dataframe.zero");
    assert(empty["rows"].GetInt64() == 0 && empty["contract_checks"][0]["output_check"] == "matched");
    auto drift =
        sql("SELECT * FROM dataframe.input USING contractdemo.output WITH operation='drift' INTO dataframe.bad", false);
    assert(drift["error"]["code"] == "OUTPUT_SCHEMA_MISMATCH" && !channels->Get("bad"));
    // Fail only the named-result use update, after real data was registered.
    sqlite3* database = nullptr;
    assert(sqlite3_open((root + "/config.db").c_str(), &database) == SQLITE_OK);
    assert(sqlite3_exec(database,
                        "CREATE TRIGGER fail_named BEFORE INSERT ON semantics_revision WHEN NEW.kind='operator_use' "
                        "AND NEW.revision=2 BEGIN SELECT RAISE(ABORT,'injected metadata failure'); END",
                        nullptr, nullptr, nullptr) == SQLITE_OK);
    auto failed = sql("SELECT * FROM dataframe.input USING contractdemo.output INTO dataframe.produced", false);
    assert(failed["error"]["code"] == "METADATA_PUBLICATION_FAILED" && channels->Get("produced"));
    const auto& failed_check = failed["error"]["details"]["checks"][0];
    assert(failed_check["data_state"] == "produced" && failed_check["metadata_state"] == "failed");
    request["locator"]["name"].SetString("produced", request.GetAllocator());
    auto unbound = control("channels/check", request);
    assert(unbound["data"]["state"] == "unbound");
    assert(sqlite3_exec(database, "DROP TRIGGER fail_named", nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(database);
    std::cout << "PASS actual numeric SQL, semantic derivation, dual producer outputs, THEN inheritance, replacement "
                 "history, zero/drift and data/metadata failure\n";
}
