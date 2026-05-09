# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```bash
# Convenience script (recommended)
sudo ./build.sh all          # deps + build + install
sudo ./build.sh deps         # install build dependencies
./build.sh build             # configure and build
sudo ./build.sh install      # copy .so to FreeSWITCH mod dir

# Manual build
mkdir build && cd build
cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DFREESWITCH_INCLUDE_DIR="/usr/local/freeswitch/include/freeswitch" \
  -DFREESWITCH_LIBRARY="/usr/local/freeswitch/lib/libfreeswitch.so"
make -j$(nproc)
sudo cp mod_audio_fork.so /usr/local/freeswitch/mod/
```

Overridable env vars: `FREESWITCH_INCLUDE_DIR`, `FREESWITCH_LIBRARY`, `FREESWITCH_MOD_DIR`, `BUILD_TYPE`.

Clean build: `rm -rf build/`

No test suite exists. Verify manually: `fs_cli -x "module_exists mod_audio_fork"` or `ldd build/mod_audio_fork.so`.

## Architecture

FreeSWITCH loadable module that streams call audio over WebSockets (L16/PCM) with bidirectional support — uplink forks call audio to a server, downlink injects server audio back into the call.

### Source Files

| File | Lang | Role |
|------|------|------|
| `mod_audio_fork.c` | C99 | FreeSWITCH module entry, API command dispatch, media bug callback routing |
| `mod_audio_fork.h` | C99 | `private_t` struct, constants, event type enums |
| `lws_glue.cpp` | C++11 | C/C++ bridge: audio capture (`fork_frame`), playback injection (`dub_speech_frame`), JSON/binary message processing, session lifecycle |
| `audio_pipe.cpp/hpp` | C++11 | WebSocket transport: LWS callbacks, connection state machine, thread-safe audio buffers |
| `parser.cpp/hpp` | C++11 | JSON message parsing (server→module commands) |
| `vector_math.cpp/h` | C99 | SIMD audio ops (AVX2/SSE2/scalar): add, normalize, volume |
| `base64.hpp` | C++11 | Header-only base64 encode/decode |

### Threading Model

Three thread types interact, making thread safety the central concern:

1. **FS Media threads** (per-session) — call `capture_callback` every 20ms for READ and WRITE_REPLACE events. Must return fast; no blocking I/O.
2. **FS API thread** — handles `uuid_audio_fork` CLI/ESL commands. Uses `switch_core_session_locate`/`rwunlock` for safe session access.
3. **LWS service thread** (global singleton) — runs `lws_service()` event loop. All LWS API calls must happen here. Other threads communicate via pending-operation lists + `lws_cancel_service()`.

### Key Synchronization

- `tech_pvt->mutex` (nested): protects `private_t` shared state and playout buffers. `fork_frame` uses `trylock` (non-blocking, drops frame on contention). `dub_speech_frame` uses blocking lock but holds it briefly (~3-15μs).
- `m_audio_mutex`: producer-consumer sync for uplink audio buffer between media thread (writer) and LWS thread (reader).
- `m_state` (atomic): connection state machine, readable across threads without locks.
- Pending-operation mutexes (`mutex_connects`, `mutex_disconnects`, `mutex_writes`): queue protection for cross-thread LWS requests.

### Data Flow

**Uplink (call → server):** Media thread → `fork_frame()` → `switch_core_media_bug_read()` writes directly into AudioPipe's `m_audio_buffer` (zero-copy when no resampling) → `unlockAudioBuffer()` triggers `lws_cancel_service()` → LWS thread sends via `lws_write()`.

**Downlink JSON (server → call):** LWS thread receives → `processIncomingMessage()` → base64-decode → lock `tech_pvt->mutex` → insert into `playoutBuffer`.

**Downlink binary (server → call):** LWS thread receives → `processIncomingBinary()` → byte-alignment fixup → write to `streamingPreBuffer` (absorbs jitter, ~120ms threshold) → Speex resample if needed → lock `tech_pvt->mutex` → insert into `playoutBuffer`.

**Playout (media thread, every 20ms):** `dub_speech_frame()` → lock `tech_pvt->mutex` → read from `playoutBuffer` → replace write frame via `switch_core_media_bug_set_write_replace_frame()`. Empty buffer → silence fill.

### AudioPipe Connection State Machine

`IDLE` → `CONNECTING` → `CONNECTED` → `DISCONNECTING` → `DISCONNECTED` (→ `delete`). On connect failure: `CONNECTING` → `FAILED` (→ `delete`). All `delete` calls happen in the LWS thread.

### Mark Mechanism

Server sends `{"type":"mark","data":{"name":"X"}}` before audio. Mark is queued, `AUDIO_MARKER` (0xFFFF) inserted into prebuffer. When `dub_speech_frame` consumes the marker, a mark event fires back to the server. Max 30 pending markers.

## Dependencies

- **FreeSWITCH** >= 1.10 (core API, media bug, events, built-in cJSON)
- **libwebsockets** >= 4.0 (WebSocket client, TLS, event loop)
- **speexdsp** >= 1.2 (Speex Resampler for sample rate conversion)
- **Boost** >= 1.71 (`circular_buffer` header-only)

## Configuration

### Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `MOD_AUDIO_FORK_SUBPROTOCOL_NAME` | `audio.drachtio.org` | WebSocket sub-protocol |
| `MOD_AUDIO_FORK_SERVICE_THREADS` | `1` | LWS service threads (1–5) |
| `MOD_AUDIO_FORK_BUFFER_SECS` | `2` | Uplink audio buffer in seconds (1–5) |
| `MOD_AUDIO_FORK_TCP_KEEPALIVE_SECS` | `55` | TCP keepalive interval |
| `MOD_AUDIO_FORK_HTTP_AUTH_USER` | (empty) | Global HTTP Basic Auth username |
| `MOD_AUDIO_FORK_HTTP_AUTH_PASSWORD` | (empty) | Global HTTP Basic Auth password |

### Per-Call Channel Variables

`MOD_AUDIO_BASIC_AUTH_USERNAME`, `MOD_AUDIO_BASIC_AUTH_PASSWORD`, `MOD_AUDIO_FORK_ALLOW_SELFSIGNED`, `MOD_AUDIO_FORK_SKIP_SERVER_CERT_HOSTNAME_CHECK`, `MOD_AUDIO_FORK_ALLOW_EXPIRED`.

## API Commands

All via `uuid_audio_fork <uuid> <command> [args...]`:

- `start <wss-url> <mix-type> <sampling-rate> [bugname] [metadata] [bidir_enabled] [bidir_stream_enabled] [bidir_stream_samplerate]` — attach media bug, start streaming
- `stop [bugname] [metadata]` — close connection, detach bug
- `send_text [bugname] <text>` — send text frame to server
- `pause [bugname]` / `resume [bugname]` — pause/resume streaming
- `graceful-shutdown [bugname]` — drain buffers then close
- `stop_play [bugname]` — clear playout buffer, stop playback

Mix types: `mono`, `mixed`, `stereo`. Sample rates: any multiple of 8000 (e.g. `8k`, `16k`, `24k`).

## Events

Module fires these FreeSWITCH custom events: `mod_audio_fork::connect`, `connect_failed`, `disconnect`, `buffer_overrun`, `transcription`, `transfer`, `play_audio`, `kill_audio`, `error`, `json`.
