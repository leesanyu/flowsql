#!/bin/bash
# FlowSQL 单进程启动脚本
set -e
cd "$(dirname "$0")"
exec ./build/output/flowsql --config config/single.yaml
