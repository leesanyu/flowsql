#!/usr/bin/env bash
# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
set -euo pipefail

if [[ ${1:-} != --inside ]]; then
    if [[ $# != 9 && $# != 10 ]]; then
        echo "usage: $0 executable capture|sql|sql_all backend interval_s duration_s ticks_per_s frame_bytes stop|cancel artifact.json [profile]" >&2
        exit 2
    fi
    exec unshare --net bash "$0" --inside "$@"
fi
shift
if [[ $(readlink /proc/self/ns/net) == $(readlink /proc/1/ns/net) ]]; then
    echo 'validation requires a private network namespace' >&2
    exit 2
fi
validation_executable=$(realpath "$1")
validation_artifact=$(realpath -m "$9")
ip link set lo up
ip link add na0 numrxqueues 2 numtxqueues 2 type veth peer name np0 numrxqueues 2 numtxqueues 2
ip link add nb0 numrxqueues 2 numtxqueues 2 type veth peer name nq0 numrxqueues 2 numtxqueues 2
for validation_interface in na0 np0 nb0 nq0; do
    ip link set "$validation_interface" up
done
mkdir -p "$(dirname "$validation_artifact")"
"$validation_executable" "$2" "$3" na0 nb0 np0 nq0 "$4" "$5" "$6" "$7" "$8" "$validation_artifact" "${10:-dual}"
ip -details -json link show na0 > "${validation_artifact%.json}-interface-a.json"
ip -details -json link show nb0 > "${validation_artifact%.json}-interface-b.json"
if [[ $3 == pfring_classic ]]; then
    cat /proc/net/pf_ring/info > "${validation_artifact%.json}-pfring-info.txt"
    python3 - "$validation_artifact" <<'PY'
import json
import pathlib
import re
import sys

base = pathlib.Path(sys.argv[1]).with_suffix('')
entries = [entry for entry in pathlib.Path('/proc/net/pf_ring').iterdir()
           if entry.is_file() and entry.name != 'info']
assert not entries, entries
assert not list(pathlib.Path('/proc/net/pf_ring/stats').iterdir())
assert re.search(r'Total rings\s*:\s*0\b', pathlib.Path(str(base) + '-pfring-info.txt').read_text())
for suffix in ['-interface-a.json', '-interface-b.json']:
    interface = json.loads(pathlib.Path(str(base) + suffix).read_text())[0]
    assert interface['promiscuity'] == 0 and 'PROMISC' not in interface['flags'], interface
print('PF_RING production reader closed: same-netns socket nodes/rings=0, both promiscuity=0')
PY
fi
# Namespace exit releases only these temporary interfaces; no host interface is touched.
