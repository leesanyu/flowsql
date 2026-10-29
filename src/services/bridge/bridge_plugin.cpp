// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include "bridge_plugin.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <utility>

#include <httplib.h>
#include <rapidjson/document.h>

#include <common/log.h>
#include <common/channel_dataset.hpp>
#include "framework/interfaces/ioperator_catalog.h"

namespace flowsql {
namespace bridge {

namespace {

std::string MakeOperatorKey(const std::string& category, const std::string& name) { return category + "." + name; }

}  // namespace

int BridgePlugin::Option(const char* arg) {
    if (!arg) return 0;

    std::string opts(arg);
    size_t pos = 0;
    while (pos < opts.size()) {
        size_t eq = opts.find('=', pos);
        if (eq == std::string::npos) break;
        size_t end = opts.find(';', eq);
        if (end == std::string::npos) end = opts.size();

        std::string key = opts.substr(pos, eq - pos);
        std::string val = opts.substr(eq + 1, end - eq - 1);

        if (key == "worker_host")
            host_ = val;
        else if (key == "worker_port")
            port_ = std::stoi(val);

        pos = (end < opts.size()) ? end + 1 : opts.size();
    }
    return 0;
}

int BridgePlugin::Load(IQuerier* querier) {
    querier_ = querier;

    // 从环境变量获取 Gateway 地址，通过 Gateway 路由发现 PyWorker
    const char* gw = std::getenv("FLOWSQL_GATEWAY_ADDR");
    if (gw) {
        gateway_addr_ = gw;
        std::string gw_host = "127.0.0.1";
        int gw_port = 18800;
        size_t colon = gateway_addr_.find(':');
        if (colon != std::string::npos) {
            gw_host = gateway_addr_.substr(0, colon);
            gw_port = std::stoi(gateway_addr_.substr(colon + 1));
        }

        // 重试查询路由表（PyWorker 可能还没注册）
        for (int retry = 0; retry < 10; ++retry) {
            httplib::Client client(gw_host, gw_port);
            client.set_connection_timeout(2);
            client.set_read_timeout(2);
            auto res = client.Get("/gateway/routes");
            if (res && res->status == 200) {
                rapidjson::Document doc;
                doc.Parse(res->body.c_str());
                if (!doc.HasParseError() && doc.IsArray()) {
                    for (auto& item : doc.GetArray()) {
                        std::string prefix = item["prefix"].GetString();
                        if (prefix.find("/operators/python") == 0) {
                            std::string addr = item["address"].GetString();
                            size_t c = addr.find(':');
                            if (c != std::string::npos) {
                                host_ = addr.substr(0, c);
                                port_ = std::stoi(addr.substr(c + 1));
                            }
                            LOG_INFO("BridgePlugin::Load: discovered PyWorker at %s:%d via Gateway", host_.c_str(),
                                     port_);
                            goto found;
                        }
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
found:
    LOG_INFO("BridgePlugin::Load: worker=%s:%d, gateway=%s", host_.c_str(), port_,
             gateway_addr_.empty() ? "(none)" : gateway_addr_.c_str());
    return 0;
}

int BridgePlugin::Unload() { return 0; }

int BridgePlugin::Start() {
    for (int retry = 0; retry < 30; ++retry) {
        if (DiscoverOperators() == 0) {
            LOG_INFO("BridgePlugin::Start: %zu Python operators registered", OperatorCount());
            return 0;
        }
        LOG_INFO("BridgePlugin::Start: waiting for Python Worker... (%d/30)", retry + 1);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    LOG_WARN("BridgePlugin::Start: Python Worker not available, continuing without Python operators");
    return 0;
}

int BridgePlugin::Stop() {
    std::unique_lock<std::shared_mutex> lk(operators_mu_);
    registered_operators_.clear();
    return 0;
}

int BridgePlugin::WorkerControl(const std::string& operation, const rapidjson::Value& request,
                                rapidjson::Document* response, DataSemanticsErrorV1* e) {
    httplib::Client client(host_, port_);
    client.set_connection_timeout(2);
    client.set_read_timeout(10);
    auto result = client.Post("/operators/python/source/" + operation, dataset::Encode(request), "application/json");
    if (!result) return dataset::Fail(e, "UNAVAILABLE", "Python Worker unavailable");
    if (result->body.size() > 12 * 1024 * 1024 || !JsonNestingWithin(result->body, 64))
        return dataset::Fail(e, "LIMIT_EXCEEDED", "Worker response exceeds control limits");
    response->Parse(result->body.c_str());
    if (response->HasParseError() || !response->IsObject())
        return dataset::Fail(e, "UNAVAILABLE", "invalid Worker response");
    if (result->status != 200) {
        if (response->HasMember("error") && (*response)["error"].IsObject())
            return dataset::Fail(e, dataset::Get((*response)["error"], "code", "CONTRACT_INVALID"),
                                 dataset::Get((*response)["error"], "message"));
        return dataset::Fail(e, "UNAVAILABLE", "Worker publication failed");
    }
    return 0;
}

int BridgePlugin::DiscoverOperators() {
    std::lock_guard<std::mutex> publication(discovery_mu_);
    httplib::Client client(host_, port_);
    client.set_connection_timeout(2);
    client.set_read_timeout(5);
    auto response = client.Get("/operators/python/list");
    if (!response || response->status != 200 || response->body.size() > 12 * 1024 * 1024 ||
        !JsonNestingWithin(response->body, 64))
        return -1;
    rapidjson::Document doc;
    doc.Parse(response->body.c_str());
    if (doc.HasParseError() || !doc.IsArray()) return -1;
    DataSemanticsErrorV1 error;
    const int rc = ApplyOperators(doc, "", &error);
    if (rc) LOG_ERROR("BridgePlugin: registration failed: %s %s", error.code.c_str(), error.message.c_str());
    return rc;
}

int BridgePlugin::ApplyOperators(const rapidjson::Value& items, const std::string& token, DataSemanticsErrorV1* e) {
    if (!items.IsArray() || items.Size() > 256)
        return dataset::Fail(e, "CONTRACT_INVALID", "Worker list must contain at most 256 operators");
    auto* catalog = querier_
                        ? static_cast<IOperatorContractCatalogV1*>(querier_->First(IID_OPERATOR_CONTRACT_CATALOG_V1))
                        : nullptr;
    std::vector<OperatorContractDeclarationV1> declarations;
    std::vector<OperatorMeta> metadata;
    std::vector<flowsql::OperatorMeta> rows;
    std::set<std::string> keys;
    for (const auto& item : items.GetArray()) {
        if (!item.IsObject()) return dataset::Fail(e, "CONTRACT_INVALID", "invalid Worker item");
        OperatorMeta meta;
        meta.category = dataset::Get(item, "category");
        meta.name = dataset::Get(item, "name");
        meta.description = dataset::Get(item, "description");
        if (meta.category.empty() || meta.name.empty()) return dataset::Fail(e, "CONTRACT_INVALID", "missing identity");
        if (!keys.insert(MakeOperatorKey(meta.category, meta.name)).second) continue;
        meta.position =
            dataset::Get(item, "position") == "STORAGE" ? OperatorPosition::STORAGE : OperatorPosition::DATA;
        const auto digest = dataset::Get(item, "component_revision");
        std::string declaration;
        if (item.HasMember("data_contract") && !item["data_contract"].IsNull())
            declaration = dataset::Encode(item["data_contract"]);
        meta.contract = {meta.category, meta.name, digest, "", "", declaration.empty() ? "missing" : "unavailable"};
        if (!declaration.empty() && !catalog)
            return dataset::Fail(e, "UNAVAILABLE", "declared contract catalog unavailable");
        if (catalog && !digest.empty()) {
            OperatorComponentContractV1 previous;
            if (catalog->Lookup(meta.category.c_str(), meta.name.c_str(), &previous, e)) return -1;
            declarations.push_back({meta.category, meta.name, digest, declaration, previous.component_revision});
        } else if (!declaration.empty())
            return dataset::Fail(e, "CONTRACT_INVALID", "declared component lacks source digest");
        rows.push_back({meta.category, meta.name, "python", "bridge", meta.description,
                        meta.position == OperatorPosition::STORAGE ? "storage" : "data"});
        metadata.push_back(std::move(meta));
    }
    std::vector<PreparedOperatorContractV1> prepared;
    if (!declarations.empty() && catalog->Prepare(declarations, &prepared, e)) return -1;
    for (auto& meta : metadata)
        for (const auto& p : prepared)
            if (p.association.category == meta.category && p.association.name == meta.name)
                meta.contract = p.association;
    BridgeOperatorMap discovered;
    for (const auto& meta : metadata)
        discovered.emplace(MakeOperatorKey(meta.category, meta.name),
                           std::make_shared<PythonOperatorBridge>(meta, host_, port_));
    if (SyncOperatorsToCatalog(rows)) return dataset::Fail(e, "UNAVAILABLE", "operator catalog update failed");
    dataset::Json request(rapidjson::kObjectType), response;
    dataset::Set(request, "token", token, request.GetAllocator());
    if (!token.empty() && WorkerControl("activate", request, &response, e)) return -1;
    if (!prepared.empty() && catalog->Commit(prepared, e)) {
        if (!token.empty()) {
            DataSemanticsErrorV1 rollback;
            if (WorkerControl("rollback", request, &response, &rollback))
                return dataset::Fail(e, "UNAVAILABLE",
                                     "association failed and Worker rollback failed; refresh required");
        }
        return -1;
    }
    {
        std::unique_lock<std::shared_mutex> lock(operators_mu_);
        registered_operators_.swap(discovered);
    }
    if (!token.empty()) {
        DataSemanticsErrorV1 finalize;
        (void)WorkerControl("finalize", request, &response, &finalize);
    }
    return 0;
}

int BridgePlugin::ReadPythonSource(const char* category, const char* name, std::string* output,
                                   DataSemanticsErrorV1* e) {
    if (output) output->clear();
    if (e) *e = {};
    if (!category || !name || !output) return dataset::Fail(e, "INVALID_REQUEST", "identity and output required");
    dataset::Json request(rapidjson::kObjectType), response;
    dataset::Set(request, "category", category, request.GetAllocator());
    dataset::Set(request, "name", name, request.GetAllocator());
    if (WorkerControl("read", request, &response, e)) return -1;
    *output = dataset::Encode(response);
    return 0;
}

int BridgePlugin::PublishPythonSource(const char* filename, const char* source, const char* expected,
                                      std::string* response_json, DataSemanticsErrorV1* e) {
    if (response_json) response_json->clear();
    if (e) *e = {};
    if (!filename || !source || !response_json)
        return dataset::Fail(e, "INVALID_REQUEST", "source and output required");
    std::lock_guard<std::mutex> publication(discovery_mu_);
    dataset::Json request(rapidjson::kObjectType), response;
    auto& a = request.GetAllocator();
    dataset::Set(request, "filename", filename, a);
    dataset::Set(request, "source", source, a);
    dataset::Set(request, "expected_component_revision", expected ? expected : "", a);
    if (WorkerControl("stage", request, &response, e)) return -1;
    const auto token = dataset::Get(response, "token");
    if (token.empty() || !response.HasMember("operators"))
        return dataset::Fail(e, "UNAVAILABLE", "invalid Worker candidate");
    if (ApplyOperators(response["operators"], token, e)) {
        dataset::Json rollback(rapidjson::kObjectType), ignored;
        dataset::Set(rollback, "token", token, rollback.GetAllocator());
        DataSemanticsErrorV1 rollback_error;
        (void)WorkerControl("rollback", rollback, &ignored, &rollback_error);
        return -1;
    }
    dataset::Json result(rapidjson::kObjectType);
    auto& alloc = result.GetAllocator();
    result.AddMember("ok", true, alloc);
    dataset::Set(result, "type", "python", alloc);
    dataset::Set(result, "filename", filename, alloc);
    dataset::Set(result, "association_state", "associated", alloc);
    result.AddMember("operators", rapidjson::Value(response["operators"], alloc), alloc);
    *response_json = dataset::Encode(result);
    return 0;
}

int BridgePlugin::SyncOperatorsToCatalog(const std::vector<flowsql::OperatorMeta>& ops) {
    if (querier_ == nullptr) return 0;

    auto* catalog = static_cast<IOperatorCatalog*>(querier_->First(IID_OPERATOR_CATALOG));
    if (catalog == nullptr) {
        LOG_WARN("BridgePlugin: IOperatorCatalog not found, skip sync");
        return 0;
    }

    UpsertResult result = catalog->UpsertBatch(ops);
    if (result.failed_count > 0) {
        LOG_ERROR("BridgePlugin: UpsertBatch failed, success=%d failed=%d err=%s", result.success_count,
                  result.failed_count, result.error_message.c_str());
        return -1;
    }
    LOG_INFO("BridgePlugin: synced %d operators to Catalog", result.success_count);
    return 0;
}

// IBridge::FindOperator — 按 category + name 查找 Python 算子
std::shared_ptr<IOperator> BridgePlugin::FindOperator(const std::string& category, const std::string& name) {
    std::shared_lock<std::shared_mutex> lk(operators_mu_);
    auto it = registered_operators_.find(MakeOperatorKey(category, name));
    if (it != registered_operators_.end()) {
        return it->second->NewInvocation();
    }
    return nullptr;
}

// IBridge::TraverseOperators — 遍历所有已发现的 Python 算子
void BridgePlugin::TraverseOperators(std::function<int(IOperator*)> fn) {
    std::vector<std::shared_ptr<PythonOperatorBridge>> snapshot;
    {
        std::shared_lock<std::shared_mutex> lk(operators_mu_);
        snapshot.reserve(registered_operators_.size());
        for (const auto& kv : registered_operators_) {
            snapshot.push_back(kv.second);
        }
    }

    for (const auto& op : snapshot) {
        if (fn(op.get()) == -1) break;
    }
}

// IBridge::Refresh — 重新从 Python Worker 发现算子
int BridgePlugin::Refresh() { return DiscoverOperators(); }

size_t BridgePlugin::OperatorCount() const {
    std::shared_lock<std::shared_mutex> lk(operators_mu_);
    return registered_operators_.size();
}

}  // namespace bridge
}  // namespace flowsql
