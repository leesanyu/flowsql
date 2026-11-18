// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#include <framework/interfaces/cpp_operator_plugin_abi.h>

#include <string>

namespace {

class FixtureCppOperator : public flowsql::IOperator {
 public:
    std::string Category() override { return "cppdemo"; }
    std::string Name() override { return "echo"; }
    std::string Description() override { return "fixture cpp operator"; }
    flowsql::OperatorPosition Position() override { return flowsql::OperatorPosition::DATA; }
    int Work(flowsql::IChannel*, flowsql::IChannel*) override { return 0; }
    int Configure(const char*, const char*) override { return 0; }
};

}  // namespace

#if defined(_WIN32)
#define FLOWSQL_SDK_EXPORT __declspec(dllexport)
#else
#define FLOWSQL_SDK_EXPORT __attribute__((visibility("default")))
#endif

extern "C" {

FLOWSQL_SDK_EXPORT int flowsql_abi_version() { return flowsql::kCppOperatorPluginAbiVersion; }

FLOWSQL_SDK_EXPORT int flowsql_operator_count() {
    return 1;
}

}  // extern "C"

extern "C" int flowsql_describe_operator(int index, flowsql::CppOperatorDescriptor* descriptor) {
    if (index != 0 || !descriptor || descriptor->struct_size < flowsql::kCppOperatorDescriptorSize) return -1;
    *descriptor = {flowsql::kCppOperatorDescriptorSize, "cppdemo", "echo", "Current operator fixture",
                   flowsql::IID_OPERATOR};
    return 0;
}
extern "C" void* flowsql_create_operator_capability(int index, flowsql::IQuerier*) {
    if (index != 0) return nullptr;
    try {
        return static_cast<flowsql::IOperator*>(new FixtureCppOperator);
    } catch (...) {
        return nullptr;
    }
}
extern "C" void flowsql_destroy_operator_capability(int, void* capability) {
    delete static_cast<flowsql::IOperator*>(capability);
}
