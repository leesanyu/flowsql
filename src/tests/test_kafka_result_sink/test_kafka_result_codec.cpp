// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <channels/kafka/kafka_result_codec.h>
#include <channels/kafka/kafka_store.h>
#include <cassert>
#include <cmath>
#include <iostream>
using namespace flowsql::channels::kafka;
template <class F>
void Reject(F f) {
    bool rejected = false;
    try {
        f();
    } catch (const std::exception&) {
        rejected = true;
    }
    assert(rejected);
}
template <class B, class T>
std::shared_ptr<arrow::Array> Array(T value) {
    B b;
    assert(b.Append(value).ok());
    return b.Finish().ValueOrDie();
}
int main() {
    const std::string text =
        R"({"encoding":"json","entity":"alert","schema_version":1,"record_kind":"upsert","key_fields":["id"],"revision_field":"revision"})";
    auto c = ParseSinkConfig(Parse(text));
    for (auto extra : {",\"unknown\":1", ",\"encoding\":\"json\"", ",\"queue_max_messages\":0",
                       ",\"max_message_bytes\":12", ",\"revision_field\":\"id\""}) {
        auto bad = text.substr(0, text.size() - 1) + extra + "}";
        Reject([&] { ParseSinkConfig(Parse(bad)); });
    }
    auto schema =
        arrow::schema({arrow::field("id", arrow::uint64(), false), arrow::field("revision", arrow::uint64(), false),
                       arrow::field("n", arrow::int64()), arrow::field("bin", arrow::binary()),
                       arrow::field("ts", arrow::timestamp(arrow::TimeUnit::NANO, "Asia/Shanghai")),
                       arrow::field("utf8", arrow::utf8())});
    arrow::TimestampBuilder tb(arrow::timestamp(arrow::TimeUnit::NANO, "Asia/Shanghai"), arrow::default_memory_pool());
    assert(tb.Append(INT64_MIN).ok());
    auto batch = arrow::RecordBatch::Make(
        schema, 1,
        {Array<arrow::UInt64Builder>(UINT64_MAX), Array<arrow::UInt64Builder>(UINT64_MAX),
         Array<arrow::Int64Builder>(INT64_MIN), Array<arrow::BinaryBuilder>(std::string("\0\xff", 2)),
         tb.Finish().ValueOrDie(), Array<arrow::StringBuilder>("中文")});
    ResultCodec codec(c, "publication-a", "task");
    codec.Open(schema);
    codec.Validate(*batch);
    auto row = codec.Row(*batch, 0, 0);
    auto v = Parse(row.value);
    assert(v["data"]["id"] == "18446744073709551615");
    assert(v["data"]["n"] == "-9223372036854775808");
    assert(v["data"]["bin"] == "AP8=");
    assert(v["revision"] == "18446744073709551615");
    assert(v["schema"]["fields"][4]["unit"] == "ns");
    assert(v["schema"]["fields"][4]["timezone"] == "Asia/Shanghai");
    ResultCodec other(c, "publication-b", "different");
    other.Open(schema);
    assert(other.Row(*batch, 0, 0).key == row.key);
    assert(other.Fingerprint() == codec.Fingerprint());
    auto changed = batch->ReplaceSchemaMetadata(arrow::key_value_metadata({"z", "a"}, {"2", "1"}));
    Reject([&] { codec.Validate(*changed); });
    other.Open(changed->schema());
    auto fp = other.Fingerprint();
    other.Open(schema->WithMetadata(arrow::key_value_metadata({"a", "z"}, {"1", "2"})));
    assert(fp == other.Fingerprint());
    auto drift = arrow::schema({arrow::field("missing", arrow::uint64())});
    Reject([&] { other.Open(drift); });
    Reject([&] { other.Open(arrow::schema({arrow::field("id", arrow::list(arrow::int64()))})); });
    auto event =
        ParseSinkConfig(Parse(R"({"encoding":"json","entity":"evt","schema_version":1,"record_kind":"event"})"));
    ResultCodec ec(event, "p", "t");
    ec.Open(schema);
    assert(ec.Row(*batch, 0, 0).key != ec.Row(*batch, 0, 1).key);
    assert(Parse(ec.Row(*batch, 0, 0).value)["revision"].IsNull());
    auto nulls =
        batch->SetColumn(0, schema->field(0), arrow::MakeArrayOfNull(arrow::uint64(), 1).ValueOrDie()).ValueOrDie();
    Reject([&] { codec.Validate(*nulls); });
    Reject([&] { codec.Row(*nulls, 0, 0); });
    auto zero = batch->SetColumn(1, schema->field(1), Array<arrow::UInt64Builder>(0)).ValueOrDie();
    Reject([&] { codec.Row(*zero, 0, 0); });
    auto oversized =
        batch->SetColumn(5, schema->field(5), Array<arrow::StringBuilder>(std::string(1100000, 'x'))).ValueOrDie();
    Reject([&] { codec.Row(*oversized, 0, 0); });
    auto invalid =
        batch->SetColumn(5, schema->field(5), Array<arrow::StringBuilder>(std::string("\xff", 1))).ValueOrDie();
    Reject([&] { codec.Row(*invalid, 0, 0); });
    c.schema_version = 2;
    ResultCodec version(c, "p", "t");
    version.Open(schema);
    assert(row.key != version.Row(*batch, 0, 0).key);
    Store::ValidateChannel(Parse(
        R"({"name":"both","connection_name":"conn","role":"both","topic":"topic","consumer_group":"group","payload_kind":"structured","payload_options":{"source":{},"sink":{"encoding":"json","entity":"evt","schema_version":1,"record_kind":"event"}}})"));
    auto scalar_schema = arrow::schema({arrow::field("b", arrow::boolean()), arrow::field("i8", arrow::int8()),
                                        arrow::field("i16", arrow::int16()), arrow::field("i32", arrow::int32()),
                                        arrow::field("u8", arrow::uint8()), arrow::field("u16", arrow::uint16()),
                                        arrow::field("u32", arrow::uint32()), arrow::field("f32", arrow::float32()),
                                        arrow::field("f64", arrow::float64()), arrow::field("null", arrow::utf8())});
    auto scalars = arrow::RecordBatch::Make(
        scalar_schema, 1,
        {Array<arrow::BooleanBuilder>(true), Array<arrow::Int8Builder>(INT8_MIN), Array<arrow::Int16Builder>(INT16_MIN),
         Array<arrow::Int32Builder>(INT32_MIN), Array<arrow::UInt8Builder>(UINT8_MAX),
         Array<arrow::UInt16Builder>(UINT16_MAX), Array<arrow::UInt32Builder>(UINT32_MAX),
         Array<arrow::FloatBuilder>(1.5f), Array<arrow::DoubleBuilder>(-2.5),
         arrow::MakeArrayOfNull(arrow::utf8(), 1).ValueOrDie()});
    ec.Open(scalar_schema);
    ec.Validate(*scalars);
    auto sv = Parse(ec.Row(*scalars, 0, 0).value);
    assert(sv["data"]["b"].GetBool() && sv["data"]["i8"].GetInt() == INT8_MIN &&
           sv["data"]["i16"].GetInt() == INT16_MIN);
    assert(sv["data"]["i32"].GetInt() == INT32_MIN && sv["data"]["u8"].GetUint() == UINT8_MAX &&
           sv["data"]["u16"].GetUint() == UINT16_MAX);
    assert(sv["data"]["u32"].GetUint() == UINT32_MAX && sv["data"]["f32"].GetDouble() == 1.5 &&
           sv["data"]["f64"].GetDouble() == -2.5 && sv["data"]["null"].IsNull());
    auto inf = scalars->SetColumn(8, scalar_schema->field(8), Array<arrow::DoubleBuilder>(INFINITY)).ValueOrDie();
    Reject([&] { ec.Row(*inf, 0, 0); });
    Reject([&] { ec.Open(arrow::schema({arrow::field("x", arrow::int32()), arrow::field("x", arrow::int32())})); });
    auto string_key = arrow::schema({arrow::field("id", arrow::utf8()), arrow::field("revision", arrow::uint64())});
    ResultCodec sk(ParseSinkConfig(Parse(text)), "p", "t");
    sk.Open(string_key);
    auto empty_key =
        arrow::RecordBatch::Make(string_key, 1, {Array<arrow::StringBuilder>(""), Array<arrow::UInt64Builder>(1)});
    Reject([&] { sk.Row(*empty_key, 0, 0); });
    Reject([&] { ec.Open(schema->WithMetadata(arrow::key_value_metadata({"a", "a"}, {"1", "2"}))); });
    Reject([&] {
        ec.Open(arrow::schema(
            {arrow::field("x", arrow::int32(), true, arrow::key_value_metadata({"a", "a"}, {"1", "2"}))}));
    });
    auto fixed_type = arrow::fixed_size_binary(2);
    arrow::FixedSizeBinaryBuilder fixed(fixed_type);
    assert(fixed.Append(std::string("\0\xff", 2)).ok());
    auto fixed_batch =
        arrow::RecordBatch::Make(arrow::schema({arrow::field("fixed", fixed_type)}), 1, {fixed.Finish().ValueOrDie()});
    ec.Open(fixed_batch->schema());
    auto fv = Parse(ec.Row(*fixed_batch, 0, 0).value);
    assert(fv["data"]["fixed"] == "AP8=" && fv["schema"]["fields"][0]["byte_width"].GetInt() == 2);
    for (auto unit : {arrow::TimeUnit::SECOND, arrow::TimeUnit::MILLI, arrow::TimeUnit::MICRO, arrow::TimeUnit::NANO}) {
        auto type = arrow::timestamp(unit, "UTC");
        arrow::TimestampBuilder builder(type, arrow::default_memory_pool());
        assert(builder.Append(INT64_MAX).ok());
        auto time_batch =
            arrow::RecordBatch::Make(arrow::schema({arrow::field("ts", type)}), 1, {builder.Finish().ValueOrDie()});
        ec.Open(time_batch->schema());
        auto tv = Parse(ec.Row(*time_batch, 0, 0).value);
        const char* units[] = {"s", "ms", "us", "ns"};
        assert(tv["schema"]["fields"][0]["unit"] == units[unit]);
        assert(tv["schema"]["fields"][0]["timezone"] == "UTC");
        assert(tv["data"]["ts"] == "9223372036854775807");
    }
    std::cout << "result codec contracts passed\n";
}
