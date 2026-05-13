#include "audio_pipe.hpp"

#include <switch.h>

#include <iostream>
#include <vector>

using namespace drachtio;

namespace {
static const char* basicAuthUser = std::getenv("MOD_AUDIO_INJECT_HTTP_AUTH_USER");
static const char* basicAuthPassword =
    std::getenv("MOD_AUDIO_INJECT_HTTP_AUTH_PASSWORD");

static const char* requestedTcpKeepaliveSecs =
    std::getenv("MOD_AUDIO_INJECT_TCP_KEEPALIVE_SECS");
static int nTcpKeepaliveSecs =
    requestedTcpKeepaliveSecs ? ::atoi(requestedTcpKeepaliveSecs) : 55;
}  // namespace

// remove once we update to lws with this helper
static int dch_lws_http_basic_auth_gen(const char* user, const char* pw,
                                       char* buf, size_t len) {
  size_t n = strlen(user), m = strlen(pw);
  char b[128];

  if (len <
      6 + ((4 * (n + m + 1) + 2) / 3) +
          1)  // BUG-06 fix: use ceiling division to prevent buffer overflow
    return 1;

  memcpy(buf, "Basic ", 6);

  n = lws_snprintf(b, sizeof(b), "%s:%s", user, pw);
  if (n >= sizeof(b) - 2) return 2;

  lws_b64_encode_string(b, n, buf + 6, len - 6);
  buf[len - 1] = '\0';

  return 0;
}

int AudioPipe::lws_callback(struct lws* wsi, enum lws_callback_reasons reason,
                            void* user, void* in, size_t len) {
  struct AudioPipe::lws_per_vhost_data* vhd =
      (struct AudioPipe::lws_per_vhost_data*)lws_protocol_vh_priv_get(
          lws_get_vhost(wsi), lws_get_protocol(wsi));

  AudioPipe** ppAp = (AudioPipe**)user;

  switch (reason) {
    case LWS_CALLBACK_PROTOCOL_INIT:
      vhd = (struct AudioPipe::lws_per_vhost_data*)lws_protocol_vh_priv_zalloc(
          lws_get_vhost(wsi), lws_get_protocol(wsi),
          sizeof(struct AudioPipe::lws_per_vhost_data));
      vhd->context = lws_get_context(wsi);
      vhd->protocol = lws_get_protocol(wsi);
      vhd->vhost = lws_get_vhost(wsi);
      break;

    case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER: {
      AudioPipe* ap = findPendingConnect(wsi);
      if (ap && ap->hasBasicAuth()) {
        unsigned char **p = (unsigned char**)in, *end = (*p) + len;
        char b[128];
        std::string username, password;

        ap->getBasicAuth(username, password);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
                          "AudioPipe::lws_service_thread "
                          "LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER "
                          "username: %s, password: xxxxxx\n",
                          username.c_str());
        if (dch_lws_http_basic_auth_gen(username.c_str(), password.c_str(), b,
                                        sizeof(b)))
          break;
        if (lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_AUTHORIZATION,
                                         (unsigned char*)b, strlen(b), p, end))
          return -1;
      }
    } break;

    case LWS_CALLBACK_WS_CLIENT_DROP_PROTOCOL:
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
                        "AudioPipe::lws_service_thread "
                        "LWS_CALLBACK_WS_CLIENT_DROP_PROTOCOL\n");
      break;

    case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
      processPendingConnects(vhd);
      processPendingDisconnects(vhd);
      processPendingWrites();
      break;
    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR: {
      AudioPipe* ap = findAndRemovePendingConnect(wsi);
      int rc = lws_http_client_http_response(wsi);
      switch_log_printf(
          SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
          "AudioPipe::lws_service_thread LWS_CALLBACK_CLIENT_CONNECTION_ERROR: "
          "%s, response status %d\n",
          in ? (char*)in : "(null)", rc);
      if (ap) {
        ap->m_state = LWS_CLIENT_FAILED;
        ap->m_callback(ap->m_uuid.c_str(), ap->m_bugname.c_str(),
                       AudioPipe::CONNECT_FAIL, (char*)in, NULL, len);
        delete ap;  // BUG-05 fix: free AudioPipe on connection failure to
                    // prevent memory leak
      } else {
        switch_log_printf(
            SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
            "AudioPipe::lws_service_thread "
            "LWS_CALLBACK_CLIENT_CONNECTION_ERROR unable to find wsi %p..\n",
            wsi);
      }
    } break;

    case LWS_CALLBACK_CLIENT_ESTABLISHED: {
      AudioPipe* ap = findAndRemovePendingConnect(wsi);
      if (ap) {
        *ppAp = ap;
        ap->m_vhd = vhd;
        // BUG-35 fix: only set CONNECTED if state is still CONNECTING
        // (close() may have already set DISCONNECTING)
        LwsState_t expected = LWS_CLIENT_CONNECTING;
        if (ap->m_state.compare_exchange_strong(expected,
                                                LWS_CLIENT_CONNECTED)) {
          ap->m_callback(ap->m_uuid.c_str(), ap->m_bugname.c_str(),
                         AudioPipe::CONNECT_SUCCESS, NULL, NULL, len);
        } else {
          // close() was called while connecting - close the connection
          // immediately
          lws_callback_on_writable(wsi);
        }
      } else {
        switch_log_printf(
            SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
            "AudioPipe::lws_service_thread LWS_CALLBACK_CLIENT_ESTABLISHED "
            "unable to find wsi %p..\n",
            wsi);  // BUG-01 fix: ap is NULL here
      }
    } break;
    case LWS_CALLBACK_CLIENT_CLOSED: {
      AudioPipe* ap = *ppAp;
      if (!ap) {
        switch_log_printf(
            SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
            "AudioPipe::lws_service_thread LWS_CALLBACK_CLIENT_CLOSED unable "
            "to find wsi %p..\n",
            wsi);  // BUG-01 fix: ap is NULL here
        return 0;
      }
      if (ap->m_state == LWS_CLIENT_DISCONNECTING) {
        // closed by us
        ap->m_callback(ap->m_uuid.c_str(), ap->m_bugname.c_str(),
                       AudioPipe::CONNECTION_CLOSED_GRACEFULLY, NULL, NULL,
                       len);
      } else if (ap->m_state == LWS_CLIENT_CONNECTED) {
        // closed by far end
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
                          "%s socket closed by far end\n", ap->m_uuid.c_str());
        ap->m_callback(ap->m_uuid.c_str(), ap->m_bugname.c_str(),
                       AudioPipe::CONNECTION_DROPPED, NULL, NULL, len);
      }
      ap->m_state = LWS_CLIENT_DISCONNECTED;

      // NB: after receiving any of the events above, any holder of a
      // pointer or reference to this object must treat is as no longer valid

      *ppAp = NULL;
      delete ap;
    } break;

    case LWS_CALLBACK_CLIENT_RECEIVE: {
      AudioPipe* ap = *ppAp;
      if (!ap) {
        switch_log_printf(
            SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
            "AudioPipe::lws_service_thread LWS_CALLBACK_CLIENT_RECEIVE unable "
            "to find wsi %p..\n",
            wsi);  // BUG-01 fix: ap is NULL here
        return 0;
      }

      if (ap->m_state == LWS_CLIENT_DISCONNECTING) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
                          "AudioPipe::lws_service_thread race condition: got "
                          "incoming message while closing the connection.\n");
        return 0;
      }

      if (lws_frame_is_binary(wsi)) {
        if (len > 0 && ap->is_bidirectional_audio_stream()) {
          ap->m_callback(ap->m_uuid.c_str(), ap->m_bugname.c_str(),
                         AudioPipe::BINARY, NULL, (char*)in, len);
        } else if (len > 0) {
          switch_log_printf(
              SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
              "AudioPipe::lws_service_thread LWS_CALLBACK_CLIENT_RECEIVE "
              "received unexpected binary frame, discarding.\n");
        } else {
          switch_log_printf(
              SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
              "AudioPipe::lws_service_thread LWS_CALLBACK_CLIENT_RECEIVE "
              "received zero length binary frame, discarding.\n");
        }
      } else {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                          "AudioPipe::lws_service_thread received text frame, "
                          "ignoring (JSON signaling removed)\n");
      }
    } break;

    case LWS_CALLBACK_CLIENT_WRITEABLE: {
      AudioPipe* ap = *ppAp;
      if (!ap) {
        switch_log_printf(
            SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
            "AudioPipe::lws_service_thread LWS_CALLBACK_CLIENT_WRITEABLE "
            "unable to find wsi %p..\n",
            wsi);  // BUG-01 fix: ap is NULL here
        return 0;
      }

      // check for graceful close - send a zero length binary frame
      if (ap->isGracefulShutdown()) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                          "%s graceful shutdown - sending zero length binary "
                          "frame to flush any final responses\n",
                          ap->m_uuid.c_str());
        std::lock_guard<std::mutex> lk(ap->m_audio_mutex);
        lws_write(wsi, (unsigned char*)ap->m_audio_buffer + LWS_PRE, 0,
                  LWS_WRITE_BINARY);
        return 0;
      }

      if (ap->m_state == LWS_CLIENT_DISCONNECTING) {
        lws_close_reason(wsi, LWS_CLOSE_STATUS_NORMAL, NULL, 0);
        return -1;
      }

      // check for audio packets
      {
        std::lock_guard<std::mutex> lk(ap->m_audio_mutex);
        if (ap->m_audio_buffer_write_offset > LWS_PRE) {
          size_t datalen = ap->m_audio_buffer_write_offset - LWS_PRE;
          int sent =
              lws_write(wsi, (unsigned char*)ap->m_audio_buffer + LWS_PRE,
                        datalen, LWS_WRITE_BINARY);
          if (sent < (int)datalen) {
            switch_log_printf(
                SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
                "AudioPipe::lws_service_thread LWS_CALLBACK_CLIENT_WRITEABLE "
                "%s attemped to send %lu only sent %d wsi %p..\n",
                ap->m_uuid.c_str(), datalen, sent, wsi);
          }
          ap->m_audio_buffer_write_offset = LWS_PRE;
        }
      }

      return 0;
    } break;

    default:
      break;
  }
  return lws_callback_http_dummy(wsi, reason, user, in, len);
}

// static members
static const lws_retry_bo_t retry = {
    nullptr,     // retry_ms_table
    0,           // retry_ms_table_count
    0,           // conceal_count
    UINT16_MAX,  // secs_since_valid_ping
    UINT16_MAX,  // secs_since_valid_hangup
    0            // jitter_percent
};

struct lws_context* AudioPipe::context = nullptr;
std::thread AudioPipe::serviceThread;
std::string AudioPipe::protocolName;
std::mutex AudioPipe::mutex_connects;
std::mutex AudioPipe::mutex_disconnects;
std::mutex AudioPipe::mutex_writes;
std::list<AudioPipe*> AudioPipe::pendingConnects;
std::list<AudioPipe*> AudioPipe::pendingDisconnects;
std::list<AudioPipe*> AudioPipe::pendingWrites;
AudioPipe::log_emit_function AudioPipe::logger;
std::mutex AudioPipe::mapMutex;
bool AudioPipe::stopFlag;

void AudioPipe::processPendingConnects(lws_per_vhost_data* vhd) {
  std::list<AudioPipe*> connects;
  {
    std::lock_guard<std::mutex> guard(mutex_connects);
    for (auto it = pendingConnects.begin(); it != pendingConnects.end(); ++it) {
      if ((*it)->m_state == LWS_CLIENT_IDLE) {
        connects.push_back(*it);
        (*it)->m_state = LWS_CLIENT_CONNECTING;
      }
    }
  }
  for (auto it = connects.begin(); it != connects.end(); ++it) {
    AudioPipe* ap = *it;
    ap->connect_client(vhd);
  }
}

void AudioPipe::processPendingDisconnects(lws_per_vhost_data* /*vhd*/) {
  std::list<AudioPipe*> disconnects;
  {
    std::lock_guard<std::mutex> guard(mutex_disconnects);
    for (auto it = pendingDisconnects.begin(); it != pendingDisconnects.end();
         ++it) {
      if ((*it)->m_state == LWS_CLIENT_DISCONNECTING)
        disconnects.push_back(*it);
    }
    pendingDisconnects.clear();
  }
  for (auto it = disconnects.begin(); it != disconnects.end(); ++it) {
    AudioPipe* ap = *it;
    lws_callback_on_writable(ap->m_wsi);
  }
}

void AudioPipe::processPendingWrites() {
  std::list<AudioPipe*> writes;
  {
    std::lock_guard<std::mutex> guard(mutex_writes);
    for (auto it = pendingWrites.begin(); it != pendingWrites.end(); ++it) {
      if ((*it)->m_state == LWS_CLIENT_CONNECTED) writes.push_back(*it);
    }
    pendingWrites.clear();
  }
  for (auto it = writes.begin(); it != writes.end(); ++it) {
    AudioPipe* ap = *it;
    lws_callback_on_writable(ap->m_wsi);
  }
}

AudioPipe* AudioPipe::findAndRemovePendingConnect(struct lws* wsi) {
  AudioPipe* ap = NULL;
  std::lock_guard<std::mutex> guard(mutex_connects);
  std::list<AudioPipe*> toRemove;

  for (auto it = pendingConnects.begin(); it != pendingConnects.end() && !ap;
       ++it) {
    int state = (*it)->m_state;

    if ((*it)->m_wsi == nullptr) toRemove.push_back(*it);

    if ((state == LWS_CLIENT_CONNECTING) && (*it)->m_wsi == wsi) ap = *it;
  }

  for (auto it = toRemove.begin(); it != toRemove.end(); ++it)
    pendingConnects.remove(*it);

  if (ap) {
    pendingConnects.remove(ap);
  }

  return ap;
}

AudioPipe* AudioPipe::findPendingConnect(struct lws* wsi) {
  AudioPipe* ap = NULL;
  std::lock_guard<std::mutex> guard(mutex_connects);

  for (auto it = pendingConnects.begin(); it != pendingConnects.end() && !ap;
       ++it) {
    int state = (*it)->m_state;
    if ((state == LWS_CLIENT_CONNECTING) && (*it)->m_wsi == wsi) ap = *it;
  }
  return ap;
}

void AudioPipe::removePendingConnect(AudioPipe* ap) {
  std::lock_guard<std::mutex> guard(mutex_connects);
  pendingConnects.remove(ap);
}

void AudioPipe::addPendingConnect(AudioPipe* ap) {
  {
    std::lock_guard<std::mutex> guard(mutex_connects);
    pendingConnects.push_back(ap);
    switch_log_printf(
        SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
        "%s after adding connect there are %lu pending connects\n",
        ap->m_uuid.c_str(), pendingConnects.size());
  }
  lws_cancel_service(context);
}
void AudioPipe::addPendingDisconnect(AudioPipe* ap) {
  // BUG-20 fix: move m_state write inside the lock to prevent data race with
  // LWS thread reads
  {
    std::lock_guard<std::mutex> guard(mutex_disconnects);
    ap->m_state = LWS_CLIENT_DISCONNECTING;
    pendingDisconnects.push_back(ap);
    switch_log_printf(
        SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
        "%s after adding disconnect there are %lu pending disconnects\n",
        ap->m_uuid.c_str(), pendingDisconnects.size());
  }
  lws_cancel_service(context);  // BUG-21 fix: use global context instead of
                                // ap->m_vhd->context which may be null
}
void AudioPipe::addPendingWrite(AudioPipe* ap) {
  {
    std::lock_guard<std::mutex> guard(mutex_writes);
    pendingWrites.push_back(ap);
  }
  lws_cancel_service(context);  // BUG-21 fix: use global context instead of
                                // ap->m_vhd->context which may be null
}

bool AudioPipe::lws_service_thread() {
  struct lws_context_creation_info info;

  const struct lws_protocols protocols[] = {
      {protocolName.c_str(), AudioPipe::lws_callback, sizeof(void*), 1024, 0,
       nullptr, 0},
      {NULL, NULL, 0, 0, 0, nullptr, 0}};

  memset(&info, 0, sizeof info);
  info.port = CONTEXT_PORT_NO_LISTEN;
  info.protocols = protocols;
  info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;

  info.ka_time = nTcpKeepaliveSecs;  // tcp keep-alive timer
  info.ka_probes = 4;    // number of times to try ka before closing connection
  info.ka_interval = 5;  // time between ka's
  info.timeout_secs = 10;  // doc says timeout for "various processes involving
                           // network roundtrips"
  info.keepalive_timeout = 5;  // seconds to allow remote client to hold on to
                               // an idle HTTP/1.1 connection
  info.timeout_secs_ah_idle =
      10;  // secs to allow a client to hold an ah without using it
  info.retry_and_idle_policy = &retry;

  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
                    "AudioPipe::lws_service_thread creating context\n");

  context = lws_create_context(&info);
  if (!context) {
    switch_log_printf(
        SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
        "AudioPipe::lws_service_thread failed creating context\n");
    return false;
  }

  int n;
  do {
    n = lws_service(context, 0);
  } while (n >= 0 && !stopFlag);

  lwsl_notice("AudioPipe::lws_service_thread ending\n");
  lws_context_destroy(context);

  return true;
}

void AudioPipe::initialize(const char* protocol, int loglevel,
                           log_emit_function logger) {
  protocolName = protocol;
  lws_set_log_level(loglevel, logger);

  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
                    "AudioPipe::initialize starting\n");
  std::lock_guard<std::mutex> lock(mapMutex);
  stopFlag = false;
  serviceThread = std::thread(&AudioPipe::lws_service_thread);
}

bool AudioPipe::deinitialize() {
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
                    "AudioPipe::deinitialize\n");
  std::lock_guard<std::mutex> lock(mapMutex);
  stopFlag = true;
  if (serviceThread.joinable()) {
    serviceThread.join();
  }
  return true;
}

// instance members
AudioPipe::AudioPipe(const char* uuid, const char* host, unsigned int port,
                     const char* path, int sslFlags, size_t bufLen,
                     size_t minFreespace, const char* username,
                     const char* password, char* bugname,
                     int bidirectional_audio_stream, notifyHandler_t callback)
    : m_state(LWS_CLIENT_IDLE),
      m_uuid(uuid),
      m_host(host),
      m_bugname(bugname),
      m_port(port),
      m_path(path),
      m_sslFlags(sslFlags),
      m_wsi(nullptr),
      m_audio_buffer_max_len(bufLen),
      m_audio_buffer_write_offset(LWS_PRE),
      m_audio_buffer_min_freespace(minFreespace),
      m_vhd(nullptr),
      m_callback(callback),
      m_gracefulShutdown(false) {
  if (username && password) {
    m_username.assign(username);
    m_password.assign(password);
  }
  m_bidirectional_audio_stream = bidirectional_audio_stream;
  m_audio_buffer = new uint8_t[m_audio_buffer_max_len];
}
AudioPipe::~AudioPipe() {
  if (m_audio_buffer) delete[] m_audio_buffer;
}

void AudioPipe::connect(void) { addPendingConnect(this); }

bool AudioPipe::connect_client(struct lws_per_vhost_data* vhd) {
  // BUG-16 fix: replace assert with runtime check to protect production builds
  if (!m_audio_buffer) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                      "connect_client: m_audio_buffer is null\n");
    return false;
  }
  if (m_vhd != nullptr) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                      "connect_client: m_vhd is already set\n");
    return false;
  }

  struct lws_client_connect_info i;

  memset(&i, 0, sizeof(i));
  i.context = vhd->context;
  i.port = m_port;
  i.address = m_host.c_str();
  i.path = m_path.c_str();
  i.host = i.address;
  i.origin = i.address;
  i.ssl_connection = m_sslFlags;
  i.protocol = protocolName.c_str();
  i.pwsi = &(m_wsi);

  m_vhd = vhd;

  m_wsi = lws_client_connect_via_info(&i);
  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
                    "%s attempting connection, wsi is %p\n", m_uuid.c_str(),
                    m_wsi);

  return nullptr != m_wsi;
}

void AudioPipe::unlockAudioBuffer() {
  if (m_audio_buffer_write_offset > LWS_PRE) addPendingWrite(this);
  m_audio_mutex.unlock();
}

bool AudioPipe::close() {
  LwsState_t state = m_state.load();
  if (state == LWS_CLIENT_CONNECTED || state == LWS_CLIENT_CONNECTING) {
    // LWS thread will handle cleanup after close
    addPendingDisconnect(this);
    return true;
  }
  if (state == LWS_CLIENT_IDLE) {
    // Connection not yet attempted - remove from pending list
    removePendingConnect(this);
    m_state = LWS_CLIENT_FAILED;
    return false;
  }
  return false;
}

void AudioPipe::do_graceful_shutdown() {
  m_gracefulShutdown = true;
  addPendingWrite(this);
}
