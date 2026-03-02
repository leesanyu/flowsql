#!/bin/bash
# FlowSQL Web 服务器启动脚本

cd "$(dirname "$0")/build/output"

echo "=========================================="
echo "  启动 FlowSQL Web 管理系统"
echo "=========================================="
echo ""
echo "服务地址: http://localhost:8081"
echo ""
echo "按 Ctrl+C 停止服务器"
echo ""

./flowsql_web
