// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <framework/interfaces/cpp_operator_plugin_abi.h>
#include <framework/interfaces/iblock_transform_operator.h>

#include <cerrno>
#include <memory>

#include "npm_basic_operator.h"

extern "C" int flowsql_abi_version() { return flowsql::kCppOperatorPluginAbiVersion; }

extern "C" int flowsql_operator_count() { return 1; }

extern "C" int flowsql_describe_operator(int index, flowsql::CppOperatorDescriptor* descriptor) {
    try {
        if (index != 0 || descriptor == nullptr || descriptor->struct_size < flowsql::kCppOperatorDescriptorSize) {
            return EINVAL;
        }

        flowsql::CppOperatorDescriptor result{};
        result.struct_size = flowsql::kCppOperatorDescriptorSize;
        result.category = "npm";
        result.name = "basic";
        result.description = "NPM basic session analysis";
        result.contract_iid = flowsql::IID_BLOCK_TRANSFORM_OPERATOR;
        *descriptor = result;
        return 0;
    } catch (...) {
        return EFAULT;
    }
}

extern "C" void* flowsql_create_operator_capability(int index, flowsql::IQuerier* querier) {
    if (index != 0 || querier == nullptr) return nullptr;

    try {
        auto instance = std::make_unique<flowsql::npm::NpmBasicOperator>();
        if (instance->Load(querier) != 0) return nullptr;
        if (instance->Start() != 0) {
            instance->Unload();
            return nullptr;
        }
        auto* provider = instance.release();
        return static_cast<void*>(static_cast<flowsql::IBlockTransformOperator*>(provider));
    } catch (...) {
        return nullptr;
    }
}

extern "C" void flowsql_destroy_operator_capability(int index, void* capability) {
    if (index != 0 || capability == nullptr) return;

    try {
        auto* instance =
            static_cast<flowsql::npm::NpmBasicOperator*>(static_cast<flowsql::IBlockTransformOperator*>(capability));
        instance->Stop();
        instance->Unload();
        delete instance;
    } catch (...) {
        std::terminate();
    }
}
