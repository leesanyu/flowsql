// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <framework/interfaces/cpp_operator_plugin_abi.h>
#include <framework/interfaces/ioperator_data_contract.h>
#include <stdexcept>
#include <string>
#ifdef FIXTURE_EXECUTION
#include <framework/core/dataframe.h>
#include <framework/interfaces/idataframe_channel.h>
#include <fstream>
#endif
#ifndef FIXTURE_ABI
#define FIXTURE_ABI 1
#endif
#ifndef FIXTURE_NAME
#define FIXTURE_NAME "echo"
#endif
namespace {
class Operator : public flowsql::IOperator
#ifndef FIXTURE_EXECUTION
    ,
                 public flowsql::IOperatorConfigAdapterV1
#endif
{
 public:
    std::string Category() override { return "contractdemo"; }
    std::string Name() override { return FIXTURE_NAME; }
    std::string Description() override { return "Contract registration fixture"; }
    flowsql::OperatorPosition Position() override { return flowsql::OperatorPosition::DATA; }
#ifndef FIXTURE_EXECUTION
    int ResolveInvocation(const char*, std::string* out, flowsql::DataSemanticsErrorV1* error) override {
        if (error) *error = {};
        if (!out) return -1;
        *out = R"({"effective_parameters":{"factor":7},"inputs":[],"config_refs":[]})";
        return 0;
    }
#endif
#ifdef FIXTURE_EXECUTION
    int Configure(const char* key, const char* value) override {
        if (key && value && std::string(key) == "marker") marker_ = value;
        return 0;
    }
    int Work(flowsql::IChannel* in, flowsql::IChannel* out) override {
        if (!marker_.empty()) std::ofstream(marker_, std::ios::app) << "work\n";
        auto* input = dynamic_cast<flowsql::IDataFrameChannel*>(in);
        auto* output = dynamic_cast<flowsql::IDataFrameChannel*>(out);
        flowsql::DataFrame frame;
        return input && output && input->Read(&frame) == 0 ? output->Write(&frame) : -1;
    }
    int Work(flowsql::Span<flowsql::IChannel*> inputs, flowsql::IChannel* out) override {
        return inputs.empty() ? -1 : Work(inputs[0], out);
    }

 private:
    std::string marker_;
#else
    int Configure(const char*, const char*) override { return 0; }
    int Work(flowsql::IChannel*, flowsql::IChannel*) override { return 0; }
#endif
};
}  // namespace
extern "C" int flowsql_abi_version() { return FIXTURE_ABI; }
extern "C" int flowsql_operator_count() { return 1; }
extern "C" flowsql::IOperator* flowsql_create_operator(int index) { return index == 0 ? new Operator : nullptr; }
extern "C" void flowsql_destroy_operator(flowsql::IOperator* op) { delete op; }
extern "C" int flowsql_describe_operator(int index, flowsql::CppOperatorDescriptorV2* d) {
    if (index || !d || d->struct_size != flowsql::kCppOperatorDescriptorV2Size) return -1;
    *d = {flowsql::kCppOperatorDescriptorV2Size, "contractdemo", FIXTURE_NAME, "Contract fixture",
          flowsql::IID_OPERATOR};
    return 0;
}
extern "C" void* flowsql_create_operator_capability(int index, flowsql::IQuerier*) {
    return index == 0 ? static_cast<flowsql::IOperator*>(new Operator) : nullptr;
}
extern "C" void flowsql_destroy_operator_capability(int, void* p) { delete static_cast<flowsql::IOperator*>(p); }
extern "C" const char* flowsql_operator_data_contract_v1(int index) {
    if (index) return nullptr;
#if defined(FIXTURE_TOO_LONG)
    static const std::string too_long(8 * 1024 * 1024 + 1, 'x');
    return too_long.c_str();
#elif defined(FIXTURE_THROW)
    throw std::runtime_error("fixture export failure");
#elif defined(FIXTURE_INVALID)
    return "{invalid";
#elif defined(FIXTURE_REFERENCE)
    return "{\"contract_ref\":\"operator_contract.contractdemo." FIXTURE_NAME "@1\"}";
#elif defined(FIXTURE_EXECUTION)
    return R"({"schema_id":"flowsql.operator-data-contract.v1","title":"Actual input fixture",
      "operator":{"category":"contractdemo","name":"execution","version":"1"},
      "configuration":{"invocation_schema":{"type":"object","additionalProperties":true},"affects":{}},
      "inputs":[{"port_id":"left","min_count":1,"max_count":1,"shape":"dataframe","roles":[
        {"role_id":"measure","required":true,"types":[{"system":"arrow","name":"int64","parameters":{}}],"nullable":null,"semantic_requirements":{},"selector":{"kind":"parameter","key":"column"}}]},
        {"port_id":"right","min_count":0,"max_count":1,"shape":"dataframe","roles":[
        {"role_id":"measure","required":true,"types":[{"system":"arrow","name":"int64","parameters":{}}],"nullable":null,"semantic_requirements":{},"selector":{"kind":"parameter","key":"column"}}]}],
      "outputs":[{"port_id":"output","shape":"dataframe","mode":"derived","rules":[{"kind":"copy","source_port":"left","all_fields":true}]}],"limitations":[]})";
#else
    return "{\"schema_id\":\"flowsql.operator-data-contract.v1\",\"title\":\"C++ fixture\","
           "\"operator\":{\"category\":\"contractdemo\",\"name\":\"" FIXTURE_NAME
           "\",\"version\":\"1\"},"
           "\"configuration\":{\"invocation_schema\":{\"type\":\"object\"},\"affects\":{}},"
           "\"inputs\":[{\"port_id\":\"input\",\"min_count\":1,\"max_count\":1,\"shape\":\"dataframe\",\"roles\":[]}],"
           "\"outputs\":[{\"port_id\":\"output\",\"shape\":\"dataframe\",\"mode\":\"derived\",\"rules\":[{\"kind\":"
           "\"copy\",\"source_port\":\"input\",\"all_fields\":true}]}],\"limitations\":[]}";
#endif
}
