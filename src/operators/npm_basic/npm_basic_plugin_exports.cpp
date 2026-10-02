// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <framework/interfaces/cpp_operator_plugin_abi.h>
#include <framework/interfaces/iblock_transform_operator.h>

#include <cerrno>
#include <memory>

#include "npm_basic_operator.h"

namespace {

constexpr int kNpmBasicV1Index = 0;
constexpr int kNpmBasicV2Index = 1;

}  // namespace

extern "C" int flowsql_abi_version() { return flowsql::kCppOperatorPluginAbiVersionV2; }

extern "C" int flowsql_operator_count() { return 2; }

extern "C" int flowsql_describe_operator(int index, flowsql::CppOperatorDescriptorV2* descriptor) {
    try {
        if ((index != kNpmBasicV1Index && index != kNpmBasicV2Index) || descriptor == nullptr ||
            descriptor->struct_size < flowsql::kCppOperatorDescriptorV2Size) {
            return EINVAL;
        }

        flowsql::CppOperatorDescriptorV2 result{};
        result.struct_size = flowsql::kCppOperatorDescriptorV2Size;
        result.category = "npm";
        result.name = "basic";
        result.description = "NPM basic session analysis";
        result.contract_iid = index == kNpmBasicV1Index ? flowsql::IID_BLOCK_TRANSFORM_OPERATOR_V1
                                                        : flowsql::IID_BLOCK_TRANSFORM_OPERATOR_V2;
        *descriptor = result;
        return 0;
    } catch (...) {
        return EFAULT;
    }
}

extern "C" void* flowsql_create_operator_capability(int index, flowsql::IQuerier* querier) {
    if ((index != kNpmBasicV1Index && index != kNpmBasicV2Index) || querier == nullptr) return nullptr;

    try {
        auto instance = std::make_unique<flowsql::npm::NpmBasicOperator>();
        if (instance->Load(querier) != 0) return nullptr;
        if (instance->Start() != 0) {
            instance->Unload();
            return nullptr;
        }
        auto* provider = instance.release();
        return index == kNpmBasicV1Index
                   ? static_cast<void*>(static_cast<flowsql::IBlockTransformOperatorV1*>(provider))
                   : static_cast<void*>(static_cast<flowsql::IBlockTransformOperatorV2*>(provider));
    } catch (...) {
        return nullptr;
    }
}

extern "C" void flowsql_destroy_operator_capability(int index, void* capability) {
    if ((index != kNpmBasicV1Index && index != kNpmBasicV2Index) || capability == nullptr) return;

    try {
        auto* instance = index == kNpmBasicV1Index ? dynamic_cast<flowsql::npm::NpmBasicOperator*>(
                                                         static_cast<flowsql::IBlockTransformOperatorV1*>(capability))
                                                   : dynamic_cast<flowsql::npm::NpmBasicOperator*>(
                                                         static_cast<flowsql::IBlockTransformOperatorV2*>(capability));
        if (instance == nullptr) return;
        instance->Stop();
        instance->Unload();
        delete instance;
    } catch (...) {
    }
}
