// Copyright (C) 2026 LIHUO. All rights reserved.
// Licensed under the MIT License.
#include <framework/interfaces/cpp_operator_plugin_abi.h>

// A non-current ABI must be rejected before looking for its capability exports.
extern "C" int flowsql_abi_version() { return 1; }
extern "C" int flowsql_operator_count() { return 1; }
