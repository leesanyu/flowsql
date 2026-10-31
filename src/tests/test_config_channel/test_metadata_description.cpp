// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <channels/config/metadata_description.h>
#include <unistd.h>
#include <atomic>
#include <cassert>
#include <common/channel_dataset.hpp>
#include <iostream>
#include <thread>
using namespace flowsql;
using namespace flowsql::dataset;
using namespace flowsql::channels::config;
void TestEditor() {
    char file[] = "/tmp/flowsql-editor-XXXXXX";
    close(mkstemp(file));
    DataSemanticsRegistry registry;
    ConfigChannelProvider configs;
    DataSemanticsErrorV1 e;
    std::string detail;
    assert(!registry.Open(file, &e) && !configs.Open(file, &detail));
    Json locator, fields, metadata(rapidjson::kObjectType);
    locator.Parse(R"({"kind":"dataframe","name":"editor"})");
    fields.Parse(R"([{"name":"value","type":{"system":"arrow","name":"double","parameters":{}},"nullable":true}])");
    auto source = Snapshot("dataframe", "editor-one", locator, fields, metadata, "1");
    MetadataDescriptionService service(registry, configs,
                                       [&](const auto&, auto* out, auto*) {
                                           out->document_json = std::make_shared<const std::string>(Encode(source));
                                           return 0;
                                       },
                                       {});
    auto describe = [&] {
        std::string out;
        assert(
            !service.Describe(R"({"target":{"kind":"channel","locator":{"kind":"dataframe","name":"editor"}}})", &out));
        Json d;
        d.Parse(out.c_str());
        return d;
    };
    auto submit = [&](const Value& basis, const char* changes, bool editor = true, int expected = 0) {
        Json req, edit;
        req.SetObject();
        edit.Parse(changes);
        auto& a = req.GetAllocator();
        req.AddMember("locator", Copy(locator, a), a).AddMember("basis", Copy(basis, a), a);
        Set(req, editor ? "editor_changes" : "changes", Copy(edit, a), a);
        std::string out;
        auto rc = service.SaveChannel(Encode(req).c_str(), &out);
        if (rc != expected) std::cerr << out << '\n';
        assert(rc == expected);
        Json d;
        d.Parse(out.c_str());
        return d;
    };
    auto before = describe();
    auto saved = submit(
        before["data"]["basis"],
        R"({"description":"purpose","row_meaning":"a sample","fields":{"value":{"definition":"first","unit":"ms"}}})");
    assert(Get(saved["data"], "status") == "applied");
    assert(
        Get(submit(
                before["data"]["basis"],
                R"({"description":"purpose","row_meaning":"a sample","fields":{"value":{"definition":"first","unit":"ms"}}})")
                ["data"],
            "binding_ref") == Get(saved["data"], "binding_ref"));
    before = describe();
    const auto id = Get(before["data"]["description"]["document"]["fields"][0], "field_id");
    submit(
        before["data"]["basis"],
        R"({"fields":{"value":{"definition":{"state":"declared","origin":"manual","value":"first"},"unit":{"state":"declared","origin":"imported","value":{"code":"ms","label":"milliseconds"}},"role":{"state":"unknown","origin":"derived","value":null},"null_meaning":{"state":"declared","origin":"producer","value":"not observed"},"measure_kind":{"state":"not_applicable","origin":"manual","value":null}}}})",
        false);
    // A single legacy dictionary contains every hidden semantic property and all four origins.
    SemanticsSaveResult standard, target;
    assert(!registry.Save(
        {"standard", "preserved", R"({"schema_id":"flowsql.data-standard.v1","title":"Preserved standard"})"},
        &standard, &e));
    assert(!registry.Save({"dictionary", "preserved.target", Encode(before["data"]["description"]["document"])},
                          &target, &e));
    Json advanced;
    advanced.Parse(
        R"({"fields":{"value":{"time_semantics":{"state":"declared","origin":"imported","value":{"kind":"instant","unit":{"state":"declared","origin":"imported","value":{"code":"ms","label":"milliseconds"}},"epoch":{"state":"declared","origin":"producer","value":"1970-01-01T00:00:00Z"}}},"validity":{"state":"declared","origin":"producer","value":{"mode":"descriptive","description":"positive samples only","field_ids":[]}},"aggregation":{"state":"declared","origin":"derived","value":[{"operation":"none","condition":{"mode":"descriptive","description":"do not aggregate","field_ids":[]}}]},"value_domain":{"state":"declared","origin":"imported","value":{"kind":"range","min":0,"max":100,"min_inclusive":true,"max_inclusive":true}},"standard_refs":{"state":"declared","origin":"imported","value":[]}}},"entity":{"row_semantics":{"state":"declared","origin":"producer","value":"event"},"identity_fields":{"state":"declared","origin":"imported","value":[]},"revision_field":{"state":"declared","origin":"manual","value":"placeholder"},"time_fields":{"state":"declared","origin":"derived","value":[]},"relations":{"state":"declared","origin":"imported","value":[]},"limitations":{"state":"declared","origin":"manual","value":["Only sampled requests are included"]}}})");
    assert(!advanced.HasParseError());
    auto& aa = advanced.GetAllocator();
    for (const auto* key : {"identity_fields", "time_fields"})
        advanced["entity"][key]["value"].PushBack(Value(id.c_str(), aa), aa);
    Set(advanced["entity"]["revision_field"], "value", id, aa);
    auto& field = advanced["fields"]["value"];
    field["standard_refs"]["value"].PushBack(Value(standard.snapshot.exact_reference.c_str(), aa), aa);
    field["validity"]["value"]["field_ids"].PushBack(Value(id.c_str(), aa), aa);
    field["aggregation"]["value"][0]["condition"]["field_ids"].PushBack(Value(id.c_str(), aa), aa);
    Value relation(rapidjson::kObjectType), mapping(rapidjson::kObjectType);
    Set(mapping, id.c_str(), id, aa);
    Set(relation, "dictionary_ref", target.snapshot.exact_reference, aa);
    Set(relation, "field_map", std::move(mapping), aa);
    advanced["entity"]["relations"]["value"].PushBack(std::move(relation), aa);
    before = describe();
    submit(before["data"]["basis"], Encode(advanced).c_str(), false);
    before = describe();
    auto changed = submit(before["data"]["basis"], R"({"fields":{"value":{"definition":"second"}}})");
    auto current = describe();
    auto expected = Copy(before["data"]["description"]["document"], before.GetAllocator());
    Set(expected["fields"][0]["definition"], "value", "second", before.GetAllocator());
    assert(current["data"]["description"]["document"] == expected);
    assert(Get(current["data"]["description"]["document"]["fields"][0], "field_id") == id);
    submit(before["data"]["basis"], R"({"description":"stale"})", true, error::CONFLICT);
    submit(current["data"]["basis"], R"({"fields":{"absent":{"definition":"x"}}})", true, error::CONFLICT);
    for (const char* invalid :
         {R"({"fields":{"value":{"type":{}}}})", R"({"unknown":1})", R"({"fields":{"value":{"unit":{}}}})"})
        submit(current["data"]["basis"], invalid, true, error::BAD_REQUEST);
    Json both;
    both.Parse(R"({"locator":{"kind":"dataframe","name":"editor"},"changes":{},"editor_changes":{}})");
    both.AddMember("basis", Copy(current["data"]["basis"], both.GetAllocator()), both.GetAllocator());
    std::string out;
    assert(service.SaveChannel(Encode(both).c_str(), &out) == error::BAD_REQUEST);
    both.RemoveMember("changes");
    both.RemoveMember("editor_changes");
    assert(service.SaveChannel(Encode(both).c_str(), &out) == error::BAD_REQUEST);
    saved = submit(
        current["data"]["basis"],
        R"({"description":null,"row_meaning":" \t ","fields":{"value":{"definition":"\n ","unit":null,"time":null}}})");
    registry.Close();
    configs.Close();
    assert(!registry.Open(file, &e) && !configs.Open(file, &detail));
    current = describe();
    assert(current["data"]["basis"] == saved["data"]["basis"]);
    for (const auto* key : {"definition", "unit"}) {
        const auto& v = current["data"]["description"]["document"]["fields"][0][key];
        assert(Get(v, "state") == "unknown" && v["value"].IsNull() && Get(v, "origin") == "manual");
    }
    assert(Get(current["data"]["description"]["document"]["description"], "state") == "unknown");
    assert(Get(current["data"]["description"]["document"]["entity"]["row_meaning"], "state") == "unknown");
    registry.Close();
    configs.Close();
    unlink(file);
}
void TestTimeEditor() {
    char file[] = "/tmp/flowsql-time-editor-XXXXXX";
    close(mkstemp(file));
    DataSemanticsRegistry registry;
    ConfigChannelProvider configs;
    DataSemanticsErrorV1 e;
    std::string detail;
    assert(!registry.Open(file, &e) && !configs.Open(file, &detail));
    Json locator, fields(rapidjson::kArrayType), metadata;
    locator.Parse(R"({"kind":"dataframe","name":"clock"})");
    auto& a = fields.GetAllocator();
    for (const auto* name : {"s", "ms", "us", "ns", "elapsed", "ordinary", "producer"})
        fields.PushBack(Field(name, Type("arrow", "int64", a), true, a), a);
    for (const auto& item :
         std::vector<std::pair<std::string, std::string>>{{"native", "timestamp[us, tz=Asia/Shanghai]"},
                                                          {"naive", "timestamp[ns]"},
                                                          {"duration", "duration[ms]"},
                                                          {"text", "string"},
                                                          {"legacy", "double"}})
        fields.PushBack(Field(item.first, Type("arrow", item.second, a), true, a), a);
    fields.PushBack(Field("date", Type("database", "TIMESTAMP", a), true, a), a);
    fields.PushBack(Field("ambiguous", Type("database", "TIME", a), true, a), a);
    metadata.Parse(
        R"({"fields":{"producer":{"unit":{"code":"ms","label":"ms"},"time_semantics":{"kind":"instant","unit":{"state":"declared","origin":"producer","value":{"code":"ms","label":"ms"}},"epoch":{"state":"declared","origin":"producer","value":"1970-01-01T00:00:00Z"}}}}})");
    auto source = Snapshot("dataframe", "clock-one", locator, fields, metadata, "1");
    MetadataDescriptionService service(registry, configs,
                                       [&](const auto&, auto* out, auto*) {
                                           out->document_json = std::make_shared<const std::string>(Encode(source));
                                           return 0;
                                       },
                                       {});
    auto describe = [&] {
        std::string out;
        assert(
            !service.Describe(R"({"target":{"kind":"channel","locator":{"kind":"dataframe","name":"clock"}}})", &out));
        Json d;
        d.Parse(out.c_str());
        return d;
    };
    auto save = [&](const Value& basis, const std::string& edits, bool editor = true, int expected = 0) {
        Json req(rapidjson::kObjectType), edit;
        edit.Parse(edits.c_str());
        assert(!edit.HasParseError());
        auto& ra = req.GetAllocator();
        Set(req, "locator", Copy(locator, ra), ra);
        Set(req, "basis", Copy(basis, ra), ra);
        Set(req, editor ? "editor_changes" : "changes", Copy(edit, ra), ra);
        std::string out;
        const auto rc = service.SaveChannel(Encode(req).c_str(), &out);
        if (rc != expected) std::cerr << out << '\n';
        assert(rc == expected);
        Json result;
        result.Parse(out.c_str());
        return result;
    };
    auto field = [](const Json& d, const std::string& name) -> const Value& {
        for (const auto& f : d["data"]["description"]["document"]["fields"].GetArray())
            if (Get(f, "name") == name) return f;
        std::abort();
    };
    auto before = describe();
    assert(before["data"]["basis"]["dictionary_ref"].IsNull());
    assert(before["data"]["context"].HasMember("editor_fields"));
    const auto& hints = before["data"]["context"]["editor_fields"];
    assert(Get(hints["native"]["time_semantics"]["value"]["timezone"], "value") == "Asia/Shanghai");
    assert(Get(hints["naive"]["time_semantics"]["value"]["timezone"], "state") == "unknown");
    assert(hints["ambiguous"]["modes"].Empty() && hints["ordinary"]["time_semantics"].IsNull());
    assert(describe()["data"]["basis"]["dictionary_ref"].IsNull());
    auto saved = save(
        before["data"]["basis"],
        R"({"fields":{"s":{"time":{"mode":"unix_timestamp","unit":"s"}},"ms":{"time":{"mode":"unix_timestamp","unit":"ms"}},"us":{"time":{"mode":"unix_timestamp","unit":"us"}},"ns":{"time":{"mode":"unix_timestamp","unit":"ns"}},"elapsed":{"time":{"mode":"duration","unit":"us"}},"date":{"time":{"mode":"datetime"}},"ordinary":{"unit":"ms"}}})");
    auto current = describe();
    for (const auto* unit : {"s", "ms", "us", "ns"}) {
        const auto& f = field(current, unit);
        const auto& time = f["time_semantics"];
        assert(Get(time, "origin") == "manual" && Get(time["value"], "kind") == "instant");
        assert(Get(time["value"]["epoch"], "value") == "1970-01-01T00:00:00Z");
        assert(time["value"]["unit"] == f["unit"] && Get(f["unit"]["value"], "code") == unit);
    }
    assert(Get(field(current, "ordinary")["time_semantics"], "state") == "unknown");
    assert(Get(field(current, "date")["time_semantics"]["value"]["timezone"], "state") == "unknown");
    assert(Get(field(current, "native")["time_semantics"], "origin") == "derived");
    assert(Get(field(current, "producer")["time_semantics"], "origin") == "producer");
    const auto& elapsed = field(current, "elapsed");
    assert(Get(elapsed["time_semantics"]["value"], "kind") == "duration");
    assert(Get(elapsed["time_semantics"]["value"]["epoch"], "state") == "unknown");
    const auto elapsed_id = Get(elapsed, "field_id");
    const auto& times = current["data"]["description"]["document"]["entity"]["time_fields"]["value"];
    assert(times.IsArray());
    for (const auto& id : times.GetArray())
        assert(Text(id) != elapsed_id && Text(id) != Get(field(current, "duration"), "field_id"));
    for (const auto* invalid :
         {R"({"fields":{"s":{"time":{"mode":"unix_timestamp","unit":"ms"},"unit":"s"}}})",
          R"({"fields":{"s":{"unit":"ms"}}})", R"({"fields":{"text":{"time":{"mode":"unix_timestamp","unit":"s"}}}})",
          R"({"fields":{"ordinary":{"time":{"mode":"datetime"}}}})",
          R"({"fields":{"date":{"time":{"mode":"duration","unit":"s"}}}})",
          R"({"fields":{"s":{"time":{"mode":"unix_timestamp","unit":"s","timezone":"UTC"}}}})",
          R"({"fields":{"date":{"time":{"mode":"datetime","unit":"ms"}}}})"})
        save(current["data"]["basis"], invalid, true, error::BAD_REQUEST);
    save(
        current["data"]["basis"],
        R"({"fields":{"legacy":{"time_semantics":{"state":"declared","origin":"imported","value":{"kind":"instant","unit":{"state":"declared","origin":"imported","value":{"code":"minute","label":"minute"}},"epoch":{"state":"declared","origin":"imported","value":"custom epoch"}}}}}})",
        false);
    current = describe();
    const auto legacy = Canonical(field(current, "legacy")["time_semantics"]);
    save(current["data"]["basis"], R"({"fields":{"legacy":{"definition":"text only"}}})");
    current = describe();
    assert(Canonical(field(current, "legacy")["time_semantics"]) == legacy);
    // Changing one time cannot rewrite an unrelated summary member or its origin.
    auto summary = Copy(current["data"]["description"]["document"]["entity"]["time_fields"], current.GetAllocator());
    Set(summary, "origin", "imported", current.GetAllocator());
    summary["value"].PushBack(Value(Get(field(current, "ordinary"), "field_id").c_str(), current.GetAllocator()),
                              current.GetAllocator());
    Json edit(rapidjson::kObjectType);
    Set(edit, "entity", Value(rapidjson::kObjectType), edit.GetAllocator());
    Set(edit["entity"], "time_fields", Copy(summary, edit.GetAllocator()), edit.GetAllocator());
    save(current["data"]["basis"], Encode(edit), false);
    current = describe();
    save(current["data"]["basis"],
         R"({"fields":{"s":{"time":null},"native":{"time":{"mode":"datetime","timezone":"Europe/Paris"}}}})");
    current = describe();
    assert(Get(field(current, "s")["unit"]["value"], "code") == "s");
    assert(Get(field(current, "s")["time_semantics"], "state") == "unknown");
    assert(current["data"]["context"]["editor_fields"]["native"]["conflict"].GetBool());
    saved = save(current["data"]["basis"], R"({"description":"unrelated"})");
    current = describe();
    assert(Get(field(current, "native")["time_semantics"]["value"]["timezone"], "value") == "Europe/Paris");
    const auto& preserved = current["data"]["description"]["document"]["entity"]["time_fields"];
    assert(Get(preserved, "origin") == "imported");
    bool other = false;
    for (const auto& id : preserved["value"].GetArray())
        other |= Text(id) == Get(field(current, "ordinary"), "field_id");
    assert(other);
    saved = save(current["data"]["basis"], R"({"fields":{"native":{"time":null}}})");
    registry.Close();
    configs.Close();
    assert(!registry.Open(file, &e) && !configs.Open(file, &detail));
    current = describe();
    assert(current["data"]["basis"] == saved["data"]["basis"]);
    assert(Get(field(current, "native")["time_semantics"]["value"]["timezone"], "value") == "Asia/Shanghai");
    assert(Get(field(current, "native")["time_semantics"], "origin") == "derived");
    registry.Close();
    configs.Close();
    unlink(file);
}

void TestRelationEditor() {
    char file[] = "/tmp/flowsql-relation-editor-XXXXXX";
    close(mkstemp(file));
    DataSemanticsRegistry registry;
    ConfigChannelProvider configs;
    DataSemanticsErrorV1 e;
    std::string detail;
    assert(!registry.Open(file, &e) && !configs.Open(file, &detail));
    Json target;
    target.Parse(
        R"({"schema_id":"flowsql.data-dictionary.v1","title":"Target entity","entity":{},"fields":[{"field_id":"a","name":"target_a","title":"A","type":{"system":"arrow","name":"int64","parameters":{}},"nullable":true},{"field_id":"b","name":"target_b","title":"B","type":{"system":"arrow","name":"int64","parameters":{}},"nullable":true}]})");
    SemanticsSaveResult saved_target;
    assert(!registry.Save({"dictionary", "target", Encode(target)}, &saved_target, &e));
    const auto exact = saved_target.snapshot.exact_reference;
    Json locator, fields, metadata(rapidjson::kObjectType);
    locator.Parse(R"({"kind":"dataframe","name":"source"})");
    fields.Parse(
        R"([{"name":"first","type":{"system":"arrow","name":"int64","parameters":{}},"nullable":true},{"name":"second","type":{"system":"arrow","name":"int64","parameters":{}},"nullable":true}])");
    auto source = Snapshot("dataframe", "relations-one", locator, fields, metadata, "1");
    MetadataDescriptionService service(registry, configs,
                                       [&](const auto&, auto* out, auto*) {
                                           out->document_json = std::make_shared<const std::string>(Encode(source));
                                           return 0;
                                       },
                                       {});
    auto describe = [&] {
        std::string out;
        assert(
            !service.Describe(R"({"target":{"kind":"channel","locator":{"kind":"dataframe","name":"source"}}})", &out));
        Json d;
        d.Parse(out.c_str());
        return d;
    };
    auto submit = [&](const Value& basis, const std::string& input, int expected = 0) {
        Json req(rapidjson::kObjectType), edits;
        edits.Parse(input.c_str());
        assert(!edits.HasParseError());
        auto& a = req.GetAllocator();
        Set(req, "locator", Copy(locator, a), a);
        Set(req, "basis", Copy(basis, a), a);
        Set(req, "editor_changes", Copy(edits, a), a);
        std::string out;
        const auto rc = service.SaveChannel(Encode(req).c_str(), &out);
        if (rc != expected) std::cerr << out << '\n';
        assert(rc == expected);
        Json d;
        d.Parse(out.c_str());
        return d;
    };
    const auto edits = std::string(R"({"relations":[{"dictionary_ref":")") + exact +
                       R"(","field_map":{"first":"a"}},{"dictionary_ref":")" + exact +
                       R"(","field_map":{"first":"a","second":"b"}}]})";
    auto before = describe();
    auto saved = submit(before["data"]["basis"], edits);
    assert(Get(saved["data"], "status") == "applied");
    assert(Get(submit(before["data"]["basis"], edits)["data"], "binding_ref") == Get(saved["data"], "binding_ref"));
    auto current = describe();
    const auto& document = current["data"]["description"]["document"];
    const auto first = Get(document["fields"][0], "field_id"), second = Get(document["fields"][1], "field_id");
    const auto& relations = document["entity"]["relations"];
    assert(Get(relations, "origin") == "manual" && relations["value"].Size() == 2);
    assert(Get(relations["value"][0]["field_map"], first.c_str()) == "a");
    assert(Get(relations["value"][1]["field_map"], second.c_str()) == "b");
    const auto original_relations = Canonical(relations);
    // Moving the target's current revision cannot rewrite an exact historical relation.
    Set(target, "title", "New target title", target.GetAllocator());
    assert(!registry.Save({"dictionary", "target", Encode(target), "", 1}, &saved_target, &e));
    assert(saved_target.snapshot.exact_reference != exact);
    submit(current["data"]["basis"], R"({"fields":{"first":{"definition":"unrelated"}}})");
    current = describe();
    assert(Canonical(current["data"]["description"]["document"]["entity"]["relations"]) == original_relations);
    for (const auto& mapping : {R"({"first":"a","second":"a"})", R"({"first":"absent"})", R"({"missing":"a"})"}) {
        const auto invalid =
            std::string(R"({"relations":[{"dictionary_ref":")") + exact + R"(","field_map":)" + mapping + "}]}";
        const auto result = submit(current["data"]["basis"], invalid, error::BAD_REQUEST);
        assert(Get(result["error"], "field_path").find("relations") != std::string::npos);
    }
    submit(current["data"]["basis"],
           R"({"relations":[{"dictionary_ref":"dictionary.absent@1","field_map":{"first":"a"}}]})", error::NOT_FOUND);
    assert(Canonical(describe()["data"]["description"]["document"]["entity"]["relations"]) == original_relations);
    DataSemanticsSnapshotV1 historical;
    assert(!registry.Resolve(exact.c_str(), &historical, &e));
    Json old;
    old.Parse(historical.document_json->c_str());
    assert(Get(old, "title") == "Target entity");
    registry.Close();
    configs.Close();
    assert(!registry.Open(file, &e) && !configs.Open(file, &detail));
    current = describe();
    assert(Canonical(current["data"]["description"]["document"]["entity"]["relations"]) == original_relations);
    submit(current["data"]["basis"], R"({"relations":[]})");
    assert(describe()["data"]["description"]["document"]["entity"]["relations"]["value"].Empty());
    registry.Close();
    configs.Close();
    unlink(file);
}

int main() {
    TestEditor();
    TestTimeEditor();
    TestRelationEditor();
    char file[] = "/tmp/flowsql-description-XXXXXX";
    int fd = mkstemp(file);
    assert(fd >= 0);
    close(fd);
    DataSemanticsRegistry registry;
    ConfigChannelProvider configs;
    DataSemanticsErrorV1 e;
    std::string detail;
    assert(!registry.Open(file, &e));
    assert(!configs.Open(file, &detail));
    Json locator;
    locator.Parse(R"({"kind":"dataframe","name":"input"})");
    Json fields(rapidjson::kArrayType), metadata(rapidjson::kObjectType);
    fields.PushBack(Field("value", Type("arrow", "double", fields.GetAllocator()), true, fields.GetAllocator()),
                    fields.GetAllocator());
    auto source = Snapshot("dataframe", "identity", locator, fields, metadata, "1");
    std::atomic<int> calls{0};
    int fail_on_call = 0;
    std::string object_identity = "identity";
    bool changed = false, slow = false, offline = false;
    MetadataDescriptionService service(registry, configs,
                                       [&](const auto&, auto* out, auto* err) {
                                           ++calls;
                                           if (offline || calls == fail_on_call)
                                               return Fail(err, "SOURCE_UNAVAILABLE", "injected");
                                           if (slow) std::this_thread::sleep_for(std::chrono::milliseconds(5));
                                           auto copy = Snapshot("dataframe", object_identity, locator, fields, metadata,
                                                                changed && calls % 2 == 0 ? "2" : "1", "known",
                                                                Get(source, "schema_state"));
                                           out->document_json = std::make_shared<const std::string>(Encode(copy));
                                           return 0;
                                       },
                                       {});
    auto call = [&](const std::string& input, int status = 0) {
        std::string response;
        int rc = service.Describe(input.c_str(), &response);
        if (rc != status) std::cerr << response << '\n';
        assert(rc == status);
        Json d;
        d.Parse(response.c_str());
        assert(d.IsObject());
        return d;
    };
    const auto input = R"({"target":{"kind":"channel","locator":{"kind":"dataframe","name":"input"}}})";
    auto d = call(input);
    assert(d["data"]["structure"]["fields"].Size() == 1);
    assert(Get(d["data"]["description"], "state") == "missing");
    assert(Get(d["data"]["checks"], "state") == "unbound");
    DataSemanticsSnapshotV1 absent;
    assert(registry.BindingLookup("dataframe", "identity", &absent, &e) != 0);
    assert(calls == 2);
    Set(source, "schema_state", "pending", source.GetAllocator());
    d = call(input);
    assert(Get(d["data"]["structure"], "state") == "pending");
    Set(source, "schema_state", "known", source.GetAllocator());
    changed = true;
    d = call(input, error::CONFLICT);
    assert(Get(d["error"], "code") == "SOURCE_CHANGED");
    changed = false;
    slow = true;
    d = call(R"({"target":{"kind":"channel","locator":{"kind":"dataframe","name":"input"}},"timeout_ms":1})",
             error::TIMEOUT);
    slow = false;
    offline = true;
    call(input, error::UNAVAILABLE);
    offline = false;
    call(R"({"target":{"kind":"channel","locator":{"kind":"dataframe","name":"input"}},"sql":"select 1"})",
         error::BAD_REQUEST);

    // Object-local editing, durable storage, stable identities and exact retry behavior.
    auto semantic = [](const std::string& value) {
        Json v(rapidjson::kObjectType);
        auto& a = v.GetAllocator();
        Set(v, "state", "declared", a);
        Set(v, "origin", "manual", a);
        Set(v, "value", value, a);
        return v;
    };
    auto command = [&](const Value& basis, const std::string& value) {
        Json r(rapidjson::kObjectType);
        auto& a = r.GetAllocator();
        r.AddMember("locator", Copy(locator, a), a)
            .AddMember("basis", Copy(basis, a), a)
            .AddMember("changes", Value(rapidjson::kObjectType), a);
        r["changes"].AddMember("fields", Value(rapidjson::kObjectType), a);
        Value f(rapidjson::kObjectType);
        f.AddMember("definition", Copy(semantic(value), a), a);
        r["changes"]["fields"].AddMember("value", f, a);
        return r;
    };
    auto save = [&](const Json& r, int status = 0) {
        std::string response;
        const auto rc = service.SaveChannel(Encode(r).c_str(), &response);
        if (rc != status) std::cerr << response << '\n';
        assert(rc == status);
        Json result;
        result.Parse(response.c_str());
        return result;
    };
    d = call(input);
    auto request = command(d["data"]["basis"], "first meaning");
    auto saved = save(request);
    assert(Get(saved["data"], "status") == "applied");
    const auto first_ref = Get(saved["data"], "dictionary_ref"), first_binding = Get(saved["data"], "binding_ref");
    assert(Get(save(request)["data"], "binding_ref") == first_binding);
    registry.Close();
    configs.Close();
    assert(!registry.Open(file, &e));
    assert(!configs.Open(file, &detail));
    d = call(input);
    assert(Get(d["data"]["description"]["document"]["fields"][0]["definition"], "value") == "first meaning");
    const auto field_id = Get(d["data"]["description"]["document"]["fields"][0], "field_id");
    // Two objects intentionally share one dictionary; saving the first cannot revise that shared definition.
    DataSemanticsSnapshotV1 binding_snapshot;
    assert(!registry.Resolve(first_binding.c_str(), &binding_snapshot, &e));
    Json shared;
    shared.Parse(binding_snapshot.document_json->c_str());
    Set(shared["object_key"], "object_id", "second", shared.GetAllocator());
    Set(shared["source_snapshot"]["object_key"], "object_id", "second", shared.GetAllocator());
    SemanticsSaveRequest share{"binding", "shared.second", Encode(shared)};
    share.source_observed = true;
    SemanticsSaveResult shared_result;
    assert(!registry.Save(share, &shared_result, &e));
    request = command(d["data"]["basis"], "new meaning");
    request.RemoveMember("changes");
    Json local_edit;
    local_edit.Parse(R"({"fields":{"value":{"definition":"new meaning"}}})");
    request.AddMember("editor_changes", Copy(local_edit, request.GetAllocator()), request.GetAllocator());
    saved = save(request);
    assert(Get(saved["data"], "dictionary_ref") != first_ref);
    d = call(input);
    assert(Get(d["data"]["description"]["document"]["fields"][0], "field_id") == field_id);
    object_identity = "second";
    auto second = call(input);
    assert(Get(second["data"]["basis"], "dictionary_ref") == first_ref);
    assert(Get(second["data"]["description"]["document"]["fields"][0]["definition"], "value") == "first meaning");
    object_identity = "identity";
    auto stale = command(request["basis"], "stale meaning");
    save(stale, error::CONFLICT);
    // Partial success carries the exact durable definition, and retry reuses it.
    request = command(d["data"]["basis"], "retry meaning");
    fail_on_call = calls + 2;
    auto partial = save(request);
    assert(Get(partial["data"], "status") == "definition_saved_not_applied");
    const auto partial_ref = Get(partial["data"], "dictionary_ref");
    DataSemanticsSnapshotV1 persisted;
    assert(!registry.Resolve(partial_ref.c_str(), &persisted, &e));
    fail_on_call = 0;
    assert(Get(save(request)["data"], "dictionary_ref") == partial_ref);
    d = call(input);
    auto invalid = command(d["data"]["basis"], "must not save");
    invalid["changes"]["fields"]["value"].AddMember("type", Value(rapidjson::kObjectType), invalid.GetAllocator());
    save(invalid, error::BAD_REQUEST);
    invalid = command(d["data"]["basis"], "must not save");
    Json missing_ref;
    missing_ref.Parse(R"({"state":"declared","origin":"manual","value":["standard.absent@1"]})");
    invalid["changes"]["fields"]["value"].AddMember("standard_refs", Copy(missing_ref, invalid.GetAllocator()),
                                                    invalid.GetAllocator());
    save(invalid, error::NOT_FOUND);
    assert(Get(call(input)["data"]["basis"], "binding_ref") == Get(d["data"]["basis"], "binding_ref"));
    // Concurrent different submissions cannot both replace the captured binding.
    auto one = command(d["data"]["basis"], "concurrent one"), two = command(d["data"]["basis"], "concurrent two");
    std::atomic<int> applied{0};
    auto writer = [&](const Json& r) {
        std::string out;
        int rc = service.SaveChannel(Encode(r).c_str(), &out);
        Json v;
        v.Parse(out.c_str());
        if (!rc && Get(v["data"], "status") == "applied")
            ++applied;
        else
            assert(rc == error::CONFLICT || (!rc && Get(v["data"], "status") == "definition_saved_not_applied"));
    };
    std::thread t1(writer, std::cref(one)), t2(writer, std::cref(two));
    t1.join();
    t2.join();
    assert(applied == 1);
    // Source changes are never silently applied, including same-name replacement.
    object_identity = "replacement";
    save(one, error::CONFLICT);
    object_identity = "identity";
    Set(source, "schema_state", "pending", source.GetAllocator());
    save(one, error::BAD_REQUEST);
    Set(source, "schema_state", "known", source.GetAllocator());

    // Real structural differences need explicit confirmation; adaptation keeps existing identities.
    fields[0]["type"]["name"].SetString("int64", fields.GetAllocator());
    fields.PushBack(Field("other", Type("arrow", "double", fields.GetAllocator()), true, fields.GetAllocator()),
                    fields.GetAllocator());
    d = call(input);
    request = command(d["data"]["basis"], "confirmed meaning");
    save(request, error::CONFLICT);
    request.AddMember("confirmations", Copy(d["data"]["checks"]["required_confirmations"], request.GetAllocator()),
                      request.GetAllocator());
    assert(!request["confirmations"].Empty());
    assert(Get(save(request)["data"], "status") == "applied");
    fields.Erase(fields.Begin());
    d = call(input);
    request = command(d["data"]["basis"], "removed field");
    request["changes"].SetObject();
    save(request, error::CONFLICT);
    request.AddMember("confirmations", Copy(d["data"]["checks"]["required_confirmations"], request.GetAllocator()),
                      request.GetAllocator());
    assert(Get(save(request)["data"], "status") == "applied");
    assert(call(input)["data"]["description"]["document"]["fields"].Size() == 1);
    OperatorComponentContractV1 association{"test", "operator", "component-one", "1", "", "missing"};
    bool replace_component = false;
    int lookups = 0;
    MetadataDescriptionService operators(registry, configs, {}, [&](const auto&, uint32_t ms, auto* out, auto*) {
        assert(ms > 0);
        ++lookups;
        *out = association;
        if (replace_component && lookups % 2 == 0) out->component_revision = "component-two";
        return 0;
    });
    const char* op = R"({"target":{"kind":"operator","name":"test.operator"}})";
    std::string response;
    assert(!operators.Describe(op, &response));
    Json result;
    result.Parse(response.c_str());
    assert(Get(result["data"]["contract"], "state") == "missing");
    assert(Get(result["data"]["basis"], "component_revision") == "component-one");

    auto save_notes = [&](const std::string& purpose, const std::string& expected, int status = 0) {
        Json request(rapidjson::kObjectType);
        auto& a = request.GetAllocator();
        Set(request, "name", "test.operator", a);
        Set(request, "expected_component_revision", "component-one", a);
        request.AddMember("expected_description_ref", expected.empty() ? Value() : Value(expected.c_str(), a), a);
        Value notes(rapidjson::kObjectType);
        Set(notes, "purpose", purpose, a);
        Set(notes, "usage_notes", "test notes", a);
        notes.AddMember("examples", Value(rapidjson::kArrayType), a);
        request.AddMember("notes", notes, a);
        const int rc = operators.SaveOperator(Encode(request).c_str(), &response);
        if (rc != status) std::cerr << response << '\n';
        assert(rc == status);
        Json out;
        out.Parse(response.c_str());
        return out;
    };
    auto note = save_notes("purpose one", "");
    const auto note_ref = Get(note["data"], "description_ref");
    assert(note["data"]["created_revision"].GetBool());
    assert(Get(save_notes("purpose one", "")["data"], "description_ref") == note_ref);
    assert(!save_notes("purpose one", note_ref)["data"]["created_revision"].GetBool());
    registry.Close();
    configs.Close();
    assert(!registry.Open(file, &e));
    assert(!configs.Open(file, &detail));
    assert(!operators.Describe(op, &response));
    result.Parse(response.c_str());
    assert(Get(result["data"]["description"]["notes"], "purpose") == "purpose one");
    assert(Get(result["data"]["description"], "origin") == "local_notes");
    assert(Get(result["data"]["contract"], "state") == "missing");
    save_notes("stale purpose", "", error::CONFLICT);
    save_notes("purpose one", "config.wrong@1", error::CONFLICT);
    auto note_two = save_notes("purpose two", note_ref);
    save_notes("outdated", note_ref, error::CONFLICT);
    association.component_revision = "component-two";
    save_notes("old component", Get(note_two["data"], "description_ref"), error::CONFLICT);
    assert(!operators.Describe(op, &response));
    result.Parse(response.c_str());
    assert(result["data"]["description"]["notes"].IsNull() && result["data"]["basis"]["description_ref"].IsNull());
    association.component_revision = "component-one";
    // A damaged current resource must report storage failure, never silently become missing notes.
    const auto name = "n" + Hash("test\noperator\ncomponent-one").substr(0, 63);
    ConfigPublishRequest damaged;
    damaged.name = name;
    damaged.expected_current_revision = 2;
    damaged.format = "json";
    damaged.schema_id = "wrong.schema";
    damaged.content = "{}";
    ConfigPublishResult damaged_result;
    assert(!configs.Publish(damaged, &damaged_result, &detail));
    assert(operators.Describe(op, &response) == error::INTERNAL_ERROR);
    // Restore a valid resource without changing its component identity.
    ConfigChannelSnapshot prior;
    assert(!configs.Resolve(note_ref.c_str(), &prior, &detail));
    damaged.expected_current_revision = 3;
    damaged.schema_id = "flowsql.operator-description.v1";
    damaged.content = *prior.content;
    assert(!configs.Publish(damaged, &damaged_result, &detail));
    lookups = 0;
    replace_component = true;
    assert(operators.Describe(op, &response) == error::CONFLICT);
    replace_component = false;
    association.contract_state = "declared";
    association.contract_ref = "operator_contract.test.operator@1";
    assert(operators.Describe(op, &response) == error::NOT_FOUND);
    result.Parse(response.c_str());
    assert(Get(result["error"], "code") == "NOT_FOUND");
    registry.Close();
    configs.Close();
    unlink(file);
    std::cout << "PASS: metadata description structure/missing/pending/concurrent source/deadline/read-only\n";
}
