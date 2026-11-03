// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <arrow/api.h>
#include <framework/core/packet_codec.h>
#include <framework/interfaces/ichannel_dataset_descriptor.h>
#include <framework/interfaces/idatabase_factory.h>
#include <framework/interfaces/imetadata_description.h>
#include <operators/npm_basic/core/npm_task_budget.h>
#include <operators/npm_basic/npm_basic_result_consumer.h>
#include <operators/npm_basic/npm_family_result_contract.h>
#include <operators/npm_basic/npm_result_semantics.h>
#include <common/loader.hpp>
#include <common/operator_block_contract.hpp>
#include <common/operator_contract_control.hpp>

#include <cassert>
#include <cstring>
#include <filesystem>
#include <iostream>

extern "C" const char* flowsql_operator_data_contract_v1(int index);
extern "C" void* flowsql_create_operator_capability(int index, flowsql::IQuerier* querier);
extern "C" void flowsql_destroy_operator_capability(int index, void* capability);
using namespace flowsql;
using namespace flowsql::dataset;
namespace {
// A metadata provider can expose another database type without changing the
// publisher or public protocol. This fixture uses real SQLite catalog facts.
class ExtensibleDatasetProvider final : public IChannelDatasetDescriptorV1 {
 public:
    explicit ExtensibleDatasetProvider(PluginLoader* loader) : loader_(loader) {}
    const char* ProviderId() const override { return "database"; }
    int Describe(const ChannelDatasetRequestV1& request, ChannelDatasetSnapshotV1* out,
                 DataSemanticsErrorV1* error) override {
        IChannelDatasetDescriptorV1* provider = nullptr;
        loader_->Traverse(IID_CHANNEL_DATASET_DESCRIPTOR_V1, [&](void* value) {
            auto* candidate = static_cast<IChannelDatasetDescriptorV1*>(value);
            if (candidate != this && std::string(candidate->ProviderId()) == "database") provider = candidate;
            return provider ? 1 : 0;
        });
        assert(provider);
        Json locator;
        locator.Parse(request.locator_json.c_str());
        if (Get(locator, "type") != "extension_db") return provider->Describe(request, out, error);
        Set(locator, "type", "sqlite", locator.GetAllocator());
        if (provider->Describe({Encode(locator), request.timeout_ms}, out, error)) return -1;
        Json snapshot;
        snapshot.Parse(out->document_json->c_str());
        auto& a = snapshot.GetAllocator();
        Set(snapshot["locator"], "type", "extension_db", a);
        Set(snapshot["object_key"], "object_id", Hash("extension:" + Get(snapshot["object_key"], "object_id")), a);
        out->document_json = std::make_shared<const std::string>(Encode(snapshot));
        return 0;
    }

 private:
    PluginLoader* loader_;
};
class StaticContractCatalog final : public IOperatorContractCatalogV1 {
 public:
    OperatorComponentContractV1 component;
    int Prepare(const std::vector<OperatorContractDeclarationV1>&, std::vector<PreparedOperatorContractV1>*,
                DataSemanticsErrorV1*) override {
        assert(false);
        return -1;
    }
    int Commit(const std::vector<PreparedOperatorContractV1>&, DataSemanticsErrorV1*) override {
        assert(false);
        return -1;
    }
    int Lookup(const char* category, const char* name, OperatorComponentContractV1* out,
               DataSemanticsErrorV1* error) override {
        if (component.category != category || component.name != name)
            return Fail(error, "NOT_FOUND", "fixture contract not found");
        *out = component;
        return 0;
    }
};
class ControlledServices final : public IQuerier, public IOperatorDataContractV1 {
 public:
    explicit ControlledServices(IQuerier* base)
        : base_(base), service_(static_cast<IOperatorDataContractV1*>(base->First(IID_OPERATOR_DATA_CONTRACT_V1))) {}
    bool fail_records = false;
    bool hide_control = false;
    int Traverse(const Guid& id, fntraverse visitor) override {
        return hide_control && std::memcmp(&id, &IID_ROUTER_HANDLE, sizeof(Guid)) == 0
                   ? 0
                   : base_->Traverse(id, std::move(visitor));
    }
    void* First(const Guid& id) override {
        return std::memcmp(&id, &IID_OPERATOR_DATA_CONTRACT_V1, sizeof(Guid)) == 0
                   ? static_cast<IOperatorDataContractV1*>(this)
                   : base_->First(id);
    }
    int Resolve(const char* ref, OperatorContractSnapshotV1* out, DataSemanticsErrorV1* error) override {
        return service_->Resolve(ref, out, error);
    }
    int CheckAndDerive(const char* request, std::string* out, DataSemanticsErrorV1* error) override {
        return service_->CheckAndDerive(request, out, error);
    }
    int RecordUse(const char* request, OperatorContractSnapshotV1* out, DataSemanticsErrorV1* error) override {
        if (fail_records) {
            *out = {};
            return Fail(error, "STORAGE_ERROR", "injected observation storage failure");
        }
        return service_->RecordUse(request, out, error);
    }

 private:
    IQuerier* base_;
    IOperatorDataContractV1* service_;
};

Json Call(IQuerier* querier, const char* operation, const Value& request) {
    Json response;
    DataSemanticsErrorV1 error;
    const auto rc = contracts::Control(querier, operation, request, &response, &error);
    if (rc) std::cerr << operation << ": " << error.code << " " << error.field_path << " " << error.message << '\n';
    assert(rc == 0);
    return response;
}
Json Describe(IQuerier* querier, const Value& locator) {
    Json request(rapidjson::kObjectType);
    auto& a = request.GetAllocator();
    Value target(rapidjson::kObjectType);
    Set(target, "kind", "channel", a);
    target.AddMember("locator", Copy(locator, a), a);
    request.AddMember("target", target, a);
    return Call(querier, "describe", request);
}
}  // namespace

int main() {
    char temporary[] = "/tmp/flowsql_npm_semantics_XXXXXX";
    assert(mkdtemp(temporary));
    const std::string root = temporary;
    auto* loader = PluginLoader::Single();
    Json persisted;
    std::string actual;
    for (int restart = 0; restart < 2; ++restart) {
        ExtensibleDatasetProvider metadata(loader);
        StaticContractCatalog static_catalog;
        const char* libraries[]{CONFIG_PLUGIN_PATH, DATABASE_PLUGIN_PATH, NPI_PLUGIN_PATH};
        const std::string config = "db_path=" + root + "/config.db";
        const std::string database_config = "config_file=" + root + "/database.yml";
        const char* options[]{config.c_str(), database_config.c_str(), "{\"ldfile\":\"" NPI_PROTOCOLS_PATH "\"}"};
        assert(loader->Load(".", libraries, options, 3) == 0);
        // The registry selects the last matching provider. Install the fixture
        // after the real provider so its extension type reaches this adapter.
        loader->Regist(IID_CHANNEL_DATASET_DESCRIPTOR_V1, &metadata);
        loader->Regist(IID_OPERATOR_CONTRACT_CATALOG_V1, &static_catalog);
        assert(loader->StartAll() == 0);
        struct Owner {
            PluginLoader* loader;
            ~Owner() {
                loader->StopAll();
                assert(loader->Unload() == 0);
            }
        } owner{loader};
        auto* databases = static_cast<IDatabaseFactory*>(loader->First(IID_DATABASE_FACTORY));
        assert(databases);
        if (!databases->Get("sqlite", "result"))
            assert(databases->AddChannel(("type=sqlite;name=result;path=" + root + "/result.db").c_str()) == 0);
        auto* database = databases->Get("sqlite", "result");
        assert(database);
        if (restart) {
            static_catalog.component = {
                "npm",     "basic", Hash("real npm metadata publication fixture"), "1", "operator_contract.npm.basic@1",
                "declared"};
            for (const auto& entity : persisted["entities"].GetArray())
                for (const auto& relation : entity["relations"].GetArray()) {
                    const auto response = Describe(loader, relation["locator"]);
                    assert(response["data"]["basis"]["dictionary_ref"] == relation["dictionary_ref"]);
                    assert(response["data"]["description"]["state"] == "present");
                }
            continue;
        }
        Json contract, declaration(rapidjson::kObjectType);
        contract.Parse(flowsql_operator_data_contract_v1(0));
        auto& ca = declaration.GetAllocator();
        Set(declaration, "kind", "operator_contract", ca);
        Set(declaration, "name", "npm.basic", ca);
        declaration.AddMember("expected_current_revision", uint64_t{0}, ca)
            .AddMember("document", Copy(contract, ca), ca);
        const auto declared = Call(loader, "save", declaration);
        OperatorComponentContractV1 component;
        component.category = "npm";
        component.name = "basic";
        component.operator_version = Get(contract["operator"], "version");
        component.component_revision = Hash("real npm metadata publication fixture");
        component.contract_ref = Get(declared["data"], "exact_reference");
        component.contract_state = "declared";
        DataSemanticsErrorV1 error;
        // Author definitions and existing managed objects are sufficient: no
        // operator task, packet processing, or publication may be required.
        static_catalog.component = component;
        assert(databases->AddChannel(("type=sqlite;name=static;path=" + root + "/static.db").c_str()) == 0);
        auto* static_database = databases->Get("sqlite", "static");
        std::vector<npm::NpmEntityDescriptorV1> static_entities;
        for (auto kind :
             {npm::NpmFamilyEntityKind::kBasic, npm::NpmFamilyEntityKind::kSession, npm::NpmFamilyEntityKind::kDns,
              npm::NpmFamilyEntityKind::kHttp1, npm::NpmFamilyEntityKind::kTls, npm::NpmFamilyEntityKind::kIcmp})
            for (auto family : {packet::AddressFamily::kIPv4, packet::AddressFamily::kIPv6})
                static_entities.push_back(npm::NpmFamilyEntityDescriptor(kind, family));
        auto static_factory = npm::MakeNpmDatabaseResultConsumerFactory(static_database, "pcapfile.static");
        std::unique_ptr<npm::INpmManagedResultConsumerV1> static_consumer;
        auto static_budget = std::make_shared<npm::NpmTaskBudget>(npm::NpmAnalysisConfig{});
        assert(static_factory->Create({"existing-fixture", "existing-run"}, static_entities, static_budget,
                                      &static_consumer) == 0);
        assert(static_consumer->Finish() == 0);
        const auto static_actual = static_consumer->ResultJson();
        static_consumer.reset();
        Json empty(rapidjson::kObjectType);
        const auto metadata_before = Call(loader, "list", empty);
        size_t static_count = 0;
        Json static_locator;
        static_locator.Parse(R"({"kind":"database_table","type":"sqlite","name":"static","table":"npm_result_runs"})");
        for (const auto& entity : static_entities)
            for (const char* view : {"history", "latest", "final", "data"}) {
                const auto suffix =
                    entity.entity_id + (std::string(view) == "data" ? "_v1_data" : "_" + std::string(view) + "_v1");
                Set(static_locator, "table", "npm_" + suffix, static_locator.GetAllocator());
                const auto value = Describe(loader, static_locator);
                const auto& data = value["data"];
                assert(data["description"]["state"] == "present");
                assert(data["description"]["origin"] == "producer_default");
                assert(data["basis"]["dictionary_ref"].IsNull() && data["basis"]["binding_ref"].IsNull());
                const auto& dictionary = data["description"]["document"];
                assert(dictionary["fields"].Size() == data["structure"]["fields"].Size());
                for (const auto& field : dictionary["fields"].GetArray()) {
                    assert(field["definition"]["state"] == "declared");
                    assert(field["type"]["system"] == "database");
                }
                ++static_count;
            }
        for (const char* table : {"npm_result_runs", "npm_result_entities"}) {
            Set(static_locator, "table", table, static_locator.GetAllocator());
            const auto value = Describe(loader, static_locator);
            assert(value["data"]["description"]["state"] == "present");
            ++static_count;
        }
        assert(static_count == 50 && Call(loader, "list", empty) == metadata_before);
        std::cout << "PASS: 50 existing empty NPM objects expose static defaults without executing a task or metadata "
                     "writes\n";
        Set(static_locator, "table", "npm_dns_transaction_ipv4_final_v1", static_locator.GetAllocator());
        Set(static_locator, "type", "extension_db", static_locator.GetAllocator());
        const auto extension_default = Describe(loader, static_locator);
        assert(extension_default["data"]["description"]["origin"] == "producer_default");
        Set(static_locator, "type", "sqlite", static_locator.GetAllocator());
        const auto original_default = Describe(loader, static_locator);
        Json updated_contract;
        updated_contract.CopyFrom(contract, updated_contract.GetAllocator());
        auto& updated_definition =
            updated_contract["resources"]["managed_dictionaries"]["dns_transaction_ipv4_final_v1"]["dictionary"]
                            ["fields"][0]["definition"];
        Set(updated_definition, "value", "新版运行身份说明", updated_contract.GetAllocator());
        declaration["expected_current_revision"].SetUint64(1);
        declaration["document"].CopyFrom(updated_contract, ca);
        const auto updated = Call(loader, "save", declaration);
        static_catalog.component.contract_ref = Get(updated["data"], "exact_reference");
        static_catalog.component.component_revision = Hash("updated npm metadata publication fixture");
        const auto updated_default = Describe(loader, static_locator);
        assert(Get(updated_default["data"]["basis"], "contract_ref") == static_catalog.component.contract_ref);
        assert(updated_default["data"]["description"]["document"]["fields"][0]["definition"]["value"] ==
               "新版运行身份说明");
        static_catalog.component = component;
        const auto restored_default = Describe(loader, static_locator);
        assert(Get(restored_default["data"]["basis"], "contract_ref") == component.contract_ref);
        assert(restored_default["data"]["description"]["document"] ==
               original_default["data"]["description"]["document"]);
        std::cout << "PASS: producer defaults follow exact contract revision changes without retaining stale content\n";
        const auto before_save = Describe(loader, static_locator);
        Json save_default(rapidjson::kObjectType), default_edits;
        auto& sa = save_default.GetAllocator();
        save_default.AddMember("locator", Copy(static_locator, sa), sa)
            .AddMember("basis", Copy(before_save["data"]["basis"], sa), sa)
            .AddMember("confirmations", Copy(before_save["data"]["checks"]["required_confirmations"], sa), sa);
        default_edits.Parse(R"({"fields":{"latency_ns":{"definition":"静态说明上的人工补充"}}})");
        save_default.AddMember("editor_changes", Copy(default_edits, sa), sa);
        Call(loader, "channels/save-description", save_default);
        const auto after_save = Describe(loader, static_locator);
        assert(after_save["data"]["description"]["origin"] == "bound_dictionary");
        const auto& saved_dictionary = after_save["data"]["description"]["document"];
        for (rapidjson::SizeType i = 0; i < saved_dictionary["fields"].Size(); ++i) {
            const auto& actual = saved_dictionary["fields"][i];
            const auto& previous = before_save["data"]["description"]["document"]["fields"][i];
            assert(actual["field_id"] == previous["field_id"]);
            assert(actual["time_semantics"] == previous["time_semantics"]);
            if (Get(actual, "name") == "latency_ns")
                assert(actual["definition"]["value"] == "静态说明上的人工补充");
            else
                assert(actual == previous);
        }
        std::string static_context;
        assert(npm::PublishNpmResultSemantics(loader, component, "sqlite", "static", "pcapfile.static", static_actual,
                                              &static_context, &error) == 0);
        const auto after_publication = Describe(loader, static_locator);
        for (const auto& field : after_publication["data"]["description"]["document"]["fields"].GetArray())
            if (Get(field, "name") == "latency_ns") assert(field["definition"]["value"] == "静态说明上的人工补充");
        assert(static_database->ExecuteSql("CREATE TABLE npm_unmanaged AS SELECT * FROM npm_basic_ipv4_final_v1") == 0);
        Set(static_locator, "table", "npm_unmanaged", static_locator.GetAllocator());
        const auto unrelated = Describe(loader, static_locator);
        assert(unrelated["data"]["description"]["state"] == "missing");
        assert(static_database->ExecuteSql("UPDATE npm_result_entities SET schema_canonical=schema_canonical || 'x' "
                                           "WHERE entity_id='basic_ipv4'") >= 0);
        Set(static_locator, "table", "npm_basic_ipv4_final_v1", static_locator.GetAllocator());
        Json invalid_request(rapidjson::kObjectType), invalid_target(rapidjson::kObjectType), rejected;
        auto& ia = invalid_request.GetAllocator();
        Set(invalid_target, "kind", "channel", ia);
        invalid_target.AddMember("locator", Copy(static_locator, ia), ia);
        invalid_request.AddMember("target", invalid_target, ia);
        assert(contracts::Control(loader, "describe", invalid_request, &rejected, &error) != 0);
        assert(error.code == "SOURCE_CHANGED");
        assert(
            static_database->ExecuteSql(
                "UPDATE npm_result_entities SET schema_canonical=substr(schema_canonical,1,length(schema_canonical)-1) "
                "WHERE entity_id='basic_ipv4'") >= 0);
        {
            auto factory = npm::MakeNpmDatabaseResultConsumerFactory(database, "pcapfile.fixture");
            std::unique_ptr<npm::INpmManagedResultConsumerV1> consumer;
            const std::vector<npm::NpmEntityDescriptorV1> entities{
                npm::NpmFamilyEntityDescriptor(npm::NpmFamilyEntityKind::kDns, packet::AddressFamily::kIPv4),
                npm::NpmFamilyEntityDescriptor(npm::NpmFamilyEntityKind::kDns, packet::AddressFamily::kIPv6)};
            auto budget = std::make_shared<npm::NpmTaskBudget>(npm::NpmAnalysisConfig{});
            assert(factory->Create({"task-fixture", "run-fixture"}, entities, budget, &consumer) == 0);
            assert(consumer->Finish() == 0);
            actual = consumer->ResultJson();
        }
        std::string context;
        auto publish = [&](const OperatorComponentContractV1& selected) {
            const auto rc = npm::PublishNpmResultSemantics(loader, selected, "sqlite", "result", "pcapfile.fixture",
                                                           actual, &context, &error);
            if (rc) std::cerr << error.code << " " << error.field_path << " " << error.message << '\n';
            return rc;
        };
        assert(publish(component) == 0);
        std::string extended;
        const auto extended_rc = npm::PublishNpmResultSemantics(loader, component, "extension_db", "result",
                                                                "pcapfile.fixture", actual, &extended, &error);
        if (extended_rc) std::cerr << error.code << " " << error.message << '\n';
        assert(extended_rc == 0);
        Json extension;
        extension.Parse(extended.c_str());
        for (const auto& entity : extension["entities"].GetArray())
            for (const auto& relation : entity["relations"].GetArray()) {
                assert(relation["locator"]["type"] == "extension_db");
                const auto described = Describe(loader, relation["locator"]);
                assert(described["data"]["description"]["state"] == "present");
                assert(described["data"]["basis"]["dictionary_ref"] == relation["dictionary_ref"]);
            }
        persisted.Parse(context.c_str());
        assert(persisted["entities"].Size() == 2 && persisted["run_id"] == "run-fixture");
        const auto first = context;
        assert(publish(component) == 0 && context == first);
        for (const auto& entity : persisted["entities"].GetArray()) {
            assert(entity["relations"].Size() == 3 && entity["description_state"] == "published");
            for (const auto& relation : entity["relations"].GetArray()) {
                const auto response = Describe(loader, relation["locator"]);
                const auto& data = response["data"];
                assert(data["basis"]["dictionary_ref"] == relation["dictionary_ref"]);
                assert(data["basis"]["binding_ref"] == relation["binding_ref"]);
                assert(data["checks"]["state"] == "matched");
                const auto& dictionary = data["description"]["document"];
                assert(dictionary["fields"].Size() == 28);
                assert(dictionary["entity"]["identity_fields"]["value"][0] == "__npm_run_id");
                for (const auto& field : dictionary["fields"].GetArray()) {
                    assert(field["type"]["system"] == "database");
                    assert(field["type"]["parameters"]["dialect"] == "sqlite");
                    if (Get(field, "name") == "entity_instance_id") assert(field["type"]["name"] == "TEXT");
                }
            }
        }
        Json relation;
        relation.CopyFrom(persisted["entities"][0]["relations"][0], relation.GetAllocator());
        const auto description = Describe(loader, relation["locator"]);
        Json save(rapidjson::kObjectType);
        auto& a = save.GetAllocator();
        save.AddMember("locator", Copy(relation["locator"], a), a)
            .AddMember("basis", Copy(description["data"]["basis"], a), a)
            .AddMember("confirmations", Copy(description["data"]["checks"]["required_confirmations"], a), a);
        Json edits;
        edits.Parse(R"({"description":"人工补充用途","fields":{"latency_ns":{"definition":"人工核验的延迟解释"}}})");
        save.AddMember("editor_changes", Copy(edits, a), a);
        Call(loader, "channels/save-description", save);
        assert(publish(component) == 0);
        const auto changed = Describe(loader, relation["locator"]);
        assert(changed["data"]["description"]["document"]["description"]["value"] == "人工补充用途");
        for (const auto& field : changed["data"]["description"]["document"]["fields"].GetArray())
            if (Get(field, "name") == "latency_ns") assert(field["definition"]["value"] == "人工核验的延迟解释");
        persisted.Parse(context.c_str());
        const auto before_read = Call(loader, "list", Json(rapidjson::kObjectType));
        Describe(loader, relation["locator"]);
        const auto after_read = Call(loader, "list", Json(rapidjson::kObjectType));
        assert(before_read == after_read);
        std::set<std::string> task_runs;
        for (int scenario : {0, 1, 2, 3}) {
            ControlledServices services(loader);
            auto* capability =
                static_cast<IBlockTransformOperatorV2*>(flowsql_create_operator_capability(1, &services));
            assert(capability);
            const auto release = [](IBlockTransformOperatorV2* value) {
                flowsql_destroy_operator_capability(1, value);
            };
            std::unique_ptr<IBlockTransformOperatorV2, decltype(release)> provider(capability, release);
            const std::string parameters = R"({"observing":"basic_ipv4"})";
            const std::string plan = R"({"version":1,"root":null})";
            BlockTransformTaskConfigV2 config{kBlockTransformTaskConfigV2Size, kBlockTransformContractVersionV2,
                                              "real-task", parameters.c_str(), plan.c_str()};
            IBlockTransformTaskV2* created = nullptr;
            assert(provider->CreateTask(config, &created) == 0);
            const auto release_task = [&](IBlockTransformTaskV2* value) { provider->ReleaseTask(value); };
            std::unique_ptr<IBlockTransformTaskV2, decltype(release_task)> task(created, release_task);
            assert(dynamic_cast<IOperatorConfigAdapterV1*>(task.get()) && "task must freeze actual invocation facts");
            auto state = std::make_shared<contracts::BlockInvocation>();
            state->querier = &services;
            state->invocation.component = component;
            state->parameters.Parse(parameters.c_str());
            Set(state->context, "source", "pcapfile.fixture", state->context.GetAllocator());
            state->context.AddMember("managed_output", true, state->context.GetAllocator());
            auto* input = dynamic_cast<IBlockTransformInputSourceTaskV1*>(task.get());
            assert(input && input->BindInputSource("pcapfile.fixture") == 0);
            if (!database->IsOpened()) assert(database->Open() == 0);
            auto* sink = dynamic_cast<IBlockTransformManagedSinkTaskV1*>(task.get());
            assert(sink);
            BlockTransformManagedSinkBindingV1 binding{};
            binding.struct_size = sizeof(binding);
            binding.contract_version = kBlockTransformManagedSinkContractVersionV1;
            binding.sink_channel = database;
            binding.target = "sqlite.result";
            binding.category = "sqlite";
            binding.name = "result";
            binding.relation = "";
            assert(sink->BindManagedSink(binding) == 0);
            services.hide_control = scenario == 3;
            contracts::CheckedBlockTask checked(task.get(), dynamic_cast<IOperatorConfigAdapterV1*>(provider.get()),
                                                state);
            std::shared_ptr<arrow::Schema> output;
            const int open_rc = checked.Open(packet::PacketSchema(), &output);
            if (scenario == 3) {
                assert(open_rc != 0 && checked.LastError().find("UNAVAILABLE") != std::string::npos);
                continue;
            }
            if (open_rc) std::cerr << checked.LastError() << '\n';
            assert(open_rc == 0);
            const auto& context = state->invocation.resolved["execution_context"];
            assert(context["finite_source"].GetBool() && context.HasMember("npm"));
            assert(context["npm"]["input_namespace"] == "pcapfile.fixture");
            assert(state->invocation.resolved["normalized_config"]["core"]["run_mode"] == "offline");
            assert(context["npm"]["run_id"] != "run-fixture");
            assert(task_runs.insert(Get(context["npm"], "run_id")).second);
            Json changed_component;
            changed_component.Parse(
                R"({"operator":{"contract_ref":"operator_contract.npm.basic@1","component_revision":"wrong"},"execution_context":{}})");
            std::string rejected;
            DataSemanticsErrorV1 rejected_error;
            assert(dynamic_cast<IOperatorConfigAdapterV1*>(task.get())
                           ->ResolveInvocation(Encode(changed_component).c_str(), &rejected, &rejected_error) != 0 &&
                   rejected.empty() && rejected_error.code == "COMPONENT_CHANGED");
            std::vector<BlockTransformOutputV1> batches;
            if (scenario == 2) {
                checked.Cancel();
                Json cancelled;
                cancelled.Parse(sink->ManagedSinkResultJson().c_str());
                assert(cancelled["run_status"] == "incomplete");
                continue;
            }
            if (scenario == 1) {
                services.fail_records = true;
                assert(checked.Flush(&batches) != 0 && batches.empty());
                assert(state->invocation.check["data_state"] == "produced" &&
                       state->invocation.check["metadata_state"] == "failed");
                assert(state->error.code == "METADATA_PUBLICATION_FAILED");
                Json committed;
                committed.Parse(sink->ManagedSinkResultJson().c_str());
                assert(committed["run_status"] == "completed");
                continue;
            }
            assert(checked.Flush(&batches) == 0);
            assert(state->invocation.check["metadata_state"] == "persisted");
        }
        auto wrong = component;
        wrong.component_revision = Hash("different component");
        assert(publish(wrong) != 0 && context.empty());
        assert(error.code == "SOURCE_CHANGED");
        assert(npm::PublishNpmResultSemantics(nullptr, component, "sqlite", "result", "pcapfile.fixture", actual,
                                              &context, &error) != 0 &&
               context.empty());
        assert(error.code == "UNAVAILABLE");
    }
    std::cout << "PASS: real SQLite DNS descriptions, native fields, precise refs, idempotence, manual edits, "
                 "read-only describe, source failure and actual plugin restart\n";
}
