// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <framework/interfaces/ioperator_catalog.h>
#include <framework/interfaces/ioperator_contract_catalog.h>
#include <unistd.h>
#include <atomic>
#include <cassert>
#include <common/loader.hpp>
#include <common/operator_contract_control.hpp>
#include <fstream>
#include <iostream>
#include <thread>

using namespace flowsql;
using namespace flowsql::dataset;
namespace {
// The RAII owner outlives all plugin objects and all snapshots with library deleters.
struct Runtime {
    PluginLoader* loader = PluginLoader::Single();
    Runtime(const std::string& directory, bool semantics = true) {
        const char* libraries[]{CONFIG_PLUGIN_PATH, BUILTIN_PLUGIN_PATH, CATALOG_PLUGIN_PATH};
        const std::string config = "db_path=" + directory + "/semantics.db";
        const std::string catalog = "data_dir=" + directory + "/frames;operator_db_dir=" + directory + "/catalog";
        const char* options[]{config.c_str(), nullptr, catalog.c_str()};
        const int skip = semantics ? 0 : 1;
        assert(loader->Load(".", libraries + skip, options + skip, 3 - skip) == 0);
        assert(loader->StartAll() == 0);
    }
    ~Runtime() { loader->Unload(); }
};
std::string Contract() {
    std::ifstream in(std::string(FLOWSQL_SOURCE_ROOT) +
                     "/config/semantics/examples/operator-contract-passthrough.json");
    assert(in.good());
    const std::string source{std::istreambuf_iterator<char>(in), {}};
    Json doc;
    doc.Parse(source.c_str());
    Set(doc["operator"], "category", "fixture", doc.GetAllocator());
    Set(doc["operator"], "name", "copy", doc.GetAllocator());
    return Encode(doc);
}
}  // namespace
int main() {
    char path[] = "/tmp/flowsql_contract_registration_XXXXXX";
    assert(mkdtemp(path));
    OperatorComponentContractV1 held;
    {
        Runtime runtime(path);
        auto* catalog =
            static_cast<IOperatorContractCatalogV1*>(runtime.loader->First(IID_OPERATOR_CONTRACT_CATALOG_V1));
        assert(catalog);
        DataSemanticsErrorV1 e;
        OperatorComponentContractV1 current;
        assert(catalog->Lookup("fixture", "copy", &current, &e) == 0);
        assert(current.contract_state == "missing" && current.component_revision.empty());
        const auto original = Contract();
        OperatorContractDeclarationV1 d{"fixture", "copy", std::string(64, 'a'), original, ""};
        std::vector<PreparedOperatorContractV1> ready;
        assert(catalog->Prepare({d}, &ready, &e) == 0);
        assert(ready.size() == 1 && ready[0].association.contract_ref == "operator_contract.fixture.copy@1");
        // Saving the definition does not claim the component is associated.
        assert(catalog->Lookup("fixture", "copy", &current, &e) == 0 && current.contract_state == "missing");
        auto forged = ready;
        forged[0].association.operator_version = "forged";
        assert(catalog->Commit(forged, &e) != 0 && e.code == "CONTRACT_INVALID");
        forged = ready;
        forged[0].association.contract_ref = "operator_contract.fixture.copy@999";
        assert(catalog->Commit(forged, &e) != 0 && e.code == "NOT_FOUND");
        assert(catalog->Commit(ready, &e) == 0);
        assert(catalog->Lookup("fixture", "copy", &held, &e) == 0);
        assert(held.contract_state == "declared" && held.component_revision == std::string(64, 'a'));
        d.expected_component_revision = held.component_revision;
        assert(catalog->Prepare({d}, &ready, &e) == 0);
        assert(ready[0].association.contract_ref == held.contract_ref);
        assert(catalog->Commit(ready, &e) == 0);
        auto reference = d;
        reference.declaration_json = R"({"contract_ref":"operator_contract.fixture.copy@1"})";
        assert(catalog->Prepare({reference}, &ready, &e) == 0 &&
               ready[0].association.contract_ref == held.contract_ref);
        reference.declaration_json = R"({"contract_ref":"operator_contract.fixture.copy@latest"})";
        assert(catalog->Prepare({reference}, &ready, &e) != 0 && ready.empty());
        reference.declaration_json = R"({"contract_ref":"operator_contract.other.operator@1"})";
        assert(catalog->Prepare({reference}, &ready, &e) != 0 && ready.empty());
        // A later invalid declaration prevents even an earlier valid definition write.
        auto changed = d;
        Json document;
        document.Parse(original.c_str());
        Set(document, "title", "Candidate not published", document.GetAllocator());
        changed.declaration_json = Encode(document);
        auto bad = changed;
        bad.name = "another";
        assert(catalog->Prepare({changed, bad}, &ready, &e) != 0 && ready.empty());
        Json history(rapidjson::kObjectType), response;
        Set(history, "kind", "operator_contract", history.GetAllocator());
        Set(history, "name", "fixture.copy", history.GetAllocator());
        assert(contracts::Control(runtime.loader, "history", history, &response, &e) == 0);
        assert(response["data"]["items"].Size() == 1);
        // Two actual component candidates compete against the same version.
        auto first = d, second = d;
        first.component_revision = std::string(64, 'b');
        second.component_revision = std::string(64, 'c');
        std::vector<PreparedOperatorContractV1> a, b;
        assert(catalog->Prepare({first}, &a, &e) == 0);
        assert(catalog->Prepare({second}, &b, &e) == 0);
        std::atomic<int> won{0}, conflicted{0};
        auto commit = [&](const auto& candidate) {
            DataSemanticsErrorV1 error;
            if (catalog->Commit(candidate, &error) == 0)
                ++won;
            else {
                assert(error.code == "COMPONENT_CHANGED");
                ++conflicted;
            }
        };
        std::thread one(commit, std::cref(a)), two(commit, std::cref(b));
        one.join();
        two.join();
        assert(won == 1 && conflicted == 1);
        assert(catalog->Lookup("fixture", "copy", &current, &e) == 0);
        assert(current.component_revision != held.component_revision && current.contract_ref == held.contract_ref);
        // Failed batch rolls back a previously inserted row in the same transaction.
        OperatorContractDeclarationV1 legacy{"legacy", "old", std::string(64, 'd'), "", ""};
        assert(catalog->Prepare({legacy}, &ready, &e) == 0);
        ready.push_back(a[0]);  // stale compare-and-swap, following a new legacy row
        assert(catalog->Commit(ready, &e) != 0 && e.code == "COMPONENT_CHANGED");
        assert(catalog->Lookup("legacy", "old", &current, &e) == 0 && current.component_revision.empty());
        assert(catalog->Prepare({legacy}, &ready, &e) == 0 && catalog->Commit(ready, &e) == 0);
        assert(catalog->Lookup("legacy", "old", &current, &e) == 0 && current.contract_state == "missing");
        assert(current.operator_version.empty());
        auto* old_catalog = static_cast<IOperatorCatalog*>(runtime.loader->First(IID_OPERATOR_CATALOG));
        assert(old_catalog->UpsertBatch({{"fixture", "copy", "fixture", "fixture", "copy", "data"}}).success_count ==
               1);
        fnRouterHandler detail;
        runtime.loader->Traverse(IID_ROUTER_HANDLE, [&](void* ptr) {
            static_cast<IRouterHandle*>(ptr)->EnumRoutes([&](const RouteItem& r) {
                if (r.uri == "/operators/detail") detail = r.handler;
            });
            return 0;
        });
        assert(detail);
        std::string text;
        assert(detail("/operators/detail", R"({"name":"fixture.copy"})", text) == 0);
        Json details;
        details.Parse(text.c_str());
        assert(Get(details, "contract_state") == "declared" && Get(details, "contract_ref") == held.contract_ref);
        assert(Get(details, "version_state") == "declared");
    }
    {
        Runtime restarted(path);
        auto* catalog =
            static_cast<IOperatorContractCatalogV1*>(restarted.loader->First(IID_OPERATOR_CONTRACT_CATALOG_V1));
        OperatorComponentContractV1 current;
        DataSemanticsErrorV1 e;
        assert(catalog->Lookup("fixture", "copy", &current, &e) == 0);
        assert(current.contract_ref == held.contract_ref && current.component_revision != held.component_revision);
        assert(held.component_revision == std::string(64, 'a'));
    }
    {
        Runtime unavailable(path, false);
        auto* catalog =
            static_cast<IOperatorContractCatalogV1*>(unavailable.loader->First(IID_OPERATOR_CONTRACT_CATALOG_V1));
        OperatorComponentContractV1 current;
        DataSemanticsErrorV1 e;
        assert(catalog->Lookup("fixture", "copy", &current, &e) == 0);
        std::vector<PreparedOperatorContractV1> ready;
        OperatorContractDeclarationV1 changed{"fixture", "copy", std::string(64, 'e'), Contract(),
                                              current.component_revision};
        assert(catalog->Prepare({changed}, &ready, &e) != 0 && e.code == "UNAVAILABLE" && ready.empty());
        OperatorComponentContractV1 after;
        assert(catalog->Lookup("fixture", "copy", &after, &e) == 0);
        assert(after.component_revision == current.component_revision && after.contract_ref == held.contract_ref);
    }
    std::cout << "PASS: actual plugins, definition/association separation, component CAS, batch rollback, restart, "
                 "legacy ABI\n";
}
