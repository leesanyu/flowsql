// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include <arrow/result.h>
#include <arrow/type.h>

#include <memory>
#include <string_view>

namespace flowsql::database {
class IDbSession;

// Recover direct columns of a single managed relation from its persisted Schema contract.
// Expressions keep their database-inferred types; aliases of direct columns retain field metadata.
arrow::Result<std::shared_ptr<arrow::Schema>> ManagedQuerySchema(IDbSession* session, std::string_view backend,
                                                                 std::string_view query,
                                                                 const std::shared_ptr<arrow::Schema>& physical);
}  // namespace flowsql::database
