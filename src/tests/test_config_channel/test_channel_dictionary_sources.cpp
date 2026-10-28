// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <channels/kafka/kafka_plugin.h>
#include <channels/kafka/kafka_structured.h>
#include <framework/core/dataframe_channel.h>
#include <services/catalog/catalog_plugin.h>
#include <services/catalog/lazy_dataframe_channel.h>
#include <unistd.h>
#include <cassert>
#include <common/channel_dataset_arrow.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace flowsql;
using namespace flowsql::dataset;
namespace fs = std::filesystem;
Json Describe(IChannelDatasetDescriptorV1& p, const std::string& locator) {
    ChannelDatasetSnapshotV1 result;
    DataSemanticsErrorV1 e;
    assert(p.Describe({locator, 5000}, &result, &e) == 0);
    assert(result.document_json && e.code.empty());
    Json d;
    d.Parse(result.document_json->c_str());
    return d;
}
std::string Id(const Json& d) { return Canonical(d["object_key"]); }
int main() {
    auto dir = fs::temp_directory_path() / ("flowsql-dataset-" + std::to_string(getpid()));
    fs::create_directories(dir / "frames");
    {
        channels::kafka::Plugin plugin;
        auto& store = plugin.Definitions();
        std::array<unsigned char, 32> key{};
        key.fill(0x34);
        store.Open((dir / "kafka.db").string(), key);
        auto connection =
            R"({"name":"cluster","bootstrap_servers":["unused.invalid:9092"],"security_protocol":"PLAINTEXT"})";
        store.Mutate("connections", "add", channels::kafka::Parse(connection));
        auto channel =
            R"({"name":"events","connection_name":"cluster","role":"both","topic":"events","consumer_group":"analysis","payload_kind":"structured","payload_options":{"source":{"encoding":"json","entity":"event","fields":[{"name":"ts","path":"/time","type":"int64","nullable":false,"unit":"unix_ms"},{"name":"value","path":"/v","type":"float64","nullable":true}]}}})";
        store.Mutate("channels", "add", channels::kafka::Parse(channel));
        auto first = Describe(plugin, R"({"kind":"kafka_source","name":"events"})");
        auto config = channels::kafka::Parse(channel);
        auto schema = channels::kafka::StructuredSchema(
            channels::kafka::ParseStructuredConfig(config["payload_options"]["source"]));
        assert(first["fields"] == ArrowFields(*schema, first.GetAllocator()));
        assert(first["fields"].Size() == 7);
        assert(Get(first["producer_metadata"], "entity") == "event");
        assert(Get(first["producer_metadata"]["fields"]["ts"], "json_pointer") == "/time");
        assert(Get(first["producer_metadata"]["fields"]["ts"]["unit"], "code") == "unix_ms");
        config.AddMember("expected_revision", 1, config.GetAllocator());
        config["payload_options"]["source"]["fields"][0]["path"].SetString("/changed", config.GetAllocator());
        store.Mutate("channels", "modify", channels::kafka::Parse(channels::kafka::Encode(config)));
        auto path_changed = Describe(plugin, R"({"kind":"kafka_source","name":"events"})");
        assert(Id(first) == Id(path_changed));
        assert(first["schema_fingerprint"] == path_changed["schema_fingerprint"]);
        assert(first["source_revision"] != path_changed["source_revision"]);
        config["expected_revision"].SetInt(2);
        config["payload_options"]["source"]["fields"][0]["unit"].SetString("unix_s", config.GetAllocator());
        store.Mutate("channels", "modify", channels::kafka::Parse(channels::kafka::Encode(config)));
        auto unit_changed = Describe(plugin, R"({"kind":"kafka_source","name":"events"})");
        assert(unit_changed["source_revision"] != path_changed["source_revision"]);
        store.Close();
        store.Open((dir / "kafka.db").string(), key);
        assert(Id(unit_changed) == Id(Describe(plugin, R"({"kind":"kafka_source","name":"events"})")));
        store.Mutate("channels", "remove", channels::kafka::Parse(R"({"name":"events","expected_revision":3})"));
        store.Mutate("channels", "add", channels::kafka::Parse(channel));
        assert(Id(first) != Id(Describe(plugin, R"({"kind":"kafka_source","name":"events"})")));
        auto sink = channels::kafka::Parse(
            R"({"name":"sink","connection_name":"cluster","role":"sink","topic":"out","payload_kind":"structured"})");
        store.Mutate("channels", "add", std::move(sink));
        ChannelDatasetSnapshotV1 out;
        DataSemanticsErrorV1 error;
        assert(plugin.Describe({R"({"kind":"kafka_source","name":"sink"})"}, &out, &error) != 0);
        assert(error.code == "UNSUPPORTED_SOURCE" && !out.document_json);
        store.Mutate(
            "channels", "add",
            channels::kafka::Parse(
                R"({"name":"packet","connection_name":"cluster","role":"source","topic":"packets","consumer_group":"p","payload_kind":"packet"})"));
        assert(plugin.Describe({R"({"kind":"kafka_source","name":"packet"})"}, &out, &error) != 0);
        assert(error.code == "UNSUPPORTED_SOURCE");
        // An offline, unresolvable broker was sufficient: no session, secret resolution or offset access was needed.
    }
    std::ofstream(dir / "frames" / "csv.csv") << "id,value\n1,2.5\n";
    auto options = "data_dir=" + (dir / "frames").string() + ";operator_db_dir=" + (dir / "catalog").string();
    std::string csv_id;
    {
        catalog::CatalogPlugin plugin;
        assert(plugin.Option(options.c_str()) == 0);
        assert(plugin.Start() == 0);
        auto pending = Describe(plugin, R"({"kind":"dataframe","name":"csv"})");
        csv_id = Id(pending);
        assert(Get(pending, "schema_state") == "pending" && pending["fields"].IsNull());
        std::ofstream(dir / "upload.csv") << "id,value\n2,7.5\n";
        Json import(rapidjson::kObjectType);
        Set(import, "filename", "uploaded.csv", import.GetAllocator());
        Set(import, "tmp_path", (dir / "upload.csv").string(), import.GetAllocator());
        std::string imported;
        plugin.EnumRoutes([&](const RouteItem& route) {
            if (route.uri == "/channels/dataframe/import")
                assert(route.handler(route.uri, Encode(import), imported) == 0);
        });
        Json import_result;
        import_result.Parse(imported.c_str());
        Json imported_locator(rapidjson::kObjectType);
        Set(imported_locator, "kind", "dataframe", imported_locator.GetAllocator());
        Set(imported_locator, "name", Get(import_result, "name"), imported_locator.GetAllocator());
        auto imported_snapshot = Describe(plugin, Encode(imported_locator));
        assert(Get(imported_snapshot, "schema_state") == "known" && imported_snapshot["fields"].Size() == 2);
        DataFrame loaded;
        assert(dynamic_cast<IDataFrameChannel*>(plugin.Get("csv").get())->Read(&loaded) == 0);
        auto actual = Describe(plugin, R"({"kind":"dataframe","name":"csv"})");
        assert(Id(actual) == csv_id &&
               actual["fields"] == ArrowFields(*loaded.ToArrow()->schema(), actual.GetAllocator()));
        assert(plugin.Rename("csv", "renamed") == 0);
        assert(csv_id == Id(Describe(plugin, R"({"kind":"dataframe","name":"renamed"})")));
        auto frame = std::make_shared<DataFrameChannel>("dataframe", "rich");
        frame->Open();
        auto schema = arrow::schema(
            {arrow::field("ts", arrow::timestamp(arrow::TimeUnit::NANO, "UTC"), false),
             arrow::field("amount", arrow::decimal128(21, 7)),
             arrow::field("items", arrow::list(arrow::struct_({arrow::field("x", arrow::int32(), false)})))});
        std::vector<std::shared_ptr<arrow::Array>> arrays;
        for (const auto& field : schema->fields())
            arrays.push_back(arrow::MakeArrayOfNull(field->type(), 0).ValueOrDie());
        DataFrame data;
        data.FromArrow(arrow::RecordBatch::Make(schema, 0, arrays));
        frame->Write(&data);
        assert(plugin.Register("rich", std::static_pointer_cast<IChannel>(frame)) == 0);
        auto before = Describe(plugin, R"({"kind":"dataframe","name":"rich"})");
        assert(before["fields"] == ArrowFields(*schema, before.GetAllocator()));
        assert(frame->Append(&data) == 0);
        assert(before["source_revision"] ==
               Describe(plugin, R"({"kind":"dataframe","name":"rich"})")["source_revision"]);
        assert(frame->Write(&data) == 0);
        auto replaced = Describe(plugin, R"({"kind":"dataframe","name":"rich"})");
        assert(Id(before) != Id(replaced) && before["schema_fingerprint"] == replaced["schema_fingerprint"]);
        assert(plugin.Unregister("rich") == 0);
        assert(plugin.Register("rich", std::static_pointer_cast<IChannel>(frame)) == 0);
        assert(Id(replaced) != Id(Describe(plugin, R"({"kind":"dataframe","name":"rich"})")));
    }
    {
        catalog::CatalogPlugin plugin;
        plugin.Option(options.c_str());
        assert(plugin.Start() == 0);
        assert(csv_id == Id(Describe(plugin, R"({"kind":"dataframe","name":"renamed"})")));
    }
    int loads = 0;
    catalog::LazyDataFrameChannel lazy((dir / "frames" / "renamed.csv").string(), "lazy",
                                       [&](auto&, auto&, auto&) -> std::shared_ptr<IDataFrameChannel> {
                                           ++loads;
                                           return {};
                                       });
    assert(lazy.DescribeMetadata().pending);
    lazy.Describe();
    lazy.DescribeMetadata();
    assert(loads == 0);
    std::cout << "Kafka and DataFrame dataset contracts passed\n";
    fs::remove_all(dir);
}
