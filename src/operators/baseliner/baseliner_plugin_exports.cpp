// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <framework/interfaces/cpp_operator_plugin_abi.h>
#include <cerrno>
#include "baseliner_operator.h"
#include "operator_contract_package.h"

extern "C" const char* flowsql_operator_data_contract(int index) {
    return index == 0 ? flowsql::contracts::kBaselinerContract : nullptr;
}
extern "C" int flowsql_abi_version() { return flowsql::kCppOperatorPluginAbiVersion; }
extern "C" int flowsql_operator_count() { return 1; }
extern "C" int flowsql_describe_operator(int index, flowsql::CppOperatorDescriptor* output) {
    if (index != 0 || !output || output->struct_size < flowsql::kCppOperatorDescriptorSize) return EINVAL;
    *output = {flowsql::kCppOperatorDescriptorSize, "explore", "baseliner",
               "Configured baseline evaluation and forecast", flowsql::IID_BLOCK_TRANSFORM_OPERATOR};
    return 0;
}
extern "C" void* flowsql_create_operator_capability(int index, flowsql::IQuerier* querier) {
    if (index != 0 || !querier) return nullptr;
    try {
        auto provider = std::make_unique<flowsql::baseliner::BaselinerOperator>();
        provider->SetHostManagedContract();
        if (provider->Load(querier) != 0 || provider->Start() != 0) return nullptr;
        return static_cast<flowsql::IBlockTransformOperator*>(provider.release());
    } catch (...) {
        return nullptr;
    }
}
extern "C" void flowsql_destroy_operator_capability(int index, void* capability) {
    if (index != 0 || !capability) return;
    auto* provider = static_cast<flowsql::IBlockTransformOperator*>(capability);
    try {
        delete provider;
    } catch (...) {
    }
}
BEGIN_PLUGIN_REGIST(flowsql::baseliner::BaselinerOperator)
____INTERFACE(flowsql::IID_PLUGIN, flowsql::IPlugin)
____INTERFACE(flowsql::IID_BLOCK_TRANSFORM_OPERATOR, flowsql::IBlockTransformOperator)
____INTERFACE(flowsql::IID_BLOCK_TRANSFORM_EXECUTION_POLICY_PROVIDER, flowsql::IBlockTransformExecutionPolicyProvider)
END_PLUGIN_REGIST()
