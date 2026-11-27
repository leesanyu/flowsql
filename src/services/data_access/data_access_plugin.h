// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <common/iplugin.h>
#include <framework/interfaces/idatabase_exploration.h>
#include <framework/interfaces/idatabase_factory.h>
#include <framework/interfaces/idataframe_snapshot.h>
#include <httplib.h>
#include <sqlite3.h>
#include <common/channel_dataset.hpp>

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

namespace flowsql::access {
class AccessStore;
using Json = dataset::Json;
using Value = dataset::Value;
using Alloc = dataset::Alloc;

struct AccessError : std::runtime_error {
    std::string code;
    int status;
    AccessError(std::string code, std::string message, int status = 400)
        : std::runtime_error(std::move(message)), code(std::move(code)), status(status) {}
};
__attribute__((visibility("default"))) Json Parse(const std::string& text);
std::string Required(const Value& value, const char* key);
uint64_t Bound(const Value& value, const char* key, uint64_t fallback, uint64_t low, uint64_t high);
void Keys(const Value& value, std::initializer_list<const char*> allowed);

// Credentials are deployment-owned opaque tokens bound to this listener. No request
// field or forwarded identity header can select a principal or expand a grant.
struct Subject {
    std::string principal, token;
    std::shared_ptr<const Json> policy;
};
struct Source {
    std::string id, revision, kind, provider, name, locator, description;
    bool available = false;
    std::shared_ptr<const Json> grant;
};
struct Running {
    std::string id, owner, kind;
    std::shared_ptr<std::atomic_bool> cancelled = std::make_shared<std::atomic_bool>(false);
    std::atomic_bool finished{false}, timed_out{false};
    std::mutex mutex;
    std::condition_variable changed;
    std::shared_ptr<IDatabaseExplorationSession> session;
    std::thread thread;
};

class __attribute__((visibility("default"))) DataAccessPlugin : public IPlugin {
 public:
    DataAccessPlugin();
    ~DataAccessPlugin() override;
    int Option(const char*) override;
    int Load(IQuerier*) override;
    int Start() override;
    int Stop() override;
    int Unload() override;

 private:
    const Subject& Authenticate(const httplib::Request&) const;
    std::vector<Source> Sources(const Subject&);
    Source Describe(const Value& locator, const Value& grant);
    Source Resolve(const Subject&, const Value& reference);
    Json SourceJson(const Source&);
    Json Call(const Subject&, const std::string& tool, const Value& arguments);
    Json ExecuteCall(const Subject&, const std::string&, const Value&);
    Json SubmitOperation(const Subject&, const std::string&, const Value&);
    Json Operations(const Subject&);
    Json DescribeOperation(const Subject&, const Value&);
    Json OperationDescriptor(const Subject&, const Value&);
    Json PrepareAnalysis(const Subject&, const Value&);
    Json ComputeAnalysis(const Subject&, const Value&, const std::shared_ptr<Running>&);
    Json AnalysisPayload(const Subject&, const Value&, const Value&);
    Json ReconcileAnalysis(const Subject&, Json);
    Source AnalysisInput(const Subject&, const Value&);
    Json Compute(const Subject&, const Source&, const Value&, const std::shared_ptr<Running>&);
    void Run(const Subject&, Source, std::string, const std::shared_ptr<Running>&);
    Json Cancel(const Subject&, const std::string&);
    bool Permitted(const Source&, const std::string& operation) const;
    void Route(const httplib::Request&, httplib::Response&);

    IQuerier* querier_ = nullptr;
    std::string host_ = "127.0.0.1", state_path_, contract_path_;
    int port_ = 18805;
    Json contract_, policy_;
    std::vector<Subject> subjects_;
    httplib::Server server_;
    std::thread listener_;
    std::unique_ptr<AccessStore> store_;
    std::mutex tasks_mutex_;
    std::vector<std::shared_ptr<Running>> tasks_;
};
}  // namespace flowsql::access
