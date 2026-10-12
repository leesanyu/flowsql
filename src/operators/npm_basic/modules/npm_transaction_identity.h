// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include <operators/npm_basic/npm_ip_family_contract.h>

namespace flowsql::npm {
// Retained callbacks own only the actual family's key allocation, never address text or a maximum-width union.
struct NpmTransactionIdentity {
    uint64_t session_id = 0;
    NpmOwnedSessionKey key;
    uint64_t charge = 0;
};
}  // namespace flowsql::npm
