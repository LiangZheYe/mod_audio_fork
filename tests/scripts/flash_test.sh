#!/bin/bash
# flash_test.sh - 快速启停循环测试
# 用法: ./flash_test.sh <UUID> [WS_URL] [ITERATIONS]

UUID=$1
WS_URL=${2:-"ws://127.0.0.1:8080"}
ITERATIONS=${3:-200}
FS_PID=$(pgrep freeswitch)

if [ -z "$UUID" ]; then
  echo "Usage: $0 <UUID> [WS_URL] [ITERATIONS]"
  exit 1
fi

echo "Flash test: $ITERATIONS iterations on UUID=$UUID WS=$WS_URL"

for i in $(seq 1 $ITERATIONS); do
  fs_cli -x "uuid_audio_inject $UUID start $WS_URL mono 8k" > /dev/null 2>&1
  fs_cli -x "uuid_audio_inject $UUID stop" > /dev/null 2>&1
  if (( i % 20 == 0 )); then
    FS_MEM=$(ps -o rss= -p $FS_PID | awk '{printf "%.1f MB", $1/1024}')
    echo "Iteration $i | FS Memory: $FS_MEM"
  fi
done

WS_CONNS=$(ss -tnp | grep 8080 | wc -l)
echo "Done. Remaining WS connections: $WS_CONNS (should be 0)"
