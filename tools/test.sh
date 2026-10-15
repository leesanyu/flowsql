#!/usr/bin/env bash
# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
set -euo pipefail
cd "$(dirname "$0")/.."
exec python3 tools/test_environment/runner.py "$@"
