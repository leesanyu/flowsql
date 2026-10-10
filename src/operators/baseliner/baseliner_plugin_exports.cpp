// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <framework/interfaces/cpp_operator_plugin_abi.h>
#include <cerrno>
#include "baseliner_operator.h"

extern "C" int flowsql_abi_version() { return flowsql::kCppOperatorPluginAbiVersionV2; }
extern "C" int flowsql_operator_count() { return 1; }
extern "C" int flowsql_describe_operator(int index, flowsql::CppOperatorDescriptorV2* output) {
    if (index != 0 || !output || output->struct_size < flowsql::kCppOperatorDescriptorV2Size) return EINVAL;
    *output = {flowsql::kCppOperatorDescriptorV2Size, "explore", "baseliner",
               "Configured baseline evaluation and forecast", flowsql::IID_BLOCK_TRANSFORM_OPERATOR_V2};
    return 0;
}
extern "C" void* flowsql_create_operator_capability(int index, flowsql::IQuerier* querier) {
    if (index != 0 || !querier) return nullptr;
    try {
        auto provider = std::make_unique<flowsql::baseliner::BaselinerOperator>();
        if (provider->Load(querier) != 0 || provider->Start() != 0) return nullptr;
        return static_cast<flowsql::IBlockTransformOperatorV2*>(provider.release());
    } catch (...) {
        return nullptr;
    }
}
extern "C" void flowsql_destroy_operator_capability(int index, void* capability) {
    if (index != 0 || !capability) return;
    auto* provider = static_cast<flowsql::IBlockTransformOperatorV2*>(capability);
    try {
        delete provider;
    } catch (...) {
    }
}
BEGIN_PLUGIN_REGIST(flowsql::baseliner::BaselinerOperator)
____INTERFACE(flowsql::IID_PLUGIN, flowsql::IPlugin)
____INTERFACE(flowsql::IID_BLOCK_TRANSFORM_OPERATOR_V2, flowsql::IBlockTransformOperatorV2)
____INTERFACE(flowsql::IID_BLOCK_TRANSFORM_EXECUTION_POLICY_PROVIDER_V1,
              flowsql::IBlockTransformExecutionPolicyProviderV1)
END_PLUGIN_REGIST()
