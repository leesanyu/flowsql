// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <channels/config/channel_dictionary.h>
#include <services/database/database_plugin.h>
#include <sqlite3.h>
#include <tests/common/test_environment.h>
#include <cassert>
#include <common/channel_dataset.hpp>
#include <filesystem>
#include <iostream>
using namespace flowsql;
using namespace flowsql::dataset;
using namespace flowsql::database;
using namespace flowsql::channels::config;

std::string Quote(const std::string& s, const std::string& dialect) {
    const char q = dialect == "mysql" || dialect == "clickhouse" ? '`' : '"';
    std::string result(1, q);
    for (char c : s) {
        if (c == q || (c == '\\' && dialect != "postgres" && dialect != "sqlite")) result += c;
        result += c;
    }
    return result + q;
}
Json Describe(DatabasePlugin& p, const Json& locator) {
    ChannelDatasetSnapshotV1 snapshot;
    DataSemanticsErrorV1 error;
    const auto rc = p.Describe({Encode(locator), 5000}, &snapshot, &error);
    if (rc) std::cerr << "describe failed: " << error.code << " " << error.message << std::endl;
    assert(rc == 0 && snapshot.document_json);
    Json d;
    d.Parse(snapshot.document_json->c_str());
    return d;
}
Json Control(ChannelDictionaryService& s, const std::string& op, const Json& request, int expected = 0) {
    std::string response;
    const auto rc = s.Control(op, Encode(request), response);
    if (rc != expected) std::cerr << op << ": " << rc << " " << response << std::endl;
    assert(rc == expected);
    Json d;
    d.Parse(response.c_str());
    return d;
}
void Backend(const std::string& dialect, const std::filesystem::path& root) {
    const auto yaml = (root / (dialect + ".yml")).string();
    std::string options = "type=sqlite;name=test;path=" + (root / "source.db").string();
    if (dialect != "sqlite") options = test::DatabaseOptions(dialect, "test");
    DatabasePlugin plugin;
    assert(plugin.Option(("config_file=" + yaml).c_str()) == 0 && plugin.Start() == 0);
    assert(plugin.AddChannel(options.c_str()) == 0);
    auto channel = plugin.AcquireChannel(dialect.c_str(), "test");
    assert(channel);
    const auto table = "dictionary_" + std::to_string(getpid());
    const auto quoted = Quote(table, dialect);
    const auto definition = dialect == "clickhouse" ? "(id Int64, amount Nullable(Decimal(21,7))) ENGINE=Memory"
                                                    : "(id BIGINT NOT NULL, amount DECIMAL(21,7))";
    assert(channel->ExecuteSql(("CREATE TABLE " + quoted + definition).c_str()) >= 0);
    struct Cleanup {
        std::shared_ptr<IDatabaseChannel> channel;
        std::string sql;
        ~Cleanup() {
            if (channel && !sql.empty()) assert(channel->ExecuteSql(sql.c_str()) >= 0);
        }
    } cleanup{channel, "DROP TABLE " + quoted};
    Json locator(rapidjson::kObjectType);
    auto& a = locator.GetAllocator();
    Set(locator, "kind", "database_table", a);
    Set(locator, "type", dialect, a);
    Set(locator, "name", "test", a);
    Set(locator, "table", table, a);
    auto first = Describe(plugin, locator);
    assert(Get(first, "schema_state") == "known" && first["fields"].Size() == 2);
    assert(Get(first["fields"][0], "name") == "id" && !first["fields"][0]["nullable"].GetBool());
    assert(first["fields"][1]["nullable"].GetBool());
    const auto native = Get(first["fields"][1]["type"], "name");
    assert(native.find("21") != std::string::npos && native.find('7') != std::string::npos);
    assert(Get(first["fields"][1]["type"], "system") == "database");
    assert(Get(first["fields"][1]["type"]["parameters"], "dialect") == dialect);
    assert(Get(first, "identity_state") == (dialect == "postgres" || dialect == "clickhouse" ? "known" : "unknown"));
    Json explicit_locator;
    explicit_locator.CopyFrom(first["locator"], explicit_locator.GetAllocator());
    assert(first["object_key"] == Describe(plugin, explicit_locator)["object_key"]);
    const auto other =
        dialect == "postgres" ? Quote(table + "_schema", dialect) + "." + quoted : Quote(table + "_other", dialect);
    if (dialect == "postgres")
        assert(channel->ExecuteSql(("CREATE SCHEMA " + Quote(table + "_schema", dialect)).c_str()) >= 0);
    const auto other_definition = dialect == "clickhouse" ? "(different Nullable(DateTime64(6,'UTC'))) ENGINE=Memory"
                                  : dialect == "postgres" ? "(different TIMESTAMP(6) WITH TIME ZONE)"
                                  : dialect == "mysql"    ? "(different DATETIME(6))"
                                                          : "(different TIMESTAMP(6))";
    assert(channel->ExecuteSql(("CREATE TABLE " + other + other_definition).c_str()) >= 0);
    Json other_locator;
    other_locator.CopyFrom(locator, other_locator.GetAllocator());
    if (dialect == "postgres")
        Set(other_locator, "schema", table + "_schema", other_locator.GetAllocator());
    else
        Set(other_locator, "table", table + "_other", other_locator.GetAllocator());
    const auto isolated = Describe(plugin, other_locator);
    assert(first["object_key"] != isolated["object_key"] && isolated["fields"].Size() == 1);
    assert(Get(isolated["fields"][0]["type"], "name").find('6') != std::string::npos);
    assert(channel->ExecuteSql(("DROP TABLE " + other).c_str()) >= 0);
    if (dialect == "postgres")
        assert(channel->ExecuteSql(("DROP SCHEMA " + Quote(table + "_schema", dialect)).c_str()) >= 0);
    Json missing;
    missing.CopyFrom(locator, missing.GetAllocator());
    Set(missing, "table", table + "_absent", missing.GetAllocator());
    ChannelDatasetSnapshotV1 out;
    DataSemanticsErrorV1 error;
    assert(plugin.Describe({Encode(missing)}, &out, &error) != 0 && error.code == "NOT_FOUND" && !out.document_json);
    Set(missing, "sql", "SELECT * FROM anything", missing.GetAllocator());
    assert(plugin.Describe({Encode(missing)}, &out, &error) != 0 && error.code == "INVALID_REQUEST");
    DataSemanticsRegistry registry;
    assert(registry.Open((root / (dialect + "-semantics.db")).string(), &error) == 0);
    ChannelDictionaryService service(
        registry, [&](const auto& request, auto* value, auto* e) { return plugin.Describe(request, value, e); });
    Json request(rapidjson::kObjectType);
    request.AddMember("locator", Copy(locator, request.GetAllocator()), request.GetAllocator());
    auto preview = Control(service, "preview", request);
    auto& data = preview["data"];
    Json dictionary;
    dictionary.CopyFrom(data["dictionary"], dictionary.GetAllocator());
    SemanticsSaveRequest save;
    save.kind = "dictionary";
    save.name = dialect + ".table";
    save.document_json = Encode(dictionary);
    SemanticsSaveResult saved;
    assert(registry.Save(save, &saved, &error) == 0);
    Json apply;
    apply.CopyFrom(request, apply.GetAllocator());
    auto& aa = apply.GetAllocator();
    Set(apply, "name", dialect + ".table", aa);
    Set(apply, "dictionary_ref", saved.snapshot.exact_reference, aa);
    apply.AddMember("expected_current_revision", 0, aa);
    apply.AddMember("field_map", Copy(data["field_map"], aa), aa);
    Value basis(rapidjson::kObjectType);
    for (const char* key : {"object_key", "source_revision", "schema_fingerprint"})
        basis.AddMember(Value(key, aa), Copy(data["source_snapshot"][key], aa), aa);
    apply.AddMember("source_basis", basis, aa);
    apply.AddMember("confirmations", Copy(data["required_confirmations"], aa), aa);
    assert(Get(Control(service, "apply", apply)["data"]["check"], "state") == "matched");
    assert(Get(Control(service, "check", request)["data"], "state") == "matched");
    assert(channel->ExecuteSql(("ALTER TABLE " + quoted + " ADD COLUMN extra INTEGER").c_str()) >= 0);
    auto changed = Describe(plugin, locator);
    assert(first["schema_fingerprint"] != changed["schema_fingerprint"] && changed["fields"].Size() == 3);
    const auto check = Control(service, "check", request);
    assert(!check["data"]["differences"].Empty());
    assert(Get(Control(service, "apply", apply, flowsql::error::CONFLICT)["error"], "code") == "SOURCE_CHANGED");
    if (dialect == "sqlite") {
        assert(channel->ExecuteSql(("DROP TABLE " + quoted).c_str()) >= 0);
        assert(channel->ExecuteSql(
                   ("CREATE TABLE " + quoted + "(id BIGINT NOT NULL,amount NUMERIC(25,9) NOT NULL,extra INTEGER)")
                       .c_str()) >= 0);
    } else {
        const auto alteration = dialect == "postgres" ? " ALTER COLUMN amount TYPE NUMERIC(25,9)"
                                : dialect == "mysql"  ? " MODIFY COLUMN amount DECIMAL(25,9) NOT NULL"
                                                      : " MODIFY COLUMN amount Nullable(Decimal(25,9))";
        assert(channel->ExecuteSql(("ALTER TABLE " + quoted + alteration).c_str()) >= 0);
        if (dialect == "postgres")
            assert(channel->ExecuteSql(("ALTER TABLE " + quoted + " ALTER COLUMN amount SET NOT NULL").c_str()) >= 0);
    }
    const auto altered = Describe(plugin, locator);
    assert(Get(altered["fields"][1]["type"], "name").find("25") != std::string::npos);
    assert(altered["fields"][1]["nullable"].GetBool() == (dialect == "clickhouse"));
    assert(channel->ExecuteSql(("ALTER TABLE " + quoted + " DROP COLUMN extra").c_str()) >= 0);
    assert(Describe(plugin, locator)["fields"].Size() == 2);
    const auto drift = Control(service, "check", request);
    bool type_changed = false;
    for (const auto& difference : drift["data"]["differences"].GetArray())
        type_changed = type_changed || Get(difference, "kind") == "type_changed";
    assert(type_changed);
    // Connection identity is restored independently of endpoint and schema fingerprints.
    {
        DatabasePlugin recovered;
        assert(recovered.Option(("config_file=" + yaml).c_str()) == 0 && recovered.Start() == 0);
        assert(first["object_key"] == Describe(recovered, locator)["object_key"]);
        assert(recovered.Stop() == 0);
    }
    // Native IDs distinguish a same-name recreation even when its schema also changes.
    assert(channel->ExecuteSql(("DROP TABLE " + quoted).c_str()) >= 0);
    assert(channel->ExecuteSql(("CREATE TABLE " + quoted + definition).c_str()) >= 0);
    const auto recreated = Describe(plugin, locator);
    if (dialect == "postgres" || dialect == "clickhouse") {
        assert(first["object_key"] != recreated["object_key"]);
        assert(Get(Control(service, "check", request)["data"], "state") == "unbound");
    } else {
        assert(Get(recreated, "identity_state") == "unknown");
    }
    // Remove/recreate is exercised after releasing every channel lease.
    cleanup.channel.reset();
    assert(channel->ExecuteSql(cleanup.sql.c_str()) >= 0);
    cleanup.sql.clear();
    channel.reset();
    assert(plugin.RemoveChannel(dialect.c_str(), "test") == 0);
    assert(plugin.AddChannel(options.c_str()) == 0);
    auto replacement = plugin.AcquireChannel(dialect.c_str(), "test");
    assert(replacement && replacement->ExecuteSql(("CREATE TABLE " + quoted + definition).c_str()) >= 0);
    assert(first["object_key"] != Describe(plugin, locator)["object_key"]);
    assert(replacement->ExecuteSql(("DROP TABLE " + quoted).c_str()) >= 0);
    replacement.reset();
    assert(plugin.Stop() == 0);
    std::cout << "PASS " << dialect << " native empty-table, exact type, binding, drift, restart and identity\n";
}
void SQLiteDeclarations(const std::filesystem::path& root) {
    const auto path = (root / "declarations.db").string();
    sqlite3* db = nullptr;
    assert(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
    assert(sqlite3_exec(db,
                        "CREATE TABLE declared(id INTEGER PRIMARY KEY,untyped);"
                        "CREATE TABLE \"quoted\"\"table\"(value TEXT NOT NULL)",
                        nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(db);
    DatabasePlugin plugin;
    assert(plugin.Option(("type=sqlite;name=declared;path=" + path).c_str()) == 0);
    Json locator;
    locator.Parse(R"({"kind":"database_table","type":"sqlite","name":"declared","table":"declared"})");
    auto incomplete = Describe(plugin, locator);
    assert(Get(incomplete, "schema_state") == "incomplete" && incomplete["schema_fingerprint"].IsNull());
    assert(!incomplete["fields"][0]["nullable"].GetBool() && incomplete["fields"][1]["type"].IsNull());
    assert(!incomplete["structure_gaps"].Empty());
    Set(locator, "table", "quoted\"table", locator.GetAllocator());
    assert(Describe(plugin, locator)["fields"].Size() == 1);
    const auto absent = (root / "absent.db").string();
    assert(plugin.Option(("type=sqlite;name=absent;path=" + absent).c_str()) == 0);
    Set(locator, "name", "absent", locator.GetAllocator());
    ChannelDatasetSnapshotV1 out;
    DataSemanticsErrorV1 error;
    assert(plugin.Describe({Encode(locator)}, &out, &error) != 0 && error.code == "SOURCE_UNAVAILABLE");
    assert(!out.document_json && !std::filesystem::exists(absent));
    Set(locator, "name", "declared", locator.GetAllocator());
    const auto second = (root / "retarget.db").string();
    assert(sqlite3_open(second.c_str(), &db) == SQLITE_OK);
    assert(sqlite3_exec(db, "CREATE TABLE \"quoted\"\"table\"(value TEXT NOT NULL)", nullptr, nullptr, nullptr) ==
           SQLITE_OK);
    sqlite3_close(db);
    // UpdateChannel is a persisted operation: exercise it through the real YAML-backed configuration.
    DatabasePlugin persisted;
    const auto yaml = (root / "retarget.yml").string();
    assert(persisted.Option(("config_file=" + yaml).c_str()) == 0);
    assert(persisted.AddChannel(("type=sqlite;name=declared;path=" + path).c_str()) == 0);
    const auto original = Describe(persisted, locator);
    assert(persisted.UpdateChannel(("type=sqlite;name=declared;path=" + second).c_str()) == 0);
    const auto retargeted = Describe(persisted, locator);
    assert(original["object_key"] == retargeted["object_key"]);
    assert(original["schema_fingerprint"] == retargeted["schema_fingerprint"]);
    assert(original["source_revision"] != retargeted["source_revision"]);
    assert(original["producer_metadata"] != retargeted["producer_metadata"]);
}
int main(int argc, char**) {
    const auto root = std::filesystem::path(test::WorkspacePath("channel-dictionary-database"));
    std::filesystem::create_directories(root);
    Backend("sqlite", root);
    SQLiteDeclarations(root);
    if (argc == 1)
        for (const char* type : {"mysql", "postgres", "clickhouse"}) Backend(type, root);
}
