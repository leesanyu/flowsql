// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <common/error_code.h>
#include <openssl/sha.h>
#include <rapidjson/schema.h>
#include <rapidjson/stringbuffer.h>
#include <sqlite3.h>
#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <limits>
#include <memory>
#include <unordered_set>
#include "config_content_validator.h"
#include "data_semantics_api_schema.h"
#include "data_semantics_registry.h"

namespace flowsql::channels::config {
namespace {
using Value = rapidjson::Value;
using Document = rapidjson::Document;
using Alloc = Document::AllocatorType;
std::string Str(const Value& v) { return {v.GetString(), v.GetStringLength()}; }
std::string String(const Value& v, const char* key, const std::string& fallback = "") {
    return v.HasMember(key) ? Str(v[key]) : fallback;
}
void Add(Value& o, const char* key, const std::string& text, Alloc& a) {
    o.AddMember(Value(key, a), Value(text.data(), text.size(), a), a);
}
std::string Hash(const std::string& text) {
    unsigned char bytes[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(text.data()), text.size(), bytes);
    std::string out;
    for (auto b : bytes) {
        out += "0123456789abcdef"[b >> 4];
        out += "0123456789abcdef"[b & 15];
    }
    return out;
}
int ApiValidate(const std::string& key, const Value& v, DataSemanticsErrorV1* e) {
    Document schema;
    schema.Parse(kSemanticsApiSchema);
    Add(schema, "$ref", "#/%24defs/" + key, schema.GetAllocator());
    rapidjson::SchemaDocument compiled(schema);
    rapidjson::SchemaValidator validator(compiled);
    if (v.Accept(validator)) return 0;
    rapidjson::StringBuffer path;
    validator.GetInvalidDocumentPointer().Stringify(path);
    return SemanticsFail(e, "INVALID_DOCUMENT", "payload does not match published " + key + " schema",
                         path.GetString());
}
int32_t Error(const DataSemanticsErrorV1& e, std::string& response) {
    Document d(rapidjson::kObjectType);
    auto& a = d.GetAllocator();
    Value v(rapidjson::kObjectType);
    Add(v, "code", e.code, a);
    Add(v, "message", e.message, a);
    Add(v, "field_path", e.field_path, a);
    v.AddMember("details", Value(rapidjson::kObjectType), a);
    d.AddMember("error", v, a);
    response = SemanticsJson(d);
    if (e.code == "NOT_FOUND") return error::NOT_FOUND;
    if (e.code == "REVISION_CONFLICT" || e.code == "BINDING_MISMATCH") return error::CONFLICT;
    if (e.code == "LIMIT_EXCEEDED") return error::PAYLOAD_TOO_LARGE;
    if (e.code == "UNAVAILABLE") return error::UNAVAILABLE;
    if (e.code == "STORAGE_ERROR") return error::INTERNAL_ERROR;
    return error::BAD_REQUEST;
}
int Finish(const std::string& op, Value& data, std::string& response) {
    Document d(rapidjson::kObjectType);
    auto& a = d.GetAllocator();
    d.AddMember("data", Value(data, a), a);
    response = SemanticsJson(d);
    DataSemanticsErrorV1 e;
    if (response.size() > 12 * 1024 * 1024) return Error({"LIMIT_EXCEEDED", "response exceeds 12 MiB", ""}, response);
    if (ApiValidate(op + "Response", d, &e))
        return Error({"STORAGE_ERROR", "response contract validation failed: " + e.message, e.field_path}, response);
    return error::OK;
}
std::string Scope(const std::string& op, const Document& r) {
    Document copy;
    copy.CopyFrom(r, copy.GetAllocator());
    copy.RemoveMember("cursor");
    copy.RemoveMember("limit");
    return op + SemanticsJson(copy);
}
std::string Cursor(const std::string& scope, uint64_t offset) {
    const auto count = std::to_string(offset);
    return count + "." + Hash(scope + "@" + count);
}
bool Offset(const std::string& scope, const std::string& cursor, uint64_t* offset) {
    *offset = 0;
    if (cursor.empty()) return true;
    const auto dot = cursor.find('.');
    if (dot == std::string::npos || dot == 0) return false;
    auto parsed = std::from_chars(cursor.data(), cursor.data() + dot, *offset);
    return parsed.ec == std::errc{} && parsed.ptr == cursor.data() + dot && *offset <= INT64_MAX - 100 &&
           cursor == Cursor(scope, *offset);
}
void Paging(Value& data, const std::string& scope, uint64_t offset, size_t count, bool more, Alloc& a) {
    Add(data, "next_cursor", more ? Cursor(scope, offset + count) : "", a);
    data.AddMember("has_more", more, a);
}
void Meta(Value& data, const DataSemanticsSnapshotV1& snap, Alloc& a) {
    SemanticsIdentity id;
    ParseSemanticsReference(snap.exact_reference, &id);
    Add(data, "exact_reference", snap.exact_reference, a);
    data.AddMember("revision", id.revision, a);
    Add(data, "schema_id", snap.schema_id, a);
    Add(data, "sha256", snap.sha256, a);
}
Value FromYaml(const YAML::Node& node, Alloc& a) {
    if (node.IsNull()) return Value();
    if (node.IsMap()) {
        Value out(rapidjson::kObjectType);
        for (const auto& pair : node) {
            const auto key = pair.first.Scalar();
            out.AddMember(Value(key.c_str(), a), FromYaml(pair.second, a), a);
        }
        return out;
    }
    if (node.IsSequence()) {
        Value out(rapidjson::kArrayType);
        for (const auto& v : node) out.PushBack(FromYaml(v, a), a);
        return out;
    }
    const auto text = node.Scalar(), tag = node.Tag();
    if (tag == "!" || tag == "tag:yaml.org,2002:str") return Value(text.c_str(), text.size(), a);
    const auto integer_start = !text.empty() && (text.front() == '-' || text.front() == '+') ? 1U : 0U;
    const bool integer_text = integer_start < text.size() && std::all_of(text.begin() + integer_start, text.end(),
                                                                         [](char c) { return c >= '0' && c <= '9'; });
    if (integer_text) {
        if (text.front() == '-') {
            int64_t number;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
            if (parsed.ec != std::errc{})
                throw YAML::RepresentationException(node.Mark(), "integer precision overflow");
            return Value(number);
        }
        uint64_t number;
        const auto parsed = std::from_chars(text.data() + integer_start, text.data() + text.size(), number);
        if (parsed.ec != std::errc{}) throw YAML::RepresentationException(node.Mark(), "integer precision overflow");
        return Value(number);
    }
    Document scalar;
    scalar.Parse(text.c_str());
    if (!scalar.HasParseError() && !scalar.IsString() && !scalar.IsObject() && !scalar.IsArray())
        return Value(scalar, a);
    auto lower = text;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower == ".nan" || lower == ".inf" || lower == "-.inf" || lower == "+.inf" ||
        tag == "tag:yaml.org,2002:float" || tag == "tag:yaml.org,2002:int" || tag == "tag:yaml.org,2002:bool")
        throw YAML::RepresentationException(node.Mark(), "unsupported or non-finite numeric scalar");
    return Value(text.c_str(), text.size(), a);
}
void EmitYaml(const Value& v, YAML::Emitter& emitter) {
    if (v.IsObject()) {
        emitter << YAML::BeginMap;
        for (const auto& m : v.GetObject()) {
            emitter << YAML::Key << YAML::DoubleQuoted << Str(m.name) << YAML::Value;
            EmitYaml(m.value, emitter);
        }
        emitter << YAML::EndMap;
    } else if (v.IsArray()) {
        emitter << YAML::BeginSeq;
        for (const auto& x : v.GetArray()) EmitYaml(x, emitter);
        emitter << YAML::EndSeq;
    } else if (v.IsNull())
        emitter << YAML::Null;
    else if (v.IsBool())
        emitter << v.GetBool();
    else if (v.IsString())
        emitter << YAML::DoubleQuoted << Str(v);
    else if (v.IsInt64())
        emitter << v.GetInt64();
    else if (v.IsUint64())
        emitter << v.GetUint64();
    else
        emitter << YAML::DoublePrecision(std::numeric_limits<double>::max_digits10) << v.GetDouble();
}
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
Statement Prepare(sqlite3* db, const std::string& sql) {
    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &s, nullptr);
    return {s, sqlite3_finalize};
}
void Bind(sqlite3_stmt* s, int index, const std::string& text) {
    sqlite3_bind_text(s, index, text.data(), text.size(), SQLITE_TRANSIENT);
}
std::string Text(sqlite3_stmt* s, int index) {
    const auto* p = sqlite3_column_text(s, index);
    return p ? std::string(reinterpret_cast<const char*>(p), sqlite3_column_bytes(s, index)) : "";
}
}  // namespace

int32_t DataSemanticsRegistry::Control(const std::string& op, const std::string& request, std::string& response) {
    Document r;
    DataSemanticsErrorV1 e;
    if (ParseSemanticsJson(request, &r, &e) || ApiValidate(op + "Request", r, &e)) return Error(e, response);
    Document data(rapidjson::kObjectType);
    auto& a = data.GetAllocator();
    const uint64_t limit = r.HasMember("limit") ? r["limit"].GetUint64() : 50;
    const auto scope = Scope(op, r);
    uint64_t offset;
    if (!Offset(scope, String(r, "cursor"), &offset))
        return Error({"INVALID_DOCUMENT", "invalid cursor for this query", "/cursor"}, response);
    if (op == "save") {
        SemanticsSaveRequest save{String(r, "kind"), String(r, "name"), SemanticsJson(r["document"]),
                                  String(r, "change_note")};
        save.expected_current_revision = r["expected_current_revision"].GetUint64();
        if (r.HasMember("base_revision")) save.base_revision = r["base_revision"].GetUint64();
        SemanticsSaveResult result;
        if (Save(save, &result, &e)) return Error(e, response);
        SemanticsIdentity id;
        ParseSemanticsReference(result.snapshot.exact_reference, &id);
        Add(data, "exact_reference", result.snapshot.exact_reference, a);
        data.AddMember("revision", id.revision, a);
        Add(data, "sha256", result.snapshot.sha256, a);
        data.AddMember("created_revision", result.created_revision, a);
        Document d;
        d.Parse(result.snapshot.document_json->c_str());
        data.AddMember("semantic_gaps", Value(SemanticsGaps(d), a), a);
        if (!result.structure_check.empty()) Add(data, "structure_check", result.structure_check, a);
        Add(data, "data_validation", "not_run", a);
    } else if (op == "import") {
        const auto content = String(r, "content"), format = String(r, "format"), kind = String(r, "kind");
        if (!SemanticsName(String(r, "name"))) return Error({"INVALID_DOCUMENT", "invalid name", "/name"}, response);
        if (content.size() > kSemanticsMaxDocument)
            return Error({"LIMIT_EXCEEDED", "import exceeds 8 MiB", "/content"}, response);
        Document d;
        if (format == "json") {
            if (ParseSemanticsJson(content, &d, &e)) return Error(e, response);
        } else {
            std::string detail;
            if (ValidateConfigContent("yaml", content, &detail))
                return Error({"INVALID_DOCUMENT", detail, "/content"}, response);
            try {
                auto value = FromYaml(YAML::Load(content), d.GetAllocator());
                d.CopyFrom(value, d.GetAllocator());
            } catch (const YAML::Exception&) {
                return Error({"INVALID_DOCUMENT", "YAML contains unsupported scalar or tag", "/content"}, response);
            }
        }
        if (d.IsObject() && d.HasMember("document")) {
            std::unordered_set<std::string> allowed{"kind", "name", "source_reference", "document"};
            for (const auto& m : d.GetObject())
                if (!allowed.count(Str(m.name)))
                    return Error({"INVALID_DOCUMENT", "unknown import envelope property", "/content"}, response);
            if (!d.HasMember("kind") || !d["kind"].IsString() || Str(d["kind"]) != kind || !d.HasMember("name") ||
                !d["name"].IsString() || !d.HasMember("source_reference") || !d["source_reference"].IsString())
                return Error({"INVALID_DOCUMENT", "invalid import envelope", "/content"}, response);
            SemanticsIdentity source;
            if (!ParseSemanticsReference(Str(d["source_reference"]), &source) || source.kind != kind)
                return Error({"INVALID_REFERENCE", "invalid export source reference", "/source_reference"}, response);
            Document document;
            document.CopyFrom(d["document"], document.GetAllocator());
            d.Swap(document);
        }
        Document normalized;
        std::string check;
        if (Preview(kind, SemanticsJson(d), &normalized, &check, &e)) return Error(e, response);
        data.AddMember("document", Value(normalized, a), a)
            .AddMember("semantic_gaps", Value(SemanticsGaps(normalized), a), a);
        Add(data, "operation_source", "import", a);
        Add(data, "data_validation", "not_run", a);
        if (!check.empty()) Add(data, "structure_check", check, a);
    } else if (op == "binding_lookup") {
        DataSemanticsSnapshotV1 snap;
        if (BindingLookup(String(r["object_key"], "provider_id"), String(r["object_key"], "object_id"), &snap, &e))
            return Error(e, response);
        Document d;
        d.Parse(snap.document_json->c_str());
        Add(data, "exact_reference", snap.exact_reference, a);
        Add(data, "dictionary_ref", Str(d["dictionary_ref"]), a);
    } else if (op == "references") {
        std::vector<SemanticsReferenceItem> refs;
        if (References(String(r, "exact_reference"), String(r, "scope", "current") == "history", &refs, &e))
            return Error(e, response);
        Value items(rapidjson::kArrayType);
        for (uint64_t i = offset; i < refs.size() && i - offset < limit; ++i) {
            Value item(rapidjson::kObjectType);
            Add(item, "source_reference", refs[i].source_reference, a);
            Add(item, "field_path", refs[i].field_path, a);
            item.AddMember("is_current", refs[i].is_current, a);
            items.PushBack(item, a);
        }
        const auto count = items.Size();
        data.AddMember("items", items, a);
        Paging(data, scope, offset, count, offset + count < refs.size(), a);
    } else if (op == "list" || op == "history") {
        const auto kind = String(r, "kind"), name = String(r, "name"), q = String(r, "q");
        if (op == "history" && !SemanticsName(name))
            return Error({"INVALID_DOCUMENT", "invalid name", "/name"}, response);
        if (op == "history") {
            DataSemanticsSnapshotV1 snap;
            if (Resolve((kind + "." + name + "@1").c_str(), &snap, &e)) return Error(e, response);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!db_) return Error({"UNAVAILABLE", "registry closed", ""}, response);
        const auto sql =
            op == "list"
                ? "SELECT o.kind,o.name,r.revision,r.sha256,r.created_at,r.document,r.base_revision,r.change_note FROM "
                  "semantics_object o JOIN semantics_revision r ON o.kind=r.kind AND o.name=r.name AND "
                  "o.current_revision=r.revision WHERE (?='' OR o.kind=?) AND (?='' OR instr(lower(o.name||' "
                  "'||r.document),lower(?))>0) ORDER BY o.kind,o.name LIMIT ? OFFSET ?"
                : "SELECT kind,name,revision,sha256,created_at,document,base_revision,change_note FROM "
                  "semantics_revision WHERE kind=? AND name=? ORDER BY revision DESC LIMIT ? OFFSET ?";
        auto statement = Prepare(db_, sql);
        if (!statement) {
            StorageError(&e);
            return Error(e, response);
        }
        int index = 1;
        Bind(statement.get(), index++, kind);
        Bind(statement.get(), index++, op == "list" ? kind : name);
        if (op == "list") {
            Bind(statement.get(), index++, q);
            Bind(statement.get(), index++, q);
        }
        sqlite3_bind_int64(statement.get(), index++, limit + 1);
        sqlite3_bind_int64(statement.get(), index, offset);
        Value items(rapidjson::kArrayType);
        int rc;
        bool more = false;
        while ((rc = sqlite3_step(statement.get())) == SQLITE_ROW) {
            if (items.Size() == limit) {
                more = true;
                break;
            }
            Value item(rapidjson::kObjectType);
            Add(item, "exact_reference",
                Text(statement.get(), 0) + "." + Text(statement.get(), 1) + "@" +
                    std::to_string(sqlite3_column_int64(statement.get(), 2)),
                a);
            item.AddMember("revision", static_cast<uint64_t>(sqlite3_column_int64(statement.get(), 2)), a);
            Add(item, "sha256", Text(statement.get(), 3), a);
            if (op == "list") {
                Document d;
                d.Parse(Text(statement.get(), 5).c_str());
                if (d.HasParseError() || !d.IsObject())
                    return Error({"STORAGE_ERROR", "stored document invalid", ""}, response);
                Add(item, "kind", Text(statement.get(), 0), a);
                Add(item, "name", Text(statement.get(), 1), a);
                Add(item, "title", String(d, d.HasMember("title") ? "title" : "object_label"), a);
                item.AddMember("semantic_gap_count", SemanticsGaps(d).Size(), a)
                    .AddMember("updated_at", static_cast<int64_t>(sqlite3_column_int64(statement.get(), 4)), a);
            } else {
                item.AddMember("created_at", static_cast<int64_t>(sqlite3_column_int64(statement.get(), 4)), a);
                if (sqlite3_column_type(statement.get(), 6) == SQLITE_NULL)
                    item.AddMember("base_revision", Value(), a);
                else
                    item.AddMember("base_revision", static_cast<uint64_t>(sqlite3_column_int64(statement.get(), 6)), a);
                Add(item, "change_note", Text(statement.get(), 7), a);
            }
            items.PushBack(item, a);
        }
        if (rc != SQLITE_DONE && !more) {
            StorageError(&e);
            return Error(e, response);
        }
        const auto count = items.Size();
        data.AddMember("items", items, a);
        Paging(data, scope, offset, count, more, a);
    } else if (op == "resolve" || op == "export") {
        DataSemanticsSnapshotV1 snap;
        if (Resolve(String(r, "exact_reference").c_str(), &snap, &e)) return Error(e, response);
        Document d;
        d.Parse(snap.document_json->c_str());
        SemanticsIdentity id;
        ParseSemanticsReference(snap.exact_reference, &id);
        if (op == "export") {
            Value envelope(rapidjson::kObjectType);
            Add(envelope, "kind", id.kind, a);
            Add(envelope, "name", id.name, a);
            Add(envelope, "source_reference", snap.exact_reference, a);
            envelope.AddMember("document", Value(d, a), a);
            data.CopyFrom(envelope, a);
            const auto format = String(r, "format");
            Add(data, "format", format, a);
            if (format == "json")
                Add(data, "content", SemanticsJson(envelope), a);
            else {
                YAML::Emitter emitter;
                EmitYaml(envelope, emitter);
                Add(data, "content", emitter.c_str(), a);
            }
        } else {
            const auto view = String(r, "view", "document");
            Meta(data, snap, a);
            Add(data, "view", view, a);
            Add(data, "data_validation", "not_run", a);
            data.AddMember("semantic_gaps", Value(SemanticsGaps(d), a), a);
            Value context(rapidjson::kArrayType), required(rapidjson::kArrayType);
            std::vector<SemanticsReference> refs;
            Document ignored;
            if (NormalizeSemantics(id.kind, *snap.document_json, &ignored, &refs, &e)) return Error(e, response);
            std::unordered_set<std::string> seen;
            if (id.kind == "binding") {
                DataSemanticsSnapshotV1 dictionary;
                if (Resolve(String(d, "dictionary_ref").c_str(), &dictionary, &e)) return Error(e, response);
                Document dd;
                dd.Parse(dictionary.document_json->c_str());
                std::string check;
                if (CheckSemanticsBinding(d, dd, &check, &e)) return Error(e, response);
                Add(data, "structure_check", check, a);
                std::vector<SemanticsReference> dictionary_refs;
                if (NormalizeSemantics("dictionary", *dictionary.document_json, &ignored, &dictionary_refs, &e))
                    return Error(e, response);
                refs.insert(refs.end(), dictionary_refs.begin(), dictionary_refs.end());
            }
            size_t context_bytes = view == "document" ? snap.document_json->size() : 0;
            for (const auto& ref : refs)
                if (seen.insert(ref.target).second) {
                    DataSemanticsSnapshotV1 target;
                    if (Resolve(ref.target.c_str(), &target, &e)) return Error(e, response);
                    if (target.document_json->size() > 12 * 1024 * 1024 - context_bytes)
                        return Error({"LIMIT_EXCEEDED", "reference context exceeds 12 MiB", ""}, response);
                    context_bytes += target.document_json->size();
                    Value item(rapidjson::kObjectType);
                    Add(item, "exact_reference", target.exact_reference, a);
                    Add(item, "schema_id", target.schema_id, a);
                    Add(item, "sha256", target.sha256, a);
                    Document td;
                    td.Parse(target.document_json->c_str());
                    item.AddMember("document", Value(td, a), a);
                    context.PushBack(item, a);
                }
            data.AddMember("reference_context", context, a);
            if (view == "document") {
                if (r.HasMember("field_ids") || r.HasMember("cursor") || r.HasMember("limit"))
                    return Error({"INVALID_DOCUMENT", "field selection/paging requires view=fields", "/view"},
                                 response);
                data.AddMember("document", Value(d, a), a);
            } else {
                if (id.kind != "dictionary")
                    return Error({"INVALID_DOCUMENT", "field projection requires dictionary", "/view"}, response);
                std::unordered_set<std::string> selection;
                if (r.HasMember("field_ids"))
                    for (const auto& field : r["field_ids"].GetArray())
                        if (!selection.insert(Str(field)).second)
                            return Error({"INVALID_DOCUMENT", "repeated field_id", "/field_ids"}, response);
                std::unordered_set<std::string> known;
                for (const auto& f : d["fields"].GetArray()) known.insert(Str(f["field_id"]));
                for (const auto& field : selection)
                    if (!known.count(field))
                        return Error({"INVALID_DOCUMENT", "unknown field_id", "/field_ids"}, response);
                std::vector<size_t> indices;
                for (size_t i = 0; i < d["fields"].Size(); ++i)
                    if (!r.HasMember("field_ids") || selection.count(Str(d["fields"][i]["field_id"])))
                        indices.push_back(i);
                Value fields(rapidjson::kArrayType), context_fields(rapidjson::kArrayType);
                std::unordered_set<std::string> expanded;
                for (uint64_t i = offset; i < indices.size() && i - offset < limit; ++i) {
                    const auto& field = d["fields"][indices[i]];
                    fields.PushBack(Value(field, a), a);
                    expanded.insert(Str(field["field_id"]));
                }
                std::unordered_set<std::string> needed;
                const auto& entity = d["entity"];
                for (const char* key : {"identity_fields", "time_fields"})
                    if (entity[key]["value"].IsArray())
                        for (const auto& f : entity[key]["value"].GetArray()) needed.insert(Str(f));
                if (entity["revision_field"]["value"].IsString()) needed.insert(Str(entity["revision_field"]["value"]));
                if (entity["relations"]["value"].IsArray())
                    for (const auto& rel : entity["relations"]["value"].GetArray())
                        for (const auto& m : rel["field_map"].GetObject()) needed.insert(Str(m.name));
                for (const auto& field : fields.GetArray()) {
                    if (field["validity"]["value"].IsObject())
                        for (const auto& f : field["validity"]["value"]["field_ids"].GetArray()) needed.insert(Str(f));
                    if (field["aggregation"]["value"].IsArray())
                        for (const auto& agg : field["aggregation"]["value"].GetArray())
                            for (const auto& f : agg["condition"]["field_ids"].GetArray()) needed.insert(Str(f));
                }
                for (size_t i = 0; i < d["fields"].Size(); ++i) {
                    const auto& f = d["fields"][i];
                    if (!needed.count(Str(f["field_id"])) || expanded.count(Str(f["field_id"]))) continue;
                    Value structure(rapidjson::kObjectType);
                    for (const char* key : {"field_id", "name", "type", "nullable", "title"})
                        structure.AddMember(Value(key, a), Value(f[key], a), a);
                    context_fields.PushBack(structure, a);
                    required.PushBack(Value((snap.exact_reference + "#/fields/" + std::to_string(i)).c_str(), a), a);
                }
                const auto count = fields.Size();
                data.AddMember("fields", fields, a)
                    .AddMember("entity", Value(entity, a), a)
                    .AddMember("context_fields", context_fields, a)
                    .AddMember("projection", true, a);
                Paging(data, scope, offset, count, offset + count < indices.size(), a);
            }
            data.AddMember("required_context_refs", required, a);
        }
    } else
        return Error({"INVALID_DOCUMENT", "unknown operation", ""}, response);
    return Finish(op, data, response);
}
}  // namespace flowsql::channels::config
