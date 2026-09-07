#!/bin/bash
# FlowSQL 单进程启动脚本
set -euo pipefail

repository_root="$(cd "$(dirname "$0")" && pwd)"

usage() {
    printf 'Usage: %s [--build-frontend]\n' "$0"
}

build_frontend=false
case "${1:-}" in
    "")
        ;;
    --build-frontend)
        build_frontend=true
        shift
        ;;
    -h|--help)
        usage
        exit 0
        ;;
    *)
        printf 'Unknown option: %s\n' "$1" >&2
        usage >&2
        exit 2
        ;;
esac

if [[ $# -ne 0 ]]; then
    printf 'Unknown option: %s\n' "$1" >&2
    usage >&2
    exit 2
fi

if [[ "$build_frontend" == true ]]; then
    printf '%s\n' '[start] Building frontend...'
    npm run build --prefix "$repository_root/src/frontend"

    printf '%s\n' '[start] Deploying frontend to build/output/static...'
    cmake -E remove_directory "$repository_root/build/output/static"
    cmake -E copy_directory \
        "$repository_root/src/frontend/dist" \
        "$repository_root/build/output/static"
    test -f "$repository_root/build/output/static/index.html"
fi

cd "$(dirname "$0")/build/output"
export LD_LIBRARY_PATH="$PWD${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec ./flowsql --config config/deploy-single.yaml
