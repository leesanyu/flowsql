#include "sample_operator.h"

#if defined(_WIN32)
#define FLOWSQL_SDK_EXPORT __declspec(dllexport)
#else
#define FLOWSQL_SDK_EXPORT __attribute__((visibility("default")))
#endif

extern "C" {

FLOWSQL_SDK_EXPORT int flowsql_abi_version() {
    return 1;
}

FLOWSQL_SDK_EXPORT int flowsql_operator_count() {
    return 3;
}

FLOWSQL_SDK_EXPORT flowsql::IOperator* flowsql_create_operator(int index) {
    switch (index) {
        case 0: return new PassThroughOperator();
        case 1: return new RowCountOperator();
        case 2: return new ColumnStatsOperator();
        default: return nullptr;
    }
}

FLOWSQL_SDK_EXPORT void flowsql_destroy_operator(flowsql::IOperator* op) {
    delete op;
}

}  // extern "C"
