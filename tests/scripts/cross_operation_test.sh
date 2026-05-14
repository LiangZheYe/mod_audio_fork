#!/bin/bash
# cross_operation_test.sh - 并发流下交叉操作测试
# 用法: ./cross_operation_test.sh [CONCURRENT] [WS_URL]

CONCURRENT=${1:-20}
WS_URL=${2:-"ws://127.0.0.1:8080"}

echo "Starting $CONCURRENT sessions for cross-operation test..."

UUIDS=()
for i in $(seq 1 $CONCURRENT); do
  RESULT=$(fs_cli -x "originate user/1001 &park" 2>/dev/null)
  UUID=$(echo "$RESULT" | grep -oP '[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}' | head -1)
  if [ -n "$UUID" ]; then
    fs_cli -x "uuid_audio_inject $UUID start $WS_URL mono 8k" > /dev/null 2>&1
    UUIDS+=("$UUID")
  fi
done

echo "Started ${#UUIDS[@]} sessions. Running cross operations..."

for UUID in "${UUIDS[@]}"; do
  OP=$(( RANDOM % 4 ))
  case $OP in
    0) # pause/resume
      echo "  $UUID: pause -> resume"
      fs_cli -x "uuid_audio_inject $UUID pause" > /dev/null 2>&1
      sleep 2
      fs_cli -x "uuid_audio_inject $UUID resume" > /dev/null 2>&1
      ;;
    1) # stop + restart
      echo "  $UUID: stop -> restart"
      fs_cli -x "uuid_audio_inject $UUID stop" > /dev/null 2>&1
      sleep 1
      fs_cli -x "uuid_audio_inject $UUID start $WS_URL mono 8k" > /dev/null 2>&1
      ;;
    2) # graceful-shutdown
      echo "  $UUID: graceful-shutdown"
      fs_cli -x "uuid_audio_inject $UUID graceful-shutdown" > /dev/null 2>&1
      ;;
    3) # 直接挂断
      echo "  $UUID: kill"
      fs_cli -x "uuid_kill $UUID" > /dev/null 2>&1
      ;;
  esac
done

sleep 5
for UUID in "${UUIDS[@]}"; do
  fs_cli -x "uuid_audio_inject $UUID stop" > /dev/null 2>&1
  fs_cli -x "uuid_kill $UUID" > /dev/null 2>&1
done

echo "Cross operation test done."
