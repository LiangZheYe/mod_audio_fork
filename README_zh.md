# mod_audio_inject

FreeSWITCH 模块，通过媒体 bug 采集通话音频，以 L16 格式经 WebSocket 流式传输到远程服务器。支持**双向音频**——通过二进制 WebSocket 帧接收服务端音频并实时回放给通话方，适用于 IVR、对话式及语音机器人等应用场景。

## 功能特性

- **双向音频** — 向 WebSocket 服务器流式传输音频，同时接收服务端音频实时回放
- **二进制音频流** — 接收服务端原始二进制音频帧，实现低延迟下行
- **多种混音模式** — 单声道（仅主叫）、混合（主叫 + 被叫）、立体声（左右声道分离）
- **灵活采样率** — 8000、16000、24000、32000、48000、64000 Hz（8000 的任意整数倍）
- **自动重采样** — 内置 Speex 重采样器，自动完成采样率转换
- **TLS 支持** — 支持 WebSocket 安全连接（wss://）
- **SIMD 优化** — AVX2/SSE2 向量化音频运算
- **优雅关闭** — 关闭连接前先排空音频缓冲区

## 环境变量

| 变量 | 说明 | 默认值 |
|---|---|---|
| `MOD_AUDIO_INJECT_SUBPROTOCOL_NAME` | WebSocket [子协议](https://tools.ietf.org/html/rfc6455#section-1.9)名称 | `audio.drachtio.org` |
| `MOD_AUDIO_INJECT_SERVICE_THREADS` | libwebsocket 服务线程数（1–5） | `1` |
| `MOD_AUDIO_INJECT_BUFFER_SECS` | 音频缓冲区大小（秒，1–5） | `2` |

## 通道变量

| 变量 | 说明 |
|---|---|
| `MOD_AUDIO_BASIC_AUTH_USERNAME` | WebSocket 连接的 HTTP Basic Auth 用户名 |
| `MOD_AUDIO_BASIC_AUTH_PASSWORD` | WebSocket 连接的 HTTP Basic Auth 密码 |
| `MOD_AUDIO_INJECT_ALLOW_SELFSIGNED` | 允许自签名 TLS 证书（`true`/`false`） |
| `MOD_AUDIO_INJECT_SKIP_SERVER_CERT_HOSTNAME_CHECK` | 跳过 TLS 主机名验证（`true`/`false`） |
| `MOD_AUDIO_INJECT_ALLOW_EXPIRED` | 允许过期 TLS 证书（`true`/`false`） |

## API

### 命令语法

```
uuid_audio_inject <uuid> <command> [参数...]
```

### 命令列表

#### start

```
uuid_audio_inject <uuid> start <wss-url> <mix-type> <sampling-rate> [bugname] [bidirectionalAudio_enabled] [bidirectionalAudio_stream_enabled] [bidirectionalAudio_stream_samplerate]
```

挂载媒体 bug，开始向 WebSocket 服务器流式传输音频。

| 参数 | 说明 |
|---|---|
| `uuid` | FreeSWITCH 通道 UUID |
| `wss-url` | WebSocket 地址（`ws://`、`wss://`、`http://` 或 `https://`） |
| `mix-type` | `mono`（仅主叫）、`mixed`（主叫 + 被叫）或 `stereo`（左右声道分离） |
| `sampling-rate` | `8k`、`16k` 或 8000 的任意整数倍（如 `24000`、`32000`、`64000`） |
| `bugname` | 可选 bug 名称，用于同时建立多路分流（默认：`audio_inject`） |
| `bidirectionalAudio_enabled` | `true` 或 `false` — 启用接收服务端音频（默认：`true`） |
| `bidirectionalAudio_stream_enabled` | `true` 或 `false` — 启用二进制音频流接收 |
| `bidirectionalAudio_stream_samplerate` | 服务端下行音频采样率（如 `8000`、`16000`） |

#### stop

```
uuid_audio_inject <uuid> stop [bugname]
```

关闭 WebSocket 连接并卸载媒体 bug。

#### pause

```
uuid_audio_inject <uuid> pause [bugname]
```

暂停音频流式传输（帧将被丢弃）。

#### resume

```
uuid_audio_inject <uuid> resume [bugname]
```

恢复暂停后的音频流式传输。

#### graceful-shutdown

```
uuid_audio_inject <uuid> graceful-shutdown [bugname]
```

优雅关闭——停止发送新音频，但等待缓冲区排空后再关闭连接。

#### stop_play

```
uuid_audio_inject <uuid> stop_play [bugname]
```

清除播放缓冲区，停止当前音频回放。

### 事件

模块产生以下 FreeSWITCH 自定义事件：

| 事件 | 说明 |
|---|---|
| `mod_audio_inject::connect` | WebSocket 连接成功建立 |
| `mod_audio_inject::connect_failed` | WebSocket 连接失败（事件体包含原因） |
| `mod_audio_inject::disconnect` | WebSocket 连接已关闭 |
| `mod_audio_inject::error` | 错误报告 |
| `mod_audio_inject::buffer_overrun` | 音频缓冲区溢出——帧正在被丢弃 |

### 二进制音频流

当 `bidirectionalAudio_stream_enabled` 设为 `true` 时，服务器通过 WebSocket 直接发送原始二进制音频帧。模块会自动处理：

- 服务端采样率与通道采样率不一致时自动重采样
- 预缓冲平滑网络抖动

## 编译

详细编译说明请参阅 [BUILD.md](BUILD.md)。

### 快速开始

```bash
# 一键安装依赖、编译并部署
chmod +x build.sh
sudo ./build.sh all

# 或分步执行：
sudo ./build.sh deps      # 安装编译依赖
./build.sh build           # 编译模块
sudo ./build.sh install    # 安装到 FreeSWITCH
```

## 使用示例

```bash
# 启动双向音频流
fs_cli -x "uuid_audio_inject <uuid> start wss://your-server.com/audio mixed 16k mybug true true 16000"

# 暂停流式传输
fs_cli -x "uuid_audio_inject <uuid> pause mybug"

# 恢复流式传输
fs_cli -x "uuid_audio_inject <uuid> resume mybug"

# 停止回放
fs_cli -x "uuid_audio_inject <uuid> stop_play mybug"

# 停止流式传输
fs_cli -x "uuid_audio_inject <uuid> stop mybug"
```

## 许可证

详见 [LICENSE](LICENSE)。
