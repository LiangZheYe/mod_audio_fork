# mod_audio_inject

[中文文档](README_zh.md)

A FreeSWITCH module that attaches a media bug to a channel and streams L16 audio via WebSockets to a remote server. This module supports **bidirectional audio** — receiving audio back from the server via binary WebSocket frames for real-time playback to the caller, enabling full-fledged IVR, dialog, and voice-bot applications.

## Features

- **Bidirectional Audio** — Stream audio to a WebSocket server and receive audio back for real-time playback
- **Binary Audio Streaming** — Receive raw binary audio frames from the server for low-latency downlink
- **Multiple Mix Types** — Mono (caller only), mixed (caller + callee), or stereo (separate channels)
- **Flexible Sample Rates** — 8000, 16000, 24000, 32000, 48000, 64000 Hz (any multiple of 8000)
- **Automatic Resampling** — Built-in Speex resampler for sample rate conversion
- **TLS Support** — Secure WebSocket connections (wss://)
- **SIMD Optimized** — AVX2/SSE2 vector math for audio processing
- **Graceful Shutdown** — Drain audio buffers before closing connections

## Environment Variables

| Variable | Description | Default |
|---|---|---|
| `MOD_AUDIO_INJECT_SUBPROTOCOL_NAME` | WebSocket [sub-protocol](https://tools.ietf.org/html/rfc6455#section-1.9) name | `audio.drachtio.org` |
| `MOD_AUDIO_INJECT_SERVICE_THREADS` | Number of libwebsocket service threads (1–5) | `1` |
| `MOD_AUDIO_INJECT_BUFFER_SECS` | Audio buffer size in seconds (1–5) | `2` |

## Channel Variables

| Variable | Description |
|---|---|
| `MOD_AUDIO_BASIC_AUTH_USERNAME` | HTTP Basic Auth username for WebSocket connection |
| `MOD_AUDIO_BASIC_AUTH_PASSWORD` | HTTP Basic Auth password for WebSocket connection |
| `MOD_AUDIO_INJECT_ALLOW_SELFSIGNED` | Allow self-signed TLS certificates (`true`/`false`) |
| `MOD_AUDIO_INJECT_SKIP_SERVER_CERT_HOSTNAME_CHECK` | Skip TLS hostname verification (`true`/`false`) |
| `MOD_AUDIO_INJECT_ALLOW_EXPIRED` | Allow expired TLS certificates (`true`/`false`) |

## API

### Command Syntax

```
uuid_audio_inject <uuid> <command> [arguments...]
```

### Commands

#### start

```
uuid_audio_inject <uuid> start <wss-url> <mix-type> <sampling-rate> [bugname] [bidirectionalAudio_enabled] [bidirectionalAudio_stream_enabled] [bidirectionalAudio_stream_samplerate]
```

Attaches a media bug and starts streaming audio to the WebSocket server.

| Parameter | Description |
|---|---|
| `uuid` | FreeSWITCH channel UUID |
| `wss-url` | WebSocket URL (`ws://`, `wss://`, `http://`, or `https://`) |
| `mix-type` | `mono` (caller only), `mixed` (caller + callee), or `stereo` (separate channels) |
| `sampling-rate` | `8k`, `16k`, or any integer multiple of 8000 (e.g. `24000`, `32000`, `64000`) |
| `bugname` | Optional bug name for multiple concurrent forks (default: `audio_inject`) |
| `bidirectionalAudio_enabled` | `true` or `false` — enable receiving audio from server (default: `true`) |
| `bidirectionalAudio_stream_enabled` | `true` or `false` — enable binary audio streaming from server |
| `bidirectionalAudio_stream_samplerate` | Sample rate of incoming audio from server (e.g. `8000`, `16000`) |

#### stop

```
uuid_audio_inject <uuid> stop [bugname]
```

Closes the WebSocket connection and detaches the media bug.

#### pause

```
uuid_audio_inject <uuid> pause [bugname]
```

Pauses audio streaming (frames are discarded).

#### resume

```
uuid_audio_inject <uuid> resume [bugname]
```

Resumes audio streaming after a pause.

#### graceful-shutdown

```
uuid_audio_inject <uuid> graceful-shutdown [bugname]
```

Initiates a graceful shutdown — stops sending new audio but allows buffered audio to drain before closing.

#### stop_play

```
uuid_audio_inject <uuid> stop_play [bugname]
```

Stops any current audio playback by clearing the playout buffer.

### Events

The module generates the following FreeSWITCH custom events:

| Event | Description |
|---|---|
| `mod_audio_inject::connect` | WebSocket connection established successfully |
| `mod_audio_inject::connect_failed` | WebSocket connection failed (body contains reason) |
| `mod_audio_inject::disconnect` | WebSocket connection closed |
| `mod_audio_inject::error` | Error reported |
| `mod_audio_inject::buffer_overrun` | Audio buffer overrun — frames are being dropped |

### Binary Audio Streaming

When `bidirectionalAudio_stream_enabled` is set to `true`, the server sends raw binary audio frames directly over the WebSocket. The module handles:

- Automatic resampling if the server's sample rate differs from the channel's rate
- Pre-buffering to smooth out network jitter

## Building

See [BUILD.md](BUILD.md) for detailed build instructions.

### Quick Start

```bash
# Install dependencies, build, and install
chmod +x build.sh
sudo ./build.sh all

# Or step by step:
sudo ./build.sh deps      # Install build dependencies
./build.sh build           # Build the module
sudo ./build.sh install    # Install to FreeSWITCH
```

## Usage Example

```bash
# Start streaming with bidirectional audio
fs_cli -x "uuid_audio_inject <uuid> start wss://your-server.com/audio mixed 16k mybug true true 16000"

# Pause streaming
fs_cli -x "uuid_audio_inject <uuid> pause mybug"

# Resume streaming
fs_cli -x "uuid_audio_inject <uuid> resume mybug"

# Stop playback
fs_cli -x "uuid_audio_inject <uuid> stop_play mybug"

# Stop streaming
fs_cli -x "uuid_audio_inject <uuid> stop mybug"
```

## License

See [LICENSE](LICENSE) for details.
