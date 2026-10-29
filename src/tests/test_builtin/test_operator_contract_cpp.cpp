// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <framework/interfaces/cpp_operator_plugin_abi.h>
#include <framework/interfaces/ibinaddon_host.h>
#include <framework/interfaces/iblock_transform_operator.h>
#include <framework/interfaces/ibuiltin_registry.h>
#include <framework/interfaces/icpp_operator_contract_provider.h>
#include <framework/interfaces/icpp_operator_plugin_registry.h>
#include <framework/interfaces/ioperator_data_contract.h>
#include <framework/interfaces/ioperator_registry.h>
#include <tests/support/scoped_shared_library.h>
#include <unistd.h>
#include <cassert>
#include <common/loader.hpp>
#include <common/operator_component.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace flowsql;
using namespace flowsql::dataset;
struct Runtime {
    PluginLoader* loader = PluginLoader::Single();
    Runtime(const std::string& root) {
        const char* libs[]{CONFIG_PLUGIN_PATH, BUILTIN_PLUGIN_PATH, CATALOG_PLUGIN_PATH, BINADDON_PLUGIN_PATH};
        const auto config = "db_path=" + root + "/config.db";
        const auto catalog = "data_dir=" + root + "/frames;operator_db_path=" + root + "/catalog.db";
        const auto host = "operator_db_path=" + root + "/catalog.db;upload_dir=" + root + "/uploads";
        const char* options[]{config.c_str(), nullptr, catalog.c_str(), host.c_str()};
        assert(loader->Load(".", libs, options, 4) == 0);
        assert(loader->StartAll() == 0);
    }
    ~Runtime() { loader->Unload(); }
};
std::string DigestFile(const std::string& file) {
    std::ifstream in(file, std::ios::binary);
    return Hash({std::istreambuf_iterator<char>(in), {}});
}
int main() {
    char root[] = "/tmp/flowsql_contract_cpp_XXXXXX";
    assert(mkdtemp(root));
    Runtime runtime(root);
    auto* catalog = static_cast<IOperatorContractCatalogV1*>(runtime.loader->First(IID_OPERATOR_CONTRACT_CATALOG_V1));
    auto* host = static_cast<IBinAddonHost*>(runtime.loader->First(IID_BINADDON_HOST));
    auto* registry = static_cast<IOperatorRegistry*>(runtime.loader->First(IID_OPERATOR_REGISTRY));
    auto* builtin = static_cast<IBuiltinRegistry*>(runtime.loader->First(IID_BUILTIN_REGISTRY));
    assert(catalog && host && registry && builtin);
    DataSemanticsErrorV1 e;
    OperatorComponentContractV1 c;
    assert(catalog->Lookup("builtin", "passthrough", &c, &e) == 0);
    assert(c.contract_state == "declared" && c.component_revision == DigestFile(BUILTIN_PLUGIN_PATH));
    {
        std::unique_ptr<IOperator> op(builtin->CreateBuiltinOperator("BUILTIN", "PassThrough"));
        auto* identity = dynamic_cast<IOperatorContractIdentityV1*>(op.get());
        assert(identity);
        assert(identity->ContractIdentity().contract_ref == c.contract_ref);
    }
    auto upload = [&](const char* path, int expected) {
        const std::string filename = std::filesystem::path(path).filename();
        const std::string tmp = std::string(root) + "/" + filename;
        std::filesystem::copy_file(path, tmp, std::filesystem::copy_options::overwrite_existing);
        std::string response;
        assert(host->UploadCppPlugin(filename, tmp, response) == 0);
        Json d;
        d.Parse(response.c_str());
        const auto id = Get(d, "plugin_id");
        assert(id == DigestFile(path));
        const int rc = host->ActivateCppPlugin(id, response);
        if (expected == 0 && rc) std::cerr << response << '\n';
        assert((rc == 0) == (expected == 0));
        return id;
    };
    setenv("FLOWSQL_FIXTURE_CPP_OPERATOR_V2_VERSION_PAIR", "1", 1);
    setenv("FLOWSQL_FIXTURE_PAIRED_INVALID_CONTRACT", "1", 1);
    upload(FIXTURE_PAIR_PATH, 1);
    unsetenv("FLOWSQL_FIXTURE_PAIRED_INVALID_CONTRACT");
    unsetenv("FLOWSQL_FIXTURE_CPP_OPERATOR_V2_VERSION_PAIR");
    for (const auto* path : {FIXTURE_V1_PATH, FIXTURE_V2_PATH}) {
        const bool v2 = std::string(path) == FIXTURE_V2_PATH;
        const std::string name = v2 ? "v2" : "v1";
        const auto id = upload(path, 0);
        assert(catalog->Lookup("contractdemo", name.c_str(), &c, &e) == 0);
        assert(c.contract_state == "declared" && c.component_revision == DigestFile(path));
        assert(c.contract_ref == "operator_contract.contractdemo." + name + "@1");
        std::string response;
        if (!v2) {
            std::unique_ptr<IOperator> op(registry->Create(("contractdemo." + name).c_str()));
            assert(op);
            auto* identity = dynamic_cast<IOperatorContractIdentityV1*>(op.get());
            assert(identity);
            const auto held = identity->ContractIdentity();
            assert(held.component_revision == c.component_revision && held.contract_ref == c.contract_ref);
            auto* adapter = dynamic_cast<IOperatorConfigAdapterV1*>(op.get());
            std::string adapted;
            assert(adapter && adapter->ResolveInvocation("{}", &adapted, &e) == 0);
            Json effective;
            effective.Parse(adapted.c_str());
            assert(effective["effective_parameters"]["factor"].GetInt() == 7);
            assert(host->DeactivateCppPlugin(id, response) != 0);
        } else {
            auto* leases =
                static_cast<ICppOperatorPluginRegistryV1*>(runtime.loader->First(IID_CPP_OPERATOR_PLUGIN_REGISTRY_V1));
            CppOperatorCapabilityLeaseV1 lease;
            assert(leases->Acquire("contractdemo", "v2", IID_OPERATOR, &lease) == 0);
            assert(lease.capability && lease.lifetime);
            assert(host->DeactivateCppPlugin(id, response) != 0);
        }
        assert(host->DeactivateCppPlugin(id, response) == 0);
        assert(host->ActivateCppPlugin(id, response) == 0);
        assert(catalog->Lookup("contractdemo", name.c_str(), &c, &e) == 0 && c.contract_ref.back() == '1');
        assert(host->DeactivateCppPlugin(id, response) == 0);
    }
    // Register a separate exact definition before loading the reference-only component.
    {
        test::ScopedSharedLibrary library(FIXTURE_V1_PATH);
        assert(library);
        auto fn = reinterpret_cast<OperatorDataContractV1Fn>(library.Symbol(kOperatorDataContractV1Symbol));
        Json doc;
        doc.Parse(fn(0));
        Set(doc["operator"], "name", "reference", doc.GetAllocator());
        std::vector<PreparedOperatorContractV1> prepared;
        assert(catalog->Prepare({{"contractdemo", "reference", DigestFile(FIXTURE_REFERENCE_PATH), Encode(doc), ""}},
                                &prepared, &e) == 0);
    }
    // V1 reference resolves the immutable document without a new revision.
    const auto ref = upload(FIXTURE_REFERENCE_PATH, 0);
    std::string response;
    assert(host->DeactivateCppPlugin(ref, response) == 0);
    for (const auto* path : {FIXTURE_INVALID_PATH, FIXTURE_TOO_LONG_PATH, FIXTURE_THROW_PATH}) upload(path, 1);
    assert(catalog->Lookup("contractdemo", "v1", &c, &e) == 0 && c.contract_state == "declared");
    // Baseliner's real optional export is a closed template for the production component.
    {
        test::ScopedSharedLibrary library(BASELINER_PLUGIN_PATH);
        assert(library);
        auto fn = reinterpret_cast<OperatorDataContractV1Fn>(library.Symbol(kOperatorDataContractV1Symbol));
        assert(fn);
        Json doc;
        doc.Parse(fn(0));
        assert(Get(doc["operator"], "name") == "baseliner" && doc["outputs"].Size() == 4);
        assert(fn(1) == nullptr);
    }
    const auto baseliner_id = upload(BASELINER_PLUGIN_PATH, 0);
    assert(catalog->Lookup("explore", "baseliner", &c, &e) == 0 && c.contract_state == "declared");
    assert(c.component_revision == DigestFile(BASELINER_PLUGIN_PATH) &&
           c.contract_ref == "operator_contract.explore.baseliner@1");
    {
        auto* contracts =
            static_cast<ICppOperatorContractProviderV1*>(runtime.loader->First(IID_CPP_OPERATOR_CONTRACT_PROVIDER_V1));
        assert(contracts);
        CppOperatorCapabilityLeaseV1 lease, second;
        OperatorComponentContractV1 held, fresh;
        assert(contracts->AcquireContract("explore", "baseliner", IID_BLOCK_TRANSFORM_OPERATOR_V2, &lease, &held) == 0);
        assert(lease.capability && lease.lifetime && held.component_revision == c.component_revision &&
               held.contract_ref == c.contract_ref);
        // A mutable catalog association cannot rewrite the actual loaded component.
        PreparedOperatorContractV1 updated;
        updated.association = c;
        updated.expected_component_revision = c.component_revision;
        updated.association.component_revision = Hash("replacement component");
        assert(catalog->Commit({updated}, &e) == 0);
        assert(contracts->AcquireContract("explore", "baseliner", IID_BLOCK_TRANSFORM_OPERATOR_V2, &second, &fresh) ==
               0);
        assert(fresh.component_revision == held.component_revision && fresh.contract_ref == held.contract_ref);
        assert(host->DeactivateCppPlugin(baseliner_id, response) != 0);
        assert(contracts->AcquireContract("absent", "missing", IID_OPERATOR, &second, &fresh) != 0);
        assert(!second.capability && !second.lifetime && fresh.component_revision.empty());
    }
    assert(host->DeactivateCppPlugin(baseliner_id, response) == 0);
    std::cout << "PASS: builtin, real V1/V2 optional metadata, exact reference, digest, leases, export failures, "
                 "Baseliner package\n";
}
