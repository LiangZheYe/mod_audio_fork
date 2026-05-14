# mod_audio_inject 稳定性与并发测试方案

## 前提条件

- FreeSWITCH 已安装并运行，mod_audio_inject 已加载
- 一个可用的 WebSocket 服务端（以下简称 WS Server），用于接收上行音频并可选地发送下行音频
- fs_cli 已连接到 FreeSWITCH ESL 接口
- 建议准备至少 2 个 SIP 终端（软电话或网关）用于建立通话

### 环境检查

```bash
# 确认模块已加载
fs_cli -x "module_exists mod_audio_inject"

# 确认模块加载状态
fs_cli -x "module_list" | grep audio_inject

# 确认 FreeSWITCH 运行状态
fs_cli -x "status"
```

### 测试用 WebSocket 服务端

需要一个简单的 WebSocket 服务端来配合测试。推荐方案：

**方案 A：使用 Node.js 快速搭建**

```javascript
// ws_test_server.js
const WebSocket = require('ws');
const server = new WebSocket.Server({ port: 8080 });

server.on('connection', (ws, req) => {
  console.log(`[CONNECT] ${new Date().toISOString()} from ${req.socket.remoteAddress}`);

  ws.on('message', (data) => {
    // 上行音频接收 - 仅统计，不回放
  });

  ws.on('close', () => {
    console.log(`[DISCONNECT] ${new Date().toISOString()}`);
  });

  ws.on('error', (err) => {
    console.error(`[ERROR] ${err.message}`);
  });

  // 可选：发送下行音频（PCM 16bit 8kHz）
  // const silence = Buffer.alloc(320, 0); // 20ms silence
  // setInterval(() => ws.send(silence), 20);
});

console.log('WebSocket test server listening on port 8080');
```

```bash
npm install ws
node ws_test_server.js
```

**方案 B：使用 Python**

```python
# ws_test_server.py
import asyncio
import websockets
from datetime import datetime

async def handle(ws, path):
    print(f"[CONNECT] {datetime.now().isoformat()}")
    try:
        async for message in ws:
            pass  # 接收上行音频
    except websockets.exceptions.ConnectionClosed:
        pass
    finally:
        print(f"[DISCONNECT] {datetime.now().isoformat()}")

asyncio.run(websockets.serve(handle, "0.0.0.0", 8080))
```

**带下行音频注入的测试服务端**（用于测试双向音频）：

```javascript
// ws_bidir_server.js
const WebSocket = require('ws');
const fs = require('fs');

const server = new WebSocket.Server({ port: 8080 });

server.on('connection', (ws) => {
  console.log(`[CONNECT] ${new Date().toISOString()}`);

  // 发送 WAV 文件中的 PCM 数据作为下行音频
  // 假设 8kHz 16bit mono WAV
  const audioData = fs.readFileSync('test_audio.pcm');
  let offset = 0;
  const frameSize = 320; // 20ms @ 8kHz 16bit = 320 bytes

  const interval = setInterval(() => {
    if (ws.readyState === WebSocket.OPEN) {
      const chunk = audioData.slice(offset, offset + frameSize);
      if (chunk.length < frameSize) {
        offset = 0; // 循环播放
      } else {
        ws.send(chunk);
      }
      offset += frameSize;
    }
  }, 20);

  ws.on('close', () => {
    clearInterval(interval);
    console.log(`[DISCONNECT] ${new Date().toISOString()}`);
  });
});
```

---

## 一、稳定性测试

### 1.1 基本连接与断开

**目标**：验证 start/stop 命令的正确性，连接事件正常触发。

```bash
# 建立一通电话，获取 UUID
fs_cli -x "originate user/1001 &park"
# 记下返回的 UUID，假设为 <UUID>

# 开始上行音频流（mono, 8kHz）
fs_cli -x "uuid_audio_inject <UUID> start ws://127.0.0.1:8080 mono 8k"

# 验证：监听事件
fs_cli -x "event json CUSTOM"

# 观察 mod_audio_inject::connect 事件是否触发
# 预期：收到 connect 事件

# 停止
fs_cli -x "uuid_audio_inject <UUID> stop"

# 观察 mod_audio_inject::disconnect 事件是否触发
```

**检查项**：
- [ ] `start` 返回 `+OK`，无错误日志
- [ ] `mod_audio_inject::connect` 事件触发
- [ ] `stop` 返回 `+OK`
- [ ] `mod_audio_inject::disconnect` 事件触发
- [ ] WS Server 端看到连接建立和断开
- [ ] FreeSWITCH 日志无 ERROR/CRASH

### 1.2 长时间运行（Soak Test）

**目标**：验证模块在长时间运行下无内存泄漏、无崩溃。

```bash
# 建立通话
fs_cli -x "originate user/1001 &park"

# 启动流（建议 16kHz stereo）
fs_cli -x "uuid_audio_inject <UUID> start ws://127.0.0.1:8080 stereo 16k"

# 持续运行 12-24 小时
```

**监控脚本**：

```bash
#!/bin/bash
# monitor.sh - 每 60 秒采集一次内存和连接状态

UUID=$1
LOG_FILE="soak_test_$(date +%Y%m%d_%H%M%S).log"

while true; do
  TIMESTAMP=$(date '+%Y-%m-%d %H:%M:%S')

  # FreeSWITCH 进程内存
  FS_MEM=$(ps -o rss= -p $(pgrep freeswitch) | awk '{printf "%.1f MB", $1/1024}')

  # 模块级 session 计数（通过日志间接判断）
  ACTIVE=$(fs_cli -x "uuid_dump $UUID" 2>/dev/null | grep -c "audio_inject" || echo "0")

  # WebSocket 连接数
  WS_CONNS=$(ss -tnp | grep 8080 | wc -l)

  echo "$TIMESTAMP | FS_MEM: $FS_MEM | WS_CONNS: $WS_CONNS | Active: $ACTIVE" >> "$LOG_FILE"

  sleep 60
done
```

**检查项**：
- [ ] FreeSWITCH 内存 RSS 无持续增长趋势（允许 ±5% 波动）
- [ ] 无 `buffer_overrun` 事件
- [ ] 无 `mod_audio_inject::error` 事件
- [ ] WS Server 端连接未断开
- [ ] 通话音频质量正常，无杂音/卡顿

**内存泄漏判断标准**：连续 12 小时，RSS 增长 < 10% 且无单调递增趋势。

### 1.3 Pause/Resume 循环

**目标**：验证暂停/恢复功能的稳定性，反复操作无资源泄漏。

```bash
UUID=<UUID>
fs_cli -x "uuid_audio_inject $UUID start ws://127.0.0.1:8080 mono 8k"
sleep 2

# 循环 100 次 pause/resume
for i in $(seq 1 100); do
  fs_cli -x "uuid_audio_inject $UUID pause"
  sleep 1
  fs_cli -x "uuid_audio_inject $UUID resume"
  sleep 1
  echo "Cycle $i done"
done

fs_cli -x "uuid_audio_inject $UUID stop"
```

**检查项**：
- [ ] 每次 pause/resume 都返回 `+OK`
- [ ] 无 ERROR 日志
- [ ] 最终 stop 正常
- [ ] 内存无累积增长

### 1.4 Stop Play 清理

**目标**：验证 `stop_play` 正确清除 playout buffer。

```bash
UUID=<UUID>
# 使用支持下行音频的 WS Server
fs_cli -x "uuid_audio_inject $UUID start ws://127.0.0.1:8080 mono 8k audio_inject true true 8000"
sleep 5  # 等待下行音频注入

# 清除播放
fs_cli -x "uuid_audio_inject $UUID stop_play"

# 验证：通话应恢复静音/原始音频
sleep 2

# 重复操作
for i in $(seq 1 50); do
  sleep 3
  fs_cli -x "uuid_audio_inject $UUID stop_play"
  echo "stop_play cycle $i"
done

fs_cli -x "uuid_audio_inject $UUID stop"
```

**检查项**：
- [ ] `stop_play` 后被叫方听不到注入音频
- [ ] 多次 stop_play 无崩溃
- [ ] 通话保持正常

### 1.5 Graceful Shutdown

**目标**：验证优雅关闭能正确排空缓冲区。

```bash
UUID=<UUID>
fs_cli -x "uuid_audio_inject $UUID start ws://127.0.0.1:8080 mono 8k"
sleep 5

# 优雅关闭
fs_cli -x "uuid_audio_inject $UUID graceful-shutdown"

# 观察：应在缓冲区排空后触发 disconnect 事件
```

**检查项**：
- [ ] `graceful-shutdown` 返回 `+OK`
- [ ] 最终收到 `disconnect` 事件
- [ ] WS Server 端正常收到连接关闭
- [ ] 无音频截断或异常

### 1.6 连接失败处理

**目标**：验证连接不可达时的错误处理和资源回收。

```bash
UUID=<UUID>

# 测试 1：连接不可达的地址
fs_cli -x "uuid_audio_inject $UUID start ws://192.0.2.1:9999 mono 8k"
# 预期：收到 connect_failed 事件

# 测试 2：连接被拒绝（端口无服务）
fs_cli -x "uuid_audio_inject $UUID start ws://127.0.0.1:19999 mono 8k"
# 预期：收到 connect_failed 事件

# 测试 3：错误 URL 格式
fs_cli -x "uuid_audio_inject $UUID start not-a-url mono 8k"
# 预期：返回错误信息

# 关键：每次失败后验证资源已释放
fs_cli -x "uuid_dump $UUID" | grep audio_inject
# 预期：无残留的 bug 引用
```

**检查项**：
- [ ] 连接失败触发 `connect_failed` 事件
- [ ] 失败后 media bug 已正确移除（BUG-08 修复验证）
- [ ] 无内存泄漏
- [ ] 通话本身不受影响（不挂断）
- [ ] 可以在失败后重新 `start` 到正确地址

### 1.7 通话状态变化

**目标**：验证通话挂断时模块正确清理资源。

```bash
# 测试 1：通话中挂断 - 主叫挂
fs_cli -x "originate user/1001 &park"
UUID=<UUID>
fs_cli -x "uuid_audio_inject $UUID start ws://127.0.0.1:8080 mono 8k"
sleep 3
fs_cli -x "uuid_kill $UUID"
# 观察日志：应看到正常的 disconnect 和清理

# 测试 2：通话中挂断 - 被叫挂
# （通过 SIP 电话主动挂断）
# 观察日志：同上

# 测试 3：通话转移
fs_cli -x "originate user/1001 &park"
UUID=<UUID>
fs_cli -x "uuid_audio_inject $UUID start ws://127.0.0.1:8080 mono 8k"
sleep 3
fs_cli -x "uuid_transfer $UUID user/1002"
# 观察：原连接应断开，新 leg 不应有残留 audio_inject
```

**检查项**：
- [ ] 挂断后 `disconnect` 事件正常触发
- [ ] 无僵死 WebSocket 连接
- [ ] 无内存泄漏
- [ ] 转移后新 leg 无残留状态

---

## 二、并发测试

### 2.1 多路并发连接

**目标**：验证多路通话同时使用 mod_audio_inject 的稳定性。

**批量发起脚本**：

```bash
#!/bin/bash
# concurrent_test.sh
# 用法: ./concurrent_test.sh <并发数> <WS_URL>

CONCURRENT=${1:-10}
WS_URL=${2:-"ws://127.0.0.1:8080"}
LOG_PREFIX="concurrent_test_$(date +%Y%m%d_%H%M%S)"

echo "Starting $CONCURRENT concurrent sessions..."

UUIDS=()

# 批量建立通话并启动流
for i in $(seq 1 $CONCURRENT); do
  RESULT=$(fs_cli -x "originate user/1001 &park" 2>/dev/null)
  UUID=$(echo "$RESULT" | grep -oP '[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}' | head -1)

  if [ -n "$UUID" ]; then
    fs_cli -x "uuid_audio_inject $UUID start $WS_URL mono 8k" > /dev/null 2>&1
    UUIDS+=("$UUID")
    echo "[$i/$CONCURRENT] Started session $UUID"
  else
    echo "[$i/$CONCURRENT] FAILED to create session"
  fi
done

echo "Active sessions: ${#UUIDS[@]}"
echo "Waiting 60 seconds..."
sleep 60

# 批量停止
echo "Stopping all sessions..."
for UUID in "${UUIDS[@]}"; do
  fs_cli -x "uuid_audio_inject $UUID stop" > /dev/null 2>&1
  fs_cli -x "uuid_kill $UUID" > /dev/null 2>&1
done

echo "All sessions stopped."
```

**检查项**：
- [ ] 所有连接的 `connect` 事件都触发
- [ ] WS Server 端看到对应数量的连接
- [ ] FreeSWITCH 内存增长与并发数成线性关系
- [ ] 无崩溃、无断言失败
- [ ] 所有 stop 操作正常

### 2.2 并发阶梯测试

**目标**：逐步增加并发数，找到稳定上限。

```bash
#!/bin/bash
# staircase_test.sh

WS_URL="ws://127.0.0.1:8080"
LEVELS=(10 25 50 100 200)
HOLD_SECS=120

for LEVEL in "${LEVELS[@]}"; do
  echo "=== Level: $LEVEL concurrent sessions ==="
  UUIDS=()

  # 启动
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

  # 采集资源
  FS_MEM=$(ps -o rss= -p $(pgrep freeswitch) | awk '{printf "%.1f MB", $1/1024}')
  WS_CONNS=$(ss -tnp | grep 8080 | wc -l)
  echo "FS Memory: $FS_MEM | WS Connections: $WS_CONNS"

  # 保持运行
  sleep $HOLD_SECS

  # 检查存活
  ALIVE=0
  for UUID in "${UUIDS[@]}"; do
    EXISTS=$(fs_cli -x "uuid_exists $UUID" 2>/dev/null | tr -d '[:space:]')
    if [ "$EXISTS" = "true" ]; then
      ALIVE=$((ALIVE + 1))
    fi
  done
  echo "Alive after ${HOLD_SECS}s: $ALIVE / $SUCCESS"

  # 清理
  for UUID in "${UUIDS[@]}"; do
    fs_cli -x "uuid_audio_inject $UUID stop" > /dev/null 2>&1
    fs_cli -x "uuid_kill $UUID" > /dev/null 2>&1
  done

  # 等待资源释放
  sleep 30

  # 检查内存回落
  FS_MEM_AFTER=$(ps -o rss= -p $(pgrep freeswitch) | awk '{printf "%.1f MB", $1/1024}')
  echo "FS Memory after cleanup: $FS_MEM_AFTER"
  echo ""
done
```

**检查项**：
- [ ] 记录每级并发下的成功率和存活率
- [ ] 内存增长与并发数成正比，清理后回落
- [ ] 无崩溃（找到崩溃阈值即为上限）
- [ ] 记录每级并发下 FreeSWITCH CPU 使用率

### 2.3 快速启停（Flash Connect/Disconnect）

**目标**：验证快速建立和断开连接不导致资源泄漏或崩溃。

```bash
#!/bin/bash
# flash_test.sh - 快速启停循环

WS_URL="ws://127.0.0.1:8080"
ITERATIONS=200

fs_cli -x "originate user/1001 &park"
UUID=<UUID>

for i in $(seq 1 $ITERATIONS); do
  fs_cli -x "uuid_audio_inject $UUID start $WS_URL mono 8k" > /dev/null 2>&1
  # 不等连接建立就立即 stop
  fs_cli -x "uuid_audio_inject $UUID stop" > /dev/null 2>&1
  if (( i % 20 == 0 )); then
    FS_MEM=$(ps -o rss= -p $(pgrep freeswitch) | awk '{printf "%.1f MB", $1/1024}')
    echo "Iteration $i | FS Memory: $FS_MEM"
  fi
done

fs_cli -x "uuid_kill $UUID"
```

**变体：带间隔的快速启停**

```bash
# 等待连接建立后再 stop
for i in $(seq 1 100); do
  fs_cli -x "uuid_audio_inject $UUID start $WS_URL mono 8k"
  sleep 1
  fs_cli -x "uuid_audio_inject $UUID stop"
  sleep 1
done
```

**检查项**：
- [ ] 无崩溃
- [ ] 无僵死 WebSocket 连接（`ss -tnp | grep 8080` 应为 0）
- [ ] 内存无累积增长
- [ ] 日志无断言失败

### 2.4 同一通话多 Bug 实例

**目标**：验证同一通话上挂载多个 audio_inject bug 的行为。

```bash
UUID=<UUID>

# 启动两个不同 bugname 的实例
fs_cli -x "uuid_audio_inject $UUID start ws://127.0.0.1:8080 mono 8k bug1"
fs_cli -x "uuid_audio_inject $UUID start ws://127.0.0.1:8081 mono 8k bug2"

# 验证两个都连接
sleep 3

# 分别停止
fs_cli -x "uuid_audio_inject $UUID stop bug1"
fs_cli -x "uuid_audio_inject $UUID stop bug2"

# 测试：同一 bugname 重复 start（应报错）
fs_cli -x "uuid_audio_inject $UUID start ws://127.0.0.1:8080 mono 8k bug1"
fs_cli -x "uuid_audio_inject $UUID start ws://127.0.0.1:8080 mono 8k bug1"
# 预期：第二次返回错误 "bug already attached"
```

**检查项**：
- [ ] 多 bug 实例可独立启停
- [ ] 重复 bugname start 被正确拒绝
- [ ] 两个实例的音频流互不干扰
- [ ] 分别 stop 后资源都正确释放

### 2.5 并发 + 通话操作交叉

**目标**：验证并发流下执行通话操作（转移、保持、挂断）的稳定性。

```bash
#!/bin/bash
# cross_operation_test.sh

WS_URL="ws://127.0.0.1:8080"
CONCURRENT=20

# 启动并发通话
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

# 交叉操作
for UUID in "${UUIDS[@]}"; do
  OP=$(( RANDOM % 4 ))
  case $OP in
    0) # pause/resume
      fs_cli -x "uuid_audio_inject $UUID pause" > /dev/null 2>&1
      sleep 2
      fs_cli -x "uuid_audio_inject $UUID resume" > /dev/null 2>&1
      ;;
    1) # stop + restart
      fs_cli -x "uuid_audio_inject $UUID stop" > /dev/null 2>&1
      sleep 1
      fs_cli -x "uuid_audio_inject $UUID start $WS_URL mono 8k" > /dev/null 2>&1
      ;;
    2) # graceful-shutdown
      fs_cli -x "uuid_audio_inject $UUID graceful-shutdown" > /dev/null 2>&1
      ;;
    3) # 直接挂断
      fs_cli -x "uuid_kill $UUID" > /dev/null 2>&1
      ;;
  esac
done

# 清理残留
sleep 5
for UUID in "${UUIDS[@]}"; do
  fs_cli -x "uuid_audio_inject $UUID stop" > /dev/null 2>&1
  fs_cli -x "uuid_kill $UUID" > /dev/null 2>&1
done

echo "Cross operation test done."
```

---

## 三、异常场景测试

### 3.1 WebSocket 服务端断开

```bash
UUID=<UUID>
fs_cli -x "uuid_audio_inject $UUID start ws://127.0.0.1:8080 mono 8k"
sleep 3

# 在 WS Server 端强制关闭连接（如 kill 服务端进程）
# 观察 FreeSWITCH 日志

# 预期：收到 disconnect 事件，通话不挂断
# 验证通话仍可正常操作
fs_cli -x "uuid_audio_inject $UUID stop"
```

### 3.2 WebSocket 服务端重启

```bash
UUID=<UUID>
fs_cli -x "uuid_audio_inject $UUID start ws://127.0.0.1:8080 mono 8k"
sleep 3

# 重启 WS Server
# （先 kill，等几秒，再启动）

# 在 WS Server 恢复后，尝试重新 start
fs_cli -x "uuid_audio_inject $UUID stop"
fs_cli -x "uuid_audio_inject $UUID start ws://127.0.0.1:8080 mono 8k"
# 预期：能重新连接
```

### 3.3 网络延迟模拟

```bash
# 使用 tc 模拟网络延迟和丢包
sudo tc qdisc add dev eth0 root netem delay 200ms loss 5%

# 运行正常测试流程
UUID=<UUID>
fs_cli -x "uuid_audio_inject $UUID start ws://192.168.1.100:8080 mono 8k"
sleep 30
fs_cli -x "uuid_audio_inject $UUID stop"

# 清除 tc 规则
sudo tc qdisc del dev eth0 root
```

**检查项**：
- [ ] 延迟下连接能建立（可能慢）
- [ ] 丢包下 `buffer_overrun` 事件频率
- [ ] 音频质量在恶劣网络下的表现

### 3.4 无效参数测试

```bash
UUID=<UUID>

# 无效采样率
fs_cli -x "uuid_audio_inject $UUID start ws://127.0.0.1:8080 mono 7k"
# 预期：错误 "invalid sample rate"

# 无效 mix type
fs_cli -x "uuid_audio_inject $UUID start ws://127.0.0.1:8080 quad 8k"
# 预期：错误 "invalid mix type"

# 无效 URL
fs_cli -x "uuid_audio_inject $UUID start ftp://bad mono 8k"
# 预期：错误 "invalid websocket uri"

# 不存在的 UUID
fs_cli -x "uuid_audio_inject 00000000-0000-0000-0000-000000000000 start ws://127.0.0.1:8080 mono 8k"
# 预期：错误 "Error locating session"

# 通话未建立就 start
fs_cli -x "uuid_audio_inject <PRE_ANSWER_UUID> start ws://127.0.0.1:8080 mono 8k"
# 预期：错误 "channel must have reached pre-answer status"
```

### 3.5 Buffer Overrun 场景

```bash
# 使用小 buffer + 慢速 WS Server 模拟 buffer overrun
export MOD_AUDIO_INJECT_BUFFER_SECS=1

UUID=<UUID>
fs_cli -x "uuid_audio_inject $UUID start ws://127.0.0.1:8080 stereo 16k"

# 让 WS Server 慢速读取（或处理慢）
# 观察 buffer_overrun 事件
```

---

## 四、双向音频专项测试

### 4.1 双向音频基本功能

```bash
UUID=<UUID>

# 启动双向流：bidir_enabled=true, bidir_stream_enabled=true, bidir_samplerate=8000
fs_cli -x "uuid_audio_inject $UUID start ws://127.0.0.1:8080 mono 8k audio_inject true true 8000"

# WS Server 发送 PCM 音频数据
# 验证被叫方能听到注入的音频

# 停止播放
fs_cli -x "uuid_audio_inject $UUID stop_play"
# 验证音频停止

# 恢复（需要 WS Server 重新发送）
# 观察 playout buffer 行为
```

### 4.2 双向音频采样率匹配

```bash
# 测试不同下行采样率
for RATE in 8000 16000 24000; do
  echo "Testing bidir sample rate: $RATE"
  fs_cli -x "uuid_audio_inject $UUID start ws://127.0.0.1:8080 mono 8k audio_inject true true $RATE"
  sleep 10
  fs_cli -x "uuid_audio_inject $UUID stop"
  sleep 3
done
```

### 4.3 双向音频 Speex 重采样

```bash
# 通话编解码为 8kHz，但下行音频为 16kHz — 验证 Speex 重采样
fs_cli -x "uuid_audio_inject $UUID start ws://127.0.0.1:8080 mono 8k audio_inject true true 16000"
# WS Server 发送 16kHz PCM
# 预期：音频正常播放，无异常噪音
```

---

## 五、监控与日志

### 5.1 事件监控

```bash
# 在单独终端持续监听 mod_audio_inject 事件
fs_cli -x "event json CUSTOM" &
# 过滤相关事件
fs_cli | grep --line-buffered "mod_audio_inject"
```

### 5.2 资源监控脚本

```bash
#!/bin/bash
# resource_monitor.sh - 持续监控 FreeSWITCH 资源使用

PID=$(pgrep freeswitch)
INTERVAL=10

echo "timestamp,rss_mb,cpu_pct,ws_conns,fds,threads" > resource_log.csv

while true; do
  TIMESTAMP=$(date '+%Y-%m-%d %H:%M:%S')
  RSS=$(ps -o rss= -p $PID | awk '{printf "%.1f", $1/1024}')
  CPU=$(ps -o %cpu= -p $PID | tr -d ' ')
  WS_CONNS=$(ss -tnp | grep 8080 | wc -l)
  FDS=$(ls /proc/$PID/fd 2>/dev/null | wc -l)
  THREADS=$(ls /proc/$PID/task 2>/dev/null | wc -l)

  echo "$TIMESTAMP,$RSS,$CPU,$WS_CONNS,$FDS,$THREADS" >> resource_log.csv
  sleep $INTERVAL
done
```

### 5.3 关键日志过滤

```bash
# 监控错误和警告
tail -f /usr/local/freeswitch/log/freeswitch.log | grep --line-buffered -E "mod_audio_inject.*(ERROR|WARN|CRASH|buffer_overrun)"

# 监控所有 mod_audio_inject 日志
tail -f /usr/local/freeswitch/log/freeswitch.log | grep --line-buffered "mod_audio_inject"
```

### 5.4 连接泄漏检测

```bash
#!/bin/bash
# leak_detector.sh - 检测 WebSocket 连接泄漏

EXPECTED=0  # 预期的活跃连接数

while true; do
  ACTUAL=$(ss -tnp | grep 8080 | wc -l)
  if [ "$ACTUAL" -gt "$EXPECTED" ]; then
    echo "$(date): LEAK DETECTED - $ACTUAL connections (expected $EXPECTED)"
    ss -tnp | grep 8080
  fi
  sleep 30
done
```

---

## 六、测试结果记录模板

| 测试项 | 通过/失败 | 详情 | 发现的问题 |
|--------|-----------|------|------------|
| 1.1 基本连接与断开 | | | |
| 1.2 长时间运行 (12h) | | | |
| 1.3 Pause/Resume 循环 | | | |
| 1.4 Stop Play 清理 | | | |
| 1.5 Graceful Shutdown | | | |
| 1.6 连接失败处理 | | | |
| 1.7 通话状态变化 | | | |
| 2.1 多路并发 (10路) | | | |
| 2.2 阶梯测试 (峰值) | | | |
| 2.3 快速启停 | | | |
| 2.4 多 Bug 实例 | | | |
| 2.5 交叉操作 | | | |
| 3.1 WS 服务端断开 | | | |
| 3.2 WS 服务端重启 | | | |
| 3.3 网络延迟模拟 | | | |
| 3.4 无效参数 | | | |
| 3.5 Buffer Overrun | | | |
| 4.1 双向音频基本 | | | |
| 4.2 采样率匹配 | | | |
| 4.3 Speex 重采样 | | | |

---

## 七、常见问题排查

### 连接建立失败
```bash
# 检查 FreeSWITCH 日志
fs_cli -x "log level 7"
# 重新执行 start，查看详细错误

# 检查网络连通性
curl -v http://127.0.0.1:8080

# 检查环境变量
fs_cli -x "global_getvar MOD_AUDIO_INJECT_SUBPROTOCOL_NAME"
```

### 内存持续增长
```bash
# 启用 FreeSWITCH 内存调试
# 在 freeswitch.xml 中设置:
# <param name="memory-debug" value="true"/>

# 定期采集内存映射
gdb -p $(pgrep freeswitch) -batch -ex "call switch_core_memory_debug()"
```

### 僵死连接
```bash
# 检查 WebSocket 连接
ss -tnp | grep <WS_PORT>

# 检查 FreeSWITCH sessions
fs_cli -x "show sessions"

# 强制清理
fs_cli -x "hupall normal_clearing"
```
