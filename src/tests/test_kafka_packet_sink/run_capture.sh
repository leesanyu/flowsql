#!/usr/bin/env bash
# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
set -euo pipefail
if [[ ${1:-} != --inside ]]; then
    test $# -eq 1
    exec unshare --net bash "$0" --inside "$(realpath "$1")"
fi
shift
if [[ $(readlink /proc/self/ns/net) == $(readlink /proc/1/ns/net) ]]; then
    echo 'execution_denied: private network namespace required' >&2
    exit 1
fi
sysctl -q -w net.ipv6.conf.all.disable_ipv6=1
sysctl -q -w net.ipv6.conf.default.disable_ipv6=1
ip link set lo up
ip link add ps0 type veth peer name pt0
ip link set ps0 up
ip link set pt0 up
"$1" ps0 pt0
