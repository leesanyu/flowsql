// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_CPP_OPERATOR_PLUGIN_ABI_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_CPP_OPERATOR_PLUGIN_ABI_H_

#include <common/guid.h>
#include <framework/interfaces/ioperator.h>
#include <common/iquerier.hpp>

#include <cstdint>

namespace flowsql {

constexpr int kCppOperatorPluginAbiVersionV1 = 1;
constexpr int kCppOperatorPluginAbiVersionV2 = 2;

inline constexpr char kCppOperatorPluginAbiVersionSymbol[] = "flowsql_abi_version";
inline constexpr char kCppOperatorPluginCountSymbol[] = "flowsql_operator_count";
inline constexpr char kCppOperatorPluginCreateV1Symbol[] = "flowsql_create_operator";
inline constexpr char kCppOperatorPluginDestroyV1Symbol[] = "flowsql_destroy_operator";
inline constexpr char kCppOperatorPluginDescribeV2Symbol[] = "flowsql_describe_operator";
inline constexpr char kCppOperatorPluginCreateCapabilityV2Symbol[] = "flowsql_create_operator_capability";
inline constexpr char kCppOperatorPluginDestroyCapabilityV2Symbol[] = "flowsql_destroy_operator_capability";

/**
 * Describes one functionally named operator in the V2 plugin-wide index space.
 *
 * The caller zero-initializes the structure and sets struct_size to
 * kCppOperatorDescriptorV2Size. The plugin must reject a null descriptor, a
 * smaller structure, or an out-of-range index. String storage remains owned by
 * the plugin and valid until its shared library is unloaded; the host copies it
 * before publishing the operator.
 */
struct CppOperatorDescriptorV2 {
    uint32_t struct_size;
    const char* category;
    const char* name;
    const char* description;
    Guid contract_iid;
};

constexpr uint32_t kCppOperatorDescriptorV2Size = static_cast<uint32_t>(sizeof(CppOperatorDescriptorV2));

using CppOperatorPluginAbiVersionFn = int (*)();
using CppOperatorPluginCountFn = int (*)();
using CppOperatorPluginCreateV1Fn = IOperator* (*)(int index);
using CppOperatorPluginDestroyV1Fn = void (*)(IOperator* operator_instance);
using CppOperatorPluginDescribeV2Fn = int (*)(int index, CppOperatorDescriptorV2* descriptor);
// Create returns the exact interface pointer identified by the descriptor's IID,
// not a most-derived object pointer. The process querier must outlive the capability.
using CppOperatorPluginCreateCapabilityV2Fn = void* (*)(int index, IQuerier* querier);
// The host destroys the capability with the same index, after all its tasks and
// leases have been released, and before unloading the shared library.
using CppOperatorPluginDestroyCapabilityV2Fn = void (*)(int index, void* capability);

}  // namespace flowsql

// The version selects either the V1 create/destroy pair or the V2
// describe/create-capability/destroy-capability group. The count is the sole
// index space for every operator in this .so. No exception may cross an export.
EXPORT_API int flowsql_abi_version();
EXPORT_API int flowsql_operator_count();
EXPORT_API flowsql::IOperator* flowsql_create_operator(int index);
EXPORT_API void flowsql_destroy_operator(flowsql::IOperator* operator_instance);
EXPORT_API int flowsql_describe_operator(int index, flowsql::CppOperatorDescriptorV2* descriptor);
EXPORT_API void* flowsql_create_operator_capability(int index, flowsql::IQuerier* querier);
EXPORT_API void flowsql_destroy_operator_capability(int index, void* capability);

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_CPP_OPERATOR_PLUGIN_ABI_H_
