// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#ifndef _FLOWSQL_FRAMEWORK_INTERFACES_CPP_OPERATOR_PLUGIN_ABI_H_
#define _FLOWSQL_FRAMEWORK_INTERFACES_CPP_OPERATOR_PLUGIN_ABI_H_

#include <common/guid.h>
#include <framework/interfaces/ioperator.h>
#include <common/iquerier.hpp>

#include <cstdint>

namespace flowsql {

constexpr int kCppOperatorPluginAbiVersion = 2;

inline constexpr char kCppOperatorPluginAbiVersionSymbol[] = "flowsql_abi_version";
inline constexpr char kCppOperatorPluginCountSymbol[] = "flowsql_operator_count";
inline constexpr char kCppOperatorPluginDescribeSymbol[] = "flowsql_describe_operator";
inline constexpr char kCppOperatorPluginCreateCapabilitySymbol[] = "flowsql_create_operator_capability";
inline constexpr char kCppOperatorPluginDestroyCapabilitySymbol[] = "flowsql_destroy_operator_capability";

/**
 * Describes one functionally named operator in the plugin-wide index space.
 *
 * The caller zero-initializes the structure and sets struct_size to
 * kCppOperatorDescriptorSize. The plugin must reject a null descriptor, a
 * smaller structure, or an out-of-range index. String storage remains owned by
 * the plugin and valid until its shared library is unloaded; the host copies it
 * before publishing the operator.
 */
struct CppOperatorDescriptor {
    uint32_t struct_size;
    const char* category;
    const char* name;
    const char* description;
    Guid contract_iid;
};

constexpr uint32_t kCppOperatorDescriptorSize = static_cast<uint32_t>(sizeof(CppOperatorDescriptor));

// Optional metadata export, independent of descriptor.contract_iid and its ABI.
inline constexpr const char* kOperatorDataContractSymbol = "flowsql_operator_data_contract";
using OperatorDataContractFn = const char* (*)(int index);
using CppOperatorPluginAbiVersionFn = int (*)();
using CppOperatorPluginCountFn = int (*)();
using CppOperatorPluginDescribeFn = int (*)(int index, CppOperatorDescriptor* descriptor);
// Create returns the exact interface pointer identified by the descriptor's IID,
// not a most-derived object pointer. The process querier must outlive the capability.
using CppOperatorPluginCreateCapabilityFn = void* (*)(int index, IQuerier* querier);
// The host destroys the capability with the same index, after all its tasks and
// leases have been released, and before unloading the shared library.
using CppOperatorPluginDestroyCapabilityFn = void (*)(int index, void* capability);

}  // namespace flowsql

// Only the current ABI is accepted. The descriptor/capability group and count form the sole
// index space for every operator in this .so. No exception may cross an export.
// Optional: immutable borrowed JSON document/{contract_ref}; null/empty = undeclared.
// The host copies at most 8 MiB while holding the library lifetime. Must not throw.
EXPORT_API const char* flowsql_operator_data_contract(int index);
EXPORT_API int flowsql_abi_version();
EXPORT_API int flowsql_operator_count();
EXPORT_API int flowsql_describe_operator(int index, flowsql::CppOperatorDescriptor* descriptor);
EXPORT_API void* flowsql_create_operator_capability(int index, flowsql::IQuerier* querier);
EXPORT_API void flowsql_destroy_operator_capability(int index, void* capability);

#endif  // _FLOWSQL_FRAMEWORK_INTERFACES_CPP_OPERATOR_PLUGIN_ABI_H_
