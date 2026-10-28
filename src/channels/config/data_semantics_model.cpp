// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include "data_semantics_model.h"
#include <rapidjson/reader.h>
#include <rapidjson/schema.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <algorithm>
#include <charconv>
#include <common/json_depth.hpp>
#include <unordered_set>
#include "config_content_validator.h"
#include "data_semantics_schema.h"

namespace flowsql::channels::config {
namespace {
using Value = rapidjson::Value;
using Alloc = rapidjson::Document::AllocatorType;
struct IntegerPrecisionGuard : rapidjson::BaseReaderHandler<rapidjson::UTF8<>, IntegerPrecisionGuard> {
    bool RawNumber(const char* text, rapidjson::SizeType size, bool) {
        const std::string number(text, size);
        if (number.find_first_of(".eE") != std::string::npos) return true;
        if (number.front() == '-') {
            int64_t value;
            return std::from_chars(text, text + size, value).ec == std::errc{};
        }
        uint64_t value;
        return std::from_chars(text, text + size, value).ec == std::errc{};
    }
};
std::string Str(const Value& v) { return {v.GetString(), v.GetStringLength()}; }
std::string Pointer(const std::string& key) {
    std::string out;
    for (char c : key) out += c == '~' ? "~0" : c == '/' ? "~1" : std::string(1, c);
    return out;
}
void Sort(Value& v) {
    if (v.IsObject()) {
        std::sort(v.MemberBegin(), v.MemberEnd(),
                  [](const auto& a, const auto& b) { return Str(a.name) < Str(b.name); });
        for (auto& m : v.GetObject()) Sort(m.value);
    } else if (v.IsArray())
        for (auto& x : v.GetArray()) Sort(x);
}
void Semantic(Value& parent, const char* key, Alloc& a) {
    if (!parent.HasMember(key)) {
        Value unknown(rapidjson::kObjectType);
        unknown.AddMember("state", "unknown", a).AddMember("value", Value(), a).AddMember("origin", "manual", a);
        parent.AddMember(Value(key, a), unknown, a);
    } else if (parent[key].IsObject() && !parent[key].HasMember("origin"))
        parent[key].AddMember("origin", "manual", a);
}
void Normalize(Value& d, const std::string& kind, Alloc& a) {
    if (kind == "standard") {
        for (const char* k : {"definition", "scope", "quantity", "representation", "unit", "value_domain"})
            Semantic(d, k, a);
    } else if (kind == "dictionary") {
        Semantic(d, "description", a);
        if (d.HasMember("entity") && d["entity"].IsObject()) {
            for (const char* k : {"row_meaning", "row_semantics", "identity_fields", "revision_field", "time_fields",
                                  "relations", "limitations"})
                Semantic(d["entity"], k, a);
        }
        if (d.HasMember("fields") && d["fields"].IsArray())
            for (auto& f : d["fields"].GetArray())
                if (f.IsObject()) {
                    for (const char* k : {"definition", "role", "measure_kind", "unit", "time_semantics",
                                          "null_meaning", "validity", "aggregation", "value_domain", "standard_refs"})
                        Semantic(f, k, a);
                    if (f.HasMember("time_semantics") && f["time_semantics"].IsObject() &&
                        f["time_semantics"].HasMember("value") && f["time_semantics"]["value"].IsObject()) {
                        auto& t = f["time_semantics"]["value"];
                        for (const char* k : {"unit", "epoch", "timezone", "description"}) Semantic(t, k, a);
                    }
                }
    }
}
void Gaps(const Value& v, const std::string& p, Value& list, Alloc& a) {
    if (v.IsObject()) {
        if (v.HasMember("state") && v.HasMember("value") && v.HasMember("origin") && v["state"].IsString() &&
            Str(v["state"]) == "unknown") {
            Value gap(rapidjson::kObjectType);
            gap.AddMember("path", Value(p.c_str(), a), a).AddMember("reason", "unknown", a);
            list.PushBack(gap, a);
        } else
            for (const auto& m : v.GetObject())
                if (Str(m.name) != "parameters") Gaps(m.value, p + "/" + Pointer(Str(m.name)), list, a);
    } else if (v.IsArray())
        for (size_t i = 0; i < v.Size(); ++i) Gaps(v[i], p + "/" + std::to_string(i), list, a);
}
}  // namespace
int SemanticsFail(DataSemanticsErrorV1* e, const std::string& code, const std::string& message, const std::string& p) {
    if (e) *e = {code, message, p};
    return -1;
}
bool SemanticsName(const std::string& n) {
    if (n.empty() || n.size() > 128 || n.front() < 'a' || n.front() > 'z') return false;
    for (char c : n)
        if (!(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9') && c != '.' && c != '_' && c != '-') return false;
    return true;
}
bool ParseSemanticsReference(const std::string& text, SemanticsIdentity* out) {
    const auto dot = text.find('.'), at = text.rfind('@');
    if (dot == std::string::npos || at == std::string::npos || at <= dot + 1) return false;
    SemanticsIdentity id{text.substr(0, dot), text.substr(dot + 1, at - dot - 1), 0};
    if ((id.kind != "standard" && id.kind != "dictionary" && id.kind != "binding") || !SemanticsName(id.name))
        return false;
    auto rev = text.substr(at + 1);
    if (rev.empty() || rev.front() == '0') return false;
    const auto parsed = std::from_chars(rev.data(), rev.data() + rev.size(), id.revision);
    if (parsed.ec != std::errc{} || parsed.ptr != rev.data() + rev.size() || id.revision > kSemanticsMaxRevision)
        return false;
    if (out) *out = id;
    return true;
}
std::string SemanticsJson(const Value& v) {
    rapidjson::StringBuffer b;
    rapidjson::Writer<rapidjson::StringBuffer> w(b);
    v.Accept(w);
    return {b.GetString(), b.GetSize()};
}
int ParseSemanticsJson(const std::string& text, rapidjson::Document* out, DataSemanticsErrorV1* e) {
    std::string error;
    if (text.size() > 12 * 1024 * 1024) return SemanticsFail(e, "LIMIT_EXCEEDED", "request exceeds 12 MiB");
    if (ValidateConfigContent("json", text, &error)) return SemanticsFail(e, "INVALID_DOCUMENT", error);
    rapidjson::Reader precision_reader;
    rapidjson::MemoryStream input(text.data(), text.size());
    IntegerPrecisionGuard precision_guard;
    if (!precision_reader.Parse<rapidjson::kParseNumbersAsStringsFlag>(input, precision_guard))
        return SemanticsFail(e, "INVALID_DOCUMENT", "integer exceeds lossless signed/unsigned 64-bit range");
    out->Parse<rapidjson::kParseValidateEncodingFlag>(text.data(), text.size());
    if (out->HasParseError()) return SemanticsFail(e, "INVALID_DOCUMENT", "invalid UTF-8 JSON");
    return 0;
}
const char* SemanticsDocumentSchema() { return kSemanticsSchema; }
int NormalizeSemantics(const std::string& kind, const std::string& text, rapidjson::Document* d,
                       std::vector<SemanticsReference>* refs, DataSemanticsErrorV1* e) {
    refs->clear();
    if (text.size() > kSemanticsMaxDocument) return SemanticsFail(e, "LIMIT_EXCEEDED", "document exceeds 8 MiB");
    if (ParseSemanticsJson(text, d, e)) return -1;
    if (!d->IsObject() || (kind != "standard" && kind != "dictionary" && kind != "binding"))
        return SemanticsFail(e, "INVALID_DOCUMENT", "invalid kind or document");
    Normalize(*d, kind, d->GetAllocator());
    rapidjson::Document schema;
    schema.Parse(kSemanticsSchema);
    rapidjson::SchemaDocument compiled(schema);
    rapidjson::SchemaValidator validator(compiled);
    if (!d->Accept(validator)) {
        rapidjson::StringBuffer path;
        validator.GetInvalidDocumentPointer().Stringify(path);
        return SemanticsFail(e, "INVALID_DOCUMENT", "document does not match published JSON Schema", path.GetString());
    }
    if (Str((*d)["schema_id"]) != "flowsql.data-" + kind + ".v1")
        return SemanticsFail(e, "INVALID_DOCUMENT", "schema_id differs from kind", "/schema_id");
    if (kind == "dictionary") {
        std::unordered_set<std::string> ids, names;
        for (auto& f : (*d)["fields"].GetArray()) {
            if (!ids.insert(Str(f["field_id"])).second || !names.insert(Str(f["name"])).second)
                return SemanticsFail(e, "INVALID_DOCUMENT", "duplicate field id or name", "/fields");
        }
        auto check_ids = [&](const Value& values, const std::string& path) {
            std::unordered_set<std::string> seen;
            for (const auto& id : values.GetArray())
                if (!ids.count(Str(id)) || !seen.insert(Str(id)).second)
                    return SemanticsFail(e, "INVALID_DOCUMENT", "unknown or repeated field_id", path);
            return 0;
        };
        const auto& entity = (*d)["entity"];
        for (const char* key : {"identity_fields", "time_fields"})
            if (entity[key]["value"].IsArray() && check_ids(entity[key]["value"], std::string("/entity/") + key))
                return -1;
        if (entity["revision_field"]["value"].IsString() && !ids.count(Str(entity["revision_field"]["value"])))
            return SemanticsFail(e, "INVALID_DOCUMENT", "unknown revision field", "/entity/revision_field");
        for (size_t i = 0; i < (*d)["fields"].Size(); ++i) {
            const auto& f = (*d)["fields"][i];
            const std::string path = "/fields/" + std::to_string(i);
            if (f["validity"]["value"].IsObject() && check_ids(f["validity"]["value"]["field_ids"], path + "/validity"))
                return -1;
            if (f["aggregation"]["value"].IsArray())
                for (const auto& v : f["aggregation"]["value"].GetArray())
                    if (check_ids(v["condition"]["field_ids"], path + "/aggregation")) return -1;
            if (f["standard_refs"]["value"].IsArray())
                for (size_t j = 0; j < f["standard_refs"]["value"].Size(); ++j) {
                    const std::string target = Str(f["standard_refs"]["value"][j]);
                    SemanticsIdentity id;
                    if (!ParseSemanticsReference(target, &id) || id.kind != "standard")
                        return SemanticsFail(e, "INVALID_REFERENCE", "standard requires an exact standard version",
                                             path + "/standard_refs");
                    refs->push_back({target, path + "/standard_refs/value/" + std::to_string(j)});
                }
        }
        if (entity["relations"]["value"].IsArray())
            for (size_t i = 0; i < entity["relations"]["value"].Size(); ++i) {
                const auto& rel = entity["relations"]["value"][i];
                const auto target = Str(rel["dictionary_ref"]);
                SemanticsIdentity id;
                if (!ParseSemanticsReference(target, &id) || id.kind != "dictionary")
                    return SemanticsFail(e, "INVALID_REFERENCE", "relation requires exact dictionary");
                for (const auto& m : rel["field_map"].GetObject())
                    if (!ids.count(Str(m.name)))
                        return SemanticsFail(e, "INVALID_DOCUMENT", "unknown relation source field");
                refs->push_back({target, "/entity/relations/value/" + std::to_string(i) + "/dictionary_ref"});
            }
    }
    if (kind == "binding") {
        SemanticsIdentity id;
        const auto target = Str((*d)["dictionary_ref"]);
        if (!ParseSemanticsReference(target, &id) || id.kind != "dictionary")
            return SemanticsFail(e, "INVALID_REFERENCE", "binding requires an exact dictionary", "/dictionary_ref");
        refs->push_back({target, "/dictionary_ref"});
    }
    Sort(*d);
    if (SemanticsJson(*d).size() > kSemanticsMaxDocument)
        return SemanticsFail(e, "LIMIT_EXCEEDED", "normalized document exceeds 8 MiB");
    return 0;
}
int CheckSemanticsBinding(const Value& b, const Value& dictionary, std::string* check, DataSemanticsErrorV1* e) {
    const auto& mapping = b["field_map"];
    const auto& fields = dictionary["fields"];
    if (mapping.MemberCount() != fields.Size())
        return SemanticsFail(e, "BINDING_MISMATCH", "mapping must cover every dictionary field", "/field_map");
    std::unordered_set<std::string> source_names;
    for (const auto& f : fields.GetArray()) {
        const auto key = Str(f["field_id"]);
        if (!mapping.HasMember(key.c_str()) || !source_names.insert(Str(mapping[key.c_str()])).second)
            return SemanticsFail(e, "BINDING_MISMATCH", "mapping missing or source field repeated", "/field_map");
    }
    *check = "unknown";
    if (b["declared_schema"].IsNull() || b["declared_schema"].Empty()) return 0;
    std::unordered_set<std::string> names;
    for (const auto& source : b["declared_schema"].GetArray())
        if (!names.insert(Str(source["name"])).second)
            return SemanticsFail(e, "BINDING_MISMATCH", "duplicate declared source field", "/declared_schema");
    for (const auto& f : fields.GetArray()) {
        const auto source_name = Str(mapping[Str(f["field_id"]).c_str()]);
        const Value* source = nullptr;
        for (const auto& candidate : b["declared_schema"].GetArray())
            if (Str(candidate["name"]) == source_name) source = &candidate;
        if (!source || (*source)["type"] != f["type"] || (*source)["nullable"] != f["nullable"])
            return SemanticsFail(e, "BINDING_MISMATCH", "missing source or incompatible declared type/nullability",
                                 "/declared_schema");
    }
    *check = "matched";
    return 0;
}
rapidjson::Document SemanticsGaps(const Value& value) {
    rapidjson::Document out(rapidjson::kArrayType);
    Gaps(value, "", out, out.GetAllocator());
    return out;
}
}  // namespace flowsql::channels::config
