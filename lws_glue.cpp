#include <switch.h>
#include <string.h>
#include <string>
#include <mutex>
#include <thread>
#include <algorithm>
#include <cstdlib>
#include <new>
#include <sstream>
#include <regex>

#include "mod_audio_fork.h"
#include "audio_pipe.hpp"
#include "vector_math.h"

#include <boost/circular_buffer.hpp>


typedef boost::circular_buffer<uint16_t> CircularBuffer_t;

#define RTP_PACKETIZATION_PERIOD 20
#define FRAME_SIZE_8000  320 /*which means each 20ms frame as 320 bytes at 8 khz (1 channel only)*/
#define BUFFER_GROW_SIZE (16384)

namespace {
  static const char *requestedBufferSecs = std::getenv("MOD_AUDIO_FORK_BUFFER_SECS");
  static int nAudioBufferSecs = std::max(1, std::min(requestedBufferSecs ? ::atoi(requestedBufferSecs) : 2, 5));
  static const char *requestedNumServiceThreads = std::getenv("MOD_AUDIO_FORK_SERVICE_THREADS");
  static const char* mySubProtocolName = std::getenv("MOD_AUDIO_FORK_SUBPROTOCOL_NAME") ?
    std::getenv("MOD_AUDIO_FORK_SUBPROTOCOL_NAME") : "audio.drachtio.org";
  static unsigned int nServiceThreads __attribute__((unused)) = std::max(1, std::min(requestedNumServiceThreads ? ::atoi(requestedNumServiceThreads) : 1, 5));
  static unsigned int idxCallCount = 0;

  switch_status_t processIncomingBinary(private_t* tech_pvt, switch_core_session_t* /*session*/, const char* message, size_t dataLength) {
    std::vector<uint8_t> data;

    // Prepend the set-aside byte if there is one
    if (tech_pvt->has_set_aside_byte) {
        data.push_back(tech_pvt->set_aside_byte);
        tech_pvt->has_set_aside_byte = false;
    }

    // Append the new incoming message
    data.insert(data.end(), message, message + dataLength);

    // Check if the total data length is now odd
    if (data.size() % 2 != 0) {
        // Set aside the last byte
        tech_pvt->set_aside_byte = data.back();
        tech_pvt->has_set_aside_byte = true;
        data.pop_back(); // Remove the last byte from the data vector
    }

    // Convert the data to 16-bit elements
    const uint16_t* data_uint16 = reinterpret_cast<const uint16_t*>(data.data());
    size_t numSamples = data.size() / sizeof(uint16_t);

    // Access the prebuffer
    CircularBuffer_t* cBuffer = static_cast<CircularBuffer_t*>(tech_pvt->streamingPreBuffer);

    // Ensure the prebuffer has enough capacity
    if (cBuffer->capacity() - cBuffer->size() < numSamples) {
        size_t newCapacity = cBuffer->size() + std::max(numSamples, (size_t)BUFFER_GROW_SIZE);
        cBuffer->set_capacity(newCapacity);
    }

    // Append the data to the prebuffer
    cBuffer->insert(cBuffer->end(), data_uint16, data_uint16 + numSamples);
    //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Appended %zu 16-bit samples to the prebuffer.\n", numSamples);

    // if we haven't reached threshold amount of prebuffered data, return
    if ((int)cBuffer->size() < tech_pvt->streamingPreBufSize) {
        //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Prebuffered data is below threshold %u, returning.\n", tech_pvt->streamingPreBufSize);
        return SWITCH_STATUS_SUCCESS;
    }

    //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Prebuffered data samples %u is above threshold %u, prepare to playout.\n", cBuffer->size(), tech_pvt->streamingPreBufSize);

    // tech_pvt->streamingPreBufSize = 320 * tech_pvt->downscale_factor * 2; // Removed dynamic resizing

    // Check for downsampling factor
    size_t downsample_factor = tech_pvt->downscale_factor;

    // Calculate the number of samples that can be evenly divided by the downsample factor
    size_t numCompleteSamples = (cBuffer->size() / downsample_factor) * downsample_factor;

    // Handle leftover samples
    std::vector<uint16_t> leftoverSamples;
    size_t numLeftoverSamples = cBuffer->size() - numCompleteSamples;
    if (numLeftoverSamples > 0) {
        leftoverSamples.assign(cBuffer->end() - numLeftoverSamples, cBuffer->end());
        cBuffer->resize(numCompleteSamples);
        //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Temporarily removing %u leftover samples due to downsampling.\n", numLeftoverSamples);
    }

    // resample if necessary
    std::vector<int16_t> out;
    try {
      if (tech_pvt->bidirectional_audio_resampler) {
        // Improvement: Use assign to convert circular buffer to vector for resampling
        std::vector<int16_t> in;
        in.assign(cBuffer->begin(), cBuffer->end()); 
        out.resize(in.size() * 6); // max upsampling would be from 8k -> 48k

        spx_uint32_t in_len = in.size();
        spx_uint32_t out_len = out.size();

        //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Resampling %u samples into a buffer that can hold %u samples\n", in.size(), out_len);

        speex_resampler_process_interleaved_int(tech_pvt->bidirectional_audio_resampler, in.data(), &in_len, out.data(), &out_len);

        // Resize the output buffer to match the output length from resampler
        //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Resizing output buffer from %u to %u samples\n", in.size(), out_len);

        out.resize(out_len);
      }
      else {
        // If no resampling is needed, copy the data from the prebuffer to the output buffer
        out.assign(cBuffer->begin(), cBuffer->end());
      }
    } catch (const std::exception& e) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error resampling incoming binary message: %s\n", e.what());
      return SWITCH_STATUS_FALSE;
    } catch (...) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error resampling incoming binary message\n");
      return SWITCH_STATUS_FALSE;
    }

    if (nullptr != tech_pvt->mutex && switch_mutex_lock(tech_pvt->mutex) == SWITCH_STATUS_SUCCESS) {
      CircularBuffer_t *playoutBuffer = (CircularBuffer_t *) tech_pvt->streamingPlayoutBuffer;

      try {
        // Resize the buffer if necessary
        if (playoutBuffer->capacity() - playoutBuffer->size() < out.size()) {
          size_t newCapacity = playoutBuffer->size() + std::max(out.size(), (size_t)BUFFER_GROW_SIZE);
          playoutBuffer->set_capacity(newCapacity);
          //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Resized playout buffer to new capacity: %zu\n", newCapacity);
        }
        // Push the data into the buffer.
        playoutBuffer->insert(playoutBuffer->end(), out.begin(), out.end());
        //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Appended %zu 16-bit samples to the playout buffer.\n", out.size());
      } catch (const std::exception& e) {
        switch_mutex_unlock(tech_pvt->mutex);
        cBuffer->clear();
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error processing incoming binary message: %s\n", e.what());
        return SWITCH_STATUS_FALSE;
      } catch (...) {
        switch_mutex_unlock(tech_pvt->mutex);
        cBuffer->clear();
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error processing incoming binary message\n");
        return SWITCH_STATUS_FALSE;
      }
      switch_mutex_unlock(tech_pvt->mutex);
      cBuffer->clear();

      // Put the leftover samples back in the prebuffer for the next time
      if (!leftoverSamples.empty()) {
          cBuffer->insert(cBuffer->end(), leftoverSamples.begin(), leftoverSamples.end());
          //switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Put back %u leftover samples into the prebuffer.\n", leftoverSamples.size());
      }
      return SWITCH_STATUS_SUCCESS;
    }
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Failed to get mutext (temp)\n");

    return SWITCH_STATUS_SUCCESS;
  }

  static void eventCallback(const char* sessionId, const char* bugname, drachtio::AudioPipe::NotifyEvent_t event, const char* message, const char* binary, size_t len) {
    switch_core_session_t* session = switch_core_session_locate(sessionId);
    if (session) {
      switch_channel_t *channel = switch_core_session_get_channel(session);
      switch_media_bug_t *bug = (switch_media_bug_t*) switch_channel_get_private(channel, bugname);
      if (bug) {
        private_t* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
        if (tech_pvt) {
          switch (event) {
            case drachtio::AudioPipe::CONNECT_SUCCESS:
              switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "connection successful\n");
              tech_pvt->responseHandler(session, EVENT_CONNECT_SUCCESS, NULL);
            break;
            case drachtio::AudioPipe::CONNECT_FAIL:
            {
              // first thing: we can no longer access the AudioPipe
              std::stringstream json;
              json << "{\"reason\":\"" << message << "\"}";
              tech_pvt->pAudioPipe = nullptr;
              tech_pvt->responseHandler(session, EVENT_CONNECT_FAIL, (char *) json.str().c_str());
              switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_NOTICE, "connection failed: %s\n", message);
            }
            break;
            case drachtio::AudioPipe::CONNECTION_DROPPED:
              // first thing: we can no longer access the AudioPipe
              tech_pvt->pAudioPipe = nullptr;
              tech_pvt->responseHandler(session, EVENT_DISCONNECT, NULL);
              switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_NOTICE, "connection dropped from far end\n");
            break;
            case drachtio::AudioPipe::CONNECTION_CLOSED_GRACEFULLY:
              // first thing: we can no longer access the AudioPipe
              tech_pvt->pAudioPipe = nullptr;
              switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "connection closed gracefully\n");
            break;
            case drachtio::AudioPipe::BINARY:
            processIncomingBinary(tech_pvt, session, binary, len);
            break;
          }
        }
      }
      switch_core_session_rwunlock(session);
    }
  }
  switch_status_t fork_data_init(private_t *tech_pvt, switch_core_session_t *session, char * host,
    unsigned int port, char* path, int sslFlags, int sampling, int desiredSampling, int channels,
    char *bugname, int bidirectional_audio_enable,
    int bidirectional_audio_stream, int bidirectional_audio_sample_rate, responseHandler_t responseHandler) {

    const char* username = nullptr;
    const char* password = nullptr;
    int err;
    int bidirectional_audio_stream_enable = bidirectional_audio_enable + bidirectional_audio_stream;
    switch_codec_implementation_t read_impl;
    switch_channel_t *channel = switch_core_session_get_channel(session);

    switch_core_session_get_read_impl(session, &read_impl);
  
    if ((username = switch_channel_get_variable(channel, "MOD_AUDIO_BASIC_AUTH_USERNAME"))) {
      password = switch_channel_get_variable(channel, "MOD_AUDIO_BASIC_AUTH_PASSWORD");
    }

    memset(tech_pvt, 0, sizeof(private_t));
  
    strncpy(tech_pvt->sessionId, switch_core_session_get_uuid(session), MAX_SESSION_ID - 1);
    tech_pvt->sessionId[MAX_SESSION_ID - 1] = '\0';
    strncpy(tech_pvt->host, host, MAX_WS_URL_LEN - 1);
    tech_pvt->host[MAX_WS_URL_LEN - 1] = '\0';
    tech_pvt->port = port;
    strncpy(tech_pvt->path, path, MAX_PATH_LEN - 1);
    tech_pvt->path[MAX_PATH_LEN - 1] = '\0';
    tech_pvt->sampling = desiredSampling;
    tech_pvt->responseHandler = responseHandler;
    tech_pvt->channels = channels;
    tech_pvt->id = ++idxCallCount;
    tech_pvt->buffer_overrun_notified = 0;
    tech_pvt->audio_paused = 0;
    tech_pvt->graceful_shutdown = 0;
    tech_pvt->streamingPlayoutBuffer = (void *) new CircularBuffer_t(8192);
    tech_pvt->bidirectional_audio_enable = bidirectional_audio_enable;
    tech_pvt->bidirectional_audio_stream = bidirectional_audio_stream;
    tech_pvt->bidirectional_audio_sample_rate = bidirectional_audio_sample_rate;
    tech_pvt->has_set_aside_byte = 0;
    tech_pvt->downscale_factor = 1;
    tech_pvt->raw_write_codec_initialized = 0;
    tech_pvt->write_ts = 0;
    if (bidirectional_audio_sample_rate > sampling) {
      tech_pvt->downscale_factor = bidirectional_audio_sample_rate / sampling;
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "downscale_factor is %d\n", tech_pvt->downscale_factor);
    }
    tech_pvt->streamingPreBufSize = 320 * tech_pvt->downscale_factor * 6; // min 120ms prebuffer
    tech_pvt->streamingPreBuffer = (void *) new CircularBuffer_t(8192);

    // BUG-12 fix: ensure null termination after strncpy
    strncpy(tech_pvt->bugname, bugname, MAX_BUG_LEN);
    tech_pvt->bugname[MAX_BUG_LEN] = '\0';

    size_t buflen = LWS_PRE + (FRAME_SIZE_8000 * desiredSampling / 8000 * channels * 1000 / RTP_PACKETIZATION_PERIOD * nAudioBufferSecs);

    // BUG-15 fix: use nothrow new so that allocation failure returns nullptr instead of throwing
    drachtio::AudioPipe* ap = new (std::nothrow) drachtio::AudioPipe(tech_pvt->sessionId, host, port, path, sslFlags, 
      buflen, read_impl.decoded_bytes_per_packet, username, password, bugname, bidirectional_audio_stream_enable, eventCallback);
    if (!ap) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error allocating AudioPipe\n");
      return SWITCH_STATUS_FALSE;
    }

    tech_pvt->pAudioPipe = static_cast<void *>(ap);

    if (switch_mutex_init(&tech_pvt->mutex, SWITCH_MUTEX_NESTED, switch_core_session_get_pool(session)) != SWITCH_STATUS_SUCCESS) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error initializing mutex\n");
      return SWITCH_STATUS_FALSE;
    }

    if (desiredSampling != sampling) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%u) resampling from %u to %u\n", tech_pvt->id, sampling, desiredSampling);
      tech_pvt->resampler = speex_resampler_init(channels, sampling, desiredSampling, SWITCH_RESAMPLE_QUALITY, &err);
      if (0 != err) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error initializing resampler: %s.\n", speex_resampler_strerror(err));
        return SWITCH_STATUS_FALSE;
      }
    }
    else {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%u) no resampling needed for this call\n", tech_pvt->id);
    }

    if (bidirectional_audio_sample_rate && sampling != bidirectional_audio_sample_rate) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%u) bidirectional audio resampling from %u to %u, channels %d\n", tech_pvt->id, bidirectional_audio_sample_rate, sampling, channels);
      tech_pvt->bidirectional_audio_resampler = speex_resampler_init(1, bidirectional_audio_sample_rate, sampling, SWITCH_RESAMPLE_QUALITY, &err);
      if (0 != err) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error initializing bidirectional audio resampler: %s.\n", speex_resampler_strerror(err));
        return SWITCH_STATUS_FALSE;
      }
    }

    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%u) fork_data_init\n", tech_pvt->id);

    return SWITCH_STATUS_SUCCESS;
  }

  void destroy_tech_pvt(private_t* tech_pvt) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "%s (%u) destroy_tech_pvt\n", tech_pvt->sessionId, tech_pvt->id);
    if (tech_pvt->pAudioPipe) {
      drachtio::AudioPipe *pAudioPipe = static_cast<drachtio::AudioPipe *>(tech_pvt->pAudioPipe);
      delete pAudioPipe;
      tech_pvt->pAudioPipe = nullptr;
    }
    if (tech_pvt->resampler) {
      speex_resampler_destroy(tech_pvt->resampler);
      tech_pvt->resampler = nullptr;
    }
    if (tech_pvt->bidirectional_audio_resampler) {
      speex_resampler_destroy(tech_pvt->bidirectional_audio_resampler);
      tech_pvt->bidirectional_audio_resampler = nullptr;
    }
    if (tech_pvt->raw_write_codec_initialized) {
      switch_core_codec_destroy(&tech_pvt->raw_write_codec);
      tech_pvt->raw_write_codec_initialized = 0;
    }
    if (tech_pvt->mutex) {
      switch_mutex_destroy(tech_pvt->mutex);
      tech_pvt->mutex = nullptr;
    }
    if (tech_pvt->streamingPlayoutBuffer) {
      CircularBuffer_t *cBuffer = (CircularBuffer_t *) tech_pvt->streamingPlayoutBuffer;
      delete cBuffer;
      tech_pvt->streamingPlayoutBuffer = nullptr;
    }
    if (tech_pvt->streamingPreBuffer) {
      CircularBuffer_t *cBuffer = (CircularBuffer_t *) tech_pvt->streamingPreBuffer;
      delete cBuffer;
      tech_pvt->streamingPreBuffer = nullptr;
    }
  }

  void lws_logger(int level, const char *line) {
    switch_log_level_t llevel = SWITCH_LOG_DEBUG;

    switch (level) {
      case LLL_ERR: llevel = SWITCH_LOG_ERROR; break;
      case LLL_WARN: llevel = SWITCH_LOG_WARNING; break;
      case LLL_NOTICE: llevel = SWITCH_LOG_NOTICE; break;
      case LLL_INFO: llevel = SWITCH_LOG_INFO; break;
      break;
    }
	  switch_log_printf(SWITCH_CHANNEL_LOG, llevel, "%s\n", line);
  }
}

extern "C" {
  int parse_ws_uri(switch_channel_t *channel, const char* szServerUri, char* host, char *path, unsigned int* pPort, int* pSslFlags) {
    int offset;
    char server[MAX_WS_URL_LEN + MAX_PATH_LEN];
    int flags = LCCSCF_USE_SSL;
    
    if (switch_true(switch_channel_get_variable(channel, "MOD_AUDIO_FORK_ALLOW_SELFSIGNED"))) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "parse_ws_uri - allowing self-signed certs\n");
      flags |= LCCSCF_ALLOW_SELFSIGNED;
    }
    if (switch_true(switch_channel_get_variable(channel, "MOD_AUDIO_FORK_SKIP_SERVER_CERT_HOSTNAME_CHECK"))) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "parse_ws_uri - skipping hostname check\n");
      flags |= LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK;
    }
    if (switch_true(switch_channel_get_variable(channel, "MOD_AUDIO_FORK_ALLOW_EXPIRED"))) {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "parse_ws_uri - allowing expired certs\n");
      flags |= LCCSCF_ALLOW_EXPIRED;
    }

    // get the scheme
    strncpy(server, szServerUri, MAX_WS_URL_LEN + MAX_PATH_LEN - 1);
    server[MAX_WS_URL_LEN + MAX_PATH_LEN - 1] = '\0';
    if (0 == strncmp(server, "https://", 8) || 0 == strncmp(server, "HTTPS://", 8)) {
      *pSslFlags = flags;
      offset = 8;
      *pPort = 443;
    }
    else if (0 == strncmp(server, "wss://", 6) || 0 == strncmp(server, "WSS://", 6)) {
      *pSslFlags = flags;
      offset = 6;
      *pPort = 443;
    }
    else if (0 == strncmp(server, "http://", 7) || 0 == strncmp(server, "HTTP://", 7)) {
      offset = 7;
      *pSslFlags = 0;
      *pPort = 80;
    }
    else if (0 == strncmp(server, "ws://", 5) || 0 == strncmp(server, "WS://", 5)) {
      offset = 5;
      *pSslFlags = 0;
      *pPort = 80;
    }
    else {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "parse_ws_uri - error parsing uri %s: invalid scheme\n", szServerUri);;
      return 0;
    }

    std::string strHost(server + offset);
    //- `([^/:]+)` captures the hostname/IP address, match any character except in the set
    //- `:?([0-9]*)?` optionally captures a colon and the port number, if it's present.
    //- `(/.*)` captures everything else (the path).
    std::regex re("([^/:]+):?([0-9]*)?(/.*)?$");
    std::smatch matches;
    if(std::regex_search(strHost, matches, re)) {
      /*
      for (int i = 0; i < matches.length(); i++) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "parse_ws_uri - %d: %s\n", i, matches[i].str().c_str());
      }
      */
      // BUG-12 fix: use MAX_WS_URL_LEN - 1 and ensure null termination
      strncpy(host, matches[1].str().c_str(), MAX_WS_URL_LEN - 1);
      host[MAX_WS_URL_LEN - 1] = '\0';
      if (matches[2].str().length() > 0) {
        *pPort = atoi(matches[2].str().c_str());
      }
      if (matches[3].str().length() > 0) {
        // BUG-12 fix: use MAX_PATH_LEN - 1 and ensure null termination
        strncpy(path, matches[3].str().c_str(), MAX_PATH_LEN - 1);
        path[MAX_PATH_LEN - 1] = '\0';
      }
      else {
        strcpy(path, "/");
      }
    } else {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "parse_ws_uri - invalid format %s\n", strHost.c_str());
      return 0;
    }
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "parse_ws_uri - host %s, path %s\n", host, path);

    return 1;
  }

  switch_status_t fork_init() {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_audio_fork: audio buffer (in secs):    %d secs\n", nAudioBufferSecs);
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_audio_fork: sub-protocol:              %s\n", mySubProtocolName);
 
    //int logs = LLL_ERR | LLL_WARN | LLL_NOTICE | LLL_INFO | LLL_PARSER | LLL_HEADER | LLL_EXT | LLL_CLIENT  | LLL_LATENCY | LLL_DEBUG ;
    int logs = LLL_ERR | LLL_WARN | LLL_NOTICE;
    drachtio::AudioPipe::initialize(mySubProtocolName, logs, lws_logger);
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_audio_fork successfully initialized\n");
    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t fork_cleanup() {
    bool cleanup = false;
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_audio_fork unloading..\n");

    cleanup = drachtio::AudioPipe::deinitialize();
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_audio_fork unloaded status %d\n", cleanup);
    if (cleanup == true) {
        return SWITCH_STATUS_SUCCESS;
    }
    return SWITCH_STATUS_FALSE;
  }

  switch_status_t fork_session_init(switch_core_session_t *session,
    responseHandler_t responseHandler,
    uint32_t samples_per_second,
    char *host,
    unsigned int port,
    char *path,
    int sampling,
    int sslFlags,
    int channels,
    char *bugname,
    int bidirectional_audio_enable,
    int bidirectional_audio_stream,
    int bidirectional_audio_sample_rate,
    void **ppUserData
    )
  {
    // allocate per-session data structure
    private_t* tech_pvt = (private_t *) switch_core_session_alloc(session, sizeof(private_t));
    if (!tech_pvt) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "error allocating memory!\n");
      return SWITCH_STATUS_FALSE;
    }

    if (SWITCH_STATUS_SUCCESS != fork_data_init(tech_pvt, session, host, port, path, sslFlags, samples_per_second, sampling, channels,
      bugname, bidirectional_audio_enable, bidirectional_audio_stream, bidirectional_audio_sample_rate, responseHandler)) {
      destroy_tech_pvt(tech_pvt);
      return SWITCH_STATUS_FALSE;
    }

    *ppUserData = tech_pvt;
    return SWITCH_STATUS_SUCCESS;
  }

   switch_status_t fork_session_connect(void **ppUserData) {
    private_t *tech_pvt = static_cast<private_t *>(*ppUserData);
    drachtio::AudioPipe *pAudioPipe = static_cast<drachtio::AudioPipe*>(tech_pvt->pAudioPipe);
    pAudioPipe->connect();
    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t fork_session_cleanup(switch_core_session_t *session, char *bugname, int channelIsClosing) {
    switch_channel_t *channel = switch_core_session_get_channel(session);
    switch_media_bug_t *bug = (switch_media_bug_t*) switch_channel_get_private(channel, bugname);
    if (!bug) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "fork_session_cleanup: no bug %s - websocket conection already closed\n", bugname);
      return SWITCH_STATUS_FALSE;
    }
    private_t* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);

    // BUG-02 fix: check tech_pvt before accessing its members
    if (!tech_pvt) return SWITCH_STATUS_FALSE;
    uint32_t id = tech_pvt->id;

    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%u) fork_session_cleanup\n", id);

    // BUG-09 fix: read pAudioPipe after acquiring the mutex to prevent TOCTOU race
    switch_mutex_lock(tech_pvt->mutex);
    drachtio::AudioPipe *pAudioPipe = static_cast<drachtio::AudioPipe *>(tech_pvt->pAudioPipe);

    // get the bug again, now that we are under lock
    {
      switch_media_bug_t *bug = (switch_media_bug_t*) switch_channel_get_private(channel, bugname);
      if (bug) {
        switch_channel_set_private(channel, bugname, NULL);
        if (!channelIsClosing) {
          switch_core_media_bug_remove(session, &bug);
        }
      }
    }

    // delete any temp files
    if (pAudioPipe) {
      // BUG-35 fix: close() returns true when LWS thread will handle AudioPipe cleanup
      // (CONNECTED/CONNECTING states), false when caller should delete (IDLE state)
      bool lwsCleanup = pAudioPipe->close();
      if (lwsCleanup) {
        tech_pvt->pAudioPipe = nullptr;
      }
    }

    switch_mutex_unlock(tech_pvt->mutex);
    destroy_tech_pvt(tech_pvt);
    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "(%u) fork_session_cleanup: connection closed\n", id);
    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t fork_session_pauseresume(switch_core_session_t *session, char *bugname, int pause) {
    switch_channel_t *channel = switch_core_session_get_channel(session);
    switch_media_bug_t *bug = (switch_media_bug_t*) switch_channel_get_private(channel, bugname);
    if (!bug) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "fork_session_pauseresume failed because no bug\n");
      return SWITCH_STATUS_FALSE;
    }
    private_t* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
  
    if (!tech_pvt) return SWITCH_STATUS_FALSE;

    switch_core_media_bug_flush(bug);
    tech_pvt->audio_paused = pause;
    return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t fork_session_graceful_shutdown(switch_core_session_t *session, char *bugname) {
    switch_channel_t *channel = switch_core_session_get_channel(session);
    switch_media_bug_t *bug = (switch_media_bug_t*) switch_channel_get_private(channel, bugname);
    if (!bug) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "fork_session_graceful_shutdown failed because no bug\n");
      return SWITCH_STATUS_FALSE;
    }
    private_t* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
  
    if (!tech_pvt) return SWITCH_STATUS_FALSE;

    tech_pvt->graceful_shutdown = 1;

    drachtio::AudioPipe *pAudioPipe = static_cast<drachtio::AudioPipe *>(tech_pvt->pAudioPipe);
    if (pAudioPipe) pAudioPipe->do_graceful_shutdown();

    return SWITCH_STATUS_SUCCESS;
  }

  switch_bool_t fork_frame(switch_core_session_t *session, switch_media_bug_t *bug) {
    private_t* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);

    if (!tech_pvt || tech_pvt->audio_paused || tech_pvt->graceful_shutdown) return SWITCH_TRUE;
    
    if (switch_mutex_trylock(tech_pvt->mutex) == SWITCH_STATUS_SUCCESS) {
      if (!tech_pvt->pAudioPipe) {
        switch_mutex_unlock(tech_pvt->mutex);
        return SWITCH_TRUE;
      }
      drachtio::AudioPipe *pAudioPipe = static_cast<drachtio::AudioPipe *>(tech_pvt->pAudioPipe);
      if (pAudioPipe->getLwsState() != drachtio::AudioPipe::LWS_CLIENT_CONNECTED) {
        switch_mutex_unlock(tech_pvt->mutex);
        return SWITCH_TRUE;
      }

      pAudioPipe->lockAudioBuffer();
      size_t available = pAudioPipe->binarySpaceAvailable();
      if (NULL == tech_pvt->resampler) {
        switch_frame_t frame;
        memset(&frame, 0, sizeof(frame));
        frame.data = pAudioPipe->binaryWritePtr();
        frame.buflen = available;
        while (true) {

          // check if buffer would be overwritten; dump packets if so
          if (available < pAudioPipe->binaryMinSpace()) {
            if (!tech_pvt->buffer_overrun_notified) {
              tech_pvt->buffer_overrun_notified = 1;
              tech_pvt->responseHandler(session, EVENT_BUFFER_OVERRUN, NULL);
            }
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "(%u) dropping packets!\n", 
              tech_pvt->id);
            pAudioPipe->binaryWritePtrResetToZero();

            frame.data = pAudioPipe->binaryWritePtr();
            frame.buflen = available = pAudioPipe->binarySpaceAvailable();
          }

          switch_status_t rv = switch_core_media_bug_read(bug, &frame, SWITCH_TRUE);
          if (rv != SWITCH_STATUS_SUCCESS) break;
          if (frame.datalen) {
            pAudioPipe->binaryWritePtrAdd(frame.datalen);
            frame.buflen = available = pAudioPipe->binarySpaceAvailable();
            frame.data = pAudioPipe->binaryWritePtr();
          }
        }
      }
      else {
        uint8_t data[SWITCH_RECOMMENDED_BUFFER_SIZE];
        switch_frame_t frame;
        memset(&frame, 0, sizeof(frame));
        frame.data = data;
        frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;
        while (switch_core_media_bug_read(bug, &frame, SWITCH_TRUE) == SWITCH_STATUS_SUCCESS) {
          if (frame.datalen) {
            spx_uint32_t out_len = available >> 1;  // space for samples which are 2 bytes
            spx_uint32_t in_len = frame.samples;

            speex_resampler_process_interleaved_int(tech_pvt->resampler, 
              (const spx_int16_t *) frame.data, 
              (spx_uint32_t *) &in_len, 
              (spx_int16_t *) ((char *) pAudioPipe->binaryWritePtr()),
              &out_len);

            if (out_len > 0) {
              // bytes written = num samples * 2 * num channels
              size_t bytes_written = out_len << tech_pvt->channels;
              pAudioPipe->binaryWritePtrAdd(bytes_written);
              available = pAudioPipe->binarySpaceAvailable();
            }
            if (available < pAudioPipe->binaryMinSpace()) {
              if (!tech_pvt->buffer_overrun_notified) {
                tech_pvt->buffer_overrun_notified = 1;
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "(%u) dropping packets!\n", 
                  tech_pvt->id);
                tech_pvt->responseHandler(session, EVENT_BUFFER_OVERRUN, NULL);
              }
              break;
            }
          }
        }
      }

      pAudioPipe->unlockAudioBuffer();
      switch_mutex_unlock(tech_pvt->mutex);
    }
    return SWITCH_TRUE;
  }

switch_bool_t dub_speech_frame(switch_media_bug_t *bug, private_t* tech_pvt) {
    if (!tech_pvt) return SWITCH_TRUE;

    static uint32_t call_count = 0;
    static uint32_t underrun_count = 0;
    call_count++;

    // Usar lock BLOQUEANTE - crítico para sincronização
    switch_mutex_lock(tech_pvt->mutex);

    CircularBuffer_t *cBuffer = (CircularBuffer_t *) tech_pvt->streamingPlayoutBuffer;

    if (call_count % 50 == 0) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                         "(%u) SISTEMA ATIVO #%u: Buffer=%zu samples, Underruns=%u\n",
                         tech_pvt->id, call_count, cBuffer->size(), underrun_count);
    }

    // Lógica de clear buffer
    // Obter frame do FreeSWITCH
    switch_frame_t* rframe = switch_core_media_bug_get_write_replace_frame(bug);

    if (rframe && rframe->datalen > 0) {
        int16_t *fp = reinterpret_cast<int16_t*>(rframe->data);
        int samples_needed = rframe->samples;

        // Verificar dados disponíveis
        int samplesToCopy = std::min(static_cast<int>(cBuffer->size()), samples_needed);

        if (samplesToCopy > 0) {
            // Preparar dados com silêncio
            std::vector<int16_t> data(samples_needed, 0);
            std::copy_n(cBuffer->begin(), samplesToCopy, data.begin());
            cBuffer->erase(cBuffer->begin(), cBuffer->begin() + samplesToCopy);

            // SUBSTITUIR completamente o frame original
            memcpy(fp, data.data(), samples_needed * sizeof(int16_t));
            rframe->channels = 1;
            rframe->datalen = samples_needed * sizeof(int16_t);

            // Aplicar a substituição
            switch_core_media_bug_set_write_replace_frame(bug, rframe);

        } else {
            underrun_count++;
            // Buffer vazio: enviar silêncio para evitar picotes
            memset(fp, 0, samples_needed * sizeof(int16_t));
            rframe->channels = 1;
            rframe->datalen = samples_needed * sizeof(int16_t);
            switch_core_media_bug_set_write_replace_frame(bug, rframe);
        }
    }

    switch_mutex_unlock(tech_pvt->mutex);
    return SWITCH_TRUE;
}

  switch_status_t fork_session_stop_play(switch_core_session_t *session, char *bugname) {
    switch_channel_t *channel = switch_core_session_get_channel(session);
    switch_media_bug_t *bug = (switch_media_bug_t*) switch_channel_get_private(channel, bugname);
    if (!bug) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "fork_session_stop_play failed because no bug\n");
      return SWITCH_STATUS_FALSE;
    }
    private_t* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);

    if (!tech_pvt) return SWITCH_STATUS_FALSE;

    CircularBuffer_t *cBuffer = (CircularBuffer_t *) tech_pvt->streamingPlayoutBuffer;

    if (switch_mutex_trylock(tech_pvt->mutex) == SWITCH_STATUS_SUCCESS) {
      if (cBuffer != nullptr) {
        cBuffer->clear();
      }
      switch_mutex_unlock(tech_pvt->mutex);
    }
    return SWITCH_STATUS_SUCCESS;
  }

}

