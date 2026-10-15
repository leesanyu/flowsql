// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <channels/kafka/kafka_store.h>
#include <channels/kafka/kafka_structured.h>
#include <cassert>
#include <iostream>
using namespace flowsql;
using namespace flowsql::channels::kafka;
int main() {
    const std::string definition = R"({"encoding":"json","entity":"metrics","fields":[
      {"name":"host","path":"/nested/a~1b","type":"utf8","nullable":false},
      {"name":"time","path":"/time","type":"int64","nullable":false,"unit":"unix_ms"},
      {"name":"u","path":"/u","type":"uint64","nullable":false},
      {"name":"value","path":"/value","type":"float64","nullable":true},
      {"name":"flag","path":"/flag","type":"bool","nullable":true}]})";
    auto config = ParseStructuredConfig(Parse(definition));
    KafkaMessageV1 m;
    m.topic = "metrics";
    m.partition = 2;
    m.offset = 91;
    m.timestamp_ms = 1720000000000;
    m.key_is_null = false;
    m.key = std::string("a\0b", 3);
    m.value = R"({"nested":{"a/b":"host1"},"time":-9223372036854775808,"u":18446744073709551615,"value":2.5})";
    auto result = DecodeStructured(config, {m});
    assert(result.ok());
    auto batch = *result;
    assert(batch->num_rows() == 1 && batch->num_columns() == 10 && batch->ValidateFull().ok());
    assert(std::static_pointer_cast<arrow::StringArray>(batch->column(0))->GetString(0) == "host1");
    assert(std::static_pointer_cast<arrow::Int64Array>(batch->column(1))->Value(0) == INT64_MIN);
    assert(std::static_pointer_cast<arrow::UInt64Array>(batch->column(2))->Value(0) == UINT64_MAX);
    assert(batch->column(4)->IsNull(0));
    assert(batch->schema()->field(1)->metadata()->Get("unit").ValueOrDie() == "unix_ms");
    assert(batch->schema()->metadata()->Get("entity").ValueOrDie() == "metrics");
    assert(std::static_pointer_cast<arrow::Int64Array>(batch->GetColumnByName("__kafka_offset"))->Value(0) == 91);
    assert(std::static_pointer_cast<arrow::BinaryArray>(batch->GetColumnByName("__kafka_key"))->GetString(0) == m.key);
    assert(DecodeStructured(config, {}).ValueOrDie()->num_rows() == 0);
    const auto good = m.value;
    for (const auto& value :
         {"[]", "null", "{}", "{\"time\":1,\"time\":2}", "{\"x\":{\"a\":1,\"a\":2}}", "{\"x\":1} trailing"}) {
        m.value = value;
        assert(!DecodeStructured(config, {m}).ok());
    }
    m.value = good;
    m.value_is_null = true;
    assert(!DecodeStructured(config, {m}).ok());
    m.value_is_null = false;
    m.value = std::string("{\"x\":\"") + char(0xff) + "\"}";
    assert(!DecodeStructured(config, {m}).ok());
    for (const auto& replacement : {"\"7\"", "true", "1.5", "9223372036854775808"}) {
        m.value = good;
        auto at = m.value.find("-9223372036854775808");
        m.value.replace(at, 20, replacement);
        assert(!DecodeStructured(config, {m}).ok());
    }
    m.value = good;
    config.max_message_bytes = 1;
    assert(!DecodeStructured(config, {m}).ok());
    for (
        const auto& change :
        {R"({"encoding":"csv","entity":"x","fields":[]})", R"({"encoding":"json","entity":"x","fields":[]})",
         R"({"encoding":"json","entity":"x","fields":[{"name":"__kafka_x","path":"/x","type":"utf8","nullable":true}]})",
         R"({"encoding":"json","entity":"x","fields":[{"name":"x","path":"/a~2","type":"utf8","nullable":true}]})"}) {
        bool failed = false;
        try {
            ParseStructuredConfig(Parse(change));
        } catch (const std::invalid_argument&) {
            failed = true;
        }
        assert(failed);
    }
    Store store;
    std::array<unsigned char, 32> key{};
    store.Open(":memory:", key);
    store.Mutate("connections", "add",
                 Parse(R"({"name":"local","bootstrap_servers":["localhost:1"],"security_protocol":"PLAINTEXT"})"));
    auto channel = Parse(
        R"({"name":"metrics","connection_name":"local","role":"source","topic":"metrics","consumer_group":"test","payload_kind":"structured","payload_options":{"source":{}}})");
    channel["payload_options"]["source"].CopyFrom(Parse(definition), channel.GetAllocator());
    store.Mutate("channels", "add", std::move(channel));
    auto rows = store.Query("channels", "metrics");
    assert(rows["items"][0]["payload_options"]["source"]["fields"].Size() == 5);
    std::cout << "PASS: strict configuration, JSON typing/null/UTF8/duplicates, Arrow schema and message metadata\n";
}
