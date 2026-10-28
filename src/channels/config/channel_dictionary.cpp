// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include "channel_dictionary.h"
#include <common/error_code.h>
#include <rapidjson/schema.h>
#include <common/channel_dataset.hpp>
#include <set>
#include "data_semantics_api_schema.h"

namespace flowsql::channels::config {
namespace {
using namespace dataset;
int Validate(const std::string& key, const Value& v, DataSemanticsErrorV1* e) {
    Json schema;
    schema.Parse(kSemanticsApiSchema);
    Set(schema, "$ref", "#/%24defs/" + key, schema.GetAllocator());
    rapidjson::SchemaDocument compiled(schema);
    rapidjson::SchemaValidator validator(compiled);
    if (v.Accept(validator)) return 0;
    rapidjson::StringBuffer path;
    validator.GetInvalidDocumentPointer().Stringify(path);
    return Fail(e, "INVALID_DOCUMENT", "payload does not match " + key, path.GetString());
}
int32_t Error(const DataSemanticsErrorV1& e, std::string& response) {
    Json d(rapidjson::kObjectType);
    auto& a = d.GetAllocator();
    Value v(rapidjson::kObjectType);
    Set(v, "code", e.code, a);
    Set(v, "message", e.message, a);
    Set(v, "field_path", e.field_path, a);
    v.AddMember("details", Value(rapidjson::kObjectType), a);
    d.AddMember("error", v, a);
    response = Encode(d);
    if (e.code == "NOT_FOUND") return error::NOT_FOUND;
    if (e.code == "REVISION_CONFLICT" || e.code == "SOURCE_CHANGED" || e.code == "BINDING_MISMATCH" ||
        e.code == "CONFIRMATION_REQUIRED")
        return error::CONFLICT;
    if (e.code == "SOURCE_TIMEOUT") return error::TIMEOUT;
    if (e.code == "SOURCE_UNAVAILABLE" || e.code == "UNAVAILABLE") return error::UNAVAILABLE;
    if (e.code == "STORAGE_ERROR") return error::INTERNAL_ERROR;
    if (e.code == "LIMIT_EXCEEDED") return error::PAYLOAD_TOO_LARGE;
    return error::BAD_REQUEST;
}
const Value* Find(const Value& fields, const std::string& name) {
    if (!fields.IsArray()) return nullptr;
    for (const auto& f : fields.GetArray())
        if (Get(f, "name") == name) return &f;
    return nullptr;
}
Value Semantic(Value value, Alloc& a) {
    Value v(rapidjson::kObjectType);
    v.AddMember("state", "declared", a).AddMember("value", value, a).AddMember("origin", "producer", a);
    return v;
}
void Producer(Value& field, const Value& source, const std::string& name, Alloc& a) {
    const auto& metadata = source["producer_metadata"];
    if (!metadata.HasMember("fields") || !metadata["fields"].IsObject() || !metadata["fields"].HasMember(name.c_str()))
        return;
    const auto& f = metadata["fields"][name.c_str()];
    for (const char* key : {"unit", "time_semantics", "definition"}) {
        if (!f.HasMember(key)) continue;
        // An existing manual/imported declaration always wins; the difference remains visible.
        if (field.HasMember(key) && Get(field[key], "origin") != "producer" && Get(field[key], "state") != "unknown")
            continue;
        Set(field, key, Semantic(Copy(f[key], a), a), a);
    }
}
void Difference(Value& diffs, Value& required, const std::string& kind, const std::string& id, const std::string& name,
                const Value& before, const Value& after, bool confirm, Alloc& a) {
    Value d(rapidjson::kObjectType);
    Set(d, "kind", kind, a);
    Set(d, "field_id", id, a);
    Set(d, "source_name", name, a);
    d.AddMember("before", Copy(before, a), a).AddMember("after", Copy(after, a), a);
    const auto key = confirm ? kind + ":" + id : "";
    Set(d, "confirmation_key", key, a);
    diffs.PushBack(d, a);
    if (confirm) required.PushBack(Value(key.c_str(), a), a);
}
void Differences(const Value* binding, const Value* dictionary, const Value& source, const Value& map,
                 const Value& renames, Value& diffs, Value& required, Alloc& a) {
    if (Get(source, "identity_state") == "unknown") required.PushBack("identity", a);
    if (!dictionary) return;
    const Value* old_source =
        binding && binding->HasMember("source_snapshot") ? &(*binding)["source_snapshot"] : nullptr;
    const Value& previous_map = binding ? (*binding)["field_map"] : map;
    if (binding && (!old_source || (*binding)["object_key"] != source["object_key"] ||
                    (old_source && Get(*old_source, "source_revision") != Get(source, "source_revision") &&
                     (*old_source)["schema_fingerprint"] == source["schema_fingerprint"] &&
                     (*old_source)["producer_metadata"] == source["producer_metadata"]))) {
        Difference(diffs, required, "identity_changed", "", "", binding->operator[]("object_key"), source["object_key"],
                   true, a);
    }
    std::set<std::string> used;
    Value previous_order(rapidjson::kArrayType), current_order(rapidjson::kArrayType);
    for (const auto& f : (*dictionary)["fields"].GetArray()) {
        const auto id = Get(f, "field_id");
        const auto old_name = previous_map.HasMember(id.c_str()) ? Get(previous_map, id.c_str()) : Get(f, "name");
        const auto name = renames.HasMember(id.c_str()) ? Get(renames, id.c_str())
                          : map.HasMember(id.c_str())   ? Get(map, id.c_str())
                                                        : old_name;
        used.insert(name);
        const Value* actual = Find(source["fields"], name);
        if (!actual) {
            Difference(diffs, required, "removed", id, name, f, Value(), true, a);
            continue;
        }
        previous_order.PushBack(Value(name.c_str(), a), a);
        if (name != old_name)
            Difference(diffs, required, "renamed", id, name, Value(old_name.c_str(), a), Value(name.c_str(), a), true,
                       a);
        for (const char* key : {"type", "nullable"})
            if (Canonical(f[key]) != Canonical((*actual)[key]))
                Difference(diffs, required, std::string(key) + "_changed", id, name, f[key], (*actual)[key], true, a);
    }
    for (const auto& f : source["fields"].GetArray()) {
        const auto name = Get(f, "name");
        if (used.count(name))
            current_order.PushBack(Value(name.c_str(), a), a);
        else
            Difference(diffs, required, "added", "", name, Value(), f, false, a);
    }
    if (previous_order != current_order)
        Difference(diffs, required, "order_changed", "", "", previous_order, current_order, false, a);
    if (old_source && Canonical((*old_source)["producer_metadata"]) != Canonical(source["producer_metadata"]))
        Difference(diffs, required, "producer_changed", "", "", (*old_source)["producer_metadata"],
                   source["producer_metadata"], true, a);
}
bool Has(const Value& values, const std::string& s) {
    for (const auto& x : values.GetArray())
        if (Text(x) == s) return true;
    return false;
}
int LoadDictionary(DataSemanticsRegistry& registry, const std::string& ref, Json* d, DataSemanticsErrorV1* e) {
    SemanticsIdentity id;
    if (!ParseSemanticsReference(ref, &id) || id.kind != "dictionary")
        return Fail(e, "INVALID_REFERENCE", "exact dictionary required");
    DataSemanticsSnapshotV1 snap;
    if (registry.Resolve(ref.c_str(), &snap, e)) return -1;
    d->Parse(snap.document_json->c_str());
    return 0;
}
}  // namespace
int32_t ChannelDictionaryService::Control(const std::string& op, const std::string& text, std::string& response) {
    using namespace dataset;
    DataSemanticsErrorV1 e;
    Json r;
    if (ParseSemanticsJson(text, &r, &e) || Validate("channels_" + op + "Request", r, &e)) return Error(e, response);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(r.HasMember("timeout_ms") ? r["timeout_ms"].GetUint() : 5000);
    ChannelDatasetSnapshotV1 snapshot;
    ChannelDatasetRequestV1 request{Encode(r["locator"]), r.HasMember("timeout_ms") ? r["timeout_ms"].GetUint() : 5000};
    if (!describe_) return Error({"UNAVAILABLE", "dataset descriptor unavailable", ""}, response);
    if (describe_(request, &snapshot, &e)) return Error(e, response);
    if (std::chrono::steady_clock::now() >= deadline)
        return Error({"SOURCE_TIMEOUT", "metadata deadline exceeded", ""}, response);
    Json source;
    if (!snapshot.document_json || ParseSemanticsJson(*snapshot.document_json, &source, &e) ||
        Validate("datasetSnapshot", source, &e))
        return Error({"SOURCE_UNAVAILABLE", "descriptor returned an invalid snapshot", ""}, response);
    if (Get(source, "schema_state") == "known") {
        std::set<std::string> names;
        if (!source["fields"].IsArray() || Get(source, "schema_fingerprint") != Hash(Canonical(source["fields"])))
            return Error({"SOURCE_UNAVAILABLE", "source structure fingerprint mismatch", ""}, response);
        for (const auto& f : source["fields"].GetArray())
            if (!names.insert(Get(f, "name")).second || f["type"].IsNull() || f["nullable"].IsNull())
                return Error({"SOURCE_UNAVAILABLE", "incomplete or repeated source field", "/fields"}, response);
    }
    Json data(rapidjson::kObjectType);
    auto& a = data.GetAllocator();
    if (op == "describe") {
        data.AddMember("source_snapshot", Copy(source, a), a).AddMember("checked_at", Now(), a);
    } else {
        DataSemanticsSnapshotV1 bound;
        Json binding, dictionary;
        const auto& key = source["object_key"];
        if (registry_.BindingLookup(Get(key, "provider_id"), Get(key, "object_id"), &bound, &e) == 0)
            binding.Parse(bound.document_json->c_str());
        else if (e.code != "NOT_FOUND")
            return Error(e, response);
        const auto dictionary_ref =
            Get(r, "base_dictionary_ref", binding.IsObject() ? Get(binding, "dictionary_ref") : "");
        if (!dictionary_ref.empty() && LoadDictionary(registry_, dictionary_ref, &dictionary, &e))
            return Error(e, response);
        Value map(rapidjson::kObjectType), renames(rapidjson::kObjectType);
        if (r.HasMember("field_map"))
            map.CopyFrom(r["field_map"], a);
        else if (binding.IsObject())
            map.CopyFrom(binding["field_map"], a);
        else if (dictionary.IsObject())
            for (const auto& f : dictionary["fields"].GetArray())
                Set(map, Get(f, "field_id").c_str(), Get(f, "name"), a);
        if (r.HasMember("renames")) renames.CopyFrom(r["renames"], a);
        Value diffs(rapidjson::kArrayType), required(rapidjson::kArrayType);
        const bool known = Get(source, "schema_state") == "known";
        bool observation_trusted = false;
        if (binding.IsObject() && binding.HasMember("source_snapshot") &&
            registry_.ObservationTrusted(bound.exact_reference, &observation_trusted, &e))
            return Error(e, response);
        if (known && binding.IsObject() && binding.HasMember("source_snapshot") && !observation_trusted)
            required.PushBack("observation", a);
        if (known)
            Differences(binding.IsObject() ? &binding : nullptr, dictionary.IsObject() ? &dictionary : nullptr, source,
                        map, renames, diffs, required, a);
        if (op == "preview") {
            Json candidate(rapidjson::kObjectType);
            auto& ca = candidate.GetAllocator();
            if (known) {
                if (dictionary.IsObject())
                    candidate.CopyFrom(dictionary, ca);
                else {
                    Set(candidate, "schema_id", "flowsql.data-dictionary.v1", ca);
                    Set(candidate, "title", Get(r["locator"], "table", Get(r["locator"], "name")), ca);
                    candidate.AddMember("entity", Value(rapidjson::kObjectType), ca)
                        .AddMember("fields", Value(rapidjson::kArrayType), ca);
                }
                Value fields(rapidjson::kArrayType), next_map(rapidjson::kObjectType);
                std::set<std::string> used;
                for (const auto& f : candidate["fields"].GetArray()) {
                    const auto id = Get(f, "field_id");
                    if (r.HasMember("remove_fields") && Has(r["remove_fields"], id)) continue;
                    const auto name =
                        renames.HasMember(id.c_str()) ? Get(renames, id.c_str()) : Get(map, id.c_str(), Get(f, "name"));
                    Value next(f, ca);
                    const auto* actual = Find(source["fields"], name);
                    if (actual) {
                        Set(next, "type", Copy((*actual)["type"], ca), ca);
                        Set(next, "nullable", Copy((*actual)["nullable"], ca), ca);
                        if (renames.HasMember(id.c_str())) Set(next, "name", name, ca);
                        Producer(next, source, name, ca);
                    }
                    fields.PushBack(next, ca);
                    used.insert(name);
                    Set(next_map, id.c_str(), name, a);
                }
                for (const auto& f : source["fields"].GetArray())
                    if (!used.count(Get(f, "name"))) {
                        Value next(f, ca);
                        const auto id = "f_" + Token(), name = Get(f, "name");
                        Set(next, "field_id", id, ca);
                        Set(next, "title", name, ca);
                        Producer(next, source, name, ca);
                        fields.PushBack(next, ca);
                        Set(next_map, id.c_str(), name, a);
                    }
                candidate["fields"] = std::move(fields);
                map = std::move(next_map);
                Json normalized;
                std::vector<SemanticsReference> refs;
                if (NormalizeSemantics("dictionary", Encode(candidate), &normalized, &refs, &e))
                    return Error(e, response);
                candidate = std::move(normalized);
            }
            data.AddMember("source_snapshot", Copy(source, a), a)
                .AddMember("dictionary", known ? Copy(candidate, a) : Value(), a)
                .AddMember("field_map", map, a);
            data.AddMember("binding_ref", bound.document_json ? Value(bound.exact_reference.c_str(), a) : Value(), a);
            data.AddMember("base_dictionary_ref", dictionary_ref.empty() ? Value() : Value(dictionary_ref.c_str(), a),
                           a);
            data.AddMember("differences", diffs, a).AddMember("required_confirmations", required, a);
            data.AddMember("semantic_gaps", known ? Value(SemanticsGaps(candidate), a) : Value(rapidjson::kArrayType),
                           a);
            Set(data, "state",
                !known                                   ? "pending"
                : data["required_confirmations"].Empty() ? "matched"
                                                         : "needs_confirmation",
                a);
        } else {
            std::string state = !known ? "pending" : binding.IsObject() ? "matched" : "unbound";
            if (op == "apply") {
                if (!known) return Error({"BINDING_MISMATCH", "source structure is not complete", ""}, response);
                const auto& basis = r["source_basis"];
                if (basis["object_key"] != source["object_key"] ||
                    Get(basis, "source_revision") != Get(source, "source_revision") ||
                    basis["schema_fingerprint"] != source["schema_fingerprint"])
                    return Error({"SOURCE_CHANGED", "source changed after preview", "/source_basis"}, response);
                SemanticsIdentity current;
                if (binding.IsObject()) ParseSemanticsReference(bound.exact_reference, &current);
                Json target;
                if (LoadDictionary(registry_, Get(r, "dictionary_ref"), &target, &e)) return Error(e, response);
                Json b(rapidjson::kObjectType);
                auto& ba = b.GetAllocator();
                Set(b, "schema_id", "flowsql.data-binding.v2", ba);
                Set(b, "object_label", Get(r["locator"], "table", Get(r["locator"], "name")), ba);
                Set(b, "dictionary_ref", Get(r, "dictionary_ref"), ba);
                b.AddMember("object_key", Copy(key, ba), ba)
                    .AddMember("field_map", Copy(r["field_map"], ba), ba)
                    .AddMember("declared_schema",
                               binding.IsObject() && binding.HasMember("declared_schema")
                                   ? Copy(binding["declared_schema"], ba)
                                   : Value(),
                               ba)
                    .AddMember("source_snapshot", Copy(source, ba), ba);
                Value confirmation(rapidjson::kObjectType);
                Set(confirmation, "source_revision", Get(source, "source_revision"), ba);
                // Normalize caller order so repeated application is a no-op.
                std::set<std::string> items;
                for (const auto& item : r["confirmations"].GetArray()) items.insert(Text(item));
                if (binding.IsObject() && binding.HasMember("confirmation") &&
                    Get(binding["confirmation"], "source_revision") == Get(source, "source_revision"))
                    for (const auto& item : binding["confirmation"]["items"].GetArray()) items.insert(Text(item));
                Value confirmed(rapidjson::kArrayType);
                for (const auto& item : items) confirmed.PushBack(Value(item.c_str(), ba), ba);
                confirmation.AddMember("items", confirmed, ba);
                b.AddMember("confirmation", confirmation, ba);
                std::string mapping_check;
                if (CheckSemanticsBinding(b, target, &mapping_check, &e)) return Error(e, response);
                // Retain F1's identical-content no-op, while stale differing writes report conflict after validation.
                if (r["expected_current_revision"].GetUint64() != current.revision &&
                    (!binding.IsObject() || Canonical(b) != Canonical(binding)))
                    return Error({"REVISION_CONFLICT", "current binding revision changed", ""}, response);
                for (const auto& item : required.GetArray())
                    if (!Has(r["confirmations"], Text(item)))
                        return Error({"CONFIRMATION_REQUIRED", "explicit change confirmation required", Text(item)},
                                     response);

                SemanticsSaveRequest save{"binding", Get(r, "name"), Encode(b)};
                save.source_observed = true;
                save.expected_current_revision = r["expected_current_revision"].GetUint64();
                SemanticsSaveResult result;
                if (registry_.Save(save, &result, &e)) return Error(e, response);
                Set(data, "exact_reference", result.snapshot.exact_reference, a);
                data.AddMember("created_revision", result.created_revision, a)
                    .AddMember("source_snapshot", Copy(source, a), a);
                bound = result.snapshot;
                dictionary = std::move(target);
                binding = std::move(b);
                state = "matched";
            } else if (known && binding.IsObject()) {
                Json check_binding;
                check_binding.CopyFrom(binding, check_binding.GetAllocator());
                Set(check_binding, "declared_schema", Copy(source["fields"], check_binding.GetAllocator()),
                    check_binding.GetAllocator());
                check_binding.RemoveMember("source_snapshot");
                std::string check;
                if (CheckSemanticsBinding(check_binding, dictionary, &check, &e))
                    state = "mismatch";
                else if (!required.Empty()) {
                    state = "needs_confirmation";
                    if (observation_trusted && binding.HasMember("confirmation") &&
                        Get(binding["confirmation"], "source_revision") == Get(source, "source_revision")) {
                        bool all = true;
                        for (const auto& item : required.GetArray())
                            all = all && Has(binding["confirmation"]["items"], Text(item));
                        if (all) state = "matched";
                    }
                }
            }
            Value check(rapidjson::kObjectType);
            check.AddMember("binding_ref", bound.document_json ? Value(bound.exact_reference.c_str(), a) : Value(), a);
            check.AddMember("dictionary_ref",
                            dictionary.IsObject() ? Value(Get(binding, "dictionary_ref").c_str(), a) : Value(), a);
            Set(check, "state", state, a);
            check.AddMember("differences", diffs, a).AddMember("required_confirmations", required, a);
            check.AddMember("structure_gaps", Copy(source["structure_gaps"], a), a)
                .AddMember("semantic_gaps",
                           dictionary.IsObject() ? Value(SemanticsGaps(dictionary), a) : Value(rapidjson::kArrayType),
                           a);
            check.AddMember("checked_at", Now(), a);
            Set(check, "data_validation", "not_run", a);
            if (op == "apply")
                data.AddMember("check", check, a);
            else
                data.CopyFrom(check, a);
        }
    }
    Json envelope(rapidjson::kObjectType);
    envelope.AddMember("data", Copy(data, envelope.GetAllocator()), envelope.GetAllocator());
    response = Encode(envelope);
    if (response.size() > 12 * 1024 * 1024) return Error({"LIMIT_EXCEEDED", "response exceeds 12 MiB", ""}, response);
    if (Validate("channels_" + op + "Response", envelope, &e))
        return Error({"STORAGE_ERROR", "response contract mismatch", e.field_path}, response);
    return error::OK;
}
}  // namespace flowsql::channels::config
