// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <framework/core/dataframe.h>
#include <framework/interfaces/cpp_operator_plugin_abi.h>
#include <framework/interfaces/idataframe_channel.h>
#include <algorithm>
#include <common/operator_contract_observation.hpp>
#include <numeric>
using namespace flowsql;
using namespace flowsql::dataset;
namespace {
class OutputOperator final : public IOperator, public IOperatorConfigAdapterV1, public IOperatorOutputObservationV1 {
 public:
    std::string Category() override { return "contractdemo"; }
    std::string Name() override { return "output"; }
    std::string Description() override { return "Numerical output contract fixture"; }
    OperatorPosition Position() override { return OperatorPosition::DATA; }
    int Configure(const char* k, const char* v) override {
        if (std::string(k) == "operation") operation_ = v;
        if (std::string(k) == "column") column_ = v;
        return 0;
    }
    int ResolveInvocation(const char* text, std::string* out, DataSemanticsErrorV1*) override {
        Json request;
        request.Parse(text);
        auto& a = request.GetAllocator();
        Value config(rapidjson::kObjectType);
        Set(config, "operation", operation_, a);
        config.AddMember("secondary", operation_ == "dual", a);
        Set(request["parameters"], "column", column_, a);
        Set(request["parameters"], "operation", operation_, a);
        Set(request, "effective_parameters", Copy(request["parameters"], a), a);
        request.RemoveMember("parameters");
        request.RemoveMember("operator");
        request.AddMember("normalized_config", config, a).AddMember("config_refs", Value(rapidjson::kArrayType), a);
        *out = Encode(request);
        return 0;
    }
    int Work(IChannel* input, IChannel* output) override {
        auto* in = dynamic_cast<IDataFrameChannel*>(input);
        auto* out = dynamic_cast<IDataFrameChannel*>(output);
        DataFrame frame;
        if (!in || !out || in->Read(&frame)) return -1;
        auto batch = frame.ToArrow();
        auto column = batch->GetColumnByName(column_);
        if (!column || column->type_id() != arrow::Type::DOUBLE) return -1;
        auto data = std::static_pointer_cast<arrow::DoubleArray>(column);
        std::vector<double> values;
        for (int64_t i = 0; i < data->length(); ++i) values.push_back(data->Value(i));
        if (operation_ == "dual") {
            arrow::Int64Builder count;
            if (!count.Append(values.size()).ok()) return -1;
            secondary_ = arrow::RecordBatch::Make(arrow::schema({arrow::field("count", arrow::int64(), false)}), 1,
                                                  {count.Finish().ValueOrDie()});
        }
        if (operation_ == "drift") {
            arrow::Int64Builder wrong;
            for (auto value : values)
                if (!wrong.Append(static_cast<int64_t>(value)).ok()) return -1;
            batch = arrow::RecordBatch::Make(arrow::schema({arrow::field(column_, arrow::int64(), false)}),
                                             values.size(), {wrong.Finish().ValueOrDie()});
        } else if (operation_ == "rename" || operation_ == "scale" || operation_ == "sum" || operation_ == "count" ||
                   operation_ == "mean" || operation_ == "min" || operation_ == "max") {
            std::string name = operation_ == "rename" ? "duration" : operation_ == "scale" ? "seconds" : "value";
            if (operation_ == "scale")
                for (auto& value : values) value /= 1000.0;
            if (name == "value") {
                if (values.empty()) return -1;
                double value = std::accumulate(values.begin(), values.end(), 0.0);
                if (operation_ == "count") value = values.size();
                if (operation_ == "mean") value /= values.size();
                if (operation_ == "min") value = *std::min_element(values.begin(), values.end());
                if (operation_ == "max") value = *std::max_element(values.begin(), values.end());
                values = {value};
            }
            arrow::DoubleBuilder builder;
            if (!builder.AppendValues(values).ok()) return -1;
            batch = arrow::RecordBatch::Make(arrow::schema({arrow::field(name, arrow::float64(), false)}),
                                             values.size(), {builder.Finish().ValueOrDie()});
        }
        frame.FromArrow(batch);
        return out->Write(&frame);
    }
    int Work(Span<IChannel*> inputs, IChannel* output) override {
        return inputs.size == 1 ? Work(inputs[0], output) : -1;
    }
    int OutputObservations(std::string* out) const override {
        Json observations(rapidjson::kArrayType);
        if (secondary_)
            observations.PushBack(contracts::ObservedOutput("secondary", secondary_->schema(), identity_,
                                                            secondary_->num_rows(), observations.GetAllocator()),
                                  observations.GetAllocator());
        *out = Encode(observations);
        return 0;
    }

 private:
    std::string operation_ = "copy", column_ = "latency", identity_ = Token();
    std::shared_ptr<arrow::RecordBatch> secondary_;
};
std::string Declaration() {
    Json d;
    d.Parse(
        R"({"schema_id":"flowsql.operator-data-contract.v1","title":"Numerical fixture","operator":{"category":"contractdemo","name":"output","version":"1"},"configuration":{"invocation_schema":{"type":"object","properties":{"operation":{"type":"string"},"column":{"type":"string"}},"additionalProperties":true},"affects":{}},"inputs":[{"port_id":"input","min_count":1,"max_count":1,"shape":"dataframe","roles":[{"role_id":"measure","required":true,"types":[{"system":"arrow","name":"double","parameters":{}}],"nullable":null,"semantic_requirements":{},"selector":{"kind":"parameter","key":"column"}}]}],"outputs":[{"port_id":"main","shape":"dataframe","mode":"configured","rules":[],"variants":[]},{"port_id":"secondary","shape":"dataframe","mode":"unknown","rules":[],"structure_ref":"#/resources/count","availability":{"scope":"config","pointer":"/secondary","equals":true}}],"resources":{"count":{"fields":[{"name":"count","type":{"system":"arrow","name":"int64","parameters":{}},"nullable":false}]}},"limitations":["Structure observation never proves row quality."]})");
    auto& a = d.GetAllocator();
    for (const char* op : {"copy", "rename", "scale", "sum", "count", "mean", "min", "max", "unknown", "dual", "drift",
                           "fail_metadata"}) {
        Value variant(rapidjson::kObjectType), when(rapidjson::kObjectType), rule(rapidjson::kObjectType),
            rules(rapidjson::kArrayType);
        Set(when, "scope", "config", a);
        Set(when, "pointer", "/operation", a);
        Set(when, "equals", op, a);
        variant.AddMember("when", when, a);
        Set(variant, "mode", std::string(op) == "unknown" ? "unknown" : "derived", a);
        const std::string operation = op;
        const bool aggregate = operation == "sum" || operation == "count" || operation == "mean" ||
                               operation == "min" || operation == "max";
        Set(rule, "source_port", "input", a);
        if (operation == "copy" || operation == "dual" || operation == "unknown" || operation == "drift" ||
            operation == "fail_metadata") {
            Set(rule, "kind", "copy", a);
            rule.AddMember("all_fields", true, a);
        } else {
            Set(rule, "kind", aggregate ? "aggregate" : operation == "scale" ? "unit_convert" : "rename", a);
            Value roles(rapidjson::kArrayType);
            roles.PushBack("measure", a);
            rule.AddMember("source_roles", roles, a);
            Set(rule, "output_field", aggregate ? "value" : operation == "scale" ? "seconds" : "duration", a);
            if (aggregate) {
                Value detail(rapidjson::kObjectType);
                Set(detail, "operation", operation, a);
                Set(detail, "null_policy", "ignore", a);
                detail.AddMember("group_by", Value(rapidjson::kArrayType), a)
                    .AddMember("prerequisites_confirmed", true, a)
                    .AddMember("type", Type("arrow", "double", a), a);
                rule.AddMember("aggregation_rule", detail, a);
            } else if (operation == "scale") {
                Json detail;
                detail.Parse(
                    R"({"source_unit":{"code":"ms","label":"milliseconds"},"target_unit":{"code":"s","label":"seconds"},"scale":0.001,"offset":0,"type":{"system":"arrow","name":"double","parameters":{}},"rounding":"none","null_policy":"propagate","overflow_policy":"error"})");
                rule.AddMember("unit_rule", Copy(detail, a), a);
            }
        }
        rules.PushBack(rule, a);
        variant.AddMember("rules", rules, a);
        d["outputs"][0]["variants"].PushBack(variant, a);
    }
    return Encode(d);
}
}  // namespace
extern "C" int flowsql_abi_version() { return 1; }
extern "C" int flowsql_operator_count() { return 1; }
extern "C" IOperator* flowsql_create_operator(int index) { return index ? nullptr : new OutputOperator; }
extern "C" void flowsql_destroy_operator(IOperator* op) { delete op; }
extern "C" const char* flowsql_operator_data_contract_v1(int index) {
    static const auto text = Declaration();
    return index ? nullptr : text.c_str();
}
