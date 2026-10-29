// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include "builtin_registry_plugin.h"

#include <common/operator_component.hpp>
#include "builtin_registry.h"
#include "operator_contract_package.h"

#include <algorithm>
#include <cctype>

namespace flowsql {
namespace builtin {

namespace {

const char component_anchor = 0;
class ContractOperator final : public IOperator, public IOperatorContractIdentityV1 {
 public:
    ContractOperator(IOperator* op, OperatorComponentContractV1 identity) : op_(op), identity_(std::move(identity)) {}
    OperatorComponentContractV1 ContractIdentity() const override { return identity_; }
    std::string Category() override { return op_->Category(); }
    std::string Name() override { return op_->Name(); }
    std::string Description() override { return op_->Description(); }
    OperatorPosition Position() override { return op_->Position(); }
    int Configure(const char* k, const char* v) override { return op_->Configure(k, v); }
    int Work(IChannel* in, IChannel* out) override { return op_->Work(in, out); }
    int Work(Span<IChannel*> in, IChannel* out) override { return op_->Work(in, out); }
    std::string LastError() override { return op_->LastError(); }

 private:
    std::unique_ptr<IOperator> op_;
    const OperatorComponentContractV1 identity_;
};
std::string ToLowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

}  // namespace

int BuiltinRegistryPlugin::Load(IQuerier* querier) {
    querier_ = querier;
    EnsureBuiltinRegistryInitialized();
    return 0;
}

int BuiltinRegistryPlugin::Start() {
    DataSemanticsErrorV1 error;
    // Reduced legacy deployments without Config stay loadable; identity remains
    // unavailable and a covered execution must report the missing declared contract.
    contracts::RegisterComponent(querier_, "builtin", "passthrough", contracts::ComponentDigest(&component_anchor),
                                 contracts::kPassthroughContract, &passthrough_contract_, &error);
    return 0;
}
int BuiltinRegistryPlugin::Unload() {
    querier_ = nullptr;
    return 0;
}

int BuiltinRegistryPlugin::FindStreamChannelType(const std::string& type, StreamChannelTypeDescriptor* out) {
    if (!out) return -1;
    return BuiltinRegistry::Instance().FindStreamChannelType(type, out) ? 0 : -1;
}

void BuiltinRegistryPlugin::ListStreamChannelTypes(std::function<void(const StreamChannelTypeDescriptor&)> callback) {
    if (!callback) return;
    const auto items = BuiltinRegistry::Instance().ListStreamChannelTypes();
    for (const auto& item : items) {
        callback(item);
    }
}

void BuiltinRegistryPlugin::ListBuiltinOperators(std::function<void(const BuiltinOperatorDescriptor&)> callback) {
    if (!callback) return;
    const auto items = BuiltinRegistry::Instance().ListBuiltinOperators();
    for (const auto& item : items) {
        callback(item);
    }
}

IOperator* BuiltinRegistryPlugin::CreateBuiltinOperator(const std::string& category, const std::string& name) {
    auto items = BuiltinRegistry::Instance().ListBuiltinOperators();
    const std::string target = category + "." + name;
    const std::string target_lower = ToLowerAscii(target);
    for (const auto& item : items) {
        if (ToLowerAscii(item.category + "." + item.name) == target_lower) {
            auto* op = item.factory ? item.factory() : nullptr;
            if (op && item.category == "builtin" && item.name == "passthrough")
                return new ContractOperator(op, passthrough_contract_);
            return op;
        }
    }
    return nullptr;
}

}  // namespace builtin
}  // namespace flowsql
