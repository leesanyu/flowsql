#!/usr/bin/env bash
# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
set -euo pipefail

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
compose=(docker compose -p flowsql-kafka-manual -f "$repo_dir/config/docker-compose-kafka-manual.yml")
source_topic=flowsql.manual.source
sink_topic=flowsql.manual.sink

help_text() {
    cat <<'HELP'
用法：./tools/kafka-manual.sh <命令>
  up                    启动 Kafka，等待就绪，幂等创建 source/sink Topic
  status                查看容器及 Topic
  seed                  向 source 写入三条 host/time/value JSON 样例
  produce               从 stdin 向 source 逐行写入 JSON（Ctrl-D 结束）
  read [source|sink] [N] 从头查看消息；可指定条数 N，默认持续等待，Ctrl-C 结束
  logs                  查看最近 100 行 broker 日志
  down                  停止 broker，保留容器、网络、数据及消费位点
  help                  显示帮助

原生 FlowSQL：127.0.0.1:19092，PLAINTEXT，无账号密码。
Docker FlowSQL：加入 flowsql-kafka-manual 网络后，使用 manual-kafka:9092。
Topic：flowsql.manual.source / flowsql.manual.sink；完整配置和 SQL 见 docs/kafka.md。
HELP
}

kafka() {
    "${compose[@]}" exec -T manual-kafka "/opt/kafka/bin/$1" "${@:2}"
}

produce() {
    kafka kafka-console-producer.sh --bootstrap-server manual-kafka:9092 --topic "$source_topic" \
        --producer-property acks=all --producer-property delivery.timeout.ms=10000 \
        --producer-property request.timeout.ms=5000 --producer-property max.block.ms=10000
}

case "${1:-help}" in
    up)
        "${compose[@]}" up -d --wait --wait-timeout 120
        for topic in "$source_topic" "$sink_topic"; do
            kafka kafka-topics.sh --bootstrap-server manual-kafka:9092 --create --if-not-exists \
                --topic "$topic" --partitions 1 --replication-factor 1
        done
        help_text
        ;;
    status)
        "${compose[@]}" ps -a
        for topic in "$source_topic" "$sink_topic"; do
            kafka kafka-topics.sh --bootstrap-server manual-kafka:9092 --describe --topic "$topic"
        done
        ;;
    seed)
        python3 - <<'PY' | produce
import json
import time

now = time.time_ns() // 1_000_000
for index, value in enumerate((1024.0, 2048.0, 4096.0), 1):
    print(json.dumps({"host": f"api-{index}", "time": now + index, "value": value}))
PY
        ;;
    produce)
        produce
        ;;
    read)
        case "${2:-sink}" in
            source) topic="$source_topic" ;;
            sink) topic="$sink_topic" ;;
            *) echo 'read 只接受 source 或 sink' >&2; exit 2 ;;
        esac
        count_args=()
        if [[ -n "${3:-}" ]]; then
            if [[ ! "$3" =~ ^[1-9][0-9]*$ ]]; then
                echo '消息条数必须为正整数' >&2
                exit 2
            fi
            count_args=(--max-messages "$3" --timeout-ms 15000)
        fi
        kafka kafka-console-consumer.sh --bootstrap-server manual-kafka:9092 --topic "$topic" \
            --from-beginning --partition 0 --consumer-property enable.auto.commit=false "${count_args[@]}" |
            awk -v expected="${3:-0}" '{ print; fflush() } END {
                if (NR < expected) {
                    print "消息数量不足：实际 " NR "，预期 " expected > "/dev/stderr"
                    exit 1
                }
            }'
        ;;
    logs)
        "${compose[@]}" logs --tail 100 manual-kafka
        ;;
    down)
        "${compose[@]}" stop --timeout 20 manual-kafka
        ;;
    help|--help|-h)
        help_text
        ;;
    *)
        help_text >&2
        exit 2
        ;;
esac
