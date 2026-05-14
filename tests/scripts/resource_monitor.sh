#!/bin/bash
# resource_monitor.sh - 持续监控 FreeSWITCH 资源使用
# 用法: ./resource_monitor.sh [WS_PORT] [INTERVAL]

WS_PORT=${1:-8080}
INTERVAL=${2:-10}
OUTPUT="resource_log.csv"

FS_PID=$(pgrep freeswitch)
if [ -z "$FS_PID" ]; then
  echo "Error: freeswitch process not found"
  exit 1
fi

echo "Monitoring PID=$FS_PID WS_PORT=$WS_PORT interval=${INTERVAL}s"
echo "Output: $OUTPUT"

echo "timestamp,rss_mb,cpu_pct,ws_conns,fds,threads" > "$OUTPUT"

while true; do
  TIMESTAMP=$(date '+%Y-%m-%d %H:%M:%S')
  RSS=$(ps -o rss= -p $FS_PID | awk '{printf "%.1f", $1/1024}')
  CPU=$(ps -o %cpu= -p $FS_PID | tr -d ' ')
  WS_CONNS=$(ss -tnp | grep "$WS_PORT" | wc -l)
  FDS=$(ls /proc/$FS_PID/fd 2>/dev/null | wc -l)
  THREADS=$(ls /proc/$FS_PID/task 2>/dev/null | wc -l)

  echo "$TIMESTAMP,$RSS,$CPU,$WS_CONNS,$FDS,$THREADS" >> "$OUTPUT"
  sleep $INTERVAL
done
