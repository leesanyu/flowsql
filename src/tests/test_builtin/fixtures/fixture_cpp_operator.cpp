#include <framework/interfaces/ioperator.h>

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

FLOWSQL_SDK_EXPORT int flowsql_abi_version() {
    return 1;
}

FLOWSQL_SDK_EXPORT int flowsql_operator_count() {
    return 1;
}

FLOWSQL_SDK_EXPORT flowsql::IOperator* flowsql_create_operator(int index) {
    if (index != 0) return nullptr;
    return new FixtureCppOperator();
}

FLOWSQL_SDK_EXPORT void flowsql_destroy_operator(flowsql::IOperator* op) {
    delete op;
}

}  // extern "C"

