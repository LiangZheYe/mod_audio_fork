#!/bin/bash
# leak_detector.sh - 检测 WebSocket 连接泄漏
# 用法: ./leak_detector.sh [WS_PORT] [EXPECTED_MAX]

WS_PORT=${1:-8080}
EXPECTED=${2:-0}

echo "Leak detector: WS_PORT=$WS_PORT expected_max=$EXPECTED"

while true; do
  ACTUAL=$(ss -tnp | grep "$WS_PORT" | wc -l)
  if [ "$ACTUAL" -gt "$EXPECTED" ]; then
    echo "$(date): LEAK DETECTED - $ACTUAL connections (expected <= $EXPECTED)"
    ss -tnp | grep "$WS_PORT"
  fi
  sleep 30
done
