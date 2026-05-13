# mod_audio_inject 代码审查报告（第二轮）

**审查日期**: 2026-05-12
**审查视角**: 资深 C/C++ 架构师 + FreeSWITCH 高级工程师
**审查范围**: 全部源文件，排除已在第一批修复的 BUG-01 ~ BUG-22
**严重级别**: P0=崩溃/数据损坏, P1=内存泄漏/安全漏洞, P2=逻辑错误/竞态条件, P3=代码质量

---

## P0 - 崩溃级 Bug

### ~~BUG-23~~: 误报 — `destroy_tech_pvt` 中 `switch_mutex_destroy` 使用 session pool 分配的 mutex 是安全的

**状态**: 误报，不是问题。

分析：`SWITCH_ABC_TYPE_CLOSE` 回调触发时，session 仍在销毁流程中，session pool 尚未回收，所有从 pool 分配的内存仍然有效。`switch_mutex_destroy` 底层调用 `apr_thread_mutex_destroy`，会从 pool 的 cleanup 列表中移除该 mutex 的注册后再销毁，因此 pool 后续销毁时不会重复清理。无论 `channelIsClosing` 为 0 还是 1，当前代码都是安全的。

---

### BUG-24: `LWS_CALLBACK_CLIENT_RECEIVE` 中 `malloc(0)` 导致未定义行为

**文件**: `audio_pipe.cpp:172-174`
**位置**: 文本帧首片且 `len + lws_remaining_packet_payload(wsi) == 0`

当 WebSocket 收到空文本帧时，`m_recv_buf_len` 为 0，`malloc(0)` 的行为由实现定义——可能返回 NULL 或一个不可解引用的非 NULL 指针。后续 `memcpy(ap->m_recv_buf_ptr, in, len)` 虽然长度为 0 不会越界，但 `ap->m_recv_buf_ptr - ap->m_recv_buf` 的运算和 `free` 调用可能在某些平台上产生异常。

```cpp
ap->m_recv_buf_len = len + lws_remaining_packet_payload(wsi);  // 可能为 0
ap->m_recv_buf = (uint8_t*) malloc(ap->m_recv_buf_len);         // malloc(0) = UB
ap->m_recv_buf_ptr = ap->m_recv_buf;
```

**修复方案**:
```cpp
ap->m_recv_buf_len = len + lws_remaining_packet_payload(wsi);
if (ap->m_recv_buf_len == 0) {
  // 空消息，直接回调
  ap->m_callback(ap->m_uuid.c_str(), ap->m_bugname.c_str(), AudioPipe::MESSAGE, "", NULL, 0);
  break;
}
ap->m_recv_buf = (uint8_t*) malloc(ap->m_recv_buf_len);
```

---

### BUG-25: `LWS_CALLBACK_CLIENT_RECEIVE` 中 `realloc` 失败后 `m_recv_buf_ptr` 未更新

**文件**: `audio_pipe.cpp:189-194`
**位置**: 缓冲区扩容路径

当 `realloc` 返回 NULL 时，原始内存块未被释放（realloc 失败时原指针仍有效），但代码只检查了 `nullptr != ap->m_recv_buf`。如果 `realloc` 失败，`ap->m_recv_buf` 被设为 NULL（原内存泄漏），而 `ap->m_recv_buf_ptr` 仍指向已被 `realloc` 释放（或未释放但丢失引用）的旧地址。后续 `ap->m_recv_buf_ptr - ap->m_recv_buf` 产生未定义行为。

```cpp
ap->m_recv_buf = (uint8_t*) realloc(ap->m_recv_buf, newlen);
if (nullptr != ap->m_recv_buf) {
  ap->m_recv_buf_len = newlen;
  ap->m_recv_buf_ptr = ap->m_recv_buf + write_offset;
}
// else: m_recv_buf = NULL, 但 m_recv_buf_ptr 仍指向旧地址!
// 且原内存已泄漏（realloc 失败不释放原内存，但我们的指针被覆盖为 NULL）
```

**修复方案**:
```cpp
uint8_t* new_buf = (uint8_t*) realloc(ap->m_recv_buf, newlen);
if (nullptr != new_buf) {
  ap->m_recv_buf = new_buf;
  ap->m_recv_buf_len = newlen;
  ap->m_recv_buf_ptr = new_buf + write_offset;
} else {
  // realloc 失败，原 m_recv_buf 仍有效，但缓冲区不足
  // 丢弃当前消息，释放旧缓冲区
  free(ap->m_recv_buf);
  ap->m_recv_buf = ap->m_recv_buf_ptr = nullptr;
  ap->m_recv_buf_len = 0;
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "realloc failed, discarding message\n");
}
```

---

## P1 - 内存泄漏 / 安全漏洞

### ~~BUG-26~~: 误报 — `start_capture` 连接失败路径不可达

**状态**: 误报，不是问题。

分析：`inject_session_connect` 永远返回 `SWITCH_STATUS_SUCCESS`，因为它只是调用 `AudioPipe::connect()` 将连接请求加入 pending 队列（`addPendingConnect`），是异步操作。连接真正的成功或失败由 LWS 线程异步回调处理（`LWS_CALLBACK_CLIENT_ESTABLISHED` / `LWS_CALLBACK_CLIENT_CONNECTION_ERROR`），通过 `eventCallback` 通知应用层。`start_capture` 第118行的 `if` 分支是死代码，永远不会进入。

真正的连接失败走的是 `eventCallback` → `CONNECT_FAIL` → `tech_pvt->pAudioPipe = nullptr` 路径，应用层收到 `mod_audio_inject::connect_failed` 事件后应调用 `stop` 清理。

---

### BUG-27: `dch_lws_http_basic_auth_gen` 中 `lws_snprintf` 返回值类型误用

**文件**: `audio_pipe.cpp:31`
**位置**: Basic Auth 生成函数

`lws_snprintf` 返回 `size_t`（实际写入的字节数），但代码将其赋值给 `n`（原为 `strlen(user)` 的 `size_t` 类型），然后作为 `lws_b64_encode_string` 的输入长度。虽然 `size_t` 是无符号类型不会有负值问题，但如果 `lws_snprintf` 截断了输出（`n >= sizeof(b) - 2`），`lws_b64_encode_string` 会编码截断的凭证，导致认证失败——这是一个功能性 bug，且截断的凭证可能产生意外的 base64 输出。

更关键的是：`sizeof(b) = 128`，当 `user:password` 总长度超过 126 字节时，凭证被静默截断，`lws_b64_encode_string` 可能写出超出 `buf` 容量的数据（因为 `len` 检查基于完整长度的估算，但实际编码的是截断后的数据——截断反而减小了编码输出，所以不会溢出，但认证会静默失败）。

**修复方案**: 已有截断检查 `if (n >= sizeof(b) - 2) return 2;`，但返回值未被调用方检查。`LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER` 中 `dch_lws_http_basic_auth_gen` 返回非零时 `break`，不会添加 Authorization header，但连接仍会继续——应返回 -1 终止连接。

---

### ~~BUG-28~~: 误报 — `AudioPipe::connect_client` 中不存在竞态窗口

**状态**: 误报，不是问题。已修复冗余代码。

分析：`lws_client_connect_via_info` 只是发起连接请求，不会同步完成连接。连接的建立或失败都在后续 `lws_service` 循环中异步处理。即使连接极快失败，`CONNECTION_ERROR` 回调也要等到下一次 `lws_service` 迭代才触发，此时 `connect_client` 早已返回，`m_wsi` 已设置完毕。且 `findAndRemovePendingConnect` 按 `m_wsi == wsi` 匹配，只有 `lws_client_connect_via_info` 返回 wsi 之后回调才能匹配到此 ap。因此不存在竞态。

已修复：删除了 `connect_client` 中冗余的 `m_state = LWS_CLIENT_CONNECTING`（该状态已由 `processPendingConnects` 在 `mutex_connects` 锁内设置）。

---

## P2 - 逻辑错误 / 竞态条件

### BUG-29: `inject_function` API 命令解析中 `assert(cmd)` 在生产环境无意义

**文件**: `mod_audio_inject.c:202`
**位置**: `inject_function` 入口

```cpp
assert(cmd);
```

FreeSWITCH API 回调函数中 `cmd` 参数不应为 NULL（框架保证），但如果确实为 NULL，`assert` 在 Release 编译（`-DNDEBUG`）时被移除，不会提供任何保护。紧接着的 `if (!zstr(cmd))` 检查才真正防止 NULL 解引用。`assert` 在此处既冗余又在 Release 模式下无效，应移除或替换为运行时检查。

**修复方案**: 删除 `assert(cmd);` 行，后续的 `zstr(cmd)` 检查已足够。

---

### BUG-30: `inject_function` 中 `argv` 数组大小为 10 但 `start` 命令需要 10 个参数（0-9 索引）

**文件**: `mod_audio_inject.c:194`
**位置**: API 命令解析

```cpp
char *mycmd = NULL, *argv[10] = { 0 };
int argc = 0;
// ...
if (argc > 9) {  // 访问 argv[9]
  bidirectional_audio_sample_rate = atoi(argv[9]);
```

`switch_separate_string` 最多填充 10 个元素（索引 0-9），`argc > 9` 时访问 `argv[9]` 是安全的。但如果用户提供了超过 10 个参数，多余的参数被静默忽略。更重要的是，`argv` 数组恰好 10 个槽位，`switch_separate_string` 传入 `sizeof(argv) / sizeof(argv[0]) = 10`，所以 `argv[9]` 是最后一个可用索引——没有越界，但扩展性差。

**修复方案**: 增大 `argv` 数组或添加明确的参数数量上限检查。

---

### BUG-31: `inject_function` 中 `start` 命令的 `parse_ws_uri` 失败后仍继续执行 `start_capture`

**文件**: `mod_audio_inject.c:319-326`
**位置**: start 命令处理

```cpp
if (!parse_ws_uri(channel, argv[2], &host[0], &path[0], &port, &sslFlags)) {
  switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "invalid websocket uri: %s\n", argv[2]);
  // ← 没有跳过 start_capture! 继续执行...
}
else if (sampling % 8000 != 0) {
  switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "invalid sample rate: %s\n", argv[4]);					
  // ← 同样没有跳过 start_capture!
}
status = start_capture(lsession, flags, host, port, path, sampling, sslFlags, ...);
// ← host/path/port/sslFlags 可能包含未初始化或部分初始化的数据
```

当 `parse_ws_uri` 失败时，`host`、`path`、`port`、`sslFlags` 可能包含栈上的未初始化数据（`start_capture` 的栈变量），`start_capture` 会使用这些垃圾值发起连接。

同样，当 `sampling % 8000 != 0` 时，仍然会执行 `start_capture`。

**修复方案**:
```cpp
if (!parse_ws_uri(channel, argv[2], &host[0], &path[0], &port, &sslFlags)) {
  switch_log_printf(..., "invalid websocket uri: %s\n", argv[2]);
  switch_core_session_rwunlock(lsession);
  goto done;
}
if (sampling % 8000 != 0) {
  switch_log_printf(..., "invalid sample rate: %s\n", argv[4]);
  switch_core_session_rwunlock(lsession);
  goto done;
}
```

---

### BUG-32: `inject_session_connect` 无 NULL 检查直接解引用

**文件**: `lws_glue.cpp:728-733`
**位置**: `inject_session_connect` 函数

```cpp
switch_status_t inject_session_connect(void **ppUserData) {
  private_t *tech_pvt = static_cast<private_t *>(*ppUserData);
  drachtio::AudioPipe *pAudioPipe = static_cast<drachtio::AudioPipe*>(tech_pvt->pAudioPipe);
  pAudioPipe->connect();  // ← 如果 pAudioPipe 为 NULL 则崩溃
  return SWITCH_STATUS_SUCCESS;
}
```

虽然正常流程下 `inject_session_connect` 在 `inject_session_init` 成功后调用，`pAudioPipe` 不应为 NULL。但如果 `inject_data_init` 中 `new (std::nothrow) AudioPipe` 失败返回了 NULL，`inject_data_init` 会返回 `SWITCH_STATUS_FALSE`，`inject_session_init` 会调用 `destroy_tech_pvt` 清理。然而 `destroy_tech_pvt` 现在会检查 `pAudioPipe` 是否为 NULL 再 delete，所以 init 失败路径是安全的。但防御性编程仍建议加检查。

**修复方案**:
```cpp
switch_status_t inject_session_connect(void **ppUserData) {
  private_t *tech_pvt = static_cast<private_t *>(*ppUserData);
  if (!tech_pvt || !tech_pvt->pAudioPipe) return SWITCH_STATUS_FALSE;
  drachtio::AudioPipe *pAudioPipe = static_cast<drachtio::AudioPipe*>(tech_pvt->pAudioPipe);
  pAudioPipe->connect();
  return SWITCH_STATUS_SUCCESS;
}
```

---

### BUG-33: `processIncomingBinary` 中 `downscale_factor` 为 0 时除零

**文件**: `lws_glue.cpp:118,121`
**位置**: `downsample_factor` 计算路径

```cpp
size_t downsample_factor = tech_pvt->downscale_factor;  // 从 int 隐式转换
size_t numCompleteSamples = (cBuffer->size() / downsample_factor) * downsample_factor;
```

`tech_pvt->downscale_factor` 类型为 `int`，初始化为 1。但 `downscale_factor` 被赋给 `size_t`（无符号），如果 `downscale_factor` 意外为 0 或负数，除以 `size_t(0)` 会导致 FPE 崩溃。

当前初始化逻辑 `tech_pvt->downscale_factor = 1`，且只在 `bidirectional_audio_sample_rate > sampling` 时设为更大值，所以正常流程不会为 0。但作为防御性编程，应加检查。

---

### BUG-34: `eventCallback` 中 `CONNECT_FAIL` 路径的 `message` 可能包含格式化字符导致日志注入

**文件**: `lws_glue.cpp:382-386`
**位置**: `eventCallback` → `CONNECT_FAIL` 分支

```cpp
std::stringstream json;
json << "{\"reason\":\"" << message << "\"}";
tech_pvt->pAudioPipe = nullptr;
tech_pvt->responseHandler(session, EVENT_CONNECT_FAIL, (char *) json.str().c_str());
```

`message` 来自 LWS 回调的 `(char*)in`，内容不可信。如果 `message` 包含 `"` 或 `\` 等字符，构造的 JSON 格式会被破坏。更严重的是如果包含 `%s`、`%n` 等格式化字符，`switch_event_add_body` 使用 `%s` 格式化是安全的，但 `switch_log_printf` 使用 `message` 时（如第386行）如果作为格式化字符串传入则存在格式化字符串漏洞。

检查第386行：`switch_log_printf(..., "connection failed: %s\n", message)` — 这里 `%s` 是正确的，`message` 作为参数而非格式化字符串，安全。

但 JSON 构造仍有问题：`message` 中的 `"` 会破坏 JSON 结构。

**修复方案**: 对 `message` 中的特殊字符进行转义，或使用 cJSON 构建 JSON 而非手动拼接。

---

### BUG-35: `AudioPipe::close()` 在 `LWS_CLIENT_CONNECTING` 状态不生效，导致资源浪费

**文件**: `audio_pipe.cpp:561-564`
**位置**: `close()` 方法
**严重级别**: P3（降级，原 P2 评估有误）

```cpp
void AudioPipe::close() {
  if (m_state != LWS_CLIENT_CONNECTED) return;
  addPendingDisconnect(this);
}
```

当用户在连接还在 `CONNECTING` 状态时调用 stop，`close()` 直接 return 不做任何事。之后 `inject_session_cleanup` 设 `tech_pvt->pAudioPipe = nullptr` 并执行 `destroy_tech_pvt`。

如果连接随后成功建立：
- `eventCallback` 中 `switch_channel_get_private` 返回 NULL（cleanup 已设 NULL）→ 跳过，不会崩溃
- AudioPipe 仍被 LWS 通过 `*ppAp` 持有，连接保持活跃直到远端关闭 → `CLIENT_CLOSED` → `delete ap`
- **不会崩溃，不会内存泄漏**，但浪费一个 WebSocket 连接直到超时

如果连接失败：
- `CLIENT_CONNECTION_ERROR` → `delete ap` → 无问题

**实际影响**: 资源浪费（连接保持到远端关闭），非安全漏洞。

**修复方案**: `close()` 应处理 `CONNECTING` 状态——从 `pendingConnects` 中移除并直接 delete：
```cpp
void AudioPipe::close() {
  if (m_state == LWS_CLIENT_CONNECTED) {
    addPendingDisconnect(this);
    return;
  }
  if (m_state == LWS_CLIENT_CONNECTING) {
    m_state = LWS_CLIENT_FAILED;
    // 从 pendingConnects 中移除由 LWS 线程处理，
    // 或直接由调用方 delete（因为 tech_pvt->pAudioPipe 会被置 nullptr）
  }
}
```

---

### BUG-36: `processIncomingBinary` 异常路径未释放 `tech_pvt->mutex` 锁

**文件**: `lws_glue.cpp:157-163`
**位置**: resample 异常处理

```cpp
try {
  // ... resampling ...
} catch (const std::exception& e) {
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error resampling incoming binary message: %s\n", e.what());
  return SWITCH_STATUS_FALSE;
  // ← 此时尚未获取 mutex，所以不会死锁
  // ← 但 prebuffer 中的数据处于不一致状态（可能已部分消费）
}
```

异常发生在 resample 阶段（mutex 加锁之前），所以不会死锁。但 prebuffer (`cBuffer`) 中的数据可能已被部分修改（leftover samples 已被移除但未放回），后续调用可能处理不一致的数据。

**修复方案**: 在异常路径中清空 prebuffer 以恢复一致状态。

---

### BUG-37: `LWS_CALLBACK_CLIENT_WRITEABLE` 发送文本时只发送一条消息就返回

**文件**: `audio_pipe.cpp:234-253`
**位置**: 文本消息发送

```cpp
if (!ap->m_metadata_list.empty()) {
  const std::string& message = ap->m_metadata_list.front();
  // ... send ...
  ap->m_metadata_list.pop_front();
  lws_callback_on_writable(wsi);  // 请求下一次 writable 事件
  return 0;
}
```

每次 WRITEABLE 回调只发送一条文本消息。如果队列中有多条消息，需要等下一个 writable 事件。这在低负载下没问题，但高频率 `send_text` 时会导致消息积压和延迟。

**修复方案**: 在一次 WRITEABLE 回调中循环发送所有排队消息（注意 LWS 不建议在一次回调中发送过多数据，但文本消息通常较小）。

---

### BUG-38: `dub_speech_frame` 中 `samples_needed` 可能为 0 或负数

**文件**: `lws_glue.cpp:970-975`
**位置**: `rframe->samples` 的使用

```cpp
switch_frame_t* rframe = switch_core_media_bug_get_write_replace_frame(bug);
if (rframe && rframe->datalen > 0) {
  int16_t *fp = reinterpret_cast<int16_t*>(rframe->data);
  int samples_needed = rframe->samples;
  int samplesToCopy = std::min(static_cast<int>(cBuffer->size()), samples_needed);
```

虽然 FreeSWITCH 通常保证 `rframe->samples > 0`，但如果 `samples_needed` 为 0，后续 `std::vector<int16_t> data(samples_needed, 0)` 创建空 vector，`memcpy(fp, data.data(), 0)` 安全。但如果 `samples_needed` 为负数（数据损坏场景），`std::vector` 构造会抛出 `std::bad_alloc`，且 `memcpy` 的 `samples_needed * sizeof(int16_t)` 会产生极大的 size → 缓冲区溢出。

**修复方案**: 添加 `samples_needed` 的正数检查。

---

## P3 - 代码质量

### BUG-39: SSE2 编译路径仍缺少 `vector_change_sln_volume_granular` 定义

**文件**: `vector_math.cpp:117-158`
**位置**: `#elif defined(USE_SSE2)` 分支

SSE2 分支定义了 `vector_add` 和 `vector_normalize`，但未定义 `vector_change_sln_volume_granular`。链接时会报 `undefined reference` 错误。这是一个已知的 P0 编译错误，在此再次标记以确认未修复。

**修复方案**: 在 SSE2 分支中添加实现，或 fallback 到标量版本。

---

### BUG-40: `normalize_volume_granular` 宏缺少大括号，在 if-else 上下文中不安全

**文件**: `vector_math.cpp:10`
**位置**: 宏定义

```cpp
#define normalize_volume_granular(x) if (x > GRANULAR_VOLUME_MAX) x = GRANULAR_VOLUME_MAX; if (x < -GRANULAR_VOLUME_MAX) x = -GRANULAR_VOLUME_MAX;
```

这是两个独立的 `if` 语句，不是 `if-else`。在以下上下文中使用：
```cpp
if (vol == 0) return;
normalize_volume_granular(vol);  // 展开为两个 if，语义正确但不直观
```

如果将来有人在 `normalize_volume_granular` 后加 `else`，会只绑定到第二个 `if`，产生难以发现的逻辑错误。

**修复方案**: 改用 `do { ... } while(0)` 包装，或改为内联函数。

---

### BUG-41: `dub_speech_frame` 中使用 `static` 局部变量统计信息不是线程安全的

**文件**: `lws_glue.cpp:927-929`
**位置**: `call_count` 和 `underrun_count`

```cpp
static uint32_t call_count = 0;
static uint32_t underrun_count = 0;
call_count++;
```

这些 `static` 变量在多个 session 的 media 线程中共享（不同 session 的 `dub_speech_frame` 都会访问），但没有任何同步保护。`call_count++` 不是原子操作，数据竞态导致计数不准确。虽然这些变量仅用于调试日志，不参与业务逻辑，但竞态写入在 TSan 下会报警。

**修复方案**: 使用 `std::atomic<uint32_t>` 或改用 per-session 统计。

---

### BUG-42: `LWS_CALLBACK_CLIENT_CLOSED` 中 `delete ap` 后可能有其他线程仍持有引用

**文件**: `audio_pipe.cpp:134-136`
**位置**: 连接关闭回调

```cpp
*ppAp = NULL;
delete ap;
```

`delete ap` 在 LWS 线程中执行。但此时 `tech_pvt->pAudioPipe` 可能尚未被设为 nullptr（取决于 `eventCallback` 中的 `CONNECTION_DROPPED`/`CLOSED_GRACEFULLY` 回调是否已执行并设了 nullptr）。

在当前代码中，`LWS_CALLBACK_CLIENT_CLOSED` 先调用 `m_callback`（即 `eventCallback`），在 `eventCallback` 中设 `tech_pvt->pAudioPipe = nullptr`，然后再 `delete ap`。这个顺序是正确的。但 `eventCallback` 中 `CONNECTION_DROPPED` 分支不调用 `destroy_tech_pvt`，所以 `tech_pvt` 仍持有 `pAudioPipe = nullptr` 的状态，后续 `inject_session_cleanup` 中不会 double-delete。

这条确认：当前代码在 `CONNECTION_DROPPED`/`CLOSED_GRACEFULLY` 路径下，`delete ap` 不会导致 double-delete（因为 `eventCallback` 先设 nullptr，而 `inject_session_cleanup` 中 close 后也设 nullptr，`destroy_tech_pvt` 检查 nullptr 后跳过 delete）。

**风险等级降为 P3**: 逻辑正确但依赖调用顺序，建议添加注释说明 `m_callback` 必须在 `delete ap` 之前执行。

---

### BUG-43: `processIncomingBinary` 中 `reinterpret_cast<const uint16_t*>(data.data())` 存在对齐问题

**文件**: `lws_glue.cpp:73`
**位置**: 二进制数据转 uint16_t

```cpp
std::vector<uint8_t> data;
// ... push_back 操作 ...
const uint16_t* data_uint16 = reinterpret_cast<const uint16_t*>(data.data());
```

`std::vector<uint8_t>` 的数据不一定满足 `uint16_t` 的对齐要求（2 字节对齐）。在某些 ARM 平台上，未对齐访问会产生 SIGBUS。x86 上通常可以工作但有性能惩罚。

**修复方案**: 使用 `memcpy` 逐个读取 uint16_t，或确保 vector 的内存对齐。

---

### BUG-44: `inject_session_stop_play` 使用 `trylock` 可能静默失败

**文件**: `lws_glue.cpp:1042-1048`
**位置**: `inject_session_stop_play` 函数

```cpp
if (switch_mutex_trylock(tech_pvt->mutex) == SWITCH_STATUS_SUCCESS) {
  if (cBuffer != nullptr) {
    cBuffer->clear();
  }
  switch_mutex_unlock(tech_pvt->mutex);
}
return SWITCH_STATUS_SUCCESS;  // ← trylock 失败也返回 SUCCESS!
```

当 `trylock` 失败时，`cBuffer->clear()` 未执行，但函数返回 `SWITCH_STATUS_SUCCESS`。调用方无法知道操作是否实际完成。

**修复方案**: `trylock` 失败时返回 `SWITCH_STATUS_FALSE`，或使用阻塞锁（`dub_speech_frame` 使用阻塞锁且持锁时间极短）。

---

## 修复优先级建议

1. **立即修复 (P0)**: BUG-23, BUG-24, BUG-25 — 崩溃或未定义行为
2. **尽快修复 (P1)**: BUG-26, BUG-27, BUG-28 — 内存泄漏和安全问题
3. **计划修复 (P2)**: BUG-29 ~ BUG-38 — 逻辑错误，可能导致功能异常
4. **择机修复 (P3)**: BUG-39 ~ BUG-44 — 代码质量，影响可维护性和特定平台兼容性

---

## 架构级建议

1. **AudioPipe 生命周期管理**: 当前 AudioPipe 的删除时机分布在三个地方（LWS 回调、`destroy_tech_pvt`、`inject_session_cleanup` 的 close 路径），建议统一由单一所有者管理。引入 `std::shared_ptr<AudioPipe>` + `std::weak_ptr<AudioPipe>` 可以优雅解决 double-delete 和 use-after-free 问题。

2. **tech_pvt 内存分配策略**: `tech_pvt` 使用 session pool 分配导致析构不可控。建议改为 `new` 分配，在 `destroy_tech_pvt` 中 `delete`，让生命周期完全由模块管理，避免与 FreeSWITCH pool 回收的交互问题。

3. **线程安全统一策略**: 当前混用了 FreeSWITCH mutex（`switch_mutex_t`）和 C++ mutex（`std::mutex`），以及 atomic 变量。建议统一为一种同步原语，减少混淆。FreeSWITCH mutex 适用于与 C 代码交互，C++ mutex 适用于纯 C++ 层。

4. **错误传播**: 多处错误路径（`parse_ws_uri` 失败、`trylock` 失败等）静默忽略，建议统一错误码和日志策略，确保所有失败路径对调用方可见。
