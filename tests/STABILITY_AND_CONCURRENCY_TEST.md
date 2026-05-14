# mod_audio_inject 稳定性与并发测试方案

## 目录结构

```
tests/
├── STABILITY_AND_CONCURRENCY_TEST.md   # 本文档
├── ws_server/                          # WebSocket 测试服务端
│   ├── package.json
│   ├── ws_test_server.js               # Node.js 上行接收服务端
│   ├── ws_test_server.py               # Python 上行接收服务端
│   └── ws_bidir_server.js              # Node.js 双向音频服务端
└── scripts/                            # 测试脚本
    ├── soak_monitor.sh                 # 长时间运行监控
    ├── concurrent_test.sh              # 多路并发测试
    ├── staircase_test.sh               # 阶梯加压测试
    ├── flash_test.sh                   # 快速启停测试
    ├── cross_operation_test.sh         # 交叉操作测试
    ├── resource_monitor.sh             # 资源监控（CSV输出）
    └── leak_detector.sh               # 连接泄漏检测
```

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

### 启动测试用 WebSocket 服务端

测试前需先启动 WS Server，提供两种实现：

**Node.js 版（推荐）**：

```bash
cd tests/ws_server
npm install
# 仅接收上行音频
node ws_test_server.js
# 或双向音频（需准备 test_audio.pcm 文件）
node ws_bidir_server.js
```

**Python 版**：

```bash
pip install websockets
cd tests/ws_server
python ws_test_server.py
```

`ws_bidir_server.js` 支持环境变量配置：
- `WS_PORT` — 监听端口（默认 8080）
- `AUDIO_FILE` — 下行音频 PCM 文件路径（默认 `test_audio.pcm`）

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

**使用监控脚本**：

```bash
./scripts/soak_monitor.sh <UUID> [WS_PORT]
# 输出示例：
# 2026-05-14 10:00:00 | FS_MEM: 245.3 MB | WS_CONNS: 1 | Active: 1
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

```bash
./scripts/concurrent_test.sh <并发数> <WS_URL>
# 示例：
./scripts/concurrent_test.sh 10 ws://127.0.0.1:8080
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
./scripts/staircase_test.sh [WS_URL]
# 默认阶梯: 10, 25, 50, 100, 200 路
# 每级保持 120 秒
```

脚本会自动采集每级的成功数、内存、WS 连接数、存活率，以及清理后的内存回落情况。

**检查项**：
- [ ] 记录每级并发下的成功率和存活率
- [ ] 内存增长与并发数成正比，清理后回落
- [ ] 无崩溃（找到崩溃阈值即为上限）
- [ ] 记录每级并发下 FreeSWITCH CPU 使用率

### 2.3 快速启停（Flash Connect/Disconnect）

**目标**：验证快速建立和断开连接不导致资源泄漏或崩溃。

```bash
# 先建立通话
fs_cli -x "originate user/1001 &park"
UUID=<返回的UUID>

./scripts/flash_test.sh <UUID> [WS_URL] [ITERATIONS]
# 示例：
./scripts/flash_test.sh $UUID ws://127.0.0.1:8080 200
```

**变体：带间隔的快速启停**：

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
./scripts/cross_operation_test.sh [CONCURRENT] [WS_URL]
# 示例：
./scripts/cross_operation_test.sh 20 ws://127.0.0.1:8080
```

脚本会对每个 session 随机执行 pause/resume、stop+restart、graceful-shutdown 或 kill 操作。

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

### 5.2 资源监控

```bash
./scripts/resource_monitor.sh [WS_PORT] [INTERVAL]
# 示例：每 10 秒采集一次，输出到 resource_log.csv
./scripts/resource_monitor.sh 8080 10
```

输出 CSV 格式：`timestamp,rss_mb,cpu_pct,ws_conns,fds,threads`

### 5.3 关键日志过滤

```bash
# 监控错误和警告
tail -f /usr/local/freeswitch/log/freeswitch.log | grep --line-buffered -E "mod_audio_inject.*(ERROR|WARN|CRASH|buffer_overrun)"

# 监控所有 mod_audio_inject 日志
tail -f /usr/local/freeswitch/log/freeswitch.log | grep --line-buffered "mod_audio_inject"
```

### 5.4 连接泄漏检测

```bash
./scripts/leak_detector.sh [WS_PORT] [EXPECTED_MAX]
# 示例：检测 8080 端口，超过 0 个连接即报警
./scripts/leak_detector.sh 8080 0
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
