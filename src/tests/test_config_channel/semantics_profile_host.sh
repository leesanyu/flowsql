#!/bin/sh
# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd) || exit 1
cd "$(dirname "$FLOWSQL_SEMANTICS_PROFILE_HOST")" || exit 1
exec gdb --quiet --nx --batch --return-child-result -x "$script_dir/semantics_profile.py" --args "$FLOWSQL_SEMANTICS_PROFILE_HOST" "$@"
