#!/bin/bash
# soak_monitor.sh - 每 60 秒采集一次内存和连接状态
# 用法: ./soak_monitor.sh <UUID> [WS_PORT]

UUID=$1
WS_PORT=${2:-8080}
LOG_FILE="soak_test_$(date +%Y%m%d_%H%M%S).log"

if [ -z "$UUID" ]; then
  echo "Usage: $0 <UUID> [WS_PORT]"
  exit 1
fi

FS_PID=$(pgrep freeswitch)
if [ -z "$FS_PID" ]; then
  echo "Error: freeswitch process not found"
  exit 1
fi

echo "Monitoring UUID=$UUID PID=$FS_PID WS_PORT=$WS_PORT"
echo "Log file: $LOG_FILE"

while true; do
  TIMESTAMP=$(date '+%Y-%m-%d %H:%M:%S')

  FS_MEM=$(ps -o rss= -p $FS_PID | awk '{printf "%.1f MB", $1/1024}')
  ACTIVE=$(fs_cli -x "uuid_dump $UUID" 2>/dev/null | grep -c "audio_inject" || echo "0")
  WS_CONNS=$(ss -tnp | grep "$WS_PORT" | wc -l)

  echo "$TIMESTAMP | FS_MEM: $FS_MEM | WS_CONNS: $WS_CONNS | Active: $ACTIVE" | tee -a "$LOG_FILE"

  sleep 60
done
