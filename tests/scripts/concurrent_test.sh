#!/bin/bash
# concurrent_test.sh - 多路并发连接测试
# 用法: ./concurrent_test.sh <并发数> <WS_URL>

CONCURRENT=${1:-10}
WS_URL=${2:-"ws://127.0.0.1:8080"}

echo "Starting $CONCURRENT concurrent sessions to $WS_URL..."

UUIDS=()
FAIL=0

for i in $(seq 1 $CONCURRENT); do
  RESULT=$(fs_cli -x "originate user/1001 &park" 2>/dev/null)
  UUID=$(echo "$RESULT" | grep -oP '[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}' | head -1)

  if [ -n "$UUID" ]; then
    fs_cli -x "uuid_audio_inject $UUID start $WS_URL mono 8k" > /dev/null 2>&1
    UUIDS+=("$UUID")
    echo "[$i/$CONCURRENT] Started session $UUID"
  else
    FAIL=$((FAIL + 1))
    echo "[$i/$CONCURRENT] FAILED to create session"
  fi
done

echo ""
echo "Active sessions: ${#UUIDS[@]}, Failed: $FAIL"
echo "Waiting 60 seconds..."
sleep 60

# 批量停止
echo "Stopping all sessions..."
for UUID in "${UUIDS[@]}"; do
  fs_cli -x "uuid_audio_inject $UUID stop" > /dev/null 2>&1
  fs_cli -x "uuid_kill $UUID" > /dev/null 2>&1
done

echo "All sessions stopped."
