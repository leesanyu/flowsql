// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <framework/interfaces/idata_semantics_registry.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <cassert>
#include <common/loader.hpp>
#include <iostream>
#include <string>
#include <vector>

class TestRuntime {
 public:
    flowsql::PluginLoader* loader = flowsql::PluginLoader::Single();
    ~TestRuntime() {
        loader->StopAll();
        assert(loader->Unload() == 0);
    }
};
int main(int argc, char** argv) {
    assert(argc == 5 || argc == 6);
    const bool provider = argc == 5;
    TestRuntime runtime;
    const std::string root = argv[1], gateway_port = argv[2], router_port = argv[3], web_port = argv[4];
    const std::vector<std::string> options = {
        "host=127.0.0.1;port=" + gateway_port,
        "host=127.0.0.1;port=" + router_port + ";gateway=127.0.0.1:" + gateway_port, "db_path=" + root + "/config.db",
        "host=127.0.0.1;port=" + web_port + ";gateway=127.0.0.1:" + gateway_port + ";db_path=" + root +
            "/web.db;upload_dir=" + root + "/uploads"};
    const char* libraries[] = {"libflowsql_gateway.so", "libflowsql_router.so", "libflowsql_config_channel.so",
                               "libflowsql_web.so"};
    const char* pointers[] = {options[0].c_str(), options[1].c_str(), options[2].c_str(), options[3].c_str()};
    if (!provider) {
        libraries[2] = libraries[3];
        pointers[2] = pointers[3];
    }
    assert(runtime.loader->Load(flowsql::get_absolute_process_path(), libraries, pointers, provider ? 4 : 3) == 0);
    assert(runtime.loader->StartAll() == 0);
    auto* registry =
        static_cast<flowsql::IDataSemanticsRegistryV1*>(runtime.loader->First(flowsql::IID_DATA_SEMANTICS_REGISTRY_V1));
    assert(!provider || registry);
    std::string reference;
    while (std::getline(std::cin, reference) && reference != "quit") {
        flowsql::DataSemanticsSnapshotV1 snapshot;
        flowsql::DataSemanticsErrorV1 error;
        rapidjson::Document result(rapidjson::kObjectType);
        auto& a = result.GetAllocator();
        const int rc = registry ? registry->Resolve(reference.c_str(), &snapshot, &error) : -1;
        result.AddMember("ok", rc == 0, a);
        if (rc == 0) {
            result.AddMember("exact_reference", rapidjson::Value(snapshot.exact_reference.c_str(), a), a);
            result.AddMember("sha256", rapidjson::Value(snapshot.sha256.c_str(), a), a);
            rapidjson::Document document;
            document.Parse(snapshot.document_json->c_str());
            result.AddMember("document", rapidjson::Value(document, a), a);
        }
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        result.Accept(writer);
        std::cout << "SEMANTICS_IID " << buffer.GetString() << std::endl;
    }
}
