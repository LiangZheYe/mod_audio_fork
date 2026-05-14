#!/bin/bash
# staircase_test.sh - 并发阶梯测试，逐步增加并发数
# 用法: ./staircase_test.sh [WS_URL]

WS_URL=${1:-"ws://127.0.0.1:8080"}
LEVELS=(10 25 50 100 200)
HOLD_SECS=120
FS_PID=$(pgrep freeswitch)

if [ -z "$FS_PID" ]; then
  echo "Error: freeswitch process not found"
  exit 1
fi

for LEVEL in "${LEVELS[@]}"; do
  echo "=== Level: $LEVEL concurrent sessions ==="
  UUIDS=()

  for i in $(seq 1 $LEVEL); do
    RESULT=$(fs_cli -x "originate user/1001 &park" 2>/dev/null)
    UUID=$(echo "$RESULT" | grep -oP '[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}' | head -1)
    if [ -n "$UUID" ]; then
      fs_cli -x "uuid_audio_inject $UUID start $WS_URL mono 8k" > /dev/null 2>&1
      UUIDS+=("$UUID")
    fi
  done

  SUCCESS=${#UUIDS[@]}
  echo "Successfully started: $SUCCESS / $LEVEL"

  FS_MEM=$(ps -o rss= -p $FS_PID | awk '{printf "%.1f MB", $1/1024}')
  WS_CONNS=$(ss -tnp | grep 8080 | wc -l)
  echo "FS Memory: $FS_MEM | WS Connections: $WS_CONNS"

  sleep $HOLD_SECS

  ALIVE=0
  for UUID in "${UUIDS[@]}"; do
    EXISTS=$(fs_cli -x "uuid_exists $UUID" 2>/dev/null | tr -d '[:space:]')
    if [ "$EXISTS" = "true" ]; then
      ALIVE=$((ALIVE + 1))
    fi
  done
  echo "Alive after ${HOLD_SECS}s: $ALIVE / $SUCCESS"

  for UUID in "${UUIDS[@]}"; do
    fs_cli -x "uuid_audio_inject $UUID stop" > /dev/null 2>&1
    fs_cli -x "uuid_kill $UUID" > /dev/null 2>&1
  done

  sleep 30

  FS_MEM_AFTER=$(ps -o rss= -p $FS_PID | awk '{printf "%.1f MB", $1/1024}')
  echo "FS Memory after cleanup: $FS_MEM_AFTER"
  echo ""
done
