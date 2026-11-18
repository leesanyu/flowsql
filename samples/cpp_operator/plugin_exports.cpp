// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <framework/interfaces/cpp_operator_plugin_abi.h>
#include "sample_operator.h"

extern "C" int flowsql_abi_version() { return flowsql::kCppOperatorPluginAbiVersion; }
extern "C" int flowsql_operator_count() { return 3; }
extern "C" int flowsql_describe_operator(int index, flowsql::CppOperatorDescriptor *descriptor) {
    static const char *names[] = {"passthrough", "row_count", "column_stats"};
    if (index < 0 || index >= 3 || !descriptor || descriptor->struct_size < flowsql::kCppOperatorDescriptorSize)
        return -1;
    *descriptor = {flowsql::kCppOperatorDescriptorSize, "sample", names[index], "Sample DataFrame operator",
                   flowsql::IID_OPERATOR};
    return 0;
}
extern "C" void *flowsql_create_operator_capability(int index, flowsql::IQuerier *) {
    try {
        switch (index) {
            case 0:
                return static_cast<flowsql::IOperator *>(new PassThroughOperator());
            case 1:
                return static_cast<flowsql::IOperator *>(new RowCountOperator());
            case 2:
                return static_cast<flowsql::IOperator *>(new ColumnStatsOperator());
            default:
                return nullptr;
        }
    } catch (...) {
        return nullptr;
    }
}
extern "C" void flowsql_destroy_operator_capability(int, void *capability) {
    delete static_cast<flowsql::IOperator *>(capability);
}
