# mod_audio_inject 详细设计文档

## 1. 模块概述

mod_audio_inject 是一个 FreeSWITCH 可加载模块，利用 FreeSWITCH 的 Media Bug 机制将通话音频实时通过 WebSocket 流式传输到远端服务器，并支持从远端服务器接收音频/指令注入通话。典型应用场景包括实时语音转写、AI 语音助手、双向语音流处理等。

### 1.1 核心能力

| 能力 | 说明 |
|------|------|
| 音频 Inject (上行) | 将通话的 read 侧音频流式发送到远端 WebSocket 服务器 |
| 音频 Dub (下行) | 从远端 WebSocket 服务器接收音频数据，注入通话的 write 侧 |
| 采样率转换 | 基于 Speex Resampler，支持任意 8kHz 倍频采样率转换 |
| 双向音频流 | 支持 WebSocket 二进制帧承载实时下行音频（低延迟） |
| TLS/WSS | 基于 libwebsockets 原生支持 WSS 加密连接 |
| Basic Auth | 支持 HTTP Basic Authentication 握手认证 |

---

## 2. FreeSWITCH Media Bug 机制

### 2.1 Media Bug 原理

FreeSWITCH 的 Media Bug 是一种"旁路监听"机制，允许模块在不修改核心媒体路径的情况下截获和操控音频帧。其核心概念：

```
                    ┌──────────────────────┐
                    │   FreeSWITCH Core    │
                    │                      │
  RTP In ──→ Read ──→ [Media Bug] ──→ Write ──→ RTP Out
                    │   ↑       ↑        │
                    │   │       │        │
                    │  READ    WRITE     │
                    │  回调    回调       │
                    │   │       │        │
                    │   ↓       ↓        │
                    │ [mod_audio_inject]    │
                    └──────────────────────┘
```

- **switch_core_media_bug_add()** 将 bug 注册到 session 的 media 栈
- bug 的回调函数在 FreeSWITCH 的 media 线程中被调用（每 20ms 一次）
- 回调类型（switch_abc_type_t）决定了何时被触发
- bug 的生命周期绑定到 session，session 关闭时自动触发 CLOSE

### 2.2 本模块使用的 Bug Flags

| Flag | 含义 | 用途 |
|------|------|------|
| `SMBF_READ_STREAM` | 监听 read 侧（远端→本端）音频 | 上行 inject：将对方语音发送到服务器 |
| `SMBF_WRITE_STREAM` | 监听 write 侧（本端→远端）音频 | mixed/stereo 模式下同时采集双方语音 |
| `SMBF_STEREO` | read/write 侧音频分通道存储 | 立体声模式，左右声道分别对应 read/write |
| `SMBF_WRITE_REPLACE` | 替换 write 侧音频帧 | 下行 dub：将服务器返回的音频注入通话 |

### 2.3 回调事件与处理

```
capture_callback(bug, user_data, type)
    │
    ├── SWITCH_ABC_TYPE_INIT ──────────→ (空操作，bug 创建时触发)
    │
    ├── SWITCH_ABC_TYPE_CLOSE ─────────→ inject_session_cleanup(session, bugname, NULL, 1)
    │                                    │  channelIsClosing=1 表示由 FreeSWITCH 驱动关闭
    │
    ├── SWITCH_ABC_TYPE_READ ──────────→ inject_frame(session, bug)
    │                                    │  每 20ms 触发一次
    │                                    │  读取音频帧 → 写入 AudioPipe 发送缓冲区
    │
    ├── SWITCH_ABC_TYPE_WRITE_REPLACE ─→ dub_speech_frame(bug, tech_pvt)
    │                                    │  每 20ms 触发一次
    │                                    │  从 playout buffer 读取 → 替换 write 帧
    │
    └── SWITCH_ABC_TYPE_WRITE ─────────→ (未使用)
```

**关键约束**：回调函数运行在 FreeSWITCH 的 media 线程中，该线程同时负责 RTP 收发和编解码。回调必须：
- 尽可能快速返回（否则会导致 jitter 和音频卡顿）
- 不能执行阻塞 I/O（网络、磁盘等）
- 不能长时间持锁

因此本模块采用"零拷贝 + 锁保护"策略：`inject_frame` 使用 `trylock`（非阻塞），`dub_speech_frame` 使用阻塞锁但操作极简（仅 buffer 读写）。

---

## 3. 架构设计

### 3.1 分层架构

```
┌─────────────────────────────────────────────────────────────────┐
│                     FreeSWITCH Core                             │
│  (session 线程, media 线程, API 线程)                           │
└──────────┬──────────────────────────────┬──────────────────────┘
           │ capture_callback              │ inject_function (API)
           │ (media 线程回调)               │ (API 线程)
           │                              │
┌──────────▼──────────────────────────────▼──────────────────────┐
│                      mod_audio_inject.c                           │
│  ┌──────────────────┐  ┌───────────────────────────────────┐   │
│  │ capture_callback │  │ inject_function (API 入口)          │   │
│  │   inject_frame()   │  │   start_capture() / do_stop()    │   │
│  │   dub_speech_    │  │   do_pauseresume()              │   │
│  │   frame()        │  │   do_graceful_shutdown()         │   │
│  └────────┬─────────┘  └───────────────┬───────────────────┘   │
└───────────┼────────────────────────────┼───────────────────────┘
            │                            │
┌───────────▼────────────────────────────▼───────────────────────┐
│                       lws_glue.cpp (C++ 桥接层)                │
│  ┌──────────────┐  ┌──────────────┐  ┌────────────────────┐   │
│  │ inject_frame   │  │ dub_speech_  │  │ processIncoming    │   │
│  │ (上行:音频    │  │ frame       │  │ Binary             │   │
│  │  写入buffer) │  │ (下行:从     │  │ (下行:二进制消息    │   │
│  │              │  │  buffer读取) │  │  处理)             │   │
│  └──────┬───────┘  └──────┬───────┘  └────────┬───────────┘   │
│         │                 │                    │               │
│  ┌──────▼─────────────────▼────────────────────▼───────────┐   │
│  │                   private_t (会话状态)                    │   │
│  │  · pAudioPipe ─────→ AudioPipe 实例                      │   │
│  │  · resampler / bidirectional_audio_resampler             │   │
│  │  · streamingPlayoutBuffer / streamingPreBuffer           │   │
│  │  · mutex (SWITCH_MUTEX_NESTED)                           │   │
│  └─────────────────────────────────────────────────────────┘   │
└───────────────────────────────────────────────────────────────┘
            │
┌───────────▼───────────────────────────────────────────────────┐
│                    AudioPipe (WebSocket 传输层)                 │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │  单例 LWS Service Thread (全模块共享)                    │   │
│  │  lws_service() 事件循环                                  │   │
│  │                                                         │   │
│  │  per-session AudioPipe 实例:                             │   │
│  │  · m_audio_buffer (上行音频缓冲区, LWS_PRE + 数据)      │   │
│  │  · m_state (atomic, 连接状态机)                          │   │
│  └─────────────────────────────────────────────────────────┘   │
└───────────────────────────────────────────────────────────────┘
            │
            ▼
    ┌───────────────┐
    │  WebSocket    │
    │  Server       │
    │  (远端)       │
    └───────────────┘
```

### 3.2 源文件职责

| 文件 | 语言 | 职责 |
|------|------|------|
| `mod_audio_inject.c` | C | FreeSWITCH 模块入口，API 命令解析，media bug 回调分发 |
| `mod_audio_inject.h` | C | 数据结构定义（private_t），常量宏，事件类型 |
| `lws_glue.cpp` | C++ | C/C++ 桥接层，上行音频采集、下行音频注入、二进制消息处理、会话生命周期 |
| `audio_pipe.cpp` | C++ | WebSocket 传输层，LWS 回调处理，连接管理，数据收发 |
| `audio_pipe.hpp` | C++ | AudioPipe 类定义，状态枚举，回调类型 |
| `vector_math.cpp` | C | SIMD 优化的向量运算（加法、归一化、音量调节） |

---

## 4. 线程模型

### 4.1 线程全景

```
┌─────────────────────────────────────────────────────────────────┐
│                        进程: freeswitch                         │
│                                                                 │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐            │
│  │ FS API 线程  │  │FS Media 线程│  │FS Media 线程 │  ...       │
│  │ (1个)        │  │ (session A) │  │ (session B)  │            │
│  │              │  │             │  │              │            │
│  │ inject_function│  │capture_     │  │capture_      │            │
│  │ (API命令)    │  │ callback    │  │ callback     │            │
│  └──────┬───────┘  │  ·READ      │  │  ·READ       │            │
│         │          │  ·WRITE_    │  │  ·WRITE_     │            │
│         │          │   REPLACE   │  │   REPLACE    │            │
│         │          └──────┬──────┘  └──────┬───────┘            │
│         │                 │                │                    │
│         │         ┌───────▼────────────────▼───────┐           │
│         │         │  LWS Service Thread (1个, 单例)  │           │
│         │         │                                  │           │
│         │         │  lws_service() 事件循环          │           │
│         │         │  · 处理 pending connects         │           │
│         │         │  · 处理 pending disconnects      │           │
│         │         │  · 处理 pending writes           │           │
│         │         │  · WebSocket 收发回调            │           │
│         │         │  · TLS 握手                     │           │
│         │         └──────────────────────────────────┘           │
│         │                                                        │
│         └───→ switch_core_session_locate / rwunlock              │
│              (跨线程安全访问 session)                             │
└─────────────────────────────────────────────────────────────────┘
```

### 4.2 各线程职责详解

#### 4.2.1 FreeSWITCH API 线程

- **触发方式**: ESL / CLI / 事件套接字调用 `uuid_audio_inject` 命令
- **执行函数**: `inject_function()` → `start_capture()` / `do_stop()` 等
- **关键操作**:
  - `switch_core_session_locate(argv[0])` 获取 session 引用（引用计数+1）
  - 调用 `inject_session_init()` 分配 private_t 和 AudioPipe
  - 调用 `switch_core_media_bug_add()` 注册 bug
  - 操作完毕后 `switch_core_session_rwunlock()` 释放引用
- **线程安全**: session 引用计数机制保证操作期间 session 不会被销毁

#### 4.2.2 FreeSWITCH Media 线程 (per-session)

每个活跃的通话 session 拥有独立的 media 线程，负责 RTP 收发、编解码和 media bug 回调。

- **触发频率**: 每 20ms 一次（RTP_PACKETIZATION_PERIOD）
- **上行路径 (SWITCH_ABC_TYPE_READ → inject_frame)**:
  1. `switch_mutex_trylock(tech_pvt->mutex)` — 非阻塞，获取失败则跳过本帧
  2. 检查 `pAudioPipe` 和连接状态
  3. `pAudioPipe->lockAudioBuffer()` — 获取音频缓冲区互斥锁
  4. `switch_core_media_bug_read(bug, &frame, SWITCH_TRUE)` — 直接读取到 AudioPipe 的发送缓冲区（零拷贝）
  5. 如需重采样，使用 Speex Resampler 转换后写入
  6. `pAudioPipe->unlockAudioBuffer()` — 释放锁并触发 pending write
  7. `switch_mutex_unlock(tech_pvt->mutex)`

- **下行路径 (SWITCH_ABC_TYPE_WRITE_REPLACE → dub_speech_frame)**:
  1. `switch_mutex_lock(tech_pvt->mutex)` — 阻塞锁（必须拿到，否则音频断裂）
  2. 从 `streamingPlayoutBuffer` 读取所需样本数
  3. 替换 rframe 的 data/datalen
  4. `switch_core_media_bug_set_write_replace_frame(bug, rframe)` — 注入替换帧
  5. 处理 mark 标记事件
  6. `switch_mutex_unlock(tech_pvt->mutex)`

#### 4.2.3 LWS Service Thread (全局单例)

- **启动时机**: 模块加载时 `inject_init()` → `AudioPipe::initialize()` → `std::thread`
- **运行函数**: `AudioPipe::lws_service_thread()`
- **核心循环**:
  ```cpp
  do {
    n = lws_service(context, 0);  // 阻塞等待事件，最长 0ms 超时
  } while (n >= 0 && !stopFlag);
  ```
- **事件驱动模型**:
  - `lws_cancel_service(context)` 唤醒事件循环（由其他线程调用）
  - 唤醒后触发 `LWS_CALLBACK_EVENT_WAIT_CANCELLED`
  - 在该回调中处理所有 pending 操作（connect/disconnect/write）
  - 然后处理 WebSocket 数据收发回调

- **关键约束**: libwebsockets 要求所有 LWS API 调用必须在 service 线程中执行。因此其他线程通过 pending 列表 + `lws_cancel_service()` 实现线程间通信。

### 4.3 线程间通信机制

```
  Media 线程                    LWS 线程
  ─────────                    ─────────
                               ┌──────────────────┐
  inject_frame():                │                  │
    lockAudioBuffer()          │  lws_service()   │
    写入 m_audio_buffer  ─────→│                  │
    unlockAudioBuffer()        │  WRITEABLE回调:  │
      └→ addPendingWrite()    │   lock m_audio_  │
      └→ lws_cancel_service() │   mutex          │
                               │   lws_write()    │
                               │   unlock         │
  dub_speech_frame():          │                  │
    lock tech_pvt->mutex       │  RECEIVE回调:    │
    读 playoutBuffer    ←──────│   eventCallback  │
    unlock                     │   → processIncoming
                               │   → 写入 buffer  │
                               └──────────────────┘
```

**通信原语**:

| 原语 | 保护对象 | 用途 |
|------|---------|------|
| `m_audio_mutex` | `m_audio_buffer` / `m_audio_buffer_write_offset` | 上行音频数据的生产者-消费者同步 |
| `mutex_connects` | `pendingConnects` 列表 | 连接请求队列保护 |
| `mutex_disconnects` | `pendingDisconnects` 列表 | 断连请求队列保护 |
| `mutex_writes` | `pendingWrites` 列表 | 写请求队列保护 |
| `tech_pvt->mutex` | private_t 共享状态 + playoutBuffer | media 回调与 LWS 回调之间的数据同步 |
| `m_state` (atomic) | 连接状态 | 跨线程状态读取（无需锁） |

---

## 5. 数据流详解

### 5.1 上行数据流 (Inject: 通话 → 服务器)

```
  RTP        Media Bug      inject_frame()        AudioPipe          LWS          WebSocket
  ──── ────→ switch_core_ ──→ media_bug_ ──→  m_audio_buffer ──→ lws_write ──→ Server
               media_bug_      read()            (零拷贝直写)       (WRITEABLE
               read()                                                 回调)
```

**详细步骤**:

1. **Media 线程**每 20ms 触发 `SWITCH_ABC_TYPE_READ`
2. `inject_frame()` 获取 `tech_pvt->mutex` (trylock) 和 `m_audio_mutex`
3. 将 AudioPipe 的 `m_audio_buffer + LWS_PRE + write_offset` 设为 `frame.data`
4. `switch_core_media_bug_read()` 直接写入 AudioPipe 缓冲区（**零拷贝**）
5. 如需重采样，先读入临时栈缓冲区，经 Speex Resampler 后写入
6. `unlockAudioBuffer()` → `addPendingWrite()` → `lws_cancel_service()`
7. **LWS 线程**被唤醒，处理 `LWS_CALLBACK_CLIENT_WRITEABLE`
8. 加锁 `m_audio_mutex`，调用 `lws_write()` 发送数据
9. 重置 `write_offset = LWS_PRE`

**缓冲区计算**:
```
buflen = LWS_PRE + (FRAME_SIZE_8000 × desiredSampling / 8000 × channels × 1000 / 20 × nAudioBufferSecs)
```

例如 16kHz 单声道, 2 秒缓冲: `LWS_PRE + 320 × 2 × 50 × 2 = LWS_PRE + 64,000 bytes`

**缓冲区溢出处理**: 当 `available < binaryMinSpace()` 时:
- 首次触发 `EVENT_BUFFER_OVERRUN` 事件通知应用层
- 重置 `write_offset = 0`（丢弃未发送数据，为新帧腾出空间）
- 继续写入最新音频帧（保证实时性，牺牲历史数据）

### 5.2 下行数据流 (Dub: 服务器 → 通话)

```
  Server ──→ WebSocket ──→ LWS_RECEIVE ──→ eventCallback ──→ processIncomingBinary
                                                                        │
                                                          奇偶字节对齐处理 (set_aside_byte)
                                                                        │
                                                          写入 streamingPreBuffer (预缓冲)
                                                                        │
                                                          ≥ 阈值时: Speex Resample
                                                                        │
                                                          switch_mutex_lock(tech_pvt->mutex)
                                                                        │
                                                          playoutBuffer.insert(resampled_audio)
                                                                        │
                                                          switch_mutex_unlock
```

**预缓冲策略** (`streamingPreBuffer`):
- 阈值: `320 × downscale_factor × 6` 样本（≥ 120ms @8kHz）
- 目的: 吸收网络抖动，避免 playout buffer 频繁 underrun
- 预缓冲满后才转入 playoutBuffer，首次播放有 120ms 延迟

**Playout 阶段** (Media 线程, 每 20ms):
```
  dub_speech_frame():
    lock(tech_pvt->mutex)
    samplesToCopy = min(playoutBuffer.size(), rframe->samples)
    if (samplesToCopy > 0):
      从 playoutBuffer 读取 → 替换 rframe → set_write_replace_frame
    else:
      memset(rframe, 0) → 静音填充（防止爆音/咔嗒声）
    unlock
```

---

## 6. AudioPipe 连接状态机

```
                    ┌───────────┐
                    │   IDLE    │ ← 初始状态 (构造函数)
                    └─────┬─────┘
                          │ addPendingConnect()
                          │ processPendingConnects()
                          ▼
                    ┌───────────┐
                    │ CONNECTING│ ← lws_client_connect_via_info() 调用
                    └─────┬─────┘
                     ┌────┴────┐
                     │         │
            ESTABLISHED    CONNECTION_ERROR
                     │         │
                     ▼         ▼
              ┌──────────┐  ┌────────┐
              │ CONNECTED│  │ FAILED │ → delete AudioPipe (BUG-05)
              └────┬─────┘  └────────┘
                   │
            close() / connection drop
                   │
                   ▼
             ┌──────────────┐
             │ DISCONNECTING│ → lws_callback_on_writable → LWS 关闭连接
             └──────┬───────┘
                    │ LWS_CALLBACK_CLIENT_CLOSED
                    ▼
             ┌──────────────┐
             │ DISCONNECTED │ → delete AudioPipe
             └──────────────┘
```

**生命周期管理**:
- AudioPipe 由 `inject_data_init()` 中的 `new` 创建
- 正常关闭: `LWS_CALLBACK_CLIENT_CLOSED` → `delete ap` (LWS 线程)
- 连接失败: `LWS_CALLBACK_CLIENT_CONNECTION_ERROR` → `delete ap` (LWS 线程)
- 会话清理: `inject_session_cleanup()` → `pAudioPipe->close()` → 触发异步关闭，最终在 LWS 线程 delete

---

## 7. 内存管理

### 7.1 内存分配策略

| 资源 | 分配方式 | 分配者 | 释放者 |
|------|---------|--------|--------|
| `private_t` | `switch_core_session_alloc()` | `inject_session_init` | FreeSWITCH session pool 自动释放 |
| `AudioPipe` | `new` | `inject_data_init` | LWS 线程 `delete` |
| `m_audio_buffer` | `new uint8_t[]` | AudioPipe 构造 | AudioPipe 析构 |
| `CircularBuffer_t` (playout) | `new` | `inject_data_init` | `destroy_tech_pvt` |
| `CircularBuffer_t` (prebuffer) | `new` | `inject_data_init` | `destroy_tech_pvt` |
| `SpeexResamplerState` | `speex_resampler_init()` | `inject_data_init` | `destroy_tech_pvt` |
| `switch_mutex_t` | `switch_mutex_init()` | `inject_data_init` | `destroy_tech_pvt` |

### 7.2 内存池使用

`private_t` 使用 FreeSWITCH 的 session memory pool 分配，该 pool 在 session 销毁时自动释放。但 `private_t` 内部的 C++ 对象（CircularBuffer、deque 等）需要显式释放，由 `destroy_tech_pvt()` 负责。

---

## 8. 性能分析

### 8.1 关键路径延迟

| 路径 | 操作 | 预估耗时 | 备注 |
|------|------|---------|------|
| 上行: inject_frame | trylock + bug_read + unlock | **5-20 μs** | 20ms 周期内最优路径 |
| 上行: inject_frame (重采样) | lock + bug_read + speex_resample + unlock | **30-80 μs** | Speex 重采样开销 |
| 上行: LWS write | lock + lws_write + unlock | **2-10 μs** | 内核 send，取决于 TCP 缓冲区 |
| 下行: dub_speech_frame | lock + buffer_read + memcpy + unlock | **3-15 μs** | 纯内存操作 |
| 下行: processIncomingBinary | lock + resample + buffer_write + unlock | **20-60 μs** | 含重采样和 buffer 操作 |
| API 命令处理 | session_locate + init/cleanup + rwunlock | **100-500 μs** | 包含内存分配和 DNS 解析 |

### 8.2 锁竞争分析

#### 8.2.1 `tech_pvt->mutex` (NESTED mutex)

**竞争方**:

| 持有者 | 持有时长 | 频率 | 阻塞影响 |
|--------|---------|------|---------|
| inject_frame (media线程) | 5-80 μs | 50次/秒 | trylock 失败则丢帧 |
| dub_speech_frame (media线程) | 3-15 μs | 50次/秒 | 阻塞锁，会影响 inject_frame |
| processIncomingBinary (LWS线程) | 20-60 μs | 取决于服务器 | 与 media 线程竞争 |
| inject_session_cleanup (API线程) | 10-50 μs | 1次/通话 | 会阻塞 media 线程 |

**风险评估**:
- `inject_frame` 使用 `trylock`，竞争时丢弃当前帧（可接受，20ms 后重试）
- `dub_speech_frame` 使用阻塞锁，但持锁时间极短（<15μs），与 LWS 线程竞争窗口小
- 最大风险: `inject_session_cleanup` 持锁期间如果同时有多个 LWS 回调，可能造成短暂阻塞

#### 8.2.2 `m_audio_mutex`

**竞争方**:

| 持有者 | 持有时长 | 频率 |
|--------|---------|------|
| inject_frame (media线程) | 5-80 μs | 50次/秒 |
| LWS_CALLBACK_CLIENT_WRITEABLE (LWS线程) | 2-10 μs | 取决于数据量 |

**风险评估**: 低。生产者（media 线程）和消费者（LWS 线程）交替运行，锁持有时间短。

### 8.3 CPU 开销

#### 8.3.1 采样率转换

Speex Resampler 的 CPU 开销与采样率比和帧大小成正比：

| 转换 | 每帧样本数 (20ms) | 每帧 CPU 估算 | 每秒 CPU |
|------|-------------------|--------------|---------|
| 8k → 8k (无转换) | 160 | 0 | 0 |
| 8k → 16k | 160→320 | ~2 μs | ~0.1 ms |
| 8k → 24k | 160→480 | ~3 μs | ~0.15 ms |
| 16k → 8k | 320→160 | ~2 μs | ~0.1 ms |

单会话重采样 CPU 开销可忽略不计（< 0.01% CPU）。

#### 8.3.2 SIMD 向量运算

`vector_math.cpp` 提供了 AVX2/SSE2/标量三种实现：

| 实现 | 同时处理样本数 | 适用场景 |
|------|--------------|---------|
| AVX2 | 16 samples/cycle | 现代 x86 服务器 |
| SSE2 | 8 samples/cycle | 老式 x86 服务器 |
| 标量 | 1 sample/cycle | ARM / 其他平台 |

当前 `vector_add` / `vector_normalize` 主要在下行音频混合场景使用，`vector_change_sln_volume_granular` 用于音量调节。在当前的下行 dub 路径中未被直接调用，但为未来音频混合场景预留。

#### 8.3.3 LWS 事件循环

LWS service 线程使用 `lws_service(context, 0)` 轮询，超时为 0（非阻塞）。空闲时 CPU 占用约 0%，有数据时取决于 WebSocket 帧大小和 TLS 开销。

TLS 握手开销: 一次完整 TLS 握手约 1-5ms CPU 时间（RSA-2048），ECDHE 约 2-8ms。

### 8.4 内存开销估算

**单会话内存** (16kHz mono, 2秒缓冲):

| 组件 | 大小 |
|------|------|
| private_t | ~0.5 KB |
| AudioPipe | ~150 bytes (对象本身) |
| m_audio_buffer | LWS_PRE(256) + 64,000 = ~64 KB |
| CircularBuffer (playout) | 初始 8192 samples × 2 = 16 KB (可动态增长) |
| CircularBuffer (prebuffer) | 初始 8192 samples × 2 = 16 KB (可动态增长) |
| SpeexResamplerState | ~4 KB |
| **总计** | **~102 KB / 会话** |

**1000 并发会话**: ~102 MB 内存

### 8.5 网络带宽估算

| 采样率 | 声道 | 原始带宽 | WebSocket 开销 (~2%) | 实际带宽 |
|--------|------|---------|---------------------|---------|
| 8kHz | 1 | 128 kbps | ~3 kbps | ~131 kbps |
| 16kHz | 1 | 256 kbps | ~5 kbps | ~261 kbps |
| 16kHz | 2 (stereo) | 512 kbps | ~10 kbps | ~522 kbps |
| 24kHz | 1 | 384 kbps | ~8 kbps | ~392 kbps |

**1000 并发会话 (16kHz mono)**: ~260 Mbps 出站带宽

### 8.6 性能瓶颈与优化建议

| 瓶颈 | 影响 | 优化建议 |
|------|------|---------|
| `trylock` 丢帧 | 高负载时上行音频丢失 | 增大 audio buffer（MOD_AUDIO_INJECT_BUFFER_SECS） |
| `dub_speech_frame` 阻塞锁 | LWS 回调写入 playoutBuffer 时可能阻塞 media 线程 | 使用 lock-free 队列替代 CircularBuffer |
| Speex Resampler 单线程 | 重采样在 media 线程中同步执行 | 对于高采样率转换，考虑预计算 FIR 系数或使用更快的重采样库 |
| LWS 单线程模型 | 所有 WebSocket 连接共享一个线程 | 高并发场景下增加 LWS service 线程数（当前 nServiceThreads 未启用） |
| CircularBuffer `set_capacity` | 涉及内存重新分配和数据拷贝 | 预分配足够容量，避免运行时扩容 |
| `processIncomingBinary` 中 `cBuffer->insert` | O(n) 拷贝开销 | 使用 ring buffer 优化批量插入 |

---

## 9. 配置参数

### 9.1 环境变量

| 环境变量 | 默认值 | 范围 | 说明 |
|---------|--------|------|------|
| `MOD_AUDIO_INJECT_BUFFER_SECS` | 2 | 1-5 | 上行音频缓冲区大小（秒） |
| `MOD_AUDIO_INJECT_TCP_KEEPALIVE_SECS` | 55 | - | TCP keepalive 间隔（秒） |
| `MOD_AUDIO_INJECT_SUBPROTOCOL_NAME` | `audio.drachtio.org` | - | WebSocket 子协议名称 |
| `MOD_AUDIO_INJECT_SERVICE_THREADS` | 1 | 1-5 | LWS service 线程数（当前未启用多线程） |
| `MOD_AUDIO_INJECT_HTTP_AUTH_USER` | (空) | - | 全局 HTTP Basic Auth 用户名 |
| `MOD_AUDIO_INJECT_HTTP_AUTH_PASSWORD` | (空) | - | 全局 HTTP Basic Auth 密码 |

### 9.2 Channel Variables (per-call)

| 变量 | 说明 |
|------|------|
| `MOD_AUDIO_BASIC_AUTH_USERNAME` | 当前通话的 HTTP Basic Auth 用户名 |
| `MOD_AUDIO_BASIC_AUTH_PASSWORD` | 当前通话的 HTTP Basic Auth 密码 |
| `MOD_AUDIO_INJECT_ALLOW_SELFSIGNED` | 允许自签名 TLS 证书 |
| `MOD_AUDIO_INJECT_SKIP_SERVER_CERT_HOSTNAME_CHECK` | 跳过 TLS 证书主机名验证 |
| `MOD_AUDIO_INJECT_ALLOW_EXPIRED` | 允许过期 TLS 证书 |

### 9.3 API 命令

```
uuid_audio_inject <uuid> start <wss-url> [mono|mixed|stereo] [8k|16k|24k|32k|64k] \
  [bugname] [bidirectionalAudio_enabled] [bidirectionalAudio_stream_enabled] \
  [bidirectionalAudio_stream_samplerate]

uuid_audio_inject <uuid> stop [bugname]
uuid_audio_inject <uuid> pause [bugname]
uuid_audio_inject <uuid> resume [bugname]
uuid_audio_inject <uuid> graceful-shutdown [bugname]
uuid_audio_inject <uuid> stop_play [bugname]
```

---

## 10. 事件系统

模块通过 FreeSWITCH 自定义事件子系统向应用层推送状态变更：

| 事件子类 | 触发时机 | 载荷 |
|---------|---------|------|
| `mod_audio_inject::connect` | WebSocket 连接成功 | 无 |
| `mod_audio_inject::connect_failed` | WebSocket 连接失败 | `{"reason":"..."}` |
| `mod_audio_inject::disconnect` | 连接被远端关闭 | 无 |
| `mod_audio_inject::error` | 收到 error 消息 | JSON data |
| `mod_audio_inject::buffer_overrun` | 上行缓冲区溢出 | 无 |

---

## 11. 安全考量

| 项目 | 实现 |
|------|------|
| 传输加密 | WSS (TLS 1.2+)，由 LWS 原生支持 |
| 认证 | HTTP Basic Auth（握手阶段），凭据通过 Channel Variables 传入 |
| 证书验证 | 默认启用；可通过 Channel Variables 放宽（自签名/过期/跳过主机名检查） |
| 输入验证 | `parse_ws_uri` 解析 URL，`parse_json` 解析消息，`strncpy` 边界保护 |
| 内存安全 | session pool 管理 private_t，RAII 管理 AudioPipe，mutex 保护共享数据 |

---

## 12. 依赖库

| 依赖 | 版本要求 | 用途 |
|------|---------|------|
| FreeSWITCH | >= 1.10 | 核心 API, Media Bug, 事件系统 |
| libwebsockets | >= 4.0 | WebSocket 客户端, TLS, 事件循环 |
| speexdsp | >= 1.2 | 采样率转换 (Speex Resampler) |
| Boost | >= 1.71 | circular_buffer (header-only) |
| cJSON | (FreeSWITCH 内置) | JSON 解析 |
