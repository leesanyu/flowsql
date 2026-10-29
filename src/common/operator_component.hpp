// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <dlfcn.h>
#include <framework/interfaces/ioperator_contract_catalog.h>
#include <openssl/evp.h>
#include <common/operator_contract_control.hpp>
#include <fstream>
#include <memory>

namespace flowsql::contracts {
inline std::string ComponentDigest(const void* symbol) {
    Dl_info info{};
    if (!dladdr(symbol, &info) || !info.dli_fname) return {};
    std::ifstream input(info.dli_fname, std::ios::binary);
    if (!input) return {};
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1) return {};
    char buffer[65536];
    while (input) {
        input.read(buffer, sizeof(buffer));
        if (EVP_DigestUpdate(ctx.get(), buffer, input.gcount()) != 1) return {};
    }
    if (!input.eof()) return {};
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (EVP_DigestFinal_ex(ctx.get(), digest, &length) != 1) return {};
    return dataset::Hex(digest, length);
}
inline int RegisterComponent(IQuerier* querier, const std::string& category, const std::string& name,
                             const std::string& digest, const std::string& declaration,
                             OperatorComponentContractV1* out, DataSemanticsErrorV1* error) {
    *out = {category, name, digest, "", "", declaration.empty() ? "missing" : "unavailable"};
    auto* catalog =
        querier ? static_cast<IOperatorContractCatalogV1*>(querier->First(IID_OPERATOR_CONTRACT_CATALOG_V1)) : nullptr;
    if (!catalog) return dataset::Fail(error, "UNAVAILABLE", "operator contract catalog unavailable");
    OperatorComponentContractV1 previous;
    if (catalog->Lookup(category.c_str(), name.c_str(), &previous, error)) return -1;
    std::vector<PreparedOperatorContractV1> prepared;
    if (catalog->Prepare({{category, name, digest, declaration, previous.component_revision}}, &prepared, error) ||
        catalog->Commit(prepared, error))
        return -1;
    *out = prepared.front().association;
    return 0;
}
}  // namespace flowsql::contracts
