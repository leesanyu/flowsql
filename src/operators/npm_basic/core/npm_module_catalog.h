// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef FLOWSQL_NPM_MODULE_CATALOG_H_
#define FLOWSQL_NPM_MODULE_CATALOG_H_

#include <operators/npm_basic/npm_protocol_contract.h>
#include <operators/npm_basic/npm_tcp_stream_contract.h>

#include <atomic>
#include <functional>

namespace flowsql::npm {

struct NpmBasicTaskConfig;
class NpmProtocolContext;
class NpmResultRouter;

struct NpmModuleInstance {
    std::unique_ptr<INpmAnalysisModule> analysis;
    std::unique_ptr<INpmProtocolModule> protocol;
    // Optional, non-owning interface of the same protocol object; this factory struct is internal, not plugin ABI.
    INpmTcpStreamConsumer* tcp_stream_consumer = nullptr;
};

/** Checks instance shape, declared stream consumption and interface ownership before runtime publication. */
NpmProtocolContractStatus ValidateNpmModuleInstance(const NpmModulePlan& plan, const NpmModuleInstance& instance);

struct NpmPreparedModule {
    NpmModulePlan plan;
    std::function<NpmModuleInstance(NpmProtocolContext&, std::shared_ptr<INpmTaskBudget>)> create;
};

struct NpmModuleRegistration {
    std::string module_id;
    bool available = true;
    std::vector<std::string> entity_ids;
    // json is borrowed only during prepare; returned plans and factory captures must own their configuration.
    std::function<NpmProtocolContractStatus(const NpmBasicTaskConfig&, std::string_view json, NpmPreparedModule*)>
        prepare;
};

using NpmModuleCatalog = std::vector<NpmModuleRegistration>;
const NpmModuleCatalog& ProductionNpmModuleCatalog();
NpmProtocolContractStatus ValidateNpmModuleCatalog(const NpmModuleCatalog& catalog);
NpmProtocolContractStatus PrepareNpmModules(const NpmBasicTaskConfig& config, const NpmModuleCatalog& catalog,
                                            std::vector<NpmPreparedModule>* output);

/** Adapts subscriptions to the existing unique session processing chain. */
class NpmProtocolModuleAdapter final : public INpmAnalysisModule, private INpmResultEmitter {
 public:
    NpmProtocolModuleAdapter(NpmModulePlan plan, std::unique_ptr<INpmProtocolModule> module,
                             const std::atomic<bool>* cancellation_requested, NpmResultRouter* router);
    ~NpmProtocolModuleAdapter() override;
    bool AcceptsControl() const;
    int OnControl(const NpmInputEvent& event);
    std::optional<int64_t> NextEventDeadlineNs() const;
    int OnTime(const NpmModuleTime& time);
    int Finish(int64_t observed_at_ns);
    void Abort() noexcept;
    const NpmModulePlan& Plan() const noexcept { return plan_; }
    INpmResultEmitter* StreamEmitter() noexcept { return this; }
    int OnPacket(const NpmPacketView&, const NpmSessionView&, INpmResultWriter&) override;
    int OnSessionSnapshot(const NpmSessionView&, int64_t, INpmResultWriter&) override;
    int OnSessionEnd(const NpmSessionView&, NpmSessionEndReason, int64_t, INpmResultWriter&) override;

 private:
    bool AcceptsSession(const NpmSessionView&) const;
    int Emit(std::string_view entity_id, const arrow::RecordBatch& rows) override;
    NpmResultRouter* router_;
    NpmModulePlan plan_;
    std::unique_ptr<INpmProtocolModule> module_;
    const std::atomic<bool>* cancellation_requested_ = nullptr;
    bool finished_ = false;
    bool aborted_ = false;
};

}  // namespace flowsql::npm
#endif  // FLOWSQL_NPM_MODULE_CATALOG_H_
