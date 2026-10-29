// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_SERVICES_BINADDON_BINADDON_OPERATOR_PROXY_H_
#define _FLOWSQL_SERVICES_BINADDON_BINADDON_OPERATOR_PROXY_H_

#include <framework/interfaces/ioperator.h>
#include <framework/interfaces/ioperator_data_contract.h>
#include <framework/interfaces/ioperator_output_observation.h>
#include <common/channel_dataset.hpp>

#include <memory>

#include "binaddon_host_plugin.h"

namespace flowsql {
namespace binaddon {

class BinAddonOperatorProxy : public IOperator,
                              public IOperatorContractIdentityV1,
                              public IOperatorConfigAdapterV1,
                              public IOperatorOutputObservationV1 {
 public:
    BinAddonOperatorProxy(IOperator* impl, const std::shared_ptr<BinAddonHostPlugin::LoadedPlugin>& plugin)
        : impl_(impl), plugin_(plugin) {
        if (plugin_) plugin_->active_count.fetch_add(1, std::memory_order_acq_rel);
    }

    ~BinAddonOperatorProxy() override {
        if (plugin_ && impl_ && plugin_->destroy_fn) {
            plugin_->destroy_fn(impl_);
            impl_ = nullptr;
        }
        if (plugin_) plugin_->active_count.fetch_sub(1, std::memory_order_acq_rel);
    }

    int OutputObservations(std::string* out) const override {
        if (auto* producer = dynamic_cast<IOperatorOutputObservationV1*>(impl_))
            return producer->OutputObservations(out);
        if (out) out->clear();
        return ENOTSUP;
    }
    OperatorComponentContractV1 ContractIdentity() const override {
        if (plugin_)
            for (const auto& c : plugin_->data_contracts)
                if (c.category == impl_->Category() && c.name == impl_->Name()) return c;
        return {};
    }
    std::string Category() override { return impl_ ? impl_->Category() : ""; }
    std::string Name() override { return impl_ ? impl_->Name() : ""; }
    std::string Description() override { return impl_ ? impl_->Description() : ""; }
    OperatorPosition Position() override { return impl_ ? impl_->Position() : OperatorPosition::DATA; }
    int Work(IChannel* in, IChannel* out) override { return impl_ ? impl_->Work(in, out) : -1; }
    int Work(Span<IChannel*> inputs, IChannel* out) override { return impl_ ? impl_->Work(inputs, out) : -1; }
    int Configure(const char* key, const char* value) override { return impl_ ? impl_->Configure(key, value) : -1; }
    std::string LastError() override { return impl_ ? impl_->LastError() : "binaddon proxy invalid"; }
    int ResolveInvocation(const char* request_json, std::string* result_json, DataSemanticsErrorV1* error) override {
        if (auto* adapter = dynamic_cast<IOperatorConfigAdapterV1*>(impl_))
            return adapter->ResolveInvocation(request_json, result_json, error);
        using namespace dataset;
        if (result_json) result_json->clear();
        if (error) *error = {};
        Json request;
        if (!result_json || !request_json || request.Parse(request_json).HasParseError() || !request.IsObject() ||
            !request.HasMember("parameters") || !request.HasMember("inputs"))
            return Fail(error, "CONTRACT_INVALID", "invalid invocation request");
        Json result(rapidjson::kObjectType);
        auto& a = result.GetAllocator();
        result.AddMember("effective_parameters", Copy(request["parameters"], a), a)
            .AddMember("inputs", Copy(request["inputs"], a), a)
            .AddMember("config_refs", Value(rapidjson::kArrayType), a);
        *result_json = Encode(result);
        return 0;
    }

 private:
    IOperator* impl_ = nullptr;
    std::shared_ptr<BinAddonHostPlugin::LoadedPlugin> plugin_;
};

}  // namespace binaddon
}  // namespace flowsql

#endif  // _FLOWSQL_SERVICES_BINADDON_BINADDON_OPERATOR_PROXY_H_
