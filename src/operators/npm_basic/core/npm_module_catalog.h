// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef FLOWSQL_NPM_MODULE_CATALOG_H_
#define FLOWSQL_NPM_MODULE_CATALOG_H_

#include <operators/npm_basic/npm_protocol_contract.h>

#include <atomic>
#include <functional>

namespace flowsql::npm {

struct NpmBasicTaskConfig;
class NpmProtocolContext;
class NpmResultRouter;

struct NpmModuleInstanceV1 {
    std::unique_ptr<INpmAnalysisModule> analysis;
    std::unique_ptr<INpmProtocolModuleV1> protocol;
};

struct NpmPreparedModuleV1 {
    NpmModulePlanV1 plan;
    std::function<NpmModuleInstanceV1(NpmProtocolContext&, std::shared_ptr<INpmTaskBudget>)> create;
};

struct NpmModuleRegistrationV1 {
    std::string module_id;
    bool available = true;
    std::vector<std::string> entity_ids;
    // json is borrowed only during prepare; returned plans and factory captures must own their configuration.
    std::function<NpmProtocolContractStatusV1(const NpmBasicTaskConfig&, std::string_view json, NpmPreparedModuleV1*)>
        prepare;
};

using NpmModuleCatalogV1 = std::vector<NpmModuleRegistrationV1>;
const NpmModuleCatalogV1& ProductionNpmModuleCatalogV1();
NpmProtocolContractStatusV1 ValidateNpmModuleCatalogV1(const NpmModuleCatalogV1& catalog);
NpmProtocolContractStatusV1 PrepareNpmModulesV1(const NpmBasicTaskConfig& config, const NpmModuleCatalogV1& catalog,
                                                std::vector<NpmPreparedModuleV1>* output);

/** Adapts subscriptions to the existing unique session processing chain. */
class NpmProtocolModuleAdapter final : public INpmAnalysisModule, private INpmResultEmitterV1 {
 public:
    NpmProtocolModuleAdapter(NpmModulePlanV1 plan, std::unique_ptr<INpmProtocolModuleV1> module,
                             const std::atomic<bool>* cancellation_requested, NpmResultRouter* router);
    ~NpmProtocolModuleAdapter() override;
    bool AcceptsControl() const;
    int OnControl(const NpmInputEventV1& event);
    std::optional<int64_t> NextEventDeadlineNs() const;
    int OnTime(const NpmModuleTimeV1& time);
    int Finish(int64_t observed_at_ns);
    void Abort() noexcept;
    int OnPacket(const NpmPacketView&, const NpmSessionView&, INpmResultWriter&) override;
    int OnSessionSnapshot(const NpmSessionView&, int64_t, INpmResultWriter&) override;
    int OnSessionEnd(const NpmSessionView&, NpmSessionEndReason, int64_t, INpmResultWriter&) override;

 private:
    bool AcceptsSession(const NpmSessionView&) const;
    int Emit(std::string_view entity_id, const arrow::RecordBatch& rows) override;
    NpmResultRouter* router_;
    NpmModulePlanV1 plan_;
    std::unique_ptr<INpmProtocolModuleV1> module_;
    const std::atomic<bool>* cancellation_requested_ = nullptr;
    bool finished_ = false;
    bool aborted_ = false;
};

}  // namespace flowsql::npm
#endif  // FLOWSQL_NPM_MODULE_CATALOG_H_
