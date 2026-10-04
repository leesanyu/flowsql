#!/usr/bin/env bash
# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
set -euo pipefail

if [[ ${1:-} != --inside ]]; then
    if [[ $# != 1 ]]; then
        echo "usage: $0 test_af_xdp_backend" >&2
        exit 2
    fi
    exec unshare --mount --net bash "$0" --inside "$@"
fi
shift
if [[ $(readlink /proc/self/ns/net) == $(readlink /proc/1/ns/net) ||
      $(readlink /proc/self/ns/mnt) == $(readlink /proc/1/ns/mnt) ]]; then
    echo 'backend smoke requires private network and mount namespaces' >&2
    exit 2
fi
smoke_executable=$(realpath "$1")
# Mount sysfs for this network namespace so queue discovery sees these TAP devices.
mount --make-rprivate /
mount -t sysfs -o nosuid,nodev,noexec sysfs /sys
ip link set lo up
# Use a temporary device node; the existing kernel TUN/TAP driver supplies real RX queues.
# Last queue-fd close removes the nonpersistent TAP devices. No host network interface is touched.
smoke_tun_node="/tmp/flowsql-netadapter-tun-${BASHPID}"
mknod "$smoke_tun_node" c 10 200
trap 'unlink "$smoke_tun_node"' EXIT
"$smoke_executable" nta0 ntb0 "$smoke_tun_node"
