// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <cctype>
#include <common/operator_contract_control.hpp>
#include <memory>
#include <regex>
#include <set>
#include "catalog_plugin.h"

namespace flowsql::catalog {
namespace {
using namespace dataset;
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
Statement PrepareSql(sqlite3* db, const char* sql) {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    return {stmt, sqlite3_finalize};
}
void Bind(sqlite3_stmt* stmt, int index, const std::string& text) {
    sqlite3_bind_text(stmt, index, text.c_str(), static_cast<int>(text.size()), SQLITE_TRANSIENT);
}
std::string Column(sqlite3_stmt* stmt, int index) {
    const auto* text = sqlite3_column_text(stmt, index);
    return text ? reinterpret_cast<const char*>(text) : "";
}
bool Digest(const std::string& s) {
    return s.size() == 64 && s.find_first_not_of("0123456789abcdef") == std::string::npos;
}
bool Identity(const std::string& s) {
    return !s.empty() && s.size() <= 128 && std::regex_match(s, std::regex("[A-Za-z0-9_][A-Za-z0-9_.-]*"));
}
Json Request(const std::string& category, const std::string& name) {
    Json r(rapidjson::kObjectType);
    Set(r, "kind", "operator_contract", r.GetAllocator());
    Set(r, "name", category + "." + name, r.GetAllocator());
    return r;
}
int DefinitionIdentity(const rapidjson::Value& d, OperatorComponentContractV1* c, DataSemanticsErrorV1* e) {
    if (dataset::Get(d, "schema_id") != "flowsql.operator-data-contract.v1" || !d.HasMember("operator") ||
        dataset::Get(d["operator"], "category") != c->category || dataset::Get(d["operator"], "name") != c->name)
        return Fail(e, "CONTRACT_INVALID", "declaration and actual component operator identity differ", "/operator");
    c->operator_version = dataset::Get(d["operator"], "version");
    c->contract_state = "declared";
    return 0;
}
}  // namespace

int CatalogPlugin::Prepare(const std::vector<OperatorContractDeclarationV1>& declarations,
                           std::vector<PreparedOperatorContractV1>* prepared, DataSemanticsErrorV1* e) {
    DataSemanticsErrorV1 local_error;
    if (!e) e = &local_error;
    *e = {};
    if (!prepared) return dataset::Fail(e, "INVALID_REQUEST", "preparation output required");
    prepared->clear();
    if (declarations.empty() || declarations.size() > 256)
        return dataset::Fail(e, "CONTRACT_INVALID", "component must contain 1..256 operator declarations");
    std::vector<PreparedOperatorContractV1> candidates;
    std::vector<std::string> documents;
    std::set<std::string> identities;
    // Validate the entire component before the first definition write. No catalog lock.
    for (const auto& declaration : declarations) {
        const auto& x = declaration;
        if (!Identity(x.category) || !Identity(x.name) || !Digest(x.component_revision) ||
            (!x.expected_component_revision.empty() && !Digest(x.expected_component_revision)))
            return dataset::Fail(e, "CONTRACT_INVALID", "valid operator identity and host component SHA256 required");
        auto key = x.category + "." + x.name;
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return std::tolower(c); });
        if (!identities.insert(key).second)
            return dataset::Fail(e, "CONTRACT_INVALID", "duplicate component operator declaration");
        PreparedOperatorContractV1 candidate;
        candidate.expected_component_revision = x.expected_component_revision;
        auto& association = candidate.association;
        association.category = x.category;
        association.name = x.name;
        association.component_revision = x.component_revision;
        std::string normalized;
        if (!x.declaration_json.empty()) {
            if (x.declaration_json.size() > 8 * 1024 * 1024 || !JsonNestingWithin(x.declaration_json, 64))
                return dataset::Fail(e, "LIMIT_EXCEEDED", "contract declaration exceeds document limits");
            Json d;
            d.Parse(x.declaration_json.c_str());
            if (d.HasParseError() || !d.IsObject())
                return dataset::Fail(e, "CONTRACT_INVALID", "contract declaration must be JSON object");
            Json request, response;
            if (d.HasMember("contract_ref")) {
                const auto ref = dataset::Get(d, "contract_ref");
                const auto prefix = "operator_contract." + x.category + "." + x.name + "@";
                if (d.MemberCount() != 1 || ref.compare(0, prefix.size(), prefix) != 0)
                    return dataset::Fail(e, "CONTRACT_INVALID", "exact contract reference for this operator required");
                request.SetObject();
                Set(request, "exact_reference", ref, request.GetAllocator());
                if (contracts::Control(querier_, "resolve", request, &response, e)) return -1;
                if (DefinitionIdentity(response["data"]["document"], &association, e)) return -1;
                association.contract_ref = ref;
            } else {
                request = Request(x.category, x.name);
                auto& a = request.GetAllocator();
                Set(request, "format", "json", a);
                Set(request, "content", x.declaration_json, a);
                if (contracts::Control(querier_, "import", request, &response, e)) return -1;
                const auto& doc = response["data"]["document"];
                if (DefinitionIdentity(doc, &association, e)) return -1;
                normalized = Encode(doc);
            }
        }
        candidates.push_back(std::move(candidate));
        documents.push_back(std::move(normalized));
    }
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (documents[i].empty()) continue;
        auto& association = candidates[i].association;
        auto history = Request(association.category, association.name);
        history.AddMember("limit", 1, history.GetAllocator());
        Json response;
        uint64_t current = 0;
        if (contracts::Control(querier_, "history", history, &response, e)) {
            if (!e || e->code != "NOT_FOUND") return -1;
            *e = {};
        } else if (!response["data"]["items"].Empty()) {
            current = response["data"]["items"][0]["revision"].GetUint64();
        }
        auto save = Request(association.category, association.name);
        auto& a = save.GetAllocator();
        Json document;
        document.Parse(documents[i].c_str());
        save.AddMember("document", Copy(document, a), a).AddMember("expected_current_revision", current, a);
        if (contracts::Control(querier_, "save", save, &response, e)) return -1;
        association.contract_ref = dataset::Get(response["data"], "exact_reference");
    }
    *prepared = std::move(candidates);
    return 0;
}

int CatalogPlugin::Commit(const std::vector<PreparedOperatorContractV1>& prepared, DataSemanticsErrorV1* e) {
    if (e) *e = {};
    if (prepared.empty() || prepared.size() > 256)
        return dataset::Fail(e, "CONTRACT_INVALID", "association batch must contain 1..256 operators");
    std::set<std::string> keys;
    for (const auto& p : prepared) {
        const auto& c = p.association;
        auto key = c.category + "." + c.name;
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) { return std::tolower(ch); });
        if (!Identity(c.category) || !Identity(c.name) || !Digest(c.component_revision) ||
            (!p.expected_component_revision.empty() && !Digest(p.expected_component_revision)) ||
            !keys.insert(key).second || (c.contract_state != "declared" && c.contract_state != "missing") ||
            (c.contract_state == "declared" && (c.contract_ref.empty() || c.operator_version.empty())) ||
            (c.contract_state == "missing" && (!c.contract_ref.empty() || !c.operator_version.empty())))
            return dataset::Fail(e, "CONTRACT_INVALID", "invalid prepared component association");
        if (c.contract_state == "declared") {
            auto* definitions =
                querier_ ? static_cast<IDataSemanticsRegistryV1*>(querier_->First(IID_DATA_SEMANTICS_REGISTRY_V1))
                         : nullptr;
            if (!definitions) return dataset::Fail(e, "UNAVAILABLE", "declared contract provider unavailable");
            DataSemanticsSnapshotV1 snapshot;
            if (definitions->Resolve(c.contract_ref.c_str(), &snapshot, e)) return -1;
            Json document;
            document.Parse(snapshot.document_json->c_str());
            auto observed = c;
            if (DefinitionIdentity(document, &observed, e)) return -1;
            if (observed.operator_version != c.operator_version)
                return dataset::Fail(e, "CONTRACT_INVALID", "prepared version differs from immutable declaration");
        }
    }
    std::lock_guard<std::mutex> lock(mu_);
    if (!operator_db_) return dataset::Fail(e, "UNAVAILABLE", "operator catalog unavailable");
    if (sqlite3_exec(operator_db_, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK)
        return dataset::Fail(e, "STORAGE_ERROR", sqlite3_errmsg(operator_db_));
    struct Transaction {
        sqlite3* db;
        ~Transaction() {
            if (db) sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        }
    } tx{operator_db_};
    for (const auto& p : prepared) {
        const auto& c = p.association;
        auto query = PrepareSql(operator_db_,
                                "SELECT component_revision FROM operator_contract_association "
                                "WHERE category=? COLLATE NOCASE AND name=? COLLATE NOCASE");
        if (!query) return dataset::Fail(e, "STORAGE_ERROR", sqlite3_errmsg(operator_db_));
        Bind(query.get(), 1, c.category);
        Bind(query.get(), 2, c.name);
        const int rc = sqlite3_step(query.get());
        if (rc != SQLITE_ROW && rc != SQLITE_DONE)
            return dataset::Fail(e, "STORAGE_ERROR", sqlite3_errmsg(operator_db_));
        const auto current = rc == SQLITE_ROW ? Column(query.get(), 0) : "";
        if (current != p.expected_component_revision)
            return dataset::Fail(e, "COMPONENT_CHANGED", "component association changed; re-observe before retry");
        auto save = PrepareSql(operator_db_,
                               "INSERT INTO operator_contract_association VALUES(?,?,?,?,?,?) "
                               "ON CONFLICT(category,name) DO UPDATE SET "
                               "component_revision=excluded.component_revision,"
                               "operator_version=excluded.operator_version,contract_ref=excluded.contract_ref,"
                               "contract_state=excluded.contract_state");
        if (!save) return dataset::Fail(e, "STORAGE_ERROR", sqlite3_errmsg(operator_db_));
        const std::string values[]{c.category,         c.name,         c.component_revision,
                                   c.operator_version, c.contract_ref, c.contract_state};
        for (int i = 0; i < 6; ++i) Bind(save.get(), i + 1, values[i]);
        if (sqlite3_step(save.get()) != SQLITE_DONE)
            return dataset::Fail(e, "STORAGE_ERROR", sqlite3_errmsg(operator_db_));
    }
    if (sqlite3_exec(operator_db_, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK)
        return dataset::Fail(e, "STORAGE_ERROR", sqlite3_errmsg(operator_db_));
    tx.db = nullptr;
    return 0;
}

int CatalogPlugin::Lookup(const char* category, const char* name, OperatorComponentContractV1* out,
                          DataSemanticsErrorV1* e) {
    if (e) *e = {};
    if (!out) return dataset::Fail(e, "INVALID_REQUEST", "association output required");
    *out = {};
    if (!category || !name || !Identity(category) || !Identity(name))
        return dataset::Fail(e, "CONTRACT_INVALID", "operator identity required");
    std::lock_guard<std::mutex> lock(mu_);
    if (!operator_db_) return dataset::Fail(e, "UNAVAILABLE", "operator catalog unavailable");
    auto query = PrepareSql(operator_db_,
                            "SELECT category,name,component_revision,operator_version,contract_ref,"
                            "contract_state FROM operator_contract_association "
                            "WHERE category=? COLLATE NOCASE AND name=? COLLATE NOCASE");
    if (!query) return dataset::Fail(e, "STORAGE_ERROR", sqlite3_errmsg(operator_db_));
    Bind(query.get(), 1, category);
    Bind(query.get(), 2, name);
    const int rc = sqlite3_step(query.get());
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) return dataset::Fail(e, "STORAGE_ERROR", sqlite3_errmsg(operator_db_));
    if (rc == SQLITE_ROW)
        *out = {Column(query.get(), 0), Column(query.get(), 1), Column(query.get(), 2),
                Column(query.get(), 3), Column(query.get(), 4), Column(query.get(), 5)};
    else {
        out->category = category;
        out->name = name;
    }
    return 0;
}
}  // namespace flowsql::catalog
