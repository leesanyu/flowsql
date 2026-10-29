// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <framework/core/dataframe.h>
#include <framework/core/dataframe_channel.h>
#include <framework/interfaces/ibinaddon_host.h>
#include <framework/interfaces/ichannel_registry.h>
#include <framework/interfaces/ioperator_catalog.h>
#include <framework/interfaces/ischeduler_control_service.h>
#include <unistd.h>
#include <cassert>
#include <common/channel_dataset.hpp>
#include <common/loader.hpp>
#include <filesystem>
#include <iostream>
using namespace flowsql;
using namespace flowsql::dataset;
class ReplacedDuringRead : public DataFrameChannel {
 public:
    ReplacedDuringRead() : DataFrameChannel("dataframe", "changing") {}
    bool armed = false;
    int Read(IDataFrame* output) override {
        const auto rc = DataFrameChannel::Read(output);
        if (armed) {
            armed = false;
            DataFrame replacement;
            replacement.SetSchema({{"x", DataType::INT64, 0, ""}});
            replacement.AppendRow({int64_t(99)});
            assert(Write(&replacement) == 0);
        }
        return rc;
    }
};
int main() {
    char temporary[] = "/tmp/flowsql_contract_execution_XXXXXX";
    assert(mkdtemp(temporary));
    const std::string root = temporary;
    auto* loader = PluginLoader::Single();
    struct Owner {
        PluginLoader* loader;
        ~Owner() { loader->Unload(); }
    } owner{loader};
    const char* libraries[]{CONFIG_PLUGIN_PATH, BUILTIN_PLUGIN_PATH, CATALOG_PLUGIN_PATH, BINADDON_PLUGIN_PATH,
                            SCHEDULER_PLUGIN_PATH};
    const auto config = "db_path=" + root + "/config.db";
    const auto catalog = "data_dir=" + root + "/frames;operator_db_path=" + root + "/catalog.db";
    const auto host_config = "operator_db_path=" + root + "/catalog.db;upload_dir=" + root + "/uploads";
    const char* options[]{config.c_str(), nullptr, catalog.c_str(), host_config.c_str(), nullptr};
    assert(loader->Load(".", libraries, options, 5) == 0 && loader->StartAll() == 0);
    auto* channels = static_cast<IChannelRegistry*>(loader->First(IID_CHANNEL_REGISTRY));
    auto* host = static_cast<IBinAddonHost*>(loader->First(IID_BINADDON_HOST));
    auto* scheduler = static_cast<ISchedulerControlService*>(loader->First(IID_SCHEDULER_CONTROL_SERVICE));
    assert(channels && host && scheduler);
    std::string response;
    const auto fixture = root + "/fixture.so";
    std::filesystem::copy_file(FIXTURE_EXECUTION_PATH, fixture);
    assert(host->UploadCppPlugin("fixture.so", fixture, response) == 0);
    Json uploaded;
    uploaded.Parse(response.c_str());
    assert(host->ActivateCppPlugin(Get(uploaded, "plugin_id"), response) == 0);
    auto* directory = static_cast<IOperatorCatalog*>(loader->First(IID_OPERATOR_CATALOG));
    assert(directory && directory->SetActive("builtin", "passthrough", true) == 0 &&
           directory->SetActive("builtin", "concat", true) == 0 &&
           directory->SetActive("contractdemo", "execution", true) == 0);
    auto add = [&](const char* name, const char* column, bool rows) {
        auto channel = std::make_shared<DataFrameChannel>("dataframe", name);
        assert(channel->Open() == 0);
        DataFrame frame;
        frame.SetSchema({{column, DataType::INT64, 0, ""}});
        if (rows) {
            frame.AppendRow({int64_t(2)});
            frame.AppendRow({int64_t(4)});
        }
        assert(channel->Write(&frame) == 0 && channels->Register(name, channel) == 0);
    };
    add("a", "x", true);
    add("b", "other", true);
    add("empty", "x", false);
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
    auto passthrough = sql("SELECT * FROM dataframe.a USING builtin.passthrough", true);
    assert(passthrough["contract_checks"][0]["state"] == "compatible");
    assert(passthrough["contract_checks"][0]["output_check"] == "matched");
    assert(passthrough["contract_checks"][0].HasMember("use_ref"));
    sql("SELECT * FROM dataframe.a USING contractdemo.execution WITH column='x'", true);
    auto chain =
        sql("SELECT * FROM dataframe.a USING builtin.passthrough THEN contractdemo.execution WITH column='x'", true);
    assert(chain["contract_checks"].Size() == 2 && chain["contract_checks"][1]["state"] == "compatible");
    const auto marker = root + "/must-not-execute";
    auto invalid = sql("SELECT * FROM dataframe.a USING contractdemo.execution WITH column='absent',marker='" + marker +
                           "' INTO dataframe.bad",
                       false);
    assert(invalid["error"]["code"] == "CONTRACT_INCOMPATIBLE");
    assert(!std::filesystem::exists(marker) && !channels->Get("bad"));
    const auto& roles = invalid["error"]["details"]["checks"][0]["role_map"]["left//0"];
    assert(roles[0]["column"] == "absent");
    invalid = sql(
        "SELECT * FROM dataframe.a,dataframe.b USING contractdemo.execution WITH column='x',marker='" + marker + "'",
        false);
    assert(invalid["error"]["code"] == "CONTRACT_INCOMPATIBLE" && !std::filesystem::exists(marker));
    assert(invalid["error"]["details"]["checks"][0]["role_map"].MemberCount() == 2);
    auto changing = std::make_shared<ReplacedDuringRead>();
    DataFrame before;
    before.SetSchema({{"x", DataType::INT64, 0, ""}});
    before.AppendRow({int64_t(2)});
    assert(changing->Open() == 0 && changing->Write(&before) == 0 && channels->Register("changing", changing) == 0);
    changing->armed = true;
    auto conflict = sql(
        "SELECT * FROM dataframe.changing USING contractdemo.execution WITH column='x',marker='" + marker + "'", false);
    assert(conflict["error"]["code"] == "SOURCE_CHANGED" && !std::filesystem::exists(marker));
    auto empty = sql("SELECT * FROM dataframe.empty USING contractdemo.execution WITH column='x'", true);
    assert(empty["rows"].GetInt64() == 0 && empty["contract_checks"][0]["state"] == "compatible");
    auto historical = sql("SELECT * FROM dataframe.a,dataframe.a USING builtin.concat", true);
    assert(historical["contract_checks"][0]["state"] == "undetermined");
    std::cout << "PASS: real Scheduler/C++ component, actual Schema, SQL without contract, THEN, isolated inputs, "
                 "incompatible before Work, zero rows, historical undeclared\n";
}
