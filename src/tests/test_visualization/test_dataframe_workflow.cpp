// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
// T3 acceptance: fixed Arrow fixtures with independently stated expectations for every frozen
// operation, NULL/empty semantics, exact decimal, calendar buckets, histograms and budgets.

#include <arrow/api.h>
#include <rapidjson/document.h>
#include <services/visualization/dataframe_workflow.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>

using namespace flowsql;
using namespace flowsql::visualization;

namespace {
int failures = 0;

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

std::chrono::steady_clock::time_point Deadline() { return std::chrono::steady_clock::now() + std::chrono::seconds(30); }

rapidjson::Document Json(const std::string& text) {
    rapidjson::Document document;
    document.Parse(text.c_str());
    if (document.HasParseError()) std::printf("bad fixture json: %s\n", text.c_str());
    return document;
}

DataFrameResultV1 Run(const std::shared_ptr<arrow::RecordBatch>& batch, const std::string& workflow,
                      const std::string& mode = "complete", uint32_t rows = 10000) {
    const auto payload = Json("{\"workflow\":" + workflow + "}");
    return ExecuteDataFrameWorkflow(batch, DescribeArrowSchema(*batch->schema()), payload, mode, rows, 8 << 20,
                                    Deadline());
}

std::string ErrorCode(const std::shared_ptr<arrow::RecordBatch>& batch, const std::string& workflow) {
    try {
        Run(batch, workflow);
    } catch (const QueryFailure& error) {
        return error.code;
    }
    return {};
}

// host (text), value (int64 with NULL), ratio (double), precise (decimal128(38,4)), flag (bool),
// day (date32), stamp (timestamp with zone).
struct Fixture {
    std::shared_ptr<arrow::RecordBatch> batch;

    Fixture() {
        arrow::StringBuilder host;
        arrow::Int64Builder value;
        arrow::DoubleBuilder ratio;
        arrow::Decimal128Builder precise(arrow::decimal128(38, 4));
        arrow::Int64Builder wide;
        arrow::BooleanBuilder flag;
        arrow::Date32Builder day;
        arrow::TimestampBuilder stamp(arrow::timestamp(arrow::TimeUnit::MILLI, "UTC"), arrow::default_memory_pool());
        const char* hosts[] = {"a", "a", "b", "", "b", "a"};
        const bool has_value[] = {true, true, true, true, false, true};
        const int64_t values[] = {10, 20, 40, 55, 0, -5};
        const int64_t wides[] = {1, 2, 3, 9007199254740992LL, 5, 6};
        const double ratios[] = {1.5, 2.5, 3.5, 4.5, 5.5, 6.5};
        const char* precises[] = {"0.1000", "1.2500", "-0.0500", "2.0000", "0.0000", "0.7000"};
        const bool flags[] = {true, false, true, true, false, true};
        const int32_t days[] = {0, 31, 59, 60, 365, 397};  // 1970-01-01 .. 1971-02-02
        const int64_t stamps[] = {0, 3600000, 86399999, 86400000, 90061000, 4102444800000LL};
        for (int i = 0; i < 6; ++i) {
            host.Append(hosts[i]);
            if (has_value[i])
                value.Append(values[i]);
            else
                value.AppendNull();
            wide.Append(wides[i]);
            ratio.Append(ratios[i]);
            arrow::Decimal128 decimal;
            int32_t precision = 0, scale = 0;
            if (!arrow::Decimal128::FromString(std::string(precises[i]), &decimal, &precision, &scale).ok())
                std::printf("decimal fixture\n");
            precise.Append(decimal);
            flag.Append(flags[i]);
            day.Append(days[i]);
            stamp.Append(stamps[i]);
        }
        std::shared_ptr<arrow::Array> host_array, value_array, wide_array, ratio_array, precise_array, flag_array,
            day_array, stamp_array;
        host.Finish(&host_array);
        value.Finish(&value_array);
        wide.Finish(&wide_array);
        ratio.Finish(&ratio_array);
        precise.Finish(&precise_array);
        flag.Finish(&flag_array);
        day.Finish(&day_array);
        stamp.Finish(&stamp_array);
        batch = arrow::RecordBatch::Make(
            arrow::schema({arrow::field("host", arrow::utf8()), arrow::field("value", arrow::int64()),
                           arrow::field("wide", arrow::int64()), arrow::field("ratio", arrow::float64()),
                           arrow::field("precise", arrow::decimal128(38, 4)), arrow::field("flag", arrow::boolean()),
                           arrow::field("day", arrow::date32()),
                           arrow::field("stamp", arrow::timestamp(arrow::TimeUnit::MILLI, "UTC"))}),
            6, {host_array, value_array, wide_array, ratio_array, precise_array, flag_array, day_array, stamp_array});
    }
};

int64_t Integer(const DataFrameResultV1& result, size_t row, size_t column) {
    if (auto value = std::get_if<int64_t>(&result.rows[row][column])) return *value;
    return INT64_MIN;
}
double Real(const DataFrameResultV1& result, size_t row, size_t column) {
    if (auto value = std::get_if<double>(&result.rows[row][column])) return *value;
    if (auto value = std::get_if<int64_t>(&result.rows[row][column])) return static_cast<double>(*value);
    return NAN;
}
std::string Text(const DataFrameResultV1& result, size_t row, size_t column) {
    if (auto value = std::get_if<std::string>(&result.rows[row][column])) return *value;
    return {};
}
std::string Decimal(const DataFrameResultV1& result, size_t row, size_t column) {
    if (auto value = std::get_if<ExplorationDecimalV1>(&result.rows[row][column])) return value->text;
    return {};
}

void TestProjectionAndSort() {
    Fixture fixture;
    auto result = Run(fixture.batch, R"([{"type":"view","query":[{"op":"raw","fields":["f0","f1"]}]}])");
    CHECK(result.fields.size() == 2 && result.fields[0].id == "f0" && result.scope == "complete");
    CHECK(result.rows.size() == 6);
    CHECK(Text(result, 0, 0) == "a" && Integer(result, 0, 1) == 10);  // snapshot order is preserved
    CHECK(std::holds_alternative<std::monostate>(result.rows[4][1]));

    result = Run(fixture.batch,
                 R"([{"type":"view","query":[{"op":"raw","fields":["f0","f1"]}]},
                     {"type":"sort","by":["f1"],"sort":"ascending"}])");
    CHECK(std::holds_alternative<std::monostate>(result.rows.back()[1]));  // NULL sorts last
    CHECK(Integer(result, 0, 1) == -5);
    CHECK(result.scope == "complete");

    result = Run(fixture.batch,
                 R"([{"type":"view","query":[{"op":"raw","fields":["f0"]}]},
                     {"type":"sort","by":["f0"],"sort":"ascending"}])");
    CHECK(Text(result, 0, 0).empty());  // empty string sorts before text, distinct from NULL
    CHECK(Text(result, 1, 0) == "a");

    auto limited = Run(fixture.batch, R"([{"type":"view","query":[{"op":"raw","fields":["f0"]}]}])", "limited");
    CHECK(limited.scope == "limited_detail");
    auto top = ExecuteDataFrameWorkflow(
        fixture.batch, DescribeArrowSchema(*fixture.batch->schema()),
        Json(R"({"workflow":[{"type":"view","query":[{"op":"raw","fields":["f0"]}]}],"limit":2})"), "complete", 10000,
        8 << 20, Deadline());
    CHECK(top.scope == "top_n" && top.rows.size() == 2);
    CHECK(ErrorCode(fixture.batch,
                    R"([{"type":"view","query":[{"op":"raw","fields":["f1"]}]},
                        {"type":"sort","by":["f0"],"sort":"ascending"}])") == "INVALID_WORKFLOW");
    CHECK(ErrorCode(fixture.batch, R"([{"type":"view","query":[{"op":"raw","fields":["f99"]}]}])") ==
          "INVALID_WORKFLOW");
    std::puts("[PASS] projection, snapshot order, NULL-last ordering, scopes");
}

void TestFilters() {
    Fixture fixture;
    const auto filtered = [&](const std::string& filters) {
        return Run(fixture.batch, R"([{"type":"filter","filters":)" + filters +
                                      R"(},{"type":"view","query":[{"op":"raw","fields":["f1"]}]}])");
    };
    auto result = filtered(R"([{"fid":"f1","rule":{"type":"comparison","op":"gt","value":15}}])");
    CHECK(result.rows.size() == 3);  // NULL never matches a comparison

    result = filtered(R"([{"fid":"f1","rule":{"type":"is null"}}])");
    CHECK(result.rows.size() == 1 && std::holds_alternative<std::monostate>(result.rows[0][0]));

    result = filtered(R"([{"fid":"f0","rule":{"type":"comparison","op":"eq","value":null}}])");
    CHECK(result.rows.empty());  // no NULL host
    result = filtered(R"([{"fid":"f1","rule":{"type":"comparison","op":"ne","value":10}}])");
    CHECK(result.rows.size() == 4);  // ne keeps non-NULL rows that differ

    result = filtered(R"([{"fid":"f0","rule":{"type":"one of","value":["a",null]}}])");
    CHECK(result.rows.size() == 3);
    result = filtered(R"([{"fid":"f0","rule":{"type":"not in","value":["a"]}}])");
    CHECK(result.rows.size() == 3);  // "", b and a NULL-free containment: "", b, b
    result = filtered(R"([{"fid":"f1","rule":{"type":"range","value":[10,40]}}])");
    CHECK(result.rows.size() == 3);

    result = filtered(
        R"([{"op":"and","filters":[{"fid":"f0","rule":{"type":"comparison","op":"eq","value":"a"}},
                                   {"fid":"f1","rule":{"type":"comparison","op":"gte","value":10}}]}])");
    CHECK(result.rows.size() == 2);
    result = filtered(
        R"([{"op":"or","filters":[{"fid":"f0","rule":{"type":"comparison","op":"eq","value":""}},
                                  {"fid":"f1","rule":{"type":"comparison","op":"lt","value":0}}]}])");
    CHECK(result.rows.size() == 2);
    CHECK(ErrorCode(fixture.batch,
                    R"([{"type":"filter","filters":[{"fid":"f0","rule":{"type":"comparison","op":"gt","value":1}}]},
                        {"type":"view","query":[{"op":"raw","fields":["f0"]}]}])") == "UNSUPPORTED_TYPE");
    std::puts("[PASS] comparison/set/NULL/range filters, AND/OR, type guards");
}

void TestAggregates() {
    Fixture fixture;
    const auto view = [](const std::string& group, const std::string& measures) {
        return R"([{"type":"view","query":[{"op":"aggregate","groupBy":[)" + group + R"(],"measures":[)" + measures +
               R"(]}]}])";
    };
    // Ungrouped over value = [10,20,40,55,NULL,-5]: count 5, sum 120, mean 24.
    auto result = Run(fixture.batch, view("", R"({"field":"*","agg":"count","asFieldKey":"rows"},
                                                {"field":"f1","agg":"count","asFieldKey":"present"},
                                                {"field":"f1","agg":"mean","asFieldKey":"mean"},
                                                {"field":"f1","agg":"sum","asFieldKey":"total"},
                                                {"field":"f1","agg":"min","asFieldKey":"low"},
                                                {"field":"f1","agg":"max","asFieldKey":"high"},
                                                {"field":"f0","agg":"distinctCount","asFieldKey":"hosts"})"));
    CHECK(result.rows.size() == 1);
    CHECK(Integer(result, 0, 0) == 6);
    CHECK(Integer(result, 0, 1) == 5);
    CHECK(Real(result, 0, 2) == 24 && Integer(result, 0, 3) == 120);
    CHECK(Integer(result, 0, 4) == -5 && Integer(result, 0, 5) == 55);
    CHECK(Integer(result, 0, 6) == 3);  // "", a, b
    // The wide column holds 9007199254740991: its sum is rejected rather than rounded.
    CHECK(ErrorCode(fixture.batch, view("", R"({"field":"f2","agg":"sum","asFieldKey":"total"})")) ==
          "UNSUPPORTED_TYPE");

    // Grouped by host: keys sort as bytes, so the empty string precedes "a". The empty host
    // carries the only NULL value, so its sum stays NULL rather than 0.
    result = Run(fixture.batch, view(R"("f0")", R"({"field":"*","agg":"count","asFieldKey":"rows"},
                                                 {"field":"f1","agg":"sum","asFieldKey":"total"})"));
    CHECK(result.rows.size() == 3);
    CHECK(Text(result, 0, 0).empty() && Integer(result, 0, 1) == 1);
    CHECK(Integer(result, 0, 2) == 55);  // the empty host owns 55, not the NULL row
    CHECK(Text(result, 1, 0) == "a" && Integer(result, 1, 1) == 3 && Integer(result, 1, 2) == 25);
    CHECK(Text(result, 2, 0) == "b" && Integer(result, 2, 1) == 2 && Integer(result, 2, 2) == 40);

    // Compound key keeps each tuple as its own group.
    result = Run(fixture.batch, view(R"("f0","f5")", R"({"field":"*","agg":"count","asFieldKey":"rows"})"));
    CHECK(result.rows.size() == 5);  // ("",true) ("a",false) ("a",true) ("b",false) ("b",true)
    CHECK(Text(result, 0, 0).empty() && Text(result, 0, 1) == "true");
    CHECK(Text(result, 4, 0) == "b" && Text(result, 4, 1) == "true");

    // All-NULL and empty inputs: count 0, other aggregates NULL, zero groups when grouped.
    arrow::Int64Builder numbers;
    numbers.AppendNull();
    numbers.AppendNull();
    std::shared_ptr<arrow::Array> nulls;
    numbers.Finish(&nulls);
    auto null_batch = arrow::RecordBatch::Make(arrow::schema({arrow::field("v", arrow::int64())}), 2, {nulls});
    result = Run(null_batch, view("", R"({"field":"f0","agg":"sum","asFieldKey":"s"},
                                          {"field":"f0","agg":"mean","asFieldKey":"m"},
                                          {"field":"f0","agg":"min","asFieldKey":"lo"},
                                          {"field":"f0","agg":"count","asFieldKey":"c"},
                                          {"field":"f0","agg":"distinctCount","asFieldKey":"d"})"));
    CHECK(result.rows.size() == 1);
    for (size_t column = 0; column < 3; ++column) CHECK(std::holds_alternative<std::monostate>(result.rows[0][column]));
    CHECK(Integer(result, 0, 3) == 0 && Integer(result, 0, 4) == 0);

    auto empty_batch = arrow::RecordBatch::Make(arrow::schema({arrow::field("v", arrow::int64())}), 0,
                                                {*arrow::MakeArrayOfNull(arrow::int64(), 0)});
    result = Run(empty_batch, view(R"("f0")", R"({"field":"*","agg":"count","asFieldKey":"rows"})"));
    CHECK(result.rows.empty());  // grouped empty input yields zero groups
    result = Run(empty_batch, view("", R"({"field":"*","agg":"count","asFieldKey":"rows"})"));
    CHECK(result.rows.size() == 1 && Integer(result, 0, 0) == 0);

    CHECK(ErrorCode(fixture.batch, view("", R"({"field":"f0","agg":"sum","asFieldKey":"s"})")) == "UNSUPPORTED_TYPE");
    CHECK(ErrorCode(fixture.batch, view("", R"({"field":"f1","agg":"median","asFieldKey":"s"})")) ==
          "UNSUPPORTED_OPERATION");
    std::puts("[PASS] counts, exact sums, mean, min/max, distinct, compound keys, empty/NULL groups");
}

void TestTopNAndStability() {
    Fixture fixture;
    auto payload = Json(R"({"workflow":[{"type":"view","query":[{"op":"aggregate","groupBy":["f0"],
        "measures":[{"field":"*","agg":"count","asFieldKey":"rows"}]}]},
        {"type":"sort","by":["rows"],"sort":"descending"}],"limit":2})");
    auto result = ExecuteDataFrameWorkflow(fixture.batch, DescribeArrowSchema(*fixture.batch->schema()), payload,
                                           "complete", 10000, 8 << 20, Deadline());
    CHECK(result.scope == "top_n" && result.rows.size() == 2);
    CHECK(Text(result, 0, 0) == "a" && Integer(result, 0, 1) == 3);
    CHECK(Text(result, 1, 0) == "b");  // tie between b and "" resolved by group key order
    CHECK(ErrorCode(fixture.batch, R"([{"type":"view","query":[{"op":"raw","fields":["f0"]}]},
                                       {"type":"sort","by":["f9"],"sort":"ascending"}])") == "INVALID_WORKFLOW");
    std::puts("[PASS] Top-N scope and deterministic tie ordering");
}

void TestDecimals() {
    Fixture fixture;
    auto result = Run(fixture.batch,
                      R"([{"type":"view","query":[{"op":"aggregate","groupBy":[],
                          "measures":[{"field":"f4","agg":"sum","asFieldKey":"s"},
                                      {"field":"f4","agg":"mean","asFieldKey":"m"},
                                      {"field":"f4","agg":"min","asFieldKey":"lo"},
                                      {"field":"f4","agg":"max","asFieldKey":"hi"}]}]}])");
    CHECK(result.rows.size() == 1);
    CHECK(Decimal(result, 0, 0) == "4");  // exact textual sum, no floating point detour
    CHECK(Decimal(result, 0, 2) == "-0.05" && Decimal(result, 0, 3) == "2");
    CHECK(result.displays.size() == 1 && result.displays[0].field_id == "m" &&
          result.displays[0].origin == "arrow_decimal_mean" && result.displays[0].mode == "approximate");

    auto filtered =
        Run(fixture.batch,
            R"([{"type":"filter","filters":[{"fid":"f4","rule":{"type":"comparison","op":"gt","value":0.05}}]},
                            {"type":"view","query":[{"op":"raw","fields":["f4"]}]},
                            {"type":"sort","by":["f4"],"sort":"ascending"}])");
    CHECK(filtered.rows.size() == 4);  // 0.1, 0.7, 1.25, 2 all exceed 0.05
    CHECK(Decimal(filtered, 0, 0) == "0.1" && Decimal(filtered, 1, 0) == "0.7");
    CHECK(Decimal(filtered, 2, 0) == "1.25" && Decimal(filtered, 3, 0) == "2");
    std::puts("[PASS] exact decimal sum/min/max/filter/order and approximate mean provenance");
}

void TestBuckets() {
    Fixture fixture;
    auto result = Run(fixture.batch,
                      R"([{"type":"view","query":[{"op":"aggregate",
                          "groupBy":[{"field":"f7","unit":"day","asFieldKey":"d"}],
                          "measures":[{"field":"*","agg":"count","asFieldKey":"rows"}]}]}])");
    CHECK(result.rows.size() == 3);
    // 0 and 01:00:00.000Z fall on 01-01; 23:59:59.999Z, 01-02T00:00Z and 01-02T01:01:01Z on 01-02.
    CHECK(Text(result, 0, 0) == "1970-01-01" && Integer(result, 0, 1) == 3);
    CHECK(Text(result, 1, 0) == "1970-01-02" && Integer(result, 1, 1) == 2);
    result = Run(fixture.batch,
                 R"([{"type":"view","query":[{"op":"aggregate",
                     "groupBy":[{"field":"f7","unit":"month","asFieldKey":"m"}],
                     "measures":[{"field":"*","agg":"count","asFieldKey":"rows"}]}]}])");
    CHECK(Text(result, 0, 0) == "1970-01-01" && Integer(result, 0, 1) == 5);
    CHECK(Text(result, 1, 0) == "2100-01-01" && Integer(result, 1, 1) == 1);

    arrow::TimestampBuilder stamp(arrow::timestamp(arrow::TimeUnit::MILLI), arrow::default_memory_pool());
    stamp.Append(86399999);  // 1970-01-01T23:59:59.999Z; a zone-less column keeps its own calendar
    std::shared_ptr<arrow::Array> array;
    stamp.Finish(&array);
    auto zone_less = arrow::RecordBatch::Make(
        arrow::schema({arrow::field("s", arrow::timestamp(arrow::TimeUnit::MILLI))}), 1, {array});
    result = Run(zone_less, R"([{"type":"view","query":[{"op":"aggregate",
                                         "groupBy":[{"field":"f0","unit":"day","asFieldKey":"d"}],
                                         "measures":[{"field":"*","agg":"count","asFieldKey":"rows"}]}]}])");
    CHECK(Text(result, 0, 0) == "1970-01-01");

    CHECK(ErrorCode(fixture.batch,
                    R"([{"type":"view","query":[{"op":"aggregate",
                        "groupBy":[{"field":"f0","unit":"day","asFieldKey":"d"}],"measures":[]}]}])") ==
          "UNSUPPORTED_TYPE");
    std::puts("[PASS] UTC day/month buckets, zone-less calendar, text fields refused");
}

void TestHistogram() {
    Fixture fixture;
    auto result = Run(fixture.batch,
                      R"([{"type":"view","query":[{"op":"histogram","field":"f3","bins":3}]},
                          {"type":"sort","by":["bin_index"],"sort":"ascending"}])");
    CHECK(result.rows.size() == 3);
    CHECK(Integer(result, 0, 0) == 0 && Real(result, 0, 1) == 1.5);
    CHECK(Real(result, 2, 2) == 6.5);  // last bin is closed at the maximum
    CHECK(Integer(result, 2, 3) == 2);
    const auto total = Integer(result, 0, 3) + Integer(result, 1, 3) + Integer(result, 2, 3);
    CHECK(total == 6);

    arrow::DoubleBuilder constant;
    constant.Append(2.5);
    constant.Append(2.5);
    std::shared_ptr<arrow::Array> array;
    constant.Finish(&array);
    auto single = arrow::RecordBatch::Make(arrow::schema({arrow::field("v", arrow::float64())}), 2, {array});
    result = Run(single, R"([{"type":"view","query":[{"op":"histogram","field":"f0","bins":5}]}])");
    CHECK(result.rows.size() == 1 && Real(result, 0, 1) == 2.5 && Real(result, 0, 2) == 2.5);

    auto none = arrow::RecordBatch::Make(arrow::schema({arrow::field("v", arrow::float64())}), 0,
                                         {*arrow::MakeArrayOfNull(arrow::float64(), 0)});
    result = Run(none, R"([{"type":"view","query":[{"op":"histogram","field":"f0","bins":4}]}])");
    CHECK(result.rows.empty() && result.fields.size() == 4);

    // The wide outlier exceeds the safe integer range for quantitative plotting.
    CHECK(ErrorCode(fixture.batch, R"([{"type":"view","query":[{"op":"histogram","field":"f2","bins":4}]}])") ==
          "UNSUPPORTED_TYPE");
    CHECK(ErrorCode(fixture.batch, R"([{"type":"view","query":[{"op":"histogram","field":"f3","bins":101}]}])") ==
          "INVALID_WORKFLOW");
    std::puts("[PASS] histogram bins, closed last bin, constant and empty inputs, guards");
}

void TestVersionAndCancellation() {
    Fixture fixture;
    const auto revision = ArrowSchemaRevision(*fixture.batch->schema());
    CHECK(revision.size() == 64);
    CHECK(ArrowSchemaRevision(*fixture.batch->schema()) == revision);
    arrow::Int32Builder other;
    other.Append(1);
    std::shared_ptr<arrow::Array> array;
    other.Finish(&array);
    auto changed = arrow::RecordBatch::Make(arrow::schema({arrow::field("v", arrow::int32())}), 1, {array});
    CHECK(ArrowSchemaRevision(*changed->schema()) != revision);

    // An exhausted deadline stops the scan instead of finishing silently.
    try {
        ExecuteDataFrameWorkflow(
            fixture.batch, DescribeArrowSchema(*fixture.batch->schema()),
            Json(R"({"workflow":[{"type":"filter","filters":[{"fid":"f0","rule":{"type":"is not null"}}]},
                                        {"type":"view","query":[{"op":"raw","fields":["f0"]}]}]})"),
            "complete", 10000, 8 << 20, std::chrono::steady_clock::now());
        CHECK(false);
    } catch (const QueryFailure& error) {
        CHECK(error.code == "TIMEOUT");
    }
    CHECK(ErrorCode(fixture.batch, R"([{"type":"pivot"}])") == "UNSUPPORTED_OPERATION");
    std::puts("[PASS] schema revision stability and deadline enforcement");
}
}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    TestProjectionAndSort();
    TestFilters();
    TestAggregates();
    TestTopNAndStability();
    TestDecimals();
    TestBuckets();
    TestHistogram();
    TestVersionAndCancellation();
    if (failures) {
        std::printf("%d assertion(s) failed\n", failures);
        return 1;
    }
    std::puts("=== All DataFrame workflow tests passed ===");
    return 0;
}
