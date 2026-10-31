// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include "metadata_description.h"
#include <rapidjson/schema.h>
#include <cerrno>
#include <charconv>
#include <common/channel_dataset.hpp>
#include <limits>
#include <set>
#include "data_semantics_api_schema.h"
#include "metadata_description_time.h"

namespace flowsql::channels::config {
namespace {
using namespace dataset;
int Error(const DataSemanticsErrorV1& e, std::string* response) {
    Json d(rapidjson::kObjectType);
    auto& a = d.GetAllocator();
    Value v(rapidjson::kObjectType);
    Set(v, "code", e.code, a);
    Set(v, "message", e.message, a);
    Set(v, "field_path", e.field_path, a);
    v.AddMember("details", Value(rapidjson::kObjectType), a);
    d.AddMember("error", v, a);
    *response = Encode(d);
    if (e.code == "NOT_FOUND") return error::NOT_FOUND;
    if (e.code == "SOURCE_TIMEOUT") return error::TIMEOUT;
    if (e.code == "REVISION_CONFLICT" || e.code == "SOURCE_CHANGED" || e.code == "CONFIRMATION_REQUIRED")
        return error::CONFLICT;
    if (e.code == "STORAGE_ERROR") return error::INTERNAL_ERROR;
    if (e.code == "SOURCE_UNAVAILABLE" || e.code == "UNAVAILABLE") return error::UNAVAILABLE;
    if (e.code == "LIMIT_EXCEEDED") return error::PAYLOAD_TOO_LARGE;
    return error::BAD_REQUEST;
}
int Parse(const char* text, const std::string& key, Json* req, DataSemanticsErrorV1* e) {
    if (ParseSemanticsJson(text ? text : "", req, e)) return -1;
    Json s;
    s.Parse(kSemanticsApiSchema);
    Set(s, "$ref", "#/%24defs/" + key, s.GetAllocator());
    rapidjson::SchemaDocument schema(s);
    rapidjson::SchemaValidator validator(schema);
    if (req->Accept(validator)) return 0;
    return Fail(e, "INVALID_DOCUMENT", "payload does not match " + key);
}
int Finish(const Value& data, std::string* response) {
    Json out(rapidjson::kObjectType);
    out.AddMember("data", Copy(data, out.GetAllocator()), out.GetAllocator());
    *response = Encode(out);
    if (response->size() > 12 * 1024 * 1024)
        return Error({"LIMIT_EXCEEDED", "description exceeds 12 MiB", ""}, response);
    return 0;
}
Json Basis() {
    Json b(rapidjson::kObjectType);
    for (const char* k : {"object_key", "source_revision", "schema_fingerprint", "binding_ref", "dictionary_ref",
                          "component_revision", "contract_ref", "description_ref"})
        b.AddMember(Value(k, b.GetAllocator()), Value(), b.GetAllocator());
    return b;
}
Value EditorValue(const Value& edit, bool unit, Alloc& a) {
    Value v(rapidjson::kObjectType);
    const auto text = edit.IsString() ? Text(edit) : "";
    const auto start = text.find_first_not_of(" \t\r\n\f\v");
    const bool clear = start == std::string::npos;
    Set(v, "state", clear ? "unknown" : "declared", a);
    Set(v, "origin", "manual", a);
    Value value;
    if (!clear) {
        if (unit) {
            const auto code = text.substr(start, text.find_last_not_of(" \t\r\n\f\v") - start + 1);
            value.SetObject();
            Set(value, "code", code, a);
            Set(value, "label", code, a);
        } else
            value.SetString(text.c_str(), text.size(), a);
    }
    v.AddMember("value", value, a);
    return v;
}
int EditorChanges(const Value& editor, Json* changes, DataSemanticsErrorV1*) {
    changes->SetObject();
    auto& a = changes->GetAllocator();
    if (editor.HasMember("description")) Set(*changes, "description", EditorValue(editor["description"], false, a), a);
    if (editor.HasMember("row_meaning")) {
        Value entity(rapidjson::kObjectType);
        Set(entity, "row_meaning", EditorValue(editor["row_meaning"], false, a), a);
        Set(*changes, "entity", std::move(entity), a);
    }
    if (editor.HasMember("fields")) {
        Value fields(rapidjson::kObjectType);
        for (const auto& item : editor["fields"].GetObject()) {
            Value field(rapidjson::kObjectType);
            for (const auto& m : item.value.GetObject())
                if (Text(m.name) != "time")
                    Set(field, m.name.GetString(), EditorValue(m.value, Text(m.name) == "unit", a), a);
            Set(fields, item.name.GetString(), std::move(field), a);
        }
        Set(*changes, "fields", std::move(fields), a);
    }
    return 0;
}
std::string NotesName(const OperatorComponentContractV1& c) {
    return "n" + Hash(c.category + "\n" + c.name + "\n" + c.component_revision).substr(0, 63);
}
int ReadNotes(ConfigChannelProvider& configs, const OperatorComponentContractV1& c, Json* notes, std::string* ref,
              DataSemanticsErrorV1* e) {
    std::vector<ConfigRevisionListItem> versions;
    uint64_t next;
    std::string detail;
    const auto name = NotesName(c);
    const int rc = configs.ListHistory(name, 0, 1, &versions, &next, &detail);
    if (rc == ENOENT) {
        notes->SetNull();
        ref->clear();
        return 0;
    }
    if (rc || versions.empty()) return Fail(e, "STORAGE_ERROR", detail);
    *ref = "config." + name + "@" + std::to_string(versions.front().revision);
    ConfigChannelSnapshot snap;
    if (configs.Resolve(ref->c_str(), &snap, &detail)) return Fail(e, "STORAGE_ERROR", detail);
    Json document;
    if (snap.schema_id != "flowsql.operator-description.v1" || !snap.content ||
        ParseSemanticsJson(*snap.content, &document, e))
        return Fail(e, "STORAGE_ERROR", "invalid operator description resource");
    if (!document.IsObject() || Get(document, "category") != c.category || Get(document, "name") != c.name ||
        Get(document, "component_revision") != c.component_revision || !document.HasMember("notes") ||
        Parse(Encode(document["notes"]).c_str(), "descriptionNotes", notes, e))
        return Fail(e, "STORAGE_ERROR", "operator description does not match its component or notes schema");
    return 0;
}
}  // namespace

int MetadataDescriptionService::Describe(const char* text, std::string* response) {
    using namespace dataset;
    const auto started = std::chrono::steady_clock::now();
    Json req;
    DataSemanticsErrorV1 e;
    if (Parse(text, "describeRequest", &req, &e)) return Error(e, response);
    const auto deadline =
        started + std::chrono::milliseconds(req.HasMember("timeout_ms") ? req["timeout_ms"].GetUint() : 5000);
    auto remaining = [&]() -> uint32_t {
        const auto n =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        return n > 0 ? static_cast<uint32_t>(n) : 0;
    };
    Json data(rapidjson::kObjectType);
    auto& a = data.GetAllocator();
    data.AddMember("subject", Copy(req["target"], a), a);
    Json basis = Basis();
    Value description(rapidjson::kObjectType), contract(rapidjson::kObjectType), context(rapidjson::kObjectType);
    Set(description, "state", "missing", a);
    Set(description, "origin", "none", a);
    description.AddMember("document", Value(), a).AddMember("notes", Value(), a);
    Set(contract, "state", "missing", a);
    contract.AddMember("document", Value(), a);
    Value gaps(rapidjson::kArrayType), checks(rapidjson::kObjectType);
    Set(checks, "data_validation", "not_run", a);
    if (Get(req["target"], "kind") == "channel") {
        const auto& locator = req["target"]["locator"];
        ChannelDatasetRequestV1 request{Encode(locator), remaining()};
        ChannelDatasetSnapshotV1 snap;
        if (!request.timeout_ms || !describe_)
            return Error({"SOURCE_TIMEOUT", "metadata deadline exceeded", ""}, response);
        if (describe_(request, &snap, &e)) {
            if (e.code != "UNSUPPORTED_SOURCE") return Error(e, response);
            Value structure(rapidjson::kObjectType);
            Set(structure, "state", "unsupported", a);
            Set(structure, "reason", e.message, a);
            structure.AddMember("fields", Value(rapidjson::kArrayType), a);
            Set(checks, "state", "unsupported", a);
            data.AddMember("structure", structure, a)
                .AddMember("basis", Copy(basis, a), a)
                .AddMember("description", description, a)
                .AddMember("contract", contract, a)
                .AddMember("checks", checks, a)
                .AddMember("gaps", gaps, a)
                .AddMember("context", context, a);
            return Finish(data, response);
        }
        if (!remaining()) return Error({"SOURCE_TIMEOUT", "metadata deadline exceeded", ""}, response);
        Json source;
        if (!snap.document_json || ParseSemanticsJson(*snap.document_json, &source, &e))
            return Error({"SOURCE_UNAVAILABLE", "invalid source snapshot", ""}, response);
        // F2 validates source completeness and captures binding before reading its exact dictionary.
        ChannelDictionaryService service(registry_, [&](const auto&, auto* out, auto*) {
            *out = snap;
            return 0;
        });
        Json r(rapidjson::kObjectType);
        r.AddMember("locator", Copy(locator, r.GetAllocator()), r.GetAllocator());
        const auto check_budget = remaining();
        if (!check_budget) return Error({"SOURCE_TIMEOUT", "metadata deadline exceeded", ""}, response);
        r.AddMember("timeout_ms", check_budget, r.GetAllocator());
        std::string checked;
        const auto rc = service.Control("check", Encode(r), checked);
        if (rc) {
            *response = checked;
            return rc;
        }
        Json check;
        check.Parse(checked.c_str());
        checks.CopyFrom(check["data"], a);
        for (const char* k : {"object_key", "source_revision", "schema_fingerprint"})
            Set(basis, k, Copy(source[k], basis.GetAllocator()), basis.GetAllocator());
        for (const char* k : {"binding_ref", "dictionary_ref"})
            Set(basis, k, Copy(checks[k], basis.GetAllocator()), basis.GetAllocator());
        Set(source, "observed_at", std::to_string(Now()), source.GetAllocator());
        Set(source, "state", Get(source, "schema_state"), source.GetAllocator());
        if (!checks["dictionary_ref"].IsNull()) {
            Json resolve(rapidjson::kObjectType);
            Set(resolve, "exact_reference", Get(checks, "dictionary_ref"), resolve.GetAllocator());
            std::string resolved;
            const auto rr = registry_.Control("resolve", Encode(resolve), resolved);
            if (rr) {
                *response = resolved;
                return rr;
            }
            Json result;
            result.Parse(resolved.c_str());
            Set(description, "document", Copy(result["data"]["document"], a), a);
            Set(description, "state", Get(checks, "state") == "matched" ? "present" : "needs_review", a);
            Set(description, "origin", "bound_dictionary", a);
            context.CopyFrom(result["data"], a);
            context.RemoveMember("document");
            DataSemanticsSnapshotV1 bound;
            if (registry_.Resolve(Get(basis, "binding_ref").c_str(), &bound, &e)) return Error(e, response);
            Json binding;
            binding.Parse(bound.document_json->c_str());
            Set(context, "binding", Copy(binding, a), a);
        }
        Value editor_fields(rapidjson::kObjectType);
        if (source["fields"].IsArray())
            for (const auto& physical : source["fields"].GetArray()) {
                const auto name = Get(physical, "name");
                auto facts = description_time::Facts(physical, source, name);
                bool conflict = false;
                if (description["document"].IsObject())
                    for (const auto& f : description["document"]["fields"].GetArray()) {
                        const auto id = Get(f, "field_id");
                        const auto mapped = context.HasMember("binding")
                                                ? Get(context["binding"]["field_map"], id.c_str(), Get(f, "name"))
                                                : Get(f, "name");
                        if (mapped == name) conflict = description_time::Conflict(f, facts);
                    }
                facts.AddMember("conflict", conflict, facts.GetAllocator());
                Set(editor_fields, name.c_str(), Copy(facts, a), a);
            }
        Set(context, "editor_fields", std::move(editor_fields), a);
        gaps.CopyFrom(checks["semantic_gaps"], a);
        // Reject mixed observations, including a changed current binding while resolving context.
        ChannelDatasetSnapshotV1 after;
        request.timeout_ms = remaining();
        if (!request.timeout_ms) return Error({"SOURCE_TIMEOUT", "metadata deadline exceeded", ""}, response);
        if (describe_(request, &after, &e)) return Error(e, response);
        Json current;
        if (after.document_json) current.Parse(after.document_json->c_str());
        if (!current.IsObject() || current["object_key"] != source["object_key"] ||
            Get(current, "source_revision") != Get(source, "source_revision"))
            return Error({"SOURCE_CHANGED", "object changed while describing", ""}, response);
        DataSemanticsSnapshotV1 binding;
        const int br = registry_.BindingLookup(Get(source["object_key"], "provider_id"),
                                               Get(source["object_key"], "object_id"), &binding, &e);
        if (br && e.code != "NOT_FOUND") return Error(e, response);
        if (binding.exact_reference != Get(basis, "binding_ref"))
            return Error({"REVISION_CONFLICT", "binding changed while describing", ""}, response);
        data.AddMember("structure", Copy(source, a), a);
    } else {
        OperatorComponentContractV1 c;
        if (!lookup_) return Error({"UNAVAILABLE", "operator catalog unavailable", ""}, response);
        if (lookup_(Get(req["target"], "name"), remaining(), &c, &e)) return Error(e, response);
        Set(basis, "component_revision", c.component_revision, basis.GetAllocator());
        if (!c.contract_ref.empty()) Set(basis, "contract_ref", c.contract_ref, basis.GetAllocator());
        if (c.contract_state != "missing" && c.contract_state != "declared")
            return Error({"SOURCE_UNAVAILABLE", "declared contract unavailable", ""}, response);
        if (c.contract_state == "declared") {
            Json r(rapidjson::kObjectType);
            Set(r, "exact_reference", c.contract_ref, r.GetAllocator());
            std::string resolved;
            const int rc = registry_.Control("resolve", Encode(r), resolved);
            if (rc) {
                *response = resolved;
                return rc;
            }
            Json result;
            result.Parse(resolved.c_str());
            Set(contract, "state", "declared", a);
            Set(contract, "document", Copy(result["data"]["document"], a), a);
            context.CopyFrom(result["data"], a);
            context.RemoveMember("document");
            Set(description, "origin", "component_author", a);
            Value author(rapidjson::kObjectType);
            for (const char* key : {"title", "operator", "limitations"})
                if (contract["document"].HasMember(key))
                    author.AddMember(Value(key, a), Copy(contract["document"][key], a), a);
            Set(description, "document", std::move(author), a);
            Set(description, "state", "present", a);
        }
        Json notes;
        std::string ref;
        if (ReadNotes(configs_, c, &notes, &ref, &e)) return Error(e, response);
        if (!ref.empty()) {
            Set(basis, "description_ref", ref, basis.GetAllocator());
            Set(description, "notes", Copy(notes, a), a);
            Set(description, "state", "present", a);
            Set(description, "origin",
                c.contract_state == "declared" ? "component_author_and_local_notes" : "local_notes", a);
        }
        OperatorComponentContractV1 after;
        if (lookup_(Get(req["target"], "name"), remaining(), &after, &e)) return Error(e, response);
        if (after.component_revision != c.component_revision || after.contract_ref != c.contract_ref)
            return Error({"SOURCE_CHANGED", "component changed while describing", ""}, response);
        Value structure(rapidjson::kObjectType);
        Set(structure, "state", "unsupported", a);
        Set(structure, "reason", "operator structure is declared by its static contract", a);
        data.AddMember("structure", structure, a);
    }
    if (!remaining()) return Error({"SOURCE_TIMEOUT", "metadata deadline exceeded", ""}, response);
    data.AddMember("basis", Copy(basis, a), a)
        .AddMember("description", description, a)
        .AddMember("contract", contract, a)
        .AddMember("checks", checks, a)
        .AddMember("gaps", gaps, a)
        .AddMember("context", context, a);
    const auto rc = Finish(data, response);
    if (!remaining()) return Error({"SOURCE_TIMEOUT", "metadata deadline exceeded", ""}, response);
    return rc;
}
int MetadataDescriptionService::SaveChannel(const char* text, std::string* response) {
    const auto started = std::chrono::steady_clock::now();
    Json req;
    DataSemanticsErrorV1 e;
    if (Parse(text, "channels_save-descriptionRequest", &req, &e)) return Error(e, response);
    const auto deadline =
        started + std::chrono::milliseconds(req.HasMember("timeout_ms") ? req["timeout_ms"].GetUint() : 5000);
    auto remaining = [&]() -> uint32_t {
        auto n =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        return n > 0 ? static_cast<uint32_t>(n) : 0;
    };
    if (!remaining()) return Error({"SOURCE_TIMEOUT", "metadata deadline exceeded", ""}, response);
    ChannelDictionaryService channels(registry_, describe_);
    Json preview_request(rapidjson::kObjectType);
    auto& pa = preview_request.GetAllocator();
    preview_request.AddMember("locator", Copy(req["locator"], pa), pa).AddMember("timeout_ms", remaining(), pa);
    std::string result;
    int rc = channels.Control("preview", Encode(preview_request), result);
    if (rc) {
        *response = result;
        return rc;
    }
    Json preview;
    preview.Parse(result.c_str());
    auto& view = preview["data"];
    auto& source = view["source_snapshot"];
    if (Get(source, "schema_state") != "known")
        return Error({"BINDING_MISMATCH", "load the source structure explicitly before saving fields", ""}, response);
    Json basis = Basis();
    auto& ba = basis.GetAllocator();
    for (const char* k : {"object_key", "source_revision", "schema_fingerprint"})
        Set(basis, k, Copy(source[k], ba), ba);
    Set(basis, "binding_ref", Copy(view["binding_ref"], ba), ba);
    Set(basis, "dictionary_ref", Copy(view["base_dictionary_ref"], ba), ba);
    for (const char* k : {"object_key", "source_revision", "schema_fingerprint"})
        if (req["basis"][k] != basis[k])
            return Error(
                {"SOURCE_CHANGED", "source changed; refresh and review the retained draft", std::string("/basis/") + k},
                response);
    Json editor_changes;
    if (req.HasMember("editor_changes") && EditorChanges(req["editor_changes"], &editor_changes, &e))
        return Error(e, response);
    const auto& changes = req.HasMember("changes") ? req["changes"] : editor_changes;
    Json candidate;
    candidate.CopyFrom(view["dictionary"], candidate.GetAllocator());
    auto& ca = candidate.GetAllocator();
    Value next_fields(rapidjson::kArrayType), next_map(rapidjson::kObjectType);
    auto confirmed = [&](const std::string& key) {
        if (!req.HasMember("confirmations")) return false;
        for (const auto& item : req["confirmations"].GetArray())
            if (Text(item) == key) return true;
        return false;
    };
    Json previous;
    DataSemanticsSnapshotV1 old;
    if (!view["base_dictionary_ref"].IsNull()) {
        if (registry_.Resolve(Get(view, "base_dictionary_ref").c_str(), &old, &e)) return Error(e, response);
        previous.Parse(old.document_json->c_str());
    }
    std::set<std::string> edited, time_touched;
    for (auto& f : candidate["fields"].GetArray()) {
        std::string id = Get(f, "field_id"), name = Get(view["field_map"], id.c_str(), Get(f, "name"));
        const Value* physical = nullptr;
        for (const auto& actual : source["fields"].GetArray())
            if (Get(actual, "name") == name) physical = &actual;
        if (!physical) {
            if (!confirmed("removed:" + id))
                return Error(
                    {"CONFIRMATION_REQUIRED", "source field was removed; explicitly confirm removal", "removed:" + id},
                    response);
            continue;
        }
        // Stable identities for new fields make retries after a failed apply reuse the same definition.
        bool existing = false;
        if (previous.IsObject())
            for (const auto& prior : previous["fields"].GetArray())
                if (Get(prior, "field_id") == id) existing = true;
        if (!existing) {
            id = "f_" + Hash(Canonical(source["object_key"]) + "\n" + name).substr(0, 32);
            Set(f, "field_id", id, ca);
        }
        if (changes.HasMember("fields") && changes["fields"].HasMember(name.c_str())) {
            for (const auto& m : changes["fields"][name.c_str()].GetObject())
                Set(f, m.name.GetString(), Copy(m.value, ca), ca);
            edited.insert(name);
        }
        if (req.HasMember("editor_changes")) {
            const auto& editor = req["editor_changes"];
            const Value* edit = editor.HasMember("fields") && editor["fields"].HasMember(name.c_str())
                                    ? &editor["fields"][name.c_str()]
                                    : nullptr;
            auto facts = description_time::Facts(*physical, source, name);
            bool touched = false;
            if (description_time::Apply(f, facts, edit, !existing, &touched, ca, &e)) return Error(e, response);
            if (touched) time_touched.insert(id);
        }
        Set(next_map, id.c_str(), name, ca);
        next_fields.PushBack(Copy(f, ca), ca);
    }
    if (changes.HasMember("fields"))
        for (const auto& m : changes["fields"].GetObject())
            if (!edited.count(m.name.GetString()))
                return Error({"SOURCE_CHANGED", "edited field is absent", m.name.GetString()}, response);
    candidate["fields"] = std::move(next_fields);
    if (changes.HasMember("description")) Set(candidate, "description", Copy(changes["description"], ca), ca);
    if (changes.HasMember("entity"))
        for (const auto& m : changes["entity"].GetObject())
            Set(candidate["entity"], m.name.GetString(), Copy(m.value, ca), ca);
    if (req.HasMember("editor_changes"))
        description_time::Summary(candidate["entity"], candidate["fields"], time_touched, ca);
    if (req.HasMember("editor_changes") && req["editor_changes"].HasMember("relations")) {
        Value relations(rapidjson::kArrayType);
        size_t index = 0;
        for (const auto& relation : req["editor_changes"]["relations"].GetArray()) {
            const auto path = "/editor_changes/relations/" + std::to_string(index++);
            const auto exact = Get(relation, "dictionary_ref");
            SemanticsIdentity identity;
            if (!ParseSemanticsReference(exact, &identity) || identity.kind != "dictionary")
                return Error({"INVALID_REFERENCE", "an exact target dictionary is required", path + "/dictionary_ref"},
                             response);
            DataSemanticsSnapshotV1 target;
            if (registry_.Resolve(exact.c_str(), &target, &e)) {
                e.field_path = path + "/dictionary_ref";
                return Error(e, response);
            }
            Json document;
            document.Parse(target.document_json->c_str());
            std::set<std::string> target_ids, used_targets;
            for (const auto& f : document["fields"].GetArray()) target_ids.insert(Get(f, "field_id"));
            Value next(rapidjson::kObjectType), mapping(rapidjson::kObjectType);
            for (const auto& pair : relation["field_map"].GetObject()) {
                const auto name = Text(pair.name), target_id = Text(pair.value);
                std::string source_id;
                for (const auto& source : next_map.GetObject())
                    if (Text(source.value) == name) source_id = Text(source.name);
                if (source_id.empty())
                    return Error({"INVALID_DOCUMENT", "relation source field is absent", path + "/field_map/" + name},
                                 response);
                if (!target_ids.count(target_id) || !used_targets.insert(target_id).second)
                    return Error({"INVALID_DOCUMENT", "relation target field is absent or repeated",
                                  path + "/field_map/" + name},
                                 response);
                Set(mapping, source_id.c_str(), target_id, ca);
            }
            Set(next, "dictionary_ref", exact, ca);
            Set(next, "field_map", std::move(mapping), ca);
            relations.PushBack(std::move(next), ca);
        }
        Set(candidate["entity"], "relations", description_time::Semantic(std::move(relations), "manual", ca), ca);
    }
    Json normalized;
    std::vector<SemanticsReference> refs;
    if (NormalizeSemantics("dictionary", Encode(candidate), &normalized, &refs, &e)) return Error(e, response);
    const bool identical = previous.IsObject() && Canonical(normalized) == Canonical(previous);
    auto finish = [&](const std::string& status, const std::string& dictionary, const std::string& binding,
                      const Value* error) {
        Json data(rapidjson::kObjectType);
        auto& a = data.GetAllocator();
        Set(data, "status", status, a);
        Set(data, "dictionary_ref", dictionary, a);
        data.AddMember("binding_ref", binding.empty() ? Value() : Value(binding.c_str(), a), a)
            .AddMember("basis", Copy(basis, a), a)
            .AddMember("error", error ? Copy(*error, a) : Value(), a);
        return Finish(data, response);
    };
    // The sole stale-write exception is an already applied, identical submission for this exact source.
    if (req["basis"] != basis) {
        if (identical && !view["binding_ref"].IsNull()) {
            DataSemanticsSnapshotV1 binding;
            if (registry_.Resolve(Get(view, "binding_ref").c_str(), &binding, &e)) return Error(e, response);
            Json document;
            document.Parse(binding.document_json->c_str());
            if (document.HasMember("source_snapshot") &&
                Get(document["source_snapshot"], "source_revision") == Get(source, "source_revision"))
                return finish("applied", Get(view, "base_dictionary_ref"), Get(view, "binding_ref"), nullptr);
        }
        return Error(
            {"REVISION_CONFLICT", "description changed; retain the draft and review the new version", "/basis"},
            response);
    }
    for (const auto& key : view["required_confirmations"].GetArray())
        if (!confirmed(Text(key)))
            return Error({"CONFIRMATION_REQUIRED", "explicit source change confirmation required", Text(key)},
                         response);
    SemanticsIdentity binding_id;
    const std::string binding_ref = Get(view, "binding_ref");
    if (!binding_ref.empty()) ParseSemanticsReference(binding_ref, &binding_id);
    const std::string object_token = Hash(Canonical(source["object_key"]));
    SemanticsSaveResult saved;
    if (identical)
        saved.snapshot = old;
    else {
        // Content-addressed, object-local copies never mutate a dictionary shared by another object.
        SemanticsSaveRequest save{
            "dictionary", "description." + object_token.substr(0, 32) + "." + Hash(Canonical(normalized)).substr(0, 48),
            Encode(normalized)};
        if (!remaining()) return Error({"SOURCE_TIMEOUT", "metadata deadline exceeded before saving", ""}, response);
        if (registry_.Save(save, &saved, &e)) return Error(e, response);
    }
    auto partial = [&](const std::string& encoded) {
        Json failed;
        failed.Parse(encoded.c_str());
        return finish("definition_saved_not_applied", saved.snapshot.exact_reference, binding_ref, &failed["error"]);
    };
    if (!remaining()) {
        std::string failed;
        Error({"SOURCE_TIMEOUT", "definition saved; deadline exceeded before apply", ""}, &failed);
        return partial(failed);
    }
    Json apply(rapidjson::kObjectType);
    auto& aa = apply.GetAllocator();
    Set(apply, "name", binding_ref.empty() ? "description." + object_token.substr(0, 48) : binding_id.name, aa);
    Set(apply, "dictionary_ref", saved.snapshot.exact_reference, aa);
    apply.AddMember("locator", Copy(req["locator"], aa), aa)
        .AddMember("expected_current_revision", binding_id.revision, aa)
        .AddMember("field_map", Copy(next_map, aa), aa)
        .AddMember("source_basis", Value(rapidjson::kObjectType), aa)
        .AddMember("confirmations",
                   req.HasMember("confirmations") ? Copy(req["confirmations"], aa) : Value(rapidjson::kArrayType), aa)
        .AddMember("timeout_ms", remaining(), aa);
    for (const char* k : {"object_key", "source_revision", "schema_fingerprint"})
        Set(apply["source_basis"], k, Copy(source[k], aa), aa);
    rc = channels.Control("apply", Encode(apply), result);
    if (rc) return partial(result);
    Json applied;
    applied.Parse(result.c_str());
    const auto next_ref = Get(applied["data"], "exact_reference");
    Set(basis, "binding_ref", next_ref, ba);
    Set(basis, "dictionary_ref", saved.snapshot.exact_reference, ba);
    return finish("applied", saved.snapshot.exact_reference, next_ref, nullptr);
}
int MetadataDescriptionService::SaveOperator(const char* text, std::string* response) {
    Json req;
    DataSemanticsErrorV1 e;
    if (Parse(text, "operators_save-descriptionRequest", &req, &e)) return Error(e, response);
    if (!lookup_) return Error({"UNAVAILABLE", "operator catalog unavailable", ""}, response);
    OperatorComponentContractV1 component;
    if (lookup_(Get(req, "name"), 5000, &component, &e)) return Error(e, response);
    if (component.component_revision != Get(req, "expected_component_revision"))
        return Error({"SOURCE_CHANGED", "operator component changed; retain and review the draft",
                      "/expected_component_revision"},
                     response);
    Json current;
    std::string current_ref;
    if (ReadNotes(configs_, component, &current, &current_ref, &e)) return Error(e, response);
    const auto expected = Get(req, "expected_description_ref");
    const bool same = !current.IsNull() && Canonical(current) == Canonical(req["notes"]);
    if (current_ref != expected && !same)
        return Error(
            {"REVISION_CONFLICT", "operator notes changed; retain and review the draft", "/expected_description_ref"},
            response);
    ConfigPublishRequest publish;
    publish.name = NotesName(component);
    auto revision = [&](const std::string& ref, uint64_t* out) {
        if (ref.empty()) {
            *out = 0;
            return true;
        }
        const auto prefix = "config." + publish.name + "@";
        if (ref.compare(0, prefix.size(), prefix) != 0) return false;
        const auto parsed = std::from_chars(ref.data() + prefix.size(), ref.data() + ref.size(), *out);
        return parsed.ec == std::errc{} && parsed.ptr == ref.data() + ref.size() && *out > 0 &&
               *out <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    };
    uint64_t expected_revision = 0;
    if (!revision(expected, &expected_revision) || !revision(current_ref, &publish.expected_current_revision))
        return Error({"REVISION_CONFLICT", "description reference does not belong to this component",
                      "/expected_description_ref"},
                     response);
    if (!same) publish.expected_current_revision = expected_revision;
    publish.format = "json";
    publish.schema_id = "flowsql.operator-description.v1";
    Json document(rapidjson::kObjectType);
    auto& a = document.GetAllocator();
    Set(document, "category", component.category, a);
    Set(document, "name", component.name, a);
    Set(document, "component_revision", component.component_revision, a);
    document.AddMember("notes", Copy(req["notes"], a), a);
    publish.content = Canonical(document);
    OperatorComponentContractV1 before;
    if (lookup_(Get(req, "name"), 5000, &before, &e)) return Error(e, response);
    if (before.component_revision != component.component_revision)
        return Error({"SOURCE_CHANGED", "component changed before saving notes", ""}, response);
    ConfigPublishResult saved;
    std::string detail;
    const int rc = configs_.Publish(publish, &saved, &detail);
    if (rc) return Error({rc == EAGAIN ? "REVISION_CONFLICT" : "STORAGE_ERROR", detail, ""}, response);
    OperatorComponentContractV1 after;
    if (lookup_(Get(req, "name"), 5000, &after, &e)) return Error(e, response);
    if (after.component_revision != component.component_revision)
        return Error({"SOURCE_CHANGED", "notes retained for the old component; current component changed", ""},
                     response);
    Json data(rapidjson::kObjectType);
    auto& da = data.GetAllocator();
    Set(data, "description_ref", "config." + publish.name + "@" + std::to_string(saved.revision), da);
    Set(data, "component_revision", component.component_revision, da);
    data.AddMember("created_revision", saved.created_revision, da);
    return Finish(data, response);
}
}  // namespace flowsql::channels::config
