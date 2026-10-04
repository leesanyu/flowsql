#!/usr/bin/env bash
# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
set -euo pipefail

if [[ ${1:-} == --inside ]]; then
    shift
    if [[ $(readlink /proc/self/ns/net) == $(readlink /proc/1/ns/net) ]]; then
        echo 'PF_RING smoke requires a private network namespace' >&2
        exit 2
    fi
    ip link set lo up
    ip link add na0 type veth peer name np0
    ip link add nb0 type veth peer name nq0
    for validation_interface in na0 np0 nb0 nq0; do
        ip link set "$validation_interface" up
    done
    if [[ $2 == abrupt ]]; then
        # exec leaves no shell retaining the netns after the test process exits.
        exec "$1" --abrupt-exit na0 nb0 np0 nq0
    fi
    exec "$1" na0 nb0 np0 nq0
fi
if [[ $# != 2 ]]; then
    echo "usage: $0 test_pfring_backend artifact_directory" >&2
    exit 2
fi
validation_executable=$(realpath "$1")
validation_artifacts=$(realpath -m "$2")
mkdir -p "$validation_artifacts"
dmesg --color=never > "$validation_artifacts/kernel-before.log"
for validation_mode in normal abrupt; do
    for validation_round in 1 2 3; do
        unshare --net bash "$0" --inside "$validation_executable" "$validation_mode" \
            > "$validation_artifacts/$validation_mode-$validation_round.log" 2>&1
        cat "$validation_artifacts/$validation_mode-$validation_round.log"
    done
done
# The last userspace exit can release files through delayed fput in a worker.
sleep 1
cat /proc/net/pf_ring/info > "$validation_artifacts/rings-after.log"
dmesg --color=never > "$validation_artifacts/kernel-after.log"
python3 - "$validation_artifacts" <<'PY'
import pathlib
import re
import sys

root = pathlib.Path(sys.argv[1])
before = (root / 'kernel-before.log').read_text().splitlines()
after = (root / 'kernel-after.log').read_text().splitlines()
assert after[:len(before)] == before, 'kernel log baseline rotated; repeat validation'
delta = after[len(before):]
(root / 'kernel-delta.log').write_text('\n'.join(delta) + '\n')
assert not any(re.search(r'WARNING:|Oops:|BUG:|leaking at least|non-empty directory', line) for line in delta), delta
assert re.search(r'Total rings\s*:\s*0\b', (root / 'rings-after.log').read_text())
print('PF_RING normal/abrupt netns exit: 3+3 runs, rings=0, no new kernel warning')
PY
